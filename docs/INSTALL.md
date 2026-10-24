# Guida: impronta digitale su Linux per Goodix 27c6:5125

Dalla prima installazione al login con l'impronta, passo passo. Ogni sezione
dice cosa fa e come verificare che abbia funzionato. Se qualcosa va storto,
vai a [Risoluzione problemi](#9-risoluzione-problemi).

Indice:

1. [Requisiti](#1-requisiti)
2. [BIOS](#2-bios)
3. [Installare il driver](#3-installare-il-driver)
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
- **Arch Linux o derivate** (CachyOS, EndeavourOS, Manjaro…): il driver si
  installa come pacchetto pacman. Su altre distribuzioni vale la stessa serie
  di patch (`packaging/libfprint-goodixgf/*.patch`) sopra libfprint 1.94.100,
  ma il pacchetto va costruito col sistema della distribuzione.
- `fprintd` e `pam_fprintd` della distribuzione (`sudo pacman -S fprintd`).
- Un utente con `sudo`.

## 2. BIOS

Per l'impronta **non serve toccare il BIOS**. Le impostazioni utili alle parti
di sicurezza facoltative (Secure Boot, TPM) e al recupero del sensore sono
descritte in [`docs/bios.md`](bios.md).

## 3. Installare il driver

Il driver è il pacchetto `libfprint-goodixgf`: la libfprint **ufficiale** della
versione dei repository, più due cose:

- **SIGFM**, il matcher per sensori piccoli, dalla merge request upstream !530;
- il **driver `goodixgf`** per questo sensore.

Il pacchetto sostituisce `libfprint` (stesse API: fprintd e pam_fprintd restano
quelli della distribuzione) e contiene **tutti** i driver standard, quindi
eventuali altri lettori continuano a funzionare.

```sh
git clone <questo repository> goodix-driver
cd goodix-driver/packaging/libfprint-goodixgf
makepkg -si
```

`makepkg` scarica la libfprint ufficiale, applica le patch, compila, esegue i
test e installa. Quando pacman chiede se rimuovere `libfprint` perché va in
conflitto, rispondi **sì**. Poi:

```sh
sudo systemctl enable goodixgf-warmup.service   # baseline a freddo a ogni boot
sudo systemctl restart fprintd
fprintd-list "$USER"                            # deve mostrare "Goodix fingerprint (27c6:5125/5135)"
```

**Cosa succede al primo avvio del sensore.** Il driver genera una chiave casuale
(PSK) per il canale cifrato col sensore, la scrive nel sensore e la conserva in
`/var/lib/fprint/goodix/psk.bin` (solo root). La scrittura tocca solo l'area
della chiave, è reversibile e **non modifica mai il firmware**: questa build non
contiene firmware e non lo riscrive. Se in futuro Windows riscrive la propria
chiave (dual boot), Linux se ne accorge e rifà questa procedura da solo.

Verifica:
```sh
journalctl -u fprintd -b | grep -E "goodix\].*(PSK|TLS|init complete)"
# ... PSK valid / PSK written ...  TLS handshake done ...  === device init complete ===
```

## 4. Registrare l'impronta

### Con la GUI

```sh
sudo pacman -S pyside6
python gui/fingerprint_manager.py
```

Scegli il dito, premi **Registra** e segui le indicazioni. Servono **circa 30
tocchi**. Appoggia il dito come quando sblocchi, ma spostalo un poco a ogni
tocco (più in alto, più in basso, a sinistra, a destra, leggermente ruotato):
il sensore vede solo circa 4×3 mm di dito per volta e il template è l'insieme
di queste zone. Se compare "tocco non valido", premi ben piatto: il controllo
qualità scarta i tocchi troppo leggeri. Con **Prova** verifichi subito.

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

Il pacchetto `libfprint-goodixgf` sostituisce `libfprint`, quindi `pacman -Syu`
non lo sovrascrive mai e il lettore non si rompe con gli aggiornamenti. Quando
la distribuzione pubblica una libfprint più recente, l'hook del pacchetto lo
segnala alla fine di `pacman -Syu`:

```
>> libfprint 1.94.101 disponibile nei repo (libfprint-goodixgf e' su 1.94.100).
   Dalla cartella del progetto:  packaging/goodixgf-update
```

Allora:
```sh
git pull                       # prendi eventuali patch aggiornate
packaging/goodixgf-update      # porta il PKGBUILD alla nuova versione, ricompila, installa
```

Lo script riapplica le stesse patch alla nuova versione ufficiale. Se una patch
non si applica più, la build si ferma e **resta installata la versione
precedente**: il lettore continua a funzionare finché le patch non vengono
aggiornate in questo repository.

## 8. Disinstallare

```sh
# togli la riga pam_fprintd dai file PAM modificati al passo 5
sudo systemctl disable goodixgf-warmup.service
sudo pacman -S libfprint       # torna alla libfprint ufficiale (rimuove libfprint-goodixgf)
```

Le impronte registrate restano in `/var/lib/fprint/` finché non le cancelli
(`fprintd-delete "$USER"`).

## 9. Risoluzione problemi

Il primo strumento è sempre il log:
```sh
journalctl -u fprintd -b --no-pager | grep -E "goodix|sigfm|frame signal"
```

**`fprintd-list` non vede il sensore.** Controlla `lsusb` e che il pacchetto sia
installato (`pacman -Q libfprint-goodixgf`), poi lancia
`sudo systemctl restart fprintd`.

**Il dito non viene mai letto al login subito dopo l'accensione.** Al boot il
sensore è freddo. Il servizio `goodixgf-warmup.service` (passo 3) fa
ricampionare la baseline a freddo: verifica che sia abilitato.

**"Tocco non valido" molto spesso.** Il controllo qualità scarta i tocchi
leggeri. Nel log, `frame signal ... ridge=` è il contrasto delle creste: un
tocco buono sta oltre 100, la soglia è 10. Premi il dito ben piatto. Se le
creste sono valide ma scartate, puoi abbassare la soglia con
`Environment=GOODIX_MIN_RIDGE=5` in un drop-in di fprintd.

**Riconosce poco (molti rifiuti).** Ogni rifiuto è un tocco caduto fuori dalle
zone registrate. Registra di nuovo il dito, coprendo la zona che usi davvero
per sbloccare.

**`no image frame received` ripetuto, o il sensore non risponde più.** Riavvia
fprintd. Se non basta, il sensore può restare bloccato fino al ripristino da
Windows: si recupera con una VM Windows con il driver Goodix originale e il
sensore passato via USB. La procedura completa è in
[`docs/recovery.md`](recovery.md); serve `Virtualization Technology` attivo nel
BIOS. Dopo il recupero, il driver Linux riscrive la propria chiave da solo.

**Diagnosi approfondita / taratura.** [`tools/fp_tune/`](../tools/fp_tune) è il
banco di prova. `calib_session.sh` registra il dito, cattura 25 tocchi genuini
e 25 di altre dita con i frame grezzi e misura FRR e FAR dal vivo.
`analyze_session.py` rifà tutto offline con il codice reale del driver. Dump e
log di debug vengono spenti e cancellati a fine sessione.

**Log di debug manuale.**
```sh
sudo systemctl edit fprintd        # aggiungi:  [Service]  Environment=G_MESSAGES_DEBUG=all
sudo systemctl restart fprintd     # (rimuovi il drop-in quando hai finito)
```
