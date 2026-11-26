#include "core/Export.h"
#include "engine/Effects.h"

#include <tiffio.h>

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>

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
                       {"fmt", fmt},             {"quality", quality},        {"jpeg_level", jpegLevel},        {"long_edge", longEdge},
                       {"name_template", nameTemplate}, {"suffix", suffix},  {"embed_rating", embedRating},
                       {"overwrite", overwrite}, {"add_to_library", addToLibrary}, {"sharpen", sharpen},
                       {"sharpen_level", sharpenLevel}};
}

ExportOptions ExportOptions::fromJson(const QJsonObject& o) {
    ExportOptions e;
    e.folder = o.value("folder").toString();
    e.sameFolder = o.value("same_folder").toBool(false);
    e.subfolder = o.value("subfolder").toString();
    e.fmt = o.value("fmt").toString("jpeg");
    e.quality = o.value("quality").toInt(92);
    e.jpegLevel = o.value("jpeg_level").toString(jpegLevelFromQuality(e.quality));
    e.longEdge = o.value("long_edge").toInt(0);
    e.sharpen = o.value("sharpen").toString("none");
    e.sharpenLevel = o.value("sharpen_level").toInt(2);
    e.nameTemplate = o.value("name_template").toString("{name}");
    e.suffix = o.value("suffix").toString();
    e.embedRating = o.value("embed_rating").toBool(true);
    e.overwrite = o.value("overwrite").toString("rename");
    e.addToLibrary = o.value("add_to_library").toBool(true);
    return e;
}

JpegSetting jpegSetting(const QString& level, int customQuality) {
    if (level == "normal") return {85, false};
    if (level == "high") return {92, true};
    if (level == "max") return {98, true};
    const int q = std::clamp(customQuality, 1, 100);
    return {q, q >= 90};  // カスタム：90 以上は色を間引かない
}

QString jpegLevelFromQuality(int q) {
    if (q == 85) return "normal";
    if (q == 92) return "high";
    if (q == 98) return "max";
    return "custom";
}

namespace {
struct JErr {
    jpeg_error_mgr mgr;
    std::jmp_buf jb;
};
void jErrExit(j_common_ptr c) { std::longjmp(reinterpret_cast<JErr*>(c->err)->jb, 1); }
void jOutput(j_common_ptr) {}
}  // namespace

QByteArray encodeJpeg(const std::uint8_t* rgb, int w, int h, const JpegSetting& s, const QByteArray& icc) {
    if (!rgb || w <= 0 || h <= 0) return {};
    jpeg_compress_struct c{};
    JErr err{};
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jErrExit;
    err.mgr.output_message = jOutput;
    unsigned char* out = nullptr;
    unsigned long size = 0;
    if (setjmp(err.jb)) {
        jpeg_destroy_compress(&c);
        free(out);
        return {};
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &size);
    c.image_width = static_cast<JDIMENSION>(w);
    c.image_height = static_cast<JDIMENSION>(h);
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, std::clamp(s.quality, 1, 100), TRUE);
    c.optimize_coding = TRUE;
    // 色の間引き：4:2:0（横・縦とも半分）か 4:4:4（間引かない）
    const int f = s.chroma444 ? 1 : 2;
    c.comp_info[0].h_samp_factor = f;
    c.comp_info[0].v_samp_factor = f;
    c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
    c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
    // 品質 90 以上は浮動小数の DCT（わずかに正確）
    if (s.quality >= 90) c.dct_method = JDCT_FLOAT;
    jpeg_start_compress(&c, TRUE);
    if (!icc.isEmpty())
        jpeg_write_icc_profile(&c, reinterpret_cast<const JOCTET*>(icc.constData()), static_cast<unsigned>(icc.size()));
    const size_t stride = static_cast<size_t>(w) * 3;
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb + c.next_scanline * stride);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    QByteArray data(reinterpret_cast<const char*>(out), static_cast<qsizetype>(size));
    jpeg_destroy_compress(&c);
    free(out);
    return data;
}

qint64 estimateJpegBytes(const std::uint8_t* rgb, int w, int h, int outW, int outH, const JpegSetting& s) {
    if (!rgb || w <= 0 || h <= 0 || outW <= 0 || outH <= 0) return 0;
    const QByteArray d = encodeJpeg(rgb, w, h, s);
    if (d.isEmpty()) return 0;
    const double bpp = double(d.size() - 600) / (double(w) * h);  // ヘッダー分を除いた 1 画素あたり
    // 縮小画像は細部が詰まっていて画素あたりが重いので、大きく書き出すほど少し軽くする
    const double ratio = std::max(1.0, std::sqrt(double(outW) * outH / (double(w) * h)));
    // 高品質ほど細かいノイズまで残るので、大きくしても軽くなりにくい
    const double ex = std::clamp(0.45 - (s.quality - 85) * 0.0285, 0.05, 0.5);
    const double corr = std::pow(ratio, -ex);
    return static_cast<qint64>(bpp * corr * double(outW) * outH) + 600 + 30000;  // EXIF・ICC など
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
    if (o.sharpen != "none" && !o.sharpen.isEmpty()) {
        if (scaled.empty()) scaled = g0;
        outputSharpen(scaled, o.sharpen, o.sharpenLevel);  // 縮小したあとに（見る大きさに合わせて）
    }
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
        QByteArray data;
        if (o.fmt == "png") {
            QImage img(g.width, g.height, QImage::Format_RGB888);
            for (int y = 0; y < g.height; ++y) memcpy(img.scanLine(y), px.data() + static_cast<size_t>(y) * g.width * 3, g.width * 3);
            img.setColorSpace(QColorSpace(QColorSpace::SRgb));
            QBuffer buf;
            buf.open(QIODevice::WriteOnly);
            QImageWriter w(&buf, "png");
            if (!w.write(img)) return w.errorString();
            data = buf.data();
        } else {
            data = encodeJpeg(px.data(), g.width, g.height, jpegSetting(o), icc);
            if (data.isEmpty()) return QStringLiteral("JPEG に変換できません");
        }
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
