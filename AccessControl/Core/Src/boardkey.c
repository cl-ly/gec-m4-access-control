/*
 * boardkey.c - the two on-board push buttons S1 / S2
 * S1 = PA0, S2 = PE2, pressed reads 0 (internal pull-up keeps them high).
 *
 * Neither pin appears in gpio.c (the CubeMX project never configured them),
 * so this module configures them itself.  BoardKey_Scan() is polled from the
 * main loop and returns one event per physical press, because the counters
 * latch until the level has been released again.
 */
#include "boardkey.h"

static uint8_t g_cnt1 = 0u;
static uint8_t g_cnt2 = 0u;
static uint8_t g_lat1 = 0u;
static uint8_t g_lat2 = 0u;

void BoardKey_Init(void)
{
    GPIO_InitTypeDef init = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    init.Mode  = GPIO_MODE_INPUT;
    init.Pull  = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_LOW;

    init.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOA, &init);

    init.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOE, &init);

    g_cnt1 = 0u;
    g_cnt2 = 0u;
    g_lat1 = 0u;
    g_lat2 = 0u;
}

uint8_t BoardKey_Scan(void)
{
    uint8_t raw1;
    uint8_t raw2;
    uint8_t ev = 0u;

    raw1 = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_RESET) ? 1u : 0u;
    raw2 = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_2) == GPIO_PIN_RESET) ? 1u : 0u;

    if (raw1 != 0u)
    {
        if (g_cnt1 < BOARDKEY_DEBOUNCE)
        {
            g_cnt1++;
        }
    }
    else
    {
        g_cnt1 = 0u;
        g_lat1 = 0u;
    }

    if (raw2 != 0u)
    {
        if (g_cnt2 < BOARDKEY_DEBOUNCE)
        {
            g_cnt2++;
        }
    }
    else
    {
        g_cnt2 = 0u;
        g_lat2 = 0u;
    }

    if ((g_cnt1 >= BOARDKEY_DEBOUNCE) && (g_lat1 == 0u))
    {
        g_lat1 = 1u;
        ev = BOARDKEY_S1;
    }

    if ((g_cnt2 >= BOARDKEY_DEBOUNCE) && (g_lat2 == 0u))
    {
        g_lat2 = 1u;
        if (ev == 0u)
        {
            ev = BOARDKEY_S2;
        }
    }

    return ev;
}
