#pragma once

/*
   @title     Pro DJ Link (With Metadata, Waveform & Artwork)
   @file      usermod_v2_prolink.h
   @brief     Syncs WLED to Pioneer CDJs including Phrase/Mood analysis,
              track metadata, preview waveform, and album artwork
   @target    ESP32 (Requires AsyncUDP, PSRAM recommended for waveform/artwork)
   @repo      WLED MoonModules

   FIXED VERSION: Improved timing for waveform/artwork fetch
*/

#include "wled.h"
#include <AsyncUDP.h>
#include <WiFiClient.h>
#include <vector>
#include "fcn_declare.h"
#include "esp_heap_caps.h"

#define PORT_ANNOUNCE 50000 
#define PORT_BEAT     50001 
#define PORT_STATUS   50002 
#define PORT_DBSERVER 12523

#define TYPE_ANNOUNCE     0x06 
#define TYPE_STATUS       0x0A  
#define TYPE_BEAT_GRID    0x0B 

#define WLED_DEVICE_NAME  "WLED-Lighting"
#define WLED_DEVICE_ID_DEFAULT 0x03

#define PITCH_CENTER 0x10000000  
#define PITCH_SCALE  2684352.0f  

// Retry/Timeout Configuration - INCREASED timeouts
#define FETCH_TIMEOUT_MS      5000   // Increased from 3000
#define FETCH_RETRY_DELAY_MS  5000
#define FETCH_MAX_RETRIES     3
#define PEER_TIMEOUT_MS       5000
#define KEEPALIVE_INTERVAL_MS 1500
#define PEER_CHECK_INTERVAL_MS 2000
#define BEAT_FLASH_DURATION_MS 80

// Data collection timeouts - NEW
#define WAVEFORM_COLLECT_MS   1500   // Time to collect waveform data (PWV4 preview ~7KB)
#define ARTWORK_COLLECT_MS    3000   // Time to collect artwork data
#define METADATA_COLLECT_MS   500    // Time to collect metadata

// Buffer sizes
#define METADATA_BUFFER_MAX   8192
#define WAVEFORM_BUFFER_MAX   4096   // Max waveform points to store
#define ARTWORK_BUFFER_MAX    (256 * 1024)  // 256KB max for album art JPEG

// --- Waveform Data Structure ---
struct WaveformPoint {
  uint8_t height;  // 0-127 amplitude (PWV4 full resolution)
  uint8_t color;   // Pioneer color index 0-7
};

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

// Connection state
volatile bool prolink_connected_public = false;

// Beat flash state
volatile bool prolink_beat_flash_active = false;
volatile uint8_t prolink_beat_flash_brightness = 0;

// Track Metadata
String prolink_track_title = "";
String prolink_track_artist = "";
String prolink_track_album = "";
String prolink_track_key = "";
String prolink_track_genre = "";
String prolink_track_label = "";
volatile bool prolink_metadata_valid = false;

// Preview Waveform (PSRAM allocated)
WaveformPoint* prolink_waveform_data = nullptr;
volatile uint16_t prolink_waveform_length = 0;
volatile bool prolink_waveform_valid = false;

// Album Artwork (PSRAM allocated, raw JPEG)
uint8_t* prolink_artwork_data = nullptr;
volatile uint32_t prolink_artwork_size = 0;
volatile bool prolink_artwork_valid = false;

// Track progress (0.0 - 1.0)
volatile float prolink_track_progress = 0.0f;
volatile uint32_t prolink_total_beats = 0;

extern std::vector<int> presetPool;
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
  volatile bool    isConnected = false;
  volatile bool    isLoaded = true;
  volatile bool    isMaster = false;
  volatile uint8_t activePlayerID = 0;
  volatile float   bpm = 0.0;
  volatile float   effectivePitch = 0.0;

  volatile uint32_t beatNumber = 0;
  volatile uint8_t  beatInMeasure = 0;

  volatile uint16_t beatsElapsed = 0;
  volatile uint8_t halfBarsRemaining = 0;
  volatile uint8_t halfBarsElapsed = 0;

  volatile uint32_t msToNextBar = 0;
  volatile float    pitchPercent = 0.0f;

  volatile uint32_t currentTrackId = 0;
  volatile uint8_t  currentSlot = 0;
  IPAddress         activePlayerIP;

  volatile uint64_t peerMap = 0;
  IPAddress peerIPs[64];
  unsigned long peerLastSeen[64] = { 0 };
};

enum FetchState {
  IDLE,
  CONNECT_STAGE1,
  WAIT_STAGE1,
  CONNECT_STAGE2,
  WAIT_STAGE2,
  HANDSHAKE,
  WAIT_HANDSHAKE,
  // PSSI first (highest priority)
  REQUEST_PHRASES,
  WAIT_PHRASES,
  // Then metadata
  REQUEST_METADATA_SETUP,
  WAIT_METADATA_SETUP,
  REQUEST_METADATA_RENDER,
  WAIT_METADATA_RENDER,
  COLLECT_METADATA,      // NEW: collecting metadata data
  // Then waveform
  REQUEST_WAVEFORM,
  WAIT_WAVEFORM,
  COLLECT_WAVEFORM,      // NEW: collecting waveform data
  // Finally artwork
  REQUEST_ARTWORK,
  WAIT_ARTWORK,
  COLLECT_ARTWORK,       // NEW: collecting artwork data
  // Done states
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
  unsigned long collectTimer = 0;  // NEW: separate timer for data collection
  uint16_t dynamicPort = 0;
  uint32_t txId = 0;

  // Retry tracking
  uint8_t fetchRetryCount = 0;
  uint32_t pendingFetchTrackId = 0;

  // Phrase change tracking
  int previousPhraseIdx = -1;

  // Metadata parsing
  std::vector<uint8_t> metadataBuffer;
  uint32_t artworkId = 0;

  // Waveform buffer for collection - NEW
  std::vector<uint8_t> waveformBuffer;

  // Artwork buffer for collection - NEW
  std::vector<uint8_t> artworkBuffer;

  // Waveform buffer management
  size_t waveformAllocated = 0;

  // Artwork buffer management
  size_t artworkAllocated = 0;

  const uint8_t TYPE_BEAT = 0x28;
  const float SPEED_UNITY = 1048576.0f;

  // Settings
  bool enabled = true;
  bool enableDebug = false;
  bool enableBeatFlash = false;
  bool enableRandomPreset = false;

  // New settings: IP override and virtual deck number
  String playerIPOverride = "";
  uint8_t virtualDeckNumber = WLED_DEVICE_ID_DEFAULT;

  // Config strings
  static const char _name[];
  static const char _enabled[];
  static const char _debug[];
  static const char _beatFlash[];
  static const char _randomPreset[];
  static const char _ipOverride[];
  static const char _deckNumber[];

  // --- Constants for Protocol ---
  // Fixed magic prefix for all dbserver queries (NOT the handshake response!)
  const uint8_t DBSERVER_MAGIC[5] = { 0x11, 0x87, 0x23, 0x49, 0xae };
  const uint8_t PROLINK_MAGIC[10] = { 0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c };  // "Qspt1WmJOL"

  const uint8_t XOR_KEY[19] = {
    0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE, 0xE9, 0xD2,
    0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8, 0xE9, 0xF4, 0xE1
  };

  // Pioneer waveform color palette (RGB888)
  const uint32_t PIONEER_COLORS[8] = {
    0x0000FF,  // 0: Blue
    0x0080FF,  // 1: Blue-cyan
    0x00FFFF,  // 2: Cyan
    0x00FF00,  // 3: Green
    0x80FF00,  // 4: Green-yellow
    0xFFFF00,  // 5: Yellow
    0xFF8000,  // 6: Orange
    0xFF0000   // 7: Red
  };

  // --- Helpers ---

  uint8_t getVirtualDeckNumber() {
    return virtualDeckNumber;
  }

  IPAddress getTargetPlayerIP() {
    if (playerIPOverride.length() > 0) {
      IPAddress overrideIP;
      if (overrideIP.fromString(playerIPOverride)) {
        return overrideIP;
      }
    }
    return linkState.activePlayerIP;
  }

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

  // --- PSRAM Memory Management ---

  bool ensureWaveformBuffer(size_t neededSize) {
    if (prolink_waveform_data && waveformAllocated >= neededSize) {
      return true;
    }

    size_t allocSize = max(neededSize, (size_t)WAVEFORM_BUFFER_MAX);

    if (prolink_waveform_data) {
      WaveformPoint* newBuf = (WaveformPoint*)heap_caps_realloc(
        prolink_waveform_data,
        allocSize * sizeof(WaveformPoint),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
      if (newBuf) {
        prolink_waveform_data = newBuf;
        waveformAllocated = allocSize;
        return true;
      }
      return false;
    }

    prolink_waveform_data = (WaveformPoint*)heap_caps_malloc(
      allocSize * sizeof(WaveformPoint),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (prolink_waveform_data) {
      waveformAllocated = allocSize;
      return true;
    }

    // Fallback to regular heap if PSRAM not available
    prolink_waveform_data = (WaveformPoint*)malloc(allocSize * sizeof(WaveformPoint));
    if (prolink_waveform_data) {
      waveformAllocated = allocSize;
      if (enableDebug) Serial.println(F("[ProLink] Waveform buffer allocated from regular heap"));
      return true;
    }

    return false;
  }

  bool ensureArtworkBuffer(size_t neededSize) {
    if (prolink_artwork_data && artworkAllocated >= neededSize) {
      return true;
    }

    size_t allocSize = max(neededSize, (size_t)ARTWORK_BUFFER_MAX);

    if (prolink_artwork_data) {
      uint8_t* newBuf = (uint8_t*)heap_caps_realloc(
        prolink_artwork_data,
        allocSize,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
      if (newBuf) {
        prolink_artwork_data = newBuf;
        artworkAllocated = allocSize;
        return true;
      }
      return false;
    }

    prolink_artwork_data = (uint8_t*)heap_caps_malloc(
      allocSize,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (prolink_artwork_data) {
      artworkAllocated = allocSize;
      return true;
    }

    // Fallback to regular heap
    prolink_artwork_data = (uint8_t*)malloc(allocSize);
    if (prolink_artwork_data) {
      artworkAllocated = allocSize;
      if (enableDebug) Serial.println(F("[ProLink] Artwork buffer allocated from regular heap"));
      return true;
    }

    return false;
  }

  void freeBuffers() {
    if (prolink_waveform_data) {
      heap_caps_free(prolink_waveform_data);
      prolink_waveform_data = nullptr;
      waveformAllocated = 0;
      prolink_waveform_length = 0;
      prolink_waveform_valid = false;
    }
    if (prolink_artwork_data) {
      heap_caps_free(prolink_artwork_data);
      prolink_artwork_data = nullptr;
      artworkAllocated = 0;
      prolink_artwork_size = 0;
      prolink_artwork_valid = false;
    }
  }

  void clearMetadata() {
    prolink_track_title = "";
    prolink_track_artist = "";
    prolink_track_album = "";
    prolink_track_key = "";
    prolink_track_genre = "";
    prolink_track_label = "";
    prolink_metadata_valid = false;
    artworkId = 0;
  }

  void clearWaveform() {
    prolink_waveform_length = 0;
    prolink_waveform_valid = false;
  }

  void clearArtwork() {
    prolink_artwork_size = 0;
    prolink_artwork_valid = false;
  }

  // --- Network Functions ---

  void sendKeepAlive() {
    if (!Network.isConnected()) return;

    uint8_t deckNum = getVirtualDeckNumber();

    uint8_t pkg[64];
    memset(pkg, 0, 64);
    memcpy(pkg, PROLINK_MAGIC, 10);
    pkg[0x0A] = TYPE_ANNOUNCE;
    snprintf((char*)&pkg[0x0C], 20, "%s", WLED_DEVICE_NAME);
    pkg[0x20] = 0x01; pkg[0x21] = 0x02; pkg[0x23] = 0x36;
    pkg[0x24] = deckNum; pkg[0x25] = 0x01;
    Network.localMAC(&pkg[0x26]);
    uint32_t ip = (uint32_t)Network.localIP();
    memcpy(&pkg[0x2C], &ip, 4);
    pkg[0x30] = getActivePeerCount() + 1;
    pkg[0x34] = 0x01;

    IPAddress broadcast = Network.localIP();
    broadcast[3] = 255;
    udpAnnounce.writeTo(pkg, 54, broadcast, PORT_ANNOUNCE);

    IPAddress lastip = INADDR_NONE;
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
      uint8_t deckNum = getVirtualDeckNumber();
      if (devID > 0 && devID < 64 && devID != deckNum) {
        unsigned long now = millis();
        uint8_t peerIdx = devID - 1;
        bool wasKnown = (linkState.peerMap & ((uint64_t)1 << peerIdx)) != 0;
        bool wasTimedOut = wasKnown && (now - linkState.peerLastSeen[peerIdx] > PEER_TIMEOUT_MS);
        bool isNew = !wasKnown;

        linkState.peerMap |= ((uint64_t)1 << peerIdx);
        linkState.peerIPs[peerIdx] = packet.remoteIP();
        linkState.peerLastSeen[peerIdx] = now;
        linkState.isConnected = true;

        if (isNew) {
          if (enableDebug) Serial.printf("[ProLink] New peer: Player %d at %s\n", devID, packet.remoteIP().toString().c_str());
          sendKeepAlive();
        } else if (wasTimedOut) {
          if (enableDebug) Serial.printf("[ProLink] Peer returned: Player %d at %s\n", devID, packet.remoteIP().toString().c_str());
          if (devID == linkState.activePlayerID && linkState.currentTrackId > 0) {
            if (enableDebug) Serial.println(F("[ProLink] Active player returned, will re-fetch if needed"));
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

      if (packet.length() > 0x9C) {
        uint32_t rawEff = ((uint32_t)data[0x99] << 24) | ((uint32_t)data[0x9A] << 16) |
          ((uint32_t)data[0x9B] << 8) | data[0x9C];
        linkState.effectivePitch = rawPitchToPercent(rawEff);
      }

      if (packet.length() > 0x2F) {
        uint32_t tid = (data[0x2C] << 24) | (data[0x2D] << 16) | (data[0x2E] << 8) | data[0x2F];
        if (tid > 0 && tid != linkState.currentTrackId) {
          linkState.currentTrackId = tid;
          linkState.currentSlot = data[0x28];

          if (tid != lastAnalyzedTrackId) {
            fetchRetryCount = 0;
            pendingFetchTrackId = tid;
            cleanupTcpConnection();
            fetchState = IDLE;

            // Clear old data for new track
            clearMetadata();
            clearWaveform();
            clearArtwork();

            startMetadataFetch();
          }
        }
      }

      if (packet.length() > 0xA6) linkState.beatInMeasure = data[0xA6];
      if (packet.length() > 0xA3) {
        linkState.beatsElapsed = (data[0xA2] << 8) | data[0xA3];
      }
      if (packet.length() > 0x11D) {
        linkState.halfBarsRemaining = data[0x11D];
      }
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

    if (playerID > 0 && playerID < 64) {
      uint8_t peerIdx = playerID - 1;
      if (linkState.peerMap & ((uint64_t)1 << peerIdx)) {
        linkState.peerLastSeen[peerIdx] = millis();
      }
    }

    if (data[0x0A] == TYPE_BEAT) {
      uint16_t baseBpmRaw = (data[0x5A] << 8) | data[0x5B];
      uint32_t speedRaw = (data[0x54] << 24) | (data[0x55] << 16) | (data[0x56] << 8) | data[0x57];

      float baseBpm = baseBpmRaw / 100.0f;
      float speedRatio = speedRaw / SPEED_UNITY;
      float currentBpm = baseBpm * speedRatio;

      uint32_t msToNextBar = (data[0x2C] << 24) | (data[0x2D] << 16) | (data[0x2E] << 8) | data[0x2F];
      uint8_t beat = data[0x5C];

      linkState.bpm = currentBpm;
      linkState.beatNumber = beat;
      linkState.msToNextBar = msToNextBar;

      linkState.pitchPercent = (speedRatio - 1.0f) * 100.0f;
      prolink_pitchPercent = linkState.pitchPercent;
    }
  }

  // --- TCP Connection Management ---

  void cleanupTcpConnection() {
    if (dbClient.connected()) {
      dbClient.stop();
    }
    while (dbClient.available()) dbClient.read();
    txId = 0;  // Reset transaction counter
  }

  void startMetadataFetch() {
    IPAddress targetIP = getTargetPlayerIP();

    if (targetIP[0] == 0) {
      if (enableDebug) Serial.println(F("[ProLink] No target player IP, skipping fetch"));
      return;
    }

    if (fetchRetryCount >= FETCH_MAX_RETRIES) {
      if (enableDebug) Serial.printf("[ProLink] Max retries (%d) reached for track %d\n", FETCH_MAX_RETRIES, pendingFetchTrackId);
      fetchState = IDLE;
      return;
    }

    fetchState = CONNECT_STAGE1;
    stateTimer = millis();
    if (enableDebug) Serial.printf("[ProLink] Starting fetch (attempt %d/%d)...\n", fetchRetryCount + 1, FETCH_MAX_RETRIES);
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

  // Send a dbserver query using the fixed magic prefix
  void sendDBQuery(uint16_t type, uint32_t* args, uint8_t argCount) {
    if (!dbClient.connected()) return;
    txId++;

    // Write the fixed DBSERVER_MAGIC prefix
    dbClient.write(DBSERVER_MAGIC, 5);

    // Transaction ID and query type header
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

  // Collect all available TCP data into a buffer (non-blocking)
  void collectAvailableData(std::vector<uint8_t>& buffer, size_t maxSize = 65535, bool drainExcess = false) {
    // Read in chunks to avoid blocking too long and triggering watchdog
    size_t chunkSize = 0;
    const size_t CHUNK_LIMIT = 1024;  // Yield every 1KB

    while (dbClient.available() && buffer.size() < maxSize) {
      buffer.push_back(dbClient.read());
      chunkSize++;

      // Yield periodically to prevent watchdog timeout
      if (chunkSize >= CHUNK_LIMIT) {
        yield();
        chunkSize = 0;
      }
    }

    // Optionally drain remaining data from socket without storing
    if (drainExcess && buffer.size() >= maxSize) {
      size_t drained = 0;
      while (dbClient.available()) {
        dbClient.read();  // Discard
        drained++;
        if ((drained % CHUNK_LIMIT) == 0) {
          yield();
        }
      }
      if (enableDebug && drained > 0) {
        Serial.printf("[ProLink] Drained %d excess bytes\n", drained);
      }
    }
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

  // --- PSSI Parsing ---

  bool parsePSSI() {
    uint8_t head[28];
    if (!readBytesWithTimeout(head, 28)) return false;

    uint32_t headerLen = (head[0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3];
    uint16_t numEntries = (head[12] << 8) | head[13];

    if (headerLen > 10000 || numEntries > 500) {
      if (enableDebug) Serial.printf("[ProLink] Invalid PSSI header: len=%d entries=%d\n", headerLen, numEntries);
      return false;
    }

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

    int readSoFar = 4 + 28;
    while (readSoFar < (int)headerLen) {
      if (!dbClient.available()) {
        delay(1);
        if (millis() - stateTimer > FETCH_TIMEOUT_MS) return false;
      }
      dbClient.read();
      readSoFar++;
    }

    for (int i = 0; i < numEntries; i++) {
      uint8_t e[24];
      if (!readBytesWithTimeout(e, 24)) return false;

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

    // Fix durations and calculate total beats
    prolink_total_beats = 0;
    for (size_t i = 0; i < phrases.size(); i++) {
      if (i < phrases.size() - 1) {
        phrases[i].count = phrases[i + 1].startBeat - phrases[i].startBeat;
      } else {
        phrases[i].count = 32;
      }
      uint32_t phraseEnd = phrases[i].startBeat + phrases[i].count;
      if (phraseEnd > prolink_total_beats) {
        prolink_total_beats = phraseEnd;
      }
    }
    prolink_total_phrases_public = phrases.size();

    if (prolink_total_phrases_public > 0) {
      auto pool = buildPresetPool();
      if (!pool.empty()) {
        prolink_presetOffset = random(pool.size());
        if (enableDebug) USER_PRINTF("[ProLink] Initialized preset offset: %d\n", prolink_presetOffset);
      }
    }

    return true;
  }

  // --- Metadata Parsing ---

  void requestMetadataSetup() {
    uint8_t deckNum = getVirtualDeckNumber();
    // Compound format: device << 24 | slot << 16 | 0x0301
    uint32_t compound = (deckNum << 24) | (linkState.currentSlot << 16) | 0x0301;
    uint32_t args[] = { compound, linkState.currentTrackId };
    sendDBQuery(0x2002, args, 2);
  }

  void requestMetadataRender() {
    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = (deckNum << 24) | (linkState.currentSlot << 16) | 0x0301;
    uint32_t args[] = { compound, 0, 16, 0, 16, 0 };
    sendDBQuery(0x3000, args, 6);
  }

  bool parseMetadataResponse() {
    if (metadataBuffer.size() < 32) return false;

    std::vector<String> strings;
    size_t pos = 0;

    while (pos < metadataBuffer.size() - 6) {
      // Look for UTF-16 string marker: 0x26 0x00 0x00 0x00
      if (metadataBuffer[pos] == 0x26 &&
        metadataBuffer[pos + 1] == 0x00 &&
        metadataBuffer[pos + 2] == 0x00 &&
        metadataBuffer[pos + 3] == 0x00) {

        uint16_t slen = metadataBuffer[pos + 4] | (metadataBuffer[pos + 5] << 8);

        if (slen > 0 && slen < 500 && (pos + 6 + slen * 2) <= metadataBuffer.size()) {
          String decoded = "";
          for (uint16_t i = 0; i < slen; i++) {
            size_t bytePos = pos + 6 + (i * 2);
            uint16_t ch = metadataBuffer[bytePos] | (metadataBuffer[bytePos + 1] << 8);

            if (ch == 0 || ch == 0x11 || ch == 0x1100) continue;

            if (ch < 128) {
              decoded += (char)ch;
            } else if (ch < 0x800) {
              decoded += (char)(0xC0 | (ch >> 6));
              decoded += (char)(0x80 | (ch & 0x3F));
            } else {
              decoded += (char)(0xE0 | (ch >> 12));
              decoded += (char)(0x80 | ((ch >> 6) & 0x3F));
              decoded += (char)(0x80 | (ch & 0x3F));
            }
          }

          decoded.trim();
          if (decoded.length() > 0) {
            strings.push_back(decoded);
          }
        }
        pos++;
      } else {
        pos++;
      }
    }

    // Extract artwork ID from response
    artworkId = 0;
    for (size_t i = 0; i < metadataBuffer.size() - 8; i++) {
      if (i > 20 && metadataBuffer[i] == 0x11) {
        uint32_t potentialId = (metadataBuffer[i + 1] << 24) |
          (metadataBuffer[i + 2] << 16) |
          (metadataBuffer[i + 3] << 8) |
          metadataBuffer[i + 4];
        if (potentialId > 0 && potentialId < 0x7FFFFFFF && artworkId == 0) {
          artworkId = potentialId;
          break;
        }
      }
    }

    // Map strings to fields
    if (strings.size() > 0) prolink_track_title = strings[0];
    if (strings.size() > 1) prolink_track_artist = strings[1];
    if (strings.size() > 2) prolink_track_album = strings[2];
    if (strings.size() > 3) prolink_track_key = strings[3];
    if (strings.size() > 4) prolink_track_genre = strings[4];
    if (strings.size() > 6) prolink_track_label = strings[6];

    prolink_metadata_valid = (strings.size() > 0);

    if (enableDebug && prolink_metadata_valid) {
      Serial.printf("[ProLink] Track: %s - %s (artwork ID: %u)\n",
        prolink_track_artist.c_str(),
        prolink_track_title.c_str(),
        artworkId);
    }

    return prolink_metadata_valid;
  }

  // --- Waveform Request & Parsing ---

  void requestPreviewWaveform() {
    // Request type 0x2C04 = Color waveform preview
    // Args: compound, trackId, tag1, tag2
    // tag1 determines what data format is returned:
    //   0x34565750 = "PWV4" reversed -> PREVIEW waveform (6 bytes × 1200 samples = ~7KB)
    //   0x35565750 = "PWV5" reversed -> DETAIL waveform (2 bytes × ~40K samples = ~80KB) TOO BIG!
    //   0x49535350 = "PSSI" reversed -> phrase structure data
    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = (deckNum << 24) | (linkState.currentSlot << 16) | 0x0301;
    uint32_t tag1 = 0x34565750;  // "4VWP" bytes = "PWV4" as string (reversed) - PREVIEW waveform (~7KB)
    uint32_t tag2 = 0x00545845;  // "\0TXE" bytes = "EXT\0" as string (reversed)  
    uint32_t args[] = { compound, linkState.currentTrackId, tag1, tag2 };

    if (enableDebug) {
      Serial.printf("[ProLink] Requesting waveform 0x2C04: compound=0x%08X, trackId=%u, tag1=0x%08X (PWV4 preview), tag2=0x%08X\n",
        compound, linkState.currentTrackId, tag1, tag2);
    }

    sendDBQuery(0x2C04, args, 4);
  }

  bool parseWaveformData() {
    if (waveformBuffer.size() < 50) {
      if (enableDebug) Serial.printf("[ProLink] Waveform data too short: %d bytes\n", waveformBuffer.size());
      return false;
    }

    if (enableDebug) {
      Serial.printf("[ProLink] Parsing waveform from %d bytes\n", waveformBuffer.size());
      Serial.print("[ProLink] First 80 bytes: ");
      for (size_t i = 0; i < min((size_t)80, waveformBuffer.size()); i++) {
        Serial.printf("%02X ", waveformBuffer[i]);
      }
      Serial.println();
    }

    // PWV4 response structure (PREVIEW waveform - ~7KB):
    // - DB message header (magic, txid, type, etc)
    // - Blob header at offset ~40: 14 xx xx xx xx (size)
    // - Blob data: xx xx xx xx (LE size) followed by "PWV4" tag (0x50 0x57 0x56 0x34)
    // - PWV4 header: 24 bytes total
    //   - 4 bytes: "PWV4" tag
    //   - 4 bytes: header size (0x00000018 = 24)
    //   - 4 bytes: data size
    //   - 4 bytes: entry size (0x00000006 = 6 bytes per sample)
    //   - 4 bytes: sample count (0x000004B0 = 1200)
    //   - 4 bytes: unknown
    // - Waveform data: 1200 samples × 6 bytes = 7200 bytes
    //   Channel layout per sample:
    //   - ch0: unknown
    //   - ch1: luminance boost
    //   - ch2: intensity for blue waveform (inverted)
    //   - ch3: red component (height & color)
    //   - ch4: green component  
    //   - ch5: blue component / front height

    // Find "PWV4" tag (0x50 0x57 0x56 0x34)
    size_t pwv4Pos = 0;
    for (size_t i = 40; i < min((size_t)120, waveformBuffer.size() - 4); i++) {
      if (waveformBuffer[i] == 0x50 && waveformBuffer[i + 1] == 0x57 &&
        waveformBuffer[i + 2] == 0x56 && waveformBuffer[i + 3] == 0x34) {
        pwv4Pos = i;
        if (enableDebug) Serial.printf("[ProLink] Found PWV4 tag at offset %d\n", pwv4Pos);
        break;
      }
    }

    if (pwv4Pos == 0) {
      if (enableDebug) Serial.println(F("[ProLink] PWV4 tag not found in response"));
      return false;
    }

    // PWV4 header is 24 bytes total, waveform data starts at PWV4 + 24
    size_t waveStart = pwv4Pos + 24;

    if (waveStart >= waveformBuffer.size() - 10) {
      if (enableDebug) Serial.println(F("[ProLink] Not enough data after PWV4 header"));
      return false;
    }

    if (enableDebug) {
      Serial.printf("[ProLink] Waveform data starts at offset %d\n", waveStart);
      Serial.print("[ProLink] Data preview: ");
      for (size_t i = waveStart; i < min(waveStart + 36, waveformBuffer.size()); i++) {
        Serial.printf("%02X ", waveformBuffer[i]);
      }
      Serial.println();
    }

    // PWV4 has 6 bytes per sample, 1200 samples typical
    // We extract height and color from channels 3, 4, 5 (RGB)
    size_t bytesAvailable = waveformBuffer.size() - waveStart;
    size_t numSamples = bytesAvailable / 6;
    if (numSamples > WAVEFORM_BUFFER_MAX) numSamples = WAVEFORM_BUFFER_MAX;
    if (numSamples > 1200) numSamples = 1200;  // PWV4 is 1200 samples max
    if (numSamples < 10) {
      if (enableDebug) Serial.printf("[ProLink] Too few waveform samples: %d\n", numSamples);
      return false;
    }

    if (!ensureWaveformBuffer(numSamples)) {
      if (enableDebug) Serial.println(F("[ProLink] Failed to allocate waveform buffer"));
      return false;
    }

    uint16_t validPoints = 0;
    for (size_t i = 0; i < numSamples && (waveStart + i * 6 + 5) < waveformBuffer.size(); i++) {
      size_t offset = waveStart + i * 6;
      // uint8_t ch0 = waveformBuffer[offset + 0];  // Unknown
      // uint8_t ch1 = waveformBuffer[offset + 1];  // Luminance
      // uint8_t ch2 = waveformBuffer[offset + 2];  // Blue intensity (inverted)
      uint8_t ch3 = waveformBuffer[offset + 3] & 0x7F;  // Red (strip high bit)
      uint8_t ch4 = waveformBuffer[offset + 4] & 0x7F;  // Green
      uint8_t ch5 = waveformBuffer[offset + 5] & 0x7F;  // Blue / front height

      // Calculate overall height as max of RGB channels
      uint8_t height = max(max(ch3, ch4), ch5);

      // Map RGB to Pioneer color index (0-7)
      // Simple mapping based on dominant channel
      uint8_t colorIdx = 0;
      if (height > 0) {
        // Calculate normalized RGB
        uint8_t r = (ch3 * 7) / 127;
        uint8_t g = (ch4 * 7) / 127;
        uint8_t b = (ch5 * 7) / 127;

        // Map to Pioneer color scheme:
        // 0=Red, 1=Orange, 2=Yellow, 3=Green, 4=Cyan, 5=Blue, 6=Purple, 7=Pink
        if (r > g && r > b) {
          colorIdx = (g > b) ? 1 : 0;  // Orange or Red
        } else if (g > r && g > b) {
          colorIdx = (b > r) ? 4 : 3;  // Cyan or Green
        } else if (b > r && b > g) {
          colorIdx = (r > g) ? 6 : 5;  // Purple or Blue
        } else if (r == g && r > b) {
          colorIdx = 2;  // Yellow
        } else {
          colorIdx = 7;  // Pink/White
        }
      }

      // Height is 0-127 (full resolution from PWV4)
      prolink_waveform_data[validPoints].height = height;
      prolink_waveform_data[validPoints].color = colorIdx;
      validPoints++;
    }

    prolink_waveform_length = validPoints;
    prolink_waveform_valid = (validPoints > 10);

    if (enableDebug) {
      Serial.printf("[ProLink] Parsed %d PWV4 waveform points\n", validPoints);
    }

    return prolink_waveform_valid;
  }

  // --- Artwork Request & Parsing ---

  void requestArtwork() {
    // Request type 0x2003 = Album artwork
    // Args: compound, trackId, 1
    // Note: This request returns JPEG data
    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = (deckNum << 24) | (linkState.currentSlot << 16) | 0x0301;
    uint32_t args[] = { compound, linkState.currentTrackId, 1 };

    if (enableDebug) {
      Serial.printf("[ProLink] Requesting artwork 0x2003: compound=0x%08X, trackId=%u, arg3=1\n",
        compound, linkState.currentTrackId);
    }

    sendDBQuery(0x2003, args, 3);
  }

  bool parseArtworkData() {
    if (artworkBuffer.size() < 100) {
      if (enableDebug) Serial.printf("[ProLink] Artwork data too short: %d bytes\n", artworkBuffer.size());
      return false;
    }

    // Find JPEG start marker (0xFF 0xD8)
    int jpegStart = -1;
    for (size_t i = 0; i < artworkBuffer.size() - 1; i++) {
      if (artworkBuffer[i] == 0xFF && artworkBuffer[i + 1] == 0xD8) {
        jpegStart = i;
        break;
      }
    }

    if (jpegStart < 0) {
      if (enableDebug) Serial.println(F("[ProLink] No JPEG marker found"));
      return false;
    }

    // Find JPEG end marker (0xFF 0xD9)
    int jpegEnd = -1;
    for (size_t i = artworkBuffer.size() - 1; i > (size_t)jpegStart; i--) {
      if (artworkBuffer[i] == 0xD9 && artworkBuffer[i - 1] == 0xFF) {
        jpegEnd = i + 1;
        break;
      }
    }

    if (jpegEnd < 0) {
      jpegEnd = artworkBuffer.size();
    }

    size_t jpegSize = jpegEnd - jpegStart;

    if (!ensureArtworkBuffer(jpegSize)) {
      if (enableDebug) Serial.println(F("[ProLink] Failed to allocate artwork buffer"));
      return false;
    }

    memcpy(prolink_artwork_data, &artworkBuffer[jpegStart], jpegSize);
    prolink_artwork_size = jpegSize;
    prolink_artwork_valid = true;

    if (enableDebug) {
      Serial.printf("[ProLink] Artwork parsed: %d bytes JPEG\n", jpegSize);
    }

    return true;
  }

  // --- Main State Machine ---

  void handleFetchStateMachine() {
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

    // Check for overall timeout (except during collection phases)
    if (fetchState != COLLECT_METADATA &&
      fetchState != COLLECT_WAVEFORM &&
      fetchState != COLLECT_ARTWORK) {
      if (millis() - stateTimer > FETCH_TIMEOUT_MS) {
        if (enableDebug) Serial.println(F("[ProLink] Fetch Timeout"));
        cleanupTcpConnection();
        scheduleFetchRetry();
        return;
      }
    }

    IPAddress targetIP = getTargetPlayerIP();

    switch (fetchState) {
    case CONNECT_STAGE1: {
      if (targetIP[0] == 0) {
        if (enableDebug) Serial.println(F("[ProLink] Player IP invalid, aborting"));
        fetchState = IDLE;
        return;
      }

      if (dbClient.connect(targetIP, PORT_DBSERVER)) {
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
      if (dbClient.connect(targetIP, dynamicPort)) {
        // Send initial handshake
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
        // Drain the handshake response
        while (dbClient.available()) dbClient.read();

        uint8_t deckNum = getVirtualDeckNumber();

        // Send setup request using the fixed DBSERVER_MAGIC
        // Transaction ID -2 (0xFFFFFFFE) for setup
        uint8_t pkt[60];
        memset(pkt, 0, 60);

        // Magic prefix
        memcpy(pkt, DBSERVER_MAGIC, 5);

        // Transaction ID = -2 (0xFFFFFFFE), type = 0x0000, 1 arg
        uint8_t mid[] = { 0x11, 0xFF,0xFF,0xFF,0xFE, 0x10, 0,0, 0x0F, 0x01, 0x14, 0,0,0,0x0C };
        memcpy(&pkt[5], mid, 15);
        pkt[20] = 0x06;  // 1 argument of type 0x06 (number)
        // Pad to 12 arg slots
        pkt[32] = 0x11;  // Number type marker
        uint32_t pid = deckNum;
        pkt[33] = (pid >> 24); pkt[34] = (pid >> 16); pkt[35] = (pid >> 8); pkt[36] = pid;

        dbClient.write(pkt, 37);
        fetchState = WAIT_HANDSHAKE;
        stateTimer = millis();
      }
      break;

    case WAIT_HANDSHAKE:
      if (dbClient.available() > 10) {
        while (dbClient.available()) dbClient.read();
        // Go to PSSI first (highest priority)
        fetchState = REQUEST_PHRASES;
        stateTimer = millis();
      }
      break;

      // --- PSSI (Phrases) ---
    case REQUEST_PHRASES: {
      uint8_t deckNum = getVirtualDeckNumber();
      uint32_t compound = (deckNum << 24) | (linkState.currentSlot << 16) | 0x0301;
      uint32_t pssi = 0x49535350; // "ISSP" reversed
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
        int searchLimit = 1000;
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
            lastAnalyzedTrackId = linkState.currentTrackId;
            fetchRetryCount = 0;
            if (enableDebug) Serial.println(F("[ProLink] PSSI Parsed Success"));
          } else {
            if (enableDebug) Serial.println(F("[ProLink] PSSI Parse Failed"));
          }
          // Continue to metadata regardless
          fetchState = REQUEST_METADATA_SETUP;
          stateTimer = millis();
        }
      }
      break;

      // --- Metadata ---
    case REQUEST_METADATA_SETUP:
      metadataBuffer.clear();
      requestMetadataSetup();
      fetchState = WAIT_METADATA_SETUP;
      stateTimer = millis();
      break;

    case WAIT_METADATA_SETUP:
      if (dbClient.available() > 10) {
        while (dbClient.available()) dbClient.read();
        fetchState = REQUEST_METADATA_RENDER;
        stateTimer = millis();
      }
      break;

    case REQUEST_METADATA_RENDER:
      requestMetadataRender();
      fetchState = WAIT_METADATA_RENDER;
      stateTimer = millis();
      break;

    case WAIT_METADATA_RENDER:
      if (dbClient.available() > 10) {
        // Start collecting metadata
        collectTimer = millis();
        fetchState = COLLECT_METADATA;
      }
      break;

    case COLLECT_METADATA:
      // Collect data for a period of time
      collectAvailableData(metadataBuffer, METADATA_BUFFER_MAX);

      if (millis() - collectTimer > METADATA_COLLECT_MS ||
        (!dbClient.available() && metadataBuffer.size() > 100)) {
        // Done collecting, parse it
        if (parseMetadataResponse()) {
          if (enableDebug) Serial.printf("[ProLink] Metadata parsed (%d bytes)\n", metadataBuffer.size());
        }
        // Continue to waveform
        fetchState = REQUEST_WAVEFORM;
        stateTimer = millis();
      }
      break;

      // --- Waveform ---
    case REQUEST_WAVEFORM:
      waveformBuffer.clear();
      requestPreviewWaveform();
      fetchState = WAIT_WAVEFORM;
      stateTimer = millis();
      break;

    case WAIT_WAVEFORM:
      // Check if connection is still valid
      if (!dbClient.connected()) {
        if (enableDebug) Serial.println(F("[ProLink] Connection lost waiting for waveform"));
        cleanupTcpConnection();
        fetchState = COOLDOWN;
        stateTimer = millis();
        break;
      }
      if (dbClient.available() > 10) {
        // Start collecting waveform data
        collectTimer = millis();
        fetchState = COLLECT_WAVEFORM;
        if (enableDebug) Serial.println(F("[ProLink] Collecting waveform data..."));
      }
      break;

    case COLLECT_WAVEFORM:
      // Collect data - PWV4 preview responses are ~7KB (1200 samples × 6 bytes)
      // Much smaller than PWV5 detail waveform (~80KB)
      collectAvailableData(waveformBuffer, 8192);  // 8KB max for PWV4 preview
      yield();  // Prevent watchdog

      if (millis() - collectTimer > WAVEFORM_COLLECT_MS) {
        // Done collecting, parse it
        if (enableDebug) Serial.printf("[ProLink] Collected %d bytes for waveform\n", waveformBuffer.size());

        if (parseWaveformData()) {
          if (enableDebug) Serial.println(F("[ProLink] Waveform parsed successfully"));
        }

        // Continue to artwork
        fetchState = REQUEST_ARTWORK;
        stateTimer = millis();
      }
      break;

      // --- Artwork ---
    case REQUEST_ARTWORK:
      artworkBuffer.clear();
      requestArtwork();
      fetchState = WAIT_ARTWORK;
      stateTimer = millis();
      break;

    case WAIT_ARTWORK:
      // Check if connection is still valid
      if (!dbClient.connected()) {
        if (enableDebug) Serial.println(F("[ProLink] Connection lost waiting for artwork"));
        cleanupTcpConnection();
        fetchState = COOLDOWN;
        stateTimer = millis();
        break;
      }
      if (dbClient.available() > 10) {
        collectTimer = millis();
        fetchState = COLLECT_ARTWORK;
        if (enableDebug) Serial.println(F("[ProLink] Collecting artwork data..."));
      }
      break;

    case COLLECT_ARTWORK:
      collectAvailableData(artworkBuffer, ARTWORK_BUFFER_MAX, true);  // Drain excess
      yield();  // Prevent watchdog during large transfers

      if (millis() - collectTimer > ARTWORK_COLLECT_MS) {
        if (enableDebug) Serial.printf("[ProLink] Collected %d bytes for artwork\n", artworkBuffer.size());

        if (parseArtworkData()) {
          if (enableDebug) Serial.println(F("[ProLink] Artwork parsed successfully"));
        }

        cleanupTcpConnection();
        fetchState = COOLDOWN;
        stateTimer = millis();
      }
      break;

    default: break;
    }
  }

  void printPhrases() {
    Serial.println("=== Parsed PSSI Phrases ===");
    for (size_t i = 0; i < phrases.size(); i++) {
      PhraseEntry& pe = phrases[i];
      USER_PRINTF("Phrase%3d : Start:%5d  Count:%4d  %s %s\n", i, pe.startBeat, pe.count, prolink_mood_public.c_str(), pe.label.c_str());
    }
  }

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

  void updatePhraseState() {
    if (phrases.empty()) {
      prolink_phrase_name_public = "";
      prolink_phrase_index_public = -1;
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
      prolink_track_progress = 0.0f;
      return;
    }

    uint32_t currentBeat = linkState.beatsElapsed;
    if (currentBeat < 1) currentBeat = 1;

    if (prolink_total_beats > 0) {
      prolink_track_progress = (float)currentBeat / (float)prolink_total_beats;
      if (prolink_track_progress > 1.0f) prolink_track_progress = 1.0f;
    }

    int activeIdx = -1;
    for (size_t i = 0; i < phrases.size(); i++) {
      if (currentBeat >= phrases[i].startBeat && currentBeat < (phrases[i].startBeat + phrases[i].count)) {
        activeIdx = i;
        break;
      }
    }

    if (enableRandomPreset && activeIdx != previousPhraseIdx && previousPhraseIdx != -1 && activeIdx != -1) {
      auto pool = buildPresetPool();
      int newPreset = getPresetForPhraseNoRepeat(activeIdx, pool);

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

  void checkPeerTimeouts() {
    unsigned long now = millis();

    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) {
        if (linkState.peerLastSeen[i] > 0 && (now - linkState.peerLastSeen[i]) > PEER_TIMEOUT_MS) {
          uint8_t devID = i + 1;

          if (enableDebug) Serial.printf("[ProLink] Peer timeout: Player %d\n", devID);

          if (devID == linkState.activePlayerID) {
            if (enableDebug) Serial.println(F("[ProLink] Active player went away"));

            linkState.isMaster = false;
            linkState.activePlayerID = 0;
            linkState.bpm = 0;
            linkState.beatNumber = 0;
            linkState.beatsElapsed = 0;
            linkState.currentTrackId = 0;

            phrases.clear();
            lastAnalyzedTrackId = 0;
            prolink_phrase_name_public = "";
            prolink_phrase_index_public = -1;
            prolink_mood_public = "";
            previousPhraseIdx = -1;
            prolink_total_beats = 0;

            clearMetadata();
            clearWaveform();
            clearArtwork();

            cleanupTcpConnection();
            fetchState = IDLE;
            fetchRetryCount = 0;

            prolink_connected_public = false;
          }
        }
      }
    }

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

    sendKeepAlive();

    if (enableDebug) {
      Serial.println(F("[ProLink] Initialized and listening"));
      Serial.printf("[ProLink] Virtual deck number: %d\n", virtualDeckNumber);
      if (playerIPOverride.length() > 0) {
        Serial.printf("[ProLink] IP Override: %s\n", playerIPOverride.c_str());
      }
    }
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

    static unsigned long lastPeerCheck = 0;
    if (millis() - lastPeerCheck > PEER_CHECK_INTERVAL_MS) {
      checkPeerTimeouts();
      lastPeerCheck = millis();
    }

    handleFetchStateMachine();

    static unsigned long lastBeatTime = 0;
    static uint32_t lastBeatNumber = 0;
    static unsigned long beatFlashStart = 0;

    if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) {
      lastBeatTime = millis();
      lastBeatNumber = linkState.beatNumber;

      if (enableBeatFlash) {
        beatFlashStart = millis();
        prolink_beat_flash_active = true;
        prolink_beat_flash_brightness = 255;
      }
    }

    if (enableBeatFlash && prolink_beat_flash_active) {
      unsigned long flashElapsed = millis() - beatFlashStart;
      if (flashElapsed < BEAT_FLASH_DURATION_MS) {
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

    updatePhraseState();

    prolink_bpm_public = getEffectiveBPM();
    prolink_beat_public = linkState.beatInMeasure;
    prolink_beat_number_public = linkState.beatNumber;
    prolink_track_id_public = linkState.currentTrackId;
    prolink_beats_elapsed_public = linkState.beatsElapsed;
    prolink_bars_elapsed_public = linkState.halfBarsElapsed / 2;
    prolink_bars_remaining_public = linkState.halfBarsRemaining / 2;
    prolink_connected_public = linkState.isMaster;

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
    top[FPSTR(_ipOverride)] = playerIPOverride;
    top[FPSTR(_deckNumber)] = virtualDeckNumber;
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

    const char* ipStr = top[FPSTR(_ipOverride)] | "";
    playerIPOverride = String(ipStr);

    virtualDeckNumber = top[FPSTR(_deckNumber)] | WLED_DEVICE_ID_DEFAULT;
    if (virtualDeckNumber < 1) virtualDeckNumber = 1;
    if (virtualDeckNumber > 127) virtualDeckNumber = 127;

    if (enableDebug) {
      Serial.printf("[ProLink] Config: enabled=%d debug=%d beatFlash=%d randomPreset=%d deck=%d ip=%s\n",
        enabled, enableDebug, enableBeatFlash, enableRandomPreset,
        virtualDeckNumber, playerIPOverride.c_str());
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

      // if (prolink_metadata_valid && linkState.isMaster) {
      //   String trackInfo = "<br />" + prolink_track_artist;
      //   if (trackInfo.length() > 5) trackInfo += " - ";
      //   trackInfo += prolink_track_title;
      //   if (trackInfo.length() > 50) trackInfo = trackInfo.substring(0, 47) + "...";
      //   infoArr.add(trackInfo);
      // }

      if (prolink_phrase_name_public.length() > 0 && linkState.isMaster) {
        infoArr.add("<br />" + prolink_mood_public + " " + prolink_phrase_name_public);
      }

      // String dataStatus = "<br />";
      // dataStatus += prolink_waveform_valid ? "W" : "-";
      // dataStatus += prolink_artwork_valid ? "A" : "-";
      // dataStatus += prolink_metadata_valid ? "M" : "-";
      // if (fetchState != IDLE && fetchState != COOLDOWN) {
      //   dataStatus += " (fetching";
      //   if (fetchRetryCount > 0) {
      //     dataStatus += " r" + String(fetchRetryCount);
      //   }
      //   dataStatus += ")";
      // }
      // infoArr.add(dataStatus);
    }
  }

  uint16_t getId() { return 0xCD30; }

  static uint32_t getPioneerColorRGB(uint8_t colorIndex) {
    static const uint32_t colors[8] = {
      0x0000FF, 0x0080FF, 0x00FFFF, 0x00FF00,
      0x80FF00, 0xFFFF00, 0xFF8000, 0xFF0000
    };
    return colors[colorIndex & 0x07];
  }

  static uint16_t getPioneerColorRGB565(uint8_t colorIndex) {
    uint32_t rgb = getPioneerColorRGB(colorIndex);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
};

// --- Static Definitions ---
const char ProLinkUsermod::_name[] PROGMEM = "Pro DJ Link";
const char ProLinkUsermod::_enabled[] PROGMEM = "Enabled";
const char ProLinkUsermod::_debug[] PROGMEM = "Enable Debug";
const char ProLinkUsermod::_beatFlash[] PROGMEM = "Beat Flash";
const char ProLinkUsermod::_randomPreset[] PROGMEM = "Random Preset on Phrase";
const char ProLinkUsermod::_ipOverride[] PROGMEM = "Player IP Override";
const char ProLinkUsermod::_deckNumber[] PROGMEM = "Virtual Deck Number";