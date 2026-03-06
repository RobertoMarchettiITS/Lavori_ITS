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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RESET(c)    LL_GPIO_ResetOutputPin(c##_GPIO_Port, c##_Pin)
#define SET(c)      LL_GPIO_SetOutputPin(c##_GPIO_Port, c##_Pin)
#define TOGGLE(c)   LL_GPIO_TogglePin(c##_GPIO_Port, c##_Pin)
#define DIR_OUT(c)  LL_GPIO_SetPinMode(c##_GPIO_Port, c##_Pin, LL_GPIO_MODE_OUTPUT)
#define DIR_IN(c)   LL_GPIO_SetPinMode(c##_GPIO_Port, c##_Pin, LL_GPIO_MODE_INPUT)
#define READ(c)     LL_GPIO_IsInputPinSet(c##_GPIO_Port, c##_Pin)
#define READPORT(c) LL_GPIO_ReadInputPort(c##_GPIO_Port)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile uint8_t paused = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
/* USER CODE BEGIN PFP */
void Delay_uS(uint32_t us);
void setLcdDataPort(uint8_t portDato);
int  lcdCheckBusy(void);
void lcdSendCmd(uint8_t cmd);
void lcdSendChar(uint8_t data);
void lcdInit(void);
void lcdTextWrite(uint8_t row, uint8_t col, const char *str,
                  uint8_t clearLine, uint8_t scroll);
void UserButtonIntCallBack(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ── Microsecond delay ──────────────────────────────────────────────────── */
/* Calibrated for STM32C031 at 48 MHz HSE.                                   */
void Delay_uS(uint32_t us)
{
    volatile uint32_t count = us * 12;
    while (count--);
}

/* ── Set DB4-DB7 data pins ──────────────────────────────────────────────── */
void setLcdDataPort(uint8_t portDato)
{
    if (portDato & 0x1) { SET(DISP_DB4); } else { RESET(DISP_DB4); }
    if (portDato & 0x2) { SET(DISP_DB5); } else { RESET(DISP_DB5); }
    if (portDato & 0x4) { SET(DISP_DB6); } else { RESET(DISP_DB6); }
    if (portDato & 0x8) { SET(DISP_DB7); } else { RESET(DISP_DB7); }
}

/* ── Wait until LCD busy flag clears ───────────────────────────────────── */
int lcdCheckBusy(void)
{
    uint8_t tmpLcdData;
    int delay2 = 0;
    DIR_IN(DISP_DB4); DIR_IN(DISP_DB5);
    DIR_IN(DISP_DB6); DIR_IN(DISP_DB7);
    RESET(DISP_RS);
    SET(DISP_RW);
    do {
        Delay_uS(1);
        SET(DISP_EN);
        Delay_uS(1);
        tmpLcdData = READ(DISP_DB7);
        RESET(DISP_EN);
        Delay_uS(1);
        SET(DISP_EN);
        Delay_uS(1);
        RESET(DISP_EN);
        if (tmpLcdData == 0) break;
    } while (delay2++ < 200);
    RESET(DISP_RW);
    DIR_OUT(DISP_DB4); DIR_OUT(DISP_DB5);
    DIR_OUT(DISP_DB6); DIR_OUT(DISP_DB7);
    return tmpLcdData;
}

/* ── Send 4 bits + EN pulse ─────────────────────────────────────────────── */
static void lcdSendNibble(uint8_t nibble)
{
    setLcdDataPort(nibble & 0x0F);
    Delay_uS(1);
    SET(DISP_EN);
    Delay_uS(1);
    RESET(DISP_EN);
    Delay_uS(1);
}

/* ── Send a full byte as two nibbles (high first, then low) ─────────────── */
static void lcdSendByte(uint8_t byte, uint8_t isData)
{
    lcdCheckBusy();

    if (isData)
        SET(DISP_RS);   /* RS=1 → data register    */
    else
        RESET(DISP_RS); /* RS=0 → command register */

    RESET(DISP_RW);     /* RW=0 → write            */

    lcdSendNibble(byte >> 4);    /* high nibble first */
    lcdSendNibble(byte & 0x0F);  /* low nibble second */
}

/* ── Send a command byte to the LCD (RS=0) ──────────────────────────────── */
void lcdSendCmd(uint8_t cmd)
{
    lcdSendByte(cmd, 0);
}

/* ── Send a data/character byte to the LCD (RS=1) ──────────────────────── */
void lcdSendChar(uint8_t data)
{
    lcdSendByte(data, 1);
}

/* ── Full HD44780 4-bit initialization sequence ─────────────────────────── */
void lcdInit(void)
{
    RESET(DISP_EN);
    RESET(DISP_RS);
    RESET(DISP_RW);

    HAL_Delay(50);          /* >40 ms after Vcc rises              */

    /* Special 8-bit wake-up sequence sent as single nibbles */
    lcdSendNibble(0x03);
    HAL_Delay(5);           /* >4.1 ms                             */

    lcdSendNibble(0x03);
    Delay_uS(150);          /* >100 µs                             */

    lcdSendNibble(0x03);
    Delay_uS(150);

    /* Switch to 4-bit mode */
    lcdSendNibble(0x02);
    Delay_uS(150);

    /* Configuration commands (now fully in 4-bit mode) */
    lcdSendCmd(0x28); /* Function Set: 4-bit, 2 lines, 5x8 font  */
    lcdSendCmd(0x08); /* Display OFF                              */
    lcdSendCmd(0x01); /* Clear Display                            */
    HAL_Delay(2);     /* Clear needs >1.52 ms                     */
    lcdSendCmd(0x06); /* Entry Mode: cursor right, no shift       */
    lcdSendCmd(0x0C); /* Display ON, cursor OFF, blink OFF        */
}

/* ── Position cursor and print a string ─────────────────────────────────── */
/* row: 0-1  |  col: 0-15                                                    */
/* clearLine: 1 = pulisce tutta la riga con spazi prima di scrivere          */
/* scroll: ignorato (non supportato dall'HD44780 in modalità base)           */
void lcdTextWrite(uint8_t row, uint8_t col, const char *str,
                  uint8_t clearLine, uint8_t scroll)
{
    (void)scroll; /* non usato */

    uint8_t address = col + (row == 0 ? 0x00 : 0x40);
    lcdSendCmd(0x80 | address);

    if (clearLine)
    {
        /* Pulisce tutta la riga con spazi, poi riposiziona il cursore */
        for (uint8_t i = 0; i < 16; i++)
            lcdSendChar(' ');
        lcdSendCmd(0x80 | address);
    }

    while (*str)
        lcdSendChar((uint8_t)*str++);
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

  /* USER CODE BEGIN 2 */
  lcdInit();
  /* USER CODE END 2 */

  /* USER CODE BEGIN WHILE */
  uint8_t i = 0;
  char buff[16];

  while (1)
  {
  /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (!paused)
    {
        lcdSendCmd(0x01);
        HAL_Delay(2);

        sprintf(buff, "Char %4d -> ", i);
        lcdTextWrite(0, 0, buff, 1, 0);
        lcdSendChar(i);

        sprintf(buff, "Char 0x%02X -> %c", i, i);
        lcdTextWrite(1, 0, buff, 1, 0);

        i += 1;
        HAL_Delay(750);
    }
    /* USER CODE END 3 */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_FLASH_SET_LATENCY(FLASH_LATENCY_1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSE;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  LL_EXTI_InitTypeDef EXTI_InitStruct = {0};
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOC);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOF);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
  LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);

  /**/
  LL_GPIO_ResetOutputPin(USER_LED_GPIO_Port, USER_LED_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_DB4_GPIO_Port, DISP_DB4_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_EN_GPIO_Port, DISP_EN_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_DB7_GPIO_Port, DISP_DB7_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_DB5_GPIO_Port, DISP_DB5_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_DB6_GPIO_Port, DISP_DB6_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_RW_GPIO_Port, DISP_RW_Pin);

  /**/
  LL_GPIO_ResetOutputPin(DISP_RS_GPIO_Port, DISP_RS_Pin);

  /**/
  EXTI_InitStruct.Line_0_31 = LL_EXTI_LINE_13;
  EXTI_InitStruct.LineCommand = ENABLE;
  EXTI_InitStruct.Mode = LL_EXTI_MODE_IT;
  EXTI_InitStruct.Trigger = LL_EXTI_TRIGGER_RISING;
  LL_EXTI_Init(&EXTI_InitStruct);

  /**/
  LL_GPIO_SetPinMode(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin, LL_GPIO_MODE_INPUT);

  /**/
  LL_GPIO_SetPinPull(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin, LL_GPIO_PULL_UP);

  /**/
  LL_EXTI_SetEXTISource(LL_EXTI_CONFIG_PORTC, LL_EXTI_CONFIG_LINE13);

  /**/
  GPIO_InitStruct.Pin = VCP_USART2_TX_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  GPIO_InitStruct.Alternate = LL_GPIO_AF_1;
  LL_GPIO_Init(VCP_USART2_TX_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = VCP_USART2_RX_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  GPIO_InitStruct.Alternate = LL_GPIO_AF_1;
  LL_GPIO_Init(VCP_USART2_RX_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = USER_LED_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(USER_LED_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_DB4_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_DB4_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_EN_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_EN_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_DB7_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_DB7_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_DB5_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_DB5_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_DB6_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_DB6_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_RW_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_RW_GPIO_Port, &GPIO_InitStruct);

  /**/
  GPIO_InitStruct.Pin = DISP_RS_Pin;
  GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  LL_GPIO_Init(DISP_RS_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  NVIC_SetPriority(EXTI4_15_IRQn, 0);
  NVIC_EnableIRQ(EXTI4_15_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* ── Interrupt callback: USER_BUTTON rilasciato → toggle USER_LED ────────── */
/* Chiamata da EXTI4_15_IRQHandler in stm32c0xx_it.c sul fronte di salita    */
/* (rising edge = rilascio del pulsante, poiché il tasto è attivo basso)     */
void UserButtonIntCallBack(void)
{
    paused = !paused;
    TOGGLE(USER_LED);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1) {}
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

  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
