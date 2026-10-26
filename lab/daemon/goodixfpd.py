#!/usr/bin/env python3
"""goodixfpd — authentication daemon for the Goodix 27c6:5125 sensor.

It owns the sensor, holds the verification logic (reuses lab/sensor.py + fragment_mapper)
and exposes a verify over a Unix socket. PAM queries it through the goodixfp-verify client;
it never touches the sensor's flash (no provisioning: it assumes the PSK is already there).

WARM SESSION: the sensor is opened ONCE (at the first verify) and the TLS session stays alive;
every verify only captures + matches, no reinit/teardown per request. The repeated open/close
cycle drives this sensor into a wedge (the long history of the project proves it); keeping it
open, as the enrollment GUI does for minutes, is stable instead. It reopens only if the session dies.

Socket protocol: one JSON line per request, one JSON line per reply.
  request  {"op": "verify", "user": "<name>", "frames": 8}
  reply    {"ok": true, "finger": "<fingerprint name>", "score": 2010}
           {"ok": false, "reason": "..."}

Security:
  - Socket root:goodixfp 0660. The caller must be root (PAM stack) or the user themselves (tests):
    checked with SO_PEERCRED, without trusting the "user" field.
  - It never returns biometric data, only outcome + score.

Per-user templates: GOODIXFP_STATE/<user>/*.npz (default /var/lib/goodixfp). FragmentMapper format.
Environment: GOODIXFP_SOCKET, GOODIXFP_STATE, GOODIXFP_FRAMES, GOODIXFP_THRESHOLD, GOODIXFP_MINFORCE,
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
                       os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
sys.path.insert(0, os.path.join(_REPO, "goodix-fp-dump"))
sys.path.insert(0, os.path.join(_REPO, "lab"))

SOCKET_PATH = os.environ.get("GOODIXFP_SOCKET", "/run/goodixfpd/sock")
STATE_DIR = os.environ.get("GOODIXFP_STATE", "/var/lib/goodixfp")
N_FRAMES = int(os.environ.get("GOODIXFP_FRAMES", "8"))        # strong frames to collect in the burst
THRESHOLD = int(os.environ.get("GOODIXFP_THRESHOLD", "150"))  # SIGFM threshold (impostors ~0: wide margin)
WAIT_SECONDS = float(os.environ.get("GOODIXFP_WAIT", "6"))    # maximum capture window per verify
MIN_FORCE = float(os.environ.get("GOODIXFP_MINFORCE", "0.6")) # minimum touch strength to count
LOG_VERIFY = os.environ.get("GOODIXFP_LOG", "") not in ("", "0")  # diagnostics: save the frames

_hw_lock = threading.Lock()                 # a single sensor: verifies are serialized
_session = {"sensor": None, "clear": None}  # shared warm session (protected by _hw_lock)
_devnull = open(os.devnull, "w")            # to silence the goodix library's spam


def log(msg):
    print(f"[goodixfpd] {msg}", flush=True)


# ------------------------------------------------------------ warm sensor session
def _open_session():
    """Opens the sensor (init + TLS) and captures the baseline clear frame. Fills _session. Raises on failure."""
    import sensor as sensormod
    s = sensormod.Sensor()
    with contextlib.redirect_stdout(_devnull):
        s.open()
        clear = s.capture_clear()
    _session["sensor"] = s
    _session["clear"] = clear
    log("sensor session open (TLS established, baseline captured)")


def _close_session():
    s = _session.get("sensor")
    _session["sensor"] = None
    _session["clear"] = None
    if s is not None:
        try:
            with contextlib.redirect_stdout(_devnull):
                s.close()                  # clean teardown: MCU soft reset, avoids the wedge
        except Exception:
            pass


def _reopen_or_fail():
    """Closes and reopens the session. -> None if ok, otherwise an error dict ready for the socket."""
    _close_session()
    try:
        _open_session()
        return None
    except (Exception, SystemExit) as e:
        _close_session()
        return {"ok": False, "reason": f"sensor error: {str(e) or type(e).__name__}"}


def _frozen(frames, thresh=20.0):
    """True if the frames are almost identical to each other (sensor asleep in powerdown: grab()
    returns a frozen frame). A real contact varies much more (measured ~500 vs ~5)."""
    import numpy as np
    if len(frames) < 2:
        return False
    return float(np.stack(frames).astype(np.float32).std(axis=0).mean()) < thresh


def _capture_burst(sensor, clear, n_frames, wait_s):
    """Collects up to n_frames frames with a finger in FULL contact within wait_s seconds.
    Weak frames (<MIN_FORCE) do not count (but set 'saw_weak'). A grab() that raises
    propagates (the session must be reopened). -> (frames, saw_weak)."""
    import coverage
    frames, saw_weak = [], False
    with contextlib.redirect_stdout(_devnull):
        deadline = time.time() + wait_s
        while time.time() < deadline and len(frames) < n_frames:
            frame = sensor.grab()          # may raise -> handled by the caller
            strength, present = coverage.frame_quality(frame, clear)
            if not present:
                continue
            if strength < MIN_FORCE:
                saw_weak = True
                continue
            frames.append(frame)
    return frames, saw_weak


# ------------------------------------------------------------------------ verification
def verify_user(user, n_frames=N_FRAMES, threshold=THRESHOLD, wait_s=WAIT_SECONDS):
    templates = load_templates(user)
    if not templates:
        return {"ok": False, "reason": "no fingerprint enrolled for the user"}
    import numpy as np

    with _hw_lock:
        # 1) make sure the warm session exists (reopen only if missing/dead)
        if _session["sensor"] is None:
            try:
                _open_session()
            except (Exception, SystemExit) as e:
                _close_session()
                return {"ok": False, "reason": f"sensor error: {str(e) or type(e).__name__}"}

        # 2) capture the burst. First WAKE the scan: after idle the sensor goes into powerdown and
        #    grab() returns a frozen frame (measured: all frames identical, score flat 0). If the
        #    capture blows up or the frames stay frozen, reopen the session (full init) and retry.
        frames, saw_weak = None, False
        for attempt in (1, 2):
            try:
                with contextlib.redirect_stdout(_devnull):
                    _session["sensor"].arm()        # re-arm FDT/scan: wakes the sensor
                frames, saw_weak = _capture_burst(_session["sensor"], _session["clear"], n_frames, wait_s)
            except (Exception, SystemExit) as e:
                log(f"capture failed (attempt {attempt}: {type(e).__name__}): reopening the session")
                if attempt == 2:
                    _close_session()
                    return {"ok": False, "reason": f"sensor error: {str(e) or type(e).__name__}"}
                err = _reopen_or_fail()
                if err:
                    return err
                continue
            if frames and _frozen(frames):          # sensor asleep: frozen frames
                if attempt == 2:
                    return {"ok": False, "reason": "sensor asleep: try again"}
                log("frozen frames (sensor asleep): reopening the session and retrying")
                err = _reopen_or_fail()
                if err:
                    return err
                continue
            break

        if not frames:
            return {"ok": False,
                    "reason": ("weak contact: press more firmly" if saw_weak
                               else "no finger detected")}

        # 3) match: accept if a frame exceeds the threshold on any fingerprint
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

        res = {"ok": False, "reason": "no match",
               "best": best["score"], "finger": best["finger"]}
        if LOG_VERIFY and rec:
            _save_verify_log(user, _session["clear"], rec, res)
        return res


def _save_verify_log(user, clear, rec, result):
    """Saves the frames of a verify (raw + PNG + score) under STATE_DIR/verify_log (writable by the
    service). Only with GOODIXFP_LOG=1."""
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
        log(f"  [log] saving failed: {type(e).__name__}: {e}")


# ---------------------------------------------------------------- user templates
def load_templates(user):
    """Loads all of the user's enrolled fingerprints as FragmentMapper objects. [] if none."""
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
                log(f"template {fn} cannot be loaded: {e}")
    return out


# ----------------------------------------------------------------- socket server
def _peer_uid(conn):
    creds = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
    _pid, uid, _gid = struct.unpack("3i", creds)
    return uid


def _authorized(peer_uid, user):
    if peer_uid == 0:
        return True            # the PAM stack (login/sudo) runs as root
    try:
        return peer_uid == pwd.getpwnam(user).pw_uid   # the user may test themselves
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
            conn.sendall(b'{"ok": false, "reason": "invalid request"}\n')
            return
        if not _authorized(_peer_uid(conn), user):
            conn.sendall(b'{"ok": false, "reason": "not authorized"}\n')
            return
        log(f"verify user={user} (peer uid {_peer_uid(conn)})")
        res = verify_user(user, int(req.get("frames", N_FRAMES)))
        log(f"  -> {res}")
        conn.sendall((json.dumps(res) + "\n").encode())
    except (Exception, SystemExit) as e:
        import traceback
        log(f"  !! error: {type(e).__name__}: {e}")
        log("  !! " + traceback.format_exc().replace("\n", " | "))
        try:
            conn.sendall((json.dumps({"ok": False, "reason": f"{type(e).__name__}: {e}"}) + "\n").encode())
        except Exception:
            pass
    finally:
        conn.close()


def _shutdown(signum, _frame):
    log(f"signal {signum}: closing the sensor session and exiting")
    _close_session()           # best effort: clean teardown so the sensor is not left in a wedge
    sys.exit(0)


def main():
    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)
    os.makedirs(os.path.dirname(SOCKET_PATH), exist_ok=True)
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o660)   # root:goodixfp (systemd/RuntimeDirectory sets the group)
    srv.listen(4)
    log(f"listening on {SOCKET_PATH}  (state={STATE_DIR}, frames={N_FRAMES}, threshold={THRESHOLD}, "
        f"minforce={MIN_FORCE}, warm session)")
    while True:
        conn, _ = srv.accept()
        # serial verify (one sensor only): one thread per connection, but _hw_lock serializes.
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
