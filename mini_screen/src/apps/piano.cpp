#include "piano.h"
#include "../ui/theme.h"

#include <math.h>

namespace mini {

// Frequencies (Hz) for one octave starting at C4. Indices 0..6 = white keys
// C D E F G A B; indices 7..11 = black keys C# D# F# G# A#. We collapse them
// into a single 12-note array for lookup, and store their (x,y) mapping in
// hitTest().
static const float kBaseFreqs[12] = {
  261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f,   // C..B
  277.18f, 311.13f, 369.99f, 415.30f, 466.16f                       // C# D# F# G# A#
};
static const char kBaseNames[12][3] = {
  "C","D","E","F","G","A","B",
  "C#","D#","F#","G#","A#"
};

// Positions of the 5 black keys relative to white keys (0-indexed white key
// they're above the right edge of). Standard piano layout: C# above C,
// D# above D, F# above F, G# above G, A# above A. So black keys sit on
// white indices [0,1,3,4,5] boundaries.
static const int kBlackWhite[5] = {0, 1, 3, 4, 5};   // white key that black sits between (this, this+1)

// UI geometry — computed dynamically so it centers on the current screen.
struct Layout {
  int white_w, white_h, white_y;
  int black_w, black_h;
  int start_x;
};
static Layout compute_layout(const AppContext& ctx) {
  Layout L;
  const int margin = 12;
  L.white_w = (ctx.w - 2 * margin) / 7;
  L.white_h = ctx.h * 60 / 100;
  L.white_y = ctx.h - L.white_h - 20;
  L.start_x = (ctx.w - L.white_w * 7) / 2;
  L.black_w = L.white_w * 60 / 100;
  L.black_h = L.white_h * 60 / 100;
  return L;
}

int Piano::hitTest(AppContext& ctx, int16_t x, int16_t y) const {
  Layout L = compute_layout(ctx);
  // Check black keys first (they're on top of the whites)
  if (y >= L.white_y && y < L.white_y + L.black_h) {
    for (int b = 0; b < 5; ++b) {
      int wl = kBlackWhite[b];
      int bx = L.start_x + (wl + 1) * L.white_w - L.black_w / 2;
      if (x >= bx && x < bx + L.black_w) {
        return 7 + b;
      }
    }
  }
  // Then white keys
  if (y >= L.white_y && y < L.white_y + L.white_h) {
    int i = (x - L.start_x) / L.white_w;
    if (i >= 0 && i < 7) {
      // Make sure we're not overlapping a black key area
      if (y < L.white_y + L.black_h) {
        // We already checked black above and didn't hit — safe to accept white
      }
      return i;
    }
  }
  return -1;
}

void Piano::renderFull(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);

  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(theme::ACCENT_MAGENTA);
  g->setCursor(16, 18);
  g->print("PIANO");

  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  char ostr[16];
  snprintf(ostr, sizeof(ostr), "octave %+d", octave_);
  int16_t ow = (int16_t)strlen(ostr) * 6;
  g->setCursor(ctx.w - ow - 16, 24);
  g->print(ostr);
  g->drawFastHLine(16, 52, ctx.w - 32, theme::DIVIDER);

  Layout L = compute_layout(ctx);

  // White keys
  for (int i = 0; i < 7; ++i) {
    int wx = L.start_x + i * L.white_w;
    g->fillRoundRect(wx + 1, L.white_y, L.white_w - 2, L.white_h, 4, theme::TEXT_HI);
    // Label at bottom
    g->setTextColor(theme::TEXT_LO);
    g->setTextSize(1);
    int16_t lw = (int16_t)strlen(kBaseNames[i]) * 6;
    g->setCursor(wx + (L.white_w - lw) / 2, L.white_y + L.white_h - 10);
    g->print(kBaseNames[i]);
  }

  // Black keys
  for (int b = 0; b < 5; ++b) {
    int wl = kBlackWhite[b];
    int bx = L.start_x + (wl + 1) * L.white_w - L.black_w / 2;
    g->fillRoundRect(bx, L.white_y, L.black_w, L.black_h, 3, 0x0000);
    g->drawRoundRect(bx, L.white_y, L.black_w, L.black_h, 3, theme::TEXT_LO);
  }

  // Bottom hint
  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* hint = "up/down = octave  |  long-press = home";
  int16_t hw = (int16_t)strlen(hint) * 6;
  g->setCursor((ctx.w - hw) / 2, ctx.h - 12);
  g->print(hint);
}

void Piano::highlightKey(AppContext& ctx, int idx, bool on) {
  if (idx < 0) return;
  auto* g = ctx.gfx;
  Layout L = compute_layout(ctx);
  if (idx < 7) {
    // White key
    int wx = L.start_x + idx * L.white_w;
    uint16_t c = on ? theme::ACCENT_MAGENTA : theme::TEXT_HI;
    g->fillRoundRect(wx + 1, L.white_y, L.white_w - 2, L.white_h, 4, c);
    // Re-label
    g->setFont(nullptr);
    g->setTextSize(1);
    g->setTextColor(on ? theme::TEXT_HI : theme::TEXT_LO);
    int16_t lw = (int16_t)strlen(kBaseNames[idx]) * 6;
    g->setCursor(wx + (L.white_w - lw) / 2, L.white_y + L.white_h - 10);
    g->print(kBaseNames[idx]);
  } else {
    // Black key
    int b = idx - 7;
    int wl = kBlackWhite[b];
    int bx = L.start_x + (wl + 1) * L.white_w - L.black_w / 2;
    uint16_t c = on ? theme::ACCENT_MAGENTA : 0x0000;
    g->fillRoundRect(bx, L.white_y, L.black_w, L.black_h, 3, c);
    g->drawRoundRect(bx, L.white_y, L.black_w, L.black_h, 3, theme::TEXT_LO);
  }
}

// Synthesize a decaying sine tone for 90 ms at the target frequency and
// dispatch to the speaker via play_.
void Piano::playNote(int idx) {
  if (!play_ || idx < 0 || idx >= 12) return;
  float base_freq = kBaseFreqs[idx];
  // Octave shift: doubling per +1
  float freq = base_freq * powf(2.0f, (float)octave_);

  const int SR = 16000;
  const int N  = SR * 90 / 1000;   // 90 ms
  static int16_t buf[SR * 90 / 1000];   // fits in .bss, ~2.8 KB
  const float tau = 0.10f;
  for (int i = 0; i < N; ++i) {
    float t = (float)i / (float)SR;
    float env = expf(-t / tau);
    float s = sinf(2.0f * (float)M_PI * freq * t);
    buf[i] = (int16_t)(s * env * 18000);
  }
  play_(buf, N);
}

void Piano::onEnter(AppContext& ctx) {
  last_pressed_ = -1;
  renderFull(ctx);
}

bool Piano::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TOUCH_DOWN: {
      int idx = hitTest(ctx, g.x, g.y);
      if (idx >= 0) {
        // Un-highlight previous, highlight new
        if (last_pressed_ >= 0 && last_pressed_ != idx) {
          highlightKey(ctx, last_pressed_, false);
        }
        if (last_pressed_ != idx) {
          last_pressed_ = idx;
          highlightKey(ctx, idx, true);
          playNote(idx);
        }
      }
      return true;
    }
    case Gesture::TOUCH_UP:
      if (last_pressed_ >= 0) {
        highlightKey(ctx, last_pressed_, false);
        last_pressed_ = -1;
      }
      return true;
    case Gesture::SWIPE_UP:
      if (octave_ < 2) octave_++;
      renderFull(ctx);
      return true;
    case Gesture::SWIPE_DOWN:
      if (octave_ > -2) octave_--;
      renderFull(ctx);
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

} // namespace mini
