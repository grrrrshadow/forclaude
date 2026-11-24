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
 * stations) are drawn for that angle only. This camera is drawn as the view from the eyes is (DrawOrbitView() of
 * viewport.cpp: our 3D models, the rest standing pictures), its look tipped down for real (CabEye::sp).
 *
 * It is one of the two cameras of the view from above, chosen in the settings (gui.orbit_view; the player, 19. 11.:
 * "orbit view behind a switch between today's camera and the orbit view. From position 0 it is the orbit view not only
 * up and down, at once from 0 to the right and to the left too. It goes off by centring the view, two clicks of the
 * middle button. From above the right button moves the map and the middle one turns the camera"). Off, the middle
 * button turns and tips the map itself (three_d.cpp), as before. On, the middle button dragged any way leaves the map
 * for this camera at once: sideways it goes round the place in the middle, up over the roofs to straight down, down to
 * the ground from the side. Its place and its turn are the map's: the right button moves the map under it and the
 * place with it, turning the map turns it. The wheel brings it nearer or farther. The view centred (two clicks of the
 * middle button, the key, a construction toolbar opened: ResetViewYaw()) the map shows again, facing north. It is only
 * to look at: a click on it builds and picks nothing. Not in the savegame or the config, as the map's turn.
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

#include <algorithm>
#include <cmath>
#include <numbers>

#include "safeguards.h"

bool DrawOrbitView(double x, double y, double z, double heading, double tip, double distance, int width, int height, CabViewCamera &camera, CabViewPicture &out);
void HideTurnedMapOnCard();
bool FirstPersonFull();

static constexpr double ORBIT_MAP_TIP_DEG = 30.0; ///< the map's own look down: the game's projection
static constexpr double ORBIT_LOWEST_DEG = 2.0;   ///< the camera down to the ground: so far over the level at least
static constexpr double ORBIT_HIGHEST_DEG = 90.0; ///< and over the roofs: straight down at most
static constexpr double ORBIT_DRAG_DEG = 1.0 / THREE_D_DRAG_PIXELS_PER_DEG; ///< a pixel of the drag: so far round or up, as the map turns
static constexpr double ORBIT_SCROLL_LEAST_SIN = 0.25; ///< the right button moving the place along the look: the ground no flatter than this under the pointer (near the ground it would run away)
static constexpr double ORBIT_NEAREST_M = 10.0;   ///< the camera this near the place at least
static constexpr double ORBIT_FARTHEST_M = 300.0; ///< and this far at most: farther the view from the eyes draws the far land only
static constexpr double ORBIT_WHEEL_STEP = 1.2;   ///< a notch of the wheel: so much nearer or farther

/** The camera going round a place, and its picture. */
struct OrbitView {
	bool on = false;
	double tip_deg = ORBIT_MAP_TIP_DEG; ///< how far it looks down
	double distance_m = 100;            ///< how far it is from the place
	/**
	 * The place as this camera put it, exactly, and where it scrolled the map to for it: the view scrolls by whole pixels
	 * of its zoom, and the place read back from it at every step of a drag would creep and shake. Good while the map is
	 * where this camera left it (OrbitPlace()); moved by anything else (a vehicle followed, a jump to a place, the keys
	 * turning it), the place is the map's again.
	 */
	bool place_kept = false;
	double place_x = 0, place_y = 0;
	int place_scroll_x = 0, place_scroll_y = 0;
	float place_yaw = 0;
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
static void OrbitPlace(const ViewportData &vp, double &x, double &y, double &z)
{
	if (_orbit.place_kept && vp.dest_scrollpos_x == _orbit.place_scroll_x && vp.dest_scrollpos_y == _orbit.place_scroll_y && GetViewYawSigned() == _orbit.place_yaw) {
		x = _orbit.place_x;
		y = _orbit.place_y;
		z = FirstPersonGroundZ(x, y);
		return;
	}
	_orbit.place_kept = false;
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

/** The map scrolled so that a place is in the middle of the main view, and the place kept as it is (OrbitView::place_kept). */
static void OrbitPutPlace(ViewportData &vp, double x, double y)
{
	x = std::clamp(x, 0.0, Map::MaxX() * static_cast<double>(TILE_SIZE));
	y = std::clamp(y, 0.0, Map::MaxY() * static_cast<double>(TILE_SIZE));
	double vx, vy;
	ProjectTurnedF(x, y, FirstPersonGroundZ(x, y), vx, vy);
	const int mask = ScaleByZoom(-1, vp.zoom);
	vp.dest_scrollpos_x = vp.scrollpos_x = (static_cast<int>(std::lround(vx)) - vp.virtual_width / 2) & mask;
	vp.dest_scrollpos_y = vp.scrollpos_y = (static_cast<int>(std::lround(vy)) - vp.virtual_height / 2) & mask;
	_orbit.place_kept = true;
	_orbit.place_x = x;
	_orbit.place_y = y;
	_orbit.place_scroll_x = vp.dest_scrollpos_x;
	_orbit.place_scroll_y = vp.dest_scrollpos_y;
	_orbit.place_yaw = GetViewYawSigned();
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

/** The camera on, as the map shows the place: as far down, as big. The map's picture on the card shown no more. */
static void OrbitStart()
{
	const Window *main = GetMainWindow();
	_orbit.on = true;
	_orbit.tip_deg = ORBIT_MAP_TIP_DEG;
	_orbit.distance_m = std::clamp(OrbitDistanceOfMap(*main->viewport), ORBIT_NEAREST_M, ORBIT_FARTHEST_M);
	_orbit.place_kept = false;
	_orbit.camera = {};
	_orbit.picture_ok = false;
	HideTurnedMapOnCard();
	MarkWholeScreenDirty();
}

/** The map again: the camera's picture on the card shown no more, its frame dropped. The map turned to a whole degree, as it turns by itself. */
static void OrbitStop()
{
	if (!_orbit.on) return;
	_orbit.on = false;
	_orbit.place_kept = false;
	if (Window *main = GetMainWindow(); main != nullptr) VideoDriver::GetInstance()->HideThreeDPicture(main);
	DropEyeFrame(&_orbit.camera);
	_orbit.picture = {};
	SetViewYaw(std::round(GetViewYawSigned()));
	MarkWholeScreenDirty();
}

/**
 * The middle button dragged with this camera chosen (gui.orbit_view; window.cpp): over the map it leaves it for the
 * camera at once, whichever way; sideways the camera goes round the place in the middle (the map under it turned so,
 * the place kept where it is), up or down it goes higher or lower.
 * @param turn pixels moved sideways, as RotateViewByDrag() takes them
 * @param up pixels moved up (down negative)
 * @return whether the drag was this camera's (else the map's own turn)
 */
bool OrbitByDrag(int turn, int up)
{
	if (!OrbitAllowed()) {
		OrbitStop();
		return false;
	}
	if (turn == 0 && up == 0) return _orbit.on;
	if (!_orbit.on) OrbitStart();
	if (turn != 0) {
		ViewportData &vp = *GetMainWindow()->viewport;
		double x, y, z;
		OrbitPlace(vp, x, y, z);
		SetViewYaw(GetViewYawSigned() + static_cast<float>(turn * ORBIT_DRAG_DEG));
		OrbitPutPlace(vp, x, y);
	}
	_orbit.tip_deg = std::clamp(_orbit.tip_deg + up * ORBIT_DRAG_DEG, ORBIT_LOWEST_DEG, ORBIT_HIGHEST_DEG);
	return true;
}

/**
 * The map dragged under the camera (the right button, MainWindow::OnScroll()): the place moves as far as the ground
 * under the pointer goes at the camera's distance and tip -- the map's own pixels are of its zoom, which the wheel on
 * the camera leaves as it was.
 * @param delta pixels of the screen the view moves by, as the map scrolls
 * @return whether the camera took it (else the map scrolls by itself)
 */
bool OrbitScroll(Point delta)
{
	if (!_orbit.on || !OrbitAllowed()) return false;
	ViewportData &vp = *GetMainWindow()->viewport;
	double x, y, z;
	OrbitPlace(vp, x, y, z);
	/* 90 degrees across the view (OrbitDistanceOfMap()): a pixel in the middle of it so many units of the world. */
	const double unit = _orbit.distance_m * OBJECT_UNITS_PER_METRE / (vp.width / 2.0);
	const double heading = OrbitHeading();
	const double fx = std::cos(heading), fy = std::sin(heading);
	const double right = delta.x * unit;
	const double ahead = -delta.y * unit / std::max(std::sin(_orbit.tip_deg * M_PI / 180.0), ORBIT_SCROLL_LEAST_SIN);
	/* To the right of the look: (-1, 1) facing north, where the look is (-1, -1). */
	OrbitPutPlace(vp, x + fy * right + fx * ahead, y - fx * right + fy * ahead);
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
	const ViewportData &vp = *main.viewport;
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
	if (!_orbit.on) OrbitStart();
	_orbit.tip_deg = std::clamp(tip_deg, ORBIT_LOWEST_DEG, ORBIT_HIGHEST_DEG);
	if (distance_m > 0) _orbit.distance_m = std::clamp(distance_m, ORBIT_NEAREST_M, ORBIT_FARTHEST_M);
}

/** The rig: the camera as it is. */
std::string DescribeOrbitView()
{
	if (!_orbit.on) return "vypnuto (mapa)";
	return fmt::format("zapnuto: sklon {:.2f} st., vzdalenost {:.0f} m, misto {:.2f},{:.2f} vyska {:.0f}, smer {:.2f} st., mapa {:.2f} st., obraz {}x{}{}, {:.1f} ms", _orbit.tip_deg,
			_orbit.distance_m, _orbit.x, _orbit.y, _orbit.z, _orbit.heading * 180.0 / M_PI, GetViewYawSigned(),
			_orbit.picture.width, _orbit.picture.height, _orbit.picture_ok ? "" : " (zadny)", _orbit.camera.ms);
}
