#pragma once
// 自動調整：シーンを見分け、縮小画像を実際に現像して評価しながらスライダーの値を探す
#include <array>

#include "engine/Image.h"
#include "engine/Params.h"
#include "engine/RawLoader.h"

#include <string>

namespace rd {

struct AutoInfo {
    std::string scene;  // 見分けたシーン（例：「逆光・夕景」）。普通の写真なら空
    bool usedCamera = false;  // カメラの JPEG を手本にしたか
};

// strength 0..100。wbOnly=true なら WB だけ自動。meta があれば撮影情報（ISO・シャッター・絞り・露出補正）も使う
// cameraRef（カメラの JPEG、表示用 sRGB）があれば、明るさとメリハリの手本にする
Params autoParams(const ImageF& lin, const std::array<float, 3>& asShot, float strength, const Params& base,
                  bool wbOnly = false, const Metadata* meta = nullptr, AutoInfo* info = nullptr,
                  const ImageF* cameraRef = nullptr);

}  // namespace rd
