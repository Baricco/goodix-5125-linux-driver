# lab — the reverse-engineering lab

The Python prototype used to decode the sensor, before the libfprint driver.
**It is not needed to use the fingerprint** (for that: `packaging/` and
`docs/INSTALL.md`). It is still useful to study the protocol and for low-level
debugging.

| File | What it does |
|---|---|
| `driver_5125.py` | PSK provisioning (E0), TLS-PSK session, image capture |
| `pairing.py` | `0xbb010003` PSK envelope (public algorithm by sorguido/grold) |
| `sensor.py`, `sensor_recover.py`, `usb_reset.py` | sensor access, self-healing, reset |
| `preprocessor.py`, `sigfm.py`, `template.py`, `coverage.py` | preprocessing and SIGFM matcher in Python |
| `fragment_mapper.py`, `mosaic*.py`, `posegraph.py`, `enroll_gui.py` | coverage enrollment with a mosaic map |
| `daemon/` | the first authenticator (`goodixfpd` daemon + `pam_exec`), superseded by fprintd |
| `docs/` | historical notes: PAM integration of the daemon, C porting, migration to fprintd |

The files in `docs/` describe paths that were later abandoned and may mention
old locations.

Dependencies: `goodix-fp-dump/` at the repository root (not included, clone it
from goodix-fp-linux-dev) and `pip install -r lab/requirements.txt`. Local data
(`state/`, `captures/`) stays at the root and is never published.

Warning: `driver_5125.py --write` rewrites the sensor's PSK. After using it,
the Linux driver synchronizes the key again by itself the next time it starts.
