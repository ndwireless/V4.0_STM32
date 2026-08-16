#include "ad9518.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* debug helpers                                                       */
/* ------------------------------------------------------------------ */

static void D(ad9518_dev_t *d, const char *s)
{
    if (d->dbg)
        d->dbg(s);
}

static void Df(ad9518_dev_t *d, const char *fmt, ...)
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
/* Everything is bit-banged in both directions on purpose. Switching a */
/* hardware SPI peripheral between transmit and receive mid-transaction */
/* in half-duplex mode is the single most common source of AD951x       */
/* readback failures, so the peripheral is not used at all.             */
/*                                                                     */
/* Format: SCLK idles low. The part latches write data on the SCLK      */
/* rising edge and drives read data on the falling edge, so read data   */
/* is stable while SCLK is high.                                        */
/* ------------------------------------------------------------------ */

static void shift_out(ad9518_dev_t *d, uint8_t byte)
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

static uint8_t shift_in(ad9518_dev_t *d)
{
    uint8_t v = 0;
    int (*rd)(void) = (d->wire == AD9518_WIRE_4 && d->sdo_read)
                          ? d->sdo_read
                          : d->sdio_read;

    for (int bit = 7; bit >= 0; bit--)
    {
        if (d->sample == AD9518_SAMPLE_SCLK_LOW)
        {
            /*
             * Sample while SCLK is still low, before driving the clock.
             *
             * The part presents each bit on the falling edge, so the bit is
             * already valid here. Sampling before the pulse also means the
             * last bit is captured while the part is still driving it -
             * sampling after the eighth rising edge can land in the window
             * where the part has released SDIO, which a bus pull-up then
             * reads back as a phantom 1 in bit 0.
             */
            d->bit_delay();
            if (rd())
                v |= (uint8_t)(1u << bit);
            d->sclk(1);
            d->bit_delay();
            d->sclk(0);
        }
        else
        {
            d->sclk(1);
            d->bit_delay();
            if (rd())
                v |= (uint8_t)(1u << bit);
            d->sclk(0);
            d->bit_delay();
        }
    }
    return v;
}

/* Shift out the instruction word in whichever format is currently selected.
 * Short form is 1 byte: R/W in bit 7, 7-bit address in bits 6:0.
 * Long form is 2 bytes: R/W in bit 15, byte count in 14:13, address in 11:0. */
static void shift_instr(ad9518_dev_t *d, uint16_t reg, int is_read)
{
    if (d->instr == AD9518_INSTR_SHORT)
    {
        uint8_t b = (uint8_t)((is_read ? 0x80u : 0x00u) | (reg & 0x7Fu));
        shift_out(d, b);
    }
    else
    {
        uint16_t cmd = (is_read ? AD9518_INSTR_READ : AD9518_INSTR_WRITE) | AD9518_INSTR_CNT(1) | AD9518_INSTR_ADDR(reg);
        shift_out(d, (uint8_t)(cmd >> 8));
        shift_out(d, (uint8_t)(cmd & 0xFF));
    }
}

int ad9518_write(ad9518_dev_t *d, uint16_t reg, uint8_t val)
{
    d->sdio_dir(1);
    d->sclk(0);
    d->cs(0);
    d->bit_delay();

    shift_instr(d, reg, 0);
    shift_out(d, val);

    d->bit_delay();
    d->cs(1);
    d->bit_delay();
    return 0;
}

/* Internal read with control over what level SDIO is parked at just before
 * the master releases the line. Used by the bus diagnosis below: if the
 * byte read back tracks the parked level, nothing is driving the line. */
static int read_parked(ad9518_dev_t *d, uint16_t reg, uint8_t *val, int park)
{
    d->sdio_dir(1);
    d->sclk(0);
    d->cs(0);
    d->bit_delay();

    shift_instr(d, reg, 1);

    /* Park SDIO at a known level, then release it. */
    if (park >= 0)
    {
        d->sdio(park);
        d->bit_delay();
    }

    if (d->wire == AD9518_WIRE_3)
        d->sdio_dir(0);
    d->bit_delay();
    d->bit_delay();

    *val = shift_in(d);

    d->cs(1);
    d->sdio_dir(1);
    d->bit_delay();
    return 0;
}

int ad9518_read(ad9518_dev_t *d, uint16_t reg, uint8_t *val)
{
    return read_parked(d, reg, val, -1);
}

int ad9518_transfer(ad9518_dev_t *d)
{
    return ad9518_write(d, AD9518_TRANSFER, AD9518_TRANSFER_NOW);
}

/* ------------------------------------------------------------------ */
/* identification and bus validation                                   */
/* ------------------------------------------------------------------ */

int ad9518_part_id_valid(uint8_t id)
{
    switch (id)
    {
    /* AD9518 grades */
    case 0x21:
    case 0x61:
    case 0xA1:
    case 0x63:
    case 0xE3:
    /* AD9516 grades, in case the board is populated with a sibling part */
    case 0x01:
    case 0x41:
    case 0x81:
    case 0x43:
    case 0xC3:
    /* AD9517 grades */
    case 0x11:
    case 0x51:
    case 0x91:
    case 0x53:
    case 0xD3:
        return 1;
    default:
        return 0;
    }
}

int ad9518_soft_reset(ad9518_dev_t *d)
{
    /* In 3-wire the dedicated SDO pin must stay disabled so that readback
     * comes back on SDIO. In 4-wire, SDO_ACTIVE enables the output pin.
     * This mirrors the Linux driver's handling of SPI_3WIRE. */
    uint8_t conf = AD9518_LONG_INSTR;
    if (d->wire == AD9518_WIRE_4)
        conf |= AD9518_SDO_ACTIVE;

    ad9518_write(d, AD9518_SERCONF, (uint8_t)(conf | AD9518_SOFT_RESET));
    d->delay_ms(2);
    ad9518_write(d, AD9518_SERCONF, conf);
    d->delay_ms(2);

    /* Read back the active registers rather than the write buffer, so a
     * verify after TRANSFER reflects what the part is really using. */
    ad9518_write(d, AD9518_RB_CTL, AD9518_RB_READ_ACTIVE);
    return 0;
}

int ad9518_part_id(ad9518_dev_t *d, uint8_t *id)
{
    return ad9518_read(d, AD9518_PARTID, id);
}

int ad9518_bus_autoprobe(ad9518_dev_t *d)
{
    static const char *wire_name[2] = {"3-wire", "4-wire"};
    static const char *sample_name[2] = {"SCLK high", "SCLK low "};
    static const char *instr_name[2] = {"long instr ", "short instr"};

    D(d, "[autoprobe] sweeping bus configurations\r\n");

    for (int in = 0; in < 2; in++)
    {
        for (int w = 0; w < 2; w++)
        {
            if (w == AD9518_WIRE_4 && d->sdo_read == 0)
                continue;

            for (int s = 0; s < 2; s++)
            {
                d->instr = (ad9518_instr_t)in;
                d->wire = (ad9518_wire_t)w;
                d->sample = (ad9518_sample_t)s;

                ad9518_soft_reset(d);

                uint8_t id = 0;
                ad9518_part_id(d, &id);

                Df(d, "  %s / %s / sample %s -> PARTID 0x%02X %s\r\n",
                   instr_name[in], wire_name[w], sample_name[s], id,
                   ad9518_part_id_valid(id) ? "<== VALID" : "");

                if (ad9518_part_id_valid(id))
                {
                    Df(d, "[autoprobe] using %s, %s, sample %s\r\n",
                       instr_name[in], wire_name[w], sample_name[s]);
                    return 0;
                }
            }
        }
    }

    D(d, "[autoprobe] no working configuration found\r\n");
    return -1;
}

/*
 * Decide whether anything is actually driving SDIO during the read phase.
 *
 * The same register is read twice, once with the master parking SDIO low
 * just before releasing it and once parking it high. If the byte read back
 * follows the parked level, the line is floating and holding residual
 * charge: the part is not responding. If both reads agree with each other
 * and ignore the parked level, something really is driving the line, and
 * the problem is sampling or protocol rather than a silent part.
 */
void ad9518_bus_diagnose(ad9518_dev_t *d)
{
    uint8_t lo = 0, hi = 0;

    D(d, "[diagnose] SDIO drive test\r\n");

    /* First: can the master itself control the line? This catches a short
     * to a supply rail or ground, which would make everything else moot. */
    d->sdio_dir(1);
    d->sdio(1);
    d->bit_delay();
    int reads_high = d->sdio_read();
    d->sdio(0);
    d->bit_delay();
    int reads_low = d->sdio_read();
    d->sdio_dir(1);

    if (!reads_high || reads_low)
    {
        Df(d, "  MCU drives high -> reads %d, drives low -> reads %d\r\n",
           reads_high, reads_low);
        D(d, "  SDIO does not follow the master. The net is shorted or\r\n");
        D(d, "  loaded by something else. Fix this before anything else.\r\n");
        return;
    }
    D(d, "  SDIO follows the master when driven: net is not shorted\r\n");

    read_parked(d, AD9518_PARTID, &lo, 0);
    read_parked(d, AD9518_PARTID, &hi, 1);

    Df(d, "  read with SDIO parked low  -> 0x%02X\r\n", lo);
    Df(d, "  read with SDIO parked high -> 0x%02X\r\n", hi);

    if (lo == 0x00 && hi == 0xFF)
    {
        D(d, "  Result tracks the parked level exactly.\r\n");
        D(d, "  The line is FLOATING during the read phase - the AD9518 is\r\n");
        D(d, "  not driving it. This is not a sampling or timing problem.\r\n");
        D(d, "  Likely causes, in order:\r\n");
        D(d, "    1. A RESET or power-down pin on the part is asserted.\r\n");
        D(d, "    2. CS is not reaching the part's CS pin (check the net,\r\n");
        D(d, "       not just the MCU pin).\r\n");
        D(d, "    3. Supplies missing or out of tolerance at the part.\r\n");
        D(d, "    4. SDIO open circuit between the part and the MCU.\r\n");
    }
    else if (lo == hi && lo == 0xFF)
    {
        D(d, "  Both reads return 0xFF regardless of the parked level.\r\n");
        D(d, "  The net is being pulled high - either by a pull-up resistor\r\n");
        D(d, "  on the board or by an active driver. These look identical\r\n");
        D(d, "  from here, but note that if the AD9518 were driving, PARTID\r\n");
        D(d, "  would be a real value rather than all ones. So the most\r\n");
        D(d, "  likely reading is: a pull-up holds the line and the part is\r\n");
        D(d, "  still silent. Check for a RESET or power-down pin that is\r\n");
        D(d, "  asserted, and CS continuity at the part's own pin.\r\n");
    }
    else if (lo == hi)
    {
        Df(d, "  Both reads agree (0x%02X) and ignore the parked level.\r\n", lo);
        D(d, "  Something is actively driving the line with real data, so\r\n");
        D(d, "  the part is responding. Suspect sample phase, bit order or\r\n");
        D(d, "  instruction framing rather than wiring.\r\n");
    }
    else
    {
        D(d, "  Mixed result: partially driven, partially floating.\r\n");
        D(d, "  Suspect marginal drive strength, a series resistor that is\r\n");
        D(d, "  too large, or excessive capacitance. Try a slower bus with\r\n");
        D(d, "  ad9518_port_set_bit_delay().\r\n");
    }
}

int ad9518_bus_selftest(ad9518_dev_t *d)
{
    /* PLL2 (0x017) is a benign scratch location: it holds no critical
     * setting in this configuration and is fully readable/writable. */
    static const uint8_t patterns[] = {0x00, 0xA5, 0x5A, 0xFF, 0x00};
    int fails = 0;

    D(d, "[selftest] write/readback patterns to 0x017\r\n");
    for (unsigned i = 0; i < sizeof(patterns); i++)
    {
        uint8_t got = 0;
        ad9518_write(d, AD9518_PLL2, patterns[i]);
        ad9518_transfer(d);
        ad9518_read(d, AD9518_PLL2, &got);
        Df(d, "  wrote 0x%02X read 0x%02X %s\r\n",
           patterns[i], got, (got == patterns[i]) ? "ok" : "FAIL");
        if (got != patterns[i])
            fails++;
    }
    ad9518_write(d, AD9518_PLL2, 0x00);
    ad9518_transfer(d);

    if (fails)
        Df(d, "[selftest] %d/%u patterns failed\r\n", fails, (unsigned)sizeof(patterns));
    else
        D(d, "[selftest] bus round-trips correctly\r\n");
    return fails ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* divider planning                                                    */
/* ------------------------------------------------------------------ */

/* PLL1[2:0] selects the prescaler. Table from the Linux driver; the high
 * bit marks fixed-divide modes, where the A counter is unused. */
#define IS_FD 0x80
static const uint8_t to_prescaler[8] = {
    1 | IS_FD,
    2 | IS_FD,
    2,
    4,
    8,
    16,
    32,
    3 | IS_FD,
};

/* The B counter cannot be clocked at the full VCO rate; the prescaler must
 * bring it down. Keep the prescaler output conservatively low. */
#define BCNT_IN_MAX_HZ 250000000u

int ad9518_plan(uint32_t ref_hz, uint32_t target_hz,
                uint32_t vco_min_hz, uint32_t vco_max_hz,
                ad9518_plan_t *plan)
{
    long best_err = -1;
    uint32_t best_centre = 0;
    uint32_t vco_mid = (uint32_t)(((uint64_t)vco_min_hz + vco_max_hz) / 2);

    if (!ref_hz || !target_hz || !plan)
        return -1;

    for (uint8_t pi = 0; pi < 8; pi++)
    {
        uint8_t praw = to_prescaler[pi];
        int fixed_div = (praw & IS_FD) != 0;
        uint8_t P = praw & (uint8_t)~IS_FD;

        for (uint16_t B = 3; B <= 1000; B++)
        {
            uint8_t amax = fixed_div ? 0 : (uint8_t)(P - 1);

            for (uint8_t A = 0; A <= amax; A++)
            {
                /* In dual-modulus mode the A counter must be less than B. */
                if (!fixed_div && A >= B)
                    continue;

                uint32_t N = (uint32_t)P * B + A;
                uint64_t vco = (uint64_t)ref_hz * N; /* R = 1 */

                if (vco < vco_min_hz || vco > vco_max_hz)
                    continue;
                if (vco / P > BCNT_IN_MAX_HZ)
                    continue;

                /* Distance from the middle of the tuning range. Among plans
                 * that hit the target exactly, prefer the one whose VCO sits
                 * furthest from the band edges - a VCO parked at the very
                 * bottom or top of its range may fail to lock over
                 * temperature and process variation even though it locks on
                 * the bench at room temperature. */
                uint32_t centre = (vco > vco_mid)
                                      ? (uint32_t)(vco - vco_mid)
                                      : (uint32_t)(vco_mid - vco);

                for (uint8_t vd = 2; vd <= 6; vd++)
                {
                    uint64_t bus = vco / vd;
                    for (uint8_t cd = 1; cd <= 32; cd++)
                    {
                        uint64_t out = bus / cd;
                        long err = (long)out - (long)target_hz;
                        if (err < 0)
                            err = -err;

                        int better = 0;
                        if (best_err < 0)
                            better = 1;
                        else if (err < best_err)
                            better = 1;
                        else if (err == best_err &&
                                 centre < best_centre)
                            better = 1;

                        if (better)
                        {
                            best_err = err;
                            best_centre = centre;
                            plan->r_counter = 1;
                            plan->a_counter = A;
                            plan->b_counter = B;
                            plan->prescaler_idx = pi;
                            plan->prescaler = P;
                            plan->vco_divider = vd;
                            plan->chan_divider = cd;
                            plan->vco_hz = (uint32_t)vco;
                            plan->out_hz = (uint32_t)out;
                        }
                    }
                }
            }
        }
    }

    if (best_err < 0)
        return -1;
    return (best_err == 0) ? 0 : 1;
}

/* Channel divider encoding: ratio 2..32 becomes a hi/lo nibble pair.
 * Same arithmetic as ad9517_calc_divider_hi_lo() in the Linux driver. */
static uint8_t div_hi_lo(uint8_t ratio)
{
    uint8_t hi = (uint8_t)(ratio / 2 - 1);
    uint8_t lo = (uint8_t)(hi + (ratio & 1));
    return (uint8_t)((hi << 4) | (lo & 0x0F));
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static void set_chan_divider(ad9518_dev_t *d, uint8_t idx, uint8_t ratio)
{
    if (ratio <= 1)
    {
        /* Divide by 1: bypass the divider entirely. */
        ad9518_write(d, AD9518_PECLDIV_1(idx), 0x00);
        ad9518_write(d, AD9518_PECLDIV_2(idx), AD9518_PECLDIV_2_BP);
    }
    else
    {
        ad9518_write(d, AD9518_PECLDIV_1(idx), div_hi_lo(ratio));
        ad9518_write(d, AD9518_PECLDIV_2(idx), 0x00);
    }
    /* Use the divider path, not the direct-from-VCO mux. */
    ad9518_write(d, AD9518_PECLDIV_3(idx), 0x00);
}

int ad9518_apply(ad9518_dev_t *d, const ad9518_plan_t *p, uint32_t ref_hz)
{
    if (!d || !p)
        return -1;

    Df(d, "[apply] REF %lu Hz, VCO %lu Hz, OUT %lu Hz\r\n",
       (unsigned long)ref_hz, (unsigned long)p->vco_hz, (unsigned long)p->out_hz);
    Df(d, "        R=%u A=%u B=%u P=%u  VCOdiv=%u  CHdiv=%u\r\n",
       p->r_counter, p->a_counter, p->b_counter, p->prescaler,
       p->vco_divider, p->chan_divider);

    /* --- PLL counters ---
     *
     * 0x010 is the PFD/charge-pump register. Bits[1:0] are the PLL power
     * control: 00 = normal operation, 01 = asynchronous power-down.
     *
     * Use 0x7C, not 0x7D. The 0x7D value that appears in the ADI Linux
     * driver's default register table has bits[1:0] = 01, which leaves the
     * PLL powered down - that table is a starting point the driver then
     * overwrites from a .stp firmware file, not a runnable configuration.
     * With the PLL down, VCO calibration still completes and every register
     * reads back correctly, but lock detect never asserts.
     */
    ad9518_write(d, AD9518_PFD_CP, 0x7C); /* PLL on, CP normal, PFD pol + */
    ad9518_write(d, AD9518_RCNT_L, (uint8_t)(p->r_counter & 0xFF));
    ad9518_write(d, AD9518_RCNT_H, (uint8_t)(p->r_counter >> 8));
    ad9518_write(d, AD9518_ACNT, (uint8_t)(p->a_counter & 0x3F));
    ad9518_write(d, AD9518_BCNT_L, (uint8_t)(p->b_counter & 0xFF));
    ad9518_write(d, AD9518_BCNT_H, (uint8_t)((p->b_counter >> 8) & 0x1F));
    ad9518_write(d, AD9518_PLL1, (uint8_t)(p->prescaler_idx & AD9518_PLL1_PRESCALER_MASK));
    ad9518_write(d, AD9518_PLL2, 0x00);

    /* --- Reference input enable ---
     *
     * The datasheet is explicit that all PLL reference inputs are OFF by
     * default and that either a differential or a single-ended reference
     * must be specifically enabled. Without this the PFD sees nothing and
     * the PLL can never lock, even though every other register is correct.
     *
     * PLL7 (0x01C) controls the reference input buffers and selection.
     * AD9518_REF_INPUT_CFG is defined in ad9518.h so it can be adjusted for
     * a differential versus single-ended reference without touching this
     * function. Verify the bit assignment against Table 44 of your
     * datasheet revision - the reference control bits moved between revs.
     */
    ad9518_write(d, AD9518_PLL7, AD9518_REF_INPUT_CFG);

    /* Single-ended AC-coupled references can chatter when the input stops
     * toggling; 0x018[7] shifts the bias point down ~140 mV to prevent it.
     * The cal-divider bits in the low nibble are preserved below. */

    /* --- Routing: internal VCO -> VCO divider -> channel dividers --- */
    ad9518_write(d, AD9518_VCO_DIVIDER, (uint8_t)(p->vco_divider - 2));
    ad9518_write(d, AD9518_INPUT_CLKS, AD9518_VCO_DIVIDER_SEL);

    /* --- Channel dividers 0 and 1 feed OUT0..OUT3 --- */
    set_chan_divider(d, 0, p->chan_divider);
    set_chan_divider(d, 1, p->chan_divider);

    /* --- Outputs ---
     *
     * The low two bits of each LVPECL output register are the power-down
     * field: 0b00 enables the output, and setting them powers it down.
     * AD9518_OUTPUT_MASK selects which of OUT0..OUT5 come up enabled;
     * everything else is explicitly powered down rather than left at its
     * reset value. Disabling unused outputs cuts power and stops unused
     * pairs radiating into the ones you care about.
     */
    for (int i = 0; i < AD9518_NUM_OUTPUTS; i++)
    {
        int on = (AD9518_OUTPUT_MASK >> i) & 1;
        ad9518_write(d, AD9518_OUT_LVPECL(i), on ? 0x08 : 0x0B);
    }

    ad9518_write(d, AD9518_POWDOWN_SYNC, 0x00);

    /* Cal divider select lives in PLL3[2:1]; 0x06 selects the ADI default. */
    const uint8_t pll3 = 0x06;

    /* Commit with the calibration bit cleared, then set it and commit again.
     * The rising edge of VCO_CAL is what starts a calibration. */
    ad9518_write(d, AD9518_PLL3, (uint8_t)(pll3 & ~AD9518_PLL3_VCO_CAL));
    ad9518_transfer(d);

    ad9518_write(d, AD9518_PLL3, (uint8_t)(pll3 | AD9518_PLL3_VCO_CAL));
    ad9518_transfer(d);

    /* tcal = 4400 * R * cal_div / f_ref  (Linux driver's formula). */
    uint32_t cal_div = 2u << ((pll3 >> 1) & 0x3);
    uint32_t cal_ms = (4400u * p->r_counter * cal_div) / (ref_hz / 1000u);
    Df(d, "[apply] VCO cal, waiting %lu ms\r\n", (unsigned long)(cal_ms + 5));
    d->delay_ms(cal_ms + 5);

    /*
     * Soft sync.
     *
     * The channel dividers hold their outputs in a static preset state
     * until a sync releases them. A sync happens automatically at the end
     * of VCO calibration, but issuing one explicitly is harmless and covers
     * the case where the outputs were left static by an earlier event.
     *
     * Per the datasheet, sync is executed by setting and then resetting the
     * soft sync bit, and both the set and the reset need an update-all
     * operation to take effect.
     */
    D(d, "[apply] soft sync to release output dividers\r\n");
    ad9518_write(d, AD9518_POWDOWN_SYNC, 0x01); /* 0x230[0] = 1 */
    ad9518_transfer(d);
    d->delay_ms(1);
    ad9518_write(d, AD9518_POWDOWN_SYNC, 0x00); /* 0x230[0] = 0 */
    ad9518_transfer(d);
    d->delay_ms(1);

    return 0;
}

int ad9518_set_frequency(ad9518_dev_t *d, uint32_t ref_hz, uint32_t target_hz,
                         uint32_t vco_min_hz, uint32_t vco_max_hz,
                         ad9518_plan_t *out_plan)
{
    ad9518_plan_t p;
    int r = ad9518_plan(ref_hz, target_hz, vco_min_hz, vco_max_hz, &p);

    if (r < 0)
    {
        Df(d, "[plan] no divider combination reaches %lu Hz\r\n",
           (unsigned long)target_hz);
        return -1;
    }
    if (r == 1)
    {
        Df(d, "[plan] exact %lu Hz not reachable; nearest is %lu Hz\r\n",
           (unsigned long)target_hz, (unsigned long)p.out_hz);
    }

    if (out_plan)
        *out_plan = p;
    return ad9518_apply(d, &p, ref_hz);
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

/*
 * Brute-force the reference input configuration until the PLL locks.
 *
 * Register 0x01C selects and powers the reference input buffers, and its
 * bit assignments were revised between datasheet revisions, so a value
 * copied from an application note or an older driver may be wrong for the
 * silicon actually fitted. Rather than trust one value, try them and let
 * the digital lock detect decide.
 *
 * Register 0x018[7] is also swept: for a single-ended AC-coupled reference
 * it shifts the input bias point down ~140 mV to stop the input buffer
 * chattering when the reference is slow or missing. It matters for some
 * reference drivers and not others.
 *
 * Assumes the PLL counters, routing and channel dividers are already
 * programmed - call this after ad9518_apply(). Returns 0 on lock with the
 * working values left in place, -1 if nothing locked.
 */
int ad9518_ref_config_sweep(ad9518_dev_t *d, uint32_t settle_ms)
{
    const uint8_t cal_base = 0x06; /* cal divider bits, cal bit cleared */

    D(d, "[refsweep] searching for a reference config that locks\r\n");

    for (int offs = 0; offs < 2; offs++)
    {
        uint8_t bias = offs ? 0x80 : 0x00;

        for (uint8_t cfg = 0x00; cfg <= 0x3F; cfg++)
        {
            ad9518_write(d, AD9518_PLL7, cfg);

            /* Re-run VCO calibration for this reference setting. */
            ad9518_write(d, AD9518_PLL3, (uint8_t)(bias | (cal_base & ~AD9518_PLL3_VCO_CAL)));
            ad9518_transfer(d);
            ad9518_write(d, AD9518_PLL3, (uint8_t)(bias | cal_base | AD9518_PLL3_VCO_CAL));
            ad9518_transfer(d);

            d->delay_ms(settle_ms);

            uint8_t st = 0;
            ad9518_read(d, AD9518_PLL_RB, &st);

            if (st & 0x01)
            {
                Df(d, "  LOCKED with 0x01C=0x%02X, 0x018[7]=%d (0x01F=0x%02X)\r\n",
                   cfg, offs, st);
                return 0;
            }

            /* Report anything that at least changes the status byte, so a
             * partially-working setting is visible even without lock. */
            if (st != 0x0E)
                Df(d, "  0x01C=0x%02X bias=%d -> 0x01F=0x%02X\r\n", cfg, offs, st);
        }
    }

    D(d, "[refsweep] no reference configuration produced lock\r\n");
    D(d, "  This points upstream of the register map. Verify:\r\n");
    D(d, "    - 25 MHz actually present at the REFIN pin\r\n");
    D(d, "    - REF_SEL pin low (it has a 30k pull-down; high selects REF2)\r\n");
    D(d, "    - loop filter components populated between CP and VCO tune\r\n");
    return -1;
}

/*
 * Enable or disable a single LVPECL output at runtime (OUT0..OUT5).
 * Commits immediately. Returns 0 on success.
 */
int ad9518_output_enable(ad9518_dev_t *d, uint8_t out, int on)
{
    if (out >= AD9518_NUM_OUTPUTS)
        return -1;
    int r = ad9518_write(d, AD9518_OUT_LVPECL(out), on ? 0x08 : 0x0B);
    if (r < 0)
        return r;
    return ad9518_transfer(d);
}

int ad9518_locked(ad9518_dev_t *d)
{
    uint8_t st = 0;
    if (ad9518_read(d, AD9518_PLL_RB, &st) < 0)
        return -1;

    /* An unresponsive or floating bus reads as all ones or all zeros.
     * Neither is a credible status byte, so do not call it a lock. */
    if (st == 0xFF || st == 0x00)
    {
        Df(d, "PLL_RB = 0x%02X - no valid readback\r\n", st);
        return -2;
    }

    int locked = (st & 0x01) ? 1 : 0;
    Df(d, "PLL_RB = 0x%02X -> %s\r\n", st, locked ? "LOCKED" : "unlocked");
    return locked;
}

void ad9518_dump(ad9518_dev_t *d)
{
    static const uint16_t regs[] = {
        AD9518_SERCONF,
        AD9518_PARTID,
        AD9518_PFD_CP,
        AD9518_RCNT_L,
        AD9518_RCNT_H,
        AD9518_ACNT,
        AD9518_BCNT_L,
        AD9518_BCNT_H,
        AD9518_PLL1,
        AD9518_PLL3,
        AD9518_PLL6,
        AD9518_PLL7,
        AD9518_PLL8,
        AD9518_PLL_RB,
        AD9518_OUT_LVPECL(0),
        AD9518_OUT_LVPECL(1),
        AD9518_OUT_LVPECL(2),
        AD9518_OUT_LVPECL(3),
        AD9518_PECLDIV_1(0),
        AD9518_PECLDIV_2(0),
        AD9518_PECLDIV_3(0),
        AD9518_PECLDIV_1(1),
        AD9518_PECLDIV_2(1),
        AD9518_PECLDIV_3(1),
        AD9518_VCO_DIVIDER,
        AD9518_INPUT_CLKS,
        AD9518_POWDOWN_SYNC,
    };

    D(d, "[dump]\r\n");
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
    {
        uint8_t v = 0;
        ad9518_read(d, regs[i], &v);
        Df(d, "  0x%03X = 0x%02X\r\n", regs[i], v);
    }
}