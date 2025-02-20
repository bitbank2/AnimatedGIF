//
//  GIF Functional Test
//  Created by Larry Bank on 2/19/25.
//
#include "../../../src/AnimatedGIF.cpp"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
// test images
#include "../../../test_images/earth_128x128.h"

AnimatedGIF gif; // static instance of GIF class

//
// Return the current time in microseconds
//
int Micros(void)
{
int iTime;
struct timespec res;

    clock_gettime(CLOCK_MONOTONIC, &res);
    iTime = (int)(1000000*res.tv_sec + res.tv_nsec/1000);

    return iTime;
} /* Micros() */

void GIFDraw(GIFDRAW *pDraw)
{
//  if (pDraw->y == 0) { // set address window on first line
//    lcd.setAddrWindow(pDraw->iX + iXOff, pDraw->iY + iYOff, pDraw->iWidth, pDraw->iHeight);
//  }
//  lcd.pushPixels((uint16_t *)pDraw->pPixels, pDraw->iWidth);
} /* GIFDraw() */

//
// Simple logging print
//
void GIFLOG(int line, char *string, const char *result)
{
    printf("Line: %d: msg: %s%s\n", line, string, result);
} /* GIFLOG() */

int main(int argc, const char * argv[]) {
    int i, rc, iTime1, iTime2;
    int w, h, iFrame, iTotal = 0;
    uint8_t *pFuzzData;
    char *szTestName;
    int iTotalPass, iTotalFail;
    uint8_t *pFrameBuffer;
    const char *szStart = " - START";

    iTotalPass = iTotalFail = iTotal++;
    // Test 1 - Decode a file to completion
    szTestName = (char *)"GIF full file decode";
    iTotal++;
    GIFLOG(__LINE__, szTestName, szStart);
    if (gif.open((uint8_t *)earth_128x128, sizeof(earth_128x128), GIFDraw)) {
        w = gif.getCanvasWidth();
        h = gif.getCanvasHeight();
        pFrameBuffer = (uint8_t *)malloc(w * (h+2)); // 2 extra lines for cooked pixels
        gif.setDrawType(GIF_DRAW_COOKED);
        gif.setFrameBuf(pFrameBuffer);
        // By setting the framebuffer pointer and passing a GIFDraw callback
        // we tell AnimatedGIF that we are only buffering the canvas as 8-bpp
        // and that we need the pixels converted through the palette one line
        // at a time
        iFrame = 0;
        iTime1 = Micros();
        while (gif.playFrame(false, NULL)) {
            iFrame++;
        }
        free(pFrameBuffer);
        if (iFrame == 102) {
            iTotalPass++;
            GIFLOG(__LINE__, szTestName, " - PASSED");
        } else {
            iTotalFail++;
            GIFLOG(__LINE__, szTestName, " - FAILED");
        }
    } else {
        GIFLOG(__LINE__, szTestName, "Error opening GIF file.");
    }
    // Test 2 - Verify bad parameters return an error
    szTestName = (char *)"GIF bad parameter test";
    iTotal++;
    GIFLOG(__LINE__, szTestName, szStart);
    if (gif.open((uint8_t *)earth_128x128, sizeof(earth_128x128), NULL)) {
        gif.setDrawType(GIF_DRAW_COOKED); // set cooked pixel type without a framebuffer nor GIFDRAW callback
        gif.setFrameBuf(NULL);
        gif.playFrame(false, NULL);
        if (gif.getLastError() == GIF_INVALID_PARAMETER) {
            iTotalPass++;
            GIFLOG(__LINE__, szTestName, " - PASSED");
        } else {
            iTotalFail++;
            GIFLOG(__LINE__, szTestName, " - FAILED");
        }
    } else {
        GIFLOG(__LINE__, szTestName, "Error opening GIF file.");
    }
    // FUZZ testing
    // Randomize the input data (file header and compressed data) and confirm that the library returns an error code
    // and doesn't have an invalid pointer exception
    printf("Begin fuzz testing...\n");
    szTestName = (char *)"Single Byte Sequential Corruption Test";
    iTotal++;
    pFuzzData = (uint8_t *)malloc(sizeof(earth_128x128));
    GIFLOG(__LINE__, szTestName, szStart);
    // We don't need to corrupt the file all the way to the end because it will take a loooong time
    // The header is the main area where corruption can cause erratic behavior
    for (i=0; i<2000; i++) { // corrupt each byte one at a time by inverting it
        memcpy(pFuzzData, earth_128x128, sizeof(earth_128x128)); // start with the valid data
        pFuzzData[i] = ~pFuzzData[i]; // invert the bits of this byte
        if (gif.open(pFuzzData, sizeof(earth_128x128), GIFDraw)) { // the GIF header may be rejected
            iFrame = 0;
            while (gif.playFrame(false, NULL)) {
                iFrame++;
            }
            gif.close();
        }
    } // for each test
    GIFLOG(__LINE__, szTestName, " - PASSED");
    iTotalPass++;
    
    // Fuzz test part 2 - multi-byte random corruption
    szTestName = (char *)"Multi-Byte Random Corruption Test";
    iTotal++;
    GIFLOG(__LINE__, szTestName, szStart);
    for (i=0; i<1000; i++) { // 1000 iterations of random spots in the file to corrupt with random values
        int iOffset;
        memcpy(pFuzzData, earth_128x128, sizeof(earth_128x128)); // start with the valid data
        iOffset = rand() % sizeof(earth_128x128);
        pFuzzData[iOffset] = (uint8_t)rand();
        iOffset = rand() % sizeof(earth_128x128); // corrupt 2 spots just for good measure
        pFuzzData[iOffset] = (uint8_t)rand();
        if (gif.open(pFuzzData, sizeof(earth_128x128), GIFDraw)) { // the JPEG header may be rejected
            iFrame = 0;
            // a certain type of file corruption could cause it to get stuck in an
            // infinite decode loop without being a dangerous fault
            while (gif.playFrame(false, NULL) && iFrame < 102) {
                iFrame++;
            }
        }
    } // for each test
    GIFLOG(__LINE__, szTestName, " - PASSED");
    iTotalPass++;
    
    free(pFuzzData);
    printf("Total tests: %d, %d passed, %d failed\n", iTotal, iTotalPass, iTotalFail);

    return 0;
} /* main() */
