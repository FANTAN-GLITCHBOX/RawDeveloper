// rd_gputest raw：同じ設定で CPU と GPU の現像結果を比べる（違いの最大・平均と速さ）
#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QGuiApplication>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "core/CameraRef.h"
#include "engine/Gpu.h"
#include "engine/Lens.h"
#include "engine/Pipeline.h"
#include "engine/Profile.h"
#include "engine/RawLoader.h"
#include "gpu/GlBackend.h"

using namespace rd;
static double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QString why;
    GlBackend* gpu = GlBackend::create(true, &why);
    if (!gpu) {
        std::printf("GPU not available: %s\n", why.toUtf8().constData());
        return 2;
    }
    std::printf("renderer: %s\n", gpu->renderer().toUtf8().constData());
    std::fflush(stdout);
    const QStringList args = QCoreApplication::arguments();  // 日本語のパスも正しく
    if (args.size() < 2) {
        std::printf("使い方: rd_gputest.exe RAWファイル [長辺]\n");
        return 2;
    }
    QFile f(args[1]);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("ファイルを開けません: %s\n", args[1].toUtf8().constData());
        return 2;
    }
    const QByteArray b = f.readAll();
    std::vector<unsigned char> bytes(b.begin(), b.end());
    auto raw = loadRaw(bytes, true);
    if (!raw.error.empty()) {
        std::printf("RAW を読めません: %s\n", raw.error.c_str());
        return 2;
    }
    const ImageF prev = downscale(raw.linear, args.size() > 2 ? args[2].toInt() : 1200);
    DevelopOptions o;
    o.scale = float(prev.width) / raw.meta.fullWidth;
    const ImageF ref = cameraReference(bytes, raw.meta.fullWidth, raw.meta.fullHeight);

    struct Case {
        const char* name;
        Params p;
    };
    std::vector<Case> cases;
    Params base = Params::defaults();
    matchCamera(raw.linear, raw.asShotGains, ref, base);
    cases.push_back({"camera profile", base});
    Params t = base;
    t.exposure = 0.7f; t.contrast = 30; t.highlights = -40; t.shadows = 35; t.saturation = 15; t.vibrance = 30;
    cases.push_back({"tone+sat+vib", t});
    Params hs = t;
    hs.hslHue[1] = 30; hs.hslSat[4] = -40; hs.hslLum[2] = 25;
    hs.lift = {0.1f, 0.3f, 0}; hs.gain = {0.05f, 0.1f, 0.0f};
    cases.push_back({"hsl+wheels", hs});
    Params sk = hs;
    sk.enabled.skin = true; sk.skinHue = -10; sk.skinSat = -15; sk.skinUniHue = 40; sk.skinUniSat = 30; sk.skinUniLum = 30; sk.skinSmooth = 50;
    sk.skinCenterHue = 18; sk.skinCenterSat = 0.35f;
    cases.push_back({"skin", sk});
    Params lc = sk;
    LocalMask a; a.type = "linear"; a.x0 = 0.5f; a.y0 = 0; a.x1 = 0.5f; a.y1 = 0.4f; a.exposure = -1; a.contrast = 30; lc.masks.push_back(a);
    LocalMask r; r.type = "radial"; r.x0 = 0.6f; r.y0 = 0.35f; r.x1 = 0.12f; r.y1 = 0.09f; r.angle = 20; r.exposure = 0.6f; r.temp = 30; r.tint = -20; lc.masks.push_back(r);
    LocalMask br; br.type = "brush"; br.saturation = -80; br.highlights = 40; br.shadows = -30;
    for (int i = 0; i < 60; ++i) br.dabs.push_back({0.2f + i * 0.005f, 0.8f, 0.03f, 0.8f, i > 50});
    lc.masks.push_back(br);
    LocalMask sm; sm.type = "skin"; sm.invert = true; sm.saturation = -30; lc.masks.push_back(sm);
    LocalMask sy; sy.type = "sky"; sy.exposure = -0.5f; lc.masks.push_back(sy);
    lc.showMaskIndex = 1;
    cases.push_back({"masks", lc});
    Params sh = lc;
    sh.sharpenAmount = 60; sh.sharpenRadius = 1.5f; sh.sharpenMasking = 30;
    cases.push_back({"sharpen", sh});
    Params ef = t;
    ef.texture = 40; ef.clarity = 35; ef.dehaze = 30; ef.sharpenAmount = 50; ef.sharpenDetail = 25;
    cases.push_back({"tex/clar/haze", ef});
    Params ef2 = ef;
    ef2.texture = -50; ef2.clarity = -40; ef2.dehaze = -40;
    cases.push_back({"negative", ef2});
    Params lm = ef;
    LocalMask bg; bg.type = "radial"; bg.x0 = 0.6f; bg.y0 = 0.35f; bg.x1 = 0.2f; bg.y1 = 0.15f; bg.invert = true; bg.sharpness = -80; lm.masks.push_back(bg);
    LocalMask fc; fc.type = "radial"; fc.x0 = 0.6f; fc.y0 = 0.35f; fc.x1 = 0.12f; fc.y1 = 0.09f; fc.sharpness = 60; fc.clarity = -30; fc.texture = -40; lm.masks.push_back(fc);
    LocalMask hz; hz.type = "linear"; hz.x0 = 0.5f; hz.y0 = 0; hz.x1 = 0.5f; hz.y1 = 0.4f; hz.dehaze = 60; lm.masks.push_back(hz);
    cases.push_back({"mask effects", lm});

    int bad = 0;
    for (auto& c : cases) {
        setGpuBackend(nullptr);
        develop(prev, c.p, raw.asShotGains, o);  // 準備（レンズ・ノイズの使い回しをそろえる）
        auto t0 = std::chrono::steady_clock::now();
        ImageF cpu;
        for (int k = 0; k < 3; ++k) cpu = develop(prev, c.p, raw.asShotGains, o);
        const double tc = ms(t0) / 3;
        setGpuBackend(gpu);
        develop(prev, c.p, raw.asShotGains, o);  // 準備
        t0 = std::chrono::steady_clock::now();
        ImageF gg;
        for (int k = 0; k < 3; ++k) gg = develop(prev, c.p, raw.asShotGains, o);
        const double tg = ms(t0) / 3;
        double mx = 0, sum = 0;
        size_t over = 0;
        for (size_t i = 0; i < cpu.data.size(); ++i) {
            const double d = std::abs(cpu.data[i] - gg.data[i]);
            mx = std::max(mx, d);
            sum += d;
            over += d > 2.0 / 255;
        }
        const double mean = sum / cpu.data.size();
        const bool ok = mean < 0.5 / 255 && double(over) / cpu.data.size() < 0.002;
        bad += !ok;
        std::printf("%-16s cpu %7.1f ms  gpu %7.1f ms  diff mean %.5f max %.4f  >2/255: %.4f%%  %s\n", c.name, tc, tg, mean, mx,
                    100.0 * over / cpu.data.size(), ok ? "OK" : "NG");
    }
    setGpuBackend(nullptr);
    delete gpu;
    return bad ? 1 : 0;
}
