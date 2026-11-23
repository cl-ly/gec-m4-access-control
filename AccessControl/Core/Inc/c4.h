#ifndef __C4_H
#define __C4_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/*
 * 薄膜键盘上的 CS C4 彩蛋.
 *
 * 炸弹键盘密码是 7355608: 七位数字, 自 CS:S 起从未改动
 * (3DM, https://ol.3dmgame.com/gl/96239.html). 玩家习惯把它记成八位,
 * 所以带前导零的 "07355608" 也一并接受.
 *
 * 输入密码再按 '#' 后启动 40 秒非阻塞倒计时, 滴滴间隔从 C4_GAP_START_MS
 * 一路均匀收窄到 C4_GAP_END_MS; 间隔小于单次蜂鸣长度后连成一片持续音,
 * 随后直接接一段短旋律. 门禁密码只有 4..6 位, 因此永远不会和七位或八位的
 * 炸弹密码混淆, 原有开门逻辑保持原样.
 */

#define C4_DIGITS       8u
#define C4_CODE7        "7355608"
#define C4_CODE8        "07355608"

#define C4_TOTAL_MS     40000u   /* 总倒计时 40 秒                  */
#define C4_GAP_START_MS 1500u    /* 第一声滴的间隔                  */
#define C4_GAP_END_MS   40u      /* 归零时的间隔: 滴滴连成持续音    */
#define C4_BEEP_MS      60u      /* 单次短滴的长度                  */
#define C4_VOLUME       5u       /* 与其它提示音相同的 5% 占空比    */
#define C4_MUSIC_VOLUME 8u       /* 结尾旋律稍响一点                */

void    C4_Init(void);
void    C4_Reset(void);
void    C4_Feed(char key);
uint8_t C4_Take(void);
void    C4_Start(void);
uint8_t C4_Active(void);
uint8_t C4_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __C4_H */
