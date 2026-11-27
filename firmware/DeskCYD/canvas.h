// Small off-screen drawing helper. Everything is drawn into a buffer first and then
// pushed to the screen in one go, which removes flicker. Shapes and text are anti-aliased.
#pragma once
#include <Arduino.h>
#include <math.h>
#include "fonts.h"

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Mix two RGB565 colors. a = 0..255 (amount of fg).
static inline uint16_t mix565(uint16_t bg, uint16_t fg, uint8_t a) {
  uint32_t a5 = (a + 4) >> 3;  // 0..32
  uint32_t rb = (((fg & 0xF81F) * a5) + ((bg & 0xF81F) * (32 - a5))) >> 5;
  uint32_t g = (((fg & 0x07E0) * a5) + ((bg & 0x07E0) * (32 - a5))) >> 5;
  return (rb & 0xF81F) | (g & 0x07E0);
}

struct Canvas {
  uint16_t *buf = nullptr;
  int cap = 0;           // buffer size in pixels
  int w = 0, h = 0;      // current size
  int ox = 0, oy = 0;    // screen position of the canvas' top-left pixel

  bool alloc(int pixels) {
    buf = (uint16_t *)malloc(pixels * 2);
    cap = buf ? pixels : 0;
    return buf != nullptr;
  }
  bool setArea(int x, int y, int W, int H) {
    if (W * H > cap) return false;
    ox = x; oy = y; w = W; h = H;
    return true;
  }

  // clip a screen rectangle to this canvas; returns false if nothing is visible
  inline bool clip(int &x0, int &y0, int &x1, int &y1) const {
    if (x0 < ox) x0 = ox;
    if (y0 < oy) y0 = oy;
    if (x1 > ox + w) x1 = ox + w;
    if (y1 > oy + h) y1 = oy + h;
    return x0 < x1 && y0 < y1;
  }
  inline uint16_t *at(int x, int y) { return buf + (y - oy) * w + (x - ox); }
  inline void blendPx(int x, int y, uint16_t c, uint8_t a) {
    if (x < ox || y < oy || x >= ox + w || y >= oy + h || a == 0) return;
    uint16_t *p = at(x, y);
    *p = (a >= 250) ? c : mix565(*p, c, a);
  }

  void fillRect(int x, int y, int W, int H, uint16_t c) {
    int x0 = x, y0 = y, x1 = x + W, y1 = y + H;
    if (!clip(x0, y0, x1, y1)) return;
    for (int yy = y0; yy < y1; yy++) {
      uint16_t *p = at(x0, yy);
      for (int xx = x0; xx < x1; xx++) *p++ = c;
    }
  }

  // anti-aliased filled circle
  void fillCircle(float cx, float cy, float r, uint16_t c) {
    int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
    int x1 = (int)ceilf(cx + r + 1), y1 = (int)ceilf(cy + r + 1);
    if (!clip(x0, y0, x1, y1)) return;
    for (int y = y0; y < y1; y++) {
      float dy = y + 0.5f - cy;
      for (int x = x0; x < x1; x++) {
        float dx = x + 0.5f - cx;
        float d = r - sqrtf(dx * dx + dy * dy) + 0.5f;
        if (d <= 0) continue;
        blendPx(x, y, c, d >= 1 ? 255 : (uint8_t)(d * 255));
      }
    }
  }

  // anti-aliased ring (circle outline)
  void ring(float cx, float cy, float r, float thick, uint16_t c) {
    int x0 = (int)floorf(cx - r - 1), y0 = (int)floorf(cy - r - 1);
    int x1 = (int)ceilf(cx + r + 1), y1 = (int)ceilf(cy + r + 1);
    if (!clip(x0, y0, x1, y1)) return;
    float half = thick / 2;
    float mid = r - half;
    for (int y = y0; y < y1; y++) {
      float dy = y + 0.5f - cy;
      for (int x = x0; x < x1; x++) {
        float dx = x + 0.5f - cx;
        float d = half - fabsf(sqrtf(dx * dx + dy * dy) - mid) + 0.5f;
        if (d <= 0) continue;
        blendPx(x, y, c, d >= 1 ? 255 : (uint8_t)(d * 255));
      }
    }
  }

  // anti-aliased rounded rectangle
  void fillRoundRect(int x, int y, int W, int H, float r, uint16_t c) {
    int x0 = x, y0 = y, x1 = x + W, y1 = y + H;
    if (!clip(x0, y0, x1, y1)) return;
    if (r > W / 2.0f) r = W / 2.0f;
    if (r > H / 2.0f) r = H / 2.0f;
    for (int yy = y0; yy < y1; yy++) {
      float py = yy + 0.5f;
      float cy = py < y + r ? y + r : (py > y + H - r ? y + H - r : py);
      for (int xx = x0; xx < x1; xx++) {
        float px = xx + 0.5f;
        float cx = px < x + r ? x + r : (px > x + W - r ? x + W - r : px);
        float dx = px - cx, dy = py - cy;
        if (dx == 0 && dy == 0) { *at(xx, yy) = c; continue; }
        float d = r - sqrtf(dx * dx + dy * dy) + 0.5f;
        if (d <= 0) continue;
        blendPx(xx, yy, c, d >= 1 ? 255 : (uint8_t)(d * 255));
      }
    }
  }

  // anti-aliased thick line with round ends
  void line(float ax, float ay, float bx, float by, float width, uint16_t c) {
    float half = width / 2;
    int x0 = (int)floorf(fminf(ax, bx) - half - 1), y0 = (int)floorf(fminf(ay, by) - half - 1);
    int x1 = (int)ceilf(fmaxf(ax, bx) + half + 1), y1 = (int)ceilf(fmaxf(ay, by) + half + 1);
    if (!clip(x0, y0, x1, y1)) return;
    float vx = bx - ax, vy = by - ay;
    float len2 = vx * vx + vy * vy;
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        float px = x + 0.5f - ax, py = y + 0.5f - ay;
        float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        float dx = px - t * vx, dy = py - t * vy;
        float d = half - sqrtf(dx * dx + dy * dy) + 0.5f;
        if (d <= 0) continue;
        blendPx(x, y, c, d >= 1 ? 255 : (uint8_t)(d * 255));
      }
    }
  }

  // anti-aliased filled triangle (any winding)
  void fillTri(float x0f, float y0f, float x1f, float y1f, float x2f, float y2f, uint16_t c) {
    float area = (x1f - x0f) * (y2f - y0f) - (y1f - y0f) * (x2f - x0f);
    if (area < 0) { float t = x1f; x1f = x2f; x2f = t; t = y1f; y1f = y2f; y2f = t; }
    int bx0 = (int)floorf(fminf(x0f, fminf(x1f, x2f))) - 1, by0 = (int)floorf(fminf(y0f, fminf(y1f, y2f))) - 1;
    int bx1 = (int)ceilf(fmaxf(x0f, fmaxf(x1f, x2f))) + 1, by1 = (int)ceilf(fmaxf(y0f, fmaxf(y1f, y2f))) + 1;
    if (!clip(bx0, by0, bx1, by1)) return;
    float ex[3] = {x0f, x1f, x2f}, ey[3] = {y0f, y1f, y2f};
    float nx[3], ny[3], nl[3];
    for (int i = 0; i < 3; i++) {
      int j = (i + 1) % 3;
      nx[i] = -(ey[j] - ey[i]); ny[i] = ex[j] - ex[i];
      nl[i] = sqrtf(nx[i] * nx[i] + ny[i] * ny[i]);
      if (nl[i] == 0) nl[i] = 1;
    }
    for (int y = by0; y < by1; y++) {
      for (int x = bx0; x < bx1; x++) {
        float px = x + 0.5f, py = y + 0.5f, m = 1e9f;
        for (int i = 0; i < 3; i++) {
          float d = ((px - ex[i]) * nx[i] + (py - ey[i]) * ny[i]) / nl[i];
          if (d < m) m = d;
        }
        float a = m + 0.5f;
        if (a <= 0) continue;
        blendPx(x, y, c, a >= 1 ? 255 : (uint8_t)(a * 255));
      }
    }
  }

  // copy an RGB565 image (clipped)
  void blit(int x, int y, const uint16_t *src, int W, int H) {
    blitPart(x, y, src, W, H, 0, W);
  }
  // copy columns [sx, sx+sw) of an image to x (wrapping around the image width, for marquees)
  void blitPart(int x, int y, const uint16_t *src, int srcW, int srcH, int sx, int sw) {
    if (!src || srcW <= 0) return;
    int x0 = x, y0 = y, x1 = x + sw, y1 = y + srcH;
    if (!clip(x0, y0, x1, y1)) return;
    for (int yy = y0; yy < y1; yy++) {
      const uint16_t *row = src + (yy - y) * srcW;
      uint16_t *p = at(x0, yy);
      int col = sx + (x0 - x);
      for (int xx = x0; xx < x1; xx++) {
        int cc = col++ % srcW;
        *p++ = row[cc];
      }
    }
  }

  // ---- text (4-bit anti-aliased Figtree glyphs) ----
  int textWidth(const char *s, const AAFont &f) {
    int w = 0;
    for (; *s; s++) {
      uint8_t ch = *s;
      if (ch < f.first || ch > f.last) ch = f.first;
      w += f.g[ch - f.first].adv;
    }
    return w;
  }
  // y = top of the text line; returns the width drawn
  int text(int x, int y, const char *s, const AAFont &f, uint16_t c) {
    int start = x;
    int base = y + f.ascent;
    for (; *s; s++) {
      uint8_t ch = *s;
      if (ch < f.first || ch > f.last) ch = f.first;
      const FontGlyph &g = f.g[ch - f.first];
      if (g.w) {
        int gx = x + g.xo, gy = base + g.yo;
        int x0 = gx, y0 = gy, x1 = gx + g.w, y1 = gy + g.h;
        if (clip(x0, y0, x1, y1)) {
          for (int yy = y0; yy < y1; yy++) {
            for (int xx = x0; xx < x1; xx++) {
              int i = (yy - gy) * g.w + (xx - gx);
              uint8_t b = f.bmp[g.off + (i >> 1)];
              uint8_t a = (i & 1) ? (b & 0x0F) : (b >> 4);
              if (a) blendPx(xx, yy, c, a * 17);
            }
          }
        }
      }
      x += g.adv;
    }
    return x - start;
  }
  int textCentered(int cx, int y, const char *s, const AAFont &f, uint16_t c) {
    int w = textWidth(s, f);
    return text(cx - w / 2, y, s, f, c);
  }
  int textRight(int rx, int y, const char *s, const AAFont &f, uint16_t c) {
    int w = textWidth(s, f);
    return text(rx - w, y, s, f, c);
  }
};
