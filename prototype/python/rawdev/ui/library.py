"""ライブラリ：サムネイル一覧、★・ラベル・フラグ表示、絞り込み、フィルムストリップ。"""
from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass, field

import numpy as np
from PySide6.QtCore import (Qt, QAbstractListModel, QModelIndex, QSortFilterProxyModel, QSize, QRect,
                            QRectF, Signal, QObject, QRunnable, QThreadPool, QDir, QPointF)
from PySide6.QtGui import QPainter, QColor, QPen, QImage, QFont, QPolygonF
from PySide6.QtWidgets import (QListView, QStyledItemDelegate, QStyle, QWidget, QHBoxLayout, QVBoxLayout,
                               QComboBox, QToolButton, QLabel, QLineEdit, QAbstractItemView, QFileSystemModel,
                               QTreeView, QSplitter, QPushButton, QButtonGroup)

from ..engine import decode
from ..engine.params import LABELS, LABEL_COLORS, LABEL_NAMES
from ..core.catalog import app_dir
from . import theme


@dataclass
class PhotoItem:
    path: str
    rating: int = 0
    label: str | None = None
    flag: int = 0
    edited: bool = False
    exports: int = 0
    is_export: bool = False       # 書き出したファイル（元 RAW と紐付き）
    missing_link: bool = False
    thumb: QImage | None = None
    mtime: float = 0.0
    meta: dict = field(default_factory=dict)

    @property
    def name(self):
        return os.path.basename(self.path)

    @property
    def ext(self):
        return os.path.splitext(self.path)[1][1:].upper()


# ---------------------------------------------------------------- thumbnails

def thumb_cache_path(path: str, mtime: float) -> str:
    base = os.environ.get("LOCALAPPDATA") or app_dir()
    d = os.path.join(base, "RawDeveloper", "thumbs")
    os.makedirs(d, exist_ok=True)
    h = hashlib.sha1(f"{path}|{mtime}".encode("utf-8")).hexdigest()
    return os.path.join(d, h + ".jpg")


def edited_thumb_path(path: str) -> str | None:
    """編集後のサムネイル（サイドカーの更新日時で区別）。"""
    sc = path + ".rdev"
    if not os.path.exists(sc):
        return None
    return thumb_cache_path(path + "#edited", os.path.getmtime(sc))


class _ThumbSignals(QObject):
    done = Signal(str, QImage)


class ThumbJob(QRunnable):
    def __init__(self, path, mtime, signals):
        super().__init__()
        self.path, self.mtime, self.signals = path, mtime, signals

    def run(self):
        img = QImage()
        sc = self.path + ".rdev"
        if os.path.exists(sc):
            ep = edited_thumb_path(self.path)
            if ep and os.path.exists(ep):
                img.load(ep)
        cp = thumb_cache_path(self.path, self.mtime)
        if img.isNull() and os.path.exists(cp):
            img.load(cp)
        if img.isNull():
            arr = decode.thumbnail_rgb8(self.path, 320)
            if arr is None:
                return
            h, w = arr.shape[:2]
            img = QImage(np.ascontiguousarray(arr).data, w, h, w * 3, QImage.Format_RGB888).copy()
            img.save(cp, "JPG", 85)
        self.signals.done.emit(self.path, img)


class PhotoModel(QAbstractListModel):
    ItemRole = Qt.UserRole + 1

    def __init__(self, parent=None):
        super().__init__(parent)
        self.items: list[PhotoItem] = []
        self.index_of: dict[str, int] = {}
        self.pool = QThreadPool.globalInstance()
        self.signals = _ThumbSignals()
        self.signals.done.connect(self._thumb_done)
        self._pending = set()

    def set_items(self, items):
        self.beginResetModel()
        self.items = items
        self.index_of = {it.path: i for i, it in enumerate(items)}
        self._pending.clear()
        self.endResetModel()

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.items)

    def data(self, idx, role=Qt.DisplayRole):
        if not idx.isValid():
            return None
        it = self.items[idx.row()]
        if role == Qt.DisplayRole:
            return it.name
        if role == self.ItemRole:
            if it.thumb is None and it.path not in self._pending:
                self._pending.add(it.path)
                self.pool.start(ThumbJob(it.path, it.mtime, self.signals))
            return it
        if role == Qt.ToolTipRole:
            return it.path
        return None

    def _thumb_done(self, path, img):
        i = self.index_of.get(path)
        if i is None:
            return
        self.items[i].thumb = img
        ix = self.index(i)
        self.dataChanged.emit(ix, ix)

    def set_thumb(self, path, img: QImage):
        i = self.index_of.get(path)
        if i is not None:
            self.items[i].thumb = img
            ix = self.index(i)
            self.dataChanged.emit(ix, ix)

    def item(self, path) -> PhotoItem | None:
        i = self.index_of.get(path)
        return None if i is None else self.items[i]

    def refresh(self, path):
        i = self.index_of.get(path)
        if i is not None:
            ix = self.index(i)
            self.dataChanged.emit(ix, ix)


class FilterProxy(QSortFilterProxyModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.min_rating = 0
        self.rating_op = ">="     # >= | == | <=
        self.labels: set = set()  # 空なら全部
        self.flag = "all"         # all | pick | reject | unflagged | notreject
        self.kind = "all"         # all | raw | image | edited | exported
        self.text = ""
        self.sort_key = "name"
        self.setDynamicSortFilter(True)

    def filterAcceptsRow(self, row, parent):
        it: PhotoItem = self.sourceModel().items[row]
        r = it.rating
        if self.min_rating or self.rating_op != ">=":
            if self.rating_op == ">=" and r < self.min_rating:
                return False
            if self.rating_op == "==" and r != self.min_rating:
                return False
            if self.rating_op == "<=" and r > self.min_rating:
                return False
        if self.labels:
            if (it.label or "none") not in self.labels:
                return False
        if self.flag == "pick" and it.flag != 1:
            return False
        if self.flag == "reject" and it.flag != -1:
            return False
        if self.flag == "unflagged" and it.flag != 0:
            return False
        if self.flag == "notreject" and it.flag == -1:
            return False
        if self.kind == "raw" and not decode.is_raw(it.path):
            return False
        if self.kind == "image" and decode.is_raw(it.path):
            return False
        if self.kind == "edited" and not it.edited:
            return False
        if self.kind == "exported" and not it.exports:
            return False
        if self.text and self.text.lower() not in it.name.lower():
            return False
        return True

    def lessThan(self, a, b):
        ia = self.sourceModel().items[a.row()]
        ib = self.sourceModel().items[b.row()]
        k = self.sort_key
        if k == "rating":
            return (-ia.rating, ia.name) < (-ib.rating, ib.name)
        if k == "label":
            la = LABELS.index(ia.label) if ia.label in LABELS else 9
            lb = LABELS.index(ib.label) if ib.label in LABELS else 9
            return (la, ia.name) < (lb, ib.name)
        if k == "date":
            return (ia.mtime, ia.name) < (ib.mtime, ib.name)
        return ia.name.lower() < ib.name.lower()

    def refilter(self):
        self.invalidate()
        self.sort(0)


def draw_star(p: QPainter, cx, cy, r, filled):
    import math
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.45
        pts.append(QPointF(cx + math.cos(a) * rr, cy + math.sin(a) * rr))
    p.setBrush(QColor("#f2c14e") if filled else Qt.NoBrush)
    p.setPen(QPen(QColor("#f2c14e") if filled else QColor("#555"), 1))
    p.drawPolygon(QPolygonF(pts))


class ThumbDelegate(QStyledItemDelegate):
    def __init__(self, size=180, compact=False, parent=None):
        super().__init__(parent)
        self.size = size
        self.compact = compact

    def sizeHint(self, opt, idx):
        if self.compact:
            return QSize(int(self.size * 1.25), self.size)
        return QSize(self.size + 12, self.size + 44)

    def paint(self, p: QPainter, opt, idx):
        it: PhotoItem = idx.data(PhotoModel.ItemRole)
        if it is None:
            return
        p.save()
        p.setRenderHint(QPainter.Antialiasing)
        p.setRenderHint(QPainter.SmoothPixmapTransform)
        r = opt.rect.adjusted(3, 3, -3, -3)
        sel = bool(opt.state & QStyle.State_Selected)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor("#3a3a3a") if sel else QColor("#262626"))
        p.drawRoundedRect(r, 4, 4)
        # ラベル色の枠
        if it.label in LABEL_COLORS:
            p.setPen(QPen(QColor(LABEL_COLORS[it.label]), 3))
            p.setBrush(Qt.NoBrush)
            p.drawRoundedRect(QRectF(r).adjusted(1.5, 1.5, -1.5, -1.5), 4, 4)
        img_area = QRect(r.left() + 6, r.top() + 6, r.width() - 12, r.height() - (12 if self.compact else 46))
        if it.thumb is not None and not it.thumb.isNull():
            ts = it.thumb.size().scaled(img_area.size(), Qt.KeepAspectRatio)
            tr = QRect(0, 0, ts.width(), ts.height())
            tr.moveCenter(img_area.center())
            if it.flag == -1:
                p.setOpacity(0.35)
            p.drawImage(tr, it.thumb)
            p.setOpacity(1.0)
        else:
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(img_area, Qt.AlignCenter, "読み込み中…")
        # 種類・状態
        p.setFont(QFont(opt.font.family(), 7))
        tag_x = r.left() + 8
        tags = [it.ext]
        if it.edited:
            tags.append("編集済")
        if it.exports:
            tags.append(f"書出 {it.exports}")
        if it.is_export:
            tags.append("書出ファイル" if not it.missing_link else "元ファイル不明")
        for t in tags:
            fm = p.fontMetrics()
            w = fm.horizontalAdvance(t) + 8
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(0, 0, 0, 160))
            p.drawRoundedRect(QRectF(tag_x, r.top() + 8, w, 14), 3, 3)
            p.setPen(QColor("#ddd"))
            p.drawText(QRectF(tag_x, r.top() + 8, w, 14), Qt.AlignCenter, t)
            tag_x += w + 3
        # フラグ
        if it.flag:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor("#ffffff") if it.flag == 1 else QColor("#e0443e"))
            fx, fy = r.right() - 20, r.top() + 8
            p.drawRect(QRectF(fx, fy, 2, 14))
            p.drawPolygon(QPolygonF([QPointF(fx + 2, fy), QPointF(fx + 12, fy + 4), QPointF(fx + 2, fy + 8)]))
            if it.flag == -1:
                p.setPen(QPen(QColor("#e0443e"), 2))
                p.drawLine(QPointF(fx - 3, fy + 15), QPointF(fx + 13, fy - 1))
        if not self.compact:
            # ★
            for i in range(5):
                draw_star(p, r.left() + 14 + i * 15, r.bottom() - 30, 5.5, i < it.rating)
            p.setPen(QColor(theme.TEXT if sel else theme.TEXT_DIM))
            p.setFont(QFont(opt.font.family(), 8))
            p.drawText(QRect(r.left() + 8, r.bottom() - 20, r.width() - 16, 16), Qt.AlignLeft | Qt.AlignVCenter,
                       p.fontMetrics().elidedText(it.name, Qt.ElideMiddle, r.width() - 16))
        else:
            if it.rating:
                for i in range(it.rating):
                    draw_star(p, r.left() + 10 + i * 10, r.bottom() - 9, 4, True)
        p.restore()


class ThumbGrid(QListView):
    middleClicked = Signal(str)

    def __init__(self, compact=False, parent=None):
        super().__init__(parent)
        self.setViewMode(QListView.IconMode)
        self.setResizeMode(QListView.Adjust)
        self.setMovement(QListView.Static)
        self.setUniformItemSizes(True)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.setSpacing(2)
        self.setMouseTracking(True)
        self.delegate = ThumbDelegate(110 if compact else 180, compact)
        self.setItemDelegate(self.delegate)
        if compact:
            self.setFlow(QListView.LeftToRight)
            self.setWrapping(False)
            self.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
            self.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
            self.setFixedHeight(self.delegate.size + 22)

    def set_thumb_size(self, s):
        self.delegate.size = s
        self.doItemsLayout()

    def mousePressEvent(self, e):
        if e.button() == Qt.MiddleButton:
            idx = self.indexAt(e.position().toPoint())
            if idx.isValid():
                it = idx.data(PhotoModel.ItemRole)
                self.middleClicked.emit(it.path)
            return
        super().mousePressEvent(e)

    def wheelEvent(self, e):
        if self.flow() == QListView.LeftToRight and not self.isWrapping():
            sb = self.horizontalScrollBar()
            sb.setValue(sb.value() - e.angleDelta().y())
            return
        if e.modifiers() & Qt.ControlModifier:
            s = self.delegate.size + (20 if e.angleDelta().y() > 0 else -20)
            self.set_thumb_size(max(100, min(400, s)))
            return
        super().wheelEvent(e)


class FilterBar(QWidget):
    changed = Signal()

    def __init__(self, proxy: FilterProxy, parent=None):
        super().__init__(parent)
        self.proxy = proxy
        h = QHBoxLayout(self)
        h.setContentsMargins(8, 4, 8, 4)
        h.setSpacing(6)
        h.addWidget(QLabel("★"))
        self.op = QComboBox()
        self.op.addItems(["以上", "と等しい", "以下"])
        self.stars = QComboBox()
        self.stars.addItems(["指定なし", "★1", "★2", "★3", "★4", "★5", "★なし(0)"])
        h.addWidget(self.stars)
        h.addWidget(self.op)
        h.addSpacing(8)
        h.addWidget(QLabel("ラベル"))
        self.label_btns = {}
        for lb in LABELS + ["none"]:
            b = QToolButton()
            b.setCheckable(True)
            b.setFixedSize(18, 18)
            col = LABEL_COLORS.get(lb, "#555")
            b.setStyleSheet(f"QToolButton{{background:{col}; border:1px solid #222; border-radius:3px;}}"
                            f"QToolButton:checked{{border:2px solid #fff;}}")
            b.setToolTip(LABEL_NAMES.get(lb, "ラベルなし"))
            b.toggled.connect(self._apply)
            self.label_btns[lb] = b
            h.addWidget(b)
        h.addSpacing(8)
        h.addWidget(QLabel("フラグ"))
        self.flag = QComboBox()
        self.flag.addItems(["すべて", "採用のみ", "除外のみ", "フラグなし", "除外以外"])
        h.addWidget(self.flag)
        self.kind = QComboBox()
        self.kind.addItems(["すべての種類", "RAW のみ", "JPEG・TIFF・PNG のみ", "編集済み", "書き出し済み"])
        h.addWidget(self.kind)
        h.addSpacing(8)
        h.addWidget(QLabel("並べ替え"))
        self.sort = QComboBox()
        self.sort.addItems(["ファイル名", "撮影日時（更新日時）", "★順", "ラベル順"])
        h.addWidget(self.sort)
        self.search = QLineEdit()
        self.search.setPlaceholderText("ファイル名で検索")
        self.search.setFixedWidth(160)
        h.addWidget(self.search)
        h.addStretch(1)
        self.count = QLabel("")
        self.count.setStyleSheet(f"color:{theme.TEXT_DIM}")
        h.addWidget(self.count)
        for w in (self.op, self.stars, self.flag, self.kind, self.sort):
            w.currentIndexChanged.connect(self._apply)
        self.search.textChanged.connect(self._apply)

    def _apply(self, *_):
        px = self.proxy
        si = self.stars.currentIndex()
        if si == 0:
            px.min_rating, px.rating_op = 0, ">="
        elif si == 6:
            px.min_rating, px.rating_op = 0, "=="
        else:
            px.min_rating = si
            px.rating_op = [">=", "==", "<="][self.op.currentIndex()]
        px.labels = {k for k, b in self.label_btns.items() if b.isChecked()}
        px.flag = ["all", "pick", "reject", "unflagged", "notreject"][self.flag.currentIndex()]
        px.kind = ["all", "raw", "image", "edited", "exported"][self.kind.currentIndex()]
        px.sort_key = ["name", "date", "rating", "label"][self.sort.currentIndex()]
        px.text = self.search.text().strip()
        px.refilter()
        self.changed.emit()

    def describe(self):
        return f"{self.proxy.rowCount()} / {self.proxy.sourceModel().rowCount()} 枚"


class FolderTree(QTreeView):
    folderChosen = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.fs = QFileSystemModel(self)
        self.fs.setFilter(QDir.AllDirs | QDir.NoDotAndDotDot | QDir.Drives)
        self.fs.setRootPath("")
        self.setModel(self.fs)
        for c in (1, 2, 3):
            self.hideColumn(c)
        self.setHeaderHidden(True)
        self.clicked.connect(lambda ix: self.folderChosen.emit(self.fs.filePath(ix)))

    def reveal(self, path):
        ix = self.fs.index(path)
        if ix.isValid():
            self.setCurrentIndex(ix)
            self.scrollTo(ix)
            self.expand(ix)
