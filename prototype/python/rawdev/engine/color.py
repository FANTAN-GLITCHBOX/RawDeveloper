"""色の基本計算：ガンマ、色温度、ホワイトバランス。

作業色空間は「リニア sRGB（D65）」。RAW はデイライト WB で現像した状態を基準にし、
色温度スライダーはそこからの相対ゲインとして掛ける。
"""
from __future__ import annotations

import math
import numpy as np

# 表示・調整に使うガンマ（sRGB に近い単純なべき乗）
GAMMA = 2.2

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

# XYZ(D65) -> linear sRGB
XYZ_TO_SRGB = np.array([
    [3.2404542, -1.5371385, -0.4985314],
    [-0.9692660, 1.8760108, 0.0415560],
    [0.0556434, -0.2040259, 1.0572252],
], dtype=np.float64)

TEMP_MIN, TEMP_MAX = 2000.0, 50000.0
TINT_RANGE = 150.0
REF_TEMP = 6504.0


def encode(lin: np.ndarray) -> np.ndarray:
    """リニア -> ガンマ（負値は 0）。"""
    return np.power(np.maximum(lin, 0.0), 1.0 / GAMMA, dtype=np.float32)


def decode(g: np.ndarray) -> np.ndarray:
    return np.power(np.maximum(g, 0.0), GAMMA, dtype=np.float32)


def srgb_to_linear(v: np.ndarray) -> np.ndarray:
    v = v.astype(np.float32)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4).astype(np.float32)


def linear_to_srgb(v: np.ndarray) -> np.ndarray:
    v = np.clip(v, 0.0, 1.0).astype(np.float32)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(v, 1 / 2.4) - 0.055).astype(np.float32)


def illuminant_xy(temp: float) -> tuple[float, float]:
    """色温度 K から光源の xy 色度（4000K 以上は CIE 昼光軌跡、未満は黒体近似）。"""
    t = float(np.clip(temp, 1667.0, 50000.0))
    if t >= 4000.0:
        if t <= 7000.0:
            x = -4.6070e9 / t**3 + 2.9678e6 / t**2 + 0.09911e3 / t + 0.244063
        else:
            x = -2.0064e9 / t**3 + 1.9018e6 / t**2 + 0.24748e3 / t + 0.237040
        y = -3.000 * x * x + 2.870 * x - 0.275
    else:
        # Kim et al. の黒体軌跡近似
        x = -0.2661239e9 / t**3 - 0.2343589e6 / t**2 + 0.8776956e3 / t + 0.179910
        if t <= 2222.0:
            y = -1.1063814 * x**3 - 1.34811020 * x**2 + 2.18555832 * x - 0.20219683
        else:
            y = -0.9549476 * x**3 - 1.37418593 * x**2 + 2.09137015 * x - 0.16748867
    return x, y


def _illum_rgb(temp: float) -> np.ndarray:
    x, y = illuminant_xy(temp)
    xyz = np.array([x / y, 1.0, (1 - x - y) / y])
    rgb = XYZ_TO_SRGB @ xyz
    return np.maximum(rgb, 1e-4)


_REF = _illum_rgb(REF_TEMP)


def wb_gains(temp: float, tint: float) -> np.ndarray:
    """デイライト（≒D65）基準の画像に掛ける RGB ゲイン。temp の光で白いものを白に戻す。"""
    rgb = _illum_rgb(temp) / _REF
    g = 1.0 / rgb
    g = g / g[1]
    # 色かぶり：プラス＝マゼンタ（緑を減らす）。G=1 正規化のため R,B を持ち上げる
    t = math.exp(tint / 100.0 * 0.30)
    g[0] *= t
    g[2] *= t
    return g.astype(np.float32)


def fit_temp_tint(gains: np.ndarray) -> tuple[float, float]:
    """RGB ゲイン（G=1 正規化）に最も近い 色温度・色かぶり を探す。"""
    gains = np.asarray(gains, dtype=np.float64)
    gains = gains / gains[1]
    lr, lb = math.log(max(gains[0], 1e-4)), math.log(max(gains[2], 1e-4))
    best = (1e9, 5500.0, 0.0)
    # ミレッド等間隔で探索
    for mired in np.linspace(1e6 / 15000.0, 1e6 / 2000.0, 400):
        t = 1e6 / mired
        g0 = wb_gains(t, 0.0)
        dr = lr - math.log(g0[0])
        db = lb - math.log(g0[2])
        d = (dr + db) / 2.0
        err = (dr - d) ** 2 + (db - d) ** 2
        if err < best[0]:
            best = (err, t, d / 0.30 * 100.0)
    _, t, tint = best
    return float(round(t / 10) * 10), float(np.clip(round(tint), -TINT_RANGE, TINT_RANGE))


def temp_to_slider(temp: float) -> float:
    """色温度 -> 0..1（ミレッド軸で直感的な動きに）。"""
    m = 1e6 / np.clip(temp, TEMP_MIN, TEMP_MAX)
    m0, m1 = 1e6 / TEMP_MIN, 1e6 / TEMP_MAX
    return float((m0 - m) / (m0 - m1))


def slider_to_temp(s: float) -> float:
    m0, m1 = 1e6 / TEMP_MIN, 1e6 / TEMP_MAX
    m = m0 - float(np.clip(s, 0, 1)) * (m0 - m1)
    return float(1e6 / m)


def luminance(rgb: np.ndarray) -> np.ndarray:
    return (rgb[..., 0] * LUMA[0] + rgb[..., 1] * LUMA[1] + rgb[..., 2] * LUMA[2]).astype(np.float32)


def rgb_to_hsv(rgb: np.ndarray):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    d = mx - mn
    s = np.where(mx > 1e-6, d / np.maximum(mx, 1e-6), 0.0)
    dd = np.where(d > 1e-6, d, 1.0)
    h = np.where(mx == r, ((g - b) / dd) % 6.0,
                 np.where(mx == g, (b - r) / dd + 2.0, (r - g) / dd + 4.0))
    h = np.where(d > 1e-6, h / 6.0, 0.0)
    return h.astype(np.float32), s.astype(np.float32), mx.astype(np.float32)


def hsv_to_rgb(h, s, v):
    h6 = (h % 1.0) * 6.0
    i = np.floor(h6).astype(np.int32) % 6
    f = h6 - np.floor(h6)
    p = v * (1 - s)
    q = v * (1 - s * f)
    t = v * (1 - s * (1 - f))
    r = np.choose(i, [v, q, p, p, t, v])
    g = np.choose(i, [t, v, v, q, p, p])
    b = np.choose(i, [p, p, t, v, v, q])
    return np.stack([r, g, b], axis=-1).astype(np.float32)
