//
// GIF Animator
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 7/5/2020
// Original GIF code written 20+ years ago :)
// The goal of this code is to decode images up to 480x320
// using no more than 22K of RAM (if sent directly to an LCD display)
//
// Copyright 2020 BitBank Software, Inc. All Rights Reserved.
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

// Here is all of the actual code...
#include "gif.c"

//
// Memory initialization
//
int AnimatedGIF::open(uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw)
{
    _gif.pfnRead = readMem;
    _gif.pfnSeek = seekMem;
    _gif.pfnDraw = pfnDraw;
    _gif.pfnOpen = NULL;
    _gif.pfnClose = NULL;
    _gif.GIFFile.iSize = iDataSize;
    _gif.GIFFile.pData = pData;
    return GIFInit(&_gif);
} /* open() */

//
// Returns the first comment block found (if any)
int AnimatedGIF::getComment(char *pDest)
{
int32_t iOldPos;

    iOldPos = _gif.GIFFile.iPos; // keep old position
    (*_gif.pfnSeek)(&_gif.GIFFile, _gif.iCommentPos);
    (*_gif.pfnRead)(&_gif.GIFFile, (uint8_t *)pDest, _gif.sCommentLen);
    (*_gif.pfnSeek)(&_gif.GIFFile, iOldPos);
    pDest[_gif.sCommentLen] = 0; // zero terminate the string
    return (int)_gif.sCommentLen;
} /* getComment() */

int AnimatedGIF::getCanvasWidth()
{
    return _gif.iCanvasWidth;
} /* getCanvasWidth() */

int AnimatedGIF::getCanvasHeight()
{
    return _gif.iCanvasHeight;
} /* getCanvasHeight() */

int AnimatedGIF::getInfo(GIFINFO *pInfo)
{
   return GIFGetInfo(&_gif, pInfo);
} /* getInfo() */

int AnimatedGIF::getLastError()
{
    return _gif.iError;
} /* getLastError() */

//
// File (SD/MMC) based initialization
//
int AnimatedGIF::open(const char *szFilename, GIF_OPEN_CALLBACK *pfnOpen, GIF_CLOSE_CALLBACK *pfnClose, GIF_READ_CALLBACK *pfnRead, GIF_SEEK_CALLBACK *pfnSeek, GIF_DRAW_CALLBACK *pfnDraw)
{
    _gif.pfnRead = pfnRead;
    _gif.pfnSeek = pfnSeek;
    _gif.pfnDraw = pfnDraw;
    _gif.pfnOpen = pfnOpen;
    _gif.pfnClose = pfnClose;
    _gif.GIFFile.fHandle = (*pfnOpen)(szFilename, &_gif.GIFFile.iSize);
    if (_gif.GIFFile.fHandle == NULL)
       return 0;
    return GIFInit(&_gif);

} /* open() */

void AnimatedGIF::close()
{
    if (_gif.pfnClose)
        (*_gif.pfnClose)(_gif.GIFFile.fHandle);
} /* close() */

void AnimatedGIF::reset()
{
    (*_gif.pfnSeek)(&_gif.GIFFile, 0);
} /* reset() */

void AnimatedGIF::begin(int iEndian, unsigned char ucPaletteType)
{
    memset(&_gif, 0, sizeof(_gif));
    if (iEndian < LITTLE_ENDIAN_PIXELS || iEndian > BIG_ENDIAN_PIXELS)
        _gif.iError = GIF_INVALID_PARAMETER;
    if (ucPaletteType != GIF_PALETTE_RGB565 && ucPaletteType != GIF_PALETTE_RGB888)
	_gif.iError = GIF_INVALID_PARAMETER;
    _gif.ucLittleEndian = (iEndian == LITTLE_ENDIAN_PIXELS);
    _gif.ucPaletteType = ucPaletteType;
}
//
// Play a single frame
// returns:
// 1 = good result and more frames exist
// 0 = good result and no more frames exist
// -1 = error
int AnimatedGIF::playFrame(bool bSync, int *delayMilliseconds)
{
int rc;
#if !defined( __MACH__ ) && !defined( __LINUX__ )
long lTime = millis();
#endif

    if (_gif.GIFFile.iPos >= _gif.GIFFile.iSize-1) // no more data exists
    {
        (*_gif.pfnSeek)(&_gif.GIFFile, 0); // seek to start
    }
    if (GIFParseInfo(&_gif, 0))
    {
        rc = DecodeLZW(&_gif, 0);
        if (rc != 0) // problem
            return 0;
    }
    else
    {
        return 0; // error parsing the frame info, we may be at the end of the file
    }
    // Return 1 for more frames or 0 if this was the last frame
    if (bSync)
    {
#if !defined( __MACH__ ) && !defined( __LINUX__ ) 
        lTime = millis() - lTime;
        if (lTime < _gif.iFrameDelay) // need to pause a bit
           delay(_gif.iFrameDelay - lTime);
#endif // __LINUX__
    }
    if (delayMilliseconds) // if not NULL, return the frame delay time
        *delayMilliseconds = _gif.iFrameDelay;
    return (_gif.GIFFile.iPos < _gif.GIFFile.iSize-1);
} /* playFrame() */
