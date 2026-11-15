/* C6748 peripheral addresses, pins and register access (docs/hardware.md). Addresses are TI's (C6748 data
 * sheet and technical reference manual); pin roles are those of the SmplTrek board. Peripheral space is
 * uncached on the C674x, so plain volatile accesses are enough. */
#ifndef HW_H
#define HW_H

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;

#define REG32(a) (*(volatile u32 *)(a))
#define REG8(a)  (*(volatile u8 *)(a))

#define PSC0     0x01C10000u
#define PSC1     0x01E27000u
#define SYSCFG0  0x01C14000u          /* PINMUXn at +0x120 + 4n */
#define SYSCFG1  0x01E2C000u          /* PUPD_ENA at +0x0C */
#define GPIO     0x01E26000u
#define TIMER0   0x01C20000u          /* TIMER64P0 */
#define SPI1     0x01F0E000u
#define UART1    0x01D0C000u
#define I2C0     0x01C22000u          /* the codec's control port; no PSC module, runs from the 24 MHz oscillator */
#define MCASP0   0x01D00000u          /* control registers; the DMA port is MCASP0_DATA */
#define MCASP0_DATA 0x01D02000u
#define EDMA     0x01C00000u          /* EDMA3 CC0; PaRAM at +0x4000, shadow region 0 at +0x2000 */

/* PSC modules used (docs/hardware.md, "Power and clocks") */
#define PSC1_SPI1   10
#define PSC1_UART1  12
#define PSC1_MCASP0 7
#define PSC0_EDMA_CC 0
#define PSC0_EDMA_TC0 1

/* GPIO pins (bank, pin). Bank pairs share registers: bank 2n in bits 0-15, 2n+1 in bits 16-31. */
#define POWER_KEY_BANK   3
#define POWER_KEY_PIN    0            /* active low */
#define POWER_HOLD_BANK  4
#define POWER_HOLD_PIN   11
#define POWER_SRC_BANK   3
#define POWER_SRC_PIN    7            /* high = batteries (unconfirmed) */
#define OLED_DC_BANK     2
#define OLED_DC_PIN      11           /* 1 = data, 0 = command */
#define CODEC_RESET_BANK 4            /* GP4[10]: held low at start, high before the first I2C access (the codec's RESET, unconfirmed) */
#define CODEC_RESET_PIN  10
#define OUT_MUTE_BANK    2            /* GP2[9]: output mute, active high, driven with the DAC volume mute */
#define OUT_MUTE_PIN     9
#define SPK_MUTE_BANK    2            /* GP2[8]: speaker mute, active high (the original firmware's SPEAKER MUTE setting) */
#define SPK_MUTE_PIN     8

#endif
