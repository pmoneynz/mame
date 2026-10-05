// license:BSD-3-Clause
// copyright-holders:MPC3000 for Mac contributors
/***************************************************************************

    mpcappmain.cpp

    Headless OSD and C API (libmpc3k.h) of the MPC3000 core library.

    No window, no host input, no host audio device: MAME's "none" modules
    fill those slots. When a machine was created through the API, the OSD
    also:
    - takes the DSP's 10 outputs at every sound flush (sound_manager
      observer) into a lock-free ring, waiting while the ring is above its
      target (the reader clocks the machine), and calls the audio tap;
    - forwards queued panel events to the driver (mpc3000_app_interface)
      and runs queued commands, at every flush and every video frame;
    - publishes the LCD pixels and LED states every video frame.

    mpc3k_main() is the plain MAME command line on this OSD (test harness).

***************************************************************************/

#include "emu.h"
#include "emuopts.h"
#include "main.h"
#include "render.h"
#include "screen.h"
#include "sound.h"

#include "akai/mpc3000_app.h"

#include "modules/diagnostics/diagnostics_module.h"
#include "modules/lib/osdobj_common.h"
#include "modules/lib/osdlib.h"
#include "modules/osdwindow.h"

#include "libmpc3k.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#endif


//**************************************************************************
//  MACHINE CONTEXT (one per process)
//**************************************************************************

struct mpc3k
{
	enum class command_kind { PAUSE, RESUME, SAVE, LOAD, FLOPPY };
	struct command { command_kind kind; std::string arg; };

	static constexpr unsigned EVENT_QUEUE = 1024;
	static constexpr unsigned RING_FRAMES = 1 << 15;    // 0.74 s; power of two

	// configuration (strings owned here)
	mpc3k_config config{};
	std::string rom_path, bios, work_dir, floppy;
	std::vector<std::string> extra;

	// thread
	std::thread thread;
	std::atomic<bool> started{ false };
	std::atomic<bool> stop_requested{ false };
	int exit_code = 0;

	// panel events: single producer (API), single consumer (emulation)
	mpc3k_event events[EVENT_QUEUE];
	std::atomic<uint32_t> event_head{ 0 }, event_tail{ 0 };

	// commands (not real-time)
	std::mutex command_mutex;
	std::deque<command> commands;

	// audio ring: interleaved MPC3K_AUDIO_CHANNELS floats per frame
	std::unique_ptr<float[]> ring;
	std::atomic<uint64_t> ring_write{ 0 }, ring_read{ 0 };
	// One label per block in the ring: its first ring position and absolute
	// frame index. 1024 > RING_FRAMES / 44, so it cannot overflow while the
	// frames fit. Dropped blocks have no label.
	struct block_label { uint64_t pos; uint64_t frame; };
	static constexpr unsigned LABELS = 1024;
	block_label labels[LABELS]{};
	std::atomic<uint32_t> label_head{ 0 }, label_tail{ 0 };
	std::atomic<bool> free_run{ false };
	std::atomic<bool> skip_stale{ false };      // reader drops what free-running left in the ring
	bool await_refill = true;                   // reader thread: no underruns before the first full read
	// back-pressure threshold: grows 1 ms (44 frames) per underrun, up to
	// 10 ms (SPEC.md 5.3)
	std::atomic<uint32_t> ring_target{ 132 };
	uint32_t underruns_seen = 0;                // emulation thread
#if defined(__APPLE__)
	dispatch_semaphore_t ring_space = nullptr;
#endif

	// published state
	std::atomic<uint64_t> time_ns{ 0 };
	std::atomic<uint64_t> frames{ 0 };
	std::atomic<uint16_t> leds{ 0 };
	std::atomic<uint32_t> late{ 0 }, underruns{ 0 }, dropped{ 0 };
	std::mutex lcd_mutex;
	uint8_t lcd[MPC3K_LCD_WIDTH * MPC3K_LCD_HEIGHT]{};
	uint64_t lcd_seq = 0;

	// emulation thread only
	running_machine *machine = nullptr;
	mpc3000_app_interface *app = nullptr;
	bool ready = false;             // devices started (first reset seen)
	bool exit_scheduled = false;
	std::vector<uint32_t> pixel_buffer;
};

namespace {

mpc3k *g_machine = nullptr;

attotime ns_to_attotime(uint64_t ns)
{
	return attotime(seconds_t(ns / 1'000'000'000), attoseconds_t(ns % 1'000'000'000) * ATTOSECONDS_PER_NANOSECOND);
}

uint64_t attotime_to_ns(const attotime &t)
{
	return uint64_t(t.seconds()) * 1'000'000'000 + uint64_t(t.attoseconds() / ATTOSECONDS_PER_NANOSECOND);
}

bool valid_event(const mpc3k_event &e)
{
	switch (e.kind)
	{
	case MPC3K_EVENT_KEY:        return e.code >= 0x40 && e.code <= 0x79;
	case MPC3K_EVENT_PAD:
	case MPC3K_EVENT_PRESSURE:   return e.code >= 1 && e.code <= 16 && e.value >= 0 && e.value <= 127;
	case MPC3K_EVENT_SLIDER:     return e.value >= 0 && e.value <= 127;
	case MPC3K_EVENT_DIAL:       return e.value >= -128 && e.value <= 127;
	case MPC3K_EVENT_FOOTSWITCH: return e.code >= 1 && e.code <= 2;
	default:                     return false;
	}
}


//**************************************************************************
//  EMULATION-THREAD SERVICE
//**************************************************************************

// Forward queued events to the driver; run commands; honour stop requests.
void service(mpc3k &m)
{
	running_machine &machine = *m.machine;

	if (m.app)
	{
		uint32_t head = m.event_head.load(std::memory_order_relaxed);
		const uint32_t tail = m.event_tail.load(std::memory_order_acquire);
		while (head != tail)
		{
			const mpc3k_event &e = m.events[head % mpc3k::EVENT_QUEUE];
			if (!m.app->app_event(ns_to_attotime(e.time_ns), e.kind, e.code, uint8_t(e.value)))
				break;      // driver queue full (events are validated on push): retry next time
			head++;
		}
		m.event_head.store(head, std::memory_order_release);
		m.late.store(m.app->app_late_events(), std::memory_order_relaxed);
	}

	std::deque<mpc3k::command> commands;
	{
		std::lock_guard<std::mutex> lock(m.command_mutex);
		commands.swap(m.commands);
	}
	for (mpc3k::command &c : commands)
	{
		switch (c.kind)
		{
		case mpc3k::command_kind::PAUSE:  machine.pause(); break;
		case mpc3k::command_kind::RESUME: machine.resume(); break;
		case mpc3k::command_kind::SAVE:   machine.schedule_save(std::move(c.arg)); break;
		case mpc3k::command_kind::LOAD:   machine.schedule_load(std::move(c.arg)); break;
		case mpc3k::command_kind::FLOPPY:
			for (device_image_interface &image : image_interface_enumerator(machine.root_device()))
			{
				if (image.brief_instance_name() != "flop")
					continue;
				if (c.arg.empty())
					image.unload();
				else if (auto [err, msg] = image.load(c.arg); err)
					osd_printf_error("mpc3k: cannot load floppy %s: %s\n", c.arg, msg.empty() ? err.message() : msg);
				break;
			}
			break;
		}
	}

	const bool stop_time = m.config.stop_at_ns && m.time_ns.load(std::memory_order_relaxed) >= m.config.stop_at_ns;
	if ((stop_time || m.stop_requested.load(std::memory_order_relaxed)) && !m.exit_scheduled)
	{
		m.exit_scheduled = true;
		machine.schedule_exit();
	}
}

// One sound flush: the DSP's samples since the previous flush.
void on_sound(mpc3k &m, const std::map<std::string, std::vector<std::pair<const float *, int>>> &data)
{
	m.time_ns.store(attotime_to_ns(m.machine->time()), std::memory_order_relaxed);

	auto it = data.find(":dsp");
	if (it != data.end() && it->second.size() >= MPC3K_AUDIO_CHANNELS)
	{
		const float *ch[MPC3K_AUDIO_CHANNELS];
		unsigned n = unsigned(it->second[0].second);
		for (unsigned c = 0; c < MPC3K_AUDIO_CHANNELS; c++)
		{
			ch[c] = it->second[c].first;
			n = std::min(n, unsigned(it->second[c].second));
		}
		// Absolute frame index: the DSP runs at exactly 44 100 Hz from power-on,
		// so a block ending at emulated time t ends near frame t * 44100. Blocks
		// follow on from each other; resync only on a jump (start, state load).
		const uint64_t by_time = (m.time_ns.load(std::memory_order_relaxed) * 441 + 5'000'000) / 10'000'000;
		uint64_t end = m.frames.load(std::memory_order_relaxed) + n;
		if (end > by_time + 2 || end + 2 < by_time)
			end = by_time;
		const uint64_t first = end >= n ? end - n : 0;
		if (m.config.audio_tap && n)
			m.config.audio_tap(m.config.user, ch, n, first);

		auto ring_fill = [&m] { return m.ring_write.load(std::memory_order_relaxed) - m.ring_read.load(std::memory_order_acquire); };
		auto wait_for_reader = [&m] (auto condition)
		{
			while (!m.stop_requested.load(std::memory_order_relaxed) && !m.free_run.load(std::memory_order_relaxed) && condition())
			{
#if defined(__APPLE__)
				dispatch_semaphore_wait(m.ring_space, dispatch_time(DISPATCH_TIME_NOW, 5'000'000));
#else
				std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
			}
		};

		// back-pressure: the reader clocks the machine
		const uint32_t underruns = m.underruns.load(std::memory_order_relaxed);
		if (underruns != m.underruns_seen)
		{
			const uint32_t grown = m.ring_target.load(std::memory_order_relaxed) + 44 * (underruns - m.underruns_seen);
			m.ring_target.store(std::min<uint32_t>(grown, 441), std::memory_order_relaxed);
			m.underruns_seen = underruns;
		}
		const uint32_t target = m.ring_target.load(std::memory_order_relaxed);
		wait_for_reader([&] { return ring_fill() > target; });

		const uint64_t w = m.ring_write.load(std::memory_order_relaxed);
		if (n && w + n - m.ring_read.load(std::memory_order_acquire) > mpc3k::RING_FRAMES)
		{
			// Ring full (free-running with no reader): drop the block.
			m.dropped.fetch_add(1, std::memory_order_relaxed);
		}
		else if (n)
		{
			for (unsigned i = 0; i < n; i++)
			{
				float *frame = &m.ring[((w + i) & (mpc3k::RING_FRAMES - 1)) * MPC3K_AUDIO_CHANNELS];
				for (unsigned c = 0; c < MPC3K_AUDIO_CHANNELS; c++)
					frame[c] = ch[c][i];
			}
			const uint32_t head = m.label_head.load(std::memory_order_relaxed);
			m.labels[head % mpc3k::LABELS] = { w, first };
			m.label_head.store(head + 1, std::memory_order_release);
			m.ring_write.store(w + n, std::memory_order_release);
		}
		m.frames.store(end, std::memory_order_relaxed);
	}

	service(m);
}

// One video frame: LCD pixels and LEDs.
void on_frame(mpc3k &m)
{
	running_machine &machine = *m.machine;
	if (m.app)
		m.leds.store(m.app->app_leds(), std::memory_order_relaxed);

	screen_device *screen = screen_device_enumerator(machine.root_device()).first();
	if (screen && screen->visible_area().width() == MPC3K_LCD_WIDTH && screen->visible_area().height() == MPC3K_LCD_HEIGHT)
	{
		m.pixel_buffer.resize(MPC3K_LCD_WIDTH * MPC3K_LCD_HEIGHT);
		screen->pixels(m.pixel_buffer.data());
		// Two-level LCD: threshold the red channel halfway (as emu/probe.lua).
		unsigned lo = 255, hi = 0;
		for (uint32_t p : m.pixel_buffer)
		{
			lo = std::min(lo, unsigned((p >> 16) & 0xff));
			hi = std::max(hi, unsigned((p >> 16) & 0xff));
		}
		std::lock_guard<std::mutex> lock(m.lcd_mutex);
		for (size_t i = 0; i < m.pixel_buffer.size(); i++)
			m.lcd[i] = (((m.pixel_buffer[i] >> 16) & 0xff) * 2 > lo + hi) ? 1 : 0;
		m.lcd_seq++;
	}

	service(m);
}


//**************************************************************************
//  OSD
//**************************************************************************

class mpcapp_options : public osd_options
{
public:
	mpcapp_options() : osd_options() { }
};

class mpcapp_osd_interface : public osd_common_t
{
public:
	mpcapp_osd_interface(mpcapp_options &options) : osd_common_t(options) { }

	virtual void init(running_machine &machine) override
	{
		osd_common_t::init(machine);
		osd_common_t::init_subsystems();

		if (mpc3k *m = g_machine)
		{
			m->machine = &machine;
			m->app = dynamic_cast<mpc3000_app_interface *>(&machine.root_device());
			m->exit_scheduled = false;
			m->ready = false;
			// The sound manager is created after the OSD; hook it at reset.
			machine.add_notifier(MACHINE_NOTIFY_RESET, machine_notify_delegate(&mpcapp_osd_interface::hook_sound, this));
		}
	}

	void hook_sound()
	{
		mpc3k *m = g_machine;
		if (!m || !m->machine)
			return;
		device_t *dsp = m->machine->root_device().subdevice("dsp");
		device_sound_interface *sound = nullptr;
		if (dsp && dsp->interface(sound))
			sound->set_sound_hook(true);
		m->machine->sound().set_sound_observer(
				[m] (const std::map<std::string, std::vector<std::pair<const float *, int>>> &data) { on_sound(*m, data); });
		m->ready = true;
	}

	virtual void update(bool skip_redraw) override
	{
		osd_common_t::update(skip_redraw);
		if (g_machine && g_machine->machine && g_machine->ready)
			on_frame(*g_machine);
	}

	virtual void osd_exit() override
	{
		if (g_machine && g_machine->machine)
		{
			g_machine->machine->sound().set_sound_observer(nullptr);
			g_machine->machine = nullptr;
			g_machine->app = nullptr;
			g_machine->ready = false;
		}
		osd_common_t::osd_exit();
	}

	// The core needs one render target (the first becomes the UI target).
	// Nothing draws it; LCD frames come from the screen device.
	virtual bool video_init() override
	{
		m_target = machine().render().target_alloc();
		return m_target != nullptr;
	}

	virtual void video_exit() override
	{
		if (m_target)
			machine().render().target_free(m_target);
		m_target = nullptr;
	}

	virtual void input_update(bool relative_reset) override { poll_input_modules(relative_reset); }
	virtual void check_osd_inputs() override { }

protected:
	virtual void process_events() override { }
	virtual bool has_focus() const override { return true; }

private:
	render_target *m_target = nullptr;
};

int run_frontend(std::vector<std::string> &args)
{
	mpcapp_options options;
	mpcapp_osd_interface osd(options);
	osd.register_options();
	return emulator_info::start_frontend(options, osd, args);
}

const char *const BOARD_VALUE[] = { "0", "1", "2" };        // 2, 8, 32 MB
const char *const SIMM_VALUE[] = { "0", "16", "32" };       // none, 2x1, 2x4 MB

} // anonymous namespace


// Free functions and globals every OSD provides. No windows: video_config
// stays zeroed and input focus has nothing to act on.
osd_video_config video_config;

void osd_setup_osd_specific_emu_options(emu_options &opts)
{
	opts.add_entries(osd_options::s_option_entries);
}

void osd_set_aggressive_input_focus(bool aggressive_focus)
{
}


//**************************************************************************
//  C API
//**************************************************************************

extern "C" int mpc3k_main(int argc, char **argv)
{
	std::vector<std::string> args = osd_get_command_line(argc, argv);
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	diagnostics_module::get_instance()->init_crash_diagnostics();
	return run_frontend(args);
}

extern "C" mpc3k *mpc3k_create(const mpc3k_config *config)
{
	if (g_machine || !config || !config->rom_path || !config->work_dir)
		return nullptr;

	auto m = std::make_unique<mpc3k>();
	m->config = *config;
	m->rom_path = config->rom_path;
	m->bios = config->bios ? config->bios : "vailixi";
	m->work_dir = config->work_dir;
	m->floppy = config->floppy ? config->floppy : "";
	for (int i = 0; i < config->extra_argc; i++)
		m->extra.emplace_back(config->extra_argv[i]);
	if (!m->config.sound_update_hz)
		m->config.sound_update_hz = 1000;
	if (!m->config.ring_target_frames)
		m->config.ring_target_frames = 132;
	m->config.ring_target_frames = std::min(m->config.ring_target_frames, mpc3k::RING_FRAMES / 2);

	const int board = config->board_mb == 32 ? 2 : config->board_mb == 8 ? 1 : 0;
	const int simm = config->simm_mb == 4 ? 2 : config->simm_mb == 1 ? 1 : 0;

	std::error_code ec;
	for (const char *sub : { "cfg", "nvram", "sta", "snap" })
		std::filesystem::create_directories(std::filesystem::path(m->work_dir) / sub, ec);
	if (ec)
		return nullptr;
	// Board, SIMMs, no SMPTE option, panel HLE (the panel ROM is not dumped).
	std::ofstream cfg(std::filesystem::path(m->work_dir) / "cfg" / "mpc3000.cfg");
	cfg << "<?xml version=\"1.0\"?>\n<mameconfig version=\"10\">\n    <system name=\"mpc3000\">\n        <input>\n"
		<< "            <port tag=\":CONFIG\" type=\"CONFIG\" mask=\"3\" defvalue=\"0\" value=\"" << BOARD_VALUE[board] << "\" />\n"
		<< "            <port tag=\":CONFIG\" type=\"CONFIG\" mask=\"48\" defvalue=\"0\" value=\"" << SIMM_VALUE[simm] << "\" />\n"
		<< "            <port tag=\":CONFIG\" type=\"CONFIG\" mask=\"8\" defvalue=\"0\" value=\"0\" />\n"
		<< "            <port tag=\":CONFIG\" type=\"CONFIG\" mask=\"64\" defvalue=\"0\" value=\"0\" />\n"
		<< "        </input>\n    </system>\n</mameconfig>\n";
	if (!cfg)
		return nullptr;

	m->ring = std::make_unique<float[]>(size_t(mpc3k::RING_FRAMES) * MPC3K_AUDIO_CHANNELS);
	m->free_run.store(config->free_run != 0);
	m->ring_target.store(m->config.ring_target_frames);
#if defined(__APPLE__)
	m->ring_space = dispatch_semaphore_create(0);
#endif
	g_machine = m.release();
	return g_machine;
}

extern "C" int mpc3k_start(mpc3k *m)
{
	if (!m || m->started.exchange(true))
		return -1;

	const std::filesystem::path wd(m->work_dir);
	std::vector<std::string> args = {
		"mpc3k", "mpc3000", "-bios", m->bios, "-rompath", m->rom_path,
		"-cfg_directory", (wd / "cfg").string(), "-nvram_directory", (wd / "nvram").string(),
		"-state_directory", (wd / "sta").string(), "-snapshot_directory", (wd / "snap").string(),
		"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo", "-noreadconfig",
		"-samplerate", std::to_string(MPC3K_SAMPLE_RATE),
		"-sound_update_hz", std::to_string(m->config.sound_update_hz) };
	if (!m->floppy.empty())
	{
		args.emplace_back("-flop");
		args.emplace_back(m->floppy);
	}
	args.insert(args.end(), m->extra.begin(), m->extra.end());

	m->thread = std::thread([m, args = std::move(args)] () mutable
	{
#if defined(__APPLE__)
		// SPEC.md 5.2: the emulation thread runs at user-interactive QoS (the
		// app can also join it to the output device's audio workgroup).
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
		m->exit_code = run_frontend(args);
	});
	return 0;
}

extern "C" void mpc3k_stop(mpc3k *m)
{
	if (!m)
		return;
	m->stop_requested.store(true);
#if defined(__APPLE__)
	dispatch_semaphore_signal(m->ring_space);
#endif
}

extern "C" int mpc3k_wait(mpc3k *m)
{
	if (!m)
		return -1;
	if (m->thread.joinable())
		m->thread.join();
	return m->exit_code;
}

extern "C" void mpc3k_destroy(mpc3k *m)
{
	if (!m)
		return;
	mpc3k_stop(m);
	mpc3k_wait(m);
#if defined(__APPLE__)
	if (m->ring_space)
		dispatch_release(m->ring_space);
#endif
	if (g_machine == m)
		g_machine = nullptr;
	delete m;
}

extern "C" int mpc3k_push_event(mpc3k *m, const mpc3k_event *event)
{
	if (!m || !event || !valid_event(*event))
		return -1;
	const uint32_t tail = m->event_tail.load(std::memory_order_relaxed);
	if (tail - m->event_head.load(std::memory_order_acquire) >= mpc3k::EVENT_QUEUE)
		return -1;
	m->events[tail % mpc3k::EVENT_QUEUE] = *event;
	m->event_tail.store(tail + 1, std::memory_order_release);
	return 0;
}

extern "C" uint64_t mpc3k_time_ns(mpc3k *m) { return m ? m->time_ns.load(std::memory_order_relaxed) : 0; }
extern "C" uint64_t mpc3k_frames_produced(mpc3k *m) { return m ? m->frames.load(std::memory_order_relaxed) : 0; }

extern "C" size_t mpc3k_audio_read_at(mpc3k *m, float *out, size_t frames, unsigned channels, uint64_t *first_frame)
{
	if (!m || !out || (channels != 2 && channels != MPC3K_AUDIO_CHANNELS))
		return 0;
	uint64_t r = m->ring_read.load(std::memory_order_relaxed);
	if (m->skip_stale.exchange(false, std::memory_order_acq_rel))
	{
		// Free-running keeps the oldest frames and drops new blocks once the
		// ring is full: none of it is current. Start from the next block, and
		// do not count the wait for it as an underrun.
		r = m->ring_write.load(std::memory_order_acquire);
		m->await_refill = true;
	}
	const uint64_t available = m->ring_write.load(std::memory_order_acquire) - r;
	const size_t n = size_t(std::min<uint64_t>(available, frames));
	// The label of the block holding position r (labels are written before
	// ring_write, so every frame below it has one).
	uint32_t tail = m->label_tail.load(std::memory_order_relaxed);
	const uint32_t head = m->label_head.load(std::memory_order_acquire);
	while (head - tail > 1 && m->labels[(tail + 1) % mpc3k::LABELS].pos <= r)
		tail++;
	m->label_tail.store(tail, std::memory_order_release);
	if (first_frame)
	{
		if (head != tail)
		{
			const mpc3k::block_label &l = m->labels[tail % mpc3k::LABELS];
			*first_frame = l.frame + (r - l.pos);
		}
		else
			*first_frame = m->frames.load(std::memory_order_relaxed);
	}
	for (size_t i = 0; i < n; i++)
	{
		const float *frame = &m->ring[((r + i) & (mpc3k::RING_FRAMES - 1)) * MPC3K_AUDIO_CHANNELS];
		std::memcpy(out + i * channels, frame, channels * sizeof(float));
	}
	m->ring_read.store(r + n, std::memory_order_release);
#if defined(__APPLE__)
	dispatch_semaphore_signal(m->ring_space);
#endif
	if (n == frames)
		m->await_refill = false;
	else if (!m->await_refill && m->frames.load(std::memory_order_relaxed))
		m->underruns.fetch_add(1, std::memory_order_relaxed);
	return n;
}

extern "C" size_t mpc3k_audio_read(mpc3k *m, float *out, size_t frames, unsigned channels)
{
	return mpc3k_audio_read_at(m, out, frames, channels, nullptr);
}

extern "C" void mpc3k_set_free_run(mpc3k *m, int free_run)
{
	if (!m)
		return;
	const bool was = m->free_run.exchange(free_run != 0);
	if (was && !free_run)
		m->skip_stale.store(true, std::memory_order_release);
#if defined(__APPLE__)
	dispatch_semaphore_signal(m->ring_space);
#endif
}

extern "C" size_t mpc3k_audio_available(mpc3k *m)
{
	return m ? size_t(m->ring_write.load(std::memory_order_acquire) - m->ring_read.load(std::memory_order_relaxed)) : 0;
}

extern "C" uint64_t mpc3k_lcd(mpc3k *m, uint8_t *pixels)
{
	if (!m || !pixels)
		return 0;
	std::lock_guard<std::mutex> lock(m->lcd_mutex);
	std::memcpy(pixels, m->lcd, sizeof(m->lcd));
	return m->lcd_seq;
}

extern "C" uint16_t mpc3k_leds(mpc3k *m) { return m ? m->leds.load(std::memory_order_relaxed) : 0; }
extern "C" uint32_t mpc3k_late_events(mpc3k *m) { return m ? m->late.load(std::memory_order_relaxed) : 0; }
extern "C" uint32_t mpc3k_underruns(mpc3k *m) { return m ? m->underruns.load(std::memory_order_relaxed) : 0; }
extern "C" uint32_t mpc3k_dropped_blocks(mpc3k *m) { return m ? m->dropped.load(std::memory_order_relaxed) : 0; }

namespace {

int queue_command(mpc3k *m, mpc3k::command_kind kind, std::string arg = std::string())
{
	if (!m)
		return -1;
	std::lock_guard<std::mutex> lock(m->command_mutex);
	m->commands.push_back({ kind, std::move(arg) });
	return 0;
}

} // anonymous namespace

extern "C" void mpc3k_pause(mpc3k *m, int paused)
{
	queue_command(m, paused ? mpc3k::command_kind::PAUSE : mpc3k::command_kind::RESUME);
}

extern "C" int mpc3k_save_state(mpc3k *m, const char *slot)
{
	return slot ? queue_command(m, mpc3k::command_kind::SAVE, slot) : -1;
}

extern "C" int mpc3k_load_state(mpc3k *m, const char *slot)
{
	return slot ? queue_command(m, mpc3k::command_kind::LOAD, slot) : -1;
}

extern "C" int mpc3k_floppy(mpc3k *m, const char *path)
{
	return queue_command(m, mpc3k::command_kind::FLOPPY, path ? path : "");
}
