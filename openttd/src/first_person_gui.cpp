/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
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
#include "three_d.h"
#include "three_d_objects.h"
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
#include "three_d_walkers.h"
#include "three_d_witches.h"
#include "vehicle_base.h"
#include "cab_drive.h"
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
/** How near a mushroom the walker picks it (E), metres. */
static constexpr double FIRST_PERSON_PICK_M = 1.5;
/** How near the middle of a door the walker opens it with E, metres. */
static constexpr double FIRST_PERSON_DOOR_M = 1.6;
/** How near a vehicle of the sets standing in an industry's yard the walker takes one like it with E (the farm's Zetor, its wheelbarrow), metres. */
static constexpr double FIRST_PERSON_YARD_VEHICLE_M = 2.5;
/** Farther than this from where the walker last stood (put somewhere else, not walked there), it stands on the ground storey again, metres. */
static constexpr double FIRST_PERSON_SETTLE_M = 2.0;

/** How a weapon of the walker is used. */
enum class WeaponUse : uint8_t {
	Swing,    ///< swung: the hammer, the swords
	Bow,      ///< drawn while held, shot let go
	Crossbow, ///< shot, then drawn again
	Burst,    ///< fires shot after shot while the button is held (the machine gun)
	Rocket,   ///< a rocket, flying straight, exploding where it hits; then loaded again (the rocket launcher)
};

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
};

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
		NWidget(WWT_TEXTBTN, Colours::Grey, WID_FP_RUN), SetStringTip(STR_FIRST_PERSON_RUN, STR_FIRST_PERSON_RUN_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Brown, WID_FP_INVENTORY), SetStringTip(STR_FIRST_PERSON_INVENTORY, STR_FIRST_PERSON_INVENTORY_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

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
			const ThreeDObject *obj = GetThreeDHouseObjectAt(north, origin);
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
			return IsCoast(tile);
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
		obj = GetThreeDHouseObjectAt(north, tile + GetHouseNorthPart(north));
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

/** Whether the walker can go to a world point from where it stands, at the height it would stand at there. */
static bool FirstPersonFreeFrom(const FirstPersonEye &w, double x, double y)
{
	return FirstPersonFree(x, y, FirstPersonStandZ(x, y, FirstPersonFeetZ(w)));
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
TileIndex FirstPersonShopNearby();
void RedrawFirstPersonPicture();

/** The window of the view from the first person; there is one, number 0. */
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
	int hurt_steps = 0;                   ///< steps the picture is still red of a dragon's blow
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

	/** The vehicle the walker is in, nullptr on foot. */
	const Vehicle *Riding() const
	{
		return this->riding != VehicleID::Invalid() ? Vehicle::GetIfValid(this->riding) : nullptr;
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
			const double d = std::hypot(u->x_pos - this->walker.x, u->y_pos - this->walker.y);
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

	/** The vehicle of a yard nearest the walker within reach (ThreeDIndustryTileParts::vehicles), shown and of an engine of the game. */
	std::optional<YardVehicle> YardVehicleToTake() const
	{
		if (!Company::IsValidID(_local_company)) return std::nullopt;
		const int tx = static_cast<int>(this->walker.x / TILE_SIZE), ty = static_cast<int>(this->walker.y / TILE_SIZE);
		std::optional<YardVehicle> best;
		double best_d = FIRST_PERSON_YARD_VEHICLE_M * OBJECT_UNITS_PER_METRE;
		std::vector<TileIndex> seen;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (tx + dx < 1 || ty + dy < 1 || tx + dx >= static_cast<int>(Map::MaxX()) || ty + dy >= static_cast<int>(Map::MaxY())) continue;
				const TileIndex tile = TileXY(tx + dx, ty + dy);
				if (!IsTileType(tile, TileType::Industry)) continue;
				const ThreeDIndustryTileParts *parts = GetThreeDOriginalIndustryTileAt(tile);
				if (parts == nullptr || !ThreeDOriginalIndustryTileStands(tile, *parts)) continue;
				/* The block's vehicles stand with its anchor, once whichever of its tiles is near. */
				const TileIndex anchor = GetThreeDOriginalIndustryAnchor(tile, parts);
				if (anchor == INVALID_TILE || std::ranges::find(seen, anchor) != seen.end()) continue;
				seen.push_back(anchor);
				for (const ThreeDObjectFigure &f : parts->vehicles) {
					const double x = TileX(anchor) * TILE_SIZE + f.at[0] * OBJECT_UNITS_PER_METRE, y = TileY(anchor) * TILE_SIZE + f.at[1] * OBJECT_UNITS_PER_METRE;
					const double d = std::hypot(x - this->walker.x, y - this->walker.y);
					if (d >= best_d) continue;
					const EngineID engine = YardVehicleEngine(f.figure);
					if (engine == EngineID::Invalid()) continue;
					best_d = d;
					const int yaw = static_cast<int>(std::lround(f.facing * CAB_YAW_TURN / 360.0));
					best = YardVehicle{anchor, engine, static_cast<int32_t>(std::lround(x * CAB_SUBUNIT)), static_cast<int32_t>(std::lround(y * CAB_SUBUNIT)), static_cast<uint8_t>(yaw & (CAB_YAW_TURN - 1))};
				}
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
		/* At a door: opened away from the walker, or shut. */
		if (const auto door = this->DoorToUse(); door.has_value()) {
			ToggleThreeDDoor(door->house, door->origin, door->door, this->walker.x - TileX(door->origin) * TILE_SIZE, this->walker.y - TileY(door->origin) * TILE_SIZE);
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
			if (rv == nullptr) this->Walk(FIRST_PERSON_TICK_MS / 1000.0);
			this->Hold(FIRST_PERSON_TICK_MS / 1000.0);
			this->Fly(FIRST_PERSON_TICK_MS / 1000.0);
		}
		this->SeeBlasts();
		if (rv != nullptr) {
			this->walker.x = rv->x_pos;
			this->walker.y = rv->y_pos;
			this->arrows.Drive(rv, ArrowsWindow() == this ? _dirkeys : DirectionKeys{}, static_cast<int>(steps) * FIRST_PERSON_TICK_MS);
		}
		/* No health left: the walker is gone, back to the map (the player, 15. 10.). */
		const Company *c = Company::GetIfValid(_local_company);
		if (c != nullptr && c->walker_health == 0) {
			this->Close();
			return;
		}
		/* The blows of the dragons and the animals since the last step (three_d_dragons.h, three_d_animals.h): the health
		 * through the command, the picture red a moment. */
		const uint damage = TakeThreeDDragonDamage() + TakeThreeDAnimalDamage();
		if (damage > 0 && c != nullptr) {
			Command<Commands::HurtWalker>::Post(TileIndex{}, static_cast<uint8_t>(std::min(damage, 255u)));
			this->hurt_steps = 6;
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
		this->MakePicture();
		this->hurt_steps = std::max(0, this->hurt_steps - static_cast<int>(steps));
		this->SetWidgetDirty(WID_FP_CANVAS);
	}

	FirstPersonWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
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

	/** The walker to the nearest place it can stand from a world point. */
	void PutNear(double x, double y)
	{
		/* The nearest place to stand, in rings a quarter of a tile apart. */
		for (int ring = 0; ring < 24; ring++) {
			for (int k = 0; k < std::max(1, ring * 8); k++) {
				const double a = k * 2 * M_PI / std::max(1, ring * 8), r = ring * TILE_SIZE / 4.0;
				const double px = x + r * std::cos(a), py = y + r * std::sin(a);
				if (FirstPersonWalkable(px, py)) {
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
		const double norm = std::sqrt(ahead * ahead + aside * aside);
		const double speed = ((this->running || _shift_pressed) ? FIRST_PERSON_RUN_MS : FIRST_PERSON_WALK_MS) * OBJECT_UNITS_PER_METRE * dt / norm;
		const double fx = std::cos(this->walker.heading), fy = std::sin(this->walker.heading);
		const double rx = fy, ry = -fx;
		const double dx = (fx * ahead + rx * aside) * speed, dy = (fy * ahead + ry * aside) * speed;
		/* Along a wall, sliding, and round a post (FirstPersonMove()); in pieces, so that a run does not step over a
		 * thin wall. */
		const int pieces = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy) / FIRST_PERSON_SUBSTEP)));
		FirstPersonSettle(this->walker);
		for (int i = 0; i < pieces; i++) {
			if (!FirstPersonMove(this->walker, dx / pieces, dy / pieces)) break;
			/* Up a stair, or down off it. */
			FirstPersonSettle(this->walker);
		}
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
		if (held.raise <= 0) {
			held.thing.clear();
			held.loaded.clear();
			return;
		}
		std::string_view state;
		switch (w.use) {
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
		/* In a vehicle, as in its cab: I the horn, F the lights, T its route or the wheel again. */
		if (const Vehicle *rv = this->Riding(); rv != nullptr) {
			if (key == 'i' || key == 'I') {
				CabHorn(rv);
				return EventState::Handled;
			}
			if (key == 'f' || key == 'F') {
				CabToggleLights(rv);
				return EventState::Handled;
			}
			if (key == 't' || key == 'T') {
				CabToggleRoute(rv, this->arrows);
				this->SetWidgetDirty(WID_FP_CANVAS);
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
		this->DrawWidgets();
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
				/* A dot in the middle, where it looks. */
				const int cx = r.left + w / 2, cy = r.top + h / 2;
				GfxFillRect(cx - 1, cy - 1, cx + 1, cy + 1, GetColourGradient(Colours::White, Shade::Lightest));
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
					riding = true;
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
				} else if (const StringID shop = WalkerShopName(FirstPersonShopNearby()); shop != INVALID_STRING_ID) {
					/* At a shop: I opens the inventory, where it is bought. */
					hint = GetString(STR_FIRST_PERSON_SHOP_HERE, shop);
				} else if (ThreeDMatNear(this->walker.x, this->walker.y, THREE_D_SELLER_REACH_M)) {
					/* By Mat walking about the town: I opens the inventory, where his marijuana is bought. */
					hint = GetString(STR_FIRST_PERSON_MAT_HERE);
				} else if (ThreeDDogSellerNear(this->walker.x, this->walker.y, THREE_D_SELLER_REACH_M) != nullptr) {
					/* At a seller of dogs: I opens the inventory, where its dogs are bought. */
					hint = GetString(STR_FIRST_PERSON_SELLER_HERE);
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
				/* The map shows again: the view a window. */
				if (this->full) this->ToggleFull();
				ScrollMainWindowTo(static_cast<int>(this->walker.x), static_cast<int>(this->walker.y), static_cast<int>(FirstPersonFeetZ(this->walker)));
				break;
			case WID_FP_RUN:
				this->running = !this->running;
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

/**
 * Open the view from the first person with the walker on a tile (the god's eye's crosshair, toolbar_gui.cpp): in its
 * middle, or the nearest place it can stand.
 */
void ShowFirstPersonWindowAt(TileIndex tile)
{
	ShowFirstPersonWindow();
	FirstPersonWindow *w = static_cast<FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr || w->Riding() != nullptr) return;
	w->PutNear(TileX(tile) * TILE_SIZE + TILE_SIZE / 2.0, TileY(tile) * TILE_SIZE + TILE_SIZE / 2.0);
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

/** Whether the view from the first person is open over the whole screen: the map under it is then not drawn (widget.cpp). */
bool FirstPersonCoversMap()
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	return w != nullptr && w->full;
}

/** The walker of the view from the first person: where it stands, world units; false when the view is not open (three_d_dragons.h). */
bool FirstPersonWalkerAt(double &x, double &y)
{
	const FirstPersonWindow *w = static_cast<const FirstPersonWindow *>(FindWindowById(WindowClass::FirstPerson, 0));
	if (w == nullptr) return false;
	x = w->walker.x;
	y = w->walker.y;
	return true;
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
	return fmt::format("chodec na {:.1f},{:.1f} (dlazdice {},{}), nohy {:.2f} m nad zemi, smer {:.0f}, pohled {:.0f}, {}x{} px, {:.1f} ms (sber {:.1f}, kresleni {:.1f}), {} dlazdic, {} plakatu",
			w->walker.x, w->walker.y, static_cast<int>(w->walker.x) / TILE_SIZE, static_cast<int>(w->walker.y) / TILE_SIZE,
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
					if (GetThreeDHouseObjectAt(north, origin) == nullptr) ch = 'X';
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
		w->walker.heading = std::remainder(w->walker.heading + turn * FIRST_PERSON_TURN * FIRST_PERSON_TICK_MS / 1000.0, 2 * M_PI);
		if (ahead == 0 && aside == 0) continue;
		const double speed = FIRST_PERSON_WALK_MS * OBJECT_UNITS_PER_METRE * FIRST_PERSON_TICK_MS / 1000.0;
		const double fx = std::cos(w->walker.heading), fy = std::sin(w->walker.heading);
		const double nx = w->walker.x + (fx * ahead + fy * aside) * speed, ny = w->walker.y + (fy * ahead - fx * aside) * speed;
		/* As the walker walks: its circle against the walls, round a post (FirstPersonMove()), up the stairs. */
		FirstPersonSettle(w->walker);
		if (FirstPersonMove(w->walker, nx - w->walker.x, ny - w->walker.y)) FirstPersonSettle(w->walker);
	}
	w->MakePicture();
	w->SetDirty();
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
