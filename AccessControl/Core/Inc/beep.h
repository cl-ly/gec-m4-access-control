#ifndef __BEEP_H
#define __BEEP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/*
 * GEC-M4 onboard buzzer: P F8 driven by TIM13 channel 1 (AF9), same wiring
 * the vendor course examples use (HARDWARE/BEEP/beep.c: BEEP(x) PFout(8)=x).
 * The buzzer is passive, so it needs a square wave instead of a static level.
 *
 * TIM13 is clocked from APB1 timers = 84 MHz.
 *   tone   = 84 MHz / (PSC + 1) / (ARR + 1) = 84 MHz / 370 / 84 = 2702 Hz
 *   volume = CCR / (ARR + 1)
 */

#define BEEP_PORT        GPIOF
#define BEEP_PIN         GPIO_PIN_8
#define BEEP_AF          GPIO_AF9_TIM13

#define BEEP_PSC         369u    /* 84 MHz / 370 = 226.9 kHz counter clock */
#define BEEP_ARR         83u     /* 226.9 kHz / 84 = 2702 Hz tone          */

void Beep_Init(void);
void Beep_SetVolume(uint8_t percent);
void Beep_On(void);
void Beep_Off(void);
void Beep_BeepMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __BEEP_H */
