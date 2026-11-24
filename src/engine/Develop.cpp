#include "engine/Develop.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace rd {

std::vector<std::uint8_t> developToRgb8(const ImageF& lin, const Params& p) {
    // ガンマ 2.2 の表（0..4 のリニア値を 4096 段で引く）
    static const std::vector<std::uint8_t> lut = [] {
        std::vector<std::uint8_t> t(4096);
        for (int i = 0; i < 4096; ++i) {
            const double v = i / 4095.0;
            t[i] = static_cast<std::uint8_t>(std::clamp(std::pow(v, 1.0 / 2.2) * 255.0 + 0.5, 0.0, 255.0));
        }
        return t;
    }();
    const float ev = std::pow(2.0f, p.exposure);
    const float g[3] = {p.wbGains[0] * ev, p.wbGains[1] * ev, p.wbGains[2] * ev};
    std::vector<std::uint8_t> out(static_cast<size_t>(lin.width) * lin.height * 3);
    const size_t n = static_cast<size_t>(lin.width) * lin.height;

    auto work = [&](size_t a, size_t b) {
        for (size_t i = a; i < b; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(lin.data[i * 3 + c] * g[c], 0.0f, 1.0f);
                out[i * 3 + c] = lut[static_cast<int>(v * 4095.0f)];
            }
        }
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < nt; ++t) ts.emplace_back(work, n * t / nt, n * (t + 1) / nt);
    for (auto& t : ts) t.join();
    return out;
}

}  // namespace rd
