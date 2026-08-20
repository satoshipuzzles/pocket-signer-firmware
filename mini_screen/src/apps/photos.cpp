#include "photos.h"
#include "../ui/kit.h"

#include <FS.h>
#include <SD_MMC.h>
#include <JPEGDEC.h>

namespace mini {

// JPEGDEC calls a static draw function for each MCU tile it decodes. We
// stash the target GFX pointer and screen-center offset in a small context
// struct that we pass via the JPEGDEC user pointer.
struct DrawCtx {
  Arduino_GFX* gfx;
  int16_t      ox, oy;    // upper-left screen offset for the image
};

static int jpegDrawTile(JPEGDRAW* pDraw) {
  DrawCtx* d = (DrawCtx*)pDraw->pUser;
  d->gfx->draw16bitRGBBitmap(
      d->ox + pDraw->x,
      d->oy + pDraw->y,
      pDraw->pPixels,
      pDraw->iWidth,
      pDraw->iHeight);
  return 1;
}

// SD-backed reader shims for JPEGDEC's Open/Close/Read/Seek callbacks.
struct SdReader {
  File file;
};

static void* jpegOpen(const char* filename, int32_t* size) {
  static SdReader r;
  r.file = SD_MMC.open(filename, FILE_READ);
  if (!r.file) { *size = 0; return nullptr; }
  *size = (int32_t)r.file.size();
  return &r;
}
static void jpegClose(void* handle) {
  SdReader* r = (SdReader*)handle;
  if (r && r->file) r->file.close();
}
static int32_t jpegRead(JPEGFILE* jf, uint8_t* buf, int32_t len) {
  SdReader* r = (SdReader*)jf->fHandle;
  return (int32_t)r->file.read(buf, len);
}
static int32_t jpegSeek(JPEGFILE* jf, int32_t pos) {
  SdReader* r = (SdReader*)jf->fHandle;
  return r->file.seek(pos) ? 1 : 0;
}

// ---- file listing --------------------------------------------------------

static bool has_jpeg_ext(const String& name) {
  String n = name; n.toLowerCase();
  return n.endsWith(".jpg") || n.endsWith(".jpeg");
}

void Photos::refreshFileList(AppContext& ctx) {
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
      if (has_jpeg_ext(name)) {
        files_[file_count_++] = String(e.path());
      }
    }
    e = root.openNextFile();
  }
  root.close();
  // Alpha sort
  for (int i = 1; i < file_count_; ++i) {
    for (int j = i; j > 0 && files_[j] < files_[j - 1]; --j) {
      String tmp = files_[j]; files_[j] = files_[j - 1]; files_[j - 1] = tmp;
    }
  }
  Serial.printf("[photos] found %d files under %s\n", file_count_, dir_.c_str());
}

// ---- render --------------------------------------------------------------

void Photos::drawEmptyState(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Photos", tint());
  theme::use_font(g, theme::FONT_HEADING_M);
  ui::draw_top_centered(g, "no photos",
                        ctx.w / 2, ctx.h / 2 - 30, theme::TEXT_HI);
  theme::use_font(g, theme::FONT_SMALL);
  if (!have_sd_) {
    ui::draw_top_centered(g, "insert an SD card and try again",
                          ctx.w / 2, ctx.h / 2 + 10, theme::TEXT_MID);
  } else {
    ui::draw_top_centered(g, "copy .jpg files to /photos on SD",
                          ctx.w / 2, ctx.h / 2 + 10, theme::TEXT_MID);
  }
  ui::screen_hint(g, ctx.w, ctx.h, "long-press = home");
}

void Photos::drawOverlay(AppContext& ctx) {
  auto* g = ctx.gfx;
  const int16_t h = 30;
  const int16_t y = ctx.h - h - 12;
  const int16_t x = 12;
  const int16_t w = ctx.w - 2 * x;
  // Semi-dim card
  g->fillRoundRect(x, y, w, h, 8, 0x0000);
  g->drawRoundRect(x, y, w, h, 8, theme::DIVIDER);
  theme::use_font(g, theme::FONT_SMALL);
  // Filename (basename)
  String path = files_[cur_];
  int slash = path.lastIndexOf('/');
  String base = slash >= 0 ? path.substring(slash + 1) : path;
  ui::draw_top(g, base.c_str(), x + 10, y + 10, theme::TEXT_HI);
  // Right side: index/count
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%d", cur_ + 1, file_count_);
  int16_t bw = ui::text_w(g, buf);
  ui::draw_top(g, buf, x + w - bw - 10, y + 10, theme::TEXT_LO);
}

void Photos::showCurrent(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(0x0000);
  if (file_count_ == 0) { drawEmptyState(ctx); return; }

  const char* path = files_[cur_].c_str();
  JPEGDEC jpeg;
  if (!jpeg.open(path, jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDrawTile)) {
    Serial.printf("[photos] open failed: %s\n", path);
    theme::use_font(g, theme::FONT_HEADING_M);
    ui::draw_top_centered(g, "unable to read image",
                          ctx.w / 2, ctx.h / 2, theme::ACCENT_RED);
    return;
  }

  int iw = jpeg.getWidth();
  int ih = jpeg.getHeight();
  // Pick a JPEGDEC scale that produces the largest image that still fits.
  // JPEGDEC supports 1/1, 1/2, 1/4, 1/8 scales.
  int scale = 0;   // JPEG_SCALE_QUARTER = ??; let's compute
  int div = 1;
  while (iw / div > ctx.w || ih / div > ctx.h) {
    if (div == 1) { scale = JPEG_SCALE_HALF;    div = 2; }
    else if (div == 2) { scale = JPEG_SCALE_QUARTER; div = 4; }
    else if (div == 4) { scale = JPEG_SCALE_EIGHTH;  div = 8; }
    else break;
  }
  int draw_w = iw / div;
  int draw_h = ih / div;
  DrawCtx dctx = { g, (int16_t)((ctx.w - draw_w) / 2), (int16_t)((ctx.h - draw_h) / 2) };
  jpeg.setUserPointer(&dctx);
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  if (!jpeg.decode(0, 0, scale)) {
    Serial.printf("[photos] decode failed: %s (last err=%d)\n",
                  path, (int)jpeg.getLastError());
  }
  jpeg.close();

  if (show_overlay_) drawOverlay(ctx);
}

// ---- lifecycle ----------------------------------------------------------

void Photos::onEnter(AppContext& ctx) {
  refreshFileList(ctx);
  cur_ = 0;
  show_overlay_ = true;
  overlay_hide_at_ = millis() + 3000;
  if (file_count_ == 0) drawEmptyState(ctx);
  else                  showCurrent(ctx);
}

void Photos::onExit(AppContext&) {}

bool Photos::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::SWIPE_LEFT:
      if (file_count_ > 0) {
        cur_ = (cur_ + 1) % file_count_;
        show_overlay_ = true;
        overlay_hide_at_ = millis() + 3000;
        showCurrent(ctx);
      }
      return true;
    case Gesture::SWIPE_RIGHT:
      if (file_count_ > 0) {
        cur_ = (cur_ - 1 + file_count_) % file_count_;
        show_overlay_ = true;
        overlay_hide_at_ = millis() + 3000;
        showCurrent(ctx);
      }
      return true;
    case Gesture::TAP:
      show_overlay_ = !show_overlay_;
      showCurrent(ctx);
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

} // namespace mini
