// license:BSD-3-Clause
// copyright-holders:MPC3000 for Mac contributors
// Modules osd_common_t registers that a headless OSD does not build, and a
// headless monitor provider (osd_common_t needs one that initialises).

#include "modules/osdmodule.h"
#include "modules/debugger/debug_module.h"
#include "modules/monitor/monitor_common.h"
#include "modules/render/render_module.h"

#include "osdepend.h"

namespace osd {
namespace {

MODULE_NOT_SUPPORTED(video_bgfx_absent, OSD_RENDERER_PROVIDER, "bgfx")
MODULE_NOT_SUPPORTED(debug_osx_absent, OSD_DEBUG_PROVIDER, "osx")

// One fixed 240x64 "monitor": nothing is ever drawn to it.
class headless_monitor_info : public osd_monitor_info
{
public:
	headless_monitor_info(monitor_module &module)
		: osd_monitor_info(module, 1, "screen0", 240.0f / 64.0f)
	{
		headless_monitor_info::refresh();
	}

private:
	virtual void refresh() override
	{
		m_pos_size = m_usuable_pos_size = osd_rect(0, 0, 240, 64);
		m_is_primary = true;
	}
};

class headless_monitor_module : public monitor_module_base
{
public:
	headless_monitor_module() : monitor_module_base(OSD_MONITOR_PROVIDER, "headless") { }

	virtual std::shared_ptr<osd_monitor_info> monitor_from_rect(const osd_rect &rect) override { return m_monitor; }
	virtual std::shared_ptr<osd_monitor_info> monitor_from_window(const osd_window &window) override { return m_monitor; }

protected:
	virtual int init_internal(const osd_options &options) override
	{
		m_monitor = std::make_shared<headless_monitor_info>(*this);
		add_monitor(m_monitor);
		return 0;
	}

private:
	std::shared_ptr<osd_monitor_info> m_monitor;
};

} // anonymous namespace
} // namespace osd

// Registered in the MONITOR_MAC slot; monitor_mac.cpp is excluded from the build.
MODULE_DEFINITION(MONITOR_MAC, osd::headless_monitor_module)

MODULE_DEFINITION(RENDERER_BGFX, osd::video_bgfx_absent)
MODULE_DEFINITION(DEBUG_OSX, osd::debug_osx_absent)
