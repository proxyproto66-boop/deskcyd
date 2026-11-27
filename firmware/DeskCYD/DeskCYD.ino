/*
  DeskCYD - Stage 1 (Now Playing), landscape
  For the ESP32-2432S028 "Cheap Yellow Display" (all versions).

  First start:
    1. Touch setup   - tap the arrow in each corner.
    2. Wi-Fi setup   - join the "DeskCYD-Setup" network from your phone.
  The screen type (original CYD or the USB-C / 2-USB version) is detected automatically.
  If the picture still looks wrong, use the screen setup below.

  During "Starting..." (first 2 seconds after power-on):
    - touch the screen          -> redo touch setup
    - press BOOT briefly        -> screen setup (press BOOT to step through options,
                                   HOLD BOOT 2 s to save)
    - hold BOOT for 5 seconds   -> forget Wi-Fi

  Hold the album art for 2 seconds to turn the picture upside down.

  Libraries: LovyanGFX, ArduinoJson (v7), WiFiManager (tzapu)
  Board: ESP32 Dev Module, Partition scheme: Minimal SPIFFS (with OTA)
*/

// ===================== SETTINGS =====================
// Leave empty to find the PC automatically. Or put its IP, e.g. "192.168.1.50"
#define PC_IP_MANUAL  ""
#define PC_PORT_MANUAL 8765
// =========================================================

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <SPI.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------------- display + touch driver setup for the CYD ----------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;   // HSPI
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();   // touch has its own SPI bus on the CYD
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3700;
      cfg.pin_int = 36;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI3_HOST;   // VSPI
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
Preferences prefs;

// ---------------- screen orientation / color settings ----------------
// Different CYD versions have their screen wired differently, so instead of guessing
// we write the screen's orientation register (MADCTL) directly and let the user pick.
// The drawing area is always 320 x 240 (landscape).
const uint8_t ORIENT_OPTIONS[8] = { 0x20, 0x60, 0xA0, 0xE0, 0x00, 0x40, 0x80, 0xC0 };
const int NUM_ORIENT = 8;
// color options: bit0 = BGR order, bit1 = inverted
const int NUM_COLOR = 4;
int orientIdx = 0;
int colorIdx = 1;           // BGR, not inverted = original CYD
bool screenFlipped = false; // picture turned 180 degrees

void applyScreenSettings() {
  uint8_t mad = ORIENT_OPTIONS[orientIdx];
  if (screenFlipped) mad ^= 0xC0;           // MX + MY = rotate 180
  if (colorIdx & 1) mad |= 0x08;            // BGR
  tft.startWrite();
  tft.writeCommand(0x36);                   // MADCTL
  tft.writeData(mad);
  tft.writeCommand((colorIdx & 2) ? 0x21 : 0x20);   // inversion on / off
  tft.endWrite();
}

// ---------------- 2-USB / USB-C CYD support ----------------
bool altPanel = false;   // true on USB-C / 2-USB CYDs

// Ask the screen chip for its ID before starting the display driver.
// The 2-USB panels answer 0x85 0x85 0x52; the original ILI9341 answers zeros to this command.
bool detectAltPanel() {
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
  pinMode(2, OUTPUT);
  SPIClass probe(HSPI);
  probe.begin(14, 12, 13, -1);
  probe.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(15, LOW);
  digitalWrite(2, LOW);
  probe.transfer(0x04);   // RDDID
  digitalWrite(2, HIGH);
  uint32_t raw = probe.transfer32(0);
  digitalWrite(15, HIGH);
  probe.endTransaction();
  probe.end();
  Serial.printf("Panel ID read: 0x%08X\n", raw);
  for (int sh = 0; sh <= 16; sh++) {
    if (((raw >> sh) & 0xFFFF) == 0x8585) return true;
  }
  return false;
}

// The 2-USB CYD needs a different start-up sequence (the "ILI9341_2" sequence that
// TFT_eSPI uses for this board). Sent after the normal driver init.
void sendCmd(uint8_t c, const uint8_t *d, int n) {
  tft.writeCommand(c);
  for (int i = 0; i < n; i++) tft.writeData(d[i]);
}

void initAltPanel() {
  static const uint8_t cf[] = {0x00, 0xC1, 0x30};
  static const uint8_t ed[] = {0x64, 0x03, 0x12, 0x81};
  static const uint8_t e8[] = {0x85, 0x00, 0x78};
  static const uint8_t cb[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
  static const uint8_t f7[] = {0x20};
  static const uint8_t ea[] = {0x00, 0x00};
  static const uint8_t c0[] = {0x10};
  static const uint8_t c1[] = {0x00};
  static const uint8_t c5[] = {0x30, 0x30};
  static const uint8_t c7[] = {0xB7};
  static const uint8_t px[] = {0x55};
  static const uint8_t b1[] = {0x00, 0x1A};
  static const uint8_t b6[] = {0x08, 0x82, 0x27};
  static const uint8_t f2[] = {0x00};
  static const uint8_t g26[] = {0x01};
  static const uint8_t e0[] = {0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9, 0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t e1[] = {0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56, 0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F};
  tft.startWrite();
  sendCmd(0xCF, cf, sizeof(cf));
  sendCmd(0xED, ed, sizeof(ed));
  sendCmd(0xE8, e8, sizeof(e8));
  sendCmd(0xCB, cb, sizeof(cb));
  sendCmd(0xF7, f7, sizeof(f7));
  sendCmd(0xEA, ea, sizeof(ea));
  sendCmd(0xC0, c0, sizeof(c0));
  sendCmd(0xC1, c1, sizeof(c1));
  sendCmd(0xC5, c5, sizeof(c5));
  sendCmd(0xC7, c7, sizeof(c7));
  sendCmd(0x3A, px, sizeof(px));
  sendCmd(0xB1, b1, sizeof(b1));
  sendCmd(0xB6, b6, sizeof(b6));
  sendCmd(0xF2, f2, sizeof(f2));
  sendCmd(0x26, g26, sizeof(g26));
  sendCmd(0xE0, e0, sizeof(e0));
  sendCmd(0xE1, e1, sizeof(e1));
  tft.writeCommand(0x11);   // sleep out
  tft.endWrite();
  delay(120);
  tft.startWrite();
  tft.writeCommand(0x29);   // display on
  tft.endWrite();
}

// ---------------- buttons ----------------
const int BOOT_PIN = 0;
bool bootDown() { return digitalRead(BOOT_PIN) == LOW; }

// Waits for a BOOT press. Returns 1 for a short press, 2 for a hold of 2 s or more.
int waitBootPress() {
  while (!bootDown()) delay(10);
  unsigned long t = millis();
  while (bootDown()) {
    if (millis() - t > 2000) {
      while (bootDown()) delay(10);
      return 2;
    }
    delay(10);
  }
  return 1;
}

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
const int SCR_W = 320, SCR_H = 240;
const int ART_X = 10, ART_Y = 10, ART_S = 150;
const int TXT_X = 172, TXT_W = 140;
const int BAR_X = 10, BAR_Y = 172, BAR_W = 300, BAR_H = 6;
const int BTN_Y = 198, BTN_H = 36, BTN_W = 90;
const int BTN_PREV_X = 10, BTN_PLAY_X = 115, BTN_NEXT_X = 220;

const uint16_t COL_TEXT = 0xFFFF;
const uint16_t COL_SUB = 0xC618;

// =====================================================================
// Text helpers
// =====================================================================
// Cut a UTF-8 string so it fits in maxW pixels, adding "..." if needed.
String fitText(const String &s, int maxW) {
  if (tft.textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() > 0 && tft.textWidth(t + "...") > maxW) {
    t.remove(t.length() - 1);
    // don't leave half of a multi-byte character
    while (t.length() > 0 && (((uint8_t)t[t.length() - 1]) & 0xC0) == 0x80) t.remove(t.length() - 1);
    if (t.length() > 0 && ((uint8_t)t[t.length() - 1]) >= 0xC0) t.remove(t.length() - 1);
  }
  return t + "...";
}

// Split text into a first line that fits (breaking at a space) and the rest.
void splitLine(const String &s, int maxW, String &line1, String &rest) {
  if (tft.textWidth(s) <= maxW) { line1 = s; rest = ""; return; }
  int cut = -1;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == ' ') {
      if (tft.textWidth(s.substring(0, i)) <= maxW) cut = i;
      else break;
    }
  }
  if (cut <= 0) { line1 = fitText(s, maxW); rest = ""; return; }
  line1 = s.substring(0, cut);
  rest = s.substring(cut + 1);
}

String fmtTime(int sec) {
  if (sec < 0) sec = 0;
  char b[12];
  snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
  return String(b);
}

// =====================================================================
// Screen setup (orientation + colors), driven by the BOOT button
// =====================================================================
void drawOrientationTest() {
  tft.fillScreen(0x0000);
  tft.drawRect(0, 0, SCR_W, SCR_H, 0xFFFF);           // frame around the whole screen
  tft.drawRect(1, 1, SCR_W - 2, SCR_H - 2, 0xFFFF);
  // arrow pointing up, on the left
  tft.fillTriangle(50, 30, 20, 80, 80, 80, 0xFFFF);
  tft.fillRect(40, 80, 20, 60, 0xFFFF);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(0xFFFF);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.drawString("Screen setup " + String(orientIdx + 1) + "/" + String(NUM_ORIENT), 100, 24);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.drawString("Is this text readable,", 100, 64);
  tft.drawString("the arrow pointing up,", 100, 86);
  tft.drawString("and the frame on all", 100, 108);
  tft.drawString("4 edges of the screen?", 100, 130);
  tft.drawString("No: press BOOT", 20, 172);
  tft.drawString("Yes: HOLD BOOT 2 seconds", 20, 200);
}

void drawColorTest() {
  tft.fillScreen(0x0000);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(0xFFFF);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.drawString("Colors " + String(colorIdx + 1) + "/" + String(NUM_COLOR), 16, 12);
  tft.fillRect(16, 50, 90, 70, 0xF800);
  tft.fillRect(115, 50, 90, 70, 0x07E0);
  tft.fillRect(214, 50, 90, 70, 0x001F);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.drawString("RED", 40, 126);
  tft.drawString("GREEN", 128, 126);
  tft.drawString("BLUE", 238, 126);
  tft.drawString("Black background, boxes match?", 16, 158);
  tft.drawString("No: press BOOT", 16, 184);
  tft.drawString("Yes: HOLD BOOT 2 seconds", 16, 208);
}

void runScreenSetup() {
  screenFlipped = false;
  // 1) orientation
  while (true) {
    applyScreenSettings();
    drawOrientationTest();
    if (waitBootPress() == 2) break;
    orientIdx = (orientIdx + 1) % NUM_ORIENT;
  }
  // 2) colors
  while (true) {
    applyScreenSettings();
    drawColorTest();
    if (waitBootPress() == 2) break;
    colorIdx = (colorIdx + 1) % NUM_COLOR;
  }
  prefs.putUChar("orient", orientIdx);
  prefs.putUChar("color", colorIdx);
  prefs.putBool("flip", false);
  prefs.remove("cal3");   // touch must be redone for the new orientation
  statusShown = "";
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
  tft.fillScreen(0x0000);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  if (tft.textWidth(msg) > SCR_W - 12) tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(COL_TEXT);
  tft.drawString(msg, SCR_W / 2, SCR_H / 2 - 16);
  if (sub.length()) {
    tft.setFont(&fonts::FreeSans9pt7b);
    if (tft.textWidth(sub) > SCR_W - 12) tft.setFont(&fonts::Font2);
    tft.setTextColor(COL_SUB);
    tft.drawString(sub, SCR_W / 2, SCR_H / 2 + 18);
  }
  tft.setTextDatum(textdatum_t::top_left);
}

void drawArtPlaceholder() {
  tft.fillRoundRect(ART_X, ART_Y, ART_S, ART_S, 8, np.btn);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextColor(np.bg);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("?", ART_X + ART_S / 2, ART_Y + ART_S / 2);
  tft.setTextDatum(textdatum_t::top_left);
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
  if (got == len) ok = tft.drawJpg(buf, len, ART_X, ART_Y);
  free(buf);
  return ok;
}

void drawButtonIcon(int which, uint16_t fill) {
  int x = which == 0 ? BTN_PREV_X : which == 1 ? BTN_PLAY_X : BTN_NEXT_X;
  tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 10, fill);
  int cx = x + BTN_W / 2, cy = BTN_Y + BTN_H / 2;
  uint16_t c = COL_TEXT;
  if (which == 0) {        // previous: bar + triangle
    tft.fillRect(cx - 12, cy - 8, 3, 16, c);
    tft.fillTriangle(cx + 8, cy - 9, cx + 8, cy + 9, cx - 8, cy, c);
  } else if (which == 2) { // next: triangle + bar
    tft.fillTriangle(cx - 8, cy - 9, cx - 8, cy + 9, cx + 8, cy, c);
    tft.fillRect(cx + 9, cy - 8, 3, 16, c);
  } else if (np.playing) { // pause
    tft.fillRect(cx - 8, cy - 9, 5, 18, c);
    tft.fillRect(cx + 3, cy - 9, 5, 18, c);
  } else {                 // play
    tft.fillTriangle(cx - 6, cy - 10, cx - 6, cy + 10, cx + 10, cy, c);
  }
}

void drawProgress() {
  float frac = np.dur > 0 ? (float)np.pos / np.dur : 0;
  if (frac > 1) frac = 1;
  int fillW = (int)(BAR_W * frac);
  tft.fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 3, np.btn);
  if (fillW > 0) tft.fillRoundRect(BAR_X, BAR_Y, max(fillW, BAR_H), BAR_H, 3, np.accent);

  tft.setFont(&fonts::Font2);
  tft.fillRect(BAR_X, BAR_Y + 8, BAR_W, 16, np.bg);
  tft.setTextColor(COL_SUB, np.bg);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(fmtTime(np.pos), BAR_X, BAR_Y + 8);
  tft.setTextDatum(textdatum_t::top_right);
  tft.drawString(np.dur > 0 ? fmtTime(np.dur) : String("--:--"), BAR_X + BAR_W, BAR_Y + 8);
  tft.setTextDatum(textdatum_t::top_left);
}

void drawText() {
  tft.fillRect(TXT_X, ART_Y, SCR_W - TXT_X, ART_S, np.bg);
  int y = ART_Y + 2;

  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(COL_TEXT);
  String l1, rest;
  splitLine(np.title.length() ? np.title : String("Unknown title"), TXT_W, l1, rest);
  tft.drawString(l1, TXT_X, y); y += 22;
  if (rest.length()) { tft.drawString(fitText(rest, TXT_W), TXT_X, y); y += 22; }

  y += 6;
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_SUB);
  if (np.artist.length()) { tft.drawString(fitText(np.artist, TXT_W), TXT_X, y); y += 22; }
  if (np.album.length())  { tft.drawString(fitText(np.album, TXT_W), TXT_X, y); y += 22; }

  // source app at the bottom of the text column
  tft.setFont(&fonts::Font2);
  tft.setTextColor(np.accent);
  tft.drawString(fitText(np.app, TXT_W), TXT_X, ART_Y + ART_S - 16);
}

void drawNowPlaying(bool full) {
  if (full) {
    tft.fillScreen(np.bg);
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
// Touch
// =====================================================================
bool wasTouched = false;
unsigned long touchStart = 0;
bool longPressDone = false;
unsigned long lastTap = 0;

// Reads a touch in screen coordinates (accounts for the 180-degree flip).
bool readTouch(int32_t &x, int32_t &y) {
  if (!tft.getTouch(&x, &y)) return false;
  if (screenFlipped) { x = SCR_W - 1 - x; y = SCR_H - 1 - y; }
  return true;
}

void flipScreen() {
  screenFlipped = !screenFlipped;
  prefs.putBool("flip", screenFlipped);
  applyScreenSettings();
  statusShown = "";
  nowPlayingOnScreen = false;   // forces a full redraw
}

void handleTouch() {
  int32_t x, y;
  bool touched = readTouch(x, y);
  if (!touched) { wasTouched = false; longPressDone = false; return; }

  if (!wasTouched) {
    wasTouched = true;
    touchStart = millis();
    if (millis() - lastTap < 200) return;
    lastTap = millis();
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

// One-time touch calibration: tap the arrows in each corner.
void calibrateTouch() {
  bool wasFlipped = screenFlipped;
  screenFlipped = false;          // calibrate in the normal orientation
  applyScreenSettings();
  uint16_t cal[8];
  tft.fillScreen(0x0000);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(COL_TEXT);
  tft.drawString("Touch setup", SCR_W / 2, SCR_H / 2 - 18);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_SUB);
  tft.drawString("Tap the arrow in each corner", SCR_W / 2, SCR_H / 2 + 14);
  tft.setTextDatum(textdatum_t::top_left);
  tft.calibrateTouch(cal, 0xFFFFu, 0x0000u, 22);
  tft.setTouchCalibrate(cal);
  prefs.putBytes("cal3", cal, sizeof(cal));
  int32_t tx, ty;
  while (tft.getTouch(&tx, &ty)) delay(10);   // wait for the finger to lift
  screenFlipped = wasFlipped;
  applyScreenSettings();
  statusShown = "";
}

// =====================================================================
// Setup / loop
// =====================================================================
WiFiManager wm;

// Shown while the CYD is waiting for you to set up Wi-Fi from your phone.
void showSetupScreen(WiFiManager *) {
  statusShown = "setup";
  nowPlayingOnScreen = false;
  tft.fillScreen(0x0000);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(0xFFE0);
  tft.drawString("Wi-Fi setup", 16, 12);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_TEXT);
  tft.drawString("1. On your phone, join the Wi-Fi", 16, 50);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(0x07FF);
  tft.drawString("DeskCYD-Setup", 36, 74);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_TEXT);
  tft.drawString("2. A setup page opens. Tap", 16, 114);
  tft.drawString("   Configure WiFi, pick your", 16, 136);
  tft.drawString("   network, enter the password.", 16, 158);
  tft.setTextColor(COL_SUB);
  tft.drawString("No page? Open 192.168.4.1", 16, 200);
}

// First 2 seconds after power-on: touch = redo touch setup, BOOT = screen setup,
// BOOT held 5 s = forget Wi-Fi.
void bootMenu() {
  showStatus("Starting...", "Touch: touch setup / BOOT: screen setup");
  for (int i = 0; i < 20; i++) {
    int32_t tx, ty;
    if (tft.getTouch(&tx, &ty)) { calibrateTouch(); return; }
    if (bootDown()) {
      showStatus("Release: screen setup", "Keep holding 5 s: forget Wi-Fi");
      unsigned long t = millis();
      while (bootDown() && millis() - t < 5000) delay(10);
      if (millis() - t >= 5000) {
        showStatus("Forgetting Wi-Fi...", "Release the BOOT button");
        wm.resetSettings();
        while (bootDown()) delay(10);
        delay(1000);
      } else {
        runScreenSetup();
        calibrateTouch();
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
  pinMode(BOOT_PIN, INPUT_PULLUP);

  altPanel = detectAltPanel();
  Serial.println(altPanel ? "Screen: 2-USB / USB-C CYD" : "Screen: original CYD");

  tft.init();
  if (altPanel) initAltPanel();
  tft.setRotation(1);       // 320 x 240 drawing area; the real orientation comes from MADCTL
  tft.setBrightness(200);

  // Settings are saved per screen type, so swapping boards starts from good defaults.
  prefs.begin(altPanel ? "deskcyd2" : "deskcyd1", false);
  orientIdx = prefs.getUChar("orient", 0) % NUM_ORIENT;            // 0x20 = landscape
  colorIdx = prefs.getUChar("color", altPanel ? 3 : 1) % NUM_COLOR; // 2-USB: BGR + inverted
  screenFlipped = prefs.getBool("flip", false);
  applyScreenSettings();
  tft.fillScreen(0x0000);

  uint16_t cal[8];
  if (prefs.getBytes("cal3", cal, sizeof(cal)) == sizeof(cal)) tft.setTouchCalibrate(cal);
  else calibrateTouch();

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
