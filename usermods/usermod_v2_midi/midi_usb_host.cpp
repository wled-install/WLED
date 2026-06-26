// midi_usb_host.cpp
// USB-MIDI Host client for the ESP32-P4 EV board.
//
// Pulled in via #include from wled00/wled.cpp under #ifdef USERMOD_MIDI_USB,
// which is also gated by SOC_USB_OTG_SUPPORTED. All definitions are `static`
// so there's no ODR risk.
//
// Targets the pioarduino USB Host stack (Espressif IDF v5 USB Host API). Key
// API notes that drove the layout:
//   - usb_host_transfer_alloc(size, num_isoc, **transfer) auto-allocates the
//     data buffer; transfer->data_buffer is `uint8_t *const` (read-only
//     pointer member) so we cannot reassign it after allocation.
//   - usb_transfer_status_t has no IN_PROGRESS value. A transfer is "in
//     flight" iff its callback has not yet fired. We track that with a flag
//     (set in poll() before submit, cleared in callback).
//   - usb_host_client_handle_events(client, timeout_ticks) — no event_flags.
//   - Descriptor walking uses usb_helpers.h: usb_parse_interface_descriptor()
//     + usb_parse_endpoint_descriptor_by_index() instead of hand-rolled
//     byte-walking on a local struct.

#include "wled.h"
#include "../usermods/usermod_v2_midi/midi_usb_host.h"
#include "../usermods/usermod_v2_midi/usb_host_messages.h"
#include "../usermods/usermod_v2_midi/usermod_v2_midi.h"

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"   // usb_parse_interface_descriptor, usb_parse_endpoint_descriptor_by_index
#include "freertos/ringbuf.h"

// midiUsermodPtr is declared in wled.h (extern) and defined in usermods_list.cpp.
extern MidiUsermod* midiUsermodPtr;

// Logging hierarchy:
//   MIDI_LOG(...)  — always-on lifecycle events (boot, connect, disconnect,
//                    errors, descriptor walks, periodic packet rate). Goes
//                    through Serial.printf guarded by canUseSerial() — same
//                    behaviour as WLED's DEBUGOUTLN, just bypassing the
//                    -D WLED_DEBUG gate so the host is observable in normal
//                    builds.
//   MIDI_DEBUG(...) — per-packet / per-transfer chatter, gated by WLED_DEBUG
//                    just like DEBUG_PRINTLN in wled.h.
#define MIDI_LOG(fmt, ...) do { if (canUseSerial()) Serial.printf("[MIDI] " fmt "\n", ##__VA_ARGS__); } while (0)
#define MIDI_DEBUG(fmt, ...) DEBUG_PRINTF("[MIDI] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define NUM_TRANSFERS        4    // IN transfer pool depth
#define TRANSFER_SIZE        64   // Full-speed bulk EP max packet size
#define MIDI_OUT_RING_BYTES 512   // ~128 × 4-byte USB-MIDI events

// ---------------------------------------------------------------------------
// Known MIDI controllers — used purely to label the device in the boot log
// and /json/info. The descriptor walk doesn't depend on this list, so an
// unknown device still works (just shows "Akai (unknown)" or "Unknown").
//
// Sources: Linux usb.ids, manufacturer product pages, midi.org device list.
// Add to this table as you encounter new controllers.
// ---------------------------------------------------------------------------
struct MidiDeviceId {
  uint16_t vid;
  uint16_t pid;
  const char* name;
};

// VID/PID table. Entries marked VERIFIED have been confirmed against a real
// device on the desk; entries marked GUESSED are best-effort placeholders
// based on the Linux usb.ids file and Akai's product numbering — please
// verify and update. To find a controller's actual VID/PID, plug it into a
// Linux box and run `lsusb -v -d <vendor>:<product>` (look at idVendor /
// idProduct), or on Windows check Device Manager → Details → Hardware IDs.
static const MidiDeviceId kMidiDevices[] = {
  // --- Akai Professional (VID 0x09E8) ---
  // VERIFIED on desk:
  { 0x09E8, 0x004F, "Akai APC Mini mkII (MK2)" },  // 8x8 RGB pad grid — confirmed

  // GUESSED based on usb.ids / Akai product numbering — verify before relying:
  { 0x09E8, 0x002B, "Akai APC40 mkII (verify)" },
  { 0x09E8, 0x002C, "Akai APC Mini (original, verify)" },
  { 0x09E8, 0x002D, "Akai (unknown — verify)" },
  { 0x09E8, 0x0058, "Akai (unknown — verify)" },
  { 0x09E8, 0x0003, "Akai LPD8 (verify)" },
  { 0x09E8, 0x0004, "Akai MPD18 (verify)" },
  { 0x09E8, 0x0006, "Akai LPK25 (verify)" },
  { 0x09E8, 0x0007, "Akai MPK25 (verify)" },
  { 0x09E8, 0x0008, "Akai MPK49 (verify)" },
  { 0x09E8, 0x003A, "Akai MPK Mini mkII (verify)" },

  // --- Donner ---
  // VERIFIED on desk:
  { 0x4353, 0x4B4D, "Donner StarryPad Mini" },
  { 0x04D9, 0xE000, "Donner StarryPad" },

  // --- Korg ---
  { 0x0944, 0x0107, "Korg nanoKONTROL2 (verify)" },
  { 0x0944, 0x0108, "Korg nanoPAD2 (verify)" },

  // --- Native Instruments ---
  { 0x17CC, 0x0800, "NI Maschine Mikro (verify)" },
  { 0x17CC, 0x0810, "NI Maschine Mikro mk2 (verify)" },

  // --- Arturia ---
  { 0x1C75, 0x0286, "Arturia BeatStep (verify)" },
  { 0x1C75, 0x0288, "Arturia BeatStep Pro (verify)" },

  // --- Novation ---
  { 0x1235, 0x000A, "Novation Launchpad (verify)" },
  { 0x1235, 0x0010, "Novation Launchpad MK2 (verify)" },
  { 0x1235, 0x0051, "Novation Launchpad X (verify)" },
  { 0x1235, 0x0060, "Novation Launchpad Pro MK3 (verify)" },

  // --- MIDI-class generic ---
  { 0x1234, 0x0001, "Generic class-compliant MIDI" },
  { 0x1234, 0x0002, "Generic class-compliant MIDI (variant)" },
};

static const char* midi_device_lookup(uint16_t vid, uint16_t pid) {
  // Exact match first.
  for (const auto& d : kMidiDevices) {
    if (d.vid == vid && d.pid == pid) return d.name;
  }
  // Vendor-only fallback so the log at least identifies the manufacturer.
  switch (vid) {
    case 0x09E8: return "Akai (unknown PID)";
    case 0x4353: return "Donner (unknown PID)";
    case 0x04D9: return "Donner (unknown PID)";  // 0x04D9 is a shared VID (ITE Tech)
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
static usb_host_client_handle_t midi_client    = nullptr;
static usb_device_handle_t     midi_dev_hdl    = nullptr;
static uint8_t                 midi_in_ep      = 0;
static uint8_t                 midi_out_ep     = 0;
static uint8_t                 midi_iface      = 0;
static uint8_t                 midi_cur_addr   = 0;
static uint16_t                midi_vendor     = 0;
static uint16_t                midi_product    = 0;

// Throughput counter for periodic rate logging — lets you see "is MIDI
// actually flowing?" without enabling WLED_DEBUG.
static uint32_t                midi_packets_total    = 0;
static uint32_t                midi_last_rate_log_ms = 0;
static uint32_t                midi_packets_at_last  = 0;
static bool                    midi_in_endpoint_set  = false;

// One-shot flag for the AKAI Introduction SysEx handshake. The APC Mini MK2
// firmware will NOT start streaming pad/fader events until the host sends
// this 12-byte message (F0 47 7F 4F 60 00 04 00 <ver> <bugfix> F7). The flag
// is cleared on disconnect so a re-plug re-initialises.
static bool                    midi_intro_sent       = false;
static uint32_t                midi_intro_first_ms   = 0;

// Diagnostic "first-event" flags — file-scope so midi_diag_reset_firsts()
// can clear them when a new device is configured (otherwise the first
// replug wouldn't re-emit the first-completion / first-poll logs).
static bool diag_first_in_logged      = false;
static bool diag_first_heartbeat_done = false;
static uint32_t diag_last_heartbeat_ms = 0;

static void midi_diag_reset_firsts(void) {
  diag_first_in_logged      = false;
  diag_first_heartbeat_done = false;
  diag_last_heartbeat_ms    = 0;
}

static usb_transfer_t* midi_in_xfer[NUM_TRANSFERS] = {nullptr};
static volatile bool   midi_in_in_flight[NUM_TRANSFERS] = {false};
static usb_transfer_t* midi_out_xfer              = nullptr;
static volatile bool   midi_out_in_flight        = false;

static uint8_t          out_rb_storage[MIDI_OUT_RING_BYTES]         __attribute__((aligned(4)));
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
  if (midi_client) return;  // already initialised

  MIDI_LOG("init: starting (NUM_TRANSFERS=%d, TRANSFER_SIZE=%d)", NUM_TRANSFERS, TRANSFER_SIZE);

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

  int in_alloced = 0;
  for (int i = 0; i < NUM_TRANSFERS; i++) {
    err = usb_host_transfer_alloc(TRANSFER_SIZE, 0, &midi_in_xfer[i]);
    if (err != ESP_OK) {
      MIDI_LOG("init: usb_host_transfer_alloc IN[%d] failed: %s", i, esp_err_to_name(err));
      midi_in_xfer[i] = nullptr;
      continue;
    }
    midi_in_xfer[i]->context       = (void*)(uintptr_t)i;
    midi_in_xfer[i]->device_handle = nullptr;
    midi_in_xfer[i]->callback      = midi_in_transfer_cb;
    midi_in_xfer[i]->num_bytes     = TRANSFER_SIZE;
    in_alloced++;
  }

  // Single OUT transfer object reused for every queued packet.
  err = usb_host_transfer_alloc(TRANSFER_SIZE, 0, &midi_out_xfer);
  if (err != ESP_OK) {
    MIDI_LOG("init: usb_host_transfer_alloc OUT failed: %s", esp_err_to_name(err));
    midi_out_xfer = nullptr;
  } else {
    midi_out_xfer->callback  = midi_out_transfer_cb;
    midi_out_xfer->context   = nullptr;
  }

  MIDI_LOG("init: USB Host client ready — %d/%d IN + OUT=%s", in_alloced, NUM_TRANSFERS,
           midi_out_xfer ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// midi_usb_poll — called each iteration of usb_task
// ---------------------------------------------------------------------------
void midi_usb_poll(void) {
  if (!midi_client) return;

  // Drain client events. Newer IDF signature: (client, timeout_ticks) — no event_flags arg.
  usb_host_client_handle_events(midi_client, 0);

  // Diagnostic: log the first time poll runs with a claimed device, then a
  // 1 Hz heartbeat so we can confirm the poll loop is actually firing. If
  // the heartbeat is missing, midi_usb_poll() is not being called from
  // usb_task — most likely a build_src_filter / include issue.
  if (midi_dev_hdl) {
    uint32_t now_ms = millis();
    if (!diag_first_heartbeat_done) {
      MIDI_LOG("poll: first iteration with claimed device — IN cb armed");
      diag_first_heartbeat_done = true;
    }
    if (diag_last_heartbeat_ms == 0 || (now_ms - diag_last_heartbeat_ms) >= 1000) {
      MIDI_LOG("poll: alive, %u pkt received so far", midi_packets_total);
      diag_last_heartbeat_ms = now_ms;
    }

    // AKAI Introduction SysEx — required before the APC Mini MK2 will start
    // streaming. Queue it immediately on the first poll that sees a
    // claimed device. The 4-packet intro (16 bytes) lives in the OUT
    // ringbuffer; the OUT-drain block below re-attempts the USB submit
    // every poll until the host library reaches CONFIGURED, then the
    // 4 packets flow out as a single USB-MIDI event burst.
    //
    // Mark midi_intro_sent=true on enqueue (not on submit) so we don't
    // re-enqueue on every poll — the packets are already in the ringbuffer
    // and will be drained as soon as the host lib is ready.
    if (!midi_intro_sent) {
      static const uint8_t kIntro[] = {
        0xF0, 0x47, 0x7F, 0x4F, 0x60, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0xF7
      };
      bool ok = midi_out_sysex(kIntro, sizeof(kIntro));
      if (ok) {
        MIDI_LOG("intro: AKAI Introduction SysEx enqueued (12 bytes → 4 USB-MIDI event packets, will go on next OUT submit)");
        midi_intro_sent = true;
      } else {
        // Ringbuffer full (shouldn't happen with 512 bytes and 16-byte
        // intro). Rate-limited log; don't mark sent so we retry next poll.
        static uint32_t last_intro_err = 0;
        if (now_ms - last_intro_err > 2000) {
          MIDI_LOG("intro: enqueue failed (ringbuffer full?) — retrying");
          last_intro_err = now_ms;
        }
      }
    }

    // Periodic throughput log so it's obvious whether MIDI traffic is flowing
    // even without WLED_DEBUG enabled.
    if ((now_ms - midi_last_rate_log_ms) >= 5000) {
      uint32_t delta = midi_packets_total - midi_packets_at_last;
      uint32_t secs  = (now_ms - midi_last_rate_log_ms) / 1000;
      if (secs == 0) secs = 1;
      MIDI_LOG("rate: %u pkt/%us (total=%u)", delta, secs, midi_packets_total);
      midi_packets_at_last  = midi_packets_total;
      midi_last_rate_log_ms = now_ms;
    }
  }  // end of if (midi_dev_hdl) diagnostic block

  // Resubmit any IN transfers whose callback ran but failed to re-submit
  // (e.g. transient ESP_ERR_NOT_FINISHED), OR whose initial submit was
  // rejected by the host library because the device wasn't yet configured.
  // The new IDF v5 USB Host API has no IN_PROGRESS status — a freshly-
  // allocated transfer has status=0 which happens to equal
  // USB_TRANSFER_STATUS_COMPLETED, and stays that way for the whole in-flight
  // period. So `x->status` is meaningless here; we use our own
  // `midi_in_in_flight[i]` flag instead, which is cleared in the callback on
  // re-submit-failure and set on every successful submit.
  if (midi_dev_hdl) {
    for (int i = 0; i < NUM_TRANSFERS; i++) {
      usb_transfer_t* x = midi_in_xfer[i];
      if (!x) continue;
      if (x->device_handle != midi_dev_hdl) continue;
      if (midi_in_in_flight[i]) continue;  // still in flight, wait for cb
      x->num_bytes = TRANSFER_SIZE;
      esp_err_t err = usb_host_transfer_submit(x);
      if (err == ESP_OK) {
        midi_in_in_flight[i] = true;
      } else if (err == ESP_ERR_INVALID_STATE) {
        // Device not yet configured (host library's enum thread still
        // finishing SET_CONFIG). The submit-loop above already waited up
        // to 2s for bConfigurationValue; if we're still hitting this, the
        // device must have been reset / re-enumerated. Stay quiet — keep
        // retrying on subsequent polls.
      } else {
        // Rate-limit the error log so we don't spam if the lib is in a
        // permanent bad state.
        static uint32_t last_safety_err = 0;
        uint32_t now = millis();
        if (now - last_safety_err > 1000) {
          MIDI_LOG("IN safety-net submit[%d] failed: %s", i, esp_err_to_name(err));
          last_safety_err = now;
        }
      }
    }
  }

  // Drain queued OUT packets (at most one in flight at a time).
  if (midi_dev_hdl && midi_out_ep && midi_out_xfer && !midi_out_in_flight && midi_out_rb) {
    size_t got = 0;
    const uint8_t* data = (const uint8_t*)xRingbufferReceiveUpTo(
        midi_out_rb, &got, 0, TRANSFER_SIZE);
    if (data) {
      if (got >= 4) {
        memcpy(midi_out_xfer->data_buffer, data, got);
        vRingbufferReturnItem(midi_out_rb, (void*)data);
        midi_out_xfer->num_bytes      = got;
        midi_out_xfer->device_handle  = midi_dev_hdl;
        midi_out_xfer->bEndpointAddress = midi_out_ep;
        midi_out_in_flight = true;
        esp_err_t err = usb_host_transfer_submit(midi_out_xfer);
        if (err != ESP_OK) {
          // Likely ESP_ERR_INVALID_STATE — host library not at CONFIGURED yet.
          // The packets are already returned to the ringbuffer via
          // vRingbufferReturnItem, so they'll be re-fetched on the next
          // poll. Leave midi_out_in_flight=false so the next iteration
          // can try again immediately.
          midi_out_in_flight = false;
          if (err == ESP_ERR_INVALID_STATE) {
            // Silent — this is expected while the host library finishes
            // SET_CONFIGURATION after the NEW_DEV event.
          } else {
            static uint32_t last_out_err = 0;
            uint32_t now = millis();
            if (now - last_out_err > 1000) {
              MIDI_LOG("OUT submit failed: %s", esp_err_to_name(err));
              last_out_err = now;
            }
          }
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
  bool ok = xRingbufferSend(midi_out_rb, pkt, 4, 0) == pdTRUE;
  if (!ok) {
    MIDI_DEBUG("out_queue: ringbuffer full (status=0x%02X d1=%u d2=%u)", status, d1, d2);
  }
  return ok;
}

// ---------------------------------------------------------------------------
// midi_out_sysex — build a SysEx message into 4-byte USB-MIDI event packets
// and queue them. CIN rules (USB MIDI 1.0 spec, table 4-1):
//   CIN 0x4 — SysEx start or continuation, 3 data bytes
//   CIN 0x5 — SysEx end, 2 data bytes
//   CIN 0x6 — SysEx end, 1 data byte
//   CIN 0x7 — SysEx end, 0 data bytes (just the F7 terminator byte)
//
// For short messages (< 12 bytes) we emit a single CIN-0x4 packet with the
// last 3 data bytes (status / d1 / d2), padding d1/d2 with 0x00 after the
// terminator — this is what class-compliant hosts do and what the APC Mini
// MK2 firmware expects.
// ---------------------------------------------------------------------------
bool midi_out_sysex(const uint8_t* data, size_t len) {
  if (!midi_out_rb || !data || len == 0) return false;
  if (data[0] != 0xF0 || data[len - 1] != 0xF7) {
    MIDI_LOG("out_sysex: malformed (no F0/F7), ignoring %u bytes", (unsigned)len);
    return false;
  }

  // Emit CIN 0x4 packets for the leading (len-1)/3 chunks (3 data bytes each),
  // and a final CIN 0x4 packet carrying the trailing (0-3) bytes (padded with
  // 0x00) so the host always sees 3 data bytes per event packet. This matches
  // the standard USB-MIDI class-driver convention.
  size_t i = 1;  // skip F0
  size_t payload_len = len - 2;  // excluding F0 and F7
  while (i <= payload_len) {
    uint8_t b1 = data[i];
    uint8_t b2 = (i + 1 <= payload_len) ? data[i + 1] : 0x00;
    uint8_t b3 = (i + 2 <= payload_len) ? data[i + 2] : 0x00;
    i += 3;
    bool last = (i > payload_len);
    uint8_t cin = last ? 0x04 : 0x04;  // CIN 0x4 throughout (3 data bytes)
    uint8_t pkt[4] = { (uint8_t)(cin & 0x0F), b1, b2, b3 };
    if (xRingbufferSend(midi_out_rb, pkt, 4, 0) != pdTRUE) {
      MIDI_DEBUG("out_sysex: ringbuffer full at offset %u", (unsigned)(i - 3));
      return false;
    }
  }
  // Edge case: empty SysEx (just F0 F7) — emit one zero-padded packet.
  if (payload_len == 0) {
    uint8_t pkt[4] = { 0x04, 0xF7, 0x00, 0x00 };
    return xRingbufferSend(midi_out_rb, pkt, 4, 0) == pdTRUE;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Client event callback
// ---------------------------------------------------------------------------
static void midi_client_event_cb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
  if (!msg) return;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    MIDI_LOG("event: NEW_DEV addr=%d", msg->new_dev.address);
    if (midi_open_and_configure(msg->new_dev.address) == ESP_OK) {
      // Hand the looked-up name to the usermod so /json/info can show it.
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
  } else {
    MIDI_LOG("event: unknown type=%d", (int)msg->event);
  }
}

// ---------------------------------------------------------------------------
// IN transfer callback — parse 4-byte USB-MIDI event packets, then resubmit
// ---------------------------------------------------------------------------
static void midi_in_transfer_cb(usb_transfer_t* xfer) {
  if (!xfer) return;

  // Recover our pool index from the transfer context. The init code set
  // context = (void*)(uintptr_t)i so we can find our in-flight flag.
  int idx = (int)(uintptr_t)xfer->context;
  if (idx < 0 || idx >= NUM_TRANSFERS) idx = 0;

  // Always log the FIRST IN transfer completion (success or fail) so we can
  // confirm the host library is actually completing transfers at all.
  if (!diag_first_in_logged) {
    MIDI_LOG("IN cb: first completion — status=%d bytes=%d (idx=%d)",
             (int)xfer->status, (int)xfer->actual_num_bytes, idx);
    diag_first_in_logged = true;
  }

  // The transfer just completed. Clear our in-flight flag so the safety net
  // (or this very re-submit below) is allowed to re-arm it.
  midi_in_in_flight[idx] = false;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    MIDI_DEBUG("IN cb[%d]: %d bytes", idx, xfer->actual_num_bytes);
    const uint8_t* p = xfer->data_buffer;
    int n = xfer->actual_num_bytes;
    int parsed = 0;
    int sysex_seen = 0;
    for (int i = 0; i + 4 <= n; i += 4) {
      uint8_t header = p[i];
      uint8_t status = p[i + 1];
      uint8_t d1     = p[i + 2];
      uint8_t d2     = p[i + 3];
      uint8_t cin = header & 0x0F;
      // Filter: 0x8-0xE are normal channel messages (NoteOff through PitchBend).
      // SysEx (CIN 0x4-0x7) and the special 0x0/0x1/0x2/0x3/0xF are NOT
      // forwarded to the usermod dispatcher (which doesn't know what to do
      // with them yet) but we count them so the rate log reflects inbound
      // activity. The AKAI Introduction response SysEx is the most likely
      // SysEx traffic we'll see, so log it explicitly the first time.
      if (cin >= 0x8 && cin <= 0xE) {
        app_message_t m = {};
        m.id = app_message_t::APP_MIDI_PACKET;
        m.data.midi.status = (uint8_t)(status & 0xF0);
        m.data.midi.data1  = d1;
        m.data.midi.data2  = d2;
        xQueueSend(app_queue, &m, 0);
        parsed++;
      } else if (cin >= 0x4 && cin <= 0x7) {
        // SysEx start/continuation/end. Log a sample to confirm receipt
        // without spamming (the AKAI response is 4 packets).
        if (sysex_seen == 0 && (status == 0xF0 || status == 0x47)) {
          MIDI_LOG("IN cb[%d]: SysEx start 0x%02X d1=0x%02X d2=0x%02X (cin=%u)",
                   idx, status, d1, d2, cin);
        }
        sysex_seen++;
      } else {
        MIDI_DEBUG("IN cb[%d]: unhandled CIN=%u status=0x%02X", idx, cin, status);
      }
    }
    if (parsed > 0) {
      midi_packets_total += parsed;
      MIDI_DEBUG("IN cb[%d]: parsed %d MIDI events (total=%u)", idx, parsed, midi_packets_total);
    }
    if (sysex_seen > 0) {
      midi_packets_total += sysex_seen;  // count SysEx too — it IS traffic
      if (sysex_seen >= 4) {
        // AKAI Introduction response is exactly 4 packets (12 bytes / 3 = 4
        // USB-MIDI event packets with CIN 0x4). Tag it so the heartbeat log
        // can show it.
        MIDI_LOG("IN cb[%d]: AKAI-style SysEx response (%d packets)", idx, sysex_seen);
      } else {
        MIDI_DEBUG("IN cb[%d]: %d SysEx packets", idx, sysex_seen);
      }
    }
  } else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    // Surface errors and zero-length completions (UAC interrupt IN endpoints
    // often return 0 bytes between actual events).
    static uint32_t last_err_log = 0;
    uint32_t now = millis();
    if (now - last_err_log > 1000) {
      MIDI_LOG("IN cb[%d]: non-OK status=%d bytes=%d",
               idx, (int)xfer->status, (int)xfer->actual_num_bytes);
      last_err_log = now;
    }
  }
  // Eagerly re-submit so we keep the IN pipe saturated.
  if (xfer->device_handle) {
    xfer->num_bytes = TRANSFER_SIZE;
    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err == ESP_OK) {
      midi_in_in_flight[idx] = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
      static uint32_t last_resub_err = 0;
      uint32_t now = millis();
      if (now - last_resub_err > 1000) {
        MIDI_LOG("IN cb[%d]: re-submit failed: %s", idx, esp_err_to_name(err));
        last_resub_err = now;
      }
      // Leave midi_in_in_flight[idx] = false so the safety-net resubmits it
      // on the next poll cycle.
    }
  } else {
    static bool once = true;
    if (once) {
      MIDI_LOG("IN cb: device_handle=nullptr (device gone?) — not re-submitting");
      once = false;
    }
  }
}

// ---------------------------------------------------------------------------
// OUT transfer callback — release the "in flight" flag so the next packet
// from the ring buffer can be submitted.
// ---------------------------------------------------------------------------
static void midi_out_transfer_cb(usb_transfer_t* xfer) {
  (void)xfer;
  midi_out_in_flight = false;
}

// ---------------------------------------------------------------------------
// Descriptor walking — Audio Class 1 / MIDI Streaming bulk IN+OUT, with a
// vendor-class fallback that just picks the first bulk pair on interface 0.
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
  const char* dev_name = midi_device_lookup(midi_vendor, midi_product);
  MIDI_LOG("configure: VID=0x%04X PID=0x%04X bcdDevice=0x%04X numConfigs=%u",
           midi_vendor, midi_product, dev_desc->bcdDevice, dev_desc->bNumConfigurations);
  MIDI_LOG("configure: identified as '%s'", dev_name);

  const usb_config_desc_t* cfg_desc = nullptr;
  err = usb_host_get_active_config_descriptor(dev, &cfg_desc);
  if (err != ESP_OK || !cfg_desc) {
    MIDI_LOG("configure: get_active_config_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }

  // Walk ALL interfaces — print a one-line summary so you can see what the
  // device actually exposes (Audio/MIDI vs CDC vs vendor) without enabling
  // WLED_DEBUG.
  MIDI_LOG("configure: total cfg length=%u bytes", cfg_desc->wTotalLength);
  int  claimed_iface = -1;
  uint8_t in_ep = 0, out_ep = 0;
  int found_audio_midi = 0;
  for (uint8_t iface_num = 0; iface_num < 8; iface_num++) {
    int iface_off = 0;
    const usb_intf_desc_t* iface = usb_parse_interface_descriptor(cfg_desc, iface_num, 0, &iface_off);
    if (!iface) break;  // no more interfaces
    MIDI_LOG("configure: iface=%u class=0x%02X subclass=0x%02X numEP=%u alt=%u",
             iface_num, iface->bInterfaceClass, iface->bInterfaceSubClass,
             iface->bNumEndpoints, iface->bAlternateSetting);
    if (iface->bInterfaceClass == 0x01 && iface->bInterfaceSubClass == 0x03) {
      found_audio_midi++;
      // First MIDI interface wins.
      if (claimed_iface < 0) {
        claimed_iface = iface_num;
        for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
          int ep_off = iface_off;
          const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
              iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
          if (!ep) continue;
          if ((ep->bmAttributes & 0x03) != 2) continue;  // not bulk
          // Log full endpoint descriptor: address, MPS, type+usage, and
          // bInterval. bInterval should be 0 for bulk, but some devices
          // report non-zero values that the host may honor.
          uint8_t  bmAttr     = ep->bmAttributes;
          uint8_t  epType     = bmAttr & 0x03;        // 2=bulk
          uint8_t  usageType  = (bmAttr >> 4) & 0x03;  // 0=data 1=fb 2=impl
          uint16_t mps        = ep->wMaxPacketSize & 0x7FF;
          uint8_t  interval   = ep->bInterval;
          MIDI_LOG("configure:   EP 0x%02X %s usage=%s MPS=%u interval=%u",
                   ep->bEndpointAddress,
                   epType == 2 ? "bulk" : (epType == 3 ? "intr" : "other"),
                   usageType == 0 ? "data" : (usageType == 1 ? "fb" : "impl"),
                   mps, interval);
          if      ((ep->bEndpointAddress & 0x80) && !in_ep)  in_ep  = ep->bEndpointAddress;
          else if (!(ep->bEndpointAddress & 0x80) && !out_ep) out_ep = ep->bEndpointAddress;
        }
      }
    }
  }

  // Vendor-class fallback: scan interface 0 endpoints.
  if (claimed_iface < 0 || in_ep == 0) {
    MIDI_LOG("configure: no Audio/MIDI iface — vendor-class fallback (iface 0)");
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
        MIDI_LOG("configure:   fallback EP 0x%02X bulk wMaxPacketSize=%u",
                 ep->bEndpointAddress, ep->wMaxPacketSize);
        if      ((ep->bEndpointAddress & 0x80) && !in_ep)  in_ep  = ep->bEndpointAddress;
        else if (!(ep->bEndpointAddress & 0x80) && !out_ep) out_ep = ep->bEndpointAddress;
      }
    }
  }

  if (claimed_iface < 0 || in_ep == 0) {
    MIDI_LOG("configure: FAILED — no usable MIDI endpoints (audio_midi_ifaces=%d)", found_audio_midi);
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
  midi_in_endpoint_set = true;
  midi_packets_total    = 0;
  midi_packets_at_last  = 0;
  midi_last_rate_log_ms = millis();

  // Reset "first-time" log flags so a re-plug also gets the first-completion
  // trace.
  midi_diag_reset_firsts();

  // CRITICAL: USB_HOST_CLIENT_EVENT_NEW_DEV fires when the device handle is
  // allocated, but the host library's background enum thread is still in the
  // middle of ENUM_STAGE_SET_CONFIG (which sends SET_CONFIGURATION to the
  // device and waits for the response). The host library rejects URB enqueue
  // with ESP_ERR_INVALID_STATE when dev->state != USB_DEVICE_STATE_CONFIGURED
  // (see usbh.c:1029-1030).
  //
  // We can't query the internal state directly, but bConfigurationValue in
  // usb_device_info_t is 0 until SET_CONFIG completes, then 1+ for the
  // active configuration. Poll it (with timeout) before submitting IN
  // transfers. The OUT path doesn't have this problem because the usermod's
  // first OUT submission is much later — the host library has finished by
  // then.
  uint32_t wait_start = millis();
  const uint32_t WAIT_TIMEOUT_MS = 2000;
  int wait_iters = 0;
  while ((millis() - wait_start) < WAIT_TIMEOUT_MS) {
    usb_device_info_t dev_info = {};
    err = usb_host_device_info(dev, &dev_info);
    if (err == ESP_OK && dev_info.bConfigurationValue != 0) {
      break;
    }
    // Give the host library's enum thread time to advance by letting its
    // event loop run. We need a real delay here (not a tight poll), and
    // pumping client events gives the lib a chance to make progress.
    usb_host_client_handle_events(midi_client, 5);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_iters++;
  }
  usb_device_info_t dev_info = {};
  usb_host_device_info(dev, &dev_info);
  if (dev_info.bConfigurationValue == 0) {
    MIDI_LOG("configure: TIMEOUT waiting for bConfigurationValue (waited %u ms, %d iters)",
             (unsigned)(millis() - wait_start), wait_iters);
    usb_host_device_close(midi_client, dev);
    return ESP_ERR_TIMEOUT;
  }
  MIDI_LOG("configure: device configured (bConfigurationValue=%u) after %d iters / %u ms",
           dev_info.bConfigurationValue, wait_iters, (unsigned)(millis() - wait_start));

  // Halt+flush any stale data on the IN endpoint before the first submit.
  // Belt-and-braces: clears any half-received state from prior aborted
  // enumeration attempts, and is harmless on a clean endpoint.
  usb_host_endpoint_halt(midi_dev_hdl, midi_in_ep);
  usb_host_endpoint_flush(midi_dev_hdl, midi_in_ep);
  usb_host_endpoint_halt(midi_dev_hdl, midi_out_ep);
  usb_host_endpoint_flush(midi_dev_hdl, midi_out_ep);

  MIDI_LOG("configure: CLAIMED iface=%d IN=0x%02X OUT=0x%02X (vendor=%s)",
           claimed_iface, in_ep, out_ep,
           found_audio_midi ? "audio/midi" : "fallback");

  for (int i = 0; i < NUM_TRANSFERS; i++) {
    usb_transfer_t* x = midi_in_xfer[i];
    if (!x) continue;
    x->device_handle     = dev;
    x->bEndpointAddress = in_ep;
    x->num_bytes        = TRANSFER_SIZE;
    err = usb_host_transfer_submit(x);
    if (err == ESP_OK) {
      midi_in_in_flight[i] = true;
    } else {
      midi_in_in_flight[i] = false;
      MIDI_LOG("configure: initial IN submit[%d] failed: %s", i, esp_err_to_name(err));
    }
  }
  MIDI_LOG("configure: %d IN transfers submitted — ready for MIDI traffic", NUM_TRANSFERS);
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
    midi_in_in_flight[i] = false;
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
  // Reset intro handshake so a re-plug re-initialises the controller.
  midi_intro_sent     = false;
  midi_intro_first_ms = 0;
}