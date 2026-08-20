// LVGL watch-style UI for the USB signer. All rendering, touch handling and
// on-screen account management lives here; the sketch pushes state in and
// receives decisions through callbacks.
#pragma once
#include <Arduino.h>

class Arduino_GFX;
class Arduino_IIC;

struct UiCallbacks {
  void (*decide)(bool approve);     // resolve the currently shown request
  void (*decidePsbt)(bool approve); // resolve the currently shown Bitcoin tx
  void (*importSD)();
  void (*exportSD)();
  void (*signPsbtSD)();             // load /unsigned.psbt from the SD card
  void (*setAutoAuth)(bool on);
};

namespace ui {

void init(Arduino_GFX* gfx, Arduino_IIC* touch, const UiCallbacks& cb, bool auto_auth);
void tick();  // call every loop iteration

// --- app -> ui -------------------------------------------------------------
void setLink(bool up);
void showRequest(uint32_t id, int kind, const String& content, int queued_behind);
// Bitcoin transaction review: `amount` is the headline (e.g. "-21 000 sats"),
// `body` the multi-line breakdown (outputs, fee, OP_RETURN). Same
// approve/decline interaction as sign requests, resolved via decidePsbt.
void showPsbt(const String& amount, const String& body);
bool psbtScreenActive();
void clearRequest();  // pending request went away (expired / resolved remotely)
void showSigningOverlay();  // synchronous "Signing..." before the blocking sign
void showResult(bool approved, int remaining);
// Full-screen QR of a signed payload for the phone camera. Payloads that
// don't fit one code are cycled as animated "NTX:<i>:<n>:<chunk>" frames
// (the NostrTX scanner reassembles them). Done button returns home.
void showTxQr(const String& payload, const String& subtitle);
void toast(const String& msg, uint32_t rgb);
void refreshAccounts();  // keystore or profiles changed outside the UI
void refreshFollows();   // bridge pushed a new follows list

// --- physical side button ----------------------------------------------------
void buttonShort();
void buttonLong();

void toggleByteSwap();  // serial 'W': flip RGB565 byte order if colors look off

}  // namespace ui
