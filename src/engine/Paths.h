#pragma once
// 保存場所（Python 版と同じ場所を使い、カタログ・プリセット・レンズを共有する）
//  設定・カタログ：%APPDATA%\RawDeveloper
//  キャッシュ（サムネイル）：%LOCALAPPDATA%\RawDeveloper
#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace rd {

inline QString appDataDir() {
    QString base = qEnvironmentVariable("APPDATA");
    QString d = base.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                               : base + "/RawDeveloper";
    QDir().mkpath(d);
    return d;
}

inline QString cacheDir() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    QString d = base.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                               : base + "/RawDeveloper";
    QDir().mkpath(d);
    return d;
}

}  // namespace rd
