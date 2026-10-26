#!/usr/bin/env python3
"""Multi-frame template for recognition with SIGFM (Goodix 27c6:5125).

Model (the right one for small sensors, see docs/matching.md):
  - coverage ENROLL: the user touches N times moving the finger to cover different areas.
    Every frame is preprocessed (CLAHE) and its SIFT features are extracted and saved in the template.
  - VERIFY: a touch is compared with ALL template frames; the score is the MAX.
    A genuine finger overlaps at least one enrolled frame (huge score), an impostor none
    (score ~0). The threshold separates the two regimes.

Features are precomputed at enrollment: verification costs one preprocessing + N SIGFM
comparisons (each ~1-80 ms), so it is fast even with large templates.

The template works on the *_plain.bin files saved by the driver, so matching can be tested offline
without touching the hardware. The clear frame (baseline) is required for preprocessing.
"""

import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocessor import decode, preprocess
import sigfm

# Acceptance threshold (SIGFM score). To be tuned on the real coverage enrollment data set;
# with CLAHE preprocessing genuine touches with overlap are in the 10^3-10^6 range, impostors < ~50.
THRESHOLD = 300


def _frame(plain_path):
    return decode(open(plain_path, "rb").read()[8:8 + 7680])


def image_features(plain_path, clear_path):
    """plain_path -> SIGFM features after CLAHE preprocessing with the clear baseline."""
    img = preprocess(_frame(plain_path), _frame(clear_path))
    return sigfm.features(img)


def build_template(plain_paths, clear_path, out_path):
    """Builds and saves an .npz template from the enrollment captures."""
    pts_list, desc_list, splits = [], [], []
    for p in plain_paths:
        pts, desc = image_features(p, clear_path)
        if desc is None or len(pts) < sigfm.MIN_MATCH:
            print(f"  [skip] {os.path.basename(p)}: too few features ({len(pts)})")
            continue
        pts_list.append(pts.astype(np.float32))
        desc_list.append(desc.astype(np.float32))
        splits.append(len(pts))
        print(f"  [ok]   {os.path.basename(p)}: {len(pts)} keypoints")
    if not splits:
        raise SystemError("no valid frame in the template")
    np.savez_compressed(out_path,
                        pts=np.vstack(pts_list), desc=np.vstack(desc_list),
                        splits=np.array(splits, dtype=np.int32))
    print(f"template saved in {out_path} ({len(splits)} frames)")


def load_template(path):
    """-> list of (pts, desc) per frame."""
    z = np.load(path)
    pts, desc, splits = z["pts"], z["desc"], z["splits"]
    frames, off = [], 0
    for n in splits:
        frames.append((pts[off:off + n], desc[off:off + n]))
        off += n
    return frames


def verify(template_frames, probe_feats, threshold=THRESHOLD):
    """-> (accepted, best_score). best_score = max SIGFM over all template frames."""
    best = 0
    for tf in template_frames:
        best = max(best, sigfm.match_score(tf, probe_feats))
    return best >= threshold, best


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="SIGFM enroll/verify from *_plain.bin")
    ap.add_argument("--clear", required=True, help="baseline frame (clear-0_plain.bin)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("enroll", help="build a template from the captures")
    e.add_argument("--out", required=True)
    e.add_argument("plain", nargs="+", help="*_plain.bin files of the enrollment touches")
    v = sub.add_parser("verify", help="verify a touch against a template")
    v.add_argument("--template", required=True)
    v.add_argument("--threshold", type=int, default=THRESHOLD)
    v.add_argument("probe", help="*_plain.bin file of the touch to verify")
    args = ap.parse_args()

    if args.cmd == "enroll":
        build_template(args.plain, args.clear, args.out)
    else:
        frames = load_template(args.template)
        feats = image_features(args.probe, args.clear)
        ok, score = verify(frames, feats, args.threshold)
        print(f"score={score}  threshold={args.threshold}  ->  {'ACCEPTED' if ok else 'REJECTED'}")
        sys.exit(0 if ok else 1)
