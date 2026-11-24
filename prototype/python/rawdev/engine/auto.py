"""自動調整：スライダーの値を決めるだけ（結果は手で直せる）。

処理順：WB → 露出 → 白/黒レベル → ハイライト/シャドウ → コントラスト → 自然な彩度
計算は長辺 512px 程度の縮小画像で行う。
"""
from __future__ import annotations

import math

import numpy as np

from . import color, decode
from .pipeline import tone_lut, apply_lut
from .params import default_params

# 撮影時 WB から離れすぎない（夕焼けなどの雰囲気を消しすぎない）
MAX_MIRED_SHIFT = 60.0


def auto_wb(small: np.ndarray, as_shot: np.ndarray):
    x = small * as_shot
    lum = color.luminance(x)
    mx = x.max(axis=2)
    valid = (mx < 0.95 * mx.max()) & (lum > np.percentile(lum, 20))
    if valid.sum() < 200:
        valid = mx < 0.98
    g = color.encode(x[valid])
    gmax, gmin = g.max(axis=1), g.min(axis=1)
    sat = (gmax - gmin) / np.maximum(gmax, 1e-4)
    # 彩度の低い画素＝灰色らしい点（White Patch 的な明るめの点に重み）
    thr = np.percentile(sat, 12)
    gray = x[valid][sat <= thr]
    w = color.luminance(gray) ** 2 + 1e-4
    gray_mean = (gray * w[:, None]).sum(0) / w.sum()
    world = x[valid].mean(0)
    est = 0.7 * gray_mean + 0.3 * world
    est = np.maximum(est, 1e-5)
    corr = (1.0 / est)
    corr = corr / corr[1]
    if np.abs(np.log(corr)).max() < 0.04:
        return None  # 撮影時 WB で十分
    gains = corr * as_shot
    t, tint = color.fit_temp_tint(gains)
    t0, tint0 = color.fit_temp_tint(as_shot)
    m, m0 = 1e6 / t, 1e6 / t0
    m = float(np.clip(m, m0 - MAX_MIRED_SHIFT, m0 + MAX_MIRED_SHIFT))
    t = round(1e6 / m / 10) * 10
    tint = float(np.clip(tint, tint0 - 40, tint0 + 40))
    return float(t), float(round(tint))


def auto_params(lin: np.ndarray, as_shot: np.ndarray, strength: float = 100.0, base: dict | None = None) -> dict:
    """自動調整後のパラメータ（base の他の項目はそのまま）。"""
    p = dict(base) if base else default_params()
    p = {k: (v.copy() if isinstance(v, dict) else v) for k, v in p.items()}
    small = decode.downscale(lin, 512)
    k = strength / 100.0

    # 1) WB
    res = auto_wb(small, as_shot)
    t0, tint0 = color.fit_temp_tint(as_shot)
    if res is None or k <= 0:
        p["wb"] = {"mode": "as_shot", "temp": t0, "tint": tint0}
        gains = np.asarray(as_shot, np.float32)
    else:
        t, tint = res
        p["wb"] = {"mode": "custom", "temp": round(t0 + (t - t0) * k, -1), "tint": round(tint0 + (tint - tint0) * k)}
        gains = color.wb_gains(p["wb"]["temp"], p["wb"]["tint"])
    x = small * gains
    lum = color.luminance(x)

    # 2) 露出：中央重点で、輝度の中央値（対数）を中間グレー付近へ
    h, w = lum.shape
    yy, xx = np.mgrid[0:h, 0:w]
    cw = np.exp(-(((xx - w / 2) / (w * 0.45)) ** 2 + ((yy - h / 2) / (h * 0.45)) ** 2))
    ll = np.log2(np.maximum(lum, 1e-5))
    order = np.argsort(ll.ravel())
    cum = np.cumsum(cw.ravel()[order])
    med = ll.ravel()[order][np.searchsorted(cum, cum[-1] * 0.5)]
    target = math.log2(0.16)
    ev = target - med
    # 白飛びが 0.5% を超えない範囲に抑える
    p995 = np.percentile(lum, 99.5)
    ev_max = math.log2(1.6 / max(p995, 1e-5))
    ev = float(np.clip(min(ev, ev_max), -4.0, 4.0))
    p["exposure"] = round(ev * k, 2)

    # 3) 4) 現在の見た目で帯域ごとの量を見る
    probe = dict(p)
    for kk in ("contrast", "highlights", "shadows", "whites", "blacks"):
        probe[kk] = 0.0
    lut = tone_lut(probe)
    g = apply_lut(color.encode(x * 2.0 ** p["exposure"]), lut)
    gl = color.luminance(g)
    hi = float((gl > 0.88).mean())
    lo = float((gl < 0.08).mean())
    p_hi = float(np.percentile(gl, 99.9))
    p_lo = float(np.percentile(gl, 0.1))

    highlights = -min(100.0, hi * 600.0) if hi > 0.02 else 0.0
    shadows = min(80.0, lo * 400.0) if lo > 0.03 else 0.0
    whites = float(np.clip((0.985 - p_hi) * 200.0, -30, 25))
    blacks = float(np.clip((0.015 - p_lo) * 300.0, -25, 25))

    # 5) コントラスト：輝度のばらつきが目標に近づくように
    sd = float(gl.std())
    contrast = float(np.clip((0.21 - sd) * 300.0, -30, 40))

    mx = g.max(axis=2)
    mn = g.min(axis=2)
    sat_mean = float(((mx - mn) / np.maximum(mx, 1e-4)).mean())
    vibrance = float(np.clip((0.30 - sat_mean) * 120.0, 0, 30))

    p["highlights"] = round(highlights * k)
    p["shadows"] = round(shadows * k)
    p["whites"] = round(whites * k)
    p["blacks"] = round(blacks * k)
    p["contrast"] = round(contrast * k)
    p["vibrance"] = round(vibrance * k)
    p["auto"] = {"strength": strength}
    return p
