/*
  DeskCYD - desk controller for the ESP32-2432S028 "Cheap Yellow Display"

  Screen + touch setup copied from cyd_scribble.ino (known to work on this board):
    Arduino_GFX ILI9341 on HSPI, rotation 1, XPT2046 touch on VSPI with fixed calibration.

  Pages (swipe left/right starting in the top half-inch of the screen):
    0 Now Playing (Spotify style)   1 USA day/night map + weather   2 Settings
  Map page: drag sideways to move time (the sun follows your finger), tap for the weather.
  First start: join the "DeskCYD-Setup" Wi-Fi from your phone.
  Hold BOOT for 5 seconds while it says "Starting..." to forget the saved Wi-Fi.
  Hold the album art for 2 seconds to turn the picture upside down.

  Built by GitHub Actions; fonts.h (Figtree font) and mapdata.h (NASA satellite pictures)
  are generated there.
  Libraries: GFX Library for Arduino, XPT2046_Touchscreen, ArduinoJson (v7), WiFiManager
*/

#ifndef FW_VERSION
#define FW_VERSION 0
#endif

// Leave empty to find the PC automatically. Or put its IP, e.g. "192.168.1.50"
#define PC_IP_MANUAL  ""
#define PC_PORT_MANUAL 8765

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>
#include "fonts.h"
#include "canvas.h"
#include "ui.h"

// ---------- Display (HSPI) - same as cyd_scribble ----------
#define GFX_BL 21
Arduino_DataBus *bus = new Arduino_ESP32SPI(2 /*DC*/, 15 /*CS*/, 14 /*SCK*/, 13 /*MOSI*/, 12 /*MISO*/, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED /*RST*/, 1 /*rotation: landscape*/);

// ---------- Touch (VSPI) - same as cyd_scribble ----------
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
const int RAW_X_MIN = 200, RAW_X_MAX = 3700;
const int RAW_Y_MIN = 240, RAW_Y_MAX = 3800;

// ---------- shared state ----------
Preferences prefs;
Canvas cv;                     // small off-screen buffer used for all drawing
UiState ui;                    // everything on screen (guarded by mtx)
SemaphoreHandle_t mtx;
#define LOCK() xSemaphoreTake(mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mtx)
SemaphoreHandle_t cmdMtx;   // separate lock for the command queue

// set by the network task, read by the main loop (under mtx)
struct NetFlags {
  bool full = false;        // redraw everything
  bool icons = false;       // heart / add icons
  bool top = false;         // top strip (update pill)
  bool pcOk = false;
  int netPosMs = 0;
  uint32_t netPosAt = 0;
  long epoch = 0, tzoff = 0;
  uint32_t epochAt = 0;
  char tid[16] = "", aid[16] = "";
  char plIds[24][40];
  bool toastNew = false;
  uint32_t wxNextAt = 0;    // when to fetch the weather next (0 = as soon as possible)
  bool wxNow = false;       // location changed: fetch now
  bool tzSaved = false;
} nf;

// location picker: one query per level ("", "r=NA", "r=NA&c=US", "r=NA&c=US&s=MA")
char locQuery[4][48];
#define LOC_BUF_SIZE 24576
#define LOC_MAX_ITEMS 1000

uint32_t playOverrideUntil = 0, posOverrideUntil = 0, likeOverrideUntil = 0;

// ---------- network ----------
#define DISCOVERY_PORT 47800
WiFiUDP udp;
IPAddress serverIP;
uint16_t serverPort = PC_PORT_MANUAL;
volatile bool haveServer = false;
int failCount = 0;

// command queue: main loop -> network task
#define CMDQ 8
char cmdQ[CMDQ][48];
int cmdHead = 0, cmdTail = 0;

void queueCmd(const char *c) {
  xSemaphoreTake(cmdMtx, portMAX_DELAY);
  int next = (cmdHead + 1) % CMDQ;
  if (next != cmdTail) {
    strncpy(cmdQ[cmdHead], c, 47);
    cmdQ[cmdHead][47] = 0;
    cmdHead = next;
  }
  xSemaphoreGive(cmdMtx);
}

bool popCmd(char *out) {
  bool ok = false;
  xSemaphoreTake(cmdMtx, portMAX_DELAY);
  if (cmdTail != cmdHead) {
    strcpy(out, cmdQ[cmdTail]);
    cmdTail = (cmdTail + 1) % CMDQ;
    ok = true;
  }
  xSemaphoreGive(cmdMtx);
  return ok;
}

String baseUrl() {
  return "http://" + serverIP.toString() + ":" + String(serverPort);
}

void showToast(const char *msg) {   // call with mtx held
  strncpy(ui.toast, msg, sizeof(ui.toast) - 1);
  ui.toast[sizeof(ui.toast) - 1] = 0;
  nf.toastNew = true;
}

// ================================================================
// Drawing
// ================================================================
void setScrollStart(int vsp) {
  bus->beginWrite();
  bus->writeC8D16(0x37, (uint16_t)vsp);   // VSCRSADD
  bus->endWrite();
}

void initScrollArea() {
  bus->beginWrite();
  bus->writeCommand(0x33);                // VSCRDEF: no fixed areas, 320 scrolling lines
  bus->write16(0);
  bus->write16(320);
  bus->write16(0);
  bus->endWrite();
  setScrollStart(0);
}

// Draw a screen rectangle through the off-screen buffer (call with mtx held).
void drawRegion(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCR_W) w = SCR_W - x;
  if (y + h > SCR_H) h = SCR_H - y;
  if (w <= 0 || h <= 0) return;
  int bh = cv.cap / w;
  if (bh > h) bh = h;
  for (int yy = y; yy < y + h; yy += bh) {
    int hh = min(bh, y + h - yy);
    cv.setArea(x, yy, w, hh);
    renderScreen(cv, ui);
    gfx->draw16bitRGBBitmap(x, yy, cv.buf, w, hh);
  }
}

void drawAll() { drawRegion(0, 0, SCR_W, SCR_H); }

float wxTarget = 0;          // weather panel: 0 closed, 1 open
void resetMapPage() {
  ui.mapOffMin = 0;
  ui.wxPos = 0;
  wxTarget = 0;
}

// Slide to another page using the screen's hardware scrolling (call with mtx held).
// dir = +1: the new page comes in from the right; -1: from the left.
void slideTo(int newPage, int dir) {
  if (newPage == ui.page) return;
  ui.plOpen = false;
  if (ui.page == 1) resetMapPage();
  if (!ui.anim || ui.otaProgress >= 0) {
    ui.page = newPage;
    drawAll();
    return;
  }
  ui.page = newPage;
  const int STRIP = 32;   // 32 x 240 pixels fits in the buffer
  const uint32_t DUR = 280;
  int drawn = 0;
  uint32_t t0 = millis();
  while (drawn < SCR_W) {
    float t = (millis() - t0) / (float)DUR;
    if (t > 1) t = 1;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease-out
    int target = (int)(e * SCR_W + 0.5f);
    if (target <= drawn) { delay(1); continue; }
    while (drawn < target) {
      int w = min(STRIP, target - drawn);
      int x0 = dir > 0 ? drawn : SCR_W - drawn - w;
      cv.setArea(x0, 0, w, SCR_H);
      renderPage(cv, ui, newPage);
      gfx->draw16bitRGBBitmap(x0, 0, cv.buf, w, SCR_H);
      drawn += w;
    }
    int S = dir > 0 ? drawn : SCR_W - drawn;      // content offset
    S %= SCR_W;
    int vsp = ui.flipped ? (SCR_W - S) % SCR_W : S;
    setScrollStart(vsp);
  }
  setScrollStart(0);
}

// ================================================================
// Network task (runs on the other CPU core so the screen stays smooth)
// ================================================================
void listenForBeacon() {
  int n = udp.parsePacket();
  while (n > 0) {
    char buf[32];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = 0;
      if (strncmp(buf, "DESKCYD ", 8) == 0) {
        IPAddress ip = udp.remoteIP();
        uint16_t port = atoi(buf + 8);
        if (!haveServer || ip != serverIP) Serial.printf("Found PC at %s:%u\n", ip.toString().c_str(), port);
        serverIP = ip;
        serverPort = port ? port : 8765;
        haveServer = true;
      }
    }
    n = udp.parsePacket();
  }
}

bool readExact(WiFiClient *s, uint8_t *dst, int n, uint32_t timeoutMs) {
  int got = 0;
  uint32_t start = millis();
  while (got < n && millis() - start < timeoutMs) {
    int a = s->available();
    if (a > 0) got += s->readBytes(dst + got, min(a, n - got));
    else delay(2);
  }
  return got == n;
}

bool skipBytes(WiFiClient *s, int n) {
  uint8_t tmp[256];
  while (n > 0) {
    int k = min(n, 256);
    if (!readExact(s, tmp, k, 2000)) return false;
    n -= k;
  }
  return true;
}

// Song title / artist / album pictures rendered by the PC.
bool fetchText() {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(3000);
  if (!http.begin(baseUrl() + "/api/text")) return false;
  if (http.GET() != 200) { http.end(); return false; }
  WiFiClient *s = http.getStreamPtr();
  TextImg *parts[4] = {&ui.t1, &ui.t2, &ui.ar, &ui.al};
  bool ok = true;
  LOCK();
  for (int i = 0; i < 4 && ok; i++) {
    uint8_t hdr[6];
    if (!readExact(s, hdr, 6, 2000)) { ok = false; break; }
    int w = hdr[0] | (hdr[1] << 8), h = hdr[2] | (hdr[3] << 8);
    bool scroll = hdr[4];
    TextImg &t = *parts[i];
    if (w * h > t.cap) {          // too big for the buffer: skip it
      ok = skipBytes(s, w * h * 2);
      t.w = 0;
      continue;
    }
    if (w * h > 0 && !readExact(s, (uint8_t *)t.px, w * h * 2, 4000)) { ok = false; t.w = 0; break; }
    t.w = w; t.h = h; t.scroll = scroll; t.offset = 0;
  }
  nf.full = true;
  UNLOCK();
  http.end();
  return ok;
}

bool fetchArt() {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(3000);
  if (!http.begin(baseUrl() + "/api/art")) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    LOCK(); ui.artValid = false; nf.full = true; UNLOCK();
    return code == 404;
  }
  WiFiClient *s = http.getStreamPtr();
  LOCK();
  bool ok = readExact(s, (uint8_t *)ui.art, ART_S * ART_S * 2, 5000);
  ui.artValid = ok;
  nf.full = true;
  UNLOCK();
  http.end();
  return ok;
}

void pollPC() {
  HTTPClient http;
  http.setConnectTimeout(800);
  http.setTimeout(1500);
  bool ok = false;
  String body;
  if (http.begin(baseUrl() + "/api/now?fw=" + String(FW_VERSION))) {
    if (http.GET() == 200) { body = http.getString(); ok = true; }
    http.end();
  }
  JsonDocument doc;
  if (ok && deserializeJson(doc, body)) ok = false;
  if (!ok) {
    if (++failCount >= 6) {
      failCount = 0;
      LOCK(); if (nf.pcOk) { nf.pcOk = false; nf.full = true; } UNLOCK();
      if (strlen(PC_IP_MANUAL) == 0) haveServer = false;
    }
    return;
  }
  failCount = 0;
  char tid[16], aid[16];
  strncpy(tid, doc["tid"] | "", 15); tid[15] = 0;
  strncpy(aid, doc["aid"] | "", 15); aid[15] = 0;
  bool needText, needArt;

  LOCK();
  uint32_t now = millis();
  if (!nf.pcOk) { nf.pcOk = true; nf.full = true; }
  bool active = doc["active"] | false;
  if (active != ui.active) { ui.active = active; nf.full = true; }
  if (now > playOverrideUntil) {
    bool playing = doc["playing"] | false;
    if (playing != ui.playing) { ui.playing = playing; nf.full = true; }
  }
  if (now > posOverrideUntil) {
    nf.netPosMs = doc["pos_ms"] | 0;
    nf.netPosAt = now;
  }
  ui.durMs = doc["dur_ms"] | 0;
  uint16_t bg = doc["bg"] | 0x18E3;
  if (bg != ui.bg) { ui.bg = bg; nf.full = true; }
  if (now > likeOverrideUntil) {
    int liked = doc["liked"] | -1;
    if (liked != ui.liked) { ui.liked = liked; nf.icons = true; }
  }
  bool sp = doc["sp"] | false;
  if (sp != ui.spotify) { ui.spotify = sp; nf.icons = true; }
  nf.epoch = doc["epoch"] | 0L;
  nf.tzoff = doc["tz"] | 0L;
  nf.epochAt = now;
  if (nf.tzoff != ui.tzoff) { ui.tzoff = nf.tzoff; nf.tzSaved = false; }
  int appVer = doc["app"] | 0, fwl = doc["fwl"] | 0;
  bool fwr = doc["fwr"] | false;
  if (appVer != ui.appVer || fwl != ui.fwLatest || fwr != ui.fwReady) {
    ui.appVer = appVer; ui.fwLatest = fwl; ui.fwReady = fwr;
    nf.top = true;
    if (ui.page != 0) nf.full = true;
  }
  needText = strcmp(tid, nf.tid) != 0;
  needArt = strcmp(aid, nf.aid) != 0;
  UNLOCK();

  if (needText && fetchText()) { LOCK(); strcpy(nf.tid, tid); UNLOCK(); }
  if (needArt) {
    if (aid[0] == 0) { LOCK(); ui.artValid = false; nf.full = true; strcpy(nf.aid, aid); UNLOCK(); }
    else if (fetchArt()) { LOCK(); strcpy(nf.aid, aid); UNLOCK(); }
  }
}

String httpGet(const String &path, int *code) {
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(6000);
  String body;
  *code = -1;
  if (http.begin(baseUrl() + path)) {
    *code = http.GET();
    if (*code > 0) body = http.getString();
    http.end();
  }
  return body;
}

// ---------- weather ----------
String urlEncode(const char *v) {
  String o;
  for (; *v; v++) {
    char ch = *v;
    if (isalnum((unsigned char)ch) || ch == '-' || ch == '.' || ch == '_') o += ch;
    else { char b[4]; sprintf(b, "%%%02X", (uint8_t)ch); o += b; }
  }
  return o;
}

bool fetchWeather() {
  String path;
  LOCK();
  if (ui.locNear) path = "/api/weather?near=1";
  else path = "/api/weather?lat=" + String(ui.locLat, 4) + "&lon=" + String(ui.locLon, 4) + "&name=" + urlEncode(ui.locName);
  UNLOCK();
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(12000);   // the PC may have to ask the weather service first
  bool ok = false;
  String body;
  if (http.begin(baseUrl() + path)) {
    if (http.GET() == 200) { body = http.getString(); ok = true; }
    http.end();
  }
  JsonDocument d;
  if (!ok || deserializeJson(d, body) || !(d["ok"] | false)) return false;
  LOCK();
  Weather &w = ui.wx;
  w.valid = true;
  strncpy(w.city, d["city"] | "", sizeof(w.city) - 1);
  w.city[sizeof(w.city) - 1] = 0;
  w.lat = d["lat"] | 0.0f;
  w.lon = d["lon"] | 0.0f;
  w.t = d["t"] | 0.0f;
  w.hi = d["hi"] | 0.0f;
  w.lo = d["lo"] | 0.0f;
  w.code = d["code"] | 0;
  w.day = (d["day"] | 1) != 0;
  w.nh = 0;
  for (JsonArray h : d["h"].as<JsonArray>()) {
    if (w.nh >= 4) break;
    w.h[w.nh].hour = h[0] | 0;
    w.h[w.nh].code = h[1] | 0;
    w.h[w.nh].day = (h[2] | 1) != 0;
    w.h[w.nh].t = h[3] | 0.0f;
    w.nh++;
  }
  if (ui.page == 1) nf.full = true;
  UNLOCK();
  return true;
}

// ---------- location picker lists ----------
// Reply: first line "<type>\t<title>", then one item per line.
void fetchLocList(const char *query) {
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(6000);
  bool ok = false;
  int len = 0;
  if (http.begin(baseUrl() + "/api/loc?" + query) && http.GET() == 200) {
    len = http.getSize();
    if (len > 0 && len < LOC_BUF_SIZE) {
      // the picker shows "Loading..." and doesn't touch the buffer until we're done
      ok = readExact(http.getStreamPtr(), (uint8_t *)ui.locBuf, len, 8000);
    }
  }
  http.end();
  LOCK();
  ui.locCount = 0;
  ui.locScroll = 0;
  if (ok) {
    ui.locBuf[len] = 0;
    char *nl = strchr(ui.locBuf, '\n');
    if (nl) {
      ui.locType = ui.locBuf[0];
      char *tab = strchr(ui.locBuf, '\t');
      int tl = (tab && tab < nl) ? (int)(nl - tab - 1) : 0;
      if (tl > (int)sizeof(ui.locTitle) - 1) tl = sizeof(ui.locTitle) - 1;
      memcpy(ui.locTitle, tab + 1, tl);
      ui.locTitle[tl] = 0;
      char *p = nl + 1;
      while (*p && ui.locCount < LOC_MAX_ITEMS) {
        ui.locOff[ui.locCount++] = (uint16_t)(p - ui.locBuf);
        char *e = strchr(p, '\n');
        if (!e) break;
        p = e + 1;
      }
    }
  } else {
    showToast("Couldn't load the list");
  }
  ui.locLoading = false;
  nf.full = true;
  UNLOCK();
}

void doOta() {
  LOCK(); ui.otaProgress = 0; nf.full = true; UNLOCK();
  HTTPClient http;
  http.setTimeout(15000);
  bool ok = false;
  if (http.begin(baseUrl() + "/api/firmware") && http.GET() == 200) {
    int len = http.getSize();
    if (Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) {
      WiFiClient *s = http.getStreamPtr();
      uint8_t buf[1024];
      int done = 0, lastPct = -1;
      uint32_t lastData = millis();
      while (len <= 0 || done < len) {
        int a = s->available();
        if (a > 0) {
          int r = s->readBytes(buf, min(a, (int)sizeof(buf)));
          Update.write(buf, r);
          done += r;
          lastData = millis();
          int pct = len > 0 ? (int)((int64_t)done * 100 / len) : 50;
          if (pct != lastPct) { lastPct = pct; LOCK(); ui.otaProgress = pct; nf.full = true; UNLOCK(); }
        } else {
          if (!http.connected() || millis() - lastData > 15000) break;
          delay(5);
        }
      }
      ok = Update.end(true);
    }
  }
  http.end();
  if (ok) {
    LOCK(); ui.otaProgress = 100; nf.full = true; UNLOCK();
    delay(800);
    ESP.restart();
  }
  LOCK(); ui.otaProgress = -1; showToast("Update failed - try again"); nf.full = true; UNLOCK();
}

void runCmd(const char *cmd) {
  int code;
  if (strncmp(cmd, "cmd:", 4) == 0) {
    httpGet(String("/api/cmd?c=") + (cmd + 4), &code);
  } else if (strncmp(cmd, "seek:", 5) == 0) {
    httpGet(String("/api/seek?ms=") + (cmd + 5), &code);
  } else if (strcmp(cmd, "like") == 0) {
    String body = httpGet("/api/like", &code);
    JsonDocument d;
    LOCK();
    if (code == 200 && !deserializeJson(d, body)) {
      ui.liked = (d["liked"] | false) ? 1 : 0;
      likeOverrideUntil = millis() + 3000;
      showToast(d["msg"] | "");
    } else {
      showToast("Couldn't reach Spotify");
    }
    nf.icons = true;
    UNLOCK();
  } else if (strcmp(cmd, "playlists") == 0) {
    String body = httpGet("/api/playlists", &code);
    JsonDocument d;
    LOCK();
    ui.plCount = 0;
    if (code == 200 && !deserializeJson(d, body)) {
      for (JsonObject p : d.as<JsonArray>()) {
        if (ui.plCount >= 24) break;
        strncpy(ui.plNames[ui.plCount], p["name"] | "", 35);
        ui.plNames[ui.plCount][35] = 0;
        strncpy(nf.plIds[ui.plCount], p["id"] | "", 39);
        nf.plIds[ui.plCount][39] = 0;
        ui.plCount++;
      }
    }
    ui.plLoading = false;
    nf.full = true;
    UNLOCK();
  } else if (strncmp(cmd, "add:", 4) == 0) {
    String body = httpGet(String("/api/addto?id=") + (cmd + 4), &code);
    JsonDocument d;
    LOCK();
    if (code == 200 && !deserializeJson(d, body)) showToast(d["msg"] | "Done");
    else showToast("Couldn't add the song");
    UNLOCK();
  } else if (strcmp(cmd, "ota") == 0) {
    doOta();
  } else if (strncmp(cmd, "loc:", 4) == 0) {
    fetchLocList(cmd + 4);
  }
}

void netTask(void *) {
  uint32_t lastPoll = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    listenForBeacon();
    if (!haveServer) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    char cmd[48];
    while (popCmd(cmd)) runCmd(cmd);
    if (millis() - lastPoll >= 400) {
      lastPoll = millis();
      pollPC();
    }
    if (nf.pcOk && (nf.wxNow || (int32_t)(millis() - nf.wxNextAt) >= 0)) {
      nf.wxNow = false;
      bool ok = fetchWeather();
      nf.wxNextAt = millis() + (ok ? 10 * 60000UL : 60000UL);   // every 10 min, retry after 1 min
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ================================================================
// Touch
// ================================================================
bool readTouch(int &x, int &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z <= 200) return false;   // ignore light/noisy touches
  x = constrain(map(p.x, RAW_X_MIN, RAW_X_MAX, 0, SCR_W - 1), 0, SCR_W - 1);
  y = constrain(map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, SCR_H - 1), 0, SCR_H - 1);
  if (ui.flipped) { x = SCR_W - 1 - x; y = SCR_H - 1 - y; }
  return true;
}

enum TouchKind { T_NONE, T_TOP, T_SCRUB, T_BUTTON, T_LIST, T_ART, T_MAP, T_SET, T_LOCLIST, T_LOCINDEX };
struct TouchState {
  bool down = false;
  uint32_t lastSeen = 0, t0 = 0;
  int x0 = 0, y0 = 0, x = 0, y = 0;
  TouchKind kind = T_NONE;
  bool moved = false;
  int listScroll0 = 0;
  bool longDone = false;
  bool swipeOk = false;   // touch started in the swipe zone
  float mapOff0 = 0;
  bool dragging = false;  // map time drag / settings scroll in progress
} tch;

// map time drag: the sun follows the finger (4 minutes of time per degree of longitude)
const float MAP_MIN_PER_PX = 4.0f / MAP_PX_PER_DEG_LON;
const float MAP_MAX_OFF_MIN = 12 * 60;
bool mapDirty = false;
uint32_t glideAt = 0, glideT0 = 0, wxOpenedAt = 0;
bool gliding = false;
float glideFrom = 0;

void applyRotation() {
  gfx->setRotation(ui.flipped ? 3 : 1);
}

int scrubFromX(int x) {
  float f = (x - BAR_X) / (float)BAR_W;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  return (int)(f * ui.durMs);
}

void redrawBar() { drawRegion(0, BAR_Y - 10, SCR_W, 32); }
void redrawControls() { drawRegion(0, CTRL_CY - 22, SCR_W, SCR_H - (CTRL_CY - 22)); }
void redrawIcons() { drawRegion(TX - 8, ICON_CY - 16, SCR_W - TX + 8, 34); }

void toggleWeather() {
  wxTarget = wxTarget > 0.5f ? 0 : 1;
  wxOpenedAt = millis();
}

// ---------- location picker ----------
void locRequest(int level) {
  ui.locLevel = level;
  ui.locLoading = true;
  ui.locCount = 0;
  char c[56];
  snprintf(c, sizeof(c), "loc:%s", locQuery[level]);
  queueCmd(c);
  drawAll();
}

void openLocPicker() {
  if (!nf.pcOk) { showToast("Needs the PC app running"); return; }
  if (!ui.locBuf) {
    ui.locBuf = (char *)malloc(LOC_BUF_SIZE);
    ui.locOff = (uint16_t *)malloc(LOC_MAX_ITEMS * sizeof(uint16_t));
    if (!ui.locBuf || !ui.locOff) {
      free(ui.locBuf); free(ui.locOff);
      ui.locBuf = nullptr; ui.locOff = nullptr;
      showToast("Not enough memory");
      return;
    }
  }
  ui.locOpen = true;
  strcpy(ui.locTitle, "Location");
  locQuery[0][0] = 0;
  locRequest(0);
}

void closeLocPicker() {
  ui.locOpen = false;
  drawAll();
}

void locPick(int idx) {
  if (idx < 0 || idx >= ui.locCount || ui.locLoading) return;
  char name[40];
  const char *rest = locItem(ui, idx, name, sizeof(name));
  if (rest[0] == '*') {                       // Near my PC
    ui.locNear = true;
    prefs.putBool("lnear", true);
  } else if (ui.locType == 'Y') {             // a city: "name\tlat\tlon"
    float lat = atof(rest);
    const char *t2 = strchr(rest, '\t');
    float lon = t2 ? atof(t2 + 1) : 0;
    ui.locNear = false;
    strncpy(ui.locName, name, sizeof(ui.locName) - 1);
    ui.locName[sizeof(ui.locName) - 1] = 0;
    ui.locLat = lat; ui.locLon = lon;
    prefs.putBool("lnear", false);
    prefs.putString("lname", ui.locName);
    prefs.putFloat("llat", lat);
    prefs.putFloat("llon", lon);
  } else {                                    // region / country / state: go one level down
    if (ui.locLevel >= 3) return;
    char id[12];
    int n = 0;
    while (rest[n] && rest[n] != '\t' && rest[n] != '\n' && n < 11) { id[n] = rest[n]; n++; }
    id[n] = 0;
    const char *key = ui.locType == 'R' ? "r" : ui.locType == 'C' ? "c" : "s";
    int L = ui.locLevel;
    snprintf(locQuery[L + 1], sizeof(locQuery[0]), "%s%s%s=%s", locQuery[L], locQuery[L][0] ? "&" : "", key, id);
    locRequest(L + 1);
    return;
  }
  ui.wx.valid = false;
  nf.wxNow = true;
  char msg[48];
  snprintf(msg, sizeof(msg), "Location: %s", ui.locNear ? "Near my PC" : ui.locName);
  showToast(msg);
  closeLocPicker();
}

void locJumpToLetter(int y) {
  int i = (int)((y - PL_TOP) * 26 / (SCR_H - PL_TOP - 8));
  if (i < 0) i = 0;
  if (i > 25) i = 25;
  char letter = 'A' + i;
  char name[40];
  int target = ui.locCount - 1;
  for (int k = 0; k < ui.locCount; k++) {
    locItem(ui, k, name, sizeof(name));
    if (toupper((unsigned char)name[0]) >= letter) { target = k; break; }
  }
  int maxScroll = max(0, ui.locCount - PL_ROWS);
  target = constrain(target, 0, maxScroll);
  if (target != ui.locScroll) { ui.locScroll = target; drawAll(); }
}

// ---------- settings ----------
void applyBacklight(int level) {
  ledcWrite(GFX_BL, level);
}

void settingsTap(int x, int y) {
  int row = setRowAt(y + ui.setScroll);
  switch (row) {
    case SR_LOCATION: openLocPicker(); return;
    case SR_UNITS: ui.unitC = !ui.unitC; prefs.putBool("unitC", ui.unitC); break;
    case SR_24H: ui.h24 = !ui.h24; prefs.putBool("h24", ui.h24); break;
    case SR_LANG: showToast("More languages coming later"); break;
    case SR_INVERT: ui.invert = !ui.invert; prefs.putBool("inv", ui.invert); gfx->invertDisplay(ui.invert); break;
    case SR_ANIM: ui.anim = !ui.anim; prefs.putBool("anim", ui.anim); break;
    case SR_FLIP: ui.flipped = !ui.flipped; prefs.putBool("flip", ui.flipped); applyRotation(); break;
    case SR_ABRI:
      ui.autoBri = !ui.autoBri;
      prefs.putBool("abri", ui.autoBri);
      if (!ui.autoBri) applyBacklight(255);
      break;
    case SR_FIRMWARE:
      if (updateAvailable(ui) && ui.otaProgress < 0) { queueCmd("ota"); return; }
      break;
    default: return;
  }
  drawAll();
}

// ---------- touch handling ----------
void onTouchDown(int x, int y) {
  tch.kind = T_NONE;
  tch.swipeOk = false;
  tch.dragging = false;
  if (ui.otaProgress >= 0) return;

  if (ui.locOpen) {
    if (x > 266 && y < 56) { closeLocPicker(); return; }
    if (x < 70 && y < 56 && ui.locLevel > 0 && !ui.locLoading) { locRequest(ui.locLevel - 1); return; }
    if (y >= PL_TOP && !ui.locLoading) {
      if (ui.locCount > LIST_INDEX_MIN && x >= LIST_INDEX_X - 6) { tch.kind = T_LOCINDEX; locJumpToLetter(y); }
      else { tch.kind = T_LOCLIST; tch.listScroll0 = ui.locScroll; }
    }
    return;
  }
  if (ui.plOpen) {
    if (x > 266 && y < 56) { ui.plOpen = false; drawAll(); return; }
    if (y >= PL_TOP) { tch.kind = T_LIST; tch.listScroll0 = ui.plScroll; }
    return;
  }
  // A sideways drag that starts in this zone changes page; a tap still goes to whatever is under the finger.
  tch.swipeOk = y < SWIPE_H;

  if (ui.page == 1) {
    wxOpenedAt = millis();          // touching the map keeps the weather panel open
    gliding = false;
    glideAt = 0;
    if (y < SWIPE_H) { tch.kind = T_TOP; return; }
    tch.kind = T_MAP;
    tch.mapOff0 = ui.mapOffMin;
    return;
  }
  if (ui.page == 2) {
    tch.kind = (y < TOP_H + 6) ? T_TOP : T_SET;
    tch.listScroll0 = ui.setScroll;
    return;
  }

  // page 0
  if (y < TOP_H + 6) { tch.kind = T_TOP; return; }
  if (ui.status[0]) return;
  if (ui.active) {
    if (y >= BAR_Y - 14 && y <= BAR_Y + 16 && ui.durMs > 0) {
      tch.kind = T_SCRUB;
      ui.scrubbing = true;
      ui.scrubMs = scrubFromX(x);
      redrawBar();
      return;
    }
    if (y >= CTRL_CY - 22) {
      int b = x < 133 ? 0 : (x < 187 ? 1 : 2);
      tch.kind = T_BUTTON;
      ui.pressed = b;
      if (b == 1) {
        ui.playing = !ui.playing;
        playOverrideUntil = millis() + 1500;
      }
      redrawControls();
      queueCmd(b == 0 ? "cmd:prev" : b == 1 ? "cmd:playpause" : "cmd:next");
      return;
    }
    if (y >= ICON_CY - 18 && y <= ICON_CY + 18 && x >= TX - 8 && x < TX + 68) {
      bool heart = x < TX + 30;
      tch.kind = T_BUTTON;
      ui.pressed = heart ? 3 : 4;
      if (!ui.spotify) {
        showToast("Connect Spotify in the tray app");
        nf.full = true;
        return;
      }
      if (heart) {
        ui.liked = ui.liked == 1 ? 0 : 1;
        likeOverrideUntil = millis() + 4000;
        queueCmd("like");
      } else {
        ui.plOpen = true;
        ui.plLoading = true;
        ui.plScroll = 0;
        queueCmd("playlists");
        drawAll();
        return;
      }
      redrawIcons();
      return;
    }
    if (x < ART_X + ART_S && y < ART_Y + ART_S) { tch.kind = T_ART; return; }
  }
}

void onTouchMove(int x, int y) {
  int dx = x - tch.x0, dy = y - tch.y0;
  if (abs(dx) > 8 || abs(dy) > 8) tch.moved = true;
  if (tch.swipeOk && tch.kind != T_SCRUB && abs(dx) > 40 && abs(dx) > abs(dy)) {
    int dir = dx < 0 ? 1 : -1;   // finger moves left -> next page
    int np = (ui.page + dir + NUM_PAGES) % NUM_PAGES;
    tch.swipeOk = false;
    tch.kind = T_NONE;
    slideTo(np, dir);
    return;
  }
  switch (tch.kind) {
    case T_SCRUB:
      ui.scrubMs = scrubFromX(x);
      redrawBar();
      break;
    case T_LIST: {
      int maxScroll = max(0, ui.plCount - PL_ROWS);
      int s = tch.listScroll0 - dy / PL_ROW_H;
      s = constrain(s, 0, maxScroll);
      if (s != ui.plScroll) { ui.plScroll = s; drawAll(); }
      break;
    }
    case T_LOCLIST: {
      int maxScroll = max(0, ui.locCount - PL_ROWS);
      int s = tch.listScroll0 - dy / (PL_ROW_H / 2);   // half-row steps: long lists scroll faster
      s = constrain(s, 0, maxScroll);
      if (s != ui.locScroll) { ui.locScroll = s; drawAll(); }
      break;
    }
    case T_LOCINDEX:
      locJumpToLetter(y);
      break;
    case T_MAP:
      if (tch.dragging || abs(dx) > 6) {
        tch.dragging = true;
        float off = tch.mapOff0 - dx * MAP_MIN_PER_PX;   // drag left = later
        if (off > MAP_MAX_OFF_MIN) off = MAP_MAX_OFF_MIN;
        if (off < -MAP_MAX_OFF_MIN) off = -MAP_MAX_OFF_MIN;
        if (off != ui.mapOffMin) { ui.mapOffMin = off; mapDirty = true; }
      }
      break;
    case T_SET:
      if (tch.dragging || abs(dy) > 6) {
        tch.dragging = true;
        int s = constrain(tch.listScroll0 - dy, 0, setMaxScroll());
        if (s != ui.setScroll) { ui.setScroll = s; drawAll(); }
      }
      break;
    case T_ART:
      if (!tch.moved && !tch.longDone && millis() - tch.t0 > 2000) {
        tch.longDone = true;
        ui.flipped = !ui.flipped;
        prefs.putBool("flip", ui.flipped);
        applyRotation();
        drawAll();
      }
      break;
    default:
      break;
  }
}

void onTouchUp() {
  int x = tch.x, y = tch.y;
  switch (tch.kind) {
    case T_TOP:
      if (tch.moved) break;
      if (updateAvailable(ui) && y < TOP_H) {
        // tap on the "Update" pill -> settings page
        bool hit = ui.page == 1 ? (x < UPDATE_PILL_X_MAP + 76) : (x > 240);
        if (hit && ui.page != 2) { slideTo(2, 1); break; }
      }
      if (ui.page == 1) toggleWeather();
      break;
    case T_SCRUB: {
      char c[24];
      sprintf(c, "seek:%d", ui.scrubMs);
      queueCmd(c);
      nf.netPosMs = ui.scrubMs;
      nf.netPosAt = millis();
      posOverrideUntil = millis() + 1500;
      ui.posMs = ui.scrubMs;
      ui.scrubbing = false;
      redrawBar();
      break;
    }
    case T_BUTTON:
      ui.pressed = -1;
      redrawControls();
      redrawIcons();
      break;
    case T_LIST:
      if (!tch.moved) {
        int idx = ui.plScroll + (tch.y0 - PL_TOP) / PL_ROW_H;
        if (idx >= 0 && idx < ui.plCount) {
          char c[48];
          snprintf(c, sizeof(c), "add:%s", nf.plIds[idx]);
          queueCmd(c);
          showToast("Adding...");
          ui.plOpen = false;
          drawAll();
        }
      }
      break;
    case T_LOCLIST:
      if (!tch.moved) locPick(ui.locScroll + (tch.y0 - PL_TOP) / PL_ROW_H);
      break;
    case T_MAP:
      if (!tch.dragging) toggleWeather();
      else glideAt = millis() + 5000;   // glide back to "now" after 5 seconds
      break;
    case T_SET:
      if (!tch.dragging && !tch.moved) settingsTap(tch.x0, tch.y0);
      break;
    default:
      break;
  }
  tch.kind = T_NONE;
  tch.dragging = false;
}

void handleTouch() {
  int x, y;
  bool t = readTouch(x, y);
  uint32_t now = millis();
  if (t) {
    tch.lastSeen = now;
    if (!tch.down) {
      tch.down = true;
      tch.moved = false;
      tch.longDone = false;
      tch.t0 = now;
      tch.x0 = tch.x = x;
      tch.y0 = tch.y = y;
      onTouchDown(x, y);
    } else {
      tch.x = x;
      tch.y = y;
      onTouchMove(x, y);
    }
  } else if (tch.down && now - tch.lastSeen > 60) {   // resistive touch drops out briefly; wait before "release"
    tch.down = false;
    onTouchUp();
  }
}

// ================================================================
// Animation / periodic updates (called with mtx held)
// ================================================================
uint32_t lastMarquee = 0, marqueePauseUntil = 0, toastUntil = 0;
int lastBarPx = -1, lastSec = -1;
uint32_t lastMapDraw = 0, lastWxStep = 0, lastBriAt = 0;
int64_t lastMapMinute = -1;
int prevStopped = -1;          // -1 = not known yet
uint32_t stoppedAt = 0;
float briSmooth = -1;

void updateStatus() {
  char st[48] = "", sub[64] = "";
  if (WiFi.status() != WL_CONNECTED) {
    strcpy(st, "Wi-Fi lost"); strcpy(sub, "Reconnecting...");
  } else if (!haveServer) {
    strcpy(st, "Looking for your PC..."); strcpy(sub, "Open DeskCYD on your PC");
  } else if (!nf.pcOk) {
    strcpy(st, "Connecting to your PC..."); strcpy(sub, "Is DeskCYD running on your PC?");
  }
  if (strcmp(st, ui.status) != 0 || strcmp(sub, ui.statusSub) != 0) {
    strcpy(ui.status, st);
    strcpy(ui.statusSub, sub);
    if (ui.page == 0) nf.full = true;
  }
}

// Current time in seconds (UTC): from the internet (NTP), or from the PC.
bool currentUtc(int64_t &out) {
  time_t t = time(nullptr);
  if (t > 1700000000) { out = t; return true; }
  if (nf.epoch > 0) { out = nf.epoch + (millis() - nf.epochAt) / 1000; return true; }
  return false;
}

// When music stops for 5 seconds: slide to the map. When it starts again: back to Now Playing.
void autoSwitch(uint32_t now) {
  if (!nf.pcOk) return;
  int stopped = (!ui.active || !ui.playing) ? 1 : 0;
  if (stopped != prevStopped) {
    if (stopped) stoppedAt = now ? now : 1;
    else {
      stoppedAt = 0;
      if (prevStopped == 1 && ui.page == 1 && !tch.down && !ui.locOpen) slideTo(0, -1);
    }
    prevStopped = stopped;
  }
  if (stoppedAt && now - stoppedAt > 5000) {
    stoppedAt = 0;
    if (ui.page == 0 && !tch.down && !ui.plOpen && !ui.locOpen) slideTo(1, 1);
  }
}

// Auto-brightness from the light sensor on GPIO 34 (off unless turned on in Settings).
void autoBrightness(uint32_t now) {
  if (!ui.autoBri || now - lastBriAt < 250) return;
  lastBriAt = now;
  int raw = analogRead(34);                   // on the CYD: more light -> lower reading
  float light = 1.0f - constrain(raw, 0, 1200) / 1200.0f;
  briSmooth = briSmooth < 0 ? light : briSmooth * 0.85f + light * 0.15f;
  applyBacklight(40 + (int)(215 * briSmooth));
}

void animateMap(uint32_t now) {
  int64_t utc;
  bool ok = currentUtc(utc);
  // weather panel slide
  if (ui.wxPos != wxTarget) {
    float step = (now - lastWxStep) / 300.0f;
    if (step > 0.25f) step = 0.25f;
    ui.wxPos += ui.wxPos < wxTarget ? step : -step;
    if (ui.wxPos > 1) ui.wxPos = 1;
    if (ui.wxPos < 0) ui.wxPos = 0;
    if (fabsf(ui.wxPos - wxTarget) < 0.02f) ui.wxPos = wxTarget;
    drawRegion(0, WX_Y - 2, WX_W + WX_X_OPEN + 4, WX_H + 4);
  }
  lastWxStep = now;
  if (wxTarget > 0.5f && !tch.down && now - wxOpenedAt > 10000) wxTarget = 0;   // hide after 10 s

  // glide back to "now" 5 s after a time drag
  if (!tch.dragging && ui.mapOffMin != 0 && glideAt && (int32_t)(now - glideAt) >= 0) {
    if (!gliding) { gliding = true; glideFrom = ui.mapOffMin; glideT0 = now; }
    float t = (now - glideT0) / 1200.0f;
    if (t >= 1) { ui.mapOffMin = 0; gliding = false; glideAt = 0; }
    else {
      float e = t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;   // ease in-out
      ui.mapOffMin = glideFrom * (1 - e);
    }
    mapDirty = true;
  }

  // redraw the whole map when dragged, every 30 s (the sun moves), and when the minute changes
  int64_t minute = ok ? (utc + ui.tzoff) / 60 : -1;
  if (mapDirty || minute != lastMapMinute || now - lastMapDraw > 30000 || ok != ui.timeValid) {
    mapDirty = false;
    lastMapMinute = minute;
    lastMapDraw = now;
    ui.timeValid = ok;
    if (ok) ui.utc = utc;
    drawAll();
  }
}

void animate() {
  uint32_t now = millis();

  // song position (smooth between updates from the PC)
  int pos = nf.netPosMs + (ui.playing ? (int)(now - nf.netPosAt) : 0);
  if (ui.durMs > 0 && pos > ui.durMs) pos = ui.durMs;
  ui.posMs = pos;

  // time (the map page sets it itself, so the whole screen uses one moment)
  if (ui.page != 1) {
    int64_t utc;
    ui.timeValid = currentUtc(utc);
    if (ui.timeValid) ui.utc = utc;
  }
  if (!nf.tzSaved && nf.epoch > 0) { nf.tzSaved = true; prefs.putInt("tz", (int)ui.tzoff); }

  autoBrightness(now);

  if (nf.full) {
    nf.full = false; nf.icons = false; nf.top = false;
    lastBarPx = -1;
    lastMapDraw = now;
    drawAll();
  }
  if (nf.toastNew) {
    nf.toastNew = false;
    ui.toastOn = ui.toast[0] != 0;
    toastUntil = now + 2500;
    drawAll();
  }
  if (ui.toastOn && now > toastUntil) { ui.toastOn = false; drawAll(); }
  if (ui.plOpen || ui.locOpen || ui.otaProgress >= 0) return;

  autoSwitch(now);
  if (nf.top) { nf.top = false; drawRegion(0, 0, SCR_W, TOP_H); }

  if (ui.page == 1) { animateMap(now); return; }

  if (ui.page == 0 && ui.active && !ui.status[0]) {
    if (nf.icons) { nf.icons = false; redrawIcons(); }
    // progress bar: redraw when it moves a pixel or the seconds change
    int show = ui.scrubbing ? ui.scrubMs : ui.posMs;
    int px = ui.durMs > 0 ? (int)((int64_t)BAR_W * show / ui.durMs) : 0;
    int sec = show / 1000;
    if (!ui.scrubbing && (px != lastBarPx || sec != lastSec)) {
      lastBarPx = px; lastSec = sec;
      redrawBar();
    }
    // scrolling title line 2 / artist
    if (now - lastMarquee >= 33 && now > marqueePauseUntil) {
      lastMarquee = now;
      TextImg *lines[2] = {&ui.t2, &ui.ar};
      int ys[2] = {T2_Y, AR_Y};
      for (int i = 0; i < 2; i++) {
        TextImg &t = *lines[i];
        if (!t.scroll || t.w <= 0) continue;
        t.offset = (t.offset + 1) % t.w;
        drawRegion(TX, ys[i], TW, t.h);
        if (t.offset == 0) marqueePauseUntil = now + 2000;
      }
    }
  }
}

// ================================================================
// Setup / loop
// ================================================================
WiFiManager wm;

void showSetupScreen(WiFiManager *) {
  LOCK();
  strcpy(ui.status, "Wi-Fi setup");
  strcpy(ui.statusSub, "On your phone, join DeskCYD-Setup");
  drawAll();
  UNLOCK();
}

void showStatusNow(const char *a, const char *b) {
  LOCK();
  strcpy(ui.status, a);
  strcpy(ui.statusSub, b);
  drawAll();
  UNLOCK();
}

// First 2 seconds after power-on: hold BOOT for 5 s to forget the saved Wi-Fi.
void bootMenu() {
  showStatusNow("Starting...", "Hold BOOT 5 s to forget Wi-Fi");
  for (int i = 0; i < 20; i++) {
    if (digitalRead(0) == LOW) {
      showStatusNow("Keep holding...", "Release to cancel");
      uint32_t t = millis();
      while (digitalRead(0) == LOW && millis() - t < 5000) delay(10);
      if (millis() - t >= 5000) {
        showStatusNow("Forgetting Wi-Fi...", "Release the BOOT button");
        wm.resetSettings();
        while (digitalRead(0) == LOW) delay(10);
        delay(1000);
      }
      return;
    }
    delay(100);
  }
}

void setupWiFi() {
  showStatusNow("Connecting to Wi-Fi", "");
  WiFi.mode(WIFI_STA);
  wm.setAPCallback(showSetupScreen);
  wm.setTitle("DeskCYD");
  wm.setConnectTimeout(20);
  if (!wm.autoConnect("DeskCYD-Setup")) ESP.restart();
  WiFi.setAutoReconnect(true);
  configTime(0, 0, "pool.ntp.org", "time.google.com");   // clock for the map, even without the PC
  Serial.print("Wi-Fi OK, my IP: ");
  Serial.println(WiFi.localIP());
  udp.begin(DISCOVERY_PORT);
}

bool allocText(TextImg &t, int pixels) {
  t.px = (uint16_t *)malloc(pixels * 2);
  t.cap = t.px ? pixels : 0;
  return t.px != nullptr;
}

void setup() {
  Serial.begin(115200);
  Serial.printf("DeskCYD firmware v%d\n", FW_VERSION);
  pinMode(0, INPUT_PULLUP);   // BOOT button
  mtx = xSemaphoreCreateMutex();
  cmdMtx = xSemaphoreCreateMutex();

  // fixed buffers, allocated once
  cv.alloc(32 * SCR_H);                 // 7680 pixels: 320x24 bands or 32x240 strips
  ui.art = (uint16_t *)malloc(ART_S * ART_S * 2);
  allocText(ui.t1, TW * 24);
  allocText(ui.t2, 480 * 24);
  allocText(ui.ar, 480 * 20);
  allocText(ui.al, TW * 18);
  Serial.printf("Free heap after buffers: %u\n", ESP.getFreeHeap());

  prefs.begin("deskcyd", false);
  ui.flipped = prefs.getBool("flip", false);
  ui.invert = prefs.getBool("inv", true);
  ui.anim = prefs.getBool("anim", true);
  ui.unitC = prefs.getBool("unitC", false);
  ui.h24 = prefs.getBool("h24", false);
  ui.autoBri = prefs.getBool("abri", false);
  ui.locNear = prefs.getBool("lnear", false);
  if (prefs.isKey("lname")) {
    prefs.getString("lname", ui.locName, sizeof(ui.locName));
    ui.locLat = prefs.getFloat("llat", ui.locLat);
    ui.locLon = prefs.getFloat("llon", ui.locLon);
  }
  ui.tzoff = prefs.getInt("tz", -5 * 3600);
  ui.fw = FW_VERSION;

  ledcAttach(GFX_BL, 5000, 8);   // backlight with brightness control
  applyBacklight(255);
  gfx->begin();
  applyRotation();
  gfx->invertDisplay(ui.invert);
  initScrollArea();

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  bootMenu();
  if (strlen(PC_IP_MANUAL) > 0) {
    serverIP.fromString(PC_IP_MANUAL);
    haveServer = true;
  }
  setupWiFi();

  LOCK();
  strncpy(ui.ssid, WiFi.SSID().c_str(), sizeof(ui.ssid) - 1);
  strncpy(ui.ip, WiFi.localIP().toString().c_str(), sizeof(ui.ip) - 1);
  ui.status[0] = 0;
  nf.full = true;
  UNLOCK();

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  LOCK();
  updateStatus();
  handleTouch();
  animate();
  UNLOCK();
  delay(5);
}
