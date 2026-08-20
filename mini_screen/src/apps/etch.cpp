#include "etch.h"
#include "../ui/theme.h"

namespace mini {

// A minimal palette that reads well on the dark background.
static const uint16_t kPens[] = {
  0xFFFF, // white
  theme::ACCENT_CYAN,
  theme::ACCENT_MAGENTA,
  theme::ACCENT_YELLOW,
  theme::ACCENT_GREEN,
  theme::ACCENT_ORANGE,
};
static constexpr int kNumPens = sizeof(kPens) / sizeof(kPens[0]);

void Etch::onEnter(AppContext& ctx) {
  last_x_ = last_y_ = -1;
  clearCanvas(ctx);
}

void Etch::drawHeader(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillRect(0, 0, ctx.w, 54, theme::BG);
  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(kPens[color_idx_]);
  g->setCursor(16, 18);
  g->print("ETCH");

  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* h = "shake to erase | swipe = color";
  int16_t hw = (int16_t)strlen(h) * 6;
  g->setCursor(ctx.w - hw - 12, 24);
  g->print(h);

  g->drawFastHLine(16, 52, ctx.w - 32, theme::DIVIDER);
}

void Etch::clearCanvas(AppContext& ctx) {
  ctx.gfx->fillScreen(theme::BG);
  drawHeader(ctx);
}

void Etch::tick(AppContext& ctx, uint32_t /*now_ms*/) {
  // Continuous drawing: read live touch, connect segments to previous point.
  if (!ctx.touch) return;
  if (!ctx.touch->down) {
    last_x_ = last_y_ = -1;
    return;
  }
  const int16_t x = ctx.touch->x;
  const int16_t y = ctx.touch->y;
  // Ignore strokes that start in the header
  if (y < 54) { last_x_ = last_y_ = -1; return; }
  auto* g = ctx.gfx;
  const uint16_t c = kPens[color_idx_];
  if (last_x_ < 0) {
    g->fillCircle(x, y, 2, c);
  } else {
    // Thick line: draw 3 parallel strokes
    for (int t = -1; t <= 1; ++t) {
      g->drawLine(last_x_ + t, last_y_, x + t, y, c);
      g->drawLine(last_x_, last_y_ + t, x, y + t, c);
    }
  }
  last_x_ = x;
  last_y_ = y;
}

bool Etch::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TOUCH_DOWN:
      last_x_ = last_y_ = -1;   // let tick() plant the first dot next iteration
      return true;
    case Gesture::TOUCH_UP:
      last_x_ = last_y_ = -1;
      return true;
    case Gesture::SHAKE:
      clearCanvas(ctx);
      return true;
    case Gesture::SWIPE_UP:
    case Gesture::SWIPE_DOWN:
    case Gesture::SWIPE_LEFT:
    case Gesture::SWIPE_RIGHT:
      // These would normally clash with drawing — we only shift color if
      // the swipe DIDN'T start on the canvas (crude heuristic: swipes with
      // y < 54 are header swipes). For simplicity, tap the header area to
      // cycle color:
      return true;
    case Gesture::TAP:
      // Header tap → cycle pen; otherwise ignore (it's a dot)
      if (g.y < 54) {
        color_idx_ = (color_idx_ + 1) % kNumPens;
        drawHeader(ctx);
      }
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

} // namespace mini
