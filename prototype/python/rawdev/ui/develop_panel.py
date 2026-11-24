"""現像パネル（右側）。値が変わると paramsChanged(params, final, 説明) を出す。"""
from __future__ import annotations

import copy

from PySide6.QtCore import Qt, Signal, QSettings
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QComboBox, QLabel, QCheckBox,
                               QTabWidget, QScrollArea, QFrame, QCompleter, QSizePolicy, QToolButton)

from ..engine import color
from ..engine.params import default_params, HSL_BANDS
from ..engine.lens import LensDB, HAVE_LENSFUN
from .widgets import ParamRow, Section, ColorWheel, ScrubField
from .curve import CurveEditor
from . import theme

TEMP_GRAD = [(0.0, "#3d6bd6"), (0.45, "#c8c8c8"), (1.0, "#e0b030")]
TINT_GRAD = [(0.0, "#3fbf4a"), (0.5, "#c8c8c8"), (1.0, "#d040c8")]
HSL_COLORS = ["#e0443e", "#e88a2c", "#e6c229", "#3fae49", "#3cc6c6", "#3b7dd8", "#8a4fd8", "#d040b0"]

TONE_ROWS = [
    ("exposure", "露出", -5, 5, 0, 2, 0.01, " EV"),
    ("contrast", "コントラスト", -100, 100, 0, 0, 1, ""),
    ("highlights", "ハイライト", -100, 100, 0, 0, 1, ""),
    ("shadows", "シャドウ", -100, 100, 0, 0, 1, ""),
    ("whites", "白レベル", -100, 100, 0, 0, 1, ""),
    ("blacks", "黒レベル", -100, 100, 0, 0, 1, ""),
]
ASPECTS = [("自由", None), ("元の比率", "orig"), ("1:1", 1.0), ("4:3", 4 / 3), ("3:2", 3 / 2), ("16:9", 16 / 9),
           ("5:4", 5 / 4), ("4:5（縦）", 4 / 5), ("2:3（縦）", 2 / 3), ("9:16（縦）", 9 / 16)]


class DevelopPanel(QWidget):
    paramsChanged = Signal(dict, bool, str)
    autoRequested = Signal(float)
    hoverKey = Signal(object)
    cropToolRequested = Signal(bool)
    aspectChanged = Signal(object)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.p = default_params()
        self.as_shot = (5500.0, 0.0)
        self.lens_detect = None
        self._updating = False
        self.rows: dict[str, ParamRow] = {}

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        # モード切替
        top = QHBoxLayout()
        top.setContentsMargins(8, 6, 8, 6)
        self.easy_btn = QToolButton()
        self.easy_btn.setText("かんたん")
        self.easy_btn.setCheckable(True)
        self.easy_btn.setToolTip("項目を「自動・明るさ・色味・鮮やかさ・メリハリ」の 5 つに絞る")
        self.easy_btn.toggled.connect(self._set_easy)
        top.addWidget(QLabel("現像"))
        top.addStretch(1)
        top.addWidget(self.easy_btn)
        outer.addLayout(top)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        body = QWidget()
        self.v = QVBoxLayout(body)
        self.v.setContentsMargins(0, 0, 0, 0)
        self.v.setSpacing(2)
        scroll.setWidget(body)
        outer.addWidget(scroll, 1)

        self._build_auto()
        self._build_easy()
        self._build_basic()
        self._build_wheels()
        self._build_curve()
        self._build_hsl()
        self._build_detail()
        self._build_lens()
        self._build_crop()
        self.v.addStretch(1)
        self._set_easy(QSettings().value("ui/easy", False, type=bool))
        self.easy_btn.setChecked(QSettings().value("ui/easy", False, type=bool))

    # ---------------------------------------------------------------- build
    def _row(self, sec, key, label, lo, hi, default, dec, step, suffix, **kw):
        r = ParamRow(key, label, lo, hi, default, dec, step, suffix, **kw)
        r.changed.connect(self._row_changed)
        r.hovered.connect(lambda k, h: self.hoverKey.emit(k if h else None))
        sec.add(r)
        self.rows[key] = r
        return r

    def _build_auto(self):
        w = QWidget()
        h = QHBoxLayout(w)
        h.setContentsMargins(8, 6, 8, 6)
        self.auto_btn = QPushButton("自動調整")
        self.auto_btn.setObjectName("Accent")
        self.auto_btn.setToolTip("露出・ホワイトバランス・トーンを自動で設定（Ctrl+U）。結果は手で直せます")
        self.auto_btn.clicked.connect(lambda: self.autoRequested.emit(self.auto_strength.value()))
        h.addWidget(self.auto_btn, 1)
        h.addWidget(QLabel("強さ"))
        self.auto_strength = ScrubField(100, 0, 100, 100, 0, 1, "%", 54, signed=False)
        self.auto_strength.valueChanged.connect(lambda v, f: f and self.autoRequested.emit(v))
        h.addWidget(self.auto_strength)
        self.v.addWidget(w)

    def _build_easy(self):
        sec = Section("かんたん調整")
        self.easy_sec = sec
        self._row(sec, "easy_bright", "明るさ", -3, 3, 0, 2, 0.01, " EV")
        self._row(sec, "easy_temp", "色味", -100, 100, 0, 0, 1, "", gradient=TEMP_GRAD)
        self._row(sec, "easy_vivid", "鮮やかさ", -100, 100, 0, 0, 1, "")
        self._row(sec, "easy_punch", "メリハリ", -100, 100, 0, 0, 1, "")
        sec.resetRequested.connect(lambda: self._reset_keys(["exposure", "wb", "vibrance", "contrast"]))
        self.v.addWidget(sec)

    def _build_basic(self):
        sec = Section("基本補正")
        self.basic_sec = sec
        wb = QWidget()
        h = QHBoxLayout(wb)
        h.setContentsMargins(8, 2, 8, 2)
        lab = QLabel("WB")
        lab.setFixedWidth(78)
        lab.setStyleSheet(f"color:{theme.TEXT_DIM}")
        self.wb_mode = QComboBox()
        self.wb_mode.addItems(["撮影時の設定", "自動", "カスタム", "太陽光（5500K）", "曇天（6500K）",
                               "日陰（7500K）", "白熱電球（2850K）", "蛍光灯（3800K）", "フラッシュ（5500K）"])
        self.wb_mode.activated.connect(self._wb_preset)
        h.addWidget(lab)
        h.addWidget(self.wb_mode, 1)
        self.wb_pick = QToolButton()
        self.wb_pick.setText("◎")
        self.wb_pick.setCheckable(True)
        self.wb_pick.setToolTip("スポイト：写真の中の白・グレーの部分をクリック（W）")
        h.addWidget(self.wb_pick)
        sec.add(wb)
        self._row(sec, "temp", "色温度", color.TEMP_MIN, color.TEMP_MAX, 5500, 0, 10, " K",
                  gradient=TEMP_GRAD, to_slider=color.temp_to_slider, from_slider=color.slider_to_temp, scrub_width=64)
        self._row(sec, "tint", "色かぶり", -150, 150, 0, 0, 1, "", gradient=TINT_GRAD)
        for key, label, lo, hi, d, dec, step, suf in TONE_ROWS:
            self._row(sec, key, label, lo, hi, d, dec, step, suf)
        self._row(sec, "vibrance", "自然な彩度", -100, 100, 0, 0, 1, "")
        self._row(sec, "saturation", "彩度", -100, 100, 0, 0, 1, "")
        sec.resetRequested.connect(lambda: self._reset_keys(
            ["wb", "exposure", "contrast", "highlights", "shadows", "whites", "blacks", "vibrance", "saturation"]))
        self.v.addWidget(sec)

    def _build_wheels(self):
        sec = Section("カラーホイール", collapsed=True)
        self.wheels_sec = sec
        w = QWidget()
        h = QHBoxLayout(w)
        h.setContentsMargins(4, 2, 4, 2)
        h.setSpacing(2)
        self.wheels = {}
        for key, name in [("lift", "リフト"), ("gamma", "ガンマ"), ("gain", "ゲイン"), ("offset", "オフセット")]:
            cw = ColorWheel(name)
            cw.changed.connect(lambda vals, final, k=key: self._wheel_changed(k, vals, final))
            h.addWidget(cw)
            self.wheels[key] = cw
        sec.add(w)
        note = QLabel("リフト＝暗部　ガンマ＝中間　ゲイン＝明部　オフセット＝全体")
        note.setWordWrap(True)
        note.setStyleSheet(f"color:{theme.TEXT_DIM}; padding:0 8px;")
        sec.add(note)
        sec.resetRequested.connect(lambda: self._reset_keys(["wheels"]))
        self.v.addWidget(sec)

    def _build_curve(self):
        sec = Section("トーンカーブ", collapsed=True)
        self.curve_sec = sec
        self.curve = CurveEditor()
        self.curve.changed.connect(self._curve_changed)
        sec.add(self.curve)
        hint = QLabel("クリック：点を追加　右クリック：削除　ダブルクリック：リセット")
        hint.setWordWrap(True)
        hint.setStyleSheet(f"color:{theme.TEXT_DIM}; padding:0 8px;")
        sec.add(hint)
        sec.resetRequested.connect(lambda: self._reset_keys(["curve"]))
        self.v.addWidget(sec)

    def _build_hsl(self):
        sec = Section("HSL / カラー", collapsed=True)
        self.hsl_sec = sec
        tabs = QTabWidget()
        self.hsl_rows = {}
        for kind, name in [("hue", "色相"), ("sat", "彩度"), ("lum", "輝度")]:
            page = QWidget()
            vv = QVBoxLayout(page)
            vv.setContentsMargins(0, 2, 0, 2)
            vv.setSpacing(1)
            for i, band in enumerate(HSL_BANDS):
                r = ParamRow(f"hsl:{kind}:{i}", band, -100, 100, 0, 0, 1, "",
                             gradient=[(0, "#555"), (0.5, "#777"), (1, HSL_COLORS[i])])
                r.changed.connect(self._row_changed)
                vv.addWidget(r)
                self.hsl_rows[(kind, i)] = r
            tabs.addTab(page, name)
        sec.add(tabs)
        sec.resetRequested.connect(lambda: self._reset_keys(["hsl"]))
        self.v.addWidget(sec)

    def _build_detail(self):
        sec = Section("ディテール", collapsed=True)
        self.detail_sec = sec
        self._row(sec, "sharpen.amount", "シャープ 量", 0, 150, 40, 0, 1, "")
        self._row(sec, "sharpen.radius", "半径", 0.5, 3.0, 1.0, 1, 0.1, "")
        self._row(sec, "sharpen.masking", "マスク", 0, 100, 0, 0, 1, "")
        self._row(sec, "nr.luma", "ノイズ 輝度", 0, 100, 0, 0, 1, "")
        self._row(sec, "nr.color", "ノイズ カラー", 0, 100, 25, 0, 1, "")
        n = QLabel("シャープ・ノイズ低減は、ズームしたときの高精細表示で確認してください")
        n.setWordWrap(True)
        n.setStyleSheet(f"color:{theme.TEXT_DIM}; padding:0 8px;")
        sec.add(n)
        sec.resetRequested.connect(lambda: self._reset_keys(["sharpen", "nr"]))
        self.v.addWidget(sec)

    def _build_lens(self):
        sec = Section("レンズ補正", collapsed=True)
        self.lens_sec = sec
        tabs = QTabWidget()
        prof = QWidget()
        pv = QVBoxLayout(prof)
        pv.setContentsMargins(8, 4, 8, 4)
        self.lens_enable = QCheckBox("プロファイル補正を使用")
        self.lens_enable.toggled.connect(lambda on: self._set("lens.profile", on, True, "レンズプロファイル"))
        pv.addWidget(self.lens_enable)
        self.lens_status = QLabel("")
        self.lens_status.setWordWrap(True)
        self.lens_status.setStyleSheet(f"color:{theme.TEXT_DIM}")
        pv.addWidget(self.lens_status)
        hl = QHBoxLayout()
        hl.addWidget(QLabel("レンズ"))
        self.lens_combo = QComboBox()
        self.lens_combo.setEditable(True)
        self.lens_combo.setInsertPolicy(QComboBox.NoInsert)
        self.lens_combo.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.lens_combo.setMinimumContentsLength(6)
        self.lens_combo.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.lens_combo.addItem("自動（EXIF から検出）", None)
        self._lens_loaded = False
        self.lens_combo.activated.connect(self._lens_selected)
        hl.addWidget(self.lens_combo, 1)
        pv.addLayout(hl)
        for key, label in [("lens.distortion", "歪曲"), ("lens.vignetting", "周辺光量")]:
            r = ParamRow(key, label, 0, 200, 100, 0, 1, "%")
            r.changed.connect(self._row_changed)
            r.layout().setContentsMargins(0, 1, 0, 1)
            pv.addWidget(r)
            self.rows[key] = r
        self.lens_ca = QCheckBox("色収差を除去")
        self.lens_ca.toggled.connect(lambda on: self._set("lens.ca", on, True, "色収差除去"))
        self.lens_trim = QCheckBox("端の空白を自動トリミング")
        self.lens_trim.toggled.connect(lambda on: self._set("lens.autotrim", on, True, "自動トリミング"))
        pv.addWidget(self.lens_ca)
        pv.addWidget(self.lens_trim)
        if not HAVE_LENSFUN:
            self.lens_status.setText("lensfunpy が入っていないため、プロファイル補正は使えません（手動補正は使えます）")
        tabs.addTab(prof, "プロファイル")
        man = QWidget()
        mv = QVBoxLayout(man)
        mv.setContentsMargins(0, 4, 0, 4)
        for key, label in [("lens.manual_distortion", "歪曲"), ("lens.manual_vignetting", "周辺光量")]:
            r = ParamRow(key, label, -100, 100, 0, 0, 1, "")
            r.changed.connect(self._row_changed)
            mv.addWidget(r)
            self.rows[key] = r
        mv.addStretch(1)
        tabs.addTab(man, "手動")
        sec.add(tabs)
        sec.resetRequested.connect(lambda: self._reset_keys(["lens"]))
        self.v.addWidget(sec)

    def _build_crop(self):
        sec = Section("切り抜きと角度", collapsed=True)
        self.crop_sec = sec
        w = QWidget()
        v = QVBoxLayout(w)
        v.setContentsMargins(8, 2, 8, 2)
        h = QHBoxLayout()
        self.crop_btn = QPushButton("切り抜きツール (R)")
        self.crop_btn.setCheckable(True)
        self.crop_btn.toggled.connect(self.cropToolRequested)
        h.addWidget(self.crop_btn)
        self.aspect = QComboBox()
        for name, val in ASPECTS:
            self.aspect.addItem(name, val)
        self.aspect.currentIndexChanged.connect(lambda i: self.aspectChanged.emit(self.aspect.itemData(i)))
        h.addWidget(self.aspect)
        v.addLayout(h)
        sec.add(w)
        self._row(sec, "crop.angle", "角度", -45, 45, 0, 2, 0.05, "°")
        sec.resetRequested.connect(lambda: self._reset_keys(["crop"]))
        self.v.addWidget(sec)

    # ---------------------------------------------------------------- easy mode
    def _set_easy(self, on):
        QSettings().setValue("ui/easy", on)
        self.easy_sec.setVisible(on)
        for s in (self.basic_sec, self.wheels_sec, self.curve_sec, self.hsl_sec, self.detail_sec,
                  self.lens_sec, self.crop_sec):
            s.setVisible(not on)

    # ---------------------------------------------------------------- set / get
    def set_params(self, p: dict, as_shot=(5500.0, 0.0)):
        self._updating = True
        self.p = copy.deepcopy(p)
        self.as_shot = as_shot
        wb = self.p["wb"]
        if wb["mode"] == "as_shot":
            t, tint = as_shot
        else:
            t, tint = wb["temp"], wb["tint"]
        self.rows["temp"].setValue(t)
        self.rows["tint"].setValue(tint)
        idx = {"as_shot": 0, "auto": 1}.get(wb["mode"], 2)
        self.wb_mode.setCurrentIndex(idx)
        for key, *_ in TONE_ROWS:
            self.rows[key].setValue(self.p[key])
        self.rows["vibrance"].setValue(self.p["vibrance"])
        self.rows["saturation"].setValue(self.p["saturation"])
        for k, cw in self.wheels.items():
            cw.setValues(self.p["wheels"][k])
        self.curve.set_curves(self.p["curve"])
        for (kind, i), r in self.hsl_rows.items():
            r.setValue(self.p["hsl"][kind][i])
        for k in ("sharpen.amount", "sharpen.radius", "sharpen.masking", "nr.luma", "nr.color",
                  "lens.distortion", "lens.vignetting", "lens.manual_distortion", "lens.manual_vignetting",
                  "crop.angle"):
            a, b = k.split(".")
            self.rows[k].setValue(self.p[a][b])
        lp = self.p["lens"]
        for cb, key in ((self.lens_enable, "profile"), (self.lens_ca, "ca"), (self.lens_trim, "autotrim")):
            cb.blockSignals(True)
            cb.setChecked(bool(lp[key]))
            cb.blockSignals(False)
        self._sync_lens_combo()
        # かんたん
        self.rows["easy_bright"].setValue(self.p["exposure"])
        self.rows["easy_vivid"].setValue(self.p["vibrance"])
        self.rows["easy_punch"].setValue(self.p["contrast"])
        t0 = as_shot[0]
        self.rows["easy_temp"].setValue(max(-100, min(100, (1e6 / t0 - 1e6 / t) / 1.5)) if t else 0)
        self.aspect.blockSignals(True)
        a = self.p["crop"].get("aspect", "free")
        for i in range(self.aspect.count()):
            d = self.aspect.itemData(i)
            if (a == "free" and d is None) or d == a:
                self.aspect.setCurrentIndex(i)
                break
        self.aspect.blockSignals(False)
        self._updating = False

    def set_histogram(self, h):
        self.curve.set_histogram(h)

    def set_lens_detect(self, text, lens_key):
        self.lens_detect = lens_key
        self.lens_status.setText(text)
        self._sync_lens_combo()

    def _ensure_lens_list(self):
        if self._lens_loaded or not HAVE_LENSFUN:
            return
        db = LensDB.get()
        names = []
        for l in db.all_lenses():
            k = LensDB.key(l)
            self.lens_combo.addItem(f"{l.maker}  {l.model}", k)
            names.append(f"{l.maker}  {l.model}")
        comp = QCompleter(names, self.lens_combo)
        comp.setCaseSensitivity(Qt.CaseInsensitive)
        comp.setFilterMode(Qt.MatchContains)
        self.lens_combo.setCompleter(comp)
        self._lens_loaded = True

    def _sync_lens_combo(self):
        self._ensure_lens_list()
        key = self.p["lens"].get("lens")
        self.lens_combo.blockSignals(True)
        if key is None:
            self.lens_combo.setCurrentIndex(0)
        else:
            i = self.lens_combo.findData(key)
            self.lens_combo.setCurrentIndex(max(0, i))
        self.lens_combo.blockSignals(False)

    def _lens_selected(self, i):
        self._set("lens.lens", self.lens_combo.itemData(i), True, "レンズを選択")

    # ---------------------------------------------------------------- changes
    def _set(self, path, value, final, desc):
        if self._updating:
            return
        parts = path.split(".")
        d = self.p
        for k in parts[:-1]:
            d = d[k]
        d[parts[-1]] = value
        self.paramsChanged.emit(copy.deepcopy(self.p), final, desc)

    def _row_changed(self, key, val, final):
        if self._updating:
            return
        names = {"temp": "色温度", "tint": "色かぶり", "exposure": "露出", "contrast": "コントラスト",
                 "highlights": "ハイライト", "shadows": "シャドウ", "whites": "白レベル", "blacks": "黒レベル",
                 "vibrance": "自然な彩度", "saturation": "彩度"}
        if key in ("temp", "tint"):
            wb = self.p["wb"]
            if wb["mode"] == "as_shot":
                wb["temp"], wb["tint"] = self.as_shot
            wb["mode"] = "custom"
            wb[key] = float(val)
            self.wb_mode.setCurrentIndex(2)
        elif key.startswith("hsl:"):
            _, kind, i = key.split(":")
            self.p["hsl"][kind][int(i)] = float(val)
            key = f"HSL {kind}"
        elif key.startswith("easy_"):
            self._easy_changed(key, val)
        elif "." in key:
            a, b = key.split(".")
            self.p[a][b] = float(val)
        else:
            self.p[key] = float(val)
        self.paramsChanged.emit(copy.deepcopy(self.p), final, names.get(key, key))

    def _easy_changed(self, key, val):
        if key == "easy_bright":
            self.p["exposure"] = float(val)
            self.rows["exposure"].setValue(val)
        elif key == "easy_vivid":
            self.p["vibrance"] = float(val)
            self.rows["vibrance"].setValue(val)
        elif key == "easy_punch":
            self.p["contrast"] = float(val)
            self.rows["contrast"].setValue(val)
        elif key == "easy_temp":
            t0, tint0 = self.as_shot
            m = 1e6 / t0 - val * 1.5
            t = 1e6 / max(m, 20)
            self.p["wb"] = {"mode": "custom", "temp": round(t, -1), "tint": self.p["wb"].get("tint", tint0)
                            if self.p["wb"]["mode"] != "as_shot" else tint0}
            self.rows["temp"].setValue(t)

    def _wheel_changed(self, key, vals, final):
        if self._updating:
            return
        self.p["wheels"][key] = [round(x, 4) for x in vals]
        self.paramsChanged.emit(copy.deepcopy(self.p), final, "カラーホイール")

    def _curve_changed(self, curves, final):
        if self._updating:
            return
        self.p["curve"] = curves
        self.paramsChanged.emit(copy.deepcopy(self.p), final, "トーンカーブ")

    def _wb_preset(self, i):
        presets = {3: 5500, 4: 6500, 5: 7500, 6: 2850, 7: 3800, 8: 5500}
        if i == 0:
            self.p["wb"] = {"mode": "as_shot", "temp": self.as_shot[0], "tint": self.as_shot[1]}
            self.paramsChanged.emit(copy.deepcopy(self.p), True, "WB：撮影時")
            self.set_params(self.p, self.as_shot)
        elif i == 1:
            self.autoRequested.emit(-1.0)   # WB だけ自動
        elif i in presets:
            self.p["wb"] = {"mode": "custom", "temp": float(presets[i]), "tint": 0.0}
            self.paramsChanged.emit(copy.deepcopy(self.p), True, "WB プリセット")
            self.set_params(self.p, self.as_shot)

    def set_wb(self, temp, tint, desc="WB"):
        self.p["wb"] = {"mode": "custom", "temp": float(temp), "tint": float(tint)}
        self.set_params(self.p, self.as_shot)
        self.paramsChanged.emit(copy.deepcopy(self.p), True, desc)

    def _reset_keys(self, keys):
        d = default_params()
        for k in keys:
            if k == "wb":
                self.p["wb"] = d["wb"]
            else:
                self.p[k] = copy.deepcopy(d[k])
        self.set_params(self.p, self.as_shot)
        self.paramsChanged.emit(copy.deepcopy(self.p), True, "リセット")

    def nudge_zone(self, key, dv, final):
        """ヒストグラムの区画ドラッグ → 対応するスライダーを動かす。"""
        if key == "exposure":
            val = self.p["exposure"] + dv * 4.0
            self.rows["exposure"].setValue(val)
            self.p["exposure"] = self.rows["exposure"].value()
        else:
            val = self.p[key] + dv * 200.0
            self.rows[key].setValue(val)
            self.p[key] = self.rows[key].value()
        self.paramsChanged.emit(copy.deepcopy(self.p), final, "ヒストグラム操作")
