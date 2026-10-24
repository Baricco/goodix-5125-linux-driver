#!/usr/bin/env python3
"""SIGFM ("SIFT Is Good For Matching") per il Goodix 27c6:5125.

Port fedele del matcher di goodix-fp-linux-dev/sigfm (cpp/match.cpp, cpp/compute.cpp),
pensato proprio per sensori 64x80. A differenza di un fit a omografia singola (RANSAC), qui la
verifica geometrica e' un VOTO A COPPIE invariante a rotazione e scala:

  1. SIFT -> ratio test di Lowe (0.75) -> match tentativi (coppie di keypoint).
  2. per ogni coppia di match, se la distanza tra i due keypoint e' conservata entro il 5%
     (LENGTH_MATCH), si registra un "angolo" (codifica sin/cos della rotazione relativa).
  3. per ogni coppia di angoli, se sin e cos coincidono entro il 5% (ANGLE_MATCH), +1 al punteggio.

Lo score e' il conteggio combinatorio di coppie mutuamente coerenti: cresce molto per lo stesso
dito (centinaia-milioni) e resta bassissimo (0-pochi) per dita diverse. I cicli O(n^2) sono
vettorizzati con numpy per restare veloci (<~decine di ms su 64x80).
"""

import numpy as np
import cv2

# costanti identiche a cpp/match.cpp
DISTANCE_MATCH = 0.75   # ratio test di Lowe
LENGTH_MATCH = 0.05     # tolleranza sulla conservazione della distanza tra keypoint
ANGLE_MATCH = 0.05      # tolleranza su sin/cos della rotazione relativa
MIN_MATCH = 5           # match tentativi minimi per procedere
EARLY_STOP = 2000       # per la verifica basta sapere se lo score supera la soglia: appena il
                        # conteggio raggiunge questo valore ci si ferma (evita l'esplosione O(m^2)
                        # sui frame quasi identici -> latenza uniforme). None = conteggio esatto.

_sift = cv2.SIFT_create()
_bf = cv2.BFMatcher_create()


def features(img_u8):
    """img_u8: (H,W) uint8 gia' preprocessata -> (keypoints Nx2 float32, descriptors)."""
    kp, desc = _sift.detectAndCompute(img_u8, None)
    pts = np.array([k.pt for k in kp], dtype=np.float64) if kp else np.empty((0, 2))
    return pts, desc


def _good_matches(desc_a, pts_a, desc_b, pts_b):
    """ratio test -> (P1, P2): coppie di punti corrispondenti, deduplicate."""
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
    P1 = np.rint(np.array(p1)).astype(np.int64)   # cpp usa Point2i
    P2 = np.rint(np.array(p2)).astype(np.int64)
    # dedup come lo std::set del cpp (coppia (p1,p2) unica)
    _, uniq = np.unique(np.hstack([P1, P2]), axis=0, return_index=True)
    return P1[np.sort(uniq)], P2[np.sort(uniq)]


def match_score(fa, fb):
    """Punteggio SIGFM tra due set di feature (pts, desc). Intero >= 0."""
    pts_a, desc_a = fa
    pts_b, desc_b = fb
    P1, P2 = _good_matches(desc_a, pts_a, desc_b, pts_b)
    n = len(P1)
    if n < MIN_MATCH:
        return 0

    # --- passo 2: angoli dalle coppie di match coerenti in lunghezza ---
    j, k = np.triu_indices(n, k=1)
    v1 = (P1[j] - P1[k]).astype(np.float64)   # vettore in immagine 1
    v2 = (P2[j] - P2[k]).astype(np.float64)   # vettore in immagine 2
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
    sin_f = np.pi / 2 + np.arcsin(np.clip(dot / product, -1.0, 1.0))      # come cpp
    cos_f = np.arccos(np.clip(cross / product, -1.0, 1.0))

    # --- passo 3: conteggio coppie di angoli coerenti in rotazione ---
    # per riga j si confronta l'angolo j con tutti i k>j (vettorizzato); con EARLY_STOP ci si
    # ferma appena il conteggio e' abbastanza alto da superare qualunque soglia utile.
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
