#!/usr/bin/env python3
"""Sblocca il keyring 'login' di gnome-keyring via D-Bus Secret Service,
usando la master password letta da stdin (byte esatti, nessuno strip).
Metodo: org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface.UnlockWithMasterPassword
con una sessione 'plain' (password in chiaro sul bus di sessione locale).
"""
import sys
import dbus

COLLECTION = "/org/freedesktop/secrets/collection/login"


def main() -> int:
    pw = sys.stdin.buffer.read()
    if not pw:
        print("gkr_unlock: password vuota su stdin", file=sys.stderr)
        return 1
    bus = dbus.SessionBus()
    svc = bus.get_object("org.freedesktop.secrets", "/org/freedesktop/secrets")
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
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 - fallback silenzioso lato chiamante
        print(f"gkr_unlock: {exc}", file=sys.stderr)
        sys.exit(1)
