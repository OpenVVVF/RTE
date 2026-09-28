#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

typedef struct { unsigned id; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef struct { uint32_t EGR; } TIM_TypeDef;

extern GPIO_TypeDef test_gpio_d;
extern GPIO_TypeDef test_gpio_c;
extern TIM_TypeDef test_tim1;
#define COPROCESSOR_SYNC_GPIO_Port (&test_gpio_d)
#define COPROCESSOR_SYNC_Pin (1u << 8)
#define COPROCESSOR_WAKEUP_GPIO_Port (&test_gpio_d)
#define COPROCESSOR_WAKEUP_Pin (1u << 9)
#define GATE_DRIVER_POWER_ENABLE_GPIO_Port (&test_gpio_c)
#define GATE_DRIVER_POWER_ENABLE_Pin (1u << 7)
#define GATE_DRIVER_RESET_GPIO_Port (&test_gpio_d)
#define GATE_DRIVER_RESET_Pin (1u << 5)
#define TIM1 (&test_tim1)
#define TIM_EGR_BG (1u << 7)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)

uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);

#endif
