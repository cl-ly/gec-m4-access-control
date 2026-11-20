/*
 * oled.h - SSD1306 128x64 OLED, software SPI
 * SCK=PD7  SDA=PC6  CS=PD6  DC=PB7  RES=PA4  VCC=3.3V
 * Glyph data lives in oledfont.c (generated), Chinese text is drawn by
 * glyph index so no non-ASCII byte ever appears in a C source file.
 */
#ifndef __OLED_H
#define __OLED_H

#include "main.h"

void OLED_Init(void);
void OLED_Clear(void);
void OLED_ClearArea(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);

/* panel power: 0xAE blanks the display but keeps the GDDRAM contents */
void OLED_DisplayOn(void);
void OLED_DisplayOff(void);
void OLED_ShowChar(uint8_t x, uint8_t y, char ch);
void OLED_ShowString(uint8_t x, uint8_t y, const char *str);
void OLED_ShowCN16(uint8_t x, uint8_t y, uint8_t idx);
void OLED_ShowCNSeq(uint8_t x, uint8_t y, const uint8_t *seq, uint8_t num);

/* glyph order inside OLED_CN16[], same order as chars.txt during generation */
#define G_YI     0
#define G_KAI    1
#define G_MEN    2
#define G_MI     3
#define G_MA     4
#define G_CUO    5
#define G_WU     6
#define G_QING   7
#define G_SHU    8
#define G_RU     9
#define G_ZHENG  10
#define G_QUE    11
#define G_CHONG  12
#define G_ZHI    13
#define G_SUO    14
#define G_DING   15
#define G_CI     16
#define G_CHENG  17
#define G_GONG   18
#define G_XIU    19
#define G_GAI    20
#define G_JIU    21
#define G_XIN    22
#define G_ZAI    23

#endif /* __OLED_H */
