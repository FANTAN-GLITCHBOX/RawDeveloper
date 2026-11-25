#pragma once
#include <QFutureWatcher>
#include <QImage>
#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

#include "core/Catalog.h"

#include "engine/Lens.h"
#include "engine/Params.h"
#include "engine/RawLoader.h"
#include "ui/Batch.h"
#include "ui/Library.h"
#include "ui/Viewer.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QToolButton;
class QActionGroup;
class QStackedWidget;
class QDockWidget;
class QAction;
class QPushButton;

namespace rd {

class Viewer;
class DevelopPanel;
class ScopesPanel;
class ScopeView;

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
    std::shared_ptr<LensInfo> lens;      // EXIF から判別したレンズ
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
    QImage before;          // 補正前の同じ範囲（比較表示のときだけ）
    QString beforeKey;
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
    void nudgeAngle(double delta);
    void commitAngle();
    Params fitCropToAngle(Params p);
    void step(int d);
    // ライブラリ
    void setMode(bool develop);
    void folderScanned();
    QString currentPath() const;
    QStringList selectedPaths() const;
    QStringList filteredPaths() const;
    QStringList ratingTargets() const;
    void showLibInfo(const QString& path);
    void rate(const std::function<void(PhotoMark&)>& fn, const QString& desc);
    void saveMark(const QString& path, const PhotoMark& m, const PhotoMark& before);
    PhotoMark markOf(const QString& path);
    void copyFromOther(const QString& path);
    void trashRejected();
    void showLinks();
    void toggleFullscreen();
    // 書き出し
    void exportDialog();
    void copySettings(bool choose);
    void pasteSettings();
    void pastePrevious();
    void batchAuto();
    void runBatch(std::vector<BatchJob> jobs, const QString& title, bool silent);
    void undoBatch();
    Params jobResult(const BatchJob& j, const Params& cur, const LoadedRaw& src) const;
    void storeEditedThumb();
    void setCompareTarget(std::optional<Params> p, const QString& label);
    Params makeBeforeParams(const Params& p) const;

    Viewer* viewer_ = nullptr;
    DevelopPanel* panel_ = nullptr;
    ScopesPanel* scopes_ = nullptr;
    ThumbView* film_ = nullptr;
    ThumbView* grid_ = nullptr;
    PhotoModel* model_ = nullptr;
    FilterProxy* proxy_ = nullptr;
    FilterBar* filterBar_ = nullptr;
    FolderTree* tree_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* libInfo_ = nullptr;
    ScopeView* libHist_ = nullptr;
    QList<QDockWidget*> docks_;
    QAction* modeLib_ = nullptr;
    QAction* modeDev_ = nullptr;
    QWidget* toolbarWidget_ = nullptr;
    QFutureWatcher<std::vector<PhotoItem>> scanWatcher_;
    QString pendingSelect_;
    bool developMode_ = false;
    QImage pendingThumb_;
    QString pendingThumbPath_;
    bool exporting_ = false;
    QListWidget* historyList_ = nullptr;
    QLabel* metaLabel_ = nullptr;
    QToolButton* compareBtn_ = nullptr;

    std::unique_ptr<Session> s_;
    struct LoadResult {
        std::shared_ptr<LoadedRaw> src;
        std::shared_ptr<LensInfo> lens;
    };
    QFutureWatcher<LoadResult> loadWatcher_;
    QFutureWatcher<RenderResult> renderWatcher_;
    QFutureWatcher<RenderResult> hiresWatcher_;
    QFutureWatcher<RoiResult> roiWatcher_;
    QFutureWatcher<std::shared_ptr<LoadedRaw>> fullWatcher_;
    QString fullLoadingPath_;
    bool roiPending_ = false;
    QString roiBeforeKey_;      // 表示中の補正前（部分高精細）の条件
    QTimer viewTimer_;
    bool renderPending_ = false;
    bool dragging_ = false;
    quint64 version_ = 0;
    quint64 hiresVersion_ = 0;
    QString beforeKey_;
    std::optional<Params> compareTarget_;
    bool beforeCrop_ = true;   // 補正前にも切り抜き・角度を当てる
    QTimer saveTimer_;
    std::optional<CropRect> angleBase_;  // 角度を変え始めたときの枠（角度を戻すと枠も元に戻せるように）
    QTimer angleTimer_;   // キーで角度を変えたあと、離して 0.5 秒で履歴に 1 件だけ記録
    QString folder_;
    // 調整のコピー・貼り付け
    std::optional<Params> clipboard_;
    QStringList clipGroups_;
    QString clipSrc_;
    double clipLum_ = -1;  // コピー元の明るさ（露出をそろえるとき）
    bool clipMatch_ = false;
    QString prevPath_;     // ひとつ前に開いていた写真
    QAction* syncAct_ = nullptr;
    std::vector<BatchJob> batchQueue_;
    bool batchRunning_ = false;
    std::vector<std::pair<QString, std::optional<QByteArray>>> undoBackup_;
    QPushButton* undoBtn_ = nullptr;
    QTimer undoTimer_;
    std::optional<Params> cropBackup_;
};

}  // namespace rd
