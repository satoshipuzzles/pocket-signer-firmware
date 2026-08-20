#include "shell.h"

namespace mini {

void Shell::begin(AppContext ctx, App** apps, size_t n_apps, size_t home_idx) {
  ctx_ = ctx;
  apps_ = apps;
  n_apps_ = n_apps;
  home_idx_ = home_idx;
  current_ = home_idx;
  if (n_apps_ > 0) apps_[current_]->onEnter(ctx_);
}

void Shell::tick(uint32_t now_ms) {
  if (n_apps_ == 0) return;
  apps_[current_]->tick(ctx_, now_ms);
}

void Shell::deliverGesture(const Gesture& g) {
  if (n_apps_ == 0) return;
  bool handled = apps_[current_]->onGesture(ctx_, g);
  // Universal "back to home" on unhandled long-press. Home always handles
  // its own long-press (no-op), so this only fires from other apps.
  if (!handled && g.type == Gesture::LONG_PRESS && current_ != home_idx_) {
    switchTo(home_idx_);
  }
}

void Shell::switchTo(size_t idx) {
  if (idx >= n_apps_) return;
  if (idx == current_) return;
  apps_[current_]->onExit(ctx_);
  current_ = idx;
  apps_[current_]->onEnter(ctx_);
}

} // namespace mini
