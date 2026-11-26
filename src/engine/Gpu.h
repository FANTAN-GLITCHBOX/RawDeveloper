#pragma once
// GPU で現像するための受け渡し。エンジンは GPU を知らない：UI 側が GpuBackend を登録すると使う
#include <array>
#include <vector>

#include "engine/Image.h"
#include "engine/Local.h"
#include "engine/Params.h"

namespace rd {

// 1 画素ごとの処理（WB・露出・プロファイル・部分補正・トーン・ホイール・彩度・HSL・肌色）と、
// その後の空間処理（肌の均一化・なめらかさ、シャープ）に必要なものを CPU で用意して渡す
struct MainStage {
    const ImageF* lin = nullptr;  // リニア（レンズ補正・ノイズ低減・スポット修復のあと）
    const Params* p = nullptr;
    float gains[3];
    bool camMat = false;
    const std::array<std::vector<float>, 3>* tab = nullptr;  // √x で引くトーン表
    float sScale = 0;
    float sat = 0, vib = 0;
    const std::vector<float>* skinTab = nullptr;              // 0..360
    bool hsl = false, hasHue = false, hasSat = false, hasLum = false;
    const std::vector<float> *tHue = nullptr, *tSat = nullptr, *tLum = nullptr;  // 721
    bool skin = false;            // 肌色の色の補正
    const MaskSet* masks = nullptr;
    PixelMap map;
    int showSlot = -1;
    std::vector<float>* showVals = nullptr;  // 範囲表示用に返す
    bool skinSpatial = false;     // 肌の均一化（明るさ）・なめらかさ
    bool detail = false;          // シャープなど細かい処理をするか（プレビューのドラッグ中は false）
    const struct EffectAmounts* effects = nullptr;  // 質感（全体の値。画素ごとの値はマスクから GPU で作る）
    const std::vector<int>* effSlots = nullptr;     // 質感のあるマスクの番号
    double longEdge = 1;
    float scale = 1;
};

class GpuBackend {
public:
    virtual ~GpuBackend() = default;
    // できなければ false（CPU で処理する）。out は表示用 RGB（0..1 に収めたもの）
    virtual bool runMain(const MainStage& st, ImageF& out) = 0;
};

void setGpuBackend(GpuBackend* b);
GpuBackend* gpuBackend();
void setGpuEnabled(bool on);
bool gpuEnabled();

}  // namespace rd
