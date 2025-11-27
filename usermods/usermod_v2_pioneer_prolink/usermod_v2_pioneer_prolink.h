#pragma once

/*
   @title     Pro DJ Link (With Metadata & Phrase Analysis)
   @file      usermod_v2_prolink.h
   @brief     Syncs WLED to Pioneer CDJs including Phrase/Mood analysis
   @target    ESP32 (Requires AsyncUDP)
   @repo      WLED MoonModules
*/

#include "wled.h"
#include <AsyncUDP.h>
#include <WiFiClient.h>
#include <vector>
#include "fcn_declare.h"

#define PORT_ANNOUNCE 50000 
#define PORT_BEAT     50001 
#define PORT_STATUS   50002 
#define PORT_DBSERVER 12523

#define TYPE_ANNOUNCE     0x06 
#define TYPE_STATUS       0x0A  
#define TYPE_BEAT_GRID    0x0B 

#define WLED_DEVICE_NAME  "WLED-Lighting"
#define WLED_DEVICE_ID    0x03

#define PITCH_CENTER 0x10000000  
#define PITCH_SCALE  2684352.0f  

// Retry/Timeout Configuration
#define FETCH_TIMEOUT_MS      3000
#define FETCH_RETRY_DELAY_MS  5000
#define FETCH_MAX_RETRIES     3
#define PEER_TIMEOUT_MS       5000   // Consider peer gone if no announce for 5s (they send every 1.5s)
#define KEEPALIVE_INTERVAL_MS 1500
#define PEER_CHECK_INTERVAL_MS 2000  // How often to check for stale peers
#define BEAT_FLASH_DURATION_MS 80    // How long the beat flash stays bright

// --- Public Variables (Exposed to FX.cpp) ---

// Timing & Sync
volatile float prolink_bpm_public = 0.0;
volatile uint8_t prolink_beat_public = 0;
volatile uint32_t prolink_beat_number_public = 0;
volatile float prolink_beat_progress_public = 0.0;
volatile uint32_t prolink_track_id_public = 0;

// Pitch slider
volatile float prolink_pitchPercent = 0.0f;

// Bar/Beat Counters
volatile uint16_t prolink_beats_elapsed_public = 0;
volatile uint8_t prolink_bars_elapsed_public = 0;
volatile uint8_t prolink_bars_remaining_public = 0;

// Phrase / Structure Public Vars
volatile int prolink_phrase_index_public = -1;
String prolink_phrase_name_public = "";
volatile uint16_t prolink_phrase_beats_public = 0;
volatile float prolink_phrase_progress_public = 0.0;
String prolink_mood_public = "";
volatile uint16_t prolink_total_phrases_public = 0;

// Connection state (public for effects to check)
volatile bool prolink_connected_public = false;

// Beat flash state (public for overlay effect)
volatile bool prolink_beat_flash_active = false;
volatile uint8_t prolink_beat_flash_brightness = 0;

extern std::vector<int> presetPool; // from presets.cpp
extern int presetOffset;

// --- Internal Data Structures ---

struct PhraseEntry {
  uint16_t index;
  uint32_t startBeat;
  uint32_t count;
  uint16_t kind;
  bool isFill;
  String label;
};

struct ProLinkState {
  // UDP Status
  volatile bool    isConnected = false;
  volatile bool    isLoaded = true;
  volatile bool    isMaster = false;
  volatile uint8_t activePlayerID = 0;
  volatile float   bpm = 0.0;
  volatile float   effectivePitch = 0.0;

  // Timing
  volatile uint32_t beatNumber = 0;
  volatile uint8_t  beatInMeasure = 0;

  // Counters
  volatile uint16_t beatsElapsed = 0;
  volatile uint8_t halfBarsRemaining = 0;
  volatile uint8_t halfBarsElapsed = 0;

  // Beat Packets
  volatile uint32_t msToNextBar = 0;
  volatile float    pitchPercent = 0.0f;

  // Track ID
  volatile uint32_t currentTrackId = 0;
  volatile uint8_t  currentSlot = 0;
  IPAddress         activePlayerIP;

  // Peers - track both presence and last seen time
  volatile uint64_t peerMap = 0;
  IPAddress peerIPs[64];
  unsigned long peerLastSeen[64] = { 0 };  // Timestamp of last keepalive/announce from each peer
};

enum FetchState {
  IDLE,
  CONNECT_STAGE1,
  WAIT_STAGE1,
  CONNECT_STAGE2,
  WAIT_STAGE2,
  HANDSHAKE,
  WAIT_HANDSHAKE,
  REQUEST_PHRASES,
  WAIT_PHRASES,
  COOLDOWN,
  RETRY_WAIT
};

class ProLinkUsermod : public Usermod {

private:
  AsyncUDP udpStatus;
  AsyncUDP udpBeat;
  AsyncUDP udpAnnounce;
  WiFiClient dbClient;

  ProLinkState linkState;

  // Phrase Storage
  std::vector<PhraseEntry> phrases;
  uint32_t lastAnalyzedTrackId = 0;

  // State Machine
  FetchState fetchState = IDLE;
  unsigned long stateTimer = 0;
  uint16_t dynamicPort = 0;
  uint32_t txId = 0;

  // Retry tracking
  uint8_t fetchRetryCount = 0;
  uint32_t pendingFetchTrackId = 0;

  // Phrase change tracking
  int previousPhraseIdx = -1;

  const uint8_t TYPE_BEAT = 0x28;
  const float SPEED_UNITY = 1048576.0f; // 0x100000

  // Settings
  bool enabled = true;
  bool enableDebug = false;
  bool enableBeatFlash = false;
  bool enableRandomPreset = false;  // New: random preset on phrase change

  // Config strings
  static const char _name[];
  static const char _enabled[];
  static const char _debug[];
  static const char _beatFlash[];
  static const char _randomPreset[];

  // --- Constants for PSSI Parsing ---
  const uint8_t DBSERVER_MAGIC[5] = { 0x11, 0x87, 0x23, 0x49, 0xae };
  const uint8_t PROLINK_MAGIC[10] = { 0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c };

  const uint8_t XOR_KEY[19] = {
    0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE, 0xE9, 0xD2,
    0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8, 0xE9, 0xF4, 0xE1
  };

  // --- Helpers ---

  float rawPitchToPercent(uint32_t rawPitch) {
    int32_t offset = (int32_t)rawPitch - PITCH_CENTER;
    return (float)offset / PITCH_SCALE;
  }

  uint8_t getActivePeerCount() {
    unsigned long now = millis();
    uint8_t count = 0;

    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) {
        if (now - linkState.peerLastSeen[i] <= PEER_TIMEOUT_MS) {
          count++;
        }
      }
    }
    return count;
  }

  void sendKeepAlive() {
    if (!Network.isConnected()) return;

    uint8_t pkg[64];
    memset(pkg, 0, 64);
    memcpy(pkg, PROLINK_MAGIC, 10);
    pkg[0x0A] = TYPE_ANNOUNCE;
    snprintf((char*)&pkg[0x0C], 20, "%s", WLED_DEVICE_NAME);
    pkg[0x20] = 0x01; pkg[0x21] = 0x02; pkg[0x23] = 0x36;
    pkg[0x24] = WLED_DEVICE_ID; pkg[0x25] = 0x01;
    Network.localMAC(&pkg[0x26]);
    uint32_t ip = (uint32_t)Network.localIP();
    memcpy(&pkg[0x2C], &ip, 4);
    pkg[0x30] = getActivePeerCount() + 1; // add 1 for ourselves.
    pkg[0x34] = 0x01;

    // Always broadcast to subnet even if no peers known yet
    IPAddress broadcast = Network.localIP();
    broadcast[3] = 255;
    udpAnnounce.writeTo(pkg, 54, broadcast, PORT_ANNOUNCE);

    IPAddress lastip = INADDR_NONE;
    // Also send to known peers...
    // in the case of the XDJ-AZ, all devices are 
    // the same IP so we don't need to spam it.
    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) {
        if (linkState.peerIPs[i][0] != 0) {
          if (lastip != linkState.peerIPs[i]) {
            udpAnnounce.writeTo(pkg, 54, linkState.peerIPs[i], PORT_ANNOUNCE);
            lastip = linkState.peerIPs[i];
          }
        }
      }
    }
  }

  void parseAnnouncePacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 36) return;
    if (data[0x0A] == TYPE_ANNOUNCE) {
      uint8_t devID = data[0x24];
      if (devID > 0 && devID < 64 && devID != WLED_DEVICE_ID) {
        unsigned long now = millis();
        uint8_t peerIdx = devID - 1;
        bool wasKnown = (linkState.peerMap & ((uint64_t)1 << peerIdx)) != 0;
        bool wasTimedOut = wasKnown && (now - linkState.peerLastSeen[peerIdx] > PEER_TIMEOUT_MS);
        bool isNew = !wasKnown;

        // Update peer info
        linkState.peerMap |= ((uint64_t)1 << peerIdx);
        linkState.peerIPs[peerIdx] = packet.remoteIP();
        linkState.peerLastSeen[peerIdx] = now;
        linkState.isConnected = true;

        if (isNew) {
          if (enableDebug) Serial.printf("[ProLink] New peer: Player %d at %s\n", devID, packet.remoteIP().toString().c_str());
          sendKeepAlive();
        } else if (wasTimedOut) {
          // Peer came back after being gone
          if (enableDebug) Serial.printf("[ProLink] Peer returned: Player %d at %s\n", devID, packet.remoteIP().toString().c_str());

          // If this was our active player, we may need to re-fetch phrase data
          if (devID == linkState.activePlayerID && linkState.currentTrackId > 0) {
            if (enableDebug) Serial.println(F("[ProLink] Active player returned, will re-fetch phrases if needed"));
            // Reset fetch state so we can try again
            if (lastAnalyzedTrackId != linkState.currentTrackId) {
              fetchRetryCount = 0;
              pendingFetchTrackId = linkState.currentTrackId;
              cleanupTcpConnection();
              fetchState = IDLE;
              startMetadataFetch();
            }
          }
          sendKeepAlive();
        }
      }
    }
  }

  void parseStatusPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 0x11F || data[0x0A] != TYPE_STATUS) return;

    uint8_t playerID = data[0x21];
    bool isMaster = (data[0x89] & 0x20) != 0;

    // Update peer lastSeen - status packets also prove the peer is alive
    if (playerID > 0 && playerID < 64) {
      uint8_t peerIdx = playerID - 1;
      if (linkState.peerMap & ((uint64_t)1 << peerIdx)) {
        linkState.peerLastSeen[peerIdx] = millis();
      }
    }

    if (isMaster) {
      linkState.activePlayerID = playerID;
      linkState.isMaster = true;
      linkState.activePlayerIP = packet.remoteIP();

      // Pitch
      if (packet.length() > 0x9C) {
        uint32_t rawEff = ((uint32_t)data[0x99] << 24) | ((uint32_t)data[0x9A] << 16) |
          ((uint32_t)data[0x9B] << 8) | data[0x9C];
        linkState.effectivePitch = rawPitchToPercent(rawEff);
      }

      // Track ID
      if (packet.length() > 0x2F) {
        uint32_t tid = (data[0x2C] << 24) | (data[0x2D] << 16) | (data[0x2E] << 8) | data[0x2F];
        if (tid > 0 && tid != linkState.currentTrackId) {
          linkState.currentTrackId = tid;
          linkState.currentSlot = data[0x28];

          if (tid != lastAnalyzedTrackId) {
            // New track - reset and start fetch
            fetchRetryCount = 0;
            pendingFetchTrackId = tid;
            cleanupTcpConnection();
            fetchState = IDLE;
            startMetadataFetch();
          }
        }
      }

      // Beat in Measure
      if (packet.length() > 0xA6) linkState.beatInMeasure = data[0xA6];

      // Beats Elapsed
      if (packet.length() > 0xA3) {
        linkState.beatsElapsed = (data[0xA2] << 8) | data[0xA3];
      }

      // Half-bars Remaining
      if (packet.length() > 0x11D) {
        linkState.halfBarsRemaining = data[0x11D];
      }

      // Half-bars Elapsed
      if (packet.length() > 0x11E) {
        linkState.halfBarsElapsed = data[0x11E];
      }
    }
  }

  void parseBeatPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 60) return;

    uint8_t playerID = data[0x21];
    if (playerID != linkState.activePlayerID) return;

    // Update peer lastSeen - beat packets also prove the peer is alive
    if (playerID > 0 && playerID < 64) {
      uint8_t peerIdx = playerID - 1;
      if (linkState.peerMap & ((uint64_t)1 << peerIdx)) {
        linkState.peerLastSeen[peerIdx] = millis();
      }
    }

    if (data[0x0A] == TYPE_BEAT) {
      // --- 0x28 PACKET PARSING ---

      // 1. Live BPM Calc
      uint16_t baseBpmRaw = (data[0x5A] << 8) | data[0x5B];
      uint32_t speedRaw = (data[0x54] << 24) | (data[0x55] << 16) | (data[0x56] << 8) | data[0x57];

      float baseBpm = baseBpmRaw / 100.0f;
      float speedRatio = speedRaw / SPEED_UNITY;
      float currentBpm = baseBpm * speedRatio;

      // 2. Rolling Grid Data (Timers to next markers)
      uint32_t msToNextBar = (data[0x2C] << 24) | (data[0x2D] << 16) | (data[0x2E] << 8) | data[0x2F];
      uint32_t msTo2ndBar = (data[0x34] << 24) | (data[0x35] << 16) | (data[0x36] << 8) | data[0x37];

      // 3. Intervals (Static durations)
      uint32_t msBeat = (data[0x24] << 24) | (data[0x25] << 16) | (data[0x26] << 8) | data[0x27];
      uint32_t msBar = (data[0x30] << 24) | (data[0x31] << 16) | (data[0x32] << 8) | data[0x33];

      // 4. Status
      uint8_t beat = data[0x5C];

      // Update Link State
      linkState.bpm = currentBpm;
      linkState.beatNumber = beat;
      linkState.msToNextBar = msToNextBar; // Use for syncing

      // Calculated Pitch % for display
      linkState.pitchPercent = (speedRatio - 1.0f) * 100.0f;
      prolink_pitchPercent = linkState.pitchPercent;
      
    }
  }

  // --- TCP Connection Management ---

  void cleanupTcpConnection() {
    if (dbClient.connected()) {
      dbClient.stop();
    }
    // Clear any pending data
    while (dbClient.available()) dbClient.read();
  }

  void startMetadataFetch() {
    if (linkState.activePlayerIP[0] == 0) {
      if (enableDebug) Serial.println(F("[ProLink] No active player IP, skipping fetch"));
      return;
    }

    if (fetchRetryCount >= FETCH_MAX_RETRIES) {
      if (enableDebug) Serial.printf("[ProLink] Max retries (%d) reached for track %d\n", FETCH_MAX_RETRIES, pendingFetchTrackId);
      fetchState = IDLE;
      return;
    }

    fetchState = CONNECT_STAGE1;
    stateTimer = millis();
    if (enableDebug) Serial.printf("[ProLink] Starting Phrase Fetch (attempt %d/%d)...\n", fetchRetryCount + 1, FETCH_MAX_RETRIES);
  }

  void scheduleFetchRetry() {
    fetchRetryCount++;
    if (fetchRetryCount < FETCH_MAX_RETRIES) {
      fetchState = RETRY_WAIT;
      stateTimer = millis();
      if (enableDebug) Serial.printf("[ProLink] Scheduling retry %d/%d in %dms\n", fetchRetryCount + 1, FETCH_MAX_RETRIES, FETCH_RETRY_DELAY_MS);
    } else {
      if (enableDebug) Serial.println(F("[ProLink] All retries exhausted"));
      fetchState = IDLE;
    }
  }

  void sendDBQuery(uint16_t type, uint32_t* args, uint8_t argCount) {
    if (!dbClient.connected()) return;
    txId++;

    dbClient.write(DBSERVER_MAGIC, 5);
    uint8_t head[] = { 0x11, 0,0,0,0, 0x10, 0,0, 0x0F, argCount };

    head[1] = (txId >> 24) & 0xFF; head[2] = (txId >> 16) & 0xFF;
    head[3] = (txId >> 8) & 0xFF;  head[4] = txId & 0xFF;
    head[6] = (type >> 8) & 0xFF;  head[7] = type & 0xFF;

    dbClient.write(head, 10);

    uint8_t argHead[] = { 0x14, 0,0,0,0x0C };
    dbClient.write(argHead, 5);

    for (int i = 0; i < argCount; i++) dbClient.write((uint8_t)0x06);
    for (int i = argCount; i < 12; i++) dbClient.write((uint8_t)0x00);

    for (int i = 0; i < argCount; i++) {
      dbClient.write((uint8_t)0x11);
      uint32_t val = args[i];
      uint8_t b[] = { (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val };
      dbClient.write(b, 4);
    }
  }

  bool readBytesWithTimeout(uint8_t* buf, size_t len, int timeoutMs = 500) {
    unsigned long start = millis();
    size_t bytesRead = 0;
    while (bytesRead < len) {
      if (millis() - start > (unsigned long)timeoutMs) return false;
      if (dbClient.available()) {
        buf[bytesRead++] = dbClient.read();
      } else {
        delay(1);
      }
    }
    return true;
  }

  String getPhraseName(uint16_t kind, uint16_t mood, uint8_t k1, uint8_t k2, uint8_t k3) {
    if (mood == 1 || mood == 0) {
      switch (kind) {
      case 1: return (k1 == 1) ? F("Intro 1") : F("Intro 2");
      case 2:
        if (k2 == 0 && k3 == 0) return F("Up 1");
        if (k2 == 0 && k3 == 1) return F("Up 2");
        if (k2 == 1 && k3 == 0) return F("Up 3");
        return F("Up");
      case 3: return F("Down");
      case 5: return (k1 == 1) ? F("Chorus 2") : F("Chorus 1");
      case 6: return (k1 == 1) ? F("Outro 1") : F("Outro 2");
      default: return "High " + String(kind);
      }
    } else if (mood == 2) {
      switch (kind) {
      case 1: return F("Intro");
      case 2: return F("Verse 1");
      case 3: return F("Verse 2");
      case 4: return F("Verse 3");
      case 5: return F("Verse 4");
      case 6: return F("Verse 5");
      case 7: return F("Verse 6");
      case 8: return F("Bridge");
      case 9: return F("Chorus");
      case 10: return F("Outro");
      default: return "Mid " + String(kind);
      }
    } else if (mood == 3) {
      switch (kind) {
      case 1: return F("Intro");
      case 2: return F("Verse 1");
      case 3: return F("Verse 1");
      case 4: return F("Verse 1");
      case 5: return F("Verse 2");
      case 6: return F("Verse 2");
      case 7: return F("Verse 2");
      case 8: return F("Bridge");
      case 9: return F("Chorus");
      case 10: return F("Outro");
      default: return "Low " + String(kind);
      }
    }
    return String(kind);
  }

  void handleFetchStateMachine() {
    // Handle retry wait state
    if (fetchState == RETRY_WAIT) {
      if (millis() - stateTimer > FETCH_RETRY_DELAY_MS) {
        startMetadataFetch();
      }
      return;
    }

    if (fetchState == IDLE || fetchState == COOLDOWN) {
      if (fetchState == COOLDOWN && millis() - stateTimer > 1000) {
        fetchState = IDLE;
      }
      return;
    }

    // Timeout handling
    if (millis() - stateTimer > FETCH_TIMEOUT_MS) {
      if (enableDebug) Serial.println(F("[ProLink] Fetch Timeout"));
      cleanupTcpConnection();
      scheduleFetchRetry();
      return;
    }

    switch (fetchState) {
    case CONNECT_STAGE1: {
      // Protect against invalid IP
      if (linkState.activePlayerIP[0] == 0) {
        if (enableDebug) Serial.println(F("[ProLink] Player IP invalid, aborting"));
        fetchState = IDLE;
        return;
      }

      if (dbClient.connect(linkState.activePlayerIP, PORT_DBSERVER)) {
        dbClient.write("\x00\x00\x00\x0fRemoteDBServer\x00", 19);
        fetchState = WAIT_STAGE1;
        stateTimer = millis();
      } else {
        if (enableDebug) Serial.println(F("[ProLink] Stage1 connect failed"));
        scheduleFetchRetry();
      }
      break;
    }

    case WAIT_STAGE1:
      if (dbClient.available() >= 2) {
        uint8_t buf[2];
        dbClient.read(buf, 2);
        dynamicPort = (buf[0] << 8) | buf[1];
        dbClient.stop();
        fetchState = CONNECT_STAGE2;
        stateTimer = millis();
        if (enableDebug) Serial.printf("[ProLink] Port: %u\n", dynamicPort);
      }
      break;

    case CONNECT_STAGE2:
      if (dbClient.connect(linkState.activePlayerIP, dynamicPort)) {
        uint8_t hand1[] = { 0x11, 0,0,0,1 };
        dbClient.write(hand1, 5);
        fetchState = HANDSHAKE;
        stateTimer = millis();
      } else {
        if (enableDebug) Serial.println(F("[ProLink] Stage2 connect failed"));
        scheduleFetchRetry();
      }
      break;

    case HANDSHAKE:
      if (dbClient.available()) {
        while (dbClient.available()) dbClient.read();

        uint8_t pkt[60];
        memset(pkt, 0, 60);
        memcpy(pkt, DBSERVER_MAGIC, 5);
        uint8_t mid[] = { 0x11, 0xFF,0xFF,0xFF,0xFE, 0x10, 0,0, 0x0F, 0x01, 0x14, 0,0,0,0x0C };
        memcpy(&pkt[5], mid, 15);
        pkt[20] = 0x06;
        pkt[32] = 0x11;
        uint32_t pid = WLED_DEVICE_ID;
        pkt[33] = (pid >> 24); pkt[34] = (pid >> 16); pkt[35] = (pid >> 8); pkt[36] = pid;

        dbClient.write(pkt, 37);
        fetchState = WAIT_HANDSHAKE;
        stateTimer = millis();
      }
      break;

    case WAIT_HANDSHAKE:
      if (dbClient.available() > 10) {
        while (dbClient.available()) dbClient.read();
        fetchState = REQUEST_PHRASES;
        stateTimer = millis();
      }
      break;

    case REQUEST_PHRASES: {
      uint32_t compound = (WLED_DEVICE_ID << 24) | (linkState.currentSlot << 16) | 0x0301;
      uint32_t pssi = 0x49535350; // "ISSP"
      uint32_t ext = 0x00545845;  // "\0TXE"
      uint32_t args[] = { compound, linkState.currentTrackId, pssi, ext };
      sendDBQuery(0x2c04, args, 4);
      fetchState = WAIT_PHRASES;
      stateTimer = millis();
      break;
    }

    case WAIT_PHRASES:
      if (dbClient.available() > 32) {
        bool found = false;
        int searchLimit = 1000; // Prevent infinite loop
        while (dbClient.available() >= 4 && searchLimit-- > 0) {
          if (dbClient.peek() == 'P') {
            uint8_t tag[4];
            dbClient.read(tag, 4);
            if (memcmp(tag, "PSSI", 4) == 0) {
              found = true;
              break;
            }
          } else {
            dbClient.read();
          }
        }

        if (found) {
          if (parsePSSI()) {
            if (enableDebug) printPhrases();
            cleanupTcpConnection();
            fetchState = IDLE;
            lastAnalyzedTrackId = linkState.currentTrackId;
            fetchRetryCount = 0;
            if (enableDebug) Serial.println(F("[ProLink] PSSI Parsed Success"));
          } else {
            if (enableDebug) Serial.println(F("[ProLink] PSSI Parse Failed"));
            cleanupTcpConnection();
            scheduleFetchRetry();
          }
        }
      }
      break;

    default: break;
    }
  }

  void printPhrases() {
    Serial.println("=== Parsed PSSI Phrases ===");
    for (size_t i = 0; i < phrases.size(); i++) {
      PhraseEntry& pe = phrases[i];
      USER_PRINTF("Phrase%3d : Start:%5d  Count:%4d  %s %s\n", i, pe.startBeat, pe.count, prolink_mood_public, pe.label);
    }
  }

  // --- Example usage in your phrase change logic ---
  void onPhraseChange(int activeIdx, int previousPhraseIdx) {
    auto pool = buildPresetPool();
    int newPreset = getPresetForPhraseNoRepeat(activeIdx, pool);

    if (newPreset > 0) {
      USER_PRINTF("[ProLink] Phrase %d -> %d. Applying Preset %d (%s)\n",
        previousPhraseIdx, activeIdx,
        newPreset, presetCache[newPreset].name);

      if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
      if (currentPlaylist >= 0) unloadPlaylist();
      applyPreset(newPreset);
      handlePresets();
    }
  }
  
  bool parsePSSI() {
    uint8_t head[28];
    if (!readBytesWithTimeout(head, 28)) return false;

    uint32_t headerLen = (head[0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3];
    uint16_t numEntries = (head[12] << 8) | head[13];

    // Sanity checks
    if (headerLen > 10000 || numEntries > 500) {
      if (enableDebug) Serial.printf("[ProLink] Invalid PSSI header: len=%d entries=%d\n", headerLen, numEntries);
      return false;
    }

    // Decrypt Header fields
    for (int i = 14; i < 28; i++) {
      head[i] = head[i] ^ ((XOR_KEY[(i - 14) % 19] + numEntries) & 0xFF);
    }

    uint16_t mood = (head[14] << 8) | head[15];
    switch (mood) {
    case 1: prolink_mood_public = "High"; break;
    case 2: prolink_mood_public = "Mid"; break;
    case 3: prolink_mood_public = "Low"; break;
    default: prolink_mood_public = "Unknown";
    }

    phrases.clear();

    // Read remaining header
    int readSoFar = 4 + 28;
    while (readSoFar < (int)headerLen) {
      if (!dbClient.available()) {
        delay(1);
        if (millis() - stateTimer > FETCH_TIMEOUT_MS) return false;
      }
      dbClient.read();
      readSoFar++;
    }

    // Read Entries
    for (int i = 0; i < numEntries; i++) {
      uint8_t e[24];
      if (!readBytesWithTimeout(e, 24)) return false;

      // Decrypt Entry
      for (int b = 0; b < 24; b++) {
        int tagOffset = headerLen + (i * 24) + b;
        e[b] = e[b] ^ ((XOR_KEY[(tagOffset - 18) % 19] + numEntries) & 0xFF);
      }

      uint16_t kind = (e[4] << 8) | e[5];
      uint16_t beat = (e[2] << 8) | e[3];
      bool fill = (e[21] != 0);
      uint16_t beatFill = (e[22] << 8) | e[23];

      PhraseEntry pe;
      pe.kind = kind;
      pe.startBeat = beat;
      pe.count = 0;
      pe.isFill = false;
      pe.label = getPhraseName(kind, mood, e[7], e[9], e[19]);

      if (fill && beatFill > beat) {
        pe.count = beatFill - beat;
        phrases.push_back(pe);

        PhraseEntry peFill;
        peFill.kind = kind;
        peFill.startBeat = beatFill;
        peFill.count = 0;
        peFill.isFill = true;
        peFill.label = pe.label + " Fill";
        phrases.push_back(peFill);
      } else {
        phrases.push_back(pe);
      }
    }

    // Fix durations
    for (size_t i = 0; i < phrases.size(); i++) {
      if (i < phrases.size() - 1) {
        phrases[i].count = phrases[i + 1].startBeat - phrases[i].startBeat;
      } else {
        phrases[i].count = 32;
      }
    }
    prolink_total_phrases_public = phrases.size();

    if (prolink_total_phrases_public > 0) {

      auto pool = buildPresetPool();

      if (!pool.empty()) {
        prolink_presetOffset = random(pool.size()); // randomized start
        USER_PRINTF("[ProLink] Initialized preset offset: %d\n", prolink_presetOffset);
      }

    }

    return true;
  }

  void updatePhraseState() {
    if (phrases.empty()) {
      prolink_phrase_name_public = "";
      prolink_phrase_index_public = -1;
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
      return;
    }

    uint32_t currentBeat = linkState.beatsElapsed;
    if (currentBeat < 1) currentBeat = 1;

    int activeIdx = -1;
    for (size_t i = 0; i < phrases.size(); i++) {
      if (currentBeat >= phrases[i].startBeat && currentBeat < (phrases[i].startBeat + phrases[i].count)) {
        activeIdx = i;
        break;
      }
    }

    // Deterministic preset on phrase change
    if (enableRandomPreset && activeIdx != previousPhraseIdx && previousPhraseIdx != -1 && activeIdx != -1) {
      auto pool = buildPresetPool(); // collect valid presets from presetCache
      int newPreset = getPresetForPhraseNoRepeat(activeIdx, pool); // uses prolink_presetOffset internally

      // hard-coded for testing. 2 is the MM mascot pulsing, looks good for fills

      if (phrases[activeIdx].label.indexOf("Fill") != -1) {
        newPreset = 2;
      } else if (newPreset == 2) {
        if (!pool.empty()) {
          int attempts = 0;
          int candidate;
          do {
            candidate = pool[random(pool.size())];
            attempts++;
          } while (candidate == 2 && attempts < 10);
          newPreset = candidate;
        }
      }
    
      if (newPreset > 0) {
        if (enableDebug) {
          Serial.printf("[ProLink] Phrase %d -> %d. Applying Preset %d (%s)\n",
            previousPhraseIdx, activeIdx,
            newPreset, presetCache[newPreset].name);
        }
        if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
        if (currentPlaylist >= 0) unloadPlaylist();
        applyPreset(newPreset);
        handlePresets();
      }
    }
    previousPhraseIdx = activeIdx;

    if (activeIdx != -1) {
      prolink_phrase_index_public = activeIdx;
      prolink_phrase_name_public = phrases[activeIdx].label;
      prolink_phrase_beats_public = phrases[activeIdx].count;

      uint32_t beatsIntoPhrase = currentBeat - phrases[activeIdx].startBeat;
      float totalProgress = (float)beatsIntoPhrase + prolink_beat_progress_public;
      if (phrases[activeIdx].count > 0) {
        prolink_phrase_progress_public = totalProgress / (float)phrases[activeIdx].count;
      }
    } else {
      prolink_phrase_index_public = -1;
      prolink_phrase_name_public = "Gap/End";
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
    }
  }

  // Check for peers that have stopped sending keepalives
  void checkPeerTimeouts() {
    unsigned long now = millis();

    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) {
        if (linkState.peerLastSeen[i] > 0 && (now - linkState.peerLastSeen[i]) > PEER_TIMEOUT_MS) {
          uint8_t devID = i + 1;

          if (enableDebug) Serial.printf("[ProLink] Peer timeout: Player %d\n", devID);

          // If this was our active/master player, reset state
          if (devID == linkState.activePlayerID) {
            if (enableDebug) Serial.println(F("[ProLink] Active player went away"));

            linkState.isMaster = false;
            linkState.activePlayerID = 0;
            linkState.bpm = 0;
            linkState.beatNumber = 0;
            linkState.beatsElapsed = 0;
            linkState.currentTrackId = 0;

            // Clear phrases
            phrases.clear();
            lastAnalyzedTrackId = 0;
            prolink_phrase_name_public = "";
            prolink_phrase_index_public = -1;
            prolink_mood_public = "";
            previousPhraseIdx = -1;

            // Cancel any pending fetch
            cleanupTcpConnection();
            fetchState = IDLE;
            fetchRetryCount = 0;

            prolink_connected_public = false;
          }

          // Mark peer as gone (but keep the IP in case it comes back)
          // We don't clear peerMap bit - parseAnnouncePacket will detect it as "returning"
          // by checking if lastSeen is stale
        }
      }
    }

    // Update overall connection status
    bool anyPeerAlive = false;
    for (int i = 0; i < 64; i++) {
      if ((linkState.peerMap & ((uint64_t)1 << i)) &&
        linkState.peerLastSeen[i] > 0 &&
        (now - linkState.peerLastSeen[i]) <= PEER_TIMEOUT_MS) {
        anyPeerAlive = true;
        break;
      }
    }
    linkState.isConnected = anyPeerAlive;
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

    // Send initial keepalive to announce ourselves even before seeing any peers
    sendKeepAlive();

    if (enableDebug) Serial.println(F("[ProLink] Initialized and listening"));
  }

  float getEffectiveBPM() {
    if (linkState.activePlayerID > 0 && linkState.activePlayerID <= 4) {
      float effectivePitch = linkState.effectivePitch;
      return linkState.bpm * (1.0f + (effectivePitch / 100.0f));
    }
    return linkState.bpm;
  }

  void loop() {
    if (!enabled) return;

    // Check for peer timeouts periodically
    static unsigned long lastPeerCheck = 0;
    if (millis() - lastPeerCheck > PEER_CHECK_INTERVAL_MS) {
      checkPeerTimeouts();
      lastPeerCheck = millis();
    }

    // Handle Metadata TCP State Machine
    handleFetchStateMachine();

    // Beat progress calculation
    static unsigned long lastBeatTime = 0;
    static uint32_t lastBeatNumber = 0;
    static unsigned long beatFlashStart = 0;

    if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) {
      lastBeatTime = millis();
      lastBeatNumber = linkState.beatNumber;

      // Trigger beat flash
      if (enableBeatFlash) {
        beatFlashStart = millis();
        prolink_beat_flash_active = true;
        prolink_beat_flash_brightness = 255;
      }
    }

    // Update beat flash decay
    if (enableBeatFlash && prolink_beat_flash_active) {
      unsigned long flashElapsed = millis() - beatFlashStart;
      if (flashElapsed < BEAT_FLASH_DURATION_MS) {
        // Quick attack, exponential decay
        prolink_beat_flash_brightness = 255 - (uint8_t)((flashElapsed * 255) / BEAT_FLASH_DURATION_MS);
      } else {
        prolink_beat_flash_active = false;
        prolink_beat_flash_brightness = 0;
      }
    }

    if (linkState.bpm > 0 && lastBeatTime > 0) {
      float effectiveBpm = getEffectiveBPM();
      float beatDurationMs = 60000.0f / effectiveBpm;
      unsigned long timeSinceBeat = millis() - lastBeatTime;

      float progress = timeSinceBeat / beatDurationMs;
      while (progress >= 1.0f) {
        progress -= 1.0f;
        lastBeatTime += (unsigned long)beatDurationMs;
      }

      prolink_beat_progress_public = progress;
    } else {
      prolink_beat_progress_public = 0.0f;
    }

    // Update phrase state
    updatePhraseState();

    // Update Public Vars
    prolink_bpm_public = getEffectiveBPM();
    prolink_beat_public = linkState.beatInMeasure;
    prolink_beat_number_public = linkState.beatNumber;
    prolink_track_id_public = linkState.currentTrackId;
    prolink_beats_elapsed_public = linkState.beatsElapsed;
    prolink_bars_elapsed_public = linkState.halfBarsElapsed / 2;
    prolink_bars_remaining_public = linkState.halfBarsRemaining / 2;
    prolink_connected_public = linkState.isMaster;

    // Keepalive - always send, even without peers
    static unsigned long lastKA = 0;
    if (millis() - lastKA > KEEPALIVE_INTERVAL_MS) {
      sendKeepAlive();
      lastKA = millis();
    }
  }

  void addToConfig(JsonObject& root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_debug)] = enableDebug;
    top[FPSTR(_beatFlash)] = enableBeatFlash;
    top[FPSTR(_randomPreset)] = enableRandomPreset;
  }

  bool readFromConfig(JsonObject& root) {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) {
      if (enableDebug) Serial.println(F("[ProLink] No config found, using defaults"));
      return false;
    }

    enabled = top[FPSTR(_enabled)] | enabled;
    enableDebug = top[FPSTR(_debug)] | enableDebug;
    enableBeatFlash = top[FPSTR(_beatFlash)] | enableBeatFlash;
    enableRandomPreset = top[FPSTR(_randomPreset)] | enableRandomPreset;

    if (enableDebug) {
      Serial.printf("[ProLink] Config: enabled=%d debug=%d beatFlash=%d randomPreset=%d\n",
        enabled, enableDebug, enableBeatFlash, enableRandomPreset);
    }
    return true;
  }

  void addToJsonInfo(JsonObject& root) {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    if (enabled) {
      JsonArray infoArr = user.createNestedArray("Pro DJ Link");

      String status;
      if (linkState.isMaster && linkState.activePlayerID > 0) {
        status = "M" + String(linkState.activePlayerID);
        status += " | " + String(getEffectiveBPM(), 1) + " BPM";
      } else if (linkState.isConnected) {
        // Count active peers
        int peerCount = 0;
        unsigned long now = millis();
        for (int i = 0; i < 64; i++) {
          if ((linkState.peerMap & ((uint64_t)1 << i)) &&
            linkState.peerLastSeen[i] > 0 &&
            (now - linkState.peerLastSeen[i]) <= PEER_TIMEOUT_MS) {
            peerCount++;
          }
        }
        status = String(peerCount) + " peer(s), waiting for master";
      } else {
        status = "Listening...";
      }
      infoArr.add(status);

      if (prolink_phrase_name_public.length() > 0 && linkState.isMaster) {
        infoArr.add("<br />" + prolink_mood_public + " " + prolink_phrase_name_public);
      }

      // Show fetch status if in progress
      if (fetchState != IDLE && fetchState != COOLDOWN) {
        String fetchStatus = "<br />Fetching phrases";
        if (fetchRetryCount > 0) {
          fetchStatus += " (retry " + String(fetchRetryCount) + "/" + String(FETCH_MAX_RETRIES) + ")";
        }
        infoArr.add(fetchStatus);
      }
    }
  }

  uint16_t getId() { return 0xCD30; }

};

// --- Static Definitions ---
const char ProLinkUsermod::_name[] PROGMEM = "Pro DJ Link";
const char ProLinkUsermod::_enabled[] PROGMEM = "Enabled";
const char ProLinkUsermod::_debug[] PROGMEM = "Enable Debug";
const char ProLinkUsermod::_beatFlash[] PROGMEM = "Beat Flash";
const char ProLinkUsermod::_randomPreset[] PROGMEM = "Random Preset on Phrase";