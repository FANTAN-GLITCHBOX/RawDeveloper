// rd_auto out_dir raw... ：撮影時の設定のままの現像・自動調整・カメラの JPEG（埋め込み）を並べた画像を書き出す
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QPainter>
#include <cstdio>
#include <cstdlib>

#include "core/CameraRef.h"
#include "engine/Auto.h"
#include "engine/Profile.h"
#include "engine/Pipeline.h"
#include "engine/RawLoader.h"

static QImage toQ(const rd::ImageF& g) {
    auto b = rd::toRgb8(g);
    QImage im(g.width, g.height, QImage::Format_RGB888);
    for (int y = 0; y < g.height; ++y) memcpy(im.scanLine(y), &b[size_t(y) * g.width * 3], g.width * 3);
    return im;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QString out = argv[1];
    QDir().mkpath(out);
    for (int i = 2; i < argc; ++i) {
        QFile f(argv[i]);
        f.open(QIODevice::ReadOnly);
        const QByteArray b = f.readAll();
        std::vector<unsigned char> bytes(b.begin(), b.end());
        auto raw = rd::loadRaw(bytes, true);
        if (!raw.error.empty()) {
            std::printf("%s: %s\n", argv[i], raw.error.c_str());
            continue;
        }
        const rd::ImageF prev = rd::downscale(raw.linear, 900);
        rd::Params base = rd::Params::defaults();
        const rd::ImageF ref0 = rd::cameraReference(bytes, raw.meta.fullWidth, raw.meta.fullHeight);
        if (!std::getenv("NO_PROFILE")) rd::matchCamera(raw.linear, raw.asShotGains, ref0, base);
        rd::AutoInfo info;
        const rd::ImageF ref = rd::cameraReference(bytes, raw.meta.fullWidth, raw.meta.fullHeight);
        const bool noCam = std::getenv("NO_CAMERA") != nullptr;
        const rd::Params a = rd::autoParams(prev, raw.asShotGains, 100, base, false, &raw.meta, &info, noCam ? nullptr : &ref);
        rd::DevelopOptions o;
        o.scale = float(prev.width) / raw.linear.width;
        QImage i0 = toQ(rd::develop(prev, base, raw.asShotGains, o));
        QImage i1 = toQ(rd::develop(prev, a, raw.asShotGains, o));
        int flip = 0;
        auto th = rd::extractThumbJpeg(bytes, flip);
        QImage i2 = QImage::fromData(th.data(), int(th.size()));
        if (flip == 3) i2 = i2.transformed(QTransform().rotate(180));
        if (flip == 6) i2 = i2.transformed(QTransform().rotate(90));
        if (flip == 5) i2 = i2.transformed(QTransform().rotate(-90));
        i2 = i2.isNull() ? QImage(i0.size(), QImage::Format_RGB888) : i2.scaled(i0.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QImage sheet(i0.width() * 3, i0.height() + 40, QImage::Format_RGB888);
        sheet.fill(Qt::black);
        QPainter p(&sheet);
        p.drawImage(0, 40, i0);
        p.drawImage(i0.width(), 40, i1);
        p.drawImage(i0.width() * 2, 40, i2);
        p.setPen(Qt::white);
        const auto& m = raw.meta;
        const QString name = QFileInfo(argv[i]).fileName();
        const QString line = QString("%1  ISO%2 1/%3 F%4 bias%5 | scene:%6 | ev %7 c %8 h %9 s %10 w %11 b %12 vib %13 | %14K %15")
                                 .arg(name).arg(m.iso).arg(qRound(1 / std::max(m.shutter, 1e-4f))).arg(m.aperture)
                                 .arg(m.hasBias ? QString::number(m.exposureBias) : "?").arg(QString::fromStdString(info.scene))
                                 .arg(a.exposure).arg(a.contrast).arg(a.highlights).arg(a.shadows).arg(a.whites).arg(a.blacks)
                                 .arg(a.vibrance).arg(a.temp).arg(a.tint) + QString(" | cam ev %1").arg(base.camExposure);
        p.drawText(6, 26, line);
        p.end();
        sheet.save(out + "/" + QFileInfo(argv[i]).completeBaseName() + ".jpg", "JPG", 85);
        std::printf("%s\n", line.toUtf8().constData());
        std::fflush(stdout);
    }
    return 0;
}
