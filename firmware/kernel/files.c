#include "kernel.h"
#include "ff.h"

/* The file service for apps (docs/design.md "Files"; the app's view is in include/enaga/abi.h).
 *
 * Immediate calls run FatFs to completion, feeding the watchdog between 8 KiB pieces. Asynchronous ones go into
 * a queue that files_service works on in the main loop, at most FILES_SLICE_MS a pass and 8 KiB a FatFs call;
 * only the request at the head is ever in progress, so a close or a card removal between slices is safe. The
 * completion callbacks run from files_service, in the main context.
 *
 * Writes go only to the app's folder (/ENAGA/DATA/<package name>/), /SAMPLES/ and /RECORD/. The card-detect pin
 * is watched every pass: a change that lasts FILES_DETECT_MS unmounts or mounts the card and tells the app.
 *
 * When the app stops, its handles are closed and its requests dropped without callbacks. When it is dropped
 * (a crash, fatal(), the watchdog), possibly from an interrupt and possibly in the middle of a FatFs call in the
 * main context, apps_drop only sets files_reset_pending: the next pass discards everything and remounts, so no
 * half-finished FatFs state is reused (data not yet written by then is lost). */
#define FILES_QUEUE      16
#define FILES_CHUNK      8192u
#define FILES_SLICE_MS   2u
#define FILES_DETECT_MS  200u

enum { REQ_READ = 1, REQ_WRITE, REQ_LOAD, REQ_LIST };

typedef struct {
    FIL fil;
    u8 used, dead, writing;
    u8 pending;                              /* requests queued on it, not cancelled */
} handle_t;

typedef struct {
    u8 op, started, cancelled;
    signed char file;
    u8 *buffer;
    u32 bytes, done;
    enaga_file_done_t done_fn;
    enaga_file_entry_t entry_fn;
    void *user;
    char path[ENAGA_FILE_PATH_MAX + 1];
} request_t;

static FATFS fs;
static int mounted;
static handle_t handles[ENAGA_FILES_OPEN];
static request_t queue[FILES_QUEUE];
static unsigned queue_head, queue_count;
static FIL load_fil;                          /* the head request's own file (loads) or folder (lists) */
static DIR list_dir;
static int card_seen, card_changing;
static u32 card_change_ms;
volatile int files_reset_pending;
u32 files_requests_done;

/* ---- helpers ---------------------------------------------------------------------------------------------- */

/* Card failures in the log (the status page shows it): "FS ERR 2" (FatFs's FRESULT for a failure reported as
 * CARD ERROR), "SD ERR -6 0 00000E00" (the driver's code and MMCST0 bits, or for -6 the card status), "MOUNT 3 -2
 * 00000000" (a failed mount: FRESULT, sd_init's code and MMCST0 bits) and "SD SLOW 120 MS" for each new slowest
 * one-sector write over 50 ms. */
static u32 slow_logged;

static int put_dec(char *line, int n, int value)
{
    char digits[12];
    int k = 0;
    u32 v = value < 0 ? (u32)-value : (u32)value;
    if (value < 0) line[n++] = '-';
    do { digits[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) line[n++] = digits[--k];
    return n;
}

/* "<prefix><a>", then " <b>" if count >= 2, then " <word in hex>" if count >= 3. */
static void log_codes(const char *prefix, int count, int a, int b, u32 word)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[40];
    int n = 0, i;
    while (*prefix) line[n++] = *prefix++;
    n = put_dec(line, n, a);
    if (count >= 2) { line[n++] = ' '; n = put_dec(line, n, b); }
    if (count >= 3) {
        line[n++] = ' ';
        for (i = 28; i >= 0; i -= 4) line[n++] = hex[word >> i & 15];
    }
    line[n] = 0;
    log_line(line);
}

static void log_slow_writes(void)
{
    u32 ms = sd_write_slowest / TICKS_PER_MS;
    char line[24] = "SD SLOW ";
    int n = 8, k = 0;
    char digits[10];
    if (ms < 50 || ms <= slow_logged) return;
    slow_logged = ms;
    do { digits[k++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    while (k) line[n++] = digits[--k];
    line[n++] = ' '; line[n++] = 'M'; line[n++] = 'S'; line[n] = 0;
    log_line(line);
}

static int32_t map(FRESULT r)
{
    if (r != FR_OK && r != FR_NO_FILE && r != FR_NO_PATH && r != FR_EXIST && r != FR_DENIED && r != FR_LOCKED &&
        r != FR_INVALID_NAME && r != FR_TOO_MANY_OPEN_FILES && r != FR_INVALID_OBJECT && sd_present()) {
        log_codes("FS ERR ", 1, (int)r, 0, 0);
        if (r == FR_DISK_ERR) log_codes("SD ERR ", 3, sd_io_error, 0, sd_io_status);
    }
    switch (r) {
    case FR_OK: return ENAGA_FILE_OK;
    case FR_NO_FILE: case FR_NO_PATH: return ENAGA_FILE_NOT_FOUND;
    case FR_INVALID_NAME: return ENAGA_FILE_BAD_PATH;
    case FR_DENIED: case FR_WRITE_PROTECTED: return ENAGA_FILE_DENIED;
    case FR_EXIST: return ENAGA_FILE_EXISTS;
    case FR_INVALID_OBJECT: return ENAGA_FILE_BAD_HANDLE;
    case FR_LOCKED: return ENAGA_FILE_BUSY;
    case FR_TOO_MANY_OPEN_FILES: return ENAGA_FILE_TOO_MANY;
    case FR_NOT_READY: return sd_present() ? ENAGA_FILE_CARD_ERROR : ENAGA_FILE_NO_CARD;
    default: return sd_present() ? ENAGA_FILE_CARD_ERROR : ENAGA_FILE_NO_CARD;
    }
}

static unsigned length(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* `path` starts with `prefix` (ASCII letters in any case) followed by '/' and at least one more byte. */
static int inside(const char *path, const char *prefix)
{
    unsigned i;
    for (i = 0; prefix[i]; i++)
        if (upper((u8)path[i]) != upper((u8)prefix[i])) return 0;
    return path[i] == '/' && path[i + 1];
}

/* Absolute, at most ENAGA_FILE_PATH_MAX bytes, no empty part, no "." or "..", no backslash or drive colon.
 * "/" alone is the root (readable). */
static int32_t check_path(const char *path)
{
    unsigned i, part = 1;
    if (!path || path[0] != '/') return ENAGA_FILE_BAD_PATH;
    if (path[1] == 0) return ENAGA_FILE_OK;
    for (i = 1; path[i]; i++) {
        if (i > ENAGA_FILE_PATH_MAX) return ENAGA_FILE_BAD_PATH;
        if (path[i] == '\\' || path[i] == ':') return ENAGA_FILE_BAD_PATH;
        if (path[i] == '/') {
            if (i == part) return ENAGA_FILE_BAD_PATH;                              /* empty part */
            if (path[part] == '.' && (i == part + 1 || (path[part + 1] == '.' && i == part + 2)))
                return ENAGA_FILE_BAD_PATH;
            part = i + 1;
        }
    }
    if (i == part) return ENAGA_FILE_BAD_PATH;                                      /* ends with '/' */
    if (path[part] == '.' && (i == part + 1 || (path[part + 1] == '.' && i == part + 2))) return ENAGA_FILE_BAD_PATH;
    return ENAGA_FILE_OK;
}

/* The app's own folder: /ENAGA/DATA/ and the package name, with characters FAT refuses replaced by '_'. */
static void data_folder(char out[48])
{
    static const char base[] = "/ENAGA/DATA/";
    unsigned n = 0, i;
    for (i = 0; base[i]; i++) out[n++] = base[i];
    for (i = 0; app_name[i] && n < 47; i++) {
        u8 c = (u8)app_name[i];
        out[n++] = (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                    c == '>' || c == '|') ? '_' : (char)c;
    }
    while (n > sizeof base - 1 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    if (n == sizeof base - 1) { out[n++] = 'A'; out[n++] = 'P'; out[n++] = 'P'; }
    out[n] = 0;
}

const char *files_data_folder(void)
{
    static char folder[48];
    data_folder(folder);
    return folder;
}

static int32_t check_write(const char *path)
{
    char own[48];
    int32_t r = check_path(path);
    if (r) return r;
    data_folder(own);
    return inside(path, own) || inside(path, "/SAMPLES") || inside(path, "/RECORD") ? ENAGA_FILE_OK : ENAGA_FILE_DENIED;
}

static int32_t usable(void)
{
    if (irq_active) return ENAGA_FILE_WRONG_CONTEXT;
    if (files_reset_pending) return ENAGA_FILE_CANCELLED;
    return mounted ? ENAGA_FILE_OK : ENAGA_FILE_NO_CARD;
}

static void make_parents(const char *path)    /* every folder above the last part */
{
    char part[ENAGA_FILE_PATH_MAX + 1];
    unsigned i;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' && i > 0) { part[i] = 0; f_mkdir(part); }
        part[i] = path[i];
    }
}

static int with_suffix(char *out, const char *path, const char *suffix)
{
    unsigned n = length(path), m = length(suffix), i;
    if (n + m > ENAGA_FILE_PATH_MAX) return -1;
    for (i = 0; i < n; i++) out[i] = path[i];
    for (i = 0; i <= m; i++) out[n + i] = suffix[i];
    return 0;
}

/* Open for reading; a missing file whose NAME.TMP exists (a save cut short after its new data was complete)
 * opens that instead. */
static FRESULT open_read(FIL *fil, const char *path)
{
    char tmp[ENAGA_FILE_PATH_MAX + 1];
    FRESULT r = f_open(fil, path, FA_READ);
    if (r == FR_NO_FILE && with_suffix(tmp, path, ".TMP") == 0) {
        FRESULT t = f_open(fil, tmp, FA_READ);
        if (t == FR_OK) return t;
    }
    return r;
}

static handle_t *handle(int32_t file)
{
    if (file < 0 || file >= (int32_t)ENAGA_FILES_OPEN || !handles[file].used) return 0;
    return &handles[file];
}

static u32 size32(FSIZE_t size) { return size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)size; }

/* ---- mounting and the card -------------------------------------------------------------------------------- */

int files_mount(void)                         /* FR_OK or the FatFs error */
{
    FRESULT r = f_mount(&fs, "", 1);
    if (r != FR_OK && sd_present()) {
        log_codes("MOUNT ", 3, (int)r, sd_error, sd_error_status);
        if (r == FR_DISK_ERR) log_codes("SD ERR ", 3, sd_io_error, 0, sd_io_status);
        if (r == FR_NOT_READY || r == FR_DISK_ERR) {   /* the card not answering: GP4[1] cycled, then once more */
            sd_power_cycle();
            r = f_mount(&fs, "", 1);
            log_codes(r == FR_OK ? "SD CYCLED OK " : "SD CYCLED MOUNT ", 1, (int)r, 0, 0);
        }
    }
    mounted = r == FR_OK;
    return r;
}

int files_mounted(void) { return mounted; }

static void drop_all(int32_t result, int callbacks)
{
    unsigned i;
    while (queue_count) {
        request_t r = queue[queue_head];
        queue_head = (queue_head + 1) % FILES_QUEUE;
        queue_count--;
        if (r.started && r.op == REQ_LOAD && mounted) f_close(&load_fil);
        if (r.started && r.op == REQ_LIST && mounted) f_closedir(&list_dir);
        if (callbacks && r.done_fn && app_current) r.done_fn(r.user, result);
    }
    for (i = 0; i < ENAGA_FILES_OPEN; i++) handles[i].pending = 0;
}

static void send_card_event(int32_t value)
{
    enaga_event_t e;
    e.type = ENAGA_EVENT_CARD; e.id = 0; e.value = value; e.ms = timer_ms();
    apps_event(&e);
}

static void card_removed(void)
{
    unsigned i;
    for (i = 0; i < ENAGA_FILES_OPEN; i++) if (handles[i].used) handles[i].dead = 1;
    drop_all(ENAGA_FILE_NO_CARD, 1);
    f_mount(0, "", 0);
    mounted = 0;
    apps_card_status();
    send_card_event(0);
}

static void card_inserted(void)
{
    FRESULT r = files_mount();
    apps_card_status();
    send_card_event(r == FR_OK ? 1 : r == FR_NO_FILESYSTEM ? ENAGA_FILE_CARD_ERROR : map(r));
}

static void card_watch(u32 ms)
{
    int present = sd_present();
    if (present == card_seen) { card_changing = 0; return; }
    if (!card_changing) { card_changing = 1; card_change_ms = ms; return; }
    if (ms - card_change_ms < FILES_DETECT_MS) return;
    card_changing = 0;
    card_seen = present;
    if (present) card_inserted(); else card_removed();
}

/* The write probe (unit session 2): how a real card takes Enaga's writes, without risking the file system. When
 * /ENAGA/SDTEST.BIN (at least 2 KiB, made on a computer) is on the card, each boot writes a pattern into its first
 * three sectors by write sequences 1, 2 and 3 (mmcsd.c sd_write), with filler for a controller still wanting data
 * (sd_write_topup), reads each back, and stops after a write that fails (on a unit that leaves the card silent until
 * it is taken out). One line each, at most 21 characters (the status page's width):
 *   "W1 OK T0 3MS"     sequence 1: the sector reads back as written, no filler, in 3 ms
 *   "W2 BAD SH-16 T1"  written, but the pattern is moved 16 words earlier (the first 16 lost), after one filler
 *   "W2 BAD 5 T1"      written, wrong from word 5, not a simple shift
 *   "W3 ERR-4 S26 T8"  sd_write's code, MMCST1 at the failure, fillers given
 * Only the file's own data is overwritten, never the FAT or a folder. */
static u32 probe_out[128], probe_back[128];

static int put_text(char *line, int n, const char *t) { while (*t) line[n++] = *t++; return n; }
static int put_hex(char *line, int n, u32 v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    while (digits--) line[n++] = hex[v >> (4 * digits) & 15];
    return n;
}

static int probe_step(int mode, u32 sector)       /* 0 if the card may now be silent */
{
    char line[40] = "W0 ";
    int i, bad = -1, shift = 0, w, n = 3, normal = sd_write_mode;
    u32 start;
    line[1] = (char)('0' + mode);
    for (i = 0; i < 128; i++) probe_out[i] = (u32)mode << 28 | (u32)i << 16 | (~(u32)i & 0xFFFF);
    for (i = 0; i < 128; i++) probe_back[i] = 0;
    sd_write_mode = mode;
    sd_write_topup = 1;
    sd_write_topups = 0;
    start = tsc_read();
    w = sd_write((const u8 *)probe_out, sector, 1);
    sd_write_mode = normal;
    sd_write_topup = 0;
    if (w) {
        n = put_text(line, n, "ERR");
        n = put_dec(line, n, w);
        n = put_text(line, n, " S");
        n = put_hex(line, n, sd_io_status1, 2);
    } else if (sd_read((u8 *)probe_back, sector, 1)) {
        n = put_text(line, n, "READ ERR");
        n = put_dec(line, n, sd_io_error);
        w = 1;
    } else {
        for (i = 0; i < 128 && bad < 0; i++) if (probe_back[i] != probe_out[i]) bad = i;
        if (bad < 0) {
            n = put_text(line, n, "OK");
        } else {
            for (w = 1; w < 64 && !shift; w++) {          /* the pattern moved later by w words? */
                for (i = w; i < 128 && probe_back[i] == probe_out[i - w]; i++) {}
                if (i == 128) shift = w;
            }
            for (w = 1; w < 64 && !shift; w++) {          /* ... or earlier (the first words lost)? */
                for (i = 0; i + w < 128 && probe_back[i] == probe_out[i + w]; i++) {}
                if (i + w == 128) shift = -w;
            }
            n = put_text(line, n, shift ? "BAD SH" : "BAD ");
            n = put_dec(line, n, shift ? shift : bad);
        }
        w = 0;
    }
    n = put_text(line, n, " T");
    n = put_dec(line, n, (int)sd_write_topups);
    if (!w && bad < 0) {
        line[n++] = ' ';
        n = put_dec(line, n, (int)((tsc_read() - start) / TICKS_PER_MS));
        n = put_text(line, n, "MS");
    }
    line[n] = 0;
    log_line(line);
    return !w;
}

void files_write_probe(void)
{
    static FIL f;
    u32 sector[3];
    UINT got;
    u8 byte;
    int k;
    if (!mounted || f_open(&f, "/ENAGA/SDTEST.BIN", FA_READ) != FR_OK) return;
    if (f_size(&f) < 2048) { f_close(&f); log_line("SDT FILE UNDER 2 KIB"); return; }
    for (k = 0; k < 3; k++) {                     /* the sectors holding bytes 0, 512 and 1024 */
        if (f_lseek(&f, 512u * (u32)k) != FR_OK || f_read(&f, &byte, 1, &got) != FR_OK || !got) { f_close(&f); return; }
        sector[k] = (u32)f.sect;
    }
    f_close(&f);
    for (k = 0; k < 3; k++)
        if (!probe_step(k + 1, sector[k])) break;
}

void files_init(void)
{
    card_seen = sd_present();
    /* The driver's waits are bounded (500 ms a step, 1 s for a sector to program), but a FatFs call can span many
     * of them on a slow card: the watchdog is fed while the driver waits, so a slow card makes a slow pass, and a
     * failing one an error, rather than the app being dropped as not responding. */
    sd_wait_hook = watchdog_feed;
}

/* ---- the app stopping ------------------------------------------------------------------------------------- */

void files_app_stopped(void)                  /* main context: the app stopped normally */
{
    unsigned i;
    drop_all(ENAGA_FILE_CANCELLED, 0);
    for (i = 0; i < ENAGA_FILES_OPEN; i++) {
        if (handles[i].used && !handles[i].dead && mounted) f_close(&handles[i].fil);
        handles[i].used = 0;
    }
}

static void reset_after_drop(void)
{
    unsigned i;
    files_reset_pending = 0;
    queue_head = queue_count = 0;
    for (i = 0; i < ENAGA_FILES_OPEN; i++) handles[i].used = handles[i].pending = 0;
    if (mounted) { f_mount(0, "", 0); files_mount(); }      /* app_status keeps the reason the app stopped */
}

/* ---- immediate calls -------------------------------------------------------------------------------------- */

int32_t files_open(const char *path, uint32_t mode)
{
    int32_t r = usable(), i;
    FRESULT f;
    if (r) return r;
    if (mode != ENAGA_FILE_READ && mode != ENAGA_FILE_WRITE) return ENAGA_FILE_BAD_HANDLE;
    r = mode == ENAGA_FILE_WRITE ? check_write(path) : check_path(path);
    if (r) return r;
    for (i = 0; i < (int32_t)ENAGA_FILES_OPEN && handles[i].used; i++)
        ;
    if (i == (int32_t)ENAGA_FILES_OPEN) return ENAGA_FILE_TOO_MANY;
    if (mode == ENAGA_FILE_WRITE) {
        make_parents(path);
        f = f_open(&handles[i].fil, path, FA_WRITE | FA_CREATE_ALWAYS);
    } else {
        f = f_open(&handles[i].fil, path, FA_READ);
    }
    if (f != FR_OK) return map(f);
    handles[i].used = 1;
    handles[i].dead = 0;
    handles[i].writing = mode == ENAGA_FILE_WRITE;
    handles[i].pending = 0;
    return i;
}

int32_t files_close(int32_t file)
{
    handle_t *h;
    unsigned i;
    FRESULT f = FR_OK;
    if (irq_active) return ENAGA_FILE_WRONG_CONTEXT;
    if (!(h = handle(file))) return ENAGA_FILE_BAD_HANDLE;
    for (i = 0; i < queue_count; i++) {
        request_t *r = &queue[(queue_head + i) % FILES_QUEUE];
        if ((r->op == REQ_READ || r->op == REQ_WRITE) && r->file == file) r->cancelled = 1;
    }
    h->pending = 0;
    if (!h->dead && mounted) f = f_close(&h->fil);
    h->used = 0;
    return h->dead ? ENAGA_FILE_NO_CARD : map(f);
}

int32_t files_size(int32_t file)
{
    handle_t *h;
    u32 size;
    if (irq_active) return ENAGA_FILE_WRONG_CONTEXT;
    if (!(h = handle(file))) return ENAGA_FILE_BAD_HANDLE;
    if (h->dead) return ENAGA_FILE_NO_CARD;
    size = size32(f_size(&h->fil));
    return size > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)size;
}

int32_t files_seek(int32_t file, uint32_t position)
{
    handle_t *h;
    if (irq_active) return ENAGA_FILE_WRONG_CONTEXT;
    if (!(h = handle(file))) return ENAGA_FILE_BAD_HANDLE;
    if (h->dead) return ENAGA_FILE_NO_CARD;
    if (h->pending) return ENAGA_FILE_BUSY;
    return map(f_lseek(&h->fil, position));
}

static void fill_info(enaga_file_info_t *info, const FILINFO *fi)
{
    unsigned i;
    for (i = 0; i < sizeof info->name - 1 && fi->fname[i]; i++) info->name[i] = fi->fname[i];
    info->name[i] = 0;
    info->size = size32(fi->fsize);
    info->flags = (fi->fattrib & AM_DIR) ? ENAGA_FILE_FOLDER : 0;
}

int32_t files_info(const char *path, enaga_file_info_t *info)
{
    static FILINFO fi;
    int32_t r = usable();
    FRESULT f;
    if (r || (r = check_path(path))) return r;
    if (path[1] == 0) {                       /* the root has no entry of its own */
        info->name[0] = 0; info->size = 0; info->flags = ENAGA_FILE_FOLDER;
        return ENAGA_FILE_OK;
    }
    if ((f = f_stat(path, &fi)) != FR_OK) return map(f);
    fill_info(info, &fi);
    return ENAGA_FILE_OK;
}

int32_t files_remove(const char *path)
{
    int32_t r = usable();
    if (r || (r = check_write(path))) return r;
    return map(f_unlink(path));
}

int32_t files_make_folder(const char *path)
{
    int32_t r = usable();
    FRESULT f;
    if (r || (r = check_write(path))) return r;
    make_parents(path);
    f = f_mkdir(path);
    return f == FR_EXIST ? ENAGA_FILE_OK : map(f);
}

int32_t files_read_all(const char *path, void *buffer, uint32_t max)
{
    static FIL fil;
    int32_t r = usable();
    u32 done = 0;
    FRESULT f;
    if (r || (r = check_path(path))) return r;
    if ((f = open_read(&fil, path)) != FR_OK) return map(f);
    if (f_size(&fil) > max) { f_close(&fil); return ENAGA_FILE_TOO_BIG; }
    while (done < f_size(&fil)) {
        UINT got;
        u32 want = (u32)f_size(&fil) - done;
        if (want > FILES_CHUNK) want = FILES_CHUNK;
        if ((f = f_read(&fil, (u8 *)buffer + done, want, &got)) != FR_OK) { f_close(&fil); return map(f); }
        done += got;
        watchdog_feed();
        if (got < want) break;
    }
    f_close(&fil);
    return (int32_t)done;
}

int32_t files_save(const char *path, const void *data, uint32_t bytes)
{
    static FIL fil;
    char tmp[ENAGA_FILE_PATH_MAX + 1];
    int32_t r = usable();
    u32 done = 0;
    FRESULT f;
    if (r || (r = check_write(path))) return r;
    if (with_suffix(tmp, path, ".TMP")) return ENAGA_FILE_BAD_PATH;
    make_parents(path);
    if ((f = f_open(&fil, tmp, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) return map(f);
    while (done < bytes) {
        UINT put;
        u32 want = bytes - done > FILES_CHUNK ? FILES_CHUNK : bytes - done;
        f = f_write(&fil, (const u8 *)data + done, want, &put);
        done += put;
        watchdog_feed();
        if (f != FR_OK || put < want) {
            f_close(&fil);
            f_unlink(tmp);
            return f != FR_OK ? map(f) : ENAGA_FILE_FULL;
        }
    }
    if ((f = f_close(&fil)) != FR_OK) { f_unlink(tmp); return map(f); }
    f = f_unlink(path);
    if (f != FR_OK && f != FR_NO_FILE) { f_unlink(tmp); return map(f); }
    return map(f_rename(tmp, path));
}

/* ---- asynchronous calls ----------------------------------------------------------------------------------- */

static request_t *push(void)
{
    request_t *r;
    if (queue_count == FILES_QUEUE) return 0;
    r = &queue[(queue_head + queue_count) % FILES_QUEUE];
    queue_count++;
    r->started = r->cancelled = 0;
    r->done = 0;
    r->file = -1;
    r->entry_fn = 0;
    return r;
}

static int32_t queue_io(int op, int32_t file, void *buffer, uint32_t bytes, enaga_file_done_t done, void *user)
{
    handle_t *h;
    request_t *r;
    int32_t u = usable();
    if (u == ENAGA_FILE_WRONG_CONTEXT || u == ENAGA_FILE_CANCELLED) return u;
    if (!(h = handle(file))) return ENAGA_FILE_BAD_HANDLE;
    if (h->dead) return ENAGA_FILE_NO_CARD;
    if (op == REQ_WRITE && !h->writing) return ENAGA_FILE_DENIED;
    if (!(r = push())) return ENAGA_FILE_TOO_MANY;
    r->op = (u8)op; r->file = (signed char)file; r->buffer = buffer; r->bytes = bytes; r->done_fn = done; r->user = user;
    h->pending++;
    return ENAGA_FILE_OK;
}

int32_t files_read(int32_t file, void *buffer, uint32_t bytes, enaga_file_done_t done, void *user)
{
    return queue_io(REQ_READ, file, buffer, bytes, done, user);
}

int32_t files_write(int32_t file, const void *data, uint32_t bytes, enaga_file_done_t done, void *user)
{
    return queue_io(REQ_WRITE, file, (void *)data, bytes, done, user);
}

static int32_t queue_path(int op, const char *path, void *buffer, uint32_t max, enaga_file_entry_t entry,
                          enaga_file_done_t done, void *user)
{
    request_t *r;
    unsigned i;
    int32_t u = usable();
    if (u || (u = check_path(path))) return u;
    if (!(r = push())) return ENAGA_FILE_TOO_MANY;
    r->op = (u8)op; r->buffer = buffer; r->bytes = max; r->entry_fn = entry; r->done_fn = done; r->user = user;
    for (i = 0; path[i]; i++) r->path[i] = path[i];
    r->path[i] = 0;
    return ENAGA_FILE_OK;
}

int32_t files_load(const char *path, void *buffer, uint32_t max, enaga_file_done_t done, void *user)
{
    return queue_path(REQ_LOAD, path, buffer, max, 0, done, user);
}

int32_t files_list(const char *path, enaga_file_entry_t entry, enaga_file_done_t done, void *user)
{
    return queue_path(REQ_LIST, path, 0, 0, entry, done, user);
}

/* ---- the service ------------------------------------------------------------------------------------------ */

static void finish(int32_t result)
{
    request_t r = queue[queue_head];          /* popped first: the callback may queue more */
    queue_head = (queue_head + 1) % FILES_QUEUE;
    queue_count--;
    if (r.file >= 0 && handles[r.file].pending && !r.cancelled) handles[r.file].pending--;
    files_requests_done++;
    if (r.done_fn && app_current) r.done_fn(r.user, result);
}

/* One step of the head request: one FatFs call (a chunk, a folder entry). */
static void step(request_t *r)
{
    UINT got;
    FRESULT f;
    u32 want = r->bytes - r->done > FILES_CHUNK ? FILES_CHUNK : r->bytes - r->done;
    switch (r->op) {
    case REQ_READ:
    case REQ_WRITE: {
        handle_t *h = &handles[(int)r->file];
        if (h->dead) { finish(ENAGA_FILE_NO_CARD); return; }
        if (want == 0) { finish((int32_t)r->done); return; }
        f = r->op == REQ_READ ? f_read(&h->fil, r->buffer + r->done, want, &got)
                              : f_write(&h->fil, r->buffer + r->done, want, &got);
        if (f != FR_OK) { finish(map(f)); return; }
        r->done += got;
        if (got < want) finish(r->op == REQ_READ ? (int32_t)r->done : ENAGA_FILE_FULL);   /* end of file; card full */
        else if (r->done == r->bytes) finish((int32_t)r->done);
        return;
    }
    case REQ_LOAD:
        if (!r->started) {
            if ((f = open_read(&load_fil, r->path)) != FR_OK) { finish(map(f)); return; }
            r->started = 1;
            if (f_size(&load_fil) > r->bytes) { f_close(&load_fil); finish(ENAGA_FILE_TOO_BIG); return; }
            r->bytes = (u32)f_size(&load_fil);
            return;
        }
        if (want && (f = f_read(&load_fil, r->buffer + r->done, want, &got)) != FR_OK) {
            f_close(&load_fil); finish(map(f)); return;
        }
        if (want) r->done += got;
        if (!want || got < want || r->done == r->bytes) { f_close(&load_fil); finish((int32_t)r->done); }
        return;
    case REQ_LIST: {
        static FILINFO fi;
        static enaga_file_info_t info;
        if (!r->started) {
            if ((f = f_opendir(&list_dir, r->path)) != FR_OK) { finish(map(f)); return; }
            r->started = 1;
            return;
        }
        if ((f = f_readdir(&list_dir, &fi)) != FR_OK) { f_closedir(&list_dir); finish(map(f)); return; }
        if (!fi.fname[0]) { f_closedir(&list_dir); finish((int32_t)r->done); return; }
        r->done++;
        fill_info(&info, &fi);
        if (r->entry_fn && app_current) r->entry_fn(r->user, &info);
        return;
    }
    default:
        finish(ENAGA_FILE_BAD_HANDLE);
    }
}

/* Each main-loop pass: the card-detect pin, then queued work for up to FILES_SLICE_MS. Returns non-zero while
 * work remains, so the loop does not sleep. */
int files_service(u32 ms)
{
    u32 start = tsc_read();
    if (files_reset_pending) reset_after_drop();
    card_watch(ms);
    while (queue_count && tsc_read() - start < FILES_SLICE_MS * TICKS_PER_MS) {
        request_t *r = &queue[queue_head];
        if (r->cancelled) finish(ENAGA_FILE_CANCELLED);
        else step(r);
    }
    log_slow_writes();
    return queue_count != 0;
}
