#pragma once
// 肌色の補正：肌らしい色（色相・彩度・明るさ）だけを選んで、色相・彩度・明るさとムラ（均一化）を調整する
#include "engine/Image.h"
#include "engine/Params.h"

namespace rd {

constexpr float kSkinLineHue = 18.0f;  // ベクトルスコープのスキントーンライン（123°）付近の色相（HSV）

bool skinActive(const Params& p);
// 肌らしさ 0..1（HSV：h は度、s・v は 0..1）
float skinMask(const Params& p, float h, float s, float v);
// 1 画素の色を補正（HSV のまま）。m は skinMask の値
void skinAdjust(const Params& p, float m, float& h, float& s, float& v);
// 明るさの均一化（空間的な処理）。g は表示用 RGB。scale はフル解像度に対する縮小率
void skinLumaUniformity(ImageF& g, const Params& p, float scale);
int skinMargin(const Params& p);
// 肌の範囲を表示（オレンジで塗る）
void skinMaskOverlay(ImageF& g, const Params& p);

// 表示用 RGB から肌らしい部分を探して代表色を測る
struct SkinStats {
    float fraction = 0;  // 肌と思われる画素の割合
    float hue = 0, sat = 0, val = 0;  // 中央値
};
SkinStats analyzeSkin(const ImageF& display);

// 「いい感じに」：level 1=弱め 2=普通 3=強め
Params skinAuto(const Params& base, const SkinStats& st, int level);

}  // namespace rd
