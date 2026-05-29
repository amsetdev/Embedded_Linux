#include "stm32mp1xx_hal.h"

int main(void)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* PE8 = D1 output */
    GPIOE->MODER  = (GPIOE->MODER  & ~(3U << (8 * 2))) | (1U << (8 * 2));
    GPIOE->OTYPER &= ~(1U << 8);
    GPIOE->PUPDR  &= ~(3U << (8 * 2));

    for (;;) {
        GPIOE->BSRR = (1U << 8);           /* D1 HIGH */
        for (volatile uint32_t i = 0; i < 500000U; i++);
        GPIOE->BSRR = (1U << (8 + 16));    /* D1 LOW  */
        for (volatile uint32_t i = 0; i < 500000U; i++);
    }
}
