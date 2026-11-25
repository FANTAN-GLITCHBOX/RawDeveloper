#pragma once
// 「カメラに合わせる」プロファイル：RAW に入っているカメラの JPEG と比べて、
// 写真ごとに明るさ（露出のずれ）と色（3×3 の行列）を求める
#include <array>

#include "engine/Image.h"
#include "engine/Params.h"

namespace rd {

// lin：リニア（昼光 WB で現像したもの）、asShot：撮影時の WB、camRef：カメラの JPEG（表示用 sRGB）
// p の WB・トーンなどはそのまま使い、profile 関係の値だけを埋めて返す。合わせられなければ false
bool matchCamera(const ImageF& lin, const std::array<float, 3>& asShot, const ImageF& camRef, Params& p);

}  // namespace rd
