// Steps — a peak-detection pedometer using the QMI8658 accelerometer.
//
// Algorithm (classic peak/valley detector):
//   1. Read raw accel (g-units), compute magnitude M = sqrt(x^2+y^2+z^2).
//   2. Feed M through a slow-EMA baseline (~1 Hz cutoff) and a fast-EMA
//      "smoothed" (~5 Hz cutoff). Deviation D = smoothed - baseline.
//   3. A step is registered when D crosses a positive peak threshold, then
//      falls below a hysteresis threshold, with a minimum time between
//      steps (~250 ms — no one steps faster than 4 Hz walking).
//
// Persistence:
//   - Total count is stored in NVS every 32 steps (limits flash wear).
//   - "Today's" count resets when we notice millis() has wrapped 24h since
//     last-day boundary. Without an RTC this is approximate but useful.
//
// UI:
//   - Big count in the center
//   - Ring showing progress toward daily goal (default 10,000)
//   - Small "today" number under the ring
//   - Tap to nudge (reset today; long-swipe = reset all)
#pragma once
#include "../shell.h"

namespace mini {

class Steps : public App {
 public:
  const char* name() const override { return "Steps"; }
  AppKind     kind() const override { return AppKind::STEPS; }
  uint16_t    tint() const override { return 0x37E0; }   // green

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  // Persistent
  uint32_t total_ = 0;
  uint32_t today_ = 0;
  uint32_t day_epoch_ms_ = 0;   // millis() value at which "today" began
  uint32_t goal_       = 10000;

  // Peak detector state
  float baseline_ = 1.0f;       // gravity magnitude, starts at 1 g
  float smoothed_ = 1.0f;
  bool  above_peak_ = false;
  uint32_t last_step_ms_ = 0;
  uint32_t last_read_ms_ = 0;
  uint32_t save_pending_ = 0;

  // UI cache
  uint32_t last_shown_total_ = 0;
  uint32_t last_shown_today_ = 0;

  void loadFromNvs(AppContext&);
  void maybeSave(AppContext&, bool force);
  void checkDailyRollover(uint32_t now_ms);
  void renderFull(AppContext&);
  void renderCount(AppContext&);
};

} // namespace mini
