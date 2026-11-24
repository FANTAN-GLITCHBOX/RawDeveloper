"""現像パラメータ（サイドカー .rdev に保存される内容）。"""
from __future__ import annotations

import copy

PROCESS_VERSION = 1

HSL_BANDS = ["レッド", "オレンジ", "イエロー", "グリーン", "アクア", "ブルー", "パープル", "マゼンタ"]
HSL_CENTERS = [0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 270.0, 300.0]

LABELS = ["red", "yellow", "green", "blue", "purple"]
LABEL_NAMES = {"red": "レッド", "yellow": "イエロー", "green": "グリーン", "blue": "ブルー", "purple": "パープル"}
LABEL_COLORS = {"red": "#e0443e", "yellow": "#e6c229", "green": "#3fae49", "blue": "#3b7dd8", "purple": "#9b59b6"}
# xmp:Label に書く名前（Lightroom / Bridge 互換）
LABEL_XMP = {"red": "Red", "yellow": "Yellow", "green": "Green", "blue": "Blue", "purple": "Purple"}


def default_params() -> dict:
    return {
        "version": 1,
        "processVersion": PROCESS_VERSION,
        "wb": {"mode": "as_shot", "temp": 5500.0, "tint": 0.0},
        "exposure": 0.0,
        "contrast": 0.0,
        "highlights": 0.0,
        "shadows": 0.0,
        "whites": 0.0,
        "blacks": 0.0,
        "saturation": 0.0,
        "vibrance": 0.0,
        "curve": {c: [[0.0, 0.0], [1.0, 1.0]] for c in ("Y", "R", "G", "B")},
        "hsl": {"hue": [0.0] * 8, "sat": [0.0] * 8, "lum": [0.0] * 8},
        # 各ホイール [u, v, master]（u,v は色の向き、master は明るさ）
        "wheels": {k: [0.0, 0.0, 0.0] for k in ("lift", "gamma", "gain", "offset")},
        "sharpen": {"amount": 40.0, "radius": 1.0, "masking": 0.0},
        "nr": {"luma": 0.0, "color": 25.0},
        "lens": {
            "profile": True,       # プロファイル補正を使う
            "lens": None,          # None=自動検出、文字列=手動選択（"メーカー|モデル"）
            "distortion": 100.0,   # 適用量 %
            "vignetting": 100.0,
            "ca": True,
            "autotrim": True,
            "manual_distortion": 0.0,
            "manual_vignetting": 0.0,
        },
        "crop": {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0, "angle": 0.0, "aspect": "free"},
        "auto": {"strength": 100.0},
    }


def merged(params: dict | None) -> dict:
    """欠けているキーを既定値で補う（古いサイドカーの読み込み用）。"""
    base = default_params()
    if not params:
        return base

    def rec(dst, src):
        for k, v in src.items():
            if k in dst and isinstance(dst[k], dict) and isinstance(v, dict):
                rec(dst[k], v)
            else:
                dst[k] = copy.deepcopy(v)

    rec(base, params)
    return base


# プリセット・コピー＆ペーストで扱うグループ
GROUPS = {
    "wb": ["wb"],
    "tone": ["exposure", "contrast", "highlights", "shadows", "whites", "blacks"],
    "presence": ["saturation", "vibrance"],
    "curve": ["curve"],
    "hsl": ["hsl"],
    "wheels": ["wheels"],
    "detail": ["sharpen", "nr"],
    "lens": ["lens"],
    "crop": ["crop"],
}
GROUP_NAMES = {
    "wb": "ホワイトバランス", "tone": "トーン（露出〜黒レベル）", "presence": "彩度",
    "curve": "トーンカーブ", "hsl": "HSL", "wheels": "カラーホイール",
    "detail": "シャープ・ノイズ低減", "lens": "レンズ補正", "crop": "切り抜き",
}


def extract_groups(params: dict, groups) -> dict:
    out = {}
    for g in groups:
        for k in GROUPS[g]:
            out[k] = copy.deepcopy(params[k])
    return out


def apply_groups(params: dict, partial: dict) -> dict:
    p = copy.deepcopy(params)
    for k, v in partial.items():
        p[k] = copy.deepcopy(v)
    return p


def is_default_curve(pts) -> bool:
    return len(pts) == 2 and pts[0] == [0.0, 0.0] and pts[1] == [1.0, 1.0]
