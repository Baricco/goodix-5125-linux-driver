# Matching with SIGFM: the lab history

> This page tells how the matcher came about, in the Python lab (`lab/`). The
> current behaviour, the measured results and the defences are in
> [`how-it-works.md`](how-it-works.md); the implementation is in
> `goodixfpd/src/core/goodix_sift.c` and `goodix_match.c`.

Recognition uses **SIGFM** ("SIFT Is Good For Matching"), the
goodix-fp-linux-dev algorithm designed precisely for 64x80 sensors. Faithful
port in `lab/sigfm.py` (from `cpp/match.cpp`/`compute.cpp` of the upstream
repository), multi-frame template in `lab/template.py`.

## Why the first version (RANSAC) failed

The first attempt (`lab/match.py`) used SIFT + `findHomography(RANSAC)`: a fit
of ONE global transformation, score = number of inliers. On 64x64 with few
noisy matches RANSAC fits nothing stable -> score 0 for both genuine and
impostor, no threshold separates them.

SIGFM instead does **pairwise geometric voting, invariant to rotation and
scale**:
1. SIFT + Lowe's ratio test (0.75) -> tentative matches.
2. for every pair of matches, if the distance between the keypoints is
   preserved within 5% (LENGTH_MATCH) it records an "angle" (sin/cos of the
   relative rotation).
3. for every pair of angles, if sin and cos agree within 5% (ANGLE_MATCH), +1
   to the score.

The score is the combinatorial count of consistent pairs: huge for the same
finger with overlap, ~0 for different fingers. It degrades gracefully (even a
partial overlap leaves a consistent cluster), unlike a single-homography fit.

## Preprocessing

`lab/preprocessor.py` (clear-frame baseline subtraction + per-column
normalization + CLAHE) clearly beats the raw min-max of the upstream demo.
Measured (template max, real data):

| probe                       | raw min-max | column-norm + CLAHE |
|-----------------------------|------------:|--------------------:|
| A2 (genuine, with overlap)  |      36 585 |         **251 661** |
| B1 (impostor)               |         228 |              **49** |

CLAHE strengthens the genuine score ~7x and cuts impostor leakage ~5x.

## Measured results (real hardware: finger A genuine, finger B impostor)

Template = burst Aenr_00..09; verify = max score over the template,
threshold 300:

- A2 (genuine, lands on a covered area) -> **ACCEPTED**, score 251661.
- B1, Bver (impostor) -> **REJECTED**, score 49 and 0.
- A1, Aver (genuine, but areas NOT covered by the burst) -> REJECTED, score 0.

Speed: ~1.35 ms per comparison on average (max ~80 ms in the combinatorial
case of nearly identical images). Verification against a 10-15 frame template
stays well under 100 ms.

## The real constraint: COVERAGE at enrollment

The false rejections (A1, Aver) are not a matcher defect: SIGFM (rightly)
finds overlap when there is some and 0 when there is none. The Aenr frames are
a tight burst on a single area of the finger; independent touches landing
elsewhere have nothing to overlap with.

The solution is **coverage-guided enrollment** (Windows Hello style): 12-15
touches spread over the whole fingertip (tip, left, right, base, centre), so
that every future touch overlaps at least one enrolled frame. With that data
set:
1. rebuild the template (`lab/template.py enroll`),
2. tune the threshold on the real genuine/impostor separation (provisionally
   300 for now).

## Next practical step

Capture session on the hardware (run by the user):
`lab/driver_5125.py --enroll 15 --tag A`, moving the finger to cover the whole
surface, then a few genuine verification touches + a couple with another
finger. Template and tuning are done offline from the saved `*_plain.bin`
files.

The rest of the chain (protocol, PSK provisioning, TLS, capture, decoding) is
already verified and working.

## Incremental mosaic (reducing false negatives)

On real dense mapping (~113 areas): 0 false positives (SIGFM is selective),
but false negatives remain when the verification touch lands in a coverage
HOLE. Adjacent areas, however, DO REGISTER with each other (rotation plus
translation, real offsets of 15-50 px, rotations < 6°): a mosaic of 8
connected areas stitches correctly with continuous ridges.

The limit is not the finger but point-based enrollment: `SAME_SPOT` creates a
new area only when the overlap is already lost, so it throws away the
intermediate frames that would be needed for stitching (largest connected
component ~8-15 out of 113). Solution: INCREMENTAL MOSAIC enrollment
(`lab/mosaic.py`, `lab/mosaic_session.py`): the dense stream is kept, every
frame registers onto the current map and extends it only if it locks; holes
(uncovered areas inside the envelope) emerge from the coverage and guide the
mapping. The GUI (`lab/enroll_gui.py`) shows the live mosaic with the holes in
red. Verification stays on the SIGFM score (0 false positives) plus the
registration inliers.
