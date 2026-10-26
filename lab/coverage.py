#!/usr/bin/env python3
"""Frame quality and overlap estimate, for guided enrollment (GUI).

- frame_quality(): how much a frame is "real fingerprint" rather than edge/background. Uses the
  structure tensor: ridge regions have a locally coherent, oriented gradient; edge and background
  are flat or incoherent. Returns (q in 0..1, is_good). Used to drop off-centre touches on the
  fly (e.g. the edge frames A_07/A_12/A_13).
- estimate_offset(): rotation plus translation between two frames from the SIGFM
  correspondences, to place the new touch in the coverage mosaic.
"""

import numpy as np
import cv2

import sigfm

STRENGTH_REF = 400.0    # typical |d| (p90) of a full touch: full scale for the strength bar
STRENGTH_MIN = 0.15     # below this threshold the frame is empty/ghost (no finger) -> drop it


def frame_quality(frame_u16, clear_u16):
    """raw frame and clear frame (H,W uint16) -> (strength in 0..1, is_good).

    strength = amplitude of the finger signal, from the raw d = clear - frame (normalized p90 of |d|).
    Used to show a "touch strength" bar in the GUI and to drop only the empty frames
    (no finger). The real quality of a match is decided by the coverage engine (SIGFM
    matchability); edge frames are visible to the user on screen, who skips them."""
    d = np.abs(clear_u16.astype(np.float32) - frame_u16.astype(np.float32))
    strength = float(np.clip(np.percentile(d, 90) / STRENGTH_REF, 0.0, 1.0))
    return strength, strength >= STRENGTH_MIN


def estimate_affine(feats_ref, feats_new):
    """Rotation plus translation (2x3) taking the points of feats_new into the frame of feats_ref,
    from the SIGFM correspondences. -> (ok, M 2x3, n_inlier). M maps new->ref."""
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
    """Like estimate_affine but decomposed -> (ok, dx, dy, deg, n_inlier). For diagnostics/mosaic."""
    ok, M, inl = estimate_affine(feats_ref, feats_new)
    if not ok:
        return False, 0.0, 0.0, 0.0, inl
    dx, dy = float(M[0, 2]), float(M[1, 2])
    deg = float(np.degrees(np.arctan2(M[1, 0], M[0, 0])))
    return True, dx, dy, deg, inl
