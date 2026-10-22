# Login a impronta con goodixfpd (Strada A)

Integrazione dell'impronta nello stack di autenticazione Linux **senza** il driver C: un demone
di sistema (`service/goodixfpd.py`) possiede il sensore ed espone una verify su socket Unix; PAM
lo interroga con un piccolo client (`service/goodixfp-verify`). Copre `sudo`, lo sblocco schermo e
il login al greeter. È la via rapida per usarlo *adesso*; la via canonica e pubblicabile per tutti
resta il driver libfprint (Strada B, `docs/porting-c.md`).

> **Regola d'oro anti-lockout:** l'impronta è sempre un fattore `sufficient` con *fallback alla
> password*. Non metterla mai `required` da sola. Durante il setup tieni **una shell root già
> aperta** (`sudo -i` in un altro terminale) finché non hai verificato che tutto funziona.

## Architettura

```
 [PAM: sudo / login]  →  goodixfp-verify  →  /run/goodixfpd/sock  →  goodixfpd  →  sensore
                         (client, root)        (0660 root:goodixfp)   (utente goodixfp)
```

- Il demone gira come utente di sistema **`goodixfp`**, non root; accede al sensore via gruppo
  `goodixfp` (regola udev `70-goodixfp.rules`).
- Verifica il chiamante con `SO_PEERCRED`: accetta solo root (lo stack PAM) o l'utente stesso.
- Le impronte stanno in `/var/lib/goodixfp/<utente>/*.npz` (formato `FragmentMapper`).
- Il demone non tocca mai la flash del sensore (niente provisioning E0) e non restituisce mai
  dati biometrici, solo esito + score.

## Installazione

Assumiamo il repository in `/opt/goodix-driver` (adatta i percorsi se diverso; il `.service` e il
`GOODIXFP_REPO` vanno allineati).

```bash
# 1. utente e gruppo di sistema dedicati
sudo groupadd -r goodixfp
sudo useradd  -r -g goodixfp -s /usr/sbin/nologin -d /var/lib/goodixfp goodixfp

# 2. accesso al sensore per il gruppo (sostituisce la vecchia 70-goodix-5125.rules)
sudo cp service/70-goodixfp.rules /etc/udev/rules.d/
sudo rm -f /etc/udev/rules.d/70-goodix-5125.rules
sudo udevadm control --reload && sudo udevadm trigger

# 3. client PAM
sudo install -Dm755 service/goodixfp-verify /usr/local/lib/goodixfp/goodixfp-verify

# 4. demone
sudo cp service/goodixfpd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now goodixfpd
systemctl status goodixfpd          # deve stampare "in ascolto su /run/goodixfpd/sock"
```

## Registrare un'impronta

L'enroll si fa con la GUI (da utente, denso: riempi i buchi rossi fino a "copertura buona"):

```bash
tools/.venv/bin/python tools/enroll_gui.py       # mappa + Salva -> state/fingers/<nome>.npz
```

Poi copia il template nello store di sistema del tuo utente:

```bash
sudo install -Dm640 -o root -g goodixfp \
     state/fingers/<nome>.npz /var/lib/goodixfp/$USER/<nome>.npz
```

Puoi registrare più impronte (più `.npz`): la verify le prova tutte.

## Collaudo SICURO (nell'ordine)

1. **Socket, senza PAM.** Da utente, tocca il sensore quando richiesto:
   ```bash
   GOODIXFP_SOCKET=/run/goodixfpd/sock python service/goodixfp-verify $USER ; echo "esito=$?"
   ```
   `esito=0` = riconosciuto. Ripeti con un altro dito: deve dare `1`.

2. **sudo.** Con la shell root aperta a parte, aggiungi *in cima* a `/etc/pam.d/sudo`:
   ```
   auth  sufficient  pam_exec.so  quiet  /usr/local/lib/goodixfp/goodixfp-verify
   ```
   Apri un nuovo terminale, `sudo -v`: tocca il sensore → niente password. Se il sensore
   fallisce, deve chiedere la password come sempre. Se qualcosa va storto, la shell root già
   aperta ti salva.

3. **Blocco schermo** (es. `/etc/pam.d/system-local-login` o il PAM del tuo lock) e infine **login
   al greeter**, solo dopo che sudo è solido. Stessa riga `sufficient`.

## Limiti noti (onesti)

- **Falsi negativi = copertura.** Il sensore è piccolo e a tocco: la verify accetta se la pressione
  cade su una zona registrata. Enroll denso + un paio di ritenute risolvono. Gli impostori restano
  a 0 (FAR ottimo). Vedi `docs/matching.md`.
- **Latenza.** Il demone apre il sensore on-demand (reset + sync + TLS ≈ qualche secondo). Una
  sessione calda persistente è un miglioramento futuro.
- **Wedge.** Se il sensore si blocca a fondo oggi serve la VM Windows per rianimarlo (vedi stato
  del progetto). Il reset vendor 100% Linux è lo step successivo del piano. Il demone fa teardown
  pulito a ogni verify proprio per non arrivarci.
- **Sicurezza biometria.** L'impronta non è un segreto forte; usala come comodità accanto alla
  password, non come unico fattore su macchine a rischio.
