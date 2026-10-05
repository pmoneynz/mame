-- license:BSD-3-Clause
-- copyright-holders:MPC3000 for Mac contributors

---------------------------------------------------------------------------
--
--   mpcapp.lua
--
--   Null OSD for the MPC3000 app library (P0.3 spike): headless, no host
--   video, input or audio. Build with OSD=mpcapp NO_OPENGL=1 USE_BGFX=0.
--
---------------------------------------------------------------------------

dofile("modules.lua")


function maintargetosdoptions(_target,_subtarget)
	osdmodulestargetconf()

	configuration { }
end

BASE_TARGETOS       = "unix"

links {
	"Cocoa.framework",
	"IOKit.framework",
	"CoreAudio.framework",
	"AudioToolbox.framework",
	"AudioUnit.framework",
}
-- libbgfx is always linked by the main target; its Metal renderer needs these.
linkoptions {
	"-framework QuartzCore",
	"-weak_framework Metal",
}

project ("qtdbg_" .. _OPTIONS["osd"])
	uuid (os.uuid("qtdbg_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	dofile("mpcapp_cfg.lua")
	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "3rdparty",
	}

	qtdebuggerbuild()

project ("osd_" .. _OPTIONS["osd"])
	uuid (os.uuid("osd_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	dofile("mpcapp_cfg.lua")
	osdmodulesbuild()

	-- The bgfx renderer needs an OSD window type (window.h); a headless OSD
	-- has none. mpcappstubs.cpp registers it, and DEBUG_OSX, as unsupported,
	-- and puts a headless monitor provider in the MONITOR_MAC slot.
	excludes {
		MAME_DIR .. "src/osd/modules/render/drawbgfx.cpp",
		MAME_DIR .. "src/osd/modules/render/bgfxutil.cpp",
		MAME_DIR .. "src/osd/modules/render/bgfx/**",
		MAME_DIR .. "src/osd/modules/monitor/monitor_mac.cpp",
	}

	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "src/osd/modules/file",
		MAME_DIR .. "src/osd/modules/render",
		MAME_DIR .. "3rdparty",
		MAME_DIR .. "src/osd/mpcapp",
	}

	files {
		MAME_DIR .. "src/osd/mpcapp/mpcappmain.cpp",
		MAME_DIR .. "src/osd/mpcapp/mpcappcli.cpp",
		MAME_DIR .. "src/osd/mpcapp/mpcappstubs.cpp",
		MAME_DIR .. "src/osd/modules/osdwindow.cpp",
		MAME_DIR .. "src/osd/modules/osdwindow.h",
	}


project ("ocore_" .. _OPTIONS["osd"])
	uuid (os.uuid("ocore_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	removeflags {
		"SingleOutputDir",
	}

	dofile("mpcapp_cfg.lua")

	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		ext_includedir("asio"),
	}

	files {
		MAME_DIR .. "src/osd/asio.cpp",
		MAME_DIR .. "src/osd/asio.h",
		MAME_DIR .. "src/osd/osdcore.cpp",
		MAME_DIR .. "src/osd/osdcore.h",
		MAME_DIR .. "src/osd/osdfile.h",
		MAME_DIR .. "src/osd/strconv.cpp",
		MAME_DIR .. "src/osd/strconv.h",
		MAME_DIR .. "src/osd/osdsync.cpp",
		MAME_DIR .. "src/osd/osdsync.h",
		MAME_DIR .. "src/osd/modules/osdmodule.cpp",
		MAME_DIR .. "src/osd/modules/osdmodule.h",
		MAME_DIR .. "src/osd/modules/lib/osdlib_macosx.cpp",
		MAME_DIR .. "src/osd/modules/lib/osdlib.h",
		MAME_DIR .. "src/osd/modules/file/posixdir.cpp",
		MAME_DIR .. "src/osd/modules/file/posixfile.cpp",
		MAME_DIR .. "src/osd/modules/file/posixfile.h",
		MAME_DIR .. "src/osd/modules/file/posixptty.cpp",
		MAME_DIR .. "src/osd/modules/file/posixsocket.cpp",
	}
