#include "ui/Library.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QThreadPool>
#include <QToolButton>
#include <QWheelEvent>
#include <cmath>

#include "core/Catalog.h"
#include "core/Xmp.h"
#include "engine/Paths.h"
#include "engine/RawLoader.h"
#include "ui/Theme.h"

namespace rd {

const QStringList kLabels{"red", "yellow", "green", "blue", "purple"};

QString labelColor(const QString& l) {
    if (l == "red") return "#e0443e";
    if (l == "yellow") return "#e6c229";
    if (l == "green") return "#3fae49";
    if (l == "blue") return "#3b7dd8";
    if (l == "purple") return "#9b59b6";
    return "#555";
}

QString labelName(const QString& l) {
    if (l == "red") return QStringLiteral("レッド");
    if (l == "yellow") return QStringLiteral("イエロー");
    if (l == "green") return QStringLiteral("グリーン");
    if (l == "blue") return QStringLiteral("ブルー");
    if (l == "purple") return QStringLiteral("パープル");
    return QStringLiteral("なし");
}

bool isRawPhoto(const QString& path) { return isRawExtension(QFileInfo(path).suffix().toLower().toStdString()); }

bool isSupportedPhoto(const QString& path) {
    const QString e = QFileInfo(path).suffix().toLower();
    return isRawPhoto(path) || e == "jpg" || e == "jpeg" || e == "png" || e == "tif" || e == "tiff";
}

QString thumbCachePath(const QString& key, qint64 stamp) {
    const QString d = cacheDir() + "/thumbs";
    QDir().mkpath(d);
    const QByteArray h = QCryptographicHash::hash((QDir::toNativeSeparators(key) + "|" + QString::number(stamp)).toUtf8(),
                                                  QCryptographicHash::Sha1).toHex();
    return d + "/" + QString::fromLatin1(h) + ".jpg";
}

QString editedThumbPath(const QString& path) {
    const QFileInfo sc(path + ".rdev");
    if (!sc.exists()) return {};
    return thumbCachePath(path + "#edited", sc.lastModified().toMSecsSinceEpoch());
}

std::vector<PhotoItem> scanFolder(const QString& folder) {
    std::vector<PhotoItem> items;
    const QFileInfoList files = QDir(folder).entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    auto& cat = Catalog::get();
    for (const QFileInfo& fi : files) {
        const QString p = fi.absoluteFilePath();
        if (!isSupportedPhoto(p)) continue;
        PhotoItem it;
        it.path = p;
        it.name = fi.fileName();
        it.isRaw = isRawPhoto(p);
        it.mtime = fi.lastModified().toMSecsSinceEpoch();
        const SidecarMark sm = readSidecarMark(p);
        if (sm.exists) {
            it.rating = sm.mark.rating;
            it.label = sm.mark.label;
            it.flag = sm.mark.flag;
            it.edited = sm.mark.edited;
        } else if (auto m = cat.mark(p)) {
            it.rating = m->rating;
            it.label = m->label;
            it.flag = m->flag;
        } else {
            const auto r = xmp::readFileRating(p);
            it.rating = std::max(0, r.rating.value_or(0));
            it.label = r.label;
        }
        int n = 0;
        for (const QString& e : cat.exportsOf(p))
            if (QFileInfo::exists(e)) ++n;
        it.exports = n;
        const QString src = cat.sourceOf(p);
        if (!src.isEmpty()) {
            it.isExport = true;
            it.missingLink = !QFileInfo::exists(src);
        }
        items.push_back(std::move(it));
    }
    return items;
}

// ================================================================ model

PhotoModel::PhotoModel(QObject* parent) : QAbstractListModel(parent) {}

void PhotoModel::setItems(std::vector<PhotoItem> items) {
    beginResetModel();
    items_ = std::move(items);
    index_.clear();
    for (int i = 0; i < int(items_.size()); ++i) index_[items_[i].path] = i;
    pending_.clear();
    ++token_;
    endResetModel();
}

int PhotoModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(items_.size()); }

QVariant PhotoModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() >= int(items_.size())) return {};
    const PhotoItem& it = items_[idx.row()];
    if (role == Qt::DisplayRole) return it.name;
    if (role == Qt::ToolTipRole) return QDir::toNativeSeparators(it.path);
    if (role == ItemRole) {
        if (it.thumb.isNull() && !pending_.contains(it.path)) requestThumb(idx.row());
        return QVariant::fromValue(static_cast<const void*>(&it));
    }
    return {};
}

void PhotoModel::requestThumb(int row) const {
    const PhotoItem& it = items_[row];
    pending_.insert(it.path);
    const QString path = it.path;
    const qint64 mtime = it.mtime;
    const int token = token_;
    auto* self = const_cast<PhotoModel*>(this);
    QThreadPool::globalInstance()->start([self, path, mtime, token] {
        QImage img;
        const QString ep = editedThumbPath(path);
        if (!ep.isEmpty() && QFileInfo::exists(ep)) img.load(ep);
        const QString cp = thumbCachePath(path, mtime);
        if (img.isNull() && QFileInfo::exists(cp)) img.load(cp);
        if (img.isNull()) {
            if (isRawPhoto(path)) {
                QFile f(path);
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray b = f.readAll();
                    int flip = 0;
                    const auto jpg = extractThumbJpeg(std::vector<unsigned char>(b.begin(), b.end()), flip);
                    if (!jpg.empty()) {
                        QImageReader r;
                        QByteArray jb(reinterpret_cast<const char*>(jpg.data()), int(jpg.size()));
                        img.loadFromData(jb);
                        if (!img.isNull()) {
                            img = img.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                            QTransform t;
                            if (flip == 3) t.rotate(180);
                            else if (flip == 5) t.rotate(-90);
                            else if (flip == 6) t.rotate(90);
                            img = img.transformed(t);
                        }
                    }
                }
            } else {
                QImageReader r(path);
                r.setAutoTransform(true);
                const QSize sz = r.size();
                if (sz.isValid()) r.setScaledSize(sz.scaled(320, 320, Qt::KeepAspectRatio));
                img = r.read();
            }
            if (!img.isNull()) img.save(cp, "JPG", 85);
        }
        if (img.isNull()) return;
        QMetaObject::invokeMethod(self, [self, path, img, token] {
            if (token == self->token_) self->setThumb(path, img);
        });
    });
}

PhotoItem* PhotoModel::item(const QString& path) {
    auto it = index_.find(path);
    return it == index_.end() ? nullptr : &items_[it.value()];
}

void PhotoModel::refresh(const QString& path) {
    auto it = index_.find(path);
    if (it == index_.end()) return;
    const QModelIndex ix = index(it.value());
    emit dataChanged(ix, ix);
}

void PhotoModel::setThumb(const QString& path, const QImage& img) {
    if (PhotoItem* it = item(path)) {
        it->thumb = img;
        refresh(path);
    }
}

// ================================================================ filter

void FilterProxy::refilter() {
    invalidate();
    sort(0);
}

bool FilterProxy::filterAcceptsRow(int row, const QModelIndex&) const {
    auto* m = static_cast<PhotoModel*>(sourceModel());
    const PhotoItem* it = m->itemAt(row);
    if (!it) return false;
    const int r = it->rating;
    if (minRating || ratingOp != ">=") {
        if (ratingOp == ">=" && r < minRating) return false;
        if (ratingOp == "==" && r != minRating) return false;
        if (ratingOp == "<=" && r > minRating) return false;
    }
    if (!labels.isEmpty() && !labels.contains(it->label.isEmpty() ? "none" : it->label)) return false;
    if (flag == "pick" && it->flag != 1) return false;
    if (flag == "reject" && it->flag != -1) return false;
    if (flag == "unflagged" && it->flag != 0) return false;
    if (flag == "notreject" && it->flag == -1) return false;
    if (kind == "raw" && !it->isRaw) return false;
    if (kind == "image" && it->isRaw) return false;
    if (kind == "edited" && !it->edited) return false;
    if (kind == "exported" && !it->exports) return false;
    if (!text.isEmpty() && !it->name.contains(text, Qt::CaseInsensitive)) return false;
    return true;
}

bool FilterProxy::lessThan(const QModelIndex& a, const QModelIndex& b) const {
    auto* m = static_cast<PhotoModel*>(sourceModel());
    const PhotoItem* ia = m->itemAt(a.row());
    const PhotoItem* ib = m->itemAt(b.row());
    const int byName = QString::compare(ia->name, ib->name, Qt::CaseInsensitive);
    if (sortKey == "rating" && ia->rating != ib->rating) return ia->rating > ib->rating;
    if (sortKey == "label") {
        const int la = ia->label.isEmpty() ? 9 : kLabels.indexOf(ia->label), lb = ib->label.isEmpty() ? 9 : kLabels.indexOf(ib->label);
        if (la != lb) return la < lb;
    }
    if (sortKey == "date" && ia->mtime != ib->mtime) return ia->mtime < ib->mtime;
    return byName < 0;
}

// ================================================================ delegate

namespace {
void drawStar(QPainter* p, QPointF c, double r, bool filled) {
    QPolygonF poly;
    for (int i = 0; i < 10; ++i) {
        const double a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 == 0 ? r : r * 0.45;
        poly << QPointF(c.x() + std::cos(a) * rr, c.y() + std::sin(a) * rr);
    }
    p->setBrush(filled ? QColor("#f2c14e") : QColor(Qt::transparent));
    p->setPen(QPen(filled ? QColor("#f2c14e") : QColor("#555"), 1));
    p->drawPolygon(poly);
}
}  // namespace

QSize ThumbDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return compact ? QSize(int(size * 1.25), size) : QSize(size + 12, size + 44);
}

void ThumbDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
    const auto* it = static_cast<const PhotoItem*>(idx.data(PhotoModel::ItemRole).value<const void*>());
    if (!it) return;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect r = opt.rect.adjusted(3, 3, -3, -3);
    const bool sel = opt.state & QStyle::State_Selected;
    p->setPen(Qt::NoPen);
    p->setBrush(sel ? QColor("#3a3a3a") : QColor("#262626"));
    p->drawRoundedRect(r, 4, 4);
    if (!it->label.isEmpty()) {
        p->setPen(QPen(QColor(labelColor(it->label)), 3));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(r).adjusted(1.5, 1.5, -1.5, -1.5), 4, 4);
    }
    const QRect area(r.left() + 6, r.top() + 6, r.width() - 12, r.height() - (compact ? 12 : 46));
    if (!it->thumb.isNull()) {
        QSize ts = it->thumb.size().scaled(area.size(), Qt::KeepAspectRatio);
        QRect tr(QPoint(0, 0), ts);
        tr.moveCenter(area.center());
        if (it->flag == -1) p->setOpacity(0.35);
        p->drawImage(tr, it->thumb);
        p->setOpacity(1.0);
    } else {
        p->setPen(theme::kTextDim);
        p->drawText(area, Qt::AlignCenter, QStringLiteral("読み込み中…"));
    }
    QFont f = opt.font;
    f.setPointSizeF(7);
    p->setFont(f);
    QStringList tags{QFileInfo(it->path).suffix().toUpper()};
    if (it->edited) tags << QStringLiteral("編集済");
    if (it->exports) tags << QStringLiteral("書出 %1").arg(it->exports);
    if (it->isExport) tags << (it->missingLink ? QStringLiteral("元ファイル不明") : QStringLiteral("書出ファイル"));
    double tx = r.left() + 8;
    for (const QString& t : tags) {
        const int w = p->fontMetrics().horizontalAdvance(t) + 8;
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 160));
        p->drawRoundedRect(QRectF(tx, r.top() + 8, w, 14), 3, 3);
        p->setPen(QColor("#ddd"));
        p->drawText(QRectF(tx, r.top() + 8, w, 14), Qt::AlignCenter, t);
        tx += w + 3;
    }
    if (it->flag) {
        const double fx = r.right() - 20, fy = r.top() + 8;
        p->setPen(Qt::NoPen);
        p->setBrush(it->flag == 1 ? QColor(Qt::white) : QColor("#e0443e"));
        p->drawRect(QRectF(fx, fy, 2, 14));
        p->drawPolygon(QPolygonF{QPointF(fx + 2, fy), QPointF(fx + 12, fy + 4), QPointF(fx + 2, fy + 8)});
        if (it->flag == -1) {
            p->setPen(QPen(QColor("#e0443e"), 2));
            p->drawLine(QPointF(fx - 3, fy + 15), QPointF(fx + 13, fy - 1));
        }
    }
    if (!compact) {
        for (int i = 0; i < 5; ++i) drawStar(p, QPointF(r.left() + 14 + i * 15, r.bottom() - 30), 5.5, i < it->rating);
        p->setPen(sel ? theme::kText : theme::kTextDim);
        f.setPointSizeF(8);
        p->setFont(f);
        p->drawText(QRect(r.left() + 8, r.bottom() - 20, r.width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(it->name, Qt::ElideMiddle, r.width() - 16));
    } else {
        for (int i = 0; i < it->rating; ++i) drawStar(p, QPointF(r.left() + 10 + i * 10, r.bottom() - 9), 4, true);
    }
    p->restore();
}

// ================================================================ view

ThumbView::ThumbView(bool compact, QWidget* parent) : QListView(parent), compact_(compact) {
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSpacing(2);
    delegate_ = new ThumbDelegate(compact ? 100 : 180, compact, this);
    setItemDelegate(delegate_);
    if (compact) {
        setFlow(QListView::LeftToRight);
        setWrapping(false);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFixedHeight(delegate_->size + 22);
        setToolTip(QStringLiteral("中クリック：その写真の調整を今の写真にコピー"));
    }
}

void ThumbView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) {
        const QModelIndex idx = indexAt(e->position().toPoint());
        if (idx.isValid())
            emit middleClicked(static_cast<const PhotoItem*>(idx.data(PhotoModel::ItemRole).value<const void*>())->path);
        return;
    }
    QListView::mousePressEvent(e);
}

void ThumbView::wheelEvent(QWheelEvent* e) {
    if (compact_) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - e->angleDelta().y());
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
        delegate_->size = std::clamp(delegate_->size + (e->angleDelta().y() > 0 ? 20 : -20), 100, 400);
        doItemsLayout();
        return;
    }
    QListView::wheelEvent(e);
}

// ================================================================ filter bar

FilterBar::FilterBar(FilterProxy* proxy, QWidget* parent) : QWidget(parent), proxy_(proxy) {
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(8, 4, 8, 4);
    h->setSpacing(6);
    h->addWidget(new QLabel(QStringLiteral("★")));
    stars_ = new QComboBox;
    stars_->addItems({QStringLiteral("指定なし"), "★1", "★2", "★3", "★4", "★5", QStringLiteral("★なし(0)")});
    op_ = new QComboBox;
    op_->addItems({QStringLiteral("以上"), QStringLiteral("と等しい"), QStringLiteral("以下")});
    h->addWidget(stars_);
    h->addWidget(op_);
    h->addSpacing(8);
    h->addWidget(new QLabel(QStringLiteral("ラベル")));
    for (const QString& lb : kLabels + QStringList{"none"}) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setFixedSize(18, 18);
        b->setStyleSheet(QString("QToolButton{background:%1; border:1px solid #222; border-radius:3px;}"
                                 "QToolButton:checked{border:2px solid #fff;}").arg(labelColor(lb)));
        b->setToolTip(lb == "none" ? QStringLiteral("ラベルなし") : labelName(lb));
        connect(b, &QToolButton::toggled, this, [this] { apply(); });
        labelBtns_[lb] = b;
        h->addWidget(b);
    }
    h->addSpacing(8);
    h->addWidget(new QLabel(QStringLiteral("フラグ")));
    flag_ = new QComboBox;
    flag_->addItems({QStringLiteral("すべて"), QStringLiteral("採用のみ"), QStringLiteral("除外のみ"),
                     QStringLiteral("フラグなし"), QStringLiteral("除外以外")});
    h->addWidget(flag_);
    kind_ = new QComboBox;
    kind_->addItems({QStringLiteral("すべての種類"), QStringLiteral("RAW のみ"), QStringLiteral("JPEG・TIFF・PNG のみ"),
                     QStringLiteral("編集済み"), QStringLiteral("書き出し済み")});
    h->addWidget(kind_);
    h->addSpacing(8);
    h->addWidget(new QLabel(QStringLiteral("並べ替え")));
    sort_ = new QComboBox;
    sort_->addItems({QStringLiteral("ファイル名"), QStringLiteral("更新日時"), QStringLiteral("★順"), QStringLiteral("ラベル順")});
    h->addWidget(sort_);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("ファイル名で検索"));
    search_->setFixedWidth(160);
    h->addWidget(search_);
    h->addStretch(1);
    count_ = new QLabel;
    count_->setStyleSheet("color:#8c8c8c");
    h->addWidget(count_);
    for (QComboBox* c : {stars_, op_, flag_, kind_, sort_}) connect(c, &QComboBox::currentIndexChanged, this, [this] { apply(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { apply(); });
}

void FilterBar::apply() {
    const int si = stars_->currentIndex();
    if (si == 0) {
        proxy_->minRating = 0;
        proxy_->ratingOp = ">=";
    } else if (si == 6) {
        proxy_->minRating = 0;
        proxy_->ratingOp = "==";
    } else {
        proxy_->minRating = si;
        proxy_->ratingOp = QStringList{">=", "==", "<="}[op_->currentIndex()];
    }
    proxy_->labels.clear();
    for (auto it = labelBtns_.begin(); it != labelBtns_.end(); ++it)
        if (it.value()->isChecked()) proxy_->labels.insert(it.key());
    proxy_->flag = QStringList{"all", "pick", "reject", "unflagged", "notreject"}[flag_->currentIndex()];
    proxy_->kind = QStringList{"all", "raw", "image", "edited", "exported"}[kind_->currentIndex()];
    proxy_->sortKey = QStringList{"name", "date", "rating", "label"}[sort_->currentIndex()];
    proxy_->text = search_->text().trimmed();
    proxy_->refilter();
    updateCount();
    emit changed();
}

void FilterBar::updateCount() {
    count_->setText(QStringLiteral("%1 / %2 枚").arg(proxy_->rowCount()).arg(proxy_->sourceModel()->rowCount()));
}

// ================================================================ folder tree

FolderTree::FolderTree(QWidget* parent) : QTreeView(parent) {
    fs_ = new QFileSystemModel(this);
    fs_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    fs_->setRootPath("");
    setModel(fs_);
    for (int c = 1; c < 4; ++c) hideColumn(c);
    setHeaderHidden(true);
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex& ix) { emit folderChosen(fs_->filePath(ix)); });
}

void FolderTree::reveal(const QString& path) {
    const QModelIndex ix = fs_->index(path);
    if (ix.isValid()) {
        setCurrentIndex(ix);
        scrollTo(ix);
        expand(ix);
    }
}

}  // namespace rd
