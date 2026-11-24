"""写真ビューアー。

ズーム：ホイール（カーソル位置中心）、Z で全体／等倍の切替、ダブルクリックでも切替
パン：中ボタンドラッグ、Space＋ドラッグ、ズーム中の左ドラッグ
比較：ワイプ表示（線をドラッグして 補正前｜補正後 を比較）
重ね表示：白飛び・黒つぶれ警告、ヒストグラムでなぞった範囲の画素
切り抜きツール：枠のドラッグ、比率固定、三分割ガイド
"""
from __future__ import annotations

import numpy as np
from PySide6.QtCore import Qt, Signal, QRectF, QPointF, QSizeF
from PySide6.QtGui import QPainter, QColor, QPen, QImage, QFont, QCursor
from PySide6.QtWidgets import QWidget

from . import theme


def to_qimage(rgb8: np.ndarray) -> QImage:
    h, w = rgb8.shape[:2]
    a = np.ascontiguousarray(rgb8)
    return QImage(a.data, w, h, w * 3, QImage.Format_RGB888).copy()


class Viewer(QWidget):
    probe = Signal(object)              # (r,g,b) / None
    zoomChanged = Signal(float, bool)   # (zoom, is_fit)
    wantHiRes = Signal()
    cropChanged = Signal(dict, bool)
    splitMoved = Signal(float)
    clicked = Signal(int, int)          # 表示配列上の画素位置（スポイト用）

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.StrongFocus)
        self.img: QImage | None = None
        self.arr: np.ndarray | None = None       # 表示中の uint8 配列（値の読み取り用）
        self.before: QImage | None = None
        self.full_size = (1, 1)                  # 元画像（切り抜き後）の画素数
        self.zoom = 1.0
        self.fit = True
        self.offset = QPointF(0, 0)              # 画像中心からのパン量（画面 px）
        self.split = None                        # None | "v" | "h"
        self.split_pos = 0.5
        self.overlay_clip = False
        self.mask: QImage | None = None
        self.message = ""
        self.info = ""
        # 切り抜き
        self.crop_mode = False
        self.crop = {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0}
        self.crop_aspect = None                  # None=自由, float=幅/高さ
        self._crop_drag = None
        self._pan = None
        self._space = False
        self._split_drag = False
        self._clip_cache = None
        self.bypass = False                      # True なら補正前を全面に表示（Shift+D）
        self.picking = False                     # WB スポイト

    # ---------------------------------------------------------------- data
    def set_image(self, rgb8: np.ndarray | None, full_size=None, keep_view=True):
        self.arr = rgb8
        self.img = to_qimage(rgb8) if rgb8 is not None else None
        if full_size:
            changed = tuple(full_size) != tuple(self.full_size)
            self.full_size = tuple(full_size)
            if changed and not keep_view:
                self.fit = True
        self._clip_cache = None
        if self.fit:
            self.zoom = self._fit_zoom()
        self.update()

    def set_before(self, rgb8):
        self.before = to_qimage(rgb8) if rgb8 is not None else None
        self.update()

    def set_mask(self, rng):
        """ヒストグラムでなぞった明るさの範囲を色付き表示。"""
        if rng is None or self.arr is None:
            self.mask = None
        else:
            lo, hi = rng
            a = self.arr
            luma = (a[..., 0].astype(np.int32) * 54 + a[..., 1].astype(np.int32) * 183 + a[..., 2].astype(np.int32) * 19) >> 8
            sel = (luma >= lo) & (luma <= hi)
            h, w = sel.shape
            m = np.zeros((h, w, 4), np.uint8)
            m[sel] = (210, 139, 44, 170)
            self.mask = QImage(m.data, w, h, w * 4, QImage.Format_RGBA8888).copy()
        self.update()

    def _clip_overlay(self):
        if self._clip_cache is None and self.arr is not None:
            a = self.arr
            hi = (a >= 254).any(axis=2)
            lo = (a <= 1).all(axis=2)
            h, w = hi.shape
            m = np.zeros((h, w, 4), np.uint8)
            m[hi] = (255, 40, 40, 230)
            m[lo] = (40, 90, 255, 230)
            self._clip_cache = QImage(m.data, w, h, w * 4, QImage.Format_RGBA8888).copy()
        return self._clip_cache

    # ---------------------------------------------------------------- geometry
    def _fit_zoom(self):
        fw, fh = self.full_size
        if fw <= 1 or fh <= 1:
            return 1.0
        return min((self.width() - 20) / fw, (self.height() - 20) / fh)

    def image_rect(self) -> QRectF:
        fw, fh = self.full_size
        w, h = fw * self.zoom, fh * self.zoom
        cx = self.width() / 2 + self.offset.x()
        cy = self.height() / 2 + self.offset.y()
        return QRectF(cx - w / 2, cy - h / 2, w, h)

    def set_zoom(self, z, anchor: QPointF | None = None, fit=False):
        old = self.image_rect()
        fz = self._fit_zoom()
        self.fit = fit
        z = fz if fit else max(fz * 0.5, min(8.0, z))
        if anchor is None:
            anchor = QPointF(self.width() / 2, self.height() / 2)
        if fit:
            self.offset = QPointF(0, 0)
        else:
            rx = (anchor.x() - old.left()) / max(old.width(), 1)
            ry = (anchor.y() - old.top()) / max(old.height(), 1)
            fw, fh = self.full_size
            nw, nh = fw * z, fh * z
            left = anchor.x() - rx * nw
            top = anchor.y() - ry * nh
            self.offset = QPointF(left + nw / 2 - self.width() / 2, top + nh / 2 - self.height() / 2)
        self.zoom = z
        self._clamp()
        self.zoomChanged.emit(self.zoom, self.fit)
        if self.img is not None and self.zoom > self.img.width() / max(self.full_size[0], 1) * 1.05:
            self.wantHiRes.emit()
        self.update()

    def toggle_zoom(self, anchor=None):
        if self.fit:
            self.set_zoom(1.0, anchor)
        else:
            self.set_zoom(0, fit=True)

    def _clamp(self):
        r = self.image_rect()
        dx = dy = 0
        if r.width() <= self.width():
            self.offset.setX(0)
        else:
            if r.left() > 0:
                dx = -r.left()
            elif r.right() < self.width():
                dx = self.width() - r.right()
        if r.height() <= self.height():
            self.offset.setY(0)
        else:
            if r.top() > 0:
                dy = -r.top()
            elif r.bottom() < self.height():
                dy = self.height() - r.bottom()
        self.offset += QPointF(dx, dy)

    def resizeEvent(self, e):
        if self.fit:
            self.zoom = self._fit_zoom()
        self._clamp()

    # ---------------------------------------------------------------- paint
    def paintEvent(self, e):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor("#191919"))
        if self.img is None:
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(self.rect(), Qt.AlignCenter, self.message or "写真がありません")
            return
        p.setRenderHint(QPainter.SmoothPixmapTransform, self.zoom < 2.0)
        r = self.image_rect()
        if self.bypass and self.before is not None and not self.crop_mode:
            p.drawImage(r, self.before)
            p.setPen(QColor(theme.ACCENT))
            p.drawText(QRectF(8, 6, 200, 16), Qt.AlignLeft, "補正前を表示中（Shift+D で戻る）")
        elif self.split and self.before is not None and not self.crop_mode:
            p.drawImage(r, self.img)
            p.save()
            if self.split == "v":
                sx = r.left() + r.width() * self.split_pos
                p.setClipRect(QRectF(r.left(), r.top(), sx - r.left(), r.height()))
            else:
                sy = r.top() + r.height() * self.split_pos
                p.setClipRect(QRectF(r.left(), r.top(), r.width(), sy - r.top()))
            p.drawImage(r, self.before)
            p.restore()
            p.setPen(QPen(QColor("#ffffff"), 1.5))
            f = QFont(self.font().family(), 8)
            p.setFont(f)
            if self.split == "v":
                p.drawLine(QPointF(sx, r.top()), QPointF(sx, r.bottom()))
                p.drawText(QPointF(sx - 60, r.top() + 16), "補正前")
                p.drawText(QPointF(sx + 8, r.top() + 16), "補正後")
                p.setBrush(QColor("#fff"))
                p.drawEllipse(QPointF(sx, r.center().y()), 6, 6)
            else:
                p.drawLine(QPointF(r.left(), sy), QPointF(r.right(), sy))
                p.drawText(QPointF(r.left() + 8, sy - 6), "補正前")
                p.drawText(QPointF(r.left() + 8, sy + 16), "補正後")
                p.setBrush(QColor("#fff"))
                p.drawEllipse(QPointF(r.center().x(), sy), 6, 6)
        else:
            p.drawImage(r, self.img)
        if self.overlay_clip and not self.crop_mode:
            ov = self._clip_overlay()
            if ov is not None:
                p.drawImage(r, ov)
        if self.mask is not None and not self.crop_mode:
            p.drawImage(r, self.mask)
        if self.crop_mode:
            self._paint_crop(p, r)
        # 情報
        p.setPen(QColor(theme.TEXT_DIM))
        p.setFont(QFont(self.font().family(), 8))
        z = "全体" if self.fit else f"{self.zoom * 100:.0f}%"
        p.drawText(QRectF(8, self.height() - 20, self.width() - 16, 16), Qt.AlignLeft | Qt.AlignVCenter,
                   f"{z}   {self.info}")
        if self.message:
            p.setPen(QColor(theme.ACCENT))
            p.drawText(QRectF(8, 6, self.width() - 16, 16), Qt.AlignRight, self.message)

    def _crop_rect_px(self, r):
        c = self.crop
        return QRectF(r.left() + c["x"] * r.width(), r.top() + c["y"] * r.height(), c["w"] * r.width(), c["h"] * r.height())

    def _paint_crop(self, p, r):
        cr = self._crop_rect_px(r)
        shade = QColor(0, 0, 0, 150)
        p.fillRect(QRectF(r.left(), r.top(), r.width(), cr.top() - r.top()), shade)
        p.fillRect(QRectF(r.left(), cr.bottom(), r.width(), r.bottom() - cr.bottom()), shade)
        p.fillRect(QRectF(r.left(), cr.top(), cr.left() - r.left(), cr.height()), shade)
        p.fillRect(QRectF(cr.right(), cr.top(), r.right() - cr.right(), cr.height()), shade)
        p.setPen(QPen(QColor(255, 255, 255, 120), 1))
        for i in (1, 2):
            x = cr.left() + cr.width() * i / 3
            y = cr.top() + cr.height() * i / 3
            p.drawLine(QPointF(x, cr.top()), QPointF(x, cr.bottom()))
            p.drawLine(QPointF(cr.left(), y), QPointF(cr.right(), y))
        p.setPen(QPen(QColor("#ffffff"), 1.5))
        p.setBrush(Qt.NoBrush)
        p.drawRect(cr)
        p.setBrush(QColor("#ffffff"))
        for pt in self._handles(cr).values():
            p.drawRect(QRectF(pt.x() - 4, pt.y() - 4, 8, 8))
        p.setPen(QColor(theme.ACCENT))
        fw, fh = self.full_size
        p.drawText(QPointF(cr.left() + 4, cr.top() - 6),
                   f"{round(self.crop['w'] * fw)} × {round(self.crop['h'] * fh)}   Enter：決定  Esc：取消")

    @staticmethod
    def _handles(cr):
        return {
            "tl": cr.topLeft(), "tr": cr.topRight(), "bl": cr.bottomLeft(), "br": cr.bottomRight(),
            "t": QPointF(cr.center().x(), cr.top()), "b": QPointF(cr.center().x(), cr.bottom()),
            "l": QPointF(cr.left(), cr.center().y()), "r": QPointF(cr.right(), cr.center().y()),
        }

    # ---------------------------------------------------------------- input
    def wheelEvent(self, e):
        if self.img is None:
            return
        f = 1.15 if e.angleDelta().y() > 0 else 1 / 1.15
        self.set_zoom(self.zoom * f, e.position())

    def mouseDoubleClickEvent(self, e):
        if not self.crop_mode:
            self.toggle_zoom(e.position())

    def keyPressEvent(self, e):
        if e.key() == Qt.Key_Space and not e.isAutoRepeat():
            self._space = True
            self.setCursor(Qt.OpenHandCursor)
        elif self.crop_mode and e.key() in (Qt.Key_Return, Qt.Key_Enter):
            self.cropChanged.emit(dict(self.crop), True)
        else:
            super().keyPressEvent(e)

    def keyReleaseEvent(self, e):
        if e.key() == Qt.Key_Space and not e.isAutoRepeat():
            self._space = False
            self.unsetCursor()
        else:
            super().keyReleaseEvent(e)

    def mousePressEvent(self, e):
        pos = e.position()
        r = self.image_rect()
        if e.button() == Qt.MiddleButton or (e.button() == Qt.LeftButton and self._space):
            self._pan = (pos, QPointF(self.offset))
            self.setCursor(Qt.ClosedHandCursor)
            return
        if e.button() != Qt.LeftButton:
            return
        if self.picking and self.arr is not None and r.contains(pos):
            h, w = self.arr.shape[:2]
            self.clicked.emit(int((pos.x() - r.left()) / r.width() * w), int((pos.y() - r.top()) / r.height() * h))
            return
        if self.crop_mode:
            cr = self._crop_rect_px(r)
            for k, pt in self._handles(cr).items():
                if (pt - pos).manhattanLength() < 12:
                    self._crop_drag = (k, pos, dict(self.crop))
                    return
            if cr.contains(pos):
                self._crop_drag = ("move", pos, dict(self.crop))
            return
        if self.split and self.before is not None:
            if self.split == "v":
                sx = r.left() + r.width() * self.split_pos
                near = abs(pos.x() - sx) < 10
            else:
                sy = r.top() + r.height() * self.split_pos
                near = abs(pos.y() - sy) < 10
            if near:
                self._split_drag = True
                return
        if not self.fit:
            self._pan = (pos, QPointF(self.offset))
            self.setCursor(Qt.ClosedHandCursor)

    def mouseMoveEvent(self, e):
        pos = e.position()
        r = self.image_rect()
        if self._pan:
            p0, o0 = self._pan
            self.offset = o0 + (pos - p0)
            self._clamp()
            self.update()
            return
        if self._split_drag:
            if self.split == "v":
                self.split_pos = min(1, max(0, (pos.x() - r.left()) / max(r.width(), 1)))
            else:
                self.split_pos = min(1, max(0, (pos.y() - r.top()) / max(r.height(), 1)))
            self.splitMoved.emit(self.split_pos)
            self.update()
            return
        if self._crop_drag:
            self._drag_crop(pos, r)
            return
        if self.crop_mode:
            cr = self._crop_rect_px(r)
            cur = Qt.ArrowCursor
            for k, pt in self._handles(cr).items():
                if (pt - pos).manhattanLength() < 12:
                    cur = {"tl": Qt.SizeFDiagCursor, "br": Qt.SizeFDiagCursor, "tr": Qt.SizeBDiagCursor,
                           "bl": Qt.SizeBDiagCursor, "t": Qt.SizeVerCursor, "b": Qt.SizeVerCursor,
                           "l": Qt.SizeHorCursor, "r": Qt.SizeHorCursor}[k]
                    break
            else:
                if cr.contains(pos):
                    cur = Qt.SizeAllCursor
            self.setCursor(cur)
            return
        # 画素の値
        if self.arr is not None and r.contains(pos):
            h, w = self.arr.shape[:2]
            x = int((pos.x() - r.left()) / r.width() * w)
            y = int((pos.y() - r.top()) / r.height() * h)
            if 0 <= x < w and 0 <= y < h:
                self.probe.emit(tuple(int(v) for v in self.arr[y, x]))
                return
        self.probe.emit(None)

    def leaveEvent(self, e):
        self.probe.emit(None)

    def mouseReleaseEvent(self, e):
        if self._pan:
            self._pan = None
            self.setCursor(Qt.OpenHandCursor if self._space else Qt.ArrowCursor)
        if self._split_drag:
            self._split_drag = False
        if self._crop_drag:
            self._crop_drag = None
            self.cropChanged.emit(dict(self.crop), False)

    def _drag_crop(self, pos, r):
        k, p0, c0 = self._crop_drag
        dx = (pos.x() - p0.x()) / r.width()
        dy = (pos.y() - p0.y()) / r.height()
        x0, y0, x1, y1 = c0["x"], c0["y"], c0["x"] + c0["w"], c0["y"] + c0["h"]
        if k == "move":
            w, h = c0["w"], c0["h"]
            nx = min(1 - w, max(0, x0 + dx))
            ny = min(1 - h, max(0, y0 + dy))
            self.crop.update(x=nx, y=ny)
            self.update()
            return
        if "l" in k:
            x0 = min(x1 - 0.02, max(0, x0 + dx))
        if "r" in k:
            x1 = max(x0 + 0.02, min(1, x1 + dx))
        if "t" in k:
            y0 = min(y1 - 0.02, max(0, y0 + dy))
        if "b" in k:
            y1 = max(y0 + 0.02, min(1, y1 + dy))
        if self.crop_aspect:
            fw, fh = self.full_size
            a = self.crop_aspect * fh / fw   # 正規化座標での 幅/高さ
            w, h = x1 - x0, y1 - y0
            if k in ("t", "b"):
                w = h * a
            elif k in ("l", "r"):
                h = w / a
            else:
                if w / max(h, 1e-6) > a:
                    w = h * a
                else:
                    h = w / a
            # 動かした辺の反対側を固定
            if "l" in k:
                x0 = x1 - w
            else:
                x1 = x0 + w
            if "t" in k:
                y0 = y1 - h
            elif "b" in k or k in ("l", "r"):
                y1 = y0 + h
            if x0 < 0 or y0 < 0 or x1 > 1 or y1 > 1:
                return
        self.crop.update(x=x0, y=y0, w=x1 - x0, h=y1 - y0)
        self.update()
