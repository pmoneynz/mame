// license:BSD-3-Clause
// copyright-holders:MPC3000 for Mac contributors
/***************************************************************************

    mpc3000_app.h

    Interface between the mpc3000 driver and a host application (the
    mpcapp OSD). The driver's root device implements it; a host finds it
    with dynamic_cast on machine().root_device(). Everything here runs on
    the emulation thread.

    App events carry an absolute emulated time. The driver fires each one
    at that time and turns it into front-panel link frames (the same
    31 250 baud serialiser as the panel HLE), for footswitches HC365
    input bits, and for MIDI IN bytes on that port's own serialiser.

***************************************************************************/

#ifndef MAME_AKAI_MPC3000_APP_H
#define MAME_AKAI_MPC3000_APP_H

#pragma once

#include "attotime.h"

#include <cstdint>
#include <functional>

class mpc3000_app_interface
{
public:
	enum event_kind : uint8_t
	{
		EVENT_KEY = 1,          // code: panel key code 0x40..0x79; value: 1 press, 0 release
		EVENT_PAD = 2,          // code: pad 1..16; value: velocity 1..127, 0 release
		EVENT_PRESSURE = 3,     // code: pad 1..16; value: 0..127
		EVENT_SLIDER = 4,       // value: 0..127
		EVENT_DIAL = 5,         // value: signed steps (int8), + is clockwise
		EVENT_FOOTSWITCH = 6,   // code: 1 or 2; value: HC365 line level 0/1
		EVENT_MIDI_IN = 7       // code: MIDI IN 1 or 2; value: one byte, sent at 31 250 baud
	};

	// MIDI OUT 1-4: called with each byte as it enters the UART's
	// transmitter (its start bit begins at `when`).
	using midi_out_sink = std::function<void (const attotime &when, uint8_t port, uint8_t byte)>;

	static constexpr unsigned EVENT_QUEUE_SIZE = 256;

	virtual ~mpc3000_app_interface() = default;

	// Queue an event at emulated time `when`. Returns false if the queue
	// is full or the event is malformed. Events in the past fire at once.
	virtual bool app_event(const attotime &when, uint8_t kind, uint8_t code, uint8_t value) = 0;

	// Events that fired after their time (queued late), since reset.
	virtual uint32_t app_late_events() const = 0;

	// LED states, bit n = LED n (MAME outputs led0..led15).
	virtual uint16_t app_leds() const = 0;

	// Receive MIDI OUT bytes (nullptr to stop).
	virtual void app_set_midi_out(midi_out_sink sink) = 0;
};

#endif // MAME_AKAI_MPC3000_APP_H
