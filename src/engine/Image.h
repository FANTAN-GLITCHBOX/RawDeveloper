#pragma once
// 画像バッファ：RGB float（リニア sRGB）
#include <cstddef>
#include <vector>

namespace rd {

struct ImageF {
    int width = 0;
    int height = 0;
    std::vector<float> data;  // RGBRGB... 行優先

    bool empty() const { return width <= 0 || height <= 0; }
    float* px(int x, int y) { return &data[(static_cast<size_t>(y) * width + x) * 3]; }
    const float* px(int x, int y) const { return &data[(static_cast<size_t>(y) * width + x) * 3]; }
    void resize(int w, int h) {
        width = w;
        height = h;
        data.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    }
};

// 長辺を maxEdge 以下に縮小（面積平均）
ImageF downscale(const ImageF& src, int maxEdge);

}  // namespace rd
