#ifndef DHT11_H
#define DHT11_H

#include "main.h"

#define DHT11_PORT  GPIOB
#define DHT11_PIN   GPIO_PIN_0   /* PB0, broche Arduino "A3" sur la Nucleo (CubeMX / CheatSheet) */

typedef struct {
  float humidity;     /* % HR */
  float temperature;  /* degres C */
} DHT11_Data;

/* A appeler une fois, avant la boucle (initialise le compteur de cycles DWT) */
void DHT11_Init(void);

/* Lit une trame complete (humidite + temperature). Ne pas appeler plus
 * souvent que toutes les 1 seconde (limite du capteur). */
HAL_StatusTypeDef DHT11_Read(DHT11_Data *out);

#endif /* DHT11_H */
