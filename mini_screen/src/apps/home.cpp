#include "home.h"
#include "../ui/kit.h"

namespace mini {

// Layout: 3 columns of tiles, evenly spaced. Only apps whose showInHome()
// returns true are shown; the rest live "under the hood" and are reachable
// via other UI or serial-debug. A small settings-gear in the header's top
// right corner opens the WiFi (or whichever apps we decide to expose as
// "utilities") on tap.
static constexpr int kCols     = 3;
static constexpr int kMargin   = 14;
static constexpr int kGap      = 10;
static constexpr int kGridTop  = ui::kHeaderH + 12;
static constexpr int kGridBot  = ui::kHintH;

// Gear tap zone (upper-right of the header).
static constexpr int16_t kGearSize = 40;
static constexpr int16_t kGearPad  = 8;

void Home::onEnter(AppContext& ctx) {
  pressed_tile_ = -1;
  render(ctx);
}

// Returns count of home-visible apps (excluding Home itself at idx 0).
static int visible_app_count(Shell* shell) {
  if (!shell) return 0;
  int n = 0;
  for (size_t i = 1; i < shell->count(); ++i) {
    if (shell->at(i)->showInHome()) ++n;
  }
  return n;
}

// Map a tile-index (0..N_visible-1) to a shell index (1..count-1) by
// skipping apps whose showInHome() is false.
static int shell_index_for_tile(Shell* shell, int tile_idx) {
  if (!shell) return -1;
  int seen = 0;
  for (size_t i = 1; i < shell->count(); ++i) {
    if (!shell->at(i)->showInHome()) continue;
    if (seen == tile_idx) return (int)i;
    ++seen;
  }
  return -1;
}

void Home::tileRect(AppContext& ctx, int idx, int16_t& tx, int16_t& ty,
                    int16_t& tw, int16_t& th) {
  const int n_tiles = visible_app_count(shell_);
  const int rows = (n_tiles + kCols - 1) / kCols;
  const int usable_w = ctx.w - 2 * kMargin - (kCols - 1) * kGap;
  const int usable_h = ctx.h - kGridTop - kGridBot - (rows > 0 ? (rows - 1) * kGap : 0);
  tw = usable_w / kCols;
  th = rows > 0 ? usable_h / rows : usable_h;
  const int c = idx % kCols;
  const int r = idx / kCols;
  tx = kMargin + c * (tw + kGap);
  ty = kGridTop + r * (th + kGap);
}

int Home::tileAt(AppContext& ctx, int16_t x, int16_t y) {
  const int n_tiles = visible_app_count(shell_);
  for (int i = 0; i < n_tiles; ++i) {
    int16_t tx, ty, tw, th;
    tileRect(ctx, i, tx, ty, tw, th);
    if (x >= tx && x < tx + tw && y >= ty && y < ty + th) return i;
  }
  return -1;
}

// Simple 3x3 gear silhouette in the header. Not tappable-precise but the
// hit zone is generous.
static void draw_gear(Arduino_GFX* g, int16_t cx, int16_t cy, int16_t size,
                      uint16_t color) {
  const int16_t r_outer = size / 2;
  const int16_t r_inner = size * 30 / 100;
  const int16_t tooth   = size * 12 / 100;
  // 8 rectangular "teeth" around a ring
  for (int i = 0; i < 8; ++i) {
    float ang = (float)i * ((float)M_PI / 4.0f);
    int16_t x1 = cx + (int16_t)(cosf(ang) * r_outer);
    int16_t y1 = cy + (int16_t)(sinf(ang) * r_outer);
    g->fillRect(x1 - tooth / 2, y1 - tooth / 2, tooth, tooth, color);
  }
  g->fillCircle(cx, cy, r_outer - tooth / 2, color);
  g->fillCircle(cx, cy, r_inner, theme::BG);
}

void Home::drawTile(AppContext& ctx, int idx, bool pressed) {
  int16_t tx, ty, tw, th;
  tileRect(ctx, idx, tx, ty, tw, th);
  int shell_idx = shell_index_for_tile(shell_, idx);
  if (shell_idx < 0) return;
  App* a = shell_->at((size_t)shell_idx);
  uint16_t tint = a->tint();
  auto* g = ctx.gfx;

  ui::card(g, tx, ty, tw, th,
           pressed ? theme::SURFACE_HI : theme::SURFACE, /*border=*/0);
  g->fillRoundRect(tx + 8, ty + th - 6, tw - 16, 3, 2, tint);

  const int16_t icon_size = min<int16_t>(tw, th) * 50 / 100;
  const int16_t icon_x = tx + (tw - icon_size) / 2;
  const int16_t icon_y = ty + 12;

  switch (a->kind()) {
    case AppKind::DICE:     theme::icon_dice  (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::SIGNER:   theme::icon_key   (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::CUBE:     theme::icon_cube  (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::VOICE:    theme::icon_mic   (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::TORCH:    theme::icon_torch (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::EXPLORER: theme::icon_block (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::STEPS:    theme::icon_shoe  (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::LEVEL:    theme::icon_level (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::PIANO:    theme::icon_piano (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::ETCH:     theme::icon_pencil(g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::WIFI:     theme::icon_wifi  (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::NOTES:    theme::icon_notes (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::GENESIS:  theme::icon_eye   (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::SNAKE:    theme::icon_snake (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::VESPERS:  theme::icon_ear   (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::PHOTOS:   theme::icon_photo (g, icon_x, icon_y, icon_size, tint); break;
    case AppKind::VIDEOS:   theme::icon_video (g, icon_x, icon_y, icon_size, tint); break;
    default:
      g->drawRoundRect(icon_x, icon_y, icon_size, icon_size, icon_size / 6, tint);
      break;
  }

  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top_centered(g, a->name(), tx + tw / 2, ty + th - 20, theme::TEXT_HI);
}

void Home::render(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "home", theme::ACCENT_CYAN);

  // Gear icon top-right, over the header — small utility affordance.
  draw_gear(g, ctx.w - kGearPad - kGearSize / 2, kGearPad + kGearSize / 2,
            kGearSize - 12, theme::TEXT_LO);

  const int n_tiles = visible_app_count(shell_);
  for (int i = 0; i < n_tiles; ++i) {
    drawTile(ctx, i, pressed_tile_ == i);
  }

  ui::screen_hint(g, ctx.w, ctx.h, "tap a tile   ·   gear = wifi setup");
}

bool Home::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TAP: {
      // Check gear first.
      const int16_t gx = ctx.w - kGearPad - kGearSize;
      const int16_t gy = 0;
      if (g.x >= gx && g.x < ctx.w && g.y >= gy && g.y < gy + kGearSize + 8) {
        // Find the WiFi app and jump to it.
        for (size_t i = 1; i < shell_->count(); ++i) {
          if (shell_->at(i)->kind() == AppKind::WIFI) {
            shell_->switchTo(i);
            return true;
          }
        }
        return true;
      }
      int idx = tileAt(ctx, g.x, g.y);
      if (idx >= 0 && shell_) {
        int shell_idx = shell_index_for_tile(shell_, idx);
        if (shell_idx > 0) {
          pressed_tile_ = idx;
          drawTile(ctx, idx, true);
          delay(80);
          shell_->switchTo((size_t)shell_idx);
          pressed_tile_ = -1;
        }
        return true;
      }
      return true;
    }
    case Gesture::LONG_PRESS:
      render(ctx);
      return true;
    default:
      return false;
  }
}

} // namespace mini
