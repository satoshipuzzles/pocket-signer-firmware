// Piano — draws a one-octave keyboard (7 white keys + 5 black keys) and
// plays a short sine burst through the ES8311 speaker when a key is
// pressed. Uses TOUCH_DOWN to trigger the note (feels responsive) and
// TOUCH_UP to un-highlight. Swipe up/down to shift octave (±1).
#pragma once
#include "../shell.h"

namespace mini {

class Piano : public App {
 public:
  const char* name() const override { return "Piano"; }
  AppKind     kind() const override { return AppKind::PIANO; }
  uint16_t    tint() const override { return 0xF81F; }   // magenta

  // Play a mono int16 buffer through the speaker.
  using PlayFn = void (*)(const int16_t*, size_t);
  void setAudio(PlayFn p) { play_ = p; }

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t) override {}
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  PlayFn  play_ = nullptr;
  int8_t  octave_ = 0;    // -1, 0, +1 offset from base
  int8_t  last_pressed_ = -1;   // 0..12, -1 = none
  void renderFull(AppContext&);
  void highlightKey(AppContext&, int idx, bool on);
  int  hitTest(AppContext&, int16_t x, int16_t y) const;
  void playNote(int idx);
};

} // namespace mini
