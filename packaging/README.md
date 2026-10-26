# packaging

## `goodixfpd/` — the package to use

Arch package of the [`goodixfpd`](../goodixfpd) service. It installs:

- `/usr/lib/goodixfpd`, the service;
- `/usr/lib/systemd/system/fprintd.service.d/goodixfpd.conf`, the drop-in that
  starts it instead of fprintd's daemon (same unit, same systemd protection);
- a pacman hook that, after every update, checks that the service still
  loads.

It neither replaces nor modifies any package: it depends on the
distribution's `libfprint` and `fprintd`. It conflicts only with the old
`libfprint-goodixgf`.

```sh
cd goodixfpd && makepkg -si
```

`check()` runs all the service tests (`meson test`): TLS channel, fingerprint
format and 16 D-Bus scenarios against `pam_fprintd.so` and the `fprintd-*`
commands, with a simulated sensor.

**Updates.** Nothing to do when new libfprint or fprintd versions come out. A
rebuild (`makepkg -sif`) is needed only if a dependency changes its binary
version (soname): the hook reports it with the name of the missing library.

## Before `goodixfpd`

Until 25/10/2026 the reader worked with `libfprint-goodixgf`: the official
libfprint plus 21 patches (the SIGFM matcher from merge request !530 and the
`goodixgf` driver), to be realigned with every new libfprint through
`goodixgf-update`. `goodixfpd` replaced it without modifying any library. The
files are in the git history (commit `905750c` and earlier); the fixes useful
to other projects are in [`contrib/`](../contrib).
