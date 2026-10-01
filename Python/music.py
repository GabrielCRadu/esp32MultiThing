from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import Response
import asyncio
import ctypes
import json
import os
import time
import socket
import ssl
import struct
import threading
import traceback
import logging
import urllib.request
from typing import Dict, Any, Tuple

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

try:
    import psutil
    _PSUTIL_OK = True
except ImportError:
    _PSUTIL_OK = False

from winsdk.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
from winsdk.windows.storage.streams import DataReader, Buffer, InputStreamOptions

import uvicorn
from PIL import Image
import io
import colorsys


app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ======================================================================
# Chrome extension bridge
# The extension content script POSTs { position, duration, playing }
# every ~1 s.  We use this as ground-truth position and interpolate
# forward between updates.  SMTC is still used for title/artist/album.
# Data is considered stale after EXT_STALE_S seconds of silence.
# ======================================================================

EXT_STALE_S = 3.0   # seconds before we stop trusting ext data

_ext_lock = asyncio.Lock()
_ext: Dict[str, Any] = {
    "base_pos":  0,      # video.currentTime when last update arrived
    "base_ts":   0.0,    # time.monotonic() of that update
    "duration":  0,
    "playing":   False,
    "last_ts":   0.0,    # 0 means never received
}


@app.post("/extension/update")
async def extension_update(payload: dict):
    """Receives position data from the Chrome extension content script."""
    async with _ext_lock:
        _ext["base_pos"] = int(payload.get("position", 0))
        _ext["base_ts"]  = time.monotonic()
        _ext["duration"] = int(payload.get("duration", 0))
        _ext["playing"]  = bool(payload.get("playing", False))
        _ext["last_ts"]  = _ext["base_ts"]
    return {"ok": True}


def _ext_position(ext_snapshot: dict) -> int:
    """Interpolate forward from the last known browser position."""
    if ext_snapshot["playing"]:
        elapsed = max(0.0, time.monotonic() - ext_snapshot["base_ts"])
        pos = ext_snapshot["base_pos"] + elapsed
        dur = ext_snapshot["duration"]
        if dur > 0:
            pos = min(pos, float(dur))
        return int(pos)
    return int(ext_snapshot["base_pos"])


def _ext_is_fresh(ext_snapshot: dict) -> bool:
    if ext_snapshot["last_ts"] == 0.0:
        return False
    if ext_snapshot["duration"] == 0:  # reset signal sent on navigation
        return False
    return (time.monotonic() - ext_snapshot["last_ts"]) < EXT_STALE_S


# ======================================================================
# SMTC helpers (used for title / artist / album metadata)
# ======================================================================

_smtc_lock = asyncio.Lock()
_smtc: Dict[str, Any] = {
    "key":          None,
    "base_pos":     0.0,
    "base_ts":      0.0,
    "base_rate":    1.0,
    "last_status":  None,
    "last_raw_pos": None,
}


def _safe_total_seconds(td) -> float:
    try:
        return float(td.total_seconds())
    except Exception:
        return 0.0


def _get_playback_rate(playback) -> float:
    for attr in ("playback_rate", "playbackrate", "rate"):
        try:
            v = getattr(playback, attr, None)
            if v is not None:
                v = float(v)
                if v > 0:
                    return v
        except Exception:
            pass
    return 1.0


def _is_playing(playback) -> bool:
    try:
        return playback.playback_status == PlaybackStatus.PLAYING
    except Exception:
        pass
    try:
        s = str(getattr(playback, "playback_status", "") or "")
        return "playing" in s.lower()
    except Exception:
        return False


def _get_status_str(playback) -> str:
    try:
        status = getattr(playback, "playback_status", None)
        if status == PlaybackStatus.PLAYING:
            return "Playing"
        if status == PlaybackStatus.PAUSED:
            return "Paused"
        if status is not None:
            return "Stopped"
        return "Unknown"
    except Exception:
        return "Unknown"


def _make_track_key(session, info, duration_s: int) -> Tuple[str, str, str, int]:
    try:
        source_app = str(getattr(session, "source_app_user_model_id", "") or "")
    except Exception:
        source_app = ""
    title  = (getattr(info, "title",  None) or "Unknown").strip()
    artist = (getattr(info, "artist", None) or "Unknown").strip()
    return (source_app, title, artist, int(duration_s))


async def _get_sessions_manager():
    try:
        return await MediaManager.request_async()
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error getting session manager: {str(e)}")


async def get_current_session():
    sessions = await _get_sessions_manager()
    try:
        current_session = sessions.get_current_session()
        if not current_session:
            raise HTTPException(status_code=404, detail="No active media session found")
        return current_session
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error getting session: {str(e)}")


# ======================================================================
# Core media info — merges SMTC metadata with extension position
# ======================================================================

async def get_media_info(include_debug: bool = False):
    # Snapshot extension state under lock (non-blocking read)
    async with _ext_lock:
        ext = dict(_ext)

    ext_fresh = _ext_is_fresh(ext)

    try:
        sessions = await _get_sessions_manager()
        current_session = sessions.get_current_session()

        if not current_session:
            # No SMTC session — return extension-only data if available
            if ext_fresh:
                pos = _ext_position(ext)
                return {
                    "playing":  ext["playing"],
                    "title":    "Unknown",
                    "artist":   "Unknown",
                    "album":    "Unknown",
                    "duration": ext["duration"],
                    "position": pos,
                    "status":   "Playing" if ext["playing"] else "Paused",
                    **({"debug": {"source": "extension_only"}} if include_debug else {}),
                }
            return {"playing": False}

        info     = await current_session.try_get_media_properties_async()
        timeline = current_session.get_timeline_properties()
        playback = current_session.get_playback_info()

        duration_s  = int(_safe_total_seconds(getattr(timeline, "end_time", None)))
        raw_pos_s   = float(_safe_total_seconds(getattr(timeline, "position", None)))
        status_str  = _get_status_str(playback)
        playing_now = _is_playing(playback)
        rate        = _get_playback_rate(playback)
        track_key   = _make_track_key(current_session, info, duration_s)
        now         = time.monotonic()

        # ---- SMTC position estimation (fallback when ext is stale) ----
        async with _smtc_lock:
            key_changed = (_smtc["key"] != track_key)
            if key_changed:
                _smtc["key"]          = track_key
                _smtc["base_pos"]     = raw_pos_s
                _smtc["base_ts"]      = now
                _smtc["base_rate"]    = rate
                _smtc["last_status"]  = getattr(playback, "playback_status", None)
                _smtc["last_raw_pos"] = raw_pos_s
            else:
                last_raw   = _smtc["last_raw_pos"]
                cur_status = getattr(playback, "playback_status", None)

                if last_raw is not None and abs(raw_pos_s - float(last_raw)) >= 1.0:
                    _smtc["base_pos"]     = raw_pos_s
                    _smtc["base_ts"]      = now
                    _smtc["base_rate"]    = rate
                    _smtc["last_raw_pos"] = raw_pos_s

                if _smtc["last_status"] != cur_status:
                    _smtc["base_pos"]    = raw_pos_s
                    _smtc["base_ts"]     = now
                    _smtc["base_rate"]   = rate
                    _smtc["last_status"] = cur_status
                    _smtc["last_raw_pos"] = raw_pos_s

            if playing_now:
                elapsed  = max(0.0, now - float(_smtc["base_ts"]))
                smtc_pos = float(_smtc["base_pos"]) + elapsed * float(_smtc["base_rate"] or 1.0)
            else:
                smtc_pos = float(_smtc["base_pos"])

        if duration_s > 0:
            smtc_pos = max(0.0, min(smtc_pos, float(duration_s)))
        else:
            smtc_pos = max(0.0, smtc_pos)

        # ---- Merge: use extension position if fresh, SMTC otherwise ----
        if ext_fresh:
            pos_used   = _ext_position(ext)
            dur_used   = ext["duration"] if ext["duration"] > 0 else duration_s
            play_used  = ext["playing"]
            stat_used  = "Playing" if ext["playing"] else status_str
            pos_source = "extension"
        else:
            pos_used   = int(smtc_pos)
            dur_used   = duration_s
            play_used  = playing_now
            stat_used  = status_str
            pos_source = "smtc"

        payload = {
            "playing":  True,
            "title":    getattr(info, "title",       None) or "Unknown",
            "artist":   getattr(info, "artist",      None) or "Unknown",
            "album":    getattr(info, "album_title", None) or "Unknown",
            "duration": int(dur_used),
            "position": int(pos_used),
            "status":   stat_used,
        }

        if include_debug:
            payload["debug"] = {
                "pos_source":      pos_source,
                "ext_fresh":       ext_fresh,
                "ext_pos":         _ext_position(ext) if ext_fresh else None,
                "ext_age_s":       round(time.monotonic() - ext["last_ts"], 2) if ext["last_ts"] else None,
                "smtc_pos":        int(smtc_pos),
                "smtc_raw_pos":    raw_pos_s,
                "smtc_playing":    playing_now,
                "smtc_status":     status_str,
                "smtc_rate":       rate,
                "track_key":       track_key,
            }

        return payload

    except Exception as e:
        if ext_fresh:
            pos = _ext_position(ext)
            return {
                "playing":  ext["playing"],
                "title":    "Unknown",
                "artist":   "Unknown",
                "album":    "Unknown",
                "duration": ext["duration"],
                "position": pos,
                "status":   "Playing" if ext["playing"] else "Paused",
            }
        return {"playing": False, "error": str(e)}


# ======================================================================
# Thumbnail helpers
# ======================================================================

async def get_thumbnail_bytes():
    sessions = await _get_sessions_manager()
    current_session = sessions.get_current_session()

    if not current_session:
        raise HTTPException(status_code=404, detail="No active media session")

    info      = await current_session.try_get_media_properties_async()
    thumbnail = getattr(info, "thumbnail", None)

    if not thumbnail:
        raise HTTPException(status_code=404, detail="No thumbnail available")

    stream      = await thumbnail.open_read_async()
    buffer      = Buffer(stream.size)
    await stream.read_async(buffer, stream.size, InputStreamOptions.READ_AHEAD)
    reader      = DataReader.from_buffer(buffer)
    image_bytes = bytearray(buffer.length)
    reader.read_bytes(image_bytes)
    return bytes(image_bytes)


def adjust_brightness(r, g, b, target_brightness=0.35):
    h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
    if v > target_brightness:
        v = target_brightness
    r, g, b = colorsys.hsv_to_rgb(h, s, v)
    return int(r * 255), int(g * 255), int(b * 255)


def get_dominant_colors(image_bytes, num_colors=2):
    img = Image.open(io.BytesIO(image_bytes))
    img = img.convert("RGB")
    img = img.resize((100, 100))
    pixels = list(img.getdata())

    saturated_pixels = []
    for r, g, b in pixels:
        h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        if s > 0.2 and v > 0.1:
            saturated_pixels.append(((r, g, b), s * v))

    if not saturated_pixels:
        saturated_pixels = [(pixel, 1.0) for pixel in pixels[:100]]

    saturated_pixels.sort(key=lambda x: x[1], reverse=True)
    top_saturated = [pixel for pixel, _ in saturated_pixels[:200]]

    from collections import Counter
    color_counts = Counter(top_saturated)
    most_common  = color_counts.most_common(num_colors * 3)

    unique_colors = []
    for color, _ in most_common:
        if len(unique_colors) >= num_colors:
            break
        if all(sum(abs(a - b) for a, b in zip(color, ec)) >= 80 for ec in unique_colors):
            unique_colors.append(color)

    while len(unique_colors) < num_colors:
        unique_colors.append((50, 60, 70))

    colors = []
    for color in unique_colors[:num_colors]:
        r, g, b = adjust_brightness(color[0], color[1], color[2], target_brightness=0.35)
        colors.append({"r": r, "g": g, "b": b, "hex": "#{:02x}{:02x}{:02x}".format(r, g, b)})
    return colors


# ======================================================================
# Forza Horizon 6 telemetry — UDP receiver
# Configure in-game: SETTINGS > GAMEPLAY & HUD > UDP RACE TELEMETRY
#   Data Out IP: <this PC's IP>  |  Port: 5300  |  Format: Car Dash  |  Rate: 60
# ======================================================================

_forza_lock    = threading.Lock()
_forza_data: Dict[str, Any] = {}
_forza_last_ts: float = 0.0
_forza_raw: bytes = b""     # last raw UDP packet for debugging

FORZA_UDP_PORT = 5300


def _parse_forza_packet(data: bytes) -> Dict[str, Any]:
    """Parse a Forza Horizon 6 Car Dash UDP packet (324 bytes, little-endian).

    Offsets confirmed empirically from live debug — FH6 Car Dash shares the
    same layout as FH5 Car Dash for all parsed fields (no +12 shift).
    """
    if len(data) < 247:
        return {}
    f32 = lambda off: struct.unpack_from('<f', data, off)[0]
    s32 = lambda off: struct.unpack_from('<i', data, off)[0]
    u8  = lambda off: struct.unpack_from('<B', data, off)[0]
    u16 = lambda off: struct.unpack_from('<H', data, off)[0]
    return {
        'is_race_on':     s32(0),
        'engine_max_rpm': f32(8),
        'engine_rpm':     f32(16),
        'speed':          f32(180),    # m/s   — confirmed @180
        'tire_temp_fl':   f32(192),    # °F    — FL @192
        'tire_temp_fr':   f32(196),    # °F    — FR @196
        'tire_temp_rl':   f32(200),    # °F    — RL @200
        'tire_temp_rr':   f32(204),    # °F    — RR @204
        'boost':          f32(208),    # bar
        'fuel':           f32(212),    # 0.0–1.0
        'best_lap':       f32(220),    # seconds
        'last_lap':       f32(224),    # seconds
        'current_lap':    f32(228),    # seconds
        'lap_number':     u16(236),
        'accel':          u8(239),     # 0–255 — confirmed @239
        'brake':          u8(240),     # 0–255
        'gear':           u8(243),     # 0=N, 1=1st, ... — confirmed @243
    }


def _forza_listener_thread() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(1.0)
    sock.bind(('0.0.0.0', FORZA_UDP_PORT))
    while True:
        try:
            raw, _ = sock.recvfrom(1024)
            parsed = _parse_forza_packet(raw)
            if parsed:
                with _forza_lock:
                    global _forza_data, _forza_last_ts, _forza_raw
                    _forza_data = parsed
                    _forza_last_ts = time.monotonic()
                    _forza_raw = raw
        except socket.timeout:
            pass
        except Exception:
            pass


threading.Thread(target=_forza_listener_thread, daemon=True).start()


@app.get("/forza")
async def get_forza():
    with _forza_lock:
        d = dict(_forza_data)
        ts = _forza_last_ts
    if not d or (time.monotonic() - ts) > 3.0:
        return {"active": False}

    def f_to_c(f: float) -> float:
        return (f - 32.0) * 5.0 / 9.0

    return {
        "active":      True,   # True whenever packets arrive; is_race_on=0 in FH6 free roam
        "in_race":     bool(d['is_race_on']),
        "rpm":         round(d['engine_rpm']),
        "rpm_max":     round(d['engine_max_rpm']),
        "gear":        d['gear'],
        "speed_kmh":   round(d['speed'] * 3.6, 1),
        "accel":       d['accel'],
        "brake":       d['brake'],
        "boost":       round(d['boost'], 2),
        "fuel":        round(d['fuel'], 2),
        "tire_temp":   [
            round(f_to_c(d['tire_temp_fl']), 1),
            round(f_to_c(d['tire_temp_fr']), 1),
            round(f_to_c(d['tire_temp_rl']), 1),
            round(f_to_c(d['tire_temp_rr']), 1),
        ],
        "lap":         d['lap_number'],
        "current_lap": round(d['current_lap'], 3),
        "best_lap":    round(max(d['best_lap'],    0.0), 3),
        "last_lap":    round(max(d['last_lap'],    0.0), 3),
    }


@app.get("/forza/debug")
async def get_forza_debug():
    """Returns raw bytes at key offsets so you can verify the packet layout."""
    with _forza_lock:
        raw = bytes(_forza_raw)
        ts  = _forza_last_ts
    if not raw:
        return {"error": "no packet received yet"}

    def peek(off, fmt):
        try:
            return struct.unpack_from(fmt, raw, off)[0]
        except Exception:
            return None

    return {
        "packet_len":   len(raw),
        "age_s":        round(time.monotonic() - ts, 2),
        "hex_0_16":     raw[:16].hex(),
        "is_race_on_@0":        peek(0,   '<i'),
        "engine_max_rpm_@8":    peek(8,   '<f'),
        "engine_rpm_@16":       peek(16,  '<f'),
        "speed_ms_@180":        peek(180, '<f'),   # FH5 offset
        "speed_ms_@192":        peek(192, '<f'),   # FH6 offset
        "tire_fl_f_@192":       peek(192, '<f'),   # would be tire temp if offsets shifted
        "tire_fl_f_@204":       peek(204, '<f'),   # FH6 tire temp
        "gear_@243":            peek(243, '<B'),   # FH5 gear
        "gear_@255":            peek(255, '<B'),   # FH6 gear
        "accel_@239":           peek(239, '<B'),   # FH5 accel
        "accel_@251":           peek(251, '<B'),   # FH6 accel
    }


# ======================================================================
# BeamNG telemetry — UDP receiver (OutGauge protocol)
# Configure in BeamNG: Settings > Other > OutGauge
#   IP: <this PC's IP>  |  Port: 4445  |  Mode: 1
# ======================================================================

_beamng_lock    = threading.Lock()
_beamng_data: Dict[str, Any] = {}
_beamng_last_ts: float = 0.0
_beamng_raw: bytes = b""

BEAMNG_UDP_PORT = 4445


def _parse_beamng_packet(data: bytes) -> Dict[str, Any]:
    """Parse a BeamNG OutGauge UDP packet (68 bytes, little-endian)."""
    if len(data) < 68:
        return {}
    f32 = lambda off: struct.unpack_from('<f', data, off)[0]
    # Gear is encoded as ASCII in Display1 (bytes 52–83)
    display1 = data[52:84].rstrip(b'\x00').decode('ascii', errors='ignore').strip()
    try:
        gear_raw = int(display1) if display1.lstrip('-').isdigit() else 0
    except ValueError:
        gear_raw = 0
    return {
        'speed':    f32(8),    # m/s
        'rpm':      f32(12),
        'turbo':    f32(16),   # bar (can be negative if no turbo)
        'fuel':     f32(24),   # 0.0–1.0
        'throttle': f32(40),   # 0.0–1.0
        'brake':    f32(44),   # 0.0–1.0
        'gear':     gear_raw,
    }


def _beamng_listener_thread() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(1.0)
    sock.bind(('0.0.0.0', BEAMNG_UDP_PORT))
    while True:
        try:
            raw, _ = sock.recvfrom(256)
            parsed = _parse_beamng_packet(raw)
            if parsed:
                with _beamng_lock:
                    global _beamng_data, _beamng_last_ts, _beamng_raw
                    _beamng_data = parsed
                    _beamng_last_ts = time.monotonic()
                    _beamng_raw = raw
        except socket.timeout:
            pass
        except Exception:
            pass


threading.Thread(target=_beamng_listener_thread, daemon=True).start()


@app.get("/beamng")
async def get_beamng():
    with _beamng_lock:
        d = dict(_beamng_data)
        ts = _beamng_last_ts
    if not d or (time.monotonic() - ts) > 3.0:
        return {"active": False}
    return {
        "active":     True,
        "rpm":        round(d.get('rpm', 0.0)),
        "rpm_max":    8000,   # OutGauge doesn't expose max RPM
        "gear":       d.get('gear', 0),
        "speed_kmh":  round(d.get('speed', 0.0) * 3.6, 1),
        "accel":      int(d.get('throttle', 0.0) * 255),
        "brake":      int(d.get('brake', 0.0) * 255),
        "boost":      round(max(d.get('turbo', 0.0), 0.0), 2),
        "fuel":       round(d.get('fuel', 0.0), 3),
        "tire_temp":  [0.0, 0.0, 0.0, 0.0],
        "lap": 0, "current_lap": 0.0, "best_lap": 0.0, "last_lap": 0.0,
    }


@app.get("/beamng/debug")
async def get_beamng_debug():
    with _beamng_lock:
        raw = bytes(_beamng_raw)
        ts  = _beamng_last_ts
    if not raw:
        return {"error": "no packet received yet"}

    def peek(off, fmt):
        try:
            return struct.unpack_from(fmt, raw, off)[0]
        except Exception:
            return None

    return {
        "packet_len":   len(raw),
        "age_s":        round(time.monotonic() - ts, 2),
        "hex_0_16":     raw[:16].hex(),
        "speed_@8":     peek(8,  '<f'),
        "rpm_@12":      peek(12, '<f'),
        "turbo_@16":    peek(16, '<f'),
        "throttle_@40": peek(40, '<f'),
        "brake_@44":    peek(44, '<f'),
        "display1_@52": raw[52:84].rstrip(b'\x00').decode('ascii', errors='ignore') if len(raw) >= 84 else None,
    }


# ======================================================================
# Routes
# ======================================================================

@app.get("/")
async def root():
    return {"message": "Media API is running"}


@app.get("/media")
async def media():
    return await get_media_info(include_debug=False)


@app.get("/media/debug")
async def media_debug():
    return await get_media_info(include_debug=True)


@app.get("/media/thumbnail")
async def get_thumbnail():
    try:
        image_bytes = await get_thumbnail_bytes()
        img = Image.open(io.BytesIO(image_bytes))
        img = img.resize((128, 128), Image.Resampling.LANCZOS)
        output = io.BytesIO()
        img.save(output, format="JPEG", quality=85)
        output.seek(0)
        return Response(content=output.read(), media_type="image/jpeg")
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error getting thumbnail: {str(e)}")


@app.get("/media/colors")
async def get_colors():
    try:
        image_bytes = await get_thumbnail_bytes()
        dominant_colors = get_dominant_colors(image_bytes, num_colors=2)
        return {"colors": dominant_colors, "count": len(dominant_colors)}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error getting colors: {str(e)}")


@app.post("/control/play")
async def play():
    session = await get_current_session()
    try:
        await session.try_play_async()
        return {"status": "success", "action": "play"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error playing: {str(e)}")


@app.post("/control/pause")
async def pause():
    session = await get_current_session()
    try:
        await session.try_pause_async()
        return {"status": "success", "action": "pause"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error pausing: {str(e)}")


@app.post("/control/next")
async def next_track():
    session = await get_current_session()
    try:
        await session.try_skip_next_async()
        return {"status": "success", "action": "next"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error skipping next: {str(e)}")


@app.post("/control/previous")
async def previous_track():
    session = await get_current_session()
    try:
        await session.try_skip_previous_async()
        return {"status": "success", "action": "previous"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error skipping previous: {str(e)}")


@app.post("/control/toggle")
async def toggle_playback():
    session = await get_current_session()
    try:
        await session.try_toggle_play_pause_async()
        return {"status": "success", "action": "toggle"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error toggling: {str(e)}")


# ======================================================================
# Volume control — pycaw (Windows master volume via COM)
# COM is blocking; run in asyncio.to_thread so the event loop stays free.
# Interface is re-created per call — COM objects are not thread-safe.
# ======================================================================

def _pycaw_get_volume() -> float:
    from pycaw.utils import AudioUtilities
    return float(AudioUtilities.GetSpeakers().EndpointVolume.GetMasterVolumeLevelScalar())


def _pycaw_set_volume(scalar: float) -> float:
    from pycaw.utils import AudioUtilities
    vol = AudioUtilities.GetSpeakers().EndpointVolume
    vol.SetMasterVolumeLevelScalar(max(0.0, min(1.0, scalar)), None)
    return float(vol.GetMasterVolumeLevelScalar())


@app.get("/volume")
async def get_volume():
    try:
        level = await asyncio.to_thread(_pycaw_get_volume)
        return {"level": round(level, 3)}
    except Exception as e:
        tb = traceback.format_exc()
        logging.error("GET /volume failed:\n%s", tb)
        raise HTTPException(status_code=500, detail=f"{type(e).__name__}: {e}\n{tb}")


@app.post("/volume/up")
async def volume_up():
    try:
        current   = await asyncio.to_thread(_pycaw_get_volume)
        new_level = await asyncio.to_thread(_pycaw_set_volume, current + 0.05)
        return {"level": round(new_level, 3)}
    except Exception as e:
        tb = traceback.format_exc()
        logging.error("POST /volume/up failed:\n%s", tb)
        raise HTTPException(status_code=500, detail=f"{type(e).__name__}: {e}\n{tb}")


@app.post("/volume/down")
async def volume_down():
    try:
        current   = await asyncio.to_thread(_pycaw_get_volume)
        new_level = await asyncio.to_thread(_pycaw_set_volume, current - 0.05)
        return {"level": round(new_level, 3)}
    except Exception as e:
        tb = traceback.format_exc()
        logging.error("POST /volume/down failed:\n%s", tb)
        raise HTTPException(status_code=500, detail=f"{type(e).__name__}: {e}\n{tb}")


@app.get("/stats")
async def get_stats():
    if not _PSUTIL_OK:
        raise HTTPException(status_code=500, detail="psutil not installed. Run: pip install psutil")
    try:
        cpu_percent = psutil.cpu_percent(interval=None)
        ram = psutil.virtual_memory()

        disk_used_gb = disk_total_gb = disk_percent = 0.0
        try:
            disk = psutil.disk_usage("C:\\")
            disk_used_gb  = round(disk.used  / (1024 ** 3), 1)
            disk_total_gb = round(disk.total / (1024 ** 3), 1)
            disk_percent  = round(disk.percent, 1)
        except Exception:
            pass

        cpu_temp = None
        try:
            temps = psutil.sensors_temperatures()
            if temps:
                for _name, entries in temps.items():
                    if entries:
                        cpu_temp = round(entries[0].current, 1)
                        break
        except Exception:
            pass

        gpu_usage = gpu_temp = None
        try:
            import pynvml  # type: ignore
            pynvml.nvmlInit()
            h        = pynvml.nvmlDeviceGetHandleByIndex(0)
            util     = pynvml.nvmlDeviceGetUtilizationRates(h)
            temp     = pynvml.nvmlDeviceGetTemperature(h, pynvml.NVML_TEMPERATURE_GPU)
            gpu_usage = round(float(util.gpu), 1)
            gpu_temp  = round(float(temp), 1)
            pynvml.nvmlShutdown()
        except Exception:
            pass

        return {
            "cpu_usage":    round(cpu_percent, 1),
            "cpu_temp":     cpu_temp,
            "gpu_usage":    gpu_usage,
            "gpu_temp":     gpu_temp,
            "ram_used_mb":  int(ram.used  // (1024 * 1024)),
            "ram_total_mb": int(ram.total // (1024 * 1024)),
            "ram_percent":  round(ram.percent, 1),
            "disk_used_gb":  disk_used_gb,
            "disk_total_gb": disk_total_gb,
            "disk_percent":  disk_percent,
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Stats error: {str(e)}")


# ── Weather endpoint ──────────────────────────────────────────────────────────
_BME_URL         = "http://192.168.1.145/api/json"
_TIMISOARA_LAT   = 45.7489
_TIMISOARA_LON   = 21.2087
_WX_CACHE_SECS   = 300          # Open-Meteo cached 5 min

_wx_lock         = threading.Lock()
_wx_outdoor: Dict[str, Any] = {}
_wx_outdoor_ts: float = 0.0

def _fetch_url(url: str, timeout: float = 5.0) -> bytes:
    ctx = ssl.create_default_context()
    with urllib.request.urlopen(url, timeout=timeout, context=ctx if url.startswith("https") else None) as r:
        return r.read()

@app.get("/weather")
def get_weather():
    global _wx_outdoor, _wx_outdoor_ts
    # ── Sensor (always fresh) ──────────────────────────────────
    sensor: Dict[str, Any] = {"ok": False, "temperatura": 0.0, "umiditate": 0.0,
                               "presiune": 0.0, "altitudine": 0.0}
    try:
        d = json.loads(urllib.request.urlopen(_BME_URL, timeout=3).read().decode())
        sensor = {
            "ok":          True,
            "temperatura": round(float(d.get("temperatura", 0)), 1),
            "umiditate":   round(float(d.get("umiditate",   0)), 1),
            "presiune":    round(float(d.get("presiune",    0)), 1),
            "altitudine":  round(float(d.get("altitudine",  0)), 1),
        }
    except Exception:
        pass

    # ── Outdoor (cached 5 min) ─────────────────────────────────
    now = time.monotonic()
    with _wx_lock:
        cached = dict(_wx_outdoor)
        fresh  = (now - _wx_outdoor_ts) < _WX_CACHE_SECS

    outdoor: Dict[str, Any] = {"ok": False, "temp": 0.0, "feels": 0.0,
                                "humidity": 0, "code": 0, "wind": 0.0}
    if fresh and cached.get("ok"):
        outdoor = cached
    else:
        try:
            url = (
                f"https://api.open-meteo.com/v1/forecast"
                f"?latitude={_TIMISOARA_LAT}&longitude={_TIMISOARA_LON}"
                f"&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                f"weather_code,wind_speed_10m&timezone=auto&wind_speed_unit=kmh"
            )
            od = json.loads(_fetch_url(url))
            c  = od.get("current", {})
            outdoor = {
                "ok":      True,
                "temp":    round(float(c.get("temperature_2m",       0)), 1),
                "feels":   round(float(c.get("apparent_temperature", 0)), 1),
                "humidity":int(c.get("relative_humidity_2m", 0)),
                "code":    int(c.get("weather_code", 0)),
                "wind":    round(float(c.get("wind_speed_10m", 0)), 1),
            }
            with _wx_lock:
                _wx_outdoor    = outdoor
                _wx_outdoor_ts = now
        except Exception:
            pass

    return {"sensor": sensor, "outdoor": outdoor}





# ── System tray ───────────────────────────────────────────────────────────────

def _make_tray_image():
    """64×64 cyan circle with a white music note shape."""
    from PIL import ImageDraw
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, 63, 63], fill=(30, 170, 220))
    # stem
    d.rectangle([38, 14, 42, 38], fill=(255, 255, 255))
    # flag
    d.polygon([(38, 14), (54, 20), (54, 26), (38, 20)], fill=(255, 255, 255))
    # note head
    d.ellipse([26, 34, 44, 46], fill=(255, 255, 255))
    return img


def _run_tray():
    try:
        import pystray
    except ImportError:
        logging.warning("pystray not installed — running without tray icon. pip install pystray")
        return

    def on_exit(icon, _item):
        icon.stop()
        os._exit(0)

    icon = pystray.Icon(
        "media_server",
        _make_tray_image(),
        "Media Server :8000",
        menu=pystray.Menu(
            pystray.MenuItem("Media Server  ●  port 8000", None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Ieșire", on_exit),
        ),
    )
    icon.run()
    os._exit(0)


if __name__ == "__main__":
    # Hide the console window immediately
    try:
        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 0)  # SW_HIDE
    except Exception:
        pass

    # Redirect uvicorn logs to file so nothing appears even if console shows
    log_path = os.path.join(os.path.dirname(__file__), "server.log")
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[
            logging.FileHandler(log_path, encoding="utf-8"),
        ],
        force=True,
    )

    # Start FastAPI server in background thread
    threading.Thread(
        target=lambda: uvicorn.run(
            app, host="0.0.0.0", port=8000,
            log_level="warning",
            access_log=False,
        ),
        daemon=True,
    ).start()

    # Block on tray icon (exits via on_exit)
    _run_tray()

    # Fallback if pystray missing: just run normally
    uvicorn.run(app, host="0.0.0.0", port=8000)
