// LVGL 9 watch-style UI. Design language borrowed from watchOS: pure black
// background, soft dark cards, anti-aliased Montserrat type, round call-style
// action buttons, kinetic scrolling, animated transitions, toasts.
#include "ui.h"

#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <ArduinoJson.h>
#include <qrcode.h>
#include <vector>

#include "pin_config.h"
#include "keystore.h"
#include "profile.h"
#include "bech32.h"
#include "taproot.h"

namespace ui {

// ---------------------------------------------------------------------------
// Palette (Apple system colors, dark mode)
// ---------------------------------------------------------------------------
static const lv_color_t C_BG      = lv_color_hex(0x000000);
static const lv_color_t C_CARD    = lv_color_hex(0x1C1C1E);
static const lv_color_t C_CARD_HI = lv_color_hex(0x2C2C2E);
static const lv_color_t C_TEXT    = lv_color_hex(0xFFFFFF);
static const lv_color_t C_DIM     = lv_color_hex(0x98989F);
static const lv_color_t C_GREEN   = lv_color_hex(0x30D158);
static const lv_color_t C_RED     = lv_color_hex(0xFF453A);
static const lv_color_t C_BLUE    = lv_color_hex(0x0A84FF);
static const lv_color_t C_TEAL    = lv_color_hex(0x64D2FF);
static const lv_color_t C_PURPLE  = lv_color_hex(0xBF5AF2);
static const lv_color_t C_AMBER   = lv_color_hex(0xFF9F0A);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static Arduino_GFX* g_gfx = nullptr;
static Arduino_IIC* g_touch = nullptr;
static UiCallbacks g_cb = {};
static bool g_auto_auth = true;
static bool g_swap = false;

static lv_obj_t* scr_home = nullptr;
static lv_obj_t* scr_accounts = nullptr;
static lv_obj_t* scr_request = nullptr;
static lv_obj_t* scr_result = nullptr;
static lv_obj_t* scr_follows = nullptr;
static lv_obj_t* scr_profile = nullptr;

static lv_obj_t* g_acct_list = nullptr;
static lv_obj_t* g_auth_btn = nullptr;
static lv_obj_t* g_auth_icon = nullptr;
static lv_obj_t* g_auth_lbl = nullptr;
static lv_obj_t* g_modal = nullptr;      // delete-confirm overlay
static lv_obj_t* g_signing = nullptr;    // "Signing..." overlay
static lv_obj_t* g_toast_obj = nullptr;
static lv_timer_t* g_result_timer = nullptr;

static lv_obj_t* g_btn_sign = nullptr;
static lv_obj_t* g_btn_rej = nullptr;
static bool g_focus_sign = false;
static bool g_focus_active = false;

static bool g_link = false;
static bool g_home_stale = true;
static bool g_follows_stale = true;
static bool g_profile_stale = false;

// Follows list (parsed from the bridge-pushed /follows.json on FFat)
struct FollowEntry {
  String pk;
  String name;
  String lud16;
};
static std::vector<FollowEntry> g_follows;
static lv_obj_t* g_follows_list = nullptr;

// Profile page state
static String g_prof_pk, g_prof_name, g_prof_lud16;
static bool g_prof_self = false;
static int g_prof_tab = 0;                 // 0 npub, 1 btc, 2 zap
static lv_obj_t* g_prof_back_to = nullptr; // screen to return to

// ---------------------------------------------------------------------------
// Display + touch drivers
// ---------------------------------------------------------------------------
static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  if (g_swap) lv_draw_sw_rgb565_swap(px_map, w * h);
  g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
  lv_display_flush_ready(disp);
}

// Ghost-touch-hardened CST816T state machine. The controller emits spurious
// events (single blips with stale coordinates); a press is only reported to
// LVGL after >=2 interrupt events spanning >=40ms. 120ms of silence = lifted.
static bool tp_pressed = false;
static int16_t tp_x = 0, tp_y = 0;
static uint32_t tp_first = 0, tp_last = 0;
static int tp_events = 0;

static void pollTouchHW() {
  const uint32_t now = millis();
  if (now < 3000) {  // power-on ghost storm
    g_touch->IIC_Interrupt_Flag = false;
    return;
  }
  if (g_touch->IIC_Interrupt_Flag) {
    g_touch->IIC_Interrupt_Flag = false;
    int32_t n = g_touch->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (n > 0) {
      int16_t x = (int16_t)g_touch->IIC_Read_Device_Value(
          Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
      int16_t y = (int16_t)g_touch->IIC_Read_Device_Value(
          Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
      if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) {
        if (tp_events == 0) tp_first = now;
        tp_events++;
        tp_x = x;
        tp_y = y;
        tp_last = now;
      }
    }
  }
  if (tp_events > 0 && now - tp_last > 120) {  // finger lifted / ghost ended
    tp_events = 0;
    tp_pressed = false;
    return;
  }
  tp_pressed = (tp_events >= 2) && (tp_last - tp_first >= 25);
}

static void touch_read_cb(lv_indev_t*, lv_indev_data_t* data) {
  data->state = tp_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = tp_x;
  data->point.y = tp_y;
}

// ---------------------------------------------------------------------------
// Data helpers
// ---------------------------------------------------------------------------
static String displayName(int idx) {
  String n = profile::nameFor(keystore::pubHexOf(idx));
  if (n.length()) return n;
  return keystore::labelOf(idx);
}

static lv_color_t keyColor(int idx) {
  uint8_t pub[32] = {0};
  keystore::pubOf(idx, pub);
  static const uint32_t hues[8] = {0x0A84FF, 0xFF9F0A, 0x30D158, 0xFF375F,
                                   0xBF5AF2, 0x64D2FF, 0xFFD60A, 0xFF6B22};
  return lv_color_hex(hues[pub[0] & 7]);
}

// Avatar PNG cache (decoded lazily by LVGL's lodepng from raw PNG bytes)
struct AvCache {
  String key;
  uint8_t* buf = nullptr;
  size_t len = 0;
  lv_image_dsc_t dsc;
  bool valid = false;
};
// Sized for 8 accounts + a full follows page; when full we fall back to the
// initials disc instead of evicting (a live widget may still reference the
// evicted buffer — use-after-free).
static AvCache g_av[48];

static lv_image_dsc_t* avatarDsc(const String& pubhex) {
  const String k = pubhex.substring(0, 12);
  for (auto& e : g_av)
    if (e.valid && e.key == k) return &e.dsc;
  size_t n = 0;
  uint8_t* b = profile::readAvatar(pubhex, &n);
  if (!b) return nullptr;
  AvCache* slot = nullptr;
  for (auto& e : g_av)
    if (!e.valid) { slot = &e; break; }
  if (!slot) {
    free(b);
    return nullptr;
  }
  slot->key = k;
  slot->buf = b;
  slot->len = n;
  memset(&slot->dsc, 0, sizeof(slot->dsc));
  slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  slot->dsc.header.cf = LV_COLOR_FORMAT_RAW;
  slot->dsc.header.w = 96;
  slot->dsc.header.h = 96;
  slot->dsc.data = slot->buf;
  slot->dsc.data_size = slot->len;
  slot->valid = true;
  return &slot->dsc;
}

static void dropAvatarCache() {
  for (auto& e : g_av) {
    if (e.valid) {
      lv_image_cache_drop(&e.dsc);
      free(e.buf);
      e.buf = nullptr;
      e.valid = false;
    }
  }
}

// Avatar widget: cached kind-0 picture if we have one, else a colored disc
// with the entity's initial (Contacts-app style). Works for any pubkey.
static lv_obj_t* makeAvatarFor(lv_obj_t* parent, const String& pubhex,
                               const String& name, lv_color_t disc_color,
                               int size) {
  lv_image_dsc_t* dsc = avatarDsc(pubhex);
  if (dsc) {
    // clip_corner only clips CHILDREN, not a widget's own bitmap — so the
    // image must live inside a circular wrapper or it draws as a square.
    lv_obj_t* wrap = lv_obj_create(parent);
    lv_obj_set_size(wrap, size, size);
    lv_obj_set_style_radius(wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(wrap, true, 0);
    lv_obj_set_style_bg_color(wrap, C_CARD, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    const bool is_gif =
        dsc->data_size > 6 && memcmp(dsc->data, "GIF8", 4) == 0;
    lv_obj_t* img;
#if LV_USE_GIF
    if (is_gif) {
      img = lv_gif_create(wrap);
      lv_gif_set_src(img, dsc);
    } else
#endif
    {
      (void)is_gif;
      img = lv_image_create(wrap);
      lv_image_set_src(img, dsc);
    }
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(img, size, size);
    lv_obj_set_pos(img, 0, 0);
    return wrap;
  }
  lv_obj_t* disc = lv_obj_create(parent);
  lv_obj_set_size(disc, size, size);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(disc, disc_color, 0);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(disc, 0, 0);
  lv_obj_set_style_pad_all(disc, 0, 0);
  lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* lbl = lv_label_create(disc);
  char ini[2] = {name.length() ? (char)toupper(name[0]) : '?', 0};
  lv_label_set_text(lbl, ini);
  lv_obj_set_style_text_color(lbl, C_TEXT, 0);
  const lv_font_t* f = size >= 100   ? &lv_font_montserrat_44
                       : size >= 44 ? &lv_font_montserrat_24
                                    : &lv_font_montserrat_14;
  lv_obj_set_style_text_font(lbl, f, 0);
  lv_obj_center(lbl);
  return disc;
}

static lv_color_t hexColor(const String& pubhex) {
  static const uint32_t hues[8] = {0x0A84FF, 0xFF9F0A, 0x30D158, 0xFF375F,
                                   0xBF5AF2, 0x64D2FF, 0xFFD60A, 0xFF6B22};
  uint8_t h = pubhex.length() ? (uint8_t)pubhex[pubhex.length() - 1] : 0;
  return lv_color_hex(hues[h & 7]);
}

static lv_obj_t* makeAvatar(lv_obj_t* parent, int idx, int size) {
  return makeAvatarFor(parent, keystore::pubHexOf(idx), displayName(idx),
                       keyColor(idx), size);
}

// ---------------------------------------------------------------------------
// Shared widget helpers
// ---------------------------------------------------------------------------
static lv_obj_t* makeScreen() {
  lv_obj_t* s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, C_BG, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s, 0, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static lv_obj_t* plainCont(lv_obj_t* parent) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static lv_obj_t* roundIconBtn(lv_obj_t* parent, const char* symbol, int size,
                              lv_color_t bg, lv_color_t fg,
                              lv_event_cb_t cb, void* user) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_bg_color(b, lv_color_darken(bg, 60), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, symbol);
  lv_obj_set_style_text_color(l, fg, 0);
  lv_obj_set_style_text_font(l, size >= 80 ? &lv_font_montserrat_32
                                           : &lv_font_montserrat_20, 0);
  lv_obj_center(l);
  // Fingers are fat and the screen is small: accept touches well outside
  // the visible circle.
  lv_obj_set_ext_click_area(b, 16);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  return b;
}

void toast(const String& msg, uint32_t rgb) {
  if (g_toast_obj) {
    lv_obj_delete(g_toast_obj);
    g_toast_obj = nullptr;
  }
  lv_obj_t* t = lv_obj_create(lv_layer_top());
  g_toast_obj = t;
  lv_obj_set_height(t, 44);
  lv_obj_set_width(t, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(t, 22, 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(rgb), 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(t, 0, 0);
  lv_obj_set_style_pad_hor(t, 20, 0);
  lv_obj_set_style_pad_ver(t, 10, 0);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

  lv_obj_t* l = lv_label_create(t);
  lv_label_set_text(l, msg.c_str());
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_center(l);

  lv_timer_t* tm = lv_timer_create(
      [](lv_timer_t* tm) {
        lv_obj_t* obj = (lv_obj_t*)lv_timer_get_user_data(tm);
        if (obj == g_toast_obj) g_toast_obj = nullptr;
        lv_obj_delete(obj);
        lv_timer_delete(tm);
      },
      2000, t);
  lv_timer_set_repeat_count(tm, 1);
}

// ---------------------------------------------------------------------------
// Home screen
// ---------------------------------------------------------------------------
static void buildFollows();
static void showProfile(const String& pk, const String& name,
                        const String& lud16, bool self, lv_obj_t* back_to);

static void gotoAccounts() {
  refreshAccounts();  // rebuild rows with fresh data before showing
  lv_screen_load_anim(scr_accounts, LV_SCR_LOAD_ANIM_OVER_LEFT, 220, 0, false);
}

static void gotoHome() {
  lv_screen_load_anim(scr_home, LV_SCR_LOAD_ANIM_OVER_RIGHT, 220, 0, false);
}

static void gotoFollows() {
  if (g_follows_stale) buildFollows();
  lv_screen_load_anim(scr_follows, LV_SCR_LOAD_ANIM_OVER_LEFT, 220, 0, false);
}

// Full-width tappable menu row: icon disc + label (+ optional right-side
// detail) + chevron. Big target, pressed highlight, generous ext click area.
static lv_obj_t* menuRow(lv_obj_t* parent, int y, const char* symbol,
                         lv_color_t icon_fg, uint32_t icon_bg,
                         const char* text, const char* detail,
                         lv_event_cb_t cb) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, LCD_WIDTH - 28, 58);
  lv_obj_set_pos(row, 14, y);
  lv_obj_set_style_radius(row, 18, 0);
  lv_obj_set_style_bg_color(row, C_CARD, 0);
  lv_obj_set_style_bg_color(row, C_CARD_HI, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_hor(row, 14, 0);
  lv_obj_set_style_pad_ver(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_set_ext_click_area(row, 5);  // rows are 6px apart: stay disjoint

  lv_obj_t* disc = plainCont(row);
  lv_obj_set_size(disc, 38, 38);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(disc, lv_color_hex(icon_bg), 0);
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_t* ic = lv_label_create(disc);
  lv_label_set_text(ic, symbol);
  lv_obj_set_style_text_color(ic, icon_fg, 0);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
  lv_obj_center(ic);

  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lbl, C_TEXT, 0);
  lv_obj_set_flex_grow(lbl, 1);

  if (detail && detail[0]) {
    lv_obj_t* det = lv_label_create(row);
    lv_label_set_text(det, detail);
    lv_obj_set_style_text_font(det, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(det, C_DIM, 0);
  }

  lv_obj_t* chev = lv_label_create(row);
  lv_label_set_text(chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(chev, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(chev, C_DIM, 0);

  if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
  return row;
}

static void buildHome() {
  lv_obj_clean(scr_home);
  lv_obj_t* s = scr_home;

  const int count = keystore::count();
  const int active = keystore::activeIndex();

  // Linked ring + avatar (compact; the ring color doubles as link status)
  lv_obj_t* ring = plainCont(s);
  lv_obj_set_size(ring, 116, 116);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 3, 0);
  lv_obj_set_style_border_color(ring, g_link ? C_GREEN : C_CARD_HI, 0);
  lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 16);

  if (count > 0) {
    lv_obj_t* av = makeAvatar(s, active, 98);
    lv_obj_align(av, LV_ALIGN_TOP_MID, 0, 25);

    lv_obj_t* name = lv_label_create(s);
    lv_label_set_text(name, displayName(active).c_str());
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name, C_TEXT, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, LCD_WIDTH - 48);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 140);

    String np = keystore::npubOf(active);
    lv_obj_t* npub = lv_label_create(s);
    lv_label_set_text(npub, (np.substring(0, 14) + "..." + np.substring(np.length() - 4)).c_str());
    lv_obj_set_style_text_font(npub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(npub, C_DIM, 0);
    lv_obj_align(npub, LV_ALIGN_TOP_MID, 0, 172);
  } else {
    lv_obj_t* q = lv_label_create(s);
    lv_label_set_text(q, "?");
    lv_obj_set_style_text_font(q, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(q, C_DIM, 0);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_t* name = lv_label_create(s);
    lv_label_set_text(name, "No keys");
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name, C_TEXT, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 140);
  }

  // Status line: dot + text (link state + reach)
  lv_obj_t* st = plainCont(s);
  lv_obj_set_size(st, LV_SIZE_CONTENT, 24);
  lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(st, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(st, 7, 0);
  lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 196);
  lv_obj_t* dot = plainCont(st);
  lv_obj_set_size(dot, 9, 9);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, g_link ? C_GREEN : C_DIM, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_t* stl = lv_label_create(st);
  // NB: stick to ASCII — the built-in Montserrat range stops at 0x7F.
  lv_label_set_text(stl, g_link ? "Connected  -  10.77.7.1"
                                : "Plug in via USB-C");
  lv_obj_set_style_text_font(stl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(stl, g_link ? C_TEXT : C_DIM, 0);

  // Big tappable menu rows
  String accd = String(count);
  menuRow(s, 234, LV_SYMBOL_SETTINGS, C_BLUE, 0x10263F,
          "Accounts", accd.c_str(),
          [](lv_event_t*) { gotoAccounts(); });
  menuRow(s, 298, LV_SYMBOL_EYE_OPEN, C_TEAL, 0x0C2B38,
          "Following", nullptr,
          [](lv_event_t*) { gotoFollows(); });
  menuRow(s, 362, LV_SYMBOL_SD_CARD, C_AMBER, 0x3B2A0A,
          "Sign PSBT from SD", nullptr,
          [](lv_event_t*) { if (g_cb.signPsbtSD) g_cb.signPsbtSD(); });

  g_home_stale = false;
}

// ---------------------------------------------------------------------------
// Accounts screen
// ---------------------------------------------------------------------------
static void openDeleteModal(int idx);

static void buildAccountRows() {
  lv_obj_clean(g_acct_list);
  const int count = keystore::count();
  for (int i = 0; i < count; ++i) {
    const bool active = (i == keystore::activeIndex());
    lv_obj_t* row = lv_obj_create(g_acct_list);
    lv_obj_set_size(row, lv_pct(100), 80);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_bg_color(row, C_CARD, 0);
    lv_obj_set_style_bg_color(row, C_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(row, C_GREEN, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    makeAvatar(row, i, 54);

    lv_obj_t* col = plainCont(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);

    lv_obj_t* name = lv_label_create(col);
    lv_label_set_text(name, displayName(i).c_str());
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(name, C_TEXT, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(100));

    String np = keystore::npubOf(i);
    lv_obj_t* npub = lv_label_create(col);
    lv_label_set_text(npub, (np.substring(0, 16) + "...").c_str());
    lv_obj_set_style_text_font(npub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(npub, C_DIM, 0);

    if (active) {
      lv_obj_t* ok = lv_label_create(row);
      lv_label_set_text(ok, LV_SYMBOL_OK);
      lv_obj_set_style_text_color(ok, C_GREEN, 0);
      lv_obj_set_style_text_font(ok, &lv_font_montserrat_20, 0);
    }

    lv_obj_add_event_cb(row,
        [](lv_event_t* e) {
          int idx = (int)(intptr_t)lv_event_get_user_data(e);
          if (idx == keystore::activeIndex()) {
            // Tapping the already-active account opens its profile card.
            const String pk = keystore::pubHexOf(idx);
            showProfile(pk, displayName(idx), profile::lud16For(pk), true,
                        scr_accounts);
            return;
          }
          keystore::setActive(idx);
          buildAccountRows();
          g_home_stale = true;
        },
        LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    lv_obj_add_event_cb(row,
        [](lv_event_t* e) {
          openDeleteModal((int)(intptr_t)lv_event_get_user_data(e));
        },
        LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
  }
  if (count == 0) {
    lv_obj_t* empty = lv_label_create(g_acct_list);
    lv_label_set_text(empty, "No keys yet.\nAdd or import one below.");
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(empty, C_DIM, 0);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
  }
}

static void styleAuthBtn() {
  lv_obj_set_style_bg_color(g_auth_btn,
                            g_auto_auth ? lv_color_hex(0x3A2350) : C_CARD, 0);
  lv_obj_set_style_text_color(g_auth_icon, g_auto_auth ? C_PURPLE : C_DIM, 0);
  lv_label_set_text(g_auth_lbl, g_auto_auth ? "auth on" : "auth off");
  lv_obj_set_style_text_color(g_auth_lbl, g_auto_auth ? C_PURPLE : C_DIM, 0);
}

static void buildAccounts() {
  lv_obj_clean(scr_accounts);
  lv_obj_t* s = scr_accounts;

  // Header
  lv_obj_t* back = roundIconBtn(s, LV_SYMBOL_LEFT, 52, C_CARD, C_TEXT,
                                [](lv_event_t*) { gotoHome(); }, NULL);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 8);
  lv_obj_set_ext_click_area(back, 24);

  lv_obj_t* title = lv_label_create(s);
  lv_label_set_text(title, "Accounts");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, C_TEXT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  // Scrollable list (kinetic)
  g_acct_list = lv_obj_create(s);
  lv_obj_set_pos(g_acct_list, 0, 68);
  lv_obj_set_size(g_acct_list, LCD_WIDTH, 268);
  lv_obj_set_style_bg_opa(g_acct_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_acct_list, 0, 0);
  lv_obj_set_style_pad_hor(g_acct_list, 14, 0);
  lv_obj_set_style_pad_ver(g_acct_list, 4, 0);
  lv_obj_set_style_pad_row(g_acct_list, 10, 0);
  lv_obj_set_flex_flow(g_acct_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(g_acct_list, LV_SCROLLBAR_MODE_AUTO);
  buildAccountRows();

  // Bottom action bar: New / Import / Export / Auth
  struct Act { const char* sym; const char* lbl; };
  static const Act acts[4] = {{LV_SYMBOL_PLUS, "new"},
                              {LV_SYMBOL_DOWNLOAD, "import"},
                              {LV_SYMBOL_UPLOAD, "export"},
                              {LV_SYMBOL_CHARGE, "auth on"}};
  const int cxs[4] = {LCD_WIDTH / 2 - 138, LCD_WIDTH / 2 - 46,
                      LCD_WIDTH / 2 + 46, LCD_WIDTH / 2 + 138};
  for (int b = 0; b < 4; ++b) {
    lv_event_cb_t cb = nullptr;
    switch (b) {
      case 0:
        cb = [](lv_event_t*) {
          int r = keystore::generateKey("key " + String(keystore::count() + 1));
          if (r >= 0) {
            toast("Created key " + String(r + 1), 0x0E3A20);
            buildAccountRows();
            g_home_stale = true;
          } else {
            toast(r == -4 ? "Store full (8 max)" : "Failed", 0x59201C);
          }
        };
        break;
      case 1: cb = [](lv_event_t*) { if (g_cb.importSD) g_cb.importSD(); }; break;
      case 2: cb = [](lv_event_t*) { if (g_cb.exportSD) g_cb.exportSD(); }; break;
      case 3:
        cb = [](lv_event_t*) {
          g_auto_auth = !g_auto_auth;
          if (g_cb.setAutoAuth) g_cb.setAutoAuth(g_auto_auth);
          styleAuthBtn();
          toast(g_auto_auth ? "Relay auth: automatic" : "Relay auth: ask me",
                0x3A2350);
        };
        break;
    }
    lv_color_t fg = (b == 0) ? C_BLUE : C_TEXT;
    lv_obj_t* btn = roundIconBtn(s, acts[b].sym, 66, C_CARD, fg, cb, NULL);
    lv_obj_set_ext_click_area(btn, 12);  // keep neighbors' hit areas apart
    lv_obj_align(btn, LV_ALIGN_TOP_MID, cxs[b] - LCD_WIDTH / 2, 340);
    lv_obj_t* lb = lv_label_create(s);
    lv_label_set_text(lb, acts[b].lbl);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lb, C_DIM, 0);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, cxs[b] - LCD_WIDTH / 2, 412);
    if (b == 3) {
      g_auth_btn = btn;
      g_auth_icon = lv_obj_get_child(btn, 0);
      g_auth_lbl = lb;
      styleAuthBtn();
    }
  }
}

// Delete confirmation modal
static void openDeleteModal(int idx) {
  if (g_modal) return;
  g_modal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_modal, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos(g_modal, 0, 0);
  lv_obj_set_style_bg_color(g_modal, C_BG, 0);
  lv_obj_set_style_bg_opa(g_modal, LV_OPA_70, 0);
  lv_obj_set_style_border_width(g_modal, 0, 0);
  lv_obj_set_style_radius(g_modal, 0, 0);
  lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* card = lv_obj_create(g_modal);
  lv_obj_set_size(card, LCD_WIDTH - 56, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(card, 24, 0);
  lv_obj_set_style_bg_color(card, C_CARD, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 22, 0);
  lv_obj_center(card);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card, 12, 0);

  makeAvatar(card, idx, 56);

  lv_obj_t* t = lv_label_create(card);
  lv_label_set_text(t, ("Delete \"" + displayName(idx) + "\"?").c_str());
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, C_TEXT, 0);

  lv_obj_t* w = lv_label_create(card);
  lv_label_set_text(w, "This cannot be undone");
  lv_obj_set_style_text_font(w, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(w, C_DIM, 0);

  lv_obj_t* rowb = plainCont(card);
  lv_obj_set_size(rowb, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rowb, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowb, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t* del = lv_button_create(rowb);
  lv_obj_set_size(del, 118, 48);
  lv_obj_set_style_radius(del, 24, 0);
  lv_obj_set_style_bg_color(del, C_RED, 0);
  lv_obj_set_style_shadow_width(del, 0, 0);
  lv_obj_t* dl = lv_label_create(del);
  lv_label_set_text(dl, "Delete");
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_16, 0);
  lv_obj_center(dl);
  lv_obj_add_event_cb(del,
      [](lv_event_t* e) {
        int idx = (int)(intptr_t)lv_event_get_user_data(e);
        String nm = displayName(idx);
        if (keystore::removeKey(idx)) toast("Deleted " + nm, 0x59201C);
        lv_obj_delete(g_modal);
        g_modal = nullptr;
        buildAccountRows();
        g_home_stale = true;
      },
      LV_EVENT_CLICKED, (void*)(intptr_t)idx);

  lv_obj_t* can = lv_button_create(rowb);
  lv_obj_set_size(can, 118, 48);
  lv_obj_set_style_radius(can, 24, 0);
  lv_obj_set_style_bg_color(can, C_CARD_HI, 0);
  lv_obj_set_style_shadow_width(can, 0, 0);
  lv_obj_t* cl = lv_label_create(can);
  lv_label_set_text(cl, "Cancel");
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
  lv_obj_center(cl);
  lv_obj_add_event_cb(can,
      [](lv_event_t*) {
        lv_obj_delete(g_modal);
        g_modal = nullptr;
      },
      LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------------------
// Follows screen
// ---------------------------------------------------------------------------
static void loadFollows() {
  g_follows.clear();
  String json = profile::getFollowsJson();
  if (!json.length()) return;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  for (JsonObject f : doc["follows"].as<JsonArray>()) {
    FollowEntry e;
    e.pk = String((const char*)(f["pk"] | ""));
    if (e.pk.length() != 64) continue;
    e.name = String((const char*)(f["name"] | ""));
    e.lud16 = String((const char*)(f["lud16"] | ""));
    g_follows.push_back(e);
  }
}

static void buildFollows() {
  loadFollows();
  lv_obj_clean(scr_follows);
  g_follows_list = nullptr;
  lv_obj_t* s = scr_follows;

  lv_obj_t* back = roundIconBtn(s, LV_SYMBOL_LEFT, 52, C_CARD, C_TEXT,
                                [](lv_event_t*) { gotoHome(); }, NULL);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 8);
  lv_obj_set_ext_click_area(back, 24);

  lv_obj_t* title = lv_label_create(s);
  lv_label_set_text(title, "Following");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, C_TEXT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  if (g_follows.empty()) {
    lv_obj_t* empty = lv_label_create(s);
    lv_label_set_text(empty,
        "No follows synced yet.\n\nOpen http://10.77.7.1/bridge\n"
        "on the connected phone or\nlaptop and it will sync your\n"
        "follow list automatically.");
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(empty, C_DIM, 0);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    g_follows_stale = false;
    return;
  }

  g_follows_list = lv_obj_create(s);
  lv_obj_set_pos(g_follows_list, 0, 68);
  lv_obj_set_size(g_follows_list, LCD_WIDTH, LCD_HEIGHT - 76);
  lv_obj_set_style_bg_opa(g_follows_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_follows_list, 0, 0);
  lv_obj_set_style_pad_hor(g_follows_list, 14, 0);
  lv_obj_set_style_pad_ver(g_follows_list, 4, 0);
  lv_obj_set_style_pad_row(g_follows_list, 10, 0);
  lv_obj_set_flex_flow(g_follows_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(g_follows_list, LV_SCROLLBAR_MODE_AUTO);

  for (size_t i = 0; i < g_follows.size(); ++i) {
    const FollowEntry& fe = g_follows[i];
    lv_obj_t* row = lv_obj_create(g_follows_list);
    lv_obj_set_size(row, lv_pct(100), 76);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_bg_color(row, C_CARD, 0);
    lv_obj_set_style_bg_color(row, C_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    makeAvatarFor(row, fe.pk, fe.name, hexColor(fe.pk), 52);

    lv_obj_t* col = plainCont(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);

    lv_obj_t* name = lv_label_create(col);
    lv_label_set_text(name, fe.name.length() ? fe.name.c_str()
                                             : (fe.pk.substring(0, 12) + "...").c_str());
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(name, C_TEXT, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(100));

    lv_obj_t* sub = lv_label_create(col);
    lv_label_set_text(sub, fe.lud16.length() ? (LV_SYMBOL_CHARGE "  zappable")
                                             : "nostr");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, fe.lud16.length() ? C_AMBER : C_DIM, 0);

    lv_obj_t* chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, C_DIM, 0);

    lv_obj_add_event_cb(row,
        [](lv_event_t* e) {
          size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
          if (idx >= g_follows.size()) return;
          const FollowEntry& fe = g_follows[idx];
          showProfile(fe.pk, fe.name, fe.lud16, false, scr_follows);
        },
        LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i);
  }
  g_follows_stale = false;
}

// ---------------------------------------------------------------------------
// Profile page: big avatar + name + about + QR carousel (npub / BTC / zap)
// ---------------------------------------------------------------------------
static const int QR_CANVAS = 224;
static uint8_t* g_qr_buf = nullptr;  // QR_CANVAS^2 RGB565, PSRAM, alloc once

static bool hexToBytes(const String& hexs, uint8_t out[32]) {
  if (hexs.length() != 64) return false;
  for (int i = 0; i < 32; ++i) {
    char hi = hexs[i * 2], lo = hexs[i * 2 + 1];
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = nib(hi), l = nib(lo);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

// esp_qrcode reports the finished code through a callback; paint it into the
// canvas buffer from there (white card, black modules, centered + scaled).
static void qrPaintCb(esp_qrcode_handle_t qr) {
  if (!g_qr_buf) return;
  uint16_t* px = (uint16_t*)g_qr_buf;
  const int size = esp_qrcode_get_size(qr);
  if (size <= 0) return;
  const int quiet = 10;
  int scale = (QR_CANVAS - 2 * quiet) / size;
  if (scale < 1) scale = 1;
  const int off = (QR_CANVAS - scale * size) / 2;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (!esp_qrcode_get_module(qr, x, y)) continue;
      for (int dy = 0; dy < scale; ++dy) {
        uint16_t* rowp = px + (off + y * scale + dy) * QR_CANVAS + off + x * scale;
        for (int dx = 0; dx < scale; ++dx) rowp[dx] = 0x0000;
      }
    }
  }
}

// Renders `text` as a QR into the canvas buffer.
static bool drawQr(lv_obj_t* canvas, const char* text) {
  if (!g_qr_buf) return false;
  uint16_t* px = (uint16_t*)g_qr_buf;
  for (int i = 0; i < QR_CANVAS * QR_CANVAS; ++i) px[i] = 0xFFFF;

  bool ok = false;
  if (text && text[0]) {
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qrPaintCb;
    cfg.max_qrcode_version = 14;  // LNURLs can get long
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    ok = esp_qrcode_generate(&cfg, text) == ESP_OK;
  }
  lv_obj_invalidate(canvas);
  return ok;
}

static lv_obj_t* g_prof_canvas = nullptr;
static lv_obj_t* g_prof_payload = nullptr;
static lv_obj_t* g_prof_tabs[3] = {nullptr, nullptr, nullptr};

// QR payloads, computed lazily per tab (taproot derivation takes ~100ms).
static String g_qr_cached[3];

static void profShowTab(int tab) {
  g_prof_tab = tab;
  uint8_t pk[32];
  const bool pk_ok = hexToBytes(g_prof_pk, pk);

  String qr_text, show_text;
  if (tab == 0) {
    if (!g_qr_cached[0].length() && pk_ok)
      g_qr_cached[0] = nostrocrypto::bech32_encode_32("npub", pk);
    qr_text = g_qr_cached[0];
    show_text = qr_text;
  } else if (tab == 1) {
    if (!g_qr_cached[1].length() && pk_ok)
      g_qr_cached[1] = taproot::addressFromXonly(pk);
    show_text = g_qr_cached[1];
    qr_text = g_qr_cached[1];
    qr_text.toUpperCase();  // alphanumeric mode: denser, easier to scan
  } else {
    if (!g_qr_cached[2].length())
      g_qr_cached[2] = taproot::lnurlFromLud16(g_prof_lud16);
    qr_text = g_qr_cached[2];
    show_text = g_prof_lud16;
  }

  if (qr_text.length()) {
    drawQr(g_prof_canvas, qr_text.c_str());
    String t = show_text.length() > 30
                   ? show_text.substring(0, 20) + "..." +
                         show_text.substring(show_text.length() - 8)
                   : show_text;
    lv_label_set_text(g_prof_payload, t.c_str());
  } else {
    drawQr(g_prof_canvas, "");
    lv_label_set_text(g_prof_payload, "unavailable");
  }

  for (int i = 0; i < 3; ++i) {
    if (!g_prof_tabs[i]) continue;
    const bool on = (i == tab);
    lv_obj_set_style_bg_color(g_prof_tabs[i], on ? C_BLUE : C_CARD, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(g_prof_tabs[i], 0),
                                on ? C_TEXT : C_DIM, 0);
  }
}

static void buildProfile() {
  lv_obj_clean(scr_profile);
  g_prof_canvas = nullptr;
  g_prof_payload = nullptr;
  g_prof_tabs[0] = g_prof_tabs[1] = g_prof_tabs[2] = nullptr;
  lv_obj_t* s = scr_profile;

  lv_obj_t* back = roundIconBtn(s, LV_SYMBOL_LEFT, 48, C_CARD, C_TEXT,
      [](lv_event_t*) {
        lv_screen_load_anim(g_prof_back_to ? g_prof_back_to : scr_home,
                            LV_SCR_LOAD_ANIM_OVER_RIGHT, 220, 0, false);
      },
      NULL);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 8);
  lv_obj_set_ext_click_area(back, 24);

  lv_obj_t* av = makeAvatarFor(s, g_prof_pk, g_prof_name,
                               hexColor(g_prof_pk), 56);
  lv_obj_align(av, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* name = lv_label_create(s);
  lv_label_set_text(name, g_prof_name.length() ? g_prof_name.c_str()
                                               : "(no name)");
  lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(name, C_TEXT, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, LCD_WIDTH - 40);
  lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 70);

  const String about = profile::aboutFor(g_prof_pk);
  lv_obj_t* ab = lv_label_create(s);
  lv_label_set_text(ab, about.c_str());
  lv_obj_set_style_text_font(ab, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(ab, C_DIM, 0);
  lv_label_set_long_mode(ab, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ab, LCD_WIDTH - 48);
  lv_obj_set_style_text_align(ab, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_height(ab, about.length() ? 34 : 0);
  lv_obj_align(ab, LV_ALIGN_TOP_MID, 0, 98);

  // QR card
  if (!g_qr_buf) {
    g_qr_buf = (uint8_t*)ps_malloc(QR_CANVAS * QR_CANVAS * 2);
    if (!g_qr_buf) g_qr_buf = (uint8_t*)malloc(QR_CANVAS * QR_CANVAS * 2);
  }
  g_prof_canvas = lv_canvas_create(s);
  lv_canvas_set_buffer(g_prof_canvas, g_qr_buf, QR_CANVAS, QR_CANVAS,
                       LV_COLOR_FORMAT_RGB565);
  lv_obj_set_style_radius(g_prof_canvas, 18, 0);
  lv_obj_set_style_clip_corner(g_prof_canvas, true, 0);
  lv_obj_align(g_prof_canvas, LV_ALIGN_TOP_MID, 0,
               about.length() ? 138 : 112);

  g_prof_payload = lv_label_create(s);
  lv_obj_set_style_text_font(g_prof_payload, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(g_prof_payload, C_DIM, 0);
  lv_obj_align(g_prof_payload, LV_ALIGN_BOTTOM_MID, 0, -66);

  // Tab pills
  static const char* tabs[3] = {"npub", "bitcoin", "zap"};
  const bool has_zap = g_prof_lud16.length() > 0;
  const int ntabs = has_zap ? 3 : 2;
  const int tw = 104, gap = 10;
  const int total = ntabs * tw + (ntabs - 1) * gap;
  for (int i = 0; i < ntabs; ++i) {
    lv_obj_t* b = lv_button_create(s);
    lv_obj_set_size(b, tw, 44);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, tabs[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID,
                 -total / 2 + tw / 2 + i * (tw + gap), -12);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b,
        [](lv_event_t* e) {
          profShowTab((int)(intptr_t)lv_event_get_user_data(e));
        },
        LV_EVENT_CLICKED, (void*)(intptr_t)i);
    g_prof_tabs[i] = b;
  }

  profShowTab(g_prof_tab < ntabs ? g_prof_tab : 0);
  g_profile_stale = false;
}

static void showProfile(const String& pk, const String& name,
                        const String& lud16, bool self, lv_obj_t* back_to) {
  g_prof_pk = pk;
  g_prof_name = name;
  g_prof_lud16 = lud16;
  g_prof_self = self;
  g_prof_tab = 0;
  g_prof_back_to = back_to;
  g_qr_cached[0] = g_qr_cached[1] = g_qr_cached[2] = "";
  buildProfile();
  lv_screen_load_anim(scr_profile, LV_SCR_LOAD_ANIM_OVER_LEFT, 220, 0, false);
}

// ---------------------------------------------------------------------------
// Request screen
// ---------------------------------------------------------------------------
static const char* kindName(int k) {
  switch (k) {
    case 0:     return "Profile Update";
    case 1:     return "Public Note";
    case 3:     return "Follow List";
    case 4:     return "Direct Message";
    case 5:     return "Delete Request";
    case 6:     return "Repost";
    case 7:     return "Reaction";
    case 1059:  return "Gift Wrap";
    case 9734:  return "Zap Request";
    case 9735:  return "Zap Receipt";
    case 10002: return "Relay List";
    case 22242: return "Relay Auth";
    case 30023: return "Article";
    default:    return nullptr;
  }
}

static void kindColors(int k, lv_color_t* fg, uint32_t* bg) {
  switch (k) {
    case 1: case 6: case 7:      *fg = C_TEAL;   *bg = 0x0C2B38; break;
    case 0: case 3: case 10002:  *fg = C_AMBER;  *bg = 0x3B2A0A; break;
    case 4: case 1059:           *fg = lv_color_hex(0xFF6482); *bg = 0x3A1420; break;
    case 22242:                  *fg = C_PURPLE; *bg = 0x2C1840; break;
    case 9734: case 9735:        *fg = C_AMBER;  *bg = 0x3B2A0A; break;
    default:                     *fg = C_TEXT;   *bg = 0x2C2C2E; break;
  }
}

// The request screen doubles as the Bitcoin-transaction review screen; this
// flag routes approve/decline to the right callback.
static bool g_req_is_psbt = false;

static void dispatchDecide(bool approve) {
  if (g_req_is_psbt) {
    if (g_cb.decidePsbt) g_cb.decidePsbt(approve);
  } else if (g_cb.decide) {
    g_cb.decide(approve);
  }
}

static void applyFocusRing() {
  if (!g_btn_sign || !g_btn_rej) return;
  lv_obj_set_style_outline_width(g_btn_sign, 0, 0);
  lv_obj_set_style_outline_width(g_btn_rej, 0, 0);
  if (!g_focus_active) return;
  lv_obj_t* f = g_focus_sign ? g_btn_sign : g_btn_rej;
  lv_obj_set_style_outline_width(f, 3, 0);
  lv_obj_set_style_outline_color(f, C_TEXT, 0);
  lv_obj_set_style_outline_pad(f, 5, 0);
}

void showRequest(uint32_t id, int kind, const String& content, int queued_behind) {
  (void)id;
  g_req_is_psbt = false;
  g_focus_active = false;
  g_focus_sign = false;
  lv_obj_clean(scr_request);
  lv_obj_t* s = scr_request;

  // Header
  lv_obj_t* hdr = lv_label_create(s);
  lv_label_set_text(hdr, "Sign Request");
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(hdr, C_AMBER, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 18);

  if (queued_behind > 0) {
    lv_obj_t* badge = plainCont(s);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(badge, 15, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x3B2A0A), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(badge, 12, 0);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -14, 14);
    lv_obj_t* bl = lv_label_create(badge);
    lv_label_set_text(bl, ("+" + String(queued_behind)).c_str());
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, C_AMBER, 0);
    lv_obj_center(bl);
  }

  // Kind chip
  lv_color_t kfg;
  uint32_t kbg;
  kindColors(kind, &kfg, &kbg);
  const char* kn = kindName(kind);
  lv_obj_t* chip = plainCont(s);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, 44);
  lv_obj_set_style_radius(chip, 22, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(kbg), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(chip, 20, 0);
  lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_t* cl = lv_label_create(chip);
  lv_label_set_text(cl, kn ? kn : ("Kind " + String(kind)).c_str());
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(cl, kfg, 0);
  lv_obj_center(cl);

  // Signing-as row
  if (keystore::count() > 0) {
    const int a = keystore::activeIndex();
    lv_obj_t* as = plainCont(s);
    lv_obj_set_size(as, LV_SIZE_CONTENT, 32);
    lv_obj_set_flex_flow(as, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(as, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(as, 8, 0);
    lv_obj_align(as, LV_ALIGN_TOP_MID, 0, 106);
    makeAvatar(as, a, 26);
    lv_obj_t* al = lv_label_create(as);
    lv_label_set_text(al, displayName(a).c_str());
    lv_obj_set_style_text_font(al, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(al, C_DIM, 0);
  }

  // Content card (scrollable if long)
  lv_obj_t* card = lv_obj_create(s);
  lv_obj_set_pos(card, 16, 146);
  lv_obj_set_size(card, LCD_WIDTH - 32, 168);
  lv_obj_set_style_radius(card, 20, 0);
  lv_obj_set_style_bg_color(card, C_CARD, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_t* body = lv_label_create(card);
  lv_label_set_text(body, content.length() ? content.c_str() : "(no content)");
  lv_obj_set_width(body, lv_pct(100));
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(body, C_TEXT, 0);

  // Call-style round buttons
  g_btn_sign = roundIconBtn(s, LV_SYMBOL_OK, 88, lv_color_hex(0x0E3A20), C_GREEN,
      [](lv_event_t*) { dispatchDecide(true); }, NULL);
  lv_obj_align(g_btn_sign, LV_ALIGN_BOTTOM_MID, -88, -44);
  g_btn_rej = roundIconBtn(s, LV_SYMBOL_CLOSE, 88, lv_color_hex(0x451512), C_RED,
      [](lv_event_t*) { dispatchDecide(false); }, NULL);
  lv_obj_align(g_btn_rej, LV_ALIGN_BOTTOM_MID, 88, -44);

  lv_obj_t* sl = lv_label_create(s);
  lv_label_set_text(sl, "Sign");
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sl, C_DIM, 0);
  lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, -88, -18);
  lv_obj_t* rl = lv_label_create(s);
  lv_label_set_text(rl, "Reject");
  lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(rl, C_DIM, 0);
  lv_obj_align(rl, LV_ALIGN_BOTTOM_MID, 88, -18);

  if (g_modal) { lv_obj_delete(g_modal); g_modal = nullptr; }
  lv_screen_load(scr_request);
}

void clearRequest() {
  if (lv_screen_active() == scr_request && !g_req_is_psbt) {
    buildHome();
    lv_screen_load(scr_home);
  }
}

// ---------------------------------------------------------------------------
// Bitcoin transaction review (PSBT)
// ---------------------------------------------------------------------------
void showPsbt(const String& amount, const String& body) {
  g_req_is_psbt = true;
  g_focus_active = false;
  g_focus_sign = false;
  lv_obj_clean(scr_request);
  lv_obj_t* s = scr_request;

  lv_obj_t* hdr = lv_label_create(s);
  lv_label_set_text(hdr, "Bitcoin Transaction");
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(hdr, C_AMBER, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 18);

  // Headline amount chip
  lv_obj_t* chip = plainCont(s);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, 46);
  lv_obj_set_style_radius(chip, 23, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(0x3B2A0A), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(chip, 22, 0);
  lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_t* cl = lv_label_create(chip);
  lv_label_set_text(cl, amount.c_str());
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(cl, C_AMBER, 0);
  lv_obj_center(cl);

  // Signing-as row
  if (keystore::count() > 0) {
    const int a = keystore::activeIndex();
    lv_obj_t* as = plainCont(s);
    lv_obj_set_size(as, LV_SIZE_CONTENT, 32);
    lv_obj_set_flex_flow(as, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(as, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(as, 8, 0);
    lv_obj_align(as, LV_ALIGN_TOP_MID, 0, 104);
    makeAvatar(as, a, 26);
    lv_obj_t* al = lv_label_create(as);
    lv_label_set_text(al, ("spend from " + displayName(a)).c_str());
    lv_obj_set_style_text_font(al, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(al, C_DIM, 0);
  }

  // Breakdown card
  lv_obj_t* card = lv_obj_create(s);
  lv_obj_set_pos(card, 16, 142);
  lv_obj_set_size(card, LCD_WIDTH - 32, 172);
  lv_obj_set_style_radius(card, 20, 0);
  lv_obj_set_style_bg_color(card, C_CARD, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_t* bodyl = lv_label_create(card);
  lv_label_set_text(bodyl, body.c_str());
  lv_obj_set_width(bodyl, lv_pct(100));
  lv_label_set_long_mode(bodyl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(bodyl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bodyl, C_TEXT, 0);

  g_btn_sign = roundIconBtn(s, LV_SYMBOL_OK, 88, lv_color_hex(0x0E3A20), C_GREEN,
      [](lv_event_t*) { dispatchDecide(true); }, NULL);
  lv_obj_align(g_btn_sign, LV_ALIGN_BOTTOM_MID, -88, -44);
  g_btn_rej = roundIconBtn(s, LV_SYMBOL_CLOSE, 88, lv_color_hex(0x451512), C_RED,
      [](lv_event_t*) { dispatchDecide(false); }, NULL);
  lv_obj_align(g_btn_rej, LV_ALIGN_BOTTOM_MID, 88, -44);

  lv_obj_t* sl = lv_label_create(s);
  lv_label_set_text(sl, "Sign");
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sl, C_DIM, 0);
  lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, -88, -18);
  lv_obj_t* rl = lv_label_create(s);
  lv_label_set_text(rl, "Reject");
  lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(rl, C_DIM, 0);
  lv_obj_align(rl, LV_ALIGN_BOTTOM_MID, 88, -18);

  if (g_modal) { lv_obj_delete(g_modal); g_modal = nullptr; }
  lv_screen_load(scr_request);
}

bool psbtScreenActive() {
  return lv_screen_active() == scr_request && g_req_is_psbt;
}

// ---------------------------------------------------------------------------
// Signed-payload QR screen (single or animated NTX frames)
// ---------------------------------------------------------------------------
static lv_obj_t* scr_txqr = nullptr;
static lv_obj_t* g_txqr_canvas = nullptr;
static lv_obj_t* g_txqr_counter = nullptr;
static lv_timer_t* g_txqr_timer = nullptr;
static String g_txqr_payload;
static int g_txqr_frame = 0, g_txqr_total = 1;
// Fits comfortably in QR version 14 (low EC, byte mode ~460 chars) with
// margin for the "NTX:i:n:" prefix and reliable camera reads on a 224px code.
static const int TXQR_CHUNK = 360;

static void txqrPaintFrame() {
  if (!g_txqr_canvas) return;
  String data;
  if (g_txqr_total <= 1) {
    data = g_txqr_payload;
  } else {
    const int a = g_txqr_frame * TXQR_CHUNK;
    const int b = min((int)g_txqr_payload.length(), a + TXQR_CHUNK);
    data = "NTX:" + String(g_txqr_frame) + ":" + String(g_txqr_total) + ":" +
           g_txqr_payload.substring(a, b);
    if (g_txqr_counter)
      lv_label_set_text(g_txqr_counter,
          ("frame " + String(g_txqr_frame + 1) + " / " + String(g_txqr_total)).c_str());
  }
  drawQr(g_txqr_canvas, data.c_str());
}

void showTxQr(const String& payload, const String& subtitle) {
  if (g_signing) { lv_obj_delete(g_signing); g_signing = nullptr; }
  if (g_txqr_timer) { lv_timer_delete(g_txqr_timer); g_txqr_timer = nullptr; }
  if (!scr_txqr) {
    scr_txqr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_txqr, lv_color_hex(0x000000), 0);
  }
  lv_obj_clean(scr_txqr);
  g_txqr_canvas = nullptr;
  g_txqr_counter = nullptr;

  g_txqr_payload = payload;
  g_txqr_frame = 0;
  g_txqr_total = payload.length() <= 450
      ? 1 : (payload.length() + TXQR_CHUNK - 1) / TXQR_CHUNK;

  lv_obj_t* hdr = lv_label_create(scr_txqr);
  lv_label_set_text(hdr, "Signed");
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(hdr, C_GREEN, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t* sub = lv_label_create(scr_txqr);
  lv_label_set_text(sub, subtitle.c_str());
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sub, C_DIM, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 48);

  if (!g_qr_buf) {
    g_qr_buf = (uint8_t*)ps_malloc(QR_CANVAS * QR_CANVAS * 2);
    if (!g_qr_buf) g_qr_buf = (uint8_t*)malloc(QR_CANVAS * QR_CANVAS * 2);
  }
  g_txqr_canvas = lv_canvas_create(scr_txqr);
  lv_canvas_set_buffer(g_txqr_canvas, g_qr_buf, QR_CANVAS, QR_CANVAS,
                       LV_COLOR_FORMAT_RGB565);
  lv_obj_set_style_radius(g_txqr_canvas, 18, 0);
  lv_obj_set_style_clip_corner(g_txqr_canvas, true, 0);
  lv_obj_align(g_txqr_canvas, LV_ALIGN_CENTER, 0, -6);

  g_txqr_counter = lv_label_create(scr_txqr);
  lv_label_set_text(g_txqr_counter,
      g_txqr_total > 1 ? "frame 1 / ..." : "scan with NostrTX");
  lv_obj_set_style_text_font(g_txqr_counter, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_txqr_counter, C_DIM, 0);
  lv_obj_align(g_txqr_counter, LV_ALIGN_CENTER, 0, 124);

  lv_obj_t* done = lv_button_create(scr_txqr);
  lv_obj_set_size(done, 180, 52);
  lv_obj_set_style_radius(done, 26, 0);
  lv_obj_set_style_bg_color(done, lv_color_hex(0x2C2C2E), 0);
  lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_t* dl = lv_label_create(done);
  lv_label_set_text(dl, "Done");
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(dl, C_TEXT, 0);
  lv_obj_center(dl);
  lv_obj_add_event_cb(done,
      [](lv_event_t*) {
        if (g_txqr_timer) { lv_timer_delete(g_txqr_timer); g_txqr_timer = nullptr; }
        g_txqr_payload = "";
        buildHome();
        lv_screen_load(scr_home);
      },
      LV_EVENT_SHORT_CLICKED, NULL);

  txqrPaintFrame();
  lv_screen_load(scr_txqr);

  if (g_txqr_total > 1) {
    g_txqr_timer = lv_timer_create(
        [](lv_timer_t*) {
          g_txqr_frame = (g_txqr_frame + 1) % g_txqr_total;
          txqrPaintFrame();
        },
        800, NULL);
  }
}

// ---------------------------------------------------------------------------
// Signing overlay + result
// ---------------------------------------------------------------------------
void showSigningOverlay() {
  if (g_signing) return;
  g_signing = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_signing, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos(g_signing, 0, 0);
  lv_obj_set_style_bg_color(g_signing, C_BG, 0);
  lv_obj_set_style_bg_opa(g_signing, LV_OPA_80, 0);
  lv_obj_set_style_border_width(g_signing, 0, 0);
  lv_obj_set_style_radius(g_signing, 0, 0);
  lv_obj_clear_flag(g_signing, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* sp = lv_spinner_create(g_signing);
  lv_obj_set_size(sp, 90, 90);
  lv_obj_align(sp, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_arc_color(sp, C_AMBER, LV_PART_INDICATOR);

  lv_obj_t* l = lv_label_create(g_signing);
  lv_label_set_text(l, "Signing...");
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, 46);

  lv_refr_now(NULL);  // paint before the CPU disappears into BIP-340
}

void showResult(bool approved, int remaining) {
  if (g_signing) { lv_obj_delete(g_signing); g_signing = nullptr; }
  lv_obj_clean(scr_result);
  lv_obj_t* s = scr_result;

  lv_obj_t* circ = plainCont(s);
  lv_obj_set_size(circ, 132, 132);
  lv_obj_set_style_radius(circ, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(circ, lv_color_hex(approved ? 0x0E3A20 : 0x451512), 0);
  lv_obj_set_style_bg_opa(circ, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(circ, 3, 0);
  lv_obj_set_style_border_color(circ, approved ? C_GREEN : C_RED, 0);
  lv_obj_align(circ, LV_ALIGN_CENTER, 0, -66);
  lv_obj_t* sym = lv_label_create(circ);
  lv_label_set_text(sym, approved ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(sym, &lv_font_montserrat_44, 0);
  lv_obj_set_style_text_color(sym, approved ? C_GREEN : C_RED, 0);
  lv_obj_center(sym);

  lv_obj_t* t = lv_label_create(s);
  lv_label_set_text(t, approved ? "Signed" : "Rejected");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(t, C_TEXT, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 30);

  lv_obj_t* sub = lv_label_create(s);
  if (remaining > 0)
    lv_label_set_text(sub, (String(remaining) + " more waiting").c_str());
  else
    lv_label_set_text(sub, approved ? "Signature sent over USB" : "Request dismissed");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sub, remaining > 0 ? C_AMBER : C_DIM, 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 70);

  // Pop-in animation on the circle
  lv_obj_set_style_transform_scale(circ, 160, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, circ);
  lv_anim_set_values(&a, 160, 256);
  lv_anim_set_duration(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_set_exec_cb(&a, [](void* var, int32_t v) {
    lv_obj_set_style_transform_scale((lv_obj_t*)var, v, 0);
  });
  lv_anim_start(&a);

  lv_screen_load(scr_result);

  if (g_result_timer) { lv_timer_delete(g_result_timer); g_result_timer = nullptr; }
  g_result_timer = lv_timer_create(
      [](lv_timer_t* tm) {
        lv_timer_delete(tm);
        g_result_timer = nullptr;
        // Return home; if more requests are queued the sketch will call
        // showRequest() again on its next tick.
        if (lv_screen_active() == scr_result) {
          buildHome();
          lv_screen_load(scr_home);
        }
      },
      1100, NULL);
  lv_timer_set_repeat_count(g_result_timer, 1);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void setLink(bool up) {
  if (up == g_link) return;
  g_link = up;
  g_home_stale = true;
}

static bool g_avatars_dirty = false;

void refreshAccounts() {
  // Freeing avatar buffers while the request/result screens render from them
  // is a use-after-free; defer the cache drop until we're on a safe screen.
  lv_obj_t* act = lv_screen_active();
  if (act == scr_request || act == scr_result) {
    g_avatars_dirty = true;
  } else {
    dropAvatarCache();
    if (act == scr_profile) g_profile_stale = true;
  }
  if (g_acct_list) buildAccountRows();
  g_home_stale = true;
  g_follows_stale = true;
}

void refreshFollows() {
  g_follows_stale = true;
  toast("Follows synced", 0x0C2B38);
}

void buttonShort() {
  lv_obj_t* act = lv_screen_active();
  if (act == scr_request) {
    g_focus_active = true;
    g_focus_sign = !g_focus_sign;
    applyFocusRing();
  } else if (act == scr_home) {
    gotoAccounts();
  } else if (act == scr_accounts) {
    if (g_modal) { lv_obj_delete(g_modal); g_modal = nullptr; return; }
    // Scroll the list one "page"; wrap to top at the end.
    if (g_acct_list) {
      int32_t y = lv_obj_get_scroll_y(g_acct_list);
      int32_t bottom = lv_obj_get_scroll_bottom(g_acct_list);
      if (bottom > 5) lv_obj_scroll_by(g_acct_list, 0, -180, LV_ANIM_ON);
      else if (y > 0) lv_obj_scroll_to_y(g_acct_list, 0, LV_ANIM_ON);
    }
  } else if (act == scr_follows) {
    if (g_follows_list) {
      int32_t y = lv_obj_get_scroll_y(g_follows_list);
      int32_t bottom = lv_obj_get_scroll_bottom(g_follows_list);
      if (bottom > 5) lv_obj_scroll_by(g_follows_list, 0, -180, LV_ANIM_ON);
      else if (y > 0) lv_obj_scroll_to_y(g_follows_list, 0, LV_ANIM_ON);
    }
  } else if (act == scr_profile) {
    // Cycle npub -> bitcoin -> zap (if any) -> npub
    const int ntabs = g_prof_tabs[2] ? 3 : 2;
    profShowTab((g_prof_tab + 1) % ntabs);
  }
}

void buttonLong() {
  lv_obj_t* act = lv_screen_active();
  if (act == scr_request) {
    if (g_focus_active) dispatchDecide(g_focus_sign);
  } else if (act == scr_accounts) {
    if (g_modal) { lv_obj_delete(g_modal); g_modal = nullptr; }
    else gotoHome();
  } else if (act == scr_follows) {
    gotoHome();
  } else if (act == scr_profile) {
    lv_screen_load_anim(g_prof_back_to ? g_prof_back_to : scr_home,
                        LV_SCR_LOAD_ANIM_OVER_RIGHT, 220, 0, false);
  }
}

void toggleByteSwap() {
  g_swap = !g_swap;
  lv_obj_invalidate(lv_screen_active());
  Serial.printf("[ui] rgb565 byte swap: %s\n", g_swap ? "ON" : "OFF");
}

void init(Arduino_GFX* gfx, Arduino_IIC* touch, const UiCallbacks& cb,
          bool auto_auth) {
  g_gfx = gfx;
  g_touch = touch;
  g_cb = cb;
  g_auto_auth = auto_auth;

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  const size_t buf_px = LCD_WIDTH * 56;
  static uint8_t* buf1 = (uint8_t*)ps_malloc(buf_px * 2);
  static uint8_t* buf2 = (uint8_t*)ps_malloc(buf_px * 2);
  if (!buf1) buf1 = (uint8_t*)malloc(buf_px * 2);
  if (!buf2) buf2 = nullptr;  // single-buffer fallback
  lv_display_set_buffers(disp, buf1, buf2, buf_px * 2,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  lv_theme_t* th = lv_theme_default_init(disp, C_BLUE, C_TEAL, true,
                                         &lv_font_montserrat_16);
  lv_display_set_theme(disp, th);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);

  scr_home = makeScreen();
  scr_accounts = makeScreen();
  scr_request = makeScreen();
  scr_result = makeScreen();
  scr_follows = makeScreen();
  scr_profile = makeScreen();

  buildHome();
  buildAccounts();
  lv_screen_load(scr_home);
}

void tick() {
  pollTouchHW();
  lv_obj_t* act = lv_screen_active();

  // Deferred avatar-cache refresh (see refreshAccounts()).
  if (g_avatars_dirty && act != scr_request && act != scr_result) {
    g_avatars_dirty = false;
    dropAvatarCache();
    if (g_acct_list) buildAccountRows();
    g_home_stale = true;
    g_follows_stale = true;
    if (act == scr_profile) g_profile_stale = true;
  }

  if (g_home_stale && act == scr_home) buildHome();
  if (g_follows_stale && act == scr_follows) buildFollows();
  if (g_profile_stale && act == scr_profile) buildProfile();
  lv_timer_handler();
}

}  // namespace ui
