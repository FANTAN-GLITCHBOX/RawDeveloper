#pragma once
// 質感の処理（表示用 RGB に対して）：かすみの除去・明瞭度・テクスチャ・ぼかし・シャープ
//  量は「全体の値＋各マスクの値×マスク」。マスクがなければ全体の値だけ（planes は空）
#include <vector>

#include "engine/Image.h"
#include "engine/Params.h"

namespace rd {

struct EffectAmounts {
    // 画素ごとの量（-100..100）。空ならどこも global の値
    std::vector<float> haze, clarity, texture, sharp;
    float gHaze = 0, gClarity = 0, gTexture = 0, gSharp = 0;
    bool any() const;
};

// 各効果のぼかしの大きさ（フル解像度の画素）
struct EffectSigmas {
    float haze, clarity, tex1, tex2, blur1, blur2;
};
EffectSigmas effectSigmas(double longEdgeFull);

constexpr float kHazeAir = 0.96f;  // かすみの色（白っぽい灰色）

// 全体の値（基本補正）とシャープ（ディテール）から EffectAmounts の global を作る
EffectAmounts globalEffects(const Params& p, bool detail);
bool maskHasEffects(const LocalMask& m);

// g をその場で処理。scale：フル解像度に対する縮小率。detail=false のときシャープは省く（ぼかしは行う）
void applyEffects(ImageF& g, const Params& p, const EffectAmounts& a, float scale, double longEdgeFull, bool detail);
int effectsMargin(const Params& p, double longEdgeFull);

// 書き出し用のシャープ（縮小後に）。kind：screen | matte | glossy、level：1 弱 2 標準 3 強
void outputSharpen(ImageF& g, const QString& kind, int level);

}  // namespace rd
