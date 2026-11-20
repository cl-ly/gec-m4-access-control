/*
 * rc522.h - MFRC522 RFID reader on software SPI
 * SCK=PG12  MOSI=PD4  MISO=PD5  CS(SDA)=PF12  RST=PE10  VCC=3.3V
 *
 * The pins the vendor example uses (PD7/PC6/PC8/PD6/PC11) are already taken
 * by the OLED and the 4x4 keypad on this board, so this driver sits on the
 * five GPIOs that only double as FSMC lines.  Those lines are free because
 * the TFTLCD panel is not fitted in this project.
 */
#ifndef __RC522_H
#define __RC522_H

#include "main.h"

/* anticollision returns UID0..UID3 plus the BCC check byte */
#define RC522_UID_LEN   5u

uint8_t RC522_Init(void);
uint8_t RC522_GetVersion(void);
uint8_t RC522_ReadCard(uint8_t *uid);

#endif /* __RC522_H */
