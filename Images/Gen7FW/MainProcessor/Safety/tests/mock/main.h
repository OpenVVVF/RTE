#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { unsigned id; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef struct { uint32_t EGR, CCER, BDTR; } TIM_TypeDef;

extern GPIO_TypeDef test_gpio_d;
extern GPIO_TypeDef test_gpio_c;
extern TIM_TypeDef test_tim1;
extern TIM_TypeDef *test_tim1_instance;
#define COPROCESSOR_SYNC_GPIO_Port (&test_gpio_d)
#define COPROCESSOR_SYNC_Pin (1u << 8)
#define COPROCESSOR_WAKEUP_GPIO_Port (&test_gpio_d)
#define COPROCESSOR_WAKEUP_Pin (1u << 9)
#define GATE_DRIVER_POWER_ENABLE_GPIO_Port (&test_gpio_c)
#define GATE_DRIVER_POWER_ENABLE_Pin (1u << 7)
#define GATE_DRIVER_RESET_GPIO_Port (&test_gpio_d)
#define GATE_DRIVER_RESET_Pin (1u << 5)
#define TIM1 test_tim1_instance
#define TIM_EGR_BG (1u << 7)
#define TIM_BDTR_MOE (1u << 15)
#define TIM_CCER_CC1E (1u << 0)
#define TIM_CCER_CC1NE (1u << 2)
#define TIM_CCER_CC2E (1u << 4)
#define TIM_CCER_CC2NE (1u << 6)
#define TIM_CCER_CC3E (1u << 8)
#define TIM_CCER_CC3NE (1u << 10)
#define __get_PRIMASK() (0u)
#define __disable_irq() ((void)0)
#define __set_PRIMASK(value) ((void)(value))
#define __DMB() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)

uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);

#ifdef __cplusplus
}
#endif

#endif
