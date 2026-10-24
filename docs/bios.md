# BIOS / firmware (Huawei MateBook, InsydeH20)

Riferimento: MateBook **CREM-WXX9**, AMD Ryzen 7 5800H, BIOS **InsydeH20 2.32**
(EC 2.32), TPM firmware AMD (**fTPM**). Altri modelli Huawei/Honor con lo stesso
sensore hanno un BIOS molto simile.

**Il lettore di impronte non richiede nessuna impostazione nel BIOS.** Il BIOS
serve solo per le parti opzionali di sicurezza (Secure Boot e TPM, vedi
`SECURITY.md`) e per il recupero del sensore tramite macchina virtuale.

## Entrare nel BIOS

Accendi e premi ripetutamente **F2** al logo Huawei (**F12** apre il menu di
avvio). Il sensore è nel tasto di accensione: premere il tasto equivale a
toccare il sensore, e non è un problema.

L'Insyde di Huawei è una pagina unica. Ti muovi con le frecce, cambi un valore
con F5/F6 o Invio, salvi ed esci con F10.

## Le voci che contano

| Voce | Valore usato | A cosa serve qui |
|---|---|---|
| **Secure Boot** | `Enable` | Base per il keyring sigillato nel TPM (PCR 7) e per LUKS+TPM. Procedura in `docs/secure-boot.md`. |
| **TPM** | `fTPM` | TPM firmware AMD, usato da `service/keyring-tpm/` e da LUKS. Deve restare attivo. |
| **Clear TPM** | non toccare | Cancella tutti i segreti sigillati (vedi sotto). |
| **SETUP Administrator Password** | consigliata | Senza password chiunque può spegnere Secure Boot dal BIOS e rendere inutile il sigillo su PCR 7. |
| **POST Password** | facoltativa | Password all'accensione, indipendente da tutto il resto. |
| **Virtualization Technology** | `Enable` | Serve solo per recuperare il sensore con la VM Windows (`docs/recovery.md`). |
| **HDD Device Enable** / **Bootable Device** | come da fabbrica | Ordine e abilitazione dei dischi di avvio. |
| **PXE Device Enable** | `Disable` | Avvio da rete, non serve. |

### Secure Boot e Setup Mode

Per usare chiavi proprie (`sbctl enroll-keys`) il firmware deve essere in
**Setup Mode**. Su questo esemplare lo era già di fabbrica: `sbctl status`
mostrava `Setup Mode: Enabled` e il passaggio di cancellazione delle chiavi non
è servito. Questo BIOS non espone un menu di gestione delle chiavi Secure Boot.
Se il tuo non è in Setup Mode, verifica le opzioni disponibili sul tuo modello
prima di procedere (non verificato qui).

Dopo `sbctl enroll-keys` il firmware esce da solo dal Setup Mode e Secure Boot
diventa effettivo. **Rollback:** se il sistema non si avvia, metti
`Secure Boot` a `Disable`.

### Cosa succede con "Clear TPM", reset CMOS o aggiornamento del BIOS

Su fTPM tutte e tre le operazioni possono azzerare i segreti sigillati:

- **keyring via TPM**: lo sblocco automatico smette di funzionare e il keyring
  chiede di nuovo la password utente (nessun lock-out). Per riattivarlo rilancia
  `service/keyring-tpm/seal.sh`.
- **LUKS+TPM**: il disco chiede la passphrase all'avvio, perciò tienine sempre
  una oltre al TPM.
- **impronte**: non dipendono dal TPM e non cambiano nulla.

Anche cambiare lo stato di Secure Boot o le chiavi modifica PCR 7, con lo stesso
effetto (una richiesta di password, poi si ri-sigilla).
