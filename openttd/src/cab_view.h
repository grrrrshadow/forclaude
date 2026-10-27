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
#include "tile_type.h"

#include <array>
#include <vector>

/** The picture of the cab view: 32bpp, a pixel's alpha 255 where something is drawn. */
struct CabViewPicture {
	int width = 0;
	int height = 0;
	std::vector<uint32_t> pixels;
	uint32_t shown = 0; ///< drawn by the graphics card and shown straight from it: its texture there (pixels not of it; three_d_gpu.h)
};

/** The eye of the cab view, kept by the window between frames: the heading it has turned to so far. */
struct CabViewCamera {
	double heading = 0.0; ///< radians, the world direction the eye looks along (atan2 of the direction's y, x)
	bool valid = false;   ///< whether heading is set
	double eye_dz = -1.0; ///< how high the eye is over the vehicle, units of height: it bows under a low bridge; -1 not yet
	double ms = 0.0;      ///< rig: how long the last picture took, milliseconds: collecting and drawing
	double collect_ms = 0.0; ///< rig: of it, collecting what to draw, under the lock of the game (CollectEyeFrame())
	double draw_ms = 0.0; ///< rig: and drawing it, on the thread of the frames (EyeFrameThread())
	bool fresh = false;   ///< a picture drawn anew was taken by the last call (DrawCabView(), DrawFirstPersonView())
	int tiles = 0;        ///< rig: tiles drawn in the last picture
	int billboards = 0;   ///< rig: billboards drawn in the last picture
	double stage[7] = {}; ///< rig: the milliseconds of the stages of collecting the last picture (CollectEyeFrame())
};

/*
 * The pictures are collected under the lock of the game and drawn on a thread of their own, one frame behind
 * (viewport.cpp, EyeFrame): what a window holds is the last one drawn, taken as it asks for the next.
 */
bool EyeFrameDrawing();                ///< a frame is being drawn: what it draws from must stay as it is
bool TurnedObjectBatchBusy();          ///< a batch of the turned map's buildings is on its way to the card: the same holds
void StopTurnedObjectBatchThread();
void WaitEyeFrame();                   ///< wait for it (the objects are read anew)
void StopEyeFrameThread();             ///< at the end of the game
void DropEyeFrame(const void *client); ///< a window closes: its frame goes
extern bool _eye_synchronous;          ///< the rig: drawn at once on the thread that asked, as before

struct Vehicle;
struct Window;
void BlitEyePicture(const Window &w, const CabViewPicture &pic, int px, int py, uint32_t tint = 0);
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
 * again every so often; the turn at the next junction once. Up twice in a row: full speed, on by itself till braked
 * (the player, 22. 10.: "and the driving on by itself too"). The view from the cab and the walker riding in the view
 * from the first person each have one.
 */
struct CabArrows {
	DirectionKeys held{};           ///< the arrows held at the last look
	std::array<int, 4> repeat_in{}; ///< for each arrow held, milliseconds till it does it again
	bool route = false;             ///< the player asked for its route (CabToggleRoute()): the arrows do not drive
	int free_wait = 0;              ///< milliseconds till the wheel is taken again, as the command takes its time
	int up_tapped = INT_MAX;        ///< milliseconds since Up was last pressed (twice in a row: full speed, Drive())
	int still_ms = 0;               ///< milliseconds it has stood since Up was pressed (its record, jizda.log)
	bool still_told = false;        ///< that standing written down, till it moves
	int down_still_ms = 0;          ///< milliseconds Down has been held with the vehicle stopped (long enough: backwards)
	int backwards_wait = 0;         ///< milliseconds till backwards or forwards is asked again, as the command takes its time
	std::string done;               ///< what the arrows did last, and how it came out, for that record
	void Drive(const Vehicle *v, DirectionKeys now, int ms);
};
void CabToggleRoute(const Vehicle *v, CabArrows &arrows);

/**
 * The walker of the view from the first person (first_person_gui.cpp; the player, 15. 10.: "let's make Morrowind,
 * the inventory, the first person view"): where it stands and where it looks. Only the picture: nothing of the game.
 */
/** How a thing is held in the walker's hands (DrawThreeDHeldThing()). */
enum class ThreeDHold : uint8_t {
	Swung,    ///< a hammer, a sword: in the right hand by its handle, swung
	Bow,      ///< a bow: upright in the left hand, its string to the eye
	Crossbow, ///< a crossbow: at the right shoulder, along the look
	Shoulder, ///< a rocket launcher: its tube along the look, low at the right, so that all of it is seen
	Left,     ///< a light (the torch, the lantern): upright in the left hand, low at the left
};

/** The thing in the walker's hands, as it is drawn over the picture from its eyes (DrawThreeDHeldThing()). */
struct ThreeDHeldThing {
	std::string thing;   ///< the thing of the indices ("predmet": "kladivo", a state of it: "luk/napnuto"), empty for none
	ThreeDHold hold = ThreeDHold::Swung; ///< how it is held
	float grip[3] = {0, 0, 0.22f}; ///< where the hand holds it, metres in its frame (its node "uchyt")
	std::string loaded;  ///< the arrow on its string, the bolt in its groove ("sip", "sipka"), empty for none
	float nock[3] = {};  ///< where the loaded one's origin (its nock) is, metres in the thing's frame (its "zarez_<stav>")
	float swing = 0;     ///< how far it is swung, degrees: 0 at rest, below 0 raised back, above 0 struck forward and down
	float raise = 0;     ///< how far up in the hand it is: 0 put away under the picture, 1 held
	float kick = 0;      ///< a shot's kick, 1 as it is shot to 0
	float drawn = 0;     ///< a bow: how far it is drawn, 0 to 1 (brought before the eye, the arrow at the dot)
	float twist = 0;     ///< swung: degrees it is turned about its handle (a sword's flat to the eye)
	int frame = 0;       ///< its frame (a torch's flame, "plamen")
};

/** A puff of the smoke of a rocket, or its blast, seen from the eyes (first_person_gui.cpp): a soft ball of colour. */
struct ThreeDBlast {
	double x = 0, y = 0, z = 0; ///< its middle, world units and units of height
	double radius_m = 0;        ///< how big, metres
	float alpha = 0;            ///< how much of what is behind it hides, at its middle
	uint32_t colour = 0;        ///< 0xRRGGBB
	bool light = false;         ///< a blast: it lights what is about it at night
	bool lights_about = true;   ///< a blast (light): it does light what is about it -- of a dragon's fire's many flames only a few do
};

struct FirstPersonEye {
	double x = 0, y = 0;  ///< where it stands, world units
	double heading = 0;   ///< radians, the world direction it looks along (atan2 of y, x)
	double pitch = 0;     ///< radians, looking up (positive) or down
	double eye_z = 0;     ///< its eyes over the ground, units of height
	double stand_z = 0;   ///< the height it stands at, units of height (FirstPersonStandZ()): up the stairs of the dragon castle, on a bridge
	double stand_x = -1, stand_y = -1; ///< where it stood at that height; anywhere else, on the ground storey (FirstPersonFeetZ())
	ThreeDHeldThing held; ///< the thing in its hands
	ThreeDHeldThing left; ///< the light in its left hand (the torch, the lantern), lighting about it at night
	uint32_t light_colour = 0; ///< that light's colour (Colour::data), 0 none
	double light_reach_m = 0;  ///< and how far it lights, metres
	std::vector<ThreeDBlast> blasts; ///< the smoke of its rockets and their blasts
};
bool DrawFirstPersonView(const FirstPersonEye &walker, int width, int height, CabViewCamera &camera, CabViewPicture &out);
double FirstPersonGroundZ(double x, double y);
double FirstPersonStandZ(double x, double y, double from);
double FirstPersonFeetZ(const FirstPersonEye &walker);
void ShowFirstPersonWindow();
void ShowFirstPersonWindowAt(TileIndex tile);

#endif /* CAB_VIEW_H */
