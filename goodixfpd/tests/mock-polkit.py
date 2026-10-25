#!/usr/bin/env python3
# mock-polkit.py - org.freedesktop.PolicyKit1 finto per i test.
#
# Autorizza tutto tranne le azioni elencate (una per riga) nel file passato
# come primo argomento, riletto a ogni richiesta. Le richieste vanno nel
# file di log passato come secondo argomento.
#
# SPDX-License-Identifier: GPL-2.0-or-later
import sys

from gi.repository import Gio, GLib

XML = """
<node>
  <interface name="org.freedesktop.PolicyKit1.Authority">
    <method name="CheckAuthorization">
      <arg type="(sa{sv})" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="a{ss}" direction="in"/>
      <arg type="u" direction="in"/>
      <arg type="s" direction="in"/>
      <arg type="(bba{ss})" direction="out"/>
    </method>
    <method name="CancelCheckAuthorization">
      <arg type="s" direction="in"/>
    </method>
  </interface>
</node>
"""

deny_file, log_file = sys.argv[1], sys.argv[2]


def denied():
    try:
        with open(deny_file) as f:
            return {line.strip() for line in f if line.strip()}
    except FileNotFoundError:
        return set()


def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "CheckAuthorization":
        action = params.unpack()[1]
        ok = action not in denied()
        with open(log_file, "a") as f:
            f.write(f"{action} {'yes' if ok else 'no'}\n")
        invocation.return_value(GLib.Variant("((bba{ss}))", ((ok, False, {}),)))
    else:
        invocation.return_value(None)


def on_bus(conn, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
    conn.register_object("/org/freedesktop/PolicyKit1/Authority", info, on_call, None, None)


def on_name(conn, name):
    print("ready", flush=True)


Gio.bus_own_name(Gio.BusType.SYSTEM, "org.freedesktop.PolicyKit1",
                 Gio.BusNameOwnerFlags.NONE, on_bus, on_name, None)
GLib.MainLoop().run()
