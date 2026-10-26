# Guide: Linux fingerprint support for the Goodix 27c6:5125

From the first installation to fingerprint login, step by step. Each section
says what it does and how to check that it worked. If something goes wrong,
go to [Troubleshooting](#9-troubleshooting).

Contents:

1. [Requirements](#1-requirements)
2. [BIOS](#2-bios)
3. [Installing the service](#3-installing-the-service)
4. [Enrolling a fingerprint](#4-enrolling-a-fingerprint)
5. [Fingerprint login (PAM)](#5-fingerprint-login-pam)
6. [Keyring unlock via TPM (optional)](#6-keyring-unlock-via-tpm-optional)
7. [Updates](#7-updates)
8. [Uninstalling](#8-uninstalling)
9. [Troubleshooting](#9-troubleshooting)

How recognition works, and why it is safe: [`docs/how-it-works.md`](how-it-works.md).

---

## 1. Requirements

- **Goodix `27c6:5125` sensor** (or `27c6:5135`). Check with:
  ```sh
  lsusb | grep -i 27c6
  # Bus 003 Device 004: ID 27c6:5125 Shenzhen Goodix Technology Co.,Ltd. FingerPrint
  ```
  It is the one in the power button of Huawei MateBook / Honor MagicBook
  laptops with firmware `GF_ST411SEC_APP_125xx`. Tested on a MateBook
  CREM-WXX9, firmware `12508`, with Arch Linux and CachyOS.
- **Your distribution's `fprintd` and `pam_fprintd`**, as they are
  (`sudo pacman -S fprintd`). The project modifies neither libfprint nor
  fprintd.
- **Arch Linux or a derivative** (CachyOS, EndeavourOS, Manjaro…) for the
  ready-made package. On other distributions you build and install with meson
  (see [below](#other-distributions)): there is no library to replace, so
  distribution updates do not touch it.
- A user with `sudo`.

## 2. BIOS

The fingerprint itself **needs no BIOS changes**. The settings that matter for
the optional security parts (Secure Boot, TPM) and for sensor recovery are
described in [`docs/bios.md`](bios.md).

## 3. Installing the service

The reader is handled by **`goodixfpd`**, a service that speaks fprintd's
D-Bus language (`net.reactivated.Fprint`). A drop-in for `fprintd.service`
starts `goodixfpd` instead of fprintd's daemon. Nothing changes for the rest of
the system: `pam_fprintd`, the `fprintd-*` commands, GNOME, KDE and the
project's GUI keep talking to "fprintd". If the Goodix reader is not present,
`goodixfpd` starts the real fprintd, so other readers keep working.

```sh
git clone https://github.com/Baricco/goodix-5125-linux-driver.git goodix-driver
cd goodix-driver/packaging/goodixfpd
makepkg -si
```

`makepkg` builds, runs the tests and installs. Then:

```sh
fprintd-list "$USER"     # must mention "Goodix fingerprint (27c6:5125/5135)"
```

**If you had the old `libfprint-goodixgf` package**, go back to the
distribution's libfprint first, then install:

```sh
sudo pacman -S libfprint                       # removes libfprint-goodixgf
cd goodix-driver/packaging/goodixfpd && makepkg -si
```

Fingerprints enrolled with the old driver are no longer valid: **enroll them
again** (step 4). The boot warm-up (`goodixgf-warmup.service`) is no longer
needed, the service does it by itself, and the installation disables it.

**What happens the first time the sensor is opened.** The encrypted channel
with the sensor uses a key (PSK) shared by the computer and the sensor. The
service reads the sensor's key back and keeps a copy in
`/var/lib/fprint/goodix/psk.bin` (root only). Only if it cannot read it does it
generate a random key and write it into the sensor. That write touches only the
key area, is reversible and **never modifies the firmware**: the project
contains no firmware and never rewrites it. If Windows later rewrites its own
key (dual boot), Linux notices and repeats this procedure by itself.

Check:
```sh
journalctl -u fprintd -b | grep -E "goodix\].*(PSK|TLS|init complete)|Sensor ready"
# ... PSK valid ...  TLS handshake done ...  === device init complete ===  Sensor ready
```

### Other distributions

You need meson, a C compiler and the development files of GLib, polkit,
libusb and OpenSSL. The path of the real fprintd differs between
distributions: it is the one on the `ExecStart` line of
`systemctl cat fprintd.service` (`/usr/lib/fprintd` on Arch, often
`/usr/libexec/fprintd` elsewhere).

```sh
cd goodix-driver/goodixfpd
meson setup build --prefix=/usr/local -Dfprintd_path=/usr/libexec/fprintd \
      -Dsystemd_dropin_dir=/etc/systemd/system/fprintd.service.d
meson test -C build
sudo meson install -C build
sudo systemctl daemon-reload && sudo systemctl restart fprintd
```

Everything goes to `/usr/local` and `/etc`, where the package manager does not
write: distribution updates do not overwrite it.

## 4. Enrolling a fingerprint

### With the GUI

```sh
sudo pacman -S pyside6
python gui/fingerprint_manager.py
```

Choose the finger, press **Enroll** and follow the instructions. The first
touch checks that the finger is not already enrolled; then it takes **15 to 30
touches**. Place the finger as you do when unlocking, but **lift it completely
after each touch** and move it a little every time (higher, lower, left, right,
slightly rotated): the sensor sees only about 4×3 mm of finger at a time, and
the saved fingerprint is the collection of these areas. A touch in the same
position as one already taken is asked again; when you run out of new
positions (4 repeated touches in a row, after at least 15) enrollment ends by
itself. If "invalid touch" appears, press flatter: the quality check discards
touches that are too light. Use **Test** to check right away.

**The fingerprint improves with use.** When a touch is recognized with margin
but falls at the edge of the enrolled areas, the service adds it to the
fingerprint, as Windows does: the recognized area grows by itself. Enrollment
areas are never replaced; learned ones are at most 30 and are renewed starting
from the oldest. To disable it: `Environment=GOODIX_LEARN=0` in an fprintd
drop-in.

The GUI also handles several fingers and several users and can delete
fingerprints. It talks only to fprintd: it never touches the sensor directly.

### From the terminal

```sh
fprintd-enroll -f right-index-finger
fprintd-verify
```

## 5. Fingerprint login (PAM)

**Anti-lockout rule.** The fingerprint always goes in as `sufficient`
**before** the password, never `required`. That way, if it fails or times out,
the system asks for the password as usual. While you edit PAM files, keep **a
root terminal already open** (`sudo -i`) until you have checked that everything
works.

The line to add is always the same:

```
auth sufficient pam_fprintd.so max-tries=10 timeout=20
```

`max-tries=10` allows ten touches (a touch outside the enrolled area just
becomes "try again"), `timeout=20` allows 20 seconds before falling back to the
password.

| Where | File | Position |
|---|---|---|
| `sudo` | `/etc/pam.d/sudo` | first `auth` line |
| greetd greeter (e.g. noctalia-greeter) | `/etc/pam.d/greetd` | first `auth` line |
| SDDM | `/etc/pam.d/sddm` | first `auth` line |
| Lock screen | `/etc/pam.d/<your locker>` (e.g. `hyprlock`, `swaylock`, `kde`) | first `auth` line |
| Console (`vlock`) | `/etc/pam.d/vlock` | first `auth` line |
| GDM | built in | enable it from the GNOME settings |

Try it right away:
```sh
sudo -k; sudo -v      # must ask for the finger; with the right finger it lets you in without a password
```

**noctalia-greeter.** To be able to choose between fingerprint and password at
the graphical login, the greeter's `greeter.toml` needs:
```toml
[auth]
allow_empty_password = true
```
Then at login you either touch the sensor or type the password.

## 6. Keyring unlock via TPM (optional)

If you log in with the fingerprint, the GNOME keyring does not receive the
password and stays locked.
[`service/keyring-tpm/`](../service/keyring-tpm/README.md) seals its password
in the TPM, bound to Secure Boot, as Windows Hello does. **It requires Secure
Boot to be enabled**: [`docs/secure-boot.md`](secure-boot.md).

```sh
cd service/keyring-tpm && ./install.sh   # then log out and log in again
```

## 7. Updates

There is nothing to do: `pacman -Syu` / `paru -Syu` update libfprint and
fprintd as usual, and `goodixfpd` does not depend on their internals, only on
fprintd's public D-Bus interface, which has been stable for years.

The only case that needs action is rare: a new **binary** version of a library
the service uses (for example the move to OpenSSL 4). Then the service no
longer loads: the reader stops, but sudo and login ask for the password as
usual. The package hook says so at the end of the update:

```
!! goodixfpd no longer loads: missing libssl.so.4
```

and a rebuild is enough:

```sh
cd goodix-driver/packaging/goodixfpd && makepkg -sif
```

`checkrebuild` (rebuild-detector) reports it too, as for any locally built
package.

**Attack surface.** The service is not exposed to the network. It runs like
fprintd: same root user, same systemd protection (`ProtectSystem=strict`,
access to USB devices only, no network), same polkit rules for who may enroll,
verify or act on another user. Its inputs are the USB sensor, authorized D-Bus
calls and the fingerprints in `/var/lib/fprint/goodixfpd/` (0600 files in 0700
directories, writable only by root, read with format and ownership checks).
The most specific code, and therefore the riskiest, is ours: the USB and TLS
protocol and the matcher. The most serious bug found so far, wrong fingers
being accepted, was exactly there and has been fixed
(`docs/how-it-works.md`).

## 8. Uninstalling

```sh
# remove the pam_fprintd line from the PAM files edited in step 5
fprintd-delete "$USER"         # optional: deletes the fingerprints
sudo pacman -R goodixfpd       # the distribution's fprintd comes back
```

Without `goodixfpd` the Goodix reader no longer works (the official libfprint
does not support it), and everything else goes back exactly as it was.

## 9. Troubleshooting

The first tool is always the log:
```sh
journalctl -u fprintd -b --no-pager | grep -E "goodix|Frame signal|Print|Learned"
```

(the service runs inside the `fprintd.service` unit, so that is the log).

**`fprintd-list` does not see the sensor.** Check `lsusb` and that the package
is installed (`pacman -Q goodixfpd`), then `sudo systemctl restart fprintd`. If
the log says "No Goodix 27c6:5125/5135 reader, starting fprintd instead", the
service did not find the reader on the USB bus.

**The finger is never read at login right after power-on.** At boot the
sensor is cold: on the first use after every boot the service discards the
saved baseline and samples it again (in the log: "New boot: stored
finger-detect baseline discarded"). Keep your finger off the sensor during that
first open.

**The sensor keeps firing on nothing / many "void" frames.** The service
re-learns the finger-detect baseline every time the finger is lifted; very fast
touches can teach it a wrong one, and the sensor then fires continuously with
"reverse" events (`IrqStatus=0x80` in the log). The service repairs it by
itself on the first empty frame ("finger-detect base out of date,
resampling"). During enrollment, lift the finger fully between touches.

**The reader stops after suspend.** If the sensor stops answering during a
verification, after a few seconds the service ends the operation ("Sensor
lost") and the login falls back to the password; the next attempt reopens the
sensor from scratch.

**"Invalid touch" very often.** The quality check discards light touches. In
the log, `Frame signal ... ridge=` is the ridge contrast: a good touch is above
100, the threshold is 10. Press the finger flat. If the ridges are good but
discarded, you can lower the threshold with
`Environment=GOODIX_MIN_RIDGE=5` in an fprintd drop-in.

**Poor recognition (many rejections).** Every rejection is a touch that fell
outside the enrolled areas. With use the fingerprint grows by itself (step 4);
if rejections stay frequent, enroll the finger again covering the area you
really use to unlock.

**Repeated `no image frame received`, or the sensor no longer answers.**
Restart fprintd. If that is not enough, the sensor may stay stuck until it is
restored from Windows: it is recovered with a Windows VM running the original
Goodix driver, with the sensor passed through over USB. The complete procedure
is in [`docs/recovery.md`](recovery.md); it needs `Virtualization Technology`
enabled in the BIOS. After the recovery, the Linux driver synchronizes the key
again by itself.

**Deep diagnosis / calibration.** With `Environment=GOODIX_DUMP_IMGPROC=1` the
service saves the raw frames in `/var/lib/fprint/goodix/` (for calibration
only: remove it and delete the files afterwards). The `template` test of
`goodixfpd/` replays the pipeline and the matching offline on a folder of
captures (`meson setup build -Ddataset=<folder>`) and measures FRR and FAR.

**Manual debug log.**
```sh
sudo systemctl edit fprintd        # add:  [Service]  Environment=G_MESSAGES_DEBUG=goodixfpd
sudo systemctl restart fprintd     # (remove the drop-in when you are done)
```
