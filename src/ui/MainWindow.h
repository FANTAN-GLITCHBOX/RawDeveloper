#pragma once
#include <QFutureWatcher>
#include <QImage>
#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>
#include <memory>
#include <optional>

#include "engine/Params.h"
#include "engine/RawLoader.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QToolButton;
class QActionGroup;

namespace rd {

class Viewer;
class DevelopPanel;
class ScopesPanel;

// 開いている 1 枚の写真
struct Session {
    QString path;
    bool isRaw = true;
    std::shared_ptr<LoadedRaw> src;      // 縮小読み込み
    std::shared_ptr<const ImageF> preview;  // プレビュー用（長辺 1600px）
    float scale = 1;                     // preview / 元画像
    std::shared_ptr<LoadedRaw> full;     // フル解像度（ズーム・書き出し時）
    Params params;
    std::vector<std::pair<QString, Params>> history;
    int historyPos = 0;
    QJsonObject sidecar;                 // .rdev の他の項目（★・ラベル等）を保つ
    std::pair<double, double> asShotTT{5500, 0};
    QImage last;                         // 直近の表示画像
};

struct RenderResult {
    QString path;
    QImage after, before;
    QSize full;
    quint64 version = 0;
    bool hires = false;
    bool hasBefore = false;
    QString error;
    std::shared_ptr<LoadedRaw> fullData;  // 高精細表示で読み込んだフル解像度
};

struct RoiResult {
    QString path;
    QImage img;
    QRect rect;
    quint64 version = 0;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    void openFile(const QString& path);
    void openFolder(const QString& folder, const QString& select = {});
    // 自動テスト：決まった操作をしながら prefix_1.png ... を保存して終了
    void runScriptedTest(const QString& prefix);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void buildUi();
    void buildActions();
    void loaded();
    void requestRender();
    void rendered();
    void requestHires();
    void hiresDone();
    void requestDetailView();   // 拡大中：部分高精細（できなければ全体の高精細）
    void requestRoi();
    void ensureFull();
    void applyParams(const Params& p, bool final, const QString& desc, bool updatePanel = false);
    void pushHistory(const QString& desc);
    void refreshHistory();
    void jumpHistory(int i);
    void saveSidecar();
    void flushSave();
    void autoAdjust(float strength, bool wbOnly);
    void wbPick(int x, int y);
    void cropTool(bool on);
    void exportJpeg();
    void step(int d);
    void loadThumbs();
    void setCompareTarget(std::optional<Params> p, const QString& label);

    Viewer* viewer_ = nullptr;
    DevelopPanel* panel_ = nullptr;
    ScopesPanel* scopes_ = nullptr;
    QListWidget* film_ = nullptr;
    QListWidget* historyList_ = nullptr;
    QLabel* metaLabel_ = nullptr;
    QToolButton* compareBtn_ = nullptr;

    std::unique_ptr<Session> s_;
    QFutureWatcher<std::shared_ptr<LoadedRaw>> loadWatcher_;
    QFutureWatcher<RenderResult> renderWatcher_;
    QFutureWatcher<RenderResult> hiresWatcher_;
    QFutureWatcher<RoiResult> roiWatcher_;
    QFutureWatcher<std::shared_ptr<LoadedRaw>> fullWatcher_;
    QString fullLoadingPath_;
    bool roiPending_ = false;
    QTimer viewTimer_;
    bool renderPending_ = false;
    bool dragging_ = false;
    quint64 version_ = 0;
    quint64 hiresVersion_ = 0;
    QString beforeKey_;
    std::optional<Params> compareTarget_;
    QTimer saveTimer_;
    QString folder_;
    int loadToken_ = 0;
    std::optional<Params> clipboard_;
    std::optional<Params> cropBackup_;
};

}  // namespace rd
