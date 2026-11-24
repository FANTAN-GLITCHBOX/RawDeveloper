// rd_develop in.f32 params.json out.f32 [asShotR asShotG asShotB] [--auto out.json]
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
    for (int i = 4; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--auto") == 0) {
            rd::Params a = rd::autoParams(lin, asShot, 100, p);
            QFile af(argv[i + 1]);
            af.open(QIODevice::WriteOnly);
            af.write(QJsonDocument(a.toJson()).toJson());
        }
    rd::DevelopOptions o;
    o.scale = 0.5f;
    writeF32(argv[3], rd::develop(lin, p, asShot, o));
    return 0;
}
