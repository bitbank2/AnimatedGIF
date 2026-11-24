#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <random>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef __LINUX__
#define __LINUX__
#endif

#include "../src/AnimatedGIF.cpp"

static volatile uint64_t g_sink = 0;
static void fuzzDraw(GIFDRAW *p) {
    if (p && p->pPixels && p->iWidth > 0) {
        g_sink += p->pPixels[0] + p->pPixels[p->iWidth - 1];
    }
}

struct MemFile {
    const uint8_t *data;
    int32_t size;
    int32_t pos;
    int32_t chunk;
};
static MemFile g_in;

static void *fuzzOpen(const char *, int32_t *sz) {
    *sz = g_in.size;
    g_in.pos = 0;
    return &g_in;
}
static void fuzzClose(void *) {}
static int32_t fuzzRead(GIFFILE *f, uint8_t *buf, int32_t len) {
    MemFile *m = (MemFile *)f->fHandle;
    if (!m || len < 0 || m->pos < 0 || m->pos > m->size) return 0;
    int32_t n = len;
    if (n > m->size - m->pos) n = m->size - m->pos;
    if (m->chunk > 0 && n > m->chunk) n = m->chunk;
    if (n > 0) memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    f->iPos = m->pos;
    return n;
}
static int32_t fuzzSeek(GIFFILE *f, int32_t pos) {
    MemFile *m = (MemFile *)f->fHandle;
    if (!m) return 0;
    if (pos < 0) pos = 0;
    if (pos >= m->size) pos = (m->size > 0) ? (m->size - 1) : 0;
    m->pos = pos;
    f->iPos = pos;
    return pos;
}

static void fuzzOne(const uint8_t *data, int n, int mode) {
    static AnimatedGIF gif;
    gif.begin(GIF_PALETTE_RGB565_LE);
    int ok = 0;
    if (mode == 0) {
        ok = gif.open((uint8_t *)data, n, fuzzDraw);
    } else {
        g_in = {data, (int32_t)n, 0, (mode == 2) ? 1 : 0};
        ok = gif.open("fuzz.gif", fuzzOpen, fuzzClose, fuzzRead, fuzzSeek, fuzzDraw);
    }
    if (!ok) {
        gif.close();
        return;
    }

    if (mode == 3) {
        GIFINFO info{};
        gif.getInfo(&info);
        gif.close();
        return;
    }

#if defined(GIF_ENABLE_COOPERATIVE) && GIF_ENABLE_COOPERATIVE
    if (mode == 4) {
        int frames = 0;
        while (frames < 100) {
            if (!gif.beginFrame(nullptr)) break;
            int st = GIF_DECODE_MORE;
            int steps = 0;
            while (!gif.frameComplete() && steps++ < 50000) {
                st = gif.decodeLines(3);
                if (st != GIF_DECODE_MORE) break;
            }
            if (st == GIF_DECODE_ERR || st == GIF_DECODE_END) break;
            frames++;
        }
        gif.close();
        return;
    }
#endif

    int frames = 0, delay = 0;
    while (frames < 100) {
        int r = gif.playFrame(false, &delay);
        if (r <= 0) break;
        frames++;
    }
    gif.close();
}

static std::vector<uint8_t> load(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    std::vector<uint8_t> v;
    if (!f) return v;
    int c;
    while ((c = fgetc(f)) != EOF) v.push_back((uint8_t)c);
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <corpus_dir> <cases> [mode: 0=ram, 1=cb, 2=1byte, 3=info, 4=coop, -1=mix]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    long cases = atol(argv[2]);
    int fixedMode = (argc >= 4) ? atoi(argv[3]) : -1;

    std::vector<std::vector<uint8_t>> seeds;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            if (strstr(e->d_name, ".gif")) {
                auto v = load(dir + "/" + e->d_name);
                if (!v.empty()) seeds.push_back(std::move(v));
            }
        }
        closedir(d);
    }
    if (seeds.empty()) {
        fprintf(stderr, "No seeds in %s\n", dir.c_str());
        return 2;
    }

    std::mt19937 rng(0xC0FFEE12U);
    const uint8_t INTERESTING[] = {0, 1, 2, 7, 8, 11, 12, 13, 0x21, 0x2C, 0x3B, 0x7F, 0x80, 0xF9, 0xFE, 0xFF};
    for (long c = 0; c < cases; c++) {
        std::vector<uint8_t> v = seeds[rng() % seeds.size()];
        if (v.size() > 8192) v.resize(13 + rng() % 8192);
        int muts = 1 + rng() % 8;
        for (int m = 0; m < muts && !v.empty(); m++) {
            size_t i = rng() % v.size();
            switch (rng() % 7) {
                case 0: v[i] ^= 1U << (rng() % 8); break;
                case 1: v[i] = INTERESTING[rng() % sizeof(INTERESTING)]; break;
                case 2: v[i] = (uint8_t)rng(); break;
                case 3: if (i + 1 < v.size()) { v[i] = (uint8_t)rng(); v[i + 1] = (uint8_t)rng(); } break;
                case 4: v.resize(i + 1); break;
                case 5: {
                    size_t l = 1 + rng() % 32;
                    if (i + l <= v.size() && v.size() < 16384) {
                        std::vector<uint8_t> cp(v.begin() + i, v.begin() + i + l);
                        v.insert(v.begin() + i, cp.begin(), cp.end());
                    }
                    break;
                }
                case 6: {
                    size_t l = 1 + rng() % 32;
                    if (i + l <= v.size()) v.erase(v.begin() + i, v.begin() + i + l);
                    break;
                }
            }
        }
        int mode = fixedMode;
        if (mode < 0) {
#if defined(GIF_ENABLE_COOPERATIVE) && GIF_ENABLE_COOPERATIVE
            mode = (int)(rng() % 5);
#else
            mode = (int)(rng() % 4);
#endif
        }
        alarm(3);
        fuzzOne(v.data(), (int)v.size(), mode);
        alarm(0);
    }
    printf("fuzz: %ld cases OK (mode=%d, sizeof(GIFIMAGE)=%zu)\n", cases, fixedMode, sizeof(GIFIMAGE));
    return 0;
}
