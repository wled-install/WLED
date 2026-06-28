// usb_host_messages.h
// Shared USB Host app_message_t used by wled.cpp (producer/consumer) and
// midi_usb_host.cpp (producer of MIDI events).
//
// Lives in the usermod_v3_midi directory because the MIDI-specific event
// IDs are added here; the MSC events are kept around so the existing
// background_loop_nonblocking() switch keeps working unchanged.
//
// Gated by SOC_USB_OTG_SUPPORTED (only the P4 USB Host subsystem uses it).
// The underlying msc_host_device_handle_t / usb_host includes are ESP-IDF
// headers, so we only attempt to include them when the target supports it.

#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef SOC_USB_OTG_SUPPORTED

#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"

typedef struct {
  enum {
    APP_QUIT = 0,                  // Signals request to exit the application
    APP_DEVICE_CONNECTED = 1,      // MSC device connect event
    APP_DEVICE_DISCONNECTED = 2,   // MSC device disconnect event
#ifdef USERMOD_MIDI_USB
    APP_MIDI_PACKET = 10,          // Inbound MIDI message from controller (parsed)
    APP_MIDI_DEVICE_CONNECTED = 11,    // Controller enumerated
    APP_MIDI_DEVICE_DISCONNECTED = 12, // Controller gone
#endif
  } id;
  union {
    uint8_t new_dev_address;                  // MSC device address for APP_DEVICE_CONNECTED
    msc_host_device_handle_t device_handle;   // MSC handle for APP_DEVICE_DISCONNECTED
#ifdef USERMOD_MIDI_USB
    struct {
      uint8_t status;        // Channel-stripped command byte (0x80/0x90/0xB0/0xC0/0xE0)
      uint8_t data1;
      uint8_t data2;
    } midi;
    // WLEDMM v3: device descriptor populated by the USB Host client when
    // posting APP_MIDI_DEVICE_CONNECTED. wled.cpp publishes a
    // UsbDeviceChanged v3 event with this payload.
    struct {
      uint16_t vid;
      uint16_t pid;
      char     name[24];
    } midi_device_info;
#endif
  } data;
} app_message_t;

#endif // SOC_USB_OTG_SUPPORTED