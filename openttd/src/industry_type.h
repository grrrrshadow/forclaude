/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file industry_type.h Types related to the industry. */

#ifndef INDUSTRY_TYPE_H
#define INDUSTRY_TYPE_H

#include "core/pool_type.hpp"

using IndustryID = PoolID<uint16_t, struct IndustryIDTag, 64000, 0xFFFF>;

typedef uint16_t IndustryGfx;
typedef uint8_t IndustryType;
struct Industry;

struct IndustrySpec;
struct IndustryTileSpec;

/** Available industry random triggers. */
enum class IndustryRandomTrigger : uint8_t {
	TileLoop, ///< The tile of the industry has been triggered during the tileloop.
	IndustryTick, ///< The industry has been triggered via its tick.
	CargoReceived, ///< Cargo has been delivered.
};

/** Bitset of \c IndustryRandomTrigger elements. */
using IndustryRandomTriggers = EnumBitSet<IndustryRandomTrigger, uint8_t>;

/** Animation triggers of the industries. */
enum class IndustryAnimationTrigger : uint8_t {
	ConstructionStageChanged, ///< Trigger whenever the construction stage changes.
	TileLoop, ///< Trigger in the periodic tile loop.
	IndustryTick, ///< Trigger every tick.
	CargoReceived, ///< Trigger when cargo is received .
	CargoDistributed, ///< Trigger when cargo is distributed.
};

/** Bitset of \c IndustryAnimationTrigger elements. */
using IndustryAnimationTriggers = EnumBitSet<IndustryAnimationTrigger, uint8_t>;

static const IndustryType NUM_INDUSTRYTYPES_PER_GRF = 128;            ///< maximum number of industry types per NewGRF; limited to 128 because bit 7 has a special meaning in some variables/callbacks (see MapNewGRFIndustryType).

static const IndustryType NEW_INDUSTRYOFFSET     = 37;                ///< original number of industry types
static const IndustryType NUM_INDUSTRYTYPES      = 240;               ///< total number of industry types, new and old; limited to 240 because we need some special ids like IT_INVALID, IT_AI_UNKNOWN, IT_AI_TOWN, ...
static const IndustryType IT_INVALID             = 0xFF;
/**
 * The marijuana plantation the game adds to every climate when
 * economy.extra_industries is on (climate_industries.h): the last industry
 * type. A set is given it only when the plantation is off, since the override
 * manager hands out only types that are not enabled.
 */
static const IndustryType IT_MARIJUANA_PLANTATION = NUM_INDUSTRYTYPES - 1;
/** The coffeeshop, where the marijuana goes (economy.extra_industries): the type before the plantation's, kept the same way. */
static const IndustryType IT_COFFEESHOP = NUM_INDUSTRYTYPES - 2;
/** The girls' grammar school, 2x2 in a town (economy.extra_industries): makes and takes studentky. Before the vending machine, so that map generation has schools to put machines by. */
static const IndustryType IT_GYMNASIUM = NUM_INDUSTRYTYPES - 4;
/** The vending machine by a girls' grammar school, where marijuana is sold (economy.extra_industries). */
static const IndustryType IT_WEED_MACHINE = NUM_INDUSTRYTYPES - 3;
/** The statue of Karel Macha, in a town or out of one (economy.extra_industries): takes studentky and tourists. */
static const IndustryType IT_STATUE = NUM_INDUSTRYTYPES - 5;

static const IndustryGfx  NUM_INDUSTRYTILES_PER_GRF = 255;            ///< Maximum number of industry tiles per NewGRF; limited to 255 to allow extending Action3 with an extended byte later on.

static const IndustryGfx  INDUSTRYTILE_NOANIM    = 0xFF;              ///< flag to mark industry tiles as having no animation
static const IndustryGfx  NEW_INDUSTRYTILEOFFSET = 175;               ///< original number of tiles
static const IndustryGfx  NUM_INDUSTRYTILES      = 2048;              ///< total number of industry tiles, new and old: several industry sets side by side (ECS, FIRS, Caribbean...) want more than the 512 there were, and the map has the bits (industry_map.h, m6)
static const IndustryGfx  INVALID_INDUSTRYTILE   = NUM_INDUSTRYTILES; ///< one above amount is considered invalid
/**
 * The tile of the marijuana plantation (IT_MARIJUANA_PLANTATION): the last
 * industry tile, which no set is ever given, drawn as the fruit plantation's
 * tile is in the base graphics -- a set that puts its own tile in place of
 * the fruit plantation's leaves this one as it is.
 */
static const IndustryGfx  GFX_MARIJUANA_PLANTATION = NUM_INDUSTRYTILES - 1;
/**
 * The tile of the coffeeshop (IT_COFFEESHOP): the tile before the plantation's,
 * which no set is ever given either, drawn as the desert house with the palm
 * tree (house 0x4E) is.
 */
static const IndustryGfx  GFX_COFFEESHOP = NUM_INDUSTRYTILES - 2;
/** The tiles of the girls' grammar school (IT_GYMNASIUM), north, west, east and south, and of the vending machine (IT_WEED_MACHINE): before the coffeeshop's, which no set is ever given either. */
static const IndustryGfx  GFX_GYMNASIUM_NORTH = NUM_INDUSTRYTILES - 3;
static const IndustryGfx  GFX_GYMNASIUM_WEST = NUM_INDUSTRYTILES - 4;
static const IndustryGfx  GFX_GYMNASIUM_EAST = NUM_INDUSTRYTILES - 5;
static const IndustryGfx  GFX_GYMNASIUM_SOUTH = NUM_INDUSTRYTILES - 6;
static const IndustryGfx  GFX_WEED_MACHINE = NUM_INDUSTRYTILES - 7;
/** The tile of the statue of Karel Macha (IT_STATUE). */
static const IndustryGfx  GFX_STATUE = NUM_INDUSTRYTILES - 8;
/** The first of the tiles of the game's own industries, which no set is given. */
static const IndustryGfx  GFX_GAME_OWN_FIRST = GFX_STATUE;

static const int INDUSTRY_COMPLETED = 3; ///< final stage of industry construction.

static const int INDUSTRY_NUM_INPUTS = 16;  ///< Number of cargo types an industry can accept
static const int INDUSTRY_NUM_OUTPUTS = 16; ///< Number of cargo types an industry can produce
static const int INDUSTRY_ORIGINAL_NUM_INPUTS = 3; ///< Original number of accepted cargo types.
static const int INDUSTRY_ORIGINAL_NUM_OUTPUTS = 2; ///< Original number of produced cargo types.


void CheckIndustries();

#endif /* INDUSTRY_TYPE_H */
