/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file map_drag.cpp Dragging the map with the right mouse button, this feature's own way. See map_drag.h. */

#include "stdafx.h"
#include "map_drag.h"
#include "openttd.h"
#include "window_gui.h"
#include "window_func.h"
#include "viewport_func.h"
#include "vehicle_base.h"
#include "progress.h"
#include "gfx_func.h"
#include "settings_type.h"
#include "error.h"
#include "strings_func.h"
#include "mouse_debug.h"

#include "table/strings.h"

#include <chrono>

#include "safeguards.h"

std::optional<bool> _right_button_system;

/** How far the pointer has to have tried to go, in pixels, before the map is expected to have followed. */
static constexpr int MAP_DRAG_EXPECT_MOVE_PX = 24;

/**
 * How far the pointer may travel with the system calling the right button
 * held and no press having arrived, before that is written down as a
 * contradiction. Generous on purpose: under Winlator the system has been seen
 * to go on calling the button held for seconds after the hand let go, and a
 * file after every drag would be noise, not evidence.
 */
static constexpr int MAP_DRAG_SYSTEM_DOWN_PX = 300;

/** The record is saved by itself at most this often; the reasons in between still go into it. */
static constexpr std::chrono::seconds MAP_DRAG_REPORT_GAP{30};

/** The one drag there can be, and what it has done so far. */
struct MapDrag {
	Window *window = nullptr; ///< The window whose viewport is being dragged; nullptr when no drag is on.
	std::chrono::steady_clock::time_point began{}; ///< When the right button was pressed.
	int pointer_px = 0; ///< How far the pointer has tried to go since the press, in pixels; it is pinned, so this is the sum of the deltas.
	int map_steps = 0; ///< In how many ticks the viewport's destination actually moved.
	bool checked_moving = false; ///< Whether the check "did the map set off" has been made for this drag.
	bool reported_system_up = false; ///< Whether "the system calls the button up while the drag is on" has been reported for this drag.
};

static MapDrag _drag;

static int _system_down_px = 0; ///< The pointer's travel since the system began calling the right button held with no press having arrived.
static bool _reported_system_down = false; ///< Whether that has been reported for the current such stretch.
static bool _ever_reported = false; ///< Whether the record has ever been saved by itself.
static std::chrono::steady_clock::time_point _last_report{}; ///< When it last was.

/** Is the map drag the one written here, as against the game's own four? */
bool MapDragOurs()
{
	return _settings_client.gui.scroll_mode == ViewportScrollMode::RMBPinned;
}

/** Is a drag of ours under way? */
bool MapDragOn()
{
	return _drag.window != nullptr;
}

static int64_t MsSince(std::chrono::steady_clock::time_point then)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - then).count();
}

/**
 * Something about the button and the map does not add up: save the mouse
 * record by itself, with the reason, beside the saved games.
 *
 * The reason goes into the record first, so the file ends with it, and onto
 * the screen with the file's name. Saved at most once every half minute;
 * between saves the reasons are still written into the record, so the next
 * file carries them.
 *
 * @param reason what did not add up, in the words of the place that saw it
 */
static void MapDragReport(std::string_view reason)
{
	MouseDebugLog(fmt::format("!!! ROZPOR: {}", reason));

	auto now = std::chrono::steady_clock::now();
	if (_ever_reported && now - _last_report < MAP_DRAG_REPORT_GAP) {
		MouseDebugLog("(zaznam se ted neuklada, posledni ulozeni je mladsi nez 30 s)");
		return;
	}
	_ever_reported = true;
	_last_report = now;

	std::string saved = MouseDebugSave();
	if (saved.empty()) {
		ShowErrorMessage(GetEncodedString(STR_MOUSE_DEBUG_FAILED), {}, WarningLevel::Error);
	} else {
		ShowErrorMessage(GetEncodedString(STR_MOUSE_DEBUG_SAVED_AUTO, saved), GetEncodedString(STR_JUST_RAW_STRING, reason), WarningLevel::Info);
	}
}

/**
 * The right button was pressed: begin the drag if the press is over the map.
 *
 * The pointer is where the game last saw it, which is where the press was
 * made; during a drag that is the pin, so a press that arrives while a drag is
 * on begins again from the pin. Only a viewport takes the drag: a press over
 * anything else is an ordinary right click and is left to the game.
 *
 * @return whether the drag began
 */
static bool MapDragBegin()
{
	int x = _cursor.pos.x;
	int y = _cursor.pos.y;

	Window *w = FindWindowFromPt(x, y);
	if (w == nullptr) {
		MouseDebugLog(fmt::format("stisk praveho v ({},{}): zadne okno pod ukazatelem, tazeni nezacalo", x, y));
		return false;
	}

	Viewport *vp = IsPtInWindowViewport(w, x, y);
	if (vp == nullptr) {
		MouseDebugLog(fmt::format("stisk praveho v ({},{}): nad oknem tridy {} mimo mapu, obycejny pravy klik", x, y, to_underlying(w->window_class)));
		return false;
	}

	if (_game_mode == GameMode::Menu || HasModalProgress()) {
		MouseDebugLog("stisk praveho nad mapou: hlavni nabidka nebo prubeh generovani, tazeni nezacalo");
		return false;
	}

	if (w->flags.Test(WindowFlag::DisableVpScroll)) {
		MouseDebugLog(fmt::format("stisk praveho nad mapou okna tridy {}: okno ma tazeni mapy vypnute, obycejny pravy klik", to_underlying(w->window_class)));
		return false;
	}

	if (_left_button_down) {
		/* The left button is remembered down: either the hand is on both, or
		 * the left one's release went missing. Either way no drag, and the
		 * record says so. */
		MapDragReport("stisk praveho nad mapou pri drzenem levem: tazeni nezacalo");
		return false;
	}

	/* An extra viewport window comes to the front on the press, as any window
	 * does on a click; the main window is at the back for good. A modal child
	 * of the window refuses the press altogether. */
	if (!MaybeBringWindowToFront(w)) {
		MouseDebugLog(fmt::format("stisk praveho nad mapou okna tridy {}: okno ma modalniho potomka, tazeni nezacalo", to_underlying(w->window_class)));
		return false;
	}

	_drag = {};
	_drag.window = w;
	_drag.began = std::chrono::steady_clock::now();

	/* The pin: the drawn cursor stays here and the video driver puts the
	 * system's pointer back here on every move, so each move message carries
	 * only how far the hand went since the last one. */
	_cursor.fix_at = true;

	MouseDebugLog(fmt::format("tazeni (nase): zacatek stiskem praveho v ({},{}) nad oknem tridy {}", x, y, to_underlying(w->window_class)));
	return true;
}

/**
 * The drag is over.
 * @param why how it ended, for the record
 */
static void MapDragEnd(std::string_view why)
{
	MouseDebugLog(fmt::format("tazeni (nase): konec - {} (drzeno {} ms, ukazatel {} px, mapa {} kroku)", why, MsSince(_drag.began), _drag.pointer_px, _drag.map_steps));
	_drag = {};
	_cursor.fix_at = false;
}

/**
 * One tick of the drag: move the map by what the pointer did since the last
 * one, and check that it did move.
 */
static void MapDragMove()
{
	Window *w = _drag.window;
	Point delta = _cursor.delta;

	if (delta.x != 0 || delta.y != 0) {
		if (abs(delta.x) > _screen.width / 2 || abs(delta.y) > _screen.height / 2) {
			/* Half the screen in one message is not a hand on a mouse; it is
			 * the pointer being put somewhere. Flinging the map that far is
			 * exactly what the player complained of. */
			MouseDebugLog(fmt::format("tazeni (nase): skok ukazatele o ({},{}) neni tah rukou, vynechan", delta.x, delta.y));
		} else {
			_drag.pointer_px += abs(delta.x) + abs(delta.y);

			if (w == GetMainWindow() && w->viewport->follow_vehicle != VehicleID::Invalid()) {
				/* A view that is following a vehicle lets go of it first, as the game's own drag does. */
				const Vehicle *veh = Vehicle::Get(w->viewport->follow_vehicle)->GetMovingFront();
				ScrollMainWindowTo(veh->x_pos, veh->y_pos, veh->z_pos, true);
			}

			/* The view follows the hand: the pointer going right takes the
			 * view right over the ground, as this mode has always done. */
			int32_t before_x = w->viewport->dest_scrollpos_x;
			int32_t before_y = w->viewport->dest_scrollpos_y;
			w->OnScroll(delta);
			if (w->viewport->dest_scrollpos_x != before_x || w->viewport->dest_scrollpos_y != before_y) {
				_drag.map_steps++;
			} else {
				MouseDebugLog(fmt::format("tazeni (nase): tah o ({},{}) a mapa se nepohnula", delta.x, delta.y));
			}
		}
	}

	/* The check the player asked for: a press, the hand moving, and the map
	 * standing still is a fault, and the record is saved the moment it shows,
	 * not when somebody gets round to pressing Ctrl and Alt. Once per drag. */
	if (!_drag.checked_moving) {
		if (_drag.map_steps > 0) {
			_drag.checked_moving = true;
			MouseDebugLog(fmt::format("tazeni (nase): mapa se rozjela ({} ms po stisku, ukazatel {} px)", MsSince(_drag.began), _drag.pointer_px));
		} else if (_drag.pointer_px >= MAP_DRAG_EXPECT_MOVE_PX) {
			_drag.checked_moving = true;
			MapDragReport(fmt::format("mapa se nerozjela: ukazatel ujel {} px za {} ms od stisku a mapa stoji", _drag.pointer_px, MsSince(_drag.began)));
		}
	}
}

/**
 * Hold what the system's latest message says about the right button against
 * what the drag is doing. Neither way round changes the drag: the player
 * holds the button and the game does not let go of it for him, and a press
 * the game never got is not a press. Both ways round go into the record.
 */
static void MapDragCheckSystem()
{
	if (!_right_button_system.has_value()) return;
	bool system_down = *_right_button_system;

	if (_drag.window != nullptr) {
		_system_down_px = 0;
		_reported_system_down = false;

		/* The map moving under a button the system itself calls up is the
		 * fault as the player sees it: the on-screen button is not lit and
		 * the map goes on. The release never reached the game. */
		if (!system_down && _right_button_down && !_drag.reported_system_up) {
			_drag.reported_system_up = true;
			MapDragReport(fmt::format("mapa jede a system hlasi prave tlacitko nahore ({} ms po stisku): zprava o pusteni se ztratila; tazeni jede dal, hra tlacitko nepousti", MsSince(_drag.began)));
		}
		return;
	}

	/* No drag: the system calling the button held when no press has arrived.
	 * Counted by how far the pointer goes meanwhile, since a moment of it is
	 * nothing and a long stretch of it is a press the game never got. */
	if (system_down && !_right_button_down) {
		_system_down_px += abs(_cursor.delta.x) + abs(_cursor.delta.y);
		if (_system_down_px >= MAP_DRAG_SYSTEM_DOWN_PX && !_reported_system_down) {
			_reported_system_down = true;
			MapDragReport(fmt::format("system hlasi prave tlacitko dole a stisk neprisel: ukazatel ujel {} px bez tazeni", _system_down_px));
		}
	} else {
		_system_down_px = 0;
		_reported_system_down = false;
	}
}

/**
 * The mouse, once a tick and on every mouse message, ahead of everything else
 * in the mouse loop, so that nothing can claim a press before it gets here.
 *
 * The drag begins on a press of the right button over the map. It ends on the
 * message that the right button was let go, or on a press of the left button
 * -- the way out should a release ever go missing -- and on nothing else. A
 * press of the right button while the drag is on means the hand let go in
 * between and the game never heard: the record is saved, and the drag begins
 * again from the pin.
 *
 * @param right_press the right button was pressed since the last call
 * @param left_press the left button was pressed since the last call
 * @return Handled while the drag has the mouse to itself; NotHandled lets the
 *         game's own mouse loop go on with the event
 */
EventState MapDragMouse(bool right_press, bool left_press)
{
	if (!MapDragOurs()) {
		if (_drag.window != nullptr) MapDragEnd("zmena nastaveni posouvani");
		return EventState::NotHandled;
	}

	MapDragCheckSystem();

	if (_drag.window == nullptr) {
		if (!right_press) return EventState::NotHandled;
		return MapDragBegin() ? EventState::Handled : EventState::NotHandled;
	}

	if (left_press) {
		MapDragEnd("stisk leveho");
		return EventState::NotHandled; // the left click goes on to whatever it was for
	}

	if (!_right_button_down) {
		MapDragEnd("prave pusteno");
		return EventState::NotHandled;
	}

	if (right_press) {
		MapDragReport(fmt::format("stisk praveho behem tazeni ({} ms po jeho zacatku): zprava o pusteni se ztratila; tazeni zacina znovu", MsSince(_drag.began)));
		MapDragEnd("novy stisk praveho");
		return MapDragBegin() ? EventState::Handled : EventState::NotHandled;
	}

	MapDragMove();
	return EventState::Handled;
}

/**
 * A window is closing; a drag of its viewport is over.
 * @param w the window
 */
void MapDragWindowClosed(const Window *w)
{
	if (_drag.window == w) MapDragEnd("okno zavreno");
}

/** The window system starts afresh; so does the drag. */
void MapDragReset()
{
	if (_drag.window != nullptr) MapDragEnd("okna znovu zalozena");
	_system_down_px = 0;
	_reported_system_down = false;
}

/**
 * What the system's latest mouse message says about the right button, from
 * the video driver. Written into the record when it changes; every line of
 * the record carries it anyway.
 * @param right_down the message's button bits have the right button down
 * @param source the message, for the record
 */
void MapDragSystemButton(bool right_down, std::string_view source)
{
	bool changed = !_right_button_system.has_value() || *_right_button_system != right_down;
	_right_button_system = right_down;
	if (changed) MouseDebugLog(fmt::format("system ({}): prave tlacitko {}", source, right_down ? "dole" : "nahore"));
}
