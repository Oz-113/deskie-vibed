# `controller.py` - PC side bridge

Headless service that feeds the ESP32-S3 controller and executes what the wheel and
buttons ask for. No GUI.

## Install

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1          # PowerShell
pip install -r requirements.txt
```

The mandatory set is `pyserial`, `psutil`, `pycaw` + `comtypes` (volume and media keys) and
`pyaudiowpatch` (audio spectrum). `pythonnet` is optional but recommended - with
LibreHardwareMonitor installed it adds temperatures, hotspot, board power, fan speed and exact
VRAM.

## Run

```powershell
python controller.py --list                 # list COM ports
python controller.py --port COM5            # start (Ctrl+C stops)
python controller.py                        # auto-detect the Espressif USB-CDC port
```

Handy for testing without hardware:

```powershell
python controller.py --dry-run --run-seconds 10 -v
```

## What it does

* pushes `S,` (summary, 4 Hz) and `E,` (detail, 1 Hz) to the board,
* mirrors the Windows master volume with `V,` and applies `VOL`/`VSET`/`MUTE` coming
  back from the board,
* injects the media keys for `MK,PP` / `MK,N` / `MK,P`,
* reports the current track with `T,`,
* captures the system audio and pushes the eight-band spectrum plus beat pulse with `A,`.

## Audio spectrum

`pyaudiowpatch` records the **default output device** through WASAPI loopback; a small biquad bank
splits it into eight log-spaced bands, auto-gain scales them to the current loudness and an
envelope detector produces the beat pulse. You can check the capture on its own, with no board
attached:

```powershell
python controller.py --audio-test
```

It prints the eight levels ten times a second - they should move with the music:

```
  [ 29  21  10   6   7   3   0   0]  beat=  0  ........
  [ 38  22   8   0   0   3  10   4]  beat=100  +.......
```

If the levels barely move, add gain (`--audio-gain 10`). If the animation on the board looks
starved, lower the send rate (`--audio-rate 15`).

## Files

| File | Purpose |
|------|---------|
| `controller.py` | the service (serial link, backends, protocol, CLI) |
| `requirements.txt` | pip dependencies |

See `../README.md` for the full protocol and architecture description.
