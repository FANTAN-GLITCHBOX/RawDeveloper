#pragma once
// 書き出し（JPEG / TIFF 16bit / PNG）。★・ラベルの埋め込み。Python 版 core/export.py の移植。
#include <QJsonObject>
#include <QString>
#include <optional>

#include "engine/Image.h"

namespace rd {

struct Metadata;

struct ExportOptions {
    QString folder;
    bool sameFolder = false;
    QString subfolder;
    QString fmt = "jpeg";        // jpeg | tiff | png
    int quality = 92;
    int longEdge = 0;            // 0 = 元のサイズ
    QString nameTemplate = "{name}";
    QString suffix;
    bool embedRating = true;
    QString overwrite = "rename";  // rename | overwrite | skip
    bool addToLibrary = true;

    QJsonObject toJson() const;
    static ExportOptions fromJson(const QJsonObject& o);
};

QString exportExtension(const QString& fmt);
// 書き出し先のパス（同名があれば -2, -3 … を付ける）。skip で同名があれば空
QString exportPath(const QString& src, const ExportOptions& o, int index);
// ガンマ 0..1 の画像を書き出す。失敗したら理由を返す
QString writeExport(const ImageF& g, const QString& path, const ExportOptions& o, int rating, const QString& label,
                    const Metadata* meta);

}  // namespace rd
