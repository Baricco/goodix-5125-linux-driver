#!/usr/bin/env python3
"""Motore di enroll a COPERTURA DI PUNTI per il Goodix 27c6:5125 (modello giusto per sensori a
tocco, vedi docs/matching.md).

Il sensore e' piccolo e a tocco: due pressioni si sovrappongono solo se cadono quasi sullo stesso
punto. Non si puo' cucire un mosaico continuo (gli offset tra tocchi sono ~0 o nulli). Quindi il
template e' un INSIEME DI PUNTI distinti del dito; la verifica accetta se il tocco si sovrappone
ad almeno un punto registrato (max score SIGFM). Piu' punti si coprono, meno "buchi", meno falsi
rifiuti.

- EnrollSession: ricevi un frame alla volta, decide se e' un punto NUOVO, un duplicato di un punto
  gia' coperto, o va scartato (vuoto). Tiene la mappa dei punti per la GUI.
- Verifier: un tocco -> (accettato, score, punto).
"""

import numpy as np

import sigfm
import coverage
from preprocessor import preprocess, decode

SAME_SPOT = 400       # score SIGFM oltre il quale due frame sono "lo stesso punto" del dito
ACCEPT = 300          # soglia di verifica (vedi template.THRESHOLD); da tarare su dati reali


def _thumb(img_u8):
    return img_u8.copy()


class Spot:
    __slots__ = ("frames", "thumb", "count")

    def __init__(self, feats, thumb):
        self.frames = [feats]   # piu' feature set dello stesso punto (varieta' pressione/umidita')
        self.thumb = thumb      # immagine preprocessata rappresentativa (per la GUI)
        self.count = 1


class EnrollSession:
    def __init__(self, clear_frame):
        self.clear = clear_frame
        self.spots = []

    def _feats(self, frame):
        img = preprocess(frame, self.clear)
        return img, sigfm.features(img)

    def best_spot(self, feats):
        """(indice, score) del punto piu' sovrapposto, o (-1, 0)."""
        best, bi = 0, -1
        for i, sp in enumerate(self.spots):
            s = max(sigfm.match_score(f, feats) for f in sp.frames)
            if s > best:
                best, bi = s, i
        return bi, best

    def add_frame(self, frame):
        """-> dict con esito. Non solleva: la GUI mostra reason e strength."""
        strength, ok = coverage.frame_quality(frame, self.clear)
        if not ok:
            return {"accepted": False, "reason": "vuoto", "strength": strength,
                    "coverage": len(self.spots)}
        img, feats = self._feats(frame)
        pts, desc = feats
        if desc is None or len(pts) < sigfm.MIN_MATCH:
            return {"accepted": False, "reason": "poche feature", "strength": strength,
                    "coverage": len(self.spots)}
        idx, score = self.best_spot(feats)
        if score >= SAME_SPOT:
            sp = self.spots[idx]
            if len(sp.frames) < 4:      # conserva un po' di varieta' per punto, senza gonfiare
                sp.frames.append(feats)
            sp.count += 1
            return {"accepted": True, "reason": "punto rinforzato", "new": False,
                    "spot": idx, "score": score, "strength": strength,
                    "coverage": len(self.spots), "thumb": img}
        self.spots.append(Spot(feats, _thumb(img)))
        return {"accepted": True, "reason": "nuovo punto", "new": True,
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
        """Ricostruisce una sessione da un template salvato (per continuare/verificare)."""
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
        """-> (accettato, best_score, spot). Passa frame grezzo (usa il clear salvato) o feats."""
        if feats is None:
            feats = sigfm.features(preprocess(frame, self.clear))
        best, bspot = 0, -1
        for fr, sid in zip(self.frames, self.spot_ids):
            s = sigfm.match_score(fr, feats)
            if s > best:
                best, bspot = s, int(sid)
        return best >= threshold, best, bspot
