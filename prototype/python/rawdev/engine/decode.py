"""ファイルの読み込み（RAW は LibRaw / rawpy、JPEG・TIFF・PNG は Pillow / tifffile）。

出力はすべて「リニア sRGB・float32・(H, W, 3)」。
RAW はデイライト WB で現像し、撮影時 WB との差を as_shot_gains として持つ。
"""
from __future__ import annotations

import io
import os
import re
from dataclasses import dataclass, field

import numpy as np

from . import color

RAW_EXTS = {
    ".cr2", ".cr3", ".crw", ".nef", ".nrw", ".arw", ".srf", ".sr2", ".raf", ".orf", ".rw2",
    ".pef", ".dng", ".srw", ".x3f", ".3fr", ".iiq", ".erf", ".mos", ".mrw", ".kdc", ".dcr", ".rwl",
}
IMG_EXTS = {".jpg", ".jpeg", ".tif", ".tiff", ".png"}
ALL_EXTS = RAW_EXTS | IMG_EXTS


def is_raw(path: str) -> bool:
    return os.path.splitext(path)[1].lower() in RAW_EXTS


def is_supported(path: str) -> bool:
    return os.path.splitext(path)[1].lower() in ALL_EXTS


@dataclass
class Source:
    path: str
    kind: str                      # "raw" | "image"
    linear: np.ndarray             # (H, W, 3) float32 リニア sRGB
    as_shot_gains: np.ndarray      # デイライト基準からの撮影時 WB ゲイン
    meta: dict = field(default_factory=dict)
    full: bool = True              # False なら縮小（half size）読み込み

    @property
    def size(self):
        return self.linear.shape[1], self.linear.shape[0]


# ---------------------------------------------------------------- metadata

def _ratio(v):
    try:
        if hasattr(v, "num"):
            return float(v.num) / float(v.den) if v.den else 0.0
        return float(v)
    except Exception:
        return 0.0


def _exif_from_bytes(data: bytes) -> dict:
    try:
        import exifread
        tags = exifread.process_file(io.BytesIO(data), details=False)
    except Exception:
        return {}
    return _tags_to_meta(tags)


def _tags_to_meta(tags) -> dict:
    m = {}

    def g(*keys):
        for k in keys:
            if k in tags:
                return tags[k]
        return None

    v = g("Image Make")
    if v:
        m["make"] = str(v).strip()
    v = g("Image Model")
    if v:
        m["model"] = str(v).strip()
    v = g("EXIF LensModel", "MakerNote LensModel", "EXIF LensSpecification")
    if v and "LensSpecification" not in str(v.tag if hasattr(v, "tag") else ""):
        m["lens_model"] = str(v).strip()
    v = g("EXIF LensMake")
    if v:
        m["lens_make"] = str(v).strip()
    v = g("EXIF FocalLength")
    if v:
        m["focal"] = _ratio(v.values[0])
    v = g("EXIF FocalLengthIn35mmFilm")
    if v:
        try:
            m["focal35"] = float(v.values[0])
        except Exception:
            pass
    v = g("EXIF FNumber")
    if v:
        m["aperture"] = _ratio(v.values[0])
    v = g("EXIF ExposureTime")
    if v:
        m["shutter"] = _ratio(v.values[0])
    v = g("EXIF ISOSpeedRatings", "EXIF PhotographicSensitivity")
    if v:
        try:
            m["iso"] = int(v.values[0])
        except Exception:
            pass
    v = g("EXIF DateTimeOriginal", "Image DateTime")
    if v:
        m["datetime"] = str(v)
    v = g("Image Orientation")
    if v:
        try:
            m["orientation"] = int(v.values[0])
        except Exception:
            pass
    return m


_RATING_RES = [
    re.compile(rb'xmp:Rating\s*=\s*"(-?\d)"'),
    re.compile(rb"<xmp:Rating>\s*(-?\d)\s*</xmp:Rating>"),
]
_LABEL_RES = [
    re.compile(rb'xmp:Label\s*=\s*"([^"]*)"'),
    re.compile(rb"<xmp:Label>([^<]*)</xmp:Label>"),
]


def read_embedded_rating(path: str) -> tuple[int | None, str | None]:
    """ファイル先頭付近の XMP から ★ とラベルを拾う（カメラ内レーティング・他ソフトの評価）。"""
    try:
        with open(path, "rb") as f:
            data = f.read(4 * 1024 * 1024)
    except OSError:
        return None, None
    rating = label = None
    for r in _RATING_RES:
        mm = r.search(data)
        if mm:
            rating = int(mm.group(1))
            break
    for r in _LABEL_RES:
        mm = r.search(data)
        if mm:
            label = mm.group(1).decode("utf-8", "ignore")
            break
    return rating, label


def read_metadata(path: str, raw=None) -> dict:
    """EXIF（カメラ・レンズ・撮影条件）。RAW は exifread → 埋め込み JPEG → LibRaw の順で補う。"""
    meta: dict = {}
    try:
        import exifread
        with open(path, "rb") as f:
            tags = exifread.process_file(f, details=False)
        meta.update(_tags_to_meta(tags))
    except Exception:
        pass
    if raw is not None:
        if "lens_model" not in meta or "make" not in meta:
            try:
                th = raw.extract_thumb()
                import rawpy
                if th.format == rawpy.ThumbFormat.JPEG:
                    for k, v in _exif_from_bytes(th.data).items():
                        meta.setdefault(k, v)
            except Exception:
                pass
        try:
            ln = raw.lens
            if ln.model:
                meta.setdefault("lens_model", str(ln.model).strip())
            if ln.make:
                meta.setdefault("lens_make", str(ln.make).strip())
        except Exception:
            pass
        try:
            o = raw.other
            if o.focal_length:
                meta.setdefault("focal", float(o.focal_length))
            if o.aperture:
                meta.setdefault("aperture", float(o.aperture))
            if o.iso_speed:
                meta.setdefault("iso", int(o.iso_speed))
            if o.shutter_speed:
                meta.setdefault("shutter", float(o.shutter_speed))
        except Exception:
            pass
    return meta


def format_shutter(s: float | None) -> str:
    if not s:
        return ""
    if s >= 1:
        return f"{s:g}秒"
    return f"1/{round(1 / s)}"


def meta_line(meta: dict) -> str:
    parts = []
    if meta.get("iso"):
        parts.append(f"ISO {meta['iso']}")
    if meta.get("shutter"):
        parts.append(format_shutter(meta["shutter"]))
    if meta.get("aperture"):
        parts.append(f"F{meta['aperture']:.1f}")
    if meta.get("focal"):
        parts.append(f"{meta['focal']:.0f}mm")
    return "   ".join(parts)


# ---------------------------------------------------------------- decoding

def open_raw(path: str):
    """rawpy で開く。Windows の日本語パスでも開けるよう、ASCII 以外はメモリ経由で渡す。"""
    import rawpy
    if os.name == "nt" or not path.isascii():
        with open(path, "rb") as f:
            return rawpy.imread(io.BytesIO(f.read()))
    return rawpy.imread(path)


def _raw_postprocess(raw, half: bool, camera_wb: bool) -> np.ndarray:
    import rawpy
    rgb = raw.postprocess(
        half_size=half,
        use_camera_wb=camera_wb,
        use_auto_wb=False,
        output_color=rawpy.ColorSpace.sRGB,
        output_bps=16,
        gamma=(1, 1),
        no_auto_bright=True,
        highlight_mode=rawpy.HighlightMode.Blend,
        demosaic_algorithm=None if not half else None,
    )
    return rgb.astype(np.float32) / 65535.0


def _as_shot_from(day: np.ndarray, cam: np.ndarray) -> np.ndarray:
    ok = (day.max(axis=2) < 0.9) & (cam.max(axis=2) < 0.9) & (day.min(axis=2) > 0.01)
    if ok.sum() < 100:
        ok = np.ones(day.shape[:2], bool)
    d = day[ok].mean(axis=0) + 1e-6
    c = cam[ok].mean(axis=0) + 1e-6
    g = c / d
    return (g / g[1]).astype(np.float32)


def load(path: str, full: bool = True, as_shot_gains=None, meta=None) -> Source:
    """ファイルを読み込む。full=False は高速な縮小読み込み（プレビュー・サムネイル用）。"""
    if is_raw(path):
        with open_raw(path) as raw:
            if meta is None:
                meta = read_metadata(path, raw)
            day = _raw_postprocess(raw, half=not full, camera_wb=False)
            if as_shot_gains is None:
                if full:
                    # 撮影時 WB の推定は縮小画像で十分
                    small_day = _raw_postprocess(raw, half=True, camera_wb=False)
                else:
                    small_day = day
                small_cam = _raw_postprocess(raw, half=True, camera_wb=True)
                as_shot_gains = _as_shot_from(small_day, small_cam)
        meta.setdefault("width", day.shape[1] * (1 if full else 2))
        meta.setdefault("height", day.shape[0] * (1 if full else 2))
        return Source(path, "raw", day, np.asarray(as_shot_gains, np.float32), meta, full)

    # ---- 通常画像
    if meta is None:
        meta = read_metadata(path)
    ext = os.path.splitext(path)[1].lower()
    arr = None
    if ext in (".tif", ".tiff"):
        try:
            import tifffile
            a = tifffile.imread(path)
            if a.ndim == 2:
                a = np.stack([a] * 3, -1)
            a = a[..., :3]
            if a.dtype == np.uint16:
                arr = a.astype(np.float32) / 65535.0
            elif a.dtype == np.uint8:
                arr = a.astype(np.float32) / 255.0
            elif a.dtype.kind == "f":
                arr = np.clip(a.astype(np.float32), 0, None)
        except Exception:
            arr = None
    if arr is None:
        from PIL import Image, ImageOps
        im = Image.open(path)
        im = ImageOps.exif_transpose(im)
        if im.mode in ("I;16", "I;16B", "I"):
            a = np.asarray(im).astype(np.float32) / 65535.0
            arr = np.stack([a] * 3, -1)
        else:
            if not full:
                im.draft("RGB", (1600, 1600))
            im = im.convert("RGB")
            arr = np.asarray(im).astype(np.float32) / 255.0
    lin = color.srgb_to_linear(arr)
    if not full:
        import cv2
        h, w = lin.shape[:2]
        s = 1600 / max(h, w)
        if s < 1:
            lin = cv2.resize(lin, (int(w * s), int(h * s)), interpolation=cv2.INTER_AREA)
    meta.setdefault("width", arr.shape[1])
    meta.setdefault("height", arr.shape[0])
    return Source(path, "image", np.ascontiguousarray(lin), np.ones(3, np.float32), meta, full)


def downscale(lin: np.ndarray, long_edge: int) -> np.ndarray:
    import cv2
    h, w = lin.shape[:2]
    s = long_edge / max(h, w)
    if s >= 1:
        return lin
    return cv2.resize(lin, (max(1, int(round(w * s))), max(1, int(round(h * s)))), interpolation=cv2.INTER_AREA)


# ---------------------------------------------------------------- thumbnails

def thumbnail_rgb8(path: str, size: int = 320) -> np.ndarray | None:
    """サムネイル用の 8bit RGB。RAW は埋め込み JPEG を使うので速い。"""
    from PIL import Image, ImageOps
    try:
        if is_raw(path):
            import rawpy
            with open_raw(path) as raw:
                flip = raw.sizes.flip
                try:
                    th = raw.extract_thumb()
                except Exception:
                    th = None
                if th is not None and th.format == rawpy.ThumbFormat.JPEG:
                    im = Image.open(io.BytesIO(th.data))
                    im.draft("RGB", (size, size))
                    im = im.convert("RGB")
                elif th is not None:
                    im = Image.fromarray(th.data).convert("RGB")
                else:
                    rgb = raw.postprocess(half_size=True, use_camera_wb=True, output_bps=8)
                    im = Image.fromarray(rgb)
                    flip = 0
                im.thumbnail((size, size))
                # 埋め込み JPEG は回転されていないことがある
                if flip == 3:
                    im = im.rotate(180, expand=True)
                elif flip == 5:
                    im = im.rotate(90, expand=True)
                elif flip == 6:
                    im = im.rotate(-90, expand=True)
                return np.asarray(im)
        im = Image.open(path)
        im.draft("RGB", (size, size))
        im = ImageOps.exif_transpose(im)
        im = im.convert("RGB")
        im.thumbnail((size, size))
        return np.asarray(im)
    except Exception:
        return None
