/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file climate_industries.h The original industries of the climates, switched on in any climate.
 *
 * The game settings economy.industries_temperate, _arctic, _tropic and
 * _toyland each put the original industries of one climate into every game,
 * whatever the climate played: those the climate lacks are added, with the
 * cargoes they need, and no industry set switches them off or puts one of
 * its own in their place -- a set's industries stand beside them. All off,
 * the game is as it always was.
 *
 * An industry of another climate produces the cargoes of its own climate --
 * the gold mine gold, the fruit plantation fruit -- and an industry that
 * takes one of the mixed cargoes (livestock or fruit; grain, wheat or maize;
 * valuables, gold or diamonds) takes the kinds of every climate on.
 *
 * economy.extra_industries puts in industries the game adds of its own, in
 * every climate: the marijuana plantation (IT_MARIJUANA_PLANTATION), the
 * fruit plantation of the base graphics growing marijuana (CT_MARIJUANA),
 * and the coffeeshop in towns (IT_COFFEESHOP), drawn as the desert house with
 * the palm tree, which takes it.
 */

#ifndef CLIMATE_INDUSTRIES_H
#define CLIMATE_INDUSTRIES_H

#include "cargo_type.h"
#include "industry_type.h"
#include "landscape_type.h"
#include "gfx_type.h"

LandscapeTypes IndustryClimatesOn();
bool IsOriginalIndustryKept(IndustryType type);
bool IsOriginalIndustryTileKept(IndustryGfx gfx);
LandscapeType IndustryHomeClimate(IndustryType type);
CargoLabel MixedCargoLabelFor(MixedCargoType mixed, LandscapeType climate);
std::vector<CargoLabel> CargoLabelsOfClimateIndustries();
void ResolveOriginalIndustryCargoes();
void ResolveExtraIndustryCargoes();
std::span<const CargoLabel> CoffeeshopCargoes();
uint8_t OriginalIndustryChance(IndustryType type, bool creation);

/**
 * The sprites of the original industries that a climate's own base graphics
 * file puts in place of the temperate ones, where each climate draws them its
 * own way: the forest and the cotton candy forest (and the ground of the
 * battery farm and the cola wells), the farm, the oil wells, and the desert
 * ground of the water supply and the water tower. Ground a climate draws as
 * its landscape -- bare land, grass, water -- is left out: an industry of
 * another climate stands on the land of the one played.
 *
 * Each climate's own of these is loaded into SPR_CLIMATE_INDUSTRY_BASE, and
 * an original industry of another climate is drawn with its own climate's
 * (ClimateIndustrySprite()).
 */
static constexpr std::pair<SpriteID, SpriteID> CLIMATE_INDUSTRY_SPRITE_RANGES[] = {
	{ 0x818,  0x81D},
	{ 0x83A,  0x845},
	{ 0x87D,  0x883},
	{0x11C6, 0x11C6},
};

/**
 * Where a sprite stands among CLIMATE_INDUSTRY_SPRITE_RANGES.
 * @param sprite the sprite number, without flags
 * @return its place, -1 when it is none of them
 */
inline int ClimateIndustrySpriteIndex(SpriteID sprite)
{
	int index = 0;
	for (const auto &[first, last] : CLIMATE_INDUSTRY_SPRITE_RANGES) {
		if (sprite >= first && sprite <= last) return index + static_cast<int>(sprite - first);
		index += static_cast<int>(last - first + 1);
	}
	return -1;
}

SpriteID ClimateIndustrySprite(SpriteID image, LandscapeType climate);

/** A sprite an original industry draws among CLIMATE_INDUSTRY_SPRITE_RANGES, for the rig. */
struct ClimateIndustrySpriteUse {
	IndustryType type; ///< the industry
	LandscapeType climate; ///< its own climate (IndustryHomeClimate())
	SpriteID sprite; ///< the sprite its drawing names
	SpriteID drawn; ///< the sprite drawn for it (ClimateIndustrySprite())
};
std::vector<ClimateIndustrySpriteUse> ClimateIndustrySpriteUses();

bool IsExtraClimateCargo(CargoType cargo);
void PlaceClimateIndustryCargoes();

#endif /* CLIMATE_INDUSTRIES_H */
