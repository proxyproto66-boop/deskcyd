"""
DeskCYD companion app for Windows.
Built into DeskCYD.exe automatically by GitHub Actions. It sits in the system
tray, reads what's playing from Windows' media controls, and serves it to the
CYD over your Wi-Fi.
"""

import os
import sys

# When built as a windowless .exe there's no console, so send output to a log file.
APP_DIR = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "DeskCYD")
os.makedirs(APP_DIR, exist_ok=True)
if sys.stdout is None or sys.stderr is None:
    _log = open(os.path.join(APP_DIR, "log.txt"), "a", buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = _log

import asyncio
import hashlib
import io
import json
import socket
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from PIL import Image
from winsdk.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
from winsdk.windows.storage.streams import Buffer, DataReader, InputStreamOptions

# ---------------- settings ----------------
HTTP_PORT = 8765          # port the CYD talks to
DISCOVERY_PORT = 47800    # UDP port used so the CYD can find this PC
ART_SIZE = 150            # album art size in pixels (square)
POLL_SECONDS = 0.4        # how often we check what's playing
PREFER_APP = "spotify"    # if several apps are playing, prefer this one
# ------------------------------------------

lock = threading.Lock()
state = {
    "active": False, "playing": False, "title": "", "artist": "", "album": "",
    "app": "", "pos": 0, "dur": 0, "art": "",
    "bg": 0x18E3, "btn": 0x3186, "accent": 0xFFFF,
}
art_jpeg = b""

loop = None          # asyncio loop running in the media thread
manager = None       # Windows media session manager
poll_now = None      # asyncio.Event: set to force an immediate refresh


# ---------- helpers ----------
def rgb565(rgb):
    r, g, b = rgb
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def tint(rgb, brightest):
    """Scale a color so its brightest channel equals `brightest`."""
    m = max(rgb) or 1
    f = brightest / m
    return tuple(min(255, int(c * f)) for c in rgb)


def process_art(raw):
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    img = img.resize((ART_SIZE, ART_SIZE), Image.LANCZOS)
    avg = img.resize((1, 1), Image.BOX).getpixel((0, 0))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85, progressive=False)  # CYD needs baseline JPEG
    return out.getvalue(), avg


async def read_thumbnail(ref):
    if ref is None:
        return None
    stream = await ref.open_read_async()
    size = int(stream.size)
    if size == 0:
        return None
    buf = Buffer(size)
    await stream.read_async(buf, size, InputStreamOptions.READ_AHEAD)
    reader = DataReader.from_buffer(buf)
    try:  # newer winsdk: fills a buffer you pass in
        data = bytearray(buf.length)
        reader.read_bytes(data)
        return bytes(data)
    except Exception:  # older style: returns the bytes
        return bytes(reader.read_bytes(buf.length))


def pick_session():
    if manager is None:
        return None
    try:
        for s in manager.get_sessions():
            if PREFER_APP in (s.source_app_user_model_id or "").lower():
                return s
    except Exception:
        pass
    return manager.get_current_session()


# ---------- media polling ----------
current_key = None
key_changed_at = 0.0
art_refetched = False


async def poll_once():
    global art_jpeg, current_key, key_changed_at, art_refetched
    s = pick_session()
    if s is None:
        with lock:
            state["active"] = False
            state["playing"] = False
        return

    props = await s.try_get_media_properties_async()
    info = s.get_playback_info()
    playing = info.playback_status == PlaybackStatus.PLAYING

    title = props.title or ""
    artist = props.artist or ""
    album = props.album_title or ""
    key = f"{title}|{artist}|{album}"

    # Fetch art when the song changes, and once more ~2 s later because some
    # apps (Spotify) update the picture a moment after the title.
    need_art = key != current_key or (not art_refetched and time.time() - key_changed_at > 2)
    if key != current_key:
        current_key = key
        key_changed_at = time.time()
        art_refetched = False
    elif need_art:
        art_refetched = True

    if need_art:
        try:
            raw = await read_thumbnail(props.thumbnail)
        except Exception:
            raw = None
        if raw:
            try:
                jpg, avg = process_art(raw)
            except Exception:
                jpg, avg = b"", (60, 60, 60)
        else:
            jpg, avg = b"", (60, 60, 60)
        with lock:
            art_jpeg = jpg
            state["art"] = hashlib.md5(jpg).hexdigest()[:8] if jpg else ""
            state["bg"] = rgb565(tint(avg, 70))
            state["btn"] = rgb565(tint(avg, 115))
            state["accent"] = rgb565(tint(avg, 240))

    pos = dur = 0.0
    try:
        tl = s.get_timeline_properties()
        pos = tl.position.total_seconds()
        dur = tl.end_time.total_seconds()
        if playing:
            elapsed = (datetime.now(timezone.utc) - tl.last_updated_time).total_seconds()
            if 0 <= elapsed < 3600:
                pos += elapsed
        if dur > 0:
            pos = max(0.0, min(pos, dur))
    except Exception:
        pass

    app = (s.source_app_user_model_id or "").split("!")[0].replace(".exe", "")
    with lock:
        state.update(active=True, playing=playing, title=title, artist=artist,
                     album=album, app=app, pos=int(pos), dur=int(dur))


async def media_main():
    global manager, poll_now
    poll_now = asyncio.Event()
    manager = await MediaManager.request_async()
    print("Watching Windows media sessions.")
    while True:
        try:
            await poll_once()
        except Exception as e:
            print("poll error:", e)
        try:
            await asyncio.wait_for(poll_now.wait(), POLL_SECONDS)
        except asyncio.TimeoutError:
            pass
        poll_now.clear()


async def do_command(cmd):
    s = pick_session()
    if s is None:
        return False
    if cmd == "playpause":
        await s.try_toggle_play_pause_async()
    elif cmd == "next":
        await s.try_skip_next_async()
    elif cmd == "prev":
        await s.try_skip_previous_async()
    else:
        return False
    await asyncio.sleep(0.15)
    poll_now.set()
    return True


def media_thread():
    global loop
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(media_main())


# ---------- HTTP API for the CYD ----------
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/api/now":
            with lock:
                body = json.dumps(state).encode()
            self._send(200, body, "application/json")
        elif url.path == "/api/art":
            with lock:
                body = art_jpeg
            if body:
                self._send(200, body, "image/jpeg")
            else:
                self._send(404, b"no art", "text/plain")
        elif url.path == "/api/cmd":
            cmd = parse_qs(url.query).get("c", [""])[0]
            ok = False
            if loop is not None:
                fut = asyncio.run_coroutine_threadsafe(do_command(cmd), loop)
                try:
                    ok = fut.result(timeout=3)
                except Exception as e:
                    print("command error:", e)
            self._send(200 if ok else 400, b"ok" if ok else b"fail", "text/plain")
        else:
            self._send(200, b"DeskCYD companion is running.", "text/plain")

    def log_message(self, *args):
        pass  # keep the console quiet


# ---------- discovery beacon ----------
def local_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                ips.add(ip)
    except Exception:
        pass
    return ips


def beacon_thread():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = f"DESKCYD {HTTP_PORT}".encode()
    while True:
        targets = {"255.255.255.255"}
        for ip in local_ips():
            targets.add(ip.rsplit(".", 1)[0] + ".255")
        for t in targets:
            try:
                sock.sendto(msg, (t, DISCOVERY_PORT))
            except OSError:
                pass
        time.sleep(2)




# ---------- tray app ----------
import ctypes
import winreg

import pystray
from PIL import ImageDraw

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
APP_NAME = "DeskCYD"


def launch_command():
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    return f'"{sys.executable}" "{os.path.abspath(__file__)}"'


def autostart_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, APP_NAME)
            return True
    except OSError:
        return False


def set_autostart(on):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
        if on:
            winreg.SetValueEx(k, APP_NAME, 0, winreg.REG_SZ, launch_command())
        else:
            try:
                winreg.DeleteValue(k, APP_NAME)
            except OSError:
                pass


def make_icon():
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((4, 10, 60, 54), radius=8, fill=(250, 204, 21, 255))
    d.rounded_rectangle((10, 16, 54, 48), radius=4, fill=(24, 24, 27, 255))
    d.polygon([(26, 22), (26, 42), (42, 32)], fill=(250, 204, 21, 255))
    return img


def status_text(item=None):
    with lock:
        if state["active"] and state["title"]:
            t = f"{state['title']} - {state['artist']}"
            return ("Now: " + t)[:60]
    return "DeskCYD is running"


def toggle_autostart(icon, item):
    set_autostart(not autostart_enabled())


def quit_app(icon, item):
    icon.stop()
    os._exit(0)


def message_box(text):
    ctypes.windll.user32.MessageBoxW(None, text, "DeskCYD", 0x40)


class SingleServer(ThreadingHTTPServer):
    allow_reuse_address = False  # on Windows, reuse would let two copies share the port


def main():
    # Only one copy can own the port; a second launch just says so and exits.
    try:
        server = SingleServer(("0.0.0.0", HTTP_PORT), Handler)
    except OSError:
        message_box("DeskCYD is already running (look for its icon in the system tray).")
        return

    first_run_marker = os.path.join(APP_DIR, "first_run_done")
    first_run = not os.path.exists(first_run_marker)
    if first_run:
        set_autostart(True)  # start with Windows by default; untick in the tray menu
        open(first_run_marker, "w").close()

    threading.Thread(target=media_thread, daemon=True).start()
    threading.Thread(target=beacon_thread, daemon=True).start()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("DeskCYD running on port", HTTP_PORT, "IPs:", ", ".join(sorted(local_ips())))

    menu = pystray.Menu(
        pystray.MenuItem(status_text, None, enabled=False),
        pystray.MenuItem("Start with Windows", toggle_autostart,
                         checked=lambda item: autostart_enabled()),
        pystray.MenuItem("Quit DeskCYD", quit_app),
    )
    icon = pystray.Icon(APP_NAME, make_icon(), "DeskCYD", menu)

    def on_ready(ic):
        ic.visible = True
        if first_run:
            try:
                ic.notify("DeskCYD is running. It lives here in the tray and starts with Windows.", "DeskCYD")
            except Exception:
                pass

    icon.run(setup=on_ready)


if __name__ == "__main__":
    main()
