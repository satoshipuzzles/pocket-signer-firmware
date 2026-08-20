#include "videos.h"
#include "../ui/kit.h"

#include <FS.h>
#include <SD_MMC.h>
#include <JPEGDEC.h>

namespace mini {

// The currently-open video file. We stream it byte-by-byte, and at each
// frame we look for the JPEG SOI marker (FFD8) then a matching EOI (FFD9)
// to bound the frame. Then we hand a memory buffer to JPEGDEC.
static File g_vf;
static uint8_t* g_frame_buf = nullptr;
static size_t   g_frame_cap = 0;

struct DrawCtx { Arduino_GFX* gfx; int16_t ox, oy; };
static int jpegDrawTile(JPEGDRAW* pDraw) {
  DrawCtx* d = (DrawCtx*)pDraw->pUser;
  d->gfx->draw16bitRGBBitmap(
      d->ox + pDraw->x, d->oy + pDraw->y,
      pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  return 1;
}

// ---- file listing --------------------------------------------------------

static bool has_mjpg_ext(const String& name) {
  String n = name; n.toLowerCase();
  return n.endsWith(".mjpg") || n.endsWith(".mjpeg");
}

void Videos::refreshFileList(AppContext& ctx) {
  file_count_ = 0;
  if (!ctx.sd_ok) { have_sd_ = false; return; }
  have_sd_ = true;
  if (!SD_MMC.exists(dir_.c_str())) SD_MMC.mkdir(dir_.c_str());
  File root = SD_MMC.open(dir_.c_str());
  if (!root || !root.isDirectory()) return;
  File e = root.openNextFile();
  while (e && file_count_ < kMaxFiles) {
    if (!e.isDirectory()) {
      String name = e.name();
      if (has_mjpg_ext(name)) files_[file_count_++] = String(e.path());
    }
    e = root.openNextFile();
  }
  root.close();
  for (int i = 1; i < file_count_; ++i) {
    for (int j = i; j > 0 && files_[j] < files_[j - 1]; --j) {
      String tmp = files_[j]; files_[j] = files_[j - 1]; files_[j - 1] = tmp;
    }
  }
  Serial.printf("[videos] found %d files under %s\n", file_count_, dir_.c_str());
}

// ---- frame handling ------------------------------------------------------

// Ensure the scratch buffer can hold at least `need` bytes.
static bool ensure_buf(size_t need) {
  if (need <= g_frame_cap) return true;
  size_t new_cap = need + 8192;
  uint8_t* nb = (uint8_t*)ps_realloc(g_frame_buf, new_cap);
  if (!nb) return false;
  g_frame_buf = nb;
  g_frame_cap = new_cap;
  return true;
}

// Read one complete JPEG frame from g_vf into g_frame_buf. Returns bytes
// written, or 0 at EOF / error.
static size_t read_next_frame() {
  if (!g_vf) return 0;
  // Scan forward for SOI (0xFF 0xD8 0xFF)
  size_t start_pos = g_vf.position();
  uint8_t b0 = 0, b1 = 0;
  bool found_soi = false;
  while (g_vf.available()) {
    uint8_t b2 = g_vf.read();
    if (b0 == 0xFF && b1 == 0xD8 && b2 == 0xFF) {
      // Rewind to place cursor at the 0xFF that starts the SOI
      g_vf.seek(g_vf.position() - 3);
      found_soi = true;
      break;
    }
    b0 = b1; b1 = b2;
  }
  if (!found_soi) return 0;

  // Now stream bytes until we hit EOI (0xFF 0xD9).
  ensure_buf(1024);
  size_t n = 0;
  uint8_t prev = 0;
  while (g_vf.available()) {
    if (n + 2 > g_frame_cap && !ensure_buf(n + 4096)) return 0;
    uint8_t c = g_vf.read();
    g_frame_buf[n++] = c;
    if (prev == 0xFF && c == 0xD9) break;
    prev = c;
  }
  (void)start_pos;
  return n;
}

// ---- rendering -----------------------------------------------------------

void Videos::drawEmptyState(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Videos", tint());
  theme::use_font(g, theme::FONT_HEADING_M);
  ui::draw_top_centered(g, "no videos",
                        ctx.w / 2, ctx.h / 2 - 30, theme::TEXT_HI);
  theme::use_font(g, theme::FONT_SMALL);
  if (!have_sd_) {
    ui::draw_top_centered(g, "insert an SD card and try again",
                          ctx.w / 2, ctx.h / 2 + 10, theme::TEXT_MID);
  } else {
    ui::draw_top_centered(g, "copy .mjpg files to /videos on SD",
                          ctx.w / 2, ctx.h / 2 + 10, theme::TEXT_MID);
  }
  ui::screen_hint(g, ctx.w, ctx.h, "long-press = home");
}

void Videos::drawPauseOverlay(AppContext& ctx) {
  auto* g = ctx.gfx;
  const int16_t cx = ctx.w / 2, cy = ctx.h / 2;
  g->fillRoundRect(cx - 40, cy - 40, 80, 80, 12, 0x0000);
  g->drawRoundRect(cx - 40, cy - 40, 80, 80, 12, theme::ACCENT_CYAN);
  // Two vertical bars (pause glyph)
  g->fillRect(cx - 16, cy - 20, 8, 40, theme::ACCENT_CYAN);
  g->fillRect(cx + 8,  cy - 20, 8, 40, theme::ACCENT_CYAN);
}

// Read the next frame from the currently-open file, decode it, and blit
// centered on screen. Returns true if a frame was drawn.
bool Videos::decodeAndDrawFrame(AppContext& ctx) {
  size_t n = read_next_frame();
  if (n == 0) {
    // Loop the file: rewind and try once more.
    g_vf.seek(0);
    n = read_next_frame();
    if (n == 0) return false;
  }

  JPEGDEC jpeg;
  if (!jpeg.openRAM(g_frame_buf, (int32_t)n, jpegDrawTile)) return false;
  int iw = jpeg.getWidth();
  int ih = jpeg.getHeight();
  int scale = 0;
  int div = 1;
  while (iw / div > ctx.w || ih / div > ctx.h) {
    if (div == 1)      { scale = JPEG_SCALE_HALF;    div = 2; }
    else if (div == 2) { scale = JPEG_SCALE_QUARTER; div = 4; }
    else if (div == 4) { scale = JPEG_SCALE_EIGHTH;  div = 8; }
    else break;
  }
  int draw_w = iw / div, draw_h = ih / div;
  DrawCtx dctx = { ctx.gfx,
                   (int16_t)((ctx.w - draw_w) / 2),
                   (int16_t)((ctx.h - draw_h) / 2) };
  jpeg.setUserPointer(&dctx);
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  jpeg.decode(0, 0, scale);
  jpeg.close();
  ++frame_index_;
  return true;
}

// ---- file open/close -----------------------------------------------------

bool Videos::openCurrent() {
  closeCurrent();
  if (cur_ < 0 || cur_ >= file_count_) return false;
  g_vf = SD_MMC.open(files_[cur_].c_str(), FILE_READ);
  if (!g_vf) return false;
  frame_index_ = 0;
  last_frame_ms_ = 0;
  return true;
}
void Videos::closeCurrent() {
  if (g_vf) g_vf.close();
}

// ---- lifecycle -----------------------------------------------------------

void Videos::onEnter(AppContext& ctx) {
  refreshFileList(ctx);
  paused_ = false;
  if (file_count_ == 0) {
    cur_ = -1;
    drawEmptyState(ctx);
    return;
  }
  cur_ = 0;
  ctx.gfx->fillScreen(0x0000);
  openCurrent();
}

void Videos::onExit(AppContext&) {
  closeCurrent();
  // Keep the scratch buffer allocated across app switches for smoother
  // re-entry — freeing it would just cause a large allocation next time.
}

void Videos::tick(AppContext& ctx, uint32_t now_ms) {
  if (paused_ || file_count_ == 0) return;
  if (now_ms - last_frame_ms_ < kFrameIntervalMs) return;
  last_frame_ms_ = now_ms;
  decodeAndDrawFrame(ctx);
}

bool Videos::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TAP:
      if (file_count_ == 0) return true;
      paused_ = !paused_;
      if (paused_) drawPauseOverlay(ctx);
      return true;
    case Gesture::SWIPE_LEFT:
      if (file_count_ == 0) return true;
      cur_ = (cur_ + 1) % file_count_;
      paused_ = false;
      ctx.gfx->fillScreen(0x0000);
      openCurrent();
      return true;
    case Gesture::SWIPE_RIGHT:
      if (file_count_ == 0) return true;
      cur_ = (cur_ - 1 + file_count_) % file_count_;
      paused_ = false;
      ctx.gfx->fillScreen(0x0000);
      openCurrent();
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

} // namespace mini
