/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file effectvehicle.cpp Implementation of everything generic to vehicles. */

#include "stdafx.h"
#include "landscape.h"
#include "core/random_func.hpp"
#include "industry_map.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "animated_tile_func.h"
#include "effectvehicle_func.h"
#include "industry.h"
#include "timer/timer_game_tick.h"
#include "console_func.h"
#include "train.h"
#include "effectvehicle_base.h"

#include "safeguards.h"


/**
 * Increment the sprite unless it has reached the end of the animation.
 * @param v Vehicle to increment sprite of.
 * @param last Last sprite of animation.
 * @return true if the sprite was incremented, false if the end was reached.
 */
static bool IncrementSprite(EffectVehicle *v, SpriteID last)
{
	if (v->sprite_cache.sprite_seq.seq[0].sprite != last) {
		v->sprite_cache.sprite_seq.seq[0].sprite++;
		return true;
	} else {
		return false;
	}
}

/** Initialise the smoke of a chimney. @copydoc EffectProcs::InitProc */
static void ChimneySmokeInit(EffectVehicle *v)
{
	uint32_t r = Random();
	v->sprite_cache.sprite_seq.Set(SPR_CHIMNEY_SMOKE_0 + GB(r, 0, 3));
	v->progress = GB(r, 16, 3);
}

/** Run a single tick of the smoke of a chimney. @copydoc EffectProcs::TickProc */
static bool ChimneySmokeTick(EffectVehicle *v)
{
	if (v->progress > 0) {
		v->progress--;
	} else {
		TileIndex tile = TileVirtXY(v->x_pos, v->y_pos);
		if (!IsTileType(tile, TileType::Industry)) {
			delete v;
			return false;
		}

		if (!IncrementSprite(v, SPR_CHIMNEY_SMOKE_7)) {
			v->sprite_cache.sprite_seq.Set(SPR_CHIMNEY_SMOKE_0);
		}
		v->progress = 7;
		v->UpdatePositionAndViewport();
	}

	return true;
}

/** Initialise the smoke of a steam engine. @copydoc EffectProcs::InitProc */
static void SteamSmokeInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_STEAM_SMOKE_0);
	v->progress = 12;
}

/** Run a single tick of the smoke of a steam engine. @copydoc EffectProcs::TickProc */
static bool SteamSmokeTick(EffectVehicle *v)
{
	bool moved = false;

	v->progress++;

	if ((v->progress & 7) == 0) {
		v->z_pos++;
		moved = true;
	}

	if ((v->progress & 0xF) == 4) {
		if (!IncrementSprite(v, SPR_STEAM_SMOKE_4)) {
			delete v;
			return false;
		}
		moved = true;
	}

	if (moved) v->UpdatePositionAndViewport();

	return true;
}

/** Initialise the smoke of a diesel engine. @copydoc EffectProcs::InitProc */
static void DieselSmokeInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_DIESEL_SMOKE_0);
	v->progress = 0;
}

/** Run a single tick of the smoke of a diesel engine. @copydoc EffectProcs::TickProc */
static bool DieselSmokeTick(EffectVehicle *v)
{
	v->progress++;

	if ((v->progress & 3) == 0) {
		v->z_pos++;
		v->UpdatePositionAndViewport();
	} else if ((v->progress & 7) == 1) {
		if (!IncrementSprite(v, SPR_DIESEL_SMOKE_5)) {
			delete v;
			return false;
		}
		v->UpdatePositionAndViewport();
	}

	return true;
}

/** Initialise the sparks of a train. @copydoc EffectProcs::InitProc */
static void ElectricSparkInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_ELECTRIC_SPARK_0);
	v->progress = 1;
}

/** Run a single tick of the sparks of a train. @copydoc EffectProcs::TickProc */
static bool ElectricSparkTick(EffectVehicle *v)
{
	if (v->progress < 2) {
		v->progress++;
	} else {
		v->progress = 0;

		if (!IncrementSprite(v, SPR_ELECTRIC_SPARK_5)) {
			delete v;
			return false;
		}
		v->UpdatePositionAndViewport();
	}

	return true;
}

/** Initialise some smoke. @copydoc EffectProcs::InitProc */
static void SmokeInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_SMOKE_0);
	v->progress = 12;
}

/** Run a single tick of some smoke. @copydoc EffectProcs::TickProc */
static bool SmokeTick(EffectVehicle *v)
{
	bool moved = false;

	v->progress++;

	if ((v->progress & 3) == 0) {
		v->z_pos++;
		moved = true;
	}

	if ((v->progress & 0xF) == 4) {
		if (!IncrementSprite(v, SPR_SMOKE_4)) {
			delete v;
			return false;
		}
		moved = true;
	}

	if (moved) v->UpdatePositionAndViewport();

	return true;
}

/** Initialise a large explosion. @copydoc EffectProcs::InitProc */
static void ExplosionLargeInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_EXPLOSION_LARGE_0);
	v->progress = 0;
}

/** Run a single tick of a large explosion. @copydoc EffectProcs::TickProc */
static bool ExplosionLargeTick(EffectVehicle *v)
{
	v->progress++;
	if ((v->progress & 3) == 0) {
		if (!IncrementSprite(v, SPR_EXPLOSION_LARGE_F)) {
			delete v;
			return false;
		}
		v->UpdatePositionAndViewport();
	}

	return true;
}

/** Initialise the smoke of a broken down vehicle. @copydoc EffectProcs::InitProc */
static void BreakdownSmokeInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_BREAKDOWN_SMOKE_0);
	v->progress = 0;
}

/** Run a single tick of the smoke of a broken down vehicle. @copydoc EffectProcs::TickProc */
static bool BreakdownSmokeTick(EffectVehicle *v)
{
	v->progress++;
	if ((v->progress & 7) == 0) {
		if (!IncrementSprite(v, SPR_BREAKDOWN_SMOKE_3)) {
			v->sprite_cache.sprite_seq.Set(SPR_BREAKDOWN_SMOKE_0);
		}
		v->UpdatePositionAndViewport();
	}

	v->animation_state--;
	if (v->animation_state == 0) {
		delete v;
		return false;
	}

	return true;
}

/** Initialise a small explosion. @copydoc EffectProcs::InitProc */
static void ExplosionSmallInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_EXPLOSION_SMALL_0);
	v->progress = 0;
}

/** Run a single tick of a small explosion. @copydoc EffectProcs::TickProc */
static bool ExplosionSmallTick(EffectVehicle *v)
{
	v->progress++;
	if ((v->progress & 3) == 0) {
		if (!IncrementSprite(v, SPR_EXPLOSION_SMALL_B)) {
			delete v;
			return false;
		}
		v->UpdatePositionAndViewport();
	}

	return true;
}

/** Initialise the bulldozer (road works). @copydoc EffectProcs::InitProc */
static void BulldozerInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_BULLDOZER_NE);
	v->progress = 0;
	v->animation_state = 0;
	v->animation_substate = 0;
}

struct BulldozerMovement {
	uint8_t direction:2;
	uint8_t image:2;
	uint8_t duration:3;
};

static const BulldozerMovement _bulldozer_movement[] = {
	{ 0, 0, 4 },
	{ 3, 3, 4 },
	{ 2, 2, 7 },
	{ 0, 2, 7 },
	{ 1, 1, 3 },
	{ 2, 2, 7 },
	{ 0, 2, 7 },
	{ 1, 1, 3 },
	{ 2, 2, 7 },
	{ 0, 2, 7 },
	{ 3, 3, 6 },
	{ 2, 2, 6 },
	{ 1, 1, 7 },
	{ 3, 1, 7 },
	{ 0, 0, 3 },
	{ 1, 1, 7 },
	{ 3, 1, 7 },
	{ 0, 0, 3 },
	{ 1, 1, 7 },
	{ 3, 1, 7 }
};

static const Coord2D<int8_t> _inc_by_dir[] = {
	{ -1,  0 },
	{  0,  1 },
	{  1,  0 },
	{  0, -1 }
};

/** Run a single tick of a bulldozer (road works). @copydoc EffectProcs::TickProc */
static bool BulldozerTick(EffectVehicle *v)
{
	v->progress++;
	if ((v->progress & 7) == 0) {
		const BulldozerMovement *b = &_bulldozer_movement[v->animation_state];

		v->sprite_cache.sprite_seq.Set(SPR_BULLDOZER_NE + b->image);

		v->x_pos += _inc_by_dir[b->direction].x;
		v->y_pos += _inc_by_dir[b->direction].y;

		v->animation_substate++;
		if (v->animation_substate >= b->duration) {
			v->animation_substate = 0;
			v->animation_state++;
			if (v->animation_state == lengthof(_bulldozer_movement)) {
				delete v;
				return false;
			}
		}
		v->UpdatePositionAndViewport();
	}

	return true;
}

/** Initialise the bubbles of the bubble generator industry. @copydoc EffectProcs::InitProc */
static void BubbleInit(EffectVehicle *v)
{
	v->sprite_cache.sprite_seq.Set(SPR_BUBBLE_GENERATE_0);
	v->spritenum = 0;
	v->progress = 0;
}

struct BubbleMovement {
	int8_t x:4;
	int8_t y:4;
	int8_t z:4;
	uint8_t image:4;
};

#define MK(x, y, z, i) { x, y, z, i }
#define ME(i) { i, 4, 0, 0 }

static const BubbleMovement _bubble_float_sw[] = {
	MK(0, 0, 1, 0),
	MK(1, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(1, 0, 1, 2),
	ME(1)
};


static const BubbleMovement _bubble_float_ne[] = {
	MK( 0, 0, 1, 0),
	MK(-1, 0, 1, 1),
	MK( 0, 0, 1, 0),
	MK(-1, 0, 1, 2),
	ME(1)
};

static const BubbleMovement _bubble_float_se[] = {
	MK(0, 0, 1, 0),
	MK(0, 1, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 1, 1, 2),
	ME(1)
};

static const BubbleMovement _bubble_float_nw[] = {
	MK(0,  0, 1, 0),
	MK(0, -1, 1, 1),
	MK(0,  0, 1, 0),
	MK(0, -1, 1, 2),
	ME(1)
};

static const BubbleMovement _bubble_burst[] = {
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 7),
	MK(0, 0, 1, 8),
	MK(0, 0, 1, 9),
	ME(0)
};

static const BubbleMovement _bubble_absorb[] = {
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(0, 0, 1, 1),
	MK(2, 1, 3, 0),
	MK(1, 1, 3, 1),
	MK(2, 1, 3, 0),
	MK(1, 1, 3, 2),
	MK(2, 1, 3, 0),
	MK(1, 1, 3, 1),
	MK(2, 1, 3, 0),
	MK(1, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(1, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(1, 0, 1, 2),
	MK(0, 0, 1, 0),
	MK(1, 0, 1, 1),
	MK(0, 0, 1, 0),
	MK(1, 0, 1, 2),
	ME(2),
	MK(0, 0, 0, 0xA),
	MK(0, 0, 0, 0xB),
	MK(0, 0, 0, 0xC),
	MK(0, 0, 0, 0xD),
	MK(0, 0, 0, 0xE),
	ME(0)
};
#undef ME
#undef MK

static const BubbleMovement * const _bubble_movement[] = {
	_bubble_float_sw,
	_bubble_float_ne,
	_bubble_float_se,
	_bubble_float_nw,
	_bubble_burst,
	_bubble_absorb,
};

/** Run a single tick of bubbles of the bubble generator industry. @copydoc EffectProcs::TickProc */
static bool BubbleTick(EffectVehicle *v)
{
	uint anim_state;

	v->progress++;
	if ((v->progress & 3) != 0) return true;

	if (v->spritenum == 0) {
		v->sprite_cache.sprite_seq.seq[0].sprite++;
		if (v->sprite_cache.sprite_seq.seq[0].sprite < SPR_BUBBLE_GENERATE_3) {
			v->UpdatePositionAndViewport();
			return true;
		}
		if (v->animation_substate != 0) {
			v->spritenum = GB(Random(), 0, 2) + 1;
		} else {
			v->spritenum = 6;
		}
		anim_state = 0;
	} else {
		anim_state = v->animation_state + 1;
	}

	const BubbleMovement *b = &_bubble_movement[v->spritenum - 1][anim_state];

	if (b->y == 4 && b->x == 0) {
		delete v;
		return false;
	}

	if (b->y == 4 && b->x == 1) {
		if (v->z_pos > 180 || Chance16I(1, 96, Random())) {
			v->spritenum = 5;
			if (_settings_client.sound.ambient) SndPlayVehicleFx(SND_2F_BUBBLE_GENERATOR_FAIL, v);
		}
		anim_state = 0;
	}

	if (b->y == 4 && b->x == 2) {
		TileIndex tile;

		anim_state++;
		if (_settings_client.sound.ambient) SndPlayVehicleFx(SND_31_BUBBLE_GENERATOR_SUCCESS, v);

		tile = TileVirtXY(v->x_pos, v->y_pos);
		if (IsTileType(tile, TileType::Industry) && GetIndustryGfx(tile) == GFX_BUBBLE_CATCHER) AddAnimatedTile(tile);
	}

	v->animation_state = anim_state;
	b = &_bubble_movement[v->spritenum - 1][anim_state];

	v->x_pos += b->x;
	v->y_pos += b->y;
	v->z_pos += b->z;
	v->sprite_cache.sprite_seq.Set(SPR_BUBBLE_0 + b->image);

	v->UpdatePositionAndViewport();

	return true;
}

/** Container holding functions to call for a specific effect vehicle type. */
struct EffectProcs {
	/**
	 * Initialises effect vehicle for a specific type.
	 * @param v The vehicle to initialise.
	 */
	using InitProc = void(EffectVehicle *v);

	/**
	 * Run the actions/perform the behaviour of an effect vehicle for a tick.
	 * @param v The vehicle to work with.
	 * @return \c true iff the vehicle is still valid, i.e. has not been removed yet.
	 */
	using TickProc = bool(EffectVehicle *v);

	InitProc *init_proc; ///< Function to initialise an effect vehicle after construction.
	TickProc *tick_proc; ///< Functions for controlling effect vehicles at each tick.
	TransparencyOption transparency; ///< Transparency option affecting the effect.

	constexpr EffectProcs(InitProc *init_proc, TickProc *tick_proc, TransparencyOption transparency)
		: init_proc(init_proc), tick_proc(tick_proc), transparency(transparency) {}
};

/**
 * Where one step of each heading takes a rocket, in pixels.
 *
 * The same table GetNewVehiclePos() moves everything else by. A rocket
 * flies along these and nothing else, so whatever heading it is drawn in is
 * the heading it is actually travelling.
 */
static constexpr DirectionIndexArray<Coord2D<int8_t>> _raid_rocket_delta{{{
	{-1, -1}, // N
	{-1,  0}, // NE
	{-1,  1}, // E
	{ 0,  1}, // SE
	{ 1,  1}, // S
	{ 1,  0}, // SW
	{ 1, -1}, // W
	{ 0, -1}, // NW
}}};

/**
 * Which way a rocket points the moment it is fired.
 *
 * Only for that moment. Every tick after it, GetDirectionTowards() turns it
 * a step of the eight at a time, the way an aircraft comes round onto its
 * heading -- but that reads the heading the vehicle already has, and a
 * thing that has only just been made has not got one.
 *
 * So this is the game's own nine-way table, copied, and it has to stay that
 * table: the rocket flies along _raid_rocket_delta and nothing else, so the
 * heading it is given must be one of those eight and not the nearest angle
 * to some line drawn on the map. Aiming by angle and moving by heading is
 * what put a sprite pointing up-left over something flying flat west.
 */
static Direction RaidRocketHeading(int from_x, int from_y, int to_x, int to_y)
{
	static const Direction table[] = {
		Direction::N,  Direction::NW, Direction::W,
		Direction::NE, Direction::SE, Direction::SW,
		Direction::E,  Direction::SE, Direction::S,
	};
	int i = 0;
	if (to_y >= from_y) {
		if (to_y != from_y) i += 3;
		i += 3;
	}
	if (to_x >= from_x) {
		if (to_x != from_x) i++;
		i++;
	}
	return table[i];
}

/**
 * How fast a raid rocket flies, in the game's own speed unit.
 *
 * That unit is what an aircraft's window shows before the game converts it
 * for the reader: one unit is one mile an hour, so a speed in km/h divided
 * by 1.609344 gives it. A vehicle then covers speed/256 pixels in a tick,
 * the same arithmetic UpdateAircraftSpeed() does, so a rocket that says 420
 * moves like anything else that says 420.
 *
 * A diagonal step covers half as much again as a straight one, which is why
 * the game's own aircraft are faster across the map than up it. A rocket
 * takes its diagonal steps that much more slowly instead, so it flies 420
 * whichever way it is pointing.
 *
 * 420 km/h: 420 / 1.609344 = 261.
 */
static const uint16_t RAID_ROCKET_SPEED = 261;
/** One over the root of two, in 256ths: what a diagonal step is worth. */
static const int RAID_ROCKET_DIAGONAL = 181;
/** How near the spot counts as arrived, in pixels. */
static const int RAID_ROCKET_ARRIVED = 8;
/**
 * How high over the ground a rocket flies once it is up, in pixels.
 *
 * Half what the game's aircraft keep to. High enough to pass over the
 * rooftops it used to go through, low enough that it still reads as
 * something shot from a deck rather than an airliner on its way somewhere.
 */
static const int RAID_ROCKET_CRUISE = 56;
/** How far from the spot it starts down, in pixels: three tiles. */
static const int RAID_ROCKET_DESCENT = 3 * TILE_SIZE;
/** How fast it climbs, in pixels a tick: three tiles' flying to get up. */
static const int RAID_ROCKET_CLIMB_RATE = 1;
/** How fast it may come down, in pixels a tick, to hold the line down. */
static const int RAID_ROCKET_DIVE_RATE = 2;
/**
 * How many ticks a rocket may live.
 *
 * Long enough for the whole of a ship's reach at the speed above -- fifty
 * tiles is eight hundred pixels and it makes about one a tick -- and then
 * some. Nothing should ever need it; a rocket that somehow cannot arrive is
 * better gone than flying for the rest of the game.
 */
static const uint16_t RAID_ROCKET_FUSE = 2000;

/** Set up a raid rocket. @copydoc EffectInitProc */
static void RaidRocketInit(EffectVehicle *v)
{
	v->animation_state = RAID_ROCKET_FUSE;
	v->x_frac = 0;
	v->y_frac = 0;
	v->UpdateSpriteSeq();
}

/**
 * Fly a raid rocket, and let it off when it gets there.
 *
 * Straight at the spot, in a line, the way a rocket goes. The smoke it
 * leaves behind is the raid itself, dropped when it arrives -- so the ship
 * fires and the damage happens where the rocket lands, not where the ship
 * is standing.
 * @copydoc EffectTickProc
 */
static bool RaidRocketTick(EffectVehicle *v)
{
	int tx = TileX(v->dest_tile) * TILE_SIZE + TILE_SIZE / 2;
	int ty = TileY(v->dest_tile) * TILE_SIZE + TILE_SIZE / 2;
	int dx = tx - v->x_pos;
	int dy = ty - v->y_pos;

	bool spent = v->animation_state == 0 || --v->animation_state == 0;
	if (spent || (abs(dx) + abs(dy)) <= RAID_ROCKET_ARRIVED) {
		/* Here. The carpet lies the way the rocket flew, the same as the one
		 * an aircraft drops on its run. */
		/* The smoke first, the rocket second. Dropping a carpet makes
		 * fifteen new effect vehicles, and doing that with this one's slot
		 * already given back hands one of them the slot the tick loop is
		 * standing on. */
		if (_show_train_orientation) {
			IConsolePrint(CC_INFO, "raketa: dopadla na ({},{}), tik {}", v->x_pos, v->y_pos, TimerGameTick::counter);
		}
		DropRaidSmoke(v->dest_tile, v->direction, v->owner);
		delete v;
		return false;
	}

	/* Turn towards the spot, a step of the eight at a time, and then fly the
	 * way it is now pointing -- the same two moves an aircraft makes, and
	 * for the same reason: there are eight sprites, so those are the eight
	 * ways it can be seen to fly. A guided missile turning onto its heading
	 * and holding it is what the player asked for, and a heading it has a
	 * picture of is the only kind worth flying.
	 *
	 * The step is worked out in 256ths of a pixel and what does not make a
	 * whole pixel this tick is kept for the next, so any speed can be asked
	 * for and a slow one does not round down to standing still. */
	v->direction = GetDirectionTowards(v, tx, ty);
	const Coord2D<int8_t> &delta = _raid_rocket_delta[v->direction];
	const int rate = (delta.x != 0 && delta.y != 0)
			? RAID_ROCKET_SPEED * RAID_ROCKET_DIAGONAL / 256 : RAID_ROCKET_SPEED;
	const int step_x = v->x_frac + delta.x * rate;
	const int step_y = v->y_frac + delta.y * rate;
	/* Shifting a negative rounds it down and the mask leaves what is left
	 * over, so a rocket going backwards up an axis keeps its fraction the
	 * same way one going forwards does. */
	v->x_pos += step_x >> 8;
	v->y_pos += step_y >> 8;
	v->x_frac = (uint8_t)(step_x & 0xFF);
	v->y_frac = (uint8_t)(step_y & 0xFF);

	/* Up off the deck, along over the rooftops, and down onto the spot over
	 * the last three tiles. The height wanted is worked out from where it is
	 * rather than remembered, so nothing has to be carried from tick to
	 * tick: near the end it is the line down to the ground at the target,
	 * and everywhere else it is the cruise over whatever is underneath. */
	const int left = abs(tx - v->x_pos) + abs(ty - v->y_pos);
	int want;
	if (left <= RAID_ROCKET_DESCENT) {
		want = GetSlopePixelZ(tx, ty, false) + RAID_ROCKET_CRUISE * left / RAID_ROCKET_DESCENT;
	} else {
		want = GetSlopePixelZ(v->x_pos, v->y_pos, false) + RAID_ROCKET_CRUISE;
	}
	v->z_pos += Clamp(want - v->z_pos, -RAID_ROCKET_DIVE_RATE, RAID_ROCKET_CLIMB_RATE);

	v->UpdateSpriteSeq();
	v->UpdatePositionAndViewport();
	return true;
}

/** Per-EffectVehicleType handling. */
static const std::array<EffectProcs, EV_END> _effect_procs = {{
	{ ChimneySmokeInit,   ChimneySmokeTick,   TransparencyOption::Industries }, // EV_CHIMNEY_SMOKE
	{ SteamSmokeInit,     SteamSmokeTick,     TransparencyOption::Invalid    }, // EV_STEAM_SMOKE
	{ DieselSmokeInit,    DieselSmokeTick,    TransparencyOption::Invalid    }, // EV_DIESEL_SMOKE
	{ ElectricSparkInit,  ElectricSparkTick,  TransparencyOption::Invalid    }, // EV_ELECTRIC_SPARK
	{ SmokeInit,          SmokeTick,          TransparencyOption::Invalid    }, // EV_CRASH_SMOKE
	{ ExplosionLargeInit, ExplosionLargeTick, TransparencyOption::Invalid    }, // EV_EXPLOSION_LARGE
	{ BreakdownSmokeInit, BreakdownSmokeTick, TransparencyOption::Invalid    }, // EV_BREAKDOWN_SMOKE
	{ ExplosionSmallInit, ExplosionSmallTick, TransparencyOption::Invalid    }, // EV_EXPLOSION_SMALL
	{ BulldozerInit,      BulldozerTick,      TransparencyOption::Invalid    }, // EV_BULLDOZER
	{ BubbleInit,         BubbleTick,         TransparencyOption::Industries }, // EV_BUBBLE
	{ SmokeInit,          SmokeTick,          TransparencyOption::Invalid    }, // EV_BREAKDOWN_SMOKE_AIRCRAFT
	{ SmokeInit,          SmokeTick,          TransparencyOption::Industries }, // EV_COPPER_MINE_SMOKE
	{ RaidRocketInit,     RaidRocketTick,     TransparencyOption::Invalid    }, // EV_RAID_ROCKET
}};

/**
 * Create an effect vehicle at a particular location.
 * @param x The x location on the map.
 * @param y The y location on the map.
 * @param z The z location on the map.
 * @param type The type of effect vehicle.
 * @return The effect vehicle.
 */
EffectVehicle *CreateEffectVehicle(int x, int y, int z, EffectVehicleType type)
{
	if (!Vehicle::CanAllocateItem()) return nullptr;

	EffectVehicle *v = EffectVehicle::Create();
	v->subtype = type;
	v->x_pos = x;
	v->y_pos = y;
	v->z_pos = z;
	v->tile = TileIndex{};
	v->UpdateDeltaXY();
	v->vehstatus = VehState::Unclickable;

	_effect_procs[type].init_proc(v);

	v->UpdatePositionAndViewport();

	return v;
}


/**
 * Fire a raid rocket at a spot.
 *
 * The livery is drawn at the moment of firing and is nothing but looks:
 * yellow or grey, as the artwork has it, one or the other each time.
 *
 * @param x      where it starts, in pixels
 * @param y      where it starts, in pixels
 * @param z      how high it starts
 * @param target the tile it is aimed at
 * @param who    whose raid this is, for the papers
 * @return whether one could be made at all
 */
bool FireRaidRocket(int x, int y, int z, TileIndex target, Owner who)
{
	EffectVehicle *v = CreateEffectVehicle(x, y, z, EV_RAID_ROCKET);
	if (v == nullptr) return false;

	v->dest_tile = target;
	v->owner = who;
	v->animation_substate = GB(Random(), 0, 1);
	v->direction = RaidRocketHeading(x, y, TileX(target) * TILE_SIZE + TILE_SIZE / 2,
			TileY(target) * TILE_SIZE + TILE_SIZE / 2);
	v->UpdateSpriteSeq();
	v->UpdatePositionAndViewport();
	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "raketa: vypustena na ({},{}), tik {}", x, y, TimerGameTick::counter);
	}
	return true;
}

/**
 * Create an effect vehicle above a particular location.
 * @param x The x location on the map.
 * @param y The y location on the map.
 * @param z The offset from the ground.
 * @param type The type of effect vehicle.
 * @return The effect vehicle.
 */
EffectVehicle *CreateEffectVehicleAbove(int x, int y, int z, EffectVehicleType type)
{
	int safe_x = Clamp(x, 0, Map::MaxX() * TILE_SIZE);
	int safe_y = Clamp(y, 0, Map::MaxY() * TILE_SIZE);
	return CreateEffectVehicle(x, y, GetSlopePixelZ(safe_x, safe_y) + z, type);
}

/**
 * Create an effect vehicle above a particular vehicle.
 * @param v The vehicle to base the position on.
 * @param x The x offset to the vehicle.
 * @param y The y offset to the vehicle.
 * @param z The z offset to the vehicle.
 * @param type The type of effect vehicle.
 * @return The effect vehicle.
 */
EffectVehicle *CreateEffectVehicleRel(const Vehicle *v, int x, int y, int z, EffectVehicleType type)
{
	return CreateEffectVehicle(v->x_pos + x, v->y_pos + y, v->z_pos + z, type);
}

bool EffectVehicle::Tick()
{
	return _effect_procs[this->subtype].tick_proc(this);
}

/**
 * Put the right picture on a raid rocket.
 *
 * Eight headings and two liveries; the livery was picked when it was fired
 * and the heading changes as it flies.
 */
void EffectVehicle::UpdateSpriteSeq()
{
	if (this->subtype != EV_RAID_ROCKET) return;
	SpriteID base = this->animation_substate == 0 ? SPR_RAID_ROCKET_GREY : SPR_RAID_ROCKET_YELLOW;
	this->sprite_cache.sprite_seq.Set(base + to_underlying(this->direction));
}

void EffectVehicle::UpdateDeltaXY()
{
	this->bounds = {{}, {1, 1, 1}, {}};
}

/**
 * Determines the transparency option affecting the effect.
 * @return Transparency option, or TransparencyOption::Invalid if none.
 */
TransparencyOption EffectVehicle::GetTransparencyOption() const
{
	return _effect_procs[this->subtype].transparency;
}
