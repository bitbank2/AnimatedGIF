//
// GIF Animator
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 7/5/2020
// Original GIF code written 20+ years ago :)
// The goal of this code is to decode images up to 480x320
// using no more than 20K of RAM (if sent directly to an LCD display)
//
//

#include "gif.h"

static const unsigned char cGIFBits[9] = {1,4,4,4,8,8,8,8,8}; // convert odd bpp values to ones we can handle
static const unsigned char cGIFPass[8] = {8,0,8,4,4,2,2,1}; // GIF interlaced y delta

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  return 0;
} /* GIFReadFile() */

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition)
{
  return 0;
} /* GIFSeekFile() */

int32_t GIFReadMem(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    int32_t iBytesRead;

    iBytesRead = iLen;
    if ((pFile->iSize - pFile->iPos) < iLen)
       iBytesRead = pFile->iSize - pFile->iPos;
    if (iBytesRead <= 0)
       return 0;
    memcpy(pBuf, &pFile->pData[pFile->iPos], iBytesRead);
    pFile->iPos += iBytesRead;
    return iBytesRead;
} /* GIFReadMem() */

int32_t GIFSeekMem(GIFFILE *pFile, int32_t iPosition)
{
    if (iPosition < 0) iPosition = 0;
    else if (iPosition >= pFile->iSize) iPosition = pFile->iSize-1;
    pFile->iPos = iPosition;
    return iPosition;
} /* GIFSeekFile() */


void GIFInit(GIFIMAGE *pGIF, char *szName, uint8_t *pData, int iDataSize)
{
  memset(pGIF, 0, sizeof(GIFIMAGE));
  if (szName)
  {
    pGIF->pfnSeek = GIFSeekFile;
    pGIF->pfnRead = GIFReadFile;    
  }
  else
  {
    pGIF->pfnSeek = GIFSeekMem;
    pGIF->pfnRead = GIFReadMem;
  }
  pGIF->pfnDraw = GIFDrawNoMem; //GIFDraw;
  pGIF->GIFFile.iSize = iDataSize;
  pGIF->GIFFile.pData = pData;
  pGIF->GIFFile.iPos = 0; // start of file
} /* GIFInit() */

#ifdef NOT_USED
//
// CountGIFFrames
//
// Given a handle to an open multipage GIF image
// This function will seek through the file and count the number
// of frames present
//
int CountGIFFrames(unsigned char *cBuf, int iFileSize, int **pOffsets)
{
    int *p, iNumFrames;
    int iOff;
    int bDone = 0;
    int bExt;
    unsigned char c;
    
    p = *pOffsets = (int *)malloc(0x1000); // 4k - max 1024 frames
    iNumFrames = 0;
    p[iNumFrames++] = 0; // first offset is 0
    iOff = 10;
    c = cBuf[iOff]; // get info bits
    iOff += 3;   /* Skip flags, background color & aspect ratio */
    if (c & 0x80) /* Deal with global color table */
    {
        c &= 7;  /* Get the number of colors defined */
        iOff += (2<<c)*3; /* skip color table */
    }
    while (!bDone && iOff < iFileSize)
    {
        bExt = 1; /* skip extension blocks */
        while (!bDone && bExt && iOff < iFileSize)
        {
            switch(cBuf[iOff])
            {
                case 0x3b: /* End of file */
                    /* we were fooled into thinking there were more frames */
                    iNumFrames--;
                    bDone = 1;
                    continue;
                    // F9 = Graphic Control Extension (fixed length of 4 bytes)
                    // FE = Comment Extension
                    // FF = Application Extension
                    // 01 = Plain Text Extension
                case 0x21: /* Extension block */
                    iOff += 2; /* skip to length */
                    iOff += (int)cBuf[iOff]; /* Skip the data block */
                    iOff++;
                    // block terminator or optional sub blocks
                    c = cBuf[iOff++]; /* Skip any sub-blocks */
                    while (c && iOff < iFileSize)
                    {
                        iOff += (int)c;
                        c = cBuf[iOff++];
                    }
                    if (c != 0) // problem, we went past the end
                    {
                        iNumFrames--; // possible corrupt data; stop
                        bDone = 1;
                        continue;
                    }
                    break;
                case 0x2c: /* Start of image data */
                    bExt = 0; /* Stop doing extension blocks */
                    break;
                default:
                    /* Corrupt data, stop here */
                    iNumFrames--;
                    bDone = 1;
                    //                    *bTruncated = TRUE;
                    continue;
            }
        }
        if (iOff >= iFileSize) // problem
        {
            iNumFrames--; // possible corrupt data; stop
            bDone = 1;
            //            *bTruncated = TRUE;
            continue;
        }
        /* Start of image data */
        c = cBuf[iOff+9]; /* Get the flags byte */
        iOff += 10; /* Skip image position and size */
        if (c & 0x80) /* Local color table */
        {
            c &= 7;
            iOff += (2<<c)*3;
        }
        iOff++; /* Skip LZW code size byte */
        c = cBuf[iOff++];
        while (c && iOff < iFileSize) /* While there are more data blocks */
        {
            iOff += (int)c;  /* Skip this data block */
            if (iOff > iFileSize) // past end of file, stop
            {
                iNumFrames--; // don't count this frame
                break; // last page is corrupted, don't use it
            }
            c = cBuf[iOff++]; /* Get length of next */
        }
        /* End of image data, check for more frames... */
        if ((iOff > iFileSize) || cBuf[iOff] == 0x3b)
        {
            bDone = 1; /* End of file has been reached */
        }
        else
        {
            p[iNumFrames++] = (int)iOff;
        }
    } /* while !bDone */
    if (iNumFrames == 1) // no need for offset list
    {
        free(*pOffsets);
        *pOffsets = NULL;
    }
    return iNumFrames;
    
} /* CountGIFFrames() */
#endif // NOT_USED
//
// Parse the GIF header, gather the size and palette info
// Returns 1 for success, 0 for failure
//
int GIFParseInfo(GIFIMAGE *pPage)
{
    int i, j, iColorTableBits;
    unsigned char c, *p;
    int32_t iOffset = 0;
    int32_t iStartPos = pPage->GIFFile.iPos; // starting file position
    
    pPage->bUseLocalPalette = 0; // assume no local palette
    pPage->bEndOfFrame = 0; // we're just getting started
    
    p = pPage->ucFileBuf;
    (*pPage->pfnRead)(&pPage->GIFFile, pPage->ucFileBuf, 2048); // 2k is plenty for this task
    if (iStartPos == 0) // start of the file
    { // overall image size
        pPage->iOriginalWidth = pPage->iWidth = INTELSHORT(&p[6]);
        pPage->iOriginalHeight = pPage->iHeight = INTELSHORT(&p[8]);
        pPage->iBpp = ((p[10] & 0x70) >> 4) + 1;
        iColorTableBits = (p[10] & 7) + 1; // Log2(size) of the color table
        pPage->cBackground = p[11]; // background color
        iOffset = 13;
        if (p[10] & 0x80) // global color table?
        { // convert to byte-reversed RGB565 for immediate use
            for (i=0; i<(1<<iColorTableBits); i++)
            {
                uint16_t usRGB565;
                usRGB565 = ((p[iOffset] >> 3) << 11); // R
                usRGB565 |= ((p[iOffset+1] >> 2) << 5); // G
                usRGB565 |= (p[iOffset+2] >> 3); // B
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
                        pPage->cGIFBits = p[iOffset+1]; // packed fields
                        //                        pPage->iFrameDelay = (INTELSHORT(&p[iOffset+2]))*10; // delay in ms
                        //                        if (pPage->iFrameDelay < 30) // 0-2 is going to make it run at 60fps; use 100 (10fps) as a reasonable substitute
                        //                            pPage->iFrameDelay = 100;
                        if (pPage->cGIFBits & 1) // transparent color is used
                            pPage->cTransparent = p[iOffset+4]; // transparent color index
                        iOffset += 6;
                    }
                    //                     else   // error
                    break;
                case 0xff: /* App extension */
                    c = 1;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
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
                    j = 0;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
                        if (j == 0) // use only first block
                        {
                            j = c;
                            if (j > 127)   // max comment length = 127
                                j = 127;
                            //                           memcpy(pPage->szComment, &p[iOffset], j);
                            //                           pPage->szComment[j] = '\0';
                            j = 1;
                        }
                        iOffset += (int)c; /* Skip this sub-block */
                    }
                    break;
                default:
                    /* Bad header info */
                    return NULL;
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
    pPage->cMap = p[iOffset++];
    if (pPage->cMap & 0x80) // local color table?
    {// convert to byte-reversed RGB565 for immediate use
        for (i=0; i<(1<<((pPage->cMap & 7)+1)); i++)
        {
            uint16_t usRGB565;
            usRGB565 = ((p[iOffset] >> 3) << 11); // R
            usRGB565 |= ((p[iOffset+1] >> 2) << 5); // G
            usRGB565 |= (p[iOffset+2] >> 3); // B
            pPage->pLocalPalette[i] = __builtin_bswap16(usRGB565); // SPI wants MSB first
            iOffset += 3;
        }
        pPage->bUseLocalPalette = 1;
    }
    pPage->cCodeStart = p[iOffset++]; /* initial code size */
    /* Since GIF can be 1-8 bpp, we only allow 1,4,8 */
//    if (iRequestedPage != 0)
        pPage->iBpp = cGIFBits[pPage->cCodeStart]; // DEBUG
    // we are re-using the same buffer turning GIF file data
    // into "pure" LZW
    (*pPage->pfnSeek)(&pPage->GIFFile, iStartPos + iOffset); // position file to new spot
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
    memcpy(pPage->ucLZW, &pPage->ucLZW[pPage->iLZWOff], pPage->iLZWSize - pPage->iLZWOff);
    pPage->iLZWSize -= pPage->iLZWOff;
    pPage->iLZWOff = 0;
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

#ifdef NOT_USED
//
// Extract and pack the GIF data for the requested frame
// returns a buffer of the GIF data and fills in the IMAGE structure
// in preparation for decoding
//
unsigned char * ReadGIF(GIFIMAGE *pPage, unsigned char *pData, int blob_size, int *size_out, int *pOffsets, int iRequestedPage)
{
    int iOffset, iErr, i, j, iMap, iColorTableBits;
    int iDataLen, iDataSize, codestart;
    unsigned char c, *d, *p, *pGIF;
    
    iErr = 0;
    iOffset = 0;
    if (pOffsets) // list of page offsets
    {
        iOffset = pOffsets[iRequestedPage];
        iDataSize = pOffsets[iRequestedPage+1] - pOffsets[iRequestedPage];
    }
    else
    {
        iDataSize = (int)blob_size;
    }
    if (iDataSize <= 0 || iOffset < 0 || iOffset+iDataSize > (int)blob_size)
    {
        // something went very wrong
        return NULL;
    }
    pGIF = (unsigned char *)malloc(iDataSize);
    *size_out = iDataSize;
    p = &pData[iOffset];
    iOffset = 0;
    if (iRequestedPage == 0)
    {
        pPage->iWidth = INTELSHORT(&p[6]);
        pPage->iHeight = INTELSHORT(&p[8]);
        pPage->iBpp = ((p[10] & 0x70) >> 4) + 1;
        iColorTableBits = (p[10] & 7) + 1; // Log2(size) of the color table
        pPage->cBackground = p[11]; // background color
        iOffset = 13;
        if (p[10] & 0x80) // global color table?
        {
            pPage->pPalette = (unsigned char *) malloc(768); /* Allocate fixed size color palette */
            if (pPage->pPalette == NULL)
            {
                free(pGIF);
                return NULL;
            }
            memcpy(pPage->pPalette, &p[iOffset], 3*(1 << iColorTableBits));
            iOffset += 3 * (1 << iColorTableBits);
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
                        pPage->cGIFBits = p[iOffset+1]; // packed fields
                        //                        pPage->iFrameDelay = (INTELSHORT(&p[iOffset+2]))*10; // delay in ms
                        //                        if (pPage->iFrameDelay < 30) // 0-2 is going to make it run at 60fps; use 100 (10fps) as a reasonable substitute
                        //                            pPage->iFrameDelay = 100;
                        if (pPage->cGIFBits & 1) // transparent color is used
                            pPage->cTransparent = p[iOffset+4]; // transparent color index
                        iOffset += 6;
                    }
                    //                     else   // error
                    break;
                case 0xff: /* App extension */
                    c = 1;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
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
                    j = 0;
                    while (c) /* Skip all data sub-blocks */
                    {
                        c = p[iOffset++]; /* Block length */
                        if (j == 0) // use only first block
                        {
                            j = c;
                            if (j > 127)   // max comment length = 127
                                j = 127;
                            //                           memcpy(pPage->szComment, &p[iOffset], j);
                            //                           pPage->szComment[j] = '\0';
                            j = 1;
                        }
                        iOffset += (int)c; /* Skip this sub-block */
                    }
                    break;
                default:
                    /* Bad header info */
                    if (pPage->iBpp != 1)
                        free(pPage->pPalette);
                    return NULL;
            } /* switch */
        }
        else // invalid byte, stop decoding
        {
            /* Bad header info */
            if (pPage->iBpp != 1)
            {
                if (pPage->pPalette)
                {
                    free(pPage->pPalette);
                    pPage->pPalette = NULL;
                }
                if (pPage->pLocalPalette)
                {
                    free(pPage->pLocalPalette);
                    pPage->pLocalPalette = NULL;
                }
            }
            return NULL;
        }
    } /* while */
    if (p[iOffset] == ',')
        iOffset++;
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
    iMap = p[iOffset++];
    if (iMap & 0x80) // local color table?
    {
        if (iRequestedPage == 0 && pPage->pPalette == NULL) // no global color table defined, use local as global
        {
            pPage->pPalette = (unsigned char *) malloc(768);
            if (pPage->pPalette == NULL)
            {
                return NULL;
            }
            i = 3 * (1 << ((iMap & 7)+1)); // get the size of the color table specified
            memcpy(pPage->pPalette, &p[iOffset], i);
        }
        else
        { // keep both global and local color tables
            if (pPage->pLocalPalette == NULL) // need to allocate it
            {
                pPage->pLocalPalette = (unsigned char *) malloc(768);
                if (pPage->pLocalPalette == NULL)
                {
                    return NULL;
                }
            }
            i = 3 * (1 << ((iMap & 7)+1)); // get the size of the color table specified
            memcpy(pPage->pLocalPalette, &p[iOffset], i);
        }
        iOffset += i;
    }
    codestart = p[iOffset++]; /* initial code size */
    /* Since GIF can be 1-8 bpp, we only allow 1,4,8 */
    if (iRequestedPage != 0)
        pPage->iBpp = cGIFBits[codestart];
    // we are re-using the same buffer turning GIF file data
    // into "pure" LZW
    d = pGIF;
    *d++ = (unsigned char)iMap; /* Store the map attributes */
    *d++ = (unsigned char)codestart; /* And the starting code */
    iDataLen = 2; /* Length of GIF data */
    c = p[iOffset++]; /* This block length */
    while (c && (iOffset+c) <= iDataSize) /* Collect all of the data packets together */
    {
        memcpy(d, &p[iOffset], c);
        iOffset += c;
        d += c;
        iDataLen += c; /* Overall data length */
        c = p[iOffset++];
    }
    if ((iOffset+c) > iDataSize) /* Error, we went beyond end of buffer */
    {
        free(pGIF);
        if (pPage->pPalette)
        {
            free(pPage->pPalette);
            pPage->pPalette = NULL;
        }
        if (pPage->pLocalPalette)
        {
            free(pPage->pLocalPalette);
            pPage->pLocalPalette = NULL;
        }
        return NULL;
    }
    //         pPage->iDataSize = iDataLen; /* Total bytes of GIF data */
    return pGIF;
} /* PILReadGIF() */
//
// AnimateGIF
// BitBlt the new data onto the old page for animation (GIF).
//
int AnimateGIF(GIFIMAGE *pDestPage, GIFIMAGE *pSrcPage)
{
    
    unsigned char c, c1, *s, *d, cTransparent;
    int x, y;
    unsigned char ucDisposalFlag;
    int iDestPitch, iSrcPitch;
    
    if (pDestPage == NULL || pSrcPage == NULL)
        return -1;
//    if (pDestPage->pPixels == NULL || pSrcPage->pPixels == NULL || pDestPage->pPalette == NULL) // must have destination buffer, src buffer & global color table
//        return -1;
    if (pSrcPage->iX < 0 || pSrcPage->iY < 0 || (pSrcPage->iX + pSrcPage->iWidth) > pDestPage->iWidth || (pSrcPage->iY + pSrcPage->iHeight) > pDestPage->iHeight)
        return -1; // bad parameter
    iDestPitch = ((pDestPage->iWidth * pDestPage->iBpp) + 7) >> 3;
    iSrcPitch = ((pSrcPage->iWidth * pSrcPage->iBpp) + 7) >> 3;
    
    // Dispose of the last frame according to the previous page disposal flags
    ucDisposalFlag = (pDestPage->cGIFBits & 0x1c)>>2; // bits 2-4 = disposal flags
    switch (ucDisposalFlag)
    {
        case 0: // not specified - nothing to do
        case 1: // do not dispose
            break;
        case 2: // restore to background color
            for (y=pDestPage->iY; y<pDestPage->iY + pDestPage->iOldHeight; y++)
            {
                d = pDestPage->pPixels + (y * iDestPitch) + pDestPage->iX;
                memset(d, pDestPage->cBackground, pDestPage->iOldWidth);
            } // for y
            break;
        case 3: // restore to previous frame
            for (y=pDestPage->iY; y<pDestPage->iY + pDestPage->iOldHeight; y++)
            {
                d = pDestPage->pPixels + (y * iDestPitch) + pDestPage->iX;
                s = pDestPage->pOldPixels + (y * iDestPitch) + pDestPage->iX;
                memcpy(d, s, pDestPage->iOldWidth);
            }
            break;
        default: // not defined
            break;
    }
    
    // if this frame uses disposal method 3, we need to prepare for the next frame
    // by saving the current image before it gets modified
    //    if (((pSrcPage->cGIFBits & 0x1c)>>2) == 3)
    {
        // Copy this frame to a swap buffer so that the disposal method 3 can work
        // this can be done more efficiently (memory, time), but keep it simple for now
        memcpy(pDestPage->pOldPixels, pDestPage->pPixels, (pDestPage->iWidth * pDestPage->iHeight * pDestPage->iBpp) >> 3);
    }
    
    switch (pSrcPage->iBpp)
    {
#ifdef FUTURE
        case 1:
            break;
#endif
        case 4:
            // Draw new sub-image onto animation bitmap
            if (pSrcPage->cGIFBits & 1) // if transparency used
                cTransparent = pSrcPage->cTransparent;
            else
                cTransparent = 16; // a value which will never match
            c1 = 0xff; // suppress compiler warning
            for (y=0; y<pSrcPage->iHeight; y++)
            {
                s = pSrcPage->pPixels + (y * iSrcPitch);
                d = &pDestPage->pPixels[iDestPitch * (pSrcPage->iY + y) + (pSrcPage->iX/2)];
                if ((pSrcPage->iX & 1) == 0) { // easier case
                    memcpy(d, s, pSrcPage->iWidth/2); // copy pairs of pixels
                    if (pSrcPage->iWidth & 1) { // final odd pixel
                        d[0] &= 0xf;
                        d[0] |= (s[0] & 0xf0); // replace upper nibble
                    }
                } else { // more complicated scenario
                    for (x=0; x<pSrcPage->iWidth; x++)
                    {
                        if (x & 1)
                            c = (*s++) & 0xf;
                        else
                            c = ((*s)>> 4) & 0xf;
                        if (c != cTransparent)
                        {
                            if (x & 1) {
                                d[0] &= 0xf0;
                                d[0] = c;
                            } else {
                                d[0] &= 0xf;
                                d[0] |= (c << 4);
                            }
                        }
                        if (!(x & 1))
                            d++;
                    } // for x
                }
            } // for y
            break;
        case 8:
            // Draw new sub-image onto animation bitmap
            if (pSrcPage->cGIFBits & 1) // if transparency used
            {
                cTransparent = (unsigned char)pSrcPage->cTransparent;
                for (y=0; y<pSrcPage->iHeight; y++)
                {
                    s = pSrcPage->pPixels + (y * iSrcPitch);
                    d = pDestPage->pPixels + (iDestPitch * (pSrcPage->iY + y)) + pSrcPage->iX;
                    for (x=0; x<pSrcPage->iWidth; x++)
                    {
                        c = *s++;
                        if (c != cTransparent)
                            *d = c;
                        d++;
                    } // for x
                } // for y
            }
            else // no transparency
            {
                for (y=0; y<pSrcPage->iHeight; y++)
                {
                    s = pSrcPage->pPixels + (y * iSrcPitch);
                    d = pDestPage->pPixels + (iDestPitch * (pSrcPage->iY + y)) + pSrcPage->iX;
                    memcpy(d, s, pSrcPage->iWidth);
                } // for y
            } // no transparency
            break;
    }
    
    // Need to hold last frame info for posible "disposition" on the next frame
    pDestPage->cGIFBits = pSrcPage->cGIFBits;
    //   pDestPage->iFrameDelay = pSrcPage->iFrameDelay;
    pDestPage->iX = pSrcPage->iX;
    pDestPage->iY = pSrcPage->iY;
    pDestPage->iOldWidth = pSrcPage->iWidth;
    pDestPage->iOldHeight = pSrcPage->iHeight;
    return 0;
    
} /* AnimateGIF() */
#endif // NOT_USED

//
// MakeGIFPels
// returns true if the Y changed (aka we finished drawing a line)
//MakeGIFPels(code, &xcount, &y, pImage, lsize)
static void MakeGIFPels(GIFIMAGE *pPage, unsigned int code)
{
    int i, k, iPixCount;
    unsigned short *giftabs;
    unsigned char *buf, *s, *pEnd, *gifpels;
    /* Copy this string of sequential pixels to output buffer */
    //   iPixCount = 0;
    s = pPage->ucFileBuf + FILE_BUF_SIZE; /* Pixels will come out in reversed order */
    buf = pPage->ucLineBuf + (pPage->iWidth - pPage->iXCount);
    giftabs = &pPage->usGIFTable[CTLINK];
    gifpels = &pPage->ucGIFPixels[CTLAST];
    while (code < CT_OLD)
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
                pEnd = buf + pPage->iXCount;
                while (buf < pEnd)
                {
                    *buf++ = *s++;
                }
            iPixCount -= pPage->iXCount;
            pPage->iXCount = pPage->iWidth; /* Reset pixel count */
            (*pPage->pfnDraw)(pPage); // callback to handle this line
            pPage->iYCount--;
            if ((pPage->iYCount & 3) == 0) // since we support only small images...
                GIFGetMoreData(pPage); // check if we need to read more LZW data every 4 lines
        }
    } /* while */
    return;
} /* MakeGIFPels() */

//
// Decode LZW into an image
//
int DecodeLZW(GIFIMAGE *pImage, int iOptions)
{
    int i, bitnum;
    unsigned short oldcode, codesize, nextcode, nextlim;
    unsigned short *giftabs, cc, eoi;
    signed short sMask;
    unsigned char *gifpels, *pOutPtr, *p;
    //    int iStripSize;
    //unsigned char **index;
#ifdef _64BITS
    uint64_t ulBits;
#else
    uint32_t ulBits;
#endif
    unsigned short code;
    // if output can be used for string table, do it faster
    //       if (bGIF && (OutPage->cBitsperpixel == 8 && ((OutPage->iWidth & 3) == 0)))
    //          return PILFastLZW(InPage, OutPage, bGIF, iOptions);
    
    pOutPtr = NULL; // suppress compiler warning
    
    p = pImage->ucLZW; // un-chunked LZW data
    sMask = -1 << (pImage->cCodeStart + 1);
    sMask = 0xffff - sMask;
    cc = (sMask >> 1) + 1; /* Clear code */
    eoi = cc + 1;
    giftabs = pImage->usGIFTable;
    gifpels = pImage->ucGIFPixels;
    pImage->iYCount = pImage->iHeight; // count down the lines
    pImage->iXCount = pImage->iWidth;
    bitnum = 0;
    pImage->iLZWOff = 0; // Offset into compressed data
    pImage->iLZWSize = 0; // no data buffered (yet)
    GIFGetMoreData(pImage); // Read some data to start
    // Initialize code table
    // this part only needs to be initialized once
    for (i = 0; i < cc; i++)
    {
        gifpels[CTFIRST + i] = gifpels[CTLAST + i] = (unsigned short) i;
        giftabs[CTLINK + i] = CT_END;
    }
init_codetable:
    codesize = pImage->cCodeStart + 1;
    sMask = -1 << (pImage->cCodeStart + 1);
    sMask = 0xffff - sMask;
    nextcode = cc + 2;
    nextlim = (unsigned short) ((1 << codesize));
    // This part of the table needs to be reset multiple times
    memset(&giftabs[CTLINK + cc], CT_OLD, (4096 - cc)*sizeof(short));
    oldcode = CT_END;
    code = CT_END;
#ifdef _64BITS
    ulBits = INTELEXTRALONG(&p[pImage->iLZWOff]);
#else
    ulBits = INTELLONG(&p[pImage->iLZWOff]);
#endif
    while (code != eoi && pImage->iYCount > 0) // && y < pImage->iHeight+1) /* Loop through all lines of the image (or strip) */
    {
        if (bitnum > (REGISTER_WIDTH - codesize))
        {
            pImage->iLZWOff += (bitnum >> 3);
            bitnum &= 7;
#ifdef _64BITS
            ulBits = INTELEXTRALONG(&p[pImage->iLZWOff]);
#else
            ulBits = INTELLONG(&p[pImage->iLZWOff]);
#endif
        }
        code = (unsigned short) (ulBits >> bitnum); /* Read a 32-bit chunk */
        code &= sMask;
        bitnum += codesize;
        if (code == cc) /* Clear code?, and not first code */
            goto init_codetable;
        if (code != eoi)
        {
            if (oldcode != CT_END)
            {
                if (nextcode < nextlim) // for deferred cc case, don't let it overwrite the last entry (fff)
                {
                    giftabs[CTLINK + nextcode] = oldcode;
                    gifpels[CTFIRST + nextcode] = gifpels[CTFIRST + oldcode];
                    if (giftabs[CTLINK + code] == CT_OLD) /* Old code */
                        gifpels[CTLAST + nextcode] = gifpels[CTFIRST + oldcode];
                    else
                        gifpels[CTLAST + nextcode] = gifpels[CTFIRST + code];
                }
                nextcode++;
                if (nextcode >= nextlim && codesize < 12)
                {
                    codesize++;
                    nextlim <<= 1;
                    sMask = (sMask << 1) | 1;
                }
            }
            MakeGIFPels(pImage, code);
//            if (buf == NULL)
//                goto gif_forced_error; /* Leave with error */
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
