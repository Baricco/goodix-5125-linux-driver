# BIOS / firmware (Huawei MateBook, InsydeH20)

Reference: MateBook **CREM-WXX9**, AMD Ryzen 7 5800H, BIOS **InsydeH20 2.32**
(EC 2.32), AMD firmware TPM (**fTPM**). Other Huawei/Honor models with the
same sensor have a very similar BIOS.

**The fingerprint reader needs no BIOS setting.** The BIOS matters only for
the optional security parts (Secure Boot and TPM, see `SECURITY.md`) and for
recovering the sensor through a virtual machine.

## Entering the BIOS

Power on and press **F2** repeatedly at the Huawei logo (**F12** opens the
boot menu). The sensor is in the power button: pressing the button also
touches the sensor, and that is not a problem.

Huawei's Insyde setup is a single page. Move with the arrow keys, change a
value with F5/F6 or Enter, save and exit with F10.

## The settings that matter

| Setting | Value used | What it is for here |
|---|---|---|
| **Secure Boot** | `Enable` | Basis for the keyring sealed in the TPM (PCR 7) and for LUKS+TPM. Procedure in `docs/secure-boot.md`. |
| **TPM** | `fTPM` | AMD firmware TPM, used by `service/keyring-tpm/` and by LUKS. Must stay enabled. |
| **Clear TPM** | do not touch | Erases every sealed secret (see below). |
| **SETUP Administrator Password** | recommended | Without it anyone can turn Secure Boot off from the BIOS and make the PCR 7 seal useless. |
| **POST Password** | optional | Power-on password, independent of everything else. |
| **Virtualization Technology** | `Enable` | Needed only to recover the sensor with the Windows VM (`docs/recovery.md`). |
| **HDD Device Enable** / **Bootable Device** | factory default | Order and enabling of boot disks. |
| **PXE Device Enable** | `Disable` | Network boot, not needed. |

### Secure Boot and Setup Mode

To use your own keys (`sbctl enroll-keys`) the firmware must be in **Setup
Mode**. On this unit it already was from the factory: `sbctl status` showed
`Setup Mode: Enabled` and the key-clearing step was not needed. This BIOS
exposes no Secure Boot key management menu. If yours is not in Setup Mode,
check the options available on your model before going ahead (not verified
here).

After `sbctl enroll-keys` the firmware leaves Setup Mode by itself and Secure
Boot takes effect. **Rollback:** if the system does not boot, set
`Secure Boot` to `Disable`.

### What happens with "Clear TPM", a CMOS reset or a BIOS update

On an fTPM all three can wipe the sealed secrets:

- **keyring via TPM**: automatic unlock stops working and the keyring asks for
  the user password again (no lockout). To re-enable it, run
  `service/keyring-tpm/seal.sh` again.
- **LUKS+TPM**: the disk asks for the passphrase at boot, so always keep one
  besides the TPM.
- **fingerprints**: they do not depend on the TPM and nothing changes.

Changing the Secure Boot state or keys also changes PCR 7, with the same
effect (one password prompt, then it seals again).
