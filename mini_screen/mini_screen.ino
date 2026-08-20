// mini_screen — multi-app "OS" for Waveshare ESP32-S3 Touch-AMOLED-1.8 (V2).
//
//   focused app list (post-focus):
//     * Home     — 3xN launcher tile grid
//     * Genesis  — "Eyes"   : Lenia continuous-CA terrarium, one universe/device
//     * Vespers  — "Ears"   : generative 3-voice composer via ES8311
//     * Signer   —           real secp256k1 keypair, npub + QR, SD backup, sign
//     * Voice    —           hold-to-record voice memos, WAV on SD
//     * Dice     —           provable-fair single die, swipe or shake to roll
//     * Photos   —           JPEG viewer, reads /photos/*.jpg from SD
//     * Videos   —           MJPEG player, reads /videos/*.mjpg from SD
//   utility apps (still registered for serial debug + Home tail):
//     * WiFi     — captive-portal provisioning ("mini_setup" AP)
//
//   universal nav:
//     * tap a tile on Home           → open that app
//     * long-press (~900ms) anywhere → back to Home
//     * app-specific gestures documented in the app files
//
// The main sketch owns the display + touch + IMU + audio init. Then it
// hands an AppContext to the Shell and lets each App do the rest.
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_random.h>
#include <SD_MMC.h>
#include <FS.h>

#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <Adafruit_XCA9554.h>
#include <ESP_I2S.h>
#include <SensorQMI8658.hpp>

#include "pin_config.h"
#include "es8311.h"

#include "src/shell.h"
#include "src/apps/home.h"
#include "src/apps/dice.h"
#include "src/apps/signer.h"
#include "src/apps/voice.h"
#include "src/apps/torch.h"
#include "src/apps/explorer.h"
#include "src/apps/steps.h"
#include "src/apps/level.h"
#include "src/apps/piano.h"
#include "src/apps/etch.h"
#include "src/apps/wifi_setup.h"
#include "src/apps/notes.h"
#include "src/apps/genesis.h"
#include "src/apps/snake.h"
#include "src/apps/vespers.h"
#include "src/apps/photos.h"
#include "src/apps/videos.h"

// Bump the Arduino loop task stack from 8KB to 16KB. FreeFont rendering,
// mbedTLS ECP mul, and I2S buffers add up — 8KB is not enough headroom.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---------------------------------------------------------------------------
// Display + touch (V2 board)
// ---------------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
#define SCREEN_ROTATION 0
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED, SCREEN_ROTATION,
    LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);

Adafruit_XCA9554 expander;
static bool touch_ok = false;

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> CST(new Arduino_CST816x(
    IIC_Bus, CST816T_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE,
    TP_INT, Arduino_IIC_Touch_Interrupt));
void IRAM_ATTR Arduino_IIC_Touch_Interrupt() { CST->IIC_Interrupt_Flag = true; }

SensorQMI8658 qmi;
static bool imu_ok = false;

// ---------------------------------------------------------------------------
// Audio (ES8311, full duplex)
// ---------------------------------------------------------------------------
I2SClass i2s;
static bool audio_ok = false;
static constexpr uint32_t SR_HZ = 16000;

static void audio_init() {
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);
  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SR_HZ, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[mini] I2S init failed");
    return;
  }
  es8311_handle_t es = es8311_create(0, ES8311_ADDRRES_0);
  if (!es) { Serial.println("[mini] ES8311 create failed"); return; }
  const es8311_clock_config_t clk = {
    .mclk_inverted      = false,
    .sclk_inverted      = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency     = (int)(SR_HZ * 256),
    .sample_frequency   = (int)SR_HZ,
  };
  es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
  // Enable analog mic (false = analog input path).
  es8311_microphone_config(es, false);
  es8311_voice_volume_set(es, 80, NULL);
  audio_ok = true;
}

// Play a mono int16 buffer as stereo through the ES8311 output.
static void audio_play_mono(const int16_t *mono, size_t frames) {
  if (!audio_ok) return;
  constexpr size_t CHUNK = 128;
  int16_t stereo[CHUNK * 2];
  size_t off = 0;
  while (off < frames) {
    size_t n = min((size_t)CHUNK, frames - off);
    for (size_t i = 0; i < n; ++i) {
      stereo[2 * i]     = mono[off + i];
      stereo[2 * i + 1] = mono[off + i];
    }
    i2s.write((uint8_t*)stereo, n * 2 * sizeof(int16_t));
    off += n;
  }
}

// Read `n` mono samples from the mic. We read stereo pairs and average, so
// we get a single-channel signal even though the I2S peripheral is in stereo.
static size_t audio_read_mono(int16_t *dst, size_t n) {
  if (!audio_ok) return 0;
  constexpr size_t CHUNK = 128;
  int16_t stereo[CHUNK * 2];
  size_t out = 0;
  while (out < n) {
    size_t want = min((size_t)CHUNK, n - out);
    size_t bytes = i2s.readBytes((char*)stereo, want * 2 * sizeof(int16_t));
    size_t got_pairs = bytes / (2 * sizeof(int16_t));
    if (got_pairs == 0) break;
    for (size_t i = 0; i < got_pairs; ++i) {
      int32_t s = ((int32_t)stereo[2 * i] + (int32_t)stereo[2 * i + 1]) / 2;
      dst[out + i] = (int16_t)s;
    }
    out += got_pairs;
    if (got_pairs < want) break;  // no more ready
  }
  return out;
}

static void sfx_click() {
  constexpr size_t N = 24;
  int16_t buf[N];
  for (size_t i = 0; i < N; ++i) {
    float env = 1.0f - (float)i / (float)N;
    float n = (float)(esp_random() & 0xFFFF) / 32768.0f - 1.0f;
    buf[i] = (int16_t)(n * env * 12000);
  }
  audio_play_mono(buf, N);
}
static void sfx_thump() {
  constexpr size_t N = SR_HZ * 120 / 1000;
  static int16_t buf[N];
  const float f = 90.0f;
  const float tau = 0.045f;
  for (size_t i = 0; i < N; ++i) {
    float t = (float)i / (float)SR_HZ;
    float env = expf(-t / tau);
    float s = sinf(2.0f * (float)M_PI * f * t);
    buf[i] = (int16_t)(s * env * 22000);
  }
  audio_play_mono(buf, N);
}

// ---------------------------------------------------------------------------
// SD card (SDMMC 1-bit mode)
// ---------------------------------------------------------------------------
static bool sd_ok = false;
static void sd_init() {
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (!SD_MMC.begin("/sdcard", /*mode1bit=*/true, /*format_if_mount_failed=*/false)) {
    Serial.println("[mini] SD_MMC mount failed (or no card inserted)");
    return;
  }
  sd_ok = true;
  Serial.printf("[mini] SD_MMC ok, cardSize=%llu MB\n",
                (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
}

// ---------------------------------------------------------------------------
// Shell + app instances
// ---------------------------------------------------------------------------
static Preferences g_prefs;
static mini::TouchInput g_touch;
static mini::Shell     g_shell;
static mini::Home      g_home;
static mini::Signer    g_signer;
static mini::Dice      g_dice;
static mini::VoiceMemo g_voice;
static mini::Torch     g_torch;
static mini::Explorer  g_explorer;
static mini::Steps     g_steps;
static mini::Level     g_level;
static mini::Piano     g_piano;
static mini::Etch      g_etch;
static mini::WifiSetup g_wifi;
static mini::Notes     g_notes;
static mini::Genesis   g_genesis;
static mini::Snake     g_snake;
static mini::Vespers   g_vespers;
static mini::Photos    g_photos;
static mini::Videos    g_videos;

// Home MUST be index 0. Post-focus order (per user):
//   Genesis (eyes) → Vespers (ears) → Signer (nostr keys + signing) →
//   Voice → Dice → Photos → Videos → WiFi (utility, keep near the end).
// The apps below WiFi are kept registered so serial-debug still reaches
// them, but they've been removed from the Home grid.
static mini::App* g_apps[] = {
    &g_home,
    &g_genesis, &g_vespers, &g_signer,
    &g_voice,   &g_dice,
    &g_photos,  &g_videos,
    &g_wifi };
static constexpr size_t N_APPS = sizeof(g_apps) / sizeof(g_apps[0]);

// ---------------------------------------------------------------------------
// Gesture producer
// ---------------------------------------------------------------------------
namespace {

// -- touch state -----------------------------------------------------------
struct TouchLocal {
  bool     down = false;
  uint32_t t0_ms = 0;
  int16_t  x0 = 0, y0 = 0;
  int16_t  x  = 0, y  = 0;
  bool     long_press_fired = false;
  bool     touch_down_fired = false;
} tp;

constexpr int16_t  SWIPE_MIN_PX     = 60;
constexpr int16_t  TAP_MAX_DIST     = 12;
constexpr uint32_t TAP_MAX_MS       = 350;
constexpr uint32_t LONG_PRESS_MS    = 900;

void emit_touch_gestures() {
  if (!touch_ok) return;
  const uint32_t now = millis();
  int32_t n = CST->IIC_Read_Device_Value(
      CST->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  if (n > 0) {
    int32_t x = CST->IIC_Read_Device_Value(
        CST->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = CST->IIC_Read_Device_Value(
        CST->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    if (!tp.down) {
      tp.down = true;
      tp.t0_ms = now;
      tp.x0 = tp.x = (int16_t)x;
      tp.y0 = tp.y = (int16_t)y;
      tp.long_press_fired = false;
      tp.touch_down_fired = true;
      // Update live touch state before firing so apps can query it.
      g_touch.down = true; g_touch.x = tp.x; g_touch.y = tp.y; g_touch.held_ms = 0;
      mini::Gesture gd;
      gd.type = mini::Gesture::TOUCH_DOWN;
      gd.x = tp.x; gd.y = tp.y;
      g_shell.deliverGesture(gd);
    } else {
      tp.x = (int16_t)x;
      tp.y = (int16_t)y;
      g_touch.x = tp.x; g_touch.y = tp.y;
      g_touch.held_ms = now - tp.t0_ms;
      if (!tp.long_press_fired) {
        const int16_t adx = abs(tp.x - tp.x0);
        const int16_t ady = abs(tp.y - tp.y0);
        if ((now - tp.t0_ms) >= LONG_PRESS_MS &&
            adx <= TAP_MAX_DIST && ady <= TAP_MAX_DIST) {
          tp.long_press_fired = true;
          mini::Gesture g;
          g.type = mini::Gesture::LONG_PRESS;
          g.x = tp.x0; g.y = tp.y0;
          g_shell.deliverGesture(g);
        }
      }
    }
  } else if (tp.down) {
    const int16_t dx = tp.x - tp.x0;
    const int16_t dy = tp.y - tp.y0;
    const int16_t adx = abs(dx), ady = abs(dy);
    const uint32_t dt = now - tp.t0_ms;
    tp.down = false;
    g_touch.down = false; g_touch.held_ms = 0;

    // Always fire TOUCH_UP first so apps like Voice can finalize a recording
    // before the higher-level TAP/SWIPE_ classification lands.
    mini::Gesture up;
    up.type = mini::Gesture::TOUCH_UP;
    up.x = tp.x; up.y = tp.y;
    up.dx = dx;  up.dy = dy;
    g_shell.deliverGesture(up);

    if (tp.long_press_fired) return;

    mini::Gesture g;
    if (adx >= SWIPE_MIN_PX && adx >= ady) {
      g.type = (dx > 0) ? mini::Gesture::SWIPE_RIGHT : mini::Gesture::SWIPE_LEFT;
      g.dx = dx; g.dy = dy; g.x = tp.x0; g.y = tp.y0;
      g_shell.deliverGesture(g);
    } else if (ady >= SWIPE_MIN_PX) {
      g.type = (dy > 0) ? mini::Gesture::SWIPE_DOWN : mini::Gesture::SWIPE_UP;
      g.dx = dx; g.dy = dy; g.x = tp.x0; g.y = tp.y0;
      g_shell.deliverGesture(g);
    } else if (dt <= TAP_MAX_MS && adx <= TAP_MAX_DIST && ady <= TAP_MAX_DIST) {
      g.type = mini::Gesture::TAP;
      g.x = tp.x0; g.y = tp.y0;
      g_shell.deliverGesture(g);
    }
  }
}

// -- IMU / shake ------------------------------------------------------------
constexpr float    SHAKE_ARM_G     = 1.0f;
constexpr float    SHAKE_RELEASE_G = 0.30f;
constexpr uint32_t SHAKE_MIN_MS    = 120;
constexpr uint32_t SHAKE_QUIET_MS  = 120;
constexpr uint32_t SHAKE_MAX_MS    = 3000;
constexpr uint32_t POST_SHAKE_MS   = 700;

enum ShakeState : uint8_t { SH_IDLE, SH_ACTIVE, SH_SETTLING };
ShakeState sh_state = SH_IDLE;
uint32_t   sh_start_ms = 0;
uint32_t   sh_quiet_since_ms = 0;
uint32_t   sh_last_fire_ms = 0;

// -- Physical BOOT button (GPIO0) ------------------------------------------
// The Waveshare board's only user button is the BOOT switch on GPIO0. It's
// pulled HIGH by default and driven LOW when pressed. We emit BUTTON_DOWN /
// BUTTON_UP gestures so apps like Voice can use it as a hardware
// hold-to-record without needing to hold a finger on the screen.
static bool     btn_prev_down = false;
static uint32_t btn_last_edge_ms = 0;
constexpr uint32_t BTN_DEBOUNCE_MS = 20;

void emit_button_gesture() {
  const uint32_t now = millis();
  if (now - btn_last_edge_ms < BTN_DEBOUNCE_MS) return;
  const bool down_now = (digitalRead(0) == LOW);
  if (down_now != btn_prev_down) {
    btn_prev_down = down_now;
    btn_last_edge_ms = now;
    mini::Gesture g;
    g.type = down_now ? mini::Gesture::BUTTON_DOWN : mini::Gesture::BUTTON_UP;
    Serial.printf("[btn] GPIO0 %s\n", down_now ? "DOWN" : "UP");
    g_shell.deliverGesture(g);
  }
}

void emit_shake_gesture() {
  if (!imu_ok) return;
  if (!qmi.getDataReady()) return;
  float ax, ay, az;
  if (!qmi.getAccelerometer(ax, ay, az)) return;
  const uint32_t now = millis();
  const float mag = sqrtf(ax*ax + ay*ay + az*az);
  const float dev = fabsf(mag - 1.0f);
  switch (sh_state) {
    case SH_IDLE:
      if (now - sh_last_fire_ms < POST_SHAKE_MS) break;
      if (dev > SHAKE_ARM_G) {
        sh_state = SH_ACTIVE;
        sh_start_ms = now;
        sh_quiet_since_ms = 0;
      }
      break;
    case SH_ACTIVE:
      if (dev < SHAKE_RELEASE_G && (now - sh_start_ms) >= SHAKE_MIN_MS) {
        sh_state = SH_SETTLING;
        sh_quiet_since_ms = now;
      } else if ((now - sh_start_ms) > SHAKE_MAX_MS) {
        sh_state = SH_IDLE;
      }
      break;
    case SH_SETTLING:
      if (dev > SHAKE_RELEASE_G) {
        sh_state = SH_ACTIVE;
        sh_quiet_since_ms = 0;
      } else if ((now - sh_quiet_since_ms) >= SHAKE_QUIET_MS) {
        mini::Gesture g;
        g.type = mini::Gesture::SHAKE;
        g_shell.deliverGesture(g);
        sh_last_fire_ms = now;
        sh_state = SH_IDLE;
      }
      break;
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("[mini] boot");
  Serial.printf("[mini] free heap = %lu\n", (unsigned long)ESP.getFreeHeap());

  // Physical side button (BOOT on GPIO0). Has an internal pull-up on the
  // ESP32-S3; button ties it to GND. Only checked as a strap at reset, so
  // safe to use as a general input during runtime.
  pinMode(0, INPUT_PULLUP);

  Wire.begin(IIC_SDA, IIC_SCL);

  if (!expander.begin(0x20)) {
    Serial.println("[mini] XCA9554 not found — display/touch may not init");
  } else {
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
    Serial.println("[mini] CST816 ok");
  } else {
    Serial.println("[mini] CST816 unreachable — touch disabled");
  }

  imu_ok = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imu_ok) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_8G,
                            SensorQMI8658::ACC_ODR_500Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS,
                        SensorQMI8658::GYR_ODR_448_4Hz,
                        SensorQMI8658::LPF_MODE_3);
    qmi.enableAccelerometer();
    qmi.enableGyroscope();
    Serial.printf("[mini] QMI8658 ok, chipID=0x%02X\n", qmi.getChipID());
  } else {
    Serial.println("[mini] QMI8658 not found — shake disabled");
  }

  audio_init();
  sd_init();

  // Wire audio hooks into the apps that need them.
  g_dice.setAudio(&sfx_click, &sfx_thump);
  g_voice.setAudio(&audio_read_mono, &audio_play_mono);
  if (sd_ok) g_voice.setStorage(&SD_MMC, "/voice");
  g_piano.setAudio(&audio_play_mono);
  g_level.setBeep(&sfx_click);
  g_vespers.setAudio(&audio_play_mono);

  // Kick off saved-WiFi auto-connect (non-blocking). Explorer + future apps
  // that need connectivity can just check WiFi.status() when they want it.
  mini::WifiSetup::tryConnectSaved();

  // Shell context
  g_home.setShell(&g_shell);
  mini::AppContext ctx;
  ctx.gfx      = gfx;
  ctx.imu      = imu_ok ? &qmi : nullptr;
  ctx.prefs    = &g_prefs;
  ctx.audio_ok = audio_ok;
  ctx.sd_ok    = sd_ok;
  ctx.w        = LCD_WIDTH;
  ctx.h        = LCD_HEIGHT;
  ctx.touch    = &g_touch;

  g_shell.begin(ctx, g_apps, N_APPS, /*home_idx=*/0);
  Serial.printf("[mini] ready. free heap = %lu, stack watermark = %lu\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL));
}

// Serial debug: single-letter commands over USB CDC let a host script
// (or a curious developer) drive the shell without touching the screen.
// Useful for isolating rendering bugs on specific apps.
//
//   h = home    g = Genesis (eyes)     r = Vespers (ears)
//   s = signer  v = voice   d = dice   P = photos    V = videos
//   w = wifi
//   ? = list apps + current index
static void process_serial_debug() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') continue;
    size_t target = (size_t)-1;
    switch (c) {
      case 'h': target = 0; break;
      case 'g': target = 1; break;
      case 'r': target = 2; break;
      case 's': target = 3; break;
      case 'v': target = 4; break;
      case 'd': target = 5; break;
      case 'P': target = 6; break;
      case 'V': target = 7; break;
      case 'w': target = 8; break;
      case '?':
        Serial.printf("[dbg] current=%u count=%u\n",
                      (unsigned)g_shell.currentIdx(),
                      (unsigned)g_shell.count());
        for (size_t i = 0; i < g_shell.count(); ++i) {
          Serial.printf("  [%u] %s\n", (unsigned)i, g_shell.at(i)->name());
        }
        break;
      default: break;
    }
    if (target != (size_t)-1 && target < g_shell.count()) {
      Serial.printf("[dbg] -> switchTo(%u) '%s'\n",
                    (unsigned)target, g_shell.at(target)->name());
      g_shell.switchTo(target);
    }
  }
}

void loop() {
  emit_touch_gestures();
  emit_shake_gesture();
  emit_button_gesture();
  process_serial_debug();
  g_shell.tick(millis());
  delay(2);
}
