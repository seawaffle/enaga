/* Enaga: the binary interface between the firmware and an app (docs/design.md).
 *
 * PROVISIONAL. ENAGA_ABI_VERSION 0 carries no compatibility promise; the table is frozen and made
 * append-only at 1.0. The boundary is plain C on purpose: apps are usually C++, but C++ ABIs are not
 * stable across compiler versions, so nothing C++ crosses it. Apps use <enaga/app.hpp> on top of this.
 *
 * An app is a flat image linked at ENAGA_APP_BASE, stored on the card as an enaga_package_header followed
 * by the payload. Enaga copies the payload to load_address, checks its CRC-32, zeroes bss_size bytes after
 * it and calls the entry with the host table. The app's runtime (runtime/runtime.cpp) sets up its heap, runs
 * its static constructors and returns the app's callback table.
 */
#ifndef ENAGA_ABI_H
#define ENAGA_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENAGA_ABI_VERSION     0u
/* Memory (provisional): Enaga keeps the bottom 4 MiB of DDR. The app's image starts at ENAGA_APP_BASE and
 * its heap starts directly after the image (rounded up to 4 KiB), running to the end of DDR. */
#define ENAGA_APP_BASE        0xC0400000u
#define ENAGA_APP_LIMIT       0xC8000000u   /* end of the 128 MiB DDR: image and heap end at or below this */
#define ENAGA_PACKAGE_MAGIC   0x41474E45u   /* "ENGA" little-endian */
#define ENAGA_NAME_BYTES      24u

#define ENAGA_SCREEN_W        128
#define ENAGA_SCREEN_H        128           /* canvas: row-major, one byte per pixel, grey 0 (black) to 15 */
#define ENAGA_SAMPLE_RATE     48000u
#define ENAGA_BLOCK_FRAMES    64u           /* audio callback block: 1.33 ms */

/* Keys by bit number in the panel's 48-bit switch word (docs/hardware.md "Keys"). Bits 5-7 and 10-15 have
 * no switch. POWER is a separate input and never reaches an app. */
enum enaga_key {
    ENAGA_KEY_PUSH_B = 0, ENAGA_KEY_PUSH_A = 1, ENAGA_KEY_PUSH_C = 2, ENAGA_KEY_PUSH_D = 3,   /* knob pushes */
    ENAGA_KEY_PUSH_VALUE = 4,
    ENAGA_KEY_SELECT_LEFT = 8, ENAGA_KEY_SELECT_RIGHT = 9,      /* the selection buttons */
    ENAGA_KEY_OCTAVE_LEFT = 16, ENAGA_KEY_OCTAVE_RIGHT = 31,
    ENAGA_KEY_UP = 17, ENAGA_KEY_RIGHT = 18, ENAGA_KEY_LEFT = 24, ENAGA_KEY_DOWN = 25,
    ENAGA_KEY_OK = 23, ENAGA_KEY_CLR = 30,
    /* The 2 x 4 function block, numbered in reading order (F1-F4 upper row). Its printed labels differ between
     * SmplTrek, Lofi-12 XT and CyDrums, so Enaga numbers it (docs/hardware.md "Keys"). */
    ENAGA_KEY_F1 = 19, ENAGA_KEY_F2 = 26, ENAGA_KEY_F3 = 20, ENAGA_KEY_F4 = 27,
    ENAGA_KEY_F5 = 22, ENAGA_KEY_F6 = 29, ENAGA_KEY_F7 = 21, ENAGA_KEY_F8 = 28,
    ENAGA_KEY_STEP1 = 32,                                       /* STEP n = ENAGA_KEY_STEP1 + n - 1, n = 1..16 */
    ENAGA_KEY_COUNT = 48
};

/* Panel LEDs by logical number (docs/hardware.md "Panel MCU", which marks what is confirmed on a unit). Levels run
 * 0 (off) to ENAGA_LED_MAX; a colour an LED does not have is ignored. Every key with an LED is ENAGA_LED_KEY + its
 * enaga_key: STEP 1-16, F1-F8, CLR and OK (red and green). The
 * octave buttons have no LED. */
#define ENAGA_LED_MAX 10u
enum enaga_led {
    ENAGA_LED_PAD = 0,                      /* + pad 0-14, the id of ENAGA_EVENT_PAD: red, green, blue */
    ENAGA_LED_VOL_L = 16, ENAGA_LED_VOL_R = 17, ENAGA_LED_MIC = 18,   /* one colour: the brightest of the three */
    ENAGA_LED_VALUE = 19,                   /* the VALUE encoder: red, green, blue */
    ENAGA_LED_KEY = 64,                     /* + enaga_key */
    ENAGA_LED_COUNT = ENAGA_LED_KEY + ENAGA_KEY_COUNT
};

enum enaga_event_type {
    ENAGA_EVENT_KEY = 1,                    /* id: key bit; value: 1 down, 0 up */
    ENAGA_EVENT_ENCODER = 2,                /* id: 0-3 = A-D, 4 = VALUE; value: signed detents */
    ENAGA_EVENT_PAD = 3                     /* id: pad 0-14; value: 14-bit pressure reading (raw, E1) */
};

typedef struct enaga_event {
    uint16_t type;
    uint16_t id;
    int32_t value;
    uint32_t ms;                            /* when Enaga read it */
} enaga_event_t;

/* On the card: this header, then load_size payload bytes. 64 bytes, little-endian. */
typedef struct enaga_package_header {
    uint32_t magic;                         /* ENAGA_PACKAGE_MAGIC */
    uint32_t header_size;                   /* sizeof(enaga_package_header_t); the payload starts here */
    uint32_t abi_version;                   /* ENAGA_ABI_VERSION the app was built against */
    uint32_t load_address;                  /* ENAGA_APP_BASE */
    uint32_t load_size;                     /* payload bytes */
    uint32_t bss_size;                      /* bytes zeroed directly after the payload */
    uint32_t entry_offset;                  /* enaga_entry_t, from load_address */
    uint32_t crc32;                         /* IEEE CRC-32 of the payload */
    uint32_t app_version;                   /* the app's own version, shown by Enaga */
    uint32_t flags;                         /* 0 */
    char name[ENAGA_NAME_BYTES];            /* NUL-padded display name */
} enaga_package_header_t;

/* Services Enaga gives the app. Pointers stay valid until the app is unloaded. */
typedef struct enaga_host {
    uint32_t abi_version;                   /* ENAGA_ABI_VERSION of the running firmware */
    uint32_t size;                          /* sizeof(enaga_host_t) of the running firmware */
    void *heap;                             /* from the end of the app image to ENAGA_APP_LIMIT, 4 KiB aligned */
    uint32_t heap_size;
    void (*log)(const char *text);          /* one line on Enaga's log */
    uint32_t (*ms)(void);                   /* milliseconds since boot */
    void (*fatal)(const char *reason);      /* does not return: Enaga shows the reason and unloads the app */
    uint32_t sample_rate;                   /* ENAGA_SAMPLE_RATE */
    uint32_t block_frames;                  /* frames per audio callback */
    void (*text)(uint8_t *canvas, int x, int y, const char *text, uint8_t grey);   /* 5 x 7 font, 6 px advance */
    uint32_t (*audio_load)(void);           /* last audio block's cost, per mille of its real-time budget */
    uint32_t (*audio_load_peak)(void);      /* highest since the app started */
    void (*led)(uint32_t led, uint32_t red, uint32_t green, uint32_t blue);   /* enaga_led; main context only */
} enaga_host_t;

/* What the app gives Enaga. Every callback receives `context`; any callback may be 0. */
typedef struct enaga_app {
    uint32_t abi_version;                   /* ENAGA_ABI_VERSION the app was built against */
    uint32_t size;                          /* sizeof(enaga_app_t) when the app was built */
    const char *name;
    void *context;
    int (*init)(void *context);             /* 0 on success */
    void (*update)(void *context, uint32_t ms);
    void (*draw)(void *context, uint8_t *canvas);   /* about 20 times a second; ENAGA_SCREEN_W x _H bytes */
    void (*stop)(void *context);            /* before unloading; save state here */
    void (*event)(void *context, const enaga_event_t *event);   /* main context */
    /* Audio context (EDMA interrupt): `frames` interleaved stereo frames, L R L R, full scale +-1.0. `in` is
     * the codec input, `out` is written completely. No file access, drawing, allocation or blocking. */
    void (*audio)(void *context, const float *in, float *out, uint32_t frames);
} enaga_app_t;

typedef const enaga_app_t *(*enaga_entry_t)(const enaga_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
