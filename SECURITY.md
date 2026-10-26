# Security

Fingerprint login changes *how* you authenticate, not *what* protects your
data. A Windows-like posture takes three independent layers.

## Layer 1 — Fingerprint authentication (this project)

`pam_fprintd` on `sudo`, the greeter and the lock screen. The fingerprint is a
**yes/no gate**: it does not release any key. The rest of the system stays
protected by its own credentials as before. Configured with password fallback
(`auth sufficient`) → no lockout.

The `goodixfpd` service runs like fprintd (root, inside the
`fprintd.service` sandbox) and enforces the same polkit rules: who may verify,
enroll, or act on another user. Fingerprints are SIFT descriptors, not images,
stored in `/var/lib/fprint/goodixfpd/<user>/` (0600 files, 0700 directories);
a file is valid only for the user and finger of its own path. Learning with use
adds only touches recognized with a double margin and never replaces the
enrollment frames (`GOODIX_LEARN=0` turns it off).

## Layer 2 — Keyring unlock via TPM (optional)

`service/keyring-tpm/`. With fingerprint login, `gnome-keyring` does not
receive the password and would stay locked. The keyring password is **sealed
in the TPM**, bound to **Secure Boot (PCR 7)**, and unsealed at login. This is
how Windows avoids asking for credentials again. **It requires Secure Boot to
be enabled** (`docs/secure-boot.md`), otherwise the PCR 7 seal protects
nothing. Fallback: if the TPM refuses, the keyring asks for the password as
usual.

## Layer 3 — Disk encryption (LUKS + TPM) — **recommended, not included**

Fingerprint login and keyring unlock do **not** protect data at rest: whoever
has the laptop can read the disk. Real protection needs **LUKS**, optionally
sealed to the TPM like BitLocker. It is not part of these scripts (a driver
installer must not encrypt someone else's disk): it is done with system tools.

The safest moment is **when installing the system**: every distribution
installer can set up an encrypted root. Converting an existing installation in
place (`cryptsetup reencrypt --encrypt`) is possible, but it rewrites the
whole partition and changes the boot chain: if anything goes wrong the system
no longer boots. Do it only with a **complete, verified backup** and a way to
boot from USB. With TPM auto-unlock (`systemd-cryptenroll --tpm2-pcrs=7`),
always keep a passphrase or recovery key as well: a firmware or Secure Boot
change alters PCR 7 and the TPM stops unlocking.

## Reporting vulnerabilities

Security problems: open a private security advisory on GitHub or contact the
maintainer before any public disclosure.
