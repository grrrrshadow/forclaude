/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file orbit_view.cpp The camera going round a place over the map: the second axis of turning the map (the player,
 * test4: "when I turn the map I hold the button and move left and right; moving up and down would be the view on the
 * roof from above, or from the ground the view from the side, the second axis of going round the object"; of the two
 * ways offered, "we do it properly, 1").
 *
 * The map, turned or not, is the game's projection, 30 degrees down, and the pictures of the GRFs (buildings, trees,
 * stations) are drawn for that angle only. The button that turns the map dragged up or down leaves the map for the
 * camera going round the place in the middle of it, drawn as the view from the eyes is (DrawOrbitView() of
 * viewport.cpp: our 3D models, the rest standing pictures), its look tipped down for real (CabEye::sp): up, from over
 * the roofs to straight down; down, to the ground from the side. Its place and its turn are the map's: the right button
 * moves the map under it as ever and the place with it, turning the map turns it. The wheel brings it nearer or farther.
 * Dragged back to the map's angle, or the button clicked twice, the map shows again. It is only to look at: a click on
 * it builds and picks nothing. Not in the savegame or the config, as the map's turn.
 */

#include "stdafx.h"
#include "orbit_view.h"
#include "cab_view.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "window_gui.h"
#include "window_func.h"
#include "viewport_func.h"
#include "zoom_func.h"
#include "openttd.h"
#include "map_func.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "core/format.hpp"
#include "video/video_driver.hpp"
#include "widgets/main_widget.h"

#include <numbers>

#include "safeguards.h"

bool DrawOrbitView(double x, double y, double z, double heading, double tip, double distance, int width, int height, CabViewCamera &camera, CabViewPicture &out);
void HideTurnedMapOnCard();
bool FirstPersonFull();

static constexpr double ORBIT_MAP_TIP_DEG = 30.0; ///< the map's own look down: the game's projection
static constexpr double ORBIT_LOWEST_DEG = 2.0;   ///< the camera down to the ground: so far over the level at least
static constexpr double ORBIT_HIGHEST_DEG = 90.0; ///< and over the roofs: straight down at most
static constexpr int ORBIT_LEAVE_MAP_PX = 16;     ///< the drag leaves the map after so many pixels more up or down than sideways (not the hand's wobble while turning)
static constexpr double ORBIT_NEAREST_M = 10.0;   ///< the camera this near the place at least
static constexpr double ORBIT_FARTHEST_M = 300.0; ///< and this far at most: farther the view from the eyes draws the far land only
static constexpr double ORBIT_WHEEL_STEP = 1.2;   ///< a notch of the wheel: so much nearer or farther

/** The camera going round a place, and its picture. */
struct OrbitView {
	bool on = false;
	double tip_deg = ORBIT_MAP_TIP_DEG; ///< how far it looks down
	int side = 0;                       ///< over the map's angle (1) or under it (-1): back across it, the map shows again
	double distance_m = 100;            ///< how far it is from the place
	int drag_y = 0;                     ///< the drag's pixels up or down not turned into degrees yet
	int drag_x_sum = 0;                 ///< over the map: the drag's pixels sideways so far
	int drag_y_sum = 0;                 ///< and up or down
	CabViewCamera camera;
	CabViewPicture picture;
	bool picture_ok = false;
	double x = 0, y = 0, z = 0, heading = 0; ///< the place and the look of the last picture (the rig)
};
static OrbitView _orbit;

/** Whether the camera going round a place covers the map now. */
bool OrbitViewOn()
{
	return _orbit.on;
}

/** The place in the middle of the main view, on the ground: world units, z the game's units of height. */
static void OrbitPlace(const Viewport &vp, double &x, double &y, double &z)
{
	const double vx = vp.virtual_left + vp.virtual_width / 2.0, vy = vp.virtual_top + vp.virtual_height / 2.0;
	x = 0;
	y = 0;
	z = 0;
	for (int k = 0; k < 3; k++) {
		InverseTurnedFlatF(vx, vy, z, x, y);
		x = std::clamp(x, 0.0, Map::MaxX() * static_cast<double>(TILE_SIZE));
		y = std::clamp(y, 0.0, Map::MaxY() * static_cast<double>(TILE_SIZE));
		z = FirstPersonGroundZ(x, y);
	}
}

/** The level direction the map looks along, radians: up its screen, (-1, -1) facing north, turned with it (TurnWorldPoint()). */
static double OrbitHeading()
{
	double c, s, px, py;
	GetViewTurn(c, s, px, py);
	return std::atan2(s - c, -c - s);
}

/** How far the camera is when it shows the place as big as the map does at its zoom (90 degrees across the view): metres. */
static double OrbitDistanceOfMap(const Viewport &vp)
{
	/* The map: a unit of the world across the screen 2 sqrt(2) pixels at the normal zoom (RemapCoords()). */
	const double pixels_per_unit = 2.0 * std::numbers::sqrt2 * ScaleByZoom(1, ZoomLevel::Normal) / ScaleByZoom(1, vp.zoom);
	const double focal = vp.width / 2.0;
	return focal / pixels_per_unit / OBJECT_UNITS_PER_METRE;
}

/** Whether the camera may go round a place now: in a game, the map of 3D shown. */
static bool OrbitAllowed()
{
	const Window *main = GetMainWindow();
	return _game_mode == GameMode::Normal && ThreeDEnabled() && main != nullptr && main->viewport != nullptr && !FirstPersonFull();
}

/** The camera on, the map's picture on the card shown no more. */
static void OrbitStart(int side)
{
	const Window *main = GetMainWindow();
	_orbit.on = true;
	_orbit.side = side;
	_orbit.tip_deg = ORBIT_MAP_TIP_DEG;
	_orbit.drag_y = 0;
	_orbit.distance_m = std::clamp(OrbitDistanceOfMap(*main->viewport), ORBIT_NEAREST_M, ORBIT_FARTHEST_M);
	_orbit.camera = {};
	_orbit.picture_ok = false;
	HideTurnedMapOnCard();
	MarkWholeScreenDirty();
}

/** The map again: the camera's picture on the card shown no more, its frame dropped. */
static void OrbitStop()
{
	if (!_orbit.on) return;
	_orbit.on = false;
	if (Window *main = GetMainWindow(); main != nullptr) VideoDriver::GetInstance()->HideThreeDPicture(main);
	DropEyeFrame(&_orbit.camera);
	_orbit.picture = {};
	MarkWholeScreenDirty();
}

/**
 * The button turning the map dragged (window.cpp): up or down, over the map it leaves it for the camera, on the camera
 * it goes higher or lower; back across the map's angle the map shows again.
 * @param dx pixels moved sideways
 * @param dy pixels moved up (down negative)
 * @return whether the drag did something here
 */
bool OrbitTipByDrag(int dx, int dy)
{
	if (!OrbitAllowed()) {
		OrbitStop();
		return false;
	}
	if (!_orbit.on) {
		_orbit.drag_x_sum += std::abs(dx);
		_orbit.drag_y_sum += dy;
		if (std::abs(_orbit.drag_y_sum) - _orbit.drag_x_sum < ORBIT_LEAVE_MAP_PX) return false;
		OrbitStart(_orbit.drag_y_sum > 0 ? 1 : -1);
		dy = _orbit.drag_y_sum - _orbit.side * ORBIT_LEAVE_MAP_PX;
	}
	_orbit.drag_y += dy;
	const int degrees = _orbit.drag_y / THREE_D_DRAG_PIXELS_PER_DEG;
	if (degrees == 0) return true;
	_orbit.drag_y -= degrees * THREE_D_DRAG_PIXELS_PER_DEG;
	_orbit.tip_deg = std::clamp(_orbit.tip_deg + degrees, ORBIT_LOWEST_DEG, ORBIT_HIGHEST_DEG);
	if ((_orbit.tip_deg - ORBIT_MAP_TIP_DEG) * _orbit.side <= 0) OrbitStop();
	return true;
}

/** The button let go: the next drag over the map counts from nothing. */
void OrbitDragEnd()
{
	_orbit.drag_x_sum = 0;
	_orbit.drag_y_sum = 0;
}

/** The button clicked twice: on the camera, the map again (true); over the map nothing here. */
bool OrbitDoubleClick()
{
	if (!_orbit.on) return false;
	OrbitStop();
	return true;
}

/** The wheel over the main view on the camera: nearer (up, negative) or farther. */
void OrbitViewWheel(int wheel)
{
	_orbit.distance_m = std::clamp(_orbit.distance_m * std::pow(ORBIT_WHEEL_STEP, wheel), ORBIT_NEAREST_M, ORBIT_FARTHEST_M);
}

/** A new picture of the camera, as big as the main view. */
static void OrbitMakePicture(const Window &main)
{
	const Viewport &vp = *main.viewport;
	OrbitPlace(vp, _orbit.x, _orbit.y, _orbit.z);
	_orbit.heading = OrbitHeading();
	_orbit.picture_ok = DrawOrbitView(_orbit.x, _orbit.y, _orbit.z, _orbit.heading, _orbit.tip_deg * M_PI / 180.0, _orbit.distance_m * OBJECT_UNITS_PER_METRE,
			vp.width, vp.height, _orbit.camera, _orbit.picture);
}

/** Every frame of the main window (MainWindow::OnRealtimeTick()): the camera's picture anew and drawn; off when it may not be. */
void OrbitViewTick(Window &main)
{
	if (!_orbit.on) return;
	if (!OrbitAllowed()) {
		OrbitStop();
		return;
	}
	OrbitMakePicture(main);
	main.SetWidgetDirty(WID_M_VIEWPORT);
}

/**
 * The camera's picture where the main view is (NWidgetViewport::Draw()), instead of the map.
 * @return whether it is on (else the map is drawn)
 */
bool DrawOrbitViewInto(const Window &main, const Rect &r)
{
	if (!_orbit.on) return false;
	if (_orbit.picture.width != r.Width() || _orbit.picture.height != r.Height()) OrbitMakePicture(main);
	if (_orbit.picture.width <= 0) {
		GfxFillRect(r, PC_BLACK);
		return true;
	}
	BlitEyePicture(main, _orbit.picture, r.left, r.top);
	return true;
}

/** The rig ('test3d krouzeni'): the camera on so far down and so far off, or off. */
void OrbitViewSet(bool on, double tip_deg, double distance_m)
{
	if (!on) {
		OrbitStop();
		return;
	}
	if (!OrbitAllowed()) return;
	if (!_orbit.on) OrbitStart(1);
	_orbit.side = tip_deg >= ORBIT_MAP_TIP_DEG ? 1 : -1;
	_orbit.tip_deg = std::clamp(tip_deg, ORBIT_LOWEST_DEG, ORBIT_HIGHEST_DEG);
	if (distance_m > 0) _orbit.distance_m = std::clamp(distance_m, ORBIT_NEAREST_M, ORBIT_FARTHEST_M);
}

/** The rig: the camera as it is. */
std::string DescribeOrbitView()
{
	if (!_orbit.on) return "vypnuto (mapa)";
	return fmt::format("zapnuto: sklon {:.0f} st. ({}), vzdalenost {:.0f} m, misto {:.0f},{:.0f} vyska {:.0f}, smer {:.0f} st., obraz {}x{}{}", _orbit.tip_deg,
			_orbit.side > 0 ? "nad mapou" : "pod mapou", _orbit.distance_m, _orbit.x, _orbit.y, _orbit.z, _orbit.heading * 180.0 / M_PI,
			_orbit.picture.width, _orbit.picture.height, _orbit.picture_ok ? "" : " (zadny)");
}
