/*
 * Passive buzzer on P F8 / TIM13_CH1 for the GEC-M4 access control board.
 * Additive module: it only claims P F8, which the rest of this project leaves
 * free (gpio.c uses P F9 and P F12 on that port).
 */

#include "beep.h"

static TIM_HandleTypeDef s_htim13;
static uint8_t s_beep_ready = 0u;

static uint32_t Beep_Pulse(uint8_t percent)
{
    uint32_t span = (uint32_t)BEEP_ARR + 1u;

    if (percent > 100u)
    {
        percent = 100u;
    }

    return (span * (uint32_t)percent) / 100u;
}

static void Beep_GpioInit(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOF_CLK_ENABLE();

    gpio.Pin       = BEEP_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = BEEP_AF;
    HAL_GPIO_Init(BEEP_PORT, &gpio);
}

void Beep_Init(void)
{
    TIM_OC_InitTypeDef oc = {0};

    __HAL_RCC_TIM13_CLK_ENABLE();
    Beep_GpioInit();

    s_htim13.Instance               = TIM13;
    s_htim13.Init.Prescaler         = BEEP_PSC;
    s_htim13.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim13.Init.Period            = BEEP_ARR;
    s_htim13.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim13.Init.RepetitionCounter = 0u;
    s_htim13.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(&s_htim13) != HAL_OK)
    {
        s_beep_ready = 0u;
        return;
    }

    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = Beep_Pulse(10u);
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&s_htim13, &oc, TIM_CHANNEL_1) != HAL_OK)
    {
        s_beep_ready = 0u;
        return;
    }

    s_beep_ready = 1u;
}

void Beep_SetVolume(uint8_t percent)
{
    if (s_beep_ready == 0u)
    {
        return;
    }

    __HAL_TIM_SET_COMPARE(&s_htim13, TIM_CHANNEL_1, Beep_Pulse(percent));
}

void Beep_On(void)
{
    if (s_beep_ready == 0u)
    {
        return;
    }

    (void)HAL_TIM_PWM_Start(&s_htim13, TIM_CHANNEL_1);
}

void Beep_Off(void)
{
    if (s_beep_ready == 0u)
    {
        return;
    }

    (void)HAL_TIM_PWM_Stop(&s_htim13, TIM_CHANNEL_1);
}

void Beep_BeepMs(uint32_t ms)
{
    if (s_beep_ready == 0u)
    {
        return;
    }

    Beep_On();
    HAL_Delay(ms);
    Beep_Off();
}
