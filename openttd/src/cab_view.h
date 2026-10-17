/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cab_view.h The view from the cab of a vehicle: the world ahead of it in perspective.
 *
 * The player (13. 10.) saw a video of someone's OpenTTD with a "cab view" window
 * -- the track ahead of a train drawn in 3D as it runs, trees and platforms
 * beside it, a panel with the speed, the next stop and the load -- and wants
 * the same in this game, for a train, a road vehicle, an aircraft and a ship.
 *
 * The picture is made by the same means as the turned map view (three_d.h):
 * the tiles ahead are asked what they draw, their ground sprites go north-up
 * into one texture (the rails, roads and platforms are ground), and every tile
 * is laid as four textured triangles in perspective from the eye at the front
 * of the vehicle; everything standing (trees, houses, signals, the other
 * vehicles) is a billboard of its sprite, facing the eye, sized as the sprite
 * is on the map. A z-buffer sorts it all. Docs: docs/decouple_three_d.md.
 */

#ifndef CAB_VIEW_H
#define CAB_VIEW_H

#include "vehicle_type.h"
#include "gfx_type.h"

#include <array>
#include <vector>

/** The picture of the cab view: 32bpp, a pixel's alpha 255 where something is drawn. */
struct CabViewPicture {
	int width = 0;
	int height = 0;
	std::vector<uint32_t> pixels;
};

/** The eye of the cab view, kept by the window between frames: the heading it has turned to so far. */
struct CabViewCamera {
	double heading = 0.0; ///< radians, the world direction the eye looks along (atan2 of the direction's y, x)
	bool valid = false;   ///< whether heading is set
	double eye_dz = -1.0; ///< how high the eye is over the vehicle, units of height: it bows under a low bridge; -1 not yet
	double ms = 0.0;      ///< rig: how long the last picture took, milliseconds
	int tiles = 0;        ///< rig: tiles drawn in the last picture
	int billboards = 0;   ///< rig: billboards drawn in the last picture
	double stage[7] = {}; ///< rig: the milliseconds of the stages of the last picture (DrawEyeView())
};

struct Vehicle;
struct Window;
bool DrawCabView(const Vehicle *v, int width, int height, CabViewCamera &camera, CabViewPicture &out);
void ShowCabViewWindow(const Vehicle *v);
Window *ArrowsWindow();

/* What the cab does to its vehicle (cab_view_gui.cpp): its buttons, its arrows, the walker riding in it. */
bool CabTurn(const Vehicle *v, bool left);
void CabSpeed(const Vehicle *v, bool faster);
void CabStartStop(const Vehicle *v);
void CabLetGo(const Vehicle *v);
void CabHorn(const Vehicle *v);
void CabToggleLights(const Vehicle *v);

/**
 * At the wheel of a vehicle from its cab (the player, 17. 10.: "while the cab view is open the arrows are for driving,
 * as in the view from the first person"; "as soon as you get into the cab you drive"): the vehicle is driven freely as
 * soon as it can be (CmdCabDrive()), until the player asks for its route. Left and right turn it as the cab's buttons
 * do, up is the throttle (and off, a vehicle that stands), down the brake; an aircraft's up and down its height. Held,
 * again every so often; the turn at the next junction once. The view from the cab and the walker riding in the view
 * from the first person each have one.
 */
struct CabArrows {
	DirectionKeys held{};           ///< the arrows held at the last look
	std::array<int, 4> repeat_in{}; ///< for each arrow held, milliseconds till it does it again
	bool route = false;             ///< the player asked for its route (CabToggleRoute()): the arrows do not drive
	int free_wait = 0;              ///< milliseconds till the wheel is taken again, as the command takes its time
	void Drive(const Vehicle *v, DirectionKeys now, int ms);
};
void CabToggleRoute(const Vehicle *v, CabArrows &arrows);

/**
 * The walker of the view from the first person (first_person_gui.cpp; the player, 15. 10.: "let's make Morrowind,
 * the inventory, the first person view"): where it stands and where it looks. Only the picture: nothing of the game.
 */
struct FirstPersonEye {
	double x = 0, y = 0;  ///< where it stands, world units
	double heading = 0;   ///< radians, the world direction it looks along (atan2 of y, x)
	double pitch = 0;     ///< radians, looking up (positive) or down
	double eye_z = 0;     ///< its eyes over the ground, units of height
	std::string held;     ///< the thing in its hands ("predmet" of the indices, the hammer), empty for none
	float swing = 0;      ///< how far the thing is swung, degrees (DrawThreeDHeldThing())
	float raise = 0;      ///< how far up in the hand the thing is, 0 put away to 1 held
};
bool DrawFirstPersonView(const FirstPersonEye &walker, int width, int height, CabViewCamera &camera, CabViewPicture &out);
double FirstPersonGroundZ(double x, double y);
void ShowFirstPersonWindow();

#endif /* CAB_VIEW_H */
