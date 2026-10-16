/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_navigation.cpp Finding a way over the land for the creatures (three_d_navigation.h). */

#include "stdafx.h"
#include "three_d_navigation.h"
#include "map_func.h"
#include "tile_map.h"
#include "water_map.h"
#include "slope_func.h"

#include <cmath>
#include <queue>
#include <unordered_map>

#include "safeguards.h"

/**
 * Whether a large creature (a dragon) may walk on a tile: the open land, the trees, the roads and the rails; not a
 * house, an industry, a station, an object, water, the end of a tunnel or a bridge, and not a slope steeper than
 * a level across the tile (its corners more than a level apart).
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
 * Whether the straight line between two points goes over walkable tiles only, every tile it touches (the line walked
 * through the grid, a tile after the next, as a ray through the tiles).
 */
bool NavStraight(double fx, double fy, double tx, double ty)
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
	if (!NavWalkable(prev)) return false;
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
		if (!NavStep(prev, t)) return false;
		prev = t;
	}
	return true;
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
 * The shortest way from one point to another over the walkable tiles (A*, a tile to each of its eight neighbours, a
 * corner cut only between two walkable tiles, a climb of a level costing half a tile more), within so many tiles of
 * the start; then straightened: a point of the way is left out where one can go straight past it (NavStraight()).
 * @return the points to walk through, world units, the last the goal (when the goal cannot be reached, the tile of
 *         the way nearest to it); empty when the start is not walkable or there is no way at all
 */
std::vector<NavPoint> NavFindWay(double fx, double fy, double tx, double ty, int reach_tiles)
{
	std::vector<NavPoint> way;
	if (NavStraight(fx, fy, tx, ty)) {
		way.emplace_back(tx, ty);
		return way;
	}
	const int sx = static_cast<int>(fx / TILE_SIZE), sy = static_cast<int>(fy / TILE_SIZE);
	const int gx = static_cast<int>(tx / TILE_SIZE), gy = static_cast<int>(ty / TILE_SIZE);
	if (sx < 0 || sy < 0 || sx > static_cast<int>(Map::MaxX()) || sy > static_cast<int>(Map::MaxY())) return way;
	const TileIndex start = TileXY(sx, sy);
	struct Node {
		double g;
		TileIndex from;
	};
	std::unordered_map<uint32_t, Node> seen;
	using Open = std::pair<double, uint32_t>;
	std::priority_queue<Open, std::vector<Open>, std::greater<>> open;
	auto h = [&](int x, int y) {
		const int ddx = std::abs(x - gx), ddy = std::abs(y - gy);
		return std::max(ddx, ddy) + (M_SQRT2 - 1) * std::min(ddx, ddy);
	};
	seen[start.base()] = {0, start};
	open.emplace(h(sx, sy), start.base());
	TileIndex best = start;
	double best_h = h(sx, sy);
	static const int8_t DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
	static const int8_t DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
	bool reached = false;
	for (int expanded = 0; !open.empty() && expanded < 8192; expanded++) {
		const auto [f, index] = open.top();
		open.pop();
		const TileIndex t{index};
		const int x = TileX(t), y = TileY(t);
		const Node node = seen[index];
		if (f - h(x, y) > node.g + 1e-9) continue; // a better way to it was found meanwhile
		if (x == gx && y == gy) {
			best = t;
			reached = true;
			break;
		}
		if (h(x, y) < best_h) {
			best_h = h(x, y);
			best = t;
		}
		for (int k = 0; k < 8; k++) {
			const int nx = x + DX[k], ny = y + DY[k];
			if (nx < 0 || ny < 0 || nx > static_cast<int>(Map::MaxX()) || ny > static_cast<int>(Map::MaxY())) continue;
			if (std::max(std::abs(nx - sx), std::abs(ny - sy)) > reach_tiles) continue;
			const TileIndex n = TileXY(nx, ny);
			if (!NavStep(t, n)) continue;
			/* Diagonally only past two walkable tiles: not through the corner of a house. */
			if (k >= 4 && (!NavWalkable(TileXY(x + DX[k], y)) || !NavWalkable(TileXY(x, y + DY[k])))) continue;
			const double cost = (k >= 4 ? M_SQRT2 : 1.0) + (NavHeight(t) != NavHeight(n) ? 0.5 : 0.0);
			const double g = node.g + cost;
			auto it = seen.find(n.base());
			if (it != seen.end() && it->second.g <= g) continue;
			seen[n.base()] = {g, t};
			open.emplace(g + h(nx, ny), n.base());
		}
	}
	/* Back from the end to the start: the middles of the tiles. */
	std::vector<NavPoint> tiles;
	for (TileIndex t = best; t != start; t = seen[t.base()].from) tiles.emplace_back((TileX(t) + 0.5) * TILE_SIZE, (TileY(t) + 0.5) * TILE_SIZE);
	std::reverse(tiles.begin(), tiles.end());
	if (reached) {
		if (tiles.empty()) tiles.emplace_back(tx, ty);
		else tiles.back() = {tx, ty};
	}
	if (tiles.empty()) return way;
	/* Straightened: from where it is, on to the farthest point it can go to straight. */
	NavPoint at{fx, fy};
	for (size_t i = 0; i < tiles.size();) {
		size_t far = i;
		for (size_t k = tiles.size(); k-- > i + 1;) {
			if (NavStraight(at.first, at.second, tiles[k].first, tiles[k].second)) {
				far = k;
				break;
			}
		}
		way.push_back(tiles[far]);
		at = tiles[far];
		i = far + 1;
	}
	return way;
}
