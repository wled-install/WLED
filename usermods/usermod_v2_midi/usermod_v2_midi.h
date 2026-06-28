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

// Playlist globals from wled00/playlist.cpp — used to detect one-shot
// playlists (playlistRepeat == 1, plays once and stops) so we can
// paint them in a distinct color from regular looping playlists.
// playlistEndPreset is also read in case we want to differentiate
// "one-shot with no end preset" from "one-shot that applies an end
// preset" later.
extern byte playlistRepeat;
extern byte playlistEndPreset;

// Per-preset playlist repeat cache. WLED's global playlistRepeat only
// reflects the currently-active playlist; we need each saved playlist's
// own repeat value to color its pad correctly when idle (one-shot cyan
// vs. looping magenta). Built lazily by reading /presets.json once and
// invalidated when presetsModifiedTime changes. The cache state and
// the ensurePlaylistRepeatCache() function live in
// wled00/playlist_repeat_cache.cpp to avoid multiple-definition link
// errors when this header is included from multiple TUs.
extern unsigned long presetsModifiedTime;
extern uint8_t cached_playlist_repeat[251];
void ensurePlaylistRepeatCache();

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
  //   copy_enabled   = true  (Scene 7 + pad + pad = copy preset)
  //   delete_enabled = true  (shift + loaded pad = delete preset)
  //   reboot_enabled = true  (shift + assigned button = arm/execute)
  // Set any to false in settings to disable the corresponding flow.
  bool     copy_enabled              = true;
  bool     delete_enabled            = true;
  bool     reboot_enabled            = true;

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

  // Target segment for the toggleMirrorX / toggleReverseX / toggleMirrorY
  // / toggleReverseY / toggleTranspose actions. Defaults to the main
  // segment (index 0). Exposed via config so multi-segment setups can be
  // supported later without code changes — for now the user just sets
  // the index they want toggled.
  uint8_t  target_segment           = 0;

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

  // Alphabetically-sorted effect id list, populated lazily on first
  // next-fx/prev-fx call from strip.getModeData() + strip.getModeCount().
  // Built once and cached because effect names are stable for the
  // lifetime of the firmware. Each entry is the WLED effect id; the
  // sorted order matches what the web UI shows (alphabetical by name).
  struct EffectIndexEntry { char name[32]; uint8_t id; };
  EffectIndexEntry *effect_index = nullptr;
  uint16_t          effect_index_count = 0;
  bool              effect_index_built = false;

  // Build `effect_index` if not already built. Safe to call multiple
  // times; the second call is a no-op once `effect_index_built` is set.
  // Memory: ~36 bytes per effect × ~180 effects ≈ 6.5 KB, freed on
  // reboot (we never deallocate).
  void ensureEffectIndex() {
    if (effect_index_built) return;
    uint16_t n = strip.getModeCount();
    if (n == 0) return;
    effect_index = (EffectIndexEntry*)malloc(sizeof(EffectIndexEntry) * n);
    if (!effect_index) return;
    effect_index_count = n;
    for (uint16_t i = 0; i < n; i++) {
      const char* nm = strip.getModeData(i);
      if (!nm) nm = "";
      // Copy from PROGMEM into RAM. Names can include "@..." metadata;
      // we strip at the first '@' so the sort key is the display name
      // (matching how json.cpp's serializeModeNames does it).
      char buf[32]; buf[0] = 0;
      strncpy_P(buf, nm, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
      char* at = strchr(buf, '@');
      if (at) *at = 0;
      strlcpy(effect_index[i].name, buf, sizeof(effect_index[i].name));
      effect_index[i].id = (uint8_t)i;
    }
    // Sort by name (case-insensitive). Stable enough for a ~180-entry list.
    for (uint16_t i = 1; i < n; i++) {
      EffectIndexEntry cur = effect_index[i];
      uint16_t j = i;
      while (j > 0 && strcasecmp(effect_index[j - 1].name, cur.name) > 0) {
        effect_index[j] = effect_index[j - 1];
        j--;
      }
      effect_index[j] = cur;
    }
    effect_index_built = true;
  }

  // Optional CC-based "save preset" path. When save_preset_cc > 0 and an
  // incoming CC equals that number with a value in 1..16, save the current
  // WLED state to that preset slot. Useful for controllers without a shift
  // button (e.g. Donner Starrypad). 0 = disabled.
  uint8_t  save_preset_cc        = 0;

  // Track buttons 0x64..0x6B = 100..107. Each maps to an action verb string.
  // Scene launch buttons 0x70..0x77 = 112..119. Same verb vocabulary.
  char     track_button_action[8][16];
  char     track_button_shift_action[8][16];   // same shape, but action runs when shift is held
  char     scene_button_action[8][16];
  char     scene_button_shift_action[8][16];

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
  uint8_t     last_scene1_led             = 0xFF;  // Scene 1 (note 112) — toggleMirrorX indicator
  uint8_t     last_scene2_led             = 0xFF;  // Scene 2 (note 113) — toggleReverseX indicator
  uint8_t     last_scene3_led             = 0xFF;  // Scene 3 (note 114) — toggleMirrorY indicator
  uint8_t     last_scene4_led             = 0xFF;  // Scene 4 (note 115) — toggleReverseY indicator
  uint8_t     last_scene5_led             = 0xFF;  // Scene 5 (note 116) — toggleTranspose indicator
  uint8_t     last_scene7_led             = 0xFF;  // Scene 7 (note 118) — select mode (copy preset)
  uint8_t     last_scene8_led             = 0xFF;  // Scene 8 (note 119) — power
  uint8_t     last_check1_led             = 0xFF;  // Track 1 (note 100) — check1
  uint8_t     last_check2_led             = 0xFF;  // Track 2 (note 101) — check2
  uint8_t     last_check3_led             = 0xFF;  // Track 3 (note 102) — check3
  uint8_t     last_track4_led             = 0xFF;  // Track 4 (note 103) — repaint / reboot-armed
  uint8_t     last_track5_led             = 0xFF;  // Track 5 (note 104) — prev FX (disabled during playlist)
  uint8_t     last_track6_led             = 0xFF;  // Track 6 (note 105) — next FX (disabled during playlist)
  uint8_t     last_track7_led             = 0xFF;  // Track 7 (note 106) — prev preset (disabled during playlist)
  uint8_t     last_track8_led             = 0xFF;  // Track 8 (note 107) — next preset (disabled during playlist)
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
      jumpPreset(+1);  // skips empty slots AND playlists
      return;
    }
    if (!strcmp(action, "prevpreset")) {
      jumpPreset(-1);  // skips empty slots AND playlists
      return;
    }
    if (!strcmp(action, "nextfx")) {
      // Walk the alphabetically-sorted effect index (matching the web
      // UI listing order) rather than the internal id sequence. The
      // built-in FX are declared in a quasi-arbitrary order in FX_fcn.cpp
      // and incrementing effectCurrent by 1 jumps to unrelated effects.
      ensureEffectIndex();
      if (effect_index && effect_index_count > 0) {
        // Find current effect in the sorted list.
        uint16_t cur_pos = 0;
        for (uint16_t i = 0; i < effect_index_count; i++) {
          if (effect_index[i].id == effectCurrent) { cur_pos = i; break; }
        }
        uint16_t next_pos = (cur_pos + 1) % effect_index_count;
        effectCurrent = effect_index[next_pos].id;
      } else {
        // Fallback to id-sequence if the index failed to build.
        effectCurrent = (uint8_t)((effectCurrent + 1) % strip.getModeCount());
      }
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
      return;
    }
    if (!strcmp(action, "prevfx")) {
      ensureEffectIndex();
      if (effect_index && effect_index_count > 0) {
        uint16_t cur_pos = 0;
        for (uint16_t i = 0; i < effect_index_count; i++) {
          if (effect_index[i].id == effectCurrent) { cur_pos = i; break; }
        }
        uint16_t prev_pos = (cur_pos == 0) ? (effect_index_count - 1) : (cur_pos - 1);
        effectCurrent = effect_index[prev_pos].id;
      } else {
        effectCurrent = (effectCurrent == 0)
            ? (uint8_t)(strip.getModeCount() - 1)
            : (uint8_t)(effectCurrent - 1);
      }
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      wsPushIfDue();
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
    // Toggle the preset-copy "select mode" on any track/scene button
    // that's been assigned the "selectMode" action. The press without
    // a prior armed source starts the source-pending state; the LED on
    // this button blinks while armed so the user knows where to start.
    if (!strcmp(action, "selectMode")) {
      select_mode_active = !select_mode_active;
      select_source_pad = 0xFF;
      DEBUG_PRINTF("[MIDI] select-mode %s (button note 0x%02X)\n",
                   select_mode_active ? "ENABLED" : "DISABLED", d1);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    // Arm the reboot on any button assigned "rebootArm", held with
    // shift. First press: arm. Second press (with shift STILL held):
    // execute. Releasing shift early cancels the arm.
    if (!strcmp(action, "rebootArm") && shift_held) {
      if (!reboot_enabled) {
        DEBUG_PRINTLN(F("[MIDI] rebootArm ignored — reboot_enabled is false"));
        return;
      }
      if (!reboot_armed) {
        reboot_armed = true;
        reboot_armed_ms = millis();
        DEBUG_PRINTLN(F("[MIDI] reboot ARMED — press shift+button again to execute"));
        stateUpdated(CALL_MODE_BUTTON);
      } else {
        DEBUG_PRINTLN(F("[MIDI] reboot EXECUTING — turning off LEDs and restarting"));
        reboot_armed = false;
        wipeAllLeds();
        unsigned long drain_start = millis();
        for (int i = 0; i < 200; i++) { midi_usb_poll(); delay(1); }
        DEBUG_PRINTF("[MIDI] reboot drain took %lu ms\n", (unsigned long)(millis() - drain_start));
        ESP.restart();
      }
      return;
    }

    // Segment transform toggles. These act on `target_segment` (defaults
    // to the main segment, index 0 — see the member var declaration for
    // config persistence). The naming mirrors the WLED JSON keys ("mi"
    // for mirror X, "rv" for reverse X, "miy" for mirror Y, "rvy" for
    // reverse Y, "tp" for transpose — see FX.h:404-412). For non-2D
    // segments the Y/transpose flags are harmless no-ops.
    //
    // Note: a stateChanged flag is needed so WLED's stateUpdated() actually
    // notifies other usermods (the existing code path didn't push Y/transpose
    // toggles through that). colorUpdated() re-renders the strip; the 2D
    // markDirty() helper blanks the segment first so the mirror/reverse
    // doesn't smear old pixels.
    if (!strcmp(action, "toggleMirrorX") ||
        !strcmp(action, "toggleReverseX") ||
        !strcmp(action, "toggleMirrorY") ||
        !strcmp(action, "toggleReverseY") ||
        !strcmp(action, "toggleTranspose")) {
      Segment& seg = strip.getSegment(target_segment);
      if      (!strcmp(action, "toggleMirrorX"))   seg.mirror    = !seg.mirror;
      else if (!strcmp(action, "toggleReverseX"))  seg.reverse   = !seg.reverse;
      else if (!strcmp(action, "toggleMirrorY"))   seg.mirror_y  = !seg.mirror_y;
      else if (!strcmp(action, "toggleReverseY"))  seg.reverse_y = !seg.reverse_y;
      else                                         seg.transpose = !seg.transpose;
      // Re-render and propagate the change.
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
    //
    // Shift + fullRepaint is the default reboot-arm flow: Track 4's
    // default action is fullRepaint, and the user expects
    // shift+Track 4 to arm/execute reboot. So if shift is held and
    // this is the first press, arm; second press (still held) executes.
    // Releasing shift early cancels.
    if (!strcmp(action, "fullRepaint")) {
      if (shift_held && reboot_enabled) {
        if (!reboot_armed) {
          reboot_armed = true;
          reboot_armed_ms = millis();
          DEBUG_PRINTLN(F("[MIDI] reboot ARMED (via shift+fullRepaint) — press again to execute"));
          stateUpdated(CALL_MODE_BUTTON);
        } else {
          DEBUG_PRINTLN(F("[MIDI] reboot EXECUTING — turning off LEDs and restarting"));
          reboot_armed = false;
          wipeAllLeds();
          unsigned long drain_start = millis();
          for (int i = 0; i < 200; i++) { midi_usb_poll(); delay(1); }
          DEBUG_PRINTF("[MIDI] reboot drain took %lu ms\n", (unsigned long)(millis() - drain_start));
          ESP.restart();
        }
        return;
      }
      // Plain press: repaint.
      // Bypass the throttle: reset last_feedback_ms so the repaint fires.
      last_feedback_ms = 0;
      // Reset dedup arrays so everything gets re-sent.
      memset(last_pad_color, 0xFF, sizeof(last_pad_color));
      memset(last_pad_status, 0xFF, sizeof(last_pad_status));
      last_check1_led = 0xFF;
      last_check2_led = 0xFF;
      last_check3_led = 0xFF;
      last_scene1_led = 0xFF;
      last_scene2_led = 0xFF;
      last_scene3_led = 0xFF;
      last_scene4_led = 0xFF;
      last_scene5_led = 0xFF;
      last_scene7_led = 0xFF;
      last_scene8_led = 0xFF;
      last_track4_led = 0xFF;
      last_track5_led = 0xFF;
      last_track6_led = 0xFF;
      last_track7_led = 0xFF;
      last_track8_led = 0xFF;
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

  // Jump to the next/previous preset, skipping empty slots AND playlists.
// direction is +1 (next) or -1 (prev). Bounded by 1..250 — preset 0 is
// reserved and 251..255 are WLED internals. Playlists are skipped because
// applying one starts a cycling playlist, which isn't what the user wants
// when they press "next preset" — they want a single regular preset.
  void jumpPreset(int direction) {
    if (currentPreset < 1) currentPreset = 1;
    int start = currentPreset;
    for (int i = 0; i < 250; i++) {
      currentPreset += direction;
      if (currentPreset < 1)   currentPreset = 250;
      if (currentPreset > 250) currentPreset = 1;
      if (currentPreset == start) break;  // wrapped the whole list
      // Skip empty slots AND playlists. The presetCache tells us both:
      // exists=true + isPlaylist=true → it's a saved playlist → skip.
      // exists=true + isPlaylist=false → it's a regular preset → land.
      if (getCachedPresetExists(currentPreset)
          && !(presetCache != nullptr && presetCache[currentPreset].isPlaylist)) {
        break;
      }
    }
    if (getCachedPresetExists(currentPreset)
        && !(presetCache != nullptr && presetCache[currentPreset].isPlaylist)) {
      tracked_active_preset = currentPreset;
      applyPreset(currentPreset, CALL_MODE_BUTTON_PRESET);
      handlePresets();
    } else {
      // Nothing to land on (no presets at all, or only playlists) —
      // leave currentPreset as it was.
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
    char dest_key[4];
    snprintf(dest_key, sizeof(dest_key), "%d", dest);
    root.remove(dest_key);  // defensive; caller checked but be safe
    // Copy the source subtree directly into the dest subtree in the
    // SAME document. The previous version serialized to a String
    // and deserialized into a separate 8KB doc, which silently
    // dropped the "playlist" sub-object on large playlists (the
    // 8KB src_only doc overflowed) � so copying a playlist produced
    // a regular preset instead of a playlist. Copying in-document
    // preserves every nested object/array without a memory ceiling.
    JsonObject src_obj = root[src_key].as<JsonObject>();
    JsonObject dest_obj = root.createNestedObject(dest_key);
    // Copy all top-level fields including the "playlist" sub-object
    // (which itself contains ps/dur/transition arrays).
    for (JsonPair kv : src_obj) {
      dest_obj[kv.key().c_str()] = kv.value();
    }

    // Write the file ourselves. writeObjectToFile() can't be used here:
    // it calls bufferedFind(key), and passing key=nullptr dereferences
    // a null pointer inside strlen() at file.cpp:65. So we serialize
    // copy_doc to a String and overwrite the file with it.
    String full_json;
    serializeJson(copy_doc, full_json);
    // (broken DEBUG_PRINTF removed)full_json.length());
    releaseJSONBufferLock();
    {
      File wf = WLED_FS.open(PRESETS_FILE, "w");
      if (!wf) {
        DEBUG_PRINTLN(F("[MIDI] copyPresetToSlot: failed to open presets.json for write"));
        return;
      }
      size_t written = wf.print(full_json);
      wf.close();
      // (debug print removed to fix literal-newline-in-string bug)
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
    //   4       = full controller repaint (shift+ = arm/execute reboot)
    //   5       = previous effect (shift+ = previous palette)
    //   6       = next effect     (shift+ = next palette)
    //   7, 8    = previous/next preset (skips empty + finite playlists)
    // Tracks 4..8 all light red when enabled (no playlist running); the
    // tracks 5..8 group goes dark when a playlist is active (you don't
    // navigate presets while a playlist is driving the show).
    setAction(track_button_action[0], "toggleCheck1");
    setAction(track_button_action[1], "toggleCheck2");
    setAction(track_button_action[2], "toggleCheck3");
    setAction(track_button_action[3], "fullRepaint");
    setAction(track_button_action[4], "prevfx");
    setAction(track_button_action[5], "nextfx");
    setAction(track_button_action[6], "prevpreset");
    setAction(track_button_action[7], "nextpreset");

    // Scene launches:
    //   1 = toggleMirrorX
    //   2 = toggleReverseX
    //   3 = toggleMirrorY
    //   4 = toggleReverseY
    //   5 = toggleTranspose
    //   6 = unused
    //   7 = "Select" — enters select mode for preset copy flow
    //   8 = power on/off toggle
    // The toggleFreeze action is still implemented in runAction() (kept
    // around for assignment via cfg.json) but no longer mapped to a
    // default scene button.
    setAction(scene_button_action[0], "toggleMirrorX");
    setAction(scene_button_action[1], "toggleReverseX");
    setAction(scene_button_action[2], "toggleMirrorY");
    setAction(scene_button_action[3], "toggleReverseY");
    setAction(scene_button_action[4], "toggleTranspose");
    setAction(scene_button_action[5], "");  // Scene 6 unused
    setAction(scene_button_action[6], "selectMode");
    setAction(scene_button_action[7], "power");

    // Shift+button actions. Default assignments preserve the user's
    // intended behavior:
    //   Tracks 1..3: shift does the same as plain (toggleCheck1/2/3)
    //   Track 4:     shift triggers reboot arm via fullRepaint+shift
    //   Tracks 5..8: shift = palette/preset navigation
    //   Scenes 1..8: empty (no default shift behavior)
    setAction(track_button_shift_action[0], "toggleCheck1");
    setAction(track_button_shift_action[1], "toggleCheck2");
    setAction(track_button_shift_action[2], "toggleCheck3");
    setAction(track_button_shift_action[3], "fullRepaint");  // shift = reboot arm
    setAction(track_button_shift_action[4], "prevpal");
    setAction(track_button_shift_action[5], "nextpal");
    setAction(track_button_shift_action[6], "prevpreset");
    setAction(track_button_shift_action[7], "nextpreset");
    for (int i = 0; i < 8; i++) setAction(scene_button_shift_action[i], "");

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

    // Watch for a non-looping playlist ending WITHOUT a playlistEndPreset.
    // handlePlaylist() in wled00/playlist.cpp calls unloadPlaylist() and
    // then applyPreset(playlistEndPreset) only if playlistEndPreset > 0.
    // If it's 0, the playlist stops but no stateUpdated fires — so our
    // onStateChange doesn't get a chance to revert the parent pad's
    // pulse (0x99) to solid magenta or turn the last child's fast
    // blink (0x9B) into solid green. The controller stays in the
    // "playlist playing" visual state forever.
    //
    // We catch this here: if currentPlaylist just transitioned from
    // positive to -1, force a repaint. Tracked on every loop iteration
    // (cheap — just one comparison + one assignment).
    static int8_t prev_currentPlaylist = -2;  // sentinel: uninitialized
    if (prev_currentPlaylist > 0 && currentPlaylist < 0) {
      // Playlist just stopped with no end preset (or end preset was
      // 0). Clear tracked_active_preset so the formerly-playing child
      // pad doesn't stay green, and let the paint loop re-render the
      // parent pad back to solid magenta.
      tracked_active_preset = 0;
      // Fire stateUpdated to trigger onStateChange, which will then
      // re-paint the controller with the post-playlist state.
      stateUpdated(CALL_MODE_BUTTON);
    }
    prev_currentPlaylist = currentPlaylist;
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
    // The previous feedback_throttle block was removed: it was causing
    // rapid controller-driven state changes (e.g., a second toggle
    // press within feedback_throttle_ms) to be silently dropped, leaving
    // the LED stuck on its old color. The per-LED dedup arrays
    // (last_pad_color[] / last_pad_status[]) already prevent redundant
    // sends, so the throttle is not needed for traffic shaping — the
    // only cost is the cost of computing the new color, which is cheap.

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
    } else if (currentPlaylist > 0) {
      // A playlist is still running. WLED's stateUpdated wiped
      // currentPreset to 0 during the slider change, but the active
      // CHILD preset is still the same one the playlist is playing.
      // Don't clear tracked — the child's fast-blink keeps painting.
    } else if (mode != CALL_MODE_BUTTON_PRESET) {
      // No playlist running AND currentPreset is 0 AND this state
      // change didn't originate from our own preset apply. So this is
      // an external change (web GUI effect slider, IR remote, etc.)
      // that deselected the active preset — clear the controller
      // indicator so the "active preset" pad no longer stays green.
      tracked_active_preset = 0;
    }
    // (else: CALL_MODE_BUTTON_PRESET with currentPreset==0 means
    // our own preset apply just ran; handleIncomingMidi already set
    // tracked_active_preset, so leave it alone.)

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
    //   * the user calls `{"playlist":{}}` or similar to clear,
    //   * the user clicks a different preset (regular or playlist) while
    //     a playlist is running.
    //
    // We deliberately do NOT clear tracked_active_preset here. Reasoning:
    // when the user clicks a regular preset via MIDI while a playlist is
    // running, handleIncomingMidi sets tracked_active_preset to the new
    // slot BEFORE calling applyPreset, and we want that pad to light up
    // green ("active") in the post-click paint. Clearing it here would
    // wipe the new active preset.
    //
    // Cost: when a non-looping playlist finishes naturally without
    // playlistEndPreset, the last child pad stays green until the next
    // state change. Acceptable trade-off for the click-while-playing bug
    // being fixed — and handlePresets still applies playlistEndPreset if
    // it's set, which fires its own stateUpdated.
    static int8_t prev_current_playlist = -2;  // sentinel: uninitialized
    (void)prev_current_playlist;  // referenced here for clarity
    prev_current_playlist = currentPlaylist;

    // Track power state across paints so we can detect transitions
    // (off→on or on→off) and handle the off state specially.
    static bool prev_power_off = false;  // tracks bri == 0 across paints
    bool power_off = (bri == 0);
    bool power_transition = (power_off != prev_power_off);
    prev_power_off = power_off;

    if (power_off) {
      // POWER OFF pattern: ALL pads off, ALL track buttons off, ALL scene
      // buttons off EXCEPT scene 8 (the power button itself), which
      // blinks to indicate the device is in standby.
      //
      // We touch every LED every paint so that any state that may have
      // drifted (a new pad color from a preset save, a track that wasn't
      // cleared by wipeAllLeds, etc.) is reliably turned off — and
      // scene 8 reliably blinks. The dedup arrays are reset on the
      // power-on transition (see below), so the next paint will re-send
      // everything that matters.
      for (int i = 0; i < 64; i++) {
        if (last_pad_color[i] != 0 || last_pad_status[i] != 0x96) {
          midi_out_queue(0x96, (uint8_t)i, 0);  // solid off
          last_pad_color[i] = 0;
          last_pad_status[i] = 0x96;
        }
      }
      for (uint8_t n = 0x64; n <= 0x6B; n++) {  // tracks 1..8
        uint8_t cur = (n == 0x6B + 1) ? 0 : 0;  // all off
        // Just send off for every track button. We don't track last
        // values for tracks here (they're single-color buttons whose
        // velocity encodes color, not RGB pads), so we rely on the
        // state_updated chain to also push them when needed.
        midi_out_queue(0x90, n, 0);
      }
      for (uint8_t n = 0x70; n <= 0x77; n++) {  // scenes 1..8
        uint8_t v = (n == 0x77) ? 2 : 0;  // scene 8 (0x77) blinks, others off
        midi_out_queue(0x90, n, v);
      }
      // Stamp throttle here so repeated stateUpdates (e.g. a long
      // playlist tick chain) don't queue 80 packets per call. Power-off
      // is a user action and the controller doesn't need a sub-50ms
      // re-paint anyway. Critically, we stamp AFTER the power-on path
      // (below) would have run — so coming back from power-off won't
      // be throttled by this stamp.
      last_feedback_ms = now;
      return;  // skip the normal pad-color logic while off
    }

    if (power_transition) {
      // Just powered on. Reset dedup so the fall-through paint loop
      // repaints the entire controller fresh. We do NOT call
      // wipeAllLeds() here because wiping takes 80 slots in the
      // OUT ring buffer, which is redundant: the dedup reset forces
      // the fall-through to send the correct color for every LED.
      // The controller ends up at the right state without the wipe.
      memset(last_pad_color, 0xFF, sizeof(last_pad_color));
      memset(last_pad_status, 0xFF, sizeof(last_pad_status));
      last_check1_led = 0xFF;
      last_check2_led = 0xFF;
      last_check3_led = 0xFF;
      last_scene1_led = 0xFF;
      last_scene5_led = 0xFF;
      last_scene8_led = 0xFF;
      // Explicit scene 8 (power button) so it lands solid green
      // immediately, in case the fall-through paint loop is delayed
      // for any reason.
      midi_out_queue(0x90, 0x77, 21);  // solid green
      last_scene8_led = 21;
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
    // Refresh the per-preset playlist repeat cache (used for idle pad
    // coloring — one-shot cyan vs. looping magenta). Inexpensive when
    // cached; only re-reads /presets.json when presetsModifiedTime
    // changes (i.e. after a save).
    ensurePlaylistRepeatCache();
    // The currently-active playlist's "finite" status — used to drive
    // the active parent/child into cyan while playing. For IDLE
    // playlists, we use cached_playlist_repeat[preset] directly in
    // the pad branch below, so each pad colors itself based on its
    // OWN playlist's repeat count (not the currently-active one).
    //
    // Convention: playlistRepeat <= 0 (or 0 after deserialisation)
    // means infinite loop; playlistRepeat > 0 means finite (will stop
    // after N+1 passes per WLED's +1 on load). So "finite / non-
    // looping" = "any positive value".
    bool active_playlist_is_finite = (playlist_active && playlistRepeat > 0);

    // Pad paint: one helper, table-style precedence.
    //
    // The previous version was a 14-branch if-else chain that
    // repeated the same `is_music_playlist_pad` / `isPlaylist` /
    // `cached_playlist_repeat[preset] > 0` checks in every branch.
    // That redundancy is what made the finite-vs-looping logic so
    // easy to get wrong (a saved finite playlist could fall through
    // to the looping branch whenever another playlist was active).
    //
    // The refactor computes the categorisation once per pad, then
    // picks (color, status) by precedence:
    //   1. Unmapped pad               → off
    //   2. Music playlist pad         → yellow (13) in all states
    //   3. Finite playlist pad        → red (5) in all states
    //   4. Looping playlist pad      → magenta (53) in all states
    //   5. Active regular preset       → green (21) solid
    //   6. Saved regular preset        → blue (45) solid
    //   7. Mapped but not saved       → off
    //
    // Within a category, the animation byte (0x96/0x99/0x9B)
    // distinguishes solid / parent-playing-slow-pulse / child-playing-
    // fast-blink, so the same color is reused across the 3 playlist
    // states — matching the autoplaylist (yellow) and infinite-
    // playlist (magenta) pattern the user requested.
    auto paint_pad = [&](int8_t preset) -> std::pair<uint8_t, uint8_t> {
      if (preset <= 0) return {0, 0x96};

      const bool is_music = (music_playlist_preset > 0
                             && (uint8_t)preset == music_playlist_preset);
      const bool is_playlist = (presetCache != nullptr
                                && preset <= 250
                                && presetCache[preset].exists
                                && presetCache[preset].isPlaylist);

      // Music playlist (yellow in all states). Music takes priority
      // over regular playlist coloring.
      if (is_music) {
        if (playlist_active && (uint8_t)preset == playlist_parent) return {13, 0x99};
        if (playlist_active && (uint8_t)preset == active)        return {13, 0x9B};
        return {13, 0x96};
      }

      // Playlist parent: a saved playlist preset that's currently
      // playing.
      if (is_playlist && playlist_active
          && (uint8_t)preset == playlist_parent) {
        const bool finite = (cached_playlist_repeat[preset] > 0);
        return {finite ? uint8_t(32) : uint8_t(53), 0x99};
      }

      // Playlist child: a playlist is running and this preset is the
      // currently-playing child. The child itself is usually NOT a
      // playlist preset (it's a regular preset in the playlist's
      // child list), so this check deliberately does NOT require
      // `is_playlist`. Pick the parent's color category so the child
      // blends with the parent (yellow for music, 32 for finite,
      // magenta for looping) — matching the "same color, different
      // animation" pattern across the 3 states of any given
      // playlist type.
      if (playlist_active && (uint8_t)preset == active
          && (uint8_t)preset != playlist_parent) {
        // Music playlist parent → yellow child
        if (music_playlist_preset > 0
            && playlist_parent == music_playlist_preset) {
          return {13, 0x9B};
        }
        // Finite/looping parent — read repeat from the parent slot
        // since the child itself usually isn't a playlist preset.
        const bool parent_finite = (playlist_parent > 0
                                  && playlist_parent <= 250
                                  && cached_playlist_repeat[playlist_parent] > 0);
        return {parent_finite ? uint8_t(32) : uint8_t(53), 0x9B};
      }

      // Playlist idle: a saved playlist that's NOT currently playing.
      if (is_playlist) {
        const bool finite = (cached_playlist_repeat[preset] > 0);
        return {finite ? uint8_t(32) : uint8_t(53), 0x96};
      }

      // Regular saved preset.
      if ((uint8_t)preset == active && getCachedPresetExists(preset)) return {21, 0x96};
      if (getCachedPresetExists(preset)) return {45, 0x96};
      return {0, 0x96};
    };

    for (int i = 0; i < 64; i++) {
      const int8_t preset = pad_to_preset[i];
      const auto [color, status] = paint_pad(preset);
      // A switch from solid to pulse (e.g., when a playlist starts and
      // the parent should pulse) needs to be sent even if the color
      // didn't change.
      if (color != last_pad_color[i] || status != last_pad_status[i]) {
        midi_out_queue(status, (uint8_t)i, color);
        last_pad_color[i] = color;
        last_pad_status[i] = status;
      }
    }

    // Track button LEDs (single-color, status 0x90). LED feedback is
    // derived from each button's *assigned action*, not its physical
    // position — so if the user re-assigns Track 5 to toggleMirrorX
    // (or any other action), the LED automatically reflects that new
    // action's state instead of the prev-FX red indicator.
    //
    // Action → LED mapping:
    //   toggleCheck1/2/3           → green/yellow/red if corresponding
    //                                Segment::checkN is set
    //   toggleMirrorX/ReverseX,
    //   toggleMirrorY/ReverseY,
    //   toggleTranspose,
    //   toggleFreeze               → green if corresponding flag set
    //   power                       → green if bri > 0, blink if off
    //   selectMode                  → blink while select_mode_active
    //   fullRepaint                 → red (and blinks while
    //                                reboot_armed — but only on the
    //                                button that triggers shift+ reboot
    //                                arming, see Track 4 below)
    //   nextfx/prevfx/nextpal/
    //   prevpal/nextpreset/
    //   prevpreset                  → red if no playlist active, off
    //                                while a playlist is driving the
    //                                show (these are navigation, not
    //                                segment flags)
    //   full/blackout/nightlight,
    //   "" (unassigned),
    //   anything else                → off
    Segment& seg_for_leds = strip.getMainSegment();
    auto action_led = [&](const char* action) -> uint8_t {
      if (!action || !action[0]) return 0;
      if (!strcmp(action, "toggleCheck1"))    return seg_for_leds.check1 ? 21 : 0;
      if (!strcmp(action, "toggleCheck2"))    return seg_for_leds.check2 ? 13 : 0;
      if (!strcmp(action, "toggleCheck3"))    return seg_for_leds.check3 ? 5  : 0;
      if (!strcmp(action, "toggleMirrorX"))   return seg_for_leds.mirror    ? 21 : 0;
      if (!strcmp(action, "toggleReverseX"))  return seg_for_leds.reverse   ? 21 : 0;
      if (!strcmp(action, "toggleMirrorY"))   return seg_for_leds.mirror_y  ? 21 : 0;
      if (!strcmp(action, "toggleReverseY"))  return seg_for_leds.reverse_y ? 21 : 0;
      if (!strcmp(action, "toggleTranspose")) return seg_for_leds.transpose ? 21 : 0;
      if (!strcmp(action, "toggleFreeze"))    return seg_for_leds.freeze    ? 21 : 0;
      if (!strcmp(action, "power"))           return (bri > 0) ? 21 : 0;
      // rebootArm button: red normally, blink when armed (handled below
      // by the override). The "selectMode" button blinks when armed too;
      // the override is per-button below.
      if (!strcmp(action, "rebootArm"))       return 5;
      // Navigation actions: red when enabled, off during a playlist.
      if (!strcmp(action, "nextfx") || !strcmp(action, "prevfx") ||
          !strcmp(action, "nextpal") || !strcmp(action, "prevpal") ||
          !strcmp(action, "nextpreset") || !strcmp(action, "prevpreset")) {
        return playlist_active ? 0 : 5;
      }
      // selectMode, fullRepaint, etc. handled as "off" here; their
      // LEDs are painted by the per-button lambda below (so they can
      // use special blink/sent status-byte behavior).
      return 0;
    };
    // For each track button, look up its assigned action's LED. The
    // rebootArm blink override (status byte 0x99) is applied separately
    // below.
    auto paint_track_led = [&](uint8_t idx, uint8_t& dedup,
                               const char* plain_act,
                               const char* shift_act) {
      // Shift overrides plain when shift is held. We need to check
      // both because the user might have assigned rebootArm to
      // either the plain or the shift action of this button.
      const char* act = (shift_act && shift_act[0]) ? shift_act : plain_act;
      // Track buttons are single-color LEDs on the APC Mini MK2. The
      // ONLY supported status is 0x90 (plain NoteOn); velocity
      // determines behavior: 0x00 = off, 0x02 = blink (the single
      // built-in blink rate), 0x01 / 0x03..0x7F = solid on (color
      // by velocity table). The 0x96 / 0x99 / 0x9B status bytes
      // are silently dropped on the user's firmware, so the only
      // way to get a non-solid indicator is velocity 0x02 (blink).
      // The blink rate is fixed; the blink color is also fixed
      // (APC's per-button blink palette), so we just send 0x02 to
      // indicate "this is a blinking state" regardless of what the
      // desired color would have been.
      bool armed = (reboot_armed &&
                    (!strcmp(act, "rebootArm") ||
                     !strcmp(act, "fullRepaint")));
      uint8_t led = armed ? 0x02 : action_led(act);
      if (led != dedup) {
        // Always 0x90 status; velocity carries the meaning.
        midi_out_queue(0x90, (uint8_t)(0x64 + idx), led);
        dedup = led;
      }
    };
    paint_track_led(0, last_check1_led, track_button_action[0], track_button_shift_action[0]);
    paint_track_led(1, last_check2_led, track_button_action[1], track_button_shift_action[1]);
    paint_track_led(2, last_check3_led, track_button_action[2], track_button_shift_action[2]);
    paint_track_led(3, last_track4_led, track_button_action[3], track_button_shift_action[3]);
    paint_track_led(4, last_track5_led, track_button_action[4], track_button_shift_action[4]);
    paint_track_led(5, last_track6_led, track_button_action[5], track_button_shift_action[5]);
    paint_track_led(6, last_track7_led, track_button_action[6], track_button_shift_action[6]);
    paint_track_led(7, last_track8_led, track_button_action[7], track_button_shift_action[7]);

    // Disarm the reboot arm state if it has timed out (no second press
    // arrived within REBOOT_ARM_TIMEOUT_MS).
    if (reboot_armed && (millis() - reboot_armed_ms) > REBOOT_ARM_TIMEOUT_MS) {
      reboot_armed = false;
      // Force a re-paint on the next iteration (fall through).
    }

    // Scene launches 1..8 — LED feedback is driven by each button's
    // *assigned action* (same action_led() helper used above). So
    // swapping a button's action automatically updates its LED.
    //
    // The "selectMode" action gets a blink (velocity 2) override while
    // select_mode_active is true — eye-catching "armed" indicator.
    // This applies to whichever scene button the user has assigned
    // selectMode to (not hardcoded to Scene 7).
    auto paint_scene_led = [&](uint8_t idx, uint8_t& dedup) {
      const char* plain_act = scene_button_action[idx];
      const char* shift_act = scene_button_shift_action[idx];
      const char* act = (shift_act && shift_act[0]) ? shift_act : plain_act;
      // Scene buttons are single-color LEDs, same as track buttons
      // (0x90 only; 0x96/0x99/0x9B silently dropped on the user's
      // firmware). Velocity 0x02 is the only way to get a blinking
      // indicator — the blink color is fixed by the APC palette.
      const bool select_armed = (select_mode_active &&
                                 !strcmp(act, "selectMode"));
      const bool reboot_armed_here = (reboot_armed &&
                                     (!strcmp(act, "rebootArm") ||
                                      !strcmp(act, "fullRepaint")));
      uint8_t led;
      if (select_armed || reboot_armed_here) {
        led = 0x02;  // blink sentinel (single-LED built-in blink rate)
      } else {
        led = action_led(act);
      }
      if (led != dedup) {
        midi_out_queue(0x90, (uint8_t)(0x70 + idx), led);
        dedup = led;
      }
    };
    paint_scene_led(0, last_scene1_led);
    paint_scene_led(1, last_scene2_led);
    paint_scene_led(2, last_scene3_led);
    paint_scene_led(3, last_scene4_led);
    paint_scene_led(4, last_scene5_led);
    paint_scene_led(5, last_scene5_led);  // scene 6 unused — share dedup; harmless
    paint_scene_led(6, last_scene7_led);
    paint_scene_led(7, last_scene8_led);
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
          // Track 9 (shift) has no LED per the APC Mini MK2 docs, so no
          // visual feedback to clear. Just update internal state.
          //
          // Releasing shift mid-arm cancels the reboot arm so the
          // Track 4 LED reverts to solid red on the next paint. (The
          // shift+Track 4 execute path only runs while shift is held,
          // so this guarantees we can't accidentally reboot by
          // releasing shift between the arm and execute.)
          if (reboot_armed) {
            reboot_armed = false;
            stateUpdated(CALL_MODE_BUTTON);
          }
        }
        return;
      }
      if (d1 == 0x7A) {                  // Shift button (Track 9, note 122)
        shift_held = true;
        // Track 9 (shift) has no LED per the APC Mini MK2 docs, so no
        // visual feedback to send. Just update internal state; the
        // shift modifier is applied to subsequent track/scene presses.
        return;
      }
      if (d1 < 64) {                     // Pad 0..63
        int8_t preset = pad_to_preset[d1];
        if (preset <= 0) {
          // Unmapped pad (pad_to_preset[d1] <= 0 — either no preset
          // assigned to this pad, or the slot was explicitly cleared via
          // config). Treat as "stop": if a playlist is active, unload
          // it. This lets the user press any unmapped pad to silence
          // a running playlist without having to navigate to a known
          // preset slot.
          if (currentPlaylist >= 0) {
            // (broken DEBUG_PRINTF removed)d1, (int)currentPlaylist);
            unloadPlaylist();
            tracked_active_preset = 0;
            stateUpdated(CALL_MODE_BUTTON_PRESET);
            suppress_feedback_until_ms = millis() + 5;
          }
          return;
        }

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
            // (broken DEBUG_PRINTF removed)d1, (int)preset);
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
            // then append " (Pad X)" where X is the MAPPED preset number
            // (not the raw MIDI note). The APC Mini MK2 sends notes 0..63
            // with note 0 = bottom-left, note 56 = top-left. Our pad layout
            // row-flips that (bottom-left → preset 57, top-left → preset 1),
            // so for an unmapped layout d1 (note) and `preset` (mapped slot)
            // differ. We want the label to match the preset slot the user
            // will see in the WLED UI, so use `preset` here. FX names in
            // WLED often have a "@" suffix with secondary metadata
            // ("Akemi PPA @Speed,Intensi"), so trim everything from the
            // first "@" onwards to keep the base name clean.
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
                     fx_clean.c_str(), (int)preset);
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
          suppress_feedback_until_ms = millis() + 5;
        } else {
          // Apply preset using the canonical "switch to a preset cleanly"
          // pattern from usermod_v2_pioneer_prolink.h:1399-1402.
          if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
          unloadPlaylist();  // best-effort; no-op if no playlist
          tracked_active_preset = (uint8_t)preset;  // survives currentPreset=0 wipes from stateUpdated
          applyPreset((uint8_t)preset, CALL_MODE_BUTTON_PRESET);
          handlePresets();
          suppress_feedback_until_ms = millis() + 5;
        }
        return;
      }
      if (d1 >= 0x64 && d1 < 0x6C) {     // Track buttons 100..107
        // Plain press runs track_button_action[N]; shift+press runs
        // track_button_shift_action[N] (if non-empty; otherwise no-op).
        // Every verb goes through runAction so the action vocabulary
        // is the single source of truth.
        const char* act = shift_held && track_button_shift_action[d1 - 0x64][0]
                              ? track_button_shift_action[d1 - 0x64]
                              : track_button_action[d1 - 0x64];
        // Any non-reboot Track press also cancels an in-progress
        // reboot arm — accidental press shouldn't leave it stuck.
        // We skip the cancel for the reboot button itself; if the user
        // accidentally presses the same button again, runAction will
        // re-arm (first press) or execute (second press) per the
        // arm/execute state machine. Cancelling first would re-arm on
        // every press and the user could never actually execute.
        if (reboot_armed && strcmp(act, "rebootArm") != 0) {
          reboot_armed = false;
        }
        runAction(act);
        return;
      }
      if (d1 >= 0x70 && d1 < 0x78) {     // Scene launches 112..119
        // Plain press runs scene_button_action[N]; shift+press runs
        // scene_button_shift_action[N] (if non-empty; otherwise no-op).
        const char* act = shift_held && scene_button_shift_action[d1 - 0x70][0]
                              ? scene_button_shift_action[d1 - 0x70]
                              : scene_button_action[d1 - 0x70];
        // Any non-shift Scene press also cancels an in-progress reboot
        // arm — accidental press shouldn't leave it stuck.
        if (reboot_armed) reboot_armed = false;
        runAction(act);
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
        suppress_feedback_until_ms = millis() + 5;
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
              USER_PRINTF("broken DEBUG_PRINTF removed)\n", (unsigned)d1, (unsigned)d2, expected_cc, SOFT_TAKEOVER_WIGGLE);
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

    // Top-level toggles. Pads (removed — we use the fixed top-left
    // physical→logical preset mapping), Music Playlist Id (removed —
    // we read state straight from the AutoPlaylist usermod), and the
    // CC map (hidden — every fader has a hardcoded default action) are
    // no longer exposed in the settings UI.
    top[FPSTR(_key_enabled)]               = enabled;
    top[FPSTR(_key_feedback_enabled)]      = feedback_enabled;
    top[FPSTR(_key_feedback_throttle_ms)]  = feedback_throttle_ms;
    top["copy_enabled"]                    = copy_enabled;
    top["delete_enabled"]                  = delete_enabled;
    top["reboot_enabled"]                  = reboot_enabled;
    top["soft_takeover_enabled"]           = soft_takeover_enabled;
    top["target_segment"]                  = target_segment;

    // Track buttons 1..8, each with a plain and shift+ variant.
    // Two parallel objects: track_buttons (plain) and track_buttons_shift.
    // The GUI renders these as 16 dropdowns (8 plain + 8 shift).
    JsonObject trkObj = top.createNestedObject("track_buttons");
    JsonObject trkShiftObj = top.createNestedObject("track_buttons_shift");
    for (int i = 0; i < 8; i++) {
      char key[4]; snprintf(key, sizeof(key), "%d", i + 1);
      trkObj[key] = track_button_action[i];
      trkShiftObj[key] = track_button_shift_action[i];
    }

    // Scene launches 1..8, plain + shift variants. Shift is omitted for
    // scenes 6 and 7 — assigning a shift action to those puts the APC
    // into internal "note" / "drum" modes, so the GUI doesn't expose
    // shift for them.
    JsonObject scnObj = top.createNestedObject("scene_buttons");
    JsonObject scnShiftObj = top.createNestedObject("scene_buttons_shift");
    for (int i = 0; i < 8; i++) {
      char key[4]; snprintf(key, sizeof(key), "%d", i + 1);
      scnObj[key] = scene_button_action[i];
      if (i < 6) scnShiftObj[key] = scene_button_shift_action[i];
    }
  }

  bool readFromConfig(JsonObject& root) override {
    bool configComplete = Usermod::readFromConfig(root);
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return configComplete;

    configComplete &= getJsonValue(top[FPSTR(_key_enabled)],              enabled, true);
    configComplete &= getJsonValue(top[FPSTR(_key_feedback_enabled)],     feedback_enabled, true);
    configComplete &= getJsonValue(top[FPSTR(_key_feedback_throttle_ms)], feedback_throttle_ms, (uint16_t)50);
    configComplete &= getJsonValue(top["copy_enabled"],                   copy_enabled, true);
    configComplete &= getJsonValue(top["delete_enabled"],                 delete_enabled, true);
    configComplete &= getJsonValue(top["reboot_enabled"],                 reboot_enabled, true);
    configComplete &= getJsonValue(top["soft_takeover_enabled"],          soft_takeover_enabled, true);
    configComplete &= getJsonValue(top["target_segment"],                 target_segment, (uint8_t)0);

    // Track buttons (plain): "1".."8" → track_button_action[0..7].
    // Missing keys keep the defaults set by the constructor.
    JsonObject trkObj = top["track_buttons"];
    if (!trkObj.isNull()) {
      for (int i = 1; i <= 8; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = trkObj[key].as<const char*>();
        if (act) setAction(track_button_action[i - 1], act);
      }
    }

    // Track buttons (shift+): "1".."8" → track_button_shift_action[0..7].
    JsonObject trkShiftObj = top["track_buttons_shift"];
    if (!trkShiftObj.isNull()) {
      for (int i = 1; i <= 8; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = trkShiftObj[key].as<const char*>();
        if (act) setAction(track_button_shift_action[i - 1], act);
      }
    }

    // Scene launches (plain): "1".."8" → scene_button_action[0..7].
    JsonObject scnObj = top["scene_buttons"];
    if (!scnObj.isNull()) {
      for (int i = 1; i <= 8; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = scnObj[key].as<const char*>();
        if (act) setAction(scene_button_action[i - 1], act);
      }
    }

    // Scene launches (shift+): "1".."6" → scene_button_shift_action[0..5].
    // Scenes 6 and 7 (idx 5, 6) intentionally have no shift binding.
    JsonObject scnShiftObj = top["scene_buttons_shift"];
    if (!scnShiftObj.isNull()) {
      for (int i = 1; i <= 6; i++) {
        char key[4]; snprintf(key, sizeof(key), "%d", i);
        const char* act = scnShiftObj[key].as<const char*>();
        if (act) setAction(scene_button_shift_action[i - 1], act);
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

  // WLED-MM calls appendConfigData() once per usermod when the
  // settings page for that usermod is loaded. We use it to upgrade
  // the auto-generated text inputs for our track/scene action
  // assignments into dropdowns with the full verb list (and the
  // current value pre-selected).
  //
  // For each "1".."8" key under track_buttons / track_buttons_shift /
  // scene_buttons / scene_buttons_shift, we find the corresponding
  // text input in the rendered form (which WLED-MM generates from
  // addToConfig's nested-object string fields) and replace it with
  // a <select> listing every valid action verb.
  void appendConfigData() override {
    // Helper: emit JS that converts the text input for one key
    // (under our usermod's namespace) into a dropdown.
    //
    // The JS template's addDropdown(usermod, fieldPath) builds the
    // selector as "<usermod>:<fieldPath>", so the second arg must be
    // ONLY the path relative to the usermod root (no usermod prefix).
    auto emit_dropdown = [&](const char* group, uint8_t idx, bool shift) {
      char path[24];
      snprintf(path, sizeof(path), "%s:%d", group, (int)(idx + 1));
      oappend(SET_F("var __s = addDropdown('MidiUsb','"));
      oappend(path);
      oappend(SET_F("');"));
      // Empty option so the user can clear an assignment.
      oappend(SET_F("addOption(__s,'(unassigned)','');"));
      // All available action verbs, with the ones that are defaults
      // for this button+shift combination marked "(default)".
      // The verb list mirrors the action vocabulary in runAction().
      static const char* verbs[] = {
        "power", "blackout", "full", "nightlight",
        "nextfx", "prevfx", "nextpal", "prevpal",
        "nextpreset", "prevpreset",
        "toggleCheck1", "toggleCheck2", "toggleCheck3",
        "toggleMirrorX", "toggleReverseX",
        "toggleMirrorY", "toggleReverseY",
        "toggleTranspose", "toggleFreeze",
        "fullRepaint", "rebootArm", "selectMode",
      };
      static const uint8_t nverbs = sizeof(verbs) / sizeof(verbs[0]);
      for (uint8_t v = 0; v < nverbs; v++) {
        // Mark defaults so the user can see what's pre-assigned.
        const char* suffix = "";
        bool is_default = false;
        const bool is_track = (group && !strcmp(group, "track_buttons"));
        const bool is_scene = (group && !strcmp(group, "scene_buttons"));
        if (!shift && is_track) {
          // Plain defaults for TRACK buttons only.
          if (idx == 0 && !strcmp(verbs[v], "toggleCheck1")) is_default = true;
          if (idx == 1 && !strcmp(verbs[v], "toggleCheck2")) is_default = true;
          if (idx == 2 && !strcmp(verbs[v], "toggleCheck3")) is_default = true;
          if (idx == 3 && !strcmp(verbs[v], "fullRepaint")) is_default = true;
          if (idx == 4 && !strcmp(verbs[v], "prevfx")) is_default = true;
          if (idx == 5 && !strcmp(verbs[v], "nextfx")) is_default = true;
          if (idx == 6 && !strcmp(verbs[v], "prevpreset")) is_default = true;
          if (idx == 7 && !strcmp(verbs[v], "nextpreset")) is_default = true;
        } else if (shift && is_track) {
          // Shift defaults for TRACK buttons only.
          if (idx == 0 && !strcmp(verbs[v], "toggleCheck1")) is_default = true;
          if (idx == 1 && !strcmp(verbs[v], "toggleCheck2")) is_default = true;
          if (idx == 2 && !strcmp(verbs[v], "toggleCheck3")) is_default = true;
          if (idx == 3 && !strcmp(verbs[v], "fullRepaint")) is_default = true;
          if (idx == 4 && !strcmp(verbs[v], "prevpal")) is_default = true;
          if (idx == 5 && !strcmp(verbs[v], "nextpal")) is_default = true;
          if (idx == 6 && !strcmp(verbs[v], "prevpreset")) is_default = true;
          if (idx == 7 && !strcmp(verbs[v], "nextpreset")) is_default = true;
        } else if (!shift && is_scene) {
          // Scene defaults for SCENE buttons only.
          if (idx == 0 && !strcmp(verbs[v], "toggleMirrorX")) is_default = true;
          if (idx == 1 && !strcmp(verbs[v], "toggleReverseX")) is_default = true;
          if (idx == 2 && !strcmp(verbs[v], "toggleMirrorY")) is_default = true;
          if (idx == 3 && !strcmp(verbs[v], "toggleReverseY")) is_default = true;
          if (idx == 4 && !strcmp(verbs[v], "toggleTranspose")) is_default = true;
          if (idx == 6 && !strcmp(verbs[v], "selectMode")) is_default = true;
          if (idx == 7 && !strcmp(verbs[v], "power")) is_default = true;
        }
        // (scene_buttons_shift has no defaults — leave unassigned)
        if (is_default) suffix = " (default)";
        oappend(SET_F("addOption(__s,'"));
        oappend(verbs[v]);
        oappend(suffix);
        oappend(SET_F("','"));
        oappend(verbs[v]);
        oappend(SET_F("');"));
      }
    };

    // Dropdowns: 8 track plain + 8 track shift + 8 scene plain +
    // 6 scene shift (scenes 6 and 7 are intentionally excluded —
    // assigning shift actions to them puts the APC into internal
    // "note" / "drum" modes we don't want).
    const char* groups[] = {
      "track_buttons", "track_buttons_shift",
      "scene_buttons", "scene_buttons_shift",
    };
    for (uint8_t g = 0; g < 4; g++) {
      bool shift = (g == 1 || g == 3);
      const bool is_scene = (g >= 2);
      const uint8_t max_i = (shift && is_scene) ? 6 : 8;
      for (uint8_t i = 0; i < max_i; i++) {
        emit_dropdown(groups[g], i, shift);
      }
    }
  }

  uint16_t getId() override { return USERMOD_ID_MIDI_USB; }
};
// FPSTR string constants are defined inline above as static constexpr —
// PROGMEM is a no-op on ESP32, so there's no need for out-of-class
// definitions (which would otherwise cause ODR violations when this header
// is included from multiple TUs).