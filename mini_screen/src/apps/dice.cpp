#include "dice.h"
#include "../ui/theme.h"
#include <esp_system.h>
#include <esp_random.h>
#include "mbedtls/sha256.h"

namespace mini {

// Palette — tuned for the AMOLED. Slightly desaturated to look less "candy".
static const uint16_t kDiePalette[] = {
    0xFCA0, // warm orange (default)
    0xFFFF, // pure white
    0x05FF, // cyan
    0xF945, // salmon red
    0x37E0, // spring green
    0xC61F, // periwinkle
    0xFFE0, // yellow
    0xF81F, // magenta
};
static const uint16_t kBgPalette[] = {
    0x10A2, // deep charcoal (default)
    0x0000, // pure black
    0x2124, // mid gray
    0x0011, // deep navy
    0x1A03, // forest
    0x2000, // wine
};
static constexpr int kNumDieColors = sizeof(kDiePalette) / sizeof(kDiePalette[0]);
static constexpr int kNumBgColors  = sizeof(kBgPalette)  / sizeof(kBgPalette[0]);

// Fresh nonce/commit per session — audit trail still binds each roll.
static uint8_t s_commit[32] = {0};
static uint8_t s_nonce[32]  = {0};
static uint32_t s_roll_n    = 0;

static void mint_commit() {
  esp_fill_random(s_nonce, 32);
  mbedtls_sha256(s_nonce, 32, s_commit, 0);
}

// Auto-pick a pip color that stays legible on any die color.
static uint16_t pip_color_for(uint16_t die) {
  uint8_t r = (die >> 11) & 0x1F;
  uint8_t g = (die >> 5)  & 0x3F;
  uint8_t b =  die        & 0x1F;
  uint16_t luma = (r * 3 + g * 3 + b * 2);
  return (luma > 80) ? 0x0000 : 0xFFFF;
}

// ---- geometry ------------------------------------------------------------
// The die is a centered square with generous corner radius. Everything else
// (the background, the header) sits *outside* this square, so animation
// frames only need to redraw this rectangle — no full-screen fills, no flicker.
static inline int die_size(int w, int h) { return min(w, h) - 108; }
static inline int die_x(int w, int h)    { return (w - die_size(w, h)) / 2; }
static inline int die_y(int w, int h)    { return (h - die_size(w, h)) / 2; }

// Draws a single face inside the die rectangle (no screen fill).
static void draw_face_local(Arduino_GFX* g, int cx, int cy, int sz,
                            uint8_t face, uint16_t die_c, uint16_t pip_c,
                            uint16_t bg_c) {
  const int x = cx - sz / 2, y = cy - sz / 2;
  // Clear the die rectangle first (with a slight outer bg ring)
  g->fillRoundRect(x, y, sz, sz, sz / 7, die_c);
  // Subtle inner border for depth
  g->drawRoundRect(x, y, sz, sz, sz / 7, pip_c);

  const int r = sz / 12;
  const int q = sz / 4;
  auto pip = [&](int px, int py) { g->fillCircle(px, py, r, pip_c); };
  const bool tl = face == 4 || face == 5 || face == 6;
  const bool tr = face >= 2;
  const bool ml = face == 6;
  const bool mm = face == 1 || face == 3 || face == 5;
  const bool mr = face == 6;
  const bool bl = face >= 2;
  const bool br = face == 4 || face == 5 || face == 6;
  if (tl) pip(x + q,       y + q);
  if (tr) pip(x + sz - q,  y + q);
  if (ml) pip(x + q,       y + sz / 2);
  if (mm) pip(x + sz / 2,  y + sz / 2);
  if (mr) pip(x + sz - q,  y + sz / 2);
  if (bl) pip(x + q,       y + sz - q);
  if (br) pip(x + sz - q,  y + sz - q);
}

uint16_t Dice::dieColor() const { return kDiePalette[die_color_idx_ % kNumDieColors]; }
uint16_t Dice::bgColor()  const { return kBgPalette[bg_color_idx_ % kNumBgColors]; }

void Dice::savePrefs(AppContext& ctx) {
  if (!ctx.prefs) return;
  ctx.prefs->begin("dice", false);
  ctx.prefs->putUChar("die_c", die_color_idx_);
  ctx.prefs->putUChar("bg_c",  bg_color_idx_);
  ctx.prefs->putUChar("face",  face_);
  ctx.prefs->end();
}

void Dice::renderDie(AppContext& ctx, uint8_t face) {
  auto* g = ctx.gfx;
  const int sz = die_size(ctx.w, ctx.h);
  const int cx = ctx.w / 2;
  const int cy = ctx.h / 2;
  draw_face_local(g, cx, cy, sz, face, dieColor(), pip_color_for(dieColor()), bgColor());
}

void Dice::renderFull(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(bgColor());
  renderDie(ctx, face_);
}

// ---- lifecycle -----------------------------------------------------------

void Dice::onEnter(AppContext& ctx) {
  if (ctx.prefs) {
    ctx.prefs->begin("dice", false);
    die_color_idx_ = ctx.prefs->getUChar("die_c", 0);
    bg_color_idx_  = ctx.prefs->getUChar("bg_c",  0);
    face_          = ctx.prefs->getUChar("face",  1);
    ctx.prefs->end();
  }
  if (s_roll_n == 0) mint_commit();
  renderFull(ctx);
}

void Dice::tick(AppContext&, uint32_t) {}

// ---- provable roll -------------------------------------------------------

void Dice::roll(AppContext& ctx) {
  if (rolling_) return;
  rolling_ = true;

  // Motion entropy: snapshot IMU + hardware entropy.
  uint8_t motion[64];
  esp_fill_random(motion, sizeof(motion));
  if (ctx.imu && ctx.imu->getDataReady()) {
    float ax, ay, az, gx, gy, gz;
    ctx.imu->getAccelerometer(ax, ay, az);
    ctx.imu->getGyroscope(gx, gy, gz);
    memcpy(motion + 0,  &ax, 4); memcpy(motion + 4,  &ay, 4); memcpy(motion + 8,  &az, 4);
    memcpy(motion + 12, &gx, 4); memcpy(motion + 16, &gy, 4); memcpy(motion + 20, &gz, 4);
  }
  esp_fill_random(motion + 24, 40);

  // Tumble: 16 fast face flashes with ease-out. Each frame we redraw ONLY the
  // die rectangle (no full-screen fill) → no flicker on the background.
  const int frames = 16;
  for (int i = 0; i < frames; ++i) {
    uint8_t next;
    do {
      esp_fill_random(&next, 1);
      next = (next % 6) + 1;
    } while (next == face_);
    face_ = next;
    renderDie(ctx, face_);
    if (sfx_click_) sfx_click_();
    // Quadratic ease-out: 22ms .. ~120ms
    const int t = i * 100 / frames;
    delay(22 + (t * t) / 90);
  }

  // Reveal + mix into seed.
  mbedtls_sha256_context sh;
  mbedtls_sha256_init(&sh);
  mbedtls_sha256_starts(&sh, 0);
  mbedtls_sha256_update(&sh, s_nonce, 32);
  mbedtls_sha256_update(&sh, motion, sizeof(motion));
  uint8_t seed[32];
  mbedtls_sha256_finish(&sh, seed);
  mbedtls_sha256_free(&sh);

  // Rejection sample to keep face uniform.
  uint32_t w = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) |
               ((uint32_t)seed[2] << 8)  |  (uint32_t)seed[3];
  const uint32_t LIMIT = (uint32_t)(0x100000000ULL - (0x100000000ULL % 6));
  int off = 0;
  while (w >= LIMIT && off + 8 <= 32) {
    off += 4;
    w = ((uint32_t)seed[off] << 24) | ((uint32_t)seed[off+1] << 16) |
        ((uint32_t)seed[off+2] << 8) |  (uint32_t)seed[off+3];
  }
  face_ = (uint8_t)((w % 6) + 1);
  renderDie(ctx, face_);
  if (sfx_thump_) sfx_thump_();
  savePrefs(ctx);
  ++s_roll_n;

  static const char* hex = "0123456789abcdef";
  auto hex32 = [](const uint8_t b[32], char out[65]) {
    for (int i = 0; i < 32; ++i) { out[2*i] = hex[b[i]>>4]; out[2*i+1] = hex[b[i]&0xf]; }
    out[64] = 0;
  };
  char hc[65], hn[65], hs[65];
  hex32(s_commit, hc); hex32(s_nonce, hn); hex32(seed, hs);
  Serial.printf("ROLL n=%u face=%u\n", (unsigned)s_roll_n, (unsigned)face_);
  Serial.printf("  commit=%s\n  nonce =%s\n  seed  =%s\n", hc, hn, hs);
  mint_commit();
  hex32(s_commit, hc);
  Serial.printf("  next_commit=%s\n", hc);

  rolling_ = false;
}

// ---- gestures ------------------------------------------------------------

bool Dice::onGesture(AppContext& ctx, const Gesture& g) {
  if (rolling_) return true;
  switch (g.type) {
    case Gesture::SHAKE:
    case Gesture::SWIPE_UP:
    case Gesture::SWIPE_DOWN:
    case Gesture::SWIPE_LEFT:
    case Gesture::SWIPE_RIGHT:
      roll(ctx);
      return true;
    case Gesture::TAP: {
      const int sz = die_size(ctx.w, ctx.h);
      const int hx = die_x(ctx.w, ctx.h), hy = die_y(ctx.w, ctx.h);
      const bool in_die = (g.x >= hx && g.x < hx + sz && g.y >= hy && g.y < hy + sz);
      if (in_die) die_color_idx_ = (die_color_idx_ + 1) % kNumDieColors;
      else        bg_color_idx_  = (bg_color_idx_  + 1) % kNumBgColors;
      savePrefs(ctx);
      renderFull(ctx);
      return true;
    }
    case Gesture::LONG_PRESS:
      return false;   // let shell pop to home
    default:
      return false;
  }
}

} // namespace mini
