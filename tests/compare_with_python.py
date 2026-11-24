"""C++ エンジンと Python 試作版の現像結果を比べる（開発用）。

  python tests/compare_with_python.py build/rd_develop
同じリニア画像・同じパラメータで両方を現像し、8bit 換算の差を表示する。
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "prototype", "python"))
from rawdev.engine import pipeline, params as P, auto  # noqa: E402

tool = sys.argv[1]
rng = np.random.default_rng(1)
h, w = 400, 600
yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
lin = np.stack([0.02 + 0.6 * xx / w, 0.03 + 0.4 * yy / h, 0.25 - 0.2 * xx / w], -1)
lin[100:200, 100:250] = [0.5, 0.3, 0.2]       # 肌色っぽい
lin[250:350, 350:550] = [0.05, 0.12, 0.45]    # 青
lin[20:60, 400:580] = [0.9, 0.9, 0.85]        # 明るい部分
lin = (lin * (1 + rng.normal(0, 0.03, lin.shape))).astype(np.float32).clip(0, None)
as_shot = np.array([1.2, 1.0, 1.6], np.float32)

cases = {
    "既定": {},
    "トーン": {"exposure": 0.6, "contrast": 40, "highlights": -50, "shadows": 40, "whites": 20, "blacks": -15},
    "WB": {"wb": {"mode": "custom", "temp": 3800, "tint": 12}},
    "彩度": {"saturation": 25, "vibrance": 40},
    "カーブ": {"curve": {"Y": [[0, 0.05], [0.4, 0.35], [1, 1]], "R": [[0, 0], [0.5, 0.56], [1, 1]],
                       "G": [[0, 0], [1, 1]], "B": [[0, 0.04], [1, 0.95]]}},
    "HSL": {"hsl": {"hue": [10, -20, 0, 30, 0, -15, 0, 0], "sat": [20, 30, -40, 0, 0, 50, 0, 0],
                    "lum": [0, 20, 0, 0, 0, -30, 0, 0]}},
    "ホイール": {"wheels": {"lift": [-0.3, -0.1, 0.05], "gamma": [0.1, 0.2, -0.1], "gain": [0.3, 0.2, 0.1],
                         "offset": [0, 0, 0.02]}},
    "シャープ・色ノイズ": {"sharpen": {"amount": 80, "radius": 2.0, "masking": 30}, "nr": {"luma": 0, "color": 50}},
    "回転・切り抜き": {"crop": {"x": 0.1, "y": 0.1, "w": 0.7, "h": 0.8, "angle": 4, "aspect": "free"}},
}

tmp = tempfile.mkdtemp()
fin = os.path.join(tmp, "in.f32")
with open(fin, "wb") as f:
    f.write(np.array([w, h], np.int32).tobytes())
    f.write(np.ascontiguousarray(lin).tobytes())


def run_cpp(prm, auto_out=None, lens=None):
    pj = os.path.join(tmp, "p.json")
    fo = os.path.join(tmp, "out.f32")
    json.dump(prm, open(pj, "w"))
    cmd = [tool, fin, pj, fo, *[str(v) for v in as_shot]]
    if auto_out:
        cmd += ["--auto", auto_out]
    if lens:
        cmd += ["--lens", *[str(v) for v in lens]]
    subprocess.run(cmd, check=True)
    d = np.fromfile(fo, np.uint8)
    ww, hh = np.frombuffer(d[:8].tobytes(), np.int32)
    return np.frombuffer(d[8:].tobytes(), np.float32).reshape(hh, ww, 3)


LENS = ("Sony|FE 24-70mm f/4 ZA OSS", 1.0, 24.0, 4.0)
cases["レンズ（プロファイル）"] = {"lens": {"profile": True, "lens": None, "distortion": 100, "vignetting": 100,
                                       "ca": True, "autotrim": True}}
cases["レンズ（手動）"] = {"lens": {"profile": False, "manual_distortion": 40, "manual_vignetting": 30}}

worst = 0.0
for name, delta in cases.items():
    prm = P.merged(delta)
    use_lens = name.startswith("レンズ")
    if not use_lens:
        prm["lens"]["profile"] = False
    info = {"lens_key": LENS[0], "crop": LENS[1], "focal": LENS[2], "aperture": LENS[3]} if use_lens else None
    py = pipeline.develop(lin, prm, as_shot, info, scale=0.5)
    cp = run_cpp(prm, lens=LENS if use_lens else None)
    if py.shape != cp.shape:
        print(f"{name}: サイズ不一致 {py.shape} {cp.shape}")
        worst = 99
        continue
    d = np.abs(py - cp) * 255
    worst = max(worst, float(np.percentile(d, 99.9)))
    print(f"{name:12s} 平均差 {d.mean():.3f}  99.9%点 {np.percentile(d, 99.9):.2f}  最大 {d.max():.2f}（8bit 換算）")

ao = os.path.join(tmp, "auto.json")
run_cpp(P.default_params(), ao)
ca = json.load(open(ao))
pa = auto.auto_params(lin, as_shot)
keys = ["exposure", "contrast", "highlights", "shadows", "whites", "blacks", "vibrance"]
print("自動調整 Python:", {k: pa[k] for k in keys}, pa["wb"])
print("自動調整 C++   :", {k: ca[k] for k in keys}, ca["wb"])
print("worst", round(worst, 2))
