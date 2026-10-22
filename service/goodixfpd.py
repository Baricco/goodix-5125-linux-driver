#!/usr/bin/env python3
"""goodixfpd — demone di autenticazione per il sensore Goodix 27c6:5125.

Possiede il sensore, mantiene la logica di verifica (riusa tools/sensor.py + fragment_mapper)
ed espone una verify su un socket Unix. PAM lo interroga tramite il client goodixfp-verify;
non tocca mai la flash del sensore (nessun provisioning: assume la PSK gia' presente).

Protocollo socket: una riga JSON per richiesta, una riga JSON per risposta.
  richiesta  {"op": "verify", "user": "<nome>", "frames": 5}
  risposta   {"ok": true, "finger": "<nome impronta>", "score": 2010}
             {"ok": false, "reason": "..."}

Sicurezza:
  - Il socket e' di proprieta' di root:goodixfp con mode 0660. Chi chiede la verify deve
    essere root (lo stack PAM di login/sudo gira come root) OPPURE l'utente stesso (per i test):
    lo si verifica con SO_PEERCRED, non fidandosi del campo "user" da solo.
  - Il demone non restituisce MAI dati biometrici, solo esito + score.

Template per-utente: GOODIXFP_STATE/<utente>/*.npz (default /var/lib/goodixfp). Formato FragmentMapper.
Configurazione via ambiente: GOODIXFP_SOCKET, GOODIXFP_STATE, GOODIXFP_FRAMES, GOODIXFP_THRESHOLD.
"""

import contextlib
import datetime
import json
import os
import pwd
import socket
import struct
import sys
import threading
import time

_REPO = os.environ.get("GOODIXFP_REPO",
                       os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
sys.path.insert(0, os.path.join(_REPO, "goodix-fp-dump"))
sys.path.insert(0, os.path.join(_REPO, "tools"))

SOCKET_PATH = os.environ.get("GOODIXFP_SOCKET", "/run/goodixfpd/sock")
STATE_DIR = os.environ.get("GOODIXFP_STATE", "/var/lib/goodixfp")
N_FRAMES = int(os.environ.get("GOODIXFP_FRAMES", "5"))
THRESHOLD = int(os.environ.get("GOODIXFP_THRESHOLD", "300"))
WAIT_SECONDS = float(os.environ.get("GOODIXFP_WAIT", "10"))   # attesa max del dito per verify
MIN_FORCE = float(os.environ.get("GOODIXFP_MINFORCE", "0.6"))  # forza minima del tocco per contare
LOG_VERIFY = os.environ.get("GOODIXFP_LOG", "") not in ("", "0")  # diagnostica: salva i frame

_hw_lock = threading.Lock()   # un solo sensore: le verify si serializzano


def log(msg):
    print(f"[goodixfpd] {msg}", flush=True)


# ---------------------------------------------------------------- template utente
def _user_dir(user):
    return os.path.join(STATE_DIR, user)


def load_templates(user):
    """Carica tutte le impronte registrate dell'utente come FragmentMapper. [] se nessuna."""
    import fragment_mapper
    d = _user_dir(user)
    if not os.path.isdir(d):
        return []
    out = []
    for fn in sorted(os.listdir(d)):
        if fn.endswith(".npz"):
            try:
                t = fragment_mapper.FragmentMapper.load(os.path.join(d, fn))
                out.append((os.path.splitext(fn)[0], t))
            except Exception as e:
                log(f"template {fn} non caricabile: {e}")
    return out


# ------------------------------------------------------------------ verifica HW
def verify_user(user, n_frames=N_FRAMES, threshold=THRESHOLD, wait_s=WAIT_SECONDS):
    """Attende il dito e confronta una raffica di frame con le impronte dell'utente.

    Usa la CATTURA CONTINUA (grab) con gate di forza — lo stesso percorso validato dalla GUI sul
    banco — invece del loop arm/wait_touch (fdt-down non blocca e "scappa"). Raccoglie fino a
    n_frames frame con un dito presente entro wait_s secondi; accetta appena un frame supera la
    soglia su una qualunque impronta (la raffica cattura i frame buoni; cfr. docs/matching.md)."""
    templates = load_templates(user)
    if not templates:
        return {"ok": False, "reason": "nessuna impronta registrata per l'utente"}

    import sensor as sensormod
    import coverage
    import numpy as np
    devnull = open(os.devnull, "w")           # silenzia lo spam della libreria goodix
    result = None
    clear = None
    rec = []                                   # diagnostica: (frame, forza, best_score)
    with _hw_lock:
        s = sensormod.Sensor()
        try:
            with contextlib.redirect_stdout(devnull):
                s.open()                       # self-healing: reset USB + sync + TLS
                clear = s.capture_clear()      # baseline di sessione, per il gate di forza
                best = {"score": 0, "finger": None}
                got = 0
                saw_weak = False               # visto un dito ma a contatto debole
                deadline = time.time() + wait_s
                while time.time() < deadline and got < n_frames:
                    try:
                        frame = s.grab()
                    except Exception:
                        continue
                    strength, finger_present = coverage.frame_quality(frame, clear)
                    if not finger_present:
                        continue               # nessun dito: continua ad aspettare
                    if strength < MIN_FORCE:
                        saw_weak = True        # contatto troppo leggero: non conta, aspetta meglio
                        continue
                    got += 1
                    fbest, fname, hit = 0, None, False
                    for name, t in templates:
                        ok, score, _ = t.verify(frame, threshold=threshold)
                        if score > fbest:
                            fbest, fname = int(score), name
                        if ok:
                            hit = True
                    if LOG_VERIFY:
                        rec.append((frame.astype(np.uint16), float(strength), fbest))
                    if fbest > best["score"]:
                        best = {"score": fbest, "finger": fname}
                    if hit:
                        result = {"ok": True, "finger": fname, "score": fbest}
                        break
            if result is None:
                if got == 0:
                    result = {"ok": False,
                              "reason": ("contatto debole: premi con piu' decisione" if saw_weak
                                         else "nessun dito rilevato")}
                else:
                    result = {"ok": False, "reason": "nessuna corrispondenza",
                              "best": best["score"], "finger": best["finger"]}
        except Exception as e:
            result = {"ok": False, "reason": f"errore sensore: {type(e).__name__}: {e}"}
        finally:
            try:
                with contextlib.redirect_stdout(devnull):
                    s.close()                  # teardown pulito: evita il wedge
            except Exception:
                pass
    if LOG_VERIFY and rec and clear is not None:
        _save_verify_log(user, clear, rec, result)
    return result


def _save_verify_log(user, clear, rec, result):
    """Salva i frame catturati durante una verify (grezzi + PNG + forza + score) per l'analisi
    offline. Solo con GOODIXFP_LOG=1. Va in captures/verify_log/ (gitignorato)."""
    import numpy as np
    out = os.path.join(_REPO, "captures", "verify_log")
    os.makedirs(out, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    frames = np.stack([r[0] for r in rec])
    np.savez_compressed(os.path.join(out, f"daemon_{ts}.npz"),
                        frames=frames, clear=clear.astype(np.uint16),
                        strengths=np.array([r[1] for r in rec], np.float32),
                        scores=np.array([r[2] for r in rec], np.int32),
                        result=json.dumps(result))
    try:
        sys.path.insert(0, os.path.join(_REPO, "tools"))
        from preprocessor import preprocess, to_png
        for i, (frame, _st, sc) in enumerate(rec):
            to_png(preprocess(frame, clear), os.path.join(out, f"daemon_{ts}_{i:02d}_s{sc}.png"))
    except Exception:
        pass
    log(f"  [log] salvati {len(rec)} frame in captures/verify_log/daemon_{ts}.* "
        f"(forze={[round(r[1],2) for r in rec]}, score={[r[2] for r in rec]})")


# ----------------------------------------------------------------- server socket
def _peer_uid(conn):
    creds = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
    _pid, uid, _gid = struct.unpack("3i", creds)
    return uid


def _authorized(peer_uid, user):
    if peer_uid == 0:
        return True            # stack PAM (login/sudo) gira come root
    try:
        return peer_uid == pwd.getpwnam(user).pw_uid   # l'utente puo' testare se stesso
    except KeyError:
        return False


def handle(conn):
    try:
        data = b""
        while b"\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
        req = json.loads(data.split(b"\n", 1)[0].decode())
        user = req.get("user", "")
        if req.get("op") != "verify" or not user:
            conn.sendall(b'{"ok": false, "reason": "richiesta non valida"}\n')
            return
        if not _authorized(_peer_uid(conn), user):
            conn.sendall(b'{"ok": false, "reason": "non autorizzato"}\n')
            return
        log(f"verify utente={user} (peer uid {_peer_uid(conn)})")
        res = verify_user(user, int(req.get("frames", N_FRAMES)))
        log(f"  -> {res}")
        conn.sendall((json.dumps(res) + "\n").encode())
    except Exception as e:
        try:
            conn.sendall((json.dumps({"ok": False, "reason": str(e)}) + "\n").encode())
        except Exception:
            pass
    finally:
        conn.close()


def main():
    os.makedirs(os.path.dirname(SOCKET_PATH), exist_ok=True)
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o660)   # root:goodixfp (il gruppo lo mette systemd/RuntimeDirectory)
    srv.listen(4)
    log(f"in ascolto su {SOCKET_PATH}  (state={STATE_DIR}, frames={N_FRAMES}, soglia={THRESHOLD})")
    while True:
        conn, _ = srv.accept()
        # verify seriale (un sensore solo): un thread per connessione, ma _hw_lock serializza.
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
