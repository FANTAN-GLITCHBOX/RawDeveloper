#pragma once
// RAW に埋め込まれたカメラの JPEG を、自動調整の手本（明るさ・メリハリ）として小さく読み込む
#include <vector>

#include "engine/Image.h"

namespace rd {

// 表示用の sRGB（0..1）。長辺 maxSize 以下。使えない（無い・小さすぎる・縦横比が違う）なら空の画像
ImageF cameraReference(const std::vector<unsigned char>& rawBytes, int imageW, int imageH, int maxSize = 400);

}  // namespace rd
