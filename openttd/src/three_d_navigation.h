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
 * Only the screen, as the dragons it serves: it reads the map, it changes nothing. One way finding for all the creatures
 * (the player, 10. 11.: "we do not need four way finders eating the power; take what is good of Godot's"): the costs of
 * the land, the room kept off what stands beside, the way kept while it is still good.
 */

#ifndef THREE_D_NAVIGATION_H
#define THREE_D_NAVIGATION_H

#include "tile_type.h"

#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

/** A point of a way, world units. */
using NavPoint = std::pair<double, double>;

/** The cost of a road's tile to walk over (NavTileCost()): the cheapest, so one keeps to the roads and their pavements. */
static constexpr double NAV_ROAD_COST = 0.6;
/** A way found again anyway after so many ticks, about 30 seconds (the player: "a preventive reset now and then"). */
static constexpr uint64_t NAV_WAY_RENEW_TICKS = 1000;
/** How far a way keeps off what is not walkable, metres (the agent's radius of Godot's way finding: NavStraight()). */
static constexpr double NAV_MARGIN_M = 0.6;

/** What a way may go over (NavFindTiles()): the creatures', a walker's along the pavements. */
struct NavProfile {
	bool diagonal;  ///< to the eight neighbours, else to the four
	double min_cost; ///< the cheapest tile's cost (the guess never more than the way)
	std::function<bool(TileIndex from, TileIndex to)> step; ///< whether one may step from a tile to its neighbour
	std::function<bool(TileIndex tile)> walkable;           ///< whether a tile may be stepped on (a diagonal's corners)
	std::function<double(TileIndex tile)> cost;             ///< how dear a tile is to walk over, a tile's length
};

bool NavWalkable(TileIndex tile);
double NavTileCost(TileIndex tile);
const NavProfile &NavCreatureProfile();
bool NavStraight(double fx, double fy, double tx, double ty);
std::vector<TileIndex> NavFindTiles(TileIndex start, std::span<const TileIndex> ends, const NavProfile &profile, int reach_tiles, bool &reached);
std::vector<NavPoint> NavFindWay(double fx, double fy, double tx, double ty, int reach_tiles = 48);
bool NavWayStillGood(double x, double y, std::span<const NavPoint> way, size_t at);
bool NavNearestWalkable(double &x, double &y, int reach_tiles = 4);

#endif /* THREE_D_NAVIGATION_H */
