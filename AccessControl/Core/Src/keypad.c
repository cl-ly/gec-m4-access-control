/*
 * keypad.c - 挂在 P8 排针上的 4x4 无源薄膜键盘驱动
 *
 * 这块键盘是无源矩阵: 按下一个键只是把八根线里的两根短接. 驱动不去判断哪根
 * 是行、哪根是列. 每次把一根线短暂拉低 (开漏), 其余七根保持上拉输入, 于是一次
 * 按下就被记成「被短接的那一对线」, 不管那两根线在矩阵里属于哪一侧.
 *
 * 每个印刷键位对应的线对随下面这张实测出厂表一起发布, 所以键盘上电即可用,
 * 不需要校准步骤. 早先固件留在 Flash 里的表仍然优先, 这样接反的键盘不改这份
 * 源码也能继续用.
 */
#include "keypad.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

#define KP_NUM_LINES   8u
#define KP_NUM_KEYS    16u

/*
 * STM32F407ZET6 的 Flash 是 512 KB, 地址 0x08000000..0x0807FFFF, 所以真正存在
 * 的最后一个扇区是 0x08060000 起的扇区 7. 扇区 11 从 0x080E0000 开始, 只有
 * 1 MB 的型号 (F407ZG / F407IG) 上才有; 把表指到那里会让擦除失败, 学到的表
 * 每次上电都会丢. 代码约 11 KB, 扇区 7 的尾部是空闲的.
 */
#define KP_SAVE_ADDR   0x0807C000u

/*
 * 这个 magic 里带格式版本号. 早先的固件曾把按猜测映射生成的表写进同一个
 * 位置; 提高 magic 就让那张过期表读不出来, 于是板子从空表开始, 而不是又一次
 * 悄悄加载错误的键位标签.
 */
#define KP_SAVE_MAGIC  0x4B503032u

typedef struct
{
    GPIO_TypeDef *port;
    uint16_t      pin;
} KP_LINE;

/* 键盘的八根线, 按它们在插头上的顺序排列 */
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

/* 每个键位槽的标签, 顺序与下面的线对表一致 */
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

    /* 先把锁存器预置为低, 这样引脚切换到输出时不会闪出一段高电平 */
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

    /* 早先学到的表优先; 否则装上实测的出厂表, 键盘立刻可响应, 没有校准界面 */
    if (KP_LoadPairs() == 0u)
    {
        KP_UseDefaults();
    }
}
