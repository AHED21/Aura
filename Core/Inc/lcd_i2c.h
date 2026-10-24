#ifndef LCD_I2C_H
#define LCD_I2C_H

#include "main.h"                       /* inclut le HAL */

/* Écran LCD1602 avec module I2C PCF8574 au dos.
 * Adresse 7 bits : 0x27 (PCF8574T) ou 0x3F (PCF8574AT). */
#define LCD_ADDR_27  (0x27 << 1)
#define LCD_ADDR_3F  (0x3F << 1)

typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint8_t            addr;            /* adresse 8 bits (deja decalee) */
    uint8_t            backlight;       /* 0x08 = retroeclairage allume */
    uint8_t            erreur;          /* 1 si un echange I2C a echoue depuis la derniere init */
} LCD_I2C_t;

HAL_StatusTypeDef LCD_Init  (LCD_I2C_t *lcd, I2C_HandleTypeDef *hi2c, uint8_t addr);
void              LCD_Clear (LCD_I2C_t *lcd);
void              LCD_Resync(LCD_I2C_t *lcd);   /* recale le mode 4 bits sans effacer */
void              LCD_SetCursor(LCD_I2C_t *lcd, uint8_t col, uint8_t row);   /* col 0..15, row 0..1 */
void              LCD_Print (LCD_I2C_t *lcd, const char *txt);

#endif /* LCD_I2C_H */
