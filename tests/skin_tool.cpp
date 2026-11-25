// rd_skin out_dir raw...：自動調整 → 肌色「普通」の前後と、肌の範囲を並べる
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <chrono>
#include <cstdio>

#include "core/CameraRef.h"
#include "engine/Auto.h"
#include "engine/Pipeline.h"
#include "engine/RawLoader.h"
#include "engine/Skin.h"

static QImage toQ(const rd::ImageF& g) {
    auto b = rd::toRgb8(g);
    QImage im(g.width, g.height, QImage::Format_RGB888);
    for (int y = 0; y < g.height; ++y) memcpy(im.scanLine(y), &b[size_t(y) * g.width * 3], g.width * 3);
    return im;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QDir().mkpath(argv[1]);
    for (int i = 2; i < argc; ++i) {
        QFile f(argv[i]);
        f.open(QIODevice::ReadOnly);
        const QByteArray b = f.readAll();
        std::vector<unsigned char> bytes(b.begin(), b.end());
        auto raw = rd::loadRaw(bytes, true);
        const rd::ImageF prev = rd::downscale(raw.linear, 1000);
        const rd::ImageF ref = rd::cameraReference(bytes, raw.meta.fullWidth, raw.meta.fullHeight);
        rd::Params a = rd::autoParams(prev, raw.asShotGains, 100, rd::Params::defaults(), false, &raw.meta, nullptr, &ref);
        rd::DevelopOptions o;
        o.scale = float(prev.width) / raw.linear.width;
        const rd::ImageF g0 = rd::develop(prev, a, raw.asShotGains, o);
        auto t0 = std::chrono::steady_clock::now(); const rd::SkinStats st = rd::analyzeSkin(g0); std::printf("analyze %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        rd::Params s = rd::skinAuto(a, st, 2);
        const rd::ImageF g1 = rd::develop(prev, s, raw.asShotGains, o);
        rd::Params m = s;
        m.skinMaskView = true;
        const rd::ImageF g2 = rd::develop(prev, m, raw.asShotGains, o);
        QImage sheet(g0.width * 3, g0.height + 30, QImage::Format_RGB888);
        sheet.fill(Qt::black);
        QPainter p(&sheet);
        p.drawImage(0, 30, toQ(g0));
        p.drawImage(g0.width, 30, toQ(g1));
        p.drawImage(g0.width * 2, 30, toQ(g2));
        p.setPen(QPen(Qt::green, 2));
        for (const auto& fb : rd::detectFaces(g0))
            p.drawRect(QRectF(fb.x * g0.width, 30 + fb.y * g0.height, fb.w * g0.width, fb.h * g0.height));
        p.setPen(Qt::white);
        const QString line = QString("faces %12 | %1 skin %2% hue %3 sat %4 val %5 -> hue %6 sat %7 lum %8 uni %9/%10/%11")
                                 .arg(QFileInfo(argv[i]).fileName()).arg(st.fraction * 100, 0, 'f', 1).arg(st.hue, 0, 'f', 1)
                                 .arg(st.sat, 0, 'f', 2).arg(st.val, 0, 'f', 2).arg(s.skinHue).arg(s.skinSat).arg(s.skinLum)
                                 .arg(s.skinUniHue).arg(s.skinUniSat).arg(s.skinUniLum).arg(st.faces);
        p.drawText(6, 20, line);
        p.end();
        sheet.save(QString(argv[1]) + "/" + QFileInfo(argv[i]).completeBaseName() + ".jpg", "JPG", 88);
        std::printf("%s\n", line.toUtf8().constData());
    }
}
