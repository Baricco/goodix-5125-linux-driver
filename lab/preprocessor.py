#!/usr/bin/env python3
"""Preprocessing of Goodix 27c6:5125 images (80x64, 12 bit) into an image ready for matching.

Chain: decoding -> baseline subtraction (clear frame) -> per-column normalization
(removes the sensor's fixed pattern) -> CLAHE -> contrast normalization.
"""

import numpy as np
import cv2

WIDTH, HEIGHT = 80, 64
SAMPLE_MAX = 4095


def decode(packed):
    """7680 packed bytes -> uint16 array (HEIGHT, WIDTH). Column-major order (grold's layout)."""
    n = WIDTH * HEIGHT
    out = np.zeros(n, dtype=np.uint16)
    for group in range(0, (n // 4) * 6, 6):
        c = packed[group:group + 6]
        vals = (((c[0] & 0xF) << 8) | c[1],
                (c[3] << 4) | (c[0] >> 4),
                ((c[5] & 0xF) << 8) | c[2],
                (c[4] << 4) | (c[5] >> 4))
        wb = (group // 6) * 4
        for item in range(4):
            wire = wb + item
            out[(wire % HEIGHT) * WIDTH + wire // HEIGHT] = vals[item]
    return out.reshape(HEIGHT, WIDTH)


def frame_from_plaintext(plain):
    return decode(plain[8:8 + 7680])


def preprocess(frame, baseline=None):
    """frame, baseline: (H,W) uint16 arrays. Returns a (H,W) uint8 image ready for matching."""
    img = frame.astype(np.float32)
    if baseline is not None:
        img = baseline.astype(np.float32) - img      # ridges = signal above the baseline
    # per-column normalization: removes the sensor's vertical fixed pattern
    img -= img.mean(axis=0, keepdims=True)
    # to 8 bit over a robust range (percentiles)
    lo, hi = np.percentile(img, 2), np.percentile(img, 98)
    if hi <= lo:
        hi = lo + 1
    img = np.clip((img - lo) * 255.0 / (hi - lo), 0, 255).astype(np.uint8)
    # local contrast equalization to bring out the ridges
    img = cv2.createCLAHE(clipLimit=2.5, tileGridSize=(4, 4)).apply(img)
    return img


def to_png(u8, path, scale=7):
    big = cv2.resize(u8, (WIDTH * scale, HEIGHT * scale), interpolation=cv2.INTER_LANCZOS4)
    cv2.imwrite(path, big)


if __name__ == "__main__":
    import sys
    plain = open(sys.argv[1], "rb").read()
    base = open(sys.argv[2], "rb").read() if len(sys.argv) > 2 else None
    frame = frame_from_plaintext(plain)
    baseline = frame_from_plaintext(base) if base else None
    out = preprocess(frame, baseline)
    to_png(out, sys.argv[3] if len(sys.argv) > 3 else "preprocessed.png")
    print("ok", out.shape, "range", int(out.min()), int(out.max()))
