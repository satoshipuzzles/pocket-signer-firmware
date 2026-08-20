#include "torch.h"
#include "../ui/theme.h"

namespace mini {

// Predefined color modes. Each is drawn edge-to-edge; brightness is set via
// the CO5300's per-pixel value (we scale the RGB565 components).
struct Mode { const char* label; uint16_t rgb; };
static const Mode kModes[] = {
  { "WHITE",   0xFFFF },
  { "WARM",    0xFEC0 },   // yellowish
  { "RED",     0xF800 },   // night vision
  { "GREEN",   0x07E0 },
  { "BLUE",    0x001F },
  { "MAGENTA", 0xF81F },
  { "CYAN",    0x07FF },
};
static constexpr int kNumModes = sizeof(kModes) / sizeof(kModes[0]);

static uint16_t scale_565(uint16_t c, uint8_t b) {
  const uint16_t r = ((c >> 11) & 0x1F) * b / 255;
  const uint16_t g = ((c >> 5)  & 0x3F) * b / 255;
  const uint16_t bl =  (c        & 0x1F) * b / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

void Torch::applyBrightness(AppContext&) {
  // Brightness is applied via color scaling (see scale_565 in render()) so
  // we don't need to poke the panel's own brightness register. Keeping this
  // method as a no-op means the app remains portable across GFX drivers.
}

void Torch::render(AppContext& ctx) {
  auto* g = ctx.gfx;
  const Mode& m = kModes[color_idx_ % kNumModes];
  const uint16_t effective = on_ ? scale_565(m.rgb, brightness_) : 0x0000;
  g->fillScreen(effective);

  if (on_) {
    // Faint inverted label — visible enough to read but not to hurt eyes.
    g->setFont(nullptr);
    g->setTextSize(2);
    g->setTextColor(0x0000);
    int16_t lw = (int16_t)strlen(m.label) * 6 * 2;
    g->setCursor((ctx.w - lw) / 2, 20);
    g->print(m.label);
    // Brightness readout
    char bstr[24];
    snprintf(bstr, sizeof(bstr), "%d%%  (swipe \x18/\x19)", (brightness_ * 100) / 255);
    int16_t bw = (int16_t)strlen(bstr) * 6;
    g->setCursor((ctx.w - bw) / 2, ctx.h - 20);
    g->setTextSize(1);
    g->print(bstr);
  } else {
    // OFF state — thin white outline
    g->drawRoundRect(20, 20, ctx.w - 40, ctx.h - 40, 20, 0x3186);
    g->setTextColor(0xFFFF);
    g->setTextSize(3);
    const char* off = "off";
    int16_t ow = (int16_t)strlen(off) * 6 * 3;
    g->setCursor((ctx.w - ow) / 2, ctx.h / 2 - 12);
    g->print(off);
    g->setTextSize(1);
    g->setTextColor(theme::TEXT_LO);
    const char* h = "tap to turn on";
    int16_t hw = (int16_t)strlen(h) * 6;
    g->setCursor((ctx.w - hw) / 2, ctx.h / 2 + 30);
    g->print(h);
  }
  applyBrightness(ctx);
}

void Torch::onEnter(AppContext& ctx) {
  on_ = true;
  render(ctx);
}

bool Torch::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TAP:
      on_ = !on_;
      render(ctx);
      return true;
    case Gesture::SWIPE_LEFT:
      color_idx_ = (color_idx_ + 1) % kNumModes;
      render(ctx);
      return true;
    case Gesture::SWIPE_RIGHT:
      color_idx_ = (color_idx_ + kNumModes - 1) % kNumModes;
      render(ctx);
      return true;
    case Gesture::SWIPE_UP:
      brightness_ = (brightness_ > 255 - 25) ? 255 : brightness_ + 25;
      render(ctx);
      return true;
    case Gesture::SWIPE_DOWN:
      brightness_ = (brightness_ < 30) ? 5 : brightness_ - 25;
      render(ctx);
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return false;
  }
}

} // namespace mini
