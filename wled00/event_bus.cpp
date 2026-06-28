// wled00/event_bus.cpp
// v3 event bus implementation.

#include "event_bus.h"
#include "event_log.h"  // WLEDMM v3: ring buffer; called first so the
                         // log captures the event even if a handler
                         // throws or returns early.
#include "wled.h"     // usermods singleton
#include "fcn_declare.h"

namespace wled {

unsigned EventBus::publish(const Event& ev) {
  // Record the event in the debug log first, so the entry exists
  // even if a usermod handler crashes or throws.
  EventLog::record(ev);

  unsigned notified = 0;
  // Walk the registered usermods in registration order. This is a
  // simple linear scan; with <WLED_MAX_USERMODS (25) entries the
  // per-event cost is negligible.
  byte count = usermods.getModCount();
  for (byte i = 0; i < count; i++) {
    Usermod* u = usermods.getMod(i);
    if (!u) continue;
    u->onEvent(ev);
    notified++;
  }
  return notified;
}

}  // namespace wled
