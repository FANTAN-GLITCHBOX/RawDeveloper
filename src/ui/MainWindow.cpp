#include "ui/MainWindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <cstdio>
#include <functional>

#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QInputDialog>
#include <QPointer>
#include <QUrl>
#include <QItemSelectionModel>
#include <QProgressDialog>
#include <QSaveFile>
#include <QComboBox>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <atomic>

#include "core/CameraRef.h"
#include "core/Catalog.h"
#include "core/Export.h"
#include "core/Xmp.h"
#include "engine/Auto.h"
#include "engine/Skin.h"
#include "engine/Profile.h"
#include "engine/Color.h"
#include "engine/Pipeline.h"
#include "ui/Batch.h"
#include "ui/DevelopPanel.h"
#include "ui/Dialogs.h"
#include "ui/Library.h"
#include "ui/Scopes.h"
#include "ui/Theme.h"
#include "ui/Viewer.h"

namespace rd {

namespace {

constexpr int kPreviewEdge = 1600;

std::vector<unsigned char> readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.readAll();
    return std::vector<unsigned char>(b.begin(), b.end());
}

bool isRawPath(const QString& path) { return isRawExtension(QFileInfo(path).suffix().toLower().toStdString()); }

bool isSupported(const QString& path) {
    const QString e = QFileInfo(path).suffix().toLower();
    return isRawPath(path) || e == "jpg" || e == "jpeg" || e == "png" || e == "tif" || e == "tiff";
}

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

// JPEG・PNG・TIFF をリニア sRGB に
std::shared_ptr<LoadedRaw> loadImageFile(const QString& path, bool half) {
    auto r = std::make_shared<LoadedRaw>();
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull()) {
        r->error = reader.errorString().toStdString();
        return r;
    }
    r->meta.fullWidth = img.width();
    r->meta.fullHeight = img.height();
    if (half && std::max(img.width(), img.height()) > 2400)
        img = img.scaled(2400, 2400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_RGBX64);
    static std::vector<float> lut = [] {
        std::vector<float> t(65536);
        for (int i = 0; i < 65536; ++i) t[i] = srgbToLinear(i / 65535.0f);
        return t;
    }();
    r->linear.resize(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const quint16*>(img.constScanLine(y));
        float* d = r->linear.px(0, y);
        for (int x = 0; x < img.width(); ++x)
            for (int c = 0; c < 3; ++c) d[x * 3 + c] = lut[line[x * 4 + c]];
    }
    return r;
}

std::shared_ptr<LoadedRaw> loadAny(const QString& path, bool half) {
    if (isRawPath(path)) {
        const auto bytes = readAll(path);
        auto r = std::make_shared<LoadedRaw>(loadRaw(bytes, half));
        if (r->error.empty()) r->cameraRef = cameraReference(bytes, r->meta.fullWidth, r->meta.fullHeight);
        return r;
    }
    return loadImageFile(path, half);
}

QImage toQImage(const ImageF& g) {
    const auto rgb = toRgb8(g);
    QImage img(g.width, g.height, QImage::Format_RGB888);
    for (int y = 0; y < g.height; ++y) memcpy(img.scanLine(y), rgb.data() + static_cast<size_t>(y) * g.width * 3, g.width * 3);
    return img;
}

QString sidecarPath(const QString& path) { return path + ".rdev"; }

// 回転した画像の中に枠（正規化座標）の四隅が収まっているか
bool cropInside(const CropRect& r, double angleDeg, double W, double H) {
    const double a = angleDeg * 3.14159265358979 / 180.0, ca = std::cos(a), sa = std::sin(a);
    const double cx = W / 2, cy = H / 2;
    for (double fx : {r.x, r.x + r.w})
        for (double fy : {r.y, r.y + r.h}) {
            const double dx = fx * W - cx, dy = fy * H - cy;
            const double sx = ca * dx + sa * dy + cx, sy = -sa * dx + ca * dy + cy;
            if (sx < -0.5 || sy < -0.5 || sx > W + 0.5 || sy > H + 0.5) return false;
        }
    return true;
}

// 黒い余白が入らないよう枠を縮める。縦横比は保ち、縮めるほど画像の中心へ寄せる
CropRect fitCrop(const CropRect& base, double angleDeg, double W, double H) {
    if (cropInside(base, angleDeg, W, H)) return base;
    const double bcx = base.x + base.w / 2, bcy = base.y + base.h / 2;
    auto at = [&](double s) {
        const double cx = 0.5 + (bcx - 0.5) * s, cy = 0.5 + (bcy - 0.5) * s;
        return CropRect{cx - base.w * s / 2, cy - base.h * s / 2, base.w * s, base.h * s};
    };
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double m = (lo + hi) / 2;
        (cropInside(at(m), angleDeg, W, H) ? lo : hi) = m;
    }
    return at(lo);
}

QString geomKey(const Params& p) {
    return QString("%1,%2,%3,%4,%5,%6|%7,%8,%9,%10,%11,%12,%13")
        .arg(p.cropX).arg(p.cropY).arg(p.cropW).arg(p.cropH).arg(p.cropAngle).arg(p.enabled.crop)
        .arg(p.enabled.lens).arg(p.lensProfile).arg(p.lensKey).arg(p.lensDistortion).arg(p.lensCa)
        .arg(p.lensAutotrim).arg(p.lensManualDistortion);
}

}  // namespace

std::shared_ptr<LoadedRaw> loadPhoto(const QString& path, bool half) { return loadAny(path, half); }

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("RawDeveloper"));
    resize(1600, 980);
    setAcceptDrops(true);
    buildUi();
    buildActions();
    connect(&loadWatcher_, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::loaded);
    connect(&renderWatcher_, &QFutureWatcher<RenderResult>::finished, this, &MainWindow::rendered);
    connect(&hiresWatcher_, &QFutureWatcher<RenderResult>::finished, this, &MainWindow::hiresDone);
    connect(&roiWatcher_, &QFutureWatcher<RoiResult>::finished, this, [this] {
        const RoiResult r = roiWatcher_.result();
        if (s_ && s_->path == r.path && !r.img.isNull() && !viewer_->isFit() && !viewer_->cropMode()) {
            viewer_->setRoi(r.img, r.rect);
            if (!r.before.isNull()) {
                viewer_->setBeforeRoi(r.before, r.rect);
                roiBeforeKey_ = r.beforeKey;
            }
        }
        if (roiPending_) {
            roiPending_ = false;
            requestRoi();
        }
    });
    connect(&fullWatcher_, &QFutureWatcher<std::shared_ptr<LoadedRaw>>::finished, this, [this] {
        auto f = fullWatcher_.result();
        viewer_->setMessage({});
        if (s_ && s_->path == fullLoadingPath_ && f && f->error.empty()) {
            s_->full = f;
            requestDetailView();
        }
        fullLoadingPath_.clear();
    });
    viewTimer_.setSingleShot(true);
    connect(&viewTimer_, &QTimer::timeout, this, &MainWindow::requestDetailView);
    saveTimer_.setSingleShot(true);
    connect(&saveTimer_, &QTimer::timeout, this, &MainWindow::saveSidecar);
    QSettings st;
    restoreGeometry(st.value("win/geometry").toByteArray());
    restoreState(st.value("win/state").toByteArray());
    statusBar()->showMessage(QStringLiteral("写真を開いてください（Ctrl+O でファイル、Ctrl+Shift+O でフォルダ）"));
}

// ================================================================ UI

void MainWindow::buildUi() {
    model_ = new PhotoModel(this);
    proxy_ = new FilterProxy;
    proxy_->setParent(this);
    proxy_->setSourceModel(model_);
    proxy_->setDynamicSortFilter(false);

    // ---- ライブラリ
    auto* lib = new QWidget;
    auto* lv = new QVBoxLayout(lib);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);
    filterBar_ = new FilterBar(proxy_);
    lv->addWidget(filterBar_);
    auto* sp = new QSplitter;
    tree_ = new FolderTree;
    connect(tree_, &FolderTree::folderChosen, this, [this](const QString& d) { openFolder(d); });
    grid_ = new ThumbView(false);
    grid_->setModel(proxy_);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QListView::doubleClicked, this, [this] { setMode(true); });
    auto* info = new QWidget;
    auto* iv = new QVBoxLayout(info);
    iv->setContentsMargins(6, 6, 6, 6);
    iv->addWidget(new QLabel(QStringLiteral("ヒストグラム（簡易：埋め込みプレビューから）")));
    libHist_ = new ScopeView(ScopeMode::HistRgb);
    libHist_->setFixedHeight(150);
    iv->addWidget(libHist_);
    libInfo_ = new QLabel;
    libInfo_->setWordWrap(true);
    libInfo_->setAlignment(Qt::AlignTop);
    libInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    iv->addWidget(libInfo_, 1);
    sp->addWidget(tree_);
    sp->addWidget(grid_);
    sp->addWidget(info);
    sp->setSizes({240, 1000, 280});
    sp->setStretchFactor(1, 1);
    lv->addWidget(sp, 1);

    // ---- 現像
    auto* dev = new QWidget;
    auto* v = new QVBoxLayout(dev);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    viewer_ = new Viewer;
    viewer_->clear(QStringLiteral("写真を開くか、ここにドラッグしてください"));
    v->addWidget(viewer_, 1);
    film_ = new ThumbView(true);
    film_->setModel(proxy_);
    film_->setSelectionModel(grid_->selectionModel());
    connect(film_, &ThumbView::middleClicked, this, &MainWindow::copyFromOther);
    film_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(film_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu m;
        m.addAction(QStringLiteral("調整をコピー"), this, [this] { copySettings(false); });
        m.addAction(QStringLiteral("項目を選んで調整をコピー…"), this, [this] { copySettings(true); });
        m.addAction(QStringLiteral("調整を貼り付け"), this, [this] { pasteSettings(); })->setEnabled(clipboard_.has_value());
        m.addAction(QStringLiteral("前の写真と同じ調整にする"), this, [this] { pastePrevious(); });
        m.addAction(QStringLiteral("まとめて自動調整"), this, [this] { batchAuto(); });
        m.exec(film_->viewport()->mapToGlobal(pos));
    });
    undoBtn_ = new QPushButton(QStringLiteral("元に戻す"));
    undoBtn_->hide();
    statusBar()->addPermanentWidget(undoBtn_);
    connect(undoBtn_, &QPushButton::clicked, this, &MainWindow::undoBatch);
    undoTimer_.setSingleShot(true);
    connect(&undoTimer_, &QTimer::timeout, undoBtn_, &QWidget::hide);
    v->addWidget(film_);

    stack_ = new QStackedWidget;
    stack_->addWidget(lib);
    stack_->addWidget(dev);
    setCentralWidget(stack_);

    connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
        if (!cur.isValid()) return;
        const auto* it = static_cast<const PhotoItem*>(cur.data(PhotoModel::ItemRole).value<const void*>());
        showLibInfo(it->path);
        if (developMode_) openFile(it->path);
        film_->scrollTo(cur);
    });
    connect(grid_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu m;
        for (int i = 0; i <= 5; ++i)
            m.addAction(i ? QString(i, QChar(0x2605)) : QStringLiteral("★ なし"), this,
                        [this, i] { rate([i](PhotoMark& k) { k.rating = i; }, i ? QStringLiteral("★%1").arg(i) : QStringLiteral("★なし")); });
        m.addSeparator();
        for (const QString& lb : kLabels)
            m.addAction(QStringLiteral("ラベル：") + labelName(lb), this,
                        [this, lb] { rate([lb](PhotoMark& k) { k.label = lb; }, QStringLiteral("ラベル：") + labelName(lb)); });
        m.addAction(QStringLiteral("ラベルなし"), this, [this] { rate([](PhotoMark& k) { k.label.clear(); }, QStringLiteral("ラベルなし")); });
        m.addSeparator();
        m.addAction(QStringLiteral("採用"), this, [this] { rate([](PhotoMark& k) { k.flag = 1; }, QStringLiteral("採用")); });
        m.addAction(QStringLiteral("除外"), this, [this] { rate([](PhotoMark& k) { k.flag = -1; }, QStringLiteral("除外")); });
        m.addAction(QStringLiteral("フラグなし"), this, [this] { rate([](PhotoMark& k) { k.flag = 0; }, QStringLiteral("フラグなし")); });
        m.addSeparator();
        m.addAction(QStringLiteral("書き出したファイル／元の写真…"), this, &MainWindow::showLinks);
        m.addSeparator();
        m.addAction(QStringLiteral("調整をコピー"), this, [this] { copySettings(false); });
        m.addAction(QStringLiteral("項目を選んで調整をコピー…"), this, [this] { copySettings(true); });
        m.addAction(QStringLiteral("調整を貼り付け"), this, [this] { pasteSettings(); })->setEnabled(clipboard_.has_value());
        m.addAction(QStringLiteral("まとめて自動調整"), this, [this] { batchAuto(); });
        m.exec(grid_->viewport()->mapToGlobal(pos));
    });
    connect(&scanWatcher_, &QFutureWatcher<std::vector<PhotoItem>>::finished, this, &MainWindow::folderScanned);

    scopes_ = new ScopesPanel;
    auto* dockScopes = new QDockWidget(QStringLiteral("スコープ"), this);
    dockScopes->setObjectName("dock_scopes");
    dockScopes->setWidget(scopes_);
    panel_ = new DevelopPanel;
    auto* dockPanel = new QDockWidget(QStringLiteral("調整"), this);
    dockPanel->setObjectName("dock_panel");
    dockPanel->setWidget(panel_);
    dockPanel->setMinimumWidth(340);
    historyList_ = new QListWidget;
    historyList_->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* dockHist = new QDockWidget(QStringLiteral("履歴"), this);
    dockHist->setObjectName("dock_history");
    dockHist->setWidget(historyList_);
    for (auto* d : {dockScopes, dockPanel, dockHist}) {
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
        docks_ << d;
    }
    addDockWidget(Qt::RightDockWidgetArea, dockScopes);
    addDockWidget(Qt::RightDockWidgetArea, dockPanel);
    splitDockWidget(dockScopes, dockPanel, Qt::Vertical);
    addDockWidget(Qt::LeftDockWidgetArea, dockHist);
    resizeDocks({dockScopes, dockPanel}, {260, 700}, Qt::Vertical);
    resizeDocks({dockPanel}, {400}, Qt::Horizontal);
    resizeDocks({dockHist}, {200}, Qt::Horizontal);

    connect(historyList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { jumpHistory(historyList_->row(it)); });
    connect(historyList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = historyList_->itemAt(pos);
        if (!it || !s_) return;
        const int i = historyList_->row(it);
        QMenu m;
        m.addAction(QStringLiteral("この時点と比較する"), this, [this, i] {
            setCompareTarget(s_->history[i].second, QStringLiteral("履歴：") + s_->history[i].first);
        });
        m.addAction(QStringLiteral("比較相手を「補正前」に戻す"), this, [this] { setCompareTarget(std::nullopt, QStringLiteral("補正前")); });
        m.exec(historyList_->viewport()->mapToGlobal(pos));
    });

    connect(panel_, &DevelopPanel::paramsChanged, this,
            [this](Params p, bool final, const QString& desc) {
                if (s_ && viewer_->cropMode()) {
                    // 枠は画面上の操作が正（パネル側の値は古いことがある）
                    p.cropX = s_->params.cropX; p.cropY = s_->params.cropY; p.cropW = s_->params.cropW; p.cropH = s_->params.cropH;
                    if (p.cropAngle != s_->params.cropAngle) p = fitCropToAngle(p);
                }
                applyParams(p, final, desc);
            });
    connect(panel_, &DevelopPanel::autoRequested, this, &MainWindow::autoAdjust);
    connect(panel_, &DevelopPanel::hoverKey, this, [this](const QString& k) {
        static const QStringList zones{"blacks", "shadows", "exposure", "highlights", "whites"};
        scopes_->setZoneHighlight(zones.contains(k) ? k : QString());
    });
    connect(panel_, &DevelopPanel::cropToolToggled, this, &MainWindow::cropTool);
    connect(panel_, &DevelopPanel::aspectChanged, this, [this] {
        double a = panel_->aspectValue();
        if (a < 0 && s_ && s_->src) a = double(s_->src->meta.fullWidth) / std::max(1, s_->src->meta.fullHeight);
        viewer_->setCropAspect(a);
    });
    connect(panel_->wbPickButton(), &QToolButton::toggled, viewer_, &Viewer::setPicking);
    connect(viewer_, &Viewer::picked, this, &MainWindow::wbPick);
    connect(panel_->skinPickButton(), &QToolButton::toggled, this, [this](bool on) {
        if (on) panel_->wbPickButton()->setChecked(false);
        viewer_->setPicking(on);
        if (on) statusBar()->showMessage(QStringLiteral("肌（頬など）をクリックしてください"), 5000);
    });
    connect(panel_->wbPickButton(), &QToolButton::toggled, this, [this](bool on) {
        if (on) panel_->skinPickButton()->setChecked(false);
    });
    setupLocal();
    connect(panel_, &DevelopPanel::profileChosen, this, [this](const QString& name) {
        if (!s_ || !s_->src) return;
        Params p = s_->params;
        if (name == "camera") {
            if (s_->src->cameraRef.width <= 0 || !matchCamera(s_->src->linear, s_->src->asShotGains, s_->src->cameraRef, p)) {
                statusBar()->showMessage(QStringLiteral("この写真にはカメラの JPEG が入っていないため、「カメラに合わせる」は使えません"), 6000);
                panel_->setParams(s_->params, s_->asShotTT);
                return;
            }
            applyParams(p, true, QStringLiteral("プロファイル：カメラに合わせる"), true);
        } else {
            p.profile = "standard";
            applyParams(p, true, QStringLiteral("プロファイル：標準"), true);
        }
    });
    connect(panel_, &DevelopPanel::skinMaskToggled, this, [this](bool on) {
        if (!s_) return;
        s_->params.skinMaskView = on;
        ++version_;
        requestRender();
    });
    connect(panel_, &DevelopPanel::skinAutoRequested, this, [this](int level) {
        if (!s_ || !s_->preview) return;
        const SkinStats st = analyzeSkin(skinProbeImage());
        if (st.fraction < 0.003f) {
            statusBar()->showMessage(QStringLiteral("肌らしい色が見つかりませんでした。「肌をクリックして指定」を使ってください"), 6000);
            return;
        }
        static const char* names[] = {"", "弱め", "普通", "強め"};
        Params p = skinAuto(s_->params, st, level);
        applyParams(p, true, QStringLiteral("肌色：いい感じに（%1）").arg(QString::fromUtf8(names[level])), true);
        statusBar()->showMessage(st.faces > 0 ? QStringLiteral("顔 %1 人の肌から色を測って整えました").arg(st.faces)
                                              : QStringLiteral("顔が見つからなかったので、色だけで肌を推定しました。違う所が選ばれていたら「肌をクリックして指定」で直せます"),
                                 7000);
    });
    connect(viewer_, &Viewer::probe, scopes_, &ScopesPanel::setProbe);
    connect(viewer_, &Viewer::wantHiRes, this, &MainWindow::requestDetailView);
    connect(viewer_, &Viewer::viewChanged, this, [this] { viewTimer_.start(30); });
    connect(viewer_, &Viewer::cropChanged, this, [this](const CropRect& r, bool final) {
        if (!s_) return;
        angleBase_.reset();
        Params p = s_->params;
        p.cropX = float(r.x);
        p.cropY = float(r.y);
        p.cropW = float(r.w);
        p.cropH = float(r.h);
        if (final) {
            panel_->cropButton()->setChecked(false);
            applyParams(p, true, QStringLiteral("切り抜き"), true);
        } else {
            s_->params = p;
        }
    });
    connect(scopes_, &ScopesPanel::hoverRange, viewer_, &Viewer::setMaskRange);
    connect(scopes_, &ScopesPanel::zoneDrag, panel_, &DevelopPanel::nudgeZone);
}

void MainWindow::buildActions() {
    auto* tb = addToolBar(QStringLiteral("ツール"));
    tb->setObjectName("toolbar");
    tb->setMovable(false);
    toolbarWidget_ = tb;
    auto* modeGroup = new QActionGroup(this);
    modeLib_ = tb->addAction(QStringLiteral("ライブラリ (G)"));
    modeDev_ = tb->addAction(QStringLiteral("現像 (D)"));
    for (QAction* a : {modeLib_, modeDev_}) {
        a->setCheckable(true);
        modeGroup->addAction(a);
    }
    modeLib_->setShortcut(QKeySequence("G"));
    modeDev_->setShortcut(QKeySequence("D"));
    connect(modeLib_, &QAction::triggered, this, [this] { setMode(false); });
    connect(modeDev_, &QAction::triggered, this, [this] { setMode(true); });
    addAction(modeLib_);
    addAction(modeDev_);
    tb->addSeparator();
    auto add = [&](QMenu* menu, const QString& text, const QKeySequence& key, auto fn, bool toolbar = false) {
        QAction* a = menu->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        if (toolbar) tb->addAction(a);
        return a;
    };
    QMenu* file = menuBar()->addMenu(QStringLiteral("ファイル"));
    add(file, QStringLiteral("ファイルを開く…"), QKeySequence::Open, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("写真を開く"), folder_,
            QStringLiteral("写真 (*.cr2 *.cr3 *.nef *.nrw *.arw *.raf *.orf *.rw2 *.pef *.dng *.srw *.3fr *.iiq *.jpg *.jpeg *.png *.tif *.tiff);;すべて (*.*)"));
        if (!f.isEmpty()) openFolder(QFileInfo(f).absolutePath(), f);
    }, true);
    add(file, QStringLiteral("フォルダを開く…"), QKeySequence("Ctrl+Shift+O"), [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("写真のフォルダ"), folder_);
        if (!d.isEmpty()) openFolder(d);
    }, true);
    add(file, QStringLiteral("書き出し…"), QKeySequence("Ctrl+Shift+E"), [this] { exportDialog(); }, true);
    file->addSeparator();
    add(file, QStringLiteral("環境設定…"), QKeySequence("Ctrl+,"), [this] { SettingsDialog(this).exec(); });
    file->addSeparator();
    add(file, QStringLiteral("終了"), QKeySequence::Quit, [this] { close(); });

    QMenu* edit = menuBar()->addMenu(QStringLiteral("編集"));
    add(edit, QStringLiteral("元に戻す"), QKeySequence::Undo, [this] { if (s_ && s_->historyPos > 0) jumpHistory(s_->historyPos - 1); });
    add(edit, QStringLiteral("やり直す"), QKeySequence("Ctrl+Y"), [this] {
        if (s_ && s_->historyPos + 1 < int(s_->history.size())) jumpHistory(s_->historyPos + 1);
    });
    edit->addSeparator();
    add(edit, QStringLiteral("調整をコピー"), QKeySequence("Ctrl+C"), [this] { copySettings(false); });
    add(edit, QStringLiteral("項目を選んで調整をコピー…"), QKeySequence("Ctrl+Shift+C"), [this] { copySettings(true); });
    add(edit, QStringLiteral("調整を貼り付け（選んでいる写真すべて）"), QKeySequence("Ctrl+V"), [this] { pasteSettings(); });
    add(edit, QStringLiteral("前の写真と同じ調整にする"), QKeySequence("Ctrl+Alt+V"), [this] { pastePrevious(); });
    syncAct_ = add(edit, QStringLiteral("同期（スライダーの変更を選んでいる写真すべてに反映）"), QKeySequence(), [this] {
        statusBar()->showMessage(syncAct_->isChecked() ? QStringLiteral("同期オン：選んでいる写真すべてに反映します")
                                                       : QStringLiteral("同期オフ"), 3000);
    });
    syncAct_->setCheckable(true);
    edit->addSeparator();
    add(edit, QStringLiteral("自動調整"), QKeySequence("Ctrl+U"), [this] { autoAdjust(100, false); });
    add(edit, QStringLiteral("選んだ写真をまとめて自動調整"), QKeySequence("Ctrl+Shift+U"), [this] { batchAuto(); });
    add(edit, QStringLiteral("すべての調整をリセット"), QKeySequence("Ctrl+Shift+R"), [this] {
        if (s_) applyParams(s_->isRaw ? Params::defaults() : Params::defaultsForImage(), true, QStringLiteral("すべてリセット"), true);
    });

    // 写真（評価）
    QMenu* photo = menuBar()->addMenu(QStringLiteral("写真"));
    for (int i = 0; i <= 5; ++i)
        add(photo, i ? QStringLiteral("★ %1").arg(i) : QStringLiteral("★ なし"), QKeySequence(QString::number(i)),
            [this, i] { rate([i](PhotoMark& k) { k.rating = i; }, i ? QStringLiteral("★%1").arg(i) : QStringLiteral("★なし")); });
    add(photo, QStringLiteral("★を 1 つ減らす"), QKeySequence("["), [this] {
        rate([](PhotoMark& k) { k.rating = std::max(0, k.rating - 1); }, QStringLiteral("★を変更"));
    });
    add(photo, QStringLiteral("★を 1 つ増やす"), QKeySequence("]"), [this] {
        rate([](PhotoMark& k) { k.rating = std::min(5, k.rating + 1); }, QStringLiteral("★を変更"));
    });
    photo->addSeparator();
    for (int i = 0; i < kLabels.size(); ++i) {
        const QString lb = kLabels[i];
        add(photo, QStringLiteral("ラベル：") + labelName(lb), i < 4 ? QKeySequence(QString::number(6 + i)) : QKeySequence(),
            [this, lb] { rate([lb](PhotoMark& k) { k.label = k.label == lb ? QString() : lb; }, QStringLiteral("ラベル：") + labelName(lb)); });
    }
    add(photo, QStringLiteral("ラベルなし"), QKeySequence(), [this] { rate([](PhotoMark& k) { k.label.clear(); }, QStringLiteral("ラベルなし")); });
    photo->addSeparator();
    add(photo, QStringLiteral("採用"), QKeySequence("P"), [this] { rate([](PhotoMark& k) { k.flag = 1; }, QStringLiteral("採用")); });
    add(photo, QStringLiteral("除外"), QKeySequence("X"), [this] { rate([](PhotoMark& k) { k.flag = -1; }, QStringLiteral("除外")); });
    add(photo, QStringLiteral("フラグなし"), QKeySequence("U"), [this] { rate([](PhotoMark& k) { k.flag = 0; }, QStringLiteral("フラグなし")); });
    photo->addSeparator();
    add(photo, QStringLiteral("除外した写真をごみ箱へ…"), QKeySequence(), [this] { trashRejected(); });
    add(photo, QStringLiteral("書き出したファイル／元の写真…"), QKeySequence(), [this] { showLinks(); });

    // 表示・比較
    QMenu* view = menuBar()->addMenu(QStringLiteral("表示"));
    compareBtn_ = new QToolButton;
    compareBtn_->setText(QStringLiteral("比較：オフ"));
    compareBtn_->setPopupMode(QToolButton::InstantPopup);
    auto* cmpMenu = new QMenu(compareBtn_);
    auto* group = new QActionGroup(this);
    const std::tuple<const char*, CompareMode, const char*> modes[] = {
        {"オフ", CompareMode::Off, ""},
        {"ワイプ（左右）", CompareMode::WipeV, "Y"},
        {"ワイプ（上下）", CompareMode::WipeH, "Shift+Y"},
        {"並べて比較（左右）", CompareMode::SideH, "Alt+Y"},
        {"並べて比較（上下）", CompareMode::SideV, "Alt+Shift+Y"},
        {"並べて比較（自動）", CompareMode::SideAuto, ""}};
    for (auto& [name, mode, key] : modes) {
        QAction* a = cmpMenu->addAction(QString::fromUtf8(name));
        a->setCheckable(true);
        a->setChecked(mode == CompareMode::Off);
        group->addAction(a);
        if (*key) a->setShortcut(QKeySequence(key));
        const CompareMode m = mode;
        const QString label = QString::fromUtf8(name);
        connect(a, &QAction::triggered, this, [this, m, label, a] {
            // 同じキーをもう一度押したらオフ
            if (viewer_->compare() == m && m != CompareMode::Off) {
                viewer_->setCompare(CompareMode::Off);
                compareBtn_->setText(QStringLiteral("比較：オフ"));
                return;
            }
            a->setChecked(true);
            viewer_->setCompare(m);
            compareBtn_->setText(QStringLiteral("比較：") + label);
        });
        view->addAction(a);
    }
    cmpMenu->addSeparator();
    QAction* swap = cmpMenu->addAction(QStringLiteral("左右（上下）を入れ替える"));
    connect(swap, &QAction::triggered, viewer_, &Viewer::swapSides);
    QAction* link = cmpMenu->addAction(QStringLiteral("2 画面のズーム・位置を連動"));
    link->setCheckable(true);
    link->setChecked(true);
    connect(link, &QAction::toggled, viewer_, &Viewer::setLinked);
    QAction* beforeCrop = cmpMenu->addAction(QStringLiteral("補正前にも切り抜き・角度を適用"));
    beforeCrop->setCheckable(true);
    beforeCrop_ = QSettings().value("compare/before_crop", true).toBool();
    beforeCrop->setChecked(beforeCrop_);
    connect(beforeCrop, &QAction::toggled, this, [this](bool on) {
        beforeCrop_ = on;
        QSettings().setValue("compare/before_crop", on);
        beforeKey_.clear();
        requestRender();
    });
    QAction* resetTarget = cmpMenu->addAction(QStringLiteral("比較相手：補正前"));
    connect(resetTarget, &QAction::triggered, this, [this] { setCompareTarget(std::nullopt, QStringLiteral("補正前")); });
    cmpMenu->addAction(QStringLiteral("（履歴を右クリックで、その時点と比較できます）"))->setEnabled(false);
    compareBtn_->setMenu(cmpMenu);
    tb->addSeparator();
    tb->addWidget(compareBtn_);
    view->addSeparator();
    QAction* bypass = add(view, QStringLiteral("補正オフ（補正前を表示）"), QKeySequence("Shift+D"), [] {}, true);
    bypass->setCheckable(true);
    connect(bypass, &QAction::toggled, viewer_, &Viewer::setBypass);
    auto* bs = new QShortcut(QKeySequence("\\"), this);
    connect(bs, &QShortcut::activated, bypass, &QAction::toggle);
    QAction* clip = add(view, QStringLiteral("白飛び・黒つぶれ警告"), QKeySequence("J"), [] {}, true);
    clip->setCheckable(true);
    connect(clip, &QAction::toggled, viewer_, &Viewer::setClipOverlay);
    add(view, QStringLiteral("全体表示"), QKeySequence(), [this] { viewer_->zoomFit(); }, true);
    add(view, QStringLiteral("100%"), QKeySequence(), [this] { viewer_->zoom100(); }, true);
    add(view, QStringLiteral("全体⇔等倍"), QKeySequence("Z"), [this] { viewer_->toggleZoom(); });
    // 切り抜き中の ← → は角度（0.1°、Shift で 1°、Ctrl で 0.01°）
    add(view, QStringLiteral("前の写真（切り抜き中は角度 −）"), QKeySequence("Left"), [this] {
        viewer_->cropMode() ? nudgeAngle(-0.1) : step(-1);
    });
    add(view, QStringLiteral("次の写真（切り抜き中は角度 ＋）"), QKeySequence("Right"), [this] {
        viewer_->cropMode() ? nudgeAngle(0.1) : step(1);
    });
    for (auto [key, d] : std::initializer_list<std::pair<const char*, double>>{
             {"Shift+Left", -1.0}, {"Shift+Right", 1.0}, {"Ctrl+Left", -0.01}, {"Ctrl+Right", 0.01}}) {
        auto* sc = new QShortcut(QKeySequence(key), this);
        connect(sc, &QShortcut::activated, this, [this, d] {
            if (viewer_->cropMode()) nudgeAngle(d);
        });
    }
    angleTimer_.setSingleShot(true);
    connect(&angleTimer_, &QTimer::timeout, this, &MainWindow::commitAngle);
    add(view, QStringLiteral("WB スポイト"), QKeySequence("W"), [this] { panel_->wbPickButton()->toggle(); });
    add(view, QStringLiteral("切り抜きツール"), QKeySequence("R"), [this] { panel_->cropButton()->toggle(); });
    add(view, QStringLiteral("全画面で選別"), QKeySequence("F"), [this] { toggleFullscreen(); });
    for (auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* enter = new QShortcut(QKeySequence(key), this);
        connect(enter, &QShortcut::activated, this, [this] {
            if (!developMode_) setMode(true);
            else if (viewer_->cropMode()) {
                commitAngle();
                emit viewer_->cropChanged(viewer_->cropRect(), true);
            }
        });
    }
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (panel_->localTool() != DevelopPanel::ToolNone) {
            panel_->setLocalTool(DevelopPanel::ToolNone);
        } else if (viewer_->cropMode() && s_) {
            angleTimer_.stop();
            viewer_->setAngleHint({});
            panel_->cropButton()->setChecked(false);
            if (cropBackup_) applyParams(*cropBackup_, true, QStringLiteral("切り抜きを取消"), true);
        } else if (isFullScreen()) {
            toggleFullscreen();
        }
    });

    QMenu* help = menuBar()->addMenu(QStringLiteral("ヘルプ"));
    add(help, QStringLiteral("操作一覧"), QKeySequence("F1"), [this] {
        QMessageBox::information(this, QStringLiteral("操作一覧"), QStringLiteral(
            "<b>評価</b><br>0〜5：★　[ ]：★を増減　6〜9：ラベル（赤・黄・緑・青）、紫はメニュー<br>"
            "P：採用　X：除外　U：フラグなし　G：ライブラリ　D / Enter：現像　F：全画面で選別<br><br>"
            "<b>部分補正</b><br>段階・円形：ドラッグで作成、印をドラッグで調整（円形は Shift で正円）　ブラシ：なぞる（Alt で消す）<br>"
            "スポット修復：クリック、点線の円（コピー元）をドラッグ　Delete：選んでいるスポット／マスクを削除　Esc：道具を終了<br><br>"
            "<b>調整のコピー</b><br>Ctrl+C：コピー　Ctrl+Shift+C：項目を選んでコピー　Ctrl+V：選んでいる写真すべてに貼り付け<br>"
            "Ctrl+Alt+V：前の写真と同じ調整　Ctrl+Shift+U：選んだ写真をまとめて自動調整　編集メニュー「同期」：変更を選択中すべてに反映<br><br>"
            "<b>調整（Resolve 風）</b><br>"
            "数値の上を左右ドラッグ：増減（Ctrl：微調整／Shift：粗調整）<br>"
            "ダブルクリック：リセット　クリック：数値入力（+=0.3 で相対）<br>"
            "パネル見出しの ●：オン/オフ（Alt＋クリックでそのパネルだけオン）　↺：初期値に戻す<br>"
            "Ctrl+U：自動調整　W：WB スポイト　R：切り抜き<br>"
            "Ctrl+Z / Ctrl+Y：元に戻す／やり直す<br><br>"
            "<b>比較</b><br>Y：ワイプ（左右）　Shift+Y：ワイプ（上下）<br>"
            "Alt+Y：並べて比較（左右）　Alt+Shift+Y：並べて比較（上下）<br>"
            "Shift+D または \\：補正前を表示　J：白飛び・黒つぶれ警告<br>"
            "履歴を右クリック：その時点と比較<br><br>"
            "<b>表示</b><br>Z / ダブルクリック：全体⇔等倍　ホイール：ズーム　中ボタン・Space＋ドラッグ：移動<br>"
            "← →：前後の写真<br>ヒストグラムの区画を左右ドラッグ：対応するスライダーを操作"));
    });
    metaLabel_ = new QLabel;
    metaLabel_->setStyleSheet("color:#8c8c8c; padding: 0 8px;");
    statusBar()->addPermanentWidget(metaLabel_);
}

// ================================================================ folder / file

void MainWindow::openFolder(const QString& folder, const QString& select) {
    flushSave();
    folder_ = QDir::fromNativeSeparators(folder);
    QSettings().setValue("library/folder", folder_);
    tree_->reveal(folder_);
    pendingSelect_ = select.isEmpty() ? QString() : QFileInfo(select).absoluteFilePath();
    setWindowTitle(QStringLiteral("RawDeveloper — ") + QDir::toNativeSeparators(folder_));
    statusBar()->showMessage(QStringLiteral("読み込み中…"));
    const QString f = folder_;
    scanWatcher_.setFuture(QtConcurrent::run([f] { return scanFolder(f); }));
}

void MainWindow::folderScanned() {
    s_.reset();
    model_->setItems(scanWatcher_.result());
    proxy_->refilter();
    filterBar_->updateCount();
    statusBar()->showMessage(QStringLiteral("%1 枚").arg(model_->rowCount()), 3000);
    QModelIndex sel;
    for (int i = 0; i < proxy_->rowCount() && !pendingSelect_.isEmpty(); ++i) {
        const QModelIndex ix = proxy_->index(i, 0);
        if (static_cast<const PhotoItem*>(ix.data(PhotoModel::ItemRole).value<const void*>())->path == pendingSelect_) sel = ix;
    }
    if (!sel.isValid() && proxy_->rowCount()) sel = proxy_->index(0, 0);
    if (!pendingSelect_.isEmpty() && !developMode_) setMode(true);
    if (sel.isValid()) grid_->selectionModel()->setCurrentIndex(sel, QItemSelectionModel::ClearAndSelect);
    else viewer_->clear(QStringLiteral("このフォルダには写真がありません"));
    pendingSelect_.clear();
}

void MainWindow::step(int d) {
    const QModelIndex cur = grid_->currentIndex();
    const int r = (cur.isValid() ? cur.row() : -1) + d;
    if (r < 0 || r >= proxy_->rowCount()) return;
    const QModelIndex ix = proxy_->index(r, 0);
    grid_->selectionModel()->setCurrentIndex(ix, QItemSelectionModel::ClearAndSelect);
    grid_->scrollTo(ix);
    film_->scrollTo(ix);
}

void MainWindow::setMode(bool develop) {
    if (develop && !grid_->currentIndex().isValid()) {
        if (!proxy_->rowCount()) {
            statusBar()->showMessage(QStringLiteral("写真がありません。フォルダを開いてください"), 4000);
            develop = false;
        } else {
            grid_->selectionModel()->setCurrentIndex(proxy_->index(0, 0), QItemSelectionModel::ClearAndSelect);
        }
    }
    developMode_ = develop;
    stack_->setCurrentIndex(develop ? 1 : 0);
    (develop ? modeDev_ : modeLib_)->setChecked(true);
    for (auto* d : docks_) d->setVisible(develop && !isFullScreen());
    if (develop) {
        const QString p = currentPath();
        if (!p.isEmpty()) openFile(p);
        viewer_->setFocus();
        film_->scrollTo(grid_->currentIndex());
    } else {
        flushSave();
        grid_->setFocus();
        grid_->scrollTo(grid_->currentIndex());
    }
}

QString MainWindow::currentPath() const {
    const QModelIndex ix = grid_->currentIndex();
    if (!ix.isValid()) return {};
    return static_cast<const PhotoItem*>(ix.data(PhotoModel::ItemRole).value<const void*>())->path;
}

QStringList MainWindow::selectedPaths() const {
    QStringList out;
    for (const QModelIndex& ix : grid_->selectionModel()->selectedIndexes())
        out << static_cast<const PhotoItem*>(ix.data(PhotoModel::ItemRole).value<const void*>())->path;
    if (out.isEmpty() && !currentPath().isEmpty()) out << currentPath();
    return out;
}

QStringList MainWindow::filteredPaths() const {
    QStringList out;
    for (int i = 0; i < proxy_->rowCount(); ++i)
        out << static_cast<const PhotoItem*>(proxy_->index(i, 0).data(PhotoModel::ItemRole).value<const void*>())->path;
    return out;
}

QStringList MainWindow::ratingTargets() const {
    const QStringList sel = selectedPaths();
    if (developMode_ && sel.size() <= 1 && s_) return {s_->path};
    return sel;
}

void MainWindow::showLibInfo(const QString& path) {
    PhotoItem* it = model_->item(path);
    if (!it) return;
    if (!it->thumb.isNull()) {
        const QImage t = it->thumb.convertToFormat(QImage::Format_RGB888);
        std::vector<std::uint8_t> packed(static_cast<size_t>(t.width()) * t.height() * 3);
        for (int y = 0; y < t.height(); ++y) memcpy(&packed[static_cast<size_t>(y) * t.width() * 3], t.constScanLine(y), t.width() * 3);
        libHist_->setData(ScopeData::make(packed.data(), t.width(), t.height()));
    }
    auto rows = std::make_shared<QStringList>();
    const QString dim = "<span style='color:#8c8c8c'>%1</span>　%2";
    *rows << dim.arg(QStringLiteral("ファイル"), it->name);
    QString stars = it->rating ? QString(it->rating, QChar(0x2605)) : QStringLiteral("なし");
    if (!it->label.isEmpty()) stars += "　" + labelName(it->label);
    if (it->flag) stars += it->flag == 1 ? QStringLiteral("　採用") : QStringLiteral("　除外");
    *rows << dim.arg(QStringLiteral("評価"), stars);
    if (it->exports) *rows << dim.arg(QStringLiteral("書き出し"), QStringLiteral("%1 件").arg(it->exports));
    if (it->isExport) {
        const QString src = Catalog::get().sourceOf(path);
        *rows << dim.arg(QStringLiteral("元の写真"), QFileInfo(src).fileName() + (it->missingLink ? QStringLiteral("（見つかりません）") : ""));
    }
    libInfo_->setText(rows->join("<br>"));
    const bool raw = it->isRaw;
    QtConcurrent::run([path, raw] {
        Metadata m;
        if (raw) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray b = f.readAll();
                m = readRawMetadata(std::vector<unsigned char>(b.begin(), b.end()));
            }
        } else {
            QImageReader r(path);
            m.fullWidth = r.size().width();
            m.fullHeight = r.size().height();
        }
        return m;
    }).then(this, [this, path, rows, dim](const Metadata& m) {
        if (currentPath() != path) return;
        QStringList r = *rows;
        if (!m.make.empty()) r << dim.arg(QStringLiteral("カメラ"), QString::fromStdString(m.make + " " + m.model));
        if (!m.lensModel.empty()) r << dim.arg(QStringLiteral("レンズ"), QString::fromStdString(m.lensModel));
        if (m.iso > 0)
            r << dim.arg(QStringLiteral("撮影条件"), QString("ISO %1  F%2  %3mm  %4")
                                                         .arg(m.iso).arg(m.aperture, 0, 'f', 1).arg(m.focal, 0, 'f', 0)
                                                         .arg(m.shutter >= 1 ? QString::number(m.shutter) + QStringLiteral("秒")
                                                                             : QString("1/%1").arg(qRound(1 / std::max(m.shutter, 1e-4f)))));
        if (m.timestamp > 0) r << dim.arg(QStringLiteral("撮影日時"), QDateTime::fromSecsSinceEpoch(m.timestamp).toString("yyyy-MM-dd HH:mm:ss"));
        if (m.fullWidth > 0) r << dim.arg(QStringLiteral("画素数"), QString("%1 × %2").arg(m.fullWidth).arg(m.fullHeight));
        libInfo_->setText(r.join("<br>"));
    });
}

void MainWindow::openFile(const QString& path) {
    if (s_ && s_->path == path) return;
    flushSave();
    if (s_) prevPath_ = s_->path;
    panel_->cropButton()->setChecked(false);
    s_ = std::make_unique<Session>();
    s_->path = path;
    s_->isRaw = isRawPath(path);
    viewer_->clear(QStringLiteral("読み込み中… ") + QFileInfo(path).fileName());
    viewer_->resetView();
    compareTarget_.reset();
    viewer_->setBeforeLabel(QStringLiteral("補正前"));
    loadWatcher_.setFuture(QtConcurrent::run([path] {
        LoadResult r;
        r.src = loadAny(path, true);
        r.lens = std::make_shared<LensInfo>(LensDb::get().identify(r.src->meta));
        return r;
    }));
}

void MainWindow::loaded() {
    const LoadResult lr = loadWatcher_.result();
    auto src = lr.src;
    if (!s_) return;
    // 読み込み中に別の写真が選ばれたら、そちらを読み直す
    if (loadWatcher_.isRunning()) return;
    if (!src->error.empty()) {
        viewer_->clear(QStringLiteral("読み込めませんでした：") + QString::fromStdString(src->error));
        return;
    }
    s_->src = src;
    s_->lens = lr.lens;
    panel_->setLensStatus(QString::fromStdString(lr.lens->status));
    auto prev = std::make_shared<ImageF>(downscale(src->linear, kPreviewEdge));
    s_->scale = float(prev->width) / std::max(1, src->meta.fullWidth);
    s_->preview = prev;
    s_->asShotTT = s_->isRaw ? fitTempTint(src->asShotGains) : std::make_pair(6500.0, 0.0);
    // サイドカー
    QFile f(sidecarPath(s_->path));
    if (f.open(QIODevice::ReadOnly)) {
        s_->sidecar = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    if (s_->sidecar.contains("params"))
        s_->params = Params::fromJson(s_->sidecar.value("params").toObject());
    else {
        s_->params = initialParams(s_->path, *src);  // ISO からノイズ軽減、カメラの JPEG に合わせた明るさと色
    }
    s_->history = {{QStringLiteral("読み込み"), s_->params}};
    s_->historyPos = 0;
    refreshHistory();
    panel_->setParams(s_->params, s_->asShotTT);
    const auto& m = src->meta;
    QString meta = QFileInfo(s_->path).fileName();
    if (m.iso > 0) meta += QString("    ISO %1  F%2  %3mm").arg(m.iso).arg(m.aperture, 0, 'f', 1).arg(m.focal, 0, 'f', 0);
    if (!m.lensModel.empty()) meta += "    " + QString::fromStdString(m.lensModel);
    metaLabel_->setText(meta);
    beforeKey_.clear();
    ++version_;
    requestRender();
}

// ================================================================ render

void MainWindow::requestRender() {
    if (!s_ || !s_->preview) return;
    if (renderWatcher_.isRunning()) {
        renderPending_ = true;
        return;
    }
    const Params p = s_->params;
    const bool cropMode = viewer_->cropMode();
    const bool detail = !dragging_;
    const quint64 ver = version_;
    const QString key = geomKey(p) + (beforeCrop_ ? "bc" : "bn") + (compareTarget_ ? "t" + QString::number(qHash(QJsonDocument(compareTarget_->toJson()).toJson())) : "") +
                        (cropMode ? "c" : "");
    const bool needBefore = key != beforeKey_;
    const Params before = makeBeforeParams(p);
    const bool hasTarget = compareTarget_.has_value();
    auto lin = s_->preview;
    const auto asShot = s_->src->asShotGains;
    const float scale = s_->scale;
    const QString path = s_->path;
    Params sizeP = p;
    if (cropMode) sizeP.enabled.crop = false;
    const auto [fw, fh] = outputSize(sizeP, s_->src->meta.fullWidth, s_->src->meta.fullHeight);
    const QSize fullSize(fw, fh);
    auto lens = s_->lens;
    renderWatcher_.setFuture(QtConcurrent::run([lin, p, asShot, scale, detail, cropMode, ver, needBefore, before, hasTarget, path, fullSize, lens] {
        RenderResult r;
        r.path = path;
        r.version = ver;
        DevelopOptions o;
        o.scale = scale;
        o.detail = detail;
        o.doCrop = !cropMode;
        o.lens = lens.get();
        const ImageF g = develop(*lin, p, asShot, o);
        r.after = toQImage(g);
        r.full = fullSize;
        if (needBefore) {
            DevelopOptions ob = o;
            ob.detail = hasTarget;
            r.before = toQImage(develop(*lin, before, asShot, ob));
            // 同じ構図なら大きさを合わせる（比率が違う＝切り抜きなしの補正前は、そのまま渡して画面側で収める）
            const double ra = double(r.after.width()) / r.after.height(), rb = double(r.before.width()) / r.before.height();
            if (r.before.size() != r.after.size() && std::abs(ra - rb) < 0.01) r.before = r.before.scaled(r.after.size());
            r.hasBefore = true;
        }
        return r;
    }));
    if (needBefore) beforeKey_ = key;
}

void MainWindow::rendered() {
    const RenderResult r = renderWatcher_.result();
    if (s_ && s_->path == r.path) {
        if (r.hasBefore) viewer_->setBefore(r.before);
        viewer_->setImage(r.after, r.full);
        s_->last = r.after;
        auto d = scopes_->setImage(r.after.constBits(), r.after.width(), r.after.height());
        // QImage の行は 4 バイト境界にそろうので、詰めた配列で作り直す
        if (r.after.bytesPerLine() != r.after.width() * 3) {
            std::vector<std::uint8_t> packed(static_cast<size_t>(r.after.width()) * r.after.height() * 3);
            for (int y = 0; y < r.after.height(); ++y)
                memcpy(&packed[static_cast<size_t>(y) * r.after.width() * 3], r.after.constScanLine(y), r.after.width() * 3);
            d = scopes_->setImage(packed.data(), r.after.width(), r.after.height());
        }
        panel_->setHistogram(d->histL);
        // フィルムストリップのサムネイルも更新
        if (!dragging_ && r.version == version_ && !viewer_->cropMode() && s_->history.size() > 1 && !s_->params.skinMaskView && s_->params.showMaskIndex < 0) {
            pendingThumb_ = r.after.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pendingThumbPath_ = r.path;
            model_->setThumb(r.path, pendingThumb_);
        }
    }
    if (renderPending_) {
        renderPending_ = false;
        requestRender();
    }
}

void MainWindow::requestHires() {
    if (!s_ || !s_->src || dragging_ || viewer_->cropMode() || hiresWatcher_.isRunning()) return;
    if (hiresVersion_ == version_) return;
    hiresVersion_ = version_;
    const Params p = s_->params;
    const QString path = s_->path;
    const auto asShot = s_->src->asShotGains;
    const int fullW = s_->src->meta.fullWidth;
    const auto [ow, oh] = outputSize(p, s_->src->meta.fullWidth, s_->src->meta.fullHeight);
    const QSize outSize(ow, oh);
    auto full = s_->full;
    const quint64 ver = version_;
    viewer_->setMessage(QStringLiteral("高精細表示を準備中…"));
    auto lens = s_->lens;
    hiresWatcher_.setFuture(QtConcurrent::run([p, path, asShot, fullW, full, ver, outSize, lens] {
        RenderResult r;
        r.path = path;
        r.version = ver;
        r.hires = true;
        auto f = full ? full : loadAny(path, false);
        if (!f->error.empty()) {
            r.error = QString::fromStdString(f->error);
            return r;
        }
        DevelopOptions o;
        o.scale = float(f->linear.width) / std::max(1, fullW);
        o.lens = lens.get();
        const ImageF g = develop(f->linear, p, asShot, o);
        r.after = toQImage(g);
        r.full = outSize;
        r.fullData = f;
        return r;
    }));
}

void MainWindow::hiresDone() {
    const RenderResult r = hiresWatcher_.result();
    viewer_->setMessage({});
    if (!s_ || s_->path != r.path || !r.error.isEmpty()) return;
    if (!s_->full && r.fullData) s_->full = r.fullData;
    if (r.version == version_ && !viewer_->isFit()) viewer_->setImage(r.after, r.full, true);
    else if (!viewer_->isFit()) requestDetailView();
}

void MainWindow::requestDetailView() {
    if (!s_ || !s_->src) return;
    if (viewer_->isFit() || viewer_->cropMode()) {
        viewer_->clearRoi();
        return;
    }
    if (regionSupported(s_->params))
        requestRoi();
    else {
        viewer_->clearRoi();
        if (!dragging_) requestHires();
    }
}

void MainWindow::ensureFull() {
    if (!s_ || s_->full || fullWatcher_.isRunning()) return;
    fullLoadingPath_ = s_->path;
    viewer_->setMessage(QStringLiteral("フル解像度を読み込み中…"));
    const QString path = s_->path;
    fullWatcher_.setFuture(QtConcurrent::run([path] { return loadAny(path, false); }));
}

void MainWindow::requestRoi() {
    if (!s_ || !s_->src || viewer_->isFit() || viewer_->cropMode()) return;
    if (!s_->full) {
        ensureFull();
        return;
    }
    if (roiWatcher_.isRunning()) {
        roiPending_ = true;
        return;
    }
    const QRect vis = viewer_->visibleFullRect();
    if (vis.isEmpty()) return;
    auto full = s_->full;
    const double k = double(full->linear.width) / std::max(1, s_->src->meta.fullWidth);
    const QRect rLin(qRound(vis.x() * k), qRound(vis.y() * k), qRound(vis.width() * k), qRound(vis.height() * k));
    const float scale = float(std::min(1.0, viewer_->zoom() * viewer_->devicePixelRatioF() / k));
    const Params p = s_->params;
    const auto asShot = s_->src->asShotGains;
    const QString path = s_->path;
    const quint64 ver = version_;
    auto lens = s_->lens;
    // 補正前も見えているなら、同じ範囲をフル解像度で（条件が変わったときだけ計算し直す）
    const Params before = makeBeforeParams(p);
    // 切り抜きの有無が補正後と違うと位置が合わないので、そのときは作らない
    const bool beforeAligned = before.enabled.crop == p.enabled.crop && regionSupported(before);
    const QString bKey = QString("%1|%2,%3,%4,%5|%6").arg(path).arg(vis.x()).arg(vis.y()).arg(vis.width()).arg(vis.height())
                             .arg(scale) + QString::fromUtf8(QJsonDocument(before.toJson()).toJson(QJsonDocument::Compact));
    const bool needBefore = viewer_->showsBefore() && beforeAligned && (bKey != roiBeforeKey_ || !viewer_->hasBeforeRoi());
    roiWatcher_.setFuture(QtConcurrent::run([full, rLin, vis, scale, p, asShot, path, ver, lens, before, needBefore, bKey] {
        RoiResult r;
        r.path = path;
        r.version = ver;
        r.rect = vis;
        r.img = toQImage(developRegion(full->linear, p, asShot, rLin.x(), rLin.y(), rLin.width(), rLin.height(), scale, lens.get()));
        if (needBefore) {
            r.before = toQImage(developRegion(full->linear, before, asShot, rLin.x(), rLin.y(), rLin.width(), rLin.height(), scale, nullptr));
            r.beforeKey = bKey;
        }
        return r;
    }));
}

// ================================================================ params / history

void MainWindow::applyParams(const Params& p, bool final, const QString& desc, bool updatePanel) {
    if (!s_) return;
    if (!final && !dragging_) {
        dragging_ = true;
        scopes_->beginGhost();
    }
    s_->params = p;
    s_->params.skinMaskView = panel_->skinMaskShown();
    // 部分補正の範囲表示：チェックが入っているか、形を作っている最中
    s_->params.showMaskIndex = (panel_->maskOverlay() || (ldrag_ != LDrag::None && ldrag_ != LDrag::SpotDst && ldrag_ != LDrag::SpotSrc))
                                   ? panel_->selectedMask()
                                   : -1;
    ++version_;
    if (final) {
        dragging_ = false;
        scopes_->endGhost();
        const Params before = s_->history[s_->historyPos].second;
        pushHistory(desc);
        // 同期：変わった項目だけを選んでいるほかの写真へ
        if (syncAct_ && syncAct_->isChecked()) {
            const QStringList keys = changedKeys(before, p);
            QStringList others = selectedPaths();
            others.removeAll(s_->path);
            if (!keys.isEmpty() && !others.isEmpty()) {
                std::vector<BatchJob> jobs;
                for (const QString& o : others) {
                    BatchJob j;
                    j.mode = BatchJob::Sync;
                    j.path = o;
                    j.src = p;
                    j.keys = keys;
                    jobs.push_back(j);
                }
                runBatch(std::move(jobs), QStringLiteral("同期"), true);
            }
        }
        saveTimer_.start(400);
    }
    if (updatePanel) panel_->setParams(p, s_->asShotTT);
    requestRender();
    if (!viewer_->isFit()) requestDetailView();
}

void MainWindow::pushHistory(const QString& desc) {
    if (s_->history[s_->historyPos].second == s_->params) return;
    s_->history.resize(s_->historyPos + 1);
    s_->history.emplace_back(desc, s_->params);
    s_->historyPos = int(s_->history.size()) - 1;
    refreshHistory();
}

void MainWindow::refreshHistory() {
    historyList_->clear();
    if (!s_) return;
    for (int i = 0; i < int(s_->history.size()); ++i) {
        auto* it = new QListWidgetItem(s_->history[i].first);
        if (i > s_->historyPos) it->setForeground(Qt::gray);
        historyList_->addItem(it);
    }
    historyList_->setCurrentRow(s_->historyPos);
}

void MainWindow::jumpHistory(int i) {
    if (!s_ || i < 0 || i >= int(s_->history.size())) return;
    s_->historyPos = i;
    s_->params = s_->history[i].second;
    ++version_;
    panel_->setParams(s_->params, s_->asShotTT);
    refreshHistory();
    saveTimer_.start(400);
    requestRender();
    if (!viewer_->isFit()) requestDetailView();
}

Params MainWindow::makeBeforeParams(const Params& p) const {
    Params before = compareTarget_ ? *compareTarget_ : (s_ && !s_->isRaw ? Params::defaultsForImage() : Params::defaults());
    if (!compareTarget_) {
        // 補正前＝撮ったまま。レンズ補正（歪曲・周辺減光・色収差・自動トリミング）は掛けない
        before.enabled.lens = false;
        before.sharpenAmount = before.nrColor = 0;
    }
    if (!compareTarget_) {
        // 補正前も「カメラに合わせる」の下地は同じ（撮ったままの見た目）
        before.profile = p.profile;
        before.camExposure = p.camExposure;
        before.camMatrix = p.camMatrix;
    }
    before.cropX = p.cropX;
    before.cropY = p.cropY;
    before.cropW = p.cropW;
    before.cropH = p.cropH;
    before.cropAngle = p.cropAngle;
    before.enabled.crop = p.enabled.crop && beforeCrop_;
    return before;
}

void MainWindow::setCompareTarget(std::optional<Params> p, const QString& label) {
    compareTarget_ = std::move(p);
    viewer_->setBeforeLabel(label);
    beforeKey_.clear();
    if (viewer_->compare() == CompareMode::Off) {
        viewer_->setCompare(CompareMode::SideAuto);
        compareBtn_->setText(QStringLiteral("比較：並べて比較（自動）"));
    }
    requestRender();
}

void MainWindow::saveSidecar() {
    if (!s_ || !s_->src) return;
    // ★などはライブラリ側で変わっていることがあるので、ファイルを読み直して params だけ差し替える
    const QString sp = sidecarPath(s_->path);
    QJsonObject o;
    QFile in(sp);
    if (in.open(QIODevice::ReadOnly)) {
        o = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
    }
    o["version"] = 1;
    o["params"] = s_->params.toJson();
    QSaveFile f(sp);
    if (!f.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage(QStringLiteral("保存できませんでした：") + QDir::toNativeSeparators(sp), 8000);
        return;
    }
    f.write(QJsonDocument(o).toJson());
    if (!f.commit()) {
        statusBar()->showMessage(QStringLiteral("保存できませんでした：") + QDir::toNativeSeparators(sp), 8000);
        return;
    }
    s_->sidecar = o;
    if (PhotoItem* it = model_->item(s_->path)) {
        if (!it->edited) {
            it->edited = true;
            PhotoMark m{it->rating, it->label, it->flag, true};
            Catalog::get().setMark(s_->path, m);
            model_->refresh(s_->path);
        }
    }
    storeEditedThumb();
}

void MainWindow::storeEditedThumb() {
    if (pendingThumb_.isNull() || pendingThumbPath_.isEmpty()) return;
    const QString ep = editedThumbPath(pendingThumbPath_);
    if (!ep.isEmpty()) pendingThumb_.save(ep, "JPG", 85);
    pendingThumb_ = QImage();
    pendingThumbPath_.clear();
}

void MainWindow::flushSave() {
    if (saveTimer_.isActive()) {
        saveTimer_.stop();
        saveSidecar();
    }
}

// ================================================================ tools

void MainWindow::autoAdjust(float strength, bool wbOnly) {
    if (!s_ || !s_->preview) return;
    AutoInfo info;
    const Params p = autoParams(*s_->preview, s_->src->asShotGains, strength, s_->params, wbOnly,
                                s_->isRaw ? &s_->src->meta : nullptr, &info,
                                s_->src->cameraRef.width > 0 ? &s_->src->cameraRef : nullptr);
    const QString scene = QString::fromStdString(info.scene);
    applyParams(p, true,
                wbOnly ? QStringLiteral("WB：自動")
                       : QStringLiteral("自動調整（%1%）").arg(strength) + (scene.isEmpty() ? QString() : "：" + scene),
                true);
    if (!wbOnly)
        statusBar()->showMessage((scene.isEmpty() ? QString() : QStringLiteral("シーン：") + scene + QStringLiteral("。")) +
                                     QStringLiteral("自動調整しました。各スライダーで調整を続けられます"),
                                 5000);
}

ImageF MainWindow::skinProbeImage() const {
    // 肌色の補正をかける前の色（表示と同じ切り抜き）
    Params q = s_->params;
    q.enabled.skin = false;
    q.skinMaskView = false;
    const ImageF small = downscale(*s_->preview, 800);
    DevelopOptions o;
    o.scale = float(small.width) / std::max(1, s_->src->linear.width) * (float(s_->src->linear.width) / s_->preview->width) *
              (float(s_->preview->width) / std::max(1, s_->src->meta.fullWidth > 0 ? s_->src->meta.fullWidth : s_->src->linear.width));
    o.detail = false;
    o.lens = s_->lens.get();
    return develop(small, q, s_->src->asShotGains, o);
}

void MainWindow::wbPick(int x, int y) {
    if (!s_ || s_->last.isNull()) return;
    if (panel_->skinPickButton()->isChecked()) {
        const ImageF g = skinProbeImage();
        const int cx = std::clamp(int(double(x) / s_->last.width() * g.width), 0, g.width - 1);
        const int cy = std::clamp(int(double(y) / s_->last.height() * g.height), 0, g.height - 1);
        double sum[3] = {0, 0, 0};
        int n = 0;
        for (int yy = std::max(0, cy - 2); yy <= std::min(g.height - 1, cy + 2); ++yy)
            for (int xx = std::max(0, cx - 2); xx <= std::min(g.width - 1, cx + 2); ++xx) {
                for (int c = 0; c < 3; ++c) sum[c] += g.px(xx, yy)[c];
                ++n;
            }
        float hh, ss, vv;
        rgbToHsv(float(sum[0] / n), float(sum[1] / n), float(sum[2] / n), hh, ss, vv);
        panel_->skinPickButton()->setChecked(false);
        if (ss < 0.05f || vv < 0.12f) {
            statusBar()->showMessage(QStringLiteral("色が薄い／暗い場所です。頬など肌の明るい部分をクリックしてください"), 5000);
            return;
        }
        Params p = s_->params;
        p.skinCenterHue = std::round(hh);
        p.skinCenterSat = std::round(ss * 100) / 100;
        p.enabled.skin = true;
        applyParams(p, true, QStringLiteral("肌色：スポイト"), true);
        statusBar()->showMessage(QStringLiteral("この色を肌として扱います。「肌の範囲を表示」で確認できます"), 5000);
        return;
    }
    const QImage& a = s_->last;
    double sum[3] = {0, 0, 0};
    int n = 0;
    for (int yy = std::max(0, y - 3); yy <= std::min(a.height() - 1, y + 3); ++yy)
        for (int xx = std::max(0, x - 3); xx <= std::min(a.width() - 1, x + 3); ++xx) {
            const QRgb c = a.pixel(xx, yy);
            sum[0] += qRed(c) / 255.0;
            sum[1] += qGreen(c) / 255.0;
            sum[2] += qBlue(c) / 255.0;
            ++n;
        }
    if (!n) return;
    float out[3] = {float(sum[0] / n), float(sum[1] / n), float(sum[2] / n)};
    const float mx = std::max({out[0], out[1], out[2]});
    if (mx > 0.98f || mx < 0.03f) {
        statusBar()->showMessage(QStringLiteral("明るすぎる／暗すぎる場所です。白〜グレーの部分をクリックしてください"), 5000);
        return;
    }
    // 表示値 → トーン LUT を逆に引いてリニアへ
    const ToneLut lut = toneLut(s_->params);
    float lin[3];
    for (int c = 0; c < 3; ++c) {
        const auto& t = lut.t[0];
        const int k = int(std::lower_bound(t.begin(), t.end(), out[c]) - t.begin());
        const float g = kLutMax * std::clamp(k, 0, kLutN - 1) / (kLutN - 1);
        lin[c] = std::pow(g, kGamma);
    }
    const auto cur = wbFor(s_->params, s_->src->asShotGains);
    std::array<float, 3> nw{cur[0] * lin[1] / std::max(lin[0], 1e-5f), cur[1], cur[2] * lin[1] / std::max(lin[2], 1e-5f)};
    const auto [t, tint] = fitTempTint(nw);
    panel_->wbPickButton()->setChecked(false);
    panel_->setWb(t, tint, QStringLiteral("WB：スポイト"));
}

void MainWindow::cropTool(bool on) {
    if (!s_) return;
    angleBase_.reset();
    if (on) {
        cropBackup_ = s_->params;
        viewer_->setCropMode(true, CropRect{s_->params.cropX, s_->params.cropY, s_->params.cropW, s_->params.cropH});
        emit panel_->aspectChanged();
        viewer_->setFocus();
    } else {
        commitAngle();
        viewer_->setCropMode(false);
        cropBackup_.reset();
    }
    ++version_;
    requestRender();
}

void MainWindow::nudgeAngle(double delta) {
    if (!s_ || !s_->src) return;
    Params p = s_->params;
    const double a = std::clamp(std::round((p.cropAngle + delta) * 100.0) / 100.0, -45.0, 45.0);
    if (a == p.cropAngle) return;
    p.cropAngle = float(a);
    p.enabled.crop = true;
    viewer_->setAngleHint(QStringLiteral("角度 %1%2°").arg(a > 0 ? "+" : "").arg(a, 0, 'f', std::abs(delta) < 0.05 ? 2 : 1));
    applyParams(fitCropToAngle(p), false, {}, true);
    angleTimer_.start(500);
}

Params MainWindow::fitCropToAngle(Params p) {
    if (!s_ || !s_->src || !viewer_->cropMode()) return p;
    if (!angleBase_) angleBase_ = viewer_->cropRect();
    const CropRect r = fitCrop(*angleBase_, p.cropAngle, s_->src->meta.fullWidth, s_->src->meta.fullHeight);
    p.cropX = float(r.x);
    p.cropY = float(r.y);
    p.cropW = float(r.w);
    p.cropH = float(r.h);
    viewer_->setCropMode(true, r);
    return p;
}

void MainWindow::commitAngle() {
    if (!angleTimer_.isActive() && !dragging_) return;
    angleTimer_.stop();
    viewer_->setAngleHint({});
    if (!s_) return;
    const double a = s_->params.cropAngle;
    applyParams(s_->params, true, QStringLiteral("角度 %1%2°").arg(a > 0 ? "+" : "").arg(a, 0, 'f', 2), false);
}

// ================================================================ rating

PhotoMark MainWindow::markOf(const QString& path) {
    if (PhotoItem* it = model_->item(path)) return PhotoMark{it->rating, it->label, it->flag, it->edited};
    const SidecarMark sm = readSidecarMark(path);
    if (sm.exists) return sm.mark;
    if (auto m = Catalog::get().mark(path)) return *m;
    PhotoMark m;
    const auto r = xmp::readFileRating(path);
    m.rating = r.rating.value_or(0);
    m.label = r.label;
    return m;
}

void MainWindow::saveMark(const QString& path, const PhotoMark& m, const PhotoMark& before) {
    QSettings st;
    const bool raw = isRawPhoto(path);
    const bool hasRdev = QFileInfo::exists(sidecarPath(path));
    // RAW は .rdev（編集していなくても）。JPEG 等は編集済み（.rdev あり）なら .rdev にも
    if (raw || hasRdev) writeSidecarMark(path, m);
    if (raw && st.value("rate/xmp_sidecar", false).toBool()) xmp::writeSidecar(path, m.rating, m.label);
    if (!raw && st.value("rate/write_files", true).toBool() && (m.rating != before.rating || m.label != before.label)) {
        QString err;
        if (xmp::writeFileRating(path, m.rating, m.label, &err).isEmpty())
            statusBar()->showMessage(QStringLiteral("ファイルに書き込めませんでした：") + err, 6000);
    }
    Catalog::get().setMark(path, m);
}

void MainWindow::rate(const std::function<void(PhotoMark&)>& fn, const QString& desc) {
    const QStringList targets = ratingTargets();
    if (targets.isEmpty()) return;
    QStringList linked;
    struct Change { QString path; PhotoMark m; };
    std::vector<Change> changes;
    for (const QString& path : targets) {
        const PhotoMark before = markOf(path);
        PhotoMark m = before;
        fn(m);
        if (m.rating == before.rating && m.label == before.label && m.flag == before.flag) continue;
        saveMark(path, m, before);
        if (PhotoItem* it = model_->item(path)) {
            it->rating = m.rating;
            it->label = m.label;
            it->flag = m.flag;
            model_->refresh(path);
        }
        if (m.rating != before.rating || m.label != before.label) {
            for (const QString& e : Catalog::get().exportsOf(path))
                if (QFileInfo::exists(e)) linked << e;
            changes.push_back({path, m});
        }
    }
    statusBar()->showMessage(QStringLiteral("%1（%2 枚）").arg(desc).arg(targets.size()), 2500);
    if (!linked.isEmpty()) {
        const QString pol = QSettings().value("rate/propagate", "ask").toString();
        bool go = pol == "always";
        if (pol == "ask") {
            QMessageBox box(QMessageBox::Question, QStringLiteral("書き出したファイルにも反映"),
                            QStringLiteral("この写真から書き出したファイルが %1 件あります。★とラベルを反映しますか？").arg(linked.size()),
                            QMessageBox::Yes | QMessageBox::No, this);
            auto* always = new QCheckBox(QStringLiteral("次から聞かない"));
            box.setCheckBox(always);
            go = box.exec() == QMessageBox::Yes;
            if (always->isChecked()) QSettings().setValue("rate/propagate", go ? "always" : "never");
        }
        if (go) {
            int ok = 0;
            for (const auto& c : changes)
                for (const QString& e : Catalog::get().exportsOf(c.path)) {
                    if (!QFileInfo::exists(e)) continue;
                    if (!xmp::writeFileRating(e, c.m.rating, c.m.label).isEmpty()) ++ok;
                    PhotoMark em = Catalog::get().mark(e).value_or(PhotoMark{});
                    em.rating = c.m.rating;
                    em.label = c.m.label;
                    Catalog::get().setMark(e, em);
                    if (PhotoItem* it = model_->item(e)) {
                        it->rating = em.rating;
                        it->label = em.label;
                        model_->refresh(e);
                    }
                }
            statusBar()->showMessage(QStringLiteral("書き出したファイル %1 件にも反映しました").arg(ok), 3000);
        }
    }
    if (!currentPath().isEmpty()) showLibInfo(currentPath());
    if (targets.size() == 1 && QSettings().value("rate/auto_advance", false).toBool()) step(1);
}

void MainWindow::copyFromOther(const QString& path) {
    if (!s_ || !s_->src || path == s_->path) return;
    QFile f(sidecarPath(path));
    if (!f.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(QStringLiteral("その写真はまだ調整されていません"), 3000);
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o.contains("params")) return;
    Params p = Params::fromJson(o.value("params").toObject());
    p.cropX = s_->params.cropX; p.cropY = s_->params.cropY; p.cropW = s_->params.cropW; p.cropH = s_->params.cropH;
    applyParams(p, true, QStringLiteral("調整をコピー：") + QFileInfo(path).fileName(), true);
}

void MainWindow::trashRejected() {
    QStringList rej;
    for (const auto& it : model_->items())
        if (it.flag == -1) rej << it.path;
    if (rej.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("除外した写真はありません"), 3000);
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("ごみ箱へ"),
                              QStringLiteral("除外した %1 枚をごみ箱へ移動します（調整ファイルも一緒に）。よろしいですか？").arg(rej.size()))
        != QMessageBox::Yes)
        return;
    if (s_ && rej.contains(s_->path)) s_.reset();
    int n = 0;
    for (const QString& p : rej) {
        if (QFile::moveToTrash(p)) ++n;
        for (const QString& extra : {sidecarPath(p), xmp::sidecarPath(p)})
            if (QFileInfo::exists(extra)) QFile::moveToTrash(extra);
    }
    statusBar()->showMessage(QStringLiteral("%1 枚をごみ箱へ移動しました").arg(n), 4000);
    openFolder(folder_);
}

void MainWindow::showLinks() {
    const QString path = currentPath();
    if (path.isEmpty()) return;
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("書き出したファイル／元の写真"));
    d.resize(620, 320);
    auto* v = new QVBoxLayout(&d);
    auto* list = new QListWidget;
    const QString src = Catalog::get().sourceOf(path);
    if (!src.isEmpty()) {
        v->addWidget(new QLabel(QStringLiteral("このファイルの元の写真")));
        auto* it = new QListWidgetItem(QDir::toNativeSeparators(src) + (QFileInfo::exists(src) ? "" : QStringLiteral("（見つかりません）")));
        it->setData(Qt::UserRole, src);
        list->addItem(it);
    } else {
        v->addWidget(new QLabel(QStringLiteral("この写真から書き出したファイル")));
        for (const QString& e : Catalog::get().exportsOf(path)) {
            auto* it = new QListWidgetItem(QDir::toNativeSeparators(e) + (QFileInfo::exists(e) ? "" : QStringLiteral("（見つかりません）")));
            it->setData(Qt::UserRole, e);
            list->addItem(it);
        }
    }
    v->addWidget(list, 1);
    auto* row = new QHBoxLayout;
    auto* open = new QPushButton(QStringLiteral("その場所を開く"));
    auto* relink = new QPushButton(QStringLiteral("場所を指定し直す…"));
    auto* unlink = new QPushButton(QStringLiteral("紐付けを外す"));
    auto* close = new QPushButton(QStringLiteral("閉じる"));
    row->addWidget(open);
    row->addWidget(relink);
    row->addWidget(unlink);
    row->addStretch(1);
    row->addWidget(close);
    v->addLayout(row);
    auto sel = [list] { return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString(); };
    connect(open, &QPushButton::clicked, &d, [this, sel] {
        const QString p = sel();
        if (p.isEmpty() || !QFileInfo::exists(p)) return;
        openFolder(QFileInfo(p).absolutePath(), p);
    });
    connect(relink, &QPushButton::clicked, &d, [this, &d, list, sel, path, src] {
        const QString old = sel();
        if (old.isEmpty()) return;
        const QString nw = QFileDialog::getOpenFileName(&d, QStringLiteral("新しい場所"), QFileInfo(old).absolutePath());
        if (nw.isEmpty()) return;
        if (src.isEmpty()) Catalog::get().relink(old, QDir::toNativeSeparators(nw));
        else { Catalog::get().removeLink(src, path); Catalog::get().addLink(QDir::toNativeSeparators(nw), path); }
        list->currentItem()->setText(QDir::toNativeSeparators(nw));
        list->currentItem()->setData(Qt::UserRole, QDir::toNativeSeparators(nw));
    });
    connect(unlink, &QPushButton::clicked, &d, [list, sel, path, src] {
        const QString p = sel();
        if (p.isEmpty()) return;
        if (src.isEmpty()) Catalog::get().removeLink(path, p);
        else Catalog::get().removeLink(src, path);
        delete list->currentItem();
    });
    connect(close, &QPushButton::clicked, &d, &QDialog::accept);
    d.exec();
}

void MainWindow::toggleFullscreen() {
    if (isFullScreen()) {
        showNormal();
        menuBar()->show();
        toolbarWidget_->show();
        statusBar()->show();
        filterBar_->show();
        film_->show();
    } else {
        showFullScreen();
        menuBar()->hide();
        toolbarWidget_->hide();
        statusBar()->hide();
        if (!developMode_) setMode(true);
        film_->hide();
    }
    for (auto* d : docks_) d->setVisible(developMode_ && !isFullScreen());
}

// ================================================================ copy / paste / batch

void MainWindow::copySettings(bool choose) {
    QString src;
    Params p;
    if (developMode_ && s_ && s_->src) {
        flushSave();
        src = s_->path;
        p = s_->params;
    } else {
        src = currentPath();
        if (src.isEmpty()) return;
        p = readParams(src);
    }
    if (choose) {
        CopyDialog d(this);
        if (d.exec() != QDialog::Accepted) return;
    }
    QSettings st;
    clipboard_ = p;
    clipGroups_ = st.value("paste/groups", defaultPasteGroups()).toStringList();
    clipMatch_ = st.value("paste/match_exposure", false).toBool();
    clipSrc_ = src;
    clipLum_ = -1;
    if (s_ && s_->path == src && s_->preview) clipLum_ = photoLuminance(*s_->preview, wbFor(p, s_->src->asShotGains));
    statusBar()->showMessage(QStringLiteral("調整をコピーしました（%1 項目）。Ctrl＋V で選んでいる写真に貼り付けます")
                                 .arg(clipGroups_.size()), 4000);
}

Params MainWindow::jobResult(const BatchJob& j, const Params& cur, const LoadedRaw& src) const {
    switch (j.mode) {
    case BatchJob::Auto: {
        const ImageF prev = downscale(src.linear, kPreviewEdge);
        return autoParams(prev, src.asShotGains, 100, cur, false, isRawPhoto(j.path) ? &src.meta : nullptr, nullptr,
                          src.cameraRef.width > 0 ? &src.cameraRef : nullptr);
    }
    case BatchJob::Thumb:
        return cur;
    default: {
        Params np = copyKeys(cur, j.src, j.keys);
        if (j.matchExposure && j.srcLum > 0 && j.keys.contains("exposure") && !src.linear.data.empty()) {
            const double tl = photoLuminance(src.linear, wbFor(np, src.asShotGains));
            const double base = (j.src.cameraLook() ? j.src.camExposure : 0) - (np.cameraLook() ? np.camExposure : 0);
            if (tl > 0) np.exposure = float(std::round(std::clamp(j.src.exposure + base + std::log2(j.srcLum / tl), -5.0, 5.0) * 100) / 100);
        }
        return np;
    }
    }
}

void MainWindow::pasteSettings() {
    if (!clipboard_) {
        statusBar()->showMessage(QStringLiteral("先に Ctrl＋C で調整をコピーしてください"), 3000);
        return;
    }
    QStringList targets = selectedPaths();
    targets.removeAll(clipSrc_);
    if (targets.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("貼り付ける写真を選んでください（コピー元には貼り付けません）"), 3000);
        return;
    }
    QStringList keys;
    for (const auto& g : pasteGroups())
        if (clipGroups_.contains(g.id)) keys << g.keys;
    keys.removeDuplicates();
    std::vector<BatchJob> jobs;
    for (const QString& t : targets) {
        BatchJob j;
        j.mode = BatchJob::Paste;
        j.path = t;
        j.src = *clipboard_;
        j.keys = keys;
        j.matchExposure = clipMatch_;
        j.srcLum = clipLum_;
        jobs.push_back(j);
    }
    // 露出をそろえるのにコピー元の明るさが必要なら、先に測る
    if (clipMatch_ && clipLum_ < 0) {
        auto f = loadPhoto(clipSrc_, true);
        if (f->error.empty()) clipLum_ = photoLuminance(f->linear, wbFor(*clipboard_, f->asShotGains));
        for (auto& j : jobs) j.srcLum = clipLum_;
    }
    runBatch(std::move(jobs), QStringLiteral("調整を貼り付け"), false);
}

void MainWindow::pastePrevious() {
    if (!s_ || !s_->src || prevPath_.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("前に開いた写真がありません"), 3000);
        return;
    }
    bool ex = false;
    const Params prev = readParams(prevPath_, &ex);
    if (!ex) {
        statusBar()->showMessage(QStringLiteral("前の写真（%1）はまだ調整されていません").arg(QFileInfo(prevPath_).fileName()), 3000);
        return;
    }
    QStringList groups;
    for (const auto& g : pasteGroups())
        if (QString(g.id) != "crop") groups << g.id;
    applyParams(mergeGroups(s_->params, prev, groups), true,
                QStringLiteral("前の写真と同じ：") + QFileInfo(prevPath_).fileName(), true);
}

void MainWindow::batchAuto() {
    const QStringList targets = selectedPaths();
    if (targets.isEmpty()) return;
    std::vector<BatchJob> jobs;
    for (const QString& t : targets) {
        BatchJob j;
        j.mode = BatchJob::Auto;
        j.path = t;
        jobs.push_back(j);
    }
    runBatch(std::move(jobs), QStringLiteral("まとめて自動調整"), false);
}

void MainWindow::runBatch(std::vector<BatchJob> jobs, const QString& title, bool silent) {
    if (jobs.empty()) return;
    if (batchRunning_) {
        // 前の処理が終わってから（同じ写真への書き込みが重ならないように）
        QTimer::singleShot(200, this, [this, jobs = std::move(jobs), title, silent]() mutable { runBatch(std::move(jobs), title, silent); });
        return;
    }
    // 開いている写真はその場で（履歴にも残す）
    const bool user = jobs.front().mode == BatchJob::Paste || jobs.front().mode == BatchJob::Auto;
    if (user) {
        flushSave();
        undoBackup_.clear();
    }
    std::vector<BatchJob> rest;
    int doneHere = 0;
    for (auto& j : jobs) {
        if (user) {
            QFile f(j.path + ".rdev");
            undoBackup_.push_back({j.path, f.open(QIODevice::ReadOnly) ? std::optional<QByteArray>(f.readAll()) : std::nullopt});
        }
        if (s_ && s_->src && j.path == s_->path && j.mode != BatchJob::Thumb) {
            Params np;
            if (j.mode == BatchJob::Auto) {
                AutoInfo info;
                np = autoParams(*s_->preview, s_->src->asShotGains, 100, s_->params, false, s_->isRaw ? &s_->src->meta : nullptr,
                                &info, s_->src->cameraRef.width > 0 ? &s_->src->cameraRef : nullptr);
            } else {
                BatchJob k = j;
                if (k.matchExposure && k.srcLum > 0) {
                    np = copyKeys(s_->params, k.src, k.keys);
                    const double tl = photoLuminance(*s_->preview, wbFor(np, s_->src->asShotGains));
                    const double base = (k.src.cameraLook() ? k.src.camExposure : 0) - (np.cameraLook() ? np.camExposure : 0);
                    if (tl > 0) np.exposure = float(std::round(std::clamp(k.src.exposure + base + std::log2(k.srcLum / tl), -5.0, 5.0) * 100) / 100);
                } else {
                    np = copyKeys(s_->params, k.src, k.keys);
                }
            }
            const bool sync = syncAct_->isChecked();
            syncAct_->setChecked(false);  // 貼り付けがさらに同期されないように
            applyParams(np, true, title, true);
            syncAct_->setChecked(sync);
            flushSave();
            ++doneHere;
        } else {
            rest.push_back(j);
        }
    }
    if (rest.empty()) {
        if (user) {
            statusBar()->showMessage(QStringLiteral("%1：%2 枚").arg(title).arg(doneHere), 6000);
            undoBtn_->show();
            undoTimer_.start(15000);
        }
        return;
    }
    batchRunning_ = true;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QPointer<QProgressDialog> prog;
    if (!silent && rest.size() >= 2) {
        prog = new QProgressDialog(title + QStringLiteral("…"), QStringLiteral("中止"), 0, int(rest.size()), this);
        prog->setWindowModality(Qt::WindowModal);
        prog->setMinimumDuration(400);
        connect(prog, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    }
    auto shared = std::make_shared<std::vector<BatchJob>>(std::move(rest));
    auto* watcher = new QFutureWatcher<void>(this);
    QPointer<MainWindow> self(this);
    watcher->setFuture(QtConcurrent::run([this, shared, cancel, self, prog] {
        int n = 0;
        for (const BatchJob& j : *shared) {
            if (*cancel) break;
            BatchDone d;
            d.path = j.path;
            bool ex = false;
            Params cur = readParams(j.path, &ex);
            auto src = loadPhoto(j.path, true);
            if (!ex && src->error.empty()) cur = initialParams(j.path, *src);
            Params np = cur;
            if (src->error.empty()) np = jobResult(j, cur, *src);
            if (j.mode != BatchJob::Thumb) d.ok = writeParams(j.path, np);
            if (src->error.empty() && (ex || j.mode != BatchJob::Thumb)) d.thumb = renderEditedThumb(j.path, *src, np);
            ++n;
            QMetaObject::invokeMethod(self, [this, self, d, n, prog] {
                if (!self) return;
                if (PhotoItem* it = model_->item(d.path)) {
                    if (!it->edited && QFileInfo::exists(d.path + ".rdev")) {
                        it->edited = true;
                        Catalog::get().setMark(d.path, PhotoMark{it->rating, it->label, it->flag, true});
                    }
                    model_->setThumb(d.path, d.thumb);
                }
                if (prog) prog->setValue(n);
            });
        }
        return;
    }));
    const int total = int(shared->size()) + doneHere;
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, prog, title, silent, user, total, cancel] {
        watcher->deleteLater();
        if (prog) prog->deleteLater();
        batchRunning_ = false;
        if (silent) {
            statusBar()->showMessage(QStringLiteral("%1：ほか %2 枚に反映").arg(title).arg(total), 2500);
            return;
        }
        statusBar()->showMessage(QStringLiteral("%1：%2 枚%3").arg(title).arg(total).arg(*cancel ? QStringLiteral("（途中で中止）") : QString()), 8000);
        if (user) {
            undoBtn_->show();
            undoTimer_.start(15000);
        }
        if (qEnvironmentVariableIsSet("RD_TEST_SCRIPT")) {
            std::printf("batch %s: %d\n", title.toUtf8().constData(), total);
            std::fflush(stdout);
        }
    });
}

void MainWindow::undoBatch() {
    undoBtn_->hide();
    if (undoBackup_.empty() || batchRunning_) return;
    std::vector<BatchJob> thumbs;
    for (const auto& [path, data] : undoBackup_) {
        if (data) {
            QSaveFile f(path + ".rdev");
            if (f.open(QIODevice::WriteOnly)) {
                f.write(*data);
                f.commit();
            }
        } else {
            QFile::remove(path + ".rdev");
        }
        if (s_ && s_->path == path) {
            const Params old = readParams(path);
            const bool sync = syncAct_->isChecked();
            syncAct_->setChecked(false);
            applyParams(old, true, QStringLiteral("貼り付けを取り消し"), true);
            syncAct_->setChecked(sync);
            saveTimer_.stop();
        } else {
            BatchJob j;
            j.mode = BatchJob::Thumb;
            j.path = path;
            thumbs.push_back(j);
            if (!data) model_->setThumb(path, QImage());
        }
    }
    statusBar()->showMessage(QStringLiteral("%1 枚を元に戻しました").arg(undoBackup_.size()), 4000);
    undoBackup_.clear();
    runBatch(std::move(thumbs), QStringLiteral("サムネイル更新"), true);
}

// ================================================================ export

void MainWindow::exportDialog() {
    if (exporting_) return;
    flushSave();
    const QStringList sel = selectedPaths();
    const QStringList all = filteredPaths();
    if (all.isEmpty()) return;
    ExportDialog dlg(sel.size(), all.size(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const ExportOptions o = dlg.options();
    QStringList targets;
    const QString mode = dlg.targetMode();
    if (mode == "selected") targets = sel;
    else
        for (const QString& p : all) {
            const PhotoMark m = markOf(p);
            if (mode == "all" || (mode == "star" && m.rating >= dlg.minStar()) || (mode == "pick" && m.flag == 1)) targets << p;
        }
    if (targets.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("書き出し"), QStringLiteral("対象の写真がありません。"));
        return;
    }
    if (!o.sameFolder) QDir().mkpath(o.folder);

    struct Job { QString src; PhotoMark mark; };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (const QString& p : targets) jobs->push_back({p, markOf(p)});
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto* prog = new QProgressDialog(QStringLiteral("書き出し中…"), QStringLiteral("中止"), 0, int(jobs->size()), this);
    prog->setWindowModality(Qt::WindowModal);
    prog->setMinimumDuration(0);
    prog->setValue(0);
    connect(prog, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    exporting_ = true;

    struct Result { QStringList done, failed; QStringList skipped; };
    auto watcher = new QFutureWatcher<Result>(this);
    QPointer<QProgressDialog> pp(prog);
    auto progress = [this, pp](int i, const QString& name) {
        QMetaObject::invokeMethod(this, [pp, i, name] {
            if (pp) {
                pp->setValue(i);
                pp->setLabelText(QStringLiteral("書き出し中… ") + name);
            }
        });
    };
    watcher->setFuture(QtConcurrent::run([jobs, o, cancel, progress] {
        Result res;
        int n = 0;
        for (const Job& j : *jobs) {
            if (*cancel) break;
            progress(n, QFileInfo(j.src).fileName());
            ++n;
            const QString out = exportPath(j.src, o, n);
            if (out.isEmpty()) {
                res.skipped << j.src;
                continue;
            }
            auto f = loadAny(j.src, false);
            if (!f->error.empty()) {
                res.failed << QFileInfo(j.src).fileName() + "：" + QString::fromStdString(f->error);
                continue;
            }
            Params p = initialParams(j.src, *f);
            QFile sf(j.src + ".rdev");
            if (sf.open(QIODevice::ReadOnly)) {
                const QJsonObject so = QJsonDocument::fromJson(sf.readAll()).object();
                if (so.contains("params")) p = Params::fromJson(so.value("params").toObject());
            }
            const LensInfo lens = LensDb::get().identify(f->meta);
            DevelopOptions dop;
            dop.lens = &lens;
            const ImageF g = develop(f->linear, p, f->asShotGains, dop);
            const QString err = writeExport(g, out, o, j.mark.rating, j.mark.label, &f->meta);
            if (err.isEmpty()) {
                res.done << out;
                if (o.addToLibrary) {
                    Catalog::get().addLink(QDir::toNativeSeparators(j.src), QDir::toNativeSeparators(out));
                    PhotoMark em = j.mark;
                    em.flag = 0;
                    em.edited = false;
                    Catalog::get().setMark(QDir::toNativeSeparators(out), em);
                }
            } else {
                res.failed << QFileInfo(j.src).fileName() + "：" + err;
            }
        }
        progress(n, QString());
        return res;
    }));
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, prog, cancel, o] {
        const Result r = watcher->result();
        watcher->deleteLater();
        prog->close();
        prog->deleteLater();
        exporting_ = false;
        QString msg = QStringLiteral("%1 枚を書き出しました。").arg(r.done.size());
        if (!r.skipped.isEmpty()) msg += QStringLiteral("\n同名ファイルがあったので %1 枚は書き出しませんでした。").arg(r.skipped.size());
        if (*cancel) msg += QStringLiteral("\n途中で中止しました。");
        if (!r.failed.isEmpty()) msg += QStringLiteral("\n\n失敗：\n") + r.failed.mid(0, 10).join("\n");
        // 書き出し数の表示を更新
        for (int i = 0; i < model_->rowCount(); ++i) {
            PhotoItem* it = model_->itemAt(i);
            const int n = Catalog::get().exportsOf(it->path).size();
            if (n != it->exports) {
                it->exports = n;
                model_->refresh(it->path);
            }
        }
        if (qEnvironmentVariableIsSet("RD_TEST_SCRIPT")) {
            std::printf("export: done %d failed %d skipped %d\n", int(r.done.size()), int(r.failed.size()), int(r.skipped.size()));
            for (const QString& f : r.failed) std::printf("  fail: %s\n", f.toUtf8().constData());
            for (const QString& f : r.done) std::printf("  out: %s\n", f.toUtf8().constData());
            std::fflush(stdout);
            return;
        }
        QMessageBox box(QMessageBox::Information, QStringLiteral("書き出し"), msg, QMessageBox::Ok, this);
        QPushButton* openBtn = nullptr;
        if (!r.done.isEmpty()) openBtn = box.addButton(QStringLiteral("書き出し先を開く"), QMessageBox::ActionRole);
        box.exec();
        if (openBtn && box.clickedButton() == openBtn)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(r.done.first()).absolutePath()));
    });
}

// ================================================================ test

void MainWindow::runScriptedTest(const QString& prefix) {
    struct Step {
        int ms;
        std::function<void()> fn;
    };
    auto shot = [this, prefix](int n) {
        grab().save(QString("%1_%2.png").arg(prefix).arg(n));
        std::printf("shot %d: version %llu, history %d, compare %d, fit %d, zoom %.3f, roi %d\n", n,
                    (unsigned long long)version_, s_ ? int(s_->history.size()) : -1, int(viewer_->compare()),
                    int(viewer_->isFit()), viewer_->zoom(), int(!viewer_->visibleFullRect().isEmpty()));
        if (s_ && s_->lens) std::printf("  lens: %s (active %d)\n", s_->lens->status.c_str(), int(lensActive(s_->params, s_->lens.get())));
        std::fflush(stdout);
    };
    auto dump = [this](const char* tag) {
        for (int i = 0; i < model_->rowCount(); ++i) {
            const QString p = model_->itemAt(i)->path;
            bool ex = false;
            const Params q = readParams(p, &ex);
            std::printf("%s %s: rdev %d ev %.2f c %.0f temp %.0f crop %.2f\n", tag, QFileInfo(p).fileName().toUtf8().constData(), int(ex),
                        q.exposure, q.contrast, q.temp, q.cropW);
        }
        std::fflush(stdout);
    };
    if (qEnvironmentVariableIsSet("RD_TEST_LOCAL")) {
        QTimer::singleShot(5000, this, [this, prefix] {
            panel_->setLocalTool(DevelopPanel::ToolLinear);
            localPress({0.5, 0.0}, {});
            localMove({0.5, 0.2}, true, {});
            localMove({0.5, 0.35}, true, {});
            localRelease({0.5, 0.35});
            Params P = s_->params;
            P.masks[0].exposure = -1.2f;
            applyParams(P, true, "test", true);
            panel_->setLocalTool(DevelopPanel::ToolRadial);
            localPress({0.62, 0.3}, {});
            localMove({0.75, 0.4}, true, {});
            localRelease({0.75, 0.4});
            P = s_->params;
            P.masks[1].exposure = 0.8f;
            P.masks[1].temp = 30;
            applyParams(P, true, "test", true);
            panel_->setLocalTool(DevelopPanel::ToolBrush);
            localPress({0.1, 0.85}, {});
            for (int i = 1; i <= 20; ++i) localMove({0.1 + i * 0.02, 0.85}, true, {});
            localRelease({0.5, 0.85});
            P = s_->params;
            P.masks[2].saturation = -100;
            applyParams(P, true, "test", true);
            panel_->setLocalTool(DevelopPanel::ToolSpot);
            localPress({0.58, 0.36}, {});
            localRelease({0.58, 0.36});
            std::printf("local: masks %d spots %d dabs %d hist %d\n", int(s_->params.masks.size()), int(s_->params.spots.size()),
                        int(s_->params.masks.size() > 2 ? s_->params.masks[2].dabs.size() : 0), int(s_->history.size()));
            for (const auto& m : s_->params.masks)
                std::printf("  %s %.2f,%.2f %.2f,%.2f ev %.1f\n", m.type.toUtf8().constData(), m.x0, m.y0, m.x1, m.y1, m.exposure);
            std::fflush(stdout);
            QTimer::singleShot(2500, this, [this, prefix] {
                grab().save(prefix + "_local1.png");
                panel_->setLocalTool(DevelopPanel::ToolRadial);
                panel_->selectMask(1);
                for (auto* cb : panel_->findChildren<QCheckBox*>())
                    if (cb->text() == QStringLiteral("範囲を表示")) cb->setChecked(true);
                QTimer::singleShot(2500, this, [this, prefix] {
                    std::printf("show %d overlay %d sel %d\n", s_->params.showMaskIndex, int(panel_->maskOverlay()), panel_->selectedMask());
                    std::fflush(stdout);
                    grab().save(prefix + "_local2.png");
                    for (auto* cb : panel_->findChildren<QCheckBox*>())
                        if (cb->text() == QStringLiteral("範囲を表示")) cb->setChecked(false);
                    panel_->setLocalTool(DevelopPanel::ToolSpot);
                    viewer_->zoom100();
                    QTimer::singleShot(4000, this, [this, prefix] {
                        grab().save(prefix + "_local3.png");
                        QApplication::quit();
                    });
                });
            });
        });
        return;
    }
    if (qEnvironmentVariableIsSet("RD_TEST_SKIN")) {
        QTimer::singleShot(5000, this, [this, prefix] {
            emit panel_->skinAutoRequested(2);
            std::printf("skin: hue %.0f sat %.0f lum %.0f uni %.0f/%.0f/%.0f center %.0f %.2f\n", s_->params.skinHue, s_->params.skinSat,
                        s_->params.skinLum, s_->params.skinUniHue, s_->params.skinUniSat, s_->params.skinUniLum, s_->params.skinCenterHue,
                        s_->params.skinCenterSat);
            std::fflush(stdout);
            QTimer::singleShot(2500, this, [this, prefix] {
                grab().save(prefix + "_skin1.png");
                for (auto* cb : panel_->findChildren<QCheckBox*>())
                    if (cb->text() == QStringLiteral("肌の範囲を表示")) cb->setChecked(true);
                QTimer::singleShot(2500, this, [this, prefix] {
                    grab().save(prefix + "_skin2.png");
                    QApplication::quit();
                });
            });
        });
        return;
    }
    if (qEnvironmentVariableIsSet("RD_TEST_LENS")) {
        QTimer::singleShot(5000, this, [this, prefix] {
            for (auto* cb : panel_->findChildren<QComboBox*>())
                if (cb->count() > 1 && cb->itemText(0).startsWith(QStringLiteral("自動（EXIF"))) {
                    const int i = cb->findData("Canon");
                    cb->setCurrentIndex(i);
                    emit cb->activated(i);
                    for (auto* sa = qobject_cast<QWidget*>(cb->parent()); sa; sa = sa->parentWidget())
                        if (auto* a = qobject_cast<QScrollArea*>(sa)) a->ensureWidgetVisible(cb);
                    std::printf("makers %d\n", cb->count());
                }
            for (auto* cb : panel_->findChildren<QComboBox*>())
                if (cb->count() > 1 && cb->itemText(0) == QStringLiteral("レンズを選んでください")) {
                    cb->hidePopup();
                    std::printf("canon lenses %d: %s / %s / %s\n", cb->count() - 1, cb->itemText(1).toUtf8().constData(),
                                cb->itemText(2).toUtf8().constData(), cb->itemText(cb->count() - 1).toUtf8().constData());
                    auto* v = cb->view();
                    v->resize(420, 640);
                    v->grab().save(prefix + "_lenslist.png");
                }
            std::fflush(stdout);
            QTimer::singleShot(1500, this, [this, prefix] {
                grab().save(prefix + "_lens.png");
                QApplication::quit();
            });
        });
        return;
    }
    if (qEnvironmentVariableIsSet("RD_TEST_BATCH")) {
        auto bsteps = std::make_shared<std::vector<Step>>(std::vector<Step>{
            {5000, [=, this] {
                 // 1 枚目を調整してコピー
                 Params p = s_->params;
                 p.exposure = 1.0f;
                 p.contrast = 30;
                 p.temp = 3000;
                 p.wbMode = "custom";
                 p.cropW = 0.5f;
                 applyParams(p, true, "test", true);
                 QSettings().setValue("paste/groups", defaultPasteGroups());
                 QSettings().setValue("paste/match_exposure", true);
                 copySettings(false);
                 grid_->selectAll();
                 pasteSettings();
             }},
            {6000, [=, this] {
                 dump("paste");
                 undoBatch();
             }},
            {5000, [=, this] {
                 dump("undo");
                 grid_->selectAll();
                 batchAuto();
             }},
            {9000, [=, this] {
                 dump("auto");
                 syncAct_->setChecked(true);
                 grid_->selectionModel()->select(grid_->model()->index(0, 0), QItemSelectionModel::Select);
                 grid_->selectAll();
                 Params p = s_->params;
                 p.saturation = 25;
                 applyParams(p, true, "sync", true);
             }},
            {6000, [=, this] {
                 for (int i = 0; i < model_->rowCount(); ++i)
                     std::printf("sync %s: sat %.0f\n", QFileInfo(model_->itemAt(i)->path).fileName().toUtf8().constData(),
                                 readParams(model_->itemAt(i)->path).saturation);
                 shot(1);
                 setMode(false);
             }},
            {1500, [=, this] {
                 shot(2);
                 QApplication::quit();
             }},
        });
        auto run = std::make_shared<std::function<void(size_t)>>();
        *run = [this, bsteps, run](size_t i) {
            if (i >= bsteps->size()) return;
            QTimer::singleShot((*bsteps)[i].ms, this, [bsteps, run, i] {
                (*bsteps)[i].fn();
                (*run)(i + 1);
            });
        };
        (*run)(0);
        return;
    }
    auto steps = std::make_shared<std::vector<Step>>(std::vector<Step>{
        {4000, [=, this] { shot(1); autoAdjust(100, false); }},
        {1500, [=, this] {
             Params p = s_->params;
             p.lift = {-0.3f, -0.1f, 0.0f};
             p.gain = {0.3f, 0.2f, 0.0f};
             p.hslSat[5] = 60;
             p.curve[0] = {{0, 0}, {0.3f, 0.25f}, {0.7f, 0.8f}, {1, 1}};
             applyParams(p, true, "test", true);
         }},
        {1500, [=, this] {
             viewer_->setCompare(CompareMode::SideH);
             shot(2);
         }},
        {800, [=, this] {
             Params p = s_->params;
             p.enabled.wheels = false;
             p.enabled.hsl = false;
             applyParams(p, true, "off", true);
         }},
        {1500, [=, this] {
             viewer_->setCompare(CompareMode::WipeH);
             shot(3);
             jumpHistory(0);
         }},
        {1500, [=, this] {
             viewer_->setCompare(CompareMode::Off);
             viewer_->zoom100();
         }},
        {6000, [=, this] {
             shot(4);
             Params p = s_->params;
             p.exposure = 0.8f;
             applyParams(p, false, "drag");   // スライダー操作中
         }},
        {1500, [=, this] {
             Params p = s_->params;
             p.exposure = 1.0f;
             applyParams(p, true, "drag end");
         }},
        {2500, [=, this] {
             shot(5);
             viewer_->setCompare(CompareMode::WipeV);
         }},
        {2500, [=, this] {
             shot(6);
             std::printf("  before roi: %d\n", int(viewer_->hasBeforeRoi()));
             viewer_->setCompare(CompareMode::Off);
             viewer_->zoomFit();
             panel_->cropButton()->setChecked(true);
             for (int i = 0; i < 3; ++i) nudgeAngle(1.0);
             nudgeAngle(-0.1);
             nudgeAngle(0.01);
         }},
        {300, [=, this] { shot(9); }},
        {1500, [=, this] {
             std::printf("angle: %.2f history '%s' crop %d\n", s_->params.cropAngle,
                         s_->history[s_->historyPos].first.toUtf8().constData(), int(viewer_->cropMode()));
             panel_->cropButton()->setChecked(false);
             flushSave();
             // 評価
             rate([](PhotoMark& k) { k.rating = 4; }, "test");
             rate([](PhotoMark& k) { k.label = "red"; k.flag = 1; }, "test");
             setMode(false);
         }},
        {2000, [=, this] {
             shot(7);
             const PhotoMark m = markOf(currentPath());
             const SidecarMark sm = readSidecarMark(currentPath());
             std::printf("mark: rating %d label %s flag %d edited %d sidecar %d/%s/%d params %d\n", m.rating,
                         m.label.toUtf8().constData(), m.flag, int(m.edited), sm.mark.rating, sm.mark.label.toUtf8().constData(),
                         sm.mark.flag, int(s_ && s_->sidecar.contains("params")));
             std::fflush(stdout);
             // 書き出し（JPEG・PNG・TIFF）
             QSettings st;
             const QString out = QDir::tempPath() + "/rd_export_test";
             QDir(out).removeRecursively();
             ExportOptions o;
             o.folder = out;
             o.fmt = "jpeg";
             o.longEdge = 1200;
             o.addToLibrary = true;
             st.setValue("rate/propagate", "always");
             st.setValue("export/last", QJsonDocument(o.toJson()).toJson(QJsonDocument::Compact));
             QTimer::singleShot(300, this, [this] {
                 for (auto* w : QApplication::topLevelWidgets())
                     if (auto* d = qobject_cast<ExportDialog*>(w)) d->accept();
             });
             exportDialog();
         }},
        {8000, [=, this] {
             std::printf("exporting %d\n", int(exporting_));
             rate([](PhotoMark& k) { k.rating = 5; }, "propagate");
             const QString out = QDir::tempPath() + "/rd_export_test";
             for (const QFileInfo& fi : QDir(out).entryInfoList(QDir::Files)) {
                 const auto r = xmp::readFileRating(fi.absoluteFilePath());
                 std::printf("exported %s: rating %d label %s src %s\n", fi.fileName().toUtf8().constData(), r.rating.value_or(-1),
                             r.label.toUtf8().constData(), Catalog::get().sourceOf(QDir::toNativeSeparators(fi.absoluteFilePath())).isEmpty() ? "none" : "linked");
             }
             std::fflush(stdout);
             shot(8);
             QApplication::quit();
         }},
    });
    auto run = std::make_shared<std::function<void(size_t)>>();
    *run = [steps, run](size_t i) {
        if (i >= steps->size()) return;
        QTimer::singleShot((*steps)[i].ms, [steps, run, i] {
            (*steps)[i].fn();
            (*run)(i + 1);
        });
    };
    (*run)(0);
}

// ================================================================ window

void MainWindow::closeEvent(QCloseEvent* e) {
    flushSave();
    QSettings st;
    st.setValue("win/geometry", saveGeometry());
    st.setValue("win/state", saveState());
    QMainWindow::closeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString p = urls.first().toLocalFile();
    if (QFileInfo(p).isDir())
        openFolder(p);
    else
        openFolder(QFileInfo(p).absolutePath(), p);
}

}  // namespace rd
