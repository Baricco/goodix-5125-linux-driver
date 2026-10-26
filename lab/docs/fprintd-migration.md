# Path C — migration to the standard stack: `fprintd` + the `goodixgf` driver

Runbook to bring the sensor into the standard Linux stack (`fprintd` + `pam_fprintd`), giving
fingerprint login on **sudo + greetd/noctalia login + lock screen**, with the UX "touch **or**
type the password". It replaces our `goodixfpd` daemon (which stays as a lab/RE tool).

Driver adopted: **`goodixgf`** from [Rockytkg/goodix-linux-27c6-5125](https://github.com/Rockytkg/goodix-linux-27c6-5125)
on the fork [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint)
(branch `libfprint-sigfm`, **SIGFM** matching like ours). The installer supports CachyOS/Arch.

## Safety — what gets written to the sensor

- **Firmware: NOT flashed.** Checked in the code (`src/goodix_fwupdate.c`): it bundles
  `GF_FW_VERSION="GF_ST411SEC_APP_12508"` = ours, and `strncmp(ver, GF_FW_VERSION)==0 -> return 1`
  (skip). Our chip is 12508 → no flashing. There is also an anti-brick CRC pre-check.
- **The only write: the PSK.** The first time `fprintd` opens the device (the first enrollment),
  the driver provisions its own PSK again (E0 command, store sector only, reversible). **It
  overwrites ours**, `state/linux_psk.bin`. It is "the write we approve explicitly".
- **Backup of our PSK:** `state/linux_psk.bin` and a copy in `state/old_maps/linux_psk.bin`. Do not
  touch them: they are needed only to go back to the Python daemon.
- The usual rule: keep a **root shell open** (`sudo -i` in another terminal) while you edit
  `/etc/pam.d/*`.

## 0. Put our stack aside (exclusive device: ONE daemon only on the sensor)

```fish
sudo systemctl disable --now goodixfpd
sudo cp /etc/pam.d/sudo.bak /etc/pam.d/sudo   # removes our pam_exec line (restores the original)
```

## 1. Missing dependencies (the rest of the toolchain is already there)

```fish
sudo pacman -S --needed opencv doctest meson fprintd gobject-introspection libgusb gtk-doc mbedtls glib2-devel
```
Notes on names/packages:
- GUsb on Arch/CachyOS is `libgusb` (not `gusb`).
- **`glib2-devel` is required**: recent Arch/CachyOS `glib2` no longer ships
  `glib-mkenums`/`glib-genmarshal` (now in `glib2-devel`); without it the build fails with
  `Program /usr/bin/glib-mkenums found: NO`.
- **`mbedtls` (4.x) is required**: the driver uses the mbedtls 4.x API and the build does
  `find_library('mbedtls')` → it looks for `/usr/lib/libmbedtls.so`. Having only `mbedtls3`
  (3.x LTS) is NOT enough: it exposes the unversioned library only in `/usr/lib/mbedtls3/`, and the
  build fails with `ERROR: C shared or static library 'mbedtls' not found`. `mbedtls` and
  `mbedtls3` **coexist** (separate headers: `/usr/include/mbedtls/` vs `/usr/include/mbedtls3/`);
  do not remove `mbedtls3` (`librist` needs it).
- Already installed: `ninja gcc pkgconf python openssl glib2 nss pixman libgudev systemd-libs`.

## 2. Clone and install the driver

```fish
cd ~
git clone --recurse-submodules https://github.com/Rockytkg/goodix-linux-27c6-5125
cd goodix-linux-27c6-5125
set -x GOODIX_LINUX $PWD        # (bash: export GOODIX_LINUX="$PWD")
bash install-goodixgf-en.sh
```
The script (it asks for the password for the `sudo` steps): installs the dependencies, copies
`goodixgf` + the protocol core into the fork, patches `meson.build` (idempotent), **builds and
installs into `/usr`** (unchanged SONAME → the system `fprintd` reuses it), installs the udev rule
`70-goodix.rules`, adds a **`--no-timeout`** drop-in to `fprintd` (essential: otherwise fprintd
exits after ~30 s and at login the screen does not see the sensor), reloads udev and restarts
`fprintd`.

## 3. Enrollment — the PSK provisioning happens here (give your OK)

On the first open the driver writes the new PSK (no firmware flashing). Enroll **densely**:

```fish
journalctl -u fprintd -b | grep -i goodix  # confirms that goodixgf opens the device
fprintd-delete $USER                       # cleans previous enrollments (fine if there are none)
fprintd-enroll -f right-index-finger $USER # finger OFF the sensor at first open (PSK provisioning + baseline),
                                           # then place it when asked, 3-8 times, moving the area
fprintd-verify $USER                       # repeat a few times
```
Matcher threshold: `GF_SIGFM_SCORE_THRESHOLD=20` in `src/goodixgf.c` (run the installer again
after a change). SIGFM wants ≥ 25 SIFT keypoints per frame, otherwise it asks for a new touch.
Press as you will really unlock (same area/orientation). Expected ~3/4+ recognitions; if lower,
`fprintd-delete` and enroll again more densely. (Our experience: coverage is everything — see
`docs/matching.md`.)

## 4. PAM — fingerprint on sudo, login, lock screen

Always `sufficient` → **password fallback** guaranteed. With the root shell open:

- **sudo** — at the top of the `auth` section of `/etc/pam.d/sudo`:
  ```
  auth sufficient pam_fprintd.so
  ```
- **greetd login** — at the top of the `auth` section of `/etc/pam.d/greetd`:
  ```
  auth sufficient pam_fprintd.so
  ```
- **lock screen** — the same line in your locker's PAM file (swaylock/hyprlock/…).

Test: a new terminal, `sudo -k; sudo -v` → touch; it must pass without a password. Without
touching → password as usual.

## 5. noctalia — password and fingerprint concurrently at login

Your `noctalia-greeter 1.7.0` already supports it (strings `allow_empty_password`,
`build fingerprint`; [Issue #35](https://github.com/noctalia-dev/noctalia-greeter/issues/35),
[PR #158](https://github.com/noctalia-dev/noctalia-greeter/pull/158)). In `greeter.toml`
(`/var/lib/noctalia-greeter/greeter.toml`, managed by `noctalia-greeter-apply-appearance
--setup-system`, owned by the `greeter` user):

```toml
[auth]
allow_empty_password = true
# request_timeout = 60   # optional (0 disables the timeout)
```
**Warning:** the key MUST be **under the `[auth]` header**, not at the root level of the file (an
`allow_empty_password = true` placed before any `[section]` ends up in the root table and is
ignored). Check with `sudo cat .../greeter.toml | grep -A1 '\[auth\]'`.

At login: **touch to get in, or type the password** (a typed password is queued and answers the
prompt only if the fingerprint fails/times out).

## 6. Rollback (if something goes wrong)

```fish
cd ~/goodix-linux-27c6-5125; bash uninstall-goodixgf-en.sh
sudo cp /etc/pam.d/sudo.bak /etc/pam.d/sudo
# remove the pam_fprintd lines from /etc/pam.d/greetd and from the locker by hand
```
Going back to the Python daemon requires pairing OUR PSK with the sensor again (the goodixgf
driver overwrote it): `goodixfpd` stopped → `python lab/driver_5125.py --write` with
`state/linux_psk.bin` → `sudo systemctl enable --now goodixfpd`.

## Notes

- **Dual-boot Windows** rewrites the PSK → enrollment/re-pairing must be redone (true for any
  stack).
- `fprintd` and `goodixfpd` **not together** (device busy): keep only ONE active.
- **Upstream contribution** (later): our measurements (dense enrollment, warm session, waking up
  from idle powerdown, FRR/FAR, `docs/protocol.md`/`matching.md`) to goodix-fp-linux-dev — the
  open value that those (single-tester) projects lack.
