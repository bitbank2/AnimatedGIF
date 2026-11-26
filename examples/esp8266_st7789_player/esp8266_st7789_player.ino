// ESP8266 + 240x240 ST7789: non-blocking GIF player from LittleFS (patterns from the smalltv-pomodoro firmware).
//
// - playFrame(false, &delayMs) keeps loop() free for Wi-Fi and a web server between frames;
// - the next frame is scheduled from the time the current one was DUE, not from when drawing finished. Scheduling
//   from millis() after the draw adds the draw time to every frame: a 35 ms full-screen frame with a 40 ms delay
//   would play every 75 ms (~53% speed). When drawing falls behind, it does not catch up in bursts and still leaves
//   a few milliseconds for the rest of loop();
// - small GIFs are scaled by an integer factor to fill the screen, and lossy-compression noise can be cleaned
//   (see gif_renderer.h).
//
// Upload a GIF to LittleFS as /anim.gif (e.g. with the LittleFS upload tool). Adjust the pins for your board.

#include <Arduino.h>
#include <LittleFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <AnimatedGIF.h>
#include "gif_renderer.h"

#define TFT_CS -1  // many 240x240 ST7789 modules have CS tied low
#define TFT_DC 0
#define TFT_RST 2
#define GIF_FILE "/anim.gif"
#define MIN_GAP_MS 5  // always leave this much time for the rest of loop() between frames

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
AnimatedGIF gif;
GifRenderState render;
static uint16_t rowBuf[240] __attribute__((aligned(4)));
static File gifFile;
static int offX = 0, offY = 0, scale = 1;
static uint32_t nextFrameAt = 0;
static bool playing = false;

struct TftOut {
  void startWrite() { tft.startWrite(); }
  void endWrite() { tft.endWrite(); }
  void setAddrWindow(int x, int y, int w, int h) { tft.setAddrWindow(x, y, w, h); }
  void writePixelsBE(uint16_t *p, int n) { tft.writePixels(p, n, true, true); }  // big-endian: buffer untouched
  void fillRect(int x, int y, int w, int h, uint16_t c) { tft.fillRect(x, y, w, h, c); }
};

static void *gifOpen(const char *name, int32_t *size) {
  gifFile = LittleFS.open(name, "r");
  if (!gifFile) return nullptr;
  *size = gifFile.size();
  return &gifFile;
}
static void gifClose(void *handle) { ((File *)handle)->close(); }
static int32_t gifRead(GIFFILE *f, uint8_t *buf, int32_t len) {
  File *fl = (File *)f->fHandle;
  int32_t n = fl->read(buf, len);
  f->iPos = fl->position();
  return n;
}
static int32_t gifSeek(GIFFILE *f, int32_t pos) {
  File *fl = (File *)f->fHandle;
  fl->seek(pos);
  f->iPos = fl->position();
  return f->iPos;
}
static void gifDraw(GIFDRAW *p) {
  TftOut out;
  gifRenderDraw(render, out, rowBuf, offX, offY, scale, p);
}

static bool startGif() {
  gifRenderAlloc(render);  // before the decoder's per-frame buffers, so it does not split the heap
  gif.begin(GIF_PALETTE_RGB565_BE);
  if (!gif.open(GIF_FILE, gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
    gifRenderFree(render);
    return false;
  }
  int w = gif.getCanvasWidth(), h = gif.getCanvasHeight();
  scale = min(240 / max(w, 1), 240 / max(h, 1));
  if (scale < 2) scale = 1;
  offX = max(0, (240 - w * scale) / 2);
  offY = max(0, (240 - h * scale) / 2);
  tft.fillScreen(ST77XX_BLACK);
  nextFrameAt = millis();
  return true;
}

void setup() {
  Serial.begin(115200);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);  // 80 MHz is out of spec for most ST7789 modules
  tft.fillScreen(ST77XX_BLACK);
  playing = LittleFS.begin() && startGif();
  Serial.println(playing ? F("playing " GIF_FILE) : F("could not open " GIF_FILE));
}

void loop() {
  // ... Wi-Fi, web server and the rest of your loop run here ...
  if (!playing || (int32_t)(millis() - nextFrameAt) < 0) return;
  int delayMs = 0;
  int r = gif.playFrame(false, &delayMs);
  if (r < 0) {  // decode error: stop
    gif.close();
    gifRenderFree(render);
    playing = false;
    return;
  }
  if (r == 0) gif.reset();  // last frame: loop
  uint32_t due = nextFrameAt + (uint32_t)max(delayMs, 20);
  uint32_t earliest = millis() + MIN_GAP_MS;
  nextFrameAt = (int32_t)(due - earliest) < 0 ? earliest : due;
}
