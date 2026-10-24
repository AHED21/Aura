#include "main.h"
#include "bh1750.h"
#include <stdio.h>
#include <string.h>

extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart2;

static BH1750_t bh;
static char     txbuf[80];

/* Envoie un message sur USART2 (visible dans le Serial Monitor) */
#define LOG(...) do { snprintf(txbuf, sizeof txbuf, __VA_ARGS__); \
    HAL_UART_Transmit(&huart2, (uint8_t *)txbuf, strlen(txbuf), 100); } while (0)

/* Echec : message repete + LED verte qui clignote vite */
static void test_fail(const char *msg)
{
  while (1)
  {
    LOG("%s\r\n", msg);
    for (int i = 0; i < 10; i++)
    {
      BSP_LED_Toggle(LED_GREEN);
      HAL_Delay(100);
    }
  }
}

/* A appeler UNE fois, avant la boucle */
void GY302_Test_Init(void)
{
  uint8_t addr = 0;

  BSP_LED_Init(LED_GREEN);
  LOG("\r\n=== TEST GY-302 / BH1750 ===\r\n");

  /* 1) Scan du bus I2C : cherche le capteur (0x23 ou 0x5C) */
  for (uint8_t a = 1; a < 128; a++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2, 10) == HAL_OK)
    {
      LOG("Peripherique I2C trouve : 0x%02X\r\n", (unsigned)a);
      if (a == 0x23 || a == 0x5C) addr = (uint8_t)(a << 1);
    }
  }
  if (addr == 0)
  {
    test_fail("ECHEC : BH1750 introuvable (SCL=D15 SDA=D14 VCC=3V3 GND)");
  }

  /* 2) Initialisation du capteur */
  if (BH1750_Init(&bh, &hi2c1, addr, BH1750_CONT_H) != HAL_OK)
  {
    test_fail("ECHEC : initialisation BH1750");
  }
  LOG("Init OK\r\n");
}

/* A appeler dans la boucle while(1) */
void GY302_Test_Loop(void)
{
  float lux;

  if (BH1750_ReadLux(&bh, &lux) == HAL_OK)
  {
    /* Veilleuse : LED allumee quand il fait sombre (30 / 60 lux) */
    static uint8_t dark = 0;
    if (!dark && lux < 30.0f) dark = 1;
    if ( dark && lux > 60.0f) dark = 0;

    if (dark) BSP_LED_On(LED_GREEN); else BSP_LED_Off(LED_GREEN);
    LOG("lux=%5u   %s\r\n", (unsigned)lux, dark ? "NUIT -> LED ON" : "JOUR -> LED OFF");
  }
  else
  {
    LOG("Erreur I2C\r\n");
  }
  HAL_Delay(300);
}