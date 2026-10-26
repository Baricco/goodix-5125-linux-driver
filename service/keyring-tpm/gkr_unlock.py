#!/usr/bin/env python3
"""Unlocks gnome-keyring's 'login' keyring through the D-Bus Secret Service,
using the master password read from stdin (exact bytes, no stripping).
Method: org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface.UnlockWithMasterPassword
with a 'plain' session (password in clear text on the local session bus).

At login the daemon may not be on the bus yet: it retries until it answers
(at most WAIT_S). Method calls only: a property read that comes too early
makes gnome-keyring abort (see keyring-unlock.sh).
"""
import sys
import time

import dbus

COLLECTION = "/org/freedesktop/secrets/collection/login"
WAIT_S = 5.0

# "Daemon not ready yet" errors: name not owned, objects not exported yet,
# collection not loaded yet. Any other error (e.g. a wrong password) fails
# right away, without making the login longer.
RETRY = {
    "org.freedesktop.DBus.Error.NoServer",
    "org.freedesktop.DBus.Error.FileNotFound",
    "org.freedesktop.DBus.Error.ServiceUnknown",
    "org.freedesktop.DBus.Error.NameHasNoOwner",
    "org.freedesktop.DBus.Error.NoReply",
    "org.freedesktop.DBus.Error.TimedOut",
    "org.freedesktop.DBus.Error.UnknownObject",
    "org.freedesktop.DBus.Error.UnknownInterface",
    "org.freedesktop.DBus.Error.UnknownMethod",
    "org.freedesktop.Secret.Error.NoSuchObject",
}


def unlock(pw: bytes) -> None:
    bus = dbus.SessionBus()
    svc = bus.get_object("org.freedesktop.secrets", "/org/freedesktop/secrets",
                         introspect=False)
    service = dbus.Interface(svc, "org.freedesktop.Secret.Service")
    # 'plain' session: the master password travels in clear text (local bus only).
    _output, session = service.OpenSession("plain", dbus.String("", variant_level=1))
    secret = dbus.Struct(
        (
            dbus.ObjectPath(session),
            dbus.Array([], signature="y"),   # parameters
            dbus.ByteArray(pw),              # value = master password
            dbus.String("text/plain"),       # content type
        )
    )
    gr = dbus.Interface(
        svc, "org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface"
    )
    gr.UnlockWithMasterPassword(dbus.ObjectPath(COLLECTION), secret)


def main() -> int:
    pw = sys.stdin.buffer.read()
    if not pw:
        print("gkr_unlock: empty password on stdin", file=sys.stderr)
        return 1
    deadline = time.monotonic() + WAIT_S
    while True:
        try:
            unlock(pw)
            return 0
        except dbus.DBusException as exc:
            name = exc.get_dbus_name() or ""
            retry = name in RETRY or name.startswith("org.freedesktop.DBus.Error.Spawn.")
            if not retry or time.monotonic() >= deadline:
                raise
        time.sleep(0.1)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 - the caller falls back silently
        print(f"gkr_unlock: {exc}", file=sys.stderr)
        sys.exit(1)
