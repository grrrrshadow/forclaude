/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_navigation.h Finding a way over the land for the creatures (the technologies of OpenMW, the player,
 * 16. 10.: its creatures find their way round what stands in it): the tiles a large creature may walk on, and the
 * shortest way over them, straightened where it can go straight.
 *
 * Only the screen, as the dragons it serves: it reads the map, it changes nothing.
 */

#ifndef THREE_D_NAVIGATION_H
#define THREE_D_NAVIGATION_H

#include "tile_type.h"

#include <utility>
#include <vector>

/** A point of a way, world units. */
using NavPoint = std::pair<double, double>;

bool NavWalkable(TileIndex tile);
bool NavStraight(double fx, double fy, double tx, double ty);
std::vector<NavPoint> NavFindWay(double fx, double fy, double tx, double ty, int reach_tiles = 48);
bool NavNearestWalkable(double &x, double &y, int reach_tiles = 4);

#endif /* THREE_D_NAVIGATION_H */
