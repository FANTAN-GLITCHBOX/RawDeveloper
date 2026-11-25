#include "engine/Pipeline.h"
#include "engine/Skin.h"
#include "engine/Gpu.h"
#include <atomic>

#include <QJsonDocument>
#include <QJsonObject>
#include <memory>
#include <mutex>
#include <cstring>
#include <cstdint>

#include <algorithm>
#include <cmath>

#include "engine/Color.h"
#include "engine/Parallel.h"

namespace rd {

namespace {
constexpr float kPi = 3.14159265358979f;

inline float sq(float x) { return x * x; }
inline float bump(float x, float c, float w) { return std::exp(-sq((x - c) / w)); }
}  // namespace

// ---------------------------------------------------------------- curves

std::vector<float> monotoneSpline(const CurvePoints& ptsIn, const std::vector<float>& xs) {
    CurvePoints pts = ptsIn;
    std::sort(pts.begin(), pts.end());
    const size_t n = pts.size();
    std::vector<float> out(xs.size());
    if (n == 1) {
        std::fill(out.begin(), out.end(), pts[0].second);
        return out;
    }
    std::vector<double> px(n), py(n), d(n - 1), m(n);
    for (size_t i = 0; i < n; ++i) {
        px[i] = pts[i].first;
        py[i] = pts[i].second;
    }
    for (size_t i = 0; i + 1 < n; ++i) d[i] = (py[i + 1] - py[i]) / std::max(px[i + 1] - px[i], 1e-9);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0.0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
        } else {
            const double a = m[i] / d[i], b = m[i + 1] / d[i];
            const double s = a * a + b * b;
            if (s > 9) {
                const double t = 3 / std::sqrt(s);
                m[i] = t * a * d[i];
                m[i + 1] = t * b * d[i];
            }
        }
    }
    for (size_t k = 0; k < xs.size(); ++k) {
        const double x = xs[k];
        if (x < px[0]) {
            out[k] = static_cast<float>(py[0]);
            continue;
        }
        if (x > px[n - 1]) {
            out[k] = static_cast<float>(py[n - 1]);
            continue;
        }
        size_t i = std::upper_bound(px.begin(), px.end(), x) - px.begin();
        i = std::clamp<size_t>(i == 0 ? 0 : i - 1, 0, n - 2);
        const double h = std::max(px[i + 1] - px[i], 1e-9);
        const double t = (x - px[i]) / h, t2 = t * t, t3 = t2 * t;
        const double y = (2 * t3 - 3 * t2 + 1) * py[i] + (t3 - 2 * t2 + t) * h * m[i] +
                         (-2 * t3 + 3 * t2) * py[i + 1] + (t3 - t2) * h * m[i + 1];
        out[k] = static_cast<float>(y);
    }
    return out;
}

ToneLut toneLut(const Params& pIn) {
    const bool basic = pIn.enabled.basic;
    const float blacks = basic ? pIn.blacks : 0, shadows = basic ? pIn.shadows : 0;
    const float highlights = basic ? pIn.highlights : 0, whites = basic ? pIn.whites : 0;
    const float contrast = basic ? pIn.contrast : 0;
    std::vector<float> y(kLutN);
    for (int i = 0; i < kLutN; ++i) {
        const float x = kLutMax * i / (kLutN - 1);
        constexpr float knee = 0.80f;
        float v = x < knee ? x : knee + (1 - knee) * (1 - std::exp(-(x - knee) / (1 - knee)));
        // 軽い S 字（中間 0.5 を軸に暗部を下げ明部を上げる）
        v = v - 0.06f * std::sin(2 * kPi * std::clamp(v, 0.0f, 1.0f)) / (2 * kPi) * 2;
        v = v + blacks / 100.0f * 0.08f * bump(v, 0.04f, 0.10f);
        v = v + shadows / 100.0f * 0.13f * bump(v, 0.25f, 0.17f);
        v = v + highlights / 100.0f * 0.13f * bump(v, 0.74f, 0.17f);
        v = v + whites / 100.0f * 0.08f * bump(v, 0.96f, 0.10f);
        const float c = contrast / 100.0f;
        if (c != 0 && v >= 0 && v <= 1) v = v - c * 0.45f * std::sin(2 * kPi * v) / (2 * kPi) * 2;
        y[i] = std::clamp(v, 0.0f, 1.0f);
    }
    for (int i = 1; i < kLutN; ++i) y[i] = std::max(y[i], y[i - 1]);
    const bool curves = pIn.enabled.curve;
    if (curves && !isIdentityCurve(pIn.curve[0])) y = monotoneSpline(pIn.curve[0], y);
    ToneLut lut;
    lut.t[0] = y;
    for (int c = 0; c < 3; ++c)
        lut.t[c + 1] = (curves && !isIdentityCurve(pIn.curve[c + 1])) ? monotoneSpline(pIn.curve[c + 1], y) : y;
    return lut;
}

std::array<float, 3> wbFor(const Params& p, const std::array<float, 3>& asShot) {
    if (!p.enabled.basic || p.wbMode == "as_shot") return asShot;
    return wbGains(p.temp, p.tint);
}

// ---------------------------------------------------------------- color

namespace {

std::array<float, 3> wheelRgb(const Wheel& w, float& master) {
    const float u = w[0], v = w[1];
    std::array<float, 3> vec{u * 0.866f, -u * 0.433f + v * 0.75f, -u * 0.433f - v * 0.75f};
    const float l = vec[0] * kLuma[0] + vec[1] * kLuma[1] + vec[2] * kLuma[2];
    for (float& x : vec) x -= l;
    master = w[2];
    return vec;
}

bool wheelsActive(const Params& p) {
    for (const Wheel* w : {&p.lift, &p.gamma, &p.gain, &p.offset})
        for (float x : *w)
            if (std::abs(x) > 1e-6f) return true;
    return false;
}

std::vector<float> bandTable(const std::array<float, kHslBands>& adj) {
    std::vector<float> t(721);
    for (int i = 0; i <= 720; ++i) {
        const float deg = i / 2.0f;
        int k = 0;
        while (k < kHslBands - 1 && deg >= kHslCenters[k + 1]) ++k;
        const float c0 = kHslCenters[k];
        const float c1 = (k == kHslBands - 1) ? 360.0f : kHslCenters[k + 1];
        const float v0 = adj[k], v1 = adj[(k + 1) % kHslBands];
        const float f = (deg - c0) / (c1 - c0);
        t[i] = v0 + (v1 - v0) * f;
    }
    return t;
}

bool anyNonZero(const std::array<float, kHslBands>& a) {
    return std::any_of(a.begin(), a.end(), [](float v) { return v != 0; });
}

// 1 次元のガウスぼかし（OpenCV と同じく端は反射、半径は σ×4）
void blur1d(const float* src, float* dst, int n, int stride, const std::vector<float>& k, int r) {
    // 端から離れた部分は境界の判定なしで速く
    for (int i = r; i < n - r; ++i) {
        float acc = 0;
        const float* s0 = src + static_cast<size_t>(i - r) * stride;
        for (int j = 0; j <= 2 * r; ++j) acc += k[j] * s0[static_cast<size_t>(j) * stride];
        dst[static_cast<size_t>(i) * stride] = acc;
    }
    for (int i = 0; i < n; ++i) {
        if (i == r && n - r > r) i = n - r;
        float acc = 0;
        for (int j = -r; j <= r; ++j) {
            int idx = i + j;
            if (idx < 0) idx = -idx;
            if (idx >= n) idx = 2 * n - 2 - idx;
            idx = std::clamp(idx, 0, n - 1);
            acc += k[j + r] * src[static_cast<size_t>(idx) * stride];
        }
        dst[static_cast<size_t>(i) * stride] = acc;
    }
}

}  // namespace

void gaussianBlur(std::vector<float>& plane, int w, int h, float sigma) {
    if (sigma <= 0.05f) return;
    const int r = std::max(1, static_cast<int>(std::lround(sigma * 4)));
    std::vector<float> k(2 * r + 1);
    float s = 0;
    for (int i = -r; i <= r; ++i) s += (k[i + r] = std::exp(-0.5f * sq(i / sigma)));
    for (float& x : k) x /= s;
    std::vector<float> tmp(plane.size());
    parallelFor(h, [&](int a, int b) {
        for (int y = a; y < b; ++y) blur1d(&plane[static_cast<size_t>(y) * w], &tmp[static_cast<size_t>(y) * w], w, 1, k, r);
    });
    parallelFor(w, [&](int a, int b) {
        for (int x = a; x < b; ++x) blur1d(&tmp[x], &plane[x], h, w, k, r);
    });
}

namespace {

void applyDetail(ImageF& g, const Params& p, float scale) {
    const int w = g.width, h = g.height;
    const size_t n = static_cast<size_t>(w) * h;
    const float rad = p.sharpenRadius * scale;
    if (p.sharpenAmount > 0 && rad >= 0.3f) {
        std::vector<float> Y(n);
        for (size_t i = 0; i < n; ++i)
            Y[i] = g.data[i * 3] * kLuma[0] + g.data[i * 3 + 1] * kLuma[1] + g.data[i * 3 + 2] * kLuma[2];
        std::vector<float> blur = Y;
        gaussianBlur(blur, w, h, rad);
        const float k = p.sharpenAmount / 100.0f * 1.2f;
        const float maskAmt = p.sharpenMasking / 100.0f;
        parallelFor(h, [&](int a, int b) {
            for (int y = a; y < b; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    float detail = Y[i] - blur[i];
                    if (maskAmt > 0) {
                        auto at = [&](int xx, int yy) {
                            xx = std::clamp(xx, 0, w - 1);
                            yy = std::clamp(yy, 0, h - 1);
                            return blur[static_cast<size_t>(yy) * w + xx];
                        };
                        const float gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) -
                                         (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
                        const float gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) -
                                         (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
                        const float mag = std::sqrt(gx * gx + gy * gy);
                        const float thr = maskAmt * 0.25f;
                        detail *= std::clamp((mag - thr * 0.5f) / std::max(thr, 1e-4f), 0.0f, 1.0f);
                    }
                    const float add = k * detail;
                    g.data[i * 3] += add;
                    g.data[i * 3 + 1] += add;
                    g.data[i * 3 + 2] += add;
                }
        });
    }
}

ImageF rotate(const ImageF& src, float angleDeg) {
    if (std::abs(angleDeg) < 1e-3f) return src;
    const int w = src.width, h = src.height;
    ImageF dst;
    dst.resize(w, h);
    const double a = angleDeg * kPi / 180.0;
    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;
    const double ca = std::cos(a), sa = std::sin(a);
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                // 出力 → 入力（逆回転）
                const double dx = x - cx, dy = y - cy;
                const double sx = ca * dx + sa * dy + cx;
                const double sy = -sa * dx + ca * dy + cy;
                const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
                const float fx = static_cast<float>(sx - ix), fy = static_cast<float>(sy - iy);
                float* d = dst.px(x, y);
                for (int c = 0; c < 3; ++c) {
                    auto at = [&](int xx, int yy) -> float {
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) return 0.0f;
                        return src.px(xx, yy)[c];
                    };
                    const float v = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                                    (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
                    d[c] = v;
                }
            }
    });
    return dst;
}

ImageF cropImage(const ImageF& src, const Params& p) {
    const int w = src.width, h = src.height;
    int x0 = static_cast<int>(std::lround(p.cropX * w)), y0 = static_cast<int>(std::lround(p.cropY * h));
    int x1 = static_cast<int>(std::lround((p.cropX + p.cropW) * w));
    int y1 = static_cast<int>(std::lround((p.cropY + p.cropH) * h));
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(w, std::max(x0 + 1, x1));
    y1 = std::min(h, std::max(y0 + 1, y1));
    ImageF dst;
    dst.resize(x1 - x0, y1 - y0);
    for (int y = y0; y < y1; ++y)
        std::copy(src.px(x0, y), src.px(x0, y) + (x1 - x0) * 3, dst.px(0, y - y0));
    return dst;
}

}  // namespace

std::pair<double, double> rotatedRectMaxArea(double w, double h, double angle) {
    if (w <= 0 || h <= 0) return {0, 0};
    const bool widthLonger = w >= h;
    const double sideLong = widthLonger ? w : h, sideShort = widthLonger ? h : w;
    const double sa = std::abs(std::sin(angle)), ca = std::abs(std::cos(angle));
    if (sideShort <= 2.0 * sa * ca * sideLong || std::abs(sa - ca) < 1e-10) {
        const double x = 0.5 * sideShort;
        return widthLonger ? std::make_pair(x / sa, x / ca) : std::make_pair(x / ca, x / sa);
    }
    const double cos2a = ca * ca - sa * sa;
    return {(w * ca - h * sa) / cos2a, (h * ca - w * sa) / cos2a};
}

namespace {
GpuBackend* gGpu = nullptr;
std::atomic<bool> gGpuOn{true};
}  // namespace
void setGpuBackend(GpuBackend* b) { gGpu = b; }
GpuBackend* gpuBackend() { return gGpu; }
void setGpuEnabled(bool on) { gGpuOn = on; }
bool gpuEnabled() { return gGpuOn.load(); }

namespace {
// 同じ画像のノイズ量・ノイズ低減の結果を覚えておく（プレビューでスライダーを離すたびに作り直さないように）
// アドレスは別の画像で使い回されることがあるので、中身の指紋も比べる
std::uint64_t fingerprint(const ImageF& img) {
    std::uint64_t h = 1469598103934665603ull ^ (std::uint64_t(img.width) << 32 | std::uint32_t(img.height));
    const size_t n = img.data.size();
    if (!n) return h;
    for (int k = 0; k < 512; ++k) {
        const size_t i = (size_t(k) * 2654435761u + size_t(k) * n / 512) % n;
        std::uint32_t b;
        std::memcpy(&b, &img.data[i], 4);
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}
std::mutex gNrMutex;
struct NoiseCache {
    std::uint64_t fp = 0;
    const float* ptr = nullptr;
    int w = 0, h = 0;
    NoiseLevel n;
};
NoiseCache gNoise[4];
int gNoiseNext = 0;
struct DenoiseCache {
    std::uint64_t fp = 0;
    const float* ptr = nullptr;
    int w = 0, h = 0;
    QByteArray key;
    std::shared_ptr<const ImageF> img;
};
DenoiseCache gDen[2];
int gDenNext = 0;

QByteArray denoiseKey(const Params& p, bool lens, const NoiseLevel& n) {
    QJsonObject o;
    o["nr"] = p.toJson().value("nr");
    if (lens) o["lens"] = p.toJson().value("lens");
    o["n"] = QString("%1/%2/%3").arg(n.y).arg(n.c1).arg(n.c2);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

std::shared_ptr<const ImageF> cachedDenoise(const ImageF& src, const ImageF& lensed, const Params& p, bool lens,
                                            const NoiseLevel* given) {
    const NoiseLevel n = given ? *given : noiseOf(src);
    const QByteArray key = denoiseKey(p, lens, n);
    const std::uint64_t fp = fingerprint(src);
    {
        std::lock_guard<std::mutex> lk(gNrMutex);
        for (auto& c : gDen)
            if (c.fp == fp && c.ptr == src.data.data() && c.w == src.width && c.h == src.height && c.key == key) return c.img;
    }
    auto out = std::make_shared<ImageF>(lensed);
    denoise(*out, p, n);
    std::lock_guard<std::mutex> lk(gNrMutex);
    gDen[gDenNext] = {fp, src.data.data(), src.width, src.height, key, out};
    gDenNext = (gDenNext + 1) % 2;
    return out;
}
}  // namespace

namespace {
// レンズ補正の結果も覚えておく（スライダーを動かすたびに作り直さない）
struct LensCache {
    std::uint64_t fp = 0;
    const float* ptr = nullptr;
    int w = 0, h = 0;
    QByteArray key;
    std::shared_ptr<const ImageF> img;
};
LensCache gLens[2];
int gLensNext = 0;
}  // namespace

std::shared_ptr<const ImageF> cachedLens(const ImageF& src, const Params& p, const LensInfo* lens) {
    QJsonObject k;
    k["lens"] = p.toJson().value("lens");
    k["el"] = p.enabled.lens;
    k["auto"] = lens ? QString::fromStdString(lens->autoKey) + QString("/%1/%2/%3").arg(lens->crop).arg(lens->focal).arg(lens->aperture)
                     : QString();
    const QByteArray key = QJsonDocument(k).toJson(QJsonDocument::Compact);
    const std::uint64_t fp = fingerprint(src);
    {
        std::lock_guard<std::mutex> lk(gNrMutex);
        for (auto& c : gLens)
            if (c.fp == fp && c.ptr == src.data.data() && c.w == src.width && c.h == src.height && c.key == key) return c.img;
    }
    static const LensInfo none;
    auto out = std::make_shared<ImageF>(lensRemapRegion(src, p, lens ? *lens : none, 0, 0, src.width, src.height));
    std::lock_guard<std::mutex> lk(gNrMutex);
    gLens[gLensNext] = {fp, src.data.data(), src.width, src.height, key, out};
    gLensNext = (gLensNext + 1) % 2;
    return out;
}

NoiseLevel noiseOf(const ImageF& img) {
    const std::uint64_t fp = fingerprint(img);
    {
        std::lock_guard<std::mutex> lk(gNrMutex);
        for (auto& c : gNoise)
            if (c.fp == fp && c.ptr == img.data.data() && c.w == img.width && c.h == img.height) return c.n;
    }
    const NoiseLevel n = estimateNoise(img);
    std::lock_guard<std::mutex> lk(gNrMutex);
    gNoise[gNoiseNext] = {fp, img.data.data(), img.width, img.height, n};
    gNoiseNext = (gNoiseNext + 1) % 4;
    return n;
}

ImageF develop(const ImageF& linIn, const Params& p, const std::array<float, 3>& asShot, const DevelopOptions& o) {
    // レンズ補正（リニアのまま、座標の付け替えと周辺減光）
    ImageF lensed;
    const bool useLens = lensActive(p, o.lens) || (p.enabled.lens && (std::abs(p.lensManualDistortion) > 0.5f ||
                                                                     std::abs(p.lensManualVignetting) > 0.5f));
    std::shared_ptr<const ImageF> lensedC;
    if (useLens) lensedC = cachedLens(linIn, p, o.lens);
    const ImageF* linp = useLens ? lensedC.get() : &linIn;
    // ノイズ低減（トーン調整の前に。リニアの √ 空間で）
    std::shared_ptr<const ImageF> denoised;
    if (o.detail && denoiseActive(p)) {
        denoised = cachedDenoise(linIn, *linp, p, useLens, o.noise);
        linp = denoised.get();
    }
    // 画素と元画像の座標の対応
    PixelMap map = o.map;
    if (map.dx <= 0) {
        map.nx0 = map.ny0 = 0;
        map.dx = 1.0 / linp->width;
        map.dy = 1.0 / linp->height;
        map.fullW = linp->width / std::max(1e-6f, o.scale);
        map.fullH = linp->height / std::max(1e-6f, o.scale);
    }
    // スポット修復（トーン調整の前、リニアで）
    ImageF spotted;
    if (p.enabled.local && !p.spots.empty()) {
        spotted = *linp;
        applySpots(spotted, p, map, o.fetch);
        linp = &spotted;
    }
    const ImageF& lin = *linp;
    const int w = lin.width, h = lin.height;
    ImageF g;
    g.resize(w, h);
    const auto wb = wbFor(p, asShot);
    // カメラに合わせたプロファイルの明るさは、基本補正のオン/オフに関係なく下地として掛ける
    const float ev = std::pow(2.0f, (p.enabled.basic ? p.exposure : 0.0f) + (p.cameraLook() ? p.camExposure : 0.0f));
    const bool camMat = p.cameraLook();
    const auto& CM = p.camMatrix;
    const float gains[3] = {wb[0] * ev, wb[1] * ev, wb[2] * ev};
    const ToneLut lut = toneLut(p);

    // ホイールの係数
    constexpr float kIdx = (kLutN - 1) / kLutMax;
    const bool wheels = p.enabled.wheels && wheelsActive(p);
    float lm, gm, gnm, om;
    const auto lv = wheelRgb(p.lift, lm), gv = wheelRgb(p.gamma, gm), gnv = wheelRgb(p.gain, gnm),
               ov = wheelRgb(p.offset, om);
    float L[3], G[3], E[3], O[3];
    for (int c = 0; c < 3; ++c) {
        L[c] = (lv[c] + lm) * 0.25f;
        G[c] = 1.0f + (gnv[c] + gnm) * 0.5f;
        E[c] = std::pow(2.0f, -(gv[c] + gm) * 1.2f);
        O[c] = (ov[c] + om) * 0.2f;
    }
    const float sat = p.enabled.basic ? p.saturation : 0, vib = p.enabled.basic ? p.vibrance : 0;
    const bool hsl = p.enabled.hsl && (anyNonZero(p.hslHue) || anyNonZero(p.hslSat) || anyNonZero(p.hslLum));
    const bool skin = skinActive(p) && (p.skinHue != 0 || p.skinSat != 0 || p.skinLum != 0 || p.skinUniHue > 0 || p.skinUniSat > 0);
    const auto tHue = bandTable(p.hslHue), tSat = bandTable(p.hslSat), tLum = bandTable(p.hslLum);
    const bool hasHue = anyNonZero(p.hslHue), hasSat = anyNonZero(p.hslSat), hasLum = anyNonZero(p.hslLum);
    // 肌色の重み（色相 0..360）
    static const std::vector<float> skinTab = [] {
        std::vector<float> t(361);
        for (int i = 0; i <= 360; ++i) t[i] = std::exp(-sq((i - 28.0f) / 18.0f));
        return t;
    }();

    // リニア値 → (ガンマ → トーン LUT → カラーホイール) を 1 枚の表にまとめる。
    // 暗部の精度を保つため、表は √x で引く。
    constexpr int kTabN = 16384;
    const float sMax = std::sqrt(std::pow(kLutMax, kGamma));
    const float sScale = (kTabN - 1) / sMax;
    std::array<std::vector<float>, 3> tab;
    for (int c = 0; c < 3; ++c) {
        tab[c].resize(kTabN);
        for (int k = 0; k < kTabN; ++k) {
            const float sv = k / sScale;
            const float e = std::pow(sv * sv, 1.0f / kGamma);
            const int idx = std::clamp(static_cast<int>(e * kIdx), 0, kLutN - 1);
            float x = lut.t[c + 1][idx];
            if (wheels) {
                x = std::max(x, 0.0f);
                x = x * G[c] + L[c] * (1.0f - std::clamp(x, 0.0f, 1.0f));
                x = std::pow(std::max(x, 0.0f), E[c]) + O[c];
            }
            tab[c][k] = x;
        }
    }

    // 部分補正
    const bool local = localActive(p);
    std::unique_ptr<MaskSet> masks;
    if (local) masks = std::make_unique<MaskSet>(p, w, h, map);
    const int nMask = masks ? masks->count() : 0;
    const int showSlot = masks && p.showMaskIndex >= 0 ? masks->slotOf(p.showMaskIndex) : -1;
    std::vector<float> showVals(showSlot >= 0 ? size_t(w) * h : 0);

    // GPU があれば、ここから肌の空間処理・シャープまでをまとめて GPU で
    bool gpuDone = false;
    if (gpuEnabled() && gpuBackend()) {
        MainStage st;
        st.lin = &lin;
        st.p = &p;
        for (int c = 0; c < 3; ++c) st.gains[c] = gains[c];
        st.camMat = camMat;
        st.tab = &tab;
        st.sScale = sScale;
        st.sat = sat;
        st.vib = vib;
        st.skinTab = &skinTab;
        st.hsl = hsl;
        st.hasHue = hasHue;
        st.hasSat = hasSat;
        st.hasLum = hasLum;
        st.tHue = &tHue;
        st.tSat = &tSat;
        st.tLum = &tLum;
        st.skin = skin;
        st.masks = masks.get();
        st.map = map;
        st.showSlot = showSlot;
        st.showVals = &showVals;
        st.skinSpatial = o.detail && p.enabled.skin && (p.skinUniLum > 0 || p.skinSmooth > 0);
        st.detail = o.detail && p.enabled.detail && p.sharpenAmount > 0 && p.sharpenRadius * o.scale >= 0.3f;
        st.scale = o.scale;
        gpuDone = gpuBackend()->runMain(st, g);
    }
    if (!gpuDone) {
    parallelFor(h, [&](int y0, int y1) {
        float mv[32];
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
            float v[3], xin[3];
            for (int c = 0; c < 3; ++c) xin[c] = lin.data[i * 3 + c] * gains[c];
            if (camMat) {
                const float a = xin[0], b = xin[1], d = xin[2];
                for (int c = 0; c < 3; ++c) xin[c] = CM[c * 3] * a + CM[c * 3 + 1] * b + CM[c * 3 + 2] * d;
            }
            if (nMask > 0) {
                const float disp[3] = {std::sqrt(std::max(xin[0], 0.0f)), std::sqrt(std::max(xin[1], 0.0f)),
                                       std::sqrt(std::max(xin[2], 0.0f))};
                masks->eval(int(i % w), int(i / w), disp, mv);
                float lg[3];
                localLinearGains(*masks, mv, lg);
                for (int c = 0; c < 3; ++c) xin[c] *= lg[c];
                if (showSlot >= 0) showVals[i] = mv[showSlot];
            }
            for (int c = 0; c < 3; ++c) {
                const float x = std::max(xin[c], 0.0f);
                const int k = std::min(static_cast<int>(std::sqrt(x) * sScale + 0.5f), kTabN - 1);
                v[c] = tab[c][k];
            }
            if (sat != 0 || vib != 0) {
                const float Y = v[0] * kLuma[0] + v[1] * kLuma[1] + v[2] * kLuma[2];
                float f = 1.0f + sat / 100.0f;
                if (vib != 0) {
                    float hh, ss, vv;
                    rgbToHsv(std::clamp(v[0], 0.f, 1.f), std::clamp(v[1], 0.f, 1.f), std::clamp(v[2], 0.f, 1.f), hh, ss, vv);
                    const float skin = skinTab[static_cast<int>(hh)] * std::min(ss * 3, 1.0f);
                    float vf = vib / 100.0f * (1 - ss) * (1 - 0.6f * skin) * (vib > 0 ? 1.2f : 1.0f);
                    f += std::max(vf, -1.0f);
                }
                for (float& c : v) c = Y + (c - Y) * f;
            }
            if (hsl) {
                float hh, ss, vv;
                rgbToHsv(std::clamp(v[0], 0.f, 1.f), std::clamp(v[1], 0.f, 1.f), std::clamp(v[2], 0.f, 1.f), hh, ss, vv);
                const int bi = std::clamp(static_cast<int>(hh * 2), 0, 720);
                const float sw = std::min(ss * 2.0f, 1.0f);
                if (hasHue) hh = std::fmod(hh + tHue[bi] * 0.3f * sw + 360.0f, 360.0f);
                if (hasSat) ss = std::clamp(ss * (1 + tSat[bi] / 100.0f), 0.0f, 1.0f);
                if (hasLum) vv = std::clamp(vv * (1 + tLum[bi] * 0.006f * sw), 0.0f, 1.0f);
                hsvToRgb(hh, ss, vv, v[0], v[1], v[2]);
            }
            if (nMask > 0) localDisplayAdjust(*masks, mv, v);
            if (skin) {
                float hh, ss, vv;
                rgbToHsv(std::clamp(v[0], 0.f, 1.f), std::clamp(v[1], 0.f, 1.f), std::clamp(v[2], 0.f, 1.f), hh, ss, vv);
                const float m = skinMask(p, hh, ss, vv);
                if (m > 0) {
                    skinAdjust(p, m, hh, ss, vv);
                    hsvToRgb(hh, ss, vv, v[0], v[1], v[2]);
                }
            }
            for (int c = 0; c < 3; ++c) g.data[i * 3 + c] = std::clamp(v[c], 0.0f, 1.0f);
        }
    });
    if (o.detail) skinLumaUniformity(g, p, o.scale);

    if (o.detail && p.enabled.detail) {
        applyDetail(g, p, o.scale);
        for (float& x : g.data) x = std::clamp(x, 0.0f, 1.0f);
    }
    }  // !gpuDone
    if (p.skinMaskView) skinMaskOverlay(g, p);
    if (showSlot >= 0) {
        // 選んでいるマスクの範囲を赤く重ねる
        for (size_t i = 0; i < showVals.size(); ++i) {
            const float a = showVals[i] * 0.55f;
            float* c = &g.data[i * 3];
            c[0] = c[0] * (1 - a) + 1.0f * a;
            c[1] = c[1] * (1 - a) + 0.15f * a;
            c[2] = c[2] * (1 - a) + 0.15f * a;
        }
    }
    if (p.enabled.crop) {
        g = rotate(g, p.cropAngle);
        if (o.doCrop && (p.cropW < 0.9999f || p.cropH < 0.9999f || p.cropX > 1e-4f || p.cropY > 1e-4f))
            g = cropImage(g, p);
    }
    return g;
}

std::pair<int, int> outputSize(const Params& p, int w, int h) {
    if (!p.enabled.crop) return {w, h};
    int x0 = static_cast<int>(std::lround(p.cropX * w)), y0 = static_cast<int>(std::lround(p.cropY * h));
    int x1 = static_cast<int>(std::lround((p.cropX + p.cropW) * w));
    int y1 = static_cast<int>(std::lround((p.cropY + p.cropH) * h));
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(w, std::max(x0 + 1, x1));
    y1 = std::min(h, std::max(y0 + 1, y1));
    return {x1 - x0, y1 - y0};
}

bool regionSupported(const Params& p) {
    // 回転があると見えている範囲と元画像の範囲が対応しないので、全体を作り直す
    return !(p.enabled.crop && std::abs(p.cropAngle) >= 1e-3f);
}

ImageF developRegion(const ImageF& lin, const Params& pIn, const std::array<float, 3>& asShot, int rx, int ry, int rw,
                     int rh, float scale, const LensInfo* lens) {
    scale = std::clamp(scale, 0.05f, 1.0f);
    const int W = lin.width, H = lin.height;
    int ox = 0, oy = 0;
    if (pIn.enabled.crop) {
        ox = std::max(0, static_cast<int>(std::lround(pIn.cropX * W)));
        oy = std::max(0, static_cast<int>(std::lround(pIn.cropY * H)));
    }
    // ぼかし系の処理が端で変わらないよう、のりしろを付けて現像してから切り取る
    float rad = 0;
    if (pIn.enabled.detail) {
        rad = std::max(rad, pIn.sharpenRadius * 4 + 2);
        rad = std::max(rad, float(denoiseMargin(pIn)));
    }
    {
        rad = std::max(rad, float(skinMargin(pIn)));
        if (pIn.enabled.local) rad = std::max(rad, float(spotMargin(pIn, W)));
    }
    const int m = static_cast<int>(std::ceil(rad));
    const int x0 = std::clamp(ox + rx - m, 0, W), y0 = std::clamp(oy + ry - m, 0, H);
    const int x1 = std::clamp(ox + rx + rw + m, 0, W), y1 = std::clamp(oy + ry + rh + m, 0, H);
    ImageF sub;
    if (x1 <= x0 || y1 <= y0) return sub;
    Params p = pIn;
    static const LensInfo none;
    if (lensActive(p, lens) || (p.enabled.lens && (std::abs(p.lensManualDistortion) > 0.5f ||
                                                   std::abs(p.lensManualVignetting) > 0.5f))) {
        // レンズ補正は元画像の座標で計算できるので、範囲だけ作る
        sub = lensRemapRegion(lin, p, lens ? *lens : none, x0, y0, x1 - x0, y1 - y0);
    } else {
        sub.resize(x1 - x0, y1 - y0);
        for (int y = y0; y < y1; ++y) std::copy(lin.px(x0, y), lin.px(x1, y), sub.px(0, y - y0));
    }
    p.enabled.lens = false;
    p.enabled.crop = false;
    DevelopOptions o;
    o.scale = 1.0f;
    double k = 1.0;
    if (scale < 0.999f) {
        const int edge = std::max(1, static_cast<int>(std::lround(std::max(sub.width, sub.height) * scale)));
        sub = downscale(sub, edge);
        k = static_cast<double>(sub.width) / (x1 - x0);
        o.scale = static_cast<float>(k);
    }
    // 部分補正・スポット修復のための座標の対応
    o.map.nx0 = double(x0) / W;
    o.map.ny0 = double(y0) / H;
    o.map.dx = 1.0 / (k * W);
    o.map.dy = 1.0 / (k * H);
    o.map.fullW = W;
    o.map.fullH = H;
    if (pIn.enabled.local && !pIn.spots.empty()) {
        const bool lensOn = lensActive(pIn, lens) || (pIn.enabled.lens && (std::abs(pIn.lensManualDistortion) > 0.5f ||
                                                                           std::abs(pIn.lensManualVignetting) > 0.5f));
        const Params pl = pIn;
        o.fetch = [&lin, pl, lens, lensOn, W, H, k](double nx, double ny, int w, int h) {
            // 元画像（フル解像度）から取り出して、いまの縮尺に合わせる
            const int fx = int(std::floor(nx * W)), fy = int(std::floor(ny * H));
            const int fw = std::max(1, int(std::ceil(w / k))), fh = std::max(1, int(std::ceil(h / k)));
            ImageF part;
            const int cx0 = std::clamp(fx, 0, W - 1), cy0 = std::clamp(fy, 0, H - 1);
            const int cx1 = std::clamp(fx + fw, 1, W), cy1 = std::clamp(fy + fh, 1, H);
            static const LensInfo none;
            if (lensOn) part = lensRemapRegion(lin, pl, lens ? *lens : none, cx0, cy0, cx1 - cx0, cy1 - cy0);
            else {
                part.resize(cx1 - cx0, cy1 - cy0);
                for (int y = cy0; y < cy1; ++y) std::copy(lin.px(cx0, y), lin.px(cx1, y), part.px(0, y - cy0));
            }
            ImageF out;
            out.resize(w, h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int sx = std::clamp(int(fx + (x + 0.5) / k) - cx0, 0, part.width - 1);
                    const int sy = std::clamp(int(fy + (y + 0.5) / k) - cy0, 0, part.height - 1);
                    std::copy(part.px(sx, sy), part.px(sx, sy) + 3, out.px(x, y));
                }
            return out;
        };
    }
    // ノイズ量は部分ではなく画像全体で測る（場所によって効き方が変わらないように）
    NoiseLevel nl;
    if (denoiseActive(p)) {
        nl = noiseOf(lin);
        const float kk = static_cast<float>(k);
        nl.y *= kk;
        nl.c1 *= kk;
        nl.c2 *= kk;
        o.noise = &nl;
    }
    const ImageF g = develop(sub, p, asShot, o);
    const int cx = static_cast<int>(std::lround((std::clamp(ox + rx, x0, x1) - x0) * k));
    const int cy = static_cast<int>(std::lround((std::clamp(oy + ry, y0, y1) - y0) * k));
    const int cw = std::min(static_cast<int>(std::lround(rw * k)), g.width - cx);
    const int ch = std::min(static_cast<int>(std::lround(rh * k)), g.height - cy);
    ImageF out;
    if (cw <= 0 || ch <= 0) return out;
    out.resize(cw, ch);
    for (int y = 0; y < ch; ++y) std::copy(g.px(cx, cy + y), g.px(cx + cw, cy + y), out.px(0, y));
    return out;
}

std::vector<std::uint8_t> toRgb8(const ImageF& g) {
    std::vector<std::uint8_t> out(g.data.size());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint8_t>(std::clamp(g.data[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return out;
}

std::vector<std::uint16_t> toRgb16(const ImageF& g) {
    std::vector<std::uint16_t> out(g.data.size());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint16_t>(std::clamp(g.data[i], 0.0f, 1.0f) * 65535.0f + 0.5f);
    return out;
}

}  // namespace rd
