#include "core/Catalog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include "engine/Paths.h"

namespace rd {

namespace {
// QSqlDatabase はスレッドごとに接続を分ける
QSqlDatabase db(const QString& base) {
    const QString name = base + QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name)) return QSqlDatabase::database(name);
    QSqlDatabase d = QSqlDatabase::addDatabase("QSQLITE", name);
    d.setDatabaseName(appDataDir() + "/catalog.db");
    d.open();
    QSqlQuery q(d);
    q.exec("PRAGMA busy_timeout=3000");
    q.exec("CREATE TABLE IF NOT EXISTS photos(path TEXT PRIMARY KEY, rating INTEGER DEFAULT 0, label TEXT, "
           "flag INTEGER DEFAULT 0, edited INTEGER DEFAULT 0, mtime REAL, added REAL)");
    q.exec("CREATE TABLE IF NOT EXISTS links(src TEXT, dst TEXT, created REAL, PRIMARY KEY(src, dst))");
    q.exec("CREATE INDEX IF NOT EXISTS links_dst ON links(dst)");
    return d;
}
double now() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

// Python 版はパスを OS の区切り文字で保存しているので合わせる
QString key(const QString& p) { return QDir::toNativeSeparators(p); }
QString fromKey(const QString& p) { return QDir::fromNativeSeparators(p); }
}  // namespace

Catalog& Catalog::get() {
    static Catalog c;
    return c;
}

Catalog::Catalog() : conn_("rdcatalog") {}

std::optional<PhotoMark> Catalog::mark(const QString& path) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("SELECT rating, label, flag, edited FROM photos WHERE path=?");
    q.addBindValue(key(path));
    if (!q.exec() || !q.next()) return std::nullopt;
    PhotoMark m;
    m.rating = q.value(0).toInt();
    m.label = q.value(1).toString();
    m.flag = q.value(2).toInt();
    m.edited = q.value(3).toBool();
    return m;
}

void Catalog::setMark(const QString& path, const PhotoMark& m) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("INSERT INTO photos(path,rating,label,flag,edited,mtime,added) VALUES(?,?,?,?,?,?,?) "
              "ON CONFLICT(path) DO UPDATE SET rating=excluded.rating,label=excluded.label,"
              "flag=excluded.flag,edited=excluded.edited,mtime=excluded.mtime");
    q.addBindValue(key(path));
    q.addBindValue(m.rating);
    q.addBindValue(m.label.isEmpty() ? QVariant() : QVariant(m.label));
    q.addBindValue(m.flag);
    q.addBindValue(int(m.edited));
    q.addBindValue(now());
    q.addBindValue(now());
    q.exec();
}

void Catalog::addLink(const QString& src, const QString& dst) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("INSERT OR REPLACE INTO links(src,dst,created) VALUES(?,?,?)");
    q.addBindValue(key(src));
    q.addBindValue(key(dst));
    q.addBindValue(now());
    q.exec();
}

QStringList Catalog::exportsOf(const QString& src) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("SELECT dst FROM links WHERE src=? ORDER BY created");
    q.addBindValue(key(src));
    QStringList out;
    if (q.exec())
        while (q.next()) out << fromKey(q.value(0).toString());
    return out;
}

QString Catalog::sourceOf(const QString& dst) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("SELECT src FROM links WHERE dst=?");
    q.addBindValue(key(dst));
    if (q.exec() && q.next()) return fromKey(q.value(0).toString());
    return {};
}

void Catalog::relink(const QString& oldDst, const QString& newDst) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("UPDATE links SET dst=? WHERE dst=?");
    q.addBindValue(key(newDst));
    q.addBindValue(key(oldDst));
    q.exec();
}

void Catalog::removeLink(const QString& src, const QString& dst) {
    QMutexLocker l(&mu_);
    QSqlQuery q(db(conn_));
    q.prepare("DELETE FROM links WHERE src=? AND dst=?");
    q.addBindValue(key(src));
    q.addBindValue(key(dst));
    q.exec();
}

// ---------------------------------------------------------------- .rdev

SidecarMark readSidecarMark(const QString& path) {
    SidecarMark s;
    QFile f(path + ".rdev");
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    s.exists = true;
    s.mark.rating = o.value("rating").toInt(0);
    s.mark.label = o.value("label").toString();
    const QString fl = o.value("flag").toString();
    s.mark.flag = fl == "pick" ? 1 : fl == "reject" ? -1 : 0;
    s.mark.edited = o.contains("params");
    return s;
}

bool writeSidecarMark(const QString& path, const PhotoMark& m) {
    const QString sp = path + ".rdev";
    QJsonObject o;
    QFile f(sp);
    if (f.open(QIODevice::ReadOnly)) {
        o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    o["version"] = 1;
    o["rating"] = m.rating;
    o["label"] = m.label.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m.label);
    o["flag"] = m.flag == 1 ? QJsonValue("pick") : m.flag == -1 ? QJsonValue("reject") : QJsonValue(QJsonValue::Null);
    QSaveFile out(sp);
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(QJsonDocument(o).toJson());
    return out.commit();
}

}  // namespace rd
