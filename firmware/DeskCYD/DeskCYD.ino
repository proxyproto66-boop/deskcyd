/*
  DeskCYD - Stage 1 (Now Playing)
  For the ESP32-2432S028 "Cheap Yellow Display".

  Normally installed from the DeskCYD web page (built automatically by GitHub).
  Wi-Fi is set up on first boot: join the "DeskCYD-Setup" network from a phone.
  To forget the saved Wi-Fi, hold BOOT during the first 2 seconds after power-on.

  Libraries: LovyanGFX, XPT2046_Touchscreen, ArduinoJson (v7), WiFiManager (tzapu)
  Board: ESP32 Dev Module, Partition scheme: Minimal SPIFFS (with OTA)
*/

// ===================== SETTINGS =====================
// Leave empty to find the PC automatically. Or put its IP, e.g. "192.168.1.50"
#define PC_IP_MANUAL  ""
#define PC_PORT_MANUAL 8765

// Display fixes - only change these if the screen looks wrong:
#define PANEL_ST7789  0   // 1 if your CYD has TWO USB ports (USB-C + micro) and nothing shows
#define COLOR_INVERT  0   // 1 if colors look like a photo negative
#define SWAP_RED_BLUE 0   // 1 if red and blue are swapped
// =========================================================

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------------- display driver setup for the CYD ----------------
class LGFX : public lgfx::LGFX_Device {
#if PANEL_ST7789
  lgfx::Panel_ST7789 _panel;
#else
  lgfx::Panel_ILI9341 _panel;
#endif
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
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
      cfg.invert = COLOR_INVERT;
      cfg.rgb_order = SWAP_RED_BLUE;
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
    setPanel(&_panel);
  }
};

LGFX tft;

// ---------------- touch (separate SPI bus on the CYD) ----------------
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

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

// ---------------- layout ----------------
const int ART_X = 10, ART_Y = 10, ART_S = 150;
const int TXT_X = 172, TXT_W = 140;
const int BAR_X = 10, BAR_Y = 172, BAR_W = 300, BAR_H = 6;
const int BTN_Y = 200, BTN_H = 34, BTN_W = 90;
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
  tft.setTextColor(COL_TEXT);
  tft.drawString(msg, 160, 105);
  if (sub.length()) {
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(COL_SUB);
    tft.drawString(sub, 160, 140);
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
  tft.fillRect(BAR_X, BAR_Y + 9, BAR_W, 16, np.bg);
  tft.setTextColor(COL_SUB, np.bg);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(fmtTime(np.pos), BAR_X, BAR_Y + 9);
  tft.setTextDatum(textdatum_t::top_right);
  tft.drawString(np.dur > 0 ? fmtTime(np.dur) : String("--:--"), BAR_X + BAR_W, BAR_Y + 9);
  tft.setTextDatum(textdatum_t::top_left);
}

void drawText() {
  tft.fillRect(TXT_X, ART_Y, 320 - TXT_X, ART_S, np.bg);
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
unsigned long lastTap = 0;

void handleTouch() {
  bool touched = ts.tirqTouched() && ts.touched();
  if (!touched) { wasTouched = false; return; }
  if (wasTouched || millis() - lastTap < 250) return;
  wasTouched = true;
  lastTap = millis();

  TS_Point p = ts.getPoint();
  int x = map(p.x, 200, 3700, 1, 320);
  int y = map(p.y, 240, 3800, 1, 240);

  if (!nowPlayingOnScreen) return;
  if (y < BTN_Y - 6) return;

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
  tft.drawString("Wi-Fi setup", 16, 14);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_TEXT);
  tft.drawString("1. On your phone, join the Wi-Fi", 16, 56);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(0x07FF);
  tft.drawString("DeskCYD-Setup", 36, 80);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_TEXT);
  tft.drawString("2. A setup page opens. Tap", 16, 120);
  tft.drawString("   \"Configure WiFi\", pick your", 16, 142);
  tft.drawString("   network, enter the password.", 16, 164);
  tft.setTextColor(COL_SUB);
  tft.drawString("(If no page opens, go to 192.168.4.1)", 16, 200);
}

void setupWiFi() {
  // Hold BOOT (GPIO0) during the first 2 seconds to forget the saved network.
  pinMode(0, INPUT_PULLUP);
  showStatus("Starting...", "Hold BOOT now to reset Wi-Fi");
  bool reset = false;
  for (int i = 0; i < 20 && !reset; i++) { if (digitalRead(0) == LOW) reset = true; delay(100); }
  if (reset) {
    showStatus("Forgetting Wi-Fi...", "Release the BOOT button");
    wm.resetSettings();
    delay(1500);
  }
  showStatus("Connecting to Wi-Fi", "Hold BOOT at startup to reset");
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
  tft.init();
  tft.setRotation(1);   // landscape, USB on the right
  tft.setBrightness(200);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

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
