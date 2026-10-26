# How it works

## The chain

```
Goodix sensor (in the power button)
   │  USB CDC, TLS-PSK channel (AES-128-GCM, OpenSSL) with a key we hold
   ▼
goodixfpd  (instead of fprintd's daemon, same D-Bus interface)
   │  finger detect (FDT) -> 80×64 image, 12 bit
   │  "ridge" image pipeline -> 8-bit ridge image
   │  SIFT + SIGFM geometric voting (in C)    fingerprint = 15-30 frames of the finger
   ▼
net.reactivated.Fprint  ──  pam_fprintd / fprintd-* / GNOME / GUI
                            sudo, greeter, lock screen
```

The sensor is tiny: one frame covers about **4×3 mm** of finger at 500 DPI. A
single image holds too little for classic minutiae matchers, so the
**SIGFM** algorithm is used: it extracts SIFT keypoints and counts the pairs
of correspondences whose geometry (distances and angles) is consistent with a
rotation plus translation of the finger. The saved **fingerprint** is a set of
15-30 frames taken at slightly different spots of the area you use. A touch is
accepted if it matches at least one of them above the threshold (20).

## Why a separate service and not a driver inside libfprint

The first version was a driver inside libfprint, with the SIGFM matcher taken
from a merge request that has not been accepted yet (!530). It worked, but it
meant **modifying a system library**: every new libfprint required reapplying
about twenty patches, with the risk that they would stop applying.

The sensor code, however, did not depend on libfprint. So it now lives in a
service, `goodixfpd`, that implements the same contract as fprintd: the same
methods, signals, errors and polkit checks
(`net.reactivated.fprint.device.*`). The distribution's fprintd stays
installed: it provides `pam_fprintd`, the `fprintd-*` commands, the interface
description and the polkit rules. A drop-in for `fprintd.service` starts
`goodixfpd` instead of its daemon. When the Goodix reader is missing,
`goodixfpd` starts the original daemon.

Result: **no modified library**. libfprint and fprintd updates arrive as for
everybody else, and the project does not depend on other people accepting
patches. SIFT and the geometric voting are rewritten in C (no OpenCV), the TLS
channel uses OpenSSL (no mbedtls).

## The problem we had: the sensor's fixed pattern

The first calibration data set showed impostors scoring **2,000 to 13,000**
against a threshold of 20. That is, wrong fingers being accepted.

The cause: every frame contains a **fixed pattern tied to the sensor pixels**
(vertical stripes and blobs), the residue of the sensor's gain and baseline. In
light touches it dominates the image. Since it always sits on the same pixels,
the SIFT keypoints of the pattern in one frame match those of any other frame,
of any finger, perfectly. 93% of these false correspondences have a
displacement of ≤ 1 pixel.

## The defences

1. **Matcher: no still correspondences.** Matching discards pairs of points
   that are ≤ 2 pixels apart. A real finger never lands again on the exact
   same pixel; the fixed pattern does. This defence alone brings impostors to
   0 on every data set. An upstream SIGFM bug was fixed too: the ordering
   compared only the y coordinate and dropped valid correspondences.
2. **Ridge image.** Instead of stretch + sharpening, which amplified the fixed
   pattern, the pipeline does: `raw − baseline` in floating point, removal by
   regression of the component shaped like the baseline (gain), a DoG
   band-pass filter on the ridge frequency (σ 1/4 px), stretch and CLAHE.
3. **Fresh baseline.** Before arming finger detection the service takes a
   frame with no finger, which becomes the baseline of the next touch. It is
   discarded if it looks like it contains a finger; in that case the last
   valid baseline is used.
4. **Quality check.** Frames whose ridge contrast is too low (`ridge` < 10; a
   full touch is 100–200) become "try again" instead of being enrolled or
   matched.

With the ridge image and the quality check, the impostors of the second data
set already score 0 even with the uncorrected SIGFM; the matcher fix is the
second line of defence and roughly doubles the scores of the right finger.

## Measured results (real sensor, 24/10/2026)

Measured live with the previous version (driver inside libfprint, SIGFM on
OpenCV). A 31-touch template, then 25 verifications with the enrolled finger
and 25 with four other fingers:

| | Outcome |
|---|---|
| Enrolled finger | **22/25 accepted** (FRR 12%) |
| Other fingers | **0/25 accepted** (FAR 0) |
| Impostor score | **0** against all 30 template frames, in every attempt |
| Mirrored fingerprints ("non-existent" fingers, stress test) | 0 |
| Frames from 2–3 days earlier, other driver | enrolled finger recognized, other fingers 0 |

The 3 rejections are touches that fell outside the enrolled areas (a flat
score of 0, not borderline cases). At login `max-tries=10` absorbs them: you
touch again.

`goodixfpd` was verified offline on the same raw frames of that session. Its
image pipeline reproduces the 81 production images **bit for bit**, and the C
matcher gives **the same decisions**: 22/25 genuine, 0/25 impostors. Out of
1550 frame-to-frame pairs only one changes outcome, and none changes the
outcome of a verification.

## A fingerprint that improves with use

The Windows driver updates the template on every successful recognition.
`goodixfpd` does the same: a touch recognized with margin (score ≥ 40, twice
the threshold) that only a few frames recognize (at most 3, i.e. a touch at
the edge of the covered area) is added to the fingerprint. That way a touch a
little further out finds something to match next time, and coverage grows with
use.

Limits chosen for safety:
- enrollment frames **are never replaced**, so the fingerprint cannot "drift"
  away from the enrolled finger;
- learned frames are at most 30 and are renewed starting from the oldest;
- learning happens only from verifications, never from the duplicate check
  touch, and only with twice the threshold as margin;
- `GOODIX_LEARN=0` disables it.

On the session data learning changes no outcome: impostors stay at 0/25 even
against the enriched fingerprint.

## Reproducibility

Every change to the matcher or the pipeline is measured **before** touching the
sensor. The tests of `goodixfpd/` (`meson test`) check:

- `tls`: the TLS-PSK channel against a client playing the sensor's part
  (handshake, data, reconnection, wrong key rejected);
- `template`: on-disk format, storage, learning rules. With
  `-Ddataset=<folder>` of real captures, also the bit-exact pipeline, FRR and
  FAR;
- `dbus`: 16 scenarios against the real clients. Private bus, fake polkit,
  simulated sensor; `fprintd-list`, `fprintd-verify` and the real
  `pam_fprintd.so` (through `pam_start_confdir`, without touching
  `/etc/pam.d`).

[`tools/fp_tune/`](../tools/fp_tune) remains the bench used for the original
calibration (sensor session with raw dumps, analysis with the OpenCV SIGFM).
