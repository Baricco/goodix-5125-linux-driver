#!/usr/bin/env python3
"""INCREMENTAL MOSAIC enrollment for the Goodix 5125.

Unlike the point model (enroll_engine), here a single map is built by stitching the scans as
they come. For every incoming frame registration (rotation plus translation) is attempted on the
recent/nearby keyframes of the map:
  - it locks (inliers >= REG_MIN) and has moved enough -> it EXTENDS the map (new keyframe);
  - it locks but has barely moved -> already covered;
  - it does not lock -> CONTACT LOST (the user jumped): the map is not extended and the user is
    told to go back. This way the map stays connected and the holes emerge from the coverage.

It provides the live composite (stitched image), the coverage map and the holes (mosaic.holes),
and a verification that uses the SIGFM score (the one that gave 0 false positives) plus the
registration inliers as a geometric signal.
"""

import numpy as np
import cv2

import sigfm
import coverage
import mosaic as mz
from preprocessor import preprocess

W, H = 80, 64
CANVAS = 520
REG_MIN = 8          # minimum inliers to lock a frame onto the map
SPACING = 9          # minimum px of displacement to add a keyframe (mosaic density)
RECENT = 10          # how many recent keyframes to try before searching the whole map
VERIFY_MIN = 300     # SIGFM acceptance threshold (= enroll_engine.ACCEPT)


class MosaicSession:
    def __init__(self, clear):
        self.clear = clear
        self.keyframes = []     # {feats, thumb, pose(2x3 new->world), center(xy world)}
        self.acc = np.zeros((CANVAS, CANVAS), np.float32)
        self.cnt = np.zeros((CANVAS, CANVAS), np.float32)
        self.origin = np.array([[1, 0, CANVAS / 2 - W / 2],
                                [0, 1, CANVAS / 2 - H / 2]], np.float32)

    # ---- geometry ----
    def _center(self, pose):
        c = np.array([W / 2, H / 2, 1.0])
        return (pose @ c).astype(np.float32)

    def _blend(self, thumb, pose):
        M = mz._compose(self.origin, pose)
        warp = cv2.warpAffine(thumb.astype(np.float32), M, (CANVAS, CANVAS))
        mask = cv2.warpAffine(np.ones((H, W), np.float32), M, (CANVAS, CANVAS))
        self.acc += warp * mask
        self.cnt += mask

    def _add_kf(self, feats, thumb, pose):
        self.keyframes.append({"feats": feats, "thumb": thumb,
                               "pose": pose, "center": self._center(pose)})
        self._blend(thumb, pose)

    def _register(self, feats, candidates):
        """Best lock of feats on a subset of keyframes. -> (kf, M new->kf, inl) or None."""
        best = None
        for kf in candidates:
            ok, M, inl = coverage.estimate_affine(kf["feats"], feats)
            if ok and inl >= REG_MIN and (best is None or inl > best[2]):
                best = (kf, M, inl)
        return best

    # ---- enroll ----
    def add_frame(self, frame):
        img = preprocess(frame, self.clear)
        feats = sigfm.features(img)
        if feats[1] is None or len(feats[0]) < sigfm.MIN_MATCH:
            return {"status": "debole", "placed": False, "n": len(self.keyframes)}
        if not self.keyframes:
            self._add_kf(feats, img, np.array([[1, 0, 0], [0, 1, 0]], np.float32))
            return {"status": "map started", "placed": True, "n": 1, "thumb": img}

        # the recent ones first, then (if needed) all of them
        best = self._register(feats, self.keyframes[-RECENT:])
        if best is None:
            best = self._register(feats, self.keyframes[:-RECENT])
        if best is None:
            return {"status": "contact lost — go back to where you were", "placed": False,
                    "n": len(self.keyframes), "thumb": img}

        kf, M, inl = best
        pose = mz._compose(kf["pose"], M)            # new->world
        center = self._center(pose)
        dmin = min(np.hypot(*(center[:2] - k["center"][:2])) for k in self.keyframes)
        if dmin < SPACING:
            return {"status": "already covered here", "placed": False,
                    "n": len(self.keyframes), "inl": inl, "thumb": img}
        self._add_kf(feats, img, pose)
        return {"status": "map extended", "placed": True, "new": True,
                "n": len(self.keyframes), "inl": inl, "thumb": img}

    # ---- rendering / holes ----
    def render(self, margin=10):
        ys, xs = np.where(self.cnt > 0)
        if len(xs) == 0:
            return np.zeros((H, W), np.uint8), self.cnt[:H, :W]
        x0, x1 = max(xs.min() - margin, 0), min(xs.max() + margin + 1, CANVAS)
        y0, y1 = max(ys.min() - margin, 0), min(ys.max() + margin + 1, CANVAS)
        acc, cnt = self.acc[y0:y1, x0:x1], self.cnt[y0:y1, x0:x1]
        mos = np.zeros(cnt.shape, np.uint8)
        nz = cnt > 0
        mos[nz] = np.clip(acc[nz] / cnt[nz], 0, 255).astype(np.uint8)
        return mos, cnt

    def holes(self):
        _, cnt = self.render()
        return mz.holes(cnt)

    # ---- verification ----
    def verify(self, frame, threshold=VERIFY_MIN):
        feats = sigfm.features(preprocess(frame, self.clear))
        best_score, best_inl = 0, 0
        for kf in self.keyframes:
            s = sigfm.match_score(kf["feats"], feats)
            if s > best_score:
                best_score = s
            ok, M, inl = coverage.estimate_affine(kf["feats"], feats)
            if ok and inl > best_inl:
                best_inl = inl
        return best_score >= threshold, best_score, best_inl

    # ---- persistence ----
    def save(self, path):
        pts = np.vstack([k["feats"][0].astype(np.float32) for k in self.keyframes])
        desc = np.vstack([k["feats"][1].astype(np.float32) for k in self.keyframes])
        splits = np.array([len(k["feats"][0]) for k in self.keyframes], np.int32)
        poses = np.array([k["pose"] for k in self.keyframes], np.float32)
        thumbs = np.array([k["thumb"] for k in self.keyframes])
        np.savez_compressed(path, pts=pts, desc=desc, splits=splits, poses=poses,
                            thumbs=thumbs, clear=self.clear.astype(np.uint16))

    @classmethod
    def load(cls, path):
        z = np.load(path)
        s = cls(z["clear"].astype(np.uint16))
        pts, desc, splits, poses, thumbs = (z["pts"], z["desc"], z["splits"],
                                            z["poses"], z["thumbs"])
        off = 0
        for n, pose, thumb in zip(splits, poses, thumbs):
            feats = (pts[off:off + n], desc[off:off + n]); off += n
            s._add_kf(feats, thumb, pose.astype(np.float32))
        return s
