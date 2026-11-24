#include "core/Export.h"

#include <tiffio.h>

#include <algorithm>

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonDocument>

#include "core/Xmp.h"
#include "engine/Pipeline.h"
#include "engine/RawLoader.h"

namespace rd {

QJsonObject ExportOptions::toJson() const {
    return QJsonObject{{"folder", folder},       {"same_folder", sameFolder}, {"subfolder", subfolder},
                       {"fmt", fmt},             {"quality", quality},        {"long_edge", longEdge},
                       {"name_template", nameTemplate}, {"suffix", suffix},  {"embed_rating", embedRating},
                       {"overwrite", overwrite}, {"add_to_library", addToLibrary}};
}

ExportOptions ExportOptions::fromJson(const QJsonObject& o) {
    ExportOptions e;
    e.folder = o.value("folder").toString();
    e.sameFolder = o.value("same_folder").toBool(false);
    e.subfolder = o.value("subfolder").toString();
    e.fmt = o.value("fmt").toString("jpeg");
    e.quality = o.value("quality").toInt(92);
    e.longEdge = o.value("long_edge").toInt(0);
    e.nameTemplate = o.value("name_template").toString("{name}");
    e.suffix = o.value("suffix").toString();
    e.embedRating = o.value("embed_rating").toBool(true);
    e.overwrite = o.value("overwrite").toString("rename");
    e.addToLibrary = o.value("add_to_library").toBool(true);
    return e;
}

QString exportExtension(const QString& fmt) {
    if (fmt == "tiff") return ".tif";
    if (fmt == "png") return ".png";
    return ".jpg";
}

QString exportPath(const QString& src, const ExportOptions& o, int index) {
    const QFileInfo fi(src);
    QString folder = o.sameFolder ? fi.absolutePath() : o.folder;
    if (!o.subfolder.isEmpty()) folder += "/" + o.subfolder;
    QDir().mkpath(folder);
    QString name = o.nameTemplate;
    name.replace("{name}", fi.completeBaseName()).replace("{n}", QString("%1").arg(index, 4, 10, QChar('0')));
    name += o.suffix;
    const QString ext = exportExtension(o.fmt);
    QString p = folder + "/" + name + ext;
    if (QFileInfo::exists(p)) {
        if (o.overwrite == "skip") return {};
        if (o.overwrite == "rename") {
            for (int k = 2; QFileInfo::exists(p); ++k) p = folder + "/" + name + "-" + QString::number(k) + ext;
        }
    }
    return p;
}

namespace {

// 最小限の EXIF（メーカー・機種・ソフト名・★）。Windows のエクスプローラーが★を読む
QByteArray makeExif(int rating, const Metadata* meta) {
    struct Entry {
        quint16 tag, type;
        QByteArray data;  // ASCII は NUL 込み
        quint16 value = 0;
    };
    std::vector<Entry> es;
    auto ascii = [&](quint16 tag, const std::string& s) {
        if (!s.empty()) es.push_back({tag, 2, QByteArray::fromStdString(s) + '\0'});
    };
    if (meta) {
        ascii(0x010F, meta->make);
        ascii(0x0110, meta->model);
    }
    ascii(0x0131, "RawDeveloper");
    if (rating > 0) {
        const int pct[6] = {0, 1, 25, 50, 75, 99};
        es.push_back({0x4746, 3, {}, quint16(rating)});
        es.push_back({0x4749, 3, {}, quint16(pct[std::min(rating, 5)])});
    }
    std::sort(es.begin(), es.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
    QByteArray t("II*\0", 4);
    auto u16 = [&](QByteArray& b, quint16 v) { b.append(char(v & 0xFF)).append(char(v >> 8)); };
    auto u32 = [&](QByteArray& b, quint32 v) {
        for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF));
    };
    u32(t, 8);
    const quint32 ifdSize = 2 + quint32(es.size()) * 12 + 4;
    quint32 dataOff = 8 + ifdSize;
    QByteArray extra;
    u16(t, quint16(es.size()));
    for (auto& e : es) {
        u16(t, e.tag);
        u16(t, e.type);
        if (e.type == 2) {
            u32(t, quint32(e.data.size()));
            if (e.data.size() <= 4) {
                QByteArray v = e.data.leftJustified(4, '\0');
                t += v;
            } else {
                u32(t, dataOff + quint32(extra.size()));
                extra += e.data;
                if (extra.size() % 2) extra += '\0';
            }
        } else {
            u32(t, 1);
            u16(t, e.value);
            u16(t, 0);
        }
    }
    u32(t, 0);
    t += extra;
    QByteArray seg = QByteArray("Exif\0\0", 6) + t;
    QByteArray out;
    out.append(char(0xFF)).append(char(0xE1));
    out.append(char((seg.size() + 2) >> 8)).append(char((seg.size() + 2) & 0xFF));
    return out + seg;
}

// APP0（JFIF）の直後に EXIF を差し込む
QByteArray insertExif(const QByteArray& jpg, const QByteArray& exif) {
    int pos = 2;
    if (jpg.size() > 4 && uchar(jpg[2]) == 0xFF && uchar(jpg[3]) == 0xE0) pos = 4 + ((uchar(jpg[4]) << 8) | uchar(jpg[5]));
    return jpg.left(pos) + exif + jpg.mid(pos);
}

QString writeTiff(const ImageF& g, const QString& path, const QByteArray& xmp, const QByteArray& icc) {
    const auto px = toRgb16(g);
#ifdef _WIN32
    TIFF* t = TIFFOpenW(reinterpret_cast<const wchar_t*>(path.utf16()), "w");
#else
    TIFF* t = TIFFOpen(QFile::encodeName(path).constData(), "w");
#endif
    if (!t) return QStringLiteral("TIFF を作れません");
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, uint32_t(g.width));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, uint32_t(g.height));
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(t, 0));
    TIFFSetField(t, TIFFTAG_SOFTWARE, "RawDeveloper");
    if (!icc.isEmpty()) TIFFSetField(t, TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.constData());
    if (!xmp.isEmpty()) TIFFSetField(t, TIFFTAG_XMLPACKET, uint32_t(xmp.size()), xmp.constData());
    std::vector<uint16_t> row(static_cast<size_t>(g.width) * 3);
    for (int y = 0; y < g.height; ++y) {
        std::copy(px.begin() + static_cast<size_t>(y) * g.width * 3, px.begin() + static_cast<size_t>(y + 1) * g.width * 3,
                  row.begin());
        if (TIFFWriteScanline(t, row.data(), uint32_t(y), 0) < 0) {
            TIFFClose(t);
            return QStringLiteral("TIFF の書き込みに失敗しました");
        }
    }
    TIFFClose(t);
    return {};
}

}  // namespace

QString writeExport(const ImageF& gIn, const QString& path, const ExportOptions& o, int rating, const QString& label,
                    const Metadata* meta) {
    const ImageF& g0 = gIn;
    ImageF scaled;
    if (o.longEdge > 0 && std::max(g0.width, g0.height) > o.longEdge) scaled = downscale(g0, o.longEdge);
    const ImageF& g = scaled.empty() ? g0 : scaled;
    const std::optional<int> r = (o.embedRating && rating > 0) ? std::optional<int>(rating) : std::nullopt;
    const QString lb = o.embedRating ? label : QString();
    const bool meta2 = r.has_value() || !lb.isEmpty();
    const QByteArray icc = QColorSpace(QColorSpace::SRgb).iccProfile();
    const QString tmp = path + ".rdtmp";

    if (o.fmt == "tiff") {
        const QByteArray x = meta2 ? xmp::newPacket(r, lb).toUtf8() : QByteArray();
        QString err = writeTiff(g, tmp, x, icc);
        if (!err.isEmpty()) return err;
    } else {
        const auto px = toRgb8(g);
        QImage img(g.width, g.height, QImage::Format_RGB888);
        for (int y = 0; y < g.height; ++y) memcpy(img.scanLine(y), px.data() + static_cast<size_t>(y) * g.width * 3, g.width * 3);
        img.setColorSpace(QColorSpace(QColorSpace::SRgb));
        QBuffer buf;
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, o.fmt == "png" ? "png" : "jpg");
        if (o.fmt != "png") {
            w.setQuality(o.quality);
            w.setOptimizedWrite(true);
        }
        if (!w.write(img)) return w.errorString();
        QByteArray data = buf.data();
        if (o.fmt == "png") {
            if (meta2) data = xmp::pngWithXmp(data, r, lb);
        } else {
            data = insertExif(data, makeExif(r.value_or(0), meta));
            if (meta2) data = xmp::jpegWithXmp(data, r, lb);
        }
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) return QStringLiteral("書き込めません：") + tmp;
        f.close();
    }
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) return QStringLiteral("名前を変更できません：") + path;
    return {};
}

}  // namespace rd
