#ifndef BH1750_H
#define BH1750_H

#include "main.h"                       /* inclut le HAL */

#define BH1750_ADDR_LOW   (0x23 << 1)   /* ADDR = GND ou libre */
#define BH1750_ADDR_HIGH  (0x5C << 1)   /* ADDR = VCC */

typedef enum {
    BH1750_CONT_H  = 0x10,   /* 1 lx,   ~120 ms, continu */
    BH1750_CONT_H2 = 0x11,   /* 0.5 lx, ~120 ms, continu */
    BH1750_CONT_L  = 0x13,   /* 4 lx,   ~16 ms,  continu */
    BH1750_ONCE_H  = 0x20,   /* 1 lx,   puis Power Down automatique */
    BH1750_ONCE_H2 = 0x21,
    BH1750_ONCE_L  = 0x23
} BH1750_Mode;

typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint8_t            addr;   /* adresse 8 bits (deja decalee) */
    BH1750_Mode        mode;
} BH1750_t;

HAL_StatusTypeDef BH1750_Init   (BH1750_t *d, I2C_HandleTypeDef *hi2c,
                                 uint8_t addr, BH1750_Mode mode);
HAL_StatusTypeDef BH1750_SetMode(BH1750_t *d, BH1750_Mode mode);
HAL_StatusTypeDef BH1750_ReadLux(BH1750_t *d, float *lux);

#endif /* BH1750_H */
