#!/usr/bin/env python3
"""Qualita' di un frame e stima della sovrapposizione, per l'enroll guidato (GUI).

- frame_quality(): quanto un frame e' "impronta vera" e non bordo/sfondo. Usa il tensore di
  struttura: le regioni di creste hanno gradiente localmente coerente e orientato; bordo e sfondo
  sono piatti o incoerenti. Ritorna (q in 0..1, is_good). Serve a scartare al volo i tocchi
  fuori-centro (es. i frame di bordo A_07/A_12/A_13).
- estimate_offset(): roto-traslazione tra due frame dalle corrispondenze SIGFM, per piazzare il
  nuovo tocco nel mosaico di copertura.
"""

import numpy as np
import cv2

import sigfm

STRENGTH_REF = 400.0    # |d| (p90) tipico di un tocco pieno: fondo scala per la barra di forza
STRENGTH_MIN = 0.15     # sotto questa soglia il frame e' vuoto/fantasma (nessun dito) -> scarta


def frame_quality(frame_u16, clear_u16):
    """frame e clear grezzi (H,W uint16) -> (strength in 0..1, is_good).

    strength = ampiezza del segnale dito, dal grezzo d = clear - frame (p90 di |d| normalizzato).
    Serve a mostrare una barra di "forza del tocco" nella GUI e a scartare solo i frame vuoti
    (nessun dito). La bonta' vera del match la decide l'engine di copertura (matchabilita' SIGFM);
    i frame di bordo l'utente li vede a schermo e li salta."""
    d = np.abs(clear_u16.astype(np.float32) - frame_u16.astype(np.float32))
    strength = float(np.clip(np.percentile(d, 90) / STRENGTH_REF, 0.0, 1.0))
    return strength, strength >= STRENGTH_MIN


def estimate_affine(feats_ref, feats_new):
    """Roto-traslazione (2x3) che porta i punti di feats_new nel sistema di feats_ref, dalle
    corrispondenze SIGFM. -> (ok, M 2x3, n_inlier). M mappa new->ref."""
    pts_r, desc_r = feats_ref
    pts_n, desc_n = feats_new
    P_ref, P_new = sigfm._good_matches(desc_n, pts_n, desc_r, pts_r)  # new->ref
    if len(P_ref) < 3:
        return False, None, len(P_ref)
    M, inl = cv2.estimateAffinePartial2D(
        P_new.astype(np.float32), P_ref.astype(np.float32),
        method=cv2.RANSAC, ransacReprojThreshold=4.0)
    if M is None:
        return False, None, 0
    return True, M.astype(np.float32), int(inl.sum()) if inl is not None else 0


def estimate_offset(feats_ref, feats_new):
    """Come estimate_affine ma decomposto -> (ok, dx, dy, deg, n_inlier). Per diagnostica/mosaico."""
    ok, M, inl = estimate_affine(feats_ref, feats_new)
    if not ok:
        return False, 0.0, 0.0, 0.0, inl
    dx, dy = float(M[0, 2]), float(M[1, 2])
    deg = float(np.degrees(np.arctan2(M[1, 0], M[0, 0])))
    return True, dx, dy, deg, inl
