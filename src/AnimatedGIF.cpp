//
// GIF Animator
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 7/5/2020
// Original GIF code written 20+ years ago :)
// The goal of this code is to decode images up to 480x320
// using no more than 22K of RAM (if sent directly to an LCD display)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "AnimatedGIF.h"

static const unsigned char cGIFBits[9] = {1,4,4,4,8,8,8,8,8}; // convert odd bpp values to ones we can handle
static const unsigned char cGIFPass[8] = {8,0,8,4,4,2,2,1}; // GIF interlaced y delta

// forward references
static int GIFInit(GIFIMAGE *pGIF);
static int GIFParseInfo(GIFIMAGE *pPage, int bInfoOnly);
static int GIFGetMoreData(GIFIMAGE *pPage);
static void GIFMakePels(GIFIMAGE *pPage, unsigned int code);
static int DecodeLZW(GIFIMAGE *pImage, int iOptions);

#if defined( __MACH__ ) || defined( __LINUX__ )
#include <time.h>

long millis()
{
long lTime;
struct timespec res;

    clock_gettime(CLOCK_MONOTONIC, &res);
    lTime = 1000*res.tv_sec + res.tv_nsec/1000000;

    return lTime;
} /* millis() */

void delay(int iDelay)
{
long d, lTime = millis();

    d = 0;
    while (d < iDelay)
    {
       d = millis() - lTime;
    }
} /* delay() */

#endif // Linux
//
// Helper functions for memory based images
//
static int32_t readMem(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    int32_t iBytesRead;

    iBytesRead = iLen;
    if ((pFile->iSize - pFile->iPos) < iLen)
       iBytesRead = pFile->iSize - pFile->iPos;
    if (iBytesRead <= 0)
       return 0;
    memcpy_P(pBuf, &pFile->pData[pFile->iPos], iBytesRead);
    pFile->iPos += iBytesRead;
    return iBytesRead;
} /* readMem() */

static int32_t seekMem(GIFFILE *pFile, int32_t iPosition)
{
    if (iPosition < 0) iPosition = 0;
    else if (iPosition >= pFile->iSize) iPosition = pFile->iSize-1;
    pFile->iPos = iPosition;
    return iPosition;
} /* seekMem() */

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

//
// File (SD/MMC) based initialization
//
int AnimatedGIF::open(char *szFilename, GIF_OPEN_CALLBACK *pfnOpen, GIF_CLOSE_CALLBACK *pfnClose, GIF_READ_CALLBACK *pfnRead, GIF_SEEK_CALLBACK *pfnSeek, GIF_DRAW_CALLBACK *pfnDraw)
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

void AnimatedGIF::begin(int iEndian)
{
    memset(&_gif, 0, sizeof(_gif));
    _gif.ucLittleEndian = (iEndian == LITTLE_ENDIAN_PIXELS);
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
long lTime = millis();
    
    if (_gif.GIFFile.iPos >= _gif.GIFFile.iSize-1) // no more data exists
    {
        (*_gif.pfnSeek)(&_gif.GIFFile, 0); // seek to start
    }
    if (GIFParseInfo(&_gif, 0))
    {
        rc = DecodeLZW(&_gif, 0);
        if (rc != 0) // problem
            return -1;
    }
    else
    {
        return -1; // error parsing the frame info
    }
    // Return 1 for more frames or 0 if this was the last frame
    if (bSync)
    {
        lTime = millis() - lTime;
        if (lTime < _gif.iFrameDelay) // need to pause a bit
           delay(_gif.iFrameDelay - lTime);
    }
    if (delayMilliseconds) // if not NULL, return the frame delay time
        *delayMilliseconds = _gif.iFrameDelay;
    return (_gif.GIFFile.iPos < _gif.GIFFile.iSize-1);
} /* playFrame() */
//
// The following functions are written in plain C and have no
// 3rd party dependencies, not even the C runtime library
//
//
// Initialize a GIF file and callback access from a file on SD or memory
// returns 1 for success, 0 for failure
// Fills in the canvas size of the GIFIMAGE structure
//
static int GIFInit(GIFIMAGE *pGIF)
{
    pGIF->GIFFile.iPos = 0; // start at beginning of file
    if (!GIFParseInfo(pGIF, 1)) // gather info for the first frame
       return 0; // something went wrong; not a GIF file?
    (*pGIF->pfnSeek)(&pGIF->GIFFile, 0); // seek back to start of the file
  return 1;
} /* GIFInit() */

//
// Parse the GIF header, gather the size and palette info
// If called with bInfoOnly set to true, it will test for a valid file
// and return the canvas size only
// Returns 1 for success, 0 for failure
//
static int GIFParseInfo(GIFIMAGE *pPage, int bInfoOnly)
{
    int i, j, iColorTableBits;
    int iBytesRead;
    unsigned char c, *p;
    int32_t iOffset = 0;
    int32_t iStartPos = pPage->GIFFile.iPos; // starting file position
    int iReadSize;
    
    pPage->bUseLocalPalette = 0; // assume no local palette
    pPage->bEndOfFrame = 0; // we're just getting started
    iReadSize = (bInfoOnly) ? 12 : MAX_CHUNK_SIZE;
    // If you try to read past the EOF, the SD lib will return garbage data
    if (iStartPos + iReadSize > pPage->GIFFile.iSize)
       iReadSize = (pPage->GIFFile.iSize - iStartPos - 1);
    p = pPage->ucFileBuf;
    iBytesRead =  (*pPage->pfnRead)(&pPage->GIFFile, pPage->ucFileBuf, iReadSize); // 255 is plenty for now

    if (iBytesRead != iReadSize) // we're at the end of the file
    {
       return 0;
    }
    if (iStartPos == 0) // start of the file
    { // canvas size
        if (memcmp(p, "GIF89", 5) != 0 && memcmp(p, "GIF87", 5) != 0) // not a GIF file
           return 0; 
        pPage->iCanvasWidth = pPage->iWidth = INTELSHORT(&p[6]);
        pPage->iCanvasHeight = pPage->iHeight = INTELSHORT(&p[8]);
        pPage->iBpp = ((p[10] & 0x70) >> 4) + 1;
        if (bInfoOnly)
           return 1; // we've got the info we needed, leave
        iColorTableBits = (p[10] & 7) + 1; // Log2(size) of the color table
        pPage->ucBackground = p[11]; // background color
        pPage->ucGIFBits = 0;
        iOffset = 13;
        if (p[10] & 0x80) // global color table?
        { // convert to byte-reversed RGB565 for immediate use
            // Read enough additional data for the color table
            iBytesRead += (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], 3*(1<<iColorTableBits));
            for (i=0; i<(1<<iColorTableBits); i++)
            {
                uint16_t usRGB565;
                usRGB565 = ((p[iOffset] >> 3) << 11); // R
                usRGB565 |= ((p[iOffset+1] >> 2) << 5); // G
                usRGB565 |= (p[iOffset+2] >> 3); // B
                if (pPage->ucLittleEndian)
                    pPage->pPalette[i] = usRGB565;
                else
                    pPage->pPalette[i] = __builtin_bswap16(usRGB565); // SPI wants MSB first
                iOffset += 3;
            }
        }
    }
    while (p[iOffset] != ',') /* Wait for image separator */
    {
        if (p[iOffset] == '!') /* Extension block */
        {
            iOffset++;
            switch(p[iOffset++]) /* Block type */
            {
                case 0xf9: /* Graphic extension */
                    if (p[iOffset] == 4) // correct length
                    {
                        pPage->ucGIFBits = p[iOffset+1]; // packed fields
                        pPage->iFrameDelay = (INTELSHORT(&p[iOffset+2]))*10; // delay in ms
                        if (pPage->iFrameDelay < 30) // 0-2 is going to make it run at 60fps; use 100 (10fps) as a reasonable substitute
                           pPage->iFrameDelay = 100;
                        if (pPage->ucGIFBits & 1) // transparent color is used
                            pPage->ucTransparent = p[iOffset+4]; // transparent color index
                        iOffset += 6;
                    }
                    //                     else   // error
                    break;
                case 0xff: /* App extension */
                    c = 1;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
                        if ((iBytesRead - iOffset) < (c+32)) // need to read more data first
                        {
                            memcpy(pPage->ucFileBuf, &pPage->ucFileBuf[iOffset], (iBytesRead-iOffset)); // move existing data down
                            iBytesRead -= iOffset;
                            iStartPos += iOffset;
                            iOffset = 0;
                            iBytesRead += (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], c+32);
                        }
                        if (c == 11) // fixed block length
                        { // Netscape app block contains the repeat count
                            if (memcmp(&p[iOffset], "NETSCAPE2.0", 11) == 0)
                            {
                                //                              if (p[iOffset+11] == 3 && p[iOffset+12] == 1) // loop count
                                //                                  pPage->iRepeatCount = INTELSHORT(&p[iOffset+13]);
                            }
                        }
                        iOffset += (int)c; /* Skip to next sub-block */
                    }
                    break;
                case 0x01: /* Text extension */
                    c = 1;
                    j = 0;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
                        if (j == 0) // use only first block
                        {
                            j = c;
                            if (j > 127)   // max comment length = 127
                                j = 127;
                            //                           memcpy(pPage->szInfo1, &p[iOffset], j);
                            //                           pPage->szInfo1[j] = '\0';
                            j = 1;
                        }
                        iOffset += (int)c; /* Skip this sub-block */
                    }
                    break;
                case 0xfe: /* Comment */
                    c = 1;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
                        if (pPage->iCommentPos == 0) // Save first block info
                        {
                            pPage->iCommentPos = iStartPos + iOffset;
                            pPage->sCommentLen = c;
                        }
                        iOffset += (int)c; /* Skip this sub-block */
                    }
                    break;
                default:
                    /* Bad header info */
                    return 0;
            } /* switch */
        }
        else // invalid byte, stop decoding
        {
            /* Bad header info */
            return 0;
        }
    } /* while */
    if (p[iOffset] == ',')
        iOffset++;
    // This particular frame's size and position on the main frame (if animated)
    pPage->iX = INTELSHORT(&p[iOffset]);
    pPage->iY = INTELSHORT(&p[iOffset+2]);
    pPage->iWidth = INTELSHORT(&p[iOffset+4]);
    pPage->iHeight = INTELSHORT(&p[iOffset+6]);
    iOffset += 8;
    
    /* Image descriptor
     7 6 5 4 3 2 1 0    M=0 - use global color map, ignore pixel
     M I 0 0 0 pixel    M=1 - local color map follows, use pixel
     I=0 - Image in sequential order
     I=1 - Image in interlaced order
     pixel+1 = # bits per pixel for this image
     */
    pPage->ucMap = p[iOffset++];
    if (pPage->ucMap & 0x80) // local color table?
    {// convert to byte-reversed RGB565 for immediate use
        j = (1<<((pPage->ucMap & 7)+1));
        // Read enough additional data for the color table
        iBytesRead += (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], j*3);            
        for (i=0; i<j; i++)
        {
            uint16_t usRGB565;
            usRGB565 = ((p[iOffset] >> 3) << 11); // R
            usRGB565 |= ((p[iOffset+1] >> 2) << 5); // G
            usRGB565 |= (p[iOffset+2] >> 3); // B
            if (pPage->ucLittleEndian)
                pPage->pLocalPalette[i] = usRGB565;
            else
                pPage->pLocalPalette[i] = __builtin_bswap16(usRGB565); // SPI wants MSB first
            iOffset += 3;
        }
        pPage->bUseLocalPalette = 1;
    }
    pPage->ucCodeStart = p[iOffset++]; /* initial code size */
    /* Since GIF can be 1-8 bpp, we only allow 1,4,8 */
    pPage->iBpp = cGIFBits[pPage->ucCodeStart];
    // we are re-using the same buffer turning GIF file data
    // into "pure" LZW
   pPage->iLZWSize = 0; // we're starting with no LZW data yet
   c = 1; // get chunk length
   while (c && iOffset < iBytesRead)
   {
//     Serial.printf("iOffset=%d, iBytesRead=%d\n", iOffset, iBytesRead);
     c = p[iOffset++]; // get chunk length
//     Serial.printf("Chunk size = %d\n", c);
     if (c <= (iBytesRead - iOffset))
     {
       memcpy(&pPage->ucLZW[pPage->iLZWSize], &p[iOffset], c);
       pPage->iLZWSize += c;
       iOffset += c;
     }
     else // partial chunk in our buffer
     {
       int iPartialLen = (iBytesRead - iOffset);
       memcpy(&pPage->ucLZW[pPage->iLZWSize], &p[iOffset], iPartialLen);
       pPage->iLZWSize += iPartialLen;
       iOffset += iPartialLen;
       (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucLZW[pPage->iLZWSize], c - iPartialLen);
       pPage->iLZWSize += (c - iPartialLen);
     }
     if (c == 0)
        pPage->bEndOfFrame = 1; // signal not to read beyond the end of the frame
   }
// seeking on an SD card is VERY VERY SLOW, so use the data we've already read by de-chunking it
// in this case, there's too much data, so we have to seek backwards a bit
   if (iOffset < iBytesRead)
   {
//     Serial.printf("Need to seek back %d bytes\n", iBytesRead - iOffset);
     (*pPage->pfnSeek)(&pPage->GIFFile, iStartPos + iOffset); // position file to new spot
   }
    return 1; // we are now at the start of the chunk data
} /* GIFParseInfo() */
//
// Unpack more chunk data for decoding
// returns 1 to signify more data available for this image
// 0 indicates there is no more data
//
static int GIFGetMoreData(GIFIMAGE *pPage)
{
    unsigned char c = 1;
    // move any existing data down
    if (pPage->bEndOfFrame || (pPage->iLZWSize - pPage->iLZWOff) >= (LZW_BUF_SIZE - MAX_CHUNK_SIZE))
        return 1; // frame is finished or buffer is already full; no need to read more data
    if (pPage->iLZWOff != 0)
    {
      memcpy(pPage->ucLZW, &pPage->ucLZW[pPage->iLZWOff], pPage->iLZWSize - pPage->iLZWOff);
      pPage->iLZWSize -= pPage->iLZWOff;
      pPage->iLZWOff = 0;
    }
    while (c && pPage->GIFFile.iPos < pPage->GIFFile.iSize && pPage->iLZWSize < (LZW_BUF_SIZE-MAX_CHUNK_SIZE))
    {
        (*pPage->pfnRead)(&pPage->GIFFile, &c, 1); // current length
        (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucLZW[pPage->iLZWSize], c);
        pPage->iLZWSize += c;
    }
    if (c == 0) // end of frame
        pPage->bEndOfFrame = 1;
    return (c != 0 && pPage->GIFFile.iPos < pPage->GIFFile.iSize); // more data available?
} /* GIFGetMoreData() */

//
// GIFMakePels
//
static void GIFMakePels(GIFIMAGE *pPage, unsigned int code)
{
    int iPixCount;
    unsigned short *giftabs;
    unsigned char *buf, *s, *pEnd, *gifpels;

    /* Copy this string of sequential pixels to output buffer */
    //   iPixCount = 0;
    s = pPage->ucFileBuf + FILE_BUF_SIZE; /* Pixels will come out in reversed order */
    buf = pPage->ucLineBuf + (pPage->iWidth - pPage->iXCount);
    giftabs = pPage->usGIFTable;
    gifpels = &pPage->ucGIFPixels[PIXEL_LAST];
    while (code < LINK_UNUSED)
    {
        if (s == pPage->ucFileBuf) /* Houston, we have a problem */
        {
            return; /* Exit with error */
        }
        *(--s) = gifpels[code];
        code = giftabs[code];
    }
    iPixCount = (int)(intptr_t)(pPage->ucFileBuf + FILE_BUF_SIZE - s);
    
    while (iPixCount && pPage->iYCount > 0)
    {
        if (pPage->iXCount > iPixCount)  /* Pixels fit completely on the line */
        {
                //            memcpy(buf, s, iPixCount);
                //            buf += iPixCount;
                pEnd = buf + iPixCount;
                while (buf < pEnd)
                {
                    *buf++ = *s++;
                }
            pPage->iXCount -= iPixCount;
            //         iPixCount = 0;
            return;
        }
        else  /* Pixels cross into next line */
        {
            GIFDRAW gd;
            pEnd = buf + pPage->iXCount;
            while (buf < pEnd)
            {
                *buf++ = *s++;
            }
            iPixCount -= pPage->iXCount;
            pPage->iXCount = pPage->iWidth; /* Reset pixel count */
            // Prepare GIDRAW structure for callback
            gd.iX = pPage->iX;
            gd.iY = pPage->iY;
            gd.iWidth = pPage->iWidth;
            gd.iHeight = pPage->iHeight;
            gd.pPixels = pPage->ucLineBuf;
            gd.pPalette = (pPage->bUseLocalPalette) ? pPage->pLocalPalette : pPage->pPalette;
            gd.y = pPage->iHeight - pPage->iYCount;
            gd.ucDisposalMethod = (pPage->ucGIFBits & 0x1c)>>2;
            gd.ucTransparent = pPage->ucTransparent;
            gd.ucHasTransparency = pPage->ucGIFBits & 1;
            gd.ucBackground = pPage->ucBackground;
            (*pPage->pfnDraw)(&gd); // callback to handle this line
            pPage->iYCount--;
            buf = pPage->ucLineBuf;
            if ((pPage->iYCount & 3) == 0) // since we support only small images...
                GIFGetMoreData(pPage); // check if we need to read more LZW data every 4 lines
        }
    } /* while */
    return;
} /* GIFMakePels() */
//
// Macro to extract a variable length code
//
#define GET_CODE if (bitnum > (REGISTER_WIDTH - codesize)) { pImage->iLZWOff += (bitnum >> 3); \
            bitnum &= 7; ulBits = INTELLONG(&p[pImage->iLZWOff]); } \
        code = (unsigned short) (ulBits >> bitnum); /* Read a 32-bit chunk */ \
        code &= sMask; bitnum += codesize;

//
// Decode LZW into an image
//
static int DecodeLZW(GIFIMAGE *pImage, int iOptions)
{
    int i, bitnum;
    unsigned short oldcode, codesize, nextcode, nextlim;
    unsigned short *giftabs, cc, eoi;
    signed short sMask;
    unsigned char *gifpels, *p;
    //    int iStripSize;
    //unsigned char **index;
    uint32_t ulBits;
    unsigned short code;
    // if output can be used for string table, do it faster
    //       if (bGIF && (OutPage->cBitsperpixel == 8 && ((OutPage->iWidth & 3) == 0)))
    //          return PILFastLZW(InPage, OutPage, bGIF, iOptions);
    p = pImage->ucLZW; // un-chunked LZW data
    sMask = -1 << (pImage->ucCodeStart + 1);
    sMask = 0xffff - sMask;
    cc = (sMask >> 1) + 1; /* Clear code */
    eoi = cc + 1;
    giftabs = pImage->usGIFTable;
    gifpels = pImage->ucGIFPixels;
    pImage->iYCount = pImage->iHeight; // count down the lines
    pImage->iXCount = pImage->iWidth;
    bitnum = 0;
    pImage->iLZWOff = 0; // Offset into compressed data
    GIFGetMoreData(pImage); // Read some data to start

    // Initialize code table
    // this part only needs to be initialized once
    for (i = 0; i < cc; i++)
    {
        gifpels[PIXEL_FIRST + i] = gifpels[PIXEL_LAST + i] = (unsigned short) i;
        giftabs[i] = LINK_END;
    }
init_codetable:
    codesize = pImage->ucCodeStart + 1;
    sMask = -1 << (pImage->ucCodeStart + 1);
    sMask = 0xffff - sMask;
    nextcode = cc + 2;
    nextlim = (unsigned short) ((1 << codesize));
    // This part of the table needs to be reset multiple times
    memset(&giftabs[cc], LINK_UNUSED, (4096 - cc)*sizeof(short));
    ulBits = INTELLONG(&p[pImage->iLZWOff]); // start by reading 4 bytes of LZW data
    GET_CODE
    if (code == cc) // we just reset the dictionary, so get another code
    {
      GET_CODE
    }
    oldcode = code;
    GIFMakePels(pImage, code); // first code is output as the first pixel
    // Main decode loop
    while (code != eoi && pImage->iYCount > 0) // && y < pImage->iHeight+1) /* Loop through all lines of the image (or strip) */
    {
        GET_CODE
        if (code == cc) /* Clear code?, and not first code */
            goto init_codetable;
        if (code != eoi)
        {
                if (nextcode < nextlim) // for deferred cc case, don't let it overwrite the last entry (fff)
                {
                    giftabs[nextcode] = oldcode;
                    gifpels[PIXEL_FIRST + nextcode] = gifpels[PIXEL_FIRST + oldcode];
                    if (giftabs[code] == LINK_UNUSED) /* Old code */
                        gifpels[PIXEL_LAST + nextcode] = gifpels[PIXEL_FIRST + oldcode];
                    else
                        gifpels[PIXEL_LAST + nextcode] = gifpels[PIXEL_FIRST + code];
                }
                nextcode++;
                if (nextcode >= nextlim && codesize < 12)
                {
                    codesize++;
                    nextlim <<= 1;
                    sMask = (sMask << 1) | 1;
                }
            GIFMakePels(pImage, code);
            oldcode = code;
        }
    } /* while not end of LZW code stream */
#ifdef NOT_USED
    if (pImage->cMap & 0x40) /* Interlaced? */
    { /* re-order the scan lines */
        unsigned char *buf, *buf2;
        int y, bitoff, iDest, iGifPass = 0;
        i = lsize * pImage->iHeight; /* color bitmap size */
        buf = (unsigned char *) pImage->pPixels; /* reset ptr to start of bitmap */
        buf2 = (unsigned char *) malloc(i);
        if (buf2 == NULL)
        {
            free(pImage->pPixels);
            pImage->pPixels = NULL;
            return -1;
        }
        y = 0;
        for (i = 0; i<pImage->iHeight; i++)
        {
            iDest = y * lsize;
            bitoff = i * lsize;
            memcpy(&buf2[iDest], &buf[bitoff], lsize);
            y += cGIFPass[iGifPass * 2];
            if (y >= pImage->iHeight)
            {
                iGifPass++;
                y = cGIFPass[iGifPass * 2 + 1];
            }
        }
        free(buf); /* Free the old buffer */
        i = lsize * pImage->iHeight; /* color bitmap size */
        pImage->pPixels = buf2; /* Replace with corrected bitmap */
    } /* If interlaced GIF */
#endif // NOT_USED
    return 0;
//gif_forced_error:
//    free(pImage->pPixels);
//    pImage->pPixels = NULL;
//    return -1;
} /* DecodeLZW() */
