/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d.h The 3D of this game, ours: the map view turned to any angle
 * and the previews that turn (gui.three_d, "3D: turning previews and turning
 * the map view", Game Options > Graphics, on by default).
 *
 * The map view is turned by an angle, the yaw, about the point in the middle
 * of the screen: 0 is the game as it has always been, drawn by the usual path
 * to the pixel; any other angle is drawn by the second renderer, which takes
 * what the tiles and vehicles say to draw and draws it turned. The angle is
 * nobody's but this client's: it is not in the savegame, not in the config,
 * and the game starts facing north every time. With the setting off nothing of
 * this exists.
 *
 * The player turns the view with the middle mouse button held and dragged
 * sideways (RotateViewByDrag(), 4 pixels to a degree), or with the keys Y and
 * N (15 degrees, with Ctrl 45, the next of the eight directions the vehicles
 * and our buildings have pictures for); the right button scrolls as it always
 * has. Y and N are the only two letters no window of the game binds.
 */

#ifndef THREE_D_H
#define THREE_D_H

/** A step of the keys: 15 degrees, a quarter of the way to the next of the eight directions. */
static constexpr int THREE_D_KEY_STEP_DEG = 15;
/** A step of the keys with Ctrl: the next of the eight directions. */
static constexpr int THREE_D_KEY_STEP_CTRL_DEG = 45;
/** The middle button dragged this many pixels turns the view by a degree. */
static constexpr int THREE_D_DRAG_PIXELS_PER_DEG = 4;

bool ThreeDEnabled();
float GetViewYaw();
bool IsViewTurned();
void RotateView(float degrees);
void SetViewYaw(float degrees);
void RotateViewByDrag(int pixels);
void ResetViewYaw();

#endif /* THREE_D_H */
