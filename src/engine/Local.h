#pragma once
// 部分補正（マスク）とスポット修復
//  座標はすべて「回転・切り抜き前、レンズ補正後」の画像に対する 0..1（x は幅、y は高さで割った値）
#include <QJsonArray>
#include <QString>
#include <functional>
#include <vector>

#include "engine/Image.h"

namespace rd {

struct Params;

struct BrushDab {
    float x, y, r;    // r は幅に対する半径
    float flow = 1;   // 0..1
    bool erase = false;
    bool operator==(const BrushDab&) const = default;
};

struct LocalMask {
    QString type = "linear";  // linear | radial | brush | skin | sky
    QString name;
    bool enabled = true, invert = false;
    // linear：(x0,y0) で効果 100%、(x1,y1) で 0%
    // radial：中心 (x0,y0)、半径 rx=x1（幅基準）ry=y1（高さ基準）、angle 度
    float x0 = 0.5f, y0 = 0.3f, x1 = 0.5f, y1 = 0.6f, angle = 0;
    float feather = 50;  // ぼかし 0..100
    std::vector<BrushDab> dabs;
    // 補正（-100..100、露出は -4..4）
    float exposure = 0, contrast = 0, highlights = 0, shadows = 0, temp = 0, tint = 0, saturation = 0;
    float sharpness = 0, clarity = 0, texture = 0, dehaze = 0;  // 質感（シャープ −100 はぼかし）
    bool operator==(const LocalMask&) const = default;
};

struct Spot {
    float x, y, sx, sy, r;  // 修復先・コピー元・半径（幅基準）
    float feather = 50;
    bool clone = false;     // false = 修復（明るさ・色をなじませる）、true = コピー
    bool operator==(const Spot&) const = default;
};

QJsonArray masksToJson(const std::vector<LocalMask>& m);
std::vector<LocalMask> masksFromJson(const QJsonArray& a);
QJsonArray spotsToJson(const std::vector<Spot>& s);
std::vector<Spot> spotsFromJson(const QJsonArray& a);

bool localActive(const Params& p);

// 画素 → 正規化座標の対応（いま処理している画像のどこが元画像のどこか）
struct PixelMap {
    double nx0 = 0, ny0 = 0, dx = 0, dy = 0;  // n = n0 + 画素 * d（画素の中心は +0.5）
    double fullW = 1, fullH = 1;              // 元画像の画素数（半径・ブラシの換算用）
};

// 各マスクの値（0..1）を画素ごとに計算する準備。brush はここで画素に描く
class MaskSet {
public:
    MaskSet(const Params& p, int w, int h, const PixelMap& map);
    int count() const { return int(masks_.size()); }
    // x,y：画素、disp：その画素のおおよその表示値（色で選ぶマスク用）
    void eval(int x, int y, const float disp[3], float* out) const;
    const LocalMask& mask(int i) const { return *masks_[i]; }
    int slotOf(int paramIndex) const;  // p.masks の番号 → ここでの番号（無ければ -1）
    const std::vector<float>& brushPlane(int i) const { return brush_[i]; }  // brush 以外は空

private:
    const Params& p_;
    std::vector<const LocalMask*> masks_;
    std::vector<int> orig_;
    std::vector<std::vector<float>> brush_;  // brush の画素ごとの値
    int w_, h_;
    PixelMap map_;
};

// リニア段階（露出・色温度・色かぶり）：xin に掛ける倍率
void localLinearGains(const MaskSet& ms, const float* m, float gain[3]);
// 表示段階（コントラスト・ハイライト・シャドウ・彩度）
void localDisplayAdjust(const MaskSet& ms, const float* m, float v[3]);

// スポット修復。fetch：元画像の正規化座標の範囲を、いまの画像と同じ縮尺で取り出す（無ければ img 自身から）
using PatchFetch = std::function<ImageF(double nx, double ny, int w, int h)>;
void applySpots(ImageF& img, const Params& p, const PixelMap& map, const PatchFetch& fetch);
int spotMargin(const Params& p, double fullW);

// コピー元を自動で探す（修復先の近くで、似た質感の場所）
void autoSpotSource(const ImageF& lin, Spot& s);

}  // namespace rd
