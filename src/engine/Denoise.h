#pragma once
// ノイズ低減（Lightroom の「ノイズ軽減」相当）
// ・リニアの √ 空間で処理（光のノイズは明るさの平方根に比例するので、明暗でノイズの大きさがそろう）
// ・明るさ（Y）と色（2 成分）に分け、ウェーブレット（à trous）で細かさ別に、ノイズに見合った分だけ縮める
#include "engine/Image.h"
#include "engine/Params.h"

namespace rd {

// √ 空間でのノイズの標準偏差（フル解像度の画像に対して）
struct NoiseLevel {
    float y = 0, c1 = 0, c2 = 0;
    bool valid() const { return y > 0; }
};

// 画像からノイズの大きさを測る（平らな部分の細かい揺らぎから）
NoiseLevel estimateNoise(const ImageF& lin);

// lin（リニア）をその場でノイズ低減する。noise は「この画像の解像度での」値
void denoise(ImageF& lin, const Params& p, const NoiseLevel& noise);

// 処理に必要な、のりしろの幅（画素）
int denoiseMargin(const Params& p);

bool denoiseActive(const Params& p);

// ISO から初期値（輝度ノイズの適用量）
float defaultLumaNr(float iso);

}  // namespace rd
