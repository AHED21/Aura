#include "lcd_i2c.h"

/* Câblage standard du PCF8574 vers le LCD :
 * P0 = RS, P1 = RW, P2 = EN, P3 = rétroéclairage, P4..P7 = D4..D7 (mode 4 bits) */
#define PIN_RS  0x01
#define PIN_EN  0x04
#define BL_ON   0x08

static HAL_StatusTypeDef write_byte(LCD_I2C_t *lcd, uint8_t data)
{
    data |= lcd->backlight;
    HAL_StatusTypeDef st = HAL_I2C_Master_Transmit(lcd->hi2c, lcd->addr, &data, 1, 50);
    if (st != HAL_OK) lcd->erreur = 1;   /* un demi-octet perdu décale tout l'affichage : il faudra réinitialiser */
    return st;
}

/* Envoie 4 bits (dans les bits 4..7) avec une impulsion sur EN */
static HAL_StatusTypeDef write_nibble(LCD_I2C_t *lcd, uint8_t nibble, uint8_t rs)
{
    uint8_t d = (uint8_t)((nibble & 0xF0) | rs);
    if (write_byte(lcd, d | PIN_EN) != HAL_OK) return HAL_ERROR;
    if (write_byte(lcd, d) != HAL_OK) return HAL_ERROR;
    return HAL_OK;
}

static HAL_StatusTypeDef send(LCD_I2C_t *lcd, uint8_t value, uint8_t rs)
{
    if (write_nibble(lcd, value & 0xF0, rs) != HAL_OK) return HAL_ERROR;
    if (write_nibble(lcd, (uint8_t)(value << 4), rs) != HAL_OK) return HAL_ERROR;
    return HAL_OK;
}

static void command(LCD_I2C_t *lcd, uint8_t cmd)
{
    send(lcd, cmd, 0);
    if (cmd <= 0x03) HAL_Delay(2);       /* Clear et Home sont lentes (~1,5 ms) */
}

HAL_StatusTypeDef LCD_Init(LCD_I2C_t *lcd, I2C_HandleTypeDef *hi2c, uint8_t addr)
{
    lcd->hi2c = hi2c;
    lcd->addr = addr;
    lcd->backlight = BL_ON;
    lcd->erreur = 0;

    HAL_Delay(50);                       /* le LCD a besoin de >40 ms après la mise sous tension */
    if (write_byte(lcd, 0) != HAL_OK) return HAL_ERROR;

    /* Séquence d'initialisation en mode 4 bits (datasheet HD44780) */
    write_nibble(lcd, 0x30, 0); HAL_Delay(5);
    write_nibble(lcd, 0x30, 0); HAL_Delay(1);
    write_nibble(lcd, 0x30, 0); HAL_Delay(1);
    write_nibble(lcd, 0x20, 0); HAL_Delay(1);

    command(lcd, 0x28);                  /* 4 bits, 2 lignes, police 5x8 */
    command(lcd, 0x0C);                  /* affichage ON, curseur OFF */
    command(lcd, 0x06);                  /* écriture de gauche à droite */
    LCD_Clear(lcd);
    return lcd->erreur ? HAL_ERROR : HAL_OK;
}

/* Recale le LCD en mode 4 bits sans effacer l'écran (~8 ms). Si un demi-octet a été
 * perdu ou parasité, le LCD est décalé et affiche des caractères bizarres : cette
 * séquence le remet d'aplomb quel que soit son état. */
void LCD_Resync(LCD_I2C_t *lcd)
{
    lcd->erreur = 0;
    write_nibble(lcd, 0x30, 0); HAL_Delay(5);
    write_nibble(lcd, 0x30, 0); HAL_Delay(1);
    write_nibble(lcd, 0x30, 0); HAL_Delay(1);
    write_nibble(lcd, 0x20, 0); HAL_Delay(1);
    command(lcd, 0x28);                  /* 4 bits, 2 lignes */
    command(lcd, 0x0C);                  /* affichage ON, curseur OFF */
    command(lcd, 0x06);
}

void LCD_Clear(LCD_I2C_t *lcd)
{
    command(lcd, 0x01);
}

void LCD_SetCursor(LCD_I2C_t *lcd, uint8_t col, uint8_t row)
{
    command(lcd, (uint8_t)(0x80 | ((row ? 0x40 : 0x00) + (col & 0x0F))));
}

void LCD_Print(LCD_I2C_t *lcd, const char *txt)
{
    while (*txt) send(lcd, (uint8_t)*txt++, PIN_RS);
}
