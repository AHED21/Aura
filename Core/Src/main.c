/* USER CODE BEGIN Header */
/**
  **************************
  * @file           : main.c
  * @brief          : Main program body
  **************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  **************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "rgb_led.h"
#include "dht11.h"
#include "bh1750.h"
#include "lcd_i2c.h"
#include "station.h"
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

I2C_HandleTypeDef hi2c1;

RTC_HandleTypeDef hrtc;

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;

UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
/* USART2 = port série virtuel du ST-LINK (PA2/PA3) pour le moniteur série à 115200 bauds.
 * CubeMX n'a pas généré son initialisation : elle est faite à la main dans USART2_Init_Manuel. */
UART_HandleTypeDef huart2;
static uint8_t mode_test = 0;   /* 1 si le bouton bleu B1 est maintenu au démarrage */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_RTC_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
static void MX_SPI2_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */
static void USART2_Init_Manuel(void);
static void Test_Materiel(void);
static void Test_Boucle(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ========================================================================
 *  TEST MATÉRIEL DE LA STATION
 *  Brochage (CubeMX / CheatSheet) :
 *    LED RGB   : R = PA0 (A0), V = PA1 (A1), B = PA4 (A2)
 *    DHT11     : data = PB0 (A3)
 *    BH1750    : SCL = PB8 (D15), SDA = PB9 (D14)      -> I2C1
 *    Carte SD  : SCK = PA5 (D13), MISO = PA6 (D12), MOSI = PA7 (D11), CS = PB6 (D10) -> SPI1
 *    GPS       : PC10 = TX STM32 -> RX GPS, PC11 = RX STM32 <- TX GPS -> USART3 9600
 *    Boutons   : B1 bleu = PC13 (sur la carte), bouton externe = PC3 vers GND
 *    Console   : USB ST-LINK, USART2, 115200 bauds
 *  La LED verte LD2 n'est pas utilisée : elle partage PA5 avec le SCK de la carte SD.
 * ======================================================================== */

typedef enum { TEST_OK, TEST_ECHEC, TEST_VISUEL } Resultat;

static char log_buf[160];
#define LOG(...) do { int n_ = snprintf(log_buf, sizeof log_buf, __VA_ARGS__); \
    if (n_ > 0) HAL_UART_Transmit(&huart2, (uint8_t *)log_buf, (uint16_t)strlen(log_buf), 200); } while (0)

static BH1750_t bh;
static uint8_t  bh_ok = 0;

static const char *Txt(Resultat r)
{
  return (r == TEST_OK) ? "OK" : (r == TEST_ECHEC) ? "ECHEC" : "A VERIFIER A L OEIL";
}

static void USART2_Init_Manuel(void)
{
  /* PA2/PA3 sont déjà configurées en AF7 par MX_GPIO_Init ; il reste l'horloge et le périphérique. */
  __HAL_RCC_USART2_CLK_ENABLE();
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* ---- 1. LED RGB : rouge, vert, bleu, blanc ---- */
static Resultat Test_LED(void)
{
  LOG("\r\n[1] LED RGB : rouge, vert, bleu, blanc (1 s chacun)\r\n");
  RGB_LED_Init();
  RGB_LED_Set(1, 0, 0); LOG("    rouge\r\n"); HAL_Delay(1000);
  RGB_LED_Set(0, 1, 0); LOG("    vert\r\n");  HAL_Delay(1000);
  RGB_LED_Set(0, 0, 1); LOG("    bleu\r\n");  HAL_Delay(1000);
  RGB_LED_Set(1, 1, 1); LOG("    blanc\r\n"); HAL_Delay(1000);
  RGB_LED_Set(0, 0, 0);
  LOG("    Si les couleurs sont inversees ou eteintes : LED a anode commune,\r\n"
      "    mettre RGB_LED_COMMON_CATHODE a 0 dans rgb_led.h\r\n");
  return TEST_VISUEL;
}

/* ---- 2 et 3. Boutons : actifs à l'état bas ---- */
static Resultat Test_Bouton(const char *nom, GPIO_TypeDef *port, uint16_t pin)
{
  LOG("\r\n[%s] Appuie sur le bouton dans les 10 secondes...\r\n", nom);
  if (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET)
  {
    LOG("    ATTENTION : la broche est deja a 0 avant tout appui\r\n"
        "    (bouton cable sur deux pattes toujours reliees : le brancher en diagonale)\r\n");
    return TEST_ECHEC;
  }
  uint32_t t0 = HAL_GetTick();
  while (HAL_GetTick() - t0 < 10000U)
  {
    if (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET)
    {
      RGB_LED_Set(0, 1, 0);
      LOG("    appui detecte\r\n");
      uint32_t t1 = HAL_GetTick();
      while (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET)        /* attendre le relâchement, 5 s maximum */
      {
        if (HAL_GetTick() - t1 > 5000U)
        {
          RGB_LED_Set(0, 0, 0);
          LOG("    le bouton ne se relache pas : cablage a verifier\r\n");
          return TEST_ECHEC;
        }
      }
      RGB_LED_Set(0, 0, 0);
      HAL_Delay(50);                                              /* anti-rebond */
      return TEST_OK;
    }
  }
  LOG("    aucun appui en 10 s\r\n");
  return TEST_ECHEC;
}

/* ---- 4. DHT11 sur PB0 ---- */
static Resultat Test_DHT11(void)
{
  LOG("\r\n[4] DHT11 sur PB0 (A3)\r\n");
  DHT11_Init();
  HAL_Delay(1500);                         /* le capteur a besoin d'environ 1 s après la mise sous tension */
  for (int essai = 1; essai <= 3; essai++)
  {
    DHT11_Data d;
    HAL_StatusTypeDef st = DHT11_Read(&d);
    if (st == HAL_OK)
    {
      LOG("    temperature = %d C   humidite = %d %%\r\n", (int)d.temperature, (int)d.humidity);
      if (d.humidity >= 5 && d.humidity <= 95 && d.temperature <= 50) return TEST_OK;
      LOG("    valeurs incoherentes\r\n");
      return TEST_ECHEC;
    }
    LOG("    essai %d : %s\r\n", essai, (st == HAL_TIMEOUT) ? "pas de reponse (cablage out, VCC, GND)" : "erreur de checksum");
    HAL_Delay(2000);                       /* au moins 1 à 2 s entre deux lectures */
  }
  return TEST_ECHEC;
}

/* ---- 5. BH1750 sur I2C1 ---- */
static Resultat Test_BH1750(void)
{
  uint8_t addr = 0;
  LOG("\r\n[5] I2C1 (SCL PB8 D15, SDA PB9 D14) : scan du bus\r\n");
  /* Au repos, les deux lignes I2C doivent être à 1 grâce aux résistances de rappel des modules. */
  int scl = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8) == GPIO_PIN_SET;
  int sda = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET;
  LOG("    lignes au repos : SCL=%d SDA=%d (attendu 1 et 1)\r\n", scl, sda);
  if (!scl || !sda)
    LOG("    une ligne I2C est bloquee a 0 : un module est mal cable, en court-circuit vers GND, ou non alimente\r\n");
  for (uint8_t a = 1; a < 128; a++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2, 10) == HAL_OK)
    {
      LOG("    peripherique trouve a l adresse 0x%02X\r\n", a);
      if (a == 0x23 || a == 0x5C) addr = (uint8_t)(a << 1);
    }
  }
  if (addr == 0)
  {
    LOG("    BH1750 introuvable (0x23 ou 0x5C)\r\n");
    return TEST_ECHEC;
  }
  if (BH1750_Init(&bh, &hi2c1, addr, BH1750_CONT_H) != HAL_OK)
  {
    LOG("    BH1750 : echec d initialisation\r\n");
    return TEST_ECHEC;
  }
  float lux;
  if (BH1750_ReadLux(&bh, &lux) != HAL_OK)
  {
    LOG("    BH1750 : erreur de lecture\r\n");
    return TEST_ECHEC;
  }
  bh_ok = 1;
  LOG("    luminosite = %u lux (cache le capteur avec la main : la valeur doit baisser)\r\n", (unsigned)lux);
  return TEST_OK;
}

/* ---- 6. RTC interne : l'heure doit avancer ---- */
static Resultat Test_RTC(void)
{
  RTC_TimeTypeDef t1, t2;
  RTC_DateTypeDef d;
  LOG("\r\n[6] RTC interne\r\n");
  HAL_RTC_GetTime(&hrtc, &t1, RTC_FORMAT_BIN); HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
  HAL_Delay(2100);
  HAL_RTC_GetTime(&hrtc, &t2, RTC_FORMAT_BIN); HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
  LOG("    %02u:%02u:%02u puis %02u:%02u:%02u\r\n", t1.Hours, t1.Minutes, t1.Seconds, t2.Hours, t2.Minutes, t2.Seconds);
  uint32_t s1 = t1.Hours * 3600U + t1.Minutes * 60U + t1.Seconds;
  uint32_t s2 = t2.Hours * 3600U + t2.Minutes * 60U + t2.Seconds;
  return (s2 > s1) ? TEST_OK : TEST_ECHEC;
}

/* ---- 7. Carte SD en SPI1 : la carte doit répondre 0x01 à la commande CMD0 ---- */
static uint8_t SD_Octet(uint8_t out)
{
  uint8_t in = 0xFF;
  HAL_SPI_TransmitReceive(&hspi1, &out, &in, 1, 50);
  return in;
}

static Resultat Test_SD(void)
{
  const uint8_t cmd0[6] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
  uint8_t r1 = 0xFF;
  LOG("\r\n[7] Carte SD en SPI1 (SCK PA5 D13, MISO PA6 D12, MOSI PA7 D11, CS PB6 D10)\r\n");

  /* BSP_LED_Init a reconfiguré PA5 pour la LED LD2 : on remet les broches du SPI1. */
  HAL_SPI_MspInit(&hspi1);

  /* Diagnostic 1 : CS haut, la carte ne doit pas parler, MISO doit rester à 1 (0xFF). */
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
  uint8_t repos = 0xFF;
  for (int i = 0; i < 10; i++) repos &= SD_Octet(0xFF);  /* 80 impulsions d'horloge, CS haut */
  LOG("    MISO au repos (CS haut) = 0x%02X (attendu 0xFF)\r\n", repos);
  if (repos == 0x00)
  {
    LOG("    MISO est bloque a 0 : verifier le fil MISO vers D12, qu il ne soit pas inverse avec MOSI,\r\n"
        "    et que le VCC du module soit sur 5V\r\n");
  }

  /* Diagnostic 2 : jusqu'à 5 essais de CMD0, certaines cartes ne répondent pas au premier. */
  for (int essai = 1; essai <= 5 && r1 != 0x01; essai++)
  {
    r1 = 0xFF;
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
    for (int i = 0; i < 6; i++) SD_Octet(cmd0[i]);
    for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = SD_Octet(0xFF);
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
    SD_Octet(0xFF);
    LOG("    essai %d : reponse a CMD0 = 0x%02X (attendu 0x01)\r\n", essai, r1);
    HAL_Delay(50);
  }

  if (r1 == 0x01) return TEST_OK;
  if (r1 == 0xFF)
    LOG("    la carte ne repond pas : carte absente ou mal inseree, CS vers D10, SCK vers D13, MOSI vers D11, VCC 5V\r\n");
  else
    LOG("    reponse anormale : verifier MISO vers D12 et MOSI vers D11 (souvent inverses), ou changer de carte\r\n");
  return TEST_ECHEC;
}

/* ---- 8. GPS sur USART3 9600 : on attend des trames NMEA commençant par $ ---- */
static Resultat Test_GPS(void)
{
  char ligne[100];
  uint8_t c;
  int n = 0, trames = 0;
  LOG("\r\n[8] GPS sur USART3 (PC10 vers RX du GPS, PC11 vers TX du GPS), 9600 bauds, 5 s d ecoute\r\n");
  __HAL_UART_CLEAR_OREFLAG(&huart3);
  uint32_t t0 = HAL_GetTick();
  while (HAL_GetTick() - t0 < 5000U)
  {
    if (HAL_UART_Receive(&huart3, &c, 1, 20) != HAL_OK)
    {
      __HAL_UART_CLEAR_OREFLAG(&huart3);
      continue;
    }
    if (c == '$') n = 0;
    if (n < (int)sizeof ligne - 1) ligne[n++] = (char)c;
    if (c == '\n' && n > 6 && ligne[0] == '$')
    {
      ligne[n] = '\0';
      trames++;
      if (strncmp(ligne + 3, "RMC", 3) == 0)
      {
        const char *p = strchr(ligne, ',');
        if (p) p = strchr(p + 1, ',');
        char statut = (p && p[1]) ? p[1] : '?';
        LOG("    %s", ligne);
        LOG("    statut %c : %s\r\n", statut, (statut == 'A') ? "position valide" : "pas encore de position, mettre l antenne face au ciel");
        return TEST_OK;
      }
      n = 0;
    }
  }
  if (trames > 0)
  {
    LOG("    %d trames recues mais pas de RMC : le GPS communique\r\n", trames);
    return TEST_OK;
  }
  LOG("    aucune trame : verifier TX GPS vers PC11, RX GPS vers PC10, VCC, GND\r\n");
  return TEST_ECHEC;
}

/* ---- 9. Écran LCD1602 I2C sur le même bus I2C1 que le BH1750 ---- */
static LCD_I2C_t lcd;
static uint8_t   lcd_ok = 0;

static Resultat Test_LCD(void)
{
  LOG("\r\n[9] Ecran LCD1602 I2C (SDA PB9 D14, SCL PB8 D15) : recherche automatique de l adresse\r\n");
  /* Aucun réglage d'adresse à faire : on essaie toutes les adresses possibles du module PCF8574
   * (0x20 à 0x27 pour le PCF8574T, 0x38 à 0x3F pour le PCF8574AT), en sautant celle du BH1750 (0x23). */
  uint8_t addr = 0;
  const uint8_t debut[2] = { 0x27, 0x3F };          /* on commence par les adresses d'usine les plus courantes */
  for (int plage = 0; plage < 2 && addr == 0; plage++)
  {
    for (int i = 0; i < 8 && addr == 0; i++)
    {
      uint8_t a = (uint8_t)(debut[plage] - i);
      if (a == 0x23) continue;                       /* adresse du BH1750 */
      if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2, 10) == HAL_OK) addr = (uint8_t)(a << 1);
    }
  }
  if (addr == 0)
  {
    LOG("    LCD introuvable : verifier SDA vers D14, SCL vers D15, VCC 5V, GND\r\n");
    return TEST_ECHEC;
  }
  if (LCD_Init(&lcd, &hi2c1, addr) != HAL_OK)
  {
    LOG("    LCD : echec d initialisation\r\n");
    return TEST_ECHEC;
  }
  lcd_ok = 1;
  LCD_SetCursor(&lcd, 0, 0); LCD_Print(&lcd, "STATION AURA");
  LCD_SetCursor(&lcd, 0, 1); LCD_Print(&lcd, "Test LCD OK");
  LOG("    LCD trouve a l adresse 0x%02X : il doit afficher STATION AURA\r\n"
      "    Ecran allume mais vide ou rempli de carres : tourner la vis bleue du contraste au dos\r\n", addr >> 1);
  return TEST_VISUEL;
}

static void Test_Materiel(void)
{
  Resultat r[9];
  const char *noms[9] = { "LED RGB", "Bouton B1 bleu PC13", "Bouton externe PC3", "DHT11",
                          "BH1750", "RTC interne", "Carte SD", "GPS", "Ecran LCD1602" };

  LOG("\r\n\r\n========== TEST MATERIEL STATION AURA ==========\r\n");
  r[0] = Test_LED();
  r[1] = Test_Bouton("2 bouton bleu B1 sur la carte, PC13", GPIOC, GPIO_PIN_13);
  r[2] = Test_Bouton("3 bouton externe PC3", GPIOC, GPIO_PIN_3);
  r[3] = Test_DHT11();
  r[4] = Test_BH1750();
  r[5] = Test_RTC();
  r[6] = Test_SD();
  r[7] = Test_GPS();
  r[8] = Test_LCD();

  int echecs = 0;
  LOG("\r\n========== BILAN ==========\r\n");
  for (int i = 0; i < 9; i++)
  {
    LOG("  %-22s : %s\r\n", noms[i], Txt(r[i]));
    if (r[i] == TEST_ECHEC) echecs++;
  }
  LOG("  %d echec(s)\r\n===========================\r\n\r\n", echecs);

  /* LED : vert fixe si tout va bien, rouge fixe sinon */
  if (echecs == 0) RGB_LED_Set(0, 1, 0); else RGB_LED_Set(1, 0, 0);
}

/* Mesures en continu toutes les 2 secondes après le test */
static void Test_Boucle(void)
{
  static uint32_t dernier = 0;
  if (HAL_GetTick() - dernier < 2000U) return;
  dernier = HAL_GetTick();

  RTC_TimeTypeDef t;
  RTC_DateTypeDef d;
  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN); HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

  DHT11_Data m;
  char dht[40] = "DHT11 NA";
  if (DHT11_Read(&m) == HAL_OK) snprintf(dht, sizeof dht, "T=%dC H=%d%%", (int)m.temperature, (int)m.humidity);

  char lux[24] = "lux NA";
  float l;
  if (bh_ok && BH1750_ReadLux(&bh, &l) == HAL_OK) snprintf(lux, sizeof lux, "%u lux", (unsigned)l);

  LOG("%02u:%02u:%02u  %s  %s  B1=%s  PC3=%s\r\n", t.Hours, t.Minutes, t.Seconds, dht, lux,
      HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET ? "appuye" : "relache",
      HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_3)  == GPIO_PIN_RESET ? "appuye" : "relache");

  /* Écran : ligne 1 = heure + lux, ligne 2 = température + humidité (16 caractères par ligne) */
  if (lcd_ok)
  {
    char l1[17], l2[17];
    snprintf(l1, sizeof l1, "%02u:%02u:%02u %-7.7s", t.Hours % 24U, t.Minutes % 60U, t.Seconds % 60U, lux);
    snprintf(l2, sizeof l2, "%-16.16s", dht);
    LCD_SetCursor(&lcd, 0, 0); LCD_Print(&lcd, l1);
    LCD_SetCursor(&lcd, 0, 1); LCD_Print(&lcd, l2);
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
  MX_RTC_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  MX_USART3_UART_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
  USART2_Init_Manuel();
  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /* Bouton bleu B1 maintenu au démarrage : test du matériel. Sinon : programme de la station. */
  if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET)
  {
    mode_test = 1;
    Test_Materiel();
  }
  else
  {
    Station_Init();
  }
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (mode_test) Test_Boucle(); else Station_Loop();
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
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 10;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
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

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x10D19CE4;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */
  /* Heure déjà réglée par CLOCK ou DATE (registre de sauvegarde) : ne pas la remettre à 00:00 au RESET */
  if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) == 0x32F2)
  {
    return;
  }
  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = RTC_MONTH_JANUARY;
  sDate.Date = 0x1;
  sDate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

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
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
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
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_4BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 7;
  hspi2.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi2.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 9600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

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
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5
                          |SD_CS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC3 */
  GPIO_InitStruct.Pin = GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PA0 PA1 PA4 */
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : USART_TX_Pin USART_RX_Pin */
  GPIO_InitStruct.Pin = USART_TX_Pin|USART_RX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PB1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF10_QUADSPI;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB3 PB4 PB5 SD_CS_Pin */
  GPIO_InitStruct.Pin = GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5|SD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
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
