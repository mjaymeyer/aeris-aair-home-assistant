# USB Setup (No Hotspot)

Writes the Wi-Fi and MQTT settings straight into a purifier over USB, so the firmware boots onto your
network without the `Aeris-XXXX` hotspot and the settings page. It can also install a newer firmware build
while keeping a unit's settings (normally every new build resets them).

How it works: a small helper app (`firmware/`) goes on the unit first. `provision.mjs` uses it to write
the settings, signed for the firmware you are about to install, and reads them back to check them. Then
you flash the real firmware. The helper stays offline and only touches the settings.

## Requirements

- The unit already runs Device OS 3.3.1 (see the bootloader and system steps in the main README).
- dfu-util and the Particle CLI, as in the main README.
- Node.js 18 or newer; run `npm install` in this folder once.
- The helper binary, built like the main firmware (free Particle account):
  `particle compile photon firmware --target 3.3.1 --saveTo usb-setup.bin`

## Set Up a Unit

1. Put the unit in DFU mode (blinking yellow): `particle usb dfu`, the ENTER DFU MODE button on the
   unit's page, or the RESET+SETUP buttons.
2. Flash the helper:
   `dfu-util -d 2b04:d006 -a 0 -s 0x080A0000:leave -D usb-setup.bin`
3. Write the settings. Passwords come from environment variables (Windows CMD: `set NAME=value`):

```
export AERIS_WIFI_PASS='wifi-password' AERIS_MQTT_PASS='mqtt-password'
node provision.mjs setup --app aerisFirmware.bin --prefix aeris/bedroom/ --ssid MyWiFi \
  --mqtt-host 192.168.0.100 --mqtt-user mqtt_user
```

4. Put the unit in DFU mode again and flash the firmware named in `--app`:
   `dfu-util -d 2b04:d006 -a 0 -s 0x080A0000:leave -D aerisFirmware.bin`

The unit joins Wi-Fi and starts publishing under the prefix. Display settings are the firmware defaults;
change them on the unit's page as usual. To validate the options without a unit, run the same command
with `check` instead of `setup`.

## Upgrade Without Losing Settings

Same steps, but in step 3 run:
`node provision.mjs upgrade --from <firmware now on the unit>.bin --app <new firmware>.bin`

It refuses if the unit's settings were not made by the `--from` build. Afterwards flash only the new
firmware: the old one would no longer match the settings and would reset them.

## Notes

- Limits: Wi-Fi password 63 characters, MQTT user and password 31.
- `--backup <file>` saves the unit's previous settings first (the file contains passwords).
- Several Photons on USB: add `--device <device id>` (from `particle usb list`).
- If a step fails, start again from step 1. Exception: after a successful `upgrade` write, just flash
  the new firmware.
