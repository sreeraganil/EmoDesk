#!/usr/bin/env python3
"""
EmoDesk PC companion - sends CPU/RAM/GPU/network stats and the currently
playing song to EmoDesk every 2 seconds (cards "PC stats" and "Now playing").

  pip install psutil requests
  optional:  pip install GPUtil          (NVIDIA GPU load/temperature)
             pip install winsdk          (Windows "now playing")
  Linux now-playing uses `playerctl`; macOS uses osascript (Music / Spotify).

  python pc_companion.py --host emo.local
"""
import argparse, platform, shutil, subprocess, time

import psutil
import requests

try:
    import GPUtil
except Exception:
    GPUtil = None

SYSTEM = platform.system()


def cpu_temp():
    try:
        temps = psutil.sensors_temperatures()
        for key in ("coretemp", "k10temp", "cpu_thermal", "acpitz", "zenpower"):
            if key in temps and temps[key]:
                return round(temps[key][0].current, 1)
    except Exception:
        pass
    return -1


def gpu():
    if not GPUtil:
        return -1, -1
    try:
        g = GPUtil.getGPUs()
        if g:
            return round(g[0].load * 100, 1), round(g[0].temperature, 1)
    except Exception:
        pass
    return -1, -1


def now_playing():
    """Return (title, artist, playing)."""
    try:
        if SYSTEM == "Linux" and shutil.which("playerctl"):
            st = subprocess.run(["playerctl", "status"], capture_output=True, text=True, timeout=2).stdout.strip()
            if st:
                meta = subprocess.run(["playerctl", "metadata", "--format", "{{title}}\t{{artist}}"],
                                      capture_output=True, text=True, timeout=2).stdout.strip().split("\t")
                return meta[0], meta[1] if len(meta) > 1 else "", st == "Playing"
        elif SYSTEM == "Darwin":
            for app in ("Spotify", "Music"):
                script = (f'if application "{app}" is running then tell application "{app}" to '
                          f'(name of current track) & "\t" & (artist of current track) & "\t" & (player state as string)')
                out = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=3).stdout.strip()
                if out:
                    t, a, s = (out.split("\t") + ["", "", ""])[:3]
                    return t, a, s == "playing"
        elif SYSTEM == "Windows":
            import asyncio
            from winsdk.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as MM

            async def get():
                mgr = await MM.request_async()
                s = mgr.get_current_session()
                if not s:
                    return "", "", False
                p = await s.try_get_media_properties_async()
                playing = s.get_playback_info().playback_status == 4
                return p.title or "", p.artist or "", playing

            return asyncio.run(get())
    except Exception:
        pass
    return "", "", False


def main():
    ap = argparse.ArgumentParser(description="Send PC stats to EmoDesk")
    ap.add_argument("--host", default="emo.local", help="EmoDesk address (default emo.local)")
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()
    url = f"http://{args.host}/api/pc"
    last = psutil.net_io_counters()
    last_t = time.time()
    print(f"Sending to {url} every {args.interval}s - Ctrl+C to stop")
    while True:
        time.sleep(args.interval)
        now = psutil.net_io_counters()
        dt = max(0.1, time.time() - last_t)
        up = (now.bytes_sent - last.bytes_sent) / dt / 1024
        down = (now.bytes_recv - last.bytes_recv) / dt / 1024
        last, last_t = now, time.time()
        g_load, g_temp = gpu()
        song, artist, playing = now_playing()
        data = {
            "cpu": psutil.cpu_percent(), "ram": psutil.virtual_memory().percent,
            "gpu": g_load, "cpuT": cpu_temp(), "gpuT": g_temp,
            "up": round(up, 1), "down": round(down, 1),
            "song": song[:60], "artist": artist[:38], "playing": playing,
        }
        try:
            requests.post(url, json=data, timeout=3)
        except requests.RequestException as e:
            print("EmoDesk not reachable:", e.__class__.__name__)


if __name__ == "__main__":
    main()
