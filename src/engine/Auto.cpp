#include "engine/Auto.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <vector>

#include "engine/Color.h"
#include "engine/Pipeline.h"

namespace rd {

namespace {

constexpr double kMaxMiredShift = 60.0;

float percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    // NumPy の線形補間と同じ定義
    const double pos = q / 100.0 * (v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    std::nth_element(v.begin(), v.begin() + lo, v.end());
    const float a = v[lo];
    const float b = hi == lo ? a : *std::min_element(v.begin() + lo + 1, v.end());
    return static_cast<float>(a + (b - a) * (pos - lo));
}

float lumOf(const float* p) { return p[0] * kLuma[0] + p[1] * kLuma[1] + p[2] * kLuma[2]; }

std::optional<std::pair<double, double>> autoWb(const ImageF& small, const std::array<float, 3>& asShot) {
    const size_t n = static_cast<size_t>(small.width) * small.height;
    std::vector<float> x(n * 3), lum(n), mx(n);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) x[i * 3 + c] = small.data[i * 3 + c] * asShot[c];
        lum[i] = lumOf(&x[i * 3]);
        mx[i] = std::max({x[i * 3], x[i * 3 + 1], x[i * 3 + 2]});
    }
    const float mxMax = *std::max_element(mx.begin(), mx.end());
    const float p20 = percentile(lum, 20);
    std::vector<size_t> valid;
    for (size_t i = 0; i < n; ++i)
        if (mx[i] < 0.95f * mxMax && lum[i] > p20) valid.push_back(i);
    if (valid.size() < 200) {
        valid.clear();
        for (size_t i = 0; i < n; ++i)
            if (mx[i] < 0.98f) valid.push_back(i);
    }
    if (valid.empty()) return std::nullopt;
    std::vector<float> sat(valid.size());
    for (size_t k = 0; k < valid.size(); ++k) {
        const float* p = &x[valid[k] * 3];
        float g[3];
        for (int c = 0; c < 3; ++c) g[c] = std::pow(std::max(p[c], 0.0f), 1.0f / kGamma);
        const float gmax = std::max({g[0], g[1], g[2]}), gmin = std::min({g[0], g[1], g[2]});
        sat[k] = (gmax - gmin) / std::max(gmax, 1e-4f);
    }
    const float thr = percentile(sat, 12);
    double gm[3] = {0, 0, 0}, ws = 0, wm[3] = {0, 0, 0};
    for (size_t k = 0; k < valid.size(); ++k) {
        const float* p = &x[valid[k] * 3];
        for (int c = 0; c < 3; ++c) wm[c] += p[c];
        if (sat[k] <= thr) {
            const double w = std::pow(lumOf(p), 2.0) + 1e-4;
            for (int c = 0; c < 3; ++c) gm[c] += p[c] * w;
            ws += w;
        }
    }
    double est[3];
    for (int c = 0; c < 3; ++c) est[c] = std::max(0.7 * gm[c] / ws + 0.3 * wm[c] / valid.size(), 1e-5);
    double corr[3] = {1.0 / est[0], 1.0 / est[1], 1.0 / est[2]};
    const double c1 = corr[1];
    for (double& c : corr) c /= c1;
    if (std::max({std::abs(std::log(corr[0])), std::abs(std::log(corr[1])), std::abs(std::log(corr[2]))}) < 0.04)
        return std::nullopt;
    std::array<float, 3> gains{static_cast<float>(corr[0] * asShot[0]), static_cast<float>(corr[1] * asShot[1]),
                               static_cast<float>(corr[2] * asShot[2])};
    auto [t, tint] = fitTempTint(gains);
    auto [t0, tint0] = fitTempTint(asShot);
    double m = 1e6 / t;
    const double m0 = 1e6 / t0;
    m = std::clamp(m, m0 - kMaxMiredShift, m0 + kMaxMiredShift);
    t = std::round(1e6 / m / 10.0) * 10.0;
    tint = std::clamp(tint, tint0 - 40, tint0 + 40);
    return std::make_pair(t, std::round(tint));
}

}  // namespace

Params autoParams(const ImageF& lin, const std::array<float, 3>& asShot, float strength, const Params& base,
                  bool wbOnly) {
    Params p = base;
    p.enabled.basic = true;
    const ImageF small = downscale(lin, 512);
    const float k = strength / 100.0f;

    // 1) WB
    const auto res = autoWb(small, asShot);
    const auto [t0, tint0] = fitTempTint(asShot);
    std::array<float, 3> gains;
    if (!res || k <= 0) {
        p.wbMode = "as_shot";
        p.temp = static_cast<float>(t0);
        p.tint = static_cast<float>(tint0);
        gains = asShot;
    } else {
        p.wbMode = "custom";
        p.temp = static_cast<float>(std::round((t0 + (res->first - t0) * k) / 10.0) * 10.0);
        p.tint = static_cast<float>(std::round(tint0 + (res->second - tint0) * k));
        gains = wbGains(p.temp, p.tint);
    }
    if (wbOnly) return p;

    const int w = small.width, h = small.height;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> lum(n);
    for (size_t i = 0; i < n; ++i) {
        float px[3];
        for (int c = 0; c < 3; ++c) px[c] = small.data[i * 3 + c] * gains[c];
        lum[i] = lumOf(px);
    }

    // 2) 露出：中央重点で輝度の中央値（対数）を中間グレー付近へ
    std::vector<std::pair<float, float>> lw(n);  // (log2 輝度, 重み)
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float cw = std::exp(-(std::pow((x - w / 2.0f) / (w * 0.45f), 2.0f) +
                                        std::pow((y - h / 2.0f) / (h * 0.45f), 2.0f)));
            lw[i] = {std::log2(std::max(lum[i], 1e-5f)), cw};
        }
    std::sort(lw.begin(), lw.end());
    double total = 0;
    for (auto& e : lw) total += e.second;
    double acc = 0;
    float med = lw.back().first;
    for (auto& e : lw) {
        acc += e.second;
        if (acc >= total * 0.5) {
            med = e.first;
            break;
        }
    }
    double ev = std::log2(0.16) - med;
    const float p995 = percentile(lum, 99.5);
    const double evMax = std::log2(1.6 / std::max(p995, 1e-5f));
    ev = std::clamp(std::min(ev, evMax), -4.0, 4.0);
    p.exposure = static_cast<float>(std::round(ev * k * 100.0) / 100.0);

    // 3) 4) 今の見た目で帯域ごとの量を見る
    Params probe = p;
    probe.contrast = probe.highlights = probe.shadows = probe.whites = probe.blacks = 0;
    probe.enabled.curve = base.enabled.curve;
    const ToneLut lut = toneLut(probe);
    const float evf = std::pow(2.0f, p.exposure);
    constexpr float kIdx = (kLutN - 1) / kLutMax;
    std::vector<float> gl(n), satv(n);
    for (size_t i = 0; i < n; ++i) {
        float v[3];
        for (int c = 0; c < 3; ++c) {
            const float x = std::max(small.data[i * 3 + c] * gains[c] * evf, 0.0f);
            const int idx = std::clamp(static_cast<int>(std::pow(x, 1.0f / kGamma) * kIdx), 0, kLutN - 1);
            v[c] = lut.t[c + 1][idx];
        }
        gl[i] = lumOf(v);
        const float mx = std::max({v[0], v[1], v[2]}), mn = std::min({v[0], v[1], v[2]});
        satv[i] = (mx - mn) / std::max(mx, 1e-4f);
    }
    double hi = 0, lo = 0, mean = 0;
    for (float v : gl) {
        hi += v > 0.88f;
        lo += v < 0.08f;
        mean += v;
    }
    hi /= n;
    lo /= n;
    mean /= n;
    double var = 0;
    for (float v : gl) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / n);
    const float pHi = percentile(gl, 99.9), pLo = percentile(gl, 0.1);
    const double highlights = hi > 0.02 ? -std::min(100.0, hi * 600.0) : 0.0;
    const double shadows = lo > 0.03 ? std::min(80.0, lo * 400.0) : 0.0;
    const double whites = std::clamp((0.985 - pHi) * 200.0, -30.0, 25.0);
    const double blacks = std::clamp((0.015 - pLo) * 300.0, -25.0, 25.0);
    const double contrast = std::clamp((0.21 - sd) * 300.0, -30.0, 40.0);
    const double satMean = std::accumulate(satv.begin(), satv.end(), 0.0) / n;
    const double vibrance = std::clamp((0.30 - satMean) * 120.0, 0.0, 30.0);
    p.highlights = static_cast<float>(std::round(highlights * k));
    p.shadows = static_cast<float>(std::round(shadows * k));
    p.whites = static_cast<float>(std::round(whites * k));
    p.blacks = static_cast<float>(std::round(blacks * k));
    p.contrast = static_cast<float>(std::round(contrast * k));
    p.vibrance = static_cast<float>(std::round(vibrance * k));
    p.autoStrength = strength;
    return p;
}

}  // namespace rd
