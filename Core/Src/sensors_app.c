#include "main.h"
#include "sensors_app.h"
#include "ldr.h"
#include "dht11.h"
#include "rgb_led.h"
#include "bh1750.h"
#include <stdio.h>
#include <string.h>

extern ADC_HandleTypeDef hadc1;
extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart2;

static char txbuf[96];
static BH1750_t bh;
static uint8_t bh_ok = 0;

/* Envoie un message sur USART2 (visible dans le Serial Monitor, 115200 bauds) */
#define LOG(...) do { snprintf(txbuf, sizeof txbuf, __VA_ARGS__); \
    HAL_UART_Transmit(&huart2, (uint8_t *)txbuf, strlen(txbuf), 100); } while (0)

void SensorsApp_Init(void)
{
  uint8_t addr = 0;

  DHT11_Init();
  RGB_LED_Init();
  BSP_LED_Init(LED_GREEN);
  LOG("\r\n=== TEST CAPTEURS : BH1750 (D14/D15) + LDR (A0) + DHT11 (D7) + LED RGB (A1/A2/A3) ===\r\n");

  /* --- Scan du bus I2C1 (SCL=D15/PB8, SDA=D14/PB9) --- */
  for (uint8_t a = 1; a < 128; a++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2, 10) == HAL_OK)
    {
      LOG("I2C: peripherique trouve 0x%02X\r\n", (unsigned)a);
      if (a == 0x23 || a == 0x5C) addr = (uint8_t)(a << 1);
    }
  }

  if (addr == 0)
  {
    LOG("BH1750: introuvable (verifier SCL=D15 SDA=D14 VCC=3V3 GND)\r\n");
  }
  else if (BH1750_Init(&bh, &hi2c1, addr, BH1750_CONT_H) != HAL_OK)
  {
    LOG("BH1750: echec initialisation\r\n");
  }
  else
  {
    bh_ok = 1;
    LOG("BH1750: init OK (0x%02X)\r\n", (unsigned)(addr >> 1));
  }
}

void SensorsApp_Loop(void)
{
  /* --- LED RGB : test de defilement Rouge -> Vert -> Bleu --- */
  RGB_LED_Test();

  /* --- BH1750 (GY-302) : luminosite en lux --- */
  if (bh_ok)
  {
    float lux;
    if (BH1750_ReadLux(&bh, &lux) == HAL_OK)
    {
      LOG("BH1750: lux=%5u\r\n", (unsigned)lux);
    }
    else
    {
      LOG("BH1750: erreur I2C\r\n");
    }
  }

  /* --- LDR : luminosite ambiante --- */
  uint32_t raw = 0;
  if (LDR_ReadRaw(&hadc1, &raw) == HAL_OK)
  {
    uint8_t percent = LDR_RawToPercent(raw);
    static uint8_t dark = 0;
    if (!dark && percent < 30) dark = 1;
    if ( dark && percent > 50) dark = 0;

    if (dark) BSP_LED_On(LED_GREEN); else BSP_LED_Off(LED_GREEN);
    LOG("LDR: raw=%4u  lumiere=%3u%%  %s\r\n",
        (unsigned)raw, percent, dark ? "NUIT->LED ON" : "JOUR->LED OFF");
  }
  else
  {
    LOG("LDR: erreur de lecture ADC\r\n");
  }

  /* --- DHT11 : temperature / humidite --- */
  DHT11_Data d;
  HAL_StatusTypeDef st = DHT11_Read(&d);
  if (st == HAL_OK)
  {
    LOG("DHT11: temp=%d C  humidite=%d %%\r\n",
        (int)d.temperature, (int)d.humidity);
  }
  else if (st == HAL_TIMEOUT)
  {
    LOG("DHT11: pas de reponse (verifier cablage D7/VCC/GND)\r\n");
  }
  else
  {
    LOG("DHT11: erreur de checksum\r\n");
  }

  LOG("----\r\n");
  HAL_Delay(1000);
}
