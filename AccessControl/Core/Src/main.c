/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "oled.h"
#include "keypad.h"
#include "rc522.h"
#include "boardkey.h"
#include "hc_sr04.h"
#include "beep.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define PWD_MIN          4u
#define PWD_MAX          6u
#define PWD_OPEN_MS      2500u
#define PWD_DENY_MS      1200u

/*
 * Password record lives in sector 6 (128 KB at 0x08040000), which is free:
 * the code itself ends inside sector 0 and the keypad table uses sector 7.
 * Three words: magic | length + chars 0..2 | chars 3..5
 */
#define PWD_SAVE_ADDR    0x08040000u
#define PWD_SAVE_SECTOR  FLASH_SECTOR_6
#define PWD_SAVE_MAGIC   0x50574433u

/* the paired RFID card sits in sector 5 (128 KB at 0x08020000) */
#define RFID_SAVE_ADDR   0x08020000u
#define RFID_SAVE_SECTOR FLASH_SECTOR_5
#define RFID_SAVE_MAGIC  0x52464944u

/* the idle interface is split over this many 128x64 screens; S1/S2 flip them */
#define PAGE_NUM         4u

/*
 * Proximity gate.  The panel and the reader only run while somebody stands in
 * front of the door: closer than PROX_NEAR_CM keeps everything awake, farther
 * than that for PROX_FAR_MS blanks the panel and lets the CPU sleep between
 * measurements.
 */
#define PROX_NEAR_CM     50u
#define PROX_FAR_MS      1200u
#define PROX_POLL_MS     300u

/* a key press keeps the panel awake this long, whatever the sensor says */
#define PROX_KEY_HOLD_MS 6000u

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* factory password; it is replaced by the flash record once one was saved */
static char    g_pwd[PWD_MAX] = { '1', '2', '3', '4', 0, 0 };
static uint8_t g_pwd_len = 4u;
static char    g_buf[PWD_MAX];
static uint8_t g_len  = 0u;
static uint8_t g_fail = 0u;

/* paired card: g_card holds UID0..UID3 plus BCC, g_card_ok says it is valid */
static uint8_t g_card[RC522_UID_LEN];
static uint8_t g_card_ok = 0u;
static uint8_t g_rfid_ok  = 0u;
static uint8_t g_rfid_ver = 0u;
static uint8_t g_page     = 0u;

/* proximity / power state */
static uint8_t  g_disp_on   = 1u;
static uint16_t g_far_ms    = 0u;
static uint32_t g_prox_tick = 0u;

/* last card result: 0 none, 1 matched, 2 denied (for the RFID page) */
static uint8_t g_last_rfid = 0u;
/* armed by the single-card deny self test; RAM only, never written to flash */
static uint8_t g_rfid_test = 0u;

/* last ultrasonic reading, shared with the distance page */
static uint16_t g_dist_cm = 0u;
static uint8_t  g_dist_ok = 0u;
/* what the distance page already shows, so unchanged text is not rewritten */
static char     g_shown_val[16];
static uint8_t  g_shown_msg  = 0u;
static uint16_t g_shown_w    = 0xFFFFu;
static uint8_t  g_shown_hits = 0xFFu;
static uint16_t g_hold_ms = 0u;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void App_CenterCN(uint8_t page, const uint8_t *seq, uint8_t num)
{
    uint16_t w = (uint16_t)num * 16u;
    uint8_t  x = (w >= 128u) ? 0u : (uint8_t)((128u - w) / 2u);

    OLED_ClearArea(0u, page, 127u, (uint8_t)(page + 1u));
    OLED_ShowCNSeq(x, page, seq, num);
}

static void App_DrawTitle(void)
{
    static const uint8_t title[] = { G_QING, G_SHU, G_RU, G_MI, G_MA };

    App_CenterCN(0u, title, 5u);
}

/* the keypad offers ten digits plus the four letter keys, nothing else */
static uint8_t App_CharOk(char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return 1u;
    }
    if ((c >= 'A') && (c <= 'D'))
    {
        return 1u;
    }

    return 0u;
}

static void App_DrawPwdLine(uint8_t page, const char *buf, uint8_t len)
{
    uint8_t i;

    OLED_ClearArea(0u, page, 127u, (uint8_t)(page + 1u));
    OLED_ShowString(16u, page, "PWD");
    for (i = 0u; i < PWD_MAX; i++)
    {
        OLED_ShowChar((uint8_t)(56u + i * 8u), page, (i < len) ? buf[i] : '_');
    }
}

static void App_DrawPwd(void)
{
    App_DrawPwdLine(2u, g_buf, g_len);
}

static void App_DrawHint(void)
{
    OLED_ClearArea(0u, 6u, 127u, 7u);
    OLED_ShowString(0u, 6u, "*CLR #:OK  S1/S2");
}

static void App_ResetInput(void)
{
    uint8_t i;

    for (i = 0u; i < PWD_MAX; i++)
    {
        g_buf[i] = 0;
    }
    g_len = 0u;
}

static void App_ShowOpen(void)
{
    static const uint8_t t1[] = { G_YI, G_KAI, G_MEN };
    static const uint8_t t2[] = { G_CHENG, G_GONG };

    OLED_Clear();
    App_CenterCN(0u, t1, 3u);
    OLED_ShowString(44u, 4u, "OPEN");
    OLED_ShowCNSeq(48u, 6u, t2, 2u);
}

static void App_ShowWrong(void)
{
    static const uint8_t t1[] = { G_MI, G_MA, G_CUO, G_WU };

    OLED_Clear();
    App_CenterCN(0u, t1, 4u);
    OLED_ShowString(56u, 4u, "RETRY");
}

static void App_ShowLock(void)
{
    static const uint8_t t1[] = { G_MI, G_MA, G_CUO, G_WU };
    static const uint8_t t2[] = { G_SUO, G_DING };

    OLED_Clear();
    App_CenterCN(0u, t1, 4u);
    App_CenterCN(4u, t2, 2u);
    OLED_ShowString(56u, 6u, "3s");
}

/* ------------------------------------------------------- idle pages */
static void App_PagePwd(void);
static void App_PageRfid(void);
static void App_PageDist(void);
static void App_PageHelp(void);

static void App_HexByte(char *out, uint8_t value)
{
    static const char HEXDIG[] = "0123456789ABCDEF";

    out[0] = HEXDIG[(value >> 4) & 0x0Fu];
    out[1] = HEXDIG[value & 0x0Fu];
}

static void App_PagePwd(void)
{
    App_DrawTitle();
    App_DrawPwd();

    OLED_ClearArea(0u, 4u, 127u, 5u);
    OLED_ShowString(0u, 4u, (g_rfid_ok != 0u) ? "#=CHG  RFID ON" : "#=CHG  RFID --");

    App_DrawHint();
}

static void App_PageRfid(void)
{
    char    line[17];
    uint8_t i;

    OLED_ShowString(20u, 0u, "RFID STATUS");

    /* 0x00 / 0xFF means the SPI pins are not talking to the reader */
    if (g_rfid_ok != 0u)
    {
        line[0] = 'V';
        line[1] = 'E';
        line[2] = 'R';
        line[3] = ' ';
        line[4] = '0';
        line[5] = 'x';
        App_HexByte(&line[6], g_rfid_ver);
        line[8] = '\0';
    }
    else
    {
        line[0] = 'N';
        line[1] = 'O';
        line[2] = ' ';
        line[3] = 'R';
        line[4] = 'E';
        line[5] = 'A';
        line[6] = 'D';
        line[7] = 'E';
        line[8] = 'R';
        line[9] = '\0';
    }
    OLED_ShowString(0u, 2u, line);

    line[0] = 'U';
    line[1] = 'I';
    line[2] = 'D';
    line[3] = ' ';
    if (g_card_ok != 0u)
    {
        for (i = 0u; i < 4u; i++)
        {
            App_HexByte(&line[4u + (i * 3u)], g_card[i]);
            if (i < 3u)
            {
                line[6u + (i * 3u)] = ' ';
            }
        }
        line[15] = '\0';
    }
    else
    {
        line[4]  = '-';
        line[5]  = '-';
        line[6]  = ' ';
        line[7]  = 'N';
        line[8]  = 'O';
        line[9]  = ' ';
        line[10] = 'C';
        line[11] = 'A';
        line[12] = 'R';
        line[13] = 'D';
        line[14] = '\0';
    }
    OLED_ShowString(0u, 4u, line);

    /* with a single card in hand the deny path can still be exercised: D
       arms a RAM-only wrong UID, a swipe must then answer TEST DENY OK */
    if (g_rfid_test != 0u)
    {
        OLED_ShowString(0u, 6u, (g_last_rfid == 2u) ? "TEST DENY  OK" : "TEST SWIPE CARD");
        return;
    }

    /* last swipe result, so a wrong card can be checked without a second one */
    line[0] = 'L';
    line[1] = 'A';
    line[2] = 'S';
    line[3] = 'T';
    line[4] = ' ';
    if (g_last_rfid == 1u)
    {
        line[5] = 'M';
        line[6] = 'A';
        line[7] = 'T';
        line[8] = 'C';
        line[9] = 'H';
        line[10] = '\0';
    }
    else if (g_last_rfid == 2u)
    {
        line[5] = 'D';
        line[6] = 'E';
        line[7] = 'N';
        line[8] = 'Y';
        line[9] = ' ';
        line[10] = '\0';
    }
    else
    {
        line[5] = '-';
        line[6] = '-';
        line[7] = '-';
        line[8] = '\0';
    }
    OLED_ShowString(24u, 6u, line);
}

/* metres with two decimals, one digit before the point is enough for the
   roughly 5 m reach of the module */
static void App_FormatMeters(char *out, uint8_t valid, uint16_t cm)
{
    uint8_t whole;
    uint8_t frac;

    if (valid == 0u)
    {
        out[0] = '-';
        out[1] = '.';
        out[2] = '-';
        out[3] = '-';
        out[4] = ' ';
        out[5] = 'm';
        out[6] = '\0';
        return;
    }

    whole = (uint8_t)(cm / 100u);
    frac  = (uint8_t)(cm % 100u);

    out[0] = (char)('0' + whole);
    out[1] = '.';
    out[2] = (char)('0' + (frac / 10u));
    out[3] = (char)('0' + (frac % 10u));
    out[4] = ' ';
    out[5] = 'm';
    out[6] = '\0';
}

/* A line is only rewritten when its text really changed: every extra write is
   another chance for a corrupted command to move the column pointer, and a
   stray pointer is exactly what turns the panel into scrambled glyphs. */
static uint8_t App_TextChanged(char *cache, const char *text)
{
    uint8_t i = 0u;
    uint8_t changed = 0u;

    for (;;)
    {
        if (cache[i] != text[i])
        {
            changed = 1u;
            cache[i] = text[i];
        }
        if (text[i] == 0)
        {
            return changed;
        }
        i++;
        if (i >= 15u)
        {
            return changed;
        }
    }
}

/* one code per status line, so the redraw can be skipped while it is unchanged */
static uint8_t App_DistMsgCode(void)
{
    if (g_sr04.idle_high != 0u)
    {
        return 1u;
    }
    if (g_dist_ok == 0u)
    {
        return 2u;
    }

    return (g_dist_cm < PROX_NEAR_CM) ? 3u : 4u;
}

/* only the changing lines, so the live page does not flicker every poll */
static void App_DistRefresh(void)
{
    char     line[8];
    char     diag[17];
    uint16_t w;

    App_FormatMeters(line, g_dist_ok, g_dist_cm);
    if (App_TextChanged(g_shown_val, line) != 0u)
    {
        OLED_ClearArea(32u, 2u, 127u, 3u);
        OLED_ShowString(40u, 2u, line);
    }

    if (App_DistMsgCode() != g_shown_msg)
    {
        g_shown_msg = App_DistMsgCode();
        OLED_ClearArea(0u, 4u, 127u, 5u);

        if (g_shown_msg == 1u)
        {
            OLED_ShowString(0u, 4u, "ECHO STUCK HIGH");
        }
        else if (g_shown_msg == 2u)
        {
            OLED_ShowString(16u, 4u, "NO ECHO");
        }
        else if (g_shown_msg == 3u)
        {
            OLED_ShowString(0u, 4u, "NEAR <0.50m");
        }
        else
        {
            OLED_ShowString(0u, 4u, "FAR >=0.50m");
        }
    }

    /* W: width of the last echo pulse in us, H: how many pulses came back.
       H staying at 00 means the trigger never reached the module, so the
       problem is VCC/Trig/GND rather than the code. */
    w = g_sr04.width_us;
    diag[0]  = 'W';
    diag[1]  = ':';
    diag[2]  = (char)('0' + ((w / 1000u) % 10u));
    diag[3]  = (char)('0' + ((w / 100u) % 10u));
    diag[4]  = (char)('0' + ((w / 10u) % 10u));
    diag[5]  = (char)('0' + (w % 10u));
    diag[6]  = 'u';
    diag[7]  = 's';
    diag[8]  = ' ';
    diag[9]  = 'H';
    diag[10] = ':';
    diag[11] = (char)('0' + ((g_sr04.hits / 10u) % 10u));
    diag[12] = (char)('0' + (g_sr04.hits % 10u));
    diag[13] = '\0';
    if ((w != g_shown_w) || (g_sr04.hits != g_shown_hits))
    {
        g_shown_w    = w;
        g_shown_hits = g_sr04.hits;
        OLED_ClearArea(0u, 6u, 127u, 7u);
        OLED_ShowString(8u, 6u, diag);
    }
}

static void App_PageDist(void)
{
    OLED_ShowString(24u, 0u, "ULTRASONIC");

    /* the panel was just cleared, so every cached line has to be drawn again */
    g_shown_val[0] = 0;
    g_shown_msg    = 0u;
    g_shown_w      = 0xFFFFu;
    g_shown_hits   = 0xFFu;

    App_DistRefresh();
}

static void App_PageHelp(void)
{
    OLED_ShowString(44u, 0u, "HELP");

    OLED_ShowString(0u, 2u, "0-9 A-D = INPUT");

    OLED_ShowString(0u, 4u, "*CLR  # = OPEN");

    OLED_ShowString(0u, 6u, "EMPTY# =CHG PWD");
}

static void App_ShowIdle(void)
{
    OLED_Clear();

    if (g_page == 1u)
    {
        App_PageRfid();
    }
    else if (g_page == 2u)
    {
        App_PageDist();
    }
    else if (g_page == 3u)
    {
        App_PageHelp();
    }
    else
    {
        App_PagePwd();
    }
}

static void App_SwitchPage(uint8_t forward)
{
    if (forward != 0u)
    {
        g_page++;
        if (g_page >= PAGE_NUM)
        {
            g_page = 0u;
        }
    }
    else
    {
        g_page = (g_page == 0u) ? (uint8_t)(PAGE_NUM - 1u) : (uint8_t)(g_page - 1u);
    }

    App_ShowIdle();
}

static void App_LoadPwd(void)
{
    const volatile uint32_t *src = (const volatile uint32_t *)PWD_SAVE_ADDR;
    uint32_t w1;
    uint32_t w2;
    uint8_t  len;
    uint8_t  i;

    if (src[0] != PWD_SAVE_MAGIC)
    {
        return;
    }

    w1  = src[1];
    w2  = src[2];
    len = (uint8_t)(w1 & 0xFFu);

    if ((len < PWD_MIN) || (len > PWD_MAX))
    {
        return;
    }

    for (i = 0u; i < PWD_MAX; i++)
    {
        char c;

        if (i < 3u)
        {
            c = (char)((w1 >> (8u * (i + 1u))) & 0xFFu);
        }
        else
        {
            c = (char)((w2 >> (8u * (i - 3u))) & 0xFFu);
        }

        if (i < len)
        {
            if (App_CharOk(c) == 0u)
            {
                return;
            }
            g_pwd[i] = c;
        }
        else
        {
            g_pwd[i] = 0;
        }
    }

    g_pwd_len = len;
}

static uint8_t App_SavePwd(const char *pwd, uint8_t len)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0u;
    uint32_t words[3];
    uint32_t w1 = 0u;
    uint32_t w2 = 0u;
    uint8_t  i;
    uint8_t  ok = 1u;
    const volatile uint32_t *chk = (const volatile uint32_t *)PWD_SAVE_ADDR;

    if ((len < PWD_MIN) || (len > PWD_MAX))
    {
        return 0u;
    }

    w1 = (uint32_t)len;
    for (i = 0u; i < 3u; i++)
    {
        w1 |= ((uint32_t)(uint8_t)pwd[i]) << (8u * (i + 1u));
    }
    for (i = 3u; i < PWD_MAX; i++)
    {
        w2 |= ((uint32_t)(uint8_t)pwd[i]) << (8u * (i - 3u));
    }

    words[0] = PWD_SAVE_MAGIC;
    words[1] = w1;
    words[2] = w2;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = PWD_SAVE_SECTOR;
    erase.NbSectors    = 1u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        ok = 0u;
    }

    for (i = 0u; (i < 3u) && (ok != 0u); i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, PWD_SAVE_ADDR + ((uint32_t)i * 4u),
                              (uint64_t)words[i]) != HAL_OK)
        {
            ok = 0u;
        }
    }

    HAL_FLASH_Lock();

    if (ok != 0u)
    {
        for (i = 0u; i < 3u; i++)
        {
            if (chk[i] != words[i])
            {
                ok = 0u;
                break;
            }
        }
    }

    return ok;
}

/*
 * Reads one password from the keypad.  Three lines only, so it fits the
 * 128x64 panel: title on pages 0-1, the input slots on pages 2-3 and the key
 * legend on pages 6-7.
 */
static uint8_t App_ReadPwd(const uint8_t *title, uint8_t tnum, char *out, uint8_t *outlen)
{
    uint8_t i;
    uint8_t len = 0u;
    char    key;
    char    ch;

    for (i = 0u; i < PWD_MAX; i++)
    {
        out[i] = 0;
    }

    OLED_Clear();
    App_CenterCN(0u, title, tnum);

    for (;;)
    {
        OLED_ClearArea(0u, 2u, 127u, 3u);
        OLED_ShowString(16u, 2u, "PWD");
        for (i = 0u; i < PWD_MAX; i++)
        {
            ch = (i < len) ? out[i] : '_';
            OLED_ShowChar((uint8_t)(56u + i * 8u), 2u, ch);
        }

        OLED_ClearArea(0u, 6u, 127u, 7u);
        OLED_ShowString(0u, 6u, "*:BACK #:NEXT");

        key = Keypad_Scan();
        if (key == 0)
        {
            continue;
        }

        if (key == '*')
        {
            return 0u;
        }
        if (key == '#')
        {
            if (len >= PWD_MIN)
            {
                *outlen = len;
                return 1u;
            }
            continue;
        }
        if ((App_CharOk(key) != 0u) && (len < PWD_MAX))
        {
            out[len] = key;
            len++;
        }
    }
}

static void App_ChangePwd(void)
{
    static const uint8_t t_old[]   = { G_JIU, G_MI, G_MA };
    static const uint8_t t_new[]   = { G_XIN, G_MI, G_MA };
    static const uint8_t t_again[] = { G_ZAI, G_CI, G_SHU, G_RU };
    static const uint8_t t_done[]  = { G_MI, G_MA, G_YI, G_XIU, G_GAI };
    char    a[PWD_MAX];
    char    b[PWD_MAX];
    uint8_t la = 0u;
    uint8_t lb = 0u;
    uint8_t i;
    uint8_t ok;

    if (App_ReadPwd(t_old, 3u, a, &la) == 0u)
    {
        App_ShowIdle();
        return;
    }

    ok = (la == g_pwd_len) ? 1u : 0u;
    for (i = 0u; (i < la) && (ok != 0u); i++)
    {
        if (a[i] != g_pwd[i])
        {
            ok = 0u;
        }
    }

    if (ok == 0u)
    {
        OLED_Clear();
        OLED_ShowString(0u, 2u, "OLD PWD WRONG");
        HAL_Delay(1500u);
        App_ShowIdle();
        return;
    }

    if (App_ReadPwd(t_new, 3u, a, &la) == 0u)
    {
        App_ShowIdle();
        return;
    }

    if (App_ReadPwd(t_again, 4u, b, &lb) == 0u)
    {
        App_ShowIdle();
        return;
    }

    ok = (la == lb) ? 1u : 0u;
    for (i = 0u; (i < la) && (ok != 0u); i++)
    {
        if (a[i] != b[i])
        {
            ok = 0u;
        }
    }

    if (ok == 0u)
    {
        OLED_Clear();
        OLED_ShowString(8u, 2u, "MISMATCH");
        OLED_ShowString(4u, 4u, "NOT SAVED");
        HAL_Delay(1500u);
        App_ShowIdle();
        return;
    }

    if (App_SavePwd(a, la) == 0u)
    {
        OLED_Clear();
        OLED_ShowString(4u, 2u, "SAVE FAILED");
        HAL_Delay(1500u);
        App_ShowIdle();
        return;
    }

    for (i = 0u; i < PWD_MAX; i++)
    {
        g_pwd[i] = a[i];
    }
    g_pwd_len = la;

    OLED_Clear();
    App_CenterCN(0u, t_done, 5u);
    OLED_ShowString(28u, 4u, "SAVED");
    HAL_Delay(1500u);
    App_ShowIdle();
}

static void App_WaitAllKeysReleased(void)
{
    uint8_t a = 0u;
    uint8_t b = 0u;

    while (Keypad_RawPair(&a, &b) != 0u)
    {
        HAL_Delay(KEYPAD_RELEASE_MS);
    }
}

static void App_ShowCardMsg(const char *msg)
{
    OLED_ClearArea(0u, 4u, 127u, 5u);
    OLED_ShowString(0u, 4u, msg);
}

static void App_LoadCard(void)
{
    const volatile uint32_t *src = (const volatile uint32_t *)RFID_SAVE_ADDR;
    uint32_t w;
    uint8_t  i;
    uint8_t  checksum;

    if (src[0] != RFID_SAVE_MAGIC)
    {
        return;
    }

    w = src[1];
    for (i = 0u; i < RC522_UID_LEN; i++)
    {
        g_card[i] = (uint8_t)((w >> (8u * i)) & 0xFFu);
    }

    checksum = (uint8_t)(g_card[0] ^ g_card[1] ^ g_card[2] ^ g_card[3]);
    if ((checksum != g_card[4]) || ((g_card[0] | g_card[1] | g_card[2] | g_card[3]) == 0u))
    {
        while (i != 0u)
        {
            i--;
            g_card[i] = 0u;
        }
        return;
    }

    g_card_ok = 1u;
}

static uint8_t App_SaveCard(const uint8_t *uid)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0u;
    uint32_t words[2];
    uint32_t w = 0u;
    uint8_t  i;
    uint8_t  ok = 1u;
    const volatile uint32_t *chk = (const volatile uint32_t *)RFID_SAVE_ADDR;

    for (i = 0u; i < RC522_UID_LEN; i++)
    {
        w |= ((uint32_t)uid[i]) << (8u * i);
    }
    words[0] = RFID_SAVE_MAGIC;
    words[1] = w;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = RFID_SAVE_SECTOR;
    erase.NbSectors    = 1u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        ok = 0u;
    }

    for (i = 0u; (i < 2u) && (ok != 0u); i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, RFID_SAVE_ADDR + ((uint32_t)i * 4u),
                              (uint64_t)words[i]) != HAL_OK)
        {
            ok = 0u;
        }
    }

    HAL_FLASH_Lock();

    if (ok != 0u)
    {
        for (i = 0u; i < 2u; i++)
        {
            if (chk[i] != words[i])
            {
                ok = 0u;
                break;
            }
        }
    }

    if (ok != 0u)
    {
        for (i = 0u; i < RC522_UID_LEN; i++)
        {
            g_card[i] = uid[i];
        }
        g_card_ok = 1u;
    }

    return ok;
}

static void App_ClearCard(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0u;
    uint8_t  i;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = RFID_SAVE_SECTOR;
    erase.NbSectors    = 1u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    (void)HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();

    for (i = 0u; i < RC522_UID_LEN; i++)
    {
        g_card[i] = 0u;
    }
    g_card_ok = 0u;
}

static void App_WaitCardRemoved(void)
{
    uint8_t uid[RC522_UID_LEN];
    uint8_t n;

    for (n = 0u; n < 40u; n++)
    {
        if (RC522_ReadCard(uid) == 0u)
        {
            return;
        }
        HAL_Delay(50u);
    }
}

static void App_RfidPoll(void)
{
    uint8_t uid[RC522_UID_LEN];
    uint8_t i;
    uint8_t ok;

    if (g_rfid_ok == 0u)
    {
        return;
    }

    if (RC522_ReadCard(uid) == 0u)
    {
        return;
    }

    /* no card paired yet: the first card ever seen becomes the door card */
    if (g_card_ok == 0u)
    {
        App_ShowCardMsg((App_SaveCard(uid) != 0u) ? "CARD PAIRED" : "PAIR FAILED");
        HAL_Delay(1200u);
        App_WaitCardRemoved();
        App_ResetInput();
        App_ShowIdle();
        return;
    }

    ok = 1u;
    for (i = 0u; i < RC522_UID_LEN; i++)
    {
        if (uid[i] != g_card[i])
        {
            ok = 0u;
        }
    }

    if (ok != 0u)
    {
        g_fail     = 0u;
        g_last_rfid = 1u;
        App_ShowOpen();
        HAL_Delay(PWD_OPEN_MS);
    }
    else
    {
        g_last_rfid = 2u;
        App_ShowCardMsg("CARD DENIED");
        HAL_Delay(PWD_DENY_MS);
    }

    App_WaitCardRemoved();
    App_ResetInput();
    App_ShowIdle();
}

static void App_RfidSelfTest(void)
{
    if ((g_card_ok == 0u) || (g_rfid_ok == 0u))
    {
        return;
    }

    /* Flip one UID byte in RAM only.  Swiping the paired card now has to be
       rejected, which is the only way to exercise the deny path with a single
       card in hand.  Nothing reaches flash, so a power cycle restores the real
       UID and that card opens the door again. */
    g_card[0]   = (uint8_t)(g_card[0] ^ 0x5Au);
    g_rfid_test = 1u;
    g_last_rfid = 0u;
    App_ShowIdle();
}

/* ------------------------------------------------------ power gating */
static void App_DisplaySet(uint8_t on)
{
    if (on != 0u)
    {
        if (g_disp_on == 0u)
        {
            OLED_DisplayOn();
            g_disp_on = 1u;
            g_far_ms  = 0u;
            App_ShowIdle();
        }
    }
    else
    {
        if (g_disp_on != 0u)
        {
            OLED_DisplayOff();
            g_disp_on = 0u;
        }
    }
}

/* Every key press keeps the panel lit for a while on its own, so paging and
   typing stay usable even when nobody stands within half a metre. */
static void App_WakeByKey(void)
{
    g_hold_ms = PROX_KEY_HOLD_MS;
    App_DisplaySet(1u);
}

static void App_PowerPoll(void)
{
    uint16_t cm   = 0u;
    uint32_t now;
    uint8_t  near = 0u;

    now = HAL_GetTick();

    if ((uint32_t)(now - g_prox_tick) < PROX_POLL_MS)
    {
        /* between measurements there is nothing to do: let the core sleep
           until the next SysTick instead of spinning the loop */
        if (g_disp_on == 0u)
        {
            HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
        }
        return;
    }
    g_prox_tick = now;

    /* No echo at all means the nearest object is beyond the roughly 4 m reach
       of the module, or the module is unplugged; both count as "nobody is
       standing at the door", so the panel gets blanked.  A key press always
       wakes the panel again, so a dead sensor can never leave the door dark
       and unusable. */
    if (HCSR04_MeasureCm(&cm) != 0u)
    {
        g_dist_ok = 1u;
        g_dist_cm = cm;
        near = (cm < PROX_NEAR_CM) ? 1u : 0u;
    }
    else
    {
        g_dist_ok = 0u;
    }

    if (near != 0u)
    {
        g_far_ms = 0u;
        App_DisplaySet(1u);
    }
    else if (g_disp_on != 0u)
    {
        if (g_hold_ms > 0u)
        {
            g_hold_ms = (uint16_t)(g_hold_ms - PROX_POLL_MS);
            g_far_ms  = 0u;
        }
        else
        {
            if (g_far_ms <= PROX_FAR_MS)
            {
                g_far_ms = (uint16_t)(g_far_ms + PROX_POLL_MS);
            }

            /* 所有页面在持续远离后都进入熄屏状态。 */
            if (g_far_ms >= PROX_FAR_MS)
            {
                App_DisplaySet(0u);
            }
        }
    }

    /* keep the live number on screen while that page is the visible one */
    if ((g_page == 2u) && (g_disp_on != 0u))
    {
        App_DistRefresh();
    }
}

static void App_HandleKey(char key)
{
    uint8_t i;
    uint8_t ok;

    /* the RFID page doubles as the entry of the single-card deny self test */
    if ((g_page == 1u) && (key == 'D'))
    {
        App_RfidSelfTest();
        return;
    }

    /* a keypad press always acts on the password page */
    if (g_page != 0u)
    {
        g_page = 0u;
        App_ShowIdle();
    }

    if (key == '*')
    {
        App_ResetInput();
        App_DrawPwd();
        return;
    }

    if (key == '#')
    {
        /* # with nothing typed opens the change-password walk; a letter or
           digit key can therefore still be part of the password itself */
        if (g_len == 0u)
        {
            App_ChangePwd();
            return;
        }

        ok = (g_len == g_pwd_len) ? 1u : 0u;
        for (i = 0u; (i < g_len) && (ok != 0u); i++)
        {
            if (g_buf[i] != g_pwd[i])
            {
                ok = 0u;
            }
        }

        if (ok != 0u)
        {
            g_fail = 0u;
            App_ShowOpen();
            HAL_Delay(PWD_OPEN_MS);
        }
        else
        {
            if (g_fail < 3u)
            {
                g_fail++;
            }

            if (g_fail >= 3u)
            {
                App_ResetInput();
                App_ShowLock();
                HAL_Delay(3000u);
                g_fail = 0u;
            }
            else
            {
                App_ShowWrong();
                HAL_Delay(PWD_DENY_MS);
            }
        }

        App_ResetInput();
        App_ShowIdle();
        return;
    }

    if ((App_CharOk(key) != 0u) && (g_len < PWD_MAX))
    {
        g_buf[g_len] = key;
        g_len++;
        App_DrawPwd();
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM1_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  uint8_t boot_a = 0u;
  uint8_t boot_b = 0u;

  OLED_Init();
  Keypad_Init();

  /* hold any key while powering up to forget the paired door card; the key
     table itself needs no teaching any more, the driver ships it */
  if (Keypad_RawPair(&boot_a, &boot_b) != 0u)
  {
    App_ClearCard();
    App_WaitAllKeysReleased();
  }

  BoardKey_Init();
  HCSR04_Init();
  App_LoadPwd();
  App_LoadCard();
  g_rfid_ok  = RC522_Init();
  g_rfid_ver = RC522_GetVersion();
  App_ResetInput();
  App_ShowIdle();

  /* power-on chime: 10 % volume, three seconds, added on top of the boot
     sequence above without touching it */
  Beep_Init();
  Beep_SetVolume(10u);
  Beep_BeepMs(3000u);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    char key = Keypad_Scan();
    uint8_t bk;

    if (key != 0)
    {
      /* touching the keypad always brings the panel back */
      App_WakeByKey();
      App_HandleKey(key);
    }
    else
    {
      bk = BoardKey_Scan();

      if (bk != 0)
      {
        App_WakeByKey();
        App_SwitchPage((bk == BOARDKEY_S1) ? 1u : 0u);
      }
      else if (g_disp_on != 0u)
      {
        App_RfidPoll();
      }
    }

    App_PowerPoll();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
