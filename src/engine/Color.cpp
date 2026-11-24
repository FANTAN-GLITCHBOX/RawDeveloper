#include "engine/Color.h"

#include <algorithm>
#include <cmath>

namespace rd {

namespace {

constexpr double kXyzToSrgb[3][3] = {
    {3.2404542, -1.5371385, -0.4985314},
    {-0.9692660, 1.8760108, 0.0415560},
    {0.0556434, -0.2040259, 1.0572252},
};

std::pair<double, double> illuminantXy(double temp) {
    const double t = std::clamp(temp, 1667.0, 50000.0);
    double x, y;
    if (t >= 4000.0) {
        if (t <= 7000.0)
            x = -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) + 0.09911e3 / t + 0.244063;
        else
            x = -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) + 0.24748e3 / t + 0.237040;
        y = -3.000 * x * x + 2.870 * x - 0.275;
    } else {
        x = -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910;
        if (t <= 2222.0)
            y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
        else
            y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    }
    return {x, y};
}

std::array<double, 3> illumRgb(double temp) {
    auto [x, y] = illuminantXy(temp);
    const double xyz[3] = {x / y, 1.0, (1 - x - y) / y};
    std::array<double, 3> rgb{};
    for (int i = 0; i < 3; ++i) {
        double s = 0;
        for (int j = 0; j < 3; ++j) s += kXyzToSrgb[i][j] * xyz[j];
        rgb[i] = std::max(s, 1e-4);
    }
    return rgb;
}

const std::array<double, 3>& refRgb() {
    static const auto r = illumRgb(6504.0);
    return r;
}

}  // namespace

std::array<float, 3> wbGains(double temp, double tint) {
    const auto rgb = illumRgb(temp);
    const auto& ref = refRgb();
    double g[3];
    for (int i = 0; i < 3; ++i) g[i] = 1.0 / (rgb[i] / ref[i]);
    const double g1 = g[1];
    for (double& v : g) v /= g1;
    const double t = std::exp(tint / 100.0 * 0.30);
    g[0] *= t;
    g[2] *= t;
    return {static_cast<float>(g[0]), static_cast<float>(g[1]), static_cast<float>(g[2])};
}

std::pair<double, double> fitTempTint(const std::array<float, 3>& gainsIn) {
    const double lr = std::log(std::max(gainsIn[0] / gainsIn[1], 1e-4f));
    const double lb = std::log(std::max(gainsIn[2] / gainsIn[1], 1e-4f));
    double bestErr = 1e9, bestT = 5500, bestTint = 0;
    const double m0 = 1e6 / 15000.0, m1 = 1e6 / 2000.0;
    for (int i = 0; i < 400; ++i) {
        const double mired = m0 + (m1 - m0) * i / 399.0;
        const double t = 1e6 / mired;
        const auto g0 = wbGains(t, 0.0);
        const double dr = lr - std::log(g0[0]);
        const double db = lb - std::log(g0[2]);
        const double d = (dr + db) / 2.0;
        const double err = (dr - d) * (dr - d) + (db - d) * (db - d);
        if (err < bestErr) {
            bestErr = err;
            bestT = t;
            bestTint = d / 0.30 * 100.0;
        }
    }
    return {std::round(bestT / 10.0) * 10.0, std::clamp(std::round(bestTint), -kTintRange, kTintRange)};
}

double tempToSlider(double temp) {
    const double m = 1e6 / std::clamp(temp, kTempMin, kTempMax);
    const double m0 = 1e6 / kTempMin, m1 = 1e6 / kTempMax;
    return (m0 - m) / (m0 - m1);
}

double sliderToTemp(double s) {
    const double m0 = 1e6 / kTempMin, m1 = 1e6 / kTempMax;
    return 1e6 / (m0 - std::clamp(s, 0.0, 1.0) * (m0 - m1));
}

void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0.0f;
    if (d <= 0) {
        h = 0;
        return;
    }
    if (mx == r)
        h = 60.0f * (g - b) / d;
    else if (mx == g)
        h = 60.0f * (b - r) / d + 120.0f;
    else
        h = 60.0f * (r - g) / d + 240.0f;
    if (h < 0) h += 360.0f;
}

void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s <= 0) {
        r = g = b = v;
        return;
    }
    float hh = h / 60.0f;
    hh -= 6.0f * std::floor(hh / 6.0f);
    const int i = static_cast<int>(hh) % 6;
    const float f = hh - std::floor(hh);
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

}  // namespace rd
