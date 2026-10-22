# Strada C — migrazione allo stack standard: `fprintd` + driver `goodixgf`

Runbook per far entrare il sensore nello stack Linux standard (`fprintd` + `pam_fprintd`), così
impronta su **sudo + login greetd/noctalia + lockscreen**, con la UX "tocca **oppure** scrivi la
password". Rimpiazza il nostro demone `goodixfpd` (che resta come laboratorio/RE).

Driver adottato: **`goodixgf`** di [Rockytkg/goodix-linux-27c6-5125](https://github.com/Rockytkg/goodix-linux-27c6-5125)
sul fork [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint)
(branch `libfprint-sigfm`, matching **SIGFM** come il nostro). L'installer supporta CachyOS/Arch.

## Sicurezza — cosa si scrive sul sensore

- **Firmware: NON flashato.** Verificato nel codice (`src/goodix_fwupdate.c`): bundla
  `GF_FW_VERSION="GF_ST411SEC_APP_12508"` = il nostro, e `strncmp(ver, GF_FW_VERSION)==0 -> return 1`
  (salta). Il nostro chip è 12508 → nessun flash. C'è anche un pre-check CRC anti-brick.
- **Unica scrittura: la PSK.** Alla prima apertura del device da `fprintd` (il primo enroll), il
  driver ri-provisiona la sua PSK (comando E0, solo settore store, reversibile). **Sovrascrive la
  nostra** `state/linux_psk.bin`. È "la scrittura che diamo sotto OK esplicito".
- **Backup PSK nostra:** `state/linux_psk.bin` e copia in `state/old_maps/linux_psk.bin`. Non toccarli:
  servono solo se si vuole tornare al demone Python.
- Regola di sempre: tieni una **root-shell aperta** (`sudo -i` in un altro terminale) mentre tocchi
  `/etc/pam.d/*`.

## 0. Metti da parte il nostro stack (device esclusivo: UN solo demone sul sensore)

```fish
sudo systemctl disable --now goodixfpd
sudo cp /etc/pam.d/sudo.bak /etc/pam.d/sudo   # rimuove la nostra riga pam_exec (ripristina l'originale)
```

## 1. Dipendenze mancanti (il resto del toolchain c'è già)

```fish
sudo pacman -S --needed opencv doctest meson fprintd gobject-introspection libgusb gtk-doc mbedtls glib2-devel
```
Note sui nomi/pacchetti:
- GUsb su Arch/CachyOS è `libgusb` (non `gusb`).
- **`glib2-devel` obbligatorio**: la `glib2` recente di Arch/CachyOS non spedisce più
  `glib-mkenums`/`glib-genmarshal` (ora in `glib2-devel`); senza, il build fallisce con
  `Program /usr/bin/glib-mkenums found: NO`.
- **`mbedtls` (4.x) è obbligatorio**: il driver usa l'API mbedtls 4.x e il build fa
  `find_library('mbedtls')` → cerca `/usr/lib/libmbedtls.so`. Avere solo `mbedtls3` (3.x LTS) NON
  basta: espone l'unversioned solo in `/usr/lib/mbedtls3/`, e il build fallisce con
  `ERROR: C shared or static library 'mbedtls' not found`. `mbedtls` e `mbedtls3` **coesistono**
  (header isolati: `/usr/include/mbedtls/` vs `/usr/include/mbedtls3/`); non rimuovere `mbedtls3`
  (serve a `librist`).
- Già installati: `ninja gcc pkgconf python openssl glib2 nss pixman libgudev systemd-libs`.

## 2. Clona e installa il driver

```fish
cd ~
git clone --recurse-submodules https://github.com/Rockytkg/goodix-linux-27c6-5125
cd goodix-linux-27c6-5125
set -x GOODIX_LINUX $PWD        # (bash: export GOODIX_LINUX="$PWD")
bash install-goodixgf-en.sh
```
Lo script (chiede la password per i passi `sudo`): installa le deps, copia `goodixgf` + core
protocollo nella fork, patcha `meson.build` (idempotente), **compila e installa in `/usr`**
(SONAME invariato → `fprintd` di sistema lo riusa), installa la regola udev `70-goodix.rules`,
aggiunge un drop-in **`--no-timeout`** a `fprintd` (indispensabile: altrimenti fprintd esce dopo
~30s e al login lo schermo non vede il sensore), ricarica udev e riavvia `fprintd`.

## 3. Enroll — qui avviene la ri-provisioning PSK (dai l'OK)

Alla prima apertura il driver scrive la nuova PSK (nessun flash firmware). Enroll **denso**:

```fish
journalctl -u fprintd -b | grep -i goodix  # conferma che goodixgf apre il device
fprintd-delete $USER                       # pulisce enroll precedenti (ok se non c'è nulla)
fprintd-enroll -f right-index-finger $USER # dito LONTANO al primo open (provisioning PSK + baseline),
                                           # poi appoggia quando chiede, 3-8 volte, spostando la zona
fprintd-verify $USER                       # ripeti qualche volta
```
Soglia matcher: `GF_SIGFM_SCORE_THRESHOLD=20` in `src/goodixgf.c` (ri-lanciare l'installer dopo una
modifica). SIGFM vuole ≥25 keypoint SIFT per frame, altrimenti chiede un nuovo tocco.
Premi come sbloccherai davvero (stessa zona/orientazione). Atteso ~3/4+ riconoscimenti; se basso,
`fprintd-delete` e ri-enrolla più denso. (La nostra esperienza: la copertura è tutto — vedi
`docs/matching.md`.)

## 4. PAM — impronta su sudo, login, lockscreen

Sempre `sufficient` → **fallback alla password** garantito. Con la root-shell aperta:

- **sudo** — in cima all'`auth` di `/etc/pam.d/sudo`:
  ```
  auth sufficient pam_fprintd.so
  ```
- **login greetd** — in cima all'`auth` di `/etc/pam.d/greetd`:
  ```
  auth sufficient pam_fprintd.so
  ```
- **lockscreen** — stessa riga nel file PAM del tuo locker (swaylock/hyprlock/…).

Test: nuovo terminale, `sudo -k; sudo -v` → tocca; deve passare senza password. Non toccando →
password come sempre.

## 5. noctalia — password + impronta concorrenti al login

La tua `noctalia-greeter 1.7.0` ha già il supporto (stringhe `allow_empty_password`,
`build fingerprint`; [Issue #35](https://github.com/noctalia-dev/noctalia-greeter/issues/35),
[PR #158](https://github.com/noctalia-dev/noctalia-greeter/pull/158)). Nel `greeter.toml`
(`/var/lib/noctalia-greeter/greeter.toml`, gestito da `noctalia-greeter-apply-appearance
--setup-system`, chownato all'utente `greeter`):

```toml
[auth]
allow_empty_password = true
# request_timeout = 60   # opzionale (0 disabilita il timeout)
```
Al login: **tocchi per entrare, oppure digiti la password** (la password digitata va in coda e
risponde al prompt solo se l'impronta fallisce/scade).

## 6. Rollback (se qualcosa non va)

```fish
cd ~/goodix-linux-27c6-5125; bash uninstall-goodixgf-en.sh
sudo cp /etc/pam.d/sudo.bak /etc/pam.d/sudo
# togli a mano le righe pam_fprintd da /etc/pam.d/greetd e dal locker
```
Per tornare al demone Python serve ri-appaiare la NOSTRA PSK sul sensore (il driver goodixgf l'ha
sovrascritta): `goodixfpd` fermo → `python driver_5125.py --write` con `state/linux_psk.bin` →
`sudo systemctl enable --now goodixfpd`.

## Note

- **Dual-boot Windows** riscrive la PSK → va rifatto l'enroll/re-pair (vale per qualsiasi stack).
- `fprintd` e `goodixfpd` **non insieme** (device busy): tienine UNO solo attivo.
- **Contributo upstream** (dopo): le nostre misure (enroll denso, sessione calda, risveglio dal
  powerdown idle, FRR/FAR, `docs/protocol.md`/`matching.md`) a goodix-fp-linux-dev — è il valore
  aperto che quei progetti (single-tester) non hanno.
