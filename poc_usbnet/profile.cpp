#include "profile.h"
#include <Preferences.h>
#include <FFat.h>

namespace profile {

static bool g_fs_ok = false;

static String short12(const String& pubhex) { return pubhex.substring(0, 12); }
static String avPath(const String& pubhex) { return "/av_" + short12(pubhex) + ".png"; }
static String avPathGif(const String& pubhex) { return "/av_" + short12(pubhex) + ".gif"; }

bool begin() {
  g_fs_ok = FFat.begin(true /* format on fail */);
  if (!g_fs_ok) Serial.println("[profile] FFat mount FAILED");
  else Serial.printf("[profile] FFat ok, %u KB free\n",
                     (unsigned)(FFat.freeBytes() / 1024));
  return g_fs_ok;
}

static String nvsGet(const char prefix, const String& pubhex) {
  Preferences pr;
  if (!pr.begin("prof", true)) return "";
  String v = pr.getString((String(prefix) + short12(pubhex)).c_str(), "");
  pr.end();
  return v;
}

String nameFor(const String& pubhex)  { return nvsGet('n', pubhex); }
String aboutFor(const String& pubhex) { return nvsGet('a', pubhex); }
String lud16For(const String& pubhex) { return nvsGet('l', pubhex); }

bool hasAvatar(const String& pubhex) {
  if (!g_fs_ok) return false;
  return FFat.exists(avPath(pubhex)) || FFat.exists(avPathGif(pubhex));
}

uint8_t* readAvatar(const String& pubhex, size_t* out_len) {
  *out_len = 0;
  if (!g_fs_ok) return nullptr;
  File f = FFat.open(avPathGif(pubhex));
  if (!f) f = FFat.open(avPath(pubhex));
  if (!f) return nullptr;
  size_t n = f.size();
  if (n == 0 || n > 256 * 1024) { f.close(); return nullptr; }
  uint8_t* buf = (uint8_t*)ps_malloc(n);
  if (!buf) buf = (uint8_t*)malloc(n);
  if (!buf) { f.close(); return nullptr; }
  if (f.read(buf, n) != n) { f.close(); free(buf); return nullptr; }
  f.close();
  *out_len = n;
  return buf;
}

bool store(const String& pubhex, const String& name, const String& about,
           const String& lud16, const uint8_t* png, size_t len) {
  bool ok = true;
  if (name.length() || about.length() || lud16.length()) {
    Preferences pr;
    if (pr.begin("prof", false)) {
      if (name.length())
        pr.putString(("n" + short12(pubhex)).c_str(), name.substring(0, 32));
      if (about.length())
        pr.putString(("a" + short12(pubhex)).c_str(), about.substring(0, 140));
      if (lud16.length())
        pr.putString(("l" + short12(pubhex)).c_str(), lud16.substring(0, 64));
      pr.end();
    } else {
      ok = false;
    }
  }
  if (png && len && g_fs_ok) {
    const bool is_gif = len > 6 && memcmp(png, "GIF8", 4) == 0;
    // Replace whichever format was stored before.
    FFat.remove(avPath(pubhex));
    FFat.remove(avPathGif(pubhex));
    File f = FFat.open(is_gif ? avPathGif(pubhex) : avPath(pubhex), FILE_WRITE);
    if (!f) return false;
    ok = (f.write(png, len) == len) && ok;
    f.close();
  }
  Serial.printf("[profile] stored %s name='%s' png=%uB\n",
                short12(pubhex).c_str(), name.c_str(), (unsigned)len);
  return ok;
}

bool setFollowsJson(const String& json) {
  if (!g_fs_ok) return false;
  File f = FFat.open("/follows.json", FILE_WRITE);
  if (!f) return false;
  bool ok = f.print(json) == json.length();
  f.close();
  return ok;
}

String getFollowsJson() {
  if (!g_fs_ok) return "";
  File f = FFat.open("/follows.json");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

}  // namespace profile
