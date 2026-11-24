#include "engine/Lens.h"

#include <lensfun/lensfun.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <regex>

#include "engine/Parallel.h"
#include "engine/Params.h"
#include "engine/RawLoader.h"

namespace rd {

namespace {
std::vector<std::string>& dataDirs() {
    static std::vector<std::string> d;
    return d;
}

std::string norm(std::string s) {
    for (auto& c : s)
        if (c == '\t') c = ' ';
    s = std::regex_replace(s, std::regex("\\s+"), " ");
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

std::string keyOf(const lfLens* l) { return std::string(l->Maker ? l->Maker : "") + "|" + (l->Model ? l->Model : ""); }
}  // namespace

struct LensDb::Impl {
    std::unique_ptr<lfDatabase> db;
    std::map<std::string, const lfLens*> byKey;
    std::vector<std::string> keys;
    mutable std::mutex mu;
};

void LensDb::setDataDirs(std::vector<std::string> dirs) { dataDirs() = std::move(dirs); }

LensDb& LensDb::get() {
    static LensDb inst;
    return inst;
}

LensDb::LensDb() : d_(std::make_unique<Impl>()) {
    d_->db = std::make_unique<lfDatabase>();
    int loaded = 0;
    std::vector<std::string> dirs = dataDirs();
    const QString user = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/lenses";
    dirs.push_back(user.toStdString());
    for (const auto& dir : dirs) {
        const QDir qd(QString::fromStdString(dir));
        for (const QFileInfo& fi : qd.entryInfoList({"*.xml"}, QDir::Files, QDir::Name)) {
            const QByteArray path = QDir::toNativeSeparators(fi.absoluteFilePath()).toLocal8Bit();
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray data = f.readAll();
            if (d_->db->Load(path.constData(), data.constData(), size_t(data.size())) == LF_NO_ERROR) ++loaded;
        }
    }
    if (loaded == 0) d_->db->Load();  // システムのデータ（Linux の開発環境用）
    if (const lfLens* const* ls = d_->db->GetLenses())
        for (int i = 0; ls[i]; ++i) {
            const std::string k = keyOf(ls[i]);
            if (!d_->byKey.count(k)) {
                d_->byKey[k] = ls[i];
                d_->keys.push_back(k);
            }
        }
    std::sort(d_->keys.begin(), d_->keys.end(), [](const std::string& a, const std::string& b) {
        std::string la = a, lb = b;
        std::transform(la.begin(), la.end(), la.begin(), ::tolower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
        return la < lb;
    });
}

bool LensDb::ok() const { return !d_->keys.empty(); }

std::vector<std::string> LensDb::allKeys() const { return d_->keys; }

std::string LensDb::displayName(const std::string& key) const {
    const auto bar = key.find('|');
    return bar == std::string::npos ? key : key.substr(0, bar) + "  " + key.substr(bar + 1);
}

LensInfo LensDb::identify(const Metadata& meta) const {
    std::lock_guard<std::mutex> lock(d_->mu);
    LensInfo info;
    info.focal = meta.focal;
    info.aperture = meta.aperture;
    const lfCamera* cam = nullptr;
    if (!meta.make.empty()) {
        const lfCamera** cams = d_->db->FindCamerasExt(meta.make.c_str(), meta.model.c_str());
        if (cams && cams[0]) cam = cams[0];
        lf_free(cams);
    }
    info.crop = cam ? cam->CropFactor : 1.0f;
    const std::string name = norm(meta.lensModel);
    if (name.empty()) {
        info.status = "EXIF にレンズ情報がありません。下の一覧から選べます";
        return info;
    }
    const char* maker = meta.lensMake.empty() ? nullptr : meta.lensMake.c_str();
    const lfLens** found = nullptr;
    for (int loose = 0; loose < 2 && !(found && found[0]); ++loose) {
        if (found) lf_free(found);
        found = d_->db->FindLenses(cam, maker, name.c_str(), loose ? LF_SEARCH_LOOSE : 0);
        if (!(found && found[0]) && maker) {
            if (found) lf_free(found);
            found = d_->db->FindLenses(cam, nullptr, name.c_str(), loose ? LF_SEARCH_LOOSE : 0);
        }
    }
    if (found && found[0]) {
        const lfLens* best = found[0];
        info.autoKey = keyOf(best);
        info.confidence = std::min(100, best->Score);
        info.status = "検出：" + displayName(info.autoKey) + "（Lensfun）";
        if (info.confidence < 50) info.status += "　※一致度が低いので確認してください";
    } else {
        info.status = "プロファイル未検出：「" + meta.lensModel + "」。下の一覧から選んでください";
    }
    if (found) lf_free(found);
    return info;
}

std::string effectiveLensKey(const Params& p, const LensInfo& info) {
    if (!p.lensKey.isEmpty()) return p.lensKey.toStdString();
    return info.autoKey;
}

bool lensActive(const Params& p, const LensInfo* info) {
    if (!p.enabled.lens) return false;
    if (std::abs(p.lensManualDistortion) > 0.5f || std::abs(p.lensManualVignetting) > 0.5f) return true;
    return p.lensProfile && info && !effectiveLensKey(p, *info).empty() && info->focal > 0;
}

// ---------------------------------------------------------------- remap

struct LensApply {
    static const lfLens* lens(const std::string& key) {
        auto& d = *LensDb::get().d_;
        auto it = d.byKey.find(key);
        return it == d.byKey.end() ? nullptr : it->second;
    }
};

ImageF lensRemapRegion(const ImageF& src, const Params& p, const LensInfo& info, int x0, int y0, int w, int h) {
    const int W = src.width, H = src.height;
    ImageF out;
    out.resize(w, h);
    const lfLens* lens = nullptr;
    if (p.enabled.lens && p.lensProfile && info.focal > 0) lens = LensApply::lens(effectiveLensKey(p, info));
    const float distAmt = p.lensDistortion / 100.0f, vigAmt = p.lensVignetting / 100.0f;
    const float md = p.enabled.lens ? p.lensManualDistortion : 0, mv = p.enabled.lens ? p.lensManualVignetting : 0;
    const double cx = (W - 1) / 2.0, cy = (H - 1) / 2.0, r0 = std::hypot(cx, cy);
    // 手動の歪曲（放射方向）。四隅が収まるよう拡大率を合わせる
    const double ma = -md / 100.0 * 0.15;
    const double ms = ma < 0 ? 1.0 / (1 + ma) : 1.0;

    parallelFor(h, [&](int ya, int yb) {
        std::unique_ptr<lfModifier> mod;
        int flags = 0;
        if (lens) {
            mod = std::make_unique<lfModifier>(lens, info.crop, W, H);
            int want = LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING;
            if (p.lensCa) want |= LF_MODIFY_TCA;
            if (p.lensAutotrim) want |= LF_MODIFY_SCALE;
            flags = mod->Initialize(lens, LF_PF_F32, info.focal, info.aperture > 0 ? info.aperture : 5.6f, 1000.0f,
                                    p.lensAutotrim ? 0.0f : 1.0f, LF_RECTILINEAR, want, false);
        }
        const bool geom = mod && (flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_SCALE)) && distAmt > 0;
        const bool vig = mod && (flags & LF_MODIFY_VIGNETTING) && vigAmt > 0;
        std::vector<float> coords(static_cast<size_t>(w) * 6);
        std::vector<float> gain(static_cast<size_t>(w) * 3);
        for (int y = ya; y < yb; ++y) {
            const int sy = y0 + y;
            // 1) 各画素の読み取り元（色ごと）
            if (geom) {
                mod->ApplySubpixelGeometryDistortion(float(x0), float(sy), w, 1, coords.data());
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c) {
                        float& cxv = coords[(x * 3 + c) * 2];
                        float& cyv = coords[(x * 3 + c) * 2 + 1];
                        cxv = (x0 + x) + distAmt * (cxv - (x0 + x));
                        cyv = sy + distAmt * (cyv - sy);
                    }
            } else {
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c) {
                        coords[(x * 3 + c) * 2] = float(x0 + x);
                        coords[(x * 3 + c) * 2 + 1] = float(sy);
                    }
            }
            if (std::abs(md) > 0.5f) {
                for (size_t i = 0; i < static_cast<size_t>(w) * 3; ++i) {
                    const double nx = (coords[i * 2] - cx) / r0, ny = (coords[i * 2 + 1] - cy) / r0;
                    const double f = (1 + ma * (nx * nx + ny * ny)) * ms;
                    coords[i * 2] = float(cx + nx * f * r0);
                    coords[i * 2 + 1] = float(cy + ny * f * r0);
                }
            }
            // 2) 周辺減光のゲイン（なめらかに変わるので、出力側の位置で計算して近似）
            std::fill(gain.begin(), gain.end(), 1.0f);
            if (vig) {
                mod->ApplyColorModification(gain.data(), float(x0), float(sy), w, 1, LF_CR_3(RED, GREEN, BLUE),
                                            int(w * 3 * sizeof(float)));
                if (std::abs(vigAmt - 1.0f) > 1e-3f)
                    for (float& g : gain) g = std::pow(std::max(g, 1e-4f), vigAmt);
            }
            float* o = out.px(0, y);
            for (int x = 0; x < w; ++x) {
                float mg = 1.0f;
                if (std::abs(mv) > 0.5f) {
                    const double dx = x0 + x - cx, dy = sy - cy;
                    const double r2 = (dx * dx + dy * dy) / (cx * cx + cy * cy);
                    mg = float(std::max(0.05, 1 + mv / 100.0 * 1.2 * std::pow(r2, 1.5)));
                }
                for (int c = 0; c < 3; ++c) {
                    const float fx = coords[(x * 3 + c) * 2], fy = coords[(x * 3 + c) * 2 + 1];
                    // 端は反射して読む（OpenCV の BORDER_REFLECT と同じ考え方）
                    auto refl = [](int v, int n) {
                        if (n == 1) return 0;
                        while (v < 0 || v >= n) v = v < 0 ? -v - 1 : 2 * n - v - 1;
                        return v;
                    };
                    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                    const float ax = fx - ix, ay = fy - iy;
                    const int xa = refl(ix, W), xb = refl(ix + 1, W), ya2 = refl(iy, H), yb2 = refl(iy + 1, H);
                    const float v = (src.px(xa, ya2)[c] * (1 - ax) + src.px(xb, ya2)[c] * ax) * (1 - ay) +
                                    (src.px(xa, yb2)[c] * (1 - ax) + src.px(xb, yb2)[c] * ax) * ay;
                    o[x * 3 + c] = v * gain[x * 3 + c] * mg;
                }
            }
        }
    });
    return out;
}

}  // namespace rd
