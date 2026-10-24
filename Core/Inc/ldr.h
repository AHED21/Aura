#ifndef LDR_H
#define LDR_H

#include "main.h"

/* Lit une conversion brute sur la voie ADC configuree pour la LDR (0..4095) */
HAL_StatusTypeDef LDR_ReadRaw(ADC_HandleTypeDef *hadc, uint32_t *raw);

/* Convertit la valeur brute en pourcentage de luminosite (0 = sombre, 100 = lumineux) */
uint8_t LDR_RawToPercent(uint32_t raw);

#endif /* LDR_H */
