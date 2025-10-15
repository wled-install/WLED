#ifndef ARTNET_RECEIVER_H
#define ARTNET_RECEIVER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

#define ARTNET_PORT 6454
#define ARTNET_MAX_UNIVERSES 64 // Increased for 8192 pixels
#define DMX_UNIVERSE_SIZE 512
#define ARTNET_MIN_HEADER_SIZE 18
#define LEDS_PER_UNIVERSE (DMX_UNIVERSE_SIZE / 3) // 170 LEDs
#define BYTES_PER_UNIVERSE (LEDS_PER_UNIVERSE * 3) // 510 bytes

// NEW: Timeout in ms to wait for a full frame. 40ms = 25 FPS.
#define ARTNET_FRAME_TIMEOUT 40 

typedef struct __attribute__((packed)) {
  uint8_t  id[8];
  uint16_t opCode;
  uint16_t protVer;
  uint8_t  sequence;
  uint8_t  physical;
  uint16_t universe;
  uint16_t length;
  uint8_t  data[DMX_UNIVERSE_SIZE];
} ArtNetDmxPacket;

class ArtNetReceiver {
public:
  ArtNetReceiver();
  ~ArtNetReceiver();

  bool begin(UBaseType_t task_priority = 5, BaseType_t core_id = 0);
  void stop();
  void processNewFrame();

private:
  static void _network_task_entry(void* arg);
  void _network_task_loop();
  void _process_frame();
  void _reset_frame_state(); // Helper to reset frame tracking

  TaskHandle_t _task_handle = nullptr;
  int _sock = -1;
  volatile bool _is_running = false;

  uint16_t _start_universe = 0;
  uint16_t _totalUniverses = 0;

  // Frame buffering state
  bool _received_universes[ARTNET_MAX_UNIVERSES];
  uint16_t _received_count = 0;
  uint8_t _current_frame_sequence;
  uint8_t _previous_frame_sequence;
  
  // PSRAM buffer for DMX data
  // uint8_t(*_dmx_data)[DMX_UNIVERSE_SIZE] = nullptr;
  uint8_t(*_dmx_buffers[2])[DMX_UNIVERSE_SIZE]; // An array holding two buffer pointers
  std::atomic<int> _active_buffer_idx;          // The buffer the MAIN loop should READ
  std::atomic<bool> _new_frame_ready;           // Flag to signal the main loop
  void _process_frame_internal();

  static const char* TAG;
};

#endif // ARTNET_RECEIVER_H
