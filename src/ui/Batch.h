#pragma once
// 調整のコピー・貼り付け、まとめて自動補正、同期
#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <memory>

#include "engine/Params.h"
#include "engine/RawLoader.h"

class QCheckBox;

namespace rd {

// 写真の読み込み（MainWindow.cpp）。half=true は速い半分サイズ
std::shared_ptr<LoadedRaw> loadPhoto(const QString& path, bool half);

// 貼り付けの単位（JSON のキー）
struct PasteGroup {
    const char* id;
    const char* label;
    QStringList keys;  // "exposure", "wb", "enabled.basic" など
};
const std::vector<PasteGroup>& pasteGroups();
QStringList defaultPasteGroups();  // WB と切り抜き以外

// src の groups に入るキーを dst に写す
Params mergeGroups(const Params& dst, const Params& src, const QStringList& groups);
// 変わったキー（同期用）。切り抜きの縦横比など細かいキーは "crop" にまとめる
QStringList changedKeys(const Params& a, const Params& b);
Params copyKeys(const Params& dst, const Params& src, const QStringList& keys);

// 中央寄りの明るさ（リニア）の中央値。露出をそろえるときの基準
double photoLuminance(const ImageF& lin, const std::array<float, 3>& gains);

// .rdev の params を読む・書く（★などほかの項目は残す）
Params readParams(const QString& path, bool* exists = nullptr);
bool writeParams(const QString& path, const Params& p);

// 調整後のサムネイルを作ってキャッシュに保存（.rdev を書いたあとに呼ぶ）
QImage renderEditedThumb(const QString& path, const LoadedRaw& src, const Params& p);

struct BatchJob {
    enum Mode { Paste, Auto, Sync, Thumb } mode = Paste;
    QString path;
    Params src;
    QStringList keys;
    bool matchExposure = false;
    double srcLum = -1;
};
struct BatchDone {
    QString path;
    QImage thumb;
    bool ok = true;
};

class CopyDialog : public QDialog {
    Q_OBJECT
public:
    explicit CopyDialog(QWidget* parent = nullptr);
    QStringList groups() const;
    bool matchExposure() const;
    void accept() override;

private:
    std::vector<std::pair<QString, QCheckBox*>> boxes_;
    QCheckBox* match_ = nullptr;
};

}  // namespace rd
