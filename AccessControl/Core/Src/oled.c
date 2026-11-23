/*
 * oled.c - SSD1306 128x64 屏, 走 P8 排针上的软件 SPI
 * PD7 = SCK, PC6 = SDA(MOSI), PD6 = CS, PB7 = DC, PA4 = RES
 * 这几个引脚在 MX_GPIO_Init() 里已经配置成推挽输出.
 */
#include "oled.h"
#include "oledfont.h"

#define OLED_SCK_SET()   (GPIOD->BSRR = (uint32_t)GPIO_PIN_7)
#define OLED_SCK_CLR()   (GPIOD->BSRR = ((uint32_t)GPIO_PIN_7) << 16)
#define OLED_SDA_SET()   (GPIOC->BSRR = (uint32_t)GPIO_PIN_6)
#define OLED_SDA_CLR()   (GPIOC->BSRR = ((uint32_t)GPIO_PIN_6) << 16)
#define OLED_CS_SET()    (GPIOD->BSRR = (uint32_t)GPIO_PIN_6)
#define OLED_CS_CLR()    (GPIOD->BSRR = ((uint32_t)GPIO_PIN_6) << 16)
#define OLED_DC_SET()    (GPIOB->BSRR = (uint32_t)GPIO_PIN_7)
#define OLED_DC_CLR()    (GPIOB->BSRR = ((uint32_t)GPIO_PIN_7) << 16)
#define OLED_RES_SET()   (GPIOA->BSRR = (uint32_t)GPIO_PIN_4)
#define OLED_RES_CLR()   (GPIOA->BSRR = ((uint32_t)GPIO_PIN_4) << 16)

#define OLED_WIDTH       128u
#define OLED_PAGES       8u

/* 这块屏规格上最高只支持 10 MHz, 而软件翻转的循环原本一点延时都不加
   (168 MHz 主频下相当于大约 30 MHz). 那个速度下边沿勉强, 偶尔会写坏一个
   命令字节, 列地址指针随即跳位, 文字就变成一堆错乱的字块 —— 距离页每次
   测量都重画时最明显. 每个边沿加一点建立时间, 总线就降到约 2 MHz. */
static void OLED_BitDelay(void)
{
    volatile uint8_t n = 6u;

    while (n != 0u)
    {
        n--;
    }
}

static void OLED_WrByte(uint8_t dat)
{
    uint8_t i;

    for (i = 0; i < 8u; i++)
    {
        OLED_SCK_CLR();              /* 时钟拉低: 这段允许数据变化 */
        OLED_BitDelay();

        if (dat & 0x80u)
        {
            OLED_SDA_SET();
        }
        else
        {
            OLED_SDA_CLR();
        }
        OLED_BitDelay();

        OLED_SCK_SET();              /* 上升沿: 屏在这一刻锁存该位 */
        OLED_BitDelay();

        dat = (uint8_t)(dat << 1);
    }
}

static void OLED_WrCmd(uint8_t cmd)
{
    OLED_DC_CLR();
    OLED_CS_CLR();
    OLED_WrByte(cmd);
    OLED_CS_SET();
}

static void OLED_WrData(uint8_t dat)
{
    OLED_DC_SET();
    OLED_CS_CLR();
    OLED_WrByte(dat);
    OLED_CS_SET();
}

static void OLED_SetPos(uint8_t x, uint8_t page)
{
    OLED_WrCmd((uint8_t)(0xB0u + (page & 0x07u)));
    OLED_WrCmd((uint8_t)(0x10u | ((x & 0xF0u) >> 4)));
    OLED_WrCmd((uint8_t)(0x00u | (x & 0x0Fu)));
}

void OLED_Init(void)
{
    OLED_CS_SET();
    OLED_DC_SET();
    OLED_SCK_SET();

    OLED_RES_CLR();
    HAL_Delay(20);
    OLED_RES_SET();
    HAL_Delay(20);

    OLED_WrCmd(0xAEu);           /* 关闭显示 */
    OLED_WrCmd(0x20u);           /* 设置内存寻址模式 */
    OLED_WrCmd(0x02u);           /* 页寻址 */
    OLED_WrCmd(0xB0u);           /* 起始页 0 */
    OLED_WrCmd(0xC8u);           /* COM 扫描方向取反 */
    OLED_WrCmd(0x00u);           /* 列地址低 4 位为 0 */
    OLED_WrCmd(0x10u);           /* 列地址高 4 位为 0 */
    OLED_WrCmd(0x40u);           /* 起始行 0 */
    OLED_WrCmd(0x81u);           /* 对比度 */
    OLED_WrCmd(0xCFu);
    OLED_WrCmd(0xA1u);           /* 段重映射 */
    OLED_WrCmd(0xA6u);           /* 正常显示, 不反色 */
    OLED_WrCmd(0xA8u);           /* 复用率 */
    OLED_WrCmd(0x3Fu);
    OLED_WrCmd(0xA4u);           /* 输出跟随 GDDRAM */
    OLED_WrCmd(0xD3u);           /* 显示偏移 */
    OLED_WrCmd(0x00u);
    OLED_WrCmd(0xD5u);           /* 时钟分频 */
    OLED_WrCmd(0x80u);           /* 内部时钟取慢档: 超声波在同一条 5 V 电源轨上
                                    发射 40 kHz 脉冲时, 屏仍有余量守住时序 */
    OLED_WrCmd(0xD9u);           /* 预充电周期 */
    OLED_WrCmd(0xF1u);
    OLED_WrCmd(0xDAu);           /* COM 引脚配置 */
    OLED_WrCmd(0x12u);
    OLED_WrCmd(0xDBu);           /* VCOMH 电压 */
    OLED_WrCmd(0x40u);
    OLED_WrCmd(0x8Du);           /* 打开电荷泵 */
    OLED_WrCmd(0x14u);
    OLED_WrCmd(0xAFu);           /* 打开显示 */

    OLED_Clear();
}

void OLED_Clear(void)
{
    OLED_ClearArea(0, 0, (uint8_t)(OLED_WIDTH - 1u), (uint8_t)(OLED_PAGES - 1u));
}

void OLED_ClearArea(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    uint8_t page;
    uint8_t x;

    if (x1 >= OLED_WIDTH)
    {
        x1 = (uint8_t)(OLED_WIDTH - 1u);
    }
    if (y1 >= OLED_PAGES)
    {
        y1 = (uint8_t)(OLED_PAGES - 1u);
    }
    if (x0 > x1 || y0 > y1)
    {
        return;
    }

    for (page = y0; page <= y1; page++)
    {
        OLED_SetPos(x0, page);
        for (x = x0; x <= x1; x++)
        {
            OLED_WrData(0x00u);
        }
    }
}

void OLED_DisplayOn(void)
{
    OLED_WrCmd(0xAFu);
}

void OLED_DisplayOff(void)
{
    OLED_WrCmd(0xAEu);
}

void OLED_ShowChar(uint8_t x, uint8_t y, char ch)
{
    uint8_t idx;
    uint8_t page;
    uint8_t i;

    if (ch < ' ' || ch > '~')
    {
        return;
    }
    if (x > (uint8_t)(OLED_WIDTH - 8u) || y > (uint8_t)(OLED_PAGES - 2u))
    {
        return;
    }

    idx = (uint8_t)(ch - ' ');
    for (page = 0; page < 2u; page++)
    {
        OLED_SetPos(x, (uint8_t)(y + page));
        for (i = 0; i < 8u; i++)
        {
            OLED_WrData(OLED_F8x16[idx][page * 8u + i]);
        }
    }
}

void OLED_ShowString(uint8_t x, uint8_t y, const char *str)
{
    if (str == 0)
    {
        return;
    }

    while (*str != '\0')
    {
        if (x > (uint8_t)(OLED_WIDTH - 8u))
        {
            break;
        }
        OLED_ShowChar(x, y, *str);
        x = (uint8_t)(x + 8u);
        str++;
    }
}

void OLED_ShowCN16(uint8_t x, uint8_t y, uint8_t idx)
{
    uint8_t page;
    uint8_t i;

    if (idx >= OLED_CN16_NUM)
    {
        return;
    }
    if (x > (uint8_t)(OLED_WIDTH - 16u) || y > (uint8_t)(OLED_PAGES - 2u))
    {
        return;
    }

    for (page = 0; page < 2u; page++)
    {
        OLED_SetPos(x, (uint8_t)(y + page));
        for (i = 0; i < 16u; i++)
        {
            OLED_WrData(OLED_CN16[idx].dat[page * 16u + i]);
        }
    }
}

void OLED_ShowCNSeq(uint8_t x, uint8_t y, const uint8_t *seq, uint8_t num)
{
    uint8_t i;

    if (seq == 0)
    {
        return;
    }

    for (i = 0; i < num; i++)
    {
        if (x > (uint8_t)(OLED_WIDTH - 16u))
        {
            break;
        }
        OLED_ShowCN16(x, y, seq[i]);
        x = (uint8_t)(x + 16u);
    }
}
