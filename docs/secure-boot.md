# Secure Boot on CachyOS + limine (with sbctl)

Enabling Secure Boot with your own keys. It is the security basis for the
keyring unlock via TPM (PCR 7) and for LUKS+TPM: without Secure Boot, PCR 7
does not bind the seal to your OS (a live USB with SB off would reproduce the
same value).

Reference stack: CachyOS, **limine** bootloader + `mkinitcpio`, ESP on `/boot`.
With limine you do **not** sign the kernels by hand: limine verifies them with
BLAKE2B hashes in `limine.conf`; the firmware verifies only limine's EFI
binary. Signing the kernels with `sbctl-batch-sign` BREAKS boot (hash panic).
The CachyOS pipeline signs kernels and EFI binaries by itself on updates
(sbctl post-hook + `limine-update`), so updates do **not** leave you with an
unsigned kernel.

## Procedure (order matters: sign FIRST, turn SB on LAST)

```sh
# 0. Tooling
sudo pacman -S --needed sbctl
sudo sbctl status        # look at "Setup Mode" and "Secure Boot"

# 1. If "Setup Mode" = Disabled: enter the BIOS -> clear/reset the Secure Boot
#    keys (this puts the firmware in Setup Mode), leave SB off, come back here.
#    If "Setup Mode" = Enabled: skip this step.

# 2. Keys + signing limine (SB still off)
sudo sbctl create-keys
sudo sbctl enroll-keys -m            # -m = keep the Microsoft keys (option ROMs/GPU)
sudo limine-enroll-config
sudo limine-update                   # signs /boot/EFI/limine/limine_x64.efi
sudo sbctl sign -s /boot/EFI/BOOT/BOOTX64.EFI   # fallback loader
sudo sbctl verify                    # limine + BOOTX64 = signed; unsigned kernels = ok

# 3. BIOS: turn Secure Boot on, save, reboot.

# 4. Check
sudo sbctl status                    # Setup Mode: Disabled, Secure Boot: Enabled
bootctl status | grep -i secure      # "Secure Boot: enabled (user)"
```

Note on the `sbctl` commands: they return a non-zero exit code even on
success; in fish an `&&` / `; and` chain breaks → use a plain `;`.

## Rollback

No data is touched. If it does not boot: BIOS → Secure Boot: **Disabled** → it
starts again. Full reset: live USB → chroot → `sbctl reset`, then disable SB in
the BIOS.

## fTPM (AMD)

On AMD the TPM is often an `fTPM` (firmware TPM). A `Clear TPM`, a CMOS reset
or a BIOS update can wipe the sealed secrets and change PCR 7 → the seals
(keyring, LUKS) have to be created again. The planned fallbacks avoid any
lockout.
