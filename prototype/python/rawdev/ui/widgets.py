"""DaVinci Resolve 風の操作部品。

共通操作：
  ・数値の上で左右ドラッグ … 値が増減（画面端でカーソルが反対側に回り込む）
  ・Ctrl＋ドラッグ … 1/10 の微調整、Shift＋ドラッグ … 10 倍の粗調整
  ・ダブルクリック … 初期値に戻す
  ・クリック … 数値を直接入力（「+=0.3」で今の値に足す相対入力）
  ・マウスホイール … 少しずつ変更（設定でオフ可）
"""
from __future__ import annotations

import math

from PySide6.QtCore import Qt, Signal, QPoint, QPointF, QRectF, QSettings
from PySide6.QtGui import (QPainter, QColor, QPen, QLinearGradient, QCursor, QBrush,
                           QConicalGradient, QRadialGradient, QFontMetrics)
from PySide6.QtWidgets import (QWidget, QLineEdit, QHBoxLayout, QVBoxLayout, QLabel, QToolButton,
                               QFrame, QSizePolicy, QApplication)

from . import theme


def wheel_enabled() -> bool:
    return QSettings().value("ui/wheel_adjust", True, type=bool)


def _modifier_factor(mods) -> float:
    if mods & Qt.ControlModifier:
        return 0.1
    if mods & Qt.ShiftModifier:
        return 10.0
    return 1.0


class _WrapDrag:
    """ドラッグ中にカーソルが画面端に来たら反対側へ回り込ませる。"""

    def __init__(self):
        self.last = None

    def start(self, gpos: QPoint):
        self.last = gpos

    def delta(self, gpos: QPoint) -> int:
        if self.last is None:
            self.last = gpos
            return 0
        dx = gpos.x() - self.last.x()
        self.last = gpos
        scr = QApplication.screenAt(gpos)
        if scr is not None:
            g = scr.geometry()
            if gpos.x() <= g.left() + 2:
                np_ = QPoint(g.right() - 4, gpos.y())
                QCursor.setPos(np_)
                self.last = np_
            elif gpos.x() >= g.right() - 2:
                np_ = QPoint(g.left() + 4, gpos.y())
                QCursor.setPos(np_)
                self.last = np_
        return dx


class ScrubField(QWidget):
    """ドラッグで値を変える数値欄。valueChanged(value, final)"""
    valueChanged = Signal(float, bool)

    def __init__(self, value=0.0, lo=-100.0, hi=100.0, default=0.0, decimals=0, step=1.0,
                 suffix="", width=52, parent=None, signed=True):
        super().__init__(parent)
        self.lo, self.hi, self.default = lo, hi, default
        self.decimals, self.step, self.suffix = decimals, step, suffix
        self.signed = signed
        self._v = float(value)
        self._drag = _WrapDrag()
        self._pressed = False
        self._moved = 0
        self._acc = 0.0
        self.setFixedSize(width, 20)
        self.setCursor(Qt.SizeHorCursor)
        self.setFocusPolicy(Qt.ClickFocus)
        self._edit = None
        self.setToolTip("ドラッグで調整（Ctrl：微調整／Shift：粗調整）、ダブルクリックでリセット\nクリックで入力（「+=0.3」で今の値に足す）")

    def value(self):
        return self._v

    def setValue(self, v, emit=False, final=True):
        v = float(min(self.hi, max(self.lo, v)))
        if self.decimals == 0:
            v = round(v)
        else:
            v = round(v, self.decimals)
        changed = v != self._v
        self._v = v
        self.update()
        if emit and (changed or final):
            self.valueChanged.emit(v, final)

    def text(self):
        v = self._v
        s = f"{v:.{self.decimals}f}"
        if self.signed and v > 0 and self.lo < 0:
            s = "+" + s
        return s + self.suffix

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = self.rect().adjusted(0, 1, -1, -2)
        active = self._pressed or self.underMouse()
        p.setPen(QPen(QColor(theme.ACCENT if self._pressed else ("#555" if active else "#3a3a3a")), 1))
        p.setBrush(QColor("#2b2b2b"))
        p.drawRoundedRect(r, 3, 3)
        changed = abs(self._v - self.default) > 1e-9
        p.setPen(QColor(theme.TEXT if changed else theme.TEXT_DIM))
        p.drawText(r, Qt.AlignCenter, self.text())

    def enterEvent(self, e):
        self.update()

    def leaveEvent(self, e):
        self.update()

    def mousePressEvent(self, e):
        if e.button() == Qt.LeftButton:
            self._pressed = True
            self._moved = 0
            self._acc = 0.0
            self._drag.start(e.globalPosition().toPoint())
            self.update()

    def mouseMoveEvent(self, e):
        if not self._pressed:
            return
        dx = self._drag.delta(e.globalPosition().toPoint())
        self._moved += abs(dx)
        if self._moved < 3:
            return
        self._acc += dx * self.step * _modifier_factor(e.modifiers())
        q = 10 ** (-self.decimals)
        n = math.trunc(self._acc / q) * q
        if n:
            self._acc -= n
            self.setValue(self._v + n, emit=True, final=False)

    def mouseReleaseEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        self._pressed = False
        if self._moved < 3:
            self._start_edit()
        else:
            self.valueChanged.emit(self._v, True)
        self.update()

    def mouseDoubleClickEvent(self, e):
        if self._edit:
            self._edit.deleteLater()
            self._edit = None
        self.setValue(self.default, emit=True, final=True)

    def wheelEvent(self, e):
        if not wheel_enabled():
            e.ignore()
            return
        d = 1 if e.angleDelta().y() > 0 else -1
        self.setValue(self._v + d * self.step * 5 * _modifier_factor(e.modifiers()), emit=True, final=True)

    def _start_edit(self):
        if self._edit:
            return
        ed = QLineEdit(self)
        ed.setGeometry(self.rect())
        ed.setAlignment(Qt.AlignCenter)
        ed.setText(f"{self._v:.{self.decimals}f}")
        ed.selectAll()
        ed.show()
        ed.setFocus()
        self._edit = ed

        def done():
            if self._edit is None:
                return
            t = ed.text().strip().replace("＋", "+").replace("－", "-").replace("＝", "=")
            try:
                if t.startswith("+=") or t.startswith("-="):
                    v = self._v + float(t[0] + t[2:])   # 相対入力：+=0.3 / -=10
                else:
                    v = float(t)
                self.setValue(v, emit=True, final=True)
            except ValueError:
                pass
            self._edit = None
            ed.deleteLater()

        ed.editingFinished.connect(done)


class SliderBar(QWidget):
    """スライダー。gradient=[(pos, color)] で背景に色の帯（色温度・色かぶり用）。"""
    valueChanged = Signal(float, bool)
    hovered = Signal(bool)

    def __init__(self, lo=-100.0, hi=100.0, default=0.0, gradient=None, parent=None):
        super().__init__(parent)
        self.lo, self.hi, self.default = lo, hi, default
        self._v = default
        self.gradient = gradient
        self._pressed = False
        self.setMinimumWidth(80)
        self.setFixedHeight(18)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self._grab_off = 0.0
        self._fine_anchor = None

    def setValue(self, v):
        self._v = min(self.hi, max(self.lo, v))
        self.update()

    def value(self):
        return self._v

    def _pos(self, v):
        w = self.width() - 12
        return 6 + (v - self.lo) / (self.hi - self.lo) * w

    def _val(self, x):
        w = self.width() - 12
        return self.lo + (x - 6) / max(1, w) * (self.hi - self.lo)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        y = self.height() / 2
        track = QRectF(6, y - 2, self.width() - 12, 4)
        if self.gradient:
            g = QLinearGradient(track.left(), 0, track.right(), 0)
            for pos, c in self.gradient:
                g.setColorAt(pos, QColor(c))
            p.setBrush(QBrush(g))
            p.setPen(Qt.NoPen)
            p.drawRoundedRect(track.adjusted(0, -1, 0, 1), 2, 2)
        else:
            p.setBrush(QColor("#3c3c3c"))
            p.setPen(Qt.NoPen)
            p.drawRoundedRect(track, 2, 2)
            # 初期値からの差分を塗る
            x0, x1 = self._pos(self.default), self._pos(self._v)
            if abs(x1 - x0) > 0.5:
                p.setBrush(QColor(theme.ACCENT if self._pressed else "#7a7a7a"))
                p.drawRect(QRectF(min(x0, x1), y - 2, abs(x1 - x0), 4))
        x = self._pos(self._v)
        p.setBrush(QColor("#e8e8e8" if (self._pressed or self.underMouse()) else "#b8b8b8"))
        p.setPen(QPen(QColor("#1a1a1a"), 1))
        p.drawEllipse(QPointF(x, y), 5.5, 5.5)

    def enterEvent(self, e):
        self.hovered.emit(True)
        self.update()

    def leaveEvent(self, e):
        self.hovered.emit(False)
        self.update()

    def mousePressEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        self._pressed = True
        x = e.position().x()
        hx = self._pos(self._v)
        if abs(x - hx) <= 7:
            self._grab_off = x - hx
        else:
            self._grab_off = 0
            self._v = min(self.hi, max(self.lo, self._val(x)))
            self.valueChanged.emit(self._v, False)
        self._fine_anchor = (x, self._v)
        self.update()

    def mouseMoveEvent(self, e):
        if not self._pressed:
            return
        x = e.position().x()
        f = _modifier_factor(e.modifiers())
        if f != 1.0:
            ax, av = self._fine_anchor
            span = (self.hi - self.lo) / max(1, self.width() - 12)
            v = av + (x - ax) * span * f
        else:
            v = self._val(x - self._grab_off)
            self._fine_anchor = (x, v)
        self._v = min(self.hi, max(self.lo, v))
        if f != 1.0:
            self._fine_anchor = (x, self._v)
        self.valueChanged.emit(self._v, False)
        self.update()

    def mouseReleaseEvent(self, e):
        if self._pressed:
            self._pressed = False
            self.valueChanged.emit(self._v, True)
            self.update()

    def mouseDoubleClickEvent(self, e):
        self._v = self.default
        self.valueChanged.emit(self._v, True)
        self.update()

    def wheelEvent(self, e):
        if not wheel_enabled():
            e.ignore()
            return
        d = 1 if e.angleDelta().y() > 0 else -1
        step = (self.hi - self.lo) / 100.0 * _modifier_factor(e.modifiers())
        self._v = min(self.hi, max(self.lo, self._v + d * step))
        self.valueChanged.emit(self._v, True)
        self.update()


class ParamRow(QWidget):
    """名前＋スライダー＋数値欄。to_slider/from_slider で非線形な対応（色温度など）。"""
    changed = Signal(str, float, bool)
    hovered = Signal(str, bool)

    def __init__(self, key, label, lo, hi, default=0.0, decimals=0, step=1.0, suffix="",
                 gradient=None, to_slider=None, from_slider=None, scrub_width=56, parent=None):
        super().__init__(parent)
        self.key = key
        self.to_slider = to_slider
        self.from_slider = from_slider
        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 1, 8, 1)
        lay.setSpacing(6)
        self.label = QLabel(label)
        self.label.setFixedWidth(78)
        self.label.setStyleSheet(f"color:{theme.TEXT_DIM}; background: transparent;")
        if to_slider:
            self.slider = SliderBar(0.0, 1.0, to_slider(default), gradient)
        else:
            self.slider = SliderBar(lo, hi, default, gradient)
        self.field = ScrubField(default, lo, hi, default, decimals, step, suffix, scrub_width)
        lay.addWidget(self.label)
        lay.addWidget(self.slider, 1)
        lay.addWidget(self.field)
        self.slider.valueChanged.connect(self._from_slider)
        self.field.valueChanged.connect(self._from_field)
        self.slider.hovered.connect(lambda h: self.hovered.emit(self.key, h))
        self.label.mouseDoubleClickEvent = lambda e: self._reset()
        self.setAttribute(Qt.WA_StyledBackground, False)

    def _reset(self):
        self.field.setValue(self.field.default, emit=True, final=True)

    def _from_slider(self, v, final):
        val = self.from_slider(v) if self.from_slider else v
        self.field.setValue(val)
        self.changed.emit(self.key, self.field.value(), final)

    def _from_field(self, v, final):
        self.slider.setValue(self.to_slider(v) if self.to_slider else v)
        self.changed.emit(self.key, v, final)

    def setValue(self, v):
        self.field.setValue(v)
        self.slider.setValue(self.to_slider(v) if self.to_slider else v)

    def value(self):
        return self.field.value()

    def enterEvent(self, e):
        self.hovered.emit(self.key, True)

    def leaveEvent(self, e):
        self.hovered.emit(self.key, False)


class Section(QFrame):
    """折りたたみできるパネル。右端のボタンでこのパネルだけリセット。"""
    resetRequested = Signal()

    def __init__(self, title, parent=None, collapsed=False):
        super().__init__(parent)
        self.setObjectName("Section")
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 6)
        v.setSpacing(2)
        head = QWidget()
        head.setObjectName("SectionHead")
        head.setStyleSheet(f"QWidget#SectionHead{{background:{theme.PANEL2};}}")
        h = QHBoxLayout(head)
        h.setContentsMargins(6, 3, 4, 3)
        self.toggle = QToolButton()
        self.toggle.setArrowType(Qt.DownArrow)
        self.toggle.setCheckable(True)
        self.toggle.setChecked(not collapsed)
        self.toggle.setStyleSheet("border:none;")
        t = QLabel(title)
        t.setObjectName("SectionTitle")
        t.setStyleSheet("background: transparent; font-weight:600;")
        self.reset_btn = QToolButton()
        self.reset_btn.setText("↺")
        self.reset_btn.setToolTip("このパネルをリセット")
        self.reset_btn.clicked.connect(self.resetRequested)
        h.addWidget(self.toggle)
        h.addWidget(t, 1)
        self.extra = QHBoxLayout()
        self.extra.setSpacing(2)
        h.addLayout(self.extra)
        h.addWidget(self.reset_btn)
        v.addWidget(head)
        self.body = QWidget()
        self.body.setStyleSheet(f"background:{theme.PANEL};")
        self.body_layout = QVBoxLayout(self.body)
        self.body_layout.setContentsMargins(0, 4, 0, 2)
        self.body_layout.setSpacing(1)
        v.addWidget(self.body)
        self.toggle.toggled.connect(self._toggled)
        head.mouseReleaseEvent = lambda e: self.toggle.toggle()
        self._toggled(self.toggle.isChecked())

    def _toggled(self, on):
        self.body.setVisible(on)
        self.toggle.setArrowType(Qt.DownArrow if on else Qt.RightArrow)

    def add(self, w):
        self.body_layout.addWidget(w)
        return w


class ColorWheel(QWidget):
    """カラーホイール（リフト／ガンマ／ゲイン／オフセット）。

    中の点をドラッグ … 色味、下のバーを左右ドラッグ … 明るさ（マスター）
    ダブルクリック … リセット、Ctrl … 微調整
    changed([u, v, master], final)
    """
    changed = Signal(list, bool)

    def __init__(self, title, parent=None):
        super().__init__(parent)
        self.title = title
        self.vals = [0.0, 0.0, 0.0]
        self.setMinimumSize(64, 118)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self._mode = None
        self._drag = _WrapDrag()
        self._last = None
        self.setToolTip("円の中をドラッグ：色味　下のバーを左右ドラッグ：明るさ\nダブルクリック：リセット　Ctrl：微調整")

    def setValues(self, vals):
        self.vals = [float(x) for x in vals]
        self.update()

    def _geom(self):
        w = self.width()
        d = max(40, min(w - 8, 80))
        cx = w / 2
        cy = 16 + d / 2
        return cx, cy, d / 2

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        cx, cy, r = self._geom()
        p.setPen(QColor(theme.TEXT_DIM))
        p.drawText(QRectF(0, 0, self.width(), 14), Qt.AlignCenter, self.title)
        # 色相環
        cg = QConicalGradient(QPointF(cx, cy), 0)
        for i, c in enumerate(["#ff4040", "#ff40ff", "#4040ff", "#40ffff", "#40ff40", "#ffff40", "#ff4040"]):
            cg.setColorAt(i / 6, QColor(c))
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(cg))
        p.drawEllipse(QPointF(cx, cy), r, r)
        rg = QRadialGradient(QPointF(cx, cy), r)
        rg.setColorAt(0, QColor(40, 40, 40, 255))
        rg.setColorAt(0.85, QColor(40, 40, 40, 200))
        rg.setColorAt(1, QColor(40, 40, 40, 60))
        p.setBrush(QBrush(rg))
        p.drawEllipse(QPointF(cx, cy), r - 3, r - 3)
        p.setPen(QPen(QColor("#555"), 1))
        p.drawLine(QPointF(cx - 5, cy), QPointF(cx + 5, cy))
        p.drawLine(QPointF(cx, cy - 5), QPointF(cx, cy + 5))
        # 点
        u, v, m = self.vals
        px, py = cx + u * (r - 6), cy - v * (r - 6)
        p.setPen(QPen(QColor("#fff"), 1.5))
        p.setBrush(QColor(theme.ACCENT) if (u or v) else Qt.NoBrush)
        p.drawEllipse(QPointF(px, py), 4, 4)
        # マスターバー
        by = cy + r + 8
        bar = QRectF(cx - r, by, 2 * r, 5)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor("#3c3c3c"))
        p.drawRoundedRect(bar, 2, 2)
        mx = cx + max(-1, min(1, m)) * r
        p.setBrush(QColor(theme.ACCENT if m else "#999"))
        p.drawRect(QRectF(min(cx, mx), by, abs(mx - cx), 5))
        p.setPen(QColor(theme.TEXT if any(self.vals) else theme.TEXT_DIM))
        p.drawText(QRectF(0, by + 6, self.width(), 14), Qt.AlignCenter, f"{m:+.2f}")

    def mousePressEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        cx, cy, r = self._geom()
        pos = e.position()
        self._mode = "bar" if pos.y() > cy + r + 2 else "point"
        self._last = pos
        self._drag.start(e.globalPosition().toPoint())

    def mouseMoveEvent(self, e):
        if not self._mode:
            return
        cx, cy, r = self._geom()
        f = _modifier_factor(e.modifiers())
        f = 0.15 if f < 1 else (1.0 if f == 1 else 1.0)
        if self._mode == "point":
            pos = e.position()
            dx = (pos.x() - self._last.x()) / (r - 6) * f
            dy = -(pos.y() - self._last.y()) / (r - 6) * f
            self._last = pos
            u, v = self.vals[0] + dx, self.vals[1] + dy
            n = math.hypot(u, v)
            if n > 1:
                u, v = u / n, v / n
            self.vals[0], self.vals[1] = u, v
        else:
            dx = self._drag.delta(e.globalPosition().toPoint())
            self.vals[2] = max(-1.0, min(1.0, self.vals[2] + dx / 200.0 * f))
        self.update()
        self.changed.emit(list(self.vals), False)

    def mouseReleaseEvent(self, e):
        if self._mode:
            self._mode = None
            self.changed.emit(list(self.vals), True)

    def mouseDoubleClickEvent(self, e):
        cx, cy, r = self._geom()
        if e.position().y() > cy + r + 2:
            self.vals[2] = 0.0
        else:
            self.vals[0] = self.vals[1] = 0.0
        self.update()
        self.changed.emit(list(self.vals), True)

    def wheelEvent(self, e):
        if not wheel_enabled():
            e.ignore()
            return
        d = 1 if e.angleDelta().y() > 0 else -1
        self.vals[2] = max(-1.0, min(1.0, self.vals[2] + d * 0.02 * _modifier_factor(e.modifiers())))
        self.update()
        self.changed.emit(list(self.vals), True)
