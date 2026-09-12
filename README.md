# Deskie

I had deepseek create this project based on my initial prototype.

A desk-side volume and system-monitor controller for the **ESP32-S3 (N16R8)**, built around a
240 × 240 round **GC9A01** display. An animated background plays behind a set of radial gauges,
the rotary encoder drives the Windows volume and media transport, and the device is fed live
CPU / RAM / GPU data by a small Python bridge over the USB serial link.

> One USB cable, no extra wiring, no drivers: the board appears as a serial port and the
> Python script does the PC-side work.

---

## Features

* **Arc-based UI** over a dimmed, still-playing animation — resource gauges, detail pages,
  a volume gauge, transport controls and an animation picker all share one visual language.
* **Real Windows volume**, read *and* written, plus mute and the media keys
  (the firmware sends intent; the PC performs it).
* **Live metrics**: CPU load / clock / threads, RAM used-total, GPU load, GPU temperature,
  hotspot, memory temperature, GPU board power, fan speed, and VRAM used-total.
* **Now-playing ticker** on the idle screen with a rotating vector ornament, all tinted from
  the animation's palette.
* **Audio reactive spectrum**: eight log-spaced bands drawn as a mirrored radial ring over the
  upper half of the display, with a beat flash. All the DSP runs on the PC, so the board only
  receives eight small numbers.
* **Animations** stored in LittleFS as raw RGB565 frames, uploadable straight from the
  Arduino IDE.
* **Dual-core FreeRTOS firmware**: serial + input on core 0, rendering on core 1, with
  queues between them.
* **Graceful degradation on the PC side**: without LibreHardwareMonitor you still get CPU,
  RAM, GPU and VRAM usage from built-in Windows APIs.

---

## Architecture

```
   ┌────────────────────────────┐   USB-Converter serial (921600 8N1) ┌────────────────────┐
   │  ESP32-S3  N16R8           │ <─────────────────────────────────> │  PC                │
   │  GC9A01 240x240 round TFT  │                                     │  controller.py     │
   │  rotary encoder + buttons  │   metrics  ->  S,/E,/V,/T,          │  psutil            │
   │  LittleFS: RGB565 frames   │   commands <-  VSET,/MUTE,/MK,/REQ  │  LHM / PDH / pycaw │
   └────────────────────────────┘                                     └────────────────────┘
```

* The **PC** gathers metrics, owns the audio, and injects the media keys.
* The **board** owns the UI, the animation and all input, and never blocks on the link:
  it renders at its own pace and simply reflects the newest values it has.

---

## Repository layout

```
VolumeControllerUART/
├─ VolumeControllerUART.ino   setup + the three pinned FreeRTOS tasks
├─ config.h                   pins, animation palette, gauge geometry, timings
├─ state.h / state.cpp        shared runtime state + mutex + the two queues
├─ comms.h / comms.cpp        serial line protocol  (core 0)
├─ input.h / input.cpp        encoder ISR + buttons (core 0)
├─ display.h / display.cpp    TFT_eSPI wrapper: sprite, gauges, vector icons
├─ anim.h / anim.cpp          LittleFS frame player (core 1)
├─ ui.h / ui.cpp              screens, navigation state machine, rendering
├─ partitions.csv             custom partition layout (large LittleFS area)
├─ data/                      the animation frames (*.raw, 115200 B each)
└─ pc/
   ├─ controller.py           the headless PC bridge
   ├─ requirements.txt
   └─ README.md
```

---

## Hardware

| Function | GPIO | Notes |
|----------|------|-------|
| Display MOSI / SCLK | 11 / 12 | GC9A01, HSPI, 27 MHz |
| Display CS / DC / RST | 10 / 14 / 15 | configured in TFT_eSPI's `User_Setup.h` |
| Rotary encoder A / B | 5 / 4 | quadrature, interrupt driven |
| Encoder push button | 17 | short press = OK/enter, long press = back |
| Media play / next / prev | 6 / 7 / 8 | work from any screen, active-low |
| Shortcut: monitor | 21 | reserved, optional |
| Shortcut: volume | 38 | reserved, optional |
| Shortcut: media | 47 | reserved, optional |
| Shortcut: gallery | 48 | reserved, optional |

The display pins live in the **TFT_eSPI** library's `User_Setup.h`, not in this sketch — that is
normal for TFT_eSPI and keeps the driver configuration with the library.

The four "shortcut" pins are wired as `INPUT_PULLUP` and are active-low, so an **unwired pin
simply reads HIGH and never fires** — you can leave them unconnected and fit buttons later.
Set `ENABLE_SHORTCUT_BUTTONS 0` in `config.h` to release them.

The PC link uses the native USB port of the board (USB-CDC): one cable carries both flashing
and the data link.

---

## Build & flash

Arduino IDE 2.x, **esp32** board package **3.3.11**, board **ESP32S3 Dev Module**:

| Setting | Value |
|---------|-------|
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| Partition Scheme | **Custom** (uses `partitions.csv`) |
| USB Mode | Hardware CDC and JTAG |
| **USB CDC On Boot** | **Enabled** |
| Upload Speed | 921600 |
| CPU Frequency | 240MHz |
| Loop Core | Core 1 *(default)* |

> `USB CDC On Boot` selects which USB port the firmware talks on. **Enabled** = the native
> USB-CDC port (labelled `USB` on most devkits). If you set it to **Disabled**, `Serial` moves
> to UART0 (GPIO43/44) and you must use the board's `UART` (CH34x/CP210x bridge) port instead.
> Getting this pair wrong is the one thing that makes the link look dead — see *Troubleshooting*.

Steps:

1. Open `VolumeControllerUART.ino`.
2. Upload the sketch.
3. Upload the filesystem (**Tools ▸ ESP32 LittleFS Data Upload**). `data/` holds the animation
   frames and is a few MB, so this takes a moment.
4. Start the PC bridge (below).

Typical footprint: **~418 KB flash, ~24 KB static RAM** plus two 115 KB PSRAM frame buffers.

---

## The serial protocol

Plain ASCII, one message per line, `\n`-terminated, comma separated. Every value is an integer
and **`-1` means "unavailable"**. Unrecognised lines (such as firmware boot logs) are ignored
by both sides.

### PC → board

| Message | Meaning |
|---------|---------|
| `H,<ver>` | handshake / protocol version (the board answers with its own `H,1`) |
| `S,<cpu>,<ram>,<gpu>,<vram>` | summary percentages |
| `E,<13 values>` | detail payload, fixed order (see below) |
| `V,<volume>,<mute>` | current master volume 0..100 and mute flag |
| `T,<state>,<artist>\|<title>` | now playing; state 0 none / 1 playing / 2 paused |
| `A,<b0>..<b7>,<pulse>` | audio spectrum: eight band levels (0..100) plus a beat pulse |
| `P` | ping (the board answers `PONG`) |

`E` field order:

| # | field | unit | # | field | unit |
|---|-------|------|---|-------|------|
| 0 | CPU temperature | ×10 | 7 | GPU hotspot | ×10 |
| 1 | CPU package power | ×10 | 8 | GPU memory temperature | ×10 |
| 2 | CPU clock | MHz | 9 | GPU board power | ×10 |
| 3 | CPU threads | count | 10 | GPU fan | % |
| 4 | RAM used | MB | 11 | VRAM used | MB |
| 5 | RAM total | MB | 12 | VRAM total | MB |
| 6 | GPU temperature | ×10 | | | |

### Board → PC

| Message | Meaning |
|---------|---------|
| `H,<ver>` | boot announce + reply to the PC handshake |
| `VSET,<n>` | absolute volume target from the volume wheel (0..100) |
| `VOL,±n` | relative step — still accepted by the PC, no longer sent by the board |
| `MUTE,<0\|1>` | toggle mute |
| `MK,PP` / `MK,N` / `MK,P` | media play-pause / next / previous |
| `REQ,<C\|R\|G\|V>` | "send the detail payload for CPU/RAM/GPU/VRAM" |
| `PG,<PAGE>` | which screen is open (`MONITOR`/`VOLUME`/`MEDIA`/`GALLERY`) |
| `PONG` | answer to `P` |

The link is considered down after 1.5 s without traffic; the monitor page shows `LINK DOWN` and
the board keeps re-announcing `H,1` every 2 s until it hears from the PC.

> Why `VSET` and not `VOL,±1`? `GetMasterVolumeLevelScalar()` on Windows lags slightly behind a
> write, so a PC-side read-modify-write drifts and can even move the volume the wrong way. The
> board already knows the value it wants, so it sends the absolute target and the PC just applies
> it.

---

## The PC bridge

Headless — no GUI, one process, run it whenever you want the board connected.

```powershell
cd pc
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt

python controller.py --scan      # find the port the board actually answers on
python controller.py             # auto-detect and run (Ctrl+C to stop)
python controller.py --port COM5 -v
```

### Flags

| Flag | Purpose |
|------|---------|
| `--port COM5` | pin the port (default: probe every port for the board) |
| `--list` | list serial ports with VID:PID |
| `--scan` | probe every port for the board and exit |
| `--selftest` | test master-volume read **and** write, then exit |
| `--rate N` | metric updates per second sent to the board (default 1) |
| `--audio-test` | print the live audio band levels (no serial port) and exit |
| `--audio-rate N` | spectrum updates per second sent to the board (default 25) |
| `--audio-gain DB` | extra gain applied to every band (default 0) |
| `--no-audio` | disable the audio reactive spectrum |
| `--np-proc NAME` | extra media process to watch for now playing (repeatable) |
| `--no-volume` / `--no-nowplaying` | disable those features |
| `--no-pdh` / `--no-lhm` | disable a GPU backend |
| `--lhm-dll PATH` | point at `LibreHardwareMonitorLib.dll` explicitly |
| `--dry-run` | exercise every backend without opening a port |
| `--run-seconds N` | exit after N seconds (handy for testing) |
| `--verbose` / `-v` | print every protocol line in both directions |

### Where the numbers come from

| Data | Source | Needed install |
|------|--------|----------------|
| CPU load, clock, threads, RAM used/total | `psutil` | `pip install psutil` |
| GPU utilisation | LibreHardwareMonitor, else Windows PDH counters | none (PDH is built in) |
| VRAM used | LibreHardwareMonitor, else PDH `Dedicated Usage` | none |
| VRAM total | `HardwareInformation.qwMemorySize` from the registry | none |
| CPU / GPU temperature, hotspot, memory temp, board power, fan | LibreHardwareMonitor | **LibreHardwareMonitor** + `pip install pythonnet` |
| Master volume / mute | `pycaw` (Core Audio) | `pip install pycaw comtypes` |
| Media keys | `user32.keybd_event` | none |
| Now playing | Windows SMTC when `winsdk`/`winrt` is importable, else media window titles | none for the fallback |

Everything is layered, so a missing optional backend degrades to `--` on the display instead of
failing to start.

#### Installing the optional sensor backend

1. Download the latest release of
   [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) and
   extract the **whole folder** to `C:\Program Files\LibreHardwareMonitor\`
   (it must contain `LibreHardwareMonitorLib.dll`, `HidSharp.dll` and `WinRing0x64.sys`).
2. **Unblock the extracted files** — they carry Windows' "downloaded from the internet" mark and
   .NET then refuses to load the DLL (`0x80131515`, "network location"). In an **admin**
   PowerShell:

   ```powershell
   Get-ChildItem "C:\Program Files\LibreHardwareMonitor" -Recurse | Unblock-File
   ```
3. Run `LibreHardwareMonitor.exe` **as administrator** once, so its kernel driver is installed.
   Leaving it running is the simplest option.
4. `pip install pythonnet` and restart the bridge. It auto-detects the DLL and logs
   `LibreHardwareMonitor loaded: ...`; otherwise pass `--lhm-dll`.

---

## Screens & controls

**turn** = move / change value · **short press** = OK / enter · **long press** = back one level

| Screen | Shows | Controls |
|--------|-------|----------|
| **IDLE** | the animation, the audio spectrum ring, and a now-playing ticker with a rotating vector ornament | turn = open the menu |
| **MENU** | four page tiles | turn = move, OK = open, hold = close |
| **MONITOR** | CPU / RAM / GPU / VRAM gauges spread around the ring, each with its `%`, the selected one large in the centre | turn = pick, OK = detail, hold = menu |
| **DETAIL** | one component full screen: big gauge plus temperature / power / clock / threads / used-total | turn = cycle component, hold = monitor |
| **VOLUME** | a big gauge driven by the real Windows volume | turn = adjust, OK = mute, hold = menu |
| **MEDIA** | now playing plus prev / play-pause / next | turn = pick, OK = go, hold = menu |
| **GALLERY** | a segmented ring with a live preview of each animation | turn = preview, OK = keep, hold = cancel |

The dimmed animation keeps playing behind every overlay, **MONITOR never times out**, `DETAIL`
falls back to `MONITOR` after 20 s, and the other pages return to the animation after 8 s of
inactivity. The four dedicated media buttons work from any screen.

---

## Firmware architecture

| Task | Core | Prio | Responsibility |
|------|------|------|----------------|
| `taskComm` | 0 | 4 | serial RX line assembly, TX queue drain, link watchdog |
| `taskInput` | 0 | 5 | encoder sampling, button debounce, short/long press |
| `taskRender` | 1 | 6 | frame load, dim, gauges, `pushSprite` |

* `taskInput` posts `InputEvent`s onto `gInputQ`; `uiRender()` pumps that queue and feeds each
  event to `uiHandleEvent()` before drawing.
* The renderer posts `OutMsg`s onto `gOutQ`; `taskComm` drains those to the PC.
* The only shared state is one `Runtime` struct behind a mutex, snapshotted once per frame.
* `loop()` is intentionally empty — all work is in the pinned tasks.

**Rendering budget.** The panel bus runs at 27 MHz, so a full 240 × 240 frame takes ~34 ms to
push; that caps the display at roughly **29 fps** whatever the animation's `fps` says. Rendering
is also "dirty" filtered: if neither a new frame was loaded nor any displayed value changed, the
frame simply is not re-pushed.

---

## Customising

Almost everything you would want to change lives in **`config.h`**.

### The colour palette

Each animation carries its own three colours, so the whole UI re-tints when you switch animation:

```cpp
struct GifPreset {
  const char* prefix;     // LittleFS filename prefix, without the "_n.raw"
  uint8_t     frames;     // number of frames on disk
  uint8_t     fps;        // target playback rate
  uint16_t    accent;     // main colour  (outlines, gauge track, labels)
  uint16_t    highlight;  // active colour (filled gauge, selection, now playing)
  uint16_t    tint;       // dark fill behind gauges
};
```

Colours are RGB565, e.g. `0xF800` = red, `0x07E0` = green, `0x001F` = blue.

### Adding an animation

1. Put the source frames in `source_frames/` (PNG/JPG, **240 × 240**, named so that a plain
   alphabetical sort puts them in playing order).
2. Run `python img_to_565.py` from the project root. It converts every image into a
   little-endian RGB565 dump in `data/`, keeping the base name — `mypic.png` → `data/mypic.raw`.
   If you want a specific prefix, either rename the images first or edit `img_to_565.py`.
3. Add **one line** to `GIF_TABLE[]` in `config.h`:

   ```cpp
   { "mypic",  24,  20,  0x07E0,  0x9FEA,  0x0A61 },   // prefix, frames, fps, accent, highlight, tint
   ```
   `frames` must equal the number of `.raw` files for that prefix.
4. Bump `GIF_COUNT` so it matches the number of rows in the table.
5. Re-upload the sketch, then re-upload the filesystem (LittleFS).

**Nothing in the UI has to change.** The gallery ring divides itself from `GIF_COUNT`, the
`n / N` counter and the centre label follow automatically, and the new palette entry tints every
screen when that animation is selected.

### Removing an animation

1. Delete its row from `GIF_TABLE[]` in `config.h`.
2. Decrement `GIF_COUNT`.
3. Optionally delete the matching `data/<prefix>_*.raw` files so the filesystem stays small.
4. Re-upload the sketch (and the filesystem if you removed files).

Keep `prefix` short-ish (under ~24 characters) — it is drawn in the centre of the gallery ring.
Also remember each frame costs 115 200 bytes: the LittleFS partition is ~13.4 MB, so about 115
frames fit in total.

### Audio reactive spectrum

The PC captures the **default output device** with WASAPI loopback (`pyaudiowpatch`), splits it
into `AUDIO_BANDS` log-spaced bands with a small biquad bank, applies auto-gain, detects beats and
sends `A,<b0>..<b7>,<pulse>` roughly 25 times a second. The board only draws the numbers:

| Define | Meaning |
|--------|---------|
| `ENABLE_SPECTRUM` | `0` to ignore the `A,` messages completely |
| `AUDIO_BANDS` | band count - must match `controller.py` |
| `SPEC_SEGMENTS` / `SPEC_GAP_DEG` | number of bars and the gap between them |
| `SPEC_R_INNER` / `SPEC_R_MAX` | bar length at 0 % and at 100 % |
| `SPEC_GAMMA` | `0.62` lifts mid levels so quiet bands still show |
| `SPEC_START_DEG` / `SPEC_END_DEG` | the arc the bars occupy (default: the upper half, which leaves the bottom free for the ticker) |
| `SPEC_PULSE_R` / `SPEC_PULSE_W` | beat ring radius and thickness |

PC-side knobs worth knowing:

* `--audio-test` - print the eight levels live (no board needed) to check the capture;
* `--audio-rate` - updates per second (lower it if the animation looks starved);
* `--audio-gain DB` - manual boost on top of the auto-gain (try `+10` for very quiet mixes).

### Gauge geometry and timings

| Group | What it controls |
|-------|------------------|
| `MON_*` | the four monitor gauges: radius, thickness, arc length, gap, label/value radii |
| `BIG_*` | the large gauge used by DETAIL and VOLUME (radius, sweep start/end) |
| `GAL_*` | the gallery ring (radii and the gap between segments) |
| `MENU_*` | tile size and corner radius |
| `IDLE_SPIN_PERIOD_MS` | one full turn of the idle now-playing ornament |
| `UI_IDLE_TIMEOUT_MS` / `UI_DETAIL_TIMEOUT_MS` | auto-return timers |
| `LONGPRESS_MS` / `BTN_DEBOUNCE_MS` | button feel |
| `RX_STALE_MS` | how long without PC traffic before the link is flagged down |
| `CORE_*`, `TASK_*` | task affinity, priority and stack sizes |

### Volume step size

The wheel sends the **absolute** target on every detent, so the step size lives in the firmware —
`ui.cpp`, `onEncoder()`, `case MODE_VOLUME`:

```cpp
gState.volume = (int16_t)constrain((int)gState.volume + dir, 0, 100);
```

`dir` is ±1 per detent. For 2 % per detent:

```cpp
gState.volume = (int16_t)constrain((int)gState.volume + dir * 2, 0, 100);
```

> Changing `+1`/`-1` in `controller.py` no longer affects the wheel: that code path only serves
> the legacy relative `VOL,±n` message, which the board no longer sends. Likewise `--rate` only
> controls how often metrics are pushed, not the volume.

### Changing the fonts

All drawing happens on a 16-bit **sprite**, and every text call passes its font explicitly:

```cpp
spr.drawString(txt, x, y, font);        // display.cpp
```

So a single `tft.setFont(...)` / `spr.setTextFont(...)` in `setup()` will **not** re-font the UI —
the per-call font number wins. To restyle, edit the font numbers at the call sites: the small
literals in `display.cpp` (`dispTextCentre`, `dispTextAt`, `dispToast`) and in `ui.cpp`
(`drawIdle`, `drawMenu`, `drawMonitor`, `drawDetail`, `drawVolume`, `drawMedia`, `drawGallery`).

| Font | Size | Used for |
|------|------|----------|
| 1 | 8 px | small labels, hints, artist line |
| 2 | 16 px | page names, titles, component names |
| 4 | 26 px | per-gauge percentage values |
| 6 | 48 px | monitor centre reading |
| 7 | 48 px | DETAIL / VOLUME big reading |

TFT_eSPI only compiles the fonts enabled in its `User_Setup.h` (1, 2, 4, 6, 7 and 8 by default).
A genuinely different typeface means enabling `LOAD_GFXFF` there and using `setFreeFont()` with an
Adafruit-GFX font.

A tidy way to make restyling a one-line change is to add `UI_FONT_SMALL` / `UI_FONT_MID` /
`UI_FONT_BIG` to `config.h` and swap the literals for them.

---

## Notes & limitations

* The panel bus runs at 27 MHz, so a full frame takes ~34 ms to push — the practical ceiling is
  about **29 fps**. The `fps` field in `GIF_TABLE` is a target; 20–30 is realistic.
* Now playing falls back to **window titles** when `winsdk`/`winrt` is unavailable. It scans every
  window (including players minimised to the tray), ignores Windows helper windows, and *scores*
  the candidates — a browser tab with a known site beats a generic window — so a title such as
  `Anything | YouTube Music - Brave` becomes `Anything`. That fallback always reports "playing"
  (Windows does not expose pause state without SMTC). If a player is not recognised, run with `-v`
  to see the candidates and add it with `--np-proc yourplayer.exe`.
* `winsdk` currently has no wheel for Python 3.14, so on that interpreter the bridge uses the
  window-title fallback. To get real SMTC metadata (artist, play state), build the venv with
  Python 3.12/3.13 and `pip install winsdk` — it is picked up automatically.
* LibreHardwareMonitor needs its kernel driver: launch the application once as administrator.
  Without it the bridge still reports CPU/RAM/GPU/VRAM usage, and the temperature, power and fan
  fields show `--`.
* The four shortcut buttons are optional and unconnected by default.
* The audio spectrum needs `pyaudiowpatch` (`pip install pyaudiowpatch`); without it the bridge
  logs a note and never sends `A,` messages. It captures the **default output device**, so
  whatever Windows is playing is what you see. The auto-gain tracks the loudest band, so the
  display looks right at any system volume, and it stays silent below the noise gate.

---

## Troubleshooting

### The PC log shows only `->` lines and no `<-` lines

The board and the PC are not talking. The UI and the animation are local to the firmware, so they
keep working — the giveaway is that the board keeps re-sending `H,1` and the PC never logs
`<- H,1`.

The ESP32-S3 has **two** USB ports and the firmware only talks on the one that matches
**`USB CDC On Boot`**:

| `USB CDC On Boot` | Firmware `Serial` is on | Cable must be in |
|-------------------|-------------------------|------------------|
| **Enabled** | native USB-CDC (GPIO19/20) | the port labelled **USB** |
| **Disabled** | UART0 (GPIO43/44) | the port labelled **UART** (CH34x/CP210x bridge) |

A CH34x/CP210x entry in `--list` is the **bridge** port; an Espressif device with VID `303A` is
the **native USB**. Let the script find it:

```powershell
python controller.py --scan

  probing COM3     ... no answer
  probing COM7     ... answered

board answered on COM7  ->  use --port COM7
```

Either move the cable to the other port so it matches the setting, or change `USB CDC On Boot`
and re-flash. You must then see, within ~2 s:

```
[..] board handshake received
  <- H,1
```

Independent check: open the Arduino IDE **Serial Monitor at 115200** on the port and press the
board's **RST** button — you should see the boot banner with the storage report.

### The volume wheel does nothing, or jumps

1. Confirm the link first (above) — the PC only ever sees `VSET,` if the link is up.
2. `python controller.py --selftest` must print `volume read/write OK`.
3. Run the bridge **without** administrator rights; Core Audio needs no elevation.
4. The bridge re-sends the volume every 3 s, so the gauge resyncs by itself, and if a Core Audio
   write ever fails it falls back to tapping the system volume keys.

### `Cannot change thread mode after it is set` (`0x80010106`)

Importing **pythonnet** (the LibreHardwareMonitor backend) initialises the main thread as MTA, and
comtypes/pycaw then cannot switch it to STA. The bridge works around this by running every Core
Audio call on its own dedicated STA worker thread; if you see this error you are running an older
copy of `controller.py`.

### `0x80131515` / "network location" when loading LibreHardwareMonitor

The extracted DLL is blocked by Windows ("mark of the web"). Fix it once in an **admin**
PowerShell, then restart the bridge:

```powershell
Get-ChildItem "C:\Program Files\LibreHardwareMonitor" -Recurse | Unblock-File
```

### Metric readings show `--`

* `-1` is the protocol's "unavailable" sentinel, so `--` simply means the PC has no value yet.
* CPU/RAM/GPU/VRAM should always be present; if they are not, check that `psutil` is installed and
  that the link is up (above).
* Temperatures, power and fan come from LibreHardwareMonitor — see the install notes above.







