// The #include order is very specific to work with Arduino.h
// and avoid some specific conflicts/bugs.
//
#define NOMINMAX

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "ArtNetReceiver.h"

#ifdef INADDR_NONE
#undef INADDR_NONE
#endif
#ifdef IPADDR_NONE
#undef IPADDR_NONE
#endif

#include "pin_manager.h"
#include "bus_manager.h"
#include "wled.h"

#include <algorithm>
#include <string.h>
#include "esp_heap_caps.h"

const char* ArtNetReceiver::TAG = "ArtNetReceiver";

ArtNetReceiver::ArtNetReceiver()
  : _dmx_buffers { nullptr, nullptr }
{
  _reset_frame_state();
}

ArtNetReceiver::~ArtNetReceiver() {
  stop();
  for (int i = 0; i < 2; ++i) {
    if (_dmx_buffers[i]) {
      heap_caps_free(_dmx_buffers[i]);
      _dmx_buffers[i] = nullptr;
    }
  }
}

void ArtNetReceiver::_reset_frame_state() {
  memset(_received_universes, false, sizeof(_received_universes));
  _received_count = 0;
}

bool ArtNetReceiver::begin(uint16_t _init_start_universe, UBaseType_t task_priority, BaseType_t core_id) {
  if (_is_running) {
    USER_PRINTLN("ArtNetReceiver: Receiver is already running.");
    return true;
  }

  _start_universe = _init_start_universe;
  _current_frame_sequence = 0;
  _previous_frame_sequence = 0;

  Bus* bus = busses.getBus(0);
  uint32_t busLedCount = 0;

  if (bus) {
    busLedCount = bus->getLength();
    if (_totalUniverses == 0) {
      _totalUniverses = (busLedCount + (LEDS_PER_UNIVERSE - 1)) / LEDS_PER_UNIVERSE;
    }
  } else {
    _totalUniverses = 0;
  }
  
  if (_totalUniverses > ARTNET_MAX_UNIVERSES) _totalUniverses = ARTNET_MAX_UNIVERSES;

  if (_totalUniverses == 0) {
    USER_PRINTLN("ArtNetReceiver: No universes to listen for. Bus 0 might have 0 length. Exiting.");
    return true;
  }

  USER_PRINTF("ArtNetReceiver: Configured to listen for %u universes for %u LEDs.\n", _totalUniverses, busLedCount);
  if (_start_universe > 0) USER_PRINTF("ArtNetReceiver: Starting listening at %u\n", _start_universe);

  if (!_dmx_buffers[0] || !_dmx_buffers[1]) {

    _dmx_buffers[0] = (uint8_t*)heap_caps_malloc(_totalUniverses * BYTES_PER_UNIVERSE, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD);
    _dmx_buffers[1] = (uint8_t*)heap_caps_malloc(_totalUniverses * BYTES_PER_UNIVERSE, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD);

    if (!_dmx_buffers[0] || !_dmx_buffers[1]) {
      USER_PRINTLN("ArtNetReceiver: Failed to allocate DMX buffer in PSRAM. Exiting.");
      return false;
    }
  }

  _is_running = true;

  BaseType_t result = xTaskCreatePinnedToCore(_network_task_entry, "artnet_in", 4096, this, task_priority, &_task_handle, core_id);

  if (result != pdPASS) {
    USER_PRINTLN("ArtNetReceiver: Failed to create network task. Exiting.");
    _is_running = false;
    return false;
  }

  USER_PRINTF("ArtNetReceiver: Art-Net receiver task started on core %d with priority %d.\n", core_id, task_priority);
  return true;
}

void ArtNetReceiver::stop() {
  if (!_is_running) return;
  _is_running = false;
  if (_sock != -1) {
    shutdown(_sock, 0);
    close(_sock);
    _sock = -1;
  }
  _task_handle = nullptr;
  USER_PRINTLN("ArtNetReceiver: Art-Net receiver stopped. Exiting.");
}

void ArtNetReceiver::_network_task_entry(void* arg) {
  static_cast<ArtNetReceiver*>(arg)->_network_task_loop();
  vTaskDelete(NULL);
}

#if defined(CONFIG_IDF_TARGET_ESP32P4)
extern "C" {
  int IRAM_ATTR p4_mul16x16(uint8_t* outpacket, uint8_t* brightness, uint16_t num_loops, uint8_t* pixelbuffer);
}
#endif

void ArtNetReceiver::processNewFrame() {

  if (!_new_frame_ready.load()) {
    return;
  }
  _new_frame_ready = false;

  _process_frame_internal();
}

void ArtNetReceiver::_process_frame_internal() {
  int read_buffer_idx = _active_buffer_idx.load();
  if (_received_count == 0) {
    _reset_frame_state();
    return;
  }

  realtimeLock(realtimeTimeoutMs, REALTIME_MODE_ARTNET);
  newArtNetData = false;
  if (xSemaphoreTake(busMutex, portMAX_DELAY) == pdTRUE) {
    Bus* bus = busses.getBus(0);
    if (bus) {
      uint8_t* busPixelData = bus->getPixelData();
      uint32_t busLedCount = bus->getLength();
      uint32_t bus_len_bytes = busLedCount * 3;

      #if defined(CONFIG_IDF_TARGET_ESP32P4)
      // This might bite you. Make sure your random buffers are +15 bytes
      // ...or don't be fancy and just the memcpy version.
      uint32_t groupsOf16 = (bus_len_bytes >> 4) + (bus_len_bytes & 0x0F) ? 0 : 1; 
      uint8_t fakebri = 255;
      p4_mul16x16(busPixelData, &fakebri, groupsOf16, _dmx_buffers[read_buffer_idx]);
      #else
      memcpy(busPixelData, _dmx_buffers[read_buffer_idx], bus_len_bytes); // tried and true
      #endif
    }
    newArtNetData = true;
    xSemaphoreGive(busMutex);
  } else {
    USER_PRINTLN("ArtNetReceiver: Failed to take bus mutex. This should never happen.");
  }
}

void IRAM_ATTR ArtNetReceiver::_network_task_loop() {
  _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (_sock < 0) {
    USER_PRINTF("ArtNetReceiver: Failed to create socket: errno %d. Exiting.\n", errno);
    _is_running = false; return;
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = 0;
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(ARTNET_PORT);

  if (bind(_sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) < 0) {
    USER_PRINTF("ArtNetReceiver: Socket unable to bind: errno %d. Exiting.\n", errno);
    close(_sock); _sock = -1; _is_running = false; return;
  }

  const size_t rx_buffer_size = sizeof(ArtNetDmxPacket);

  static uint8_t* rx_buffer = (uint8_t*)heap_caps_malloc(rx_buffer_size, MALLOC_CAP_TCM);
  const char ARTNET_ID[8] = "Art-Net";

  while (_is_running) {

    int write_buffer_idx = 1 - _active_buffer_idx.load();
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(_sock, rx_buffer, rx_buffer_size, 0, (struct sockaddr*)&source_addr, &socklen);

    if (len < 0) {
      if (_is_running) USER_PRINTF("ArtNetReceiver: recvfrom failed : errno %d\n", errno);
      break;
    }

    if (len < ARTNET_MIN_HEADER_SIZE+3) {
      USER_PRINTF("ArtNetReceiver: Length mismatch %u < %u Skipping.\n", len, ARTNET_MIN_HEADER_SIZE + 3);
      continue;
    }

    ArtNetDmxPacket* packet = (ArtNetDmxPacket*)rx_buffer;

    if (memcmp(packet->id, ARTNET_ID, sizeof(ARTNET_ID)) != 0 || packet->opCode != 0x5000) {
      USER_PRINTF("ArtNetReceiver: Not an Art-Net packet\n");
      continue;
    }

    uint16_t received_universe = packet->universe;
    uint16_t universe_index = received_universe - _start_universe;

    uint16_t declared_dmx_length = ntohs(packet->length);
    uint16_t actual_dmx_length = len - ARTNET_MIN_HEADER_SIZE;

    if (actual_dmx_length < declared_dmx_length) {
      USER_PRINTF("ArtNetReceiver: Packet size mismatch U %u S %u. Skipping.\n", packet->universe, packet->sequence);
      continue;
    }

    if (_current_frame_sequence == 0 || packet->sequence != _current_frame_sequence) {
      _current_frame_sequence = packet->sequence;
      _reset_frame_state();
    }

    if (packet->sequence != _current_frame_sequence) {
      USER_PRINTF("\nArtNetReceiver: Unexpected sequence %d. Skipping.\n", packet->sequence);
      continue;
    }

    if (received_universe >= _start_universe && received_universe < (_start_universe + _totalUniverses)) {
      uint16_t universe_index = received_universe - _start_universe;

      realtimeLock(realtimeTimeoutMs, REALTIME_MODE_ARTNET);

      if (!_received_universes[universe_index]) {
        _received_universes[universe_index] = true;
        _received_count++;
      }

      uint16_t data_len = ntohs(packet->length);
      uint32_t offset = universe_index * BYTES_PER_UNIVERSE;
      uint32_t total_buffer_size = _totalUniverses * BYTES_PER_UNIVERSE;

      if (offset < total_buffer_size) {
        uint16_t bytes_to_copy = std::min((uint32_t)data_len, total_buffer_size - offset);
        uint8_t* target_ptr = _dmx_buffers[write_buffer_idx] + offset;
        memcpy(target_ptr, packet->data, bytes_to_copy);
      }

      if (_received_count >= _totalUniverses) {
        _active_buffer_idx.store(write_buffer_idx);
        _new_frame_ready = true;

        _previous_frame_sequence = _current_frame_sequence;
        // USER_PRINTF("\nArtNetReceiver: Got %d/%d universes. Last universe was %u - processing sequence %u.\n", _received_count, _totalUniverses, received_universe+1, _current_frame_sequence);
      }
    } else {
      uint16_t universe_index = received_universe - _start_universe;
      USER_PRINTF("ArtNetReceiver: Failed U %d from U %d to U %d mapped to U %u\n", received_universe, _start_universe, (_start_universe + _totalUniverses), universe_index);
    }
  }

  if (_sock != -1) {
    close(_sock);
    _sock = -1;
  }
  _is_running = false;
  USER_PRINTLN("ArtNetReceiver: Network task finished.");
}
