#include "dht11.h"

/* ---- Delai microseconde base sur le compteur de cycles DWT (Cortex-M4) ---- */

void DHT11_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < cycles)
  {
  }
}

/* ---- Bascule la broche entre sortie (pour envoyer le start) et entree ---- */

static void DHT11_SetOutput(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin   = DHT11_PIN;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT11_PORT, &GPIO_InitStruct);
}

static void DHT11_SetInput(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin  = DHT11_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(DHT11_PORT, &GPIO_InitStruct);
}

/* Attend que la broche passe a l'etat "level". Retourne le nombre de
 * microsecondes attendues, ou 0xFFFFFFFF en cas de timeout. */
static uint32_t DHT11_WaitForState(GPIO_PinState level, uint32_t timeout_us)
{
  /* Durée mesurée avec le compteur de cycles DWT : compter les tours de boucle
   * sous-estime le temps (surtout en Debug -O0) et fait lire tous les bits à 0. */
  const uint32_t cycles_par_us = SystemCoreClock / 1000000U;
  const uint32_t start = DWT->CYCCNT;
  while (HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) != level)
  {
    if ((DWT->CYCCNT - start) > timeout_us * cycles_par_us)
    {
      return 0xFFFFFFFFU;
    }
  }
  return (DWT->CYCCNT - start) / cycles_par_us;
}

HAL_StatusTypeDef DHT11_Read(DHT11_Data *out)
{
  uint8_t bits[5] = {0, 0, 0, 0, 0};

  /* 1) Signal de depart : broche basse >= 18 ms, puis haute 20-40 us */
  DHT11_SetOutput();
  HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_RESET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
  delay_us(30);
  DHT11_SetInput();

  /* 2) Reponse du capteur : 80us bas puis 80us haut */
  if (DHT11_WaitForState(GPIO_PIN_RESET, 100) == 0xFFFFFFFFU) return HAL_TIMEOUT;
  if (DHT11_WaitForState(GPIO_PIN_SET,   100) == 0xFFFFFFFFU) return HAL_TIMEOUT;
  if (DHT11_WaitForState(GPIO_PIN_RESET, 100) == 0xFFFFFFFFU) return HAL_TIMEOUT;

  /* 3) 40 bits de donnees : chaque bit commence par 50us bas, puis
   *    26-28us haut = 0, ~70us haut = 1 */
  for (int i = 0; i < 40; i++)
  {
    if (DHT11_WaitForState(GPIO_PIN_SET, 100) == 0xFFFFFFFFU) return HAL_TIMEOUT;

    uint32_t high_us = DHT11_WaitForState(GPIO_PIN_RESET, 100);
    if (high_us == 0xFFFFFFFFU) return HAL_TIMEOUT;

    bits[i / 8] <<= 1;
    if (high_us > 40U)
    {
      bits[i / 8] |= 1U;
    }
  }

  /* 4) Verification du checksum */
  uint8_t checksum = (uint8_t)(bits[0] + bits[1] + bits[2] + bits[3]);
  if (checksum != bits[4])
  {
    return HAL_ERROR;
  }
  /* Une trame entièrement à 0 passe le checksum mais n'est pas une vraie mesure (humidité 0 % impossible). */
  if (bits[0] == 0 && bits[2] == 0)
  {
    return HAL_ERROR;
  }

  out->humidity    = (float)bits[0];
  out->temperature = (float)bits[2];
  return HAL_OK;
}
