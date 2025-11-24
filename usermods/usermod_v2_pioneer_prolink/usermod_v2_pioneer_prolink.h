#pragma once

/*
   @title     Pro DJ Link
   @file      usermod_v2_prolink.h
   @brief     Syncs WLED to Pioneer CDJs via Pro DJ Link protocol
   @target    ESP32 (Requires AsyncUDP)
   @repo      WLED MoonModules
*/

#include "wled.h"
#include <AsyncUDP.h>

#define PORT_ANNOUNCE 50000 
#define PORT_BEAT     50001 
#define PORT_STATUS   50002 

#define TYPE_ANNOUNCE     0x06 
#define TYPE_STATUS       0x0A 
#define TYPE_BEAT_GRID    0x0B 

#define WLED_DEVICE_NAME  "WLED-Lighting"
#define WLED_DEVICE_ID    0x05 

// Center value for pitch calculations (at 0% pitch)
#define PITCH_CENTER 0x10000000  // 268,435,456 decimal
#define PITCH_SCALE  2684352.0f  // Scale factor for percentage conversion

volatile float prolink_bpm_public = 0.0;
volatile uint8_t prolink_beat_public = 0;
volatile uint16_t prolink_beats_elapsed_public = 0;
volatile uint8_t prolink_bars_elapsed_public = 0;
volatile uint8_t prolink_bars_remaining_public = 0;
volatile uint32_t prolink_beat_number_public = 0;
volatile float prolink_beat_progress_public = 0.0;  // 0.0 to 1.0 progress through current beat
volatile uint32_t prolink_track_id_public = 0;
volatile uint32_t prolink_track_position_ms_public = 0;

// Pitch values for all players
volatile float prolink_pitch_slider_player1_public = 0.0;  // Pitch slider position (%)
volatile float prolink_pitch_slider_player2_public = 0.0;
volatile float prolink_pitch_slider_player3_public = 0.0;
volatile float prolink_pitch_slider_player4_public = 0.0;

volatile float prolink_effective_pitch_player1_public = 0.0;  // Pitch including jog (%)
volatile float prolink_effective_pitch_player2_public = 0.0;
volatile float prolink_effective_pitch_player3_public = 0.0;
volatile float prolink_effective_pitch_player4_public = 0.0;

// Jog wheel state for all players
volatile uint8_t prolink_jog_state_player1_public = 0;  // 0=Normal, 1=Nudged, 2=Jogged
volatile uint8_t prolink_jog_state_player2_public = 0;
volatile uint8_t prolink_jog_state_player3_public = 0;
volatile uint8_t prolink_jog_state_player4_public = 0;

struct ProLinkState {
  volatile int8_t  targetPreset = -1;
  volatile bool    triggerBeat = false;
  volatile uint8_t vuMeter = 0;

  volatile float   pitchSlider[4] = { 0.0, 0.0, 0.0, 0.0 };  // Pitch slider per player (%)
  volatile float   effectivePitch[4] = { 0.0, 0.0, 0.0, 0.0 };  // Effective pitch per player (%)
  volatile float   bpm = 0.0;
  volatile float   origBpm = 0.0;

  volatile bool    isConnected = false;
  volatile bool    isLoaded = true;
  volatile bool    isMaster = false;
  volatile uint8_t beatInMeasure = 0;
  volatile uint8_t activePlayerID = 0;

  volatile uint16_t beatsElapsed = 0;
  volatile uint8_t halfBarsRemaining = 0;
  volatile uint8_t halfBarsElapsed = 0;
  volatile uint32_t trackPositionMs = 0;

  // Beat packet timing data
  volatile uint32_t beatNumber = 0;
  volatile uint32_t nextBeatMs = 0;

  // Track identification
  volatile uint32_t currentTrackId = 0;
  volatile uint8_t currentSlot = 0;

  // Jog wheel state (per player)
  volatile uint8_t jogState[4] = { 0, 0, 0, 0 };  // 0=Normal, 1=Nudged, 2=Jogged

  volatile uint64_t peerMap = 0;
  IPAddress peerIPs[64];
};

class ProLinkUsermod : public Usermod {

private:
  AsyncUDP udpStatus;
  AsyncUDP udpBeat;
  AsyncUDP udpAnnounce;

  ProLinkState linkState;
  unsigned long lastKeepAlive = 0;
  uint8_t lastPrintedBeat = 0;

  bool enabled = true;
  bool enableBeatFlash = true;
  bool enableVuMeter = false;
  bool enableBeatPacketDebug = false;
  bool enablePitchDebug = false;

  uint8_t presetOnBeat = 0;

  static const char _name[];
  static const char _enabled[];
  static const char _beatFlash[];
  static const char _vuMeter[];
  static const char _beatPreset[];
  static const char _beatDebug[];
  static const char _pitchDebug[];

  // Helper function to convert raw pitch value to percentage
  float rawPitchToPercent(uint32_t rawPitch) {
    // Center value is 0x10000000 (268,435,456) at 0% pitch
    // Each 1% = 2,684,352 units
    int32_t offset = (int32_t)rawPitch - PITCH_CENTER;
    return (float)offset / PITCH_SCALE;
  }

  // Helper function to format track position as MM:SS.mmm
  String formatTrackPosition(uint32_t positionMs) {
    uint32_t totalSeconds = positionMs / 1000;
    uint32_t minutes = totalSeconds / 60;
    uint32_t seconds = totalSeconds % 60;
    uint32_t milliseconds = positionMs % 1000;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%02u:%02u.%03u", minutes, seconds, milliseconds);
    return String(buffer);
  }

  void sendKeepAlive() {
    if (!Network.isConnected()) return;

    uint8_t peerCount = 0;
    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) peerCount++;
    }

    uint8_t pkg[64]; memset(pkg, 0, 64);
    uint8_t header[] = { 0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c };
    memcpy(pkg, header, 10);

    pkg[0x0A] = TYPE_ANNOUNCE;
    pkg[0x0B] = 0x00;
    snprintf((char*)&pkg[0x0C], 20, "%s", WLED_DEVICE_NAME);

    pkg[0x20] = 0x01; pkg[0x21] = 0x02; pkg[0x22] = 0x00; pkg[0x23] = 0x36;
    pkg[0x24] = WLED_DEVICE_ID; pkg[0x25] = 0x01;

    Network.localMAC(&pkg[0x26]);
    uint32_t ip = (uint32_t)Network.localIP();
    memcpy(&pkg[0x2C], &ip, 4);

    pkg[0x30] = peerCount + 1;
    pkg[0x34] = 0x01;

    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) {
        IPAddress targetIP = linkState.peerIPs[i];
        if (targetIP[0] != 0) udpAnnounce.writeTo(pkg, 54, targetIP, PORT_ANNOUNCE);
      }
    }
  }

  void parseAnnouncePacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 36) return;
    if (data[0x0A] == TYPE_ANNOUNCE) {
      uint8_t devID = data[0x24];
      if (devID > 0 && devID < 64 && devID != WLED_DEVICE_ID) {
        bool isNew = !(linkState.peerMap & ((uint64_t)1 << (devID - 1)));
        linkState.peerMap |= ((uint64_t)1 << (devID - 1));
        linkState.peerIPs[devID - 1] = packet.remoteIP();
        linkState.isConnected = true;
        if (isNew) sendKeepAlive();
      }
    }
  }

  void parseStatusPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    size_t len = packet.length();

    if (len < 0x11F) return;
    if (data[0x0A] != TYPE_STATUS) return;

    // Track not loaded
    if (data[0xA5] == 0x01) {
      linkState.isLoaded = false;
      return;
    }
    linkState.isLoaded = true;

    uint8_t playerID = data[0x21];
    uint8_t flags = data[0x89];
    bool isPlaying = (flags & 0x40) != 0;
    bool isMaster = (flags & 0x20) != 0;

    // Track which player is master
    if (isMaster) {
      linkState.activePlayerID = playerID;
      linkState.isMaster = true;
    }

    // Parse pitch slider (32-bit at 0xB7-0xBA)
    // Note: Byte offsets in UDP packet include Ethernet/IP/UDP headers (42 bytes = 0x2A)
    // So wire offset 0xB7 = packet data offset 0x8D
    if (len > 0x90 && playerID >= 1 && playerID <= 4) {
      uint32_t rawPitchSlider = ((uint32_t)data[0x8D] << 24) | ((uint32_t)data[0x8E] << 16) |
        ((uint32_t)data[0x8F] << 8) | data[0x90];
      linkState.pitchSlider[playerID - 1] = rawPitchToPercent(rawPitchSlider);
    }

    // Parse effective pitch with jog (32-bit at 0xC3-0xC6)
    // Wire offset 0xC3 = packet data offset 0x99
    if (len > 0x9C && playerID >= 1 && playerID <= 4) {
      uint32_t rawEffectivePitch = ((uint32_t)data[0x99] << 24) | ((uint32_t)data[0x9A] << 16) |
        ((uint32_t)data[0x9B] << 8) | data[0x9C];
      linkState.effectivePitch[playerID - 1] = rawPitchToPercent(rawEffectivePitch);
    }

    // Parse play mode / jog state (byte at 0x9D)
    // Wire offset 0xC7 = packet data offset 0x9D
    if (len > 0x9D && playerID >= 1 && playerID <= 4) {
      uint8_t playMode = data[0x9D];

      // Jog wheel state is ONLY determined by the play mode byte.
      // This tells us HOW the platter is being physically interacted with,
      // independent of how much it affects the pitch/BPM.
      // 
      // 0x09 = Normal playback (no physical jog interaction)
      // 0x0D = Nudge (platter touched and released)
      // 0x01 = Jog/Scratch (platter actively being spun)

      switch (playMode) {
      case 0x09:
        linkState.jogState[playerID - 1] = 0;  // Normal - no touch
        break;
      case 0x0D:
        linkState.jogState[playerID - 1] = 1;  // Nudged - touch and release
        break;
      case 0x01:
        linkState.jogState[playerID - 1] = 2;  // Jogged - actively spinning
        break;
      default:
        // Unknown play mode - leave state unchanged
        // (Don't try to guess based on pitch - that's separate data)
        break;
      }

      // Debug output for pitch and jog state
      if (enablePitchDebug && Serial && playerID == linkState.activePlayerID) {
        const char* jogStateStr[] = { "Normal", "Nudged", "Jogged" };
        float pitchDiff = linkState.effectivePitch[playerID - 1] - linkState.pitchSlider[playerID - 1];
        Serial.printf("[P%u] Mode:0x%02X Jog:%s | Slider:%+6.2f%% Eff:%+7.2f%% (Δ%+7.2f%%)\n",
          playerID,
          playMode,
          jogStateStr[linkState.jogState[playerID - 1]],
          linkState.pitchSlider[playerID - 1],
          linkState.effectivePitch[playerID - 1],
          pitchDiff);
      }
    }

    // Only process timing data for master player
    if (!isMaster) return;

    // Track identification
    if (len > 0x2F) {
      uint32_t rekordboxTrackId = (data[0x2C] << 24) | (data[0x2D] << 16) |
        (data[0x2E] << 8) | data[0x2F];

      // Detect track changes (only for master)
      static uint32_t lastTrackId = 0;
      if (rekordboxTrackId > 0 && rekordboxTrackId != lastTrackId) {
        lastTrackId = rekordboxTrackId;
        linkState.currentTrackId = rekordboxTrackId;

        if (Serial) {
          Serial.printf("[TRACK] Player %u loaded track ID %u\n", playerID, rekordboxTrackId);
        }
      }
    }

    // Beat counter at 0xA2-0xA3
    if (len > 0xA3) {
      linkState.beatsElapsed = (data[0xA2] << 8) | data[0xA3];

      // Calculate track position from beats elapsed and track BPM
      // Position (ms) = (beats * 60000) / trackBPM
      if (linkState.bpm > 0) {
        linkState.trackPositionMs = (linkState.beatsElapsed * 60000) / linkState.bpm;
      }
    }

    // Beat within bar at 0xA6
    if (len > 0xA6) {
      uint8_t beatVal = data[0xA6];
      if (beatVal >= 1 && beatVal <= 4) {
        if (beatVal != linkState.beatInMeasure) {
          linkState.beatInMeasure = beatVal;
          if (beatVal == 1) linkState.triggerBeat = true;

          if (enableBeatPacketDebug && Serial) {
            // Calculate effective BPM and format track position
            float trackBPM = linkState.bpm;
            float effectiveBPM = trackBPM * (1.0f + (linkState.effectivePitch[playerID - 1] / 100.0f));
            String trackPos = formatTrackPosition(linkState.trackPositionMs);
            Serial.printf("Beat %u/4 | %s | BPM: %.1f (track: %.1f) | Eff.Pitch: %+.2f%%\n",
              beatVal, trackPos.c_str(), effectiveBPM, trackBPM, linkState.effectivePitch[playerID - 1]);
          }
        }
      }
    }

    // Store track BPM (from beat packet)
    linkState.origBpm = linkState.bpm;

    // Half-bars counters
    linkState.halfBarsRemaining = data[0x11D];
    linkState.halfBarsElapsed = data[0x11E];
  }

  void parseBeatPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 60) return;
    if (data[0x21] != linkState.activePlayerID) return;

    if (data[0x0A] == TYPE_BEAT_GRID) {
      // BPM (32-bit at 0x38, divide by 10)
      uint32_t rawBpm = (data[0x38] << 24) | (data[0x39] << 16) |
        (data[0x3A] << 8) | data[0x3B];
      if (rawBpm != 0xFFFFFFFF) {
        linkState.bpm = rawBpm / 10.0f;
      }

      // Beat number (32-bit at 0x24)
      linkState.beatNumber = (data[0x24] << 24) | (data[0x25] << 16) |
        (data[0x26] << 8) | data[0x27];

      // Note: Track position is calculated from beats elapsed in status packet,
      // not from beat packets. The 0x30 field in beat packets appears to be
      // something else (possibly next beat timing).
    }
  }

public:
  ProLinkUsermod() : Usermod() { }

  void setup() {
    if (!enabled) return;
    if (udpStatus.listen(PORT_STATUS)) {
      udpStatus.onPacket([this](AsyncUDPPacket packet) { this->parseStatusPacket(packet); });
    }
    if (udpBeat.listen(PORT_BEAT)) {
      udpBeat.onPacket([this](AsyncUDPPacket packet) { this->parseBeatPacket(packet); });
    }
    if (udpAnnounce.listen(PORT_ANNOUNCE)) {
      udpAnnounce.onPacket([this](AsyncUDPPacket packet) { this->parseAnnouncePacket(packet); });
    }
  }

  void loop() {
    if (!enabled) return;

    // Update public variables for all players
    prolink_pitch_slider_player1_public = linkState.pitchSlider[0];
    prolink_pitch_slider_player2_public = linkState.pitchSlider[1];
    prolink_pitch_slider_player3_public = linkState.pitchSlider[2];
    prolink_pitch_slider_player4_public = linkState.pitchSlider[3];

    prolink_effective_pitch_player1_public = linkState.effectivePitch[0];
    prolink_effective_pitch_player2_public = linkState.effectivePitch[1];
    prolink_effective_pitch_player3_public = linkState.effectivePitch[2];
    prolink_effective_pitch_player4_public = linkState.effectivePitch[3];

    prolink_jog_state_player1_public = linkState.jogState[0];
    prolink_jog_state_player2_public = linkState.jogState[1];
    prolink_jog_state_player3_public = linkState.jogState[2];
    prolink_jog_state_player4_public = linkState.jogState[3];

    // Update timing variables
    // linkState.bpm from beat packet is the TRACK BPM (stays constant)
    // Calculate effective BPM by applying the pitch
    if (linkState.activePlayerID > 0 && linkState.activePlayerID <= 4) {
      uint8_t masterIdx = linkState.activePlayerID - 1;
      float effectivePitch = linkState.effectivePitch[masterIdx];
      prolink_bpm_public = linkState.bpm * (1.0f + (effectivePitch / 100.0f));
    } else {
      prolink_bpm_public = linkState.bpm;
    }

    prolink_beat_public = linkState.beatInMeasure;
    prolink_beats_elapsed_public = linkState.beatsElapsed;
    prolink_bars_elapsed_public = linkState.halfBarsElapsed / 2;
    prolink_bars_remaining_public = linkState.halfBarsRemaining / 2;
    prolink_beat_number_public = linkState.beatNumber;
    prolink_track_id_public = linkState.currentTrackId;
    prolink_track_position_ms_public = linkState.trackPositionMs;

    // Calculate beat progress using beat changes
    static unsigned long lastBeatChangeTime = 0;
    static uint8_t lastBeatInMeasure = 0;
    static bool beatChangeProcessed = false;

    if (linkState.beatInMeasure != lastBeatInMeasure && linkState.beatInMeasure > 0) {
      if (!beatChangeProcessed) {
        lastBeatChangeTime = millis();
        lastBeatInMeasure = linkState.beatInMeasure;
        beatChangeProcessed = true;
      }
    } else {
      beatChangeProcessed = false;
    }

    // Calculate progress since last beat change using effective pitch
    if (linkState.bpm > 0 && lastBeatChangeTime > 0) {
      // Use effective pitch (including jog) for accurate beat progress
      uint8_t activePlayer = linkState.activePlayerID;
      if (activePlayer >= 1 && activePlayer <= 4) {
        float effectiveBpm = linkState.bpm * (1.0f + (linkState.effectivePitch[activePlayer - 1] / 100.0f));
        float beatDurationMs = 60000.0f / effectiveBpm;
        unsigned long timeSinceBeat = millis() - lastBeatChangeTime;
        prolink_beat_progress_public = (float)timeSinceBeat / beatDurationMs;

        if (prolink_beat_progress_public > 1.0f) prolink_beat_progress_public = 1.0f;
        if (prolink_beat_progress_public < 0.0f) prolink_beat_progress_public = 0.0f;
      }
    } else {
      prolink_beat_progress_public = 0.0f;
    }

    if (millis() - lastKeepAlive > 1500) {
      sendKeepAlive();
      lastKeepAlive = millis();
    }

    static unsigned long lastBeatTime = 0;
    if (linkState.triggerBeat) {
      linkState.triggerBeat = false;
      lastBeatTime = millis();
      strip.setBrightness(255);
    } else if (millis() - lastBeatTime < 250) {
      uint8_t decay = map(millis() - lastBeatTime, 0, 200, 255, 128);
      strip.setBrightness(decay);
    }
  }

  void addToConfig(JsonObject& root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_beatDebug)] = enableBeatPacketDebug;
    top[FPSTR(_pitchDebug)] = enablePitchDebug;
  }

  bool readFromConfig(JsonObject& root) {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;
    enabled = top[FPSTR(_enabled)] | enabled;
    enableBeatPacketDebug = top[FPSTR(_beatDebug)] | enableBeatPacketDebug;
    enablePitchDebug = top[FPSTR(_pitchDebug)] | enablePitchDebug;
    return true;
  }

  void addToJsonInfo(JsonObject& root) {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    if (enabled) {
      JsonArray infoArr = user.createNestedArray(FPSTR(_name));
      String status = linkState.isConnected ? F("Linked") : F("Scanning");
      if (linkState.isConnected && linkState.activePlayerID > 0) {
        uint8_t idx = linkState.activePlayerID - 1;
        status += " | P" + String(linkState.activePlayerID);
        status += " | " + String(linkState.bpm, 1) + " BPM";
        status += " | " + String(linkState.pitchSlider[idx], 2) + "%";
        if (linkState.jogState[idx] > 0) {
          status += " [JOG]";
        }
      }
      infoArr.add(status);
    }
  }

  void appendConfigData() { oappend(SET_F("addHB('Pro DJ Link');")); }
  uint16_t getId() { return 0xCD30; }
};

const char ProLinkUsermod::_name[] PROGMEM = "Pro DJ Link";
const char ProLinkUsermod::_enabled[] PROGMEM = "Enabled";
const char ProLinkUsermod::_beatDebug[] PROGMEM = "Beat Packet Debug";
const char ProLinkUsermod::_pitchDebug[] PROGMEM = "Pitch Debug";