#pragma once
// DaVinci Resolve 風の操作部品（Python 版 ui/widgets.py の移植）
//  数値の上で左右ドラッグ … 増減（画面端で反対側へ回り込む）
//  Ctrl＋ドラッグ … 1/10 の微調整、Shift＋ドラッグ … 10 倍の粗調整
//  ダブルクリック … 初期値に戻す、クリック … 数値入力（「+=0.3」で相対入力）
#include <QFrame>
#include <QWidget>
#include <array>
#include <functional>
#include <vector>

class QLabel;
class QLineEdit;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;

namespace rd {

class ScrubField : public QWidget {
    Q_OBJECT
public:
    ScrubField(double value, double lo, double hi, double def, int decimals, double step, QString suffix = {},
               int width = 56, QWidget* parent = nullptr);
    double value() const { return v_; }
    void setValue(double v, bool emitSignal = false, bool final = true);
    double defaultValue() const { return def_; }

signals:
    void valueChanged(double v, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    QString text() const;
    void startEdit();
    double v_, lo_, hi_, def_, step_;
    int dec_;
    QString suffix_;
    bool pressed_ = false;
    int moved_ = 0;
    double acc_ = 0;
    QPoint last_;
    QLineEdit* edit_ = nullptr;
};

class SliderBar : public QWidget {
    Q_OBJECT
public:
    SliderBar(double lo, double hi, double def, std::vector<std::pair<double, QColor>> gradient = {},
              QWidget* parent = nullptr);
    void setValue(double v);
    double value() const { return v_; }

signals:
    void valueChanged(double v, bool final);
    void hovered(bool on);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    double posOf(double v) const;
    double valOf(double x) const;
    double v_, lo_, hi_, def_;
    std::vector<std::pair<double, QColor>> grad_;
    bool pressed_ = false;
    double grabOff_ = 0, anchorX_ = 0, anchorV_ = 0;
};

// 名前＋スライダー＋数値欄。toSlider/fromSlider で非線形な対応（色温度など）
class ParamRow : public QWidget {
    Q_OBJECT
public:
    ParamRow(QString key, QString label, double lo, double hi, double def, int decimals, double step,
             QString suffix = {}, std::vector<std::pair<double, QColor>> gradient = {},
             std::function<double(double)> toSlider = {}, std::function<double(double)> fromSlider = {},
             int scrubWidth = 56, QWidget* parent = nullptr);
    void setValue(double v);
    double value() const;
    QString key() const { return key_; }

signals:
    void changed(const QString& key, double v, bool final);
    void hovered(const QString& key, bool on);

protected:
    void enterEvent(QEnterEvent*) override { emit hovered(key_, true); }
    void leaveEvent(QEvent*) override { emit hovered(key_, false); }

private:
    QString key_;
    SliderBar* slider_;
    ScrubField* field_;
    std::function<double(double)> to_, from_;
};

// 折りたたみ・オン/オフ・初期値に戻すボタン付きのパネル
class Section : public QFrame {
    Q_OBJECT
public:
    Section(QString title, bool collapsed = false, bool hasToggle = true, QWidget* parent = nullptr);
    void add(QWidget* w);
    void setEnabledState(bool on);  // 見た目のみ（シグナルは出さない）
    bool isOn() const { return on_; }
    QHBoxLayout* extraLayout() { return extra_; }

signals:
    void toggled(bool on);          // オン/オフ
    void soloRequested();           // Alt＋クリック
    void resetRequested();

private:
    void setCollapsed(bool c);
    QWidget* body_;
    QVBoxLayout* bodyLayout_;
    QToolButton* arrow_;
    QToolButton* power_;
    QLabel* title_;
    QHBoxLayout* extra_;
    bool on_ = true;
};

// カラーホイール（中の点：色味、下のバー：明るさ）
class ColorWheel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheel(QString title, QWidget* parent = nullptr);
    void setValues(const std::array<float, 3>& v);
    std::array<float, 3> values() const { return v_; }

signals:
    void changed(const std::array<float, 3>& v, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    void geom(double& cx, double& cy, double& r) const;
    QString title_;
    std::array<float, 3> v_{0, 0, 0};
    int mode_ = 0;  // 0 なし, 1 点, 2 バー
    QPointF last_;
};

bool wheelAdjustEnabled();

}  // namespace rd
