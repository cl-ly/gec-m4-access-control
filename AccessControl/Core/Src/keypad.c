/*
 * keypad.c - 4x4 passive membrane keypad on the P8 header
 *
 * The pad is a passive matrix: pressing a key only shorts two of the eight
 * wires.  The driver never decides which wire is a row and which is a
 * column.  Each line is briefly driven low (open drain) while the other
 * seven stay pulled-up inputs, so a pressed key is reported as the shorted
 * pair of lines, whichever side of the matrix those two wires belong to.
 *
 * The pair of every printed key ships as the measured factory table below, so
 * the pad answers keys on the first power-up with no teaching step.  A table
 * left in flash by an earlier firmware still wins, which keeps a pad that was
 * plugged in the other way around usable without touching this source file.
 */
#include "keypad.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

#define KP_NUM_LINES   8u
#define KP_NUM_KEYS    16u

/*
 * STM32F407ZET6 has 512 KB of flash, 0x08000000..0x0807FFFF, so the last
 * sector that really exists is sector 7 at 0x08060000.  Sector 11 starts at
 * 0x080E0000 and only exists on the 1 MB parts (F407ZG / F407IG); pointing
 * the table there makes the erase fail and the learned table vanish on every
 * power cycle.  The code is about 11 KB, so the tail of sector 7 is free.
 */
#define KP_SAVE_ADDR   0x0807C000u

/*
 * The magic carries a format version.  Earlier firmware wrote a table built
 * from a guessed default mapping into the same place; bumping the magic makes
 * that stale table unreadable, so the board calibrates from an empty table
 * instead of silently loading wrong labels again.
 */
#define KP_SAVE_MAGIC  0x4B503032u

typedef struct
{
    GPIO_TypeDef *port;
    uint16_t      pin;
} KP_LINE;

/* the eight wires of the pad, in the order they sit on the plug */
static const KP_LINE KP_PIN[KP_NUM_LINES] =
{
    { GPIOG, GPIO_PIN_15 },
    { GPIOC, GPIO_PIN_7  },
    { GPIOC, GPIO_PIN_9  },
    { GPIOG, GPIO_PIN_9  },
    { GPIOB, GPIO_PIN_6  },
    { GPIOE, GPIO_PIN_6  },
    { GPIOA, GPIO_PIN_8  },
    { GPIOC, GPIO_PIN_8  }
};

/* label of each key slot, in the order the pair table below lists them */
static const char KP_LABEL[KP_NUM_KEYS] =
{
    '1', '2', '3', 'A', '4', '5', '6', 'B',
    '7', '8', '9', 'C', '*', '0', '#', 'D'
};

static uint8_t g_a[KP_NUM_KEYS];
static uint8_t g_b[KP_NUM_KEYS];
static uint8_t g_valid = 0u;

/*
 * 当前键盘实测线对表：四行线为 0、1、2、4，四列线为 6、5、7、3。
 * Flash 没有保存表时使用这张表，避免上电后键值错位。
 */
static const uint8_t KP_DEFAULT[KP_NUM_KEYS][2] =
{
    { 0u, 6u }, { 0u, 5u }, { 0u, 7u }, { 0u, 3u },
    { 1u, 6u }, { 1u, 5u }, { 1u, 7u }, { 1u, 3u },
    { 2u, 6u }, { 2u, 5u }, { 2u, 7u }, { 2u, 3u },
    { 4u, 6u }, { 4u, 5u }, { 4u, 7u }, { 4u, 3u }
};

static void KP_UseDefaults(void)
{
    uint8_t i;

    for (i = 0u; i < KP_NUM_KEYS; i++)
    {
        g_a[i] = KP_DEFAULT[i][0];
        g_b[i] = KP_DEFAULT[i][1];
    }
    g_valid = 1u;
}

static void KP_AsInput(uint8_t idx)
{
    GPIO_InitTypeDef init = {0};

    init.Pin   = KP_PIN[idx].pin;
    init.Mode  = GPIO_MODE_INPUT;
    init.Pull  = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(KP_PIN[idx].port, &init);
}

static void KP_DriveLow(uint8_t idx)
{
    GPIO_InitTypeDef init = {0};

    /* preload the latch low so the pin never glitches high when it switches */
    HAL_GPIO_WritePin(KP_PIN[idx].port, KP_PIN[idx].pin, GPIO_PIN_RESET);

    init.Pin   = KP_PIN[idx].pin;
    init.Mode  = GPIO_MODE_OUTPUT_OD;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(KP_PIN[idx].port, &init);
}

static void KP_Settle(void)
{
    volatile uint32_t n = 200u;

    while (n != 0u)
    {
        n--;
    }
}

static uint8_t KP_FindPair(uint8_t a, uint8_t b, uint8_t skip)
{
    uint8_t i;

    for (i = 0u; i < KP_NUM_KEYS; i++)
    {
        if (i == skip)
        {
            continue;
        }
        if ((g_a[i] == a) && (g_b[i] == b))
        {
            return i;
        }
    }

    return 0xFFu;
}

uint8_t Keypad_RawPair(uint8_t *a, uint8_t *b)
{
    uint8_t drive;
    uint8_t read;
    uint8_t found = 0u;

    for (drive = 0u; (drive < KP_NUM_LINES) && (found == 0u); drive++)
    {
        KP_DriveLow(drive);
        KP_Settle();

        for (read = 0u; read < KP_NUM_LINES; read++)
        {
            if (read == drive)
            {
                continue;
            }
            if (HAL_GPIO_ReadPin(KP_PIN[read].port, KP_PIN[read].pin) == GPIO_PIN_RESET)
            {
                if (drive < read)
                {
                    *a = drive;
                    *b = read;
                }
                else
                {
                    *a = read;
                    *b = drive;
                }
                found = 1u;
                break;
            }
        }

        KP_AsInput(drive);
    }

    return found;
}

void Keypad_ResetPairs(void)
{
    uint8_t i;

    for (i = 0u; i < KP_NUM_KEYS; i++)
    {
        g_a[i] = 0xFFu;
        g_b[i] = 0xFFu;
    }
    g_valid = 0u;
}

static uint8_t KP_LoadPairs(void)
{
    const volatile uint32_t *src = (const volatile uint32_t *)KP_SAVE_ADDR;
    uint8_t i;
    uint8_t byte;

    if (src[0] != KP_SAVE_MAGIC)
    {
        return 0u;
    }

    for (i = 0u; i < KP_NUM_KEYS; i++)
    {
        byte = (uint8_t)((src[1u + (i >> 2)] >> (8u * (i & 3u))) & 0xFFu);

        g_a[i] = (uint8_t)(byte >> 4);
        g_b[i] = (uint8_t)(byte & 0x0Fu);

        if ((g_a[i] >= KP_NUM_LINES) || (g_b[i] >= KP_NUM_LINES) || (g_a[i] == g_b[i]))
        {
            Keypad_ResetPairs();
            return 0u;
        }
    }

    g_valid = 1u;

    return 1u;
}

char Keypad_Scan(void)
{
    uint8_t a  = 0u;
    uint8_t b  = 0u;
    uint8_t a2 = 0u;
    uint8_t b2 = 0u;
    uint8_t idx;
    char    key;

    if (g_valid == 0u)
    {
        return 0;
    }

    if (Keypad_RawPair(&a, &b) == 0u)
    {
        return 0;
    }

    idx = KP_FindPair(a, b, 0xFFu);
    if (idx >= KP_NUM_KEYS)
    {
        return 0;
    }
    key = KP_LABEL[idx];

    HAL_Delay(KEYPAD_DEBOUNCE_MS);

    if (Keypad_RawPair(&a2, &b2) == 0u)
    {
        return 0;
    }
    if ((a2 != a) || (b2 != b))
    {
        return 0;
    }

    while (Keypad_RawPair(&a2, &b2) != 0u)
    {
        HAL_Delay(KEYPAD_RELEASE_MS);
    }

    return key;
}

void Keypad_Init(void)
{
    uint8_t i;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    for (i = 0u; i < KP_NUM_LINES; i++)
    {
        KP_AsInput(i);
    }

    Keypad_ResetPairs();

    /* a table learned earlier wins; otherwise the measured factory table is
       installed so the pad answers keys right away, with no calibration UI */
    if (KP_LoadPairs() == 0u)
    {
        KP_UseDefaults();
    }
}
