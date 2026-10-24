#!/usr/bin/env python3
"""Matcher stile SIGFM per il Goodix 27c6:5125.

Il sensore e' piccolo (80x64) e il matcher a minuzie standard di libfprint va male. Qui si usa
il matching per feature locali (SIFT + ratio test di Lowe + verifica geometrica RANSAC), lo stesso
principio del matcher SIGFM del driver open source. Fornisce enroll/verify per il prototipo.
"""

import os
import sys

import numpy as np
import cv2

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocessor import frame_from_plaintext, preprocess

_sift = cv2.SIFT_create(nfeatures=200, contrastThreshold=0.02, edgeThreshold=12)
_bf = cv2.BFMatcher(cv2.NORM_L2)


def features(plain, baseline=None):
    img = preprocess(frame_from_plaintext(plain),
                     frame_from_plaintext(baseline) if baseline else None)
    img = cv2.resize(img, (img.shape[1] * 4, img.shape[0] * 4), interpolation=cv2.INTER_CUBIC)
    kp, desc = _sift.detectAndCompute(img, None)
    return kp, desc


def score(a, b):
    """Punteggio di similarita' tra due (kp,desc): n. di corrispondenze geometricamente coerenti."""
    (ka, da), (kb, db) = a, b
    if da is None or db is None or len(ka) < 4 or len(kb) < 4:
        return 0
    good = []
    for pair in _bf.knnMatch(da, db, k=2):
        if len(pair) == 2 and pair[0].distance < 0.8 * pair[1].distance:
            good.append(pair[0])
    if len(good) < 4:
        return len(good)
    src = np.float32([ka[m.queryIdx].pt for m in good]).reshape(-1, 1, 2)
    dst = np.float32([kb[m.trainIdx].pt for m in good]).reshape(-1, 1, 2)
    _, mask = cv2.findHomography(src, dst, cv2.RANSAC, 5.0)
    return int(mask.sum()) if mask is not None else 0


THRESHOLD = 12  # corrispondenze coerenti minime per dichiarare "stesso dito" (da tarare sul banco)


def verify(template_feats, probe_feats):
    s = max(score(t, probe_feats) for t in template_feats)
    return s >= THRESHOLD, s


if __name__ == "__main__":
    import sys
    # self-test: immagine vs se stessa e vs versioni trasformate (deve combaciare),
    # vs rumore (non deve). Uso: match.py fingerprint_plain.bin [clear_plain.bin]
    plain = open(sys.argv[1], "rb").read()
    base = open(sys.argv[2], "rb").read() if len(sys.argv) > 2 else None
    f = features(plain, base)
    print("keypoint estratti:", len(f[0]))
    print("self-match:", score(f, f))

    img = preprocess(frame_from_plaintext(plain),
                     frame_from_plaintext(base) if base else None)
    img = cv2.resize(img, (img.shape[1] * 4, img.shape[0] * 4), interpolation=cv2.INTER_CUBIC)
    M = cv2.getRotationMatrix2D((img.shape[1] / 2, img.shape[0] / 2), 12, 1.0)
    rot = cv2.warpAffine(img, M, (img.shape[1], img.shape[0]))
    kr, dr = _sift.detectAndCompute(rot, None)
    print("match con rotazione 12 gradi:", score(f, (kr, dr)))

    rng = np.random.default_rng(0)
    noise = rng.integers(0, 255, img.shape, dtype=np.uint8)
    kn, dn = _sift.detectAndCompute(noise, None)
    print("match con rumore (atteso basso):", score(f, (kn, dn)))
