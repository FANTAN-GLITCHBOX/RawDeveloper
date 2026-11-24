#pragma once
// ライブラリ：サムネイル一覧、★・ラベル・フラグ表示、絞り込み・並べ替え、フィルムストリップ
#include <QAbstractListModel>
#include <QImage>
#include <QListView>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QWidget>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QFileSystemModel;

namespace rd {

struct PhotoItem {
    QString path, name;
    int rating = 0;
    QString label;
    int flag = 0;
    bool edited = false;
    int exports = 0;
    bool isExport = false;
    bool missingLink = false;
    bool isRaw = true;
    qint64 mtime = 0;
    QImage thumb;
};

QString labelColor(const QString& label);
QString labelName(const QString& label);
extern const QStringList kLabels;  // red, yellow, green, blue, purple

// フォルダを調べて一覧を作る（別スレッドで呼ぶ）
std::vector<PhotoItem> scanFolder(const QString& folder);
bool isSupportedPhoto(const QString& path);
bool isRawPhoto(const QString& path);
QString thumbCachePath(const QString& key, qint64 stamp);
QString editedThumbPath(const QString& path);  // 編集後のサムネイル（.rdev の更新日時で区別）

class PhotoModel : public QAbstractListModel {
    Q_OBJECT
public:
    static constexpr int ItemRole = Qt::UserRole + 1;
    explicit PhotoModel(QObject* parent = nullptr);
    void setItems(std::vector<PhotoItem> items);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    PhotoItem* item(const QString& path);
    PhotoItem* itemAt(int row) { return row >= 0 && row < int(items_.size()) ? &items_[row] : nullptr; }
    void refresh(const QString& path);
    void setThumb(const QString& path, const QImage& img);
    const std::vector<PhotoItem>& items() const { return items_; }

private:
    void requestThumb(int row) const;
    std::vector<PhotoItem> items_;
    QHash<QString, int> index_;
    mutable QSet<QString> pending_;
    int token_ = 0;
};

class FilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    int minRating = 0;
    QString ratingOp = ">=";       // >= | == | <=
    QSet<QString> labels;          // 空なら全部（"none" = ラベルなし）
    QString flag = "all";          // all | pick | reject | unflagged | notreject
    QString kind = "all";          // all | raw | image | edited | exported
    QString text;
    QString sortKey = "name";      // name | date | rating | label
    void refilter();

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    bool lessThan(const QModelIndex& a, const QModelIndex& b) const override;
};

class ThumbDelegate : public QStyledItemDelegate {
public:
    ThumbDelegate(int size, bool compact, QObject* parent = nullptr) : QStyledItemDelegate(parent), size(size), compact(compact) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    int size;
    bool compact;
};

class ThumbView : public QListView {
    Q_OBJECT
public:
    explicit ThumbView(bool compact, QWidget* parent = nullptr);
    ThumbDelegate* delegate() const { return delegate_; }

signals:
    void middleClicked(const QString& path);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    ThumbDelegate* delegate_;
    bool compact_;
};

class FilterBar : public QWidget {
    Q_OBJECT
public:
    explicit FilterBar(FilterProxy* proxy, QWidget* parent = nullptr);
    void updateCount();

signals:
    void changed();

private:
    void apply();
    FilterProxy* proxy_;
    QComboBox *stars_, *op_, *flag_, *kind_, *sort_;
    QLineEdit* search_;
    QLabel* count_;
    QMap<QString, QToolButton*> labelBtns_;
};

class FolderTree : public QTreeView {
    Q_OBJECT
public:
    explicit FolderTree(QWidget* parent = nullptr);
    void reveal(const QString& path);

signals:
    void folderChosen(const QString& path);

private:
    QFileSystemModel* fs_;
};

}  // namespace rd
