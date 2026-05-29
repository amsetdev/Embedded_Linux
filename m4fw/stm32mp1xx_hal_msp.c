/*
 * stm32mp1xx_hal_msp_blink.c
 * Minimal MSP stubs for blink-only build.
 * GPIO clock is handled inside blink_gpio_init() in main.
 */

#include "stm32mp1xx_hal.h"

void HAL_UART_MspInit(UART_HandleTypeDef *h)   { (void)h; }
void HAL_UART_MspDeInit(UART_HandleTypeDef *h) { (void)h; }
void HAL_IPCC_MspInit(IPCC_HandleTypeDef *h)   { (void)h; }
void HAL_IPCC_MspDeInit(IPCC_HandleTypeDef *h) { (void)h; }