#include "wifi_setup.h"
#include "../ui/kit.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

namespace mini {

// NVS
static constexpr const char* kNs   = "wifi";
static constexpr const char* kKSsid = "ssid";
static constexpr const char* kKPass = "pass";

// Portal singletons — lifetime bound to the app instance.
static WebServer* s_server = nullptr;
static DNSServer* s_dns    = nullptr;

// Pending credentials (set from HTTP handler, consumed by tick()).
static String s_pending_ssid;
static String s_pending_pass;
static volatile bool s_have_pending = false;

// ---- HTML ---------------------------------------------------------------

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>
<title>mini_setup</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;
     background:#0d1218;color:#f0f4fa;margin:0;padding:22px;max-width:520px;margin:0 auto;
     min-height:100vh}
h1{font-size:30px;font-weight:800;letter-spacing:-.03em;margin:0 0 6px;
   background:linear-gradient(135deg,#05CBFF,#37E0FF);-webkit-background-clip:text;
   -webkit-text-fill-color:transparent}
.sub{color:#8a95a8;font-size:14px;margin:0 0 24px;line-height:1.4}
form{background:#181c22;border:1px solid #2b323d;border-radius:18px;padding:20px;
     box-shadow:0 8px 24px rgba(0,0,0,.25)}
label{display:block;font-size:11px;font-weight:600;letter-spacing:.08em;
      color:#7a8598;margin:14px 0 6px;text-transform:uppercase}
input{width:100%;padding:14px 16px;border-radius:12px;border:1px solid #2b323d;
      background:#0e131b;color:#fff;font-size:16px;-webkit-appearance:none}
input:focus{outline:none;border-color:#05CBFF}
button{margin-top:22px;width:100%;padding:16px;border-radius:14px;border:0;
       background:#05CBFF;color:#000;font-weight:800;font-size:17px;
       letter-spacing:-.01em;cursor:pointer}
button:active{transform:scale(.98)}
.small{font-size:12px;color:#5a6478;margin-top:22px;text-align:center;line-height:1.5}
.tag{display:inline-block;padding:3px 8px;background:#181c22;border:1px solid #2b323d;
     border-radius:6px;font-size:11px;color:#8a95a8;font-family:monospace}
</style></head><body>
<h1>mini · setup</h1>
<p class=sub>Give your device Wi-Fi so it can pull live Bitcoin blocks
and stream a Nostr relay.</p>
<form method=POST action=/save>
  <label>Network name</label>
  <input name=ssid list=nets required autocomplete=off spellcheck=false
         placeholder='My Home Network'>
  <datalist id=nets>%NETS%</datalist>
  <label>Password</label>
  <input name=pass type=password autocomplete=off placeholder='(leave blank if open)'>
  <button>save + connect</button>
</form>
<p class=small>Device: <span class=tag>mini_setup</span> · portal at
  <span class=tag>192.168.4.1</span></p>
</body></html>)HTML";

static const char kSavedHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>
<title>saved</title>
<style>body{font-family:-apple-system,system-ui,sans-serif;background:#0d1218;
color:#f0f4fa;text-align:center;padding:60px 24px;margin:0}
.card{background:#181c22;border:1px solid #2b323d;border-radius:18px;padding:30px;
      max-width:440px;margin:0 auto;box-shadow:0 8px 24px rgba(0,0,0,.25)}
h1{color:#37E0;font-size:26px;margin:0 0 10px}
.big{font-size:44px;margin:0 0 16px}
p{color:#c2c9d4;line-height:1.5}
.tag{display:inline-block;padding:3px 8px;background:#0e131b;border:1px solid #2b323d;
     border-radius:6px;font-family:monospace;color:#37E0FF}
</style></head><body>
<div class=card>
<div class=big>✓</div>
<h1>saved</h1>
<p>Trying to join <span class=tag>%SSID%</span>…</p>
<p>You can close this page. The device will show its status on-screen.</p>
</div></body></html>)HTML";

static String scan_options_html() {
  int n = WiFi.scanNetworks();
  String out;
  for (int i = 0; i < n && i < 24; ++i) {
    out += "<option value=\"";
    String s = WiFi.SSID(i);
    s.replace("\"", "");
    out += s;
    out += "\">";
  }
  WiFi.scanDelete();
  return out;
}

static void handle_root() {
  String page = FPSTR(kIndexHtml);
  page.replace("%NETS%", scan_options_html());
  s_server->send(200, "text/html", page);
}

static void handle_save() {
  String ssid = s_server->arg("ssid");
  String pass = s_server->arg("pass");
  if (ssid.length() == 0) { s_server->send(400, "text/plain", "missing ssid"); return; }
  s_pending_ssid = ssid;
  s_pending_pass = pass;
  s_have_pending = true;
  Preferences p;
  if (p.begin(kNs, false)) {
    p.putString(kKSsid, ssid);
    p.putString(kKPass, pass);
    p.end();
  }
  String page = FPSTR(kSavedHtml);
  page.replace("%SSID%", ssid);
  s_server->send(200, "text/html", page);
}

static void handle_captive() {
  s_server->sendHeader("Location", "http://192.168.4.1/", true);
  s_server->send(302, "text/plain", "");
}

// ---- portal lifecycle -----------------------------------------------------

void WifiSetup::startPortal() {
  Serial.println("[wifi] startPortal: begin");
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.mode(WIFI_AP_STA);
  delay(150);

  IPAddress ap_ip(192, 168, 4, 1);
  IPAddress ap_gw(192, 168, 4, 1);
  IPAddress ap_mask(255, 255, 255, 0);
  WiFi.softAPConfig(ap_ip, ap_gw, ap_mask);
  bool ap_ok = WiFi.softAP("mini_setup", nullptr, /*channel=*/6, /*hidden=*/0,
                           /*max_conn=*/4);
  Serial.printf("[wifi] softAP=%d ip=%s\n", (int)ap_ok,
                WiFi.softAPIP().toString().c_str());
  delay(200);

  if (!s_dns) s_dns = new DNSServer();
  s_dns->setErrorReplyCode(DNSReplyCode::NoError);
  s_dns->start(53, "*", ap_ip);

  if (!s_server) s_server = new WebServer(80);
  s_server->on("/",                    handle_root);
  s_server->on("/save",  HTTP_POST,    handle_save);
  s_server->on("/generate_204",              handle_captive);
  s_server->on("/gen_204",                   handle_captive);
  s_server->on("/hotspot-detect.html",       handle_captive);
  s_server->on("/library/test/success.html", handle_captive);
  s_server->on("/ncsi.txt",                  handle_captive);
  s_server->on("/connecttest.txt",           handle_captive);
  s_server->on("/redirect",                  handle_captive);
  s_server->on("/success.txt",               handle_captive);
  s_server->onNotFound(                      handle_captive);
  s_server->begin();

  state_ = S_PORTAL_ACTIVE;
  Serial.println("[wifi] portal ready");
}

void WifiSetup::stopPortal() {
  if (s_server) s_server->stop();
  if (s_dns)    s_dns->stop();
  WiFi.softAPdisconnect(true);
  Serial.println("[wifi] portal stopped");
}

bool WifiSetup::tryConnectSaved() {
  Preferences p;
  if (!p.begin(kNs, /*ro=*/true)) return false;
  String ssid = p.getString(kKSsid, "");
  String pass = p.getString(kKPass, "");
  p.end();
  if (ssid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  Serial.printf("[wifi] auto-connecting to '%s'...\n", ssid.c_str());
  return true;
}

// ---- lifecycle + tick -----------------------------------------------------

void WifiSetup::onEnter(AppContext& ctx) {
  if (WiFi.status() == WL_CONNECTED) {
    state_ = S_CONNECTED;
    saved_ssid_ = WiFi.SSID();
    status_line_ = "connected";
  } else {
    Preferences p;
    if (p.begin(kNs, /*ro=*/true)) {
      saved_ssid_ = p.getString(kKSsid, "");
      p.end();
    }
    startPortal();
  }
  last_render_ms_ = 0;
  renderFull(ctx);
}

void WifiSetup::onExit(AppContext&) {
  if (state_ == S_PORTAL_ACTIVE || state_ == S_FAILED) {
    stopPortal();
    state_ = S_IDLE;
  }
}

void WifiSetup::tick(AppContext& ctx, uint32_t now_ms) {
  if (state_ == S_PORTAL_ACTIVE || state_ == S_CONNECTING) {
    if (s_dns) s_dns->processNextRequest();
    if (s_server) s_server->handleClient();
  }
  if (s_have_pending) {
    s_have_pending = false;
    WiFi.begin(s_pending_ssid.c_str(),
               s_pending_pass.length() ? s_pending_pass.c_str() : nullptr);
    connect_started_ms_ = now_ms;
    state_ = S_CONNECTING;
    saved_ssid_ = s_pending_ssid;
    status_line_ = "connecting…";
    renderFull(ctx);
  }
  if (state_ == S_CONNECTING) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      state_ = S_CONNECTED;
      status_line_ = String("IP ") + WiFi.localIP().toString();
      stopPortal();
      renderFull(ctx);
    } else if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL ||
               (now_ms - connect_started_ms_) > 15000) {
      state_ = S_FAILED;
      status_line_ = (s == WL_NO_SSID_AVAIL) ? "SSID not found"
                   : (s == WL_CONNECT_FAILED) ? "auth failed"
                   : "timed out";
      renderFull(ctx);
    }
  }
  // Live-refresh the status area on portal and connected pages
  if ((state_ == S_PORTAL_ACTIVE || state_ == S_CONNECTED) &&
      (now_ms - last_render_ms_) > 2000) {
    last_render_ms_ = now_ms;
    renderFull(ctx);
  }
}

// ---- rendering ------------------------------------------------------------

void WifiSetup::renderFull(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "WiFi", theme::ACCENT_CYAN);
  if (state_ == S_CONNECTED) renderConnected(ctx);
  else                       renderPortal(ctx);
}

void WifiSetup::renderPortal(AppContext& ctx) {
  auto* g = ctx.gfx;

  // Status pill top-right of body
  const int16_t body_y = ui::kHeaderH + 12;
  const uint16_t pill_c = (state_ == S_CONNECTING) ? theme::ACCENT_YELLOW
                       : (state_ == S_FAILED)     ? theme::ACCENT_RED
                                                  : theme::ACCENT_GREEN;
  const char* pill_txt = (state_ == S_CONNECTING) ? "CONNECTING"
                       : (state_ == S_FAILED)     ? "FAILED"
                                                  : "AP LIVE";
  ui::status_pill(g, ui::kSideMargin, body_y, pill_txt, pill_c);
  const int clients = WiFi.softAPgetStationNum();
  char cbuf[24]; snprintf(cbuf, sizeof(cbuf), "%d phone%s joined",
                          clients, clients == 1 ? "" : "s");
  ui::status_pill(g, ui::kSideMargin + 130, body_y, cbuf, theme::TEXT_MID);

  // Step-by-step instruction card
  const int16_t x = ui::kSideMargin;
  const int16_t w = ctx.w - 2 * ui::kSideMargin;
  const int16_t card_y = body_y + 46;
  const int16_t card_h = 246;
  ui::card(g, x, card_y, w, card_h, theme::SURFACE, 0);

  auto step = [&](int n, const char* title, const char* body, int16_t sy,
                  uint16_t accent) {
    // Circle numeral
    g->fillCircle(x + 26, sy + 8, 12, accent);
    theme::use_font(g, theme::FONT_HEADING_M);
    char nstr[3]; nstr[0] = '0' + n; nstr[1] = 0;
    int16_t nw = ui::text_w(g, nstr);
    ui::draw_top(g, nstr, x + 26 - nw / 2, sy - 4, theme::BG);
    // Title
    theme::use_font(g, theme::FONT_HEADING_M);
    ui::draw_top(g, title, x + 50, sy - 4, theme::TEXT_HI);
    // Body
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top(g, body, x + 50, sy + 22, theme::TEXT_MID);
  };
  step(1, "Join Wi-Fi 'mini_setup'",
       "on your phone; it's open/no password",
       card_y + 22, theme::ACCENT_CYAN);
  step(2, "Wait for pop-up",
       "iOS auto-opens the login page",
       card_y + 100, theme::ACCENT_CYAN);
  step(3, "Enter your home Wi-Fi",
       "SSID + password, tap save",
       card_y + 178, theme::ACCENT_CYAN);

  // Progress / status area
  const int16_t sy = card_y + card_h + 16;
  theme::use_font(g, theme::FONT_SMALL);
  if (state_ == S_CONNECTING) {
    ui::draw_top_centered(g, "connecting to your home network…",
                          ctx.w / 2, sy, theme::ACCENT_YELLOW);
  } else if (state_ == S_FAILED) {
    String err = String("failed: ") + status_line_;
    ui::draw_top_centered(g, err.c_str(), ctx.w / 2, sy, theme::ACCENT_RED);
  } else if (clients > 0) {
    ui::draw_top_centered(g, "phone connected — waiting for form submit",
                          ctx.w / 2, sy, theme::ACCENT_GREEN);
  }

  ui::screen_hint(g, ctx.w, ctx.h,
                  "long-press = home (portal stops)");
}

void WifiSetup::renderConnected(AppContext& ctx) {
  auto* g = ctx.gfx;

  const int16_t body_y = ui::kHeaderH + 12;
  ui::status_pill(g, ui::kSideMargin, body_y, "ONLINE", theme::ACCENT_GREEN);
  char rss[24]; snprintf(rss, sizeof(rss), "%d dBm", WiFi.RSSI());
  ui::status_pill(g, ui::kSideMargin + 100, body_y, rss, theme::TEXT_MID);

  // Big SSID + IP card
  const int16_t x = ui::kSideMargin;
  const int16_t w = ctx.w - 2 * ui::kSideMargin;
  const int16_t card_y = body_y + 46;
  const int16_t card_h = 180;
  ui::card(g, x, card_y, w, card_h, theme::SURFACE, 0);

  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top(g, "network", x + 20, card_y + 20, theme::TEXT_LO);
  theme::use_font(g, theme::FONT_HEADING_L);
  String ssid = WiFi.SSID();
  if (ssid.length() == 0) ssid = saved_ssid_;
  ui::draw_top(g, ssid.c_str(), x + 20, card_y + 40, theme::TEXT_HI);

  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top(g, "ip", x + 20, card_y + 100, theme::TEXT_LO);
  theme::use_font(g, theme::FONT_MONO);
  ui::draw_top(g, WiFi.localIP().toString().c_str(),
               x + 20, card_y + 120, theme::TEXT_HI);

  // Forget-wifi pill
  const int16_t bx = ui::kSideMargin;
  const int16_t bw = ctx.w - 2 * ui::kSideMargin;
  const int16_t bh = 56;
  const int16_t by = ctx.h - bh - 44;
  ui::PillButton fb{ "forget", "clear saved wifi", theme::ACCENT_RED, false };
  ui::pill_button(g, bx, by, bw, bh, fb);

  ui::screen_hint(g, ctx.w, ctx.h, "tap 'forget' to re-setup  ·  long-press = home");
}

bool WifiSetup::onGesture(AppContext& ctx, const Gesture& g) {
  switch (g.type) {
    case Gesture::TAP:
      if (state_ == S_CONNECTED) {
        // Bottom pill = forget
        const int16_t bh = 56;
        const int16_t by = ctx.h - bh - 44;
        if (g.y >= by && g.y < by + bh) {
          Preferences p;
          if (p.begin(kNs, false)) {
            p.remove(kKSsid);
            p.remove(kKPass);
            p.end();
          }
          WiFi.disconnect(true);
          state_ = S_IDLE;
          saved_ssid_ = "";
          startPortal();
          renderFull(ctx);
        }
      }
      return true;
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

} // namespace mini
