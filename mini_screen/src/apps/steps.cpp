#include "steps.h"
#include "../ui/theme.h"

#include <math.h>
#include <Preferences.h>

namespace mini {

// NVS namespace + keys
static constexpr const char* kNs      = "steps";
static constexpr const char* kKTotal  = "total";
static constexpr const char* kKToday  = "today";
static constexpr const char* kKDayMs  = "dayms";
static constexpr const char* kKGoal   = "goal";

// Detector tuning — adjust these if steps aren't registering right.
static constexpr float    kSmoothAlpha    = 0.30f;   // fast EMA (~5 Hz @ 50Hz sample)
static constexpr float    kBaselineAlpha  = 0.02f;   // slow EMA (~1 Hz)
static constexpr float    kPeakThreshold  = 0.14f;   // g above baseline to count as a peak
static constexpr float    kValleyThresh   = 0.05f;   // must fall back below this
static constexpr uint32_t kMinStepIntMs   = 250;
static constexpr uint32_t kSampleIntMs    = 20;      // ~50 Hz polling
static constexpr uint32_t kDayMs          = 24u * 60u * 60u * 1000u;

void Steps::loadFromNvs(AppContext&) {
  Preferences p;
  if (!p.begin(kNs, /*ro=*/false)) return;
  total_        = p.getUInt(kKTotal, 0);
  today_        = p.getUInt(kKToday, 0);
  day_epoch_ms_ = p.getUInt(kKDayMs, 0);
  goal_         = p.getUInt(kKGoal, 10000);
  p.end();
}

void Steps::maybeSave(AppContext&, bool force) {
  if (!force && (total_ % 32) != 0) return;
  Preferences p;
  if (!p.begin(kNs, /*ro=*/false)) return;
  p.putUInt(kKTotal, total_);
  p.putUInt(kKToday, today_);
  p.putUInt(kKDayMs, day_epoch_ms_);
  p.putUInt(kKGoal,  goal_);
  p.end();
}

void Steps::checkDailyRollover(uint32_t now_ms) {
  // Without RTC, "day" is a rolling 24h window since day_epoch_ms_. This
  // means "today" is really "the last 24h since last reset" — imperfect, but
  // predictable and doesn't over-count.
  if (day_epoch_ms_ == 0) day_epoch_ms_ = now_ms;
  if ((now_ms - day_epoch_ms_) >= kDayMs) {
    today_ = 0;
    day_epoch_ms_ = now_ms;
    maybeSave(*(AppContext*)nullptr, true);
  }
}

void Steps::onEnter(AppContext& ctx) {
  loadFromNvs(ctx);
  last_read_ms_ = millis();
  last_step_ms_ = 0;
  above_peak_ = false;
  last_shown_total_ = ~0u;
  last_shown_today_ = ~0u;
  renderFull(ctx);
}

void Steps::onExit(AppContext& ctx) {
  maybeSave(ctx, /*force=*/true);
}

void Steps::tick(AppContext& ctx, uint32_t now_ms) {
  if (!ctx.imu) return;
  if (now_ms - last_read_ms_ < kSampleIntMs) return;
  last_read_ms_ = now_ms;

  float ax, ay, az;
  if (!ctx.imu->getAccelerometer(ax, ay, az)) return;
  float mag = sqrtf(ax * ax + ay * ay + az * az);

  smoothed_ = kSmoothAlpha * mag + (1.0f - kSmoothAlpha) * smoothed_;
  baseline_ = kBaselineAlpha * smoothed_ + (1.0f - kBaselineAlpha) * baseline_;

  const float dev = smoothed_ - baseline_;

  bool step_registered = false;
  if (!above_peak_) {
    if (dev > kPeakThreshold &&
        (now_ms - last_step_ms_) >= kMinStepIntMs) {
      above_peak_ = true;
    }
  } else {
    if (dev < kValleyThresh) {
      above_peak_ = false;
      total_ += 1;
      today_ += 1;
      last_step_ms_ = now_ms;
      step_registered = true;
    }
  }

  checkDailyRollover(now_ms);

  if (step_registered) {
    maybeSave(ctx, /*force=*/false);
    // Update just the numbers, not the whole ring, for smoothness.
    if (total_ != last_shown_total_ || today_ != last_shown_today_) {
      renderCount(ctx);
    }
  }
}

// ---- Rendering ------------------------------------------------------------

// Draw a segmented progress ring: `progress` in [0..1]. We render arcs as
// short line segments around a circle for a clean modern feel.
static void draw_ring(Arduino_GFX* g, int cx, int cy, int r,
                      float progress, uint16_t on_c, uint16_t off_c) {
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  const int N = 60;
  const int on = (int)(N * progress + 0.5f);
  for (int i = 0; i < N; ++i) {
    float a1 = -1.5707963f + (float)i / (float)N * 6.2831853f;
    float a2 = -1.5707963f + (float)(i + 1) / (float)N * 6.2831853f;
    for (int t = -1; t <= 1; ++t) {
      int x1 = cx + (int)(cosf(a1) * (r + t));
      int y1 = cy + (int)(sinf(a1) * (r + t));
      int x2 = cx + (int)(cosf(a2) * (r + t));
      int y2 = cy + (int)(sinf(a2) * (r + t));
      g->drawLine(x1, y1, x2, y2, i < on ? on_c : off_c);
    }
  }
}

void Steps::renderFull(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);

  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(theme::ACCENT_GREEN);
  g->setCursor(16, 18);
  g->print("STEPS");
  g->drawFastHLine(16, 52, ctx.w - 32, theme::DIVIDER);

  const int cx = ctx.w / 2;
  const int cy = ctx.h / 2 + 6;
  const int r = min<int>(ctx.w, ctx.h) / 2 - 40;

  const float prog = goal_ ? (float)today_ / (float)goal_ : 0.0f;
  draw_ring(g, cx, cy, r, prog, theme::ACCENT_GREEN, theme::DIVIDER);

  renderCount(ctx);

  g->setFont(nullptr);
  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  char buf[48];
  snprintf(buf, sizeof(buf), "goal %lu   tap=reset day", (unsigned long)goal_);
  int16_t bw = (int16_t)strlen(buf) * 6;
  g->setCursor((ctx.w - bw) / 2, ctx.h - 20);
  g->print(buf);
}

void Steps::renderCount(AppContext& ctx) {
  auto* g = ctx.gfx;
  const int cx = ctx.w / 2;
  const int cy = ctx.h / 2 + 6;

  // Clear number region (interior of ring)
  const int r_inner = min<int>(ctx.w, ctx.h) / 2 - 52;
  g->fillCircle(cx, cy, r_inner - 4, theme::BG);

  // Big today count
  char t_str[16];
  snprintf(t_str, sizeof(t_str), "%lu", (unsigned long)today_);
  g->setFont(nullptr);
  g->setTextSize(4);
  g->setTextColor(theme::TEXT_HI);
  int16_t tw = (int16_t)strlen(t_str) * 6 * 4;
  g->setCursor(cx - tw / 2, cy - 20);
  g->print(t_str);

  // "today" label
  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* lbl = "today";
  int16_t lw = (int16_t)strlen(lbl) * 6;
  g->setCursor(cx - lw / 2, cy + 18);
  g->print(lbl);

  // Total below
  char tot_str[32];
  snprintf(tot_str, sizeof(tot_str), "total %lu", (unsigned long)total_);
  g->setTextSize(1);
  int16_t xw = (int16_t)strlen(tot_str) * 6;
  g->setCursor(cx - xw / 2, cy + 32);
  g->setTextColor(theme::TEXT_MID);
  g->print(tot_str);

  last_shown_today_ = today_;
  last_shown_total_ = total_;
}

bool Steps::onGesture(AppContext& ctx, const Gesture& gest) {
  switch (gest.type) {
    case Gesture::TAP:
      today_ = 0;
      day_epoch_ms_ = millis();
      maybeSave(ctx, /*force=*/true);
      renderFull(ctx);
      return true;
    case Gesture::SWIPE_LEFT:
    case Gesture::SWIPE_RIGHT: {
      // Cycle goals: 5000 / 8000 / 10000 / 15000 / 20000
      static const uint32_t opts[] = { 5000, 8000, 10000, 15000, 20000 };
      static const int n = sizeof(opts) / sizeof(opts[0]);
      int cur = 2;
      for (int i = 0; i < n; ++i) if (opts[i] == goal_) { cur = i; break; }
      cur = (gest.type == Gesture::SWIPE_LEFT) ? (cur + 1) % n : (cur + n - 1) % n;
      goal_ = opts[cur];
      maybeSave(ctx, /*force=*/true);
      renderFull(ctx);
      return true;
    }
    case Gesture::SHAKE:
      // Shake to reset total (long-press-like emergency reset)
      total_ = 0;
      today_ = 0;
      day_epoch_ms_ = millis();
      maybeSave(ctx, /*force=*/true);
      renderFull(ctx);
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return false;
  }
}

} // namespace mini
