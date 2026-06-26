# usermod_v2_midi — USB-MIDI Control Surface for WLED

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

## Build / flash

```bash
cd c:/Users/troys/WLED
~/.platformio/penv/Scripts/platformio.exe run -e esp32p4_8MB_troyhacks
~/.platformio/penv/Scripts/platformio.exe run -e esp32p4_8MB_troyhacks -t upload
~/.platformio/penv/Scripts/platformio.exe device monitor -e esp32p4_8MB_troyhacks
```

## Files

- `usermod_v2_midi.h` — main usermod class (header-only, ~480 LOC).
- `midi_usb_host.h` / `midi_usb_host.cpp` — USB Host client (descriptor walk, IN/OUT transfers, packet parser).
- Modifies `wled00/wled.cpp` (extends `app_message_t`, calls `midi_usb_init`/`midi_usb_poll` in `usb_task`, dispatches MIDI in `WLED::loop`).
- Modifies `wled00/usermods_list.cpp` (registers `MidiUsermod`).
- Modifies `wled00/const.h` (adds `USERMOD_ID_MIDI_USB 96`).
- Modifies `platformio_override.ini` (adds `-D USERMOD_MIDI_USB`).

No external library dependencies. Uses only `<usb/usb_host.h>` and FreeRTOS ring buffer / queues already shipped by pioarduino's `framework-arduinoespressif32`.

## Verification checklist

1. Build with `-D USERMOD_MIDI_USB`, flash.
2. Boot. Serial log should include:
   ```
   [MIDI] init: starting (NUM_TRANSFERS=4, TRANSFER_SIZE=64)
   [MIDI] init: USB Host client ready — 4/4 IN + OUT=yes
   ```
3. `GET /json/info` — confirm `u.MidiUsb` shows `disconnected`.
4. Plug APC Mini MK2 into P4-EV USB-A. Log (with timestamps) should look like:
   ```
   [MIDI] event: NEW_DEV addr=1
   [MIDI] configure: VID=0x09E8 PID=0x004F bcdDevice=0x0521 numConfigs=1
   [MIDI] configure: total cfg length=... bytes
   [MIDI] configure: iface=0 class=0x01 subclass=0x03 numEP=2   ← Audio/MIDI Streaming
   [MIDI] configure:   MIDI EP 0x81 bulk wMaxPacketSize=64
   [MIDI] configure:   MIDI EP 0x01 bulk wMaxPacketSize=64
   [MIDI] configure: CLAIMED iface=0 IN=0x81 OUT=0x01 (vendor=audio/midi)
   [MIDI] configure: 4 IN transfers submitted — ready for MIDI traffic
   USB MIDI Device Connected
   ```
   `u.MidiUsb` should now show `Akai APC Mini mkII (MK2) (0x09E8:0x004F)`.
5. Press pad 0 — log `MIDI usermod IN: status=0x90 d1=0 d2=127`, preset 1 applied.
6. Move fader 1 (CC 48) — `effectSpeed` updates in WLED.
7. From web UI, change `bri` to 200 — pads repaint via the Madrix palette (active preset pad = green or magenta).
8. Hold Shift, press pad 0 — preset 1 saved with current state.
9. Unplug USB — log `MIDI event: DEV_GONE` then `USB MIDI Device Disconnected`, `u.MidiUsb` shows `disconnected`.

### Logging levels

The usermod emits two log streams on the debug serial:

- **`[MIDI]` (always on)** — lifecycle events that should always be visible:
  host client init, NEW_DEV / DEV_GONE, descriptor walk results, claimed
  interface + endpoints, IN submit failures, periodic packet rate (every 5 s).
  Goes through `Serial.printf` guarded by `canUseSerial()`, matching the rest
  of WLED's serial logging behaviour (silent when USB CDC is disconnected).
- **`MIDI_DEBUG` (gated by `-D WLED_DEBUG`)** — per-packet and per-transfer
  chatter: IN-transfer byte count, parsed MIDI events, OUT submit status,
  per-packet trace in `MidiUsermod::handleIncomingMidi`. To enable, uncomment
  `-D WLED_DEBUG` in `platformio_override.ini` under `[env:esp32p4_8MB_troyhacks]`.

If you see nothing at all, check:
- Are you reading the right serial? On the P4-EV board the USB-A ports
  (where the MIDI controller plugs in) are *not* the CDC serial — the debug
  serial is on the GPIO UART pins or a separate USB-C port. Use the
  `monitor` command from the README, not a terminal on the controller port.
- `canUseSerial()` returns false when the TX pin is allocated to LEDs or
  realtime. Check `WLED_USE_ETHERNET_ONLY` and bus pin assignments.

## Open caveats

- The first MSI enumeration after a cold boot takes ~1-2s for class-compliant devices (the host controller has to drop to FS and re-enumerate).
- Some early Donner Starrypad firmware revisions present under `bInterfaceClass == 0xFF` (vendor-specific) — `midi_open_and_configure` in `midi_usb_host.cpp` has a bulk-pair fallback that handles these.
- The APC Mini MK2's master fader (Fader 9) is unidirectional (device→host); sending CC56 back has no visible effect on the controller. Fader feedback is implemented but cosmetic.