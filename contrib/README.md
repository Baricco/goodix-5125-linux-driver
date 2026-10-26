# contrib — fixes sent back to the projects this code comes from

Fixes born here that are useful to others too. **The project depends on none
of them**: `goodixfpd` uses neither libfprint nor Rockytkg's driver. They are
contributions back, not requirements.

The patches are split into self-contained commits with their final messages.
Status: **submitted** on 25/10/2026.

## 1. Rockytkg/goodix-linux-27c6-5125 — GitHub pull request

Submitted as [Rockytkg/goodix-linux-27c6-5125#4](https://github.com/Rockytkg/goodix-linux-27c6-5125/pull/4).

[`rockytkg/`](rockytkg): four commits on top of `227eba2` (master). Each one
builds by itself with their `make`, the third and the fourth also without
`include/goodix_fw.h`.

| | |
|---|---|
| `0001` | No more wrong fingers accepted: "ridge" image pipeline, fresh baseline before every touch, contrast check (`GOODIX_MIN_RIDGE`); the wait for an image already requested can no longer be cancelled (it left half a TLS record in the channel), "reverse" touches 0x80/0x82 recognized. |
| `0002` | Enrollment with 15 to 30 touches (3-8 before, not enough coverage); frame buffer on the heap (64 KiB limit of GObject instances). |
| `0003` | Builds without the Goodix firmware too: it only compares the version and never erases the APP area without an image to write; the installers copy `goodix_fw.h` only if it exists. |
| `0004` | The installers apply the two SIGFM fixes (see below) to the fork's `sigfm.cpp`: a second line of defence. |

Measurements (data sets in `captures/`, bench in `tools/fp_tune`):

| | impostors accepted | genuine accepted |
|---|---|---|
| weak touches, old pipeline, original SIGFM | 15/20 | 7/20 |
| weak touches, old pipeline, fixed SIGFM | 0/20 | 0/20 |
| ridge pipeline (commit 1), original SIGFM | 0/25 | 22/25 |
| ridge pipeline (commit 1), fixed SIGFM (commit 4) | 0/25 | 22/25, scores ~2.5× higher |

Title and text as submitted:

> **Fix false accepts on the 27c6:5125, 15-30 press enrollment, optional firmware**
>
> Four independent commits; each one builds with `make` (the third and the
> fourth also without `include/goodix_fw.h`).
>
> 1. **False accepts.** Every frame carries a fixed pattern tied to the
>    sensor pixels; in light presses SIFT keypoints land on it and match any
>    other frame, of any finger (impostors scored 2,000-13,000 against a
>    threshold of 20). A ridge image pipeline (baseline-shaped residue
>    removal, DoG band-pass, CLAHE), a fresh no-finger baseline before each
>    press and a ridge-contrast gate fix it: measured live on a MateBook
>    CREM-WXX9, 0/25 impostor presses and 22/25 genuine presses accepted.
>    Two capture bugs found on the way are fixed too: cancelling an image
>    wait left half a TLS record in the channel (every later image failed to
>    decrypt), and "reverse" touch events 0x80/0x82 were ignored, which hung
>    enrollment.
> 2. **Enroll 15-30 presses.** One frame covers about 4 x 3 mm; 3-8 frames
>    cannot cover the area used to unlock.
> 3. **Optional firmware.** Distributions cannot ship `goodix_fw.h`; without
>    it the driver only compares the version and never erases the APP area,
>    and the installers copy the header only when it exists.
> 4. **SIGFM fixes in the installers.** The SIGFM copy in the libfprint fork
>    orders matches by `p1.y` only (`std::set` keeps one correspondence per
>    row) and keeps correspondences that did not move, which is how the fixed
>    pattern matched across fingers. The installers now patch both in
>    `sigfm.cpp`. With commit 1 our impostors already score 0, so this is a
>    second line of defence: on weak frames from the old pipeline the
>    unpatched SIGFM accepts 15/20 impostors, the patched one 0/20 (those
>    frames carry almost no ridge signal, which is why commit 1 now asks
>    for a retry). Both fixes are also proposed upstream on libfprint !530.

## 2. SIGFM — comment on libfprint merge request !530

Submitted as a comment on
<https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530>, with
the two patches attached (the MR lives on another author's fork, so commits
cannot be pushed to its branch).

[`sigfm/`](sigfm): two commits on top of the current head of the MR
(`bc986621 elan: switch to sigfm`). The SIGFM tests (`sigfm-tests`, doctest)
pass.

| | |
|---|---|
| `0001` | `match::operator<` compared only `p1.y`: `std::set` kept one correspondence per row and dropped the others. A pure fix. |
| `0002` | Drops correspondences displaced by ≤ 2 px (the fixed pattern of small sensors made wrong fingers match). |

Text as submitted:

> Two fixes for `sigfm.cpp`, found while using SIGFM with a Goodix 27c6:5125
> (80x64, match-on-host) on top of this MR. Patches attached, based on
> `bc986621`; `sigfm-tests` pass.
>
> 1. `match::operator<` compares only `p1.y` (the second clause repeats the
>    first condition), so `std::set` keeps one correspondence per keypoint row
>    and drops the rest. The patch uses a full lexicographic order consistent
>    with `operator==`. Genuine scores go up, so thresholds tuned on the old
>    behaviour (e.g. elan) may deserve a second look.
> 2. On small sensors every frame carries a fixed pattern locked to the pixel
>    grid. Its keypoints match across *different* fingers with perfect
>    geometry: impostors scored 2,000-13,000 against a threshold of 20, and
>    93% of those correspondences moved by <= 1 px. Dropping correspondences
>    displaced by <= 2 px brought impostors to 0 on all our data (live: 0/25
>    impostors, 22/25 genuine accepted). Side effect worth knowing: an image
>    compared with a pixel-identical copy now scores 0.
