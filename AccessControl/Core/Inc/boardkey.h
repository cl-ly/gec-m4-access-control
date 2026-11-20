/*
 * boardkey.h - the two on-board push buttons S1 / S2
 * S1 = KEY0 = PA0, S2 = KEY1 = PE2, both active low with internal pull-up.
 *
 * The 4x4 membrane keypad owns the P8 header, so the on-board buttons are the
 * cleanest way to page through the 128x64 panel: one screen cannot hold the
 * password prompt, the RFID status and the key legend at once.
 */
#ifndef __BOARDKEY_H
#define __BOARDKEY_H

#include "main.h"

#define BOARDKEY_S1          1u
#define BOARDKEY_S2          2u

/* number of consecutive polls a level must stay stable to count as a press */
#define BOARDKEY_DEBOUNCE    3u

void    BoardKey_Init(void);
uint8_t BoardKey_Scan(void);

#endif /* __BOARDKEY_H */
