#ifndef __OLEDFONT_H
#define __OLEDFONT_H

typedef struct
{
    unsigned short code;
    unsigned char  dat[32];
} CN_GLYPH;

#define OLED_CN16_NUM  24

extern const unsigned char OLED_F8x16[95][16];
extern const CN_GLYPH OLED_CN16[OLED_CN16_NUM];

#endif
