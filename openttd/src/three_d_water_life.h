/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file three_d_water_life.h The world under the water: the plants and the animals of the beds, the water lilies and the cattails of the rivers, the fish swimming about the walker. */

#ifndef THREE_D_WATER_LIFE_H
#define THREE_D_WATER_LIFE_H

#include "tile_type.h"

#include <string>
#include <string_view>

struct TileInfo;

void AddThreeDWaterLifeToEye(bool under_water);
void DrawThreeDRiverPlants(const TileInfo *ti);
bool SendThreeDWaterAnimal(std::string_view name, double ahead_m);
std::string DescribeThreeDWaterLife();
std::string DescribeThreeDRiverPlants(TileIndex tile);

#endif /* THREE_D_WATER_LIFE_H */
