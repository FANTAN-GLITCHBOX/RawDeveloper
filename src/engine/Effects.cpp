#include "engine/Effects.h"

#include <algorithm>
#include <cmath>

#include "engine/Color.h"
#include "engine/Parallel.h"
#include "engine/Pipeline.h"

namespace rd {

bool EffectAmounts::any() const {
    return !haze.empty() || gHaze != 0 || gClarity != 0 || gTexture != 0 || gSharp != 0;
}

EffectSigmas effectSigmas(double L) {
    EffectSigmas s;
    s.haze = float(0.008 * L);
    s.clarity = float(0.005 * L);
    s.tex1 = 0.8f;
    s.tex2 = float(std::max(2.5, 0.0008 * L));
    s.blur1 = float(0.0027 * L);
    s.blur2 = float(0.008 * L);
    return s;
}

bool maskHasEffects(const LocalMask& m) { return m.enabled && (m.sharpness != 0 || m.clarity != 0 || m.texture != 0 || m.dehaze != 0); }

EffectAmounts globalEffects(const Params& p, bool detail) {
    EffectAmounts a;
    if (p.enabled.basic) {
        a.gHaze = p.dehaze;
        a.gClarity = p.clarity;
        a.gTexture = p.texture;
    }
    if (detail && p.enabled.detail) a.gSharp = p.sharpenAmount;
    return a;
}

int effectsMargin(const Params& p, double L) {
    bool any = p.enabled.basic && (p.dehaze != 0 || p.clarity != 0 || p.texture != 0);
    bool blur = false;
    if (p.enabled.local)
        for (const auto& m : p.masks)
            if (maskHasEffects(m)) {
                any = true;
                blur = blur || m.sharpness < 0 || m.dehaze != 0;
            }
    if (!any) return 0;
    const EffectSigmas s = effectSigmas(L);
    return int(std::ceil(4 * std::max({s.haze, s.clarity, s.tex2, blur ? s.blur2 : 0.0f}))) + 4;
}

namespace {
inline float lum(const float* c) { return c[0] * kLuma[0] + c[1] * kLuma[1] + c[2] * kLuma[2]; }
inline float amt(const std::vector<float>& v, float g, size_t i) { return v.empty() ? g : v[i]; }
std::vector<float> lumPlane(const ImageF& g) {
    std::vector<float> Y(size_t(g.width) * g.height);
    for (size_t i = 0; i < Y.size(); ++i) Y[i] = lum(&g.data[i * 3]);
    return Y;
}
bool anyOf(const std::vector<float>& v, float g, bool neg) {
    if (v.empty()) return neg ? g < 0 : g != 0;
    for (float x : v)
        if (neg ? x < 0 : x != 0) return true;
    return false;
}
}  // namespace

void applyEffects(ImageF& g, const Params& p, const EffectAmounts& a, float scale, double L, bool detail) {
    const int w = g.width, h = g.height;
    const size_t n = size_t(w) * h;
    const EffectSigmas S = effectSigmas(L);
    // ---- かすみの除去（＋）・かすみ（−）：暗い成分（各画素の最小の色）の広がりからかすみの濃さを見る
    if (anyOf(a.haze, a.gHaze, false)) {
        std::vector<float> dark(n);
        for (size_t i = 0; i < n; ++i) {
            const float* c = &g.data[i * 3];
            dark[i] = std::min({c[0], c[1], c[2]}) / kHazeAir;
        }
        gaussianBlur(dark, w, h, std::max(0.5f, S.haze * scale));
        parallelFor(h, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
                const float hz = amt(a.haze, a.gHaze, i) / 100.0f;
                if (hz == 0) continue;
                float* c = &g.data[i * 3];
                const float d = std::clamp(dark[i], 0.0f, 1.0f);
                if (hz > 0) {
                    const float t = std::max(1.0f - 0.9f * hz * d, 0.15f);
                    for (int k = 0; k < 3; ++k) c[k] = std::clamp((c[k] - kHazeAir) / t + kHazeAir, 0.0f, 1.0f);
                } else {
                    const float k2 = -hz * 0.55f * (0.4f + 0.6f * (1.0f - d));
                    for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k] * (1 - k2) + kHazeAir * 0.9f * k2, 0.0f, 1.0f);
                }
            }
        });
    }
    // ---- 明瞭度：中くらいの大きさの明暗差（中間の明るさに強く効かせ、白と黒は守る）
    if (anyOf(a.clarity, a.gClarity, false)) {
        const std::vector<float> Y = lumPlane(g);
        std::vector<float> B = Y;
        gaussianBlur(B, w, h, std::max(0.5f, S.clarity * scale));
        parallelFor(h, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
                const float c0 = amt(a.clarity, a.gClarity, i) / 100.0f;
                if (c0 == 0) continue;
                const float y = std::clamp(Y[i], 0.0f, 1.0f);
                const float wgt = 0.25f + 3.0f * y * (1 - y);
                const float d = (Y[i] - B[i]) * c0 * (c0 > 0 ? 1.1f : 0.9f) * wgt;
                for (int k = 0; k < 3; ++k) g.data[i * 3 + k] = std::clamp(g.data[i * 3 + k] + d, 0.0f, 1.0f);
            }
        });
    }
    // ---- テクスチャ：細かい〜少し大きい凹凸（いちばん細かいノイズの段は含めない）
    if (anyOf(a.texture, a.gTexture, false)) {
        const std::vector<float> Y = lumPlane(g);
        std::vector<float> B1 = Y, B2 = Y;
        gaussianBlur(B1, w, h, std::max(0.3f, S.tex1 * scale));
        gaussianBlur(B2, w, h, std::max(0.6f, S.tex2 * scale));
        parallelFor(h, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
                const float t = amt(a.texture, a.gTexture, i) / 100.0f;
                if (t == 0) continue;
                const float band = B1[i] - B2[i];
                const float d = band * t * (t > 0 ? 1.5f : 1.0f);
                for (int k = 0; k < 3; ++k) g.data[i * 3 + k] = std::clamp(g.data[i * 3 + k] + d, 0.0f, 1.0f);
            }
        });
    }
    // ---- ぼかし（シャープを − にしたマスク）
    if (anyOf(a.sharp, a.gSharp, true)) {
        std::vector<float> c1[3], c2[3];
        for (int k = 0; k < 3; ++k) {
            c1[k].resize(n);
            for (size_t i = 0; i < n; ++i) c1[k][i] = g.data[i * 3 + k];
            c2[k] = c1[k];
            gaussianBlur(c1[k], w, h, std::max(0.5f, S.blur1 * scale));
            gaussianBlur(c2[k], w, h, std::max(0.5f, S.blur2 * scale));
        }
        for (size_t i = 0; i < n; ++i) {
            const float s = amt(a.sharp, a.gSharp, i);
            if (s >= 0) continue;
            const float kk = std::clamp(-s / 100.0f, 0.0f, 1.0f);
            for (int k = 0; k < 3; ++k) {
                const float v = kk < 0.5f ? g.data[i * 3 + k] + (c1[k][i] - g.data[i * 3 + k]) * (kk * 2)
                                          : c1[k][i] + (c2[k][i] - c1[k][i]) * (kk * 2 - 1);
                g.data[i * 3 + k] = v;
            }
        }
    }
    // ---- シャープ（アンシャープマスク。ディテールが低いほど強い輪郭のにじみ〔ハロー〕を抑える）
    const float rad = p.sharpenRadius * scale;
    if (detail && rad >= 0.3f && anyOf(a.sharp, a.gSharp, false)) {
        const std::vector<float> Y = lumPlane(g);
        std::vector<float> blur = Y;
        gaussianBlur(blur, w, h, rad);
        const float maskAmt = p.sharpenMasking / 100.0f;
        const float damp = (1.0f - std::clamp(p.sharpenDetail, 0.0f, 100.0f) / 100.0f) * 20.0f;
        parallelFor(h, [&](int a0, int b0) {
            for (int y = a0; y < b0; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    const float s = amt(a.sharp, a.gSharp, i);
                    if (s <= 0) continue;
                    float det = Y[i] - blur[i];
                    if (maskAmt > 0) {
                        auto at = [&](int xx, int yy) {
                            xx = std::clamp(xx, 0, w - 1);
                            yy = std::clamp(yy, 0, h - 1);
                            return blur[size_t(yy) * w + xx];
                        };
                        const float gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) -
                                         (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
                        const float gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) -
                                         (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
                        const float mag = std::sqrt(gx * gx + gy * gy);
                        const float thr = maskAmt * 0.25f;
                        det *= std::clamp((mag - thr * 0.5f) / std::max(thr, 1e-4f), 0.0f, 1.0f);
                    }
                    if (damp > 0) {
                        const float q = det * damp;
                        det /= 1.0f + q * q;
                    }
                    const float add = s / 100.0f * 1.2f * det;
                    for (int k = 0; k < 3; ++k) g.data[i * 3 + k] += add;
                }
        });
    }
    for (float& x : g.data) x = std::clamp(x, 0.0f, 1.0f);
}

void outputSharpen(ImageF& g, const QString& kind, int level) {
    if (kind.isEmpty() || kind == "none" || level <= 0) return;
    // 画面：細かく弱め、印刷：少し太く強め（紙ではにじむ分を見込む）
    float rad = 0.6f, amount = 0.35f;
    if (kind == "matte") {
        rad = 1.0f;
        amount = 0.7f;
    } else if (kind == "glossy") {
        rad = 0.9f;
        amount = 0.55f;
    }
    amount *= level == 1 ? 0.6f : level == 3 ? 1.5f : 1.0f;
    const int w = g.width, h = g.height;
    std::vector<float> Y = lumPlane(g), B = Y;
    gaussianBlur(B, w, h, rad);
    for (size_t i = 0; i < Y.size(); ++i) {
        float d = Y[i] - B[i];
        const float q = d * 12.0f;
        d /= 1.0f + q * q;  // 強い輪郭のにじみを抑える
        for (int k = 0; k < 3; ++k) g.data[i * 3 + k] = std::clamp(g.data[i * 3 + k] + amount * d, 0.0f, 1.0f);
    }
}

}  // namespace rd
