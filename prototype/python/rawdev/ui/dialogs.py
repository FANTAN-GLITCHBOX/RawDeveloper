"""書き出しダイアログ・設定ダイアログ・プリセット保存ダイアログ。"""
from __future__ import annotations

import json
import os

from PySide6.QtCore import Qt, QSettings
from PySide6.QtWidgets import (QDialog, QVBoxLayout, QHBoxLayout, QFormLayout, QComboBox, QSpinBox, QLineEdit,
                               QPushButton, QCheckBox, QFileDialog, QLabel, QDialogButtonBox, QRadioButton,
                               QButtonGroup, QGroupBox, QListWidget, QListWidgetItem, QInputDialog)

from ..core.export import ExportOptions
from ..engine.params import GROUPS, GROUP_NAMES
from . import theme

BUILTIN_EXPORT = {
    "SNS 用（長辺 2048px・JPEG）": {"fmt": "jpeg", "quality": 88, "long_edge": 2048},
    "印刷用（TIFF 16bit・元サイズ）": {"fmt": "tiff", "quality": 100, "long_edge": 0},
    "納品用（JPEG 最高画質・元サイズ）": {"fmt": "jpeg", "quality": 97, "long_edge": 0},
}


class ExportDialog(QDialog):
    def __init__(self, n_selected, n_filtered, parent=None):
        super().__init__(parent)
        self.setWindowTitle("書き出し")
        self.setMinimumWidth(520)
        st = QSettings()
        v = QVBoxLayout(self)

        pr = QHBoxLayout()
        pr.addWidget(QLabel("設定"))
        self.preset = QComboBox()
        self.preset.addItem("（前回の設定）")
        self._user_presets = json.loads(st.value("export/presets", "{}"))
        for name in list(BUILTIN_EXPORT) + list(self._user_presets):
            self.preset.addItem(name)
        self.preset.activated.connect(self._apply_preset)
        pr.addWidget(self.preset, 1)
        save = QPushButton("この設定を保存")
        save.clicked.connect(self._save_preset)
        pr.addWidget(save)
        v.addLayout(pr)

        g1 = QGroupBox("対象")
        t = QVBoxLayout(g1)
        self.target = QButtonGroup(self)
        self.t_sel = QRadioButton(f"選択中の写真（{n_selected} 枚）")
        self.t_all = QRadioButton(f"表示中の写真すべて（{n_filtered} 枚）")
        self.t_star = QRadioButton("表示中のうち ★ が次以上：")
        self.t_pick = QRadioButton("表示中のうち 採用フラグだけ")
        for i, b in enumerate((self.t_sel, self.t_all, self.t_star, self.t_pick)):
            self.target.addButton(b, i)
        hs = QHBoxLayout()
        hs.addWidget(self.t_star)
        self.min_star = QSpinBox()
        self.min_star.setRange(1, 5)
        self.min_star.setValue(int(st.value("export/min_star", 4)))
        hs.addWidget(self.min_star)
        hs.addStretch(1)
        t.addWidget(self.t_sel)
        t.addWidget(self.t_all)
        t.addLayout(hs)
        t.addWidget(self.t_pick)
        self.t_sel.setChecked(True)
        v.addWidget(g1)

        g2 = QGroupBox("形式とサイズ")
        f = QFormLayout(g2)
        self.fmt = QComboBox()
        self.fmt.addItem("JPEG", "jpeg")
        self.fmt.addItem("TIFF（16bit）", "tiff")
        self.fmt.addItem("PNG（8bit）", "png")
        self.quality = QSpinBox()
        self.quality.setRange(50, 100)
        self.long_edge = QSpinBox()
        self.long_edge.setRange(0, 20000)
        self.long_edge.setSingleStep(256)
        self.long_edge.setSpecialValueText("元のサイズ")
        self.space = QComboBox()
        self.space.addItem("sRGB（ICC プロファイル埋め込み）")
        f.addRow("形式", self.fmt)
        f.addRow("画質（JPEG）", self.quality)
        f.addRow("長辺のピクセル数", self.long_edge)
        f.addRow("色空間", self.space)
        v.addWidget(g2)

        g3 = QGroupBox("保存先とファイル名")
        f3 = QFormLayout(g3)
        self.same = QCheckBox("元の写真と同じフォルダ")
        fl = QHBoxLayout()
        self.folder = QLineEdit()
        b = QPushButton("参照…")
        b.clicked.connect(self._browse)
        fl.addWidget(self.folder, 1)
        fl.addWidget(b)
        self.subfolder = QLineEdit()
        self.subfolder.setPlaceholderText("例：export（空なら作らない）")
        self.template = QLineEdit()
        self.template.setToolTip("{name}＝元のファイル名、{n}＝連番（0001〜）")
        self.suffix = QLineEdit()
        self.suffix.setPlaceholderText("例：_edit")
        self.overwrite = QComboBox()
        self.overwrite.addItem("同名があれば番号を付ける", "rename")
        self.overwrite.addItem("上書きする", "overwrite")
        self.overwrite.addItem("書き出さない", "skip")
        f3.addRow("", self.same)
        f3.addRow("フォルダ", fl)
        f3.addRow("サブフォルダ", self.subfolder)
        f3.addRow("ファイル名", self.template)
        f3.addRow("末尾に付ける文字", self.suffix)
        f3.addRow("同名ファイル", self.overwrite)
        v.addWidget(g3)

        g4 = QGroupBox("レーティング・ラベル")
        v4 = QVBoxLayout(g4)
        self.embed = QCheckBox("★とラベルをファイルに埋め込む（エクスプローラーでも★が見えます）")
        self.add_lib = QCheckBox("書き出したファイルをライブラリに追加し、元の写真と紐付ける")
        v4.addWidget(self.embed)
        v4.addWidget(self.add_lib)
        v.addWidget(g4)

        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.button(QDialogButtonBox.Ok).setText("書き出す")
        bb.button(QDialogButtonBox.Cancel).setText("キャンセル")
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        v.addWidget(bb)
        self.same.toggled.connect(lambda on: self.folder.setEnabled(not on))
        self._load(json.loads(st.value("export/last", "{}")))

    def _load(self, d):
        o = ExportOptions(**{k: v for k, v in d.items() if k in ExportOptions.__dataclass_fields__})
        self.fmt.setCurrentIndex(max(0, self.fmt.findData(o.fmt)))
        self.quality.setValue(o.quality)
        self.long_edge.setValue(o.long_edge)
        self.same.setChecked(o.same_folder)
        self.folder.setText(o.folder or os.path.join(os.path.expanduser("~"), "Pictures", "RawDeveloper"))
        self.subfolder.setText(o.subfolder)
        self.template.setText(o.name_template)
        self.suffix.setText(o.suffix)
        self.overwrite.setCurrentIndex(max(0, self.overwrite.findData(o.overwrite)))
        self.embed.setChecked(o.embed_rating)
        self.add_lib.setChecked(o.add_to_library)

    def _apply_preset(self, i):
        name = self.preset.itemText(i)
        d = BUILTIN_EXPORT.get(name) or self._user_presets.get(name)
        if d:
            cur = self.options().__dict__
            cur.update(d)
            self._load(cur)

    def _save_preset(self):
        name, ok = QInputDialog.getText(self, "書き出し設定の保存", "名前")
        if ok and name.strip():
            d = self.options().__dict__
            self._user_presets[name.strip()] = {k: d[k] for k in ("fmt", "quality", "long_edge", "suffix",
                                                                  "name_template", "subfolder", "embed_rating")}
            QSettings().setValue("export/presets", json.dumps(self._user_presets, ensure_ascii=False))
            self.preset.addItem(name.strip())

    def _browse(self):
        d = QFileDialog.getExistingDirectory(self, "書き出し先フォルダ", self.folder.text())
        if d:
            self.folder.setText(d)

    def options(self) -> ExportOptions:
        return ExportOptions(
            folder=self.folder.text(), same_folder=self.same.isChecked(), subfolder=self.subfolder.text().strip(),
            fmt=self.fmt.currentData(), quality=self.quality.value(), long_edge=self.long_edge.value(),
            name_template=self.template.text() or "{name}", suffix=self.suffix.text(),
            embed_rating=self.embed.isChecked(), overwrite=self.overwrite.currentData(),
            add_to_library=self.add_lib.isChecked())

    def target_mode(self):
        return ["selected", "all", "star", "pick"][self.target.checkedId()], self.min_star.value()

    def accept(self):
        st = QSettings()
        st.setValue("export/last", json.dumps(self.options().__dict__, ensure_ascii=False))
        st.setValue("export/min_star", self.min_star.value())
        super().accept()


class SettingsDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("環境設定")
        st = QSettings()
        f = QFormLayout(self)
        self.preview = QComboBox()
        for s in (1280, 1600, 2048, 2560):
            self.preview.addItem(f"長辺 {s}px", s)
        self.preview.setCurrentIndex(max(0, self.preview.findData(int(st.value("render/preview", 1600)))))
        self.hires = QComboBox()
        for s in (3000, 4000, 6000, 0):
            self.hires.addItem("元のサイズ（重い）" if s == 0 else f"長辺 {s}px", s)
        self.hires.setCurrentIndex(max(0, self.hires.findData(int(st.value("render/hires", 6000)))))
        self.auto_adv = QCheckBox("★・ラベル・フラグを付けたら次の写真へ進む")
        self.auto_adv.setChecked(st.value("rate/auto_advance", False, type=bool))
        self.write_files = QCheckBox("JPEG・TIFF・PNG の★・ラベルはファイル自体にも書き込む")
        self.write_files.setChecked(st.value("rate/write_files", True, type=bool))
        self.xmp_raw = QCheckBox("RAW の★・ラベルを XMP サイドカー（.xmp）にも書く（Lightroom などと共有）")
        self.xmp_raw.setChecked(st.value("rate/xmp_sidecar", False, type=bool))
        self.propagate = QComboBox()
        self.propagate.addItem("常に反映する", "always")
        self.propagate.addItem("その都度確認する", "ask")
        self.propagate.addItem("反映しない", "never")
        self.propagate.setCurrentIndex(max(0, self.propagate.findData(st.value("rate/propagate", "ask"))))
        self.wheel = QCheckBox("マウスホイールでスライダー・数値を変更する")
        self.wheel.setChecked(st.value("ui/wheel_adjust", True, type=bool))
        self.keys = QComboBox()
        self.keys.addItem("Resolve 風（Shift+D で補正オン/オフ、Ctrl 微調整）", "resolve")
        self.keys.addItem("Lightroom 風（\\ で補正オン/オフ）", "lightroom")
        self.keys.setCurrentIndex(max(0, self.keys.findData(st.value("ui/keys", "resolve"))))
        f.addRow("プレビュー解像度", self.preview)
        f.addRow("ズーム時の高精細表示", self.hires)
        f.addRow("", self.auto_adv)
        f.addRow("", self.write_files)
        f.addRow("", self.xmp_raw)
        f.addRow("元の写真の★を変えたとき\n書き出したファイルにも", self.propagate)
        f.addRow("", self.wheel)
        f.addRow("ショートカット", self.keys)
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        f.addRow(bb)

    def accept(self):
        st = QSettings()
        st.setValue("render/preview", self.preview.currentData())
        st.setValue("render/hires", self.hires.currentData())
        st.setValue("rate/auto_advance", self.auto_adv.isChecked())
        st.setValue("rate/write_files", self.write_files.isChecked())
        st.setValue("rate/xmp_sidecar", self.xmp_raw.isChecked())
        st.setValue("rate/propagate", self.propagate.currentData())
        st.setValue("ui/wheel_adjust", self.wheel.isChecked())
        st.setValue("ui/keys", self.keys.currentData())
        super().accept()


class GroupPickDialog(QDialog):
    """プリセット保存・設定のペースト時に、どの項目を対象にするか選ぶ。"""

    def __init__(self, title, default_all=True, parent=None, with_name=False):
        super().__init__(parent)
        self.setWindowTitle(title)
        v = QVBoxLayout(self)
        self.name = None
        if with_name:
            self.name = QLineEdit()
            self.name.setPlaceholderText("プリセット名")
            v.addWidget(self.name)
        self.checks = {}
        for g in GROUPS:
            c = QCheckBox(GROUP_NAMES[g])
            c.setChecked(default_all and g != "crop")
            v.addWidget(c)
            self.checks[g] = c
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        v.addWidget(bb)

    def groups(self):
        return [g for g, c in self.checks.items() if c.isChecked()]
