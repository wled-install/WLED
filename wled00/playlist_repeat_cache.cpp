// playlist_repeat_cache.cpp
// Per-preset playlist repeat cache used by the MIDI usermod to color
// idle playlist pads (one-shot cyan vs. looping magenta) regardless
// of which playlist is currently active.
//
// Lives in its own TU so the global state isn't duplicated when
// usermod_v2_midi.h is included from multiple translation units
// (usermods_list.cpp + wled.cpp via midi_usb_host.cpp).

#include "wled.h"
#include "fcn_declare.h"
#include "../usermods/usermod_v2_midi/usermod_v2_midi.h"

uint8_t cached_playlist_repeat[251] = {0};
bool cached_playlist_repeat_built = false;
unsigned long cached_playlist_repeat_built_for = 0;

void ensurePlaylistRepeatCache() {
  // No time check — always rebuild. The previous version skipped
  // rebuilds when presetsModifiedTime hadn't changed, but that left a
  // stale-cache window: if the user edited a preset's repeat value
  // in the WLED web UI between two stateChanged events, the cache
  // would still show the old value. Pressing shift (which doesn't
  // trigger a state change) would then repaint the controller with
  // the wrong color. The rebuild is a single /presets.json read
  // (~1-2 ms) and runs at most once per state change, so the cost
  // is negligible.
  cached_playlist_repeat_built = true;
  cached_playlist_repeat_built_for = presetsModifiedTime;
  memset(cached_playlist_repeat, 0, sizeof(cached_playlist_repeat));

  if (!requestJSONBufferLock(20)) return;
  DynamicJsonDocument doc(16384);
  bool ok = readObjectFromFile("/presets.json", nullptr, &doc);
  releaseJSONBufferLock();
  if (!ok) return;

  JsonObject root = doc.as<JsonObject>();
  for (uint16_t i = 1; i <= 250; i++) {
    char key[4];
    snprintf(key, sizeof(key), "%u", (unsigned)i);
    if (!root.containsKey(key)) continue;
    JsonObject preset = root[key];
    JsonObject playlist = preset[F("playlist")];
    if (playlist.isNull()) continue;
    // WLED's serializePlaylist writes the repeat field at the top
    // level of the playlist object. The values can be:
    //   0  = infinite loop
    //   -1 = infinite loop + shuffle (legacy "negative = infinite" marker;
    //        deserializePlaylist in wled00/playlist.cpp translates this
    //        to rep=0 + shuffle flag, but it can still appear in the
    //        stored JSON)
    //   N > 0 = finite (play N+1 times via deserialize's +1)
    // We store ANY non-positive value as 0 (looping) and any positive
    // value as itself (finite). Using `.as<int>()` explicitly so any
    // type-coercion surprises (string "1", float 1.0, etc.) round-trip
    // correctly through ArduinoJson.
    JsonVariant repVar = playlist[F("repeat")];
    int rep = repVar.isNull() ? 0 : repVar.as<int>();
    cached_playlist_repeat[i] = (rep > 0) ? (uint8_t)rep : 0;
  }
}