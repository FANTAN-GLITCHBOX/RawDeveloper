#pragma once
// レンズ補正（Lensfun）と手動補正。Python 版 engine/lens.py の移植。
//  データ源：アプリ同梱の Lensfun データ（lensfun フォルダ）＋ %APPDATA%/RawDeveloper/lenses/*.xml
//  補正の順：周辺減光（リニアのまま）→ 色収差・歪曲（座標の付け替え。切り抜きの前）
#include <memory>
#include <string>
#include <vector>

#include "engine/Image.h"

namespace rd {

struct Params;
struct Metadata;

// 写真ごとのレンズ情報（EXIF から）
struct LensInfo {
    std::string autoKey;   // 自動検出したレンズ（"メーカー|モデル"、見つからなければ空）
    float crop = 1.0f;     // カメラのクロップ係数
    float focal = 0;       // mm
    float aperture = 0;    // F 値
    int confidence = 0;    // 0..100
    std::string status;    // パネルに出す説明
};

class LensDb {
public:
    static LensDb& get();
    // dataDirs：XML を探すフォルダ（先頭から順に）
    static void setDataDirs(std::vector<std::string> dirs);
    bool ok() const;
    LensInfo identify(const Metadata& meta) const;
    std::vector<std::string> allKeys() const;  // 一覧用（メーカー・モデル順）
    std::string displayName(const std::string& key) const;
    // レンズのマウント（一覧の見出し用）。複数マウント対応なら "*"、レンズ一体型カメラなら "fixed"
    std::string mountOf(const std::string& key) const;

private:
    LensDb();
    struct Impl;
    std::unique_ptr<Impl> d_;
    friend struct LensApply;
};

// 実際に使うレンズ（パラメータで手動選択されていればそれ、なければ自動）
std::string effectiveLensKey(const Params& p, const LensInfo& info);

// レンズ補正が座標を動かすか（動かすなら回転と同様に「全体」が必要…ではなく領域計算も可能）
bool lensActive(const Params& p, const LensInfo* info);

// 元画像（幅 fullW×高さ fullH）のうち (x0, y0, w, h) の範囲を、補正後の画像として作る。
// src はフル画像（fullW×fullH と同じ縮尺のリニア画像）。
ImageF lensRemapRegion(const ImageF& src, const Params& p, const LensInfo& info, int x0, int y0, int w, int h);

}  // namespace rd
