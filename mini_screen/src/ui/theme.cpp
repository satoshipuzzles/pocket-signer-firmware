#include "theme.h"

namespace mini { namespace theme {

void use_font(Arduino_GFX* g, FontSel f) {
  g->setFont(nullptr);        // use built-in 5x7 bitmap
  g->setTextSize(f.size);
  g->setTextWrap(false);      // we always wrap manually
}

int16_t text_width(Arduino_GFX* g, const char* s) {
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)w;
}

void draw_text_at(Arduino_GFX* g, const char* s,
                  int16_t x, int16_t baseline_y, uint16_t color) {
  g->setTextColor(color);
  g->setCursor(x, baseline_y);
  g->print(s);
}

void draw_text_centered(Arduino_GFX* g, const char* s,
                        int16_t cx, int16_t baseline_y, uint16_t color) {
  int16_t w = text_width(g, s);
  draw_text_at(g, s, cx - w / 2, baseline_y, color);
}

void draw_card(Arduino_GFX* g, int16_t x, int16_t y, int16_t w, int16_t h,
               int16_t radius, uint16_t fill, uint16_t border) {
  g->fillRoundRect(x, y, w, h, radius, fill);
  if (border != 0) {
    g->drawRoundRect(x, y, w, h, radius, border);
  }
}

void draw_hint(Arduino_GFX* g, int16_t screen_w, int16_t screen_h,
               const char* text) {
  use_font(g, FONT_SMALL);
  int16_t tw = text_width(g, text);
  draw_text_at(g, text, (screen_w - tw) / 2, screen_h - 14, TEXT_LO);
}

void draw_header(Arduino_GFX* g, int16_t screen_w, const char* title,
                 uint16_t title_color) {
  use_font(g, FONT_SMALL);
  draw_text_at(g, "mini", 16, 8, TEXT_LO);
  use_font(g, FONT_HEADING_M);
  int16_t tw = text_width(g, title);
  draw_text_at(g, title, screen_w - tw - 16, 4, title_color);
}

void draw_page_dots(Arduino_GFX* g, int16_t screen_w, int16_t y,
                    int cur, int total, uint16_t on_color, uint16_t off_color) {
  const int gap = 10, r = 3;
  const int tot = (total - 1) * gap;
  int x = (screen_w - tot) / 2;
  for (int i = 0; i < total; ++i) {
    if (i == cur) g->fillCircle(x + i * gap, y, r, on_color);
    else          g->fillCircle(x + i * gap, y, 2, off_color);
  }
}

// Simple word-wrap over a bech32-style long token: no whitespace to break on,
// so we hard-wrap after `chars_per_line`. For strings with spaces, we prefer
// breaking on space to keep readability.
int16_t draw_wrapped(Arduino_GFX* g, const String& s,
                     int16_t x, int16_t baseline_y, int16_t w,
                     int16_t line_height, uint16_t color) {
  g->setTextColor(color);
  int i = 0;
  int16_t y = baseline_y;
  while (i < (int)s.length()) {
    // Greedy fit: probe by cutting the string until it fits width w.
    int lo = 1, hi = (int)s.length() - i;
    int fit = 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      String piece = s.substring(i, i + mid);
      int16_t pw = text_width(g, piece.c_str());
      if (pw <= w) { fit = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    g->setCursor(x, y);
    for (int k = 0; k < fit; ++k) g->print(s[i + k]);
    i += fit;
    y += line_height;
  }
  return y;
}

// ------------- icons -------------------------------------------------------

// Face-up d6: rounded square outline (thick), five solid pips.
void icon_dice(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t r = size / 5;
  const int16_t inset = size / 14;
  const int16_t bx = x + inset, by = y + inset;
  const int16_t bw = size - inset * 2, bh = size - inset * 2;
  for (int t = 0; t < 3; ++t) {
    g->drawRoundRect(bx + t, by + t, bw - 2 * t, bh - 2 * t, r - t, color);
  }
  const int16_t pr = size / 12;
  const int16_t q  = size / 4;
  g->fillCircle(x + q,        y + q,        pr, color);
  g->fillCircle(x + size - q, y + q,        pr, color);
  g->fillCircle(x + size / 2, y + size / 2, pr, color);
  g->fillCircle(x + q,        y + size - q, pr, color);
  g->fillCircle(x + size - q, y + size - q, pr, color);
}

// A stylized key silhouette. Head is a bow, shaft points right, two teeth.
void icon_key(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  // Scale-based geometry.
  const int16_t s     = size;
  const int16_t head_d= s * 44 / 100;
  const int16_t head_r= head_d / 2;
  const int16_t head_cx = x + head_r + 1;
  const int16_t head_cy = y + s / 2;

  // Bow (ring)
  g->fillCircle(head_cx, head_cy, head_r, color);
  g->fillCircle(head_cx, head_cy, head_r / 2, theme::BG);

  // Shaft
  const int16_t shaft_y = head_cy - s / 14;
  const int16_t shaft_h = s / 7;
  const int16_t shaft_x = head_cx + head_r - 2;
  const int16_t shaft_w = x + s - shaft_x;
  g->fillRect(shaft_x, shaft_y, shaft_w, shaft_h, color);

  // Teeth
  const int16_t tooth_w = s / 10;
  const int16_t tooth_h = s / 6;
  g->fillRect(x + s - tooth_w - 2, shaft_y + shaft_h, tooth_w, tooth_h, color);
  g->fillRect(x + s - tooth_w * 2 - 8, shaft_y + shaft_h, tooth_w, tooth_h / 2 + 2, color);
}

// Clean modern microphone. Solid capsule + short stem + base plate. No fussy
// horseshoe cage — the silhouette reads immediately at tile size.
void icon_mic(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s / 2;
  const int16_t head_w = s * 40 / 100;
  const int16_t head_h = s * 58 / 100;
  const int16_t head_x = cx - head_w / 2;
  const int16_t head_y = y + s * 8 / 100;
  const int16_t head_r = head_w / 2;

  // Capsule
  g->fillRoundRect(head_x, head_y, head_w, head_h, head_r, color);
  // Two grille lines cut through in BG
  for (int i = 1; i <= 2; ++i) {
    int16_t gy = head_y + head_h * i / 3;
    g->drawFastHLine(head_x + 6, gy, head_w - 12, theme::BG);
  }
  // Stem
  const int16_t stem_top = head_y + head_h + 4;
  const int16_t stem_bot = y + s * 88 / 100;
  g->fillRect(cx - 2, stem_top, 4, stem_bot - stem_top, color);
  // Base plate
  g->fillRoundRect(cx - s * 22 / 100, stem_bot - 4,
                   s * 44 / 100, 6, 3, color);
}

// Torch (flashlight) icon: a "sun" — circle with radial spikes.
void icon_torch(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s / 2, cy = y + s / 2;
  const int16_t r_inner = s * 22 / 100;
  const int16_t r_outer = s * 44 / 100;
  g->fillCircle(cx, cy, r_inner, color);
  g->drawCircle(cx, cy, r_inner + 2, color);
  // 8 spikes
  for (int k = 0; k < 8; ++k) {
    float ang = (float)k * ((float)M_PI / 4.0f);
    int16_t x1 = cx + (int16_t)(cosf(ang) * (r_inner + 6));
    int16_t y1 = cy + (int16_t)(sinf(ang) * (r_inner + 6));
    int16_t x2 = cx + (int16_t)(cosf(ang) * r_outer);
    int16_t y2 = cy + (int16_t)(sinf(ang) * r_outer);
    g->drawLine(x1, y1, x2, y2, color);
    g->drawLine(x1 + 1, y1, x2 + 1, y2, color);
  }
}

// Stacked-block icon for Explorer: three offset squares suggesting a chain.
void icon_block(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t bs = s * 42 / 100;
  const int16_t r  = bs / 6;
  const int16_t off = s * 10 / 100;
  // Back block (dim)
  g->drawRoundRect(x + s - bs - 2,      y + 2,             bs, bs, r, color);
  // Mid block
  g->drawRoundRect(x + s - bs - 2 - off, y + 2 + off,       bs, bs, r, color);
  // Front block (solid outline + interior tick marks)
  int16_t fx = x + 2, fy = y + s - bs - 2;
  g->drawRoundRect(fx, fy, bs, bs, r, color);
  g->drawRoundRect(fx+1, fy+1, bs-2, bs-2, r-1, color);
  // Tiny tx-line marks inside
  for (int i = 1; i <= 3; ++i) {
    int16_t ly = fy + bs * i / 4;
    g->drawFastHLine(fx + 6, ly, bs - 12, color);
  }
}

// Isometric cube outline.
void icon_cube(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s / 2;
  const int16_t cy = y + s / 2;
  const int16_t rx = s * 42 / 100;
  const int16_t ry = s * 22 / 100;

  // Top diamond
  g->drawLine(cx,       cy - ry - ry/2, cx + rx,  cy - ry/2, color);
  g->drawLine(cx + rx,  cy - ry/2,      cx,       cy + ry/2, color);
  g->drawLine(cx,       cy + ry/2,      cx - rx,  cy - ry/2, color);
  g->drawLine(cx - rx,  cy - ry/2,      cx,       cy - ry - ry/2, color);

  // Verticals down
  g->drawLine(cx - rx,  cy - ry/2,      cx - rx,  cy + ry + ry/2, color);
  g->drawLine(cx,       cy + ry/2,      cx,       cy + ry + ry*2, color);
  g->drawLine(cx + rx,  cy - ry/2,      cx + rx,  cy + ry + ry/2, color);

  // Bottom sides
  g->drawLine(cx - rx,  cy + ry + ry/2, cx,       cy + ry + ry*2, color);
  g->drawLine(cx,       cy + ry + ry*2, cx + rx,  cy + ry + ry/2, color);
}

// Human silhouette running: circle head + angled torso + swinging arms/legs.
// Reads as "steps" more clearly than a footprint on a small tile.
void icon_shoe(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s / 2;
  // Head
  g->fillCircle(cx + 6, y + s * 18 / 100, s * 10 / 100, color);
  // Torso — slanted rounded rect
  auto thick_line = [&](int16_t x0, int16_t y0, int16_t x1, int16_t y1, int t) {
    for (int i = -t; i <= t; ++i) {
      g->drawLine(x0 + i, y0, x1 + i, y1, color);
    }
  };
  const int16_t torso_x0 = cx - 4, torso_y0 = y + s * 30 / 100;
  const int16_t torso_x1 = cx + 6, torso_y1 = y + s * 58 / 100;
  thick_line(torso_x0, torso_y0, torso_x1, torso_y1, 4);
  // Front leg (running kick)
  thick_line(torso_x1, torso_y1,
             cx + s * 22 / 100, y + s * 82 / 100, 3);
  // Back leg (planted)
  thick_line(torso_x1, torso_y1,
             cx - s * 18 / 100, y + s * 90 / 100, 3);
  // Front arm
  thick_line(cx - 2, y + s * 38 / 100,
             cx + s * 20 / 100, y + s * 30 / 100, 3);
  // Back arm
  thick_line(cx - 2, y + s * 40 / 100,
             cx - s * 18 / 100, y + s * 52 / 100, 3);
}

// Bubble level: horizontal tube with a bubble slightly right of center.
void icon_level(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  // Frame — long horizontal rectangle
  const int16_t tube_w = s * 88 / 100;
  const int16_t tube_h = s * 34 / 100;
  const int16_t tube_x = x + (s - tube_w) / 2;
  const int16_t tube_y = y + (s - tube_h) / 2;
  g->drawRoundRect(tube_x, tube_y, tube_w, tube_h, tube_h / 3, color);
  g->drawRoundRect(tube_x - 1, tube_y - 1, tube_w + 2, tube_h + 2, tube_h / 3, color);
  // Center indicator lines
  const int16_t cx = x + s / 2;
  g->drawFastVLine(cx - s * 8 / 100, tube_y - 4, tube_h + 8, color);
  g->drawFastVLine(cx + s * 8 / 100, tube_y - 4, tube_h + 8, color);
  // Bubble
  g->fillCircle(cx + s * 4 / 100, tube_y + tube_h / 2, tube_h * 34 / 100, color);
}

// Piano keys icon: 5 white keys with 3 black keys overlaid.
void icon_piano(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t kw = s * 18 / 100;
  const int16_t kh = s * 76 / 100;
  const int16_t start_x = x + (s - kw * 5) / 2;
  const int16_t start_y = y + (s - kh) / 2;
  // White keys — outline only
  for (int i = 0; i < 5; ++i) {
    g->drawRect(start_x + i * kw, start_y, kw, kh, color);
  }
  // Black keys — filled, half-height, offset between whites 1-2, 2-3, 4-5
  const int16_t bh = kh * 60 / 100;
  const int16_t bw = kw * 60 / 100;
  const int gaps[3] = {1, 2, 4};
  for (int i = 0; i < 3; ++i) {
    int16_t bx = start_x + gaps[i] * kw - bw / 2;
    g->fillRect(bx, start_y, bw, bh, color);
  }
}

// Pencil icon: diagonal shaft with tip and eraser.
void icon_pencil(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  // Diagonal from top-right to bottom-left
  const int16_t pad = s * 12 / 100;
  const int16_t x0 = x + s - pad;
  const int16_t y0 = y + pad;
  const int16_t x1 = x + pad;
  const int16_t y1 = y + s - pad;
  // Body: thick diagonal line (draw as parallel lines)
  for (int i = -3; i <= 3; ++i) {
    g->drawLine(x0 + i, y0 - i, x1 + i, y1 - i, color);
  }
  // Tip triangle at bottom-left
  g->fillTriangle(x1 - 4, y1 - 4, x1 + 4, y1 + 4, x1 - 8, y1 + 8, color);
  // Eraser cap at top-right (small square)
  g->fillRect(x0 - 4, y0 - 4, 8, 8, color);
}

// WiFi icon: three concentric arcs + dot.
void icon_wifi(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s / 2;
  const int16_t cy = y + s * 76 / 100;   // dot near bottom
  // Dot
  g->fillCircle(cx, cy, s * 5 / 100, color);
  // Three arcs, drawn as thickened partial circles (upper half)
  auto arc = [&](int16_t r) {
    for (int t = 0; t < 3; ++t) {
      // Approximate an arc by drawing many small chords along the top half.
      const int N = 26;
      for (int i = 0; i < N; ++i) {
        float a1 = 3.14159f + (float)i / (float)N * 3.14159f;
        float a2 = 3.14159f + (float)(i + 1) / (float)N * 3.14159f;
        int16_t xa = cx + (int16_t)(cosf(a1) * (r - t));
        int16_t ya = cy + (int16_t)(sinf(a1) * (r - t));
        int16_t xb = cx + (int16_t)(cosf(a2) * (r - t));
        int16_t yb = cy + (int16_t)(sinf(a2) * (r - t));
        g->drawLine(xa, ya, xb, yb, color);
      }
    }
  };
  arc(s * 20 / 100);
  arc(s * 34 / 100);
  arc(s * 48 / 100);
}

// Nostr ostrich-egg silhouette for the relay explorer. Clean rounded rect
// with a purple tint and three "signal" bars, evoking a feed of events.
void icon_notes(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t bx = x + s * 10 / 100;
  const int16_t by = y + s * 12 / 100;
  const int16_t bw = s * 80 / 100;
  const int16_t bh = s * 62 / 100;
  g->fillRoundRect(bx, by, bw, bh, s / 8, color);
  // Tail — triangle bottom-left
  const int16_t tx1 = bx + s * 10 / 100;
  const int16_t ty1 = by + bh;
  const int16_t tx2 = bx + s * 26 / 100;
  const int16_t ty2 = by + bh;
  const int16_t tx3 = bx + s * 6 / 100;
  const int16_t ty3 = by + bh + s * 20 / 100;
  g->fillTriangle(tx1, ty1, tx2, ty2, tx3, ty3, color);
  // Cut three "lines of text" out in BG for a cleaner reading
  for (int i = 0; i < 3; ++i) {
    int16_t ly = by + bh * (i + 1) / 4;
    int16_t lw = bw * (8 - i * 2) / 10;
    g->drawFastHLine(bx + 12, ly, lw, theme::BG);
    g->drawFastHLine(bx + 12, ly + 1, lw, theme::BG);
  }
}

// Eye icon (Genesis): almond-shape outline, iris circle, small pupil, glint.
// The whole thing is drawn thick so it reads at tile size.
void icon_eye(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s / 2;
  const int16_t cy = y + s / 2;
  const int16_t rx = s * 44 / 100;   // horizontal radius
  const int16_t ry = s * 22 / 100;   // vertical radius

  // Almond outline: draw as two arcs approximated by many chords.
  auto arc = [&](float y_sign) {
    const int N = 30;
    int16_t px = cx - rx, py = cy;
    for (int i = 1; i <= N; ++i) {
      float t = -1.0f + 2.0f * ((float)i / (float)N);
      // Ellipse with a pointier "tail" — bias with |t|
      int16_t nx = cx + (int16_t)(t * rx);
      float bend = 1.0f - t * t;
      int16_t ny = cy + (int16_t)(y_sign * ry * bend);
      g->drawLine(px, py,     nx, ny,     color);
      g->drawLine(px, py + 1, nx, ny + 1, color);
      px = nx; py = ny;
    }
  };
  arc(-1.0f);   // upper lid
  arc(+1.0f);   // lower lid

  // Iris — filled circle
  const int16_t ir = s * 16 / 100;
  g->fillCircle(cx, cy, ir, color);
  // Pupil — cut out
  g->fillCircle(cx, cy, ir * 45 / 100, theme::BG);
  // Glint
  g->fillCircle(cx - ir * 40 / 100, cy - ir * 40 / 100, 2, color);
}

// Snake icon: three body segments in an S-curve with a food dot.
void icon_snake(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t seg = s * 22 / 100;   // segment cell size
  const int16_t r   = seg / 4;
  // Position segments: head top-left, then right, then down, then right = classic S.
  struct P { int16_t px, py; };
  const int16_t x0 = x + s * 14 / 100;
  const int16_t y0 = y + s * 22 / 100;
  P pts[5] = {
    {x0,                  y0},
    {x0 + seg,            y0},
    {x0 + seg,            y0 + seg},
    {x0 + seg * 2,        y0 + seg},
    {x0 + seg * 2,        y0 + seg * 2},
  };
  for (int i = 0; i < 5; ++i) {
    g->fillRoundRect(pts[i].px, pts[i].py, seg - 2, seg - 2, r, color);
  }
  // Head accent: two "eye" dots
  g->fillCircle(pts[0].px + seg * 25 / 100, pts[0].py + seg * 40 / 100, 2, theme::BG);
  g->fillCircle(pts[0].px + seg * 65 / 100, pts[0].py + seg * 40 / 100, 2, theme::BG);
  // Food dot: magenta apple to the right
  const int16_t food_x = x + s * 78 / 100;
  const int16_t food_y = y + s * 30 / 100;
  g->fillCircle(food_x, food_y, s * 6 / 100, theme::ACCENT_MAGENTA);
}

// Ear icon (Vespers): a stylized side-view ear made of concentric arcs. We
// approximate the outer ear (helix) and inner canal with three nested
// C-shapes. Reads as "listening" or "sound" at tile size.
void icon_ear(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t cx = x + s * 46 / 100;
  const int16_t cy = y + s / 2;
  // Outer helix — thick partial ring, opening on the right (like a C).
  auto arc_c = [&](int16_t rad, int16_t thick) {
    const int N = 44;
    // Sweep from ~-100° to +100° going CCW (open right side)
    for (int i = 0; i < N; ++i) {
      float a1 = (float)M_PI * 0.5f + (float)i * (float)M_PI * 1.4f / (float)N;
      float a2 = (float)M_PI * 0.5f + (float)(i + 1) * (float)M_PI * 1.4f / (float)N;
      for (int t = 0; t < thick; ++t) {
        int r = rad - t;
        int16_t xa = cx + (int16_t)(cosf(a1) * r);
        int16_t ya = cy + (int16_t)(sinf(a1) * r);
        int16_t xb = cx + (int16_t)(cosf(a2) * r);
        int16_t yb = cy + (int16_t)(sinf(a2) * r);
        g->drawLine(xa, ya, xb, yb, color);
      }
    }
  };
  arc_c(s * 40 / 100, 4);   // outer helix
  arc_c(s * 26 / 100, 3);   // inner anti-helix
  // Ear canal — small filled circle
  g->fillCircle(cx - s * 6 / 100, cy + s * 8 / 100, s * 6 / 100, color);
  // Three little sound waves on the right, radiating out from the ear
  const int16_t sx = x + s * 78 / 100;
  for (int i = 0; i < 3; ++i) {
    int16_t r = s * (6 + i * 6) / 100;
    // Draw an arc (right half) at (sx, cy)
    const int N = 12;
    for (int k = 0; k < N; ++k) {
      float a1 = -(float)M_PI * 0.4f + (float)k * (float)M_PI * 0.8f / (float)N;
      float a2 = -(float)M_PI * 0.4f + (float)(k + 1) * (float)M_PI * 0.8f / (float)N;
      int16_t xa = sx + (int16_t)(cosf(a1) * r);
      int16_t ya = cy + (int16_t)(sinf(a1) * r);
      int16_t xb = sx + (int16_t)(cosf(a2) * r);
      int16_t yb = cy + (int16_t)(sinf(a2) * r);
      g->drawLine(xa, ya, xb, yb, color);
      g->drawLine(xa, ya + 1, xb, yb + 1, color);
    }
  }
}

// Photo icon: a picture frame with a mountain landscape + sun.
void icon_photo(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t fx = x + s * 8 / 100;
  const int16_t fy = y + s * 14 / 100;
  const int16_t fw = s * 84 / 100;
  const int16_t fh = s * 72 / 100;
  // Frame (double outline)
  g->drawRoundRect(fx, fy, fw, fh, 4, color);
  g->drawRoundRect(fx + 1, fy + 1, fw - 2, fh - 2, 3, color);
  // Sun
  g->fillCircle(fx + fw * 30 / 100, fy + fh * 32 / 100, s * 6 / 100, color);
  // Two mountains
  g->fillTriangle(fx + 4, fy + fh - 4,
                  fx + fw * 40 / 100, fy + fh * 40 / 100,
                  fx + fw * 60 / 100, fy + fh - 4, color);
  g->fillTriangle(fx + fw * 45 / 100, fy + fh - 4,
                  fx + fw * 72 / 100, fy + fh * 48 / 100,
                  fx + fw - 4,        fy + fh - 4, color);
}

// Video icon: play button inside a "film-strip" rectangle with sprocket dots.
void icon_video(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color) {
  const int16_t s = size;
  const int16_t fx = x + s * 6 / 100;
  const int16_t fy = y + s * 18 / 100;
  const int16_t fw = s * 88 / 100;
  const int16_t fh = s * 64 / 100;
  // Body
  g->drawRoundRect(fx, fy, fw, fh, 6, color);
  g->drawRoundRect(fx + 1, fy + 1, fw - 2, fh - 2, 5, color);
  // Sprocket dots along top and bottom edges
  for (int i = 0; i < 6; ++i) {
    int16_t px = fx + fw * (10 + i * 15) / 100;
    g->fillRect(px, fy + 2, 4, 4, color);
    g->fillRect(px, fy + fh - 6, 4, 4, color);
  }
  // Play triangle at center
  const int16_t cx = fx + fw / 2;
  const int16_t cy = fy + fh / 2;
  const int16_t r  = s * 12 / 100;
  g->fillTriangle(cx - r, cy - r, cx - r, cy + r, cx + r, cy, color);
}

}} // namespace mini::theme
