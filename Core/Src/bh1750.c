#include "bh1750.h"

#define CMD_POWER_ON  0x01
#define CMD_RESET     0x07

static HAL_StatusTypeDef send(BH1750_t *d, uint8_t cmd)
{
    return HAL_I2C_Master_Transmit(d->hi2c, d->addr, &cmd, 1, 100);
}

static uint8_t is_once(BH1750_Mode m)
{
    return (m & 0x20) != 0;
}

/* Valeurs maximales de la datasheet */
static uint32_t meas_time_ms(BH1750_Mode m)
{
    return (m == BH1750_CONT_L || m == BH1750_ONCE_L) ? 24 : 180;
}

HAL_StatusTypeDef BH1750_SetMode(BH1750_t *d, BH1750_Mode mode)
{
    d->mode = mode;
    if (is_once(mode)) return HAL_OK;      /* la commande est renvoyee a chaque lecture */

    HAL_StatusTypeDef st = send(d, (uint8_t)mode);
    if (st == HAL_OK) HAL_Delay(meas_time_ms(mode));
    return st;
}

HAL_StatusTypeDef BH1750_Init(BH1750_t *d, I2C_HandleTypeDef *hi2c,
                              uint8_t addr, BH1750_Mode mode)
{
    d->hi2c = hi2c;
    d->addr = addr;

    HAL_StatusTypeDef st = send(d, CMD_POWER_ON);
    if (st != HAL_OK) return st;
    st = send(d, CMD_RESET);
    if (st != HAL_OK) return st;
    return BH1750_SetMode(d, mode);
}

HAL_StatusTypeDef BH1750_ReadLux(BH1750_t *d, float *lux)
{
    HAL_StatusTypeDef st;
    uint8_t buf[2];

    if (is_once(d->mode))                  /* mode One-Time : relancer la mesure */
    {
        st = send(d, (uint8_t)d->mode);
        if (st != HAL_OK) return st;
        HAL_Delay(meas_time_ms(d->mode));
    }

    st = HAL_I2C_Master_Receive(d->hi2c, d->addr, buf, 2, 100);
    if (st != HAL_OK) return st;

    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    float div = (d->mode == BH1750_CONT_H2 || d->mode == BH1750_ONCE_H2) ? 2.4f : 1.2f;
    *lux = raw / div;
    return HAL_OK;
}
