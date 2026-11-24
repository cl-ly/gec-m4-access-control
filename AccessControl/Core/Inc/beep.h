#ifndef __BEEP_H
#define __BEEP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/*
 * 粤嵌 GEC-M4 板载蜂鸣器: P F8 由 TIM13 通道 1 (AF9) 驱动, 与厂商例程的
 * 接法一致 (HARDWARE/BEEP/beep.c: BEEP(x) PFout(8)=x).
 * 该蜂鸣器是无源的, 必须给方波才会响, 给静态电平没有声音.
 *
 * TIM13 时钟来自 APB1 定时器, 共 84 MHz.
 *   音调 = 84 MHz / (PSC + 1) / (ARR + 1) = 84 MHz / 370 / 84 = 2702 Hz
 *   音量 = CCR / (ARR + 1)
 */

#define BEEP_PORT        GPIOF
#define BEEP_PIN         GPIO_PIN_8
#define BEEP_AF          GPIO_AF9_TIM13

#define BEEP_PSC         369u    /* 84 MHz / 370 = 226.9 kHz 计数时钟 */
#define BEEP_ARR         83u     /* 226.9 kHz / 84 = 2702 Hz 音调      */

/* 5% 占空比是最初 10% 的一半: 仍能听见, 但明显更轻 */
#define BEEP_VOLUME_BOOT 5u
/* 薄膜键盘每次按键的短提示音 */
#define BEEP_VOLUME_KEY  5u
#define BEEP_MS_BOOT     3000u
#define BEEP_MS_KEY      300u

void Beep_Init(void);
void Beep_SetVolume(uint8_t percent);
/* 传 0 恢复默认的 2.7 kHz 音调, 传其它值则重新设定 TIM13 */
void Beep_SetTone(uint16_t hz);
void Beep_On(void);
void Beep_Off(void);
void Beep_BeepMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __BEEP_H */
