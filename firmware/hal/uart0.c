#include "hal.h"

/* MIDI DIN on UART0, set up as the original firmware does (smpltrek-re HW-11): PSC0 module 9, PINMUX3 nibbles 4 and
 * 5 = UART0 RXD/TXD, MDR = 0 (16x), divisor 372 (186 MHz / 16 / 372 = 31,250 baud), LCR = 3 (8N1), FIFOs on with a
 * receive trigger of 1, then PWREMU_MGMT's two enables.
 *
 * One interrupt does both directions. Receive: every byte into rx_ring (uart0_read takes them in the main loop).
 * Transmit: one byte per THRE interrupt, so the hardware FIFO holds at most one byte waiting behind the one being
 * shifted out, and a real-time byte (clock, start, stop) queued in rt_ring goes ahead of the message queue with at
 * most one byte, 320 us, of delay. THRE is enabled only while something waits. Producers (the main loop and the
 * app's audio callback) queue with interrupts off; the handler is the only consumer. */
#define UART_RBR  0x00
#define UART_THR  0x00
#define UART_IER  0x04
#define UART_IIR  0x08
#define UART_FCR  0x08
#define UART_LCR  0x0C
#define UART_LSR  0x14
#define UART_DLL  0x20
#define UART_DLH  0x24
#define UART_PWREMU 0x30
#define UART_MDR  0x34
#define LSR_DR    0x01
#define LSR_OE    0x02
#define LSR_ERRORS 0x8C                       /* parity, framing, receiver FIFO error */
#define LSR_THRE  0x20
#define IER_RDA   0x01
#define IER_THRE  0x02
#define DIVISOR   372u

u32 uart0_rx_bytes, uart0_tx_bytes, uart0_overruns, uart0_ring_lost, uart0_line_errors;

#define RX_RING 2048u                         /* 650 ms of a saturated MIDI input */
#define TX_RING 2048u
#define RT_RING 16u
static u8 rx_ring[RX_RING], tx_ring[TX_RING], rt_ring[RT_RING];
static volatile u32 rx_head, rx_tail, tx_head, tx_tail, rt_head, rt_tail;

u32 irq_global_disable(void);
void irq_global_enable(void);

void uart0_init(void)
{
    psc_enable(PSC0, PSC0_UART0);
    pinmux_set(3, 4, 2);
    pinmux_set(3, 5, 2);
    REG32(UART0 + UART_MDR) = 0;
    REG32(UART0 + UART_DLL) = DIVISOR & 0xFF;
    REG32(UART0 + UART_DLH) = DIVISOR >> 8;
    REG32(UART0 + UART_LCR) = 3;
    REG32(UART0 + UART_FCR) = 7;
    REG32(UART0 + UART_FCR) = 1;
    REG32(UART0 + UART_IER) = 0;
    REG32(UART0 + UART_PWREMU) = 0x2001;
    REG32(UART0 + UART_PWREMU) = 0x6001;
    REG32(UART0 + UART_IER) = IER_RDA;        /* the CPU interrupt is enabled by the kernel */
}

static void transmit(void)                    /* in the handler, or with interrupts off */
{
    if (!(REG32(UART0 + UART_LSR) & LSR_THRE)) return;
    if (rt_tail != rt_head) REG32(UART0 + UART_THR) = rt_ring[rt_tail++ % RT_RING];
    else if (tx_tail != tx_head) REG32(UART0 + UART_THR) = tx_ring[tx_tail++ % TX_RING];
    else { REG32(UART0 + UART_IER) = IER_RDA; return; }
    uart0_tx_bytes++;
    REG32(UART0 + UART_IER) = IER_RDA | IER_THRE;
}

void uart0_isr(void)
{
    u32 lsr;
    while ((lsr = REG32(UART0 + UART_LSR)) & LSR_DR) {
        u8 b = (u8)REG32(UART0 + UART_RBR);
        if (lsr & LSR_OE) uart0_overruns++;
        if (lsr & LSR_ERRORS) uart0_line_errors++;
        uart0_rx_bytes++;
        if (rx_head - rx_tail < RX_RING) rx_ring[rx_head++ % RX_RING] = b;
        else uart0_ring_lost++;
    }
    if (lsr & LSR_OE) uart0_overruns++;
    (void)REG32(UART0 + UART_IIR);            /* acknowledges a THRE interrupt */
    transmit();
}

int uart0_read(u8 *byte)
{
    if (rx_tail == rx_head) return 0;
    *byte = rx_ring[rx_tail % RX_RING];
    rx_tail++;
    return 1;
}

u32 uart0_tx_free(void) { return TX_RING - (tx_head - tx_tail); }

int uart0_send(const u8 *data, u32 bytes)
{
    u32 gie = irq_global_disable(), i;
    int result = -1;
    if (TX_RING - (tx_head - tx_tail) >= bytes) {
        for (i = 0; i < bytes; i++) tx_ring[(tx_head + i) % TX_RING] = data[i];
        tx_head += bytes;
        transmit();
        result = 0;
    }
    if (gie) irq_global_enable();
    return result;
}

int uart0_send_realtime(u8 byte)
{
    u32 gie = irq_global_disable();
    int result = -1;
    if (rt_head - rt_tail < RT_RING) {
        rt_ring[rt_head++ % RT_RING] = byte;
        transmit();
        result = 0;
    }
    if (gie) irq_global_enable();
    return result;
}
