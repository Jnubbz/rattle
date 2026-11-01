# rattle

Two-way Rattlegram-compatible text messaging between computers and radios, keyed by
**CAT or RTS — never VOX**. One small program for Windows, macOS and Linux/Raspberry Pi.

- Uses aicodix's Rattlegram encoder/decoder unchanged, so it also talks to the
  Rattlegram Android/iOS apps.
- Up to 170 bytes of UTF-8 per message, about 1–2 s on the air, 1600 Hz wide.
- Transmit: key (CAT `TX;` / RTS) → play the signal → unkey. Watchdog unkeys after
  `max_tx_s`; Ctrl+C unkeys too. The radio is never keyed if the audio device fails to open.
- Receive: decodes continuously; prints, logs, and can run a command per message.

## Commands

```
rattle                       chat: listen, and send each line you type (Enter)
rattle listen                receive only
rattle send "message"        send one message and exit (empty = ping)
rattle devices               list audio devices
rattle ptt-test [ms]         key the radio with no audio (default 1000 ms)
rattle send "msg" --wav f.wav    write the signal to a file (no radio)
rattle listen --wav f.wav        decode a recording
```

Settings come from `rattle.ini` (current folder, then next to the program) or
`--config FILE`. Any setting can be overridden: `--ptt cat --port COM5 --level 0.3`.
Ready-made configs are in `examples/`.

| setting | meaning |
|---|---|
| `callsign` | sent in every header (max 9 chars) |
| `input`, `output` | audio device: part of its name, or its number from `rattle devices` |
| `ptt` | `cat`, `rts`, `dtr` or `none` |
| `port`, `baud` | serial port for PTT (`COM5`, `/dev/cu.…`) |
| `cat_on`, `cat_off` | CAT strings. FX-4CR (fx4cr-open): `TX;` / `RX;` |
| `level` | transmit audio 0–1. Set drive with this; keep ALC quiet |
| `carrier` | audio centre frequency, default 1500 Hz (same as the app) |
| `noise` | leading noise symbols 0–6 (helps the far receiver's AGC) |
| `lead_ms`, `tail_ms` | delay after keying / before unkeying |
| `max_tx_s` | transmit watchdog |
| `log` | append every sent/received message to this file |
| `on_message` | command run per received message. Text arrives **only** in env vars `RATTLE_FROM`, `RATTLE_TEXT`, `RATTLE_TIME` — never pasted into the command — so received text can't inject shell commands |
| `verbose` | show sync, centre frequency, corrected bits |

## Field: Toughbook (Windows) + FX-4CR

1. Unzip `rattle.exe` and `rattle.ini` into a folder.
2. Close WSJT-X (only one program can hold the COM port).
3. `rattle devices` → put part of the FX-4CR sound card name in `input`/`output`.
4. Device Manager → Ports → set `port = COMx`.
5. Radio in **DIG** mode (USB side), dummy load: `rattle ptt-test` — it should key for 1 s.
6. `rattle` and type a message.

## Home: Mac mini M4 + FT-710

Build once (Apple Silicon):

```
xcode-select --install          # if not already installed
brew install cmake
cd rattle && cmake -S . -B build && cmake --build build -j
cp examples/home-mac-ft710.ini rattle.ini
./build/rattle devices
ls /dev/cu.*                    # FT-710 shows two Silicon Labs ports
./build/rattle ptt-test         # dummy load / low power
./build/rattle
```

FT-710: radio in **DATA-U**, data PTT ("RPTT SELECT") = **RTS**, data audio source = **USB**,
`port` = the **Standard** COM port. Opening the port can briefly assert RTS before rattle
clears it (a fraction of a millisecond, once at start-up). If you'd rather avoid that,
use the CAT alternative shown in the example config, verified with `ptt-test` first.

Quit WSJT-X while rattle is running.

## Raspberry Pi (POTA Pi)

```
sudo apt install cmake g++
cmake -S . -B build && cmake --build build -j4
```

## Building every platform on GitHub

Push this folder to a repo: `.github/workflows/build.yml` builds Windows, macOS (Apple
Silicon), Linux x64 and Linux ARM64 (Pi), runs a loopback test on each, and uploads them
as artifacts.

## What to expect on the air

- Rattlegram needs a clean signal: in simulation it decodes reliably down to about
  **+3 dB SNR** (2.4 kHz bandwidth) and fails around 0 dB. That's far less sensitive
  than FT8, so pick a band and time with solid copy (e.g. 40 m NVIS across Florida
  during the day, 80 m at night) and agree on a frequency.
- Messages up to 85 bytes use the most robust mode; 86–128 and 129–170 bytes use
  progressively weaker codes. Short is better.
- Tuning error up to tens of Hz is fine.
- Rattlegram is not encryption: the code is open and documented. Your callsign is in
  every header; sign the message text too if you want plain-language ID.

## First test plan

1. Offline: `rattle send "test" --wav t.wav` then `rattle listen --wav t.wav`.
2. Each station into a dummy load: `rattle ptt-test`, then `rattle send "test"`;
   watch power and ALC, adjust `level`.
3. Short range on the air (field kit in the driveway, low power), both directions.
4. Real distance.

## Credits and licences

- Rattlegram modem: Ahmet Inan / aicodix GmbH, 0BSD — `third_party/rattlegram/`
  (from github.com/aicodix/rattlegram @ 4df0f3a).
- miniaudio: David Reid, public domain / MIT-0 — `third_party/miniaudio/`.
