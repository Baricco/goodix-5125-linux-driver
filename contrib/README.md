# contrib — contributi ai progetti da cui viene il codice

Correzioni nate qui che servono anche agli altri. **Il progetto non dipende
da nessuna di queste**: `goodixfpd` non usa né libfprint né il driver di
Rockytkg. Sono restituzioni, non requisiti.

Le patch sono già divise in commit autonomi, con i messaggi definitivi e
come autore l'indirizzo anonimo GitHub
(`38298051+Baricco@users.noreply.github.com`). Stato: **non ancora inviate**.

## 1. Rockytkg/goodix-linux-27c6-5125 — pull request su GitHub

[`rockytkg/`](rockytkg): quattro commit sopra `227eba2` (master), anche come
ramo `fix-false-accepts` nel clone `ext/goodix-linux-27c6-5125`. Ognuno
compila da solo con il loro `make`, il terzo e il quarto anche senza
`include/goodix_fw.h`.

| | |
|---|---|
| `0001` | Niente più dita sbagliate accettate: pipeline immagine "ridge", baseline fresca prima di ogni tocco, controllo del contrasto (`GOODIX_MIN_RIDGE`); attesa dell'immagine non annullabile (evitava record TLS a metà), tocchi "rovesciati" 0x80/0x82 riconosciuti. |
| `0002` | Registrazione da 15 a 30 tocchi (prima 3-8, copertura insufficiente); buffer dei frame sullo heap (limite 64 KiB delle istanze GObject). |
| `0003` | Si compila anche senza il firmware Goodix: confronta solo la versione, non cancella mai l'APP senza un'immagine da scrivere; gli installatori copiano `goodix_fw.h` solo se c'è. |
| `0004` | Gli installatori applicano a `sigfm.cpp` del fork le due correzioni di SIGFM (vedi sotto): seconda linea di difesa. |

Misure (dataset in `captures/`, banco `tools/fp_tune`):

| | impostori accettati | genuini accettati |
|---|---|---|
| tocchi deboli, pipeline vecchia, SIGFM originale | 15/20 | 7/20 |
| tocchi deboli, pipeline vecchia, SIGFM corretto | 0/20 | 0/20 |
| pipeline ridge (commit 1), SIGFM originale | 0/25 | 22/25 |
| pipeline ridge (commit 1), SIGFM corretto (commit 4) | 0/25 | 22/25, punteggi ~2,5× più alti |

Titolo e testo proposti:

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
>
> 🤖 Generated with [Claude Code](https://claude.com/claude-code)

## 2. SIGFM — commento sulla merge request libfprint !530

[`sigfm/`](sigfm): due commit sopra la testa attuale della MR
(`bc986621 elan: switch to sigfm`), anche come ramo `sigfm-fixes` nel clone
`ext/goodix-linux-27c6-5125/libfprint`. I test di SIGFM (`sigfm-tests`,
doctest) passano.

| | |
|---|---|
| `0001` | `operator<` di `match` confrontava solo `p1.y`: `std::set` teneva una corrispondenza per riga e scartava le altre. Correzione pura. |
| `0002` | Scarta le corrispondenze con spostamento ≤ 2 px (il pattern fisso dei sensori piccoli faceva accettare dita sbagliate). |

La MR è sul fork di un altro autore, quindi non si può spingere sul suo
ramo: si lascia un commento con le due patch allegate. Testo proposto:

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

## Come inviarle (a mano)

**GitHub (Rockytkg).** Fork dalla pagina del progetto (pulsante *Fork*), poi
dal clone:

```sh
cd ext/goodix-linux-27c6-5125
git remote add fork git@github.com:Baricco/goodix-linux-27c6-5125.git
git push fork fix-false-accepts
```

Infine su GitHub *Compare & pull request*, verso `Rockytkg:master`, con il
titolo e il testo qui sopra.

**GitLab (SIGFM).** Account su <https://gitlab.freedesktop.org> (i nuovi
account non possono fare fork, ma possono commentare), poi sulla pagina
<https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530>:
testo nel riquadro dei commenti, i due file di `sigfm/` allegati, *Comment*.
