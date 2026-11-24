/*
 * 粤嵌 GEC-M4 门禁板上的 CS C4 彩蛋.
 *
 * 纯新增模块: 门禁密码的逻辑一行都不动. CS 里 C4 的键盘密码是 7355608
 * (七位, 自 CS:S 起未变); 因为玩家习惯记成八位, 带前导零的 07355608 也接受.
 * 输入密码再按 '#' 启动 40 秒倒计时, 滴滴间隔均匀收窄, 末尾接一段旋律.
 *
 * 全部动作由主循环里的 C4_Poll() 推进, 所以彩蛋运行期间键盘, 刷卡和
 * 超声波门控仍然照常工作, 不会像阻塞写法那样把整机冻住 40 秒.
 */

#include "c4.h"
#include "beep.h"
#include "oled.h"

/* 32 位毫秒节拍上的回绕安全比较: now 已经走到 ref */
#define C4_REACHED(now, ref) (((uint32_t)((now) - (ref))) < 0x80000000u)

#define C4_ST_IDLE 0u
#define C4_ST_RUN  1u
#define C4_ST_SONG 2u

typedef struct
{
    uint16_t hz;   /* 0 表示休止符 */
    uint16_t ms;
} C4_Note;

/* 欢乐颂, 人人熟悉的那四小节; 贝多芬, 公有领域. 开头留一个休止, 把最后一声
   滴和第一个音符分开, 免得糊在一起. */
static const C4_Note s_song[] =
{
    {   0u, 120u },
    { 330u, 320u }, { 330u, 320u }, { 349u, 320u }, { 392u, 320u },
    { 392u, 320u }, { 349u, 320u }, { 330u, 320u }, { 294u, 320u },
    { 262u, 320u }, { 262u, 320u }, { 294u, 320u }, { 330u, 320u },
    { 330u, 480u }, { 294u, 160u }, { 294u, 640u }, {   0u, 200u }
};

#define C4_SONG_LEN ((uint8_t)(sizeof(s_song) / sizeof(s_song[0])))

static char     s_digits[C4_DIGITS];
static uint8_t  s_len;
static uint8_t  s_ready;
static uint8_t  s_state;
static uint8_t  s_beeping;
static uint8_t  s_idx;
static uint32_t s_start;
static uint32_t s_next_beep;
static uint32_t s_next_step;
static uint32_t s_beep_end;
static uint32_t s_shown_s;

static uint8_t C4_Match(const char *code, uint8_t n)
{
    uint8_t i;

    if (s_len != n)
    {
        return 0u;
    }

    for (i = 0u; i < n; i++)
    {
        if (s_digits[i] != code[i])
        {
            return 0u;
        }
    }

    return 1u;
}

/* hz 为 0 表示休止: 直接关掉输出 */
static void C4_NoteOn(uint16_t hz)
{
    if (hz == 0u)
    {
        Beep_Off();
        return;
    }

    Beep_SetTone(hz);
    Beep_On();
}

/* 单次短滴到点就收声, 只在导通状态下才判断 */
static void C4_BeepStop(uint32_t now)
{
    if ((s_beeping != 0u) && (C4_REACHED(now, s_beep_end) != 0u))
    {
        Beep_Off();
        s_beeping = 0u;
    }
}

void C4_Reset(void)
{
    uint8_t i;

    for (i = 0u; i < C4_DIGITS; i++)
    {
        s_digits[i] = 0;
    }

    s_len = 0u;
}

void C4_Init(void)
{
    s_ready   = 1u;
    s_state   = C4_ST_IDLE;
    s_beeping = 0u;
    s_idx     = 0u;
    s_shown_s = 0xFFFFFFFFu;

    C4_Reset();
}

/* 每次数字键按下都同时收进这里, 不管密码缓冲区怎么处理它:
   炸弹密码比任何允许的门禁密码都长 */
void C4_Feed(char key)
{
    uint8_t i;

    if ((key < '0') || (key > '9'))
    {
        return;
    }

    if (s_len >= C4_DIGITS)
    {
        /* 已经满了: 只保留最新的八位数字 */
        for (i = 1u; i < C4_DIGITS; i++)
        {
            s_digits[i - 1u] = s_digits[i];
        }
        s_digits[C4_DIGITS - 1u] = key;
        return;
    }

    s_digits[s_len] = key;
    s_len++;
}

/* 返回 1 表示命中炸弹密码并已启动; 否则清空缓冲, 交回正常开门流程 */
uint8_t C4_Take(void)
{
    if (s_ready == 0u)
    {
        return 0u;
    }

    if ((C4_Match(C4_CODE7, 7u) != 0u) || (C4_Match(C4_CODE8, 8u) != 0u))
    {
        C4_Start();
        return 1u;
    }

    C4_Reset();
    return 0u;
}

void C4_Start(void)
{
    uint32_t now = HAL_GetTick();

    s_state     = C4_ST_RUN;
    s_beeping   = 0u;
    s_idx       = 0u;
    s_start     = now;
    s_next_beep = now;            /* 第一声立刻响 */
    s_next_step = now;
    s_beep_end  = now;
    s_shown_s   = 0xFFFFFFFFu;

    C4_Reset();

    Beep_SetTone(0u);             /* 倒计时的滴滴用基础音调 */
    Beep_SetVolume(C4_VOLUME);

    OLED_Clear();
    OLED_ShowString(28u, 0u, "C4 ARMED");
    OLED_ShowString(28u, 2u, "CODE OK");
    OLED_ShowString(0u, 6u, "T-MINUS 40 SEC");
}

/* 彩蛋是否占用面板与蜂鸣器; 主循环和按键处理都靠它让路 */
uint8_t C4_Active(void)
{
    return (s_state != C4_ST_IDLE) ? 1u : 0u;
}

/* 每次只推进一步, 返回 1 只发生在整段演示彻底结束的那一拍 */
uint8_t C4_Poll(void)
{
    uint32_t now;
    uint32_t elapsed;
    uint32_t remain;
    uint32_t span;
    uint32_t gap;
    uint8_t  secs;
    char     line[3];

    if (s_state == C4_ST_IDLE)
    {
        return 0u;
    }

    now = HAL_GetTick();

    if (s_state == C4_ST_SONG)
    {
        /* 旋律: 每个音符按自己的时值推进 */
        if (C4_REACHED(now, s_next_step) == 0u)
        {
            return 0u;
        }

        s_idx++;

        if (s_idx >= C4_SONG_LEN)
        {
            Beep_Off();
            Beep_SetTone(0u);      /* 恢复默认的 2.7 kHz 提示音 */
            OLED_ClearArea(0u, 2u, 127u, 8u);
            OLED_ShowString(32u, 4u, "DEFUSED");
            s_state = C4_ST_IDLE;
            return 1u;
        }

        C4_NoteOn(s_song[s_idx].hz);
        s_next_step = now + (uint32_t)s_song[s_idx].ms;
        return 0u;
    }

    elapsed = (uint32_t)(now - s_start);

    if (elapsed >= C4_TOTAL_MS)
    {
        /* 归零: 此时滴滴早已连成一片持续音, 所以不再额外长鸣,
           而是直接交给旋律 */
        Beep_Off();
        s_beeping = 0u;

        Beep_SetVolume(C4_MUSIC_VOLUME);
        s_state     = C4_ST_SONG;
        s_idx       = 0u;
        s_next_step = now + (uint32_t)s_song[0].ms;
        C4_NoteOn(s_song[0].hz);

        OLED_ClearArea(0u, 2u, 127u, 3u);
        OLED_ShowString(28u, 2u, "MUSIC");
        return 0u;
    }

    remain = C4_TOTAL_MS - elapsed;

    /* 间隔按已用时间线性收窄, 所以全程连续加速, 而不是在几档固定节奏
       之间跳变 */
    span = C4_GAP_START_MS - C4_GAP_END_MS;
    gap  = C4_GAP_START_MS - (span * elapsed) / C4_TOTAL_MS;

    if (gap <= C4_BEEP_MS)
    {
        /* 间隔已经并拢: 保持导通, 听起来就是一直在滴 */
        if (s_beeping == 0u)
        {
            Beep_On();
            s_beeping = 1u;
        }
    }
    else
    {
        C4_BeepStop(now);

        if ((s_beeping == 0u) && (C4_REACHED(now, s_next_beep) != 0u))
        {
            Beep_On();
            s_beeping   = 1u;
            s_beep_end  = now + C4_BEEP_MS;
            s_next_beep = now + gap;
        }
    }

    /* 剩余秒数只在变化时刷一次屏, 别每轮都重画 */
    secs = (uint8_t)((remain + 999u) / 1000u);

    if ((uint32_t)secs != s_shown_s)
    {
        s_shown_s = (uint32_t)secs;
        line[0]   = (char)('0' + (char)((secs / 10u) % 10u));
        line[1]   = (char)('0' + (char)(secs % 10u));
        line[2]   = 0;

        OLED_ClearArea(0u, 4u, 127u, 5u);
        OLED_ShowString(0u, 4u, "C4  SEC");
        OLED_ShowString(96u, 4u, line);
    }

    return 0u;
}
