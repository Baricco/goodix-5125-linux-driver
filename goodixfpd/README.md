# goodixfpd

Servizio per il lettore Goodix `27c6:5125` / `27c6:5135` che implementa
l'interfaccia D-Bus di fprintd (`net.reactivated.Fprint`). Prende il posto del
demone di fprintd tramite un drop-in di `fprintd.service`; se il lettore non
c'è avvia il vero fprintd. Installazione e uso: [`docs/INSTALL.md`](../docs/INSTALL.md).

## Struttura

| | |
|---|---|
| `src/core/` | protocollo del sensore (USB CDC, comandi, PSK, TLS-PSK su OpenSSL, finger-detect, acquisizione), pipeline immagine, SIFT e confronto SIGFM in C |
| `src/daemon/gxd-device.c` | l'oggetto `Device`: sessioni, Claim/Release, verifica, identificazione, registrazione, polkit, come fprintd 1.94 |
| `src/daemon/gxd-manager.c` | `Manager` e ObjectManager |
| `src/daemon/gxd-sensor.c` | il sensore in un thread di lavoro, eventi verso il contesto principale |
| `src/daemon/gxd-template.c`, `gxd-storage.c` | impronte: confronto, apprendimento con l'uso, formato su disco |
| `src/daemon/gxd-sensor-test.c` | sensore simulato (solo `goodixfpd-test`, mai installato) |
| `src/daemon/*.xml` | descrizione dell'interfaccia, da fprintd |
| `tests/` | `tls`, `template`, `dbus` |

## Compilare e provare

```sh
meson setup build                      # -Ddataset=<catture> per il test sul matcher
meson test -C build
meson setup build-asan -Db_sanitize=address,undefined && meson test -C build-asan
```

Il test `dbus` avvia un bus di sistema privato, un polkit finto e
`goodixfpd-test`, poi usa il servizio come pam_fprintd e i comandi `fprintd-*`
(compreso il vero `pam_fprintd.so`, tramite `pam_start_confdir`). Le "dita"
sono immagini sintetiche generate dal test; servono `dbus-daemon`,
`python-gobject` e `python-numpy`.

## Variabili d'ambiente

Si impostano con un drop-in di `fprintd.service` (`Environment=`):

| | |
|---|---|
| `GOODIX_LEARN=0` | l'impronta non cresce con l'uso |
| `GOODIX_MIN_RIDGE` | contrasto minimo delle creste (default 10; 0 = nessun controllo) |
| `GOODIX_ENROLL_MIN_DIFF` | differenza minima fra due tocchi di registrazione (default 8) |
| `GOODIX_IMGPROC_*` | parametri della pipeline immagine (vedi `src/core/goodix_imgproc.h`); cambiarli richiede di registrare di nuovo |
| `GOODIX_DUMP_IMGPROC=1` | salva i frame grezzi in `/var/lib/fprint/goodix/` (solo per taratura) |
| `G_MESSAGES_DEBUG=goodixfpd` | log dettagliato |
