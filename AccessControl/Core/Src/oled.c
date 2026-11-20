/*
 * oled.c - SSD1306 128x64, software SPI on P8 header
 * PD7 = SCK, PC6 = SDA(MOSI), PD6 = CS, PB7 = DC, PA4 = RES
 * Pins are already configured as push-pull outputs by MX_GPIO_Init().
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

/* The panel is specified for 10 MHz at most, while the bit-banged loop used
   to run with no delay at all (roughly 30 MHz at 168 MHz core clock).  At that
   speed a marginal edge corrupted a command byte now and then, the column
   address pointer jumped and the text turned into scrambled glyphs - most
   visibly while the distance page was rewritten on every measurement.  A
   short settle time per edge brings the bus down to about 2 MHz. */
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
        OLED_SCK_CLR();              /* clock low: data may change here */
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

        OLED_SCK_SET();              /* rising edge: the panel latches the bit */
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

    OLED_WrCmd(0xAEu);           /* display off */
    OLED_WrCmd(0x20u);           /* memory addressing mode */
    OLED_WrCmd(0x02u);           /* page addressing */
    OLED_WrCmd(0xB0u);           /* page start 0 */
    OLED_WrCmd(0xC8u);           /* COM scan remapped */
    OLED_WrCmd(0x00u);           /* low column 0 */
    OLED_WrCmd(0x10u);           /* high column 0 */
    OLED_WrCmd(0x40u);           /* start line 0 */
    OLED_WrCmd(0x81u);           /* contrast */
    OLED_WrCmd(0xCFu);
    OLED_WrCmd(0xA1u);           /* segment remap */
    OLED_WrCmd(0xA6u);           /* normal (not inverted) */
    OLED_WrCmd(0xA8u);           /* multiplex */
    OLED_WrCmd(0x3Fu);
    OLED_WrCmd(0xA4u);           /* output follows RAM */
    OLED_WrCmd(0xD3u);           /* display offset */
    OLED_WrCmd(0x00u);
    OLED_WrCmd(0xD5u);           /* clock divide */
    OLED_WrCmd(0x80u);           /* slower internal clock: the panel keeps its
                                    timing margin while the ranger fires a
                                    40 kHz burst on the same 5 V rail */
    OLED_WrCmd(0xD9u);           /* pre-charge */
    OLED_WrCmd(0xF1u);
    OLED_WrCmd(0xDAu);           /* COM pins */
    OLED_WrCmd(0x12u);
    OLED_WrCmd(0xDBu);           /* VCOMH */
    OLED_WrCmd(0x40u);
    OLED_WrCmd(0x8Du);           /* charge pump on */
    OLED_WrCmd(0x14u);
    OLED_WrCmd(0xAFu);           /* display on */

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
