#include "genesis.h"
#include "../ui/kit.h"

#include <math.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <FS.h>
#include <SD_MMC.h>

namespace mini {

static constexpr int kCells = Genesis::W * Genesis::H;
static constexpr int kFieldBytes = kCells;
static constexpr const char* kCheckpointPath = "/genesis/state.bin";

// Lenia "Orbium" parameters (Bert Chan, 2018).
static constexpr float kKernelMu    = 0.5f;    // shell centered at r/R = 0.5
static constexpr float kKernelSigma = 0.15f;
static constexpr float kGrowthMu    = 0.15f;
static constexpr float kGrowthSigma = 0.015f;
static constexpr float kDt          = 0.1f;
static constexpr uint32_t kCosmicRayEvery = 5000;   // generations between random flips
static constexpr uint32_t kSaveEveryMs    = 2u * 60u * 1000u;   // 2 minutes
static constexpr uint32_t kMinStepMs      = 40;                 // ~25 FPS cap

// ---- allocation ----------------------------------------------------------

bool Genesis::allocate() {
  if (a_ && b_ && kernel_ && scanline_) return true;
  // Field buffers in PSRAM (~10K each, easily fits)
  a_        = (uint8_t*)  ps_malloc(kFieldBytes);
  b_        = (uint8_t*)  ps_malloc(kFieldBytes);
  // Scanline holds one output pixel row (W * SCALE wide) = 368 * uint16_t
  scanline_ = (uint16_t*) ps_malloc(sizeof(uint16_t) * W * SCALE);
  const int R2 = (R + 1) * (R + 1);
  kernel_   = (KernelPoint*) malloc(sizeof(KernelPoint) * (R2 * 4 + 16));
  if (!a_ || !b_ || !kernel_ || !scanline_) {
    Serial.println("[genesis] alloc FAILED");
    deallocate();
    return false;
  }
  memset(a_, 0, kFieldBytes);
  memset(b_, 0, kFieldBytes);
  Serial.printf("[genesis] alloc ok: field=%d each, scanline=%d\n",
                kFieldBytes, (int)(sizeof(uint16_t) * W * 2));
  return true;
}

void Genesis::deallocate() {
  if (a_)        { free(a_);        a_ = nullptr; }
  if (b_)        { free(b_);        b_ = nullptr; }
  if (kernel_)   { free(kernel_);   kernel_ = nullptr; kernel_n_ = 0; kernel_sum_ = 0; }
  if (scanline_) { free(scanline_); scanline_ = nullptr; }
}

// ---- precompute ----------------------------------------------------------

void Genesis::precomputeKernel() {
  // Sample every offset (dx, dy) in [-R..R], keep those with r/R ∈ (0, 1].
  // Weight = gaussian shell centered at μ=0.5, σ=0.15 (normalized peak).
  kernel_n_  = 0;
  kernel_sum_ = 0;
  for (int dy = -R; dy <= R; ++dy) {
    for (int dx = -R; dx <= R; ++dx) {
      float r = sqrtf((float)(dx * dx + dy * dy)) / (float)R;
      if (r <= 0.0f || r > 1.0f) continue;
      float d = (r - kKernelMu) / kKernelSigma;
      float wf = expf(-0.5f * d * d);
      uint16_t w = (uint16_t)(wf * 1000.0f + 0.5f);
      if (w == 0) continue;
      kernel_[kernel_n_++] = { (int8_t)dx, (int8_t)dy, w };
      kernel_sum_ += w;
    }
  }
  Serial.printf("[genesis] kernel: %u points, sum=%lu\n",
                (unsigned)kernel_n_, (unsigned long)kernel_sum_);
}

void Genesis::precomputeGrowthLut() {
  // Input bucket i maps to normalized neighbor-average u = i / 255.
  // growth(u) = 2 * gauss(u; μ=0.15, σ=0.015) - 1  ∈ [-1, +1]
  // per-frame delta = dt * growth * 255  ∈ [-25, +25] roughly.
  for (int i = 0; i < 256; ++i) {
    float u = (float)i / 255.0f;
    float d = (u - kGrowthMu) / kGrowthSigma;
    float g = 2.0f * expf(-0.5f * d * d) - 1.0f;
    int val = (int)(g * kDt * 255.0f);
    if (val >  120) val =  120;
    if (val < -120) val = -120;
    growth_lut_[i] = (int8_t)val;
  }
}

// ---- seed handling --------------------------------------------------------

void Genesis::generateSeed() {
  esp_fill_random(seed_, sizeof(seed_));
  computeSeedFingerprint();
  Serial.printf("[genesis] new seed fingerprint = %s\n", seed_fp_);
}

void Genesis::computeSeedFingerprint() {
  uint8_t hash[32];
  mbedtls_sha256(seed_, sizeof(seed_), hash, 0);
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 4; ++i) {
    seed_fp_[2*i]   = hex[(hash[i] >> 4) & 0xf];
    seed_fp_[2*i+1] = hex[ hash[i]       & 0xf];
  }
  seed_fp_[8] = 0;
}

// Scatter primordial intensity based on the seed. We use the seed as a
// stream-cipher-ish PRNG source (XOR with a rolling counter, mixed via
// simple hash) — deterministic per seed but noise-like across the field.
void Genesis::seedField() {
  // Draw a few asymmetric blob clusters so a coherent Lenia organism has
  // enough mass to bootstrap. Radii are chosen around R (the kernel size)
  // because that's the natural scale of an Orbium creature.
  memset(a_, 0, kFieldBytes);
  const int num_blobs = 4 + (seed_[0] & 0x03);   // 4..7 blobs
  for (int b = 0; b < num_blobs; ++b) {
    uint32_t r0 = ((uint32_t)seed_[(b*4+0) & 31] << 24) |
                  ((uint32_t)seed_[(b*4+1) & 31] << 16) |
                  ((uint32_t)seed_[(b*4+2) & 31] << 8)  |
                  ((uint32_t)seed_[(b*4+3) & 31]);
    int cx = R + (int)(r0 % (W - 2 * R));
    int cy = R + (int)((r0 >> 8) % (H - 2 * R));
    int rad = R * 60 / 100 + (int)((r0 >> 16) % (R * 60 / 100));   // ~0.6R..1.2R
    for (int dy = -rad; dy <= rad; ++dy) {
      for (int dx = -rad; dx <= rad; ++dx) {
        int r2 = dx * dx + dy * dy;
        if (r2 > rad * rad) continue;
        int x = (cx + dx + W) % W;
        int y = (cy + dy + H) % H;
        float f = expf(-2.0f * (float)r2 / (float)(rad * rad));
        int v = (int)(f * 240.0f);
        if (v > a_[y * W + x]) a_[y * W + x] = (uint8_t)v;
      }
    }
  }
}

// ---- checkpoint I/O -------------------------------------------------------

// Binary layout (little-endian):
//   [magic 4b = "gen1"] [seed 32b] [generation u32] [brightness u16]
//   [reserved 6b]       [field W*H bytes]
static constexpr uint32_t kMagic = 0x316E6567;   // 'g','e','n','1'

bool Genesis::loadCheckpoint(AppContext& ctx) {
  if (!ctx.sd_ok) return false;
  if (!SD_MMC.exists(kCheckpointPath)) return false;
  File f = SD_MMC.open(kCheckpointPath, FILE_READ);
  if (!f) return false;

  uint32_t magic = 0;
  f.read((uint8_t*)&magic, 4);
  if (magic != kMagic) { f.close(); return false; }
  f.read(seed_, 32);
  f.read((uint8_t*)&generation_, 4);
  f.read((uint8_t*)&brightness_, 2);
  uint8_t reserved[6]; f.read(reserved, 6);
  size_t got = f.read(a_, kFieldBytes);
  f.close();
  if (got != kFieldBytes) { generation_ = 0; return false; }
  computeSeedFingerprint();
  Serial.printf("[genesis] resumed gen=%lu seed=%s\n",
                (unsigned long)generation_, seed_fp_);
  return true;
}

bool Genesis::saveCheckpoint(AppContext& ctx) {
  if (!ctx.sd_ok) return false;
  if (!SD_MMC.exists("/genesis")) SD_MMC.mkdir("/genesis");
  File f = SD_MMC.open(kCheckpointPath, FILE_WRITE);
  if (!f) return false;
  uint32_t magic = kMagic;
  uint8_t reserved[6] = {0};
  f.write((uint8_t*)&magic, 4);
  f.write(seed_, 32);
  f.write((uint8_t*)&generation_, 4);
  f.write((uint8_t*)&brightness_, 2);
  f.write(reserved, 6);
  f.write(a_, kFieldBytes);
  f.close();
  Serial.printf("[genesis] checkpoint saved (gen=%lu)\n",
                (unsigned long)generation_);
  return true;
}

// ---- simulation step -----------------------------------------------------

// Hoist grid dimensions to compile-time constants for the inner-loop.
static constexpr int H_impl = Genesis::H;
static constexpr int W_impl = Genesis::W;

void Genesis::step() {
  // For each cell, sum weighted kernel neighbors → normalize → LUT → apply.
  //
  // Neighbor cells are uint8_t ∈ [0, 255]. Weights sum to kernel_sum_.
  // Therefore Σ(cell × weight) fits in a uint32_t (max 255 × kernel_sum_)
  // and Σ / kernel_sum_ yields the normalized average already in [0, 255].
  const uint32_t norm_den = kernel_sum_;
  const uint8_t* __restrict A = a_;
  uint8_t* __restrict B = b_;

  for (int y = 0; y < H_impl; ++y) {
    for (int x = 0; x < W_impl; ++x) {
      uint32_t sum = 0;
      for (size_t k = 0; k < kernel_n_; ++k) {
        const KernelPoint& kp = kernel_[k];
        int nx = x + kp.dx; if (nx < 0) nx += W_impl; else if (nx >= W_impl) nx -= W_impl;
        int ny = y + kp.dy; if (ny < 0) ny += H_impl; else if (ny >= H_impl) ny -= H_impl;
        sum += (uint32_t)A[ny * W_impl + nx] * kp.weight;
      }
      uint32_t avg_255 = sum / norm_den;
      if (avg_255 > 255) avg_255 = 255;
      int8_t delta = growth_lut_[avg_255];
      int v = (int)A[y * W_impl + x] + delta;
      if (v < 0)   v = 0;
      if (v > 255) v = 255;
      B[y * W_impl + x] = (uint8_t)v;
    }
  }
  uint8_t* tmp = a_; a_ = b_; b_ = tmp;
  ++generation_;

  if ((generation_ % kCosmicRayEvery) == 0) cosmicRay();
}

void Genesis::cosmicRay() {
  int x = (int)(esp_random() % W_impl);
  int y = (int)(esp_random() % H_impl);
  a_[y * W_impl + x] = (uint8_t)(esp_random() & 0xff);
  Serial.printf("[genesis] cosmic ray @ (%d,%d) gen=%lu\n",
                x, y, (unsigned long)generation_);
}

uint32_t Genesis::populationEstimate() const {
  uint32_t total = 0;
  for (int i = 0; i < kCells; ++i) total += a_[i];
  return total;   // arbitrary "life units"; big field so this is fine
}

// ---- color mapping --------------------------------------------------------

// Map cell intensity 0..255 → RGB565 with a bioluminescent palette:
//   0        : pure black (unlit AMOLED)
//   1..90    : cool cyan glow (nascent)
//   91..170  : cyan → warm amber (mature)
//   171..255 : amber → red (decay)
uint16_t Genesis::colorFor(uint8_t v) const {
  if (v == 0) return 0x0000;
  int r, g, b;
  if (v < 90) {
    // Cool cyan: (0, v*2.5, v*2.8)  — dominant blue, some green
    int t = v;
    r = 0;
    g = (t * 200) / 90;
    b = (t * 255) / 90;
  } else if (v < 170) {
    // Cyan → amber
    int t = v - 90;   // 0..79
    r = (t * 255) / 80;
    g = 200 - (t * 60) / 80;
    b = 255 - (t * 220) / 80;
  } else {
    // Amber → red
    int t = v - 170;  // 0..85
    r = 255;
    g = 140 - (t * 100) / 85;
    b = 35  - (t * 35)  / 85;
    if (g < 30) g = 30;
    if (b < 0)  b = 0;
  }
  // Apply brightness scaling
  r = (r * brightness_) / 255;
  g = (g * brightness_) / 255;
  b = (b * brightness_) / 255;
  // RGB565
  uint16_t rr = (uint16_t)((r >> 3) & 0x1f);
  uint16_t gg = (uint16_t)((g >> 2) & 0x3f);
  uint16_t bb = (uint16_t)((b >> 3) & 0x1f);
  return (rr << 11) | (gg << 5) | bb;
}

// ---- rendering ----------------------------------------------------------

// Full field: iterate rows, expand each cell to SCALE×SCALE pixels.
// For each source row we build one scanline of W*SCALE pixels and push it
// SCALE times. 4× scale on a 92×112 grid → 368×448 exactly.
void Genesis::renderField(AppContext& ctx) {
  auto* g = ctx.gfx;
  const uint8_t* __restrict A = a_;
  const int stride = W_impl * Genesis::SCALE;
  for (int y = 0; y < H_impl; ++y) {
    for (int x = 0; x < W_impl; ++x) {
      uint16_t c = colorFor(A[y * W_impl + x]);
      for (int k = 0; k < Genesis::SCALE; ++k) {
        scanline_[x * Genesis::SCALE + k] = c;
      }
    }
    for (int k = 0; k < Genesis::SCALE; ++k) {
      g->draw16bitRGBBitmap(0, y * Genesis::SCALE + k, scanline_, stride, 1);
    }
  }
}

// Small semi-opaque generation counter in the upper-left. We can't
// alpha-blend, so we paint a small dark card and print white on it.
void Genesis::drawGenerationCorner(AppContext& ctx) {
  auto* g = ctx.gfx;
  const int16_t x = 8, y = 8;
  const int16_t w = 108, h = 26;
  g->fillRoundRect(x, y, w, h, 8, 0x0000);
  g->drawRoundRect(x, y, w, h, 8, 0x2965);
  theme::use_font(g, theme::FONT_MONO);
  char buf[16];
  snprintf(buf, sizeof(buf), "g%lu", (unsigned long)generation_);
  ui::draw_top(g, buf, x + 10, y + 6, theme::TEXT_HI);
}

void Genesis::drawCeremony(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(0x0000);
  // Simple ceremony: fingerprint centered, small print above.
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top_centered(g, "your universe", ctx.w / 2, ctx.h / 2 - 90,
                        theme::TEXT_LO);
  theme::use_font(g, theme::FONT_HEADING_XL);
  ui::draw_top_centered(g, seed_fp_, ctx.w / 2, ctx.h / 2 - 50,
                        theme::ACCENT_CYAN);
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top_centered(g, "this signature will not occur again",
                        ctx.w / 2, ctx.h / 2 + 40, theme::TEXT_MID);
  // Countdown ticks
  int remaining = 10 - (int)((millis() - entered_ms_) / 1000);
  if (remaining < 0) remaining = 0;
  char cd[16];
  snprintf(cd, sizeof(cd), "starting in %d", remaining);
  ui::draw_top_centered(g, cd, ctx.w / 2, ctx.h / 2 + 80, theme::TEXT_LO);
}

void Genesis::drawInfoOverlay(AppContext& ctx) {
  auto* g = ctx.gfx;
  // Center dark card
  const int16_t w = 300, h = 200;
  const int16_t x = (ctx.w - w) / 2;
  const int16_t y = (ctx.h - h) / 2;
  g->fillRoundRect(x, y, w, h, 16, 0x0000);
  g->drawRoundRect(x, y, w, h, 16, theme::ACCENT_CYAN);

  theme::use_font(g, theme::FONT_HEADING_M);
  ui::draw_top(g, "universe", x + 20, y + 20, theme::TEXT_LO);
  theme::use_font(g, theme::FONT_HEADING_L);
  ui::draw_top(g, seed_fp_, x + 20, y + 42, theme::ACCENT_CYAN);

  theme::use_font(g, theme::FONT_SMALL);
  char buf[48];
  snprintf(buf, sizeof(buf), "age: %lu generations", (unsigned long)generation_);
  ui::draw_top(g, buf, x + 20, y + 90, theme::TEXT_HI);
  snprintf(buf, sizeof(buf), "population: %lu", (unsigned long)populationEstimate());
  ui::draw_top(g, buf, x + 20, y + 116, theme::TEXT_HI);
  snprintf(buf, sizeof(buf), "brightness: %u%%", (unsigned)(brightness_ * 100 / 255));
  ui::draw_top(g, buf, x + 20, y + 142, theme::TEXT_HI);

  ui::draw_top_centered(g, "tap to dismiss",
                        x + w / 2, y + h - 22, theme::TEXT_LO);
}

void Genesis::drawResetPrompt(AppContext& ctx) {
  auto* g = ctx.gfx;
  const int16_t w = 320, h = 170;
  const int16_t x = (ctx.w - w) / 2;
  const int16_t y = (ctx.h - h) / 2;
  g->fillRoundRect(x, y, w, h, 16, 0x0000);
  g->drawRoundRect(x, y, w, h, 16, theme::ACCENT_RED);

  theme::use_font(g, theme::FONT_HEADING_L);
  ui::draw_top_centered(g, "end this universe?",
                        ctx.w / 2, y + 24, theme::ACCENT_RED);
  theme::use_font(g, theme::FONT_SMALL);
  char buf[64];
  snprintf(buf, sizeof(buf), "it is %lu generations old.", (unsigned long)generation_);
  ui::draw_top_centered(g, buf, ctx.w / 2, y + 68, theme::TEXT_MID);

  // Hold-progress bar
  uint32_t held = reset_button_held_ ? (millis() - reset_arm_ms_) : 0;
  float progress = (float)held / 10000.0f;
  ui::progress_bar(g, x + 24, y + 110, w - 48, progress,
                   theme::ACCENT_RED, theme::DIVIDER, 8);
  ui::draw_top_centered(g,
                        reset_button_held_ ? "hold the side button…"
                                           : "hold the side button 10s to wipe",
                        ctx.w / 2, y + 128, theme::TEXT_LO);
  ui::draw_top_centered(g, "long-press = cancel",
                        ctx.w / 2, y + 148, theme::TEXT_LO);
}

// ---- lifecycle ------------------------------------------------------------

void Genesis::onEnter(AppContext& ctx) {
  Serial.printf("[genesis] enter: heap=%lu psram=%lu\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getFreePsram());
  if (!allocate()) {
    // Can't run — show a small message and bail.
    ctx.gfx->fillScreen(theme::BG);
    ui::screen_hint(ctx.gfx, ctx.w, ctx.h, "out of memory — genesis needs PSRAM");
    return;
  }
  precomputeKernel();
  precomputeGrowthLut();

  bool resumed = loadCheckpoint(ctx);
  if (!resumed) {
    generateSeed();
    seedField();
    generation_ = 0;
    state_ = S_CEREMONY;
    entered_ms_ = millis();
  } else {
    state_ = S_LIVE;
  }
  last_step_ms_ = millis();
  last_save_ms_ = millis();
  renderField(ctx);
  drawGenerationCorner(ctx);
  if (state_ == S_CEREMONY) drawCeremony(ctx);
}

void Genesis::onExit(AppContext& ctx) {
  saveCheckpoint(ctx);
  // Keep buffers allocated so we can resume instantly next time — free the
  // scanline buffer though (it's fine to reconstruct).
  // If memory pressure elsewhere becomes an issue, uncomment:
  // deallocate();
}

void Genesis::tick(AppContext& ctx, uint32_t now_ms) {
  if (!a_ || !b_) return;

  // Ceremony auto-advances to LIVE after 10s
  if (state_ == S_CEREMONY) {
    if (now_ms - entered_ms_ >= 10000) {
      state_ = S_LIVE;
      renderField(ctx);
      drawGenerationCorner(ctx);
    } else {
      // Redraw only the countdown line every second
      if ((now_ms - entered_ms_) / 1000 !=
          (last_step_ms_ - entered_ms_) / 1000) {
        drawCeremony(ctx);
        last_step_ms_ = now_ms;
      }
    }
    return;
  }

  // Reset arming: watch the physical button
  if (state_ == S_RESET_ARM) {
    if (reset_button_held_ && (now_ms - reset_arm_ms_) >= 10000) {
      // Wipe everything
      Serial.println("[genesis] RESET confirmed — wiping universe");
      if (ctx.sd_ok && SD_MMC.exists(kCheckpointPath)) SD_MMC.remove(kCheckpointPath);
      generateSeed();
      seedField();
      generation_ = 0;
      state_ = S_CEREMONY;
      entered_ms_ = now_ms;
      renderField(ctx);
      drawCeremony(ctx);
      return;
    }
    // Repaint the progress bar every ~200ms so the hold visualization moves
    if ((now_ms - last_step_ms_) > 200) {
      drawResetPrompt(ctx);
      last_step_ms_ = now_ms;
    }
    return;
  }

  // Live simulation
  if (now_ms - last_step_ms_ >= kMinStepMs) {
    step();
    last_step_ms_ = now_ms;
    renderField(ctx);
    if (state_ == S_INFO)  drawInfoOverlay(ctx);
    else                   drawGenerationCorner(ctx);
  }

  // Periodic checkpoint
  if (now_ms - last_save_ms_ >= kSaveEveryMs) {
    saveCheckpoint(ctx);
    last_save_ms_ = now_ms;
  }
}

bool Genesis::onGesture(AppContext& ctx, const Gesture& gest) {
  switch (gest.type) {
    case Gesture::TAP:
      if (state_ == S_LIVE)  { state_ = S_INFO; drawInfoOverlay(ctx); return true; }
      if (state_ == S_INFO)  { state_ = S_LIVE; renderField(ctx); drawGenerationCorner(ctx); return true; }
      return true;
    case Gesture::SWIPE_UP:
      brightness_ = (brightness_ > 235) ? 255 : brightness_ + 20;
      if (state_ == S_INFO) drawInfoOverlay(ctx);
      return true;
    case Gesture::SWIPE_DOWN:
      brightness_ = (brightness_ < 40) ? 20 : brightness_ - 20;
      if (state_ == S_INFO) drawInfoOverlay(ctx);
      return true;
    case Gesture::SWIPE_LEFT:
      // Long-press-to-reset gate: swipe to reveal the reset prompt.
      if (state_ == S_LIVE || state_ == S_INFO) {
        state_ = S_RESET_ARM;
        reset_button_held_ = false;
        reset_arm_ms_ = millis();
        last_step_ms_ = 0;
        drawResetPrompt(ctx);
      }
      return true;
    case Gesture::LONG_PRESS:
      if (state_ == S_RESET_ARM) {
        // Cancel reset
        state_ = S_LIVE;
        reset_button_held_ = false;
        renderField(ctx);
        drawGenerationCorner(ctx);
        return true;
      }
      return false;
    case Gesture::BUTTON_DOWN:
      if (state_ == S_RESET_ARM) {
        reset_button_held_ = true;
        reset_arm_ms_ = millis();
      }
      return true;
    case Gesture::BUTTON_UP:
      if (state_ == S_RESET_ARM) {
        reset_button_held_ = false;
      }
      return true;
    default:
      return false;
  }
}

} // namespace mini
