"""書き出し（JPEG / TIFF 16bit / PNG）。★・ラベルの埋め込み、元 RAW との紐付け。"""
from __future__ import annotations

import io
import os
from dataclasses import dataclass

import cv2
import numpy as np

from ..engine import decode, pipeline
from ..engine.params import merged
from . import xmp


@dataclass
class ExportOptions:
    folder: str = ""
    same_folder: bool = False
    subfolder: str = ""
    fmt: str = "jpeg"            # jpeg | tiff | png
    quality: int = 92
    long_edge: int = 0           # 0 = 元のサイズ
    name_template: str = "{name}"
    suffix: str = ""
    embed_rating: bool = True
    overwrite: str = "rename"    # rename | overwrite | skip
    add_to_library: bool = True


EXT = {"jpeg": ".jpg", "tiff": ".tif", "png": ".png"}


def _srgb_icc() -> bytes | None:
    try:
        from PIL import ImageCms
        return ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
    except Exception:
        return None


def out_path(src: str, opts: ExportOptions, index: int = 1) -> str:
    base = os.path.splitext(os.path.basename(src))[0]
    folder = os.path.dirname(src) if opts.same_folder else opts.folder
    if opts.subfolder:
        folder = os.path.join(folder, opts.subfolder)
    os.makedirs(folder, exist_ok=True)
    name = opts.name_template.replace("{name}", base).replace("{n}", f"{index:04d}") + opts.suffix
    p = os.path.join(folder, name + EXT[opts.fmt])
    if os.path.exists(p) and opts.overwrite == "rename":
        k = 2
        while os.path.exists(p):
            p = os.path.join(folder, f"{name}-{k}{EXT[opts.fmt]}")
            k += 1
    return p


def render_full(src: str, params: dict | None, lens_info: dict | None, cached=None) -> np.ndarray:
    """フル解像度で現像（ガンマ 0..1 float32）。"""
    s = cached if (cached is not None and cached.full) else decode.load(src, full=True)
    p = merged(params)
    return pipeline.develop(s.linear, p, s.as_shot_gains, lens_info, scale=1.0, detail=True)


def write_image(g: np.ndarray, path: str, opts: ExportOptions, rating: int | None, label: str | None,
                meta: dict | None = None):
    from PIL import Image
    if opts.long_edge and max(g.shape[:2]) > opts.long_edge:
        h, w = g.shape[:2]
        s = opts.long_edge / max(h, w)
        g = cv2.resize(g, (round(w * s), round(h * s)), interpolation=cv2.INTER_AREA)
    icc = _srgb_icc()
    tmp = path + ".rdtmp"
    r = rating if (opts.embed_rating and rating) else None
    lb = label if opts.embed_rating else None
    exif = Image.Exif()
    if r:
        exif[0x4746] = int(r)                                   # Rating（Windows が読む）
        exif[0x4749] = {1: 1, 2: 25, 3: 50, 4: 75, 5: 99}[int(r)]  # RatingPercent
    if meta:
        if meta.get("make"):
            exif[0x010F] = str(meta["make"])
        if meta.get("model"):
            exif[0x0110] = str(meta["model"])
    exif[0x0131] = "RawDeveloper"

    if opts.fmt == "jpeg":
        im = Image.fromarray(pipeline.to_uint8(g))
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=int(opts.quality), subsampling=0 if opts.quality >= 90 else 2,
                icc_profile=icc, exif=exif.tobytes(), optimize=True)
        data = buf.getvalue()
        if r is not None or lb:
            data = xmp.jpeg_with_xmp(data, r, lb)
        with open(tmp, "wb") as f:
            f.write(data)
    elif opts.fmt == "png":
        from PIL import PngImagePlugin
        im = Image.fromarray(pipeline.to_uint8(g))
        info = PngImagePlugin.PngInfo()
        if r is not None or lb:
            info.add_itxt("XML:com.adobe.xmp", xmp.new_packet(r, lb))
        im.save(tmp, "PNG", pnginfo=info, icc_profile=icc)
    else:
        import tifffile
        extratags = []
        if r is not None or lb:
            pkt = xmp.new_packet(r, lb).encode("utf-8")
            extratags.append((700, "B", len(pkt), pkt, True))
        kw = {}
        if icc:
            kw["iccprofile"] = icc
        tifffile.imwrite(tmp, pipeline.to_uint16(g), photometric="rgb", compression="zlib",
                         extratags=extratags, software="RawDeveloper", **kw)
    os.replace(tmp, path)
    return path
