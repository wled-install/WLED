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

// --- Public Variables (Exposed to FX.cpp) ---

// Timing & Sync
volatile float prolink_bpm_public = 0.0;
volatile uint8_t prolink_beat_public = 0;
volatile uint32_t prolink_beat_number_public = 0;
volatile float prolink_beat_progress_public = 0.0;
volatile uint32_t prolink_track_id_public = 0;

// Bar/Beat Counters
volatile uint16_t prolink_beats_elapsed_public = 0;
volatile uint8_t prolink_bars_elapsed_public = 0;
volatile uint8_t prolink_bars_remaining_public = 0;

// Phrase / Structure Public Vars
volatile int prolink_phrase_index_public = -1;       // Current phrase index
String prolink_phrase_name_public = "";              // e.g. "Intro", "Chorus Fill"
volatile uint16_t prolink_phrase_beats_public = 0;   // Length of current phrase in beats
volatile float prolink_phrase_progress_public = 0.0; // 0.0-1.0 progress through phrase
String prolink_mood_public = "";                     // "High", "Mid", "Low"
volatile uint16_t prolink_total_phrases_public = 0;

// --- Internal Data Structures ---

struct PhraseEntry {
  uint16_t index;
  uint32_t startBeat; // Absolute cumulative beat
  uint32_t count;     // Duration in beats
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
  volatile uint32_t beatNumber = 0;      // Current absolute beat from CDJ
  volatile uint8_t  beatInMeasure = 0;

  // Counters
  volatile uint16_t beatsElapsed = 0;
  volatile uint8_t halfBarsRemaining = 0;
  volatile uint8_t halfBarsElapsed = 0;

  // Track ID
  volatile uint32_t currentTrackId = 0;
  volatile uint8_t  currentSlot = 0;
  IPAddress         activePlayerIP;

  // Peers
  volatile uint64_t peerMap = 0;
  IPAddress peerIPs[64];
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
  COOLDOWN
};

class ProLinkUsermod : public Usermod {

private:
  AsyncUDP udpStatus;
  AsyncUDP udpBeat;
  AsyncUDP udpAnnounce;
  WiFiClient dbClient; // TCP Client for DB Server

  ProLinkState linkState;

  // Phrase Storage
  std::vector<PhraseEntry> phrases;
  uint32_t lastAnalyzedTrackId = 0;

  // State Machine
  FetchState fetchState = IDLE;
  unsigned long stateTimer = 0;
  uint16_t dynamicPort = 0;
  uint32_t txId = 0;

  // Settings
  bool enabled = true;
  bool enableDebug = false;
  bool enableBeatFlash = false; // Optional visual debug

  // Config strings
  static const char _name[];
  static const char _enabled[];
  static const char _beatDebug[];
  static const char _pitchDebug[];

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

  void sendKeepAlive() {
    if (!Network.isConnected()) return;
    uint8_t pkg[64]; memset(pkg, 0, 64);
    memcpy(pkg, PROLINK_MAGIC, 10);
    pkg[0x0A] = TYPE_ANNOUNCE;
    snprintf((char*)&pkg[0x0C], 20, "%s", WLED_DEVICE_NAME);
    pkg[0x20] = 0x01; pkg[0x21] = 0x02; pkg[0x23] = 0x36;
    pkg[0x24] = WLED_DEVICE_ID; pkg[0x25] = 0x01;
    Network.localMAC(&pkg[0x26]);
    uint32_t ip = (uint32_t)Network.localIP();
    memcpy(&pkg[0x2C], &ip, 4);
    pkg[0x30] = 1; pkg[0x34] = 0x01;

    for (int i = 0; i < 64; i++) {
      if (linkState.peerMap & ((uint64_t)1 << i)) {
        if (linkState.peerIPs[i][0] != 0) udpAnnounce.writeTo(pkg, 54, linkState.peerIPs[i], PORT_ANNOUNCE);
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
    if (packet.length() < 0x11F || data[0x0A] != TYPE_STATUS) return;

    uint8_t playerID = data[0x21];
    bool isMaster = (data[0x89] & 0x20) != 0;

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
            fetchState = IDLE;
            startMetadataFetch();
          }
        }
      }

      // Beat in Measure
      if (packet.length() > 0xA6) linkState.beatInMeasure = data[0xA6];

      // Beats Elapsed (2 bytes at 0xA2-0xA3)
      if (packet.length() > 0xA3) {
        linkState.beatsElapsed = (data[0xA2] << 8) | data[0xA3];
      }

      // Half-bars Remaining (Byte 0x11D)
      if (packet.length() > 0x11D) {
        linkState.halfBarsRemaining = data[0x11D];
      }

      // Half-bars Elapsed (Byte 0x11E)
      if (packet.length() > 0x11E) {
        linkState.halfBarsElapsed = data[0x11E];
      }
    }
  }

  void parseBeatPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 60) return;
    if (data[0x21] != linkState.activePlayerID) return;

    if (data[0x0A] == TYPE_BEAT_GRID) {
      // Raw BPM (Value is typically BPM * 10, not * 100, for this packet offset)
      uint32_t rawBpm = (data[0x38] << 24) | (data[0x39] << 16) | (data[0x3A] << 8) | data[0x3B];
      if (rawBpm != 0xFFFFFFFF) linkState.bpm = rawBpm / 10.0f; // <-- FIXED DIVISION FACTOR

      // Absolute Beat Number
      linkState.beatNumber = (data[0x24] << 24) | (data[0x25] << 16) | (data[0x26] << 8) | data[0x27];
    }
  }

  // --- TCP Fetch Logic ---

  void startMetadataFetch() {
    if (linkState.activePlayerIP[0] == 0) return;
    fetchState = CONNECT_STAGE1;
    stateTimer = millis();
    if (enableDebug) Serial.println(F("[ProLink] Starting Phrase Fetch..."));
  }

  void sendDBQuery(uint16_t type, uint32_t* args, uint8_t argCount) {
    if (!dbClient.connected()) return;
    txId++;

    // Header
    dbClient.write(DBSERVER_MAGIC, 5);
    uint8_t head[] = { 0x11, 0,0,0,0, 0x10, 0,0, 0x0F, argCount };

    // Inject TX ID
    head[1] = (txId >> 24) & 0xFF; head[2] = (txId >> 16) & 0xFF;
    head[3] = (txId >> 8) & 0xFF;  head[4] = txId & 0xFF;

    // Inject Type
    head[6] = (type >> 8) & 0xFF;  head[7] = type & 0xFF;

    dbClient.write(head, 10);

    // Arg Types Header
    uint8_t argHead[] = { 0x14, 0,0,0,0x0C };
    dbClient.write(argHead, 5);

    // Arg Types List
    for (int i = 0; i < argCount; i++) dbClient.write((uint8_t)0x06);
    for (int i = argCount; i < 12; i++) dbClient.write((uint8_t)0x00);

    // Args
    for (int i = 0; i < argCount; i++) {
      dbClient.write((uint8_t)0x11);
      uint32_t val = args[i];
      uint8_t b[] = { (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val };
      dbClient.write(b, 4);
    }
  }

  bool readBytesWithTimeout(uint8_t* buf, size_t len, int timeoutMs = 500) {
    unsigned long start = millis();
    size_t read = 0;
    while (read < len) {
      if (millis() - start > timeoutMs) return false;
      if (dbClient.available()) {
        buf[read++] = dbClient.read();
      } else {
        delay(1);
      }
    }
    return true;
  }

  String getPhraseName(uint16_t kind, uint16_t mood, uint8_t k1, uint8_t k2, uint8_t k3) {
    // Mood 1: High (Default)
    if (mood == 1 || mood == 0) {
      switch (kind) {
      case 1: return (k1 == 1) ? F("Intro 1") : F("Intro 2");
      case 2:
        if (k2 == 0 && k3 == 0) return F("Up 1");
        if (k2 == 0 && k3 == 1) return F("Up 2");
        if (k2 == 1 && k3 == 0) return F("Up 3");
        return F("Up");
      case 3: return F("Down");
      case 5: return (k1 == 1) ? F("Chorus 2") : F("Chorus 1"); // Inverted
      case 6: return (k1 == 1) ? F("Outro 1") : F("Outro 2");
      default: return "High " + String(kind);
      }
    }
    // Mood 2: Mid
    else if (mood == 2) {
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
    }
    // Mood 3: Low
    else if (mood == 3) {
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
    if (fetchState == IDLE || fetchState == COOLDOWN) return;

    if (millis() - stateTimer > 3000) {
      if (enableDebug) Serial.println(F("[ProLink] Fetch Timeout"));
      dbClient.stop();
      fetchState = COOLDOWN;
      stateTimer = millis();
      return;
    }

    switch (fetchState) {
    case CONNECT_STAGE1:
      if (dbClient.connect(linkState.activePlayerIP, PORT_DBSERVER)) {
        dbClient.write("\x00\x00\x00\x0fRemoteDBServer\x00", 19);
        fetchState = WAIT_STAGE1;
      }
      break;

    case WAIT_STAGE1:
      if (dbClient.available() >= 2) {
        uint8_t buf[2];
        dbClient.read(buf, 2);
        dynamicPort = (buf[0] << 8) | buf[1];
        dbClient.stop();
        fetchState = CONNECT_STAGE2;
        if (enableDebug) Serial.printf("[ProLink] Port: %u\n", dynamicPort);
      }
      break;

    case CONNECT_STAGE2:
      if (dbClient.connect(linkState.activePlayerIP, dynamicPort)) {
        uint8_t hand1[] = { 0x11, 0,0,0,1 };
        dbClient.write(hand1, 5);
        fetchState = HANDSHAKE;
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
      }
      break;

    case WAIT_HANDSHAKE:
      if (dbClient.available() > 10) {
        while (dbClient.available()) dbClient.read();
        fetchState = REQUEST_PHRASES;
      }
      break;

    case REQUEST_PHRASES: {
      uint32_t compound = (WLED_DEVICE_ID << 24) | (linkState.currentSlot << 16) | 0x0301;
      uint32_t pssi = 0x49535350; // "ISSP"
      uint32_t ext = 0x00545845;  // "\0TXE"
      uint32_t args[] = { compound, linkState.currentTrackId, pssi, ext };
      sendDBQuery(0x2c04, args, 4);
      fetchState = WAIT_PHRASES;
      break;
    }

    case WAIT_PHRASES:
      if (dbClient.available() > 32) {
        bool found = false;
        while (dbClient.available() >= 4) {
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
          parsePSSI();
          if (enableDebug) printPhrases();
          dbClient.stop();
          fetchState = IDLE;
          lastAnalyzedTrackId = linkState.currentTrackId;
          if (enableDebug) Serial.println(F("[ProLink] PSSI Parsed Success"));
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

  void parsePSSI() {
    uint8_t head[28];
    if (!readBytesWithTimeout(head, 28)) return;

    uint32_t headerLen = (head[0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3];
    uint16_t numEntries = (head[12] << 8) | head[13];

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
    while (readSoFar < headerLen) { dbClient.read(); readSoFar++; }

    // Read Entries
    for (int i = 0; i < numEntries; i++) {
      uint8_t e[24];
      if (!readBytesWithTimeout(e, 24)) break;

      // Decrypt Entry
      for (int b = 0; b < 24; b++) {
        int tagOffset = headerLen + (i * 24) + b;
        e[b] = e[b] ^ ((XOR_KEY[(tagOffset - 18) % 19] + numEntries) & 0xFF);
      }

      // Parse Fields
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

      // Linearization Logic (Splitting Fills)
      if (fill && beatFill > beat) {
        // 1. Main Part
        pe.count = beatFill - beat;
        phrases.push_back(pe);

        // 2. Fill Part
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
        phrases[i].count = 32; // Fallback default
      }
    }
    prolink_total_phrases_public = phrases.size();
  }

  void updatePhraseState() {

    static uint8_t previous_phrase = 0;

    if (phrases.empty()) {
      // If phrase list is empty (no track loaded/analyzed), reset all.
      prolink_phrase_name_public = "No Analysis";
      prolink_phrase_index_public = -1;
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
      return;
    }

    uint32_t currentBeat = linkState.beatsElapsed;

    // Ensure currentBeat is valid (avoid 0, which often falls outside the phrase structure)
    // We use the absolute beat number. The first phrase usually starts >= 1.
    if (currentBeat < 1) {
      currentBeat = 1;
    }

    int activeIdx = -1;
    // --- Core Logic: Find active phrase ---
    for (size_t i = 0; i < phrases.size(); i++) {
      // Check if currentBeat falls within the range [startBeat, startBeat + count)
      if (currentBeat >= phrases[i].startBeat && currentBeat < (phrases[i].startBeat + phrases[i].count)) {
        activeIdx = i;
        break;
      }
    }
    // --- End Find active phrase ---

    if (activeIdx != previous_phrase) {
      uint16_t high_bits = random16();
      uint16_t low_bits = random16();
      uint32_t random32_value = ((uint32_t)high_bits << 16) | low_bits;
      strip.fill(random32_value);
      previous_phrase = activeIdx;
    }


    if (activeIdx != -1) {
      // --- Update Public Variables (Active Phrase Found) ---
      prolink_phrase_index_public = activeIdx;
      prolink_phrase_name_public = phrases[activeIdx].label;
      prolink_phrase_beats_public = phrases[activeIdx].count;

      uint32_t beatsIntoPhrase = currentBeat - phrases[activeIdx].startBeat;

      // Add progress within the beat (0.0 - 0.99)
      float totalProgress = (float)beatsIntoPhrase + prolink_beat_progress_public;
      if (phrases[activeIdx].count > 0) {
        prolink_phrase_progress_public = totalProgress / (float)phrases[activeIdx].count;
      }
    } else {
      // --- Reset Public Variables (No Phrase Match Found / Gap) ---
      prolink_phrase_index_public = -1;
      prolink_phrase_name_public = "Gap/End";
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
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

  // --- ADDED: Helper to calculate effective BPM ---
  float getEffectiveBPM() {
    if (linkState.activePlayerID > 0 && linkState.activePlayerID <= 4) {
      float effectivePitch = linkState.effectivePitch;
      // linkState.bpm is track BPM (from beat packet)
      return linkState.bpm * (1.0f + (effectivePitch / 100.0f));
    }
    return linkState.bpm;
  }

  void loop() {
    if (!enabled) return;

    // Handle Metadata TCP State Machine
    handleFetchStateMachine();

    // --- BEAT PROGRESS CALCULATION ---
    // This logic relies on tracking the system time (millis()) since the last detected beat.
    static unsigned long lastBeatTime = 0;
    static uint32_t lastBeatNumber = 0;

    // Check if a new beat number has been received (from parseBeatPacket)
    if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) {
      lastBeatTime = millis();
      lastBeatNumber = linkState.beatNumber;
      // You could trigger a visual effect here if needed (e.g., enableBeatFlash)
    }

    if (linkState.bpm > 0 && lastBeatTime > 0) {
      float effectiveBpm = getEffectiveBPM();
      float beatDurationMs = 60000.0f / effectiveBpm;
      unsigned long timeSinceBeat = millis() - lastBeatTime;

      // Calculate progress (0.0 to 0.999...)
      float progress = timeSinceBeat / beatDurationMs;

      // Ensure progress wraps correctly (though beat packets should handle major jumps)
      while (progress >= 1.0f) {
        progress -= 1.0f;
        lastBeatTime += (unsigned long)beatDurationMs; // Adjust the reference time
      }

      prolink_beat_progress_public = progress;
    } else {
      prolink_beat_progress_public = 0.0f;
    }
    // --- END BEAT PROGRESS CALCULATION ---


    // Determine current phrase based on beats
    updatePhraseState();

    // Update Public Vars (ALWAYS LAST)
    prolink_bpm_public = getEffectiveBPM();
    prolink_beat_public = linkState.beatInMeasure;
    prolink_beat_number_public = linkState.beatNumber;
    prolink_track_id_public = linkState.currentTrackId;
    prolink_beats_elapsed_public = linkState.beatsElapsed;
    prolink_bars_elapsed_public = linkState.halfBarsElapsed / 2;
    prolink_bars_remaining_public = linkState.halfBarsRemaining / 2;


    // Keepalive (every 1.5s)
    static unsigned long lastKA = 0;
    if (millis() - lastKA > 1500) {
      sendKeepAlive();
      lastKA = millis();
    }
  }

  void addToConfig(JsonObject& root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_beatDebug)] = enableDebug;
    top[FPSTR(_pitchDebug)] = enableBeatFlash; // Reuse key for now or add new one
  }

  bool readFromConfig(JsonObject& root) {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;
    enabled = top[FPSTR(_enabled)] | enabled;
    enableDebug = top[FPSTR(_beatDebug)] | enableDebug;
    enableBeatFlash = top[FPSTR(_pitchDebug)] | enableBeatFlash;
    return true;
  }

  void addToJsonInfo(JsonObject& root) {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    if (enabled) {
      JsonArray infoArr = user.createNestedArray("Pro DJ Link");
      String status = linkState.isMaster ? "M" + String(linkState.activePlayerID) : "Waiting";
      status += " | " + String(linkState.bpm * 10.0, 1) + " BPM";
      infoArr.add(status);

      if (prolink_phrase_name_public.length() > 0) {
        infoArr.add("<br />" + prolink_phrase_name_public + " (" + String((int)(prolink_phrase_progress_public * 100)) + "%)");
      }
    }
  }

  uint16_t getId() { return 0xCD30; }
};

// --- Static Definitions ---
const char ProLinkUsermod::_name[] PROGMEM = "Pro DJ Link";
const char ProLinkUsermod::_enabled[] PROGMEM = "Enabled";
const char ProLinkUsermod::_beatDebug[] PROGMEM = "Beat Packet Debug";
const char ProLinkUsermod::_pitchDebug[] PROGMEM = "Pitch Debug";