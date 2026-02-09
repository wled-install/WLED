#pragma once

#include <stdint.h>
#include <stddef.h>

// ColorLight 5A-75B Protocol Constants
// Based on FPP (Falcon Player) reverse engineering:
// https://github.com/FalconChristmas/fpp/blob/master/src/channeloutput/ColorLight-5a-75.h

// Ethernet frame constants
#define COLORLIGHT_ETHERTYPE_MSB 0x88
#define COLORLIGHT_ETHERTYPE_LSB 0xB8

// Maximum pixels per packet (from FPP spec)
#define COLORLIGHT_MAX_PIXELS_PER_PACKET 497
#define COLORLIGHT_MAX_CHANNELS_PER_PACKET (COLORLIGHT_MAX_PIXELS_PER_PACKET * 3)

// Packet types
#define COLORLIGHT_SYNC_PACKET_TYPE 0x01  // Sync memory to outputs
#define COLORLIGHT_SYNC_PACKET_SIZE 112

#define COLORLIGHT_DISC_PACKET_TYPE 0x07  // Discover receivers
#define COLORLIGHT_DISC_PACKET_SIZE 284

#define COLORLIGHT_RESP_PACKET_TYPE 0x08  // Receiver response to discovery
#define COLORLIGHT_RESP_PACKET_SIZE 1070

#define COLORLIGHT_BRIG_PACKET_TYPE 0x0A  // Brightness control
#define COLORLIGHT_BRIG_PACKET_SIZE 77

#define COLORLIGHT_PIXL_PACKET_TYPE 0x55  // Row/pixel data
#define COLORLIGHT_PIXL_HEADER_SIZE 8

// Ethernet frame structure
#define COLORLIGHT_ETH_HEADER_SIZE 14  // Standard Ethernet header (6+6+2)
#define COLORLIGHT_ETH_MIN_FRAME 60    // Minimum Ethernet frame size

// ColorLight packet offsets (from FPP)
#define COLORLIGHT_PACKET_TYPE_OFFSET 12   // Packet type at byte 12 (EtherType MSB)
#define COLORLIGHT_PACKET_DATA_OFFSET 14   // Payload starts at byte 14

namespace ColorLight {

/**
 * Build Ethernet header for ColorLight packet
 * @param buffer Output buffer (must be at least 14 bytes)
 * @param destMAC Destination MAC address (6 bytes)
 * @param srcMAC Source MAC address (6 bytes)
 * @param packetType Packet type (0x01, 0x0A, 0x55, etc.) - becomes EtherType MSB
 * @return Number of bytes written (14 - standard Ethernet header)
 *
 * Note: ColorLight uses custom EtherType values where MSB = packet type, LSB = 0xB8
 * Examples: 0x01B8 (sync), 0x0AB8 (brightness), 0x55B8 (pixel data)
 */
size_t buildEthernetHeader(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC, uint8_t packetType);

/**
 * Build brightness control packet
 * @param buffer Output buffer (must be at least 77 bytes)
 * @param destMAC Destination MAC address
 * @param srcMAC Source MAC address
 * @param brightness Brightness value (0-255)
 * @return Total frame size in bytes (77)
 */
size_t buildBrightnessPacket(uint8_t* buffer, const uint8_t* destMAC,
                             const uint8_t* srcMAC, uint8_t brightness);

/**
 * Build sync packet (triggers display update)
 * @param buffer Output buffer (must be at least 112 bytes)
 * @param destMAC Destination MAC address
 * @param srcMAC Source MAC address
 * @return Total frame size in bytes (112)
 */
size_t buildSyncPacket(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC);

/**
 * Build pixel data packet
 * @param buffer Output buffer (must be large enough for header + pixel data)
 * @param destMAC Destination MAC address
 * @param srcMAC Source MAC address
 * @param rowIndex Row number (0-based)
 * @param pixelData RGB pixel data (3 bytes per pixel)
 * @param pixelCount Number of pixels in this packet (max COLORLIGHT_MAX_PIXELS_PER_PACKET)
 * @return Total frame size in bytes
 */
size_t buildPixelDataPacket(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC,
                            uint8_t rowIndex, const uint8_t* pixelData, uint16_t pixelCount);

/**
 * Build discovery packet (optional - for auto-detecting receivers)
 * @param buffer Output buffer (must be at least 284 bytes)
 * @param destMAC Destination MAC address (usually broadcast)
 * @param srcMAC Source MAC address
 * @return Total frame size in bytes (284)
 */
size_t buildDiscoveryPacket(uint8_t* buffer, const uint8_t* destMAC, const uint8_t* srcMAC);

/**
 * Calculate number of packets needed for given pixel count
 * @param pixelCount Total number of pixels
 * @return Number of packets required
 */
inline uint16_t calculatePacketCount(uint32_t pixelCount) {
  return (pixelCount + COLORLIGHT_MAX_PIXELS_PER_PACKET - 1) / COLORLIGHT_MAX_PIXELS_PER_PACKET;
}

} // namespace ColorLight
