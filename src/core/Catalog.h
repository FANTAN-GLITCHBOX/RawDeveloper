#pragma once
// カタログ（SQLite）。Python 版と同じファイル・同じ表なので、どちらの版からも評価と紐付けが見える。
//  photos：path, rating, label, flag, edited
//  links ：src（元の写真）→ dst（書き出したファイル）
// サイドカー（.rdev）が正、カタログは絞り込み・紐付け用。
#include <QMutex>
#include <QString>
#include <QStringList>
#include <optional>

namespace rd {

struct PhotoMark {
    int rating = 0;
    QString label;   // red / yellow / green / blue / purple / 空
    int flag = 0;    // 1 採用, -1 除外, 0 なし
    bool edited = false;
};

class Catalog {
public:
    static Catalog& get();
    std::optional<PhotoMark> mark(const QString& path);
    void setMark(const QString& path, const PhotoMark& m);
    void addLink(const QString& src, const QString& dst);
    QStringList exportsOf(const QString& src);
    QString sourceOf(const QString& dst);
    void relink(const QString& oldDst, const QString& newDst);
    void removeLink(const QString& src, const QString& dst);

private:
    Catalog();
    QString conn_;
    QMutex mu_;
};

// .rdev の★・ラベル・フラグ（RAW は常にここ。JPEG 等は編集したときだけ .rdev を作る）
struct SidecarMark {
    bool exists = false;
    PhotoMark mark;
};
SidecarMark readSidecarMark(const QString& path);
bool writeSidecarMark(const QString& path, const PhotoMark& m);  // 既存の params 等は残す

}  // namespace rd
