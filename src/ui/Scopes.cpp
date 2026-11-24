#include "ui/Scopes.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "ui/Theme.h"

namespace rd {

namespace {

struct Zone {
    const char* key;
    const char* name;
    double a, b;
};
const Zone kZones[] = {{"blacks", "黒レベル", 0.00, 0.06},
                       {"shadows", "シャドウ", 0.06, 0.30},
                       {"exposure", "露出", 0.30, 0.70},
                       {"highlights", "ハイライト", 0.70, 0.94},
                       {"whites", "白レベル", 0.94, 1.00}};

const char* modeName(ScopeMode m) {
    switch (m) {
        case ScopeMode::HistRgb: return "ヒストグラム（RGB）";
        case ScopeMode::HistLuma: return "ヒストグラム（輝度）";
        case ScopeMode::HistSplit: return "ヒストグラム（RGB 別）";
        case ScopeMode::Waveform: return "波形";
        case ScopeMode::Parade: return "RGB パレード";
        case ScopeMode::Vector: return "ベクトルスコープ";
    }
    return "";
}

bool isHist(ScopeMode m) { return m == ScopeMode::HistRgb || m == ScopeMode::HistLuma || m == ScopeMode::HistSplit; }

// 件数の 2 次元表を、少ない所も見える発光風の画像に
QImage glow(const std::vector<float>& a, int w, int h, QColor col, bool flipY) {
    float mx = 1;
    for (float v : a) mx = std::max(mx, v);
    const float lm = std::log1p(mx);
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        auto* line = img.scanLine(flipY ? h - 1 - y : y);
        for (int x = 0; x < w; ++x) {
            const float v = std::clamp(std::log1p(a[static_cast<size_t>(y) * w + x]) / lm * 1.35f, 0.0f, 1.0f);
            line[x * 4] = static_cast<std::uint8_t>(v * col.red());
            line[x * 4 + 1] = static_cast<std::uint8_t>(v * col.green());
            line[x * 4 + 2] = static_cast<std::uint8_t>(v * col.blue());
            line[x * 4 + 3] = static_cast<std::uint8_t>(std::min(1.0f, v * 1.4f) * 255);
        }
    }
    return img;
}

float pct995(const std::vector<float>& h) {
    std::vector<float> v(h.begin() + 2, h.end() - 2);
    std::sort(v.begin(), v.end());
    return std::max(1.0f, v[static_cast<size_t>(v.size() * 0.995)] * 1.1f);
}

}  // namespace

std::shared_ptr<ScopeData> ScopeData::make(const std::uint8_t* rgb8, int w, int h) {
    auto d = std::make_shared<ScopeData>();
    const int step = std::max(1, std::max(w, h) / 480);
    d->w = (w + step - 1) / step;
    d->h = (h + step - 1) / step;
    d->rgb.resize(static_cast<size_t>(d->w) * d->h * 3);
    d->luma.resize(static_cast<size_t>(d->w) * d->h);
    for (auto& hh : d->hist) hh.assign(256, 0);
    d->histL.assign(256, 0);
    size_t hi = 0, lo = 0;
    for (int y = 0; y < d->h; ++y)
        for (int x = 0; x < d->w; ++x) {
            const std::uint8_t* s = rgb8 + (static_cast<size_t>(y * step) * w + x * step) * 3;
            const size_t i = static_cast<size_t>(y) * d->w + x;
            for (int c = 0; c < 3; ++c) {
                d->rgb[i * 3 + c] = s[c];
                d->hist[c][s[c]] += 1;
            }
            const int l = (s[0] * 54 + s[1] * 183 + s[2] * 19) >> 8;
            d->luma[i] = l;
            d->histL[l] += 1;
            if (std::max({s[0], s[1], s[2]}) >= 254) ++hi;
            if (std::min({s[0], s[1], s[2]}) <= 1) ++lo;
        }
    const double n = static_cast<double>(d->w) * d->h;
    d->clipHi = hi / n;
    d->clipLo = lo / n;
    // 波形・パレード・ベクトル
    d->wave.assign(256 * 256, 0);
    for (auto& p : d->parade) p.assign(256 * 160, 0);
    d->vec.assign(200 * 200, 0);
    for (int y = 0; y < d->h; ++y)
        for (int x = 0; x < d->w; ++x) {
            const size_t i = static_cast<size_t>(y) * d->w + x;
            d->wave[static_cast<size_t>(d->luma[i]) * 256 + x * 256 / d->w] += 1;
            for (int c = 0; c < 3; ++c) d->parade[c][static_cast<size_t>(d->rgb[i * 3 + c]) * 160 + x * 160 / d->w] += 1;
            const float r = d->rgb[i * 3], g = d->rgb[i * 3 + 1], b = d->rgb[i * 3 + 2];
            const float cb = -0.1687f * r - 0.3313f * g + 0.5f * b;
            const float cr = 0.5f * r - 0.4187f * g - 0.0813f * b;
            const int xi = std::clamp(static_cast<int>((cb / 128 * 0.5f + 0.5f) * 199), 0, 199);
            const int yi = std::clamp(static_cast<int>((0.5f - cr / 128 * 0.5f) * 199), 0, 199);
            d->vec[static_cast<size_t>(yi) * 200 + xi] += 1;
        }
    return d;
}

ScopeView::ScopeView(ScopeMode mode, QWidget* parent) : QWidget(parent), mode_(mode) {
    setMouseTracking(true);
    setMinimumSize(160, 110);
}

void ScopeView::setMode(ScopeMode m) {
    mode_ = m;
    emit modeChanged();
    update();
}

QRectF ScopeView::plotRect() const { return QRectF(6, 14, width() - 12, height() - 34); }

void ScopeView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#141414"));
    if (isHist(mode_))
        paintHist(p);
    else if (mode_ == ScopeMode::Waveform)
        paintWave(p);
    else if (mode_ == ScopeMode::Parade)
        paintParade(p);
    else
        paintVector(p);
    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    p.setPen(QColor("#666"));
    p.drawText(QRectF(4, 2, width() - 8, 12), Qt::AlignRight, modeName(mode_));
}

void ScopeView::paintHist(QPainter& p) {
    const QRectF r = plotRect();
    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    int zi = 0;
    for (const auto& z : kZones) {
        const double x0 = r.left() + z.a * r.width(), x1 = r.left() + z.b * r.width();
        const bool hi = zoneHi_ == z.key || dragKey_ == z.key;
        QColor c = hi ? theme::kAccent : QColor(Qt::white);
        c.setAlpha(hi ? 38 : (zi % 2 == 0 ? 10 : 0));
        p.fillRect(QRectF(x0, r.top(), x1 - x0, r.height()), c);
        p.setPen(hi ? theme::kAccent : QColor("#6a6a6a"));
        const QString name = QString::fromUtf8(z.name);
        const auto fm = p.fontMetrics();
        if (fm.horizontalAdvance(name) + 2 <= x1 - x0)
            p.drawText(QRectF(x0, r.bottom() + 2, x1 - x0, 12), Qt::AlignCenter, name);
        else if (fm.horizontalAdvance(name.left(1)) + 2 <= x1 - x0)
            p.drawText(QRectF(x0, r.bottom() + 2, x1 - x0, 12), Qt::AlignCenter, name.left(1));
        ++zi;
    }
    if (!data_) return;
    auto norm = [&](const std::vector<float>& h) {
        if (!log_) return h;
        std::vector<float> o(h.size());
        for (size_t i = 0; i < h.size(); ++i) o[i] = std::log1p(h[i]);
        return o;
    };
    auto path = [&](const std::vector<float>& h, const QRectF& rr, float scale) {
        QPainterPath pp(QPointF(rr.left(), rr.bottom()));
        for (int i = 0; i < 256; ++i)
            pp.lineTo(rr.left() + i / 255.0 * rr.width(), rr.bottom() - std::min(1.0f, h[i] / scale) * rr.height());
        pp.lineTo(rr.right(), rr.bottom());
        pp.closeSubpath();
        return pp;
    };
    if (ghost_) {
        const auto g = norm(ghost_->histL);
        p.setPen(QPen(QColor(150, 150, 150, 120), 1));
        p.setBrush(QColor(120, 120, 120, 40));
        p.drawPath(path(g, r, pct995(g)));
    }
    if (mode_ == ScopeMode::HistLuma) {
        const auto h = norm(data_->histL);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 220, 220, 170));
        p.drawPath(path(h, r, pct995(h)));
    } else {
        const QColor cols[3] = {QColor(255, 70, 70, 150), QColor(70, 220, 70, 150), QColor(80, 130, 255, 150)};
        std::array<std::vector<float>, 3> hs;
        float scale = 1;
        for (int c = 0; c < 3; ++c) {
            hs[c] = norm(data_->hist[c]);
            scale = std::max(scale, pct995(hs[c]));
        }
        if (mode_ == ScopeMode::HistSplit) {
            const double hh = r.height() / 3;
            for (int c = 0; c < 3; ++c) {
                QColor cc = cols[c];
                cc.setAlpha(200);
                p.setPen(Qt::NoPen);
                p.setBrush(cc);
                p.drawPath(path(hs[c], QRectF(r.left(), r.top() + c * hh, r.width(), hh - 2), pct995(hs[c])));
            }
        } else {
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            for (int c = 0; c < 3; ++c) {
                p.setPen(Qt::NoPen);
                p.setBrush(cols[c]);
                p.drawPath(path(hs[c], r, scale));
            }
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            for (int c = 0; c < 3; ++c) {
                QColor cc = cols[c];
                cc.setAlpha(255);
                p.setPen(QPen(cc, 1));
                p.setBrush(Qt::NoBrush);
                QPainterPath line(QPointF(r.left(), r.bottom() - std::min(1.0f, hs[c][0] / scale) * r.height()));
                for (int i = 1; i < 256; ++i)
                    line.lineTo(r.left() + i / 255.0 * r.width(), r.bottom() - std::min(1.0f, hs[c][i] / scale) * r.height());
                p.drawPath(line);
            }
        }
    }
    p.setPen(data_->clipLo > 0.001 ? QColor("#5a8cff") : QColor("#777"));
    p.drawText(QRectF(r.left() + 2, r.top(), 100, 12), Qt::AlignLeft,
               QString("黒つぶれ %1%").arg(data_->clipLo * 100, 0, 'f', 1));
    p.setPen(data_->clipHi > 0.001 ? QColor("#ff6a6a") : QColor("#777"));
    p.drawText(QRectF(r.right() - 100, r.top() + 12, 98, 12), Qt::AlignRight,
               QString("白飛び %1%").arg(data_->clipHi * 100, 0, 'f', 1));
    if (probe_) {
        const char* pc[3] = {"#ff6060", "#60e060", "#6090ff"};
        for (int c = 0; c < 3; ++c) {
            const double x = r.left() + (*probe_)[c] / 255.0 * r.width();
            p.setPen(QPen(QColor(pc[c]), 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(theme::kText);
        p.drawText(QRectF(r.left(), r.top() + 12, 160, 12), Qt::AlignLeft,
                   QString("R%1 G%2 B%3").arg((*probe_)[0]).arg((*probe_)[1]).arg((*probe_)[2]));
    }
    if (hoverX_ >= 0) {
        p.setPen(QPen(theme::kAccent, 1, Qt::DashLine));
        p.drawLine(QPointF(hoverX_, r.top()), QPointF(hoverX_, r.bottom()));
    }
}

void ScopeView::paintGrid(QPainter& p, const QRectF& r) {
    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    const std::pair<double, const char*> lines[] = {{0, "0"}, {std::pow(0.18, 1 / 2.2), "18%"}, {0.5, "50"}, {1, "100"}};
    for (auto [v, lab] : lines) {
        const double y = r.bottom() - v * r.height();
        const bool mid = QString(lab) == "18%";
        p.setPen(QPen(mid ? QColor("#4a3a20") : QColor("#333"), 1, mid ? Qt::DashLine : Qt::SolidLine));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QColor("#666"));
        p.drawText(QRectF(r.left() + 1, y - 12, 30, 12), Qt::AlignLeft, lab);
    }
}

void ScopeView::paintWave(QPainter& p) {
    const QRectF r(6, 14, width() - 12, height() - 20);
    paintGrid(p, r);
    if (!data_) return;
    p.drawImage(r, glow(data_->wave, 256, 256, QColor(150, 255, 170), true));
    if (probe_) {
        const double l = 0.2126 * (*probe_)[0] + 0.7152 * (*probe_)[1] + 0.0722 * (*probe_)[2];
        const double y = r.bottom() - l / 255 * r.height();
        p.setPen(QPen(theme::kAccent, 1, Qt::DashLine));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
}

void ScopeView::paintParade(QPainter& p) {
    const QRectF r(6, 14, width() - 12, height() - 20);
    paintGrid(p, r);
    if (!data_) return;
    const QColor cols[3] = {QColor(255, 90, 90), QColor(90, 240, 90), QColor(100, 140, 255)};
    const double w3 = r.width() / 3;
    for (int c = 0; c < 3; ++c)
        p.drawImage(QRectF(r.left() + c * w3 + 2, r.top(), w3 - 4, r.height()), glow(data_->parade[c], 160, 256, cols[c], true));
}

void ScopeView::paintVector(QPainter& p) {
    const double s = std::min(width() - 12, height() - 20);
    const QRectF r((width() - s) / 2, 14 + (height() - 20 - s) / 2, s, s);
    const QPointF c = r.center();
    p.setPen(QPen(QColor("#333"), 1));
    p.drawEllipse(c, s / 2, s / 2);
    p.drawEllipse(c, s / 4, s / 4);
    p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
    p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
    const double ang = 123 * 3.14159265 / 180;
    p.setPen(QPen(theme::kAccent, 1, Qt::DashLine));
    const QPointF skin(c.x() + std::cos(ang) * s / 2, c.y() - std::sin(ang) * s / 2);
    p.drawLine(c, skin);
    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    p.drawText(skin + QPointF(-10, -2), QStringLiteral("肌色"));
    const std::pair<const char*, std::array<int, 3>> targets[] = {
        {"R", {255, 0, 0}}, {"Y", {255, 255, 0}}, {"G", {0, 255, 0}}, {"C", {0, 255, 255}}, {"B", {0, 0, 255}}, {"M", {255, 0, 255}}};
    for (auto& [name, rgb] : targets) {
        const double cb = -0.1687 * rgb[0] - 0.3313 * rgb[1] + 0.5 * rgb[2];
        const double cr = 0.5 * rgb[0] - 0.4187 * rgb[1] - 0.0813 * rgb[2];
        const double x = c.x() + cb / 128 * 0.5 * s, y = c.y() - cr / 128 * 0.5 * s;
        p.setPen(QColor("#666"));
        p.drawRect(QRectF(x - 4, y - 4, 8, 8));
        p.drawText(QPointF(x + 6, y + 3), name);
    }
    if (data_) p.drawImage(r, glow(data_->vec, 200, 200, QColor(190, 255, 200), false));
}

void ScopeView::mouseMoveEvent(QMouseEvent* e) {
    if (!isHist(mode_)) return;
    const QRectF r = plotRect();
    const double x = e->position().x();
    if (!dragKey_.isEmpty()) {
        emit zoneDrag(dragKey_, (x - dragX_) / std::max(1.0, r.width()), false);
        dragX_ = x;
        return;
    }
    hoverX_ = x;
    const double v = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    emit hoverRange(std::max(0, int(v * 255) - 6), std::min(255, int(v * 255) + 6));
    update();
}

void ScopeView::leaveEvent(QEvent*) {
    hoverX_ = -1;
    emit hoverRange(-1, -1);
    update();
}

void ScopeView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        QMenu m(this);
        for (ScopeMode md : {ScopeMode::HistRgb, ScopeMode::HistLuma, ScopeMode::HistSplit, ScopeMode::Waveform,
                             ScopeMode::Parade, ScopeMode::Vector}) {
            QAction* a = m.addAction(modeName(md));
            a->setCheckable(true);
            a->setChecked(md == mode_);
            connect(a, &QAction::triggered, this, [this, md] { setMode(md); });
        }
        m.addSeparator();
        QAction* la = m.addAction(QStringLiteral("縦軸を対数にする"));
        la->setCheckable(true);
        la->setChecked(log_);
        connect(la, &QAction::triggered, this, [this] {
            log_ = !log_;
            update();
        });
        m.exec(e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::LeftButton && isHist(mode_)) {
        const QRectF r = plotRect();
        const double v = std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0);
        for (const auto& z : kZones)
            if (v >= z.a && v <= z.b) {
                dragKey_ = z.key;
                dragX_ = e->position().x();
                setCursor(Qt::SizeHorCursor);
                update();
                break;
            }
    }
}

void ScopeView::mouseReleaseEvent(QMouseEvent*) {
    if (dragKey_.isEmpty()) return;
    emit zoneDrag(dragKey_, 0, true);
    dragKey_.clear();
    unsetCursor();
    update();
}

// ================================================================ panel

ScopesPanel::ScopesPanel(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);
    auto* bar = new QHBoxLayout;
    firstCombo_ = new QComboBox;
    for (ScopeMode m : {ScopeMode::HistRgb, ScopeMode::HistLuma, ScopeMode::HistSplit, ScopeMode::Waveform,
                        ScopeMode::Parade, ScopeMode::Vector})
        firstCombo_->addItem(modeName(m), static_cast<int>(m));
    layoutCombo_ = new QComboBox;
    layoutCombo_->addItems({QStringLiteral("1 画面"), QStringLiteral("2 画面"), QStringLiteral("4 画面")});
    layoutCombo_->setToolTip(QStringLiteral("スコープの分割数（各スコープは右クリックで種類を変更）"));
    bar->addWidget(firstCombo_);
    bar->addWidget(layoutCombo_);
    bar->addStretch(1);
    v->addLayout(bar);
    grid_ = new QGridLayout;
    grid_->setSpacing(3);
    v->addLayout(grid_, 1);
    QSettings st;
    const QList<int> def{0, 3, 4, 5};
    QList<QVariant> saved = st.value("scope/modes").toList();
    for (int i = 0; i < 4; ++i) {
        const int m = i < saved.size() ? saved[i].toInt() : def[i];
        auto* vw = new ScopeView(static_cast<ScopeMode>(m));
        connect(vw, &ScopeView::hoverRange, this, &ScopesPanel::hoverRange);
        connect(vw, &ScopeView::zoneDrag, this, &ScopesPanel::zoneDrag);
        connect(vw, &ScopeView::modeChanged, this, &ScopesPanel::save);
        views_.push_back(vw);
    }
    firstCombo_->setCurrentIndex(firstCombo_->findData(static_cast<int>(views_[0]->mode())));
    connect(firstCombo_, &QComboBox::currentIndexChanged, this,
            [this](int i) { views_[0]->setMode(static_cast<ScopeMode>(firstCombo_->itemData(i).toInt())); });
    layoutCombo_->setCurrentIndex(st.value("scope/layout", 0).toInt());
    connect(layoutCombo_, &QComboBox::currentIndexChanged, this, [this] { relayout(); });
    relayout();
}

void ScopesPanel::save() {
    QList<QVariant> l;
    for (auto* v : views_) l.append(static_cast<int>(v->mode()));
    QSettings().setValue("scope/modes", l);
    const int idx = firstCombo_->findData(static_cast<int>(views_[0]->mode()));
    if (idx != firstCombo_->currentIndex()) {
        QSignalBlocker b(firstCombo_);
        firstCombo_->setCurrentIndex(idx);
    }
}

void ScopesPanel::relayout() {
    const int n = std::array{1, 2, 4}[layoutCombo_->currentIndex()];
    QSettings().setValue("scope/layout", layoutCombo_->currentIndex());
    for (auto* v : views_) {
        grid_->removeWidget(v);
        v->hide();
    }
    const int pos[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    for (int i = 0; i < n; ++i) {
        grid_->addWidget(views_[i], pos[i][0], pos[i][1]);
        views_[i]->show();
    }
}

std::shared_ptr<ScopeData> ScopesPanel::setImage(const std::uint8_t* rgb8, int w, int h) {
    auto d = ScopeData::make(rgb8, w, h);
    for (auto* v : views_) v->setData(d);
    return d;
}

void ScopesPanel::beginGhost() {
    if (ghosting_) return;
    ghosting_ = true;
    for (auto* v : views_) v->setGhost(v->data());
}

void ScopesPanel::endGhost() {
    ghosting_ = false;
    for (auto* v : views_) v->setGhost(nullptr);
}

void ScopesPanel::setZoneHighlight(const QString& key) {
    for (auto* v : views_) v->setZoneHighlight(key);
}

void ScopesPanel::setProbe(std::optional<std::array<int, 3>> p) {
    for (auto* v : views_) v->setProbe(p);
}

}  // namespace rd
