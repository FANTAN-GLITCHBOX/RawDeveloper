#include "engine/Auto.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <vector>

#include "engine/Color.h"
#include "engine/Pipeline.h"
#include <tuple>
#include <cstdio>
#include <cstdlib>

namespace rd {

namespace {

constexpr double kMaxMiredShift = 110.0;

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

// ---------------------------------------------------------------- WB

struct WbEstimate {
    double temp = 5500, tint = 0;  // 光源（これに合わせると灰色が灰色になる）
    double confidence = 0;         // 0..1：灰色らしい部分がどれだけ見つかったか
};

// 灰色らしい部分（彩度の低い、白飛びしていない、暗すぎない画素）から光源の色を推定
std::optional<WbEstimate> estimateWb(const ImageF& small, const std::array<float, 3>& asShot) {
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
    // 撮影時の色のままで彩度が低い画素の割合＝信頼度の目安（色かぶりが強いと彩度が上がるので、閾値は緩め）
    double nearGray = 0;
    for (float v : sat) nearGray += v < 0.18f;
    nearGray /= valid.size();

    const float thr = percentile(sat, 12);
    double gm[3] = {0, 0, 0}, ws = 0, wm[3] = {0, 0, 0};
    // 明るい灰色（白い壁・雲など）を重視
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
    for (int c = 0; c < 3; ++c) est[c] = std::max(0.8 * gm[c] / ws + 0.2 * wm[c] / valid.size(), 1e-5);
    double corr[3] = {1.0 / est[0], 1.0 / est[1], 1.0 / est[2]};
    const double c1 = corr[1];
    for (double& c : corr) c /= c1;
    const std::array<float, 3> gains{static_cast<float>(corr[0] * asShot[0]), static_cast<float>(corr[1] * asShot[1]),
                                     static_cast<float>(corr[2] * asShot[2])};
    WbEstimate e;
    std::tie(e.temp, e.tint) = fitTempTint(gains);
    // 補正したあとに灰色になる画素の割合。少ない（森・空・夕焼けなど単色が多い）ほど信頼しない
    double gray2 = 0;
    for (size_t k = 0; k < valid.size(); ++k) {
        const float* p = &x[valid[k] * 3];
        float g[3];
        for (int c = 0; c < 3; ++c) g[c] = std::pow(std::max(float(p[c] * corr[c]), 0.0f), 1.0f / kGamma);
        const float gmax = std::max({g[0], g[1], g[2]}), gmin = std::min({g[0], g[1], g[2]});
        gray2 += (gmax - gmin) / std::max(gmax, 1e-4f) < 0.10f;
    }
    gray2 /= valid.size();
    // 補正後に灰色になるのは「単色の被写体を灰色にしてしまった」場合もあるので、電球色のような強いかぶりのときだけ使う
    const double g = e.temp < 4200 ? std::max(gray2 * 0.8, nearGray) : nearGray;
    e.confidence = std::clamp((g - 0.04) / 0.20, 0.0, 1.0);
    return e;
}

// ---------------------------------------------------------------- シーン

struct Scene {
    double ev100 = std::nan("");  // 撮影時の明るさ（EV100）。不明なら NaN
    bool night = false, dim = false, bright = false;
    bool backlit = false, highKey = false, warmLight = false, sunset = false;
    double bias = 0;  // カメラの露出補正
};

// ---------------------------------------------------------------- 仕上がりの評価

// 縮小画像を実際のトーン処理に通し、明るさの分布から「よい仕上がり」からのずれを点数にする
struct ToneEval {
    const ImageF* img = nullptr;
    std::array<float, 3> gains{1, 1, 1};
    std::vector<float> weight;  // 画素ごとの重要度（中央・主役寄り）
    double totalW = 1;
    Params base;
    Scene scene;
    double midTarget = 0.42;     // 重要部分の明るさの中央値（表示値）
    double hiAllow = 0.003;      // 白飛びしてよい割合
    double evPrior = 0;          // 露出の目安（ここから大きく離れると減点）

    struct Stats {
        double med = 0, p005 = 0, p10 = 0, p995 = 0, sd = 0, clipHi = 0, clipLo = 0, crushed = 0;
    };

    Stats measure(const Params& p) const {
        Params probe = base;
        probe.exposure = p.exposure;
        probe.contrast = p.contrast;
        probe.highlights = p.highlights;
        probe.shadows = p.shadows;
        probe.whites = p.whites;
        probe.blacks = p.blacks;
        probe.saturation = probe.vibrance = 0;
        const ToneLut lut = toneLut(probe);
        const float evf = std::pow(2.0f, p.exposure);
        constexpr float kIdx = (kLutN - 1) / kLutMax;
        constexpr int kBins = 1024;
        std::array<double, kBins> hw{}, hu{};
        double s = 0, s2 = 0, nu = 0, hiC = 0, loC = 0;
        const size_t n = static_cast<size_t>(img->width) * img->height;
        for (size_t i = 0; i < n; ++i) {
            float v[3];
            float vmax = 0;
            for (int c = 0; c < 3; ++c) {
                const float x = std::max(img->data[i * 3 + c] * gains[c] * evf, 0.0f);
                const int idx = std::clamp(static_cast<int>(std::pow(x, 1.0f / kGamma) * kIdx), 0, kLutN - 1);
                v[c] = lut.t[c + 1][idx];
                vmax = std::max(vmax, v[c]);
            }
            const double y = std::clamp(lumOf(v), 0.0f, 1.0f);
            const int b = std::min(kBins - 1, static_cast<int>(y * kBins));
            hw[b] += weight[i];
            hu[b] += 1;
            s += y;
            s2 += y * y;
            nu += 1;
            hiC += vmax >= 0.995f;
            loC += y <= 0.004;
        }
        auto pct = [&](const std::array<double, kBins>& h, double total, double q) {
            double acc = 0;
            for (int b = 0; b < kBins; ++b) {
                acc += h[b];
                if (acc >= total * q) return (b + 0.5) / kBins;
            }
            return 1.0;
        };
        Stats st;
        st.med = pct(hw, totalW, 0.5);
        st.p10 = pct(hw, totalW, 0.10);
        st.p005 = pct(hu, nu, 0.005);
        st.p995 = pct(hu, nu, 0.995);
        const double mean = s / nu;
        st.sd = std::sqrt(std::max(0.0, s2 / nu - mean * mean));
        st.clipHi = hiC / nu;
        st.clipLo = loC / nu;
        // 暗部が真っ黒に近い割合（0.03 以下）
        double dark = 0;
        for (int b = 0; b < kBins * 3 / 100; ++b) dark += hw[b];
        st.crushed = dark / totalW;
        return st;
    }

    double cost(const Params& p) const {
        const Stats st = measure(p);
        auto sq = [](double v) { return v * v; };
        double c = 0;
        c += sq((st.med - midTarget) / 0.04);                                   // 主役の明るさ
        c += sq(std::max(0.0, st.clipHi - hiAllow) / 0.01) * 1.5;               // 白飛び
        c += sq(std::max(0.0, st.clipLo - 0.004) / 0.01);                       // 黒つぶれ
        if (!scene.highKey) c += sq(std::max(0.0, 0.93 - st.p995) / 0.05) * 0.6;  // 白がくすむ
        if (!scene.night) c += sq(std::max(0.0, st.p005 - 0.07) / 0.05) * 0.3;    // 黒が浮く
        c += sq(std::max(0.0, st.crushed - (scene.night ? 0.25 : 0.06)) / 0.05) * 0.6;  // 暗部がつぶれる
        // メリハリ：眠い（平坦な）ときだけ上げる。もともとメリハリがある写真は下げない
        const double sdMin = scene.highKey ? 0.12 : 0.17;
        c += sq(std::max(0.0, sdMin - st.sd) / 0.05) * 0.6;
        // 動かしすぎない（同じ見た目なら素直な値を選ぶ）
        c += 0.08 * (sq(p.contrast / 22) + sq(p.highlights / 60) + sq(p.shadows / 60) + sq(p.whites / 30) +
                     sq(p.blacks / 30));
        c += 0.15 * sq((p.exposure - evPrior) / 1.5);
        if (scene.night) c += sq(std::max(0.0, p.shadows - 35.0) / 20.0) + sq(std::max(0.0, std::abs(p.exposure - evPrior) - 0.8) / 0.3);
        // シャドウを大きく持ち上げてコントラストを下げると眠い画になる
        if (p.shadows > 30 && p.contrast < 0) c += sq(p.shadows * -p.contrast / 1500.0);
        return c;
    }
};

// 座標ごとに少しずつ動かして点数が下がる方へ（値の範囲はスライダーと同じ）
Params optimizeTone(const ToneEval& ev, Params p) {
    struct Var {
        float Params::*f;
        float step, lo, hi;
    };
    Var vars[] = {{&Params::exposure, 0.4f, -4, 4},     {&Params::contrast, 16, -25, 35},
                  {&Params::highlights, 24, -100, 0},  {&Params::shadows, 24, 0, 100},
                  {&Params::whites, 16, -50, 50},       {&Params::blacks, 16, -50, 50}};
    double best = ev.cost(p);
    for (int round = 0; round < 6; ++round) {
        for (int pass = 0; pass < 8; ++pass) {
            bool improved = false;
            for (auto& v : vars) {
                for (float dir : {1.0f, -1.0f}) {
                    Params q = p;
                    q.*v.f = std::clamp(p.*v.f + dir * v.step, v.lo, v.hi);
                    if (q.*v.f == p.*v.f) continue;
                    const double c = ev.cost(q);
                    if (c < best - 1e-6) {
                        best = c;
                        p = q;
                        improved = true;
                        break;
                    }
                }
            }
            if (!improved) break;
        }
        for (auto& v : vars) v.step *= 0.5f;
    }
    return p;
}

}  // namespace

Params autoParams(const ImageF& lin, const std::array<float, 3>& asShot, float strength, const Params& base,
                  bool wbOnly, const Metadata* meta, AutoInfo* info) {
    Params p = base;
    p.enabled.basic = true;
    const ImageF small = downscale(lin, 512);
    const float k = strength / 100.0f;

    // ---- シーンの手がかり（撮影情報）
    Scene sc;
    if (meta && meta->iso > 0 && meta->shutter > 0 && meta->aperture > 0) {
        sc.ev100 = std::log2(double(meta->aperture) * meta->aperture / meta->shutter) - std::log2(meta->iso / 100.0);
        sc.night = sc.ev100 < 5.0;
        sc.dim = sc.ev100 < 9.0;
        sc.bright = sc.ev100 >= 12.5;
    }
    if (meta && meta->hasBias) sc.bias = meta->exposureBias;

    // ---- 1) WB：雰囲気を残しつつ、自然な光の色の範囲で
    const auto [t0, tint0] = fitTempTint(asShot);
    const auto est = estimateWb(small, asShot);
    std::array<float, 3> gains = asShot;
    if (!est || k <= 0) {
        p.wbMode = "as_shot";
        p.temp = static_cast<float>(t0);
        p.tint = static_cast<float>(tint0);
    } else {
        const double m0 = 1e6 / t0;
        double m = 1e6 / est->temp;
        // 暖かい光（電球・夕日）は暖かさを少し残す。日中なのにとても暖かい＝夕景とみなして多めに残す
        sc.warmLight = est->temp < 4200;
        sc.sunset = sc.warmLight && !std::isnan(sc.ev100) && sc.ev100 >= 9.0;
        const double mNeutral = 1e6 / 5200.0;
        if (sc.warmLight) m -= (m - mNeutral) * (sc.sunset ? 0.8 : 0.30);
        else if (est->temp > 7500) m -= (m - mNeutral) * 0.15;  // 日陰の青さも少し残す
        double tint = est->tint;
        // 信頼度で撮影時の設定と混ぜる
        const double w = k * (0.35 + 0.65 * est->confidence);
        m = m0 + std::clamp(m - m0, -kMaxMiredShift, kMaxMiredShift) * w;
        // 緑・マゼンタ方向は間違えると目立つので、自信があるときだけ大きく動かす
        tint = tint0 + std::clamp(tint - tint0, -14.0, 14.0) * k * est->confidence;
        if (std::abs(m - m0) < 4 && std::abs(tint - tint0) < 2) {
            p.wbMode = "as_shot";
            p.temp = static_cast<float>(t0);
            p.tint = static_cast<float>(tint0);
        } else {
            p.wbMode = "custom";
            p.temp = static_cast<float>(std::round(1e6 / m / 10.0) * 10.0);
            p.tint = static_cast<float>(std::round(tint));
            gains = wbGains(p.temp, p.tint);
        }
    }
    if (wbOnly) return p;

    // ---- 2) シーンを見分ける（明るさの分布）
    const ImageF tiny = downscale(small, 256);
    const int w = tiny.width, h = tiny.height;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> lum(n);
    for (size_t i = 0; i < n; ++i) {
        float px[3];
        for (int c = 0; c < 3; ++c) px[c] = tiny.data[i * 3 + c] * gains[c];
        lum[i] = lumOf(px);
    }
    // 中央と周辺・上部の明るさの差で逆光を見分ける
    auto regionMedian = [&](double x0, double y0, double x1, double y1) {
        std::vector<float> v;
        for (int y = int(y0 * h); y < int(y1 * h); ++y)
            for (int x = int(x0 * w); x < int(x1 * w); ++x) v.push_back(std::log2(std::max(lum[size_t(y) * w + x], 1e-5f)));
        return v.empty() ? 0.0 : double(percentile(v, 50));
    };
    const double center = regionMedian(0.25, 0.25, 0.75, 0.8);
    const double top = regionMedian(0.0, 0.0, 1.0, 0.3);
    const float p995 = percentile(lum, 99.5);
    const float pMed = percentile(lum, 50);
    sc.backlit = top - center > 2.0 && std::log2(std::max(p995, 1e-5f)) - center > 3.5;
    // 雪・白い背景：明るい場所で、全体が明るく色が薄い
    double satMean = 0;
    for (size_t i = 0; i < n; ++i) {
        const float* q = &tiny.data[i * 3];
        const float a = q[0] * gains[0], b = q[1] * gains[1], c = q[2] * gains[2];
        const float mx = std::max({a, b, c}), mn = std::min({a, b, c});
        satMean += (mx - mn) / std::max(mx, 1e-4f);
    }
    satMean /= n;
    const double medEv = std::log2(std::max(pMed, 1e-5f)), hiEv = std::log2(std::max(p995, 1e-5f));
    sc.highKey = satMean < 0.18 && hiEv - medEv < 1.6 && (std::isnan(sc.ev100) ? true : sc.ev100 >= 11.0);

    // ---- 3) 目標を決めて、実際の仕上がりを見ながら値を探す
    ToneEval te;
    te.img = &tiny;
    te.gains = gains;
    te.base = base;
    te.scene = sc;
    te.weight.resize(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dx = (x - w / 2.0) / (w * 0.45), dy = (y - h * 0.55) / (h * 0.45);
            double wt = std::exp(-(dx * dx + dy * dy));
            if (sc.backlit) wt = std::pow(wt, 2.0) * (lum[size_t(y) * w + x] < std::exp2(center + 1.5) ? 1.0 : 0.15);
            te.weight[size_t(y) * w + x] = float(wt + 0.02);
        }
    te.totalW = std::accumulate(te.weight.begin(), te.weight.end(), 0.0);
    te.midTarget = sc.night ? 0.22 : sc.highKey ? 0.66 : sc.dim ? 0.40 : 0.43;
    if (sc.sunset) te.midTarget -= 0.04;  // 夕景は少し暗めの方が雰囲気が出る
    te.midTarget = std::clamp(te.midTarget + sc.bias * 0.06, 0.22, 0.72);  // 撮影者の露出補正の意図を半分くらい残す
    te.hiAllow = sc.backlit ? 0.04 : sc.highKey ? 0.006 : 0.003;
    // 露出の目安：重み付き中央値を中間グレーへ
    {
        std::vector<std::pair<float, float>> lw(n);
        for (size_t i = 0; i < n; ++i) lw[i] = {std::log2(std::max(lum[i], 1e-5f)), te.weight[i]};
        std::sort(lw.begin(), lw.end());
        double acc = 0, med = lw.back().first;
        for (auto& e : lw) {
            acc += e.second;
            if (acc >= te.totalW * 0.5) {
                med = e.first;
                break;
            }
        }
        te.evPrior = std::clamp(std::log2(sc.highKey ? 0.32 : sc.night ? 0.07 : 0.16) - med, -4.0, 4.0);
        if (sc.night) te.evPrior = std::min(te.evPrior, 1.5);
    }
    Params start = p;
    start.exposure = float(te.evPrior);
    start.contrast = start.highlights = start.shadows = start.whites = start.blacks = 0;
    const Params opt = optimizeTone(te, start);
    if (std::getenv("RD_AUTO_DEBUG")) {
        for (const Params* q : {static_cast<const Params*>(&start), &opt}) {
            const auto st = te.measure(*q);
            std::fprintf(stderr, "  ev %.2f c %.0f h %.0f s %.0f w %.0f b %.0f | med %.3f p005 %.3f p10 %.3f p995 %.3f sd %.3f hi %.4f lo %.4f cr %.3f cost %.3f\n",
                         q->exposure, q->contrast, q->highlights, q->shadows, q->whites, q->blacks, st.med, st.p005, st.p10, st.p995,
                         st.sd, st.clipHi, st.clipLo, st.crushed, te.cost(*q));
        }
    }
    p.exposure = static_cast<float>(std::round(opt.exposure * k * 100.0) / 100.0);
    p.contrast = static_cast<float>(std::round(opt.contrast * k));
    p.highlights = static_cast<float>(std::round(opt.highlights * k));
    p.shadows = static_cast<float>(std::round(opt.shadows * k));
    p.whites = static_cast<float>(std::round(opt.whites * k));
    p.blacks = static_cast<float>(std::round(opt.blacks * k));

    // 自然な彩度：くすんだ写真だけ少し上げる（夕景・夜景は控えめ）
    const double vib = std::clamp((0.30 - satMean) * 120.0, 0.0, sc.sunset || sc.night ? 10.0 : 30.0);
    p.vibrance = static_cast<float>(std::round(vib * k));
    p.autoStrength = strength;
    if (info) {
        info->scene.clear();
        auto add = [&](const char* t) { info->scene += (info->scene.empty() ? "" : "・") + std::string(t); };
        if (sc.backlit) add("逆光");
        if (sc.highKey) add("明るい背景");
        if (sc.night) add("夜景");
        else if (sc.sunset) add("夕景");
        else if (sc.warmLight) add("電球色の光");
        if (sc.bias != 0) add(sc.bias > 0 ? "露出補正＋" : "露出補正−");
    }
    return p;
}

}  // namespace rd
