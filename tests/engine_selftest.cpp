// エンジンの動作確認：RAW を読み込み、時間と撮影時 WB を表示する
// 使い方：rd_selftest <RAW ファイル>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "engine/Auto.h"
#include "engine/Pipeline.h"
#include "engine/RawLoader.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: rd_selftest <raw>\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    using clk = std::chrono::steady_clock;
    for (bool half : {true, false}) {
        auto t0 = clk::now();
        rd::LoadedRaw r = rd::loadRaw(bytes, half);
        auto t1 = clk::now();
        if (!r.error.empty()) {
            std::printf("error: %s\n", r.error.c_str());
            return 1;
        }
        auto prev = rd::downscale(r.linear, 1600);
        rd::Params p = rd::autoParams(prev, r.asShotGains, 100, rd::Params::defaults());
        p.hslSat[3] = 20;
        p.lift = {0.1f, 0.0f, 0.0f};
        p.curve[0] = {{0, 0}, {0.5f, 0.55f}, {1, 1}};
        auto t2 = clk::now();
        rd::DevelopOptions o;
        o.scale = static_cast<float>(prev.width) / r.meta.fullWidth;
        auto rgb = rd::toRgb8(rd::develop(prev, p, r.asShotGains, o));
        auto t3 = clk::now();
        if (!half) {
            o.scale = 1.0f;
            auto t4 = clk::now();
            auto big = rd::develop(r.linear, p, r.asShotGains, o);
            auto t5 = clk::now();
            std::printf("full-size develop %dx%d: %.0f ms\n", big.width, big.height,
                        std::chrono::duration<double, std::milli>(t5 - t4).count());
        }
        double m[3] = {0, 0, 0};
        for (size_t i = 0; i < rgb.size(); i += 3)
            for (int c = 0; c < 3; ++c) m[c] += rgb[i + c];
        const double n = rgb.size() / 3.0;
        std::printf("%s: %dx%d load %.0f ms, preview %dx%d develop %.1f ms, asShot %.3f %.3f %.3f, mean %.1f %.1f %.1f\n",
                    half ? "half" : "full", r.linear.width, r.linear.height,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), prev.width, prev.height,
                    std::chrono::duration<double, std::milli>(t3 - t2).count(), r.asShotGains[0], r.asShotGains[1],
                    r.asShotGains[2], m[0] / n, m[1] / n, m[2] / n);
        std::printf("meta: %s / %s / lens '%s' ISO %.0f F%.1f %.0fmm full %dx%d\n", r.meta.make.c_str(),
                    r.meta.model.c_str(), r.meta.lensModel.c_str(), r.meta.iso, r.meta.aperture, r.meta.focal,
                    r.meta.fullWidth, r.meta.fullHeight);
    }
    return 0;
}
