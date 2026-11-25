#include "engine/Local.h"

#include <QJsonObject>
#include <algorithm>
#include <cmath>

#include "engine/Color.h"
#include "engine/Params.h"
#include "engine/Pipeline.h"
#include "engine/Skin.h"

namespace rd {

namespace {
float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float num(const QJsonObject& o, const char* k, float d) { return float(o.value(k).toDouble(d)); }
}  // namespace

// ---------------------------------------------------------------- JSON

QJsonArray masksToJson(const std::vector<LocalMask>& ms) {
    QJsonArray a;
    for (const auto& m : ms) {
        QJsonObject o{{"type", m.type},         {"name", m.name},           {"enabled", m.enabled},
                      {"invert", m.invert},     {"x0", m.x0},               {"y0", m.y0},
                      {"x1", m.x1},             {"y1", m.y1},               {"angle", m.angle},
                      {"feather", m.feather},   {"exposure", m.exposure},   {"contrast", m.contrast},
                      {"highlights", m.highlights}, {"shadows", m.shadows}, {"temp", m.temp},
                      {"tint", m.tint},         {"saturation", m.saturation}};
        if (!m.dabs.empty()) {
            QJsonArray d;
            for (const auto& b : m.dabs) d.append(QJsonArray{b.x, b.y, b.r, b.flow, b.erase ? 1 : 0});
            o["dabs"] = d;
        }
        a.append(o);
    }
    return a;
}

std::vector<LocalMask> masksFromJson(const QJsonArray& a) {
    std::vector<LocalMask> out;
    for (const auto& v : a) {
        const QJsonObject o = v.toObject();
        LocalMask m;
        m.type = o.value("type").toString("linear");
        m.name = o.value("name").toString();
        m.enabled = o.value("enabled").toBool(true);
        m.invert = o.value("invert").toBool(false);
        m.x0 = num(o, "x0", 0.5f);
        m.y0 = num(o, "y0", 0.3f);
        m.x1 = num(o, "x1", 0.5f);
        m.y1 = num(o, "y1", 0.6f);
        m.angle = num(o, "angle", 0);
        m.feather = num(o, "feather", 50);
        m.exposure = num(o, "exposure", 0);
        m.contrast = num(o, "contrast", 0);
        m.highlights = num(o, "highlights", 0);
        m.shadows = num(o, "shadows", 0);
        m.temp = num(o, "temp", 0);
        m.tint = num(o, "tint", 0);
        m.saturation = num(o, "saturation", 0);
        for (const auto& d : o.value("dabs").toArray()) {
            const QJsonArray q = d.toArray();
            if (q.size() >= 4) m.dabs.push_back({float(q[0].toDouble()), float(q[1].toDouble()), float(q[2].toDouble()),
                                                 float(q[3].toDouble()), q.size() > 4 && q[4].toInt() != 0});
        }
        out.push_back(std::move(m));
    }
    return out;
}

QJsonArray spotsToJson(const std::vector<Spot>& s) {
    QJsonArray a;
    for (const auto& p : s)
        a.append(QJsonObject{{"x", p.x}, {"y", p.y}, {"sx", p.sx}, {"sy", p.sy}, {"r", p.r}, {"feather", p.feather}, {"clone", p.clone}});
    return a;
}

std::vector<Spot> spotsFromJson(const QJsonArray& a) {
    std::vector<Spot> out;
    for (const auto& v : a) {
        const QJsonObject o = v.toObject();
        Spot s{num(o, "x", 0), num(o, "y", 0), num(o, "sx", 0), num(o, "sy", 0), num(o, "r", 0.01f)};
        s.feather = num(o, "feather", 50);
        s.clone = o.value("clone").toBool(false);
        out.push_back(s);
    }
    return out;
}

bool localActive(const Params& p) {
    if (!p.enabled.local) return false;
    for (const auto& m : p.masks)
        if (m.enabled) return true;
    return p.showMaskIndex >= 0;
}

// ---------------------------------------------------------------- マスク

MaskSet::MaskSet(const Params& p, int w, int h, const PixelMap& map) : p_(p), w_(w), h_(h), map_(map) {
    if (!p.enabled.local) return;
    for (size_t i = 0; i < p.masks.size(); ++i) {
        const LocalMask& m = p.masks[i];
        if (!m.enabled && int(i) != p.showMaskIndex) continue;
        masks_.push_back(&m);
        orig_.push_back(int(i));
        std::vector<float> b;
        if (m.type == "brush") {
            b.assign(size_t(w) * h, 0.0f);
            const float hard = 1.0f - std::clamp(m.feather, 0.0f, 100.0f) / 100.0f;
            for (const BrushDab& d : m.dabs) {
                const double cx = (d.x - map.nx0) / map.dx - 0.5, cy = (d.y - map.ny0) / map.dy - 0.5;
                const double R = d.r / map.dx;
                if (R < 0.3) continue;
                const int xa = std::max(0, int(cx - R - 1)), xb = std::min(w - 1, int(cx + R + 1));
                const int ya = std::max(0, int(cy - R - 1)), yb = std::min(h - 1, int(cy + R + 1));
                for (int y = ya; y <= yb; ++y)
                    for (int x = xa; x <= xb; ++x) {
                        const double e = std::hypot(x - cx, y - cy) / R;
                        if (e >= 1) continue;
                        const float f = d.flow * (1.0f - smooth01(float((e - hard) / std::max(1e-3, 1.0 - hard))));
                        float& v = b[size_t(y) * w + x];
                        v = d.erase ? v * (1 - f) : std::max(v, f);
                    }
            }
        }
        brush_.push_back(std::move(b));
    }
}

int MaskSet::slotOf(int paramIndex) const {
    for (size_t i = 0; i < orig_.size(); ++i)
        if (orig_[i] == paramIndex) return int(i);
    return -1;
}

void MaskSet::eval(int x, int y, const float disp[3], float* out) const {
    const double nx = map_.nx0 + (x + 0.5) * map_.dx, ny = map_.ny0 + (y + 0.5) * map_.dy;
    const double asp = map_.fullH / map_.fullW;  // 高さ方向を幅の単位に
    for (size_t i = 0; i < masks_.size(); ++i) {
        const LocalMask& m = *masks_[i];
        float v = 0;
        if (m.type == "linear") {
            const double ax = m.x0, ay = m.y0 * asp, bx = m.x1, by = m.y1 * asp;
            const double vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy;
            const double t = L2 > 1e-12 ? ((nx - ax) * vx + (ny * asp - ay) * vy) / L2 : 0;
            v = 1.0f - smooth01(float(t));
        } else if (m.type == "radial") {
            const double a = m.angle * 3.14159265358979 / 180.0;
            const double dx = nx - m.x0, dy = (ny - m.y0) * asp;
            const double u = std::cos(a) * dx + std::sin(a) * dy, w = -std::sin(a) * dx + std::cos(a) * dy;
            const double rx = std::max(1e-4f, m.x1), ry = std::max(1e-4, double(m.y1) * asp);
            const double e = std::sqrt((u / rx) * (u / rx) + (w / ry) * (w / ry));
            const float f0 = 1.0f - std::clamp(m.feather, 1.0f, 100.0f) / 100.0f * 0.95f;
            v = 1.0f - smooth01(float((e - f0) / (1.0 - f0)));
        } else if (m.type == "brush") {
            v = brush_[i].empty() ? 0.0f : brush_[i][size_t(y) * w_ + x];
        } else {
            float hh, ss, vv;
            rgbToHsv(std::clamp(disp[0], 0.f, 1.f), std::clamp(disp[1], 0.f, 1.f), std::clamp(disp[2], 0.f, 1.f), hh, ss, vv);
            if (m.type == "skin") {
                v = skinMask(p_, hh, ss, vv);
            } else if (m.type == "sky") {
                // 青空：青〜水色。曇り空：上の方の明るく色の薄い所
                const float blue = smooth01((hh - 175) / 20) * (1 - smooth01((hh - 250) / 20)) * smooth01((ss - 0.08f) / 0.12f) *
                                   smooth01((vv - 0.35f) / 0.2f);
                const float white = smooth01((vv - 0.78f) / 0.12f) * (1 - smooth01((ss - 0.08f) / 0.1f));
                const float top = 1.0f - smooth01(float((ny - 0.3) / 0.4));
                v = std::max(blue * (0.5f + 0.5f * top), white * top * 0.85f);
            }
        }
        if (m.invert) v = 1.0f - v;
        out[i] = std::clamp(v, 0.0f, 1.0f);
    }
}

void localLinearGains(const MaskSet& ms, const float* m, float gain[3]) {
    gain[0] = gain[1] = gain[2] = 1;
    for (int i = 0; i < ms.count(); ++i) {
        const LocalMask& k = ms.mask(i);
        if (!k.enabled || m[i] <= 0) continue;
        const float e = std::exp2(k.exposure * m[i]);
        const float t = k.temp / 100.0f * m[i], g = k.tint / 100.0f * m[i];
        gain[0] *= e * std::exp(0.35f * t) * std::exp(0.12f * g);
        gain[1] *= e * std::exp(-0.25f * g);
        gain[2] *= e * std::exp(-0.35f * t) * std::exp(0.12f * g);
    }
}

void localDisplayAdjust(const MaskSet& ms, const float* m, float v[3]) {
    for (int i = 0; i < ms.count(); ++i) {
        const LocalMask& k = ms.mask(i);
        if (!k.enabled || m[i] <= 0) continue;
        const float a = m[i];
        float Y = v[0] * kLuma[0] + v[1] * kLuma[1] + v[2] * kLuma[2];
        const float y0 = std::clamp(Y, 0.0f, 1.0f);
        float y = y0;
        if (k.contrast != 0) y -= k.contrast / 100.0f * a * 0.30f * std::sin(2 * 3.14159265f * y) / (2 * 3.14159265f) * 2;
        if (k.highlights != 0) y += k.highlights / 100.0f * a * 0.35f * y0 * y0 * (1 - y0) * 4 * (y0 > 0.5f ? 1.0f : y0 * 2);
        if (k.shadows != 0) y += k.shadows / 100.0f * a * 0.35f * (1 - y0) * (1 - y0) * y0 * 4 * (y0 < 0.5f ? 1.0f : (1 - y0) * 2);
        const float d = y - y0;
        for (int c = 0; c < 3; ++c) v[c] += d;
        if (k.saturation != 0) {
            Y = v[0] * kLuma[0] + v[1] * kLuma[1] + v[2] * kLuma[2];
            const float f = 1.0f + k.saturation / 100.0f * a;
            for (int c = 0; c < 3; ++c) v[c] = Y + (v[c] - Y) * f;
        }
    }
}

// ---------------------------------------------------------------- スポット修復

int spotMargin(const Params& p, double fullW) {
    float r = 0;
    for (const auto& s : p.spots) r = std::max(r, s.r);
    return p.spots.empty() ? 0 : int(r * fullW * 1.6) + 4;
}

namespace {
void blurPlane(std::vector<float>& a, int w, int h, float sigma) { gaussianBlur(a, w, h, sigma); }
}  // namespace

void applySpots(ImageF& img, const Params& p, const PixelMap& map, const PatchFetch& fetch) {
    if (!p.enabled.local || p.spots.empty()) return;
    const ImageF orig = fetch ? ImageF{} : img;  // 自分自身から取るときは、直す前の画像から
    for (const Spot& s : p.spots) {
        const double R = s.r / map.dx;
        if (R < 0.7) continue;
        const double cx = (s.x - map.nx0) / map.dx - 0.5, cy = (s.y - map.ny0) / map.dy - 0.5;
        const int pad = int(std::ceil(R * 1.4)) + 2;
        const int bx = int(std::floor(cx)) - pad, by = int(std::floor(cy)) - pad, bw = 2 * pad + 1, bh = 2 * pad + 1;
        if (bx + bw < 0 || by + bh < 0 || bx >= img.width || by >= img.height) continue;
        // コピー元（修復先と同じ大きさの範囲）
        const double ox = (s.sx - s.x) / map.dx, oy = (s.sy - s.y) / map.dy;
        ImageF src;
        if (fetch) {
            src = fetch(map.nx0 + (bx + ox) * map.dx, map.ny0 + (by + oy) * map.dy, bw, bh);
        } else {
            src.resize(bw, bh);
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) {
                    const int xx = std::clamp(int(std::lround(bx + x + ox)), 0, orig.width - 1);
                    const int yy = std::clamp(int(std::lround(by + y + oy)), 0, orig.height - 1);
                    std::copy(orig.px(xx, yy), orig.px(xx, yy) + 3, src.px(x, y));
                }
        }
        if (src.width != bw || src.height != bh) continue;
        // 修復：周り（輪）での「修復先 − コピー元」の差を内側へなめらかに広げて足す（明るさ・色をなじませる）
        std::vector<float> off[3];
        if (!s.clone) {
            std::vector<float> wgt(size_t(bw) * bh, 0.0f);
            for (auto& o : off) o.assign(size_t(bw) * bh, 0.0f);
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) {
                    const int ix = bx + x, iy = by + y;
                    if (ix < 0 || iy < 0 || ix >= img.width || iy >= img.height) continue;
                    const double e = std::hypot(ix - cx, iy - cy) / R;
                    if (e < 1.0 || e > 1.35) continue;
                    const size_t k = size_t(y) * bw + x;
                    wgt[k] = 1;
                    for (int c = 0; c < 3; ++c) off[c][k] = img.px(ix, iy)[c] - src.px(x, y)[c];
                }
            const float sig = float(std::max(1.0, R * 0.55));
            blurPlane(wgt, bw, bh, sig);
            for (auto& o : off) {
                blurPlane(o, bw, bh, sig);
                for (size_t k = 0; k < o.size(); ++k) o[k] = wgt[k] > 1e-4f ? o[k] / wgt[k] : 0.0f;
            }
        }
        const float hard = 1.0f - std::clamp(s.feather, 0.0f, 100.0f) / 100.0f * 0.8f;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const int ix = bx + x, iy = by + y;
                if (ix < 0 || iy < 0 || ix >= img.width || iy >= img.height) continue;
                const double e = std::hypot(ix - cx, iy - cy) / R;
                if (e >= 1) continue;
                const float a = 1.0f - smooth01(float((e - hard) / std::max(1e-3f, 1.0f - hard)));
                float* d = img.px(ix, iy);
                const size_t k = size_t(y) * bw + x;
                for (int c = 0; c < 3; ++c) {
                    const float v = src.px(x, y)[c] + (s.clone ? 0.0f : off[c][k]);
                    d[c] = d[c] * (1 - a) + std::max(v, 0.0f) * a;
                }
            }
    }
}

void autoSpotSource(const ImageF& lin, Spot& s) {
    const int W = lin.width, H = lin.height;
    const double cx = s.x * W, cy = s.y * H, R = std::max(1.5, double(s.r) * W);
    auto at = [&](double x, double y) {
        const int xi = std::clamp(int(x), 0, W - 1), yi = std::clamp(int(y), 0, H - 1);
        const float* q = lin.px(xi, yi);
        return std::sqrt(std::max(0.0f, q[0] * kLuma[0] + q[1] * kLuma[1] + q[2] * kLuma[2]));
    };
    // 修復先の周り（輪）と似ている場所を探す。中身（直したい物）は比べない
    std::vector<std::pair<double, double>> ring;
    for (int k = 0; k < 24; ++k)
        for (double rr : {1.15, 1.35}) {
            const double a = k * 2 * 3.14159265358979 / 24;
            ring.push_back({std::cos(a) * R * rr, std::sin(a) * R * rr});
        }
    double best = 1e30, bx = cx + 2.5 * R, by = cy;
    for (double dist : {2.2, 2.8, 3.6, 4.6})
        for (int k = 0; k < 24; ++k) {
            const double a = k * 2 * 3.14159265358979 / 24;
            const double qx = cx + std::cos(a) * R * dist, qy = cy + std::sin(a) * R * dist;
            if (qx - R < 0 || qy - R < 0 || qx + R >= W || qy + R >= H) continue;
            double cost = 0;
            for (auto [dx, dy] : ring) {
                const double d = at(cx + dx, cy + dy) - at(qx + dx, qy + dy);
                cost += d * d;
            }
            // 中身もなめらかな方がよい（コピー元に別の汚れがあると困る）
            double mean = 0, var = 0;
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j) {
                    const double v = at(qx + i * R / 3, qy + j * R / 3);
                    mean += v;
                    var += v * v;
                }
            mean /= 25;
            var = var / 25 - mean * mean;
            cost += var * 30 + dist * 1e-4;
            if (cost < best) {
                best = cost;
                bx = qx;
                by = qy;
            }
        }
    s.sx = float(bx / W);
    s.sy = float(by / H);
}

}  // namespace rd
