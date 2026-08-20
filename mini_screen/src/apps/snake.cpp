#include "snake.h"
#include "../ui/kit.h"

#include <esp_random.h>
#include <Preferences.h>

namespace mini {

static constexpr const char* kNs = "snake";
static constexpr const char* kKHigh = "high";
static constexpr int kHudPx = 44;   // top HUD height

// Cell size derived so the grid centers on the screen with a small margin.
static inline int cell_px(int screen_w, int screen_h) {
  int usable_w = screen_w - 24;
  int usable_h = screen_h - kHudPx - 32;
  int cw = usable_w / Snake::GRID_W;
  int ch = usable_h / Snake::GRID_H;
  return (cw < ch) ? cw : ch;
}

static inline int grid_x0(int screen_w) {
  int cs = cell_px(screen_w, /*not used*/ 448);
  return (screen_w - cs * Snake::GRID_W) / 2;
}
static inline int grid_y0(int screen_h) {
  int cs = cell_px(/*not used*/ 368, screen_h);
  return kHudPx + (screen_h - kHudPx - cs * Snake::GRID_H) / 2;
}

void Snake::loadHigh() {
  Preferences p;
  if (!p.begin(kNs, /*ro=*/true)) return;
  high_ = p.getUInt(kKHigh, 0);
  p.end();
}

void Snake::saveHigh() {
  Preferences p;
  if (!p.begin(kNs, /*ro=*/false)) return;
  p.putUInt(kKHigh, high_);
  p.end();
}

void Snake::reset() {
  head_ = 4; tail_ = 0; len_ = 5;
  for (int i = 0; i < 5; ++i) {
    int x = 4 + i;
    int y = GRID_H / 2;
    seg_[i] = (uint16_t)(y * GRID_W + x);
  }
  dir_ = RIGHT; pending_dir_ = RIGHT;
  score_ = 0;
  step_ms_ = 180;
  placeFood();
}

void Snake::placeFood() {
  // Randomly pick until we find a cell not occupied by the snake.
  for (int tries = 0; tries < 200; ++tries) {
    int x = (int)(esp_random() % GRID_W);
    int y = (int)(esp_random() % GRID_H);
    uint16_t pos = (uint16_t)(y * GRID_W + x);
    bool clash = false;
    for (int i = 0; i < len_; ++i) {
      int idx = (tail_ + i) % MAX_LEN;
      if (seg_[idx] == pos) { clash = true; break; }
    }
    if (!clash) { food_x_ = x; food_y_ = y; return; }
  }
}

// Advance one grid step. Returns false on death.
bool Snake::advance() {
  dir_ = pending_dir_;
  int head_idx = (tail_ + len_ - 1) % MAX_LEN;
  int hx = seg_[head_idx] % GRID_W;
  int hy = seg_[head_idx] / GRID_W;
  switch (dir_) {
    case UP:    hy -= 1; break;
    case DOWN:  hy += 1; break;
    case LEFT:  hx -= 1; break;
    case RIGHT: hx += 1; break;
  }
  // Wall collisions
  if (hx < 0 || hx >= GRID_W || hy < 0 || hy >= GRID_H) return false;
  uint16_t new_head = (uint16_t)(hy * GRID_W + hx);
  // Self collision — check body except the tail (which is about to move).
  for (int i = 1; i < len_; ++i) {
    int idx = (tail_ + i) % MAX_LEN;
    if (seg_[idx] == new_head) return false;
  }
  bool ate = (hx == food_x_ && hy == food_y_);
  if (ate) {
    // Grow: keep tail, append new head
    int new_idx = (tail_ + len_) % MAX_LEN;
    seg_[new_idx] = new_head;
    ++len_;
    ++score_;
    if (score_ > high_) high_ = score_;
    // Speed up every 5 apples, min 60ms
    if ((score_ % 5) == 0 && step_ms_ > 60) step_ms_ -= 10;
    placeFood();
  } else {
    // Move: drop tail, add new head
    seg_[tail_] = new_head;
    tail_ = (tail_ + 1) % MAX_LEN;
    // Head is now conceptually the last, but we're using a rolling head slot
    // stored at (tail_ + len_ - 1) % MAX_LEN. Update by writing into the
    // freed slot:
    int prev_head = (tail_ + len_ - 1) % MAX_LEN;
    seg_[prev_head] = new_head;
  }
  return true;
}

void Snake::drawCell(AppContext& ctx, int x, int y, uint16_t color) {
  int cs = cell_px(ctx.w, ctx.h);
  int px = grid_x0(ctx.w) + x * cs;
  int py = grid_y0(ctx.h) + y * cs;
  ctx.gfx->fillRoundRect(px + 1, py + 1, cs - 2, cs - 2, cs / 4, color);
}

void Snake::drawHUD(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillRect(0, 0, ctx.w, kHudPx, theme::BG);
  theme::use_font(g, theme::FONT_HEADING_M);
  char sbuf[32];
  snprintf(sbuf, sizeof(sbuf), "%lu", (unsigned long)score_);
  ui::draw_top(g, sbuf, 16, 10, theme::ACCENT_GREEN);
  theme::use_font(g, theme::FONT_SMALL);
  char hbuf[32];
  snprintf(hbuf, sizeof(hbuf), "high %lu", (unsigned long)high_);
  int16_t hw = ui::text_w(g, hbuf);
  ui::draw_top(g, hbuf, ctx.w - hw - 16, 16, theme::TEXT_LO);
  // Divider
  g->drawFastHLine(16, kHudPx - 4, ctx.w - 32, theme::DIVIDER);
}

void Snake::drawFull(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);
  drawHUD(ctx);

  int cs = cell_px(ctx.w, ctx.h);
  int gx0 = grid_x0(ctx.w);
  int gy0 = grid_y0(ctx.h);
  // Playfield border
  g->drawRoundRect(gx0 - 3, gy0 - 3, cs * GRID_W + 6, cs * GRID_H + 6, 8,
                   theme::DIVIDER);

  // Food (magenta apple)
  drawCell(ctx, food_x_, food_y_, theme::ACCENT_MAGENTA);
  // Snake body
  for (int i = 0; i < len_; ++i) {
    int idx = (tail_ + i) % MAX_LEN;
    int x = seg_[idx] % GRID_W;
    int y = seg_[idx] / GRID_W;
    uint16_t c = (i == len_ - 1) ? theme::ACCENT_GREEN : 0x2FE4;
    drawCell(ctx, x, y, c);
  }

  // State overlay
  if (state_ == S_READY) {
    ui::screen_hint(ctx.gfx, ctx.w, ctx.h,
                    "swipe = start   ·   long-press = home");
  } else if (state_ == S_PAUSED) {
    theme::use_font(g, theme::FONT_HEADING_L);
    ui::draw_top_centered(g, "paused", ctx.w / 2, ctx.h / 2 - 16,
                          theme::ACCENT_YELLOW);
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top_centered(g, "tap to resume", ctx.w / 2, ctx.h / 2 + 20,
                          theme::TEXT_LO);
  } else if (state_ == S_DEAD) {
    theme::use_font(g, theme::FONT_HEADING_L);
    ui::draw_top_centered(g, "game over", ctx.w / 2, ctx.h / 2 - 30,
                          theme::ACCENT_RED);
    theme::use_font(g, theme::FONT_HEADING_M);
    char b[32];
    snprintf(b, sizeof(b), "score %lu   high %lu",
             (unsigned long)score_, (unsigned long)high_);
    ui::draw_top_centered(g, b, ctx.w / 2, ctx.h / 2 + 8, theme::TEXT_HI);
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top_centered(g, "tap or swipe to restart",
                          ctx.w / 2, ctx.h / 2 + 44, theme::TEXT_LO);
  } else {
    ui::screen_hint(ctx.gfx, ctx.w, ctx.h,
                    "swipe = turn  ·  tap = pause  ·  long-press = home");
  }
}

void Snake::onEnter(AppContext& ctx) {
  loadHigh();
  reset();
  state_ = S_READY;
  last_step_ms_ = millis();
  drawFull(ctx);
}

void Snake::onExit(AppContext&) {
  saveHigh();
}

void Snake::tick(AppContext& ctx, uint32_t now_ms) {
  if (state_ != S_PLAYING) return;
  if (now_ms - last_step_ms_ < step_ms_) return;
  last_step_ms_ = now_ms;
  bool alive = advance();
  if (!alive) {
    state_ = S_DEAD;
    saveHigh();
  }
  drawFull(ctx);
}

bool Snake::onGesture(AppContext& ctx, const Gesture& gest) {
  switch (gest.type) {
    case Gesture::SWIPE_UP:    if (dir_ != DOWN)  pending_dir_ = UP;    break;
    case Gesture::SWIPE_DOWN:  if (dir_ != UP)    pending_dir_ = DOWN;  break;
    case Gesture::SWIPE_LEFT:  if (dir_ != RIGHT) pending_dir_ = LEFT;  break;
    case Gesture::SWIPE_RIGHT: if (dir_ != LEFT)  pending_dir_ = RIGHT; break;
    case Gesture::TAP:
      if (state_ == S_PLAYING) { state_ = S_PAUSED; drawFull(ctx); return true; }
      if (state_ == S_PAUSED)  { state_ = S_PLAYING; drawFull(ctx); return true; }
      if (state_ == S_DEAD)    { reset(); state_ = S_READY; drawFull(ctx); return true; }
      return true;
    case Gesture::BUTTON_DOWN:
      if (state_ == S_PLAYING) { state_ = S_PAUSED; drawFull(ctx); return true; }
      if (state_ == S_PAUSED)  { state_ = S_PLAYING; drawFull(ctx); return true; }
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
  // Any swipe starts the game / restarts after death.
  if (state_ == S_READY || state_ == S_DEAD) {
    if (state_ == S_DEAD) reset();
    state_ = S_PLAYING;
    // The pending_dir_ was set above; make dir_ match immediately.
    dir_ = pending_dir_;
    last_step_ms_ = millis();
    drawFull(ctx);
  }
  return true;
}

} // namespace mini
