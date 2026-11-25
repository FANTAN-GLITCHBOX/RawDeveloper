"""現像パイプライン本体。

  リニア画像 → レンズ補正 → WB → 露出 → (ガンマ) → トーン LUT（白黒/ハイライト/シャドウ/
  コントラスト/トーンカーブ）→ カラーホイール → 彩度・自然な彩度 → HSL → ノイズ低減・シャープ
  → 回転・切り抜き → 表示/書き出し用 sRGB(ガンマ) 0..1

トーン LUT は 4096 段の 1 次元表にまとめて一度で引くので速い。
"""
from __future__ import annotations

import math

import cv2
import numpy as np

from . import color, lens
from .params import HSL_CENTERS, is_default_curve

LUT_N = 4096
LUT_MAX = 2.0   # ガンマ空間で 2.0 まで（白飛び手前の情報を残す）


# ---------------------------------------------------------------- curves

def monotone_spline(points, xs: np.ndarray) -> np.ndarray:
    """Fritsch–Carlson の単調 3 次補間（トーンカーブ用）。"""
    pts = sorted([(float(x), float(y)) for x, y in points])
    px = np.array([p[0] for p in pts])
    py = np.array([p[1] for p in pts])
    if len(px) == 1:
        return np.full_like(xs, py[0])
    d = np.diff(py) / np.maximum(np.diff(px), 1e-9)
    m = np.empty(len(px))
    m[0], m[-1] = d[0], d[-1]
    for i in range(1, len(px) - 1):
        m[i] = 0.0 if d[i - 1] * d[i] <= 0 else (d[i - 1] + d[i]) / 2
    for i in range(len(d)):
        if d[i] == 0:
            m[i] = m[i + 1] = 0
        else:
            a, b = m[i] / d[i], m[i + 1] / d[i]
            s = a * a + b * b
            if s > 9:
                t = 3 / math.sqrt(s)
                m[i], m[i + 1] = t * a * d[i], t * b * d[i]
    xc = np.clip(xs, px[0], px[-1])
    idx = np.clip(np.searchsorted(px, xc) - 1, 0, len(px) - 2)
    x0, x1 = px[idx], px[idx + 1]
    hh = np.maximum(x1 - x0, 1e-9)
    t = (xc - x0) / hh
    t2, t3 = t * t, t * t * t
    h00 = 2 * t3 - 3 * t2 + 1
    h10 = t3 - 2 * t2 + t
    h01 = -2 * t3 + 3 * t2
    h11 = t3 - t2
    y = h00 * py[idx] + h10 * hh * m[idx] + h01 * py[idx + 1] + h11 * hh * m[idx + 1]
    # 端点の外側は水平
    y = np.where(xs < px[0], py[0], y)
    y = np.where(xs > px[-1], py[-1], y)
    return y


def _bump(x, c, w):
    return np.exp(-((x - c) / w) ** 2)


def tone_lut(p: dict) -> np.ndarray:
    """(4, LUT_N) の LUT：[共通, R, G, B]。入力はガンマ空間 0..LUT_MAX。"""
    x = np.linspace(0.0, LUT_MAX, LUT_N)
    # 1) 白レベル手前を柔らかく丸めるショルダー（露出を上げても階調が残る）
    knee = 0.80
    y = np.where(x < knee, x, knee + (1 - knee) * (1 - np.exp(-(x - knee) / (1 - knee))))
    # 2) RAW の素の状態は眠いので、カメラ JPEG 寄りの軽い S 字を基本に入れる
    y = y - 0.06 * np.sin(2 * np.pi * np.clip(y, 0, 1)) / (2 * np.pi) * 2
    # 3) 白レベル・黒レベル・ハイライト・シャドウ（帯域ごとの持ち上げ/押し下げ）
    y = y + p["blacks"] / 100.0 * 0.08 * _bump(y, 0.04, 0.10)
    y = y + p["shadows"] / 100.0 * 0.13 * _bump(y, 0.25, 0.17)
    y = y + p["highlights"] / 100.0 * 0.13 * _bump(y, 0.74, 0.17)
    y = y + p["whites"] / 100.0 * 0.08 * _bump(y, 0.96, 0.10)
    # 4) コントラスト（中間 0.5 を軸にした S 字）
    c = p["contrast"] / 100.0
    if c:
        yc = np.clip(y, 0, 1)
        y = y - c * 0.45 * np.sin(2 * np.pi * yc) / (2 * np.pi) * 2 * np.where((y >= 0) & (y <= 1), 1, 0)
    y = np.maximum.accumulate(np.clip(y, 0.0, 1.0))
    # 5) ユーザーのトーンカーブ
    curves = p["curve"]
    if not is_default_curve(curves["Y"]):
        y = monotone_spline(curves["Y"], y)
    out = np.empty((4, LUT_N), np.float32)
    out[0] = y
    for i, ch in enumerate("RGB"):
        out[i + 1] = y if is_default_curve(curves[ch]) else monotone_spline(curves[ch], y)
    return out


def apply_lut(g: np.ndarray, lut: np.ndarray) -> np.ndarray:
    idx = np.clip(g * ((LUT_N - 1) / LUT_MAX), 0, LUT_N - 1).astype(np.int32)
    out = np.empty_like(g)
    for c in range(3):
        out[..., c] = lut[c + 1][idx[..., c]]
    return out


# ---------------------------------------------------------------- color

def _wheel_rgb(w):
    """ホイールの (u, v, master) → RGB の差分（明るさを変えない色ベクトル + マスター）。"""
    u, v, m = w
    # u: 赤↔シアン方向、v: 緑/黄↔青/マゼンタ方向（色相環上の位置）
    r = u * 0.866 + v * 0.0
    g = -u * 0.433 + v * 0.75
    b = -u * 0.433 - v * 0.75
    vec = np.array([r, g, b], np.float32)
    vec -= (vec * color.LUMA).sum()
    return vec, float(m)


def apply_wheels(g: np.ndarray, wheels: dict) -> np.ndarray:
    if all(abs(x) < 1e-6 for w in wheels.values() for x in w):
        return g
    lift, lm = _wheel_rgb(wheels["lift"])
    gam, gm = _wheel_rgb(wheels["gamma"])
    gain, gnm = _wheel_rgb(wheels["gain"])
    off, om = _wheel_rgb(wheels["offset"])
    x = np.clip(g, 0, None)
    L = (lift + lm) * 0.25
    G = 1.0 + (gain + gnm) * 0.5
    x = x * G + L * (1.0 - np.clip(x, 0, 1))
    gexp = np.power(2.0, -(gam + gm) * 1.2).astype(np.float32)
    x = np.power(np.clip(x, 0, None), gexp)
    x = x + (off + om) * 0.2
    return x.astype(np.float32)


def apply_saturation(g: np.ndarray, sat: float, vib: float) -> np.ndarray:
    if not sat and not vib:
        return g
    Y = color.luminance(g)[..., None]
    chroma = g - Y
    f = 1.0 + sat / 100.0
    if vib:
        hsv = cv2.cvtColor(np.clip(g, 0, 1).astype(np.float32), cv2.COLOR_RGB2HSV)
        hue, s = hsv[..., 0], hsv[..., 1]
        # 肌色（オレンジ付近）は控えめに：色相 0..360 の表を引く
        hue_tab = np.exp(-((np.arange(361, dtype=np.float32) - 28) / 18) ** 2)
        skin = hue_tab[hue.astype(np.int32)] * np.minimum(s * 3, 1)
        vf = vib / 100.0 * (1 - s) * (1 - 0.6 * skin) * (1.2 if vib > 0 else 1.0)
        f = f + np.maximum(vf, -1)[..., None]
    return (Y + chroma * f).astype(np.float32)


def _band_table(adj) -> np.ndarray:
    """8 色の調整値 → 色相 0..360（0.5° 刻み）の表。"""
    deg = np.arange(721, dtype=np.float32) / 2.0
    centers = np.array(HSL_CENTERS + [360.0], np.float32)
    vals = np.array(list(adj) + [adj[0]], np.float32)
    return np.interp(deg, centers, vals).astype(np.float32)


def apply_hsl(g: np.ndarray, hsl: dict) -> np.ndarray:
    if not (any(hsl["hue"]) or any(hsl["sat"]) or any(hsl["lum"])):
        return g
    x = np.clip(g, 0, 1).astype(np.float32)
    hsv = cv2.cvtColor(x, cv2.COLOR_RGB2HSV)
    deg, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    idx = np.clip((deg * 2).astype(np.int32), 0, 720)
    sw = np.minimum(s * 2.0, 1.0)          # 無彩色には効かせない
    out = np.empty_like(hsv)
    if any(hsl["hue"]):
        out[..., 0] = (deg + _band_table(hsl["hue"])[idx] * (30.0 / 100.0) * sw) % 360.0
    else:
        out[..., 0] = deg
    out[..., 1] = np.clip(s * (1 + _band_table(hsl["sat"])[idx] / 100.0), 0, 1) if any(hsl["sat"]) else s
    out[..., 2] = np.clip(v * (1 + _band_table(hsl["lum"])[idx] * (0.6 / 100.0) * sw), 0, 1) if any(hsl["lum"]) else v
    return cv2.cvtColor(out, cv2.COLOR_HSV2RGB)


# ---------------------------------------------------------------- detail

def apply_detail(g: np.ndarray, sharpen: dict, nr: dict, scale: float) -> np.ndarray:
    out = g
    luma_nr = nr.get("luma", 0.0)
    color_nr = nr.get("color", 0.0)
    if color_nr > 0 or luma_nr > 0:
        ycc = cv2.cvtColor(np.clip(out, 0, 1).astype(np.float32), cv2.COLOR_RGB2YCrCb)
        if color_nr > 0:
            sig = max(0.3, color_nr / 100.0 * 6.0 * scale)
            for c in (1, 2):
                ycc[:, :, c] = cv2.GaussianBlur(ycc[:, :, c], (0, 0), sig)
        if luma_nr > 0:
            sc = luma_nr / 100.0 * 0.08
            ss = max(0.8, 2.5 * scale)
            y = ycc[:, :, 0]
            ycc[:, :, 0] = cv2.bilateralFilter(y, -1, sc, ss)
        out = cv2.cvtColor(ycc, cv2.COLOR_YCrCb2RGB)
    amt = sharpen.get("amount", 0.0)
    rad = sharpen.get("radius", 1.0) * scale
    if amt > 0 and rad >= 0.3:
        Y = color.luminance(out)
        blur = cv2.GaussianBlur(Y, (0, 0), rad)
        detail = Y - blur
        k = amt / 100.0 * 1.2
        mask_amt = sharpen.get("masking", 0.0) / 100.0
        if mask_amt > 0:
            gx = cv2.Sobel(blur, cv2.CV_32F, 1, 0)
            gy = cv2.Sobel(blur, cv2.CV_32F, 0, 1)
            mag = np.sqrt(gx * gx + gy * gy)
            thr = mask_amt * 0.25
            m = np.clip((mag - thr * 0.5) / max(thr, 1e-4), 0, 1)
            detail = detail * m
        out = out + (k * detail)[..., None]
    return out.astype(np.float32)


# ---------------------------------------------------------------- geometry

def rotated_rect_max_area(w, h, angle_rad):
    """回転後の画像に収まる、元と同じ縦横比の最大の矩形 (幅, 高さ)。"""
    if w <= 0 or h <= 0:
        return 0, 0
    width_is_longer = w >= h
    side_long, side_short = (w, h) if width_is_longer else (h, w)
    sin_a, cos_a = abs(math.sin(angle_rad)), abs(math.cos(angle_rad))
    if side_short <= 2.0 * sin_a * cos_a * side_long or abs(sin_a - cos_a) < 1e-10:
        x = 0.5 * side_short
        wr, hr = (x / sin_a, x / cos_a) if width_is_longer else (x / cos_a, x / sin_a)
    else:
        cos_2a = cos_a * cos_a - sin_a * sin_a
        wr, hr = (w * cos_a - h * sin_a) / cos_2a, (h * cos_a - w * sin_a) / cos_2a
    return wr, hr


def rotate(img: np.ndarray, angle_deg: float) -> np.ndarray:
    if abs(angle_deg) < 1e-3:
        return img
    h, w = img.shape[:2]
    M = cv2.getRotationMatrix2D(((w - 1) / 2, (h - 1) / 2), -angle_deg, 1.0)
    return cv2.warpAffine(img, M, (w, h), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)


def crop(img: np.ndarray, c: dict) -> np.ndarray:
    h, w = img.shape[:2]
    x0 = int(round(c["x"] * w))
    y0 = int(round(c["y"] * h))
    x1 = int(round((c["x"] + c["w"]) * w))
    y1 = int(round((c["y"] + c["h"]) * h))
    x0, x1 = max(0, x0), min(w, max(x0 + 1, x1))
    y0, y1 = max(0, y0), min(h, max(y0 + 1, y1))
    return img[y0:y1, x0:x1]


# ---------------------------------------------------------------- main

def wb_for(p: dict, as_shot: np.ndarray) -> np.ndarray:
    wb = p["wb"]
    if wb.get("mode") == "as_shot":
        return np.asarray(as_shot, np.float32)
    return color.wb_gains(wb["temp"], wb["tint"])


def develop(lin: np.ndarray, p: dict, as_shot: np.ndarray, lens_info: dict | None,
            scale: float = 1.0, detail: bool = True, do_crop: bool = True) -> np.ndarray:
    """リニア画像を現像して、ガンマ 0..1 の float32 RGB を返す。

    scale: 元画像に対するこの画像の縮小率（シャープ半径などの換算用）
    detail: False ならノイズ低減・シャープを省略（スライダー操作中の高速表示）
    do_crop: False なら回転のみ行い切り抜かない（切り抜きツール表示用）
    """
    x = lin
    x = lens.apply(x, p["lens"], lens_info)
    gains = wb_for(p, as_shot) * np.float32(2.0 ** p["exposure"])
    x = x * gains
    g = color.encode(x)
    g = apply_lut(g, tone_lut(p))
    g = apply_wheels(g, p["wheels"])
    g = apply_saturation(g, p["saturation"], p["vibrance"])
    g = apply_hsl(g, p["hsl"])
    g = np.clip(g, 0.0, 1.0)
    if detail:
        g = apply_detail(g, p["sharpen"], p["nr"], scale)
        g = np.clip(g, 0.0, 1.0)
    c = p["crop"]
    g = rotate(g, c.get("angle", 0.0))
    if do_crop and (c["w"] < 0.9999 or c["h"] < 0.9999 or c["x"] > 1e-4 or c["y"] > 1e-4):
        g = crop(g, c)
    return np.ascontiguousarray(g, dtype=np.float32)


def to_uint8(g: np.ndarray) -> np.ndarray:
    return (np.clip(g, 0, 1) * 255.0 + 0.5).astype(np.uint8)


def to_uint16(g: np.ndarray) -> np.ndarray:
    return (np.clip(g, 0, 1) * 65535.0 + 0.5).astype(np.uint16)


def output_srgb(g: np.ndarray) -> np.ndarray:
    """作業ガンマ(2.2) → sRGB 規格のトーン（書き出し時に厳密化）。"""
    return color.linear_to_srgb(color.decode(g))
