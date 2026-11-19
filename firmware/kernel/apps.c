#include "kernel.h"
#include "ff.h"
#include "loader.h"

/* App hosting: mount the card, find the apps under /ENAGA/APPS, load one, give it the host table, route events
 * and the audio callback to it, and recover when it fails. The boot policy (last app, the only app, else the
 * Enaga menu) and switching apps are below.
 *
 * Memory (docs/design.md "Memory"): Enaga below ENAGA_APP_BASE; the app's image from there, and its heap
 * from the end of the image (rounded up to 4 KiB) to ENAGA_APP_LIMIT, the end of DDR. */
#define APPS_DIR  "/ENAGA/APPS"

const enaga_app_t *app_current;
volatile int app_busy;
char app_status[32];
char app_name[ENAGA_NAME_BYTES];
char crash_reason[48];

static void copy_text(char *dst, unsigned size, const char *src)
{
    unsigned i;
    for (i = 0; i + 1 < size && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
static void set_status(const char *s) { copy_text(app_status, sizeof app_status, s); }

static void host_log(const char *text) { log_line(text); }
static uint32_t host_ms(void)
{
    u32 gie = irq_global_disable(), ms = timer_ms();   /* timer_ms keeps state: one context at a time */
    if (gie) irq_global_enable();
    return ms;
}
static void host_fatal(const char *reason) { apps_fatal(reason); }
static void host_text(u8 *target, int x, int y, const char *text, u8 grey)
{
    if (target == canvas) gfx_text(x, y, text, grey);
}
static uint32_t host_audio_load(void) { return audio_cycles_last / (AUDIO_BUDGET_CYCLES / 1000u); }
static uint32_t host_audio_load_peak(void) { return audio_cycles_peak / (AUDIO_BUDGET_CYCLES / 1000u); }
static void host_led(uint32_t led, uint32_t red, uint32_t green, uint32_t blue) { leds_set(led, red, green, blue); }
volatile int app_menu_request;
int app_keep_on;
static void host_open_menu(void) { app_menu_request = 1; }          /* main.c opens it after the callback */
static void host_set_busy(uint32_t busy) { app_keep_on = busy != 0; }

enaga_host_t app_host = {                     /* heap filled in per app by load_path */
    ENAGA_ABI_VERSION, sizeof(enaga_host_t), 0, 0,
    host_log, host_ms, host_fatal, ENAGA_SAMPLE_RATE, AUDIO_FRAMES, host_text, host_audio_load, host_audio_load_peak,
    host_led,
    files_open, files_close, files_size, files_seek, files_info, files_remove, files_make_folder,
    files_read_all, files_save, files_read, files_write, files_load, files_list, files_data_folder,
    files_stream_open, files_stream_create, files_stream_read, files_stream_write, files_stream_status,
    files_stream_close,
    host_open_menu, host_set_busy,
};

static void cache_sync(uint32_t address, uint32_t bytes)   /* the loader calls this just before the app's entry */
{
    cache_wbinv((void *)address, bytes);
    watchdog_feed();                          /* from here a hang is the app's (constructors, init) */
    app_busy = 1;
}

static int mount_result;

void apps_card_status(void)                   /* the app's own status stays while it runs */
{
    if (app_current) return;
    if (files_mounted()) set_status("CARD OK");
    else if (!sd_present()) set_status("NO CARD");
    else if (mount_result == FR_NO_FILESYSTEM) set_status("CARD NOT FAT");
    else set_status("CARD ERROR");
}

void apps_mount(void)
{
    mount_result = files_mount();
    if (mount_result == FR_NOT_READY && !sd_present()) set_status("NO CARD");
    else apps_card_status();
}

static const char *load_error(int status)
{
    switch (status) {
    case ENAGA_LOAD_TRUNCATED: return "APP TRUNCATED";
    case ENAGA_LOAD_BAD_MAGIC: return "NOT AN APP";
    case ENAGA_LOAD_BAD_HEADER: return "BAD APP HEADER";
    case ENAGA_LOAD_ABI: return "APP ABI MISMATCH";
    case ENAGA_LOAD_WINDOW: return "APP TOO LARGE";
    case ENAGA_LOAD_CRC: return "APP CRC ERROR";
    case ENAGA_LOAD_ENTRY_FAILED: return "APP ENTRY FAILED";
    default: return "APP LOAD ERROR";
    }
}

static int load_path(const char *path)
{
    FIL file;
    UINT got;
    enaga_package_header_t header;
    const enaga_app_t *loaded = 0;
    int status;
    u32 done;
    if (f_open(&file, path, FA_READ) != FR_OK) { set_status("APP OPEN FAILED"); return -1; }
    if (f_read(&file, &header, sizeof header, &got) != FR_OK || got != sizeof header) {
        f_close(&file); set_status(load_error(ENAGA_LOAD_TRUNCATED)); return -1;
    }
    status = enaga_package_check(&header, (u32)f_size(&file));
    if (status != ENAGA_LOAD_OK) { f_close(&file); set_status(load_error(status)); return -1; }
    for (done = 0; done < header.load_size; done += got) {   /* straight into the app window */
        u32 chunk = header.load_size - done > 0x8000u ? 0x8000u : header.load_size - done;
        if (f_read(&file, (u8 *)header.load_address + done, chunk, &got) != FR_OK || got != chunk) {
            f_close(&file); set_status(load_error(ENAGA_LOAD_TRUNCATED)); return -1;
        }
        watchdog_feed();
    }
    f_close(&file);
    copy_text(app_name, sizeof app_name, header.name);
    crash_reason[0] = 0;
    {
        u32 heap = (header.load_address + header.load_size + header.bss_size + 4095u) & ~4095u;
        app_host.heap = (void *)heap;
        app_host.heap_size = heap < ENAGA_APP_LIMIT ? ENAGA_APP_LIMIT - heap : 0;
    }
    status = enaga_package_start(&header, &app_host, cache_sync, &loaded);   /* runs the app's constructors */
    if (status != ENAGA_LOAD_OK) { app_busy = 0; set_status(load_error(status)); return -1; }
    if (loaded->init && loaded->init(loaded->context) != 0) { app_busy = 0; set_status("APP INIT FAILED"); return -1; }
    app_busy = 0;
    app_menu_request = 0;
    app_keep_on = 0;
    app_current = loaded;
    audio_set_callback(loaded->audio, loaded->context);
    set_status("APP RUNNING");
    log_line(app_name);
    return 0;
}

/* The installed apps: folders under /ENAGA/APPS (each should hold APP.ENA), in directory order. */
int apps_scan(char (*names)[APPS_FOLDER], int max)
{
    DIR dir;
    FILINFO info;
    int count = 0;
    unsigned i;
    if (!files_mounted() || f_opendir(&dir, APPS_DIR) != FR_OK) return -1;
    while (count < max && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        if (!(info.fattrib & AM_DIR) || info.fname[0] == '.') continue;
        for (i = 0; info.fname[i] && i < APPS_FOLDER - 1; i++) names[count][i] = info.fname[i];
        names[count][i] = 0;
        if (!info.fname[i]) count++;          /* a name too long to keep is left out */
    }
    f_closedir(&dir);
    return count;
}

/* The boot record, /ENAGA/LAST.TXT: the last app's folder, then STARTING until it has run APPS_SETTLED_MS, then
 * RUNNING. An app that never got to RUNNING failed at start-up and is not loaded at the next boot (design.md
 * "Boot and app switching"). */
#define RECORD "/ENAGA/LAST.TXT"
#define APPS_SETTLED_MS 2000u
char app_folder[APPS_FOLDER];
static int settled;
static u32 started_ms;

static void record(const char *folder, const char *state)
{
    static FIL f;
    UINT put;
    char text[APPS_FOLDER + 16];
    unsigned n = 0, i;
    for (i = 0; folder[i]; i++) text[n++] = folder[i];
    text[n++] = '\n';
    for (i = 0; state[i]; i++) text[n++] = state[i];
    text[n++] = '\n';
    if (f_open(&f, RECORD, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    f_write(&f, text, n, &put);
    f_close(&f);
}

static int read_record(char folder[APPS_FOLDER], int *starting)   /* 0 = a record was read */
{
    static FIL f;
    char text[APPS_FOLDER + 16];
    UINT got;
    unsigned i, n;
    if (f_open(&f, RECORD, FA_READ) != FR_OK) return -1;
    if (f_read(&f, text, sizeof text - 1, &got) != FR_OK) got = 0;
    f_close(&f);
    text[got] = 0;
    for (n = 0; n < got && text[n] != '\n' && n < APPS_FOLDER - 1; n++) folder[n] = text[n];
    folder[n] = 0;
    if (!n || n == got || text[n] != '\n') return -1;
    for (i = 0; "STARTING"[i] && text[n + 1 + i] == "STARTING"[i]; i++) {}
    *starting = "STARTING"[i] == 0;
    return 0;
}

static void package_path(char *path, const char *folder)     /* /ENAGA/APPS/<folder>/APP.ENA */
{
    unsigned n = 0, i;
    for (i = 0; APPS_DIR[i]; i++) path[n++] = APPS_DIR[i];
    path[n++] = '/';
    for (i = 0; folder[i]; i++) path[n++] = folder[i];
    for (i = 0; "/APP.ENA"[i]; i++) path[n++] = "/APP.ENA"[i];
    path[n] = 0;
}

int apps_title(const char *folder, char title[ENAGA_NAME_BYTES])   /* the package's display name; 0 = read */
{
    static FIL f;
    char path[APPS_FOLDER + 24];
    enaga_package_header_t header;
    UINT got;
    unsigned i;
    package_path(path, folder);
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    if (f_read(&f, &header, sizeof header, &got) != FR_OK) got = 0;
    f_close(&f);
    if (got != sizeof header || header.magic != ENAGA_PACKAGE_MAGIC) return -1;
    for (i = 0; i + 1 < ENAGA_NAME_BYTES && header.name[i]; i++) title[i] = header.name[i];
    title[i] = 0;
    return 0;
}

static int installed(const char *folder)
{
    char path[APPS_FOLDER + 24];
    package_path(path, folder);
    return f_stat(path, 0) == FR_OK;
}

/* Stops the app running (if any) and starts /ENAGA/APPS/<folder>/APP.ENA; 0 = running. If the old app fails in
 * stop() (or overruns APPS_STOP_MS) it is dropped, which leaves for the kernel loop: apps_service then finishes
 * the switch from `apps_next`. */
static char apps_next[APPS_FOLDER];

int apps_start(const char *folder)
{
    char path[APPS_FOLDER + 24];
    if (!files_mounted()) return -1;
    package_path(path, folder);
    if (f_stat(path, 0) != FR_OK) { set_status("APP OPEN FAILED"); return -1; }
    copy_text(apps_next, sizeof apps_next, folder);
    if (app_current) apps_unload();
    apps_next[0] = 0;
    copy_text(app_folder, sizeof app_folder, folder);
    settled = 0;
    record(folder, "STARTING");
    if (load_path(path) != 0) { app_folder[0] = 0; return -1; }
    started_ms = timer_ms();
    return 0;
}

void apps_service(u32 ms)                     /* each pass: a switch to finish; the boot record once settled */
{
    if (apps_next[0] && !app_current && !files_reset_pending) {
        char folder[APPS_FOLDER];
        copy_text(folder, sizeof folder, apps_next);
        apps_next[0] = 0;
        if (apps_start(folder) != 0) menu_open(app_status);
    }
    if (app_current && !settled && ms - started_ms >= APPS_SETTLED_MS) {
        settled = 1;
        record(app_folder, "RUNNING");
    }
}

/* Power-on (decision 2): the last app, unless it failed at start-up; else the only one installed; else the menu.
 * Returns 1 when the menu should open, with a message for it in `message` (or empty). */
int apps_boot(int menu_requested, char *message, unsigned size)
{
    static char names[2][APPS_FOLDER];
    char last[APPS_FOLDER];
    int starting = 0, count;
    message[0] = 0;
    if (!files_mounted()) return 0;           /* the status page says why */
    count = apps_scan(names, 2);
    if (count < 0) { set_status("NO /ENAGA/APPS"); return 0; }
    if (count == 0) { set_status("NO APPS"); return 0; }
    if (menu_requested) { copy_text(message, size, "F1 HELD: MENU"); return 1; }
    if (read_record(last, &starting) == 0 && installed(last)) {   /* a record of an app since removed: ignored */
        if (starting) {
            unsigned n = 0, i;
            for (i = 0; last[i] && n + 1 < size; i++) message[n++] = last[i];
            for (i = 0; " DID NOT START"[i] && n + 1 < size; i++) message[n++] = " DID NOT START"[i];
            message[n] = 0;
            return 1;                          /* until an app chosen from the menu runs APPS_SETTLED_MS */
        }
        if (apps_start(last) == 0) return 0;
        copy_text(message, size, app_status);  /* why it did not load (gone, damaged): the menu */
        return 1;
    }
    if (count == 1) return apps_start(names[0]) == 0 ? 0 : 1;
    return 1;
}

/* Stopping an app: its sound fades out, then stop() has APPS_STOP_MS (watchdog.c checks it; a stop that saves to
 * the card keeps feeding the watchdog, so the main loop's limit alone would not end it). */
volatile int app_stopping;
volatile u32 app_stop_start;

void apps_unload(void)
{
    const enaga_app_t *a = app_current;
    if (a) audio_fade_out();
    audio_set_callback(0, 0);
    app_current = 0;
    if (a && a->stop) {
        app_current = a;                      /* stop() may still use the file service */
        app_busy = 1;
        app_stop_start = tsc_read();
        app_stopping = 1;
        a->stop(a->context);
        app_stopping = 0;
        app_busy = 0;
        app_current = 0;
    }
    app_keep_on = 0;
    files_app_stopped();
    leds_off();
}

void apps_event(const enaga_event_t *event)
{
    if (app_current && app_current->event) app_current->event(app_current->context, event);
}

/* An app called fatal() (or failed in a way the runtime caught, or the watchdog found it hung): drop it and
 * continue in the kernel loop on a fresh stack. From interrupt context (the audio callback, the watchdog) this
 * leaves through IRP. Whatever the main context
 * was doing is abandoned (a FatFs operation in progress is not completed). */
void apps_drop(const char *reason)           /* the app failed: forget it without calling it again */
{
    u32 gie = irq_global_disable();
    audio_silence_now();                      /* also clears the callback */
    app_current = 0;
    app_busy = 0;
    app_stopping = 0;
    app_keep_on = 0;
    files_reset_pending = 1;                  /* the main loop drops its files and remounts (files.c) */
    if (gie) irq_global_enable();
    copy_text(crash_reason, sizeof crash_reason, reason);
    set_status("APP STOPPED");
    log_line("FATAL");
    log_line(reason);
}

void apps_fatal(const char *reason)
{
    apps_drop(reason);
    if (irq_active) {
        irq_unwind();
        irq_resume_at(kernel_loop, KERNEL_SP);
    }
    restart_at(kernel_loop, KERNEL_SP);
}
