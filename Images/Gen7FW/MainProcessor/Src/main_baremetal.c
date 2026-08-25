/* Bare-metal MainProcessor bring-up test.
 * No HAL, no CubeMX init — just toggle COPROCESSOR_WAKEUP (PD9) forever.
 */

#include "stm32h7xx.h"

void delay(volatile uint32_t count)
{
    while (count--) {}
}

int main(void)
{
    /* Enable GPIOD clock. */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;

    /* PD9 as general purpose output. */
    GPIOD->MODER &= ~(3UL << (9U * 2U));
    GPIOD->MODER |=  (1UL << (9U * 2U));

    while (1)
    {
        GPIOD->BSRR = (1UL << 9U);          /* PD9 high */
        delay(1000000);
        GPIOD->BSRR = (1UL << (9U + 16U));  /* PD9 low */
        delay(1000000);
    }
}
