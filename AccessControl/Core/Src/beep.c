/*
 * 粤嵌 GEC-M4 门禁板的有源/无源蜂鸣器驱动, 挂在 P F8 / TIM13_CH1 上.
 * 纯新增模块: 只占用 P F8, 工程里其余代码在这一组端口上只用 P F9 和 P F12.
 */

#include "beep.h"

static TIM_HandleTypeDef s_htim13;
static uint8_t s_beep_ready = 0u;

/* 百分比换算成比较值: 占空比 = CCR / (ARR + 1), 因此这就是音量 */
static uint32_t Beep_Pulse(uint8_t percent)
{
    uint32_t span = (uint32_t)BEEP_ARR + 1u;

    if (percent > 100u)
    {
        percent = 100u;
    }

    return (span * (uint32_t)percent) / 100u;
}

/* P F8 复用成 TIM13_CH1 的推挽输出 */
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
    oc.Pulse      = Beep_Pulse(BEEP_VOLUME_BOOT);
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&s_htim13, &oc, TIM_CHANNEL_1) != HAL_OK)
    {
        s_beep_ready = 0u;
        return;
    }

    s_beep_ready = 1u;
}

/* 只改比较值, 不动预分频, 所以音调不变、只有音量变 */
void Beep_SetVolume(uint8_t percent)
{
    if (s_beep_ready == 0u)
    {
        return;
    }

    __HAL_TIM_SET_COMPARE(&s_htim13, TIM_CHANNEL_1, Beep_Pulse(percent));
}

/* 音调由预分频决定, 占空比 (CCR / ARR+1) 仍然代表音量.
   tone = 84 MHz / (PSC + 1) / (ARR + 1)  ->  PSC = 84 MHz / tone / (ARR+1) - 1 */
void Beep_SetTone(uint16_t hz)
{
    uint32_t psc;

    if (s_beep_ready == 0u)
    {
        return;
    }

    if (hz < 100u)
    {
        __HAL_TIM_SET_PRESCALER(&s_htim13, BEEP_PSC);
        return;
    }

    psc = 84000000u / ((uint32_t)hz * ((uint32_t)BEEP_ARR + 1u));

    if (psc == 0u)
    {
        psc = 1u;
    }

    __HAL_TIM_SET_PRESCALER(&s_htim13, psc - 1u);
}

/* 起振: 输出 PWM 方波, 无源蜂鸣器必须靠方波才响 */
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

/* 阻塞式短鸣: 内部用 HAL_Delay, 期间不返回主循环.
   只用于上电提示音和按键提示音; C4 倒计时必须用非阻塞写法, 不能用它. */
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
