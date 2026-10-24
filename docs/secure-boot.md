# Secure Boot su CachyOS + limine (con sbctl)

Abilitare Secure Boot con chiavi proprie. Serve come base di sicurezza per lo
sblocco del keyring via TPM (PCR 7) e per LUKS+TPM: senza Secure Boot, PCR 7 non
lega il sigillo al tuo OS (un live USB con SB off riprodurrebbe lo stesso valore).

Stack di riferimento: CachyOS, bootloader **limine** + `mkinitcpio`, ESP su `/boot`.
Su limine **non** si firmano i kernel a mano: limine li verifica con hash BLAKE2B
in `limine.conf`; il firmware verifica solo l'EFI di limine. Firmare i kernel con
`sbctl-batch-sign` ROMPE il boot (hash panic). La pipeline CachyOS firma kernel ed
EFI da sola agli update (post-hook sbctl + `limine-update`), quindi gli
aggiornamenti **non** ti lasciano un kernel non firmato.

## Procedura (l'ordine conta: firma PRIMA, accendi SB PER ULTIMO)

```sh
# 0. Tooling
sudo pacman -S --needed sbctl
sudo sbctl status        # guarda "Setup Mode" e "Secure Boot"

# 1. Se "Setup Mode" = Disabled: entra in BIOS -> cancella/azzera le chiavi
#    Secure Boot (mette il firmware in Setup Mode), lascia SB spento, torna qui.
#    Se "Setup Mode" = Enabled: salta questo passo.

# 2. Chiavi + firma di limine (SB ancora spento)
sudo sbctl create-keys
sudo sbctl enroll-keys -m            # -m = tieni le chiavi Microsoft (option ROM/GPU)
sudo limine-enroll-config
sudo limine-update                   # firma /boot/EFI/limine/limine_x64.efi
sudo sbctl sign -s /boot/EFI/BOOT/BOOTX64.EFI   # fallback loader
sudo sbctl verify                    # limine + BOOTX64 = signed; kernel unsigned = ok

# 3. BIOS: accendi Secure Boot, salva, riavvia.

# 4. Verifica
sudo sbctl status                    # Setup Mode: Disabled, Secure Boot: Enabled
bootctl status | grep -i secure      # "Secure Boot: enabled (user)"
```

NB sui comandi `sbctl`: ritornano un exit-code non-zero anche in caso di successo;
in fish la catena `&&` / `; and` si spezza → usare `;` secco.

## Rollback

Nessun dato toccato. Se non avvia: BIOS → Secure Boot: **Disabled** → riparte.
Reset completo: USB live → chroot → `sbctl reset`, poi disattiva SB in BIOS.

## fTPM (AMD)

Su AMD il TPM è spesso `fTPM` (firmware). Un `Clear TPM`, un reset CMOS o un
update del BIOS possono azzerare i segreti sigillati e cambiare PCR 7 → i sigilli
(keyring, LUKS) vanno ri-creati. I fallback previsti evitano il lock-out.
