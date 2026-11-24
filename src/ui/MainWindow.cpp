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

#include "engine/Auto.h"
#include "engine/Color.h"
#include "engine/Pipeline.h"
#include "ui/DevelopPanel.h"
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
    if (isRawPath(path)) return std::make_shared<LoadedRaw>(loadRaw(readAll(path), half));
    return loadImageFile(path, half);
}

QImage toQImage(const ImageF& g) {
    const auto rgb = toRgb8(g);
    QImage img(g.width, g.height, QImage::Format_RGB888);
    for (int y = 0; y < g.height; ++y) memcpy(img.scanLine(y), rgb.data() + static_cast<size_t>(y) * g.width * 3, g.width * 3);
    return img;
}

QString sidecarPath(const QString& path) { return path + ".rdev"; }

QString geomKey(const Params& p) {
    return QString("%1,%2,%3,%4,%5,%6|%7,%8,%9,%10,%11,%12,%13")
        .arg(p.cropX).arg(p.cropY).arg(p.cropW).arg(p.cropH).arg(p.cropAngle).arg(p.enabled.crop)
        .arg(p.enabled.lens).arg(p.lensProfile).arg(p.lensKey).arg(p.lensDistortion).arg(p.lensCa)
        .arg(p.lensAutotrim).arg(p.lensManualDistortion);
}

}  // namespace

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
    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    viewer_ = new Viewer;
    viewer_->clear(QStringLiteral("写真を開くか、ここにドラッグしてください"));
    v->addWidget(viewer_, 1);
    film_ = new QListWidget;
    film_->setViewMode(QListView::IconMode);
    film_->setFlow(QListView::LeftToRight);
    film_->setWrapping(false);
    film_->setIconSize(QSize(120, 90));
    film_->setFixedHeight(130);
    film_->setMovement(QListView::Static);
    film_->setSpacing(4);
    film_->setToolTip(QStringLiteral("中クリック：その写真の調整を今の写真にコピー"));
    connect(film_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
        if (it) openFile(it->data(Qt::UserRole).toString());
    });
    v->addWidget(film_);
    setCentralWidget(central);

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
    for (auto* d : {dockScopes, dockPanel, dockHist})
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
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
            [this](const Params& p, bool final, const QString& desc) { applyParams(p, final, desc); });
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
    connect(viewer_, &Viewer::probe, scopes_, &ScopesPanel::setProbe);
    connect(viewer_, &Viewer::wantHiRes, this, &MainWindow::requestDetailView);
    connect(viewer_, &Viewer::viewChanged, this, [this] { viewTimer_.start(30); });
    connect(viewer_, &Viewer::cropChanged, this, [this](const CropRect& r, bool final) {
        if (!s_) return;
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
    add(file, QStringLiteral("JPEG で書き出し…"), QKeySequence("Ctrl+Shift+E"), [this] { exportJpeg(); }, true);
    file->addSeparator();
    add(file, QStringLiteral("終了"), QKeySequence::Quit, [this] { close(); });

    QMenu* edit = menuBar()->addMenu(QStringLiteral("編集"));
    add(edit, QStringLiteral("元に戻す"), QKeySequence::Undo, [this] { if (s_ && s_->historyPos > 0) jumpHistory(s_->historyPos - 1); });
    add(edit, QStringLiteral("やり直す"), QKeySequence("Ctrl+Y"), [this] {
        if (s_ && s_->historyPos + 1 < int(s_->history.size())) jumpHistory(s_->historyPos + 1);
    });
    edit->addSeparator();
    add(edit, QStringLiteral("調整をコピー"), QKeySequence("Ctrl+Shift+C"), [this] {
        if (s_) { clipboard_ = s_->params; statusBar()->showMessage(QStringLiteral("調整をコピーしました"), 2500); }
    });
    add(edit, QStringLiteral("調整をペースト（切り抜き以外）"), QKeySequence("Ctrl+Shift+V"), [this] {
        if (!s_ || !clipboard_) return;
        Params p = *clipboard_;
        p.cropX = s_->params.cropX; p.cropY = s_->params.cropY; p.cropW = s_->params.cropW;
        p.cropH = s_->params.cropH; p.cropAngle = s_->params.cropAngle; p.cropAspect = s_->params.cropAspect;
        applyParams(p, true, QStringLiteral("ペースト"), true);
    });
    add(edit, QStringLiteral("自動調整"), QKeySequence("Ctrl+U"), [this] { autoAdjust(100, false); });
    add(edit, QStringLiteral("すべての調整をリセット"), QKeySequence("Ctrl+Shift+R"), [this] {
        if (s_) applyParams(s_->isRaw ? Params::defaults() : Params::defaultsForImage(), true, QStringLiteral("すべてリセット"), true);
    });

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
    add(view, QStringLiteral("前の写真"), QKeySequence("Left"), [this] { step(-1); });
    add(view, QStringLiteral("次の写真"), QKeySequence("Right"), [this] { step(1); });
    add(view, QStringLiteral("WB スポイト"), QKeySequence("W"), [this] { panel_->wbPickButton()->toggle(); });
    add(view, QStringLiteral("切り抜きツール"), QKeySequence("R"), [this] { panel_->cropButton()->toggle(); });
    auto* enter = new QShortcut(QKeySequence(Qt::Key_Return), this);
    connect(enter, &QShortcut::activated, this, [this] {
        if (viewer_->cropMode()) emit viewer_->cropChanged(viewer_->cropRect(), true);
    });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (!viewer_->cropMode() || !s_) return;
        panel_->cropButton()->setChecked(false);
        if (cropBackup_) applyParams(*cropBackup_, true, QStringLiteral("切り抜きを取消"), true);
    });

    QMenu* help = menuBar()->addMenu(QStringLiteral("ヘルプ"));
    add(help, QStringLiteral("操作一覧"), QKeySequence("F1"), [this] {
        QMessageBox::information(this, QStringLiteral("操作一覧"), QStringLiteral(
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
    folder_ = folder;
    QSettings().setValue("library/folder", folder);
    QDir dir(folder);
    const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    QSignalBlocker b(film_);
    film_->clear();
    QListWidgetItem* sel = nullptr;
    for (const QFileInfo& fi : files) {
        if (!isSupported(fi.filePath())) continue;
        auto* it = new QListWidgetItem(fi.fileName());
        it->setData(Qt::UserRole, fi.absoluteFilePath());
        it->setSizeHint(QSize(130, 120));
        film_->addItem(it);
        if (fi.absoluteFilePath() == QFileInfo(select).absoluteFilePath()) sel = it;
    }
    setWindowTitle(QStringLiteral("RawDeveloper — ") + QDir::toNativeSeparators(folder));
    statusBar()->showMessage(QStringLiteral("%1 枚").arg(film_->count()), 3000);
    loadThumbs();
    b.unblock();
    if (!sel && film_->count()) sel = film_->item(0);
    if (sel) film_->setCurrentItem(sel);
}

void MainWindow::loadThumbs() {
    const int token = ++loadToken_;
    QStringList paths;
    for (int i = 0; i < film_->count(); ++i) paths << film_->item(i)->data(Qt::UserRole).toString();
    QThreadPool::globalInstance()->start([this, paths, token] {
        for (int i = 0; i < paths.size(); ++i) {
            if (token != loadToken_) return;
            QImage img;
            const QString& p = paths[i];
            if (isRawPath(p)) {
                int flip = 0;
                const auto jpg = extractThumbJpeg(readAll(p), flip);
                if (!jpg.empty()) img.loadFromData(jpg.data(), int(jpg.size()));
                if (!img.isNull()) {
                    img = img.scaled(240, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    QTransform t;
                    if (flip == 3) t.rotate(180);
                    else if (flip == 5) t.rotate(-90);
                    else if (flip == 6) t.rotate(90);
                    img = img.transformed(t);
                }
            } else {
                QImageReader r(p);
                r.setAutoTransform(true);
                const QSize sz = r.size();
                if (sz.isValid()) r.setScaledSize(sz.scaled(240, 180, Qt::KeepAspectRatio));
                img = r.read();
            }
            if (img.isNull()) continue;
            QMetaObject::invokeMethod(this, [this, i, img, token] {
                if (token == loadToken_ && i < film_->count()) film_->item(i)->setIcon(QIcon(QPixmap::fromImage(img)));
            });
        }
    });
}

void MainWindow::step(int d) {
    const int r = film_->currentRow() + d;
    if (r >= 0 && r < film_->count()) film_->setCurrentRow(r);
}

void MainWindow::openFile(const QString& path) {
    if (s_ && s_->path == path) return;
    flushSave();
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
    else
        s_->params = s_->isRaw ? Params::defaults() : Params::defaultsForImage();
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
        if (!dragging_ && r.version == version_ && !viewer_->cropMode()) {
            if (auto* it = film_->currentItem())
                it->setIcon(QIcon(QPixmap::fromImage(r.after.scaled(240, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
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
    ++version_;
    if (final) {
        dragging_ = false;
        scopes_->endGhost();
        pushHistory(desc);
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
    QJsonObject o = s_->sidecar;
    o["version"] = 1;
    o["params"] = s_->params.toJson();
    const QString sp = sidecarPath(s_->path);
    QFile f(sp + ".tmp");
    if (!f.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage(QStringLiteral("保存できませんでした：") + sp, 8000);
        return;
    }
    f.write(QJsonDocument(o).toJson());
    f.close();
    QFile::remove(sp);
    QFile::rename(sp + ".tmp", sp);
    s_->sidecar = o;
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
    const Params p = autoParams(*s_->preview, s_->src->asShotGains, strength, s_->params, wbOnly);
    applyParams(p, true, wbOnly ? QStringLiteral("WB：自動") : QStringLiteral("自動調整（%1%）").arg(strength), true);
    if (!wbOnly) statusBar()->showMessage(QStringLiteral("自動調整しました。各スライダーで調整を続けられます"), 4000);
}

void MainWindow::wbPick(int x, int y) {
    if (!s_ || s_->last.isNull()) return;
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
    if (on) {
        cropBackup_ = s_->params;
        viewer_->setCropMode(true, CropRect{s_->params.cropX, s_->params.cropY, s_->params.cropW, s_->params.cropH});
        emit panel_->aspectChanged();
        viewer_->setFocus();
    } else {
        viewer_->setCropMode(false);
        cropBackup_.reset();
    }
    ++version_;
    requestRender();
}

void MainWindow::exportJpeg() {
    if (!s_ || !s_->src) return;
    flushSave();
    const QString def = QFileInfo(s_->path).absolutePath() + "/" + QFileInfo(s_->path).completeBaseName() + ".jpg";
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("JPEG で書き出し"), def, "JPEG (*.jpg)");
    if (out.isEmpty()) return;
    statusBar()->showMessage(QStringLiteral("書き出し中…"));
    const Params p = s_->params;
    const QString path = s_->path;
    const auto asShot = s_->src->asShotGains;
    auto full = s_->full;
    auto lens = s_->lens;
    QtConcurrent::run([=] {
        auto f = full ? full : loadAny(path, false);
        if (!f->error.empty()) return QString::fromStdString(f->error);
        DevelopOptions o;
        o.lens = lens.get();
        const QImage img = toQImage(develop(f->linear, p, asShot, o));
        QImageWriter w(out, "jpg");
        w.setQuality(92);
        return w.write(img) ? QString() : w.errorString();
    }).then(this, [this, out](const QString& err) {
        if (err.isEmpty())
            statusBar()->showMessage(QStringLiteral("書き出しました：") + QDir::toNativeSeparators(out), 6000);
        else
            QMessageBox::warning(this, QStringLiteral("書き出し"), QStringLiteral("失敗しました：") + err);
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
