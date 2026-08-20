// Dice app — a slimmer port of the standalone WM_Dice sketch.
//
// Behavior (unchanged from the standalone version):
//   * Any-direction swipe rolls the die.
//   * IMU shake rolls the die.
//   * Tap the die to cycle die color; tap the background to cycle bg color.
//   * Commit + reveal for each roll is emitted over Serial only (audit trail).
//   * Colors persist in NVS.
//
// Owned by the shell — see mini_screen.ino for registration.
#pragma once
#include "../shell.h"

namespace mini {

class Dice : public App {
 public:
  const char* name() const override { return "Dice"; }
  AppKind     kind() const override { return AppKind::DICE; }
  uint16_t    tint() const override { return 0xFCA0; }   // warm orange

  // Audio driver hooks, set by the sketch during setup(). Optional.
  using SfxFn = void (*)();
  void setAudio(SfxFn click, SfxFn thump) { sfx_click_ = click; sfx_thump_ = thump; }

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  uint8_t face_ = 1;
  uint8_t die_color_idx_ = 0;
  uint8_t bg_color_idx_  = 0;
  bool    rolling_ = false;
  SfxFn   sfx_click_ = nullptr;
  SfxFn   sfx_thump_ = nullptr;

  uint16_t dieColor() const;
  uint16_t bgColor()  const;
  void     savePrefs(AppContext&);

  // Full-screen paint (background + die). Used on entry / color change.
  void renderFull(AppContext&);
  // Redraws ONLY the die rectangle (no flicker on the rest of the screen).
  void renderDie(AppContext&, uint8_t face);

  void     roll(AppContext&);
};

} // namespace mini
