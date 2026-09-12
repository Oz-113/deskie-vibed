#!/usr/bin/env python3
"""
============================================================================
 controller.py - PC side bridge for the ESP32-S3 UART Volume Controller
============================================================================
 Headless replacement for the old USB-HID firmware. It

   * reads CPU / RAM / GPU / VRAM / temperature / power / fan data,
   * reads and changes the Windows master volume,
   * injects the media keys,
   * reports the currently playing track,
   * and talks to the board with a simple line protocol over the USB
     serial port (the same cable the old HID build used).

 Run with --list to see the available COM ports, then:

     python controller.py --port COM5

 See README.md for the full protocol description.
============================================================================
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import math
import os
import queue
import struct
import sys
import threading
import time
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Protocol constants - these MUST match firmware config.h / comms.cpp
# ---------------------------------------------------------------------------
PROTO_VERSION = 1

SUMMARY_HZ   = 1.0      # S,<cpu>,<ram>,<gpu>,<vram>  (default; --rate overrides)
DETAIL_HZ    = 1.0      # E,<13 values>                (default; --rate overrides)
VOLUME_HZ    = 4.0      # V,<volume>,<mute>
VOLUME_RESYNC_S = 3.0   # re-send the volume even if unchanged (self-healing)
NOWPLAY_HZ   = 1.0      # T,<state>,<artist>|<title>
HELLO_HZ     = 2.0      # H,<ver> until the board answers

NA = -1                 # "not available" sentinel used by the firmware


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def clamp(v: float, lo: float = 0.0, hi: float = 100.0) -> int:
    return int(max(lo, min(hi, v)))


@dataclass
class Metrics:
    """One complete snapshot; every field is an int, NA == unavailable."""
    cpu: int = NA
    ram: int = NA
    gpu: int = NA
    vram: int = NA

    cpu_temp_x10: int = NA
    cpu_power_x10: int = NA
    cpu_freq_mhz: int = NA
    cpu_cores: int = NA

    ram_used_mb: int = NA
    ram_total_mb: int = NA

    gpu_temp_x10: int = NA
    gpu_hot_x10: int = NA
    gpu_memtemp_x10: int = NA
    gpu_power_x10: int = NA
    gpu_fan: int = NA

    vram_used_mb: int = NA
    vram_total_mb: int = NA

    def summary_line(self) -> str:
        return f"S,{self.cpu},{self.ram},{self.gpu},{self.vram}"

    def detail_line(self) -> str:
        # order MUST match state.cpp / the README table
        return ("E," + ",".join(str(v) for v in (
            self.cpu_temp_x10, self.cpu_power_x10, self.cpu_freq_mhz, self.cpu_cores,
            self.ram_used_mb, self.ram_total_mb,
            self.gpu_temp_x10, self.gpu_hot_x10, self.gpu_memtemp_x10,
            self.gpu_power_x10, self.gpu_fan,
            self.vram_used_mb, self.vram_total_mb,
        )))


@dataclass
class NowPlaying:
    state: int = 0          # 0 none, 1 playing, 2 paused
    artist: str = ""
    title: str = ""

    def line(self) -> str:
        # keep the payload free of separators the firmware uses
        a = self.artist.replace(",", " ").replace("|", " ")[:31]
        t = self.title.replace(",", " ").replace("|", " ")[:39]
        return f"T,{self.state},{a}|{t}"


# ---------------------------------------------------------------------------
# libc / user32 for media keys and window titles (no third-party deps)
# ---------------------------------------------------------------------------
_user32 = ctypes.windll.user32

VK_MEDIA_NEXT_TRACK = 0xB0
VK_MEDIA_PREV_TRACK = 0xB1
VK_MEDIA_STOP       = 0xB2
VK_MEDIA_PLAY_PAUSE = 0xB3
VK_VOLUME_DOWN      = 0xAE
VK_VOLUME_UP        = 0xAF
KEYEVENTF_KEYUP     = 0x0002


def _send_vk(vk: int) -> None:
    _user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.01)
    _user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)


# ---------------------------------------------------------------------------
# Serial link
# ---------------------------------------------------------------------------
def list_ports() -> list:
    try:
        from serial.tools import list_ports as lp
    except Exception:
        log("pyserial is not installed - run: pip install -r requirements.txt")
        return []
    return list(lp.comports())


def autodetect_port() -> str | None:
    """Prefer an Espressif CDC/JTAG device, then any USB serial port."""
    ports = list_ports()
    for p in ports:
        if p.vid == 0x303A:                       # Espressif
            return p.device
    for p in ports:
        desc = (p.description or "").lower()
        if "usb" in desc or "jtag" in desc or "cdc" in desc:
            return p.device
    return ports[0].device if ports else None


def probe_port(port: str, baud: int, timeout: float = 1.5) -> bool:
    """Open 'port', poke the board with a ping and see whether it answers."""
    try:
        import serial
    except Exception:
        return False
    try:
        s = serial.Serial(port=port, baudrate=baud, timeout=0, write_timeout=0.5)
    except Exception:
        return False
    try:
        time.sleep(0.1)
        s.reset_input_buffer()
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            s.write(b"P\n")                        # firmware answers PONG
            time.sleep(0.2)
            n = s.in_waiting
            if n:
                buf += s.read(n)
                if b"PONG" in buf or b"H," in buf:
                    return True
        return False
    except Exception:
        return False
    finally:
        try:
            s.close()
        except Exception:
            pass


def scan_for_board(baud: int) -> str | None:
    """
    Return the first serial port that actually speaks the protocol.

    This is the reliable way to find the right port: the ESP32-S3 exposes TWO
    USB ports (native USB-CDC and the on-board UART bridge) and only one of them
    matches the firmware's 'USB CDC On Boot' setting.
    """
    ports = list_ports()
    if not ports:
        return None
    ordered = sorted(ports, key=lambda p: 0 if p.vid == 0x303A else 1)
    for p in ordered:
        print(f"  probing {p.device:8} ...", end=" ", flush=True)
        ok = probe_port(p.device, baud)
        print("answered" if ok else "no answer", flush=True)
        if ok:
            return p.device
    return None


class Link:
    """Non-blocking line oriented serial link (with a dry-run mode)."""

    def __init__(self, port: str | None, baud: int, dry_run: bool, verbose: bool):
        self.dry      = dry_run
        self.verbose  = verbose
        self.rx       = bytearray()
        self.ser      = None
        self.board_hello = False

        if dry_run:
            log("dry-run: no serial port will be opened")
            return

        try:
            import serial
        except Exception:
            log("pyserial is not installed - run: pip install -r requirements.txt")
            raise SystemExit(2)

        if port is None:
            log("auto-detecting the board (probing every serial port) ...")
            found = scan_for_board(baud)
            if found:
                port = found
                log(f"board answered on {port}")
            else:
                port = autodetect_port()
                if port is None:
                    log("no serial port found - use --list and --port")
                    raise SystemExit(2)
                log(f"WARNING: no port answered the handshake, using {port} anyway")
                log("         no '<- H,1' lines within a few seconds means the")
                log("         cable is in the wrong USB port (see README 4/11).")

        self.ser = serial.Serial(port=port, baudrate=baud, timeout=0,
                                 write_timeout=0.5)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        log(f"opened {port} @ {baud}")

    def write(self, line: str) -> None:
        if self.verbose:
            print(f"  -> {line}", flush=True)
        if self.dry:
            return
        try:
            self.ser.write((line + "\n").encode("ascii", "replace"))
        except Exception as e:                    # never let the loop die
            log(f"write failed: {e}")

    def read_lines(self) -> list[str]:
        if self.dry:
            return []
        try:
            n = self.ser.in_waiting
            if n:
                self.rx.extend(self.ser.read(n))
        except Exception as e:
            log(f"read failed: {e}")
            return []

        lines = []
        while True:
            i = self.rx.find(b"\n")
            if i < 0:
                break
            raw = bytes(self.rx[:i]).decode("ascii", "ignore").strip()
            del self.rx[: i + 1]
            if raw:
                lines.append(raw)
        if len(self.rx) > 8192:                   # guard against garbage floods
            del self.rx[:-256]
        return lines

    def close(self) -> None:
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# CPU / RAM - psutil is the mandatory dependency
# ---------------------------------------------------------------------------
class PsutilBackend:
    def __init__(self) -> None:
        self.ok = False
        try:
            import psutil
            self.psutil = psutil
            self.ok = True
        except Exception:
            log("psutil is not installed - CPU/RAM will be unavailable")

    def apply(self, m: Metrics) -> None:
        if not self.ok:
            return
        ps = self.psutil
        try:
            m.cpu = clamp(ps.cpu_percent(interval=None))
            vm = ps.virtual_memory()
            m.ram = clamp(vm.percent)
            m.ram_used_mb  = int(vm.used / (1024 * 1024))
            m.ram_total_mb = int(vm.total / (1024 * 1024))
        except Exception:
            pass
        try:
            f = ps.cpu_freq()
            if f:
                m.cpu_freq_mhz = int(f.current)
        except Exception:
            pass
        try:
            m.cpu_cores = ps.cpu_count(logical=True)
        except Exception:
            pass

    def prime(self) -> None:
        """First cpu_percent() call always returns 0.0 - warm it up."""
        if self.ok:
            self.psutil.cpu_percent(interval=None)


# ---------------------------------------------------------------------------
# Mark-of-the-web removal
#   A DLL extracted from a downloaded ZIP carries a "Zone.Identifier" ADS.
#   .NET then refuses to LoadFrom it with 0x80131515 ("network location"),
#   which is exactly what LibreHardwareMonitorLib.dll hits. DeleteFileW on
#   "<file>:Zone.Identifier" strips the mark - the same thing Unblock-File does.
# ---------------------------------------------------------------------------
def _unblock_zone(path: str) -> bool:
    try:
        return bool(ctypes.windll.kernel32.DeleteFileW(f"{path}:Zone.Identifier"))
    except Exception:
        return False


def _has_zone(path: str) -> bool:
    """True when the file still carries a Zone.Identifier (blocked) stream."""
    try:
        attrs = ctypes.windll.kernel32.GetFileAttributesW(f"{path}:Zone.Identifier")
        return attrs != 0xFFFFFFFF
    except Exception:
        return False


def _unblock_folder(folder: str) -> int:
    cleared = 0
    try:
        for root, _dirs, files in os.walk(folder):
            for f in files:
                if _unblock_zone(os.path.join(root, f)):
                    cleared += 1
    except Exception:
        pass
    return cleared


def _blocked_count(folder: str) -> int:
    n = 0
    try:
        for root, _dirs, files in os.walk(folder):
            for f in files:
                if _has_zone(os.path.join(root, f)):
                    n += 1
    except Exception:
        pass
    return n


# ---------------------------------------------------------------------------
# LibreHardwareMonitor backend
#   Recommended: it is the only driver-free way to get AMD GPU temperature,
#   hotspot, board power, fan speed and VRAM totals on Windows.
#   Install LibreHardwareMonitor (it ships LibreHardwareMonitorLib.dll) or
#   point --lhm-dll at the DLL.
# ---------------------------------------------------------------------------
class LhmBackend:
    DEFAULT_PATHS = (
        r"C:\Program Files\LibreHardwareMonitor\LibreHardwareMonitorLib.dll",
        r"C:\Program Files (x86)\LibreHardwareMonitor\LibreHardwareMonitorLib.dll",
        os.path.expandvars(r"%LOCALAPPDATA%\LibreHardwareMonitor\LibreHardwareMonitorLib.dll"),
        os.path.expandvars(r"%USERPROFILE%\LibreHardwareMonitor\LibreHardwareMonitorLib.dll"),
    )

    def __init__(self, dll: str | None = None) -> None:
        self.ok       = False
        self.computer = None

        if dll is None:
            for p in self.DEFAULT_PATHS:
                if os.path.isfile(p):
                    dll = p
                    break
        if dll is None or not os.path.isfile(dll):
            log("LibreHardwareMonitor not found - GPU temps/power/VRAM unavailable")
            return

        try:
            import clr                                     # pythonnet
            dll = os.path.abspath(dll)
            folder = os.path.dirname(dll)

            # .NET refuses to load an assembly that still carries the
            # "downloaded from the internet" mark (0x80131515 / "network
            # location"), which is what a DLL from a downloaded ZIP has.
            blocked = _blocked_count(folder)
            if blocked:
                cleared = _unblock_folder(folder)
                left    = _blocked_count(folder)
                log(f"mark-of-the-web: {blocked} blocked file(s), cleared {cleared}")
                if left:
                    log(f"  -> {left} still blocked (needs admin). Run once:")
                    log(f'     Get-ChildItem "{folder}" -Recurse | Unblock-File')

            sys.path.append(folder)
            try:
                os.add_dll_directory(folder)               # HidSharp etc.
            except Exception:
                pass
            clr.AddReference(dll)

            from LibreHardwareMonitor.Hardware import (    # type: ignore
                Computer, HardwareType, SensorType,
            )
            comp = Computer()
            comp.IsCpuEnabled    = True
            comp.IsGpuEnabled    = True
            comp.IsMemoryEnabled = True
            comp.Open()

            self.Computer     = Computer
            self.HardwareType = HardwareType
            self.SensorType   = SensorType
            self.computer     = comp
            self.ok           = True
            log(f"LibreHardwareMonitor loaded: {dll}")
        except Exception as e:
            first = str(e).splitlines()[0] if str(e) else repr(e)
            log(f"LibreHardwareMonitor unavailable: {first}")
            msg = str(e)
            if ("network location" in msg or "0x80131515" in msg
                    or "LoadFrom" in msg):
                log("  -> the DLL is still blocked by Windows. Run this once:")
                log('     Get-ChildItem "C:\\Program Files\\LibreHardwareMonitor" '
                    '-Recurse | Unblock-File')

    # -- internals ---------------------------------------------------------
    @staticmethod
    def _sensors(hw) -> list:
        out = list(hw.Sensors)
        for sub in hw.SubHardware:
            try:
                sub.Update()
            except Exception:
                pass
            out.extend(sub.Sensors)
        return out

    @staticmethod
    def _find(sensors, stype, *names):
        """First sensor of 'stype' whose name contains one of 'names'."""
        for s in sensors:
            if s.SensorType != stype or not s.Value:
                continue
            n = (s.Name or "").lower()
            for want in names:
                if want in n:
                    return float(s.Value)
        return None

    # -- collect ------------------------------------------------------------
    def apply(self, m: Metrics) -> None:
        if not self.ok:
            return
        HT, ST = self.HardwareType, self.SensorType

        for hw in self.computer.Hardware:
            try:
                hw.Update()
            except Exception:
                pass
            sensors = self._sensors(hw)
            ht = hw.HardwareType

            # ---- CPU ----------------------------------------------------
            if ht == HT.Cpu and m.cpu_temp_x10 == NA:
                v = self._find(sensors, ST.Temperature, "package", "core (tctl", "cpu")
                if v is not None:
                    m.cpu_temp_x10 = int(round(v * 10))
                v = self._find(sensors, ST.Power, "package", "cpu")
                if v is not None:
                    m.cpu_power_x10 = int(round(v * 10))

            # ---- GPU ----------------------------------------------------
            elif ht in (HT.GpuNvidia, HT.GpuAmd, HT.GpuIntel):
                v = self._find(sensors, ST.Load, "gpu core", "d3d 3d", "gpu 3d", "core")
                if v is not None:
                    m.gpu = clamp(v)

                v = self._find(sensors, ST.Temperature, "hot spot", "hotspot", "junction")
                if v is not None:
                    m.gpu_hot_x10 = int(round(v * 10))

                v = self._find(sensors, ST.Temperature, "memory", "vram", "hbm")
                if v is not None:
                    m.gpu_memtemp_x10 = int(round(v * 10))

                v = self._find(sensors, ST.Temperature, "gpu core", "core", "edge")
                if v is not None:
                    m.gpu_temp_x10 = int(round(v * 10))

                v = self._find(sensors, ST.Power, "package", "board", "gpu")
                if v is not None:
                    m.gpu_power_x10 = int(round(v * 10))

                v = self._find(sensors, ST.Control, "fan", "gpu")
                if v is None:
                    v = self._find(sensors, ST.Fan, "fan")          # RPM fallback
                if v is not None:
                    m.gpu_fan = clamp(v, 0, 100)

                used = self._find(sensors, ST.SmallData, "gpu memory used", "memory used")
                tot  = self._find(sensors, ST.SmallData, "gpu memory total", "memory total")
                if used is None:
                    u = self._find(sensors, ST.Data, "gpu memory used")
                    used = u * 1024.0 if u is not None else None
                if tot is None:
                    t = self._find(sensors, ST.Data, "gpu memory total")
                    tot = t * 1024.0 if t is not None else None
                if used is not None:
                    m.vram_used_mb = int(used)
                if tot is not None:
                    m.vram_total_mb = int(tot)
                if m.vram_used_mb != NA and m.vram_total_mb and m.vram_total_mb > 0:
                    m.vram = clamp(m.vram_used_mb * 100.0 / m.vram_total_mb)


# ---------------------------------------------------------------------------
# Windows PDH backend (no third-party dependency)
#   Fallback that still provides GPU utilisation + VRAM usage/limit when
#   LibreHardwareMonitor is not installed. Works with AMD / NVIDIA / Intel.
# ---------------------------------------------------------------------------
PDH_FMT_DOUBLE = 0x00000200


class _PdhFmtValue(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [
            ("longValue",       wt.LONG),
            ("doubleValue",     ctypes.c_double),
            ("largeValue",      ctypes.c_longlong),
            ("AnsiStringValue", ctypes.c_char_p),
            ("WideStringValue", ctypes.c_wchar_p),
        ]
    _anonymous_ = ("u",)
    _fields_ = [("CStatus", wt.DWORD), ("u", _U)]


class _PdhItem(ctypes.Structure):
    _fields_ = [("szName", ctypes.c_wchar_p), ("FmtValue", _PdhFmtValue)]


def _pdh_array(handle) -> list[tuple[str, float]]:
    """Read a wildcard PDH counter as a list of (instance, value)."""
    pdh = ctypes.windll.pdh
    size, count = wt.DWORD(0), wt.DWORD(0)
    pdh.PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE,
                                     ctypes.byref(size), ctypes.byref(count), None)
    if size.value == 0:
        return []
    buf = ctypes.create_string_buffer(size.value)
    rc = pdh.PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE,
                                          ctypes.byref(size), ctypes.byref(count), buf)
    if rc != 0:
        return []
    items = ctypes.cast(buf, ctypes.POINTER(_PdhItem))
    out = []
    for i in range(count.value):
        try:
            out.append((items[i].szName or "", float(items[i].FmtValue.doubleValue)))
        except Exception:
            pass
    return out


def _registry_vram_total_mb() -> int:
    """
    Dedicated VRAM size from the display-adapter driver key.
    'HardwareInformation.qwMemorySize' is the reliable, non-wrapping source
    of the real VRAM size on Windows (WMI AdapterRAM caps at 4 GB).
    """
    import winreg
    key_path = (r"SYSTEM\CurrentControlSet\Control\Class"
                r"\{4d36e968-e325-11ce-bfc1-08002be10318}")
    best = 0
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as root:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(root, i)
                except OSError:
                    break
                i += 1
                try:
                    with winreg.OpenKey(root, sub) as k:
                        val, _typ = winreg.QueryValueEx(
                            k, "HardwareInformation.qwMemorySize")
                        n = int.from_bytes(val, "little") if isinstance(val, bytes) else int(val)
                        best = max(best, n)
                except OSError:
                    continue
    except OSError:
        return 0
    return best // (1024 * 1024)


class PdhBackend:
    def __init__(self) -> None:
        self.ok       = False
        self.query    = wt.HANDLE()
        self.c_util   = None
        self.c_vused  = None
        self.c_vlimit = None
        self.vram_total_mb = _registry_vram_total_mb()
        try:
            pdh = ctypes.windll.pdh
            self._pdh = pdh
            if pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.query)) != 0:
                return

            def add(path: str):
                h  = wt.HANDLE()
                rc = pdh.PdhAddEnglishCounterW(self.query, path, 0, ctypes.byref(h))
                if rc != 0:
                    rc = pdh.PdhAddCounterW(self.query, path, 0, ctypes.byref(h))
                return h if rc == 0 else None

            self.c_util   = add(r"\GPU Engine(*)\Utilization Percentage")
            self.c_vused  = add(r"\GPU Adapter Memory(*)\Dedicated Usage")

            self.ok = any(c is not None for c in (self.c_util, self.c_vused))
            pdh.PdhCollectQueryData(self.query)       # prime the rate counters
            if self.ok:
                log(f"PDH GPU counters active (VRAM total from registry: "
                    f"{self.vram_total_mb} MB)")
        except Exception as e:
            log(f"PDH backend unavailable: {e}")

    def apply(self, m: Metrics) -> None:
        if not self.ok:
            return
        try:
            self._pdh.PdhCollectQueryData(self.query)
        except Exception:
            return

        # ---- GPU utilisation : busiest engine (matches Task Manager) ------
        if m.gpu == NA and self.c_util is not None:
            raw = _pdh_array(self.c_util)
            if raw:
                m.gpu = clamp(max(v for _, v in raw))

        # ---- VRAM : sum the dedicated usage of every adapter --------------
        if m.vram == NA and self.c_vused is not None:
            raw = _pdh_array(self.c_vused)
            if raw:
                used_bytes     = sum(v for _, v in raw)
                m.vram_used_mb = int(used_bytes / (1024 * 1024))
                total          = self.vram_total_mb
                if m.vram_total_mb == NA and total:
                    m.vram_total_mb = total
                if total and total > 0:
                    m.vram = clamp(m.vram_used_mb * 100.0 / total)



# ---------------------------------------------------------------------------
# Master volume - pycaw (Windows Core Audio)
# ---------------------------------------------------------------------------
# ---- Core Audio primitives -------------------------------------------------
# These all run on the volume worker thread (see VolumeBackend).
def _ca_get_volume(iface):
    return float(iface.GetMasterVolumeLevelScalar()) * 100.0


def _ca_get_mute(iface):
    return bool(iface.GetMute())


def _ca_set_volume(iface, percent):
    iface.SetMasterVolumeLevelScalar(max(0.0, min(1.0, float(percent) / 100.0)), None)
    return True


def _ca_set_mute(iface, on):
    iface.SetMute(1 if on else 0, None)
    return True


class VolumeBackend:
    """
    Windows master volume through pycaw.

    Every COM call happens on a dedicated worker thread, and that is on purpose:

    importing pythonnet (the LibreHardwareMonitor backend) initialises the MAIN
    thread as MTA. comtypes/pycaw need STA, and Windows refuses to switch an
    already-initialised thread, so on the main thread all Core Audio calls fail
    with RPC_E_CHANGED_MODE (0x80010106, "Cannot change thread mode after it is
    set"). A freshly created thread starts uninitialised, so COM there is always
    clean - and it also serialises the calls, which Core Audio prefers anyway.
    """

    def __init__(self) -> None:
        self.iface       = None
        self.errs        = 0
        self._lastErrLog = 0.0
        self._q          = queue.Queue()
        self._ready      = threading.Event()

        self._thread = threading.Thread(target=self._worker, name="volume-com",
                                        daemon=True)
        self._thread.start()
        if not self._ready.wait(4.0):
            log("volume: the COM worker thread did not start in time")

    # -- worker thread -------------------------------------------------------
    def _worker(self) -> None:
        try:
            import comtypes
            try:
                comtypes.CoInitialize()             # fresh thread -> STA
            except Exception:
                pass
            from ctypes import cast, POINTER
            from comtypes import CLSCTX_ALL
            from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

            dev   = AudioUtilities.GetSpeakers()
            iface = getattr(dev, "EndpointVolume", None)      # newer pycaw
            how   = "EndpointVolume property"
            if iface is None:                                  # older pycaw
                raw   = dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
                iface = cast(raw, POINTER(IAudioEndpointVolume))
                how   = "Activate()"
            self.iface = iface
            log(f"master volume control ready (pycaw via {how})")
        except Exception as e:
            log(f"volume control unavailable: {e!r}")
            log("  -> install the audio bindings:  pip install pycaw comtypes")
        finally:
            self._ready.set()

        while True:                                  # serve requests forever
            fn, args, box, done = self._q.get()
            try:
                box.append(fn(*args))
            except Exception as e:
                box.append(e)
            done.set()

    # -- plumbing ------------------------------------------------------------
    def _warn(self, what: str, e) -> None:
        self.errs += 1
        now = time.time()
        if now - self._lastErrLog >= 5.0:      # never spam the console
            self._lastErrLog = now
            log(f"{what} failed (#{self.errs}): {e!r}")

    def _call(self, fn, *args):
        """Run fn(iface, *args) on the COM thread; None if unavailable/failed."""
        if self.iface is None:
            return None
        box, done = [], threading.Event()
        self._q.put((fn, (self.iface,) + args, box, done))
        if not done.wait(1.0):
            self._warn(fn.__name__, "COM call timed out")
            return None
        r = box[0] if box else None
        if isinstance(r, BaseException):
            self._warn(fn.__name__, r)
            return None
        return r

    # -- operations (never raise; report through log) ------------------------
    def read(self):
        """Return (percent 0..100, muted) or (None, False)."""
        vol = self._call(_ca_get_volume)
        if vol is None:
            return None, False
        mute = self._call(_ca_get_mute)
        return clamp(vol), bool(mute)

    def write(self, percent: int) -> bool:
        return self._call(_ca_set_volume, int(percent)) is not None

    def step(self, delta: int) -> None:
        # use the mute flag from the same read (no extra COM round trip that
        # could throw and abort the write)
        cur, mute = self.read()
        if cur is not None:
            if delta > 0 and mute:
                self.set_mute(False)           # turning it up unmutes
            if self.write(clamp(cur + delta)):
                return

        # Last resort: the system volume keys always work, even when the Core
        # Audio interface cannot be opened (each tap is about 2 %).
        self._tap_keys(delta)

    @staticmethod
    def _tap_keys(delta: int) -> None:
        vk = VK_VOLUME_UP if delta > 0 else VK_VOLUME_DOWN
        for _ in range(min(abs(delta), 10)):
            _send_vk(vk)

    def set_mute(self, on: bool) -> None:
        self._call(_ca_set_mute, bool(on))


def volume_selftest() -> int:
    """`--selftest`: prove that master volume can be read *and* written."""
    log("master volume self-test ------------------------------")
    vb = VolumeBackend()
    v0, m0 = vb.read()
    log(f"read        : volume={v0} mute={m0}" + ("" if m0 else ""))

    if v0 is None:
        log("RESULT: FAILED - the master volume cannot be read.")
        log("        Try:  pip install pycaw comtypes")
        return 1

    target  = clamp(v0 + 15) if v0 <= 50 else clamp(v0 - 15)
    wrote   = vb.write(target)
    time.sleep(0.4)
    v1, _   = vb.read()
    log(f"write {target:3d} -> ok={wrote}, read back={v1}")

    vb.write(v0)                                        # restore
    time.sleep(0.4)
    v2, _ = vb.read()
    log(f"restore {v0:3d} -> read back={v2}")

    ok = v1 is not None and abs(v1 - target) <= 1 and v2 is not None and abs(v2 - v0) <= 1
    log("RESULT: " + ("volume read/write OK" if ok else
                      "volume read/write FAILED (see the errors above)"))
    return 0 if ok else 1



# ---------------------------------------------------------------------------
# Media keys - user32, no dependencies
# ---------------------------------------------------------------------------
class MediaBackend:
    def play_pause(self) -> None:
        _send_vk(VK_MEDIA_PLAY_PAUSE)

    def next(self) -> None:
        _send_vk(VK_MEDIA_NEXT_TRACK)

    def prev(self) -> None:
        _send_vk(VK_MEDIA_PREV_TRACK)

    def stop(self) -> None:
        _send_vk(VK_MEDIA_STOP)


# ---------------------------------------------------------------------------
# Now playing - best effort, no mandatory dependency
#   1. Windows SMTC through winsdk / winrt when one of them is importable
#   2. otherwise the title of a known media window (Spotify, browsers, VLC...)
# ---------------------------------------------------------------------------
import re

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def _window_title(hwnd) -> str:
    n = _user32.GetWindowTextLengthW(hwnd)
    if n <= 0:
        return ""
    buf = ctypes.create_unicode_buffer(n + 1)
    _user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def _window_pid(hwnd) -> int:
    pid = wt.DWORD(0)
    _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def _process_name(pid: int) -> str:
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)   # QUERY_LIMITED
    if not h:
        return ""
    try:
        size = wt.DWORD(300)
        buf  = ctypes.create_unicode_buffer(300)
        if ctypes.windll.kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return os.path.basename(buf.value).lower()
        return ""
    finally:
        ctypes.windll.kernel32.CloseHandle(h)


class NowPlayingBackend:
    BROWSERS = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe",
                "opera.exe", "vivaldi.exe", "thorium.exe", "librewolf.exe",
                "waterfox.exe", "yandex.exe", "arc.exe"}
    # applicationframehost.exe hosts UWP apps (Media Player, Films & TV, ...)
    PLAYERS  = {"spotify.exe", "foobar2000.exe", "vlc.exe", "musicbee.exe",
                "musicbee64.exe", "aimp.exe", "wmplayer.exe", "itunes.exe",
                "deezer.exe", "tidal.exe", "groove.exe", "zunemusic.exe",
                "applicationframehost.exe", "media player.exe",
                "microsoft.media.player.exe", "potplayer.exe",
                "potplayermini64.exe", "mpc-hc64.exe", "mpc-hc.exe",
                "mpc-be64.exe", "mplayerc.exe", "jetaudio.exe", "winamp.exe",
                "cider.exe", "strawberry.exe", "clementine.exe",
                "audacious.exe", "deadbeef.exe", "youtube-music-desktop.exe"}
    SITES    = ("youtube music", "youtube", "soundcloud", "spotify", "deezer",
                "bandcamp", "twitch", "apple music", "tidal", "qobuz")
    # windows that exist but never carry media information
    JUNK     = {"msctfime ui", "default ime", "program manager", "settings",
                "windows input experience", "task switching", "search",
                "start", "notification center", "widgets"}
    UWP_HOSTS = {"applicationframehost.exe"}

    def __init__(self, verbose: bool = False, extra_procs=None) -> None:
        self.kind  = "windows"
        self._loop = None
        self._smtc = None
        self._enabled = True
        self.verbose  = verbose
        self.procs    = self.PLAYERS | self.BROWSERS | \
                        {p.lower() for p in (extra_procs or [])}

        for mod in ("winsdk.windows.media.control", "winrt.windows.media.control"):
            try:
                import asyncio
                import importlib
                m = importlib.import_module(mod)
                Mgr = m.GlobalSystemMediaTransportControlsSessionManager
                self._loop = asyncio.new_event_loop()
                self._smtc = self._loop.run_until_complete(Mgr.request_async())
                if self._smtc is not None:
                    self.kind = "smtc"
                    log(f"now playing via Windows SMTC ({mod.split('.')[0]})")
                    return
            except Exception:
                continue
        log("now playing: using window titles (SMTC unavailable)")

    # -- SMTC ---------------------------------------------------------------
    def _poll_smtc(self) -> NowPlaying:
        sess = self._smtc.get_current_session()
        if sess is None:
            return NowPlaying(0, "", "")
        props = self._loop.run_until_complete(sess.try_get_media_properties_async())
        info  = sess.get_playback_info()
        state = 1 if int(info.playback_status) == 4 else 2      # 4 == PLAYING
        return NowPlaying(state, props.artist or "", props.title or "")

    # -- window titles ------------------------------------------------------
    def _enum_media_windows(self):
        found: list[tuple[str, str]] = []
        procs = self.procs

        def cb(hwnd, _):
            # NOTE: no IsWindowVisible() test - a player minimised to the tray
            # is "not visible" but still has a valid title.
            t = _window_title(hwnd)
            if not t or t.strip().lower() in self.JUNK:
                return True
            proc = _process_name(_window_pid(hwnd))
            if proc in procs:
                found.append((proc, t))
            return True

        _user32.EnumWindows(WNDENUMPROC(cb), 0)

        if self.verbose:
            if found:
                for p, t in found:
                    print(f"  [np] candidate {p}: {t!r}", flush=True)
            else:
                print("  [np] no media window matched (use --np-proc NAME)",
                      flush=True)
        return found

    def _parse(self, proc: str, title: str) -> NowPlaying:
        title = re.sub(r"^\(\d+\)\s*", "", title).strip()
        if proc in self.BROWSERS:
            low  = title.lower()
            song = ""
            for site in self.SITES:
                for sep in (" - ", " | ", " — ", " · "):
                    tag = sep + site
                    if low.endswith(tag):
                        song = title[: -len(tag)].strip()
                        break
                if not song:
                    idx = low.rfind(site)         # site may sit mid-title
                    if idx > 0:
                        song = title[:idx].rstrip(" -|—·").strip()
                if song:
                    break
            if not song:
                return NowPlaying(0, "", "")
            song = re.sub(r"^\(\d+\)\s*", "", song).strip()
            if not song:
                return NowPlaying(0, "", "")
            if " • " in song:
                a, _, t = song.partition(" • ")
                return NowPlaying(1, t.strip(), a.strip())
            return NowPlaying(1, "", song)
        for sep in (" - ", " – ", " — "):
            parts = title.split(sep)
            if len(parts) >= 2:
                return NowPlaying(1, parts[0].strip(), parts[1].strip())
        return NowPlaying(1, "", title)

    # -- public -------------------------------------------------------------
    def poll(self) -> NowPlaying:
        if not self._enabled:
            return NowPlaying(0, "", "")
        try:
            if self.kind == "smtc" and self._smtc is not None:
                return self._poll_smtc()

            # Score the candidates instead of taking the first one: browser
            # windows are full of helper windows with titles of their own.
            best, best_score = None, -1
            for proc, title in self._enum_media_windows():
                low = title.lower()
                if any(s in low for s in self.SITES):
                    score = 3                       # browser tab or player page
                elif proc in self.PLAYERS and proc not in self.UWP_HOSTS:
                    score = 2                       # a dedicated media player
                else:
                    score = 1                       # generic window
                if score <= best_score:
                    continue
                np = self._parse(proc, title)
                if np.title:
                    best_score, best = score, np
                    if score == 3:
                        break                       # nothing can beat this
            if best is not None:
                return best
        except Exception:
            pass
        return NowPlaying(0, "", "")


AUDIO_BANDS = 8          # must match config.h on the board


def _band_centres(fs: float, n: int = AUDIO_BANDS,
                  lo: float = 60.0, hi: float = 3500.0) -> list:
    """Log-spaced band centre frequencies, clamped below Nyquist."""
    if hi > fs * 0.45:
        hi = fs * 0.45
    k = (hi / lo) ** (1.0 / max(1, n - 1))
    return [lo * (k ** i) for i in range(n)]


class _Biquad:
    """RBJ band-pass biquad (constant peak gain) with an energy read-out."""

    __slots__ = ("b0", "b1", "b2", "a1", "a2", "x1", "x2", "y1", "y2")

    def __init__(self, f0: float, fs: float, q: float = 1.4) -> None:
        w0 = 2.0 * math.pi * f0 / fs
        alpha = math.sin(w0) / (2.0 * q)
        a0 = 1.0 + alpha
        self.b0 = alpha / a0
        self.b1 = 0.0
        self.b2 = -alpha / a0
        self.a1 = (-2.0 * math.cos(w0)) / a0
        self.a2 = (1.0 - alpha) / a0
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def energy(self, samples) -> float:
        """Mean-square of the band-passed signal over 'samples'."""
        b0, b1, b2, a1, a2 = self.b0, self.b1, self.b2, self.a1, self.a2
        x1, x2, y1, y2 = self.x1, self.x2, self.y1, self.y2
        acc = 0.0
        for x in samples:
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2 = x1
            x1 = x
            y2 = y1
            y1 = y
            acc += y * y
        self.x1, self.x2, self.y1, self.y2 = x1, x2, y1, y2
        return acc / max(1, len(samples))


# ---------------------------------------------------------------------------
# Audio reactive spectrum - WASAPI loopback of the default output device
#
#   pyaudiowpatch (a PyAudio fork with loopback support) captures whatever is
#   playing; a small biquad bank splits it into AUDIO_BANDS log-spaced bands
#   which are turned into 0..100 levels plus a beat pulse. Only eight small
#   numbers ever reach the board, so all of the DSP stays here.
# ---------------------------------------------------------------------------
class AudioBackend:
    DB_SPAN     = 42.0          # dynamic range spread across the 0..100 scale
    DB_CEIL_MIN = -24.0         # the AGC ceiling never falls below this
    DB_GATE     = -70.0         # below this the input counts as silence

    def __init__(self, gain_db: float = 0.0, verbose: bool = False) -> None:
        self.ok      = False
        self.note    = ""
        self.levels  = [0] * AUDIO_BANDS
        self.pulse   = 0
        self.gain_db = gain_db
        self.verbose = verbose
        self._lock   = threading.Lock()
        self._stop   = threading.Event()
        self._avg    = 0.0
        self._ceil   = -30.0        # adaptive AGC ceiling (dBFS)

        try:
            import pyaudiowpatch  # noqa: F401
        except Exception:
            self.note = "pyaudiowpatch not installed (pip install pyaudiowpatch)"
            log(f"audio: {self.note}")
            return

        threading.Thread(target=self._run, name="audio-loopback",
                         daemon=True).start()

    # -- capture thread -----------------------------------------------------
    def _run(self) -> None:
        import pyaudiowpatch as pyaudio

        pa = pyaudio.PyAudio()
        try:
            loop = pa.get_default_wasapi_loopback()
            rate = int(loop["defaultSampleRate"])
            ch   = max(1, int(loop["maxInputChannels"]))
            dec  = max(1, int(round(rate / 8000.0)))      # -> ~8 kHz analysis rate
            fs   = rate / dec
            frame = 1024

            filters = [_Biquad(f, fs) for f in _band_centres(fs)]
            stream  = pa.open(format=pyaudio.paFloat32, channels=ch, rate=rate,
                              input=True, input_device_index=loop["index"],
                              frames_per_buffer=frame)
            self.ok = True
            log(f"audio: loopback '{loop['name']}' @ {rate} Hz, {ch} ch, "
                f"{AUDIO_BANDS} bands, analysis {fs:.0f} Hz")
        except Exception as e:
            self.note = f"loopback unavailable: {e!r}"
            log(f"audio: {self.note}")
            try:
                pa.terminate()
            except Exception:
                pass
            return

        try:
            while not self._stop.is_set():
                raw = stream.read(frame, exception_on_overflow=False)
                n   = len(raw) // 4
                try:
                    vals = struct.unpack("<%df" % n, raw)
                except struct.error:
                    continue

                # downmix to mono (the loopback device can expose >2 channels)
                mono = [0.0] * (n // ch)
                for i in range(len(mono)):
                    b = i * ch
                    s = 0.0
                    for c in range(ch):
                        s += vals[b + c]
                    mono[i] = s / ch
                mono = mono[::dec]            # crude decimation, fine for a VU

                # per-band energy in dBFS
                dbs  = [10.0 * math.log10(f.energy(mono) + 1e-12) + self.gain_db
                        for f in filters]
                peak = max(dbs)

                if peak < self.DB_GATE:
                    # silence: do not amplify the noise floor
                    lv = [0] * AUDIO_BANDS
                    self._ceil = max(self.DB_GATE, self._ceil - 1.0)
                else:
                    # auto-gain: the ceiling jumps to a new peak instantly and
                    # falls back slowly, so the scale fits whatever the system
                    # volume happens to be
                    self._ceil = peak if peak > self._ceil else \
                                 max(self.DB_CEIL_MIN, self._ceil - 0.6)
                    floor = self._ceil - self.DB_SPAN
                    lv = [int(max(0.0, min(100.0,
                                           (d - floor) * 100.0 / self.DB_SPAN)))
                          for d in dbs]

                with self._lock:
                    for i, v in enumerate(lv):
                        cur = self.levels[i]
                        # fast attack, gentle release
                        self.levels[i] = v if v >= cur else max(v, cur - 6)
                    low = (lv[0] + lv[1]) / 2.0
                    self._avg = self._avg * 0.92 + low * 0.08
                    if low > self._avg * 1.30 and low > 25:
                        self.pulse = 100                     # beat!
                    else:
                        self.pulse = max(0, self.pulse - 12)
        except Exception as e:
            if not self._stop.is_set():
                self.note = f"capture stopped: {e!r}"
                log(f"audio: {self.note}")
        finally:
            for fn in (stream.stop_stream, stream.close):
                try:
                    fn()
                except Exception:
                    pass
            try:
                pa.terminate()
            except Exception:
                pass

    # -- public -------------------------------------------------------------
    def read(self):
        """Return (levels[AUDIO_BANDS], pulse) - zeros when unavailable."""
        if not self.ok:
            return [0] * AUDIO_BANDS, 0
        with self._lock:
            return list(self.levels), self.pulse

    def close(self) -> None:
        self._stop.set()


# ---------------------------------------------------------------------------
# No-op stubs so every backend can be disabled from the command line
# ---------------------------------------------------------------------------
class _NoSensors:
    def apply(self, m: Metrics) -> None:
        pass


class _NoVolume:
    def read(self):
        return None, False

    def write(self, percent: int) -> None:
        pass

    def step(self, delta: int) -> None:
        pass

    def set_mute(self, on: bool) -> None:
        pass


class _NoNowPlaying:
    def poll(self) -> NowPlaying:
        return NowPlaying(0, "", "")


class _NoAudio:
    ok = False

    def read(self):
        return [0] * AUDIO_BANDS, 0

    def close(self) -> None:
        pass


# ---------------------------------------------------------------------------
# Sensor hub - merges all metric backends into one Metrics snapshot
# ---------------------------------------------------------------------------
class SensorHub:
    def __init__(self, args) -> None:
        self.psutil = PsutilBackend()
        self.lhm    = _NoSensors() if args.no_lhm else LhmBackend(args.lhm_dll)
        self.pdh    = _NoSensors() if args.no_pdh else PdhBackend()
        self.psutil.prime()

    def poll(self) -> Metrics:
        m = Metrics()
        self.psutil.apply(m)
        self.lhm.apply(m)
        self.pdh.apply(m)
        return m


# ---------------------------------------------------------------------------
# Bridge - the main service loop
# ---------------------------------------------------------------------------
class Bridge:
    def __init__(self, args) -> None:
        self.args    = args
        self.link    = Link(args.port, args.baud, args.dry_run, args.verbose)
        self.sensors = SensorHub(args)
        self.volume  = _NoVolume()     if args.no_volume     else VolumeBackend()
        self.media   = MediaBackend()
        self.nowp    = _NoNowPlaying() if args.no_nowplaying else \
                       NowPlayingBackend(args.verbose, args.np_proc)
        self.audio   = _NoAudio() if args.no_audio else \
                       AudioBackend(args.audio_gain, args.verbose)

        self.metrics: Metrics = Metrics()
        self.page             = "-"
        self.last_vol         = None
        self.last_vol_tx      = 0.0
        self.last_np          = ""
        # how fast the board is fed; --rate overrides
        self.summary_hz       = max(0.2, float(args.rate))
        self.detail_hz        = max(0.2, float(args.rate))
        # after we set the volume ourselves, ignore lagging reads for a moment
        self.vol_hold_until   = 0.0
        # audio spectrum pacing
        self.audio_hz         = max(1.0, float(args.audio_rate))
        self.last_audio_line  = ""
        self.last_audio_tx    = 0.0

    # -- incoming commands --------------------------------------------------
    def handle(self, line: str) -> None:
        if self.args.verbose:
            print(f"  <- {line}", flush=True)

        head, _, arg = line.partition(",")
        head = head.strip().upper()
        arg  = arg.strip()

        if head == "H":
            if not self.link.board_hello:
                log("board handshake received")
            self.link.board_hello = True

        elif head == "VOL":
            try:
                self.volume.step(int(arg))
            except Exception as e:
                log(f"VOL handler failed: {e!r}")

        elif head == "VSET":
            try:
                self.volume.write(int(arg))
                self.vol_hold_until = time.time() + 0.8   # ignore lagging reads
            except Exception as e:
                log(f"VSET handler failed: {e!r}")

        elif head == "MUTE":
            self.volume.set_mute(arg == "1")

        elif head == "MK":
            a = arg.upper()
            if a == "PP":
                self.media.play_pause()
            elif a == "N":
                self.media.next()
            elif a == "P":
                self.media.prev()

        elif head == "REQ":
            # the board wants the detail payload - we stream it at a fixed
            # rate anyway, so answer immediately as well
            self.link.write(self.metrics.detail_line())

        elif head == "PG":
            self.page = arg

        # anything else (e.g. firmware boot logs) is ignored on purpose

    # -- main loop ----------------------------------------------------------
    def run(self) -> None:
        now      = time.time()
        next_sum = now
        next_det = now
        next_vol = now
        next_np  = now
        next_hel = now
        next_aud = now
        deadline = (now + self.args.run_seconds) if self.args.run_seconds else None

        self.link.write(f"H,{PROTO_VERSION}")
        log("bridge running (Ctrl+C to stop)")

        try:
            while True:
                now = time.time()
                if deadline is not None and now >= deadline:
                    log("run-seconds reached, exiting")
                    break

                # ---- serial input -----------------------------------------
                for line in self.link.read_lines():
                    self.handle(line)

                # ---- handshake --------------------------------------------
                if not self.link.board_hello and now >= next_hel:
                    self.link.write(f"H,{PROTO_VERSION}")
                    next_hel = now + 1.0 / HELLO_HZ

                # ---- metrics: summary -------------------------------------
                if now >= next_sum:
                    self.metrics = self.sensors.poll()
                    self.link.write(self.metrics.summary_line())
                    next_sum = now + 1.0 / self.summary_hz

                # ---- metrics: detail --------------------------------------
                if now >= next_det:
                    self.link.write(self.metrics.detail_line())
                    next_det = now + 1.0 / self.detail_hz

                # ---- volume (change driven + periodic resync) -------------
                if now >= next_vol:
                    v, mu = self.volume.read()
                    if v is not None:
                        # while our own set is settling, a read can still report
                        # the previous value - do not fight it
                        changed = ((v, mu) != self.last_vol) and (now >= self.vol_hold_until)
                        stale   = (now - self.last_vol_tx) >= VOLUME_RESYNC_S
                        if changed or stale:
                            self.last_vol    = (v, mu)
                            self.last_vol_tx = now
                            self.link.write(f"V,{v},{1 if mu else 0}")
                    next_vol = now + 1.0 / VOLUME_HZ

                # ---- now playing (change driven) --------------------------
                if now >= next_np:
                    line = self.nowp.poll().line()
                    if line != self.last_np:
                        self.last_np = line
                        self.link.write(line)
                    next_np = now + 1.0 / NOWPLAY_HZ

                # ---- audio spectrum (change driven + 1 s keepalive) -------
                if now >= next_aud:
                    levels, pulse = self.audio.read()
                    line = "A," + ",".join(str(v) for v in levels) + "," + str(pulse)
                    if (line != self.last_audio_line
                            or (now - self.last_audio_tx) >= 1.0):
                        self.last_audio_line = line
                        self.last_audio_tx   = now
                        self.link.write(line)
                    next_aud = now + 1.0 / self.audio_hz

                time.sleep(0.005)

        except KeyboardInterrupt:
            log("interrupted")
        finally:
            self.link.close()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def audio_selftest(seconds: float = 12.0, gain: float = 0.0) -> int:
    """`--audio-test`: print the live band levels so the capture can be checked."""
    log("audio self-test - play some music --------------------------------")
    ab = AudioBackend(gain, verbose=True)

    for _ in range(20):                      # wait for the capture thread
        if ab.ok or ab.note:
            break
        time.sleep(0.1)
    if not ab.ok:
        log(f"RESULT: FAILED - {ab.note or 'loopback did not start'}")
        log("  -> pip install pyaudiowpatch")
        return 1

    log("press Ctrl+C to stop early")
    end = time.time() + seconds
    try:
        while time.time() < end:
            lv, pu = ab.read()
            nums  = " ".join(f"{v:3d}" for v in lv)
            graph = "".join("#" if v > 66 else "+" if v > 33 else "." for v in lv)
            print(f"  [{nums}]  beat={pu:3d}  {graph}", flush=True)
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        ab.close()

    log("RESULT: capture finished - the levels should have moved with the music")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="PC side bridge for the ESP32-S3 UART Volume Controller")
    p.add_argument("--port", help="serial port, e.g. COM5 (default: autodetect)")
    p.add_argument("--baud", type=int, default=115200,
                   help="nominal baud (USB CDC ignores it) - default 115200")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    p.add_argument("--scan", action="store_true",
                   help="probe every serial port for the board and exit")

    p.add_argument("--lhm-dll", help="explicit path to LibreHardwareMonitorLib.dll")
    p.add_argument("--no-lhm", action="store_true", help="disable LibreHardwareMonitor")
    p.add_argument("--no-pdh", action="store_true", help="disable the PDH GPU fallback")
    p.add_argument("--no-volume", action="store_true", help="disable volume control")
    p.add_argument("--no-nowplaying", action="store_true", help="disable now playing")
    p.add_argument("--np-proc", action="append", default=[], metavar="NAME",
                   help="extra media process to watch, e.g. --np-proc foo.exe "
                        "(repeatable; use -v to see the candidates)")

    p.add_argument("--rate", type=float, default=SUMMARY_HZ,
                   help=f"metric updates per second sent to the board "
                        f"(default {SUMMARY_HZ:g})")
    p.add_argument("--no-audio", action="store_true",
                   help="disable the audio reactive spectrum")
    p.add_argument("--audio-rate", type=float, default=25.0,
                   help="spectrum updates per second sent to the board (default 25)")
    p.add_argument("--audio-gain", type=float, default=0.0, metavar="DB",
                   help="extra gain in dB applied to every band (default 0)")
    p.add_argument("--audio-test", action="store_true",
                   help="print the live band levels (no serial port) and exit")
    p.add_argument("--selftest", action="store_true",
                   help="test reading/writing the master volume and exit")
    p.add_argument("--dry-run", action="store_true",
                   help="do not open a serial port, just exercise the backends")
    p.add_argument("--run-seconds", type=float, default=0.0,
                   help="stop after N seconds (0 = forever, handy for testing)")
    p.add_argument("--verbose", "-v", action="store_true", help="log protocol traffic")
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    if args.list:
        ports = list_ports()
        if not ports:
            print("no serial ports found")
        for p in ports:
            vid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "----:----"
            print(f"{p.device:8}  {vid}  {p.description or ''}")
        return 0

    if args.selftest:
        return volume_selftest()

    if args.audio_test:
        return audio_selftest(12.0, args.audio_gain)

    if args.scan:
        log("probing serial ports for the board ...")
        found = scan_for_board(args.baud)
        if found:
            print(f"\nboard answered on {found}  ->  use --port {found}")
            return 0
        print("\nNo port answered.")
        print("The ESP32-S3 has two USB ports. Match the cable to 'USB CDC On Boot':")
        print("  Enabled   -> plug into the port labelled USB   (native USB-CDC)")
        print("  Disabled  -> plug into the port labelled UART  (CH34x/CP210x bridge)")
        print("Also check that 'USB CDC On Boot' is really set before uploading.")
        return 1

    Bridge(args).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())









