// Genesis — a Lenia continuous cellular automaton, one universe per device.
//
// Grid:   184 × 224 cells, each an unsigned 8-bit intensity (0 = dead, 255 = full).
// Render: 2× scale to fill the 368 × 448 AMOLED edge-to-edge.
//
// Lenia (Bert Chan, 2018): each cell samples a smooth, ring-shaped kernel
// over its neighbors, computes a weighted average, then updates itself by a
// gaussian growth function of that average. The Orbium parameters we use
// here (R=13, kernel μ=0.5 σ=0.15, growth μ=0.15 σ=0.015, dt=0.1) are the
// canonical settings that yield coherent self-repairing "creatures".
//
// Persistence: on first entry we generate one unique seed via hardware TRNG
// and scatter that seed's stream across the field as initial intensity. The
// seed + generation counter + field snapshot get checkpointed to
// /genesis/state.bin on the SD card every ~2 minutes so that power loss
// merely pauses the universe. A cosmic-ray subsystem flips one random cell
// every 5000 generations to keep history from collapsing into a cycle.
//
// UI:
//   * Single generation counter in the upper-left corner.
//   * Tap once: overlay showing seed fingerprint, age (in generations), and
//     population estimate. Tap again to dismiss.
//   * Swipe up/down: adjust screen brightness (persisted).
//   * Reset is deliberately hard: hold the physical BOOT button for 10
//     seconds while the reset prompt is showing. No touchscreen shortcut.
#pragma once
#include "../shell.h"

namespace mini {

class Genesis : public App {
 public:
  const char* name() const override { return "Genesis"; }
  AppKind     kind() const override { return AppKind::GENESIS; }
  uint16_t    tint() const override { return 0x07FF; }   // cool cyan

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

  // 92×112 continuous cells, rendered at 4× scale to fill the 368×448 AMOLED
  // exactly. Halving the linear resolution (vs. the ideal 184×224 grid) is
  // the price we pay for hitting ~25 FPS on a 240 MHz ESP32; each cell then
  // takes a 4×4 pixel block, which actually reads more clearly as
  // "bioluminescent life" than single-pixel dots.
  static constexpr int W = 92;
  static constexpr int H = 112;
  static constexpr int SCALE = 4;
  static constexpr int R = 13;                   // kernel radius

 private:
  enum State : uint8_t {
    S_UNINIT = 0,
    S_CEREMONY,   // showing seed fingerprint for 10s on first entry
    S_LIVE,
    S_INFO,       // overlay showing age/seed/population
    S_RESET_ARM,  // reset prompt visible, BOOT-hold pending
  };

  // Field buffers (double-buffered).
  uint8_t* a_ = nullptr;   // current generation
  uint8_t* b_ = nullptr;   // next generation (staging)

  // Simulation state (checkpointed to SD)
  uint8_t  seed_[32]      = {0};   // hardware TRNG at first boot
  char     seed_fp_[9]    = {0};   // first 8 hex of sha256(seed) — display fingerprint
  uint32_t generation_    = 0;
  uint16_t brightness_    = 220;

  // Runtime state (not checkpointed)
  State    state_         = S_UNINIT;
  uint32_t entered_ms_    = 0;
  uint32_t last_step_ms_  = 0;
  uint32_t last_save_ms_  = 0;
  uint32_t reset_arm_ms_  = 0;      // when BOOT was pressed during S_RESET_ARM
  bool     reset_button_held_ = false;

  // Kernel (precomputed once)
  struct KernelPoint { int8_t dx, dy; uint16_t weight; };
  KernelPoint* kernel_    = nullptr;
  size_t       kernel_n_  = 0;
  uint32_t     kernel_sum_= 0;      // sum of all weights (for normalization)

  // Growth LUT: growth_lut_[normalized_avg 0..255] = signed dt to apply
  int8_t   growth_lut_[256];

  // Scanline pixel buffer (uint16_t RGB565, W*2 pixels wide = 368 px)
  uint16_t* scanline_ = nullptr;

  // ---- init / io -----------------------------------------------------------
  bool allocate();
  void deallocate();
  void precomputeKernel();
  void precomputeGrowthLut();
  void generateSeed();
  void seedField();
  void computeSeedFingerprint();
  bool loadCheckpoint(AppContext&);
  bool saveCheckpoint(AppContext&);

  // ---- simulation ----------------------------------------------------------
  void step();
  void cosmicRay();
  uint32_t populationEstimate() const;

  // ---- rendering -----------------------------------------------------------
  uint16_t colorFor(uint8_t v) const;
  void renderField(AppContext&);
  void drawCeremony(AppContext&);
  void drawGenerationCorner(AppContext&);
  void drawInfoOverlay(AppContext&);
  void drawResetPrompt(AppContext&);
};

} // namespace mini
