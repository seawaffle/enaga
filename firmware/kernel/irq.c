#include "kernel.h"

/* C674x interrupt controller (SPRUFE8B chapter 5; C6748 data sheet SPRS590 table 6-13 for event numbers).
 * INTMUX1-3 at INTC + 0x104..0x10C pick the event for CPU interrupts 4-15, one byte each. Two are used: CPU
 * interrupt 4 <- event 4 (T64P0_TINT12, the watchdog tick; the original firmware's clock tick is the same event)
 * and CPU interrupt 8 <- event 8 (EDMA3_0_CC0_INT1, the DSP's shadow region 1, the audio block). Both are also
 * the reset mapping, written explicitly so the kernel does not depend on what the bootloader left. CPU
 * interrupt 6 <- event 46 (UART1_INT, the panel MCU's bytes) and CPU interrupt 7 <- event 38 (UART0_INT, MIDI DIN)
 * are not.
 *
 * Handlers run with interrupts off, except the app's audio callback, during which the watchdog, UART1 and UART0
 * may nest (audio.c), so irq_active counts the handlers running. */
#define INTC     0x01800000u
#define INTMUX1  (INTC + 0x104)
#define INTMUX2  (INTC + 0x108)

volatile int irq_active;
volatile u32 irq_count[16];
static u32 enabled;

void irq_init(void)
{
    REG32(INTMUX1) = (REG32(INTMUX1) & ~0x7F7F007Fu) | 4u | 46u << 16 | 38u << 24;   /* INTSEL4 = 4, 6 = 46, 7 = 38 */
    REG32(INTMUX2) = (REG32(INTMUX2) & ~0x7Fu) | 8u;   /* INTSEL8 = event 8 */
    irq_set_istp((u32)vectors);
}

void irq_enable(u32 mask)
{
    enabled |= mask;
    irq_enable_mask(mask);
}

void enaga_irq(unsigned number)
{
    irq_active++;
    irq_count[number & 15]++;
    if (number == IRQ_WATCHDOG) watchdog_isr();
    else if (number == IRQ_UART1) uart1_isr();
    else if (number == IRQ_UART0) uart0_isr();
    else if (number == IRQ_AUDIO) audio_isr();
    irq_active--;
}

void irq_unwind(void)
{
    irq_global_disable();
    irq_active = 0;
    irq_enable_mask(enabled);                 /* a nested handler masked the others */
    audio_irq_rearm();
}
