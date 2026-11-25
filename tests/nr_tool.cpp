// rd_nr out.jpg raw [cx cy]：100% 切り出しで、ノイズ低減なし／ISO 初期値／強めを並べる
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <chrono>
#include <cstdio>

#include "core/CameraRef.h"
#include "engine/Auto.h"
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
    QFile f(argv[2]);
    f.open(QIODevice::ReadOnly);
    const QByteArray b = f.readAll();
    std::vector<unsigned char> bytes(b.begin(), b.end());
    auto raw = rd::loadRaw(bytes, false);
    const double fx = argc > 3 ? atof(argv[3]) : 0.5, fy = argc > 4 ? atof(argv[4]) : 0.5;
    const rd::ImageF prev = rd::downscale(raw.linear, 1600);
    const rd::ImageF ref = rd::cameraReference(bytes, raw.meta.fullWidth, raw.meta.fullHeight);
    rd::Params base = rd::Params::defaults();
    rd::Params a = rd::autoParams(prev, raw.asShotGains, 100, base, false, &raw.meta, nullptr, &ref);
    a.sharpenAmount = 40;
    const int W = raw.linear.width, H = raw.linear.height, cw = 500, ch = 500;
    const int rx = int(fx * W) - cw / 2, ry = int(fy * H) - ch / 2;
    const rd::NoiseLevel nl = rd::noiseOf(raw.linear);
    std::printf("ISO %.0f noise y %.4f c1 %.4f c2 %.4f\n", raw.meta.iso, nl.y, nl.c1, nl.c2);
    struct V { const char* name; float l, ld, lc, c, cd, cs; };
    const float iso = rd::defaultLumaNr(raw.meta.iso);
    V vs[] = {{"なし", 0, 50, 0, 0, 50, 50}, {"ISO初期値", iso, 50, 0, 25, 50, 50}, {"強め", 60, 50, 0, 50, 50, 50}, {"強め+ディテール80", 60, 80, 30, 50, 50, 50}};
    QImage sheet(cw * 4, ch + 30, QImage::Format_RGB888);
    sheet.fill(Qt::black);
    QPainter p(&sheet);
    p.setPen(Qt::white);
    int i = 0;
    for (auto& v : vs) {
        rd::Params q = a;
        q.nrLuma = v.l; q.nrLumaDetail = v.ld; q.nrLumaContrast = v.lc; q.nrColor = v.c; q.nrColorDetail = v.cd; q.nrColorSmooth = v.cs;
        q.enabled.lens = false;
        auto t0 = std::chrono::steady_clock::now();
        const rd::ImageF g = rd::developRegion(raw.linear, q, raw.asShotGains, rx, ry, cw, ch, 1.0f, nullptr);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        p.drawImage(i * cw, 30, toQ(g));
        p.drawText(i * cw + 6, 20, QString("%1  (輝度 %2)  %3ms").arg(QString::fromUtf8(v.name)).arg(v.l).arg(ms, 0, 'f', 0));
        ++i;
    }
    p.end();
    sheet.save(argv[1], "JPG", 92);
    // フル現像の時間
    auto t0 = std::chrono::steady_clock::now();
    rd::Params q = a; q.nrLuma = 30; q.nrColor = 25;
    rd::DevelopOptions o;
    rd::develop(raw.linear, q, raw.asShotGains, o);
    std::printf("full develop with NR: %.0f ms (%dx%d)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), W, H);
    t0 = std::chrono::steady_clock::now();
    rd::DevelopOptions op; op.scale = float(prev.width) / W;
    rd::develop(prev, q, raw.asShotGains, op);
    std::printf("preview develop with NR: %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
