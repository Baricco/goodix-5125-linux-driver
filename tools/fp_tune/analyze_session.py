#!/usr/bin/env python3
"""Analisi offline di una sessione di calibrazione (calib_session.sh).

Ricostruisce ogni frame dai dump grezzi 16-bit con la pipeline C REALE del
driver (imgproc_run) in piu' varianti, estrae SIGFM con il sigfm.cpp di
produzione (libsigfm_prod.so) e simula enroll -> verify:
  template  = frame della fase enroll che passano il gate di qualita'
  genuini   = fase genuine, impostori = fase impostor (+ genuini specchiati:
              stesso dito "inesistente", stress test del FAR)
Per ogni variante stampa FRR/FAR alle varie soglie.

Uso: tools/.venv/bin/python tools/fp_tune/analyze_session.py [captures/fp-dataset2]
"""
import ctypes, glob, os, re, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(HERE, 'imgproc_run')
DS = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else
                        os.path.join(HERE, '..', '..', 'captures', 'fp-dataset2'))
PHASES = ('enroll', 'genuine', 'impostor')


class Sigfm:
    def __init__(self):
        L = ctypes.CDLL(os.path.join(HERE, 'libsigfm_prod.so'))
        L.sigfm_extract.restype = ctypes.c_void_p
        L.sigfm_extract.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.sigfm_match_score.restype = ctypes.c_int
        L.sigfm_match_score.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        L.sigfm_keypoints_count.restype = ctypes.c_int
        L.sigfm_keypoints_count.argtypes = [ctypes.c_void_p]
        L.sigfm_set_ratio.argtypes = [ctypes.c_double]
        self.L = L

    def extract(self, img):
        a = np.ascontiguousarray(img, dtype=np.uint8)
        return self.L.sigfm_extract(a.ctypes.data, a.shape[1], a.shape[0])

    def kp(self, info):
        return self.L.sigfm_keypoints_count(info)

    def score(self, a, b):
        return self.L.sigfm_match_score(a, b)


def read16(path):
    with open(path, 'rb') as f:
        data = f.read()
    w, h = 80, 64
    return np.frombuffer(data[-w * h * 2:], '>u2').reshape(h, w).astype(np.float32)


def write16(path, a):
    with open(path, 'wb') as f:
        f.write(b'P5\n80 64\n4095\n')
        f.write(np.clip(a, 0, 4095).astype('>u2').tobytes())


def read8(path):
    with open(path, 'rb') as f:
        data = f.read()
    return np.frombuffer(data[-5120:], np.uint8).reshape(64, 80).copy()


def load_phase(phase):
    d = os.path.join(DS, phase)
    frames = []
    for raw in sorted(glob.glob(f'{d}/imgproc-*-raw16.pgm')):
        seq = re.search(r'imgproc-(\d+)-raw16', raw).group(1)
        fr = {'seq': int(seq), 'raw': read16(raw)}
        for st in ('base16', 'fresh16', 'pre16'):
            p = f'{d}/imgproc-{seq}-{st}.pgm'
            if os.path.exists(p):
                fr[st] = read16(p)
        frames.append(fr)
    return frames


def render(frames, env, tmp):
    """Esegue imgproc_run su ogni frame -> (immagini 8-bit, segnali [dc, ridge])."""
    imgs, sig = [], []
    e = dict(os.environ, **env)
    for fr in frames:
        base = fr.get('base16')
        if env.get('GOODIX_IMGPROC_FRESH_BASE') == '1' and 'fresh16' in fr:
            base_f = fr['fresh16']
        else:
            base_f = None
        write16(f'{tmp}/r.pgm', fr['raw'])
        write16(f'{tmp}/b.pgm', base)
        args = [RUN, f'{tmp}/r.pgm', f'{tmp}/b.pgm', f'{tmp}/o.pgm']
        if base_f is not None:
            write16(f'{tmp}/f.pgm', base_f)
            args.append(f'{tmp}/f.pgm')
        o = subprocess.run(args, env=e, capture_output=True, text=True)
        sig.append([float(x) for x in o.stdout.split()] or [0.0, 0.0])
        imgs.append(read8(f'{tmp}/o.pgm'))
    return imgs, np.array(sig)


def evaluate(S, imgs, sig, n_e, n_g, min_ridge, ratio):
    S.L.sigfm_set_ratio(ratio)
    ok = sig[:, 1] >= min_ridge
    infos = [S.extract(im) for im in imgs]
    mir = [S.extract(np.ascontiguousarray(im[:, ::-1])) for im in imgs[n_e:n_e + n_g]]
    tmpl = [infos[i] for i in range(n_e) if ok[i] and S.kp(infos[i]) >= 25]

    def best(info):
        return max((S.score(t, info) for t in tmpl), default=0)

    gen = [best(infos[i]) if ok[i] else -1 for i in range(n_e, n_e + n_g)]
    imp = [best(infos[i]) if ok[i] else -1 for i in range(n_e + n_g, len(imgs))]
    mrr = [best(m) for m in mir]
    return len(tmpl), np.array(gen), np.array(imp), np.array(mrr)


def main():
    data = {p: load_phase(p) for p in PHASES}
    for p in PHASES:
        print(f'{p}: {len(data[p])} frame grezzi')
    if not all(data.values()):
        sys.exit('dataset incompleto')
    allfr = data['enroll'] + data['genuine'] + data['impostor']
    n_e, n_g = len(data['enroll']), len(data['genuine'])
    S = Sigfm()
    variants = {
        'legacy/stored':        {'GOODIX_IMGPROC_MODE': 'legacy', 'GOODIX_IMGPROC_FRESH_BASE': '0'},
        'legacy/fresh':         {'GOODIX_IMGPROC_MODE': 'legacy', 'GOODIX_IMGPROC_FRESH_BASE': '1'},
        'ridge/stored':         {'GOODIX_IMGPROC_MODE': 'ridge', 'GOODIX_IMGPROC_FRESH_BASE': '0', 'GOODIX_IMGPROC_DEGHOST': '0'},
        'ridge/stored/deghost': {'GOODIX_IMGPROC_MODE': 'ridge', 'GOODIX_IMGPROC_FRESH_BASE': '0', 'GOODIX_IMGPROC_DEGHOST': '1'},
        'ridge/fresh':          {'GOODIX_IMGPROC_MODE': 'ridge', 'GOODIX_IMGPROC_FRESH_BASE': '1', 'GOODIX_IMGPROC_DEGHOST': '0'},
        'ridge/fresh/deghost':  {'GOODIX_IMGPROC_MODE': 'ridge', 'GOODIX_IMGPROC_FRESH_BASE': '1', 'GOODIX_IMGPROC_DEGHOST': '1'},
    }
    with tempfile.TemporaryDirectory() as tmp:
        for name, env in variants.items():
            imgs, sig = render(allfr, env, tmp)
            for ratio in (0.75, 0.8):
                for min_ridge in (0.0, 10.0, 20.0):
                    nt, g, i, m = evaluate(S, imgs, sig, n_e, n_g, min_ridge, ratio)
                    gv, iv = g[g >= 0], i[i >= 0]
                    line = [f'{name:22s} r{ratio} gate{min_ridge:>4.0f} tmpl={nt:2d}',
                            f'gen_rej={int((g < 0).sum())} imp_rej={int((i < 0).sum())}',
                            f'imp_max={iv.max() if len(iv) else 0} mir_max={m.max() if len(m) else 0}']
                    for thr in (20, 40, 80):
                        frr = (gv < thr).mean() if len(gv) else 1.0
                        far = (iv >= thr).mean() if len(iv) else 0.0
                        line.append(f'@{thr}: FRR={frr:.2f} FAR={far:.2f}')
                    print('  '.join(line), flush=True)


if __name__ == '__main__':
    main()
