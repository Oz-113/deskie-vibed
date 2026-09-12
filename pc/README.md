# `controller.py` - PC side bridge

Headless service that feeds the ESP32-S3 controller and executes what the wheel and
buttons ask for. No GUI.

## Install

```powershell
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

`pyserial` + `psutil` + `pycaw` + `comtypes` are the mandatory set. `pythonnet` is
optional but recommended (with LibreHardwareMonitor it adds temperatures, hotspot,
board power, fan speed and exact VRAM).

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
* reports the current track with `T,`.

## Files

| File | Purpose |
|------|---------|
| `controller.py` | the service (serial link, backends, protocol, CLI) |
| `requirements.txt` | pip dependencies |

See `../README.md` for the full protocol and architecture description.
