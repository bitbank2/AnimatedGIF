#ifndef __GIF_ANIMATOR__
#define __GIF_ANIMATOR__
//
// GIF Animator
// Written by Larry Bank
// Designed to decode images up to 480x320
// using less than 20K of RAM
#include <stdint.h>
#ifdef __MACH__
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#else
#include <Arduino.h>
#endif // __MACH__
//#ifdef TEENSYDUINO
#include <SD.h>
//#endif

/* GIF Defines and variables */
#define MAX_CHUNK_SIZE 255
#define LZW_BUF_SIZE (6*MAX_CHUNK_SIZE)
#define LZW_HIGHWATER (4*MAX_CHUNK_SIZE)
#define MAX_WIDTH 320
#define FILE_BUF_SIZE 4096

#define PIXEL_FIRST 0
#define PIXEL_LAST 4096
#define CT_OLD 5911 // 0x1717 to use memset
#define CT_END 5912
#define MAX_HASH 5003
#define MAXMAXCODE 4096


typedef struct gif_file_tag
{
  int32_t iPos; // current file position
  int32_t iSize; // file size
  uint8_t *pData; // memory file pointer
//#ifdef TEENSYDUINO
  File fHandle;
//#endif
} GIFFILE;

// Callback function prototype
typedef int32_t (GIF_READ_CALLBACK)(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
typedef int32_t (GIF_SEEK_CALLBACK)(GIFFILE *pFile, int32_t iPosition);
typedef void (GIF_DRAW_CALLBACK)(void *);
//
// our private structure to hold a GIF image and its info
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
    GIF_READ_CALLBACK *pfnRead;
    GIF_SEEK_CALLBACK *pfnSeek;
    GIF_DRAW_CALLBACK *pfnDraw;
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
    unsigned char cGIFBits, cBackground, cTransparent, cCodeStart, cMap, bUseLocalPalette;
} GIFIMAGE;


// Due to unaligned memory causing an exception, we have to do these macros the slow way
#define INTELSHORT(p) ((*p) + (*(p+1)<<8))
#define INTELLONG(p) ((*p) + (*(p+1)<<8) + (*(p+2)<<16) + (*(p+3)<<24))
#define MOTOSHORT(p) (((*(p))<<8) + (*(p+1)))
#define MOTOLONG(p) (((*p)<<24) + ((*(p+1))<<16) + ((*(p+2))<<8) + (*(p+3)))

// Must be a 32-bit target processor
#define REGISTER_WIDTH 32

// API
int CountGIFFrames(unsigned char *cBuf, int iFileSize, int **pOffsets);
unsigned char * ReadGIF(GIFIMAGE *pPage, unsigned char *pData, int blob_size, int *size_out, int *pOffsets, int iRequestedPage);
int AnimateGIF(GIFIMAGE *pDestPage, GIFIMAGE *pSrcPage);
int DecodeLZW(GIFIMAGE *pImage, int iOptions);
int GIFParseInfo(GIFIMAGE *pPage, int bInfoOnly);
int GIFInit(GIFIMAGE *pGIF, char *szName, uint8_t *pData, int iDataSize);
void GIFTerminate(GIFIMAGE *pGIF);
void GIFDrawNoMem(void *p);

#endif // __GIF_ANIMATOR__
