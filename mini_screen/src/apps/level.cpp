#include "level.h"
#include "../ui/theme.h"

#include <math.h>

namespace mini {

static constexpr float kLevelDeadband = 0.8f;   // ±deg to be considered "level"
static constexpr uint32_t kSampleMs   = 20;     // 50 Hz
static constexpr uint32_t kRenderMs   = 50;     // ~20 Hz redraw
static constexpr uint32_t kBeepCooldownMs = 800;

void Level::onEnter(AppContext& ctx) {
  last_read_ms_ = millis();
  last_render_ms_ = 0;
  last_beep_ms_ = 0;
  last_bx_ = last_by_ = -9999;
  render(ctx);
}

void Level::tick(AppContext& ctx, uint32_t now_ms) {
  if (!ctx.imu) return;
  if (now_ms - last_read_ms_ < kSampleMs) return;
  last_read_ms_ = now_ms;

  float ax, ay, az;
  if (!ctx.imu->getAccelerometer(ax, ay, az)) return;

  // Pitch = tilt front/back = atan2(ax, sqrt(ay^2 + az^2))
  // Roll  = tilt left/right = atan2(ay, az)
  // Values are in g-units — magnitudes don't need normalization for atan2.
  const float p = atan2f(ax, sqrtf(ay * ay + az * az)) * (180.0f / (float)M_PI);
  const float r = atan2f(ay, az) * (180.0f / (float)M_PI);

  // Light smoothing so the bubble doesn't jitter.
  pitch_deg_ = 0.7f * pitch_deg_ + 0.3f * p;
  roll_deg_  = 0.7f * roll_deg_  + 0.3f * r;

  const bool level_now = fabsf(pitch_deg_) < kLevelDeadband &&
                         fabsf(roll_deg_)  < kLevelDeadband;

  if (level_now && !currently_level_) {
    // Transitioned to level — beep once (throttled)
    if (beep_ && (now_ms - last_beep_ms_) > kBeepCooldownMs) {
      beep_();
      last_beep_ms_ = now_ms;
    }
  }
  currently_level_ = level_now;

  if (now_ms - last_render_ms_ >= kRenderMs) {
    last_render_ms_ = now_ms;
    render(ctx);
  }
}

void Level::render(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);

  // Header
  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(currently_level_ ? theme::ACCENT_GREEN : theme::ACCENT_YELLOW);
  g->setCursor(16, 18);
  g->print("LEVEL");
  g->drawFastHLine(16, 52, ctx.w - 32, theme::DIVIDER);

  const int cx = ctx.w / 2;
  const int cy = ctx.h / 2 + 4;
  const int r_out = min<int>(ctx.w, ctx.h) / 2 - 40;
  const int r_deadband = 14;

  // Concentric rings (bullseye)
  const uint16_t ring_c = currently_level_ ? theme::ACCENT_GREEN : theme::DIVIDER;
  g->drawCircle(cx, cy, r_out,      ring_c);
  g->drawCircle(cx, cy, r_out - 1,  ring_c);
  g->drawCircle(cx, cy, r_out * 2 / 3, theme::DIVIDER);
  g->drawCircle(cx, cy, r_out / 3, theme::DIVIDER);

  // Deadband target circle in the center
  g->drawCircle(cx, cy, r_deadband, currently_level_ ? theme::ACCENT_GREEN : theme::TEXT_LO);
  g->drawCircle(cx, cy, r_deadband + 1, currently_level_ ? theme::ACCENT_GREEN : theme::TEXT_LO);

  // Crosshair
  g->drawFastHLine(cx - r_out - 6, cy, r_out * 2 + 12, theme::DIVIDER);
  g->drawFastVLine(cx, cy - r_out - 6, r_out * 2 + 12, theme::DIVIDER);

  // Bubble position: map ±90° → ±(r_out - 8)
  const float scale = (float)(r_out - 8) / 45.0f;   // ±45° = edge
  int16_t bx = cx + (int16_t)(roll_deg_  * scale);
  int16_t by = cy - (int16_t)(pitch_deg_ * scale);   // negative: nose-up moves bubble up
  // Clamp
  if (bx < cx - r_out + 8) bx = cx - r_out + 8;
  if (bx > cx + r_out - 8) bx = cx + r_out - 8;
  if (by < cy - r_out + 8) by = cy - r_out + 8;
  if (by > cy + r_out - 8) by = cy + r_out - 8;

  const uint16_t bubble_c = currently_level_ ? theme::ACCENT_GREEN : theme::ACCENT_YELLOW;
  g->fillCircle(bx, by, 12, bubble_c);
  g->drawCircle(bx, by, 13, theme::BG);

  // Readout
  g->setFont(nullptr);
  g->setTextSize(2);
  char row1[32], row2[32];
  snprintf(row1, sizeof(row1), "P %+5.1f", pitch_deg_);
  snprintf(row2, sizeof(row2), "R %+5.1f", roll_deg_);
  g->setTextColor(theme::TEXT_HI);
  int16_t w1 = (int16_t)strlen(row1) * 6 * 2;
  int16_t w2 = (int16_t)strlen(row2) * 6 * 2;
  g->setCursor((ctx.w - w1) / 2, ctx.h - 84);
  g->print(row1);
  g->setCursor((ctx.w - w2) / 2, ctx.h - 58);
  g->print(row2);

  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* hint = "long-press = home";
  int16_t hw = (int16_t)strlen(hint) * 6;
  g->setCursor((ctx.w - hw) / 2, ctx.h - 20);
  g->print(hint);
}

bool Level::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::LONG_PRESS: return false;
    default: return true;   // swallow — level is view-only
  }
}

} // namespace mini
