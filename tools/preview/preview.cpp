// Draws every page into PPM pictures on a PC, using the real layout code (ui.h).
// Built and run by the GitHub build; the pictures end up on the website under preview/.
#include "ui.h"
#include <stdio.h>

static void save(Canvas &c, const char *n) {
  FILE *f = fopen(n, "wb");
  fprintf(f, "P6 320 240 255\n");
  for (int i = 0; i < 320 * 240; i++) {
    uint16_t p = c.buf[i];
    unsigned char b[3] = {(unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3)};
    fwrite(b, 1, 3, f);
  }
  fclose(f);
}

static void mk(TextImg &t, const char *s, const AAFont &f, uint16_t fg, uint16_t bg, int h) {
  Canvas c;
  c.alloc(TW * h);
  c.setArea(0, 0, TW, h);
  c.fillRect(0, 0, TW, h, bg);
  c.text(0, (h - f.lineH) / 2, s, f, fg);
  t.px = c.buf; t.w = TW; t.h = h;
}

int main() {
  static UiState s;
  Canvas c;
  c.alloc(320 * 240);
  c.setArea(0, 0, 320, 240);

  s.active = true; s.playing = true; s.posMs = 83000; s.durMs = 200000; s.bg = rgb(48, 30, 40);
  mk(s.t1, "Blinding Lights", F_TITLE, 0xFFFF, s.bg, 24);
  mk(s.ar, "The Weeknd", F_BODY, C_GREY_TEXT, s.bg, 20);
  mk(s.al, "After Hours", F_SMALL, C_DIM_TEXT, s.bg, 18);
  s.liked = 1; s.spotify = true; s.fw = 19; s.fwLatest = 19; s.fwReady = true;
  renderScreen(c, s); save(c, "nowplaying.ppm");

  // map: Friday Nov 27 2026, 4:30 PM in Worcester (21:30 UTC)
  s.page = 1; s.timeValid = true; s.utc = 1795815000; s.tzoff = -5 * 3600;
  renderScreen(c, s); save(c, "map.ppm");

  Weather &w = s.wx;
  w.valid = true; strcpy(w.city, "Worcester"); w.lat = 42.26f; w.lon = -71.80f;
  w.t = 1.2f; w.hi = 4.0f; w.lo = -3.5f; w.code = 2; w.day = true; w.nh = 4;
  int hrs[4] = {17, 18, 19, 20}, codes[4] = {2, 3, 61, 71};
  float ts[4] = {0.5f, -0.4f, -1.0f, -1.8f};
  for (int i = 0; i < 4; i++) { w.h[i].hour = hrs[i]; w.h[i].code = codes[i]; w.h[i].day = i == 0; w.h[i].t = ts[i]; }
  s.wxPos = 1;
  renderScreen(c, s); save(c, "map-weather.ppm");

  s.wxPos = 0; s.mapOffMin = 200;
  renderScreen(c, s); save(c, "map-later.ppm");
  s.mapOffMin = -420;
  renderScreen(c, s); save(c, "map-earlier.ppm");
  s.mapOffMin = 0;

  s.page = 2; strcpy(s.ssid, "HomeWiFi"); s.appVer = 19;
  renderScreen(c, s); save(c, "settings.ppm");
  s.setScroll = setMaxScroll();
  renderScreen(c, s); save(c, "settings-bottom.ppm");

  // location picker (cities)
  static char buf[4096];
  static uint16_t off[64];
  const char *names[] = {"Agawam", "Amesbury", "Amherst Center", "Andover", "Arlington", "Attleboro", "Barnstable",
                         "Belmont", "Beverly", "Billerica", "Boston", "Braintree", "Brockton", "Brookline", "Burlington",
                         "Cambridge", "Chelmsford", "Chelsea", "Chicopee", "Concord", "Danvers", "Dedham", "Dracut",
                         "Everett", "Fall River", "Fitchburg", "Framingham", "Franklin", "Gloucester", "Haverhill",
                         "Holyoke", "Lawrence", "Leominster", "Lowell", "Worcester"};
  int n = sizeof(names) / sizeof(names[0]), pos = 0;
  for (int i = 0; i < n; i++) { off[i] = pos; pos += sprintf(buf + pos, "%s\t42.0\t-71.0\n", names[i]); }
  s.locOpen = true; s.locBuf = buf; s.locOff = off; s.locCount = n; s.locLevel = 3; s.locType = 'Y';
  strcpy(s.locTitle, "Massachusetts");
  renderScreen(c, s); save(c, "picker.ppm");
  return 0;
}
