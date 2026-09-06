/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file mouse_debug.h A running record of what the mouse did in the last few
 * seconds, written to a file on demand.
 *
 * The map-drag button of this feature has been read as held when nobody was
 * holding it, and the only way to see why is to have what the system said,
 * what the game remembered, and what the drag did, all on one timeline. Every
 * mouse message, every warp of the pointer, every reading of the buttons off
 * the system, every start and end of a drag is written here as it happens;
 * only the last few seconds are kept. Ctrl and the right button save them
 * beside the saved games, where the crash reports go.
 */

#ifndef MOUSE_DEBUG_H
#define MOUSE_DEBUG_H

#include <string>
#include <string_view>

/** How long a stretch of mouse history is kept, in milliseconds. */
static constexpr int MOUSE_DEBUG_KEEP_MS = 5000;

void MouseDebugLog(std::string_view what);
std::string MouseDebugSave();

#endif /* MOUSE_DEBUG_H */
