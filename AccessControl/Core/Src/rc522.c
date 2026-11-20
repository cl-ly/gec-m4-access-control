/*
 * rc522.c - MFRC522 RFID reader on software SPI
 * SCK=PG12  MOSI=PD4  MISO=PD5  CS(SDA)=PF12  RST=PE10
 *
 * The vendor example drives PD7/PC6/PC8/PD6/PC11, and on this board four of
 * those already belong to the OLED and the 4x4 keypad, so the reader sits on
 * five GPIOs that only double as FSMC lines (free because no TFTLCD panel is
 * fitted).  The bit banging follows the vendor sequence: MOSI is set first,
 * then SCK rises and MISO is sampled, then SCK falls again.
 */
#include "rc522.h"

/* ---------------------------------------------------------------- pins */
/* SCK=PG12  MOSI=PD4  MISO=PD5  CS(SDA)=PF12  RST=PE10 */
#define RC522_SCK_H()    (GPIOG->BSRR = (uint32_t)GPIO_PIN_12)
#define RC522_SCK_L()    (GPIOG->BSRR = ((uint32_t)GPIO_PIN_12) << 16)
#define RC522_MOSI_H()   (GPIOD->BSRR = (uint32_t)GPIO_PIN_4)
#define RC522_MOSI_L()   (GPIOD->BSRR = ((uint32_t)GPIO_PIN_4) << 16)
#define RC522_CS_H()     (GPIOF->BSRR = (uint32_t)GPIO_PIN_12)
#define RC522_CS_L()     (GPIOF->BSRR = ((uint32_t)GPIO_PIN_12) << 16)
#define RC522_RST_H()    (GPIOE->BSRR = (uint32_t)GPIO_PIN_10)
#define RC522_RST_L()    (GPIOE->BSRR = ((uint32_t)GPIO_PIN_10) << 16)
#define RC522_MISO()     (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_5) == GPIO_PIN_SET)

/* ----------------------------------------------------------- registers */
#define CommandReg       0x01u
#define ComIEnReg        0x02u
#define ComIrqReg        0x04u
#define DivIrqReg        0x05u
#define ErrorReg         0x06u
#define Status2Reg       0x08u
#define FIFODataReg      0x09u
#define FIFOLevelReg     0x0Au
#define ControlReg       0x0Cu
#define BitFramingReg    0x0Du
#define CollReg          0x0Eu
#define ModeReg          0x11u
#define TxControlReg     0x14u
#define TxAutoReg        0x15u
#define TModeReg         0x2Au
#define TPrescalerReg    0x2Bu
#define TReloadRegH      0x2Cu
#define TReloadRegL      0x2Du
#define VersionReg       0x37u

/* ------------------------------------------------------------ commands */
#define PCD_IDLE         0x00u
#define PCD_AUTHENT      0x0Eu
#define PCD_TRANSCEIVE   0x0Cu
#define PCD_RESETPHASE   0x0Fu

#define PICC_REQIDL      0x26u
#define PICC_ANTICOLL1   0x93u

#define MI_OK            0u
#define MI_ERR           1u
#define MI_NOTAGERR      2u

/* SCK/MOSI/CS/RST are outputs, MISO (PD5) stays an input with a pull-up */
static void RC522_GpioInit(void)
{
    GPIO_InitTypeDef init = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    init.Mode  = GPIO_MODE_OUTPUT_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    /* MOSI = PD4 */
    init.Pin = GPIO_PIN_4;
    HAL_GPIO_Init(GPIOD, &init);

    /* RST = PE10 */
    init.Pin = GPIO_PIN_10;
    HAL_GPIO_Init(GPIOE, &init);

    /* CS (SDA on the module) = PF12 */
    init.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOF, &init);

    /* SCK = PG12 */
    init.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOG, &init);

    /* MISO = PD5, driven by the reader, so input with pull-up */
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLUP;
    init.Pin  = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOD, &init);
}

/* --------------------------------------------------------- spi transfer */
static uint8_t RC522_Swap(uint8_t value)
{
    uint8_t i;
    uint8_t in = 0u;

    for (i = 0u; i < 8u; i++)
    {
        if ((value & 0x80u) != 0u)
        {
            RC522_MOSI_H();
        }
        else
        {
            RC522_MOSI_L();
        }
        value = (uint8_t)(value << 1);

        RC522_SCK_H();

        if (RC522_MISO() != 0u)
        {
            in |= (uint8_t)(0x80u >> i);
        }

        RC522_SCK_L();
    }

    return in;
}

static void RC522_WriteReg(uint8_t addr, uint8_t value)
{
    RC522_CS_L();
    (void)RC522_Swap((uint8_t)((addr << 1) & 0x7Eu));
    (void)RC522_Swap(value);
    RC522_CS_H();
}

static uint8_t RC522_ReadReg(uint8_t addr)
{
    uint8_t value;

    RC522_CS_L();
    (void)RC522_Swap((uint8_t)(((addr << 1) & 0x7Eu) | 0x80u));
    value = RC522_Swap(0x00u);
    RC522_CS_H();

    return value;
}

static void RC522_SetMask(uint8_t addr, uint8_t mask)
{
    RC522_WriteReg(addr, (uint8_t)(RC522_ReadReg(addr) | mask));
}

static void RC522_ClearMask(uint8_t addr, uint8_t mask)
{
    RC522_WriteReg(addr, (uint8_t)(RC522_ReadReg(addr) & (uint8_t)(~mask)));
}

static void RC522_AntennaOn(void)
{
    if ((RC522_ReadReg(TxControlReg) & 0x03u) == 0u)
    {
        RC522_SetMask(TxControlReg, 0x03u);
    }
}

static void RC522_Reset(void)
{
    RC522_RST_H();
    HAL_Delay(1u);
    RC522_RST_L();
    HAL_Delay(1u);
    RC522_RST_H();
    HAL_Delay(1u);

    RC522_WriteReg(CommandReg, PCD_RESETPHASE);
    HAL_Delay(1u);
}

static uint8_t RC522_ToCard(uint8_t cmd, const uint8_t *send, uint8_t send_len,
                            uint8_t *back, uint16_t *back_bits)
{
    uint8_t  status = MI_ERR;
    uint8_t  irq_en = 0u;
    uint8_t  wait_irq = 0u;
    uint8_t  n;
    uint8_t  last;
    uint16_t i;
    uint32_t guard = 20000u;

    if (cmd == PCD_AUTHENT)
    {
        irq_en   = 0x12u;
        wait_irq = 0x10u;
    }
    else if (cmd == PCD_TRANSCEIVE)
    {
        irq_en   = 0x77u;
        wait_irq = 0x30u;
    }

    RC522_WriteReg(ComIEnReg, (uint8_t)(irq_en | 0x80u));
    RC522_ClearMask(ComIrqReg, 0x80u);
    RC522_SetMask(FIFOLevelReg, 0x80u);
    RC522_WriteReg(CommandReg, PCD_IDLE);

    for (i = 0u; i < send_len; i++)
    {
        RC522_WriteReg(FIFODataReg, send[i]);
    }

    RC522_WriteReg(CommandReg, cmd);

    if (cmd == PCD_TRANSCEIVE)
    {
        RC522_SetMask(BitFramingReg, 0x80u);
    }

    do
    {
        n = RC522_ReadReg(ComIrqReg);
        guard--;
    } while ((guard != 0u) && ((n & 0x01u) == 0u) && ((n & wait_irq) == 0u));

    RC522_ClearMask(BitFramingReg, 0x80u);

    if (guard != 0u)
    {
        if ((RC522_ReadReg(ErrorReg) & 0x1Bu) == 0u)
        {
            if ((n & irq_en & 0x01u) != 0u)
            {
                status = MI_NOTAGERR;
            }

            if (cmd == PCD_TRANSCEIVE)
            {
                n    = (uint8_t)(RC522_ReadReg(FIFOLevelReg) & 0x7Fu);
                last = (uint8_t)(RC522_ReadReg(ControlReg) & 0x07u);

                if (last != 0u)
                {
                    *back_bits = (uint16_t)((n - 1u) * 8u + last);
                }
                else
                {
                    *back_bits = (uint16_t)(n * 8u);
                }

                if (n == 0u)
                {
                    n = 1u;
                }
                if (n > RC522_UID_LEN)
                {
                    n = RC522_UID_LEN;
                }

                for (i = 0u; i < n; i++)
                {
                    back[i] = RC522_ReadReg(FIFODataReg);
                }
            }

            status = MI_OK;
        }
    }

    RC522_WriteReg(CommandReg, PCD_IDLE);
    RC522_ClearMask(ComIrqReg, 0x80u);
    RC522_SetMask(FIFOLevelReg, 0x80u);

    return status;
}

static uint8_t RC522_Request(uint8_t mode)
{
    uint8_t  tag[2];
    uint16_t bits = 0u;
    uint8_t  status;

    RC522_WriteReg(BitFramingReg, 0x07u);
    tag[0] = mode;

    status = RC522_ToCard(PCD_TRANSCEIVE, tag, 1u, tag, &bits);
    if ((status != MI_OK) || (bits != 0x10u))
    {
        status = MI_ERR;
    }

    return status;
}

static uint8_t RC522_Anticoll(uint8_t *uid)
{
    uint8_t  buf[RC522_UID_LEN];
    uint16_t bits = 0u;
    uint8_t  status;
    uint8_t  i;

    RC522_WriteReg(BitFramingReg, 0x00u);

    buf[0] = PICC_ANTICOLL1;
    buf[1] = 0x20u;

    status = RC522_ToCard(PCD_TRANSCEIVE, buf, 2u, buf, &bits);

    if (status == MI_OK)
    {
        if (bits != 0x28u)
        {
            status = MI_ERR;
        }
    }

    if (status == MI_OK)
    {
        for (i = 0u; i < RC522_UID_LEN; i++)
        {
            uid[i] = buf[i];
        }

        if ((uint8_t)(uid[0] ^ uid[1] ^ uid[2] ^ uid[3]) != uid[4])
        {
            status = MI_ERR;
        }
    }

    return status;
}

uint8_t RC522_GetVersion(void)
{
    return RC522_ReadReg(VersionReg);
}

uint8_t RC522_Init(void)
{
    uint8_t version;

    RC522_GpioInit();

    RC522_CS_H();
    RC522_SCK_L();
    RC522_MOSI_L();

    RC522_Reset();

    /* 25 ms internal timer, 100 % ASK, CRC preset 0x6363 */
    RC522_WriteReg(TModeReg, 0x8Du);
    RC522_WriteReg(TPrescalerReg, 0x3Eu);
    RC522_WriteReg(TReloadRegL, 0x1Eu);
    RC522_WriteReg(TReloadRegH, 0x00u);
    RC522_WriteReg(TxAutoReg, 0x40u);
    RC522_WriteReg(ModeReg, 0x3Du);

    RC522_AntennaOn();

    version = RC522_GetVersion();
    if ((version == 0x00u) || (version == 0xFFu))
    {
        return 0u;
    }

    return 1u;
}

uint8_t RC522_ReadCard(uint8_t *uid)
{
    if (RC522_Request(PICC_REQIDL) != MI_OK)
    {
        return 0u;
    }

    if (RC522_Anticoll(uid) != MI_OK)
    {
        return 0u;
    }

    return 1u;
}
