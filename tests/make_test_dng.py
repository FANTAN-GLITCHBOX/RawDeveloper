"""CI 用のテスト DNG（6000x4000、Bayer RGGB、合成グラデーション）を作る。"""
import sys

import numpy as np
import tifffile

out = sys.argv[1] if len(sys.argv) > 1 else "test.dng"
h, w = 4000, 6000
yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
cam = np.stack([0.3 + 0.5 * xx / w, 0.3 + 0.3 * yy / h, 0.6 - 0.4 * xx / w], -1) * np.array([0.9, 1.0, 0.45]) * 0.5
cfa = np.zeros((h, w), np.float32)
cfa[0::2, 0::2] = cam[0::2, 0::2, 0]
cfa[0::2, 1::2] = cam[0::2, 1::2, 1]
cfa[1::2, 0::2] = cam[1::2, 0::2, 1]
cfa[1::2, 1::2] = cam[1::2, 1::2, 2]
data = (np.clip(cfa, 0, 1) * 4000 + 256).astype(np.uint16)
M = np.array([[3.2404542, -1.5371385, -0.4985314], [-0.9692660, 1.8760108, 0.0415560],
              [0.0556434, -0.2040259, 1.0572252]])


def srat(v):
    o = []
    for x in v.ravel():
        o += [int(round(x * 10000)), 10000]
    return o


tags = [
    (33421, "H", 2, (2, 2), True), (33422, "B", 4, (0, 1, 1, 2), True), (50706, "B", 4, (1, 4, 0, 0), True),
    (50708, "s", 0, "Test", True), (271, "s", 0, "Sony", True), (272, "s", 0, "ILCE-7M3", True),
    (50721, "2i", 9, srat(M), True), (50778, "H", 1, 21, True),
    (50728, "2I", 3, [9000, 10000, 10000, 10000, 4500, 10000], True), (50717, "I", 1, 4256, True),
    (50714, "H", 1, 256, True),
    # レンズ情報（レンズ補正のテスト用）
    (0xA434, "s", 0, "FE 24-70mm F4 ZA OSS", True), (0x920A, "2I", 1, (24, 1), True),
    (0x829D, "2I", 1, (4, 1), True),
]
tifffile.imwrite(out, data, photometric=32803, extratags=tags)
print("wrote", out)
