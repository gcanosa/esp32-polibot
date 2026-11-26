# Changelog

## 0.1.5 - 2026-11-26

First public release, on GitHub as `gcanosa/esp32-polibot` under the MIT license.

### Added
- `hop` and `dehop` as short forms of `half` and `dehalf`.
- Auto-op, auto half-op and auto-voice lists for each channel of each network: hostmasks, services accounts and the bot's own accounts, from the channel cards or with `autoop`, `autohop` and `autovoice`.
- Timed voice per channel (`timevoice`): voice after a set time there, paused while the channel is moderated or locked down, remembered across rejoins and restarts, and lost on a devoice, kick, ban or mute.
- Automatic operator for admins can be switched per channel.
- **Commands After Connecting** per network, such as `/mode -x`.
- The home page says who wrote the software and where to ask: irc.SekurNet.org, #SekurNet.

### Changed
- A new device starts with the network SekurNet (irc.SekurNet.org) over TLS on port 6697 with certificate verification, and the examples use it. Saved settings are not touched.
- Operator status for admins and list entries is also given after joining: on `login`, when services report an account, and when the bot is opped.
- A mute or a ban removes voice first.
- The unused `web.auth_token` is gone from the default settings.

### Fixed
- **Security:** an account hostmask naming only a nickname or ident, such as `*!alice@*`, was trusted without a password. Only a mask that names a host is now; others need `login`.
- Removing all of a user's channel roles in the panel reported success and kept them.
- The bot stood still while a command on IRC, or an automatic lockdown, wrote the settings to flash. Such changes now apply at once and are written in the background.

## 0.1.4 - 2026-11-26

### Added
- A new look for both pages in white, grey, black and bordeaux, light and dark, with a switch.
- The home page says the whole application runs on the microcontroller, with a picture of the board.
- **Scan Now** in **WiFi Setup** lists the networks in range, to pick one; the name can still be typed.
- Icons and one style for every button on both pages.
- The status LED can be switched off, and its blink interval set (**Status LED** tab).
- Quick violet flashes while somebody has the status page on screen.
- `half` / `dehalf` and `owner` / `deowner`, on servers that offer those ranks.

### Changed
- Both pages fit phone screens better: equal buttons in the navigation and device rows, tabs in an even grid, status cards two to a row.
- The status LED blinks slowly in the colour of the connection state instead of staying lit; quick white flashes in between show a signed-in web panel.
- The event log shows time, source and kind in colour, newest first, with a filter, and no longer jumps while older lines are being read.
- `op`, `voice` and their relatives take several nicknames at once, and apply to whoever asks when none is named.
- `help` asked in a channel now answers in the channel, listing the general commands; **Bot Behaviour > Help** restores the answer by notice.

### Fixed
- **Clear / Refresh** did not clear the event log. **Clear** now empties it (admin).
- The panel showed an enabled entry message with a sample text for channels that had none saved, although nothing was sent.

## 0.1.3 - 2026-11-25

### Added
- The on-board RGB LED shows the connection status, and blinks white while someone uses the web panel.
- `help <command>` explains a command with an example.
- `sysinfo`: processor, load, memory, storage, temperature, Wi-Fi signal and uptime in one line.
- Alternative nickname per network, then numbered ones, when the nickname is taken; the bot takes its own back when it is freed.
- SASL sign-in to services, falling back to NickServ; optional account name per network.
- A home page with the board's specifications and live load. The control panel moved to `/panel`.
- **Status LED** tab: a colour for each state and a brightness slider.
- An on-screen copy of the status LED next to the title on both pages.
- PoliBOT logo on both pages and as the browser icon.
- A navigation bar, a separate row for device actions, and the tabs on one line.
- Operator status is tracked. The bot asks the services for it, spares channel operators from the filters, and says so when it cannot act.
- Joining again after a kick, a ban, an invite-only or full channel, with growing delays; invitations are accepted.
- Outgoing messages are paced, commands from people without a role are limited, and failed sign-ins are paused (IRC and web).
- Raid protection: join floods and the same text from several users, reported or answered with an automatic lockdown. Netsplits are not counted.
- Bans by host, timed bans and mutes (`ban nick 2h`), a saved auto-kick list (`akick`), and escalation from warning to mute to ban.
- A password protected report channel per network.
- `seen` and `tell`.
- Bot mode (user mode `+B`) per network, where the server offers it.
- Every behaviour setting in the web panel (**Guardian Rules**, **Bot Behaviour**, **IRC Settings**).
- Settings backup and restore for owners.
- Staff can be recognised by their services account (per user, `network/account`), as well as by hostmask or password.
- Passwords for guarded `+k` channels, in the panel or with `join #channel password` in private.
- The restart reason and low internal memory are reported to the report channel.

### Changed
- The floating channel limit (`+l`) is adjusted every 30 seconds instead of on every join and part, comes down gradually, is removed and held back for three minutes after the bot joins a channel, and keeps room for users lost in a netsplit. The timings are under **Bot Behaviour**.
- The default password must be changed at the first sign-in, and no longer works over IRC.
- `/api/status` needs a sign-in (the home page uses `/api/system`).
- Idle web sessions expire (30 minutes by default, adjustable); at most eight are kept.
- Wi-Fi and TCP/IP buffers use external RAM first.
- The bot asks every server for its capabilities.
- Raid protection only reports until it is told to lock channels down (**Guardian Rules > Raid Protection**).
- The source is split into modules (`logic`, `memory`, `irc`, `web_api`), and the panel into a stylesheet and scripts.
- The bot's lists and the configuration live in external RAM; rate limit tables have a fixed size.
- `help` lists your commands with the configured prefix. Asked in a channel, it answers you privately by notice.
- Changing the nickname no longer reconnects the network.

### Fixed
- Channel messages starting with the command prefix skipped the flood, word and capitals filters.
- Commands claimed success when the bot was not an operator.
- Filters could kick channel operators who had no bot account.
- The reason was not reported when joining again after a kick was refused.
- Switching the floating limit off in the panel left the last `+l` on the channel.
- Changing nickname restarted the flood count.
- The NickServ password was sent before registration, where servers reject it.
- Pressing Enter in the sign-in dialog now signs in.
- The bot lost track of itself when the server gave it a different nickname.

## 0.1.2 - 2026-11-25

### Added
- IRC commands `ban`, `unban`, `quiet`, `unquiet`, `entrymsg`, `badwords`, `join`, `part`, `say`, `masskick` and `admin`.
- **Change My Password** in the web panel, for every signed-in account.
- WiFi reconnects by itself, opens the setup access point after 30 seconds offline, and applies saved WiFi settings without a reboot.
- WiFi status and address in the web panel.

### Changed
- Shorter CTCP VERSION and `!version` reply.
- Passwords are no longer sent to the browser: they show as `********`.
- Reading the configuration and the logs requires a login.
- A new password signs that account out everywhere else.
- Network changes apply at once, also while a connection attempt is still in progress.

### Fixed
- `/api/config` showed the WiFi and NickServ passwords to anyone.
- `!dynlimit` with something other than a number crashed the device.
- A short or invalid access point password caused a boot loop.
- The only owner account could be demoted, locking everyone out.
- User names and roles were not validated, and could inject code into the web panel.
- Saving a network in the web panel could undo channels changed from IRC.

## 0.1.1 - 2026-11-25

First numbered release of the native ESP32-S3 port.

### Added
- Firmware updates (OTA) by web upload, URL or the IRC `ota` command, with automatic rollback.
- Up to 4 IRC networks at once, managed from the web panel; each with its own channels, logins and channel rules.
- Add and remove channels from the web panel.
- NTP & Timezone and Firmware Update tabs.
- Option to accept self-signed certificates for IRC over TLS.
- Version number in the web panel, `!status`, `!version` and the CTCP VERSION reply.

### Changed
- Saving server settings reconnects the affected network automatically.
- A catch-all hostmask (`*!*@*`) or a nick-only mask no longer identifies a user: a login is required.
- Configuration partition reduced to 512 KB, which makes saving much faster (applies on a USB flash).

### Fixed
- The project did not compile, and crashed at boot.
- Saved IRC settings were ignored at boot.
- Settings could not be saved from the web panel.
- Saving channel settings erased all guardian rules and crashed the device.
- Every user was treated as owner: all joiners were opped and nobody was moderated.
- IRC over TLS could not connect.
- The web panel could freeze when settings were re-applied.
- Anonymous web visitors could run owner commands; administrators could take over owner accounts.
- A power cut during a save could lose the configuration.
