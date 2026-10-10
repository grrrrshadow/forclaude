/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file map_drag.h Dragging the map with the right mouse button, written
 * afresh for this feature's own scroll mode (ViewportScrollMode::RMBPinned).
 *
 * The game's own drag lives in window.cpp and is left as it is for the
 * game's own four modes. This is a separate thing with one rule at its heart:
 * the drag begins with a press of the right button over the map and ends with
 * the message saying the button was let go, or with a press of the left one,
 * and with nothing else. Not with a poll of the system, not with a pause, not
 * with the pointer leaving the window. The player holds the button; the game
 * does not let go of it for him.
 *
 * The press is taken first, before anything else in the mouse loop can claim
 * it, so a press always reaches here; and from the press on, what the drag did
 * is counted -- how far the pointer tried to go, whether the map followed -- and
 * when the two disagree, or the system's own word about the button disagrees
 * with the drag, the mouse record (mouse_debug.h) is saved by itself, with the
 * reason, so that a fault shows up in a file without anyone having to press
 * anything.
 */

#ifndef MAP_DRAG_H
#define MAP_DRAG_H

#include "window_type.h"

#include <optional>
#include <string_view>

/**
 * The right button as the system's latest mouse message described it: unset
 * until the first message, then up or down as the button bits of that message
 * had it. Set by the video drivers, read against the drag here.
 */
extern std::optional<bool> _right_button_system;

bool MapDragOurs();
bool MapDragOn();
EventState MapDragMouse(bool right_press, bool left_press);
void MapDragWindowClosed(const Window *w);
void MapDragReset();
void MapDragSystemButton(bool right_down, std::string_view source);

#endif /* MAP_DRAG_H */
