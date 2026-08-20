// Vespers — a small generative chamber musician.
//
// Three voices (bass / mid / high) speak diatonic notes over a slow tempo,
// each choosing its next pitch by a lightly-weighted Markov step biased
// toward small intervals and contrary motion. Mode drifts every couple of
// minutes; nothing repeats.
//
// Synth: pure sine oscillators with attack/release envelopes, summed to the
// ES8311 output at 16 kHz mono. Furniture volume. Not a synthesizer's
// synthesizer — closer to a music box or glass harmonica.
//
// Display: a scrolling piano-roll of the last ~24 seconds. Every note is a
// bright streak that fades as its sound decays. Tap once to freeze the
// display and see current mode / key / notes-released counter. Released is
// the right word: nothing is recorded. Every phrase is performed once for
// whoever is in the room and then gone from the universe.
//
// This is the minimum-viable Vespers — three-voice sequencer with mode drift
// and honest Markov note selection. Full counterpoint rules, time-of-day
// awareness, and slow weather-layer parameter drift are next-round work.
#pragma once
#include "../shell.h"

namespace mini {

class Vespers : public App {
 public:
  const char* name() const override { return "Vespers"; }
  AppKind     kind() const override { return AppKind::VESPERS; }
  uint16_t    tint() const override { return 0xDEDF; }   // warm off-white / candlelight

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

  // Set by the sketch so we can push mono int16 samples out.
  void setAudio(void (*play)(const int16_t*, size_t)) { play_ = play; }

  static constexpr int kSampleRate  = 16000;
  static constexpr int kBufFrames   = 256;    // ~16ms per chunk
  static constexpr int kMaxNotes    = 24;     // simultaneously-sounding
  static constexpr int kRollCap     = 64;     // piano-roll history

 private:
  struct ActiveNote {
    float    freq;
    float    phase;        // radians
    uint32_t start_ms;
    uint32_t dur_ms;
    uint16_t attack_ms;
    uint16_t release_ms;
    int8_t   voice;        // 0=bass 1=mid 2=high
    int8_t   pitch;        // MIDI number
    bool     active;
  };

  struct RollNote {
    int8_t   pitch;
    int8_t   voice;
    uint32_t start_ms;
    uint32_t dur_ms;
  };

  struct Voice {
    int8_t   last_pitch;      // MIDI
    uint32_t next_event_ms;
  };

  ActiveNote notes_[kMaxNotes];
  RollNote   roll_[kRollCap];
  int        roll_head_ = 0;     // next slot to write (ring)
  int        roll_count_ = 0;    // total in ring (<=kRollCap)
  Voice      voices_[3]{};

  uint8_t    mode_index_ = 0;       // 0..6, Ionian .. Locrian
  int8_t     tonic_ = 60;           // MIDI C4
  uint32_t   entered_ms_ = 0;
  uint32_t   last_mode_shift_ms_ = 0;
  uint32_t   notes_released_ = 0;
  bool       frozen_ = false;
  bool       hud_dirty_ = true;
  uint32_t   last_roll_draw_ms_ = 0;

  int16_t    audio_buf_[kBufFrames];
  void      (*play_)(const int16_t*, size_t) = nullptr;

  // ---- generation ----------------------------------------------------------
  void seedFromEntropy();
  int  scaleDegreeAt(int index) const;   // returns semitones from tonic
  int  clampToMode(int pitch) const;     // snap pitch to nearest scale note
  int  chooseNextPitch(int voice, int prev_pitch);
  uint32_t nextEventDelay(int voice) const;
  uint32_t nextNoteDuration(int voice) const;
  void maybeShiftMode(uint32_t now_ms);
  void addRollEntry(const ActiveNote& n);

  // ---- events ------------------------------------------------------------
  void spawnNote(int voice, int pitch, uint32_t now_ms);
  void generateEvents(uint32_t now_ms);

  // ---- audio ------------------------------------------------------------
  void renderAudioChunk(uint32_t now_ms);

  // ---- rendering ---------------------------------------------------------
  void drawStatic(AppContext&);
  void drawRoll(AppContext&, uint32_t now_ms);
  void drawHUD(AppContext&);
  void drawFrozenOverlay(AppContext&);
};

} // namespace mini
