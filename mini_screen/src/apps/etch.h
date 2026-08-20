// Etch — draw with your finger; shake the device to erase (just like the
// classic red toy). Long-press returns home. Swipe up/down cycles pen color
// (subtle so we don't reset the canvas mid-swipe).
#pragma once
#include "../shell.h"

namespace mini {

class Etch : public App {
 public:
  const char* name() const override { return "Etch"; }
  AppKind     kind() const override { return AppKind::ETCH; }
  uint16_t    tint() const override { return 0x05FF; }   // cyan

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  int16_t last_x_ = -1, last_y_ = -1;
  uint8_t color_idx_ = 0;
  void clearCanvas(AppContext&);
  void drawHeader(AppContext&);
};

} // namespace mini
