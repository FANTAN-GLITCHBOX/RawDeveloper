"""トーンカーブ（Y / R / G / B）。

クリック：点を追加　ドラッグ：移動　右クリック：点を削除　ダブルクリック：このチャンネルをリセット
Shift＋クリック：その位置に、カーブの形を変えない点を追加
Y カーブは R・G・B すべてに掛かり、R・G・B カーブはその後で各色だけに掛かる。
"""
from __future__ import annotations

import copy

import numpy as np
from PySide6.QtCore import Qt, Signal, QPointF, QRectF
from PySide6.QtGui import QPainter, QColor, QPen, QPainterPath, QBrush
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QToolButton, QButtonGroup, QSizePolicy

from ..engine.pipeline import monotone_spline
from . import theme

CH_COLORS = {"Y": "#e6e6e6", "R": "#ff5a5a", "G": "#5ad05a", "B": "#5a8cff"}


class CurveCanvas(QWidget):
    changed = Signal(dict, bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.curves = {c: [[0.0, 0.0], [1.0, 1.0]] for c in "YRGB"}
        self.channel = "Y"
        self.hist = None
        self._drag = None
        self.setMinimumHeight(200)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setMouseTracking(True)
        self._hover = None

    def heightForWidth(self, w):
        return w

    def hasHeightForWidth(self):
        return True

    def set_curves(self, curves):
        self.curves = copy.deepcopy(curves)
        self.update()

    def set_histogram(self, hist):
        self.hist = hist
        self.update()

    def _rect(self):
        s = min(self.width(), self.height()) - 16
        return QRectF((self.width() - s) / 2, 8, s, s)

    def _to_px(self, x, y):
        r = self._rect()
        return QPointF(r.left() + x * r.width(), r.bottom() - y * r.height())

    def _from_px(self, p):
        r = self._rect()
        return (min(1, max(0, (p.x() - r.left()) / r.width())), min(1, max(0, (r.bottom() - p.y()) / r.height())))

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = self._rect()
        p.fillRect(r, QColor("#202020"))
        if self.hist is not None:
            h = self.hist
            h = h / max(h.max(), 1)
            path = QPainterPath(QPointF(r.left(), r.bottom()))
            for i, v in enumerate(h):
                path.lineTo(r.left() + i / (len(h) - 1) * r.width(), r.bottom() - v * r.height() * 0.9)
            path.lineTo(r.right(), r.bottom())
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(255, 255, 255, 28))
            p.drawPath(path)
        p.setPen(QPen(QColor("#333"), 1))
        for i in range(1, 4):
            x = r.left() + r.width() * i / 4
            y = r.top() + r.height() * i / 4
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))
        p.setPen(QPen(QColor("#444"), 1, Qt.DashLine))
        p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()))
        xs = np.linspace(0, 1, 128)
        for ch in "RGBY":
            pts = self.curves[ch]
            if ch != self.channel and len(pts) == 2 and pts == [[0.0, 0.0], [1.0, 1.0]]:
                continue
            ys = monotone_spline(pts, xs)
            path = QPainterPath(self._to_px(xs[0], ys[0]))
            for x, y in zip(xs[1:], ys[1:]):
                path.lineTo(self._to_px(x, y))
            c = QColor(CH_COLORS[ch])
            if ch != self.channel:
                c.setAlpha(90)
            p.setPen(QPen(c, 2 if ch == self.channel else 1))
            p.setBrush(Qt.NoBrush)
            p.drawPath(path)
        for i, (x, y) in enumerate(self.curves[self.channel]):
            pt = self._to_px(x, y)
            p.setPen(QPen(QColor("#fff"), 1.2))
            p.setBrush(QColor(theme.ACCENT) if self._drag == i else QColor("#333"))
            p.drawEllipse(pt, 4, 4)
        if self._hover:
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(QRectF(r.left() + 4, r.top() + 2, 120, 14), Qt.AlignLeft,
                       f"{self._hover[0] * 255:.0f} → {self._hover[1] * 255:.0f}")

    def _nearest(self, pos):
        best, bd = None, 1e9
        for i, (x, y) in enumerate(self.curves[self.channel]):
            d = (self._to_px(x, y) - pos).manhattanLength()
            if d < bd:
                best, bd = i, d
        return best if bd < 12 else None

    def _emit(self, final):
        self.changed.emit(copy.deepcopy(self.curves), final)

    def mousePressEvent(self, e):
        pos = e.position()
        pts = self.curves[self.channel]
        i = self._nearest(pos)
        if e.button() == Qt.RightButton:
            if i is not None and 0 < i < len(pts) - 1:
                pts.pop(i)
                self.update()
                self._emit(True)
            return
        if e.button() != Qt.LeftButton:
            return
        if i is None:
            x, y = self._from_px(pos)
            if e.modifiers() & Qt.ShiftModifier:
                y = float(monotone_spline(pts, np.array([x]))[0])
            pts.append([x, y])
            pts.sort(key=lambda q: q[0])
            i = [k for k, q in enumerate(pts) if q[0] == x][0]
        self._drag = i
        self.update()

    def mouseMoveEvent(self, e):
        pos = e.position()
        x, y = self._from_px(pos)
        self._hover = (x, float(monotone_spline(self.curves[self.channel], np.array([x]))[0]))
        if self._drag is not None:
            pts = self.curves[self.channel]
            i = self._drag
            lo = pts[i - 1][0] + 0.01 if i > 0 else 0.0
            hi = pts[i + 1][0] - 0.01 if i < len(pts) - 1 else 1.0
            if i == 0:
                hi = min(hi, 0.99)
            if i == len(pts) - 1:
                lo = max(lo, 0.01)
            pts[i] = [round(min(hi, max(lo, x)), 4), round(y, 4)]
            self._emit(False)
        self.update()

    def mouseReleaseEvent(self, e):
        if self._drag is not None:
            self._drag = None
            self._emit(True)
            self.update()

    def mouseDoubleClickEvent(self, e):
        self.curves[self.channel] = [[0.0, 0.0], [1.0, 1.0]]
        self._emit(True)
        self.update()

    def leaveEvent(self, e):
        self._hover = None
        self.update()


class CurveEditor(QWidget):
    changed = Signal(dict, bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        v = QVBoxLayout(self)
        v.setContentsMargins(8, 0, 8, 0)
        top = QHBoxLayout()
        self.group = QButtonGroup(self)
        for ch in "YRGB":
            b = QToolButton()
            b.setText(ch)
            b.setCheckable(True)
            b.setFixedWidth(28)
            b.setStyleSheet(f"color:{CH_COLORS[ch]}; font-weight:600;")
            self.group.addButton(b)
            top.addWidget(b)
            b.clicked.connect(lambda _=False, c=ch: self.set_channel(c))
            if ch == "Y":
                b.setChecked(True)
        top.addStretch(1)
        v.addLayout(top)
        self.canvas = CurveCanvas()
        self.canvas.changed.connect(self.changed)
        v.addWidget(self.canvas)

    def set_channel(self, ch):
        self.canvas.channel = ch
        self.canvas.update()

    def set_curves(self, c):
        self.canvas.set_curves(c)

    def set_histogram(self, h):
        self.canvas.set_histogram(h)
