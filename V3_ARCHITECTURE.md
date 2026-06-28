# V3 Architecture

The "v3" layer in this port is a typed event bus, a pre-state-change
latch, a state-query API, and a debug event log. It sits on top of the
existing `Usermod` v2 base class. v3 is *additive* — existing v2
usermods keep working unchanged, and v3-aware code (usermods or core)
opts in by overriding two new virtuals.

This is not stock WLED. This is the WLED-MM-P4 port. v3 is built to
solve specific pain points in this fork. Backward compat with stock
WLED is not a constraint.

## Scope (be honest about this)

**Tested on:** ESP32-P4 (`env:esp32p4_8MB_troyhacks`).

**Future-friendly (should compile, not yet verified):** other
RISC-V ESP32 chips (C5, H2, S31). The architecture is portable
across any FreeRTOS-capable ESP32 variant. There is no P4-specific
code in the v3 layer.

**Out of scope:** ESP32 (Xtensa), ESP32-S3, ESP32-Pico, ESP8266. Code
that only exists for those chips is not part of the v3 work. The
P4-only build flags (`PARLIO_PINS`, the `WLED_DANGEROUS_OPTIMIZATIONS`
family, the `CONFIG_ARDUINO_RUNNING_CORE` overrides) are unchanged.

## Why we built it (the motivation, plainly)

The MIDI usermod is the largest usermod in the port and it had a
shoebox full of "hacks" that existed only because WLED's existing
state-propagation surface was too coarse:

- A `tracked_active_preset` byte that mirrors `currentPreset`
  because `stateUpdated()` wipes it to 0 before any usermod sees it
  (led.cpp:106).
- A 251-byte `cached_playlist_repeat[]` + a standalone
  `playlist_repeat_cache.cpp` that re-reads `/presets.json` on
  every `onStateChange` because the existing `presetCache[]` didn't
  carry the per-preset repeat value.
- A 6.5 KB heap allocation `effect_index[]` + `ensureEffectIndex()`
  because there's no `getEffectIdByDisplayIndex()`.
- A `prev_currentPlaylist` static polled in `loop()` to detect
  "playlist just ended" because there's no `PlaylistEnded` event.
- A `prev_power_off` static in `handlePowerPaint` to detect the
  off→on edge because there's no `PowerEdge` event.

These were all symptoms of one underlying thing: the only existing
hook (`onStateChange(uint8_t mode)`) is a firehose with no payload.
It tells you *something* changed. It doesn't tell you what, who
caused it, or what the relevant state was before the wipe.

v3 adds three things on top:

1. **`onPreStateChange(uint8_t mode)`** — fires *before* the wipe.
   Subscribers can latch pre-wipe values into their own state.
   This single hook deletes `tracked_active_preset` and friends.
2. **`onEvent(const wled::Event& ev)`** — typed event delivery with
   payloads. `PowerEdge` instead of polling `bri`. `PlaylistEnded`
   instead of polling `currentPlaylist`.
3. **State-query accessors in core** — `getPresetRepeat(slot)`,
   `getEffectIdByDisplayIndex(pos)`, plus the `PresetMetadata` struct
   now carrying `repeat`. Usermods no longer maintain their own
   shadow caches of data the core already has.

There's also a debug tool: a 50-entry ring buffer (`/json/eventlog`)
that records every event published. It exists because the lack of
visibility into "did the event fire, with what payload" is the #1
debugging pain when building an event-driven system. Build the
observability up front.

## Core concepts (what a "v3" thing actually is)

### The `wled::Event` struct

POD. 32 bytes payload + 4 bytes type/timestamp + 1 byte source.

```cpp
struct Event {
  EventType type;             // enum: see event_bus.h
  uint32_t  timestamp_ms;
  uint8_t   source_id;         // USERMOD_ID_* of publisher; 0 = WLED core
  union Payload {
    struct { uint8_t preset; } presetApplied;
    struct { uint8_t kind; uint8_t slot; } presetListMutated;
    struct { int16_t playlist; uint8_t entry; } playlistStarted;
    struct { int16_t playlist; bool hadEndPreset; uint8_t endPreset; } playlistEnded;
    struct { bool wasOff; bool isOff; } powerEdge;
    struct { uint8_t newIndex; } effectIndexChanged;
    struct { bool connected; uint16_t vid; uint16_t pid; char name[24]; } usbDevice;
    uint8_t raw[32];
  } payload;
};
```

### Dispatch is synchronous

`EventBus::publish(ev)` walks the registered usermods in the
caller's task context and calls each one's `onEvent(ev)`. No queue,
no FreeRTOS task for event delivery. This is the deliberate
choice — it means:

- A handler that takes 1ms delays the publisher by 1ms.
- Handlers run in whichever task is the publisher at that moment
  (often `BG_Blocking` because many publishers are inside
  `handlePlaylist` and friends).
- No queue means no event-drop on overflow. (The opposite trade-off
  also applies.)

The contract for handlers: short, no blocking I/O, no `busMutex`
acquisition, no JSON lock, no long loops. Enforce by code review.
Documented in the event_bus.h header.

If you need async delivery later, layer it on top. Don't change the
synchronous contract; instead, add a queue-backed path that mirrors
events to a queue and dispatches in the background. Today's
publishers all want the synchronous semantics; future event-bus
growth can add a queue.

### The pre-state-change latch

`onPreStateChange(uint8_t mode)` fires inside `stateUpdated()` *before*
the `currentPreset = 0` wipe. Use it to capture pre-wipe state into
your usermod's own members.

```cpp
void onPreStateChange(uint8_t mode) override {
  if (currentPreset != 0) tracked_active_preset = currentPreset;
}
```

This is a one-liner replacement for the entire
`updateTrackedActivePreset()` heuristic tree the MIDI usermod had.

### State-query accessors

Plain additions to WLED core. No hooks, no events, just functions
that read from the existing core state (`presetCache[]`,
`strip.getModeData()`) and return useful derivations.

- `getPresetRepeat(byte slot)` — playlist repeat count, 0 = infinite.
- `getNextPreset(byte slot)` / `getPreviousPreset(byte slot)` /
  `getFirstPreset()` / `getLastPreset()` — navigation helpers.
  Wrap around. Return 0 if none.
- `getPresetCount()` / `getPlaylistPresetCount()` — totals.
- `getEffectDisplayCount()` / `getEffectIdByDisplayIndex(pos)` —
  sorted effect index, built lazily.

The MIDI usermod's `effect_index` heap (6.5 KB) and
`cached_playlist_repeat[]` (251 B) are both gone. Replaced by these
calls.

### The debug event log

`/json/eventlog` returns the last 50 events, most-recent first:

```json
[
  {
    "type": "PlaylistEnded",
    "timestamp_ms": 1234567,
    "source_id": 0,
    "seq": 47,
    "payload": {"playlist": 15, "hadEndPreset": false, "endPreset": 0}
  },
  ...
]
```

Optional `?since=<ms>` filter. 50 entries × ~40 bytes ≈ 2 KB RAM.

## What a "v3 usermod" looks like

A v3 usermod is a regular `Usermod` subclass that overrides one or
both new virtuals. The minimal example:

```cpp
class MyUsermod : public Usermod {
  // ... existing v2 stuff: setup, loop, addToConfig, etc. ...

  void onPreStateChange(uint8_t mode) override {
    // Latch the pre-wipe value of currentPreset, if it's set.
    if (currentPreset != 0) my_tracked_preset_ = currentPreset;
  }

  void onEvent(const wled::Event& ev) override {
    switch (ev.type) {
      case wled::EventType::PowerEdge:
        // Repaint on off↔on transitions instead of polling bri.
        if (ev.payload.powerEdge.wasOff != ev.payload.powerEdge.isOff) {
          repaint_controller();
        }
        break;
      case wled::EventType::PlaylistEnded:
        // Force a repaint so the controller's child pad stops
        // fast-blinking after the playlist ends.
        repaint_controller();
        break;
      // ... other cases you care about ...
      default: break;
    }
  }
};
```

That's it. No new base class, no new lifecycle. v3 is a *layer* on
v2, not a replacement. Existing v2 hooks continue to work
(`setup`, `loop`, `onStateChange`, `addToConfig`, etc.).

## How to add a new event type

1. Add to `EventType` enum in `wled00/event_bus.h`. Use the next
   unused value in the relevant category. Keep values stable — they
   are ABI.

2. Add a payload struct in the `Event::Payload` union in
   `event_bus.h`. Document the field meanings.

3. Add a publish site in core where the event naturally fires.
   Most events are published from `presets.cpp` (preset save/apply/
   delete), `playlist.cpp` (playlist start/end), `led.cpp`
   (stateUpdated, power transitions), `wled.cpp` (USB callbacks),
   `FX.cpp` (effect changes).

4. Add the serializer case in `wled00/event_log.cpp`'s
   `writePayload()`. Otherwise the event shows up in
   `/json/eventlog` as `"payload": {}`.

5. Done. No new build flag. No registration. The event is
   published; existing usermods ignore it; new usermods subscribe
   by adding a `case` in their `onEvent`.

The pattern in code:

```cpp
// In some core function, where the event naturally occurs.
{
  wled::Event ev = {};
  ev.type = wled::EventType::MyNewEvent;
  ev.timestamp_ms = millis();
  ev.source_id = 0;  // WLED core
  ev.payload.myNewEvent.someField = someValue;
  wled::EventBus::publish(ev);
}
```

## How to make an existing usermod v3-aware

Three-step migration:

1. **Add the overrides** to your usermod class. Start with
   `onPreStateChange` (latch pre-wipe state) and `onEvent` (switch
   on the events you care about). Both default to no-op in the
   base class, so they don't affect anyone else.

2. **Replace the workarounds in your existing logic.** As you wire
   up `onEvent` to do what your old static-tracking logic did, the
   old logic becomes redundant. Delete the statics, the cached
   arrays, the polling in `loop()`. The MIDI usermod's
   `effect_index` heap, `ensureEffectIndex()`,
   `cached_playlist_repeat[]`, `ensurePlaylistRepeatCache()`,
   `prev_currentPlaylist`, and `prev_power_off` all went away this
   way.

3. **Verify the build is still green and the existing manual tests
   still pass.** v3 hook overrides are no-ops until they do
   something, so a misstep in step 1 doesn't break anything; a
   misstep in step 2 is where the bugs will be. Migrate one
   workaround at a time, test after each.

The MIDI usermod migration took the existing hacks from ~410 bytes
of state + a 6.5 KB heap allocation down to nothing. The net
result is a smaller, simpler, more correct usermod.

## Gotchas (things that will bite you if you don't read this)

**1. Handlers run in the publisher's task context.** A handler
called from `handlePlaylist` runs in `BG_Blocking` with
`busMutex` held. A handler that tries to take `busMutex` or
`requestJSONBufferLock` will deadlock. A handler that does
heavy I/O will block the playlist engine. Keep handlers short.

**2. Handlers must not throw.** C++ exceptions are disabled in this
port (`-fno-exceptions` is on), so this is academic for now. If
that ever changes, an unhandled throw in a handler will unwind
through the playlist engine and bad things will happen.

**3. `updateInterfaces()` is bypassed for the 1.2s cooldown by
`pushInterfaceUpdate()`.** If you need to force a WS push after
a preset-list mutation, use `pushInterfaceUpdate(mode)`, not
`updateInterfaces(mode)`. Or just listen for `PresetListMutated`
and call `pushInterfaceUpdate` from your handler. The cooldown
exists for a reason (don't blow up the WS path on every save).

**4. The ArduinoJson ABI version is sensitive.** `event_log.h`
forces `ARDUINOJSON_DECODE_UNICODE 0` before the ArduinoJson
include to keep the namespace version tag stable. If you add a new
TUs that include `event_log.h` (or any header that brings in
ArduinoJson), match the same macro value, or you'll get
`undefined reference` errors at link time with mismatched
namespace version tags (`V6215PA2` vs `V6215PB2`). The
`wled.h` path sets it to 0 after the first include; mirror that.

**5. Don't poll globals in `loop()` to "watch for events".** The
v3 philosophy is: get notified. Polling is a smell. If you find
yourself writing `static bool prev_x = ...; if (prev_x !=
current_x) { ... }`, the right answer is an event type + an
`onEvent` handler.

**6. Don't store huge state in `Event` payloads.** Events are
POD, copied by value, and the union is fixed-size. If you need
to pass a 100KB blob, pass a pointer or queue separately and
let the handler pull from a shared buffer.

**7. The 50-entry eventlog ring is fixed-size.** When it fills, the
oldest entry is overwritten. If you need more, change
`kEventLogCapacity` in `event_log.h`. But if you need *much* more,
you want a circular buffer in PSRAM, not a bigger static array.

**8. `onPreStateChange` fires INSIDE `stateUpdated()`, *before*
`currentPreset = 0`.** It does NOT fire for free-running state
changes (e.g., transition completion). If you want to be notified
about those too, the right pattern is to publish a new event
type from the place where the transition completes, not to try to
catch it in `onPreStateChange`.

**9. Don't override `Usermod`'s `onEvent` to do JSON parsing of
big payloads.** The current payload union is 32 bytes; if a
future event needs more, prefer pointer-to-shared-buffer over
blowing the union out. (32 bytes is already a non-trivial chunk
to put on the stack of a 240 MHz Cortex.)

**10. Pioneer v3 and the v2 Pioneer are MUTUALLY EXCLUSIVE.**
Both define the same `prolink_*_public` file-scope symbols
that `FX.cpp` reads. Defining both `-D USERMOD_PIONEER_PROLINK`
and `-D USERMOD_PIONEER_PROLINK_V3` will give you duplicate
symbol link errors. Choose one.

**11. `applyPreset` is async, `handlePresets` does the work.**
`applyPreset(15)` just sets a global `presetToApply = 15` and
returns. The actual deserialization and apply happens in
`handlePresets()`, which is called from `BG_Blocking`. So
`applyPreset(...)` followed by `stateUpdated(...)` in the same
handler does NOT see the applied state — the state change
happens later. The pattern for the MIDI usermod is:
set `tracked_active_preset`, call `applyPreset`,
call `handlePresets`, then call `midi_usb_poll()`. The
`midi_usb_poll()` after `handlePresets` is the trick that
fixed the 1-second visual lag.

**12. The Pioneer v3 file is a separate copy, not a derivative
of the original.** Pioneer v3 is a full copy of
`usermod_v2_pioneer_prolink/usermod_v2_pioneer_prolink.h` with
the class renamed to `ProLinkUsermodV3` and v3 hook stubs
added. The original Pioneer is untouched. The Pioneer v3
handler currently logs every event to `USER_PRINTF` and does
nothing else. Migrating Pioneer v3 to actually use the
events (replacing `prolink_presetMover` flag-borrow with
`PresetCycleRequested` subscriptions) is future work.

**13. v3 is not a replacement for the existing `onStateChange`
hook.** Both fire. `onStateChange` fires AFTER `stateUpdated`
finishes its wipe and notification chain; `onPreStateChange`
fires BEFORE the wipe; `onEvent` fires from explicit publish
sites. If you only need to know "the state changed" with no
specific event in mind, `onStateChange` is still the right
hook. If you need a specific event with a payload, use
`onEvent`. If you need to capture pre-wipe state, use
`onPreStateChange`. They're complementary, not redundant.

**14. Don't put long blocking calls in `onEvent`.** Same
restriction as `loop()` (which is also "in some task's context"),
but easier to forget because `onEvent` is small and focused
looking. The biggest temptation: `midi_usb_poll()` from an event
handler. It works in practice (the `midi_usb_poll` ring buffer
is interrupt-safe enough), but it's a smell. If you need to
drain the OUT queue, do it in the call site that published
the event, not in the handler.

**15. v3 hooks on usermods that aren't `Usermod` subclasses
won't be called.** All v3 hooks are virtuals on `Usermod`. If
you have a class that derives from something else, it won't get
any of this. There are no such classes in the port today, but
the rule stands.

## What we'd want to add (forward looking, not done)

These are all known extension points where the v3 infrastructure
makes the work easy, but we haven't done them because nobody has
asked yet:

- **`EffectIndexChanged` publisher.** The `EffectIndexChanged`
  enum value is in the bus; nothing publishes it yet. The
  publish site is `FX.cpp` when `setMode()` runs. A usermod
  that wants to track "the FX changed" would subscribe.

- **AutoPlaylist publishes `PresetCycleRequested`.** The event
  type is defined; `usermods/usermod_v2_auto_playlist/` doesn't
  publish it yet. The publish site is the `applyPreset` call
  inside the AutoChange branch of `change()`. Pioneer v3 could
  then drop the `prolink_presetMover` flag and subscribe to
  the event instead.

- **SilenceEntered / SoundEntered events** from AutoPlaylist.
  The `loop()` polling the silence/sound edge in
  AutoPlaylist is the kind of thing that should be an event.

- **Preset struct extensions** (quickload name, ledmap, etc.)
  Navigation helpers are done; the struct itself could carry
  more fields if any usermod needs them. ~2.5 KB more RAM if
  you add `ql[9]` and `ledmap` to `PresetMetadata`.

- **Async event delivery.** The current design is
  synchronous-fanout. If a future publisher needs to fire
  from an ISR or a hard-real-time task, an async-queue
  layer on top of the current `EventBus` is the right
  extension. Don't change the synchronous contract; add a
  parallel path.

- **The `app_queue` could be unified with `EventBus`.** Right
  now `app_queue` is a USB-only FreeRTOS queue, and `EventBus`
  is a synchronous in-call dispatch. They're different
  mechanisms for different things. Unifying them is a larger
  refactor; defer until there's a concrete need.

## Where the v3 files live

```
wled00/
  event_bus.h         # EventType enum, Event struct, EventBus class
  event_bus.cpp       # EventBus::publish — walks usermods, calls onEvent
  event_log.h         # EventLog class declaration
  event_log.cpp       # 50-entry ring buffer + toJson serializer
  fcn_declare.h       # Usermod adds onPreStateChange, onEvent virtuals
                      # UsermodManager adds onPreStateChange, onEvent fan-out
                      # UsermodManager adds getMod(i) accessor
                      # PresetMetadata adds repeat field
                      # getPresetRepeat, getNextPreset, etc. declared
  led.cpp             # onPreStateChange call before currentPreset wipe
                      # PowerEdge publish on bri zero-crossing
  playlist.cpp        # PlaylistEnded publish on finite-playlist end
  presets.cpp         # PresetApplied, PresetListMutated publishes
                      # getPresetRepeat, navigation helpers
  FX.h / FX.cpp       # getEffectDisplayCount, getEffectIdByDisplayIndex
  json.cpp            # /json/eventlog endpoint
  wled.cpp            # UsbDeviceChanged publish from MIDI callbacks

usermods/
  usermod_v2_midi/
    usermod_v2_midi.h  # removed: effect_index, ensureEffectIndex,
                       # cached_playlist_repeat, prev_currentPlaylist,
                       # prev_power_off, ensurePlaylistRepeatCache call
                       # added: onPreStateChange, onEvent overrides
    usb_host_messages.h # added midi_device_info struct in app_message_t
    midi_usb_host.cpp   # populates midi_device_info on connect
  usermod_v2_pioneer_prolink_v3/
    usermod_v2_pioneer_prolink_v3.h  # copy of v2 Pioneer with v3 hook stubs
                                     # class renamed to ProLinkUsermodV3
                                     # _name is "Pro_DJ_Link_v3"
                                     # registered in usermods_list.cpp
                                     # gated by USERMOD_PIONEER_PROLINK_V3
                                     # mutually exclusive with v2
```

## Testing the v3 work

On the P4, with `pio run -e esp32p4_8MB_troyhacks`:

1. **Hit `/json/eventlog`.** You should see a JSON array. Press
   a regular preset, then a playlist pad. You should see
   `PresetApplied` events. When the playlist ends, you should
   see `PlaylistEnded`. Power-cycle to see `PowerEdge`.

2. **Press a playlist pad.** The LED update should be instant
   (≤ 100 ms, one USB poll cycle). It used to be ~1 second; the
   `midi_usb_poll()` in the pad-press handler is what fixed
   that.

3. **MIDI usermod "100% works" but with a rare 1% glitch** where
   the child pads don't fast-blink on the very first frame of
   a playlist. Unreproducible for the user; suspected to be a
   race between the natural `stateUpdated` and the
   `midi_usb_poll()` drain. Not yet root-caused.

4. **Pioneer v3** is opt-in via `-D USERMOD_PIONEER_PROLINK_V3`
   (and remove `-D USERMOD_PIONEER_PROLINK`). Requires DJ gear
   to actually exercise; the v3 hooks log every event to
   serial. When you're ready, replace the
   `prolink_presetMover` flag handling with a subscription to
   `PresetCycleRequested` (which AutoPlaylist would publish
   after the appropriate edit).
