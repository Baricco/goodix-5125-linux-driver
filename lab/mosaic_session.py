#!/usr/bin/env python3
"""Enroll a MOSAICO INCREMENTALE per il Goodix 5125.

A differenza del modello a punti (enroll_engine), qui si costruisce una mappa unica cucendo le
scansioni man mano. Per ogni frame in arrivo si tenta la registrazione (roto-traslazione) sui
keyframe recenti/vicini della mappa:
  - si aggancia (inlier >= REG_MIN) e si e' spostato abbastanza -> ESTENDE la mappa (nuovo keyframe);
  - si aggancia ma quasi fermo -> gia' coperto;
  - non si aggancia -> CONTATTO PERSO (l'utente ha saltato): non si estende, si avvisa di tornare
    indietro. Cosi' la mappa resta connessa e i buchi emergono dalla copertura.

Fornisce il composito live (immagine cucita), la mappa di copertura e i buchi (mosaic.holes), e una
verifica che usa lo score SIGFM (quello che ha dato 0 falsi positivi) piu' gli inlier di
registrazione come segnale geometrico.
"""

import numpy as np
import cv2

import sigfm
import coverage
import mosaic as mz
from preprocessor import preprocess

W, H = 80, 64
CANVAS = 520
REG_MIN = 8          # inlier minimi per agganciare un frame alla mappa
SPACING = 9          # px minimi di spostamento per aggiungere un keyframe (densita' del mosaico)
RECENT = 10          # quanti keyframe recenti provare prima di cercare in tutta la mappa
VERIFY_MIN = 300     # soglia SIGFM di accettazione (= enroll_engine.ACCEPT)


class MosaicSession:
    def __init__(self, clear):
        self.clear = clear
        self.keyframes = []     # {feats, thumb, pose(2x3 new->world), center(xy world)}
        self.acc = np.zeros((CANVAS, CANVAS), np.float32)
        self.cnt = np.zeros((CANVAS, CANVAS), np.float32)
        self.origin = np.array([[1, 0, CANVAS / 2 - W / 2],
                                [0, 1, CANVAS / 2 - H / 2]], np.float32)

    # ---- geometria ----
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
        """Miglior aggancio di feats su un sottoinsieme di keyframe. -> (kf, M new->kf, inl) o None."""
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
            return {"status": "inizio mappa", "placed": True, "n": 1, "thumb": img}

        # prima i recenti, poi (se serve) tutti
        best = self._register(feats, self.keyframes[-RECENT:])
        if best is None:
            best = self._register(feats, self.keyframes[:-RECENT])
        if best is None:
            return {"status": "contatto perso — torna dove eri", "placed": False,
                    "n": len(self.keyframes), "thumb": img}

        kf, M, inl = best
        pose = mz._compose(kf["pose"], M)            # new->world
        center = self._center(pose)
        dmin = min(np.hypot(*(center[:2] - k["center"][:2])) for k in self.keyframes)
        if dmin < SPACING:
            return {"status": "già coperto qui", "placed": False,
                    "n": len(self.keyframes), "inl": inl, "thumb": img}
        self._add_kf(feats, img, pose)
        return {"status": "mappa estesa", "placed": True, "new": True,
                "n": len(self.keyframes), "inl": inl, "thumb": img}

    # ---- resa / buchi ----
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

    # ---- verifica ----
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

    # ---- persistenza ----
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
