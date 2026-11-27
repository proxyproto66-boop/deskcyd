"""
DeskCYD companion app for Windows.
Built into DeskCYD.exe automatically by GitHub Actions. It sits in the system
tray, reads what's playing from Windows' media controls, renders the song text
for the CYD, talks to Spotify (heart / playlists, optional) and hands out
firmware updates to the CYD over Wi-Fi.
"""

import os
import sys

APP_VERSION = 0          # replaced with the build number by the GitHub build
SITE = "https://proxyproto66-boop.github.io/deskcyd/"

# When built as a windowless .exe there's no console, so send output to a log file.
APP_DIR = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "DeskCYD")
os.makedirs(APP_DIR, exist_ok=True)
if sys.stdout is None or sys.stderr is None:
    _log = open(os.path.join(APP_DIR, "log.txt"), "a", buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = _log

import array
import asyncio
import base64
import hashlib
import io
import json
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from PIL import Image, ImageDraw, ImageFont
from winsdk.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
from winsdk.windows.storage.streams import Buffer, DataReader, InputStreamOptions

# ---------------- settings ----------------
HTTP_PORT = 8765          # port the CYD talks to
DISCOVERY_PORT = 47800    # UDP port used so the CYD can find this PC
SPOTIFY_REDIRECT_PORT = 8766
ART_SIZE = 140            # must match ART_S in the firmware
TEXT_W = 140              # must match TW in the firmware
SCROLL_MAX_W = 480        # must match the firmware's scroll buffers
POLL_SECONDS = 0.4
PREFER_APP = "spotify"
UPDATE_CHECK_SECONDS = 1800
# ------------------------------------------

lock = threading.Lock()
state = {
    "active": False, "playing": False, "pos_ms": 0, "dur_ms": 0,
    "bg": 0x18E3, "tid": "", "aid": "", "liked": -1, "sp": False,
    "app": APP_VERSION, "fwl": 0, "fwr": False,
}
art_raw = b""
text_blob = b""
cyd_fw = 0                # firmware version the CYD reports
fw_latest = 0             # newest firmware on the website
app_latest = 0            # newest PC app on the website
fw_file = os.path.join(APP_DIR, "DeskCYD-app.bin")

loop = None
manager = None
poll_now = None
current_title = ""
current_artist = ""


# =====================================================================
# Colors and pictures for the CYD
# =====================================================================
def rgb565(rgb):
    r, g, b = rgb[:3]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def from565(c):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def tint(rgb, brightest):
    m = max(rgb) or 1
    f = brightest / m
    return tuple(min(255, int(ch * f)) for ch in rgb)


def to_raw565(img):
    """RGB image -> little-endian RGB565 bytes."""
    px = img.convert("RGB").tobytes()
    out = array.array("H", bytes(len(px) // 3 * 2))
    j = 0
    for i in range(0, len(px), 3):
        out[j] = ((px[i] & 0xF8) << 8) | ((px[i + 1] & 0xFC) << 3) | (px[i + 2] >> 3)
        j += 1
    if sys.byteorder != "little":
        out.byteswap()
    return out.tobytes()


def process_art(raw, bg_rgb):
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    # Spotify sometimes gives a wide image with bars; crop to the centre square
    w, h = img.size
    s = min(w, h)
    img = img.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
    avg = img.resize((1, 1), Image.BOX).getpixel((0, 0))
    img = img.resize((ART_SIZE, ART_SIZE), Image.LANCZOS)
    return img, avg


def round_corners(img, bg_rgb, radius=6):
    mask = Image.new("L", (img.width * 4, img.height * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, mask.width - 1, mask.height - 1), radius * 4, fill=255)
    mask = mask.resize(img.size, Image.LANCZOS)
    base = Image.new("RGB", img.size, bg_rgb)
    base.paste(img, (0, 0), mask)
    return base


# ---------------- fonts ----------------
def resource(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


WIN_FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
_font_cache = {}


def figtree(size, weight):
    key = ("fig", size, weight)
    if key not in _font_cache:
        path = resource("Figtree.ttf")
        try:
            f = ImageFont.truetype(path, size)
            try:
                f.set_variation_by_axes([weight])
            except Exception:
                pass
        except Exception:
            f = win_font("segoeuib.ttf" if weight >= 600 else "segoeui.ttf", size)
        _font_cache[key] = f
    return _font_cache[key]


def win_font(name, size):
    key = ("win", name, size)
    if key not in _font_cache:
        f = None
        for n in name.split("|"):
            try:
                f = ImageFont.truetype(os.path.join(WIN_FONTS, n), size)
                break
            except Exception:
                continue
        if f is None:
            f = ImageFont.load_default()
        _font_cache[key] = f
    return _font_cache[key]


def font_for_char(ch, size, bold):
    """Figtree for Latin text, Windows fonts for everything else (any language)."""
    o = ord(ch)
    if o < 0x250 or 0x2000 <= o <= 0x206F or 0x20A0 <= o <= 0x20CF:
        return figtree(size, 700 if bold else 500)
    if 0xAC00 <= o <= 0xD7AF or 0x1100 <= o <= 0x11FF or 0x3130 <= o <= 0x318F:
        return win_font("malgunbd.ttf|malgun.ttf" if bold else "malgun.ttf", size)
    if 0x3040 <= o <= 0x30FF:
        return win_font("YuGothB.ttc|YuGothM.ttc|msgothic.ttc" if bold else "YuGothM.ttc|msgothic.ttc", size)
    if 0x2E80 <= o <= 0x9FFF or 0xF900 <= o <= 0xFAFF or 0xFF00 <= o <= 0xFFEF:
        return win_font("msyhbd.ttc|msyh.ttc|YuGothB.ttc" if bold else "msyh.ttc|YuGothM.ttc", size)
    if o >= 0x1F000:
        return win_font("seguiemj.ttf|seguisym.ttf", size)
    return win_font("segoeuib.ttf|segoeui.ttf" if bold else "segoeui.ttf", size)


def runs(text, size, bold):
    """Split text into pieces that share a font."""
    out = []
    for ch in text:
        f = font_for_char(ch, size, bold)
        if out and out[-1][1] is f:
            out[-1][0] += ch
        else:
            out.append([ch, f])
    return out


def text_width(text, size, bold):
    return sum(f.getlength(t) for t, f in runs(text, size, bold))


def draw_text(img, x, y_center, text, size, bold, color):
    d = ImageDraw.Draw(img)
    for t, f in runs(text, size, bold):
        try:
            d.text((x, y_center), t, font=f, fill=color, anchor="lm", embedded_color=True)
        except Exception:
            d.text((x, y_center), t, font=f, fill=color, anchor="lm")
        x += f.getlength(t)
    return x


def ellipsize(text, width, size, bold):
    if text_width(text, size, bold) <= width:
        return text
    while text and text_width(text + "…", size, bold) > width:
        text = text[:-1]
    return text.rstrip() + "…"


def line_image(text, size, bold, color, bg_rgb, h, allow_scroll):
    """Returns (w, h, scroll, raw565). Static lines are TEXT_W wide; long ones scroll."""
    w_needed = text_width(text, size, bold)
    if w_needed <= TEXT_W or not allow_scroll:
        text = ellipsize(text, TEXT_W, size, bold)
        img = Image.new("RGB", (TEXT_W, h), bg_rgb)
        draw_text(img, 0, h / 2, text, size, bold, color)
        return TEXT_W, h, False, to_raw565(img)
    gap = 48
    w = int(w_needed) + gap
    if w > SCROLL_MAX_W:
        text = ellipsize(text, SCROLL_MAX_W - gap, size, bold)
        w = int(text_width(text, size, bold)) + gap
    img = Image.new("RGB", (w, h), bg_rgb)
    draw_text(img, 0, h / 2, text, size, bold, color)
    return w, h, True, to_raw565(img)


def wrap_two(text, size, bold):
    """First line that fits, and the rest."""
    if text_width(text, size, bold) <= TEXT_W:
        return text, ""
    words = text.split(" ")
    line = ""
    for i, wd in enumerate(words):
        test = (line + " " + wd).strip()
        if text_width(test, size, bold) > TEXT_W:
            if not line:   # one long word (or no spaces, e.g. Japanese): break by characters
                cut = len(wd)
                while cut > 1 and text_width(wd[:cut], size, bold) > TEXT_W:
                    cut -= 1
                return wd[:cut], (wd[cut:] + " " + " ".join(words[i + 1:])).strip()
            return line, " ".join(words[i:])
        line = test
    return line, ""


def build_text_blob(title, artist, album, bg_rgb):
    title = title or "Unknown title"
    l1, rest = wrap_two(title, 19, True)
    parts = [line_image(l1, 19, True, (255, 255, 255), bg_rgb, 24, False)]
    if rest:
        parts.append(line_image(rest, 19, True, (255, 255, 255), bg_rgb, 24, True))
    else:
        parts.append((0, 0, False, b""))
    parts.append(line_image(artist or "", 15, False, (179, 179, 179), bg_rgb, 20, True))
    parts.append(line_image(album or "", 13, False, (138, 138, 138), bg_rgb, 18, False))
    out = bytearray()
    for w, h, scroll, raw in parts:
        out += bytes([w & 255, w >> 8, h & 255, h >> 8, 1 if scroll else 0, 0])
        out += raw
    return bytes(out)


# =====================================================================
# Windows media session
# =====================================================================
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
    try:
        data = bytearray(buf.length)
        reader.read_bytes(data)
        return bytes(data)
    except Exception:
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


current_key = None
key_changed_at = 0.0
art_refetched = False
bg_rgb = (24, 24, 24)


async def poll_once():
    global art_raw, text_blob, current_key, key_changed_at, art_refetched, bg_rgb
    global current_title, current_artist
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

    need_art = key != current_key or (not art_refetched and time.time() - key_changed_at > 2)
    song_changed = key != current_key
    if song_changed:
        current_key = key
        key_changed_at = time.time()
        art_refetched = False
        current_title, current_artist = title, artist
        threading.Thread(target=spotify_refresh_track, daemon=True).start()
    elif need_art:
        art_refetched = True

    if need_art:
        img = None
        avg = (60, 60, 60)
        try:
            raw = await read_thumbnail(props.thumbnail)
            if raw:
                img, avg = process_art(raw, bg_rgb)
        except Exception:
            img = None
        new_bg565 = rgb565(tint(avg, 64))
        new_bg = from565(new_bg565)          # exactly the color the CYD will fill with
        art = round_corners(img, new_bg) if img is not None else None
        art_bytes = to_raw565(art) if art is not None else b""
        blob = build_text_blob(title, artist, album, new_bg)
        with lock:
            bg_rgb = new_bg
            art_raw = art_bytes
            text_blob = blob
            state["bg"] = new_bg565
            state["aid"] = hashlib.md5(art_bytes).hexdigest()[:8] if art_bytes else ""
            state["tid"] = hashlib.md5(blob).hexdigest()[:8]

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

    with lock:
        state.update(active=True, playing=playing, pos_ms=int(pos * 1000), dur_ms=int(dur * 1000))


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


async def do_seek(ms):
    s = pick_session()
    if s is None:
        return False
    ok = False
    try:
        ok = await s.try_change_playback_position_async(int(ms) * 10000)   # 100 ns units
    except Exception as e:
        print("seek error:", e)
    if not ok and spotify.connected():
        ok = spotify.seek(ms)
    await asyncio.sleep(0.2)
    poll_now.set()
    return ok


def media_thread():
    global loop
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(media_main())


def run_async(coro, timeout=4):
    if loop is None:
        return False
    fut = asyncio.run_coroutine_threadsafe(coro, loop)
    try:
        return fut.result(timeout=timeout)
    except Exception as e:
        print("async error:", e)
        return False


# =====================================================================
# Spotify (optional: heart, playlists, seek fallback)
# =====================================================================
SPOTIFY_FILE = os.path.join(APP_DIR, "spotify.json")
SCOPES = ("user-library-read user-library-modify playlist-read-private playlist-read-collaborative "
          "playlist-modify-private playlist-modify-public user-read-currently-playing "
          "user-read-playback-state user-modify-playback-state")


class Spotify:
    def __init__(self):
        self.cfg = {}
        self.token = None
        self.token_exp = 0
        self.user_id = None
        self.track_uri = None
        try:
            with open(SPOTIFY_FILE, "r", encoding="utf-8") as fh:
                self.cfg = json.load(fh)
        except Exception:
            self.cfg = {}

    def save(self):
        with open(SPOTIFY_FILE, "w", encoding="utf-8") as fh:
            json.dump(self.cfg, fh)

    def connected(self):
        return bool(self.cfg.get("refresh_token") and self.cfg.get("client_id"))

    def redirect_uri(self):
        return f"http://127.0.0.1:{SPOTIFY_REDIRECT_PORT}/callback"

    # ---- login (PKCE, no secret needed) ----
    def login(self, client_id):
        verifier = base64.urlsafe_b64encode(secrets.token_bytes(48)).decode().rstrip("=")
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
        st = secrets.token_hex(8)
        result = {}

        class CB(BaseHTTPRequestHandler):
            def do_GET(self):
                q = parse_qs(urlparse(self.path).query)
                result["code"] = q.get("code", [""])[0]
                result["state"] = q.get("state", [""])[0]
                result["error"] = q.get("error", [""])[0]
                msg = "DeskCYD is connected to Spotify. You can close this tab." if result["code"] else \
                    "Spotify login failed: " + result["error"]
                body = f"<html><body style='font-family:sans-serif;background:#121212;color:#fff;" \
                       f"display:grid;place-items:center;height:90vh'><h2>{msg}</h2></body></html>".encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        srv = ThreadingHTTPServer(("127.0.0.1", SPOTIFY_REDIRECT_PORT), CB)
        srv.timeout = 180
        url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
            "client_id": client_id, "response_type": "code", "redirect_uri": self.redirect_uri(),
            "code_challenge_method": "S256", "code_challenge": challenge, "scope": SCOPES, "state": st,
        })
        webbrowser.open(url)
        srv.handle_request()   # waits for the redirect (or times out)
        srv.server_close()
        if not result.get("code") or result.get("state") != st:
            return False
        tok = self._token_request({
            "grant_type": "authorization_code", "code": result["code"],
            "redirect_uri": self.redirect_uri(), "client_id": client_id, "code_verifier": verifier,
        })
        if not tok or "refresh_token" not in tok:
            return False
        self.cfg = {"client_id": client_id, "refresh_token": tok["refresh_token"]}
        self.token = tok["access_token"]
        self.token_exp = time.time() + tok.get("expires_in", 3600) - 60
        self.save()
        return True

    def logout(self):
        self.cfg = {}
        self.token = None
        try:
            os.remove(SPOTIFY_FILE)
        except Exception:
            pass

    def _token_request(self, form):
        data = urllib.parse.urlencode(form).encode()
        req = urllib.request.Request("https://accounts.spotify.com/api/token", data=data,
                                     headers={"Content-Type": "application/x-www-form-urlencoded"})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return json.loads(r.read())
        except Exception as e:
            print("spotify token error:", e)
            return None

    def access_token(self):
        if self.token and time.time() < self.token_exp:
            return self.token
        if not self.connected():
            return None
        tok = self._token_request({"grant_type": "refresh_token",
                                   "refresh_token": self.cfg["refresh_token"],
                                   "client_id": self.cfg["client_id"]})
        if not tok:
            return None
        self.token = tok["access_token"]
        self.token_exp = time.time() + tok.get("expires_in", 3600) - 60
        if tok.get("refresh_token"):
            self.cfg["refresh_token"] = tok["refresh_token"]
            self.save()
        return self.token

    def api(self, method, path, query=None, body=None):
        tok = self.access_token()
        if not tok:
            return None, 401
        url = "https://api.spotify.com/v1" + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method, headers={
            "Authorization": "Bearer " + tok, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                raw = r.read()
                return (json.loads(raw) if raw else {}), r.status
        except urllib.error.HTTPError as e:
            print("spotify api", method, path, e.code, e.read()[:200])
            return None, e.code
        except Exception as e:
            print("spotify api error:", e)
            return None, 0

    # ---- features ----
    def current_uri(self):
        data, code = self.api("GET", "/me/player/currently-playing")
        if data and data.get("item"):
            return data["item"].get("uri")
        return None

    def is_saved(self, uri):
        data, code = self.api("GET", "/me/library/contains", {"uris": uri})
        if isinstance(data, list) and data:
            return bool(data[0])
        return None

    def set_saved(self, uri, saved):
        data, code = self.api("PUT" if saved else "DELETE", "/me/library", {"uris": uri}, {"uris": [uri]})
        return code in (200, 201, 204)

    def playlists(self):
        if not self.user_id:
            me, _ = self.api("GET", "/me")
            self.user_id = (me or {}).get("id")
        out = []
        offset = 0
        while offset < 200:
            data, code = self.api("GET", "/me/playlists", {"limit": 50, "offset": offset})
            if not data:
                break
            for p in data.get("items", []):
                if not p:
                    continue
                own = (p.get("owner") or {}).get("id") == self.user_id
                if own or p.get("collaborative"):
                    out.append({"id": p["id"], "name": p.get("name", "")[:35]})
            if not data.get("next"):
                break
            offset += 50
        return out

    def add_to_playlist(self, pid, uri):
        data, code = self.api("POST", f"/playlists/{pid}/items", None, {"uris": [uri]})
        return code in (200, 201)

    def seek(self, ms):
        data, code = self.api("PUT", "/me/player/seek", {"position_ms": int(ms)})
        return code in (200, 202, 204)


spotify = Spotify()


def spotify_refresh_track():
    """When the song changes: look up its Spotify ID and whether it's liked."""
    if not spotify.connected():
        with lock:
            state["sp"] = False
            state["liked"] = -1
        return
    time.sleep(0.8)   # give Spotify a moment to report the new song
    uri = spotify.current_uri()
    spotify.track_uri = uri
    liked = spotify.is_saved(uri) if uri else None
    with lock:
        state["sp"] = True
        state["liked"] = -1 if liked is None else (1 if liked else 0)


# =====================================================================
# Updates (firmware for the CYD + this app)
# =====================================================================
def fetch(url, timeout=20):
    req = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def check_updates():
    global fw_latest, app_latest
    try:
        info = json.loads(fetch(SITE + "version.json?t=" + str(int(time.time()))))
        v = int(info.get("version", 0))
    except Exception as e:
        print("update check failed:", e)
        return
    fw_latest = app_latest = v
    have = 0
    try:
        with open(fw_file + ".ver", "r") as fh:
            have = int(fh.read().strip() or 0)
    except Exception:
        pass
    if v > have:
        try:
            data = fetch(SITE + "DeskCYD-app.bin?v=" + str(v), timeout=60)
            with open(fw_file, "wb") as fh:
                fh.write(data)
            with open(fw_file + ".ver", "w") as fh:
                fh.write(str(v))
            have = v
            print("downloaded firmware v", v)
        except Exception as e:
            print("firmware download failed:", e)
    with lock:
        state["fwl"] = v
        state["fwr"] = have >= v and os.path.exists(fw_file)
    refresh_menu()


def update_thread():
    while True:
        check_updates()
        time.sleep(UPDATE_CHECK_SECONDS)


def update_app():
    """Download the new DeskCYD.exe, swap it in and restart."""
    if not getattr(sys, "frozen", False):
        return
    try:
        data = fetch(SITE + "DeskCYD.exe?v=" + str(app_latest), timeout=180)
    except Exception as e:
        message_box("Couldn't download the update:\n" + str(e))
        return
    exe = sys.executable
    new = exe + ".new"
    with open(new, "wb") as fh:
        fh.write(data)
    bat = os.path.join(tempfile.gettempdir(), "deskcyd_update.bat")
    with open(bat, "w") as fh:
        fh.write("@echo off\r\n"
                 "timeout /t 2 /nobreak >nul\r\n"
                 f'move /y "{new}" "{exe}" >nul\r\n'
                 f'start "" "{exe}"\r\n'
                 'del "%~f0"\r\n')
    subprocess.Popen(["cmd", "/c", bat], creationflags=0x08000000)   # no window
    os._exit(0)


# =====================================================================
# HTTP API for the CYD
# =====================================================================
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj).encode(), "application/json")

    def do_GET(self):
        global cyd_fw
        url = urlparse(self.path)
        q = parse_qs(url.query)
        p = url.path
        if p == "/api/now":
            try:
                cyd_fw = int(q.get("fw", ["0"])[0])
            except ValueError:
                pass
            now = time.time()
            tzoff = int(datetime.now().astimezone().utcoffset().total_seconds())
            with lock:
                st = dict(state)
            st["epoch"] = int(now)
            st["tz"] = tzoff
            # old firmware compatibility
            st["pos"] = st["pos_ms"] // 1000
            st["dur"] = st["dur_ms"] // 1000
            self._json(st)
        elif p == "/api/text":
            with lock:
                body = text_blob
            self._send(200 if body else 404, body or b"none", "application/octet-stream")
        elif p == "/api/art":
            with lock:
                body = art_raw
            if body:
                self._send(200, body, "application/octet-stream")
            else:
                self._send(404, b"no art", "text/plain")
        elif p == "/api/cmd":
            ok = run_async(do_command(q.get("c", [""])[0]))
            self._send(200 if ok else 400, b"ok" if ok else b"fail", "text/plain")
        elif p == "/api/seek":
            try:
                ms = int(q.get("ms", ["0"])[0])
            except ValueError:
                ms = 0
            ok = run_async(do_seek(ms), timeout=8)
            self._send(200 if ok else 400, b"ok" if ok else b"fail", "text/plain")
        elif p == "/api/like":
            self._json(self.like())
        elif p == "/api/playlists":
            self._json(spotify.playlists() if spotify.connected() else [])
        elif p == "/api/addto":
            self._json(self.add_to(q.get("id", [""])[0]))
        elif p == "/api/firmware":
            try:
                with open(fw_file, "rb") as fh:
                    body = fh.read()
                self._send(200, body, "application/octet-stream")
            except Exception:
                self._send(404, b"no firmware", "text/plain")
        else:
            self._send(200, b"DeskCYD companion is running.", "text/plain")

    def like(self):
        if not spotify.connected():
            return {"ok": False, "liked": False, "msg": "Connect Spotify in the tray app"}
        uri = spotify.track_uri or spotify.current_uri()
        if not uri:
            return {"ok": False, "liked": False, "msg": "Song not found on Spotify"}
        spotify.track_uri = uri
        now_saved = spotify.is_saved(uri)
        want = not bool(now_saved)
        ok = spotify.set_saved(uri, want)
        if ok:
            with lock:
                state["liked"] = 1 if want else 0
            return {"ok": True, "liked": want, "msg": "Added to Liked Songs" if want else "Removed from Liked Songs"}
        return {"ok": False, "liked": bool(now_saved), "msg": "Spotify said no - try again"}

    def add_to(self, pid):
        if not spotify.connected():
            return {"ok": False, "msg": "Connect Spotify in the tray app"}
        uri = spotify.track_uri or spotify.current_uri()
        if not uri or not pid:
            return {"ok": False, "msg": "Song not found on Spotify"}
        ok = spotify.add_to_playlist(pid, uri)
        name = next((x["name"] for x in spotify.playlists() if x["id"] == pid), "playlist") if ok else ""
        return {"ok": ok, "msg": ("Added to " + name)[:40] if ok else "Couldn't add the song"}

    def log_message(self, *args):
        pass


# =====================================================================
# Discovery beacon
# =====================================================================
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


# =====================================================================
# Tray app
# =====================================================================
import ctypes
import winreg

import pystray

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
APP_NAME = "DeskCYD"
tray_icon = None


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


def message_box(text):
    ctypes.windll.user32.MessageBoxW(None, text, "DeskCYD", 0x40)


def ask_text(title, prompt):
    """Small input box (tkinter ships with Python)."""
    import tkinter as tk
    from tkinter import simpledialog
    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    try:
        return simpledialog.askstring(title, prompt, parent=root)
    finally:
        root.destroy()


def status_text(item=None):
    with lock:
        if state["active"] and current_title:
            return ("Now: " + current_title + " - " + current_artist)[:60]
    return f"DeskCYD v{APP_VERSION} is running"


def spotify_label(item=None):
    return "Disconnect Spotify" if spotify.connected() else "Connect Spotify (for heart / playlists)..."


def spotify_menu(icon, item):
    if spotify.connected():
        spotify.logout()
        with lock:
            state["sp"] = False
            state["liked"] = -1
        refresh_menu()
        return

    def run():
        message_box(
            "One-time Spotify setup:\n\n"
            "1. Open developer.spotify.com/dashboard and log in.\n"
            "2. Create app - any name. Redirect URI:\n"
            f"     {spotify.redirect_uri()}\n"
            "   Tick 'Web API', save.\n"
            "3. Copy the app's Client ID.\n\n"
            "Click OK, then paste the Client ID in the next box.")
        cid = ask_text("DeskCYD - Spotify", "Paste your Spotify Client ID:")
        if not cid:
            return
        if spotify.login(cid.strip()):
            message_box("Spotify connected. The heart and playlist buttons now work.")
            spotify_refresh_track()
        else:
            message_box("Spotify login didn't finish. Check the Redirect URI in your Spotify app and try again.")
        refresh_menu()

    threading.Thread(target=run, daemon=True).start()


def app_update_label(item=None):
    return f"Update DeskCYD app to v{app_latest}" if app_latest > APP_VERSION else "DeskCYD app is up to date"


def app_update_click(icon, item):
    if app_latest > APP_VERSION:
        threading.Thread(target=update_app, daemon=True).start()
    else:
        threading.Thread(target=check_updates, daemon=True).start()


def toggle_autostart(icon, item):
    set_autostart(not autostart_enabled())


def quit_app(icon, item):
    icon.stop()
    os._exit(0)


def refresh_menu():
    if tray_icon is not None:
        try:
            tray_icon.update_menu()
        except Exception:
            pass


class SingleServer(ThreadingHTTPServer):
    allow_reuse_address = False  # on Windows, reuse would let two copies share the port


def main():
    global tray_icon
    try:
        server = SingleServer(("0.0.0.0", HTTP_PORT), Handler)
    except OSError:
        message_box("DeskCYD is already running (look for its icon in the system tray).")
        return

    first_run_marker = os.path.join(APP_DIR, "first_run_done")
    first_run = not os.path.exists(first_run_marker)
    if first_run:
        set_autostart(True)
        open(first_run_marker, "w").close()
    elif autostart_enabled():
        set_autostart(True)   # keep the path current after updates / moves

    threading.Thread(target=media_thread, daemon=True).start()
    threading.Thread(target=beacon_thread, daemon=True).start()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    threading.Thread(target=update_thread, daemon=True).start()
    if spotify.connected():
        with lock:
            state["sp"] = True
    print(f"DeskCYD v{APP_VERSION} running on port", HTTP_PORT, "IPs:", ", ".join(sorted(local_ips())))

    menu = pystray.Menu(
        pystray.MenuItem(status_text, None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem(spotify_label, spotify_menu),
        pystray.MenuItem(app_update_label, app_update_click),
        pystray.MenuItem("Start with Windows", toggle_autostart, checked=lambda item: autostart_enabled()),
        pystray.MenuItem("Quit DeskCYD", quit_app),
    )
    tray_icon = pystray.Icon(APP_NAME, make_icon(), "DeskCYD", menu)

    def on_ready(ic):
        ic.visible = True
        if first_run:
            try:
                ic.notify("DeskCYD is running. It lives here in the tray and starts with Windows.", "DeskCYD")
            except Exception:
                pass

    tray_icon.run(setup=on_ready)


if __name__ == "__main__":
    main()
