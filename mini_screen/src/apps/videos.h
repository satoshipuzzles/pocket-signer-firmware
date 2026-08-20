// Videos — a minimal Motion-JPEG player.
//
// This app plays .mjpg files: a stream of JPEG frames back-to-back on the SD
// card. Each frame is decoded and drawn full-screen; we then wait until the
// target frame interval elapses. On this hardware you can expect ~8–15 fps
// for 368×448 or 240×320 sources depending on JPEG quality; larger sources
// are auto-downscaled to fit.
//
// Controls:
//   * Tap                 → toggle pause / resume.
//   * Swipe left / right  → next / previous file.
//   * Long-press          → home.
//
// Producing .mjpg files: on a computer, `ffmpeg -i input.mp4 -vf scale=368:-2
// -q:v 8 -r 12 -c:v mjpeg out.mjpg` works well. Copy the result to
// /videos/*.mjpg on the SD card.
#pragma once
#include "../shell.h"

namespace mini {

class Videos : public App {
 public:
  const char* name() const override { return "Videos"; }
  AppKind     kind() const override { return AppKind::VIDEOS; }
  uint16_t    tint() const override { return 0xF81F; }   // magenta

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  static constexpr int kMaxFiles = 64;
  static constexpr uint32_t kFrameIntervalMs = 80;   // ~12 fps target

  String   dir_ = "/videos";
  String   files_[kMaxFiles];
  int      file_count_ = 0;
  int      cur_ = -1;

  // Open file handle spans frames of the currently-playing file.
  bool     have_sd_ = false;
  bool     paused_  = false;
  uint32_t last_frame_ms_ = 0;
  uint32_t frame_index_   = 0;

  void refreshFileList(AppContext&);
  bool openCurrent();
  void closeCurrent();
  bool decodeAndDrawFrame(AppContext&);
  void drawEmptyState(AppContext&);
  void drawPauseOverlay(AppContext&);
};

} // namespace mini
