#pragma once
// 色の基本計算（Python 版 engine/color.py と同じ式）
#include <array>
#include <utility>

namespace rd {

constexpr float kGamma = 2.2f;
constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
constexpr double kTempMin = 2000.0, kTempMax = 50000.0, kTintRange = 150.0;

std::array<float, 3> wbGains(double temp, double tint);
// RGB ゲイン（G=1）に最も近い 色温度・色かぶり
std::pair<double, double> fitTempTint(const std::array<float, 3>& gains);
double tempToSlider(double temp);
double sliderToTemp(double s);

// cv2 の float HSV と同じ定義（H: 0..360, S, V: 0..1）
void rgbToHsv(float r, float g, float b, float& h, float& s, float& v);
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b);

}  // namespace rd
