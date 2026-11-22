// Enaga app runtime: linked into every app. Entry point, static constructors, heap, and the C++ ABI
// hooks a freestanding -fno-exceptions -fno-rtti program needs. No libc and no libsupc++ are linked.
//
// Order on entry: keep the host table, set up the heap, run static constructors (they may log or allocate),
// then ask the app for its callback table. This GCC emits .ctors; .init_array is honoured too in case a
// later toolchain build switches. Destructors are never run: Enaga discards an unloaded app's memory.
#include <enaga/app.hpp>

#include <stddef.h>
#include <stdint.h>

namespace {
const enaga_host_t *g_host;

// First-fit free list over the host's heap region, 8-byte granules, coalescing on free. Single context:
// allocation is main-context only (the audio callback must not allocate).
struct Block {
    size_t size;        // bytes including this header, multiple of 8; low bit set while allocated
    Block *next;        // free list, address order (free blocks only)
};
constexpr size_t kHeader = 8;
static_assert(sizeof(Block) == kHeader);
Block *g_free;

void heap_init(void *base, size_t size) {
    uintptr_t start = (reinterpret_cast<uintptr_t>(base) + 7) & ~uintptr_t(7);
    size_t usable = (size - (start - reinterpret_cast<uintptr_t>(base))) & ~size_t(7);
    g_free = nullptr;
    if (!base || usable < 2 * kHeader) return;
    g_free = reinterpret_cast<Block *>(start);
    g_free->size = usable;
    g_free->next = nullptr;
}

void *heap_alloc(size_t bytes) {
    if (bytes > (size_t(1) << 30)) return nullptr;
    size_t need = (bytes + kHeader + 7) & ~size_t(7);
    if (need < 2 * kHeader) need = 2 * kHeader;
    for (Block **link = &g_free; *link; link = &(*link)->next) {
        Block *block = *link;
        if (block->size < need) continue;
        if (block->size - need >= 2 * kHeader) {
            Block *rest = reinterpret_cast<Block *>(reinterpret_cast<char *>(block) + need);
            rest->size = block->size - need;
            rest->next = block->next;
            *link = rest;
            block->size = need;
        } else {
            *link = block->next;
        }
        block->size |= 1;
        return reinterpret_cast<char *>(block) + kHeader;
    }
    return nullptr;
}

void heap_free(void *pointer) {
    if (!pointer) return;
    Block *block = reinterpret_cast<Block *>(static_cast<char *>(pointer) - kHeader);
    if (!(block->size & 1)) enaga::fatal("heap: free of a block that is not allocated");
    block->size &= ~size_t(1);
    Block *previous = nullptr, *next = g_free;
    while (next && next < block) { previous = next; next = next->next; }
    block->next = next;
    if (previous) previous->next = block; else g_free = block;
    if (next && reinterpret_cast<char *>(block) + block->size == reinterpret_cast<char *>(next)) {
        block->size += next->size;
        block->next = next->next;
    }
    if (previous && reinterpret_cast<char *>(previous) + previous->size == reinterpret_cast<char *>(block)) {
        previous->size += block->size;
        previous->next = block->next;
    }
}

void *checked_alloc(size_t bytes) {
    void *pointer = heap_alloc(bytes);
    if (!pointer) enaga::fatal("out of memory");
    return pointer;
}
}  // namespace

extern "C" {
typedef void (*init_function)(void);
extern init_function __init_array_start[], __init_array_end[];   // app.ld
extern init_function __ctors_start[], __ctors_end[];
const enaga_app_t *enaga_app_main(void);                        // ENAGA_APP

const enaga_app_t *enaga_entry(const enaga_host_t *host) {
    if (!host || host->abi_version != ENAGA_ABI_VERSION || host->size < sizeof(enaga_host_t)) return nullptr;
    g_host = host;
    heap_init(host->heap, host->heap_size);
    for (init_function *function = __init_array_start; function != __init_array_end; ++function) (*function)();
    for (init_function *function = __ctors_end; function != __ctors_start;) (*--function)();
    return enaga_app_main();
}

void *malloc(size_t bytes) { return heap_alloc(bytes); }
void free(void *pointer) { heap_free(pointer); }

// GCC emits calls to these for struct copies and initialisers even when freestanding.
void *memcpy(void *destination, const void *source, size_t bytes) {
    auto *d = static_cast<unsigned char *>(destination);
    auto *s = static_cast<const unsigned char *>(source);
    while (bytes--) *d++ = *s++;
    return destination;
}
void *memmove(void *destination, const void *source, size_t bytes) {
    auto *d = static_cast<unsigned char *>(destination);
    auto *s = static_cast<const unsigned char *>(source);
    if (d < s) while (bytes--) *d++ = *s++;
    else { d += bytes; s += bytes; while (bytes--) *--d = *--s; }
    return destination;
}
void *memset(void *destination, int value, size_t bytes) {
    auto *d = static_cast<unsigned char *>(destination);
    while (bytes--) *d++ = static_cast<unsigned char>(value);
    return destination;
}
int memcmp(const void *a, const void *b, size_t bytes) {
    auto *x = static_cast<const unsigned char *>(a);
    auto *y = static_cast<const unsigned char *>(b);
    for (; bytes; --bytes, ++x, ++y) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
size_t strlen(const char *text) { size_t n = 0; while (text[n]) ++n; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
int strncmp(const char *a, const char *b, size_t bytes) {
    for (; bytes; --bytes, ++a, ++b) {
        if (*a != *b) return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
        if (!*a) return 0;
    }
    return 0;
}
char *strchr(const char *text, int c) {
    for (;; ++text) {
        if (*text == static_cast<char>(c)) return const_cast<char *>(text);
        if (!*text) return nullptr;
    }
}
char *strrchr(const char *text, int c) {
    const char *found = nullptr;
    for (;; ++text) {
        if (*text == static_cast<char>(c)) found = text;
        if (!*text) return const_cast<char *>(found);
    }
}

// 64-bit division, which the C674x does in software. libgcc has these, but its objects carry unwind tables that
// pull in the C++ unwinder, which an app cannot link (__exidx_start undefined); these come first, so libgcc's are
// never linked. Shift and subtract: up to 64 steps, about as fast as libgcc's for the dividends apps use.
static uint64_t divide(uint64_t n, uint64_t d, uint64_t *remainder) {
    if (d == 0) enaga::fatal("division by zero");
    uint64_t q = 0, r = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if (!r && !(n >> bit)) continue;          // leading zeros of the dividend
        r = r << 1 | (n >> bit & 1);
        if (r >= d) { r -= d; q |= uint64_t(1) << bit; }
    }
    if (remainder) *remainder = r;
    return q;
}
static uint64_t magnitude(int64_t v) { return v < 0 ? 0 - uint64_t(v) : uint64_t(v); }
uint64_t __c6xabi_divull(uint64_t n, uint64_t d) { return divide(n, d, nullptr); }
uint64_t __c6xabi_remull(uint64_t n, uint64_t d) { uint64_t r; divide(n, d, &r); return r; }
int64_t __c6xabi_divlli(int64_t n, int64_t d) {
    uint64_t q = divide(magnitude(n), magnitude(d), nullptr);
    return (n < 0) != (d < 0) ? int64_t(0 - q) : int64_t(q);
}
int64_t __c6xabi_remlli(int64_t n, int64_t d) {   // the sign of the dividend, as C's %
    uint64_t r;
    divide(magnitude(n), magnitude(d), &r);
    return n < 0 ? int64_t(0 - r) : int64_t(r);
}

void abort(void) { enaga::fatal("abort"); }

// C++ ABI. Guards assume one context (statics are not initialised from the audio callback).
void __cxa_pure_virtual(void) { enaga::fatal("pure virtual call"); }
void __cxa_deleted_virtual(void) { enaga::fatal("deleted virtual call"); }
int __cxa_guard_acquire(uint64_t *guard) { return *reinterpret_cast<unsigned char *>(guard) == 0; }
void __cxa_guard_release(uint64_t *guard) { *reinterpret_cast<unsigned char *>(guard) = 1; }
void __cxa_guard_abort(uint64_t *) {}
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }   // destructors never run
void *__dso_handle = &__dso_handle;
}

namespace enaga {
const enaga_host_t &host() { return *g_host; }
[[noreturn]] void fatal(const char *reason) {
    g_host->fatal(reason);
    for (;;) {}
}
}  // namespace enaga

void *operator new(size_t bytes) { return checked_alloc(bytes); }
void *operator new[](size_t bytes) { return checked_alloc(bytes); }
void operator delete(void *pointer) noexcept { heap_free(pointer); }
void operator delete[](void *pointer) noexcept { heap_free(pointer); }
void operator delete(void *pointer, size_t) noexcept { heap_free(pointer); }
void operator delete[](void *pointer, size_t) noexcept { heap_free(pointer); }
