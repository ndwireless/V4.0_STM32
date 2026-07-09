#include "ad9518.h"
#include "stm32f1xx_hal.h"
#include "main.h"

extern SPI_HandleTypeDef hspi1;
#define CS_PORT CS_CLK_GPIO_Port
#define CS_PIN CS_CLK_Pin

static inline void cs_low(void) { HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET); }

/* In half-duplex master (BIDIMODE=1), BIDIOE=1 => output (transmit),
 * BIDIOE=0 => input (receive). The SPI must be disabled while changing it. */
static inline void spi_dir_tx(void)
{
    __HAL_SPI_DISABLE(&hspi1);
    hspi1.Instance->CR1 |= SPI_CR1_BIDIOE; /* drive the data line */
}
static inline void spi_dir_rx(void)
{
    __HAL_SPI_DISABLE(&hspi1);
    hspi1.Instance->CR1 &= ~SPI_CR1_BIDIOE; /* release: chip drives line */
}

/* ---- WRITE: TX only ----------------------------------------------------- */
static int stm32_spi_write(const uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef s;
    cs_low();
    spi_dir_tx();
    s = HAL_SPI_Transmit(&hspi1, (uint8_t *)buf, len, HAL_MAX_DELAY);
    cs_high();
    return (s == HAL_OK) ? 0 : -1;
}

/* ---- READ: TX instruction, turn line around, RX data -------------------- */
static int stm32_spi_read(const uint8_t *tx, uint16_t txlen,
                          uint8_t *rx, uint16_t rxlen)
{
    HAL_StatusTypeDef s;
    cs_low();

    /* Phase 1: send instruction, host drives SDIO */
    spi_dir_tx();
    s = HAL_SPI_Transmit(&hspi1, (uint8_t *)tx, txlen, HAL_MAX_DELAY);
    if (s != HAL_OK)
    {
        cs_high();
        return -1;
    }

    /* Phase 2: turn line around; the chip drives SDIO with readback.
     * In half-duplex master RX the clock runs continuously once enabled,
     * so HAL_SPI_Receive handles the fixed count, but we make sure SPI is
     * left disabled afterward to stop clocking. */
    spi_dir_rx();
    s = HAL_SPI_Receive(&hspi1, rx, rxlen, HAL_MAX_DELAY);
    __HAL_SPI_DISABLE(&hspi1); /* stop the continuous clock immediately */

    cs_high();

    /* Restore to TX (idle) direction for the next write */
    spi_dir_tx();

    return (s == HAL_OK) ? 0 : -1;
}

static void stm32_delay_ms(uint32_t ms) { HAL_Delay(ms); }

/* Expose a ready-made device struct */
ad9518_dev_t ad9518 = {
    .spi_write = stm32_spi_write,
    .spi_read = stm32_spi_read,
    .delay_ms = stm32_delay_ms,
};