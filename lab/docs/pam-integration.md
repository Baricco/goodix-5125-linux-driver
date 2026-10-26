# Fingerprint login with goodixfpd (Path A)

Integration of the fingerprint into the Linux authentication stack **without** the C driver: a
system daemon (`lab/daemon/goodixfpd.py`) owns the sensor and exposes a verify over a Unix
socket; PAM queries it through a small client (`lab/daemon/goodixfp-verify`). It covers `sudo`,
the lock screen and the greeter login. It is the quick way to use it *now*; the canonical path,
publishable for everyone, remains the libfprint driver (Path B, `lab/docs/porting-c.md`).

> **Golden anti-lockout rule:** the fingerprint is always a `sufficient` factor with *password
> fallback*. Never make it `required` alone. During setup keep **a root shell already open**
> (`sudo -i` in another terminal) until you have checked that everything works.

## Architecture

```
 [PAM: sudo / login]  →  goodixfp-verify  →  /run/goodixfpd/sock  →  goodixfpd  →  sensor
                         (client, root)        (0660 root:goodixfp)   (user goodixfp)
```

- The daemon runs as the system user **`goodixfp`**, not root; it reaches the sensor through
  the `goodixfp` group (udev rule `70-goodixfp.rules`).
- It checks the caller with `SO_PEERCRED`: it accepts only root (the PAM stack) or the user
  themselves.
- Fingerprints live in `/var/lib/goodixfp/<user>/*.npz` (`FragmentMapper` format).
- The daemon never touches the sensor's flash (no E0 provisioning) and never returns biometric
  data, only outcome + score.

## Quick installation (scripts)

The project runs as a system service from `/opt/goodix-driver` (production copy, owned by root:
the authentication code must not be writable by a normal user). Development stays in
`~/goodix-driver`.

```bash
sudo lab/daemon/install.sh        # user+group, deploy to /opt (venv recreated + PSK), udev, systemd
# ...enroll a fingerprint and test the socket (install.sh prints how), THEN hook up PAM...
sudo lab/daemon/deploy.sh         # resyncs /opt after every code change
sudo lab/daemon/uninstall.sh      # removes everything (add --purge for fingerprints+user)
```

`install.sh` does **not** touch PAM: you hook it up by hand with the safe test below. The rest
of this page explains the single steps (useful if you prefer to do them by hand, or to understand
what the scripts do).

## Manual installation (equivalent to the scripts)

```bash
# 1. dedicated system user and group
sudo groupadd -r goodixfp
sudo useradd  -r -g goodixfp -s /usr/sbin/nologin -d /var/lib/goodixfp goodixfp

# 2. sensor access for the group (replaces the old 70-goodix-5125.rules)
sudo cp lab/daemon/70-goodixfp.rules /etc/udev/rules.d/
sudo rm -f /etc/udev/rules.d/70-goodix-5125.rules
sudo udevadm control --reload && sudo udevadm trigger

# 3. PAM client
sudo install -Dm755 lab/daemon/goodixfp-verify /usr/local/lib/goodixfp/goodixfp-verify

# 4. daemon
sudo cp lab/daemon/goodixfpd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now goodixfpd
systemctl status goodixfpd          # must print "listening on /run/goodixfpd/sock"
```

## Enrolling a fingerprint

Enrollment is done with the GUI (as the user, densely: fill the red holes until "good
coverage"):

```bash
tools/.venv/bin/python lab/enroll_gui.py       # map + Save -> state/fingers/<name>.npz
```

Then copy the template into your user's system store:

```bash
sudo install -Dm640 -o root -g goodixfp \
     state/fingers/<name>.npz /var/lib/goodixfp/$USER/<name>.npz
```

You can enroll several fingerprints (several `.npz` files): verification tries them all.

## SAFE testing (in this order)

1. **Socket, without PAM.** As the user, touch the sensor when asked:
   ```bash
   GOODIXFP_SOCKET=/run/goodixfpd/sock python lab/daemon/goodixfp-verify $USER ; echo "result=$?"
   ```
   `result=0` = recognized. Repeat with another finger: it must give `1`.

2. **sudo.** With the root shell open on the side, add *at the top* of `/etc/pam.d/sudo`:
   ```
   auth  sufficient  pam_exec.so  quiet  /usr/local/lib/goodixfp/goodixfp-verify
   ```
   Open a new terminal, `sudo -v`: touch the sensor → no password. If the sensor fails, it must
   ask for the password as usual. If something goes wrong, the root shell already open saves you.

3. **Lock screen** (e.g. `/etc/pam.d/system-local-login` or your locker's PAM file) and finally
   **greeter login**, only after sudo is solid. Same `sufficient` line.

## Known limits (honest)

- **False negatives = coverage.** The sensor is small and touch-based: verification accepts if
  the press lands on an enrolled area. Dense enrollment + a couple of retries solve it. Impostors
  stay at 0 (excellent FAR). See `docs/matching.md`.
- **Latency.** The daemon opens the sensor on demand (reset + sync + TLS ≈ a few seconds). A
  persistent warm session is a future improvement.
- **Wedge.** If the sensor locks up deeply, today the Windows VM is needed to revive it (see the
  project status). A 100% Linux vendor reset is the next step of the plan. The daemon tears down
  cleanly after every verify precisely to avoid getting there.
- **Biometric security.** A fingerprint is not a strong secret; use it as a convenience next to
  the password, not as the only factor on machines at risk.
