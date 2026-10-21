#!/usr/bin/env python3
"""Mappatura a PUZZLE (pose-graph multi-frammento con fusione) per il Goodix 5125.

Problema del mosaico a singola ancora: se un frame non si aggancia alla mappa corrente va perso, e
l'utente deve ritrovare il bordo. Qui invece ogni scansione entra sempre in un PEZZO (frammento):
  - si aggancia a un pezzo esistente -> lo ESTENDE;
  - combacia con DUE o piu' pezzi -> fa da PONTE e li FONDE in uno piu' grande (riallineando);
  - non combacia con nulla -> inizia un PEZZO NUOVO (niente va perso).
I pezzi si assemblano iterativamente come un puzzle. La GUI mostra il pezzo piu' grande come mappa
principale, gli altri come tessere da incastrare, e "dove sei" (ultimo frame) per non perdersi.

Salvaguardie contro incastri sbagliati (che corromperebbero la mappa): per FONDERE serve un numero
di inlier alto (BRIDGE_MIN) e la coerenza tra le registrazioni ai due pezzi; per estendere basta
REG_MIN."""

import numpy as np
import cv2

import sigfm
import coverage
import mosaic as mz
from preprocessor import preprocess

W, H = 80, 64
CANVAS = 600
REG_MIN = 7          # inlier per agganciare/estendere un pezzo
BRIDGE_MIN = 11      # inlier (su entrambi i pezzi) per fondere: piu' severo, evita fusioni errate
SPACING = 9          # px minimi di spostamento per un nuovo keyframe
VERIFY_MIN = 300     # soglia SIGFM (= enroll_engine.ACCEPT)
IDENT = np.array([[1, 0, 0], [0, 1, 0]], np.float32)


def _center(pose):
    return (pose @ np.array([W / 2, H / 2, 1.0])).astype(np.float32)


class Fragment:
    def __init__(self):
        self.kfs = []        # [{feats, thumb, pose(2x3 coord-pezzo)}]
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
        """Corner del frame (pose) in pixel del render, per il marker 'dove sei'."""
        M = mz._compose(self.origin, pose)
        corners = np.array([[0, 0], [W, 0], [W, H], [0, H]], np.float32)
        pc = (M[:, :2] @ corners.T).T + M[:, 2]
        return (pc - np.array([x0, y0])).astype(np.int32)


class FragmentMapper:
    def __init__(self, clear):
        self.clear = clear
        self.frags = []
        self.last = None        # (fragment, pose) ultimo frame piazzato

    # ---- registrazione ----
    def _best_in(self, feats, frag):
        # sottoinsieme limitato per restare veloci: keyframe recenti + l'ancora (primo) per
        # chiudere i loop su aree vecchie. Evita l'O(N) su mappe grandi.
        cand = frag.kfs if len(frag.kfs) <= 9 else frag.kfs[-8:] + [frag.kfs[0]]
        best = None
        for kf in cand:
            ok, M, inl = coverage.estimate_affine(kf["feats"], feats)
            if ok and inl >= REG_MIN and (best is None or inl > best[2]):
                best = (kf, M, inl)
        return best

    def add_frame(self, frame):
        img = preprocess(frame, self.clear)
        return self.ingest(sigfm.features(img), img)

    def ingest(self, feats, img):
        if feats[1] is None or len(feats[0]) < sigfm.MIN_MATCH:
            return {"status": "debole", "placed": False, "frags": len(self.frags)}

        if not self.frags:
            fr = Fragment(); fr.add_kf(feats, img, IDENT.copy())
            self.frags.append(fr); self.last = (fr, IDENT.copy())
            return self._info("inizio mappa", True, fr, IDENT.copy(), [])

        hits = {}
        for fr in self.frags:
            b = self._best_in(feats, fr)
            if b:
                hits[id(fr)] = (fr, b)
        if not hits:
            fr = Fragment(); fr.add_kf(feats, img, IDENT.copy())
            self.frags.append(fr); self.last = (fr, IDENT.copy())
            return self._info("nuovo pezzo", True, fr, IDENT.copy(), [])

        # ancora = pezzo con match migliore
        anchor, (kfA, MA, inlA) = max(hits.values(), key=lambda v: v[1][2])
        poseA = mz._compose(kfA["pose"], MA)            # new -> coord ancora
        dmin = min(np.hypot(*(_center(poseA)[:2] - c[:2])) for c in anchor.centers())
        placed = dmin >= SPACING
        if placed:
            anchor.add_kf(feats, img, poseA)

        # ponti: fonde gli altri pezzi che combaciano con inlier alti
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

        self.last = (anchor, poseA)
        status = (f"ponte: uniti {len(bridged)} pezzi" if bridged
                  else ("mappa estesa" if placed else "già coperto qui"))
        return self._info(status, placed or bool(bridged), anchor, poseA, bridged, inlA)

    def _info(self, status, placed, frag, pose, bridged, inl=None):
        d = {"status": status, "placed": placed, "frags": len(self.frags),
             "main_tiles": len(self.main().kfs) if self.frags else 0,
             "in_main": frag is self.main()}
        if inl is not None:
            d["inl"] = inl
        return d

    # ---- resa ----
    def main(self):
        return max(self.frags, key=lambda f: len(f.kfs)) if self.frags else None

    def render_main(self):
        """BGR del pezzo principale con buchi (rosso) e marker 'dove sei' (giallo)."""
        fr = self.main()
        if fr is None:
            return None, 0.0
        mos, cnt, (x0, y0) = fr.render()
        hole, frac = mz.holes(cnt)
        vis = cv2.cvtColor(mos, cv2.COLOR_GRAY2BGR)
        vis[hole > 0] = (40, 40, 220)
        if self.last and self.last[0] is fr:
            poly = fr.footprint_px(self.last[1], x0, y0)
            cv2.polylines(vis, [poly], True, (60, 220, 240), 1, cv2.LINE_AA)
        return vis, frac

    def pieces(self):
        """Miniature dei pezzi NON principali (tessere da incastrare)."""
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

    # ---- verifica ----
    def verify(self, frame, threshold=VERIFY_MIN):
        """Accetta se ALMENO un keyframe aggancia (score>=soglia). Early-exit: i pezzi piu' grandi
        (dove stanno i keyframe utili) per primi, si ferma al primo aggancio -> veloce."""
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

    def prune(self, min_tiles=2):
        """Rimuove i pezzetti troppo piccoli (bordi/rumore, inutili alla verifica). -> n rimossi."""
        before = len(self.frags)
        self.frags = [f for f in self.frags if len(f.kfs) >= min_tiles]
        return before - len(self.frags)

    # ---- persistenza ----
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
            raise ValueError("non e' un template a frammenti")
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
