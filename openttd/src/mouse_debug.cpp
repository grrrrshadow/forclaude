/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file mouse_debug.cpp The last few seconds of the mouse, kept and saved on demand. See mouse_debug.h. */

#include "stdafx.h"
#include "mouse_debug.h"
#include "gfx_func.h"
#include "fileio_func.h"
#include "settings_type.h"
#include "core/enum_type.hpp"
#include "3rdparty/fmt/chrono.h"

#include <chrono>
#include <deque>
#include <ctime>

#include "safeguards.h"

extern bool _scrolling_viewport;

/** One line of the record: when, and the whole state of the mouse as the game sees it, then what happened. */
struct MouseDebugEntry {
	std::chrono::steady_clock::time_point when;
	std::string line;
};

static std::deque<MouseDebugEntry> _mouse_debug;
static std::chrono::steady_clock::time_point _mouse_debug_start;
static bool _mouse_debug_started = false;

/**
 * Write one event down, with the state the game holds at that moment.
 *
 * The state is what makes a line worth reading: the remembered buttons, the
 * pointer, the pin, and whether a drag is on. The event alone says what
 * arrived; the state says what the game made of it.
 *
 * @param what the event, in the words of the place that saw it
 */
void MouseDebugLog(std::string_view what)
{
	auto now = std::chrono::steady_clock::now();
	if (!_mouse_debug_started) {
		_mouse_debug_start = now;
		_mouse_debug_started = true;
	}

	/* Only the last few seconds are kept; older lines go as new ones come. */
	while (!_mouse_debug.empty() && now - _mouse_debug.front().when > std::chrono::milliseconds(MOUSE_DEBUG_KEEP_MS)) {
		_mouse_debug.pop_front();
	}
	/* And never more than this many, whatever the clock says: a drag at a
	 * high frame rate writes a line a frame. Raised along with the stretch of
	 * time above, so that the count is the safety catch it was meant to be and
	 * not what decides how far back the record goes. */
	while (_mouse_debug.size() >= 60000) _mouse_debug.pop_front();

	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - _mouse_debug_start).count();
	_mouse_debug.push_back({now, fmt::format("{:>9} ms  L={} R={} Rclk={} pos=({},{}) delta=({},{}) pin={} drag={} mode={}  {}",
			ms,
			_left_button_down ? 1 : 0, _right_button_down ? 1 : 0, _right_button_clicked ? 1 : 0,
			_cursor.pos.x, _cursor.pos.y, _cursor.delta.x, _cursor.delta.y,
			_cursor.fix_at ? 1 : 0, _scrolling_viewport ? 1 : 0,
			to_underlying(_settings_client.gui.scroll_mode),
			what)});
}

/**
 * Save the record beside the saved games, where the crash reports go.
 * @return the name of the file written, or an empty string if it could not be
 */
std::string MouseDebugSave()
{
	MouseDebugLog("--- ulozeni debugu mysi ---");

	std::string name = fmt::format("{}mouse{:%Y%m%d%H%M%S}.log", FioFindDirectory(Subdirectory::Save), fmt::gmtime(time(nullptr)));
	auto file = FioFOpenFile(name, "w", Subdirectory::None);
	if (!file.has_value()) return {};

	fmt::print(*file, "Mouse debug: last {} ms before the save. L/R = buttons as the game remembers them, Rclk = right click waiting to be handled, pin = pointer pinned, drag = map drag on, mode = scroll mode setting (0 = ours).\n", MOUSE_DEBUG_KEEP_MS);
	for (const MouseDebugEntry &e : _mouse_debug) {
		fmt::print(*file, "{}\n", e.line);
	}
	return name;
}
