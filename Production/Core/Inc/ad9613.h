/*
 * AD9613 dual 12-bit ADC driver.
 *
 * Register map from the AD9613 data sheet (Rev. E), Table 14. The serial
 * port is the standard ADI high-speed-ADC 3-wire SPI described in AN-877.
 *
 * NOTE ON LEVELS: the AD9613 serial pins are referenced to DRVDD (1.8 V
 * nominal) and its absolute maximum is DRVDD + 0.3 V. A 3.3 V master must
 * go through a level translator - on this board that is the
 * SN74AVCH4T245 sitting between the STM32 and the ADC.
 */
#ifndef AD9613_H
#define AD9613_H

#include <stdint.h>

/* ---- Chip configuration ---- */
#define AD9613_SPI_CONFIG 0x00 /* mirrored nibbles */
#define AD9613_CHIP_ID 0x01    /* reads 0x83 */
#define AD9613_CHIP_GRADE 0x02 /* bits[5:4] speed grade */

#define AD9613_SOFT_RESET 0x24 /* bits 5 and 2, mirrored pair */
#define AD9613_SPI_CONFIG_DEF 0x18

#define AD9613_ID_EXPECTED 0x83

/* Speed grade, register 0x02 bits[5:4] */
#define AD9613_GRADE_MASK 0x30
#define AD9613_GRADE_250 0x00
#define AD9613_GRADE_210 0x10
#define AD9613_GRADE_170 0x30

/* ---- Channel index and transfer ---- */
#define AD9613_CHAN_INDEX 0x05
#define AD9613_CHAN_A 0x01
#define AD9613_CHAN_B 0x02
#define AD9613_CHAN_BOTH 0x03 /* default */

#define AD9613_TRANSFER 0xFF
#define AD9613_TRANSFER_NOW 0x01

/* ---- ADC functions ---- */
#define AD9613_POWER_MODES 0x08
#define AD9613_GLOBAL_CLOCK 0x09 /* bit0 duty cycle stabilizer */
#define AD9613_CLOCK_DIVIDE 0x0B /* bits[2:0] divide ratio */
#define AD9613_TEST_MODE 0x0D
#define AD9613_OFFSET_ADJUST 0x10
#define AD9613_OUTPUT_MODE 0x14
#define AD9613_OUTPUT_ADJUST 0x15 /* LVDS drive current */
#define AD9613_CLOCK_PHASE 0x16
#define AD9613_DCO_DELAY 0x17
#define AD9613_INPUT_SPAN 0x18
#define AD9613_SYNC_CONTROL 0x3A

/* Power modes (0x08) */
#define AD9613_PWR_NORMAL 0x00
#define AD9613_PWR_FULL_DOWN 0x01
#define AD9613_PWR_STANDBY 0x02

/* Global clock (0x09) */
#define AD9613_DCS_ENABLE 0x01

/* Clock divide (0x0B) bits[2:0]: 000 = divide by 1 ... 111 = divide by 8 */
#define AD9613_CLKDIV_1 0x00

/* Output mode (0x14) */
#define AD9613_FMT_OFFSET_BIN 0x00
#define AD9613_FMT_TWOS_COMP 0x01 /* default */
#define AD9613_FMT_GRAY 0x02
#define AD9613_OUT_NOT_INVERTED 0x04 /* bit2: 1 = normal, 0 = inverted */
#define AD9613_OUT_DISABLE 0x10      /* bit4: output enable bar */

/* Output adjust (0x15), LVDS drive current bits[3:0] */
#define AD9613_LVDS_3_72MA 0x00
#define AD9613_LVDS_3_5MA 0x01 /* default, ANSI-644 levels */
#define AD9613_LVDS_3_30MA 0x02
#define AD9613_LVDS_2_96MA 0x03
#define AD9613_LVDS_2_0MA 0x07 /* reduced range */

/* Clock phase control (0x16) */
#define AD9613_DCO_INVERT 0x80
#define AD9613_ODD_EVEN_MODE 0x20 /* 0 = interleaved parallel LVDS */

/* Sync control (0x3A) */
#define AD9613_SYNC_MAIN_BUF_EN 0x01
#define AD9613_SYNC_CLKDIV_EN 0x02
#define AD9613_SYNC_NEXT_ONLY 0x04

/* ---- Platform glue ---- */
typedef struct
{
    void (*cs)(int level); /* ADC chip select, 0 = assert */
    void (*sclk)(int level);
    void (*sdio)(int level);
    void (*sdio_dir)(int output);
    int (*sdio_read)(void);
    void (*bit_delay)(void);
    void (*delay_ms)(uint32_t ms);
    void (*dbg)(const char *s);
} ad9613_dev_t;

/* Output data format selection. */
typedef enum
{
    AD9613_FORMAT_TWOS_COMPLEMENT = 0,
    AD9613_FORMAT_OFFSET_BINARY = 1,
} ad9613_format_t;

/* ---- Register access ---- */
int ad9613_write(ad9613_dev_t *d, uint16_t reg, uint8_t val);
int ad9613_read(ad9613_dev_t *d, uint16_t reg, uint8_t *val);
int ad9613_transfer(ad9613_dev_t *d);

/* ---- Identification ---- */
int ad9613_chip_id(ad9613_dev_t *d, uint8_t *id);
/* Returns max sample rate in MSPS for the fitted grade, or 0 if unknown. */
int ad9613_max_msps(ad9613_dev_t *d);

/* ---- Bring-up ---- */
/*
 * Configure for interleaved parallel LVDS output on D0..D11 with DCO,
 * clock divider bypassed so the conversion rate equals the CLK+/CLK- rate.
 *
 * sample_hz is used only to sanity-check against the fitted speed grade and
 * the 40 MSPS lower limit; the actual rate is set by the clock you apply.
 *
 * Returns 0 on success, -1 if the part does not respond or the requested
 * rate is out of range for this grade.
 */
int ad9613_init_lvds(ad9613_dev_t *d, uint32_t sample_hz,
                     ad9613_format_t format);

/* Put a known pattern on the LVDS outputs so the receiver can be verified
 * without a real analog signal. mode uses the 0x0D output test mode codes;
 * 0x0F is a ramp, 0x04 an alternating checkerboard, 0x00 turns it off. */
int ad9613_test_pattern(ad9613_dev_t *d, uint8_t mode);

/* Read back and print the registers this driver programs. */
void ad9613_dump(ad9613_dev_t *d);

#endif /* AD9613_H */