# esp32-polibot

PoliBOT is an IRC channel guardian that runs entirely on an ESP32-S3: protection against floods, spam and raids, automatic operator and voice, and a web control panel. No server or computer has to stay on.

Written by Gerardo Canosa. To find out more, join **irc.SekurNet.org**, channel **#SekurNet**.

## What you need

- An **ESP32-S3-DevKitC-1** with the **WROOM-1-N16R8** module (16 MB flash, 8 MB PSRAM)
- A USB data cable and a 2.4 GHz WiFi network

## Downloads

Ready-made images are in [`firmware/`](firmware):

| File | Use |
| :--- | :--- |
| `polibot-<version>-full.bin` | First installation, over USB |
| `polibot-<version>-update.bin` | Updating a board that already runs PoliBOT, from its control panel |

Check a download before using it: `shasum -a 256 <file>` (Windows: `certutil -hashfile <file> SHA256`) must print the value listed in [`firmware/SHA256SUMS`](firmware/SHA256SUMS).

## Install on a new board

This **erases the whole board**.

```bash
pip install esptool
esptool.py --chip esp32s3 -p <port> erase_flash
esptool.py --chip esp32s3 -p <port> write_flash 0x0 polibot-<version>-full.bin
```

Then:

1. Join the WiFi network `PoliBOT-Setup` (password `polibotpassword`).
2. Open `http://192.168.4.1/`, choose **Control Panel** and sign in as `admin` / `polibotadmin`.
3. Set a password of your own, then enter your WiFi in **WiFi Setup**.
4. Set up your IRC networks and channels.

## Update

From version 0.2.0 the control panel does it for you: **Firmware & Backup > Online Updates > Check Now**, or switch on the automatic check. Releases are signed, and the board installs only what carries the project's signature.

From an earlier version, or to choose a file yourself: open **Firmware & Backup**, choose `polibot-<version>-update.bin` and upload it. Either way, settings and accounts are kept.

## Documentation

Features, commands and building from source: [`polibot_native/README.md`](polibot_native/README.md). Changes: [`polibot_native/CHANGELOG.md`](polibot_native/CHANGELOG.md).

## License

[MIT](LICENSE)
