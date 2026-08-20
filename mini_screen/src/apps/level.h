// Bubble Level — uses the IMU accelerometer to compute pitch/roll and draw a
// bullseye level. When both axes are within ±0.8° of horizontal we flash the
// bullseye green and (optionally) beep.
//
// Assumes the screen is roughly horizontal (face up). The Z-axis is used as
// the up-axis for computing angles from gravity.
#pragma once
#include "../shell.h"

namespace mini {

class Level : public App {
 public:
  const char* name() const override { return "Level"; }
  AppKind     kind() const override { return AppKind::LEVEL; }
  uint16_t    tint() const override { return 0xFFE0; }   // amber

  using ClickFn = void (*)();
  void setBeep(ClickFn c) { beep_ = c; }

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  ClickFn beep_ = nullptr;
  uint32_t last_read_ms_ = 0;
  uint32_t last_render_ms_ = 0;
  uint32_t last_beep_ms_ = 0;
  float pitch_deg_ = 0.0f;   // rotation around x axis (nose up/down)
  float roll_deg_  = 0.0f;   // rotation around y axis
  bool  currently_level_ = false;
  // For live pitch/roll trace
  int16_t last_bx_ = -9999, last_by_ = -9999;
  void render(AppContext&);
};

} // namespace mini
