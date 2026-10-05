// license:BSD-3-Clause
// copyright-holders:MPC3000 for Mac contributors
/***************************************************************************

    mpcappmain.cpp

    Null OSD for the MPC3000 app library (P0.3 spike, SPEC.md section 10
    C4/C7). No window, no host input, no host audio device: MAME's "none"
    modules fill every slot. The app links the core through mpc3k_main();
    the CLI in mpcappcli.cpp is only a thin host for the harness.

***************************************************************************/

#include "emu.h"
#include "emuopts.h"
#include "main.h"
#include "render.h"

#include "modules/diagnostics/diagnostics_module.h"
#include "modules/lib/osdobj_common.h"
#include "modules/lib/osdlib.h"
#include "modules/osdwindow.h"

#include <cstdio>

namespace {

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
	}

	// The core needs one render target (the first becomes the UI target).
	// Nothing draws it here; the app will read LCD frames from it (C4).
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


extern "C" int mpc3k_main(int argc, char **argv)
{
	std::vector<std::string> args = osd_get_command_line(argc, argv);

	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	diagnostics_module::get_instance()->init_crash_diagnostics();

	mpcapp_options options;
	mpcapp_osd_interface osd(options);
	osd.register_options();
	return emulator_info::start_frontend(options, osd, args);
}
