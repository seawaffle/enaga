#include "kernel.h"

/* C674x interrupt controller (SPRUFE8B chapter 5; C6748 data sheet SPRS590 table 6-13 for event numbers).
 * INTMUX1-3 at INTC + 0x104..0x10C pick the event for CPU interrupts 4-15, one byte each. Only the audio block
 * interrupt is used so far: CPU interrupt 8 <- event 8 (EDMA3_0_CC0_INT1, the DSP's shadow region 1). That is
 * also the reset mapping, written explicitly so the kernel does not depend on what the bootloader left. */
#define INTC     0x01800000u
#define INTMUX2  (INTC + 0x108)

volatile int irq_active;

void irq_init(void)
{
    u32 mux = REG32(INTMUX2);
    mux = (mux & ~0x7Fu) | 8u;                /* INTSEL8 = event 8 */
    REG32(INTMUX2) = mux;
    irq_set_istp((u32)vectors);
}

void enaga_irq(unsigned number)
{
    irq_active = 1;
    if (number == IRQ_AUDIO) audio_isr();
    irq_active = 0;
}
