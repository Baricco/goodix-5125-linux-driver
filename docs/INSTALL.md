# Guida: impronta digitale su Linux per Goodix 27c6:5125

Dalla prima installazione al login con l'impronta, passo passo. Ogni sezione
dice cosa fa e come verificare che abbia funzionato. Se qualcosa va storto,
vai a [Risoluzione problemi](#9-risoluzione-problemi).

Indice:

1. [Requisiti](#1-requisiti)
2. [BIOS](#2-bios)
3. [Installare il servizio](#3-installare-il-servizio)
4. [Registrare l'impronta](#4-registrare-limpronta)
5. [Login con l'impronta (PAM)](#5-login-con-limpronta-pam)
6. [Sblocco del keyring via TPM (facoltativo)](#6-sblocco-del-keyring-via-tpm-facoltativo)
7. [Aggiornamenti](#7-aggiornamenti)
8. [Disinstallare](#8-disinstallare)
9. [Risoluzione problemi](#9-risoluzione-problemi)

Come funziona il riconoscimento, e perché è sicuro: [`docs/how-it-works.md`](how-it-works.md).

---

## 1. Requisiti

- **Sensore Goodix `27c6:5125`** (o `27c6:5135`). Verifica con:
  ```sh
  lsusb | grep -i 27c6
  # Bus 003 Device 004: ID 27c6:5125 Shenzhen Goodix Technology Co.,Ltd. FingerPrint
  ```
  È quello nel tasto di accensione dei portatili Huawei MateBook / Honor
  MagicBook con firmware `GF_ST411SEC_APP_125xx`. Testato su MateBook
  CREM-WXX9, firmware `12508`.
- **`fprintd` e `pam_fprintd` della distribuzione**, così come sono
  (`sudo pacman -S fprintd`). Il progetto non modifica né libfprint né fprintd.
- **Arch Linux o derivate** (CachyOS, EndeavourOS, Manjaro…) per il pacchetto
  pronto. Sulle altre distribuzioni si compila e si installa con meson
  (vedi [più sotto](#altre-distribuzioni)): non c'è nessuna libreria da
  sostituire, quindi gli aggiornamenti della distribuzione non lo toccano.
- Un utente con `sudo`.

## 2. BIOS

Per l'impronta **non serve toccare il BIOS**. Le impostazioni utili alle parti
di sicurezza facoltative (Secure Boot, TPM) e al recupero del sensore sono
descritte in [`docs/bios.md`](bios.md).

## 3. Installare il servizio

Il lettore è gestito da **`goodixfpd`**, un servizio che parla la stessa
lingua D-Bus di fprintd (`net.reactivated.Fprint`). Un drop-in di
`fprintd.service` avvia `goodixfpd` al posto del demone di fprintd. Per tutto
il resto del sistema non cambia niente: `pam_fprintd`, i comandi `fprintd-*`,
GNOME, KDE e la GUI del progetto continuano a usare "fprintd". Se il lettore
Goodix non c'è, `goodixfpd` avvia il vero fprintd, quindi gli altri lettori
continuano a funzionare.

```sh
git clone <questo repository> goodix-driver
cd goodix-driver/packaging/goodixfpd
makepkg -si
```

`makepkg` compila, esegue i test e installa. Poi:

```sh
fprintd-list "$USER"     # deve citare "Goodix fingerprint (27c6:5125/5135)"
```

**Se avevi il vecchio pacchetto `libfprint-goodixgf`**, prima torna alla
libfprint della distribuzione, poi installa:

```sh
sudo pacman -S libfprint                       # rimuove libfprint-goodixgf
cd goodix-driver/packaging/goodixfpd && makepkg -si
```

Le impronte registrate col vecchio driver non valgono più: **registrale di
nuovo** (passo 4). Il warm-up al boot (`goodixgf-warmup.service`) non serve
più, lo fa il servizio da solo, e l'installazione lo disattiva.

**Cosa succede al primo avvio del sensore.** Il servizio genera una chiave
casuale (PSK) per il canale cifrato col sensore, la scrive nel sensore e la
conserva in `/var/lib/fprint/goodix/psk.bin` (solo root). La scrittura tocca
solo l'area della chiave, è reversibile e **non modifica mai il firmware**: il
progetto non contiene firmware e non lo riscrive. Se in futuro Windows
riscrive la propria chiave (dual boot), Linux se ne accorge e rifà questa
procedura da solo. Se avevi già usato il vecchio pacchetto, la chiave salvata
viene riutilizzata.

Verifica:
```sh
journalctl -u fprintd -b | grep -E "goodix\].*(PSK|TLS|init complete)|Sensor ready"
# ... PSK valid ...  TLS handshake done ...  === device init complete ===  Sensor ready
```

### Altre distribuzioni

Servono meson, un compilatore C e le librerie di sviluppo di GLib, polkit,
libusb e OpenSSL. Il percorso del vero fprintd cambia da distribuzione a
distribuzione: è quello della riga `ExecStart` in `systemctl cat fprintd.service`
(su Arch `/usr/lib/fprintd`, altrove spesso `/usr/libexec/fprintd`).

```sh
cd goodix-driver/goodixfpd
meson setup build --prefix=/usr/local -Dfprintd_path=/usr/libexec/fprintd \
      -Dsystemd_dropin_dir=/etc/systemd/system/fprintd.service.d
meson test -C build
sudo meson install -C build
sudo systemctl daemon-reload && sudo systemctl restart fprintd
```

Tutto finisce in `/usr/local` e in `/etc`, dove il gestore di pacchetti non
scrive: gli aggiornamenti della distribuzione non lo sovrascrivono.

## 4. Registrare l'impronta

### Con la GUI

```sh
sudo pacman -S pyside6
python gui/fingerprint_manager.py
```

Scegli il dito, premi **Registra** e segui le indicazioni. Il primo tocco
controlla che quel dito non sia già registrato; poi servono **da 15 a 30
tocchi**. Appoggia il dito come quando sblocchi, ma spostalo un poco a ogni
tocco (più in alto, più in basso, a sinistra, a destra, leggermente ruotato):
il sensore vede solo circa 4×3 mm di dito per volta e l'impronta salvata è
l'insieme di queste zone. Un tocco nella stessa posizione di uno già preso
viene chiesto di nuovo; quando non trovi più posizioni nuove (4 tocchi
ripetuti di fila, dopo almeno 15) la registrazione si chiude da sola. Se
compare "tocco non valido", premi ben piatto: il controllo qualità scarta i
tocchi troppo leggeri. Con **Prova** verifichi subito.

**L'impronta migliora con l'uso.** Quando un tocco viene riconosciuto con
margine ma cade al bordo delle zone registrate, il servizio lo aggiunge
all'impronta, come fa Windows: la zona riconosciuta si allarga da sola. Le
zone della registrazione non vengono mai sostituite; quelle imparate sono al
massimo 30 e si rinnovano dalla più vecchia. Per disattivarlo:
`Environment=GOODIX_LEARN=0` in un drop-in di fprintd.

La GUI gestisce anche più dita e più utenti e permette di cancellare le
impronte. Parla solo con fprintd: non tocca mai direttamente il sensore.

### Da terminale

```sh
fprintd-enroll -f right-index-finger
fprintd-verify
```

## 5. Login con l'impronta (PAM)

**Regola anti-lockout.** L'impronta va sempre messa come `sufficient` **prima**
della password, mai `required`. Così, se fallisce o scade, il sistema chiede la
password come sempre. Mentre modifichi i file PAM tieni **un terminale root già
aperto** (`sudo -i`), finché non hai verificato che tutto funziona.

La riga da aggiungere è sempre la stessa:

```
auth sufficient pam_fprintd.so max-tries=10 timeout=20
```

`max-tries=10` concede dieci tocchi (un tocco fuori dalla zona registrata
diventa un semplice "riprova"), `timeout=20` concede 20 secondi prima di
passare alla password.

| Dove | File | Posizione |
|---|---|---|
| `sudo` | `/etc/pam.d/sudo` | prima riga `auth` |
| Greeter greetd (es. noctalia-greeter) | `/etc/pam.d/greetd` | prima riga `auth` |
| SDDM | `/etc/pam.d/sddm` | prima riga `auth` |
| Blocco schermo | `/etc/pam.d/<il tuo locker>` (es. `hyprlock`, `swaylock`, `kde`) | prima riga `auth` |
| Console (`vlock`) | `/etc/pam.d/vlock` | prima riga `auth` |
| GDM | integrato | si attiva dalle impostazioni di GNOME |

Prova subito:
```sh
sudo -k; sudo -v      # deve chiedere il dito; con il dito giusto entra senza password
```

**noctalia-greeter.** Per poter scegliere tra impronta e password al login
grafico, nel `greeter.toml` del greeter serve:
```toml
[auth]
allow_empty_password = true
```
Allora al login tocchi per entrare oppure scrivi la password.

## 6. Sblocco del keyring via TPM (facoltativo)

Se entri con l'impronta, il portachiavi GNOME non riceve la password e resta
bloccato. [`service/keyring-tpm/`](../service/keyring-tpm/README.md) sigilla la
sua password nel TPM, legata a Secure Boot, come fa Windows Hello.
**Richiede Secure Boot attivo**: [`docs/secure-boot.md`](secure-boot.md).

```sh
cd service/keyring-tpm && ./install.sh   # poi logout e nuovo login
```

## 7. Aggiornamenti

Non c'è niente da fare: `pacman -Syu` / `paru -Syu` aggiornano libfprint e
fprintd come sempre, e `goodixfpd` non dipende dalle loro parti interne,
solo dall'interfaccia D-Bus pubblica di fprintd, stabile da anni.

L'unico caso che richiede un intervento è raro: una nuova versione **binaria**
di una libreria usata dal servizio (per esempio il passaggio a OpenSSL 4).
Allora il servizio non si carica più: il lettore si ferma, ma sudo e il login
chiedono la password come sempre. L'hook del pacchetto lo dice alla fine
dell'aggiornamento:

```
!! goodixfpd non si carica piu': mancano libssl.so.4
```

e basta ricompilare:

```sh
cd goodix-driver/packaging/goodixfpd && makepkg -sif
```

Lo segnala anche `checkrebuild` (rebuild-detector, presente su CachyOS), come
per qualsiasi pacchetto compilato in locale.

**Superficie d'attacco.** Il servizio non è esposto in rete. Gira come fprintd:
stesso utente root, stessa protezione systemd (`ProtectSystem=strict`, accesso
solo ai dispositivi USB, niente rete), stesse regole polkit per chi può
registrare, verificare o agire su un altro utente. I suoi input sono il sensore
USB, le chiamate D-Bus autorizzate e le impronte in `/var/lib/fprint/goodixfpd/`
(file 0600 in cartelle 0700, scrivibili solo da root, letti con controllo di
formato e di appartenenza). Il codice più specifico, e quindi più a rischio, è
il nostro: protocollo USB e TLS, matcher. Il bug più grave trovato finora, le
dita sbagliate accettate, stava proprio lì ed è stato corretto
(`docs/how-it-works.md`).

## 8. Disinstallare

```sh
# togli la riga pam_fprintd dai file PAM modificati al passo 5
fprintd-delete "$USER"         # facoltativo: cancella le impronte
sudo pacman -R goodixfpd       # torna il fprintd della distribuzione
```

Senza `goodixfpd` il lettore Goodix non funziona più (libfprint ufficiale non
lo supporta), e tutto il resto torna esattamente com'era.

## 9. Risoluzione problemi

Il primo strumento è sempre il log:
```sh
journalctl -u fprintd -b --no-pager | grep -E "goodix|Frame signal|Print|Learned"
```

(il servizio gira dentro l'unità `fprintd.service`, quindi il log è quello).

**`fprintd-list` non vede il sensore.** Controlla `lsusb` e che il pacchetto sia
installato (`pacman -Q goodixfpd`), poi `sudo systemctl restart fprintd`. Se
nel log c'è "No Goodix 27c6:5125/5135 reader, starting fprintd instead", il
servizio non ha trovato il lettore sul bus USB.

**Il dito non viene mai letto al login subito dopo l'accensione.** Al boot il
sensore è freddo: al primo uso dopo ogni avvio il servizio scarta la baseline
salvata e la ricampiona (nel log: "New boot: stored finger-detect baseline
discarded"). Durante quella prima apertura non tenere il dito sul sensore.

**Il lettore si ferma dopo una sospensione.** Se il sensore smette di
rispondere durante una verifica, dopo pochi secondi il servizio chiude
l'operazione ("Sensor lost") e il login passa alla password; il tentativo
successivo riapre il sensore da capo.

**"Tocco non valido" molto spesso.** Il controllo qualità scarta i tocchi
leggeri. Nel log, `frame signal ... ridge=` è il contrasto delle creste: un
tocco buono sta oltre 100, la soglia è 10. Premi il dito ben piatto. Se le
creste sono valide ma scartate, puoi abbassare la soglia con
`Environment=GOODIX_MIN_RIDGE=5` in un drop-in di fprintd.

**Riconosce poco (molti rifiuti).** Ogni rifiuto è un tocco caduto fuori dalle
zone registrate. Con l'uso l'impronta si allarga da sola (passo 4); se i
rifiuti restano tanti, registra di nuovo il dito coprendo la zona che usi
davvero per sbloccare.

**`no image frame received` ripetuto, o il sensore non risponde più.** Riavvia
fprintd. Se non basta, il sensore può restare bloccato fino al ripristino da
Windows: si recupera con una VM Windows con il driver Goodix originale e il
sensore passato via USB. La procedura completa è in
[`docs/recovery.md`](recovery.md); serve `Virtualization Technology` attivo nel
BIOS. Dopo il recupero, il driver Linux riscrive la propria chiave da solo.

**Diagnosi approfondita / taratura.** Con `Environment=GOODIX_DUMP_IMGPROC=1`
il servizio salva i frame grezzi in `/var/lib/fprint/goodix/` (solo per la
taratura: toglilo e cancella i file dopo). Il test `template` di
`goodixfpd/` rifà offline pipeline e confronto su una cartella di catture
(`meson setup build -Ddataset=<cartella>`) e misura FRR e FAR.

**Log di debug manuale.**
```sh
sudo systemctl edit fprintd        # aggiungi:  [Service]  Environment=G_MESSAGES_DEBUG=goodixfpd
sudo systemctl restart fprintd     # (rimuovi il drop-in quando hai finito)
```
