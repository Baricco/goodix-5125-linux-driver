# Recovering a stuck sensor (Windows VM)

Rarely, after a crash in the middle of the encrypted session, the sensor gets
into a state where **it shows up on USB but answers no command**. A USB reset,
a reboot and even a full power-off are not enough, because the sensor in the
power button stays powered. The Goodix Windows driver reinitializes it: it is
enough to run it in a virtual machine with the sensor passed through over USB.
Procedure tested on a MateBook CREM-WXX9, firmware 12508.

First of all try the simple things: `sudo systemctl restart fprintd`, then a
reboot.

## What you need

- `Virtualization Technology` enabled in the BIOS ([`docs/bios.md`](bios.md)).
- QEMU/KVM, for example with [quickemu](https://github.com/quickemu-project/quickemu),
  and a Windows 10/11 VM.
- The Goodix driver **1.1.125.12 or 1.1.125.13** from the Microsoft Update
  Catalog (search for `27c6 5125`). These versions contain the **same firmware
  12508** as the sensor. Versions from 1.1.125.14 on contain newer firmware
  (12509, 12512) and **would flash it onto the sensor**: do not use them.

## Rules to keep the firmware from being updated

1. **VM network off** before attaching the sensor, so Windows Update does not
   download a newer driver. With quickemu: HMP monitor, `set_link <nic> off`.
   The setting resets at every VM reboot.
2. Install **only** driver 1.1.125.13 in the VM
   (`pnputil /add-driver <inf> /install`).
3. In the VM's registry set
   `HKLM\SOFTWARE\Goodix\FP\FirmwareUpdateSwitch = 0` (DWORD).

## Procedure

1. On Linux stop whatever uses the sensor: `sudo systemctl stop fprintd`.
2. Start the VM and attach the sensor from the QEMU monitor, on the **xhci**
   bus (with quickemu it is called `spicepass.0`; on an EHCI bus the sensor
   gives a speed error):
   ```
   device_add usb-host,vendorid=0x27c6,productid=0x5125,id=goodix,bus=spicepass.0
   ```
3. In Windows the device shows up in Device Manager (Biometric devices) with
   status OK. Force a reinitialization from an administrator PowerShell:
   ```powershell
   $d = Get-PnpDevice | Where-Object { $_.InstanceId -like '*VID_27C6&PID_5125*' }
   Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false
   Enable-PnpDevice  -InstanceId $d.InstanceId -Confirm:$false
   ```
4. Detach the sensor from the VM (`device_del goodix` in the monitor) and shut
   the VM down.
5. On Linux: `sudo systemctl start fprintd`, then `fprintd-verify`.

Windows has written **its own** PSK into the sensor. On the first start the
Linux driver notices ("local PSK mismatch with MCU") and synchronizes again:
it reads the new key back from the sensor or, if it cannot, writes its own.
You do not have to do anything. Enrolled fingerprints stay valid.

## Prevention

The deep lock-up comes from sessions interrupted in the middle of the
handshake (repeated crashes of whatever keeps the sensor open). `goodixfpd`
always closes the session cleanly (even when the client disappears, as
pam_fprintd does after a successful login) and stays resident, without
restarting at every login.
