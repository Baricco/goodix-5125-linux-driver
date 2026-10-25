#!/usr/bin/env python3
# test-dbus.py - goodixfpd contro i client veri dell'API fprintd.
#
# Avvia un bus di sistema privato, un polkit finto e goodixfpd-test (il
# servizio con il sensore simulato), poi lo usa come fanno pam_fprintd, i
# comandi fprintd-* e la GUI: chiamate D-Bus dirette, fprintd-list/-verify,
# pam_fprintd.so vero tramite pam-harness. Le "dita" sono immagini sintetiche
# a creste generate qui (nessun dato biometrico nel repository).
#
#   test-dbus.py <goodixfpd-test> <pam-harness> <system-bus.conf> <mock-polkit.py>
#
# SPDX-License-Identifier: GPL-2.0-or-later
import os
import pwd
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

try:
    import numpy as np
    from gi.repository import Gio, GLib
except ImportError as e:
    print(f"SKIP: {e}")
    sys.exit(77)

DAEMON, HARNESS, BUS_CONF, MOCK = sys.argv[1:5]
USER = pwd.getpwuid(os.getuid()).pw_name
NAME = "net.reactivated.Fprint"
DEV = "/net/reactivated/Fprint/Device/0"
DEV_IFACE = "net.reactivated.Fprint.Device"
TIMEOUT = 15

# ---------------------------------------------------------------- immagini


def smooth(a, k):
    f = np.fft.rfft2(a)
    ky = np.fft.fftfreq(a.shape[0])[:, None]
    kx = np.fft.rfftfreq(a.shape[1])[None, :]
    return np.fft.irfft2(f * np.exp(-(kx ** 2 + ky ** 2) * k), s=a.shape)


def finger(seed, h=160, w=300):
    """Campo di creste con orientamento e fase che variano piano."""
    r = np.random.default_rng(seed)
    th = smooth(r.standard_normal((h, w)), 4000)
    th = th / th.std() * 1.2
    ph = smooth(r.standard_normal((h, w)), 3000)
    ph = ph / ph.std() * 3
    y, x = np.mgrid[0:h, 0:w]
    img = np.cos(2 * np.pi * (x * np.cos(th) + y * np.sin(th)) / 9.0 + ph)
    img += 0.25 * smooth(r.standard_normal((h, w)), 30)
    return (img - img.min()) / (img.max() - img.min()) * 255


class Images:
    def __init__(self, d):
        self.d = d
        self.fingers = {k: finger(s) for k, s in (("A", 1), ("B", 2), ("C", 3))}

    def get(self, f, ox, oy):
        path = os.path.join(self.d, f"{f}-{ox}-{oy}.pgm")
        if not os.path.exists(path):
            px = self.fingers[f][oy:oy + 64, ox:ox + 80].astype(np.uint8)
            with open(path, "wb") as fh:
                fh.write(b"P5\n80 64\n255\n" + px.tobytes())
        return path


ENROLL_AT = [(ox, oy) for oy in (10, 40, 70) for ox in (10, 30, 50, 70, 90)]

# ---------------------------------------------------------------- ambiente


class Env:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="gxd-dbus-")
        self.addr = f"unix:path={self.tmp}/bus"
        self.deny = os.path.join(self.tmp, "polkit-deny")
        self.polkit_log = os.path.join(self.tmp, "polkit.log")
        self.state = os.path.join(self.tmp, "state")
        self.sock = os.path.join(self.tmp, "sensor.sock")
        self.images = Images(os.path.join(self.tmp, "img"))
        os.makedirs(self.images.d)
        os.environ["DBUS_SYSTEM_BUS_ADDRESS"] = self.addr
        self.procs = []
        self.procs.append(subprocess.Popen(
            ["dbus-daemon", f"--config-file={BUS_CONF}", f"--address={self.addr}",
             "--nofork", "--nopidfile"], stdout=subprocess.DEVNULL))
        self.wait_for(lambda: os.path.exists(f"{self.tmp}/bus"))
        mock = subprocess.Popen([sys.executable, MOCK, self.deny, self.polkit_log],
                                stdout=subprocess.PIPE, text=True)
        self.procs.append(mock)
        assert mock.stdout.readline().strip() == "ready"
        self.daemon_log = open(os.path.join(self.tmp, "daemon.log"), "w")
        self.start_daemon()
        self.bus = self.connect()

    def start_daemon(self):
        env = dict(os.environ, G_MESSAGES_DEBUG="goodixfpd")
        self.daemon = subprocess.Popen(
            [DAEMON, f"--state-dir={self.state}", f"--test-socket={self.sock}"],
            stdout=self.daemon_log, stderr=subprocess.STDOUT, env=env)
        self.wait_for(lambda: self.name_has_owner())
        self.sensor = socket.socket(socket.AF_UNIX)
        self.sensor.connect(self.sock)
        self.sensor.settimeout(TIMEOUT)

    def name_has_owner(self):
        try:
            c = Gio.DBusConnection.new_for_address_sync(
                self.addr, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
                Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
            r = c.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "NameHasOwner", GLib.Variant("(s)", (NAME,)), None, 0, -1, None)
            c.close_sync(None)
            return r.unpack()[0]
        except GLib.Error:
            return False

    def connect(self):
        return Gio.DBusConnection.new_for_address_sync(
            self.addr, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    @staticmethod
    def wait_for(cond, timeout=TIMEOUT):
        end = time.monotonic() + timeout
        ctx = GLib.MainContext.default()
        while not cond():
            if time.monotonic() > end:
                raise AssertionError("timeout")
            ctx.iteration(False)
            time.sleep(0.01)

    def touch(self, *cmd):
        """Un comando al sensore simulato; torna quando il servizio lo ha letto."""
        line = " ".join(str(c) for c in cmd) + "\n"
        self.sensor.sendall(line.encode())
        buf = b""
        while not buf.endswith(b"\n"):
            buf += self.sensor.recv(16)
        assert buf == b"ok\n", buf

    def image(self, f, ox, oy):
        self.touch("image", self.images.get(f, ox, oy))

    def set_denied(self, *actions):
        with open(self.deny, "w") as fh:
            fh.write("\n".join(actions) + "\n")

    def close(self):
        self.daemon.send_signal(signal.SIGTERM)
        try:
            rc = self.daemon.wait(10)
        except subprocess.TimeoutExpired:
            self.daemon.kill()
            rc = "killed"
        for p in self.procs:
            p.terminate()
            p.wait()
        self.daemon_log.close()
        return rc


CLIENTS = []


class Client:
    """Un processo client sul bus: chiamate e segnali del dispositivo."""

    def __init__(self, env):
        self.env = env
        self.bus = env.connect()
        self.signals = []
        self.bus.signal_subscribe(NAME, DEV_IFACE, None, DEV, None, 0, self._on_signal)
        CLIENTS.append(self)

    def _on_signal(self, conn, sender, path, iface, name, params):
        self.signals.append((name, params.unpack()))

    def call(self, method, sig=None, *args):
        params = GLib.Variant(f"({sig})", args) if sig else None
        r = self.bus.call_sync(NAME, DEV, DEV_IFACE, method, params, None, 0, 30000, None)
        return r.unpack() if r else None

    def fails(self, err, method, sig=None, *args):
        try:
            self.call(method, sig, *args)
        except GLib.Error as e:
            name = Gio.DBusError.get_remote_error(e)
            assert name == f"net.reactivated.Fprint.Error.{err}", (name, e.message)
            return e.message
        raise AssertionError(f"{method} should fail with {err}")

    def prop(self, name):
        r = self.bus.call_sync(NAME, DEV, "org.freedesktop.DBus.Properties", "Get",
                               GLib.Variant("(ss)", (DEV_IFACE, name)), None, 0, -1, None)
        return r.unpack()[0]

    def next_status(self, kind):
        """Il prossimo VerifyStatus/EnrollStatus: (risultato, done)."""
        def has():
            return any(n == kind for n, _ in self.signals)
        Env.wait_for(has)
        for i, (n, args) in enumerate(self.signals):
            if n == kind:
                del self.signals[i]
                return args
        raise AssertionError

    def drain(self):
        ctx = GLib.MainContext.default()
        for _ in range(20):
            ctx.iteration(False)
            time.sleep(0.005)
        self.signals.clear()

    def close(self):
        if not self.bus.is_closed():
            self.bus.close_sync(None)


# ---------------------------------------------------------------- test

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def enroll(env, c, finger_name, f, dup_check=None):
    """Registrazione completa: tocco di controllo, 15 posizioni, 4 ripetuti."""
    c.drain()
    c.call("EnrollStart", "s", finger_name)
    env.image(*(dup_check or (f, 10, 10)))
    assert c.next_status("EnrollStatus") == ("enroll-stage-passed", False)
    for ox, oy in ENROLL_AT:
        env.image(f, ox, oy)
        assert c.next_status("EnrollStatus") == ("enroll-stage-passed", False)
    for _ in range(3):
        env.image(f, 50, 40)
        assert c.next_status("EnrollStatus") == ("enroll-retry-scan", False)
    env.image(f, 50, 40)
    assert c.next_status("EnrollStatus") == ("enroll-completed", True)
    c.call("EnrollStop")


def verify(env, c, finger_name, image, expect_selected=None):
    c.drain()
    c.call("VerifyStart", "s", finger_name)
    env.wait_for(lambda: any(n == "VerifyFingerSelected" for n, _ in c.signals))
    selected = [a for n, a in c.signals if n == "VerifyFingerSelected"][0][0]
    if expect_selected:
        assert selected == expect_selected, selected
    env.image(*image)
    res = c.next_status("VerifyStatus")
    c.call("VerifyStop")
    return res


@test
def manager_and_properties(env):
    m = env.bus.call_sync(NAME, "/net/reactivated/Fprint/Manager", "net.reactivated.Fprint.Manager",
                          "GetDevices", None, None, 0, -1, None).unpack()
    assert m == ([DEV],), m
    d = env.bus.call_sync(NAME, "/net/reactivated/Fprint/Manager", "net.reactivated.Fprint.Manager",
                          "GetDefaultDevice", None, None, 0, -1, None).unpack()
    assert d == (DEV,), d
    c = Client(env)
    assert c.prop("name") == "Goodix fingerprint (27c6:5125/5135)"
    assert c.prop("scan-type") == "press"
    assert c.prop("num-enroll-stages") == 31
    assert c.prop("finger-present") is False
    om = env.bus.call_sync(NAME, "/net/reactivated/Fprint", "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", None, None, 0, -1, None).unpack()[0]
    assert DEV in om and DEV_IFACE in om[DEV]
    c.close()


@test
def errors_without_claim(env):
    c = Client(env)
    c.fails("NoEnrolledPrints", "ListEnrolledFingers", "s", "")
    c.fails("ClaimDevice", "VerifyStart", "s", "any")
    c.fails("ClaimDevice", "Release")
    c.close()


@test
def claim_release_and_busy(env):
    a, b = Client(env), Client(env)
    a.call("Claim", "s", "")
    b.fails("AlreadyInUse", "Claim", "s", "")
    b.fails("AlreadyInUse", "VerifyStart", "s", "any")
    a.fails("NoEnrolledPrints", "VerifyStart", "s", "any")
    a.fails("InvalidFingername", "EnrollStart", "s", "thumb")
    a.fails("NoActionInProgress", "VerifyStop")
    a.call("Release")
    b.call("Claim", "s", "")
    b.call("Release")
    a.close()
    b.close()


@test
def enroll_and_list(env):
    c = Client(env)
    c.call("Claim", "s", "")
    c.drain()
    c.call("EnrollStart", "s", "right-index-finger")
    c.fails("AlreadyInUse", "VerifyStart", "s", "any")
    assert c.prop("finger-needed") is True
    env.touch("retry")
    assert c.next_status("EnrollStatus") == ("enroll-retry-scan", False)
    c.call("EnrollStop")
    assert c.next_status("EnrollStatus") == ("enroll-failed", True)

    enroll(env, c, "right-index-finger", "A")
    stages = c.prop("num-enroll-stages")
    assert stages == 17, stages          # 15 posizioni + chiusura + controllo
    assert c.call("ListEnrolledFingers", "s", "") == (["right-index-finger"],)
    c.call("Release")
    c.close()
    path = os.path.join(env.state, "goodixfpd", USER, "right-index-finger")
    st = os.stat(path)
    assert st.st_mode & 0o777 == 0o600, oct(st.st_mode)


@test
def verify_match_nomatch_retry(env):
    c = Client(env)
    c.call("Claim", "s", "")
    assert verify(env, c, "any", ("A", 53, 43), "right-index-finger") == ("verify-match", True)
    assert verify(env, c, "right-index-finger", ("C", 50, 40)) == ("verify-no-match", True)
    c.drain()
    c.call("VerifyStart", "s", "any")
    env.touch("retry")
    assert c.next_status("VerifyStatus") == ("verify-retry-scan", False)
    env.image("A", 47, 37)
    assert c.next_status("VerifyStatus") == ("verify-match", True)
    c.fails("AlreadyInUse", "VerifyStart", "s", "any")   # prima serve VerifyStop
    c.call("VerifyStop")
    c.fails("NoEnrolledPrints", "VerifyStart", "s", "left-thumb")
    c.call("Release")
    c.close()


@test
def verify_stop_before_result(env):
    c = Client(env)
    c.call("Claim", "s", "")
    c.drain()
    c.call("VerifyStart", "s", "any")
    c.call("VerifyStop")
    assert c.next_status("VerifyStatus") == ("verify-no-match", True)
    c.call("Release")
    c.close()


@test
def learning_extends_coverage(env):
    path = os.path.join(env.state, "goodixfpd", USER, "right-index-finger")
    c = Client(env)
    c.call("Claim", "s", "")
    assert verify(env, c, "any", ("A", 170, 40))[0] == "verify-no-match"
    size = os.path.getsize(path)
    assert verify(env, c, "any", ("A", 150, 40))[0] == "verify-match"
    assert os.path.getsize(path) > size, "the print did not grow"
    assert verify(env, c, "any", ("A", 170, 40))[0] == "verify-match"
    c.call("Release")
    c.close()


@test
def duplicates_and_identify(env):
    c = Client(env)
    c.call("Claim", "s", "")
    c.drain()
    c.call("EnrollStart", "s", "left-thumb")
    env.image("A", 53, 43)                       # dito gia' registrato
    assert c.next_status("EnrollStatus") == ("enroll-duplicate", True)
    c.call("EnrollStop")
    assert c.call("ListEnrolledFingers", "s", "") == (["right-index-finger"],)
    enroll(env, c, "left-thumb", "B", dup_check=("B", 30, 70))
    assert sorted(c.call("ListEnrolledFingers", "s", "")[0]) == ["left-thumb", "right-index-finger"]
    assert verify(env, c, "any", ("B", 33, 43), "any") == ("verify-match", True)
    assert verify(env, c, "any", ("C", 40, 40), "any") == ("verify-no-match", True)
    c.call("Release")
    c.close()


@test
def client_vanishes(env):
    a = Client(env)
    a.call("Claim", "s", "")
    a.call("VerifyStart", "s", "any")
    a.close()                                    # come pam_fprintd dopo un successo
    b = Client(env)
    env.wait_for(lambda: try_claim(b))
    b.call("Release")
    b.close()


def try_claim(c):
    try:
        c.call("Claim", "s", "")
        return True
    except GLib.Error:
        return False


@test
def release_during_enroll(env):
    c = Client(env)
    c.call("Claim", "s", "")
    c.drain()
    c.call("EnrollStart", "s", "right-thumb")
    c.call("Release")
    assert c.next_status("EnrollStatus") == ("enroll-failed", True)
    c.call("Claim", "s", "")
    c.fails("NoEnrolledPrints", "VerifyStart", "s", "right-thumb")
    c.call("Release")
    c.close()


@test
def sensor_lost(env):
    c = Client(env)
    c.call("Claim", "s", "")
    c.drain()
    c.call("VerifyStart", "s", "any")
    env.touch("lost")
    assert c.next_status("VerifyStatus") == ("verify-disconnected", True)
    c.call("VerifyStop")
    c.call("Release")
    c.close()


@test
def polkit(env):
    c = Client(env)
    env.set_denied("net.reactivated.fprint.device.setusername")
    c.fails("PermissionDenied", "Claim", "s", "someone-else")
    c.fails("PermissionDenied", "ListEnrolledFingers", "s", "someone-else")
    env.set_denied("net.reactivated.fprint.device.enroll")
    c.call("Claim", "s", "")                     # basta verify
    c.fails("PermissionDenied", "EnrollStart", "s", "left-index-finger")
    c.fails("PermissionDenied", "DeleteEnrolledFinger", "s", "left-thumb")
    c.call("Release")
    env.set_denied("net.reactivated.fprint.device.verify", "net.reactivated.fprint.device.enroll")
    c.fails("PermissionDenied", "Claim", "s", "")
    env.set_denied()
    c.close()


@test
def fprintd_cli(env):
    out = subprocess.run(["fprintd-list", USER], capture_output=True, text=True, timeout=30)
    assert "right-index-finger" in out.stdout and "left-thumb" in out.stdout, out.stdout + out.stderr
    p = subprocess.Popen(["fprintd-verify", "-f", "right-index-finger", USER],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    env.image("A", 50, 43)
    out = p.communicate(timeout=30)[0]
    assert "verify-match" in out and p.returncode == 0, out


def pam(env, images, max_tries=2):
    confdir = os.path.join(env.tmp, "pam.d")
    os.makedirs(confdir, exist_ok=True)
    with open(os.path.join(confdir, "gxd-test"), "w") as fh:
        fh.write(f"auth required pam_fprintd.so max-tries={max_tries} timeout=20\n")
    p = subprocess.Popen([HARNESS, "gxd-test", USER, confdir], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    lines = []
    for img in images:
        # aspetta "Place your ... finger on ..." prima di toccare
        while True:
            line = p.stdout.readline()
            assert line, "".join(lines)
            lines.append(line)
            if line.startswith("info: ") and "finger" in line:
                break
        env.image(*img)
    lines += p.stdout.readlines()
    p.wait(30)
    return p.returncode, "".join(lines)


@test
def pam_fprintd(env):
    if not os.path.exists("/usr/lib/security/pam_fprintd.so"):
        print("  (pam_fprintd.so assente, salto)")
        return
    rc, out = pam(env, [("A", 53, 43)])
    assert rc == 0 and "result: PAM_SUCCESS" in out, out
    rc, out = pam(env, [("C", 50, 40), ("C", 60, 40)])
    assert "Failed to match fingerprint" in out and "result: PAM_MAXTRIES" in out, out
    rc, out = pam(env, [("C", 50, 40), ("A", 47, 37)])   # secondo tentativo buono
    assert rc == 0, out


@test
def delete(env):
    c = Client(env)
    c.call("Claim", "s", "")
    c.call("DeleteEnrolledFinger", "s", "left-thumb")
    c.fails("NoEnrolledPrints", "DeleteEnrolledFinger", "s", "left-thumb")
    assert c.call("ListEnrolledFingers", "s", "") == (["right-index-finger"],)
    c.call("DeleteEnrolledFingers2")
    c.fails("NoEnrolledPrints", "ListEnrolledFingers", "s", "")
    c.call("Release")
    c.close()


@test
def restart_keeps_prints(env):
    c = Client(env)
    c.call("Claim", "s", "")
    enroll(env, c, "right-middle-finger", "A")
    c.call("Release")
    c.close()
    env.sensor.close()
    assert env.close_daemon() == 0
    env.start_daemon()
    c = Client(env)
    c.call("Claim", "s", "")
    assert verify(env, c, "any", ("A", 53, 43)) == ("verify-match", True)
    c.call("Release")
    c.close()


def close_daemon(self):
    self.daemon.send_signal(signal.SIGTERM)
    return self.daemon.wait(10)


Env.close_daemon = close_daemon


def main():
    env = Env()
    failed = 0
    try:
        for t in TESTS:
            try:
                t(env)
                print(f"ok - {t.__name__}", flush=True)
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL - {t.__name__}: {type(e).__name__}: {e}", flush=True)
                if env.daemon.poll() is not None:
                    print("  servizio terminato!", flush=True)
                    break
            # client rimasti aperti (test fallito): chiusi, il servizio deve
            # liberare il dispositivo come per un client sparito
            for c in CLIENTS:
                c.close()
            CLIENTS.clear()
            probe = Client(env)
            env.wait_for(lambda: try_claim(probe))
            probe.call("Release")
            probe.close()
            CLIENTS.clear()
    finally:
        rc = env.close()
        if failed or rc != 0:
            print(f"--- log del servizio (exit {rc}) ---")
            print(open(os.path.join(env.tmp, "daemon.log")).read()[-6000:])
            print("--- polkit ---")
            if os.path.exists(env.polkit_log):
                print(open(env.polkit_log).read()[-2000:])
        else:
            shutil.rmtree(env.tmp, ignore_errors=True)
    sys.exit(1 if failed or rc != 0 else 0)


if __name__ == "__main__":
    main()
