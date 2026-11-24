#include "ui/Widgets.h"

#include <QApplication>
#include <QConicalGradient>
#include <QGraphicsOpacityEffect>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QScreen>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "ui/Theme.h"

namespace rd {

bool wheelAdjustEnabled() { return QSettings().value("ui/wheel_adjust", true).toBool(); }

namespace {

double modFactor(Qt::KeyboardModifiers m) {
    if (m & Qt::ControlModifier) return 0.1;
    if (m & Qt::ShiftModifier) return 10.0;
    return 1.0;
}

// ドラッグ中にカーソルが画面端に来たら反対側へ回り込ませ、移動量を返す
int wrapDelta(QPoint& last, const QPoint& g) {
    int dx = g.x() - last.x();
    last = g;
    if (QScreen* s = QApplication::screenAt(g)) {
        const QRect r = s->geometry();
        if (g.x() <= r.left() + 2) {
            last = QPoint(r.right() - 4, g.y());
            QCursor::setPos(last);
        } else if (g.x() >= r.right() - 2) {
            last = QPoint(r.left() + 4, g.y());
            QCursor::setPos(last);
        }
    }
    return dx;
}

}  // namespace

// ================================================================ ScrubField

ScrubField::ScrubField(double value, double lo, double hi, double def, int decimals, double step, QString suffix,
                       int width, QWidget* parent)
    : QWidget(parent), v_(value), lo_(lo), hi_(hi), def_(def), step_(step), dec_(decimals), suffix_(std::move(suffix)) {
    setFixedSize(width, 20);
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(QStringLiteral("ドラッグで調整（Ctrl：微調整／Shift：粗調整）、ダブルクリックでリセット\n"
                              "クリックで入力（「+=0.3」で今の値に足す）"));
}

void ScrubField::setValue(double v, bool emitSignal, bool final) {
    v = std::clamp(v, lo_, hi_);
    const double q = std::pow(10.0, dec_);
    v = std::round(v * q) / q;
    const bool changed = v != v_;
    v_ = v;
    update();
    if (emitSignal && (changed || final)) emit valueChanged(v_, final);
}

QString ScrubField::text() const {
    QString s = QString::number(v_, 'f', dec_);
    if (v_ > 0 && lo_ < 0) s = "+" + s;
    return s + suffix_;
}

void ScrubField::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 1.5, -0.5, -1.5);
    p.setPen(QPen(pressed_ ? theme::kAccent : (underMouse() ? QColor("#555") : theme::kLine), 1));
    p.setBrush(QColor("#2b2b2b"));
    p.drawRoundedRect(r, 3, 3);
    p.setPen(std::abs(v_ - def_) > 1e-9 ? theme::kText : theme::kTextDim);
    p.drawText(r, Qt::AlignCenter, text());
}

void ScrubField::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    pressed_ = true;
    moved_ = 0;
    acc_ = 0;
    last_ = e->globalPosition().toPoint();
    update();
}

void ScrubField::mouseMoveEvent(QMouseEvent* e) {
    if (!pressed_) return;
    const int dx = wrapDelta(last_, e->globalPosition().toPoint());
    moved_ += std::abs(dx);
    if (moved_ < 3) return;
    acc_ += dx * step_ * modFactor(e->modifiers());
    const double q = std::pow(10.0, -dec_);
    const double n = std::trunc(acc_ / q) * q;
    if (n != 0) {
        acc_ -= n;
        setValue(v_ + n, true, false);
    }
}

void ScrubField::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    pressed_ = false;
    if (moved_ < 3)
        startEdit();
    else
        emit valueChanged(v_, true);
    update();
}

void ScrubField::mouseDoubleClickEvent(QMouseEvent*) {
    if (edit_) {
        edit_->deleteLater();
        edit_ = nullptr;
    }
    setValue(def_, true, true);
}

void ScrubField::wheelEvent(QWheelEvent* e) {
    if (!wheelAdjustEnabled()) {
        e->ignore();
        return;
    }
    const int d = e->angleDelta().y() > 0 ? 1 : -1;
    setValue(v_ + d * step_ * 5 * modFactor(e->modifiers()), true, true);
}

void ScrubField::startEdit() {
    if (edit_) return;
    edit_ = new QLineEdit(this);
    edit_->setGeometry(rect());
    edit_->setAlignment(Qt::AlignCenter);
    edit_->setText(QString::number(v_, 'f', dec_));
    edit_->selectAll();
    edit_->show();
    edit_->setFocus();
    connect(edit_, &QLineEdit::editingFinished, this, [this] {
        if (!edit_) return;
        QString t = edit_->text().trimmed();
        t.replace(QChar(0xFF0B), '+').replace(QChar(0xFF0D), '-').replace(QChar(0xFF1D), '=');
        bool ok = false;
        double v;
        if (t.startsWith("+=") || t.startsWith("-=")) {
            v = (t.mid(0, 1) + t.mid(2)).toDouble(&ok);
            v += v_;
        } else {
            v = t.toDouble(&ok);
        }
        if (ok) setValue(v, true, true);
        edit_->deleteLater();
        edit_ = nullptr;
    });
}

// ================================================================ SliderBar

SliderBar::SliderBar(double lo, double hi, double def, std::vector<std::pair<double, QColor>> gradient,
                     QWidget* parent)
    : QWidget(parent), v_(def), lo_(lo), hi_(hi), def_(def), grad_(std::move(gradient)) {
    setMinimumWidth(70);
    setFixedHeight(18);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SliderBar::setValue(double v) {
    v_ = std::clamp(v, lo_, hi_);
    update();
}

double SliderBar::posOf(double v) const { return 6 + (v - lo_) / (hi_ - lo_) * (width() - 12); }
double SliderBar::valOf(double x) const { return lo_ + (x - 6) / std::max(1, width() - 12) * (hi_ - lo_); }

void SliderBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double y = height() / 2.0;
    const QRectF track(6, y - 2, width() - 12, 4);
    p.setPen(Qt::NoPen);
    if (!grad_.empty()) {
        QLinearGradient g(track.left(), 0, track.right(), 0);
        for (auto& [pos, c] : grad_) g.setColorAt(pos, c);
        p.setBrush(g);
        p.drawRoundedRect(track.adjusted(0, -1, 0, 1), 2, 2);
    } else {
        p.setBrush(QColor("#3c3c3c"));
        p.drawRoundedRect(track, 2, 2);
        const double x0 = posOf(def_), x1 = posOf(v_);
        if (std::abs(x1 - x0) > 0.5) {
            p.setBrush(pressed_ ? theme::kAccent : QColor("#7a7a7a"));
            p.drawRect(QRectF(std::min(x0, x1), y - 2, std::abs(x1 - x0), 4));
        }
    }
    p.setBrush((pressed_ || underMouse()) ? QColor("#e8e8e8") : QColor("#b8b8b8"));
    p.setPen(QPen(QColor("#1a1a1a"), 1));
    p.drawEllipse(QPointF(posOf(v_), y), 5.5, 5.5);
}

void SliderBar::enterEvent(QEnterEvent*) {
    emit hovered(true);
    update();
}

void SliderBar::leaveEvent(QEvent*) {
    emit hovered(false);
    update();
}

void SliderBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    pressed_ = true;
    const double x = e->position().x();
    if (std::abs(x - posOf(v_)) <= 7) {
        grabOff_ = x - posOf(v_);
    } else {
        grabOff_ = 0;
        v_ = std::clamp(valOf(x), lo_, hi_);
        emit valueChanged(v_, false);
    }
    anchorX_ = x;
    anchorV_ = v_;
    update();
}

void SliderBar::mouseMoveEvent(QMouseEvent* e) {
    if (!pressed_) return;
    const double x = e->position().x();
    const double f = modFactor(e->modifiers());
    double v;
    if (f != 1.0) {
        const double span = (hi_ - lo_) / std::max(1, width() - 12);
        v = anchorV_ + (x - anchorX_) * span * f;
    } else {
        v = valOf(x - grabOff_);
    }
    v_ = std::clamp(v, lo_, hi_);
    anchorX_ = x;
    anchorV_ = v_;
    emit valueChanged(v_, false);
    update();
}

void SliderBar::mouseReleaseEvent(QMouseEvent*) {
    if (!pressed_) return;
    pressed_ = false;
    emit valueChanged(v_, true);
    update();
}

void SliderBar::mouseDoubleClickEvent(QMouseEvent*) {
    v_ = def_;
    emit valueChanged(v_, true);
    update();
}

void SliderBar::wheelEvent(QWheelEvent* e) {
    if (!wheelAdjustEnabled()) {
        e->ignore();
        return;
    }
    const int d = e->angleDelta().y() > 0 ? 1 : -1;
    v_ = std::clamp(v_ + d * (hi_ - lo_) / 100.0 * modFactor(e->modifiers()), lo_, hi_);
    emit valueChanged(v_, true);
    update();
}

// ================================================================ ParamRow

ParamRow::ParamRow(QString key, QString label, double lo, double hi, double def, int decimals, double step,
                   QString suffix, std::vector<std::pair<double, QColor>> gradient,
                   std::function<double(double)> toSlider, std::function<double(double)> fromSlider, int scrubWidth,
                   QWidget* parent)
    : QWidget(parent), key_(std::move(key)), to_(std::move(toSlider)), from_(std::move(fromSlider)) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 1, 8, 1);
    lay->setSpacing(6);
    auto* lab = new QLabel(label);
    lab->setFixedWidth(78);
    lab->setStyleSheet("color:#8c8c8c; background: transparent;");
    slider_ = to_ ? new SliderBar(0, 1, to_(def), std::move(gradient)) : new SliderBar(lo, hi, def, std::move(gradient));
    field_ = new ScrubField(def, lo, hi, def, decimals, step, std::move(suffix), scrubWidth);
    lay->addWidget(lab);
    lay->addWidget(slider_, 1);
    lay->addWidget(field_);
    connect(slider_, &SliderBar::valueChanged, this, [this](double v, bool final) {
        field_->setValue(from_ ? from_(v) : v);
        emit changed(key_, field_->value(), final);
    });
    connect(field_, &ScrubField::valueChanged, this, [this](double v, bool final) {
        slider_->setValue(to_ ? to_(v) : v);
        emit changed(key_, v, final);
    });
    connect(slider_, &SliderBar::hovered, this, [this](bool on) { emit hovered(key_, on); });
}

void ParamRow::setValue(double v) {
    field_->setValue(v);
    slider_->setValue(to_ ? to_(v) : v);
}

double ParamRow::value() const { return field_->value(); }

// ================================================================ Section

Section::Section(QString title, bool collapsed, bool hasToggle, QWidget* parent) : QFrame(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 6);
    v->setSpacing(2);
    auto* head = new QWidget;
    head->setObjectName("SectionHead");
    head->setStyleSheet("QWidget#SectionHead { background: #2e2e2e; }");
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(4);
    arrow_ = new QToolButton;
    arrow_->setArrowType(Qt::DownArrow);
    arrow_->setStyleSheet("border:none;");
    power_ = new QToolButton;
    power_->setCheckable(true);
    power_->setChecked(true);
    power_->setText(QStringLiteral("●"));
    power_->setToolTip(QStringLiteral("このパネルのオン/オフ（値は残ります）\nAlt＋クリック：このパネルだけオン"));
    power_->setStyleSheet("QToolButton { color: #666; border: 1px solid #444; border-radius: 9px; padding: 0 3px; }"
                          "QToolButton:checked { color: #d28b2c; border-color: #d28b2c; background: transparent; }");
    power_->setFixedSize(20, 20);
    power_->setVisible(hasToggle);
    title_ = new QLabel(title);
    title_->setStyleSheet("background: transparent; font-weight: 600;");
    auto* reset = new QToolButton;
    reset->setText(QStringLiteral("↺"));
    reset->setToolTip(QStringLiteral("このパネルを初期値に戻す"));
    extra_ = new QHBoxLayout;
    extra_->setSpacing(2);
    h->addWidget(arrow_);
    h->addWidget(power_);
    h->addWidget(title_, 1);
    h->addLayout(extra_);
    h->addWidget(reset);
    v->addWidget(head);
    body_ = new QWidget;
    body_->setStyleSheet("background: #262626;");
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(0, 4, 0, 2);
    bodyLayout_->setSpacing(1);
    v->addWidget(body_);

    connect(arrow_, &QToolButton::clicked, this, [this] { setCollapsed(body_->isVisible()); });
    connect(reset, &QToolButton::clicked, this, &Section::resetRequested);
    connect(power_, &QToolButton::clicked, this, [this](bool on) {
        if (QApplication::keyboardModifiers() & Qt::AltModifier) {
            power_->setChecked(on_);
            emit soloRequested();
            return;
        }
        setEnabledState(on);
        emit toggled(on);
    });
    setCollapsed(collapsed);
}

void Section::add(QWidget* w) { bodyLayout_->addWidget(w); }

void Section::setCollapsed(bool c) {
    body_->setVisible(!c);
    arrow_->setArrowType(c ? Qt::RightArrow : Qt::DownArrow);
}

void Section::setEnabledState(bool on) {
    on_ = on;
    power_->setChecked(on);
    title_->setStyleSheet(on ? "background: transparent; font-weight: 600;"
                             : "background: transparent; font-weight: 600; color: #666;");
    // オフのときは中身を半透明に見せる（操作はできる）
    auto* eff = body_->graphicsEffect();
    if (!on && !eff) {
        auto* op = new QGraphicsOpacityEffect(body_);
        op->setOpacity(0.45);
        body_->setGraphicsEffect(op);
    } else if (on && eff) {
        body_->setGraphicsEffect(nullptr);
    }
}

// ================================================================ ColorWheel

ColorWheel::ColorWheel(QString title, QWidget* parent) : QWidget(parent), title_(std::move(title)) {
    setMinimumSize(64, 118);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("円の中をドラッグ：色味　下のバーを左右ドラッグ：明るさ\nダブルクリック：リセット　Ctrl：微調整"));
}

void ColorWheel::setValues(const std::array<float, 3>& v) {
    v_ = v;
    update();
}

void ColorWheel::geom(double& cx, double& cy, double& r) const {
    const double d = std::max(40.0, std::min(width() - 8.0, 80.0));
    cx = width() / 2.0;
    cy = 16 + d / 2;
    r = d / 2;
}

void ColorWheel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    double cx, cy, r;
    geom(cx, cy, r);
    p.setPen(theme::kTextDim);
    p.drawText(QRectF(0, 0, width(), 14), Qt::AlignCenter, title_);
    QConicalGradient cg(QPointF(cx, cy), 0);
    const char* cols[] = {"#ff4040", "#ff40ff", "#4040ff", "#40ffff", "#40ff40", "#ffff40", "#ff4040"};
    for (int i = 0; i < 7; ++i) cg.setColorAt(i / 6.0, QColor(cols[i]));
    p.setPen(Qt::NoPen);
    p.setBrush(cg);
    p.drawEllipse(QPointF(cx, cy), r, r);
    QRadialGradient rg(QPointF(cx, cy), r);
    rg.setColorAt(0, QColor(40, 40, 40, 255));
    rg.setColorAt(0.85, QColor(40, 40, 40, 200));
    rg.setColorAt(1, QColor(40, 40, 40, 60));
    p.setBrush(rg);
    p.drawEllipse(QPointF(cx, cy), r - 3, r - 3);
    p.setPen(QPen(QColor("#555"), 1));
    p.drawLine(QPointF(cx - 5, cy), QPointF(cx + 5, cy));
    p.drawLine(QPointF(cx, cy - 5), QPointF(cx, cy + 5));
    const double px = cx + v_[0] * (r - 6), py = cy - v_[1] * (r - 6);
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush((v_[0] != 0 || v_[1] != 0) ? QBrush(theme::kAccent) : Qt::NoBrush);
    p.drawEllipse(QPointF(px, py), 4, 4);
    const double by = cy + r + 8;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#3c3c3c"));
    p.drawRoundedRect(QRectF(cx - r, by, 2 * r, 5), 2, 2);
    const double mx = cx + std::clamp<double>(v_[2], -1, 1) * r;
    p.setBrush(v_[2] != 0 ? theme::kAccent : QColor("#999"));
    p.drawRect(QRectF(std::min(cx, mx), by, std::abs(mx - cx), 5));
    const bool any = v_[0] != 0 || v_[1] != 0 || v_[2] != 0;
    p.setPen(any ? theme::kText : theme::kTextDim);
    p.drawText(QRectF(0, by + 6, width(), 14), Qt::AlignCenter, QString::asprintf("%+.2f", v_[2]));
}

void ColorWheel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    double cx, cy, r;
    geom(cx, cy, r);
    mode_ = e->position().y() > cy + r + 2 ? 2 : 1;
    last_ = e->position();
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e) {
    if (!mode_) return;
    double cx, cy, r;
    geom(cx, cy, r);
    const double f = (e->modifiers() & Qt::ControlModifier) ? 0.15 : 1.0;
    const QPointF pos = e->position();
    if (mode_ == 1) {
        double u = v_[0] + (pos.x() - last_.x()) / (r - 6) * f;
        double v = v_[1] - (pos.y() - last_.y()) / (r - 6) * f;
        const double n = std::hypot(u, v);
        if (n > 1) {
            u /= n;
            v /= n;
        }
        v_[0] = static_cast<float>(u);
        v_[1] = static_cast<float>(v);
    } else {
        v_[2] = static_cast<float>(std::clamp(v_[2] + (pos.x() - last_.x()) / 200.0 * f, -1.0, 1.0));
    }
    last_ = pos;
    update();
    emit changed(v_, false);
}

void ColorWheel::mouseReleaseEvent(QMouseEvent*) {
    if (!mode_) return;
    mode_ = 0;
    emit changed(v_, true);
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent* e) {
    double cx, cy, r;
    geom(cx, cy, r);
    if (e->position().y() > cy + r + 2)
        v_[2] = 0;
    else
        v_[0] = v_[1] = 0;
    update();
    emit changed(v_, true);
}

void ColorWheel::wheelEvent(QWheelEvent* e) {
    if (!wheelAdjustEnabled()) {
        e->ignore();
        return;
    }
    const int d = e->angleDelta().y() > 0 ? 1 : -1;
    v_[2] = static_cast<float>(std::clamp(v_[2] + d * 0.02 * modFactor(e->modifiers()), -1.0, 1.0));
    update();
    emit changed(v_, true);
}

}  // namespace rd
