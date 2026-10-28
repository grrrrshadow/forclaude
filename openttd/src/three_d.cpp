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

#include "table/three_d_houses.h"

#include "safeguards.h"

/**
 * The boxes of a house of the original graphics, if it has a hand table.
 * @param house the id of the house's north tile
 * @return its table, or nullptr for a house without one (drawn as a billboard)
 */
const ThreeDHouse *GetThreeDHouse(HouseID house)
{
	for (const ThreeDHouse &h : _three_d_houses) {
		if (h.id == house) return &h;
	}
	return nullptr;
}

/** The parts of a house from GetThreeDHouse(): house->count boxes. */
const ThreeDPart *GetThreeDHouseParts(const ThreeDHouse *house)
{
	return _three_d_house_parts + house->first;
}

/**
 * The angle the map view is turned to, in degrees, -180 to 180: 0 is north
 * up, the game as it was; positive to the right. Past 180 it goes on at -180:
 * the view turns round and round without a stop (the player: the stops at
 * 180 either way were in the way).
 */
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

/** The angle the map view is turned to, in degrees, 0 to 360 (the drawing counts it that way round). */
float GetViewYaw()
{
	return _view_yaw < 0.0f ? _view_yaw + 360.0f : _view_yaw;
}

/** The angle as the player sees it: -180 to 180, positive to the right. */
float GetViewYawSigned()
{
	return _view_yaw;
}

/** Whether the map view is turned away from north at all: if it is, the second renderer draws it. */
bool IsViewTurned()
{
	return _view_yaw != 0.0f;
}

/**
 * Whether the main view is drawn by the second renderer: with the 3D on, at every angle, north too (the player,
 * 28. 10.: angle 0 is only another camera, "3D without the choice of the old way"). Not on 8 bpp, where the view
 * does not turn (SetViewYaw()).
 */
bool IsViewDrawnTurned()
{
	return ThreeDEnabled() && BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 32;
}

/**
 * Whether this viewport is drawn by the second renderer (IsViewDrawnTurned()): only the main view, the extra
 * viewports and the ones in vehicle windows keep facing north, drawn as the game drew them.
 * @param vp the viewport about to be drawn
 */
bool IsViewportTurned(const Viewport &vp)
{
	if (!IsViewDrawnTurned()) return false;
	const Window *w = FindWindowById(WindowClass::MainWindow, 0);
	return w != nullptr && w->viewport != nullptr && w->viewport.get() == &vp;
}

/**
 * Whether this viewport is turned away from north (IsViewportTurned() and IsViewTurned()): its points elsewhere on
 * the screen. Facing north the second renderer draws them where the game always did, so finding the tile under the
 * mouse, keeping the view on the map and marking what to repaint stay as they were.
 * @param vp the viewport
 */
bool IsViewportTurnedAway(const Viewport &vp)
{
	return IsViewTurned() && IsViewportTurned(vp);
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

/** The turn of the view: its cosine and sine, and the world point it turns about (TurnWorldPoint()). */
void GetViewTurn(double &c, double &s, double &px, double &py)
{
	c = _turn_c;
	s = _turn_s;
	px = _pivot_x;
	py = _pivot_y;
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

/** Where what is being marked dirty is (DirtyPlaceScope). */
static DirtyPlace _dirty_place;
/** For the rig (test3d smycka): how many places of each kind were marked dirty turned, and how big (pixels at the usual zoom). */
uint64_t _turned_dirty_count[3] = {};
double _turned_dirty_area[3] = {};

DirtyPlaceScope::DirtyPlaceScope(const DirtyPlace &place) : was(_dirty_place)
{
	_dirty_place = place;
}

DirtyPlaceScope::~DirtyPlaceScope()
{
	_dirty_place = this->was;
}

/**
 * Where a rectangle of the view facing north lands in the turned view: the
 * game marks what to repaint in the coordinates of the view facing north
 * (MarkAllViewportsDirty(): a vehicle that moved, a tile that changed), and
 * turned, those places are drawn elsewhere on the screen -- without this a
 * vehicle far from the pivot moved out of its old picture and was not drawn
 * at its new place until something else repainted it.
 *
 * Where the thing is in the world is known (DirtyPlaceScope): a box of the
 * world, its corners turned and projected; or a point it is drawn round, the
 * rectangle moved with the point. Not known: a point of the north view, taken
 * as a point of the ground, turned about the pivot and projected again. Its
 * height is not known then; it moves a point by at most twice its height either
 * way under the turn, so the rectangle grows by that much for anything up to
 * the heights vehicles fly at -- most of the screen at the usual zoom (the
 * player: the mouse jerky and everything slow while something moved).
 * @param[in,out] left,top,right,bottom the rectangle, virtual screen coordinates (ZoomLevel::Min)
 */
void TurnDirtyRect(int &left, int &top, int &right, int &bottom)
{
	double nl = std::numeric_limits<double>::infinity(), nt = nl, nr = -nl, nb = -nl;
	auto take = [&](double tx, double ty) {
		nl = std::min(nl, tx); nr = std::max(nr, tx);
		nt = std::min(nt, ty); nb = std::max(nb, ty);
	};
	const DirtyPlace &p = _dirty_place;
	double margin;
	if (p.kind == DirtyPlace::Kind::Box) {
		for (double x : {p.x0, p.x1}) {
			for (double y : {p.y0, p.y1}) {
				for (double z : {p.z0, p.z1}) {
					double tx, ty;
					ProjectTurnedF(x, y, z, tx, ty);
					take(tx, ty);
				}
			}
		}
		margin = 2 * ZOOM_BASE;
	} else if (p.kind == DirtyPlace::Kind::Anchored) {
		/* Drawn round its point in the turned view as round it in the north view (RemapCoords()). */
		const double nx = (p.ay - p.ax) * 2 * ZOOM_BASE, ny = (p.ax + p.ay - p.az) * ZOOM_BASE;
		double tx, ty;
		ProjectTurnedF(p.ax, p.ay, p.az, tx, ty);
		take(left + tx - nx, top + ty - ny);
		take(right + tx - nx, bottom + ty - ny);
		/* A step it made since it was drawn, and a round curve drawing it beside its point (round_track.cpp). */
		margin = 16 * ZOOM_BASE;
	} else {
		const double max_height = 256.0; // world units: higher than the ground and the aircraft go
		const double xs[2] = {static_cast<double>(left), static_cast<double>(right)};
		const double ys[2] = {static_cast<double>(top), static_cast<double>(bottom)};
		for (double vx : xs) {
			for (double vy : ys) {
				/* The ground point the north view draws here (the inverse of RemapCoords() at z = 0). */
				double sum = vy / ZOOM_BASE, diff = vx / (2.0 * ZOOM_BASE);
				double x = (sum - diff) / 2, y = (sum + diff) / 2;
				double tx, ty;
				ProjectTurnedF(x, y, 0, tx, ty);
				take(tx, ty);
			}
		}
		margin = 2 * max_height * ZOOM_BASE;
	}
	left = static_cast<int>(std::floor(nl - margin));
	right = static_cast<int>(std::ceil(nr + margin));
	top = static_cast<int>(std::floor(nt - margin));
	bottom = static_cast<int>(std::ceil(nb + margin));
	_turned_dirty_count[to_underlying(p.kind)]++;
	_turned_dirty_area[to_underlying(p.kind)] += static_cast<double>(right - left) * (bottom - top) / (ZOOM_BASE * ZOOM_BASE);
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
	if (yaw > 180.0f) yaw -= 360.0f;
	if (yaw < -180.0f) yaw += 360.0f;
	if (std::abs(yaw) < 0.1f) yaw = 0.0f;
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

/**
 * A construction toolbar opened or one of its tools picked: the view goes back to north, where the player builds
 * (the player, 15. 10.: "when he clicks construction, put the view straight to 0 so he can build"; angle 0 is for
 * building, turning is for looking). Nothing when the view faces north already.
 */
void ThreeDViewForBuilding()
{
	if (IsViewTurned()) ResetViewYaw();
}
