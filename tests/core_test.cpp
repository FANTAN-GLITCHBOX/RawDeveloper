// 評価・書き出しの確認：書き出し → 読み戻し → 書き換え → 読み戻し
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <cstdio>

#include "core/Catalog.h"
#include "core/Export.h"
#include "core/Xmp.h"
#include "engine/RawLoader.h"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath() + "/rdcore";
    QDir().mkpath(dir);
    rd::ImageF g;
    g.resize(320, 200);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x) {
            float* p = g.px(x, y);
            p[0] = x / 320.0f;
            p[1] = y / 200.0f;
            p[2] = 0.5f;
        }
    rd::Metadata meta;
    meta.make = "Sony";
    meta.model = "ILCE-7M3";
    int fails = 0;
    for (const char* fmt : {"jpeg", "png", "tiff"}) {
        rd::ExportOptions o;
        o.folder = dir;
        o.fmt = fmt;
        o.overwrite = "overwrite";
        o.longEdge = 160;
        const QString path = rd::exportPath(dir + "/src.dng", o, 1);
        const QString err = rd::writeExport(g, path, o, 4, "green", &meta);
        auto r1 = rd::xmp::readFileRating(path);
        QString werr;
        const QString where = rd::xmp::writeFileRating(path, 2, "red", &werr);
        auto r2 = rd::xmp::readFileRating(path);
        const QImage img(path);
        const bool ok = err.isEmpty() && r1.rating == 4 && r1.label == "green" && r2.rating == 2 && r2.label == "red";
        std::printf("%-5s write:%s read:%d/%s rewrite:%s read:%d/%s image:%dx%d %s\n", fmt, err.isEmpty() ? "ok" : qPrintable(err),
                    r1.rating.value_or(-9), qPrintable(r1.label), qPrintable(where), r2.rating.value_or(-9),
                    qPrintable(r2.label), img.width(), img.height(), ok ? "OK" : "NG");
        if (!ok) ++fails;
    }
    // カタログ
    auto& c = rd::Catalog::get();
    c.setMark(dir + "/a.dng", {3, "blue", 1, true});
    c.addLink(dir + "/a.dng", dir + "/a.jpg");
    auto m = c.mark(dir + "/a.dng");
    const bool cok = m && m->rating == 3 && m->label == "blue" && m->flag == 1 && c.exportsOf(dir + "/a.dng").size() == 1 &&
                     c.sourceOf(dir + "/a.jpg") == dir + "/a.dng";
    std::printf("catalog %s\n", cok ? "OK" : "NG");
    if (!cok) ++fails;
    // .rdev
    rd::writeSidecarMark(dir + "/b.dng", {5, "purple", -1, false});
    auto sm = rd::readSidecarMark(dir + "/b.dng");
    const bool sok = sm.exists && sm.mark.rating == 5 && sm.mark.label == "purple" && sm.mark.flag == -1;
    std::printf("sidecar %s\n", sok ? "OK" : "NG");
    if (!sok) ++fails;
    return fails;
}
