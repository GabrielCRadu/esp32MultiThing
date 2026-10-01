from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware

try:
    import psutil
    _PSUTIL_OK = True
except ImportError:
    _PSUTIL_OK = False

import uvicorn
import threading
import subprocess
import shutil
import os
import time
from collections import deque

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/")
async def root():
    return {"message": "Stats API is running"}


@app.get("/stats")
async def get_stats():
    if not _PSUTIL_OK:
        raise HTTPException(
            status_code=500,
            detail="psutil not installed. Run: pip install psutil",
        )
    try:
        cpu_percent = psutil.cpu_percent(interval=None)
        ram = psutil.virtual_memory()

        # Disk (C: drive on Windows)
        disk_used_gb = disk_total_gb = disk_percent = 0.0
        try:
            disk = psutil.disk_usage("C:\\")
            disk_used_gb  = round(disk.used  / (1024 ** 3), 1)
            disk_total_gb = round(disk.total / (1024 ** 3), 1)
            disk_percent  = round(disk.percent, 1)
        except Exception:
            pass

        # CPU temperature — usually unavailable on Windows without WMI/OHM
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

        # GPU stats via pynvml (NVIDIA only; install with: pip install nvidia-ml-py)
        gpu_usage = gpu_temp = None
        try:
            import pynvml  # type: ignore
            pynvml.nvmlInit()
            h = pynvml.nvmlDeviceGetHandleByIndex(0)
            util = pynvml.nvmlDeviceGetUtilizationRates(h)
            temp = pynvml.nvmlDeviceGetTemperature(h, pynvml.NVML_TEMPERATURE_GPU)
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


# ---------------------------------------------------------------------------
# FPS Monitor — wraps PresentMon to capture per-frame timing data.
# Place PresentMon.exe (any recent 1.x build) next to this script,
# or anywhere on PATH.  Run the server as Administrator so PresentMon can
# attach to elevated game processes.
# ---------------------------------------------------------------------------

class _FPSMonitor:
    """Background thread that tails PresentMon stdout and stores frametimes."""

    _MAX_FRAMES = 300  # ~5 s at 60 fps

    def __init__(self):
        self._lock   = threading.Lock()
        self._frames: deque[tuple[float, float]] = deque(maxlen=self._MAX_FRAMES)
        self._exe: str | None = None

    def _find_exe(self) -> str | None:
        here = os.path.dirname(os.path.abspath(__file__))
        for name in ("PresentMon.exe", "PresentMon64.exe", "PresentMon64A.exe", "presentmon.exe"):
            p = os.path.join(here, name)
            if os.path.exists(p):
                return p
        for name in ("PresentMon", "PresentMon64", "presentmon"):
            found = shutil.which(name)
            if found:
                return found
        return None

    def start(self):
        self._exe = self._find_exe()
        if not self._exe:
            return
        t = threading.Thread(target=self._run, daemon=True)
        t.start()

    def _run(self):
        cmd = [self._exe, "--output_stdout", "--timed", "0"]
        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                bufsize=1,
            )
            headers: list[str] | None = None
            ft_idx:  int | None       = None
            for line in proc.stdout:
                line = line.strip()
                if not line:
                    continue
                if headers is None:
                    headers = [h.strip() for h in line.split(",")]
                    try:
                        ft_idx = headers.index("msBetweenPresents")
                    except ValueError:
                        pass
                    continue
                if ft_idx is None:
                    continue
                parts = line.split(",")
                if len(parts) > ft_idx:
                    try:
                        ft = float(parts[ft_idx])
                        if 0.1 <= ft <= 1000.0:   # sanity: 1 fps – 10 000 fps
                            with self._lock:
                                self._frames.append((time.monotonic(), ft))
                    except ValueError:
                        pass
        except Exception:
            pass

    @property
    def available(self) -> bool:
        return self._exe is not None

    def get_stats(self) -> dict:
        with self._lock:
            frames = list(self._frames)

        if not frames:
            return {
                "available": self.available,
                "fps": 0.0,
                "fps_1pct_low": 0.0,
                "frametimes": [],
            }

        now = time.monotonic()

        # Current FPS: average frametime of the last 1 second of data
        recent_fts = [ft for ts, ft in frames if now - ts <= 1.0]
        if len(recent_fts) < 5:
            recent_fts = [ft for _, ft in frames[-60:]]

        avg_ft = sum(recent_fts) / len(recent_fts)
        fps    = round(1000.0 / avg_ft, 1) if avg_ft > 0 else 0.0

        # 1 % lows: the slowest (longest frametime) 1 % of all buffered frames
        all_fts     = [ft for _, ft in frames]
        worst_first = sorted(all_fts, reverse=True)
        n           = max(1, len(worst_first) // 100)
        low_avg_ft  = sum(worst_first[:n]) / n
        fps_1pct    = round(1000.0 / low_avg_ft, 1) if low_avg_ft > 0 else 0.0

        # Graph payload: last 50 frametimes in ms
        graph = [round(ft, 2) for _, ft in frames[-50:]]

        return {
            "available":    True,
            "fps":          fps,
            "fps_1pct_low": fps_1pct,
            "frametimes":   graph,
        }


_fps_monitor = _FPSMonitor()
_fps_monitor.start()


@app.get("/fps")
async def get_fps():
    return _fps_monitor.get_stats()


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8001)
