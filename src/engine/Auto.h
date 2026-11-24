#pragma once
// 自動調整（Python 版 engine/auto.py の移植）。スライダーの値を決めるだけ。
#include <array>

#include "engine/Image.h"
#include "engine/Params.h"

namespace rd {

// strength 0..100。wbOnly=true なら WB だけ自動
Params autoParams(const ImageF& lin, const std::array<float, 3>& asShot, float strength, const Params& base,
                  bool wbOnly = false);

}  // namespace rd
