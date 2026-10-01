"""USB Display host for esp32MultiThing (App Drawer -> USB DISPLAY).

Captures one Windows monitor, streams it to the ESP32 as JPEG frames over the
USB serial port and turns the touch events coming back into mouse input on
that monitor.

Protocol (see the USB Display section of esp32MultiThing.ino):
  PC -> ESP32   b"FRM1" + uint32 little-endian length + baseline JPEG
                (length 0 = keepalive ping)
                b"FRM2" + uint32 LE length + uint16 LE x + uint16 LE y + JPEG:
                tile with only the changed area, drawn at x,y
                b"FRM3" + uint32 LE length + uint16 LE x, y, w, h + native data:
                lossless rect in the ILI9488 pixel format (see encode_native)
  ESP32 -> PC   one text line per event
                READY,<w>,<h>,<max>,<proto>  app open, wants a full frame
                                     (proto 2 = FRM2 tiles, 3 = FRM3 native)
                ACK                  message handled, send the next one
                REFRESH              resend a full frame
                ERR,<reason>,...     frame dropped (answered with a full frame)
                T,<x>,<y>,<D|M|U>    touch down / move / up in screen pixels
                BYE                  app closed
Every frame or ping waits for its ACK before the next one goes out, so the
ESP32 UART never overflows while it is decoding. After the first full frame
only the changed 16x16 blocks are sent, grouped into a few boxes; each box
goes out as native pixels or as JPEG, whichever reaches the screen sooner
(the PC does the work, the ESP32 mostly copies bytes to the display). While
the screen is idle, areas last sent as JPEG are resent losslessly.
"""

from __future__ import annotations

import argparse
import ctypes
import io
import logging
import re
import struct
import sys
import threading
import time
from ctypes import wintypes
from dataclasses import dataclass

import mss
import serial
from mss.exception import ScreenShotError
from PIL import Image, ImageChops
from serial.tools import list_ports

MAGIC = b"FRM1"
TILE_MAGIC = b"FRM2"
NATIVE_MAGIC = b"FRM3"
TILE = 16                      # JPEG block size with 4:2:0: tiles on this grid match the full frame
MAX_TILES = 8                  # more separate changes than this -> one box over the whole screen
FULL_FRAME_SHARE = 0.6         # changed area above this share of the screen -> same
# ESP32 drawing time per pixel, used to pick the faster format for each box
ESP_NATIVE_S_PER_PX = 1.0e-6   # byte copies + SPI burst
ESP_JPEG_S_PER_PX = 3.0e-6     # JPEG decode + SPI burst
SHARPEN_IDLE_S = 0.5           # screen unchanged this long -> resend JPEG areas losslessly
DEFAULT_BAUD = 921600          # USBD_BAUD in esp32MultiThing.ino
DEFAULT_PANEL = (480, 320)     # until the ESP32 reports its screen in READY
DEFAULT_MAX_BYTES = 48 * 1024  # USBD_FRAME_MAX, until READY reports the real limit
ACK_TIMEOUT_S = 2.0            # no ACK by then: the message is considered lost
MAX_LOST = 3                   # lost messages in a row -> wait for READY again
PING_INTERVAL_S = 1.0          # keepalive while the screen does not change
PROBE_INTERVAL_S = 2.0         # ping while the ESP32 app is not known to be open
RECONNECT_DELAY_S = 2.0
STATS_INTERVAL_S = 10.0
MIN_QUALITY = 20

# USB-serial bridges found on ESP32 boards, used for --port auto-detection
USB_SERIAL_VIDS = {0x10C4: "CP210x", 0x1A86: "CH34x", 0x0403: "FTDI", 0x303A: "Espressif"}

# Clockwise --rotate -> PIL transpose (PIL names its rotations counter-clockwise)
ROTATIONS = {
    0: None,
    90: Image.Transpose.ROTATE_270,
    180: Image.Transpose.ROTATE_180,
    270: Image.Transpose.ROTATE_90,
}

log = logging.getLogger("usb_display")

# mss 10.2 renamed the mss() factory to MSS (the old name still works but warns)
MSS = getattr(mss, "MSS", mss.mss)


# ======================================================================
# Windows mouse input: SendInput with absolute virtual-desktop coordinates
# ======================================================================

INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_VIRTUALDESK = 0x4000
MOUSEEVENTF_ABSOLUTE = 0x8000
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


class INPUT(ctypes.Structure):
    class _Union(ctypes.Union):
        # MOUSEINPUT is the largest member of the Win32 union, so sizes match
        _fields_ = [("mi", MOUSEINPUT)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _Union)]


user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wintypes.UINT
user32.GetSystemMetrics.argtypes = (ctypes.c_int,)
user32.GetSystemMetrics.restype = ctypes.c_int


def enable_dpi_awareness() -> None:
    """Physical pixels everywhere, so mss, GetSystemMetrics and SendInput agree."""
    try:
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):  # per-monitor v2
            return
    except AttributeError:
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # per-monitor
    except (AttributeError, OSError):
        user32.SetProcessDPIAware()


class Mouse:
    """Left button only. Touch events arrive from the serial reader thread."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._down = False

    @staticmethod
    def _send(flags: int, x: int = 0, y: int = 0) -> None:
        inp = INPUT(type=INPUT_MOUSE)
        if flags & MOUSEEVENTF_ABSOLUTE:
            vx = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
            vy = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
            vw = max(user32.GetSystemMetrics(SM_CXVIRTUALSCREEN), 2)
            vh = max(user32.GetSystemMetrics(SM_CYVIRTUALSCREEN), 2)
            inp.mi.dx = round((x - vx) * 65535 / (vw - 1))
            inp.mi.dy = round((y - vy) * 65535 / (vh - 1))
        inp.mi.dwFlags = flags
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

    def touch(self, x: int, y: int, state: str) -> None:
        move = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
        with self._lock:
            if state == "D":
                self._send(move | MOUSEEVENTF_LEFTDOWN, x, y)
                self._down = True
            elif state == "M":
                self._send(move, x, y)
            elif state == "U":
                self._send(move | (MOUSEEVENTF_LEFTUP if self._down else 0), x, y)
                self._down = False

    def release(self) -> None:
        """Let go of the button (link lost mid-drag); the cursor stays put."""
        with self._lock:
            if self._down:
                self._send(MOUSEEVENTF_LEFTUP)
                self._down = False


# ======================================================================
# Mouse cursor overlay: screen captures leave the cursor out
# ======================================================================

CURSOR_SHOWING = 0x0001
DI_NORMAL = 0x0003
BI_RGB = 0
DIB_RGB_COLORS = 0


class CURSORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("hCursor", wintypes.HANDLE),
        ("ptScreenPos", wintypes.POINT),
    ]


class ICONINFO(ctypes.Structure):
    _fields_ = [
        ("fIcon", wintypes.BOOL),
        ("xHotspot", wintypes.DWORD),
        ("yHotspot", wintypes.DWORD),
        ("hbmMask", wintypes.HBITMAP),
        ("hbmColor", wintypes.HBITMAP),
    ]


class BITMAP(ctypes.Structure):
    _fields_ = [
        ("bmType", wintypes.LONG),
        ("bmWidth", wintypes.LONG),
        ("bmHeight", wintypes.LONG),
        ("bmWidthBytes", wintypes.LONG),
        ("bmPlanes", wintypes.WORD),
        ("bmBitsPixel", wintypes.WORD),
        ("bmBits", ctypes.c_void_p),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32.GetCursorInfo.argtypes = (ctypes.POINTER(CURSORINFO),)
user32.GetCursorInfo.restype = wintypes.BOOL
user32.GetIconInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(ICONINFO))
user32.GetIconInfo.restype = wintypes.BOOL
user32.DrawIconEx.argtypes = (wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.HANDLE, ctypes.c_int,
                              ctypes.c_int, wintypes.UINT, wintypes.HBRUSH, wintypes.UINT)
user32.DrawIconEx.restype = wintypes.BOOL
gdi32.CreateCompatibleDC.argtypes = (wintypes.HDC,)
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateDIBSection.argtypes = (wintypes.HDC, ctypes.POINTER(BITMAPINFOHEADER), wintypes.UINT,
                                   ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD)
gdi32.CreateDIBSection.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = (wintypes.HDC, wintypes.HGDIOBJ)
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.DeleteObject.argtypes = (wintypes.HGDIOBJ,)
gdi32.DeleteObject.restype = wintypes.BOOL
gdi32.DeleteDC.argtypes = (wintypes.HDC,)
gdi32.DeleteDC.restype = wintypes.BOOL
gdi32.GetObjectW.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.POINTER(BITMAP))
gdi32.GetObjectW.restype = ctypes.c_int
gdi32.GdiFlush.argtypes = ()
gdi32.GdiFlush.restype = wintypes.BOOL


def draw_cursor(img: Image.Image, left: int, top: int) -> None:
    """Paint the mouse cursor onto img, a capture whose top-left corner is at
    left, top on the virtual desktop. DrawIconEx over the captured pixels
    renders it exactly like Windows does, inverting cursors (I-beam) included."""
    info = CURSORINFO(cbSize=ctypes.sizeof(CURSORINFO))
    if not user32.GetCursorInfo(ctypes.byref(info)) or not info.flags & CURSOR_SHOWING or not info.hCursor:
        return
    icon = ICONINFO()
    if not user32.GetIconInfo(info.hCursor, ctypes.byref(icon)):
        return
    try:
        bm = BITMAP()
        gdi32.GetObjectW(icon.hbmMask, ctypes.sizeof(BITMAP), ctypes.byref(bm))
        width = bm.bmWidth
        height = bm.bmHeight if icon.hbmColor else bm.bmHeight // 2  # monochrome: AND + XOR masks stacked
    finally:
        for handle in (icon.hbmMask, icon.hbmColor):
            if handle:
                gdi32.DeleteObject(handle)
    x = info.ptScreenPos.x - icon.xHotspot - left
    y = info.ptScreenPos.y - icon.yHotspot - top
    if width <= 0 or height <= 0 or x >= img.width or y >= img.height or x + width <= 0 or y + height <= 0:
        return  # cursor on another monitor
    patch = img.crop((x, y, x + width, y + height))  # parts off the image are clipped by paste()
    header = BITMAPINFOHEADER(biSize=ctypes.sizeof(BITMAPINFOHEADER), biWidth=width, biHeight=-height,
                              biPlanes=1, biBitCount=32, biCompression=BI_RGB)  # top-down rows
    bits = ctypes.c_void_p()
    dc = gdi32.CreateCompatibleDC(None)
    dib = gdi32.CreateDIBSection(dc, ctypes.byref(header), DIB_RGB_COLORS, ctypes.byref(bits), None, 0)
    if not dib:
        gdi32.DeleteDC(dc)
        return
    old = gdi32.SelectObject(dc, dib)
    try:
        ctypes.memmove(bits, patch.tobytes("raw", "BGRX"), width * height * 4)
        user32.DrawIconEx(dc, 0, 0, info.hCursor, width, height, 0, None, DI_NORMAL)
        gdi32.GdiFlush()
        patch = Image.frombytes("RGB", (width, height), ctypes.string_at(bits, width * height * 4),
                                "raw", "BGRX")
    finally:
        gdi32.SelectObject(dc, old)
        gdi32.DeleteObject(dib)
        gdi32.DeleteDC(dc)
    img.paste(patch, (x, y))


# ======================================================================
# Capture, encoding and touch mapping
# ======================================================================

@dataclass(frozen=True)
class Geometry:
    """Maps ESP32 screen pixels back to desktop pixels for the frame on screen."""

    monitor: tuple[int, int, int, int]  # left, top, width, height on the virtual desktop
    rotate: int
    image: tuple[int, int]              # frame size sent to the ESP32
    panel: tuple[int, int]              # ESP32 screen size

    def to_desktop(self, tx: int, ty: int) -> tuple[int, int]:
        iw, ih = self.image
        # The ESP32 centres frames that are smaller than its screen
        ix = min(max(tx - max(0, (self.panel[0] - iw) // 2), 0), iw - 1)
        iy = min(max(ty - max(0, (self.panel[1] - ih) // 2), 0), ih - 1)
        u, v = (ix + 0.5) / iw, (iy + 0.5) / ih
        # Undo the clockwise --rotate to get back to monitor coordinates
        if self.rotate == 90:
            u, v = v, 1.0 - u
        elif self.rotate == 180:
            u, v = 1.0 - u, 1.0 - v
        elif self.rotate == 270:
            u, v = 1.0 - v, u
        left, top, width, height = self.monitor
        return left + min(int(u * width), width - 1), top + min(int(v * height), height - 1)


def grab(sct, monitor: dict, rotate: int, size: tuple[int, int]) -> Image.Image:
    shot = sct.grab(monitor)
    img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
    draw_cursor(img, monitor["left"], monitor["top"])  # before scaling, so it scales with the screen
    if ROTATIONS[rotate] is not None:
        img = img.transpose(ROTATIONS[rotate])
    if img.size != size:
        img = img.resize(size, Image.Resampling.LANCZOS, reducing_gap=2.0)
    return img


def encode_jpeg(img: Image.Image, quality: int, max_bytes: int) -> tuple[bytes | None, int]:
    """Baseline 4:2:0 JPEG (TJpg_Decoder cannot do progressive); the quality
    is lowered until the frame fits in the ESP32 buffer."""
    q = quality
    while True:
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=q, subsampling=2, optimize=True, progressive=False)
        data = buf.getvalue()
        if len(data) <= max_bytes:
            return data, q
        if q <= MIN_QUALITY:
            return None, q
        q = max(MIN_QUALITY, q - 10)


def changed_tiles(old: Image.Image, new: Image.Image) -> list[tuple[int, int, int, int]]:
    """Boxes covering every pixel that differs between two frames of the same size."""
    # Non-zero wherever any channel differs
    return boxes_from_mask(ImageChops.difference(old, new).point(lambda v: 255 if v else 0).convert("L"))


def boxes_from_mask(mask: Image.Image) -> list[tuple[int, int, int, int]]:
    """Boxes (left, top, right, bottom) on the TILE grid covering every non-zero
    pixel of an "L" mask; [] when it is all zero."""
    width, height = mask.size
    boxes: list[list[int]] = []
    for top in range(0, height, TILE):
        bottom = min(top + TILE, height)
        found = mask.crop((0, top, width, bottom)).getbbox()
        if found is None:
            continue
        left = found[0] // TILE * TILE
        right = min(width, -(-found[2] // TILE) * TILE)
        last = boxes[-1] if boxes else None
        # Grow the box above when this band touches it and overlaps it horizontally
        if last and last[3] == top and left < last[2] and right > last[0]:
            last[0], last[2], last[3] = min(last[0], left), max(last[2], right), bottom
        else:
            boxes.append([left, top, right, bottom])
    return [(b[0], b[1], b[2], b[3]) for b in boxes]


# FRM3 ops: one header byte, n = low 6 bits + 1 (same format in esp32MultiThing.ino)
OP_LITERAL = 0x00  # n pixels follow, 3 bytes each
OP_REPEAT = 0x40   # previous pixel n more times
OP_ABOVE = 0x80    # n pixels copied from the row above
OP_CACHED = 0xC0   # one pixel from colour cache slot (low 6 bits)
_NONZERO = re.compile(rb"[^\x00]")


def encode_native(img: Image.Image, limit: int) -> bytes | None:
    """Lossless FRM3 payload for img, or None as soon as it would exceed limit
    bytes. Pixels are in the ILI9488 18-bit format (R, G, B bytes, top 6 bits),
    so the ESP32 only copies bytes; all the searching happens here."""
    width, height = img.size
    count = width * height
    data = img.point(lambda v: v & 0xFC).tobytes()
    flat = Image.frombytes("RGB", (count, 1), data)

    def same_as(shift: int) -> bytes:  # byte 0 where pixel i == pixel i - shift
        diff = ImageChops.difference(flat, ImageChops.offset(flat, shift, 0))
        return diff.point(lambda v: 255 if v else 0).convert("L").tobytes()

    prev, above = same_as(1), same_as(width)
    out = bytearray()
    cache = [0] * 64  # starts black, like the ESP32
    lit_start = lit_len = 0

    def flush_literals() -> None:
        nonlocal lit_start, lit_len
        while lit_len:
            n = min(64, lit_len)
            out.append(OP_LITERAL | (n - 1))
            out.extend(data[3 * lit_start: 3 * (lit_start + n)])
            lit_start += n
            lit_len -= n

    i = 0
    while i < count:
        repeat = copy = 0
        if i and not prev[i]:
            m = _NONZERO.search(prev, i)
            repeat = (m.start() if m else count) - i
        if i >= width and not above[i]:
            m = _NONZERO.search(above, i)
            copy = (m.start() if m else count) - i
        if repeat >= 2 or copy >= 2:
            flush_literals()
            n, op = (repeat, OP_REPEAT) if repeat >= copy else (copy, OP_ABOVE)
            i += n
            while n:
                k = min(64, n)
                out.append(op | (k - 1))
                n -= k
        else:
            r, g, b = data[3 * i], data[3 * i + 1], data[3 * i + 2]
            slot = ((r >> 2) * 3 + (g >> 2) * 5 + (b >> 2) * 7) & 63
            colour = r << 16 | g << 8 | b
            if cache[slot] == colour:
                flush_literals()
                out.append(OP_CACHED | slot)
            else:
                cache[slot] = colour
                if not lit_len:
                    lit_start = i
                lit_len += 1
            i += 1
        if len(out) + 3 * lit_len > limit:
            return None
    flush_literals()
    return bytes(out)


def native_messages(img: Image.Image, x: int, y: int, max_bytes: int,
                    limit: int) -> list[tuple[bytes, bytes, tuple[int, ...]]] | None:
    """FRM3 messages for img drawn at x,y, split into bands that fit the ESP32
    buffer; None if they would total more than limit bytes."""
    data = encode_native(img, limit)
    if data is None:
        return None
    if len(data) <= max_bytes:
        return [(NATIVE_MAGIC, data, (x, y, img.width, img.height))]
    if img.height < 2:
        return None
    half = img.height // 2
    top = native_messages(img.crop((0, 0, img.width, half)), x, y, max_bytes, limit)
    if top is None:
        return None
    rest = native_messages(img.crop((0, half, img.width, img.height)), x, y + half, max_bytes,
                           limit - sum(len(m[1]) for m in top))
    return None if rest is None else top + rest


def encode_box(img: Image.Image, box: tuple[int, int, int, int], native: bool, quality: int,
               max_bytes: int, byte_s: float,
               force_native: bool = False) -> tuple[list[tuple[bytes, bytes, tuple[int, ...]]], bool, int]:
    """Messages that redraw box, in the format that is on screen sooner (time
    on the wire + drawing time on the ESP32). Returns (messages, lossy,
    quality); no messages if nothing fits the ESP32 buffer."""
    part = img.crop(box)
    pixels = part.width * part.height
    jpg, quality = (None, quality) if force_native else encode_jpeg(part, quality, max_bytes)
    if native:
        # Native wins while its extra bytes cost less than the JPEG decode saves
        limit = 1 << 30 if jpg is None else int(
            len(jpg) + pixels * (ESP_JPEG_S_PER_PX - ESP_NATIVE_S_PER_PX) / byte_s)
        msgs = native_messages(part, box[0], box[1], max_bytes, limit)
        if msgs:
            return msgs, False, quality
    if jpg is None:
        return [], True, quality
    return [(TILE_MAGIC, jpg, (box[0], box[1]))], True, quality


# ======================================================================
# Serial link
# ======================================================================

class Link:
    """One open serial port: framing, ACK flow control and a reader thread
    that dispatches the lines sent by the ESP32."""

    def __init__(self, ser: serial.SerialBase, mouse: Mouse) -> None:
        self.ser = ser
        self.mouse = mouse
        self.alive = True
        self.ready = False                # USB DISPLAY app open on the ESP32
        self.tiles = False                # ESP32 accepts FRM2 tiles (READY protocol >= 2)
        self.native = False               # ESP32 accepts FRM3 native rects (protocol >= 3)
        self.panel = DEFAULT_PANEL
        self.max_bytes = DEFAULT_MAX_BYTES
        self.geometry: Geometry | None = None
        self.lost = 0                     # messages in a row without ACK
        self._credit = threading.Event()  # set = the next message may be sent
        self._credit.set()
        self._lock = threading.Lock()
        self._refresh = False
        self._reader = threading.Thread(target=self._read_loop, name="serial-reader", daemon=True)
        self._reader.start()

    # --- main thread ---

    def send(self, payload: bytes = b"", magic: bytes = MAGIC, *rect: int) -> None:
        """Send a frame (or a ping when payload is empty), an FRM2 tile at x,y
        or an FRM3 rect x,y,w,h. Uses up the credit."""
        self._credit.clear()
        self.ser.write(magic + struct.pack("<I%dH" % len(rect), len(payload), *rect) + payload)

    def post(self, payload: bytes, magic: bytes = MAGIC, *rect: int) -> bool:
        """send() once the previous message is acknowledged; False if the ESP32
        went away meanwhile."""
        self.wait_credit()
        if not self.alive or not self.ready:
            return False
        self.send(payload, magic, *rect)
        return True

    def wait_credit(self) -> None:
        if self._credit.wait(ACK_TIMEOUT_S):
            return
        self.lost += 1
        if self.ready and self.lost >= MAX_LOST:
            log.warning("ESP32 stopped answering, waiting for READY")
            self.ready = False
            self.mouse.release()
        self._credit.set()  # give up on that message

    def take_refresh(self) -> bool:
        with self._lock:
            flag, self._refresh = self._refresh, False
        return flag

    # --- reader thread ---

    def _request_refresh(self) -> None:
        with self._lock:
            self._refresh = True

    def _read_loop(self) -> None:
        pending = bytearray()
        while self.alive:
            try:
                chunk = self.ser.read(max(1, self.ser.in_waiting))
            except (serial.SerialException, OSError) as exc:
                if self.alive:
                    log.warning("serial read failed: %s", exc)
                break
            if not chunk:
                continue
            pending += chunk
            while (end := pending.find(b"\n")) >= 0:
                line = pending[:end].decode("ascii", "replace").strip()
                del pending[: end + 1]
                if line:
                    self._handle(line)
            if len(pending) > 1024:  # no newline: sketch output at another baud rate
                pending.clear()
        self.alive = False
        self._credit.set()  # wake the main loop so it notices

    def _handle(self, line: str) -> None:
        kind, *fields = line.split(",")
        if kind == "T" and len(fields) == 3 and fields[2] in ("D", "M", "U"):
            geo = self.geometry
            if geo is None:
                return
            try:
                x, y = geo.to_desktop(int(fields[0]), int(fields[1]))
            except ValueError:
                return
            self.mouse.touch(x, y, fields[2])
        elif kind == "ACK":
            self.lost = 0
            if not self.ready:  # answer to a probe: the app was already open
                log.info("ESP32 USB DISPLAY app is open")
                self.ready = True
                # Assume current firmware; with an older one the tiles get no ACK,
                # the app falls back to its waiting screen and READY corrects this
                self.tiles = self.native = True
                self._request_refresh()
            self._credit.set()
        elif kind == "READY" and len(fields) in (3, 4):
            try:
                width, height, max_bytes = (int(f) for f in fields[:3])
                proto = int(fields[3]) if len(fields) > 3 else 1
            except ValueError:
                return
            if not self.ready or (width, height) != self.panel or max_bytes != self.max_bytes:
                log.info("ESP32 ready: screen %dx%d, frames up to %d KB%s", width, height, max_bytes // 1024,
                         ", native pixels" if proto >= 3 else ", partial updates" if proto >= 2 else "")
            self.panel, self.max_bytes = (width, height), max_bytes
            self.tiles = proto >= 2
            self.native = proto >= 3
            self.mouse.release()
            self.ready = True
            self.lost = 0
            self._request_refresh()
            self._credit.set()
        elif kind == "REFRESH":
            self._request_refresh()
        elif kind == "ERR":
            log.warning("ESP32 dropped a frame: %s", line)
            if len(fields) >= 3 and fields[0] == "SIZE" and fields[2].isdigit():
                self.max_bytes = int(fields[2])
            self._request_refresh()
        elif kind == "BYE":
            log.info("ESP32 USB DISPLAY app closed")
            self.ready = False
            self.mouse.release()
        # Anything else is sketch output from outside the app: ignored


def plan_update(link: Link, last: Image.Image | None,
                img: Image.Image) -> list[tuple[int, int, int, int]] | None:
    """What to send for img: None = full JPEG frame, [] = nothing changed,
    else the boxes to redraw."""
    if last is None or not link.tiles or img.size != link.panel or last.size != img.size:
        return None
    boxes = changed_tiles(last, img)
    area = sum((r - l) * (b - t) for l, t, r, b in boxes)
    if len(boxes) > MAX_TILES or area > FULL_FRAME_SHARE * img.width * img.height:
        return [(0, 0, img.width, img.height)]
    return boxes


def run_session(ser: serial.SerialBase, args: argparse.Namespace, mouse: Mouse, monitor: dict) -> None:
    """Stream until the serial port goes away."""
    link = Link(ser, mouse)
    mon_rect = (monitor["left"], monitor["top"], monitor["width"], monitor["height"])
    period = 1.0 / args.fps
    last_img = None  # what the ESP32 shows; None = unknown, send a full frame
    lossy = None     # "L" mask of the screen areas last drawn from JPEG
    changed_at = 0.0
    byte_s = 10.0 / args.baud  # time on the wire per byte (8N1)
    last_send = 0.0
    capture_failed = False
    stats_at, frames, native_bytes, sent_bytes, quality = time.monotonic(), 0, 0, 0, args.quality
    try:
        with MSS() as sct:
            while link.alive:
                tick = time.monotonic()
                link.wait_credit()
                if not link.alive:
                    break
                now = time.monotonic()

                if not link.ready:
                    # Probe: an ACK means the app was already open (script restarted)
                    if now - last_send >= PROBE_INTERVAL_S:
                        link.send()
                        last_send = now
                    else:
                        time.sleep(0.05)
                    continue

                sent = False
                size = (args.width or link.panel[0], args.height or link.panel[1])
                try:
                    img = grab(sct, monitor, args.rotate, size)
                    capture_failed = False
                except ScreenShotError as exc:  # lock screen, UAC prompt, monitor gone
                    if not capture_failed:
                        log.warning("capture failed: %s", exc)
                    capture_failed = True
                    img = None
                if img is not None:
                    if link.take_refresh():
                        last_img = None
                    boxes = plan_update(link, last_img, img)
                    sharpen = False
                    if boxes:
                        changed_at = now
                    elif (boxes == [] and link.native and lossy is not None and lossy.getbbox()
                          and now - changed_at >= SHARPEN_IDLE_S):
                        # Idle: redraw one area that came from JPEG, losslessly this time
                        boxes, sharpen = boxes_from_mask(lossy)[:1], True
                    posted = 0
                    complete = True
                    if boxes is None:  # full frame (the ESP32 may show its waiting screen)
                        data, quality = encode_jpeg(img, args.quality, link.max_bytes)
                        link.geometry = Geometry(mon_rect, args.rotate, img.size, link.panel)
                        if data is None:
                            log.warning("frame over %d KB even at quality %d, skipped",
                                        link.max_bytes // 1024, MIN_QUALITY)
                            complete = False
                        elif link.post(data):
                            posted += 1
                            sent_bytes += len(data)
                            lossy = Image.new("L", img.size, 255)
                            changed_at = now
                        else:
                            complete = False
                    else:
                        for box in boxes:
                            msgs, is_lossy, quality = encode_box(img, box, link.native, args.quality,
                                                                 link.max_bytes, byte_s, sharpen)
                            for magic, payload, rect in msgs:
                                if not link.post(payload, magic, *rect):
                                    complete = False
                                    break
                                posted += 1
                                sent_bytes += len(payload)
                                if not is_lossy:
                                    native_bytes += len(payload)
                            if not msgs or not complete:
                                complete = False
                                break
                            lossy.paste(255 if is_lossy else 0, box)
                    # Only a complete update leaves the ESP32 showing img
                    last_img = img if complete else None
                    if posted:
                        sent = True
                        frames += 1
                if not sent and now - last_send >= PING_INTERVAL_S:
                    link.send()
                    sent = True
                if sent:
                    last_send = now

                if now - stats_at >= STATS_INTERVAL_S:
                    if frames:
                        log.info("%.1f updates/s, %.1f KB per update, %d%% native pixels, JPEG quality %d",
                                 frames / (now - stats_at), sent_bytes / frames / 1024,
                                 100 * native_bytes // max(1, sent_bytes), quality)
                    stats_at, frames, native_bytes, sent_bytes = now, 0, 0, 0

                delay = period - (time.monotonic() - tick)
                if delay > 0:
                    time.sleep(delay)
    finally:
        link.alive = False
        mouse.release()


# ======================================================================
# Setup
# ======================================================================

def list_devices() -> None:
    with MSS() as sct:
        print("Monitors (--monitor):")
        for i, m in enumerate(sct.monitors[1:], start=1):
            print(f"  {i}: {m['width']}x{m['height']} at ({m['left']}, {m['top']})")
    print("Serial ports (--port):")
    for p in list_ports.comports():
        bridge = USB_SERIAL_VIDS.get(p.vid or 0, "")
        print(f"  {p.device}: {p.description}" + (f"  [{bridge}]" if bridge else ""))


def pick_monitor(sct, index: int | None, target: tuple[int, int]) -> dict:
    monitors = sct.monitors  # [0] = all monitors together, [1:] = each monitor
    if index is not None:
        if 1 <= index < len(monitors):
            return monitors[index]
        raise ValueError(f"monitor {index} does not exist (1..{len(monitors) - 1}, see --list)")
    matches = [m for m in monitors[1:] if (m["width"], m["height"]) == target]
    if len(matches) == 1:
        return matches[0]
    raise ValueError(f"no single {target[0]}x{target[1]} monitor found, "
                     "choose one with --monitor (see --list)")


def find_port() -> str | None:
    """The one plugged-in USB-serial bridge of a type used by ESP32 boards."""
    ports = [p.device for p in list_ports.comports() if p.vid in USB_SERIAL_VIDS]
    if len(ports) > 1:
        raise ValueError(f"several serial ports found ({', '.join(ports)}), choose one with --port")
    return ports[0] if ports else None


def open_port(port: str, baud: int) -> serial.SerialBase:
    ser = serial.serial_for_url(port, baudrate=baud, timeout=0.1, write_timeout=10, do_not_open=True)
    # Keep DTR/RTS released: toggling them resets the ESP32 through its
    # auto-program circuit (the Arduino IDE monitor does the same)
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Use the esp32MultiThing TFT as a small monitor (USB DISPLAY app).")
    p.add_argument("--port", help="serial port, e.g. COM3 (default: auto-detect)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help=f"serial speed, must match USBD_BAUD in the sketch (default {DEFAULT_BAUD})")
    p.add_argument("--monitor", type=int,
                   help="monitor index from --list (default: the one with the ESP32 resolution)")
    p.add_argument("--width", type=int, help="frame width (default: ESP32 screen width)")
    p.add_argument("--height", type=int, help="frame height (default: ESP32 screen height)")
    p.add_argument("--fps", type=float, default=5.0, help="max frames per second (default 5)")
    p.add_argument("--quality", type=int, default=70, help="JPEG quality 20-95 (default 70)")
    p.add_argument("--rotate", type=int, default=0, choices=(0, 90, 180, 270),
                   help="rotate the capture clockwise before sending (default 0)")
    p.add_argument("--list", action="store_true", help="list monitors and serial ports, then exit")
    args = p.parse_args()
    if args.fps <= 0:
        p.error("--fps must be greater than 0")
    if not MIN_QUALITY <= args.quality <= 95:
        p.error(f"--quality must be between {MIN_QUALITY} and 95")
    if (args.width is None) != (args.height is None):
        p.error("--width and --height must be given together")
    if args.width is not None and (args.width < 1 or args.height < 1):
        p.error("--width and --height must be positive")
    return args


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    args = parse_args()
    enable_dpi_awareness()
    if args.list:
        list_devices()
        return 0

    width, height = args.width or DEFAULT_PANEL[0], args.height or DEFAULT_PANEL[1]
    target = (height, width) if args.rotate in (90, 270) else (width, height)
    try:
        with MSS() as sct:
            monitor = pick_monitor(sct, args.monitor, target)
    except ValueError as exc:
        log.error("%s", exc)
        return 2
    log.info("capturing monitor %dx%d at (%d, %d)",
             monitor["width"], monitor["height"], monitor["left"], monitor["top"])

    mouse = Mouse()
    last_problem = None
    try:
        while True:
            try:
                port = args.port or find_port()
            except ValueError as exc:
                log.error("%s", exc)
                return 2
            if port is None:
                if last_problem != "no port":
                    log.info("waiting for the ESP32 serial port...")
                last_problem = "no port"
                time.sleep(RECONNECT_DELAY_S)
                continue
            try:
                ser = open_port(port, args.baud)
            except (serial.SerialException, ValueError) as exc:
                if str(exc) != last_problem:  # busy (Arduino Serial Monitor?) or unplugged
                    log.warning("cannot open %s: %s", port, exc)
                last_problem = str(exc)
                time.sleep(RECONNECT_DELAY_S)
                continue
            last_problem = None
            log.info("connected to %s at %d baud; open USB DISPLAY on the ESP32", port, args.baud)
            try:
                run_session(ser, args, mouse, monitor)
            except (serial.SerialException, OSError) as exc:
                log.warning("connection lost: %s", exc)
            finally:
                try:
                    ser.close()
                except (serial.SerialException, OSError):
                    pass
            time.sleep(RECONNECT_DELAY_S)
    except KeyboardInterrupt:
        log.info("stopped")
        return 0
    finally:
        mouse.release()


if __name__ == "__main__":
    sys.exit(main())
