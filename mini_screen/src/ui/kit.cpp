#include "kit.h"

namespace mini { namespace ui {

void screen_header(Arduino_GFX* g, int16_t screen_w, const char* app_name,
                   uint16_t accent) {
  // Wordmark left
  theme::use_font(g, theme::FONT_SMALL);
  draw_top(g, "mini", kSideMargin, 18, theme::TEXT_LO);

  // App name right — a hair bigger, in accent color
  theme::use_font(g, theme::FONT_HEADING_M);
  int16_t nw = text_w(g, app_name);
  draw_top(g, app_name, screen_w - nw - kSideMargin, 12, accent);

  // Subtle divider
  g->drawFastHLine(kSideMargin, kHeaderH - 6, screen_w - 2 * kSideMargin,
                   theme::DIVIDER);
}

void screen_hint(Arduino_GFX* g, int16_t screen_w, int16_t screen_h,
                 const char* text) {
  theme::use_font(g, theme::FONT_SMALL);
  int16_t w = text_w(g, text);
  int16_t h = text_h(g, text);
  // Sit hint text ~4 px above the very bottom of the screen.
  draw_at(g, text, (screen_w - w) / 2, screen_h - h - 4, theme::TEXT_LO);
}

// Vertically center a top-y-anchored glyph inside a container of height H.
static inline int16_t vcenter(int16_t container_y, int16_t container_h,
                              int16_t glyph_h) {
  return container_y + (container_h - glyph_h) / 2;
}

void pill_button(Arduino_GFX* g, int16_t x, int16_t y, int16_t w, int16_t h,
                 const PillButton& btn) {
  const int16_t r = h / 2;
  const uint16_t fill      = btn.pressed ? btn.accent : theme::SURFACE;
  const uint16_t label_col = btn.pressed ? theme::BG   : btn.accent;
  const uint16_t sub_col   = btn.pressed ? theme::BG   : theme::TEXT_LO;
  g->fillRoundRect(x, y, w, h, r, fill);
  if (!btn.pressed) g->drawRoundRect(x, y, w, h, r, btn.accent);

  // Auto-scale label so long words still fit inside the pill width.
  theme::FontSel fs = theme::FONT_HEADING_M;
  while (fs.size > 1) {
    theme::use_font(g, fs);
    if (text_w(g, btn.label) < w - 20) break;
    fs.size--;
  }
  theme::use_font(g, fs);
  int16_t lw = text_w(g, btn.label);
  int16_t lh = 8 * fs.size;
  int16_t label_y = btn.subtitle
      ? y + (h - lh) / 3               // upper third when subtitle exists
      : vcenter(y, h, lh);              // otherwise vertically centered
  draw_at(g, btn.label, x + (w - lw) / 2, label_y, label_col);

  if (btn.subtitle) {
    theme::use_font(g, theme::FONT_SMALL);
    int16_t sw = text_w(g, btn.subtitle);
    int16_t sh = 8 * theme::FONT_SMALL.size;
    draw_at(g, btn.subtitle, x + (w - sw) / 2, y + h - sh - 8, sub_col);
  }
}

void status_pill(Arduino_GFX* g, int16_t x, int16_t y, const char* text,
                 uint16_t accent) {
  theme::use_font(g, theme::FONT_SMALL);
  int16_t tw = text_w(g, text);
  const int16_t pad_x = 10;
  const int16_t w = tw + 2 * pad_x;
  const int16_t h = 22;
  // Tint background: 20% of accent brightness in each channel.
  const uint16_t rr = ((accent >> 11) & 0x1F) * 2 / 10;
  const uint16_t gg = ((accent >> 5)  & 0x3F) * 2 / 10;
  const uint16_t bb =  (accent        & 0x1F) * 2 / 10;
  const uint16_t bg = (uint16_t)((rr << 11) | (gg << 5) | bb);
  g->fillRoundRect(x, y, w, h, h / 2, bg);
  g->drawRoundRect(x, y, w, h, h / 2, accent);
  int16_t th = 8 * theme::FONT_SMALL.size;
  draw_at(g, text, x + pad_x, vcenter(y, h, th), accent);
}

void kv_row(Arduino_GFX* g, int16_t x, int16_t y, int16_t w,
            const char* key, const char* value,
            uint16_t key_color, uint16_t val_color) {
  theme::use_font(g, theme::FONT_SMALL);
  draw_top(g, key, x, y, key_color);
  int16_t vw = text_w(g, value);
  draw_top(g, value, x + w - vw, y, val_color);
}

void progress_bar(Arduino_GFX* g, int16_t x, int16_t y, int16_t w,
                  float progress, uint16_t on_color, uint16_t off_color,
                  int16_t h) {
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  g->fillRoundRect(x, y, w, h, h / 2, off_color);
  int16_t fw = (int16_t)(w * progress);
  if (fw > 0) g->fillRoundRect(x, y, fw, h, h / 2, on_color);
}

int16_t draw_paragraph(Arduino_GFX* g, const String& s,
                       int16_t x, int16_t top_y, int16_t width,
                       int16_t line_height, uint16_t color) {
  g->setTextColor(color);
  int i = 0;
  int16_t y = top_y;
  const int L = (int)s.length();
  while (i < L) {
    // Skip leading whitespace on each line
    while (i < L && s[i] == ' ') ++i;
    if (i >= L) break;
    // Greedy fit on word boundaries; fall back to hard cut if a word is longer
    // than the entire width.
    int lo = 1, hi = L - i, fit = 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      // Prefer break at last space in [i, i+mid)
      int piece_end = i + mid;
      int break_at = piece_end;
      // If we're not at end of string and next char isn't a space, walk back
      // to previous space to avoid splitting a word.
      if (piece_end < L && s[piece_end] != ' ') {
        while (break_at > i + 1 && s[break_at - 1] != ' ') --break_at;
        if (break_at == i + 1) break_at = piece_end;   // huge word, hard cut
      }
      String piece = s.substring(i, break_at);
      int16_t pw = text_w(g, piece.c_str());
      if (pw <= width) { fit = break_at - i; lo = mid + 1; }
      else hi = mid - 1;
    }
    String line = s.substring(i, i + fit);
    line.trim();
    int16_t line_top = y;
    draw_top(g, line.c_str(), x, line_top, color);
    i += fit;
    y += line_height;
  }
  return y;
}

int16_t draw_wrapped_token(Arduino_GFX* g, const String& s,
                           int16_t x, int16_t top_y, int16_t width,
                           int16_t line_height, uint16_t color) {
  g->setTextColor(color);
  int i = 0;
  int16_t y = top_y;
  while (i < (int)s.length()) {
    int lo = 1, hi = (int)s.length() - i, fit = 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      String piece = s.substring(i, i + mid);
      int16_t pw = text_w(g, piece.c_str());
      if (pw <= width) { fit = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    draw_top(g, s.substring(i, i + fit).c_str(), x, y, color);
    i += fit;
    y += line_height;
  }
  return y;
}

}} // namespace mini::ui
