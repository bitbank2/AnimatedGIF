//
// GIF on SPI LCD test program
// Written by Larry Bank
// demo written for the Waveshare 1.3" 240x240 IPS LCD "Hat"
// or the Pimoroni mini display HAT
//
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "bb_spi_lcd.h"
#include "../src/AnimatedGIF.h"
#include "../src/gif.inl"

#define PIMORONI_HAT

#ifdef PIMORONI_HAT
#define DC_PIN 9
#define RESET_PIN -1
#define CS_PIN 7
#define LED_PIN 13
#define LCD_TYPE LCD_ST7789
#else
// Pin definitions for Adafruit PiTFT HAT
// GPIO 25 = Pin 22
#define DC_PIN 22
// GPIO 27 = Pin 13
#define RESET_PIN 13
// GPIO 8 = Pin 24
#define CS_PIN 24
// GPIO 24 = Pin 18
#define LED_PIN 18
#define LCD_TYPE LCD_ST7789_240
#endif

SPILCD lcd;
GIFIMAGE gif;
uint8_t *pStart;

void GIFDraw(GIFDRAW *pDraw)
{
uint8_t *d;
	d = &pStart[((pDraw->iY + pDraw->y) * pDraw->iWidth + pDraw->iX)*2];
	memcpy(d, pDraw->pPixels, pDraw->iWidth * 2);
}

int main(int argc, char *argv[])
{
int i, iFrame;
int w, h;

// int spilcdInit(int iLCDType, int bFlipRGB, int bInvert, int bFlipped, int32_t iSPIFreq, int iCSPin, int iDCPin, int iResetPin, int iLEDPin, int iMISOPin, int iMOSIPin, int iCLKPin);
	i = spilcdInit(&lcd, LCD_TYPE, FLAGS_NONE, 62500000, CS_PIN, DC_PIN, RESET_PIN, LED_PIN, -1,-1,-1,1);
	if (i == 0)
	{
		spilcdSetOrientation(&lcd, LCD_ORIENTATION_90);
		spilcdFill(&lcd, 0, DRAW_TO_LCD);
		GIF_begin(&gif, BIG_ENDIAN_PIXELS);
		i = GIF_openFile(&gif, argv[1], GIFDraw);
		while (i) {
			w = gif.iCanvasWidth;
			h = gif.iCanvasHeight;
			printf("GIF opened; w=%d, h=%d\n", w, h);
			gif.pFrameBuffer = (uint8_t*)malloc(w * h * 3);
                        pStart = &gif.pFrameBuffer[w*h];
			gif.ucDrawType = GIF_DRAW_COOKED;
                        iFrame = 0;
			while (GIF_playFrame(&gif, NULL, NULL)) {
				spilcdSetPosition(&lcd, gif.iX,gif.iY,gif.iWidth,gif.iHeight, DRAW_TO_LCD);
				spilcdWriteDataBlock(&lcd, pStart, gif.iWidth * gif.iHeight * 2, DRAW_TO_LCD);
			}
			GIF_reset(&gif); // repeat
		}
	}
	else
	{
		printf("Unable to initialize the spi_lcd library\n");
	}
   return 0;
} /* main() */
