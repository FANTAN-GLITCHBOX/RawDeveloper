#include "engine/RawLoader.h"

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace rd {

namespace {

// LibRaw で現像してリニア float に（Python 版の _raw_postprocess と同じ設定）
bool develop(LibRaw& raw, bool half, bool cameraWb, ImageF& out, std::string& err) {
    auto& p = raw.imgdata.params;
    p.half_size = half ? 1 : 0;
    p.use_camera_wb = cameraWb ? 1 : 0;
    p.use_auto_wb = 0;
    p.output_color = 1;   // sRGB
    p.output_bps = 16;
    p.gamm[0] = 1.0;      // リニア
    p.gamm[1] = 1.0;
    p.no_auto_bright = 1;
    p.highlight = 2;      // Blend
    int rc = raw.dcraw_process();
    if (rc != LIBRAW_SUCCESS) {
        err = libraw_strerror(rc);
        return false;
    }
    int e = 0;
    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&e);
    if (!img) {
        err = libraw_strerror(e);
        return false;
    }
    std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> guard(
        img, [](libraw_processed_image_t* i) { LibRaw::dcraw_clear_mem(i); });
    if (img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3 || img->bits != 16) {
        err = "unsupported image format";
        return false;
    }
    out.resize(img->width, img->height);
    const auto* src = reinterpret_cast<const unsigned short*>(img->data);
    const size_t n = static_cast<size_t>(img->width) * img->height * 3;
    constexpr float k = 1.0f / 65535.0f;
    for (size_t i = 0; i < n; ++i) out.data[i] = src[i] * k;
    return true;
}

std::array<float, 3> asShotFrom(const ImageF& day, const ImageF& cam) {
    double sd[3] = {0, 0, 0}, sc[3] = {0, 0, 0};
    size_t cnt = 0;
    const size_t n = static_cast<size_t>(std::min(day.width, cam.width)) * std::min(day.height, cam.height);
    for (size_t i = 0; i < n; ++i) {
        const float* d = &day.data[i * 3];
        const float* c = &cam.data[i * 3];
        float dmax = std::max({d[0], d[1], d[2]}), dmin = std::min({d[0], d[1], d[2]});
        float cmax = std::max({c[0], c[1], c[2]});
        if (dmax < 0.9f && cmax < 0.9f && dmin > 0.01f) {
            for (int k = 0; k < 3; ++k) {
                sd[k] += d[k];
                sc[k] += c[k];
            }
            ++cnt;
        }
    }
    if (cnt < 100) return {1, 1, 1};
    float g[3];
    for (int k = 0; k < 3; ++k) g[k] = static_cast<float>((sc[k] + 1e-6) / (sd[k] + 1e-6));
    return {g[0] / g[1], 1.0f, g[2] / g[1]};
}

}  // namespace

namespace {
// EXIF の露出補正（0x9204, SRATIONAL）を拾う
struct ExifGrab {
    float bias = 0;
    bool has = false;
};
void exifCallback(void* ctx, int tag, int type, int len, unsigned int ord, void* ifp, INT64 /*base*/) {
    auto* g = static_cast<ExifGrab*>(ctx);
    if ((tag & 0xffff) != 0x9204 || g->has || len < 1 || (type != 10 && type != 5)) return;
    auto* s = static_cast<LibRaw_abstract_datastream*>(ifp);
    unsigned char b[8];
    if (s->read(b, 1, 8) != 8) return;
    auto u32 = [&](const unsigned char* p) {
        return ord == 0x4949 ? (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24)
                             : (uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24);
    };
    const uint32_t nu = u32(b), de = u32(b + 4);
    if (de == 0) return;
    const double v = type == 10 ? double(int32_t(nu)) / double(int32_t(de)) : double(nu) / double(de);
    if (std::abs(v) > 10) return;
    g->bias = float(v);
    g->has = true;
}

void fillMeta(const LibRaw& raw, Metadata& m) {
    const auto& id = raw.imgdata;
    m.make = id.idata.make;
    m.model = id.idata.model;
    m.lensMake = id.lens.LensMake;
    m.lensModel = id.lens.Lens;
    m.iso = id.other.iso_speed;
    m.shutter = id.other.shutter;
    m.aperture = id.other.aperture;
    m.focal = id.other.focal_len;
    m.timestamp = static_cast<long long>(id.other.timestamp);
    m.fullWidth = id.sizes.flip & 4 ? id.sizes.height : id.sizes.width;
    m.fullHeight = id.sizes.flip & 4 ? id.sizes.width : id.sizes.height;
}
}  // namespace

Metadata readRawMetadata(const std::vector<unsigned char>& bytes) {
    Metadata m;
    auto raw = std::make_unique<LibRaw>();
    ExifGrab g;
    raw->set_exifparser_handler(exifCallback, &g);
    if (raw->open_buffer(const_cast<unsigned char*>(bytes.data()), bytes.size()) == LIBRAW_SUCCESS) fillMeta(*raw, m);
    m.exposureBias = g.bias;
    m.hasBias = g.has;
    return m;
}

LoadedRaw loadRaw(const std::vector<unsigned char>& bytes, bool half) {
    LoadedRaw res;
    auto raw = std::make_unique<LibRaw>();
    ExifGrab g;
    raw->set_exifparser_handler(exifCallback, &g);
    int rc = raw->open_buffer(const_cast<unsigned char*>(bytes.data()), bytes.size());
    if (rc != LIBRAW_SUCCESS) {
        res.error = libraw_strerror(rc);
        return res;
    }
    if ((rc = raw->unpack()) != LIBRAW_SUCCESS) {
        res.error = libraw_strerror(rc);
        return res;
    }
    fillMeta(*raw, res.meta);
    res.meta.exposureBias = g.bias;
    res.meta.hasBias = g.has;

    // 撮影時 WB の推定：半分サイズでデイライト / カメラ WB を現像して比べる
    ImageF dayHalf, camHalf;
    std::string err;
    if (!develop(*raw, true, false, dayHalf, err) || !develop(*raw, true, true, camHalf, err)) {
        res.error = err;
        return res;
    }
    res.asShotGains = asShotFrom(dayHalf, camHalf);
    if (half) {
        res.linear = std::move(dayHalf);
    } else if (!develop(*raw, false, false, res.linear, err)) {
        res.error = err;
    }
    return res;
}

std::vector<unsigned char> extractThumbJpeg(const std::vector<unsigned char>& bytes, int& flip) {
    flip = 0;
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_buffer(const_cast<unsigned char*>(bytes.data()), bytes.size()) != LIBRAW_SUCCESS) return {};
    flip = raw->imgdata.sizes.flip;
    if (raw->unpack_thumb() != LIBRAW_SUCCESS) return {};
    int e = 0;
    libraw_processed_image_t* t = raw->dcraw_make_mem_thumb(&e);
    if (!t) return {};
    std::vector<unsigned char> out;
    if (t->type == LIBRAW_IMAGE_JPEG) out.assign(t->data, t->data + t->data_size);
    LibRaw::dcraw_clear_mem(t);
    return out;
}

bool isRawExtension(const std::string& e) {
    static const char* exts[] = {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef",
                                 "dng", "srw", "x3f", "3fr", "iiq", "erf", "mos", "mrw", "kdc", "dcr", "rwl"};
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

ImageF downscale(const ImageF& src, int maxEdge) {
    const int longEdge = std::max(src.width, src.height);
    if (longEdge <= maxEdge) return src;
    const double s = static_cast<double>(maxEdge) / longEdge;
    const int w = std::max(1, static_cast<int>(src.width * s + 0.5));
    const int h = std::max(1, static_cast<int>(src.height * s + 0.5));
    ImageF dst;
    dst.resize(w, h);
    for (int y = 0; y < h; ++y) {
        const int y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
            float acc[3] = {0, 0, 0};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const float* p = src.px(xx, yy);
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
            const float inv = 1.0f / ((y1 - y0) * (x1 - x0));
            float* d = dst.px(x, y);
            d[0] = acc[0] * inv;
            d[1] = acc[1] * inv;
            d[2] = acc[2] * inv;
        }
    }
    return dst;
}

}  // namespace rd
