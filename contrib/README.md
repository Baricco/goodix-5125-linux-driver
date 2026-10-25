# contrib — contributi ai progetti da cui viene il codice

Correzioni nate qui che servono anche agli altri. **Il progetto non dipende
da nessuna di queste**: `goodixfpd` non usa né libfprint né il driver di
Rockytkg. Sono restituzioni, non requisiti.

Le patch sono già divise in commit autonomi, con i messaggi definitivi. Per
inviarle serve un account sui rispettivi siti.

## 1. SIGFM — merge request libfprint !530

[`sigfm/`](sigfm): due commit sopra la testa attuale della MR
(`bc986621 elan: switch to sigfm`). I test di SIGFM (`sigfm-tests`, doctest)
passano.

| | |
|---|---|
| `0001` | `operator<` di `match` confrontava solo `p1.y`: `std::set` teneva una corrispondenza per riga e scartava le altre. Correzione pura. |
| `0002` | Scarta le corrispondenze con spostamento ≤ 2 px (il pattern fisso dei sensori piccoli faceva accettare dita sbagliate). |

Dove: commento sulla MR
<https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530>
con le due patch allegate (la MR è sul fork di un altro autore, quindi non si
può spingere sul suo ramo). Testo proposto:

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

## 2. Rockytkg/goodix-linux-27c6-5125 — pull request

[`rockytkg/`](rockytkg): tre commit sopra `227eba2` (master). Ognuno compila
da solo con il loro `make`; il terzo anche senza `include/goodix_fw.h`.

| | |
|---|---|
| `0001` | Niente più dita sbagliate accettate: pipeline immagine "ridge", baseline fresca prima di ogni tocco, controllo del contrasto (`GOODIX_MIN_RIDGE`), attesa dell'immagine non annullabile (evitava record TLS a metà). |
| `0002` | Registrazione da 15 a 30 tocchi (prima 3-8, copertura insufficiente); buffer dei frame sullo heap (limite 64 KiB delle istanze GObject). |
| `0003` | Si compila anche senza il firmware Goodix incluso: confronta solo la versione e non cancella mai l'APP senza un'immagine da scrivere. |

Dove: fork di <https://github.com/Rockytkg/goodix-linux-27c6-5125> su GitHub,
`git am contrib/rockytkg/*.patch` su un ramo, push, pull request. Titolo e
testo proposti:

> **Fix false accepts on the 27c6:5125, larger enroll template, build without bundled firmware**
>
> Three independent commits, each builds with `make`:
>
> 1. **False accepts.** Every frame carries a fixed pattern tied to the
>    sensor pixels; in light presses SIFT keypoints land on it and match any
>    other frame, of any finger (impostors scored 2,000-13,000 vs a threshold
>    of 20). A ridge image pipeline (baseline-shaped residue removal, DoG
>    band-pass, CLAHE), a fresh no-finger baseline before each press and a
>    ridge-contrast gate fix it; together with a SIGFM change (correspondences
>    displaced by <= 2 px are ignored, proposed on libfprint !530) we measured
>    0/25 impostors and 22/25 genuine presses accepted on a MateBook CREM-WXX9.
> 2. **Enroll 15-30 presses.** One frame covers ~4x3 mm; 3-8 frames cannot
>    cover the area used to unlock.
> 3. **Optional firmware.** Distributions cannot ship `goodix_fw.h`; without
>    it the driver only compares the version and never erases the APP area.
