#pragma once
// 現像パイプライン（Python 版 engine/pipeline.py の移植）
//  リニア → WB → 露出 → ガンマ → トーン LUT → カラーホイール → 彩度 → HSL → ディテール → 回転・切り抜き
#include <array>
#include <cstdint>
#include <vector>

#include "engine/Image.h"
#include "engine/Params.h"

namespace rd {

constexpr int kLutN = 4096;
constexpr float kLutMax = 2.0f;

struct ToneLut {
    std::array<std::vector<float>, 4> t;  // 共通, R, G, B
};

std::vector<float> monotoneSpline(const CurvePoints& pts, const std::vector<float>& xs);
ToneLut toneLut(const Params& p);
std::array<float, 3> wbFor(const Params& p, const std::array<float, 3>& asShot);

struct DevelopOptions {
    float scale = 1.0f;    // 元画像に対する縮小率（シャープ半径などの換算）
    bool detail = true;    // false：ノイズ低減・シャープを省略（スライダー操作中）
    bool doCrop = true;    // false：回転のみ（切り抜きツール表示用）
};

// リニア画像を現像し、ガンマ 0..1 の RGB を返す
ImageF develop(const ImageF& lin, const Params& p, const std::array<float, 3>& asShot, const DevelopOptions& o);

std::vector<std::uint8_t> toRgb8(const ImageF& g);
std::vector<std::uint16_t> toRgb16(const ImageF& g);

// 回転後の画像に収まる、元と同じ縦横比の最大の矩形（幅, 高さ）
std::pair<double, double> rotatedRectMaxArea(double w, double h, double angleRad);

// 画像処理の部品（テスト・他所でも使う）
void gaussianBlur(std::vector<float>& plane, int w, int h, float sigma);

}  // namespace rd
