#include "engine/Denoise.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Parallel.h"

namespace rd {

namespace {

constexpr int kLumaLevels = 5;
constexpr int kChromaLevels = 6;
// B3 スプラインの à trous で、白色ノイズ（σ=1）の各段の標準偏差
constexpr float kLevelSigma[7] = {0.889f, 0.200f, 0.086f, 0.041f, 0.020f, 0.010f, 0.005f};

// 1 段ぶんのぼかし（[1 4 6 4 1]/16 を、間隔 step で縦横に）
void smoothStep(const std::vector<float>& in, std::vector<float>& out, int w, int h, int step) {
    std::vector<float> tmp(in.size());
    auto cl = [](int v, int n) { return v < 0 ? -v - 1 < n ? -v - 1 : 0 : v >= n ? std::max(0, 2 * n - v - 1) : v; };
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* r = &in[size_t(y) * w];
            float* o = &tmp[size_t(y) * w];
            for (int x = 0; x < w; ++x) {
                o[x] = (r[cl(x - 2 * step, w)] + r[cl(x + 2 * step, w)]) * (1.0f / 16) +
                       (r[cl(x - step, w)] + r[cl(x + step, w)]) * (4.0f / 16) + r[x] * (6.0f / 16);
            }
        }
    });
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* a = &tmp[size_t(cl(y - 2 * step, h)) * w];
            const float* b = &tmp[size_t(cl(y - step, h)) * w];
            const float* c = &tmp[size_t(y) * w];
            const float* d = &tmp[size_t(cl(y + step, h)) * w];
            const float* e = &tmp[size_t(cl(y + 2 * step, h)) * w];
            float* o = &out[size_t(y) * w];
            for (int x = 0; x < w; ++x) o[x] = (a[x] + e[x]) * (1.0f / 16) + (b[x] + d[x]) * (4.0f / 16) + c[x] * (6.0f / 16);
        }
    });
}

// 各段の係数を、しきい値 t[l] で縮める（小さい揺らぎ＝ノイズほど強く、大きい＝エッジは残す）
void shrinkChannel(std::vector<float>& ch, int w, int h, int levels, const float* thr) {
    std::vector<float> cur = ch, next(ch.size()), out(ch.size(), 0.0f);
    for (int l = 0; l < levels; ++l) {
        smoothStep(cur, next, w, h, 1 << l);
        const float t2 = thr[l] * thr[l];
        parallelFor(h, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
                const float d = cur[i] - next[i];
                const float d2 = d * d;
                out[i] += t2 <= 0 ? d : d2 > t2 ? d * (1.0f - t2 / d2) : 0.0f;
            }
        });
        std::swap(cur, next);
    }
    for (size_t i = 0; i < ch.size(); ++i) ch[i] = out[i] + cur[i];
}

float mad(std::vector<float> v) {
    if (v.empty()) return 0;
    auto mid = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), mid, v.end());
    return *mid / 0.6745f;
}

}  // namespace

bool denoiseActive(const Params& p) { return p.enabled.detail && (p.nrLuma > 0 || p.nrColor > 0); }

int denoiseMargin(const Params& p) {
    if (!denoiseActive(p)) return 0;
    const int levels = p.nrColor > 0 ? kChromaLevels : kLumaLevels;
    return 2 * ((1 << levels) - 1) + 4;
}

float defaultLumaNr(float iso) {
    if (iso <= 200) return 0;
    return std::clamp(std::round(9.0f * std::log2(iso / 200.0f)), 0.0f, 60.0f);  // 400→9, 1600→27, 6400→45
}

NoiseLevel estimateNoise(const ImageF& lin) {
    // 中央の最大 1024×1024 で測る
    const int cw = std::min(lin.width, 1024), ch = std::min(lin.height, 1024);
    const int ox = (lin.width - cw) / 2, oy = (lin.height - ch) / 2;
    if (cw < 32 || ch < 32) return {};
    std::vector<float> c[3];
    for (auto& v : c) v.resize(size_t(cw) * ch);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const float* p = lin.px(ox + x, oy + y);
            const float r = std::sqrt(std::max(p[0], 0.0f)), g = std::sqrt(std::max(p[1], 0.0f)),
                        b = std::sqrt(std::max(p[2], 0.0f));
            const size_t i = size_t(y) * cw + x;
            c[0][i] = (r + g + b) / 3;
            c[1][i] = (r - b) / 2;
            c[2][i] = (r - 2 * g + b) / 4;
        }
    NoiseLevel n;
    float* outs[3] = {&n.y, &n.c1, &n.c2};
    for (int k = 0; k < 3; ++k) {
        std::vector<float> s(c[k].size()), d;
        smoothStep(c[k], s, cw, ch, 1);
        d.reserve(c[k].size() / 4);
        // 真っ暗・白飛びの所は除く（揺らぎが小さく出てしまう）
        for (size_t i = 0; i < s.size(); i += 2) {
            const float yv = c[0][i];
            if (yv > 0.02f && yv < 0.95f) d.push_back(std::abs(c[k][i] - s[i]));
        }
        *outs[k] = mad(std::move(d)) / kLevelSigma[0];
    }
    return n;
}

void denoise(ImageF& lin, const Params& p, const NoiseLevel& noise) {
    if (!denoiseActive(p) || !noise.valid()) return;
    const int w = lin.width, h = lin.height;
    const size_t n = size_t(w) * h;
    std::vector<float> Y(n), C1(n), C2(n);
    parallelFor(h, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
            const float r = std::sqrt(std::max(lin.data[i * 3], 0.0f)), g = std::sqrt(std::max(lin.data[i * 3 + 1], 0.0f)),
                        b = std::sqrt(std::max(lin.data[i * 3 + 2], 0.0f));
            Y[i] = (r + g + b) / 3;
            C1[i] = (r - b) / 2;
            C2[i] = (r - 2 * g + b) / 4;
        }
    });
    if (p.nrLuma > 0) {
        // 適用量 → しきい値の倍率。ディテール：細かい段を守る。コントラスト：粗い段（質感・立体感）を守る
        const float k = 3.2f * p.nrLuma / 100.0f;
        const float fine = 1.35f - 0.9f * p.nrLumaDetail / 100.0f;
        const float coarse = 1.0f - 0.75f * p.nrLumaContrast / 100.0f;
        float thr[kLumaLevels];
        for (int l = 0; l < kLumaLevels; ++l) thr[l] = k * noise.y * kLevelSigma[l] * (l < 2 ? fine : coarse);
        shrinkChannel(Y, w, h, kLumaLevels, thr);
    }
    if (p.nrColor > 0) {
        const float k = 4.0f * p.nrColor / 100.0f;
        const float fine = 1.4f - 0.9f * p.nrColorDetail / 100.0f;
        const float smooth = 0.5f + 1.0f * p.nrColorSmooth / 100.0f;  // 大きな色ムラ
        float t1[kChromaLevels], t2[kChromaLevels];
        for (int l = 0; l < kChromaLevels; ++l) {
            const float m = l < 2 ? fine : l < 4 ? 1.0f : smooth;
            // 色のノイズは粗い段にも残りやすいので、粗い段のしきい値は白色ノイズの見積もりより大きめに
            const float boost = 1.0f + 0.6f * l;
            t1[l] = k * noise.c1 * kLevelSigma[l] * m * boost;
            t2[l] = k * noise.c2 * kLevelSigma[l] * m * boost;
        }
        shrinkChannel(C1, w, h, kChromaLevels, t1);
        shrinkChannel(C2, w, h, kChromaLevels, t2);
    }
    parallelFor(h, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
            const float y = Y[i], a = C1[i], b = C2[i];
            // 逆変換：r = y + a + b*2/3 ... を解く
            const float g = y - 4.0f * b / 3.0f;
            const float r = y + a + 2.0f * b / 3.0f;
            const float bl = y - a + 2.0f * b / 3.0f;
            const float v[3] = {r, g, bl};
            for (int c = 0; c < 3; ++c) {
                const float s = std::max(v[c], 0.0f);
                lin.data[i * 3 + c] = s * s;
            }
        }
    });
}

}  // namespace rd
