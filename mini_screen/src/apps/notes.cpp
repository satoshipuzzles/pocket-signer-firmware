#include "notes.h"
#include "../ui/kit.h"
#include "../crypto/bech32.h"

#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>

namespace mini {

Notes* g_notes_singleton = nullptr;

static constexpr const char* kNsRelay  = "notes";
static constexpr const char* kKRelay   = "url";

// C-style callback for WebSocketsClient; dispatch to the singleton.
static void ws_event_trampoline(WStype_t type, uint8_t* payload, size_t length) {
  if (g_notes_singleton) g_notes_singleton->handleWsEvent(type, payload, length);
}

// Parse a wss:// URL into host + port + path.
// Only bare 'wss://host[:port][/path]' — no fragments, no query.
static bool parse_ws_url(const String& url, String& host, uint16_t& port,
                         String& path, bool& use_ssl) {
  int scheme_end = -1;
  if (url.startsWith("wss://"))      { scheme_end = 6; use_ssl = true;  port = 443; }
  else if (url.startsWith("ws://"))  { scheme_end = 5; use_ssl = false; port = 80;  }
  else return false;
  int slash = url.indexOf('/', scheme_end);
  int host_end = (slash >= 0) ? slash : (int)url.length();
  String host_port = url.substring(scheme_end, host_end);
  int colon = host_port.indexOf(':');
  if (colon >= 0) {
    host = host_port.substring(0, colon);
    port = (uint16_t)host_port.substring(colon + 1).toInt();
  } else {
    host = host_port;
  }
  path = (slash >= 0) ? url.substring(slash) : String("/");
  return host.length() > 0;
}

// ---- lifecycle ------------------------------------------------------------

void Notes::onEnter(AppContext& ctx) {
  g_notes_singleton = this;
  loadRelay();
  detail_idx_ = -1;
  last_render_ms_ = 0;

  if (WiFi.status() != WL_CONNECTED) {
    state_ = S_OFFLINE;
  } else {
    connect();
  }
  renderFeed(ctx);
}

void Notes::onExit(AppContext&) {
  disconnect();
  g_notes_singleton = nullptr;
}

void Notes::loadRelay() {
  // Priority: SD card /nostr/relay.txt > NVS > default.
  String url;
  if (SD_MMC.exists("/nostr/relay.txt")) {
    File f = SD_MMC.open("/nostr/relay.txt", FILE_READ);
    if (f) {
      while (f.available() && url.length() < 128) {
        char c = (char)f.read();
        if (c == '\n' || c == '\r') break;
        url += c;
      }
      f.close();
    }
  }
  if (url.length() == 0) {
    Preferences p;
    if (p.begin(kNsRelay, /*ro=*/true)) {
      url = p.getString(kKRelay, "");
      p.end();
    }
  }
  if (url.length() == 0) url = "wss://nos.lol";
  relay_url_ = url;
  // Pretty display: strip scheme
  int idx = relay_url_.indexOf("//");
  relay_host_display_ = (idx >= 0) ? relay_url_.substring(idx + 2) : relay_url_;
  Serial.printf("[notes] relay = %s\n", relay_url_.c_str());
}

void Notes::connect() {
  String host, path;
  uint16_t port;
  bool ssl;
  if (!parse_ws_url(relay_url_, host, port, path, ssl)) {
    state_ = S_ERROR;
    Serial.printf("[notes] bad url: %s\n", relay_url_.c_str());
    return;
  }
  Serial.printf("[notes] connecting %s://%s:%d%s\n",
                ssl ? "wss" : "ws", host.c_str(), (int)port, path.c_str());
  state_ = S_CONNECTING;
  connect_started_ms_ = millis();
  ws_.onEvent(ws_event_trampoline);
  // arduinoWebSockets uses beginSSL() vs begin() for TLS.
  if (ssl) ws_.beginSSL(host.c_str(), port, path.c_str());
  else     ws_.begin   (host.c_str(), port, path.c_str());
  ws_.setReconnectInterval(5000);
  sub_open_ = false;
}

void Notes::disconnect() {
  if (state_ != S_OFFLINE) {
    ws_.disconnect();
  }
  state_ = S_OFFLINE;
  sub_open_ = false;
}

void Notes::handleWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[notes] ws CONNECTED\n");
      state_ = S_LIVE;
      // Subscribe: [ "REQ", "<sub-id>", { "kinds": [1], "limit": 30 } ]
      {
        String req = String("[\"REQ\",\"") + subscription_id_ +
                     "\",{\"kinds\":[1],\"limit\":30}]";
        ws_.sendTXT(req);
        sub_open_ = true;
      }
      break;
    case WStype_DISCONNECTED:
      Serial.println("[notes] ws DISCONNECTED");
      if (state_ == S_LIVE) state_ = S_CONNECTING;
      sub_open_ = false;
      break;
    case WStype_TEXT:
      handleFrame(reinterpret_cast<const char*>(payload), length);
      break;
    case WStype_ERROR:
      Serial.println("[notes] ws ERROR");
      state_ = S_ERROR;
      break;
    default:
      break;
  }
}

void Notes::handleFrame(const char* data, size_t len) {
  // Relay frames are JSON arrays. We care about:
  //   [ "EVENT", "<sub-id>", <event-json> ]
  //   [ "EOSE",  "<sub-id>" ]
  //   [ "NOTICE", "<message>" ]
  // We do a minimal top-level scan to identify the kind before parsing the
  // nested event object with ArduinoJson (which is heavier).
  if (len < 8) return;
  const char* first_quote = strchr(data, '"');
  if (!first_quote) return;
  if (!strncmp(first_quote + 1, "EVENT", 5)) {
    // Extract the event JSON — it's the 3rd array element, which starts after
    // the 2nd comma.
    int comma_count = 0;
    size_t event_start = 0;
    for (size_t i = 0; i < len; ++i) {
      if (data[i] == ',') {
        ++comma_count;
        if (comma_count == 2) {
          event_start = i + 1;
          break;
        }
      }
    }
    if (event_start == 0 || event_start >= len) return;
    // Trim leading whitespace
    while (event_start < len && (data[event_start] == ' ' ||
                                 data[event_start] == '\t')) ++event_start;
    // The event ends at the last ']' of the outer array. Since events
    // contain no unquoted brackets outside strings, we can just strip the
    // trailing ']'.
    size_t event_end = len;
    while (event_end > event_start && data[event_end - 1] != '}') --event_end;
    if (event_end <= event_start) return;
    String json(data + event_start, event_end - event_start);
    handleEvent(json);
  } else if (!strncmp(first_quote + 1, "NOTICE", 6)) {
    Serial.print("[notes] NOTICE: ");
    Serial.write((const uint8_t*)data, len);
    Serial.println();
  }
  // EOSE, OK, AUTH etc. — ignored for now.
}

void Notes::handleEvent(const String& json) {
  // Parse only the fields we care about to keep the doc small.
  JsonDocument filter;
  filter["pubkey"]     = true;
  filter["content"]    = true;
  filter["kind"]       = true;
  filter["created_at"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json,
                              DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[notes] json err: %s\n", err.c_str());
    return;
  }
  int kind = doc["kind"] | -1;
  if (kind != 1) return;   // only text notes

  Note n;
  const char* pk = doc["pubkey"] | "";
  const char* co = doc["content"] | "";
  n.pubkey_hex = String(pk);
  n.content = String(co);
  if (n.content.length() > 240) n.content = n.content.substring(0, 240) + "…";
  n.created_at = doc["created_at"] | 0;

  // Prepend, keep at most kEventCap
  if (event_count_ < kEventCap) ++event_count_;
  for (size_t i = event_count_ - 1; i > 0; --i) {
    events_[i] = events_[i - 1];
  }
  events_[0] = n;
  // Trigger a re-render soon
  last_render_ms_ = 0;
}

// ---- tick + gestures ------------------------------------------------------

void Notes::tick(AppContext& ctx, uint32_t now_ms) {
  if (state_ != S_OFFLINE) ws_.loop();

  // If WiFi came online while we were showing offline, opportunistically try.
  if (state_ == S_OFFLINE && WiFi.status() == WL_CONNECTED &&
      (now_ms - last_reconnect_attempt_ms_) > 3000) {
    last_reconnect_attempt_ms_ = now_ms;
    connect();
    renderFeed(ctx);
  }
  // Live-refresh feed every ~700ms (new events set last_render_ms_ = 0)
  if ((now_ms - last_render_ms_) > 700) {
    last_render_ms_ = now_ms;
    if (detail_idx_ < 0) renderFeed(ctx);
  }
}

void Notes::pump() { ws_.loop(); }

bool Notes::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::SWIPE_UP:
      if (detail_idx_ < 0) {
        scroll_offset_++;
        if (scroll_offset_ > (int)event_count_ - 1) scroll_offset_ = (int)event_count_ - 1;
        if (scroll_offset_ < 0) scroll_offset_ = 0;
        renderFeed(ctx);
      }
      return true;
    case Gesture::SWIPE_DOWN:
      if (detail_idx_ < 0) {
        scroll_offset_--;
        if (scroll_offset_ < 0) scroll_offset_ = 0;
        renderFeed(ctx);
      }
      return true;
    case Gesture::TAP: {
      if (detail_idx_ >= 0) {
        // Any tap on the detail page returns to feed
        detail_idx_ = -1;
        renderFeed(ctx);
        return true;
      }
      // Feed: work out which card was tapped based on layout
      const int16_t list_top = ui::kHeaderH + 46;
      const int16_t card_h   = 96;
      const int16_t gap      = 8;
      int rel = (int)g.y - (int)list_top;
      if (rel < 0) return true;
      int visible_idx = rel / (card_h + gap);
      int absolute_idx = scroll_offset_ + visible_idx;
      if (absolute_idx >= 0 && absolute_idx < (int)event_count_) {
        detail_idx_ = absolute_idx;
        renderDetail(ctx);
      }
      return true;
    }
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

// ---- rendering ------------------------------------------------------------

// Convert 64-hex pubkey → shortened bech32 "npub1abc…xyz"
static String short_npub(const String& hex64) {
  if (hex64.length() != 64) return hex64.substring(0, 12);
  uint8_t bin[32];
  for (int i = 0; i < 32; ++i) {
    auto hv = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };
    bin[i] = (uint8_t)((hv(hex64[2*i]) << 4) | hv(hex64[2*i+1]));
  }
  String npub = nostrocrypto::bech32_encode_32("npub", bin);
  if (npub.length() < 16) return npub;
  return npub.substring(0, 8) + "…" + npub.substring(npub.length() - 6);
}

static String age_str(uint32_t created_at) {
  // We don't have a real clock, so we can't compute true "N minutes ago".
  // Show the last portion of the epoch instead as a stable-ish label.
  // TODO: when WiFi time-sync arrives, switch to relative age.
  if (created_at == 0) return String("");
  char buf[16];
  snprintf(buf, sizeof(buf), "#%lu", (unsigned long)(created_at % 100000));
  return String(buf);
}

void Notes::drawNoteCard(AppContext& ctx, int16_t x, int16_t y,
                         int16_t w, int16_t h, const Note& n, bool pressed) {
  auto* g = ctx.gfx;
  ui::card(g, x, y, w, h, pressed ? theme::SURFACE_HI : theme::SURFACE, 0,
           ui::kRadiusMd);
  // Left accent stripe
  g->fillRoundRect(x, y, 4, h, 2, theme::ACCENT_MAGENTA);

  // Author (shortened npub) top-left, age top-right
  theme::use_font(g, theme::FONT_MONO);
  String sp = short_npub(n.pubkey_hex);
  ui::draw_top(g, sp.c_str(), x + 14, y + 12, theme::ACCENT_MAGENTA);
  String age = age_str(n.created_at);
  if (age.length()) {
    theme::use_font(g, theme::FONT_SMALL);
    int16_t aw = ui::text_w(g, age.c_str());
    ui::draw_top(g, age.c_str(), x + w - aw - 14, y + 12, theme::TEXT_LO);
  }

  // Content
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_paragraph(g, n.content, x + 14, y + 38, w - 28, 14, theme::TEXT_HI);
}

void Notes::renderFeed(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Notes", theme::ACCENT_MAGENTA);

  // Status row: relay host + state pill
  const int16_t body_y = ui::kHeaderH + 12;
  const char* pill_txt = (state_ == S_LIVE)       ? "LIVE"
                       : (state_ == S_CONNECTING) ? "CONNECTING"
                       : (state_ == S_ERROR)      ? "ERROR"
                                                  : "OFFLINE";
  uint16_t pill_c = (state_ == S_LIVE)       ? theme::ACCENT_GREEN
                  : (state_ == S_CONNECTING) ? theme::ACCENT_YELLOW
                  : (state_ == S_ERROR)      ? theme::ACCENT_RED
                                             : theme::TEXT_MID;
  ui::status_pill(g, ui::kSideMargin, body_y, pill_txt, pill_c);

  theme::use_font(g, theme::FONT_SMALL);
  const int16_t rx = ui::kSideMargin + 90;
  ui::draw_top(g, relay_host_display_.c_str(), rx, body_y + 4, theme::TEXT_MID);

  // WiFi warning if offline
  if (WiFi.status() != WL_CONNECTED) {
    ui::card(g, ui::kSideMargin, body_y + 40,
             ctx.w - 2 * ui::kSideMargin, 80,
             theme::SURFACE, theme::ACCENT_YELLOW);
    theme::use_font(g, theme::FONT_HEADING_M);
    ui::draw_top(g, "no wifi", ui::kSideMargin + 16, body_y + 56,
                 theme::ACCENT_YELLOW);
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top(g, "open the Wi-Fi app to connect first",
                 ui::kSideMargin + 16, body_y + 90, theme::TEXT_MID);
    ui::screen_hint(g, ctx.w, ctx.h, "long-press = home");
    return;
  }

  // Empty state (connected but no events yet)
  if (event_count_ == 0) {
    theme::use_font(g, theme::FONT_SMALL);
    const char* msg = (state_ == S_LIVE) ? "waiting for notes…"
                    : (state_ == S_CONNECTING) ? "opening WebSocket…"
                    : "no notes yet";
    ui::draw_top_centered(g, msg, ctx.w / 2, ctx.h / 2 - 6, theme::TEXT_MID);
    ui::screen_hint(g, ctx.w, ctx.h, "long-press = home");
    return;
  }

  // Scrollable list of cards
  const int16_t list_top = body_y + 34;
  const int16_t card_h   = 96;
  const int16_t gap      = 8;
  const int16_t list_bot = ctx.h - ui::kHintH - 4;

  int16_t y = list_top;
  for (size_t i = (size_t)scroll_offset_; i < event_count_; ++i) {
    if (y + card_h > list_bot) break;
    drawNoteCard(ctx, ui::kSideMargin, y,
                 ctx.w - 2 * ui::kSideMargin, card_h,
                 events_[i], false);
    y += card_h + gap;
  }

  // Scroll hint
  char sc[24];
  snprintf(sc, sizeof(sc), "%d / %u  ·  swipe = scroll",
           scroll_offset_ + 1, (unsigned)event_count_);
  ui::screen_hint(g, ctx.w, ctx.h, sc);
}

void Notes::renderDetail(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Notes · detail", theme::ACCENT_MAGENTA);
  if (detail_idx_ < 0 || detail_idx_ >= (int)event_count_) return;
  const Note& n = events_[detail_idx_];

  const int16_t x = ui::kSideMargin;
  const int16_t w = ctx.w - 2 * ui::kSideMargin;
  const int16_t body_y = ui::kHeaderH + 12;

  // Full-width author card
  ui::card(g, x, body_y, w, 60, theme::SURFACE, 0, ui::kRadiusMd);
  g->fillRoundRect(x, body_y, 4, 60, 2, theme::ACCENT_MAGENTA);
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top(g, "author", x + 14, body_y + 10, theme::TEXT_LO);
  String sp = short_npub(n.pubkey_hex);
  theme::use_font(g, theme::FONT_MONO);
  ui::draw_top(g, sp.c_str(), x + 14, body_y + 30, theme::ACCENT_MAGENTA);

  // Content card
  const int16_t body_top = body_y + 76;
  const int16_t body_h   = ctx.h - body_top - ui::kHintH - 6;
  ui::card(g, x, body_top, w, body_h, theme::SURFACE, 0, ui::kRadiusMd);
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_paragraph(g, n.content, x + 14, body_top + 16, w - 28, 16,
                     theme::TEXT_HI);

  ui::screen_hint(g, ctx.w, ctx.h, "tap = back to feed  ·  long-press = home");
}

} // namespace mini
