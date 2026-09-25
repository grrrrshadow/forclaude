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
 * which the desert house with the palm tree takes.
 */

#ifndef CLIMATE_INDUSTRIES_H
#define CLIMATE_INDUSTRIES_H

#include "cargo_type.h"
#include "industry_type.h"
#include "house_type.h"
#include "landscape_type.h"

LandscapeTypes IndustryClimatesOn();
bool IsOriginalIndustryKept(IndustryType type);
bool IsOriginalIndustryTileKept(IndustryGfx gfx);
LandscapeType IndustryHomeClimate(IndustryType type);
CargoLabel MixedCargoLabelFor(MixedCargoType mixed, LandscapeType climate);
std::vector<CargoLabel> CargoLabelsOfClimateIndustries();
void ResolveOriginalIndustryCargoes();
void ResolveExtraIndustryHouses();
HouseID HouseTakingMarijuana();
std::span<const CargoLabel> PalmHouseCargoes();
uint8_t OriginalIndustryChance(IndustryType type, bool creation);

bool IsExtraClimateCargo(CargoType cargo);
void PlaceClimateIndustryCargoes();

#endif /* CLIMATE_INDUSTRIES_H */
