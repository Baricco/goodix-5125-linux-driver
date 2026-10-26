# keyring-tpm — GNOME keyring auto-unlock via TPM (optional)

When you log in with the **fingerprint** instead of the password,
`pam_gnome_keyring` does not receive the password and the GNOME keyring stays
locked: the first program that uses it asks for the password. This component
solves it **the way Windows does with the TPM**: the keyring password is
**sealed in the TPM**, bound to the **Secure Boot state (PCR 7)**, and an
unlock step at login retrieves it from the TPM.

It is **optional** and **independent** of the fingerprint driver: install it
or not.

## How it works

1. The keyring password (= your login password) is encrypted with
   `systemd-creds -T --tpm2-pcrs=7` → file `~/.local/share/goodixfp/keyring-tpm.cred`.
   `-T` = TPM only (no host key), so a user in the `tss` group can decrypt it.
   It is bound to PCR 7: if someone boots another OS, the TPM does not release
   it.
2. At login, a systemd user service (`goodix-keyring-unlock.service`) unseals
   the password from the TPM and unlocks the keyring through the D-Bus Secret
   Service (`UnlockWithMasterPassword`). It runs **before the graphical
   session** (`Before=graphical-session-pre.target`), so the shell and the
   apps find the keyring already open and no prompt appears; and **at every
   (re)start of `gnome-keyring-daemon`**, because the unlocked state lives only
   in the daemon's memory and a restart would lock it again.
3. A polkit rule allows `systemd-creds decrypt` without a prompt even while
   the session is not yet "active" for polkit.

**Safe fallback:** if the TPM refuses (e.g. after a change of Secure Boot state
or keys → different PCR 7), the unlock fails silently and the keyring asks for
the password as usual. The keyring password stays your login password → **never
a lockout**. After a PCR 7 change, just run `seal.sh` again.

## Requirements

- **Secure Boot ENABLED** (otherwise PCR 7 protects nothing: a live USB with
  SB off would reproduce the same value). See `../../docs/secure-boot.md`.
- TPM 2.0 (`/dev/tpmrm0`), `systemd-creds`, `gnome-keyring`, `python3`+`dbus`.

## Installation

```sh
./install.sh      # as a normal user (uses sudo where needed)
# then a full LOGOUT + login (activates the tss group)
```

Check after logging in: `journalctl --user -b -u goodix-keyring-unlock` must
report `keyring unlocked`.

Uninstall: `./uninstall.sh`.

## Files

- `gkr_unlock.py` — unlocks the keyring through D-Bus (password read from stdin).
- `keyring-unlock.sh` — login hook: unseals from the TPM + calls the unlocker.
- `goodix-keyring-unlock.service` — user service (at login, before the
  graphical session, and at every start of `gnome-keyring-daemon.service`).
- `seal.sh` — (re)creates the seal in the TPM.
- `49-goodix-keyring-creds.rules.in` — polkit rule (template, `@USER@`).

## Security note

The polkit rule lets the user decrypt system credentials without
authentication. On a single-user machine where the user already has `sudo`
this is not a real escalation. On multi-user systems consider restricting it or
avoiding it.
