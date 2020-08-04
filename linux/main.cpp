//
//  main.cpp
//  jpeg_test
//
//  Created by Laurence Bank on 8/2/20.
//  Copyright © 2020 Laurence Bank. All rights reserved.
//
#include "../src/AnimatedGIF.h"
#include "../test_images/badgers.h"

AnimatedGIF gif;

void GIFDraw(GIFDRAW *pDraw)
{
} /* GIFDraw() */

int main(int argc, const char * argv[]) {
char szTemp[256];

    gif.begin(BIG_ENDIAN_PIXELS);
    printf("Starting GIF decoder...\n");
    if (gif.open((uint8_t *)ucBadgers, sizeof(ucBadgers), GIFDraw))
    {
        printf("Successfully opened GIF\n");
        printf("Image size: %d x %d\n", gif.getCanvasWidth(), gif.getCanvasHeight());
        if (gif.playFrame(false, NULL))
        {
            printf("Successfully decoded image\n");
        }
        if (gif.getComment(szTemp))
            printf("GIF Comment: %s\n", szTemp);
        gif.close();
    }
    return 0;
}
