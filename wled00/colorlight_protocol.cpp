#include "colorlight_protocol.h"
#include <string.h>

namespace ColorLight {

size_t buildEthernetHeader(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC, uint8_t packetType) {
  // Destination MAC (6 bytes)
  memcpy(buffer, destMAC, 6);

  // Source MAC (6 bytes)
  memcpy(buffer + 6, srcMAC, 6);

  // EtherType (2 bytes) - ColorLight uses packet type as MSB, 0xB8 as LSB
  // Examples: 0x01B8 (sync), 0x0AB8 (brightness), 0x55B8 (pixel data)
  buffer[12] = packetType;  // Packet type = EtherType high byte
  buffer[13] = COLORLIGHT_ETHERTYPE_LSB;  // Always 0xB8

  return 14;  // Standard Ethernet header size
}

size_t buildBrightnessPacket(uint8_t* buffer, const uint8_t* destMAC,
                             const uint8_t* srcMAC, uint8_t brightness) {
  size_t offset = 0;

  // Ethernet header with EtherType = 0x0AB8
  offset += buildEthernetHeader(buffer + offset, destMAC, srcMAC, COLORLIGHT_BRIG_PACKET_TYPE);

  // Payload starts at offset 14
  // FPP sends 4 brightness bytes, then padding
  buffer[offset++] = brightness;
  buffer[offset++] = brightness;
  buffer[offset++] = brightness;
  buffer[offset++] = brightness;

  // Padding to reach total packet size of 77 bytes
  // 77 total = 14 (Eth header) + 63 (payload)
  // We've written 4 bytes, need 59 more
  memset(buffer + offset, 0, 59);
  offset += 59;

  return offset;  // Should be 77 bytes
}

size_t buildSyncPacket(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC) {
  size_t offset = 0;

  // Ethernet header with EtherType = 0x01B8
  offset += buildEthernetHeader(buffer + offset, destMAC, srcMAC, COLORLIGHT_SYNC_PACKET_TYPE);

  // Payload starts at offset 14
  // FPP format: Data[0]=0x07, Data[22]=brightness, Data[23]=0x05, Data[25-27]=0xFF 0xFF 0xFF
  // Total packet size: 112 bytes = 14 (Eth header) + 98 (payload)

  // Initialize all payload bytes to 0
  memset(buffer + offset, 0, 98);

  // Set FPP-specific bytes
  buffer[offset + 0] = 0x07;   // Data[0]
  buffer[offset + 22] = 0xFF;  // Data[22] (brightness)
  buffer[offset + 23] = 0x05;  // Data[23]
  buffer[offset + 25] = 0xFF;  // Data[25]
  buffer[offset + 26] = 0xFF;  // Data[26]
  buffer[offset + 27] = 0xFF;  // Data[27]

  offset += 98;

  return offset;  // Should be 112 bytes
}

size_t buildPixelDataPacket(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC,
                            uint8_t rowIndex, const uint8_t* pixelData, uint16_t pixelCount) {
  // Validate pixel count
  if (pixelCount > COLORLIGHT_MAX_PIXELS_PER_PACKET) {
    pixelCount = COLORLIGHT_MAX_PIXELS_PER_PACKET;
  }

  size_t offset = 0;

  // Ethernet header with EtherType = 0x55B8
  offset += buildEthernetHeader(buffer + offset, destMAC, srcMAC, COLORLIGHT_PIXL_PACKET_TYPE);

  // Payload starts at offset 14 (FPP format):
  // Data[0-1]: Row Number (MSB, LSB) - always 0 for strip/single row mode
  buffer[offset++] = 0x00;        // Row MSB
  buffer[offset++] = 0x00;        // Row LSB

  // Data[2-3]: Pixel Offset (MSB, LSB) - starting pixel offset in this packet
  uint16_t pixelOffset = rowIndex * COLORLIGHT_MAX_PIXELS_PER_PACKET;
  buffer[offset++] = (pixelOffset >> 8) & 0xFF;   // Offset MSB
  buffer[offset++] = pixelOffset & 0xFF;          // Offset LSB

  // Data[4-5]: Pixel Count (MSB, LSB)
  buffer[offset++] = (pixelCount >> 8) & 0xFF;    // Count MSB
  buffer[offset++] = pixelCount & 0xFF;           // Count LSB

  // Data[6-7]: Magic bytes
  buffer[offset++] = 0x08;
  buffer[offset++] = 0x88;

  // Data[8+]: RGB pixel data (3 bytes per pixel)
  size_t dataSize = pixelCount * 3;
  memcpy(buffer + offset, pixelData, dataSize);
  offset += dataSize;

  // Ensure minimum Ethernet frame size
  if (offset < COLORLIGHT_ETH_MIN_FRAME) {
    memset(buffer + offset, 0, COLORLIGHT_ETH_MIN_FRAME - offset);
    offset = COLORLIGHT_ETH_MIN_FRAME;
  }

  return offset;
}

size_t buildDiscoveryPacket(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC) {
  size_t offset = 0;

  // Ethernet header with EtherType = 0x07B8
  offset += buildEthernetHeader(buffer + offset, destMAC, srcMAC, COLORLIGHT_DISC_PACKET_TYPE);

  // Payload: 284 total = 14 (Eth header) + 270 (payload)
  memset(buffer + offset, 0, 270);
  offset += 270;

  return offset;  // Should be 284 bytes
}

} // namespace ColorLight
