// Block Explorer — Bitcoin mempool-style block viewer.
//
// This is a v1 stub: it renders a stylized block card with placeholder height
// and fee data, so we can polish the visual language before wiring in real
// mempool.space fetches. Once WiFi is provisioned we'll replace kFake* with
// live values.
#pragma once
#include "../shell.h"

namespace mini {

class Explorer : public App {
 public:
  const char* name() const override { return "Blocks"; }
  AppKind     kind() const override { return AppKind::EXPLORER; }
  uint16_t    tint() const override { return 0xFCA0; }   // bitcoin orange

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  uint32_t last_pulse_ms_ = 0;
  uint8_t  pulse_phase_ = 0;

  void render(AppContext&);
};

} // namespace mini
