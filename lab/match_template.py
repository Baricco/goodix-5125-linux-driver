#!/usr/bin/env python3
"""Enroll/verify con template multi-frame per il Goodix 27c6:5125.

Come Windows Hello: l'enroll combina molti tocchi (zone diverse del dito) in un template;
la verifica confronta la piccola zona letta contro OGNI frame del template e prende il migliore.

  match_template.py enroll <template.npz> <clear_plain.bin> <frame_plain.bin> [frame2 ...]
  match_template.py verify <template.npz> <clear_plain.bin> <probe_plain.bin>
"""
import os
import sys

import numpy as np
import cv2

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocessor import frame_from_plaintext, preprocess

_sift = cv2.SIFT_create(nfeatures=250, contrastThreshold=0.02, edgeThreshold=12)
_bf = cv2.BFMatcher(cv2.NORM_L2)
MATCH_THRESHOLD = 15   # inlier geometrici minimi; separa genuino-sovrapposto (35-97) da rumore (<=8)


def _features(plain_path, clear_path):
    clear = frame_from_plaintext(open(clear_path, "rb").read())
    img = preprocess(frame_from_plaintext(open(plain_path, "rb").read()), clear)
    img = cv2.resize(img, (320, 256), interpolation=cv2.INTER_CUBIC)
    kp, desc = _sift.detectAndCompute(img, None)
    pts = np.float32([k.pt for k in kp]) if kp else np.zeros((0, 2), np.float32)
    return pts, (desc if desc is not None else np.zeros((0, 128), np.float32))


def _match_one(pa, da, pb, db):
    """Inlier geometrici (affine RANSAC) tra due frame."""
    if len(da) < 2 or len(db) < 2:
        return 0
    good = [m for m, n in _bf.knnMatch(da, db, k=2) if m.distance < 0.75 * n.distance]
    if len(good) < 4:
        return 0
    src = np.float32([pa[m.queryIdx] for m in good]).reshape(-1, 1, 2)
    dst = np.float32([pb[m.trainIdx] for m in good]).reshape(-1, 1, 2)
    _, mask = cv2.estimateAffinePartial2D(src, dst, method=cv2.RANSAC,
                                          ransacReprojThreshold=6, maxIters=3000)
    return int(mask.sum()) if mask is not None else 0


def enroll(template_path, clear_path, frame_paths):
    data = {}
    for i, fp in enumerate(frame_paths):
        pts, desc = _features(fp, clear_path)
        data[f"pts_{i}"] = pts
        data[f"desc_{i}"] = desc
        print(f"frame {i}: {len(pts)} keypoint da {os.path.basename(fp)}")
    data["count"] = np.array([len(frame_paths)])
    np.savez(template_path, **data)
    print(f"template salvato: {template_path} ({len(frame_paths)} frame)")


def verify(template_path, clear_path, probe_path):
    t = np.load(template_path)
    n = int(t["count"][0])
    pp, pd = _features(probe_path, clear_path)
    scores = [_match_one(t[f"pts_{i}"], t[f"desc_{i}"], pp, pd) for i in range(n)]
    best = max(scores) if scores else 0
    print(f"probe {os.path.basename(probe_path)}: inlier per frame = {scores}")
    print(f"miglior match = {best} (soglia {MATCH_THRESHOLD}) -> "
          f"{'ACCETTATO (stesso dito)' if best >= MATCH_THRESHOLD else 'RIFIUTATO'}")
    return best


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "enroll":
        enroll(sys.argv[2], sys.argv[3], sys.argv[4:])
    elif cmd == "verify":
        verify(sys.argv[2], sys.argv[3], sys.argv[4])
