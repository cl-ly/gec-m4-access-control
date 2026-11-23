/*
 * hc_sr04.c - 用普通 GPIO 驱动 HC-SR04 超声波测距
 * Trig = PE12 (丝印 D9), Echo = PE14 (丝印 D11), VCC = 5 V, GND = GND.
 *
 * MX_TIM1_Init() 已经把 PSC 设成 167, 于是 TIM1 以 1 MHz 计数, 其计数器正好
 * 当作自由运行的微秒时基. 本模块只负责打开计数器, TIM1 的输入捕获通道不动.
 *
 * 为什么诊断量很关键: 测距突然没读数有三种完全不同的原因, 手上没有万用表时
 * 只能靠屏幕把它们区分开.
 *   hits == 0                    触发脉冲根本没送到模块
 *                                (VCC 没接 5 V, Trig 线插错孔)
 *   idle_high != 0               没人测距时回波线一直为高
 *                                (Echo 接在被上拉的引脚, 或模块没接)
 *   hits > 0 但 width_us 为 0     回波升起后到超时都没落下, 说明线接在 MCU
 *                                能看到但模块没有驱动的引脚上
 */
#include "hc_sr04.h"
#include "tim.h"

#define HCSR04_TRIG_PORT   GPIOE
#define HCSR04_TRIG_PIN    GPIO_PIN_12
#define HCSR04_ECHO_PORT   GPIOE
#define HCSR04_ECHO_PIN    GPIO_PIN_14

/* 数据手册要求 10 us; 20 us 给 HAL 调用开销留了余量 */
#define HCSR04_TRIG_US     20u
/* 模块几百微秒内就会应答, 3 ms 已经很宽裕 */
#define HCSR04_RISE_US     3000u
/* 30 ms 约合 5 m; 超过这个值就是完全没有回波 */
#define HCSR04_WIDTH_US    30000u
#define HCSR04_MIN_WIDTH_US 100u

HCSR04_Info g_sr04;

static uint16_t HCSR04_Now(void)
{
    return (uint16_t)TIM1->CNT;
}

static void HCSR04_DelayUs(uint16_t us)
{
    uint16_t start = HCSR04_Now();

    while ((uint16_t)(HCSR04_Now() - start) < us)
    {
    }
}

static void HCSR04_EchoInput(void)
{
    GPIO_InitTypeDef init = {0};

    init.Pin  = HCSR04_ECHO_PIN;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(HCSR04_ECHO_PORT, &init);
}

/* 回波线空闲时由模块推挽拉低. 这里把内部上拉打开几百微秒再读一次电平: 模块在位
   就仍然读低, 模块被拔掉或 VCC 没接, 这条线就浮空被上拉成高. 探测完立刻恢复成
   原来的下拉输入, 不影响正常测距. */
uint8_t HCSR04_Present(void)
{
    GPIO_InitTypeDef init = {0};
    GPIO_PinState    level;

    init.Pin  = HCSR04_ECHO_PIN;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(HCSR04_ECHO_PORT, &init);

    HCSR04_DelayUs(200u);

    level = HAL_GPIO_ReadPin(HCSR04_ECHO_PORT, HCSR04_ECHO_PIN);

    HCSR04_EchoInput();

    return (level == GPIO_PIN_RESET) ? 1u : 0u;
}

/* 一次没拿到有效距离: 连续失败次数加一, 到门限就用电平探测复核模块在不在 */
static void HCSR04_CountMiss(void)
{
    if (g_sr04.streak < 0xFFu)
    {
        g_sr04.streak++;
    }

    if ((g_sr04.streak >= HCSR04_ABSENT_STREAK) &&
        ((g_sr04.streak % HCSR04_ABSENT_STREAK) == 0u))
    {
        g_sr04.absent = (HCSR04_Present() != 0u) ? 0u : 1u;
    }
}

void HCSR04_Init(void)
{
    GPIO_InitTypeDef init = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    HAL_GPIO_WritePin(HCSR04_TRIG_PORT, HCSR04_TRIG_PIN, GPIO_PIN_RESET);

    init.Mode  = GPIO_MODE_OUTPUT_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    init.Pin   = HCSR04_TRIG_PIN;
    HAL_GPIO_Init(HCSR04_TRIG_PORT, &init);

    /* 回波平时为低, 模块在往返飞行期间把它拉高 */
    HCSR04_EchoInput();

    g_sr04.valid     = 0u;
    g_sr04.cm        = 0u;
    g_sr04.width_us  = 0u;
    g_sr04.idle_high = 0u;
    g_sr04.hits      = 0u;
    g_sr04.trigs     = 0u;
    g_sr04.misses    = 0u;
    g_sr04.streak    = 0u;
    g_sr04.absent    = 0u;

    /* TIM1 在这里只当 1 MHz 时基用, CH4 保持不用. 置位启动位后发一次更新
       请求, 计数器会干净地重新装载. */
    TIM1->EGR  = TIM_EGR_UG;
    TIM1->SR   = 0u;
    TIM1->CR1 |= TIM_CR1_CEN;

    HAL_Delay(20u);
}

/* 回波线仍停留在 from_level 时继续等待, 一旦变化就返回 1 */
static uint8_t HCSR04_WaitChange(GPIO_PinState from, uint16_t timeout_us,
                                 uint16_t *spent_us)
{
    uint16_t start = HCSR04_Now();
    uint16_t dt;

    while (HAL_GPIO_ReadPin(HCSR04_ECHO_PORT, HCSR04_ECHO_PIN) == from)
    {
        dt = (uint16_t)(HCSR04_Now() - start);
        if (dt > timeout_us)
        {
            *spent_us = dt;
            return 0u;
        }
    }

    *spent_us = (uint16_t)(HCSR04_Now() - start);

    return 1u;
}

uint8_t HCSR04_MeasureCm(uint16_t *cm)
{
    uint16_t spent = 0u;
    uint16_t width;

    if (cm == 0)
    {
        return 0u;
    }

    if (g_sr04.trigs < 99u)
    {
        g_sr04.trigs++;
    }

    /* 还没触发时回波线就为高, 说明这个引脚不是模块驱动的那一根 (或者模块没接):
       先给它一次落回的机会, 落不下来就把这次异常记进诊断量. */
    g_sr04.idle_high = (HAL_GPIO_ReadPin(HCSR04_ECHO_PORT, HCSR04_ECHO_PIN) == GPIO_PIN_SET)
                       ? 1u : 0u;
    if (g_sr04.idle_high != 0u)
    {
        if (HCSR04_WaitChange(GPIO_PIN_SET, HCSR04_WIDTH_US, &spent) == 0u)
        {
            g_sr04.width_us = 0u;
            g_sr04.valid    = 0u;
            /* 发触发前回波线就一直高, 而且落不下来: 没有模块在驱动这根线 */
            g_sr04.absent   = 1u;
            if (g_sr04.misses < 99u)
            {
                g_sr04.misses++;
            }
            return 0u;
        }
        g_sr04.idle_high = 0u;
    }

    /* 20 us 触发脉冲 */
    HAL_GPIO_WritePin(HCSR04_TRIG_PORT, HCSR04_TRIG_PIN, GPIO_PIN_SET);
    HCSR04_DelayUs(HCSR04_TRIG_US);
    HAL_GPIO_WritePin(HCSR04_TRIG_PORT, HCSR04_TRIG_PIN, GPIO_PIN_RESET);

    /* 等待回波升起 */
    if (HCSR04_WaitChange(GPIO_PIN_RESET, HCSR04_RISE_US, &spent) == 0u)
    {
        g_sr04.width_us = 0u;
        g_sr04.valid    = 0u;
        if (g_sr04.misses < 99u)
        {
            g_sr04.misses++;
        }
        HCSR04_CountMiss();
        return 0u;
    }

    /* 回波已经升起: 仅这一条就证明触发脉冲, VCC 和 GND 都通到了模块 */
    if (g_sr04.hits < 99u)
    {
        g_sr04.hits++;
    }

    /* 测量回波保持高电平的时长 */
    if (HCSR04_WaitChange(GPIO_PIN_SET, HCSR04_WIDTH_US, &spent) == 0u)
    {
        g_sr04.width_us = spent;
        g_sr04.valid    = 0u;
        if (g_sr04.misses < 99u)
        {
            g_sr04.misses++;
        }
        return 0u;
    }

    width = spent;
    g_sr04.width_us = width;

    /* 约 2 cm 以内脉冲太短, 不算有效读数 */
    if ((width < HCSR04_MIN_WIDTH_US) || (width > HCSR04_WIDTH_US))
    {
        g_sr04.valid = 0u;
        if (g_sr04.misses < 99u)
        {
            g_sr04.misses++;
        }
        return 0u;
    }

    *cm = (uint16_t)(width / 58u);

    g_sr04.cm     = *cm;
    g_sr04.valid  = 1u;
    /* 拿到一次有效回波就证明模块在位, 连续失败计数清零 */
    g_sr04.streak = 0u;
    g_sr04.absent = 0u;

    return 1u;
}
