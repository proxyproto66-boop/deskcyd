// Screen layouts. Everything here only draws into a Canvas, so it has no hardware code.
#pragma once
#include "canvas.h"
#include <stdio.h>
#include <string.h>

#define SCR_W 320
#define SCR_H 240
#define NUM_PAGES 3   // 0 = Now Playing, 1 = Clock, 2 = Device

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
static const uint16_t C_TOGGLE_OFF = rgb(57, 57, 61);

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

// ---------------- Device page layout ----------------
#define DEV_ROW_H 30
#define DEV_CARD1_Y 32
#define DEV_CARD2_Y 134
#define DEV_CARD_X 10
#define DEV_CARD_W 300

struct TextImg {
  uint16_t *px = nullptr;  // RGB565 pixels from the PC
  int cap = 0;             // buffer capacity in pixels
  int w = 0, h = 0;
  bool scroll = false;     // wider than the space: scroll it
  int offset = 0;          // current scroll position in pixels
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
  // clock
  int hh = 0, mm = 0, ss = 0;
  bool timeValid = false;
  char dateStr[40] = "";
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

// ---------------- playlist picker ----------------
#define PL_ROW_H 34
#define PL_TOP 58
#define PL_ROWS 5

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

// ---------------- Clock (Apple lock-screen style) ----------------
static void renderClock(Canvas &c, const UiState &s) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
  drawTopStrip(c, s, C_BLACK, true);
  if (!s.timeValid) {
    c.textCentered(SCR_W / 2, 110, "Waiting for the time...", F_BODY, C_APPLE_GREY2);
    return;
  }
  c.textCentered(SCR_W / 2, 48, s.dateStr, F_BODYB, rgb(235, 235, 245));
  char t[8];
  int h12 = s.hh % 12;
  if (h12 == 0) h12 = 12;
  sprintf(t, "%d:%02d", h12, s.mm);
  c.textCentered(SCR_W / 2, 74, t, F_HUGE, C_WHITE);
  c.textCentered(SCR_W / 2, 184, "Weather and more widgets arrive in the next update", F_SMALL, rgb(99, 99, 102));
}

// ---------------- Device (Apple Settings style) ----------------
static void drawToggle(Canvas &c, int x, int y, bool on) {
  c.fillRoundRect(x, y, 40, 24, 12, on ? C_APPLE_GREEN : C_TOGGLE_OFF);
  c.fillCircle(on ? x + 28 : x + 12, y + 12, 10, C_WHITE);
}

static void renderDevice(Canvas &c, const UiState &s) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
  drawTopStrip(c, s, C_BLACK, true);
  char buf[48];

  // card 1: software + network
  c.fillRoundRect(DEV_CARD_X, DEV_CARD1_Y, DEV_CARD_W, DEV_ROW_H * 3, 12, C_APPLE_CARD);
  int y = DEV_CARD1_Y;
  c.text(24, y + 7, "Firmware", F_BODY, C_WHITE);
  if (s.otaProgress >= 0) {
    sprintf(buf, "Updating %d%%", s.otaProgress);
    c.textRight(298, y + 7, buf, F_BODY, C_APPLE_GREY2);
  } else if (updateAvailable(s)) {
    sprintf(buf, "Update to v%d", s.fwLatest);
    int w = c.textWidth(buf, F_BODYB) + 20;
    c.fillRoundRect(302 - w, y + 4, w, 22, 11, C_APPLE_BLUE);
    c.textRight(292, y + 7, buf, F_BODYB, C_WHITE);
  } else {
    sprintf(buf, "v%d  Up to date", s.fw);
    c.textRight(298, y + 7, buf, F_BODY, C_APPLE_GREY2);
  }
  c.fillRect(24, y + DEV_ROW_H, 286, 1, C_APPLE_SEP);
  y += DEV_ROW_H;
  c.text(24, y + 7, "PC app", F_BODY, C_WHITE);
  if (s.appVer > 0) sprintf(buf, "v%d", s.appVer); else strcpy(buf, "Not connected");
  c.textRight(298, y + 7, buf, F_BODY, C_APPLE_GREY2);
  c.fillRect(24, y + DEV_ROW_H, 286, 1, C_APPLE_SEP);
  y += DEV_ROW_H;
  c.text(24, y + 7, "Wi-Fi", F_BODY, C_WHITE);
  snprintf(buf, sizeof(buf), "%s  %s", s.ssid, s.ip);
  c.textRight(298, y + 7, buf, F_SMALL, C_APPLE_GREY2);

  // card 2: display options
  c.fillRoundRect(DEV_CARD_X, DEV_CARD2_Y, DEV_CARD_W, DEV_ROW_H * 3, 12, C_APPLE_CARD);
  y = DEV_CARD2_Y;
  const char *labels[3] = {"Invert colors", "Slide animation", "Upside down"};
  bool vals[3] = {s.invert, s.anim, s.flipped};
  for (int i = 0; i < 3; i++) {
    c.text(24, y + 7, labels[i], F_BODY, C_WHITE);
    drawToggle(c, 258, y + 3, vals[i]);
    if (i < 2) c.fillRect(24, y + DEV_ROW_H, 286, 1, C_APPLE_SEP);
    y += DEV_ROW_H;
  }
}

// ---------------- status / update screens ----------------
static void renderStatus(Canvas &c, const UiState &s) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
  c.textCentered(SCR_W / 2, 96, s.status, F_TITLE, C_WHITE);
  c.textCentered(SCR_W / 2, 128, s.statusSub, F_BODY, C_APPLE_GREY2);
}

static void renderOta(Canvas &c, const UiState &s) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
  c.textCentered(SCR_W / 2, 84, "Updating DeskCYD", F_TITLE, C_WHITE);
  c.fillRoundRect(60, 124, 200, 6, 3, C_TOGGLE_OFF);
  int w = 200 * (s.otaProgress < 0 ? 0 : s.otaProgress) / 100;
  if (w > 0) c.fillRoundRect(60, 124, w < 6 ? 6 : w, 6, 3, C_WHITE);
  c.textCentered(SCR_W / 2, 146, "Keep it plugged in", F_BODY, C_APPLE_GREY2);
}

// Draw one page (used for page slides).
static void renderPage(Canvas &c, const UiState &s, int page) {
  if (page == 0) renderNowPlaying(c, s);
  else if (page == 1) renderClock(c, s);
  else renderDevice(c, s);
}

// Draw whatever should currently be on screen.
static void renderScreen(Canvas &c, const UiState &s) {
  if (s.otaProgress >= 0) { renderOta(c, s); return; }
  if (s.status[0]) { renderStatus(c, s); return; }
  if (s.plOpen) { renderPlaylists(c, s); return; }
  renderPage(c, s, s.page);
}
