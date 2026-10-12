/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d.cpp The angle the map view is turned to, the turn of a world point, and how the player turns it (three_d.h). */

#include "stdafx.h"
#include "three_d.h"
#include "settings_type.h"
#include "gfx_func.h"
#include "landscape.h"
#include "map_func.h"
#include "tile_map.h"
#include "window_func.h"
#include "window_gui.h"
#include "viewport_type.h"
#include "blitter/factory.hpp"
#include "zoom_func.h"
#include "core/math_func.hpp"

#include <cmath>

#include "safeguards.h"

/** The angle the map view is turned to, in degrees, 0 to 360: 0 is north up, the game as it was. */
static float _view_yaw = 0.0f;
/** Cosine and sine of the yaw, the turn of the world about the pivot. */
static double _turn_c = 1.0, _turn_s = 0.0;
/** The pivot: the world point (world units, a tile is TILE_SIZE) the world is turned about. */
static double _pivot_x = 0.0, _pivot_y = 0.0;
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
 * Whether this viewport is drawn turned: only the main view turns, the extra
 * viewports and the ones in vehicle windows keep facing north.
 * @param vp the viewport about to be drawn
 */
bool IsViewportTurned(const Viewport &vp)
{
	if (!IsViewTurned()) return false;
	const Window *w = FindWindowById(WindowClass::MainWindow, 0);
	return w != nullptr && w->viewport != nullptr && w->viewport.get() == &vp;
}

/**
 * How many of the eight directions the view is turned by: a vehicle facing
 * direction d is drawn with its picture for direction (d - step) & 7.
 */
int TurnedDirectionStep()
{
	if (!IsViewTurned()) return 0;
	return static_cast<int>(std::lround(_view_yaw / 45.0f)) & 7;
}

/**
 * Turn a world point about the pivot by the yaw.
 * @param x,y the world point
 * @param[out] xr,yr the point of the turned world the game draws at the same place
 */
void TurnWorldPoint(double x, double y, double &xr, double &yr)
{
	double dx = x - _pivot_x, dy = y - _pivot_y;
	xr = _pivot_x + _turn_c * dx - _turn_s * dy;
	yr = _pivot_y + _turn_s * dx + _turn_c * dy;
}

/**
 * Turn a point of the turned world back: the inverse of TurnWorldPoint().
 * @param xr,yr the point of the turned world
 * @param[out] x,y the world point
 */
void UnturnWorldPoint(double xr, double yr, double &x, double &y)
{
	double dx = xr - _pivot_x, dy = yr - _pivot_y;
	x = _pivot_x + _turn_c * dx + _turn_s * dy;
	y = _pivot_y - _turn_s * dx + _turn_c * dy;
}

/**
 * Where a world point is drawn in the turned view, in virtual screen
 * coordinates (RemapCoords() of the turned point), not rounded.
 */
void ProjectTurnedF(double x, double y, double z, double &vx, double &vy)
{
	double xr, yr;
	TurnWorldPoint(x, y, xr, yr);
	vx = (yr - xr) * 2 * ZOOM_BASE;
	vy = (yr + xr - z) * ZOOM_BASE;
}

/** RemapCoords() of the turned view: where a world point is drawn. */
Point ProjectTurned(int x, int y, int z)
{
	double vx, vy;
	ProjectTurnedF(x, y, z, vx, vy);
	return {static_cast<int>(std::lround(vx)), static_cast<int>(std::lround(vy))};
}

/**
 * The world point at height z drawn at a virtual screen coordinate of the
 * turned view: the inverse of ProjectTurnedF() for a given z.
 */
void InverseTurnedFlatF(double vx, double vy, double z, double &x, double &y)
{
	double sum = vy / ZOOM_BASE + z;       // xr + yr
	double diff = vx / (2.0 * ZOOM_BASE);  // yr - xr
	double xr = (sum - diff) / 2, yr = (sum + diff) / 2;
	UnturnWorldPoint(xr, yr, x, y);
}

/**
 * The world point under a virtual screen coordinate of the turned view, with
 * the height of the ground taken in, the way InverseRemapCoords2() does it for
 * the view facing north: walk down from the top of the world and stop where
 * the line of sight first meets the ground.
 * @param x,y the virtual screen coordinate
 * @param clamp_to_map clamp the result to the map
 */
Point InverseRemapCoordsTurned(int x, int y, bool clamp_to_map)
{
	const int max_x = Map::MaxX() * TILE_SIZE - 1;
	const int max_y = Map::MaxY() * TILE_SIZE - 1;
	int top = _settings_game.construction.map_height_limit * TILE_HEIGHT;
	Point pt{};
	for (int z = top; ; z -= TILE_HEIGHT / 2) {
		if (z < 0) z = 0;
		double wx, wy;
		InverseTurnedFlatF(x, y, z, wx, wy);
		pt.x = static_cast<int>(std::floor(wx));
		pt.y = static_cast<int>(std::floor(wy));
		int ground;
		if (pt.x >= 0 && pt.x <= max_x && pt.y >= 0 && pt.y <= max_y) {
			ground = GetSlopePixelZ(pt.x, pt.y);
		} else {
			ground = GetSlopePixelZOutsideMap(pt.x, pt.y);
		}
		if (ground >= z || z == 0) break;
	}
	if (clamp_to_map) {
		pt.x = Clamp(pt.x, 0, max_x);
		pt.y = Clamp(pt.y, 0, max_y);
	}
	return pt;
}

/**
 * Turn the map view to an angle. An angle within a tenth of a degree of north
 * is north: the usual drawing path, to the pixel. The ground point under the
 * middle of the main view becomes the pivot and stays under the middle.
 * @param degrees the angle, any value; taken modulo 360
 */
void SetViewYaw(float degrees)
{
	if (!ThreeDEnabled() || BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) degrees = 0.0f;
	float yaw = std::fmod(degrees, 360.0f);
	if (yaw < 0) yaw += 360.0f;
	if (yaw < 0.1f || yaw > 359.9f) yaw = 0.0f;
	if (yaw == _view_yaw) return;

	Window *w = FindWindowById(WindowClass::MainWindow, 0);
	ViewportData *vp = (w != nullptr) ? w->viewport.get() : nullptr;
	double wx = _pivot_x, wy = _pivot_y;
	if (vp != nullptr) {
		/* The ground point under the middle of the screen, in the turn so far. */
		InverseTurnedFlatF(vp->dest_scrollpos_x + vp->virtual_width / 2, vp->dest_scrollpos_y + vp->virtual_height / 2, 0, wx, wy);
	}

	_view_yaw = yaw;
	double a = yaw * 3.14159265358979323846 / 180.0;
	_turn_c = std::cos(a);
	_turn_s = std::sin(a);
	_pivot_x = wx;
	_pivot_y = wy;

	if (vp != nullptr) {
		/* The pivot is drawn where it always was; put it back under the middle. */
		double vx, vy;
		ProjectTurnedF(wx, wy, 0, vx, vy);
		int mask = ScaleByZoom(-1, vp->zoom);
		vp->dest_scrollpos_x = vp->scrollpos_x = (static_cast<int>(std::lround(vx)) - vp->virtual_width / 2) & mask;
		vp->dest_scrollpos_y = vp->scrollpos_y = (static_cast<int>(std::lround(vy)) - vp->virtual_height / 2) & mask;
	}
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
	SetViewYaw(0.0f);
}
