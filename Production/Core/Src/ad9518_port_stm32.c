#include "ad9518_port_stm32.h"
#include "ad9613.h"
#include "stm32f1xx_hal.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;

/* ------------------------------------------------------------------ */
/* Pin map. Adjust here if your board differs.                          */
/*                                                                      */
/* These match the SPI1 pins CubeMX assigned, but they are driven as    */
/* plain GPIO - the SPI peripheral is not used.                         */
/* ------------------------------------------------------------------ */
#define SCLK_PORT GPIOA
#define SCLK_PIN GPIO_PIN_5

#define SDIO_PORT GPIOA
#define SDIO_PIN GPIO_PIN_7

#define CS_PORT CS_CLK_GPIO_Port
#define CS_PIN CS_CLK_Pin

/*
 * AD9518 hardware RESET, active low.
 *
 * CubeMX's MX_GPIO_Init drives this pin LOW at startup along with the chip
 * selects, which holds the part in reset indefinitely. It must be released
 * before any serial access or the AD9518 ignores the bus completely while
 * CS, SCLK and SDIO all still look perfectly healthy on a scope.
 */
#define RST_PORT RST_CLK_GPIO_Port
#define RST_PIN RST_CLK_Pin

/*
 * Dedicated SDO pin (4-wire mode). This board is 3-wire: the AD9518's SDO
 * is not connected, so 4-wire must stay disabled. Testing it against a
 * floating input would only produce misleading results.
 */
/* #define AD9518_HAS_SDO */
#ifdef AD9518_HAS_SDO
#define SDO_PORT GPIOA
#define SDO_PIN GPIO_PIN_6
#endif

/* ------------------------------------------------------------------ */

static uint32_t bit_delay_loops = 400;

void ad9518_port_set_bit_delay(uint32_t loops)
{
    bit_delay_loops = loops;
}

static void port_bit_delay(void)
{
    for (volatile uint32_t i = 0; i < bit_delay_loops; i++)
        __NOP();
}

static void port_cs(int level)
{
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void port_sclk(int level)
{
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void port_sdio(int level)
{
    HAL_GPIO_WritePin(SDIO_PORT, SDIO_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void port_sdio_dir(int output)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = SDIO_PIN;
    g.Speed = GPIO_SPEED_FREQ_HIGH;

    if (output)
    {
        g.Mode = GPIO_MODE_OUTPUT_PP;
        g.Pull = GPIO_NOPULL;
    }
    else
    {
        g.Mode = GPIO_MODE_INPUT;
        /* No pull. A pull-up here masks a non-responding part by making a
         * floating line read back as 0xFF, which looks like valid data. */
        g.Pull = GPIO_NOPULL;
    }
    HAL_GPIO_Init(SDIO_PORT, &g);
}

static int port_sdio_read(void)
{
    return HAL_GPIO_ReadPin(SDIO_PORT, SDIO_PIN) == GPIO_PIN_SET;
}

#ifdef AD9518_HAS_SDO
static int port_sdo_read(void)
{
    return HAL_GPIO_ReadPin(SDO_PORT, SDO_PIN) == GPIO_PIN_SET;
}
#endif

static void port_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

static void port_dbg(const char *s)
{
    HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

/*
 * Toggle each serial-port line on its own, slowly, announcing which one is
 * moving. Probe at the AD9518's own pins (not the MCU's) with a meter or
 * scope: the line that is announced should be the line that moves. This is
 * how you confirm the nets actually reach the part.
 *
 * Runs forever; reset the board to stop.
 */
/* ------------------------------------------------------------------ */
/* AD9613 port layer                                                    */
/*                                                                      */
/* The ADC shares SCLK and SDIO with the clock chip and has its own      */
/* chip select. Its serial pins are DRVDD (1.8 V) referenced, so the     */
/* level translator between the STM32 and the ADC must be in circuit.    */
/* ------------------------------------------------------------------ */

static void adc_cs(int level)
{
    HAL_GPIO_WritePin(CS_ADC_GPIO_Port, CS_ADC_Pin,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void ad9613_port_init(ad9613_dev_t *d)
{
    /* Pins are already configured by ad9518_port_init(); only the chip
     * select differs. Make sure the clock chip stays deselected. */
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);
    adc_cs(1);

    d->cs = adc_cs;
    d->sclk = port_sclk;
    d->sdio = port_sdio;
    d->sdio_dir = port_sdio_dir;
    d->sdio_read = port_sdio_read;
    d->bit_delay = port_bit_delay;
    d->delay_ms = port_delay_ms;
    d->dbg = port_dbg;
}

void ad9518_port_hw_reset(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = RST_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RST_PORT, &g);

    HAL_GPIO_WritePin(RST_PORT, RST_PIN, GPIO_PIN_RESET); /* assert */
    HAL_Delay(2);
    HAL_GPIO_WritePin(RST_PORT, RST_PIN, GPIO_PIN_SET); /* release */
    HAL_Delay(10);                                      /* settle */
}

void ad9518_port_pin_walk(void)
{
    struct
    {
        GPIO_TypeDef *port;
        uint16_t pin;
        const char *name;
    } lines[] = {
        {CS_PORT, CS_PIN, "CS   (AD9518 CS pin)"},
        {SCLK_PORT, SCLK_PIN, "SCLK (AD9518 SCLK pin)"},
        {SDIO_PORT, SDIO_PIN, "SDIO (AD9518 SDIO pin)"},
    };

    /* Everything an output, everything idle high except SCLK. */
    port_sdio_dir(1);
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_RESET);
    port_sdio(0);

    for (;;)
    {
        for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        {
            char msg[80];
            snprintf(msg, sizeof(msg),
                     "\r\n--- toggling %s for 10 s ---\r\n", lines[i].name);
            port_dbg(msg);

            for (int t = 0; t < 10; t++)
            {
                HAL_GPIO_WritePin(lines[i].port, lines[i].pin, GPIO_PIN_SET);
                port_dbg("  high\r\n");
                HAL_Delay(500);
                HAL_GPIO_WritePin(lines[i].port, lines[i].pin, GPIO_PIN_RESET);
                port_dbg("  low\r\n");
                HAL_Delay(500);
            }

            /* Restore idle level for this line before moving on. */
            HAL_GPIO_WritePin(lines[i].port, lines[i].pin,
                              (lines[i].pin == SCLK_PIN) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        }
    }
}

void ad9518_port_init(ad9518_dev_t *d)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Idle levels before enabling the pins, so no glitch is presented to
     * the part: CS high (deasserted), SCLK low. */
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(SCLK_PORT, SCLK_PIN, GPIO_PIN_RESET);

    g.Pin = SCLK_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SCLK_PORT, &g);

    g.Pin = CS_PIN;
    HAL_GPIO_Init(CS_PORT, &g);

    port_sdio_dir(1);
    port_sdio(0);

#ifdef AD9518_HAS_SDO
    g.Pin = SDO_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(SDO_PORT, &g);
#endif

    /* Make sure the other chip select on the bus stays deasserted. */
    HAL_GPIO_WritePin(CS_ADC_GPIO_Port, CS_ADC_Pin, GPIO_PIN_SET);

    /* Release the AD9518 from hardware reset. Must happen before any
     * serial transaction; CubeMX leaves this pin asserted low. */
    ad9518_port_hw_reset();

    d->cs = port_cs;
    d->sclk = port_sclk;
    d->sdio = port_sdio;
    d->sdio_dir = port_sdio_dir;
    d->sdio_read = port_sdio_read;
#ifdef AD9518_HAS_SDO
    d->sdo_read = port_sdo_read;
    d->wire = AD9518_WIRE_4;
#else
    d->sdo_read = 0;
    d->wire = AD9518_WIRE_3;
#endif
    d->bit_delay = port_bit_delay;
    d->delay_ms = port_delay_ms;
    d->dbg = port_dbg;
    d->sample = AD9518_SAMPLE_SCLK_LOW; /* sample before the clock pulse */
    d->instr = AD9518_INSTR_LONG;
}