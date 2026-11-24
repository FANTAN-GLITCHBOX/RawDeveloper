"""ヒストグラムとスコープ（DaVinci Resolve 風）。

表示：ヒストグラム（RGB 重ね／輝度／RGB 別）、波形、RGB パレード、ベクトルスコープ
・背景を「黒レベル・シャドウ・露出・ハイライト・白レベル」の 5 区画に色分けして名前を表示
・補正スライダーにマウスを乗せると該当区画が光る。区画を左右ドラッグするとそのスライダーが動く
・ヒストグラムをなぞると写真の該当画素を色付き表示（hoverRange）
・写真の上のカーソル位置の値を縦線で表示（set_probe）
・白飛び／黒つぶれの割合を表示。スライダー操作中は操作前の形を灰色で重ねる
"""
from __future__ import annotations

import numpy as np
from PySide6.QtCore import Qt, Signal, QRectF, QPointF, QSettings
from PySide6.QtGui import QPainter, QColor, QPen, QImage, QPainterPath, QFont
from PySide6.QtWidgets import (QWidget, QMenu, QHBoxLayout, QVBoxLayout, QGridLayout, QToolButton,
                               QComboBox, QSizePolicy, QLabel)

from . import theme

ZONES = [  # (key, 名前, 開始, 終了)
    ("blacks", "黒レベル", 0.00, 0.06),
    ("shadows", "シャドウ", 0.06, 0.30),
    ("exposure", "露出", 0.30, 0.70),
    ("highlights", "ハイライト", 0.70, 0.94),
    ("whites", "白レベル", 0.94, 1.00),
]
MODES = {
    "hist_rgb": "ヒストグラム（RGB）",
    "hist_luma": "ヒストグラム（輝度）",
    "hist_split": "ヒストグラム（RGB 別）",
    "waveform": "波形",
    "parade": "RGB パレード",
    "vector": "ベクトルスコープ",
}


class ScopeData:
    """表示画像（uint8 RGB）から各スコープ用の集計を作る。"""

    def __init__(self, rgb8: np.ndarray):
        h, w = rgb8.shape[:2]
        step = max(1, int(max(h, w) / 480))
        a = rgb8[::step, ::step].astype(np.int32)
        self.small = a
        self.hist = np.stack([np.bincount(a[..., c].ravel(), minlength=256) for c in range(3)]).astype(np.float32)
        luma = (a[..., 0] * 54 + a[..., 1] * 183 + a[..., 2] * 19) >> 8
        self.luma = luma
        self.hist_l = np.bincount(luma.ravel(), minlength=256).astype(np.float32)
        n = luma.size
        self.clip_hi = float((a.max(axis=2) >= 254).sum()) / n
        self.clip_lo = float((a.min(axis=2) <= 1).sum()) / n
        self._wave = None
        self._parade = None
        self._vec = None

    def wave(self):
        if self._wave is None:
            H, W = self.luma.shape
            cols = 256
            xi = (np.arange(W) * cols // max(W, 1))
            xi = np.broadcast_to(xi, (H, W))
            idx = self.luma * cols + xi
            self._wave = np.bincount(idx.ravel(), minlength=256 * cols).reshape(256, cols).astype(np.float32)
        return self._wave

    def parade(self):
        if self._parade is None:
            H, W = self.luma.shape
            cols = 160
            xi = np.broadcast_to(np.arange(W) * cols // max(W, 1), (H, W))
            out = []
            for c in range(3):
                idx = self.small[..., c] * cols + xi
                out.append(np.bincount(idx.ravel(), minlength=256 * cols).reshape(256, cols).astype(np.float32))
            self._parade = out
        return self._parade

    def vector(self):
        if self._vec is None:
            a = self.small.astype(np.float32)
            cb = -0.1687 * a[..., 0] - 0.3313 * a[..., 1] + 0.5 * a[..., 2]
            cr = 0.5 * a[..., 0] - 0.4187 * a[..., 1] - 0.0813 * a[..., 2]
            N = 200
            xi = np.clip((cb / 128 * 0.5 + 0.5) * (N - 1) * 1.0, 0, N - 1).astype(np.int32)
            yi = np.clip((0.5 - cr / 128 * 0.5) * (N - 1), 0, N - 1).astype(np.int32)
            self._vec = np.bincount((yi * N + xi).ravel(), minlength=N * N).reshape(N, N).astype(np.float32)
        return self._vec


def _glow_image(arr: np.ndarray, rgb=(255, 255, 255), gain=1.0) -> QImage:
    """件数の 2 次元表を、少ない所も見える発光風の画像に。"""
    v = np.log1p(arr) / max(np.log1p(arr.max()), 1e-6)
    v = np.clip(v * 1.35 * gain, 0, 1)
    h, w = v.shape
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0] = (v * rgb[0]).astype(np.uint8)
    img[..., 1] = (v * rgb[1]).astype(np.uint8)
    img[..., 2] = (v * rgb[2]).astype(np.uint8)
    img[..., 3] = (np.clip(v * 1.4, 0, 1) * 255).astype(np.uint8)
    q = QImage(img.data, w, h, w * 4, QImage.Format_RGBA8888)
    return q.copy()


class ScopeView(QWidget):
    hoverRange = Signal(object)       # (lo, hi) 0..255 / None
    zoneDrag = Signal(str, float, bool)
    modeChanged = Signal(str)

    def __init__(self, mode="hist_rgb", parent=None):
        super().__init__(parent)
        self.mode = mode
        self.data: ScopeData | None = None
        self.ghost: ScopeData | None = None
        self.log = False
        self.probe = None
        self.zone_hi = None
        self._hover_x = None
        self._drag = None
        self.setMouseTracking(True)
        self.setMinimumSize(160, 110)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

    # ---- 描画
    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.fillRect(self.rect(), QColor("#141414"))
        if self.mode.startswith("hist"):
            self._paint_hist(p)
        elif self.mode == "waveform":
            self._paint_wave(p)
        elif self.mode == "parade":
            self._paint_parade(p)
        else:
            self._paint_vector(p)
        p.setPen(QColor("#666"))
        p.setFont(QFont(self.font().family(), 7))
        p.drawText(QRectF(4, 2, self.width() - 8, 12), Qt.AlignRight, MODES[self.mode])

    def _plot_rect(self):
        return QRectF(6, 14, self.width() - 12, self.height() - 34)

    def _hist_path(self, h, r, scale):
        path = QPainterPath(QPointF(r.left(), r.bottom()))
        n = len(h)
        for i in range(n):
            x = r.left() + i / (n - 1) * r.width()
            path.lineTo(x, r.bottom() - min(1.0, h[i] / scale) * r.height())
        path.lineTo(r.right(), r.bottom())
        path.closeSubpath()
        return path

    def _norm(self, h):
        if self.log:
            return np.log1p(h)
        return h

    def _paint_zones(self, p, r):
        for key, name, a, b in ZONES:
            x0 = r.left() + a * r.width()
            x1 = r.left() + b * r.width()
            hi = (self.zone_hi == key) or (self._drag and self._drag[0] == key)
            c = QColor(theme.ACCENT) if hi else QColor("#ffffff")
            c.setAlpha(38 if hi else (10 if ZONES.index((key, name, a, b)) % 2 == 0 else 0))
            p.fillRect(QRectF(x0, r.top(), x1 - x0, r.height()), c)
            p.setPen(QColor(theme.ACCENT) if hi else QColor("#6a6a6a"))
            f = QFont(self.font().family(), 7)
            p.setFont(f)
            fm = p.fontMetrics()
            if fm.horizontalAdvance(name) + 2 <= (x1 - x0):
                p.drawText(QRectF(x0, r.bottom() + 2, x1 - x0, 12), Qt.AlignCenter, name)
            elif fm.horizontalAdvance(name[:1]) + 2 <= (x1 - x0):
                p.drawText(QRectF(x0, r.bottom() + 2, x1 - x0, 12), Qt.AlignCenter, name[:1])

    def _paint_hist(self, p):
        r = self._plot_rect()
        self._paint_zones(p, r)
        d = self.data
        if d is None:
            return
        if self.mode == "hist_luma":
            series = [(self._norm(d.hist_l), QColor(220, 220, 220, 170))]
            ghost = [self._norm(self.ghost.hist_l)] if self.ghost else []
        else:
            cols = [QColor(255, 70, 70, 150), QColor(70, 220, 70, 150), QColor(80, 130, 255, 150)]
            series = [(self._norm(d.hist[c]), cols[c]) for c in range(3)]
            ghost = [self._norm(self.ghost.hist_l)] if self.ghost else []
        # 端の値（白飛び）で全体が潰れないよう、端を除いた上位で正規化
        allh = np.concatenate([s[0][2:-2] for s in series])
        scale = max(np.percentile(allh, 99.5) * 1.1, 1.0)
        for g in ghost:
            p.setPen(QPen(QColor(150, 150, 150, 120), 1))
            p.setBrush(QColor(120, 120, 120, 40))
            p.drawPath(self._hist_path(g, r, max(np.percentile(g[2:-2], 99.5) * 1.1, 1)))
        if self.mode == "hist_split":
            hh = r.height() / 3
            for i, (h, c) in enumerate(series):
                rr = QRectF(r.left(), r.top() + i * hh, r.width(), hh - 2)
                p.setPen(Qt.NoPen)
                c2 = QColor(c)
                c2.setAlpha(200)
                p.setBrush(c2)
                p.drawPath(self._hist_path(h, rr, max(np.percentile(h[2:-2], 99.5) * 1.1, 1)))
        else:
            p.setCompositionMode(QPainter.CompositionMode_Plus)
            for h, c in series:
                p.setPen(Qt.NoPen)
                p.setBrush(c)
                p.drawPath(self._hist_path(h, r, scale))
            p.setCompositionMode(QPainter.CompositionMode_SourceOver)
            # 輪郭線（重なっても形が追えるように）
            for h, c in series:
                cc = QColor(c)
                cc.setAlpha(255)
                p.setPen(QPen(cc, 1))
                p.setBrush(Qt.NoBrush)
                path = QPainterPath(QPointF(r.left(), r.bottom() - min(1, h[0] / scale) * r.height()))
                for i in range(1, 256):
                    path.lineTo(r.left() + i / 255 * r.width(), r.bottom() - min(1, h[i] / scale) * r.height())
                p.drawPath(path)
        # 白飛び・黒つぶれ
        p.setFont(QFont(self.font().family(), 7))
        p.setPen(QColor("#5a8cff") if d.clip_lo > 0.001 else QColor("#777"))
        p.drawText(QRectF(r.left() + 2, r.top(), 90, 12), Qt.AlignLeft, f"黒つぶれ {d.clip_lo * 100:.1f}%")
        p.setPen(QColor("#ff6a6a") if d.clip_hi > 0.001 else QColor("#777"))
        p.drawText(QRectF(r.right() - 92, r.top() + 12, 90, 12), Qt.AlignRight, f"白飛び {d.clip_hi * 100:.1f}%")
        self._paint_probe_v(p, r)
        if self._hover_x is not None:
            v = (self._hover_x - r.left()) / r.width()
            x = r.left() + v * r.width()
            p.setPen(QPen(QColor(theme.ACCENT), 1, Qt.DashLine))
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))

    def _paint_probe_v(self, p, r):
        if self.probe is None:
            return
        cols = ["#ff6060", "#60e060", "#6090ff"]
        for c in range(3):
            x = r.left() + self.probe[c] / 255 * r.width()
            p.setPen(QPen(QColor(cols[c]), 1))
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
        p.setPen(QColor(theme.TEXT))
        p.drawText(QRectF(r.left(), r.top() + 12, 160, 12), Qt.AlignLeft,
                   f"R{self.probe[0]} G{self.probe[1]} B{self.probe[2]}")

    def _paint_grid_lines(self, p, r):
        p.setFont(QFont(self.font().family(), 7))
        for v, lab in [(0, "0"), (0.18 ** (1 / 2.2), "18%"), (0.5, "50"), (1.0, "100")]:
            y = r.bottom() - v * r.height()
            p.setPen(QPen(QColor("#333") if lab != "18%" else QColor("#4a3a20"), 1, Qt.DashLine if lab == "18%" else Qt.SolidLine))
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))
            p.setPen(QColor("#666"))
            p.drawText(QRectF(r.left() + 1, y - 12, 30, 12), Qt.AlignLeft, lab)

    def _paint_wave(self, p):
        r = QRectF(6, 14, self.width() - 12, self.height() - 20)
        self._paint_grid_lines(p, r)
        if self.data is None:
            return
        img = _glow_image(self.data.wave()[::-1], (150, 255, 170),
                          gain=1.4 if QSettings().value("scope/glow", True, type=bool) else 1.0)
        p.drawImage(r, img)
        if self.probe is not None:
            y = r.bottom() - (0.2126 * self.probe[0] + 0.7152 * self.probe[1] + 0.0722 * self.probe[2]) / 255 * r.height()
            p.setPen(QPen(QColor(theme.ACCENT), 1, Qt.DashLine))
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))

    def _paint_parade(self, p):
        r = QRectF(6, 14, self.width() - 12, self.height() - 20)
        self._paint_grid_lines(p, r)
        if self.data is None:
            return
        cols = [(255, 90, 90), (90, 240, 90), (100, 140, 255)]
        w3 = r.width() / 3
        for c, arr in enumerate(self.data.parade()):
            rr = QRectF(r.left() + c * w3 + 2, r.top(), w3 - 4, r.height())
            p.drawImage(rr, _glow_image(arr[::-1], cols[c]))

    def _paint_vector(self, p):
        s = min(self.width() - 12, self.height() - 20)
        r = QRectF((self.width() - s) / 2, 14 + (self.height() - 20 - s) / 2, s, s)
        c = r.center()
        p.setPen(QPen(QColor("#333"), 1))
        p.drawEllipse(c, s / 2, s / 2)
        p.drawEllipse(c, s / 4, s / 4)
        p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()))
        p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()))
        # 肌色ライン（I 軸：約 123°）
        import math
        ang = math.radians(123)
        p.setPen(QPen(QColor(theme.ACCENT), 1, Qt.DashLine))
        p.drawLine(c, QPointF(c.x() + math.cos(ang) * s / 2, c.y() - math.sin(ang) * s / 2))
        p.setFont(QFont(self.font().family(), 7))
        p.drawText(QPointF(c.x() + math.cos(ang) * s / 2 - 10, c.y() - math.sin(ang) * s / 2 - 2), "肌色")
        # 主要色のターゲット
        targets = {"R": (255, 0, 0), "Y": (255, 255, 0), "G": (0, 255, 0), "C": (0, 255, 255), "B": (0, 0, 255), "M": (255, 0, 255)}
        for name, (rr, gg, bb) in targets.items():
            cb = -0.1687 * rr - 0.3313 * gg + 0.5 * bb
            cr = 0.5 * rr - 0.4187 * gg - 0.0813 * bb
            x = c.x() + cb / 128 * 0.5 * s
            y = c.y() - cr / 128 * 0.5 * s
            p.setPen(QColor("#666"))
            p.drawRect(QRectF(x - 4, y - 4, 8, 8))
            p.drawText(QPointF(x + 6, y + 3), name)
        if self.data is None:
            return
        p.drawImage(r, _glow_image(self.data.vector(), (190, 255, 200)))

    # ---- 操作
    def _value_at(self, x):
        r = self._plot_rect()
        return min(1, max(0, (x - r.left()) / r.width()))

    def mouseMoveEvent(self, e):
        if not self.mode.startswith("hist"):
            return
        x = e.position().x()
        if self._drag:
            key, x0, acc = self._drag
            dv = (x - x0) / max(1, self._plot_rect().width())
            self._drag = (key, x, acc)
            self.zoneDrag.emit(key, dv, False)
            return
        self._hover_x = x
        v = self._value_at(x)
        lo = max(0, int(v * 255) - 6)
        hi = min(255, int(v * 255) + 6)
        self.hoverRange.emit((lo, hi))
        self.update()

    def leaveEvent(self, e):
        self._hover_x = None
        self.hoverRange.emit(None)
        self.update()

    def mousePressEvent(self, e):
        if e.button() == Qt.RightButton:
            self._menu(e.globalPosition().toPoint())
            return
        if e.button() == Qt.LeftButton and self.mode.startswith("hist"):
            v = self._value_at(e.position().x())
            for key, _, a, b in ZONES:
                if a <= v <= b:
                    self._drag = (key, e.position().x(), 0.0)
                    self.setCursor(Qt.SizeHorCursor)
                    self.update()
                    break

    def mouseReleaseEvent(self, e):
        if self._drag:
            self.zoneDrag.emit(self._drag[0], 0.0, True)
            self._drag = None
            self.unsetCursor()
            self.update()

    def _menu(self, gpos):
        m = QMenu(self)
        for k, name in MODES.items():
            a = m.addAction(name)
            a.setCheckable(True)
            a.setChecked(k == self.mode)
            a.triggered.connect(lambda _=False, kk=k: self.set_mode(kk))
        m.addSeparator()
        a = m.addAction("縦軸を対数にする")
        a.setCheckable(True)
        a.setChecked(self.log)
        a.triggered.connect(self._toggle_log)
        m.exec(gpos)

    def _toggle_log(self):
        self.log = not self.log
        self.update()

    def set_mode(self, k):
        self.mode = k
        self.modeChanged.emit(k)
        self.update()


class ScopesPanel(QWidget):
    """1・2・4 分割でスコープを並べる。右クリックで種類の切替。"""
    hoverRange = Signal(object)
    zoneDrag = Signal(str, float, bool)
    popOut = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        v = QVBoxLayout(self)
        v.setContentsMargins(4, 4, 4, 4)
        v.setSpacing(3)
        bar = QHBoxLayout()
        self.layout_combo = QComboBox()
        self.layout_combo.addItems(["1 画面", "2 画面", "4 画面"])
        self.layout_combo.setToolTip("スコープの分割数（各スコープは右クリックで種類を変更）")
        self.first_combo = QComboBox()
        for k, n in MODES.items():
            self.first_combo.addItem(n, k)
        self.info = QLabel("")
        self.info.setStyleSheet(f"color:{theme.TEXT_DIM};")
        bar.addWidget(self.first_combo)
        bar.addWidget(self.layout_combo)
        bar.addStretch(1)
        v.addLayout(bar)
        self.grid = QGridLayout()
        self.grid.setSpacing(3)
        v.addLayout(self.grid, 1)
        v.addWidget(self.info)
        st = QSettings()
        modes = st.value("scope/modes", ["hist_rgb", "waveform", "parade", "vector"])
        self.views = [ScopeView(m) for m in modes]
        for vw in self.views:
            vw.hoverRange.connect(self.hoverRange)
            vw.zoneDrag.connect(self.zoneDrag)
            vw.modeChanged.connect(self._save)
        self.first_combo.setCurrentIndex(list(MODES).index(self.views[0].mode))
        self.first_combo.currentIndexChanged.connect(lambda i: self.views[0].set_mode(self.first_combo.itemData(i)))
        self.layout_combo.currentIndexChanged.connect(self._relayout)
        self.layout_combo.setCurrentIndex(int(st.value("scope/layout", 0)))
        self._relayout()

    def _save(self, *_):
        QSettings().setValue("scope/modes", [vw.mode for vw in self.views])
        idx = list(MODES).index(self.views[0].mode)
        if self.first_combo.currentIndex() != idx:
            self.first_combo.blockSignals(True)
            self.first_combo.setCurrentIndex(idx)
            self.first_combo.blockSignals(False)

    def _relayout(self, *_):
        n = [1, 2, 4][self.layout_combo.currentIndex()]
        QSettings().setValue("scope/layout", self.layout_combo.currentIndex())
        for vw in self.views:
            self.grid.removeWidget(vw)
            vw.hide()
        pos = {1: [(0, 0)], 2: [(0, 0), (0, 1)], 4: [(0, 0), (0, 1), (1, 0), (1, 1)]}[n]
        for vw, (r, c) in zip(self.views, pos):
            self.grid.addWidget(vw, r, c)
            vw.show()

    def set_image(self, rgb8: np.ndarray | None):
        d = ScopeData(rgb8) if rgb8 is not None else None
        for vw in self.views:
            vw.data = d
            vw.update()
        return d

    def set_data(self, d):
        for vw in self.views:
            vw.data = d
            vw.update()

    def begin_ghost(self):
        for vw in self.views:
            if vw.ghost is None:
                vw.ghost = vw.data

    def end_ghost(self):
        for vw in self.views:
            vw.ghost = None
            vw.update()

    def set_zone_highlight(self, key):
        for vw in self.views:
            vw.zone_hi = key
            vw.update()

    def set_probe(self, rgb):
        for vw in self.views:
            vw.probe = rgb
            vw.update()

    def set_info(self, text):
        self.info.setText(text)
