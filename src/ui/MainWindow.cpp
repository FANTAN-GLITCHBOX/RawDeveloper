#include "ui/MainWindow.h"

#include <QDateTime>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMimeData>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

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
}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("RawDeveloper（C++ 版・開発中）"));
    resize(1400, 900);
    setAcceptDrops(true);

    viewer_ = new Viewer(this);
    setCentralWidget(viewer_);

    auto* file = menuBar()->addMenu(QStringLiteral("ファイル"));
    auto* openAct = file->addAction(QStringLiteral("開く…"));
    connect(openAct, &QAction::triggered, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("RAW を開く"), QString(),
            QStringLiteral("RAW (*.cr2 *.cr3 *.nef *.nrw *.arw *.raf *.orf *.rw2 *.pef *.dng *.srw *.3fr *.iiq);;すべて (*.*)"));
        if (!f.isEmpty()) openFile(f);
    });
    openAct->setShortcut(QKeySequence::Open);
    auto* quitAct = file->addAction(QStringLiteral("終了"));
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);
    auto* tb = addToolBar(QStringLiteral("ツール"));
    tb->addAction(openAct);

    // 調整パネル（この段階では露出のみ）
    auto* panel = new QWidget;
    auto* form = new QFormLayout(panel);
    exposure_ = new QSlider(Qt::Horizontal);
    exposure_->setRange(-500, 500);
    exposureValue_ = new QLabel("+0.00 EV");
    form->addRow(QStringLiteral("露出"), exposure_);
    form->addRow(QString(), exposureValue_);
    info_ = new QLabel;
    info_->setWordWrap(true);
    form->addRow(info_);
    auto* dock = new QDockWidget(QStringLiteral("調整"), this);
    dock->setWidget(panel);
    dock->setMinimumWidth(300);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    connect(exposure_, &QSlider::valueChanged, this, [this](int v) {
        exposureValue_->setText(QString::asprintf("%+.2f EV", v / 100.0));
        render();
    });

    connect(&watcher_, &QFutureWatcher<LoadedRaw>::finished, this, &MainWindow::loaded);
    statusBar()->showMessage(QStringLiteral("RAW ファイルを開いてください（Ctrl+O）"));
}

void MainWindow::openFile(const QString& path) {
    if (watcher_.isRunning()) return;
    path_ = path;
    viewer_->setMessage(QStringLiteral("読み込み中… ") + QFileInfo(path).fileName());
    loadStart_ = QDateTime::currentMSecsSinceEpoch();
    watcher_.setFuture(QtConcurrent::run([path] { return loadRaw(readAll(path), true); }));
}

void MainWindow::loaded() {
    raw_ = std::make_shared<LoadedRaw>(watcher_.result());
    if (!raw_->error.empty()) {
        viewer_->setMessage(QStringLiteral("読み込めませんでした：") + QString::fromStdString(raw_->error));
        return;
    }
    preview_ = downscale(raw_->linear, kPreviewEdge);
    const auto& m = raw_->meta;
    info_->setText(QStringLiteral("%1 %2\n%3\nISO %4  F%5  %6mm\n%7×%8")
                       .arg(QString::fromStdString(m.make), QString::fromStdString(m.model),
                            QString::fromStdString(m.lensModel))
                       .arg(m.iso)
                       .arg(m.aperture, 0, 'f', 1)
                       .arg(m.focal, 0, 'f', 0)
                       .arg(m.fullWidth)
                       .arg(m.fullHeight));
    statusBar()->showMessage(QStringLiteral("%1 を開きました（%2 秒）")
                                 .arg(QFileInfo(path_).fileName())
                                 .arg((QDateTime::currentMSecsSinceEpoch() - loadStart_) / 1000.0, 0, 'f', 2));
    render();
}

void MainWindow::render() {
    if (preview_.empty() || !raw_) return;
    QElapsedTimer t;
    t.start();
    Params p;
    p.wbGains = raw_->asShotGains;
    p.exposure = exposure_->value() / 100.0f;
    const auto rgb = developToRgb8(preview_, p);
    QImage img(rgb.data(), preview_.width, preview_.height, preview_.width * 3, QImage::Format_RGB888);
    viewer_->setImage(img.copy());
    setWindowTitle(QStringLiteral("RawDeveloper（C++ 版・開発中） — %1　表示 %2 ms")
                       .arg(QFileInfo(path_).fileName())
                       .arg(t.elapsed()));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const auto urls = e->mimeData()->urls();
    if (!urls.isEmpty()) openFile(urls.first().toLocalFile());
}

}  // namespace rd
