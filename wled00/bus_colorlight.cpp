#ifdef WLED_ENABLE_COLORLIGHT

#include "wled.h"
#include <esp_eth.h>
#include <esp_err.h>
#include "bus_manager.h"

BusColorLight5A75B::BusColorLight5A75B(BusConfig &bc, const ColorOrderMap &com)
  : Bus(bc.type, bc.start, bc.autoWhite)
  , _colorOrderMap(com)
  , _transmitLock(false)
  , _lastShowTime(0)
  , _lastBrightness(255)
  , _linkUp(false)
  , _lastLinkCheck(0)
{
  _valid = false;
  _pixelBuffer = nullptr;

  // Determine if RGBW
  _rgbw = (bc.type == TYPE_NET_COLORLIGHT_RGBW);

  // Try broadcast MAC address first: FF:FF:FF:FF:FF:FF
  _receiverMAC[0] = 0x11;
  _receiverMAC[1] = 0x22;
  _receiverMAC[2] = 0x33;
  _receiverMAC[3] = 0x44;
  _receiverMAC[4] = 0x55;
  _receiverMAC[5] = 0x66;

  // Panel dimensions: single row (works like a strip)
  _panelWidth = bc.count;
  _panelHeight = 1;

  // Store configuration
  _len = bc.count;
  _colorOrder = bc.colorOrder;
  _fpsLimit = bc.fps_limit > 0 ? bc.fps_limit : 30;

  // Allocate pixel buffer (RGB * pixel count)
  uint32_t channels = _rgbw ? 4 : 3;
  _bufferSize = _len * channels;

  _pixelBuffer = (uint8_t*)heap_caps_calloc(
    _bufferSize, 1,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
  );

  if (!_pixelBuffer) {
    // Try internal memory if SPIRAM fails
    _pixelBuffer = (uint8_t*)heap_caps_calloc(
      _bufferSize, 1,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
    );
  }

  if (!_pixelBuffer) {
    USER_PRINTF("[ColorLight] Error: Failed to allocate %u bytes for pixel buffer\n", _bufferSize);
    return;
  }

  // Get Ethernet handle (global from wled.h)
  extern esp_eth_handle_t eth_handle;
  _ethHandle = eth_handle;

  if (!_ethHandle) {
    USER_PRINTLN(F("[ColorLight] Error: Ethernet not initialized"));
    cleanup();
    return;
  }

  // Get our MAC address
  getMACAddress();

  _valid = true;

  USER_PRINTF("[ColorLight 5A-75B] Initialized: %u LEDs, MAC: %02X:%02X:%02X:%02X:%02X:%02X, FPS: %u\n",
              _len,
              _receiverMAC[0], _receiverMAC[1], _receiverMAC[2],
              _receiverMAC[3], _receiverMAC[4], _receiverMAC[5],
              _fpsLimit);

  // Debug: Check initial link status
  extern bool eth_link_up;
  USER_PRINTF("[ColorLight] Initial eth_link_up status: %s\n", eth_link_up ? "UP" : "DOWN");
}

BusColorLight5A75B::~BusColorLight5A75B() {
  cleanup();
}

void BusColorLight5A75B::getMACAddress() {
  esp_err_t result = esp_eth_ioctl(_ethHandle, ETH_CMD_G_MAC_ADDR, _ourMAC);
  if (result != ESP_OK) {
    USER_PRINTF("[ColorLight] Warning: Failed to get MAC address: %s\n", esp_err_to_name(result));
    // Use a default MAC if we can't read it
    memset(_ourMAC, 0xAA, 6);
  }
}

bool BusColorLight5A75B::checkLinkStatus() {
  // Use global eth_link_up status from event handlers (Layer 2 link, no IP needed)
  extern bool eth_link_up;

  bool wasUp = _linkUp;
  _linkUp = eth_link_up;

  // Log link status changes (throttled to once per second)
  uint32_t now = millis();
  if (_linkUp != wasUp && (now - _lastLinkCheck) >= 1000) {
    _lastLinkCheck = now;
    if (_linkUp) {
      USER_PRINTLN(F("[ColorLight] Ethernet link UP - ready to transmit"));
    } else {
      USER_PRINTLN(F("[ColorLight] Ethernet link DOWN"));
    }
  }

  return _linkUp;
}

void IRAM_ATTR BusColorLight5A75B::setPixelColor(uint32_t pix, uint32_t c) {
  if (!_valid || pix >= _len) return;

  // Apply auto white calculation if needed
  if (_rgbw) c = autoWhiteCalc(c);

  // Get color order for this pixel
  uint8_t co = _colorOrderMap.getPixelColorOrder(pix + _start, _colorOrder);

  // Calculate buffer offset
  uint32_t channels = _rgbw ? 4 : 3;
  uint32_t offset = pix * channels;

  // Store RGB(W) data in correct color order
  switch (co & 0x0F) {  // Mask to get base color order
    case COL_ORDER_GRB:
      _pixelBuffer[offset + 0] = G(c);
      _pixelBuffer[offset + 1] = R(c);
      _pixelBuffer[offset + 2] = B(c);
      break;
    case COL_ORDER_RGB:
      _pixelBuffer[offset + 0] = R(c);
      _pixelBuffer[offset + 1] = G(c);
      _pixelBuffer[offset + 2] = B(c);
      break;
    case COL_ORDER_BRG:
      _pixelBuffer[offset + 0] = B(c);
      _pixelBuffer[offset + 1] = R(c);
      _pixelBuffer[offset + 2] = G(c);
      break;
    case COL_ORDER_RBG:
      _pixelBuffer[offset + 0] = R(c);
      _pixelBuffer[offset + 1] = B(c);
      _pixelBuffer[offset + 2] = G(c);
      break;
    case COL_ORDER_GBR:
      _pixelBuffer[offset + 0] = G(c);
      _pixelBuffer[offset + 1] = B(c);
      _pixelBuffer[offset + 2] = R(c);
      break;
    case COL_ORDER_BGR:
      _pixelBuffer[offset + 0] = B(c);
      _pixelBuffer[offset + 1] = G(c);
      _pixelBuffer[offset + 2] = R(c);
      break;
    default:
      _pixelBuffer[offset + 0] = R(c);
      _pixelBuffer[offset + 1] = G(c);
      _pixelBuffer[offset + 2] = B(c);
      break;
  }

  if (_rgbw) {
    _pixelBuffer[offset + 3] = W(c);
  }
}

uint32_t BusColorLight5A75B::getPixelColor(uint32_t pix) const {
  if (!_valid || pix >= _len) return 0;

  uint32_t channels = _rgbw ? 4 : 3;
  uint32_t offset = pix * channels;

  uint8_t co = _colorOrderMap.getPixelColorOrder(pix + _start, _colorOrder);

  uint8_t r, g, b, w = 0;

  // Read back in correct color order
  switch (co & 0x0F) {
    case COL_ORDER_GRB:
      g = _pixelBuffer[offset + 0];
      r = _pixelBuffer[offset + 1];
      b = _pixelBuffer[offset + 2];
      break;
    case COL_ORDER_RGB:
      r = _pixelBuffer[offset + 0];
      g = _pixelBuffer[offset + 1];
      b = _pixelBuffer[offset + 2];
      break;
    case COL_ORDER_BRG:
      b = _pixelBuffer[offset + 0];
      r = _pixelBuffer[offset + 1];
      g = _pixelBuffer[offset + 2];
      break;
    case COL_ORDER_RBG:
      r = _pixelBuffer[offset + 0];
      b = _pixelBuffer[offset + 1];
      g = _pixelBuffer[offset + 2];
      break;
    case COL_ORDER_GBR:
      g = _pixelBuffer[offset + 0];
      b = _pixelBuffer[offset + 1];
      r = _pixelBuffer[offset + 2];
      break;
    case COL_ORDER_BGR:
      b = _pixelBuffer[offset + 0];
      g = _pixelBuffer[offset + 1];
      r = _pixelBuffer[offset + 2];
      break;
    default:
      r = _pixelBuffer[offset + 0];
      g = _pixelBuffer[offset + 1];
      b = _pixelBuffer[offset + 2];
      break;
  }

  if (_rgbw) {
    w = _pixelBuffer[offset + 3];
  }

  return RGBW32(r, g, b, w);
}

bool BusColorLight5A75B::canShow() {
  if (!_valid) return false;
  if (_transmitLock) return false;

  // Check Ethernet link status
  if (!checkLinkStatus()) {
    // Debug: Log why we can't show (throttled)
    static uint32_t lastDebugLog = 0;
    uint32_t now = millis();
    if ((now - lastDebugLog) > 5000) {  // Log every 5 seconds
      lastDebugLog = now;
      USER_PRINTLN(F("[ColorLight] Cannot show: Ethernet link is DOWN"));
    }
    return false;  // Link is down, cannot transmit
  }

  // FPS limiting
  if (_fpsLimit > 0) {
    uint32_t minInterval = 1000 / _fpsLimit;
    uint32_t now = millis();
    if ((now - _lastShowTime) < minInterval) {
      return false;
    }
  }

  return true;
}

void IRAM_ATTR BusColorLight5A75B::show() {
  if (!_valid || !canShow()) return;

  _transmitLock = true;

  // Debug: Log transmission (throttled)
  static uint32_t lastShowLog = 0;
  static uint32_t showCount = 0;
  showCount++;
  uint32_t now = millis();
  if ((now - lastShowLog) > 5000) {
    lastShowLog = now;
    USER_PRINTF("[ColorLight] show() called %u times, sending %u packets\n",
                showCount, (uint32_t)((_len + COLORLIGHT_MAX_PIXELS_PER_PACKET - 1) / COLORLIGHT_MAX_PIXELS_PER_PACKET) + 2);
    showCount = 0;
  }

  // Send brightness packet if changed
  if (_bri != _lastBrightness) {
    sendBrightnessPacket();
    _lastBrightness = _bri;
  }

  // Send pixel data
  sendPixelData();

  // Send sync packet to latch data
  sendSyncPacket();

  _lastShowTime = millis();
  _transmitLock = false;
}

bool BusColorLight5A75B::sendEthernetFrame(const uint8_t* frame, size_t len) {
  if (!_ethHandle) {
    USER_PRINTLN(F("[ColorLight] Error: Ethernet handle is NULL"));
    return false;
  }

  // Retry logic for ESP_ERR_NO_MEM (transmit queue full)
  const int maxRetries = 20;
  const int retryDelayUs = 500;  // 500 microseconds between retries

  for (int retry = 0; retry < maxRetries; retry++) {
    esp_err_t result = esp_eth_transmit(_ethHandle, (void*)frame, len);

    if (result == ESP_OK) {
      return true;  // Success
    }

    if (result == ESP_ERR_NO_MEM) {
      // Transmit queue is full, wait a bit and retry
      if (retry < maxRetries - 1) {
        delayMicroseconds(retryDelayUs);
        yield();  // Give Ethernet task time to process queue
        continue;
      }
    }

    // Other errors or max retries reached
    if (result == ESP_ERR_INVALID_STATE) {
      USER_PRINTLN(F("[ColorLight] Transmit error: ESP_ERR_INVALID_STATE - Ethernet not ready or link down"));
      _linkUp = false;  // Force link status recheck
    } else if (result == ESP_ERR_NO_MEM) {
      // Only log NO_MEM error if we've exhausted retries (throttled)
      static uint32_t lastNoMemLog = 0;
      uint32_t now = millis();
      if ((now - lastNoMemLog) > 1000) {
        lastNoMemLog = now;
        USER_PRINTLN(F("[ColorLight] Transmit error: ESP_ERR_NO_MEM - Queue full after retries"));
      }
    } else if (result == ESP_ERR_TIMEOUT) {
      USER_PRINTLN(F("[ColorLight] Transmit error: ESP_ERR_TIMEOUT - Transmission timed out"));
    } else {
      USER_PRINTF("[ColorLight] Transmit error: %s (0x%x)\n", esp_err_to_name(result), result);
    }
    return false;
  }

  return false;
}

bool BusColorLight5A75B::sendBrightnessPacket() {
  uint8_t packet[128];  // CL_BRIG_PACKET_SIZE + headers

  size_t len = ColorLight::buildBrightnessPacket(
    packet, _receiverMAC, _ourMAC, _bri
  );

  return sendEthernetFrame(packet, len);
}

bool BusColorLight5A75B::sendSyncPacket() {
  uint8_t packet[256];  // CL_SYNC_PACKET_SIZE + headers

  size_t len = ColorLight::buildSyncPacket(
    packet, _receiverMAC, _ourMAC
  );

  return sendEthernetFrame(packet, len);
}

bool BusColorLight5A75B::sendPixelData() {
  uint32_t pixelsSent = 0;
  uint8_t rowIndex = 0;

  uint8_t packet[1518];  // Max Ethernet frame size

  while (pixelsSent < _len) {
    uint16_t pixelsInPacket = min(
      (uint16_t)COLORLIGHT_MAX_PIXELS_PER_PACKET,
      (uint16_t)(_len - pixelsSent)
    );

    // Get pixel data pointer (RGB data only, 3 bytes per pixel)
    const uint8_t* pixelData = _pixelBuffer + (pixelsSent * 3);

    size_t len = ColorLight::buildPixelDataPacket(
      packet, _receiverMAC, _ourMAC,
      rowIndex++, pixelData, pixelsInPacket
    );

    if (!sendEthernetFrame(packet, len)) {
      return false;
    }

    // Small delay between packets to prevent queue overflow
    // Only needed when sending multiple packets
    if (pixelsSent + pixelsInPacket < _len) {
      delayMicroseconds(200);  // 200us between packets (WiFi coexistence needs more time)
    }

    pixelsSent += pixelsInPacket;
  }

  return true;
}

uint8_t BusColorLight5A75B::getPins(uint8_t* pinArray) const {
  if (!_valid) return 0;

  // Return receiver MAC address in pins array (follows BusNetwork pattern)
  for (uint8_t i = 0; i < 4; i++) {
    pinArray[i] = _receiverMAC[i];
  }

  return 4;
}

void BusColorLight5A75B::cleanup() {
  _valid = false;

  if (_pixelBuffer) {
    heap_caps_free(_pixelBuffer);
    _pixelBuffer = nullptr;
  }

  _len = 0;
  _bufferSize = 0;
}

#endif // WLED_ENABLE_COLORLIGHT
