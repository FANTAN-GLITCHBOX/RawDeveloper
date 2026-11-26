// 部分補正（段階・円形・ブラシ・肌・空）とスポット修復の画面操作
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QPainterPath>
#include <QShortcut>
#include <QStatusBar>
#include <cmath>

#include "engine/Local.h"
#include "ui/DevelopPanel.h"
#include "ui/MainWindow.h"
#include "ui/Viewer.h"

namespace rd {

namespace {
constexpr double kPi = 3.14159265358979;
constexpr double kHit = 11;  // 印をつかめる距離（画面の画素）
}  // namespace

void MainWindow::setupLocal() {
    connect(panel_, &DevelopPanel::localToolChanged, this, [this](int t) {
        const bool on = t != DevelopPanel::ToolNone;
        if (on) {
            panel_->cropButton()->setChecked(false);
            if (viewer_->compare() != CompareMode::Off && viewer_->compare() != CompareMode::WipeV &&
                viewer_->compare() != CompareMode::WipeH)
                viewer_->setCompare(CompareMode::Off);
            statusBar()->showMessage(t == DevelopPanel::ToolSpot ? QStringLiteral("消したい所をクリック（Delete で取り消し、Esc で終了）")
                                     : t == DevelopPanel::ToolBrush ? QStringLiteral("補正したい所をなぞってください（Alt で消す）")
                                                                     : QStringLiteral("画像の上をドラッグして作成、丸い印をドラッグで調整"),
                                     6000);
        }
        viewer_->setToolMode(on);
        selSpot_ = -1;
    });
    connect(panel_, &DevelopPanel::maskSelectionChanged, this, [this](int) {
        if (!s_) return;
        viewer_->update();
        if (panel_->maskOverlay()) applyLocal(s_->params, false, {});
    });
    connect(panel_, &DevelopPanel::maskOverlayToggled, this, [this](bool) {
        if (!s_) return;
        applyLocal(s_->params, false, {});
        dragging_ = false;
    });
    connect(viewer_, &Viewer::toolPress, this, &MainWindow::localPress);
    connect(viewer_, &Viewer::toolMove, this, &MainWindow::localMove);
    connect(viewer_, &Viewer::toolRelease, this, &MainWindow::localRelease);
    viewer_->setOverlay([this](QPainter& p, const QRectF& r) { drawLocalOverlay(p, r); });
    auto* del = new QShortcut(QKeySequence::Delete, viewer_);
    connect(del, &QShortcut::activated, this, &MainWindow::deleteLocalSelection);
    auto* bs = new QShortcut(QKeySequence(Qt::Key_Backspace), viewer_);
    connect(bs, &QShortcut::activated, this, &MainWindow::deleteLocalSelection);
}

QSizeF MainWindow::fullDims() const {
    if (!s_ || !s_->src) return {1, 1};
    const auto& m = s_->src->meta;
    if (m.fullWidth > 0 && m.fullHeight > 0) return QSizeF(m.fullWidth, m.fullHeight);
    return QSizeF(s_->src->linear.width, s_->src->linear.height);
}

// 表示画像の 0..1 → 回転・切り抜き前の画像の 0..1
QPointF MainWindow::dispToImg(QPointF n) const {
    if (!s_) return n;
    const Params& p = s_->params;
    if (!p.enabled.crop) return n;
    const QSizeF F = fullDims();
    const double W = F.width(), H = F.height();
    QPointF r = viewer_->cropMode() ? n : QPointF(p.cropX + n.x() * p.cropW, p.cropY + n.y() * p.cropH);
    if (std::abs(p.cropAngle) < 1e-3f) return r;
    const double a = p.cropAngle * kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
    const double cx = (W - 1) / 2.0, cy = (H - 1) / 2.0;
    const double dx = r.x() * W - cx, dy = r.y() * H - cy;
    return QPointF((ca * dx + sa * dy + cx) / W, (-sa * dx + ca * dy + cy) / H);
}

QPointF MainWindow::imgToDisp(QPointF i) const {
    if (!s_) return i;
    const Params& p = s_->params;
    if (!p.enabled.crop) return i;
    const QSizeF F = fullDims();
    const double W = F.width(), H = F.height();
    QPointF r = i;
    if (std::abs(p.cropAngle) >= 1e-3f) {
        const double a = p.cropAngle * kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
        const double cx = (W - 1) / 2.0, cy = (H - 1) / 2.0;
        const double sx = i.x() * W - cx, sy = i.y() * H - cy;
        r = QPointF((ca * sx - sa * sy + cx) / W, (sa * sx + ca * sy + cy) / H);
    }
    if (viewer_->cropMode()) return r;
    return QPointF((r.x() - p.cropX) / std::max(1e-6f, p.cropW), (r.y() - p.cropY) / std::max(1e-6f, p.cropH));
}

void MainWindow::applyLocal(const Params& p, bool final, const QString& desc) {
    // 範囲表示：チェックが入っているか、作っている最中は選んでいるマスクを赤く
    Params q = p;
    applyParams(q, final, desc, final);
}

namespace {
// 画像座標の円（半径は幅基準）を、表示の多角形にする
QPolygonF imgCircle(const std::function<QPointF(QPointF)>& toDisp, const QRectF& r, QPointF c, double rw, double rh, double angleDeg = 0) {
    QPolygonF poly;
    const double a = angleDeg * kPi / 180.0;
    for (int k = 0; k <= 64; ++k) {
        const double t = k * 2 * kPi / 64;
        const double u = std::cos(t) * rw, v = std::sin(t) * rh;
        const QPointF q(c.x() + std::cos(a) * u - std::sin(a) * v, c.y() + std::sin(a) * u + std::cos(a) * v);
        const QPointF d = toDisp(q);
        poly << QPointF(r.left() + d.x() * r.width(), r.top() + d.y() * r.height());
    }
    return poly;
}
}  // namespace

void MainWindow::drawLocalOverlay(QPainter& p, const QRectF& r) {
    if (!s_ || !developMode_) return;
    const int tool = panel_->localTool();
    const Params& P = s_->params;
    const QSizeF F = fullDims();
    const double asp = F.width() / F.height();  // 幅基準の長さを高さの 0..1 に
    auto toW = [&](QPointF i) {
        const QPointF d = imgToDisp(i);
        return QPointF(r.left() + d.x() * r.width(), r.top() + d.y() * r.height());
    };
    auto toDispFn = [this](QPointF i) { return imgToDisp(i); };
    const QColor acc(0xd2, 0x8b, 0x2c);
    const int sel = panel_->selectedMask();
    // 印の表示：0 常に 1 マウスが写真の上にあるとき 2 選んだものだけ 3 表示しない（H で一時的に隠す）
    const int mode = overlayHidden_ ? 3 : overlayMode_;
    const bool hoverOk = mode != 1 || viewer_->underMouse();
    auto visible = [&](bool selected) { return mode != 3 && hoverOk && (mode != 2 || selected); };
    auto pin = [&](QPointF w, bool on) {
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(on ? acc : QColor(240, 240, 240));
        p.drawEllipse(w, 6, 6);
    };
    // マスクの印と形
    if (tool == DevelopPanel::ToolLinear || tool == DevelopPanel::ToolRadial || (sel >= 0 && tool == DevelopPanel::ToolNone && false)) {
        for (int i = 0; i < int(P.masks.size()); ++i) {
            const LocalMask& m = P.masks[i];
            if (m.type != "linear" && m.type != "radial") continue;
            const bool on = i == sel;
            if (!visible(on)) continue;
            if (m.type == "linear") {
                const QPointF a(m.x0, m.y0), b(m.x1, m.y1);
                if (on) {
                    // 始め（100%）と終わり（0%）の線。向きに直交する長い線
                    const double vx = (b.x() - a.x()), vy = (b.y() - a.y()) / asp;
                    const double L = std::hypot(vx, vy);
                    const double px = L > 1e-9 ? -vy / L : 1, py = L > 1e-9 ? vx / L : 0;
                    for (const QPointF& c : {a, b}) {
                        const QPointF e1(c.x() + px * 3, c.y() + py * 3 * asp), e2(c.x() - px * 3, c.y() - py * 3 * asp);
                        p.setPen(QPen(QColor(255, 255, 255, 220), 1.5, &c == &a ? Qt::SolidLine : Qt::DashLine));
                        p.drawLine(toW(e1), toW(e2));
                    }
                    pin(toW(a), true);
                    pin(toW(b), false);
                }
                pin(toW(QPointF((a.x() + b.x()) / 2, (a.y() + b.y()) / 2)), on);
            } else {
                const QPointF c(m.x0, m.y0);
                if (on) {
                    p.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
                    p.setBrush(Qt::NoBrush);
                    p.drawPolyline(imgCircle(toDispFn, r, c, m.x1, m.y1, m.angle));
                    const double f0 = 1.0 - std::clamp(m.feather, 1.0f, 100.0f) / 100.0 * 0.95;
                    p.setPen(QPen(QColor(255, 255, 255, 140), 1, Qt::DashLine));
                    p.drawPolyline(imgCircle(toDispFn, r, c, m.x1 * f0, m.y1 * f0, m.angle));
                    pin(toW(QPointF(c.x() + m.x1, c.y())), false);
                    pin(toW(QPointF(c.x(), c.y() + m.y1)), false);
                }
                pin(toW(c), on);
            }
        }
    }
    // ブラシの大きさ
    if (tool == DevelopPanel::ToolBrush && hoverN_.x() >= 0) {
        const QPointF c = dispToImg(hoverN_);
        const double R = panel_->brushSize();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 2.5));
        p.drawPolyline(imgCircle(toDispFn, r, c, R, R * asp));
        p.setPen(QPen(QColor(255, 255, 255, 230), 1.2));
        p.drawPolyline(imgCircle(toDispFn, r, c, R, R * asp));
    }
    // スポット
    if (tool == DevelopPanel::ToolSpot) {
        for (int i = 0; i < int(P.spots.size()); ++i) {
            const Spot& s = P.spots[i];
            const bool on = i == selSpot_;
            if (!visible(on)) continue;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(on ? acc : QColor(255, 255, 255, 200), on ? 2.0 : 1.3));
            p.drawPolyline(imgCircle(toDispFn, r, QPointF(s.x, s.y), s.r, s.r * asp));
            if (on) {
                p.setPen(QPen(acc, 1.5, Qt::DashLine));
                p.drawPolyline(imgCircle(toDispFn, r, QPointF(s.sx, s.sy), s.r, s.r * asp));
                p.drawLine(toW(QPointF(s.x, s.y)), toW(QPointF(s.sx, s.sy)));
            }
        }
        if (hoverN_.x() >= 0 && ldrag_ == LDrag::None) {
            const QPointF c = dispToImg(hoverN_);
            p.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DotLine));
            p.drawPolyline(imgCircle(toDispFn, r, c, panel_->spotSize(), panel_->spotSize() * asp));
        }
    }
}

void MainWindow::localPress(QPointF n, Qt::KeyboardModifiers mods) {
    if (!s_ || !s_->src) return;
    const int tool = panel_->localTool();
    const QPointF ip = dispToImg(n);
    const QRectF vr = viewer_->imageRectNow();
    auto wpos = [&](QPointF img) {
        const QPointF d = imgToDisp(img);
        return QPointF(vr.left() + d.x() * vr.width(), vr.top() + d.y() * vr.height());
    };
    const QPointF here(vr.left() + n.x() * vr.width(), vr.top() + n.y() * vr.height());
    auto near = [&](QPointF img) { return QLineF(wpos(img), here).length() < kHit; };
    Params& P = s_->params;
    ldragStart_ = ip;
    ldragMoved_ = false;
    const int sel = panel_->selectedMask();

    if (tool == DevelopPanel::ToolLinear || tool == DevelopPanel::ToolRadial) {
        // 選んでいるマスクの印
        if (sel >= 0 && sel < int(P.masks.size())) {
            const LocalMask& m = P.masks[sel];
            ldragMask_ = m;
            if (m.type == "linear") {
                if (near({m.x0, m.y0})) { ldrag_ = LDrag::LinA; return; }
                if (near({m.x1, m.y1})) { ldrag_ = LDrag::LinB; return; }
                if (near({(m.x0 + m.x1) / 2, (m.y0 + m.y1) / 2})) { ldrag_ = LDrag::LinMove; return; }
            } else if (m.type == "radial") {
                if (near({m.x0 + m.x1, m.y0})) { ldrag_ = LDrag::RadX; return; }
                if (near({m.x0, m.y0 + m.y1})) { ldrag_ = LDrag::RadY; return; }
                if (near({m.x0, m.y0})) { ldrag_ = LDrag::RadCenter; return; }
            }
        }
        // ほかのマスクの印をクリック → 選ぶ
        for (int i = 0; i < int(P.masks.size()); ++i) {
            const LocalMask& m = P.masks[i];
            const QPointF c = m.type == "linear" ? QPointF((m.x0 + m.x1) / 2, (m.y0 + m.y1) / 2) : QPointF(m.x0, m.y0);
            if ((m.type == "linear" || m.type == "radial") && near(c)) {
                panel_->selectMask(i);
                emit panel_->maskSelectionChanged(i);
                ldragMask_ = m;
                ldrag_ = m.type == "linear" ? LDrag::LinMove : LDrag::RadCenter;
                return;
            }
        }
        // 新しく作る
        LocalMask m;
        m.type = tool == DevelopPanel::ToolLinear ? "linear" : "radial";
        m.x0 = float(ip.x());
        m.y0 = float(ip.y());
        if (m.type == "linear") {
            m.x1 = m.x0;
            m.y1 = m.y0 + 0.001f;
        } else {
            m.x1 = 0.001f;
            m.y1 = 0.001f;
        }
        P.masks.push_back(m);
        P.enabled.local = true;
        const int idx = int(P.masks.size()) - 1;
        panel_->setParams(P, s_->asShotTT);
        panel_->selectMask(idx);
        ldragMask_ = m;
        ldrag_ = m.type == "linear" ? LDrag::NewLinear : LDrag::NewRadial;
        return;
    }
    if (tool == DevelopPanel::ToolBrush) {
        if (sel < 0 || sel >= int(P.masks.size()) || P.masks[sel].type != "brush") {
            LocalMask m;
            m.type = "brush";
            m.feather = 50;
            P.masks.push_back(m);
            P.enabled.local = true;
            panel_->setParams(P, s_->asShotTT);
            panel_->selectMask(int(P.masks.size()) - 1);
            emit panel_->maskSelectionChanged(int(P.masks.size()) - 1);
        }
        ldrag_ = LDrag::Brush;
        lastDab_ = QPointF(-10, -10);
        localMove(n, true, mods);
        return;
    }
    if (tool == DevelopPanel::ToolSpot) {
        // 既存のスポット：コピー元（選択中のみ）→ 修復先
        if (selSpot_ >= 0 && selSpot_ < int(P.spots.size())) {
            const Spot& s = P.spots[selSpot_];
            const QPointF sc(s.sx, s.sy);
            if (QLineF(wpos(sc), here).length() < std::max(kHit, s.r * vr.width() / std::max(1e-6f, P.enabled.crop ? P.cropW : 1.0f))) {
                ldragSpot_ = s;
                ldrag_ = LDrag::SpotSrc;
                return;
            }
        }
        for (int i = int(P.spots.size()) - 1; i >= 0; --i) {
            const Spot& s = P.spots[i];
            const double rpx = s.r * vr.width() / std::max(1e-6f, P.enabled.crop ? P.cropW : 1.0f);
            if (QLineF(wpos({s.x, s.y}), here).length() < std::max(kHit, rpx)) {
                selSpot_ = i;
                ldragSpot_ = s;
                ldrag_ = LDrag::SpotDst;
                viewer_->update();
                return;
            }
        }
        Spot s{float(ip.x()), float(ip.y()), float(ip.x()), float(ip.y()), float(panel_->spotSize())};
        s.clone = panel_->spotClone();
        if (s_->preview) autoSpotSource(*s_->preview, s);
        P.spots.push_back(s);
        P.spotsOn = true;
        P.enabled.local = true;
        selSpot_ = int(P.spots.size()) - 1;
        ldrag_ = LDrag::None;
        applyLocal(P, true, QStringLiteral("スポット修復"));
        return;
    }
}

void MainWindow::localMove(QPointF n, bool pressed, Qt::KeyboardModifiers mods) {
    hoverN_ = n;
    if (!s_ || !pressed || ldrag_ == LDrag::None) return;
    Params P = s_->params;
    const QPointF ip = dispToImg(n);
    const QPointF d = ip - ldragStart_;
    const QSizeF F = fullDims();
    const double asp = F.width() / F.height();
    ldragMoved_ = true;
    const int sel = panel_->selectedMask();
    auto cur = [&]() -> LocalMask* { return sel >= 0 && sel < int(P.masks.size()) ? &P.masks[sel] : nullptr; };
    LocalMask* m = cur();
    switch (ldrag_) {
    case LDrag::NewLinear:
    case LDrag::LinB:
        if (m) {
            m->x1 = float(ip.x());
            m->y1 = float(ip.y());
        }
        break;
    case LDrag::LinA:
        if (m) {
            m->x0 = float(ip.x());
            m->y0 = float(ip.y());
        }
        break;
    case LDrag::LinMove:
    case LDrag::RadCenter:
        if (m) {
            m->x0 = float(ldragMask_.x0 + d.x());
            m->y0 = float(ldragMask_.y0 + d.y());
            if (ldrag_ == LDrag::LinMove) {
                m->x1 = float(ldragMask_.x1 + d.x());
                m->y1 = float(ldragMask_.y1 + d.y());
            }
        }
        break;
    case LDrag::NewRadial:
        if (m) {
            m->x1 = float(std::max(0.005, std::abs(ip.x() - m->x0)));
            m->y1 = float(std::max(0.005, std::abs(ip.y() - m->y0)));
            if (mods & Qt::ShiftModifier) m->y1 = float(m->x1 * asp);  // 正円
        }
        break;
    case LDrag::RadX:
        if (m) m->x1 = float(std::max(0.005, std::abs(ip.x() - m->x0)));
        break;
    case LDrag::RadY:
        if (m) m->y1 = float(std::max(0.005, std::abs(ip.y() - m->y0)));
        break;
    case LDrag::Brush:
        if (m && m->type == "brush") {
            const double R = panel_->brushSize();
            const double dist = std::hypot(ip.x() - lastDab_.x(), (ip.y() - lastDab_.y()) / asp);
            if (dist < R * 0.25) return;
            m->dabs.push_back({float(ip.x()), float(ip.y()), float(R), float(panel_->brushFlow()),
                               panel_->brushErase() || (mods & Qt::AltModifier)});
            lastDab_ = ip;
        }
        break;
    case LDrag::SpotDst:
        if (selSpot_ >= 0 && selSpot_ < int(P.spots.size())) {
            P.spots[selSpot_].x = float(ldragSpot_.x + d.x());
            P.spots[selSpot_].y = float(ldragSpot_.y + d.y());
            P.spots[selSpot_].sx = float(ldragSpot_.sx + d.x());
            P.spots[selSpot_].sy = float(ldragSpot_.sy + d.y());
        }
        break;
    case LDrag::SpotSrc:
        if (selSpot_ >= 0 && selSpot_ < int(P.spots.size())) {
            P.spots[selSpot_].sx = float(ldragSpot_.sx + d.x());
            P.spots[selSpot_].sy = float(ldragSpot_.sy + d.y());
        }
        break;
    default:
        return;
    }
    applyLocal(P, false, {});
}

void MainWindow::localRelease(QPointF) {
    if (!s_) return;
    const LDrag d = ldrag_;
    ldrag_ = LDrag::None;
    Params P = s_->params;
    const int sel = panel_->selectedMask();
    QString desc;
    switch (d) {
    case LDrag::NewLinear:
        if (sel >= 0 && !ldragMoved_) {  // クリックだけ：下へ 20% の段階
            P.masks[sel].x1 = P.masks[sel].x0;
            P.masks[sel].y1 = P.masks[sel].y0 + 0.2f;
        }
        desc = QStringLiteral("段階フィルターを追加");
        break;
    case LDrag::NewRadial:
        if (sel >= 0 && !ldragMoved_) {
            const QSizeF F = fullDims();
            P.masks[sel].x1 = 0.12f;
            P.masks[sel].y1 = float(0.12 * F.width() / F.height());
        }
        desc = QStringLiteral("円形フィルターを追加");
        break;
    case LDrag::Brush: desc = QStringLiteral("ブラシ"); break;
    case LDrag::SpotDst:
    case LDrag::SpotSrc: desc = ldragMoved_ ? QStringLiteral("スポットを移動") : QString(); break;
    case LDrag::None: return;
    default: desc = ldragMoved_ ? QStringLiteral("マスクを調整") : QString(); break;
    }
    if (desc.isEmpty()) {
        viewer_->update();
        return;
    }
    applyLocal(P, true, desc);
    if (d == LDrag::NewLinear || d == LDrag::NewRadial || d == LDrag::Brush) emit panel_->maskSelectionChanged(panel_->selectedMask());
}

void MainWindow::deleteLocalSelection() {
    if (!s_ || !developMode_) return;
    Params P = s_->params;
    const int tool = panel_->localTool();
    if (tool == DevelopPanel::ToolSpot) {
        if (selSpot_ < 0 || selSpot_ >= int(P.spots.size())) {
            if (P.spots.empty()) return;
            selSpot_ = int(P.spots.size()) - 1;  // 選んでいなければ最後に置いたもの
        }
        P.spots.erase(P.spots.begin() + selSpot_);
        selSpot_ = -1;
        applyLocal(P, true, QStringLiteral("スポットを削除"));
        return;
    }
    const int sel = panel_->selectedMask();
    if (tool != DevelopPanel::ToolNone && sel >= 0 && sel < int(P.masks.size())) {
        P.masks.erase(P.masks.begin() + sel);
        applyLocal(P, true, QStringLiteral("マスクを削除"));
        panel_->selectMask(std::min(sel, int(P.masks.size()) - 1));
        emit panel_->maskSelectionChanged(panel_->selectedMask());
    }
}

}  // namespace rd
