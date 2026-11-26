#pragma once
// 書き出しダイアログ・環境設定
#include <QDialog>
#include <QImage>
#include <QJsonObject>

#include <vector>

#include "core/Export.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace rd {

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(int nSelected, int nFiltered, QWidget* parent = nullptr);
    ExportOptions options() const;
    QString targetMode() const;  // selected | all | star | pick
    int minStar() const;
    void accept() override;
    // ファイルサイズの目安に使う現像済みの画像と、元サイズで書き出したときの大きさ
    void setSample(const QImage& img, QSize fullOut);

private:
    void load(const ExportOptions& o);
    void updateEstimate();
    QComboBox *preset_, *fmt_, *jpegLevel_, *overwrite_, *sharpen_, *sharpenLevel_;
    QSpinBox *quality_, *longEdge_, *minStar_;
    QCheckBox *same_, *embed_, *addLib_;
    QLineEdit *folder_, *subfolder_, *template_, *suffix_;
    QButtonGroup* target_;
    QJsonObject userPresets_;
    QLabel* estimate_ = nullptr;
    std::vector<std::uint8_t> sample_;
    int sampleW_ = 0, sampleH_ = 0;
    QSize fullOut_;
    bool settingLevel_ = false;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    void accept() override;

private:
    QCheckBox *autoAdv_, *writeFiles_, *xmpRaw_, *wheel_, *camProfile_, *gpu_;
    QComboBox* propagate_;
};

}  // namespace rd
