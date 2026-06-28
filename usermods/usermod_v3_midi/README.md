# usermod_v3_midi — USB-MIDI Control Surface for WLED

Use a class-compliant USB-MIDI controller (Akai APC Mini MK2, Donner Starrypad, etc.) as a control surface for WLED. Plugs into one of the ESP32-P4 EV board's USB-A ports.

## Targets

- **Board:** ESP32-P4 EVB (`esp32-p4-evboard`)
- **Build env:** `env:esp32p4_8MB_troyhacks` in `platformio_override.ini`
- **Build flag:** `-D USERMOD_MIDI_USB`
- **Framework:** pioarduino + `framework-arduinoespressif32 @ https://github.com/troyhacks/arduino-esp32#feature/esp32p4`

## Default mapping (Madrix-style)

Designed around the **Akai APC Mini MK2** (8x8 RGB pad grid + 9 faders + 8 track buttons + 8 scene launch buttons + shift).

| Control | MIDI | Maps to |
|---|---|---|
| Pads 0..63 | Note 0..63 | Preset 1..64 |
| Fader 1 | CC 48 | effectSpeed |
| Fader 2 | CC 49 | effectIntensity |
| Fader 3 | CC 50 | effectPalette |
| Fader 4 | CC 51 | effect custom1 (main segment) |
| Fader 5 | CC 52 | effect custom2 (main segment) |
| Faders 6..8 | CC 53..55 | unused |
| Fader 9 (master) | CC 56 | Global brightness (`bri`) |
| Track 1 | Note 100 | Power toggle |
| Track 2 | Note 101 | Nightlight toggle |
| Track 3 | Note 102 | Next preset |
| Track 4..6 | Note 103..105 | unused |
| Track 7 | Note 106 | Previous preset (`<`) |
| Track 8 | Note 107 | Next preset (`>`) |
| Shift (Track 9) | Note 122 | Hold + pad = save current state to that pad's preset slot |
| Scene 1..7 | Note 112..118 | unused |
| Scene 8 | Note 119 | Blackout (`bri = 0`) |

## Pad feedback (Madrix palette)

Painted automatically on every WLED state change:

| Pad state | Color | Velocity |
|---|---|---|
| No preset mapped / not saved | off | 0 |
| Preset saved, not active | blue (#0000FF) | 45 |
| Preset active, no playlist | green (#00FF00) | 21 |
| Preset active, playlist | magenta (#FF00FF) | 53 |

Track button LEDs (single-color, status 0x90): Track 1 lit when `bri > 0`, Track 2 blinking when nightlight active, Scene 8 lit when blackout.

## JSON config (cfg.json → `um.MidiUsb`)

```json
{
  "MidiUsb": {
    "enabled": true,
    "channel": 1,
    "feedback_enabled": true,
    "feedback_mode": 0,
    "feedback_throttle_ms": 50,
    "save_preset_cc": 0,
    "pads": [
      { "note": 0,  "preset": 1 },
      { "note": 1,  "preset": 2 }
      // pads not listed default to (note + 1)
    ],
    "track_buttons": [
      { "note": 100, "action": "power" },
      { "note": 101, "action": "nightlight" },
      { "note": 106, "action": "prevpreset" },
      { "note": 107, "action": "nextpreset" }
    ],
    "scene_buttons": [
      { "note": 119, "action": "blackout" }
    ],
    "cc_map": {
      "48": "effectSpeed",
      "49": "effectIntensity",
      "50": "effectPalette",
      "51": "effectCustom1",
      "52": "effectCustom2",
      "56": "bri"
    }
  }
}
```

### Action vocabulary (case-sensitive)

**Parameter setters** (CC value 0..127 maps to 0..254 for 8-bit globals):
- `bri`
- `effectSpeed`
- `effectIntensity`
- `effectPalette`
- `effectCurrent`
- `effectCustom1` (writes `Segment::custom1` of main segment)
- `effectCustom2` (writes `Segment::custom2` of main segment)

**Verbs** (button press triggers):
- `power` — toggle on/off
- `nightlight` — toggle nightlight mode
- `nextpreset` / `prevpreset` — cycle current preset
- `nextfx` / `prevfx` — cycle effect mode
- `nextpal` / `prevpal` — cycle palette
- `blackout` — `bri = 0`
- `full` — `bri = 255`
- `""` (empty string) — button is unused

## USB-HS / USB-FS caveat

The P4's USB OTG controller runs **High-Speed OR Full-Speed, never both**. Plugging in a Full-Speed MIDI controller drops the bus to FS mode, slowing down the existing `/usb0` MSC mass-storage mount. Acceptable for MIDI — bandwidth is trivial — but `ImageCacheManager::startPreload("/usb0")` may be slower while a MIDI controller is attached.

## Reliability caveat (important!)

**The P4 EV board's ESP-IDF USB Host library has known issues bringing Full-Speed MIDI devices into `USB_DEVICE_STATE_CONFIGURED` state.** The host library's internal enum thread routinely fails on `CHECK_CONFIG` for the AKAI APC Mini MK2 and similar controllers. While in that failed state, `usb_host_transfer_submit()` returns `ESP_ERR_INVALID_STATE` for every URB — the device handle is allocated, the descriptors are read, the interface is claimed, but no transfers can be submitted.

In practice this means **you may need to unplug and replug the controller several times before the host library happens to complete `SET_CONFIGURATION` successfully.** When it does, the lights come on, faders start working, and both paths stay alive until the next host-library hiccup.

This is a known limitation of the P4 EV board's USB Host controller + ESP-IDF v5 USB Host library combination. Possible workarounds are discussed in the [memory note](C:/Users/troys/.claude/projects/c--Users-troys-WLED/memory/wled-usb-midi-usermod.md) — the most likely effective fix is migrating to the TinyUSB host stack (which has a more tolerant enumeration state machine), but the current usermod sticks with ESP-IDF USB Host and accepts the intermittent behavior.

**What NOT to do:** do not add aggressive retry/poll logic to `midi_usb_host.cpp`. Earlier debugging showed that adding heartbeat logging, safety-net IN resubmits, or AKAI Introduction SysEx sends interfered with the brief window where the host library reaches `CONFIGURED`, breaking the working state. Keep `midi_usb_host.cpp` minimal — only do work when the host library reports it has transfers to deliver or when our OUT ringbuffer has data to send.

## Build / flash

```bash
cd c:/Users/troys/WLED
~/.platformio/penv/Scripts/platformio.exe run -e esp32p4_8MB_troyhacks
~/.platformio/penv/Scripts/platformio.exe run -e esp32p4_8MB_troyhacks -t upload
~/.platformio/penv/Scripts/platformio.exe device monitor -e esp32p4_8MB_troyhacks
```

## Files

- `usermod_v3_midi.h` — main usermod class (header-only, ~1850 LOC).
- `midi_usb_host.h` / `midi_usb_host.cpp` — USB Host client (descriptor walk, IN/OUT transfers, packet parser).
- `usb_host_messages.h` — shared `app_message_t` (MSC events + MIDI events).
- Modifies `wled00/wled.cpp` (extends `app_message_t`, calls `midi_usb_init`/`midi_usb_poll` in `usb_task`, dispatches MIDI in `WLED::loop`).
- Modifies `wled00/usermods_list.cpp` (registers `MidiUsermod`).
- Modifies `wled00/const.h` (adds `USERMOD_ID_MIDI_USB 96`).
- Modifies `platformio_override.ini` (adds `-D USERMOD_MIDI_USB`).

No external library dependencies. Uses only `<usb/usb_host.h>` and FreeRTOS ring buffer / queues already shipped by pioarduino's `framework-arduinoespressif32`.

## Verification checklist

1. Build with `-D USERMOD_MIDI_USB`, flash.
2. Boot. Serial log should include:
   ```
   [MIDI] init: USB Host client ready
   ```
3. `GET /json/info` — confirm `u.MidiUsb` shows `disconnected`.
4. Plug APC Mini MK2 into P4-EV USB-A. Serial log should look like:
   ```
   [MIDI] event: NEW_DEV addr=1
   [MIDI] configure: VID=0x09E8 PID=0x004F (Akai APC Mini mkII (MK2))
   [MIDI] configure: CLAIMED iface=1 IN=0x81 OUT=0x01
   [MIDI] configure: 4 IN transfers submitted
   USB MIDI Device Connected
   ```
   `u.MidiUsb` should now show `Akai APC Mini mkII (MK2) (0x09E8:0x004F)`.
5. **Pads light up** to reflect WLED preset state via the Madrix palette (active preset = green, saved-but-inactive = blue).
6. Move Fader 9 — `bri` should change in WLED.
7. Press pad 0 — preset 1 should load.
8. From web UI, change `bri` to 200 — pads repaint via the Madrix palette.
9. Hold Shift, press pad 0 — preset 1 saved with current state.
10. Unplug USB — log `MIDI event: DEV_GONE` then `USB MIDI Device Disconnected`, `u.MidiUsb` shows `disconnected`.

**If the pads don't light up and Fader 9 doesn't respond**, see "Reliability caveat" above — unplug and replug the controller a few times to give the host library another chance to complete `SET_CONFIGURATION`.

### Logging levels

The usermod emits two log streams on the debug serial:

- **`[MIDI]` (always on)** — lifecycle events: host client init, NEW_DEV / DEV_GONE, descriptor walk results, claimed interface + endpoints, IN submit failures. Goes through `Serial.printf` guarded by `canUseSerial()`, matching the rest of WLED's serial logging behaviour (silent when USB CDC is disconnected).
- **`MIDI_DEBUG` (gated by `-D WLED_DEBUG`)** — per-packet and per-transfer chatter: OUT submit errors, IN re-submit errors. To enable, uncomment `-D WLED_DEBUG` in `platformio_override.ini` under `[env:esp32p4_8MB_troyhacks]`.

If you see nothing at all, check:
- Are you reading the right serial? On the P4-EV board the USB-A ports (where the MIDI controller plugs in) are *not* the CDC serial — the debug serial is on the GPIO UART pins or a separate USB-C port. Use the `monitor` command from the README, not a terminal on the controller port.
- `canUseSerial()` returns false when the TX pin is allocated to LEDs or realtime. Check `WLED_USE_ETHERNET_ONLY` and bus pin assignments.

## Open caveats

- The first MSI enumeration after a cold boot takes ~1-2s for class-compliant devices (the host controller has to drop to FS and re-enumerate).
- Some early Donner Starrypad firmware revisions present under `bInterfaceClass == 0xFF` (vendor-specific) — `midi_open_and_configure` in `midi_usb_host.cpp` has a bulk-pair fallback that handles these.
- The APC Mini MK2's master fader (Fader 9) is unidirectional (device→host); sending CC56 back has no visible effect on the controller. Fader feedback is implemented but cosmetic.
- The host library's `USB_DEVICE_STATE_CONFIGURED` window is intermittent on the P4 EV board. When the window collapses, IN callbacks stop firing and OUT submits fail with `ESP_ERR_INVALID_STATE` until the user unplugs and replugs the controller. This is a hardware/library limitation that we can't fix without modifying the ESP-IDF USB Host source — see "Reliability caveat" above.
- Earlier debugging explored sending an AKAI Introduction SysEx (`F0 47 7F 4F 60 00 04 00 <verh> <verl> <bugfix> F7`) on connect to initialize the firmware. This regressed the working state — do NOT re-introduce it. The APC Mini MK2 streams pad/fader events on power-up without any prior SysEx; the default NoteOn / CC / Pad state path is sufficient.