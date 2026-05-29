/*
 * main_blink_d8.c — STM32MP157F-DK2  Cortex-M4
 *
 * Blinks Arduino connector pin D8 = PD14
 * Toggle every 500 ms  →  1 Hz blink visible on LED / logic analyser
 *
 * No RPMsg, no OpenAMP — bare minimum to confirm the pin works.
 *
 * Build:
 *   arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 \
 *                     -mfloat-abi=hard ...
 */

#include "stm32mp1xx_hal.h"

/* ── D8 = PD14 ──────────────────────────────────────────────────────── */
#define BLINK_PORT      GPIOD
#define BLINK_PIN       GPIO_PIN_14
#define BLINK_DELAY_MS  500U     /* toggle every 500 ms = 1 Hz blink */

/* ── GPIO init ──────────────────────────────────────────────────────── */
static void blink_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();

    g.Pin   = BLINK_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;   /* push-pull output */
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BLINK_PORT, &g);

    /* Start LOW */
    HAL_GPIO_WritePin(BLINK_PORT, BLINK_PIN, GPIO_PIN_RESET);
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void)
{
    HAL_Init();

    blink_gpio_init();

    for (;;) {
        HAL_GPIO_TogglePin(BLINK_PORT, BLINK_PIN);
        HAL_Delay(BLINK_DELAY_MS);
    }
}