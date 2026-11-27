# PoliBOT Sentry - ESP32-S3 Native C/C++ Firmware (ESP-IDF)

Current version: **0.2.0** (see [CHANGELOG.md](CHANGELOG.md)). Target board: **ESP32-S3-DevKitC-1 with WROOM-1-N16R8** (16 MB flash, 8 MB octal PSRAM).

High-performance, hardware-accelerated C/C++ firmware: **PoliBOT Sentry**, engineered for the **ESP32-S3** microcontroller.

---

## Table of Contents
1. [Architecture & Performance Highlights](#architecture--performance-highlights)
2. [Module Breakdown](#module-breakdown)
3. [Configuring WiFi, AP Name & Credentials](#configuring-wifi-ap-name--credentials)
4. [Prerequisites on macOS](#prerequisites-on-macos)
5. [Building and Flashing to ESP32-S3](#building-and-flashing-to-esp32-s3)
6. [First Boot & Web Control Panel](#first-boot--web-control-panel)
7. [Firmware Updates (OTA)](#firmware-updates-ota)
8. [IRC Sentry Commands Reference](#irc-sentry-commands-reference)

---

## Architecture & Performance Highlights

- **Hardware-Accelerated SHA-256**: Leverages the ESP32-S3 cryptographic hardware acceleration engine (`mbedtls_sha256`) for sub-millisecond password hashing.
- **Zero-Copy IRC Protocol Engine**: Incoming lines are parsed in-place using static buffers—no heap thrashing and **zero garbage collection freezes** during massive clone/botnet raids.
- **Embedded Web Control Panel**: The complete 38 KB SPA dashboard (`web/index.html`) is compiled directly into flash ROM symbols (`EMBED_TXTFILES`), serving instantly with zero filesystem reads.
- **NTP Network Time & Automatic Timezone Sync**: Synchronizes with atomic NTP servers (`pool.ntp.org`) on boot, with customizable city/country presets and POSIX daylight saving rules.
- **Dual-Core FreeRTOS Multithreading**: Sentry & network tasks run on Core 0 while crypto, display, and maintenance run on Core 1.
- **SPIFFS Persistent Storage**: Configuration is saved to its own flash partition (`/spiffs/config.json`), written to a temporary file and swapped in so a power cut cannot leave it half-written. It survives reboots, OTA updates, rollbacks and `idf.py flash`; only `idf.py erase-flash` removes it.

---

## Module Breakdown

Sources live in `main/`: device services at the top level, then `logic/` (rules with no ESP-IDF, tested on a PC),
`memory/` (what the bot remembers), `irc/` (the IRC client, filters and commands), `web_api/` (the panel's server)
and `web/` (the pages). Large or rarely used data is kept in PSRAM.

| Module | Source Files | Description |
| :--- | :--- | :--- |
| **Main Orchestrator** | `main/main.cpp` | FreeRTOS multitasking across both Xtensa cores, NVS initialization, and heartbeat task. |
| **Config Manager** | `main/config_manager.cpp`, `.h` | Thread-safe `cJSON` manager backed by `/spiffs/config.json` with live rehash listeners. |
| **Hardware Security** | `main/security_manager.cpp`, `.h` | Hardware SHA-256, crypto RNG salt generator, role hierarchy, IRC PM auth, and web token sessions. |
| **WiFi Manager** | `main/wifi_manager.cpp`, `.h`, `wifi_policy.h` | Station mode with automatic reconnection and a fallback setup access point. |
| **Guardian Sentry** | `main/irc/guardian.*` | Anti-flood, word blacklist, anti-caps, repeated text, join floods, auto-kick list, escalation and entry rules. |
| **IRC Engine** | `main/irc/irc_*.cpp` | One task per network (TLS or TCP), operator status, protection actions, report channel, dynamic floating limits (`+l`). |
| **IRC Commands** | `main/irc/commands*.cpp`, `command_help.*` | The command set, one file per group. |
| **Logic** | `main/logic/` | Masks, channel modes, member lists, auto-op and voice lists, rate limits and send pacing. |
| **Memory** | `main/memory/` | `seen`, `tell`, placed bans and earned voice, saved in the settings partition. |
| **Bot Settings** | `main/bot_settings.*` | Every behaviour setting described once: loading, ranges and the panel's values. |
| **OTA Manager** | `main/ota_manager.cpp`, `.h` | Dual-slot firmware updates by web upload or URL download, with automatic rollback. |
| **Update Checker** | `main/update_checker.cpp`, `logic/releases.*` | Signed online releases: check, notice and upgrade. |
| **NTP Manager** | `main/ntp_manager.cpp`, `.h` | SNTP synchronization and POSIX timezone handling. |
| **Web Server & UI** | `main/web_api/`, `main/web/` | Built-in `esp_http_server` serving the embedded dashboard (`index.html`, `panel.css`, `panel_*.js`) and its REST APIs. |
| **Display Hook** | `main/display_interface.cpp`, `.h` | Modular C++ interface ready for attaching a color SPI TFT screen (ST7789, ILI9341, GC9A01) via DMA. |

---

## Configuring WiFi, AP Name & Credentials

There are **two ways** to configure your WiFi network credentials, Access Point name, and passwords:

### Method 1: Web Control Panel (Recommended - No Recompiling Needed)
1. When the ESP32-S3 boots for the first time without configured credentials, it automatically launches its own WiFi Access Point:
   - **Default AP SSID**: `PoliBOT-Setup`
   - **Default AP Password**: `polibotpassword`
2. Connect your phone or laptop to the `PoliBOT-Setup` WiFi network.
3. Open your browser and navigate to:
   ```text
   http://192.168.4.1/
   ```
4. Click **CONTROL PANEL** and log in (first sign-in: `admin` / `polibotadmin`). The panel then asks for a password of your own before anything else.
5. Go to the **WiFi Setup** tab, enter your home/office WiFi SSID & Password, and click **Save WiFi Settings**.
6. The bot saves the credentials to `/spiffs/config.json` and connects right away. The new IP address appears on the same tab (and in the router's device list as `polibot`). The setup access point closes two minutes after the connection succeeds.

The ESP32-S3 supports **2.4 GHz** WiFi only.

**Reconnection.** If WiFi drops, the bot retries every 5 seconds. After 30 seconds offline it also opens the setup access point (unless *Access Point Fallback* is disabled) and keeps retrying every 30 seconds, so a router that was merely restarting is picked up again by itself.

### Method 2: Set Defaults in Code (Before Flashing)
To bake your WiFi credentials directly into your own build, edit **`main/config_manager.cpp`** around line 15. **Never commit or share that change or that binary: both hold your WiFi password.**

```cpp
static const char* DEFAULT_CONFIG_STR = R"JSON({
  "device_name": "PoliBOT-Sentry",
  "wifi": {
    "ssid": "YOUR_HOME_WIFI_SSID",         // <--- Put your WiFi SSID here
    "password": "YOUR_WIFI_PASSWORD",       // <--- Put your WiFi Password here
    "ap_fallback": true,                    // <--- Keep true for fallback AP
    "ap_ssid": "PoliBOT-Setup",             // <--- Access Point name if WiFi fails
    "ap_password": "polibotpassword"        // <--- Access Point password (min 8 chars)
  },
  "irc": {
    "cmd_prefix": "!",
    "reconnect_delay": 5,
    "max_reconnect_delay": 60,
    "ping_timeout": 120,
    "networks": [                            // up to 4, each with its own connection
      {
        "name": "SekurNet",                  // letters, digits, . - _
        "host": "irc.SekurNet.org",
        "port": 6697,
        "ssl": true,
        "ssl_verify": true,                  // false only for self-signed IRC servers
        "nick": "PoliBOT",
        "ident": "polibot",
        "realname": "ESP32-S3 Guardian Sentry",
        "nickserv_password": "",
        "channels": ["#polibot"]
      }
    ]
  },
  "ntp": {
    "enabled": true,
    "server": "pool.ntp.org",
    "timezone": "CET-1CEST,M3.5.0,M10.5.0/3",
    "location": "Europe/Madrid"
  },
  ...
})JSON";
```

---

## Configuring NTP Time & Timezones (City / Country)

PoliBOT includes a built-in SNTP client that automatically syncs the ESP32-S3 hardware clock with atomic internet time servers as soon as WiFi connects.

### In the Web Control Panel
Go to the **NTP & Timezone** tab:
1. Select your city or country from the preset dropdown (e.g., `Europe/Madrid`, `America/New_York`, `Asia/Tokyo`, `Europe/London`, etc.).
2. The POSIX timezone string and Daylight Saving Time (DST) transitions are auto-populated.
3. Click **Save & Synchronize Time**. The bot updates its clock immediately without rebooting!

### Common Timezone Presets
| City / Region | Location Label | POSIX Timezone String |
| :--- | :--- | :--- |
| **Madrid, Paris, Berlin, Rome** | `Europe/Madrid` | `CET-1CEST,M3.5.0,M10.5.0/3` |
| **London, Dublin, Lisbon** | `Europe/London` | `GMT0BST,M3.5.0/1,M10.5.0` |
| **New York, Miami, Toronto** | `America/New_York` | `EST5EDT,M3.2.0,M11.1.0` |
| **Chicago, Dallas, Mexico City**| `America/Chicago` | `CST6CDT,M3.2.0,M11.1.0` |
| **Los Angeles, Vancouver** | `America/Los_Angeles` | `PST8PDT,M3.2.0,M11.1.0` |
| **São Paulo, Buenos Aires** | `America/Sao_Paulo` | `BRT3` |
| **Dubai** | `Asia/Dubai` | `GST-4` |
| **Shanghai, Hong Kong, Singapore**| `Asia/Shanghai` | `CST-8` |
| **Tokyo, Seoul** | `Asia/Tokyo` | `JST-9` |
| **Sydney, Melbourne** | `Australia/Sydney` | `AEST-10AEDT,M10.1.0,M4.1.0/3` |
| **UTC** | `UTC` | `UTC0` |

### Checking Time via IRC
You can check the synchronized time from IRC at any time:
- `!time` or `!date` — Displays: `Local Time: 2026-11-25 01:45:12 CET (Europe/Madrid, NTP Synced)`
- `!status` — Includes the live local date/time in the status line.

---

## Prerequisites on macOS

Ensure you have Homebrew installed on your Mac, then install the build tools:

```bash
brew install cmake ninja dfu-util ccache
```

### Install the ESP-IDF Toolchain (One-Time Setup)
Create an `esp` folder in your home directory and clone ESP-IDF v5.2+:

```bash
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.2.2
cd esp-idf
./install.sh esp32s3
```

> **Tip:** Add this alias to your `~/.zshrc` so you can activate ESP-IDF anytime by typing `get_idf`:
> ```bash
> echo "alias get_idf='. ~/esp/esp-idf/export.sh'" >> ~/.zshrc
> source ~/.zshrc
> ```

---

## Building and Flashing to ESP32-S3

### 1. Load the ESP-IDF Environment
In your terminal, activate the toolchain:
```bash
. ~/esp/esp-idf/export.sh
```
*(or run `get_idf` if you added the alias).*

### 2. Navigate to the Native Project Directory
```bash
git clone https://github.com/gcanosa/esp32-polibot.git
cd esp32-polibot/polibot_native
```

### 3. Set the Target Chip (First Time Only)
```bash
idf.py set-target esp32s3
```

### 4. Build the Firmware
```bash
idf.py build
```
*This compiles the C/C++ code, links the hardware SHA-256 drivers, and embeds the web frontend into the binary.*

### 5. Flash and Launch Serial Monitor
Plug your ESP32-S3 board into your Mac using a data USB cable. Run:
```bash
ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial*    # find the port name
idf.py -p <PORT> flash monitor
```
Either USB-C socket of the DevKitC-1 can be used.

**First flash of a board that ran something else**: run `idf.py -p <PORT> erase-flash` first. This removes the old firmware and its files; settings are not carried over. Never use `erase-flash` for updates, it also removes `config.json`.

> **Flashing Tip for ESP32-S3:**  
> If the flasher reports connection timeout:
> 1. Press and hold the **BOOT** button on the ESP32-S3 board.
> 2. Press and release the **RESET (RST/EN)** button.
> 3. Release the **BOOT** button.  
> 4. Run `idf.py flash monitor` again.  
> To exit the serial monitor at any time, press **`Ctrl + ]`**.

---

## First Boot & Web Control Panel

Once booted, the serial monitor will display:
```text
I (1234) WIFI: Connected to WiFi! IP: 192.168.1.150
I (1250) WEB: Web Control Panel started on port 80
I (1310) IRC: Joined network successfully. Autojoining channels...
```

1. Open your browser to `http://<IP_ADDRESS>/` (e.g. `http://192.168.1.150/`). Both pages have a light and a dark look: they follow your device until you press the sun/moon button, and the choice is remembered in that browser. The **home page** says what it runs on and shows the board's specifications and live figures (CPU load per core, RAM, PSRAM, settings storage, temperature, Wi-Fi signal, uptime, last restart reason). It needs no login and shows no server names, channels or accounts. **CONTROL PANEL** (`http://<IP_ADDRESS>/panel`) opens the administration panel.
2. Log in with your admin credentials.
3. From the dashboard you can:
   - Monitor real-time IRC connection status, active channels, and memory.
   - Read the event log: newest first, with time, source and a colour per kind (problems, moderation, security, channels, connections, console), a filter, and **Clear** (admin).
   - Add, edit or delete IRC networks (**IRC Settings** tab, up to 4 at once). A new nickname applies without disconnecting; other server changes reconnect that network automatically.
   - Add or remove channels per network and configure dynamic floating limits (`+l`), entry notices, badword blacklists, automatic operator and voice, and timed voice (**Channel Sentry** tab).
   - For TLS, set **Use SSL / TLS** and the port. Set **Verify TLS Certificate** to *No* only for self-signed (LAN) servers.
   - Change your password, and manage user roles, hostmasks and accounts.
   - Execute emergency channel lockdowns (`+mi`) and mass kicks.
   - Run live IRC commands from the integrated web console.

---

## Choosing the WiFi Network

In **WiFi Setup**, **Scan Now** lists the 2.4 GHz networks in range, strongest first. Pick one and enter its password, or type the name (needed for a hidden network). The device looks only when the button is pressed. It works over the setup access point too. A look takes a couple of seconds, during which the connection may stutter. API (administrator): `POST /api/wifi/scan` looks, `GET /api/wifi/scan` gives the result.

---

## Status LED

The on-board RGB LED (WS2812 on GPIO48) blinks for half a second every two seconds, in the colour of the connection state:

| Colour | Meaning |
| :--- | :--- |
| **Red** | No WiFi connection (also while only the setup access point is open) |
| **Orange** | WiFi connected, no IRC network connected |
| **Yellow** | Some IRC networks connected, not all |
| **Green** | All IRC networks connected |
| Quick **white** flashes in between | A signed-in user has the web panel open |
| Quick **violet** flashes in between | Somebody has the status page on screen (without the panel being in use) |

An IRC network counts as connected once the server has welcomed the bot. The LED does not test internet access separately: with WiFi up but no internet, it shows orange (or yellow/green for servers on your own network). The quick flashes stop about 10 seconds after the page is closed.

The control panel's **Status LED** tab switches the LED off altogether (saving the few milliamps it draws; the board's red power LED is wired directly and stays on), and sets the blink interval (1 to 10 seconds), the colours and the brightness (12% by default). Changes apply within a second, without a restart or signing anyone out. The same pattern is drawn next to the title of the home page and the control panel; it follows within about three seconds. API: `GET /api/led` (login) and `POST /api/led` (administrator) with any of `enabled`, `period`, `brightness`, `no_wifi`, `no_irc`, `some_irc`, `all_irc`, `panel`, `visitor` (colours as `#rrggbb`).

DevKitC-1 **v1.1** boards have the LED on GPIO38: change `LED_GPIO` in `main/led_driver.cpp`.

---

## Signing In to Services (SASL / NickServ)

Give a network a **Services Password** (and a **Services Account** if it differs from the nickname). The bot then signs in with **SASL PLAIN** while connecting, before joining any channel. If the server offers no SASL, refuses it, rejects the credentials or does not answer within 15 seconds, the bot identifies with **NickServ** after the welcome and waits 2 seconds before joining. The log says which of the two was used. Registering a new account with the network's services is still done once by hand. Use TLS: without it the password crosses the network readable.

---

## Commands After Connecting

Each network has a **Commands After Connecting** box (**IRC Settings**), one command per line. They are sent in order after the server's welcome (and after signing in to services), before the channels are joined, every time that network connects. A change applies at the next connection (**Reconnect** forces one).

| Written | Sent |
| :--- | :--- |
| `/mode -x` or `/umode -x` | `MODE <bot nick> -x` (the bot's own modes, here removing the host cloak) |
| `/msg NickServ GROUP {nick}` | `PRIVMSG NickServ :GROUP <bot nick>` |
| `/notice #chan hello` | `NOTICE #chan :hello` |
| anything else, e.g. `OPER name password` | as written, without a leading `/` |

At most 20 lines, 400 characters each and 2000 together. The lines are not written to the event log, but administrators can read them in the panel: keep the services password in its own field.

---

## When the Nickname is Taken

While connecting, the bot tries the network's **Alternative Nickname** at once; if that is taken too (or none is set) it appends a number, up to 8 attempts. Once connected under another name it takes the configured nickname back as soon as its holder quits or renames in a channel the bot shares.

---

## Channel Protection

- **Operator status.** The bot tracks it. When it is missing, it sends the network's *Ask for Operator Status* command (by default `PRIVMSG ChanServ :OP {channel}`) and repeats it every five minutes. Channel operators are spared by the filters.
- **Rejoining.** After a kick, or when a channel is invite-only, full, keyed or has banned it, the bot tries again with growing delays and asks the services for an invitation or unban.
- **Filters** (flood, forbidden words, capitals) check every channel message and notice from people without a staff role, commands included. Each can warn, kick, ban, mute, escalate (warn, then mute, then ban, by default) or only report.
- **Raids.** Many joins at once, or the same text from several users, are reported. To lock channels down automatically, change it under **Guardian Rules > Raid Protection**. Users returning from a netsplit are not counted.
- **Bans** aim at the host (`*!*@host`) unless the host is shared with the bot. `ban troll 2h` and `quiet troll 30m` end by themselves; `unban troll` lifts what the bot set against that nickname.
- **Auto-kick list.** `akick #chan add *@bad.example reason`, or the panel. Matching users are banned and kicked on sight; staff are spared.
- **Floating limit (`+l`).** Looked at every 30 seconds, not on every join, so a flood runs into the limit and the channel grows only a margin at a time. The limit is the users present plus the channel's free places (never below its lowest limit, both under **Channel Sentry**), raised with a little headroom, and lowered in steps only after it has been too high for five minutes. After joining a channel the bot removes `+l` and sets none for three minutes, so that everyone can come back after a restart or a netsplit; users lost in a netsplit stay counted for up to 30 minutes. The timings are under **Bot Behaviour > Floating Channel Limit**.
- **Muting and banning** take voice away first, since some servers let voiced users talk through them.
- **Pacing.** Lines are sent a few at once and then one per second, moderation first. Commands from people without a role are limited, and too many failed passwords pause a host or web address.

## Automatic Operator, Half-Op and Voice

Everything here is set **per channel of a network**, on the channel's card under **Channel Sentry**.

**Lists.** Each channel has its own auto-op, auto half-op and auto-voice list (200 entries in all). Add and remove them on the card or with `autoop`, `autohop` and `autovoice`. An entry is:

| Entry | Means |
| :--- | :--- |
| `*!*@host.example` | a hostmask; it must name a host |
| `alice` | whoever is signed in to services (NickServ) as `alice` |
| `user:alice` | whoever the bot identifies as its account `alice` (login, trusted hostmask or services account) |

A nickname alone never counts. Typing the nickname of somebody in a channel stores their services account, or else their hostmask, and the reply says which; `account:alice` stores the account name even then. Services accounts need a server that reports them (**IRC Settings** shows whether it does).

**Admins** are opped on joining unless switched off, generally (**Guardian Rules > Operators**) or per channel.

**When.** On joining, when somebody identifies later (login, or services reporting their account), when the bot gets operator status, and after a change to the lists. A mode somebody takes away is not given back during that visit. A half-operator bot gives voice only. Nobody covered by a ban or mute the bot knows of gets anything.

**Timed voice** (off by default) voices whoever has been in the channel for the time you set (10 minutes by default; minutes, hours or days), so that regulars can still talk when the channel is made moderated.

- While the channel is `+m` or locked down, nobody new earns voice and the waiting time stands still.
- Earned voice is remembered, across rejoins and restarts: on the next visit it is given at once, also while moderated. Only a hash per person is stored (**Bot Behaviour > Timed Voice**: 500 people, forgotten after 90 days away, by default).
- Anyone devoiced, kicked, banned or muted loses it and does not earn it again, until somebody voices them by hand or the memory expires.
- A patient bot that idles long enough earns voice too: a longer time filters more.

---

## Report Channel

Per network, under **IRC Settings**, set a channel (for example `#esp32-debug`) and a password. The bot joins it, sets the password as the channel key (`+sk`) and, once the server confirms it, reports what it does: moderation, raids, sign-ins, operator and join problems, settings changes, restarts and low memory. It needs operator status there, which it has when it creates the channel. Staff join with `/join #esp32-debug password`. Nothing is sent to a channel without the password. Filters and `seen` ignore this channel, and it is not listed publicly.

## Bot Mode

**IRC Settings > Bot Mode** sets the user mode the server announces for bots (`BOT=B`), or `+B` where the server only lists it among its user modes. The panel shows whether it took effect.

## Guarded Channels with a Password

Give a `+k` channel its password under **Channel Sentry** (or when adding it), or send `join #channel password` to the bot in private. It is stored with the network, masked in the panel, and kept out of logs and reports.

## Signing In and Sessions

The default password only serves to replace it: until then the web session can do nothing else, and `login` on IRC refuses it. A panel session ends after 30 idle minutes (**Bot Behaviour > Rate Limits and Sign-In**); the panel's own refreshing does not count as use. Too many wrong passwords pause that web address or IRC host for a while.

## Seen and Tell

`seen <nick>` says when somebody was last around and what they did; other channels' details are only shown to their members and staff. `tell <nick> <message>` is delivered the next time that nickname speaks or joins. Both can be limited or switched off under **Bot Behaviour**.

## When Settings Are Saved

A change made in the panel is in flash before the panel reports success. A change made on IRC (and an automatic lockdown) applies at once and is written in the background, so the bot never stands still for the flash; the write follows within seconds, and a restart from IRC, the panel or an update writes it first. Cutting the power in those seconds loses that last change. If a write fails, the report channel is told and it is tried again.

## Backup and Restore

**Firmware & Backup** downloads every setting and account as one file (owner only; it contains passwords) and restores it, keeping the current WiFi settings unless told otherwise. `seen`, waiting messages and timed bans are not included.

## Multiple Networks

Each entry in `irc.networks` runs in its own task with its own nickname, channels, logins and channel rules. A `login` on one network grants nothing on another. Channel rules are stored as `network/#channel` (a plain `#channel` rule applies to any network without its own), and per-channel user roles accept the same two forms. Settings from earlier single-network versions appear automatically as a network called `default`.

---

## Versioning

The version number lives in `version.txt`. For each release: change it, add an entry to `CHANGELOG.md`, and build. It is shown in the web panel header and the **Firmware & Backup** tab, in `!status` / `!version`, and in the bot's CTCP VERSION reply:

```text
/ctcp PoliBOT VERSION
PoliBOT v0.2.0 - Running on ESP32-S3 (2 cores @ 240 MHz, 16 MB flash) - ESP-IDF v5.2.2
```

---

## PC Tests

The parts without ESP-IDF are tested on the computer, from this folder:

```bash
c++ -std=c++17 -I main/logic test/logic_test.cpp main/logic/*.cpp -o /tmp/logic_test && /tmp/logic_test
c++ -std=c++17 -I main/memory -I main/logic test/memory_test.cpp main/memory/*.cpp -o /tmp/memory_test && /tmp/memory_test
c++ -std=c++17 -I main/irc test/command_help_test.cpp main/irc/command_help.cpp -o /tmp/help_test && /tmp/help_test
for t in wifi_policy status_led; do c++ -std=c++17 -I main test/${t}_test.cpp -o /tmp/$t && /tmp/$t; done
```

---

## Firmware Updates (OTA)

Flash over USB **once** (`idf.py -p <port> flash`) so the bootloader and dual-slot partition table are installed. Later updates can go over the network. `config.json` lives in its own partition and survives updates.

All update methods require an **owner** account. The file to send is the application image, `build/polibot_native.bin` (not the bootloader or a merged image).

| Method | How |
| :--- | :--- |
| Online | **Firmware & Backup > Online Updates** (see below). |
| Web upload | **Firmware & Backup** tab, choose the `.bin`, click **Upload & Flash**. |
| Web URL | **Firmware & Backup** tab, paste a direct `.bin` link, click **Download & Flash**. |
| IRC | `/msg PoliBOT login <password>` then `/msg PoliBOT ota <url>`; `ota status` shows progress. |
| Command line | See below. |

```bash
TOKEN=$(curl -s http://DEVICE_IP/api/login -d '{"username":"admin","password":"YOUR_PASSWORD"}' \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
curl -H "Authorization: Bearer $TOKEN" --data-binary @build/polibot_native.bin http://DEVICE_IP/api/ota/upload
```

REST endpoints: `GET /api/ota/status`, `POST /api/ota/upload` (raw binary body), `POST /api/ota/url` (`{"url": "..."}`).

### Online Updates

**Check Now** reads the release list in this repository (`firmware/latest.json`). **Check automatically** (off by default) does so two minutes after a start and then every so many hours (24 by default, 1 to 720); a check that fails is tried again after 15 minutes. A newer release is announced by a notice at the top of the panel and once in the event log, and **Upgrade** installs it. API (owner): `GET /api/update`; `POST /api/update` with `{"action":"check"}`, `{"action":"install"}` or `{"auto_check":true,"check_hours":24}`.

The device offers a release only if the list carries a signature made with the key in `main/update_key.pem`, names this board and is newer than the running version, and it installs the download only if its size and SHA-256 are the signed ones. Uploading a file or giving an address yourself needs no signature, so your own builds install as before. A fork that publishes its own releases changes the address in `main/update_checker.cpp` and the key. Certificate expiry dates are not checked; the signature is what protects the update. A check sends nothing about the device beyond an ordinary web request.

**Rollback protection.** A newly installed image must run for 30 seconds with the network up and the web panel started before it is kept. If it crashes, is reset, or loses power before that, the bootloader returns to the previous firmware. Further updates are refused during that window.

**Passwords.** The web API never returns stored passwords: they read as `********`, and saving that value keeps the stored one. Status, configuration and logs need a login.

**Security.** HTTPS downloads are verified against the ESP-IDF root certificate bundle (self-signed certificates are rejected). Plain `http://` URLs are accepted for local servers but are not authenticated, and the web panel itself is plain HTTP, so only update over a network you trust.

The tab shows the installed version and build time.

---

## IRC Sentry Commands Reference

You can send commands directly in guarded channels using the configured prefix (`!`) or in **Private Message** to the bot without any prefix:

### Authentication & Identification (PM Only)
- `/msg PoliBOT login <password>` — Authenticate your current nick & hostmask.
- `/msg PoliBOT login <username> <password>` — Authenticate with explicit username.
- `/msg PoliBOT logout` — Terminate your authenticated session.
- `/msg PoliBOT whoami` — Display your active identity and global/channel permissions.

**Who is trusted.** Permissions (including auto-op for admins, and exemption from flood/word filters) require one of three things: a password `login`; a hostmask that names a host, such as `*!alice@home.example.org` or `*!*@home.example.org` (a mask without one, like `*!*@*`, `alice!*@*` or `*!alice@*`, only allows logging in, since anyone can choose a nickname or ident); or a **services account** listed on the bot user as `network/account` (**Admin & Users > Manage Users**). A login lasts while your nick and hostmask stay the same.

**Services accounts.** Someone signed in to NickServ (or SASL) with a listed account on that network is that user, without a bot password. This needs a server that offers `account-tag` or `account-notify`; **IRC Settings** shows whether it does. With only `account-notify`, accounts of people already present need WHOX. Account changes made in the panel apply at once. Owner actions that need a password (`admin` changes, `ota`) still need `login`.

### General Commands
- `!help` — List the commands available to your role, using the configured prefix. Asked in a channel, the general commands are listed there (or sent to you as a notice: **Bot Behaviour > Help**).
- `!help <command>` — Explain one command, with an example (e.g. `!help ban`).
- `!status` or `!uptime` — Display bot version, uptime, free RAM, channels, and network.
- `!version` — Software version and hardware (also answered to CTCP VERSION).
- `!sysinfo` — Processor, load, memory, storage, temperature, Wi-Fi signal and uptime in one line.
- `!chanstats [#chan]` — Display user count, active `+l` dynamic limit, lockdown and operator state.
- `!seen <nick>` — When somebody was last around.
- `!tell <nick> <message>` — Leave a message for them.

### Moderator Commands
- `!op` / `!deop`, `!voice` / `!devoice`, `!half` / `!dehalf` (also `!halfop` / `!dehalfop`, `!hop` / `!dehop`) — Grant or revoke operator (`+o`), voice (`+v`) or half-operator (`+h`) status: `[#chan] [nick...]`. Several nicknames can be named at once; with none, it applies to you.
- `!kick [#chan] <nick> [reason]` — Kick user from channel.
- `!quiet [#chan] <nick|mask> [time]` / `!unquiet [#chan] <nick|mask>` — Mute or unmute, optionally for a time such as `30m`. The mode is set per network (`+q {mask}` by default, as on Solanum servers; `+b ~m:{mask}` on SekurIRCd).
- `!topic [#chan] <new topic>` — Update channel topic.

### Admin Commands
- `!dynlimit [#chan] <on [slots]|off|slots>` — Enable/disable the floating channel limit (`+l users + slots`, adjusted gradually: see [Channel Protection](#channel-protection)).
- `!lockdown <#chan>` — Activate emergency lockdown (`MODE +mi`). Only voiced/ops can join and speak.
- `!unlock <#chan>` — Release lockdown (`MODE -mi`).
- `!entrymsg <#chan> [<message>|off]` — Show, set or disable the rules/welcome `NOTICE` on join.
- `!badwords [#chan] [list|add <word>|del <word>]` — Show or edit the channel's forbidden words.
- `!ban [#chan] <nick|mask> [time]` / `!unban [#chan] <nick|mask>` — Ban (a nick is banned by host), optionally for a time such as `2h` or `7d`, or unban.
- `!akick [#chan|*] list | add <nick|mask> [reason] | del <nick|mask>` — The saved auto-kick list.
- `!autoop` / `!autohop` / `!autovoice` `[#chan] [list|add <who>|del <who>]` — The channel's [automatic lists](#automatic-operator-half-op-and-voice).
- `!timevoice [#chan] [on|off|<time>]` — Timed voice, e.g. `!timevoice #chan 2h`.
- `!join <#chan> [password]` / `!part <#chan>` — Join or leave a channel and save the change to this network's autojoin list. Send a password in private only.
- `!say <#chan|nick> <message>` — Send a message as the bot.
- `!rehash` — Reload configuration and security rules from flash memory without restarting.

### Owner Commands
- `!owner [#chan] [nick...]` / `!deowner [#chan] [nick...]` — Grant or revoke channel owner status (`+q`), on servers that have it. The bot must be an owner of the channel itself.
- `!masskick [#chan] <pattern> [reason]` — Kick every nick matching the pattern (e.g. `!masskick #chan botnet*`). Channel operators and staff accounts are skipped; at most 50 per command.
- `!admin list` — List accounts.
- `!raw <irc command>` — Send raw line directly to IRC server socket.
- `!reboot` — Safely restart the ESP32-S3 microcontroller.

These need a password `login` first and work only in Private Message (or from the web console):
- `admin add <username> <password> <role> [#chan:role,...] [hostmask] [account=name]` — Add or update an account. `account=name` is a services account on this network (`account=network/name` for another).
- `admin passwd <username> <password>` — Change an account's password.
- `admin del <username>` — Delete an account (owner accounts cannot be deleted).
- `ota <url>` / `ota status` — Firmware update (Private Message only).
