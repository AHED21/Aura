#include "rgb_led.h"

static void set_pin(GPIO_TypeDef *port, uint16_t pin, uint8_t on)
{
#if RGB_LED_COMMON_CATHODE
  HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

void RGB_LED_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  GPIO_InitStruct.Pin = RGB_LED_R_PIN;
  HAL_GPIO_Init(RGB_LED_R_PORT, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = RGB_LED_G_PIN;
  HAL_GPIO_Init(RGB_LED_G_PORT, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = RGB_LED_B_PIN;
  HAL_GPIO_Init(RGB_LED_B_PORT, &GPIO_InitStruct);

  RGB_LED_Set(0, 0, 0);
}

void RGB_LED_Set(uint8_t r, uint8_t g, uint8_t b)
{
  set_pin(RGB_LED_R_PORT, RGB_LED_R_PIN, r);
  set_pin(RGB_LED_G_PORT, RGB_LED_G_PIN, g);
  set_pin(RGB_LED_B_PORT, RGB_LED_B_PIN, b);
}

void RGB_LED_Test(void)
{
  RGB_LED_Set(1, 0, 0);
  HAL_Delay(300);
  RGB_LED_Set(0, 1, 0);
  HAL_Delay(300);
  RGB_LED_Set(0, 0, 1);
  HAL_Delay(300);
  RGB_LED_Set(0, 0, 0);
}
