#pragma once
// 書き出し（JPEG / TIFF 16bit / PNG）。★・ラベルの埋め込み。Python 版 core/export.py の移植。
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <optional>

#include "engine/Image.h"

namespace rd {

struct Metadata;

struct ExportOptions {
    QString folder;
    bool sameFolder = false;
    QString subfolder;
    QString fmt = "jpeg";        // jpeg | tiff | png
    int quality = 92;            // カスタムのときの JPEG 品質
    QString jpegLevel = "high";  // JPEG 画質：normal 普通 | high 高画質 | max 最高画質 | custom
    int longEdge = 0;            // 0 = 元のサイズ
    QString nameTemplate = "{name}";
    QString suffix;
    bool embedRating = true;
    QString overwrite = "rename";  // rename | overwrite | skip
    bool addToLibrary = true;
    QString sharpen = "none";    // 書き出し時のシャープ：none | screen | glossy | matte
    int sharpenLevel = 2;        // 1 弱 2 標準 3 強

    QJsonObject toJson() const;
    static ExportOptions fromJson(const QJsonObject& o);
};

// JPEG の実際の設定（品質と、色を間引かない 4:4:4 かどうか）
struct JpegSetting {
    int quality = 92;
    bool chroma444 = true;
};
JpegSetting jpegSetting(const QString& level, int customQuality);
inline JpegSetting jpegSetting(const ExportOptions& o) { return jpegSetting(o.jpegLevel, o.quality); }
// 古い設定（品質の数値だけ）から段階を推定
QString jpegLevelFromQuality(int q);
// 8bit RGB（詰めた配列）を JPEG にする。失敗したら空
QByteArray encodeJpeg(const std::uint8_t* rgb, int w, int h, const JpegSetting& s, const QByteArray& icc = {});
// 書き出したときのおおよそのファイルサイズ（バイト）。sample は現像済みの縮小画像、outW×outH は書き出しの大きさ
qint64 estimateJpegBytes(const std::uint8_t* rgb, int w, int h, int outW, int outH, const JpegSetting& s);

QString exportExtension(const QString& fmt);
// 書き出し先のパス（同名があれば -2, -3 … を付ける）。skip で同名があれば空
QString exportPath(const QString& src, const ExportOptions& o, int index);
// ガンマ 0..1 の画像を書き出す。失敗したら理由を返す
QString writeExport(const ImageF& g, const QString& path, const ExportOptions& o, int rating, const QString& label,
                    const Metadata* meta);

}  // namespace rd
