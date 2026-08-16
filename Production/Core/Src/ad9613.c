#include "ad9613.h"
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* debug                                                               */
/* ------------------------------------------------------------------ */

static void D(ad9613_dev_t *d, const char *s)
{
    if (d->dbg)
        d->dbg(s);
}

static void Df(ad9613_dev_t *d, const char *fmt, ...)
{
    if (!d->dbg)
        return;
    char b[144];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    d->dbg(b);
}

/* ------------------------------------------------------------------ */
/* bit-banged serial port                                              */
/*                                                                     */
/* AN-877 format: a 16-bit instruction word, then data. Bit 15 is R/W,  */
/* bits 14:13 (W1:W0) give the byte count minus one, bits 12:0 are the  */
/* address. Single-byte transfers only here, so W1:W0 = 00.             */
/*                                                                     */
/* SCLK idles low. Write data is registered on the SCLK rising edge and */
/* read data on the falling edge, so read data is sampled while SCLK is */
/* low - before the clock pulse, which also keeps the final bit inside  */
/* the window where the part is still driving SDIO.                     */
/* ------------------------------------------------------------------ */

static void shift_out(ad9613_dev_t *d, uint8_t byte)
{
    for (int bit = 7; bit >= 0; bit--)
    {
        d->sdio((byte >> bit) & 1);
        d->bit_delay();
        d->sclk(1);
        d->bit_delay();
        d->sclk(0);
    }
}

static uint8_t shift_in(ad9613_dev_t *d)
{
    uint8_t v = 0;
    for (int bit = 7; bit >= 0; bit--)
    {
        d->bit_delay();
        if (d->sdio_read())
            v |= (uint8_t)(1u << bit);
        d->sclk(1);
        d->bit_delay();
        d->sclk(0);
    }
    return v;
}

int ad9613_write(ad9613_dev_t *d, uint16_t reg, uint8_t val)
{
    uint16_t cmd = (uint16_t)(reg & 0x1FFFu); /* write, 1 byte */

    d->sdio_dir(1);
    d->sclk(0);
    d->cs(0);
    d->bit_delay();

    shift_out(d, (uint8_t)(cmd >> 8));
    shift_out(d, (uint8_t)(cmd & 0xFF));
    shift_out(d, val);

    d->bit_delay();
    d->cs(1);
    d->bit_delay();
    return 0;
}

int ad9613_read(ad9613_dev_t *d, uint16_t reg, uint8_t *val)
{
    uint16_t cmd = (uint16_t)(0x8000u | (reg & 0x1FFFu)); /* read, 1 byte */

    d->sdio_dir(1);
    d->sclk(0);
    d->cs(0);
    d->bit_delay();

    shift_out(d, (uint8_t)(cmd >> 8));
    shift_out(d, (uint8_t)(cmd & 0xFF));

    /* Turnaround: the part takes over SDIO for the data phase. */
    d->sdio_dir(0);
    d->bit_delay();
    d->bit_delay();

    *val = shift_in(d);

    d->cs(1);
    d->sdio_dir(1);
    d->bit_delay();
    return 0;
}

int ad9613_transfer(ad9613_dev_t *d)
{
    /* Registers 0x08 to 0x20 and 0x3A are shadowed; writes do not take
     * effect until the transfer bit is set. The bit is self-clearing. */
    return ad9613_write(d, AD9613_TRANSFER, AD9613_TRANSFER_NOW);
}

/* ------------------------------------------------------------------ */
/* identification                                                      */
/* ------------------------------------------------------------------ */

int ad9613_chip_id(ad9613_dev_t *d, uint8_t *id)
{
    return ad9613_read(d, AD9613_CHIP_ID, id);
}

int ad9613_max_msps(ad9613_dev_t *d)
{
    uint8_t g = 0;
    if (ad9613_read(d, AD9613_CHIP_GRADE, &g) < 0)
        return 0;

    switch (g & AD9613_GRADE_MASK)
    {
    case AD9613_GRADE_250:
        return 250;
    case AD9613_GRADE_210:
        return 210;
    case AD9613_GRADE_170:
        return 170;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

int ad9613_init_lvds(ad9613_dev_t *d, uint32_t sample_hz,
                     ad9613_format_t format)
{
    D(d, "\r\n==== AD9613 bring-up ====\r\n");

    /* 1. Soft reset. The channel index must be 0x03 when writing 0x00. */
    ad9613_write(d, AD9613_CHAN_INDEX, AD9613_CHAN_BOTH);
    ad9613_write(d, AD9613_SPI_CONFIG,
                 (uint8_t)(AD9613_SPI_CONFIG_DEF | AD9613_SOFT_RESET));
    d->delay_ms(2);
    ad9613_write(d, AD9613_SPI_CONFIG, AD9613_SPI_CONFIG_DEF);
    d->delay_ms(2);

    /* 2. Identify. */
    uint8_t id = 0;
    ad9613_chip_id(d, &id);
    Df(d, "chip ID = 0x%02X (expect 0x%02X)\r\n", id, AD9613_ID_EXPECTED);
    if (id != AD9613_ID_EXPECTED)
    {
        D(d, "AD9613 not responding correctly on the serial port\r\n");
        return -1;
    }

    int max_msps = ad9613_max_msps(d);
    uint32_t msps = sample_hz / 1000000u;
    Df(d, "speed grade = %d MSPS, requested %lu MSPS\r\n",
       max_msps, (unsigned long)msps);

    if (max_msps && msps > (uint32_t)max_msps)
    {
        Df(d, "REQUESTED RATE EXCEEDS GRADE - %lu > %d MSPS\r\n",
           (unsigned long)msps, max_msps);
        return -1;
    }
    if (msps < 40)
    {
        D(d, "WARNING: below the 40 MSPS minimum; dynamic performance\r\n");
        D(d, "         degrades at clock rates under 40 MSPS\r\n");
    }

    /* 3. Both channels receive the following writes. */
    ad9613_write(d, AD9613_CHAN_INDEX, AD9613_CHAN_BOTH);

    /* 4. Normal power, duty cycle stabilizer on.
     *    The DCS retimes the nonsampling edge to give an internal 50%
     *    duty cycle, which matters because this part uses both clock
     *    edges internally. It does not reduce rising-edge jitter. */
    ad9613_write(d, AD9613_POWER_MODES, AD9613_PWR_NORMAL);
    ad9613_write(d, AD9613_GLOBAL_CLOCK, AD9613_DCS_ENABLE);

    /* 5. Clock divider bypassed: conversion rate = CLK+/CLK- rate.
     *    Any divide ratio other than 000 forces the DCS active. */
    ad9613_write(d, AD9613_CLOCK_DIVIDE, AD9613_CLKDIV_1);

    /* 6. Output mode: LVDS, outputs enabled, chosen data format.
     *    Bit 2 set means normal (noninverted) output. */
    uint8_t outmode = AD9613_OUT_NOT_INVERTED;
    outmode |= (format == AD9613_FORMAT_OFFSET_BINARY)
                   ? AD9613_FMT_OFFSET_BIN
                   : AD9613_FMT_TWOS_COMP;
    ad9613_write(d, AD9613_OUTPUT_MODE, outmode);

    /* 7. LVDS drive: 3.5 mA gives ANSI-644 levels (VOD 250-450 mV).
     *    Drop to AD9613_LVDS_2_0MA for reduced-swing mode. */
    ad9613_write(d, AD9613_OUTPUT_ADJUST, AD9613_LVDS_3_5MA);

    /* 8. Interleaved parallel LVDS on D0..D11 plus DCO and OR.
     *    Bit 5 clear selects interleaved parallel rather than the
     *    channel-multiplexed (even/odd) pinout. DCO not inverted. */
    ad9613_write(d, AD9613_CLOCK_PHASE, 0x00);
    ad9613_write(d, AD9613_DCO_DELAY, 0x00); /* no added DCO delay */

    /* 9. Full-scale input span 1.75 V p-p (default). */
    ad9613_write(d, AD9613_INPUT_SPAN, 0x00);

    /* 10. No offset trim, no test pattern. */
    ad9613_write(d, AD9613_OFFSET_ADJUST, 0x00);
    ad9613_write(d, AD9613_TEST_MODE, 0x00);

    /* 11. Sync off unless multiple parts need aligning; leaving the main
     *     sync buffer disabled conserves power. */
    ad9613_write(d, AD9613_SYNC_CONTROL, 0x00);

    /* 12. Commit the shadowed registers. */
    ad9613_transfer(d);
    d->delay_ms(1);

    Df(d, "configured: interleaved parallel LVDS, %s, ANSI levels\r\n",
       (format == AD9613_FORMAT_OFFSET_BINARY) ? "offset binary"
                                               : "twos complement");
    D(d, "DCO carries the data clock; OEB pin must be low for output\r\n");
    return 0;
}

int ad9613_test_pattern(ad9613_dev_t *d, uint8_t mode)
{
    ad9613_write(d, AD9613_CHAN_INDEX, AD9613_CHAN_BOTH);
    ad9613_write(d, AD9613_TEST_MODE, (uint8_t)(mode & 0x0F));
    int r = ad9613_transfer(d);
    Df(d, "test pattern mode 0x%02X %s\r\n", mode & 0x0F,
       (mode & 0x0F) ? "enabled" : "off");
    return r;
}

void ad9613_dump(ad9613_dev_t *d)
{
    static const uint16_t regs[] = {
        AD9613_SPI_CONFIG,
        AD9613_CHIP_ID,
        AD9613_CHIP_GRADE,
        AD9613_CHAN_INDEX,
        AD9613_POWER_MODES,
        AD9613_GLOBAL_CLOCK,
        AD9613_CLOCK_DIVIDE,
        AD9613_TEST_MODE,
        AD9613_OFFSET_ADJUST,
        AD9613_OUTPUT_MODE,
        AD9613_OUTPUT_ADJUST,
        AD9613_CLOCK_PHASE,
        AD9613_DCO_DELAY,
        AD9613_INPUT_SPAN,
        AD9613_SYNC_CONTROL,
    };

    D(d, "[ad9613 dump]\r\n");
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
    {
        uint8_t v = 0;
        ad9613_read(d, regs[i], &v);
        Df(d, "  0x%02X = 0x%02X\r\n", regs[i], v);
    }
}