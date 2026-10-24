#!/usr/bin/env python3
"""Template multi-frame per il riconoscimento con SIGFM (Goodix 27c6:5125).

Modello (quello giusto per sensori piccoli, vedi docs/matching.md):
  - ENROLL per copertura: l'utente tocca N volte spostando il dito per coprire zone diverse.
    Ogni frame viene preprocessato (CLAHE) e se ne estraggono le feature SIFT, salvate nel template.
  - VERIFY: un tocco viene confrontato con TUTTI i frame del template; lo score e' il MAX.
    Un dito genuino si sovrappone ad almeno un frame registrato (score enorme), un impostore a
    nessuno (score ~0). La soglia separa i due regimi.

Le feature sono precalcolate in enroll: la verify costa una preprocessatura + N confronti SIGFM
(ciascuno ~1-80 ms), quindi e' veloce anche con template ampi.

Il template lavora sui *_plain.bin salvati dal driver, cosi' il matching e' testabile offline
senza toccare l'hardware. Il frame clear (baseline) e' richiesto per il preprocessing.
"""

import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocessor import decode, preprocess
import sigfm

# Soglia di accettazione (score SIGFM). Da tarare sul dataset reale di enroll per copertura;
# con preprocessing CLAHE i genuini con overlap stanno nell'ordine 10^3-10^6, gli impostori < ~50.
THRESHOLD = 300


def _frame(plain_path):
    return decode(open(plain_path, "rb").read()[8:8 + 7680])


def image_features(plain_path, clear_path):
    """plain_path -> feature SIGFM dopo preprocessing CLAHE con baseline clear."""
    img = preprocess(_frame(plain_path), _frame(clear_path))
    return sigfm.features(img)


def build_template(plain_paths, clear_path, out_path):
    """Costruisce e salva un template .npz dalle catture di enroll."""
    pts_list, desc_list, splits = [], [], []
    for p in plain_paths:
        pts, desc = image_features(p, clear_path)
        if desc is None or len(pts) < sigfm.MIN_MATCH:
            print(f"  [skip] {os.path.basename(p)}: troppe poche feature ({len(pts)})")
            continue
        pts_list.append(pts.astype(np.float32))
        desc_list.append(desc.astype(np.float32))
        splits.append(len(pts))
        print(f"  [ok]   {os.path.basename(p)}: {len(pts)} keypoint")
    if not splits:
        raise SystemError("nessun frame valido nel template")
    np.savez_compressed(out_path,
                        pts=np.vstack(pts_list), desc=np.vstack(desc_list),
                        splits=np.array(splits, dtype=np.int32))
    print(f"template salvato in {out_path} ({len(splits)} frame)")


def load_template(path):
    """-> lista di (pts, desc) per frame."""
    z = np.load(path)
    pts, desc, splits = z["pts"], z["desc"], z["splits"]
    frames, off = [], 0
    for n in splits:
        frames.append((pts[off:off + n], desc[off:off + n]))
        off += n
    return frames


def verify(template_frames, probe_feats, threshold=THRESHOLD):
    """-> (accettato, best_score). best_score = max SIGFM su tutti i frame del template."""
    best = 0
    for tf in template_frames:
        best = max(best, sigfm.match_score(tf, probe_feats))
    return best >= threshold, best


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Enroll/verify SIGFM da *_plain.bin")
    ap.add_argument("--clear", required=True, help="frame baseline (clear-0_plain.bin)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("enroll", help="costruisci un template dalle catture")
    e.add_argument("--out", required=True)
    e.add_argument("plain", nargs="+", help="file *_plain.bin dei tocchi di enroll")
    v = sub.add_parser("verify", help="verifica un tocco contro un template")
    v.add_argument("--template", required=True)
    v.add_argument("--threshold", type=int, default=THRESHOLD)
    v.add_argument("probe", help="file *_plain.bin del tocco da verificare")
    args = ap.parse_args()

    if args.cmd == "enroll":
        build_template(args.plain, args.clear, args.out)
    else:
        frames = load_template(args.template)
        feats = image_features(args.probe, args.clear)
        ok, score = verify(frames, feats, args.threshold)
        print(f"score={score}  soglia={args.threshold}  ->  {'ACCETTATO' if ok else 'RIFIUTATO'}")
        sys.exit(0 if ok else 1)
