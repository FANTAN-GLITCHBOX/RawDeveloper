"""レンズプロファイル補正（Lensfun データベース）と手動補正。

データ源の優先順：
  1. RAW 内蔵の補正データ … LibRaw がこれを外に出さないため、この試作では未対応
  2. Lensfun データベース（lensfunpy に同梱）
  3. ユーザーが追加した Lensfun XML（%APPDATA%/RawDeveloper/lenses/*.xml）
  4. 手動スライダー
"""
from __future__ import annotations

import glob
import os
import re
import threading

import numpy as np

try:
    import lensfunpy
    HAVE_LENSFUN = True
except Exception:  # pragma: no cover
    lensfunpy = None
    HAVE_LENSFUN = False


def user_lens_dir() -> str:
    base = os.environ.get("APPDATA") or os.path.expanduser("~/.config")
    d = os.path.join(base, "RawDeveloper", "lenses")
    os.makedirs(d, exist_ok=True)
    return d


class LensDB:
    _inst = None
    _lock = threading.Lock()

    def __init__(self):
        self.db = None
        self.error = None
        if not HAVE_LENSFUN:
            self.error = "lensfunpy がインストールされていません"
            return
        try:
            extra = glob.glob(os.path.join(user_lens_dir(), "*.xml"))
            if extra:
                xml = [open(p, encoding="utf-8").read() for p in extra]
                try:
                    self.db = lensfunpy.Database(xml=xml, load_common=True)
                except TypeError:
                    self.db = lensfunpy.Database()
            else:
                self.db = lensfunpy.Database()
        except Exception as e:  # pragma: no cover
            self.error = str(e)
        self._all = None

    @classmethod
    def get(cls) -> "LensDB":
        with cls._lock:
            if cls._inst is None:
                cls._inst = LensDB()
            return cls._inst

    @property
    def ok(self):
        return self.db is not None

    def all_lenses(self):
        if not self.ok:
            return []
        if self._all is None:
            self._all = sorted(self.db.lenses, key=lambda l: (l.maker.lower(), l.model.lower()))
        return self._all

    @staticmethod
    def key(lens) -> str:
        return f"{lens.maker}|{lens.model}"

    def by_key(self, key: str):
        for l in self.all_lenses():
            if self.key(l) == key:
                return l
        return None

    def find_camera(self, meta):
        if not self.ok or not meta.get("make"):
            return None
        make = meta.get("make", "")
        model = meta.get("model", "")
        try:
            cams = self.db.find_cameras(make, model, loose_search=True)
        except Exception:
            cams = []
        if not cams and model:
            # "Canon EOS R5" のようにメーカー名が重複しているケース
            m2 = re.sub("^" + re.escape(make.split()[0]) + r"\s*", "", model, flags=re.I)
            try:
                cams = self.db.find_cameras(make, m2, loose_search=True)
            except Exception:
                cams = []
        return cams[0] if cams else None

    @staticmethod
    def _normalize(name: str) -> str:
        s = name or ""
        s = s.replace("ƒ", "f").replace("F/", "f/")
        s = re.sub(r"\s+", " ", s).strip()
        return s

    def identify(self, meta: dict):
        """EXIF からレンズを特定。戻り値 (lens, camera, crop, confidence)。"""
        if not self.ok:
            return None, None, 1.0, 0.0
        cam = self.find_camera(meta)
        crop = cam.crop_factor if cam else 1.0
        if not cam and meta.get("focal") and meta.get("focal35"):
            try:
                crop = float(meta["focal35"]) / float(meta["focal"])
            except Exception:
                crop = 1.0
        name = self._normalize(meta.get("lens_model", ""))
        if not name:
            return None, cam, crop, 0.0
        maker = meta.get("lens_make") or None
        cands = []
        for loose in (False, True):
            try:
                cands = self.db.find_lenses(cam, maker, name, loose_search=loose)
            except Exception:
                cands = []
            if not cands and maker:
                try:
                    cands = self.db.find_lenses(cam, None, name, loose_search=loose)
                except Exception:
                    cands = []
            if cands:
                break
        if not cands:
            return None, cam, crop, 0.0
        # センサーサイズ（クロップ係数）が近いものを優先
        cands = sorted(cands, key=lambda l: (-l.score, abs((l.crop_factor or crop) - crop)))
        best = cands[0]
        conf = min(1.0, best.score / 100.0) if best.score else 0.5
        return best, cam, crop, conf


# ---------------------------------------------------------------- 補正マップ

_cache: dict = {}
_cache_lock = threading.Lock()


def _identity(w, h):
    xs, ys = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return xs, ys


def _manual_maps(w, h, k):
    """手動の歪曲補正（単純な放射方向モデル）。k: -100..100"""
    xs, ys = _identity(w, h)
    cx, cy = (w - 1) / 2, (h - 1) / 2
    r0 = np.hypot(cx, cy)
    nx, ny = (xs - cx) / r0, (ys - cy) / r0
    r2 = nx * nx + ny * ny
    a = -k / 100.0 * 0.15
    f = 1 + a * r2
    # 端が欠けないよう、四隅が収まるよう拡大率を合わせる
    s = 1.0 / (1 + a) if a < 0 else 1.0
    mx = cx + nx * f * s * r0
    my = cy + ny * f * s * r0
    return mx.astype(np.float32), my.astype(np.float32)


def build_maps(w: int, h: int, lp: dict, info: dict | None):
    """補正用データ：(vignette_gain (h,w,3) or None, maps [(mx,my)]*3 or None)"""
    key = (w, h, lp.get("profile"), lp.get("lens"), round(lp.get("distortion", 100)),
           round(lp.get("vignetting", 100)), lp.get("ca"), lp.get("autotrim"),
           round(lp.get("manual_distortion", 0)), round(lp.get("manual_vignetting", 0)),
           None if not info else (info.get("lens_key"), info.get("crop"), info.get("focal"), info.get("aperture")))
    with _cache_lock:
        if key in _cache:
            return _cache[key]

    vgain = None
    maps = None
    lens = None
    if lp.get("profile") and info and info.get("lens_key") and HAVE_LENSFUN:
        db = LensDB.get()
        lens = db.by_key(info["lens_key"])
    if lens is not None and info.get("focal"):
        focal = float(info["focal"])
        aperture = float(info.get("aperture") or 5.6)
        crop = float(info.get("crop") or 1.0)
        try:
            mod = lensfunpy.Modifier(lens, crop, w, h)
            scale = 0.0 if lp.get("autotrim", True) else 1.0
            mod.initialize(focal, aperture, 1000.0, scale=scale, pixel_format=np.float32)
            va = lp.get("vignetting", 100.0) / 100.0
            if va > 0:
                ones = np.ones((h, w, 3), np.float32)
                if mod.apply_color_modification(ones):
                    vgain = np.power(ones, va).astype(np.float32)
            da = lp.get("distortion", 100.0) / 100.0
            coords = None
            if lp.get("ca", True):
                coords = mod.apply_subpixel_geometry_distortion()
            if coords is None:
                c2 = mod.apply_geometry_distortion()
                if c2 is not None:
                    coords = np.repeat(c2[:, :, None, :], 3, axis=2)
            if coords is not None:
                xs, ys = _identity(w, h)
                maps = []
                for c in range(3):
                    mx = xs + da * (coords[:, :, c, 0] - xs)
                    my = ys + da * (coords[:, :, c, 1] - ys)
                    maps.append((mx.astype(np.float32), my.astype(np.float32)))
        except Exception:
            vgain, maps = None, None

    md = lp.get("manual_distortion", 0.0)
    if abs(md) > 0.5:
        mx2, my2 = _manual_maps(w, h, md)
        if maps is None:
            maps = [(mx2, my2)] * 3
        else:
            import cv2
            maps = [(cv2.remap(mx, mx2, my2, cv2.INTER_LINEAR), cv2.remap(my, mx2, my2, cv2.INTER_LINEAR))
                    for mx, my in maps]
    mv = lp.get("manual_vignetting", 0.0)
    if abs(mv) > 0.5:
        xs, ys = _identity(w, h)
        cx, cy = (w - 1) / 2, (h - 1) / 2
        r2 = ((xs - cx) ** 2 + (ys - cy) ** 2) / (cx * cx + cy * cy)
        g = (1 + mv / 100.0 * 1.2 * r2 ** 1.5).astype(np.float32)
        g = np.maximum(g, 0.05)[..., None]
        vgain = g if vgain is None else vgain * g

    res = (vgain, maps)
    with _cache_lock:
        if len(_cache) > 6:
            _cache.clear()
        _cache[key] = res
    return res


def apply(lin: np.ndarray, lp: dict, info: dict | None) -> np.ndarray:
    import cv2
    h, w = lin.shape[:2]
    vgain, maps = build_maps(w, h, lp, info)
    out = lin
    if vgain is not None:
        out = out * vgain
    if maps is not None:
        chans = [cv2.remap(np.ascontiguousarray(out[:, :, c]), maps[c][0], maps[c][1],
                           cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)
                 for c in range(3)]
        out = np.stack(chans, axis=-1)
    return out.astype(np.float32, copy=False)
