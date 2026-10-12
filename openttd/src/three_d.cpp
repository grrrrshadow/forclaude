/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d.cpp The angle the map view is turned to, and how the player turns it (three_d.h). */

#include "stdafx.h"
#include "three_d.h"
#include "settings_type.h"
#include "gfx_func.h"

#include <cmath>

#include "safeguards.h"

/** The angle the map view is turned to, in degrees, 0 to 360: 0 is north up, the game as it was. */
static float _view_yaw = 0.0f;
/** Pixels of a middle-button drag not yet turned into a whole degree. */
static int _drag_remainder = 0;

/** Whether the 3D of this game is switched on at all (gui.three_d). */
bool ThreeDEnabled()
{
	return _settings_client.gui.three_d;
}

/** The angle the map view is turned to, in degrees, 0 to 360. */
float GetViewYaw()
{
	return _view_yaw;
}

/** Whether the map view is turned away from north at all: if it is, the second renderer draws it. */
bool IsViewTurned()
{
	return _view_yaw != 0.0f;
}

/**
 * Turn the map view to an angle. An angle within a tenth of a degree of north
 * is north: the usual drawing path, to the pixel.
 * @param degrees the angle, any value; taken modulo 360
 */
void SetViewYaw(float degrees)
{
	if (!ThreeDEnabled()) degrees = 0.0f;
	float yaw = std::fmod(degrees, 360.0f);
	if (yaw < 0) yaw += 360.0f;
	if (yaw < 0.1f || yaw > 359.9f) yaw = 0.0f;
	if (yaw == _view_yaw) return;
	_view_yaw = yaw;
	MarkWholeScreenDirty();
}

/**
 * Turn the map view on by an angle.
 * @param degrees how far, positive to the right (clockwise seen from above)
 */
void RotateView(float degrees)
{
	if (!ThreeDEnabled()) return;
	SetViewYaw(_view_yaw + degrees);
}

/**
 * Turn the map view by a drag of the middle mouse button: THREE_D_DRAG_PIXELS_PER_DEG
 * pixels sideways make a degree, the rest is kept for the next move.
 * @param pixels how far the mouse moved sideways since the last call
 */
void RotateViewByDrag(int pixels)
{
	if (!ThreeDEnabled()) return;
	_drag_remainder += pixels;
	int degrees = _drag_remainder / THREE_D_DRAG_PIXELS_PER_DEG;
	if (degrees == 0) return;
	_drag_remainder -= degrees * THREE_D_DRAG_PIXELS_PER_DEG;
	RotateView(static_cast<float>(degrees));
}

/** North up again: a new game, the setting switched off. */
void ResetViewYaw()
{
	_drag_remainder = 0;
	if (_view_yaw == 0.0f) return;
	_view_yaw = 0.0f;
	MarkWholeScreenDirty();
}
