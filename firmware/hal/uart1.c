#include "hal.h"

/* Panel MCU link on UART1 (docs/hardware.md "Panel MCU"), set up as the original firmware does: PSC1 module 12,
 * PINMUX4 nibbles 6 and 7 = UART1 RXD/TXD, MDR = 0 (16x), DLL = 25, DLH = 0 (186 MHz / 16 / 25 = 465,000),
 * LCR = 3 (8N1), FCR = 7 then 1 (FIFOs reset, trigger 1), then the two enables PWREMU_MGMT = 0x2001 and
 * 0x6001. The MCU sends all 16 analogue channels continuously, about 4,200 messages (12,600 bytes) a second
 * whether or not a pad is touched (smpltrek-re session M1, measured), so the 16-byte FIFO fills in about 1.3 ms.
 * Start-up polls; from uart1_irq_start on, the receive interrupt (IER = 1, trigger level 1) moves every byte
 * into a ring that uart1_poll parses in the main loop. The original firmware also takes receive interrupts. */
#define UART_RBR  0x00
#define UART_THR  0x00
#define UART_IER  0x04
#define UART_FCR  0x08
#define UART_LCR  0x0C
#define UART_MCR  0x10
#define UART_LSR  0x14
#define UART_DLL  0x20
#define UART_DLH  0x24
#define UART_PWREMU 0x30
#define UART_MDR  0x34
#define LSR_DR    0x01
#define LSR_OE    0x02
#define LSR_THRE  0x20
#define LSR_TEMT  0x40
#define MCR_LOOP  0x10

u16 pad_value[16];
u8 mcu_info[6];
u32 pad_messages, mcu_replies, uart1_dropped;
u32 uart1_rx_bytes;                      /* every byte the receiver delivered, message or not */
u32 uart1_overruns;                      /* LSR OE seen: bytes lost in the receive FIFO */
u32 uart1_ring_lost;                     /* bytes dropped because the ring was full */
u8 uart1_rx_last;
int uart1_loopback_ok;                   /* the receiver self-test at set-up: a byte sent in internal loopback came back */
void (*pad_callback)(int channel, unsigned value);
static int loopback_test(void);

void uart1_init(void)
{
    psc_enable(PSC1, PSC1_UART1);
    pinmux_set(4, 6, 2);
    pinmux_set(4, 7, 2);
    REG32(UART1 + UART_MDR) = 0;
    REG32(UART1 + UART_DLL) = 25;
    REG32(UART1 + UART_DLH) = 0;
    REG32(UART1 + UART_LCR) = 3;
    REG32(UART1 + UART_FCR) = 7;
    REG32(UART1 + UART_FCR) = 1;
    REG32(UART1 + UART_IER) = 0;
    REG32(UART1 + UART_PWREMU) = 0x2001;
    REG32(UART1 + UART_PWREMU) = 0x6001;
    uart1_loopback_ok = loopback_test();
}

/* Receiver self-test at set-up, for diagnosing a link that sends but never receives. MCR LOOP
 * feeds the transmitter's output into the receiver inside the UART and holds the TXD pin high (SPRUH79 "Loopback
 * control"), so the MCU sees nothing. A byte that comes back proves the receive side of the peripheral (reset
 * release, FIFO, LSR DR, RBR); one that does not points at this set-up. Then back to normal with the FIFO empty. */
static int loopback_test(void)
{
    u32 start = tsc_read();
    u8 got = 0;
    int ok = 0;
    REG32(UART1 + UART_MCR) = MCR_LOOP;
    REG32(UART1 + UART_THR) = 0x5A;
    while (!(REG32(UART1 + UART_LSR) & LSR_DR))
        if (tsc_read() - start > 10 * TICKS_PER_MS) goto out;
    got = (u8)REG32(UART1 + UART_RBR);
    ok = got == 0x5A;
out:
    start = tsc_read();
    while (!(REG32(UART1 + UART_LSR) & LSR_TEMT))              /* the shift register empty before the pin is reconnected */
        if (tsc_read() - start > 10 * TICKS_PER_MS) break;
    REG32(UART1 + UART_MCR) = 0;
    return ok;
}

void uart1_send(u8 byte)
{
    u32 start = tsc_read();
    while (!(REG32(UART1 + UART_LSR) & LSR_THRE))
        if (tsc_read() - start > 10 * TICKS_PER_MS) { uart1_dropped++; return; }   /* transmitter never ready: drop the byte */
    REG32(UART1 + UART_THR) = byte;
}

/* Messages from the MCU: `Bn lo hi` = analogue channel n, 14-bit lo | hi << 7 (channels 0-14 pads,
 * 15 battery); `A1` + six bytes = MCU information (bytes 3-5 shown as the MCU version). */
static u8 msg[8];
static unsigned msg_len, msg_need;

#define RING 1024u                              /* bytes: about 80 ms of the MCU's traffic */
static u8 ring[RING];
static volatile u32 ring_head, ring_tail;      /* head: written by the receiver; tail: by uart1_poll */
static int irq_on;

static void drain(void)                        /* the FIFO into the ring */
{
    u32 lsr;
    while ((lsr = REG32(UART1 + UART_LSR)) & LSR_DR) {
        u8 b = (u8)REG32(UART1 + UART_RBR);
        if (lsr & LSR_OE) uart1_overruns++;
        if (ring_head - ring_tail < RING) ring[ring_head++ % RING] = b;
        else uart1_ring_lost++;
    }
    if (lsr & LSR_OE) uart1_overruns++;
}

void uart1_isr(void) { drain(); }

void uart1_irq_start(void)                     /* call with interrupts off; the caller enables the CPU interrupt */
{
    drain();
    REG32(UART1 + UART_IER) = 1;               /* received data available */
    irq_on = 1;
}

int uart1_poll(void)
{
    int completed = 0;
    if (!irq_on) drain();
    while (ring_tail != ring_head) {
        u8 b = ring[ring_tail % RING];
        ring_tail++;
        uart1_rx_bytes++;
        uart1_rx_last = b;
        if (msg_need == 0) {
            if ((b & 0xF0) == 0xB0) { msg[0] = b; msg_len = 1; msg_need = 3; }
            else if (b == 0xA1) { msg[0] = b; msg_len = 1; msg_need = 7; }
            continue;                           /* anything else: not a message start, dropped */
        }
        if (b & 0x80) {                         /* a new status byte inside a message: resynchronise */
            msg_need = 0;
            if ((b & 0xF0) == 0xB0) { msg[0] = b; msg_len = 1; msg_need = 3; }
            else if (b == 0xA1) { msg[0] = b; msg_len = 1; msg_need = 7; }
            continue;
        }
        msg[msg_len++] = b;
        if (msg_len == msg_need) {
            if (msg[0] == 0xA1) {
                for (int i = 0; i < 6; i++) mcu_info[i] = msg[1 + i];
                mcu_replies++;
            } else {
                pad_value[msg[0] & 0x0F] = (u16)(msg[1] | (msg[2] << 7));
                pad_messages++;
                if (pad_callback) pad_callback(msg[0] & 0x0F, pad_value[msg[0] & 0x0F]);
            }
            msg_need = 0;
            completed = 1;
        }
    }
    return completed;
}

/* `C1 a b c v`: LED (a, b, c) to v. */
void led_set(u8 a, u8 b, u8 c, u8 value)
{
    uart1_send(0xC1);
    uart1_send(a);
    uart1_send(b);
    uart1_send(c);
    uart1_send(value);
}
