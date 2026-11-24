#pragma once
// 現像処理（この段階では WB・露出・ガンマのみ。Python 版の pipeline.py を順に移植していく）
#include <array>
#include <cstdint>
#include <vector>
#include "engine/Image.h"

namespace rd {

struct Params {
    std::array<float, 3> wbGains{1, 1, 1};
    float exposure = 0.0f;  // EV
};

// リニア画像 → 表示用 8bit RGB（RGB888, 行の詰め物なし）
std::vector<std::uint8_t> developToRgb8(const ImageF& lin, const Params& p);

}  // namespace rd
