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
#include "c4.h"
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
 * 密码记录存放在扇区 6 (128 KB, 起始 0x08040000), 这块是空闲的: 代码本身
 * 结束在扇区 0 内, 键盘线对表用的是扇区 7.
 * 共三个字: magic | 长度 + 前三个字符 | 后三个字符
 */
#define PWD_SAVE_ADDR    0x08040000u
#define PWD_SAVE_SECTOR  FLASH_SECTOR_6
#define PWD_SAVE_MAGIC   0x50574433u

/* 配对的 RFID 门卡存放在扇区 5 (128 KB, 起始 0x08020000) */
#define RFID_SAVE_ADDR   0x08020000u
#define RFID_SAVE_SECTOR FLASH_SECTOR_5
#define RFID_SAVE_MAGIC  0x52464944u

/* 待机界面分散在这么多块 128x64 屏幕上; 用 S1/S2 翻页 */
#define PAGE_NUM         4u

/*
 * 接近门控. 屏幕和读卡器只在有人站在门前时才工作: 距离小于 PROX_NEAR_CM 时一切
 * 保持唤醒, 超过该距离持续 PROX_FAR_MS 就熄屏, 并让 CPU 在两次测量之间进睡眠.
 */
#define PROX_NEAR_CM     50u
#define PROX_FAR_MS      1200u
#define PROX_POLL_MS     300u

/* 按键后屏幕保持点亮这么久, 不论超声波怎么判断 */
#define PROX_KEY_HOLD_MS 6000u

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 出厂密码; 一旦 Flash 里存过密码记录, 就用记录替换它 */
static char    g_pwd[PWD_MAX] = { '1', '2', '3', '4', 0, 0 };
static uint8_t g_pwd_len = 4u;
static char    g_buf[PWD_MAX];
static uint8_t g_len  = 0u;
static uint8_t g_fail = 0u;

/* 配对的卡: g_card 存 UID0..UID3 加 BCC 校验字节, g_card_ok 表示它有效 */
static uint8_t g_card[RC522_UID_LEN];
static uint8_t g_card_ok = 0u;
static uint8_t g_rfid_ok  = 0u;
static uint8_t g_rfid_ver = 0u;
static uint8_t g_page     = 0u;

/* 接近检测 / 功耗状态 */
static uint8_t  g_disp_on   = 1u;
static uint16_t g_far_ms    = 0u;
static uint32_t g_prox_tick = 0u;

/* 最近一次刷卡结果: 0 无, 1 匹配, 2 拒绝 (供刷卡页显示) */
static uint8_t g_last_rfid = 0u;
/* 由单卡拒卡自检置起; 只存在 RAM, 从不写入 Flash */
static uint8_t g_rfid_test = 0u;

/* 最近一次超声波读数, 与距离页共用 */
static uint16_t g_dist_cm = 0u;
static uint8_t  g_dist_ok = 0u;
/* 距离页已经显示的内容, 内容没变就不重写 */
static char     g_shown_val[16];
static uint8_t  g_shown_msg  = 0u;
static uint16_t g_shown_w    = 0xFFFFu;
static uint8_t  g_shown_hits = 0xFFu;
static uint16_t g_hold_ms = 0u;

/* 开机自检 / 首页故障页状态:
   g_fault_shown     当前屏上画的就是故障页;
   g_fault_latch     上一次判过的"有模块缺失"状态, 用来抓上升沿和下降沿;
   g_fault_slot      多个模块同时缺失时, 单行报错在这些缺失项之间轮转;
   g_fault_tick      轮转计时;
   g_rfid_check_tick 读卡器缺失期间重新探测的计时. */
static uint8_t  g_fault_shown     = 0u;
static uint8_t  g_fault_latch     = 0u;
static uint8_t  g_fault_slot      = 0u;
static uint32_t g_fault_tick      = 0u;
static uint32_t g_rfid_check_tick = 0u;
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

/* 键盘只提供十个数字和四个字母键, 别的都没有 */
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
static void App_PageFault(void);

static void App_HexByte(char *out, uint8_t value)
{
    static const char HEXDIG[] = "0123456789ABCDEF";

    out[0] = HEXDIG[(value >> 4) & 0x0Fu];
    out[1] = HEXDIG[value & 0x0Fu];
}

/* --------------------------------------------------------------- 开机自检
 *
 * 自检只覆盖"软件能电气回读"的模块:
 *   HC-SR04 : 回波线空闲时由模块推挽拉低, 拔掉即浮空. hc_sr04.c 的上拉电平探测
 *             连续 3 次拿不到回波就复核一次, 结论写进 g_sr04.absent.
 *   RC522   : VersionReg 读到 0x00 / 0xFF 说明 SPI 上没人应答,
 *             RC522_Init() 返回 0; 缺失期间每 SELFCHECK_RFID_MS 重探一次.
 *
 * 4x4 薄膜键盘是纯干接点 (按下只是两根线短接), 蜂鸣器和 OLED 只有写通道,
 * 这三者在软件层面无法判断"到底插没插", 因此不进自检表, 它们坏了靠现象发现.
 *
 * 报错格式: 一条 !(具体器件)! , 全大写 ASCII, 整行不超过 16 个 8x16 字符.
 * 多个模块同时缺失时, 单行报错按 SELFCHECK_CYCLE_MS 轮转, 保证每个缺失的
 * 器件都能被看到, 同时不破坏"故障页只占一行"这条约定.
 */
#define SELFCHECK_NUM           2u
#define SELFCHECK_CYCLE_MS      2000u
/* 读卡器在位巡检周期: 在位时回读 VersionReg, 缺失时重新初始化, 都按这个节拍 */
#define SELFCHECK_RFID_POLL_MS  1000u

/* 每一位对应一个自检项, 位序与 App_FaultName() 里的顺序一致 */
#define SELFCHECK_BIT_SR04  0x01u
#define SELFCHECK_BIT_RFID  0x02u

static uint8_t App_FaultMask(void)
{
    uint8_t mask = 0u;

    if (g_sr04.absent != 0u)
    {
        mask |= SELFCHECK_BIT_SR04;
    }
    if (g_rfid_ok == 0u)
    {
        mask |= SELFCHECK_BIT_RFID;
    }

    return mask;
}

static uint8_t App_FaultActive(void)
{
    return (App_FaultMask() != 0u) ? 1u : 0u;
}

static uint8_t App_FaultCount(uint8_t mask)
{
    uint8_t n = 0u;
    uint8_t i;

    for (i = 0u; i < SELFCHECK_NUM; i++)
    {
        if ((mask & (uint8_t)(1u << i)) != 0u)
        {
            n++;
        }
    }

    return n;
}

/* 取第 slot 个缺失项的名字; 调用前必须保证 slot < App_FaultCount(mask) */
static const char *App_FaultName(uint8_t mask, uint8_t slot)
{
    uint8_t seen = 0u;
    uint8_t i;

    for (i = 0u; i < SELFCHECK_NUM; i++)
    {
        if ((mask & (uint8_t)(1u << i)) == 0u)
        {
            continue;
        }
        if (seen == slot)
        {
            return (i == 0u) ? "!(NO HC-SR04)!" : "!(NO RC522)!";
        }
        seen++;
    }

    return "!(SELFCHECK)!";
}

/* 硬件故障页. 自检发现模块缺失时, 首页画的就是这一页, 密码页被顶下去.
 * 只画一行, 而且必须点名是哪个器件缺失; 不附操作提示行.
 */
static void App_PageFault(void)
{
    uint8_t     mask = App_FaultMask();
    uint8_t     cnt  = App_FaultCount(mask);
    const char *err;
    uint8_t     len = 0u;
    uint8_t     x;

    if (cnt == 0u)
    {
        return;
    }

    err = App_FaultName(mask, (uint8_t)(g_fault_slot % cnt));

    while ((err[len] != '\0') && (len < 16u))
    {
        len++;
    }

    x = (uint8_t)((128u - ((uint16_t)len * 8u)) / 2u);

    OLED_Clear();
    OLED_ShowString(x, 3u, err);

    g_fault_shown = 1u;
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

    /* 读到 0x00 / 0xFF 说明 SPI 这几根引脚没在读卡器上通信 */
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

    /* 手上只有一张卡时也能验证拒卡路径: 按 D 把一个错误 UID 只写进 RAM,
       再刷卡应当显示 TEST DENY OK */
    if (g_rfid_test != 0u)
    {
        OLED_ShowString(0u, 6u, (g_last_rfid == 2u) ? "TEST DENY  OK" : "TEST SWIPE CARD");
        return;
    }

    /* 最近一次刷卡结果, 这样手上没有第二张卡也能核对拒卡 */
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

/* 以米为单位保留两位小数; 该模块量程约 5 m, 小数点前一位数字就够 */
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

/* 只有文字真的变了才重写这一行: 每多写一次就多一次把列指针搞乱的机会,
   而列指针一旦跑偏, 整块屏就会变成错位的乱码字形. */
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

/* 每个状态行一个编码, 内容没变就跳过重绘 */
static uint8_t App_DistMsgCode(void)
{
    if (g_sr04.absent != 0u)
    {
        return 5u;
    }
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

/* 只重画发生变化的那几行, 实时页才不会每次轮询都闪一下 */
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
            OLED_ShowString(12u, 4u, "!(STUCK HIGH)!");
        }
        else if (g_shown_msg == 2u)
        {
            OLED_ShowString(20u, 4u, "!(NO ECHO)!");
        }
        else if (g_shown_msg == 3u)
        {
            OLED_ShowString(0u, 4u, "NEAR <0.50m");
        }
        else if (g_shown_msg == 4u)
        {
            OLED_ShowString(0u, 4u, "FAR >=0.50m");
        }
        else
        {
            /* 模块不在位: 两行都按统一的 !(报错)! 格式写, 并点名具体器件 */
            OLED_ShowString(8u, 4u, "!(NO HC-SR04)!");
            OLED_ClearArea(0u, 6u, 127u, 7u);
            OLED_ShowString(12u, 6u, "!(CHECK WIRE)!");
        }

        /* 诊断行可能被报错占用过, 状态一变就强制重画一次 */
        g_shown_w    = 0xFFFFu;
        g_shown_hits = 0xFFu;
    }

    /* W 是上一段回波脉宽的微秒数, H 是回波真正升起过的次数.
       H 一直停在 00 说明触发脉冲根本没送到模块, 问题在 VCC/Trig/GND 而不在代码. */
    if (g_sr04.absent != 0u)
    {
        /* 诊断行现在显示的是模块缺失报错, 不能再被 W/H 覆盖 */
        return;
    }

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

    /* 屏幕刚被清空, 所以每个缓存行都得重新画一遍 */
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
    /* 首页优先级最高: 自检发现任一模块缺失时, 密码页让位给故障页.
       按键后的保持期内仍显示密码页, 免得模块坏了连门都进不去. */
    if ((g_page == 0u) && (App_FaultActive() != 0u) && (g_hold_ms == 0u))
    {
        App_PageFault();
        return;
    }

    g_fault_shown = 0u;

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
 * 从键盘读入一次密码. 只排三行, 正好放进 128x64 的屏: 标题在第 0-1 页, 输入位
 * 在第 2-3 页, 按键说明在第 6-7 页.
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

    /* 还没配卡: 第一次刷到的卡就自动成为门卡 */
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

    /* 只在 RAM 里翻转 UID 的一个字节. 此时刷配对卡必须被拒绝, 这是手上只有
       一张卡时验证拒卡路径的唯一办法. 全程不写 Flash, 所以断电重启后真实 UID
       恢复, 那张卡照样能开门. */
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

/* 每次按键都让面板自己保持点亮一段时间, 这样即使没人在半米内站着,
   翻页和输密码也照常可用. */
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
    uint8_t  ver  = 0u;

    now = HAL_GetTick();

    if ((uint32_t)(now - g_prox_tick) < PROX_POLL_MS)
    {
        /* 两次测量之间没有别的事可做: 让内核睡到下一个 SysTick,
           不要在这里空转主循环 */
        if (g_disp_on == 0u)
        {
            HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
        }
        return;
    }
    g_prox_tick = now;

    /* 完全没有回波意味着最近的目标已超出模块约 4 m 的量程, 或者模块被拔掉了;
       两者都按门口没人处理, 于是面板熄灭. 任何一次按键都能把面板重新点亮,
       所以传感器坏掉也不会让门禁一直黑着没法用. */
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

    /* 读卡器在位巡检. g_rfid_ok 只在上电 RC522_Init() 时置位, 运行中把模块拔掉
       它自己不会变, 所以必须周期回读一次 VersionReg: 读到 0x00 / 0xFF 说明 SPI 上
       没人应答, 当场清 g_rfid_ok 让故障页接管; 缺失期间再按同一节拍 RC522_Init()
       重新初始化, 插回来就恢复. 两种情况都不需要按 RESET. */
    if ((uint32_t)(now - g_rfid_check_tick) >= SELFCHECK_RFID_POLL_MS)
    {
        g_rfid_check_tick = now;

        if (g_rfid_ok != 0u)
        {
            ver = RC522_GetVersion();

            if ((ver == 0x00u) || (ver == 0xFFu))
            {
                g_rfid_ok = 0u;
            }
            else
            {
                g_rfid_ver = ver;
            }
        }
        else if (RC522_Init() != 0u)
        {
            g_rfid_ok  = 1u;
            g_rfid_ver = RC522_GetVersion();
        }
    }

    /* 硬件故障 (超声波模块不在位 / 读卡器不在位) 要顶在首页上: 这里既不执行
       远离熄屏, 也不让密码页把它盖回去. 故障一出现就清保持期、跳回首页并立即
       重绘; 按键后的保持期内先让密码页可用, 保持期结束再把故障页顶回来. */
    if (App_FaultActive() != 0u)
    {
        g_far_ms = 0u;
        App_DisplaySet(1u);

        if (g_fault_latch == 0u)
        {
            g_fault_latch = 1u;
            g_hold_ms     = 0u;
            g_page        = 0u;
            g_fault_slot  = 0u;
            g_fault_tick  = now;
            App_ShowIdle();
        }
        else if (g_hold_ms > 0u)
        {
            g_hold_ms = (uint16_t)(g_hold_ms - PROX_POLL_MS);
            if (g_hold_ms == 0u)
            {
                App_ShowIdle();
            }
        }
        else if (App_FaultCount(App_FaultMask()) > 1u)
        {
            /* 多个模块同时缺失: 单行报错按固定节拍轮转, 每个都能看到 */
            if ((uint32_t)(now - g_fault_tick) >= SELFCHECK_CYCLE_MS)
            {
                g_fault_tick = now;
                g_fault_slot++;
                if ((g_page == 0u) && (g_disp_on != 0u))
                {
                    App_ShowIdle();
                }
            }
        }
        else if ((g_page == 0u) && (g_fault_shown == 0u))
        {
            App_ShowIdle();
        }

        if ((g_page == 2u) && (g_disp_on != 0u))
        {
            App_DistRefresh();
        }

        return;
    }

    /* 故障消失 (模块插回来了): 首页恢复成密码页 */
    if (g_fault_latch != 0u)
    {
        g_fault_latch = 0u;
        g_hold_ms     = 0u;
        g_fault_slot  = 0u;
        App_ShowIdle();
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

    /* 距离页正显示时, 持续刷新上面的实时读数 */
    if ((g_page == 2u) && (g_disp_on != 0u))
    {
        App_DistRefresh();
    }
}

static void App_HandleKey(char key)
{
    uint8_t i;
    uint8_t ok;

    /* C4 彩蛋运行期间由它独占面板和蜂鸣器 */
    if (C4_Active() != 0u)
    {
        return;
    }

    /* 刷卡页同时作为单卡拒卡自检的入口 */
    if ((g_page == 1u) && (key == 'D'))
    {
        App_RfidSelfTest();
        return;
    }

    /* 键盘按键始终作用于密码页 */
    if (g_page != 0u)
    {
        g_page = 0u;
        App_ShowIdle();
    }

    if (key == '*')
    {
        C4_Reset();
        App_ResetInput();
        App_DrawPwd();
        return;
    }

    /* 炸弹密码加 '#' 启动倒计时彩蛋, 而不是开门; 真正的密码最长六位,
       两者永远不会混淆 */
    if ((key == '#') && (C4_Take() != 0u))
    {
        App_ResetInput();
        return;
    }

    if (key == '#')
    {
        /* 什么都没输入时按 # 进入改密流程; 字母或数字键因此仍然
           可以是密码本身的一部分 */
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

    /* 炸弹密码比任何密码都长, 所以每个数字键按下时也同时收进彩蛋缓冲 */
    C4_Feed(key);

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

  /* 复位所有外设, 初始化 Flash 接口和 SysTick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* 配置系统时钟 */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* 初始化所有已配置的外设 */
  MX_GPIO_Init();
  MX_TIM1_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  uint8_t boot_a = 0u;
  uint8_t boot_b = 0u;

  OLED_Init();
  Keypad_Init();

  /* 上电时按住任意键可清除已配对的门卡; 线对表本身不再需要现场校准,
     驱动里已经带了实测表 */
  if (Keypad_RawPair(&boot_a, &boot_b) != 0u)
  {
    App_ClearCard();
    App_WaitAllKeysReleased();
  }

  BoardKey_Init();
  HCSR04_Init();
  App_LoadPwd();
  App_LoadCard();
  g_rfid_ok         = RC522_Init();
  g_rfid_ver        = RC522_GetVersion();
  g_rfid_check_tick = HAL_GetTick();
  App_ResetInput();
  App_ShowIdle();

  /* 上电提示音: 比最初轻一半 (5% 占空比), 响三秒;
     追加在上面那段启动流程之后, 不改动它 */
  Beep_Init();
  Beep_SetVolume(BEEP_VOLUME_BOOT);
  Beep_BeepMs(BEEP_MS_BOOT);

  /* C4 彩蛋: 输入炸弹密码再按 '#' 即启动 */
  C4_Init();
  /* USER CODE END 2 */

  /* 主循环 */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    char key = Keypad_Scan();
    uint8_t bk;

    if (key != 0)
    {
      /* 只要碰键盘就把屏幕唤回 */
      App_WakeByKey();
      App_HandleKey(key);

      /* 薄膜键盘每次按键给一声短提示音; 彩蛋运行期间跳过,
         免得切断倒计时音和结尾旋律 */
      if (C4_Active() == 0u)
      {
        Beep_SetVolume(BEEP_VOLUME_KEY);
        Beep_BeepMs(BEEP_MS_KEY);
      }
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

    if (C4_Active() != 0u)
    {
      /* 倒计时期间屏幕归它使用: 保持点亮 */
      App_WakeByKey();
    }

    if (C4_Poll() != 0u)
    {
      App_ShowIdle();
    }

    App_PowerPoll();
  }
  /* USER CODE END 3 */
}

/**
  * @brief 系统时钟配置
  * @retval 无
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** 配置内部主稳压器的输出电压
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** 按 RCC_OscInitTypeDef 结构体里给定的参数初始化 RCC 振荡器。
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

  /** 初始化 CPU、AHB 与 APB 总线时钟
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
  * @brief  出错时进入这个函数。
  * @retval 无
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* 用户可以在这里自行上报 HAL 的出错状态 */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  上报 assert_param 出错所在的源文件名与行号。
  * @param  file: 源文件名的指针
  * @param  line: assert_param 出错的行号
  * @retval 无
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* 用户可以在这里自行上报文件名与行号,
     例如: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
