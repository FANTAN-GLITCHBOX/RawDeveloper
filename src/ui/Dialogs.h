#pragma once
// 書き出しダイアログ・環境設定
#include <QDialog>
#include <QJsonObject>

#include "core/Export.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
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

private:
    void load(const ExportOptions& o);
    QComboBox *preset_, *fmt_, *overwrite_;
    QSpinBox *quality_, *longEdge_, *minStar_;
    QCheckBox *same_, *embed_, *addLib_;
    QLineEdit *folder_, *subfolder_, *template_, *suffix_;
    QButtonGroup* target_;
    QJsonObject userPresets_;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    void accept() override;

private:
    QCheckBox *autoAdv_, *writeFiles_, *xmpRaw_, *wheel_;
    QComboBox* propagate_;
};

}  // namespace rd
