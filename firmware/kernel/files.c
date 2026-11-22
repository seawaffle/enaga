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

/* Streams (abi.h). The ring is single-producer, single-consumer: for playback the main loop puts bytes in
 * (`in`) and the app takes them out (`out`), for a recording the other way round. The counters run freely and
 * index the ring masked; each side writes only its own. */
typedef struct {
    FIL fil;
    u8 used, writing;
    volatile u8 primed, eof;                  /* playback: the ring has been full; the file is all in it */
    volatile int32_t error;                   /* a negative enaga_file_error once the stream has failed */
    u8 *ring;
    u32 mask;
    volatile u32 in, out, underruns;
} stream_t;
static stream_t streams[ENAGA_STREAMS];

/* ---- helpers ---------------------------------------------------------------------------------------------- */

/* Card failures in the log (the status page shows it): "FS ERR 2" (FatFs's FRESULT for a failure reported as
 * CARD ERROR), "SD ERR -6 0 00000E00" (the driver's code and MMCST0 bits, or for -6 the card status), "MOUNT 3 -2
 * 00000000" (a failed mount: FRESULT, sd_init's code and MMCST0 bits) and "SD SLOW 120 MS" for each new slowest
 * write command (up to 128 sectors) over 50 ms. */
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

void files_release(void)                      /* no app running, so nothing is open or queued */
{
    if (mounted) f_mount(0, "", 0);
    mounted = 0;
}

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
    for (i = 0; i < ENAGA_STREAMS; i++) if (streams[i].used && !streams[i].error) streams[i].error = ENAGA_FILE_NO_CARD;
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

/* The write probe (unit session 2): a check that writes work on this card, without risking the file system. When
 * /ENAGA/SDTEST.BIN (at least 4 KiB, made on a computer) is on the card, each boot writes a pattern into its first
 * sector (CMD24), then into the next seven in one command (CMD25), and reads each back (CMD17, CMD18). One line
 * each, at most 21 characters (the status page's width), "W" and the sector count first:
 *   "W1 OK 3MS"      the sector reads back as written, in 3 ms
 *   "W7 BAD SH-16"   written, but the pattern is moved 16 words earlier (the first 16 lost)
 *   "W7 BAD 5"       written, wrong from word 5, not a simple shift
 *   "W7 ERR-4 S26"   sd_write's code and MMCST1 at the failure (the card may now be silent until taken out)
 *   "W7 NOT IN A ROW" the file's sectors 1-7 are not consecutive on the card, so no multi-block write
 * Only the file's own data is overwritten, never the FAT or a folder. */
#define PROBE_WORDS (7 * 128)
static u32 probe_out[PROBE_WORDS], probe_back[PROBE_WORDS];

static int put_text(char *line, int n, const char *t) { while (*t) line[n++] = *t++; return n; }
static int put_hex(char *line, int n, u32 v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    while (digits--) line[n++] = hex[v >> (4 * digits) & 15];
    return n;
}

static int probe_write(u32 sector, u32 count)       /* 0 if the card may now be silent */
{
    char line[40] = "W0 ";
    int i, bad = -1, shift = 0, w, n = 3, words = (int)(128 * count);
    u32 start;
    line[1] = (char)('0' + count);
    for (i = 0; i < words; i++) probe_out[i] = (count << 28) | (u32)i << 16 | (~(u32)i & 0xFFFF);
    for (i = 0; i < words; i++) probe_back[i] = 0;
    start = tsc_read();
    w = sd_write((const u8 *)probe_out, sector, count);
    if (w) {
        n = put_text(line, n, "ERR");
        n = put_dec(line, n, w);
        n = put_text(line, n, " S");
        n = put_hex(line, n, sd_io_status1, 2);
    } else if (sd_read((u8 *)probe_back, sector, count)) {
        n = put_text(line, n, "READ ERR");
        n = put_dec(line, n, sd_io_error);
        w = 1;
    } else {
        for (i = 0; i < words && bad < 0; i++) if (probe_back[i] != probe_out[i]) bad = i;
        if (bad < 0) {
            n = put_text(line, n, "OK");
        } else {
            for (w = 1; w < 64 && !shift; w++) {          /* the pattern moved later by w words? */
                for (i = w; i < words && probe_back[i] == probe_out[i - w]; i++) {}
                if (i == words) shift = w;
            }
            for (w = 1; w < 64 && !shift; w++) {          /* ... or earlier (the first words lost)? */
                for (i = 0; i + w < words && probe_back[i] == probe_out[i + w]; i++) {}
                if (i + w == words) shift = -w;
            }
            n = put_text(line, n, shift ? "BAD SH" : "BAD ");
            n = put_dec(line, n, shift ? shift : bad);
        }
        w = 0;
    }
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
    UINT got;
    u8 byte;
    u32 sector[8], k;
    if (!mounted || f_open(&f, "/ENAGA/SDTEST.BIN", FA_READ) != FR_OK) return;
    if (f_size(&f) < 4096) { f_close(&f); log_line("SDT UNDER 4 KIB"); return; }
    for (k = 0; k < 8; k++) {                        /* the sector holding each 512 bytes */
        if (f_lseek(&f, 512 * k) != FR_OK || f_read(&f, &byte, 1, &got) != FR_OK || !got) { f_close(&f); return; }
        sector[k] = (u32)f.sect;
    }
    f_close(&f);
    if (!probe_write(sector[0], 1)) return;
    for (k = 2; k < 8 && sector[k] == sector[1] + k - 1; k++) {}
    if (k < 8) log_line("W7 NOT IN A ROW");
    else probe_write(sector[1], 7);
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
    for (i = 0; i < ENAGA_STREAMS; i++) if (streams[i].used) files_stream_close((int32_t)i, 0, 0);
}

static void reset_after_drop(void)
{
    unsigned i;
    files_reset_pending = 0;
    queue_head = queue_count = 0;
    for (i = 0; i < ENAGA_FILES_OPEN; i++) handles[i].used = handles[i].pending = 0;
    for (i = 0; i < ENAGA_STREAMS; i++) streams[i].used = 0;
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

/* ---- streams --------------------------------------------------------------------------------------------- */

static stream_t *stream(int32_t id)
{
    if (id < 0 || id >= (int32_t)ENAGA_STREAMS || !streams[id].used) return 0;
    return &streams[id];
}

static int32_t stream_new(const void *ring, uint32_t ring_bytes)   /* a free slot, or an error */
{
    int32_t i;
    if (!ring || ring_bytes < ENAGA_STREAM_RING_MIN || (ring_bytes & (ring_bytes - 1))) return ENAGA_FILE_BAD_HANDLE;
    for (i = 0; i < (int32_t)ENAGA_STREAMS && streams[i].used; i++)
        ;
    return i == (int32_t)ENAGA_STREAMS ? ENAGA_FILE_TOO_MANY : i;
}

static void stream_start(stream_t *s, void *ring, uint32_t ring_bytes, int writing)
{
    s->ring = ring;
    s->mask = ring_bytes - 1;
    s->in = s->out = s->underruns = 0;
    s->primed = writing;
    s->eof = 0;
    s->error = 0;
    s->writing = (u8)writing;
    s->used = 1;                              /* last: the audio side may look from here on */
}

int32_t files_stream_open(const char *path, uint32_t offset, void *ring, uint32_t ring_bytes)
{
    int32_t r = usable(), i;
    FRESULT f;
    if (r || (r = check_path(path))) return r;
    if ((i = stream_new(ring, ring_bytes)) < 0) return i;
    if ((f = f_open(&streams[i].fil, path, FA_READ)) != FR_OK) return map(f);
    if ((f = f_lseek(&streams[i].fil, offset)) != FR_OK) { f_close(&streams[i].fil); return map(f); }
    stream_start(&streams[i], ring, ring_bytes, 0);
    return i;
}

int32_t files_stream_create(const char *path, uint32_t reserve, void *ring, uint32_t ring_bytes)
{
    static const u8 zeros[64];
    int32_t r = usable(), i;
    FRESULT f = FR_OK;
    if (r || (r = check_write(path))) return r;
    if ((i = stream_new(ring, ring_bytes)) < 0) return i;
    make_parents(path);
    if ((f = f_open(&streams[i].fil, path, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) return map(f);
    while (reserve && f == FR_OK) {
        UINT put, n = reserve > sizeof zeros ? sizeof zeros : reserve;
        f = f_write(&streams[i].fil, zeros, n, &put);
        if (f == FR_OK && put < n) { f_close(&streams[i].fil); return ENAGA_FILE_FULL; }
        reserve -= n;
    }
    if (f != FR_OK) { f_close(&streams[i].fil); return map(f); }
    stream_start(&streams[i], ring, ring_bytes, 1);
    return i;
}

static void copy_bytes(u8 *dst, const u8 *src, u32 n)
{
    if (!(((u32)dst | (u32)src | n) & 3)) {
        u32 *d = (u32 *)dst;
        const u32 *s = (const u32 *)src;
        for (n >>= 2; n; n--) *d++ = *s++;
    } else {
        while (n--) *dst++ = *src++;
    }
}

uint32_t files_stream_read(int32_t id, void *out, uint32_t bytes)
{
    stream_t *s = stream(id);
    u32 n, at, first;
    if (!s || s->writing || s->error) return 0;
    n = s->in - s->out;
    if (n > bytes) n = bytes;
    at = s->out & s->mask;
    first = n < s->mask + 1 - at ? n : s->mask + 1 - at;
    copy_bytes(out, s->ring + at, first);
    copy_bytes((u8 *)out + first, s->ring, n - first);
    s->out += n;
    if (n < bytes && !s->eof) s->underruns++;
    return n;
}

uint32_t files_stream_write(int32_t id, const void *data, uint32_t bytes)
{
    stream_t *s = stream(id);
    u32 n, at, first;
    if (!s || !s->writing || s->error) return 0;
    n = s->mask + 1 - (s->in - s->out);
    if (n > bytes) n = bytes;
    at = s->in & s->mask;
    first = n < s->mask + 1 - at ? n : s->mask + 1 - at;
    copy_bytes(s->ring + at, data, first);
    copy_bytes(s->ring, (const u8 *)data + first, n - first);
    s->in += n;
    if (n < bytes) s->underruns++;
    return n;
}

int32_t files_stream_status(int32_t id, enaga_stream_status_t *status)
{
    stream_t *s = stream(id);
    u32 in, out;
    if (!s) return ENAGA_FILE_BAD_HANDLE;
    in = s->in;
    out = s->out;
    status->buffered = in - out;
    status->position = s->writing ? in : out;
    status->underruns = s->underruns;
    status->state = s->error ? s->error
                  : s->writing ? ENAGA_STREAM_RUNNING
                  : s->eof && in == out ? ENAGA_STREAM_ENDED
                  : s->primed ? ENAGA_STREAM_RUNNING : ENAGA_STREAM_FILLING;
    return ENAGA_FILE_OK;
}

/* The work a stream wants now: playback, room for a chunk (a quarter of the ring, at most FILES_CHUNK); a
 * recording, a chunk waiting (or, when closing, anything). As a share of the ring, so the neediest goes first. */
static u32 stream_chunk(const stream_t *s) { u32 q = (s->mask + 1) / 4; return q < FILES_CHUNK ? q : FILES_CHUNK; }

static u32 stream_need(const stream_t *s)    /* 0 = nothing to do; else higher is needier, 1 to 1025 */
{
    u32 size = s->mask + 1, used = s->in - s->out, shift = (u32)__builtin_ctz(size) - 10;   /* size >= 4096 */
    if (!s->used || s->error || !mounted) return 0;
    if (s->writing) return used >= stream_chunk(s) ? 1 + (used >> shift) : 0;
    if (s->eof || size - used < stream_chunk(s)) return 0;
    return 1 + ((size - used) >> shift);
}

/* One FatFs call for a stream: a chunk into a playback ring, or out of a recording one, up to the ring's end. */
static void stream_step(stream_t *s, int flush)
{
    u32 size = s->mask + 1, used = s->in - s->out, at, want;
    UINT got;
    FRESULT f;
    if (s->writing) {
        at = s->out & s->mask;
        want = used < size - at ? used : size - at;
        if (!flush && want > FILES_CHUNK) want = FILES_CHUNK;
        if (!want) return;
        f = f_write(&s->fil, s->ring + at, want, &got);
        s->out += got;
        if (f != FR_OK) s->error = map(f);
        else if (got < want) s->error = ENAGA_FILE_FULL;
    } else {
        at = s->in & s->mask;
        want = size - used < size - at ? size - used : size - at;
        if (want > FILES_CHUNK) want = FILES_CHUNK;
        if (!want) return;
        f = f_read(&s->fil, s->ring + at, want, &got);
        s->in += got;
        if (f != FR_OK) s->error = map(f);
        else if (got < want) s->eof = 1;
        if (s->eof || s->in - s->out == size) s->primed = 1;
    }
}

static int stream_service(void)               /* one step for the neediest stream; 0 if none wanted one */
{
    stream_t *best = 0;
    u32 i, need, most = 0;
    for (i = 0; i < ENAGA_STREAMS; i++)
        if ((need = stream_need(&streams[i])) > most) { most = need; best = &streams[i]; }
    if (best) stream_step(best, 0);
    return best != 0;
}

int32_t files_stream_close(int32_t id, const void *header, uint32_t header_bytes)
{
    stream_t *s;
    int32_t r;
    UINT put;
    FRESULT f;
    if (irq_active) return ENAGA_FILE_WRONG_CONTEXT;
    if (!(s = stream(id))) return ENAGA_FILE_BAD_HANDLE;
    s->used = 0;                              /* first: the audio side stops looking */
    if (!mounted || s->error == ENAGA_FILE_NO_CARD) return ENAGA_FILE_NO_CARD;
    if (s->writing) {
        while (!s->error && s->in != s->out) { stream_step(s, 1); watchdog_feed(); }
        if (!s->error && header && header_bytes) {
            if ((f = f_lseek(&s->fil, 0)) != FR_OK || (f = f_write(&s->fil, header, header_bytes, &put)) != FR_OK)
                s->error = map(f);
            else if (put < header_bytes)
                s->error = ENAGA_FILE_FULL;
        }
    }
    r = s->error;
    f = f_close(&s->fil);
    return r ? r : map(f);
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

/* Each main-loop pass: the card-detect pin, then streams and queued work for up to FILES_SLICE_MS, a step of the
 * neediest stream before each step of the queue's head. Returns non-zero while work remains, so the loop does
 * not sleep. */
int files_service(u32 ms)
{
    u32 start = tsc_read(), i;
    if (files_reset_pending) reset_after_drop();
    card_watch(ms);
    while (tsc_read() - start < FILES_SLICE_MS * TICKS_PER_MS) {   /* a stream step, then a request step */
        int worked = mounted && stream_service();
        if (queue_count) {
            request_t *r = &queue[queue_head];
            if (r->cancelled) finish(ENAGA_FILE_CANCELLED);
            else step(r);
            worked = 1;
        }
        if (!worked) break;
    }
    log_slow_writes();
    for (i = 0; i < ENAGA_STREAMS; i++) if (stream_need(&streams[i])) return 1;
    return queue_count != 0;
}
