#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("RawDeveloper");
    QApplication::setApplicationName("RawDeveloper");
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
    pal.setColor(QPalette::WindowText, QColor(0xd6, 0xd6, 0xd6));
    pal.setColor(QPalette::Base, QColor(0x2e, 0x2e, 0x2e));
    pal.setColor(QPalette::Text, QColor(0xd6, 0xd6, 0xd6));
    pal.setColor(QPalette::Button, QColor(0x2e, 0x2e, 0x2e));
    pal.setColor(QPalette::ButtonText, QColor(0xd6, 0xd6, 0xd6));
    pal.setColor(QPalette::Highlight, QColor(0xd2, 0x8b, 0x2c));
    app.setPalette(pal);

    rd::MainWindow w;
    w.show();
    if (argc > 1) w.openFile(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
