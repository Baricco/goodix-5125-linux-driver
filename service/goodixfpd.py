#!/usr/bin/env python3
"""goodixfpd — demone di autenticazione per il sensore Goodix 27c6:5125.

Possiede il sensore, mantiene la logica di verifica (riusa tools/sensor.py + fragment_mapper)
ed espone una verify su un socket Unix. PAM lo interroga tramite il client goodixfp-verify;
non tocca mai la flash del sensore (nessun provisioning: assume la PSK gia' presente).

SESSIONE CALDA: il sensore si apre UNA volta (alla prima verify) e la sessione TLS resta viva;
ogni verify fa solo cattura+match, niente reinit/teardown a ogni richiesta. Il ciclo apri/chiudi
ripetuto manda in wedge questo sensore (lo prova la lunga storia del progetto); tenerlo aperto,
come fa la GUI di enroll per minuti, e' invece stabile. Si riapre solo se la sessione muore.

Protocollo socket: una riga JSON per richiesta, una riga JSON per risposta.
  richiesta  {"op": "verify", "user": "<nome>", "frames": 8}
  risposta   {"ok": true, "finger": "<nome impronta>", "score": 2010}
             {"ok": false, "reason": "..."}

Sicurezza:
  - Socket root:goodixfp 0660. Chi chiede deve essere root (stack PAM) o l'utente stesso (test):
    verifica con SO_PEERCRED, non fidandosi del campo "user".
  - Non restituisce mai dati biometrici, solo esito + score.

Template per-utente: GOODIXFP_STATE/<utente>/*.npz (default /var/lib/goodixfp). Formato FragmentMapper.
Ambiente: GOODIXFP_SOCKET, GOODIXFP_STATE, GOODIXFP_FRAMES, GOODIXFP_THRESHOLD, GOODIXFP_MINFORCE,
GOODIXFP_WAIT, GOODIXFP_LOG.
"""

import contextlib
import datetime
import json
import os
import pwd
import signal
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
N_FRAMES = int(os.environ.get("GOODIXFP_FRAMES", "8"))        # frame forti da raccogliere nella raffica
THRESHOLD = int(os.environ.get("GOODIXFP_THRESHOLD", "150"))  # soglia SIGFM (impostori ~0: margine ampio)
WAIT_SECONDS = float(os.environ.get("GOODIXFP_WAIT", "6"))    # finestra max di cattura per verify
MIN_FORCE = float(os.environ.get("GOODIXFP_MINFORCE", "0.6")) # forza minima del tocco per contare
LOG_VERIFY = os.environ.get("GOODIXFP_LOG", "") not in ("", "0")  # diagnostica: salva i frame

_hw_lock = threading.Lock()                 # un solo sensore: le verify si serializzano
_session = {"sensor": None, "clear": None}  # sessione calda condivisa (protetta da _hw_lock)
_devnull = open(os.devnull, "w")            # per silenziare lo spam della libreria goodix


def log(msg):
    print(f"[goodixfpd] {msg}", flush=True)


# ------------------------------------------------------------ sessione calda del sensore
def _open_session():
    """Apre il sensore (init + TLS) e cattura il clear di baseline. Riempie _session. Solleva se fallisce."""
    import sensor as sensormod
    s = sensormod.Sensor()
    with contextlib.redirect_stdout(_devnull):
        s.open()
        clear = s.capture_clear()
    _session["sensor"] = s
    _session["clear"] = clear
    log("sessione sensore aperta (TLS stabilita, baseline acquisita)")


def _close_session():
    s = _session.get("sensor")
    _session["sensor"] = None
    _session["clear"] = None
    if s is not None:
        try:
            with contextlib.redirect_stdout(_devnull):
                s.close()                  # teardown pulito: soft-reset MCU, evita il wedge
        except Exception:
            pass


def _capture_burst(sensor, clear, n_frames, wait_s):
    """Raccoglie fino a n_frames frame con un dito a contatto PIENO entro wait_s secondi.
    I frame deboli (<MIN_FORCE) non contano (ma segnalano 'saw_weak'). grab() che solleva
    propaga (la sessione va riaperta). -> (frames, saw_weak)."""
    import coverage
    frames, saw_weak = [], False
    with contextlib.redirect_stdout(_devnull):
        deadline = time.time() + wait_s
        while time.time() < deadline and len(frames) < n_frames:
            frame = sensor.grab()          # puo' sollevare -> gestito dal chiamante
            strength, present = coverage.frame_quality(frame, clear)
            if not present:
                continue
            if strength < MIN_FORCE:
                saw_weak = True
                continue
            frames.append(frame)
    return frames, saw_weak


# ------------------------------------------------------------------------ verifica
def verify_user(user, n_frames=N_FRAMES, threshold=THRESHOLD, wait_s=WAIT_SECONDS):
    templates = load_templates(user)
    if not templates:
        return {"ok": False, "reason": "nessuna impronta registrata per l'utente"}
    import numpy as np

    with _hw_lock:
        # 1) assicura la sessione calda (riapri solo se assente/morta)
        if _session["sensor"] is None:
            try:
                _open_session()
            except (Exception, SystemExit) as e:
                _close_session()
                return {"ok": False, "reason": f"errore sensore: {str(e) or type(e).__name__}"}

        # 2) cattura la raffica; se la sessione e' morta la riapro una volta e ritento
        frames, saw_weak = None, False
        for attempt in (1, 2):
            try:
                frames, saw_weak = _capture_burst(_session["sensor"], _session["clear"], n_frames, wait_s)
                break
            except (Exception, SystemExit) as e:
                log(f"cattura fallita (tentativo {attempt}: {type(e).__name__}): riapro la sessione")
                _close_session()
                if attempt == 2:
                    return {"ok": False, "reason": f"errore sensore: {str(e) or type(e).__name__}"}
                try:
                    _open_session()
                except (Exception, SystemExit) as e2:
                    _close_session()
                    return {"ok": False, "reason": f"errore sensore: {str(e2) or type(e2).__name__}"}

        if not frames:
            return {"ok": False,
                    "reason": ("contatto debole: premi con piu' decisione" if saw_weak
                               else "nessun dito rilevato")}

        # 3) match: accetta se un frame supera la soglia su una qualunque impronta
        best = {"score": 0, "finger": None}
        rec = []
        for frame in frames:
            fbest, fname, hit = 0, None, False
            for name, t in templates:
                ok, score, _ = t.verify(frame, threshold=threshold)
                if score > fbest:
                    fbest, fname = int(score), name
                if ok:
                    hit = True
            if LOG_VERIFY:
                rec.append((frame.astype(np.uint16), 0.0, fbest))
            if fbest > best["score"]:
                best = {"score": fbest, "finger": fname}
            if hit:
                res = {"ok": True, "finger": fname, "score": fbest}
                if LOG_VERIFY:
                    _save_verify_log(user, _session["clear"], rec, res)
                return res

        res = {"ok": False, "reason": "nessuna corrispondenza",
               "best": best["score"], "finger": best["finger"]}
        if LOG_VERIFY and rec:
            _save_verify_log(user, _session["clear"], rec, res)
        return res


def _save_verify_log(user, clear, rec, result):
    """Salva i frame di una verify (grezzi + PNG + score) sotto STATE_DIR/verify_log (scrivibile dal
    servizio). Solo con GOODIXFP_LOG=1."""
    import numpy as np
    out = os.path.join(STATE_DIR, "verify_log")
    try:
        os.makedirs(out, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        np.savez_compressed(os.path.join(out, f"daemon_{ts}.npz"),
                            frames=np.stack([r[0] for r in rec]), clear=clear.astype(np.uint16),
                            scores=np.array([r[2] for r in rec], np.int32),
                            result=json.dumps(result))
        from preprocessor import preprocess, to_png
        for i, (frame, _st, sc) in enumerate(rec):
            to_png(preprocess(frame, clear), os.path.join(out, f"daemon_{ts}_{i:02d}_s{sc}.png"))
        log(f"  [log] {len(rec)} frame in {out}/daemon_{ts}.* (score={[r[2] for r in rec]})")
    except Exception as e:
        log(f"  [log] salvataggio fallito: {type(e).__name__}: {e}")


# ---------------------------------------------------------------- template utente
def load_templates(user):
    """Carica tutte le impronte registrate dell'utente come FragmentMapper. [] se nessuna."""
    import fragment_mapper
    d = os.path.join(STATE_DIR, user)
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
    except (Exception, SystemExit) as e:
        import traceback
        log(f"  !! errore: {type(e).__name__}: {e}")
        log("  !! " + traceback.format_exc().replace("\n", " | "))
        try:
            conn.sendall((json.dumps({"ok": False, "reason": f"{type(e).__name__}: {e}"}) + "\n").encode())
        except Exception:
            pass
    finally:
        conn.close()


def _shutdown(signum, _frame):
    log(f"segnale {signum}: chiudo la sessione sensore e esco")
    _close_session()           # best-effort: teardown pulito per non lasciare il sensore in wedge
    sys.exit(0)


def main():
    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)
    os.makedirs(os.path.dirname(SOCKET_PATH), exist_ok=True)
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o660)   # root:goodixfp (il gruppo lo mette systemd/RuntimeDirectory)
    srv.listen(4)
    log(f"in ascolto su {SOCKET_PATH}  (state={STATE_DIR}, frames={N_FRAMES}, soglia={THRESHOLD}, "
        f"minforza={MIN_FORCE}, sessione calda)")
    while True:
        conn, _ = srv.accept()
        # verify seriale (un sensore solo): un thread per connessione, ma _hw_lock serializza.
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
