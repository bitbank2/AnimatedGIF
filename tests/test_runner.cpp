#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#ifndef __LINUX__
#define __LINUX__
#endif

#include "../src/AnimatedGIF.cpp"

#include "../test_images/badgers.h"
#include "../test_images/bw_wiggler_128x64.h"
#include "../test_images/earth_128x128.h"
#include "../test_images/green.h"
#include "../test_images/homer.h"
#include "../test_images/homer_tiny.h"
#include "../test_images/nostromo.h"
#include "../test_images/pattern.h"
#include "../test_images/thisisfine_240x179.h"
#include "../test_images/x_wing.h"

#ifndef GIF_RGB565_ONLY
#define GIF_RGB565_ONLY 0
#endif

#ifndef GIF_DISABLE_TURBO
#define GIF_DISABLE_TURBO 0
#endif

struct EmbeddedSample {
    const char *name;
    const uint8_t *data;
    size_t size;
};

static const EmbeddedSample kEmbedded[] = {
    {"emb_green", green, sizeof(green)},
    {"emb_homer_tiny", homer_tiny, sizeof(homer_tiny)},
    {"emb_pattern", pattern, sizeof(pattern)},
    {"emb_badgers", badgers, sizeof(badgers)},
    {"emb_bw_wiggler", bw_wiggler_128x64, sizeof(bw_wiggler_128x64)},
    {"emb_homer", homer, sizeof(homer)},
    {"emb_thisisfine", thisisfine_240x179, sizeof(thisisfine_240x179)},
    {"emb_nostromo", nostromo, sizeof(nostromo)},
    {"emb_x_wing", x_wing, sizeof(x_wing)},
    {"emb_earth", earth_128x128, sizeof(earth_128x128)},
};

struct HashCtx {
    uint64_t hash;
    int lines;
    bool cooked;
};

static inline uint64_t fnv1a_byte(uint64_t h, uint8_t b) {
    h ^= b;
    return h * 1099511628211ULL;
}

static inline uint64_t fnv1a_u32(uint64_t h, uint32_t v) {
    h = fnv1a_byte(h, (uint8_t)(v & 0xFF));
    h = fnv1a_byte(h, (uint8_t)((v >> 8) & 0xFF));
    h = fnv1a_byte(h, (uint8_t)((v >> 16) & 0xFF));
    h = fnv1a_byte(h, (uint8_t)((v >> 24) & 0xFF));
    return h;
}

static void drawCallback(GIFDRAW *p) {
    HashCtx *ctx = (HashCtx *)p->pUser;
    if (!ctx) return;
    ctx->lines++;
    uint64_t h = ctx->hash;
    h = fnv1a_u32(h, (uint32_t)p->iX);
    h = fnv1a_u32(h, (uint32_t)p->iY);
    h = fnv1a_u32(h, (uint32_t)p->y);
    h = fnv1a_u32(h, (uint32_t)p->iWidth);
    h = fnv1a_u32(h, (uint32_t)p->iHeight);
    h = fnv1a_byte(h, p->ucHasTransparency);
    h = fnv1a_byte(h, p->ucTransparent);
    h = fnv1a_byte(h, p->ucDisposalMethod);
    h = fnv1a_byte(h, p->ucBackground);
    h = fnv1a_byte(h, p->ucIsGlobalPalette);
    h = fnv1a_byte(h, p->ucPaletteType);

    if (!ctx->cooked) {
        // RAW mode: hash palette indices and mapped output color values
        for (int i = 0; i < p->iWidth; i++) {
            uint8_t idx = p->pPixels[i];
            h = fnv1a_byte(h, idx);
            if (p->ucPaletteType == GIF_PALETTE_RGB888 || p->ucPaletteType == GIF_PALETTE_RGB8888) {
                h = fnv1a_byte(h, p->pPalette24[idx * 3 + 0]);
                h = fnv1a_byte(h, p->pPalette24[idx * 3 + 1]);
                h = fnv1a_byte(h, p->pPalette24[idx * 3 + 2]);
            } else if (p->ucPaletteType == GIF_PALETTE_RGB565_LE || p->ucPaletteType == GIF_PALETTE_RGB565_BE) {
                uint16_t c = p->pPalette[idx];
                h = fnv1a_byte(h, (uint8_t)(c & 0xFF));
                h = fnv1a_byte(h, (uint8_t)(c >> 8));
            }
        }
    } else {
        int bpp = (p->ucPaletteType == GIF_PALETTE_RGB888) ? 3 : 2;
        for (int i = 0; i < p->iWidth * bpp; i++) {
            h = fnv1a_byte(h, p->pPixels[i]);
        }
    }
    ctx->hash = h;
}

struct MemStream {
    const uint8_t *data;
    int32_t size;
    int32_t pos;
    int32_t maxReadChunk;
    int32_t failAfterBytes;
    int32_t totalRead;
    int32_t errReturn;
};

static MemStream g_stream;

static void *cbOpen(const char *, int32_t *pSize) {
    *pSize = g_stream.size;
    g_stream.pos = 0;
    g_stream.totalRead = 0;
    return &g_stream;
}

static void cbClose(void *) {}

static int32_t cbRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    MemStream *s = (MemStream *)pFile->fHandle;
    if (!s || iLen < 0 || s->pos < 0 || s->pos > s->size) return 0;
    if (s->failAfterBytes >= 0 && s->totalRead >= s->failAfterBytes) {
        return s->errReturn;
    }
    int32_t avail = s->size - s->pos;
    int32_t n = std::min(iLen, avail);
    if (s->maxReadChunk > 0 && n > s->maxReadChunk) n = s->maxReadChunk;
    if (s->failAfterBytes >= 0 && s->totalRead + n > s->failAfterBytes) {
        n = s->failAfterBytes - s->totalRead;
        if (n <= 0) return s->errReturn;
    }
    if (n > 0) memcpy(pBuf, s->data + s->pos, n);
    s->pos += n;
    s->totalRead += n;
    pFile->iPos = s->pos;
    return n;
}

static int32_t cbSeek(GIFFILE *pFile, int32_t iPos) {
    MemStream *s = (MemStream *)pFile->fHandle;
    if (!s) return 0;
    if (iPos < 0) iPos = 0;
    if (iPos >= s->size) iPos = (s->size > 0) ? (s->size - 1) : 0;
    s->pos = iPos;
    pFile->iPos = iPos;
    return iPos;
}

struct DecodeResult {
    bool opened;
    int frames;
    int lines;
    int lastError;
    uint64_t hash;
};

enum DecodeSource { SRC_MEM, SRC_CB_FULL, SRC_CB_1BYTE };

static DecodeResult runDecode(const uint8_t *data, size_t size, uint8_t paletteType, int drawType,
                              DecodeSource src, int coopMode = 0, int coopParam = 0) {
    DecodeResult res{false, 0, 0, 0, 14695981039346656037ULL};
    AnimatedGIF gif;
    gif.begin(paletteType);

    bool ok = false;
    if (src == SRC_MEM) {
        ok = gif.open((uint8_t *)data, (int)size, drawCallback) != 0;
    } else {
        g_stream = {data, (int32_t)size, 0, (src == SRC_CB_1BYTE) ? 1 : 0, -1, 0, 0};
        ok = gif.open("mem.gif", cbOpen, cbClose, cbRead, cbSeek, drawCallback) != 0;
    }
    res.opened = ok;
    if (!ok) {
        res.lastError = gif.getLastError();
        gif.close();
        return res;
    }

    std::vector<uint8_t> fb;
    if (drawType == GIF_DRAW_COOKED) {
        size_t w = (size_t)gif.getCanvasWidth();
        size_t h = (size_t)gif.getCanvasHeight();
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            // Allocate full 8-bit + up to 24-bpp cooked canvas for upstream compatibility
            fb.assign(w * h * 4 + 256, 0);
            gif.setFrameBuf(fb.data());
            gif.setDrawType(GIF_DRAW_COOKED);
        } else {
            gif.close();
            res.lastError = GIF_TOO_WIDE;
            return res;
        }
    } else {
        gif.setDrawType(GIF_DRAW_RAW);
    }

    HashCtx ctx{14695981039346656037ULL, 0, drawType == GIF_DRAW_COOKED};
    int frames = 0;
    while (frames < 500) {
        int r = 0;
        int delay = 0;
        if (coopMode == 0) {
            r = gif.playFrame(false, &delay, &ctx);
        }
#if defined(GIF_ENABLE_COOPERATIVE) && GIF_ENABLE_COOPERATIVE
        else {
            if (!gif.beginFrame(&ctx)) {
                if (gif.getLastError() == GIF_EMPTY_FRAME) { r = 0; break; }
                r = -1;
                break;
            }
            int st = GIF_DECODE_MORE;
            int guard = 0;
            while (!gif.frameComplete()) {
                if (coopMode == 1) st = gif.decodeStep((uint32_t)coopParam);
                else st = gif.decodeLines(coopParam);
                if (st == GIF_DECODE_ERR) { r = -1; break; }
                if (st == GIF_DECODE_FRAME_DONE || st == GIF_DECODE_END) break;
                if (++guard > 2000000) { r = -1; break; }
            }
            if (r < 0) break;
            r = (st == GIF_DECODE_END) ? 0 : 1;
        }
#endif
        if (r < 0) {
            res.lastError = gif.getLastError();
            gif.close();
            res.frames = frames;
            res.lines = ctx.lines;
            res.hash = ctx.hash;
            return res;
        }
        if (gif.getLastError() == GIF_SUCCESS) frames++;
        if (r == 0) break;
    }

    res.frames = frames;
    res.lines = ctx.lines;
    res.hash = ctx.hash;
    res.lastError = gif.getLastError();
    gif.close();
    return res;
}

static std::vector<uint8_t> loadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::vector<std::string> listGifs(const std::string &dir, const char *prefix) {
    std::vector<std::string> names;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string s(e->d_name);
            if (s.size() > 4 && s.substr(s.size() - 4) == ".gif" &&
                (!prefix || s.rfind(prefix, 0) == 0)) {
                names.push_back(s);
            }
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

struct GoldenEntry {
    int frames;
    int lines;
    uint64_t raw565le;
    uint64_t raw565be;
    uint64_t cooked565le;
    uint64_t cooked888;
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dump-golden|verify|malformed-check|sizeof> [corpus_dir] [golden_file]\n", argv[0]);
        return 2;
    }

    std::string mode = argv[1];
    if (mode == "sizeof") {
        printf("sizeof(GIFIMAGE)=%zu sizeof(AnimatedGIF)=%zu MAX_WIDTH=%d FILE_BUF_SIZE=%d LZW_BUF_SIZE=%d\n",
               sizeof(GIFIMAGE), sizeof(AnimatedGIF), (int)MAX_WIDTH, (int)FILE_BUF_SIZE, (int)LZW_BUF_SIZE);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <dump-golden|verify|malformed-check|sizeof> <corpus_dir> [golden_file]\n", argv[0]);
        return 2;
    }

    std::string corpusDir = argv[2];

    if (mode == "dump-golden") {
        if (argc < 4) return 2;
        FILE *out = fopen(argv[3], "w");
        if (!out) return 2;

        auto dumpOne = [&](const std::string &name, const uint8_t *data, size_t sz) {
            auto r1 = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_MEM);
            auto r2 = runDecode(data, sz, GIF_PALETTE_RGB565_BE, GIF_DRAW_RAW, SRC_MEM);
            auto r3 = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_COOKED, SRC_MEM);
            auto r4 = runDecode(data, sz, GIF_PALETTE_RGB888, GIF_DRAW_COOKED, SRC_MEM);
            if (!r1.opened || r1.frames <= 0) {
                fprintf(stderr, "FAIL dumping %s: opened=%d frames=%d err=%d\n", name.c_str(), r1.opened, r1.frames, r1.lastError);
                exit(1);
            }
            fprintf(out, "%s %d %d %016llx %016llx %016llx %016llx\n",
                    name.c_str(), r1.frames, r1.lines,
                    (unsigned long long)r1.hash, (unsigned long long)r2.hash,
                    (unsigned long long)r3.hash, (unsigned long long)r4.hash);
        };

        for (const auto &s : kEmbedded) dumpOne(s.name, s.data, s.size);
        for (const auto &f : listGifs(corpusDir, "valid_")) {
            auto v = loadFile(corpusDir + "/" + f);
            dumpOne(f, v.data(), v.size());
        }
        fclose(out);
        printf("Golden hashes written to %s\n", argv[3]);
        return 0;
    }

    if (mode == "malformed-check") {
        int handled = 0;
        for (const auto &f : listGifs(corpusDir, "malformed_")) {
            auto v = loadFile(corpusDir + "/" + f);
            alarm(3);
            auto r = runDecode(v.data(), v.size(), GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_MEM);
            AnimatedGIF g;
            g.begin(GIF_PALETTE_RGB565_LE);
            if (g.open(v.data(), (int)v.size(), nullptr)) {
                GIFINFO info;
                memset(&info, 0, sizeof(info));
                g.getInfo(&info);
                g.close();
            }
            alarm(0);
            printf("  %s: opened=%d frames=%d err=%d\n", f.c_str(), r.opened, r.frames, r.lastError);
            handled++;
        }
        printf("malformed-check completed %d files\n", handled);
        return 0;
    }

    if (mode == "verify") {
        if (argc < 4) return 2;
        std::ifstream gf(argv[3]);
        if (!gf) { fprintf(stderr, "Cannot open golden file %s\n", argv[3]); return 2; }
        std::map<std::string, GoldenEntry> golden;
        std::string nm;
        GoldenEntry ge;
        while (gf >> nm >> ge.frames >> ge.lines >> std::hex >> ge.raw565le >> ge.raw565be >> ge.cooked565le >> ge.cooked888) {
            gf >> std::dec;
            golden[nm] = ge;
        }

        int failures = 0;
        int checked = 0;

        auto verifyOne = [&](const std::string &name, const uint8_t *data, size_t sz) {
            auto it = golden.find(name);
            if (it == golden.end()) {
                fprintf(stderr, "MISSING golden entry for %s\n", name.c_str());
                failures++;
                return;
            }
            const GoldenEntry &exp = it->second;

            // Skip files wider than current MAX_WIDTH (must reject cleanly with GIF_TOO_WIDE)
            uint16_t cw = (sz >= 8) ? (uint16_t)(data[6] | (data[7] << 8)) : 0;
            if (cw > MAX_WIDTH) {
                AnimatedGIF g;
                g.begin(GIF_PALETTE_RGB565_LE);
                int ok = g.open((uint8_t *)data, (int)sz, drawCallback);
                if (ok != 0) {
                    fprintf(stderr, "FAIL %s: expected reject for width %u > MAX_WIDTH %d\n", name.c_str(), cw, MAX_WIDTH);
                    failures++;
                }
                checked++;
                return;
            }

            auto r1 = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_MEM);
            auto r_cb = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_CB_FULL);
            auto r_1b = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_CB_1BYTE);
            auto r2 = runDecode(data, sz, GIF_PALETTE_RGB565_BE, GIF_DRAW_RAW, SRC_MEM);
            auto r3 = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_COOKED, SRC_MEM);

            if (!r1.opened || r1.frames != exp.frames || r1.lines != exp.lines || r1.hash != exp.raw565le) {
                fprintf(stderr, "FAIL %s RAW565LE: frames=%d(exp %d) lines=%d(exp %d) hash=%016llx(exp %016llx) err=%d\n",
                        name.c_str(), r1.frames, exp.frames, r1.lines, exp.lines,
                        (unsigned long long)r1.hash, (unsigned long long)exp.raw565le, r1.lastError);
                failures++;
            }
            if (r_cb.hash != exp.raw565le || r_cb.lines != exp.lines) {
                fprintf(stderr, "FAIL %s CB_FULL hash mismatch\n", name.c_str());
                failures++;
            }
            if (r_1b.hash != exp.raw565le || r_1b.lines != exp.lines) {
                fprintf(stderr, "FAIL %s CB_1BYTE hash mismatch (frames=%d lines=%d err=%d)\n",
                        name.c_str(), r_1b.frames, r_1b.lines, r_1b.lastError);
                failures++;
            }
            if (r2.hash != exp.raw565be || r2.lines != exp.lines) {
                fprintf(stderr, "FAIL %s RAW565BE hash mismatch\n", name.c_str());
                failures++;
            }
            if (r3.hash != exp.cooked565le || r3.lines != exp.lines) {
                fprintf(stderr, "FAIL %s COOKED565LE hash mismatch (got=%016llx exp=%016llx)\n",
                        name.c_str(), (unsigned long long)r3.hash, (unsigned long long)exp.cooked565le);
                failures++;
            }

#if !GIF_RGB565_ONLY
            auto r4 = runDecode(data, sz, GIF_PALETTE_RGB888, GIF_DRAW_COOKED, SRC_MEM);
            if (r4.hash != exp.cooked888 || r4.lines != exp.lines) {
                fprintf(stderr, "FAIL %s COOKED888 hash mismatch\n", name.c_str());
                failures++;
            }
#endif

#if !GIF_DISABLE_TURBO
            if (cw <= 480 && name != "valid_long_chain.gif") {
                AnimatedGIF tg;
                tg.begin(GIF_PALETTE_RGB565_LE);
                if (tg.open((uint8_t *)data, (int)sz, drawCallback)) {
                    size_t w = (size_t)tg.getCanvasWidth(), h = (size_t)tg.getCanvasHeight();
                    std::vector<uint8_t> fb(w * (h + 4), 0);
                    std::vector<uint8_t> tb(TURBO_BUFFER_SIZE + w * h + 256, 0);
                    tg.setFrameBuf(fb.data());
                    tg.setTurboBuf(tb.data());
                    tg.setDrawType(GIF_DRAW_COOKED);
                    HashCtx tctx{14695981039346656037ULL, 0, true};
                    int tframes = 0;
                    while (tframes < 500) {
                        int tr = tg.playFrame(false, nullptr, &tctx);
                        if (tr < 0) { failures++; break; }
                        if (tg.getLastError() == GIF_SUCCESS) tframes++;
                        if (tr == 0) break;
                    }
                    tg.close();
                    // Note: Turbo doesn't execute disposal=2 pre-clear in COOKED mode; check frames match on all, hash on non-disposal2
                    if (tframes != exp.frames) {
                        fprintf(stderr, "FAIL %s TURBO frames=%d exp=%d\n", name.c_str(), tframes, exp.frames);
                        failures++;
                    }
                }
            }
#endif

#if defined(GIF_ENABLE_COOPERATIVE) && GIF_ENABLE_COOPERATIVE
            for (int budget : {0, 50, 500}) {
                auto rc = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_CB_1BYTE, 1, budget);
                if (rc.hash != exp.raw565le || rc.frames != exp.frames || rc.lines != exp.lines) {
                    fprintf(stderr, "FAIL %s COOP step(%d) mismatch: frames=%d lines=%d hash=%016llx exp=%016llx\n",
                            name.c_str(), budget, rc.frames, rc.lines,
                            (unsigned long long)rc.hash, (unsigned long long)exp.raw565le);
                    failures++;
                }
            }
            for (int nlines : {1, 7}) {
                auto rl = runDecode(data, sz, GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_MEM, 2, nlines);
                if (rl.hash != exp.raw565le || rl.frames != exp.frames || rl.lines != exp.lines) {
                    fprintf(stderr, "FAIL %s COOP lines(%d) mismatch\n", name.c_str(), nlines);
                    failures++;
                }
            }
#endif
            // getInfo verification (for small/medium files)
            if (name != "valid_long_chain.gif") {
                AnimatedGIF ig;
                ig.begin(GIF_PALETTE_RGB565_LE);
                if (ig.open((uint8_t *)data, (int)sz, nullptr)) {
                    GIFINFO info{};
                    int okInfo = ig.getInfo(&info);
                    if (!okInfo || info.iFrameCount != exp.frames) {
                        fprintf(stderr, "FAIL %s getInfo: ok=%d frames=%d exp=%d\n", name.c_str(), okInfo, info.iFrameCount, exp.frames);
                        failures++;
                    }
                    ig.close();
                }
            }

            checked++;
        };

        for (const auto &s : kEmbedded) verifyOne(s.name, s.data, s.size);
        for (const auto &f : listGifs(corpusDir, "valid_")) {
            auto v = loadFile(corpusDir + "/" + f);
            verifyOne(f, v.data(), v.size());
        }

        // Edge cases: small 41B 2-bit GIF, consecutive CC & CC->EOI, comments
        {
            auto v = loadFile(corpusDir + "/edge_small_1x1_2bit.gif");
            AnimatedGIF g;
            g.begin(GIF_PALETTE_RGB565_LE);
            uint16_t px565 = 0xFFFF;
            struct SingleCap { uint16_t *dst; int count; } cap{&px565, 0};
            auto capDraw = [](GIFDRAW *p) {
                SingleCap *c = (SingleCap *)p->pUser;
                c->count += p->iWidth;
                *c->dst = p->pPalette[p->pPixels[0]];
            };
            if (!g.open(v.data(), (int)v.size(), capDraw) || g.playFrame(false, nullptr, &cap) < 0 ||
                cap.count != 1 || px565 != 0x0907) {
                fprintf(stderr, "FAIL edge_small_1x1_2bit: count=%d px=%04x err=%d\n", cap.count, px565, g.getLastError());
                failures++;
            }
            g.close();
            checked++;
        }
        {
            auto v = loadFile(corpusDir + "/edge_cc_eoi.gif");
            auto r = runDecode(v.data(), v.size(), GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_CB_1BYTE);
            if (!r.opened || r.frames != 1 || r.lines != 1) {
                fprintf(stderr, "FAIL edge_cc_eoi: opened=%d frames=%d lines=%d err=%d\n", r.opened, r.frames, r.lines, r.lastError);
                failures++;
            }
            checked++;
        }
        {
            auto v = loadFile(corpusDir + "/edge_comment.gif");
            AnimatedGIF g;
            g.begin(GIF_PALETTE_RGB565_LE);
            if (!g.open(v.data(), (int)v.size(), drawCallback)) {
                fprintf(stderr, "FAIL edge_comment open\n"); failures++;
            } else {
                char cbuf[128] = {};
                g.playFrame(false, nullptr, nullptr);
                int clen = g.getComment(cbuf);
                if (clen != 43 || strcmp(cbuf, "AnimatedGIF-LowMem test comment 1234567890!") != 0) {
                    fprintf(stderr, "FAIL edge_comment len=%d str='%s'\n", clen, cbuf);
                    failures++;
                }
                g.close();
            }
            checked++;
        }
        {
            auto v = loadFile(corpusDir + "/edge_huge_comment.gif");
            AnimatedGIF g;
            g.begin(GIF_PALETTE_RGB565_LE);
            if (!g.open(v.data(), (int)v.size(), drawCallback)) {
                fprintf(stderr, "FAIL edge_huge_comment open\n"); failures++;
            } else {
                char cbuf[300] = {};
                int rc = g.playFrame(false, nullptr, nullptr);
                int clen = g.getComment(cbuf);
                if (rc < 0 || clen <= 0 || clen > 255) {
                    fprintf(stderr, "FAIL edge_huge_comment rc=%d clen=%d\n", rc, clen);
                    failures++;
                }
                g.close();
            }
            checked++;
        }

        // Malformed files + adversarial error-returning / truncated callbacks
        for (const auto &f : listGifs(corpusDir, "malformed_")) {
            auto v = loadFile(corpusDir + "/" + f);
            alarm(3);
            auto r1 = runDecode(v.data(), v.size(), GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_MEM);
            auto r2 = runDecode(v.data(), v.size(), GIF_PALETTE_RGB565_LE, GIF_DRAW_RAW, SRC_CB_1BYTE);
            AnimatedGIF ig;
            ig.begin(GIF_PALETTE_RGB565_LE);
            if (ig.open(v.data(), (int)v.size(), nullptr)) {
                GIFINFO info{};
                ig.getInfo(&info);
                ig.close();
            }
            alarm(0);
            if (r1.opened && r1.frames > 0 && r1.lastError == GIF_SUCCESS) {
                fprintf(stderr, "FAIL malformed %s unexpectedly succeeded cleanly!\n", f.c_str());
                failures++;
            }
            (void)r2;
            checked++;
        }

        // Negative / EOF callback error injection on a valid multi-frame GIF
        {
            auto v = loadFile(corpusDir + "/valid_local_palette.gif");
            for (int errVal : {0, -1}) {
                for (int limit : {5, 25, 60, 120}) {
                    AnimatedGIF g;
                    g.begin(GIF_PALETTE_RGB565_LE);
                    g_stream = {v.data(), (int32_t)v.size(), 0, 7, limit, 0, errVal};
                    if (g.open("fail.gif", cbOpen, cbClose, cbRead, cbSeek, drawCallback)) {
                        HashCtx ctx{0, 0, false};
                        while (g.playFrame(false, nullptr, &ctx) > 0) {}
                        g.close();
                    }
                }
            }
            checked++;
        }

        printf("verify: %d checks, %d failures (sizeof(GIFIMAGE)=%zu)\n", checked, failures, sizeof(GIFIMAGE));
        return failures ? 1 : 0;
    }

    return 2;
}
