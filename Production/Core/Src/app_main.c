#include "main.h"
#include "stm32f1xx_hal.h"
#include "ad9518.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

extern ad9518_dev_t ad9518;
extern UART_HandleTypeDef huart1; /* enable USART1 in CubeMX, 115200 8N1 */

#define AD1_CLK_HZ 250000000UL
#define AD2_CLK_HZ 250000000UL
#define SPARE_CLK_HZ 100000000UL

/* Send a string over UART */
static void uart_print(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), HAL_MAX_DELAY);
}

/* Format + send, like printf but self-contained */
static void uart_printf(const char *fmt, ...)
{
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0)
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, HAL_MAX_DELAY);
}

int app_main(void)
{
    HAL_GPIO_WritePin(CS_CLK_GPIO_Port, CS_CLK_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(CS_ADC_GPIO_Port, CS_ADC_Pin, GPIO_PIN_SET);

    uart_print("\r\n--- AD9518 bring-up ---\r\n");

    /* Init first (write-only path) */
    int init_ret = ad9518_init(&ad9518, 0);
    uart_printf("init_ret = %d\r\n", init_ret);

    /* Write/read a scratch register to test both directions independently */
    ad9518_reg_write(&ad9518, AD9518_BCNT_L, 0x05);
    uint8_t chk = 0;
    ad9518_reg_read(&ad9518, AD9518_BCNT_L, &chk);
    uart_printf("wrote 0x05 to BCNT_L, read 0x%02X\r\n", chk);

    /* Read part ID */
    uint8_t part_id = 0;
    int rd_ret = ad9518_read_part_id(&ad9518, &part_id);
    uart_printf("read_ret = %d, part_id = 0x%02X (expect 0x63)\r\n",
                rd_ret, part_id);

    int comms_ok = (part_id == 0x63);

    if (comms_ok)
    {
        uint32_t g0 = 0, g1 = 0, g2 = 0;
        ad9518_set_channel_freq(&ad9518, 0, AD1_CLK_HZ, &g0);
        ad9518_set_channel_freq(&ad9518, 1, AD2_CLK_HZ, &g1);
        ad9518_set_channel_freq(&ad9518, 2, SPARE_CLK_HZ, &g2);
        uart_printf("freqs: ch0=%lu ch1=%lu ch2=%lu Hz\r\n",
                    (unsigned long)g0, (unsigned long)g1, (unsigned long)g2);

        ad9518_channel_enable(&ad9518, 0, 1);
        ad9518_channel_enable(&ad9518, 1, 1);
        ad9518_channel_enable(&ad9518, 2, 0);
        uart_print("outputs enabled: ch0, ch1 on; ch2 off\r\n");
    }
    else
    {
        uart_print("comms failed - outputs not configured\r\n");
    }

    while (1)
    {
        HAL_GPIO_TogglePin(USR1_GPIO_Port, USR1_Pin);
        HAL_Delay(comms_ok ? 1000 : 200);
    }
    return 0;
}