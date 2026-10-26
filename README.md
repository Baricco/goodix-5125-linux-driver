# goodix-5125 — Linux fingerprint support for the Goodix 27c6:5125

Fingerprint login on Linux for the **Goodix `27c6:5125`** reader (and
`27c6:5135`) built into the power button of Huawei MateBook / Honor MagicBook
laptops. It plugs into the standard stack, **fprintd + pam_fprintd**, so it
works with `sudo`, the greeter and the lock screen, with the password always
available as an alternative. **No library is modified**: libfprint and fprintd
stay the ones shipped by your distribution and update as usual.

**Status:** working on the real sensor (MateBook CREM-WXX9, Arch Linux and
CachyOS): enrollment, `sudo`, greeter, lock screen and GUI. On the calibration
data, 0/25 wrong fingers accepted and 22/25 touches of the enrolled finger
recognized at the first attempt; 16 automated scenarios against `pam_fprintd`
and the `fprintd-*` commands. See [how it works and why it is
safe](docs/how-it-works.md).

## What is inside

| | |
|---|---|
| [`goodixfpd/`](goodixfpd) | **The service**: speaks fprintd's D-Bus interface and drives the sensor (USB, TLS, imaging, matching, fingerprints that improve with use) |
| [`packaging/`](packaging) | Arch package **`goodixfpd`** |
| [`gui/`](gui) | **Fingerprint manager** (PySide6): enroll, test and delete fingerprints, per finger and per user |
| [`service/keyring-tpm/`](service/keyring-tpm) | Optional: unlocks the GNOME keyring at fingerprint login, with the password sealed in the TPM |
| [`docs/`](docs) | Installation guide, BIOS, Secure Boot, sensor recovery, how it works, protocol |
| [`tools/fp_tune/`](tools/fp_tune) | Calibration bench: sensor session with raw dumps and offline analysis using the driver's real code |
| [`contrib/`](contrib) | Fixes sent back to SIGFM and to Rockytkg's driver |
| [`lab/`](lab) | The reverse-engineering lab (Python prototype) the project grew out of |

## Quick install (Arch and derivatives)

```sh
git clone https://github.com/Baricco/goodix-5125-linux-driver.git goodix-driver
cd goodix-driver/packaging/goodixfpd && makepkg -si   # libfprint and fprintd stay the official ones
fprintd-enroll -f right-index-finger                  # or: python gui/fingerprint_manager.py
```

Then add `auth sufficient pam_fprintd.so max-tries=10 timeout=20` at the top
of the PAM files you care about. The complete procedure, with the
anti-lockout rules and troubleshooting, is in the
**[step-by-step guide](docs/INSTALL.md)**.

## Documentation

- [Installation and usage guide](docs/INSTALL.md) — from the package to login, updates, troubleshooting
- [How it works](docs/how-it-works.md) — the chain, the matcher, the false-accept problem and its defences, results
- [BIOS](docs/bios.md) — relevant settings (Secure Boot, TPM, virtualization)
- [Secure Boot on CachyOS + limine](docs/secure-boot.md)
- [Recovering a stuck sensor](docs/recovery.md)
- [Overall security](SECURITY.md) — fingerprint, keyring, disk encryption
- [Sensor protocol](docs/protocol.md) and [history of the matcher](docs/matching.md)

## Sensor safety

The service writes only its own key for the encrypted channel into the sensor
(PSK area, reversible), and only when it cannot read the existing one back.
**It contains no firmware and never writes any.** The Goodix firmware, the
Windows DLLs, USB captures and keys stay out of the repository.

## Credits

- [fprintd](https://gitlab.freedesktop.org/libfprint/fprintd) — the D-Bus
  interface that `goodixfpd` implements (its XML descriptions are included).
- SIGFM, from libfprint merge request !530 (Matthieu Charette, Natasha
  England-Elbro, Timur Mangliev) — the matching algorithm, rewritten in C.
- [Rockytkg/goodix-linux-27c6-5125](https://github.com/Rockytkg/goodix-linux-27c6-5125)
  — the sensor protocol core our changes are based on.
- [goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev) — goodix-fp-dump, SIGFM.
- sorguido / grold (goodix-27c6-5125, LGPL-2.1) — PSK provisioning algorithm.

## License

[GPL-2.0-or-later](LICENSE), including the sensor core and the fprintd
interface descriptions. Reverse engineering for interoperability on hardware
you own.
