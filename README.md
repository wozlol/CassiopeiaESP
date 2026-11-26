# CassiopeiaESP

A Casio A-11 shell turned into a pocket computer. An ESP32-S3 board drives a 4 inch touch screen. The A-11's own keyboard is read by an nRF52832 chip. The handheld is an SSH terminal to your own computers, an offline reference library with pictures, and a media viewer. It runs on a 3000 mAh battery. Every mode lives in its own flash slot, and a small loader on top lets you switch between them or install new ones from the SD card.

This is a personal hobby project for my own devices and my own computers. It is shared as it is. No license has been chosen yet, so all rights are reserved until one is added.

## Contents

1. [What is in the box](#what-is-in-the-box)
2. [Status, what has been seen working](#status-what-has-been-seen-working)
3. [Parts and wiring](#parts-and-wiring)
4. [Build and flash](#build-and-flash)
5. [The loader and the four slots](#the-loader-and-the-four-slots)
6. [Mode 1, the terminal](#mode-1-the-terminal)
7. [The helper on your computers](#the-helper-on-your-computers)
8. [The SD card and the library](#the-sd-card-and-the-library)
9. [The reader](#the-reader)
10. [Update the card over Wi-Fi, sync mode](#update-the-card-over-wi-fi-sync-mode)
11. [Remote access with Tailscale, step by step](#remote-access-with-tailscale-step-by-step)
12. [Making your own packs](#making-your-own-packs)
13. [Problems and fixes](#problems-and-fixes)
14. [Safety notes](#safety-notes)
15. [Repository layout](#repository-layout)

## What is in the box

| Part | What it does |
|---|---|
| Loader | Starts at power on unless a mode is picked. Chooses a mode, installs mode files from the SD card, changes the theme, sleeps the device. |
| Mode 1 | SSH terminal to your computers through a small program on each, the helper. Sessions live in tmux on the computer, so they survive the handheld going away. Also Wi-Fi, clipboard, themes, battery, files, Tailscale. |
| Reader | Offline library with search, articles, pictures you can zoom, find, copy and paste. Packs are made from Kiwix files, PDF manuals and plain text. |
| Media | Picture viewer, WAV and MP3 player. |
| Helper | A single Go program for Mac, Linux, and Windows through WSL. It serves sessions, history, and a drop folder of files for the handheld to pull. |
| Tools | Python and shell tools that build packs, sync the SD card and talk to the keyboard chip. |

## Status, what has been seen working

| Seen working on the device | Built, not yet seen working |
|---|---|
| Terminal sessions to a Mac and a Windows PC through the helper, at home | Tailscale link from the handheld to your computers while away |
| Pairing a computer by matching a code on both screens | Zoom and pan in the picture viewer, manual packs with page pictures |
| Library list, search, scroll bar | Tap and hold on links, find as you type, copy and paste in the reader |
| Theme editor | The star effect for GUI plus Esc, brightness key and Off in the loader |
| Installing modes from the SD card in the loader | Get files from the computer, Wi-Fi sync mode, the loader suggesting the slot with the same name |

Anything in the right column is a test build until it is checked on hardware. See `Firmware/README.txt` for the exact build.

## Parts and wiring

Board: Freenove FNK0104S, ESP32-S3, 16 MB flash, 8 MB PSRAM, 480 by 320 ST7796 display, FT6336U touch, SD slot, ES8311 audio chip with speaker and microphone, WS2812 LED on the back. Keyboard: the Casio A-11's matrix, read by an nRF52832 that talks to the ESP over I2C. Battery: 3000 mAh. Pin tables and the keyboard map are in `WIRING.md` and `keymap/A11_KEYBOARD.md`. The plan, decisions and safety ledger are in `PLAN.md`.

## Build and flash

You need ESP-IDF 6.0.1 and Python 3. Each mode is its own project under `modes/`. They share the components under `components/`.

```
. ~/esp/esp-idf/export.sh
cd modes/mode1
idf.py build
```

The first time, flash everything at once. The files are in `Firmware/`.

```
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x20000 loader.bin 0x120000 mode1.bin 0x3a0000 reader.bin 0x620000 media.bin
```

Later, flash one mode into its slot.

```
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 write-flash 0x3a0000 reader.bin
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 erase-region 0xf000 0x2000
```

The second line clears the record of which slot to boot, so the loader comes up next. Without it the device starts the last mode that ran.

| Slot | Name | Offset | Size |
|---|---|---|---|
| Loader | factory | 0x20000 | 1 MB |
| 1 | ota_0 | 0x120000 | 2.5 MB |
| 2 | ota_1 | 0x3a0000 | 2.5 MB |
| 3 | ota_2 | 0x620000 | 2.5 MB |
| 4 | ota_3 | 0x8a0000 | 2.5 MB |
| Settings and clipboard | storage | 0xb20000 | 4.9 MB |

Wi-Fi, saved computers, your encrypted key, themes and the clipboard live in the settings area and in NVS. Flashing a mode does not erase them.

## The loader and the four slots

Press 1 to 4 to start the mode in that slot. Up and Down move, Enter starts. Press T for the theme list. The brightness key steps the screen down through six levels and wraps. Off sleeps the whole device and On wakes it into the loader.

Install a mode from the SD card: put the file in `modes` on the card, pick it in the install list, press Enter, then press the slot number. File names keep the same names as the project: `mode1.bin`, `reader.bin`, `media.bin`. A file named like a mode goes back into the slot that holds that mode. When you pick `reader.bin` and the reader is in slot 2, the bottom line says it matches slot 2 and Enter puts it there. You can still press 1 to 4 for another slot.

From any mode, hold GUI and Esc to go back to the loader. A short star field plays in your theme colors while you hold. Let go early to stay. Holding GUI while pressing On or while resetting also goes to the loader. If a mode crashes three times in a row, the loader starts by itself.

## Mode 1, the terminal

Tap GUI to open the menu. Up and Down move, Left and Right change a value, Enter picks, Esc closes.

| Menu item | What it does |
|---|---|
| Sessions | Lists the tmux sessions on the computer in use, opens one, starts a new one, ends one. |
| Clipboard | 40 saved clips. Enter pastes. E edits, N adds, C grabs a line from the screen. |
| Wi-Fi | Join a network. |
| Computer | Up to four computers. Enter switches, A adds or pairs again, E sets the away address, F forgets. |
| Theme, Theme editor | Nine built-in themes and eight of your own. The editor sets text, background and cursor color, saves, loads, copies, deletes. |
| Tailscale | Switch it on, join with a one-time key, choose whether the away address is used. See the Tailscale section. |
| Font, Brightness, Backgrounds, Caps key | Display and key settings. |
| Get files from the computer | Pulls files the computer put in its drop folder onto the SD card. |
| Battery and power, Security, Status | Charge estimate and auto sleep, PIN and key, system numbers including the last route used. |
| Back to the loader | Restarts into the loader. |

The back LED glows green, yellow or red for the battery level, pulses while charging, and fades to blue when a terminal reply has finished.

## The helper on your computers

The helper is the other end of every connection. It listens on port 2222, keeps sessions in tmux, and only accepts devices you have paired.

Download the program for your computer from `https://woz.lol/cassiopeia/` or build it from `helper/`.

| Computer | Run |
|---|---|
| Mac | `./a11helper-darwin-arm64` for Apple silicon, `./a11helper-darwin-amd64` for Intel. Needs tmux, for example `brew install tmux`. |
| Linux | `./a11helper-linux-amd64`. Needs tmux. |
| Windows | Inside Ubuntu on WSL, see below. The helper needs tmux, which Windows does not have. |

Options: `-start FOLDER` is where new sessions begin and defaults to your home folder. `-drop FOLDER` is the drop folder and defaults to `~/a11-drop`. `-listen ADDRESS` defaults to `:2222`. `-config FOLDER` holds the paired devices.

Type these in the helper window: `y` or `n` answers a pairing request, `devices` lists paired handhelds, `revoke NAME` removes one, `web` prints a link to the history page, `quit` stops it.

Other commands: `a11helper send FILE [modes|library|media]` copies a file into the drop folder for the handheld. `a11helper history [NAME [LINES]]` prints a session's history. `a11helper sync-do ...` works the handheld while it is in sync mode, see the next sections.

### Pairing

On the handheld: GUI menu, Computer, pick a slot, press A. If the computer answers the broadcast it appears on its own and pairing starts. If not, press A again and type the computer's address. Both screens show a four digit code. If they match, type `y` in the helper window within 90 seconds.

### Windows through WSL

Ubuntu inside Windows is not a second computer. It is a program, and you open it by typing `wsl` in PowerShell.

1. Open PowerShell as administrator and run these lines.

```
wsl --install -d Ubuntu --no-launch
Set-Content "$env:USERPROFILE\.wslconfig" "[wsl2]`nnetworkingMode=mirrored"
New-NetFirewallRule -DisplayName a11helper -Direction Inbound -Protocol TCP -LocalPort 2222 -RemoteAddress 100.64.0.0/10,192.168.0.0/16 -Action Allow
New-NetFirewallRule -DisplayName a11find -Direction Inbound -Protocol UDP -LocalPort 2223 -RemoteAddress 192.168.0.0/16 -Action Allow
New-NetFirewallHyperVRule -Name a11helper -DisplayName a11helper -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol TCP -LocalPorts 2222
New-NetFirewallHyperVRule -Name a11find -DisplayName a11find -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 2223
wsl --shutdown
```

2. Type `wsl` and make a Linux user the first time.
3. In Ubuntu run these lines. Always start with `cd ~`, otherwise Ubuntu may be standing in a Windows folder it cannot write to.

```
cd ~
sudo apt update && sudo apt install -y tmux curl
curl -O https://woz.lol/cassiopeia/helper/a11helper-linux-amd64
chmod +x a11helper-linux-amd64
./a11helper-linux-amd64
```

4. Keep that window open, or use the start files from `https://woz.lol/cassiopeia/pc/`. `Start A11 Helper.bat` opens a visible window. `A11 Helper at login.bat` goes in the Startup folder and starts the helper minimized at every login.
5. Stop Windows from sleeping while plugged in. A sleeping PC cannot answer.

```
powercfg /change standby-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
```

## The SD card and the library

Format the card as FAT32. The handheld reads three folders.

| Folder | Holds |
|---|---|
| `library` | One folder per pack, plus `synonyms.txt` for everyday wording, `Start here.txt` and a `Notes` folder of loose text files. |
| `media` | Pictures, WAV and MP3 files. |
| `modes` | Mode images to install from the loader. |

`tools/sync_card.sh [VOLUME]` copies the packs staged in `~/a11-staging/packs`, the media samples, and the mode images in `modes/sd/modes` onto a card in the Mac. It only adds and updates, except for a short list of retired packs it removes. Files can also be put on the card over USB with `tools/loader.py put` and `tools/loader.py putdir`, and over Wi-Fi with Get files from the computer.

## The reader

Open Library. Enter opens a pack or a file. Inside a pack, the top line is the search box and every title is listed below it in order. Typing jumps the list to those letters and Enter searches the words. Searching everything is the first entry of the library. Search understands everyday wording, so pee hurts finds dysuria. The wording rules are in `library/synonyms.txt`, one `words = target, target` per line.

| Key or touch | Does |
|---|---|
| Up, Down, Alt+Up, Alt+Down | Move, or page. Alt+Left and Alt+Right are Home and End. |
| Tab and Enter | In an article, move to a link and follow it. A tap on a link follows it. Holding a finger on a link copies its words. |
| `/` | Find in the article. It works as you type, every match is highlighted. Up and Down in the box step through matches. Enter keeps the highlights. |
| `,` and `.` or `p` and `n` | Previous and next match after a find. |
| Taps on text | Two taps copy a word, three a line, four a paragraph, five the screen. The copy goes to the same clipboard as Mode 1. |
| Ctrl+V, Ctrl+C, Ctrl+X | Paste, copy and cut in the search box and the find box. |
| Scroll bar | The strip on the right edge. Drag it to jump. |
| Picture | Tap or Enter zooms in and back. Plus and minus change the zoom. Drag or the arrows move around. Esc, Backspace or holding a finger down closes. |
| T | Next theme. |

## Update the card over Wi-Fi, sync mode

Sync mode lets your computer, or Claude working on it, change the card and the clipboard without taking the card out and without plugging in a cable. It works at home and through Tailscale. The handheld makes the connection, so nothing on the handheld listens, and it is off unless you open it.

1. Make sure the helper is running on the computer you want to use.
2. On the handheld: GUI menu, Sync with the computer. It connects to the computer in use and shows Connected. The screen stays on and the handheld does not sleep while it is open. Esc ends it.
3. On the computer, in another window, use `a11helper sync-do`. If the handheld is not in sync mode, the command says so.

| Command | Does |
|---|---|
| `a11helper sync-do status` | Says whether the handheld is in sync mode. |
| `a11helper sync-do put FILE CARDPATH` | Sends one file. If the link drops, run it again and it carries on from where it stopped. |
| `a11helper sync-do putdir FOLDER CARDFOLDER` | Sends every file in a folder that is missing on the card or a different size. |
| `a11helper sync-do get CARDPATH FILE` | Takes a file from the card. |
| `a11helper sync-do list CARDFOLDER` | Lists the files and sizes under a folder on the card. |
| `a11helper sync-do rm CARDPATH` and `rmdir CARDFOLDER` | Deletes a file, or a folder with everything in it. |
| `a11helper sync-do clip list` | Shows the handheld's clipboard, numbered from 1. |
| `a11helper sync-do clip add TEXT`, `del N`, `set N TEXT` | Adds, deletes or changes a clip. |
| `a11helper sync-do install CARDFILE SLOT` | Installs a mode file already on the card into slot 1 to 4. |

Card paths start with `modes/`, `library/` or `media/`. Anything else is refused, and so is any path with `..` in it.

Updating a mode, using the names the loader recognizes:

```
a11helper sync-do put reader.bin modes/reader.bin
a11helper sync-do install modes/reader.bin 2
```

The handheld cannot overwrite the slot it is running from. While you are in Mode 1 you can install into slots 2 to 4. To update Mode 1 itself, send the file, then GUI menu, Back to the loader, pick `mode1.bin`, and press Enter. The loader sees the name matches slot 1.

Adding a pack to the library:

```
a11helper sync-do putdir ~/a11-staging/packs/honda-fit-2015-manual library/honda-fit-2015-manual
```

Over Wi-Fi this is slow for big packs. The first fill of a card with tens of gigabytes belongs in a card reader. Sync mode is for updates, new manuals, one mode at a time, the clipboard, and anything you want to do without opening the Casio.

Away from home it is the same: with Tailscale joined and the away address set, the handheld connects out through the tunnel, and the same commands work on the computer. The Status screen shows which route was used.

## Remote access with Tailscale, step by step

Goal: use the handheld away from home and still reach your computers, with nothing open to the internet. Tailscale gives each device a private address that starts with 100. The handheld only ever calls out. Your computers open no ports to the world.

How it is kept safe:

| Layer | What it does |
|---|---|
| Tailscale sign-in | Only devices on your own tailnet can reach each other. Turn on two-step login on your Tailscale account. |
| Access rules | The handheld gets one door only, port 2222 on your own computers. Nothing else. |
| One-time key | The key that joins the handheld expires in a day and works once. |
| Helper pairing | A computer only accepts devices you paired in person, by matching codes. |

### Part 1, the account and the computers

1. Make a Tailscale account at `https://login.tailscale.com`. Turn on two-step login in the account settings.
2. Install Tailscale on every computer the handheld should reach, and sign in with that account. On Windows install it on Windows itself, not inside WSL. WSL shares the PC's address when mirrored networking is on, as set up above.
3. Write down each computer's Tailscale address. It is in the admin page under Machines, or you can run `tailscale ip -4` on the computer. Example: `100.89.194.12`.
4. Check that the helper is reachable over Tailscale. From a different computer on your tailnet run `nc -vz 100.89.194.12 2222` and look for `succeeded`.

### Part 2, the access rules

In the admin page open Access controls, then JSON editor. Replace everything with the text below and press Save. It keeps your own devices able to reach everything, as before, and gives the handheld port 2222 on your computers only. The test at the bottom makes Tailscale refuse the save if the rule is wrong.

```
{
    "tagOwners": {
        "tag:casio": ["autogroup:admin"],
    },

    "grants": [
        {"src": ["autogroup:member"], "dst": ["*"], "ip": ["*"]},
        {"src": ["tag:casio"], "dst": ["autogroup:member"], "ip": ["tcp:2222"]},
    ],

    "ssh": [
        {
            "action": "check",
            "src":    ["autogroup:member"],
            "dst":    ["autogroup:self"],
            "users":  ["autogroup:nonroot", "root"],
        },
    ],

    "tests": [
        {
            "src": "tag:casio",
            "accept": ["100.89.194.12:2222"],
            "deny": ["100.89.194.12:22", "100.89.194.12:8000"],
        },
    ],
}
```

Change the address in the test to one of your computers. If your Access controls already had other rules, keep them and add the pieces above to them. Do not delete rules you do not understand.

### Part 3, the one-time key

1. In the admin page open Settings, then Keys, then Generate auth key.
2. Description `casio`. Reusable off. Expiration 1 day. Ephemeral off. Under Tags choose `tag:casio`.
3. Copy the key. It starts with `tskey-`. Do not share it, and do not put it in a file or a chat. Make it right before the next step, because it expires.

### Part 4, join the handheld

1. Install Mode 1 with the Tailscale item. GUI menu, Tailscale should be there.
2. Be on Wi-Fi at home. GUI menu, Tailscale, move to Join with a key, press Enter, type the key, press Enter.
3. Wait up to a minute. The Status line goes from Joining to Connected and shows a 100. address.
4. In the admin page under Machines you should see `casio` with the `tag:casio` tag.

If you would rather not type a key, switch Tailscale on without one. It shows a link on the screen. Open the link on a phone and approve. Then in the admin page under Machines edit the Casio's tags and add `tag:casio`, otherwise it has the same reach as your own devices.

### Part 5, tell it where each computer is

1. GUI menu, Computer. Move to the Mac's slot and press E. Type the Mac's Tailscale address, for example `100.89.194.12`, and press Enter.
2. Do the same for every other computer. Use the number, not the name. The tunnel does not look up names.
3. GUI menu, Tailscale. Check that Use the away address if home does not answer says Yes.

How it chooses a route: it always tries the home address first for 3 seconds. If that does not answer and Tailscale is connected, it uses the away address. At home nothing changes.

### Part 6, keep the computers ready

| Computer | Needs |
|---|---|
| All | Awake, on power or a good battery, Tailscale running and signed in, the helper running. |
| Mac | `caffeinate -i ./a11helper-darwin-arm64` keeps it awake while the helper runs. Or set the Mac not to sleep on power. |
| Windows | The two `powercfg` lines above, Tailscale signed in on Windows, Ubuntu open with the helper running or the login start file in place. |

### Part 7, test it without leaving the house

1. Turn on your phone's hotspot. On an iPhone turn on Maximize Compatibility, because the handheld only uses 2.4 GHz Wi-Fi.
2. GUI menu, Wi-Fi, join the hotspot. Tailscale shows Connected again after a short wait.
3. GUI menu, Computer, pick a computer with Enter. Then GUI menu, Sessions, and open a session. The first connect takes longer, because the home address is tried first.
4. GUI menu, Status. The Last link line says through Tailscale when the away address was used.
5. Repeat for every computer.

### Putting it away, and taking it back

| Task | How |
|---|---|
| Stop using it for now | GUI menu, Tailscale, switch it Off. |
| Remove the handheld from your tailnet | GUI menu, Tailscale, Forget this device. Then delete `casio` in the admin page under Machines. |
| Stop one handheld reaching your computers | In the helper window type `devices`, then `revoke NAME`. |
| Cut off the handheld without touching it | Delete `casio` in the admin page under Machines. The key it used is already spent. |

## Making your own packs

A pack is a folder of files the reader searches. Put it in `library` on the card.

| Tool | Makes a pack from |
|---|---|
| `tools/a11pack.py` | A Kiwix `.zim` file. Options include `--images`, `--no-molecules`, `--letters`, `--english-only`, `--articles-only`, `--no-people`, `--no-places`, `--no-entertainment`, `--no-sports`, `--per-article N|auto`, `--weight N`, `--name TITLE`. |
| `tools/a11manual.py` | A PDF manual, one article per page with a zoomable picture of the page. |
| `tools/a11json2pack.py` | A JSON list of entries. |
| `tools/a11hub.py` | The Quick topics page that links into the best articles of every pack. |
| `tools/a11fieldguide.py` | A plants and animals guide from Wikipedia, with pictures from Wikimedia Commons. |
| `tools/mirror_get.py` | Downloads a big Kiwix file from several mirrors at once and checks it. |

Run the tools with a Python that has Pillow, libzim and cairosvg. Pictures are shrunk to the screen size and anything with a face or a person is dropped on macOS using Vision.

## Problems and fixes

| What you see | Cause | Fix |
|---|---|---|
| No answer from the computer | The computer is asleep, the helper is not running, or the Wi-Fi is wrong | Wake it, start the helper, check GUI menu, Wi-Fi. |
| Session ended | The link dropped or the computer slept | Make sure the computer does not sleep, then open the session again. |
| Pairing says nothing is waiting | The request went to another computer, or 90 seconds passed | Check which computer the slot points at, start pairing again, and answer in the right helper window. |
| Find does not list a computer | The firewall blocks the broadcast | Press A again on the empty slot and type the address. |
| Tailscale stays at Joining | The key is wrong, spent or expired | Make a new key and join again. |
| Tailscale says the key was refused | Same | Same. |
| Sync command says the handheld is not in sync mode | The handheld is not on the Sync screen, or it connected to a different computer | Open GUI menu, Sync with the computer, and check which computer is in use. |
| Install says the slot is running | You asked it to overwrite the mode it is running from | Use the loader for that one. |
| Away connect works at home but not on the hotspot | The hotspot is 5 GHz only, or Tailscale on the computer is stopped | Turn on Maximize Compatibility, and check Tailscale on the computer. |
| Connect works from the Mac but the handheld times out | The access rule does not allow port 2222 for `tag:casio` | Check Part 2 and the tag on the handheld. |
| A pack is missing from search | It is not in `library`, or it has no `pack.txt` | Check the folder and run the sync again. |
| Loader says a slot is empty | Nothing is installed there | Install a mode file into it. |

## Safety notes

The handheld stores its SSH key encrypted. A short PIN could still be guessed offline by someone who has the device, so use a longer one if that matters. Anyone with the handheld, the PIN and your Wi-Fi can reach your paired computers, so the helper accepts only devices you paired and you can revoke one from the helper window at any time. Medical and survival material in the library is reference only and not advice. Old books in some packs are out of date. The recall and bulletin packs are summaries and say so.

## Repository layout

| Folder | Holds |
|---|---|
| `modes/` | One ESP-IDF project per mode, the shared defaults, the SD card starter files. |
| `components/` | Shared code: display, keys, SSH client, sessions, themes, clipboard, Tailscale link, status LED and more. |
| `helper/` | The Go program that runs on your computers. |
| `tools/` | Pack builders, SD sync, loader and keyboard tools. |
| `nrf/` and `nrf-tools/` | The keyboard chip's firmware and the tools that flash it over SWD. |
| `keymap/` | The keyboard map. |
| `Firmware/` | The latest built images and how to flash them. |
| `PLAN.md`, `WIRING.md` | The plan with its safety ledger, and the pin tables. |

Third party code and licenses are listed in `components/THIRD_PARTY.md`.
