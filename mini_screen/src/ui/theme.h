// Shared visual language for every screen in mini_screen.
//
// Colors are in RGB565 (Arduino_GFX native format).
// Values sampled to hit a modern dark-mode palette:
//   - deep charcoal surface, not pure #000 (feels less crushed)
//   - saturated but not neon accent hues
//   - subtle mid-gray for hints / dividers
//
// All helpers are free functions taking a gfx pointer so any app can compose
// them without inheritance juggling.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace mini { namespace theme {

// ------------- palette (RGB565) ---------------------------------------------

// Surfaces
constexpr uint16_t BG            = 0x10A2;   // #101820 — deep navy-charcoal
constexpr uint16_t SURFACE       = 0x1904;   // #181C22 — one step lighter, tile fill
constexpr uint16_t SURFACE_HI    = 0x2124;   // #212227 — pressed / hover
constexpr uint16_t DIVIDER       = 0x2965;   // #2B2D33 — subtle border

// Text
constexpr uint16_t TEXT_HI       = 0xFFFF;   // primary
constexpr uint16_t TEXT_MID      = 0xC618;   // secondary
constexpr uint16_t TEXT_LO       = 0x7BEF;   // muted / hints
constexpr uint16_t TEXT_INVERSE  = 0x0000;   // for light backgrounds (QR page)

// Accents
constexpr uint16_t ACCENT_CYAN   = 0x05FF;   // signer
constexpr uint16_t ACCENT_ORANGE = 0xFCA0;   // dice / bitcoin
constexpr uint16_t ACCENT_GREEN  = 0x37E0;
constexpr uint16_t ACCENT_MAGENTA= 0xF81F;
constexpr uint16_t ACCENT_YELLOW = 0xFFE0;
constexpr uint16_t ACCENT_RED    = 0xF945;

// ------------- typography ---------------------------------------------------
//
// We standardized on the Adafruit_GFX built-in 5x7 bitmap font scaled via
// setTextSize(). It renders reliably on this display (unlike the vendored
// FreeFonts, which showed up as invisible glyphs on some builds — probably
// a PROGMEM alignment quirk in the GFX_Library_for_Arduino port). Scaling
// the bitmap font isn't as pretty, but it's crisp on the AMOLED and always
// draws. Each font "size" below is a setTextSize() multiplier plus, for the
// bolder headings, we draw twice with a 1-px offset to fake weight.
//
// Font-selector: just a textSize multiplier for the built-in 5x7 bitmap
// font. Hierarchy comes from size alone. The struct wrapper is kept so
// callers can write `theme::FONT_HEADING_L` uniformly.
struct FontSel {
  uint8_t size;     // setTextSize multiplier
};

constexpr FontSel FONT_HEADING_XL = {5};   // ~40px tall
constexpr FontSel FONT_HEADING_L  = {4};   // ~32px tall
constexpr FontSel FONT_HEADING_M  = {3};   // ~24px tall
constexpr FontSel FONT_BODY       = {2};   // 16px
constexpr FontSel FONT_SMALL      = {1};   // 8px
constexpr FontSel FONT_MONO       = {2};   // 16px (built-in is monospace)

// Configure the gfx state for the given font selector. Always resets to the
// built-in font and applies the size multiplier.
void use_font(Arduino_GFX* g, FontSel f);

// ------------- helpers ------------------------------------------------------

// Measures a null-terminated string using the currently-set font. Returns pixel
// width so we can center text without hard-coding character advance widths.
int16_t text_width(Arduino_GFX* g, const char* s);

// Centered draw at (cx, y-baseline). Sets cursor to reach visual center of x.
void draw_text_centered(Arduino_GFX* g, const char* s,
                        int16_t cx, int16_t baseline_y, uint16_t color);

// Left-aligned draw at cursor baseline y (font baseline, not top).
void draw_text_at(Arduino_GFX* g, const char* s,
                  int16_t x, int16_t baseline_y, uint16_t color);

// Filled rounded card. Optionally with a 1px accent border (0 = no border).
void draw_card(Arduino_GFX* g, int16_t x, int16_t y, int16_t w, int16_t h,
               int16_t radius, uint16_t fill, uint16_t border);

// A framed screen "hint bar" pinned to bottom. Small mono-color hint text.
void draw_hint(Arduino_GFX* g, int16_t screen_w, int16_t screen_h,
               const char* text);

// Draw a header row: small "mini" wordmark on the left, page title on the right.
void draw_header(Arduino_GFX* g, int16_t screen_w, const char* title,
                 uint16_t title_color);

// Draw a horizontal progress-dot indicator (used for multi-page apps).
void draw_page_dots(Arduino_GFX* g, int16_t screen_w, int16_t y,
                    int cur, int total, uint16_t on_color, uint16_t off_color);

// Word-wrap `s` into a paragraph starting at (x, baseline_y) with width w and
// line height lh. Returns the y-baseline of the last line drawn.
int16_t draw_wrapped(Arduino_GFX* g, const String& s,
                     int16_t x, int16_t baseline_y, int16_t w,
                     int16_t line_height, uint16_t color);

// ------------- icon drawing -------------------------------------------------
// Each icon fits within a bounding box of side `size` at (x, y).

void icon_dice (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_key  (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_cube (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_mic  (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_torch(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_block(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_shoe (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_level(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_piano(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_pencil(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_wifi (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_notes(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_eye  (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color); // Genesis
void icon_snake(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_ear  (Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color); // Vespers
void icon_photo(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);
void icon_video(Arduino_GFX* g, int16_t x, int16_t y, int16_t size, uint16_t color);

}} // namespace mini::theme
