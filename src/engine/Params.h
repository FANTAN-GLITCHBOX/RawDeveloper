#pragma once
// 現像パラメータ。.rdev（JSON）は Python 版と同じ形式で読み書きする。
#include <QJsonObject>
#include <QString>
#include <array>
#include <utility>
#include <vector>

namespace rd {

using CurvePoints = std::vector<std::pair<float, float>>;
using Wheel = std::array<float, 3>;  // u, v, master

constexpr int kHslBands = 8;
constexpr float kHslCenters[kHslBands] = {0, 30, 60, 120, 180, 240, 270, 300};

// パネルのオン/オフ（オフでも値は残し、処理だけ飛ばす）
struct Enabled {
    bool basic = true, wheels = true, curve = true, hsl = true, detail = true, lens = true, crop = true;
    bool operator==(const Enabled&) const = default;
};

struct Params {
    int version = 1;
    int processVersion = 1;
    // WB
    QString wbMode = "as_shot";  // as_shot | custom
    float temp = 5500, tint = 0;
    // トーン
    float exposure = 0, contrast = 0, highlights = 0, shadows = 0, whites = 0, blacks = 0;
    float saturation = 0, vibrance = 0;
    // カーブ（Y, R, G, B）
    std::array<CurvePoints, 4> curve{CurvePoints{{0, 0}, {1, 1}}, CurvePoints{{0, 0}, {1, 1}},
                                     CurvePoints{{0, 0}, {1, 1}}, CurvePoints{{0, 0}, {1, 1}}};
    // HSL
    std::array<float, kHslBands> hslHue{}, hslSat{}, hslLum{};
    // カラーホイール
    Wheel lift{}, gamma{}, gain{}, offset{};
    // ディテール
    float sharpenAmount = 40, sharpenRadius = 1.0f, sharpenMasking = 0;
    float nrLuma = 0, nrColor = 25;
    // レンズ補正
    bool lensProfile = true;      // プロファイル補正を使う
    QString lensKey;              // 空 = 自動検出、"メーカー|モデル" = 手動選択
    float lensDistortion = 100;   // 適用量 %
    float lensVignetting = 100;
    bool lensCa = true;
    bool lensAutotrim = true;
    float lensManualDistortion = 0;
    float lensManualVignetting = 0;
    // 切り抜き
    float cropX = 0, cropY = 0, cropW = 1, cropH = 1, cropAngle = 0;
    QString cropAspect = "free";
    float autoStrength = 100;
    Enabled enabled;

    bool operator==(const Params&) const = default;

    QJsonObject toJson() const;
    static Params fromJson(const QJsonObject& o);
    static Params defaults();          // RAW 用の初期値
    static Params defaultsForImage();  // JPEG 等の初期値（シャープ・ノイズ低減なし）
};

bool isIdentityCurve(const CurvePoints& c);

// パネル単位で初期値に戻す
enum class Panel { Basic, Wheels, Curve, Hsl, Detail, Lens, Crop };
void resetPanel(Params& p, Panel panel);
bool& enabledFlag(Params& p, Panel panel);
const char* panelName(Panel panel);

}  // namespace rd
