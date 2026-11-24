#include "core/Xmp.h"

#include <tiffio.h>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <zlib.h>

#include <algorithm>

namespace rd::xmp {

namespace {

const QByteArray kXmpHeader = QByteArray("http://ns.adobe.com/xap/1.0/") + '\0';
const QString kXmpNs = "http://ns.adobe.com/xap/1.0/";
const QString kMsNs = "http://ns.microsoft.com/photo/1.0/";

int percent(int r) {
    switch (r) {
        case 1: return 1;
        case 2: return 25;
        case 3: return 50;
        case 4: return 75;
        case 5: return 99;
        default: return 0;
    }
}

quint32 be32(const char* p) {
    return (quint32(uchar(p[0])) << 24) | (quint32(uchar(p[1])) << 16) | (quint32(uchar(p[2])) << 8) | uchar(p[3]);
}
quint16 be16(const char* p) { return quint16((uchar(p[0]) << 8) | uchar(p[1])); }

QByteArray put32(quint32 v) {
    QByteArray b(4, 0);
    b[0] = char(v >> 24);
    b[1] = char(v >> 16);
    b[2] = char(v >> 8);
    b[3] = char(v);
    return b;
}

}  // namespace

QString labelFromXmp(const QString& t) {
    const QString s = t.trimmed().toLower();
    if (s == "red" || s == QStringLiteral("赤") || s == QStringLiteral("レッド")) return "red";
    if (s == "yellow" || s == QStringLiteral("黄") || s == QStringLiteral("イエロー")) return "yellow";
    if (s == "green" || s == QStringLiteral("緑") || s == QStringLiteral("グリーン")) return "green";
    if (s == "blue" || s == QStringLiteral("青") || s == QStringLiteral("ブルー")) return "blue";
    if (s == "purple" || s == QStringLiteral("紫") || s == QStringLiteral("パープル")) return "purple";
    return {};
}

QString labelToXmp(const QString& l) {
    if (l.isEmpty()) return {};
    return l.left(1).toUpper() + l.mid(1);
}

QString newPacket(std::optional<int> rating, const QString& label) {
    QStringList attrs;
    if (rating) {
        attrs << QString("xmp:Rating=\"%1\"").arg(*rating) << QString("MicrosoftPhoto:Rating=\"%1\"").arg(percent(*rating));
    }
    if (!label.isEmpty()) attrs << QString("xmp:Label=\"%1\"").arg(labelToXmp(label));
    return QString("<?xpacket begin=\"\uFEFF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                   "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"RawDeveloper\">\n"
                   " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                   "  <rdf:Description rdf:about=\"\"\n    xmlns:xmp=\"%1\"\n    xmlns:MicrosoftPhoto=\"%2\"\n    %3/>\n"
                   " </rdf:RDF>\n</x:xmpmeta>\n%4\n<?xpacket end=\"w\"?>")
        .arg(kXmpNs, kMsNs, attrs.join("\n    "), QString(512, ' '));
}

QString updatePacket(const QString& xmlIn, std::optional<int> rating, const QString& label) {
    QString xml = xmlIn;
    for (const char* tag : {"xmp:Rating", "xmp:Label", "MicrosoftPhoto:Rating"}) {
        const QString t = QRegularExpression::escape(tag);
        xml.remove(QRegularExpression("\\s" + t + "\\s*=\\s*\"[^\"]*\""));
        xml.remove(QRegularExpression("<" + t + ">[^<]*</" + t + ">\\s*"));
    }
    QRegularExpression re("<rdf:Description\\b([^>]*?)(/?)>");
    auto m = re.match(xml);
    if (!m.hasMatch()) return newPacket(rating, label);
    QString add;
    if (!xml.contains("xmlns:xmp=\"")) add += QString(" xmlns:xmp=\"%1\"").arg(kXmpNs);
    if (rating && !xml.contains("xmlns:MicrosoftPhoto=\"")) add += QString(" xmlns:MicrosoftPhoto=\"%1\"").arg(kMsNs);
    if (rating) add += QString(" xmp:Rating=\"%1\" MicrosoftPhoto:Rating=\"%2\"").arg(*rating).arg(percent(*rating));
    if (!label.isEmpty()) add += QString(" xmp:Label=\"%1\"").arg(labelToXmp(label));
    const QString tag = "<rdf:Description" + m.captured(1) + add + m.captured(2) + ">";
    return xml.left(m.capturedStart()) + tag + xml.mid(m.capturedEnd());
}

Rating parsePacket(const QString& xml) {
    Rating r;
    auto m = QRegularExpression("xmp:Rating\\s*=\\s*\"(-?\\d)\"").match(xml);
    if (!m.hasMatch()) m = QRegularExpression("<xmp:Rating>\\s*(-?\\d)\\s*<").match(xml);
    if (m.hasMatch()) r.rating = m.captured(1).toInt();
    m = QRegularExpression("xmp:Label\\s*=\\s*\"([^\"]*)\"").match(xml);
    if (!m.hasMatch()) m = QRegularExpression("<xmp:Label>([^<]*)<").match(xml);
    if (m.hasMatch()) r.label = labelFromXmp(m.captured(1));
    return r;
}

bool atomicWrite(const QString& path, const QByteArray& data, bool keepMtime) {
    const QFileInfo fi(path);
    const QDateTime mtime = fi.exists() ? fi.lastModified() : QDateTime();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(data);
    if (!f.commit()) return false;
    if (keepMtime && mtime.isValid()) {
        // 更新日時は元のまま（並び順が変わらないように）
        QFile g(path);
        if (g.open(QIODevice::ReadWrite)) g.setFileTime(mtime, QFileDevice::FileModificationTime);
    }
    return true;
}

// ---------------------------------------------------------------- JPEG

QByteArray jpegReadXmp(const QByteArray& d) {
    if (d.size() < 4 || uchar(d[0]) != 0xFF || uchar(d[1]) != 0xD8) return {};
    int i = 2;
    while (i + 4 <= d.size()) {
        if (uchar(d[i]) != 0xFF) return {};
        const int marker = uchar(d[i + 1]);
        if (marker == 0xDA) break;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const int len = be16(d.constData() + i + 2);
        if (marker == 0xE1 && d.mid(i + 4, kXmpHeader.size()) == kXmpHeader)
            return d.mid(i + 4 + kXmpHeader.size(), len - 2 - kXmpHeader.size());
        i += 2 + len;
    }
    return {};
}

QByteArray jpegWithXmp(const QByteArray& d, std::optional<int> rating, const QString& label) {
    if (d.size() < 4 || uchar(d[0]) != 0xFF || uchar(d[1]) != 0xD8) return {};
    const QByteArray old = jpegReadXmp(d);
    QString xml = old.isEmpty() ? newPacket(rating, label) : updatePacket(QString::fromUtf8(old), rating, label);
    QByteArray payload = kXmpHeader + xml.toUtf8();
    if (payload.size() + 2 > 65535) payload = kXmpHeader + newPacket(rating, label).toUtf8();
    QByteArray seg;
    seg.append(char(0xFF)).append(char(0xE1));
    seg.append(char((payload.size() + 2) >> 8)).append(char((payload.size() + 2) & 0xFF));
    seg.append(payload);
    QByteArray out("\xFF\xD8", 2);
    bool inserted = false;
    int i = 2;
    while (i + 4 <= d.size()) {
        const int marker = uchar(d[i + 1]);
        if (marker == 0xDA) break;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const int len = be16(d.constData() + i + 2);
        const bool isXmp = marker == 0xE1 && d.mid(i + 4, kXmpHeader.size()) == kXmpHeader;
        if (!isXmp) {
            if (!inserted && marker != 0xE0 && marker != 0xE1) {
                out += seg;
                inserted = true;
            }
            out += d.mid(i, 2 + len);
        }
        i += 2 + len;
    }
    if (!inserted) out += seg;
    out += d.mid(i);
    return out;
}

// EXIF の Rating（0x4746）・RatingPercent（0x4749）が IFD0 にあれば値を書き換える（大きさは変えない）
namespace {
void jpegPatchExifRating(QByteArray& d, int rating) {
    int i = 2;
    while (i + 4 <= d.size()) {
        const int marker = uchar(d[i + 1]);
        if (marker == 0xDA) return;
        const int len = be16(d.constData() + i + 2);
        if (marker == 0xE1 && d.mid(i + 4, 6) == QByteArray("Exif\0\0", 6)) {
            const int t = i + 10;  // TIFF ヘッダー
            const bool le = d.mid(t, 2) == "II";
            auto rd16 = [&](int o) {
                return le ? quint16(uchar(d[o]) | (uchar(d[o + 1]) << 8)) : quint16((uchar(d[o]) << 8) | uchar(d[o + 1]));
            };
            auto rd32 = [&](int o) {
                return le ? quint32(rd16(o)) | (quint32(rd16(o + 2)) << 16) : (quint32(rd16(o)) << 16) | rd16(o + 2);
            };
            auto wr16 = [&](int o, quint16 v) {
                if (le) { d[o] = char(v & 0xFF); d[o + 1] = char(v >> 8); }
                else { d[o] = char(v >> 8); d[o + 1] = char(v & 0xFF); }
            };
            const int ifd = t + int(rd32(t + 4));
            if (ifd + 2 > d.size()) return;
            const int n = rd16(ifd);
            const int pct[6] = {0, 1, 25, 50, 75, 99};
            for (int k = 0; k < n; ++k) {
                const int e = ifd + 2 + k * 12;
                if (e + 12 > d.size()) return;
                const quint16 tag = rd16(e), type = rd16(e + 2);
                if (type != 3) continue;
                if (tag == 0x4746) wr16(e + 8, quint16(std::clamp(rating, 0, 5)));
                if (tag == 0x4749) wr16(e + 8, quint16(pct[std::clamp(rating, 0, 5)]));
            }
            return;
        }
        i += 2 + len;
    }
}
}  // namespace

// ---------------------------------------------------------------- PNG

namespace {
const QByteArray kPngSig("\x89PNG\r\n\x1a\n", 8);
const QByteArray kItxtKey = QByteArray("XML:com.adobe.xmp") + '\0';

QByteArray itxtChunk(const QString& xml) {
    QByteArray body = kItxtKey + QByteArray(2, '\0') + '\0' + '\0' + xml.toUtf8();
    QByteArray typed = QByteArray("iTXt") + body;
    const quint32 crc = crc32(0, reinterpret_cast<const Bytef*>(typed.constData()), uInt(typed.size()));
    return put32(quint32(body.size())) + typed + put32(crc);
}
}  // namespace

QByteArray pngReadXmp(const QByteArray& d) {
    if (!d.startsWith(kPngSig)) return {};
    int i = 8;
    while (i + 12 <= d.size()) {
        const quint32 len = be32(d.constData() + i);
        const QByteArray type = d.mid(i + 4, 4);
        if (type == "iTXt" && d.mid(i + 8, kItxtKey.size()) == kItxtKey) {
            QByteArray body = d.mid(i + 8 + kItxtKey.size(), int(len) - kItxtKey.size());
            const bool comp = body.size() > 0 && body[0] != 0;
            body = body.mid(2);
            const int a = body.indexOf('\0');
            const int b = body.indexOf('\0', a + 1);
            QByteArray text = body.mid(b + 1);
            if (comp) text = qUncompress(put32(quint32(text.size() * 10)) + text);
            return text;
        }
        i += 12 + int(len);
    }
    return {};
}

QByteArray pngWithXmp(const QByteArray& d, std::optional<int> rating, const QString& label) {
    if (!d.startsWith(kPngSig)) return {};
    const QByteArray old = pngReadXmp(d);
    const QString xml = old.isEmpty() ? newPacket(rating, label) : updatePacket(QString::fromUtf8(old), rating, label);
    QByteArray out = kPngSig;
    bool done = false;
    int i = 8;
    while (i + 12 <= d.size()) {
        const quint32 len = be32(d.constData() + i);
        const QByteArray type = d.mid(i + 4, 4);
        const QByteArray chunk = d.mid(i, 12 + int(len));
        i += 12 + int(len);
        if (type == "iTXt" && chunk.mid(8, kItxtKey.size()) == kItxtKey) continue;
        if (type == "IDAT" && !done) {
            out += itxtChunk(xml);
            done = true;
        }
        out += chunk;
    }
    return out;
}

// ---------------------------------------------------------------- TIFF

namespace {
QByteArray tiffReadXmp(const QString& path) {
    TIFFSetWarningHandler(nullptr);
#ifdef _WIN32
    TIFF* t = TIFFOpenW(reinterpret_cast<const wchar_t*>(path.utf16()), "r");
#else
    TIFF* t = TIFFOpen(QFile::encodeName(path).constData(), "r");
#endif
    if (!t) return {};
    uint32_t n = 0;
    void* data = nullptr;
    QByteArray out;
    if (TIFFGetField(t, TIFFTAG_XMLPACKET, &n, &data) && data) out = QByteArray(static_cast<const char*>(data), int(n));
    TIFFClose(t);
    return out;
}

bool tiffWriteXmp(const QString& path, std::optional<int> rating, const QString& label) {
    const QByteArray old = tiffReadXmp(path);
    if (old.isEmpty()) return false;
    const QByteArray xml = updatePacket(QString::fromUtf8(old), rating, label).toUtf8();
    const QString tmp = path + ".rdtmp";
    QFile::remove(tmp);
    if (!QFile::copy(path, tmp)) return false;
#ifdef _WIN32
    TIFF* t = TIFFOpenW(reinterpret_cast<const wchar_t*>(tmp.utf16()), "r+");
#else
    TIFF* t = TIFFOpen(QFile::encodeName(tmp).constData(), "r+");
#endif
    bool ok = false;
    if (t) {
        ok = TIFFSetField(t, TIFFTAG_XMLPACKET, uint32_t(xml.size()), xml.constData()) && TIFFRewriteDirectory(t);
        TIFFClose(t);
    }
    if (!ok) {
        QFile::remove(tmp);
        return false;
    }
    const QDateTime mtime = QFileInfo(path).lastModified();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) return false;
    QFile g(path);
    if (g.open(QIODevice::ReadWrite)) g.setFileTime(mtime, QFileDevice::FileModificationTime);
    return true;
}
}  // namespace

// ---------------------------------------------------------------- public

QString sidecarPath(const QString& path) {
    const QFileInfo fi(path);
    return fi.path() + "/" + fi.completeBaseName() + ".xmp";
}

Rating readSidecar(const QString& path) {
    QFile f(sidecarPath(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return parsePacket(QString::fromUtf8(f.readAll()));
}

bool writeSidecar(const QString& path, std::optional<int> rating, const QString& label) {
    const QString sp = sidecarPath(path);
    QString xml;
    QFile f(sp);
    if (f.open(QIODevice::ReadOnly)) {
        xml = updatePacket(QString::fromUtf8(f.readAll()), rating, label);
        f.close();
    } else {
        xml = newPacket(rating, label);
    }
    return atomicWrite(sp, xml.toUtf8(), false);
}

Rating readEmbedded(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = f.read(4 * 1024 * 1024);
    Rating r;
    static const QRegularExpression re1("xmp:Rating\\s*=\\s*\"(-?\\d)\""), re2("<xmp:Rating>\\s*(-?\\d)\\s*</xmp:Rating>");
    static const QRegularExpression rl1("xmp:Label\\s*=\\s*\"([^\"]*)\""), rl2("<xmp:Label>([^<]*)</xmp:Label>");
    const QString s = QString::fromLatin1(head);
    auto m = re1.match(s);
    if (!m.hasMatch()) m = re2.match(s);
    if (m.hasMatch()) r.rating = m.captured(1).toInt();
    m = rl1.match(s);
    if (!m.hasMatch()) m = rl2.match(s);
    if (m.hasMatch()) r.label = labelFromXmp(QString::fromUtf8(m.captured(1).toLatin1()));
    return r;
}

Rating readFileRating(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    QByteArray x;
    if (ext == "jpg" || ext == "jpeg") {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) x = jpegReadXmp(f.read(1024 * 1024));
    } else if (ext == "png") {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) x = pngReadXmp(f.readAll());
    } else if (ext == "tif" || ext == "tiff") {
        x = tiffReadXmp(path);
    }
    if (!x.isEmpty()) return parsePacket(QString::fromUtf8(x));
    Rating r = readSidecar(path);
    if (r.rating || !r.label.isEmpty()) return r;
    return readEmbedded(path);
}

QString writeFileRating(const QString& path, std::optional<int> rating, const QString& label, QString* error) {
    const QString ext = QFileInfo(path).suffix().toLower();
    auto fail = [&](const QString& e) {
        if (error) *error = e;
        return QString();
    };
    if (ext == "jpg" || ext == "jpeg" || ext == "png") {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("開けません"));
        const QByteArray d = f.readAll();
        f.close();
        QByteArray out = ext == "png" ? pngWithXmp(d, rating, label) : jpegWithXmp(d, rating, label);
        if (ext != "png" && !out.isEmpty() && rating) jpegPatchExifRating(out, *rating);
        if (out.isEmpty()) return fail(QStringLiteral("形式を読み取れません"));
        if (!atomicWrite(path, out)) return fail(QStringLiteral("書き込めません"));
        return "file";
    }
    if ((ext == "tif" || ext == "tiff") && tiffWriteXmp(path, rating, label)) return "file";
    if (!writeSidecar(path, rating, label)) return fail(QStringLiteral("サイドカーを書き込めません"));
    return "sidecar";
}

}  // namespace rd::xmp
