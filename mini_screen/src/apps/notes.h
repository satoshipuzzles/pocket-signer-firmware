// Notes — a Nostr relay explorer.
//
// Connects (over WiFi) to a configurable relay via WebSocket, subscribes to
// kind=1 events (short-form text notes), and streams them into a scrolling
// list. Each note shows the author's shortened npub, the (truncated) note
// text, and how long ago it was created.
//
// Design:
//   * Uses WebSocketsClient (arduinoWebSockets) with WiFiClientSecure for
//     wss:// support.
//   * JSON parsing via ArduinoJson v7 (streaming filter to keep RAM low).
//   * Circular buffer of the last kEventCap events, newest at index 0.
//   * The relay URL is stored in NVS and defaults to wss://nos.lol on first
//     boot. User can edit it via SD (drop a text file at /nostr/relay.txt).
//
// UI:
//   * Header: relay hostname + a status pill (LIVE / OFFLINE / CONNECTING)
//   * Scrolling list of note cards. Swipe up/down to scroll.
//   * Tap a card to enlarge (show full author + full text on its own page).
//   * Long-press = home.
#pragma once
#include "../shell.h"
#include <WebSocketsClient.h>

namespace mini {

class Notes : public App {
 public:
  const char* name() const override { return "Notes"; }
  AppKind     kind() const override { return AppKind::NOTES; }
  uint16_t    tint() const override { return 0xF81F; }   // magenta — Nostr vibe

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

  // Called from the sketch loop when we're the current app; we forward it
  // to the underlying WebSocketsClient::loop().
  void pump();

  // Exposed so the C-style WS event trampoline can dispatch; not part of
  // the app-facing API.
  void handleWsEvent(WStype_t type, uint8_t* payload, size_t length);

 private:
  static constexpr size_t kEventCap = 30;
  struct Note {
    String pubkey_hex;     // 64-char lowercase hex (raw x-only pubkey)
    String content;        // truncated to at most 240 chars in-memory
    uint32_t created_at = 0;
  };

  enum State : uint8_t {
    S_OFFLINE = 0,
    S_CONNECTING,
    S_LIVE,
    S_ERROR,
  };

  State state_ = S_OFFLINE;
  String relay_url_ = "wss://nos.lol";
  String relay_host_display_;

  // Circular buffer, newest first at index 0.
  Note   events_[kEventCap];
  size_t event_count_ = 0;

  // Scroll offset (0 = top of feed). One card ≈ 90 px tall.
  int    scroll_offset_ = 0;

  // Selected note detail page
  int    detail_idx_ = -1;

  // Timing
  uint32_t last_render_ms_ = 0;
  uint32_t last_reconnect_attempt_ms_ = 0;
  uint32_t connect_started_ms_ = 0;

  // Housekeeping
  WebSocketsClient ws_;
  String subscription_id_ = "mini";
  bool   sub_open_ = false;

  // Life-cycle
  void loadRelay();
  void connect();
  void disconnect();
  void handleFrame(const char* data, size_t len);
  void handleEvent(const String& json);

  // Rendering
  void renderFeed(AppContext&);
  void renderDetail(AppContext&);
  void drawNoteCard(AppContext&, int16_t x, int16_t y, int16_t w, int16_t h,
                    const Note& n, bool pressed);
};

// Global pointer so a free-function WS event callback can find its Notes
// instance (WebSocketsClient callbacks are C-style function pointers).
extern Notes* g_notes_singleton;

} // namespace mini
