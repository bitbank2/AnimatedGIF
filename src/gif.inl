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
//
// Modified 2026 in AnimatedGIF-LowMem (https://github.com/davicorrea0/AnimatedGIF-LowMem):
//   - Sequential bounds-checked parser, safe GIF_getInfo() / getComment()
//   - Checked partial-read I/O, bit accumulator reader, LZW bounds/saturation hardening
//   - Optional transient per-frame work buffers and idempotent close() cleanup
//===========================================================================
#include "AnimatedGIF.h"

#if defined( HAL_ESP32_HAL_H_ )
#define memcpy_P memcpy
#pragma GCC optimize("O2")
#endif
#if (__ARM_ARCH >= 7) || defined(__arm64__) || defined(__aarch64__)
#include <arm_neon.h>
#define HAS_NEON
#endif 

#if GIF_ENABLE_COOPERATIVE
#if defined(__LINUX__) || defined(__MACH__)
#include <time.h>
#endif
#ifndef GIF_MICROS
#if defined(ARDUINO)
#define GIF_MICROS() ((uint32_t)micros())
#elif defined(__LINUX__) || defined(__MACH__)
static inline uint32_t GIFGetMicros(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL);
#else
    return (uint32_t)(((uint64_t)clock() * 1000000ULL) / (uint64_t)CLOCKS_PER_SEC);
#endif
}
#define GIF_MICROS() GIFGetMicros()
#else
#define GIF_MICROS() ((uint32_t)0)
#endif
#endif // GIF_MICROS
#endif // GIF_ENABLE_COOPERATIVE

static const unsigned char cGIFBits[9] = {1,4,4,4,8,8,8,8,8}; // convert odd bpp values to ones we can handle
// forward references
static int GIFInit(GIFIMAGE *pGIF);
static int GIFParseInfo(GIFIMAGE *pPage, int bInfoOnly);
static int GIFGetMoreData(GIFIMAGE *pPage);
static int GIFReadExact(GIFIMAGE *g, uint8_t *dst, int n);
static int GIFSkipExact(GIFIMAGE *g, int n);
static int GIFGetCommentImpl(GIFIMAGE *pGIF, char *pDest);

static int DecodeLZW(GIFIMAGE *pImage, int iOptions);
#if !GIF_DISABLE_TURBO
static int DecodeLZWTurbo(GIFIMAGE *pImage, int iOptions);
#endif
static int32_t readMem(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
static int32_t seekMem(GIFFILE *pFile, int32_t iPosition);
int GIF_getInfo(GIFIMAGE *pPage, GIFINFO *pInfo);
#if GIF_TRANSIENT_WORK
static void GIFWorkEnd(GIFIMAGE *g)
{
    if (!g) return;
#if GIF_DICT_MODE == 12
    GIF_FREE(g->ucGIFPrefix12);
    g->ucGIFPrefix12 = NULL;
#else
    GIF_FREE(g->usGIFTable);
    g->usGIFTable = NULL;
#endif
    GIF_FREE(g->ucGIFPixels);
    GIF_FREE(g->ucFileBuf);
    GIF_FREE(g->pSmallWork);
    g->ucGIFPixels = NULL;
    g->ucFileBuf = NULL;
    g->pSmallWork = NULL;
    g->pLocalPalette = NULL;
    g->ucLZW = NULL;
    g->ucLineBuf = NULL;
    g->pLineBufAligned = NULL;
} /* GIFWorkEnd() */

static int GIFWorkBegin(GIFIMAGE *g)
{
    uint8_t *w;
    uintptr_t addr, rem;
    if (!g) return 0;
    if (g->pSmallWork) {
        if (g->pLocalPalette) memset(g->pLocalPalette, 0, GIF_PAL_BYTES);
        return 1;
    }
#if GIF_DICT_MODE == 12
    g->ucGIFPrefix12 = (uint8_t *)GIF_MALLOC(GIF_PREFIX12_BYTES);
#else
    g->usGIFTable = (uint16_t *)GIF_MALLOC(GIF_TABLE_BYTES);
#endif
    g->ucGIFPixels = (uint8_t *)GIF_MALLOC(GIF_PIXELS_BYTES);
    g->ucFileBuf = (uint8_t *)GIF_MALLOC((size_t)FILE_BUF_SIZE);
    g->pSmallWork = GIF_CALLOC(1, GIF_SMALL_WORK_BYTES);
#if GIF_DICT_MODE == 12
    if (!g->ucGIFPrefix12 || !g->ucGIFPixels || !g->ucFileBuf || !g->pSmallWork) {
#else
    if (!g->usGIFTable || !g->ucGIFPixels || !g->ucFileBuf || !g->pSmallWork) {
#endif
        GIFWorkEnd(g);
        g->iError = GIF_ERROR_MEMORY;
        return 0;
    }
    w = (uint8_t *)g->pSmallWork;
    g->pLocalPalette = (uint16_t *)w;
    g->ucLZW = w + GIF_PAL_BYTES;
    g->ucLineBuf = g->ucLZW + LZW_BUF_SIZE;
    addr = (uintptr_t)g->ucLineBuf;
    rem = addr & 15U;
    g->pLineBufAligned = rem ? (g->ucLineBuf + (16U - rem)) : g->ucLineBuf;
    return 1;
} /* GIFWorkBegin() */
#endif // GIF_TRANSIENT_WORK
#if defined( PICO_BUILD ) || defined( __LINUX__ ) || defined( __MCUXPRESSO )
static int32_t readFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
static int32_t seekFile(GIFFILE *pFile, int32_t iPosition);
static void closeFile(void *handle);


// C API
int GIF_openRAM(GIFIMAGE *pGIF, uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw)
{
    if (!pGIF || !pData || iDataSize <= 0) {
        if (pGIF) pGIF->iError = GIF_INVALID_PARAMETER;
        return 0;
    }
    pGIF->iError = GIF_SUCCESS;
    pGIF->pfnRead = readMem;
    pGIF->pfnSeek = seekMem;
    pGIF->pfnDraw = pfnDraw;
    pGIF->pfnOpen = NULL;
    pGIF->pfnClose = NULL;
    pGIF->GIFFile.iSize = iDataSize;
    pGIF->GIFFile.pData = pData;
    return GIFInit(pGIF);
} /* GIF_openRAM() */

#ifdef __LINUX__
int GIF_openFile(GIFIMAGE *pGIF, const char *szFilename, GIF_DRAW_CALLBACK *pfnDraw)
{
    long sz;
    if (!pGIF || !szFilename) {
        if (pGIF) pGIF->iError = GIF_INVALID_PARAMETER;
        return 0;
    }
    pGIF->iError = GIF_SUCCESS;
    pGIF->pfnRead = readFile;
    pGIF->pfnSeek = seekFile;
    pGIF->pfnDraw = pfnDraw;
    pGIF->pfnOpen = NULL;
    pGIF->pfnClose = NULL;
    pGIF->GIFFile.fHandle = fopen(szFilename, "rb");
    if (pGIF->GIFFile.fHandle == NULL) {
       pGIF->iError = GIF_FILE_NOT_OPEN;
       return 0;
    }
    fseek((FILE *)pGIF->GIFFile.fHandle, 0, SEEK_END);
    sz = ftell((FILE *)pGIF->GIFFile.fHandle);
    fseek((FILE *)pGIF->GIFFile.fHandle, 0, SEEK_SET);
    if (sz <= 0 || sz > INT32_MAX) {
        fclose((FILE *)pGIF->GIFFile.fHandle);
        pGIF->GIFFile.fHandle = NULL;
        pGIF->iError = GIF_BAD_FILE;
        return 0;
    }
    pGIF->pfnClose = closeFile;
    pGIF->GIFFile.iSize = (int32_t)sz;
    return GIFInit(pGIF);
} /* GIF_openFile() */
#endif

void GIF_close(GIFIMAGE *pGIF)
{
    if (!pGIF) return;
    if (pGIF->pfnClose) {
        GIF_CLOSE_CALLBACK *fn = pGIF->pfnClose;
        void *handle = pGIF->GIFFile.fHandle;
        pGIF->pfnClose = NULL;
        pGIF->GIFFile.fHandle = NULL;
        (*fn)(handle);
    }
#if GIF_TRANSIENT_WORK
    GIFWorkEnd(pGIF);
#endif
#if GIF_ENABLE_COOPERATIVE
    pGIF->ucCoopState = 0;
#endif
} /* GIF_close() */

void GIF_begin(GIFIMAGE *pGIF, unsigned char ucPaletteType)
{
#if !GIF_TRANSIENT_WORK
    uint8_t *p;
    uintptr_t u;
#endif
    /* C callers must call GIF_close() before reusing an already-opened GIFIMAGE */
    memset(pGIF, 0, sizeof(GIFIMAGE));
#if GIF_RGB565_ONLY
    if (ucPaletteType != GIF_PALETTE_RGB565_LE && ucPaletteType != GIF_PALETTE_RGB565_BE)
        pGIF->iError = GIF_UNSUPPORTED_FEATURE;
#else
    if (ucPaletteType > GIF_PALETTE_1BPP_OLED)
        pGIF->iError = GIF_INVALID_PARAMETER;
#endif
    pGIF->ucPaletteType = ucPaletteType;
    pGIF->ucDrawType = GIF_DRAW_RAW;
#if !GIF_TRANSIENT_WORK
    p = &pGIF->ucLineBuf[0];
    u = ((uintptr_t)p) & 15U;
    if (u > 0) p += (16U - u);
    pGIF->pLineBufAligned = p;
#endif
} /* GIF_begin() */

void GIF_reset(GIFIMAGE *pGIF)
{
    pGIF->iError = GIF_SUCCESS;
#if GIF_ENABLE_COOPERATIVE
    pGIF->ucCoopState = 0;
#endif
#if GIF_TRANSIENT_WORK
    GIFWorkEnd(pGIF);
#endif
    (*pGIF->pfnSeek)(&pGIF->GIFFile, 0);
} /* GIF_reset() */
//
// Return value:
// 1 = good decode, more frames exist
// 0 = good decode, no more frames
// -1 = error
//
int GIF_playFrame(GIFIMAGE *pGIF, int *delayMilliseconds, void *pUser)
{
int rc;
int ret = 0;

    if (delayMilliseconds)
       *delayMilliseconds = 0; // clear any old valid
#if GIF_TRANSIENT_WORK
    if (!GIFWorkBegin(pGIF))
        return -1;
#endif
    if (pGIF->GIFFile.iPos >= pGIF->GIFFile.iSize-1) // no more data exists
    {   
        (*pGIF->pfnSeek)(&pGIF->GIFFile, 0); // seek to start
    }
    if (GIFParseInfo(pGIF, 0))
    {
        pGIF->pUser = pUser;
        if (pGIF->iError == GIF_EMPTY_FRAME) // don't try to decode it
            goto done;
#if !GIF_DISABLE_TURBO
        if (pGIF->pTurboBuffer) { // the presence of the Turbo buffer indicates Turbo mode
            rc = DecodeLZWTurbo(pGIF, 0);
        } else
#endif
        {
            rc = DecodeLZW(pGIF, 0);
        }
        if (rc != 0) // problem
            goto done;
    }
    else
    {
        goto done; // error parsing the frame info, we may be at the end of the file
    }
    // Return 1 for more frames or 0 if this was the last frame
    if (delayMilliseconds) // if not NULL, return the frame delay time
        *delayMilliseconds = pGIF->iFrameDelay;
    ret = (pGIF->GIFFile.iPos < pGIF->GIFFile.iSize-1);
done:
#if GIF_TRANSIENT_WORK
    GIFWorkEnd(pGIF);
#endif
    return ret;
} /* GIF_playFrame() */

int GIF_getCanvasWidth(GIFIMAGE *pGIF)
{
    return pGIF->iCanvasWidth;
} /* GIF_getCanvasWidth() */

int GIF_getCanvasHeight(GIFIMAGE *pGIF)
{
    return pGIF->iCanvasHeight;
} /* GIF_getCanvasHeight() */

int GIF_getLoopCount(GIFIMAGE *pGIF)
{
    return pGIF->iRepeatCount;
} /* GIF_getLoopCount() */

int GIF_getComment(GIFIMAGE *pGIF, char *pDest)
{
    return GIFGetCommentImpl(pGIF, pDest);
} /* GIF_getComment() */

int GIF_getLastError(GIFIMAGE *pGIF)
{
    return pGIF->iError;
} /* GIF_getLastError() */

#endif // !__cplusplus
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
    memmove(pBuf, &pFile->pData[pFile->iPos], iBytesRead);
    pFile->iPos += iBytesRead;
    return iBytesRead;
} /* readMem() */

#ifndef __LINUX__
static int32_t readFLASH(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
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
} /* readFLASH() */
#endif // __LINUX__

static int32_t seekMem(GIFFILE *pFile, int32_t iPosition)
{
    if (iPosition < 0) iPosition = 0;
    else if (iPosition >= pFile->iSize) iPosition = pFile->iSize-1;
    pFile->iPos = iPosition;
    return iPosition;
} /* seekMem() */

#if defined ( __LINUX__ ) || defined( __MCUXPRESSO )
static void closeFile(void *handle)
{
    fclose((FILE *)handle);
} /* closeFile() */

static int32_t seekFile(GIFFILE *pFile, int32_t iPosition)
{
    if (iPosition < 0) iPosition = 0;
    else if (iPosition >= pFile->iSize) iPosition = pFile->iSize-1;
    pFile->iPos = iPosition;
    fseek((FILE *)pFile->fHandle, iPosition, SEEK_SET);
    return iPosition;
} /* seekMem() */

static int32_t readFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    int32_t iBytesRead;

    iBytesRead = iLen;
    if ((pFile->iSize - pFile->iPos) < iLen)
       iBytesRead = pFile->iSize - pFile->iPos;
    if (iBytesRead <= 0)
       return 0;
    iBytesRead = (int)fread(pBuf, 1, iBytesRead, (FILE *)pFile->fHandle);
    pFile->iPos += iBytesRead;
    return iBytesRead;
} /* readFile() */

#endif // __LINUX__
//
// The following functions are written in plain C and have no
// 3rd party dependencies, not even the C runtime library
//
//
// Initialize a GIF file and callback access from a file on SD or memory
// returns 1 for success, 0 for failure
// Fills in the canvas size of the GIFIMAGE structure
//
//
// Checked sequential I/O helpers (handle short partial reads safely)
//
static int GIFReadExact(GIFIMAGE *g, uint8_t *dst, int n)
{
    int got = 0;
    if (!g || !g->pfnRead || n < 0 || g->GIFFile.iPos < 0 ||
        g->GIFFile.iPos > g->GIFFile.iSize || n > (g->GIFFile.iSize - g->GIFFile.iPos)) {
        if (g) g->iError = GIF_EARLY_EOF;
        return 0;
    }
    while (got < n) {
        int32_t r = (*g->pfnRead)(&g->GIFFile, dst + got, n - got);
        if (r <= 0 || r > (n - got)) {
            g->iError = GIF_EARLY_EOF;
            return 0;
        }
        got += r;
    }
    return 1;
} /* GIFReadExact() */

static int GIFSkipExact(GIFIMAGE *g, int n)
{
    int32_t pos;
    if (!g || !g->pfnSeek || n < 0 || g->GIFFile.iPos < 0 ||
        g->GIFFile.iPos > g->GIFFile.iSize || n > (g->GIFFile.iSize - g->GIFFile.iPos)) {
        if (g) g->iError = GIF_EARLY_EOF;
        return 0;
    }
    if (n == 0) return 1;
    pos = g->GIFFile.iPos + n;
    if ((*g->pfnSeek)(&g->GIFFile, pos) != pos) {
        g->iError = GIF_EARLY_EOF;
        return 0;
    }
    return 1;
} /* GIFSkipExact() */

static int GIFGetCommentImpl(GIFIMAGE *pGIF, char *pDest)
{
    int32_t iOldPos;
    int len;
    if (!pGIF || !pDest) return 0;
    len = (int)pGIF->sCommentLen;
    if (len <= 0 || pGIF->iCommentPos <= 0 || len > 255) {
        pDest[0] = 0;
        return 0;
    }
    iOldPos = pGIF->GIFFile.iPos;
    if (!pGIF->pfnSeek || (*pGIF->pfnSeek)(&pGIF->GIFFile, pGIF->iCommentPos) != pGIF->iCommentPos ||
        !GIFReadExact(pGIF, (uint8_t *)pDest, len)) {
        if (pGIF->pfnSeek) (*pGIF->pfnSeek)(&pGIF->GIFFile, iOldPos);
        pDest[0] = 0;
        return 0;
    }
    (*pGIF->pfnSeek)(&pGIF->GIFFile, iOldPos);
    pDest[len] = 0;
    return len;
} /* GIFGetCommentImpl() */

static int GIFReadPalette(GIFIMAGE *g, uint16_t *pal, int colors)
{
    int i, done, maxBatch;
#if GIF_TRANSIENT_WORK
    uint8_t stackBuf[48]; // 16-color stack batch keeps open()/GIFInit() at 0 B heap
    uint8_t *buf;
#else
    uint8_t *buf = g->ucFileBuf;
#endif
    if (!pal || colors <= 0 || colors > MAX_COLORS) {
        g->iError = GIF_DECODE_ERROR;
        return 0;
    }
#if GIF_RGB565_ONLY
    if (g->ucPaletteType != GIF_PALETTE_RGB565_LE && g->ucPaletteType != GIF_PALETTE_RGB565_BE) {
        g->iError = GIF_UNSUPPORTED_FEATURE;
        return 0;
    }
#endif
#if GIF_TRANSIENT_WORK
    maxBatch = g->ucFileBuf ? (FILE_BUF_SIZE / 3) : 16;
    buf = g->ucFileBuf ? g->ucFileBuf : stackBuf;
#else
    maxBatch = FILE_BUF_SIZE / 3;
#endif
    if (maxBatch <= 0) {
        g->iError = GIF_DECODE_ERROR;
        return 0;
    }
    done = 0;
    while (done < colors) {
        int batch = colors - done;
        if (batch > maxBatch) batch = maxBatch;
        if (!GIFReadExact(g, buf, batch * 3)) return 0;
#if GIF_RGB565_ONLY
        for (i = 0; i < batch; i++) {
            uint8_t *rgb = buf + (i * 3);
            uint16_t color = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
            pal[done + i] = (g->ucPaletteType == GIF_PALETTE_RGB565_BE) ? __builtin_bswap16(color) : color;
        }
#else
        if (g->ucPaletteType == GIF_PALETTE_RGB888 || g->ucPaletteType == GIF_PALETTE_RGB8888) {
            memcpy(((uint8_t *)pal) + (done * 3), buf, (size_t)batch * 3);
        } else {
            for (i = 0; i < batch; i++) {
                uint8_t *rgb = buf + (i * 3);
                if (g->ucPaletteType == GIF_PALETTE_RGB565_LE || g->ucPaletteType == GIF_PALETTE_RGB565_BE) {
                    uint16_t color = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
                    pal[done + i] = (g->ucPaletteType == GIF_PALETTE_RGB565_BE) ? __builtin_bswap16(color) : color;
                } else if (g->ucPaletteType == GIF_PALETTE_1BPP || g->ucPaletteType == GIF_PALETTE_1BPP_OLED) {
                    ((uint8_t *)pal)[done + i] = (uint8_t)((rgb[0] + (2 * rgb[1]) + rgb[2]) >= 512);
                }
            }
        }
#endif
        done += batch;
    }
    return 1;
} /* GIFReadPalette() */

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
    if (pGIF->iCanvasWidth > MAX_WIDTH || pGIF->iCanvasHeight > GIF_MAX_HEIGHT) { // too big or corrupt
        pGIF->iError = GIF_TOO_WIDE;
        return 0;
    }
    return 1;
} /* GIFInit() */

//
// Parse the GIF header, gather the size and palette info
// Sequential, checked reads: no parsing beyond the bytes actually received.
// Returns 1 for success, 0 for failure
//
static int GIFParseInfo(GIFIMAGE *pPage, int bInfoOnly)
{
    uint8_t b[13];
    pPage->iError = GIF_SUCCESS;
    pPage->bUseLocalPalette = 0;
    pPage->bEndOfFrame = 0;
    pPage->ucGIFBits = 0;
    pPage->iFrameDelay = 0;

    if (pPage->GIFFile.iPos == 0) {
        if (!GIFReadExact(pPage, b, 13)) return 0;
        if (memcmp(b, "GIF87a", 6) != 0 && memcmp(b, "GIF89a", 6) != 0) {
            pPage->iError = GIF_BAD_FILE;
            return 0;
        }
        pPage->iCanvasWidth = pPage->iWidth = GIF_INTELSHORT(&b[6]);
        pPage->iCanvasHeight = pPage->iHeight = GIF_INTELSHORT(&b[8]);
        if (pPage->iCanvasWidth == 0 || pPage->iCanvasHeight == 0) goto bad;
        if (pPage->iCanvasWidth > MAX_WIDTH || pPage->iCanvasHeight > GIF_MAX_HEIGHT) {
            pPage->iError = GIF_TOO_WIDE;
            return 0;
        }
        pPage->iBpp = ((b[10] & 0x70) >> 4) + 1;
        pPage->iGlobalPalSize = 0;
        pPage->ucBackground = b[11];
        pPage->iRepeatCount = -1;
        pPage->iCommentPos = 0;
        pPage->sCommentLen = 0;
        if (b[10] & 0x80) {
            pPage->iGlobalPalSize = 2 << (b[10] & 7);
            if (!GIFReadPalette(pPage, pPage->pPalette, pPage->iGlobalPalSize)) return 0;
        }
    }

    for (;;) {
        uint8_t label;
        if (!GIFReadExact(pPage, b, 1)) return 0;
        if (b[0] == 0x3b) {
            pPage->iError = GIF_EMPTY_FRAME;
            return 1;
        }
        if (b[0] == 0x2c) break;
        if (b[0] != 0x21 || !GIFReadExact(pPage, &label, 1)) goto bad;
        if (label == 0xf9) {
            if (!GIFReadExact(pPage, b, 6)) return 0;
            if (b[0] != 4 || b[5] != 0) goto bad;
            pPage->ucGIFBits = b[1];
            pPage->iFrameDelay = (uint16_t)(GIF_INTELSHORT(&b[2]) * 10);
            if (pPage->iFrameDelay <= 1) pPage->iFrameDelay = 100;
            if (pPage->ucGIFBits & 1) pPage->ucTransparent = b[4];
        } else {
            int netscape = 0;
            int block = 0;
            if (label != 0xff && label != 0xfe && label != 0x01) goto bad;
            for (;;) {
                int n;
                if (!GIFReadExact(pPage, b, 1)) return 0;
                n = b[0];
                if (n == 0) break;
                if (label == 0xff && block == 0 && n == 11) {
                    if (!GIFReadExact(pPage, b, n)) return 0;
                    netscape = (memcmp(b, "NETSCAPE2.0", 11) == 0);
                } else if (netscape && block == 1 && n == 3) {
                    if (!GIFReadExact(pPage, b, n)) return 0;
                    if (b[0] == 1) pPage->iRepeatCount = (int16_t)GIF_INTELSHORT(&b[1]);
                } else {
                    if (label == 0xfe && pPage->iCommentPos == 0) {
                        pPage->iCommentPos = pPage->GIFFile.iPos;
                        pPage->sCommentLen = (short)n;
                    }
                    if (!GIFSkipExact(pPage, n)) return 0;
                }
                block++;
            }
        }
    }

    if (!GIFReadExact(pPage, b, 9)) return 0;
    pPage->iX = GIF_INTELSHORT(&b[0]);
    pPage->iY = GIF_INTELSHORT(&b[2]);
    pPage->iWidth = GIF_INTELSHORT(&b[4]);
    pPage->iHeight = GIF_INTELSHORT(&b[6]);
    pPage->ucMap = b[8];
    if (pPage->iWidth == 0 || pPage->iHeight == 0 || pPage->iWidth > MAX_WIDTH ||
        ((int32_t)pPage->iX + (int32_t)pPage->iWidth) > (int32_t)pPage->iCanvasWidth ||
        ((int32_t)pPage->iY + (int32_t)pPage->iHeight) > (int32_t)pPage->iCanvasHeight) {
        goto bad;
    }
    if (pPage->ucMap & 0x80) {
        pPage->iLocalPalSize = 2 << (pPage->ucMap & 7);
        if (bInfoOnly) {
            if (!GIFSkipExact(pPage, pPage->iLocalPalSize * 3)) return 0;
        } else {
#if GIF_TRANSIENT_WORK
            if (!pPage->pLocalPalette) {
                pPage->iError = GIF_ERROR_MEMORY;
                return 0;
            }
#endif
            if (!GIFReadPalette(pPage, pPage->pLocalPalette, pPage->iLocalPalSize)) return 0;
        }
        pPage->bUseLocalPalette = 1;
    } else if (pPage->iGlobalPalSize == 0) {
        goto bad;
    }
    if (!GIFReadExact(pPage, b, 1)) return 0;
    if (b[0] < 2 || b[0] > 8) goto bad;
    pPage->ucCodeStart = b[0];
    pPage->iBpp = cGIFBits[b[0]];
    pPage->iLZWSize = 0;
    pPage->iLZWOff = 0;
    return 1;
bad:
    pPage->iError = GIF_DECODE_ERROR;
    return 0;
} /* GIFParseInfo() */
//
// Gather info about an animated GIF file (sequential bounds-checked scan)
//
int GIF_getInfo(GIFIMAGE *pPage, GIFINFO *pInfo)
{
    int iNumFrames = 0;
    int iDelay, iMaxDelay = 0, iMinDelay = 10000, iTotalDelay = 0, hasDelay = 0;
    int32_t iOldPos;
    uint8_t b[13];

    if (!pPage || !pInfo || !pPage->pfnRead || !pPage->pfnSeek) return 0;
    memset(pInfo, 0, sizeof(GIFINFO));
    if (pPage->GIFFile.iSize < 14) return 0;

    iOldPos = pPage->GIFFile.iPos;
    if ((*pPage->pfnSeek)(&pPage->GIFFile, 0) != 0) return 0;
    if (!GIFReadExact(pPage, b, 13)) goto done;
    if (memcmp(b, "GIF87a", 6) != 0 && memcmp(b, "GIF89a", 6) != 0) goto done;
    if (b[10] & 0x80) {
        if (!GIFSkipExact(pPage, 3 * (2 << (b[10] & 7)))) goto done;
    }

    for (;;) {
        uint8_t t;
        if (!GIFReadExact(pPage, &t, 1)) break;
        if (t == 0x3b) break;
        if (t == 0x21) {
            uint8_t label;
            if (!GIFReadExact(pPage, &label, 1)) break;
            if (label == 0xf9) {
                if (!GIFReadExact(pPage, b, 6) || b[0] != 4 || b[5] != 0) break;
                iDelay = (int)GIF_INTELSHORT(&b[2]);
                if (iDelay < 2) iDelay = 2;
                iDelay *= 10;
                iTotalDelay += iDelay;
                if (iDelay > iMaxDelay) iMaxDelay = iDelay;
                if (iDelay < iMinDelay) iMinDelay = iDelay;
                hasDelay = 1;
            } else if (label == 0xff || label == 0xfe || label == 0x01) {
                uint8_t n;
                for (;;) {
                    if (!GIFReadExact(pPage, &n, 1)) goto done;
                    if (n == 0) break;
                    if (!GIFSkipExact(pPage, n)) goto done;
                }
            } else {
                break;
            }
        } else if (t == 0x2c) {
            uint8_t d[9], mcs, n;
            if (!GIFReadExact(pPage, d, 9)) break;
            if (d[8] & 0x80) {
                if (!GIFSkipExact(pPage, 3 * (2 << (d[8] & 7)))) break;
            }
            if (!GIFReadExact(pPage, &mcs, 1) || mcs < 2 || mcs > 8) break;
            for (;;) {
                if (!GIFReadExact(pPage, &n, 1)) goto done;
                if (n == 0) break;
                if (!GIFSkipExact(pPage, n)) goto done;
            }
            iNumFrames++;
        } else {
            break;
        }
    }

done:
    (*pPage->pfnSeek)(&pPage->GIFFile, iOldPos);
    pInfo->iFrameCount = iNumFrames;
    pInfo->iMaxDelay = iMaxDelay;
    pInfo->iMinDelay = hasDelay ? iMinDelay : 0;
    pInfo->iDuration = iTotalDelay;
    return (iNumFrames > 0) ? 1 : 0;
} /* GIF_getInfo() */

//
// Unpack more chunk data for decoding
// returns 1 on success (even if bEndOfFrame was reached cleanly)
// returns 0 on I/O error or premature EOF
//
static int GIFGetMoreData(GIFIMAGE *pPage)
{
    int iDelta = pPage->iLZWSize - pPage->iLZWOff;
#if !GIF_DISABLE_TURBO
    int iLZWBufSize = (pPage->pTurboBuffer) ? LZW_BUF_SIZE_TURBO : LZW_BUF_SIZE;
    uint8_t *pLZW = (pPage->pTurboBuffer) ? pPage->ucTurboLZW : pPage->ucLZW;
#else
    int iLZWBufSize = LZW_BUF_SIZE;
    uint8_t *pLZW = pPage->ucLZW;
#endif

    if (iDelta < 0 || iDelta > iLZWBufSize) {
        pPage->iError = GIF_DECODE_ERROR;
        return 0;
    }
    if (pPage->bEndOfFrame) {
        return 1;
    }
    if (iDelta > 0 && pPage->iLZWOff > 0) {
        memmove(pLZW, pLZW + pPage->iLZWOff, (size_t)iDelta);
    }
    pPage->iLZWOff = 0;
    pPage->iLZWSize = iDelta;

    while (!pPage->bEndOfFrame && pPage->iLZWSize <= (iLZWBufSize - MAX_CHUNK_SIZE)) {
        uint8_t n;
        if (!GIFReadExact(pPage, &n, 1)) return 0;
        if (n == 0) {
            pPage->bEndOfFrame = 1;
            break;
        }
        if (!GIFReadExact(pPage, &pLZW[pPage->iLZWSize], n)) return 0;
        pPage->iLZWSize += n;
    }
    return 1;
} /* GIFGetMoreData() */
static inline uint32_t GIFReadU32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static inline void GIFWriteU32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}
static inline uint16_t GIFReadU16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static inline void GIFWriteU16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, sizeof(v));
}

//
// Draw and convert pixels when the user wants fully rendered output
//
static void DrawCooked(GIFIMAGE *pPage, GIFDRAW *pDraw, void *pDest)
{
    uint8_t c, *s, *d8, *pEnd;
    uint8_t *pActivePalette;
    
    pActivePalette = (pPage->bUseLocalPalette) ? (uint8_t *)pPage->pLocalPalette : (uint8_t *)pPage->pPalette;
    // d8 points to the line in the full sized canvas where the new opaque pixels will be merged
    d8 = &pPage->pFrameBuffer[pDraw->iX + (pDraw->iY + pDraw->y) * pPage->iCanvasWidth];
    s = pDraw->pPixels; // s points to the newly decoded pixels of this line of the current frame
    pEnd = s + pDraw->iWidth; // faster way to loop over the source pixels - eliminates a counter variable
    
#if !GIF_RGB565_ONLY
    if (pPage->ucPaletteType == GIF_PALETTE_1BPP || pPage->ucPaletteType == GIF_PALETTE_1BPP_OLED) { // 1-bit mono
        uint8_t *d = NULL;
        uint8_t *pPal = pActivePalette;
        uint8_t uc, ucMask;
        int iPitch = 0;
         if (pPage->ucPaletteType == GIF_PALETTE_1BPP) { // horizontal pixels
             d = pPage->pFrameBuffer;
             iPitch = (pPage->iCanvasWidth+7)/8;
             d += (pPage->iCanvasWidth * pPage->iCanvasHeight);
             d += pDraw->iX/8; // starting column
             d += (pDraw->iY + pDraw->y) * iPitch;
             // Apply the new pixels to the main image and generate 1-bpp output
             if (pDraw->ucHasTransparency) { // if transparency used
                 uint8_t ucTransparent = pDraw->ucTransparent;
                 if (pDraw->ucDisposalMethod == 2) {
                     // Make an attempt to do disposal method 2 (restore to background color)
                     // even though we can't touch pixels outside of the current frame size.
                     // (the previous frame may be larger or in a different position)
                     uint8_t u8BG = pPal[pDraw->ucBackground];
                     if (u8BG == 1) u8BG = 0xff; // set all bits to use mask
                     uc = *d; ucMask = (0x80 >> (pDraw->iX & 7));;
                     while (s < pEnd) {
                         c = *s++;
                         if (c != ucTransparent) {
                             if (pPal[c])
                                 uc |= ucMask;
                             else
                                 uc &= ~ucMask;
                             *d8++ = c;
                         } else {
                             uc |= (u8BG & ucMask); // transparent pixel is restored to background color
                             *d8++ = pDraw->ucBackground;
                         }
                         ucMask >>= 1;
                         if (ucMask == 0) { // write the completed byte
                             *d++ = uc;
                             uc = *d;
                             ucMask = 0x80;
                         }
                     }
                     *d = uc; // write last partial byte
                 } else { // no disposal, just write non-transparent pixels
                     uc = *d; ucMask = (0x80 >> (pDraw->iX & 7));
                     while (s < pEnd) {
                         c = *s++;
                         if (c != ucTransparent) {
                             if (pPal[c])
                                 uc |= ucMask;
                             else
                                 uc &= ~ucMask;
                             *d8 = c;
                         }
                         d8++;
                         ucMask >>= 1;
                         if (ucMask == 0) {
                             *d++ = uc;
                             uc = *d;
                             ucMask = 0x80;
                         }
                     }
                     *d = uc;
                 }
             } else { // convert everything as opaque
                 uc = *d; ucMask = (0x80 >> (pDraw->iX & 7)); // left pixel is MSB
                 while (s < pEnd) {
                     c = *d8++ = *s++; // just write the new opaque pixels over the old
                     if (pPal[c]) // if non-zero, set white pixel
                        uc |= ucMask;
                     else
                        uc &= ~ucMask;
                     ucMask >>= 1;
                     if (ucMask == 0) { // time to write the current byte
                        *d++ = uc;
                        uc = *d;
                        ucMask = 0x80;
                     }
                 }
                 *d = uc;
             }
         } else { // vertical pixels
             d = pPage->pFrameBuffer;
             d += (pPage->iCanvasWidth * pPage->iCanvasHeight);
             d += pDraw->iX; // starting column
             d += ((pDraw->iY + pDraw->y)>>3) * pPage->iCanvasWidth;
             ucMask = 1 << ((pDraw->iY + pDraw->y) & 7);
             // Apply the new pixels to the main image and generate 1-bpp output
             if (pDraw->ucHasTransparency) { // if transparency used
                 uint8_t ucTransparent = pDraw->ucTransparent;
                 if (pDraw->ucDisposalMethod == 2) {
                     // Make an attempt to do disposal method 2 (restore to background color)
                     // even though we can't touch pixels outside of the current frame size.
                     // (the previous frame may be larger or in a different position)
                     uint8_t u8BG = pPal[pDraw->ucBackground];
                     u8BG *= ucMask; // set the right bit
                     while (s < pEnd) {
                         c = *s++;
                         uc = *d & ~ucMask; // clear old pixel
                         if (c != ucTransparent) {
                             uc |= (pPal[c] * ucMask);
                             *d8++ = c;
                         } else {
                             uc |= u8BG; // transparent pixel is restored to background color
                             *d8++ = pDraw->ucBackground;
                         }
                         *d++ = uc; // write back the updated pixel
                     }
                 } else { // no disposal, just write non-transparent pixels
                     while (s < pEnd) {
                         c = *s++;
                         uc = *d & ~ucMask;
                         if (c != ucTransparent) {
                             *d = uc | (pPal[c] * ucMask);
                             *d8 = c;
                         }
                         d++;
                         d8++;
                     }
                 }
             } else { // convert everything as opaque
                 while (s < pEnd) {
                     c = *d8++ = *s++; // just write the new opaque pixels over the old
                     uc = *d & ~ucMask;
                     *d++ = uc | (pPal[c] * ucMask);
                 }
             }
         }
    } else
#endif
    if (pPage->ucPaletteType == GIF_PALETTE_RGB565_LE || pPage->ucPaletteType == GIF_PALETTE_RGB565_BE) {
        uint8_t *d = (uint8_t *)pDest;
        uint16_t *pPal = (uint16_t *)pActivePalette;
        if (pDraw->ucHasTransparency) {
            uint8_t ucTransparent = pDraw->ucTransparent;
            if (pDraw->ucDisposalMethod == 2) {
                uint16_t u16BG = pPal[pDraw->ucBackground];
                while (s < pEnd) {
                    c = *s++;
                    if (c != ucTransparent) {
                        GIFWriteU16(d, pPal[c]);
                        *d8++ = c;
                    } else {
                        GIFWriteU16(d, u16BG);
                        *d8++ = pDraw->ucBackground;
                    }
                    d += 2;
                }
            } else {
                while (s < pEnd) {
                    c = *s++;
                    if (c != ucTransparent) {
                        GIFWriteU16(d, pPal[c]);
                        *d8 = c;
                    }
                    d += 2;
                    d8++;
                }
            }
        } else {
            while (s < pEnd) {
                c = *d8++ = *s++;
                GIFWriteU16(d, pPal[c]);
                d += 2;
            }
        }
    }
#if !GIF_RGB565_ONLY
    else { // 24bpp or 32bpp
        uint8_t pixel, *d, *pPal;
        int x;
        d = (uint8_t *)pDest;
        pPal = pActivePalette;
        if (pDraw->ucHasTransparency) {
            uint8_t ucTransparent = pDraw->ucTransparent;
            if (pDraw->ucDisposalMethod == 2) {
                // Make an attempt to do disposal method 2 (restore to background color)
                // even though we can't touch pixels outside of the current frame size.
                // (the previous frame may be larger or in a different position)
                uint8_t * bg = &pPal[pDraw->ucBackground * 3];
                if (pDraw->ucPaletteType == GIF_PALETTE_RGB888) {
                while (s < pEnd) {
                    pixel = *s++;
                    if (pixel != ucTransparent) {
                        *d8++ = pixel;
                        d[0] = pPal[(pixel * 3) + 0];
                        d[1] = pPal[(pixel * 3) + 1];
                        d[2] = pPal[(pixel * 3) + 2];
                        d += 3;
                    } else {
                        *d8++ = pDraw->ucBackground;
                        d[0] = bg[0];
                        d[1] = bg[1];
                        d[2] = bg[2];
                        d += 3;
                    }
                } // while
                } else { // must be RGB8888
                while (s < pEnd) {
                    pixel = *s++;
                    if (pixel != ucTransparent) {
                        *d8++ = pixel;
                        d[0] = pPal[(pixel * 3) + 0];
                        d[1] = pPal[(pixel * 3) + 1];
                        d[2] = pPal[(pixel * 3) + 2];
                        d[3] = 0xff;
                        d += 4;
                    } else {
                        *d8++ = pDraw->ucBackground;
                        d[0] = bg[0];
                        d[1] = bg[1];
                        d[2] = bg[2];
                        d[3] = 0xff;
                        d += 4;
                    }
                } // while
                }
            } else { // no disposal, just write non-transparent pixels
                if (pPage->ucPaletteType == GIF_PALETTE_RGB888) {
                    for (x=0; x<pPage->iWidth; x++) {
                        pixel = *s++;
                        if (pixel != ucTransparent) {
                            *d8 = pixel;
                            d[0] = pPal[(pixel * 3) + 0]; // convert to RGB888 pixels
                            d[1] = pPal[(pixel * 3) + 1];
                            d[2] = pPal[(pixel * 3) + 2];
                        }
                        d8++;
                        d += 3;
                    }
                } else { // must be RGBA32
                    for (x=0; x<pPage->iWidth; x++) {
                        pixel = *s++;
                        if (pixel != ucTransparent) {
                            *d8 = pixel;
                            d[0] = pPal[(pixel * 3) + 0]; // convert to RGB8888 pixels
                            d[1] = pPal[(pixel * 3) + 1];
                            d[2] = pPal[(pixel * 3) + 2];
                            d[3] = 0xff;
                        }
                        d8++;
                        d += 4;
                    }
                }
            }
        } else { // no transparency
            if (pPage->ucPaletteType == GIF_PALETTE_RGB888) {
                for (x=0; x<pPage->iWidth; x++) {
                    pixel = *d8++ = *s++;
                    *d++ = pPal[(pixel * 3) + 0]; // convert to RGB888 pixels
                    *d++ = pPal[(pixel * 3) + 1];
                    *d++ = pPal[(pixel * 3) + 2];
                }
            } else { // must be RGBA32
                for (x=0; x<pPage->iWidth; x++) {
                    pixel = *d8++ = *s++;
                    *d++ = pPal[(pixel * 3) + 0]; // convert to RGB8888 pixels
                    *d++ = pPal[(pixel * 3) + 1];
                    *d++ = pPal[(pixel * 3) + 2];
                    *d++ = 0xff;
                }
            }
        } // opaque
    }
#endif
} /* DrawCooked() */
#if (defined (ARDUINO_ESP32S31_DEV) || defined (ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32P4_DEV)) && !defined(NO_SIMD)
#ifdef __cplusplus
extern "C" {
#endif // __cplusplus
void s3_merge_transparent(uint8_t *pSrc, uint8_t *pDst, uint8_t *ucTrans, int iLen, uint32_t *pPalette, uint16_t *pRGB565);
#ifdef __cplusplus
}
#endif // __cplusplus
#endif // ESP32S3/P4/S31 SIMD
//
// Merge transparent pixels of a new line of image into the existing framebuffer
//
// This function is SIMD-optimized on CPUs which support SIMD instructions
// SIMD instructions are well suited to speed up this operation since they
// include compare instructions which allow merging of transparent pixels without
// the use of branching.
//
void GIF_mergeTransparent(uint8_t *pSrc, uint8_t *pDst, uint8_t ucTrans, int iLen)
{
#if defined (HAS_NEON) && !defined(NO_SIMD)
    uint8x16_t u8x16_src, u8x16_dst, u8x16_mask;
    const uint8x16_t u8x16_trans = vdupq_n_u8(ucTrans);
    while (iLen >= 16) {
        u8x16_src = vld1q_u8(pSrc);
        u8x16_dst = vld1q_u8(pDst);
        pSrc += 16;
        u8x16_mask = vceqq_u8(u8x16_src, u8x16_trans);
        u8x16_mask = vmvnq_u8(u8x16_mask); // there is no compare-not-equal
        u8x16_dst = vandq_u8(u8x16_dst, u8x16_mask); // preserve original pixels
        u8x16_src = vbicq_u8(u8x16_src, u8x16_mask); // preserve opaque src pixels
        u8x16_dst = vorrq_u8(u8x16_src, u8x16_dst); // combine everything
        vst1q_u8(pDst, u8x16_dst); // store final pixels
        pDst += 16;
        iLen -= 16;
    }
    // Fall through to the generic C code for the 'tail' bytes
#endif // Arm NEON

// Generic C version
    for (int i=0; i<iLen; i++) {
        uint8_t c = *pSrc++;
        if (c != ucTrans) {
            *pDst = c;
        }
        pDst++;
    } // for i
} /* GIF_mergeTransparent() */
//
// Prepare final RGB565 pixels from a newly decoded line and 8-bit framebuffer
//
void GIF_cookPixels(uint8_t *pSrc, uint8_t *pDst, int iTrans, int iLen, uint32_t *pPalette, uint16_t *pRGB565)
{
#if (defined (ARDUINO_ESP32S31_DEV) || defined (ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32P4_DEV)) && !defined(NO_SIMD)
    uint8_t ucTrans, *pTrans;
    if (iTrans == -1) {
        pTrans = NULL;
    } else {
        ucTrans = (uint8_t)iTrans;
        pTrans = &ucTrans;
    }
    s3_merge_transparent(pSrc, pDst, pTrans, iLen, pPalette, pRGB565);
    return;
#endif // ESP32S3/P4

// Generic C version
uint16_t *pPal = (uint16_t *)pPalette; // assume 16-bit palette for non-SIMD
    if (iTrans == -1) { // no transparent color
        for (int i=0; i<iLen; i++) {
            uint8_t c = *pSrc++;
            *pDst++ = c;
            *pRGB565++ = pPal[c];
        }
    } else {
        uint8_t ucTrans = (uint8_t)iTrans;
        for (int i=0; i<iLen; i++) {
            uint8_t c = *pSrc++;
            if (c != ucTrans) {
                *pDst++ = c;
            } else {
                c = *pDst++;
            }
            *pRGB565++ = pPal[c];
        } // for i
    } // transparent color
} /* GIF_cookPixels() */

//
// Handle transparent pixels and disposal method
// Used only when a frame buffer is allocated
//
static void DrawNewPixels(GIFIMAGE *pPage, GIFDRAW *pDraw)
{
    uint8_t *d, *s;
    int iPitch = pPage->iCanvasWidth;

    s = pDraw->pPixels;
    d = &pPage->pFrameBuffer[pDraw->iX + (pDraw->y + pDraw->iY)  * iPitch]; // dest pointer in our complete canvas buffer
    
    // Apply the new pixels to the main image
    if (pDraw->ucHasTransparency) { // if transparency used
        uint8_t ucTransparent = pDraw->ucTransparent;
        if (pDraw->ucDisposalMethod == 2) {
            memset(d, pDraw->ucBackground, pDraw->iWidth); // start with background color
        }
        GIF_mergeTransparent(s, d, ucTransparent, pDraw->iWidth);
    } else { // disposal method doesn't matter when there aren't any transparent pixels
        memcpy(d, s, pDraw->iWidth); // just overwrite the old pixels
    }
} /* DrawNewPixels() */
//
// Bounds-checked 32-bit bit-accumulator LZW code reader (pure C99 inline fast-path)
//
static inline int GIFReadCode(GIFIMAGE *pImage, uint32_t *pulBits, uint8_t *pucBitCount,
                              int iCodeSize, uint16_t sMask, uint16_t *pCode)
{
    uint32_t bits = *pulBits;
    uint8_t cnt = *pucBitCount;
    if (cnt < iCodeSize) {
#if !GIF_DISABLE_TURBO
        uint8_t *pLZW = (pImage->pTurboBuffer) ? pImage->ucTurboLZW : pImage->ucLZW;
#else
        uint8_t *pLZW = pImage->ucLZW;
#endif
        do {
            if (pImage->iLZWOff >= pImage->iLZWSize) {
                if (pImage->bEndOfFrame || !GIFGetMoreData(pImage) || pImage->iLZWOff >= pImage->iLZWSize) {
                    pImage->iError = GIF_EARLY_EOF;
                    return 0;
                }
            }
            bits |= (uint32_t)pLZW[pImage->iLZWOff++] << cnt;
            cnt = (uint8_t)(cnt + 8);
        } while (cnt < iCodeSize);
    }
    *pCode = (uint16_t)(bits & sMask);
    *pulBits = bits >> iCodeSize;
    *pucBitCount = (uint8_t)(cnt - iCodeSize);
    return 1;
} /* GIFReadCode() */

#if !GIF_DISABLE_TURBO
//
// LZWCopyBytes
//
// Output the bytes for a single code with bounds checks
//
static int LZWCopyBytes(unsigned char *buf, int iOffset, int iUncompressedLen,
                        uint8_t *pSymEntry, uint8_t *pLenEntry)
{
    int iLen;
    uint32_t u32Offset, srcOff;
    int extra;

    iLen = (int)GIFReadU16(pLenEntry);
    u32Offset = GIFReadU32(pSymEntry);
    srcOff = u32Offset & 0x7fffffU;
    extra = (u32Offset & 0x800000U) ? 1 : 0;

    if (iLen <= 0 || (iLen + extra) > (iUncompressedLen - iOffset)) {
        return -1;
    }
    if (srcOff >= (uint32_t)(iUncompressedLen + 256) ||
        (srcOff < (uint32_t)iUncompressedLen && (uint32_t)iLen > ((uint32_t)iUncompressedLen - srcOff))) {
        return -1;
    }
    memcpy(&buf[iOffset], &buf[srcOff], (size_t)iLen);
    if (extra) {
        uint8_t c = (uint8_t)(u32Offset >> 24);
        buf[iOffset + iLen] = c;
        iLen++;
        GIFWriteU32(pSymEntry, (uint32_t)iOffset);
        GIFWriteU16(pLenEntry, (uint16_t)iLen);
    }
    return iLen;
} /* LZWCopyBytes() */

#define GET_CODE_TURBO do { \
    uint16_t _c16 = 0; \
    if (!GIFReadCode(pImage, &ulBits, &ucBitCount, (int)codesize, (uint16_t)sMask, &_c16)) return 1; \
    code = _c16; \
} while (0)

//
// DecodeLZWTurbo
//
static int DecodeLZWTurbo(GIFIMAGE *pImage, int iOptions)
{
    int i;
    int iUncompressedLen;
    uint32_t code, oldcode, codesize, nextcode, nextlim;
    uint32_t cc, eoi;
    uint32_t sMask;
    uint8_t c, *buf, codestart;
    uint32_t ulBits = 0;
    uint8_t ucBitCount = 0;
    int iLen, iColors;
    int iOffset;
    uint8_t *pSymbols;
    uint8_t *pLengths;

    (void)iOptions;
    if (!pImage->pTurboBuffer) {
        pImage->iError = GIF_INVALID_PARAMETER;
        return 1;
    }
    pImage->iYCount = pImage->iHeight;
    pImage->iXCount = pImage->iWidth;
    pImage->iLZWOff = 0;
    pImage->iLZWSize = 0;
    if (!GIFGetMoreData(pImage)) return 1;
    codestart = pImage->ucCodeStart;
    iColors = 1 << codestart;
    cc = (uint32_t)iColors;
    eoi = cc + 1;
    if ((uint64_t)pImage->iWidth * (uint64_t)pImage->iHeight > (uint64_t)(INT32_MAX - 256)) {
        pImage->iError = GIF_DECODE_ERROR;
        return 1;
    }
    iUncompressedLen = (int)(pImage->iWidth * pImage->iHeight);
    buf = (uint8_t *)pImage->pTurboBuffer;
    pSymbols = &buf[iUncompressedLen + 256];
    pLengths = pSymbols + ((1 << MAX_CODE_SIZE) * 4);
    iOffset = 0;

    for (i = 0; i < iColors; i++) {
        GIFWriteU32(pSymbols + (i * 4), (uint32_t)(iUncompressedLen + i));
        GIFWriteU16(pLengths + (i * 2), 1);
        buf[iUncompressedLen + i] = (unsigned char)i;
    }

init_codetable:
    codesize = (uint32_t)codestart + 1;
    nextlim = 1U << codesize;
    sMask = nextlim - 1U;
    nextcode = cc + 2;
    GET_CODE_TURBO;
    while (code == cc) {
        GET_CODE_TURBO;
    }
    if (code == eoi) goto turbo_done;
    if (code >= cc || iOffset >= iUncompressedLen) {
        pImage->iError = GIF_DECODE_ERROR;
        return 1;
    }
    buf[iOffset++] = (unsigned char)code;
    oldcode = code;
    GET_CODE_TURBO;
    while (code != eoi && iOffset < iUncompressedLen) {
        if (code == cc) {
            goto init_codetable;
        }
        if (code > nextcode || code >= (1U << MAX_CODE_SIZE) ||
            (nextcode >= (1U << MAX_CODE_SIZE) && code == nextcode)) {
            pImage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        if (nextcode < nextlim) {
            if (code != nextcode) {
                iLen = LZWCopyBytes(buf, iOffset, iUncompressedLen, pSymbols + (code * 4), pLengths + (code * 2));
                if (iLen < 0) { pImage->iError = GIF_DECODE_ERROR; return 1; }
                GIFWriteU32(pSymbols + (nextcode * 4), (GIFReadU32(pSymbols + (oldcode * 4)) | 0x800000U | ((uint32_t)buf[iOffset] << 24)));
                GIFWriteU16(pLengths + (nextcode * 2), GIFReadU16(pLengths + (oldcode * 2)));
                iOffset += iLen;
            } else {
                iLen = LZWCopyBytes(buf, iOffset, iUncompressedLen, pSymbols + (oldcode * 4), pLengths + (oldcode * 2));
                if (iLen < 0 || iOffset + iLen >= iUncompressedLen) {
                    pImage->iError = GIF_DECODE_ERROR;
                    return 1;
                }
                GIFWriteU16(pLengths + (nextcode * 2), (uint16_t)(iLen + 1));
                GIFWriteU32(pSymbols + (nextcode * 4), (uint32_t)iOffset);
                c = buf[iOffset];
                iOffset += iLen;
                buf[iOffset++] = c;
            }
        } else {
            iLen = LZWCopyBytes(buf, iOffset, iUncompressedLen, pSymbols + (code * 4), pLengths + (code * 2));
            if (iLen < 0) { pImage->iError = GIF_DECODE_ERROR; return 1; }
            iOffset += iLen;
        }
        if (nextcode < (1U << MAX_CODE_SIZE)) nextcode++;
        if (nextcode >= nextlim && codesize < MAX_CODE_SIZE) {
            codesize++;
            nextlim <<= 1;
            sMask = nextlim - 1U;
        }
        oldcode = code;
        GET_CODE_TURBO;
    }

turbo_done:
    if (iOffset < iUncompressedLen) {
        pImage->iError = GIF_DECODE_ERROR;
        return 1;
    }
    while (!pImage->bEndOfFrame) {
        pImage->iLZWOff = pImage->iLZWSize;
        if (!GIFGetMoreData(pImage)) return 1;
    }
    if (pImage->ucDrawType == GIF_DRAW_COOKED && pImage->pFrameBuffer) {
        GIFDRAW gd;
        uint32_t *pPal32 = NULL;
        gd.iX = pImage->iX;
        gd.iY = pImage->iY;
        gd.iWidth = pImage->iWidth;
        gd.iHeight = pImage->iHeight;
        gd.pPalette = (pImage->bUseLocalPalette) ? pImage->pLocalPalette : pImage->pPalette;
        gd.pPalette24 = (uint8_t *)gd.pPalette;
        gd.ucIsGlobalPalette = (pImage->bUseLocalPalette == 1) ? 0 : 1;
        gd.pUser = pImage->pUser;
        gd.ucPaletteType = pImage->ucPaletteType;
#if defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S31_DEV) || defined(ARDUINO_ESP32P4_DEV)
        if (pImage->ucPaletteType == GIF_PALETTE_RGB565_BE || pImage->ucPaletteType == GIF_PALETTE_RGB565_LE) {
            uint16_t *pPal16 = (uint16_t *)gd.pPalette;
            int palCount = pImage->bUseLocalPalette ? pImage->iLocalPalSize : pImage->iGlobalPalSize;
            if (palCount > 256) palCount = 256;
            pPal32 = (uint32_t *)malloc(1024);
            if (pPal32) {
                for (int j = 0; j < palCount; j++) {
                    pPal32[j] = pPal16[j];
                }
            }
        }
#endif
        for (int y = 0; y < pImage->iHeight; y++) {
            gd.y = y;
            gd.pPixels = &buf[(y * pImage->iWidth)];
            if (pImage->ucMap & 0x40) {
                int height = pImage->iHeight - 1;
                if (gd.y > height / 2)
                    gd.y = gd.y * 2 - (height | 1);
                else if (gd.y > height / 4)
                    gd.y = gd.y * 4 - ((height & ~1) | 2);
                else if (gd.y > height / 8)
                    gd.y = gd.y * 8 - ((height & ~3) | 4);
                else
                    gd.y = gd.y * 8;
            }
            pImage->ucDisposalMethod = gd.ucDisposalMethod = (pImage->ucGIFBits & 0x1c) >> 2;
            gd.ucTransparent = pImage->ucTransparent;
            gd.ucHasTransparency = pImage->ucGIFBits & 1;
            gd.ucBackground = pImage->ucBackground;
            gd.iCanvasWidth = pImage->iCanvasWidth;
            if (pImage->pfnDraw) {
                DrawCooked(pImage, &gd, &buf[pImage->iCanvasHeight * pImage->iCanvasWidth]);
                gd.pPixels = &buf[pImage->iCanvasHeight * pImage->iCanvasWidth];
                (*pImage->pfnDraw)(&gd);
            } else if (pImage->pFrameBuffer) {
                uint8_t *d = &pImage->pFrameBuffer[pImage->iCanvasWidth * pImage->iCanvasHeight];
                d += (((gd.y + gd.iY) * pImage->iCanvasWidth) + gd.iX) * 2;
                if (pPal32) {
#if defined (ARDUINO_ESP32S3_DEV) || defined (ARDUINO_ESP32P4_DEV) || defined(ARDUINO_ESP32S31_DEV)
                    uint8_t *d8 = &pImage->pFrameBuffer[gd.iX + (gd.iY + gd.y) * pImage->iCanvasWidth];
                    GIF_cookPixels(gd.pPixels, d8, (pImage->ucGIFBits & 1) ? pImage->ucTransparent : -1, gd.iWidth, pPal32, (uint16_t *)d);
#endif
                } else {
                    DrawCooked(pImage, &gd, d);
                }
            }
        }
        if (pPal32) free(pPal32);
    }
    return GIF_SUCCESS;
} /* DecodeLZWTurbo() */
#endif // !GIF_DISABLE_TURBO

//
// GIFEmitPixels: emit a contiguous run of decoded 8-bit pixel indices to line buffer / display / framebuffer
//
static void GIFEmitPixels(GIFIMAGE *pPage, const uint8_t *s, int iPixCount)
{
    uint8_t *buf = pPage->pLineBufAligned + (pPage->iWidth - pPage->iXCount);
    while (iPixCount > 0 && pPage->iYCount > 0) {
        if (pPage->iXCount > iPixCount) {
            memcpy(buf, s, (size_t)iPixCount);
            pPage->iXCount = (uint16_t)(pPage->iXCount - iPixCount);
            return;
        } else {
            GIFDRAW gd;
            int take = pPage->iXCount;
            memcpy(buf, s, (size_t)take);
            s += take;
            iPixCount -= take;
            pPage->iXCount = pPage->iWidth;

            gd.iX = pPage->iX;
            gd.iY = pPage->iY;
            gd.iWidth = pPage->iWidth;
            gd.iHeight = pPage->iHeight;
            gd.pPixels = pPage->pLineBufAligned;
            gd.pPalette = (pPage->bUseLocalPalette) ? pPage->pLocalPalette : pPage->pPalette;
            gd.pPalette24 = (uint8_t *)gd.pPalette;
            gd.ucIsGlobalPalette = (pPage->bUseLocalPalette == 1) ? 0 : 1;
            gd.y = pPage->iHeight - pPage->iYCount;
            if (pPage->ucMap & 0x40) {
                int height = pPage->iHeight - 1;
                if (gd.y > height / 2)
                    gd.y = gd.y * 2 - (height | 1);
                else if (gd.y > height / 4)
                    gd.y = gd.y * 4 - ((height & ~1) | 2);
                else if (gd.y > height / 8)
                    gd.y = gd.y * 8 - ((height & ~3) | 4);
                else
                    gd.y = gd.y * 8;
            }
            pPage->ucDisposalMethod = gd.ucDisposalMethod = (pPage->ucGIFBits & 0x1c) >> 2;
            gd.ucTransparent = pPage->ucTransparent;
            gd.ucHasTransparency = pPage->ucGIFBits & 1;
            gd.ucBackground = pPage->ucBackground;
            gd.iCanvasWidth = pPage->iCanvasWidth;
            gd.pUser = pPage->pUser;
            gd.ucPaletteType = pPage->ucPaletteType;
            if (pPage->pFrameBuffer) {
                int iPitch = 0, iBpp = 1, iOffset = pPage->iCanvasWidth * pPage->iCanvasHeight;
                if (pPage->ucDrawType == GIF_DRAW_COOKED) {
                    if (!pPage->pfnDraw) {
                        switch (pPage->ucPaletteType) {
                            case GIF_PALETTE_RGB565_BE:
                            case GIF_PALETTE_RGB565_LE:
                                iPitch = (pPage->iCanvasWidth * 2);
                                iBpp = 2;
                                break;
#if !GIF_RGB565_ONLY
                            case GIF_PALETTE_1BPP:
                                iPitch = (pPage->iCanvasWidth + 7) / 8;
                                break;
                            case GIF_PALETTE_RGB888:
                                iPitch = pPage->iCanvasWidth * 3;
                                iBpp = 3;
                                break;
                            case GIF_PALETTE_RGB8888:
                                iPitch = pPage->iCanvasWidth * 4;
                                iBpp = 4;
                                break;
#endif
                        }
                        iOffset += (iBpp * pPage->iX) + ((gd.y + pPage->iY) * iPitch);
                    }
                    DrawCooked(pPage, &gd, &pPage->pFrameBuffer[iOffset]);
                    gd.pPixels = &pPage->pFrameBuffer[iOffset];
                } else {
                    DrawNewPixels(pPage, &gd);
                }
            }
            if (pPage->pfnDraw) {
                (*pPage->pfnDraw)(&gd);
            }
            pPage->iYCount--;
            buf = pPage->pLineBufAligned;
        }
    }
} /* GIFEmitPixels() */

//
// Compact dictionary accessors (GIF_DICT_MODE: 0=standard, 16=16-bit prefix, 12=packed 12-bit prefix)
//
#if GIF_DICT_MODE == 12
static inline uint16_t GIFGetPrefix(const GIFIMAGE *g, unsigned int code)
{
    unsigned int pair = (code >> 1) * 3U;
    if ((code & 1U) == 0U) {
        return (uint16_t)(g->ucGIFPrefix12[pair] | ((uint16_t)(g->ucGIFPrefix12[pair + 1U] & 0x0FU) << 8));
    } else {
        return (uint16_t)((g->ucGIFPrefix12[pair + 1U] >> 4) | ((uint16_t)g->ucGIFPrefix12[pair + 2U] << 4));
    }
}
static inline void GIFSetPrefix(GIFIMAGE *g, unsigned int code, uint16_t val)
{
    unsigned int pair = (code >> 1) * 3U;
    if ((code & 1U) == 0U) {
        g->ucGIFPrefix12[pair] = (uint8_t)(val & 0xFFU);
        g->ucGIFPrefix12[pair + 1U] = (uint8_t)((g->ucGIFPrefix12[pair + 1U] & 0xF0U) | ((val >> 8) & 0x0FU));
    } else {
        g->ucGIFPrefix12[pair + 1U] = (uint8_t)((g->ucGIFPrefix12[pair + 1U] & 0x0FU) | ((val & 0x0FU) << 4));
        g->ucGIFPrefix12[pair + 2U] = (uint8_t)((val >> 4) & 0xFFU);
    }
}
static inline uint8_t GIFGetSuffix(const GIFIMAGE *g, unsigned int code) { return g->ucGIFPixels[code]; }
static inline void GIFSetSuffix(GIFIMAGE *g, unsigned int code, uint8_t val) { g->ucGIFPixels[code] = val; }
#elif GIF_DICT_MODE == 16
static inline uint16_t GIFGetPrefix(const GIFIMAGE *g, unsigned int code) { return g->usGIFTable[code]; }
static inline void GIFSetPrefix(GIFIMAGE *g, unsigned int code, uint16_t val) { g->usGIFTable[code] = val; }
static inline uint8_t GIFGetSuffix(const GIFIMAGE *g, unsigned int code) { return g->ucGIFPixels[code]; }
static inline void GIFSetSuffix(GIFIMAGE *g, unsigned int code, uint8_t val) { g->ucGIFPixels[code] = val; }
#else
static inline uint16_t GIFGetPrefix(const GIFIMAGE *g, unsigned int code) { return g->usGIFTable[code]; }
static inline uint8_t GIFGetSuffix(const GIFIMAGE *g, unsigned int code) { return g->ucGIFPixels[PIXEL_LAST + code]; }
#endif

//
// Chunked emission fallback when an LZW chain exceeds FILE_BUF_SIZE (preserves full 12-bit support)
//
static int GIFEmitChainChunked(GIFIMAGE *pPage, unsigned int walkCode, unsigned int cur,
                               unsigned int cc, unsigned int nextcode, int alreadyPopped,
                               int isKwKwK, uint8_t *pFirstChar)
{
    uint8_t *pEnd = pPage->ucFileBuf;
    int totalWalkLen = alreadyPopped;
    uint8_t firstChar;
    int remFromTail;

    while (cur >= cc) {
        unsigned int link;
        if (cur >= nextcode || cur <= (cc + 1U) || totalWalkLen >= (1 << MAX_CODE_SIZE)) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        totalWalkLen++;
        link = GIFGetPrefix(pPage, cur);
        if (link >= cur) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        cur = link;
    }
    totalWalkLen++;
    firstChar = (uint8_t)cur;
    if (pFirstChar) *pFirstChar = firstChar;

    remFromTail = totalWalkLen;
    while (remFromTail > 0) {
        int take = (remFromTail > FILE_BUF_SIZE) ? FILE_BUF_SIZE : remFromTail;
        int skip = remFromTail - take;
        unsigned int node = walkCode;
        uint8_t *dst = pEnd + take;
        int k;
        for (k = 0; k < skip; k++) {
            node = GIFGetPrefix(pPage, node);
        }
        for (k = 0; k < take; k++) {
            if (node < cc) {
                *(--dst) = (uint8_t)node;
            } else {
                *(--dst) = GIFGetSuffix(pPage, node);
                node = GIFGetPrefix(pPage, node);
            }
        }
        GIFEmitPixels(pPage, pEnd, take);
        remFromTail -= take;
    }
    if (isKwKwK) {
        GIFEmitPixels(pPage, &firstChar, 1);
    }
    return 0;
} /* GIFEmitChainChunked() */

#if GIF_DICT_MODE != 0
//
// GIFExpandAndEmit: compact dictionary expansion without PIXEL_FIRST table.
// Expands code (or oldcode for KwKwK) in one pass; the root byte at *s gives firstChar.
//
static int GIFExpandAndEmit(GIFIMAGE *pPage, unsigned int code, unsigned int oldcode,
                            unsigned int cc, unsigned int nextcode, uint8_t *pFirstChar)
{
    int isKwKwK = (code == nextcode);
    unsigned int walkCode = isKwKwK ? oldcode : code;
    unsigned int cur = walkCode;
    uint8_t *pEnd = pPage->ucFileBuf;
    uint8_t *s = pEnd + FILE_BUF_SIZE - (isKwKwK ? 1 : 0);
    uint8_t firstChar;
    int iPixCount;

    while (cur >= cc) {
        unsigned int link;
        if (cur >= nextcode || cur <= (cc + 1U)) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        if (s == pEnd) {
            return GIFEmitChainChunked(pPage, walkCode, cur, cc, nextcode,
                                       FILE_BUF_SIZE - (isKwKwK ? 1 : 0), isKwKwK, pFirstChar);
        }
        *(--s) = GIFGetSuffix(pPage, cur);
        link = GIFGetPrefix(pPage, cur);
        if (link >= cur) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        cur = link;
    }
    if (s == pEnd) {
        return GIFEmitChainChunked(pPage, walkCode, cur, cc, nextcode,
                                   FILE_BUF_SIZE - (isKwKwK ? 1 : 0), isKwKwK, pFirstChar);
    }
    firstChar = (uint8_t)cur;
    *(--s) = firstChar;
    if (isKwKwK) {
        pEnd[FILE_BUF_SIZE - 1] = firstChar;
    }
    *pFirstChar = firstChar;
    iPixCount = (int)(intptr_t)(pEnd + FILE_BUF_SIZE - s);
    GIFEmitPixels(pPage, s, iPixCount);
    return 0;
} /* GIFExpandAndEmit() */
#else
//
// GIFMakePels: expand one LZW code and emit pixels; returns 0 on success, 1 on error
//
#if GIF_LAZY_RESET
static int GIFMakePels(GIFIMAGE *pPage, unsigned int code, unsigned int cc, unsigned int nextcode)
{
    int iPixCount;
    unsigned int walkCode = code;
    unsigned short *giftabs;
    unsigned char *s, *pEnd, *gifpels;

    pEnd = pPage->ucFileBuf;
    s = pEnd + FILE_BUF_SIZE;
    giftabs = pPage->usGIFTable;
    gifpels = &pPage->ucGIFPixels[PIXEL_LAST];

    while (code >= cc) {
        unsigned int link;
        if (code >= nextcode || code <= (cc + 1U)) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        if (s == pEnd) {
            return GIFEmitChainChunked(pPage, walkCode, code, cc, nextcode, FILE_BUF_SIZE, 0, NULL);
        }
        *(--s) = gifpels[code];
        link = giftabs[code];
        if (link >= code) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        code = link;
    }
    if (s == pEnd) {
        return GIFEmitChainChunked(pPage, walkCode, code, cc, nextcode, FILE_BUF_SIZE, 0, NULL);
    }
    *(--s) = (unsigned char)code;
    iPixCount = (int)(intptr_t)(pEnd + FILE_BUF_SIZE - s);
    GIFEmitPixels(pPage, s, iPixCount);
    return 0;
} /* GIFMakePels() */
#else
static int GIFMakePels(GIFIMAGE *pPage, unsigned int code, unsigned int cc, unsigned int nextcode)
{
    int iPixCount;
    unsigned short *giftabs;
    unsigned char *s, *pEnd, *gifpels;
    (void)cc;
    (void)nextcode;

    pEnd = pPage->ucFileBuf;
    s = pEnd + FILE_BUF_SIZE;
    giftabs = pPage->usGIFTable;
    gifpels = &pPage->ucGIFPixels[PIXEL_LAST];

    while (code < LINK_UNUSED) {
        unsigned int link;
        if (code >= (1U << MAX_CODE_SIZE) || s == pEnd) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        *(--s) = gifpels[code];
        link = giftabs[code];
        if (link < LINK_UNUSED && link >= code) {
            pPage->iError = GIF_DECODE_ERROR;
            return 1;
        }
        code = link;
    }
    if (code != LINK_END) {
        pPage->iError = GIF_DECODE_ERROR;
        return 1;
    }
    iPixCount = (int)(intptr_t)(pEnd + FILE_BUF_SIZE - s);
    GIFEmitPixels(pPage, s, iPixCount);
    return 0;
} /* GIFMakePels() */
#endif
#endif // GIF_DICT_MODE != 0

#define GET_CODE_STATE do { \
    if (!GIFReadCode(pImage, &st->ulBits, &st->ucBitCount, (int)st->codesize, st->sMask, &code)) { \
        st->state = GIF_COOP_ERROR; \
        return GIF_DECODE_ERR; \
    } \
} while (0)

enum {
    GIF_COOP_IDLE = 0,
    GIF_COOP_WAIT_FIRST = 1,
    GIF_COOP_LOOP = 2,
    GIF_COOP_FRAME_DONE = 3,
    GIF_COOP_END = 4,
    GIF_COOP_ERROR = 5
};

typedef struct gif_lzw_state_tag {
    uint32_t ulBits;
    uint16_t oldcode;
    uint16_t nextcode;
    uint16_t nextlim;
    uint16_t sMask;
    uint16_t cc;
    uint16_t eoi;
    uint8_t codesize;
    uint8_t ucBitCount;
    uint8_t state;
    uint8_t firstChar;
} GIFLZWState;

static void GIFLZWInitState(GIFIMAGE *pImage, GIFLZWState *st)
{
    st->cc = (uint16_t)(1U << pImage->ucCodeStart);
    st->eoi = (uint16_t)(st->cc + 1U);
    st->codesize = (uint8_t)(pImage->ucCodeStart + 1U);
    st->nextlim = (uint16_t)(1U << st->codesize);
    st->sMask = (uint16_t)(st->nextlim - 1U);
    st->nextcode = (uint16_t)(st->cc + 2U);
    st->ulBits = 0;
    st->ucBitCount = 0;
    st->oldcode = 0;
    st->firstChar = 0;
    st->state = GIF_COOP_WAIT_FIRST;
#if GIF_DICT_MODE == 0 && !GIF_LAZY_RESET
    memset(&pImage->usGIFTable[st->cc], LINK_UNUSED, GIF_TABLE_BYTES - sizeof(pImage->usGIFTable[0]) * st->cc);
#endif
} /* GIFLZWInitState() */

static int GIFLZWStartFrame(GIFIMAGE *pImage)
{
    int i;
    uint16_t cc;

    if (pImage->ucDrawType == GIF_DRAW_COOKED && pImage->pFrameBuffer == NULL && pImage->pfnDraw == NULL) {
        pImage->iError = GIF_INVALID_PARAMETER;
        return 1;
    }
    if (pImage->ucDrawType == GIF_DRAW_RAW && pImage->pfnDraw == NULL) {
        pImage->iError = GIF_INVALID_PARAMETER;
        return 1;
    }
#if GIF_RGB565_ONLY
    if (pImage->ucPaletteType != GIF_PALETTE_RGB565_LE && pImage->ucPaletteType != GIF_PALETTE_RGB565_BE) {
        pImage->iError = GIF_UNSUPPORTED_FEATURE;
        return 1;
    }
#endif
    if (pImage->ucDrawType == GIF_DRAW_COOKED && pImage->pFrameBuffer && pImage->ucPrevDisp == 2) {
        uint8_t *pActivePalette = (pImage->bUseLocalPalette) ? (uint8_t *)pImage->pLocalPalette : (uint8_t *)pImage->pPalette;
        uint16_t *pPal = (uint16_t *)pActivePalette;
        uint16_t u16BG;
        uint8_t c = pImage->ucBackground;
        for (int y = pImage->iPrevY; y < pImage->iPrevH + pImage->iPrevY; y++) {
            uint8_t *p = &pImage->pFrameBuffer[(y * pImage->iCanvasWidth) + pImage->iPrevX];
            memset(p, c, pImage->iPrevW);
            if (!pImage->pfnDraw && (pImage->ucPaletteType == GIF_PALETTE_RGB565_LE || pImage->ucPaletteType == GIF_PALETTE_RGB565_BE)) {
                uint8_t *d16 = &pImage->pFrameBuffer[(pImage->iCanvasWidth * pImage->iCanvasHeight) + (y * pImage->iCanvasWidth * 2) + (pImage->iPrevX * 2)];
                u16BG = pPal[c];
                for (i = 0; i < pImage->iPrevW; i++) {
                    GIFWriteU16(d16 + (i * 2), u16BG);
                }
            }
        }
    }

    cc = (uint16_t)(1U << pImage->ucCodeStart);
    pImage->iYCount = pImage->iHeight;
    pImage->iXCount = pImage->iWidth;
    pImage->iLZWOff = 0;
    pImage->iLZWSize = 0;
    if (!GIFGetMoreData(pImage)) return 1;

#if GIF_DICT_MODE == 0
    for (i = 0; i < (int)cc; i++) {
        pImage->ucGIFPixels[PIXEL_FIRST + i] = (unsigned char)i;
#if !GIF_LAZY_RESET
        pImage->ucGIFPixels[PIXEL_LAST + i] = (unsigned char)i;
        pImage->usGIFTable[i] = LINK_END;
#endif
    }
#else
    (void)cc;
#endif
    return 0;
} /* GIFLZWStartFrame() */

static int GIFLZWFinishFrame(GIFIMAGE *pImage)
{
    if (pImage->iYCount != 0) {
        pImage->iError = GIF_DECODE_ERROR;
        return 1;
    }
    while (!pImage->bEndOfFrame) {
        pImage->iLZWOff = pImage->iLZWSize;
        if (!GIFGetMoreData(pImage)) return 1;
    }
    if (pImage->ucDisposalMethod == 2) {
        pImage->iPrevW = pImage->iWidth;
        pImage->iPrevH = pImage->iHeight;
        pImage->iPrevX = pImage->iX;
        pImage->iPrevY = pImage->iY;
    }
    pImage->ucPrevDisp = pImage->ucDisposalMethod;
    return 0;
} /* GIFLZWFinishFrame() */

static inline int GIFLZWRun(GIFIMAGE *pImage, GIFLZWState *st, int bBounded,
                            uint32_t u32StartUs, uint32_t u32BudgetUs, int iMaxLines)
{
    uint16_t startYCount = pImage->iYCount;
    int codesRun = 0;
    uint16_t code;

    for (;;) {
        uint16_t prevY = pImage->iYCount;
        if (st->state == GIF_COOP_WAIT_FIRST) {
            GET_CODE_STATE;
            if (code == st->cc) {
#if GIF_ENABLE_COOPERATIVE
                if (bBounded) {
                    codesRun++;
                    if (iMaxLines == 0 && (u32BudgetUs == 0 || ((codesRun & 15) == 0 && (uint32_t)(GIF_MICROS() - u32StartUs) >= u32BudgetUs))) {
                        return GIF_DECODE_MORE;
                    }
                }
#endif
                continue;
            }
            if (code == st->eoi) {
                st->state = GIF_COOP_FRAME_DONE;
                return GIF_DECODE_FRAME_DONE;
            }
            if (code >= st->cc) {
                pImage->iError = GIF_DECODE_ERROR;
                st->state = GIF_COOP_ERROR;
                return GIF_DECODE_ERR;
            }
            st->firstChar = (uint8_t)code;
            st->oldcode = code;
#if GIF_DICT_MODE != 0
            {
                uint8_t firstPix = st->firstChar;
                GIFEmitPixels(pImage, &firstPix, 1);
            }
#else
            if (GIFMakePels(pImage, code, st->cc, st->nextcode) != 0) {
                st->state = GIF_COOP_ERROR;
                return GIF_DECODE_ERR;
            }
#endif
            st->state = GIF_COOP_LOOP;
            if (pImage->iYCount == 0) {
                st->state = GIF_COOP_FRAME_DONE;
                return GIF_DECODE_FRAME_DONE;
            }
        } else {
            if (pImage->iYCount == 0) {
                st->state = GIF_COOP_FRAME_DONE;
                return GIF_DECODE_FRAME_DONE;
            }
            GET_CODE_STATE;
            if (code == st->cc) {
                st->codesize = (uint8_t)(pImage->ucCodeStart + 1U);
                st->nextlim = (uint16_t)(1U << st->codesize);
                st->sMask = (uint16_t)(st->nextlim - 1U);
                st->nextcode = (uint16_t)(st->cc + 2U);
#if GIF_DICT_MODE == 0 && !GIF_LAZY_RESET
                memset(&pImage->usGIFTable[st->cc], LINK_UNUSED, GIF_TABLE_BYTES - sizeof(pImage->usGIFTable[0]) * st->cc);
#endif
                st->state = GIF_COOP_WAIT_FIRST;
#if GIF_ENABLE_COOPERATIVE
                if (bBounded) {
                    codesRun++;
                    if (iMaxLines == 0 && (u32BudgetUs == 0 || ((codesRun & 15) == 0 && (uint32_t)(GIF_MICROS() - u32StartUs) >= u32BudgetUs))) {
                        return GIF_DECODE_MORE;
                    }
                }
#endif
                continue;
            }
            if (code == st->eoi) {
                st->state = GIF_COOP_FRAME_DONE;
                return GIF_DECODE_FRAME_DONE;
            }
            if (code > st->nextcode || code >= (1U << MAX_CODE_SIZE) ||
                (st->nextcode >= (1U << MAX_CODE_SIZE) && code == st->nextcode)) {
                pImage->iError = GIF_DECODE_ERROR;
                st->state = GIF_COOP_ERROR;
                return GIF_DECODE_ERR;
            }
#if GIF_DICT_MODE != 0
            if (GIFExpandAndEmit(pImage, code, st->oldcode, st->cc, st->nextcode, &st->firstChar) != 0) {
                st->state = GIF_COOP_ERROR;
                return GIF_DECODE_ERR;
            }
            if (st->nextcode < st->nextlim) {
                GIFSetPrefix(pImage, st->nextcode, st->oldcode);
                GIFSetSuffix(pImage, st->nextcode, st->firstChar);
            }
#else
            if (st->nextcode < st->nextlim) {
                pImage->usGIFTable[st->nextcode] = st->oldcode;
                pImage->ucGIFPixels[PIXEL_FIRST + st->nextcode] = st->firstChar;
                pImage->ucGIFPixels[PIXEL_LAST + st->nextcode] = st->firstChar = pImage->ucGIFPixels[PIXEL_FIRST + code];
            }
#endif
            if (st->nextcode < (1U << MAX_CODE_SIZE)) st->nextcode++;
            if (st->nextcode >= st->nextlim && st->codesize < MAX_CODE_SIZE) {
                st->codesize++;
                st->nextlim <<= 1;
                st->sMask = (uint16_t)(st->nextlim - 1U);
            }
#if GIF_DICT_MODE == 0
            if (GIFMakePels(pImage, code, st->cc, st->nextcode) != 0) {
                st->state = GIF_COOP_ERROR;
                return GIF_DECODE_ERR;
            }
#endif
            st->oldcode = code;
            if (pImage->iYCount == 0) {
                st->state = GIF_COOP_FRAME_DONE;
                return GIF_DECODE_FRAME_DONE;
            }
        }

#if GIF_ENABLE_COOPERATIVE
        if (bBounded) {
            codesRun++;
            if (iMaxLines > 0) {
                if ((int)(startYCount - pImage->iYCount) >= iMaxLines) {
                    return GIF_DECODE_MORE;
                }
            } else {
                if (u32BudgetUs == 0 ||
                    ((pImage->iYCount != prevY || (codesRun & 15) == 0) && (uint32_t)(GIF_MICROS() - u32StartUs) >= u32BudgetUs)) {
                    return GIF_DECODE_MORE;
                }
            }
        }
#else
        (void)bBounded;
        (void)u32StartUs;
        (void)u32BudgetUs;
        (void)iMaxLines;
        (void)startYCount;
        (void)codesRun;
        (void)prevY;
#endif
    }
} /* GIFLZWRun() */

//
// Decode LZW into an image (synchronous full-frame path)
//
static int DecodeLZW(GIFIMAGE *pImage, int iOptions)
{
    GIFLZWState st;
    int rc;
    (void)iOptions;

    if (GIFLZWStartFrame(pImage) != 0) return 1;
    GIFLZWInitState(pImage, &st);
    rc = GIFLZWRun(pImage, &st, 0, 0, 0, 0);
    if (rc != GIF_DECODE_FRAME_DONE) return 1;
    return GIFLZWFinishFrame(pImage);
} /* DecodeLZW() */

#if GIF_ENABLE_COOPERATIVE
int GIF_beginFrame(GIFIMAGE *pGIF, void *pUser)
{
    GIFLZWState st;
    if (!pGIF || !pGIF->pfnRead || !pGIF->pfnSeek) {
        if (pGIF) {
            pGIF->iError = GIF_INVALID_PARAMETER;
            pGIF->ucCoopState = GIF_COOP_ERROR;
        }
        return 0;
    }
    if (pGIF->GIFFile.iPos >= pGIF->GIFFile.iSize) {
        pGIF->iError = GIF_EMPTY_FRAME;
        pGIF->ucCoopState = GIF_COOP_END;
        return 0;
    }
#if GIF_TRANSIENT_WORK
    if (!GIFWorkBegin(pGIF)) {
        pGIF->ucCoopState = GIF_COOP_ERROR;
        return 0;
    }
#endif
    if (!GIFParseInfo(pGIF, 0)) {
#if GIF_TRANSIENT_WORK
        GIFWorkEnd(pGIF);
#endif
        pGIF->ucCoopState = (pGIF->iError == GIF_EMPTY_FRAME) ? GIF_COOP_END : GIF_COOP_ERROR;
        return 0;
    }
    if (pGIF->iError == GIF_EMPTY_FRAME) {
#if GIF_TRANSIENT_WORK
        GIFWorkEnd(pGIF);
#endif
        pGIF->ucCoopState = GIF_COOP_END;
        return 0;
    }
    pGIF->pUser = pUser;
    if (GIFLZWStartFrame(pGIF) != 0) {
#if GIF_TRANSIENT_WORK
        GIFWorkEnd(pGIF);
#endif
        pGIF->ucCoopState = GIF_COOP_ERROR;
        return 0;
    }
    GIFLZWInitState(pGIF, &st);
    pGIF->ulCoopBits = st.ulBits;
    pGIF->usCoopOldCode = st.oldcode;
    pGIF->usCoopNextCode = st.nextcode;
    pGIF->usCoopNextLim = st.nextlim;
    pGIF->usCoopMask = st.sMask;
    pGIF->usCoopCC = st.cc;
    pGIF->usCoopEOI = st.eoi;
    pGIF->ucCoopCodeSize = st.codesize;
    pGIF->ucCoopBitCount = st.ucBitCount;
    pGIF->ucCoopState = st.state;
    pGIF->ucCoopFirstChar = st.firstChar;
    return 1;
} /* GIF_beginFrame() */

static int GIFCoopAdvance(GIFIMAGE *pGIF, uint32_t u32BudgetUs, int iMaxLines, int *delayMilliseconds)
{
    GIFLZWState st;
    int rc;
    uint32_t startUs;

    if (!pGIF) return GIF_DECODE_ERR;
    if (pGIF->ucCoopState == GIF_COOP_FRAME_DONE) {
        if (delayMilliseconds) *delayMilliseconds = pGIF->iFrameDelay;
        return GIF_DECODE_FRAME_DONE;
    }
    if (pGIF->ucCoopState == GIF_COOP_END) {
        if (delayMilliseconds) *delayMilliseconds = 0;
        return GIF_DECODE_END;
    }
    if (pGIF->ucCoopState != GIF_COOP_WAIT_FIRST && pGIF->ucCoopState != GIF_COOP_LOOP) {
        pGIF->iError = GIF_INVALID_PARAMETER;
        return GIF_DECODE_ERR;
    }
#if GIF_TRANSIENT_WORK
    if (!pGIF->pSmallWork) {
        pGIF->iError = GIF_INVALID_PARAMETER;
        pGIF->ucCoopState = GIF_COOP_ERROR;
        return GIF_DECODE_ERR;
    }
#endif

    st.ulBits = pGIF->ulCoopBits;
    st.oldcode = pGIF->usCoopOldCode;
    st.nextcode = pGIF->usCoopNextCode;
    st.nextlim = pGIF->usCoopNextLim;
    st.sMask = pGIF->usCoopMask;
    st.cc = pGIF->usCoopCC;
    st.eoi = pGIF->usCoopEOI;
    st.codesize = pGIF->ucCoopCodeSize;
    st.ucBitCount = pGIF->ucCoopBitCount;
    st.state = pGIF->ucCoopState;
    st.firstChar = pGIF->ucCoopFirstChar;

    startUs = (iMaxLines == 0 && u32BudgetUs > 0) ? GIF_MICROS() : 0;
    rc = GIFLZWRun(pGIF, &st, 1, startUs, u32BudgetUs, iMaxLines);

    pGIF->ulCoopBits = st.ulBits;
    pGIF->usCoopOldCode = st.oldcode;
    pGIF->usCoopNextCode = st.nextcode;
    pGIF->usCoopNextLim = st.nextlim;
    pGIF->usCoopMask = st.sMask;
    pGIF->ucCoopCodeSize = st.codesize;
    pGIF->ucCoopBitCount = st.ucBitCount;
    pGIF->ucCoopState = st.state;
    pGIF->ucCoopFirstChar = st.firstChar;

    if (rc == GIF_DECODE_MORE) {
        return GIF_DECODE_MORE;
    }
    if (rc == GIF_DECODE_FRAME_DONE) {
        int finErr = GIFLZWFinishFrame(pGIF);
#if GIF_TRANSIENT_WORK
        GIFWorkEnd(pGIF);
#endif
        if (finErr != 0) {
            pGIF->ucCoopState = GIF_COOP_ERROR;
            return GIF_DECODE_ERR;
        }
        pGIF->ucCoopState = GIF_COOP_FRAME_DONE;
        if (delayMilliseconds) *delayMilliseconds = pGIF->iFrameDelay;
        return GIF_DECODE_FRAME_DONE;
    }
#if GIF_TRANSIENT_WORK
    GIFWorkEnd(pGIF);
#endif
    pGIF->ucCoopState = GIF_COOP_ERROR;
    return GIF_DECODE_ERR;
} /* GIFCoopAdvance() */

int GIF_decodeStep(GIFIMAGE *pGIF, uint32_t u32BudgetUs, int *delayMilliseconds)
{
    return GIFCoopAdvance(pGIF, u32BudgetUs, 0, delayMilliseconds);
} /* GIF_decodeStep() */

int GIF_decodeLines(GIFIMAGE *pGIF, int iMaxLines, int *delayMilliseconds)
{
    if (iMaxLines <= 0) iMaxLines = 1;
    return GIFCoopAdvance(pGIF, 0, iMaxLines, delayMilliseconds);
} /* GIF_decodeLines() */

int GIF_frameComplete(const GIFIMAGE *pGIF)
{
    if (!pGIF) return 1;
    return (pGIF->ucCoopState == GIF_COOP_FRAME_DONE || pGIF->ucCoopState == GIF_COOP_END) ? 1 : 0;
} /* GIF_frameComplete() */
#endif // GIF_ENABLE_COOPERATIVE

void GIF_setDrawCallback(GIFIMAGE *pGIF, GIF_DRAW_CALLBACK *pfnDraw)
{
   pGIF->pfnDraw = pfnDraw;
} /* GIF_setDrawCallback() */
//
// Scale 2 scanlines down by 50% with pixel averaging
// writes new values over previous line
// expects RGB565 little endian pixels as input
//
void GIF_scaleHalf(uint16_t *pCurrent, uint16_t *pPrev, int iWidth, int bBigEndian)
{
int x;
uint16_t *d = pPrev;
uint32_t gSum, rbSum, pix0,pix1,pix2,pix3;
const uint32_t RBMask = 0xf81f, GMask = 0x7e0;

   for (x=0; x<iWidth; x+=2)
   {
      pix0 = pCurrent[0]; pix1 = pCurrent[1];
      pix2 = pPrev[0]; pix3 = pPrev[1];
      pCurrent += 2; pPrev += 2;
      gSum = (pix0 & GMask) + (pix1 & GMask) + (pix2 & GMask) + (pix3 & GMask);
      gSum = ((gSum + 0x40) >> 2) & GMask; // for rounding towards 1
      rbSum = (pix0 & RBMask) + (pix1 & RBMask) + (pix2 & RBMask) + (pix3 & RBMask);
      rbSum = ((rbSum + 0x1002) >> 2) & RBMask;
      if (bBigEndian)
         *d++ = __builtin_bswap16((uint16_t)(gSum + rbSum));
      else
         *d++ = (uint16_t)(gSum + rbSum); // store finished pixel
   } // for x
} /* GIF_scaleHalf() */


