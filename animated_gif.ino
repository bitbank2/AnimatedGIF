#include <bb_spi_lcd.h>

//#include "testimage.h"
//#include "testimage2.h"
#include "testimage3.h"
#include "gif.h"

// Files on SD card
// /GIF/homer.gif
// /GIF/tt.gif
// /GIF/bigbuck.gif
// /GIF/futurama.gif

// Teensy 4.0
#ifdef TEENSYDUINO
#define CS_PIN 10
#define DC_PIN 9
#define RESET_PIN 8
#define LED_PIN -1
#else
//#define CS_PIN -1
//#define DC_PIN 0
//#define RESET_PIN 1
//#define CS_PIN 4
//#define DC_PIN 2
//#define RESET_PIN 3
//#define LED_PIN -1
// TTGO T-Camera Plus
//#define CS_PIN 12
//#define DC_PIN 15
//#define LED_PIN 2
//#define BUILTIN_SDCARD 0
//#define RESET_PIN -1
//#define MISO_PIN 22
//#define MOSI_PIN 19
//#define SCK_PIN 21
// Janzen Hub
#define CS_PIN 4
#define DC_PIN 12
#define LED_PIN 16
#define BUILTIN_SDCARD 0
#define RESET_PIN -1
#define MISO_PIN 19
#define MOSI_PIN 23
#define SCK_PIN 18
// Feather M0 proto
//#define CS_PIN 10
//#define DC_PIN 11
//#define LED_PIN -1
//#define RESET_PIN 9
//#define MISO_PIN -1
//#define MOSI_PIN -1
//#define SCK_PIN -1
#endif

uint8_t ucTXBuf[1024];
GIFIMAGE gif;
//uint8_t ucCurrent[128*128]; // current frame
//uint8_t ucPrevious[128*128];

#ifdef FUTURE
// Animate the new buffer into the old buffer
void GIFDispose(GIFIMAGE *pImage)
{
    uint8_t *s, *d;
    int x, y;

    // Dispose of the last frame according to the previous page disposal flags
//    switch (pImage->ucDisposalFlag)
   switch ((pImage->cGIFBits & 0x1c)>>2)
    {
        case 0: // not specified - nothing to do
        case 1: // do not dispose
            break;
        case 2: // restore to background color
            for (y=pImage->iY; y < (pImage->iY + pImage->iHeight); y++)
            {
               d = &ucCurrent[(y * 128) + pImage->iX];
               memset(d, pImage->cBackground, pImage->iWidth);
            }
            break;
        case 3: // restore to previous frame
            for (y=pImage->iY; y < (pImage->iY + pImage->iHeight); y++)
            {
              d = &ucCurrent[(y * 128) + pImage->iX];
              s = &ucPrevious[(y * 128) + pImage->iX];
              memcpy(d, s, pImage->iWidth);
            }
            break;
        default: // not defined
            break;
    }
} /* GIFDispose() */

void ShowFrame(GIFIMAGE *pImage)
{
int x, y;
uint8_t *s;
uint16_t *usPalette, *usTemp = (uint16_t *)ucTXBuf;

    usPalette = (pImage->bUseLocalPalette) ? pImage->pLocalPalette : pImage->pPalette;
    for (y=pImage->iY; y<(pImage->iY+pImage->iHeight); y++)
    {
      spilcdSetPosition(pImage->iX, y, pImage->iWidth, 1, 1);
      s = &ucCurrent[(y * 128)+pImage->iX];
      // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
      for (x=0; x<pImage->iWidth; x++)
        usTemp[x] = usPalette[*s++];
      spilcdWriteDataBlock((uint8_t *)usTemp, pImage->iWidth*2, 1);
    } // for y
} /* ShowFrame() */

// Draw a line of image
void GIFDraw(void *p)
{
    GIFIMAGE *pImage = (GIFIMAGE *)p;
    uint8_t *s, *d;
    int x, y = pImage->iY + (pImage->iHeight - pImage->iYCount); // current line
    
    s = pImage->ucLineBuf;
    d = &ucCurrent[(y * 128) + pImage->iX];
    // Apply the new pixels to the main image
    if (pImage->cGIFBits & 1) // if transparency used
    {
      uint8_t c, cTransparent = pImage->cTransparent;
      for (x=0; x<pImage->iWidth; x++)
      {
        c = *s++;
        if (c != cTransparent)
           *d = c;
        d++;
      }
    }
    else
    {    
      memcpy(d, s, pImage->iWidth);
    }
} /* GIFDraw() */
#endif

// Draw a line of image directly on the LCD
void GIFDrawNoMem(void *p)
{
    GIFIMAGE *pImage = (GIFIMAGE *)p;
    uint8_t *s;
    uint16_t *d, *usPalette, *usTemp = (uint16_t *)ucTXBuf;
    int x, y;
   
    usPalette = (pImage->bUseLocalPalette) ? pImage->pLocalPalette : pImage->pPalette;
    y = pImage->iY + (pImage->iHeight - pImage->iYCount); // current line
    
    s = pImage->ucLineBuf;
    // Apply the new pixels to the main image
    if (pImage->cGIFBits & 1) // if transparency used
    {
      uint8_t *pEnd, c, cTransparent = pImage->cTransparent;
      int x, iCount;
      pEnd = s + pImage->iWidth;
      x = 0;
      iCount = 0; // count non-transparent pixels
      while(x < pImage->iWidth)
      {
        c = cTransparent-1;
        d = usTemp;
        while (c != cTransparent && s < pEnd)
        {
          c = *s++;
          if (c == cTransparent) // done, stop
          {
            s--; // back up to treat it like transparent
          }
          else // opaque
          {
             *d++ = usPalette[c];
             iCount++;
          }
        } // while looking for opaque pixels
        if (iCount) // any opaque pixels?
        {
          spilcdSetPosition(pImage->iX+x, y, iCount, 1, 1);
          spilcdWriteDataBlock((uint8_t *)usTemp, iCount*2, 1);
          x += iCount;
          iCount = 0;
        }
        // no, look for a run of transparent pixels
        c = cTransparent;
        while (c == cTransparent && s < pEnd)
        {
          c = *s++;
          if (c == cTransparent)
             iCount++;
          else
             s--; 
        }
        if (iCount)
        {
          x += iCount; // skip these
          iCount = 0;
        }
      }
    }
    else
    {   
      spilcdSetPosition(pImage->iX, y, pImage->iWidth, 1, 1);
      s = pImage->ucLineBuf;
      // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
      for (x=0; x<pImage->iWidth; x++)
        usTemp[x] = usPalette[*s++];
      spilcdWriteDataBlock((uint8_t *)usTemp, pImage->iWidth*2, 1);
    }
} /* GIFDrawNoMem() */

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);

//SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, -1);
//#ifdef TEENSYDUINO
//  if(!SD.begin(BUILTIN_SDCARD))
//  {
//    Serial.println("SD Card mount failed!");
//    return;
//  }
//  else
//  {
//    Serial.println("SD Card mount succeeded!");
//  }
//#endif

  // put your setup code here, to run once:
  spilcdSetTXBuffer(ucTXBuf, sizeof(ucTXBuf));
//  spilcdInit(LCD_ST7789, 0, 0, 0, 32000000, CS_PIN, DC_PIN, RESET_PIN, LED_PIN, MISO_PIN, MOSI_PIN, SCK_PIN);
  spilcdInit(LCD_ILI9341, 0, 0, 0, 40000000, CS_PIN, DC_PIN, RESET_PIN, LED_PIN, MISO_PIN, MOSI_PIN, SCK_PIN);
//  spilcdInit(LCD_ST7735R, 0, 0, 1, 8000000, CS_PIN, DC_PIN, RESET_PIN, LED_PIN, MISO_PIN, MOSI_PIN, SCK_PIN); // custom ESP32 rig
//  spilcdInit(LCD_ST7735R, 0, 0, 1, 12000000, CS_PIN, DC_PIN, RESET_PIN, LED_PIN, MISO_PIN, MOSI_PIN, SCK_PIN); // custom ESP32 rig
  spilcdSetOrientation(LCD_ORIENTATION_ROTATED);
  spilcdFill(0,1);
} /* setup() */

void loop()
{
int rc, iFrame;
int iTime;

  if (!GIFInit(&gif, NULL, (uint8_t *)ucGIF3, sizeof(ucGIF3)))
//  if (!GIFInit(&gif, "/GIF/FUTURAMA.GIF", NULL, 0))
  {
     Serial.println("GIFInit failed!");
     while (1);
  }
  Serial.printf("File size = %d, Canvas size = %d x %d\n", gif.GIFFile.iSize, gif.iCanvasWidth, gif.iCanvasHeight);
  iFrame = 0;
  while (gif.GIFFile.iPos < gif.GIFFile.iSize-1) // go through the frames
  {
//    memcpy(ucPrevious, ucCurrent, sizeof(ucCurrent)); // keep last frame
    if (GIFParseInfo(&gif, 0))
    {
      iFrame++;
      iTime = millis();
      rc = DecodeLZW(&gif, 0);
      iTime = millis() - iTime;
      if (iTime < gif.iFrameDelay) // need to pause a bit
         delay(gif.iFrameDelay - iTime);
      Serial.printf("frame decode time = %dms\n", iTime);
      if (rc == 0) // decoded
      {
//        ShowFrame(&gif);
//        GIFDispose(&gif); // take care of disposal flags
      } // decoded successfully
    } // parsed successfully
    else
    {
      Serial.printf("GIFParse failed at frame %d\n", iFrame);
      delay(10000);
    }
  } // while decoding frames
  GIFTerminate(&gif);
  Serial.printf("Frame count = %d\n", iFrame);
//  delay(5000);
} /* loop() */
