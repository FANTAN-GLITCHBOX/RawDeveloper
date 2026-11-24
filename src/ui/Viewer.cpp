#include "ui/Viewer.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>

namespace rd {

Viewer::Viewer(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(320, 240);
}

void Viewer::setImage(const QImage& img) {
    img_ = img;
    message_.clear();
    update();
}

void Viewer::setMessage(const QString& msg) {
    message_ = msg;
    update();
}

QRectF Viewer::imageRect() const {
    if (img_.isNull()) return {};
    const double fit = std::min((width() - 20.0) / img_.width(), (height() - 20.0) / img_.height());
    const double z = zoom_ > 0 ? zoom_ : fit;
    const double w = img_.width() * z, h = img_.height() * z;
    return QRectF(width() / 2.0 - w / 2 + offset_.x(), height() / 2.0 - h / 2 + offset_.y(), w, h);
}

void Viewer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x19, 0x19, 0x19));
    if (!img_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(imageRect(), img_);
    }
    if (!message_.isEmpty() || img_.isNull()) {
        p.setPen(QColor(0xd2, 0x8b, 0x2c));
        p.drawText(rect(), Qt::AlignCenter,
                   message_.isEmpty() ? QStringLiteral("RAW ファイルを開くか、ここにドラッグしてください") : message_);
    }
}

void Viewer::wheelEvent(QWheelEvent* e) {
    if (img_.isNull()) return;
    const QRectF r = imageRect();
    const double cur = r.width() / img_.width();
    const double z = std::clamp(cur * (e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15), 0.05, 8.0);
    // カーソル位置を中心にズーム
    const QPointF pos = e->position();
    const double rx = (pos.x() - r.left()) / r.width(), ry = (pos.y() - r.top()) / r.height();
    const double nw = img_.width() * z, nh = img_.height() * z;
    offset_ = QPointF(pos.x() - rx * nw + nw / 2 - width() / 2.0, pos.y() - ry * nh + nh / 2 - height() / 2.0);
    zoom_ = z;
    update();
}

void Viewer::mouseDoubleClickEvent(QMouseEvent*) {
    zoom_ = 0;
    offset_ = {};
    update();
}

void Viewer::mousePressEvent(QMouseEvent* e) {
    dragStart_ = e->position();
    offsetStart_ = offset_;
}

void Viewer::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & (Qt::LeftButton | Qt::MiddleButton)) {
        offset_ = offsetStart_ + (e->position() - dragStart_);
        update();
    }
}

}  // namespace rd
