/*
 * sil_hal.cpp — SIL implementations of the stm32h7xx_hal.h surface.
 *
 * All "peripherals" are plain structs; the simulated clock (sil_rt) backs
 * HAL_GetTick/HAL_Delay.  GPIO is a tiny pin-state model with the few
 * virtual inputs the firmware can observe (gate-driver READY/FAULT).
 */
#include "main.h"   /* SIL CubeMX-style pin map (sil/stm32shim) */
#include "tim.h"
#include "adc.h"
#include "spi.h"
#include "usart.h"
#include "fdcan.h"
#include "gpio.h"
#include "dma.h"

#include "sil_rt.h"
#include "sil_hooks.h"
#include "sil_live_server.h"

#include <cstdio>
#include <cstdlib>

/* --------------------------------------------------------------------------
 * Global peripheral register blocks, handles, and port objects
 * ------------------------------------------------------------------------ */

SIL_DWT_Type       sil_dwt;
SIL_CoreDebug_Type sil_coredebug;
uint32_t           SystemCoreClock = 550000000U;

GPIO_TypeDef sil_gpio_a{0, 0, 0, 0, 0, 0, 0, 0};
GPIO_TypeDef sil_gpio_b{1, 0, 0, 0, 0, 0, 0, 0};
GPIO_TypeDef sil_gpio_c{2, 0, 0, 0, 0, 0, 0, 0};
GPIO_TypeDef sil_gpio_d{3, 0, 0, 0, 0, 0, 0, 0};
GPIO_TypeDef sil_gpio_e{4, 0, 0, 0, 0, 0, 0, 0};
GPIO_TypeDef sil_gpio_f{5, 0, 0, 0, 0, 0, 0, 0};
GPIO_TypeDef sil_gpio_g{6, 0, 0, 0, 0, 0, 0, 0};

namespace {
} // namespace

TIM_TypeDef  sil_tim1;
TIM_HandleTypeDef htim1;

ADC_TypeDef  sil_adc1;
ADC_TypeDef  sil_adc2;
ADC_TypeDef  sil_adc3;
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;
ADC_HandleTypeDef hadc3;

SPI_TypeDef  sil_spi2{1};
SPI_TypeDef  sil_spi4{3};
SPI_HandleTypeDef hspi2;
SPI_HandleTypeDef hspi4;

USART_TypeDef sil_usart3{2};
UART_HandleTypeDef huart3;

FDCAN_GlobalTypeDef sil_fdcan1{0};
FDCAN_GlobalTypeDef sil_fdcan2{1};
FDCAN_HandleTypeDef hfdcan1;
FDCAN_HandleTypeDef hfdcan2;

namespace {
struct UartState {
    bool               tx_pending = false;
    UART_HandleTypeDef* pending_huart = nullptr;
};
UartState g_uart3;
} // namespace

/* Called once from host_sil main() before the firmware thread starts. */
void sil_hal_init() {
    sil_tim1 = {};
    /* Mirror MX_TIM1_Init (Src/tim.c): PSC=0, ARR=27500, RCR=0 —
     * 275 MHz counter, center-aligned, ~5 kHz switching. */
    sil_tim1.PSC = 0;
    sil_tim1.ARR = 27500;
    sil_tim1.RCR = 0;
    htim1 = TIM_HandleTypeDef{};
    htim1.Instance = &sil_tim1;
    htim1.Init.Prescaler = 0;
    htim1.Init.Period = 27500;
    htim1.Init.RepetitionCounter = 0;

    hadc1 = ADC_HandleTypeDef{}; hadc1.Instance = &sil_adc1;
    hadc2 = ADC_HandleTypeDef{}; hadc2.Instance = &sil_adc2;
    hadc3 = ADC_HandleTypeDef{}; hadc3.Instance = &sil_adc3;
    hspi2 = SPI_HandleTypeDef{}; hspi2.Instance = &sil_spi2;
    hspi4 = SPI_HandleTypeDef{}; hspi4.Instance = &sil_spi4;
    huart3 = UART_HandleTypeDef{}; huart3.Instance = &sil_usart3;
    hfdcan1 = FDCAN_HandleTypeDef{}; hfdcan1.Instance = &sil_fdcan1;
    hfdcan2 = FDCAN_HandleTypeDef{}; hfdcan2.Instance = &sil_fdcan2;
}

/* --------------------------------------------------------------------------
 * Tick / delay / interrupts
 * ------------------------------------------------------------------------ */

extern "C" {

uint32_t HAL_GetTick(void) {
    return static_cast<uint32_t>(sil_rt_now_us() / 1000ULL);
}

void HAL_Delay(uint32_t Delay) {
    sil_rt_delay_ms(Delay);
}

void __disable_irq(void) { /* cooperative model: no preemption (see README) */ }
void __enable_irq(void)  {}
void __NOP(void)         { __asm__ volatile("nop"); }
void __WFI(void)         {}
void __DMB(void)         { __asm__ volatile("" ::: "memory"); }

unsigned int __get_PRIMASK(void) { return 0; }
void __set_PRIMASK(unsigned int) {}
unsigned int __get_IPSR(void) { return 0; }

void HAL_NVIC_SetPriority(int32_t, uint32_t, uint32_t) {}
void HAL_NVIC_EnableIRQ(int32_t) {}
void HAL_NVIC_DisableIRQ(int32_t) {}
void HAL_NVIC_ClearPendingIRQ(int32_t) {}

void HAL_NVIC_SystemReset(void) {
    fprintf(stderr, "[SIL] HAL_NVIC_SystemReset called — aborting sim\n");
    abort();
}

void Error_Handler(void) {
    fprintf(stderr, "[SIL] Error_Handler called by firmware — aborting sim\n");
    abort();
}

/* --------------------------------------------------------------------------
 * GPIO model
 *
 * Output state lives in each port's ODR; firmware bit-bang through BSRR is
 * folded in at read time.  Virtual inputs the firmware can observe:
 *   GPIOC.12 GATE_DRIVER_READY   = power(PC10) && reset released(PD5)
 *   GPIOC.11 GATE_DRIVER_FAULT   = high (never faulted)
 * ------------------------------------------------------------------------ */

namespace {
uint32_t effOdr(const GPIO_TypeDef* port) {
    /* BSRR is write-only on real hardware; here it retains the last write,
     * so fold it into the visible state. */
    return (port->ODR | (port->BSRR & 0xFFFFU)) & ~(port->BSRR >> 16U);
}

bool silPinState(uint32_t port_idx, uint16_t pin) {
    GPIO_TypeDef* ports[7] = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG};
    return (effOdr(ports[port_idx]) & pin) != 0;
}
} // namespace

void HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, GPIO_PinState state) {
    if (port == nullptr || port->sil_index >= 7) return;
    /* Explicit HAL write wins over any pending BSRR state for this pin. */
    port->BSRR &= ~(static_cast<uint32_t>(pin) | (static_cast<uint32_t>(pin) << 16U));
    port->BSRR |= (state == GPIO_PIN_SET)
        ? static_cast<uint32_t>(pin)
        : (static_cast<uint32_t>(pin) << 16U);
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef* port, uint16_t pin) {
    if (port == nullptr || port->sil_index >= 7) return GPIO_PIN_RESET;
    const uint32_t idx = port->sil_index;

    if (idx == 2 && pin == GPIO_PIN_12) {          /* GATE_DRIVER_READY   */
        const bool powered  = silPinState(2, GPIO_PIN_10);
        const bool released = silPinState(3, GPIO_PIN_5);
        return (powered && released) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }
    if (idx == 2 && pin == GPIO_PIN_11) {          /* GATE_DRIVER_FAULT   */
        return GPIO_PIN_SET;                       /* active-low: no fault */
    }
    return (effOdr(port) & pin) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

void HAL_GPIO_TogglePin(GPIO_TypeDef* port, uint16_t pin) {
    if (port == nullptr || port->sil_index >= 7) return;
    port->ODR ^= pin;
}

/* --------------------------------------------------------------------------
 * TIM
 * ------------------------------------------------------------------------ */

uint32_t* SIL_TIM_CcrPtr(TIM_HandleTypeDef* htim, uint32_t channel) {
    switch (channel) {
        case TIM_CHANNEL_1: return const_cast<uint32_t*>(&htim->Instance->CCR1);
        case TIM_CHANNEL_2: return const_cast<uint32_t*>(&htim->Instance->CCR2);
        case TIM_CHANNEL_3: return const_cast<uint32_t*>(&htim->Instance->CCR3);
        case TIM_CHANNEL_4: return const_cast<uint32_t*>(&htim->Instance->CCR4);
        default:            return const_cast<uint32_t*>(&htim->Instance->CCR1);
    }
}

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* htim, uint32_t channel) {
    if (channel <= TIM_CHANNEL_4) {
        htim->sil_active_channels |= (1U << (channel >> 2));
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef* htim, uint32_t channel) {
    if (channel <= TIM_CHANNEL_4) {
        htim->sil_active_channels &= ~(1U << (channel >> 2));
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIMEx_PWMN_Start(TIM_HandleTypeDef* htim, uint32_t channel) {
    if (channel <= TIM_CHANNEL_4) {
        htim->sil_active_channels_n |= (1U << (channel >> 2));
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_OC_Start(TIM_HandleTypeDef*, uint32_t) { return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_OC_Stop(TIM_HandleTypeDef*, uint32_t) { return HAL_OK; }

HAL_StatusTypeDef HAL_TIMEx_PWMN_Stop(TIM_HandleTypeDef* htim, uint32_t channel) {
    if (channel <= TIM_CHANNEL_4) {
        htim->sil_active_channels_n &= ~(1U << (channel >> 2));
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Start(TIM_HandleTypeDef* htim) {
    htim->sil_base_running = 1;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Stop(TIM_HandleTypeDef* htim) {
    htim->sil_base_running = 0;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_OC_ConfigChannel(TIM_HandleTypeDef*,
                                           const TIM_OC_InitTypeDef*,
                                           uint32_t) {
    return HAL_OK;
}

void HAL_TIM_IRQHandler(TIM_HandleTypeDef*) {}

/* --------------------------------------------------------------------------
 * ADC
 * ------------------------------------------------------------------------ */

HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef* hadc) {
    hadc->sil_regular_running = 1;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef* hadc) {
    hadc->sil_regular_running = 0;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef*, uint32_t) {
    return HAL_OK;
}
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef* hadc) {
    return hadc->Instance->DR;
}
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef*,
                                        const ADC_ChannelConfTypeDef*) {
    return HAL_OK;
}
uint32_t HAL_ADCEx_InjectedGetValue(ADC_HandleTypeDef* hadc, uint32_t rank) {
    switch (rank) {
        case ADC_INJECTED_RANK_1: return hadc->Instance->JDR1;
        case ADC_INJECTED_RANK_2: return hadc->Instance->JDR2;
        case ADC_INJECTED_RANK_3: return hadc->Instance->JDR3;
        case ADC_INJECTED_RANK_4: return hadc->Instance->JDR4;
        default: return 0;
    }
}
HAL_StatusTypeDef HAL_ADCEx_DisableInjectedQueue(ADC_HandleTypeDef*) { return HAL_OK; }
HAL_StatusTypeDef HAL_ADCEx_InjectedConfigChannel(ADC_HandleTypeDef*,
                                                  const ADC_InjectionConfTypeDef*) {
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADCEx_MultiModeConfigChannel(ADC_HandleTypeDef*,
                                                   const ADC_MultiModeTypeDef*) {
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef*, uint32_t, uint32_t) {
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADCEx_InjectedStart_IT(ADC_HandleTypeDef* hadc) {
    hadc->sil_injected_running = 1;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADCEx_InjectedStop_IT(ADC_HandleTypeDef* hadc) {
    hadc->sil_injected_running = 0;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADC_AnalogWDGConfig(ADC_HandleTypeDef*,
                                          const ADC_AnalogWDGConfTypeDef*) {
    return HAL_OK;
}

/* --------------------------------------------------------------------------
 * SPI (no slave devices modeled; always succeeds)
 * ------------------------------------------------------------------------ */

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef*, const uint8_t*,
                                   uint16_t, uint32_t) { return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef*, uint8_t*,
                                  uint16_t, uint32_t) { return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef* hspi,
                                          const uint8_t* tx, uint8_t* rx,
                                          uint16_t size, uint32_t timeout_ms) {
    return HAL_SPI_Transmit(hspi, tx, size, timeout_ms);
}

/* --------------------------------------------------------------------------
 * UART (TX DMA completes via silUartPumpTxCompletion from the scheduler)
 *
 * In this image the only in-firmware user of HAL_UART_Transmit_DMA is the
 * Telemetry module: the bytes handed over here are the COBS-framed
 * InverterProtocol stream exactly as it would leave USART3 on hardware.
 * When the live server is active, forward them verbatim to TCP clients.
 * ------------------------------------------------------------------------ */

HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* huart,
                                        const uint8_t* data, uint16_t len) {
    if (huart != &huart3) return HAL_ERROR;
    if (g_uart3.tx_pending) return HAL_BUSY;
    sil_live_feed_tx(data, len);
    g_uart3.tx_pending = true;
    g_uart3.pending_huart = huart;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef*, uint8_t*, uint16_t) {
    return HAL_OK;   /* no RX traffic in SIL */
}

/* --------------------------------------------------------------------------
 * CubeMX init shells (never called in SIL; defined for link completeness)
 * ------------------------------------------------------------------------ */

void MX_ADC1_Init(void) {}
void MX_ADC2_Init(void) {}
void MX_ADC3_Init(void) {}
void MX_TIM1_Init(void) {}
void HAL_TIM_MspPostInit(TIM_HandleTypeDef*) {}
void MX_SPI2_Init(void) {}
void MX_SPI4_Init(void) {}
void MX_USART3_UART_Init(void) {}
void MX_FDCAN1_Init(void) {}
void MX_FDCAN2_Init(void) {}
void MX_GPIO_Init(void) {}
void MX_DMA_Init(void) {}

} /* extern "C" */

/* --------------------------------------------------------------------------
 * Scheduler hooks
 * ------------------------------------------------------------------------ */

bool silGateOutputsEnabled() {
    const bool powered  = silPinState(2, GPIO_PIN_10);
    const bool released = silPinState(3, GPIO_PIN_5);
    return powered && released;
}

void silUartPumpTxCompletion() {
    if (!g_uart3.tx_pending) return;
    UART_HandleTypeDef* h = g_uart3.pending_huart;
    g_uart3.tx_pending = false;
    g_uart3.pending_huart = nullptr;
    HAL_UART_TxCpltCallback(h);   /* declared by the shim HAL header */
}
