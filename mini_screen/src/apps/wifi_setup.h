// WiFi Setup — one-tap-provisioning via a temporary AP + captive portal.
//
// Flow:
//   1. On entry, device starts a soft-AP named "mini_setup" (open, no
//      password so iOS auto-detects it as a captive portal).
//   2. A DNS server on the AP intercepts every A-record lookup and points
//      it at 192.168.4.1, so any URL a phone tries to load hits our
//      HTTP server.
//   3. HTTP server serves a small styled page: SSID scan list + password
//      input + Save button. On save, we store creds to NVS and try to
//      connect (STA + AP simultaneously so the phone stays connected).
//   4. Status page reports success/failure. On success, device is now
//      online; user can go home and open Explorer to see live blocks.
//
// If already connected, entering this app shows the current IP/SSID with
// a "forget" button.
#pragma once
#include "../shell.h"

namespace mini {

class WifiSetup : public App {
 public:
  const char* name() const override { return "WiFi"; }
  AppKind     kind() const override { return AppKind::WIFI; }
  uint16_t    tint() const override { return 0x05FF; }   // cyan
  // Utility app — reachable via a gear tap on Home or serial 'w', but not
  // shown as a tile in the main app grid.
  bool        showInHome() const override { return false; }

  void onEnter(AppContext&) override;
  void onExit(AppContext&) override;
  void tick(AppContext&, uint32_t now_ms) override;
  bool onGesture(AppContext&, const Gesture&) override;

  // Try to connect to previously-saved WiFi (called at boot from the sketch).
  // Returns true if a saved SSID was found and a connect attempt is in
  // progress. The connection itself is non-blocking; check WiFi.status()
  // later.
  static bool tryConnectSaved();

 private:
  enum State : uint8_t {
    S_IDLE = 0,          // no portal running
    S_PORTAL_ACTIVE,     // AP + DNS + HTTP running
    S_CONNECTING,        // credentials submitted, connecting
    S_CONNECTED,         // STA connected
    S_FAILED,            // last connect attempt failed
  };
  State state_ = S_IDLE;
  uint32_t last_render_ms_ = 0;
  uint32_t connect_started_ms_ = 0;
  String   saved_ssid_;
  String   status_line_;

  void startPortal();
  void stopPortal();
  void renderFull(AppContext&);
  void renderPortal(AppContext&);
  void renderConnected(AppContext&);
};

} // namespace mini
