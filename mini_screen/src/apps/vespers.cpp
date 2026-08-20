#include "vespers.h"
#include "../ui/kit.h"

#include <math.h>
#include <esp_random.h>

namespace mini {

// Diatonic modes as semitone offsets from tonic. Each row has 7 pitches
// spanning one octave; we tile it up and down to reach every register.
static const int8_t kModes[7][7] = {
  {0, 2, 4, 5, 7, 9, 11},   // Ionian
  {0, 2, 3, 5, 7, 9, 10},   // Dorian
  {0, 1, 3, 5, 7, 8, 10},   // Phrygian
  {0, 2, 4, 6, 7, 9, 11},   // Lydian
  {0, 2, 4, 5, 7, 9, 10},   // Mixolydian
  {0, 2, 3, 5, 7, 8, 10},   // Aeolian (natural minor)
  {0, 1, 3, 5, 6, 8, 10},   // Locrian
};
static const char* kModeName[7] = {
  "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian"
};

// Voice registers in MIDI notes.
static constexpr int8_t kBassLo = 36, kBassHi = 50;
static constexpr int8_t kMidLo  = 55, kMidHi  = 72;
static constexpr int8_t kHighLo = 72, kHighHi = 86;

static constexpr uint32_t kModeShiftEveryMs = 120u * 1000u;   // 2 min

// Fast MIDI → frequency: 440 * 2^((m - 69)/12). Precompute a small table
// so we avoid a powf() per note.
static float kMidiFreq[128];
static bool  kMidiFreqInited = false;
static void init_midi_freq_table() {
  if (kMidiFreqInited) return;
  for (int m = 0; m < 128; ++m) {
    kMidiFreq[m] = 440.0f * powf(2.0f, (float)(m - 69) / 12.0f);
  }
  kMidiFreqInited = true;
}

// Uniform in [0, 1)
static inline float urand() {
  return (float)(esp_random() & 0xFFFFFF) / (float)0x1000000;
}

// ---- helpers -------------------------------------------------------------

int Vespers::scaleDegreeAt(int index) const {
  return (int)kModes[mode_index_][((index % 7) + 7) % 7];
}

int Vespers::clampToMode(int pitch) const {
  // Snap pitch to nearest note in the current mode centered on tonic_.
  int best = pitch, best_dist = 999;
  for (int oct = -3; oct <= 3; ++oct) {
    int base = tonic_ + oct * 12;
    for (int d = 0; d < 7; ++d) {
      int cand = base + kModes[mode_index_][d];
      int dist = abs(cand - pitch);
      if (dist < best_dist) { best = cand; best_dist = dist; }
    }
  }
  return best;
}

// Prefer small steps, occasional leaps, resolves toward tonic near octave
// boundaries.
int Vespers::chooseNextPitch(int voice, int prev_pitch) {
  int lo, hi;
  switch (voice) {
    case 0: lo = kBassLo; hi = kBassHi; break;
    case 1: lo = kMidLo;  hi = kMidHi;  break;
    default: lo = kHighLo; hi = kHighHi; break;
  }
  // Roll: step (60%), leap (30%), repeat (10%).
  float r = urand();
  int delta_semis;
  if (r < 0.10f)        delta_semis = 0;
  else if (r < 0.70f)   delta_semis = (urand() < 0.5f ? -1 : 1) * (1 + (int)(urand() * 3.0f));
  else                  delta_semis = (urand() < 0.5f ? -1 : 1) * (3 + (int)(urand() * 4.0f));
  int cand = prev_pitch + delta_semis;
  // Fold into register range with a soft rebound.
  if (cand < lo) cand = lo + (lo - cand);
  if (cand > hi) cand = hi - (cand - hi);
  if (cand < lo) cand = lo;
  if (cand > hi) cand = hi;
  return clampToMode(cand);
}

uint32_t Vespers::nextEventDelay(int voice) const {
  // Poisson-ish spacing per voice (in ms).
  switch (voice) {
    case 0: return 1800 + (uint32_t)(urand() * 2400.0f);   // 1.8..4.2 s
    case 1: return  550 + (uint32_t)(urand() *  900.0f);   // 0.55..1.45 s
    default:return 4500 + (uint32_t)(urand() *12000.0f);   // 4.5..16.5 s
  }
}
uint32_t Vespers::nextNoteDuration(int voice) const {
  switch (voice) {
    case 0: return 1400 + (uint32_t)(urand() * 1600.0f);
    case 1: return  400 + (uint32_t)(urand() *  700.0f);
    default:return  900 + (uint32_t)(urand() * 1600.0f);
  }
}

void Vespers::maybeShiftMode(uint32_t now_ms) {
  if (now_ms - last_mode_shift_ms_ < kModeShiftEveryMs) return;
  mode_index_ = (uint8_t)(esp_random() % 7);
  last_mode_shift_ms_ = now_ms;
  hud_dirty_ = true;
  Serial.printf("[vespers] mode -> %s\n", kModeName[mode_index_]);
}

void Vespers::addRollEntry(const ActiveNote& n) {
  RollNote& r = roll_[roll_head_];
  r.pitch    = n.pitch;
  r.voice    = n.voice;
  r.start_ms = n.start_ms;
  r.dur_ms   = n.dur_ms;
  roll_head_ = (roll_head_ + 1) % kRollCap;
  if (roll_count_ < kRollCap) ++roll_count_;
}

void Vespers::spawnNote(int voice, int pitch, uint32_t now_ms) {
  for (int i = 0; i < kMaxNotes; ++i) {
    if (notes_[i].active) continue;
    notes_[i].active     = true;
    notes_[i].voice      = (int8_t)voice;
    notes_[i].pitch      = (int8_t)pitch;
    notes_[i].freq       = kMidiFreq[pitch];
    notes_[i].phase      = 0.0f;
    notes_[i].start_ms   = now_ms;
    notes_[i].dur_ms     = nextNoteDuration(voice);
    notes_[i].attack_ms  = 80;
    notes_[i].release_ms = 320;
    voices_[voice].last_pitch = (int8_t)pitch;
    addRollEntry(notes_[i]);
    ++notes_released_;
    hud_dirty_ = true;
    return;
  }
  // No slot available — drop the event silently.
}

void Vespers::generateEvents(uint32_t now_ms) {
  for (int v = 0; v < 3; ++v) {
    if (now_ms < voices_[v].next_event_ms) continue;
    int prev = voices_[v].last_pitch;
    if (prev == 0) {   // first note of voice: pick anywhere in register
      int lo = (v == 0 ? kBassLo : v == 1 ? kMidLo : kHighLo);
      int hi = (v == 0 ? kBassHi : v == 1 ? kMidHi : kHighHi);
      prev = (int)(lo + (esp_random() % (hi - lo + 1)));
      prev = clampToMode(prev);
    }
    int next = chooseNextPitch(v, prev);
    spawnNote(v, next, now_ms);
    voices_[v].next_event_ms = now_ms + nextEventDelay(v);
  }
}

// ---- audio synthesis ---------------------------------------------------

// Returns per-note amplitude in [0, 1] given elapsed time within the note.
static inline float envelope(uint32_t elapsed_ms, uint32_t dur_ms,
                             uint32_t attack_ms, uint32_t release_ms) {
  if (elapsed_ms < attack_ms) {
    return (float)elapsed_ms / (float)attack_ms;
  }
  if (elapsed_ms >= dur_ms) {
    uint32_t after = elapsed_ms - dur_ms;
    if (after >= release_ms) return 0.0f;
    return 1.0f - (float)after / (float)release_ms;
  }
  return 1.0f;
}

void Vespers::renderAudioChunk(uint32_t now_ms) {
  const float dt = 1.0f / (float)kSampleRate;
  // Zero the mix buffer
  for (int i = 0; i < kBufFrames; ++i) audio_buf_[i] = 0;

  for (int n = 0; n < kMaxNotes; ++n) {
    ActiveNote& note = notes_[n];
    if (!note.active) continue;
    uint32_t note_end = note.start_ms + note.dur_ms + note.release_ms;
    if (now_ms > note_end + 40) { note.active = false; continue; }
    // Voice-specific amplitude weight (bass louder for warmth).
    float voice_amp = (note.voice == 0 ? 0.55f
                     : note.voice == 1 ? 0.42f
                                       : 0.30f);
    float omega = 2.0f * (float)M_PI * note.freq;
    for (int i = 0; i < kBufFrames; ++i) {
      uint32_t t_ms = now_ms + (uint32_t)((i * 1000) / kSampleRate);
      uint32_t elapsed = (t_ms > note.start_ms) ? (t_ms - note.start_ms) : 0;
      float env = envelope(elapsed, note.dur_ms, note.attack_ms, note.release_ms);
      if (env <= 0.0f) continue;
      float s = sinf(note.phase) * env * voice_amp;
      // Accumulate (int16 add)
      int32_t v = (int32_t)audio_buf_[i] + (int32_t)(s * 8000.0f);
      if (v > 32000)  v =  32000;
      if (v < -32000) v = -32000;
      audio_buf_[i] = (int16_t)v;
      note.phase += omega * dt;
      if (note.phase > 6.28318f) note.phase -= 6.28318f;
    }
  }
  if (play_) play_(audio_buf_, kBufFrames);
}

// ---- rendering ---------------------------------------------------------

// Screen layout:
//   [ header 58 ]
//   [ piano roll — full width, remaining height ]
//   [ hint 30 ]
void Vespers::drawStatic(AppContext& ctx) {
  ui::begin_screen(ctx.gfx, ctx.w, "Vespers", tint());
  // Static frame around the roll area
  const int16_t rx = 8, ry = ui::kHeaderH + 6;
  const int16_t rw = ctx.w - 16;
  const int16_t rh = ctx.h - ry - ui::kHintH - 6;
  ctx.gfx->drawRoundRect(rx, ry, rw, rh, 8, theme::DIVIDER);
  drawHUD(ctx);
  ui::screen_hint(ctx.gfx, ctx.w, ctx.h,
                  "tap = freeze · long-press = home");
}

void Vespers::drawHUD(AppContext& ctx) {
  auto* g = ctx.gfx;
  // Wipe HUD area (right side of header row)
  const int16_t hx = ctx.w / 2 - 10;
  g->fillRect(hx, 4, ctx.w - hx - 10, 26, theme::BG);
  theme::use_font(g, theme::FONT_SMALL);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s · %lu",
           kModeName[mode_index_], (unsigned long)notes_released_);
  int16_t tw = ui::text_w(g, buf);
  ui::draw_top(g, buf, ctx.w - tw - 12, 10, theme::TEXT_MID);
  hud_dirty_ = false;
}

// Piano roll draws in the frame area:
//   * X axis = time (right = now, left = past). Window = 24s.
//   * Y axis = pitch (MIDI 36 at bottom → 96 at top → 60 semitones spread)
//   * Each note is a colored horizontal streak whose alpha fades with age.
void Vespers::drawRoll(AppContext& ctx, uint32_t now_ms) {
  auto* g = ctx.gfx;
  const int16_t rx = 8, ry = ui::kHeaderH + 6;
  const int16_t rw = ctx.w - 16;
  const int16_t rh = ctx.h - ry - ui::kHintH - 6;
  // Clear interior
  g->fillRoundRect(rx + 1, ry + 1, rw - 2, rh - 2, 7, theme::BG);

  const uint32_t window_ms = 24000;
  const int MIDI_LO = 36, MIDI_HI = 96;   // 60 semitones
  const float px_per_ms = (float)(rw - 8) / (float)window_ms;
  const float px_per_semi = (float)(rh - 8) / (float)(MIDI_HI - MIDI_LO);

  auto voice_color = [&](int v) -> uint16_t {
    switch (v) {
      case 0: return 0x9CDF;   // bass:  soft blue
      case 1: return 0xDEDF;   // mid:   warm off-white
      default:return 0xFCA0;   // high:  amber
    }
  };

  // Draw notes newest-first so older/faded notes get overdrawn last.
  int n = roll_count_;
  int idx = roll_head_ - 1;
  for (int k = 0; k < n; ++k, --idx) {
    if (idx < 0) idx += kRollCap;
    const RollNote& r = roll_[idx];
    int64_t age = (int64_t)now_ms - (int64_t)r.start_ms;
    if (age > (int64_t)window_ms + 4000) continue;
    // Compute right edge = now (rx + rw), left edge = start
    int x_end = rx + rw - 4 - (int)(age * px_per_ms);
    int x_start = x_end - (int)(r.dur_ms * px_per_ms);
    if (x_end < rx + 2 || x_start > rx + rw - 2) continue;
    if (x_start < rx + 2) x_start = rx + 2;
    if (x_end > rx + rw - 2) x_end = rx + rw - 2;
    int y = ry + rh - 4 - (int)((r.pitch - MIDI_LO) * px_per_semi);
    if (y < ry + 2 || y > ry + rh - 2) continue;
    // Age-based dim: reduce brightness after the note ends.
    uint16_t c = voice_color(r.voice);
    if ((uint32_t)age > r.dur_ms) {
      uint32_t fade_ms = (uint32_t)age - r.dur_ms;
      uint32_t rem_max = window_ms - r.dur_ms;
      uint16_t dim = 255 - (uint16_t)((fade_ms * 200) / (rem_max + 1));
      // Multiply each channel by dim/255 (approximate)
      uint16_t rr = ((c >> 11) & 0x1F) * dim / 255;
      uint16_t gg = ((c >> 5)  & 0x3F) * dim / 255;
      uint16_t bb =  (c        & 0x1F) * dim / 255;
      c = (rr << 11) | (gg << 5) | bb;
    }
    g->drawFastHLine(x_start, y,     x_end - x_start + 1, c);
    g->drawFastHLine(x_start, y + 1, x_end - x_start + 1, c);
  }

  // Sweep bar at "now"
  g->drawFastVLine(rx + rw - 4, ry + 4, rh - 8, theme::ACCENT_CYAN);
}

void Vespers::drawFrozenOverlay(AppContext& ctx) {
  auto* g = ctx.gfx;
  const int16_t w = 300, h = 160;
  const int16_t x = (ctx.w - w) / 2;
  const int16_t y = (ctx.h - h) / 2;
  g->fillRoundRect(x, y, w, h, 12, 0x0000);
  g->drawRoundRect(x, y, w, h, 12, theme::ACCENT_CYAN);
  theme::use_font(g, theme::FONT_HEADING_M);
  ui::draw_top_centered(g, "frozen", ctx.w / 2, y + 14, theme::ACCENT_CYAN);
  theme::use_font(g, theme::FONT_SMALL);
  char buf[64];
  snprintf(buf, sizeof(buf), "mode: %s", kModeName[mode_index_]);
  ui::draw_top_centered(g, buf, ctx.w / 2, y + 52, theme::TEXT_HI);
  int tonic_pc = ((tonic_ % 12) + 12) % 12;
  static const char* pcname[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  snprintf(buf, sizeof(buf), "key: %s", pcname[tonic_pc]);
  ui::draw_top_centered(g, buf, ctx.w / 2, y + 74, theme::TEXT_HI);
  snprintf(buf, sizeof(buf), "notes released: %lu",
           (unsigned long)notes_released_);
  ui::draw_top_centered(g, buf, ctx.w / 2, y + 96, theme::TEXT_HI);
  ui::draw_top_centered(g, "tap to resume",
                        ctx.w / 2, y + h - 22, theme::TEXT_LO);
}

// ---- lifecycle ----------------------------------------------------------

void Vespers::seedFromEntropy() {
  mode_index_ = (uint8_t)(esp_random() % 7);
  tonic_ = 55 + (int8_t)(esp_random() % 8);   // G3..D#4
  for (int v = 0; v < 3; ++v) {
    voices_[v].last_pitch    = 0;
    voices_[v].next_event_ms = 0;
  }
  memset(notes_, 0, sizeof(notes_));
  roll_head_ = 0;
  roll_count_ = 0;
  notes_released_ = 0;
  Serial.printf("[vespers] seed: mode=%s tonic=%d\n",
                kModeName[mode_index_], (int)tonic_);
}

void Vespers::onEnter(AppContext& ctx) {
  init_midi_freq_table();
  seedFromEntropy();
  frozen_ = false;
  hud_dirty_ = true;
  entered_ms_ = millis();
  last_mode_shift_ms_ = entered_ms_;
  last_roll_draw_ms_ = 0;
  drawStatic(ctx);
}

void Vespers::onExit(AppContext&) {
  // Silence remaining notes so no ghost samples linger.
  memset(notes_, 0, sizeof(notes_));
}

void Vespers::tick(AppContext& ctx, uint32_t now_ms) {
  if (frozen_) return;

  maybeShiftMode(now_ms);
  generateEvents(now_ms);

  // Push one audio chunk (~16ms of sound). This blocks briefly on the I2S
  // write, which naturally paces the tick loop.
  renderAudioChunk(now_ms);

  // Redraw the piano roll about 5 times a second.
  if (now_ms - last_roll_draw_ms_ >= 200) {
    drawRoll(ctx, now_ms);
    if (hud_dirty_) drawHUD(ctx);
    last_roll_draw_ms_ = now_ms;
  }
}

bool Vespers::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TAP:
      frozen_ = !frozen_;
      if (frozen_) drawFrozenOverlay(ctx);
      else         { drawStatic(ctx); hud_dirty_ = true; }
      return true;
    case Gesture::LONG_PRESS:
      return false;   // let shell go home
    default:
      return true;
  }
}

} // namespace mini
