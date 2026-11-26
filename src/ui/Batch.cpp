#include "ui/Batch.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "engine/Color.h"
#include "engine/Lens.h"
#include "engine/Pipeline.h"
#include "engine/Profile.h"
#include "engine/Denoise.h"
#include "ui/Library.h"

namespace rd {

const std::vector<PasteGroup>& pasteGroups() {
    static const std::vector<PasteGroup> g{
        {"wb", "ホワイトバランス", {"wb", "enabled.basic"}},
        {"exposure", "露出", {"exposure", "enabled.basic"}},
        {"tone", "コントラスト・ハイライト・シャドウ・白・黒", {"contrast", "highlights", "shadows", "whites", "blacks", "enabled.basic"}},
        {"presence", "彩度・自然な彩度・テクスチャ・明瞭度・かすみの除去",
         {"saturation", "vibrance", "texture", "clarity", "dehaze", "enabled.basic"}},
        {"wheels", "カラーホイール", {"wheels", "enabled.wheels"}},
        {"curve", "トーンカーブ", {"curve", "enabled.curve"}},
        {"hsl", "HSL", {"hsl", "enabled.hsl"}},
        {"skin", "肌色", {"skin", "enabled.skin"}},
        {"local", "部分補正（マスク）", {"masks", "enabled.local"}},
        {"spots", "スポット修復", {"spots"}},
        {"detail", "ディテール（シャープ・ノイズ低減）", {"sharpen", "nr", "enabled.detail"}},
        {"lens", "レンズ補正", {"lens", "enabled.lens"}},
        {"crop", "切り抜き・角度", {"crop", "enabled.crop"}},
    };
    return g;
}

QStringList defaultPasteGroups() { return {"exposure", "tone", "presence", "wheels", "curve", "hsl", "skin", "detail", "lens"}; }

namespace {
QJsonValue getKey(const QJsonObject& o, const QString& key) {
    const int dot = key.indexOf('.');
    if (dot < 0) return o.value(key);
    return o.value(key.left(dot)).toObject().value(key.mid(dot + 1));
}
void setKey(QJsonObject& o, const QString& key, const QJsonValue& v) {
    const int dot = key.indexOf('.');
    if (dot < 0) {
        o[key] = v;
        return;
    }
    QJsonObject sub = o.value(key.left(dot)).toObject();
    sub[key.mid(dot + 1)] = v;
    o[key.left(dot)] = sub;
}
const QStringList& allKeys() {
    static const QStringList k{"wb",     "exposure",       "contrast", "texture", "clarity", "dehaze",       "highlights",    "shadows",
                               "whites", "blacks",         "saturation",     "vibrance",      "curve",
                               "hsl",    "wheels",         "sharpen",        "nr",            "lens",
                               "crop",   "enabled.basic",  "enabled.wheels", "enabled.curve", "enabled.hsl",
                               "enabled.detail", "enabled.lens", "enabled.crop", "skin", "enabled.skin", "masks", "spots", "enabled.local"};
    return k;
}
}  // namespace

Params copyKeys(const Params& dst, const Params& src, const QStringList& keys) {
    QJsonObject d = dst.toJson();
    const QJsonObject s = src.toJson();
    for (const QString& k : keys) setKey(d, k, getKey(s, k));
    return Params::fromJson(d);
}

Params mergeGroups(const Params& dst, const Params& src, const QStringList& groups) {
    QStringList keys;
    for (const auto& g : pasteGroups())
        if (groups.contains(g.id)) keys << g.keys;
    keys.removeDuplicates();
    return copyKeys(dst, src, keys);
}

QStringList changedKeys(const Params& a, const Params& b) {
    const QJsonObject ja = a.toJson(), jb = b.toJson();
    QStringList out;
    for (const QString& k : allKeys())
        if (getKey(ja, k) != getKey(jb, k)) out << k;
    return out;
}

double photoLuminance(const ImageF& lin, const std::array<float, 3>& gains) {
    const ImageF t = downscale(lin, 256);
    std::vector<std::pair<float, float>> v;
    v.reserve(size_t(t.width) * t.height);
    double total = 0;
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            const float* p = t.px(x, y);
            const float l = p[0] * gains[0] * kLuma[0] + p[1] * gains[1] * kLuma[1] + p[2] * gains[2] * kLuma[2];
            const double dx = (x - t.width / 2.0) / (t.width * 0.45), dy = (y - t.height / 2.0) / (t.height * 0.45);
            const float w = float(std::exp(-(dx * dx + dy * dy)));
            v.push_back({std::log2(std::max(l, 1e-6f)), w});
            total += w;
        }
    std::sort(v.begin(), v.end());
    double acc = 0;
    for (auto& e : v) {
        acc += e.second;
        if (acc >= total / 2) return std::exp2(e.first);
    }
    return 0.18;
}

Params readParams(const QString& path, bool* exists) {
    QFile f(path + ".rdev");
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.contains("params")) {
            if (exists) *exists = true;
            return Params::fromJson(o.value("params").toObject());
        }
    }
    if (exists) *exists = false;
    return isRawPhoto(path) ? Params::defaults() : Params::defaultsForImage();  // 輝度ノイズの ISO 初期値は開いたときに
}

Params initialParams(const QString& path, const LoadedRaw& src) {
    if (!isRawPhoto(path)) return Params::defaultsForImage();
    Params p = Params::defaults();
    p.nrLuma = defaultLumaNr(src.meta.iso);
    if (src.cameraRef.width > 0 && QSettings().value("develop/camera_profile", true).toBool())
        matchCamera(src.linear, src.asShotGains, src.cameraRef, p);
    return p;
}

bool writeParams(const QString& path, const Params& p) {
    const QString sp = path + ".rdev";
    QJsonObject o;
    QFile in(sp);
    if (in.open(QIODevice::ReadOnly)) {
        o = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
    }
    o["version"] = 1;
    o["params"] = p.toJson();
    QSaveFile f(sp);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson());
    return f.commit();
}

QImage renderEditedThumb(const QString& path, const LoadedRaw& src, const Params& p) {
    const ImageF small = downscale(src.linear, 480);
    const LensInfo lens = LensDb::get().identify(src.meta);
    DevelopOptions o;
    o.scale = float(small.width) / std::max(1, src.meta.fullWidth > 0 ? src.meta.fullWidth : src.linear.width);
    o.lens = &lens;
    const ImageF g = develop(small, p, src.asShotGains, o);
    const auto rgb = toRgb8(g);
    QImage img(g.width, g.height, QImage::Format_RGB888);
    for (int y = 0; y < g.height; ++y) memcpy(img.scanLine(y), rgb.data() + size_t(y) * g.width * 3, g.width * 3);
    img = img.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString ep = editedThumbPath(path);
    if (!ep.isEmpty()) img.save(ep, "JPG", 85);
    return img;
}

CopyDialog::CopyDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("コピーする調整を選ぶ"));
    QSettings st;
    const QStringList on = st.value("paste/groups", defaultPasteGroups()).toStringList();
    auto* v = new QVBoxLayout(this);
    auto* g = new QGroupBox(QStringLiteral("貼り付ける項目"));
    auto* gl = new QVBoxLayout(g);
    for (const auto& pg : pasteGroups()) {
        auto* cb = new QCheckBox(QString::fromUtf8(pg.label));
        cb->setChecked(on.contains(pg.id));
        gl->addWidget(cb);
        boxes_.push_back({pg.id, cb});
    }
    auto* row = new QHBoxLayout;
    auto* all = new QPushButton(QStringLiteral("すべて"));
    auto* none = new QPushButton(QStringLiteral("なし"));
    auto* def = new QPushButton(QStringLiteral("おすすめ"));
    row->addWidget(all);
    row->addWidget(none);
    row->addWidget(def);
    row->addStretch(1);
    gl->addLayout(row);
    v->addWidget(g);
    match_ = new QCheckBox(QStringLiteral("露出は写真ごとの明るさに合わせる（連写で明るさが違うとき）"));
    match_->setChecked(st.value("paste/match_exposure", false).toBool());
    v->addWidget(match_);
    auto* note = new QLabel(QStringLiteral("貼り付けは Ctrl＋V（選んでいる写真すべてに）。この選択は次回も使います。"));
    note->setStyleSheet("color:#8c8c8c");
    v->addWidget(note);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("コピー"));
    bb->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(bb, &QDialogButtonBox::accepted, this, &CopyDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &CopyDialog::reject);
    v->addWidget(bb);
    connect(all, &QPushButton::clicked, this, [this] { for (auto& b : boxes_) b.second->setChecked(true); });
    connect(none, &QPushButton::clicked, this, [this] { for (auto& b : boxes_) b.second->setChecked(false); });
    connect(def, &QPushButton::clicked, this, [this] {
        const QStringList d = defaultPasteGroups();
        for (auto& b : boxes_) b.second->setChecked(d.contains(b.first));
    });
}

QStringList CopyDialog::groups() const {
    QStringList out;
    for (auto& b : boxes_)
        if (b.second->isChecked()) out << b.first;
    return out;
}
bool CopyDialog::matchExposure() const { return match_->isChecked(); }

void CopyDialog::accept() {
    QSettings st;
    st.setValue("paste/groups", groups());
    st.setValue("paste/match_exposure", matchExposure());
    QDialog::accept();
}

}  // namespace rd
