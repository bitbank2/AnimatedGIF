#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <AnimatedGIF.h>

static int DISPLAY_WIDTH;
static int DISPLAY_HEIGHT;

static SDL_Window *win = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *displayTexture = NULL;

static void GIFDraw(GIFDRAW *pDraw);
static void *GIFOpenFile(const char *fname, int32_t *pSize);
static void GIFCloseFile(void *pHandle);
static int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition);
static void draw_SDL(int x, int y, int w, int h, uint16_t *pixels);

static int play_gif_callbacks(const char *gif_path);

AnimatedGIF gif;

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("sdl2 gif player\nUsage: sdl2_gif <filename>\n");
    return -1;
  }

  return play_gif_callbacks(argv[1]);
}

static int play_gif_callbacks(const char *gif_path) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL Init Failed: %s\n", SDL_GetError());
    return 1;
  }

  gif.begin(LITTLE_ENDIAN_PIXELS);

  if (gif.open(gif_path, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile,
               GIFDraw)) {
    DISPLAY_WIDTH = gif.getCanvasWidth();
    DISPLAY_HEIGHT = gif.getCanvasHeight();
    printf("GIF Open: %d x %d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);

    win = SDL_CreateWindow("GIF Decoder Sim", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH,
                           DISPLAY_HEIGHT, SDL_WINDOW_SHOWN);

    renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    displayTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                       SDL_TEXTUREACCESS_STATIC, DISPLAY_WIDTH,
                                       DISPLAY_HEIGHT);

    int max_count = 5;
    SDL_Event e;

    for (int i = 0; i < max_count;) {
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) i = max_count;
      }

      int frameDelay;
      if (gif.playFrame(true, &frameDelay)) {
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, displayTexture, NULL, NULL);
        SDL_RenderPresent(renderer);

        SDL_Delay(frameDelay);
      } else {
        gif.reset();
        i++;
      }
    }
    gif.close();
  }

  SDL_DestroyTexture(displayTexture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(win);
  SDL_Quit();

  return EXIT_SUCCESS;
}

static void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *d, *usPalette;
  // Increased buffer size to prevent stack overflow on wider images
  // In a desktop env, 4096 is safe. On micro, keep it tight.
  uint16_t usTemp[4096];
  int x, y, iWidth;

  iWidth = pDraw->iWidth;
  if (iWidth > DISPLAY_WIDTH) iWidth = DISPLAY_WIDTH;

  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y;  // current line

  s = pDraw->pPixels;

  // Handle Disposal Method 2 (Restore to background)
  if (pDraw->ucDisposalMethod == 2) {
    for (x = 0; x < iWidth; x++) {
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  // Apply pixels
  if (pDraw->ucHasTransparency)  // Transparency logic
  {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int iCount;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0;

    while (x < iWidth) {
      c = ucTransparent - 1;
      d = usTemp;

      // Look for OPAQUE pixels
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) {
          s--;  // Back up
        } else {
          *d++ = usPalette[c];
          iCount++;
        }
      }

      // Render the run of opaque pixels
      if (iCount) {
        draw_SDL(pDraw->iX + x, y, iCount, 1, usTemp);
        x += iCount;
        iCount = 0;
      }

      // Look for TRANSPARENT pixels (and skip them)
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent)
          iCount++;
        else
          s--;
      }
      if (iCount) {
        x += iCount;  // Just skip x, do not draw
        iCount = 0;
      }
    }
  } else  // No transparency (Standard draw)
  {
    s = pDraw->pPixels;
    // Translate 8-bit pixels to RGB565
    for (x = 0; x < iWidth; x++) usTemp[x] = usPalette[*s++];

    draw_SDL(pDraw->iX, y, iWidth, 1, usTemp);
  }
}

static void draw_SDL(int x, int y, int w, int h, uint16_t *pixels) {
  SDL_Rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = w;
  rect.h = h;

  // We use w * 2 because RGB565 is 2 bytes per pixel
  SDL_UpdateTexture(displayTexture, &rect, pixels, w * 2);
}

static void *GIFOpenFile(const char *fname, int32_t *pSize) {
  FILE *file = fopen(fname, "r");

  if (file == NULL) {
    perror("fopen");
    return NULL;
  }

  struct stat st;

  if (fstat(fileno(file), &st) != 0) return NULL;

  if (st.st_size < 0 || st.st_size > INT32_MAX) return NULL;

  *pSize = (int32_t)st.st_size;

  return (void *)file;
}

static void GIFCloseFile(void *pHandle) {
  FILE *f = static_cast<FILE *>(pHandle);

  if (fclose(f) != 0) {
    perror("fclose");
  }
}

static int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;

  FILE *f = static_cast<FILE *>(pFile->fHandle);

  // Note: If you read a file all the way to the last byte, seek() stops working
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos - 1;  // <-- ugly work-around
  if (iBytesRead <= 0) return 0;

  size_t n = fread(pBuf, 1, (size_t)iBytesRead, f);

  if (n == 0 && ferror(f)) {
    return -1;
  }

  long pos = ftell(f);

  if (pos < 0 || pos > INT32_MAX) return -1;

  pFile->iPos = (int32_t)pos;

  return (int32_t)n;
}

static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  FILE *f = static_cast<FILE *>(pFile->fHandle);

  fseek(f, (long)iPosition, SEEK_SET);

  long pos = ftell(f);

  if (pos < 0 || pos > INT32_MAX) return -1;

  pFile->iPos = (int32_t)pos;

  return pFile->iPos;
}
