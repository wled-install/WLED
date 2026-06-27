// usermod_v2_midi.h
// USB-MIDI control surface usermod for WLED-MoonModules on ESP32-P4.
//
// Default mapping:
//   Pads 0..63        (notes 0..63)        -> Presets 1..64
//   Fader 9 (Master)  (CC 0x38 = 56)       -> Global brightness (bri)
//   Faders 1..5       (CC 0x30..0x34 = 48..52) -> Effect params (speed, intensity, palette, custom1, custom2)
//   Track 1           (note 0x64 = 100)    -> Power toggle
//   Track 2           (note 0x65 = 101)    -> Nightlight toggle
//   Track 3           (note 0x66 = 102)    -> Next preset
//   Track 7           (note 0x6A = 106)    -> Previous preset (<)
//   Track 8           (note 0x6B = 107)    -> Next preset (>)
//   Track 9 (Shift)   (note 0x7A = 122)    -> Shift modifier (hold + pad = save preset)
//   Scene 1..7        (notes 0x70..0x76)   -> Unused by default
//   Scene 8           (note 0x77 = 119)    -> Blackout (bri = 0)
//
// Build flag: -D USERMOD_MIDI_USB  (gated in usermods_list.cpp)
// Target env: env:esp32p4_8MB_troyhacks  (platformio_override.ini)

#pragma once

#include "wled.h"
#include "midi_usb_host.h"  // midi_out_queue() for pad/track feedback

// Forward-declared hook for cross-usermod state. Defined in
// usermod_v2_auto_playlist.h when AutoPlaylist is compiled in.
class AutoPlaylistUsermod;
extern AutoPlaylistUsermod* autoPlaylistUsermodPtr;

// Music playlist state. Populated by queryAutoPlaylist() (defined in
// midi_usb_host.cpp so the full AutoPlaylistUsermod definition is
// visible at the call site — auto_playlist.h transitively pulls in
// audio_reactive.h which has plain globals that break the link if
// included from multiple TUs).
struct AutoPlaylistState {
  bool   present       = false;   // AutoPlaylist is compiled in + registered
  bool   active        = false;   // AutoChange on, OR autoChangeIds non-empty
  bool   autoChange    = false;   // AutoPlaylist's autoChange flag
  uint8_t musicPlaylist = 0;       // the configured music playlist slot
  bool   musicIsActive = false;   // current WLED playlist == musicPlaylist AND usermod is active
};
AutoPlaylistState queryAutoPlaylist();

class MidiUsermod : public Usermod {
 private:
  bool initDone = false;

  // ---------------------------------------------------------------------------
  // Persistent config
  // ---------------------------------------------------------------------------
  bool     enabled                = true;
  uint8_t  midi_channel          = 1;        // 1..16
  bool     feedback_enabled      = true;
  uint8_t  feedback_mode         = 0;        // 0=off, 1=state, 2=segment, 3=preset
  uint16_t feedback_throttle_ms  = 15;

  // Action gate flags — configurable from the usermod settings page.
  // Defaults match Troy's preference:
  //   copy_enabled   = true  (Scene 5 + pad + pad = copy preset)
  //   delete_enabled = false (shift + pad = save preset, NOT delete)
  //   reboot_enabled = false (shift + Scene 1 = NO-OP, not reboot)
  bool     copy_enabled              = true;
  bool     delete_enabled            = false;
  bool     reboot_enabled            = false;

  // "Music" playlist slot — the pad mapped to this preset is lit yellow
  // instead of the default magenta, so the user can find their music
  // playlist at a glance. 0 = disabled (use plain magenta for all
  // playlists, like before). Set via the usermod settings page.
  uint8_t  music_playlist_id        = 0;

  // Select-mode runtime state for the preset-copy flow.
  // Press Scene 5 to enter; press pad A (source); press pad B (destination);
  // preset A is copied to slot B if B is empty. Press Scene 5 again to exit.
  bool     select_mode_active        = false;
  uint8_t  select_source_pad         = 0xFF;  // 0xFF = no source selected yet

  // Reboot arm-and-execute state. First press of Shift + Scene 1 sets
  // `reboot_armed = true` and starts a slow flash on Scene 1 LED. Second
  // press (within REBOOT_ARM_TIMEOUT_MS) turns off all button LEDs and
  // calls ESP.restart(). Timeout (or any other button press) disarms.
  bool     reboot_armed              = false;
  uint32_t reboot_armed_ms           = 0;
  static constexpr uint32_t REBOOT_ARM_TIMEOUT_MS = 5000;

  // Soft takeover for faders. Default true — protects against sudden jumps
  // when a fader is touched at a position different from the current WLED
  // parameter value. Until the fader crosses the current value (within a
  // ±SOFT_TAKEOVER_WIGGLE window), incoming CC packets are ignored. Once
  // the fader has crossed, subsequent CCs drive the parameter as normal.
  // Per-CC tracking — each of the 128 CCs has its own takeover state.
  bool     soft_takeover_enabled     = true;
  uint8_t  cc_taken_over[128];       // 0 = not taken over, 1 = taken over
  static constexpr uint8_t SOFT_TAKEOVER_WIGGLE = 8;  // ±8 CC steps (±16 WLED units)

  // Pads 0..63 -> preset ID 1..64. -1 means unused.
  int8_t   pad_to_preset[64];

  // Optional CC-based "save preset" path. When save_preset_cc > 0 and an
  // incoming CC equals that number with a value in 1..16, save the current
  // WLED state to that preset slot. Useful for controllers without a shift
  // button (e.g. Donner Starrypad). 0 = disabled.
  uint8_t  save_preset_cc        = 0;

  // Track buttons 0x64..0x6B = 100..107. Each maps to an action verb string.
  // Scene launch buttons 0x70..0x77 = 112..119. Same verb vocabulary.
  char     track_button_action[8][16];
  char     scene_button_action[8][16];

  // CC# 0..127 -> action string. Most are unused.
  char     cc_to_action[128][16];

  // ---------------------------------------------------------------------------
  // Runtime state
  // ---------------------------------------------------------------------------
  bool        midi_connected              = false;
  bool        shift_held                  = false;
  uint16_t    midi_vendor                 = 0;
  uint16_t    midi_product                = 0;
  char        midi_device_name[32]        = {0};

  uint32_t    last_feedback_ms            = 0;
  uint32_t    suppress_feedback_until_ms  = 0;

  // Active preset tracker — see onStateChange() comment for why we need this.
  // Updated whenever we see a trustworthy currentPreset value, and used as a
  // fallback when WLED's stateUpdated() wipes currentPreset to 0.
  byte        tracked_active_preset      = 0;

  // "Is this preset slot actually saved on disk?" — uses WLED's built-in
  // presetCache (built once at boot by buildPresetCache() in wled.cpp:1810,
  // refreshed automatically by doSaveState() at presets.cpp:269 and cleared
  // by deletePreset() at presets.cpp:285). getCachedPresetExists() is O(1)
  // and acquires NO JSON buffer, so it's safe to call from onStateChange
  // without causing the "Locking JSON buffer failed!" contention that
  // getPresetName() produces when called 64 times in a row.

  // Per-pad / per-LED last-sent color cache. Used to dedup OUT traffic —
  // onStateChange compares the computed color against this array and only
  // calls midi_out_queue() when it actually changed. Initialised to 0xFF so
  // the first repaint sends everything once.
  uint8_t     last_pad_color[64]          = {};
  uint8_t     last_pad_status[64]         = {};  // 0x96 = solid, 0x99 = pulse, etc.
  uint8_t     last_check1_led             = 0xFF;  // Track 1 (note 100) — check1
  uint8_t     last_check2_led             = 0xFF;  // Track 2 (note 101) — check2
  uint8_t     last_check3_led             = 0xFF;  // Track 3 (note 102) — check3
  uint8_t     last_scene1_led             = 0xFF;  // Scene 1 (note 112) — freeze / reboot-armed
  uint8_t     last_scene5_led             = 0xFF;  // Scene 5 (note 116) — select mode
  uint8_t     last_scene8_led             = 0xFF;  // Scene 8 (note 119) — power
  uint8_t     last_scene_led              = 0xFF;  // legacy alias

  // 0xFF sentinel for "never sent" — distinct from valid colors 0..127
  static constexpr uint8_t UNSENT = 0xFF;

  // Throttle for fader-driven websocket pushes. Sweeping a fader would
  // otherwise generate one WS message per CC packet (e.g., 127 messages
  // per sweep). This caps the rate to one push per interval — the GUI
  // shows the final value within the interval.
  uint32_t    lastWsFaderPush            = 0;
  static constexpr uint32_t WS_FADER_PUSH_INTERVAL_MS = 250;

  // FPSTR-friendly config namespace keys. PROGMEM is a no-op on ESP32
  // (see cores/esp32/pgmspace.h), so these can be defined inline as constexpr
  // to avoid ODR violations when the header is included from multiple TUs
  // (usermods_list.cpp + wled.cpp via midi_usb_host.cpp).
  static constexpr const char _name[]    PROGMEM = "MidiUsb";
  static constexpr const char _enabled[] PROGMEM = "enabled";
  static constexpr const char _key_enabled[] PROGMEM = "enabled";
  static constexpr const char _key_channel[] PROGMEM = "channel";
  static constexpr const char _key_feedback_enabled[] PROGMEM = "feedback_enabled";
  static constexpr const char _key_feedback_mode[] PROGMEM = "feedback_mode";
  static constexpr const char _key_feedback_throttle_ms[] PROGMEM = "feedback_throttle_ms";
  static constexpr const char _key_pads[] PROGMEM = "pads";
  static constexpr const char _key_track_buttons[] PROGMEM = "track_buttons";
  static constexpr const char _key_scene_buttons[] PROGMEM = "scene_buttons";
  static constexpr const char _key_cc_map[] PROGMEM = "cc_map";
  static constexpr const char _key_action[] PROGMEM = "action";
  static constexpr const char _key_preset[] PROGMEM = "preset";
  static constexpr const char _key_note[] PROGMEM = "note";
  static constexpr const char _key_cc[] PROGMEM = "cc";

  // ---------------------------------------------------------------------------
  // Action dispatcher (private)
  // ---------------------------------------------------------------------------
  static void setAction(char* dst, const char* src) {
    strncpy(dst, src, 15);
    dst[15] = '\0';
  }

  // Apply a verb action (from a track / scene button press, or from a CC whose
  // mapping is a verb rather than a parameter name). value is the optional
  // argument for actions like "save<N>" (currently unused — shift+pad uses
  // savePreset() directly).
  void runAction(const char* action, uint8_t value = 0) {
    if (!action || !*action) return;

    // Parameter setters (7-bit MIDI value -> 8-bit WLED global).
    if (!strcmp(action, "bri")) {
      bri = (uint8_t)(value << 1);
      strip.setBrightness(bri, true);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectSpeed")) {
      effectSpeed = (uint8_t)(value << 1);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      // Throttle WS pushes for fader movements so the GUI doesn't get
      // spammed while the user sweeps a fader — one push per ~250 ms.
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectIntensity")) {
      effectIntensity = (uint8_t)(value << 1);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectPalette")) {
      // 0..127 mapped linearly to the palette range, so every CC value
      // drives a UNIQUE palette — no repeats (which `value % N` would
      // cause when palette count < 128) and no skips.
      uint16_t pc = strip.getPaletteCount();
      if (pc == 0) return;
      effectPalette = (uint8_t)((uint16_t)value * pc / 128);
      if (effectPalette >= pc) effectPalette = pc - 1;
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCurrent")) {
      effectCurrent = (uint8_t)((value << 1) % strip.getModeCount());
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom1")) {
      // custom1/custom2/custom3 are per-Segment (FX.h:424), not globals. Write
      // to the main segment's custom1; effects read it on each frame.
      strip.getMainSegment().custom1 = (uint8_t)(value << 1);
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom2")) {
      strip.getMainSegment().custom2 = (uint8_t)(value << 1);
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "effectCustom3")) {
      // 0..31 — clamp to the range (FX uses this for fine detail where
      // 5 bits of resolution is enough). value << 1 would give 0..254
      // which is too coarse for the intended use.
      int v = value;
      if (v > 31) v = 31;
      strip.getMainSegment().custom3 = (uint8_t)v;
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }

    // Toggle / verb actions.
    if (!strcmp(action, "power")) {
      toggleOnOff();
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "nightlight")) {
      nightlightActive = !nightlightActive;
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "nextpreset")) {
      if (currentPreset < 250) applyPreset((uint8_t)(currentPreset + 1), CALL_MODE_BUTTON_PRESET);
      handlePresets();
      return;
    }
    if (!strcmp(action, "prevpreset")) {
      if (currentPreset > 1) applyPreset((uint8_t)(currentPreset - 1), CALL_MODE_BUTTON_PRESET);
      else applyPreset(1, CALL_MODE_BUTTON_PRESET);
      handlePresets();
      return;
    }
    if (!strcmp(action, "nextfx")) {
      effectCurrent = (uint8_t)((effectCurrent + 1) % strip.getModeCount());
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "prevfx")) {
      effectCurrent = (effectCurrent == 0)
          ? (uint8_t)(strip.getModeCount() - 1)
          : (uint8_t)(effectCurrent - 1);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "nextpal")) {
      effectPalette = (uint8_t)((effectPalette + 1) % strip.getPaletteCount());
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "prevpal")) {
      effectPalette = (effectPalette == 0)
          ? (uint8_t)(strip.getPaletteCount() - 1)
          : (uint8_t)(effectPalette - 1);
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "blackout")) {
      bri = 0;
      strip.setBrightness(0, true);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "full")) {
      bri = 255;
      strip.setBrightness(255, true);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }

    // Per-effect checkmark toggles (Segment::check1/2/3 — FX-specific
    // options that some effects like Freqwave / DJ Light / etc. expose).
    // If the current effect doesn't use a given check, toggling it is a
    // harmless no-op (the bool just gets set but doesn't affect rendering).
    if (!strcmp(action, "toggleCheck1") ||
        !strcmp(action, "toggleCheck2") ||
        !strcmp(action, "toggleCheck3")) {
      Segment& seg = strip.getMainSegment();
      if      (!strcmp(action, "toggleCheck1")) seg.check1 = !seg.check1;
      else if (!strcmp(action, "toggleCheck2")) seg.check2 = !seg.check2;
      else                                      seg.check3 = !seg.check3;
      // We need THREE things for a complete UI sync:
      //   1. colorUpdated(CALL_MODE_BUTTON) — triggers the LED strip to
      //      re-render so the FX actually picks up the new check value.
      //      stateUpdated() alone doesn't re-render.
      //   2. stateUpdated(CALL_MODE_BUTTON) — fires our own onStateChange
      //      handler so the controller's pad/track LEDs reflect anything
      //      derived from the new state (and any other usermods see it).
      //   3. wsPushIfDue() — pushes a fresh websocket message if it's
      //      been long enough since the last push. Throttled so the GUI
      //      isn't spammed.
      colorUpdated(CALL_MODE_BUTTON);
      stateUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }

    // Freeze the current segment (pause FX, keep LEDs at last state).
    if (!strcmp(action, "toggleFreeze")) {
      Segment& seg = strip.getMainSegment();
      seg.freeze = !seg.freeze;
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }

    // Force a full controller repaint (e.g., after a manual state change
    // that the regular throttle/suppress logic skipped).
    if (!strcmp(action, "fullRepaint")) {
      // Bypass the throttle: reset last_feedback_ms so the repaint fires.
      last_feedback_ms = 0;
      // Reset dedup arrays so everything gets re-sent.
      memset(last_pad_color, 0xFF, sizeof(last_pad_color));
      memset(last_pad_status, 0xFF, sizeof(last_pad_status));
      last_check1_led = 0xFF;
      last_check2_led = 0xFF;
      last_check3_led = 0xFF;
      last_scene_led = 0xFF;
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }

    // Reboot the ESP. Only invoked if the user has explicitly enabled
    // reboot_enabled in the usermod settings — accidental reboots from
    // a stray button press would be very annoying.
    if (!strcmp(action, "reboot")) {
      if (!reboot_enabled) {
        DEBUG_PRINTLN(F("[MIDI] reboot ignored — reboot_enabled is false"));
        return;
      }
      DEBUG_PRINTLN(F("[MIDI] reboot requested via controller — restarting"));
      delay(100);  // let the OUT packet finish flushing
      ESP.restart();
      return;
    }
  }

  // Push a websocket state update if it's been long enough since the
  // last one. Used for fader-driven state changes so sweeping a fader
  // doesn't spam the GUI with one WS message per CC packet — we push
  // at most every WS_FADER_PUSH_INTERVAL_MS.
  void wsPushIfDue() {
    if (millis() - lastWsFaderPush < WS_FADER_PUSH_INTERVAL_MS) return;
    lastWsFaderPush = millis();
    interfaceUpdateCallMode = CALL_MODE_BUTTON;
    lastInterfaceUpdate = 0;
    updateInterfaces(CALL_MODE_BUTTON);
  }

  // Jump to the next/previous preset, skipping empty slots. direction
  // is +1 (next) or -1 (prev). Bounded by 1..250 — preset 0 is reserved
  // and 251..255 are WLED internals.
  void jumpPreset(int direction) {
    if (currentPreset < 1) currentPreset = 1;
    int start = currentPreset;
    for (int i = 0; i < 250; i++) {
      currentPreset += direction;
      if (currentPreset < 1)   currentPreset = 250;
      if (currentPreset > 250) currentPreset = 1;
      if (currentPreset == start) break;  // wrapped the whole list
      if (getCachedPresetExists(currentPreset)) break;
    }
    if (getCachedPresetExists(currentPreset)) {
      tracked_active_preset = currentPreset;
      applyPreset(currentPreset, CALL_MODE_BUTTON_PRESET);
      handlePresets();
    } else {
      // No presets at all — leave currentPreset as it was.
      currentPreset = start;
    }
  }

  // Copy a preset from src slot to dest slot by editing /presets.json
  // directly. The destination is assumed to be empty (caller checks).
  // Preserves presetCache[dest] so the on-screen LED updates without
  // requiring a full preset cache rebuild.
  static constexpr const char PRESETS_FILE[] = "/presets.json";

  void copyPresetToSlot(int src, int dest) {
    if (src <= 0 || src > 250 || dest <= 0 || dest > 250 || src == dest) return;
    if (getCachedPresetExists(dest)) {
      DEBUG_PRINTLN(F("[MIDI] copyPresetToSlot: destination already exists, no-op"));
      return;
    }
    if (!requestJSONBufferLock(20)) {
      DEBUG_PRINTLN(F("[MIDI] copyPresetToSlot: JSON buffer busy"));
      return;
    }
    // Allocate our own JsonDocument for the presets.json content (don't
    // touch WLED's fileDoc — it's owned by the JSON / preset subsystem).
    DynamicJsonDocument copy_doc(16384);
    bool ok = readObjectFromFile(PRESETS_FILE, nullptr, &copy_doc);
    if (!ok) {
      releaseJSONBufferLock();
      DEBUG_PRINTLN(F("[MIDI] copyPresetToSlot: read presets.json failed"));
      return;
    }
    JsonObject root = copy_doc.as<JsonObject>();
    char src_key[4];
    snprintf(src_key, sizeof(src_key), "%d", src);
    if (!root.containsKey(src_key)) {
      releaseJSONBufferLock();
      DEBUG_PRINTF("[MIDI] copyPresetToSlot: source preset %d not in file\n", src);
      return;
    }
    // Serialize the source subtree to a String first so we can pin the
    // data safely while we mutate copy_doc.
    String src_json;
    serializeJson(root[src_key], src_json);
    char dest_key[4];
    snprintf(dest_key, sizeof(dest_key), "%d", dest);
    root.remove(dest_key);  // defensive; caller checked but be safe

    // Parse the stringified source into a SEPARATE small doc, then
    // copy all top-level fields into the new dest subtree in copy_doc.
    DynamicJsonDocument src_only(8192);
    deserializeJson(src_only, src_json);
    JsonObject src_obj = src_only.as<JsonObject>();
    JsonObject dest_obj = root.createNestedObject(dest_key);
    // Copy ALL top-level fields including "ps" (preset cycling string)
    // and "win" (API Command URL).
    for (JsonPair kv : src_obj) {
      dest_obj[kv.key()] = kv.value();
    }

    // Write the file ourselves. writeObjectToFile() can't be used here:
    // it calls bufferedFind(key), and passing key=nullptr dereferences
    // a null pointer inside strlen() at file.cpp:65. So we serialize
    // copy_doc to a String and overwrite the file with it.
    String full_json;
    serializeJson(copy_doc, full_json);
    releaseJSONBufferLock();
    {
      File wf = WLED_FS.open(PRESETS_FILE, "w");
      if (!wf) {
        DEBUG_PRINTLN(F("[MIDI] copyPresetToSlot: failed to open presets.json for write"));
        return;
      }
      wf.print(full_json);
      wf.close();
    }
    updateFSInfo();

    // Mark the preset file as modified so the GUI's preset list refreshes.
    presetsModifiedTime = toki.second();

    if (presetCache != nullptr) {
      // Mirror what doSaveState() does on a real save so the LED feedback
      // shows the new pad as saved immediately. Match WLED's own detection
      // (presets.cpp:361) — `!presetObj["playlist"].isNull()`. The
      // "playlist" key is stored as a JsonObject containing the ps/dur/
      // transition arrays (see playlist.cpp:167 serializePlaylist), so
      // checking isNull() is the correct gate; is<JsonArray>() would miss
      // it and the destination pad would paint blue instead of magenta.
      presetCache[dest].exists = true;
      presetCache[dest].isPlaylist = !src_obj[F("playlist")].isNull();
      String nm;
      if (src_obj["n"]) {
        nm = (const char*)(src_obj["n"]);
      }
      strlcpy(presetCache[dest].name, nm.c_str(), sizeof(presetCache[dest].name));
    }
    stateUpdated(CALL_MODE_BUTTON_PRESET);
    // Bypass the 1.2s interface-update cooldown so the GUI sees the new
    // preset list immediately.
    interfaceUpdateCallMode = CALL_MODE_BUTTON_PRESET;
    lastInterfaceUpdate = 0;
    updateInterfaces(CALL_MODE_BUTTON_PRESET);
    DEBUG_PRINTF("[MIDI] copyPresetToSlot: %d -> %d OK\n", src, dest);
  }

 public:
  // ---------------------------------------------------------------------------
  // Construction / lifecycle
  // ---------------------------------------------------------------------------
  MidiUsermod(const char* name = "MidiUsb", bool enabled = true)
      : Usermod(name, enabled) {
    // Pad layout: top-left = preset 1, left-to-right then top-to-bottom.
    // The APC Mini MK2 sends notes 0..63 with note 0 = bottom-left and
    // note 56..63 = top row. Flip the row index so that the physical
    // top-left pad (note 56) maps to preset 1, top-right (note 63) to
    // preset 8, next row down to 9..16, etc., ending with bottom-left
    // (note 0) = preset 57 and bottom-right (note 7) = preset 64.
    for (int i = 0; i < 64; i++) {
      uint8_t row_from_bottom = i / 8;       // 0..7
      uint8_t col            = i % 8;       // 0..7
      uint8_t row_from_top   = 7 - row_from_bottom;
      pad_to_preset[i] = (int8_t)(row_from_top * 8 + col + 1);
    }

    // Track buttons:
    //   1, 2, 3 = Segment.check1/2/3 (FX-specific options, no-op if unused)
    //   4       = unused (shift+ = fullRepaint)
    //   5       = previous effect (shift+ = previous palette)
    //   6       = next effect     (shift+ = next palette)
    //   7, 8    = previous/next preset (skips empty slots)
    setAction(track_button_action[0], "toggleCheck1");
    setAction(track_button_action[1], "toggleCheck2");
    setAction(track_button_action[2], "toggleCheck3");
    setAction(track_button_action[3], "");
    setAction(track_button_action[4], "");  // 5: handled in handleIncomingMidi (shift-aware prevfx/prevpal)
    setAction(track_button_action[5], "");  // 6: handled in handleIncomingMidi (shift-aware nextfx/nextpal)
    setAction(track_button_action[6], "");  // 7: handled in handleIncomingMidi (prevpreset, skip empty)
    setAction(track_button_action[7], "");  // 8: handled in handleIncomingMidi (nextpreset, skip empty)

    // Scene launches:
    //   1 = toggleFreeze (Clip Stop)
    //   2..4 = unused
    //   5 = "Select" button — enters select mode for preset copy flow
    //   6..7 = unused
    //   8 = power on/off toggle (was blackout)
    setAction(scene_button_action[0], "toggleFreeze");
    setAction(scene_button_action[1], "");
    setAction(scene_button_action[2], "");
    setAction(scene_button_action[3], "");
    setAction(scene_button_action[4], "");  // Scene 5: handled in handleIncomingMidi (toggleSelectMode)
    setAction(scene_button_action[5], "");
    setAction(scene_button_action[6], "");
    setAction(scene_button_action[7], "power");

    // Clear all CC actions.
    for (int i = 0; i < 128; i++) cc_to_action[i][0] = '\0';

    // Fader 9 (CC 0x38 = 56) -> Global brightness.
    setAction(cc_to_action[56], "bri");
    // Faders 1..6 (CC 0x30..0x35 = 48..53) -> Effect parameters:
    //   1 = speed    (0..255)
    //   2 = intensity (0..255)
    //   3 = custom1  (0..255)
    //   4 = custom2  (0..255)
    //   5 = custom3  (0..31, clamped — FX uses this for fine detail)
    //   6 = palette  (0..127 direct, modulo palette count)
    setAction(cc_to_action[48], "effectSpeed");
    setAction(cc_to_action[49], "effectIntensity");
    setAction(cc_to_action[50], "effectCustom1");
    setAction(cc_to_action[51], "effectCustom2");
    setAction(cc_to_action[52], "effectCustom3");
    setAction(cc_to_action[53], "effectPalette");
    // Faders 7..8 (CC 0x36..0x37 = 54..55) default unused.

    // All CCs start "not taken over" — first CC after connect must wait
    // until the fader is near the current WLED value before driving it.
    memset(cc_taken_over, 0, sizeof(cc_taken_over));
  }

  void setup() override {
    DEBUG_PRINTLN(F("[MIDI] usermod_v2_midi loaded"));
    initDone = true;
  }

  void loop() override {
    if (!enabled || strip.isUpdating()) return;
  }

  void connected() override {
    // Network reconnect — no-op for MIDI (USB is local).
  }

  // ---------------------------------------------------------------------------
  // State-change feedback: repaint pads using the Madrix palette.
  //   no light  = no preset mapped/saved
  //   blue      = preset saved, not active
  //   green     = active preset (no playlist)
  //   magenta   = active preset (playlist active)
  //
  // Called by WLED core after every state mutation. We throttle to avoid
  // spamming the controller and skip calls during the suppress_feedback window
  // so a controller-initiated change doesn't echo back.
  // ---------------------------------------------------------------------------
  void onStateChange(uint8_t mode) override {
    // FULL-FEATURE repaint: for each of the 64 pads, light it up based on
    // whether its preset slot is saved (from preset_saved[] cache — we do
    // NOT call getPresetName here, it acquires the JSON buffer), and
    // whether that preset is the currently-active one (green if so,
    // magenta if a playlist is also running, blue if saved but not active,
    // off if not saved / unmapped). Also drives the Track 1/2 LEDs and
    // Scene Launch 8.
    //
    // Each color is compared against last_pad_color[] / last_track_led /
    // last_scene_led and only sent when it actually changed. This cuts
    // bulk-OUT traffic from 64+ packets per state change to just the
    // pads that actually need updating.
    if (!enabled || !midi_connected || !feedback_enabled) return;
    if (mode == CALL_MODE_INIT || mode == CALL_MODE_NO_NOTIFY) return;
    uint32_t now = millis();
    // Only suppress feedback echoes for state changes that WE caused
    // (CALL_MODE_BUTTON_PRESET — the mode our MIDI handler uses). For all
    // other modes (NOTIFICATION, button, etc. — i.e., changes from the
    // web UI, IR, schedule, etc.), always repaint the controller so the
    // pads reflect the new state immediately.
    if (mode == CALL_MODE_BUTTON_PRESET &&
        (int32_t)(now - suppress_feedback_until_ms) < 0) return;
    if ((uint32_t)(now - last_feedback_ms) < (uint32_t)feedback_throttle_ms) return;
    last_feedback_ms = now;

    // WLED's stateUpdated() does `if (stateChanged) currentPreset = 0;`
    // before calling us (led.cpp:106), and only restores currentPreset for
    // Path A preset requests (`{pd: ...}`) via presetToRestore in json.cpp.
    // For Path B (`{ps: N}` — the GUI dropdown), handlePresets sets
    // currentPreset = N at presets.cpp:210 and then stateUpdated wipes it
    // back to 0. So when our onStateChange fires for a GUI-driven preset
    // change, currentPreset is 0 and no pad lights up green.
    //
    // Workaround: track the active preset ourselves. We trust currentPreset
    // whenever it's non-zero (the only way it can be non-zero at this point
    // is if some path restored it after the reset, e.g. Path A's
    // presetToRestore). Otherwise we keep our last-known active preset.
    DEBUG_PRINTF("[MIDI] onStateChange mode=%u currentPreset=%u tracked=%u\n", (unsigned)mode, (unsigned)currentPreset, (unsigned)tracked_active_preset);
    if (currentPreset != 0) {
      tracked_active_preset = currentPreset;
    }

    bool playlist_active = (currentPlaylist > 0);  // matches WLED's own check
    byte active = tracked_active_preset;          // currently-playing child preset
    byte playlist_parent = playlist_active ? (byte)currentPlaylist : 0;
    uint8_t active_color = playlist_active ? 53 : 21;  // magenta if playlist, green otherwise

    // Detect "playlist just stopped" — currentPlaylist went from positive
    // (>=1) to negative (-1, the value WLED's unloadPlaylist() sets). This
    // happens when:
    //   * a non-looping playlist finishes its last entry (playlist.cpp:149),
    //   * the user changes state via web UI/IR/etc. without selecting a
    //     preset (json.cpp:297 — `if (!presetId && currentPlaylist>=0)
    //     unloadPlaylist();`),
    //   * the user calls `{"playlist":{}}` or similar to clear.
    // We clear tracked_active_preset so the previously-playing child pad
    // stops showing as green and reverts to its saved-state color (blue for
    // regular presets, solid magenta for saved playlists that are no longer
    // playing). If a new preset is being applied (e.g. playlistEndPreset),
    // WLED's stateUpdated() chain will set currentPreset before our
    // onStateChange fires again, and our normal `if (currentPreset != 0)`
    // capture will restore tracked_active_preset to the new active preset.
    static int8_t prev_current_playlist = -2;  // sentinel: uninitialized
    bool playlist_just_stopped = (prev_current_playlist > 0 && currentPlaylist < 0);
    prev_current_playlist = currentPlaylist;
    if (playlist_just_stopped) {
      tracked_active_preset = 0;  // nothing is "active" until the next preset apply
      active = 0;                  // reflect immediately for this paint
    }

    // Track power state across paints so we can detect transitions
    // (off→on or on→off) and handle the off state specially.
    static bool prev_power_off = false;  // tracks bri == 0 across paints
    bool power_off = (bri == 0);
    bool power_transition = (power_off != prev_power_off);
    prev_power_off = power_off;

    if (power_off) {
      // POWER OFF pattern: all 64 pads fast-blink red, all track/scene
      // buttons off (set by the Scene 1/5/8 section below when they
      // see power_off; the override for Scene 8 is below).
      // The user's spec: "off should be fast flashing red and ALL other
      // buttons should be blacked out". So every pad shows the same
      // status 0x9B (Blink 1/24) with velocity 5 (red).
      static uint8_t last_power_off_status[64] = {0};
      for (int i = 0; i < 64; i++) {
        if (last_power_off_status[i] != 0x9B || last_pad_status[i] != 0x9B) {
          midi_out_queue(0x9B, (uint8_t)i, 5);  // fast blink red
          last_power_off_status[i] = 0x9B;
          last_pad_status[i] = 0x9B;
          last_pad_color[i] = 5;
        }
      }
      // When power comes back, force a full repaint so all LEDs
      // restore correctly. The power_transition check below handles this.
      if (power_transition) {
        // Just entered off state — nothing extra needed, the pads
        // already paint above. On transition to ON, the fullRepaint
        // path is invoked via setConnected / requestFullRepaint.
      }
      return;  // skip the normal pad-color logic while off
    }

    if (power_transition) {
      // Just powered on. Per spec: "first push all the button states
      // and then turn on". Clear all button LEDs first, then fall
      // through to the normal paint.
      wipeAllLeds();
    }

    // Status-byte colors per the APC protocol:
    //   0x96 = solid 100%
    //   0x99 = Pulse 1/4 (slow pulse)
    //   0x9B = Blink 1/24 (fast blink)
    //
    // Pad state precedence (first match wins):
    //   1. Pad's preset is the playlist parent AND playlist is active →
    //      SLOW PULSE magenta — "this is the playlist anchor, currently
    //      playing"
    //   2. Pad's preset is the currently-playing child of the active
    //      playlist → FAST BLINK magenta — "this is what's playing right
    //      now"
    //   3. Pad's preset is a PLAYLIST (any, active or not) → SOLID magenta
    //      — "click me to start/manage a playlist" (visual differentiator
    //      from regular presets, which are blue)
    //   4. Pad's preset is the currently-active regular preset → SOLID
    //      green
    //   5. Pad's preset is saved → SOLID blue
    //   6. otherwise → off

    AutoPlaylistState aps = queryAutoPlaylist();
    bool music_playing = aps.musicIsActive;
    uint8_t music_playlist_preset = aps.musicPlaylist;  // 0 if unset
    // True when the currently-active WLED playlist IS the configured music
    // playlist. Used to swap magenta→yellow for the currently-playing child
    // pad (and the parent pad) so the user sees a unified yellow palette
    // for the music playlist's full state.
    bool active_playlist_is_music = (music_playlist_preset > 0
                                  && (uint8_t)playlist_parent == music_playlist_preset);

    for (int i = 0; i < 64; i++) {
      uint8_t color;
      uint8_t status = 0x96;  // default: solid 100%
      int8_t preset = pad_to_preset[i];
      // Pad corresponds to the configured AutoPlaylist music playlist?
      bool is_music_playlist_pad = (music_playlist_preset > 0
                                    && (uint8_t)preset == music_playlist_preset);
      if (preset <= 0) {
        color = 0;  // not mapped -> off
      } else if (is_music_playlist_pad && playlist_active
                 && (uint8_t)preset == playlist_parent) {
        // Music playlist anchor, currently playing — slow pulse yellow.
        color = 13;
        status = 0x99;
      } else if (is_music_playlist_pad && playlist_active
                 && (uint8_t)preset == active
                 && (uint8_t)preset != playlist_parent) {
        // Music playlist currently-playing child — fast blink yellow.
        color = 13;
        status = 0x9B;
      } else if (is_music_playlist_pad && playlist_active) {
        // Music playlist active but this isn't the parent or current
        // child — solid yellow so the user sees it's the music one.
        color = 13;
        status = 0x96;
      } else if (is_music_playlist_pad) {
        // Music playlist saved but not currently active — solid yellow.
        color = 13;
        status = 0x96;
      } else if (playlist_active && active_playlist_is_music
                 && (uint8_t)preset == active
                 && (uint8_t)preset != playlist_parent) {
        // Currently-playing child of the MUSIC playlist (parent is the
        // music slot, child is a different preset in the music playlist).
        // Fast blink YELLOW instead of magenta, so the child blends with
        // the parent's yellow pulse.
        color = 13;
        status = 0x9B;
      } else if (playlist_active && (uint8_t)preset == playlist_parent) {
        // Playlist anchor pad — slow pulse so it's distinguishable
        // from the currently-playing child (which fast-blinks). Both
        // magenta, but different rates.
        color = 53;
        status = 0x99;
      } else if (playlist_active && (uint8_t)preset == active
                 && (uint8_t)preset != playlist_parent) {
        // Currently-playing child preset (different from the parent) —
        // fast blink magenta.
        color = 53;
        status = 0x9B;
      } else if (presetCache != nullptr
                 && preset <= 250
                 && presetCache[preset].exists
                 && presetCache[preset].isPlaylist) {
        // A playlist that isn't currently active — solid magenta so the
        // user can tell at a glance "this is a playlist" vs the blue
        // regular presets.
        color = 53;
        status = 0x96;
      } else if ((uint8_t)preset == active && getCachedPresetExists(preset)) {
        // Active regular preset (no playlist running).
        color = 21;  // green
        status = 0x96;
      } else if (getCachedPresetExists(preset)) {
        color = 45;  // blue (saved regular preset, not active)
        status = 0x96;
      } else {
        color = 0;  // mapped but not saved -> off
      }
      // Compare both color AND status — a switch from solid to pulse
      // (e.g., when a playlist starts and the parent should pulse)
      // needs to be sent even if the color didn't change.
      if (color != last_pad_color[i] || status != last_pad_status[i]) {
        midi_out_queue(status, (uint8_t)i, color);
        last_pad_color[i] = color;
        last_pad_status[i] = status;
      }
    }

    // Track button LEDs (single-color, always status 0x90).
    // Tracks 1/2/3 reflect Segment.check1/2/3 — light up when the
    // corresponding check is on (FX-specific options; no-op for FXes that
    // don't expose them). Each check gets a distinct color from the APC
    // velocity→color table:
    //   check1 → green  (velocity 21 = #00FF00)
    //   check2 → yellow (velocity 13 = #FFFF00)
    //   check3 → red    (velocity 5  = #FF0000)
    // (APC single-color buttons only support the velocity lookup table —
    // you can't send arbitrary RGB. Different velocities give different
    // preset colors.)
    Segment& seg_for_leds = strip.getMainSegment();
    uint8_t check1_led = seg_for_leds.check1 ? 21 : 0;
    if (check1_led != last_check1_led) {
      midi_out_queue(0x90, 0x64, check1_led);
      last_check1_led = check1_led;
    }
    uint8_t check2_led = seg_for_leds.check2 ? 13 : 0;  // 13 = yellow, NOT blink
    if (check2_led != last_check2_led) {
      midi_out_queue(0x90, 0x65, check2_led);
      last_check2_led = check2_led;
    }
    uint8_t check3_led = seg_for_leds.check3 ? 5 : 0;
    if (check3_led != last_check3_led) {
      midi_out_queue(0x90, 0x66, check3_led);
      last_check3_led = check3_led;
    }

    // Disarm the reboot arm state if it has timed out (no second press
    // arrived within REBOOT_ARM_TIMEOUT_MS). Repaint Scene 1 so it goes
    // back to the normal "frozen?" indicator.
    if (reboot_armed && (millis() - reboot_armed_ms) > REBOOT_ARM_TIMEOUT_MS) {
      reboot_armed = false;
      // Force a re-paint on the next iteration (fall through).
    }

    // Scene Launch 1 (note 0x70 / 112):
    //   - solid green     when the segment is frozen
    //   - slow pulse (reboot armed, 0x99 Pulse 1/4) when waiting for
    //     the second press of shift+Scene1
    //   - off              otherwise
    // We use status 0x99 for pulse, but our single-color section uses
    // status 0x90 + velocity 2 for blink. Since the protocol reloads the
    // velocity table on every NoteOn, we just send the right velocity:
    //   velocity 1 = solid on (we choose 21 = green)
    //   velocity 2 = blink (APC native blink)
    uint8_t scene1_target;
    if (reboot_armed) {
      scene1_target = 2;  // blink — "reboot armed, press again to execute"
    } else if (seg_for_leds.freeze) {
      scene1_target = 21;  // solid green — "frozen"
    } else {
      scene1_target = 0;   // off
    }
    if (scene1_target != last_scene1_led) {
      midi_out_queue(0x90, 0x70, scene1_target);
      last_scene1_led = scene1_target;
    }

    // Scene Launch 5 (note 0x74 / 116):
    //   - solid yellow when select-mode (preset copy) is active
    //   - off otherwise
    uint8_t scene5_target = select_mode_active ? 13 : 0;  // 13 = yellow
    if (scene5_target != last_scene5_led) {
      midi_out_queue(0x90, 0x74, scene5_target);
      last_scene5_led = scene5_target;
    }

    // Scene Launch 8 (note 119): power indicator.
    //   - solid green when power is on (bri > 0)
    //   - off             when power is off (bri == 0) — the "all pads
    //                     blink red" pattern below provides the off-state
    //                     feedback instead.
    uint8_t scene8 = (uint8_t)(bri > 0 ? 21 : 0);  // 21 = green
    if (scene8 != last_scene8_led) {
      midi_out_queue(0x90, 0x77, scene8);
      last_scene8_led = scene8;
    }
  }

  // ---------------------------------------------------------------------------
  // Public hooks called from wled.cpp's app_queue drain
  // ---------------------------------------------------------------------------

  // ---------------------------------------------------------------------------
  // Public hooks called from wled.cpp's app_queue drain
  // ---------------------------------------------------------------------------

  // Inbound MIDI packet from the USB Host client. status is the channel-stripped
  // command byte (0x80 NoteOff, 0x90 NoteOn, 0xB0 CC).
  void handleIncomingMidi(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!enabled) return;

    // Per-packet trace — gated by WLED_DEBUG so the log isn't spammed while
    // you're just running. Lets you confirm "USB Host delivered to usermod"
    // end-to-end.
    DEBUG_PRINTF("[MIDI] usermod IN: status=0x%02X d1=%u d2=%u\n", status, d1, d2);

    if (status == 0x90) {                // NoteOn
      if (d2 == 0) {                     // velocity 0 = NoteOff
        if (d1 == 0x7A) {
          shift_held = false;
          // Turn off Track 9 LED to give visual feedback that shift is
          // released. (Solid yellow = held, off = released.)
          midi_out_queue(0x90, 0x7B, 0);
          // Releasing shift disarms the reboot arm state — user has
          // changed their mind. No LED re-paint here; onStateChange
          // will fire on the next stateUpdated and re-draw Scene 1
          // with the normal freeze-only logic.
          reboot_armed = false;
        }
        return;
      }
      if (d1 == 0x7A) {                  // Shift button (Track 9, note 122)
        shift_held = true;
        // Light Track 9 LED solid yellow to confirm shift is held.
        midi_out_queue(0x90, 0x7B, 13);  // 13 = yellow
        return;
      }
      if (d1 < 64) {                     // Pad 0..63
        int8_t preset = pad_to_preset[d1];
        if (preset <= 0) return;

        // --- Select mode: Scene 5 pressed, waiting for source then dest ---
        if (select_mode_active) {
          if (select_source_pad == 0xFF) {
            // First pad = source.
            select_source_pad = (uint8_t)d1;
            DEBUG_PRINTF("[MIDI] select-mode source = pad %u (preset %d)\n",
                         (unsigned)d1, (int)preset);
          } else if ((uint8_t)d1 != select_source_pad) {
            // Second pad = destination. Copy preset if destination is empty.
            if (!copy_enabled) {
              DEBUG_PRINTLN(F("[MIDI] select-mode copy ignored — copy_enabled is false"));
            } else {
              int8_t dest_preset = pad_to_preset[d1];
              if (dest_preset > 0 && !getCachedPresetExists(dest_preset)) {
                copyPresetToSlot(pad_to_preset[select_source_pad], dest_preset);
              } else {
                DEBUG_PRINTLN(F("[MIDI] select-mode copy: destination occupied or invalid, no-op"));
              }
            }
            // Auto-exit select mode after the second pad.
            select_mode_active = false;
            select_source_pad = 0xFF;
          }
          return;
        }

        if (shift_held) {
          // Shift + pad: SAVE by default; DELETE when delete_enabled is true
          // AND the preset slot is already occupied. Saving stays on the
          // current preset; deleting fires a state change so the pad clears.
          if (delete_enabled && getCachedPresetExists(preset)) {
            DEBUG_PRINTF("[MIDI] shift+pad %u → delete preset %d\n",
                         (unsigned)d1, (int)preset);
            deletePreset((uint8_t)preset);
            tracked_active_preset = 0;  // no preset active after delete (unless GUI restores)
            stateUpdated(CALL_MODE_BUTTON_PRESET);
            // Force a websocket refresh so the GUI sees the new preset list.
            // stateUpdated() only sets interfaceUpdateCallMode when state
            // actually changed (bri/segments/etc); a pure preset-list
            // change (add/delete/copy) doesn't trip that path, so we set
            // it directly here. We also bypass the 1.2s cooldown by
            // clearing lastInterfaceUpdate so the WS push fires right away.
            interfaceUpdateCallMode = CALL_MODE_BUTTON_PRESET;
            lastInterfaceUpdate = 0;
            updateInterfaces(CALL_MODE_BUTTON_PRESET);
          } else {
            // Default: save current state to this preset slot.
            char name[40];
            // Build the preset name from the current effect's name, with
            // unicode/special chars stripped via WLED's strip_unicode(),
            // then append " (Pad X)" so the user can still see which
            // physical pad saved it. FX names in WLED often have a "@"
            // suffix with secondary metadata ("Akemi PPA @Speed,Intensi"),
            // so trim everything from the first "@" onwards to keep the
            // base name clean.
            const char* fx_name = strip.getModeData(strip.getMainSegment().mode);
            String fx_clean = strip_unicode(String(fx_name ? fx_name : ""));
            int at_pos = fx_clean.indexOf('@');
            if (at_pos >= 0) fx_clean = fx_clean.substring(0, at_pos);
            // Trim trailing whitespace introduced by the cut.
            while (fx_clean.length() > 0 && isspace((unsigned char)fx_clean[fx_clean.length()-1])) {
              fx_clean.remove(fx_clean.length()-1);
            }
            snprintf(name, sizeof(name), "%.*s (Pad %d)",
                     (int)(sizeof(name) - 16),  // leave room for " (Pad XX)"
                     fx_clean.c_str(), (int)d1);
            savePreset((uint8_t)preset, name);
            // doSaveState() automatically updates presetCache[index].exists,
            // so onStateChange's getCachedPresetExists() check will now show
            // this slot as saved without any JSON buffer acquisition.
            // Stay on the currently-active preset — saving doesn't switch.
            stateUpdated(CALL_MODE_BUTTON_PRESET);
            // Same — set the interface update flag directly and bypass
            // the cooldown so the GUI sees the new preset list right away.
            interfaceUpdateCallMode = CALL_MODE_BUTTON_PRESET;
            lastInterfaceUpdate = 0;
            updateInterfaces(CALL_MODE_BUTTON_PRESET);
          }
          // Suppress feedback echo briefly so the pad-light feedback doesn't
          // immediately re-fire.
          suppress_feedback_until_ms = millis() + feedback_throttle_ms + 50;
        } else {
          // Apply preset using the canonical "switch to a preset cleanly"
          // pattern from usermod_v2_pioneer_prolink.h:1399-1402.
          if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
          unloadPlaylist();  // best-effort; no-op if no playlist
          tracked_active_preset = (uint8_t)preset;  // survives currentPreset=0 wipes from stateUpdated
          applyPreset((uint8_t)preset, CALL_MODE_BUTTON_PRESET);
          handlePresets();
          suppress_feedback_until_ms = millis() + feedback_throttle_ms + 50;
        }
        return;
      }
      if (d1 >= 0x64 && d1 < 0x6C) {     // Track buttons 100..107
        // Track 4 (note 0x67) + shift = full controller repaint.
        // Tracks 5 & 6 (notes 0x68, 0x69): prev/next FX, with shift = prev/next palette.
        // Tracks 7 & 8 (notes 0x6A, 0x6B): prev/next preset, skipping empty slots.
        if (d1 == 0x67 && shift_held) {
          runAction("fullRepaint");
        } else if (d1 == 0x68) {
          runAction(shift_held ? "prevpal" : "prevfx");
        } else if (d1 == 0x69) {
          runAction(shift_held ? "nextpal" : "nextfx");
        } else if (d1 == 0x6A) {
          jumpPreset(-1);  // previous preset (skips empty)
        } else if (d1 == 0x6B) {
          jumpPreset(+1);  // next preset (skips empty)
        } else {
          runAction(track_button_action[d1 - 0x64]);
        }
        return;
      }
      if (d1 >= 0x70 && d1 < 0x78) {     // Scene launches 112..119
        uint8_t scene_idx = (uint8_t)(d1 - 0x70);
        // Scene 5 (note 0x74): toggle select-mode for preset copy.
        if (scene_idx == 4) {
          select_mode_active = !select_mode_active;
          select_source_pad = 0xFF;
          DEBUG_PRINTF("[MIDI] select-mode %s\n",
                       select_mode_active ? "ENABLED" : "DISABLED");
          return;
        }
        // Scene 1 (note 0x70): freeze, with shift = reboot (if enabled).
        if (scene_idx == 0 && shift_held) {
          // Arm-and-execute reboot flow:
          //   First press: arm. Scene 1 LED starts slow-flashing.
          //   Second press (within REBOOT_ARM_TIMEOUT_MS): wipe all
          //     button LEDs and call ESP.restart().
          //   Timeout: disarm (the LED goes back to its normal state
          //     driven by the seg.freeze check).
          if (reboot_enabled) {
            if (!reboot_armed) {
              reboot_armed = true;
              reboot_armed_ms = millis();
              DEBUG_PRINTLN(F("[MIDI] reboot ARMED — press shift+Scene1 again to execute"));
              // Don't fire stateUpdated here — the slow-flash LED will
              // be drawn by the next onStateChange when we set
              // reboot_armed in it. Or trigger one now via a quick
              // repaint — actually we just need to redraw Scene 1,
              // simplest is to fire stateUpdated with our armed state.
              stateUpdated(CALL_MODE_BUTTON);
            } else {
              DEBUG_PRINTLN(F("[MIDI] reboot EXECUTING — turning off LEDs and restarting"));
              reboot_armed = false;
              // Wipe all button lights so the user sees a clean shutdown.
              wipeAllLeds();
              delay(100);  // let the OUT packet drain
              ESP.restart();
            }
          }
          return;
        }
        // Any other Scene 1 press without shift (or shift + Scene 1
        // when reboot is disabled) cancels an in-progress arm.
        if (reboot_armed && scene_idx != 0) {
          // No-op — the timeout / next stateUpdated will clear it.
          // We don't disarm here so a stray pad press during the arm
          // window doesn't silently cancel; only timeout or actual
          // reboot clears it. Actually safer to disarm on any other
          // button so an accidental press doesn't leave it stuck.
          reboot_armed = false;
        }
        runAction(scene_button_action[scene_idx]);
        return;
      }
      return;
    }

    if (status == 0x80) {                // NoteOff
      if (d1 == 0x7A) shift_held = false;
      return;
    }

    if (status == 0xB0) {                // Control Change
      if (save_preset_cc != 0 && d1 == save_preset_cc && d2 >= 1 && d2 <= 16) {
        char name[40];
        // Same naming convention as shift+pad: FX name (trimmed at '@')
        // + " (CC X)".
        const char* fx_name = strip.getModeData(strip.getMainSegment().mode);
        String fx_clean = strip_unicode(String(fx_name ? fx_name : ""));
        int at_pos = fx_clean.indexOf('@');
        if (at_pos >= 0) fx_clean = fx_clean.substring(0, at_pos);
        while (fx_clean.length() > 0 && isspace((unsigned char)fx_clean[fx_clean.length()-1])) {
          fx_clean.remove(fx_clean.length()-1);
        }
        snprintf(name, sizeof(name), "%.*s (CC %d)",
                 (int)(sizeof(name) - 12),  // leave room for " (CC XX)"
                 fx_clean.c_str(), (int)save_preset_cc);
        savePreset(d2, name);
        // doSaveState() already updated presetCache[index].exists; the next
        // onStateChange will see the new state via getCachedPresetExists().
        stateUpdated(CALL_MODE_BUTTON_PRESET);
        // Force websocket refresh (preset-list change doesn't trip the
        // state-changed path inside stateUpdated()), and bypass the 1.2s
        // cooldown so the GUI sees the new preset list right away.
        interfaceUpdateCallMode = CALL_MODE_BUTTON_PRESET;
        lastInterfaceUpdate = 0;
        updateInterfaces(CALL_MODE_BUTTON_PRESET);
        suppress_feedback_until_ms = millis() + feedback_throttle_ms + 50;
        return;
      }
      // Soft takeover: if enabled and this CC hasn't been "taken over" yet
      // (no recent continuous movement near the current WLED value), drop
      // the packet unless d2 is within SOFT_TAKEOVER_WIGGLE of the CC
      // value that would map to the current parameter. The first time the
      // fader crosses the current value (with wiggle) we mark it taken
      // over and start driving the parameter.
      if (soft_takeover_enabled && d1 < 128 && !cc_taken_over[d1]) {
        const char* act = cc_to_action[d1];
        if (act[0] != '\0') {
          int current_value = -1;
          if      (!strcmp(act, "bri"))            current_value = bri;
          else if (!strcmp(act, "effectSpeed"))    current_value = effectSpeed;
          else if (!strcmp(act, "effectIntensity"))current_value = effectIntensity;
          else if (!strcmp(act, "effectPalette")) current_value = effectPalette;
          else if (!strcmp(act, "effectCurrent")) current_value = effectCurrent;
          else if (!strcmp(act, "effectCustom1")) current_value = strip.getMainSegment().custom1;
          else if (!strcmp(act, "effectCustom2")) current_value = strip.getMainSegment().custom2;
          // For bri / speed / intensity / custom1 / custom2: WLED value is
          // CC*2, so expected CC = value/2. For palette / mode, the value
          // is bounded by strip count; expected CC = (value/2) clamped to
          // CC range. All these have a clear 1:1 correspondence to CC.
          int expected_cc = (current_value >= 0) ? (current_value >> 1) : -1;
          if (expected_cc >= 0) {
            int delta = (int)d2 - expected_cc;
            if (delta < 0) delta = -delta;
            if (delta > SOFT_TAKEOVER_WIGGLE) {
              DEBUG_PRINTF("[MIDI] soft-takeover: CC %u ignored (d2=%u, "
                           "expected~%d, wiggle=%d)\n",
                           (unsigned)d1, (unsigned)d2, expected_cc,
                           SOFT_TAKEOVER_WIGGLE);
              return;  // drop packet; fader hasn't reached the value yet
            }
            // Within wiggle — take over and apply below.
          }
          // unknown / un-mapped action: skip the takeover gate, apply.
        }
        cc_taken_over[d1] = 1;
      }
      runAction(cc_to_action[d1], d2);
      return;
    }
  }

  // Called by the USB Host client event callback when a device enumerates
  // successfully. Sets the runtime "connected" flag.
  void setConnected(bool c) {
    midi_connected = c;
    if (!c) {
      // Device disconnected — reset soft-takeover state so the next
      // device connection starts fresh (the user may have changed WLED
      // values via the GUI while we were disconnected, so the previous
      // "taken over" flags would now point at stale values).
      memset(cc_taken_over, 0, sizeof(cc_taken_over));
      select_mode_active = false;
      select_source_pad = 0xFF;
    }
  }

  // Called by the USB Host client after a device is identified, so /json/info
  // can show a human-readable name (looked up from kMidiDevices in
  // midi_usb_host.cpp).
  void setDeviceInfo(uint16_t vid, uint16_t pid, const char* name) {
    midi_vendor  = vid;
    midi_product = pid;
    if (name) {
      strncpy(midi_device_name, name, sizeof(midi_device_name) - 1);
      midi_device_name[sizeof(midi_device_name) - 1] = '\0';
    } else {
      midi_device_name[0] = '\0';
    }
  }

  // Called by the USB Host client right before setConnected(true), so the
  // usermod can capture the active preset BEFORE stateUpdated() wipes
  // currentPreset to 0 (led.cpp:106 — `if (stateChanged) currentPreset = 0`).
  // Without this, the on-boot preset's identity is lost the moment the
  // controller connects and we paint the pads.
  void captureActivePreset(byte preset) {
    if (preset != 0) tracked_active_preset = preset;
  }

  // Music playlist helper — light the music playlist pad in yellow (instead
// of the default magenta) so the user can find it at a glance. Pulls
// state from the AutoPlaylist usermod (if compiled in and registered),
// with a fallback to the local music_playlist_id config field.

// Wipe every LED on the controller — all 64 RGB pads (NoteOn velocity
// 0 = off) and all 16 single-color buttons (Track 1-8 + Scene 1-8).
// Used right before ESP.restart() so the user sees the device go dark
// rather than freeze mid-state.
void wipeAllLeds() {
    // All 64 pads off
    for (uint8_t i = 0; i < 64; i++) {
      midi_out_queue(0x96, i, 0);
    }
    // Track 1-8 (notes 100..107) off
    for (uint8_t n = 0x64; n <= 0x6B; n++) midi_out_queue(0x90, n, 0);
    // Scene 1-8 (notes 112..119) off
    for (uint8_t n = 0x70; n <= 0x77; n++) midi_out_queue(0x90, n, 0);
  }

  // Called by the USB Host client 500ms after device connect to do a
  // second "settled" repaint (see midi_usb_host.cpp). Forwards to the
  // same code path as the controller's Shift+Track 4 = full repaint.
  void requestFullRepaint() {
    if (!midi_connected) return;
    runAction("fullRepaint");
  }

  // ---------------------------------------------------------------------------
  // Usermod v2 boilerplate (config persistence + JSON info)
  // ---------------------------------------------------------------------------

  void addToConfig(JsonObject& root) override {
    Usermod::addToConfig(root);
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) top = root.createNestedObject(FPSTR(_name));

    top[FPSTR(_key_enabled)]               = enabled;
    top[FPSTR(_key_channel)]               = midi_channel;
    top[FPSTR(_key_feedback_enabled)]      = feedback_enabled;
    top[FPSTR(_key_feedback_mode)]         = feedback_mode;
    top[FPSTR(_key_feedback_throttle_ms)]  = feedback_throttle_ms;
    top["save_preset_cc"]                  = save_preset_cc;
    top["copy_enabled"]                    = copy_enabled;
    top["delete_enabled"]                  = delete_enabled;
    top["reboot_enabled"]                  = reboot_enabled;
    top["soft_takeover_enabled"]           = soft_takeover_enabled;
    top["music_playlist_id"]               = music_playlist_id;

    // Pads: array of {"note":N, "preset":P}. Skip pads with default mapping
    // (preset == note+1) to keep cfg.json small; full state always recoverable
    // by reading the array.
    JsonArray padsArr = top.createNestedArray(FPSTR(_key_pads));
    for (int i = 0; i < 64; i++) {
      if (pad_to_preset[i] != (int8_t)(i + 1)) {
        JsonObject o = padsArr.createNestedObject();
        o[FPSTR(_key_note)]   = i;
        o[FPSTR(_key_preset)] = (int)pad_to_preset[i];
      }
    }

    // Track buttons 0..7 (notes 0x64..0x6B): array of {"note":N, "action":"..."}.
    JsonArray tbArr = top.createNestedArray(FPSTR(_key_track_buttons));
    for (int i = 0; i < 8; i++) {
      if (track_button_action[i][0] != '\0') {
        JsonObject o = tbArr.createNestedObject();
        o[FPSTR(_key_note)]   = 0x64 + i;
        o[FPSTR(_key_action)] = track_button_action[i];
      }
    }

    // Scene launches 0..7 (notes 0x70..0x77).
    JsonArray sbArr = top.createNestedArray(FPSTR(_key_scene_buttons));
    for (int i = 0; i < 8; i++) {
      if (scene_button_action[i][0] != '\0') {
        JsonObject o = sbArr.createNestedObject();
        o[FPSTR(_key_note)]   = 0x70 + i;
        o[FPSTR(_key_action)] = scene_button_action[i];
      }
    }

    // CC map: object {"<cc>": "<action>", ...} — sparse, only populated entries.
    JsonObject ccObj = top.createNestedObject(FPSTR(_key_cc_map));
    for (int i = 0; i < 128; i++) {
      if (cc_to_action[i][0] != '\0') {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", i);
        ccObj[buf] = cc_to_action[i];
      }
    }
  }

  bool readFromConfig(JsonObject& root) override {
    bool configComplete = Usermod::readFromConfig(root);
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return configComplete;

    configComplete &= getJsonValue(top[FPSTR(_key_enabled)],              enabled, true);
    configComplete &= getJsonValue(top[FPSTR(_key_channel)],              midi_channel, (uint8_t)1);
    configComplete &= getJsonValue(top[FPSTR(_key_feedback_enabled)],     feedback_enabled, true);
    configComplete &= getJsonValue(top[FPSTR(_key_feedback_mode)],        feedback_mode, (uint8_t)0);
    configComplete &= getJsonValue(top[FPSTR(_key_feedback_throttle_ms)], feedback_throttle_ms, (uint16_t)50);
    configComplete &= getJsonValue(top["save_preset_cc"],                 save_preset_cc, (uint8_t)0);
    configComplete &= getJsonValue(top["copy_enabled"],                   copy_enabled, true);
    configComplete &= getJsonValue(top["delete_enabled"],                 delete_enabled, false);
    configComplete &= getJsonValue(top["reboot_enabled"],                 reboot_enabled, false);
    configComplete &= getJsonValue(top["soft_takeover_enabled"],          soft_takeover_enabled, true);
    configComplete &= getJsonValue(top["music_playlist_id"],             music_playlist_id, (uint8_t)0);

    // Pads. Missing pads keep their defaults; pads present in JSON override.
    JsonArray padsArr = top[FPSTR(_key_pads)];
    if (!padsArr.isNull()) {
      for (JsonObject o : padsArr) {
        int note = -1, preset = -1;
        getJsonValue(o[FPSTR(_key_note)],   note, -1);
        getJsonValue(o[FPSTR(_key_preset)], preset, -1);
        if (note >= 0 && note < 64 && preset >= -1 && preset <= 250) {
          pad_to_preset[note] = (int8_t)preset;
        }
      }
    }

    // Track buttons.
    JsonArray tbArr = top[FPSTR(_key_track_buttons)];
    if (!tbArr.isNull()) {
      for (JsonObject o : tbArr) {
        int note = -1;
        const char* act = nullptr;
        getJsonValue(o[FPSTR(_key_note)],   note, -1);
        act = o[FPSTR(_key_action)].as<const char*>();
        if (note >= 0x64 && note < 0x6C && act) {
          setAction(track_button_action[note - 0x64], act);
        }
      }
    }

    // Scene launches.
    JsonArray sbArr = top[FPSTR(_key_scene_buttons)];
    if (!sbArr.isNull()) {
      for (JsonObject o : sbArr) {
        int note = -1;
        const char* act = nullptr;
        getJsonValue(o[FPSTR(_key_note)],   note, -1);
        act = o[FPSTR(_key_action)].as<const char*>();
        if (note >= 0x70 && note < 0x78 && act) {
          setAction(scene_button_action[note - 0x70], act);
        }
      }
    }

    // CC map.
    JsonObject ccObj = top[FPSTR(_key_cc_map)];
    if (!ccObj.isNull()) {
      // Wipe existing first so removed entries don't linger.
      for (int i = 0; i < 128; i++) cc_to_action[i][0] = '\0';
      for (JsonPair kv : ccObj) {
        const char* key = kv.key().c_str();
        const char* act = kv.value().as<const char*>();
        if (!key || !act) continue;
        int cc = atoi(key);
        if (cc >= 0 && cc < 128) setAction(cc_to_action[cc], act);
      }
    }

    return configComplete;
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray infoArr = user.createNestedArray(FPSTR(_name));
    if (midi_connected) {
      char buf[64];
      if (midi_device_name[0]) {
        snprintf(buf, sizeof(buf), "%s (0x%04X:0x%04X)",
                 midi_device_name, midi_vendor, midi_product);
      } else {
        snprintf(buf, sizeof(buf), "0x%04X:0x%04X", midi_vendor, midi_product);
      }
      infoArr.add(buf);
    } else {
      infoArr.add("disconnected");
    }
  }

  uint16_t getId() override { return USERMOD_ID_MIDI_USB; }
};
// FPSTR string constants are defined inline above as static constexpr —
// PROGMEM is a no-op on ESP32, so there's no need for out-of-class
// definitions (which would otherwise cause ODR violations when this header
// is included from multiple TUs).