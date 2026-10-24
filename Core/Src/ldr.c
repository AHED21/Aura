#include "ldr.h"

HAL_StatusTypeDef LDR_ReadRaw(ADC_HandleTypeDef *hadc, uint32_t *raw)
{
  if (HAL_ADC_Start(hadc) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_ADC_PollForConversion(hadc, 10) != HAL_OK)
  {
    HAL_ADC_Stop(hadc);
    return HAL_TIMEOUT;
  }
  *raw = HAL_ADC_GetValue(hadc);
  HAL_ADC_Stop(hadc);
  return HAL_OK;
}

uint8_t LDR_RawToPercent(uint32_t raw)
{
  if (raw > 4095U) raw = 4095U;

  /* Sur la plupart des modules LDR (diviseur de tension), "out" monte
   * quand il fait sombre et descend quand il y a de la lumiere.
   * Si le comportement est invers sur ton module, enleve le "100 -". */
  return (uint8_t)(100U - (raw * 100U) / 4095U);
}
