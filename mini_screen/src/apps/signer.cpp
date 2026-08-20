#include "signer.h"
#include "../ui/qrcode_lib.h"
#include "../ui/kit.h"
#include "../crypto/bech32.h"

#include <FS.h>
#include <SD_MMC.h>

namespace mini {

static inline void crumb(const char* s) {
  Serial.print("[signer] "); Serial.println(s); Serial.flush();
}

// ---- lifecycle ------------------------------------------------------------

void Signer::onEnter(AppContext& ctx) {
  Serial.printf("[signer] enter: heap=%lu stack=%lu\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL));
  bool have = nostrocrypto::load(keys_);
  page_ = have ? PAGE_MAIN : PAGE_INTRO;
  nsec_revealed_ = false;
  reset_confirm_deadline_ms_ = 0;
  sd_status_ = "";
  render(ctx);
}

void Signer::generateKeys(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Signer", theme::ACCENT_CYAN);

  // Big loading state
  theme::use_font(g, theme::FONT_HEADING_L);
  ui::draw_top_centered(g, "generating…", ctx.w / 2, ctx.h / 2 - 40,
                        theme::ACCENT_CYAN);
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top_centered(g, "hardware entropy · secp256k1",
                        ctx.w / 2, ctx.h / 2 + 6, theme::TEXT_MID);

  bool ok = nostrocrypto::generate_and_store(keys_);
  if (ok) {
    Serial.print("[signer] npub = ");
    Serial.println(nostrocrypto::to_npub(keys_));
    Serial.flush();
    page_ = PAGE_MAIN;
  } else {
    Serial.println("[signer] keygen failed");
    page_ = PAGE_INTRO;
    delay(600);
  }
}

// ---- rendering ------------------------------------------------------------

void Signer::render(AppContext& ctx) {
  switch (page_) {
    case PAGE_INTRO:  renderIntro(ctx);  break;
    case PAGE_MAIN:   renderMain(ctx);   break;
    case PAGE_QR:     renderQR(ctx);     break;
    case PAGE_SD:     renderSD(ctx);     break;
    case PAGE_DANGER: renderDanger(ctx); break;
  }
}

void Signer::renderIntro(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Signer", theme::ACCENT_CYAN);

  const int16_t icon_size = 132;
  const int16_t icon_x = (ctx.w - icon_size) / 2;
  const int16_t icon_y = ui::kHeaderH + 40;
  theme::icon_key(g, icon_x, icon_y, icon_size, theme::ACCENT_CYAN);

  theme::use_font(g, theme::FONT_HEADING_L);
  ui::draw_top_centered(g, "no keys yet", ctx.w / 2, icon_y + icon_size + 26,
                        theme::TEXT_HI);
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top_centered(g, "tap anywhere to generate a fresh identity",
                        ctx.w / 2, icon_y + icon_size + 66, theme::TEXT_MID);

  ui::screen_hint(g, ctx.w, ctx.h, "long-press = home");
}

// Layout: NPUB card (multi-line bech32) up top, then three pill buttons.
void Signer::renderMain(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Signer", theme::ACCENT_CYAN);

  // NPUB card
  const int16_t card_x = ui::kSideMargin;
  const int16_t card_y = ui::kHeaderH + 12;
  const int16_t card_w = ctx.w - 2 * ui::kSideMargin;
  const int16_t card_h = 168;
  ui::card(g, card_x, card_y, card_w, card_h, theme::SURFACE, 0);

  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top(g, "public key (npub)", card_x + 16, card_y + 14, theme::TEXT_LO);

  String np = nostrocrypto::to_npub(keys_);
  theme::use_font(g, theme::FONT_MONO);
  ui::draw_wrapped_token(g, np, card_x + 16, card_y + 40,
                         card_w - 32, /*line_h=*/20, theme::TEXT_HI);

  // Three pill buttons in a row
  const int16_t btn_top = card_y + card_h + 20;
  const int16_t btn_h   = 84;
  const int16_t gap     = 10;
  const int16_t btn_w   = (ctx.w - 2 * ui::kSideMargin - 2 * gap) / 3;

  ui::PillButton bs[3] = {
    { "QR",      "share",  theme::ACCENT_CYAN,    false },
    { "SD",      "backup", theme::ACCENT_GREEN,   false },
    { "reveal",  "danger", theme::ACCENT_RED,     false },
  };
  for (int i = 0; i < 3; ++i) {
    ui::pill_button(g,
                    ui::kSideMargin + i * (btn_w + gap),
                    btn_top, btn_w, btn_h, bs[i]);
  }

  ui::screen_hint(g, ctx.w, ctx.h, "tap a pill  ·  long-press = home");
}

void Signer::renderQR(AppContext& ctx) {
  auto* g = ctx.gfx;
  g->fillScreen(0xFFFF);

  String np = nostrocrypto::to_npub(keys_);
  QRCode qr;
  const uint8_t QR_VERSION = 4;
  const uint8_t QR_SIZE    = 4 * QR_VERSION + 17;
  uint8_t qr_data[qrcode_getBufferSize(QR_VERSION)];
  int rc = qrcode_initText(&qr, qr_data, QR_VERSION, ECC_LOW, np.c_str());
  if (rc != 0) {
    g->fillScreen(theme::BG);
    theme::use_font(g, theme::FONT_HEADING_L);
    ui::draw_top_centered(g, "QR error", ctx.w / 2, ctx.h / 2, theme::ACCENT_RED);
    return;
  }

  const int reserved_top = 44, reserved_bot = 44;
  const int avail = min((int)ctx.w - 40, (int)ctx.h - reserved_top - reserved_bot);
  const int module_px = max(1, avail / QR_SIZE);
  const int qr_px = module_px * QR_SIZE;
  const int qx = (ctx.w - qr_px) / 2;
  const int qy = reserved_top + (ctx.h - reserved_top - reserved_bot - qr_px) / 2;

  for (int y = 0; y < QR_SIZE; ++y) {
    for (int x = 0; x < QR_SIZE; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        g->fillRect(qx + x * module_px, qy + y * module_px,
                    module_px, module_px, 0x0000);
      }
    }
  }
  theme::use_font(g, theme::FONT_HEADING_M);
  ui::draw_top_centered(g, "scan me", ctx.w / 2, 16, 0x0000);
  theme::use_font(g, theme::FONT_SMALL);
  ui::draw_top_centered(g, "tap anywhere to go back", ctx.w / 2, ctx.h - 24, 0x0000);
}

void Signer::renderSD(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Signer · SD", theme::ACCENT_GREEN);

  // Status pill top
  const uint16_t ok_c = ctx.sd_ok ? theme::ACCENT_GREEN : theme::ACCENT_RED;
  ui::status_pill(g, ui::kSideMargin, ui::kHeaderH + 12,
                  ctx.sd_ok ? "SD OK" : "SD MISSING", ok_c);

  // Two cards stacked
  const int16_t x = ui::kSideMargin;
  const int16_t w = ctx.w - 2 * ui::kSideMargin;
  const int16_t card_h = 110;
  const int16_t y_export = ui::kHeaderH + 48;
  const int16_t y_import = y_export + card_h + 14;

  auto action_card = [&](int16_t cy, const char* title, const char* body,
                         uint16_t accent) {
    ui::card(g, x, cy, w, card_h, theme::SURFACE, accent, ui::kRadiusLg);
    theme::use_font(g, theme::FONT_HEADING_L);
    ui::draw_top(g, title, x + 18, cy + 18, accent);
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top(g, body, x + 18, cy + 62, theme::TEXT_MID);
    // Right-side chevron hint
    theme::use_font(g, theme::FONT_HEADING_M);
    ui::draw_top(g, ">", x + w - 30, cy + 34, accent);
  };
  action_card(y_export, "EXPORT",
              "writes /nostr/npub.txt + nsec.txt",
              theme::ACCENT_GREEN);
  action_card(y_import, "IMPORT",
              "reads /nostr/nsec.txt (bech32 or hex)",
              theme::ACCENT_YELLOW);

  // Status message
  if (sd_status_.length()) {
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top_centered(g, sd_status_.c_str(), ctx.w / 2,
                          y_import + card_h + 14, theme::TEXT_HI);
  }

  ui::screen_hint(g, ctx.w, ctx.h,
                  "tap a card  ·  long-press = home");

  // Remember zone bounds for onGesture. (Y ranges computed above.)
  sd_export_y0_ = y_export;
  sd_export_y1_ = y_export + card_h;
  sd_import_y0_ = y_import;
  sd_import_y1_ = y_import + card_h;
}

void Signer::renderDanger(AppContext& ctx) {
  auto* g = ctx.gfx;
  ui::begin_screen(g, ctx.w, "Signer · danger", theme::ACCENT_RED);

  ui::status_pill(g, ui::kSideMargin, ui::kHeaderH + 12,
                  "KEEP OFFLINE", theme::ACCENT_RED);

  if (nsec_revealed_) {
    theme::use_font(g, theme::FONT_HEADING_M);
    ui::draw_top_centered(g, "secret key (nsec)", ctx.w / 2, ui::kHeaderH + 50,
                          theme::ACCENT_YELLOW);
    String ns = nostrocrypto::to_nsec(keys_);
    theme::use_font(g, theme::FONT_MONO);
    ui::draw_wrapped_token(g, ns, ui::kSideMargin, ui::kHeaderH + 84,
                           ctx.w - 2 * ui::kSideMargin, 20,
                           theme::ACCENT_YELLOW);
  } else {
    const int16_t cy = ui::kHeaderH + 90;
    ui::card(g, ui::kSideMargin, cy, ctx.w - 2 * ui::kSideMargin, 110,
             theme::SURFACE, theme::ACCENT_RED);
    theme::use_font(g, theme::FONT_HEADING_L);
    ui::draw_top_centered(g, "tap to reveal nsec", ctx.w / 2, cy + 20,
                          theme::TEXT_HI);
    theme::use_font(g, theme::FONT_SMALL);
    ui::draw_top_centered(g, "your private key — never share",
                          ctx.w / 2, cy + 60, theme::TEXT_MID);
  }

  // Reset button pinned near bottom
  const int16_t rx = ui::kSideMargin;
  const int16_t rw = ctx.w - 2 * ui::kSideMargin;
  const int16_t rh = 56;
  const int16_t ry = ctx.h - rh - 44;
  ui::PillButton reset_btn{
    reset_confirm_deadline_ms_ ? "TAP AGAIN TO WIPE" : "reset key",
    reset_confirm_deadline_ms_ ? "irreversible" : "wipe on-device identity",
    theme::ACCENT_RED,
    reset_confirm_deadline_ms_ != 0,
  };
  ui::pill_button(g, rx, ry, rw, rh, reset_btn);
  reset_btn_y0_ = ry;
  reset_btn_y1_ = ry + rh;

  ui::screen_hint(g, ctx.w, ctx.h, "tap = reveal  ·  long-press = home");
}

// ---- SD I/O ---------------------------------------------------------------

void Signer::exportToSD(AppContext& ctx) {
  if (!ctx.sd_ok) { sd_status_ = "no SD card inserted"; return; }
  fs::FS& fs = SD_MMC;
  if (!fs.exists("/nostr")) fs.mkdir("/nostr");
  String np = nostrocrypto::to_npub(keys_);
  String ns = nostrocrypto::to_nsec(keys_);
  auto write_line = [&](const char* path, const String& body) -> bool {
    File f = fs.open(path, FILE_WRITE);
    if (!f) return false;
    size_t n = f.print(body);
    f.print("\n");
    f.close();
    return n == body.length();
  };
  bool ok = write_line("/nostr/npub.txt", np) &&
            write_line("/nostr/nsec.txt", ns);
  sd_status_ = ok ? "exported to /nostr/" : "export failed";
  Serial.printf("[signer] SD export ok=%d\n", (int)ok);
}

void Signer::importFromSD(AppContext& ctx) {
  if (!ctx.sd_ok) { sd_status_ = "no SD card inserted"; return; }
  fs::FS& fs = SD_MMC;
  if (!fs.exists("/nostr/nsec.txt")) {
    sd_status_ = "no /nostr/nsec.txt on card";
    return;
  }
  File f = fs.open("/nostr/nsec.txt", FILE_READ);
  if (!f) { sd_status_ = "cannot open file"; return; }
  String raw;
  while (f.available()) raw += (char)f.read();
  f.close();

  uint8_t priv[32];
  char hrp[16] = {0};
  int rc = nostrocrypto::parse_private_key(raw, priv, hrp);
  if (rc != 0) {
    sd_status_ = (rc == -1) ? "file too short"
                : (rc == -2) ? "bad bech32"
                : "bad hex";
    return;
  }
  if (strcmp(hrp, "npub") == 0) { sd_status_ = "that's an npub, not nsec"; return; }
  if (!nostrocrypto::import_and_store(priv, keys_)) { sd_status_ = "invalid key"; return; }
  sd_status_ = "imported! new npub set";
  Serial.print("[signer] imported new npub = "); Serial.println(nostrocrypto::to_npub(keys_));
}

// ---- gestures -------------------------------------------------------------

bool Signer::onGesture(AppContext& ctx, const Gesture& g) {
  if (page_ == PAGE_INTRO) {
    if (g.type == Gesture::TAP)        { generateKeys(ctx); render(ctx); return true; }
    if (g.type == Gesture::LONG_PRESS) return false;
    return true;
  }

  switch (g.type) {
    case Gesture::TAP: {
      // MAIN page: three pill buttons at bottom
      if (page_ == PAGE_MAIN) {
        const int16_t btn_top = ui::kHeaderH + 12 + 168 + 20;
        const int16_t btn_h   = 84;
        const int16_t gap     = 10;
        const int16_t btn_w   = (ctx.w - 2 * ui::kSideMargin - 2 * gap) / 3;
        if (g.y >= btn_top && g.y < btn_top + btn_h) {
          for (int i = 0; i < 3; ++i) {
            const int16_t bx = ui::kSideMargin + i * (btn_w + gap);
            if (g.x >= bx && g.x < bx + btn_w) {
              if      (i == 0) page_ = PAGE_QR;
              else if (i == 1) page_ = PAGE_SD;
              else             page_ = PAGE_DANGER;
              nsec_revealed_ = false;
              reset_confirm_deadline_ms_ = 0;
              sd_status_ = "";
              render(ctx);
              return true;
            }
          }
        }
      }
      // QR page: any tap goes back to MAIN
      if (page_ == PAGE_QR) {
        page_ = PAGE_MAIN;
        render(ctx);
        return true;
      }
      // SD page: two cards
      if (page_ == PAGE_SD) {
        Serial.printf("[signer] SD tap y=%d exp[%d..%d] imp[%d..%d]\n",
                      (int)g.y, sd_export_y0_, sd_export_y1_,
                      sd_import_y0_, sd_import_y1_);
        if (g.y >= sd_export_y0_ && g.y < sd_export_y1_) {
          exportToSD(ctx);
          render(ctx);
        } else if (g.y >= sd_import_y0_ && g.y < sd_import_y1_) {
          importFromSD(ctx);
          render(ctx);
        }
        return true;
      }
      // DANGER page: tap reveals nsec, or taps reset button (two-tap confirm)
      if (page_ == PAGE_DANGER) {
        if (g.y >= reset_btn_y0_ && g.y < reset_btn_y1_) {
          const uint32_t now = millis();
          if (reset_confirm_deadline_ms_ && now < reset_confirm_deadline_ms_) {
            // Second tap within 5s — actually wipe.
            Preferences p;
            if (p.begin("nostr", false)) { p.clear(); p.end(); }
            memset(&keys_, 0, sizeof(keys_));
            page_ = PAGE_INTRO;
            reset_confirm_deadline_ms_ = 0;
            sd_status_ = "";
            Serial.println("[signer] keys WIPED");
            render(ctx);
          } else {
            reset_confirm_deadline_ms_ = now + 5000;
            render(ctx);
          }
        } else if (!nsec_revealed_) {
          nsec_revealed_ = true;
          render(ctx);
        }
        return true;
      }
      return true;
    }
    case Gesture::SWIPE_LEFT: {
      // Forward: MAIN → QR → SD → DANGER → MAIN
      static const Page fwd[] = { PAGE_QR, PAGE_SD, PAGE_DANGER, PAGE_MAIN };
      static const Page src[] = { PAGE_MAIN, PAGE_QR, PAGE_SD, PAGE_DANGER };
      for (size_t i = 0; i < 4; ++i) if (page_ == src[i]) { page_ = fwd[i]; break; }
      nsec_revealed_ = false;
      reset_confirm_deadline_ms_ = 0;
      sd_status_ = "";
      render(ctx);
      return true;
    }
    case Gesture::SWIPE_RIGHT: {
      static const Page bwd[] = { PAGE_DANGER, PAGE_SD, PAGE_QR, PAGE_MAIN };
      static const Page src[] = { PAGE_MAIN, PAGE_DANGER, PAGE_SD, PAGE_QR };
      for (size_t i = 0; i < 4; ++i) if (page_ == src[i]) { page_ = bwd[i]; break; }
      nsec_revealed_ = false;
      reset_confirm_deadline_ms_ = 0;
      sd_status_ = "";
      render(ctx);
      return true;
    }
    case Gesture::LONG_PRESS:
      return false;
    default:
      return true;
  }
}

} // namespace mini
