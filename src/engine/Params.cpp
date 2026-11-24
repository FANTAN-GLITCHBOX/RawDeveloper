#include "engine/Params.h"

#include <QJsonArray>

namespace rd {

namespace {

QJsonArray toArr(const float* v, int n) {
    QJsonArray a;
    for (int i = 0; i < n; ++i) a.append(static_cast<double>(v[i]));
    return a;
}

void fromArr(const QJsonValue& v, float* out, int n) {
    const QJsonArray a = v.toArray();
    for (int i = 0; i < n && i < a.size(); ++i) out[i] = static_cast<float>(a[i].toDouble());
}

QJsonArray curveToJson(const CurvePoints& c) {
    QJsonArray a;
    for (auto [x, y] : c) a.append(QJsonArray{x, y});
    return a;
}

CurvePoints curveFromJson(const QJsonValue& v) {
    CurvePoints c;
    for (const auto& p : v.toArray()) {
        const QJsonArray pa = p.toArray();
        if (pa.size() >= 2) c.emplace_back(static_cast<float>(pa[0].toDouble()), static_cast<float>(pa[1].toDouble()));
    }
    if (c.size() < 2) c = {{0, 0}, {1, 1}};
    return c;
}

float num(const QJsonObject& o, const char* k, float def) {
    return o.contains(k) ? static_cast<float>(o.value(k).toDouble(def)) : def;
}

}  // namespace

bool isIdentityCurve(const CurvePoints& c) {
    return c.size() == 2 && c[0].first == 0 && c[0].second == 0 && c[1].first == 1 && c[1].second == 1;
}

Params Params::defaults() { return Params{}; }

Params Params::defaultsForImage() {
    Params p;
    p.sharpenAmount = 0;
    p.nrColor = 0;
    p.lens = QJsonObject{{"profile", false}};
    return p;
}

QJsonObject Params::toJson() const {
    QJsonObject o;
    o["version"] = version;
    o["processVersion"] = processVersion;
    o["wb"] = QJsonObject{{"mode", wbMode}, {"temp", temp}, {"tint", tint}};
    o["exposure"] = exposure;
    o["contrast"] = contrast;
    o["highlights"] = highlights;
    o["shadows"] = shadows;
    o["whites"] = whites;
    o["blacks"] = blacks;
    o["saturation"] = saturation;
    o["vibrance"] = vibrance;
    o["curve"] = QJsonObject{{"Y", curveToJson(curve[0])}, {"R", curveToJson(curve[1])},
                             {"G", curveToJson(curve[2])}, {"B", curveToJson(curve[3])}};
    o["hsl"] = QJsonObject{{"hue", toArr(hslHue.data(), kHslBands)},
                           {"sat", toArr(hslSat.data(), kHslBands)},
                           {"lum", toArr(hslLum.data(), kHslBands)}};
    o["wheels"] = QJsonObject{{"lift", toArr(lift.data(), 3)}, {"gamma", toArr(gamma.data(), 3)},
                              {"gain", toArr(gain.data(), 3)}, {"offset", toArr(offset.data(), 3)}};
    o["sharpen"] = QJsonObject{{"amount", sharpenAmount}, {"radius", sharpenRadius}, {"masking", sharpenMasking}};
    o["nr"] = QJsonObject{{"luma", nrLuma}, {"color", nrColor}};
    if (!lens.isEmpty()) o["lens"] = lens;
    o["crop"] = QJsonObject{{"x", cropX}, {"y", cropY}, {"w", cropW}, {"h", cropH}, {"angle", cropAngle},
                            {"aspect", cropAspect}};
    o["auto"] = QJsonObject{{"strength", autoStrength}};
    o["enabled"] = QJsonObject{{"basic", enabled.basic}, {"wheels", enabled.wheels}, {"curve", enabled.curve},
                               {"hsl", enabled.hsl},     {"detail", enabled.detail}, {"lens", enabled.lens},
                               {"crop", enabled.crop}};
    return o;
}

Params Params::fromJson(const QJsonObject& o) {
    Params p;
    p.version = o.value("version").toInt(1);
    p.processVersion = o.value("processVersion").toInt(1);
    const QJsonObject wb = o.value("wb").toObject();
    p.wbMode = wb.value("mode").toString("as_shot");
    p.temp = num(wb, "temp", 5500);
    p.tint = num(wb, "tint", 0);
    p.exposure = num(o, "exposure", 0);
    p.contrast = num(o, "contrast", 0);
    p.highlights = num(o, "highlights", 0);
    p.shadows = num(o, "shadows", 0);
    p.whites = num(o, "whites", 0);
    p.blacks = num(o, "blacks", 0);
    p.saturation = num(o, "saturation", 0);
    p.vibrance = num(o, "vibrance", 0);
    if (o.contains("curve")) {
        const QJsonObject c = o.value("curve").toObject();
        const char* keys[4] = {"Y", "R", "G", "B"};
        for (int i = 0; i < 4; ++i)
            if (c.contains(keys[i])) p.curve[i] = curveFromJson(c.value(keys[i]));
    }
    const QJsonObject h = o.value("hsl").toObject();
    fromArr(h.value("hue"), p.hslHue.data(), kHslBands);
    fromArr(h.value("sat"), p.hslSat.data(), kHslBands);
    fromArr(h.value("lum"), p.hslLum.data(), kHslBands);
    const QJsonObject w = o.value("wheels").toObject();
    fromArr(w.value("lift"), p.lift.data(), 3);
    fromArr(w.value("gamma"), p.gamma.data(), 3);
    fromArr(w.value("gain"), p.gain.data(), 3);
    fromArr(w.value("offset"), p.offset.data(), 3);
    const QJsonObject s = o.value("sharpen").toObject();
    p.sharpenAmount = num(s, "amount", 40);
    p.sharpenRadius = num(s, "radius", 1.0f);
    p.sharpenMasking = num(s, "masking", 0);
    const QJsonObject nr = o.value("nr").toObject();
    p.nrLuma = num(nr, "luma", 0);
    p.nrColor = num(nr, "color", 25);
    p.lens = o.value("lens").toObject();
    const QJsonObject c = o.value("crop").toObject();
    p.cropX = num(c, "x", 0);
    p.cropY = num(c, "y", 0);
    p.cropW = num(c, "w", 1);
    p.cropH = num(c, "h", 1);
    p.cropAngle = num(c, "angle", 0);
    p.cropAspect = c.value("aspect").toVariant().toString();
    if (p.cropAspect.isEmpty()) p.cropAspect = "free";
    p.autoStrength = num(o.value("auto").toObject(), "strength", 100);
    // 古い .rdev には enabled が無い → すべてオン
    const QJsonObject e = o.value("enabled").toObject();
    p.enabled.basic = e.value("basic").toBool(true);
    p.enabled.wheels = e.value("wheels").toBool(true);
    p.enabled.curve = e.value("curve").toBool(true);
    p.enabled.hsl = e.value("hsl").toBool(true);
    p.enabled.detail = e.value("detail").toBool(true);
    p.enabled.lens = e.value("lens").toBool(true);
    p.enabled.crop = e.value("crop").toBool(true);
    return p;
}

void resetPanel(Params& p, Panel panel) {
    const Params d;
    switch (panel) {
        case Panel::Basic:
            p.wbMode = d.wbMode;
            p.temp = d.temp;
            p.tint = d.tint;
            p.exposure = p.contrast = p.highlights = p.shadows = p.whites = p.blacks = 0;
            p.saturation = p.vibrance = 0;
            break;
        case Panel::Wheels: p.lift = p.gamma = p.gain = p.offset = Wheel{}; break;
        case Panel::Curve: p.curve = d.curve; break;
        case Panel::Hsl: p.hslHue = p.hslSat = p.hslLum = {}; break;
        case Panel::Detail:
            p.sharpenAmount = d.sharpenAmount;
            p.sharpenRadius = d.sharpenRadius;
            p.sharpenMasking = d.sharpenMasking;
            p.nrLuma = d.nrLuma;
            p.nrColor = d.nrColor;
            break;
        case Panel::Lens: p.lens = QJsonObject{}; break;
        case Panel::Crop:
            p.cropX = p.cropY = 0;
            p.cropW = p.cropH = 1;
            p.cropAngle = 0;
            p.cropAspect = "free";
            break;
    }
}

bool& enabledFlag(Params& p, Panel panel) {
    switch (panel) {
        case Panel::Basic: return p.enabled.basic;
        case Panel::Wheels: return p.enabled.wheels;
        case Panel::Curve: return p.enabled.curve;
        case Panel::Hsl: return p.enabled.hsl;
        case Panel::Detail: return p.enabled.detail;
        case Panel::Lens: return p.enabled.lens;
        case Panel::Crop: return p.enabled.crop;
    }
    return p.enabled.basic;
}

const char* panelName(Panel panel) {
    switch (panel) {
        case Panel::Basic: return "基本補正";
        case Panel::Wheels: return "カラーホイール";
        case Panel::Curve: return "トーンカーブ";
        case Panel::Hsl: return "HSL";
        case Panel::Detail: return "ディテール";
        case Panel::Lens: return "レンズ補正";
        case Panel::Crop: return "切り抜きと角度";
    }
    return "";
}

}  // namespace rd
