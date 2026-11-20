/*
 * hc_sr04.c - HC-SR04 ultrasonic ranger on plain GPIO
 * Trig = PE12 (decal D9), Echo = PE14 (decal D11), VCC = 5 V, GND = GND.
 *
 * MX_TIM1_Init() already sets PSC = 167, so TIM1 counts at 1 MHz and its
 * counter works as a free running microsecond time base.  This module only
 * switches the counter on; the capture channel of TIM1 stays untouched.
 *
 * Why the diagnostics matter: a silent ranger has three very different causes
 * and the screen has to tell them apart without a meter.
 *   hits == 0                    the trigger pulse never reaches the module
 *                                (VCC not on 5 V, Trig wire on a wrong hole)
 *   idle_high != 0               the echo line sits high while nobody pings it
 *                                (Echo wire on a pulled-up pin, or no module)
 *   hits > 0 but width_us 0      the echo rises but never falls inside the
 *                                timeout, which means the wire is on a pin the
 *                                MCU can see but the module is not driving
 */
#include "hc_sr04.h"
#include "tim.h"

#define HCSR04_TRIG_PORT   GPIOE
#define HCSR04_TRIG_PIN    GPIO_PIN_12
#define HCSR04_ECHO_PORT   GPIOE
#define HCSR04_ECHO_PIN    GPIO_PIN_14

/* the datasheet asks for 10 us; 20 us leaves room for the HAL call overhead */
#define HCSR04_TRIG_US     20u
/* the module answers within a few hundred us, 3 ms is very generous */
#define HCSR04_RISE_US     3000u
/* 30 ms is roughly 5 m; anything longer means no echo came back at all */
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

    /* echo idles low, the module drives it high for the round trip time */
    HCSR04_EchoInput();

    g_sr04.valid     = 0u;
    g_sr04.cm        = 0u;
    g_sr04.width_us  = 0u;
    g_sr04.idle_high = 0u;
    g_sr04.hits      = 0u;
    g_sr04.trigs     = 0u;
    g_sr04.misses    = 0u;

    /* TIM1 is used as a 1 MHz time base only, CH4 stays unused.  The update
       request reloads the counter cleanly after the start bit is set. */
    TIM1->EGR  = TIM_EGR_UG;
    TIM1->SR   = 0u;
    TIM1->CR1 |= TIM_CR1_CEN;

    HAL_Delay(20u);
}

/* waits while the echo line still shows from_level, returns 1 when it moved */
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

    /* A line that sits high before any trigger means this pin is not the one
       the module drives (or the module is missing): give it one chance to
       drop back, then report it. */
    g_sr04.idle_high = (HAL_GPIO_ReadPin(HCSR04_ECHO_PORT, HCSR04_ECHO_PIN) == GPIO_PIN_SET)
                       ? 1u : 0u;
    if (g_sr04.idle_high != 0u)
    {
        if (HCSR04_WaitChange(GPIO_PIN_SET, HCSR04_WIDTH_US, &spent) == 0u)
        {
            g_sr04.width_us = 0u;
            g_sr04.valid    = 0u;
            if (g_sr04.misses < 99u)
            {
                g_sr04.misses++;
            }
            return 0u;
        }
        g_sr04.idle_high = 0u;
    }

    /* 20 us trigger pulse */
    HAL_GPIO_WritePin(HCSR04_TRIG_PORT, HCSR04_TRIG_PIN, GPIO_PIN_SET);
    HCSR04_DelayUs(HCSR04_TRIG_US);
    HAL_GPIO_WritePin(HCSR04_TRIG_PORT, HCSR04_TRIG_PIN, GPIO_PIN_RESET);

    /* wait for the echo to rise */
    if (HCSR04_WaitChange(GPIO_PIN_RESET, HCSR04_RISE_US, &spent) == 0u)
    {
        g_sr04.width_us = 0u;
        g_sr04.valid    = 0u;
        if (g_sr04.misses < 99u)
        {
            g_sr04.misses++;
        }
        return 0u;
    }

    /* the echo rose: this alone proves the trigger, VCC and GND got through */
    if (g_sr04.hits < 99u)
    {
        g_sr04.hits++;
    }

    /* measure how long the echo stays high */
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

    /* under about 2 cm the pulse is too short to be a real reading */
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

    g_sr04.cm    = *cm;
    g_sr04.valid = 1u;

    return 1u;
}
