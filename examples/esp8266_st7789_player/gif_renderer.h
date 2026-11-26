// gif_renderer.h — draw callback helpers for a 240x240 ST7789 on ESP8266 (from the smalltv-pomodoro firmware).
//
// What it does on top of a plain GIFDraw callback:
//  - one SPI address window per frame when the frame has no transparency (rows stream into it);
//  - integer upscaling for small GIFs (80x80 -> 3x), computed once per row and sent `scale` times;
//  - optional cleanup of lossy-compression noise: palette entries with R, G and B <= 12 become black, and a pixel
//    that is very different from both horizontal neighbours (which are similar to each other) takes the left
//    neighbour's colour. The filter only runs when the frame's palette has those near-black entries (the usual
//    trace of lossy GIF encoders), so clean pixel art is drawn exactly as stored.
//
// The filter compares colours through a 768-byte R,G,B table built once per frame. Allocate it when the GIF is opened
// (before the per-frame decoder buffers) and free it on close; without it, frames are drawn unfiltered.
#pragma once

#include <stdlib.h>
#include <AnimatedGIF.h>

#ifndef GIF_CLEAN_PALETTE
#define GIF_CLEAN_PALETTE 1
#endif
#ifndef GIF_ISOLATED_PIXEL_FILTER
#define GIF_ISOLATED_PIXEL_FILTER 1
#endif

static constexpr uint8_t GIF_NEAR_BLACK = 12;

struct GifRenderState {
  uint8_t prevDisp = 0;
  bool clearTrans = false;
  int16_t prevX = 0, prevY = 0, prevW = 0, prevH = 0;
  int16_t oldX0 = 0, oldY0 = 0, oldX1 = 0, oldY1 = 0;
  bool writing = false;
  bool blockWindow = false;
  int rowsSeen = 0;
  int expectedY = 0;       // interlaced GIFs deliver rows out of order: then one window per row
  int nearBlack = 0;       // near-black palette entries in this frame: turns the isolated-pixel filter on
  uint8_t *rgb = nullptr;  // 256 x (R, G, B) 8-bit, owned by the caller (see gifRenderAlloc/gifRenderFree)
};

static inline bool gifRenderAlloc(GifRenderState &st) {
  if (!st.rgb) st.rgb = (uint8_t *)malloc(256 * 3);
  return st.rgb != nullptr;
}
static inline void gifRenderFree(GifRenderState &st) {
  free(st.rgb);
  st.rgb = nullptr;
}

static inline int gifClip240(int v) { return v < 0 ? 0 : v > 240 ? 240 : v; }
static inline uint16_t gifSwap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint8_t gifExpand5(uint16_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t gifExpand6(uint16_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

// One pass over the (big-endian RGB565) palette per frame: count and clear near-black entries and fill the R,G,B
// table with the cleaned colours. Returns how many near-black entries there were.
static int gifPreparePalette(uint16_t *palette, uint8_t *rgb) {
  int nearBlack = 0;
  for (int i = 0; i < 256; i++) {
    uint16_t v = gifSwap16(palette[i]);
    uint8_t r = gifExpand5((v >> 11) & 31), g = gifExpand6((v >> 5) & 63), b = gifExpand5(v & 31);
    if (palette[i] && r <= GIF_NEAR_BLACK && g <= GIF_NEAR_BLACK && b <= GIF_NEAR_BLACK) {
      nearBlack++;
#if GIF_CLEAN_PALETTE
      palette[i] = 0;
      r = g = b = 0;
#endif
    }
    if (rgb) { rgb[3 * i] = r; rgb[3 * i + 1] = g; rgb[3 * i + 2] = b; }
  }
  return nearBlack;
}

static inline int gifAbs(int v) { return v < 0 ? -v : v; }
static inline bool gifFar(const uint8_t *a, const uint8_t *b) {
  return gifAbs(a[0] - b[0]) >= 64 || gifAbs(a[1] - b[1]) >= 64 || gifAbs(a[2] - b[2]) >= 64;
}
static inline bool gifNear(const uint8_t *a, const uint8_t *b) {
  return gifAbs(a[0] - b[0]) <= 16 && gifAbs(a[1] - b[1]) <= 16 && gifAbs(a[2] - b[2]) <= 16;
}

// Opaque row with the isolated-pixel filter. Fast path: a pixel whose index equals a neighbour's has the same colour
// and needs no check (most pixels); only pixels that differ from both neighbours look at the table.
static inline int gifRowFiltered(uint16_t *buf, const uint16_t *pal, const uint8_t *rgb, const uint8_t *row,
                                 int width, int src0, int src1, int scale) {
  int n = 0;
  for (int x = src0; x < src1; x++) {
    uint8_t c = row[x];
    uint16_t color = pal[c];
#if GIF_ISOLATED_PIXEL_FILTER
    if (rgb && x > 0 && x + 1 < width) {
      uint8_t l = row[x - 1], r = row[x + 1];
      if (c != l && c != r) {
        const uint8_t *C = rgb + 3 * c, *L = rgb + 3 * l, *R = rgb + 3 * r;
        if ((l == r || gifNear(L, R)) && gifFar(C, L) && gifFar(C, R)) color = pal[l];
      }
    }
#else
    (void)rgb; (void)width;
#endif
    for (int k = 0; k < scale; k++) buf[n++] = color;
  }
  return n;
}

template <class Display>
static inline void gifRowDone(GifRenderState &st, Display &out, GIFDRAW *p) {
  if (st.writing && st.rowsSeen >= p->iHeight) {
    out.endWrite();
    st.writing = false;
  }
}

// Call from your GIFDraw callback. `buf` holds at least 240 pixels. Display needs startWrite(), endWrite(),
// setAddrWindow(x, y, w, h), writePixelsBE(uint16_t *, n) (big-endian, must not modify the buffer) and
// fillRect(x, y, w, h, colour). Use GIF_PALETTE_RGB565_BE.
template <class Display>
static void gifRenderDraw(GifRenderState &st, Display &out, uint16_t *buf, int offX, int offY, int scale, GIFDRAW *p) {
  if (p->y == 0) {
    if (st.writing) {
      out.endWrite();
      st.writing = false;
    }
    int fx0 = gifClip240(p->iX * scale + offX), fy0 = gifClip240(p->iY * scale + offY);
    int fx1 = gifClip240((p->iX + p->iWidth) * scale + offX), fy1 = gifClip240((p->iY + p->iHeight) * scale + offY);
    st.clearTrans = st.prevDisp == 2 && st.prevW > 0 && st.prevH > 0;
    if (st.clearTrans) {  // disposal 2: clear the previous frame's area to black, as browsers do
      st.oldX0 = st.prevX; st.oldY0 = st.prevY;
      st.oldX1 = st.prevX + st.prevW; st.oldY1 = st.prevY + st.prevH;
      if (st.oldX1 <= fx0 || fx1 <= st.oldX0 || st.oldY1 <= fy0 || fy1 <= st.oldY0) {
        out.fillRect(st.oldX0, st.oldY0, st.oldX1 - st.oldX0, st.oldY1 - st.oldY0, 0);
      } else {
        int cy0 = st.oldY0 > fy0 ? st.oldY0 : fy0, cy1 = st.oldY1 < fy1 ? st.oldY1 : fy1;
        if (st.oldY0 < cy0) out.fillRect(st.oldX0, st.oldY0, st.oldX1 - st.oldX0, cy0 - st.oldY0, 0);
        if (cy1 < st.oldY1) out.fillRect(st.oldX0, cy1, st.oldX1 - st.oldX0, st.oldY1 - cy1, 0);
        if (cy0 < cy1 && st.oldX0 < fx0) out.fillRect(st.oldX0, cy0, fx0 - st.oldX0, cy1 - cy0, 0);
        if (cy0 < cy1 && fx1 < st.oldX1) out.fillRect(fx1, cy0, st.oldX1 - fx1, cy1 - cy0, 0);
      }
    }
    st.prevDisp = p->ucDisposalMethod;
    st.prevX = (int16_t)fx0; st.prevY = (int16_t)fy0;
    st.prevW = (int16_t)(fx1 > fx0 ? fx1 - fx0 : 0);
    st.prevH = (int16_t)(fy1 > fy0 ? fy1 - fy0 : 0);
    st.rowsSeen = 0;
    st.nearBlack = gifPreparePalette(p->pPalette, st.rgb);
    st.blockWindow = !p->ucHasTransparency && fx1 > fx0 && fy1 > fy0;
    st.expectedY = 0;
    if (fx1 > fx0 && fy1 > fy0) {
      out.startWrite();
      st.writing = true;
      if (st.blockWindow) out.setAddrWindow(fx0, fy0, fx1 - fx0, fy1 - fy0);
    }
  }

  st.rowsSeen++;
  int frameX = p->iX * scale + offX;
  int src0 = frameX < 0 ? (-frameX + scale - 1) / scale : 0;
  int src1 = p->iWidth;
  if (frameX + src1 * scale > 240) src1 = (240 - frameX) / scale;
  int x0 = frameX + src0 * scale, w = (src1 - src0) * scale;
  uint8_t *s = p->pPixels + src0;
  int y0 = (p->iY + p->y) * scale + offY;
  if (!p->ucHasTransparency) {  // opaque: build the (filtered, scaled) row once, send it `scale` times
    if (st.blockWindow && p->y != st.expectedY) st.blockWindow = false;  // interlaced: rows out of order
    st.expectedY = p->y + 1;
    if (w <= 0) return gifRowDone(st, out, p);
    gifRowFiltered(buf, p->pPalette, st.nearBlack ? st.rgb : nullptr, p->pPixels, p->iWidth, src0, src1, scale);
    for (int k = 0; k < scale; k++) {
      int y = y0 + k;
      if (y < 0 || y >= 240) continue;
      if (!st.blockWindow) out.setAddrWindow(x0, y, w, 1);
      out.writePixelsBE(buf, w);
    }
    return gifRowDone(st, out, p);
  }
  for (int k = 0; k < scale; k++) {  // transparent: draw only opaque runs (and restore a disposal-2 area)
    int y = y0 + k;
    if (w <= 0 || y < 0 || y >= 240) continue;
    uint16_t *pal = p->pPalette;
    bool rowRestore = st.clearTrans && y >= st.oldY0 && y < st.oldY1;
    uint8_t tr = p->ucTransparent;
    if (rowRestore && x0 >= st.oldX0 && x0 + w <= st.oldX1) {
      int n = 0;
      for (int i = 0; i < src1 - src0; i++) {
        uint16_t color = s[i] == tr ? 0 : pal[s[i]];
        for (int r = 0; r < scale; r++) buf[n++] = color;
      }
      out.setAddrWindow(x0, y, w, 1);
      out.writePixelsBE(buf, w);
    } else {
      auto drawPx = [&](int x) {
        int pxX = x0 + x * scale;
        return s[x] != tr || (rowRestore && pxX >= st.oldX0 && pxX < st.oldX1);
      };
      int srcW = src1 - src0, x = 0;
      while (x < srcW && !drawPx(x)) x++;
      while (x < srcW) {
        int start = x, n = 0;
        while (x < srcW && drawPx(x)) {
          uint8_t px = s[x++];
          uint16_t color = px == tr ? 0 : pal[px];
          for (int r = 0; r < scale; r++) buf[n++] = color;
        }
        out.setAddrWindow(x0 + start * scale, y, n, 1);
        out.writePixelsBE(buf, n);
        while (x < srcW && !drawPx(x)) x++;
      }
    }
  }
  gifRowDone(st, out, p);
}
