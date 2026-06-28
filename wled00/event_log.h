// wled00/event_log.h
// v3 debug event log — a small ring buffer of the last 50 events
// published through EventBus::publish(). Exposed as /json/eventlog.
//
// Built early (Phase 1.5) so the rest of the v3 work has a debugging
// tool. The buffer is allocated statically; ~1.6 KB of RAM.

#pragma once

#include "event_bus.h"
// Match wled.h's ARDUINOJSON_DECODE_UNICODE setting. Without this,
// the ArduinoJson namespace version tag (e.g., V6215PA2 vs V6215PB2)
// would differ between TUs that include this header directly vs.
// through wled.h, and the linker would report "undefined reference"
// even though the implementation exists. The wled.h override happens
// AFTER the first ArduinoJson include, so the macro ends up at 0 in
// any TU that includes wled.h. We force 0 here so TUs that include
// event_log.h directly (without wled.h first) match.
#define ARDUINOJSON_DECODE_UNICODE 0
// Bring in the full ArduinoJson JsonArray type so callers don't need
// a separate forward declaration. The ArduinoJson-v6 fork is bundled
// at this path in WLED (see wled.h / fcn_declare.h).
#include "src/dependencies/json/ArduinoJson-v6.h"

namespace wled {

// Log capacity. Each entry is a copy of wled::Event (currently 32 bytes
// payload + 4 bytes type+timestamp + 1 byte source = ~40 bytes). 50
// entries ~= 2 KB. Tunable.
static constexpr unsigned kEventLogCapacity = 50;

class EventLog {
 public:
  // Called by EventBus::publish() to record the event. Overwrites the
  // oldest entry when the buffer is full.
  static void record(const Event& ev);

  // Returns the number of events currently in the log (0..capacity).
  static unsigned size();

  // Append the recorded events to the given JSON array, most-recent
  // first. Each entry is an object: {type, timestamp_ms, source_id,
  // payload}. Returns the number of entries appended.
  // Optional `since_ms`: only include events with timestamp_ms >=
  // since_ms. Pass 0 to include all.
  static unsigned toJson(JsonArray& arr, unsigned long since_ms = 0);

  // Clear the log.
  static void clear();
};

}  // namespace wled
