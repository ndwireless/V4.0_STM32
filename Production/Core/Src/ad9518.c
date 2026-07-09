#include "ad9518.h"

/* ---- low level ---------------------------------------------------------- */

int ad9518_reg_write(ad9518_dev_t *d, uint16_t reg, uint8_t val)
{
    uint16_t cmd = AD9518_WRITE | AD9518_CNT(1) | AD9518_ADDR(reg);
    uint8_t buf[3] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF), val};
    return d->spi_write(buf, 3);
}

int ad9518_reg_read(ad9518_dev_t *d, uint16_t reg, uint8_t *val)
{
    uint16_t cmd = AD9518_READ | AD9518_CNT(1) | AD9518_ADDR(reg);
    uint8_t tx[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    return d->spi_read(tx, 2, val, 1);
}

int ad9518_update(ad9518_dev_t *d)
{
    return ad9518_reg_write(d, AD9518_TRANSFER, AD9518_TRANSFER_NOW);
}

int ad9518_read_part_id(ad9518_dev_t *d, uint8_t *id)
{
    return ad9518_reg_read(d, AD9518_PARTID, id);
}

/* ---- helpers ------------------------------------------------------------ */

/* divider = (LO+1)+(HI+1) = LO+HI+2, split as evenly as possible.
 * Reg byte = (HI<<4)|LO. Divider 1 uses the bypass bit instead. */
static void divider_to_reg(uint8_t div, uint8_t *reg_lohi, int *bypass)
{
    if (div <= 1)
    {
        *bypass = 1;
        *reg_lohi = 0;
        return;
    }
    *bypass = 0;
    uint8_t hi = div / 2 - 1;
    uint8_t lo = hi + (div & 1);
    if (hi > 15)
        hi = 15;
    if (lo > 15)
        lo = 15;
    *reg_lohi = (uint8_t)((hi << 4) | (lo & 0x0F));
}

/* ---- init: fixed 2000 MHz VCO ------------------------------------------- */

int ad9518_init(ad9518_dev_t *d, int four_wire)
{
    int ret;
    uint8_t conf = AD9518_LONG_INSTR | (four_wire ? AD9518_SDO_ACTIVE : 0);

    /* 1. Soft reset then normal serial config (3-wire: no SDO-active bit) */
    ret = ad9518_reg_write(d, AD9518_SERCONF, conf | AD9518_SOFT_RESET);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_SERCONF, conf);
    if (ret < 0)
        return ret;

    /* 2. PLL: R=1, P=16 DM, B=5, A=0 -> N=80 -> 25MHz*80 = 2000 MHz VCO */
    ret = ad9518_reg_write(d, AD9518_PFD_CP, 0x7D);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_RCNT_L, 0x01);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_RCNT_H, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_ACNT, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_BCNT_L, 0x05);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_BCNT_H, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_PLL1, 0x06);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_PLL2, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_PLL3, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_PLL4, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_PLL6, 0x00);
    if (ret < 0)
        return ret;

    /* 3. Clock source: internal VCO -> /2 VCO divider -> channels */
    ret = ad9518_reg_write(d, AD9518_VCO_DIVIDER, 0x00);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_INPUT_CLKS, 0x02);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_POWDOWN_SYNC, 0x00);
    if (ret < 0)
        return ret;

    /* 4. Latch, then run VCO calibration (first time: set cal + update) */
    ret = ad9518_update(d);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_PLL3, AD9518_PLL3_VCO_CAL);
    if (ret < 0)
        return ret;
    ret = ad9518_update(d);
    if (ret < 0)
        return ret;

    if (d->delay_ms)
        d->delay_ms(10);

    return 0;
}

/* ---- per-channel frequency (chan 0..2) ---------------------------------- */

int ad9518_set_channel_freq(ad9518_dev_t *d, uint8_t chan, uint32_t hz,
                            uint32_t *actual_hz)
{
    if (chan >= AD9518_NUM_CHANNELS || hz == 0)
        return -1;

    uint32_t div = (AD9518_CHDIV_IN_HZ + hz / 2) / hz; /* round */
    if (div < 1)
        div = 1;
    if (div > 32)
        div = 32;

    uint8_t reg_lohi;
    int bypass;
    divider_to_reg((uint8_t)div, &reg_lohi, &bypass);

    int ret;
    uint8_t p2 = 0;

    ret = ad9518_reg_write(d, AD9518_PECLDIV_1(chan), reg_lohi);
    if (ret < 0)
        return ret;

    if (bypass)
        p2 |= AD9518_PECLDIV_BYPASS;
    ret = ad9518_reg_write(d, AD9518_PECLDIV_2(chan), p2);
    if (ret < 0)
        return ret;

    ret = ad9518_reg_write(d, AD9518_PECLDIV_3(chan), 0x00);
    if (ret < 0)
        return ret;

    ret = ad9518_update(d);
    if (ret < 0)
        return ret;

    if (actual_hz)
        *actual_hz = (uint32_t)(AD9518_CHDIV_IN_HZ / div);
    return 0;
}

int ad9518_get_channel_freq(ad9518_dev_t *d, uint8_t chan, uint32_t *hz)
{
    if (chan >= AD9518_NUM_CHANNELS || !hz)
        return -1;

    uint8_t p1, p2;
    int ret;
    ret = ad9518_reg_read(d, AD9518_PECLDIV_1(chan), &p1);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_read(d, AD9518_PECLDIV_2(chan), &p2);
    if (ret < 0)
        return ret;

    uint32_t div;
    if (p2 & AD9518_PECLDIV_BYPASS)
        div = 1;
    else
        div = (uint32_t)((p1 & 0x0F) + (p1 >> 4) + 2);

    *hz = (uint32_t)(AD9518_CHDIV_IN_HZ / div);
    return 0;
}

/* ---- enable/disable a channel's two LVPECL outputs ---------------------- */

int ad9518_channel_enable(ad9518_dev_t *d, uint8_t chan, int on)
{
    if (chan >= AD9518_NUM_CHANNELS)
        return -1;

    uint8_t out0 = (uint8_t)(2 * chan);
    uint8_t out1 = (uint8_t)(2 * chan + 1);
    uint8_t val = on ? 0x08 : 0x0B; /* bits[1:0]: 00=on, 11=power down */

    int ret;
    ret = ad9518_reg_write(d, AD9518_OUT_LVPECL(out0), val);
    if (ret < 0)
        return ret;
    ret = ad9518_reg_write(d, AD9518_OUT_LVPECL(out1), val);
    if (ret < 0)
        return ret;
    return ad9518_update(d);
}