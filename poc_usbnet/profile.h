// Cached Nostr kind-0 profile data, pushed to the device by the browser
// extension over the USB link (the device itself is airgapped).
//
//   names    -> NVS  (namespace "prof", key "n" + first 12 hex of pubkey)
//   pictures -> FFat (/av_<first 12 hex>.png, 96x96 PNG, extension-downscaled)
#pragma once
#include <Arduino.h>

namespace profile {

bool begin();  // mounts FFat (formats on first use)

// Display name from the cached kind-0, or "" if none.
String nameFor(const String& pubhex);

// kind-0 "about" text (truncated to 140 chars on store), or "".
String aboutFor(const String& pubhex);

// lud16 lightning address ("user@domain") from kind-0, or "".
String lud16For(const String& pubhex);

bool hasAvatar(const String& pubhex);

// Reads the avatar PNG into a heap buffer (caller keeps it alive for as long
// as LVGL renders from it; free() when replacing). NULL if none.
uint8_t* readAvatar(const String& pubhex, size_t* out_len);

// Store/update profile. png may be NULL (metadata-only update). Empty
// strings leave the stored value untouched.
bool store(const String& pubhex, const String& name, const String& about,
           const String& lud16, const uint8_t* png, size_t len);

// Follows list for the active account, pushed by the bridge as JSON:
//   {"pubkey":"<hex>","follows":[{"pk":"<hex>","name":"...","lud16":"..."}]}
// Stored on FFat (/follows.json).
bool  setFollowsJson(const String& json);
String getFollowsJson();

}  // namespace profile
