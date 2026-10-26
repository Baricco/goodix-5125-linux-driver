#!/usr/bin/env python3
"""PUZZLE mapping (multi-fragment pose graph with merging) for the Goodix 5125.

The problem of a single-anchor mosaic: if a frame does not lock onto the current map it is lost,
and the user has to find the edge again. Here every scan always goes into a PIECE (fragment):
  - it locks onto an existing piece -> it EXTENDS it;
  - it matches TWO or more pieces -> it acts as a BRIDGE and MERGES them into a bigger one (realigning);
  - it matches nothing -> it starts a NEW PIECE (nothing is lost).
The pieces are assembled iteratively like a puzzle. The GUI shows the biggest piece as the main
map, the others as tiles still to fit, and "where you are" (last frame) so you do not get lost.

Safeguards against wrong fits (which would corrupt the map): MERGING needs a high number of
inliers (BRIDGE_MIN) and consistency between the registrations to the two pieces; extending only
needs REG_MIN."""

import numpy as np
import cv2

import sigfm
import coverage
import mosaic as mz
import posegraph
from preprocessor import preprocess

W, H = 80, 64
CANVAS = 600
REG_MIN = 7          # inliers to lock onto/extend a piece
BRIDGE_MIN = 11      # inliers (on both pieces) to merge: stricter, avoids wrong merges
FAST_INL = 14        # "solid" lock on the current fragment -> fast path without searching all of them
NEIGH_DIST = 45      # px: two keyframes within this distance overlap -> pose-graph edge
LOOP_MIN_SEP = 12    # if an edge links keyframes far apart in insertion order = loop closure
SPACING = 9          # minimum px of displacement for a new keyframe
VERIFY_MIN = 300     # SIGFM threshold (= enroll_engine.ACCEPT)
IDENT = np.array([[1, 0, 0], [0, 1, 0]], np.float32)


def _center(pose):
    return (pose @ np.array([W / 2, H / 2, 1.0])).astype(np.float32)


class Fragment:
    def __init__(self):
        self.kfs = []        # [{feats, thumb, pose(2x3 piece coordinates)}]
        self.edges = []      # pose-graph edges: (i, j, (dx,dy,dth), w)
        self.origin = np.array([[1, 0, CANVAS / 2 - W / 2],
                                [0, 1, CANVAS / 2 - H / 2]], np.float32)
        self.acc = np.zeros((CANVAS, CANVAS), np.float32)
        self.cnt = np.zeros((CANVAS, CANVAS), np.float32)

    def _blend(self, thumb, pose):
        M = mz._compose(self.origin, pose)
        self.acc += cv2.warpAffine(thumb.astype(np.float32), M, (CANVAS, CANVAS)) \
            * cv2.warpAffine(np.ones((H, W), np.float32), M, (CANVAS, CANVAS))
        self.cnt += cv2.warpAffine(np.ones((H, W), np.float32), M, (CANVAS, CANVAS))

    def add_kf(self, feats, thumb, pose):
        self.kfs.append({"feats": feats, "thumb": thumb, "pose": pose})
        self._blend(thumb, pose)
        return len(self.kfs) - 1

    def apply_poses(self, xyt):
        """Replaces the keyframe poses with the optimized ones (Nx3) and rebuilds."""
        for kf, p in zip(self.kfs, xyt):
            kf["pose"] = posegraph.xyt_to_pose(p[0], p[1], p[2])
        self.rebuild()

    def rebuild(self):
        self.acc[:] = 0; self.cnt[:] = 0
        for kf in self.kfs:
            self._blend(kf["thumb"], kf["pose"])

    def centers(self):
        return [_center(kf["pose"]) for kf in self.kfs]

    def render(self, margin=10):
        ys, xs = np.where(self.cnt > 0)
        if len(xs) == 0:
            return np.zeros((H, W), np.uint8), self.cnt[:H, :W], (0, 0)
        x0, x1 = max(xs.min() - margin, 0), min(xs.max() + margin + 1, CANVAS)
        y0, y1 = max(ys.min() - margin, 0), min(ys.max() + margin + 1, CANVAS)
        acc, cnt = self.acc[y0:y1, x0:x1], self.cnt[y0:y1, x0:x1]
        mos = np.zeros(cnt.shape, np.uint8); nz = cnt > 0
        mos[nz] = np.clip(acc[nz] / cnt[nz], 0, 255).astype(np.uint8)
        return mos, cnt, (x0, y0)

    def footprint_px(self, pose, x0, y0):
        """Frame corners (pose) in render pixels, for the 'where you are' marker."""
        M = mz._compose(self.origin, pose)
        corners = np.array([[0, 0], [W, 0], [W, H], [0, H]], np.float32)
        pc = (M[:, :2] @ corners.T).T + M[:, 2]
        return (pc - np.array([x0, y0])).astype(np.int32)


class FragmentMapper:
    def __init__(self, clear):
        self.clear = clear
        self.frags = []
        self.last = None        # (fragment, pose) of the last placed frame
        self.current = None     # last extended fragment (for the fast path / view)
        self.prev = None        # {feats, pose, frag} of the last tracked frame (for odometry)

    # ---- registration ----
    def _best_in(self, feats, frag):
        # a limited subset to stay fast: recent keyframes + the anchor (first one) to
        # close loops over old areas. Avoids O(N) on large maps.
        cand = frag.kfs if len(frag.kfs) <= 9 else frag.kfs[-8:] + [frag.kfs[0]]
        best = None
        for kf in cand:
            ok, M, inl = coverage.estimate_affine(kf["feats"], feats)
            if ok and inl >= REG_MIN and (best is None or inl > best[2]):
                best = (kf, M, inl)
        return best

    def add_frame(self, frame):
        # STRENGTH GATE: drops frames without a finger (static sensor noise, strength ~0.03).
        # Without it the noise has random SIFT features and would create junk tiles.
        strength, ok = coverage.frame_quality(frame, self.clear)
        if not ok:
            return {"status": "empty (no finger)", "placed": False,
                    "frags": len(self.frags), "strength": strength}
        img = preprocess(frame, self.clear)
        return self.ingest(sigfm.features(img), img)

    def _new_fragment(self, feats, img, status):
        fr = Fragment(); fr.add_kf(feats, img, IDENT.copy())
        self.frags.append(fr)
        self.last = (fr, IDENT.copy()); self.current = fr
        self.prev = {"feats": feats, "pose": IDENT.copy(), "frag": fr}
        return self._info(status, True, fr, IDENT.copy(), [])

    def _link_and_optimize(self, frag, j):
        """Links keyframe j to its SPATIAL neighbours (pose-graph edges); if it closes a loop
        (an edge towards a keyframe far away in time) it optimizes and straightens the drift."""
        cj = _center(frag.kfs[j]["pose"])[:2]
        fj = frag.kfs[j]["feats"]
        loop = False
        for i, kf in enumerate(frag.kfs):
            if i == j:
                continue
            if np.hypot(*(cj - _center(kf["pose"])[:2])) > NEIGH_DIST:
                continue
            ok, M, inl = coverage.estimate_affine(kf["feats"], fj)    # j -> i
            if ok and inl >= REG_MIN:
                z = (float(M[0, 2]), float(M[1, 2]), float(np.arctan2(M[1, 0], M[0, 0])))
                frag.edges.append((i, j, z, float(inl)))
                if abs(i - j) > LOOP_MIN_SEP:
                    loop = True
        if loop and len(frag.kfs) >= 4:
            try:
                xyt = np.array([posegraph.pose_to_xyt(kf["pose"]) for kf in frag.kfs])
                frag.apply_poses(posegraph.optimize(xyt, frag.edges))
                return True
            except Exception:
                pass
        return False

    def _place(self, frag, feats, img, pose, inl, status):
        """Places a frame at a given pose on a fragment; updates last/current/prev."""
        dmin = min(np.hypot(*(_center(pose)[:2] - c[:2])) for c in frag.centers())
        placed = dmin >= SPACING
        if placed:
            j = frag.add_kf(feats, img, pose)
            if self._link_and_optimize(frag, j):
                pose = frag.kfs[j]["pose"]      # pose straightened by the optimization
        self.last = (frag, pose); self.current = frag
        self.prev = {"feats": feats, "pose": pose, "frag": frag}
        return self._info(status if placed else "already covered here", placed, frag, pose, [], inl)

    def ingest(self, feats, img):
        if feats[1] is None or len(feats[0]) < sigfm.MIN_MATCH:
            return {"status": "debole", "placed": False, "frags": len(self.frags)}

        if not self.frags:
            return self._new_fragment(feats, img, "map started")

        # 1) FAST PATH: continuous swiping stays on the current fragment -> extend right away.
        cur = self.current if self.current in self.frags else None
        if cur is not None:
            b = self._best_in(feats, cur)
            if b and b[2] >= FAST_INL:
                kf, M, inl = b
                return self._place(cur, feats, img, mz._compose(kf["pose"], M), inl, "map extended")

        # 2) FULL SEARCH: relocalization over all pieces + bridges.
        hits = {}
        for fr in self.frags:
            b = self._best_in(feats, fr)
            if b:
                hits[id(fr)] = (fr, b)
        if hits:
            anchor, (kfA, MA, inlA) = max(hits.values(), key=lambda v: v[1][2])
            poseA = mz._compose(kfA["pose"], MA)
            dmin = min(np.hypot(*(_center(poseA)[:2] - c[:2])) for c in anchor.centers())
            placed = dmin >= SPACING
            jj = anchor.add_kf(feats, img, poseA) if placed else None
            bridged = []
            for key, (fr, (kfB, MB, inlB)) in list(hits.items()):
                if fr is anchor or inlB < BRIDGE_MIN:
                    continue
                poseB = mz._compose(kfB["pose"], MB)
                BtoA = mz._compose(poseA, mz._invert(poseB))
                for kf in fr.kfs:
                    kf["pose"] = mz._compose(BtoA, kf["pose"])
                    anchor.kfs.append(kf)
                self.frags.remove(fr)
                bridged.append(fr)
            if bridged:
                anchor.rebuild()
            elif jj is not None and self._link_and_optimize(anchor, jj):
                poseA = anchor.kfs[jj]["pose"]     # straightened by the pose graph (loop closure)
            reloc = (anchor is not cur)
            self.last = (anchor, poseA); self.current = anchor
            self.prev = {"feats": feats, "pose": poseA, "frag": anchor}
            status = (f"bridge: merged {len(bridged)} pieces" if bridged
                      else ("found again on a piece" if reloc else
                            ("map extended" if placed else "already covered here")))
            return self._info(status, placed or bool(bridged), anchor, poseA, bridged, inlA)

        # 3) frame-to-frame ODOMETRY: no keyframe locks, but the frame overlaps the
        #    PREVIOUS one (slow swipe) -> keep tracking in the same coordinate system.
        #    This way the marker does not disappear and the new area joins the current piece (no fragments).
        if self.prev is not None and self.prev["frag"] in self.frags:
            ok, M, inl = coverage.estimate_affine(self.prev["feats"], feats)   # new -> prev
            if ok and inl >= REG_MIN:
                pose = mz._compose(self.prev["pose"], M)
                return self._place(self.prev["frag"], feats, img, pose, inl, "traccia")

        # 4) NEW PIECE: tracking really lost (finger lifted or a clear jump).
        return self._new_fragment(feats, img, "new piece")

    def _info(self, status, placed, frag, pose, bridged, inl=None):
        d = {"status": status, "placed": placed, "frags": len(self.frags),
             "main_tiles": len(self.main().kfs) if self.frags else 0,
             "in_main": frag is self.main()}
        if inl is not None:
            d["inl"] = inl
        return d

    # ---- rendering ----
    def main(self):
        return max(self.frags, key=lambda f: len(f.kfs)) if self.frags else None

    def view(self):
        """Fragment to show = the one WHERE YOU ARE now (current), otherwise the main one.
        This way the 'you are here' marker stays visible even when you move onto a detached piece."""
        if self.current is not None and self.current in self.frags:
            return self.current
        return self.main()

    def render_main(self):
        """BGR of the current piece with holes (red) and the 'you are here' marker."""
        fr = self.view()
        if fr is None:
            return None, 0.0
        mos, cnt, (x0, y0) = fr.render()
        hole, frac = mz.holes(cnt)
        vis = cv2.cvtColor(mos, cv2.COLOR_GRAY2BGR)
        vis[hole > 0] = (40, 40, 220)
        if self.last and self.last[0] is fr:
            poly = fr.footprint_px(self.last[1], x0, y0)   # "you are here" marker
            overlay = vis.copy()
            cv2.fillPoly(overlay, [poly], (70, 200, 240))
            cv2.addWeighted(overlay, 0.30, vis, 0.70, 0, vis)
            cv2.polylines(vis, [poly], True, (80, 230, 255), 2, cv2.LINE_AA)
        return vis, frac

    def pieces(self):
        """Thumbnails of the NON-main pieces (tiles still to fit)."""
        main = self.main()
        out = []
        for fr in self.frags:
            if fr is main:
                continue
            mos, _, _ = fr.render(margin=4)
            out.append((mos, len(fr.kfs)))
        return out

    def holes(self):
        fr = self.main()
        if fr is None:
            return None, 0.0
        _, cnt, _ = fr.render()
        return mz.holes(cnt)

    # ---- verification ----
    def verify(self, frame, threshold=VERIFY_MIN):
        """Accepts if AT LEAST one keyframe locks (score>=threshold). Early exit: the biggest pieces
        (where the useful keyframes are) first, it stops at the first lock -> fast."""
        feats = sigfm.features(preprocess(frame, self.clear))
        best = 0
        for fr in sorted(self.frags, key=lambda f: -len(f.kfs)):
            for kf in fr.kfs:
                s = sigfm.match_score(kf["feats"], feats)
                if s > best:
                    best = s
                if best >= threshold:
                    return True, best, 0
        return False, best, 0

    def best_score(self, feats, cap=VERIFY_MIN):
        """Best SIGFM score of feats (already extracted) against the keyframes; early exit at cap.
        For verification against several fingerprints without preprocessing again."""
        best = 0
        for fr in sorted(self.frags, key=lambda f: -len(f.kfs)):
            for kf in fr.kfs:
                s = sigfm.match_score(kf["feats"], feats)
                if s > best:
                    best = s
                if best >= cap:
                    return best
        return best

    def prune(self, min_tiles=2):
        """Removes pieces that are too small (edges/noise, useless for verification). -> number removed."""
        before = len(self.frags)
        self.frags = [f for f in self.frags if len(f.kfs) >= min_tiles]
        return before - len(self.frags)

    # ---- persistence ----
    def save(self, path):
        pts, desc, splits, frag_ids, poses, thumbs = [], [], [], [], [], []
        for fid, fr in enumerate(self.frags):
            for kf in fr.kfs:
                pts.append(kf["feats"][0].astype(np.float32))
                desc.append(kf["feats"][1].astype(np.float32))
                splits.append(len(kf["feats"][0]))
                frag_ids.append(fid)
                poses.append(kf["pose"]); thumbs.append(kf["thumb"])
        np.savez_compressed(path, pts=np.vstack(pts), desc=np.vstack(desc),
                            splits=np.array(splits, np.int32), frag_ids=np.array(frag_ids, np.int32),
                            poses=np.array(poses, np.float32), thumbs=np.array(thumbs),
                            clear=self.clear.astype(np.uint16))

    @classmethod
    def load(cls, path):
        z = np.load(path)
        if "frag_ids" not in z:
            raise ValueError("not a fragment template")
        s = cls(z["clear"].astype(np.uint16))
        pts, desc, splits, fids, poses, thumbs = (z["pts"], z["desc"], z["splits"],
                                                  z["frag_ids"], z["poses"], z["thumbs"])
        by = {}
        off = 0
        for n, fid, pose, thumb in zip(splits, fids, poses, thumbs):
            by.setdefault(int(fid), []).append(((pts[off:off + n], desc[off:off + n]),
                                                thumb, pose.astype(np.float32)))
            off += n
        for fid in sorted(by):
            fr = Fragment()
            for feats, thumb, pose in by[fid]:
                fr.add_kf(feats, thumb, pose)
            s.frags.append(fr)
        return s
