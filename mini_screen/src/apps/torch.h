// Torch — a full-screen flashlight. Tap toggles on/off. Swipe left/right
// cycles between white, warm, red (night vision) and party rainbow. Swipe
// up/down changes brightness. Long-press returns to home.
#pragma once
#include "../shell.h"

namespace mini {

class Torch : public App {
 public:
  const char* name() const override { return "Torch"; }
  AppKind     kind() const override { return AppKind::TORCH; }
  uint16_t    tint() const override { return 0xFFE0; }   // amber

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t) override {}
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  uint8_t color_idx_    = 0;
  uint8_t brightness_   = 200;   // 0..255 (AMOLED backlight-like)
  bool    on_           = true;

  void applyBrightness(AppContext&);
  void render(AppContext&);
};

} // namespace mini
