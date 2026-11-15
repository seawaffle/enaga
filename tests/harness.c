/* E0 test harness: a guest program standing in for Enaga's loader host. tests/test_e0.py loads this ELF on
 * the emulator core, writes a package into harness_package, runs harness_main and reads harness_result and
 * harness_log back. The package goes through the real loader (loader/loader.c) and the app's real runtime.
 * Card reading and the firmware's event loop are not part of E0's harness. */
#include <enaga/abi.h>
#include "../loader/loader.h"

#define LOG_BYTES 8192u

struct harness_result {
    int32_t load_status;
    int32_t init_result;
    uint32_t updates;
    uint32_t stopped;
    uint32_t fatal;
    uint32_t done;
    uint32_t log_length;
    uint32_t app_name;             /* guest address of the table's name */
};

struct harness_result harness_result;
char harness_log[LOG_BYTES];
uint32_t harness_package_size;
uint8_t harness_package[256 * 1024] __attribute__((aligned(8)));
static uint8_t harness_heap[512 * 1024] __attribute__((aligned(8)));
static uint32_t now_ms;

static void host_log(const char *text) {
    while (*text && harness_result.log_length < LOG_BYTES - 1) harness_log[harness_result.log_length++] = *text++;
    if (harness_result.log_length < LOG_BYTES - 1) harness_log[harness_result.log_length++] = '\n';
}
static uint32_t host_ms(void) { return now_ms; }
static void host_fatal(const char *reason) {
    harness_result.fatal = 1;
    host_log("FATAL");
    host_log(reason);
    for (;;) {}
}

static void host_text(uint8_t *canvas, int x, int y, const char *text, uint8_t grey)
{
    (void)canvas; (void)x; (void)y; (void)text; (void)grey;
}
static uint32_t host_zero(void) { return 0; }

static const enaga_host_t host = {ENAGA_ABI_VERSION, sizeof(enaga_host_t), harness_heap, sizeof(harness_heap),
                                  host_log, host_ms, host_fatal, ENAGA_SAMPLE_RATE, ENAGA_BLOCK_FRAMES, host_text,
                                  host_zero, host_zero};

void harness_main(void) {
    const enaga_app_t *app = 0;
    harness_result.load_status = enaga_load_image(harness_package, harness_package_size, &host, 0, &app);
    if (harness_result.load_status == ENAGA_LOAD_OK) {
        harness_result.app_name = (uint32_t)(uintptr_t)app->name;
        harness_result.init_result = app->init ? app->init(app->context) : 0;
        for (uint32_t i = 1; i <= 3 && app->update; i++) {
            now_ms = i * 10;
            app->update(app->context, now_ms);
            harness_result.updates++;
        }
        if (app->stop) { app->stop(app->context); harness_result.stopped = 1; }
    }
    harness_result.done = 1;
}
