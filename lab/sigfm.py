#!/usr/bin/env python3
"""SIGFM ("SIFT Is Good For Matching") for the Goodix 27c6:5125.

Faithful port of the matcher of goodix-fp-linux-dev/sigfm (cpp/match.cpp, cpp/compute.cpp),
designed precisely for 64x80 sensors. Unlike a single-homography fit (RANSAC), here the
geometric verification is PAIRWISE VOTING invariant to rotation and scale:

  1. SIFT -> Lowe's ratio test (0.75) -> tentative matches (pairs of keypoints).
  2. for every pair of matches, if the distance between the two keypoints is preserved within 5%
     (LENGTH_MATCH), an "angle" is recorded (sin/cos encoding of the relative rotation).
  3. for every pair of angles, if sin and cos agree within 5% (ANGLE_MATCH), +1 to the score.

The score is the combinatorial count of mutually consistent pairs: it grows a lot for the same
finger (hundreds to millions) and stays very low (0 to a few) for different fingers. The O(n^2)
loops are vectorized with numpy to stay fast (<~tens of ms on 64x80).
"""

import numpy as np
import cv2

# constants identical to cpp/match.cpp
DISTANCE_MATCH = 0.75   # Lowe's ratio test
LENGTH_MATCH = 0.05     # tolerance on the preservation of the distance between keypoints
ANGLE_MATCH = 0.05      # tolerance on sin/cos of the relative rotation
MIN_MATCH = 5           # minimum tentative matches to go on
EARLY_STOP = 2000       # for verification it is enough to know whether the score exceeds the threshold: as soon as
                        # the count reaches this value it stops (avoids the O(m^2) explosion
                        # on nearly identical frames -> uniform latency). None = exact count.

_sift = cv2.SIFT_create()
_bf = cv2.BFMatcher_create()


def features(img_u8):
    """img_u8: (H,W) uint8 already preprocessed -> (keypoints Nx2 float32, descriptors)."""
    kp, desc = _sift.detectAndCompute(img_u8, None)
    pts = np.array([k.pt for k in kp], dtype=np.float64) if kp else np.empty((0, 2))
    return pts, desc


def _good_matches(desc_a, pts_a, desc_b, pts_b):
    """ratio test -> (P1, P2): pairs of corresponding points, deduplicated."""
    if desc_a is None or desc_b is None or len(desc_a) < MIN_MATCH or len(desc_b) < MIN_MATCH:
        return np.empty((0, 2)), np.empty((0, 2))
    pairs = _bf.knnMatch(desc_a, desc_b, k=2)
    p1, p2 = [], []
    for pr in pairs:
        if len(pr) == 2 and pr[0].distance < DISTANCE_MATCH * pr[1].distance:
            p1.append(pts_a[pr[0].queryIdx])
            p2.append(pts_b[pr[0].trainIdx])
    if len(p1) < MIN_MATCH:
        return np.array(p1), np.array(p2)
    P1 = np.rint(np.array(p1)).astype(np.int64)   # cpp uses Point2i
    P2 = np.rint(np.array(p2)).astype(np.int64)
    # dedup like the cpp's std::set (unique (p1,p2) pair)
    _, uniq = np.unique(np.hstack([P1, P2]), axis=0, return_index=True)
    return P1[np.sort(uniq)], P2[np.sort(uniq)]


def match_score(fa, fb):
    """SIGFM score between two feature sets (pts, desc). Integer >= 0."""
    pts_a, desc_a = fa
    pts_b, desc_b = fb
    P1, P2 = _good_matches(desc_a, pts_a, desc_b, pts_b)
    n = len(P1)
    if n < MIN_MATCH:
        return 0

    # --- step 2: angles from the match pairs consistent in length ---
    j, k = np.triu_indices(n, k=1)
    v1 = (P1[j] - P1[k]).astype(np.float64)   # vector in image 1
    v2 = (P2[j] - P2[k]).astype(np.float64)   # vector in image 2
    len1 = np.hypot(v1[:, 0], v1[:, 1])
    len2 = np.hypot(v2[:, 0], v2[:, 1])
    product = len1 * len2
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.minimum(len1, len2) / np.maximum(len1, len2)
    keep = (product > 0) & (1.0 - ratio <= LENGTH_MATCH)
    if keep.sum() < MIN_MATCH:
        return 0
    v1, v2, product = v1[keep], v2[keep], product[keep]
    dot = v1[:, 0] * v2[:, 0] + v1[:, 1] * v2[:, 1]
    cross = v1[:, 0] * v2[:, 1] - v1[:, 1] * v2[:, 0]
    sin_f = np.pi / 2 + np.arcsin(np.clip(dot / product, -1.0, 1.0))      # as in cpp
    cos_f = np.arccos(np.clip(cross / product, -1.0, 1.0))

    # --- step 3: count of angle pairs consistent in rotation ---
    # for row j angle j is compared with every k>j (vectorized); with EARLY_STOP it
    # stops as soon as the count is high enough to exceed any useful threshold.
    m = len(sin_f)
    count = 0
    for j in range(m - 1):
        sj, cj = sin_f[j], cos_f[j]
        sk, ck = sin_f[j + 1:], cos_f[j + 1:]
        with np.errstate(divide="ignore", invalid="ignore"):
            s_ok = 1.0 - np.minimum(sj, sk) / np.maximum(sj, sk) <= ANGLE_MATCH
            c_ok = 1.0 - np.minimum(cj, ck) / np.maximum(cj, ck) <= ANGLE_MATCH
        count += int(np.count_nonzero(s_ok & c_ok))
        if EARLY_STOP is not None and count >= EARLY_STOP:
            return count
    return count
