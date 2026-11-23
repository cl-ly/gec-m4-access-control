/*
 * boardkey.h - 板载两个按键 S1 / S2
 * S1 = KEY0 = PA0, S2 = KEY1 = PE2, 均为低电平有效, 内部上拉.
 *
 * 4x4 薄膜键盘占用了 P8 排针, 所以板载按键是翻页查看 128x64 屏幕最干净的入口:
 * 一块屏放不下密码提示, 刷卡状态和按键说明.
 */
#ifndef __BOARDKEY_H
#define __BOARDKEY_H

#include "main.h"

#define BOARDKEY_S1          1u
#define BOARDKEY_S2          2u

/* 电平必须连续稳定的轮询次数, 达到即视为一次按下 */
#define BOARDKEY_DEBOUNCE    3u

void    BoardKey_Init(void);
uint8_t BoardKey_Scan(void);

#endif /* __BOARDKEY_H */
