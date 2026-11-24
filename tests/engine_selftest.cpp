// エンジンの動作確認：RAW を読み込み、時間と撮影時 WB を表示する
// 使い方：rd_selftest <RAW ファイル>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "engine/Develop.h"
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
        rd::Params p;
        p.wbGains = r.asShotGains;
        auto t2 = clk::now();
        auto rgb = rd::developToRgb8(prev, p);
        auto t3 = clk::now();
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
