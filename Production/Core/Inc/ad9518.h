#ifndef AD9518_H
#define AD9518_H

#include <stdint.h>

/* --- board-fixed assumptions ---------------------------------------------
 *   Reference : 25 MHz crystal on REF1
 *   VCO       : internal, locked to 2000 MHz  (R=1, P=16 DM, B=5, A=0)
 *   VCO div   : /2  -> 1000 MHz feeds the channel dividers
 *   Outputs   : 6 LVPECL in 3 pairs; each pair shares one 1..32 divider
 *   Serial    : 3-wire (SDIO bidirectional), SDO unused
 *   => per-channel output = 1000 MHz / channel_divider  (chan 0..2)
 * ------------------------------------------------------------------------- */
#define AD9518_REF_HZ 25000000UL
#define AD9518_VCO_HZ 2000000000UL
#define AD9518_CHDIV_IN_HZ 1000000000UL /* VCO / 2 */
#define AD9518_NUM_CHANNELS 3

/* Instruction word (16-bit, MSB first):
 *   bit15    : R/W (1=read, 0=write)
 *   bit14:13 : (byte count - 1)
 *   bit11:0  : register address
 */
#define AD9518_READ (1u << 15)
#define AD9518_WRITE (0u << 15)
#define AD9518_CNT(x) (((x) - 1u) << 13)
#define AD9518_ADDR(x) ((x) & 0x0FFFu)

/* Registers */
#define AD9518_SERCONF 0x000
#define AD9518_PARTID 0x003
#define AD9518_PFD_CP 0x010
#define AD9518_RCNT_L 0x011
#define AD9518_RCNT_H 0x012
#define AD9518_ACNT 0x013
#define AD9518_BCNT_L 0x014
#define AD9518_BCNT_H 0x015
#define AD9518_PLL1 0x016
#define AD9518_PLL2 0x017
#define AD9518_PLL3 0x018
#define AD9518_PLL4 0x019
#define AD9518_PLL6 0x01B
#define AD9518_OUT_LVPECL(x) (0x0F0 + (x))    /* x=0..5 */
#define AD9518_PECLDIV_1(c) (0x190 + (c) * 3) /* c=0..2 divide LO/HI */
#define AD9518_PECLDIV_2(c) (0x191 + (c) * 3) /* bypass/phase */
#define AD9518_PECLDIV_3(c) (0x192 + (c) * 3) /* source mux */
#define AD9518_VCO_DIVIDER 0x1E0
#define AD9518_INPUT_CLKS 0x1E1
#define AD9518_POWDOWN_SYNC 0x230
#define AD9518_TRANSFER 0x232

/* Bit values */
#define AD9518_TRANSFER_NOW 0x01
#define AD9518_PLL3_VCO_CAL 0x01
#define AD9518_SOFT_RESET 0x24
#define AD9518_SDO_ACTIVE 0x81 /* 4-wire only; NOT used in 3-wire */
#define AD9518_LONG_INSTR 0x18
#define AD9518_PECLDIV_BYPASS 0x80

/* Provide these two. Each must assert CS, move bytes, deassert CS.
 * Return 0 on success, negative on error. */
typedef int (*ad9518_spi_write_fn)(const uint8_t *buf, uint16_t len);
typedef int (*ad9518_spi_read_fn)(const uint8_t *tx, uint16_t txlen,
                                  uint8_t *rx, uint16_t rxlen);

typedef struct
{
    ad9518_spi_write_fn spi_write;
    ad9518_spi_read_fn spi_read;
    void (*delay_ms)(uint32_t ms); /* for VCO cal settling; may be NULL */
} ad9518_dev_t;

/* Low level */
int ad9518_reg_write(ad9518_dev_t *d, uint16_t reg, uint8_t val);
int ad9518_reg_read(ad9518_dev_t *d, uint16_t reg, uint8_t *val);
int ad9518_update(ad9518_dev_t *d);
int ad9518_read_part_id(ad9518_dev_t *d, uint8_t *id);

/* High level */
int ad9518_init(ad9518_dev_t *d, int four_wire); /* pass 0 for your 3-wire board */
int ad9518_set_channel_freq(ad9518_dev_t *d, uint8_t chan, uint32_t hz,
                            uint32_t *actual_hz); /* chan 0..2 */
int ad9518_channel_enable(ad9518_dev_t *d, uint8_t chan, int on);
int ad9518_get_channel_freq(ad9518_dev_t *d, uint8_t chan, uint32_t *hz);

#endif /* AD9518_H */