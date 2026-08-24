/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "cordic.h"
#include "crc.h"
#include "dac.h"
#include "fmac.h"
#include "hrtim.h"
#include "iwdg.h"
#include "rng.h"
#include "tim.h"
#include "usb_device.h"
#include "wwdg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Clock fixes (root-caused via DFU flash breadcrumbs, no JTAG needed):
     1) This G4 HAL version's HAL_RCC_OscConfig() enables only the PLLR
        output -- PLLP (ADC12 clock) and PLLQ (nominal 48 MHz USB/RNG clock)
        stay disabled. Enable both outputs here.
     2) USB/RNG 48 MHz comes from HSI48 + CRS (SOF-trimmed), not PLLQ:
        PLLQ is derived from HSI16 (±1%), outside USB FS tolerance (±0.25%).
        HSI48+CRS is the same crystal-less path the ST ROM bootloader uses
        for DFU. USB/RNG clock selection is set to HSI48 in usbd_conf.c /
        rng.c -- mirror that in the CubeMX clock tree before regenerating. */
  SET_BIT(RCC->PLLCFGR, RCC_PLLCFGR_PLLPEN | RCC_PLLCFGR_PLLQEN);
  SET_BIT(RCC->CRRCR, RCC_CRRCR_HSI48ON);
  while (!READ_BIT(RCC->CRRCR, RCC_CRRCR_HSI48RDY)) { }
  __HAL_RCC_CRS_CLK_ENABLE();
  {
    RCC_CRSInitTypeDef crs = {0};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000U, 1000U);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    (void)HAL_RCCEx_CRSConfig(&crs);
  }
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  /* MX_GPIO_Init() intentionally disabled: the generated init drives
     GATE_DRIVE_PWR_ENABLE / BOOTSEL_MAIN_MCU / RESET_MAIN_MCU /
     GATE_DRIVER_RESET low, which brown-out-resets THIS board (boot loop,
     measured via flash breadcrumbs). Re-enable only after setting the
     correct output levels for your hardware in CubeMX ("GPIO output level"
     High where a line must stay high). */
  /* MX_GPIO_Init(); */
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_HRTIM1_Init();
  /* WWDG disabled: 28 us timeout with no refresh resets the MCU before USB init.
     Also disable "WWDG Activated" in CubeMX or regeneration restores this call. */
  /* MX_WWDG_Init(); */
  MX_ADC3_Init();
  MX_CORDIC_Init();
  MX_CRC_Init();
  MX_DAC1_Init();
  MX_FMAC_Init();
  /* IWDG disabled: ~0.5 s timeout with no refresh would reset-loop the MCU and
     drop USB enumeration. Also disable "IWDG Activated" in CubeMX. */
  /* MX_IWDG_Init(); */
  MX_RNG_Init();
  MX_TIM1_Init();
  MX_USB_Device_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  /* PLLM /4 + PLLN 72: same 288 MHz VCO as /6 + 108 (PLLR=144M, PLLQ=48M,
     PLLP=144M) but a 4.0 MHz VCO input instead of 2.67 MHz, which sat at the
     2.66 MHz spec minimum and could dip below it with HSI16 tolerance.
     Mirror PLLM /4, PLLN 72 in the CubeMX clock tree. */
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 72;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV6;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
