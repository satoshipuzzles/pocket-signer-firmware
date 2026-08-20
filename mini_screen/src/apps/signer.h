// Signer app — first cut of the "Nostrochain-on-device" concept.
//
// This first cut delivers the *identity* half of the vision:
//   * On first launch (or after a reset), generate a real secp256k1 keypair
//     using ESP32-S3 hardware entropy.
//   * Store the keypair in NVS. Survives reboots.
//   * Display the BIP-340 x-only public key as both a bech32 npub string and
//     a scannable QR code.
//   * Provide a "danger zone" to reveal the nsec (with a swipe-in confirm)
//     or wipe keys entirely (with a two-tap confirm).
//
// The signing half (BIP-340 schnorr for Nostr events, ECDSA + PSBT for
// Bitcoin) will layer on top of these keys in a follow-up. All roads lead
// through this app's stored keypair.
#pragma once
#include "../shell.h"
#include "../crypto/nostr_keys.h"

namespace mini {

class Signer : public App {
 public:
  const char* name() const override { return "Signer"; }
  AppKind     kind() const override { return AppKind::SIGNER; }
  uint16_t    tint() const override { return 0x05FF; }   // cyan

  void onEnter(AppContext&) override;
  void tick(AppContext&, uint32_t) override {}
  bool onGesture(AppContext&, const Gesture&) override;

 private:
  enum Page : uint8_t {
    PAGE_INTRO   = 0,   // no keys yet — tap to generate
    PAGE_MAIN    = 1,   // npub summary + hints
    PAGE_QR      = 2,   // scannable QR of npub
    PAGE_SD      = 3,   // SD card export/import
    PAGE_DANGER  = 4,   // reveal nsec / reset keys
  };

  nostrocrypto::NostrKeys keys_{};
  Page page_ = PAGE_INTRO;
  uint32_t reset_confirm_deadline_ms_ = 0;
  bool     nsec_revealed_ = false;
  // Transient SD status message: "exported ok", "imported ok", "no card", etc.
  String   sd_status_;
  // Tap-zone y coordinates cached during render, consumed in onGesture.
  int16_t  sd_export_y0_ = 0, sd_export_y1_ = 0;
  int16_t  sd_import_y0_ = 0, sd_import_y1_ = 0;
  int16_t  reset_btn_y0_ = 0, reset_btn_y1_ = 0;

  void render(AppContext&);
  void renderIntro(AppContext&);
  void renderMain(AppContext&);
  void renderQR(AppContext&);
  void renderSD(AppContext&);
  void renderDanger(AppContext&);
  void generateKeys(AppContext&);
  void exportToSD(AppContext&);
  void importFromSD(AppContext&);
};

} // namespace mini
