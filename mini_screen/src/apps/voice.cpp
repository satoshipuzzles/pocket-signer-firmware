#include "voice.h"
#include "../ui/theme.h"

namespace mini {

// -- WAV constants ----------------------------------------------------------
static constexpr uint32_t kSampleRateHz = 16000;
static constexpr uint16_t kBitsPerSample = 16;
static constexpr uint16_t kNumChannels = 1;

// -- geometry ---------------------------------------------------------------
// Top 60px is the header/nav row. Bottom 100px is the record area.
// Middle is the waveform strip / recording status.
static constexpr int kHeaderH  = 56;
static constexpr int kWaveH    = 130;
static constexpr int kListRowH = 40;

// ---- WAV I/O --------------------------------------------------------------

bool VoiceMemo::writeWavHeader(File& f) {
  // 44-byte canonical WAV/RIFF header. We patch the size fields on finalize.
  uint8_t hdr[44] = {
    'R','I','F','F',
    0,0,0,0,           // total size - 8, patched later
    'W','A','V','E',
    'f','m','t',' ',
    16,0,0,0,          // fmt chunk size (16 for PCM)
    1,0,               // audio format (1 = PCM)
    0,0,               // channels, patched
    0,0,0,0,           // sample rate, patched
    0,0,0,0,           // byte rate, patched
    0,0,               // block align, patched
    0,0,               // bits per sample, patched
    'd','a','t','a',
    0,0,0,0            // data size, patched
  };
  const uint32_t byte_rate    = kSampleRateHz * kNumChannels * (kBitsPerSample / 8);
  const uint16_t block_align  = kNumChannels * (kBitsPerSample / 8);
  hdr[22] = (uint8_t)(kNumChannels & 0xff);
  hdr[23] = (uint8_t)((kNumChannels >> 8) & 0xff);
  hdr[24] = (uint8_t)(kSampleRateHz & 0xff);
  hdr[25] = (uint8_t)((kSampleRateHz >> 8) & 0xff);
  hdr[26] = (uint8_t)((kSampleRateHz >> 16) & 0xff);
  hdr[27] = (uint8_t)((kSampleRateHz >> 24) & 0xff);
  hdr[28] = (uint8_t)(byte_rate & 0xff);
  hdr[29] = (uint8_t)((byte_rate >> 8) & 0xff);
  hdr[30] = (uint8_t)((byte_rate >> 16) & 0xff);
  hdr[31] = (uint8_t)((byte_rate >> 24) & 0xff);
  hdr[32] = (uint8_t)(block_align & 0xff);
  hdr[33] = (uint8_t)((block_align >> 8) & 0xff);
  hdr[34] = (uint8_t)(kBitsPerSample & 0xff);
  hdr[35] = (uint8_t)((kBitsPerSample >> 8) & 0xff);
  return f.write(hdr, 44) == 44;
}

bool VoiceMemo::finalizeWav(File& f, uint32_t sample_count) {
  const uint32_t data_size  = sample_count * kNumChannels * (kBitsPerSample / 8);
  const uint32_t riff_size  = 36 + data_size;
  auto write_u32 = [&](uint32_t off, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    f.seek(off);
    return f.write(b, 4) == 4;
  };
  bool ok = write_u32(4,  riff_size);
  ok = ok && write_u32(40, data_size);
  f.flush();
  return ok;
}

// ---- list bookkeeping -----------------------------------------------------

int VoiceMemo::nextRecordingIndex() {
  if (!fs_) return 1;
  int max_seen = 0;
  File dir = fs_->open(dir_);
  if (!dir || !dir.isDirectory()) return 1;
  File f = dir.openNextFile();
  while (f) {
    String n = f.name();
    // Extract trailing digits from "memo_NNN.wav"
    int u = n.lastIndexOf('_');
    int d = n.lastIndexOf('.');
    if (u >= 0 && d > u) {
      String num = n.substring(u + 1, d);
      int v = num.toInt();
      if (v > max_seen) max_seen = v;
    }
    f = dir.openNextFile();
  }
  return max_seen + 1;
}

void VoiceMemo::refreshList() {
  list_count_ = 0;
  if (!fs_) return;
  if (!fs_->exists(dir_)) { fs_->mkdir(dir_); return; }
  File dir = fs_->open(dir_);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f && list_count_ < kMaxList) {
    if (!f.isDirectory()) {
      // IMPORTANT: use f.path() (full absolute path) not f.name() (basename)
      // — playback opens this string as a path so we need the leading dir.
      // Some core versions have empty path() but always populate name(); if
      // path is empty, synthesize it from dir_.
      String p = f.path();
      if (p.length() == 0) {
        p = String(dir_) + "/" + String(f.name());
      }
      list_names_[list_count_] = p;
      const uint32_t br = kSampleRateHz * kNumChannels * (kBitsPerSample / 8);
      uint32_t sz = f.size();
      list_ms_[list_count_] = br ? ((sz > 44 ? sz - 44 : 0) * 1000UL / br) : 0;
      ++list_count_;
    }
    f = dir.openNextFile();
  }
  Serial.printf("[voice] refreshList: %d file(s) in %s\n",
                (int)list_count_, dir_ ? dir_ : "(null)");
}

// ---- lifecycle ------------------------------------------------------------

void VoiceMemo::onEnter(AppContext& ctx) {
  page_ = PAGE_RECORD;
  recording_ = false;
  playing_idx_ = -1;
  memset(wave_ring_, 0, sizeof(wave_ring_));
  wave_head_ = 0;
  refreshList();
  renderRecord(ctx);
}

void VoiceMemo::onExit(AppContext& ctx) {
  if (recording_)  stopRecording(ctx, /*keep=*/false);
  if (playing_idx_ >= 0) {
    if (play_file_) play_file_.close();
    playing_idx_ = -1;
  }
}

void VoiceMemo::tick(AppContext& ctx, uint32_t now_ms) {
  if (recording_) pumpRecording(ctx);
  if (playing_idx_ >= 0) pumpPlayback(ctx);
}

// ---- recording ------------------------------------------------------------

bool VoiceMemo::startRecording(AppContext& ctx) {
  if (recording_) return true;
  memset(wave_ring_, 0, sizeof(wave_ring_));
  wave_head_ = 0;
  rec_samples_written_ = 0;
  rec_start_ms_ = millis();
  last_wave_push_ms_ = rec_start_ms_;
  last_ui_ms_ = rec_start_ms_;

  if (fs_) {
    if (!fs_->exists(dir_)) fs_->mkdir(dir_);
    int idx = nextRecordingIndex();
    char path[48];
    snprintf(path, sizeof(path), "%s/memo_%03d.wav", dir_, idx);
    rec_file_ = fs_->open(path, FILE_WRITE);
    if (rec_file_) {
      writeWavHeader(rec_file_);
      Serial.printf("[voice] recording -> %s\n", path);
    } else {
      Serial.printf("[voice] open %s FAILED\n", path);
    }
  } else {
    Serial.println("[voice] recording (no SD - preview only)");
  }

  recording_ = true;
  renderRecord(ctx);
  return true;
}

void VoiceMemo::stopRecording(AppContext& ctx, bool keep) {
  if (!recording_) return;
  recording_ = false;
  uint32_t dur_ms = millis() - rec_start_ms_;
  bool did_save = false;
  if (rec_file_) {
    if (keep && dur_ms >= 300) {
      finalizeWav(rec_file_, rec_samples_written_);
      Serial.printf("[voice] saved %lu samples (%lu ms)\n",
                    (unsigned long)rec_samples_written_,
                    (unsigned long)dur_ms);
      did_save = true;
    } else {
      // Too short — discard.
      String path = rec_file_.path();
      rec_file_.close();
      if (fs_) fs_->remove(path);
      Serial.printf("[voice] discarded %lums recording\n", (unsigned long)dur_ms);
    }
    if (rec_file_) rec_file_.close();
  }
  refreshList();
  // If we actually saved something, jump to the list page so the user
  // immediately sees that their memo is there. Otherwise stay on record.
  if (did_save && fs_) {
    page_ = PAGE_LIST;
    renderList(ctx);
  } else {
    renderRecord(ctx);
  }
}

void VoiceMemo::pumpRecording(AppContext& ctx) {
  if (!read_) return;
  // Pull whatever's ready from the mic in chunks of 512 samples (32ms @16k).
  constexpr size_t CHUNK = 512;
  int16_t buf[CHUNK];
  size_t got = read_(buf, CHUNK);
  if (got == 0) return;

  // Compute RMS of this chunk for the VU trace.
  uint64_t acc = 0;
  for (size_t i = 0; i < got; ++i) {
    int32_t v = buf[i];
    acc += (uint32_t)(v * v);
  }
  float rms = sqrtf((float)acc / (float)got);
  int amp = (int)(rms * 100.0f / 4000.0f);
  if (amp > 100) amp = 100;

  const uint32_t now = millis();
  if (now - last_wave_push_ms_ >= 30) {
    wave_ring_[wave_head_] = (int8_t)amp;
    wave_head_ = (uint8_t)((wave_head_ + 1) % (uint8_t)sizeof(wave_ring_));
    last_wave_push_ms_ = now;
  }

  if (rec_file_) {
    rec_file_.write((uint8_t*)buf, got * sizeof(int16_t));
    rec_samples_written_ += got;
  }

  if (now - last_ui_ms_ >= 60) {
    renderRecordingProgress(ctx, now - rec_start_ms_);
    last_ui_ms_ = now;
  }
}

// ---- playback -------------------------------------------------------------

void VoiceMemo::pumpPlayback(AppContext& ctx) {
  if (!write_ || !play_file_) return;
  constexpr size_t CHUNK = 512;
  int16_t buf[CHUNK];
  size_t got = play_file_.read((uint8_t*)buf, CHUNK * sizeof(int16_t)) / sizeof(int16_t);
  if (got == 0) {
    play_file_.close();
    playing_idx_ = -1;
    renderList(ctx);
    return;
  }
  write_(buf, got);
  const uint32_t now = millis();
  if (now - last_ui_ms_ >= 100) {
    renderPlayback(ctx, now - play_start_ms_, play_total_ms_);
    last_ui_ms_ = now;
  }
}

// ---- rendering ------------------------------------------------------------

static void render_header(Arduino_GFX* g, int16_t w, const char* label,
                          const char* right, uint16_t accent) {
  g->fillRect(0, 0, w, kHeaderH, theme::BG);
  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(accent);
  g->setCursor(16, 18);
  g->print(label);
  if (right && *right) {
    g->setTextSize(1);
    g->setTextColor(theme::TEXT_LO);
    int16_t rw = (int16_t)strlen(right) * 6;
    g->setCursor(w - rw - 16, 24);
    g->print(right);
  }
  g->drawFastHLine(16, kHeaderH - 4, w - 32, theme::DIVIDER);
}

void VoiceMemo::renderRecord(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);
  render_header(g, ctx.w, "VOICE", "swipe: list", theme::ACCENT_MAGENTA);

  const int wave_y = kHeaderH + 20;
  g->drawRoundRect(16, wave_y, ctx.w - 32, kWaveH, 12, theme::DIVIDER);

  const int mic_size = 90;
  theme::icon_mic(g,
                  (ctx.w - mic_size) / 2,
                  wave_y + kWaveH + 24,
                  mic_size,
                  recording_ ? theme::ACCENT_RED : theme::ACCENT_MAGENTA);

  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(theme::TEXT_HI);
  const char* label = recording_ ? "RECORDING" : "hold to record";
  int16_t lw = (int16_t)strlen(label) * 6 * 2;
  g->setCursor((ctx.w - lw) / 2, wave_y + kWaveH + 24 + mic_size + 12);
  g->print(label);

  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* w2 = fs_ ? "screen OR side button = record"
                       : "insert SD to save (preview only)";
  int16_t ww = (int16_t)strlen(w2) * 6;
  g->setCursor((ctx.w - ww) / 2, ctx.h - 40);
  g->print(w2);
  const char* hint = "swipe = memos list  |  long-press = home";
  int16_t hw = (int16_t)strlen(hint) * 6;
  g->setCursor((ctx.w - hw) / 2, ctx.h - 20);
  g->print(hint);
}

void VoiceMemo::renderRecordingProgress(AppContext& ctx, uint32_t elapsed_ms) {
  auto* g = ctx.gfx;
  const int wave_y = kHeaderH + 20;
  const int wave_x = 18;
  const int wave_w = ctx.w - 36;
  const int wave_inner_h = kWaveH - 8;
  const int cy = wave_y + kWaveH / 2;
  g->fillRoundRect(wave_x, wave_y + 4, wave_w, wave_inner_h, 8, theme::SURFACE);

  // Draw wave columns from oldest to newest, right-aligned.
  const int cols = (int)sizeof(wave_ring_);
  const int col_step = max(1, wave_w / cols);
  for (int i = 0; i < cols; ++i) {
    int idx = (wave_head_ + i) % cols;
    int a = wave_ring_[idx];
    int bar_h = (a * (wave_inner_h - 4)) / 100;
    if (bar_h < 2) bar_h = 2;
    int cx = wave_x + i * col_step + col_step / 2;
    uint16_t color = (i >= cols - 10) ? theme::ACCENT_RED : theme::ACCENT_MAGENTA;
    g->fillRect(cx - 1, cy - bar_h / 2, 2, bar_h, color);
  }

  // Timer mm:ss.ms
  uint32_t ms = elapsed_ms;
  uint32_t s  = ms / 1000; ms %= 1000;
  uint32_t m  = s / 60;    s  %= 60;
  char t[16];
  snprintf(t, sizeof(t), "%02lu:%02lu.%01lu",
           (unsigned long)m, (unsigned long)s, (unsigned long)(ms / 100));
  g->fillRect(0, wave_y + kWaveH + 4, ctx.w, 20, theme::BG);
  g->setFont(nullptr);
  g->setTextSize(2);
  g->setTextColor(theme::ACCENT_RED);
  int16_t tw = (int16_t)strlen(t) * 6 * 2;
  g->setCursor((ctx.w - tw) / 2, wave_y + kWaveH + 6);
  g->print(t);

  // Pulsing REC dot
  const bool blink = (elapsed_ms / 500) % 2 == 0;
  if (blink) g->fillCircle(ctx.w - 26, 26, 6, theme::ACCENT_RED);
  else       g->fillCircle(ctx.w - 26, 26, 6, theme::BG);
}

void VoiceMemo::renderList(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(theme::BG);
  render_header(g, ctx.w, "MEMOS", "swipe: record", theme::ACCENT_MAGENTA);

  g->setFont(nullptr);
  if (list_count_ == 0) {
    g->setTextSize(2);
    g->setTextColor(theme::TEXT_MID);
    const char* m = "no recordings yet";
    int16_t mw = (int16_t)strlen(m) * 6 * 2;
    g->setCursor((ctx.w - mw) / 2, ctx.h / 2 - 8);
    g->print(m);
  } else {
    int y = kHeaderH + 12;
    for (size_t i = 0; i < list_count_; ++i) {
      // Card
      g->fillRoundRect(12, y, ctx.w - 24, kListRowH - 6, 8,
                       ((int)i == playing_idx_) ? theme::SURFACE_HI : theme::SURFACE);
      g->drawRoundRect(12, y, ctx.w - 24, kListRowH - 6, 8, theme::DIVIDER);
      // Filename
      String short_name = list_names_[i];
      int slash = short_name.lastIndexOf('/');
      if (slash >= 0) short_name = short_name.substring(slash + 1);
      g->setTextSize(1);
      g->setTextColor(theme::TEXT_HI);
      g->setCursor(22, y + 8);
      g->print(short_name);
      // Duration
      uint32_t s = list_ms_[i] / 1000;
      uint32_t m = s / 60; s %= 60;
      char d[16]; snprintf(d, sizeof(d), "%lu:%02lu", (unsigned long)m, (unsigned long)s);
      g->setTextColor(theme::TEXT_LO);
      g->setCursor(22, y + 20);
      g->print(d);
      // Play indicator on the right
      const int px = ctx.w - 32;
      const int py = y + (kListRowH - 6) / 2 - 6;
      uint16_t pcol = ((int)i == playing_idx_) ? theme::ACCENT_MAGENTA : theme::TEXT_LO;
      g->fillTriangle(px, py, px, py + 12, px + 10, py + 6, pcol);
      y += kListRowH;
      if (y > ctx.h - 40) break;
    }
  }

  g->setTextSize(1);
  g->setTextColor(theme::TEXT_LO);
  const char* h = "tap to play  |  long-press = home";
  int16_t hw = (int16_t)strlen(h) * 6;
  g->setCursor((ctx.w - hw) / 2, ctx.h - 20);
  g->print(h);
}

void VoiceMemo::renderPlayback(AppContext& ctx, uint32_t elapsed_ms, uint32_t total_ms) {
  auto* g = ctx.gfx;
  const int bar_x = 20;
  const int bar_y = ctx.h - 60;
  const int bar_w = ctx.w - 40;
  const int bar_h = 6;
  g->fillRect(bar_x, bar_y, bar_w, bar_h, theme::SURFACE);
  int fw = total_ms ? (int)(((uint64_t)elapsed_ms * bar_w) / total_ms) : 0;
  if (fw > bar_w) fw = bar_w;
  g->fillRect(bar_x, bar_y, fw, bar_h, theme::ACCENT_MAGENTA);
}

// ---- gestures -------------------------------------------------------------

bool VoiceMemo::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TOUCH_DOWN:
      if (page_ == PAGE_RECORD && !recording_) {
        // Only start if the touch is inside the record area (below header)
        if (g.y >= kHeaderH) {
          startRecording(ctx);
          return true;
        }
      }
      return false;

    case Gesture::TOUCH_UP:
      if (recording_) {
        stopRecording(ctx, /*keep=*/true);
        return true;
      }
      return false;

    // Physical BOOT button on the side of the device: press and hold to
    // record without needing to keep a finger on the touchscreen. Works
    // from either the RECORD or LIST page (it forces us to RECORD).
    case Gesture::BUTTON_DOWN:
      if (!recording_) {
        if (page_ != PAGE_RECORD) {
          page_ = PAGE_RECORD;
          renderRecord(ctx);
        }
        startRecording(ctx);
      }
      return true;
    case Gesture::BUTTON_UP:
      if (recording_) {
        stopRecording(ctx, /*keep=*/true);
      }
      return true;

    case Gesture::TAP:
      if (page_ == PAGE_LIST && list_count_ > 0) {
        // Which row?
        int y = g.y - (kHeaderH + 12);
        if (y >= 0) {
          int idx = y / kListRowH;
          if (idx >= 0 && idx < (int)list_count_) {
            // Toggle: tap same idx again = stop
            if (playing_idx_ == idx) {
              if (play_file_) play_file_.close();
              playing_idx_ = -1;
              renderList(ctx);
              return true;
            }
            if (play_file_) play_file_.close();
            play_file_ = fs_ ? fs_->open(list_names_[idx].c_str(), FILE_READ) : File();
            if (play_file_) {
              // Skip 44-byte WAV header
              play_file_.seek(44);
              playing_idx_ = idx;
              play_start_ms_ = millis();
              play_total_ms_ = list_ms_[idx];
              last_ui_ms_ = play_start_ms_;
              renderList(ctx);
            }
            return true;
          }
        }
      }
      return true;

    case Gesture::SWIPE_LEFT:
    case Gesture::SWIPE_RIGHT:
      if (recording_) return true;   // don't nav while recording
      page_ = (page_ == PAGE_RECORD) ? PAGE_LIST : PAGE_RECORD;
      if (page_ == PAGE_LIST) refreshList();
      if (page_ == PAGE_RECORD) renderRecord(ctx);
      else                       renderList(ctx);
      return true;

    case Gesture::LONG_PRESS:
      if (recording_) {
        // Discard mid-recording rather than pop to home unexpectedly.
        stopRecording(ctx, /*keep=*/false);
        return true;
      }
      return false;

    default:
      return false;
  }
}

} // namespace mini
