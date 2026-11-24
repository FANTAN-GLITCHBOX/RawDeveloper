"""★レーティング・カラーラベルを XMP で読み書きする。

- RAW:          <名前>.xmp サイドカー（設定でオン）
- JPEG / PNG:   ファイル内の XMP を直接書き換え（画像データは再圧縮しない）
- TIFF:         XMP タグがあれば書き換え、なければ <名前>.xmp サイドカーへ
書き込みは一時ファイルに書いてから入れ替えるので、途中で落ちても元ファイルは壊れない。
"""
from __future__ import annotations

import os
import re
import struct
import zlib

from ..engine.params import LABEL_XMP

XMP_HEADER = b"http://ns.adobe.com/xap/1.0/\x00"
XMP_NS = "http://ns.adobe.com/xap/1.0/"
MSPHOTO_NS = "http://ns.microsoft.com/photo/1.0/"

_XMP_LABEL_REV = {v.lower(): k for k, v in LABEL_XMP.items()}
# 他ソフトの日本語ラベル名にも対応
_XMP_LABEL_REV.update({"赤": "red", "黄": "yellow", "緑": "green", "青": "blue", "紫": "purple",
                       "レッド": "red", "イエロー": "yellow", "グリーン": "green", "ブルー": "blue", "パープル": "purple"})


def label_from_xmp(text: str | None) -> str | None:
    if not text:
        return None
    return _XMP_LABEL_REV.get(text.strip().lower())


def _percent(rating: int) -> int:
    return {0: 0, 1: 1, 2: 25, 3: 50, 4: 75, 5: 99}.get(int(rating), 0)


def new_packet(rating: int | None, label: str | None) -> str:
    attrs = []
    if rating is not None:
        attrs.append(f'xmp:Rating="{int(rating)}"')
        attrs.append(f'MicrosoftPhoto:Rating="{_percent(rating)}"')
    if label:
        attrs.append(f'xmp:Label="{LABEL_XMP.get(label, label)}"')
    a = "\n    ".join(attrs)
    return (
        '<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>\n'
        '<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="RawDeveloper">\n'
        ' <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">\n'
        f'  <rdf:Description rdf:about=""\n    xmlns:xmp="{XMP_NS}"\n    xmlns:MicrosoftPhoto="{MSPHOTO_NS}"\n    {a}/>\n'
        " </rdf:RDF>\n</x:xmpmeta>\n" + " " * 512 + '\n<?xpacket end="w"?>'
    )


def update_packet(xml: str, rating: int | None, label: str | None) -> str:
    """既存の XMP に ★ とラベルを上書き（他の情報は残す）。"""
    # 既存の値を消す（属性形式・要素形式の両方）
    for tag in ("xmp:Rating", "xmp:Label", "MicrosoftPhoto:Rating"):
        xml = re.sub(r"\s" + re.escape(tag) + r'\s*=\s*"[^"]*"', "", xml)
        xml = re.sub(r"<" + re.escape(tag) + r">[^<]*</" + re.escape(tag) + r">\s*", "", xml)
    m = re.search(r"<rdf:Description\b([^>]*?)(/?)>", xml)
    if not m:
        return new_packet(rating, label)
    attrs = m.group(1)
    add = ""
    if 'xmlns:xmp="' not in xml:
        add += f' xmlns:xmp="{XMP_NS}"'
    if rating is not None and 'xmlns:MicrosoftPhoto="' not in xml:
        add += f' xmlns:MicrosoftPhoto="{MSPHOTO_NS}"'
    if rating is not None:
        add += f' xmp:Rating="{int(rating)}" MicrosoftPhoto:Rating="{_percent(rating)}"'
    if label:
        add += f' xmp:Label="{LABEL_XMP.get(label, label)}"'
    new_tag = f"<rdf:Description{attrs}{add}{m.group(2)}>"
    return xml[: m.start()] + new_tag + xml[m.end():]


def parse_packet(xml: str) -> tuple[int | None, str | None]:
    rating = label = None
    m = re.search(r'xmp:Rating\s*=\s*"(-?\d)"', xml) or re.search(r"<xmp:Rating>\s*(-?\d)\s*<", xml)
    if m:
        rating = int(m.group(1))
    m = re.search(r'xmp:Label\s*=\s*"([^"]*)"', xml) or re.search(r"<xmp:Label>([^<]*)<", xml)
    if m:
        label = label_from_xmp(m.group(1))
    return rating, label


def _atomic_write(path: str, data: bytes):
    tmp = path + ".rdtmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    st = os.stat(path) if os.path.exists(path) else None
    os.replace(tmp, path)
    if st is not None:
        try:
            os.utime(path, (st.st_atime, st.st_mtime))  # 更新日時は元のまま（並び順が変わらないように）
        except OSError:
            pass


# ---------------------------------------------------------------- JPEG

def _jpeg_segments(data: bytes):
    if data[:2] != b"\xff\xd8":
        raise ValueError("JPEG ではありません")
    i = 2
    segs = []
    while i < len(data):
        if data[i] != 0xFF:
            raise ValueError("JPEG の構造が不正です")
        marker = data[i + 1]
        if marker == 0xDA:  # SOS 以降は画像データ
            segs.append((marker, None, i))
            break
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        ln = struct.unpack(">H", data[i + 2:i + 4])[0]
        segs.append((marker, data[i:i + 2 + ln], i))
        i += 2 + ln
    return segs


def jpeg_read_xmp(data: bytes) -> str | None:
    for marker, seg, _ in _jpeg_segments(data):
        if marker == 0xE1 and seg is not None and seg[4:4 + len(XMP_HEADER)] == XMP_HEADER:
            return seg[4 + len(XMP_HEADER):].decode("utf-8", "ignore")
    return None


def jpeg_with_xmp(data: bytes, rating, label) -> bytes:
    segs = _jpeg_segments(data)
    old = jpeg_read_xmp(data)
    xml = update_packet(old, rating, label) if old else new_packet(rating, label)
    payload = XMP_HEADER + xml.encode("utf-8")
    if len(payload) + 2 > 65535:
        xml = new_packet(rating, label)
        payload = XMP_HEADER + xml.encode("utf-8")
    seg_new = b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload
    out = bytearray(b"\xff\xd8")
    inserted = False
    sos_pos = None
    for marker, seg, pos in segs:
        if marker == 0xDA:
            sos_pos = pos
            break
        is_xmp = marker == 0xE1 and seg[4:4 + len(XMP_HEADER)] == XMP_HEADER
        if is_xmp:
            continue
        if not inserted and marker not in (0xE0, 0xE1):
            out += seg_new
            inserted = True
        out += seg
    if not inserted:
        out += seg_new
    out += data[sos_pos:]
    return bytes(out)


# ---------------------------------------------------------------- PNG

def _png_chunks(data: bytes):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("PNG ではありません")
    i = 8
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        yield typ, data[i:i + 12 + ln]
        i += 12 + ln


def png_read_xmp(data: bytes) -> str | None:
    for typ, ch in _png_chunks(data):
        if typ == b"iTXt" and ch[8:].startswith(b"XML:com.adobe.xmp\x00"):
            body = ch[8:-4]
            parts = body.split(b"\x00", 1)[1]
            # 圧縮フラグ, 圧縮方式, 言語タグ\0, 翻訳キーワード\0, テキスト
            comp = parts[0]
            rest = parts[2:]
            rest = rest.split(b"\x00", 2)[2]
            if comp:
                rest = zlib.decompress(rest)
            return rest.decode("utf-8", "ignore")
    return None


def _png_itxt(xml: str) -> bytes:
    body = b"XML:com.adobe.xmp\x00" + b"\x00\x00" + b"\x00" + b"\x00" + xml.encode("utf-8")
    return struct.pack(">I", len(body)) + b"iTXt" + body + struct.pack(">I", zlib.crc32(b"iTXt" + body) & 0xFFFFFFFF)


def png_with_xmp(data: bytes, rating, label) -> bytes:
    old = png_read_xmp(data)
    xml = update_packet(old, rating, label) if old else new_packet(rating, label)
    out = bytearray(data[:8])
    done = False
    for typ, ch in _png_chunks(data):
        if typ == b"iTXt" and ch[8:].startswith(b"XML:com.adobe.xmp\x00"):
            continue
        if typ == b"IDAT" and not done:
            out += _png_itxt(xml)
            done = True
        out += ch
    return bytes(out)


# ---------------------------------------------------------------- public

def sidecar_path(path: str) -> str:
    return os.path.splitext(path)[0] + ".xmp"


def read_sidecar(path: str) -> tuple[int | None, str | None]:
    sp = sidecar_path(path)
    if not os.path.exists(sp):
        return None, None
    try:
        return parse_packet(open(sp, encoding="utf-8", errors="ignore").read())
    except OSError:
        return None, None


def write_sidecar(path: str, rating, label):
    sp = sidecar_path(path)
    xml = None
    if os.path.exists(sp):
        try:
            xml = update_packet(open(sp, encoding="utf-8", errors="ignore").read(), rating, label)
        except OSError:
            xml = None
    if xml is None:
        xml = new_packet(rating, label)
    _atomic_write(sp, xml.encode("utf-8"))


def read_file_rating(path: str) -> tuple[int | None, str | None]:
    ext = os.path.splitext(path)[1].lower()
    try:
        if ext in (".jpg", ".jpeg"):
            with open(path, "rb") as f:
                x = jpeg_read_xmp(f.read(1024 * 1024))
            if x:
                return parse_packet(x)
        elif ext == ".png":
            with open(path, "rb") as f:
                x = png_read_xmp(f.read())
            if x:
                return parse_packet(x)
        elif ext in (".tif", ".tiff"):
            import tifffile
            with tifffile.TiffFile(path) as tif:
                tag = tif.pages[0].tags.get(700)
                if tag is not None:
                    v = tag.value
                    v = v.decode("utf-8", "ignore") if isinstance(v, (bytes, bytearray)) else str(v)
                    return parse_packet(v)
    except Exception:
        pass
    r, l = read_sidecar(path)
    if r is not None or l is not None:
        return r, l
    from ..engine.decode import read_embedded_rating
    r, l = read_embedded_rating(path)
    return r, label_from_xmp(l)


def write_file_rating(path: str, rating, label) -> str:
    """現像後ファイルに直接書き込む。戻り値は書いた先の説明。"""
    ext = os.path.splitext(path)[1].lower()
    if ext in (".jpg", ".jpeg"):
        data = open(path, "rb").read()
        _atomic_write(path, jpeg_with_xmp(data, rating, label))
        return "file"
    if ext == ".png":
        data = open(path, "rb").read()
        _atomic_write(path, png_with_xmp(data, rating, label))
        return "file"
    if ext in (".tif", ".tiff") and _tiff_write(path, rating, label):
        return "file"
    write_sidecar(path, rating, label)
    return "sidecar"


def _tiff_write(path: str, rating, label) -> bool:
    """TIFF の XMP タグ(700)があれば、コピー上で書き換えてから入れ替える。"""
    try:
        import shutil
        import tifffile
        tmp = path + ".rdtmp"
        shutil.copy2(path, tmp)
        ok = False
        with tifffile.TiffFile(tmp, mode="r+b") as tif:
            tag = tif.pages[0].tags.get(700)
            if tag is not None:
                old = tag.value
                old = old.decode("utf-8", "ignore") if isinstance(old, (bytes, bytearray)) else str(old)
                new = update_packet(old, rating, label).encode("utf-8")
                tag.overwrite(new)
                ok = True
        if ok:
            st = os.stat(path)
            os.replace(tmp, path)
            os.utime(path, (st.st_atime, st.st_mtime))
        else:
            os.remove(tmp)
        return ok
    except Exception:
        try:
            os.remove(path + ".rdtmp")
        except OSError:
            pass
        return False
