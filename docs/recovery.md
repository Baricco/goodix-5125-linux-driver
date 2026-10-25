# Recupero del sensore bloccato (VM Windows)

Raramente, dopo un crash a metà della sessione cifrata, il sensore entra in uno
stato in cui **si vede su USB ma non risponde a nessun comando**. Il reset USB,
il riavvio e anche lo spegnimento completo non bastano, perché il sensore nel
tasto di accensione resta alimentato. Il driver Goodix per Windows lo
reinizializza: basta farlo girare in una macchina virtuale con il sensore
passato via USB. Procedura provata su MateBook CREM-WXX9, firmware 12508.

Prima di tutto prova le cose semplici: `sudo systemctl restart fprintd`, poi un
riavvio.

## Cosa serve

- `Virtualization Technology` attivo nel BIOS ([`docs/bios.md`](bios.md)).
- QEMU/KVM, per esempio con [quickemu](https://github.com/quickemu-project/quickemu),
  e una VM Windows 10/11.
- Il driver Goodix **1.1.125.12 o 1.1.125.13** dal Microsoft Update Catalog
  (cerca `27c6 5125`). Queste versioni contengono lo **stesso firmware 12508**
  del sensore. Le versioni da 1.1.125.14 in poi contengono firmware più nuovi
  (12509, 12512) e **lo flasherebbero sul sensore**: non usarle.

## Regole per non far aggiornare il firmware

1. **Rete della VM spenta** prima di collegare il sensore, così Windows Update
   non scarica un driver più nuovo. Con quickemu: monitor HMP, `set_link <nic> off`.
   L'impostazione si azzera a ogni riavvio della VM.
2. Installa nella VM **solo** il driver 1.1.125.13 (`pnputil /add-driver <inf> /install`).
3. Nel registro della VM imposta
   `HKLM\SOFTWARE\Goodix\FP\FirmwareUpdateSwitch = 0` (DWORD).

## Procedura

1. Su Linux ferma chi usa il sensore: `sudo systemctl stop fprintd`.
2. Avvia la VM e collega il sensore dal monitor QEMU, sul bus **xhci** (con
   quickemu si chiama `spicepass.0`; su un bus EHCI il sensore dà errore di
   velocità):
   ```
   device_add usb-host,vendorid=0x27c6,productid=0x5125,id=goodix,bus=spicepass.0
   ```
3. In Windows il dispositivo compare in Gestione dispositivi (Biometria) con
   stato OK. Forza la reinizializzazione da PowerShell amministratore:
   ```powershell
   $d = Get-PnpDevice | Where-Object { $_.InstanceId -like '*VID_27C6&PID_5125*' }
   Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false
   Enable-PnpDevice  -InstanceId $d.InstanceId -Confirm:$false
   ```
4. Scollega il sensore dalla VM (`device_del goodix` nel monitor) e spegni la VM.
5. Su Linux: `sudo systemctl start fprintd`, poi `fprintd-verify`.

Windows ha riscritto la **propria** chiave PSK nel sensore. Al primo avvio il
driver Linux se ne accorge ("local PSK mismatch with MCU") e scrive di nuovo la
sua: non devi fare niente. Le impronte registrate restano valide.

## Prevenzione

Il blocco profondo viene da sessioni interrotte a metà handshake (crash ripetuti
di chi tiene aperto il sensore). `goodixfpd` chiude sempre la sessione in modo
pulito (anche quando il client sparisce, come fa pam_fprintd dopo un accesso
riuscito) e resta residente, senza riavvii a ogni accesso.
