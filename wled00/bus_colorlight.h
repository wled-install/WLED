#pragma once

#include "colorlight_protocol.h"

#ifdef WLED_ENABLE_COLORLIGHT

#include <esp_eth.h>

// Forward declarations (bus_manager.h will be included before this file)
class Bus;
class ColorOrderMap;
struct BusConfig;

/**
 * BusColorLight5A75B - ColorLight 5A-75B LED receiver bus
 *
 * Sends LED data over Ethernet using the ColorLight proprietary protocol.
 * Allows using Ethernet for LED data while WiFi handles WLED control.
 *
 * Configuration:
 * - Hardcoded receiver MAC: FF:FF:FF:FF:FF:FF (broadcast - works with any receiver)
 * - count: LED count (treated as single row/strip)
 * - colorOrder: RGB/GRB/BRG/etc color order
 * - fps_limit: Maximum frame rate (default 30)
 */
class BusColorLight5A75B : public Bus {
public:
  BusColorLight5A75B(BusConfig &bc, const ColorOrderMap &com);
  ~BusColorLight5A75B();

  // Core Bus interface
  void setPixelColor(uint32_t pix, uint32_t c) override;
  uint32_t getPixelColor(uint32_t pix) const override;
  uint32_t getPixelColorRestored(uint32_t pix) const override { return getPixelColor(pix); }
  void show() override;
  bool canShow() override;
  void cleanup() override;

  // Configuration getters
  uint8_t getPins(uint8_t* pinArray) const override;
  uint32_t getLength() const override { return _len; }
  bool hasRGB() const override { return true; }
  bool hasWhite() const override { return _rgbw; }
  uint8_t getColorOrder() const override { return _colorOrder; }
  uint8_t get_fps_limit() const override { return _fpsLimit; }

  // Panel configuration
  uint16_t getPanelWidth() const { return _panelWidth; }
  uint16_t getPanelHeight() const { return _panelHeight; }

private:
  // Color order map (must be first - const reference)
  const ColorOrderMap &_colorOrderMap;

  // Network configuration
  uint8_t _receiverMAC[6];       // ColorLight receiver MAC address
  esp_eth_handle_t _ethHandle;   // ESP-IDF Ethernet handle
  uint8_t _ourMAC[6];            // Our MAC address (cached)

  // Panel configuration
  uint16_t _panelWidth;          // Panel width in pixels
  uint16_t _panelHeight;         // Panel height in pixels
  uint8_t _colorOrder;           // Color order (RGB/GRB/etc)
  bool _rgbw;                    // RGBW support (future)

  // Buffer management
  uint8_t* _pixelBuffer;         // RGB pixel data buffer
  uint32_t _bufferSize;          // Buffer size in bytes
  uint32_t _len;                 // Active pixel count

  // Protocol state
  bool _transmitLock;            // Prevent concurrent transmission
  uint32_t _lastShowTime;        // Last show() timestamp (for FPS limiting)
  uint8_t _lastBrightness;       // Last transmitted brightness
  uint8_t _fpsLimit;             // Maximum FPS
  bool _linkUp;                  // Ethernet link status (cached)
  uint32_t _lastLinkCheck;       // Last link status check timestamp

  // Helper methods
  bool sendEthernetFrame(const uint8_t* frame, size_t len);
  bool sendBrightnessPacket();
  bool sendSyncPacket();
  bool sendPixelData();
  void getMACAddress();
  bool checkLinkStatus();
};

#endif // WLED_ENABLE_COLORLIGHT
