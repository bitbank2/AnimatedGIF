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
//   - Hardened sequential parser, bounds-checked I/O/LZW decoding, UB-free unaligned reads
//   - Optional LowMem dictionary/buffer/cooperative profiles (forthcoming commits)
//===========================================================================

#ifndef __ANIMATEDGIF__
#define __ANIMATEDGIF__
#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define memcpy_P memcpy
#define PROGMEM
#endif

// Cortex-M4/M7 allow unaligned access to SRAM
#if defined(HAL_ESP32_HAL_H_) || defined(TEENSYDUINO) || defined(ARM_MATH_CM4) || defined(ARM_MATH_CM7)
#define ALLOWS_UNALIGNED
#endif

//
// GIF Animator
// Written by Larry Bank
// Copyright (c) 2020 BitBank Software, Inc.
// bitbank@pobox.com
// 
// Designed to decode images up to 480x320 on MCUs
// using less than 22K of RAM
// ...and decode any sized image when more RAM is available
//
// ** NEW **
// Turbo mode added Feb 18, 2024. This option decodes images
// up to 30x faster if there is enough RAM (48K + full framebuffer)
//

/* GIF Defines and variables */
#define MAX_CHUNK_SIZE 255
//
// These 2 macros can be changed to limit the amount of RAM
// required by the decoder. For example, decoding 1-bit images to
// a 128x32 display will not need a max code size of 12 nor a palette
// with 256 entries
//
#define TURBO_BUFFER_SIZE 0x6100

// If you intend to decode generic GIFs, you want this value to be 12. If you are using GIFs solely for animations in
// your own project, and you control the GIFs you intend to play, then you can save additional RAM here: 
// the decoder must reserve a minimum of 4 byte * (1<<MAX_CODE_SIZE) for the dictionary, but based on implementation
// actually reserves 5 byte * (1<<MAX_CODE_SIZE). Small or low colour GIFs may inherently not require a large
// dictionary. For larger GIFs, the en(!)coder can "voluntarily" choose not to utilize the entire dictionary. I.e.,
// by preparing (specially encoding) the GIFs, you can save >10kB RAM, but you will not be able to decode arbitrary
// images anymore. One application to craft such GIFs can be found here (use option -d) 
// https://create.stephan-brumme.com/flexigif-lossless-gif-lzw-optimization/
#define MAX_CODE_SIZE 12

#define MAX_COLORS 256
#ifndef MAX_WIDTH
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
#endif // MAX_WIDTH
#ifndef GIF_MAX_HEIGHT
#define GIF_MAX_HEIGHT 32767
#endif

//
// LowMem & Experimental Compile-Time Configuration
//
// GIF_DICT_MODE:
//   0  = Standard upstream tables (uint16_t prefix[4096] + uint8_t pixels[8192] = 16384 B)
//   16 = Compact 16-bit prefix + 8-bit suffix (8192 + 4096 = 12288 B, saves 4096 B)
//   12 = Packed 12-bit prefix + 8-bit suffix (6144 + 4096 = 10240 B, saves 6144 B)
#ifndef GIF_DICT_MODE
#define GIF_DICT_MODE 0
#endif
#if GIF_DICT_MODE != 0 && GIF_DICT_MODE != 16 && GIF_DICT_MODE != 12
#error "GIF_DICT_MODE must be 0, 16, or 12"
#endif

// Enable lazy dictionary reset (1 = skip memset on Clear Code, 0 = legacy memset)
#ifndef GIF_LAZY_RESET
#define GIF_LAZY_RESET 1
#endif
#if GIF_DICT_MODE != 0 && !GIF_LAZY_RESET
#error "Compact dictionary (GIF_DICT_MODE != 0) requires GIF_LAZY_RESET=1"
#endif

// Palette storage mode (1 = RGB565 LE/BE only, saves 512 B + Flash; 0 = all formats)
#ifndef GIF_RGB565_ONLY
#define GIF_RGB565_ONLY 0
#endif

// De-chunked LZW input buffer size (minimum 260 B to hold at least one 255-byte sub-block)
#ifndef GIF_LZW_BUF_SIZE
#define GIF_LZW_BUF_SIZE (6*MAX_CHUNK_SIZE)
#endif
#if GIF_LZW_BUF_SIZE < 260
#error "GIF_LZW_BUF_SIZE must be >= 260"
#endif
#define LZW_BUF_SIZE GIF_LZW_BUF_SIZE
#define LZW_HIGHWATER ((LZW_BUF_SIZE * 2) / 3)

// Pixel expansion / temp buffer size (minimum 768 B for 256-color RGB palette reads;
// chains longer than FILE_BUF_SIZE fall back to bounded multi-chunk emission)
#ifndef GIF_FILE_BUF_SIZE
#define GIF_FILE_BUF_SIZE (1<<MAX_CODE_SIZE)
#endif
#if GIF_FILE_BUF_SIZE < 768
#error "GIF_FILE_BUF_SIZE must be >= 768 (max 256-color RGB palette size)"
#endif
#define FILE_BUF_SIZE GIF_FILE_BUF_SIZE

#ifndef GIF_DISABLE_TURBO
#if (GIF_DICT_MODE != 0) || (GIF_FILE_BUF_SIZE < (1<<MAX_CODE_SIZE)) || (GIF_LZW_BUF_SIZE < (6*MAX_CHUNK_SIZE))
#define GIF_DISABLE_TURBO 1
#else
#define GIF_DISABLE_TURBO 0
#endif
#endif

#if !GIF_DISABLE_TURBO && ((GIF_DICT_MODE != 0) || (GIF_FILE_BUF_SIZE < (1<<MAX_CODE_SIZE)) || (GIF_LZW_BUF_SIZE < (6*MAX_CHUNK_SIZE)))
#error "Turbo mode overlays ucLZW across standard usGIFTable/ucGIFPixels/ucLineBuf; set GIF_DISABLE_TURBO=1 when using LowMem dictionary or smaller buffers."
#endif

// Optional cooperative incremental frame decoding API (adds ~20 bytes to GIFIMAGE)
#ifndef GIF_ENABLE_COOPERATIVE
#define GIF_ENABLE_COOPERATIVE 0
#endif

// Cooperative decode status codes
enum {
    GIF_DECODE_MORE = 0,       // Budget reached; call decodeStep()/decodeLines() again
    GIF_DECODE_FRAME_DONE = 1, // Current frame finished
    GIF_DECODE_END = 2,        // End of GIF stream (trailer / no frame decoded)
    GIF_DECODE_ERR = -1        // Decode or I/O error (inspect getLastError())
};

#define PIXEL_FIRST 0
#define PIXEL_LAST (1<<MAX_CODE_SIZE)
#define LINK_UNUSED 5911 // 0x1717 to use memset
#define LINK_END 5912
#define MAX_HASH 5003
// expanded LZW buffer for Turbo mode
#define LZW_BUF_SIZE_TURBO (LZW_BUF_SIZE + (2<<MAX_CODE_SIZE) + (PIXEL_LAST*2) + MAX_WIDTH)
#define LZW_HIGHWATER_TURBO ((LZW_BUF_SIZE_TURBO * 14) / 16)

//
// Pixel types
//
enum {
   GIF_PALETTE_RGB565_LE = 0, // little endian (default)
   GIF_PALETTE_RGB565_BE,     // big endian
   GIF_PALETTE_RGB888,        // original 24-bpp entries
   GIF_PALETTE_RGB8888,       // 32-bit (alpha = 0xff)
   GIF_PALETTE_1BPP,          // 1-bit per pixel (horizontal, MSB on left)
   GIF_PALETTE_1BPP_OLED      // 1-bit per pixel (vertical, LSB on top)
};
// for compatibility with older code
#define LITTLE_ENDIAN_PIXELS GIF_PALETTE_RGB565_LE
#define BIG_ENDIAN_PIXELS GIF_PALETTE_RGB565_BE
//
// Draw types
//
// RAW = 8-bit palettized pixels requiring transparent pixel handling and conversion through the palette.
//       Each line is sent to the GIFDraw callback as 8-bit pixels. If a framebuffer exists, the lines will be
//       written there too. The GIFDraw callback is optional if there is a framebuffer allocated.
//
// COOKED = 16/24/32-bpp fully rendered pixels ready for display. This requires a full frame buffer with extra
//          room for the fully rendered pixels at the end of the 8-bit pixel buffer. For example, a 160x120
//          canvas size with 24-bit output would require (160*120 + 3*160) bytes.
//          Each prepared line is sent to the GIFDraw callback as a row of 16/24/32-bit pixels.
//
enum {
   GIF_DRAW_RAW = 0,
   GIF_DRAW_COOKED
};

enum {
   GIF_SUCCESS = 0,
   GIF_DECODE_ERROR,
   GIF_TOO_WIDE,
   GIF_INVALID_PARAMETER,
   GIF_UNSUPPORTED_FEATURE,
   GIF_FILE_NOT_OPEN,
   GIF_EARLY_EOF,
   GIF_EMPTY_FRAME,
   GIF_BAD_FILE,
   GIF_ERROR_MEMORY
};

typedef struct gif_file_tag
{
  int32_t iPos; // current file position
  int32_t iSize; // file size
  uint8_t *pData; // memory file pointer
  void * fHandle; // class pointer to File/SdFat or whatever you want
} GIFFILE;

typedef struct gif_info_tag
{
  int32_t iFrameCount; // total frames in file
  int32_t iDuration; // duration of animation in milliseconds
  int32_t iMaxDelay; // maximum frame delay
  int32_t iMinDelay; // minimum frame delay
} GIFINFO;

typedef struct gif_draw_tag
{
    int iX, iY; // Corner offset of this frame on the canvas
    int y; // current line being drawn (0 = top line of image)
    int iWidth, iHeight; // size of this frame
    int iCanvasWidth; // need this to know where to place output in a fully cooked bitmap
    void *pUser; // user supplied pointer
    uint8_t *pPixels; // 8-bit source pixels for this line
    uint16_t *pPalette; // little or big-endian RGB565 palette entries (default)
    uint8_t *pPalette24; // RGB888 palette (optional)
    uint8_t ucTransparent; // transparent color
    uint8_t ucHasTransparency; // flag indicating the transparent color is in use
    uint8_t ucDisposalMethod; // frame disposal method
    uint8_t ucBackground; // background color
    uint8_t ucPaletteType; // type of palette entries
    uint8_t ucIsGlobalPalette; // Flag to indicate that a global palette, rather than a local palette is being used
} GIFDRAW;

// Callback function prototypes
typedef int32_t (GIF_READ_CALLBACK)(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
typedef int32_t (GIF_SEEK_CALLBACK)(GIFFILE *pFile, int32_t iPosition);
typedef void (GIF_DRAW_CALLBACK)(GIFDRAW *pDraw);
typedef void * (GIF_OPEN_CALLBACK)(const char *szFilename, int32_t *pFileSize);
typedef void (GIF_CLOSE_CALLBACK)(void *pHandle);
typedef void * (GIF_ALLOC_CALLBACK)(uint32_t iSize);
typedef void (GIF_FREE_CALLBACK)(void *buffer);
//
// our private structure to hold a GIF image decode state
//
typedef struct gif_image_tag
{
    uint16_t iWidth, iHeight, iCanvasWidth, iCanvasHeight;
    uint16_t iPrevW, iPrevH, iPrevX, iPrevY; // for disposal method 2 (restore to bg)
    uint16_t iX, iY; // GIF corner offset
    uint16_t iBpp;
    int16_t iError; // last error
    uint16_t iFrameDelay; // delay in milliseconds for this frame
    int16_t iRepeatCount; // NETSCAPE animation repeat count. 0=forever
    uint16_t iXCount, iYCount; // decoding position in image (countdown values)
    int iLZWOff; // current LZW data offset
    int iLZWSize; // current quantity of data in the LZW buffer
    int iCommentPos; // file offset of start of comment data
    short sCommentLen; // length of comment
    unsigned char bEndOfFrame;
    unsigned char ucPrevDisp, ucDisposalMethod;
    unsigned char ucGIFBits, ucBackground, ucTransparent, ucCodeStart, ucMap, bUseLocalPalette;
    unsigned char ucPaletteType; // RGB565 or RGB888
    unsigned char ucDrawType; // RAW or COOKED
    GIF_READ_CALLBACK *pfnRead;
    GIF_SEEK_CALLBACK *pfnSeek;
    GIF_DRAW_CALLBACK *pfnDraw;
    GIF_OPEN_CALLBACK *pfnOpen;
    GIF_CLOSE_CALLBACK *pfnClose;
    GIFFILE GIFFile;
    void *pUser;
    unsigned char *pFrameBuffer;
#if !GIF_DISABLE_TURBO
    unsigned char *pTurboBuffer;
#endif
    unsigned char *pPixels, *pOldPixels;
    unsigned char ucFileBuf[FILE_BUF_SIZE]; // holds temp data and pixel expansion stack
#if GIF_RGB565_ONLY
    uint16_t pPalette[MAX_COLORS];
    uint16_t pLocalPalette[MAX_COLORS];
#else
    unsigned short pPalette[(MAX_COLORS * 3)/2]; // can hold RGB565 or RGB888 - set in begin()
    unsigned short pLocalPalette[(MAX_COLORS * 3)/2]; // color palettes for GIF images
#endif
    int iLocalPalSize, iGlobalPalSize;
#if !GIF_DISABLE_TURBO
    union {
        struct {
            uint8_t ucLZW[LZW_BUF_SIZE]; // holds de-chunked LZW data
            // These next 3 are reused as ucTurboLZW when Turbo mode is active
            uint16_t usGIFTable[1<<MAX_CODE_SIZE];
            uint8_t ucGIFPixels[(PIXEL_LAST*2)];
            uint8_t ucLineBuf[MAX_WIDTH+15]; // current line
        };
        uint8_t ucTurboLZW[LZW_BUF_SIZE_TURBO];
    };
#else
    uint8_t ucLZW[LZW_BUF_SIZE]; // holds de-chunked LZW data
#if GIF_DICT_MODE == 0
    uint16_t usGIFTable[1<<MAX_CODE_SIZE];
    uint8_t ucGIFPixels[(PIXEL_LAST*2)];
#elif GIF_DICT_MODE == 16
    uint16_t usGIFTable[1<<MAX_CODE_SIZE]; // 16-bit prefix table (8192 B)
    uint8_t ucGIFPixels[1<<MAX_CODE_SIZE]; // 8-bit suffix table (4096 B)
#elif GIF_DICT_MODE == 12
    // Packed 12-bit prefix table: 2 entries per 3 bytes (6144 B)
    uint8_t ucGIFPrefix12[((1<<MAX_CODE_SIZE) * 3 + 1) / 2];
    uint8_t ucGIFPixels[1<<MAX_CODE_SIZE]; // 8-bit suffix table (4096 B)
#endif
    uint8_t ucLineBuf[MAX_WIDTH+15]; // current line
#endif
    uint8_t *pLineBufAligned;
#if GIF_ENABLE_COOPERATIVE
    // Compact incremental LZW state (~20 bytes) preserved across decodeStep()/decodeLines()
    uint32_t ulCoopBits;
    uint16_t usCoopOldCode, usCoopNextCode, usCoopNextLim, usCoopMask, usCoopCC, usCoopEOI;
    uint8_t ucCoopCodeSize, ucCoopBitCount, ucCoopState, ucCoopFirstChar;
#endif
} GIFIMAGE;

#ifdef __cplusplus
//
// The GIF class wraps portable C code which does the actual work
//
class AnimatedGIF
{
  public:
    int open(uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw);
    int openFLASH(uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw);
#ifdef __LINUX__
    int open(const char *szFilename, GIF_DRAW_CALLBACK *pfnDraw);
#endif
    int open(const char *szFilename, GIF_OPEN_CALLBACK *pfnOpen, GIF_CLOSE_CALLBACK *pfnClose, GIF_READ_CALLBACK *pfnRead, GIF_SEEK_CALLBACK *pfnSeek, GIF_DRAW_CALLBACK *pfnDraw);
    void close();
    void reset();
    void begin(uint8_t ucPaletteType = GIF_PALETTE_RGB565_LE);
    void begin(int iEndian, uint8_t ucPaletteType) { begin(ucPaletteType); };
    int playFrame(bool bSync, int *delayMilliseconds, void *pUser = NULL);
#if GIF_ENABLE_COOPERATIVE
    int beginFrame(void *pUser = NULL);
    int decodeStep(uint32_t u32BudgetUs, int *delayMilliseconds = NULL);
    int decodeLines(int iMaxLines, int *delayMilliseconds = NULL);
    bool frameComplete() const;
#endif
    int getCanvasWidth();
    int getFrameWidth();
    int getFrameHeight();
    int getFrameXOff();
    int getFrameYOff();
    int allocTurboBuf(GIF_ALLOC_CALLBACK *pfnAlloc = nullptr);
    int allocFrameBuf(GIF_ALLOC_CALLBACK *pfnAlloc = nullptr);
    void setTurboBuf(void *pTurboBuffer);
    void setFrameBuf(void *pFrameBuffer);
    int setDrawType(int iType);
    int freeFrameBuf(GIF_FREE_CALLBACK *pfnFree);
    int freeTurboBuf(GIF_FREE_CALLBACK *pfnFree);
    uint8_t *getFrameBuf();
    uint8_t *getTurboBuf();
    int getCanvasHeight();
    int getLoopCount();
    int getInfo(GIFINFO *pInfo);
    int getLastError();
    int getComment(char *destBuffer);
    void mergeTransparent(uint8_t *pSrc, uint8_t *pDst, uint8_t ucTrans, int iLen);
    void cookPixels(uint8_t *pSrc, uint8_t *pDst, int iTrans, int iLen, uint32_t *pPalette, uint16_t *pRGB565);

  private:
    GIFIMAGE _gif;
};
#else
// C interface
    int GIF_openRAM(GIFIMAGE *pGIF, uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw);
    int GIF_openFile(GIFIMAGE *pGIF, const char *szFilename, GIF_DRAW_CALLBACK *pfnDraw);
    void GIF_close(GIFIMAGE *pGIF);
    void GIF_begin(GIFIMAGE *pGIF, unsigned char ucPaletteType);
    void GIF_reset(GIFIMAGE *pGIF);
    int GIF_playFrame(GIFIMAGE *pGIF, int *delayMilliseconds, void *pUser);
#if GIF_ENABLE_COOPERATIVE
    int GIF_beginFrame(GIFIMAGE *pGIF, void *pUser);
    int GIF_decodeStep(GIFIMAGE *pGIF, uint32_t u32BudgetUs, int *delayMilliseconds);
    int GIF_decodeLines(GIFIMAGE *pGIF, int iMaxLines, int *delayMilliseconds);
    int GIF_frameComplete(const GIFIMAGE *pGIF);
#endif
    int GIF_getCanvasWidth(GIFIMAGE *pGIF);
    int GIF_getCanvasHeight(GIFIMAGE *pGIF);
    int GIF_getComment(GIFIMAGE *pGIF, char *destBuffer);
    int GIF_getInfo(GIFIMAGE *pGIF, GIFINFO *pInfo);
    int GIF_getLastError(GIFIMAGE *pGIF);
    int GIF_getLoopCount(GIFIMAGE *pGIF);
    void GIF_mergeTransparent(uint8_t *pSrc, uint8_t *pDst, uint8_t ucTrans, int iLen);
    void GIF_cookPixels(uint8_t *pSrc, uint8_t *pDst, int iTrans, int iLen, uint32_t *pPalette, uint16_t *pRGB565);
#endif // __cplusplus

#if (INTPTR_MAX == INT64_MAX)
#define REGISTER_WIDTH 64
#define BIGINT int64_t
#define BIGUINT uint64_t
#else
#define REGISTER_WIDTH 32
#define BIGINT int32_t
#define BIGUINT uint32_t
#endif // 64 vs 32-bit native register size

// Portable, strict-aliasing and alignment-safe little-endian readers
#define INTELSHORT(p) ((uint16_t)((uint16_t)((const uint8_t *)(p))[0] | ((uint16_t)((const uint8_t *)(p))[1] << 8)))
#define INTELLONG(p) ((uint32_t)((uint32_t)((const uint8_t *)(p))[0] | ((uint32_t)((const uint8_t *)(p))[1] << 8) | ((uint32_t)((const uint8_t *)(p))[2] << 16) | ((uint32_t)((const uint8_t *)(p))[3] << 24)))

#endif // __ANIMATEDGIF__
