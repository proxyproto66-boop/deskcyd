/*
  DeskCYD - Now Playing, landscape
  For the ESP32-2432S028 "Cheap Yellow Display".

  Screen + touch setup copied from cyd_scribble.ino, which is known to work on this board:
    Arduino_GFX ILI9341 on HSPI, rotation 1, XPT2046 touch on VSPI with fixed calibration.

  First start: join the "DeskCYD-Setup" Wi-Fi from your phone to connect the CYD to your network.
  Hold BOOT for 5 seconds while it says "Starting..." to forget the saved Wi-Fi.
  Hold the album art for 2 seconds to turn the picture upside down.

  Libraries: GFX Library for Arduino (moononournation), XPT2046_Touchscreen (Paul Stoffregen),
             ArduinoJson (v7), WiFiManager (tzapu), JPEGDEC (Larry Bank)
  Board: ESP32 Dev Module, Partition scheme: Minimal SPIFFS (with OTA)
*/

// ===================== SETTINGS =====================
// Leave empty to find the PC automatically. Or put its IP, e.g. "192.168.1.50"
#define PC_IP_MANUAL  ""
#define PC_PORT_MANUAL 8765
// =========================================================

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <JPEGDEC.h>

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

const int SCR_W = 320, SCR_H = 240;

Preferences prefs;
JPEGDEC jpeg;
bool screenFlipped = false;   // picture turned 180 degrees

// ---------------- network ----------------
#define DISCOVERY_PORT 47800
WiFiUDP udp;
IPAddress serverIP;
uint16_t serverPort = PC_PORT_MANUAL;
bool haveServer = false;
int failCount = 0;

// ---------------- now playing state ----------------
struct NowPlaying {
  bool active = false;
  bool playing = false;
  String title, artist, album, app, art;
  int pos = 0, dur = 0;
  uint16_t bg = 0x18E3, btn = 0x3186, accent = 0xFFFF;
};
NowPlaying np;          // latest from PC
String drawnArt = "#";  // what's currently on screen
String drawnTitle = "#";
bool drawnPlaying = false;
uint16_t drawnBg = 0;
String statusShown = "";
bool nowPlayingOnScreen = false;

// ---------------- layout (landscape, 320 x 240) ----------------
const int ART_X = 10, ART_Y = 10, ART_S = 150;
const int TXT_X = 170, TXT_W = 142;
const int BAR_X = 10, BAR_Y = 170, BAR_W = 300, BAR_H = 6;
const int BTN_Y = 198, BTN_H = 36, BTN_W = 90;
const int BTN_PREV_X = 10, BTN_PLAY_X = 115, BTN_NEXT_X = 220;

const uint16_t COL_TEXT = 0xFFFF;
const uint16_t COL_SUB = 0xC618;

// =====================================================================
// Text helpers (built-in font: each character is 6 x 8 pixels times the text size)
// =====================================================================
// The built-in font only has plain ASCII, so replace other characters with '?'.
String toAscii(const String &s) {
  String out;
  for (int i = 0; i < (int)s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) out += (char)c;
    else if ((c & 0xC0) == 0xC0) out += '?';   // start of a multi-byte character
  }
  return out;
}

// Cut text to at most `maxChars` characters, adding "..." if it was longer.
String fitChars(const String &s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 3) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 3) + "...";
}

// Split into a first line of at most maxChars (breaking at a space) and the rest.
void splitLine(const String &s, int maxChars, String &line1, String &rest) {
  if ((int)s.length() <= maxChars) { line1 = s; rest = ""; return; }
  int cut = s.lastIndexOf(' ', maxChars);
  if (cut <= 0) cut = maxChars;
  line1 = s.substring(0, cut);
  rest = s.substring(cut);
  rest.trim();
}

void printAt(const String &s, int x, int y, int size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void printCentered(const String &s, int y, int size, uint16_t color) {
  String t = fitChars(s, (SCR_W - 8) / (6 * size));
  int w = t.length() * 6 * size;
  printAt(t, (SCR_W - w) / 2, y, size, color);
}

String fmtTime(int sec) {
  if (sec < 0) sec = 0;
  char b[12];
  snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
  return String(b);
}

// =====================================================================
// Screens
// =====================================================================
void showStatus(const String &msg, const String &sub) {
  String key = msg + "|" + sub;
  if (statusShown == key) return;
  statusShown = key;
  nowPlayingOnScreen = false;
  drawnArt = "#"; drawnTitle = "#";
  gfx->fillScreen(RGB565_BLACK);
  printCentered(msg, SCR_H / 2 - 20, 2, COL_TEXT);
  if (sub.length()) printCentered(sub, SCR_H / 2 + 8, 1, COL_SUB);
}

void drawArtPlaceholder() {
  gfx->fillRoundRect(ART_X, ART_Y, ART_S, ART_S, 8, np.btn);
  printAt("?", ART_X + ART_S / 2 - 12, ART_Y + ART_S / 2 - 16, 4, np.bg);
}

int jpegDrawCallback(JPEGDRAW *p) {
  gfx->draw16bitRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
  return 1;
}

bool fetchAndDrawArt() {
  HTTPClient http;
  String url = "http://" + serverIP.toString() + ":" + String(serverPort) + "/api/art";
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  int len = http.getSize();
  if (len <= 0 || len > 60000) { http.end(); return false; }
  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf) { http.end(); return false; }
  WiFiClient *stream = http.getStreamPtr();
  int got = 0;
  unsigned long start = millis();
  while (got < len && millis() - start < 3000) {
    int avail = stream->available();
    if (avail > 0) got += stream->readBytes(buf + got, min(avail, len - got));
    else delay(2);
  }
  http.end();
  bool ok = false;
  if (got == len && jpeg.openRAM(buf, len, jpegDrawCallback)) {
    ok = jpeg.decode(ART_X, ART_Y, 0);
    jpeg.close();
  }
  free(buf);
  return ok;
}

void drawButtonIcon(int which, uint16_t fill) {
  int x = which == 0 ? BTN_PREV_X : which == 1 ? BTN_PLAY_X : BTN_NEXT_X;
  gfx->fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 10, fill);
  int cx = x + BTN_W / 2, cy = BTN_Y + BTN_H / 2;
  uint16_t c = COL_TEXT;
  if (which == 0) {        // previous: bar + triangle
    gfx->fillRect(cx - 12, cy - 8, 3, 16, c);
    gfx->fillTriangle(cx + 8, cy - 9, cx + 8, cy + 9, cx - 8, cy, c);
  } else if (which == 2) { // next: triangle + bar
    gfx->fillTriangle(cx - 8, cy - 9, cx - 8, cy + 9, cx + 8, cy, c);
    gfx->fillRect(cx + 9, cy - 8, 3, 16, c);
  } else if (np.playing) { // pause
    gfx->fillRect(cx - 8, cy - 9, 5, 18, c);
    gfx->fillRect(cx + 3, cy - 9, 5, 18, c);
  } else {                 // play
    gfx->fillTriangle(cx - 6, cy - 10, cx - 6, cy + 10, cx + 10, cy, c);
  }
}

void drawProgress() {
  float frac = np.dur > 0 ? (float)np.pos / np.dur : 0;
  if (frac > 1) frac = 1;
  int fillW = (int)(BAR_W * frac);
  gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 3, np.btn);
  if (fillW > 0) gfx->fillRoundRect(BAR_X, BAR_Y, max(fillW, BAR_H), BAR_H, 3, np.accent);

  gfx->fillRect(BAR_X, BAR_Y + 10, BAR_W, 16, np.bg);
  String left = fmtTime(np.pos);
  String right = np.dur > 0 ? fmtTime(np.dur) : String("--:--");
  printAt(left, BAR_X, BAR_Y + 11, 1, COL_SUB);
  printAt(right, BAR_X + BAR_W - right.length() * 6, BAR_Y + 11, 1, COL_SUB);
}

void drawText() {
  gfx->fillRect(TXT_X, ART_Y, SCR_W - TXT_X, ART_S, np.bg);
  int y = ART_Y + 4;
  const int perLine2 = TXT_W / 12;   // size-2 characters per line
  const int perLine1 = TXT_W / 6;    // size-1 characters per line

  String title = toAscii(np.title.length() ? np.title : String("Unknown title"));
  String l1, rest;
  splitLine(title, perLine2, l1, rest);
  printAt(l1, TXT_X, y, 2, COL_TEXT); y += 20;
  if (rest.length()) {
    String l2, rest2;
    splitLine(rest, perLine2, l2, rest2);
    if (rest2.length()) l2 = fitChars(l2 + " " + rest2, perLine2);
    printAt(l2, TXT_X, y, 2, COL_TEXT); y += 20;
  }

  y += 8;
  if (np.artist.length()) { printAt(fitChars(toAscii(np.artist), perLine1), TXT_X, y, 1, COL_SUB); y += 14; }
  if (np.album.length())  { printAt(fitChars(toAscii(np.album), perLine1), TXT_X, y, 1, COL_SUB); y += 14; }

  // source app at the bottom of the text column
  printAt(fitChars(toAscii(np.app), perLine1), TXT_X, ART_Y + ART_S - 10, 1, np.accent);
}

void drawNowPlaying(bool full) {
  if (full) {
    gfx->fillScreen(np.bg);
    if (np.art.length() == 0 || !fetchAndDrawArt()) drawArtPlaceholder();
    drawText();
    for (int i = 0; i < 3; i++) drawButtonIcon(i, np.btn);
    drawnArt = np.art;
    drawnTitle = np.title + np.artist;
    drawnBg = np.bg;
    drawnPlaying = np.playing;
    nowPlayingOnScreen = true;
    statusShown = "";
  } else {
    if (drawnTitle != np.title + np.artist) { drawText(); drawnTitle = np.title + np.artist; }
    if (drawnPlaying != np.playing) { drawButtonIcon(1, np.btn); drawnPlaying = np.playing; }
  }
  drawProgress();
}

// =====================================================================
// Talking to the PC
// =====================================================================
String baseUrl() {
  return "http://" + serverIP.toString() + ":" + String(serverPort);
}

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
        if (!haveServer || ip != serverIP) {
          Serial.printf("Found PC at %s:%u\n", ip.toString().c_str(), port);
        }
        serverIP = ip;
        serverPort = port ? port : 8765;
        haveServer = true;
      }
    }
    n = udp.parsePacket();
  }
}

bool pollPC() {
  HTTPClient http;
  http.setConnectTimeout(800);
  http.setTimeout(1000);
  if (!http.begin(baseUrl() + "/api/now")) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  np.active  = doc["active"]  | false;
  np.playing = doc["playing"] | false;
  np.title   = (const char *)(doc["title"]  | "");
  np.artist  = (const char *)(doc["artist"] | "");
  np.album   = (const char *)(doc["album"]  | "");
  np.app     = (const char *)(doc["app"]    | "");
  np.art     = (const char *)(doc["art"]    | "");
  np.pos     = doc["pos"] | 0;
  np.dur     = doc["dur"] | 0;
  np.bg      = doc["bg"]     | 0x18E3;
  np.btn     = doc["btn"]    | 0x3186;
  np.accent  = doc["accent"] | 0xFFFF;
  return true;
}

void sendCommand(const char *cmd) {
  HTTPClient http;
  http.setConnectTimeout(800);
  http.setTimeout(3000);
  if (http.begin(baseUrl() + "/api/cmd?c=" + cmd)) {
    http.GET();
    http.end();
  }
}

// =====================================================================
// Touch (same reading and calibration as cyd_scribble)
// =====================================================================
bool readTouch(int &x, int &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z <= 200) return false;   // ignore light/noisy touches
  x = constrain(map(p.x, RAW_X_MIN, RAW_X_MAX, 0, SCR_W - 1), 0, SCR_W - 1);
  y = constrain(map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, SCR_H - 1), 0, SCR_H - 1);
  if (screenFlipped) { x = SCR_W - 1 - x; y = SCR_H - 1 - y; }
  return true;
}

bool wasTouched = false;
unsigned long touchStart = 0;
bool longPressDone = false;

void flipScreen() {
  screenFlipped = !screenFlipped;
  prefs.putBool("flip", screenFlipped);
  gfx->setRotation(screenFlipped ? 3 : 1);
  statusShown = "";
  nowPlayingOnScreen = false;   // forces a full redraw
}

void handleTouch() {
  int x, y;
  if (!readTouch(x, y)) { wasTouched = false; longPressDone = false; return; }

  if (!wasTouched) {
    wasTouched = true;
    touchStart = millis();
    if (!nowPlayingOnScreen || y < BTN_Y - 8) return;

    int which = -1;
    if (x >= BTN_PREV_X && x < BTN_PREV_X + BTN_W) which = 0;
    else if (x >= BTN_PLAY_X && x < BTN_PLAY_X + BTN_W) which = 1;
    else if (x >= BTN_NEXT_X && x < BTN_NEXT_X + BTN_W) which = 2;
    if (which < 0) return;

    drawButtonIcon(which, np.accent);  // flash for feedback
    sendCommand(which == 0 ? "prev" : which == 1 ? "playpause" : "next");
    if (which == 1) np.playing = !np.playing;  // instant feel
    drawButtonIcon(which, np.btn);
    drawnPlaying = np.playing;
    return;
  }

  // Hold the album art for 2 s to turn the picture upside down.
  if (!longPressDone && nowPlayingOnScreen && x < ART_X + ART_S && y < ART_Y + ART_S &&
      millis() - touchStart > 2000) {
    longPressDone = true;
    flipScreen();
  }
}

// =====================================================================
// Setup / loop
// =====================================================================
WiFiManager wm;

// Shown while the CYD is waiting for you to set up Wi-Fi from your phone.
void showSetupScreen(WiFiManager *) {
  statusShown = "setup";
  nowPlayingOnScreen = false;
  gfx->fillScreen(RGB565_BLACK);
  printAt("Wi-Fi setup", 16, 14, 3, RGB565_YELLOW);
  printAt("1. On your phone, join the Wi-Fi:", 16, 56, 1, COL_TEXT);
  printAt("DeskCYD-Setup", 16, 74, 3, RGB565_CYAN);
  printAt("2. A setup page opens. Tap", 16, 116, 1, COL_TEXT);
  printAt("   \"Configure WiFi\", pick your network", 16, 130, 1, COL_TEXT);
  printAt("   and enter its password.", 16, 144, 1, COL_TEXT);
  printAt("No page? Open 192.168.4.1 in a browser", 16, 190, 1, COL_SUB);
}

// First 2 seconds after power-on: hold BOOT for 5 s to forget the saved Wi-Fi.
void bootMenu() {
  showStatus("Starting...", "Hold BOOT 5 s to forget Wi-Fi");
  for (int i = 0; i < 20; i++) {
    if (digitalRead(0) == LOW) {
      showStatus("Keep holding...", "Release to cancel");
      unsigned long t = millis();
      while (digitalRead(0) == LOW && millis() - t < 5000) delay(10);
      if (millis() - t >= 5000) {
        showStatus("Forgetting Wi-Fi...", "Release the BOOT button");
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
  showStatus("Connecting to Wi-Fi", "");
  WiFi.mode(WIFI_STA);
  wm.setAPCallback(showSetupScreen);
  wm.setTitle("DeskCYD");
  wm.setConnectTimeout(20);
  if (!wm.autoConnect("DeskCYD-Setup")) {
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  Serial.print("Wi-Fi OK, my IP: ");
  Serial.println(WiFi.localIP());
  udp.begin(DISCOVERY_PORT);
}

void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);   // BOOT button

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->begin();
  prefs.begin("deskcyd", false);
  screenFlipped = prefs.getBool("flip", false);
  gfx->setRotation(screenFlipped ? 3 : 1);
  gfx->setTextWrap(false);
  gfx->fillScreen(RGB565_BLACK);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  bootMenu();

  if (strlen(PC_IP_MANUAL) > 0) {
    serverIP.fromString(PC_IP_MANUAL);
    haveServer = true;
  }
  setupWiFi();
}

unsigned long lastPoll = 0;

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("Wi-Fi lost", "Reconnecting...");
    delay(500);
    return;
  }

  listenForBeacon();
  if (!haveServer) {
    showStatus("Looking for your PC...", "Open DeskCYD on your PC");
    delay(20);
    return;
  }

  handleTouch();

  if (millis() - lastPoll >= 500) {
    lastPoll = millis();
    if (pollPC()) {
      failCount = 0;
      if (!np.active) {
        showStatus("Nothing playing", "Start some music on your PC");
      } else {
        bool full = !nowPlayingOnScreen || np.art != drawnArt || np.bg != drawnBg;
        drawNowPlaying(full);
      }
    } else if (++failCount >= 6) {
      failCount = 0;
      if (strlen(PC_IP_MANUAL) == 0) haveServer = false;  // wait for a new beacon
      showStatus("Lost connection to PC", "Is DeskCYD running on your PC?");
    }
  }
  delay(5);
}
