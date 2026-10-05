-- license:BSD-3-Clause
-- copyright-holders:MPC3000 for Mac contributors

dofile('modules.lua')

defines {
	"OSD_MPCAPP",
	"SDLMAME_UNIX",
	"SDLMAME_MACOSX",
	"SDLMAME_DARWIN",
}

configuration { "osx*" }
	includedirs {
		MAME_DIR .. "3rdparty/bx/include/compat/osx",
	}

configuration { }
