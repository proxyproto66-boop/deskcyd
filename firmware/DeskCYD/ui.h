// Screen layouts. Everything here only draws into a Canvas, so it has no hardware code.
// To change the look of a page, edit its render function and the layout numbers next to it;
// the touch code reads the same numbers, so buttons and their touch areas stay together.
#pragma once
#include "canvas.h"
#include "mapdata.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SCR_W 320
#define SCR_H 240
#define NUM_PAGES 3   // 0 = Now Playing, 1 = Map, 2 = Settings
#define SWIPE_H 72    // page-swipe zone: top ~0.5 inch (2.8" panel is ~143 px per inch)

// ---------------- colors ----------------
#define C_WHITE   0xFFFF
#define C_BLACK   0x0000
static const uint16_t C_SPOTIFY_GREEN = rgb(30, 215, 96);
static const uint16_t C_GREY_TEXT = rgb(179, 179, 179);
static const uint16_t C_DIM_TEXT = rgb(138, 138, 138);
static const uint16_t C_APPLE_CARD = rgb(28, 28, 30);
static const uint16_t C_APPLE_SEP = rgb(56, 56, 58);
static const uint16_t C_APPLE_GREEN = rgb(52, 199, 89);
static const uint16_t C_APPLE_BLUE = rgb(10, 132, 255);
static const uint16_t C_APPLE_GREY2 = rgb(142, 142, 147);
static const uint16_t C_APPLE_YELLOW = rgb(255, 214, 10);
static const uint16_t C_TOGGLE_OFF = rgb(57, 57, 61);
static const uint16_t C_PANEL = rgb(30, 30, 32);

// ---------------- Now Playing layout ----------------
#define TOP_H 26
#define ART_X 12
#define ART_Y 30
#define ART_S 140
#define TX 168
#define TW 140
#define T1_Y 32
#define T2_Y 56
#define AR_Y 84
#define AL_Y 106
#define HEART_CX (TX + 12)
#define ADD_CX (TX + 48)
#define ICON_CY 152
#define BAR_X 12
#define BAR_Y 182
#define BAR_W 296
#define BAR_H 4
#define CTRL_CY 220
#define PREV_CX 106
#define PLAY_CX 160
#define NEXT_CX 214
#define PLAY_R 18

// ---------------- Map page layout ----------------
#define WX_W 164          // weather panel
#define WX_Y 34
#define WX_H 198
#define WX_X_OPEN 8
#define TIMEBOX_R 312     // right edge of the time box
#define TIMEBOX_Y 6
#define TIMEBOX_H 44
#define UPDATE_PILL_X_MAP 8

// ---------------- Settings layout ----------------
#define SET_ROW_H 32
#define SET_CARD_X 10
#define SET_CARD_W 300
#define SET_TITLE_Y 30
#define SET_FIRST_Y 74    // first group header (content coordinates)

// ---------------- list screens (playlists, location picker) ----------------
#define PL_ROW_H 34
#define PL_TOP 58
#define PL_ROWS 5
#define LIST_INDEX_X 298  // A-Z index bar starts here
#define LIST_INDEX_MIN 30 // show the index bar for lists longer than this

struct TextImg {
  uint16_t *px = nullptr;  // RGB565 pixels from the PC
  int cap = 0;             // buffer capacity in pixels
  int w = 0, h = 0;
  bool scroll = false;     // wider than the space: scroll it
  int offset = 0;          // current scroll position in pixels
};

struct WxHour { int hour = 0; int code = 0; bool day = true; float t = 0; };
struct Weather {
  bool valid = false;
  char city[32] = "";
  float lat = 0, lon = 0;
  float t = 0, hi = 0, lo = 0;   // degrees C
  int code = 0;                  // WMO weather code
  bool day = true;
  int nh = 0;
  WxHour h[4];
};

struct UiState {
  int page = 0;
  // status (no Wi-Fi / no PC); empty = none
  char status[48] = "";
  char statusSub[64] = "";
  // now playing
  bool active = false, playing = false;
  int posMs = 0, durMs = 0;
  uint16_t bg = rgb(18, 18, 18);
  uint16_t *art = nullptr;
  bool artValid = false;
  TextImg t1, t2, ar, al;
  int liked = -1;          // -1 unknown / Spotify not connected
  bool spotify = false;
  bool scrubbing = false;
  int scrubMs = 0;
  int pressed = -1;        // 0 prev, 1 play, 2 next, 3 heart, 4 add
  // time
  bool timeValid = false;
  int64_t utc = 0;         // seconds (UTC)
  long tzoff = 0;          // seconds east of UTC
  // map
  float mapOffMin = 0;     // minutes the map is dragged away from "now"
  float wxPos = 0;         // weather panel: 0 = hidden, 1 = fully open
  Weather wx;
  // settings
  bool h24 = false, unitC = false, autoBri = false;
  bool locNear = false;
  char locName[32] = "Worcester";
  float locLat = 42.2626f, locLon = -71.8023f;
  int setScroll = 0;
  // device
  int fw = 0, fwLatest = 0, appVer = 0;
  bool fwReady = false;
  char ip[20] = "";
  char ssid[33] = "";
  bool invert = true, anim = true, flipped = false;
  int otaProgress = -1;    // -1 = no update running
  // playlist picker
  bool plOpen = false, plLoading = false;
  int plCount = 0, plScroll = 0;
  char plNames[24][36];
  // location picker
  bool locOpen = false, locLoading = false;
  int locLevel = 0;        // 0 regions, 1 countries, 2 states, 3 cities
  char locType = 'R';      // R region, C country, S state, Y city
  char locTitle[32] = "";
  char *locBuf = nullptr;  // lines from the PC: "name\tid" or "name\tlat\tlon"
  uint16_t *locOff = nullptr;
  int locCount = 0, locScroll = 0;
  // toast
  char toast[48] = "";
  bool toastOn = false;
};

static inline bool updateAvailable(const UiState &s) {
  return s.fwLatest > s.fw && s.fwReady;
}

static void fmtTime(char *out, int ms) {
  if (ms < 0) ms = 0;
  int sec = ms / 1000;
  sprintf(out, "%d:%02d", sec / 60, sec % 60);
}

static int tempOut(const UiState &s, float c) {   // degrees C -> shown number
  float v = s.unitC ? c : c * 9.0f / 5.0f + 32.0f;
  return (int)lroundf(v);
}

// clock text like "4:39" + "AM" (12 h) or "16:39" + "" (24 h)
static void fmtClock(const UiState &s, int hh, int mm, char *t, char *ampm) {
  if (s.h24) { sprintf(t, "%d:%02d", hh, mm); ampm[0] = 0; return; }
  int h12 = hh % 12;
  if (h12 == 0) h12 = 12;
  sprintf(t, "%d:%02d", h12, mm);
  strcpy(ampm, hh < 12 ? "AM" : "PM");
}

static void fmtHour(const UiState &s, int hh, char *out) {
  if (s.h24) { sprintf(out, "%02d", hh); return; }
  int h12 = hh % 12;
  if (h12 == 0) h12 = 12;
  sprintf(out, "%d%s", h12, hh < 12 ? "AM" : "PM");
}

// draws text followed by a degree ring; returns the total width
static int textDeg(Canvas &c, int x, int y, const char *s, const AAFont &f, uint16_t col) {
  int w = c.text(x, y, s, f, col);
  float r = f.lineH * 0.085f + 0.8f;
  c.ring(x + w + r + 1.2f, y + f.ascent * 0.28f + r, r + 0.6f, r * 0.55f + 0.5f, col);
  return w + (int)(2 * r + 3);
}
static int textDegWidth(Canvas &c, const char *s, const AAFont &f) {
  float r = f.lineH * 0.085f + 0.8f;
  return c.textWidth(s, f) + (int)(2 * r + 3);
}

// ---------------- shared pieces ----------------
static void drawTopStrip(Canvas &c, const UiState &s, uint16_t bg, bool light) {
  // page dots
  int n = NUM_PAGES, gap = 12;
  int x0 = SCR_W / 2 - (n - 1) * gap / 2;
  for (int i = 0; i < n; i++) {
    bool on = i == s.page;
    c.fillCircle(x0 + i * gap, 12, on ? 3.2f : 2.6f, on ? C_WHITE : mix565(bg, C_WHITE, 90));
  }
  if (updateAvailable(s)) {
    c.fillRoundRect(244, 4, 68, 17, 8.5f, light ? C_APPLE_BLUE : C_SPOTIFY_GREEN);
    c.textCentered(278, 6, "Update", F_SMALL, light ? C_WHITE : C_BLACK);
  }
}

static void drawToast(Canvas &c, const UiState &s) {
  if (!s.toastOn || !s.toast[0]) return;
  int w = c.textWidth(s.toast, F_BODY) + 32;
  int x = SCR_W / 2 - w / 2;
  c.fillRoundRect(x, 188, w, 34, 17, rgb(240, 240, 240));
  c.textCentered(SCR_W / 2, 196, s.toast, F_BODY, rgb(18, 18, 18));
}

static void drawChevron(Canvas &c, float x, float cy, bool left, uint16_t col) {
  float d = left ? -1 : 1;
  c.line(x - 3 * d, cy - 6, x + 3 * d, cy, 2.2f, col);
  c.line(x + 3 * d, cy, x - 3 * d, cy + 6, 2.2f, col);
}

static void drawCloseButton(Canvas &c) {
  c.fillCircle(296, 30, 14, rgb(40, 40, 40));
  c.line(291, 25, 301, 35, 2, C_WHITE);
  c.line(301, 25, 291, 35, 2, C_WHITE);
}

// ---------------- Now Playing (Spotify style) ----------------
static void drawHeart(Canvas &c, float cx, float cy, uint16_t fill, uint16_t bg, bool solid) {
  // two circles + a triangle make the heart
  c.fillCircle(cx - 5, cy - 3, 5.6f, fill);
  c.fillCircle(cx + 5, cy - 3, 5.6f, fill);
  c.fillTri(cx - 10.4f, cy - 1.2f, cx + 10.4f, cy - 1.2f, cx, cy + 9.5f, fill);
  if (!solid) {
    c.fillCircle(cx - 5, cy - 3, 3.8f, bg);
    c.fillCircle(cx + 5, cy - 3, 3.8f, bg);
    c.fillTri(cx - 8.2f, cy - 1.8f, cx + 8.2f, cy - 1.8f, cx, cy + 7.0f, bg);
  }
}

static void drawAddIcon(Canvas &c, float cx, float cy, uint16_t col) {
  c.ring(cx, cy, 10.5f, 1.8f, col);
  c.line(cx - 4.5f, cy, cx + 4.5f, cy, 1.8f, col);
  c.line(cx, cy - 4.5f, cx, cy + 4.5f, 1.8f, col);
}

static void drawTextImg(Canvas &c, int x, int y, const TextImg &t) {
  if (!t.px || t.w <= 0) return;
  if (t.scroll) {
    c.blitPart(x, y, t.px, t.w, t.h, t.offset % t.w, TW);
  } else {
    c.blit(x, y, t.px, t.w, t.h);
  }
}

static void renderNowPlaying(Canvas &c, const UiState &s) {
  uint16_t bg = s.bg;
  c.fillRect(0, 0, SCR_W, SCR_H, bg);
  drawTopStrip(c, s, bg, false);

  if (!s.active) {
    c.textCentered(SCR_W / 2, 92, "Nothing playing", F_TITLE, C_WHITE);
    c.textCentered(SCR_W / 2, 122, "Play something on your PC", F_BODY, C_GREY_TEXT);
    drawToast(c, s);
    return;
  }

  // album art
  if (s.artValid && s.art) c.blit(ART_X, ART_Y, s.art, ART_S, ART_S);
  else {
    c.fillRoundRect(ART_X, ART_Y, ART_S, ART_S, 6, mix565(bg, C_WHITE, 30));
    // music note
    c.fillCircle(ART_X + 60, ART_Y + 92, 11, mix565(bg, C_WHITE, 90));
    c.fillRect(ART_X + 68, ART_Y + 44, 4, 48, mix565(bg, C_WHITE, 90));
    c.line(ART_X + 70, ART_Y + 46, ART_X + 88, ART_Y + 56, 4, mix565(bg, C_WHITE, 90));
  }

  // title / artist / album (pictures rendered by the PC)
  drawTextImg(c, TX, T1_Y, s.t1);
  drawTextImg(c, TX, T2_Y, s.t2);
  int arY = s.t2.w > 0 ? AR_Y : T2_Y + 4;
  int alY = s.t2.w > 0 ? AL_Y : T2_Y + 26;
  drawTextImg(c, TX, arY, s.ar);
  drawTextImg(c, TX, alY, s.al);

  // heart + add to playlist
  uint16_t iconCol = s.spotify ? C_GREY_TEXT : mix565(bg, C_WHITE, 70);
  if (s.liked == 1) drawHeart(c, HEART_CX, ICON_CY, C_SPOTIFY_GREEN, bg, true);
  else drawHeart(c, HEART_CX, ICON_CY, s.pressed == 3 ? C_WHITE : iconCol, bg, false);
  drawAddIcon(c, ADD_CX, ICON_CY, s.pressed == 4 ? C_WHITE : iconCol);

  // progress bar
  int pos = s.scrubbing ? s.scrubMs : s.posMs;
  float frac = s.durMs > 0 ? (float)pos / s.durMs : 0;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  int fillW = (int)(BAR_W * frac + 0.5f);
  c.fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 2, mix565(bg, C_WHITE, 77));
  if (fillW > 0) c.fillRoundRect(BAR_X, BAR_Y, fillW < BAR_H ? BAR_H : fillW, BAR_H, 2, s.scrubbing ? C_SPOTIFY_GREEN : C_WHITE);
  if (s.scrubbing) c.fillCircle(BAR_X + fillW, BAR_Y + BAR_H / 2.0f, 6.5f, C_WHITE);
  char a[12], b[12];
  fmtTime(a, pos);
  if (s.durMs > 0) fmtTime(b, s.durMs); else strcpy(b, "--:--");
  c.text(BAR_X, BAR_Y + 9, a, F_SMALL, C_GREY_TEXT);
  c.textRight(BAR_X + BAR_W, BAR_Y + 9, b, F_SMALL, C_GREY_TEXT);

  // controls
  uint16_t pc = s.pressed == 0 ? C_GREY_TEXT : C_WHITE;
  c.fillRect(PREV_CX - 10, CTRL_CY - 9, 3, 18, pc);
  c.fillTri(PREV_CX + 9, CTRL_CY - 9, PREV_CX + 9, CTRL_CY + 9, PREV_CX - 6, CTRL_CY, pc);
  uint16_t nc = s.pressed == 2 ? C_GREY_TEXT : C_WHITE;
  c.fillTri(NEXT_CX - 9, CTRL_CY - 9, NEXT_CX - 9, CTRL_CY + 9, NEXT_CX + 6, CTRL_CY, nc);
  c.fillRect(NEXT_CX + 7, CTRL_CY - 9, 3, 18, nc);
  float r = s.pressed == 1 ? PLAY_R - 1.5f : PLAY_R;
  c.fillCircle(PLAY_CX, CTRL_CY, r, s.pressed == 1 ? C_GREY_TEXT : C_WHITE);
  if (s.playing) {
    c.fillRoundRect(PLAY_CX - 6, CTRL_CY - 7, 4, 14, 1, C_BLACK);
    c.fillRoundRect(PLAY_CX + 2, CTRL_CY - 7, 4, 14, 1, C_BLACK);
  } else {
    c.fillTri(PLAY_CX - 4.5f, CTRL_CY - 7.5f, PLAY_CX - 4.5f, CTRL_CY + 7.5f, PLAY_CX + 7.5f, CTRL_CY, C_BLACK);
  }
  drawToast(c, s);
}


static void renderPlaylists(Canvas &c, const UiState &s) {
  uint16_t bg = rgb(18, 18, 18);
  c.fillRect(0, 0, SCR_W, SCR_H, bg);
  c.text(16, 18, "Add to playlist", F_TITLE, C_WHITE);
  // close button
  c.fillCircle(296, 30, 14, rgb(40, 40, 40));
  c.line(291, 25, 301, 35, 2, C_WHITE);
  c.line(301, 25, 291, 35, 2, C_WHITE);
  if (s.plLoading) {
    c.textCentered(SCR_W / 2, 120, "Loading playlists...", F_BODY, C_GREY_TEXT);
  } else if (s.plCount == 0) {
    c.textCentered(SCR_W / 2, 110, "No playlists found", F_BODY, C_GREY_TEXT);
    c.textCentered(SCR_W / 2, 132, "Connect Spotify from the tray icon", F_SMALL, C_DIM_TEXT);
  } else {
    for (int i = 0; i < PL_ROWS; i++) {
      int idx = s.plScroll + i;
      if (idx >= s.plCount) break;
      int y = PL_TOP + i * PL_ROW_H;
      c.fillRoundRect(16, y + 4, 26, 26, 4, rgb(40, 40, 40));
      c.fillCircle(26, y + 22, 3.5f, C_GREY_TEXT);
      c.fillRect(28, y + 10, 2, 12, C_GREY_TEXT);
      c.text(54, y + 9, s.plNames[idx], F_BODY, C_WHITE);
    }
    if (s.plCount > PL_ROWS) {
      // scroll hint
      int trackH = PL_ROWS * PL_ROW_H - 8;
      int thumbH = trackH * PL_ROWS / s.plCount;
      int maxScroll = s.plCount - PL_ROWS;
      int ty = PL_TOP + 4 + (trackH - thumbH) * s.plScroll / (maxScroll > 0 ? maxScroll : 1);
      c.fillRoundRect(310, ty, 3, thumbH, 1.5f, rgb(90, 90, 90));
    }
  }
  drawToast(c, s);
}

// ---------------- weather icons and text ----------------
enum WxIcon { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_RAIN, WX_SNOW, WX_THUNDER };

static WxIcon wxIcon(int code) {
  if (code <= 0) return WX_CLEAR;
  if (code <= 2) return WX_PARTLY;
  if (code == 3) return WX_CLOUDY;
  if (code == 45 || code == 48) return WX_FOG;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  if (code >= 95) return WX_THUNDER;
  return WX_RAIN;
}

static const char *wxText(int code, bool day) {
  switch (code) {
    case 0: return day ? "Sunny" : "Clear";
    case 1: return day ? "Mostly Sunny" : "Mostly Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Cloudy";
    case 45: case 48: return "Foggy";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing Drizzle";
    case 61: return "Light Rain";
    case 63: return "Rain";
    case 65: return "Heavy Rain";
    case 66: case 67: return "Freezing Rain";
    case 71: return "Light Snow";
    case 73: return "Snow";
    case 75: return "Heavy Snow";
    case 77: return "Snow Grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy Showers";
    case 85: case 86: return "Snow Showers";
    case 95: case 96: case 99: return "Thunderstorms";
    default: return "";
  }
}

static void drawSun(Canvas &c, float cx, float cy, float u) {
  uint16_t col = rgb(255, 204, 0);
  c.fillCircle(cx, cy, 4.6f * u, col);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785398f;
    float ca = cosf(a), sa = sinf(a);
    c.line(cx + ca * 7.2f * u, cy + sa * 7.2f * u, cx + ca * 9.6f * u, cy + sa * 9.6f * u, 1.6f * u, col);
  }
}
static void drawMoon(Canvas &c, float cx, float cy, float u) {
  c.fillCircleCut(cx, cy, 7 * u, cx + 4.2f * u, cy - 3.2f * u, 5.6f * u, rgb(235, 235, 245));
}
static void drawCloud(Canvas &c, float cx, float cy, float u, uint16_t col) {
  c.fillCircle(cx - 4.5f * u, cy + 1 * u, 4.6f * u, col);
  c.fillCircle(cx + 0.5f * u, cy - 2 * u, 6.2f * u, col);
  c.fillCircle(cx + 5.8f * u, cy + 1.6f * u, 4.2f * u, col);
  c.fillRoundRect((int)(cx - 9.2f * u), (int)(cy + 0.5f * u), (int)(19.6f * u), (int)(5.6f * u), 2.8f * u, col);
}

static void drawWxIcon(Canvas &c, float cx, float cy, float size, int code, bool day) {
  float u = size / 24.0f;
  uint16_t white = rgb(242, 242, 247), grey = rgb(199, 199, 204);
  switch (wxIcon(code)) {
    case WX_CLEAR:
      if (day) drawSun(c, cx, cy, u); else drawMoon(c, cx, cy, u);
      break;
    case WX_PARTLY:
      if (day) drawSun(c, cx - 4 * u, cy - 4 * u, u * 0.8f); else drawMoon(c, cx - 4 * u, cy - 4 * u, u * 0.8f);
      drawCloud(c, cx + 2 * u, cy + 3 * u, u * 0.82f, white);
      break;
    case WX_CLOUDY:
      drawCloud(c, cx, cy + 1 * u, u, grey);
      break;
    case WX_FOG:
      for (int i = 0; i < 3; i++) {
        float y = cy - 5 * u + i * 5 * u, o = (i == 1) ? 2 * u : 0;
        c.line(cx - 9 * u + o, y, cx + 9 * u - (2 * u - o), y, 2 * u, grey);
      }
      break;
    case WX_RAIN:
      drawCloud(c, cx, cy - 3 * u, u, white);
      for (int i = 0; i < 3; i++)
        c.line(cx - 4.5f * u + i * 4.5f * u, cy + 6 * u, cx - 6 * u + i * 4.5f * u, cy + 10 * u, 1.7f * u, rgb(100, 210, 255));
      break;
    case WX_SNOW:
      drawCloud(c, cx, cy - 3 * u, u, white);
      for (int i = 0; i < 3; i++) c.fillCircle(cx - 4.5f * u + i * 4.5f * u, cy + 7.5f * u + (i == 1 ? 1.5f * u : 0), 1.5f * u, white);
      break;
    case WX_THUNDER:
      drawCloud(c, cx, cy - 3 * u, u, grey);
      c.fillTri(cx + 1.5f * u, cy + 2 * u, cx - 3.5f * u, cy + 8 * u, cx + 0.2f * u, cy + 8 * u, C_APPLE_YELLOW);
      c.fillTri(cx + 1.2f * u, cy + 6.5f * u, cx - 1.2f * u, cy + 12 * u, cx + 3.2f * u, cy + 6.5f * u, C_APPLE_YELLOW);
      break;
  }
}

// ---------------- map (day / night, drawn on the CYD) ----------------
// Each screen row is one latitude and each column one longitude, so the sun height at a
// pixel is rowA[y] + rowB[y] * colC[x]. Those tables are rebuilt when the time changes.
struct MapCache {
  int64_t t = -1;
  float rowA[MAP_H], rowB[MAP_H], colC[MAP_W];
};
static MapCache mapCache;
static uint8_t mapBlend[66];   // smooth day/night blend curve (0..255)

static void sunPosition(double t, double &decDeg, double &subLonDeg) {
  double n = (t - 946728000.0) / 86400.0;        // days since 2000-01-01 12:00 UTC
  double L = fmod(280.460 + 0.9856474 * n, 360.0);
  double g = fmod(357.528 + 0.9856003 * n, 360.0) * M_PI / 180.0;
  double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * M_PI / 180.0;
  double eps = (23.439 - 0.0000004 * n) * M_PI / 180.0;
  double ra = atan2(cos(eps) * sin(lam), cos(lam)) * 180.0 / M_PI;
  decDeg = asin(sin(eps) * sin(lam)) * 180.0 / M_PI;
  double gmst = fmod(280.46061837 + 360.98564736629 * n, 360.0);
  double lon = fmod(ra - gmst + 540.0, 360.0);
  if (lon < 0) lon += 360.0;
  subLonDeg = lon - 180.0;
}

// sin(sun height) where full night starts / full day starts
#define MAP_E0 (-0.1219f)   // sun 7 degrees below the horizon: full night, city lights on
#define MAP_E1 (0.0523f)    // sun 3 degrees above the horizon: full daylight

static void mapPrepare(int64_t t) {
  if (t == mapCache.t) return;
  mapCache.t = t;
  if (mapBlend[65] == 0) {
    for (int i = 0; i <= 65; i++) {
      float x = i / 64.0f;
      if (x > 1) x = 1;
      mapBlend[i] = (uint8_t)(255 * x * x * (3 - 2 * x));
    }
  }
  double dec, subLon;
  sunPosition((double)t, dec, subLon);
  float sd = sinf(dec * M_PI / 180.0), cd = cosf(dec * M_PI / 180.0);
  for (int y = 0; y < MAP_H; y++) {
    float lat = (MAP_LAT_N - (y + 0.5f) / MAP_PX_PER_DEG_LAT) * (float)M_PI / 180.0f;
    mapCache.rowA[y] = sinf(lat) * sd;
    mapCache.rowB[y] = cosf(lat) * cd;
  }
  for (int x = 0; x < MAP_W; x++) {
    float lon = MAP_LON_W + (x + 0.5f) / MAP_PX_PER_DEG_LON;
    mapCache.colC[x] = cosf((lon - (float)subLon) * (float)M_PI / 180.0f);
  }
}

static void mapLatLonToXY(float lat, float lon, float &x, float &y) {
  x = (lon - MAP_LON_W) * MAP_PX_PER_DEG_LON;
  y = (MAP_LAT_N - lat) * MAP_PX_PER_DEG_LAT;
}

static int64_t mapTime(const UiState &s) {
  return s.utc + (int64_t)(s.mapOffMin * 60.0f);
}

// fill the canvas area with the map
static void drawMapPixels(Canvas &c, const UiState &s) {
  bool timeOk = s.timeValid;
  if (timeOk) mapPrepare(mapTime(s));
  const float k = 64.0f / (MAP_E1 - MAP_E0);
  for (int y = c.oy; y < c.oy + c.h; y++) {
    const uint16_t *dRow = MAP_DAY + y * MAP_W;
    const uint16_t *nRow = MAP_NIGHT + y * MAP_W;
    uint16_t *p = c.buf + (y - c.oy) * c.w;
    if (!timeOk) {
      memcpy(p, dRow + c.ox, c.w * 2);
      continue;
    }
    float A = mapCache.rowA[y], B = mapCache.rowB[y];
    for (int x = c.ox; x < c.ox + c.w; x++) {
      float e = A + B * mapCache.colC[x];
      int i = (int)((e - MAP_E0) * k);
      if (i <= 0) *p++ = nRow[x];
      else if (i >= 64) *p++ = dRow[x];
      else *p++ = mix565(nRow[x], dRow[x], mapBlend[i]);
    }
  }
}

static void drawLocationDot(Canvas &c, const UiState &s) {
  float lat = s.locLat, lon = s.locLon;
  if (s.locNear) {
    if (!s.wx.valid) return;
    lat = s.wx.lat; lon = s.wx.lon;
  }
  float x, y;
  mapLatLonToXY(lat, lon, x, y);
  if (x < -10 || y < -10 || x > SCR_W + 10 || y > SCR_H + 10) return;
  c.fillCircle(x, y + 0.6f, 8.2f, C_BLACK, 70);
  c.fillCircle(x, y, 6.6f, C_WHITE);
  c.fillCircle(x, y, 4.6f, C_APPLE_BLUE);
}

static void drawTimeBox(Canvas &c, const UiState &s) {
  char t[12], ampm[4], line2[24];
  if (!s.timeValid) {
    strcpy(t, "--:--"); ampm[0] = 0; strcpy(line2, "Getting time");
  } else {
    time_t lt = (time_t)(mapTime(s) + s.tzoff);
    struct tm tmv;
    gmtime_r(&lt, &tmv);
    fmtClock(s, tmv.tm_hour, tmv.tm_min, t, ampm);
    int off = (int)lroundf(s.mapOffMin);
    if (off != 0) {
      int a = off < 0 ? -off : off;
      if (a >= 60 && a % 60) sprintf(line2, "%c%dh %dm", off < 0 ? '-' : '+', a / 60, a % 60);
      else if (a >= 60) sprintf(line2, "%c%d hr", off < 0 ? '-' : '+', a / 60);
      else sprintf(line2, "%c%d min", off < 0 ? '-' : '+', a);
    } else {
      static const char *D[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
      static const char *M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      sprintf(line2, "%s, %s %d", D[tmv.tm_wday], M[tmv.tm_mon], tmv.tm_mday);
    }
  }
  int wt = c.textWidth(t, F_TITLE) + (ampm[0] ? c.textWidth(ampm, F_SMALL) + 3 : 0);
  int w2 = c.textWidth(line2, F_SMALL);
  int w = (wt > w2 ? wt : w2) + 24;
  if (w < 92) w = 92;
  int x = TIMEBOX_R - w;
  c.fillRoundRect(x, TIMEBOX_Y, w, TIMEBOX_H, 12, C_BLACK, 120);
  int tx = x + (w - wt) / 2;
  tx += c.text(tx, TIMEBOX_Y + 3, t, F_TITLE, C_WHITE);
  if (ampm[0]) c.text(tx + 3, TIMEBOX_Y + 3 + F_TITLE.ascent - F_SMALL.ascent, ampm, F_SMALL, C_WHITE);
  bool off = s.timeValid && lroundf(s.mapOffMin) != 0;
  c.textCentered(x + w / 2, TIMEBOX_Y + 26, line2, F_SMALL, off ? C_APPLE_YELLOW : rgb(220, 220, 225));
}

static void drawMapTopStrip(Canvas &c, const UiState &s) {
  int n = NUM_PAGES, gap = 12;
  int x0 = SCR_W / 2 - (n - 1) * gap / 2;
  for (int i = 0; i < n; i++) {
    bool on = i == s.page;
    c.fillCircle(x0 + i * gap, 12, 4.6f, C_BLACK, 70);
    c.fillCircle(x0 + i * gap, 12, on ? 3.2f : 2.6f, C_WHITE, on ? 255 : 130);
  }
  if (updateAvailable(s)) {
    c.fillRoundRect(UPDATE_PILL_X_MAP, 4, 68, 17, 8.5f, C_APPLE_BLUE);
    c.textCentered(UPDATE_PILL_X_MAP + 34, 6, "Update", F_SMALL, C_WHITE);
  }
}

static float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

static int wxPanelX(const UiState &s) {
  return (int)(-WX_W + (WX_W + WX_X_OPEN) * easeOut(s.wxPos));
}

static void drawWeatherPanel(Canvas &c, const UiState &s) {
  if (s.wxPos <= 0) return;
  int px = wxPanelX(s), py = WX_Y;
  c.fillRoundRect(px, py, WX_W, WX_H, 16, C_PANEL, 165);
  int x = px + 14, inner = WX_W - 28;
  const Weather &w = s.wx;
  const char *city = (w.valid && w.city[0]) ? w.city : (s.locNear ? "Near my PC" : s.locName);
  c.textFit(x, py + 12, city, F_BODYB, C_WHITE, inner);
  if (!w.valid) {
    c.text(x, py + 46, "No weather yet", F_BODY, rgb(220, 220, 225));
    c.text(x, py + 68, "Needs the PC app", F_SMALL, C_APPLE_GREY2);
    return;
  }
  char buf[24];
  sprintf(buf, "%d", tempOut(s, w.t));
  textDeg(c, x - 1, py + 32, buf, F_TEMP, C_WHITE);
  drawWxIcon(c, px + WX_W - 30, py + 54, 28, w.code, w.day);
  c.textFit(x, py + 82, wxText(w.code, w.day), F_BODY, C_WHITE, inner);
  sprintf(buf, "H:%d", tempOut(s, w.hi));
  int hx = x + textDeg(c, x, py + 101, buf, F_BODY, rgb(220, 220, 225)) + 8;
  sprintf(buf, "L:%d", tempOut(s, w.lo));
  textDeg(c, hx, py + 101, buf, F_BODY, rgb(220, 220, 225));
  c.fillRect(x, py + 126, inner, 1, C_WHITE, 60);
  int cw = (WX_W - 16) / 4;
  for (int i = 0; i < w.nh && i < 4; i++) {
    int cx = px + 8 + cw * i + cw / 2;
    fmtHour(s, w.h[i].hour, buf);
    c.textCentered(cx, py + 134, buf, F_SMALL, rgb(220, 220, 225));
    drawWxIcon(c, cx, py + 162, 18, w.h[i].code, w.h[i].day);
    sprintf(buf, "%d", tempOut(s, w.h[i].t));
    int tw = textDegWidth(c, buf, F_BODY);
    textDeg(c, cx - tw / 2 + 2, py + 176, buf, F_BODY, C_WHITE);
  }
}

static void renderMap(Canvas &c, const UiState &s) {
  drawMapPixels(c, s);
  drawLocationDot(c, s);
  drawMapTopStrip(c, s);
  drawTimeBox(c, s);
  drawWeatherPanel(c, s);
  drawToast(c, s);
}

// ---------------- Settings (Apple style) ----------------
enum SetRow { SR_LOCATION, SR_UNITS, SR_24H, SR_LANG, SR_INVERT, SR_ANIM, SR_FLIP, SR_ABRI,
              SR_FIRMWARE, SR_APP, SR_WIFI, SR_COUNT };
struct SetGroup { const char *title; int first, count; };
static const SetGroup SET_GROUPS[] = {
  {"WEATHER & TIME", SR_LOCATION, 3},
  {"GENERAL", SR_LANG, 1},
  {"DISPLAY", SR_INVERT, 4},
  {"ABOUT", SR_FIRMWARE, 3},
};
#define SET_NGROUPS 4

// content-space y of the top of each group's card / each row
static int setGroupHeaderY(int g) {
  int y = SET_FIRST_Y;
  for (int i = 0; i < g; i++) y += 18 + SET_GROUPS[i].count * SET_ROW_H + 16;
  return y;
}
static int setRowY(int row) {
  for (int g = 0; g < SET_NGROUPS; g++) {
    const SetGroup &G = SET_GROUPS[g];
    if (row >= G.first && row < G.first + G.count) return setGroupHeaderY(g) + 18 + (row - G.first) * SET_ROW_H;
  }
  return 0;
}
static int setContentH() { return setGroupHeaderY(SET_NGROUPS) + 4; }
static int setMaxScroll() { int m = setContentH() - SCR_H; return m > 0 ? m : 0; }
// which row is at content y (or -1)
static int setRowAt(int cy) {
  for (int r = 0; r < SR_COUNT; r++) {
    int y = setRowY(r);
    if (cy >= y && cy < y + SET_ROW_H) return r;
  }
  return -1;
}

static void drawToggle(Canvas &c, int x, int y, bool on) {
  c.fillRoundRect(x, y, 40, 24, 12, on ? C_APPLE_GREEN : C_TOGGLE_OFF);
  c.fillCircle(on ? x + 28 : x + 12, y + 12, 10, C_WHITE);
}

static void drawDegLetter(Canvas &c, int cx, int y, const char *L, const AAFont &f, uint16_t col) {
  float r = f.lineH * 0.085f + 0.8f;
  int ringW = (int)(2 * r + 3);
  int x = cx - (ringW + c.textWidth(L, f)) / 2;
  c.ring(x + r + 0.6f, y + f.ascent * 0.28f + r, r + 0.6f, r * 0.55f + 0.5f, col);
  c.text(x + ringW, y, L, f, col);
}

static void drawSegment(Canvas &c, int x, int y, bool right) {
  c.fillRoundRect(x, y, 84, 24, 7, rgb(44, 44, 46));
  c.fillRoundRect(right ? x + 43 : x + 2, y + 2, 39, 20, 6, rgb(99, 99, 102));
  drawDegLetter(c, x + 22, y + 3, "F", F_BODYB, C_WHITE);
  drawDegLetter(c, x + 62, y + 3, "C", F_BODYB, C_WHITE);
}

static void textRightFit(Canvas &c, int rx, int y, const char *v, const AAFont &f, uint16_t col, int maxW) {
  int w = c.textWidth(v, f);
  if (w > maxW) w = maxW;
  c.textFit(rx - w, y, v, f, col, maxW);
}

static void drawSetRow(Canvas &c, const UiState &s, int row, int y) {
  char buf[48];
  const char *label = "";
  int ty = y + 8;
  switch (row) {
    case SR_LOCATION: label = "Location"; break;
    case SR_UNITS: label = "Temperature"; break;
    case SR_24H: label = "24-Hour Time"; break;
    case SR_LANG: label = "Language"; break;
    case SR_INVERT: label = "Invert colors"; break;
    case SR_ANIM: label = "Slide animation"; break;
    case SR_FLIP: label = "Upside down"; break;
    case SR_ABRI: label = "Auto-Brightness"; break;
    case SR_FIRMWARE: label = "Software"; break;
    case SR_APP: label = "PC app"; break;
    case SR_WIFI: label = "Wi-Fi"; break;
  }
  c.text(24, ty, label, F_BODY, C_WHITE);
  switch (row) {
    case SR_LOCATION: {
      textRightFit(c, 286, ty, s.locNear ? "Near my PC" : s.locName, F_BODY, C_APPLE_GREY2, 160);
      drawChevron(c, 298, y + 16, false, rgb(99, 99, 102));
      break;
    }
    case SR_UNITS: drawSegment(c, 216, y + 4, s.unitC); break;
    case SR_24H: drawToggle(c, 258, y + 4, s.h24); break;
    case SR_LANG: c.textRight(298, ty, "English", F_BODY, C_APPLE_GREY2); break;
    case SR_INVERT: drawToggle(c, 258, y + 4, s.invert); break;
    case SR_ANIM: drawToggle(c, 258, y + 4, s.anim); break;
    case SR_FLIP: drawToggle(c, 258, y + 4, s.flipped); break;
    case SR_ABRI: drawToggle(c, 258, y + 4, s.autoBri); break;
    case SR_FIRMWARE:
      if (s.otaProgress >= 0) {
        sprintf(buf, "Updating %d%%", s.otaProgress);
        c.textRight(298, ty, buf, F_BODY, C_APPLE_GREY2);
      } else if (updateAvailable(s)) {
        sprintf(buf, "Update to v%d", s.fwLatest);
        int w = c.textWidth(buf, F_BODYB) + 20;
        c.fillRoundRect(302 - w, y + 5, w, 22, 11, C_APPLE_BLUE);
        c.textRight(292, ty, buf, F_BODYB, C_WHITE);
      } else {
        sprintf(buf, "v%d  Up to date", s.fw);
        c.textRight(298, ty, buf, F_BODY, C_APPLE_GREY2);
      }
      break;
    case SR_APP:
      if (s.appVer > 0) sprintf(buf, "v%d", s.appVer); else strcpy(buf, "Not connected");
      c.textRight(298, ty, buf, F_BODY, C_APPLE_GREY2);
      break;
    case SR_WIFI:
      textRightFit(c, 298, ty, s.ssid, F_BODY, C_APPLE_GREY2, 180);
      break;
  }
}

static void renderSettings(Canvas &c, const UiState &s) {
  c.fillRect(c.ox, c.oy, c.w, c.h, C_BLACK);
  int off = -s.setScroll;
  c.text(16, SET_TITLE_Y + off, "Settings", F_LARGE, C_WHITE);
  for (int g = 0; g < SET_NGROUPS; g++) {
    const SetGroup &G = SET_GROUPS[g];
    int hy = setGroupHeaderY(g) + off;
    if (hy > SCR_H || hy + 18 + G.count * SET_ROW_H < 0) continue;
    c.text(24, hy + 2, G.title, F_SMALL, C_APPLE_GREY2);
    int cy = hy + 18;
    c.fillRoundRect(SET_CARD_X, cy, SET_CARD_W, G.count * SET_ROW_H, 12, C_APPLE_CARD);
    for (int i = 0; i < G.count; i++) {
      int y = cy + i * SET_ROW_H;
      drawSetRow(c, s, G.first + i, y);
      if (i < G.count - 1) c.fillRect(24, y + SET_ROW_H, SET_CARD_X + SET_CARD_W - 24, 1, C_APPLE_SEP);
    }
  }
  // fixed bar behind the page dots
  c.fillRect(0, 0, SCR_W, TOP_H, C_BLACK);
  if (s.setScroll > 4) c.fillRect(0, TOP_H - 1, SCR_W, 1, C_APPLE_SEP);
  drawTopStrip(c, s, C_BLACK, true);
  // scroll hint
  int maxS = setMaxScroll();
  if (s.setScroll > 0 && maxS > 0) {
    int trackH = SCR_H - TOP_H - 12, thumbH = trackH * SCR_H / setContentH();
    int ty = TOP_H + 6 + (trackH - thumbH) * s.setScroll / maxS;
    c.fillRoundRect(314, ty, 3, thumbH, 1.5f, rgb(90, 90, 90));
  }
  drawToast(c, s);
}

// ---------------- location picker ----------------
// copies the name of list item i into out (up to the first tab)
static const char *locItem(const UiState &s, int i, char *out, int outLen) {
  const char *p = s.locBuf + s.locOff[i];
  int n = 0;
  while (p[n] && p[n] != '\t' && p[n] != '\n' && n < outLen - 1) { out[n] = p[n]; n++; }
  out[n] = 0;
  const char *tab = strchr(p, '\t');
  return tab ? tab + 1 : "";
}

static void renderLocPicker(Canvas &c, const UiState &s) {
  c.fillRect(c.ox, c.oy, c.w, c.h, C_BLACK);
  int tx = 16;
  if (s.locLevel > 0) {
    drawChevron(c, 20, 30, true, C_APPLE_BLUE);
    tx = 34;
  }
  c.textFit(tx, 18, s.locTitle, F_TITLE, C_WHITE, 250 - tx);
  drawCloseButton(c);
  if (s.locLoading) {
    c.textCentered(SCR_W / 2, 120, "Loading...", F_BODY, C_APPLE_GREY2);
    return;
  }
  if (s.locCount == 0) {
    c.textCentered(SCR_W / 2, 110, "Nothing to show", F_BODY, C_APPLE_GREY2);
    c.textCentered(SCR_W / 2, 132, "Is the PC app running?", F_SMALL, C_DIM_TEXT);
    return;
  }
  bool index = s.locCount > LIST_INDEX_MIN;
  int rowRight = index ? LIST_INDEX_X - 6 : 304;
  char name[40];
  for (int i = 0; i < PL_ROWS + 1; i++) {
    int idx = s.locScroll + i;
    if (idx >= s.locCount) break;
    int y = PL_TOP + i * PL_ROW_H;
    const char *rest = locItem(s, idx, name, sizeof(name));
    bool near = rest[0] == '*';
    if (near) {
      // location arrow
      c.fillTri(18, y + 18, 32, y + 10, 25, y + 25, C_APPLE_BLUE);
      c.text(42, y + 9, name, F_BODY, C_APPLE_BLUE);
    } else {
      c.textFit(18, y + 9, name, F_BODY, C_WHITE, rowRight - 40);
    }
    if (s.locType != 'Y' && !near) drawChevron(c, rowRight - 8, y + 17, false, rgb(99, 99, 102));
    if (i < PL_ROWS && idx < s.locCount - 1) c.fillRect(18, y + PL_ROW_H - 1, rowRight - 18, 1, C_APPLE_SEP);
  }
  if (index) {
    static const char *L = "ACEGIKMOQSUWY";
    float step = (SCR_H - PL_TOP - 8) / 13.0f;
    for (int i = 0; i < 13; i++) {
      char t[2] = {L[i], 0};
      c.textCentered(LIST_INDEX_X + 10, PL_TOP + (int)(i * step), t, F_SMALL, C_APPLE_BLUE);
    }
  }
  drawToast(c, s);
}

// ---------------- status / update screens ----------------
static void renderStatus(Canvas &c, const UiState &s) {
  c.fillRect(c.ox, c.oy, c.w, c.h, C_BLACK);
  drawTopStrip(c, s, C_BLACK, true);
  c.textCentered(SCR_W / 2, 96, s.status, F_TITLE, C_WHITE);
  c.textCentered(SCR_W / 2, 128, s.statusSub, F_BODY, C_APPLE_GREY2);
  drawToast(c, s);
}

static void renderOta(Canvas &c, const UiState &s) {
  c.fillRect(c.ox, c.oy, c.w, c.h, C_BLACK);
  c.textCentered(SCR_W / 2, 84, "Updating DeskCYD", F_TITLE, C_WHITE);
  c.fillRoundRect(60, 124, 200, 6, 3, C_TOGGLE_OFF);
  int w = 200 * (s.otaProgress < 0 ? 0 : s.otaProgress) / 100;
  if (w > 0) c.fillRoundRect(60, 124, w < 6 ? 6 : w, 6, 3, C_WHITE);
  c.textCentered(SCR_W / 2, 146, "Keep it plugged in", F_BODY, C_APPLE_GREY2);
}

// Draw one page (used for page slides).
static void renderPage(Canvas &c, const UiState &s, int page) {
  if (page == 0) {
    if (s.status[0]) renderStatus(c, s);
    else renderNowPlaying(c, s);
  } else if (page == 1) renderMap(c, s);
  else renderSettings(c, s);
}

// Draw whatever should currently be on screen.
static void renderScreen(Canvas &c, const UiState &s) {
  if (s.otaProgress >= 0) { renderOta(c, s); return; }
  if (s.locOpen) { renderLocPicker(c, s); return; }
  if (s.plOpen) { renderPlaylists(c, s); return; }
  renderPage(c, s, s.page);
}
