#ifndef RGB_LED_H
#define RGB_LED_H

#include "main.h"

/* Cathode commune : 1 = allume, 0 = eteint.
 * Si la LED est a anode commune, inverser cette macro. */
#define RGB_LED_COMMON_CATHODE 0   /* LED du kit testée le 18/10/2026 : anode commune (logique inversée) */

/* Brochage CubeMX / CheatSheet : R = PA0 (A0), V = PA1 (A1), B = PA4 (A2) */
#define RGB_LED_R_PORT  GPIOA
#define RGB_LED_R_PIN   GPIO_PIN_0
#define RGB_LED_G_PORT  GPIOA
#define RGB_LED_G_PIN   GPIO_PIN_1
#define RGB_LED_B_PORT  GPIOA
#define RGB_LED_B_PIN   GPIO_PIN_4

/* A appeler une fois, avant la boucle */
void RGB_LED_Init(void);

/* Allume/eteint chaque canal independamment */
void RGB_LED_Set(uint8_t r, uint8_t g, uint8_t b);

/* Test : fait defiler Rouge -> Vert -> Bleu (300ms chacune) */
void RGB_LED_Test(void);

#endif /* RGB_LED_H */
