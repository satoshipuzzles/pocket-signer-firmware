// Snake — classic snake game on a 16×20 tile grid.
//
// Controls:
//   * Swipe up/down/left/right to change direction (no U-turns).
//   * Tap to pause / resume.
//   * Long-press: return home.
//   * BUTTON_DOWN (side button): also pause/resume, handy for one-handed.
//
// Persistence: high score saved to NVS.
#pragma once
#include "../shell.h"

namespace mini {

class Snake : public App {
 public:
  const char* name() const override { return "Snake"; }
  AppKind     kind() const override { return AppKind::SNAKE; }
  uint16_t    tint() const override { return 0x37E0; }   // green

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

  static constexpr int GRID_W = 16;
  static constexpr int GRID_H = 20;
  static constexpr int MAX_LEN = GRID_W * GRID_H;

 private:

  enum Dir : uint8_t { UP = 0, RIGHT, DOWN, LEFT };
  enum State : uint8_t { S_READY, S_PLAYING, S_PAUSED, S_DEAD };

  // Ring buffer of segments (head = seg_[head_], tail-first at seg_[tail_])
  uint16_t seg_[MAX_LEN];   // packed (y * GRID_W + x)
  int      head_ = 0, tail_ = 0, len_ = 0;
  Dir      dir_ = RIGHT, pending_dir_ = RIGHT;
  int      food_x_ = 0, food_y_ = 0;
  uint32_t step_ms_ = 180;
  uint32_t last_step_ms_ = 0;
  uint32_t score_ = 0;
  uint32_t high_ = 0;
  State    state_ = S_READY;

  void reset();
  void placeFood();
  bool advance();
  void drawFull(AppContext&);
  void drawCell(AppContext&, int x, int y, uint16_t color);
  void drawHUD(AppContext&);
  void loadHigh();
  void saveHigh();
};

} // namespace mini
