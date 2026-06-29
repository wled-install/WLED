# usermod_v3_midi — USB-MIDI Control Surface for WLED

Use a class-compliant USB-MIDI controller (Akai APC Mini MK2, Donner Starrypad, etc.) as a control surface for WLED. Plugs into one of the ESP32-P4 EV board's USB-A ports.

## Compatibility notes (read this first)

**This usermod is fully functional today with the Akai APC Mini Mk2 — that's the primary target.** Default button/pad mappings, LED feedback palette, and the two-press confirmation flow were all designed around it. Future revisions will aim to make it more generic for other class-compliant controllers (Donner Starrypad, Korg nanoKONTROL, etc.). Patches welcome — VID/PID additions and CC layout tweaks go in `midi_usb_host.cpp`'s `kMidiVendorIds` / `kMidiDevices` tables.

**Hardware caveat — ESP32-P4 boards *without* an integrated USB hub only.** Many ESP32-P4 dev boards (e.g., the 4-port USB-A carrier boards) have an integrated High-Speed USB hub chip between the ESP32-P4 and the USB-A ports. Full-Speed MIDI controllers behind those HS hubs hit a known limitation in the current Espressif ESP-IDF USB Host stack — enumeration stalls and `usb_host_transfer_submit()` returns `ESP_ERR_INVALID_STATE`. This is **not a bug in this usermod** — it's a limitation of the Espressif IDF. The usermod is verified on the Espressif ESP32-P4 EVB, which exposes the P4's native USB OTG directly.

**The USB port must be powered.** Some ESP32-P4 boards (e.g., the WaveShare P4 box with the square display) expose a USB-C port for power only — there is no USB data path, so this usermod cannot work on those boards. Check your board schematic before assuming USB-MIDI will work.

**USB-MSC vs USB-MIDI — it's exclusive, not concurrent.** This usermod does not stop USB Mass Storage from working, but the P4's USB Host stack can only operate **one** class driver at a time per device. If you want USB-MIDI on the same USB-A port, you cannot also mount a USB stick there for `ImagePlayer`. The recommended workaround: put your media on the board's **microSD card slot** instead. The microSD path is faster than USB-MSC, supports hot-unmount cleanly, and leaves the USB-A port free for the MIDI controller.

## Targets

- **Board:** ESP32-P4 EVB (`esp32-p4-evboard`)
- **Build env:** `env:esp32p4_8MB_troyhacks` in `platformio_override.ini`
- **Build flag:** `-D USERMOD_MIDI_USB`
- **Framework:** pioarduino + `framework-arduinoespressif32 @ https://github.com/troyhacks/arduino-esp32#feature/esp32p4`

## Default mapping (Akai APC Mini MK2)

8x8 RGB pad grid + 9 faders + 8 track buttons + 8 scene launch buttons + shift.

| Control           | MIDI             | Maps to                                          |
|-------------------|------------------|--------------------------------------------------|
| Pads 0..63        | Note 0x00..0x3F  | Preset 1..64                                     |
| Fader 1           | CC 0x30 (48)     | `effectSpeed`                                    |
| Fader 2           | CC 0x31 (49)     | `effectIntensity`                                |
| Fader 3           | CC 0x32 (50)     | `effectCustom1` (main segment)                   |
| Fader 4           | CC 0x33 (51)     | `effectCustom2` (main segment)                   |
| Fader 5           | CC 0x34 (52)     | `effectCustom3` (0..31, main segment)            |
| Fader 6           | CC 0x35 (53)     | `effectPalette`                                  |
| Faders 7..8       | CC 0x36..0x37    | unused                                           |
| Fader 9 (master)  | CC 0x38 (56)     | Global brightness (`bri`)                        |
| Track 1..3        | Note 100..102    | `toggleCheck1` / `toggleCheck2` / `toggleCheck3` |
| Track 4           | Note 103         | `fullRepaint` (shift+ = reboot arm)              |
| Track 5..6        | Note 104..105    | `prevfx` / `nextfx` (shift+ = `prevpal` / `nextpal`) |
| Track 7..8        | Note 106..107    | `prevpreset` / `nextpreset` (skips empty + finite playlists) |
| Shift (Track 9)   | Note 122         | Hold + pad = save current state to that pad's preset slot |
| Scene 1..5        | Note 112..116    | `toggleMirrorX` / `toggleReverseX` / `toggleMirrorY` / `toggleReverseY` / `toggleTranspose` |
| Scene 6           | Note 117         | unused                                           |
| Scene 7           | Note 118         | Hold to activate select mode (copy preset)       |
| Scene 8           | Note 119         | `power` on/off                                   |

Shift+Track 1..3 mirror the plain press (`toggleCheck1/2/3`). Shift+Track 5..8 give palette navigation. All Scene shift variants default to empty (no shift action).

## Pad feedback (APC palette)

Painted automatically on every WLED state change. Status byte drives the animation: `0x96` solid, `0x99` slow pulse (playlist parent playing), `0x9B` fast blink (playlist child / armed-for-delete / copy-failure flash).

| Pad state                                         | Color (APC) | Status |
|---------------------------------------------------|-------------|--------|
| No preset mapped / not saved                      | off         | 0x96   |
| Saved regular preset, not active                  | 45 (blue)   | 0x96   |
| Active regular preset, no playlist               | 21 (green)  | 0x96   |
| Active regular preset, playlist running (child)   | 21 / 32 / 53 | 0x9B  |
| Looping playlist (saved, idle)                    | 53 (magenta)| 0x96   |
| Finite (one-shot) playlist (saved, idle)          | 32 (red)    | 0x96   |
| Looping playlist playing (parent)                 | 53 (magenta)| 0x99   |
| Finite playlist playing (parent)                  | 32 (red)    | 0x99   |
| Music playlist (auto-playlist music slot)         | 13 (yellow) | varies |
| Armed for delete (waiting for second shift+pad)   | 5 (red)     | 0x9B   |
| Copy failed (waiting for Scene 7 release)        | 5 (red)     | 0x9B   |

Single-color buttons (status `0x90`) use velocity-encoded behavior: `0` off, `1` solid, `2` blink (single built-in rate). Color follows the APC velocity table. The `armed` override (reboot / select mode / delete / copy-failure) forces velocity `2` for the affected button.

## Known shift-mode conflicts (read before configuring!)

The Akai APC Mini MK2 firmware **eats** Shift + Scene Launch 6 (note 0x75) and Shift + Scene Launch 7 (note 0x76) as internal hardware-mode toggles — they change the controller's internal mode (Drum Rack / Note Mode / similar firmware-side state changes). These are firmware-internal state changes, not MIDI commands, so they do not reach this usermod at all. If you ever do manage to assign a shift action to Scene Launch 6 or 7, that action will silently never fire when shift is held — the controller intercepts the input first.

The usermod's settings GUI only exposes shift assignments for scene launches 1..5 (and Scene 8 via plain only — Scene 8's shift slot is omitted from the config UI). Scene 6 and 7's shift slots are intentionally not exposed because they're known no-ops. The plain (non-shift) actions on Scene Launch 6 and 7 (default Scene 6 unused, default Scene 7 = `selectMode`) still work normally. Special-case support for the APC firmware-side modes is a possible future extension but is not implemented today.

Before assigning a custom shift action to any scene launch, press the button on the actual device with shift held and confirm the MIDI event reached WLED (look for a `[MIDI] IN: status=0x90 d1=...` line in the log). Only assign shift actions whose MIDI events actually traverse the controller.

## Destructive actions: two-press confirmation

Both the reboot flow and the per-preset delete flow use a two-press confirmation:

1. **First press** (with shift held, for delete; or with shift + reboot button, for reboot): arms the action. The pad / button flashes red (status `0x9B`, color 5).
2. **Releasing shift** (or the relevant modifier) cancels the arm — the LED reverts to its normal color on the next paint.
3. **5-second timeout** cancels the arm.
4. **Second press** (modifier still held, same pad/button) confirms and executes.

This protects against accidental destruction (a stray pad press won't reboot or delete).

## Select-mode preset copy (hold Scene 7)

Hold Scene 7 to activate select mode. While held:
- Press the source pad (any pad with a saved preset).
- Press the destination pad. If the destination is empty, the source preset is copied to that slot. If the destination is already occupied (or the copy fails internally — JSON busy, source missing, file write error), the destination pad flashes red-fast-blink until you release Scene 7. The source selection is also cleared on any failure.

Scene 7 release always clears the select state and any pending copy-failure flash.

## Soft takeover

When `soft_takeover_enabled` is true (default), each fader must "cross" the current WLED value (±8 CC steps) before it starts driving the parameter. This protects against sudden jumps when a fader is touched at a position different from WLED's current value.

## JSON config (`um.MidiUsb`)

```json
{
  "MidiUsb": {
    "enabled": true,
    "feedback_enabled": true,
    "copy_enabled": true,
    "delete_enabled": true,
    "reboot_enabled": true,
    "soft_takeover_enabled": true,
    "target_segment": 0,
    "save_preset_cc": 0,
    "track_buttons": [
      { "id": 1, "action": "toggleCheck1" },
      { "id": 2, "action": "toggleCheck2" },
      { "id": 3, "action": "toggleCheck3" },
      { "id": 4, "action": "fullRepaint" },
      { "id": 5, "action": "prevfx" },
      { "id": 6, "action": "nextfx" },
      { "id": 7, "action": "prevpreset" },
      { "id": 8, "action": "nextpreset" }
    ],
    "track_buttons_shift": [
      { "id": 1, "action": "toggleCheck1" },
      { "id": 2, "action": "toggleCheck2" },
      { "id": 3, "action": "toggleCheck3" },
      { "id": 4, "action": "fullRepaint" },
      { "id": 5, "action": "prevpal" },
      { "id": 6, "action": "nextpal" },
      { "id": 7, "action": "prevpreset" },
      { "id": 8, "action": "nextpreset" }
    ],
    "scene_buttons": [
      { "id": 1, "action": "toggleMirrorX" },
      { "id": 2, "action": "toggleReverseX" },
      { "id": 3, "action": "toggleMirrorY" },
      { "id": 4, "action": "toggleReverseY" },
      { "id": 5, "action": "toggleTranspose" },
      { "id": 6, "action": "" },
      { "id": 7, "action": "selectMode" },
      { "id": 8, "action": "power" }
    ]
  }
}
```

The pad layout (top-left physical pad = preset 1) and CC map (faders → effect params, master → `bri`) are fixed and not exposed in the settings UI.

### Action vocabulary (case-sensitive)

**Parameter setters** (CC value 0..127 maps to 0..254 for 8-bit globals; fader CCs only):
- `bri`
- `effectSpeed`
- `effectIntensity`
- `effectPalette`
- `effectCurrent`
- `effectCustom1`, `effectCustom2` (writes `Segment::custom1/2` of main segment)
- `effectCustom3` (clamped to 0..31)

**Verbs** (track / scene button press; exposed in the GUI dropdown):
- `power`, `blackout`, `full`, `nightlight`
- `nextfx`, `prevfx`, `nextpal`, `prevpal`
- `nextpreset`, `prevpreset` (skip empty + finite playlists)
- `toggleCheck1`, `toggleCheck2`, `toggleCheck3` (FX-specific per-effect options)
- `toggleMirrorX`, `toggleReverseX`, `toggleMirrorY`, `toggleReverseY`, `toggleTranspose`
- `toggleFreeze`
- `fullRepaint` (repaint controller colors, just in case)
- `rebootArm` (shift + assigned button; hit twice to arm then execute)
- `selectMode` (hold to activate copy flow)
- `""` (empty string) — button is unused

## USB-HS / USB-FS caveat

The P4's USB OTG controller runs **High-Speed OR Full-Speed, never both**. Plugging in a Full-Speed MIDI controller drops the bus to FS mode, slowing down the existing `/usb0` MSC mass-storage mount. Acceptable for MIDI — bandwidth is trivial — but `ImageCacheManager::startPreload("/usb0")` may be slower while a MIDI controller is attached.

## Reliability caveat (important!)

**The P4 EV board's ESP-IDF USB Host library has known issues bringing Full-Speed MIDI devices into `USB_DEVICE_STATE_CONFIGURED` state.** The host library's internal enum thread routinely fails on `CHECK_CONFIG` for the AKAI APC Mini MK2 and similar controllers. While in that failed state, `usb_host_transfer_submit()` returns `ESP_ERR_INVALID_STATE` for every URB — the device handle is allocated, the descriptors are read, the interface is claimed, but no transfers can be submitted.

In practice this means **you may need to unplug and replug the controller several times before the host library happens to complete `SET_CONFIGURATION` successfully.** When it does, the lights come on, faders start working, and both paths stay alive until the next host-library hiccup.

This is a known limitation of the P4 EV board's USB Host controller + ESP-IDF v5 USB Host library combination. The most likely effective fix is migrating to the TinyUSB host stack (which has a more tolerant enumeration state machine), but the current usermod sticks with ESP-IDF USB Host and accepts the intermittent behavior.

**What NOT to do:** do not add aggressive retry/poll logic to `midi_usb_host.cpp`. Earlier debugging showed that adding heartbeat logging, safety-net IN resubmits, or AKAI Introduction SysEx sends interfered with the brief window where the host library reaches `CONFIGURED`, breaking the working state. Keep `midi_usb_host.cpp` minimal — only do work when the host library reports it has transfers to deliver or when our OUT ringbuffer has data to send.

## Build / flash

```bash
cd c:/Users/troys/WLED
~/.platformio/penv/Scripts/platformio.exe run -e esp32p4_8MB_troyhacks
~/.platformio/penv/Scripts/platformio.exe run -e esp32p4_8MB_troyhacks -t upload
~/.platformio/penv/Scripts/platformio.exe device monitor -e esp32p4_8MB_troyhacks
```

## Files

- `usermod_v3_midi.h` — main usermod class (header-only, ~1850 LOC). Implements the v3 hooks (`onPreStateChange`, `onEvent`) on top of the v2 `Usermod` base class.
- `midi_usb_host.h` / `midi_usb_host.cpp` — USB Host client (descriptor walk, IN/OUT transfers, packet parser, VID/PID filter).
- `usb_host_messages.h` — shared `app_message_t` (MSC events + MIDI events).
- Modifies `wled00/wled.cpp` (extends `app_message_t`, calls `midi_usb_init`/`midi_usb_poll` in `usb_task`, publishes `UsbDeviceChanged` from MSC + MIDI callbacks, dispatches MIDI in `background_loop_nonblocking`).
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
   [MIDI] configure: VID=0x09E8 PID=0x004F (Akai APC Mini Mk2) — MIDI vendor match
   [MIDI] configure: CLAIMED iface=1 IN=0x81 OUT=0x01
   [MIDI] configure: 1 IN transfer submitted
   USB MIDI Device Connected
   ```
   `u.MidiUsb` should now show `Akai APC Mini Mk2 (0x09E8:0x004F)`.
5. **Pads light up** to reflect WLED preset state via the APC palette (active preset = green, saved-but-inactive = blue).
6. Move Fader 9 — `bri` should change in WLED.
7. Press pad 0 — preset 1 should load.
8. From web UI, change `bri` to 200 — pads repaint via the APC palette.
9. Hold Shift, press pad 0 — preset 1 saved with current state.
10. Hold Shift, press a saved pad twice — preset is deleted (red-flash first press, second press confirms).
11. Hold Scene 7, press pad 0, press empty pad 8 — preset 1 is copied to slot 8.
12. Hold Scene 7, press pad 0, press already-saved pad 1 — destination flashes red while Scene 7 is held.
13. Unplug USB — log `[MIDI] event: DEV_GONE`, then `USB MIDI Device Disconnected`, `u.MidiUsb` shows `disconnected`.

**If the pads don't light up and Fader 9 doesn't respond**, see "Reliability caveat" above — unplug and replug the controller a few times to give the host library another chance to complete `SET_CONFIGURATION`.

### Logging levels

The usermod emits two log streams on the debug serial:

- **`[MIDI]` (always on)** — lifecycle events: host client init, NEW_DEV / DEV_GONE, descriptor walk results, claimed interface + endpoints, IN submit failures, OUT queue traces. Goes through `Serial.printf` guarded by `canUseSerial()`, matching the rest of WLED's serial logging behaviour (silent when USB CDC is disconnected).
- **`[MIDI]` from `MIDI_DEBUG` (gated by `-D WLED_DEBUG`)** — per-transfer chatter: OUT submit errors, IN re-submit errors. To enable, uncomment `-D WLED_DEBUG` in `platformio_override.ini` under `[env:esp32p4_8MB_troyhacks]`.

If you see nothing at all, check:
- Are you reading the right serial? On the P4-EV board the USB-A ports (where the MIDI controller plugs in) are *not* the CDC serial — the debug serial is on the GPIO UART pins or a separate USB-C port. Use the `monitor` command above, not a terminal on the controller port.
- `canUseSerial()` returns false when the TX pin is allocated to LEDs or realtime. Check `WLED_USE_ETHERNET_ONLY` and bus pin assignments.

## Open caveats

- The first MSI enumeration after a cold boot takes ~1-2s for class-compliant devices (the host controller has to drop to FS and re-enumerate).
- Some early Donner Starrypad firmware revisions present under `bInterfaceClass == 0xFF` (vendor-specific) — `midi_open_and_configure` in `midi_usb_host.cpp` has a bulk-pair fallback that handles these.
- The APC Mini MK2's master fader (Fader 9) is unidirectional (device→host); sending CC56 back has no visible effect on the controller. Fader feedback is implemented but cosmetic.
- The host library's `USB_DEVICE_STATE_CONFIGURED` window is intermittent on the P4 EV board. When the window collapses, IN callbacks stop firing and OUT submits fail with `ESP_ERR_INVALID_STATE` until the user unplugs and replugs the controller. This is a hardware/library limitation that we can't fix without modifying the ESP-IDF USB Host source — see "Reliability caveat" above.
- Earlier debugging explored sending an AKAI Introduction SysEx (`F0 47 7F 4F 60 00 04 00 <verh> <verl> <bugfix> F7`) on connect to initialize the firmware. This regressed the working state — do NOT re-introduce it. The APC Mini MK2 streams pad/fader events on power-up without any prior SysEx; the default NoteOn / CC / Pad state path is sufficient.
