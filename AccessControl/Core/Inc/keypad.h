/*
 * keypad.h - 4x4 passive membrane keypad on the P8 header
 *
 * Neither the row group nor the column group of the matrix is assumed: the
 * driver reports which two of the eight wires a pressed key shorts.  The
 * pair of every printed key ships as a table measured on this board, so the pad
 * answers keys on the very first power-up.  A table left in flash by an earlier
 * firmware wins over it; there is no calibration screen any more.
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
