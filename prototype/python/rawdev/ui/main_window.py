"""メインウィンドウ：ライブラリ／現像の切替、描画スレッド、評価、書き出し。"""
from __future__ import annotations

import copy
import glob
import json
import os
import time
import traceback
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PySide6.QtCore import Qt, QObject, Signal, QTimer, QSettings, QItemSelectionModel, QSize
from PySide6.QtGui import QAction, QKeySequence, QShortcut, QImage
from PySide6.QtWidgets import (QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QStackedWidget, QSplitter,
                               QDockWidget, QToolButton, QPushButton, QLabel, QFileDialog, QMessageBox,
                               QListWidget, QListWidgetItem, QProgressDialog, QInputDialog, QApplication,
                               QTabWidget, QMenu, QSizePolicy, QFrame, QFormLayout)

from ..engine import decode, pipeline, auto, color
from ..engine.params import (default_params, merged, LABELS, LABEL_NAMES, extract_groups, apply_groups)
from ..engine.lens import LensDB
from ..core.catalog import Catalog, PhotoState, save_sidecar, app_dir
from ..core import xmp, export as exporter
from .library import (PhotoModel, FilterProxy, ThumbGrid, FilterBar, FolderTree, PhotoItem,
                      edited_thumb_path)
from .viewer import Viewer, to_qimage
from .scopes import ScopesPanel, ScopeView
from .develop_panel import DevelopPanel
from .dialogs import ExportDialog, SettingsDialog, GroupPickDialog
from . import theme

BUILTIN_PRESETS = {
    "モノクロ": {"saturation": -100.0, "vibrance": 0.0, "contrast": 15.0},
    "鮮やか": {"vibrance": 35.0, "saturation": 5.0, "contrast": 15.0},
    "ふんわり": {"contrast": -25.0, "highlights": -30.0, "shadows": 30.0, "vibrance": -10.0},
    "くっきり": {"contrast": 30.0, "whites": 10.0, "blacks": -10.0, "sharpen": {"amount": 70.0, "radius": 1.0, "masking": 20.0}},
    "フィルム風": {"curve": {"Y": [[0.0, 0.06], [0.25, 0.22], [0.75, 0.80], [1.0, 0.96]],
                         "R": [[0.0, 0.0], [1.0, 1.0]], "G": [[0.0, 0.0], [1.0, 1.0]], "B": [[0.0, 0.0], [1.0, 1.0]]},
                "wheels": {"lift": [-0.35, -0.2, 0.0], "gamma": [0.0, 0.0, 0.0], "gain": [0.3, 0.25, 0.0],
                           "offset": [0.0, 0.0, 0.0]}, "vibrance": -10.0},
}


def presets_dir():
    d = os.path.join(app_dir(), "presets")
    os.makedirs(d, exist_ok=True)
    return d


class Emitter(QObject):
    folderLoaded = Signal(int, list)
    photoLoaded = Signal(int, object)
    rendered = Signal(int, str, object)
    failed = Signal(str)
    exportProgress = Signal(int, str)
    exportDone = Signal(list, list)


class Session:
    """開いている 1 枚の写真の作業データ。"""

    def __init__(self, path):
        self.path = path
        self.src = None           # 縮小読み込み Source
        self.preview = None       # プレビュー用リニア画像
        self.scale = 1.0          # preview / 元画像
        self.full = None          # フル解像度 Source（ズーム・書き出し時に読み込み）
        self.lens_info = None
        self.as_shot_tt = (5500.0, 0.0)
        self.state: PhotoState | None = None
        self.last_arr = None


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("RawDeveloper")
        self.resize(1600, 980)
        self.settings = QSettings()
        self.catalog = Catalog()
        self.em = Emitter()
        self.pool = ThreadPoolExecutor(max_workers=3)
        self.export_pool = ThreadPoolExecutor(max_workers=1)
        self.folder = None
        self.states: dict[str, PhotoState] = {}
        self.session: Session | None = None
        self._load_token = 0
        self._render_busy = False
        self._render_pending = False
        self._hires_busy = False
        self._hires_key = None
        self._params_version = 0
        self._dragging = False
        self._clipboard = None
        self._before_key = None
        self._crop_backup = None

        self.em.folderLoaded.connect(self._folder_loaded)
        self.em.photoLoaded.connect(self._photo_loaded)
        self.em.rendered.connect(self._rendered)
        self.em.failed.connect(lambda m: self.statusBar().showMessage(m, 8000))

        self.model = PhotoModel()
        self.proxy = FilterProxy()
        self.proxy.setSourceModel(self.model)

        self._build_ui()
        self._build_menus()
        self._build_shortcuts()
        self.save_timer = QTimer(self)
        self.save_timer.setSingleShot(True)
        self.save_timer.timeout.connect(self._save_current)

        geo = self.settings.value("win/geometry")
        if geo is not None:
            self.restoreGeometry(geo)
        last = self.settings.value("library/folder")
        self.set_mode("library")
        if last and os.path.isdir(last):
            QTimer.singleShot(50, lambda: self.open_folder(last))

    # ================================================================ UI
    def _build_ui(self):
        # 上部バー
        top = QWidget()
        top.setObjectName("TopBar")
        top.setStyleSheet(f"QWidget#TopBar{{background:{theme.PANEL};}}")
        th = QHBoxLayout(top)
        th.setContentsMargins(8, 4, 8, 4)
        self.btn_lib = QPushButton("ライブラリ (G)")
        self.btn_dev = QPushButton("現像 (D)")
        for b in (self.btn_lib, self.btn_dev):
            b.setCheckable(True)
        self.btn_lib.clicked.connect(lambda: self.set_mode("library"))
        self.btn_dev.clicked.connect(lambda: self.set_mode("develop"))
        open_btn = QPushButton("フォルダを開く")
        open_btn.clicked.connect(self.choose_folder)
        self.btn_export = QPushButton("書き出し…")
        self.btn_export.setObjectName("Accent")
        self.btn_export.clicked.connect(self.export_dialog)
        self.title = QLabel("")
        self.title.setStyleSheet(f"color:{theme.TEXT_DIM}")
        th.addWidget(self.btn_lib)
        th.addWidget(self.btn_dev)
        th.addSpacing(12)
        th.addWidget(open_btn)
        th.addSpacing(12)
        th.addWidget(self.title, 1)
        th.addWidget(self.btn_export)

        # ライブラリページ
        lib = QWidget()
        lv = QVBoxLayout(lib)
        lv.setContentsMargins(0, 0, 0, 0)
        lv.setSpacing(0)
        self.filter_bar = FilterBar(self.proxy)
        self.filter_bar.changed.connect(self._update_count)
        lv.addWidget(self.filter_bar)
        sp = QSplitter()
        self.tree = FolderTree()
        self.tree.folderChosen.connect(self.open_folder)
        self.grid = ThumbGrid()
        self.grid.setModel(self.proxy)
        self.grid.doubleClicked.connect(lambda ix: self.set_mode("develop"))
        self.grid.selectionModel().currentChanged.connect(self._grid_current)
        self.grid.setContextMenuPolicy(Qt.CustomContextMenu)
        self.grid.customContextMenuRequested.connect(self._grid_menu)
        info = QWidget()
        iv = QVBoxLayout(info)
        iv.setContentsMargins(6, 6, 6, 6)
        self.lib_hist = ScopeView("hist_rgb")
        self.lib_hist.setFixedHeight(150)
        iv.addWidget(QLabel("ヒストグラム（簡易：埋め込みプレビューから）"))
        iv.addWidget(self.lib_hist)
        self.lib_info = QLabel("")
        self.lib_info.setWordWrap(True)
        self.lib_info.setAlignment(Qt.AlignTop)
        self.lib_info.setTextInteractionFlags(Qt.TextSelectableByMouse)
        iv.addWidget(self.lib_info, 1)
        sp.addWidget(self.tree)
        sp.addWidget(self.grid)
        sp.addWidget(info)
        sp.setSizes([240, 1000, 280])
        sp.setStretchFactor(1, 1)
        lv.addWidget(sp, 1)

        # 現像ページ
        dev = QWidget()
        dv = QVBoxLayout(dev)
        dv.setContentsMargins(0, 0, 0, 0)
        dv.setSpacing(0)
        vbar = QWidget()
        vbar.setObjectName("ViewBar")
        vbar.setStyleSheet(f"QWidget#ViewBar{{background:{theme.PANEL};}}")
        vh = QHBoxLayout(vbar)
        vh.setContentsMargins(6, 2, 6, 2)

        def tb(text, tip, checkable=False, slot=None):
            b = QToolButton()
            b.setText(text)
            b.setToolTip(tip)
            b.setCheckable(checkable)
            if slot:
                (b.toggled if checkable else b.clicked).connect(slot)
            vh.addWidget(b)
            return b

        self.tb_wipe = tb("ワイプ比較", "補正前と補正後を線で分けて比較（Y）", True, self._toggle_wipe)
        self.tb_wipe_dir = tb("左右/上下", "ワイプの向きを切替", False, self._toggle_wipe_dir)
        self.tb_bypass = tb("補正オフ", "補正前を表示（Shift+D）", True, self._set_bypass)
        self.tb_clip = tb("白飛び警告", "白飛び（赤）・黒つぶれ（青）を表示（J）", True, self._set_clip)
        tb("全体", "画面に合わせる（Z）", False, lambda: self.viewer.set_zoom(0, fit=True))
        tb("100%", "等倍表示（Z）", False, lambda: self.viewer.set_zoom(1.0))
        vh.addStretch(1)
        self.meta_label = QLabel("")
        self.meta_label.setStyleSheet(f"color:{theme.TEXT_DIM}")
        vh.addWidget(self.meta_label)
        dv.addWidget(vbar)
        self.viewer = Viewer()
        self.viewer.probe.connect(self._probe)
        self.viewer.wantHiRes.connect(self._request_hires)
        self.viewer.zoomChanged.connect(lambda z, fit: (not fit) and self._request_hires())
        self.viewer.cropChanged.connect(self._crop_changed)
        self.viewer.clicked.connect(self._wb_pick)
        dv.addWidget(self.viewer, 1)
        self.film = ThumbGrid(compact=True)
        self.film.setModel(self.proxy)
        self.film.setSelectionModel(self.grid.selectionModel())
        self.film.middleClicked.connect(self._copy_from_other)
        self.film.setToolTip("中クリック：その写真の調整を今の写真にコピー")
        dv.addWidget(self.film)

        self.stack = QStackedWidget()
        self.stack.addWidget(lib)
        self.stack.addWidget(dev)
        central = QWidget()
        cv = QVBoxLayout(central)
        cv.setContentsMargins(0, 0, 0, 0)
        cv.setSpacing(0)
        cv.addWidget(top)
        cv.addWidget(self.stack, 1)
        self.setCentralWidget(central)

        # ドック：スコープ・調整・プリセット/履歴
        self.scopes = ScopesPanel()
        self.scopes.hoverRange.connect(self.viewer.set_mask)
        self.scopes.zoneDrag.connect(self._zone_drag)
        self.dock_scopes = QDockWidget("スコープ", self)
        self.dock_scopes.setObjectName("dock_scopes")
        self.dock_scopes.setWidget(self.scopes)
        self.panel = DevelopPanel()
        self.panel.paramsChanged.connect(self._params_changed)
        self.panel.autoRequested.connect(self.auto_adjust)
        self.panel.hoverKey.connect(self._hover_key)
        self.panel.cropToolRequested.connect(self._crop_tool)
        self.panel.aspectChanged.connect(self._aspect_changed)
        self.panel.wb_pick.toggled.connect(self._picking)
        self.dock_panel = QDockWidget("調整", self)
        self.dock_panel.setObjectName("dock_panel")
        self.dock_panel.setWidget(self.panel)
        self.dock_left = QDockWidget("プリセット・履歴", self)
        self.dock_left.setObjectName("dock_left")
        self.dock_left.setWidget(self._build_left())
        for d in (self.dock_scopes, self.dock_panel, self.dock_left):
            d.setFeatures(QDockWidget.DockWidgetMovable | QDockWidget.DockWidgetFloatable)
        self.addDockWidget(Qt.RightDockWidgetArea, self.dock_scopes)
        self.addDockWidget(Qt.RightDockWidgetArea, self.dock_panel)
        self.splitDockWidget(self.dock_scopes, self.dock_panel, Qt.Vertical)
        self.addDockWidget(Qt.LeftDockWidgetArea, self.dock_left)
        self.resizeDocks([self.dock_scopes, self.dock_panel], [260, 700], Qt.Vertical)
        self.resizeDocks([self.dock_panel], [400], Qt.Horizontal)
        self.resizeDocks([self.dock_left], [230], Qt.Horizontal)
        self.dock_panel.setMinimumWidth(340)
        st = self.settings.value("win/state")
        if st is not None:
            self.restoreState(st)
        self.statusBar().showMessage("フォルダを開いてください（Ctrl+O）")

    def _build_left(self):
        tabs = QTabWidget()
        # プリセット
        pw = QWidget()
        pv = QVBoxLayout(pw)
        pv.setContentsMargins(4, 4, 4, 4)
        self.preset_list = QListWidget()
        self.preset_list.itemDoubleClicked.connect(self._apply_preset)
        self.preset_list.setToolTip("ダブルクリックで適用（選択中の写真すべてに）")
        pv.addWidget(self.preset_list, 1)
        h = QHBoxLayout()
        b1 = QPushButton("保存…")
        b1.clicked.connect(self._save_preset)
        b2 = QPushButton("削除")
        b2.clicked.connect(self._delete_preset)
        h.addWidget(b1)
        h.addWidget(b2)
        pv.addLayout(h)
        tabs.addTab(pw, "プリセット")
        # 履歴
        self.history_list = QListWidget()
        self.history_list.itemClicked.connect(self._history_jump)
        tabs.addTab(self.history_list, "履歴")
        # スナップショット
        sw = QWidget()
        sv = QVBoxLayout(sw)
        sv.setContentsMargins(4, 4, 4, 4)
        self.snap_list = QListWidget()
        self.snap_list.itemDoubleClicked.connect(self._apply_snapshot)
        self.snap_list.setToolTip("ダブルクリックでその時点の調整に戻す")
        sv.addWidget(self.snap_list, 1)
        b = QPushButton("今の状態を保存")
        b.clicked.connect(self._add_snapshot)
        sv.addWidget(b)
        tabs.addTab(sw, "スナップ")
        self._reload_presets()
        return tabs

    def _build_menus(self):
        mb = self.menuBar()

        def act(menu, text, slot, key=None):
            a = QAction(text, self)
            if key:
                a.setShortcut(QKeySequence(key))
            a.triggered.connect(slot)
            menu.addAction(a)
            return a

        m = mb.addMenu("ファイル")
        act(m, "フォルダを開く…", self.choose_folder, "Ctrl+O")
        act(m, "書き出し…", self.export_dialog, "Ctrl+Shift+E")
        m.addSeparator()
        act(m, "環境設定…", self.settings_dialog, "Ctrl+,")
        act(m, "終了", self.close, "Ctrl+Q")
        m = mb.addMenu("編集")
        act(m, "元に戻す", self.undo, "Ctrl+Z")
        act(m, "やり直す", self.redo, "Ctrl+Y")
        m.addSeparator()
        act(m, "調整をコピー", self.copy_settings, "Ctrl+Shift+C")
        act(m, "調整をペースト（選択中の写真へ）", self.paste_settings, "Ctrl+Shift+V")
        act(m, "自動調整", lambda: self.auto_adjust(self.panel.auto_strength.value()), "Ctrl+U")
        act(m, "すべての調整をリセット", self.reset_all, "Ctrl+Shift+R")
        m = mb.addMenu("写真")
        for i in range(6):
            act(m, f"★ {i}" if i else "★ なし", lambda _=False, v=i: self.set_rating(v))
        m.addSeparator()
        for lb in LABELS:
            act(m, f"ラベル：{LABEL_NAMES[lb]}", lambda _=False, v=lb: self.set_label(v))
        act(m, "ラベルなし", lambda: self.set_label(None))
        m.addSeparator()
        act(m, "採用 (P)", lambda: self.set_flag(1))
        act(m, "除外 (X)", lambda: self.set_flag(-1))
        act(m, "フラグなし (U)", lambda: self.set_flag(0))
        m.addSeparator()
        act(m, "除外した写真をごみ箱へ…", self.trash_rejected)
        act(m, "書き出したファイル／元の写真を表示", self.show_links)
        m = mb.addMenu("表示")
        act(m, "ライブラリ", lambda: self.set_mode("library"))
        act(m, "現像", lambda: self.set_mode("develop"))
        act(m, "全画面で選別 (F)", self.toggle_fullscreen)
        m.addSeparator()
        act(m, "パネル配置：右に縦並び（Lightroom 型）", lambda: self.layout_preset("right"))
        act(m, "パネル配置：下に横並び（Resolve 型）", lambda: self.layout_preset("bottom"))
        act(m, "スコープを別ウィンドウにする", lambda: self.dock_scopes.setFloating(not self.dock_scopes.isFloating()))
        m = mb.addMenu("ヘルプ")
        act(m, "操作一覧", self.show_keys, "F1")

    def _build_shortcuts(self):
        def sc(key, fn):
            s = QShortcut(QKeySequence(key), self)
            s.activated.connect(fn)
            return s

        for i in range(6):
            sc(str(i), lambda v=i: self.set_rating(v))
        for i, lb in enumerate(["red", "yellow", "green", "blue"]):
            sc(str(6 + i), lambda v=lb: self.set_label(v, toggle=True))
        sc("[", lambda: self.bump_rating(-1))
        sc("]", lambda: self.bump_rating(1))
        sc("P", lambda: self.set_flag(1))
        sc("X", lambda: self.set_flag(-1))
        sc("U", lambda: self.set_flag(0))
        sc("G", lambda: self.set_mode("library"))
        sc("D", lambda: self.set_mode("develop"))
        sc("Return", self._enter)
        sc("Enter", self._enter)
        sc("Right", lambda: self.step_photo(1))
        sc("Left", lambda: self.step_photo(-1))
        sc("Shift+D", lambda: self.tb_bypass.toggle())
        sc("\\", lambda: self.tb_bypass.toggle())
        sc("Y", lambda: self.tb_wipe.toggle())
        sc("J", lambda: self.tb_clip.toggle())
        sc("Z", lambda: self.viewer.toggle_zoom())
        sc("R", lambda: self.panel.crop_btn.toggle() if self.stack.currentIndex() == 1 else None)
        sc("W", lambda: self.panel.wb_pick.toggle() if self.stack.currentIndex() == 1 else None)
        sc("F", self.toggle_fullscreen)
        sc("Escape", self._escape)
        sc("Ctrl+E", lambda: self.set_mode("develop"))

    # ================================================================ mode / layout
    def set_mode(self, mode):
        dev = mode == "develop"
        if dev and not self.current_path():
            ix = self.proxy.index(0, 0)
            if not ix.isValid():
                self.statusBar().showMessage("写真がありません。フォルダを開いてください", 5000)
                dev = False
            else:
                self.grid.setCurrentIndex(ix)
        self.stack.setCurrentIndex(1 if dev else 0)
        self.btn_lib.setChecked(not dev)
        self.btn_dev.setChecked(dev)
        for d in (self.dock_scopes, self.dock_panel, self.dock_left):
            d.setVisible(dev)
        if dev:
            p = self.current_path()
            if p and (self.session is None or self.session.path != p):
                self.open_photo(p)
            self.viewer.setFocus()
            cur = self.grid.currentIndex()
            if cur.isValid():
                self.film.scrollTo(cur)
        else:
            self.grid.setFocus()

    def layout_preset(self, where):
        if where == "bottom":
            for d in (self.dock_scopes, self.dock_panel):
                self.removeDockWidget(d)
                self.addDockWidget(Qt.BottomDockWidgetArea, d)
                d.show()
            self.splitDockWidget(self.dock_scopes, self.dock_panel, Qt.Horizontal)
            self.resizeDocks([self.dock_scopes], [330], Qt.Vertical)
        else:
            for d in (self.dock_scopes, self.dock_panel):
                self.removeDockWidget(d)
                self.addDockWidget(Qt.RightDockWidgetArea, d)
                d.show()
            self.splitDockWidget(self.dock_scopes, self.dock_panel, Qt.Vertical)
            self.resizeDocks([self.dock_panel], [380], Qt.Horizontal)

    def toggle_fullscreen(self):
        if self.isFullScreen():
            self.showNormal()
            self.menuBar().show()
            self.film.show()
            self.centralWidget().layout().itemAt(0).widget().show()
            self.set_mode("develop")
        else:
            if self.stack.currentIndex() == 0:
                self.set_mode("develop")
            for d in (self.dock_scopes, self.dock_panel, self.dock_left):
                d.hide()
            self.menuBar().hide()
            self.film.hide()
            self.centralWidget().layout().itemAt(0).widget().hide()
            self.showFullScreen()
            self.statusBar().showMessage("全画面選別：0〜5 で★、6〜9 でラベル、P/X で採用/除外、←→ で移動、Esc で戻る", 6000)

    def _enter(self):
        if self.stack.currentIndex() == 0:
            self.set_mode("develop")
        elif self.viewer.crop_mode:
            self._crop_changed(dict(self.viewer.crop), True)

    def _escape(self):
        if self.viewer.crop_mode:
            self.panel.crop_btn.setChecked(False)
            if self._crop_backup is not None and self.session and self.session.state:
                p = copy.deepcopy(self.session.state.params)
                p["crop"] = self._crop_backup
                self._apply_params(p, final=True, desc="切り抜きを取消")
        elif self.isFullScreen():
            self.toggle_fullscreen()

    # ================================================================ folder
    def choose_folder(self):
        d = QFileDialog.getExistingDirectory(self, "写真のフォルダ", self.folder or os.path.expanduser("~"))
        if d:
            self.open_folder(d)

    def open_folder(self, folder):
        folder = os.path.normpath(folder)
        self.folder = folder
        self.settings.setValue("library/folder", folder)
        self.tree.reveal(folder)
        self.title.setText(folder)
        self.statusBar().showMessage("読み込み中…")
        self._load_token += 1
        tok = self._load_token
        self.pool.submit(self._scan_folder, tok, folder)

    def _scan_folder(self, tok, folder):
        try:
            names = sorted(os.listdir(folder), key=str.lower)
        except OSError as e:
            self.em.failed.emit(f"フォルダを開けません：{e}")
            return
        items = []
        for n in names:
            p = os.path.join(folder, n)
            if not os.path.isfile(p) or not decode.is_supported(p):
                continue
            try:
                mt = os.path.getmtime(p)
            except OSError:
                continue
            it = PhotoItem(p, mtime=mt)
            st = PhotoState.from_sidecar(p)
            if os.path.exists(p + ".rdev"):
                it.rating, it.label, it.flag = st.rating, st.label, st.flag
                it.edited = st.params is not None
            else:
                c = self.catalog.get(p)
                if c:
                    it.rating, it.label, it.flag = c["rating"], c["label"], c["flag"]
                else:
                    r, lb = xmp.read_file_rating(p)
                    it.rating = max(0, r or 0)
                    it.label = lb
            it.exports = len([x for x in self.catalog.exports_of(p) if os.path.exists(x)])
            src = self.catalog.source_of(p)
            if src:
                it.is_export = True
                it.missing_link = not os.path.exists(src)
            items.append(it)
        self.em.folderLoaded.emit(tok, items)

    def _folder_loaded(self, tok, items):
        if tok != self._load_token:
            return
        self.session = None
        self.states.clear()
        self.model.set_items(items)
        self.proxy.refilter()
        self._update_count()
        self.statusBar().showMessage(f"{len(items)} 枚を読み込みました", 4000)
        if items:
            self.grid.setCurrentIndex(self.proxy.index(0, 0))

    def _update_count(self):
        self.filter_bar.count.setText(self.filter_bar.describe())

    # ================================================================ selection
    def current_path(self):
        ix = self.grid.currentIndex()
        if not ix.isValid():
            return None
        return ix.data(PhotoModel.ItemRole).path

    def selected_paths(self):
        rows = self.grid.selectionModel().selectedIndexes()
        paths = [ix.data(PhotoModel.ItemRole).path for ix in rows]
        cur = self.current_path()
        if not paths and cur:
            paths = [cur]
        return paths

    def filtered_paths(self):
        return [self.proxy.index(i, 0).data(PhotoModel.ItemRole).path for i in range(self.proxy.rowCount())]

    def _grid_current(self, cur, prev):
        if not cur.isValid():
            return
        it: PhotoItem = cur.data(PhotoModel.ItemRole)
        self._show_lib_info(it)
        if self.stack.currentIndex() == 1:
            self.open_photo(it.path)

    def _show_lib_info(self, it: PhotoItem):
        def work():
            meta = decode.read_metadata(it.path)
            arr = decode.thumbnail_rgb8(it.path, 480)
            return meta, arr

        fut = self.pool.submit(work)

        def done(f):
            try:
                meta, arr = f.result()
            except Exception:
                return
            self.em.rendered.emit(-1, "libinfo", (it.path, meta, arr))

        fut.add_done_callback(done)

    def step_photo(self, d):
        ix = self.grid.currentIndex()
        r = (ix.row() if ix.isValid() else -1) + d
        if 0 <= r < self.proxy.rowCount():
            nix = self.proxy.index(r, 0)
            self.grid.selectionModel().setCurrentIndex(nix, QItemSelectionModel.ClearAndSelect)
            self.grid.scrollTo(nix)
            self.film.scrollTo(nix)

    # ================================================================ photo
    def get_state(self, path) -> PhotoState:
        st = self.states.get(path)
        if st is None:
            st = PhotoState.from_sidecar(path)
            if not os.path.exists(path + ".rdev"):
                it = self.model.item(path)
                if it:
                    st.rating, st.label, st.flag = it.rating, it.label, it.flag
            self.states[path] = st
        return st

    def open_photo(self, path):
        if self.session and self.session.path == path:
            return
        self._flush_save()
        self.panel.crop_btn.setChecked(False)
        self.session = Session(path)
        self.session.state = self.get_state(path)
        self.viewer.set_image(None)
        self.viewer.message = "読み込み中…"
        self.viewer.update()
        self._load_token += 1
        tok = self._load_token
        self.pool.submit(self._load_photo, tok, path)

    def _load_photo(self, tok, path):
        try:
            t0 = time.time()
            src = decode.load(path, full=False)
            prev_size = int(QSettings().value("render/preview", 1600))
            preview = decode.downscale(src.linear, prev_size)
            full_w = src.meta.get("width") or src.linear.shape[1]
            scale = preview.shape[1] / full_w
            info = None
            meta = src.meta
            db = LensDB.get()
            lens_txt = ""
            if db.ok:
                lens, cam, crop, conf = db.identify(meta)
                if lens is not None:
                    info = {"lens_key": LensDB.key(lens), "crop": crop, "focal": meta.get("focal"),
                            "aperture": meta.get("aperture"), "auto": True}
                    lens_txt = (f"検出：{lens.maker} {lens.model}（Lensfun）" +
                                ("" if conf >= 0.5 else "　※一致度が低いので確認してください"))
                elif meta.get("lens_model"):
                    lens_txt = f"プロファイル未検出：「{meta['lens_model']}」。下の一覧から選んでください"
                else:
                    lens_txt = "EXIF にレンズ情報がありません。下の一覧から選べます"
                info = info or {"lens_key": None, "crop": crop, "focal": meta.get("focal"),
                                "aperture": meta.get("aperture"), "auto": True}
            self.em.photoLoaded.emit(tok, (path, src, preview, scale, info, lens_txt, time.time() - t0))
        except Exception as e:
            traceback.print_exc()
            self.em.failed.emit(f"読み込めませんでした：{os.path.basename(path)}（{e}）")

    def _photo_loaded(self, tok, payload):
        path, src, preview, scale, info, lens_txt, dt = payload
        s = self.session
        if s is None or s.path != path:
            return
        s.src, s.preview, s.scale = src, preview, scale
        s.lens_info = info
        s.as_shot_tt = color.fit_temp_tint(src.as_shot_gains) if src.kind == "raw" else (6500.0, 0.0)
        st = s.state
        if st.params is None:
            st.params = default_params()
            if src.kind != "raw":
                st.params["sharpen"]["amount"] = 0.0
                st.params["nr"]["color"] = 0.0
                st.params["lens"]["profile"] = False
        if not st.history:
            st.history = [("読み込み", copy.deepcopy(st.params))]
            st.history_pos = 0
        self._refresh_history()
        self._refresh_snapshots()
        self.panel.set_params(st.params, s.as_shot_tt)
        self.panel.set_lens_detect(lens_txt, (info or {}).get("lens_key"))
        self.viewer.message = ""
        self.meta_label.setText(f"{os.path.basename(path)}    {decode.meta_line(src.meta)}")
        self._before_key = None
        self._params_version += 1
        self.request_render()
        self.statusBar().showMessage(f"{os.path.basename(path)} を開きました（{dt:.1f} 秒）", 3000)

    # ================================================================ render
    def _lens_info_for(self, p):
        s = self.session
        info = dict(s.lens_info or {})
        if p["lens"].get("lens"):
            info["lens_key"] = p["lens"]["lens"]
        return info if info.get("lens_key") else None

    def request_render(self):
        s = self.session
        if s is None or s.preview is None:
            return
        if self._render_busy:
            self._render_pending = True
            return
        self._render_busy = True
        p = copy.deepcopy(s.state.params)
        crop_mode = self.viewer.crop_mode
        detail = not self._dragging
        ver = self._params_version
        need_before = self._before_key != self._geom_key(p)
        args = (s.path, s.preview, p, s.src.as_shot_gains, self._lens_info_for(p), s.scale, detail, crop_mode, ver,
                need_before)
        self.pool.submit(self._render_job, *args)

    @staticmethod
    def _geom_key(p):
        return json.dumps([p["lens"], p["crop"]], sort_keys=True)

    def _render_job(self, path, lin, p, as_shot, info, scale, detail, crop_mode, ver, need_before):
        try:
            g = pipeline.develop(lin, p, as_shot, info, scale=scale, detail=detail, do_crop=not crop_mode)
            arr = pipeline.to_uint8(g)
            before = None
            if need_before:
                b = default_params()
                b["lens"], b["crop"] = p["lens"], p["crop"]
                b["sharpen"]["amount"] = 0.0
                b["nr"]["color"] = 0.0
                b["lens"] = dict(p["lens"], profile=False, manual_distortion=0.0, manual_vignetting=0.0)
                before = pipeline.to_uint8(pipeline.develop(lin, b, as_shot, None, scale=scale, detail=False,
                                                            do_crop=not crop_mode))
                if before.shape != arr.shape:
                    import cv2
                    before = cv2.resize(before, (arr.shape[1], arr.shape[0]), interpolation=cv2.INTER_AREA)
            self.em.rendered.emit(ver, "preview", (path, arr, scale, before, self._geom_key(p) if need_before else None))
        except Exception as e:
            traceback.print_exc()
            self.em.rendered.emit(ver, "error", str(e))

    def _rendered(self, ver, kind, payload):
        if kind == "libinfo":
            path, meta, arr = payload
            if self.current_path() == path:
                self.lib_hist.data = None
                if arr is not None:
                    from .scopes import ScopeData
                    self.lib_hist.data = ScopeData(arr)
                self.lib_hist.update()
                rows = [("ファイル", os.path.basename(path))]
                for k, lab in [("make", "メーカー"), ("model", "カメラ"), ("lens_model", "レンズ"), ("datetime", "撮影日時")]:
                    if meta.get(k):
                        rows.append((lab, str(meta[k])))
                ml = decode.meta_line(meta)
                if ml:
                    rows.append(("撮影条件", ml))
                it = self.model.item(path)
                if it:
                    rows.append(("評価", "★" * it.rating + ("　" + LABEL_NAMES.get(it.label, "") if it.label else "")))
                    if it.exports:
                        rows.append(("書き出し", f"{it.exports} 件"))
                    if it.is_export:
                        src = self.catalog.source_of(path)
                        rows.append(("元の写真", os.path.basename(src) + ("（見つかりません）" if it.missing_link else "")))
                self.lib_info.setText("<br>".join(f"<span style='color:{theme.TEXT_DIM}'>{a}</span>　{b}" for a, b in rows))
            return
        if kind == "hires":
            path, arr, key = payload
            self._hires_busy = False
            s = self.session
            self.viewer.message = ""
            if arr is None:
                self.viewer.update()
                return
            if s and s.path == path and key == self._hires_key_now() and not self.viewer.fit:
                self._show(arr, getattr(s, "_hires_scale", None), hires=True)
            elif s and not self.viewer.fit:
                self._hires_key = None
                self._request_hires()
            else:
                self.viewer.update()
            return
        self._render_busy = False
        if kind == "error":
            self.statusBar().showMessage(f"現像エラー：{payload}", 8000)
        else:
            path, arr, scale, before, bkey = payload
            s = self.session
            if s and s.path == path:
                s.last_arr = arr
                if before is not None:
                    self.viewer.set_before(before)
                    self._before_key = bkey
                self._show(arr, scale)
                if not self._dragging and ver == self._params_version and not self.viewer.crop_mode:
                    self._update_thumb(path, arr)
                if not self.viewer.fit and ver == self._params_version and not self._dragging:
                    self._request_hires()
        if self._render_pending:
            self._render_pending = False
            self.request_render()

    def _show(self, arr, scale, hires=False):
        h, w = arr.shape[:2]
        sc = scale if scale else 1.0
        full = (round(w / sc), round(h / sc))
        self.viewer.set_image(arr, full_size=full)
        d = self.scopes.set_image(arr)
        if d is not None:
            self.panel.set_histogram(d.hist_l)
        self.viewer.info = f"{full[0]}×{full[1]}" + ("　高精細" if hires else "")

    # ---- 高精細（ズーム時）
    def _hires_key_now(self):
        s = self.session
        if not s or not s.state:
            return None
        return (s.path, self._params_version)

    def _request_hires(self):
        s = self.session
        if s is None or s.src is None or self._hires_busy or self._dragging or self.viewer.crop_mode:
            return
        key = self._hires_key_now()
        if key == self._hires_key:
            return
        self._hires_key = key
        self._hires_busy = True
        p = copy.deepcopy(s.state.params)
        info = self._lens_info_for(p)
        limit = int(QSettings().value("render/hires", 6000))
        self.viewer.message = "高精細表示を準備中…"
        self.viewer.update()

        def job():
            try:
                if s.full is None:
                    s.full = decode.load(s.path, full=True, as_shot_gains=s.src.as_shot_gains, meta=s.src.meta)
                lin = s.full.linear if not limit else decode.downscale(s.full.linear, limit)
                sc = lin.shape[1] / (s.src.meta.get("width") or s.full.linear.shape[1])
                g = pipeline.develop(lin, p, s.src.as_shot_gains, info, scale=sc, detail=True)
                arr = pipeline.to_uint8(g)
                s._hires_scale = sc
                self.em.rendered.emit(0, "hires", (s.path, arr, key))
            except Exception as e:
                traceback.print_exc()
                self.em.rendered.emit(0, "hires", (s.path, None, None))

        self.pool.submit(job)

    # ================================================================ params
    def _params_changed(self, p, final, desc):
        s = self.session
        if s is None or s.state is None:
            return
        if desc == "crop.angle":
            self._fit_crop_to_angle(p)
            if self.viewer.crop_mode:
                self.viewer.crop = {k: p["crop"][k] for k in ("x", "y", "w", "h")}
            desc = "角度"
        self._apply_params(p, final, desc)

    def _fit_crop_to_angle(self, p):
        """回転で四隅に空白が出ないよう、切り抜き枠を収まる大きさに縮める。"""
        import math
        s = self.session
        H, W = s.preview.shape[:2]
        wr, hr = pipeline.rotated_rect_max_area(W, H, math.radians(abs(p["crop"]["angle"])))
        mw, mh = min(1.0, wr / W), min(1.0, hr / H)
        c = p["crop"]
        if c["w"] > mw + 1e-4 or c["h"] > mh + 1e-4 or c["x"] < (1 - mw) / 2 - 1e-4 or c["y"] < (1 - mh) / 2 - 1e-4 \
                or c["x"] + c["w"] > (1 + mw) / 2 + 1e-4 or c["y"] + c["h"] > (1 + mh) / 2 + 1e-4:
            w, h = min(c["w"], mw), min(c["h"], mh)
            c.update(x=(1 - w) / 2, y=(1 - h) / 2, w=w, h=h)

    def _apply_params(self, p, final, desc, update_panel=False):
        s = self.session
        st = s.state
        if not final and not self._dragging:
            self._dragging = True
            self.scopes.begin_ghost()
        st.params = p
        self.panel.p = copy.deepcopy(p)   # パネル側の控えも常に同じにする
        self._params_version += 1
        if final:
            self._dragging = False
            self.scopes.end_ghost()
            self._push_history(desc)
            self.save_timer.start(400)
            self.viewer.message = ""
        if update_panel:
            self.panel.set_params(p, s.as_shot_tt)
        self.request_render()

    def _push_history(self, desc):
        st = self.session.state
        if st.history and st.history[st.history_pos][1] == st.params:
            return
        st.history = st.history[: st.history_pos + 1]
        st.history.append((desc, copy.deepcopy(st.params)))
        st.history_pos = len(st.history) - 1
        self._refresh_history()

    def _refresh_history(self):
        self.history_list.clear()
        st = self.session.state if self.session else None
        if not st:
            return
        for i, (d, _) in enumerate(st.history):
            it = QListWidgetItem(d)
            if i > st.history_pos:
                it.setForeground(Qt.gray)
            self.history_list.addItem(it)
        self.history_list.setCurrentRow(st.history_pos)

    def _history_jump(self, item):
        st = self.session.state if self.session else None
        if not st:
            return
        i = self.history_list.row(item)
        st.history_pos = i
        st.params = copy.deepcopy(st.history[i][1])
        self._params_version += 1
        self.viewer.crop = {k: st.params["crop"][k] for k in ("x", "y", "w", "h")}
        self.panel.set_params(st.params, self.session.as_shot_tt)
        self._refresh_history()
        self.save_timer.start(400)
        self.request_render()

    def undo(self):
        st = self.session.state if self.session else None
        if st and st.history_pos > 0:
            self._history_jump(self.history_list.item(st.history_pos - 1))

    def redo(self):
        st = self.session.state if self.session else None
        if st and st.history_pos < len(st.history) - 1:
            self._history_jump(self.history_list.item(st.history_pos + 1))

    def reset_all(self):
        if self.session and self.session.state:
            self._apply_params(default_params(), True, "すべてリセット", update_panel=True)

    def auto_adjust(self, strength):
        s = self.session
        if s is None or s.preview is None:
            return
        base = copy.deepcopy(s.state.params)
        if strength < 0:   # WB だけ
            p = auto.auto_params(s.preview, s.src.as_shot_gains, 100.0, base)
            base["wb"] = p["wb"]
            self._apply_params(base, True, "WB：自動", update_panel=True)
            return
        p = auto.auto_params(s.preview, s.src.as_shot_gains, strength, base)
        self._apply_params(p, True, f"自動調整（{strength:.0f}%）", update_panel=True)
        self.statusBar().showMessage("自動調整しました。各スライダーで調整を続けられます", 4000)

    def _zone_drag(self, key, dv, final):
        if self.session and self.session.state:
            self.panel.nudge_zone(key, dv, final)

    def _hover_key(self, key):
        zone = key if key in ("blacks", "shadows", "exposure", "highlights", "whites", "easy_bright") else None
        if zone == "easy_bright":
            zone = "exposure"
        self.scopes.set_zone_highlight(zone)

    def _probe(self, rgb):
        self.scopes.set_probe(rgb)

    # ---- WB スポイト
    def _picking(self, on):
        self.viewer.picking = on
        self.viewer.setCursor(Qt.CrossCursor if on else Qt.ArrowCursor)

    def _wb_pick(self, x, y):
        s = self.session
        if s is None or s.last_arr is None:
            return
        a = s.last_arr
        h, w = a.shape[:2]
        patch = a[max(0, y - 3):min(h, y + 4), max(0, x - 3):min(w, x + 4)].reshape(-1, 3).astype(np.float32) / 255
        out = patch.mean(0)
        if out.max() > 0.98 or out.max() < 0.03:
            self.statusBar().showMessage("明るすぎる／暗すぎる場所です。白〜グレーの部分をクリックしてください", 5000)
            return
        p = s.state.params
        lut = pipeline.tone_lut(p)[0]
        xs = np.linspace(0, pipeline.LUT_MAX, pipeline.LUT_N)
        g_in = np.interp(out, lut, xs)
        lin = color.decode(g_in)
        cur = pipeline.wb_for(p, s.src.as_shot_gains)
        new = cur * (lin[1] / np.maximum(lin, 1e-5))
        t, tint = color.fit_temp_tint(new)
        self.panel.wb_pick.setChecked(False)
        self.panel.set_wb(t, tint, "WB：スポイト")

    # ---- 切り抜き
    def _crop_tool(self, on):
        s = self.session
        if s is None or s.state is None:
            return
        self.viewer.crop_mode = on
        if on:
            self._crop_backup = copy.deepcopy(s.state.params["crop"])
            self.viewer.crop = {k: s.state.params["crop"][k] for k in ("x", "y", "w", "h")}
            self._aspect_changed(self.panel.aspect.currentData(), apply=False)
            self.viewer.setFocus()
        else:
            self._crop_backup = None
        self._params_version += 1
        self.request_render()

    def _aspect_changed(self, val, apply=True):
        s = self.session
        if s is None or s.src is None:
            return
        if val == "orig":
            w, h = s.src.meta.get("width", 3), s.src.meta.get("height", 2)
            a = w / h
        else:
            a = val
        self.viewer.crop_aspect = a
        if s.state:
            s.state.params["crop"]["aspect"] = "free" if val is None else val
            self.panel.p["crop"]["aspect"] = s.state.params["crop"]["aspect"]
        if a and apply:
            # 今の枠の中心で比率を合わせる
            c = dict(self.viewer.crop)
            fw, fh = self.viewer.full_size
            na = a * fh / fw
            w, h = c["w"], c["h"]
            if w / h > na:
                w = h * na
            else:
                h = w / na
            cx, cy = c["x"] + c["w"] / 2, c["y"] + c["h"] / 2
            c.update(x=max(0, cx - w / 2), y=max(0, cy - h / 2), w=w, h=h)
            self.viewer.crop = c
            self.viewer.update()
            if not self.viewer.crop_mode:
                self._crop_changed(c, True)

    def _crop_changed(self, c, final):
        s = self.session
        if s is None or s.state is None:
            return
        p = copy.deepcopy(s.state.params)
        p["crop"].update({k: float(c[k]) for k in ("x", "y", "w", "h")})
        if final:
            self.panel.crop_btn.setChecked(False)
            self._crop_backup = None
            self._apply_params(p, True, "切り抜き")
        else:
            s.state.params = p
            self.panel.p = copy.deepcopy(p)

    # ================================================================ save / thumbs
    def _save_current(self):
        s = self.session
        if s is None or s.state is None:
            return
        self._save_state(s.path, s.state, edited=True)

    def _flush_save(self):
        if self.save_timer.isActive():
            self.save_timer.stop()
            self._save_current()

    def _save_state(self, path, st: PhotoState, edited=None):
        is_raw = decode.is_raw(path)
        has_params = st.params is not None and (edited or os.path.exists(path + ".rdev"))
        try:
            if is_raw or has_params:
                save_sidecar(path, st.to_json())
            self.catalog.upsert(path, rating=st.rating, label=st.label, flag=st.flag,
                                edited=bool(has_params) if edited is not None else None)
        except OSError as e:
            self.statusBar().showMessage(f"保存できませんでした：{e}", 8000)
        it = self.model.item(path)
        if it:
            it.rating, it.label, it.flag = st.rating, st.label, st.flag
            if edited:
                it.edited = True
            self.model.refresh(path)

    def _update_thumb(self, path, arr):
        h, w = arr.shape[:2]
        s = 320 / max(h, w)
        import cv2
        small = cv2.resize(arr, (max(1, int(w * s)), max(1, int(h * s))), interpolation=cv2.INTER_AREA)
        img = to_qimage(small)
        self.model.set_thumb(path, img)
        self._pending_thumb = (path, img)
        QTimer.singleShot(800, self._write_thumb)

    def _write_thumb(self):
        pt = getattr(self, "_pending_thumb", None)
        if not pt:
            return
        path, img = pt
        ep = edited_thumb_path(path)
        if ep:
            img.save(ep, "JPG", 85)
        self._pending_thumb = None

    # ================================================================ rating / label / flag
    def _targets(self):
        if self.stack.currentIndex() == 1 and self.session:
            sel = self.selected_paths()
            return sel if len(sel) > 1 else [self.session.path]
        return self.selected_paths()

    def set_rating(self, v):
        self._rate(lambda st: setattr(st, "rating", v), f"★{v}" if v else "★なし")

    def bump_rating(self, d):
        self._rate(lambda st: setattr(st, "rating", max(0, min(5, st.rating + d))), "★を変更")

    def set_label(self, v, toggle=False):
        def f(st):
            st.label = None if (toggle and st.label == v) else v
        self._rate(f, f"ラベル：{LABEL_NAMES.get(v, 'なし')}")

    def set_flag(self, v):
        self._rate(lambda st: setattr(st, "flag", v), {1: "採用", -1: "除外", 0: "フラグなし"}[v])

    def _rate(self, fn, desc):
        paths = self._targets()
        if not paths:
            return
        write_files = QSettings().value("rate/write_files", True, type=bool)
        xmp_raw = QSettings().value("rate/xmp_sidecar", False, type=bool)
        propagate = QSettings().value("rate/propagate", "ask")
        touched_exports = []
        for p in paths:
            st = self.get_state(p)
            before = (st.rating, st.label)
            fn(st)
            self._save_state(p, st)
            try:
                if not decode.is_raw(p) and write_files and before != (st.rating, st.label):
                    xmp.write_file_rating(p, st.rating, st.label)
                elif decode.is_raw(p) and xmp_raw:
                    xmp.write_sidecar(p, st.rating, st.label)
            except Exception as e:
                self.statusBar().showMessage(f"ファイルへの書き込みに失敗：{os.path.basename(p)}（{e}）", 8000)
            if before != (st.rating, st.label):
                ex = [x for x in self.catalog.exports_of(p) if os.path.exists(x)]
                if ex:
                    touched_exports.append((p, st, ex))
        if touched_exports and propagate != "never":
            n = sum(len(x[2]) for x in touched_exports)
            ok = propagate == "always" or QMessageBox.question(
                self, "書き出したファイルにも反映",
                f"この写真から書き出したファイルが {n} 件あります。★・ラベルを書き出したファイルにも反映しますか？"
            ) == QMessageBox.Yes
            if ok:
                for _, st, ex in touched_exports:
                    for e in ex:
                        try:
                            xmp.write_file_rating(e, st.rating, st.label)
                            est = self.get_state(e)
                            est.rating, est.label = st.rating, st.label
                            self._save_state(e, est)
                        except Exception as err:
                            self.statusBar().showMessage(f"反映できませんでした：{os.path.basename(e)}（{err}）", 8000)
        self.statusBar().showMessage(f"{desc}（{len(paths)} 枚）", 2500)
        cur = self.grid.currentIndex()
        if cur.isValid():
            self._show_lib_info(cur.data(PhotoModel.ItemRole))
        if self.proxy.min_rating or self.proxy.labels or self.proxy.flag != "all":
            pass  # 絞り込み中でも、付けた直後に消えると戸惑うのでその場では再絞り込みしない
        if QSettings().value("rate/auto_advance", False, type=bool) and len(paths) == 1:
            self.step_photo(1)

    def trash_rejected(self):
        rej = [it.path for it in self.model.items if it.flag == -1]
        if not rej:
            QMessageBox.information(self, "除外した写真", "除外フラグの付いた写真はありません")
            return
        if QMessageBox.question(self, "ごみ箱へ移動",
                                f"除外フラグの付いた {len(rej)} 枚をごみ箱へ移動します。よろしいですか？\n"
                                "（サイドカー .rdev / .xmp も一緒に移動します）") != QMessageBox.Yes:
            return
        from PySide6.QtCore import QFile
        n = 0
        for p in rej:
            ok = QFile.moveToTrash(p)
            if ok:
                n += 1
                for side in (p + ".rdev", xmp.sidecar_path(p)):
                    if os.path.exists(side):
                        QFile.moveToTrash(side)
        self.statusBar().showMessage(f"{n} 枚をごみ箱へ移動しました", 5000)
        self.open_folder(self.folder)

    def show_links(self):
        p = self.current_path()
        if not p:
            return
        ex = self.catalog.exports_of(p)
        src = self.catalog.source_of(p)
        lines = []
        if src:
            lines.append(f"元の写真：{src}" + ("" if os.path.exists(src) else "（見つかりません）"))
        for e in ex:
            lines.append(("" if os.path.exists(e) else "【見つかりません】") + e)
        if not lines:
            QMessageBox.information(self, "紐付け", "この写真に紐付いたファイルはありません")
            return
        box = QMessageBox(self)
        box.setWindowTitle("紐付け")
        box.setText("\n".join(["書き出したファイル："] + lines if ex else lines))
        relink = box.addButton("見つからないファイルを再指定…", QMessageBox.ActionRole)
        box.addButton(QMessageBox.Close)
        box.exec()
        if box.clickedButton() is relink:
            for e in ex:
                if not os.path.exists(e):
                    f, _ = QFileDialog.getOpenFileName(self, f"「{os.path.basename(e)}」の新しい場所", self.folder or "")
                    if f:
                        self.catalog.relink(e, f)
            if src and not os.path.exists(src):
                f, _ = QFileDialog.getOpenFileName(self, f"元の写真「{os.path.basename(src)}」の場所", self.folder or "")
                if f:
                    self.catalog.remove_link(src, p)
                    self.catalog.add_link(f, p)

    # ================================================================ copy / paste / presets
    def copy_settings(self):
        s = self.session
        p = None
        if s and s.state and s.state.params:
            p = s.state.params
        else:
            cur = self.current_path()
            if cur:
                p = self.get_state(cur).params
        if p is None:
            return
        dlg = GroupPickDialog("コピーする項目", parent=self)
        if dlg.exec():
            self._clipboard = extract_groups(p, dlg.groups())
            self.statusBar().showMessage("調整をコピーしました", 2500)

    def paste_settings(self):
        if not self._clipboard:
            self.statusBar().showMessage("先に「調整をコピー」してください", 3000)
            return
        self._apply_partial(self._clipboard, "ペースト")

    def _apply_partial(self, partial, desc):
        paths = self._targets() if self.stack.currentIndex() == 1 else self.selected_paths()
        for p in paths:
            st = self.get_state(p)
            if self.session and self.session.path == p:
                newp = apply_groups(st.params or default_params(), partial)
                self._apply_params(newp, True, desc, update_panel=True)
            else:
                st.params = apply_groups(st.params or default_params(), partial)
                self._save_state(p, st, edited=True)
        self.statusBar().showMessage(f"{desc}：{len(paths)} 枚に適用しました", 3000)

    def _copy_from_other(self, path):
        """フィルムストリップの中クリック：その写真の調整を今の写真へ（Resolve 風）。"""
        s = self.session
        if s is None or path == s.path:
            return
        st = self.get_state(path)
        if st.params is None:
            self.statusBar().showMessage("その写真はまだ調整されていません", 3000)
            return
        partial = {k: v for k, v in st.params.items() if k not in ("crop",)}
        newp = apply_groups(s.state.params, partial)
        self._apply_params(newp, True, f"{os.path.basename(path)} からコピー", update_panel=True)

    def _reload_presets(self):
        self.preset_list.clear()
        for name in BUILTIN_PRESETS:
            it = QListWidgetItem(f"{name}")
            it.setData(Qt.UserRole, ("builtin", name))
            self.preset_list.addItem(it)
        for f in sorted(glob.glob(os.path.join(presets_dir(), "*.json"))):
            name = os.path.splitext(os.path.basename(f))[0]
            it = QListWidgetItem(f"★ {name}")
            it.setData(Qt.UserRole, ("user", f))
            self.preset_list.addItem(it)

    def _apply_preset(self, item):
        kind, ref = item.data(Qt.UserRole)
        if kind == "builtin":
            partial = copy.deepcopy(BUILTIN_PRESETS[ref])
        else:
            try:
                partial = json.load(open(ref, encoding="utf-8"))
            except Exception as e:
                QMessageBox.warning(self, "プリセット", f"読み込めませんでした：{e}")
                return
        self._apply_partial(partial, f"プリセット：{item.text().lstrip('★ ')}")

    def _save_preset(self):
        s = self.session
        if s is None or s.state is None:
            return
        dlg = GroupPickDialog("プリセットとして保存", parent=self, with_name=True)
        if dlg.exec() and dlg.name.text().strip():
            name = "".join(c for c in dlg.name.text().strip() if c not in '\\/:*?"<>|')
            data = extract_groups(s.state.params, dlg.groups())
            with open(os.path.join(presets_dir(), name + ".json"), "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            self._reload_presets()

    def _delete_preset(self):
        it = self.preset_list.currentItem()
        if not it:
            return
        kind, ref = it.data(Qt.UserRole)
        if kind != "user":
            QMessageBox.information(self, "プリセット", "最初から入っているプリセットは削除できません")
            return
        if QMessageBox.question(self, "プリセット", f"「{it.text()}」を削除しますか？") == QMessageBox.Yes:
            os.remove(ref)
            self._reload_presets()

    def _add_snapshot(self):
        s = self.session
        if s is None or s.state is None:
            return
        name, ok = QInputDialog.getText(self, "スナップショット", "名前", text=time.strftime("%H:%M:%S"))
        if ok:
            s.state.snapshots.append((name or "スナップショット", copy.deepcopy(s.state.params)))
            self._refresh_snapshots()
            self.save_timer.start(200)

    def _refresh_snapshots(self):
        self.snap_list.clear()
        if self.session and self.session.state:
            for n, _ in self.session.state.snapshots:
                self.snap_list.addItem(n)

    def _apply_snapshot(self, item):
        st = self.session.state
        n, p = st.snapshots[self.snap_list.row(item)]
        self._apply_params(copy.deepcopy(p), True, f"スナップショット：{n}", update_panel=True)

    # ================================================================ viewer toggles
    def _toggle_wipe(self, on):
        self.viewer.split = ("v" if getattr(self, "_wipe_dir", "v") == "v" else "h") if on else None
        self.viewer.update()

    def _toggle_wipe_dir(self):
        self._wipe_dir = "h" if getattr(self, "_wipe_dir", "v") == "v" else "v"
        if self.viewer.split:
            self.viewer.split = self._wipe_dir
            self.viewer.update()

    def _set_bypass(self, on):
        self.viewer.bypass = on
        self.viewer.update()

    def _set_clip(self, on):
        self.viewer.overlay_clip = on
        self.viewer.update()

    # ================================================================ export
    def export_dialog(self):
        if not self.model.items:
            return
        self._flush_save()
        sel = self.selected_paths()
        dlg = ExportDialog(len(sel), self.proxy.rowCount(), self)
        if not dlg.exec():
            return
        opts = dlg.options()
        mode, min_star = dlg.target_mode()
        if mode == "selected":
            paths = sel
        else:
            paths = self.filtered_paths()
            if mode == "star":
                paths = [p for p in paths if self.get_state(p).rating >= min_star]
            elif mode == "pick":
                paths = [p for p in paths if self.get_state(p).flag == 1]
        if not paths:
            QMessageBox.information(self, "書き出し", "条件に合う写真がありません")
            return
        if not opts.same_folder and not opts.folder:
            QMessageBox.warning(self, "書き出し", "保存先フォルダを指定してください")
            return
        jobs = []
        for p in paths:
            st = self.get_state(p)
            jobs.append((p, copy.deepcopy(st.params), st.rating, st.label))
        prog = QProgressDialog("書き出し中…", "中止", 0, len(jobs), self)
        prog.setWindowTitle("書き出し")
        prog.setWindowModality(Qt.WindowModal)
        prog.setMinimumDuration(0)
        prog.setValue(0)
        self._export_cancel = False
        prog.canceled.connect(lambda: setattr(self, "_export_cancel", True))
        self._export_prog = prog
        try:
            self.em.exportProgress.disconnect()
            self.em.exportDone.disconnect()
        except Exception:
            pass
        self.em.exportProgress.connect(lambda i, name: (prog.setValue(i), prog.setLabelText(f"書き出し中… {name}")))
        self.em.exportDone.connect(lambda done, errs: self._export_finished(done, errs, opts))
        self.export_pool.submit(self._export_job, jobs, opts)

    def _export_job(self, jobs, opts):
        done, errs = [], []
        db = LensDB.get()
        for i, (p, params, rating, label) in enumerate(jobs):
            if self._export_cancel:
                break
            self.em.exportProgress.emit(i, os.path.basename(p))
            try:
                out = exporter.out_path(p, opts, i + 1)
                if os.path.exists(out) and opts.overwrite == "skip":
                    continue
                src = decode.load(p, full=True)
                prm = merged(params)
                info = None
                if db.ok:
                    lens, cam, crop, conf = db.identify(src.meta)
                    key = prm["lens"].get("lens") or (LensDB.key(lens) if lens is not None else None)
                    if key:
                        info = {"lens_key": key, "crop": crop, "focal": src.meta.get("focal"),
                                "aperture": src.meta.get("aperture")}
                if params is None and not decode.is_raw(p):
                    prm["sharpen"]["amount"] = 0.0
                    prm["nr"]["color"] = 0.0
                    prm["lens"]["profile"] = False
                g = pipeline.develop(src.linear, prm, src.as_shot_gains, info, scale=1.0, detail=True)
                exporter.write_image(g, out, opts, rating, label, src.meta)
                done.append((p, out))
            except Exception as e:
                traceback.print_exc()
                errs.append(f"{os.path.basename(p)}：{e}")
        self.em.exportProgress.emit(len(jobs), "")
        self.em.exportDone.emit(done, errs)

    def _export_finished(self, done, errs, opts):
        self._export_prog.close()
        if opts.add_to_library:
            for src, out in done:
                self.catalog.add_link(src, out)
                st = self.get_state(src)
                if opts.embed_rating:
                    self.catalog.upsert(out, rating=st.rating, label=st.label)
                it = self.model.item(src)
                if it:
                    it.exports = len([x for x in self.catalog.exports_of(src) if os.path.exists(x)])
                    self.model.refresh(src)
        msg = f"{len(done)} 枚を書き出しました"
        if errs:
            QMessageBox.warning(self, "書き出し", msg + "\n\n失敗：\n" + "\n".join(errs[:20]))
        else:
            self.statusBar().showMessage(msg, 6000)
        if done and opts.add_to_library:
            out_dir = os.path.dirname(done[0][1])
            if os.path.normpath(out_dir) == os.path.normpath(self.folder or ""):
                self.open_folder(self.folder)

    # ================================================================ misc
    def settings_dialog(self):
        SettingsDialog(self).exec()

    def _grid_menu(self, pos):
        m = QMenu(self)
        for i in range(6):
            m.addAction("★" * i if i else "★ なし", lambda v=i: self.set_rating(v))
        m.addSeparator()
        for lb in LABELS:
            m.addAction(f"ラベル：{LABEL_NAMES[lb]}", lambda v=lb: self.set_label(v))
        m.addAction("ラベルなし", lambda: self.set_label(None))
        m.addSeparator()
        m.addAction("採用", lambda: self.set_flag(1))
        m.addAction("除外", lambda: self.set_flag(-1))
        m.addAction("フラグなし", lambda: self.set_flag(0))
        m.addSeparator()
        m.addAction("調整をコピー", self.copy_settings)
        m.addAction("調整をペースト", self.paste_settings)
        m.addAction("書き出したファイル／元の写真", self.show_links)
        m.exec(self.grid.viewport().mapToGlobal(pos))

    def show_keys(self):
        QMessageBox.information(self, "操作一覧", """<b>評価</b><br>
0〜5：★　[ ]：★を1つ増減　6〜9：ラベル（赤・黄・緑・青）、紫は右クリック<br>
P：採用　X：除外　U：フラグなし<br><br>
<b>移動・表示</b><br>
← →：前後の写真　G：ライブラリ　D / Enter：現像　F：全画面で選別　Esc：戻る<br>
Z / ダブルクリック：全体⇔等倍　ホイール：ズーム　中ボタン・Space＋ドラッグ：移動<br>
Shift+D または \\：補正前を表示　Y：ワイプ比較　J：白飛び・黒つぶれ警告<br><br>
<b>調整（Resolve 風）</b><br>
数値の上を左右ドラッグ：増減（Ctrl：微調整／Shift：粗調整）<br>
ダブルクリック：リセット　クリック：数値入力（+=0.3 で相対）<br>
Ctrl+U：自動調整　W：WB スポイト　R：切り抜き<br>
Ctrl+Z / Ctrl+Y：元に戻す／やり直す<br>
Ctrl+Shift+C / V：調整のコピー／ペースト<br>
フィルムストリップを中クリック：その写真の調整を今の写真へ<br>
ヒストグラムの区画を左右ドラッグ：対応するスライダーを操作""")

    def closeEvent(self, e):
        self._flush_save()
        self.settings.setValue("win/geometry", self.saveGeometry())
        self.settings.setValue("win/state", self.saveState())
        self.pool.shutdown(wait=False, cancel_futures=True)
        super().closeEvent(e)
