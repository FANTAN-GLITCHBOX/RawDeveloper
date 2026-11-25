#include "engine/Skin.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Color.h"
#include "engine/Parallel.h"
#include "engine/Pipeline.h"
#include "facedetectcnn.h"

namespace rd {

namespace {
float ramp(float x, float a, float b) { return std::clamp((x - a) / (b - a), 0.0f, 1.0f); }
float hueDiff(float a, float b) {
    float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
    return d;
}
}  // namespace

bool skinActive(const Params& p) {
    if (!p.enabled.skin) return false;
    return p.skinHue != 0 || p.skinSat != 0 || p.skinLum != 0 || p.skinUniHue > 0 || p.skinUniSat > 0 || p.skinUniLum > 0 ||
           p.skinMaskView;
}

float skinMask(const Params& p, float h, float s, float v) {
    // 中心の色（スポイト・自動で決める）の近くだけ。木・壁・服など、色相が近くても彩度が違う物は外れやすいように
    const float f = std::clamp(p.skinRange / 50.0f, 0.2f, 2.0f);
    const float width = 12.0f * f;
    const float d = std::abs(hueDiff(h, p.skinCenterHue));
    const float mh = ramp(width - d, 0.0f, width * 0.5f);
    if (mh <= 0) return 0;
    const float sc = std::clamp(p.skinCenterSat, 0.08f, 0.85f);
    const float r = s / sc;
    const float lo = 1.0f - 0.32f * f, hi = 1.0f + 0.45f * f;
    const float ms = ramp(r, lo - 0.2f, lo) * (1.0f - ramp(r, hi, hi + 0.4f));
    const float mv = ramp(v, 0.15f, 0.28f) * (1.0f - ramp(v, 0.985f, 1.0f));
    return mh * ms * mv;
}

void skinAdjust(const Params& p, float m, float& h, float& s, float& v) {
    if (m <= 0) return;
    // 均一化：中心の色に寄せる
    const float uh = p.skinUniHue / 100.0f * m, us = p.skinUniSat / 100.0f * m;
    h += hueDiff(p.skinCenterHue, h) * uh;
    s += (p.skinCenterSat - s) * us * 0.8f;
    // 調整：色相 ±15°（−＝赤み、＋＝黄み）、彩度 ±100%、明るさ ±30%
    h += p.skinHue * 0.15f * m;
    s *= 1.0f + p.skinSat / 100.0f * m;
    v *= 1.0f + p.skinLum / 100.0f * 0.3f * m;
    h = std::fmod(h + 360.0f, 360.0f);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
}

int skinMargin(const Params& p) { return p.enabled.skin && p.skinUniLum > 0 ? 40 : 0; }

void skinLumaUniformity(ImageF& g, const Params& p, float scale) {
    if (!p.enabled.skin || p.skinUniLum <= 0) return;
    const int w = g.width, h = g.height;
    const size_t n = size_t(w) * h;
    std::vector<float> Y(n), M(n);
    parallelFor(h, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
            const float* c = &g.data[i * 3];
            float hh, ss, vv;
            rgbToHsv(std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f), std::clamp(c[2], 0.f, 1.f), hh, ss, vv);
            M[i] = skinMask(p, hh, ss, vv);
            Y[i] = c[0] * kLuma[0] + c[1] * kLuma[1] + c[2] * kLuma[2];
        }
    });
    // 肌の部分だけの平均（肌以外の明るさが混ざらないよう、マスクで重み付け）
    std::vector<float> YM(n), Mb = M;
    for (size_t i = 0; i < n; ++i) YM[i] = Y[i] * M[i];
    const float sigma = std::max(1.0f, 12.0f * scale);  // フル解像度で 12px：シミ・ムラの大きさ
    gaussianBlur(YM, w, h, sigma);
    gaussianBlur(Mb, w, h, sigma);
    const float k = p.skinUniLum / 100.0f * 0.85f;
    parallelFor(h, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * w; i < size_t(y1) * w; ++i) {
            if (M[i] <= 0 || Mb[i] < 1e-3f) continue;
            const float local = YM[i] / Mb[i];
            const float d = (local - Y[i]) * k * M[i];
            // 明るさだけ動かす（色はそのまま）
            for (int c = 0; c < 3; ++c) g.data[i * 3 + c] = std::clamp(g.data[i * 3 + c] + d, 0.0f, 1.0f);
        }
    });
}

void skinMaskOverlay(ImageF& g, const Params& p) {
    const size_t n = size_t(g.width) * g.height;
    for (size_t i = 0; i < n; ++i) {
        float* c = &g.data[i * 3];
        float hh, ss, vv;
        rgbToHsv(std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f), std::clamp(c[2], 0.f, 1.f), hh, ss, vv);
        const float m = skinMask(p, hh, ss, vv);
        const float y = (c[0] * kLuma[0] + c[1] * kLuma[1] + c[2] * kLuma[2]) * 0.45f;
        const float o[3] = {1.0f, 0.55f, 0.1f};
        for (int k = 0; k < 3; ++k) c[k] = y * (1 - m) + (y * 0.4f + o[k] * 0.6f) * m;
    }
}

std::vector<FaceBox> detectFaces(const ImageF& display) {
    std::vector<FaceBox> out;
    const ImageF small = downscale(display, 1024);
    const int w = small.width, h = small.height;
    if (w < 32 || h < 32) return out;
    std::vector<unsigned char> bgr(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c)
            bgr[i * 3 + (2 - c)] = static_cast<unsigned char>(std::clamp(small.data[i * 3 + c], 0.0f, 1.0f) * 255 + 0.5f);
    std::vector<unsigned char> buf(FACEDETECTION_RESULT_BUFFER_SIZE);
    int* res = facedetect_cnn(buf.data(), bgr.data(), w, h, w * 3);
    if (!res) return out;
    for (int i = 0; i < *res; ++i) {
        const short* q = reinterpret_cast<const short*>(res + 1) + FACEDETECTION_RESULT_STRIDE_SHORTS * i;
        if (q[0] < 60) continue;  // 自信の低いものは使わない
        out.push_back({float(q[1]) / w, float(q[2]) / h, float(q[3]) / w, float(q[4]) / h, float(q[0])});
    }
    return out;
}

SkinStats analyzeSkin(const ImageF& d) {
    // 顔が見つかれば、顔の中央（目より下・口より上あたり）の肌の色を代表にする。服や木の色に引っ張られない
    {
        const auto faces = detectFaces(d);
        std::vector<float> hs, ss, vs;
        for (const auto& f : faces) {
            const int x0 = int((f.x + f.w * 0.2f) * d.width), x1 = int((f.x + f.w * 0.8f) * d.width);
            const int y0 = int((f.y + f.h * 0.35f) * d.height), y1 = int((f.y + f.h * 0.75f) * d.height);
            for (int y = std::max(0, y0); y < std::min(d.height, y1); ++y)
                for (int x = std::max(0, x0); x < std::min(d.width, x1); ++x) {
                    const float* c = d.px(x, y);
                    float hh, s2, v;
                    rgbToHsv(std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f), std::clamp(c[2], 0.f, 1.f), hh, s2, v);
                    if (hh > 180) hh -= 360;
                    // 目・眉・唇・マスク・メガネの反射などを外す
                    if (hh < -15 || hh > 45 || s2 < 0.08f || v < 0.2f || v > 0.98f) continue;
                    hs.push_back(hh);
                    ss.push_back(s2);
                    vs.push_back(v);
                }
        }
        if (hs.size() >= 30) {
            auto med = [](std::vector<float>& v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            SkinStats st;
            st.faces = int(faces.size());
            st.hue = std::fmod(med(hs) + 360.0f, 360.0f);
            st.sat = med(ss);
            st.val = med(vs);
            st.fraction = float(hs.size()) / (size_t(d.width) * d.height);
            return st;
        }
    }
    // 1) 肌らしい色を YCbCr の定番の範囲で拾う  2) 色相×彩度の分布でいちばん多い所を肌の色とする
    //    （木・壁など、肌に近い色が広く写っていても、人の肌のまとまりを選びやすい）
    SkinStats st;
    constexpr int HB = 60, SB = 40;  // 色相 -10..50°（1°刻み）、彩度 0..0.8
    std::vector<float> hist(HB * SB, 0.0f);
    struct Px {
        float h, s, v;
    };
    std::vector<Px> cand;
    const size_t n = size_t(d.width) * d.height;
    for (size_t i = 0; i < n; ++i) {
        const float* c = &d.data[i * 3];
        const float R = std::clamp(c[0], 0.f, 1.f), G = std::clamp(c[1], 0.f, 1.f), B = std::clamp(c[2], 0.f, 1.f);
        const float Y = 0.299f * R + 0.587f * G + 0.114f * B;
        const float cb = 128 + 255 * (-0.1687f * R - 0.3313f * G + 0.5f * B);
        const float cr = 128 + 255 * (0.5f * R - 0.4187f * G - 0.0813f * B);
        if (!(cb > 77 && cb < 127 && cr > 133 && cr < 173 && R > G && G > B * 0.9f && Y > 0.2f && Y < 0.97f)) continue;
        float h, s, v;
        rgbToHsv(R, G, B, h, s, v);
        if (h > 180) h -= 360;
        const int hb = int(h + 10), sb = int(s / 0.02f);
        if (hb < 0 || hb >= HB || sb < 3 || sb >= SB) continue;
        hist[hb * SB + sb] += 1;
        cand.push_back({h, s, v});
    }
    st.fraction = n ? float(cand.size()) / n : 0;
    if (cand.size() < 50) return st;
    // ぼかして山を探す
    std::vector<float> sm(hist.size(), 0.0f);
    for (int a = 0; a < HB; ++a)
        for (int b = 0; b < SB; ++b) {
            float acc = 0;
            for (int da = -3; da <= 3; ++da)
                for (int db = -2; db <= 2; ++db) {
                    const int x = a + da, y = b + db;
                    if (x >= 0 && x < HB && y >= 0 && y < SB) acc += hist[x * SB + y] * std::exp(-(da * da) / 8.0f - (db * db) / 4.0f);
                }
            sm[a * SB + b] = acc;
        }
    const int best = int(std::max_element(sm.begin(), sm.end()) - sm.begin());
    const float ph = best / SB - 10 + 0.5f, ps = (best % SB + 0.5f) * 0.02f;
    // 山の近くの画素の平均で仕上げ
    double sh = 0, ss = 0, sv = 0, cnt = 0;
    for (const auto& q : cand)
        if (std::abs(q.h - ph) < 6 && std::abs(q.s - ps) < 0.08f) {
            sh += q.h;
            ss += q.s;
            sv += q.v;
            cnt += 1;
        }
    if (cnt < 20) return st;
    st.hue = std::fmod(float(sh / cnt) + 360.0f, 360.0f);
    st.sat = float(ss / cnt);
    st.val = float(sv / cnt);
    st.fraction = float(cnt / n);
    return st;
}

Params skinAuto(const Params& base, const SkinStats& st, int level) {
    Params p = base;
    p.enabled.skin = true;
    const float k = level == 1 ? 0.5f : level == 3 ? 1.4f : 1.0f;
    p.skinCenterHue = std::round(st.hue);
    p.skinCenterSat = std::round(st.sat * 100) / 100;
    p.skinRange = 50;
    // 色相：スキントーンラインへ（−＝赤み、＋＝黄み）
    const float dh = hueDiff(kSkinLineHue + 2.0f, st.hue);  // ライン上より少しだけ黄み寄りが自然に見える
    // 顔から測れたときはしっかり、色だけで推定したとき（服・木などを選んでいることもある）は控えめに
    const bool face = st.faces > 0;
    const float cap = face ? 35.0f : 25.0f;
    p.skinHue = std::round(std::clamp(dh / 0.15f * 0.5f * k, -cap, cap));
    // 彩度：派手すぎ・くすみすぎを目安の 0.38 付近へ（控えめに）
    const float ds = (0.38f / std::max(st.sat, 0.05f) - 1.0f) * 100.0f;
    p.skinSat = std::round(face ? std::clamp(ds * 0.6f * k, -35.0f, 20.0f) : std::clamp(ds * 0.4f * k, -25.0f, 15.0f));
    p.skinLum = std::round(std::clamp((0.72f - st.val) * 60.0f * k, -10.0f, 15.0f));
    p.skinUniHue = std::round(std::clamp(35.0f * k, 0.0f, 80.0f));
    p.skinUniSat = std::round(std::clamp(25.0f * k, 0.0f, 70.0f));
    p.skinUniLum = std::round(std::clamp(20.0f * k, 0.0f, 60.0f));
    return p;
}

}  // namespace rd
