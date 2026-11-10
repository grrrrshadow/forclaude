/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file first_person_gui.cpp The view from the first person: a walker on the map, seen from its eyes.
 *
 * The player (15. 10.), after looking at OpenMW: "let's make Morrowind, the inventory, the first person view", "and
 * don't forget anything, the lights". The world is drawn as from the cab of a vehicle (cab_view.h, DrawEyeView() in
 * viewport.cpp), the eye at the walker's eyes; the walker goes with W and S or the arrows up and down, steps aside
 * with A and D, turns with the arrows left and right, looks about with the right mouse button held (as the map is
 * dragged; two clicks of it without a drag look straight ahead again, as two of the middle one put the map back to
 * north), runs with Shift. It walks the ground as it lies and does not go through houses, industries' buildings
 * or water. At night (gui.night) the picture is dark and the vehicles shine. Only the picture: nothing of the game
 * changes, and nothing of it is saved.
 */

#include "stdafx.h"
#include "cab_view.h"
#include "orbit_view.h"
#include "landscape.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "tile_map.h"
#include "map_func.h"
#include "town.h"
#include "house.h"
#include "industry.h"
#include "industry_map.h"
#include "station_map.h"
#include "water_map.h"
#include "bridge_map.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"
#include "language.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "blitter/factory.hpp"
#include "widgets/first_person_widget.h"
#include "walker_inventory.h"
#include "company_base.h"
#include "company_func.h"
#include "command_func.h"
#include "walker_cmd.h"
#include "three_d_dragons.h"
#include "three_d_animals.h"
#include "timer/timer_game_tick.h"
#include "three_d_walkers.h"
#include "three_d_witches.h"
#include "vehicle_base.h"
#include "train.h"
#include "aircraft.h"
#include "cab_drive.h"
#include "simtrains_cab.h"
#include "engine_base.h"
#include "engine_func.h"
#include "road_func.h"
#include "roadveh.h"
#include "roadveh_cmd.h"
#include "sound_func.h"
#include "core/random_func.hpp"
#include "frame_diary.h"
#include "video/video_driver.hpp"

#include "table/strings.h"
#include "table/sprites.h"

#include <chrono>
#include <cmath>

#include "map_drag.h"

#include "safeguards.h"

/** The walker's eyes over the ground, metres (a girl of the indices is 1.62 m tall). */
static constexpr double FIRST_PERSON_EYES_M = 1.55;
/** How fast it walks and runs, metres a second. */
static constexpr double FIRST_PERSON_WALK_MS = 3.0;
static constexpr double FIRST_PERSON_RUN_MS = 8.5;
/** How fast it turns on the arrows, radians a second. */
static constexpr double FIRST_PERSON_TURN = 2.0;
/**
 * The turn on the arrows starts at this share of that, stays there a beat (FIRST_PERSON_TURN_SLOW_S) and runs up to
 * the whole of it as the arrow stays held (FIRST_PERSON_TURN_RAMP_S): a tap turns a degree or two to aim by, held it
 * still turns fast. At the one speed, a tap was the whole 115 degrees a second for however long a frame took -- on a
 * slow machine a frame is long, and the player saw his view leap (25. 10. on the run-up from 0.3: "the arrows still
 * turn me a big piece, I cannot turn where I want" -- so the first beat about a tenth, 10 degrees a second).
 */
static constexpr double FIRST_PERSON_TURN_FIRST = 0.09;
static constexpr double FIRST_PERSON_TURN_SLOW_S = 0.35;
static constexpr double FIRST_PERSON_TURN_RAMP_S = 1.1;
/** How far it looks up or down at most, radians. */
static constexpr double FIRST_PERSON_PITCH_MAX = 1.0;
/** How much a pixel of the mouse turns it, radians. */
static constexpr double FIRST_PERSON_MOUSE = 0.005;
/** How far from a wall it stops, world units (THREE_D_WALKER_RADIUS_M). */
static constexpr double FIRST_PERSON_RADIUS = THREE_D_WALKER_RADIUS_M * OBJECT_UNITS_PER_METRE;
/** The walker inside a house's interior: a body's half-width, between the rows of seats. */
static constexpr double FIRST_PERSON_INDOOR_RADIUS = THREE_D_WALKER_BODY_M * OBJECT_UNITS_PER_METRE;
/** The longest move checked against the walls at once, world units: running a step goes in pieces, so no wall is stepped over. */
static constexpr double FIRST_PERSON_SUBSTEP = 0.1;
/** How far aside the walker steps round what stands a little to one side of its way (FirstPersonMove()), world units. */
static constexpr double FIRST_PERSON_SIDESTEP = 0.3 * OBJECT_UNITS_PER_METRE;
/** In so many tries each way, the nearest first. */
static constexpr int FIRST_PERSON_SIDESTEPS = 6;
/** How often it moves and the picture is made, milliseconds. */
static constexpr int FIRST_PERSON_TICK_MS = 40;
static constexpr double FIRST_PERSON_DIVE_M_S = 1.0; ///< a submarine goes up or down so fast, metres a second (DiveTowards())
/** An arrow pressed twice within this many seconds: the walker walks on by itself (the player, 21. 10.). */
static constexpr double FIRST_PERSON_DOUBLE_TAP_S = 0.4;
/** How long taking a weapon out or putting it away takes, and a swing of it, seconds. */
static constexpr double FIRST_PERSON_DRAW_S = 0.35;
static constexpr double FIRST_PERSON_SWING_S = 0.6;
/** When in the swing the weapon strikes, seconds. */
static constexpr double FIRST_PERSON_BLOW_S = 0.27;
/** How long the bow is drawn fully, how long a new arrow is set on its string, how long the crossbow is drawn again, seconds. */
static constexpr double FIRST_PERSON_BOW_DRAW_S = 0.8;
static constexpr double FIRST_PERSON_NOCK_S = 0.5;
static constexpr double FIRST_PERSON_RELOAD_S = 1.5;
/** How long a shot kicks the bow or the crossbow back, seconds. */
static constexpr double FIRST_PERSON_KICK_S = 0.15;
/** An arrow's fall, metres a second each second; how long one flies at most and stays stuck where it hit, seconds; how many there are at most. */
static constexpr double FIRST_PERSON_GRAVITY = 9.81;
static constexpr double FIRST_PERSON_FLY_S = 8.0;
static constexpr double FIRST_PERSON_STUCK_S = 20.0;
static constexpr size_t FIRST_PERSON_MISSILES = 40;
/** The machine gun: seconds between its shots (the colleague: "ten a second"), shots of a burst from Space. */
static constexpr double FIRST_PERSON_BURST_S = 0.1;
static constexpr int FIRST_PERSON_BURST_SHOTS = 5;
/** The rocket launcher: seconds to load it again (the colleague: "then it loads 2 s"); a rocket's blast, metres, and how
 * long it is seen; its smoke: a puff every so many seconds, each seen so long. */
static constexpr double FIRST_PERSON_ROCKET_RELOAD_S = 2.0;
static constexpr double FIRST_PERSON_BLAST_M = 4.0;
static constexpr double FIRST_PERSON_BLAST_S = 0.8;
static constexpr double FIRST_PERSON_PUFF_EVERY_S = 0.04;
static constexpr double FIRST_PERSON_PUFF_S = 1.6;
/** Feeding a farm's dragon (the player, 26. 10.): how near its body the walker feeds it with E, metres; how long it
 * smokes the marijuana, its breaths of smoke, seconds; how long the beer takes it to breathe fire, how long it breathes
 * it, how long it smokes after, seconds. */
static constexpr double FIRST_PERSON_FEED_M = 4.0;
/** How near the fire of a forest the walker stands for E to grill there (beyond the logs about it), metres. */
static constexpr double FIRST_PERSON_GRILL_M = 4.0;
static constexpr double FIRST_PERSON_SMOKE_S = 8.0;
static constexpr double FIRST_PERSON_BREATH_S = 2.0;
static constexpr double FIRST_PERSON_GULP_S = 0.8;
static constexpr double FIRST_PERSON_FIRE_S = 2.5;
static constexpr double FIRST_PERSON_AFTER_FIRE_S = 1.5;
/** A farm's dragon's mouth, resting: so much of its length in front of its middle, of its height over its feet. */
static constexpr double FIRST_PERSON_MOUTH_AHEAD = 0.4;
static constexpr double FIRST_PERSON_MOUTH_UP = 0.55;
/** A flame of a dragon's fire: how long it is seen, seconds; how fast it flies out of the mouth, metres a second. */
static constexpr double FIRST_PERSON_FLAME_S = 0.6;
static constexpr double FIRST_PERSON_FLAME_SPEED = 9.0;
/** The mine hammer's blow: where its bomb blows up, so far before the walker, so high over its feet, metres. */
static constexpr double FIRST_PERSON_MINE_AHEAD_M = 1.8;
static constexpr double FIRST_PERSON_MINE_UP_M = 0.8;
/** How near a vehicle the walker gets into it (E), metres from a piece of it. */
static constexpr double FIRST_PERSON_BOARD_M = 5.0;
/**
 * On board a boat (FirstPersonWindow::aboard): how high a step up on its deck the walker takes (the yacht's cockpit is
 * 1.4 m deep, out of it over its seats, 0.8 m each), how far down it steps, metres; how much deeper than where its
 * helmsman is it goes (below, inside its hull, it does not: its cabins are its interior's); its foot, metres either way
 * from its middle, on a deck at two of its corners at least (not on the yacht's boom, not on a rail; over an edge as it
 * steps up, yes); how near its helmsman's place E takes the wheel; the height a wall stops it at, over the higher of its
 * feet where it is and where it steps (a mast, a wheelhouse; the cockpit's coaming as it climbs its seats, not), and
 * how far before it, metres.
 */
static constexpr double FIRST_PERSON_DECK_STEP_M = 0.8;
static constexpr double FIRST_PERSON_DECK_DROP_M = 1.0;
static constexpr double FIRST_PERSON_DECK_DEPTH_M = 1.0;
static constexpr double FIRST_PERSON_DECK_FOOT_M = 0.15;
static constexpr double FIRST_PERSON_HELM_M = 1.5;
static constexpr double FIRST_PERSON_DECK_CHEST_M = 1.2;
static constexpr double FIRST_PERSON_DECK_REACH_M = 0.3;
/**
 * In a boat's rooms (ThreeDBoatRooms(), the colleague's sailing yacht; the player, 7. 11.: "shrink the player so he fits
 * under the cockpit and the low ceiling over the stairs does not matter"): the walker as small as the room over him, its
 * eyes so far under its ceiling; no smaller than so much (under the yacht's cockpit 0.87 m), metres; what is lower over
 * the floor than so much is no ceiling (a step, a seat's edge), metres; the chest a wall stops at is this much of the room.
 */
static constexpr double FIRST_PERSON_ROOM_MIN_M = 0.8;
static constexpr double FIRST_PERSON_ROOM_OVER_EYES_M = 0.15;
static constexpr double FIRST_PERSON_ROOM_FLOOR_M = 0.3;
static constexpr double FIRST_PERSON_ROOM_CHEST = 0.6;
/**
 * In the water (FirstPersonEye::swimming; the player, 7. 11.: "under the water without breath and without the health
 * going"; "I swim after my look, the arrows as ever, and with the right button I look up, down, left, right and swim
 * that way"): how fast it swims, metres a second; its eyes over the surface at the top, metres; over the bed at the
 * bottom, metres; how high a shore over the surface it climbs out onto from the top, metres; how high over the water a
 * deck or a quay it steps onto from a boat, metres.
 */
static constexpr double FIRST_PERSON_SWIM_MS = 1.4;
static constexpr double FIRST_PERSON_SWIM_RUN_MS = 2.4;
static constexpr double FIRST_PERSON_SWIM_EYE_OVER_M = 0.15;
static constexpr double FIRST_PERSON_SWIM_BED_M = 0.5;
static constexpr double FIRST_PERSON_SWIM_CLIMB_M = 0.8;
static constexpr double FIRST_PERSON_DECK_ASHORE_M = 1.5;

/** A point of a boat's frame (metres, +x its bow) in the world as the eyes see it lie (GetThreeDEyeVehicle()). */
static void BoatToWorld(const ThreeDEyeVehicle &ev, double lx, double ly, double lz, double &x, double &y, double &z)
{
	const double a = ev.facing * M_PI / 180.0, fx = std::cos(a), fy = std::sin(a);
	const double mx = lx * ev.scale * ev.along * OBJECT_UNITS_PER_METRE, my = ly * ev.scale * OBJECT_UNITS_PER_METRE;
	x = ev.x + mx * fx - my * fy;
	y = ev.y + mx * fy + my * fx;
	z = ev.z + lz * ev.scale * OBJECT_HEIGHT_PER_METRE;
}

/** A world point in a boat's frame, metres (BoatToWorld() the other way). */
static void WorldToBoat(const ThreeDEyeVehicle &ev, double x, double y, double &lx, double &ly)
{
	const double a = ev.facing * M_PI / 180.0, fx = std::cos(a), fy = std::sin(a);
	const double dx = x - ev.x, dy = y - ev.y;
	lx = (dx * fx + dy * fy) / (ev.scale * ev.along * OBJECT_UNITS_PER_METRE);
	ly = (dy * fx - dx * fy) / (ev.scale * OBJECT_UNITS_PER_METRE);
}
/** How near a mushroom the walker picks it (E), metres. */
static constexpr double FIRST_PERSON_PICK_M = 1.5;
/** How near the middle of a door the walker opens it with E, metres. */
static constexpr double FIRST_PERSON_DOOR_M = 1.6;
/** How near a vehicle of the sets standing in an industry's yard the walker takes one like it with E (the farm's Zetor, its wheelbarrow), metres. */
static constexpr double FIRST_PERSON_YARD_VEHICLE_M = 2.5;
/** How many tiles from the walker's the north tile of a building whose event's vehicle it takes so may be: a building of the game is 2×2 tiles at most, the vehicle on one of them. */
static constexpr int FIRST_PERSON_HOUSE_EVENT_TILES = 2;
/** Farther than this from where the walker last stood (put somewhere else, not walked there), it stands on the ground storey again, metres. */
static constexpr double FIRST_PERSON_SETTLE_M = 2.0;

/** How a weapon of the walker is used. */
enum class WeaponUse : uint8_t {
	Swing,    ///< swung: the hammer, the swords
	Bow,      ///< drawn while held, shot let go
	Crossbow, ///< shot, then drawn again
	Burst,    ///< fires shot after shot while the button is held (the machine gun)
	Rocket,   ///< a rocket, flying straight, exploding where it hits; then loaded again (the rocket launcher)
	Rod,      ///< cast into the water, a fish caught on it (the fishing rod, FishingStage)
};

/**
 * How the walker fishes (the player, 7. 11.: "the rod in the inventory from the start, held as the hammer; a click casts
 * the hook with a worm six to ten metres ahead, only into the water; after five to twenty seconds a bite, the rod bends,
 * two seconds 'A bite! Click'; clicked in time the fish hangs by its mouth and goes into the inventory, late it escapes;
 * the worms are not used up"). Only the screen, never saved.
 */
enum class FishingStage : uint8_t {
	Ready,   ///< the hook hanging under the rod's tip before the eyes
	Flying,  ///< the hook flying out to the water
	Waiting, ///< the hook floating on the water, a fish to bite
	Bite,    ///< a fish biting: the rod bent, a click in time catches it
	Caught,  ///< a fish caught, hanging by its mouth before the eyes a moment
};
/** How far ahead the hook falls, metres: somewhere between. */
static constexpr double FIRST_PERSON_CAST_MIN_M = 6.0;
static constexpr double FIRST_PERSON_CAST_MAX_M = 10.0;
/** Seconds the hook flies out; the rod's swing back, then forward, in the first of them. */
static constexpr double FIRST_PERSON_CAST_S = 0.9;
static constexpr double FIRST_PERSON_CAST_SWING_S = 0.45;
/** Seconds on the water before a fish bites: somewhere between. */
static constexpr double FIRST_PERSON_BITE_MIN_S = 5.0;
static constexpr double FIRST_PERSON_BITE_MAX_S = 20.0;
/** Seconds a fish bites: clicked in them, it is caught. */
static constexpr double FIRST_PERSON_BITE_S = 2.0;
/** Seconds a fish caught hangs before the eyes before it goes into the inventory. */
static constexpr double FIRST_PERSON_CAUGHT_S = 2.5;
/** Seconds a word about the fishing is shown (no water there, the fish escaped, what was caught). */
static constexpr double FIRST_PERSON_FISH_SAY_S = 2.5;
/** The line from the tip to the hook hanging before the eyes, and to a fish caught, metres. */
static constexpr float FIRST_PERSON_HOOK_HANG_M = 0.7f;
static constexpr float FIRST_PERSON_FISH_HANG_M = 0.5f;
/** The hook's eyelet (its origin) over the water floating, so that its worm is half in it; its bobbing up and down, metres. */
static constexpr double FIRST_PERSON_HOOK_FLOAT_M = 0.3;
static constexpr double FIRST_PERSON_HOOK_BOB_M = 0.04;
/** The fish on the rod (the player: "only those three on the rod, the big green one in a river, the other two in the sea"). */
struct FirstPersonRodFish {
	WalkerItem item;        ///< the thing of the inventory
	std::string_view figure; ///< its animal of the water of the indices
	bool sea;               ///< caught in the sea, else in a river, a canal, a lake
};
static const FirstPersonRodFish _first_person_rod_fish[] = {
	{WalkerItem::Perch, "ryba_okoun", false},
	{WalkerItem::Discus, "ryba_tercovec", true},
	{WalkerItem::Goldfish, "ryba_zavojnatka", true},
};

/** Whether the water at a world point is the sea's (the fish of the sea bite there), else a river's, a canal's, a lake's. */
static bool FishingInSea(double x, double y)
{
	const TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	if (!IsValidTile(tile) || !HasTileWaterClass(tile)) return true;
	return GetWaterClass(tile) == WaterClass::Sea;
}

/**
 * A weapon of the walker, by its key 1 to 5 (the player, 15. 10.: the big hammer; 17. 10.: "the bow and the crossbow
 * are there", and the colleague's swords): the thing of the inventory, and the colleague's thing of the indices
 * (objekty/predmety/, hra/3d/LUK-SIP-PRO-HRU.md).
 */
void FirstPersonTakeLight(WalkerItem item);

struct FirstPersonWeapon {
	WalkerItem item;        ///< the thing of the inventory
	std::string_view thing; ///< its thing of the indices
	WeaponUse use;          ///< swung or shot
	double reach_m;         ///< swung: how far it reaches, metres
	int damage;             ///< the health a blow or a hit takes of a dragon
	WalkerItem ammo = WalkerItem::End; ///< shot: what it shoots ...
	std::string_view missile = {};     ///< ... and its thing of the indices
	double speed_ms = 0;    ///< how fast that flies, metres a second (the bow drawn fully)
	float twist = 0;        ///< swung: degrees it is turned about its handle in the hand (a sword's flat to the eye, not its edge)
};
static const FirstPersonWeapon _first_person_weapons[] = {
	{WalkerItem::Hammer, "kladivo", WeaponUse::Swing, 2.5, 1},
	{WalkerItem::Bow, "luk", WeaponUse::Bow, 0, 1, WalkerItem::Arrows, "sip", 55},
	{WalkerItem::Crossbow, "kuse", WeaponUse::Crossbow, 0, 2, WalkerItem::Bolts, "sipka", 70},
	{WalkerItem::Sword, "mec", WeaponUse::Swing, 2.2, 1, WalkerItem::End, {}, 0, 65},
	{WalkerItem::TwoHandedSword, "mec_obourucni", WeaponUse::Swing, 2.8, 2, WalkerItem::End, {}, 0, 65},
	{WalkerItem::MachineGun, "kulomet", WeaponUse::Burst, 0, 1, WalkerItem::Bullets, "naboj", 300},
	{WalkerItem::RocketLauncher, "raketomet", WeaponUse::Rocket, 0, 6, WalkerItem::Rockets, "raketa", 40},
	{WalkerItem::WarHammer, "kladivo_valecne", WeaponUse::Swing, 2.7, 2},
	{WalkerItem::Pitchfork, "vidle", WeaponUse::Swing, 2.6, 2},
	{WalkerItem::MineHammer, "kladivo_minove", WeaponUse::Swing, 2.0, 6},
	{WalkerItem::FishingRod, "prut_bambus", WeaponUse::Rod, 0, 0},
	{WalkerItem::ShortRod, "prut_kratky", WeaponUse::Rod, 0, 0},
};
/** A fishing rod bent as a fish bites: its figure's name and this (objekty/nase: prut_bambus_ohnuty, prut_kratky_ohnuty); and the hook with its worm. */
static constexpr std::string_view FIRST_PERSON_ROD_BENT = "_ohnuty";
static constexpr std::string_view FIRST_PERSON_HOOK = "hacek_zizala";

/**
 * A light of the walker for its left hand (the colleague's hra/3d/PROSBA-NOC-ZBRANE-TRABANT-LAMPY.md, point 3: "a thing
 * of the category light in the walker's hand shines -- warm orange, about 8 m -- in the left hand, so that a weapon
 * goes in the right, as in Morrowind"; the player: "take the torch too"): the thing of the inventory, its thing of
 * the indices, its light.
 */
struct FirstPersonLight {
	WalkerItem item;        ///< the thing of the inventory
	std::string_view thing; ///< its thing of the indices ("kategorie": "svetlo")
	uint32_t colour;        ///< its light's colour, 0xRRGGBB
	double reach_m;         ///< how far it lights, metres
};
static const FirstPersonLight _first_person_lights[] = {
	{WalkerItem::Torch, "pochoden", 0xFF9A40, 8.0},
	{WalkerItem::Lantern, "lucerna", 0xFFC070, 8.0},
	{WalkerItem::PaperLantern, "lucerna_papirova", 0xFFB24A, 6.0},
	{WalkerItem::IronLantern, "lucerna_kovana", 0xFFB24A, 7.0},
};

/** An arrow or a bolt the walker shot: flying, or stuck where it hit. Only the screen, never saved. */
struct FirstPersonMissile {
	std::string_view thing; ///< its thing of the indices, its +x its tip, its origin its nock
	int damage = 1;         ///< the health it takes of a dragon it hits
	double x = 0, y = 0, z = 0;    ///< its origin, world units and units of height
	double vx = 0, vy = 0, vz = 0; ///< how fast it goes, metres a second
	double length_m = 0.75; ///< from its nock to its tip
	double age = 0;         ///< seconds flying, or since it stuck
	bool stuck = false;     ///< stuck in the ground or a wall
	bool gone = false;      ///< in a dragon, in the water or off the map: gone
	float facing = 0;       ///< degrees in the world its tip points to
	float pitch = 0;        ///< degrees its tip is raised
	bool rocket = false;    ///< a rocket: straight on, smoke behind it, a blast where it hits (Explode())
	double smoke_time = 0;  ///< a rocket: seconds till its next puff of smoke
};

/** A puff of a rocket's smoke, or its blast: only the screen, never saved. */
struct FirstPersonBlast {
	double x = 0, y = 0, z = 0; ///< where, world units and units of height
	double age = 0;             ///< seconds
	bool smoke = true;          ///< a puff of smoke; else the blast
	double size = 1;            ///< a puff: how many metres it grows; the cloud a blast leaves, more and darker, a while longer
	double vx = 0, vy = 0, vz = 0; ///< how it drifts, metres a second: a dragon's smoke and fire out of its mouth, rising
	bool flame = false;         ///< not a puff: a flame of a dragon's fire, flying out and growing by size, not a blast
	bool lights = true;         ///< a flame: it lights what is about it too (one of a few, many fly together)
	uint32_t colour = 0;        ///< a puff's colour, 0xRRGGBB; 0 as its size says

	/** Seconds it is seen. */
	double Life() const { return this->smoke ? FIRST_PERSON_PUFF_S * std::sqrt(this->size) : this->flame ? FIRST_PERSON_FLAME_S : FIRST_PERSON_BLAST_S; }
};

/** A dragon of a dragon farm fed (the player, 26. 10.: "the dragon smokes or breathes out fire"), as it goes on. */
struct FirstPersonFeeding {
	uint32_t place = 0;  ///< the dragon (ThreeDRestingDragonAt())
	WalkerItem item = WalkerItem::End; ///< what it was fed: the marijuana or the beer
	double age = 0;      ///< seconds since
	double mouth = 0;    ///< seconds till the next puff or flame out of its mouth
	double round = 0;    ///< seconds till the next puff about it

	/** Seconds it goes on. */
	double Life() const { return this->item == WalkerItem::Beer ? FIRST_PERSON_GULP_S + FIRST_PERSON_FIRE_S + FIRST_PERSON_AFTER_FIRE_S : FIRST_PERSON_SMOKE_S; }
};

/**
 * The angle of a swing of the hammer a time into it, degrees (DrawThreeDHeldThing()): raised back over the shoulder,
 * struck forward and down, and back to rest.
 */
static float SwingAngle(double t)
{
	if (t < 0.15) return static_cast<float>(-40.0 * t / 0.15);
	if (t < 0.27) return static_cast<float>(-40.0 + 110.0 * (t - 0.15) / 0.12);
	if (t < FIRST_PERSON_SWING_S) return static_cast<float>(70.0 * (1.0 - (t - 0.27) / (FIRST_PERSON_SWING_S - 0.27)));
	return 0.0f;
}

static constexpr NWidgetPart _nested_first_person_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_FP_CAPTION), SetStringTip(STR_FIRST_PERSON_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_FP_FULL), SetSpriteTip(SPR_LARGE_SMALL_WINDOW, STR_FIRST_PERSON_FULL_TOOLTIP), SetAspect(1),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(WWT_EMPTY, Colours::Invalid, WID_FP_CANVAS), SetMinimalSize(480, 300), SetResize(1, 1), SetFill(1, 1),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_HERE), SetStringTip(STR_FIRST_PERSON_HERE, STR_FIRST_PERSON_HERE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_CENTRE), SetStringTip(STR_FIRST_PERSON_CENTRE, STR_FIRST_PERSON_CENTRE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		/* In a vehicle its route instead of running (the player, 27. 10.: "put the button 'go your route' there"). */
		NWidget(NWID_SELECTION, Colours::Invalid, WID_FP_RUN_SEL),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_FP_RUN), SetStringTip(STR_FIRST_PERSON_RUN, STR_FIRST_PERSON_RUN_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
			/* And loading and unloading at a stop (the player: "naložit, vyložit"). */
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_ROUTE), SetStringTip(STR_CAB_VIEW_AUTO, STR_CAB_VIEW_AUTO_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_FP_LOAD), SetStringTip(STR_CAB_VIEW_LOAD_CARGO, STR_CAB_VIEW_LOAD_CARGO_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_FP_UNLOAD), SetStringTip(STR_CAB_VIEW_UNLOAD_CARGO, STR_CAB_VIEW_UNLOAD_CARGO_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
				/* And the camera, as the cab view's (the player, 28. 10.: "give a button camera"). */
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_CAMERA), SetStringTip(STR_CAB_VIEW_CAMERA, STR_CAB_VIEW_CAMERA_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
				/* And in a train of MSTS the switch of its controls, as the cab view's (the player, 31. 10.: "add the
				 * controls MSTS to E"). */
				NWidget(NWID_SELECTION, Colours::Invalid, WID_FP_CONTROLS_SEL),
					NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_FP_CONTROLS), SetStringTip(STR_CAB_VIEW_CONTROLS, STR_CAB_VIEW_CONTROLS_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
				EndContainer(),
			EndContainer(),
		EndContainer(),
		NWidget(WWT_PUSHTXTBTN, Colours::Brown, WID_FP_INVENTORY), SetStringTip(STR_FIRST_PERSON_INVENTORY, STR_FIRST_PERSON_INVENTORY_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

/** A house's 3D object as the view from the eyes has it (ThreeDGroupShown() by the setting for the eyes): what the walker walks about, goes into, looks at. */
static const ThreeDObject *EyeHouseObject(HouseID north, TileIndex origin)
{
	const ThreeDEyeScope eye;
	return GetThreeDHouseObjectAt(north, origin);
}

/**
 * Whether the walker can stand on a world point: not in a house, an industry's building (a field is walked), a
 * station's building (a platform or a stop is walked), an object, the water or off the map.
 * @param stand the height it stands at there, units of height (FirstPersonStandZ()): up on a building (the dragon
 *              castle's wall walks) only what stands over its feet is in its way; lower than the building's ground, on it
 */
static bool FirstPersonWalkable(double x, double y, double stand = -std::numeric_limits<double>::infinity())
{
	if (x < TILE_SIZE || y < TILE_SIZE || x >= (Map::SizeX() - 1) * TILE_SIZE || y >= (Map::SizeY() - 1) * TILE_SIZE) return false;
	TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	/* On the deck of a bridge over the tile (FirstPersonStandZ()), whatever is under it is not in the way: the river, the
	 * sea, a railway (the player, 26. 10.: up the ramp onto the road bridge, and no further along it). */
	if (IsBridgeAbove(tile) && stand >= GetBridgePixelHeight(GetSouthernBridgeEnd(tile)) - 0.1 * OBJECT_HEIGHT_PER_METRE) return true;
	/* Under the land in a tunnel (FirstPersonTunnelFloor(); the player, 8. 11.: "I cannot walk through the tunnel on
	 * foot"): within its tube, not into the rock beside it. */
	if (std::isfinite(stand) && stand < FirstPersonGroundZ(x, y) - TILE_HEIGHT / 2.0) return FirstPersonTunnelFloor(x, y, stand).has_value();
	/* A walker's tent: in through its entrance, not through its cloth; round its fireplace and its barrel (WalkerTent). */
	if (WalkerTentBlocks(x, y)) return false;
	/* The feet over the ground of a building whose north tile is origin. */
	auto feet = [stand](TileIndex origin) { return std::max(0.0, stand - GetTileMaxPixelZ(origin)); };
	switch (GetTileType(tile)) {
		case TileType::Void:
		case TileType::Object:
			return false;
		case TileType::House: {
			/* A house that is a 3D object: by its shape (ThreeDObject::Blocks()), into its park, under its roof. */
			if (!IsHouseCompleted(tile)) return false;
			HouseID north = GetHouseType(tile);
			const TileIndex origin = tile + GetHouseNorthPart(north);
			const ThreeDObject *obj = EyeHouseObject(north, origin);
			if (obj == nullptr) return false;
			const double lx = x - TileX(origin) * TILE_SIZE, ly = y - TileY(origin) * TILE_SIZE;
			const double up = feet(origin);
			if (obj->Blocks(lx, ly, up)) return false;
			/* Its interior's walls and seats, and the wings of its doors as they stand (the colleague's hra/3d/PROSBA-INTERIERY.md). */
			if (const ThreeDObject *in = GetThreeDInterior(obj); in != nullptr && in->IsRead() && in->Blocks(lx, ly, up)) return false;
			for (size_t k = 0; k < obj->door_wings.size(); k++) {
				if (GetThreeDDoorWing(obj, origin, k)->Blocks(lx, ly, up)) return false;
			}
			return true;
		}
		case TileType::Water:
			/* A coast, and the bank a river or a canal has along the land (EyeOnRiverBank()). */
			return IsCoast(tile) || EyeOnRiverBank(x, y);
		case TileType::Industry: {
			const IndustryGfx gfx = GetIndustryGfx(tile);
			if (gfx == GFX_MARIJUANA_PLANTATION || gfx == GFX_POTATO_FIELD) return true;
			/* The forest with a clearing: among the trunks of its trees, round the cabin, the high seat and the logs. */
			if (const ThreeDForestPart *forest = ThreeDGameForestPartAt(tile); forest != nullptr) {
				const double lx = x - TileX(tile) * TILE_SIZE, ly = y - TileY(tile) * TILE_SIZE;
				const ThreeDObject *obj = forest->ground;
				if (obj == nullptr) return true;
				const double up = feet(tile);
				if (obj->Blocks(lx, ly, up)) return false;
				/* The cabin's interior and the wings of its door, as a house's. */
				if (const ThreeDObject *in = GetThreeDInterior(obj); in != nullptr && in->IsRead() && in->Blocks(lx, ly, up)) return false;
				for (size_t k = 0; k < obj->door_wings.size(); k++) {
					if (GetThreeDDoorWing(obj, tile, k)->Blocks(lx, ly, up)) return false;
				}
				return true;
			}
			/* One of the game's own that is a 3D object (the pub, the distillery): by its shape, as a house. Being built a
			 * wall, but the forest: its trees stand at once (ThreeDOriginalIndustryTileStands()). */
			const Industry *ind = Industry::GetByTile(tile);
			const ThreeDObject *obj = IsIndustryCompleted(tile) ? GetThreeDIndustryObjectOf(ind) : nullptr;
			const TileIndex origin = GetThreeDIndustryObjectOrigin(ind);
			if (obj == nullptr || GetThreeDIndustryYard(ind, tile) != nullptr) {
				/* One of the original game from the parts the industries share (the forest; the refinery's tanks beside
				 * the distillery it shows): by its buildings' shapes and its trees' trunks. */
				const std::optional<bool> blocks = ThreeDOriginalIndustryBlocks(tile, x, y);
				return blocks.has_value() && !*blocks;
			}
			const double lx = x - TileX(origin) * TILE_SIZE, ly = y - TileY(origin) * TILE_SIZE;
			const double up = feet(origin);
			if (obj->Blocks(lx, ly, up)) return false;
			/* Its interior's straw and troughs, and the wings of its doors, as a house's (the colleague's dragon castle). */
			if (const ThreeDObject *in = GetThreeDInterior(obj); in != nullptr && in->IsRead() && in->Blocks(lx, ly, up)) return false;
			for (size_t k = 0; k < obj->door_wings.size(); k++) {
				if (GetThreeDDoorWing(obj, origin, k)->Blocks(lx, ly, up)) return false;
			}
			return true;
		}
		case TileType::Station:
			return HasStationRail(tile) || IsAnyRoadStop(tile);
		default:
			return true;
	}
}

/**
 * How high what the walker cannot stand on at a world point reaches, units of height over the ground: a house or an
 * industry that is a 3D object, its top (ThreeDObject::top); anything else, as a house of four floors.
 */
static double FirstPersonWallTop(double x, double y)
{
	const double floors = 12 * OBJECT_HEIGHT_PER_METRE;
	if (x < TILE_SIZE || y < TILE_SIZE || x >= (Map::SizeX() - 1) * TILE_SIZE || y >= (Map::SizeY() - 1) * TILE_SIZE) return floors;
	const TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	const ThreeDObject *obj = nullptr;
	if (IsTileType(tile, TileType::House) && IsHouseCompleted(tile)) {
		HouseID north = GetHouseType(tile);
		obj = EyeHouseObject(north, tile + GetHouseNorthPart(north));
	} else if (IsTileType(tile, TileType::Industry) && IsIndustryCompleted(tile)) {
		obj = GetThreeDIndustryObjectOf(Industry::GetByTile(tile));
	}
	return obj != nullptr && obj->top > 0 ? obj->top : floors;
}

/**
 * Whether the walker, a circle, can stand on a world point: its middle and eight points round it, so that the thin
 * walls of a house's shape (ThreeDObject::Blocks()) slip between none of them.
 */
/**
 * Whether a world point is on the plot of a house or an industry with an interior (the colleague's hra/3d/PROSBA-INTERIERY.md): there
 * the walker is as narrow as a body (FIRST_PERSON_INDOOR_RADIUS) -- through its doorways, between the rows of seats of
 * the cinema.
 */
static bool FirstPersonIndoors(double x, double y)
{
	if (x < TILE_SIZE || y < TILE_SIZE || x >= (Map::SizeX() - 1) * TILE_SIZE || y >= (Map::SizeY() - 1) * TILE_SIZE) return false;
	TileIndex origin;
	return GetThreeDInterior(GetThreeDObjectWithRoomsAt(TileVirtXY(static_cast<int>(x), static_cast<int>(y)), origin)) != nullptr;
}

static bool FirstPersonFree(double x, double y, double stand = -std::numeric_limits<double>::infinity())
{
	if (!FirstPersonWalkable(x, y, stand)) return false;
	/* Indoors, and up on stairs or a wall walk (standing over the ground) or at their foot (a step under its circle), a
	 * body's half-width: the ruin's stairs are a metre wide between their stringers, the high seat's 0.8 m (the
	 * colleague's hra/3d/HRAD-ZRICENINA-POSED-PRO-HRU.md). */
	auto over_ground = [stand](double px, double py) {
		return FirstPersonStandZ(px, py, stand) > FirstPersonGroundZ(px, py) + 0.1 * OBJECT_HEIGHT_PER_METRE;
	};
	bool narrow = std::isfinite(stand) && (stand > FirstPersonGroundZ(x, y) + 0.1 * OBJECT_HEIGHT_PER_METRE);
	for (int k = 0; k < 8 && !narrow && std::isfinite(stand); k++) {
		narrow = over_ground(x + FIRST_PERSON_RADIUS * std::cos(k * M_PI / 4), y + FIRST_PERSON_RADIUS * std::sin(k * M_PI / 4));
	}
	const double radius = narrow || FirstPersonIndoors(x, y) ? FIRST_PERSON_INDOOR_RADIUS : FIRST_PERSON_RADIUS;
	for (int k = 0; k < 8; k++) {
		const double a = k * M_PI / 4;
		if (!FirstPersonWalkable(x + radius * std::cos(a), y + radius * std::sin(a), stand)) return false;
	}
	return true;
}

/** The company's own tent whose entrance the walker stands at, to fold it with E (WalkerTent); nullptr none. */
static const WalkerTent *FirstPersonTentToStrike(double x, double y)
{
	const WalkerTent *t = WalkerTentEntranceNear(x, y, WALKER_TENT_REACH_M);
	return t != nullptr && t->owner == _local_company.base() ? t : nullptr;
}

/**
 * The walker where it stands now: up the stairs or down from where it stood a step before (FirstPersonStandZ()), on
 * the ground storey where it was put somewhere else (FirstPersonGroundZ()).
 */
static void FirstPersonSettle(FirstPersonEye &w)
{
	const bool moved = std::hypot(w.x - w.stand_x, w.y - w.stand_y) > FIRST_PERSON_SETTLE_M * OBJECT_UNITS_PER_METRE;
	w.stand_z = moved ? FirstPersonGroundZ(w.x, w.y) : FirstPersonStandZ(w.x, w.y, w.stand_z);
	w.stand_x = w.x;
	w.stand_y = w.y;
}

/**
 * How much higher the land may be where the walker steps than where it stands, units of height: a foundation's side is
 * a retaining wall, from the eyes a level of the land 5 m (EYE_LEVEL_STRETCH; the player, 4. 11.: "it will be a wall");
 * the walker does not climb it, it goes round to where it is low. Over the game's whole units of the ground on a slope
 * (a unit at a step now and then), less than a metre from the eyes; down from it, it jumps.
 */
static constexpr double FIRST_PERSON_WALL_UP = 1.5;

/** Whether the walker can go to a world point from where it stands, at the height it would stand at there. */
static bool FirstPersonFreeFrom(const FirstPersonEye &w, double x, double y)
{
	const double feet = FirstPersonFeetZ(w);
	const double stand = FirstPersonStandZ(x, y, feet);
	if (stand - feet > FIRST_PERSON_WALL_UP) return false;
	return FirstPersonFree(x, y, stand);
}

/**
 * The walker moved by a piece of its step where it can go: straight; else along a wall, each way on its own; else round
 * what stands a little to one side of its way, aslant by up to FIRST_PERSON_SIDESTEP aside, the nearest that is free --
 * as one walks round a post. The ruin's lookout: its stairs end at a corner of its platform, the last post of
 * its railing a tenth of a metre into their width, half of them by the platform's slanting edge; the walker up the
 * middle of the stairs stopped at the post (the player, 26. 10.: "I get stuck at the top, the last bit onto the
 * platform of the lookout").
 * @return whether it moved
 */
static bool FirstPersonMove(FirstPersonEye &w, double px, double py)
{
	const double x = w.x, y = w.y;
	auto go = [&w](double nx, double ny) {
		if (!FirstPersonFreeFrom(w, nx, ny)) return false;
		w.x = nx;
		w.y = ny;
		return true;
	};
	if (go(x + px, y + py)) return true;
	const double len = std::hypot(px, py);
	if (len <= 0) return false;
	/* Along a wall, where the way goes along it at least a little (straight at it, the slide is nowhere). */
	static constexpr double SLIDE_AT_LEAST = 0.3;
	if (std::abs(px) >= SLIDE_AT_LEAST * len && go(x + px, y)) return true;
	if (std::abs(py) >= SLIDE_AT_LEAST * len && go(x, y + py)) return true;
	/*
	 * Round it, to the left of the way or to the right, the nearest first: aslant, nothing standing halfway there (the
	 * body is wider than half the step, nothing thin is stepped through) at the height of the feet or of the stair
	 * stepped onto, a floor under it or not -- off the top of the ruin's stairs one steps aslant onto the platform of
	 * its lookout, the stairs end at a corner of it, half of them by its edge; or aside first, where it goes on from
	 * there -- between the post and the handrail of the stairs there is a hand's breadth to spare.
	 */
	const double sx = -py / len, sy = px / len;
	const double feet = FirstPersonFeetZ(w);
	for (int k = 1; k <= FIRST_PERSON_SIDESTEPS; k++) {
		for (const int side : {1, -1}) {
			const double ox = sx * side * FIRST_PERSON_SIDESTEP * k / FIRST_PERSON_SIDESTEPS;
			const double oy = sy * side * FIRST_PERSON_SIDESTEP * k / FIRST_PERSON_SIDESTEPS;
			const double on = FirstPersonStandZ(x + ox + px, y + oy + py, feet);
			if (FirstPersonFree(x + (ox + px) / 2, y + (oy + py) / 2, std::max(feet, on)) && go(x + ox + px, y + oy + py)) return true;
			const double aside = FirstPersonStandZ(x + ox, y + oy, feet);
			if (FirstPersonFree(x + ox + px, y + oy + py, FirstPersonStandZ(x + ox + px, y + oy + py, aside)) && go(x + ox, y + oy)) return true;
		}
	}
	/* Straight at a wall, at least the little along it there is. */
	return (std::abs(px) > 0 && std::abs(px) < SLIDE_AT_LEAST * len && go(x + px, y)) || (std::abs(py) > 0 && std::abs(py) < SLIDE_AT_LEAST * len && go(x, y + py));
}

/**
 * The engine of the game a vehicle of the sets standing in a yard is of ("vozidlo:<record>|...",
 * GetThreeDStandingVehicle()): of the climate and the road, its first piece the record's object, one the company
 * could buy now rather; Invalid when none (its set not loaded).
 */
static EngineID YardVehicleEngine(std::string_view figure)
{
	if (!figure.starts_with("vozidlo:")) return EngineID::Invalid();
	std::string_view record = figure.substr(8);
	record = record.substr(0, record.find('|'));
	EngineID found = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->IsEnabled() || RoadTypeIsTram(e->VehInfo<RoadVehicleInfo>().roadtype) || !IsValidCargoType(e->GetDefaultCargoType())) continue;
		int piece = 0;
		const ThreeDVehicleObject *obj = GetThreeDVehicleObject(e->index, &piece);
		if (obj == nullptr || piece != 0 || obj->name != record) continue;
		if (IsEngineBuildable(e->index, VehicleType::Road, _local_company)) return e->index;
		if (found == EngineID::Invalid()) found = e->index;
	}
	return found;
}

void ShowWalkerInventoryWindow();
static void ShowDragonFeedWindow(uint32_t place);
static void ShowForestGrillWindow(TileIndex fire);
TileIndex FirstPersonShopNearby();
void RedrawFirstPersonPicture();

/** The window of the view from the first person; there is one, number 0. */
/** A submarine steered a metre up (-1) or down (1) from where it is going (ShipDiveM()): as deep as there is room. */
static void SteerDive(const Vehicle *rv, int way)
{
	Vehicle *v = Vehicle::Get(rv->index);
	v->cab_dive_to = static_cast<float>(std::clamp(static_cast<double>(v->cab_dive_to) + way, 0.0, ShipDiveRoomM(v)));
}

/** A submarine going up or down to where it was steered, FIRST_PERSON_DIVE_M_S at most; up over a shallower bed. */
static void DiveTowards(const Vehicle *rv, double seconds)
{
	if (rv->type != VehicleType::Ship || (rv->cab_dive == 0 && rv->cab_dive_to == 0)) return;
	Vehicle *v = Vehicle::Get(rv->index);
	const double room = ShipDiveRoomM(v);
	v->cab_dive_to = static_cast<float>(std::min<double>(v->cab_dive_to, room));
	const double step = FIRST_PERSON_DIVE_M_S * seconds;
	v->cab_dive = static_cast<float>(std::min(room, v->cab_dive < v->cab_dive_to ? std::min<double>(v->cab_dive_to, v->cab_dive + step) : std::max<double>(v->cab_dive_to, v->cab_dive - step)));
}

struct FirstPersonWindow : Window {
	FirstPersonEye walker{};             ///< where the walker is and looks
	mutable CabViewCamera camera{};      ///< the eye between frames
	mutable CabViewPicture picture{};    ///< the last picture
	mutable bool picture_ok = true;       ///< whether the last picture could be made (32 bpp)
	bool full = false;                    ///< over the whole screen, between the toolbar and the status bar
	Rect before_full{};                   ///< where it was before that
	bool running = false;                 ///< runs instead of walking (also while Shift is held)
	int walk_on = 0;                      ///< walks on by itself, 1 ahead, -1 back (Up or Down pressed twice), 0 not
	bool walk_held[3] = {};               ///< Up, Down, Left or Right held at the last step (their presses, Walk())
	double turn_held = 0;                 ///< how long Left or Right has been held, seconds, for the turn speed's run-up
	double seen_heading = 0;              ///< the heading and the pitch at the last picture, for the look ahead (Step())
	double seen_pitch = 0;
	double look_ahead_heading = 0;        ///< the look ahead the picture is made with (Step()), radians
	double look_ahead_pitch = 0;
	double walk_tapped[2] = {FIRST_PERSON_DOUBLE_TAP_S, FIRST_PERSON_DOUBLE_TAP_S}; ///< seconds since Up, Down was pressed
	bool weapon_out = false;              ///< the weapon taken out (F, or its key), in the hand
	int weapon = 0;                       ///< the weapon in the hand or put away (_first_person_weapons)
	int wanted = 0;                       ///< the weapon asked for (its key): the one in the hand is put away first
	double swing_time = -1;               ///< seconds into a swing of the weapon, -1 when it is not swung
	double draw_time = -1;                ///< seconds the bow is drawn, -1 when it is not
	bool mouse_draw = false;              ///< the bow drawn with the left button held: let go, it shoots
	double reload_time = 0;               ///< seconds till the next arrow is on the string, the crossbow drawn again
	float kick = 0;                       ///< a shot's kick, 1 as it is shot to 0
	bool firing = false;                  ///< the machine gun firing (Attack(), Hold())
	bool mouse_fire = false;              ///< fired with the left button held: firing till it is let go
	int burst_left = 0;                   ///< shots it fires yet (a burst of Space), or many with the button
	double burst_time = 0;                ///< seconds till its next shot
	std::vector<FirstPersonMissile> missiles; ///< the arrows and the bolts shot
	std::vector<FirstPersonBlast> blasts;  ///< the rockets' smoke and blasts
	std::vector<FirstPersonFeeding> feedings; ///< the farms' dragons fed, smoking or breathing fire
	FishingStage fishing = FishingStage::Ready; ///< how the walker fishes with the rod in the hand
	double fish_time = 0;                 ///< seconds in that stage
	double fish_bites_at = 0;             ///< waiting: the second in it a fish bites
	double hook_x = 0, hook_y = 0, hook_z = 0; ///< the hook flying or on the water: world units, units of height (its eyelet)
	double cast_x = 0, cast_y = 0, cast_z = 0; ///< where it flew from (the rod's tip about)
	double land_x = 0, land_y = 0;        ///< where it falls onto the water
	double water_z = 0;                   ///< the surface of the water it falls onto, units of height
	int rod_fish = -1;                    ///< the fish biting or caught (_first_person_rod_fish), -1 none
	int rod_fish_colour = 0;              ///< its colour of its palette (ThreeDVehicleObject::PaletteState())
	std::string fish_say;                 ///< a word about the fishing shown a moment (FIRST_PERSON_FISH_SAY_S), empty none
	double fish_say_left = 0;             ///< seconds it is shown yet
	int hurt_steps = 0;                   ///< steps the picture is still red of a dragon's blow
	uint64_t nurse_healed = 0;            ///< the tick of the game the walker's nurse last healed it (or began to be able to)
	int light = -1;                       ///< the light asked for in the left hand (_first_person_lights), -1 none: the one held put out first
	int light_held = -1;                  ///< the light in the left hand or being put out, -1 none
	double light_time = 0;                ///< seconds it burns, for its flame's frames
	std::chrono::steady_clock::time_point look_clicked{}; ///< when the right button was last pressed without a look about after it
	bool looked = false;                  ///< the view moved since the right button was pressed (OnScroll())
	double head = 0;                      ///< radians the head is turned by the right button from where the body faces, left positive
	/*
	 * In a vehicle (the player, 17. 10.: "in the first person game, E gets into the car, into the cab view; no other
	 * window of the cab view should open, it stays in the window of the first person -- a special window, the game
	 * under it is not drawn"): the picture is the view from its cab, the arrows drive it as from the cab view when it
	 * is the company's (else the walker rides along), E again gets out beside it.
	 */
	VehicleID riding = VehicleID::Invalid(); ///< the vehicle the walker is in, Invalid on foot
	CabArrows arrows{};                      ///< the arrows driving it
	mutable SimtrainsCabSeat seat{};         ///< a train of MSTS: the player at its desk, as in the cab view (simtrains_cab.h)
	CabLook cab_look{};                      ///< the head turned in it with the right button, as in the cab view
	/*
	 * On board a boat (the player, 7. 11.: "the second E, to stop steering, must not put me into the water"; the
	 * colleague's hra/3d/LODE-3D-PRO-HRU.md: "drive it from the cab or walk about the boat"): the walker stands on its
	 * deck and goes with it, its place kept in the boat's frame, its height on the boat's level faces
	 * (ThreeDVehicleDeckZ()), its walls in its way (ThreeDVehicleBlocks()).
	 */
	VehicleID aboard = VehicleID::Invalid(); ///< the boat the walker stands on, Invalid elsewhere
	double deck_x = 0, deck_y = 0, deck_z = 0; ///< where on it, metres of its frame (+x its bow), and its feet's height there
	double deck_heading = 0;                 ///< radians the walker faces from the boat's bow
	double deck_helm = 0;                    ///< the height of its helmsman's place (StandOnBoard()), metres of its frame: no deeper than a metre under it

	/** The vehicle the walker is in, nullptr on foot. */
	const Vehicle *Riding() const
	{
		return this->riding != VehicleID::Invalid() ? Vehicle::GetIfValid(this->riding) : nullptr;
	}

	/** The boat the walker stands on and how it lies now (GetThreeDEyeVehicle()), nullptr when there is none (gone, sunk). */
	const Vehicle *Aboard(ThreeDEyeVehicle &ev) const
	{
		const Vehicle *v = this->aboard != VehicleID::Invalid() ? Vehicle::GetIfValid(this->aboard) : nullptr;
		if (v == nullptr || v->vehstatus.Test(VehState::Crashed) || !GetThreeDEyeVehicle(v, ev) || ev.obj == nullptr) return nullptr;
		return v;
	}

	/** The walker where it stands on the boat as the boat lies now: its place, height and facing in the world. */
	void PlaceOnBoard(const ThreeDEyeVehicle &ev)
	{
		double x, y, z;
		BoatToWorld(ev, this->deck_x, this->deck_y, this->deck_z, x, y, z);
		this->walker.x = x;
		this->walker.y = y;
		this->walker.stand_z = z;
		this->walker.stand_x = x;
		this->walker.stand_y = y;
		this->walker.heading = std::remainder(ev.facing * M_PI / 180.0 + this->deck_heading, 2 * M_PI);
	}

	/**
	 * Letting go of a boat's wheel (GetOut()): the walker stays on board where its helmsman stands (the index's "ridic"
	 * "poloha"), else in its middle, facing its bow.
	 * @return false when it is no boat of a 3D object with a deck there: out beside it as from a car
	 */
	bool StandOnBoard(const Vehicle *v)
	{
		if (v->type != VehicleType::Ship) return false;
		ThreeDEyeVehicle ev;
		if (!GetThreeDEyeVehicle(v, ev) || ev.obj == nullptr) return false;
		const ThreeDVehicleObject &obj = *ev.obj;
		const double hx = obj.has_helm ? obj.helm[0] : 0, hy = obj.has_helm ? obj.helm[1] : 0, hz = obj.has_helm ? obj.helm[2] : 0;
		const std::optional<double> deck = ThreeDVehicleDeckZ(WalkedOn(obj), hx, hy, hz + FIRST_PERSON_DECK_STEP_M);
		if (!deck.has_value()) return false;
		this->aboard = v->index;
		this->walker.swimming = false;
		this->deck_x = hx;
		this->deck_y = hy;
		this->deck_z = *deck;
		StandUpFromHelm(WalkedOn(obj), ThreeDBoatRooms(obj) != nullptr, this->deck_x, this->deck_y, this->deck_z);
		this->deck_helm = obj.has_helm ? hz : *deck;
		this->deck_heading = 0;
		this->head = 0;
		this->PlaceOnBoard(ev);
		return true;
	}

	/**
	 * Up from the helmsman's seat onto the floor beside it, when no step leads off it (MoveOnBoard()): the seat stands
	 * higher over the floor than a step down (the sailing yacht twice as big, its seat 1.1 m over its cockpit; the player,
	 * test4: "something keeps me from walking on the deck when I press E, stop driving"). The floor nearest it, lower,
	 * within 1.5 m and no wall at the chest between; else on the seat as before.
	 * @param obj what of the boat the walker walks on (WalkedOn())
	 * @param rooms whether that is its rooms (under their ceilings)
	 * @param x,y,z where the walker stands, metres of the boat's frame: moved there
	 */
	static void StandUpFromHelm(const ThreeDVehicleObject &obj, bool rooms, double &x, double &y, double &z)
	{
		auto walkable = [&](double px, double py, double floor) {
			if (!DeckFooting(obj, px, py, floor)) return false;
			return !rooms || RoomAt(obj, px, py, floor) >= FIRST_PERSON_ROOM_MIN_M;
		};
		/* Where steps lead from there, a quarter of a metre each, within 2.5 m: past 1.5 m, it is not shut in. */
		static constexpr int N = 10;
		static constexpr double CELL = 0.25;
		std::array<std::optional<double>, (2 * N + 1) * (2 * N + 1)> reached{};
		std::vector<std::pair<int, int>> todo{{0, 0}};
		reached[N * (2 * N + 1) + N] = z;
		while (!todo.empty()) {
			const auto [i, j] = todo.back();
			todo.pop_back();
			const double from = *reached[(j + N) * (2 * N + 1) + i + N];
			if (std::hypot(i, j) * CELL > 1.5) return;
			for (const auto &[di, dj] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
				const int ni = i + di, nj = j + dj;
				if (std::abs(ni) > N || std::abs(nj) > N || reached[(nj + N) * (2 * N + 1) + ni + N].has_value()) continue;
				const double ax = x + i * CELL, ay = y + j * CELL, px = x + ni * CELL, py = y + nj * CELL;
				const std::optional<double> step = ThreeDVehicleDeckZ(obj, px, py, from + FIRST_PERSON_DECK_STEP_M);
				if (!step.has_value() || *step < from - FIRST_PERSON_DECK_DROP_M || !walkable(px, py, *step)) continue;
				if (ThreeDVehicleBlocks(obj, ax, ay, px, py, std::max(from, *step) + FIRST_PERSON_DECK_CHEST_M)) continue;
				reached[(nj + N) * (2 * N + 1) + ni + N] = *step;
				todo.emplace_back(ni, nj);
			}
		}
		for (int ring = 1; ring <= 6; ring++) {
			const double r = ring * 0.25;
			for (int k = 0; k < ring * 8; k++) {
				const double a = k * 2 * M_PI / (ring * 8), px = x + r * std::cos(a), py = y + r * std::sin(a);
				/* A floor lower than the seat, open over it (not under the seat). */
				const std::optional<double> floor = ThreeDVehicleDeckZ(obj, px, py, z + FIRST_PERSON_DECK_STEP_M);
				if (!floor.has_value() || *floor > z - 0.2 || *floor < z - 2.5 || !walkable(px, py, *floor)) continue;
				if (ThreeDVehicleBlocks(obj, x, y, px, py, *floor + FIRST_PERSON_DECK_CHEST_M)) continue;
				x = px;
				y = py;
				z = *floor;
				return;
			}
		}
	}

	/** What of a boat the walker walks on: its rooms when it has them (ThreeDBoatRooms(): its whole hull with its cabins), else its object. */
	static const ThreeDVehicleObject &WalkedOn(const ThreeDVehicleObject &obj)
	{
		const ThreeDVehicleObject *rooms = ThreeDBoatRooms(obj);
		return rooms != nullptr ? *rooms : obj;
	}

	/**
	 * How high the room over a floor of a boat is where the walker stands (FIRST_PERSON_ROOM_*): to the lowest ceiling
	 * over its head and round it a little, metres; infinite under the sky.
	 */
	static double RoomAt(const ThreeDVehicleObject &obj, double x, double y, double floor)
	{
		double room = std::numeric_limits<double>::infinity();
		for (const auto &[ox, oy] : {std::pair{0.0, 0.0}, {0.1, 0.0}, {-0.1, 0.0}, {0.0, 0.1}, {0.0, -0.1}}) {
			const std::optional<double> ceiling = ThreeDVehicleCeilingZ(obj, x + ox, y + oy, floor + FIRST_PERSON_ROOM_FLOOR_M);
			if (ceiling.has_value()) room = std::min(room, *ceiling - floor);
		}
		return room;
	}

	/**
	 * The walker's eyes under a low ceiling (the player, 7. 11.: "shrink the player so he fits under the cockpit and the
	 * low ceiling over the stairs does not matter"): in a boat's rooms as high as the room over him lets them, a little
	 * under its ceiling; elsewhere at their height.
	 */
	void FitEyes()
	{
		double eyes = FIRST_PERSON_EYES_M;
		ThreeDEyeVehicle ev;
		if (this->aboard != VehicleID::Invalid() && this->Aboard(ev) != nullptr) {
			if (const ThreeDVehicleObject *rooms = ThreeDBoatRooms(*ev.obj); rooms != nullptr) {
				eyes = std::min(eyes, (RoomAt(*rooms, this->deck_x, this->deck_y, this->deck_z) - FIRST_PERSON_ROOM_OVER_EYES_M) * ev.scale);
			}
		}
		this->walker.eye_z = eyes * OBJECT_HEIGHT_PER_METRE;
	}

	/**
	 * Whether the walker's foot finds a deck at a height at a point of a boat: under two of its corners at least,
	 * something no lower and not a step higher (not the top of a boom, a rail, a winch; over an edge as it steps up, yes).
	 */
	static bool DeckFooting(const ThreeDVehicleObject &obj, double x, double y, double z)
	{
		int corners = 0;
		for (const double fx : {-FIRST_PERSON_DECK_FOOT_M, FIRST_PERSON_DECK_FOOT_M}) {
			for (const double fy : {-FIRST_PERSON_DECK_FOOT_M, FIRST_PERSON_DECK_FOOT_M}) {
				const std::optional<double> under = ThreeDVehicleDeckZ(obj, x + fx, y + fy, z + FIRST_PERSON_DECK_STEP_M);
				if (under.has_value() && *under >= z - FIRST_PERSON_DECK_FOOT_M) corners++;
			}
		}
		return corners >= 2;
	}

	/**
	 * A step on board (Walk()): the way, world units, in the boat's frame; onto the highest deck there the foot finds
	 * (DeckFooting(); the top of the yacht's binnacle no, the seat under it yes) at most a step up or down, no wall
	 * across it at the chest; off its deck nowhere (over its side to the land: E, GoAshore()).
	 */
	void MoveOnBoard(double dx, double dy)
	{
		ThreeDEyeVehicle ev;
		if (this->Aboard(ev) == nullptr) return;
		/* A boat with rooms: their shape, all its hull; down into them and under its low ceilings, as small as they are. */
		const bool rooms = ThreeDBoatRooms(*ev.obj) != nullptr;
		const ThreeDVehicleObject &obj = WalkedOn(*ev.obj);
		double lx, ly;
		WorldToBoat(ev, this->walker.x + dx, this->walker.y + dy, lx, ly);
		std::optional<double> deck;
		double up_to = this->deck_z + FIRST_PERSON_DECK_STEP_M;
		/* Nothing of the boat under the step at all: over its side (OverBoard()). */
		if (!ThreeDVehicleDeckZ(obj, lx, ly, std::numeric_limits<double>::infinity()).has_value()) {
			this->OverBoard(this->walker.x + dx, this->walker.y + dy);
			return;
		}
		for (int tries = 0;; tries++) {
			deck = ThreeDVehicleDeckZ(obj, lx, ly, up_to);
			if (!deck.has_value() || *deck < this->deck_z - FIRST_PERSON_DECK_DROP_M || (!rooms && *deck < this->deck_helm - FIRST_PERSON_DECK_DEPTH_M)) return;
			if (DeckFooting(obj, lx, ly, *deck)) break;
			if (tries == 3) return;
			up_to = *deck - 0.02;
		}
		/* Room over the step: the walker as small as it is, not smaller than he can be (the table, a bunk's shelf). */
		const double room = rooms ? RoomAt(obj, lx, ly, *deck) : std::numeric_limits<double>::infinity();
		if (room < FIRST_PERSON_ROOM_MIN_M) return;
		/* Walls: the way a body's half-width farther, at the chest over the higher deck of the two -- lower as he is. */
		const double len = std::hypot(lx - this->deck_x, ly - this->deck_y);
		const double ex = len > 0 ? lx + (lx - this->deck_x) / len * FIRST_PERSON_DECK_REACH_M : lx;
		const double ey = len > 0 ? ly + (ly - this->deck_y) / len * FIRST_PERSON_DECK_REACH_M : ly;
		const double chest = std::min(FIRST_PERSON_DECK_CHEST_M, FIRST_PERSON_ROOM_CHEST * room);
		if (ThreeDVehicleBlocks(obj, this->deck_x, this->deck_y, ex, ey, std::max(this->deck_z, *deck) + chest)) return;
		this->deck_x = lx;
		this->deck_y = ly;
		this->deck_z = *deck;
		this->PlaceOnBoard(ev);
		if (rooms) this->FitEyes();
	}

	/** Whether the walker on board stands by its wheel (its helmsman's place, else its middle): E takes it there. */
	bool AtHelm(const ThreeDEyeVehicle &ev) const
	{
		const ThreeDVehicleObject &obj = *ev.obj;
		const double hx = obj.has_helm ? obj.helm[0] : 0, hy = obj.has_helm ? obj.helm[1] : 0;
		return std::hypot(this->deck_x - hx, this->deck_y - hy) <= FIRST_PERSON_HELM_M;
	}

	/** Where the walker on board steps over the side onto the land near (a quay, the shore): the nearest place to stand within reach, world units. */
	std::optional<std::pair<double, double>> AshorePlace() const
	{
		const double reach = FIRST_PERSON_BOARD_M * OBJECT_UNITS_PER_METRE;
		for (int ring = 1; ring <= 20; ring++) {
			const double r = reach * ring / 20;
			for (int k = 0; k < ring * 8; k++) {
				const double a = k * 2 * M_PI / (ring * 8);
				const double x = this->walker.x + r * std::cos(a), y = this->walker.y + r * std::sin(a);
				if (FirstPersonFree(x, y)) return std::make_pair(x, y);
			}
		}
		return std::nullopt;
	}

	/** E on board away from the wheel: over the side onto the land near (AshorePlace()). */
	bool GoAshore()
	{
		const auto place = this->AshorePlace();
		if (!place.has_value()) return false;
		this->aboard = VehicleID::Invalid();
		this->walker.x = place->first;
		this->walker.y = place->second;
		FirstPersonSettle(this->walker);
		return true;
	}

	/**
	 * A step off a boat's deck over its side, to a world point: into the water there (IntoWater()); onto a quay or a
	 * shore about as high as the deck; else nowhere.
	 */
	void OverBoard(double x, double y)
	{
		if (EyeWaterSurfaceZ(x, y).has_value()) {
			this->IntoWater(x, y);
			return;
		}
		if (!FirstPersonFree(x, y) || std::abs(FirstPersonGroundZ(x, y) - FirstPersonFeetZ(this->walker)) > FIRST_PERSON_DECK_ASHORE_M * OBJECT_HEIGHT_PER_METRE) return;
		this->aboard = VehicleID::Invalid();
		this->walker.x = x;
		this->walker.y = y;
		this->walker.stand_x = -1;
		FirstPersonSettle(this->walker);
	}

	/** Into the water at a world point (off the shore, a quay, over a boat's side): at its surface, swimming. */
	void IntoWater(double x, double y)
	{
		const std::optional<double> surface = EyeWaterSurfaceZ(x, y);
		if (!surface.has_value()) return;
		this->aboard = VehicleID::Invalid();
		this->walker.swimming = true;
		this->walker.x = x;
		this->walker.y = y;
		this->walker.stand_z = *surface + FIRST_PERSON_SWIM_EYE_OVER_M * OBJECT_HEIGHT_PER_METRE - this->walker.eye_z;
		this->walker.stand_x = x;
		this->walker.stand_y = y;
	}

	/**
	 * A stroke in the water by so much: world units across, units of height up. Its eyes no higher than a little over
	 * the surface, no lower than a little over the bed (EyeWaterDepth()): near the shore, where the bed comes up, the
	 * walker wades. At the shore from the top: out onto it, low and free; else no further that way.
	 * @return whether it moved
	 */
	bool SwimBy(double dx, double dy, double dz)
	{
		double x = this->walker.x + dx, y = this->walker.y + dy;
		std::optional<double> surface = EyeWaterSurfaceZ(x, y);
		if (!surface.has_value()) {
			const std::optional<double> here = EyeWaterSurfaceZ(this->walker.x, this->walker.y);
			if (!here.has_value()) return false;
			/* Where it stands up: on along its way till its circle is all on the land, out of the water (aslant to the
			 * shore farther), a couple of metres at most. */
			const double len = std::hypot(dx, dy);
			const bool at_top = FirstPersonFeetZ(this->walker) + this->walker.eye_z >= *here;
			for (double reach = 1.25; at_top && len > 0 && reach <= 6.0; reach *= 1.6) {
				const double ox = x + dx / len * FIRST_PERSON_RADIUS * reach, oy = y + dy / len * FIRST_PERSON_RADIUS * reach;
				if (!FirstPersonFree(ox, oy) || FirstPersonGroundZ(ox, oy) > *here + FIRST_PERSON_SWIM_CLIMB_M * OBJECT_HEIGHT_PER_METRE) continue;
				this->walker.swimming = false;
				this->walker.x = ox;
				this->walker.y = oy;
				this->walker.stand_x = -1;
				FirstPersonSettle(this->walker);
				return true;
			}
			if (dz == 0) return false;
			x = this->walker.x;
			y = this->walker.y;
			surface = here;
		}
		const double eye = this->walker.eye_z;
		const double top = *surface + FIRST_PERSON_SWIM_EYE_OVER_M * OBJECT_HEIGHT_PER_METRE - eye;
		const double bottom = *surface - EyeWaterDepth(x, y) + FIRST_PERSON_SWIM_BED_M * OBJECT_HEIGHT_PER_METRE - eye;
		this->walker.stand_z = std::max(std::min(FirstPersonFeetZ(this->walker) + dz, top), bottom);
		this->walker.x = x;
		this->walker.y = y;
		this->walker.stand_x = x;
		this->walker.stand_y = y;
		return true;
	}

	/**
	 * Swimming (Walk()): ahead the way the eyes look, up and down with them (pitch), aside level, in pieces as on the
	 * ground; out onto the shore, the rest of the way walking no more this step.
	 */
	void Swim(double ahead, double aside, double dt)
	{
		const double norm = std::sqrt(ahead * ahead + aside * aside);
		const double ms = ((this->running || _shift_pressed) ? FIRST_PERSON_SWIM_RUN_MS : FIRST_PERSON_SWIM_MS) * dt / norm;
		const double fx = std::cos(this->walker.heading), fy = std::sin(this->walker.heading), rx = fy, ry = -fx;
		const double cp = std::cos(this->walker.pitch), sp = std::sin(this->walker.pitch);
		const double dx = (fx * cp * ahead + rx * aside) * ms * OBJECT_UNITS_PER_METRE, dy = (fy * cp * ahead + ry * aside) * ms * OBJECT_UNITS_PER_METRE;
		const double dz = sp * ahead * ms * OBJECT_HEIGHT_PER_METRE;
		const int pieces = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy) / FIRST_PERSON_SUBSTEP)));
		for (int i = 0; i < pieces && this->walker.swimming; i++) {
			if (!this->SwimBy(dx / pieces, dy / pieces, dz / pieces)) break;
		}
	}

	/**
	 * How far a vehicle is from the walker for getting into it: from its middle; a boat of a 3D object from its hull,
	 * its outline in its frame (the yacht is 16 m long: from its middle the walker on the quay beside it was out of reach).
	 */
	double BoardDistance(const Vehicle *u) const
	{
		if (u->type == VehicleType::Ship) {
			ThreeDEyeVehicle ev;
			const ThreeDVehicleShape *shape = GetThreeDEyeVehicle(u, ev) && ev.obj != nullptr ? ev.obj->GetShape() : nullptr;
			if (shape != nullptr) {
				double lx, ly;
				WorldToBoat(ev, this->walker.x, this->walker.y, lx, ly);
				const double ox = std::max({0.0, shape->x0 - lx, lx - shape->x1}), oy = std::max({0.0, shape->y0 - ly, ly - shape->y1});
				return std::hypot(ox, oy) * ev.scale * OBJECT_UNITS_PER_METRE;
			}
		}
		return std::hypot(u->x_pos - this->walker.x, u->y_pos - this->walker.y);
	}

	/**
	 * The vehicle near enough to get into: the nearest going about (a car, a train, a ship, not in its depot), a piece
	 * of it within reach and about as high as the ground under the walker (not on a bridge overhead).
	 */
	const Vehicle *VehicleToBoard() const
	{
		const Vehicle *best = nullptr;
		double best_d = FIRST_PERSON_BOARD_M * OBJECT_UNITS_PER_METRE;
		const double ground = FirstPersonFeetZ(this->walker);
		for (const Vehicle *u : Vehicle::Iterate()) {
			if (u->type != VehicleType::Road && u->type != VehicleType::Train && u->type != VehicleType::Ship) continue;
			if (u->vehstatus.Any({VehState::Hidden, VehState::Crashed})) continue;
			if (std::abs(u->z_pos - ground) > 2 * TILE_HEIGHT) continue;
			const double d = this->BoardDistance(u);
			if (d < best_d) {
				best_d = d;
				best = u->First();
			}
		}
		return best;
	}

	/** Into a vehicle: the picture from its cab from now on, the weapon away. */
	void GetIn(const Vehicle *v)
	{
		this->riding = v->index;
		this->aboard = VehicleID::Invalid();
		this->walker.swimming = false;
		this->walk_on = 0;
		this->head = 0;
		this->weapon_out = false;
		this->swing_time = -1;
		this->draw_time = -1;
		this->arrows = {};
		this->camera = {};
		this->walker.x = v->x_pos;
		this->walker.y = v->y_pos;
		this->MakePicture();
		this->SetDirty();
	}

	/**
	 * Out of the vehicle: the cab lets go of it as when the cab view closes (CabLetGo()), and the walker stands beside
	 * its front, on its left (the driver's door), else on its right, else at the nearest place it can stand, facing
	 * the way the vehicle goes.
	 */
	void GetOut()
	{
		const Vehicle *v = this->Riding();
		this->riding = VehicleID::Invalid();
		this->arrows = {};
		this->camera = {};
		if (v != nullptr) {
			CabLetGo(v);
			/* A boat: the walker stays on board by its wheel, not in the water (StandOnBoard()). */
			if (this->StandOnBoard(v)) {
				this->MakePicture();
				this->SetDirty();
				return;
			}
			const int yaw = v->cab_free ? v->cab_yaw : (v->direction != Direction::Invalid ? DirectionToCabYaw(v->direction) : 0);
			const double heading = yaw * 2 * M_PI / CAB_YAW_TURN, fx = std::cos(heading), fy = std::sin(heading);
			this->walker.heading = std::remainder(heading, 2 * M_PI);
			this->head = 0;
			bool placed = false;
			for (double side : {1.0, -1.0}) {
				for (double metres : {2.5, 3.5, 5.0}) {
					const double d = metres * OBJECT_UNITS_PER_METRE;
					const double x = v->x_pos - fy * d * side, y = v->y_pos + fx * d * side;
					if (!placed && FirstPersonFree(x, y)) {
						this->walker.x = x;
						this->walker.y = y;
						placed = true;
					}
				}
			}
			if (!placed) this->PutNear(v->x_pos, v->y_pos);
		}
		this->MakePicture();
		this->SetDirty();
	}

	/** E: a mushroom at the walker's feet picked (into the inventory, three_d_animals.h); else into the vehicle near, or out of the one the walker is in. */
	/** A door of a house within reach of the walker: the house, its north tile and the door. */
	struct DoorAt {
		const ThreeDObject *house;
		TileIndex origin;
		size_t door;
	};

	/** The door within reach of the walker, E opens or shuts it (the colleague's hra/3d/PROSBA-INTERIERY.md). */
	std::optional<DoorAt> DoorToUse() const
	{
		const int tx = static_cast<int>(this->walker.x / TILE_SIZE), ty = static_cast<int>(this->walker.y / TILE_SIZE);
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (tx + dx < 1 || ty + dy < 1 || tx + dx >= static_cast<int>(Map::MaxX()) || ty + dy >= static_cast<int>(Map::MaxY())) continue;
				TileIndex origin;
				const ThreeDObject *house = GetThreeDObjectWithRoomsAt(TileXY(tx + dx, ty + dy), origin);
				if (house == nullptr || house->door_wings.empty()) continue;
				const auto door = ThreeDDoorNear(house, origin, this->walker.x - TileX(origin) * TILE_SIZE, this->walker.y - TileY(origin) * TILE_SIZE, FIRST_PERSON_DOOR_M * OBJECT_UNITS_PER_METRE);
				if (door.has_value()) return DoorAt{house, origin, *door};
			}
		}
		return std::nullopt;
	}

	/** A vehicle of the sets standing in an industry's yard within reach of the walker: E gives the company one like it there (CmdBuildYardRoadVehicle()). */
	struct YardVehicle {
		TileIndex tile;  ///< the anchor of the industry's block it stands on
		EngineID engine; ///< the engine of the one given
		int32_t px, py;  ///< where it stands, CAB_SUBUNIT of a unit
		uint8_t yaw;     ///< the way it faces, 128ths of a turn
	};

	/**
	 * The vehicle of a yard nearest the walker within reach, shown and of an engine of the game: of an industry's yard
	 * (ThreeDIndustryTileParts::vehicles), of the forest's clearing (ThreeDForestPart::vehicles: the horse by the cabin),
	 * or of a building's event on its days (the stadium's pitch, GetThreeDHouseEvent()).
	 */
	std::optional<YardVehicle> YardVehicleToTake() const
	{
		if (!Company::IsValidID(_local_company)) return std::nullopt;
		const int tx = static_cast<int>(this->walker.x / TILE_SIZE), ty = static_cast<int>(this->walker.y / TILE_SIZE);
		std::optional<YardVehicle> best;
		double best_d = FIRST_PERSON_YARD_VEHICLE_M * OBJECT_UNITS_PER_METRE;
		std::vector<TileIndex> seen;
		/* The vehicles standing about a place: the one nearest so far. A building's is given on its tile it stands on. */
		auto consider = [&](TileIndex origin, std::span<const ThreeDObjectFigure> vehicles, bool on_own_tile) {
			for (const ThreeDObjectFigure &f : vehicles) {
				const double x = TileX(origin) * TILE_SIZE + f.at[0] * OBJECT_UNITS_PER_METRE, y = TileY(origin) * TILE_SIZE + f.at[1] * OBJECT_UNITS_PER_METRE;
				const double d = std::hypot(x - this->walker.x, y - this->walker.y);
				if (d >= best_d) continue;
				const EngineID engine = YardVehicleEngine(f.figure);
				if (engine == EngineID::Invalid()) continue;
				TileIndex given = origin;
				if (on_own_tile) {
					if (x < 0 || y < 0) continue;
					given = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
					if (!IsValidTile(given) || !IsTileType(given, TileType::House)) continue;
				}
				best_d = d;
				const int yaw = static_cast<int>(std::lround(f.facing * CAB_YAW_TURN / 360.0));
				best = YardVehicle{given, engine, static_cast<int32_t>(std::lround(x * CAB_SUBUNIT)), static_cast<int32_t>(std::lround(y * CAB_SUBUNIT)), static_cast<uint8_t>(yaw & (CAB_YAW_TURN - 1))};
			}
		};
		/* The buildings by their north tiles, farther: the stadium's pitch a tile from its north tile. */
		const int reach = FIRST_PERSON_HOUSE_EVENT_TILES;
		for (int dy = -reach; dy <= reach; dy++) {
			for (int dx = -reach; dx <= reach; dx++) {
				if (tx + dx < 1 || ty + dy < 1 || tx + dx >= static_cast<int>(Map::MaxX()) || ty + dy >= static_cast<int>(Map::MaxY())) continue;
				const TileIndex tile = TileXY(tx + dx, ty + dy);
				const bool close = std::abs(dx) <= 1 && std::abs(dy) <= 1;
				if (IsTileType(tile, TileType::House)) {
					if (!IsHouseCompleted(tile)) continue;
					HouseID north = GetHouseType(tile);
					const TileIndex origin = tile + GetHouseNorthPart(north);
					if (std::ranges::find(seen, origin) != seen.end()) continue;
					seen.push_back(origin);
					consider(origin, GetThreeDHouseEvent(EyeHouseObject(north, origin), origin), true);
					continue;
				}
				if (!close || !IsTileType(tile, TileType::Industry)) continue;
				if (const ThreeDForestPart *forest = ThreeDGameForestPartAt(tile); forest != nullptr) {
					consider(tile, forest->vehicles, false);
					continue;
				}
				const ThreeDIndustryTileParts *parts = GetThreeDOriginalIndustryTileAt(tile);
				if (parts == nullptr || !ThreeDOriginalIndustryTileStands(tile, *parts)) continue;
				/* The block's vehicles stand with its anchor, once whichever of its tiles is near. */
				const TileIndex anchor = GetThreeDOriginalIndustryAnchor(tile, parts);
				if (anchor == INVALID_TILE || std::ranges::find(seen, anchor) != seen.end()) continue;
				seen.push_back(anchor);
				consider(anchor, GetThreeDIndustryTileVehicles(*parts, anchor), false);
			}
		}
		return best;
	}

	void BoardOrLeave()
	{
		if (this->Riding() != nullptr) {
			this->GetOut();
			return;
		}
		/* On board a boat: by its wheel E takes it again; elsewhere on its deck, over the side onto the land near. */
		if (this->aboard != VehicleID::Invalid()) {
			ThreeDEyeVehicle ev;
			if (const Vehicle *boat = this->Aboard(ev); boat != nullptr) {
				if (this->AtHelm(ev)) {
					this->GetIn(boat);
				} else if (this->GoAshore()) {
					this->MakePicture();
					this->SetDirty();
				}
				return;
			}
			this->aboard = VehicleID::Invalid();
		}
		/* At a door: opened away from the walker, or shut. A wing that swings through the walker standing by its hinge
		 * moves it aside (the player, 29. 10.: "the window of the hut has to push me away when I open it, it stopped
		 * open through me and I could not leave"); with nowhere to go, the wing stays as it was. */
		if (const auto door = this->DoorToUse(); door.has_value()) {
			const double lx = this->walker.x - TileX(door->origin) * TILE_SIZE, ly = this->walker.y - TileY(door->origin) * TILE_SIZE;
			ToggleThreeDDoor(door->house, door->origin, door->door, lx, ly);
			if (!this->StepAside()) ToggleThreeDDoor(door->house, door->origin, door->door, lx, ly);
			return;
		}
		/* The walker's plant ripe, or dried to hay: harvested (WalkerPlant). */
		if (const WalkerPlant *p = WalkerPlantNear(this->walker.x, this->walker.y, WALKER_PLANT_REACH_M); p != nullptr) {
			const WalkerPlantStage stage = GetWalkerPlantStage(*p);
			if (stage == WalkerPlantStage::Ripe || stage == WalkerPlantStage::Hay) {
				Command<Commands::HarvestWalkerPlant>::Post(STR_ERROR_WALKER_CAN_T_HARVEST, p->x, p->y);
				return;
			}
		}
		if (PickThreeDMushroom(this->walker.x, this->walker.y, FIRST_PERSON_PICK_M)) {
			Command<Commands::PickWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_BUY, WalkerItem::Mushrooms);
			return;
		}
		/* By a farm's dragon resting: what it is fed chosen (ShowDragonFeedWindow()). */
		if (const auto dragon = ThreeDRestingDragonNear(this->walker.x, this->walker.y, FirstPersonFeetZ(this->walker), FIRST_PERSON_FEED_M); dragon.has_value()) {
			ShowDragonFeedWindow(dragon->place);
			return;
		}
		/* At the entrance of its tent: folded back into the inventory (WalkerTent). */
		if (const WalkerTent *tent = FirstPersonTentToStrike(this->walker.x, this->walker.y); tent != nullptr) {
			CloseWindowById(WindowClass::ForestGrill, 0);
			Command<Commands::StrikeWalkerTent>::Post(STR_ERROR_WALKER_CAN_T_STRIKE, TileIndex{tent->tile});
			return;
		}
		/* By a forest's fire: what is grilled chosen (ShowForestGrillWindow()). */
		if (const TileIndex fire = ThreeDForestFireNear(this->walker.x, this->walker.y, FIRST_PERSON_GRILL_M * OBJECT_UNITS_PER_METRE); fire != INVALID_TILE) {
			ShowForestGrillWindow(fire);
			return;
		}
		this->riding = VehicleID::Invalid();
		const Vehicle *v = this->VehicleToBoard();
		if (v != nullptr) {
			this->GetIn(v);
			return;
		}
		/* By the Zetor or the wheelbarrow of a farm: one like it the company's, there, and into it (CcBuildYardRoadVehicle()). */
		if (const auto yard = this->YardVehicleToTake(); yard.has_value()) {
			Command<Commands::BuildYardRoadVehicle>::Post(STR_ERROR_CAN_T_BUY_ROAD_VEHICLE, CcBuildYardRoadVehicle, yard->tile, yard->engine, yard->px, yard->py, yard->yaw);
		}
	}

	/*
	 * Moves while the keys are held and makes the picture anew, twenty-five times a second -- only here: a paint of
	 * the window (the map under it marks the screen dirty as its vehicles move, in several pieces a frame) puts the
	 * picture made on the screen and nothing more.
	 */
	IntervalTimer<TimerWindow> step{std::chrono::milliseconds(FIRST_PERSON_TICK_MS), [this](uint count) { this->Step(count); }};

	/**
	 * The steps of the walker since the last picture, and the picture. A frame of the game longer than a step (the
	 * picture itself taking 30 to 200 ms, the map, the vehicles) is so many steps: each is taken, against the walls
	 * on its own, and one picture after them -- not one step a frame, which walked the walker (the player, 16. 10.:
	 * "walking is slow") as many times slower as a frame was longer than a step. At most a second of them at once (a
	 * save, the window dragged).
	 * @param count the steps since the last picture
	 */
	void Step(uint count)
	{
		const uint steps = std::min(count, 1000u / FIRST_PERSON_TICK_MS);
		/* In a vehicle: where it is (gone or crashed, the walker is out where it was); the arrows drive it. */
		const Vehicle *rv = this->Riding();
		if (rv != nullptr && rv->vehstatus.Test(VehState::Crashed)) {
			this->GetOut();
			rv = nullptr;
		}
		if (rv == nullptr) this->riding = VehicleID::Invalid();
		for (uint i = 0; i < steps; i++) {
			if (rv == nullptr && this->aboard != VehicleID::Invalid()) {
				/* On board: where the boat has gone and turned meanwhile; gone or sunk, the walker out where it was. */
				ThreeDEyeVehicle ev;
				if (this->Aboard(ev) == nullptr) {
					/* Into the water where it was, or out onto the land near. */
					this->aboard = VehicleID::Invalid();
					if (EyeWaterSurfaceZ(this->walker.x, this->walker.y).has_value()) {
						this->IntoWater(this->walker.x, this->walker.y);
					} else {
						this->PutNear(this->walker.x, this->walker.y);
					}
				} else {
					this->PlaceOnBoard(ev);
					this->Walk(FIRST_PERSON_TICK_MS / 1000.0);
					/* Still on board (not over its side): facing as it turns. */
					if (this->aboard != VehicleID::Invalid()) this->deck_heading = std::remainder(this->walker.heading - ev.facing * M_PI / 180.0, 2 * M_PI);
				}
			} else if (rv == nullptr) {
				this->Walk(FIRST_PERSON_TICK_MS / 1000.0);
			}
			this->Hold(FIRST_PERSON_TICK_MS / 1000.0);
			this->Fly(FIRST_PERSON_TICK_MS / 1000.0);
		}
		this->SeeBlasts();
		if (rv != nullptr) {
			this->walker.x = rv->x_pos;
			this->walker.y = rv->y_pos;
			const bool route = this->arrows.route;
			/* A train of MSTS by its desk's lever (simtrains_cab.h), the others as the cab view drives them. */
			const DirectionKeys keys = ArrowsWindow() == this ? _dirkeys : DirectionKeys{};
			const int ms = static_cast<int>(steps) * FIRST_PERSON_TICK_MS;
			if (!this->seat.Drive(rv, keys, ms)) this->arrows.Drive(rv, keys, ms);
			if (this->arrows.route != route) this->SetWidgetDirty(WID_FP_ROUTE);
			DiveTowards(rv, ms / 1000.0);
		}
		/* The button of the route in a vehicle, of running on foot. */
		bool changed = this->GetWidget<NWidgetStacked>(WID_FP_RUN_SEL)->SetDisplayedPlane(rv != nullptr ? 1 : 0);
		changed |= this->GetWidget<NWidgetStacked>(WID_FP_CONTROLS_SEL)->SetDisplayedPlane(IsSimtrainsTrain(rv) ? 0 : SZSP_NONE);
		if (changed) this->ReInit();
		/* No health left: the walker is gone, back to the map (the player, 15. 10.). */
		const Company *c = Company::GetIfValid(_local_company);
		if (c != nullptr && c->walker_health == 0) {
			this->Close();
			return;
		}
		/* The blows of the dragons and the animals since the last step (three_d_dragons.h, three_d_animals.h): the health
		 * through the command, the picture red a moment; in a vehicle its breakdown first (CmdHurtWalker()). */
		const uint damage = TakeThreeDDragonDamage() + TakeThreeDAnimalDamage();
		if (damage > 0 && c != nullptr) {
			const bool broken = rv == nullptr || rv->First()->breakdown_ctr != 0;
			Command<Commands::HurtWalker>::Post(TileIndex{}, static_cast<uint8_t>(std::min(damage, 255u)), rv != nullptr ? rv->First()->index : VehicleID::Invalid());
			if (broken) this->hurt_steps = 6;
		}
		/* The walker's nurse with it, insured (three_d_animals.h): a little health back every few seconds of the game,
		 * counted from when there is any to give back (the player, 31. 10.: "+10 every 3 s, yes"). */
		if (c != nullptr && c->walker_health < WALKER_HEALTH_MAX && WalkerInsured(*c) && ThreeDNurseHealsWalker()) {
			if (TimerGameTick::counter - this->nurse_healed >= WALKER_NURSE_HEAL_TICKS) {
				this->nurse_healed = TimerGameTick::counter;
				Command<Commands::HealWalkerByNurse>::Post(WALKER_NURSE_HEAL);
			}
		} else {
			this->nurse_healed = TimerGameTick::counter;
		}
		/* The look ahead: on its thread the picture made now is seen a frame late (EyeView()), so a steady turn of
		 * the head -- the right button dragged, an arrow held -- was always shown where the hand had already been,
		 * and the view felt tied to the hand by a rubber band (the player, 25. 10.). The picture is therefore made
		 * looking where the head will be when it is seen: ahead by most of the turn of the last frame, which for a
		 * steady turn is the lag itself. A sudden stop overshoots by one frame and settles; a leap of the view (the
		 * double click putting the look back) stays a leap, the clamp keeps the guess small. The rig draws at once
		 * and guesses nothing, so its pictures stay of the moment. */
		if (!_eye_synchronous && rv == nullptr) {
			/* A turn bigger than the guess may be is a leap, not a turn -- the double click putting the look back, the
			 * walker put somewhere (the god's eye): shown as it is, nothing guessed off it. */
			constexpr double keep = 0.8, most = 0.17;
			const double dh = std::remainder(this->walker.heading - this->seen_heading, 2 * M_PI), dp = this->walker.pitch - this->seen_pitch;
			this->look_ahead_heading = std::abs(dh) > most ? 0 : std::clamp(keep * dh, -most, most);
			this->look_ahead_pitch = std::abs(dp) > most ? 0 : std::clamp(keep * dp, -most, most);
		} else {
			this->look_ahead_heading = 0;
			this->look_ahead_pitch = 0;
		}
		this->seen_heading = this->walker.heading;
		this->seen_pitch = this->walker.pitch;
		this->FitEyes();
		this->MakePicture();
		this->hurt_steps = std::max(0, this->hurt_steps - static_cast<int>(steps));
		this->SetWidgetDirty(WID_FP_CANVAS);
	}

	FirstPersonWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->GetWidget<NWidgetStacked>(WID_FP_CONTROLS_SEL)->SetDisplayedPlane(SZSP_NONE);
		this->FinishInitNested(number);
		this->walker.eye_z = FIRST_PERSON_EYES_M * OBJECT_HEIGHT_PER_METRE;
		this->walker.heading = std::atan2(1.0, 1.0); // looking south-east, down the screen of the map facing north
		this->seen_heading = this->walker.heading;
		this->PutWhereTheMainViewLooks();
		/* Opened over the whole screen (the player, 15. 10.); the button makes it a window again. */
		this->ToggleFull();
	}

	void Close(int data = 0) override
	{
		/* Out of the vehicle the walker was in: let go of it, as the cab view closing does. */
		CabLetGo(this->Riding());
		DropEyeFrame(&this->camera);
		VideoDriver::GetInstance()->HideThreeDPicture(this);
		this->Window::Close(data);
	}

	/** The picture from the walker's eyes, as big as the canvas is now. */
	void MakePicture() const
	{
		const NWidgetBase *canvas = this->GetWidget<NWidgetBase>(WID_FP_CANVAS);
		const int w = canvas->current_x, h = canvas->current_y;
		if (w <= 0 || h <= 0) return;
		const Vehicle *rv = this->Riding();
		FirstPersonEye eye = this->walker;
		eye.heading = std::remainder(eye.heading + this->look_ahead_heading, 2 * M_PI);
		eye.pitch = std::clamp(eye.pitch + this->look_ahead_pitch, -FIRST_PERSON_PITCH_MAX, FIRST_PERSON_PITCH_MAX);
		/* In a train of MSTS with its cab: the eye where the cab has it, its desk over the view (as the cab view's). */
		if (rv != nullptr) {
			this->seat.Prepare(rv, this->camera);
		} else {
			this->camera.msts = false;
		}
		this->picture_ok = rv != nullptr ? DrawCabView(rv, w, h, this->camera, this->picture) : DrawFirstPersonView(eye, w, h, this->camera, this->picture);
	}

	/** Over the whole screen between the toolbar and the status bar, or back where the window was. */
	void ToggleFull()
	{
		if (!this->full) {
			this->before_full = {this->left, this->top, this->left + this->width, this->top + this->height};
			this->full = true;
			const int top = GetMainViewTop(), bottom = GetMainViewBottom();
			this->left = 0;
			this->top = top;
			ResizeWindow(this, _screen.width - this->width, bottom - top - this->height, false);
		} else {
			this->full = false;
			this->left = this->before_full.left;
			this->top = this->before_full.top;
			ResizeWindow(this, this->before_full.Width() - this->width, this->before_full.Height() - this->height, false);
		}
		this->MakePicture();
		MarkWholeScreenDirty();
	}

	/** The walker to where the middle of the main view is, on the ground; it steps aside to the nearest place it can stand. */
	void PutWhereTheMainViewLooks()
	{
		const Window *main = GetMainWindow();
		if (main == nullptr || main->viewport == nullptr) return;
		const Viewport &vp = *main->viewport;
		const double vx = vp.virtual_left + vp.virtual_width / 2.0, vy = vp.virtual_top + vp.virtual_height / 2.0;
		double x = 0, y = 0, z = 0;
		for (int k = 0; k < 3; k++) {
			InverseTurnedFlatF(vx, vy, z, x, y);
			z = FirstPersonGroundZ(x, y);
		}
		this->PutNear(x, y);
	}

	/**
	 * The walker where it stands, or, when something came into its way there (a wing of a door opened through it), the
	 * nearest place it can go from there, in rings 5 cm apart up to 2 m.
	 * @return whether it stands free now
	 */
	bool StepAside()
	{
		if (FirstPersonFreeFrom(this->walker, this->walker.x, this->walker.y)) return true;
		for (int ring = 1; ring <= 40; ring++) {
			const double r = ring * 0.05 * OBJECT_UNITS_PER_METRE;
			for (int k = 0; k < ring * 8; k++) {
				const double a = k * 2 * M_PI / (ring * 8);
				const double px = this->walker.x + r * std::cos(a), py = this->walker.y + r * std::sin(a);
				if (!FirstPersonFreeFrom(this->walker, px, py)) continue;
				this->walker.x = px;
				this->walker.y = py;
				FirstPersonSettle(this->walker);
				return true;
			}
		}
		return false;
	}

	/**
	 * The walker to the nearest place it can stand from a world point: beside an industry of the original game it
	 * cannot go into (the player, 6. 11.), round a house, out of the water.
	 * @param away_from_dragons and where no dragon would go for it at once (the god's eye: ShowFirstPersonWindowAt())
	 */
	void PutNear(double x, double y, bool away_from_dragons = false)
	{
		this->walker.swimming = false;
		/* The nearest place to stand, in rings a quarter of a tile apart, up to twelve tiles off (the largest
		 * industries). */
		for (int ring = 0; ring < 48; ring++) {
			for (int k = 0; k < std::max(1, ring * 8); k++) {
				const double a = k * 2 * M_PI / std::max(1, ring * 8), r = ring * TILE_SIZE / 4.0;
				const double px = x + r * std::cos(a), py = y + r * std::sin(a);
				if (FirstPersonWalkable(px, py) && (!away_from_dragons || !ThreeDDragonWouldFight(px, py))) {
					this->walker.x = px;
					this->walker.y = py;
					return;
				}
			}
		}
		this->walker.x = x;
		this->walker.y = y;
	}

	/**
	 * Up or Down pressed twice in a row: the walker walks on that way without the key held, till Up or Down is pressed
	 * again (the player, 21. 10.: "when I press the arrow twice, I walk and do not have to hold it"), or Left or Right
	 * (the player, 22. 10.: "left, right turn the walking without the arrow off").
	 * @param up Up (or W) held now
	 * @param down Down (or S) held now
	 * @param side Left or Right (or A or D) held now
	 * @param dt seconds since the last step
	 */
	void WalkOn(bool up, bool down, bool side, double dt)
	{
		if (side && !this->walk_held[2]) this->walk_on = 0;
		this->walk_held[2] = side;
		const bool held[2] = {up, down};
		for (int k = 0; k < 2; k++) {
			this->walk_tapped[k] = std::min(this->walk_tapped[k] + dt, FIRST_PERSON_DOUBLE_TAP_S);
			const bool pressed = held[k] && !this->walk_held[k];
			this->walk_held[k] = held[k];
			if (!pressed) continue;
			if (this->walk_on != 0) {
				/* Walking on: a press stops it, and is not the first of two again. */
				this->walk_on = 0;
				this->walk_tapped[k] = FIRST_PERSON_DOUBLE_TAP_S;
			} else if (this->walk_tapped[k] < FIRST_PERSON_DOUBLE_TAP_S) {
				this->walk_on = k == 0 ? 1 : -1;
				this->walk_tapped[k] = FIRST_PERSON_DOUBLE_TAP_S;
			} else {
				this->walk_tapped[k] = 0;
			}
		}
	}

	/**
	 * A step of the walker: the arrows held while they are its window's (ArrowsWindow(): the player, 17. 10.: "the
	 * arrows always to that window, not to the map"), W, A, S and D while its window has the focus; on by itself after
	 * Up or Down pressed twice (WalkOn()).
	 */
	void Walk(double dt)
	{
		const DirectionKeys arrows = ArrowsWindow() == this ? _dirkeys : DirectionKeys{};
		const DirectionKeys wasd = _focused_window == this && !EditBoxInGlobalFocus() ? _walkkeys : DirectionKeys{};
		/* The keys not the walker's any more (the map or another window has them): it stops. */
		if (ArrowsWindow() != this && _focused_window != this) this->walk_on = 0;
		this->WalkOn(arrows.Test(DirectionKey::Up) || wasd.Test(DirectionKey::Up), arrows.Test(DirectionKey::Down) || wasd.Test(DirectionKey::Down),
				arrows.Any({DirectionKey::Left, DirectionKey::Right}) || wasd.Any({DirectionKey::Left, DirectionKey::Right}), dt);
		if (arrows.Any({DirectionKey::Left, DirectionKey::Right})) {
			this->turn_held = std::min(this->turn_held + dt, FIRST_PERSON_TURN_RAMP_S);
		} else {
			this->turn_held = 0;
		}
		if (arrows.None() && wasd.None() && this->walk_on == 0) return;
		/* The turn holds gentle for a beat, then runs up to full as the arrow stays held (FIRST_PERSON_TURN_FIRST). */
		const double run_up = std::clamp((this->turn_held - FIRST_PERSON_TURN_SLOW_S) / (FIRST_PERSON_TURN_RAMP_S - FIRST_PERSON_TURN_SLOW_S), 0.0, 1.0);
		const double turn = FIRST_PERSON_TURN * (FIRST_PERSON_TURN_FIRST + (1.0 - FIRST_PERSON_TURN_FIRST) * run_up);
		if (arrows.Test(DirectionKey::Left)) this->walker.heading += turn * dt;
		if (arrows.Test(DirectionKey::Right)) this->walker.heading -= turn * dt;
		this->walker.heading = std::remainder(this->walker.heading, 2 * M_PI);
		double ahead = 0, aside = 0;
		if (arrows.Test(DirectionKey::Up) || wasd.Test(DirectionKey::Up)) ahead += 1;
		if (arrows.Test(DirectionKey::Down) || wasd.Test(DirectionKey::Down)) ahead -= 1;
		if (wasd.Test(DirectionKey::Right)) aside += 1;
		if (wasd.Test(DirectionKey::Left)) aside -= 1;
		if (ahead == 0 && !this->walk_held[0] && !this->walk_held[1]) ahead = this->walk_on;
		if (ahead == 0 && aside == 0) return;
		/* Going: the body turns to where the eyes look, and goes there (the right button steers the walk). */
		this->head = 0;
		/* In the water: after the eyes, up and down too (Swim()). */
		if (this->walker.swimming) {
			this->Swim(ahead, aside, dt);
			return;
		}
		const double norm = std::sqrt(ahead * ahead + aside * aside);
		const double speed = ((this->running || _shift_pressed) ? FIRST_PERSON_RUN_MS : FIRST_PERSON_WALK_MS) * OBJECT_UNITS_PER_METRE * dt / norm;
		const double fx = std::cos(this->walker.heading), fy = std::sin(this->walker.heading);
		const double rx = fy, ry = -fx;
		const double dx = (fx * ahead + rx * aside) * speed, dy = (fy * ahead + ry * aside) * speed;
		/* On board a boat: on its deck (MoveOnBoard()), in pieces as on the ground. */
		if (this->aboard != VehicleID::Invalid()) {
			const int deck_pieces = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy) / FIRST_PERSON_SUBSTEP)));
			for (int i = 0; i < deck_pieces; i++) this->MoveOnBoard(dx / deck_pieces, dy / deck_pieces);
			return;
		}
		/* Along a wall, sliding, and round a post (FirstPersonMove()); in pieces, so that a run does not step over a
		 * thin wall. */
		const int pieces = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy) / FIRST_PERSON_SUBSTEP)));
		FirstPersonSettle(this->walker);
		for (int i = 0; i < pieces; i++) {
			if (this->StepIntoWater(dx / pieces, dy / pieces)) break;
			if (!FirstPersonMove(this->walker, dx / pieces, dy / pieces)) break;
			/* Up a stair, or down off it. */
			FirstPersonSettle(this->walker);
		}
	}

	/**
	 * A step off the shore, a quay or a bank (not off a bridge's deck: its railing) into the water before the walker,
	 * the edge of its circle over it: swimming there (IntoWater()). Not along the shore, it does not slide along it.
	 * @return whether it went into the water
	 */
	bool StepIntoWater(double dx, double dy)
	{
		const double len = std::hypot(dx, dy);
		if (len <= 0) return false;
		/* The edge of its circle where this step would take it: the walker stops a step short of the water (its circle
		 * may not reach over it), so the edge where it stands may never come over it (the player, test3: "I cannot step
		 * onto the river's tile", stopped a tenth of a unit off the shore of a lake for ever). */
		const double reach = len + FIRST_PERSON_RADIUS * 1.2;
		const double wx = this->walker.x + dx / len * reach, wy = this->walker.y + dy / len * reach;
		if (!EyeWaterSurfaceZ(wx, wy).has_value()) return false;
		const TileIndex under = TileVirtXY(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y));
		if (IsBridgeAbove(under) && FirstPersonFeetZ(this->walker) >= GetBridgePixelHeight(GetSouthernBridgeEnd(under)) - 0.1 * OBJECT_HEIGHT_PER_METRE) return false;
		this->IntoWater(wx, wy);
		return true;
	}

	/** How many of a thing the walker carries. */
	static uint Carried(WalkerItem item)
	{
		const Company *c = Company::GetIfValid(_local_company);
		return c != nullptr && item < WalkerItem::End ? c->walker_items[to_underlying(item)] : 0;
	}

	/**
	 * The weapon in the hand (the player, 15. 10.: "I went through the whole of Morrowind with a big hammer"; 17. 10.:
	 * the bow and the crossbow): taken out and put away with F or chosen with its key, another put away first; a
	 * hammer or a sword swung with the left button or Space, the bow drawn while the button is held and shot as it is
	 * let go, the crossbow shot and drawn again -- as long as the walker carries it.
	 */
	void Hold(double dt)
	{
		if (Carried(_first_person_weapons[this->wanted].item) == 0) this->wanted = this->weapon;
		if (Carried(_first_person_weapons[this->weapon].item) == 0 && this->wanted == this->weapon) this->weapon_out = false;
		const bool changing = this->wanted != this->weapon;
		ThreeDHeldThing &held = this->walker.held;
		const float target = this->weapon_out && !changing ? 1.0f : 0.0f, step = static_cast<float>(dt / FIRST_PERSON_DRAW_S);
		held.raise = target > held.raise ? std::min(target, held.raise + step) : std::max(target, held.raise - step);
		if (changing && held.raise <= 0) {
			this->weapon = this->wanted;
			this->swing_time = -1;
			this->draw_time = -1;
			held.swing = 0;
		}
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		if (this->swing_time >= 0) {
			/* The blow lands as the weapon comes down before the eyes: on a dragon there, if one is. */
			const bool before = this->swing_time < FIRST_PERSON_BLOW_S;
			this->swing_time += dt;
			if (before && this->swing_time >= FIRST_PERSON_BLOW_S && w.item == WalkerItem::MineHammer) {
				/* The mine hammer: its bomb blows up where its nose strikes, as a rocket (the player, 26. 10.: "let the mine
				 * hammer blow up"), whatever is there. */
				const double ahead = FIRST_PERSON_MINE_AHEAD_M * OBJECT_UNITS_PER_METRE;
				this->Explode(this->walker.x + std::cos(this->walker.heading) * ahead, this->walker.y + std::sin(this->walker.heading) * ahead,
						FirstPersonFeetZ(this->walker) + FIRST_PERSON_MINE_UP_M * OBJECT_HEIGHT_PER_METRE, w.damage);
			} else if (before && this->swing_time >= FIRST_PERSON_BLOW_S && !HitThreeDDragon(this->walker.x, this->walker.y, this->walker.heading, w.reach_m, w.damage)) {
				HitThreeDAnimal(this->walker.x, this->walker.y, this->walker.heading, w.reach_m, w.damage);
			}
			held.swing = SwingAngle(this->swing_time);
			if (this->swing_time >= FIRST_PERSON_SWING_S) {
				this->swing_time = -1;
				held.swing = 0;
			}
		}
		if (this->draw_time >= 0) {
			/* The bow drawn: put away or out of arrows, let down; drawn with the button, shot as it is let go. */
			if (held.raise < 1.0f || Carried(w.ammo) == 0) {
				this->draw_time = -1;
				this->mouse_draw = false;
			} else {
				this->draw_time += dt;
				if (this->mouse_draw && !_left_button_down) this->Release();
			}
		}
		if (this->firing) {
			/* The machine gun: a shot every tenth of a second while the button is held (or the burst of Space lasts). */
			if (held.raise < 1.0f || Carried(w.ammo) == 0 || (this->mouse_fire && !_left_button_down) || this->burst_left <= 0 || w.use != WeaponUse::Burst) {
				this->firing = false;
			} else {
				this->burst_time -= dt;
				while (this->burst_time <= 0 && this->burst_left > 0) {
					this->Shoot(w, 1.0);
					this->burst_left--;
					this->burst_time += FIRST_PERSON_BURST_S;
				}
			}
		}
		this->reload_time = std::max(0.0, this->reload_time - dt);
		this->kick = std::max(0.0f, this->kick - static_cast<float>(dt / FIRST_PERSON_KICK_S));
		this->Fish(dt);
		this->Dress();
		this->HoldLight(dt);
	}

	/**
	 * The light in the left hand (L, or a click on it in the inventory): raised as the weapon is, another put out first,
	 * its flame going through its frames ("plamen") -- as long as the walker carries it, and not in a vehicle. The
	 * torch says so as it is put out ("zhasnuti").
	 */
	void HoldLight(double dt)
	{
		if (this->light >= 0 && (Carried(_first_person_lights[this->light].item) == 0 || this->Riding() != nullptr)) this->light = -1;
		ThreeDHeldThing &left = this->walker.left;
		const bool changing = this->light != this->light_held;
		const float target = this->light_held >= 0 && !changing ? 1.0f : 0.0f, step = static_cast<float>(dt / FIRST_PERSON_DRAW_S);
		left.raise = target > left.raise ? std::min(target, left.raise + step) : std::max(target, left.raise - step);
		if (changing && left.raise <= 0) {
			if (this->light_held >= 0) {
				if (auto sound = GetThreeDThingSound(_first_person_lights[this->light_held].thing, "zhasnuti"); sound.has_value()) SndPlayFx(*sound);
			}
			this->light_held = this->light;
			this->light_time = 0;
		}
		this->light_time += dt;
		if (this->light_held < 0 || left.raise <= 0) {
			left.thing.clear();
			this->walker.light_colour = 0;
			return;
		}
		const FirstPersonLight &l = _first_person_lights[this->light_held];
		left.thing = std::string(l.thing);
		left.hold = ThreeDHold::Left;
		/* Held so that its flame ("svetlo") is where the light is seen, low at the left: the torch's handle below it,
		 * the lantern about it. */
		auto grip = GetThreeDThingPlace(l.thing, "svetlo");
		if (!grip.has_value()) grip = GetThreeDThingPlace(l.thing, "uchyt");
		const std::array<float, 3> at = grip.value_or(std::array<float, 3>{0, 0, 0});
		std::copy(at.begin(), at.end(), left.grip);
		const ThreeDVehicleObject *fig = GetThreeDFigure(l.thing);
		left.frame = fig != nullptr && fig->frames >= 2 ? static_cast<int>(this->light_time * 1000.0 / (MILLISECONDS_PER_TICK * fig->frame_ticks)) % fig->frames : 0;
		/* Its light as its index has it (the torch's fire 10 m about, flickering with the flame), else the game's own. */
		this->walker.light_colour = fig != nullptr && fig->light_colour != 0 ? fig->light_colour : l.colour;
		double reach = fig != nullptr && fig->light_reach_m > 0 ? fig->light_reach_m : l.reach_m;
		if (fig != nullptr && fig->light_flickers) reach *= 0.9 + 0.1 * std::abs(std::sin(this->light_time * 7.3) * std::sin(this->light_time * 2.9 + left.frame));
		this->walker.light_reach_m = reach * left.raise;
	}

	/** L: the next light the walker carries into the left hand, after the last one out (the torch, the lantern, none). */
	void NextLight()
	{
		int next = this->light;
		do {
			next++;
			if (next >= static_cast<int>(std::size(_first_person_lights))) {
				next = -1;
				break;
			}
		} while (Carried(_first_person_lights[next].item) == 0);
		this->light = this->Riding() != nullptr ? -1 : next;
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}

	/**
	 * What the walker holds as it is drawn (DrawThreeDHeldThing()): the weapon in its state -- the bow drawn or not,
	 * the crossbow drawn or shot -- held by its grip ("uchyt"), the arrow on the string or the bolt in the groove at
	 * its nock ("zarez_<stav>") while there is one to shoot.
	 */
	void Dress()
	{
		ThreeDHeldThing &held = this->walker.held;
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		held.line = false;
		held.hanging_glows = false;
		if (held.raise <= 0) {
			held.thing.clear();
			held.loaded.clear();
			return;
		}
		std::string_view state;
		switch (w.use) {
			case WeaponUse::Rod: held.hold = ThreeDHold::Rod; break;
			case WeaponUse::Swing: held.hold = ThreeDHold::Swung; break;
			case WeaponUse::Bow: held.hold = ThreeDHold::Bow; state = this->draw_time >= FIRST_PERSON_BOW_DRAW_S * 0.4 ? "napnuto" : "volno"; break;
			case WeaponUse::Crossbow: held.hold = ThreeDHold::Crossbow; state = this->reload_time > 0 ? "vystreleno" : "natazeno"; break;
			case WeaponUse::Burst: held.hold = ThreeDHold::Crossbow; break;
			/* Shot, its loaded rocket gone from it till it is loaded again. */
			case WeaponUse::Rocket: held.hold = ThreeDHold::Shoulder; state = this->reload_time > 0 || Carried(w.ammo) == 0 ? "prazdny" : ""; break;
		}
		held.thing = state.empty() ? std::string(w.thing) : fmt::format("{}/{}", w.thing, state);
		const auto grip = GetThreeDThingPlace(w.thing, "uchyt");
		const std::array<float, 3> at = grip.value_or(w.use == WeaponUse::Swing ? std::array<float, 3>{0, 0, 0.22f} : std::array<float, 3>{0, 0, 0});
		std::copy(at.begin(), at.end(), held.grip);
		held.loaded.clear();
		if (!w.missile.empty() && this->reload_time <= 0 && Carried(w.ammo) > 0) {
			auto nock = GetThreeDThingPlace(w.thing, fmt::format("zarez_{}", state));
			if (!nock.has_value()) nock = GetThreeDThingPlace(w.thing, "zarez");
			if (nock.has_value()) {
				held.loaded = std::string(w.missile);
				std::copy(nock->begin(), nock->end(), held.nock);
			}
		}
		held.kick = this->kick;
		held.drawn = this->draw_time >= 0 ? static_cast<float>(std::min(1.0, this->draw_time / FIRST_PERSON_BOW_DRAW_S)) : 0.0f;
		held.twist = w.twist;
		if (w.use == WeaponUse::Rod) this->DressRod();
	}

	/**
	 * The fishing rod in the hand as the walker fishes (FishingStage): bent as a fish bites (the colleague's
	 * prut_ohnuty); its line from its tip ("spicka") down to the hook hanging before the eyes, out to it on the water, or
	 * down to the fish caught, by its mouth ("usta"), in its colour.
	 */
	void DressRod()
	{
		ThreeDHeldThing &held = this->walker.held;
		/* The rod in the hand bent (its "<jmeno>_ohnuty") as a fish bites and while it hangs on it, else straight -- the same
		 * rod (the player, test3: "two rods, each straight and bent, so that the hand does not hold another rod"). */
		std::string rod{_first_person_weapons[this->weapon].thing};
		if (this->fishing == FishingStage::Bite || this->fishing == FishingStage::Caught) {
			std::string bent = rod + std::string(FIRST_PERSON_ROD_BENT);
			if (GetThreeDFigure(bent) != nullptr) rod = std::move(bent);
		}
		held.thing = rod;
		const std::array<float, 3> grip = GetThreeDThingPlace(rod, "uchyt").value_or(std::array<float, 3>{0, 0, 0.25f});
		std::copy(grip.begin(), grip.end(), held.grip);
		const auto tip = GetThreeDThingPlace(rod, "spicka");
		if (!tip.has_value()) return;
		std::copy(tip->begin(), tip->end(), held.tip);
		held.line = true;
		held.hanging.clear();
		held.hanging_fish = false;
		held.hanging_state = 0;
		held.hanging_scale = 1.0f;
		std::fill(std::begin(held.hanging_at), std::end(held.hanging_at), 0.0f);
		std::fill(std::begin(held.line_end), std::end(held.line_end), std::numeric_limits<double>::quiet_NaN());
		if (this->fishing == FishingStage::Caught && this->rod_fish >= 0) {
			const FirstPersonRodFish &f = _first_person_rod_fish[this->rod_fish];
			held.hanging = std::string(f.figure);
			held.hanging_fish = true;
			held.hang_m = FIRST_PERSON_FISH_HANG_M;
			const std::array<float, 3> mouth = GetThreeDThingPlace(f.figure, "usta").value_or(std::array<float, 3>{0, 0, 0});
			std::copy(mouth.begin(), mouth.end(), held.hanging_at);
			const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(f.figure);
			held.hanging_scale = a != nullptr ? a->scale : 1.0f;
			held.hanging_state = this->rod_fish_colour;
			/* The fish of the sea glow at night ("v_noci_sviti"). */
			held.hanging_glows = a != nullptr && a->glows;
		} else if (this->fishing == FishingStage::Ready || this->fishing == FishingStage::Caught) {
			held.hanging = std::string(FIRST_PERSON_HOOK);
			held.hang_m = FIRST_PERSON_HOOK_HANG_M;
		} else {
			held.line_end[0] = this->hook_x;
			held.line_end[1] = this->hook_y;
			held.line_end[2] = this->hook_z;
		}
	}

	/** A word about the fishing shown a moment over the view. */
	void SayFishing(std::string text)
	{
		this->fish_say = std::move(text);
		this->fish_say_left = FIRST_PERSON_FISH_SAY_S;
	}

	/** The rod's tip about where it is in the world as the hook is cast: ahead of the walker and over its eyes. */
	void RodTipInWorld(double &x, double &y, double &z) const
	{
		const double h = this->walker.heading;
		x = this->walker.x + std::cos(h) * 3.0 * OBJECT_UNITS_PER_METRE;
		y = this->walker.y + std::sin(h) * 3.0 * OBJECT_UNITS_PER_METRE;
		z = FirstPersonFeetZ(this->walker) + this->walker.eye_z + 1.2 * OBJECT_HEIGHT_PER_METRE;
	}

	/**
	 * A click with the rod in the hand: the hook cast six to ten metres ahead, only into the water and not swimming;
	 * on the water reeled in again; a fish biting caught -- into the inventory (the command), hanging before the eyes a
	 * moment.
	 */
	void FishClick()
	{
		switch (this->fishing) {
			case FishingStage::Ready: {
				if (this->walker.swimming) {
					this->SayFishing(GetString(STR_FIRST_PERSON_FISH_SWIMMING));
					break;
				}
				const double metres = FIRST_PERSON_CAST_MIN_M + (FIRST_PERSON_CAST_MAX_M - FIRST_PERSON_CAST_MIN_M) * InteractiveRandomRange(1001) / 1000.0;
				const double h = this->walker.heading;
				const double tx = this->walker.x + std::cos(h) * metres * OBJECT_UNITS_PER_METRE, ty = this->walker.y + std::sin(h) * metres * OBJECT_UNITS_PER_METRE;
				const std::optional<double> water = EyeWaterSurfaceZ(tx, ty);
				if (!water.has_value()) {
					this->SayFishing(GetString(STR_FIRST_PERSON_FISH_NO_WATER));
					break;
				}
				this->water_z = *water;
				this->RodTipInWorld(this->cast_x, this->cast_y, this->cast_z);
				this->land_x = tx;
				this->land_y = ty;
				this->hook_x = this->cast_x;
				this->hook_y = this->cast_y;
				this->hook_z = this->cast_z;
				this->fishing = FishingStage::Flying;
				this->fish_time = 0;
				break;
			}
			case FishingStage::Waiting:
				/* Reeled in, nothing on it. */
				this->fishing = FishingStage::Ready;
				this->fish_time = 0;
				break;
			case FishingStage::Bite: {
				const WalkerItem item = this->rod_fish >= 0 ? _first_person_rod_fish[this->rod_fish].item : WalkerItem::End;
				if (item == WalkerItem::End) break;
				Command<Commands::PickWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_BUY, item);
				this->SayFishing(GetString(STR_FIRST_PERSON_FISH_CAUGHT, GetWalkerItemSpec(item).name));
				this->fishing = FishingStage::Caught;
				this->fish_time = 0;
				break;
			}
			case FishingStage::Flying:
			case FishingStage::Caught:
				break;
		}
		this->Dress();
	}

	/** A fish of the water the hook is in bites (the big green perch in a river, a canal, a lake; the two of the sea there), its colour at random. */
	void FishBites()
	{
		const bool sea = FishingInSea(this->hook_x, this->hook_y);
		std::vector<int> fish;
		for (int k = 0; k < static_cast<int>(std::size(_first_person_rod_fish)); k++) {
			const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(_first_person_rod_fish[k].figure);
			if (_first_person_rod_fish[k].sea == sea && a != nullptr && a->on_rod) fish.push_back(k);
		}
		this->rod_fish = fish.empty() ? -1 : fish[InteractiveRandomRange(static_cast<uint32_t>(fish.size()))];
		this->rod_fish_colour = 0;
		if (this->rod_fish >= 0) {
			/* Its colour of its palette as it is: the first of the four shades ReadFigurePalette() makes of each. */
			const ThreeDVehicleObject *fig = GetThreeDFigure(_first_person_rod_fish[this->rod_fish].figure);
			const uint32_t colours = fig != nullptr ? fig->palette_states.second / 4 : 0;
			if (colours > 0) this->rod_fish_colour = fig->PaletteState(InteractiveRandomRange(colours));
		}
		this->fishing = this->rod_fish >= 0 ? FishingStage::Bite : FishingStage::Waiting;
		this->fish_time = 0;
		if (this->rod_fish < 0) this->fish_bites_at = 1e9;
	}

	/**
	 * The fishing with the rod in the hand, a step: the rod swung back and forward as the hook flies out along its arc;
	 * the hook bobbing on the water till a fish bites (five to twenty seconds); the hook pulled under while it bites, two
	 * seconds, then it escapes; the fish caught hanging before the eyes a moment. Put away, in a vehicle or swimming, the
	 * hook is reeled in.
	 */
	void Fish(double dt)
	{
		this->fish_say_left = std::max(0.0, this->fish_say_left - dt);
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		ThreeDHeldThing &held = this->walker.held;
		const bool fishing = w.use == WeaponUse::Rod && this->weapon_out && this->wanted == this->weapon && held.raise >= 1.0f && this->Riding() == nullptr && !this->walker.swimming;
		if (!fishing) {
			this->fishing = FishingStage::Ready;
			this->fish_time = 0;
			this->rod_fish = -1;
			if (w.use == WeaponUse::Rod) held.swing = 0;
			return;
		}
		this->fish_time += dt;
		held.swing = 0;
		const double float_z = this->water_z + FIRST_PERSON_HOOK_FLOAT_M * OBJECT_HEIGHT_PER_METRE;
		switch (this->fishing) {
			case FishingStage::Ready:
				break;
			case FishingStage::Flying: {
				/* The rod back and up, forward and down, back to rest; the hook leaving as it comes forward. */
				const double s = this->fish_time;
				const double back = FIRST_PERSON_CAST_SWING_S * 0.4;
				if (s < back) {
					held.swing = static_cast<float>(-35.0 * s / back);
				} else if (s < FIRST_PERSON_CAST_SWING_S) {
					held.swing = static_cast<float>(-35.0 + 50.0 * (s - back) / (FIRST_PERSON_CAST_SWING_S - back));
				} else {
					held.swing = static_cast<float>(15.0 * std::max(0.0, 1.0 - (s - FIRST_PERSON_CAST_SWING_S) / (FIRST_PERSON_CAST_S - FIRST_PERSON_CAST_SWING_S)));
				}
				const double u = std::clamp((s - back) / (FIRST_PERSON_CAST_S - back), 0.0, 1.0);
				this->hook_x = this->cast_x + (this->land_x - this->cast_x) * u;
				this->hook_y = this->cast_y + (this->land_y - this->cast_y) * u;
				this->hook_z =this->cast_z + (float_z - this->cast_z) * u + 1.5 * OBJECT_HEIGHT_PER_METRE * 4 * u * (1 - u);
				if (s >= FIRST_PERSON_CAST_S) {
					this->fishing = FishingStage::Waiting;
					this->fish_time = 0;
					this->fish_bites_at = FIRST_PERSON_BITE_MIN_S + (FIRST_PERSON_BITE_MAX_S - FIRST_PERSON_BITE_MIN_S) * InteractiveRandomRange(1001) / 1000.0;
					this->hook_z = float_z;
				}
				break;
			}
			case FishingStage::Waiting:
				this->hook_z = float_z + FIRST_PERSON_HOOK_BOB_M * OBJECT_HEIGHT_PER_METRE * std::sin(this->fish_time * 2 * M_PI * 0.7);
				if (this->fish_time >= this->fish_bites_at) this->FishBites();
				break;
			case FishingStage::Bite:
				/* Pulled under in jerks. */
				this->hook_z = float_z - (0.18 + 0.07 * std::sin(this->fish_time * 13.0)) * OBJECT_HEIGHT_PER_METRE;
				if (this->fish_time >= FIRST_PERSON_BITE_S) {
					this->SayFishing(GetString(STR_FIRST_PERSON_FISH_ESCAPED));
					this->rod_fish = -1;
					this->fishing = FishingStage::Waiting;
					this->fish_time = 0;
					this->fish_bites_at = FIRST_PERSON_BITE_MIN_S + (FIRST_PERSON_BITE_MAX_S - FIRST_PERSON_BITE_MIN_S) * InteractiveRandomRange(1001) / 1000.0;
				}
				break;
			case FishingStage::Caught:
				if (this->fish_time >= FIRST_PERSON_CAUGHT_S) {
					this->fishing = FishingStage::Ready;
					this->fish_time = 0;
					this->rod_fish = -1;
				}
				break;
		}
	}

	/**
	 * The left button or Space: a swing of the hammer or the sword in the hand; the bow drawn (with the button: shot as
	 * it is let go; Space again shoots); the crossbow shot.
	 * @param mouse with the left button
	 */
	void Attack(bool mouse)
	{
		if (this->Riding() != nullptr || !this->weapon_out || this->wanted != this->weapon || this->walker.held.raise < 1.0f) return;
		const FirstPersonWeapon &w = _first_person_weapons[this->weapon];
		switch (w.use) {
			case WeaponUse::Swing:
				if (this->swing_time < 0) this->swing_time = 0;
				break;
			case WeaponUse::Bow:
				if (this->draw_time >= 0) {
					if (!mouse) this->Release();
					break;
				}
				if (this->reload_time > 0 || Carried(w.ammo) == 0) break;
				this->draw_time = 0;
				this->mouse_draw = mouse;
				if (auto sound = GetThreeDThingSound(w.thing, "napnuti"); sound.has_value()) SndPlayFx(*sound);
				break;
			case WeaponUse::Crossbow:
				if (this->reload_time > 0 || Carried(w.ammo) == 0) break;
				this->Shoot(w, 1.0);
				this->reload_time = FIRST_PERSON_RELOAD_S;
				break;
			case WeaponUse::Burst:
				if (Carried(w.ammo) == 0) break;
				this->firing = true;
				this->mouse_fire = mouse;
				this->burst_left = mouse ? WALKER_ITEM_MAX : FIRST_PERSON_BURST_SHOTS;
				this->burst_time = 0;
				break;
			case WeaponUse::Rocket:
				if (this->reload_time > 0 || Carried(w.ammo) == 0) break;
				this->Shoot(w, 1.0);
				this->reload_time = FIRST_PERSON_ROCKET_RELOAD_S;
				break;
			case WeaponUse::Rod:
				this->FishClick();
				break;
		}
		this->Dress();
	}

	/** The bow let go: the arrow flies as fast as the bow was drawn, the next one set on the string a moment after. */
	void Release()
	{
		const double drawn = std::clamp(this->draw_time / FIRST_PERSON_BOW_DRAW_S, 0.0, 1.0);
		this->draw_time = -1;
		this->mouse_draw = false;
		this->Shoot(_first_person_weapons[this->weapon], 0.25 + 0.75 * drawn);
		this->reload_time = FIRST_PERSON_NOCK_S;
	}

	/**
	 * An arrow or a bolt shot where the walker looks (the dot in the middle), from just below its eyes: one less of it
	 * in the inventory (the command), flying along its arc (Fly()).
	 * @param power how fast, of the weapon's speed
	 */
	void Shoot(const FirstPersonWeapon &w, double power)
	{
		if (Carried(w.ammo) == 0) return;
		Command<Commands::ShootWalkerItem>::Post(w.ammo);
		const double h = this->walker.heading, p = this->walker.pitch;
		const double fx = std::cos(h) * std::cos(p), fy = std::sin(h) * std::cos(p), fz = std::sin(p);
		const double speed = w.speed_ms * power;
		FirstPersonMissile m;
		m.thing = w.missile;
		m.damage = w.damage;
		const ThreeDVehicleObject *fig = GetThreeDFigure(w.missile);
		const ThreeDVehicleShape *shape = fig != nullptr ? fig->GetShape(0) : nullptr;
		if (shape != nullptr) m.length_m = shape->x1;
		m.x = this->walker.x + fx * 0.3 * OBJECT_UNITS_PER_METRE;
		m.y = this->walker.y + fy * 0.3 * OBJECT_UNITS_PER_METRE;
		m.z = FirstPersonFeetZ(this->walker) + this->walker.eye_z + (fz * 0.3 - 0.12) * OBJECT_HEIGHT_PER_METRE;
		m.vx = fx * speed;
		m.vy = fy * speed;
		m.vz = fz * speed;
		m.facing = static_cast<float>(h * 180.0 / M_PI);
		m.pitch = static_cast<float>(p * 180.0 / M_PI);
		m.rocket = w.use == WeaponUse::Rocket;
		this->missiles.push_back(m);
		if (this->missiles.size() > FIRST_PERSON_MISSILES) this->missiles.erase(this->missiles.begin());
		this->kick = 1.0f;
		if (auto sound = GetThreeDThingSound(w.thing, "vystrel"); sound.has_value()) SndPlayFx(*sound);
	}

	/**
	 * The arrows and the bolts on: each along its arc, falling, turned along its way, in steps of half a metre so that
	 * none passes through a dragon; its tip in a dragon (the dragon takes it, HitThreeDDragonAt()), in the water or off
	 * the map, it is gone; in the ground or a wall, it stays stuck a while.
	 */
	void Fly(double dt)
	{
		/* The rockets' smoke and blasts, as they fade; the dragons' smoke and fire drifting, the flames slowing in the air. */
		for (FirstPersonBlast &b : this->blasts) {
			b.age += dt;
			b.x += b.vx * dt * OBJECT_UNITS_PER_METRE;
			b.y += b.vy * dt * OBJECT_UNITS_PER_METRE;
			b.z += b.vz * dt * OBJECT_HEIGHT_PER_METRE;
			if (b.flame) {
				const double slow = std::max(0.0, 1.0 - 2.0 * dt);
				b.vx *= slow;
				b.vy *= slow;
				b.vz = b.vz * slow + 1.5 * dt;
			}
		}
		std::erase_if(this->blasts, [](const FirstPersonBlast &b) { return b.age > b.Life(); });
		this->Smoke(dt);
		for (FirstPersonMissile &m : this->missiles) {
			m.age += dt;
			if (m.stuck || m.gone) continue;
			if (m.rocket) {
				/* Its smoke behind it, from its tail (its origin). */
				m.smoke_time -= dt;
				while (m.smoke_time <= 0) {
					this->blasts.push_back({m.x, m.y, m.z, 0, true});
					m.smoke_time += FIRST_PERSON_PUFF_EVERY_S;
				}
			}
			const int n = std::max(1, static_cast<int>(std::ceil(std::sqrt(m.vx * m.vx + m.vy * m.vy + m.vz * m.vz) * dt / 0.5)));
			const double sub = dt / n;
			/* How high its tip was over the ground before the step (where it went in, when it goes in). */
			double above = -1;
			for (int k = 0; k < n && !m.stuck && !m.gone; k++) {
				const double px = m.x, py = m.y, pz = m.z;
				/* A rocket flies straight (the colleague: "it flies straight on, 40 m/s"). */
				if (!m.rocket) m.vz -= FIRST_PERSON_GRAVITY * sub;
				m.x += m.vx * sub * OBJECT_UNITS_PER_METRE;
				m.y += m.vy * sub * OBJECT_UNITS_PER_METRE;
				m.z += m.vz * sub * OBJECT_HEIGHT_PER_METRE;
				const double level = std::hypot(m.vx, m.vy), all = std::hypot(level, m.vz);
				const double ux = m.vx / all, uy = m.vy / all, uz = m.vz / all;
				m.facing = static_cast<float>(std::atan2(m.vy, m.vx) * 180.0 / M_PI);
				m.pitch = static_cast<float>(std::atan2(m.vz, level) * 180.0 / M_PI);
				const double tx = m.x + ux * m.length_m * OBJECT_UNITS_PER_METRE, ty = m.y + uy * m.length_m * OBJECT_UNITS_PER_METRE;
				const double tz = m.z + uz * m.length_m * OBJECT_HEIGHT_PER_METRE;
				if (tx < TILE_SIZE || ty < TILE_SIZE || tx >= (Map::SizeX() - 1) * TILE_SIZE || ty >= (Map::SizeY() - 1) * TILE_SIZE) {
					m.gone = true;
					continue;
				}
				if (HitThreeDDragonAt(tx, ty, tz, m.damage) || HitThreeDWitchAt(tx, ty, tz) || HitThreeDAnimalAt(tx, ty, tz, m.damage)) {
					m.gone = true;
					if (m.rocket) this->Explode(tx, ty, tz, m.damage);
					continue;
				}
				const double ground = FirstPersonGroundZ(tx, ty);
				const TileIndex tile = TileVirtXY(static_cast<int>(tx), static_cast<int>(ty));
				if (tz <= ground) {
					if (IsTileType(tile, TileType::Water) && !IsCoast(tile)) {
						m.gone = true;
						continue;
					}
					/* Into the ground where its tip met it, and a few centimetres in -- not as far as the step went. */
					const double t = above > 0 ? above / (above + (ground - tz)) : 0.0;
					m.x = px + (m.x - px) * t + ux * 0.06 * OBJECT_UNITS_PER_METRE;
					m.y = py + (m.y - py) * t + uy * 0.06 * OBJECT_UNITS_PER_METRE;
					m.z = pz + (m.z - pz) * t + uz * 0.06 * OBJECT_HEIGHT_PER_METRE;
					m.stuck = true;
				} else if (!FirstPersonWalkable(tx, ty) && !IsTileType(tile, TileType::Water) && tz - ground < FirstPersonWallTop(tx, ty)) {
					/* A house or a building, under its top: stuck in its wall, where it was before the step. */
					m.x = px;
					m.y = py;
					m.z = pz;
					m.stuck = true;
				}
				above = tz - ground;
			}
			if (m.stuck) m.age = 0;
			/* A rocket where it hit the ground or a wall: its blast, nothing of it left. */
			if (m.rocket && m.stuck) {
				m.gone = true;
				this->Explode(m.x, m.y, m.z, m.damage);
			}
		}
		std::erase_if(this->missiles, [](const FirstPersonMissile &m) {
			return m.gone || m.age > (m.stuck ? FIRST_PERSON_STUCK_S : FIRST_PERSON_FLY_S);
		});
	}

	/**
	 * A rocket's blast (the colleague: "at its end an explosion, of a radius of about 4 m, which should bring down a
	 * dragon, a witch and the ghost"): every dragon within it struck, every witch and ghost falls; seen as a ball of fire
	 * (ThreeDBlast), lighting about it at night.
	 */
	void Explode(double x, double y, double z, int damage)
	{
		BlastThreeDDragons(x, y, z, FIRST_PERSON_BLAST_M, damage);
		BlastThreeDWitches(x, y, z, FIRST_PERSON_BLAST_M);
		BlastThreeDAnimals(x, y, z, FIRST_PERSON_BLAST_M, damage);
		this->blasts.push_back({x, y, z, 0, false});
		/* The cloud it leaves: a puff in its middle and round it, a little higher. */
		const double m = OBJECT_UNITS_PER_METRE, up = OBJECT_HEIGHT_PER_METRE;
		this->blasts.push_back({x, y, z + 0.5 * up, 0, true, 3.0});
		for (int k = 0; k < 4; k++) {
			const double a = k * M_PI / 2 + 0.4;
			this->blasts.push_back({x + 1.4 * m * std::cos(a), y + 1.4 * m * std::sin(a), z + (0.2 + 0.3 * k) * up, 0, true, 2.5});
		}
	}

	/**
	 * A dragon of a farm fed (FeedDragon()): one less of it, through the command; the dragon then smokes the marijuana or
	 * breathes the beer out as fire (Smoke()), the hay it only eats.
	 * @return whether it was fed
	 */
	bool FeedDragon(uint32_t place, WalkerItem item)
	{
		if (!ThreeDRestingDragonAt(place).has_value()) return false;
		if (!Command<Commands::FeedWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_FEED, item)) return false;
		if (item == WalkerItem::Hay) return true;
		std::erase_if(this->feedings, [place](const FirstPersonFeeding &f) { return f.place == place; });
		FirstPersonFeeding &f = this->feedings.emplace_back();
		f.place = place;
		f.item = item;
		f.mouth = item == WalkerItem::Beer ? FIRST_PERSON_GULP_S : 0.3;
		return true;
	}

	/**
	 * The dragons fed (the player, 26. 10.: "I give it marijuana and it smokes round the dragon", "rather smoke and fire
	 * out of its mouth"): the marijuana smoked, in breaths of smoke out of its mouth and a cloud about it, rising; the
	 * beer, after a gulp, fire out of its mouth with dark smoke over it, and a little smoke after. Each where the dragon
	 * is now; one gone off its place (flying out) is done with it.
	 */
	void Smoke(double dt)
	{
		const double m = OBJECT_UNITS_PER_METRE, up = OBJECT_HEIGHT_PER_METRE;
		auto rnd = [](double from, double to) { return from + (to - from) * InteractiveRandomRange(1000) / 1000.0; };
		for (FirstPersonFeeding &f : this->feedings) {
			f.age += dt;
			const std::optional<ThreeDRestingDragon> d = ThreeDRestingDragonAt(f.place);
			if (!d.has_value()) {
				f.age = f.Life() + 1;
				continue;
			}
			const double a = d->facing * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
			/* Its mouth: in front of it, a little over half its height. */
			const double mx = d->x + ca * d->length_m * FIRST_PERSON_MOUTH_AHEAD * m, my = d->y + sa * d->length_m * FIRST_PERSON_MOUTH_AHEAD * m, mz = d->z + d->height_m * FIRST_PERSON_MOUTH_UP * up;
			auto puff = [&](double x, double y, double z, double size, uint32_t colour, double vx, double vy, double vz) {
				FirstPersonBlast &b = this->blasts.emplace_back();
				b = {x, y, z, 0, true, size};
				b.vx = vx;
				b.vy = vy;
				b.vz = vz;
				b.colour = colour;
			};
			if (f.item == WalkerItem::Marijuana) {
				/* Out of its mouth: a breath of smoke, then a breath taken. */
				for (f.mouth -= dt; f.mouth <= 0; f.mouth += 0.07) {
					if (std::fmod(f.age, FIRST_PERSON_BREATH_S) > 0.8) continue;
					const double turn = rnd(-0.3, 0.3), speed = rnd(1.0, 1.8);
					puff(mx, my, mz, rnd(0.6, 1.0), 0xDCDED2, std::cos(a + turn) * speed, std::sin(a + turn) * speed, rnd(0.6, 1.0));
				}
				/* About it: a cloud round its body, as long and as wide as it is, rising slowly. */
				for (f.round -= dt; f.round <= 0; f.round += 0.15) {
					if (f.age > FIRST_PERSON_SMOKE_S - 1.5) continue;
					const double t = rnd(0, 2 * M_PI), along = std::cos(t) * (d->length_m / 2 + 0.5), across = std::sin(t) * (d->width_m / 2 + 0.5);
					const double x = d->x + (ca * along - sa * across) * m, y = d->y + (sa * along + ca * across) * m;
					puff(x, y, d->z + rnd(0.2, 0.9) * d->height_m * up, rnd(2.0, 3.0), 0xC8CCC2, std::cos(t) * 0.2, std::sin(t) * 0.2, rnd(0.25, 0.45));
				}
			} else {
				const double fire_end = FIRST_PERSON_GULP_S + FIRST_PERSON_FIRE_S;
				for (f.mouth -= dt; f.mouth <= 0; f.mouth += f.age < fire_end ? 0.025 : 0.1) {
					if (f.age < FIRST_PERSON_GULP_S) continue;
					if (f.age >= fire_end) {
						/* After the fire: a little smoke out of its mouth. */
						puff(mx, my, mz, 1.0, 0x6A6660, ca * 0.5, sa * 0.5, 0.5);
						continue;
					}
					/* The fire: flames out of its mouth up into the air, as a column seen from wherever the walker
					 * stands -- not at the walker before it, whose eyes it would fill --, spread a little, slowing as
					 * they grow. */
					const double turn = rnd(-0.12, 0.12), lift = rnd(0.7, 0.95);
					FirstPersonBlast &b = this->blasts.emplace_back();
					b = {mx, my, mz, 0, false, rnd(0.5, 0.9)};
					b.flame = true;
					b.lights = InteractiveRandomRange(6) == 0;
					b.vx = std::cos(a + turn) * std::cos(lift) * FIRST_PERSON_FLAME_SPEED;
					b.vy = std::sin(a + turn) * std::cos(lift) * FIRST_PERSON_FLAME_SPEED;
					b.vz = std::sin(lift) * FIRST_PERSON_FLAME_SPEED;
					/* Dark smoke over its end now and then. */
					if (InteractiveRandomRange(5) == 0) {
						const double far = rnd(2.5, 3.5);
						puff(mx + ca * far * m, my + sa * far * m, mz + far * 1.3 * up, rnd(1.2, 1.8), 0x4A4642, ca * 0.5, sa * 0.5, rnd(0.8, 1.2));
					}
				}
			}
		}
		std::erase_if(this->feedings, [](const FirstPersonFeeding &f) { return f.age > f.Life(); });
	}

	/**
	 * The rockets' smoke and blasts as the eyes see them (ThreeDBlast): a puff grows and fades, a blast swells and fades
	 * fast; and the flame behind each rocket flying, flickering.
	 */
	void SeeBlasts()
	{
		std::vector<ThreeDBlast> &out = this->walker.blasts;
		out.clear();
		for (const FirstPersonMissile &m : this->missiles) {
			if (!m.rocket || m.stuck || m.gone) continue;
			ThreeDBlast &s = out.emplace_back();
			s.x = m.x;
			s.y = m.y;
			s.z = m.z;
			s.radius_m = 0.3 + 0.08 * std::sin(m.age * 90.0);
			s.alpha = 0.95f;
			s.colour = 0xFFE080;
			s.light = true;
		}
		for (const FirstPersonBlast &b : this->blasts) {
			ThreeDBlast &s = out.emplace_back();
			s.x = b.x;
			s.y = b.y;
			s.z = b.z;
			if (b.smoke) {
				const double t = b.age / b.Life();
				s.radius_m = 0.25 + b.size * t;
				s.alpha = static_cast<float>((b.size > 1 ? 0.7 : 0.55) * (1.0 - t));
				s.colour = b.colour != 0 ? b.colour : b.size > 1 ? 0x5C5852 : 0xB8B4AC;
			} else if (b.flame) {
				/* A flame of a dragon's fire: white-yellow out of the mouth, orange, red as it goes out. */
				const double t = b.age / FIRST_PERSON_FLAME_S;
				s.radius_m = 0.2 + b.size * std::sqrt(t);
				s.alpha = static_cast<float>(0.9 * (1.0 - t * t));
				s.colour = t < 0.2 ? 0xFFF0A0 : t < 0.55 ? 0xFFA040 : 0xE05020;
				s.light = true;
				s.lights_about = b.lights;
			} else {
				const double t = b.age / FIRST_PERSON_BLAST_S;
				s.radius_m = FIRST_PERSON_BLAST_M * std::min(1.0, 0.35 + 1.3 * t);
				s.alpha = static_cast<float>(0.95 * (1.0 - t));
				s.colour = t < 0.3 ? 0xFFF0A0 : 0xFF8A30;
				s.light = true;
			}
		}
	}

	/**
	 * A quick key (the player, 23. 10.: "the numbers of the weapons are not needed, the player assigns them as in
	 * Morrowind; the left hand the lights, the right the weapons"): the thing under it -- a weapon into the right hand
	 * (or put away when it is there), a light into the left (or put out), the beer or the marijuana used.
	 */
	void QuickKey(int k)
	{
		const Company *c = Company::GetIfValid(_local_company);
		if (c == nullptr || k < 0 || k >= static_cast<int>(WALKER_QUICK_KEYS)) return;
		const WalkerItem item = static_cast<WalkerItem>(c->walker_quick[k]);
		if (item >= WalkerItem::End || Carried(item) == 0) return;
		for (size_t w = 0; w < std::size(_first_person_weapons); w++) {
			if (_first_person_weapons[w].item != item) continue;
			if (this->InHand() == item) {
				this->weapon_out = false;
			} else {
				this->Choose(static_cast<int>(w));
			}
			return;
		}
		const WalkerItemSpec &spec = GetWalkerItemSpec(item);
		if (spec.kind == WalkerItemKind::Light) {
			FirstPersonTakeLight(item);
		} else if (spec.heal != 0) {
			const TileIndex here = TileVirtXY(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y));
			Command<Commands::UseWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_USE, here, item);
		}
	}

	/** A weapon by its key, out in the hand (another put away first), when the walker carries it. */
	void Choose(int k)
	{
		if (k < 0 || k >= static_cast<int>(std::size(_first_person_weapons)) || Carried(_first_person_weapons[k].item) == 0) return;
		this->wanted = k;
		this->weapon_out = true;
	}

	/** The weapon in the hand or being taken there (out, Choose()), End for none. */
	WalkerItem InHand() const
	{
		return this->weapon_out ? _first_person_weapons[this->wanted].item : WalkerItem::End;
	}

	EventState OnKeyPress(char32_t key, uint16_t keycode) override
	{
		/* Esc leaves the view (the player, 25. 10.: the cursor is slow on his phone -- "when I want out of the first
		 * person view, it is not easy to hit" -- so a way out without the mouse). */
		if ((keycode & ~WKC_SPECIAL_KEYS) == WKC_ESC) {
			/* The menu to feed a dragon open: only it goes. */
			if (Window *feed = FindWindowById(WindowClass::DragonFeed, 0); feed != nullptr) {
				feed->Close();
				return EventState::Handled;
			}
			this->Close();
			return EventState::Handled;
		}
		/* Ctrl+Up, Ctrl+Down: walks on that way without the key held, as Up or Down pressed twice (WalkOn()) -- told by
		 * the key's own press, not by how it is held at the next step: on the player's phone the steps were too far
		 * apart for two quick presses to be seen (#300, 25. 10.: "pressing the arrow twice still does not walk on, let us
		 * try Ctrl+Up"). The arrow held with it is not a press that stops it again. */
		if (keycode == (WKC_CTRL | WKC_UP) || keycode == (WKC_CTRL | WKC_DOWN)) {
			const int k = keycode == (WKC_CTRL | WKC_UP) ? 0 : 1;
			this->walk_on = k == 0 ? 1 : -1;
			this->walk_held[k] = true;
			this->walk_tapped[k] = FIRST_PERSON_DOUBLE_TAP_S;
			return EventState::Handled;
		}
		/* In a train of MSTS with the keys of MSTS chosen: its desk's keys first -- W and S its reverser, A and D its
		 * throttle, not the walker's (simtrains_cab.h). */
		if (const Vehicle *rv = this->Riding(); rv != nullptr && this->seat.Key(rv, key, keycode)) {
			this->SetWidgetDirty(WID_FP_CANVAS);
			return EventState::Handled;
		}
		/* The walking keys are the walker's, not the toolbar's (they are read as held, Walk()). */
		switch (keycode & ~WKC_SPECIAL_KEYS) {
			case 'W': case 'A': case 'S': case 'D':
			case WKC_UP: case WKC_DOWN: case WKC_LEFT: case WKC_RIGHT:
				return EventState::Handled;
			default:
				break;
		}
		if (key == 'w' || key == 'a' || key == 's' || key == 'd' || key == 'W' || key == 'A' || key == 'S' || key == 'D') return EventState::Handled;
		/* E: into the vehicle near, or out of it (as using a thing in Morrowind). */
		if (key == 'e' || key == 'E') {
			this->BoardOrLeave();
			return EventState::Handled;
		}
		/* A submarine (its object's "ponoreni_m", ShipDiveM()): up with Ctrl+I or Page Up, down with Ctrl+F or Page Down
		 * (the player, test4: "I have Ctrl+F and Ctrl+I free, and set the players something usual for the height"), a
		 * metre a press, before the horn and the lights of I and F. */
		if (const Vehicle *rv = this->Riding(); rv != nullptr && ShipDiveRoomM(rv) > 0) {
			const uint16_t k = keycode & ~WKC_SPECIAL_KEYS;
			const bool ctrl = (keycode & WKC_CTRL) != 0;
			const int way = (ctrl && k == 'I') || keycode == WKC_PAGEUP ? -1 : (ctrl && k == 'F') || keycode == WKC_PAGEDOWN ? 1 : 0;
			if (way != 0) {
				SteerDive(rv, way);
				return EventState::Handled;
			}
		}
		/* In a vehicle, as in its cab: I the horn, F the lights, C the next camera, T its route or the wheel again. */
		if (const Vehicle *rv = this->Riding(); rv != nullptr) {
			if (key == 'i' || key == 'I') {
				CabHorn(rv);
				return EventState::Handled;
			}
			if (key == 'f' || key == 'F') {
				CabToggleLights(rv);
				return EventState::Handled;
			}
			if (key == 'c' || key == 'C') {
				CabNextCamera(this->camera);
				return EventState::Handled;
			}
			if (key == 't' || key == 'T') {
				CabToggleRoute(rv, this->arrows);
				this->SetWidgetDirty(WID_FP_CANVAS);
				this->SetWidgetDirty(WID_FP_ROUTE);
				return EventState::Handled;
			}
		}
		/* I: the inventory (as in Morrowind). */
		if (key == 'i' || key == 'I') {
			ShowWalkerInventoryWindow();
			return EventState::Handled;
		}
		/* 1 to 9: what the player put under the key in the inventory (Morrowind's quick keys, QuickKey()); F: the weapon
		 * out or away (as in Morrowind); Space: a swing of it, a shot. */
		if (key >= '1' && key <= '9') {
			this->QuickKey(static_cast<int>(key - '1'));
			return EventState::Handled;
		}
		if (key == 'f' || key == 'F') {
			this->weapon_out = !this->weapon_out;
			return EventState::Handled;
		}
		/* L: the torch, the lantern, the light put out (in the left hand, the weapon in the right). */
		if (key == 'l' || key == 'L') {
			this->NextLight();
			return EventState::Handled;
		}
		if (key == ' ' || (keycode & ~WKC_SPECIAL_KEYS) == WKC_SPACE) {
			this->Attack(false);
			return EventState::Handled;
		}
		return EventState::NotHandled;
	}

	bool OnRightClick([[maybe_unused]] Point pt, WidgetID widget) override
	{
		/* Looking about with the right mouse button held, as the map is dragged (OnScroll()). Pressed twice without
		 * looking about between (the player, 18. 10.: "two clicks of the right button and the view is centred again,
		 * as the map's turn is"; 23. 10.: "it puts only up and down back, the head stays turned right or left"):
		 * looking straight ahead the way the body faces, level. */
		if (widget != WID_FP_CANVAS || _scrolling_viewport) return false;
		if (this->Riding() != nullptr) {
			/* In a vehicle the head turns in its cab, as in the cab view (the player, 28. 10.: "boarding with E has no
			 * turning of the head"): the walker's own heading was turned, which the picture from the cab never read. */
			this->cab_look.Press(this->camera);
			this->SetWidgetDirty(WID_FP_CANVAS);
			return true;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!this->looked && now - this->look_clicked < std::chrono::milliseconds(THREE_D_DOUBLE_CLICK_MS)) {
			this->walker.heading = std::remainder(this->walker.heading - this->head, 2 * M_PI);
			this->head = 0;
			this->walker.pitch = 0;
			this->look_clicked = {};
			this->SetWidgetDirty(WID_FP_CANVAS);
		} else {
			this->look_clicked = now;
		}
		this->looked = false;
		/* The pointer pinned where the button went down, as in our drag of the map (map_drag.cpp): each move only how
		 * far the hand went, so no edge of the screen stops the look about (the player, 23. 10.: "it stops at 180" --
		 * the pointer had run from the middle to the edge of the screen). */
		_scrolling_viewport = true;
		_cursor.fix_at = true;
		_look_pinned = true;
		return true;
	}

	void OnScroll(Point delta) override
	{
		/* As the right button moves the view of the map (the player, #287: "turn left, right, up and down in keeping with
		 * the right button on the map"): the pointer to the right looks to the right, down looks down. */
		if (this->Riding() != nullptr) {
			this->cab_look.Move(this->camera, delta.x, delta.y);
			this->SetWidgetDirty(WID_FP_CANVAS);
			return;
		}
		if (delta.x != 0 || delta.y != 0) this->looked = true;
		this->walker.heading = std::remainder(this->walker.heading - delta.x * FIRST_PERSON_MOUSE, 2 * M_PI);
		this->head = std::remainder(this->head - delta.x * FIRST_PERSON_MOUSE, 2 * M_PI);
		this->walker.pitch = std::clamp(this->walker.pitch - delta.y * FIRST_PERSON_MOUSE, -FIRST_PERSON_PITCH_MAX, FIRST_PERSON_PITCH_MAX);
		this->SetWidgetDirty(WID_FP_CANVAS);
	}

	/** The walker's health changed (a dragon's blow, CmdHurtWalker()): with none left, back to the map at once. */
	void OnInvalidateData([[maybe_unused]] int data = 0, bool gui_scope = true) override
	{
		if (!gui_scope) return;
		const Company *c = Company::GetIfValid(_local_company);
		if (c != nullptr && c->walker_health == 0) this->Close();
	}

	void OnPaint() override
	{
		this->SetWidgetLoweredState(WID_FP_RUN, this->running);
		const Vehicle *rv = this->Riding();
		this->SetWidgetLoweredState(WID_FP_LOAD, rv != nullptr && rv->cab_cargo == Vehicle::CAB_CARGO_LOAD);
		this->SetWidgetLoweredState(WID_FP_UNLOAD, rv != nullptr && rv->cab_cargo == Vehicle::CAB_CARGO_UNLOAD);
		this->SetWidgetsDisabledState(rv == nullptr || rv->owner != _local_company || rv->type != VehicleType::Road, WID_FP_LOAD, WID_FP_UNLOAD);
		this->GetWidget<NWidgetCore>(WID_FP_CENTRE)->SetToolTip(rv != nullptr ? STR_FIRST_PERSON_STAND_TOOLTIP : STR_FIRST_PERSON_CENTRE_TOOLTIP);
		this->DrawWidgets();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		/* Its route, or the wheel again (as the cab view's). */
		if (widget == WID_FP_ROUTE) return GetString(this->arrows.route ? STR_CAB_VIEW_DRIVE : STR_CAB_VIEW_AUTO);
		/* Riding, the driver's stand at the front or at the back of the vehicle (CabViewCamera::rear). */
		if (widget == WID_FP_CENTRE && this->Riding() != nullptr) return GetString(this->camera.rear ? STR_FIRST_PERSON_STAND_BACK : STR_FIRST_PERSON_STAND_FRONT);
		if (widget == WID_FP_CAMERA) return CabCameraText();
		if (widget == WID_FP_CONTROLS) return GetString(STR_CAB_VIEW_CONTROLS, _settings_client.gui.cab_msts_keys ? STR_CAB_VIEW_CONTROLS_MSTS : STR_CAB_VIEW_CONTROLS_ARROWS);
		return this->Window::GetWidgetString(widget, stringid);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_FP_CENTRE) {
			/* On the map, or the driver's stand when riding (as wide as its longest). */
			for (StringID s : {STR_FIRST_PERSON_CENTRE, STR_FIRST_PERSON_STAND_FRONT, STR_FIRST_PERSON_STAND_BACK}) size.width = std::max(size.width, GetStringBoundingBox(s).width + padding.width);
			return;
		}
		if (widget != WID_FP_CAMERA) return;
		const Dimension d = CabCameraTextSize();
		size.width = std::max(size.width, d.width + padding.width);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_FP_CANVAS: {
				/* How long the canvas takes to draw into the window, for the diary of the frames (frame_diary.h). */
				struct EyeWindowTimer {
					std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
					~EyeWindowTimer() { FrameDiaryEyeWindow(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - this->start).count()); }
				} eye_window_timer;
				int w = r.Width(), h = r.Height();
				if (w <= 0 || h <= 0) return;
				/* Made by the step; anew here only when the window changed its size since. */
				if (this->picture.width != w || this->picture.height != h) this->MakePicture();
				if (!this->picture_ok) {
					GfxFillRect(r, GetColourGradient(Colours::Grey, Shade::Darkest));
					DrawString(r.left, r.right, r.top + h / 2, STR_FIRST_PERSON_NEEDS_32BPP, TextColour::White, AlignmentH::Centre);
					return;
				}
				/* Hurt: red over the picture, fading. */
				BlitEyePicture(*this, this->picture, r.left, r.top, this->hurt_steps > 0 ? Colour(220, 0, 0, static_cast<uint8_t>(30 + this->hurt_steps * 18)).data : 0);
				/* In the cab of a train of MSTS its desk over the view (as the cab view's). */
				if (const Vehicle *rv = this->Riding(); rv != nullptr) this->seat.Draw(rv, this->camera, r.left, r.top, w, h);
				/* A dot in the middle, where it looks. */
				const int cx = r.left + w / 2, cy = r.top + h / 2;
				GfxFillRect(cx - 1, cy - 1, cx + 1, cy + 1, GetColourGradient(Colours::White, Shade::Lightest));
				/* The fishing: a fish biting (the player: "two seconds 'A bite! Click'"), or a word about it a moment, big
				 * under the dot. */
				const std::string fishing_word = this->fishing == FishingStage::Bite ? GetString(STR_FIRST_PERSON_FISH_BITE) : this->fish_say_left > 0 ? this->fish_say : std::string();
				if (!fishing_word.empty()) DrawString(r.left, r.right, cy + GetCharacterHeight(FontSize::Large) * 2, fishing_word, TextColour::Yellow, AlignmentH::Centre, false, FontSize::Large);
				/* The weapon out, and how many arrows or bolts there are for it. */
				const int line = r.bottom - GetCharacterHeight(FontSize::Normal) - WidgetDimensions::scaled.framerect.bottom * 2;
				if (this->weapon_out && this->Riding() == nullptr) {
					const FirstPersonWeapon &wp = _first_person_weapons[this->weapon];
					const StringID name = GetWalkerItemSpec(wp.item).name;
					const std::string text = wp.ammo != WalkerItem::End ? GetString(STR_FIRST_PERSON_WEAPON_AMMO, name, GetWalkerItemSpec(wp.ammo).name, Carried(wp.ammo)) : GetString(name);
					DrawString(r.left, r.right - WidgetDimensions::scaled.framerect.right * 2, line, text, TextColour::White, AlignmentH::End);
				}
				/* One line under the view (the player, #287: no long words there how it is played): the walker's health,
				 * the town it is near, and what can be done where it stands -- in a vehicle which and how fast; else E's use
				 * here or I at a shop, in yellow. */
				const int left = r.left + WidgetDimensions::scaled.framerect.left * 2, right = r.right - WidgetDimensions::scaled.framerect.right * 2;
				const Company *c = Company::GetIfValid(_local_company);
				const Town *town = ClosestTownFromTile(TileVirtXY(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y)), UINT_MAX);
				std::string text;
				if (c != nullptr && town != nullptr) {
					text = GetString(STR_FIRST_PERSON_LINE, c->walker_health, WALKER_HEALTH_MAX, town->index);
				} else if (c != nullptr) {
					text = GetString(STR_WALKER_HEALTH, c->walker_health, WALKER_HEALTH_MAX);
				} else if (town != nullptr) {
					text = GetString(STR_FIRST_PERSON_NEAR_TOWN, town->index);
				}
				std::string hint;
				bool riding = false;
				if (const Vehicle *rv = this->Riding(); rv != nullptr) {
					hint = GetString(this->arrows.route || rv->owner != _local_company ? STR_FIRST_PERSON_RIDING_ROUTE : STR_FIRST_PERSON_RIDING, rv->index, PackVelocity(rv->GetDisplaySpeed(), rv->type));
					/* A train driven from its desk: where its controls stand (as the cab view's panel). */
					if (const std::string desk = SimtrainsDeskText(rv); !desk.empty()) hint += "   " + desk;
					riding = true;
				} else if (this->fishing == FishingStage::Waiting) {
					/* The hook on the water: a fish to bite, a click reels it in. */
					hint = GetString(STR_FIRST_PERSON_FISH_WAITING);
				} else if (ThreeDEyeVehicle ev; this->Aboard(ev) != nullptr) {
					/* On board a boat: by its wheel E takes it, elsewhere over the side onto the land near. */
					if (this->AtHelm(ev)) {
						hint = GetString(STR_FIRST_PERSON_HELM_HERE, this->aboard);
					} else if (this->AshorePlace().has_value()) {
						hint = GetString(STR_FIRST_PERSON_ASHORE_HERE);
					}
				} else if (const WalkerPlant *plant = WalkerPlantNear(this->walker.x, this->walker.y, WALKER_PLANT_REACH_M); plant != nullptr) {
					/* By the walker's plant: how long till the harvest, or E harvests it. */
					switch (GetWalkerPlantStage(*plant)) {
						case WalkerPlantStage::Ripe: hint = GetString(STR_FIRST_PERSON_PLANT_RIPE); break;
						case WalkerPlantStage::Hay: hint = GetString(STR_FIRST_PERSON_PLANT_HAY); break;
						default: hint = GetString(STR_FIRST_PERSON_PLANT_GROWING, std::max<int32_t>(1, WALKER_PLANT_RIPE_DAYS - WalkerPlantAgeDays(*plant))); break;
					}
				} else if (const auto door = this->DoorToUse(); door.has_value()) {
					/* At a door or a window: E opens it, or shuts it. */
					const bool open = ThreeDDoorOpen(door->origin, door->door);
					if (ThreeDDoorIsWindow(door->house, door->door)) {
						hint = GetString(open ? STR_FIRST_PERSON_WINDOW_SHUT : STR_FIRST_PERSON_WINDOW_OPEN);
					} else {
						hint = GetString(open ? STR_FIRST_PERSON_DOOR_SHUT : STR_FIRST_PERSON_DOOR_OPEN);
					}
				} else if (const ThreeDMushroom *mushroom = ThreeDMushroomNear(this->walker.x, this->walker.y, FIRST_PERSON_PICK_M); mushroom != nullptr) {
					/* At a mushroom: E picks it. */
					const bool cs = std::string_view(_current_language->isocode).starts_with("cs");
					hint = GetString(STR_FIRST_PERSON_PICK_HERE, cs ? mushroom->title_cs : mushroom->title_en);
				} else if (ThreeDRestingDragonNear(this->walker.x, this->walker.y, FirstPersonFeetZ(this->walker), FIRST_PERSON_FEED_M).has_value()) {
					/* By a farm's dragon resting: E feeds it. */
					hint = GetString(STR_FIRST_PERSON_FEED_DRAGON);
				} else if (FirstPersonTentToStrike(this->walker.x, this->walker.y) != nullptr) {
					/* At the entrance of its tent: E folds it (before the grill, as BoardOrLeave). */
					hint = GetString(STR_FIRST_PERSON_TENT_STRIKE);
				} else if (ThreeDForestFireNear(this->walker.x, this->walker.y, FIRST_PERSON_GRILL_M * OBJECT_UNITS_PER_METRE) != INVALID_TILE) {
					/* By a forest's fire (or a tent's): E grills there. */
					hint = GetString(STR_FIRST_PERSON_GRILL);
				} else if (const StringID shop = WalkerShopName(FirstPersonShopNearby()); shop != INVALID_STRING_ID) {
					/* At a shop: I opens the inventory, where it is bought. */
					hint = GetString(STR_FIRST_PERSON_SHOP_HERE, shop);
				} else if (ThreeDMatNear(this->walker.x, this->walker.y, THREE_D_SELLER_REACH_M)) {
					/* By Mat walking about the town: I opens the inventory, where his marijuana is bought. */
					hint = GetString(STR_FIRST_PERSON_MAT_HERE);
				} else if (ThreeDDogSellerNear(this->walker.x, this->walker.y, THREE_D_SELLER_REACH_M) != nullptr) {
					/* At a seller of dogs: I opens the inventory, where its dogs are bought. */
					hint = GetString(STR_FIRST_PERSON_SELLER_HERE);
				} else if (ThreeDNurseNear(this->walker.x, this->walker.y, THREE_D_SELLER_REACH_M)) {
					/* At a nurse before a church: I opens the inventory, where the health insurance is bought. */
					hint = GetString(STR_FIRST_PERSON_NURSE_HERE);
				} else if (const Vehicle *near = this->VehicleToBoard(); near != nullptr) {
					/* Beside a vehicle: E gets into it. */
					hint = GetString(STR_FIRST_PERSON_BOARD_HERE, near->index);
				} else if (const auto yard = this->YardVehicleToTake(); yard.has_value()) {
					/* By the Zetor or the wheelbarrow of a farm: E takes one like it. */
					hint = GetString(STR_FIRST_PERSON_TAKE_VEHICLE, PackEngineNameDParam(yard->engine, EngineNameContext::Generic));
				}
				if (!hint.empty()) {
					if (text.empty()) {
						text = riding ? hint : GetString(STR_FIRST_PERSON_LINE_HINT_ONLY, hint);
					} else {
						text = GetString(riding ? STR_FIRST_PERSON_LINE_AND : STR_FIRST_PERSON_LINE_HINT, text, hint);
					}
				}
				DrawString(left, right, line, text, TextColour::White);
				break;
			}
			default:
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_FP_CANVAS:
				this->Attack(true);
				break;
			case WID_FP_HERE:
				/* Out of a vehicle first. */
				if (this->Riding() != nullptr) this->GetOut();
				this->riding = VehicleID::Invalid();
				this->PutWhereTheMainViewLooks();
				this->SetDirty();
				break;
			case WID_FP_CENTRE:
				/* Riding (the player, test4: "instead of the button 'On the map' the driver's stand changes by clicking one
				 * button"): to the other end of the vehicle, the eye turned at once. */
				if (this->Riding() != nullptr) {
					this->camera.rear = !this->camera.rear;
					this->camera.valid = false;
					this->camera.head = 0;
					this->SetDirty();
					break;
				}
				/* The map shows again: the view a window. */
				if (this->full) this->ToggleFull();
				ScrollMainWindowTo(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y), static_cast<int>(FirstPersonFeetZ(this->walker)));
				break;
			case WID_FP_RUN:
				this->running = !this->running;
				this->SetDirty();
				break;
			case WID_FP_ROUTE:
				if (const Vehicle *rv = this->Riding(); rv != nullptr) {
					CabToggleRoute(rv, this->arrows);
					this->SetDirty();
				}
				break;
			case WID_FP_LOAD:
			case WID_FP_UNLOAD:
				if (const Vehicle *rv = this->Riding(); rv != nullptr) {
					CabCargo(rv, widget == WID_FP_UNLOAD);
					this->SetDirty();
				}
				break;
			case WID_FP_CAMERA:
				if (this->Riding() != nullptr) CabNextCamera(this->camera);
				break;
			case WID_FP_CONTROLS:
				/* The arrows or the keys of MSTS at the desk of a train of MSTS (as the cab view's). */
				this->seat.ToggleControls();
				this->SetDirty();
				break;
			case WID_FP_INVENTORY:
				ShowWalkerInventoryWindow();
				break;
			case WID_FP_FULL:
				this->ToggleFull();
				break;
			default:
				break;
		}
	}
};

static WindowDesc _first_person_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::FirstPerson, WindowClass::None,
	{},
	_nested_first_person_widgets
);

/** Open the view from the first person, or bring it to the front; the walker stands where the main view looks. */
void ShowFirstPersonWindow()
{
	/* After the walker had no health left, it comes back whole. */
	const Company *c = Company::GetIfValid(_local_company);
	if (c != nullptr && c->walker_health == 0) Command<Commands::ReviveWalker>::Post(TileIndex{}, WALKER_HEALTH_MAX);
	AllocateWindowDescFront<FirstPersonWindow>(_first_person_desc, 0);
}

/** The vehicle of a yard given (CmdBuildYardRoadVehicle()): the walker who asked for it gets into it, and drives it. */
void CcBuildYardRoadVehicle(Commands, const CommandCost &result, VehicleID veh_id)
{
	if (result.Failed()) return;
	FirstPersonWindow *fp = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	const Vehicle *v = Vehicle::GetIfValid(veh_id);
	if (fp == nullptr || v == nullptr || fp->Riding() != nullptr) return;
	fp->GetIn(v);
}

/** The things a farm's dragon is fed, in the order of the lines of its window. */
static constexpr WalkerItem DRAGON_FOODS[] = {WalkerItem::Beer, WalkerItem::Marijuana, WalkerItem::Hay};

static constexpr NWidgetPart _nested_dragon_feed_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_DF_CAPTION), SetStringTip(STR_DRAGON_FEED_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown, WID_DF_LIST), SetFill(1, 0), EndContainer(),
};

/**
 * Feeding a farm's dragon the walker stands by (the player, 26. 10.: "E pops up a menu 'feed the dragon' and in it what
 * I have that I can give it, beer and marijuana, and when I choose, the window goes and the dragon smokes or breathes
 * out fire"; "we will feed hay too, and after the hay it does nothing"): a button for each of them the walker carries,
 * a click feeds it (FirstPersonWindow::FeedDragon()). It goes as the walker walks away or the dragon flies out.
 */
struct DragonFeedWindow : Window {
	uint32_t place; ///< the dragon (ThreeDRestingDragonAt())

	DragonFeedWindow(WindowDesc &desc, uint32_t place) : Window(desc), place(place)
	{
		this->InitNested(0);
	}

	/** The things it can be fed that the walker carries, as the buttons show them. */
	static std::vector<WalkerItem> Foods()
	{
		std::vector<WalkerItem> foods;
		for (WalkerItem item : DRAGON_FOODS) {
			if (FirstPersonWindow::Carried(item) > 0) foods.push_back(item);
		}
		return foods;
	}

	/** A button's height: tall enough for a finger on the phone. */
	static int ButtonHeight()
	{
		return GetCharacterHeight(FontSize::Normal) * 2 + WidgetDimensions::scaled.framerect.Vertical();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_DF_LIST) return;
		size.height = static_cast<uint>(std::max<size_t>(1, Foods().size()) * ButtonHeight()) + padding.height;
		size.width = std::max(size.width, GetStringBoundingBox(STR_DRAGON_FEED_NOTHING).width);
		for (WalkerItem item : DRAGON_FOODS) {
			size.width = std::max(size.width, GetStringBoundingBox(GetString(STR_DRAGON_FEED_ITEM, GetWalkerItemSpec(item).name, 999)).width);
		}
		size.width += padding.width + 4 * WidgetDimensions::scaled.framerect.Horizontal();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_DF_LIST) return;
		const Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		const int bh = ButtonHeight(), text = GetCharacterHeight(FontSize::Normal);
		const std::vector<WalkerItem> foods = Foods();
		if (foods.empty()) {
			DrawString(ir.left, ir.right, ir.top + (bh - text) / 2, STR_DRAGON_FEED_NOTHING, TextColour::Grey, AlignmentH::Centre);
			return;
		}
		int y = ir.top;
		for (WalkerItem item : foods) {
			const Rect b{ir.left, y + 1, ir.right, y + bh - 2};
			DrawFrameRect(b, Colours::Brown, {});
			DrawString(b.left, b.right, b.top + (b.Height() - text) / 2, GetString(STR_DRAGON_FEED_ITEM, GetWalkerItemSpec(item).name, FirstPersonWindow::Carried(item)), TextColour::White, AlignmentH::Centre);
			y += bh;
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_DF_LIST) return;
		const Rect ir = this->GetWidget<NWidgetBase>(WID_DF_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
		const std::vector<WalkerItem> foods = Foods();
		if (pt.y < ir.top) return;
		const size_t k = static_cast<size_t>((pt.y - ir.top) / ButtonHeight());
		if (k < foods.size()) this->Feed(foods[k]);
	}

	/** The dragon fed one of it; the window goes. */
	void Feed(WalkerItem item)
	{
		FirstPersonWindow *fp = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
		if (fp != nullptr) fp->FeedDragon(this->place, item);
		this->Close();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (gui_scope) this->ReInit();
	}

	/** The walker walked away from the dragon, got into a vehicle, left the view, or the dragon flew out: no feeding then. */
	IntervalTimer<TimerWindow> check{std::chrono::milliseconds(250), [this](uint) {
		const FirstPersonWindow *fp = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
		const auto near = fp != nullptr && fp->Riding() == nullptr ? ThreeDRestingDragonNear(fp->walker.x, fp->walker.y, FirstPersonFeetZ(fp->walker), FIRST_PERSON_FEED_M + 2.0) : std::nullopt;
		if (!near.has_value() || near->place != this->place) this->Close();
	}};
};

static WindowDesc _dragon_feed_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::DragonFeed, WindowClass::None,
	{},
	_nested_dragon_feed_widgets
);

/** Open the window to feed a farm's dragon, instead of one open for another. */
static void ShowDragonFeedWindow(uint32_t place)
{
	CloseWindowById(WindowClass::DragonFeed, 0);
	new DragonFeedWindow(_dragon_feed_desc, place);
}

/**
 * The rig: the menu to feed a farm's dragon open, a click on the button of a thing (as the player would); "" nothing
 * done.
 * @return "otevreno" with what it offers, then what was fed: the effect seen ("kour", "ohen", "nic")
 */
std::string RigDragonFeed(std::string_view what)
{
	DragonFeedWindow *w = static_cast<DragonFeedWindow *>(FindWindowById(WindowClass::DragonFeed, 0));
	if (w == nullptr) return "zavreno";
	std::string out = "nabidka:";
	const std::vector<WalkerItem> foods = DragonFeedWindow::Foods();
	for (WalkerItem item : foods) out += " " + GetString(GetWalkerItemSpec(item).name);
	WalkerItem item = WalkerItem::End;
	if (what == "pivo") item = WalkerItem::Beer;
	if (what == "mari") item = WalkerItem::Marijuana;
	if (what == "seno") item = WalkerItem::Hay;
	const auto it = std::ranges::find(foods, item);
	if (it == foods.end()) return out;
	const Rect ir = w->GetWidget<NWidgetBase>(WID_DF_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
	w->OnClick({w->width / 2, ir.top + static_cast<int>(it - foods.begin()) * DragonFeedWindow::ButtonHeight() + DragonFeedWindow::ButtonHeight() / 2}, WID_DF_LIST, 1);
	return out + fmt::format(", krmeno {}, okno {}", what, FindWindowById(WindowClass::DragonFeed, 0) != nullptr ? "zustalo" : "zmizelo");
}

static constexpr NWidgetPart _nested_forest_grill_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_FG_CAPTION), SetStringTip(STR_FOREST_GRILL_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown, WID_FG_LIST), SetFill(1, 0), EndContainer(),
};

/**
 * The grill over a forest's fire the walker stands by (the player, 29. 10.: "E at the fireplace, a menu: grill a lamb,
 * grill a piglet, grill a calf, and over the fireplace the game's grill of the pitch and the pub"; only to be looked
 * at, the animal not needed): a button for each animal, the spit with it over the fire (SetThreeDForestGrill()), and
 * with one on it a button to eat it, the grill gone with it (the player: "not to take it off, to eat it, and the grill
 * goes"). It goes as the walker walks away.
 */
struct ForestGrillWindow : Window {
	TileIndex fire; ///< the tile of the fire (ThreeDForestFireNear())

	/** The animals as the player named them, by their frames of the spit (ThreeDForestPart::spit). */
	static constexpr std::array<std::pair<int, StringID>, THREE_D_GRILL_ANIMALS> ANIMALS = {{{2, STR_FOREST_GRILL_LAMB}, {0, STR_FOREST_GRILL_PIGLET}, {1, STR_FOREST_GRILL_CALF}}};

	ForestGrillWindow(WindowDesc &desc, TileIndex fire) : Window(desc), fire(fire)
	{
		this->InitNested(0);
	}

	/** The buttons: the animals, and eating it with one on the spit; -1 that one. */
	std::vector<int> Buttons() const
	{
		std::vector<int> buttons;
		for (const auto &[animal, name] : ANIMALS) buttons.push_back(animal);
		if (ThreeDForestGrill(this->fire) >= 0) buttons.push_back(-1);
		return buttons;
	}

	static StringID ButtonName(int animal)
	{
		for (const auto &[a, name] : ANIMALS) {
			if (a == animal) return name;
		}
		return STR_FOREST_GRILL_EAT;
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_FG_LIST) return;
		size.height = static_cast<uint>(this->Buttons().size() * DragonFeedWindow::ButtonHeight()) + padding.height;
		for (const auto &[animal, name] : ANIMALS) size.width = std::max(size.width, GetStringBoundingBox(name).width);
		size.width = std::max(size.width, GetStringBoundingBox(STR_FOREST_GRILL_EAT).width);
		size.width += padding.width + 4 * WidgetDimensions::scaled.framerect.Horizontal();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_FG_LIST) return;
		const Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		const int bh = DragonFeedWindow::ButtonHeight(), text = GetCharacterHeight(FontSize::Normal);
		const int on = ThreeDForestGrill(this->fire);
		int y = ir.top;
		for (int animal : this->Buttons()) {
			const Rect b{ir.left, y + 1, ir.right, y + bh - 2};
			/* The animal on the spit now pressed in. */
			DrawFrameRect(b, Colours::Brown, animal >= 0 && animal == on ? FrameFlags{FrameFlag::Lowered} : FrameFlags{});
			DrawString(b.left, b.right, b.top + (b.Height() - text) / 2, ButtonName(animal), TextColour::White, AlignmentH::Centre);
			y += bh;
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_FG_LIST) return;
		const Rect ir = this->GetWidget<NWidgetBase>(WID_FG_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
		const std::vector<int> buttons = this->Buttons();
		if (pt.y < ir.top) return;
		const size_t k = static_cast<size_t>((pt.y - ir.top) / DragonFeedWindow::ButtonHeight());
		if (k >= buttons.size()) return;
		SetThreeDForestGrill(this->fire, buttons[k]);
		this->Close();
	}

	/** The walker walked away from the fire, got into a vehicle or left the view: no grilling then. */
	IntervalTimer<TimerWindow> check{std::chrono::milliseconds(250), [this](uint) {
		const FirstPersonWindow *fp = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
		const TileIndex near = fp != nullptr && fp->Riding() == nullptr ? ThreeDForestFireNear(fp->walker.x, fp->walker.y, (FIRST_PERSON_GRILL_M + 2.0) * OBJECT_UNITS_PER_METRE) : INVALID_TILE;
		if (near != this->fire) this->Close();
	}};
};

static WindowDesc _forest_grill_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::ForestGrill, WindowClass::None,
	{},
	_nested_forest_grill_widgets
);

/** Open the window of the grill over a forest's fire, instead of one open for another. */
static void ShowForestGrillWindow(TileIndex fire)
{
	CloseWindowById(WindowClass::ForestGrill, 0);
	new ForestGrillWindow(_forest_grill_desc, fire);
}

/**
 * The rig: the walker on board its boat at a point of its frame (metres, +x its bow), on the floor there no higher than
 * so much, facing so many degrees from its bow (into the rooms of the sailing yacht).
 * @return the floor it stands on, the room over it, its eyes
 */
std::string RigOnBoard(double x, double y, double up_to, double facing)
{
	FirstPersonWindow *fp = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (fp == nullptr) return "zadny chodec";
	ThreeDEyeVehicle ev;
	if (fp->aboard == VehicleID::Invalid() || fp->Aboard(ev) == nullptr) return "chodec neni na lodi";
	const ThreeDVehicleObject &obj = FirstPersonWindow::WalkedOn(*ev.obj);
	const std::optional<double> deck = ThreeDVehicleDeckZ(obj, x, y, up_to);
	if (!deck.has_value()) return "tam neni podlaha";
	fp->deck_x = x;
	fp->deck_y = y;
	fp->deck_z = *deck;
	fp->deck_heading = facing * M_PI / 180.0;
	fp->PlaceOnBoard(ev);
	fp->FitEyes();
	return fmt::format("{} podlaha {:.2f} m, mistnost {:.2f} m, oci {:.2f} m", ThreeDBoatRooms(*ev.obj) != nullptr ? "interier," : "obal,", *deck,
			FirstPersonWindow::RoomAt(obj, x, y, *deck), fp->walker.eye_z / OBJECT_HEIGHT_PER_METRE);
}

/**
 * The rig: the boat's floor about the walker on board, as a step finds it (ThreeDVehicleDeckZ() a step over its feet at
 * most): a row each quarter of a metre across, its height in decimetres, '.' none; '#' a wall at the chest between the
 * walker and there (ThreeDVehicleBlocks()), '@' the walker. +x its bow to the right.
 */
std::string RigDeckAround()
{
	FirstPersonWindow *fp = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (fp == nullptr) return "zadny chodec";
	ThreeDEyeVehicle ev;
	if (fp->aboard == VehicleID::Invalid() || fp->Aboard(ev) == nullptr) return "chodec neni na lodi";
	const ThreeDVehicleObject &obj = FirstPersonWindow::WalkedOn(*ev.obj);
	std::string out = fmt::format("na {:.2f},{:.2f} paluba {:.2f} m\n", fp->deck_x, fp->deck_y, fp->deck_z);
	for (int j = 6; j >= -6; j--) {
		for (int i = -6; i <= 6; i++) {
			const double x = fp->deck_x + i * 0.25, y = fp->deck_y + j * 0.25;
			if (i == 0 && j == 0) {
				out += "  @ ";
				continue;
			}
			const std::optional<double> deck = ThreeDVehicleDeckZ(obj, x, y, fp->deck_z + FIRST_PERSON_DECK_STEP_M);
			if (!deck.has_value()) {
				out += "  . ";
			} else if (ThreeDVehicleBlocks(obj, fp->deck_x, fp->deck_y, x, y, std::max(fp->deck_z, *deck) + FIRST_PERSON_DECK_CHEST_M)) {
				out += fmt::format("{:3d}#", static_cast<int>(std::lround(*deck * 10)));
			} else {
				out += fmt::format("{:3d} ", static_cast<int>(std::lround(*deck * 10)));
			}
		}
		out += "\n";
	}
	return out;
}

/**
 * The rig: the walker by the fire of the forest on a tile, E pressed there, a click on a button (as the player would:
 * "ovecka", "prasatko", "kravicka", "snist"; "" none).
 * @return the menu it offers and what is on the spit after
 */
std::string RigForestGrill(TileIndex tile, std::string_view what)
{
	FirstPersonWindow *fp = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (fp == nullptr) return "zadny chodec";
	/* The forest's fire, or the fireplace of a walker's tent on the tile (WalkerTent). */
	const ThreeDForestPart *forest = ThreeDGameForestPartAt(tile);
	double fx, fy;
	if (forest != nullptr && forest->grill != nullptr) {
		fx = TileX(tile) * TILE_SIZE + forest->fire[0] * OBJECT_UNITS_PER_METRE;
		fy = TileY(tile) * TILE_SIZE + forest->fire[1] * OBJECT_UNITS_PER_METRE;
	} else if (!WalkerTentFireAt(tile, fx, fy)) {
		return "zadny ohen";
	}
	/* Beyond the logs about the fire, on the side of the north corner (a tent's: on the side away from the tent). */
	const WalkerTent *tent = WalkerTentAt(tile);
	const double away = tent != nullptr ? tent->facing * M_PI / 180.0 : M_PI;
	fp->walker.x = fx + std::cos(away) * 3.2 * OBJECT_UNITS_PER_METRE;
	fp->walker.y = fy + std::sin(away) * 3.2 * OBJECT_UNITS_PER_METRE;
	fp->walker.heading = away + M_PI;
	fp->walker.pitch = -0.3;
	SetFocusedWindow(fp);
	fp->OnKeyPress('e', 'E');
	const double mx = (fx - TileX(tile) * TILE_SIZE) / OBJECT_UNITS_PER_METRE, my = (fy - TileY(tile) * TILE_SIZE) / OBJECT_UNITS_PER_METRE;
	ForestGrillWindow *w = static_cast<ForestGrillWindow *>(FindWindowById(WindowClass::ForestGrill, 0));
	if (w == nullptr) return fmt::format("ohen na {:.1f},{:.1f} m, okno zavreno", mx, my);
	std::string out = fmt::format("ohen na {:.1f},{:.1f} m, nabidka:", mx, my);
	const std::vector<int> buttons = w->Buttons();
	for (int animal : buttons) out += " " + GetString(ForestGrillWindow::ButtonName(animal));
	const int want = what == "prasatko" ? 0 : what == "kravicka" ? 1 : what == "ovecka" ? 2 : what == "snist" ? -1 : -2;
	const auto it = std::ranges::find(buttons, want);
	if (it != buttons.end()) {
		const Rect ir = w->GetWidget<NWidgetBase>(WID_FG_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
		w->OnClick({w->width / 2, ir.top + static_cast<int>(it - buttons.begin()) * DragonFeedWindow::ButtonHeight() + DragonFeedWindow::ButtonHeight() / 2}, WID_FG_LIST, 1);
	}
	fp->MakePicture();
	fp->SetDirty();
	return out + fmt::format("; na rozni {}", ThreeDForestGrill(tile));
}

/**
 * Open the view from the first person with the walker on a tile (the god's eye's crosshair, toolbar_gui.cpp): in its
 * middle, or the nearest place it can stand.
 */
void ShowFirstPersonWindowAt(TileIndex tile)
{
	ShowFirstPersonWindow();
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || w->Riding() != nullptr) return;
	/* Not into a fight with a dragon at once (the player, 6. 11.); a building the walker cannot go into, beside it
	 * (PutNear()). */
	double x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2.0, y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2.0;
	ThreeDDragonsKeepAway(x, y);
	w->PutNear(x, y, true);
	w->MakePicture();
	w->SetDirty();
}

/**
 * Where things are sold near the walker of the view from the first person (walker_inventory.h): the nearest tile
 * of a pub, a vending machine, a coffeeshop or a shop of a town, within a tile of where it stands.
 * @return the tile, or INVALID_TILE when there is no walker or nothing is sold near it
 */
TileIndex FirstPersonShopNearby()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return INVALID_TILE;
	TileIndex best = INVALID_TILE;
	double best_d = 1e18;
	const int tx = static_cast<int>(w->walker.x) / static_cast<int>(TILE_SIZE), ty = static_cast<int>(w->walker.y) / static_cast<int>(TILE_SIZE);
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int x = tx + dx, y = ty + dy;
			if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) continue;
			const TileIndex tile = TileXY(x, y);
			if (WalkerShopName(tile) == INVALID_STRING_ID) continue;
			const double cx = (x + 0.5) * TILE_SIZE - w->walker.x, cy = (y + 0.5) * TILE_SIZE - w->walker.y;
			if (cx * cx + cy * cy < best_d) {
				best_d = cx * cx + cy * cy;
				best = tile;
			}
		}
	}
	return best;
}

/** Whether the view from the first person is open over the whole screen. */
bool FirstPersonFull()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr && w->full;
}

/**
 * Whether the map is covered: by the view from the first person over the whole screen, or by the camera going round a
 * place (orbit_view.cpp). The map under it is then not drawn (widget.cpp), nor laid by the card.
 */
bool FirstPersonCoversMap()
{
	return FirstPersonFull() || OrbitViewOn();
}

/** The walker of the view from the first person: where it stands, world units; false when the view is not open (three_d_dragons.h). */
/** The vehicle the walker of the view from the first person is in (the rig: test3d inventar zraneni), nullptr on foot or without the view. */
const Vehicle *FirstPersonRiding()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr ? w->Riding() : nullptr;
}

/** The boat the walker of the view from the first person stands on (its rooms drawn from the eyes, ThreeDBoatRooms()), Invalid elsewhere. */
VehicleID FirstPersonBoatAboard()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr ? w->aboard : VehicleID::Invalid();
}

bool FirstPersonWalkerAt(double &x, double &y)
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return false;
	x = w->walker.x;
	y = w->walker.y;
	return true;
}

/** The height of the walker's eyes swimming, units of height of the game (three_d_water_life.cpp: the fish keep away from the diver); false not swimming. */
bool FirstPersonSwimmerEyeZ(double &z)
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || !w->walker.swimming) return false;
	z = FirstPersonFeetZ(w->walker) + w->walker.eye_z;
	return true;
}

/** Whether one can stand on the ground at a world point, as the walker can (among the buildings of an industry by their shapes; three_d_animals.cpp: the dogs of a farm's yard). */
bool FirstPersonGroundWalkable(double x, double y)
{
	return FirstPersonWalkable(x, y);
}

/** The rig: the direction the walker of the view from the first person faces, radians (0 without the window). */
double FirstPersonWalkerHeading()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr ? w->walker.heading : 0.0;
}

/** Whether the view from the first person is open. */
bool FirstPersonOpen()
{
	return FindWindowById(WindowClass::FirstPerson, 0) != nullptr;
}

/** The tile the walker of the view from the first person stands on, or INVALID_TILE without the window. */
TileIndex FirstPersonWalkerTile()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return INVALID_TILE;
	const int x = static_cast<int>(w->walker.x) / static_cast<int>(TILE_SIZE), y = static_cast<int>(w->walker.y) / static_cast<int>(TILE_SIZE);
	if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) return INVALID_TILE;
	return TileXY(x, y);
}

/** The rig: the walker of the view from the first person, where it is and how long its last picture took. */
std::string DescribeFirstPerson()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	const Vehicle *boat = w->aboard != VehicleID::Invalid() ? Vehicle::GetIfValid(w->aboard) : nullptr;
	std::string deck = boat == nullptr ? std::string() : fmt::format(", na palube lodi {} na {:.1f},{:.1f} m (paluba {:.2f} m)", boat->unitnumber, w->deck_x, w->deck_y, w->deck_z);
	if (const std::optional<double> surface = EyeWaterSurfaceZ(w->walker.x, w->walker.y); w->walker.swimming && surface.has_value()) {
		const double eyes = (*surface - (FirstPersonFeetZ(w->walker) + w->walker.eye_z)) / OBJECT_HEIGHT_PER_METRE;
		deck = fmt::format(", plave: oci {:.2f} m {} hladinou, dno {:.2f} m pod hladinou", std::abs(eyes), eyes > 0 ? "pod" : "nad", EyeWaterDepth(w->walker.x, w->walker.y) / OBJECT_HEIGHT_PER_METRE);
	}
	return fmt::format("chodec na {:.1f},{:.1f} (dlazdice {},{}){}, nohy {:.2f} m nad zemi, smer {:.0f}, pohled {:.0f}, {}x{} px, {:.1f} ms (sber {:.1f}, kresleni {:.1f}), {} dlazdic, {} plakatu",
			w->walker.x, w->walker.y, static_cast<int>(w->walker.x) / TILE_SIZE, static_cast<int>(w->walker.y) / TILE_SIZE, deck,
			(FirstPersonFeetZ(w->walker) - GetSlopePixelZOutsideMap(static_cast<int>(w->walker.x), static_cast<int>(w->walker.y))) / OBJECT_HEIGHT_PER_METRE, w->walker.heading * 180.0 / M_PI, w->walker.pitch * 180.0 / M_PI, w->picture.width, w->picture.height, w->camera.ms, w->camera.collect_ms, w->camera.draw_ms, w->camera.tiles, w->camera.billboards);
}

/**
 * The rig: the hammer in the hand at once, swung so far (degrees), or put away.
 * @return whether the walker has it out
 */
bool RigFirstPersonHold(bool out, float swing)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return false;
	w->weapon = w->wanted = 0;
	w->weapon_out = out;
	w->Hold(0);
	w->walker.held.raise = w->weapon_out ? 1.0f : 0.0f;
	w->Dress();
	w->walker.held.swing = swing;
	w->MakePicture();
	w->SetDirty();
	return w->weapon_out;
}

/**
 * The rig: a weapon by its key (1 to 5) in the hand at once; then a shot -- the bow drawn so many milliseconds and let
 * go, the crossbow shot, a sword swung (0 ms: only in the hand) -- and the arrows let fly so many steps.
 * @return what is held and where the arrows are
 */
/** The rig ('test3d chodec svetlo'): L pressed and the light held up at once; what is in the left hand. */
std::string RigFirstPersonLight()
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	w->NextLight();
	w->light_held = w->light;
	w->walker.left.raise = w->light >= 0 ? 1.0f : 0.0f;
	w->HoldLight(0);
	return w->walker.left.thing.empty() ? std::string("nic") : fmt::format("{} snimek {} uchyt {:.2f},{:.2f},{:.2f} barva {:06X} dosah {:.1f} m", w->walker.left.thing, w->walker.left.frame, w->walker.left.grip[0], w->walker.left.grip[1], w->walker.left.grip[2], w->walker.light_colour, w->walker.light_reach_m);
}

std::string RigFirstPersonWeapon(int key, int draw_ms, int steps)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	w->Choose(key - 1);
	w->weapon = w->wanted;
	w->walker.held.raise = w->weapon_out ? 1.0f : 0.0f;
	w->reload_time = 0;
	w->Dress();
	const double dt = FIRST_PERSON_TICK_MS / 1000.0;
	if (draw_ms > 0) {
		w->Attack(false);
		for (int t = 0; t < draw_ms; t += FIRST_PERSON_TICK_MS) w->Hold(dt);
		if (w->draw_time >= 0 && steps >= 0) w->Attack(false);
	}
	std::string held = fmt::format("drzi {} ({}) nabito {} zarez {:.3f},{:.3f},{:.3f} uchyt {:.3f},{:.3f},{:.3f}", w->walker.held.thing, w->weapon_out ? "venku" : "schovano",
			w->walker.held.loaded.empty() ? "-" : w->walker.held.loaded, w->walker.held.nock[0], w->walker.held.nock[1], w->walker.held.nock[2], w->walker.held.grip[0], w->walker.held.grip[1], w->walker.held.grip[2]);
	for (int i = 0; i < steps; i++) {
		w->Hold(dt);
		w->Fly(dt);
	}
	for (const FirstPersonMissile &m : w->missiles) {
		held += fmt::format("; {} na {:.1f},{:.1f} vyska {:.1f} m nad zemi, smer {:.0f} sklon {:.0f}{}", m.thing, m.x, m.y, (m.z - FirstPersonGroundZ(m.x, m.y)) / OBJECT_HEIGHT_PER_METRE, m.facing, m.pitch, m.stuck ? " zabodnuty" : "");
	}
	int puffs = 0, fires = 0;
	for (const FirstPersonBlast &b : w->blasts) (b.smoke ? puffs : fires)++;
	if (puffs + fires > 0) held += fmt::format("; kour {}x, vybuch {}x", puffs, fires);
	w->SeeBlasts();
	w->MakePicture();
	w->SetDirty();
	return held;
}

/**
 * The rig: the fishing rod (FishingStage) -- "prut" into the hand ("prut2" the short one), "nahod"/"klik" a click with it, "zabira" a fish to
 * bite now on the water -- then so many steps; how it fishes then, the picture made.
 */
int FirstPersonWeaponKey(WalkerItem item);

std::string RigFirstPersonFish(std::string_view what, int steps)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	if (what == "prut" || what == "prut2") {
		w->Choose(FirstPersonWeaponKey(what == "prut2" ? WalkerItem::ShortRod : WalkerItem::FishingRod) - 1);
		w->weapon = w->wanted;
		w->walker.held.raise = w->weapon_out ? 1.0f : 0.0f;
		w->Dress();
	} else if (what == "nahod" || what == "klik") {
		w->Attack(false);
	} else if (what == "zabira" && w->fishing == FishingStage::Waiting) {
		w->fish_bites_at = w->fish_time;
	}
	const double dt = FIRST_PERSON_TICK_MS / 1000.0;
	for (int i = 0; i < steps; i++) w->Hold(dt);
	static constexpr std::string_view STAGES[] = {"pripraveno", "leti", "ceka", "zabira", "chycena"};
	std::string out = fmt::format("{} ({:.1f} s), v ruce {}{}", STAGES[to_underlying(w->fishing)], w->fish_time, w->walker.held.thing.empty() ? std::string("nic") : w->walker.held.thing, w->walker.held.line ? " s vlascem" : "");
	if (!w->walker.held.hanging.empty()) out += fmt::format(", visi {} {:.1f} m pod spickou", w->walker.held.hanging, w->walker.held.hang_m);
	if (w->fishing == FishingStage::Flying || w->fishing == FishingStage::Waiting || w->fishing == FishingStage::Bite) {
		out += fmt::format(", hacek {:.1f} m pred chodcem, ocko {:.2f} m nad vodou", std::hypot(w->hook_x - w->walker.x, w->hook_y - w->walker.y) / OBJECT_UNITS_PER_METRE, (w->hook_z - w->water_z) / OBJECT_HEIGHT_PER_METRE);
		out += FishingInSea(w->hook_x, w->hook_y) ? ", more" : ", reka";
	}
	if (w->rod_fish >= 0) out += fmt::format(", ryba {} stav {}", _first_person_rod_fish[w->rod_fish].figure, w->rod_fish_colour);
	if (w->fish_say_left > 0) out += ", hlaska: " + w->fish_say;
	if (const Company *c = Company::GetIfValid(_local_company); c != nullptr) {
		out += fmt::format("; inventar prut {}, okoun {}, tercovec {}, zavojnatka {}", c->walker_items[to_underlying(WalkerItem::FishingRod)], c->walker_items[to_underlying(WalkerItem::Perch)],
				c->walker_items[to_underlying(WalkerItem::Discus)], c->walker_items[to_underlying(WalkerItem::Goldfish)]);
	}
	w->MakePicture();
	w->SetDirty();
	return out;
}

/**
 * The rig: the walker before the n-th dragon of the farms resting on its place, so many metres from its mouth at an angle
 * from its front, looking at its mouth.
 * @return where, or why not
 */
std::string RigFirstPersonAtDragon(int n, double metres, double angle_deg)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	const std::vector<ThreeDRestingDragon> resting = ThreeDRestingDragons();
	if (w == nullptr || n < 0 || static_cast<size_t>(n) >= resting.size()) return fmt::format("zadny (odpociva {})", resting.size());
	const ThreeDRestingDragon &d = resting[n];
	const double a = d.facing * M_PI / 180.0, m = OBJECT_UNITS_PER_METRE;
	const double mx = d.x + std::cos(a) * d.length_m * FIRST_PERSON_MOUTH_AHEAD * m, my = d.y + std::sin(a) * d.length_m * FIRST_PERSON_MOUTH_AHEAD * m;
	const double b = a + angle_deg * M_PI / 180.0;
	w->walker.x = mx + std::cos(b) * metres * m;
	w->walker.y = my + std::sin(b) * metres * m;
	FirstPersonSettle(w->walker);
	w->walker.heading = std::atan2(my - w->walker.y, mx - w->walker.x);
	w->walker.pitch = 0;
	w->MakePicture();
	w->SetDirty();
	return fmt::format("drak {} ({}x{}x{} m) na {:.2f},{:.2f} smer {:.0f}; chodec na {:.2f},{:.2f}, k tlame {:.1f} m", d.place, d.length_m, d.width_m, d.height_m, d.x / TILE_SIZE, d.y / TILE_SIZE, d.facing,
			w->walker.x / TILE_SIZE, w->walker.y / TILE_SIZE, std::hypot(mx - w->walker.x, my - w->walker.y) / m);
}

/**
 * The rig: in the menu to feed a dragon (E pressed before), the button of a thing clicked (RigDragonFeed()), then so
 * many steps of the dragon smoking or breathing fire; the smoke and the flames then, the picture made.
 */
std::string RigFirstPersonFeed(std::string_view what, int steps)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "bez chodce";
	std::string out = RigDragonFeed(what);
	const double dt = FIRST_PERSON_TICK_MS / 1000.0;
	for (int i = 0; i < steps; i++) w->Fly(dt);
	int puffs = 0, flames = 0;
	double far = 0;
	for (const FirstPersonBlast &b : w->blasts) {
		(b.flame ? flames : puffs)++;
		if (b.flame) far = std::max(far, std::hypot(b.x - w->walker.x, b.y - w->walker.y) / OBJECT_UNITS_PER_METRE);
	}
	out += fmt::format("; krmeni {}, kour {}x, plamenu {}x", w->feedings.size(), puffs, flames);
	if (flames > 0) out += fmt::format(" (nejdal {:.1f} m od chodce)", far);
	w->SeeBlasts();
	w->MakePicture();
	w->SetDirty();
	return out;
}

/** The arrows and the bolts of the walker in the view from the eyes (AddCabMissile()). */
void AddFirstPersonMissilesToEye()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	for (const FirstPersonMissile &m : w->missiles) AddCabMissile(m.thing, m.x, m.y, m.z, m.facing, m.pitch);
	/* The hook of the fishing rod flying out or on the water (its line drawn with the rod, ThreeDHeldThingTriangles()). */
	if (w->fishing == FishingStage::Flying || w->fishing == FishingStage::Waiting || w->fishing == FishingStage::Bite) {
		AddCabMissile(FIRST_PERSON_HOOK, w->hook_x, w->hook_y, w->hook_z, static_cast<float>(w->walker.heading * 180.0 / M_PI), 0.0f);
	}
}

/** The key of a weapon of the walker (1 to 5, as Choose() counts them from 0), 0 for a thing that is not one. */
int FirstPersonWeaponKey(WalkerItem item)
{
	for (size_t k = 0; k < std::size(_first_person_weapons); k++) {
		if (_first_person_weapons[k].item == item) return static_cast<int>(k) + 1;
	}
	return 0;
}

/** The weapon in the hand of the walker of the view from the first person, End for none or no such view open. */
WalkerItem FirstPersonWeaponInHand()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr ? w->InHand() : WalkerItem::End;
}

/**
 * A weapon clicked in the inventory (walker_inventory_gui.cpp; the player, 17. 10.: "a click on a thing in the inventory
 * does not use it, I want to take the bow in my hand"): into the hand, as its key does; the one in the hand put away.
 */
void FirstPersonTakeWeapon(WalkerItem item)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	const int key = FirstPersonWeaponKey(item);
	if (w == nullptr || key == 0) return;
	if (w->InHand() == item) {
		w->weapon_out = false;
	} else {
		w->Choose(key - 1);
	}
}

/** The light in the left hand of the walker of the view from the first person (the torch, the lantern), End for none. */
WalkerItem FirstPersonLightInHand()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr && w->light >= 0 ? _first_person_lights[w->light].item : WalkerItem::End;
}

/** A light clicked in the inventory (walker_inventory_gui.cpp): into the left hand, or put out when it is there. */
void FirstPersonTakeLight(WalkerItem item)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || w->Riding() != nullptr) return;
	for (int k = 0; k < static_cast<int>(std::size(_first_person_lights)); k++) {
		if (_first_person_lights[k].item != item) continue;
		w->light = w->light == k || w->Carried(item) == 0 ? -1 : k;
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
		return;
	}
}

/** The rig: make the picture so many times on so many threads (0 every core); the milliseconds a picture took. */
double RigFirstPersonMeasure(int times, int threads)
{
	extern int _cab_threads;
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || times <= 0) return 0;
	/* At once, each picture collected and drawn here: the time of one. */
	AutoRestoreBackup at_once(_eye_synchronous, true);
	_cab_threads = threads;
	double total = 0, collect = 0, draw = 0;
	for (int i = 0; i < times; i++) {
		w->MakePicture();
		total += w->camera.ms;
		collect += w->camera.collect_ms;
		draw += w->camera.draw_ms;
	}
	_cab_threads = 0;
	uint64_t sum = 0;
	for (uint32_t p : w->picture.pixels) sum = sum * 1000003u + p;
	Debug(misc, 0, "rig: first person picture checksum {:016x}; collecting {:.1f} ms under the lock, drawing {:.1f} ms; stages of collecting ms: tiles {:.1f}, collect {:.1f}, ground texture {:.1f}, ground {:.1f}, bridges and objects {:.1f}, billboards and figures {:.1f}, haze and night {:.1f}", sum,
			collect / times, draw / times, w->camera.stage[0], w->camera.stage[1], w->camera.stage[2], w->camera.stage[3], w->camera.stage[4], w->camera.stage[5], w->camera.stage[6]);
	return total / times;
}

/** The picture from the eyes made again at once, as it is drawn only as the walker steps (a setting of the night changed). */
void RedrawFirstPersonPicture()
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	w->MakePicture();
	w->SetDirty();
}

/**
 * The rig: the walker beside a vehicle (on foot, a few metres off its left), or E pressed (into the vehicle near, out
 * of the one it is in).
 * @return what the walker is in now, or VehicleID::Invalid() on foot
 */
VehicleID RigFirstPersonBoard(const Vehicle *beside, bool press_e)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return VehicleID::Invalid();
	if (beside != nullptr) {
		const double heading = DirectionToCabYaw(beside->direction) * 2 * M_PI / CAB_YAW_TURN, d = 3.0 * OBJECT_UNITS_PER_METRE;
		w->walker.x = beside->x_pos - std::sin(heading) * d;
		w->walker.y = beside->y_pos + std::cos(heading) * d;
	}
	if (press_e) {
		SetFocusedWindow(w);
		w->OnKeyPress('e', 'E');
	}
	w->SetDirty();
	return w->riding;
}

/**
 * The rig: the walker so many metres from a vehicle, at an angle from its front (0 before it, 90 on its left, 180
 * behind it), looking at it.
 */
/**
 * The rig ('test3d chodec dum <x> <y> <metres x> <metres y> <degrees>'): the walker at a place of the house on a tile
 * (or of what else has rooms: the dragon castle, the forester's cabin; elsewhere of the tile), looking the way given (0 along +x).
 */
std::string RigFirstPersonAtHouse(TileIndex tile, double mx, double my, double heading_deg)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	TileIndex origin = tile;
	const ThreeDObject *house = GetThreeDObjectWithRoomsAt(tile, origin);
	if (house == nullptr && !IsTileType(tile, TileType::House)) origin = tile; // anywhere: from the tile's north corner (a bridge's ramp)
	w->walker.x = TileX(origin) * TILE_SIZE + mx * OBJECT_UNITS_PER_METRE;
	w->walker.y = TileY(origin) * TILE_SIZE + my * OBJECT_UNITS_PER_METRE;
	w->walker.heading = heading_deg * M_PI / 180.0;
	w->walker.pitch = 0;
	const ThreeDObject *in = GetThreeDInterior(house);
	if (in != nullptr) in->Read(); // the rig: read now, there in the picture
	const bool inside = ThreeDInsideInterior(house, mx * OBJECT_UNITS_PER_METRE, my * OBJECT_UNITS_PER_METRE);
	const auto door = w->DoorToUse();
	w->MakePicture();
	w->SetDirty();
	return fmt::format("dum {} interier {} uvnitr {} dvere {} (u dveri {}{}), vyska {:.2f} m nad zemi domu", house != nullptr ? house->name : std::string("zadny"), in != nullptr ? in->name : std::string("zadny"),
			inside ? "ano" : "ne", house != nullptr ? house->door_wings.size() : 0, door.has_value() ? fmt::format("{}{}", door->door, ThreeDDoorOpen(door->origin, door->door) ? " otevrene" : " zavrene") : std::string("zadnych"),
			door.has_value() && ThreeDDoorIsWindow(door->house, door->door) ? " okno" : "", (FirstPersonFeetZ(w->walker) - GetTileMaxPixelZ(origin)) / OBJECT_HEIGHT_PER_METRE);
}

void RigFirstPersonLookAt(const Vehicle *v, double metres, double angle_deg)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || v == nullptr) return;
	const int yaw = v->cab_free ? v->cab_yaw : DirectionToCabYaw(v->direction);
	const double a = yaw * 2 * M_PI / CAB_YAW_TURN + angle_deg * M_PI / 180.0, d = metres * OBJECT_UNITS_PER_METRE;
	w->walker.x = v->x_pos + std::cos(a) * d;
	w->walker.y = v->y_pos + std::sin(a) * d;
	w->walker.heading = std::atan2(v->y_pos - w->walker.y, v->x_pos - w->walker.x);
	w->walker.pitch = 0;
	w->MakePicture();
	w->SetDirty();
}

/**
 * The rig: where the walker can stand about it, so many tiles each way, a character a half metre: '.' it can, '#' it
 * cannot (a house's wall), 'X' a house that is not a 3D object (all its tile), 'o' where it stands; the walker's circle
 * (FirstPersonFree()), as it walks, at the height it stands at (up on the dragon castle's walls, what stands over it).
 */
std::string RigFirstPersonFreeMap(int tiles, int step_cm)
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	const double step = std::max(step_cm, 5) / 100.0 * OBJECT_UNITS_PER_METRE;
	const int n = std::min(static_cast<int>(tiles * TILE_SIZE / step), 60);
	std::string out = fmt::format("volno kolem {:.1f},{:.1f} (x doprava, y dolu, po {} cm, polomer chodce {:.2f} m; ':' stred volny, kruh ne)", w->walker.x, w->walker.y, std::max(step_cm, 5), FIRST_PERSON_RADIUS / OBJECT_UNITS_PER_METRE);
	for (int r = -n; r <= n; r++) {
		std::string line = "\n";
		for (int c = -n; c <= n; c++) {
			const double px = w->walker.x + c * step, py = w->walker.y + r * step;
			const double feet = FirstPersonFeetZ(w->walker);
			char ch = FirstPersonFree(px, py, feet) ? '.' : FirstPersonWalkable(px, py, feet) ? ':' : '#';
			if (ch == '#' && px >= 0 && py >= 0 && px < Map::SizeX() * TILE_SIZE && py < Map::SizeY() * TILE_SIZE) {
				const TileIndex tile = TileVirtXY(static_cast<int>(px), static_cast<int>(py));
				if (IsTileType(tile, TileType::House)) {
					HouseID north = GetHouseType(tile);
					const TileIndex origin = tile + GetHouseNorthPart(north);
					if (EyeHouseObject(north, origin) == nullptr) ch = 'X';
				}
			}
			line += (r == 0 && c == 0) ? 'o' : ch;
		}
		out += line;
	}
	return out;
}

/** The rig: walk the walker as if the keys were held so many steps (ahead, aside, turn: -1, 0 or 1). */
/**
 * The rig: the walker to the mushroom nearest it (three tiles about), 1.4 m from it looking down at it; with pick, E
 * pressed there.
 * @return what was found and done
 */
std::string RigFirstPersonToMushroom(bool pick)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny chodec";
	double mx = 0, my = 0;
	std::string name;
	if (!ThreeDMushroomPlace(w->walker.x, w->walker.y, 3, mx, my, name)) return "zadna houba blizko";
	w->walker.x = mx - 1.4 * OBJECT_UNITS_PER_METRE;
	w->walker.y = my;
	w->walker.heading = 0;
	w->walker.pitch = -0.75;
	std::string out = fmt::format("{} na {:.1f},{:.1f}", name, mx / TILE_SIZE, my / TILE_SIZE);
	if (pick) {
		w->BoardOrLeave();
		out += ThreeDMushroomNear(w->walker.x, w->walker.y, FIRST_PERSON_PICK_M) == nullptr ? ", utrzena" : ", NEUTRZENA";
	}
	w->MakePicture();
	w->SetDirty();
	return out;
}

void RigFirstPersonWalk(int steps, int ahead, int aside, int turn, double pitch)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	w->walker.pitch = std::clamp(pitch, -FIRST_PERSON_PITCH_MAX, FIRST_PERSON_PITCH_MAX);
	for (int i = 0; i < steps; i++) {
		const double turned = turn * FIRST_PERSON_TURN * FIRST_PERSON_TICK_MS / 1000.0;
		w->walker.heading = std::remainder(w->walker.heading + turned, 2 * M_PI);
		/* On board a boat: the facing kept to the boat's bow (as Step()). */
		if (w->aboard != VehicleID::Invalid()) w->deck_heading = std::remainder(w->deck_heading + turned, 2 * M_PI);
		if (ahead == 0 && aside == 0) continue;
		const double speed = FIRST_PERSON_WALK_MS * OBJECT_UNITS_PER_METRE * FIRST_PERSON_TICK_MS / 1000.0;
		const double fx = std::cos(w->walker.heading), fy = std::sin(w->walker.heading);
		const double nx = w->walker.x + (fx * ahead + fy * aside) * speed, ny = w->walker.y + (fy * ahead - fx * aside) * speed;
		if (w->aboard != VehicleID::Invalid()) {
			w->MoveOnBoard(nx - w->walker.x, ny - w->walker.y);
			continue;
		}
		/* In the water as it swims (Swim()): after the eyes, up and down too. */
		if (w->walker.swimming) {
			w->Swim(ahead, aside, FIRST_PERSON_TICK_MS / 1000.0);
			continue;
		}
		/* As the walker walks: its circle against the walls, round a post (FirstPersonMove()), up the stairs; into the
		 * water off the shore. */
		FirstPersonSettle(w->walker);
		if (w->StepIntoWater(nx - w->walker.x, ny - w->walker.y)) continue;
		if (FirstPersonMove(w->walker, nx - w->walker.x, ny - w->walker.y)) FirstPersonSettle(w->walker);
	}
	w->MakePicture();
	w->SetDirty();
}

/** The rig: the walker looking the way given, degrees from the map's x towards its y (as RigFirstPersonWalk() turns it); on board a boat from its bow. */
void RigFirstPersonFace(double degrees)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return;
	w->walker.heading = std::remainder(degrees * M_PI / 180.0, 2 * M_PI);
	/* On board a boat: the degrees from its bow (towards its +y), the facing kept to it (as Step()). */
	if (ThreeDEyeVehicle ev; w->Aboard(ev) != nullptr) {
		w->deck_heading = w->walker.heading;
		w->PlaceOnBoard(ev);
	}
	w->MakePicture();
	w->SetDirty();
}

/** The rig ('test3d chodec voda <x> <y>'): the walker into the water in the middle of a tile, at its surface; whether it swims. */
bool RigFirstPersonIntoWater(TileIndex tile)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return false;
	w->IntoWater(TileX(tile) * TILE_SIZE + TILE_SIZE / 2.0, TileY(tile) * TILE_SIZE + TILE_SIZE / 2.0);
	w->MakePicture();
	w->SetDirty();
	return w->walker.swimming;
}

/** The rig: so many steps of the view's timer with no key held, as the walker stands (FirstPersonWindow::Step()). */
void RigFirstPersonStand(uint steps)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w != nullptr && steps > 0) w->Step(steps);
}

/**
 * The rig: W held so many frames of the game, each so many milliseconds long, the steps counted as the window's timer
 * counts them (IntervalTimer<TimerWindow>::Elapsed()); the metres walked.
 */
double RigFirstPersonHoldW(int frame_ms, int frames)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || frame_ms <= 0) return 0;
	SetFocusedWindow(w);
	const double x = w->walker.x, y = w->walker.y;
	_walkkeys.Set(DirectionKey::Up);
	int elapsed = 0;
	for (int i = 0; i < frames; i++) {
		elapsed += frame_ms;
		const uint count = elapsed / FIRST_PERSON_TICK_MS;
		elapsed %= FIRST_PERSON_TICK_MS;
		if (count > 0) w->Step(count);
	}
	_walkkeys.Reset(DirectionKey::Up);
	return std::hypot(w->walker.x - x, w->walker.y - y) / OBJECT_UNITS_PER_METRE;
}

/**
 * The rig: the arrow Up pressed twice (two frames held, so many frames let go between, frames of 40 ms), then let go so
 * many frames, then pressed once (Up, or Left with side) and let go as long again; the metres walked after the two
 * presses and after the one.
 */
/**
 * The rig: a quick double tap of Up as a player's fingers make it (down 90 ms, up 90 ms, down 90 ms, then let go), the
 * game looking at the keys once a frame of so many milliseconds (InputLoop()), as a slow frame does; the metres walked
 * in the two seconds after.
 */
double RigFirstPersonTapTimeline(int frame_ms)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || frame_ms <= 0) return 0;
	const double x0 = w->walker.x, y0 = w->walker.y;
	int elapsed = 0;
	for (int t = 0; t < 2000; t += frame_ms) {
		const bool down = t < 90 || (t >= 180 && t < 270);
		_dirkeys.Reset();
		_dirkeys.Set(DirectionKey::Up, down);
		elapsed += frame_ms;
		const uint count = elapsed / FIRST_PERSON_TICK_MS;
		elapsed %= FIRST_PERSON_TICK_MS;
		if (count > 0) w->Step(count);
	}
	_dirkeys.Reset();
	const double metres = std::hypot(w->walker.x - x0, w->walker.y - y0) / OBJECT_UNITS_PER_METRE;
	w->walk_on = 0;
	return metres;
}

std::pair<double, double> RigFirstPersonDoubleTap(int gap_frames, int frames, bool side)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return {};
	auto frame = [w](bool up, DirectionKey key = DirectionKey::Up) {
		_dirkeys.Reset();
		_dirkeys.Set(key, up);
		w->Step(1);
	};
	const double x0 = w->walker.x, y0 = w->walker.y;
	for (int press = 0; press < 2; press++) {
		frame(true);
		frame(true);
		for (int i = 0; i < gap_frames; i++) frame(false);
	}
	for (int i = 0; i < frames; i++) frame(false);
	const double x1 = w->walker.x, y1 = w->walker.y;
	const DirectionKey stop = side ? DirectionKey::Left : DirectionKey::Up;
	frame(true, stop);
	frame(true, stop);
	for (int i = 0; i < frames; i++) frame(false);
	_dirkeys.Reset();
	return {std::hypot(x1 - x0, y1 - y0) / OBJECT_UNITS_PER_METRE, std::hypot(w->walker.x - x1, w->walker.y - y1) / OBJECT_UNITS_PER_METRE};
}

/**
 * The rig: the right button held over the picture and the pointer moved so many times by dx (as the game's drag sends
 * it, OnScroll()), let go; then a walk of a few steps if asked, else two clicks of the right button. What it says: the
 * pointer pinned while held, the turn of the head and the look after the moves, and the look after the two clicks.
 */
/** The rig: the eye of the cab of the vehicle the walker rides in (its head turned, CabLook), nullptr on foot or none. */
const CabViewCamera *RigFirstPersonCabCamera()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr && w->Riding() != nullptr ? &w->camera : nullptr;
}

/*
 * The rig in a vehicle ('test3d kabina ...'): the window of the cab view is gone (the player, test4: "no cab view, E
 * gets in; in the rig remap the cab view to getting in with E"), the rig rides as the player does, in the view from
 * the first person.
 */

/** The view from the first person, nullptr not open. */
static FirstPersonWindow *RigFirstPerson()
{
	return static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
}

/** The rig ('kabina <n>'): the walker gets into a vehicle as with E, the view from the first person opened first. */
std::string RigRide(const Vehicle *v)
{
	if (v == nullptr) return "vozidlo neni";
	if (RigFirstPerson() == nullptr) ShowFirstPersonWindow();
	FirstPersonWindow *w = RigFirstPerson();
	if (w == nullptr) return "pohled z prvni osoby nejde otevrit";
	w->GetIn(v);
	return fmt::format("nastoupeno do vozidla {} (typ {})", v->unitnumber, to_underlying(v->type));
}

/** The rig: the vehicle the walker rides in, nullptr none. */
const Vehicle *RigRiding()
{
	const FirstPersonWindow *w = RigFirstPerson();
	return w != nullptr ? w->Riding() : nullptr;
}

/** The rig ('kabina zavri'): out of the vehicle, as E again. */
std::string RigRideOut()
{
	FirstPersonWindow *w = RigFirstPerson();
	if (w == nullptr || w->Riding() == nullptr) return "nikdo nejede";
	AutoRestoreBackup cur_company(_current_company, _local_company);
	w->GetOut();
	return "vystoupeno";
}

/**
 * The rig ('kabina klik <tlacitko>'): a button of the view in a vehicle clicked as the player clicks it: trasa (its
 * route or the wheel), nalozit, vylozit, kamera, cely (over the whole screen), zadni (the driver's stand at the other end).
 */
std::string RigRideClick(std::string_view name)
{
	static const std::pair<std::string_view, WidgetID> buttons[] = {
		{"trasa", WID_FP_ROUTE}, {"nalozit", WID_FP_LOAD}, {"vylozit", WID_FP_UNLOAD}, {"kamera", WID_FP_CAMERA}, {"cely", WID_FP_FULL}, {"zadni", WID_FP_CENTRE},
	};
	FirstPersonWindow *w = RigFirstPerson();
	const Vehicle *v = w != nullptr ? w->Riding() : nullptr;
	WidgetID id = -1;
	for (const auto &[n, wid] : buttons) if (name == n) id = wid;
	if (id < 0 || v == nullptr) return "takove tlacitko neni, nebo nikdo nejede";
	{
		AutoRestoreBackup cur_company(_current_company, _local_company);
		w->OnClick({}, id, 1);
	}
	return fmt::format("klik {}: vozidlo {} na {},{} smer {} smer kabiny {} volne {} zpet {} yaw {} tlacitko vypnuto {} stanoviste {}", name, v->unitnumber, v->x_pos, v->y_pos,
			to_underlying(v->direction), to_underlying(v->cab_heading), v->cab_free, v->cab_rejoin, v->cab_yaw, w->IsWidgetDisabled(id) ? "ano" : "ne", w->camera.rear ? "vzadu" : "vpredu");
}

/**
 * The rig ('kabina hlava <dx> <dy>'): the right button held on the picture from the vehicle and the hand moved so many
 * pixels, as the player looks about, let go.
 * @return how the head is turned then
 */
std::string RigRideLook(int dx, int dy)
{
	FirstPersonWindow *w = RigFirstPerson();
	if (w == nullptr || w->Riding() == nullptr) return "nikdo nejede";
	const bool taken = w->OnRightClick({}, WID_FP_CANVAS);
	if (dx != 0 || dy != 0) w->OnScroll({dx, dy});
	_scrolling_viewport = false;
	_cursor.fix_at = false;
	_look_pinned = false;
	return fmt::format("{}: otoceni {:.0f} stupnu, nahoru {:.0f}, kamera {}", taken ? "vzato" : "NEVZATO", w->camera.head * 180.0 / M_PI, w->camera.pitch * 180.0 / M_PI, to_underlying(CabCameraChosen()));
}

/** The rig ('kabina cas', after a key): the picture from the vehicle the walker rides in, how long it took and what was in it, and the vehicle. */
std::string DescribeRiding()
{
	const FirstPersonWindow *w = RigFirstPerson();
	const Vehicle *v = w != nullptr ? w->Riding() : nullptr;
	if (v == nullptr) return "nikdo nejede";
	std::string s = fmt::format("jizda ve vozidle {}: {}x{} px, {:.1f} ms (sber {:.1f}, kresleni {:.1f}), {} dlazdic, {} plakatu, smer {:.0f} stupnu, stanoviste {}", v->unitnumber, w->picture.width, w->picture.height,
			w->camera.ms, w->camera.collect_ms, w->camera.draw_ms, w->camera.tiles, w->camera.billboards, w->camera.heading * 180.0 / M_PI, w->camera.rear ? "vzadu" : "vpredu");
	s += fmt::format(", volne {} yaw {} limit {} rychlost {} stoji {} svetla {} (prani {})", v->cab_free, v->cab_yaw, v->cab_speed_limit, v->GetDisplaySpeed(), v->vehstatus.Test(VehState::Stopped) ? "ano" : "ne",
			VehicleLightsOn(v) ? "sviti" : "nesviti", v->First()->cab_lights);
	if (v->type == VehicleType::Aircraft) s += fmt::format(" vyska {} cil {} stav {} na {},{}", v->z_pos, v->cab_altitude, Aircraft::From(v)->state, v->x_pos, v->y_pos);
	if (v->type == VehicleType::Road) s += fmt::format(" na {},{},{} smer {} stav {:#x} skryte {}", v->x_pos, v->y_pos, v->z_pos, to_underlying(v->direction), RoadVehicle::From(v)->state, v->vehstatus.Test(VehState::Hidden) ? "ano" : "ne");
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v->First());
		s += fmt::format(" cur {} pult MSTS {} rizen {} vykon {} edb {} brzda {} primocinna {} rychlobrzda {} smer {} tfp {} na {},{} rozkaz {} ({})", t->cur_speed, IsSimtrainsTrain(v) ? "ano" : "ne",
				t->cab_driven, t->cab_power, t->cab_dynamic, t->cab_brake, t->cab_engine_brake, t->cab_emergency, t->cab_reverser, to_underlying(t->force_proceed),
				TileX(t->tile), TileY(t->tile), to_underlying(t->current_order.GetType()), w->seat.driver.done);
	}
	return s;
}

std::string RigFirstPersonLook(int dx, int moves, bool walk)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return "zadny pohled z oci";
	auto deg = [](double r) { return r * 180.0 / M_PI; };
	const double h0 = w->walker.heading;
	auto release = [] {
		_scrolling_viewport = false;
		_cursor.fix_at = false;
		_look_pinned = false;
	};
	w->OnRightClick({}, WID_FP_CANVAS);
	const bool pinned = _cursor.fix_at && _look_pinned;
	for (int i = 0; i < moves; i++) w->OnScroll({dx, 0});
	release();
	std::string s = fmt::format("pred {:.0f}, po tazeni {:.0f} (hlava {:.0f}), ukazatel prisp. {}", deg(h0), deg(w->walker.heading), deg(w->head), pinned ? "ano" : "NE");
	if (walk) {
		_dirkeys.Reset();
		_dirkeys.Set(DirectionKey::Up);
		for (int i = 0; i < 5; i++) w->Step(1);
		_dirkeys.Reset();
		s += fmt::format(", po chuzi {:.0f} (hlava {:.0f})", deg(w->walker.heading), deg(w->head));
	}
	w->walker.pitch = 0.5;
	w->OnRightClick({}, WID_FP_CANVAS);
	release();
	w->OnRightClick({}, WID_FP_CANVAS);
	release();
	s += fmt::format(", po dvojkliku {:.0f} (hlava {:.0f}, sklon {:.0f})", deg(w->walker.heading), deg(w->head), deg(w->walker.pitch));
	return s;
}

/** A quick key of the walker pressed in its inventory over no thing (walker_inventory_gui.cpp): as in the view from the first person. */
void FirstPersonQuickKey(int key)
{
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w != nullptr) w->QuickKey(key);
}
