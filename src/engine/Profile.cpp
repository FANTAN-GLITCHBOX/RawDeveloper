#include "engine/Profile.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/Color.h"
#include "engine/Pipeline.h"

namespace rd {

namespace {
float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

// 3×3 の連立方程式（正規方程式）を解く
bool solve3(const double A[3][3], const double b[3], double x[3]) {
    const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                       A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (std::abs(det) < 1e-12) return false;
    for (int k = 0; k < 3; ++k) {
        double M[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M[r][c] = c == k ? b[r] : A[r][c];
        x[k] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) /
               det;
    }
    return true;
}
}  // namespace

bool matchCamera(const ImageF& lin, const std::array<float, 3>& asShot, const ImageF& camRef, Params& p) {
    if (camRef.width < 32 || camRef.height < 32) return false;
    const ImageF t = downscale(lin, 256);
    const int w = t.width, h = t.height;
    const auto gains = wbFor(p, asShot);
    struct S {
        float o[3], c[3], w;
    };
    std::vector<S> px;
    px.reserve(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* q = camRef.px(std::min(camRef.width - 1, x * camRef.width / w), std::min(camRef.height - 1, y * camRef.height / h));
            const float* o = t.px(x, y);
            S s;
            for (int c = 0; c < 3; ++c) {
                s.o[c] = o[c] * gains[c];
                s.c[c] = q[c];
            }
            s.w = 1;
            px.push_back(s);
        }

    // 1) 色：明るさで割った色同士（トーンの違いの影響を受けにくい）を最小二乗で合わせる
    std::array<float, 9> M{1, 0, 0, 0, 1, 0, 0, 0, 1};
    {
        double A[3][3] = {}, B[3][3] = {};
        double n = 0;
        for (const S& s : px) {
            const float cm = std::max({s.c[0], s.c[1], s.c[2]}), cn = std::min({s.c[0], s.c[1], s.c[2]});
            if (cm > 0.94f || cn < 0.04f) continue;  // 白飛び・黒つぶれ寸前は使わない
            float cl[3];
            for (int c = 0; c < 3; ++c) cl[c] = srgbToLinear(s.c[c]);
            const float lo = s.o[0] * kLuma[0] + s.o[1] * kLuma[1] + s.o[2] * kLuma[2];
            const float lc = cl[0] * kLuma[0] + cl[1] * kLuma[1] + cl[2] * kLuma[2];
            if (lo <= 1e-5f || lc <= 1e-5f) continue;
            double on[3], cn2[3];
            for (int c = 0; c < 3; ++c) {
                on[c] = s.o[c] / lo;
                cn2[c] = cl[c] / lc;
            }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    A[r][c] += on[r] * on[c];
                    B[r][c] += on[r] * cn2[c];  // B[:,k] = Σ o * c_k
                }
            n += 1;
        }
        if (n < 200) return false;
        for (int k = 0; k < 3; ++k) {
            // 単位行列へ少し寄せる（極端な行列にならないよう）
            double AA[3][3], b[3], x[3];
            const double lam = 0.15 * n;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) AA[r][c] = A[r][c] + (r == c ? lam : 0);
                b[r] = B[r][k] + (r == k ? lam : 0);
            }
            if (!solve3(AA, b, x)) return false;
            for (int c = 0; c < 3; ++c) M[k * 3 + c] = float(std::clamp(x[c], -1.5, 2.5));
        }
        // 明るさ（灰色の輝度）は変えない
        const float gy = (M[0] + M[1] + M[2]) * kLuma[0] + (M[3] + M[4] + M[5]) * kLuma[1] + (M[6] + M[7] + M[8]) * kLuma[2];
        if (gy <= 0.2f) return false;
        for (float& v : M) v /= gy;
    }

    // 2) 明るさ：このアプリの初期トーンで現像したときの明るさの中央値を、カメラの JPEG に合わせる
    std::vector<float> camY;
    camY.reserve(px.size());
    for (const S& s : px) camY.push_back(s.c[0] * kLuma[0] + s.c[1] * kLuma[1] + s.c[2] * kLuma[2]);
    std::nth_element(camY.begin(), camY.begin() + camY.size() / 2, camY.end());
    const float target = camY[camY.size() / 2];
    Params q = p;
    q.exposure = 0;
    q.profile = "standard";
    const ToneLut lut = toneLut(q);
    constexpr float kIdx = (kLutN - 1) / kLutMax;
    auto medianAt = [&](float ev) {
        const float f = std::pow(2.0f, ev);
        std::vector<float> ys;
        ys.reserve(px.size());
        for (const S& s : px) {
            float v[3];
            for (int c = 0; c < 3; ++c) {
                const float x = std::max((M[c * 3] * s.o[0] + M[c * 3 + 1] * s.o[1] + M[c * 3 + 2] * s.o[2]) * f, 0.0f);
                const int idx = std::clamp(int(std::pow(x, 1.0f / kGamma) * kIdx), 0, kLutN - 1);
                v[c] = lut.t[c + 1][idx];
            }
            ys.push_back(v[0] * kLuma[0] + v[1] * kLuma[1] + v[2] * kLuma[2]);
        }
        std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        return ys[ys.size() / 2];
    };
    float lo = -3, hi = 4;
    for (int i = 0; i < 30; ++i) {
        const float mid = (lo + hi) / 2;
        (medianAt(mid) < target ? lo : hi) = mid;
    }
    p.profile = "camera";
    p.camExposure = std::round((lo + hi) / 2 * 100) / 100;
    p.camMatrix = M;
    return true;
}

}  // namespace rd
