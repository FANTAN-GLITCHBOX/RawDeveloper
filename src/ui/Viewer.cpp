#include "ui/Viewer.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <map>

#include "ui/Theme.h"

namespace rd {

namespace {
// 比率の違う画像（切り抜きなしの補正前など）を、枠の中に比率を保って収める
QRectF fitInto(const QRectF& r, const QImage& img) {
    if (img.isNull()) return r;
    const double ia = double(img.width()) / img.height(), ra = r.width() / r.height();
    if (std::abs(ia - ra) < 0.01) return r;
    if (ia > ra) {
        const double h = r.width() / ia;
        return QRectF(r.left(), r.center().y() - h / 2, r.width(), h);
    }
    const double w = r.height() * ia;
    return QRectF(r.center().x() - w / 2, r.top(), w, r.height());
}
}  // namespace

Viewer::Viewer(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
}

void Viewer::clear(const QString& message) {
    img_ = before_ = mask_ = clipCache_ = QImage();
    message_ = message;
    update();
}

void Viewer::setImage(const QImage& img, QSize fullSize, bool hires) {
    const QSize newFull = fullSize.isValid() ? fullSize : img.size();
    if (newFull != full_) {
        // 切り抜きなどで大きさが変わっても拡大率は保ち、表示位置だけ収める
        roi_ = roiBefore_ = QImage();
        full_ = newFull;
    }
    img_ = img;
    hires_ = hires;
    message_.clear();
    clipCache_ = QImage();
    mask_ = QImage();
    if (fit_) {
        zoom_ = fitZoom();
        offsets_ = {};
    }
    clamp();
    update();
}

void Viewer::resetView() {
    fit_ = true;
    offsets_ = {};
    roi_ = roiBefore_ = QImage();
    zoom_ = fitZoom();
    update();
}

void Viewer::setRoi(const QImage& img, const QRect& rectFull) {
    roi_ = img;
    roiRect_ = rectFull;
    update();
}

void Viewer::setBeforeRoi(const QImage& img, const QRect& rectFull) {
    roiBefore_ = img;
    roiBeforeRect_ = rectFull;
    update();
}

QRect Viewer::visibleFullRect() const {
    if (img_.isNull()) return {};
    const auto ps = panes();
    int idx = 0;
    if (sideBySide()) idx = swapped_ ? 0 : 1;  // 補正後を表示している側
    const QRectF r = imageRect(ps[idx], idx);
    const QRectF vis = r.intersected(ps[idx]);
    if (vis.isEmpty()) return {};
    const double sx = full_.width() / r.width(), sy = full_.height() / r.height();
    QRect out(int(std::floor((vis.left() - r.left()) * sx)), int(std::floor((vis.top() - r.top()) * sy)),
              int(std::ceil(vis.width() * sx)) + 1, int(std::ceil(vis.height() * sy)) + 1);
    return out.intersected(QRect(QPoint(0, 0), full_));
}

void Viewer::setCompare(CompareMode m) {
    compare_ = m;
    if (fit_) zoom_ = fitZoom();
    clamp();
    update();
    emit viewChanged();
}

bool Viewer::sideBySide() const {
    return !cropMode_ && !before_.isNull() &&
           (compare_ == CompareMode::SideH || compare_ == CompareMode::SideV || compare_ == CompareMode::SideAuto);
}

bool Viewer::sideHorizontal() const {
    if (compare_ == CompareMode::SideH) return true;
    if (compare_ == CompareMode::SideV) return false;
    // 自動：写真を大きく見せられる方（縦長なら左右、横長なら上下）
    const double ia = double(full_.width()) / std::max(1, full_.height());
    const double fitLR = std::min(width() / 2.0 / full_.width(), double(height()) / full_.height());
    const double fitTB = std::min(double(width()) / full_.width(), height() / 2.0 / full_.height());
    return ia < 1.0 ? true : fitLR >= fitTB;
}

std::array<QRectF, 2> Viewer::panes() const {
    const QRectF all(0, 0, width(), height());
    if (!sideBySide()) return {all, QRectF()};
    if (sideHorizontal())
        return {QRectF(0, 0, width() / 2.0 - 1, height()), QRectF(width() / 2.0 + 1, 0, width() / 2.0 - 1, height())};
    return {QRectF(0, 0, width(), height() / 2.0 - 1), QRectF(0, height() / 2.0 + 1, width(), height() / 2.0 - 1)};
}

double Viewer::fitZoom() const {
    const QRectF p = panes()[0];
    return std::min((p.width() - 20) / std::max(1, full_.width()), (p.height() - 20) / std::max(1, full_.height()));
}

QRectF Viewer::imageRect(const QRectF& pane, int idx) const {
    const double w = full_.width() * zoom_, h = full_.height() * zoom_;
    const QPointF o = offsets_[linked_ ? 0 : idx];
    return QRectF(pane.center().x() - w / 2 + o.x(), pane.center().y() - h / 2 + o.y(), w, h);
}

double Viewer::displayScaleOfImage() const { return img_.isNull() ? 1.0 : double(img_.width()) / full_.width(); }

void Viewer::zoomFit() { setZoom(0, {}, true); }
void Viewer::zoom100() { setZoom(1.0, QPointF(width() / 2.0, height() / 2.0), false); }
void Viewer::toggleZoom() { fit_ ? zoom100() : zoomFit(); }

void Viewer::setZoom(double z, QPointF anchor, bool fit) {
    const auto ps = panes();
    const int idx = std::max(0, paneAt(anchor));
    const QRectF pane = ps[idx];
    const QRectF old = imageRect(pane, idx);
    const double fz = fitZoom();
    fit_ = fit;
    z = fit ? fz : std::clamp(z, fz * 0.5, 8.0);
    if (fit) {
        offsets_ = {};
    } else {
        if (anchor.isNull()) anchor = pane.center();
        const double rx = (anchor.x() - old.left()) / std::max(1.0, old.width());
        const double ry = (anchor.y() - old.top()) / std::max(1.0, old.height());
        const double nw = full_.width() * z, nh = full_.height() * z;
        const QPointF o(anchor.x() - rx * nw + nw / 2 - pane.center().x(), anchor.y() - ry * nh + nh / 2 - pane.center().y());
        if (linked_)
            offsets_ = {o, o};
        else
            offsets_[idx] = o;
    }
    zoom_ = z;
    clamp();
    if (!img_.isNull() && zoom_ > displayScaleOfImage() * 1.05) emit wantHiRes();
    if (fit_) roi_ = roiBefore_ = QImage();
    update();
    emit viewChanged();
}

void Viewer::clamp() {
    const auto ps = panes();
    for (int i = 0; i < 2; ++i) {
        if (ps[i].isNull()) continue;
        const QRectF r = imageRect(ps[i], i);
        QPointF& o = offsets_[linked_ ? 0 : i];
        if (r.width() <= ps[i].width())
            o.setX(0);
        else if (r.left() > ps[i].left())
            o.rx() -= r.left() - ps[i].left();
        else if (r.right() < ps[i].right())
            o.rx() += ps[i].right() - r.right();
        if (r.height() <= ps[i].height())
            o.setY(0);
        else if (r.top() > ps[i].top())
            o.ry() -= r.top() - ps[i].top();
        else if (r.bottom() < ps[i].bottom())
            o.ry() += ps[i].bottom() - r.bottom();
    }
    if (linked_) offsets_[1] = offsets_[0];
}

int Viewer::paneAt(const QPointF& p) const {
    const auto ps = panes();
    if (!ps[1].isNull() && ps[1].contains(p)) return 1;
    return 0;
}

void Viewer::resizeEvent(QResizeEvent*) {
    if (fit_) zoom_ = fitZoom();
    clamp();
    emit viewChanged();
}

QImage Viewer::clipOverlay() {
    if (clipCache_.isNull() && !img_.isNull()) {
        const QImage src = img_.convertToFormat(QImage::Format_RGB888);
        clipCache_ = QImage(src.size(), QImage::Format_RGBA8888);
        clipCache_.fill(Qt::transparent);
        for (int y = 0; y < src.height(); ++y) {
            const uchar* s = src.constScanLine(y);
            uchar* d = clipCache_.scanLine(y);
            for (int x = 0; x < src.width(); ++x) {
                const uchar r = s[x * 3], g = s[x * 3 + 1], b = s[x * 3 + 2];
                if (r >= 254 || g >= 254 || b >= 254) {
                    d[x * 4] = 255; d[x * 4 + 1] = 40; d[x * 4 + 2] = 40; d[x * 4 + 3] = 230;
                } else if (r <= 1 && g <= 1 && b <= 1) {
                    d[x * 4] = 40; d[x * 4 + 1] = 90; d[x * 4 + 2] = 255; d[x * 4 + 3] = 230;
                }
            }
        }
    }
    return clipCache_;
}

void Viewer::setMaskRange(int lo, int hi) {
    mask_ = QImage();
    if (lo >= 0 && !img_.isNull()) {
        const QImage src = img_.convertToFormat(QImage::Format_RGB888);
        mask_ = QImage(src.size(), QImage::Format_RGBA8888);
        mask_.fill(Qt::transparent);
        for (int y = 0; y < src.height(); ++y) {
            const uchar* s = src.constScanLine(y);
            uchar* d = mask_.scanLine(y);
            for (int x = 0; x < src.width(); ++x) {
                const int l = (s[x * 3] * 54 + s[x * 3 + 1] * 183 + s[x * 3 + 2] * 19) >> 8;
                if (l >= lo && l <= hi) {
                    d[x * 4] = 210; d[x * 4 + 1] = 139; d[x * 4 + 2] = 44; d[x * 4 + 3] = 170;
                }
            }
        }
    }
    update();
}

// ---------------------------------------------------------------- paint

namespace {
void drawRoi(QPainter& p, const QRectF& r, QSize full, const QImage& img, const QRect& rect) {
    if (img.isNull()) return;
    p.drawImage(QRectF(r.left() + rect.x() * r.width() / full.width(), r.top() + rect.y() * r.height() / full.height(),
                       rect.width() * r.width() / full.width(), rect.height() * r.height() / full.height()),
                img);
}
}  // namespace

void Viewer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x19, 0x19, 0x19));
    QFont small = font();
    small.setPointSizeF(8);
    if (img_.isNull()) {
        p.setPen(theme::kTextDim);
        p.drawText(rect(), Qt::AlignCenter, message_.isEmpty() ? QStringLiteral("写真がありません") : message_);
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 2.0);
    const auto ps = panes();
    if (sideBySide()) {
        const QImage* imgs[2] = {&before_, &img_};
        QString labels[2] = {beforeLabel_, QStringLiteral("補正後")};
        if (swapped_) {
            std::swap(imgs[0], imgs[1]);
            std::swap(labels[0], labels[1]);
        }
        for (int i = 0; i < 2; ++i) {
            p.save();
            p.setClipRect(ps[i]);
            const QRectF r = imageRect(ps[i], i);
            p.drawImage(imgs[i] == &before_ ? fitInto(r, before_) : r, *imgs[i]);
            if (imgs[i] == &before_) drawRoi(p, r, full_, roiBefore_, roiBeforeRect_);
            if (imgs[i] == &img_) {
                if (!roi_.isNull()) p.drawImage(QRectF(r.left() + roiRect_.x() * r.width() / full_.width(),
                                                       r.top() + roiRect_.y() * r.height() / full_.height(),
                                                       roiRect_.width() * r.width() / full_.width(),
                                                       roiRect_.height() * r.height() / full_.height()), roi_);
                if (clip_) p.drawImage(r, clipOverlay());
                if (!mask_.isNull()) p.drawImage(r, mask_);
            }
            p.setFont(small);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawRoundedRect(QRectF(ps[i].left() + 8, ps[i].top() + 8, 70, 18), 3, 3);
            p.setPen(Qt::white);
            p.drawText(QRectF(ps[i].left() + 8, ps[i].top() + 8, 70, 18), Qt::AlignCenter, labels[i]);
            p.restore();
        }
        p.setPen(QPen(QColor("#555"), 2));
        if (sideHorizontal())
            p.drawLine(QPointF(width() / 2.0, 0), QPointF(width() / 2.0, height()));
        else
            p.drawLine(QPointF(0, height() / 2.0), QPointF(width(), height() / 2.0));
    } else {
        const QRectF r = imageRect(ps[0], 0);
        const bool wipe = (compare_ == CompareMode::WipeV || compare_ == CompareMode::WipeH) && !before_.isNull() && !cropMode_;
        if (bypass_ && !before_.isNull() && !cropMode_) {
            p.drawImage(fitInto(r, before_), before_);
            drawRoi(p, r, full_, roiBefore_, roiBeforeRect_);
            p.setPen(theme::kAccent);
            p.drawText(QRectF(8, 6, 300, 16), Qt::AlignLeft, QStringLiteral("補正前を表示中（Shift+D で戻る）"));
        } else {
            p.drawImage(r, img_);
            if (!roi_.isNull() && !cropMode_)
                p.drawImage(QRectF(r.left() + roiRect_.x() * r.width() / full_.width(),
                                   r.top() + roiRect_.y() * r.height() / full_.height(),
                                   roiRect_.width() * r.width() / full_.width(),
                                   roiRect_.height() * r.height() / full_.height()), roi_);
            if (wipe) {
                p.save();
                if (compare_ == CompareMode::WipeV)
                    p.setClipRect(QRectF(r.left(), r.top(), r.width() * split_, r.height()));
                else
                    p.setClipRect(QRectF(r.left(), r.top(), r.width(), r.height() * split_));
                p.fillRect(r, QColor(0x19, 0x19, 0x19));
                p.drawImage(fitInto(r, before_), before_);
                drawRoi(p, r, full_, roiBefore_, roiBeforeRect_);
                p.restore();
                p.setPen(QPen(Qt::white, 1.5));
                p.setFont(small);
                p.setBrush(Qt::white);
                if (compare_ == CompareMode::WipeV) {
                    const double sx = r.left() + r.width() * split_;
                    p.drawLine(QPointF(sx, r.top()), QPointF(sx, r.bottom()));
                    p.drawText(QPointF(sx - 60, r.top() + 16), beforeLabel_);
                    p.drawText(QPointF(sx + 8, r.top() + 16), QStringLiteral("補正後"));
                    p.drawEllipse(QPointF(sx, r.center().y()), 6, 6);
                } else {
                    const double sy = r.top() + r.height() * split_;
                    p.drawLine(QPointF(r.left(), sy), QPointF(r.right(), sy));
                    p.drawText(QPointF(r.left() + 8, sy - 6), beforeLabel_);
                    p.drawText(QPointF(r.left() + 8, sy + 16), QStringLiteral("補正後"));
                    p.drawEllipse(QPointF(r.center().x(), sy), 6, 6);
                }
            }
            if (!cropMode_) {
                if (clip_) p.drawImage(r, clipOverlay());
                if (!mask_.isNull()) p.drawImage(r, mask_);
            }
        }
        if (cropMode_) drawCrop(p, r);
        if (overlay_ && !cropMode_ && !bypass_) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            overlay_(p, r);
            p.restore();
        }
    }
    p.setFont(small);
    p.setPen(theme::kTextDim);
    const QString z = fit_ ? QStringLiteral("全体") : QString("%1%").arg(qRound(zoom_ * 100));
    p.drawText(QRectF(8, height() - 20, width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter,
               QString("%1   %2×%3%4").arg(z).arg(full_.width()).arg(full_.height())
                   .arg((hires_ || !roi_.isNull()) ? QStringLiteral("　高精細") : ""));
    if (!message_.isEmpty()) {
        p.setPen(theme::kAccent);
        p.drawText(QRectF(8, 6, width() - 16, 16), Qt::AlignRight, message_);
    }
}

QRectF Viewer::cropPx(const QRectF& r) const {
    return QRectF(r.left() + crop_.x * r.width(), r.top() + crop_.y * r.height(), crop_.w * r.width(), crop_.h * r.height());
}

namespace {
std::map<QString, QPointF> handles(const QRectF& c) {
    return {{"tl", c.topLeft()}, {"tr", c.topRight()}, {"bl", c.bottomLeft()}, {"br", c.bottomRight()},
            {"t", QPointF(c.center().x(), c.top())}, {"b", QPointF(c.center().x(), c.bottom())},
            {"l", QPointF(c.left(), c.center().y())}, {"r", QPointF(c.right(), c.center().y())}};
}
}  // namespace

void Viewer::drawCrop(QPainter& p, const QRectF& r) {
    const QRectF c = cropPx(r);
    const QColor shade(0, 0, 0, 150);
    p.fillRect(QRectF(r.left(), r.top(), r.width(), c.top() - r.top()), shade);
    p.fillRect(QRectF(r.left(), c.bottom(), r.width(), r.bottom() - c.bottom()), shade);
    p.fillRect(QRectF(r.left(), c.top(), c.left() - r.left(), c.height()), shade);
    p.fillRect(QRectF(c.right(), c.top(), r.right() - c.right(), c.height()), shade);
    if (!angleHint_.isEmpty()) {
        // 水平・垂直を合わせやすい細かいグリッド（正方形のマス）
        p.setPen(QPen(QColor(255, 255, 255, 70), 1));
        const double cell = std::max(16.0, std::min(c.width(), c.height()) / 12);
        for (double x = c.left() + cell; x < c.right() - 1; x += cell) p.drawLine(QPointF(x, c.top()), QPointF(x, c.bottom()));
        for (double y = c.top() + cell; y < c.bottom() - 1; y += cell) p.drawLine(QPointF(c.left(), y), QPointF(c.right(), y));
        QFont f = p.font();
        f.setPointSizeF(f.pointSizeF() * 1.6);
        f.setBold(true);
        p.save();
        p.setFont(f);
        const QRectF tr(c.right() - 200, c.bottom() - 44, 192, 36);
        p.fillRect(tr, QColor(0, 0, 0, 160));
        p.setPen(Qt::white);
        p.drawText(tr, Qt::AlignCenter, angleHint_);
        p.restore();
    }
    p.setPen(QPen(QColor(255, 255, 255, 120), 1));
    for (int i = 1; i <= 2; ++i) {
        const double x = c.left() + c.width() * i / 3, y = c.top() + c.height() * i / 3;
        p.drawLine(QPointF(x, c.top()), QPointF(x, c.bottom()));
        p.drawLine(QPointF(c.left(), y), QPointF(c.right(), y));
    }
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(c);
    p.setBrush(Qt::white);
    for (auto& [k, pt] : handles(c)) p.drawRect(QRectF(pt.x() - 4, pt.y() - 4, 8, 8));
    p.setPen(theme::kAccent);
    p.drawText(QPointF(c.left() + 4, c.top() - 6), QString("%1 × %2   Enter：決定  Esc：取消")
                                                       .arg(qRound(crop_.w * full_.width()))
                                                       .arg(qRound(crop_.h * full_.height())));
}

void Viewer::setCropMode(bool on, const CropRect& r) {
    cropMode_ = on;
    if (!on) angleHint_.clear();
    if (on) crop_ = r;
    update();
}

// ---------------------------------------------------------------- input

void Viewer::wheelEvent(QWheelEvent* e) {
    if (img_.isNull()) return;
    setZoom(zoom_ * (e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15), e->position(), false);
}

void Viewer::mouseDoubleClickEvent(QMouseEvent* e) {
    if (cropMode_) return;
    fit_ ? setZoom(1.0, e->position(), false) : zoomFit();
}

void Viewer::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        space_ = true;
        setCursor(Qt::OpenHandCursor);
    } else {
        QWidget::keyPressEvent(e);
    }
}

void Viewer::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        space_ = false;
        setCursor(picking_ ? Qt::CrossCursor : Qt::ArrowCursor);
    } else {
        QWidget::keyReleaseEvent(e);
    }
}

void Viewer::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const int pi = paneAt(pos);
    const auto ps = panes();
    const QRectF r = imageRect(ps[pi], pi);
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_)) {
        panPane_ = pi;
        panStart_ = pos;
        offStart_ = offsets_[linked_ ? 0 : pi];
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || img_.isNull()) return;
    if (toolMode_ && !picking_ && !cropMode_ && !sideBySide()) {
        toolDown_ = true;
        emit toolPress(toolN(pos), e->modifiers());
        return;
    }
    if (picking_ && r.contains(pos)) {
        emit picked(int((pos.x() - r.left()) / r.width() * img_.width()), int((pos.y() - r.top()) / r.height() * img_.height()));
        return;
    }
    if (cropMode_) {
        const QRectF c = cropPx(r);
        for (auto& [k, pt] : handles(c))
            if ((pt - pos).manhattanLength() < 12) {
                cropHandle_ = k;
                cropP0_ = pos;
                cropStart_ = crop_;
                return;
            }
        if (c.contains(pos)) {
            cropHandle_ = "move";
            cropP0_ = pos;
            cropStart_ = crop_;
        }
        return;
    }
    if ((compare_ == CompareMode::WipeV || compare_ == CompareMode::WipeH) && !before_.isNull() && !sideBySide()) {
        const bool near = compare_ == CompareMode::WipeV ? std::abs(pos.x() - (r.left() + r.width() * split_)) < 10
                                                         : std::abs(pos.y() - (r.top() + r.height() * split_)) < 10;
        if (near) {
            splitDrag_ = true;
            return;
        }
    }
    if (!fit_) {
        panPane_ = pi;
        panStart_ = pos;
        offStart_ = offsets_[linked_ ? 0 : pi];
        setCursor(Qt::ClosedHandCursor);
    }
}

void Viewer::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const int pi = paneAt(pos);
    const auto ps = panes();
    const QRectF r = imageRect(ps[pi], pi);
    if (panPane_ >= 0) {
        QPointF& o = offsets_[linked_ ? 0 : panPane_];
        o = offStart_ + (pos - panStart_);
        clamp();
        update();
        emit viewChanged();
        return;
    }
    if (toolMode_ && !cropMode_ && !sideBySide()) {
        emit toolMove(toolN(pos), toolDown_, e->modifiers());
        update();
        if (toolDown_) return;
    }
    if (splitDrag_) {
        split_ = compare_ == CompareMode::WipeV ? std::clamp((pos.x() - r.left()) / std::max(1.0, r.width()), 0.0, 1.0)
                                                : std::clamp((pos.y() - r.top()) / std::max(1.0, r.height()), 0.0, 1.0);
        update();
        return;
    }
    if (!cropHandle_.isEmpty()) {
        dragCrop(pos, r);
        return;
    }
    if (cropMode_) {
        const QRectF c = cropPx(r);
        Qt::CursorShape cur = c.contains(pos) ? Qt::SizeAllCursor : Qt::ArrowCursor;
        for (auto& [k, pt] : handles(c))
            if ((pt - pos).manhattanLength() < 12) {
                cur = (k == "tl" || k == "br") ? Qt::SizeFDiagCursor
                      : (k == "tr" || k == "bl") ? Qt::SizeBDiagCursor
                      : (k == "t" || k == "b") ? Qt::SizeVerCursor
                                               : Qt::SizeHorCursor;
            }
        setCursor(cur);
        return;
    }
    // 画素の値（補正後の画像から）
    if (!img_.isNull() && r.contains(pos)) {
        const int x = int((pos.x() - r.left()) / r.width() * img_.width());
        const int y = int((pos.y() - r.top()) / r.height() * img_.height());
        if (x >= 0 && y >= 0 && x < img_.width() && y < img_.height()) {
            const QRgb c = img_.pixel(x, y);
            emit probe(std::array<int, 3>{qRed(c), qGreen(c), qBlue(c)});
            return;
        }
    }
    emit probe(std::nullopt);
}

QRectF Viewer::imageRectNow() const { return imageRect(panes()[0], 0); }

QPointF Viewer::toolN(const QPointF& pos) const {
    const QRectF r = imageRect(panes()[0], 0);
    return QPointF((pos.x() - r.left()) / std::max(1.0, r.width()), (pos.y() - r.top()) / std::max(1.0, r.height()));
}

void Viewer::setToolMode(bool on) {
    toolMode_ = on;
    toolDown_ = false;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void Viewer::mouseReleaseEvent(QMouseEvent* e) {
    if (toolDown_) {
        toolDown_ = false;
        emit toolRelease(toolN(e->position()));
        return;
    }
    if (panPane_ >= 0) {
        panPane_ = -1;
        setCursor(space_ ? Qt::OpenHandCursor : (picking_ ? Qt::CrossCursor : Qt::ArrowCursor));
    }
    splitDrag_ = false;
    if (!cropHandle_.isEmpty()) {
        cropHandle_.clear();
        emit cropChanged(crop_, false);
    }
}

void Viewer::leaveEvent(QEvent*) { emit probe(std::nullopt); }

void Viewer::dragCrop(const QPointF& pos, const QRectF& r) {
    const double dx = (pos.x() - cropP0_.x()) / r.width(), dy = (pos.y() - cropP0_.y()) / r.height();
    const CropRect c0 = cropStart_;
    double x0 = c0.x, y0 = c0.y, x1 = c0.x + c0.w, y1 = c0.y + c0.h;
    const QString& k = cropHandle_;
    if (k == "move") {
        crop_.x = std::clamp(x0 + dx, 0.0, 1 - c0.w);
        crop_.y = std::clamp(y0 + dy, 0.0, 1 - c0.h);
        update();
        return;
    }
    if (k.contains('l')) x0 = std::clamp(x0 + dx, 0.0, x1 - 0.02);
    if (k.contains('r')) x1 = std::clamp(x1 + dx, x0 + 0.02, 1.0);
    if (k.contains('t')) y0 = std::clamp(y0 + dy, 0.0, y1 - 0.02);
    if (k.contains('b')) y1 = std::clamp(y1 + dy, y0 + 0.02, 1.0);
    if (aspect_ > 0) {
        const double a = aspect_ * full_.height() / full_.width();  // 正規化座標での 幅/高さ
        double w = x1 - x0, h = y1 - y0;
        if (k == "t" || k == "b")
            w = h * a;
        else if (k == "l" || k == "r")
            h = w / a;
        else if (w / std::max(h, 1e-6) > a)
            w = h * a;
        else
            h = w / a;
        if (k.contains('l')) x0 = x1 - w; else x1 = x0 + w;
        if (k.contains('t')) y0 = y1 - h; else y1 = y0 + h;
        if (x0 < 0 || y0 < 0 || x1 > 1 || y1 > 1) return;
    }
    crop_ = {x0, y0, x1 - x0, y1 - y0};
    update();
}

}  // namespace rd
