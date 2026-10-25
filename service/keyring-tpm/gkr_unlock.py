#!/usr/bin/env python3
"""Sblocca il keyring 'login' di gnome-keyring via D-Bus Secret Service,
usando la master password letta da stdin (byte esatti, nessuno strip).
Metodo: org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface.UnlockWithMasterPassword
con una sessione 'plain' (password in chiaro sul bus di sessione locale).

Al login il daemon puo' non essere ancora sul bus: si riprova finche' risponde
(max WAIT_S). Solo chiamate di metodo: una lettura di proprieta' troppo precoce
fa abortire gnome-keyring (vedi keyring-unlock.sh).
"""
import sys
import time

import dbus

COLLECTION = "/org/freedesktop/secrets/collection/login"
WAIT_S = 5.0

# Errori da "daemon non ancora pronto": nome non posseduto, oggetti non ancora
# esportati, collezione non ancora caricata. Gli altri (es. password errata)
# falliscono subito, senza allungare il login.
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
    # Sessione 'plain': la master password viaggia in chiaro (solo bus locale).
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
        print("gkr_unlock: password vuota su stdin", file=sys.stderr)
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
    except Exception as exc:  # noqa: BLE001 - fallback silenzioso lato chiamante
        print(f"gkr_unlock: {exc}", file=sys.stderr)
        sys.exit(1)
