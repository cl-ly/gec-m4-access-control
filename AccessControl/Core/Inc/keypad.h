/*
 * keypad.h - 挂在 P8 排针上的 4x4 无源薄膜键盘
 *
 * 不假设矩阵里哪一组是行、哪一组是列: 驱动只上报按下的键把八根线里的哪两根
 * 短接. 每个印刷键位对应的线对以在本板上实测到的表发布, 所以键盘第一次上电就
 * 能响应按键. 早先固件留在 Flash 里的表优先级更高; 现在已经没有校准界面.
 */
#ifndef __KEYPAD_H
#define __KEYPAD_H

#include "main.h"

#define KEYPAD_DEBOUNCE_MS   12u
#define KEYPAD_RELEASE_MS    10u
#define KEYPAD_NUM_KEYS      16u

void    Keypad_Init(void);
void    Keypad_ResetPairs(void);

uint8_t Keypad_RawPair(uint8_t *a, uint8_t *b);
char    Keypad_Scan(void);

#endif /* __KEYPAD_H */
