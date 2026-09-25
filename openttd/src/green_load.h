/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file green_load.h The loaded pictures of the marijuana wagons and lorries.
 *
 * The game's own vehicles for marijuana (economy.extra_industries) are the
 * coal truck of each railtype and the three coal lorries over again. Loaded,
 * they are drawn with the coal green: each loaded picture is made again when
 * it is read, from the loaded and the empty picture of the base set, with
 * every pixel the two do not share -- the load -- in the leaf greens of the
 * palette (SetGreenLoadSprite()). Anything carrying marijuana in one of those
 * pictures is drawn with it.
 */

#ifndef GREEN_LOAD_H
#define GREEN_LOAD_H

#include "gfx_type.h"
#include "vehicle_type.h"

void SetupGreenLoadSprites();
SpriteID GreenLoadSprite(SpriteID full);
std::vector<std::pair<SpriteID, SpriteID>> GreenLoadSprites();

/* Where the pictures come from, by the vehicle tables that know them. */
std::vector<std::pair<SpriteID, SpriteID>> RoadVehicleLoadPictures(uint8_t image_index);
std::vector<std::pair<SpriteID, SpriteID>> WagonLoadPictures(uint8_t image_index);
std::vector<std::pair<VehicleType, uint8_t>> MarijuanaEngineImages();

#endif /* GREEN_LOAD_H */
