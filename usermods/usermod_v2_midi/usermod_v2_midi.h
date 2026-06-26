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
  uint16_t feedback_throttle_ms  = 50;

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
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectIntensity")) {
      effectIntensity = (uint8_t)(value << 1);
      applyValuesToSelectedSegs();
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectPalette")) {
      effectPalette = (uint8_t)((value << 1) % strip.getPaletteCount());
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectCurrent")) {
      effectCurrent = (uint8_t)((value << 1) % strip.getModeCount());
      applyValuesToSelectedSegs();
      colorUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectCustom1")) {
      // custom1/custom2/custom3 are per-Segment (FX.h:424), not globals. Write
      // to the main segment's custom1; effects read it on each frame.
      strip.getMainSegment().custom1 = (uint8_t)(value << 1);
      stateUpdated(CALL_MODE_BUTTON);
      return;
    }
    if (!strcmp(action, "effectCustom2")) {
      strip.getMainSegment().custom2 = (uint8_t)(value << 1);
      stateUpdated(CALL_MODE_BUTTON);
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
  }

 public:
  // ---------------------------------------------------------------------------
  // Construction / lifecycle
  // ---------------------------------------------------------------------------
  MidiUsermod(const char* name = "MidiUsb", bool enabled = true)
      : Usermod(name, enabled) {
    // Pads: 1..64 (every pad maps to a preset).
    for (int i = 0; i < 64; i++) {
      pad_to_preset[i] = (int8_t)(i + 1);
    }

    // Track buttons per "initial concept" mapping.
    setAction(track_button_action[0], "power");       // Track 1
    setAction(track_button_action[1], "nightlight");  // Track 2
    setAction(track_button_action[2], "nextpreset");  // Track 3
    setAction(track_button_action[3], "");            // Track 4 unused
    setAction(track_button_action[4], "");            // Track 5 unused
    setAction(track_button_action[5], "");            // Track 6 unused
    setAction(track_button_action[6], "prevpreset");  // Track 7 (<)
    setAction(track_button_action[7], "nextpreset");  // Track 8 (>)

    // Scene launches: 1..7 unused, 8 = blackout.
    for (int i = 0; i < 7; i++) setAction(scene_button_action[i], "");
    setAction(scene_button_action[7], "blackout");

    // Clear all CC actions.
    for (int i = 0; i < 128; i++) cc_to_action[i][0] = '\0';

    // Fader 9 (CC 0x38 = 56) -> Global brightness.
    setAction(cc_to_action[56], "bri");
    // Faders 1..5 (CC 0x30..0x34 = 48..52) -> Effect parameters.
    setAction(cc_to_action[48], "effectSpeed");
    setAction(cc_to_action[49], "effectIntensity");
    setAction(cc_to_action[50], "effectPalette");
    setAction(cc_to_action[51], "effectCustom1");
    setAction(cc_to_action[52], "effectCustom2");
    // Faders 6..8 (CC 0x35..0x37 = 53..55) default unused.
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
    if (!enabled || !midi_connected || !feedback_enabled) return;
    if (mode == CALL_MODE_INIT || mode == CALL_MODE_NO_NOTIFY) return;
    uint32_t now = millis();
    if ((int32_t)(now - suppress_feedback_until_ms) < 0) return;
    if ((uint32_t)(now - last_feedback_ms) < (uint32_t)feedback_throttle_ms) return;
    last_feedback_ms = now;

    bool playlist_active = (currentPlaylist >= 0);

    for (int i = 0; i < 64; i++) {
      int8_t preset = pad_to_preset[i];
      uint8_t color;
      if (preset <= 0) {
        color = 0;  // not mapped -> off
      } else {
        // Check whether a preset is actually saved at this slot. getPresetName
        // returns true iff preset file exists.
        String name;
        bool exists = getPresetName((uint8_t)preset, name);
        if (!exists) {
          color = 0;  // not saved -> off
        } else if ((uint8_t)preset == currentPreset) {
          color = playlist_active ? 53 : 21;  // magenta or green
        } else {
          color = 45;  // saved but not active -> blue
        }
      }
      // 0x96 = "On 100%" RGB-pad status; channel nibble encodes behaviour.
      midi_out_queue(0x96, (uint8_t)i, color);
    }

    // Track button LEDs (single-color, always status 0x90). Light up Track 1
    // (note 100) when power is on, Track 2 (note 101) when nightlight is on.
    midi_out_queue(0x90, 0x64, bri > 0 ? 1 : 0);            // Track 1
    midi_out_queue(0x90, 0x65, nightlightActive ? 2 : 0);    // Track 2 (blink when active)

    // Scene Launch 8 (note 119) lit when blackout (bri == 0).
    midi_out_queue(0x90, 0x77, bri == 0 ? 1 : 0);
  }

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
        if (d1 == 0x7A) shift_held = false;
        return;
      }
      if (d1 == 0x7A) {                  // Shift button (Track 9, note 122)
        shift_held = true;
        return;
      }
      if (d1 < 64) {                     // Pad 0..63
        int8_t preset = pad_to_preset[d1];
        if (preset <= 0) return;
        if (shift_held) {
          // Shift + pad = save current state to this preset slot.
          char name[32];
          snprintf(name, sizeof(name), "MIDI pad %d", (int)d1);
          savePreset((uint8_t)preset, name);
          stateUpdated(CALL_MODE_BUTTON_PRESET);
          // Suppress feedback echo briefly so the pad-light feedback doesn't
          // immediately re-fire.
          suppress_feedback_until_ms = millis() + feedback_throttle_ms + 50;
        } else {
          // Apply preset using the canonical "switch to a preset cleanly"
          // pattern from usermod_v2_pioneer_prolink.h:1399-1402.
          if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
          unloadPlaylist();  // best-effort; no-op if no playlist
          applyPreset((uint8_t)preset, CALL_MODE_BUTTON_PRESET);
          handlePresets();
          suppress_feedback_until_ms = millis() + feedback_throttle_ms + 50;
        }
        return;
      }
      if (d1 >= 0x64 && d1 < 0x6C) {     // Track buttons 100..107
        runAction(track_button_action[d1 - 0x64]);
        return;
      }
      if (d1 >= 0x70 && d1 < 0x78) {     // Scene launches 112..119
        runAction(scene_button_action[d1 - 0x70]);
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
        char name[24];
        snprintf(name, sizeof(name), "MIDI CC %d", (int)save_preset_cc);
        savePreset(d2, name);
        stateUpdated(CALL_MODE_BUTTON_PRESET);
        suppress_feedback_until_ms = millis() + feedback_throttle_ms + 50;
        return;
      }
      runAction(cc_to_action[d1], d2);
      return;
    }
  }

  // Called by the USB Host client event callback when a device enumerates
  // successfully. Sets the runtime "connected" flag.
  void setConnected(bool c) { midi_connected = c; }

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