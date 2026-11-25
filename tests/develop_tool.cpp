// rd_develop in.f32 params.json out.f32 [asShotR asShotG asShotB] [--auto out.json] [--meta iso shutter aperture bias] [--apply]
// .f32 = int32 幅, int32 高さ, float RGB...
#include <QFile>
#include <QJsonDocument>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "engine/Auto.h"
#include "engine/Pipeline.h"

static bool readF32(const char* path, rd::ImageF& img) {
    std::ifstream f(path, std::ios::binary);
    int32_t wh[2];
    if (!f.read(reinterpret_cast<char*>(wh), 8)) return false;
    img.resize(wh[0], wh[1]);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(img.data.data()), img.data.size() * 4));
}

static void writeF32(const char* path, const rd::ImageF& img) {
    std::ofstream f(path, std::ios::binary);
    int32_t wh[2] = {img.width, img.height};
    f.write(reinterpret_cast<const char*>(wh), 8);
    f.write(reinterpret_cast<const char*>(img.data.data()), img.data.size() * 4);
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    rd::ImageF lin;
    if (!readF32(argv[1], lin)) return 1;
    QFile pf(argv[2]);
    pf.open(QIODevice::ReadOnly);
    rd::Params p = rd::Params::fromJson(QJsonDocument::fromJson(pf.readAll()).object());
    std::array<float, 3> asShot{1, 1, 1};
    if (argc >= 7) asShot = {std::strtof(argv[4], nullptr), std::strtof(argv[5], nullptr), std::strtof(argv[6], nullptr)};
    rd::Metadata meta;
    bool haveMeta = false, apply = false;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--apply") == 0) apply = true;
        if (std::strcmp(argv[i], "--meta") == 0 && i + 4 < argc) {
            meta.iso = std::strtof(argv[i + 1], nullptr);
            meta.shutter = std::strtof(argv[i + 2], nullptr);
            meta.aperture = std::strtof(argv[i + 3], nullptr);
            meta.exposureBias = std::strtof(argv[i + 4], nullptr);
            meta.hasBias = true;
            haveMeta = true;
        }
    }
    for (int i = 4; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--auto") == 0) {
            rd::AutoInfo info;
            rd::Params a = rd::autoParams(lin, asShot, 100, p, false, haveMeta ? &meta : nullptr, &info);
            std::printf("scene: %s\n", info.scene.c_str());
            if (apply) p = a;
            QFile af(argv[i + 1]);
            af.open(QIODevice::WriteOnly);
            af.write(QJsonDocument(a.toJson()).toJson());
        }
    rd::LensInfo li;
    bool haveLens = false;
    for (int i = 4; i + 4 < argc; ++i)
        if (std::strcmp(argv[i], "--lens") == 0) {
            rd::LensDb::setDataDirs({LENS_DIR});
            li.autoKey = argv[i + 1];
            li.crop = std::strtof(argv[i + 2], nullptr);
            li.focal = std::strtof(argv[i + 3], nullptr);
            li.aperture = std::strtof(argv[i + 4], nullptr);
            haveLens = true;
        }
    rd::DevelopOptions o;
    o.scale = 0.5f;
    if (haveLens) o.lens = &li;
    writeF32(argv[3], rd::develop(lin, p, asShot, o));
    return 0;
}
