// Home screen: a 2xN grid of app tiles. Tap a tile to launch that app.
// The Home app itself is skipped when rendering tiles.
#pragma once
#include "../shell.h"

namespace mini {

class Home : public App {
 public:
  const char* name()  const override { return "Home"; }
  uint16_t    tint()  const override { return 0xFFFF; }

  void setShell(Shell* s) { shell_ = s; }

  void onEnter(AppContext& ctx) override;
  void tick(AppContext&, uint32_t) override {}
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  Shell* shell_ = nullptr;
  int    pressed_tile_ = -1;

  void render(AppContext& ctx);
  int  tileAt(AppContext& ctx, int16_t x, int16_t y);
  void tileRect(AppContext& ctx, int idx, int16_t& tx, int16_t& ty,
                int16_t& tw, int16_t& th);
  void drawTile(AppContext& ctx, int idx, bool pressed);
};

} // namespace mini
