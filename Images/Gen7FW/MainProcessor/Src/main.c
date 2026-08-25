/* Bare-metal MainProcessor bring-up test.
 * No HAL, no CubeMX init — toggle COPROCESSOR_WAKEUP (PD9) forever
 * and send a UART heartbeat on USART3 (PB10/PB11).
 */

#include "stm32h7xx.h"

void delay(volatile uint32_t count)
{
    while (count--) {}
}

void uart_send(uint8_t byte)
{
    while (!(USART3->ISR & USART_ISR_TXE_TXFNF)) {}
    USART3->TDR = byte;
}

int main(void)
{
    /* Enable GPIOD and GPIOB clocks. */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN | RCC_AHB4ENR_GPIOBEN;

    /* PD9 as general purpose output. */
    GPIOD->MODER &= ~(3UL << (9U * 2U));
    GPIOD->MODER |=  (1UL << (9U * 2U));

    /* PB10/PB11 as USART3 TX/RX (AF7). */
    GPIOB->MODER &= ~(3UL << (10U * 2U) | (3UL << (11U * 2U)));
    GPIOB->MODER |=  (2UL << (10U * 2U) | (2UL << (11U * 2U)));
    GPIOB->AFR[1] &= ~(0xFUL << ((10U - 8U) * 4U) | (0xFUL << ((11U - 8U) * 4U)));
    GPIOB->AFR[1] |=  (7UL << ((10U - 8U) * 4U) | (7UL << ((11U - 8U) * 4U)));

    /* Enable USART3 clock. */
    RCC->APB1LENR |= RCC_APB1LENR_USART3EN;

    /* USART3: 115200 8E1 (9-bit word + even parity), assuming 64MHz HSI clock. */
    USART3->BRR = 64000000 / 115200;
    USART3->CR1 = USART_CR1_M0 | USART_CR1_PCE | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

    while (1)
    {
        GPIOD->BSRR = (1UL << 9U);          /* PD9 high */
        uart_send('H');
        uart_send('7');
        uart_send(' ');
        uart_send('A');
        uart_send('L');
        uart_send('I');
        uart_send('V');
        uart_send('E');
        uart_send('\r');
        uart_send('\n');
        delay(1000000);

        GPIOD->BSRR = (1UL << (9U + 16U));  /* PD9 low */
        delay(1000000);
    }
}
