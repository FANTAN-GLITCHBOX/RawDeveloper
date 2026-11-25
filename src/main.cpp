#include <QApplication>
#include <QFileInfo>
#include <QSettings>
#include <memory>

#include "engine/Gpu.h"
#include "gpu/GlBackend.h"
#include <QTimer>

#include "engine/Lens.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("RawDeveloper");
    QApplication::setApplicationName("RawDeveloper");
    rd::theme::apply(app);
    rd::LensDb::setDataDirs({(QCoreApplication::applicationDirPath() + "/lensfun").toStdString()});
    // GPU（OpenGL 4.3）。使えなければ CPU のまま
    std::unique_ptr<rd::GlBackend> gpu;
    {
        const QByteArray env = qgetenv("RD_GPU");
        QString why;
        if (env != "0" && QSettings().value("develop/gpu", true).toBool()) {
            gpu.reset(rd::GlBackend::create(env == "soft", &why));
            if (gpu) rd::setGpuBackend(gpu.get());
        } else {
            why = QStringLiteral("環境設定でオフ");
        }
        rd::MainWindow::setGpuStatus(gpu ? gpu->renderer() : QString(), why);
    }
    rd::MainWindow w;
    w.show();
    const QString arg = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!arg.isEmpty() && QFileInfo(arg).isDir())
        w.openFolder(arg);
    else if (!arg.isEmpty())
        w.openFolder(QFileInfo(arg).absolutePath(), arg);
    else if (const QString last = QSettings().value("library/folder").toString(); !last.isEmpty() && QFileInfo(last).isDir())
        w.openFolder(last);
    if (const QByteArray t = qgetenv("RD_TEST_SCRIPT"); !t.isEmpty()) w.runScriptedTest(QString::fromLocal8Bit(t));
    // 自動テスト用：RD_SCREENSHOT=保存先.png  RD_SCREENSHOT_DELAY=ミリ秒
    if (const QByteArray shot = qgetenv("RD_SCREENSHOT"); !shot.isEmpty()) {
        const int delay = qEnvironmentVariableIntValue("RD_SCREENSHOT_DELAY");
        QTimer::singleShot(delay > 0 ? delay : 5000, &w, [&w, shot] {
            w.grab().save(QString::fromLocal8Bit(shot));
            QApplication::quit();
        });
    }
    const int rc = app.exec();
    rd::setGpuBackend(nullptr);
    return rc;
}
