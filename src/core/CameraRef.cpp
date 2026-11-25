#include "core/CameraRef.h"

#include <QImage>
#include <QTransform>
#include <cmath>

#include "engine/RawLoader.h"

namespace rd {

ImageF cameraReference(const std::vector<unsigned char>& rawBytes, int imageW, int imageH, int maxSize) {
    int flip = 0;
    const auto jpg = extractThumbJpeg(rawBytes, flip);
    if (jpg.empty()) return {};
    QImage im = QImage::fromData(jpg.data(), int(jpg.size()));
    if (im.isNull() || std::max(im.width(), im.height()) < 300) return {};
    if (flip == 3) im = im.transformed(QTransform().rotate(180));
    else if (flip == 6) im = im.transformed(QTransform().rotate(90));
    else if (flip == 5) im = im.transformed(QTransform().rotate(-90));
    if (imageW > 0 && imageH > 0) {
        const double a = double(im.width()) / im.height(), b = double(imageW) / imageH;
        if (std::abs(a / b - 1) > 0.04) return {};  // 切り抜き済み・別比率のプレビューは手本にしない
    }
    im = im.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    ImageF out;
    out.resize(im.width(), im.height());
    for (int y = 0; y < im.height(); ++y) {
        const uchar* s = im.constScanLine(y);
        for (int x = 0; x < im.width() * 3; ++x) out.data[size_t(y) * im.width() * 3 + x] = s[x] / 255.0f;
    }
    return out;
}

}  // namespace rd
