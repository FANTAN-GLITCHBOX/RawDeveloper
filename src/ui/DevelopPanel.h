#pragma once
// 現像パネル（右側）。値が変わると paramsChanged(params, final, 説明) を出す。
// 各パネルの見出し：●オン/オフ（Alt＋クリックでソロ）、↺ 初期値に戻す
#include <QMap>
#include <optional>
#include <QWidget>
#include <array>

#include "engine/Params.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QToolButton;
class QTabWidget;
class QVBoxLayout;

namespace rd {

class ParamRow;
class Section;
class ColorWheel;
class CurveEditor;
class ScrubField;

class DevelopPanel : public QWidget {
    Q_OBJECT
public:
    explicit DevelopPanel(QWidget* parent = nullptr);
    void setParams(const Params& p, std::pair<double, double> asShot);
    const Params& params() const { return p_; }
    void setHistogram(const std::vector<float>& h);
    void nudgeZone(const QString& key, double dv, bool final);
    void setWb(double temp, double tint, const QString& desc);
    QToolButton* wbPickButton() const { return wbPick_; }
    QPushButton* cropButton() const { return cropBtn_; }
    double aspectValue() const;  // 0 = 自由、-1 = 元の比率
    void setLensStatus(const QString& text);

signals:
    void paramsChanged(const rd::Params& p, bool final, const QString& desc);
    void autoRequested(float strength, bool wbOnly);
    void hoverKey(const QString& key);
    void cropToolToggled(bool on);
    void aspectChanged();

private:
    ParamRow* row(Section* sec, const QString& key, const QString& label, double lo, double hi, double def, int dec,
                  double step, const QString& suffix = {}, std::vector<std::pair<double, QColor>> grad = {});
    void addSection(Section* sec, Panel panel);
    void rowChanged(const QString& key, double v, bool final);
    void emitChange(bool final, const QString& desc);
    void solo(Panel panel);
    void syncSections();

    Params p_;
    std::pair<double, double> asShot_{5500, 0};
    bool updating_ = false;
    QMap<QString, ParamRow*> rows_;
    std::array<ParamRow*, 24> hslRows_{};
    std::array<ColorWheel*, 4> wheels_{};
    QMap<Panel, Section*> sections_;
    std::optional<Enabled> soloBackup_;
    QVBoxLayout* v_ = nullptr;
    QComboBox* wbMode_ = nullptr;
    QToolButton* wbPick_ = nullptr;
    ScrubField* autoStrength_ = nullptr;
    CurveEditor* curve_ = nullptr;
    QPushButton* cropBtn_ = nullptr;
    QComboBox* aspect_ = nullptr;
    QCheckBox* lensProfile_ = nullptr;
    QCheckBox* lensCa_ = nullptr;
    QCheckBox* lensTrim_ = nullptr;
    QComboBox* lensCombo_ = nullptr;   // レンズ（選んだメーカーの中から）
    QComboBox* lensMaker_ = nullptr;   // メーカー（先頭は「自動」）
    QLineEdit* lensSearch_ = nullptr;  // 全メーカーから名前で検索
    void fillLensModels(const QString& maker, const QString& selectKey);
    QLabel* lensStatus_ = nullptr;
};

}  // namespace rd
