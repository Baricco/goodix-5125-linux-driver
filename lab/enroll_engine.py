#!/usr/bin/env python3
"""POINT-COVERAGE enrollment engine for the Goodix 27c6:5125 (the right model for touch
sensors, see docs/matching.md).

The sensor is small and touch-based: two presses overlap only if they land almost on the same
spot. A continuous mosaic cannot be stitched (the offsets between touches are ~0 or none). So the
template is a SET of distinct POINTS of the finger; verification accepts if the touch overlaps
at least one enrolled point (max SIGFM score). The more points are covered, the fewer "holes",
the fewer false rejections.

- EnrollSession: receives one frame at a time and decides whether it is a NEW point, a duplicate
  of a point already covered, or must be dropped (empty). Keeps the point map for the GUI.
- Verifier: a touch -> (accepted, score, point).
"""

import numpy as np

import sigfm
import coverage
from preprocessor import preprocess, decode

SAME_SPOT = 400       # SIGFM score above which two frames are "the same point" of the finger
ACCEPT = 300          # verification threshold (see template.THRESHOLD); to be tuned on real data


def _thumb(img_u8):
    return img_u8.copy()


class Spot:
    __slots__ = ("frames", "thumb", "count")

    def __init__(self, feats, thumb):
        self.frames = [feats]   # several feature sets of the same point (variety of pressure/moisture)
        self.thumb = thumb      # representative preprocessed image (for the GUI)
        self.count = 1


class EnrollSession:
    def __init__(self, clear_frame):
        self.clear = clear_frame
        self.spots = []

    def _feats(self, frame):
        img = preprocess(frame, self.clear)
        return img, sigfm.features(img)

    def best_spot(self, feats):
        """(index, score) of the most overlapping point, or (-1, 0)."""
        best, bi = 0, -1
        for i, sp in enumerate(self.spots):
            s = max(sigfm.match_score(f, feats) for f in sp.frames)
            if s > best:
                best, bi = s, i
        return bi, best

    def add_frame(self, frame):
        """-> dict with the outcome. Does not raise: the GUI shows reason and strength."""
        strength, ok = coverage.frame_quality(frame, self.clear)
        if not ok:
            return {"accepted": False, "reason": "empty", "strength": strength,
                    "coverage": len(self.spots)}
        img, feats = self._feats(frame)
        pts, desc = feats
        if desc is None or len(pts) < sigfm.MIN_MATCH:
            return {"accepted": False, "reason": "few features", "strength": strength,
                    "coverage": len(self.spots)}
        idx, score = self.best_spot(feats)
        if score >= SAME_SPOT:
            sp = self.spots[idx]
            if len(sp.frames) < 4:      # keep some variety per point, without bloating
                sp.frames.append(feats)
            sp.count += 1
            return {"accepted": True, "reason": "point reinforced", "new": False,
                    "spot": idx, "score": score, "strength": strength,
                    "coverage": len(self.spots), "thumb": img}
        self.spots.append(Spot(feats, _thumb(img)))
        return {"accepted": True, "reason": "new point", "new": True,
                "spot": len(self.spots) - 1, "score": score, "strength": strength,
                "coverage": len(self.spots), "thumb": img}

    def save(self, path):
        pts_list, desc_list, splits, spot_ids, thumbs = [], [], [], [], []
        for sid, sp in enumerate(self.spots):
            for (pts, desc) in sp.frames:
                pts_list.append(pts.astype(np.float32))
                desc_list.append(desc.astype(np.float32))
                splits.append(len(pts))
                spot_ids.append(sid)
            thumbs.append(sp.thumb)
        np.savez_compressed(
            path,
            pts=np.vstack(pts_list), desc=np.vstack(desc_list),
            splits=np.array(splits, np.int32), spot_ids=np.array(spot_ids, np.int32),
            thumbs=np.array(thumbs), clear=self.clear.astype(np.uint16))

    @classmethod
    def load(cls, path):
        """Rebuilds a session from a saved template (to continue/verify)."""
        z = np.load(path)
        s = cls(z["clear"].astype(np.uint16))
        pts, desc, splits, spot_ids, thumbs = (
            z["pts"], z["desc"], z["splits"], z["spot_ids"], z["thumbs"])
        off = 0
        per_spot = {}
        for n, sid in zip(splits, spot_ids):
            per_spot.setdefault(int(sid), []).append((pts[off:off + n], desc[off:off + n]))
            off += n
        for sid in sorted(per_spot):
            sp = Spot(per_spot[sid][0], thumbs[sid])
            sp.frames = per_spot[sid]
            s.spots.append(sp)
        return s


class Verifier:
    def __init__(self, path):
        z = np.load(path)
        pts, desc, splits = z["pts"], z["desc"], z["splits"]
        self.spot_ids = z["spot_ids"]
        self.frames, off = [], 0
        for n in splits:
            self.frames.append((pts[off:off + n], desc[off:off + n]))
            off += n
        self.clear = z["clear"] if "clear" in z else None

    def verify(self, frame=None, feats=None, threshold=ACCEPT):
        """-> (accepted, best_score, spot). Pass a raw frame (uses the saved clear frame) or feats."""
        if feats is None:
            feats = sigfm.features(preprocess(frame, self.clear))
        best, bspot = 0, -1
        for fr, sid in zip(self.frames, self.spot_ids):
            s = sigfm.match_score(fr, feats)
            if s > best:
                best, bspot = s, int(sid)
        return best >= threshold, best, bspot
