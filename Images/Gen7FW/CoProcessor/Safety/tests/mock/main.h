#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

typedef struct { uint32_t BSRR, OTYPER, MODER; } GPIO_TypeDef;
typedef struct { uint32_t AHB2ENR; } RCC_TypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
extern GPIO_TypeDef test_gpio_c, test_gpio_b;
extern RCC_TypeDef test_rcc;
#define GPIOC (&test_gpio_c)
#define RCC (&test_rcc)
#define GPIO_PIN_13 (1u << 13)
#define GATE_DRIVE_PWR_ENABLE_GPIO_Port GPIOC
#define GATE_DRIVE_PWR_ENABLE_Pin (1u << 7)
#define GATE_DRIVE_PWR2_FEEDBACK_GPIO_Port GPIOC
#define GATE_DRIVE_PWR2_FEEDBACK_Pin (1u << 15)
#define GATE_DRIVE_PWR1_FEEDBACK_GPIO_Port GPIOC
#define GATE_DRIVE_PWR1_FEEDBACK_Pin (1u << 14)
#define GATE_DRIVER_FAULT_IN_GPIO_Port (&test_gpio_b)
#define GATE_DRIVER_FAULT_IN_Pin (1u << 10)
#define INTERMCU_SYNC_LINE_GPIO_Port (&test_gpio_b)
#define INTERMCU_SYNC_LINE_Pin (1u << 9)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
uint32_t HAL_GetTick(void);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level);

#endif
