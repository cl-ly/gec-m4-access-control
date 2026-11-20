/*
 * hc_sr04.h - HC-SR04 ultrasonic ranger on plain GPIO
 * Trig = PE12 (34 pin decal D9), Echo = PE14 (decal D11), VCC = 5 V.
 *
 * Echo is polled with a 1 MHz time base (TIM1), which is plenty for a pulse
 * that lasts 100 us .. 25 ms.  The info block below exists so the screen can
 * show why a reading failed: whether the echo line ever rose, how wide the
 * last pulse was, and whether the line sat high while idle.
 */
#ifndef __HC_SR04_H
#define __HC_SR04_H

#include "main.h"

typedef struct
{
    uint8_t  valid;      /* 1 = cm holds a fresh distance            */
    uint16_t cm;         /* last distance in centimetres             */
    uint16_t width_us;   /* width of the last echo pulse, us         */
    uint8_t  idle_high;  /* echo sat high before a trigger was sent  */
    uint8_t  hits;       /* pulses where the echo actually rose 0..99 */
    uint8_t  trigs;      /* triggers sent, 0..99                     */
    uint8_t  misses;     /* triggers without a usable answer, 0..99  */
} HCSR04_Info;

extern HCSR04_Info g_sr04;

void    HCSR04_Init(void);
uint8_t HCSR04_MeasureCm(uint16_t *cm);

#endif /* __HC_SR04_H */
