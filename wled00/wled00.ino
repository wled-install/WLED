/*
 * WLED Arduino IDE compatibility file.
 * * Where has everything gone?
 * * In April 2020, the project's structure underwent a major change.
 * Global variables are now found in file "wled.h"
 * Global function declarations are found in "fcn_declare.h"
 * * Usermod compatibility: Existing wled06_usermod.ino mods should continue to work. Delete usermod.cpp.
 * New usermods should use usermod.cpp instead.
 */

#ifdef WLED_DEBUG_HEAP
void heap_caps_alloc_failed_hook(size_t requested_size, uint32_t caps, const char* function_name) {
  Serial.printf("*** %s failed to allocate %d bytes with ", function_name, requested_size);
  if (caps & (1 << 0)) Serial.print("Executable ");
  if (caps & (1 << 1)) Serial.print("32-Bit_Aligned ");
  if (caps & (1 << 2)) Serial.print("8-Bit_Aligned ");
  if (caps & (1 << 3)) Serial.print("DMA ");
  if (caps & (1 << 10)) Serial.print("SPI_RAM ");
  if (caps & (1 << 11)) Serial.print("Internal ");
  if (caps & (1 << 12)) Serial.print("Default ");
  if (caps & (1 << 13)) Serial.print("IRAM+unaligned ");
  if (caps & (1 << 14)) Serial.print("Retention_DMA ");
  if (caps & (1 << 15)) Serial.print("RTC_fast ");
  Serial.print("capabilities - largest free block: " + String(heap_caps_get_largest_free_block(caps)));

  size_t largest_free = heap_caps_get_largest_free_block(caps);
  size_t total_free = heap_caps_get_free_size(caps);
  float fragmentation = 100.0f;
  if ((largest_free > 1) && (total_free > largest_free))
    fragmentation = 100.f * (1.0f - (float(largest_free) / float(total_free)));
  Serial.print("; \t available: " + String(total_free));
  Serial.print(" (frag "); Serial.print(fragmentation, 2); Serial.println("%).");

  if (!heap_caps_check_integrity_all(false)) {
    Serial.println("*** Heap CORRUPTED: " + String(heap_caps_check_integrity_all(true)));
  }
}
#endif

#include "wled.h"

unsigned long lastMillis = 0; //WLEDMM
unsigned long loopCounter = 0; //WLEDMM

unsigned long lps = 0; // loops per second

// --------------------------------------------------------------------------
// TROYHACKS: Core 0 Migrator
// --------------------------------------------------------------------------
void setup() __attribute__((used)); // needed for -flto
void loop() __attribute__((used));  // needed for -flto

// This wrapper function runs on Core 0
// It calls setup() (which skips the check 2nd time) and then loops forever
void wledCore0Entry(void* params) {
  // 1. Run Setup (Now we are on Core 0, so it proceeds normally)
  setup();

  // 2. Run the Loop forever (Replacing the Arduino scheduler)
  while (true) {
    loop();
    // Yield to prevent Watchdog triggers (WLED handles this internally usually, but this is safe)
    vTaskDelay(1);
  }
}
// --------------------------------------------------------------------------

void setup() {
  // ----------------------------------------------------------------------
  // TROYHACKS: FORCE MIGRATION TO CORE 0
  // ----------------------------------------------------------------------
  // On the P4, Arduino usually boots on Core 1. We check here.
  if (xPortGetCoreID() == 1) {
    // Spawn a new task pinned to Core 0 to run WLED
    xTaskCreatePinnedToCore(
      wledCore0Entry,
      "wled_main",
      16384,      // 16KB Stack (WLED loop usually needs ~4-5KB)
      NULL,
      1,          // Priority 1 (Standard Arduino)
      NULL,
      0           // <--- HARD PIN TO CORE 0
    );

    // Kill this original Core 1 task so it stops interfering
    vTaskDelete(NULL);
    return;
  }
  // ----------------------------------------------------------------------

  #ifdef WLED_DEBUG_HEAP
  esp_err_t error = heap_caps_register_failed_alloc_callback(heap_caps_alloc_failed_hook);
  #endif
  WLED::instance().setup();
}

void loop() {
  //WLEDMM show loops per second
  #ifdef WLED_DEBUG
  loopCounter++;
  if (millis() - lastMillis >= 8000) {
    long delta = millis() - lastMillis;
    if (delta > 0) {
      lps = (loopCounter * 1000U) / delta;
      USER_PRINTF("%lu lps\t", lps);
      USER_PRINTF("%u fps\t", strip.getFps());
      USER_PRINTF("target frametime %dms\t", int(strip.getFrameTime()));
      USER_PRINTF("target FPS %d\n", int(strip.getTargetFps()));
    }
    lastMillis = millis();
    loopCounter = 0;
  }
  #endif
  WLED::instance().loop();
}