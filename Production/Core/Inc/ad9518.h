/*
 * Portable AD9518 driver. No STM32 headers here - all hardware access goes
 * through the ad9518_dev_t function pointers, which the port layer fills in.
 */
#ifndef AD9518_H
#define AD9518_H

#include <stdint.h>
#include "ad9518_regs.h"

/* How the serial port is wired. */
typedef enum
{
    AD9518_WIRE_3 = 0, /* bidirectional SDIO, no SDO pin */
    AD9518_WIRE_4 = 1, /* separate SDIO (in) and SDO (out) */
} ad9518_wire_t;

/* Which SCLK phase the master samples read data on. The part drives read
 * data on the falling edge, so sampling while SCLK is high is correct; the
 * alternative exists only for the bus auto-probe. */
typedef enum
{
    AD9518_SAMPLE_SCLK_HIGH = 0, /* datasheet-correct default */
    AD9518_SAMPLE_SCLK_LOW = 1,
} ad9518_sample_t;

/* Instruction word length. The part supports a 1-byte short instruction
 * (7-bit address) and a 2-byte long instruction (12-bit address). Which one
 * is active at power-up is what SERCONF's LONG_INSTR bit controls - but that
 * write must itself be correctly framed, so if the assumed length is wrong
 * the mode can never be set. The autoprobe sweeps both. */
typedef enum
{
    AD9518_INSTR_LONG = 0,  /* 2-byte instruction, 12-bit address */
    AD9518_INSTR_SHORT = 1, /* 1-byte instruction, 7-bit address */
} ad9518_instr_t;

/*
 * Reference input configuration, written to PLL7 (0x01C).
 *
 * All PLL reference inputs are powered down at reset, so one must be
 * explicitly enabled or the PLL will never lock.
 *
 * This board has a single-ended 25 MHz reference on REF1, so only the REF1
 * buffer is enabled. Leaving the REF2 buffer powered as well (0x06) invites
 * the automatic switchover logic to hand the PLL over to a REF2 input that
 * has no clock on it.
 *
 * If this value does not lock, run ad9518_ref_config_sweep() - the bit
 * assignments in 0x01C[5:1] were revised between datasheet revisions, so
 * the correct value depends on which silicon is fitted.
 */
#ifndef AD9518_REF_INPUT_CFG
#define AD9518_REF_INPUT_CFG 0x02
#endif

/*
 * Which LVPECL outputs come up enabled, one bit per output:
 *   bit0 = OUT0, bit1 = OUT1, ... bit5 = OUT5
 *
 * Outputs are paired onto shared dividers: OUT0/OUT1 use divider 0,
 * OUT2/OUT3 use divider 1, OUT4/OUT5 use divider 2. Enabling an output
 * whose divider was never programmed will not produce a useful clock.
 *
 * Default: OUT0 only.
 */
#ifndef AD9518_OUTPUT_MASK
#define AD9518_OUTPUT_MASK 0x01
#endif

/* Port layer. All function pointers required except sdo_read and dbg. */
typedef struct
{
    void (*cs)(int level); /* chip select: 0 = assert (low) */
    void (*sclk)(int level);
    void (*sdio)(int level);      /* drive SDIO while it is an output */
    void (*sdio_dir)(int output); /* 1 = SDIO output, 0 = SDIO input */
    int (*sdio_read)(void);
    int (*sdo_read)(void);   /* 4-wire only; NULL if no SDO pin */
    void (*bit_delay)(void); /* half-bit-period busy wait */
    void (*delay_ms)(uint32_t ms);
    void (*dbg)(const char *s); /* UART debug sink, or NULL */

    /* Runtime bus settings; ad9518_bus_autoprobe() may change these. */
    ad9518_wire_t wire;
    ad9518_sample_t sample;
    ad9518_instr_t instr;
} ad9518_dev_t;

/* Computed divider plan for a requested output frequency. */
typedef struct
{
    uint16_t r_counter;
    uint8_t a_counter;
    uint16_t b_counter;
    uint8_t prescaler_idx; /* index into the PLL1 prescaler table */
    uint8_t prescaler;     /* resolved P value */
    uint8_t vco_divider;   /* 2..6 */
    uint8_t chan_divider;  /* 1..32 */
    uint32_t vco_hz;
    uint32_t out_hz; /* frequency actually achievable */
} ad9518_plan_t;

/* ---- Register access ---- */
int ad9518_write(ad9518_dev_t *d, uint16_t reg, uint8_t val);
int ad9518_read(ad9518_dev_t *d, uint16_t reg, uint8_t *val);
int ad9518_transfer(ad9518_dev_t *d); /* commit buffered registers */

/* ---- Bus bring-up ---- */
int ad9518_soft_reset(ad9518_dev_t *d);
int ad9518_part_id(ad9518_dev_t *d, uint8_t *id);
int ad9518_part_id_valid(uint8_t id);

/*
 * Sweep wire mode x sample phase looking for a configuration that returns a
 * valid part ID. On success d->wire and d->sample are left set to the working
 * combination and 0 is returned. Returns -1 if nothing worked.
 */
int ad9518_bus_autoprobe(ad9518_dev_t *d);

/* Write a scratch pattern and read it back. 0 if the bus round-trips. */
int ad9518_bus_selftest(ad9518_dev_t *d);

/* Determine whether anything is driving SDIO during the read phase, and
 * report what the result implies. Run this when reads return all 0xFF. */
void ad9518_bus_diagnose(ad9518_dev_t *d);

/* ---- Configuration ---- */
/*
 * Compute a divider plan. vco_min_hz/vco_max_hz bound your part grade's VCO
 * tuning range. Returns 0 exact, 1 nearest achievable, -1 no plan fits.
 */
int ad9518_plan(uint32_t ref_hz, uint32_t target_hz,
                uint32_t vco_min_hz, uint32_t vco_max_hz,
                ad9518_plan_t *plan);

/* Program PLL, route VCO -> VCO divider -> channel dividers, set channel
 * dividers 0 and 1, enable OUT0..OUT3, run VCO calibration. */
int ad9518_apply(ad9518_dev_t *d, const ad9518_plan_t *plan, uint32_t ref_hz);

/* Convenience: plan + apply. */
int ad9518_set_frequency(ad9518_dev_t *d, uint32_t ref_hz, uint32_t target_hz,
                         uint32_t vco_min_hz, uint32_t vco_max_hz,
                         ad9518_plan_t *out_plan);

/* ---- Status ---- */
/* Enable or disable one LVPECL output (OUT0..OUT5) at runtime. */
int ad9518_output_enable(ad9518_dev_t *d, uint8_t out, int on);

/* 1 locked, 0 unlocked, -1 bus error, -2 implausible readback (all 0s/1s). */
int ad9518_locked(ad9518_dev_t *d);

/*
 * Sweep register 0x01C (reference input select/power) and 0x018[7] (single
 * ended input bias offset) until the PLL locks. Call after ad9518_apply()
 * when the configuration is verified correct but lock does not assert.
 * settle_ms is the wait per candidate; 30 is a reasonable starting point.
 * Returns 0 on lock with the working values left programmed, -1 otherwise.
 */
int ad9518_ref_config_sweep(ad9518_dev_t *d, uint32_t settle_ms);

/* Read back and print the registers the driver programmed. */
void ad9518_dump(ad9518_dev_t *d);

#endif /* AD9518_H */