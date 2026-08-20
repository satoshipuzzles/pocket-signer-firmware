#include "explorer.h"
#include "../ui/theme.h"

namespace mini {

// Real block data will populate these once we have WiFi + mempool.space
// fetching wired in. Until then, we display placeholders that make it clear
// nothing is live (dashes, no fake numbers).
static bool     s_have_data       = false;
static uint32_t s_block_height    = 0;
static uint16_t s_fee_rate        = 0;
static uint32_t s_tx_count        = 0;
static uint32_t s_seconds_ago     = 0;

void Explorer::onEnter(AppContext& ctx) {
  last_pulse_ms_ = millis();
  pulse_phase_ = 0;
  render(ctx);
}

void Explorer::tick(AppContext& ctx, uint32_t now_ms) {
  if (now_ms - last_pulse_ms_ >= 800) {
    pulse_phase_ = (pulse_phase_ + 1) & 0x3;
    last_pulse_ms_ = now_ms;
    // Redraw only the WiFi indicator dot in the corner.
    ctx.gfx->fillCircle(ctx.w - 24, 24, 5,
                        (pulse_phase_ & 1) ? theme::ACCENT_RED : theme::BG);
    ctx.gfx->drawCircle(ctx.w - 24, 24, 5, theme::ACCENT_RED);
  }
}

static void draw_block_cube(Arduino_GFX* g, int cx, int cy, int size,
                            uint16_t color, uint16_t fill) {
  const int r = size / 2;
  int tx1 = cx - r, ty1 = cy - r/2;
  int tx2 = cx,     ty2 = cy - r;
  int tx3 = cx + r, ty3 = cy - r/2;
  int tx4 = cx,     ty4 = cy;
  g->fillTriangle(tx1, ty1, tx2, ty2, tx4, ty4, fill);
  g->fillTriangle(tx2, ty2, tx3, ty3, tx4, ty4, fill);
  g->drawLine(tx1, ty1, tx2, ty2, color);
  g->drawLine(tx2, ty2, tx3, ty3, color);
  g->drawLine(tx3, ty3, tx4, ty4, color);
  g->drawLine(tx4, ty4, tx1, ty1, color);
  int lx1 = tx1, ly1 = ty1;
  int lx2 = tx4, ly2 = ty4;
  int lx3 = cx,  ly3 = cy + r;
  int lx4 = cx - r, ly4 = cy + r/2;
  g->fillTriangle(lx1, ly1, lx2, ly2, lx4, ly4, fill);
  g->fillTriangle(lx2, ly2, lx3, ly3, lx4, ly4, fill);
  g->drawLine(lx1, ly1, lx4, ly4, color);
  g->drawLine(lx4, ly4, lx3, ly3, color);
  g->drawLine(lx3, ly3, lx2, ly2, color);
  int rx1 = tx3, ry1 = ty3;
  int rx2 = tx4, ry2 = ty4;
  int rx3 = cx,  ry3 = cy + r;
  int rx4 = cx + r, ry4 = cy + r/2;
  g->fillTriangle(rx1, ry1, rx2, ry2, rx4, ry4, fill);
  g->fillTriangle(rx2, ry2, rx3, ry3, rx4, ry4, fill);
  g->drawLine(rx1, ry1, rx4, ry4, color);
  g->drawLine(rx4, ry4, rx3, ry3, color);
}

void Explorer::render(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);

  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(theme::ACCENT_ORANGE);
  g->setCursor(16, 18);
  g->print("BLOCKS");
  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  g->setCursor(ctx.w - 90, 24);
  g->print("no wifi");
  g->drawFastHLine(16, 52, ctx.w - 32, theme::DIVIDER);

  const int cube_size = 130;
  const int cube_cx = ctx.w / 2;
  const int cube_cy = 170;
  draw_block_cube(g, cube_cx, cube_cy, cube_size, theme::ACCENT_ORANGE, 0x2924);

  // Big value: dashes when no data, real height when we have it
  char h_str[16];
  if (s_have_data) snprintf(h_str, sizeof(h_str), "%lu", (unsigned long)s_block_height);
  else             snprintf(h_str, sizeof(h_str), "%s", "------");
  g->setTextSize(3);
  g->setTextColor(theme::TEXT_HI);
  int16_t hw = (int16_t)strlen(h_str) * 6 * 3;
  g->setCursor((ctx.w - hw) / 2, cube_cy + cube_size / 2 + 34);
  g->print(h_str);

  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* h_lbl = "block height";
  int16_t hlw = (int16_t)strlen(h_lbl) * 6;
  g->setCursor((ctx.w - hlw) / 2, cube_cy + cube_size / 2 + 66);
  g->print(h_lbl);

  const int row_y = ctx.h - 82;
  auto stat = [&](int x, const char* val, const char* lbl) {
    g->setTextSize(2);
    g->setTextColor(theme::TEXT_HI);
    int16_t vw = (int16_t)strlen(val) * 6 * 2;
    g->setCursor(x - vw / 2, row_y);
    g->print(val);
    g->setTextSize(1);
    g->setTextColor(theme::TEXT_LO);
    int16_t lw = (int16_t)strlen(lbl) * 6;
    g->setCursor(x - lw / 2, row_y + 22);
    g->print(lbl);
  };
  const char* fee_s = s_have_data ? "" : "--";
  const char* tx_s  = s_have_data ? "" : "--";
  const char* age_s = s_have_data ? "" : "--";
  char fee_buf[16], tx_buf[16], age_buf[16];
  if (s_have_data) {
    snprintf(fee_buf, sizeof(fee_buf), "%u", (unsigned)s_fee_rate);      fee_s = fee_buf;
    snprintf(tx_buf,  sizeof(tx_buf),  "%lu", (unsigned long)s_tx_count); tx_s  = tx_buf;
    snprintf(age_buf, sizeof(age_buf), "%lum", (unsigned long)(s_seconds_ago / 60));
    age_s = age_buf;
  }
  stat(ctx.w / 6,     fee_s, "sat/vB");
  stat(ctx.w / 2,     tx_s,  "txs");
  stat(5 * ctx.w / 6, age_s, "ago");

  g->setTextColor(theme::TEXT_LO);
  const char* hint = s_have_data ? "long-press = home"
                                 : "connect wifi in Setup app | long-press home";
  int16_t xw = (int16_t)strlen(hint) * 6;
  g->setCursor((ctx.w - xw) / 2, ctx.h - 18);
  g->print(hint);
}

bool Explorer::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TAP:
      render(ctx);
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return false;
  }
}

} // namespace mini
