/* USER CODE BEGIN Header */
/**
  **************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  **************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  **************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <math.h>
#include <stdint.h>

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
ADC_HandleTypeDef hadc1;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim14;
TIM_HandleTypeDef htim16;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

#define WAVE_TABLE_SIZE          256U
#define ADC_FULL_SCALE           4095U
#define DAC_MID_SCALE            128U
#define DAC_MAX_AMPLITUDE        127U
#define MAX_ANALOG_FREQUENCY_HZ  50000UL
#define MAX_SQUARE_FREQUENCY_HZ  100000UL
#define MAX_SAMPLE_RATE_HZ       200000UL
#define TIM16_INPUT_CLOCK_HZ     64000000UL
#define BUTTON_DEBOUNCE_MS       50UL
#define ADC_UPDATE_PERIOD_MS     10UL

typedef enum
{
  WAVE_SINE = 0,
  WAVE_TRIANGLE,
  WAVE_SQUARE,
  WAVE_COUNT
} Waveform_t;

typedef enum
{
  CONTROL_FREQUENCY = 0,
  CONTROL_AMPLITUDE
} ControlMode_t;

static uint8_t wave_tables[2][WAVE_TABLE_SIZE];
static uint8_t * volatile active_wave_table = wave_tables[0];
static volatile uint32_t phase_accumulator = 0U;
static volatile uint32_t phase_increment = 0U;
static volatile Waveform_t waveform = WAVE_SQUARE;
static volatile ControlMode_t control_mode = CONTROL_FREQUENCY;
static volatile bool waveform_button_event = false;
static volatile bool control_button_event = false;

static uint8_t amplitude_code = DAC_MAX_AMPLITUDE;
static uint32_t frequency_hz = 1000U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM16_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM14_Init(void);
/* USER CODE BEGIN PFP */
static uint16_t MCP4901_Frame(uint8_t sample);
static void DAC_Write(uint8_t sample);
static void WaveTable_Rebuild(void);
static void OutputRouting_Update(void);
static void ControlLeds_Update(void);
static uint16_t ADC_ReadFiltered(void);
static uint32_t FrequencyFromAdc(uint16_t adc_value, uint32_t maximum_hz);
static void GeneratorFrequency_Update(void);
static void UserInterface_Process(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* MCP4901 frame: channel A, unbuffered Vref, 1x gain, active output. */
static uint16_t MCP4901_Frame(uint8_t sample)
{
  return (uint16_t)(0x3000U | ((uint16_t)sample << 4));
}

static void DAC_Write(uint8_t sample)
{
  const uint16_t frame = MCP4901_Frame(sample);
  uint8_t tx[2] = {(uint8_t)(frame >> 8), (uint8_t)frame};

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
  (void)HAL_SPI_Transmit(&hspi1, tx, sizeof(tx), 1U);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
}

static void WaveTable_Rebuild(void)
{
  uint32_t i;
  uint8_t * const build_table =
      (active_wave_table == wave_tables[0]) ? wave_tables[1] : wave_tables[0];

  for (i = 0U; i < WAVE_TABLE_SIZE; ++i)
  {
    int32_t signed_sample;

    switch (waveform)
    {
      case WAVE_SINE:
        signed_sample = (int32_t)lroundf(
            sinf((2.0f * 3.14159265358979323846f * (float)i) /
                 (float)WAVE_TABLE_SIZE) * (float)amplitude_code);
        break;

      case WAVE_TRIANGLE:
        if (i < 64U)
        {
          signed_sample = (int32_t)((i * (uint32_t)amplitude_code) / 64U);
        }
        else if (i < 192U)
        {
          signed_sample = (int32_t)amplitude_code -
              (int32_t)(((i - 64U) * 2U * (uint32_t)amplitude_code) / 128U);
        }
        else
        {
          signed_sample = -(int32_t)amplitude_code +
              (int32_t)(((i - 192U) * (uint32_t)amplitude_code) / 64U);
        }
        break;

      case WAVE_SQUARE:
      default:
        signed_sample = (i < (WAVE_TABLE_SIZE / 2U)) ?
            (int32_t)amplitude_code : -(int32_t)amplitude_code;
        break;
    }

    build_table[i] = (uint8_t)((int32_t)DAC_MID_SCALE + signed_sample);
  }

  /* Swap complete tables atomically so amplitude changes do not tear a cycle. */
  __disable_irq();
  active_wave_table = build_table;
  __enable_irq();
}

static void OutputRouting_Update(void)
{
  const bool square = (waveform == WAVE_SQUARE);

  /* Q3/PB5 mutes the square path; Q4/PB9 mutes the sine/triangle path. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, square ? GPIO_PIN_RESET : GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, square ? GPIO_PIN_SET : GPIO_PIN_RESET);

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6,
                    (waveform == WAVE_SINE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4,
                    (waveform == WAVE_TRIANGLE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7,
                    square ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ControlLeds_Update(void)
{
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1,
                    (control_mode == CONTROL_AMPLITUDE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2,
                    (control_mode == CONTROL_FREQUENCY) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint16_t ADC_ReadFiltered(void)
{
  uint32_t sum = 0U;

  for (uint32_t i = 0U; i < 8U; ++i)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      Error_Handler();
    }
    if (HAL_ADC_PollForConversion(&hadc1, 2U) != HAL_OK)
    {
      Error_Handler();
    }
    sum += HAL_ADC_GetValue(&hadc1);
  }

  return (uint16_t)((sum + 4U) / 8U);
}

/* Logarithmic control makes the 1 Hz end usable while still reaching 100 kHz. */
static uint32_t FrequencyFromAdc(uint16_t adc_value, uint32_t maximum_hz)
{
  const float position = (float)adc_value / (float)ADC_FULL_SCALE;
  uint32_t result = (uint32_t)lroundf(powf((float)maximum_hz, position));

  if (result < 1U)
  {
    result = 1U;
  }
  if (result > maximum_hz)
  {
    result = maximum_hz;
  }
  return result;
}

static void GeneratorFrequency_Update(void)
{
  uint32_t desired_samples_per_cycle;
  uint32_t sample_rate;
  uint32_t timer_ticks;

  desired_samples_per_cycle = (waveform == WAVE_SQUARE) ? 2U : WAVE_TABLE_SIZE;
  sample_rate = frequency_hz * desired_samples_per_cycle;
  if (sample_rate > MAX_SAMPLE_RATE_HZ)
  {
    sample_rate = MAX_SAMPLE_RATE_HZ;
  }
  if (sample_rate < (frequency_hz * 2U))
  {
    sample_rate = frequency_hz * 2U;
  }

  timer_ticks = (TIM16_INPUT_CLOCK_HZ + (sample_rate / 2U)) / sample_rate;
  if (timer_ticks < 1U)
  {
    timer_ticks = 1U;
  }
  if (timer_ticks > 65536U)
  {
    timer_ticks = 65536U;
  }
  sample_rate = TIM16_INPUT_CLOCK_HZ / timer_ticks;

  __disable_irq();
  __HAL_TIM_SET_AUTORELOAD(&htim16, timer_ticks - 1U);
  __HAL_TIM_SET_COUNTER(&htim16, 0U);
  phase_increment = (uint32_t)((((uint64_t)frequency_hz << 32) +
                               (sample_rate / 2U)) / sample_rate);
  phase_accumulator = 0U;
  __enable_irq();
}

static void UserInterface_Process(void)
{
  static uint32_t last_waveform_event_ms = 0U;
  static uint32_t last_control_event_ms = 0U;
  static uint32_t last_adc_ms = 0U;
  static uint16_t previous_adc = 0xFFFFU;
  const uint32_t now = HAL_GetTick();

  if (waveform_button_event)
  {
    waveform_button_event = false;
    if ((now - last_waveform_event_ms) >= BUTTON_DEBOUNCE_MS)
    {
      last_waveform_event_ms = now;
      waveform = (Waveform_t)(((uint32_t)waveform + 1U) % (uint32_t)WAVE_COUNT);
      if ((waveform != WAVE_SQUARE) && (frequency_hz > MAX_ANALOG_FREQUENCY_HZ))
      {
        frequency_hz = MAX_ANALOG_FREQUENCY_HZ;
      }
      WaveTable_Rebuild();
      OutputRouting_Update();
      GeneratorFrequency_Update();
    }
  }

  if (control_button_event)
  {
    control_button_event = false;
    if ((now - last_control_event_ms) >= BUTTON_DEBOUNCE_MS)
    {
      last_control_event_ms = now;
      control_mode = (control_mode == CONTROL_FREQUENCY) ?
          CONTROL_AMPLITUDE : CONTROL_FREQUENCY;
      previous_adc = 0xFFFFU; /* Apply the pot immediately in the new mode. */
      ControlLeds_Update();
    }
  }

  if ((now - last_adc_ms) >= ADC_UPDATE_PERIOD_MS)
  {
    const uint16_t adc = ADC_ReadFiltered();
    last_adc_ms = now;

    if ((previous_adc == 0xFFFFU) ||
        ((adc > previous_adc) ? (adc - previous_adc) : (previous_adc - adc)) >= 8U)
    {
      previous_adc = adc;
      if (control_mode == CONTROL_AMPLITUDE)
      {
        amplitude_code = (uint8_t)(((uint32_t)adc * DAC_MAX_AMPLITUDE +
                                    (ADC_FULL_SCALE / 2U)) / ADC_FULL_SCALE);
        WaveTable_Rebuild();
      }
      else
      {
        const uint32_t maximum_hz = (waveform == WAVE_SQUARE) ?
            MAX_SQUARE_FREQUENCY_HZ : MAX_ANALOG_FREQUENCY_HZ;
        frequency_hz = FrequencyFromAdc(adc, maximum_hz);
        GeneratorFrequency_Update();
      }
    }
  }
}

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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_SPI1_Init();
  MX_TIM16_Init();
  MX_ADC1_Init();
  MX_TIM14_Init();
  /* USER CODE BEGIN 2 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
  WaveTable_Rebuild();
  OutputRouting_Update();
  ControlLeds_Update();
  GeneratorFrequency_Update();

  if (HAL_TIM_Base_Start_IT(&htim16) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    UserInterface_Process();
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 8;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.LowPowerAutoPowerOff = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_19CYCLES_5;
  hadc1.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_19CYCLES_5;
  hadc1.Init.OversamplingMode = DISABLE;
  hadc1.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM14 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM14_Init(void)
{

  /* USER CODE BEGIN TIM14_Init 0 */

  /* USER CODE END TIM14_Init 0 */

  /* USER CODE BEGIN TIM14_Init 1 */

  /* USER CODE END TIM14_Init 1 */
  htim14.Instance = TIM14;
  htim14.Init.Prescaler = 64000 - 1;
  htim14.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim14.Init.Period = 10 - 1;
  htim14.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim14.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim14) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM14_Init 2 */

  /* USER CODE END TIM14_Init 2 */

}

/**
  * @brief TIM16 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM16_Init(void)
{

  /* USER CODE BEGIN TIM16_Init 0 */

  /* USER CODE END TIM16_Init 0 */

  /* USER CODE BEGIN TIM16_Init 1 */

  /* USER CODE END TIM16_Init 1 */
  htim16.Instance = TIM16;
  htim16.Init.Prescaler = 0;
  htim16.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim16.Init.Period = 64000-1;
  htim16.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim16.Init.RepetitionCounter = 0;
  htim16.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim16) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM16_Init 2 */

  /* USER CODE END TIM16_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_7B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9|GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2
                          |GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : PB9 PB0 PB1 PB2
                           PB4 PB5 PB6 PB7 */
  GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2
                          |GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : T_NRST_Pin */
  GPIO_InitStruct.Pin = T_NRST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(T_NRST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PA8 PA9 */
  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : LD3_Pin */
  GPIO_InitStruct.Pin = LD3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD3_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI4_15_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */


/**
 * @brief Callback function triggered on falling edge interrupt for GPIO pins.
 *
 * This function is called when a falling edge interrupt is detected on the specified GPIO pin.
 * If the GPIO pin is GPIO_PIN_9, it increments the mode variable cyclically from 1 to 3
 * to choose what kind of signals display.
 * If the GPIO pin is GPIO_PIN_8, it toggles the change_amplitude
 * boolean variable and adjusts GPIO pins accordingly for visual feedback.
 *
 * @param GPIO_Pin The GPIO pin that triggered the falling edge interrupt.
 *
 * @return None.
 */
void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_9)
  {
    waveform_button_event = true;
  }
  else if (GPIO_Pin == GPIO_PIN_8)
  {
    control_button_event = true;
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM16)
  {
    const uint8_t table_index = (uint8_t)(phase_accumulator >> 24);
    phase_accumulator += phase_increment;
    DAC_Write(active_wave_table[table_index]);
  }
}
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

#ifdef  USE_FULL_ASSERT
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