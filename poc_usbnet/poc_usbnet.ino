// poc_usbnet — USB-C plug-in Nostr signer.
//
// Transport: enumerates as a USB CDC-ECM network adapter (native on iOS,
// macOS, Linux, Android). Host gets 10.77.7.x over the cable and reaches a
// NIP-07-shaped HTTP API at http://10.77.7.1:
//
//   GET  /               demo page (stand-in for the browser extension)
//   GET  /api/status     device + queue status
//   GET  /api/pubkey     active account pubkey
//   POST /api/sign       queue a nostr event -> approve on device screen.
//                        kind-22242 relay auth auto-signs (on-device toggle).
//   GET  /api/result?id  poll: pending | declined | approved + REAL BIP-340
//                        signature, self-verified before it leaves the device
//   GET  /api/profile?pubkey    cached kind-0 name / picture presence
//   POST /api/profile    extension pushes kind-0 name + 96x96 PNG avatar
//                        (device is airgapped; the extension is its eyes)
//
// UI: LVGL 9, watchOS-style (see ui.cpp). Physical BOOT side button:
// click = move / next, hold = confirm / back.
//
// Build (differs from mini_screen: USB-OTG mode!):
//   FQBN esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,
//        PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,FlashSize=16M
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <USB.h>
#include <SD_MMC.h>
#include <mbedtls/base64.h>

#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <Adafruit_XCA9554.h>

#include "pin_config.h"
#include "usb_net.h"
#include "bip340.h"
#include "keystore.h"
#include "bech32.h"
#include "sign_queue.h"
#include "profile.h"
#include "taproot.h"
#include "psbt.h"
#include "ui.h"
#include "bridge_page.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#define BTN_PIN 0  // BOOT side button

// ---------------------------------------------------------------------------
// Display + touch (same V2 board bring-up as mini_screen)
// ---------------------------------------------------------------------------
Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300* gfx = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);

Adafruit_XCA9554 expander;
static bool touch_ok = false;

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> CST(new Arduino_CST816x(
    IIC_Bus, CST816T_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE,
    TP_INT, Arduino_IIC_Touch_Interrupt));
void IRAM_ATTR Arduino_IIC_Touch_Interrupt() { CST->IIC_Interrupt_Flag = true; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static SignRequest g_queue[QLEN];
static uint32_t g_next_id = 1;
static uint32_t g_http_hits = 0;
static bool g_auto_auth = true;
static uint32_t g_shown_req_id = 0;

static WebServer server(80);

// One Bitcoin transaction under review at a time (unlike Nostr events these
// are rare, high-stakes, and human-paced).
enum PsbtState : uint8_t { PS_NONE, PS_PENDING, PS_APPROVED, PS_DECLINED };
static psbt::Tx g_ptx;
static PsbtState g_pstate = PS_NONE;
static String g_ptx_hex;         // final network tx after approval (single-sig)
static String g_ptx_b64;         // partially-signed PSBT (multisig)
static bool g_ptx_from_sd = false;
static bool g_ptx_shown = false;
static uint32_t g_ptx_born = 0;

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------
static SignRequest* findReq(uint32_t id) {
  for (int i = 0; i < QLEN; ++i)
    if (g_queue[i].status != REQ_NONE && g_queue[i].id == id) return &g_queue[i];
  return nullptr;
}

static SignRequest* allocReq() {
  for (int i = 0; i < QLEN; ++i)
    if (g_queue[i].status == REQ_NONE) return &g_queue[i];
  for (int i = 0; i < QLEN; ++i) {
    SignRequest& r = g_queue[i];
    if ((r.status == REQ_APPROVED || r.status == REQ_DECLINED) &&
        (r.polled || millis() - r.done_ms > 15000)) return &r;
  }
  return nullptr;
}

static SignRequest* currentPending() {
  SignRequest* best = nullptr;
  for (int i = 0; i < QLEN; ++i) {
    if (g_queue[i].status != REQ_PENDING) continue;
    if (!best || g_queue[i].id < best->id) best = &g_queue[i];
  }
  return best;
}

static int pendingCount() {
  int n = 0;
  for (int i = 0; i < QLEN; ++i)
    if (g_queue[i].status == REQ_PENDING) n++;
  return n;
}

// ---------------------------------------------------------------------------
// NIP-01 event id + BIP-340 signing
// ---------------------------------------------------------------------------
static String hexOf(const uint8_t* b, size_t n) {
  static const char* h = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s += h[b[i] >> 4]; s += h[b[i] & 0xF]; }
  return s;
}

static bool signRequest(SignRequest& req) {
  uint8_t priv[32];
  if (!keystore::activePriv(priv)) return false;

  JsonDocument src;
  if (deserializeJson(src, req.raw)) { memset(priv, 0, 32); return false; }

  const String pub_hex = keystore::activePubHex();
  uint32_t created_at = src["created_at"] | 0;
  if (created_at == 0) created_at = 1755200000;  // no RTC: extension sets it

  JsonDocument arr;
  JsonArray a = arr.to<JsonArray>();
  a.add(0);
  a.add(pub_hex);
  a.add(created_at);
  a.add(req.kind);
  if (src["tags"].is<JsonArray>()) a.add(src["tags"]);
  else a.add(JsonArray());
  a.add(src["content"] | "");

  String ser;
  serializeJson(arr, ser);

  uint8_t id[32], sig[64];
  bip340::sha256((const uint8_t*)ser.c_str(), ser.length(), id);

  const uint32_t t0 = millis();
  const bool ok = bip340::sign(priv, id, sig);
  memset(priv, 0, 32);
  Serial.printf("[poc] BIP-340 sign #%lu %s in %lums\n", (unsigned long)req.id,
                ok ? "OK" : "FAILED", (unsigned long)(millis() - t0));
  if (!ok) return false;

  req.event_id_hex = hexOf(id, 32);
  req.sig_hex = hexOf(sig, 64);
  req.pubkey_hex = pub_hex;
  req.created_at = created_at;
  return true;
}

// Resolve the currently shown request (from touch or physical button).
static void onDecide(bool approve) {
  SignRequest* cur = currentPending();
  if (!cur) return;
  if (approve) {
    ui::showSigningOverlay();
    if (signRequest(*cur)) {
      cur->status = REQ_APPROVED;
    } else {
      cur->status = REQ_DECLINED;  // fail closed
      Serial.println("[poc] signing failed — declining request");
    }
  } else {
    cur->status = REQ_DECLINED;
  }
  cur->done_ms = millis();
  Serial.printf("[poc] request #%lu %s on-device\n", (unsigned long)cur->id,
                cur->status == REQ_APPROVED ? "APPROVED" : "DECLINED");
  g_shown_req_id = 0;
  ui::showResult(cur->status == REQ_APPROVED, pendingCount());
}

// ---------------------------------------------------------------------------
// Bitcoin PSBT review + signing
// ---------------------------------------------------------------------------
static String fmtSats(uint64_t v) {
  String d = String((unsigned long long)v);
  String out;
  int c = 0;
  for (int i = d.length() - 1; i >= 0; --i) {
    out = String(d[i]) + out;
    if (++c % 3 == 0 && i > 0) out = "," + out;
  }
  return out + " sats";
}

static void psbtPrompt() {
  uint8_t pub[32], prog[32];
  bool have_prog = false;
  if (keystore::count() > 0) {
    keystore::pubOf(keystore::activeIndex(), pub);
    have_prog = taproot::tweakedXOnly(pub, prog);
  }

  uint64_t sent = 0;
  bool any_ext = false;
  String body;
  for (size_t i = 0; i < g_ptx.outs.size(); ++i) {
    if (g_ptx.isOpReturn(i)) {
      body += "OP_RETURN\n\"" + g_ptx.opReturnText(i) + "\"\n\n";
      continue;
    }
    const std::vector<uint8_t>& sc = g_ptx.outs[i].script;
    const bool ours = have_prog && sc.size() == 34 && sc[0] == 0x51 &&
                      sc[1] == 0x20 && memcmp(sc.data() + 2, prog, 32) == 0;
    String a = g_ptx.outAddress(i);
    if (a.length() > 26) a = a.substring(0, 14) + "..." + a.substring(a.length() - 6);
    if (ours) {
      body += "Change back to you\n";
    } else {
      body += "To\n";
      sent += g_ptx.outs[i].amount;
      any_ext = true;
    }
    body += a + "\n" + fmtSats(g_ptx.outs[i].amount) + "\n\n";
  }
  body += "Fee  " + fmtSats(g_ptx.fee());
  body += "\nSpending " + String(g_ptx.ins.size()) +
          (g_ptx.ins.size() == 1 ? " coin  (" : " coins  (") +
          fmtSats(g_ptx.totalIn()) + ")";
  if (g_ptx.isMultisig())
    body += "\n" + g_ptx.multisigDesc() + " — adds your signature";
  body += g_ptx_from_sd ? "\nSource: SD card" : "\nSource: USB link";

  ui::showPsbt(any_ext ? ("-" + fmtSats(sent)) : "Self transfer", body);
  g_ptx_shown = true;
}

static void writeSignedToSD() {
  if (!sdBegin()) {
    ui::toast("Signed, but no SD to write", 0x3B2A0A);
    return;
  }
  bool ok = false;
  if (g_ptx.isMultisig()) {
    // binary PSBT for tools + base64 text for easy pasting into NostrTX
    std::vector<uint8_t> bin;
    if (g_ptx.partialPsbt(bin)) {
      File f = SD_MMC.open("/signed.psbt", FILE_WRITE);
      if (f) { f.write(bin.data(), bin.size()); f.close(); ok = true; }
    }
    File t = SD_MMC.open("/signed.txt", FILE_WRITE);
    if (t) { t.print(g_ptx_b64); t.close(); ok = true; }
    ui::toast(ok ? "Wrote /signed.psbt to SD" : "SD write failed",
              ok ? 0x0E3A20 : 0x59201C);
  } else {
    File f = SD_MMC.open("/signed.txt", FILE_WRITE);
    if (f) { f.print(g_ptx_hex); f.close(); ok = true; }
    ui::toast(ok ? "Wrote /signed.txt to SD" : "SD write failed",
              ok ? 0x0E3A20 : 0x59201C);
  }
  SD_MMC.end();
}

static void onPsbtDecide(bool approve) {
  if (g_pstate != PS_PENDING) return;
  if (approve) {
    ui::showSigningOverlay();
    uint8_t priv[32], pub[32];
    bool ok = keystore::activePriv(priv) &&
              keystore::pubOf(keystore::activeIndex(), pub);
    const bool multi = g_ptx.isMultisig();
    if (ok) ok = multi ? g_ptx.signPartial(priv, pub) : g_ptx.signAll(priv, pub);
    memset(priv, 0, 32);
    if (ok) {
      if (multi) {
        g_ptx_b64 = g_ptx.partialPsbtB64();
        g_pstate = g_ptx_b64.length() ? PS_APPROVED : PS_DECLINED;
      } else {
        g_ptx_hex = g_ptx.finalHex();
        g_pstate = g_ptx_hex.length() ? PS_APPROVED : PS_DECLINED;
      }
    } else {
      g_pstate = PS_DECLINED;
      Serial.println("[psbt] sign failed: " + g_ptx.error);
      if (g_ptx.error.length()) ui::toast(g_ptx.error, 0x59201C);
    }
    if (g_pstate == PS_APPROVED) {
      Serial.println(multi
          ? "[psbt] APPROVED (partial), " + String(g_ptx_b64.length()) + " b64 chars"
          : "[psbt] APPROVED, " + String(g_ptx_hex.length() / 2) + " byte tx");
      if (g_ptx_from_sd) {
        // Airgapped path: the result can only leave by SD or QR, so write it
        // to the card and show the (animated) QR screen.
        writeSignedToSD();
        ui::showTxQr(multi ? g_ptx_b64 : g_ptx_hex,
                     multi ? "Partial multisig PSBT" : "Signed transaction");
        g_ptx_shown = false;
        return;
      }
      // USB/bridge path: the result is already flowing back to NostrTX over
      // the relays for broadcast — just confirm and return home.
      ui::toast("Signed — sent to NostrTX", 0x0E3A20);
    }
  } else {
    g_pstate = PS_DECLINED;
    Serial.println("[psbt] DECLINED on device");
  }
  g_ptx_shown = false;
  ui::showResult(g_pstate == PS_APPROVED, pendingCount());
}

// Accepts either raw PSBT bytes or base64 text; parses into g_ptx.
static bool loadPsbtBlob(const uint8_t* data, size_t len) {
  if (len > 6 && memcmp(data, "cHNidP", 6) == 0) {  // base64 of "psbt\xff"
    size_t cap = (len * 3) / 4 + 4, out_len = 0;
    uint8_t* bin = (uint8_t*)ps_malloc(cap);
    if (!bin) bin = (uint8_t*)malloc(cap);
    if (!bin) { g_ptx.error = "out of memory"; return false; }
    // strip whitespace/newlines that editors love to append
    uint8_t* clean = (uint8_t*)malloc(len);
    size_t clen = 0;
    if (!clean) { free(bin); g_ptx.error = "out of memory"; return false; }
    for (size_t i = 0; i < len; ++i)
      if (data[i] > ' ') clean[clen++] = data[i];
    bool ok = mbedtls_base64_decode(bin, cap, &out_len, clean, clen) == 0;
    free(clean);
    if (!ok) { free(bin); g_ptx.error = "bad base64"; return false; }
    ok = g_ptx.parse(bin, out_len);
    free(bin);
    return ok;
  }
  return g_ptx.parse(data, len);
}

static void signPsbtFromSD() {
  if (g_pstate == PS_PENDING) {
    ui::toast("A transaction is already waiting", 0x3B2A0A);
    return;
  }
  if (keystore::count() == 0) {
    ui::toast("No key on device", 0x59201C);
    return;
  }
  if (!sdBegin()) {
    ui::toast("No SD card", 0x59201C);
    return;
  }
  File f = SD_MMC.open("/unsigned.psbt");
  if (!f) f = SD_MMC.open("/unsigned.txt");
  if (!f) {
    SD_MMC.end();
    ui::toast("No /unsigned.psbt on SD", 0x59201C);
    return;
  }
  size_t len = f.size();
  if (len == 0 || len > 100000) {
    f.close();
    SD_MMC.end();
    ui::toast("PSBT file too large", 0x59201C);
    return;
  }
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) buf = (uint8_t*)malloc(len);
  if (!buf) {
    f.close();
    SD_MMC.end();
    ui::toast("Out of memory", 0x59201C);
    return;
  }
  f.read(buf, len);
  f.close();
  SD_MMC.end();

  bool ok = loadPsbtBlob(buf, len);
  free(buf);
  if (!ok) {
    ui::toast("PSBT: " + g_ptx.error, 0x59201C);
    return;
  }
  g_pstate = PS_PENDING;
  g_ptx_from_sd = true;
  g_ptx_born = millis();
  Serial.printf("[psbt] SD: %d in / %d out, fee %llu\n", (int)g_ptx.ins.size(),
                (int)g_ptx.outs.size(), (unsigned long long)g_ptx.fee());
  psbtPrompt();
}

// ---------------------------------------------------------------------------
// SD import / export
// ---------------------------------------------------------------------------
static bool sdBegin() {
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  return SD_MMC.begin("/sdcard", /*1bit=*/true);
}

static void importFromSD() {
  if (!sdBegin()) {
    ui::toast("No SD card", 0x59201C);
    return;
  }

  int imported = 0, dup = 0, bad = 0;
  bool found_file = false;

  File f = SD_MMC.open("/nostr-keys.json");
  if (f) {
    found_file = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      ui::toast("Bad JSON in nostr-keys.json", 0x59201C);
      SD_MMC.end();
      return;
    }
    for (JsonObject k : doc["keys"].as<JsonArray>()) {
      String key = k["key"] | "";
      if (!key.length()) key = String(k["nsec"] | "");
      if (!key.length()) key = String(k["hex"] | "");
      String label = k["label"] | "";
      int r = keystore::importKey(key, label);
      if (r >= 0) imported++;
      else if (r == -3) dup++;
      else if (r == -4) { ui::toast("Store full (8 max)", 0x3B2A0A); break; }
      else bad++;
    }
  }

  if (!found_file) {
    f = SD_MMC.open("/nostr-keys.txt");
    if (f) {
      found_file = true;
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length() || line[0] == '#') continue;
        String label, key = line;
        int colon = line.indexOf(':');
        if (colon > 0 && line.indexOf("nsec1", colon) >= 0) {
          label = line.substring(0, colon);
          label.trim();
          key = line.substring(colon + 1);
          key.trim();
        }
        int r = keystore::importKey(key, label);
        if (r >= 0) imported++;
        else if (r == -3) dup++;
        else if (r == -4) { ui::toast("Store full (8 max)", 0x3B2A0A); break; }
        else bad++;
      }
      f.close();
    }
  }

  SD_MMC.end();

  if (!found_file) {
    ui::toast("No /nostr-keys.json on SD", 0x59201C);
    return;
  }
  ui::toast("Imported " + String(imported) +
                (dup ? " (+" + String(dup) + " dup)" : ""),
            imported ? 0x0E3A20 : 0x3B2A0A);
  ui::refreshAccounts();
  Serial.printf("[poc] SD import: %d ok, %d dup, %d bad\n", imported, dup, bad);
}

static void exportToSD() {
  if (keystore::count() == 0) {
    ui::toast("Nothing to export", 0x3B2A0A);
    return;
  }
  if (!sdBegin()) {
    ui::toast("No SD card", 0x59201C);
    return;
  }

  JsonDocument doc;
  doc["version"] = 1;
  JsonArray keys = doc["keys"].to<JsonArray>();
  for (int i = 0; i < keystore::count(); ++i) {
    uint8_t priv[32];
    if (!keystore::privOf(i, priv)) continue;
    JsonObject k = keys.add<JsonObject>();
    k["label"] = keystore::labelOf(i);
    k["nsec"] = nostrocrypto::bech32_encode_32("nsec", priv);
    k["npub"] = keystore::npubOf(i);
    memset(priv, 0, 32);
  }

  File f = SD_MMC.open("/nostr-keys-export.json", FILE_WRITE);
  if (!f) {
    ui::toast("SD write failed", 0x59201C);
    SD_MMC.end();
    return;
  }
  serializeJsonPretty(doc, f);
  f.close();
  SD_MMC.end();
  ui::toast("Exported " + String(keystore::count()) + " keys to SD", 0x0E3A20);
  Serial.printf("[poc] exported %d keys to /nostr-keys-export.json\n",
                keystore::count());
}

static void persistAutoAuth(bool on) {
  g_auto_auth = on;
  Preferences pr;
  if (pr.begin("nostr", false)) {
    pr.putUChar("autoauth", on ? 1 : 0);
    pr.end();
  }
}

// ---------------------------------------------------------------------------
// Demo web page (stand-in for the browser extension transport)
// ---------------------------------------------------------------------------
static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Signer Link</title>
<style>
 body{font-family:-apple-system,system-ui,sans-serif;background:#0b0e13;color:#e8ecf2;
      margin:0;padding:24px;max-width:640px;margin-inline:auto}
 h1{font-size:22px;margin:0 0 4px}
 .sub{color:#7d8794;font-size:14px;margin-bottom:20px}
 .card{background:#141a22;border-radius:14px;padding:16px;margin-bottom:16px}
 .lbl{color:#7d8794;font-size:12px;text-transform:uppercase;letter-spacing:.08em}
 .mono{font-family:ui-monospace,Menlo,monospace;font-size:13px;word-break:break-all}
 textarea{width:100%;box-sizing:border-box;background:#0b0e13;color:#e8ecf2;
      border:1px solid #26303c;border-radius:10px;padding:12px;font-family:ui-monospace,Menlo,monospace;
      font-size:13px;min-height:130px}
 button{background:#18c29c;color:#04110d;border:0;border-radius:10px;padding:14px 22px;
      font-size:16px;font-weight:700;width:100%;margin-top:12px}
 button:disabled{opacity:.4}
 .status{margin-top:12px;font-size:14px}
 .ok{color:#18c29c}.warn{color:#f5a623}.err{color:#ef5d5d}
</style></head><body>
<h1>Signer Link</h1>
<div class="sub">You are talking to the device over the USB-C cable.
Nothing here touches the internet.</div>
<div class="card"><div class="lbl">Active device account</div>
<div class="mono" id="pk">loading&hellip;</div></div>
<div class="card"><div class="lbl">Event to sign (NIP-07 signEvent stand-in)</div>
<textarea id="ev">{"kind":1,"tags":[],"content":"Signed on an airgapped pocket device, over a USB-C wire."}</textarea>
<button id="go" onclick="send()">Request signature on device</button>
<div class="status" id="st"></div>
<div class="mono" id="out" style="margin-top:10px"></div></div>
<script>
fetch('/api/pubkey').then(r=>r.json()).then(j=>{
  document.getElementById('pk').textContent=j.pubkey||'(no key on device)';});
let poll=null;
function send(){
  const st=document.getElementById('st'),go=document.getElementById('go'),out=document.getElementById('out');
  let ev;try{ev=JSON.parse(document.getElementById('ev').value);}catch(e){st.textContent='Bad JSON';st.className='status err';return;}
  ev.created_at=ev.created_at||Math.floor(Date.now()/1000);
  st.textContent='Sent. Approve or decline ON THE DEVICE SCREEN.';st.className='status warn';
  out.textContent='';go.disabled=true;
  fetch('/api/sign',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify(ev)})
   .then(r=>r.json()).then(j=>{
     poll=setInterval(()=>{
       fetch('/api/result?id='+j.id).then(r=>r.json()).then(res=>{
         if(res.status==='pending')return;
         clearInterval(poll);go.disabled=false;
         if(res.status==='approved'){
           st.textContent='SIGNED on device — real BIP-340, verified before leaving.';st.className='status ok';
           out.textContent='id: '+res.id+'\nsig: '+res.sig;
         }
         else{st.textContent='DECLINED on device.';st.className='status err';}
       });
     },300);
   }).catch(e=>{st.textContent='Error: '+e;st.className='status err';go.disabled=false;});
}
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// HTTP API
// ---------------------------------------------------------------------------
// Origin policy: the device's own pages (same-origin) and the browser
// extension (whose background worker carries a *-extension:// origin) may use
// the API. Arbitrary websites may NOT — with "*" here, any page open while
// the device is plugged in could silently read the user's pubkey, profile
// and follows, linking their Nostr identity to their browsing. 403 them.
static bool originAllowed() {
  const String o = server.header("Origin");
  if (o.length() == 0) return true;  // curl / same-origin GET / non-browser
  if (o == "http://10.77.7.1") return true;  // device-served pages
  if (o.startsWith("chrome-extension://") ||
      o.startsWith("moz-extension://") ||
      o.startsWith("safari-web-extension://")) return true;
  return false;
}

static void addCors() {
  const String o = server.header("Origin");
  if (o.length() == 0) return;  // no cross-origin request, no CORS needed
  server.sendHeader("Access-Control-Allow-Origin", o);
  server.sendHeader("Vary", "Origin");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// Gate every API handler: reject foreign web origins, else attach CORS.
static bool corsGate() {
  if (!originAllowed()) {
    server.send(403, "application/json", "{\"error\":\"origin not allowed\"}");
    return false;
  }
  addCors();
  return true;
}

static void handleRoot() {
  g_http_hits++;
  server.send_P(200, "text/html", PAGE);
}

static void handleBridge() {
  g_http_hits++;
  // Stale cached copies of this page cause ghost bridge tabs after firmware
  // updates — always fetch fresh.
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", BRIDGE_PAGE);
}

static void handleStatus() {
  g_http_hits++;
  if (!corsGate()) return;
  String out = "{\"mounted\":true,\"pubkey_set\":";
  out += keystore::count() > 0 ? "true" : "false";
  out += ",\"accounts\":" + String(keystore::count());
  out += ",\"pending\":" + String(pendingCount());
  out += ",\"auto_auth\":";
  out += g_auto_auth ? "true" : "false";
  out += "}";
  server.send(200, "application/json", out);
}

static void handlePubkey() {
  g_http_hits++;
  if (!corsGate()) return;
  if (keystore::count() == 0) {
    server.send(404, "application/json",
                "{\"error\":\"no key on device — import or generate one\"}");
    return;
  }
  server.send(200, "application/json",
              "{\"pubkey\":\"" + keystore::activePubHex() + "\"}");
}

static void handleSign() {
  g_http_hits++;
  if (!corsGate()) return;
  if (keystore::count() == 0) {
    server.send(409, "application/json", "{\"error\":\"no key on device\"}");
    return;
  }
  String body = server.arg("plain");
  if (!body.length()) {
    server.send(400, "application/json", "{\"error\":\"empty body\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }

  // Idempotency: NIP-46 clients re-send requests while a human is still
  // deciding on the device. An identical body maps to the existing request
  // (pending, or resolved within the last 20s) instead of a duplicate prompt.
  for (int i = 0; i < QLEN; ++i) {
    SignRequest& q = g_queue[i];
    if (q.status == REQ_NONE || q.raw != body) continue;
    if (q.status == REQ_PENDING ||
        ((q.status == REQ_APPROVED || q.status == REQ_DECLINED) &&
         millis() - q.done_ms < 20000)) {
      server.send(200, "application/json", "{\"id\":" + String(q.id) + "}");
      return;
    }
  }

  SignRequest* r = allocReq();
  if (!r) {
    server.send(429, "application/json", "{\"error\":\"request queue full\"}");
    return;
  }
  *r = SignRequest();
  r->id = g_next_id++;
  r->kind = doc["kind"] | -1;
  r->content = String((const char*)(doc["content"] | ""));
  r->raw = body;
  r->status = REQ_PENDING;
  r->born_ms = millis();
  Serial.printf("[poc] sign request #%lu kind=%d (%u bytes, %d pending)\n",
                (unsigned long)r->id, r->kind, body.length(), pendingCount());

  // Relay auth (NIP-42) is identity boilerplate — sign silently when allowed.
  if (r->kind == 22242 && g_auto_auth) {
    if (signRequest(*r)) {
      r->status = REQ_APPROVED;
      r->auto_signed = true;
      r->done_ms = millis();
      ui::toast("Auto-signed relay auth", 0x2C1840);
      Serial.printf("[poc] request #%lu AUTO-SIGNED (relay auth)\n",
                    (unsigned long)r->id);
    } else {
      r->status = REQ_DECLINED;
      r->done_ms = millis();
    }
  }

  server.send(200, "application/json", "{\"id\":" + String(r->id) + "}");
}

static void handleResult() {
  g_http_hits++;
  if (!corsGate()) return;
  uint32_t id = (uint32_t)server.arg("id").toInt();
  SignRequest* r = findReq(id);
  if (!r) {
    server.send(404, "application/json", "{\"error\":\"unknown id\"}");
    return;
  }
  switch (r->status) {
    case REQ_PENDING:
      server.send(200, "application/json", "{\"status\":\"pending\"}");
      break;
    case REQ_APPROVED: {
      String out = "{\"status\":\"approved\"";
      out += ",\"id\":\"" + r->event_id_hex + "\"";
      out += ",\"sig\":\"" + r->sig_hex + "\"";
      out += ",\"pubkey\":\"" + r->pubkey_hex + "\"";
      out += ",\"created_at\":" + String(r->created_at);
      out += "}";
      r->polled = true;
      server.send(200, "application/json", out);
      break;
    }
    case REQ_DECLINED:
      r->polled = true;
      server.send(200, "application/json", "{\"status\":\"declined\"}");
      break;
    default:
      server.send(500, "application/json", "{\"error\":\"bad state\"}");
  }
}

// GET /api/profile?pubkey=<hex>  |  POST {"pubkey","name","picture_b64"}
static void handleProfileGet() {
  g_http_hits++;
  if (!corsGate()) return;
  String pk = server.arg("pubkey");
  if (pk.length() != 64) {
    server.send(400, "application/json", "{\"error\":\"pubkey required\"}");
    return;
  }
  String name = profile::nameFor(pk);
  bool pic = profile::hasAvatar(pk);
  if (!name.length() && !pic) {
    server.send(404, "application/json", "{\"error\":\"no profile cached\"}");
    return;
  }
  JsonDocument doc;
  doc["name"] = name;
  doc["has_picture"] = pic;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleProfilePost() {
  g_http_hits++;
  if (!corsGate()) return;
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  String pk = doc["pubkey"] | "";
  String name = doc["name"] | "";
  String about = doc["about"] | "";
  String lud16 = doc["lud16"] | "";
  const char* b64 = doc["picture_b64"] | (const char*)nullptr;
  if (pk.length() != 64) {
    server.send(400, "application/json", "{\"error\":\"pubkey required\"}");
    return;
  }

  uint8_t* png = nullptr;
  size_t png_len = 0;
  if (b64 && strlen(b64) > 0) {
    size_t b64len = strlen(b64);
    size_t cap = (b64len * 3) / 4 + 4;
    if (cap > 256 * 1024) {  // animated GIF avatars can be chunky
      server.send(413, "application/json", "{\"error\":\"picture too large\"}");
      return;
    }
    png = (uint8_t*)ps_malloc(cap);
    if (!png) png = (uint8_t*)malloc(cap);
    if (!png || mbedtls_base64_decode(png, cap, &png_len,
                                      (const uint8_t*)b64, b64len) != 0) {
      free(png);
      server.send(400, "application/json", "{\"error\":\"bad base64\"}");
      return;
    }
  }

  bool ok = profile::store(pk, name, about, lud16, png, png_len);
  free(png);
  if (ok) {
    ui::refreshAccounts();
    if (name.length()) ui::toast("Profile: " + name, 0x0C2B38);
  }
  server.send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"store failed\"}");
}

static void handleOptions() {
  if (!corsGate()) return;
  server.send(204);
}

// POST /api/psbt {"psbt":"<base64>"} -> review + sign a Bitcoin transaction
static void handlePsbtPost() {
  g_http_hits++;
  if (!corsGate()) return;
  if (keystore::count() == 0) {
    server.send(409, "application/json", "{\"error\":\"no key on device\"}");
    return;
  }
  if (g_pstate == PS_PENDING) {
    server.send(429, "application/json",
                "{\"error\":\"a transaction is already awaiting review\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  const char* b64 = doc["psbt"] | "";
  size_t blen = strlen(b64);
  if (blen < 10 || blen > 100000) {
    server.send(400, "application/json", "{\"error\":\"psbt field required\"}");
    return;
  }
  if (!loadPsbtBlob((const uint8_t*)b64, blen)) {
    server.send(400, "application/json",
                "{\"error\":\"" + g_ptx.error + "\"}");
    return;
  }
  g_pstate = PS_PENDING;
  g_ptx_from_sd = false;
  g_ptx_shown = false;  // main loop prompts once the screen is free
  g_ptx_born = millis();
  Serial.printf("[psbt] USB: %d in / %d out, fee %llu\n", (int)g_ptx.ins.size(),
                (int)g_ptx.outs.size(), (unsigned long long)g_ptx.fee());
  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /api/psbt-result -> pending | declined | approved + final tx hex
static void handlePsbtResult() {
  g_http_hits++;
  if (!corsGate()) return;
  switch (g_pstate) {
    case PS_NONE:
      server.send(404, "application/json", "{\"error\":\"no transaction\"}");
      break;
    case PS_PENDING:
      server.send(200, "application/json", "{\"status\":\"pending\"}");
      break;
    case PS_DECLINED:
      server.send(200, "application/json", "{\"status\":\"declined\"}");
      break;
    case PS_APPROVED:
      if (g_ptx.isMultisig())
        server.send(200, "application/json",
                    "{\"status\":\"approved\",\"psbt\":\"" + g_ptx_b64 + "\"}");
      else
        server.send(200, "application/json",
                    "{\"status\":\"approved\",\"hex\":\"" + g_ptx_hex + "\"}");
      break;
  }
}

// All accounts, so the bridge/extension can sync kind-0 profiles for each.
static void handleAccounts() {
  g_http_hits++;
  if (!corsGate()) return;
  JsonDocument doc;
  JsonArray arr = doc["accounts"].to<JsonArray>();
  for (int i = 0; i < keystore::count(); ++i) {
    String pk = keystore::pubHexOf(i);
    JsonObject o = arr.add<JsonObject>();
    o["pubkey"] = pk;
    o["label"] = keystore::labelOf(i);
    o["name"] = profile::nameFor(pk);
    o["has_picture"] = profile::hasAvatar(pk);
    o["active"] = (i == keystore::activeIndex());
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/follows — bridge pushes the active account's follow list:
//   {"pubkey":"<hex>","follows":[{"pk":"<hex>","name":"...","lud16":"..."}]}
// Avatars for follows arrive separately via POST /api/profile (picture-only).
static void handleFollowsPost() {
  g_http_hits++;
  if (!corsGate()) return;
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc["follows"].is<JsonArray>()) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  if (!profile::setFollowsJson(body)) {
    server.send(500, "application/json", "{\"error\":\"store failed\"}");
    return;
  }
  int n = doc["follows"].as<JsonArray>().size();
  Serial.printf("[poc] follows synced: %d entries\n", n);
  ui::refreshFollows();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleFollowsGet() {
  g_http_hits++;
  if (!corsGate()) return;
  String s = profile::getFollowsJson();
  if (!s.length()) {
    server.send(404, "application/json", "{\"error\":\"no follows cached\"}");
    return;
  }
  server.send(200, "application/json", s);
}

// The bridge page registers its bunker URI here when it loads.
static String g_bridge_bunker;

static void handleBridgeInfoPost() {
  g_http_hits++;
  if (!corsGate()) return;
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  g_bridge_bunker = String((const char*)(doc["bunker"] | ""));
  Serial.println("[poc] bridge online: " + g_bridge_bunker);
  ui::toast("Bridge online", 0x0C2B38);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleBridgeInfoGet() {
  g_http_hits++;
  if (!corsGate()) return;
  if (!g_bridge_bunker.length()) {
    server.send(404, "application/json", "{\"error\":\"bridge not connected\"}");
    return;
  }
  JsonDocument doc;
  doc["bunker"] = g_bridge_bunker;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// Physical BOOT button: click = move / next, hold = confirm / back
// ---------------------------------------------------------------------------
static bool btn_down = false;
static uint32_t btn_t0 = 0;
static bool btn_long_fired = false;

static void pollButton() {
  const uint32_t now = millis();
  if (now < 3000) return;
  const bool p = digitalRead(BTN_PIN) == LOW;
  if (p && !btn_down) {
    btn_down = true;
    btn_t0 = now;
    btn_long_fired = false;
  } else if (p && btn_down && !btn_long_fired && now - btn_t0 >= 600) {
    btn_long_fired = true;
    ui::buttonLong();
  } else if (!p && btn_down) {
    btn_down = false;
    if (!btn_long_fired && now - btn_t0 >= 30) ui::buttonShort();
  }
}

// ---------------------------------------------------------------------------
// Serial debug: T self-test, A accounts, S usb status, N new key,
// 0-7 set active, X remove active, U auto-auth toggle, B btc address,
// W rgb565 swap
// ---------------------------------------------------------------------------
static void processSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'T') {
      if (keystore::count() == 0) {
        Serial.println("[selftest] no keys");
        continue;
      }
      uint8_t priv[32], msg[32], sig[64];
      keystore::activePriv(priv);
      const char* m = "signer link self test";
      bip340::sha256((const uint8_t*)m, strlen(m), msg);
      uint32_t t0 = millis();
      bool ok = bip340::sign(priv, msg, sig);
      memset(priv, 0, 32);
      Serial.printf("[selftest] sign+verify %s in %lums\n",
                    ok ? "OK" : "FAILED", (unsigned long)(millis() - t0));
      Serial.println("[selftest] pub " + keystore::activePubHex());
      Serial.println("[selftest] sig " + hexOf(sig, 64));
    } else if (c == 'A') {
      Serial.printf("[accounts] %d key(s), active=%d\n",
                    keystore::count(), keystore::activeIndex());
      for (int i = 0; i < keystore::count(); ++i) {
        Serial.printf("  [%d]%s %s  %s\n", i,
                      i == keystore::activeIndex() ? "*" : " ",
                      keystore::labelOf(i).c_str(), keystore::npubOf(i).c_str());
      }
    } else if (c == 'S') {
      extern uint32_t ecm_report_attempts, ecm_report_sends;
      Serial.printf("[usb] mounted=%d dataItf=%d linkUp=%d rx=%lu tx=%lu "
                    "notify_att=%lu notify_sent=%lu pending=%d heap=%u psram=%u\n",
                    (int)usbnet::mounted(), (int)usbnet::dataActive(),
                    (int)usbnet::linkUp(),
                    (unsigned long)usbnet::rxPackets(),
                    (unsigned long)usbnet::txPackets(),
                    (unsigned long)ecm_report_attempts,
                    (unsigned long)ecm_report_sends, pendingCount(),
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    } else if (c == 'N') {
      int r = keystore::generateKey("key " + String(keystore::count() + 1));
      Serial.printf("[accounts] generateKey -> %d\n", r);
      ui::refreshAccounts();
    } else if (c >= '0' && c <= '7') {
      bool ok = keystore::setActive(c - '0');
      Serial.printf("[accounts] setActive(%c) %s\n", c, ok ? "ok" : "FAILED");
      ui::refreshAccounts();
    } else if (c == 'X') {
      int idx = keystore::activeIndex();
      bool ok = keystore::removeKey(idx);
      Serial.printf("[accounts] removeKey(%d) %s\n", idx, ok ? "ok" : "FAILED");
      ui::refreshAccounts();
    } else if (c == 'U') {
      persistAutoAuth(!g_auto_auth);
      Serial.printf("[poc] auto-auth -> %s\n", g_auto_auth ? "on" : "off");
    } else if (c == 'B') {
      if (keystore::count() == 0) {
        Serial.println("[btc] no keys");
        continue;
      }
      uint8_t pub[32];
      keystore::pubOf(keystore::activeIndex(), pub);
      uint32_t t0 = millis();
      String addr = taproot::addressFromXonly(pub);
      Serial.printf("[btc] pub  %s\n", keystore::activePubHex().c_str());
      Serial.printf("[btc] p2tr %s (%lums)\n", addr.c_str(),
                    (unsigned long)(millis() - t0));
    } else if (c == 'W') {
      ui::toggleByteSwap();
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("[poc] usb-net signer boot (ECM + BIP-340 + LVGL watch UI)");

  pinMode(BTN_PIN, INPUT_PULLUP);

  Wire.begin(IIC_SDA, IIC_SCL);
  if (expander.begin(0x20)) {
    for (int p = 0; p <= 2; ++p) expander.pinMode(p, OUTPUT);
    for (int p = 0; p <= 2; ++p) expander.digitalWrite(p, LOW);
    delay(20);
    for (int p = 0; p <= 2; ++p) expander.digitalWrite(p, HIGH);
    delay(20);
  }

  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setBrightness(255);

  for (int tries = 0; tries < 5 && !touch_ok; ++tries) {
    if (CST->begin()) { touch_ok = true; break; }
    delay(200);
  }
  if (touch_ok) {
    CST->IIC_Write_Device_State(
        CST->Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
        CST->Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC);
  }
  Serial.printf("[poc] touch %s\n", touch_ok ? "ok" : "UNAVAILABLE");

  keystore::begin();
  profile::begin();

  {
    Preferences pr;
    if (pr.begin("nostr", false)) {
      g_auto_auth = pr.getUChar("autoauth", 1) != 0;
      pr.end();
    }
  }

  UiCallbacks cb;
  cb.decide = onDecide;
  cb.decidePsbt = onPsbtDecide;
  cb.importSD = importFromSD;
  cb.exportSD = exportToSD;
  cb.signPsbtSD = signPsbtFromSD;
  cb.setAutoAuth = persistAutoAuth;
  ui::init(gfx, CST.get(), cb, g_auto_auth);

  if (!usbnet::begin()) {
    Serial.println("[poc] usbnet begin FAILED");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/bridge", HTTP_GET, handleBridge);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/pubkey", HTTP_GET, handlePubkey);
  server.on("/api/sign", HTTP_POST, handleSign);
  server.on("/api/sign", HTTP_OPTIONS, handleOptions);
  server.on("/api/result", HTTP_GET, handleResult);
  server.on("/api/profile", HTTP_GET, handleProfileGet);
  server.on("/api/profile", HTTP_POST, handleProfilePost);
  server.on("/api/profile", HTTP_OPTIONS, handleOptions);
  server.on("/api/bridge-info", HTTP_GET, handleBridgeInfoGet);
  server.on("/api/bridge-info", HTTP_POST, handleBridgeInfoPost);
  server.on("/api/accounts", HTTP_GET, handleAccounts);
  server.on("/api/follows", HTTP_GET, handleFollowsGet);
  server.on("/api/follows", HTTP_POST, handleFollowsPost);
  server.on("/api/follows", HTTP_OPTIONS, handleOptions);
  server.on("/api/psbt", HTTP_POST, handlePsbtPost);
  server.on("/api/psbt", HTTP_OPTIONS, handleOptions);
  server.on("/api/psbt-result", HTTP_GET, handlePsbtResult);
  // WebServer only captures headers it's told to collect; without this,
  // server.header("Origin") is always "" and the origin gate silently
  // fails open. Do not remove.
  static const char* kCollect[] = {"Origin"};
  server.collectHeaders(kCollect, 1);
  server.begin();
  Serial.println("[poc] http server on :80");
}

void loop() {
  usbnet::tick();
  server.handleClient();
  pollButton();
  processSerial();

  const uint32_t now = millis();

  // Expire pending requests nobody acted on (extension gives up at ~3 min).
  for (int i = 0; i < QLEN; ++i) {
    if (g_queue[i].status == REQ_PENDING && now - g_queue[i].born_ms > 180000) {
      g_queue[i].status = REQ_DECLINED;
      g_queue[i].done_ms = now;
    }
  }

  ui::setLink(usbnet::linkUp());

  // A pending Bitcoin tx expires after 5 minutes unattended.
  if (g_pstate == PS_PENDING && now - g_ptx_born > 300000) {
    g_pstate = PS_DECLINED;
    g_ptx_shown = false;
    if (ui::psbtScreenActive()) ui::showResult(false, pendingCount());
  }

  SignRequest* cur = currentPending();
  if (cur && cur->id != g_shown_req_id) {
    g_shown_req_id = cur->id;
    g_ptx_shown = false;  // nostr request takes the screen; re-prompt after
    ui::showRequest(cur->id, cur->kind, cur->content, pendingCount() - 1);
  } else if (!cur && g_shown_req_id) {
    g_shown_req_id = 0;
    ui::clearRequest();
  }

  // With the screen free of Nostr requests, surface a waiting Bitcoin tx.
  if (!cur && g_pstate == PS_PENDING && !g_ptx_shown) psbtPrompt();

  ui::tick();
  delay(2);
}
