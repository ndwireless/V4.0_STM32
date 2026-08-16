#include "app_main.h"
#include "ad9518.h"
#include "ad9518_port_stm32.h"
#include "ad9613.h"
#include "stm32f1xx_hal.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;

/* ---- Board parameters. Change these to match your hardware. ---- */
#define REF_HZ 25000000UL     /* REF1 input frequency */
#define TARGET_HZ 175000000UL /* desired output on OUT0..OUT3 */

/*
 * VCO tuning range. This board carries an AD9518-3, whose on-chip VCO tunes
 * from 1.75 GHz to 2.25 GHz. The planner only searches inside this window.
 *
 * With a 25 MHz reference the chosen plan is N=84 -> VCO 2100 MHz, VCO
 * divider /2 -> 1050 MHz, channel divider /6 -> 175.000 MHz exactly. 2100
 * MHz is comfortably inside the -3's range.
 *
 * Other grades, if the board is ever repopulated:
 *   AD9518-0  2.55 - 2.95 GHz
 *   AD9518-1  2.30 - 2.65 GHz (approx; confirm)
 *   AD9518-3  1.75 - 2.25 GHz
 */
#define VCO_MIN_HZ 1750000000UL
#define VCO_MAX_HZ 2250000000UL

/*
 * Set to 1 to configure the part without requiring readback to work.
 *
 * Readback is only a diagnostic. If writes reach the AD9518 it will lock and
 * produce output whether or not the readback path works. On a board where
 * SDO is not connected and the part powers up with SDO_ACTIVE set, readback
 * lands on an unconnected pin and SDIO stays idle high - which looks exactly
 * like a dead part even though writes are fine. This mode programs the chip
 * anyway so you can confirm on a scope.
 *
 * Check OUT0/OUT1 for the target frequency, and the LD/STATUS pin for lock.
 */
#define BLIND_CONFIGURE 1

static ad9518_dev_t ad9518;
static ad9613_dev_t ad9613;

static void say(const char *s)
{
    HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

int app_main(void)
{
    HAL_Delay(250);

    ad9518_port_init(&ad9518);

    say("\r\n\r\n================================\r\n");
    say("AD9518 bring-up\r\n");
    say("================================\r\n");

    /* 1. Establish that the serial port works in both directions before
     *    trusting any configuration. */
    uint8_t id = 0;
    ad9518_soft_reset(&ad9518);
    ad9518_part_id(&ad9518, &id);

    if (!ad9518_part_id_valid(id))
    {
        say("part ID implausible; sweeping bus configurations\r\n");
        if (ad9518_bus_autoprobe(&ad9518) < 0)
        {
            say("\r\n");
            ad9518_bus_diagnose(&ad9518);

#if BLIND_CONFIGURE
            say("\r\n--- readback unavailable; configuring blind ---\r\n");
            say("Writes may still be landing. Programming the part anyway.\r\n");
            say("Verify with a scope on OUT0/OUT1 and the LD/STATUS pin.\r\n\r\n");

            /* Force a known-good serial mode for a 3-wire board and go. */
            ad9518.wire = AD9518_WIRE_3;
            ad9518.instr = AD9518_INSTR_LONG;
            ad9518.sample = AD9518_SAMPLE_SCLK_HIGH;
            ad9518_soft_reset(&ad9518);

            ad9518_plan_t bp;
            ad9518_set_frequency(&ad9518, REF_HZ, TARGET_HZ,
                                 VCO_MIN_HZ, VCO_MAX_HZ, &bp);

            say("\r\nConfiguration written. Expected on OUT0..OUT3:\r\n");
            {
                char b[80];
                snprintf(b, sizeof(b), "  %lu Hz  (VCO %lu Hz)\r\n",
                         (unsigned long)bp.out_hz, (unsigned long)bp.vco_hz);
                say(b);
            }
            say("If the scope shows this, the part works and only readback\r\n");
            say("is broken. If there is no output at all, writes are not\r\n");
            say("landing either - check AD9518 supplies and its reset pin.\r\n");

            for (;;)
            {
                HAL_GPIO_TogglePin(USR1_GPIO_Port, USR1_Pin);
                HAL_Delay(500);
            }
#else
            say("\r\nEntering pin walk: probe at the AD9518's own pins and\r\n");
            say("confirm each announced line actually moves there.\r\n");
            ad9518_port_pin_walk(); /* never returns */
#endif
        }
        ad9518_part_id(&ad9518, &id);
    }

    {
        char b[64];
        snprintf(b, sizeof(b), "part ID = 0x%02X\r\n", id);
        say(b);
    }

    /* 2. Prove the bus round-trips data before configuring anything. */
    if (ad9518_bus_selftest(&ad9518) < 0)
        say("WARNING: bus self-test failed; configuration may not stick\r\n");

    /* 3. Configure. */
    ad9518_plan_t plan;
    if (ad9518_set_frequency(&ad9518, REF_HZ, TARGET_HZ,
                             VCO_MIN_HZ, VCO_MAX_HZ, &plan) < 0)
    {
        say("configuration failed\r\n");
    }

    /* 4. Report what the part actually holds. */
    ad9518_dump(&ad9518);

    /* 5. Wait for lock. */
    say("waiting for PLL lock\r\n");
    int locked = 0;
    for (int i = 0; i < 20; i++)
    {
        locked = ad9518_locked(&ad9518);
        if (locked == 1)
            break;
        HAL_Delay(100);
    }

    if (locked == 1)
    {
        say(">>> PLL LOCKED - outputs live <<<\r\n");
    }
    else
    {
        say(">>> not locked with the default reference config <<<\r\n");
        if (ad9518_ref_config_sweep(&ad9518, 30) == 0)
        {
            say(">>> PLL LOCKED after reference sweep <<<\r\n");
            say("Set AD9518_REF_INPUT_CFG in ad9518.h to the value above\r\n");
            say("so future boots lock immediately.\r\n");
            ad9518_dump(&ad9518);
        }
        else
        {
            say(">>> still not locked - see checklist above <<<\r\n");
        }
    }

    /* ---- AD9613 ADC: sample at the clock we just generated ---- */
    if (locked == 1)
    {
        ad9613_port_init(&ad9613);
        if (ad9613_init_lvds(&ad9613, TARGET_HZ,
                             AD9613_FORMAT_OFFSET_BINARY) == 0) // can be AD9613_FORMAT_TWOS_COMPLEMENT
        {
            ad9613_dump(&ad9613);
            say(">>> ADC configured for interleaved LVDS <<<\r\n");
            // Uncomment to drive a known ramp for receiver bring-up:
            // ad9613_test_pattern(&ad9613, 0x0F); // 0x0F for ramp 0x07 for 101010
        }
        else
        {
            say(">>> ADC configuration failed <<<\r\n");
        }
    }
    else
    {
        say("skipping ADC config: no valid sample clock\r\n");
    }

    /* Slow blink = ran to completion. Re-check lock periodically. */
    uint32_t n = 0;
    for (;;)
    {
        HAL_GPIO_TogglePin(USR1_GPIO_Port, USR1_Pin);
        HAL_Delay(500);
        if (++n >= 10)
        {
            n = 0;
            ad9518_locked(&ad9518);
        }
    }
    return 0;
}