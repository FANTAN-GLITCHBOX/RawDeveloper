#pragma once
#include <QFutureWatcher>
#include <QMainWindow>
#include <memory>

#include "engine/Develop.h"
#include "engine/RawLoader.h"

class QLabel;
class QSlider;

namespace rd {

class Viewer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    void openFile(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void render();
    void loaded();

    Viewer* viewer_ = nullptr;
    QSlider* exposure_ = nullptr;
    QLabel* exposureValue_ = nullptr;
    QLabel* info_ = nullptr;
    QFutureWatcher<LoadedRaw> watcher_;
    std::shared_ptr<LoadedRaw> raw_;
    ImageF preview_;
    QString path_;
    qint64 loadStart_ = 0;
};

}  // namespace rd
