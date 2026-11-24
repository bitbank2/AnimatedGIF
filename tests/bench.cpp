#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef __LINUX__
#define __LINUX__
#endif

#include "../src/AnimatedGIF.cpp"

#include "../test_images/badgers.h"
#include "../test_images/bw_wiggler_128x64.h"
#include "../test_images/earth_128x128.h"
#include "../test_images/nostromo.h"
#include "../test_images/thisisfine_240x179.h"

static void nullDraw(GIFDRAW *p) {
    (void)p;
}

static std::vector<uint8_t> loadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(p * (double)(v.size() - 1));
    return v[idx];
}

struct BenchTarget {
    const char *name;
    const uint8_t *data;
    size_t size;
    int reps;
};

static void runTarget(const char *label, const BenchTarget &t) {
    std::vector<double> frameUs;
    int totalFrames = 0;

    // Warmup
    {
        AnimatedGIF g;
        g.begin(GIF_PALETTE_RGB565_LE);
        if (g.open((uint8_t *)t.data, (int)t.size, nullDraw)) {
            while (g.playFrame(false, nullptr) > 0) {}
            g.close();
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < t.reps; r++) {
        AnimatedGIF g;
        g.begin(GIF_PALETTE_RGB565_LE);
        if (!g.open((uint8_t *)t.data, (int)t.size, nullDraw)) continue;
        while (true) {
            auto f0 = std::chrono::steady_clock::now();
            int rc = g.playFrame(false, nullptr);
            auto f1 = std::chrono::steady_clock::now();
            if (g.getLastError() == GIF_SUCCESS) {
                double us = std::chrono::duration<double, std::micro>(f1 - f0).count();
                frameUs.push_back(us);
                totalFrames++;
            }
            if (rc <= 0) break;
        }
        g.close();
    }
    auto t1 = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double med = percentile(frameUs, 0.50);
    double p95 = percentile(frameUs, 0.95);

    double coopMaxStepUs = -1.0;
#if defined(GIF_ENABLE_COOPERATIVE) && GIF_ENABLE_COOPERATIVE
    {
        AnimatedGIF g;
        g.begin(GIF_PALETTE_RGB565_LE);
        if (g.open((uint8_t *)t.data, (int)t.size, nullDraw)) {
            while (true) {
                if (!g.beginFrame(nullptr)) break;
                int st = GIF_DECODE_MORE;
                while (!g.frameComplete()) {
                    auto s0 = std::chrono::steady_clock::now();
                    st = g.decodeStep(500);
                    auto s1 = std::chrono::steady_clock::now();
                    double sus = std::chrono::duration<double, std::micro>(s1 - s0).count();
                    if (sus > coopMaxStepUs) coopMaxStepUs = sus;
                    if (st != GIF_DECODE_MORE) break;
                }
                if (st == GIF_DECODE_ERR || st == GIF_DECODE_END) break;
            }
            g.close();
        }
    }
#endif

    printf("%-16s | %-18s | sizeof=%5zu | frames=%4d | total=%7.2f ms | med=%7.2f us | p95=%7.2f us | max_step=%7.2f us\n",
           label, t.name, sizeof(GIFIMAGE), totalFrames / t.reps, totalMs / t.reps, med, p95, coopMaxStepUs);
}

int main(int argc, char **argv) {
    const char *label = (argc >= 2) ? argv[1] : "default";
    std::string corpusDir = (argc >= 3) ? argv[2] : "tests/corpus";

    auto def_cc = loadFile(corpusDir + "/valid_deferred_cc.gif");
    auto multi_cc = loadFile(corpusDir + "/valid_multi_cc.gif");
    auto long_chain = loadFile(corpusDir + "/valid_long_chain.gif");

    std::vector<BenchTarget> targets = {
        {"emb_badgers", badgers, sizeof(badgers), 40},
        {"emb_bw_wiggler", bw_wiggler_128x64, sizeof(bw_wiggler_128x64), 10},
        {"emb_earth", earth_128x128, sizeof(earth_128x128), 10},
        {"emb_nostromo", nostromo, sizeof(nostromo), 5},
        {"emb_thisisfine", thisisfine_240x179, sizeof(thisisfine_240x179), 10},
    };
    if (!def_cc.empty()) targets.push_back({"valid_deferred_cc", def_cc.data(), def_cc.size(), 40});
    if (!multi_cc.empty()) targets.push_back({"valid_multi_cc", multi_cc.data(), multi_cc.size(), 100});
    if (!long_chain.empty()) targets.push_back({"valid_long_chain", long_chain.data(), long_chain.size(), 5});

    for (const auto &t : targets) {
        runTarget(label, t);
    }
    return 0;
}
