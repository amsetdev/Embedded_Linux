/*
 * Minimal blink test for STM32MP157F-DK2 M4
 * Toggles PE0 (Arduino D4 / RS-485 DE) once per second.
 * Use this to confirm the M4 boots and remoteproc is working
 * before debugging UART7 / RPMsg.
 *
 * Build: rename this file to main.c (backup the original first)
 *   cp main.c main_uart7.c && cp blink_main.c main.c
 *   make clean && make CUBE=/mnt/c/Users/mauli/Desktop/linux/STM32CubeMP1
 */

#include "main.h"

int main(void)
{
    HAL_Init();

    __HAL_RCC_GPIOE_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_0;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &g);

    for (;;) {
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_SET);
        HAL_Delay(500);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_RESET);
        HAL_Delay(500);
    }
}

