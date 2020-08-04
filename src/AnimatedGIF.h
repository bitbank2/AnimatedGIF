#ifndef __ANIMATEDGIF__
#define __ANIMATEDGIF__
#if defined( __MACH__ ) || defined( __LINUX__ )
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define memcpy_P memcpy
#define PROGMEM
#else
#include <Arduino.h>
#endif
//
// GIF Animator
// Written by Larry Bank
// Copyright (c) 2020 BitBank Software, Inc.
// 
// Designed to decode images up to 480x320
// using less than 22K of RAM
//

/* GIF Defines and variables */
#define MAX_CHUNK_SIZE 255
#define LZW_BUF_SIZE (6*MAX_CHUNK_SIZE)
#define LZW_HIGHWATER (4*MAX_CHUNK_SIZE)
#define MAX_WIDTH 320
#define FILE_BUF_SIZE 4096

#define PIXEL_FIRST 0
#define PIXEL_LAST 4096
#define LINK_UNUSED 5911 // 0x1717 to use memset
#define LINK_END 5912
#define MAX_HASH 5003
#define MAXMAXCODE 4096

// RGB565 pixel byte order in the palette
#define BIG_ENDIAN_PIXELS 0
#define LITTLE_ENDIAN_PIXELS 1

typedef struct gif_file_tag
{
  int32_t iPos; // current file position
  int32_t iSize; // file size
  uint8_t *pData; // memory file pointer
  void * fHandle; // class pointer to File/SdFat or whatever you want
} GIFFILE;

typedef struct gif_draw_tag
{
    int iX, iY; // Corner offset of this frame on the canvas
    int y; // current line being drawn (0 = top line of image)
    int iWidth, iHeight; // size of this frame
    uint8_t *pPixels; // 8-bit source pixels for this line
    uint16_t *pPalette; // little or big-endian RGB565 palette entries
    uint8_t ucTransparent; // transparent color
    uint8_t ucHasTransparency; // flag indicating the transparent color is in use
    uint8_t ucDisposalMethod; // frame disposal method
    uint8_t ucBackground; // background color
} GIFDRAW;

// Callback function prototypes
typedef int32_t (GIF_READ_CALLBACK)(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
typedef int32_t (GIF_SEEK_CALLBACK)(GIFFILE *pFile, int32_t iPosition);
typedef void (GIF_DRAW_CALLBACK)(GIFDRAW *pDraw);
typedef void * (GIF_OPEN_CALLBACK)(char *szFilename, int32_t *pFileSize);
typedef void (GIF_CLOSE_CALLBACK)(void *pHandle);

//
// our private structure to hold a GIF image decode state
//
typedef struct gif_image_tag
{
    int iWidth, iHeight, iCanvasWidth, iCanvasHeight;
    int iX, iY; // GIF corner offset
    int iBpp;
    int iFrameDelay; // delay in milliseconds for this frame
    int iXCount, iYCount; // decoding position in image (countdown values)
    int iLZWOff; // current LZW data offset
    int iLZWSize; // current quantity of data in the LZW buffer
    int iCommentPos; // file offset of start of comment data
    short sCommentLen; // length of comment
    GIF_READ_CALLBACK *pfnRead;
    GIF_SEEK_CALLBACK *pfnSeek;
    GIF_DRAW_CALLBACK *pfnDraw;
    GIF_OPEN_CALLBACK *pfnOpen;
    GIF_CLOSE_CALLBACK *pfnClose;
    GIFFILE GIFFile;
    unsigned char *pPixels, *pOldPixels;
    unsigned char ucLineBuf[MAX_WIDTH]; // current line
    unsigned char ucFileBuf[FILE_BUF_SIZE]; // holds temp data and pixel stack
    unsigned short pPalette[256];
    unsigned short pLocalPalette[256]; // color palettes for GIF images
    unsigned char ucLZW[LZW_BUF_SIZE]; // holds 6 chunks (6x255) of GIF LZW data packed together
    unsigned short usGIFTable[4096];
    unsigned char ucGIFPixels[8192];
    unsigned char bEndOfFrame;
    unsigned char ucGIFBits, ucBackground, ucTransparent, ucCodeStart, ucMap, bUseLocalPalette, ucLittleEndian;
} GIFIMAGE;

//
// The GIF class wraps portable C code which does the actual work
//
class AnimatedGIF
{
  public:
    int open(uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw);
    int open(char *szFilename, GIF_OPEN_CALLBACK *pfnOpen, GIF_CLOSE_CALLBACK *pfnClose, GIF_READ_CALLBACK *pfnRead, GIF_SEEK_CALLBACK *pfnSeek, GIF_DRAW_CALLBACK *pfnDraw);
    void close();
    void reset();
    void begin(int iEndian);
    int playFrame(bool bSync, int *delayMilliseconds);
    int getCanvasWidth();
    int getCanvasHeight();
    int getComment(char *destBuffer);

  private:
    GIFIMAGE _gif;
};

// Due to unaligned memory causing an exception, we have to do these macros the slow way
#define INTELSHORT(p) ((*p) + (*(p+1)<<8))
#define INTELLONG(p) ((*p) + (*(p+1)<<8) + (*(p+2)<<16) + (*(p+3)<<24))
#define MOTOSHORT(p) (((*(p))<<8) + (*(p+1)))
#define MOTOLONG(p) (((*p)<<24) + ((*(p+1))<<16) + ((*(p+2))<<8) + (*(p+3)))

// Must be a 32-bit target processor
#define REGISTER_WIDTH 32

#endif // __ANIMATEDGIF__
