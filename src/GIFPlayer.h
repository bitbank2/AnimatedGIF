//
// GIF Player
// A helper class to play GIF Animations on ESP32 MCUs
// Written by Larry Bank (bitbank@pobox.com)
//
// Although it is possible to display GIF animations using less memory,
// the results can be slow and/or have incorrect colors. This code
// allocates 3 x W x H bytes (an 8-bit and RGB565 canvas) to
// play all GIF animations successfully. The memory MUST come from PSRAM
// 
// Copyright 2025 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
#include "AnimatedGIF.h"
#include <bb_spi_lcd.h>
#include <SD.h>
#include "FS.h"
#include <LittleFS.h>

// To ask the player to center the frame
#define GIF_CENTER -2
static BB_SPI_LCD *_pLCD;
static int _x, _y;

class GIFPlayer
{
  public:
    int openData(BB_SPI_LCD *pLCD, const void *pData, int iDataSize);
    int openSD(BB_SPI_LCD *pLCD, const char *fname);
    int openLFS(BB_SPI_LCD *pLCD, const char *fname);
    int getInfo(int *width, int *height);
    int play(int x, int y, bool bDelay);
    int close();
protected:
    AnimatedGIF _gif;
};

static void * gifOpenSD(const char *filename, int32_t *size) {
  static File myfile;
  myfile = SD.open(filename);
  if (myfile) {
      *size = myfile.size();
      return &myfile;
  }
  return NULL;
}
static void * gifOpenLFS(const char *filename, int32_t *size) {
  static File myfile;
  myfile = LittleFS.open(filename, FILE_READ);
  if (myfile) {
      *size = myfile.size();
      return &myfile;
  }
  return NULL;
}
static void gifClose(void *handle) {
  File *pFile = (File *)handle;
  if (pFile) pFile->close();
}
static int32_t gifRead(GIFFILE *handle, uint8_t *buffer, int32_t length) {
    int32_t iBytesRead;
    iBytesRead = length;
    File *f = static_cast<File *>(handle->fHandle);
    // Note: If you read a file all the way to the last byte, seek() stops working
    if ((handle->iSize - handle->iPos) < length)
       iBytesRead = handle->iSize - handle->iPos - 1; // <-- ugly work-around
    if (iBytesRead <= 0)
       return 0;
    iBytesRead = (int32_t)f->read(buffer, iBytesRead);
    handle->iPos = f->position();
    return iBytesRead;
}
static int32_t gifSeek(GIFFILE *handle, int32_t iPosition) {
  File *f = static_cast<File *>(handle->fHandle);
  f->seek(iPosition);
  handle->iPos = (int32_t)f->position();
  return handle->iPos;
}

// Class implementation
int GIFPlayer::openData(BB_SPI_LCD *pLCD, const void *pData, int iDataSize)
{
    int rc;
    void *pBuf;
    _gif.begin(GIF_PALETTE_RGB565_BE);
    if (_gif.open((uint8_t *)pData, iDataSize, NULL)) {
        pBuf = ps_malloc(_gif.getCanvasWidth() * _gif.getCanvasHeight() * 3);
        if (!pBuf) {
            return GIF_ERROR_MEMORY;
        }
        _gif.setFrameBuf(pBuf);
        _gif.setDrawType(GIF_DRAW_COOKED);
    } else {
        return GIF_FILE_NOT_OPEN;
    }
    _pLCD = pLCD;
    return GIF_SUCCESS;
} /* openData() */

int GIFPlayer::openSD(BB_SPI_LCD *pLCD, const char *fname)
{
    int rc;
    void *pBuf;
    _gif.begin(GIF_PALETTE_RGB565_BE);
    if (_gif.open(fname, gifOpenSD, gifClose, gifRead, gifSeek, NULL)) {
        pBuf = ps_malloc(_gif.getCanvasWidth() * _gif.getCanvasHeight()*3);
        if (!pBuf) {
            _gif.close();
            return GIF_ERROR_MEMORY;
        }
        _gif.setFrameBuf(pBuf);
        _gif.setDrawType(GIF_DRAW_COOKED);
    }
    _pLCD = pLCD;
    return GIF_SUCCESS;
} /* openSD() */

int GIFPlayer::openLFS(BB_SPI_LCD *pLCD, const char *fname)
{
    int rc;
    void *pBuf;
    if (!LittleFS.begin(false)) {
        return GIF_FILE_NOT_OPEN;
    }
    _gif.begin(GIF_PALETTE_RGB565_BE);
    if (_gif.open(fname, gifOpenLFS, gifClose, gifRead, gifSeek, NULL)) {
        pBuf = ps_malloc(_gif.getCanvasWidth() * _gif.getCanvasHeight() * 3);
        if (!pBuf) {
            _gif.close();
            return GIF_ERROR_MEMORY;
        }
        _gif.setFrameBuf(pBuf);
        _gif.setDrawType(GIF_DRAW_COOKED);
    }
    _pLCD = pLCD;
    return GIF_SUCCESS;
} /* openLFS() */

int GIFPlayer::getInfo(int *width, int *height)
{
    if (_pLCD) { // initialized successfully?
        if (width) *width = _gif.getCanvasWidth();
        if (height) *height = _gif.getCanvasHeight();
        return GIF_SUCCESS;
    } else {
        return GIF_INVALID_PARAMETER;
    }
} /* getInfo() */

int GIFPlayer::play(int x, int y, bool bDelay)
{
    int rc, ty, cw, ch, w, h;
    int iX, iY;
    uint16_t *pPixels;
    if (x == GIF_CENTER) {
        x = (_pLCD->width() - _gif.getCanvasWidth())/2;
        if (x < 0) x = 0;
    }
    if (y == GIF_CENTER) {
        y = (_pLCD->height() - _gif.getCanvasHeight())/2;
        if (y < 0) y = 0;
    }
    _x = x; _y = y;
    cw = _gif.getCanvasWidth();
    ch = _gif.getCanvasHeight();
    w = _gif.getFrameWidth();
    h = _gif.getFrameHeight();
    iX = _gif.getFrameXOff();
    iY = _gif.getFrameYOff();
    rc = _gif.playFrame(bDelay, NULL);
    if (!rc) _gif.reset(); // loop forever
    // Update the display with the new pixels
    _pLCD->setAddrWindow(_x + iX, _y + iY, w, h);
        pPixels = (uint16_t *)(_gif.getFrameBuf() + (cw * ch)); // cooked pixels start here
        pPixels += iX + (iY * cw);
        // the frame is a sub-region of the canvas, so we can't push
        // the pixels in one shot
        for (ty = 0; ty < h; ty++) {
            _pLCD->pushPixels(pPixels, w);
            pPixels += cw; // canvas width to the next line
        } // for y
    return rc;
} /* play() */

int GIFPlayer::close()
{
    free(_gif.getFrameBuf());
    _gif.close();
} /* close() */
