// Demo sketch to play all GIF files in a directory
// Tested with SmartMatrix Library on Teensy 4.1 and ESP32

#include <AnimatedGIF.h>
#include <SD.h>

// uncomment one line to select your MatrixHardware configuration - configuration header needs to be included before <SmartMatrix3.h>
//#include <MatrixHardware_Teensy3_ShieldV4.h>        // SmartLED Shield for Teensy 3 (V4)
//#include <MatrixHardware_Teensy4_ShieldV5.h>        // SmartLED Shield for Teensy 4 (V5)
//#include <MatrixHardware_Teensy3_ShieldV1toV3.h>    // SmartMatrix Shield for Teensy 3 V1-V3
//#include <MatrixHardware_Teensy4_ShieldV4Adapter.h> // Teensy 4 Adapter attached to SmartLED Shield for Teensy 3 (V4)
//#include <MatrixHardware_ESP32_V0.h>                // This file contains multiple ESP32 hardware configurations, edit the file to define GPIOPINOUT (or add #define GPIOPINOUT with a hardcoded number before this #include)
//#include "MatrixHardware_Custom.h"                  // Copy an existing MatrixHardware file to your Sketch directory, rename, customize, and you can include it like this
#include <SmartMatrix3.h>

/* SmartMatrix configuration and memory allocation */
#define COLOR_DEPTH 24                  // known working: 24, 48 - If the sketch uses type `rgb24` directly, COLOR_DEPTH must be 24
const uint8_t kMatrixWidth = 128;        // known working: 32, 64, 96, 128
const uint8_t kMatrixHeight = 64;       // known working: 16, 32, 48, 64
const uint8_t kRefreshDepth = 36;       // known working: 24, 36, 48
const uint8_t kDmaBufferRows = 2;       // known working: 2-4
const uint8_t kPanelType = SMARTMATRIX_HUB75_64ROW_MOD32SCAN; // use SMARTMATRIX_HUB75_16ROW_MOD8SCAN for common 16x32 panels, or use SMARTMATRIX_HUB75_64ROW_MOD32SCAN for common 64x64 panels
const uint8_t kMatrixOptions = (SMARTMATRIX_OPTIONS_NONE);    // see http://docs.pixelmatix.com/SmartMatrix for options
const uint8_t kBackgroundLayerOptions = (SM_BACKGROUND_OPTIONS_NONE);
const uint8_t kScrollingLayerOptions = (SM_SCROLLING_OPTIONS_NONE);

SMARTMATRIX_ALLOCATE_BUFFERS(matrix, kMatrixWidth, kMatrixHeight, kRefreshDepth, kDmaBufferRows, kPanelType, kMatrixOptions);
SMARTMATRIX_ALLOCATE_BACKGROUND_LAYER(backgroundLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kBackgroundLayerOptions);

const int defaultBrightness = 255;

// SD Card (use BUILTIN_SDCARD for Teensy 3.5/3.6/4.1)
#if !defined(ESP32)
#define SD_CS_PIN BUILTIN_SDCARD
#else
#define SD_CS_PIN 5
#endif

#define DISPLAY_WIDTH kMatrixWidth
#define DISPLAY_HEIGHT kMatrixHeight

AnimatedGIF gif;
File f;
int x_offset, y_offset;

void DrawPixelRow(int startX, int y, int numPixels, rgb24 * data) {
  for(int i=0; i<numPixels; i++)
  {
    backgroundLayer.drawPixel(startX + i, y, *data);
    data++;
  }
}

// Draw a line of image directly on the LCD
void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  //uint16_t *d, *usPalette, usTemp[320];
  int x, y, iWidth;
  rgb24 *d, *usPalette, usTemp[320];

  iWidth = pDraw->iWidth;
  if (iWidth > DISPLAY_WIDTH)
    iWidth = DISPLAY_WIDTH;
  usPalette = (rgb24*)pDraw->pPalette;

  y = pDraw->iY + pDraw->y; // current line
  
  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x=0; x<iWidth; x++)
    {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }
  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) // if transparency used
  {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0; // count non-transparent pixels
    while(x < iWidth)
    {
      c = ucTransparent-1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent) // done, stop
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
        DrawPixelRow(pDraw->iX+x+x_offset, y+y_offset, iCount, (rgb24 *)usTemp);
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent)
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
    s = pDraw->pPixels;
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    for (x=0; x<iWidth; x++)
      usTemp[x] = usPalette[*s++];
      DrawPixelRow(pDraw->iX+x_offset, y+y_offset, iWidth, (rgb24 *)usTemp);
  }
} /* GIFDraw() */

void * GIFOpenFile(const char *fname, int32_t *pSize)
{
  f = SD.open(fname);
  if (f)
  {
    *pSize = f.size();
    return (void *)&f;
  }
  return NULL;
} /* GIFOpenFile() */

void GIFCloseFile(void *pHandle)
{
  File *f = static_cast<File *>(pHandle);
  if (f != NULL)
    f->close();
} /* GIFCloseFile() */

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  int32_t iBytesRead;
  iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  // Note: If you read a file all the way to the last byte, seek() stops working
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos - 1; // <-- ugly work-around
  if (iBytesRead <= 0)
    return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
} /* GIFReadFile() */

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition)
{ 
  int i = micros();
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  i = micros() - i;
  //Serial.printf("Seek time = %d us\n", i);
  return pFile->iPos;
} /* GIFSeekFile() */

void setup() {
  Serial.begin(115200);
  while (!Serial);

  matrix.addLayer(&backgroundLayer); 
  matrix.setBrightness(defaultBrightness);
  matrix.setRefreshRate(240);
  matrix.begin();

  // Clear screen
  backgroundLayer.fillScreen({0,0,0});
  backgroundLayer.swapBuffers(false);

  // Note: this sketch doesn't initialize SPI before calling SD.begin(), as it assumes default SPI pins are used
  pinMode(SD_CS_PIN, OUTPUT);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card init failed!");
    while (1); // SD initialisation failed so wait here
  }
  Serial.println("SD Card init success!");

  // using RGB888 = rgb24 palette instead of default RGB565
  gif.begin(BIG_ENDIAN_PIXELS, GIF_PALETTE_RGB888);
}

void ShowGIF(const char *name)
{
  int frameStatus;

  backgroundLayer.fillScreen({0,0,0});
  
  if (gif.open(name, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw))
  {
    x_offset = (DISPLAY_WIDTH - gif.getCanvasWidth())/2;
    if (x_offset < 0) x_offset = 0;
    y_offset = (DISPLAY_HEIGHT - gif.getCanvasHeight())/2;
    if (y_offset < 0) y_offset = 0;
    Serial.printf("Successfully opened GIF; Canvas size = %d x %d\n", gif.getCanvasWidth(), gif.getCanvasHeight());
#if 0
    GIFINFO gi;
    if (gif.getInfo(&gi)) {
      Serial.printf("frame count: %d\n", gi.iFrameCount);
      Serial.printf("duration: %d ms\n", gi.iDuration);
      Serial.printf("max delay: %d ms\n", gi.iMaxDelay);
      Serial.printf("min delay: %d ms\n", gi.iMinDelay);
    }
#endif
    Serial.flush();
    do
    {
      frameStatus = gif.playFrame(true, NULL);
      backgroundLayer.swapBuffers();
    } while (frameStatus > 0);
    if(frameStatus < 0) {
      Serial.print("playFrame failed: ");
      Serial.println(gif.getLastError());
    }
    gif.close();
  } else {
    Serial.print("open failed: ");
    Serial.println(gif.getLastError());
  }
} /* ShowGIF() */

//
// Return true if a file's leaf name starts with a special character (it's been erased)
//
int ErasedFile(char *fname)
{
  int iLen = strlen(fname);
  int i;
  for (i=iLen-1; i>0; i--) // find start of leaf name
  {
    if (fname[i] == '/')
      break;
  }
  // if fname includes the full path, the first character of the filename is i+1
  if(i>0)
    i++;
  return (fname[i] == '.' || fname[i] == '_' || fname[i] == '~'); // these characters tend to show up before erased files that aren't playable
}

void loop() {
#if !defined(ESP32)
  // the standard Arduino SD Library requires a trailing slash in the directory name
  const char *szDir = "/gifs/"; // play all GIFs in this directory on the SD card
#else  
  // ESP32 can't handle a trailing slash in the directory name
  const char *szDir = "/gifs"; // play all GIFs in this directory on the SD card
#endif

  char fname[256];
  File root, temp;

  while (1) // run forever
  {
    root = SD.open(szDir);
    if (root)
    {
      temp = root.openNextFile();
      while (temp)
      {
        if (!temp.isDirectory()) // play it
        {
#if !defined(ESP32)
            // the standard Arduino SD library returns the filename without path
            strcpy(fname, szDir);
            strcat(fname, temp.name());
#else
            // ESP32 already includes the path in the filename
            strcpy(fname, temp.name());
#endif
          if (!ErasedFile(fname))
          {
            Serial.printf("Playing %s\n", fname);
            Serial.flush();
            ShowGIF((char *)fname);
          }
        }
        temp.close();
        temp = root.openNextFile();
      }
      root.close();
    } else {
      Serial.print("Couldn't Open Directory: ");
      Serial.println(szDir);
    } // root
    delay(4000); // pause before restarting
  } // while
} /* loop() */
