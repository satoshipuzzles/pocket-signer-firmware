// Modern UI kit — a small set of reusable, opinionated primitives so every
// app in mini_screen has a consistent visual language.
//
// Design tenets:
//   * Big, legible FreeSans typography (no more 5x7 bitmap font).
//   * A shared header/hint band so every app feels like part of one product.
//   * Pill-shaped tappable buttons with icon slots.
//   * Cards for content grouping; dividers only when actually needed.
//   * Everything is drawn immediately — no widget tree, no retained state.
//
// Layout constants live in this header so pixel-level tuning happens in
// exactly one place.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "theme.h"

namespace mini { namespace ui {

// ---- layout constants -----------------------------------------------------

constexpr int16_t kHeaderH   = 58;      // top band with wordmark + app title
constexpr int16_t kHintH     = 30;      // bottom hint band
constexpr int16_t kSideMargin= 16;
constexpr int16_t kRadiusLg  = 16;      // cards
constexpr int16_t kRadiusMd  = 12;      // buttons
constexpr int16_t kRadiusSm  = 8;       // list rows

// ---- text helpers ---------------------------------------------------------

// Measure text width using the currently-set font.
inline int16_t text_w(Arduino_GFX* g, const char* s) {
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)w;
}
inline int16_t text_h(Arduino_GFX* g, const char* s) {
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)h;
}

// Draw a string with its TOP-LEFT at (x, y). Both draw_at and draw_top
// share the same semantics now that we're on the built-in bitmap font
// (setCursor sets top-left, not baseline). draw_at is kept for API
// backwards-compat.
inline void draw_at(Arduino_GFX* g, const char* s, int16_t x, int16_t y,
                    uint16_t color) {
  g->setTextColor(color);
  g->setCursor(x, y);
  g->print(s);
}
inline void draw_top(Arduino_GFX* g, const char* s, int16_t x, int16_t top_y,
                     uint16_t color) {
  draw_at(g, s, x, top_y, color);
}

inline void draw_centered(Arduino_GFX* g, const char* s, int16_t cx,
                          int16_t y, uint16_t color) {
  int16_t w = text_w(g, s);
  draw_at(g, s, cx - w / 2, y, color);
}

inline void draw_top_centered(Arduino_GFX* g, const char* s, int16_t cx,
                              int16_t top_y, uint16_t color) {
  int16_t w = text_w(g, s);
  draw_at(g, s, cx - w / 2, top_y, color);
}

// Word-wrap a string into (x, top_y, width) with a given line height.
// Returns the y coord AFTER the last line.
int16_t draw_paragraph(Arduino_GFX* g, const String& s,
                       int16_t x, int16_t top_y, int16_t width,
                       int16_t line_height, uint16_t color);

// Char-wrap (for long tokens like npub1... with no whitespace).
int16_t draw_wrapped_token(Arduino_GFX* g, const String& s,
                           int16_t x, int16_t top_y, int16_t width,
                           int16_t line_height, uint16_t color);

// ---- surface primitives ---------------------------------------------------

// Draw a filled rounded rectangle with a subtle 1-pixel accent border.
inline void card(Arduino_GFX* g, int16_t x, int16_t y, int16_t w, int16_t h,
                 uint16_t fill, uint16_t border, int16_t radius = kRadiusLg) {
  g->fillRoundRect(x, y, w, h, radius, fill);
  if (border) g->drawRoundRect(x, y, w, h, radius, border);
}

// Draw the standard app header: small "mini" wordmark on the left in a muted
// tone, and the app's own name on the right in its accent color. Draws a
// subtle divider line under the header.
void screen_header(Arduino_GFX* g, int16_t screen_w, const char* app_name,
                   uint16_t accent);

// Bottom hint band — small centered muted text.
void screen_hint(Arduino_GFX* g, int16_t screen_w, int16_t screen_h,
                 const char* text);

// Big pill-shaped tappable button. Returns nothing but the tap zone is
// (x, y, w, h) — check gesture y/x against the same values in your handler.
struct PillButton {
  const char* label;
  const char* subtitle;   // may be nullptr
  uint16_t    accent;
  bool        pressed;    // if true, invert colors
};
void pill_button(Arduino_GFX* g, int16_t x, int16_t y, int16_t w, int16_t h,
                 const PillButton& btn);

// A subtle "status pill" — small rounded rect with a tinted background and
// a text label. Useful for connection state, error banners, mode chips.
void status_pill(Arduino_GFX* g, int16_t x, int16_t y, const char* text,
                 uint16_t accent);

// Key/value row — left-aligned key in muted text, right-aligned value in
// primary text. Useful for settings rows.
void kv_row(Arduino_GFX* g, int16_t x, int16_t y, int16_t w,
            const char* key, const char* value,
            uint16_t key_color, uint16_t val_color);

// Draw a horizontal stepped progress bar (0..1 progress). Height defaults
// to 6 px. Rounded caps.
void progress_bar(Arduino_GFX* g, int16_t x, int16_t y, int16_t w,
                  float progress, uint16_t on_color, uint16_t off_color,
                  int16_t h = 6);

// Convenience: fill the screen and paint the header band, returning the
// y-coord where content should begin (kHeaderH). All app renders should
// start with this call so the visual language stays uniform.
inline int16_t begin_screen(Arduino_GFX* g, int16_t screen_w,
                            const char* app_name, uint16_t accent) {
  g->fillScreen(theme::BG);
  screen_header(g, screen_w, app_name, accent);
  return kHeaderH;
}

}} // namespace mini::ui
