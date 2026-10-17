#!/usr/bin/env python3
"""Preprocessing immagini Goodix 27c6:5125 (80x64, 12 bit) verso un'immagine pronta al matching.

Catena: decodifica -> sottrazione baseline (frame clear) -> normalizzazione per colonna
(rimuove il fixed-pattern del sensore) -> CLAHE -> normalizzazione del contrasto.
"""

import numpy as np
import cv2

WIDTH, HEIGHT = 80, 64
SAMPLE_MAX = 4095


def decode(packed):
    """7680 byte packed -> array uint16 (HEIGHT, WIDTH). Ordine colonna-major (schema grold)."""
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
    """frame, baseline: array (H,W) uint16. Restituisce uint8 (H,W) pronto al matching."""
    img = frame.astype(np.float32)
    if baseline is not None:
        img = baseline.astype(np.float32) - img      # creste = segnale sopra la baseline
    # normalizzazione per colonna: toglie il fixed-pattern verticale del sensore
    img -= img.mean(axis=0, keepdims=True)
    # a 8 bit su range robusto (percentili)
    lo, hi = np.percentile(img, 2), np.percentile(img, 98)
    if hi <= lo:
        hi = lo + 1
    img = np.clip((img - lo) * 255.0 / (hi - lo), 0, 255).astype(np.uint8)
    # equalizzazione locale del contrasto per far risaltare le creste
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
