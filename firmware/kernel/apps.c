#include "kernel.h"
#include "ff.h"
#include "loader.h"

/* App hosting: mount the card, load the first package found under /ENAGA/APPS, give it the host table,
 * route events and the audio callback to it, and recover when it fails. The boot policy (last app, the only
 * app, else the Enaga menu) and switching apps arrive with the menu in E3.
 *
 * Memory (docs/design.md "Memory"): Enaga below ENAGA_APP_BASE; the app's image from there, and its heap
 * from the end of the image (rounded up to 4 KiB) to ENAGA_APP_LIMIT, the end of DDR. */
#define APPS_DIR  "/ENAGA/APPS"

const enaga_app_t *app_current;
volatile int app_busy;
char app_status[32];
char app_name[ENAGA_NAME_BYTES];
char crash_reason[48];
static FATFS fs;
static int mounted;

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

enaga_host_t app_host = {                     /* heap filled in per app by load_path */
    ENAGA_ABI_VERSION, sizeof(enaga_host_t), 0, 0,
    host_log, host_ms, host_fatal, ENAGA_SAMPLE_RATE, AUDIO_FRAMES, host_text, host_audio_load, host_audio_load_peak,
};

static void cache_sync(uint32_t address, uint32_t bytes)   /* the loader calls this just before the app's entry */
{
    cache_wbinv((void *)address, bytes);
    watchdog_feed();                          /* from here a hang is the app's (constructors, init) */
    app_busy = 1;
}

void apps_mount(void)
{
    FRESULT r = f_mount(&fs, "", 1);
    mounted = r == FR_OK;
    if (mounted) set_status("CARD OK");
    else if (r == FR_NOT_READY && !sd_present()) set_status("NO CARD");
    else if (r == FR_NO_FILESYSTEM) set_status("CARD NOT FAT");
    else set_status("CARD ERROR");
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
    app_current = loaded;
    audio_set_callback(loaded->audio, loaded->context);
    set_status("APP RUNNING");
    log_line(app_name);
    return 0;
}

int apps_load_first(void)
{
    DIR dir;
    FILINFO info;
    char path[48];
    if (!mounted) return -1;
    if (f_opendir(&dir, APPS_DIR) != FR_OK) { set_status("NO /ENAGA/APPS"); return -1; }
    for (;;) {
        unsigned n = 0, i;
        if (f_readdir(&dir, &info) != FR_OK || !info.fname[0]) { f_closedir(&dir); set_status("NO APPS"); return -1; }
        if (!(info.fattrib & AM_DIR) || info.fname[0] == '.') continue;
        for (i = 0; APPS_DIR[i]; i++) path[n++] = APPS_DIR[i];
        path[n++] = '/';
        for (i = 0; info.fname[i] && n < sizeof path - 10; i++) path[n++] = info.fname[i];
        for (i = 0; "/APP.ENA"[i]; i++) path[n++] = "/APP.ENA"[i];
        path[n] = 0;
        f_closedir(&dir);
        return load_path(path);
    }
}

void apps_unload(void)
{
    const enaga_app_t *a = app_current;
    audio_set_callback(0, 0);
    app_current = 0;
    if (a && a->stop) {
        app_busy = 1;
        a->stop(a->context);
        app_busy = 0;
    }
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
