// midi_usb_host.h
// USB-MIDI Host client public interface.
//
// Provided by midi_usb_host.cpp. Callers (wled.cpp's usb_task) only need to
// include this header — midi_usb_host.cpp is compiled automatically because
// the usermod's build_src_filter picks it up when USERMOD_MIDI_USB is set.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialise the USB Host client and OUT ring buffer. Idempotent.
// Call once after usb_host_lib has installed (e.g. from usb_task, after msc_host_install()).
void midi_usb_init(void);

// Drain client events, re-submit completed IN transfers, and submit any
// queued OUT packets. Call once per iteration of the same task.
void midi_usb_poll(void);

// Queue a single 3-byte USB-MIDI event (status nibble + two data bytes) for
// asynchronous submission on the OUT endpoint. Safe to call from any task.
// Returns false if the ring buffer is full or the host hasn't been initialised.
bool midi_out_queue(uint8_t status, uint8_t d1, uint8_t d2);