// Tiny "shell" for the ESP32-S3 mini screen.
//
// Design goals:
//  * One App is active at a time; the Shell routes ticks + gestures to it.
//  * All apps share a single `AppContext` (display, IMU, prefs, audio-on flag)
//    so we don't re-init hardware per app.
//  * Home is just another App (idx 0 by convention). The Shell exposes a
//    universal "back to home" gesture (long-press ≥ 900ms anywhere).
//  * Rendering is immediate-mode: apps `render()` themselves on demand.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <SensorQMI8658.hpp>

namespace mini {

// Live touch state exposed to apps that need to react while a finger is
// still down (Voice Memo's hold-to-record, Torch's hold-to-dim, etc.).
struct TouchInput {
  bool     down   = false;
  int16_t  x      = 0;
  int16_t  y      = 0;
  uint32_t held_ms = 0;   // elapsed since press-down
};

struct AppContext {
  Arduino_GFX*      gfx        = nullptr;
  SensorQMI8658*    imu        = nullptr;
  Preferences*      prefs      = nullptr;
  bool              audio_ok   = false;
  bool              sd_ok      = false;
  int16_t           w          = 0;
  int16_t           h          = 0;
  TouchInput*       touch      = nullptr;   // updated by the gesture producer
};

struct Gesture {
  enum Type : uint8_t {
    NONE = 0,
    TOUCH_DOWN,    // finger just pressed down at (x, y)
    TOUCH_UP,      // finger just lifted from (x, y)
    TAP,           // brief touch (fires just after TOUCH_UP for short holds)
    LONG_PRESS,    // held ≥ 900ms without moving — reserved for shell nav
    SWIPE_UP,
    SWIPE_DOWN,
    SWIPE_LEFT,
    SWIPE_RIGHT,
    SHAKE,         // IMU-detected shake
    BUTTON_DOWN,   // physical side button (BOOT / GPIO0) just pressed
    BUTTON_UP,     // physical side button just released
  };
  Type    type = NONE;
  int16_t x = 0, y = 0;
  int16_t dx = 0, dy = 0;
};

// Well-known app "kinds" so Home can draw the right vector icon per app
// without needing RTTI (dynamic_cast is disabled in the ESP32 Arduino core).
enum class AppKind : uint8_t {
  OTHER    = 0,
  DICE     = 1,
  SIGNER   = 2,
  CUBE     = 3,
  VOICE    = 4,   // microphone icon
  TORCH    = 5,   // sun / flashlight icon
  EXPLORER = 6,   // stacked-block icon
  STEPS    = 7,   // shoe / footprint icon
  LEVEL    = 8,   // bubble level icon
  PIANO    = 9,   // piano keys icon
  ETCH     = 10,  // pencil / doodle icon
  WIFI     = 11,  // WiFi arcs icon
  NOTES    = 12,  // Nostr relay explorer (kind=1 feed)
  GENESIS  = 13,  // Lenia terrarium — one universe per device
  SNAKE    = 14,  // classic snake game
  VESPERS  = 15,  // generative composer, 3 voices via ES8311
  PHOTOS   = 16,  // photo gallery (JPEG from SD)
  VIDEOS   = 17,  // video archive (MJPEG player from SD)
};

class App {
 public:
  virtual ~App() = default;
  virtual const char* name() const = 0;
  // Optional short glyph fallback for the classic bitmap font (unused when a
  // kind()-based vector icon is available).
  virtual const char* glyph() const { return "?"; }
  virtual AppKind     kind()  const { return AppKind::OTHER; }
  virtual uint16_t    tint()  const { return 0xFFFF; }
  // If false, Home skips this app when laying out its tile grid. The app is
  // still reachable via serial debug and other apps' explicit switchTo calls.
  virtual bool        showInHome() const { return true; }
  // Lifecycle.
  virtual void onEnter(AppContext&) {}
  virtual void onExit(AppContext&) {}
  // Called every loop iteration (~500 Hz). Should be non-blocking.
  virtual void tick(AppContext&, uint32_t now_ms) {}
  // Called when the shell routes a gesture. Return true if handled;
  // if false and the gesture is LONG_PRESS, the shell will pop to home.
  virtual bool onGesture(AppContext&, const Gesture&) { return false; }
};

class Shell {
 public:
  void begin(AppContext ctx, App** apps, size_t n_apps, size_t home_idx);
  void tick(uint32_t now_ms);
  void deliverGesture(const Gesture& g);
  void switchTo(size_t idx);
  App*    current() { return apps_[current_]; }
  size_t  currentIdx() const { return current_; }
  size_t  count() const { return n_apps_; }
  App*    at(size_t i) { return apps_[i]; }

 private:
  AppContext ctx_{};
  App**   apps_ = nullptr;
  size_t  n_apps_ = 0;
  size_t  current_ = 0;
  size_t  home_idx_ = 0;
};

} // namespace mini
