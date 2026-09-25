/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file green_load.cpp The loaded pictures of the marijuana wagons and lorries, see green_load.h. */

#include "stdafx.h"
#include "green_load.h"
#include "spritecache.h"

#include "table/sprites.h"

#include "safeguards.h"

/** Every loaded picture drawn green: the base set's loaded picture, and the green one made from it. */
static std::vector<std::pair<SpriteID, SpriteID>> _green_loads;

/**
 * Make the loaded pictures of the marijuana wagons and lorries, in the block
 * after SPR_GREEN_LOAD_BASE: one for every direction of every picture they
 * use. Called once the base set is loaded and before the NewGRFs, so they are
 * made from the base set's pictures.
 */
void SetupGreenLoadSprites()
{
	_green_loads.clear();
	SpriteID next = SPR_GREEN_LOAD_BASE;
	for (const auto &[type, image] : MarijuanaEngineImages()) {
		for (const auto &[empty, full] : type == VehicleType::Train ? WagonLoadPictures(image) : RoadVehicleLoadPictures(image)) {
			if (std::ranges::find(_green_loads, full, &std::pair<SpriteID, SpriteID>::first) != std::end(_green_loads)) continue;
			assert(next < SPR_GREEN_LOAD_BASE + GREEN_LOAD_SPRITE_COUNT);
			SetGreenLoadSprite(next, full, empty);
			_green_loads.emplace_back(full, next);
			next++;
		}
	}
}

/**
 * The picture to draw for something loaded with marijuana.
 * @param full the loaded picture of the base set
 * @return the same with the load green, or the picture itself where it has no green one
 */
SpriteID GreenLoadSprite(SpriteID full)
{
	auto it = std::ranges::find(_green_loads, full, &std::pair<SpriteID, SpriteID>::first);
	return it == std::end(_green_loads) ? full : it->second;
}

/**
 * Every loaded picture drawn green, for the rig.
 * @return the base set's loaded picture and the green one, pair by pair
 */
std::vector<std::pair<SpriteID, SpriteID>> GreenLoadSprites()
{
	return _green_loads;
}
