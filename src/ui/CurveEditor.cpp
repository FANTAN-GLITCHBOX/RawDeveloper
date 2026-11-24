#include "ui/CurveEditor.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "engine/Pipeline.h"
#include "ui/Theme.h"

namespace rd {

namespace {
const QColor kChColors[4] = {QColor("#e6e6e6"), QColor("#ff5a5a"), QColor("#5ad05a"), QColor("#5a8cff")};
}

CurveCanvas::CurveCanvas(QWidget* parent) : QWidget(parent) {
    for (auto& c : curves_) c = {{0, 0}, {1, 1}};
    setMinimumHeight(200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
}

void CurveCanvas::setCurves(const std::array<CurvePoints, 4>& c) {
    curves_ = c;
    update();
}

void CurveCanvas::setHistogram(const std::vector<float>& h) {
    hist_ = h;
    update();
}

void CurveCanvas::setChannel(int ch) {
    ch_ = ch;
    update();
}

QRectF CurveCanvas::plotRect() const {
    const double s = std::min(width(), height()) - 16.0;
    return QRectF((width() - s) / 2, 8, s, s);
}

QPointF CurveCanvas::toPx(double x, double y) const {
    const QRectF r = plotRect();
    return {r.left() + x * r.width(), r.bottom() - y * r.height()};
}

QPointF CurveCanvas::fromPx(const QPointF& p) const {
    const QRectF r = plotRect();
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - p.y()) / r.height(), 0.0, 1.0)};
}

void CurveCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(r, QColor("#202020"));
    if (!hist_.empty()) {
        const float mx = std::max(1.0f, *std::max_element(hist_.begin(), hist_.end()));
        QPainterPath path(QPointF(r.left(), r.bottom()));
        for (size_t i = 0; i < hist_.size(); ++i)
            path.lineTo(r.left() + double(i) / (hist_.size() - 1) * r.width(), r.bottom() - hist_[i] / mx * r.height() * 0.9);
        path.lineTo(r.right(), r.bottom());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 28));
        p.drawPath(path);
    }
    p.setPen(QPen(QColor("#333"), 1));
    for (int i = 1; i < 4; ++i) {
        const double x = r.left() + r.width() * i / 4, y = r.top() + r.height() * i / 4;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(QColor("#444"), 1, Qt::DashLine));
    p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()));
    std::vector<float> xs(128);
    for (int i = 0; i < 128; ++i) xs[i] = i / 127.0f;
    for (int ch : {1, 2, 3, 0}) {
        if (ch != ch_ && isIdentityCurve(curves_[ch])) continue;
        const auto ys = monotoneSpline(curves_[ch], xs);
        QPainterPath path(toPx(xs[0], ys[0]));
        for (int i = 1; i < 128; ++i) path.lineTo(toPx(xs[i], ys[i]));
        QColor c = kChColors[ch];
        if (ch != ch_) c.setAlpha(90);
        p.setPen(QPen(c, ch == ch_ ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    const auto& pts = curves_[ch_];
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        p.setPen(QPen(Qt::white, 1.2));
        p.setBrush(i == drag_ ? theme::kAccent : QColor("#333"));
        p.drawEllipse(toPx(pts[i].first, pts[i].second), 4, 4);
    }
    if (hover_) {
        p.setPen(theme::kTextDim);
        p.drawText(QRectF(r.left() + 4, r.top() + 2, 140, 14), Qt::AlignLeft,
                   QString("%1 → %2").arg(int(hoverVal_.x() * 255)).arg(int(hoverVal_.y() * 255)));
    }
}

int CurveCanvas::nearest(const QPointF& pos) const {
    int best = -1;
    double bd = 1e9;
    const auto& pts = curves_[ch_];
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const double d = (toPx(pts[i].first, pts[i].second) - pos).manhattanLength();
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return bd < 12 ? best : -1;
}

void CurveCanvas::mousePressEvent(QMouseEvent* e) {
    auto& pts = curves_[ch_];
    int i = nearest(e->position());
    if (e->button() == Qt::RightButton) {
        if (i > 0 && i < static_cast<int>(pts.size()) - 1) {
            pts.erase(pts.begin() + i);
            update();
            emit changed(curves_, true);
        }
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (i < 0) {
        QPointF v = fromPx(e->position());
        if (e->modifiers() & Qt::ShiftModifier) v.setY(monotoneSpline(pts, {static_cast<float>(v.x())})[0]);
        pts.emplace_back(static_cast<float>(v.x()), static_cast<float>(v.y()));
        std::sort(pts.begin(), pts.end());
        for (int k = 0; k < static_cast<int>(pts.size()); ++k)
            if (pts[k].first == static_cast<float>(v.x())) i = k;
    }
    drag_ = i;
    update();
}

void CurveCanvas::mouseMoveEvent(QMouseEvent* e) {
    const QPointF v = fromPx(e->position());
    hover_ = true;
    hoverVal_ = QPointF(v.x(), monotoneSpline(curves_[ch_], {static_cast<float>(v.x())})[0]);
    if (drag_ >= 0) {
        auto& pts = curves_[ch_];
        const int n = static_cast<int>(pts.size());
        double lo = drag_ > 0 ? pts[drag_ - 1].first + 0.01 : 0.0;
        double hi = drag_ < n - 1 ? pts[drag_ + 1].first - 0.01 : 1.0;
        if (drag_ == 0) hi = std::min(hi, 0.99);
        if (drag_ == n - 1) lo = std::max(lo, 0.01);
        pts[drag_] = {static_cast<float>(std::clamp(v.x(), lo, hi)), static_cast<float>(v.y())};
        emit changed(curves_, false);
    }
    update();
}

void CurveCanvas::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ < 0) return;
    drag_ = -1;
    update();
    emit changed(curves_, true);
}

void CurveCanvas::mouseDoubleClickEvent(QMouseEvent*) {
    curves_[ch_] = {{0, 0}, {1, 1}};
    update();
    emit changed(curves_, true);
}

void CurveCanvas::leaveEvent(QEvent*) {
    hover_ = false;
    update();
}

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 0, 8, 0);
    auto* top = new QHBoxLayout;
    group_ = new QButtonGroup(this);
    const char* names[4] = {"Y", "R", "G", "B"};
    canvas_ = new CurveCanvas;
    for (int i = 0; i < 4; ++i) {
        auto* b = new QToolButton;
        b->setText(names[i]);
        b->setCheckable(true);
        b->setFixedWidth(28);
        b->setStyleSheet(QString("color:%1; font-weight:600;").arg(kChColors[i].name()));
        group_->addButton(b, i);
        top->addWidget(b);
        if (i == 0) b->setChecked(true);
    }
    connect(group_, &QButtonGroup::idClicked, canvas_, &CurveCanvas::setChannel);
    top->addStretch(1);
    v->addLayout(top);
    v->addWidget(canvas_);
}

}  // namespace rd
