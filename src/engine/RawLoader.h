#pragma once
#include <array>
#include <string>
#include "engine/Image.h"

namespace rd {

struct Metadata {
    std::string make, model, lensMake, lensModel;
    float iso = 0, shutter = 0, aperture = 0, focal = 0;
    int fullWidth = 0, fullHeight = 0;
};

struct LoadedRaw {
    ImageF linear;                       // デイライト WB で現像したリニア sRGB
    std::array<float, 3> asShotGains{1, 1, 1};  // デイライト基準からの撮影時 WB（G=1）
    Metadata meta;
    std::string error;                   // 空なら成功
};

// RAW を読み込む。half=true は高速な半分サイズ（プレビュー用）
// bytes：ファイルの中身（Windows の日本語パスでも読めるよう、メモリ経由で渡す）
LoadedRaw loadRaw(const std::vector<unsigned char>& bytes, bool half);

// 埋め込みサムネイル（JPEG のバイト列）。flip は LibRaw の向き（0, 3, 5, 6）。無ければ空
std::vector<unsigned char> extractThumbJpeg(const std::vector<unsigned char>& bytes, int& flip);

bool isRawExtension(const std::string& extLower);

}  // namespace rd
