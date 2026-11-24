/*
 * rc522.h - 软件 SPI 驱动的 MFRC522 读卡器
 * SCK=PG12  MOSI=PD4  MISO=PD5  CS(SDA)=PF12  RST=PE10  VCC=3.3V
 *
 * 厂商例程用的那组引脚 (PD7/PC6/PC8/PD6/PC11) 在本板上已经被 OLED 和 4x4
 * 键盘占用, 所以这个驱动改用另外五根只兼作 FSMC 的 GPIO. 本工程没有装 TFTLCD
 * 屏, 那几根线因此是空闲的.
 */
#ifndef __RC522_H
#define __RC522_H

#include "main.h"

/* 防冲撞读卡返回 UID0..UID3 加一个 BCC 校验字节 */
#define RC522_UID_LEN   5u

uint8_t RC522_Init(void);
uint8_t RC522_GetVersion(void);
uint8_t RC522_ReadCard(uint8_t *uid);

#endif /* __RC522_H */
