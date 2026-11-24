#pragma once
// ★レーティング・カラーラベルを XMP で読み書きする（Python 版 core/xmp.py の移植）
//  RAW：        <名前>.xmp サイドカー（設定でオン）
//  JPEG / PNG： ファイル内の XMP を直接書き換え（画像データは再圧縮しない）
//  TIFF：       XMP タグがあれば書き換え、なければ <名前>.xmp サイドカーへ
//  書き込みは一時ファイルに書いてから入れ替える（途中で落ちても元ファイルは壊れない）
#include <QByteArray>
#include <QString>
#include <optional>

namespace rd::xmp {

struct Rating {
    std::optional<int> rating;   // 0..5（-1 は除外扱いの値が入ることがある）
    QString label;               // red / yellow / green / blue / purple / 空
};

QString labelFromXmp(const QString& text);
QString labelToXmp(const QString& label);

QString newPacket(std::optional<int> rating, const QString& label);
QString updatePacket(const QString& xml, std::optional<int> rating, const QString& label);
Rating parsePacket(const QString& xml);

QByteArray jpegReadXmp(const QByteArray& data);
QByteArray jpegWithXmp(const QByteArray& data, std::optional<int> rating, const QString& label);
QByteArray pngReadXmp(const QByteArray& data);
QByteArray pngWithXmp(const QByteArray& data, std::optional<int> rating, const QString& label);

QString sidecarPath(const QString& path);
Rating readSidecar(const QString& path);
bool writeSidecar(const QString& path, std::optional<int> rating, const QString& label);

// ファイル先頭付近の XMP（カメラ内レーティング・他ソフトの評価）
Rating readEmbedded(const QString& path);
// ファイルの★・ラベル（JPEG/PNG/TIFF は中身、なければサイドカー、なければ先頭付近）
Rating readFileRating(const QString& path);
// 書き出し済みファイルへ書く。戻り値："file" / "sidecar" / エラーなら空
QString writeFileRating(const QString& path, std::optional<int> rating, const QString& label, QString* error = nullptr);

bool atomicWrite(const QString& path, const QByteArray& data, bool keepMtime = true);

}  // namespace rd::xmp
