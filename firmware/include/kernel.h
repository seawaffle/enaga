/* Enaga kernel interfaces. The HAL (hal.h) is polled; the kernel adds interrupts, the audio engine,
 * the app host and the main loop. */
#ifndef KERNEL_H
#define KERNEL_H
#include "hal.h"
#include <enaga/abi.h>

#define ENAGA_VERSION "0.1.0"

/* start.s */
extern char vectors[];
void irq_set_istp(u32 table);
void irq_enable_mask(u32 mask);
void irq_global_enable(void);
u32  irq_global_disable(void);                 /* returns the previous GIE bit */
void irq_resume_at(void (*fn)(void), u32 sp);  /* from interrupt context, continue at fn (never returns) */
void restart_at(void (*fn)(void), u32 sp);     /* main context: continue at fn on a fresh stack */
void irq_nest_begin(u32 saved[3], u32 mask);   /* in a handler: let the interrupts in mask nest (keeps IRP, ITSR, IER) */
void irq_nest_end(const u32 saved[3]);         /* GIE off and IRP, ITSR, IER as they were */
extern char _stack_top[];
#define KERNEL_SP (((u32)_stack_top - 8) & ~7u)   /* a fresh kernel stack: *SP must lie inside it */

/* irq.c */
#define IRQ_WATCHDOG 4                        /* CPU interrupt 4 <- event 4, T64P0_TINT12 (the highest maskable priority) */
#define IRQ_AUDIO 8                           /* CPU interrupt 8 <- event 8, EDMA3_0_CC0_INT1 (shadow region 1) */
void irq_init(void);
void irq_enable(u32 mask);                    /* IER |= mask, remembered for irq_unwind */
extern volatile int irq_active;               /* handlers running: 2 while the watchdog has nested into audio */
void enaga_irq(unsigned number);
void irq_unwind(void);                        /* before leaving interrupt context abnormally: GIE off, no handler
                                                 active, every enabled interrupt unmasked, audio rearmed */

/* audio.c: McASP0 + EDMA ring, one interrupt per block, the app's callback in interrupt context */
#define AUDIO_FRAMES ENAGA_BLOCK_FRAMES
#define AUDIO_BUDGET_CYCLES (AUDIO_FRAMES * (372000000u / ENAGA_SAMPLE_RATE))   /* 496,000 at 64 frames */
typedef void (*audio_callback_t)(void *context, const float *in, float *out, uint32_t frames);   /* ABI type */
int  audio_init(void);                        /* 0 = running; else -stage: -2 EDMA PSC, -3 McASP PSC, -4 codec, -5 McASP start */
void audio_isr(void);
void audio_set_callback(audio_callback_t callback, void *context);   /* 0 = silence; safe from main context */
void audio_mute(void);                        /* power-off order: silence, DAC mute, output mute */
void audio_silence_now(void);                 /* zero the transmit ring at once (crash paths) */
void audio_irq_rearm(void);                   /* re-evaluate the EDMA interrupt (after leaving the ISR abnormally) */
extern volatile u32 audio_blocks, audio_overruns, audio_cycles_last, audio_cycles_peak;
extern volatile int audio_in_callback;        /* the app's callback is running, since audio_callback_start (TSC) */
extern volatile u32 audio_callback_start;
extern int audio_status, audio_codec_errors;

/* apps.c: find, load, host and unload apps */
extern const enaga_app_t *app_current;
extern volatile int app_busy;                 /* Enaga is running the app's start-up (constructors, init) or stop */
extern enaga_host_t app_host;                 /* the table every app receives */
extern char app_status[32];
extern char app_name[ENAGA_NAME_BYTES];
void apps_mount(void);                        /* mounts the card; app_status says what happened */
int  apps_load_first(void);                   /* the first /ENAGA/APPS/<dir>/APP.ENA; 0 = running */
void apps_unload(void);
void apps_event(const enaga_event_t *event);
void apps_drop(const char *reason);           /* forget a failed app; returns */
void apps_fatal(const char *reason);          /* never returns */
extern char crash_reason[48];

/* leds.c: panel LEDs by enaga_led, cached per address so only changes are sent */
void leds_set(unsigned led, unsigned red, unsigned green, unsigned blue);   /* main context; ignored in interrupts */
void leds_forget(void);                       /* the panel's state is unknown: the next write of each LED is sent */
void leds_off(void);

/* log.c: Enaga's log, also read by the emulator tests */
#define LOG_LINES 16
#define LOG_WIDTH 40
extern char log_lines[LOG_LINES][LOG_WIDTH];
extern u32 log_count;                         /* lines ever logged; the newest is log_lines[(log_count-1) % LOG_LINES] */
void log_line(const char *text);

/* exception.c: CPU exceptions (start.s nmi_entry). In app code (the app window) the app is dropped as with
 * fatal(); anywhere else Enaga shows the kernel crash screen and waits for the power key. */
extern u32 exception_record[6];               /* NRP, EFR, IERR, NTSR, SP, B3 at the exception */
extern u32 crash_pc, crash_efr, crash_ierr;   /* the last exception that dropped an app (0 if none) */
extern int kernel_crashed;
void exceptions_enable(void);
void exception_resume(void (*fn)(void), u32 sp);
void enaga_exception(void);
void kernel_crash(const char *reason);         /* never returns */

/* watchdog.c: the 10 ms tick checks the audio callback and the main loop are still progressing */
extern volatile u32 watchdog_ticks;
void watchdog_start(void);
void watchdog_feed(void);                     /* the main loop is progressing */
void watchdog_isr(void);

/* main.c */
void kernel_loop(void);                       /* never returns */

#endif
