/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file three_d_navigation.cpp Finding a way over the land for the creatures (three_d_navigation.h). */

#include "stdafx.h"
#include "three_d_navigation.h"
#include "three_d_objects.h"
#include "map_func.h"
#include "tile_map.h"
#include "water_map.h"
#include "slope_func.h"
#include "clear_map.h"

#include <cmath>
#include <numbers>
#include <queue>
#include <unordered_map>

#include "safeguards.h"

/**
 * Whether a large creature (a dragon) may walk on a tile: the open land, the trees, the forests, the roads and the
 * rails; not a house, another industry, a station, an object, water, the end of a tunnel or a bridge, and not a slope
 * steeper than a level across the tile (its corners more than a level apart).
 */
bool NavWalkable(TileIndex tile)
{
	if (!IsValidTile(tile)) return false;
	switch (GetTileType(tile)) {
		case TileType::Clear:
		case TileType::Trees:
		case TileType::Road:
		case TileType::Railway:
			break;
		case TileType::Industry:
			/* The forest's game among its trees (ThreeDForestTile()). */
			if (!ThreeDForestTile(tile)) return false;
			break;
		default:
			return false;
	}
	auto [slope, z] = GetTileSlopeZ(tile);
	return !IsSteepSlope(slope);
}

/** The height of a tile's middle, levels (for the climb between two tiles). */
static int NavHeight(TileIndex tile)
{
	return GetTileMaxZ(tile);
}

/** Whether one may step from a tile to its neighbour: both walkable and at most a level apart. */
static bool NavStep(TileIndex from, TileIndex to)
{
	return NavWalkable(to) && std::abs(NavHeight(from) - NavHeight(to)) <= 1;
}

/**
 * How dear a tile is to walk over, its length on plain grass 1 (the player, 10. 11.: "take what is good of Godot's way
 * finding": the travel cost of its regions): the roads and their pavements cheaper, so one keeps to them; the rough
 * land, the trees, the forest, across the rails and the fields dearer.
 */
double NavTileCost(TileIndex tile)
{
	switch (GetTileType(tile)) {
		case TileType::Road: return NAV_ROAD_COST;
		case TileType::Railway: return 1.5;
		case TileType::Trees: return 1.2;
		case TileType::Industry: return 1.3;
		case TileType::Clear:
			switch (GetClearGround(tile)) {
				case ClearGround::Fields: return 1.6;
				case ClearGround::Rocks: return 1.4;
				case ClearGround::Rough: return 1.2;
				default: return 1.0;
			}
		default: return 1.0;
	}
}

/** The creatures' way: the walkable tiles (NavWalkable()), a level's climb at most, eight neighbours, the costs of the land. */
const NavProfile &NavCreatureProfile()
{
	static const NavProfile profile{true, NAV_ROAD_COST, NavStep, NavWalkable, NavTileCost};
	return profile;
}

/**
 * Whether a straight line goes over tiles one may walk along (the line walked through the grid, a tile after the next,
 * as a ray through the tiles): each walkable from the one before (NavStep()), none dearer than so much.
 * @param max_cost the dearest tile it may cross; 0 any
 */
static bool NavRay(double fx, double fy, double tx, double ty, double max_cost)
{
	const double ux = fx / TILE_SIZE, uy = fy / TILE_SIZE, vx = tx / TILE_SIZE, vy = ty / TILE_SIZE;
	int cx = static_cast<int>(std::floor(ux)), cy = static_cast<int>(std::floor(uy));
	const int ex = static_cast<int>(std::floor(vx)), ey = static_cast<int>(std::floor(vy));
	const double dx = vx - ux, dy = vy - uy;
	const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
	const double tdx = dx != 0 ? std::abs(1.0 / dx) : 1e18, tdy = dy != 0 ? std::abs(1.0 / dy) : 1e18;
	double tmx = dx != 0 ? ((sx > 0 ? cx + 1 - ux : ux - cx) * tdx) : 1e18;
	double tmy = dy != 0 ? ((sy > 0 ? cy + 1 - uy : uy - cy) * tdy) : 1e18;
	if (cx < 0 || cy < 0 || cx > static_cast<int>(Map::MaxX()) || cy > static_cast<int>(Map::MaxY())) return false;
	TileIndex prev = TileXY(cx, cy);
	if (!NavWalkable(prev) || (max_cost > 0 && NavTileCost(prev) > max_cost)) return false;
	for (int guard = 0; (cx != ex || cy != ey) && guard < 4096; guard++) {
		if (tmx < tmy) {
			tmx += tdx;
			cx += sx;
		} else {
			tmy += tdy;
			cy += sy;
		}
		if (cx < 0 || cy < 0 || cx > static_cast<int>(Map::MaxX()) || cy > static_cast<int>(Map::MaxY())) return false;
		const TileIndex t = TileXY(cx, cy);
		if (!NavStep(prev, t) || (max_cost > 0 && NavTileCost(t) > max_cost)) return false;
		prev = t;
	}
	return true;
}

/**
 * Whether one can walk straight between two points keeping off what is not walkable by NAV_MARGIN (the agent's radius
 * of Godot's way finding: the way not along a house's wall, round its corner with room): the line and the two beside it
 * so far to either side over walkable tiles (NavRay()).
 * @param max_cost the dearest tile it may cross; 0 any
 */
static bool NavStraightUpTo(double fx, double fy, double tx, double ty, double max_cost)
{
	if (!NavRay(fx, fy, tx, ty, max_cost)) return false;
	const double len = std::hypot(tx - fx, ty - fy);
	if (len < 1e-6) return true;
	const double m = NAV_MARGIN_M * OBJECT_UNITS_PER_METRE, nx = -(ty - fy) / len * m, ny = (tx - fx) / len * m;
	return NavRay(fx + nx, fy + ny, tx + nx, ty + ny, max_cost) && NavRay(fx - nx, fy - ny, tx - nx, ty - ny, max_cost);
}

bool NavStraight(double fx, double fy, double tx, double ty)
{
	return NavStraightUpTo(fx, fy, tx, ty, 0);
}

/** Move a point to the middle of the nearest walkable tile within so many tiles; whether there is one. */
bool NavNearestWalkable(double &x, double &y, int reach_tiles)
{
	const int cx = static_cast<int>(x / TILE_SIZE), cy = static_cast<int>(y / TILE_SIZE);
	if (cx >= 0 && cy >= 0 && cx <= static_cast<int>(Map::MaxX()) && cy <= static_cast<int>(Map::MaxY()) && NavWalkable(TileXY(cx, cy))) return true;
	for (int r = 1; r <= reach_tiles; r++) {
		double best = 1e18;
		int bx = -1, by = -1;
		for (int ty = cy - r; ty <= cy + r; ty++) {
			for (int tx = cx - r; tx <= cx + r; tx++) {
				if (std::max(std::abs(tx - cx), std::abs(ty - cy)) != r) continue;
				if (tx < 0 || ty < 0 || tx > static_cast<int>(Map::MaxX()) || ty > static_cast<int>(Map::MaxY())) continue;
				if (!NavWalkable(TileXY(tx, ty))) continue;
				const double d = std::hypot((tx + 0.5) * TILE_SIZE - x, (ty + 0.5) * TILE_SIZE - y);
				if (d < best) {
					best = d;
					bx = tx;
					by = ty;
				}
			}
		}
		if (bx >= 0) {
			x = (bx + 0.5) * TILE_SIZE;
			y = (by + 0.5) * TILE_SIZE;
			return true;
		}
	}
	return false;
}

/**
 * The cheapest way from one tile to one of others over the tiles a profile walks (A*, to the four neighbours and with
 * NavProfile::diagonal to the eight, a corner cut only between two walkable tiles; a step costing its length times the
 * mean of the two tiles' costs, a climb half a tile more), within so many tiles of the start.
 * @param ends the tiles it may end on
 * @param[out] reached whether it got to one; else the way goes to the tile nearest the first end
 * @return the tiles of the way after the start, the last the end
 */
std::vector<TileIndex> NavFindTiles(TileIndex start, std::span<const TileIndex> ends, const NavProfile &profile, int reach_tiles, bool &reached)
{
	reached = false;
	std::vector<TileIndex> tiles;
	if (ends.empty() || !IsValidTile(start)) return tiles;
	const int sx = TileX(start), sy = TileY(start);
	const int gx = TileX(ends[0]), gy = TileY(ends[0]);
	struct Node {
		double g;
		TileIndex from;
	};
	std::unordered_map<uint32_t, Node> seen;
	using Open = std::pair<double, uint32_t>;
	std::priority_queue<Open, std::vector<Open>, std::greater<>> open;
	/* The cheapest a tile can be, so it never guesses too much: the nearest end. */
	auto h = [&](int x, int y) {
		double best = 1e18;
		for (const TileIndex e : ends) {
			const int ddx = std::abs(x - static_cast<int>(TileX(e))), ddy = std::abs(y - static_cast<int>(TileY(e)));
			best = std::min(best, profile.diagonal ? std::max(ddx, ddy) + (std::numbers::sqrt2 - 1) * std::min(ddx, ddy) : static_cast<double>(ddx + ddy));
		}
		return best * profile.min_cost;
	};
	seen[start.base()] = {0, start};
	open.emplace(h(sx, sy), start.base());
	TileIndex best = start;
	double best_h = 1e18;
	static const int8_t DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
	static const int8_t DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
	for (int expanded = 0; !open.empty() && expanded < 8192; expanded++) {
		const auto [f, index] = open.top();
		open.pop();
		const TileIndex t{index};
		const int x = TileX(t), y = TileY(t);
		const Node node = seen[index];
		if (f - h(x, y) > node.g + 1e-9) continue; // a better way to it was found meanwhile
		if (std::ranges::find(ends, t) != ends.end()) {
			best = t;
			reached = true;
			break;
		}
		const double dist = std::max(std::abs(x - gx), std::abs(y - gy));
		if (dist < best_h) {
			best_h = dist;
			best = t;
		}
		for (int k = 0; k < (profile.diagonal ? 8 : 4); k++) {
			const int nx = x + DX[k], ny = y + DY[k];
			if (nx < 0 || ny < 0 || nx > static_cast<int>(Map::MaxX()) || ny > static_cast<int>(Map::MaxY())) continue;
			if (std::max(std::abs(nx - sx), std::abs(ny - sy)) > reach_tiles) continue;
			const TileIndex n = TileXY(nx, ny);
			if (k < 4 && !profile.step(t, n)) continue;
			/* Diagonally only past two walkable tiles: not through the corner of a house. */
			if (k >= 4 && (!profile.walkable(n) || !profile.step(t, TileXY(x + DX[k], y)) || !profile.step(t, TileXY(x, y + DY[k])) ||
					std::abs(NavHeight(t) - NavHeight(n)) > 1)) continue;
			const double cost = (k >= 4 ? std::numbers::sqrt2 : 1.0) * (profile.cost(t) + profile.cost(n)) / 2 + (NavHeight(t) != NavHeight(n) ? 0.5 : 0.0);
			const double g = node.g + cost;
			auto it = seen.find(n.base());
			if (it != seen.end() && it->second.g <= g) continue;
			seen[n.base()] = {g, t};
			open.emplace(g + h(nx, ny), n.base());
		}
	}
	for (TileIndex t = best; t != start; t = seen[t.base()].from) tiles.push_back(t);
	std::reverse(tiles.begin(), tiles.end());
	return tiles;
}

/**
 * The cheapest way from one point to another over the walkable tiles (NavFindTiles() with the creatures' profile), within
 * so many tiles of the start; then straightened (the string pulled, as Godot's): a point of the way is left out where
 * one can go straight past it keeping off what stands beside (NavStraightUpTo()) over tiles no dearer than those of the
 * way it leaves out -- the pavement kept to, not a field cut across.
 * @return the points to walk through, world units, the last the goal (when the goal cannot be reached, the tile of
 *         the way nearest to it); empty when the start is not walkable or there is no way at all
 */
std::vector<NavPoint> NavFindWay(double fx, double fy, double tx, double ty, int reach_tiles)
{
	std::vector<NavPoint> way;
	const int sx = static_cast<int>(fx / TILE_SIZE), sy = static_cast<int>(fy / TILE_SIZE);
	const int gx = static_cast<int>(tx / TILE_SIZE), gy = static_cast<int>(ty / TILE_SIZE);
	if (sx < 0 || sy < 0 || sx > static_cast<int>(Map::MaxX()) || sy > static_cast<int>(Map::MaxY())) return way;
	if (gx < 0 || gy < 0 || gx > static_cast<int>(Map::MaxX()) || gy > static_cast<int>(Map::MaxY())) return way;
	const TileIndex start = TileXY(sx, sy), goal = TileXY(gx, gy);
	/* Straight there over land no dearer than where it is and where it goes. */
	if (NavWalkable(start) && NavStraightUpTo(fx, fy, tx, ty, std::max(NavTileCost(start), NavWalkable(goal) ? NavTileCost(goal) : 0.0))) {
		way.emplace_back(tx, ty);
		return way;
	}
	bool reached;
	const std::vector<TileIndex> tiles = NavFindTiles(start, {&goal, 1}, NavCreatureProfile(), reach_tiles, reached);
	if (tiles.empty()) {
		if (reached) way.emplace_back(tx, ty);
		return way;
	}
	/* The middles of the tiles, the last the goal itself when it got there; each with its cost. */
	std::vector<NavPoint> points;
	std::vector<double> costs;
	for (const TileIndex t : tiles) {
		points.emplace_back((TileX(t) + 0.5) * TILE_SIZE, (TileY(t) + 0.5) * TILE_SIZE);
		costs.push_back(NavTileCost(t));
	}
	if (reached) points.back() = {tx, ty};
	/* Straightened: from where it is, on to the farthest point it can go to straight. */
	NavPoint at{fx, fy};
	double at_cost = NavTileCost(start);
	for (size_t i = 0; i < points.size();) {
		size_t far = i;
		for (size_t k = points.size(); k-- > i + 1;) {
			/* The dearest tile of the way it would leave out, it may cross no dearer. */
			double upto = at_cost;
			for (size_t j = i; j <= k; j++) upto = std::max(upto, costs[j]);
			if (NavStraightUpTo(at.first, at.second, points[k].first, points[k].second, upto)) {
				far = k;
				break;
			}
		}
		way.push_back(points[far]);
		at = points[far];
		at_cost = costs[far];
		i = far + 1;
	}
	return way;
}

/**
 * Whether a way found is still good to go on along (instead of finding it again now and then): the next point of it
 * can still be walked to straight from where one is; else the land has changed, or one was pushed off it.
 */
bool NavWayStillGood(double x, double y, std::span<const NavPoint> way, size_t at)
{
	if (at >= way.size()) return false;
	return NavRay(x, y, way[at].first, way[at].second, 0);
}
