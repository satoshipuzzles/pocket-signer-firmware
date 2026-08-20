// Photos — a modest JPEG viewer for images on the SD card.
//
// Reads /photos/*.jpg (or .jpeg) from the SD card, sorts alphabetically, and
// shows them one at a time centered on screen. Swipe left/right to advance,
// tap to toggle a small filename overlay, long-press to go home.
//
// Under the hood: uses Larry Bank's JPEGDEC. The library streams the JPEG
// from a File handle, decodes into 16×16 or 8×8 MCU blocks, and calls back
// with an RGB565 tile which we push to the panel via draw16bitRGBBitmap.
#pragma once
#include "../shell.h"

namespace mini {

class Photos : public App {
 public:
  const char* name() const override { return "Photos"; }
  AppKind     kind() const override { return AppKind::PHOTOS; }
  uint16_t    tint() const override { return 0xFCA0; }   // warm amber

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override {}
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  static constexpr int kMaxFiles = 64;

  String   dir_     = "/photos";
  String   files_[kMaxFiles];
  int      file_count_ = 0;
  int      cur_ = 0;
  bool     show_overlay_ = true;
  uint32_t overlay_hide_at_ = 0;
  bool     have_sd_ = false;

  void refreshFileList(AppContext&);
  void showCurrent(AppContext&);
  void drawEmptyState(AppContext&);
  void drawOverlay(AppContext&);
};

} // namespace mini
