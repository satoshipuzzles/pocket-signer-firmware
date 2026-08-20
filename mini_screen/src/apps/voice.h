// Voice Memo — press-and-hold anywhere on the record area to capture audio
// from the ES8311 microphone at 16 kHz mono, 16-bit PCM.
//
// While the finger is held:
//   * a scrolling waveform strip shows the last ~4 seconds of amplitude
//   * a mm:ss timer counts up
//   * a red REC dot pulses
//
// On finger release:
//   * anything under 300ms is discarded as an accidental tap
//   * otherwise a WAV file is written to /voice/memo_NNN.wav on the SD card
//     (auto-numbered, so nothing ever overwrites)
//
// A second page shows a list of recorded memos — tap a row to play it back
// through the ES8311 speaker with a moving progress bar.
//
// If no SD card is present the app still works for hold-to-record preview,
// but writes are disabled and the screen shows an "insert SD" warning.
#pragma once
#include "../shell.h"

// FS handle is set from the sketch via VoiceMemo::setStorage()
class fs__forward__;
#include <FS.h>

namespace mini {

class VoiceMemo : public App {
 public:
  const char* name() const override { return "Voice"; }
  AppKind     kind() const override { return AppKind::VOICE; }
  uint16_t    tint() const override { return 0xF81F; }   // magenta

  // Hooks provided by the sketch during setup(). audio_read reads N int16
  // samples from the mic into `dst` and returns how many samples were
  // actually read (may be < n if the driver isn't ready). audio_write plays
  // N samples to the speaker.
  using ReadFn  = size_t (*)(int16_t* dst, size_t n);
  using WriteFn = void   (*)(const int16_t* src, size_t n);
  void setAudio(ReadFn r, WriteFn w) { read_ = r; write_ = w; }
  void setStorage(fs::FS* filesystem, const char* dir) {
    fs_ = filesystem; dir_ = dir;
  }

  void onEnter(AppContext&) override;
  void onExit(AppContext&)  override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  enum Page : uint8_t { PAGE_RECORD = 0, PAGE_LIST = 1 };

  Page   page_ = PAGE_RECORD;
  ReadFn read_  = nullptr;
  WriteFn write_ = nullptr;
  fs::FS* fs_   = nullptr;
  const char* dir_ = "/voice";

  // Recording state
  bool     recording_ = false;
  uint32_t rec_start_ms_ = 0;
  uint32_t rec_samples_written_ = 0;
  File     rec_file_;
  int8_t   wave_ring_[128] = {0};   // 0..100 amplitude per column
  uint8_t  wave_head_ = 0;
  uint32_t last_wave_push_ms_ = 0;
  uint32_t last_ui_ms_ = 0;

  // Playback state
  int  playing_idx_ = -1;
  File play_file_;
  uint32_t play_start_ms_ = 0;
  uint32_t play_total_ms_ = 0;

  // Recording list
  static constexpr size_t kMaxList = 24;
  String    list_names_[kMaxList];
  uint32_t  list_ms_[kMaxList] = {0};
  size_t    list_count_ = 0;

  // Rendering
  void renderRecord(AppContext&);
  void renderList  (AppContext&);
  void renderRecordingProgress(AppContext&, uint32_t elapsed_ms);
  void renderPlayback(AppContext&, uint32_t elapsed_ms, uint32_t total_ms);

  // Helpers
  bool startRecording(AppContext&);
  void stopRecording (AppContext&, bool keep);
  void pumpRecording (AppContext&);
  void pumpPlayback  (AppContext&);
  void refreshList();
  int  nextRecordingIndex();
  bool writeWavHeader(File& f);
  bool finalizeWav(File& f, uint32_t sample_count);
};

} // namespace mini
