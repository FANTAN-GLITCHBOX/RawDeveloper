#include <QApplication>
#include <QFileInfo>
#include <QSettings>
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
    return app.exec();
}
