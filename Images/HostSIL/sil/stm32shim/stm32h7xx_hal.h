/*
 * stm32h7xx_hal.h — SIL shim for the STM32H7 HAL.
 *
 * Minimal host-compiled re-implementation of exactly the HAL surface the
 * Gen6FW application code touches (verified by grepping the compiled TUs).
 * All peripheral state lives in plain host variables; HAL_GetTick/HAL_Delay
 * are backed by the simulated clock (see sil_world.h / sil_rt.h).
 *
 * This file shadows the real HAL header via include-path ordering and must
 * stay compilable as both C and C++ (firmware .c drivers include it).
 */
#ifndef SIL_STM32H7XX_HAL_H
#define SIL_STM32H7XX_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Status / tick
 * ------------------------------------------------------------------------ */
typedef enum {
    HAL_OK      = 0x00U,
    HAL_ERROR   = 0x01U,
    HAL_BUSY    = 0x02U,
    HAL_TIMEOUT = 0x03U
} HAL_StatusTypeDef;

uint32_t HAL_GetTick(void);          /* simulated ms tick (sil clock)       */
void     HAL_Delay(uint32_t Delay);  /* advances sim time; pumps scheduler   */

/* CMSIS intrinsics used by the firmware. */
void __disable_irq(void);
void __enable_irq(void);
void __NOP(void);
void __WFI(void);
void __DMB(void);
unsigned int __get_PRIMASK(void);
void __set_PRIMASK(unsigned int primask);
unsigned int __get_IPSR(void);

#define TIM_EGR_BG 0x0080U

/* FunctionalState like the real HAL (enum, NOT a macro — firmware has
 * class-scoped enum members named ENABLE/DISABLE). */
typedef enum { DISABLE = 0U, ENABLE = 1U } FunctionalState;

/* IRQ priority grouping etc. are irrelevant in SIL. */
void HAL_NVIC_SetPriority(int32_t irqn, uint32_t preempt_priority, uint32_t sub_priority);
void HAL_NVIC_EnableIRQ(int32_t irqn);
void HAL_NVIC_DisableIRQ(int32_t irqn);
void HAL_NVIC_ClearPendingIRQ(int32_t irqn);
void HAL_NVIC_SystemReset(void);

typedef int32_t IRQn_Type;
#define TIM1_UP_IRQn    ((IRQn_Type)25)
#define ADC_IRQn        ((IRQn_Type)18)
#define EXTI1_IRQn      ((IRQn_Type)7)
#define USART3_IRQn     ((IRQn_Type)39)
#define FDCAN1_IT0_IRQn ((IRQn_Type)19)
#define FDCAN2_IT0_IRQn ((IRQn_Type)21)

/* --------------------------------------------------------------------------
 * CMSIS core peripherals (DWT cycle counter drives all microsecond timing)
 * ------------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t CYCCNT;
} SIL_DWT_Type;

typedef struct {
    volatile uint32_t DHCSR;
    volatile uint32_t DCRSR;
    volatile uint32_t DCRDR;
    volatile uint32_t DEMCR;
} SIL_CoreDebug_Type;

extern SIL_DWT_Type       sil_dwt;
extern SIL_CoreDebug_Type sil_coredebug;

#define DWT       ((SIL_DWT_Type*)&sil_dwt)
#define CoreDebug ((SIL_CoreDebug_Type*)&sil_coredebug)

#define CoreDebug_DEMCR_TRCENA_Msk  (1UL << 24)
#define DWT_CTRL_CYCCNTENA_Msk      (1UL << 0)

/* H723 clock tree: PLLN=68 frac .75 on 8 MHz HSE -> 550 MHz SYSCLK. */
extern uint32_t SystemCoreClock;

/* --------------------------------------------------------------------------
 * GPIO
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t sil_index;   /* port index (0=A .. 6=G) used by the sim model */

    /* Register-level view for firmware that pokes ports directly (e.g.
     * ResistanceCalibrator's GPIOE MODER/BSRR bit-bang). */
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
} GPIO_TypeDef;

typedef enum {
    GPIO_PIN_RESET = 0U,
    GPIO_PIN_SET   = 1U
} GPIO_PinState;

#define GPIO_PIN_0   ((uint16_t)0x0001U)
#define GPIO_PIN_1   ((uint16_t)0x0002U)
#define GPIO_PIN_2   ((uint16_t)0x0004U)
#define GPIO_PIN_3   ((uint16_t)0x0008U)
#define GPIO_PIN_4   ((uint16_t)0x0010U)
#define GPIO_PIN_5   ((uint16_t)0x0020U)
#define GPIO_PIN_6   ((uint16_t)0x0040U)
#define GPIO_PIN_7   ((uint16_t)0x0080U)
#define GPIO_PIN_8   ((uint16_t)0x0100U)
#define GPIO_PIN_9   ((uint16_t)0x0200U)
#define GPIO_PIN_10  ((uint16_t)0x0400U)
#define GPIO_PIN_11  ((uint16_t)0x0800U)
#define GPIO_PIN_12  ((uint16_t)0x1000U)
#define GPIO_PIN_13  ((uint16_t)0x2000U)
#define GPIO_PIN_14  ((uint16_t)0x4000U)
#define GPIO_PIN_15  ((uint16_t)0x8000U)

extern GPIO_TypeDef sil_gpio_a;
extern GPIO_TypeDef sil_gpio_b;
extern GPIO_TypeDef sil_gpio_c;
extern GPIO_TypeDef sil_gpio_d;
extern GPIO_TypeDef sil_gpio_e;
extern GPIO_TypeDef sil_gpio_f;
extern GPIO_TypeDef sil_gpio_g;

#define GPIOA ((GPIO_TypeDef*)&sil_gpio_a)
#define GPIOB ((GPIO_TypeDef*)&sil_gpio_b)
#define GPIOC ((GPIO_TypeDef*)&sil_gpio_c)
#define GPIOD ((GPIO_TypeDef*)&sil_gpio_d)
#define GPIOE ((GPIO_TypeDef*)&sil_gpio_e)
#define GPIOF ((GPIO_TypeDef*)&sil_gpio_f)
#define GPIOG ((GPIO_TypeDef*)&sil_gpio_g)

void          HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef* port, uint16_t pin);
void          HAL_GPIO_TogglePin(GPIO_TypeDef* port, uint16_t pin);

#define __HAL_GPIO_EXTI_CLEAR_IT(__EXTI_LINE__) ((void)0)

/* --------------------------------------------------------------------------
 * TIM
 * ------------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
    volatile uint32_t RCR;
    volatile uint32_t CCR1;
    volatile uint32_t CCR2;
    volatile uint32_t CCR3;
    volatile uint32_t CCR4;
    volatile uint32_t BDTR;
} TIM_TypeDef;

extern TIM_TypeDef sil_tim1;
#define TIM1 ((TIM_TypeDef*)&sil_tim1)

#define TIM_CHANNEL_1 0x0000U
#define TIM_CHANNEL_2 0x0004U
#define TIM_CHANNEL_3 0x0008U
#define TIM_CHANNEL_4 0x000CU
#define TIM_CHANNEL_ALL 0x0018U

#define TIM_IT_UPDATE    0x0001U
#define TIM_FLAG_UPDATE  0x0001U
#define TIM_FLAG_BREAK   0x0080U
#define TIM_BDTR_DTG     0x00FFU
#define TIM_BDTR_MOE     (1UL << 15)
#define TIM_CR2_MMS      (7UL << 4)
#define TIM_CR2_MMS2     (15UL << 20)
#define TIM_TRGO_OC4REF  (6UL << 4)
#define TIM_TRGO2_UPDATE (2UL << 20)

#define TIM_OCMODE_PWM1        0x0060U
#define TIM_OCPOLARITY_HIGH    0x0000U
#define TIM_OCNPOLARITY_HIGH   0x0000U
#define TIM_OCFAST_DISABLE     0x0000U
#define TIM_OCIDLESTATE_RESET  0x0000U
#define TIM_OCNIDLESTATE_RESET 0x0000U
#define TIM_CLOCKDIVISION_DIV1 0x0000U
#define TIM_COUNTERMODE_UP     0x0000U
#define TIM_DIER_UIE           0x0001U
#define TIM_CCER_CC1E          (1U << 0)
#define TIM_CCER_CC1NE         (1U << 2)
#define TIM_CCER_CC2E          (1U << 4)
#define TIM_CCER_CC2NE         (1U << 6)
#define TIM_CCER_CC3E          (1U << 8)
#define TIM_CCER_CC3NE         (1U << 10)

typedef struct {
    uint32_t OCMode;
    uint32_t Pulse;
    uint32_t OCPolarity;
    uint32_t OCNPolarity;
    uint32_t OCFastMode;
    uint32_t OCIdleState;
    uint32_t OCNIdleState;
} TIM_OC_InitTypeDef;

typedef struct {
    uint32_t Prescaler;
    uint32_t CounterMode;
    uint32_t Period;
    uint32_t ClockDivision;
    uint32_t RepetitionCounter;
    uint32_t AutoReloadPreload;
} TIM_Base_InitTypeDef;

typedef struct SIL_TimHandle {
    TIM_TypeDef*       Instance;
    TIM_Base_InitTypeDef Init;
    /* SIL-side bookkeeping (not part of the real struct layout contract —
     * only shim code reads these). */
    uint32_t           sil_active_channels;  /* bitmask of started PWM chs  */
    uint32_t           sil_active_channels_n;
    int                sil_base_running;     /* HAL_TIM_Base_Start was called */
} TIM_HandleTypeDef;

extern TIM_HandleTypeDef sil_htim1_impl;
#define htim1_ptr (&sil_htim1_impl)

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_OC_Start(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_OC_Stop(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIMEx_PWMN_Start(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIMEx_PWMN_Stop(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_Base_Start(TIM_HandleTypeDef* htim);
HAL_StatusTypeDef HAL_TIM_Base_Stop(TIM_HandleTypeDef* htim);
HAL_StatusTypeDef HAL_TIM_OC_ConfigChannel(TIM_HandleTypeDef* htim,
                                           const TIM_OC_InitTypeDef* sConfig,
                                           uint32_t channel);
void              HAL_TIM_IRQHandler(TIM_HandleTypeDef* htim);

#define __HAL_TIM_SET_PRESCALER(__HANDLE__, __VALUE__) \
    ((__HANDLE__)->Instance->PSC = (__VALUE__))
#define __HAL_TIM_SET_AUTORELOAD(__HANDLE__, __VALUE__) \
    ((__HANDLE__)->Instance->ARR = (__VALUE__))
#define __HAL_TIM_GET_AUTORELOAD(__HANDLE__) ((__HANDLE__)->Instance->ARR)
#define __HAL_TIM_ENABLE_IT(__HANDLE__, __INTERRUPT__) \
    ((__HANDLE__)->Instance->DIER |= (__INTERRUPT__))
#define __HAL_TIM_DISABLE_IT(__HANDLE__, __INTERRUPT__) \
    ((__HANDLE__)->Instance->DIER &= ~(__INTERRUPT__))
#define __HAL_TIM_CLEAR_FLAG(__HANDLE__, __FLAG__) \
    ((__HANDLE__)->Instance->SR = ~(__FLAG__))
#define __HAL_TIM_MOE_ENABLE(__HANDLE__) \
    ((__HANDLE__)->Instance->BDTR |= TIM_BDTR_MOE)
#define __HAL_TIM_MOE_DISABLE(__HANDLE__) \
    ((__HANDLE__)->Instance->BDTR &= ~TIM_BDTR_MOE)

/* Channel compare accessors used by __HAL_TIM_SET_COMPARE / GET_COMPARE. */
uint32_t* SIL_TIM_CcrPtr(TIM_HandleTypeDef* htim, uint32_t channel);
#define __HAL_TIM_SET_COMPARE(__HANDLE__, __CHANNEL__, __COMPARE__) \
    (*SIL_TIM_CcrPtr((__HANDLE__), (__CHANNEL__)) = (__COMPARE__))
#define __HAL_TIM_GET_COMPARE(__HANDLE__, __CHANNEL__) \
    (*SIL_TIM_CcrPtr((__HANDLE__), (__CHANNEL__)))

#define MODIFY_REG(REG, CLEARMASK, SETMASK) \
    ((REG) = (((REG) & ~(CLEARMASK)) | (SETMASK)))

/* HAL callback symbols the firmware overrides and the SIL scheduler calls. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef* htim);

/* --------------------------------------------------------------------------
 * ADC
 * ------------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t ISR;
    volatile uint32_t CR;
    volatile uint32_t CFGR;
    volatile uint32_t DR;
    volatile uint32_t JDR1;
    volatile uint32_t JDR2;
    volatile uint32_t JDR3;
    volatile uint32_t JDR4;
} ADC_TypeDef;

extern ADC_TypeDef sil_adc1;
extern ADC_TypeDef sil_adc2;
extern ADC_TypeDef sil_adc3;
#define ADC1 ((ADC_TypeDef*)&sil_adc1)
#define ADC2 ((ADC_TypeDef*)&sil_adc2)
#define ADC3 ((ADC_TypeDef*)&sil_adc3)

#define ADC_SCAN_ENABLE       1U
#define ADC_SCAN_DISABLE      0U
#define ADC_EOC_SEQ_CONV      1U
#define ADC_EOC_SINGLE_CONV   0U
#define ADC_SINGLE_ENDED      0U
#define ADC_OFFSET_NONE       0U
#define ADC_RIGHTBITSHIFT_NONE 0U
#define ADC_SAMPLETIME_8CYCLES_5 2U
#define ADC_CALIB_OFFSET      0U
#define ADC_CALIB_OFFSET_LINEARITY 0U

#define ADC_CHANNEL_3  3U
#define ADC_CHANNEL_4  4U
#define ADC_CHANNEL_7  7U
#define ADC_CHANNEL_8  8U
#define ADC_CHANNEL_10 10U
#define ADC_CHANNEL_11 11U

#define ADC_INJECTED_RANK_1 1U
#define ADC_INJECTED_RANK_2 2U
#define ADC_INJECTED_RANK_3 3U
#define ADC_INJECTED_RANK_4 4U

#define ADC_INJECTED_SOFTWARE_START  0U
#define ADC_EXTERNALTRIGINJEC_T1_TRGO 1U
#define ADC_EXTERNALTRIGINJECCONV_EDGE_NONE   0U
#define ADC_EXTERNALTRIGINJECCONV_EDGE_RISING 2U

#define ADC_EXTERNALTRIG_T2_TRGO   0x0BU
#define ADC_EXTERNALTRIG_T1_TRGO2  0x1BU
#define ADC_CFGR_EXTSEL            0x1FUL

#define ADC_DUALMODE_INJECSIMULT        7U
#define ADC_DUALMODEDATAFORMAT_DISABLED 0U
#define ADC_TWOSAMPLINGDELAY_5CYCLES    4U

#define ADC_ANALOGWATCHDOG_1        1U
#define ADC_ANALOGWATCHDOG_NONE     0U
#define ADC_ANALOGWATCHDOG_ALL_INJEC 0x00C00000U

/* NOTE: the real HAL defines ENABLE/DISABLE macros; Gen6FW has enum members
 * named ENABLE/DISABLE, so this shim deliberately does NOT define them (no
 * compiled firmware TU uses the macro forms). */

#define ADC_REGULAR_RANK_1          1U
#define ADC_REGULAR_RANK_2          2U
#define ADC_SAMPLETIME_24CYCLES_5   1U
#define ADC_SAMPLETIME_32CYCLES_5   2U
#define ADC3_SAMPLETIME_24CYCLES_5  1U
#define ADC3_OFFSET_SIGN_NEGATIVE   0U

typedef struct {
    uint32_t ScanConvMode;
    uint32_t EOCSelection;
} SIL_ADC_InitTypeDef;

typedef struct SIL_AdcHandle {
    ADC_TypeDef*        Instance;
    SIL_ADC_InitTypeDef Init;
    int                 sil_regular_running;
    int                 sil_injected_running;
} ADC_HandleTypeDef;

extern ADC_HandleTypeDef sil_hadc1_impl;
extern ADC_HandleTypeDef sil_hadc2_impl;
extern ADC_HandleTypeDef sil_hadc3_impl;

typedef struct {
    uint32_t InjectedChannel;
    uint32_t InjectedRank;
    uint32_t InjectedSamplingTime;
    uint32_t InjectedSingleDiff;
    uint32_t InjectedOffsetNumber;
    uint32_t InjectedOffset;
    uint32_t InjectedOffsetSignedSaturation;
    uint32_t InjectedNbrOfConversion;
    uint32_t InjectedDiscontinuousConvMode;
    uint32_t AutoInjectedConv;
    uint32_t QueueInjectedContext;
    uint32_t ExternalTrigInjecConv;
    uint32_t ExternalTrigInjecConvEdge;
    uint32_t InjecOversamplingMode;
    struct {
        uint32_t Ratio;
        uint32_t RightBitShift;
    } InjecOversampling;
} ADC_InjectionConfTypeDef;

typedef struct {
    uint32_t Mode;
    uint32_t DualModeData;
    uint32_t TwoSamplingDelay;
} ADC_MultiModeTypeDef;

typedef struct {
    uint32_t WatchdogNumber;
    uint32_t WatchdogMode;
    uint32_t ITMode;
    uint32_t HighThreshold;
    uint32_t LowThreshold;
    uint32_t Channel;
} ADC_AnalogWDGConfTypeDef;

typedef struct {
    uint32_t Channel;
    uint32_t Rank;
    uint32_t SamplingTime;
    uint32_t SingleDiff;
    uint32_t OffsetNumber;
    uint32_t Offset;
    uint32_t OffsetSignedSaturation;
    uint32_t OffsetSign;
} ADC_ChannelConfTypeDef;

HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef* hadc);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef* hadc);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef* hadc, uint32_t timeout_ms);
uint32_t          HAL_ADC_GetValue(ADC_HandleTypeDef* hadc);
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef* hadc,
                                        const ADC_ChannelConfTypeDef* sConfig);
uint32_t          HAL_ADCEx_InjectedGetValue(ADC_HandleTypeDef* hadc, uint32_t rank);
HAL_StatusTypeDef HAL_ADCEx_DisableInjectedQueue(ADC_HandleTypeDef* hadc);
HAL_StatusTypeDef HAL_ADCEx_InjectedConfigChannel(ADC_HandleTypeDef* hadc,
                                                  const ADC_InjectionConfTypeDef* cfg);
HAL_StatusTypeDef HAL_ADCEx_MultiModeConfigChannel(ADC_HandleTypeDef* hadc,
                                                   const ADC_MultiModeTypeDef* mm);
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef* hadc,
                                              uint32_t calib, uint32_t diff);
HAL_StatusTypeDef HAL_ADCEx_InjectedStart_IT(ADC_HandleTypeDef* hadc);
HAL_StatusTypeDef HAL_ADCEx_InjectedStop_IT(ADC_HandleTypeDef* hadc);
HAL_StatusTypeDef HAL_ADC_AnalogWDGConfig(ADC_HandleTypeDef* hadc,
                                          const ADC_AnalogWDGConfTypeDef* awd);

/* --------------------------------------------------------------------------
 * SPI
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t sil_index;
} SPI_TypeDef;

extern SPI_TypeDef sil_spi2;
extern SPI_TypeDef sil_spi4;
#define SPI2 ((SPI_TypeDef*)&sil_spi2)
#define SPI4 ((SPI_TypeDef*)&sil_spi4)

typedef struct {
    SPI_TypeDef* Instance;
} SPI_HandleTypeDef;

extern SPI_HandleTypeDef sil_hspi2_impl;
extern SPI_HandleTypeDef sil_hspi4_impl;

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef* hspi, const uint8_t* data,
                                   uint16_t size, uint32_t timeout_ms);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef* hspi, uint8_t* data,
                                  uint16_t size, uint32_t timeout_ms);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef* hspi,
                                          const uint8_t* tx, uint8_t* rx,
                                          uint16_t size, uint32_t timeout_ms);

/* --------------------------------------------------------------------------
 * UART
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t sil_index;
} USART_TypeDef;

extern USART_TypeDef sil_usart3;
#define USART3 ((USART_TypeDef*)&sil_usart3)

typedef struct {
    USART_TypeDef* Instance;
} UART_HandleTypeDef;

extern UART_HandleTypeDef sil_huart3_impl;

/* SIL: transmit "succeeds" immediately; the completion callback is fired by
 * the SIL scheduler one context-switch later (see sil_hal_pump()). */
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* huart,
                                        const uint8_t* data, uint16_t size);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef* huart,
                                      uint8_t* data, uint16_t size);

#define UART_CLEAR_PEF   0x0001U
#define UART_CLEAR_FEF   0x0002U
#define UART_CLEAR_NEF   0x0004U
#define UART_CLEAR_OREF  0x0008U
#define UART_CLEAR_IDLEF 0x0010U
#define __HAL_UART_CLEAR_FLAG(__HANDLE__, __FLAG__) ((void)0)

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart);   /* firmware defines */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart);   /* firmware may define */

/* --------------------------------------------------------------------------
 * FDCAN (only handle shells — the CanBus driver is a SIL stub)
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t sil_index;
} FDCAN_GlobalTypeDef;

extern FDCAN_GlobalTypeDef sil_fdcan1;
extern FDCAN_GlobalTypeDef sil_fdcan2;
#define FDCAN1 ((FDCAN_GlobalTypeDef*)&sil_fdcan1)
#define FDCAN2 ((FDCAN_GlobalTypeDef*)&sil_fdcan2)

typedef struct {
    FDCAN_GlobalTypeDef* Instance;
} FDCAN_HandleTypeDef;

extern FDCAN_HandleTypeDef sil_hfdcan1_impl;
extern FDCAN_HandleTypeDef sil_hfdcan2_impl;

/* --------------------------------------------------------------------------
 * Misc
 * ------------------------------------------------------------------------ */
void Error_Handler(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SIL_STM32H7XX_HAL_H */
