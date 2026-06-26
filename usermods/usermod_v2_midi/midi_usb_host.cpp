// midi_usb_host.cpp
// USB-MIDI Host client for the ESP32-P4 EV board.
//
// Pulled in via #include from wled00/wled.cpp under #ifdef USERMOD_MIDI_USB,
// which is also gated by SOC_USB_OTG_SUPPORTED. All definitions are `static`
// so there's no ODR risk.
//
// Targets the pioarduino USB Host stack (Espressif IDF v5 USB Host API).

#include "wled.h"
#include "../usermods/usermod_v2_midi/midi_usb_host.h"
#include "../usermods/usermod_v2_midi/usb_host_messages.h"
#include "../usermods/usermod_v2_midi/usermod_v2_midi.h"

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "freertos/ringbuf.h"

// midiUsermodPtr is declared in wled.h (extern) and defined in usermods_list.cpp.
extern MidiUsermod* midiUsermodPtr;

#define MIDI_LOG(fmt, ...) do { if (canUseSerial()) Serial.printf("[MIDI] " fmt "\n", ##__VA_ARGS__); } while (0)
#define MIDI_DEBUG(fmt, ...) DEBUG_PRINTF("[MIDI] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define NUM_TRANSFERS        4
#define TRANSFER_SIZE        64
#define MIDI_OUT_RING_BYTES  512

// ---------------------------------------------------------------------------
// Known MIDI controllers — used purely to label the device in the boot log
// and /json/info. The descriptor walk doesn't depend on this list, so an
// unknown device still works (just shows "Unknown MIDI controller").
// ---------------------------------------------------------------------------
struct MidiDeviceId {
  uint16_t vid;
  uint16_t pid;
  const char* name;
};

static const MidiDeviceId kMidiDevices[] = {
  // VERIFIED on desk:
  { 0x09E8, 0x004F, "Akai APC Mini mkII (MK2)" },
  { 0x4353, 0x4B4D, "Donner StarryPad Mini" },
  { 0x04D9, 0xE000, "Donner StarryPad" },

  // GUESSED — please verify before relying:
  { 0x09E8, 0x002B, "Akai APC40 mkII (verify)" },
  { 0x09E8, 0x002C, "Akai APC Mini (original, verify)" },
  { 0x09E8, 0x002D, "Akai (unknown — verify)" },
  { 0x09E8, 0x0058, "Akai (unknown — verify)" },
  { 0x0944, 0x0107, "Korg nanoKONTROL2 (verify)" },
  { 0x17CC, 0x0800, "NI Maschine Mikro (verify)" },
  { 0x1C75, 0x0286, "Arturia BeatStep (verify)" },
  { 0x1235, 0x0010, "Novation Launchpad MK2 (verify)" },
  { 0x1234, 0x0001, "Generic class-compliant MIDI" },
};

static const char* midi_device_lookup(uint16_t vid, uint16_t pid) {
  for (const auto& d : kMidiDevices) {
    if (d.vid == vid && d.pid == pid) return d.name;
  }
  switch (vid) {
    case 0x09E8: return "Akai (unknown PID)";
    case 0x4353: return "Donner (unknown PID)";
    case 0x04D9: return "Donner (unknown PID)";
    case 0x0944: return "Korg (unknown PID)";
    case 0x17CC: return "Native Instruments (unknown PID)";
    case 0x1C75: return "Arturia (unknown PID)";
    case 0x1235: return "Novation (unknown PID)";
    case 0x1234: return "Generic MIDI (unknown PID)";
    default:     return "Unknown MIDI controller";
  }
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static usb_host_client_handle_t midi_client  = nullptr;
static usb_device_handle_t     midi_dev_hdl  = nullptr;
static uint8_t                 midi_in_ep    = 0;
static uint8_t                 midi_out_ep   = 0;
static uint8_t                 midi_iface    = 0;
static uint8_t                 midi_cur_addr = 0;
static uint16_t                midi_vendor   = 0;
static uint16_t                midi_product  = 0;

static usb_transfer_t* midi_in_xfer[NUM_TRANSFERS] = {nullptr};
static usb_transfer_t* midi_out_xfer              = nullptr;
static volatile bool   midi_out_in_flight        = false;

static uint8_t          out_rb_storage[MIDI_OUT_RING_BYTES] __attribute__((aligned(4)));
static StaticRingbuffer_t out_rb_struct;
static RingbufHandle_t    midi_out_rb = nullptr;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void midi_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void midi_in_transfer_cb(usb_transfer_t* xfer);
static void midi_out_transfer_cb(usb_transfer_t* xfer);
static esp_err_t midi_open_and_configure(uint8_t dev_addr);
static void      midi_close_device(void);

// ---------------------------------------------------------------------------
// midi_usb_init — called once from usb_task after msc_host_install()
// ---------------------------------------------------------------------------
void midi_usb_init(void) {
  if (midi_client) return;

  midi_out_rb = xRingbufferCreateStatic(
      MIDI_OUT_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
      out_rb_storage, &out_rb_struct);
  if (!midi_out_rb) {
    MIDI_LOG("init: OUT ring buffer alloc failed");
    return;
  }

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous           = false;
  cfg.max_num_event_msg        = 5;
  cfg.async.client_event_callback = midi_client_event_cb;
  cfg.async.callback_arg          = nullptr;

  esp_err_t err = usb_host_client_register(&cfg, &midi_client);
  if (err != ESP_OK) {
    MIDI_LOG("init: usb_host_client_register failed: %s", esp_err_to_name(err));
    midi_client = nullptr;
    return;
  }

  for (int i = 0; i < NUM_TRANSFERS; i++) {
    err = usb_host_transfer_alloc(TRANSFER_SIZE, 0, &midi_in_xfer[i]);
    if (err != ESP_OK) {
      MIDI_LOG("init: usb_host_transfer_alloc[%d] failed: %s", i, esp_err_to_name(err));
      midi_in_xfer[i] = nullptr;
      continue;
    }
    midi_in_xfer[i]->context       = nullptr;
    midi_in_xfer[i]->device_handle = nullptr;
    midi_in_xfer[i]->callback      = midi_in_transfer_cb;
    midi_in_xfer[i]->num_bytes     = TRANSFER_SIZE;
  }

  err = usb_host_transfer_alloc(TRANSFER_SIZE, 0, &midi_out_xfer);
  if (err != ESP_OK) {
    MIDI_LOG("init: usb_host_transfer_alloc OUT failed: %s", esp_err_to_name(err));
    midi_out_xfer = nullptr;
  } else {
    midi_out_xfer->callback = midi_out_transfer_cb;
    midi_out_xfer->context  = nullptr;
  }

  MIDI_LOG("init: USB Host client ready");
}

// ---------------------------------------------------------------------------
// midi_usb_poll — called each iteration of usb_task
// ---------------------------------------------------------------------------
void midi_usb_poll(void) {
  if (!midi_client) return;

  // Drain client events (transfers complete, new-device events, etc.)
  usb_host_client_handle_events(midi_client, 0);

  // Drain queued OUT packets (at most one in flight at a time).
  if (midi_dev_hdl && midi_out_ep && midi_out_xfer && !midi_out_in_flight && midi_out_rb) {
    size_t got = 0;
    const uint8_t* data = (const uint8_t*)xRingbufferReceiveUpTo(
        midi_out_rb, &got, 0, TRANSFER_SIZE);
    if (data) {
      if (got >= 4) {
        memcpy(midi_out_xfer->data_buffer, data, got);
        vRingbufferReturnItem(midi_out_rb, (void*)data);
        midi_out_xfer->num_bytes       = got;
        midi_out_xfer->device_handle   = midi_dev_hdl;
        midi_out_xfer->bEndpointAddress = midi_out_ep;
        midi_out_in_flight = true;
        esp_err_t err = usb_host_transfer_submit(midi_out_xfer);
        if (err != ESP_OK) {
          midi_out_in_flight = false;
          MIDI_DEBUG("OUT submit failed: %s", esp_err_to_name(err));
        }
      } else {
        vRingbufferReturnItem(midi_out_rb, (void*)data);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// midi_out_queue — safe to call from any task
// ---------------------------------------------------------------------------
bool midi_out_queue(uint8_t status, uint8_t d1, uint8_t d2) {
  if (!midi_out_rb) return false;
  uint8_t cin = (status & 0xF0) >> 4;
  uint8_t pkt[4] = { (uint8_t)(cin & 0x0F), status, d1, d2 };
  return xRingbufferSend(midi_out_rb, pkt, 4, 0) == pdTRUE;
}

// ---------------------------------------------------------------------------
// Client event callback
// ---------------------------------------------------------------------------
static void midi_client_event_cb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
  if (!msg) return;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    MIDI_LOG("event: NEW_DEV addr=%d", msg->new_dev.address);
    if (midi_open_and_configure(msg->new_dev.address) == ESP_OK) {
      if (midiUsermodPtr) {
        midiUsermodPtr->setDeviceInfo(midi_vendor, midi_product,
                                      midi_device_lookup(midi_vendor, midi_product));
      }
      app_message_t m = {};
      m.id = app_message_t::APP_MIDI_DEVICE_CONNECTED;
      xQueueSend(app_queue, &m, 0);
    } else {
      MIDI_LOG("event: NEW_DEV addr=%d configure failed", msg->new_dev.address);
    }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    MIDI_LOG("event: DEV_GONE");
    midi_close_device();
    app_message_t m = {};
    m.id = app_message_t::APP_MIDI_DEVICE_DISCONNECTED;
    xQueueSend(app_queue, &m, 0);
  }
}

// ---------------------------------------------------------------------------
// IN transfer callback — parse 4-byte USB-MIDI event packets, then resubmit
// ---------------------------------------------------------------------------
static void midi_in_transfer_cb(usb_transfer_t* xfer) {
  if (!xfer) return;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    const uint8_t* p = xfer->data_buffer;
    int n = xfer->actual_num_bytes;
    for (int i = 0; i + 4 <= n; i += 4) {
      uint8_t status = p[i + 1];
      uint8_t d1     = p[i + 2];
      uint8_t d2     = p[i + 3];
      uint8_t cin = p[i] & 0x0F;
      // 0x8..0xE are normal channel messages (NoteOff..PitchBend).
      // SysEx (0x4..0x7) and 0x0..0x3 / 0xF are not forwarded.
      if (cin < 0x8 || cin > 0xE) continue;
      app_message_t m = {};
      m.id = app_message_t::APP_MIDI_PACKET;
      m.data.midi.status = (uint8_t)(status & 0xF0);
      m.data.midi.data1  = d1;
      m.data.midi.data2  = d2;
      xQueueSend(app_queue, &m, 0);
    }
  }

  // Eagerly re-submit so we keep the IN pipe saturated.
  if (xfer->device_handle) {
    xfer->num_bytes = TRANSFER_SIZE;
    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
      MIDI_DEBUG("IN re-submit failed: %s", esp_err_to_name(err));
    }
  }
}

// ---------------------------------------------------------------------------
// OUT transfer callback — release the "in flight" flag
// ---------------------------------------------------------------------------
static void midi_out_transfer_cb(usb_transfer_t* xfer) {
  (void)xfer;
  midi_out_in_flight = false;
}

// ---------------------------------------------------------------------------
// Descriptor walking — Audio Class 1 / MIDI Streaming bulk IN+OUT, with a
// vendor-class fallback that picks the first bulk pair on interface 0.
// ---------------------------------------------------------------------------
static esp_err_t midi_open_and_configure(uint8_t dev_addr) {
  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(midi_client, dev_addr, &dev);
  if (err != ESP_OK) {
    MIDI_LOG("configure: device_open(addr=%d) failed: %s", dev_addr, esp_err_to_name(err));
    return err;
  }

  const usb_device_desc_t* dev_desc = nullptr;
  err = usb_host_get_device_descriptor(dev, &dev_desc);
  if (err != ESP_OK) {
    MIDI_LOG("configure: get_device_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return err;
  }
  midi_vendor  = dev_desc->idVendor;
  midi_product = dev_desc->idProduct;
  MIDI_LOG("configure: VID=0x%04X PID=0x%04X (%s)",
           midi_vendor, midi_product,
           midi_device_lookup(midi_vendor, midi_product));

  const usb_config_desc_t* cfg_desc = nullptr;
  err = usb_host_get_active_config_descriptor(dev, &cfg_desc);
  if (err != ESP_OK || !cfg_desc) {
    MIDI_LOG("configure: get_active_config_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }

  int  claimed_iface = -1;
  uint8_t in_ep = 0, out_ep = 0;
  for (uint8_t iface_num = 0; iface_num < 8; iface_num++) {
    int iface_off = 0;
    const usb_intf_desc_t* iface = usb_parse_interface_descriptor(cfg_desc, iface_num, 0, &iface_off);
    if (!iface) break;
    if (iface->bInterfaceClass == 0x01 && iface->bInterfaceSubClass == 0x03) {
      claimed_iface = iface_num;
      for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
        int ep_off = iface_off;
        const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
            iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
        if (!ep) continue;
        if ((ep->bmAttributes & 0x03) != 2) continue;  // not bulk
        if      ((ep->bEndpointAddress & 0x80) && !in_ep)  in_ep  = ep->bEndpointAddress;
        else if (!(ep->bEndpointAddress & 0x80) && !out_ep) out_ep = ep->bEndpointAddress;
      }
      break;
    }
  }

  // Vendor-class fallback: scan interface 0 for a bulk pair.
  if (claimed_iface < 0 || in_ep == 0) {
    int iface_off = 0;
    const usb_intf_desc_t* iface = usb_parse_interface_descriptor(cfg_desc, 0, 0, &iface_off);
    if (iface) {
      claimed_iface = 0;
      for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
        int ep_off = iface_off;
        const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
            iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
        if (!ep) continue;
        if ((ep->bmAttributes & 0x03) != 2) continue;
        if      ((ep->bEndpointAddress & 0x80) && !in_ep)  in_ep  = ep->bEndpointAddress;
        else if (!(ep->bEndpointAddress & 0x80) && !out_ep) out_ep = ep->bEndpointAddress;
      }
    }
  }

  if (claimed_iface < 0 || in_ep == 0) {
    MIDI_LOG("configure: no usable MIDI endpoints");
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }

  err = usb_host_interface_claim(midi_client, dev, (uint8_t)claimed_iface, 0);
  if (err != ESP_OK) {
    MIDI_LOG("configure: interface_claim(iface=%d) failed: %s", claimed_iface, esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return err;
  }

  midi_dev_hdl   = dev;
  midi_in_ep     = in_ep;
  midi_out_ep    = out_ep;
  midi_iface     = (uint8_t)claimed_iface;
  midi_cur_addr  = dev_addr;

  MIDI_LOG("configure: CLAIMED iface=%d IN=0x%02X OUT=0x%02X",
           claimed_iface, in_ep, out_ep);

  for (int i = 0; i < NUM_TRANSFERS; i++) {
    usb_transfer_t* x = midi_in_xfer[i];
    if (!x) continue;
    x->device_handle     = dev;
    x->bEndpointAddress  = in_ep;
    x->num_bytes         = TRANSFER_SIZE;
    err = usb_host_transfer_submit(x);
    if (err != ESP_OK) {
      MIDI_LOG("configure: initial IN submit[%d] failed: %s", i, esp_err_to_name(err));
    }
  }
  MIDI_LOG("configure: %d IN transfers submitted", NUM_TRANSFERS);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Close device
// ---------------------------------------------------------------------------
static void midi_close_device(void) {
  if (!midi_dev_hdl) return;
  usb_host_interface_release(midi_client, midi_dev_hdl, midi_iface);
  for (int i = 0; i < NUM_TRANSFERS; i++) {
    usb_transfer_t* x = midi_in_xfer[i];
    if (x && x->device_handle == midi_dev_hdl) x->device_handle = nullptr;
  }
  if (midi_out_xfer) midi_out_xfer->device_handle = nullptr;
  midi_out_in_flight = false;
  usb_host_device_close(midi_client, midi_dev_hdl);
  midi_dev_hdl  = nullptr;
  midi_in_ep    = 0;
  midi_out_ep   = 0;
  midi_iface    = 0;
  midi_cur_addr = 0;
  midi_vendor   = 0;
  midi_product  = 0;
}