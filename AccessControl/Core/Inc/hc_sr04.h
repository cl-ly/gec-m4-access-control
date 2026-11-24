/*
 * hc_sr04.h - 用普通 GPIO 驱动 HC-SR04 超声波测距
 * Trig = PE12 (34 针座丝印 D9), Echo = PE14 (丝印 D11), VCC = 5 V.
 *
 * 回波用 1 MHz 时基 (TIM1) 轮询, 对 100 us .. 25 ms 的脉冲足够精确. 下面这个
 * 信息结构体是为了让屏幕能说清一次测距为什么失败: 回波线到底有没有升起过,
 * 上一段脉冲有多宽, 以及空闲时这根线是不是一直为高.
 *
 * absent 是模块在位检测: 回波线空闲时由模块推挽拉低, 连续几次量不到回波就临时
 * 打开内部上拉复核电平, 读到高说明这根线根本没人驱动, 也就是模块不在位.
 */
#ifndef __HC_SR04_H
#define __HC_SR04_H

#include "main.h"

/* 连续这么多次没有有效回波, 就做一次上拉探测判断模块是否还在 */
#define HCSR04_ABSENT_STREAK 3u

typedef struct
{
    uint8_t  valid;      /* 1 表示 cm 里是刚测到的距离               */
    uint16_t cm;         /* 最近一次距离, 单位厘米                   */
    uint16_t width_us;   /* 最近一段回波脉冲的宽度, 微秒             */
    uint8_t  idle_high;  /* 发触发之前回波线就已经为高               */
    uint8_t  hits;       /* 回波真正升起过的次数, 0..99              */
    uint8_t  trigs;      /* 已发出的触发次数, 0..99                  */
    uint8_t  misses;     /* 没拿到有效结果的触发次数, 0..99          */
    uint8_t  streak;     /* 连续没拿到有效结果的次数, 0..255         */
    uint8_t  absent;     /* 1 表示回波线没人驱动, 模块不在位         */
} HCSR04_Info;

extern HCSR04_Info g_sr04;

void    HCSR04_Init(void);
uint8_t HCSR04_MeasureCm(uint16_t *cm);
/* 探测回波线空闲电平: 1 = 模块在位 (线被拉低), 0 = 不在位 (线浮空被读成高) */
uint8_t HCSR04_Present(void);

#endif /* __HC_SR04_H */
