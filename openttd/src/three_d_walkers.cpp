/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_walkers.cpp People walking about the towns in the turned view: Mat the repairman, from the door of a
 * house along the pavements of the town's roads to the door of another, where he goes in.
 *
 * Each town near the middle of the turned main view gets its Mat now and then. He comes out of a door of one of its
 * houses that are 3D objects with doors ("dvere"), the door looking out onto a road of the town, and goes by the
 * town's roads (BFS over their tiles, both ends of each step joined) to the door of another house, keeping to the
 * pavement on one side of the road: PAVEMENT from the middle of the road, crossing where the door he goes to is on
 * the other side. His frames are by the way walked (the colleague's "krok_m"), so his feet do not slide; he walks
 * as the frames walk, a round of them every snimku * tiku ticks.
 *
 * And he visits the depots and the lighthouses (the player, 10. 11.: "the lighthouse is in a lonely place, no other
 * doors about; a depot may be the same: Mat appears about, walks to the door, goes in -- disappears, after a while
 * comes out, walks off a bit and disappears there"): his way there and away the creatures' (NavFindWay()), as his way
 * along the pavements is found by the same finder (NavFindTiles()).
 */

#include "stdafx.h"
#include "three_d_walkers.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"
#include "three_d_navigation.h"
#include "object_map.h"
#include "object_type.h"
#include "rail_map.h"
#include "water_map.h"
#include "town.h"
#include "house.h"
#include "road.h"
#include "road_map.h"
#include "road_func.h"
#include "map_func.h"
#include "landscape.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "core/random_func.hpp"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "settings_type.h"
#include "transparency.h"
#include "debug.h"
#include "gfx_type.h"

#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

#include "safeguards.h"

bool FirstPersonWalkerAt(double &x, double &y);

namespace {

/** The figure that walks. */
constexpr std::string_view WALKER_FIGURE = "mat";
/** The pavement, from the middle line of the road, world units (a tile is 16): along the edge of the road tile. */
constexpr double PAVEMENT = 5.5;
/** Towns this far from the middle of the view (tiles, Manhattan) get their walker. */
constexpr uint TOWN_REACH = 40;
/** A walker whose town gets this far from the middle of the view goes. */
constexpr uint TOWN_GONE = 64;
/** The houses of a town looked at for doors: this far from its middle (tiles, each way). */
constexpr int DOOR_SEARCH = 20;
/**
 * How much faster than the frames' own pace it walks (the colleague's "tiku"): at that pace a tile takes some
 * fifteen seconds, true to the vehicles but slow to watch; faster, the legs swing faster, the feet still do not slide.
 */
constexpr double WALK_PACE = 2.0;
/** How far about a door (tiles, each way) the road to it may be when it does not look out onto one: over a fence. */
constexpr int THROUGH_REACH = 3;
/** How far the way to a door is looked for (tiles each way from where he comes out). */
constexpr int ROUTE_REACH = 64;
/** The depots and lighthouses this far from the middle of the view (tiles, each way) get his visits. */
constexpr int VISIT_REACH = 16;
/** He appears, and goes off to, between so many tiles from the door he visits. */
constexpr int VISIT_FROM_MIN = 3, VISIT_FROM_MAX = 7;
/** He is inside so long (ticks): from the first, then up to the second more. */
constexpr uint64_t VISIT_INSIDE = 300, VISIT_INSIDE_MORE = 600;
/** One visit at a time among the depots and lighthouses within so many tiles (a row of depots: one Mat, not one each). */
constexpr uint VISIT_APART = 8;
/** At most so many visits at once about the view. */
constexpr size_t VISITS_AT_ONCE = 3;
/** He visits the same door again after so long (ticks): from the first, then up to the second more. */
constexpr uint64_t VISIT_AGAIN = 900, VISIT_AGAIN_MORE = 1800;
/** How far before a door he steps to it from (metres). */
constexpr double DOOR_FRONT_M = 1.5;
/**
 * And how far out of the door the creatures' way comes to (world units): on the tile beyond, the door's front being on
 * the depot's or lighthouse's own tile, which no way goes over (NavWalkable()).
 */
constexpr double DOOR_APPROACH = TILE_SIZE * 0.75;

struct Point2 {
	double x, y;
};

/** A door of a house looking out onto a road of the town, and the way from it to the pavement. */
struct Door {
	TileIndex house; ///< the north tile of the house
	TileIndex road;  ///< the road tile outside it
	Point2 door;     ///< where it is, world units
	Point2 edge;     ///< where the way out of it leaves the plot
	Point2 pavement; ///< on the pavement of the road tile in front of it
};

/** One walking. */
struct Walker {
	TownID town;
	std::vector<Point2> path;    ///< the way, world units
	std::vector<double> at;      ///< how far along the way each point of it is
	double walked = 0;           ///< how far it has walked
	TileIndex tile = INVALID_TILE; ///< the tile it is on
	int frame = -1;              ///< the frame it is drawn in
	double x = 0, y = 0;
	int z = 0;
	double zf = 0;     ///< the ground there on its slope's plane, not the game's whole steps (GroundZF()): where the eyes see it stand
	float facing = 0;            ///< degrees, 0 along +x, 90 along +y
	/* Where it was last drawn again on the map (MarkStandingDirty()), at its last frame: the map draws it there, as the
	 * screen has it, till its next frame draws it again there and where it is then. */
	double drawn_x = 0, drawn_y = 0;
	int drawn_z = 0;
	float drawn_facing = 0;
	/* A visit to a depot or a lighthouse (SendVisit()), not a town's walk. */
	TileIndex visit = INVALID_TILE; ///< the depot's or lighthouse's north tile, INVALID_TILE a town's walk
	int stage = 0;                  ///< 0 to the door, 1 inside (not drawn), 2 off from it
	uint64_t until = 0;             ///< inside till then
	Point2 door{0, 0}, front{0, 0}; ///< the door and where he steps to it from
	Point2 approach{0, 0};          ///< and where the way comes to it, on the walkable tile beyond the door's front
};

std::vector<Walker> _walkers;
/** When a depot or lighthouse may get his visit again (the game's tick counter), by its north tile. */
std::map<TileIndex, uint64_t> _next_visit;
/** When a town's walker may come out again (the game's tick counter). */
std::map<TownID, uint64_t> _next_walk;

/** The figure walking, if it is there and walks. */
const ThreeDVehicleObject *WalkerFigure()
{
	const ThreeDVehicleObject *fig = GetThreeDFigure(WALKER_FIGURE);
	return fig != nullptr && fig->walk_step > 0 && fig->frames > 1 ? fig : nullptr;
}

/** The walker's size: as the vehicles of 3D objects, the size of the original game or CZTR's. */
float WalkerScale()
{
	return _settings_game.vehicle.object_original_size ? 1.2f : 1.0f;
}

/** How tall a thing of the game's own is drawn on the map, units of height: bigger than it is (WalkerScale()). */
int DrawnTall(const ThreeDVehicleObject *o)
{
	const ThreeDVehicleShape *shape = o != nullptr ? o->GetShape(0) : nullptr;
	if (shape == nullptr) return MAX_BUILDING_PIXELS;
	return static_cast<int>(std::ceil(shape->z1 * WalkerScale() * OBJECT_HEIGHT_PER_METRE));
}

/**
 * A thing of the game's own (a walker, a car) drawn again where it stands: as far as its shape reaches about its point
 * whichever way it faces (drawn bigger, WalkerScale()), from a little below its feet to above its head -- as a figure
 * that moves is (NoteMovingFigure()). On the turned map not its tile and the half tile or the tile about it: the card
 * laid the buildings there again at every step it made (the player's #326: Mat and the cars about a town, the map 13
 * to 25 frames a second). A car's lights at night reach further: two tiles about it.
 * @param o its object
 * @param x,y,z where it stands, world units
 * @param lit its lights on
 */
void MarkStandingDirty(const ThreeDVehicleObject *o, double x, double y, int z, bool lit)
{
	const ThreeDVehicleShape *shape = o != nullptr ? o->GetShape(0) : nullptr;
	double reach = 2.0 * TILE_SIZE;
	if (shape != nullptr) {
		const double rx = std::max(std::abs(shape->x0), std::abs(shape->x1)), ry = std::max(std::abs(shape->y0), std::abs(shape->y1));
		reach = std::hypot(rx, ry) * WalkerScale() * OBJECT_UNITS_PER_METRE + 2;
		if (lit) reach = std::max(reach, 2.0 * TILE_SIZE);
	}
	MarkWorldBoxDirty(x - reach, y - reach, x + reach, y + reach, z - TILE_HEIGHT, z + DrawnTall(o) + TILE_HEIGHT, true);
}

/** World units a round of the frames walks. */
double RoundLength(const ThreeDVehicleObject *fig)
{
	return fig->walk_step * WalkerScale() * OBJECT_UNITS_PER_METRE;
}

/**
 * A road of the town to walk on: a plain road tile (not a crossing, a stop or a depot) within the town (its
 * outermost zone; past it is the country), with a pavement -- one the game paves, or of a road type of the
 * towns (the player: "that is another road, the town's": CZTR's town roads draw their own pavements).
 */
bool IsWalkway(TileIndex t, const Town *town)
{
	if (!IsValidTile(t) || !IsNormalRoadTile(t) || !HasTileRoadType(t, RoadTramType::Road)) return false;
	if (GetRoadBits(t, RoadTramType::Road).None()) return false;
	if (DistanceSquare(t, town->xy) >= town->cache.squared_town_zone_radius[to_underlying(HouseZone::TownEdge)]) return false;
	switch (GetRoadside(t)) {
		case Roadside::Paved:
		case Roadside::StreetLights:
		case Roadside::Trees:
		case Roadside::PavedRoadWorks:
			return true;
		default:
			break;
	}
	return GetRoadTypeInfo(GetRoadTypeRoad(t))->flags.Test(RoadTypeFlag::TownBuild);
}

/** The tile next to a tile, or INVALID_TILE past the map's edge. */
TileIndex Next(TileIndex t, DiagDirection d)
{
	return AddTileIndexDiffCWrap(t, TileIndexDiffCByDiagDir(d));
}

/** Whether the road of a tile goes on into the next one that way, and the next one's comes back. */
bool Joined(TileIndex a, TileIndex b, DiagDirection d)
{
	return GetRoadBits(a, RoadTramType::Road).Any(DiagDirToRoadBits(d)) && GetRoadBits(b, RoadTramType::Road).Any(DiagDirToRoadBits(ReverseDiagDir(d)));
}

/** The middle of a tile, world units. */
Point2 Middle(TileIndex t)
{
	return {TileX(t) * TILE_SIZE + TILE_SIZE / 2.0, TileY(t) * TILE_SIZE + TILE_SIZE / 2.0};
}

/** The 3D object a house is drawn as in the turned view (as the view picks it), or nullptr. */
const ThreeDObject *HouseObject(TileIndex origin)
{
	HouseID north = GetHouseType(origin);
	return GetThreeDHouseObjectAt(north, origin);
}

/** The doors of the town's houses looking out onto its roads. */
void TownDoors(const Town *town, std::vector<Door> &doors)
{
	const int cx = TileX(town->xy), cy = TileY(town->xy);
	for (int y = std::max(0, cy - DOOR_SEARCH); y <= std::min<int>(Map::MaxY(), cy + DOOR_SEARCH); y++) {
		for (int x = std::max(0, cx - DOOR_SEARCH); x <= std::min<int>(Map::MaxX(), cx + DOOR_SEARCH); x++) {
			TileIndex t = TileXY(x, y);
			if (!IsTileType(t, TileType::House)) continue;
			HouseID house = GetHouseType(t);
			if (GetHouseNorthPart(house) != 0) continue; // the north tile only
			if (!IsHouseCompleted(t)) {
				Debug(misc, 3, "3D walker: the house {} at {},{} is being built", house, x, y);
				continue;
			}
			const ThreeDObject *obj = HouseObject(t);
			Debug(misc, 4, "3D walker: the house {} at {},{}: {}", house, x, y, obj == nullptr ? std::string("no object") : fmt::format("{} with {} doors", obj->name, obj->doors.size()));
			if (obj == nullptr || obj->doors.empty()) continue;
			const double ox = x * TILE_SIZE, oy = y * TILE_SIZE;
			const double w = obj->tiles_x * TILE_SIZE, h = obj->tiles_y * TILE_SIZE;
			for (const ThreeDObjectDoor &d : obj->doors) {
				/* Out of it along its way out, the nearer axis, to the edge of the plot; the road tile beyond. */
				int sx = 0, sy = 0;
				if (std::abs(d.out[0]) >= std::abs(d.out[1])) {
					sx = d.out[0] >= 0 ? 1 : -1;
				} else {
					sy = d.out[1] >= 0 ? 1 : -1;
				}
				Door door;
				door.house = t;
				door.door = {ox + d.at[0] * OBJECT_UNITS_PER_METRE, oy + d.at[1] * OBJECT_UNITS_PER_METRE};
				door.edge = door.door;
				if (sx > 0) door.edge.x = ox + w;
				if (sx < 0) door.edge.x = ox;
				if (sy > 0) door.edge.y = oy + h;
				if (sy < 0) door.edge.y = oy;
				const int rx = static_cast<int>(std::floor((door.edge.x + sx * 0.5) / TILE_SIZE)), ry = static_cast<int>(std::floor((door.edge.y + sy * 0.5) / TILE_SIZE));
				/* The road of the town before the door; else the nearest one about it: Mat is a repairman, he goes
				 * to a door over a fence or whatever is in the way (the player). */
				door.road = INVALID_TILE;
				double best = 1e18;
				for (int ny = ry - THROUGH_REACH; ny <= ry + THROUGH_REACH; ny++) {
					for (int nx = rx - THROUGH_REACH; nx <= rx + THROUGH_REACH; nx++) {
						if (nx < 0 || ny < 0 || nx > static_cast<int>(Map::MaxX()) || ny > static_cast<int>(Map::MaxY())) continue;
						TileIndex n = TileXY(nx, ny);
						if (!IsWalkway(n, town)) continue;
						Point2 c = Middle(n);
						double dist = std::hypot(c.x - door.edge.x, c.y - door.edge.y) + (n == TileXY(std::max(rx, 0), std::max(ry, 0)) ? -static_cast<double>(TILE_SIZE) : 0.0);
						if (dist < best) {
							best = dist;
							door.road = n;
						}
					}
				}
				if (door.road == INVALID_TILE) {
					Debug(misc, 3, "3D walker: no road of the town about the door of the house at {},{}", x, y);
					continue;
				}
				/* On the pavement of that road tile, on the side of the door: the nearest point of the square the
				 * pavement goes round. */
				const Point2 c = Middle(door.road);
				door.pavement = {c.x + std::clamp(door.edge.x - c.x, -PAVEMENT, PAVEMENT), c.y + std::clamp(door.edge.y - c.y, -PAVEMENT, PAVEMENT)};
				doors.push_back(door);
			}
		}
	}
}

/**
 * The road tiles from one to another, both ends of each step joined; empty when there is no way.
 * @param from the first tile
 * @param ends the tiles it may end on
 * @param[out] end the one it ends on
 */
std::vector<TileIndex> Route(const Town *town, TileIndex from, const std::vector<TileIndex> &ends, TileIndex &end)
{
	/* The creatures' finder (NavFindTiles()) over the town's walkways, each step's road joined both ways, to the four
	 * neighbours: no finder of his own (the player, 10. 11.: "we do not need four way finders eating the power"). */
	const NavProfile profile{false, 1.0,
			[town](TileIndex a, TileIndex b) { return IsWalkway(b, town) && Joined(a, b, DiagdirBetweenTiles(a, b)); },
			[town](TileIndex t) { return IsWalkway(t, town); },
			[](TileIndex) { return 1.0; }};
	bool reached;
	std::vector<TileIndex> route = NavFindTiles(from, ends, profile, ROUTE_REACH, reached);
	end = INVALID_TILE;
	if (!reached) return {};
	route.insert(route.begin(), from);
	end = route.back();
	return route;
}

/** The way from one door to another over a route of road tiles, on the pavement on one side. */
std::vector<Point2> MakePath(const Door &from, const Door &to, const std::vector<TileIndex> &route)
{
	std::vector<Point2> path{from.door, from.edge, from.pavement};
	if (route.size() >= 2) {
		/* The side of the road it keeps to: the one of the door it comes out of, seen along its first step. */
		auto dir = [&](size_t i) {
			Point2 a = Middle(route[i]), b = Middle(route[i + 1]);
			double l = std::hypot(b.x - a.x, b.y - a.y);
			return Point2{(b.x - a.x) / l, (b.y - a.y) / l};
		};
		auto left = [](Point2 d) { return Point2{-d.y, d.x}; };
		Point2 d0 = dir(0), c0 = Middle(route[0]);
		double cross = d0.x * (from.pavement.y - c0.y) - d0.y * (from.pavement.x - c0.x);
		const double side = cross >= 0 ? 1.0 : -1.0;
		for (size_t i = 1; i + 1 < route.size(); i++) {
			Point2 a = dir(i - 1), b = dir(i);
			if (std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6) continue; // straight on
			/* A corner: where the pavements along both ways meet. */
			Point2 c = Middle(route[i]), la = left(a), lb = left(b);
			path.push_back({c.x + side * PAVEMENT * (la.x + lb.x), c.y + side * PAVEMENT * (la.y + lb.y)});
		}
	}
	path.push_back(to.pavement);
	path.push_back(to.edge);
	path.push_back(to.door);
	return path;
}

/** Put a walker where it is along its way, facing along it, in its frame. */
void Place(Walker &w, const ThreeDVehicleObject *fig)
{
	size_t i = 1;
	while (i + 1 < w.path.size() && w.at[i] < w.walked) i++;
	const Point2 &a = w.path[i - 1], &b = w.path[i];
	double len = w.at[i] - w.at[i - 1];
	double f = len > 0 ? std::clamp((w.walked - w.at[i - 1]) / len, 0.0, 1.0) : 1.0;
	w.x = a.x + (b.x - a.x) * f;
	w.y = a.y + (b.y - a.y) * f;
	if (len > 0) w.facing = static_cast<float>(std::atan2(b.y - a.y, b.x - a.x) * 180.0 / M_PI);
	int ix = std::clamp<int>(static_cast<int>(w.x), 0, Map::MaxX() * TILE_SIZE), iy = std::clamp<int>(static_cast<int>(w.y), 0, Map::MaxY() * TILE_SIZE);
	w.z = GetSlopePixelZ(ix, iy);
	w.zf = GroundZF(w.x, w.y, false);
	w.tile = TileVirtXY(ix, iy);
	double round = RoundLength(fig);
	w.frame = static_cast<int>(std::floor(w.walked / round * fig->frames)) % fig->frames;
}

/** A walker drawn again on the map where it is now, as it is (Walker::drawn_x). */
void Redraw(Walker &w, const ThreeDVehicleObject *fig)
{
	MarkStandingDirty(fig, w.x, w.y, w.z, false);
	w.drawn_x = w.x;
	w.drawn_y = w.y;
	w.drawn_z = w.z;
	w.drawn_facing = w.facing;
}

/** The lengths along a way, for a walker going along it (Walker::at). */
void Measure(Walker &w)
{
	w.at.assign(1, 0.0);
	for (size_t i = 1; i < w.path.size(); i++) w.at.push_back(w.at.back() + std::hypot(w.path[i].x - w.path[i - 1].x, w.path[i].y - w.path[i - 1].y));
}

/** Send a walker out in a town now, if it has two doors with a way between; whether one went. */
bool Send(const Town *town, const ThreeDVehicleObject *fig)
{
	std::vector<Door> doors;
	TownDoors(town, doors);
	if (doors.size() < 2) return false;
	const Door &from = doors[InteractiveRandomRange(static_cast<uint32_t>(doors.size()))];
	/* The door it goes to: a random one of another house that the town's roads lead to. */
	std::vector<const Door *> others;
	for (const Door &d : doors) {
		if (d.house != from.house) others.push_back(&d);
	}
	const Door *to = nullptr;
	std::vector<TileIndex> route;
	for (int tries = 0; tries < 4 && to == nullptr && !others.empty(); tries++) {
		size_t pick = InteractiveRandomRange(static_cast<uint32_t>(others.size()));
		TileIndex end;
		route = Route(town, from.road, {others[pick]->road}, end);
		if (!route.empty()) {
			to = others[pick];
		} else {
			others.erase(others.begin() + pick);
		}
	}
	if (to == nullptr) return false;

	Walker w;
	w.town = town->index;
	w.path = MakePath(from, *to, route);
	Measure(w);
	Place(w, fig);
	Redraw(w, fig);
	Debug(misc, 1, "3D walker: {} out of the door of the house at {},{} to the one at {},{} in {}, {} road tiles, {:.0f} units", WALKER_FIGURE,
			TileX(from.house), TileY(from.house), TileX(to->house), TileY(to->house), town->index.base(), route.size(), w.at.back());
	_walkers.push_back(std::move(w));
	return true;
}

/**
 * A walkable place some tiles off a point, from where the creatures' way (NavFindWay()) leads to it: where he appears
 * before a visit, or goes off to after it.
 * @param[out] way the way from there to the point, or back from the point to there
 * @param back the way from the point to the place, not to the point
 */
bool PlaceOff(Point2 at, bool back, Point2 &place, std::vector<Point2> &way)
{
	for (int tries = 0; tries < 8; tries++) {
		const double a = InteractiveRandomRange(360) * M_PI / 180.0;
		const double r = (VISIT_FROM_MIN + InteractiveRandomRange(VISIT_FROM_MAX - VISIT_FROM_MIN + 1)) * static_cast<double>(TILE_SIZE);
		double x = at.x + std::cos(a) * r, y = at.y + std::sin(a) * r;
		if (x < TILE_SIZE || y < TILE_SIZE || x >= (Map::MaxX() - 1) * TILE_SIZE || y >= (Map::MaxY() - 1) * TILE_SIZE) continue;
		if (!NavNearestWalkable(x, y, 1)) continue;
		const std::vector<NavPoint> found = back ? NavFindWay(at.x, at.y, x, y, 24) : NavFindWay(x, y, at.x, at.y, 24);
		if (found.empty()) continue;
		/* It must get there (DOOR_APPROACH: a walkable tile). */
		const NavPoint end = found.back();
		const Point2 goal = back ? Point2{x, y} : at;
		if (std::hypot(end.first - goal.x, end.second - goal.y) > TILE_SIZE / 2.0) continue;
		place = {x, y};
		way.clear();
		for (const auto &[px, py] : found) way.push_back({px, py});
		return true;
	}
	return false;
}

/**
 * Send him to visit a depot or a lighthouse now, by one of its doors (ThreeDPeopleDoorsAt()): from a place some tiles
 * off by the creatures' way to the front of the door, in at it; whether he went.
 */
bool SendVisit(TileIndex tile, const ThreeDVehicleObject *fig)
{
	const std::vector<ThreeDPeopleDoor> doors = ThreeDPeopleDoorsAt(tile);
	if (doors.empty()) return false;
	const ThreeDPeopleDoor &d = doors[InteractiveRandomRange(static_cast<uint32_t>(doors.size()))];
	Walker w;
	w.town = TownID::Invalid();
	w.visit = tile;
	w.door = {d.x, d.y};
	w.front = {d.x + d.out_x * DOOR_FRONT_M * OBJECT_UNITS_PER_METRE, d.y + d.out_y * DOOR_FRONT_M * OBJECT_UNITS_PER_METRE};
	w.approach = {d.x + d.out_x * DOOR_APPROACH, d.y + d.out_y * DOOR_APPROACH};
	Point2 from;
	std::vector<Point2> way;
	if (!PlaceOff(w.approach, false, from, way)) return false;
	w.path.push_back(from);
	w.path.insert(w.path.end(), way.begin(), way.end());
	w.path.push_back(w.approach);
	w.path.push_back(w.front);
	w.path.push_back(w.door);
	Measure(w);
	Place(w, fig);
	Redraw(w, fig);
	Debug(misc, 1, "3D walker: {} visits {},{} from {:.0f},{:.0f}, {} points, {:.0f} units", WALKER_FIGURE, TileX(tile), TileY(tile), from.x, from.y, w.path.size(), w.at.back());
	_walkers.push_back(std::move(w));
	return true;
}

/** Out of the door after a visit: off by the creatures' way to a place some tiles away; whether there is one. */
bool LeaveVisit(Walker &w, const ThreeDVehicleObject *fig)
{
	Point2 to;
	std::vector<Point2> way;
	if (!PlaceOff(w.approach, true, to, way)) return false;
	w.path = {w.door, w.front, w.approach};
	w.path.insert(w.path.end(), way.begin(), way.end());
	Measure(w);
	w.walked = 0;
	w.stage = 2;
	Place(w, fig);
	Redraw(w, fig);
	return true;
}

/** The depots and lighthouses with doors for people about the middle of the view (ThreeDPeopleDoorsAt()), their north tiles. */
std::vector<TileIndex> VisitPlaces(TileIndex middle)
{
	std::vector<TileIndex> places;
	const int cx = TileX(middle), cy = TileY(middle);
	for (int y = std::max(1, cy - VISIT_REACH); y <= std::min<int>(Map::MaxY() - 1, cy + VISIT_REACH); y++) {
		for (int x = std::max(1, cx - VISIT_REACH); x <= std::min<int>(Map::MaxX() - 1, cx + VISIT_REACH); x++) {
			const TileIndex t = TileXY(x, y);
			const bool lighthouse = IsTileType(t, TileType::Object) && GetObjectType(t) == OBJECT_LIGHTHOUSE;
			const bool depot = IsRailDepotTile(t) || IsRoadDepotTile(t) || IsShipDepotTile(t);
			if ((lighthouse || depot) && !ThreeDPeopleDoorsAt(t).empty()) places.push_back(t);
		}
	}
	return places;
}

/** The middle of the main view when it is turned, else INVALID_TILE. */
TileIndex TurnedViewMiddle()
{
	if (_game_mode != GameMode::Normal) return INVALID_TILE;
	Window *w = GetMainWindow();
	if (w == nullptr || w->viewport == nullptr || !IsViewportTurned(*w->viewport)) return INVALID_TILE;
	const Viewport &vp = *w->viewport;
	Point p = TranslateXYToTileCoord(vp, vp.left + vp.width / 2, vp.top + vp.height / 2, true);
	return TileVirtXY(p.x, p.y);
}

/**
 * Where the life of the towns goes on (the walkers, the cars of the game's own): about the walker of the view from the
 * first person while it is out (the player, 16. 10.: "Mat was not seen at all, I came out beside him"), else about
 * the middle of the main view when it is turned; else nowhere.
 */
static TileIndex LifeMiddle()
{
	double x, y;
	if (_game_mode == GameMode::Normal && FirstPersonWalkerAt(x, y)) {
		const int tx = std::clamp(static_cast<int>(x / TILE_SIZE), 0, static_cast<int>(Map::SizeX()) - 1);
		const int ty = std::clamp(static_cast<int>(y / TILE_SIZE), 0, static_cast<int>(Map::SizeY()) - 1);
		return TileXY(tx, ty);
	}
	return TurnedViewMiddle();
}

/** Every tick: the walkers walk on; at their doors they go in; the towns near the view get theirs. */
const IntervalTimer<TimerGameTick> _walkers_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	TileIndex middle = LifeMiddle();
	const ThreeDVehicleObject *fig = middle != INVALID_TILE ? WalkerFigure() : nullptr;
	if (fig == nullptr) {
		for (const Walker &w : _walkers) MarkStandingDirty(GetThreeDFigure(WALKER_FIGURE), w.drawn_x, w.drawn_y, w.drawn_z, false);
		_walkers.clear();
		return;
	}
	const double speed = WALK_PACE * RoundLength(fig) / (fig->frames * fig->frame_ticks);
	const uint64_t now = TimerGameTick::counter;
	for (auto it = _walkers.begin(); it != _walkers.end();) {
		Walker &w = *it;
		if (w.visit != INVALID_TILE) {
			/* A visit: to the door and in, a while inside, out and off (SendVisit()). */
			if (DistanceManhattan(w.visit, middle) > TOWN_GONE || ThreeDPeopleDoorsAt(w.visit).empty()) {
				if (w.stage != 1) MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
				it = _walkers.erase(it);
				continue;
			}
			if (w.stage == 1) {
				if (now >= w.until && !LeaveVisit(w, fig)) {
					/* Nowhere to go off to: he stays in. */
					_next_visit[w.visit] = now + VISIT_AGAIN + InteractiveRandomRange(VISIT_AGAIN_MORE);
					it = _walkers.erase(it);
					continue;
				}
				++it;
				continue;
			}
			w.walked += speed;
			if (w.walked >= w.at.back()) {
				MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
				if (w.stage == 0) {
					/* In at the door. */
					w.stage = 1;
					w.until = now + VISIT_INSIDE + InteractiveRandomRange(VISIT_INSIDE_MORE);
					++it;
				} else {
					/* Off where he went: gone. */
					_next_visit[w.visit] = now + VISIT_AGAIN + InteractiveRandomRange(VISIT_AGAIN_MORE);
					it = _walkers.erase(it);
				}
				continue;
			}
			const int frame = w.frame;
			Place(w, fig);
			if (w.frame != frame) {
				MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
				Redraw(w, fig);
			}
			++it;
			continue;
		}
		const Town *town = Town::GetIfValid(w.town);
		w.walked += speed;
		if (town == nullptr || w.walked >= w.at.back() || DistanceManhattan(town->xy, middle) > TOWN_GONE) {
			/* In at the door: out again a while later, somewhere. */
			MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
			_next_walk[w.town] = now + 150 + InteractiveRandomRange(450);
			it = _walkers.erase(it);
			continue;
		}
		const int frame = w.frame;
		Place(w, fig);
		/* Its next frame: drawn again where it was and where it is. */
		if (w.frame != frame) {
			MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
			Redraw(w, fig);
		}
		++it;
	}
	if (now % 16 != 0) return;
	/* The depots and lighthouses about get his visit now and then, each its own (the first time after a while). */
	if (now % 64 == 0) {
		for (const TileIndex place : VisitPlaces(middle)) {
			const size_t visits = std::ranges::count_if(_walkers, [](const Walker &w) { return w.visit != INVALID_TILE; });
			if (visits >= VISITS_AT_ONCE) break;
			if (std::ranges::any_of(_walkers, [&](const Walker &w) { return w.visit != INVALID_TILE && DistanceManhattan(w.visit, place) <= VISIT_APART; })) continue;
			auto next = _next_visit.find(place);
			if (next == _next_visit.end()) {
				_next_visit[place] = now + InteractiveRandomRange(VISIT_AGAIN);
				continue;
			}
			if (now < next->second) continue;
			if (!SendVisit(place, fig)) _next_visit[place] = now + VISIT_AGAIN;
		}
	}
	for (const Town *town : Town::Iterate()) {
		if (DistanceManhattan(town->xy, middle) > TOWN_REACH) continue;
		if (std::ranges::any_of(_walkers, [&](const Walker &w) { return w.town == town->index; })) continue;
		auto next = _next_walk.find(town->index);
		if (next != _next_walk.end() && now < next->second) continue;
		/* No two doors with a way between: look again in a while. */
		if (!Send(town, fig)) _next_walk[town->index] = now + 500;
	}
});

/*
 * The cars of the game's own driving about by themselves (the player, 15. 10.: "the Tatra will be in the game like the
 * girls at the stops and Mat"; "it will drive like Mat, from one ÚV KSČ to another ÚV KSČ"): a car of the index with
 * "jizda" drives on the roads between the houses it names, from the stop ("stani") of one to the stop of another,
 * keeping to its side of the road, stands a while and drives on. Only the screen, as the walkers.
 */

/** How far from the middle of the road the cars drive, world units. */
constexpr double LANE = 3.0;
/** The stops looked for within this far of the middle of the view (tiles, Manhattan). */
constexpr uint STOP_REACH = 64;
/** How many road tiles the way between two stops is looked for over. */
constexpr size_t DRIVE_SEARCH = 20000;
/** Ticks of the game a second. */
constexpr double TICKS_A_SECOND = 1000.0 / MILLISECONDS_PER_TICK;

/** A stop of a house to drive to: where the car stands, and the road tile nearest it. */
struct Stop {
	TileIndex house;
	TileIndex road;
	Point2 at;
	float facing;
};

/** One car driving. */
struct Driver {
	const ThreeDVehicleObject *car = nullptr;
	std::vector<Point2> path;
	std::vector<double> at;
	double driven = 0;
	TileIndex house = INVALID_TILE;  ///< the house it stands at or drives to
	uint64_t stand_until = 0;        ///< standing until then (the game's tick counter); 0 driving
	TileIndex tile = INVALID_TILE;
	double x = 0, y = 0;
	int z = 0;
	double zf = 0;     ///< the ground there on its slope's plane, not the game's whole steps (GroundZF()): where the eyes see it stand
	float facing = 0;
};

std::vector<Driver> _drivers;

/** The stops of the houses a car drives between, near the middle of the view. */
std::vector<Stop> HouseStops(HouseID house, TileIndex middle)
{
	std::vector<Stop> stops;
	for (const Town *town : Town::Iterate()) {
		if (DistanceManhattan(town->xy, middle) > STOP_REACH) continue;
		const int cx = TileX(town->xy), cy = TileY(town->xy);
		for (int y = std::max(0, cy - DOOR_SEARCH); y <= std::min<int>(Map::MaxY(), cy + DOOR_SEARCH); y++) {
			for (int x = std::max(0, cx - DOOR_SEARCH); x <= std::min<int>(Map::MaxX(), cx + DOOR_SEARCH); x++) {
				TileIndex t = TileXY(x, y);
				if (!IsTileType(t, TileType::House) || GetHouseType(t) != house || !IsHouseCompleted(t)) continue;
				if (std::ranges::any_of(stops, [t](const Stop &s) { return s.house == t; })) continue; // two towns about it
				HouseID north = GetHouseType(t);
				if (GetHouseNorthPart(north) != 0) continue;
				const ThreeDObject *obj = HouseObject(t);
				if (obj == nullptr || !obj->has_stop) continue;
				Stop s;
				s.house = t;
				s.at = {x * TILE_SIZE + obj->stop[0], y * TILE_SIZE + obj->stop[1]};
				s.facing = obj->stop_turn;
				/* The road tile nearest the stop, about the plot. */
				s.road = INVALID_TILE;
				double best = 1e18;
				for (int dy = -2; dy <= obj->tiles_y + 1; dy++) {
					for (int dx = -2; dx <= obj->tiles_x + 1; dx++) {
						if (x + dx < 0 || y + dy < 0 || x + dx > static_cast<int>(Map::MaxX()) || y + dy > static_cast<int>(Map::MaxY())) continue;
						TileIndex r = TileXY(x + dx, y + dy);
						if (!IsNormalRoadTile(r) || !HasTileRoadType(r, RoadTramType::Road) || GetRoadBits(r, RoadTramType::Road).None()) continue;
						Point2 m = Middle(r);
						double d = std::hypot(m.x - s.at.x, m.y - s.at.y);
						if (d < best) {
							best = d;
							s.road = r;
						}
					}
				}
				if (s.road != INVALID_TILE) stops.push_back(s);
			}
		}
	}
	return stops;
}

/** The road tiles from one to another over the roads, both ends with; empty when there is no way. */
std::vector<TileIndex> RoadRoute(TileIndex from, TileIndex to)
{
	std::unordered_map<TileIndex, TileIndex> came;
	std::vector<TileIndex> queue{from};
	came[from] = from;
	for (size_t i = 0; i < queue.size() && came.size() < DRIVE_SEARCH; i++) {
		TileIndex t = queue[i];
		if (t == to) break;
		for (DiagDirection d : {DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
			TileIndex n = Next(t, d);
			if (n == INVALID_TILE || came.contains(n)) continue;
			if (!IsNormalRoadTile(n) || !HasTileRoadType(n, RoadTramType::Road) || !Joined(t, n, d)) continue;
			came[n] = t;
			queue.push_back(n);
		}
	}
	if (!came.contains(to)) return {};
	std::vector<TileIndex> route{to};
	while (route.back() != from) route.push_back(came[route.back()]);
	std::reverse(route.begin(), route.end());
	return route;
}

/** The way from one stop to another over a route of road tiles, keeping to the side of the road the game drives on. */
std::vector<Point2> DrivePath(const Stop &from, const Stop &to, const std::vector<TileIndex> &route)
{
	auto dir = [&](size_t i) {
		Point2 a = Middle(route[i]), b = Middle(route[i + 1]);
		double l = std::hypot(b.x - a.x, b.y - a.y);
		return Point2{(b.x - a.x) / l, (b.y - a.y) / l};
	};
	auto left = [](Point2 d) { return Point2{-d.y, d.x}; };
	/* Seen from above (z up), the left of a way along d is (-d.y, d.x); the right is the other side. */
	const double side = _settings_game.vehicle.road_side == RoadVehicleDrivingSide::Right ? -LANE : LANE;
	std::vector<Point2> path{from.at};
	if (route.size() < 2) {
		path.push_back(Middle(route.front()));
	} else {
		Point2 d0 = dir(0), l0 = left(d0), c0 = Middle(route[0]);
		path.push_back({c0.x + side * l0.x, c0.y + side * l0.y});
		for (size_t i = 1; i + 1 < route.size(); i++) {
			Point2 a = dir(i - 1), b = dir(i);
			if (std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6) continue; // straight on
			Point2 c = Middle(route[i]), la = left(a), lb = left(b);
			path.push_back({c.x + side * (la.x + lb.x), c.y + side * (la.y + lb.y)});
		}
		Point2 dn = dir(route.size() - 2), ln = left(dn), cn = Middle(route.back());
		path.push_back({cn.x + side * ln.x, cn.y + side * ln.y});
	}
	path.push_back(to.at);
	return path;
}

/** Put a car where it is along its way, facing along it. */
void PlaceCar(Driver &d)
{
	size_t i = 1;
	while (i + 1 < d.path.size() && d.at[i] < d.driven) i++;
	const Point2 &a = d.path[i - 1], &b = d.path[i];
	double len = d.at[i] - d.at[i - 1];
	double f = len > 0 ? std::clamp((d.driven - d.at[i - 1]) / len, 0.0, 1.0) : 1.0;
	d.x = a.x + (b.x - a.x) * f;
	d.y = a.y + (b.y - a.y) * f;
	if (len > 0) d.facing = static_cast<float>(std::atan2(b.y - a.y, b.x - a.x) * 180.0 / M_PI);
	int ix = std::clamp<int>(static_cast<int>(d.x), 0, Map::MaxX() * TILE_SIZE), iy = std::clamp<int>(static_cast<int>(d.y), 0, Map::MaxY() * TILE_SIZE);
	d.z = GetSlopePixelZ(ix, iy);
	d.zf = GroundZF(d.x, d.y, false);
	d.tile = TileVirtXY(ix, iy);
}

/** A car drawn again where it is now (MarkStandingDirty()), once it was put on its way. */
void MarkCarDirty(const Driver &d)
{
	if (d.tile == INVALID_TILE) return;
	MarkStandingDirty(d.car, d.x, d.y, d.z, IsNightNow());
}

/** Send a car from the stop it stands at (or any, the first time) to another one the roads lead to; whether it went. */
bool Drive(Driver &d, TileIndex middle)
{
	std::vector<Stop> stops = HouseStops(d.car->drive_house, middle);
	if (stops.size() < 2) return false;
	const Stop *from = nullptr;
	for (const Stop &s : stops) {
		if (s.house == d.house) from = &s;
	}
	if (from == nullptr) from = &stops[InteractiveRandomRange(static_cast<uint32_t>(stops.size()))];
	std::vector<const Stop *> others;
	for (const Stop &s : stops) {
		if (s.house != from->house) others.push_back(&s);
	}
	while (!others.empty()) {
		size_t pick = InteractiveRandomRange(static_cast<uint32_t>(others.size()));
		std::vector<TileIndex> route = RoadRoute(from->road, others[pick]->road);
		if (route.empty()) {
			others.erase(others.begin() + pick);
			continue;
		}
		d.path = DrivePath(*from, *others[pick], route);
		d.at.assign(1, 0.0);
		for (size_t i = 1; i < d.path.size(); i++) d.at.push_back(d.at.back() + std::hypot(d.path[i].x - d.path[i - 1].x, d.path[i].y - d.path[i - 1].y));
		d.driven = 0;
		d.house = others[pick]->house;
		d.stand_until = 0;
		MarkCarDirty(d);
		PlaceCar(d);
		MarkCarDirty(d);
		Debug(misc, 1, "3D car {}: from the house at {},{} to the one at {},{}, {} road tiles", d.car->name, TileX(from->house), TileY(from->house), TileX(d.house), TileY(d.house), route.size());
		return true;
	}
	return false;
}

/** Every tick: the cars drive on, stand at their stops and drive on again. */
const IntervalTimer<TimerGameTick> _drivers_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	TileIndex middle = LifeMiddle();
	const std::vector<ThreeDVehicleObject> &cars = GetThreeDDrivingCars();
	if (middle == INVALID_TILE || cars.empty()) {
		for (const Driver &d : _drivers) MarkCarDirty(d);
		_drivers.clear();
		return;
	}
	const uint64_t now = TimerGameTick::counter;
	/* One of each car of the index. */
	if (_drivers.size() != cars.size()) {
		for (const Driver &d : _drivers) MarkCarDirty(d);
		_drivers.clear();
		for (const ThreeDVehicleObject &car : cars) {
			Driver d;
			d.car = &car;
			d.stand_until = now + InteractiveRandomRange(100);
			_drivers.push_back(std::move(d));
		}
	}
	const double units_per_metre = OBJECT_UNITS_PER_METRE;
	for (Driver &d : _drivers) {
		if (d.stand_until != 0) {
			if (now < d.stand_until) continue;
			/* Off again, or stand a while longer when there is nowhere to go. */
			if (!Drive(d, middle)) d.stand_until = now + 500;
			continue;
		}
		if (d.path.empty()) continue;
		d.driven += d.car->drive_kmh / 3.6 / TICKS_A_SECOND * units_per_metre * WalkerScale();
		if (d.driven >= d.at.back()) {
			d.driven = d.at.back();
			d.stand_until = now + std::max(1, d.car->drive_stand);
		}
		/* Drawn again where it was and where it is (each tick it drives: where it was is where it was drawn). */
		MarkCarDirty(d);
		PlaceCar(d);
		MarkCarDirty(d);
	}
});

} // namespace

/** Put the cars of the game's own driving about on the turned view being drawn (after its vehicles). */
void AddThreeDDriversToViewport(ZoomLevel zoom)
{
	if (_drivers.empty()) return;
	const float scale = WalkerScale();
	for (const Driver &d : _drivers) {
		if (d.path.empty() || d.tile == INVALID_TILE) continue;
		AddThreeDObjectToViewport(d.car, scale, static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)), d.z, d.facing, zoom, IsNightNow());
	}
}

/** The cars of the game's own driving about in the view from the eyes: their objects among its figures (AddCabCar()). */
void AddThreeDDriversToEye()
{
	const float scale = WalkerScale();
	for (const Driver &d : _drivers) {
		if (d.path.empty() || d.tile == INVALID_TILE) continue;
		AddCabCar(d.car, scale, d.x, d.y, d.zf, d.facing, IsNightNow());
	}
}

/** At night the lights of the cars of the game's own, for the night over the turned view (DrawNight()): each with where its car stands. */
void ForEachThreeDDriverLight(ZoomLevel zoom, const std::function<void(int x, int y, int z, const ThreeDPictureLight &light)> &fn)
{
	if (_drivers.empty()) return;
	const float scale = WalkerScale();
	for (const Driver &d : _drivers) {
		if (d.path.empty() || d.tile == INVALID_TILE) continue;
		const std::vector<ThreeDPictureLight> *lights = GetThreeDObjectLights(d.car, scale, d.facing, zoom);
		if (lights == nullptr) continue;
		for (const ThreeDPictureLight &l : *lights) fn(static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)), d.z, l);
	}
}

/** The rig: where the first car of the game's own is, world units; false when none drives. */
bool GetFirstThreeDDriverPlace(int &x, int &y)
{
	for (const Driver &d : _drivers) {
		if (d.path.empty() || d.tile == INVALID_TILE) continue;
		x = static_cast<int>(std::lround(d.x));
		y = static_cast<int>(std::lround(d.y));
		return true;
	}
	return false;
}

/** The rig: the cars driving, a line each. */
std::string DescribeThreeDDrivers()
{
	std::string out = fmt::format("{} cars of {}", _drivers.size(), GetThreeDDrivingCars().size());
	for (const Driver &d : _drivers) {
		out += fmt::format("\n  {} at {:.1f},{:.1f},{} facing {:.0f}, {:.0f} of {:.0f} driven, {}", d.car->name, d.x, d.y, d.z, d.facing, d.driven, d.at.empty() ? 0.0 : d.at.back(), d.stand_until != 0 ? "standing" : "driving");
	}
	TileIndex middle = TurnedViewMiddle();
	if (middle != INVALID_TILE && !GetThreeDDrivingCars().empty()) {
		std::vector<Stop> stops = HouseStops(GetThreeDDrivingCars().front().drive_house, middle);
		out += fmt::format("\n  {} stops near the view", stops.size());
		for (const Stop &s : stops) out += fmt::format("\n    house {},{} stop {:.1f},{:.1f} road {},{}", TileX(s.house), TileY(s.house), s.at.x, s.at.y, TileX(s.road), TileY(s.road));
	}
	return out;
}

/** Where the life of the towns goes on (LifeMiddle()): about the walker of the view from the first person, else the middle of the turned main view; INVALID_TILE nowhere. */
TileIndex ThreeDLifeMiddle()
{
	return LifeMiddle();
}

/** Put the walkers on the turned view being drawn (after its vehicles; ViewportDoDrawTurned()). */
void AddThreeDWalkersToViewport(ZoomLevel zoom)
{
	if (_walkers.empty() || IsTransparencySet(TransparencyOption::Houses)) return;
	const float scale = WalkerScale();
	for (const Walker &w : _walkers) {
		if (w.stage == 1) continue; // inside a depot or a lighthouse
		/* Where it was drawn again last (Walker::drawn_x): as the screen has it. */
		const int x = static_cast<int>(std::lround(w.drawn_x)), y = static_cast<int>(std::lround(w.drawn_y));
		const TileIndex tile = TileVirtXY(std::clamp<int>(x, 0, Map::MaxX() * TILE_SIZE), std::clamp<int>(y, 0, Map::MaxY() * TILE_SIZE));
		AddThreeDFigureToViewport(WALKER_FIGURE, scale, x, y, w.drawn_z, w.drawn_facing, zoom, tile, 0, std::max(0, w.frame));
	}
}

/** The walkers in the view from the eyes, among its figures at their true size (AddCabFigure()). */
void AddThreeDWalkersToEye(float scale)
{
	if (_walkers.empty()) return;
	for (const Walker &w : _walkers) {
		if (w.stage == 1) continue; // inside a depot or a lighthouse
		/* Where it is and on the slope's plane, not at whole units (the player, test4: the stairs). */
		AddCabFigure(WALKER_FIGURE, scale, w.x, w.y, w.zf, w.facing, std::max(0, w.frame));
	}
}

/** Send a walker out now in the town nearest the middle of the turned view or the walker of the view from the first person (the rig); whether one went. */
bool SendThreeDWalker()
{
	TileIndex middle = LifeMiddle();
	const ThreeDVehicleObject *fig = WalkerFigure();
	if (middle == INVALID_TILE || fig == nullptr) return false;
	const Town *best = nullptr;
	for (const Town *town : Town::Iterate()) {
		if (best == nullptr || DistanceManhattan(town->xy, middle) < DistanceManhattan(best->xy, middle)) best = town;
	}
	if (best == nullptr) return false;
	std::erase_if(_walkers, [&](const Walker &w) { return w.town == best->index; });
	return Send(best, fig);
}

/** Send him now to visit the depot or lighthouse nearest the middle of the life about (the rig, SendVisit()); whether he went. */
bool SendThreeDVisit()
{
	const TileIndex middle = LifeMiddle();
	const ThreeDVehicleObject *fig = WalkerFigure();
	if (middle == INVALID_TILE || fig == nullptr) return false;
	std::vector<TileIndex> places = VisitPlaces(middle);
	if (places.empty()) return false;
	std::ranges::sort(places, [&](TileIndex a, TileIndex b) { return DistanceManhattan(a, middle) < DistanceManhattan(b, middle); });
	std::erase_if(_walkers, [&](const Walker &w) { return w.visit == places.front(); });
	return SendVisit(places.front(), fig);
}

/** Where the first walker is, world units (the rig); false when none walks. */
bool ThreeDWalkerAt(int &x, int &y, int &z)
{
	if (_walkers.empty()) return false;
	x = static_cast<int>(std::lround(_walkers.front().x));
	y = static_cast<int>(std::lround(_walkers.front().y));
	z = _walkers.front().z;
	return true;
}

/** Whether Mat walks within reach of a point (the walker), metres: he sells marijuana a piece at a time (walker_inventory.h). */
bool ThreeDMatNear(double x, double y, double reach_m)
{
	for (const Walker &w : _walkers) {
		if (w.stage != 1 && std::hypot(w.x - x, w.y - y) <= reach_m * OBJECT_UNITS_PER_METRE) return true;
	}
	return false;
}

/** Whether a tile is a road of a town to walk on (the rig). */
bool IsThreeDWalkway(TileIndex t, const Town *town)
{
	return IsWalkway(t, town);
}

/** The rig: the walkers, a line each, and the doors of the town nearest the view. */
std::string DescribeThreeDWalkers()
{
	std::string out = fmt::format("{} walkers", _walkers.size());
	for (const Walker &w : _walkers) {
		if (w.visit != INVALID_TILE) {
			out += fmt::format("\n  visit {},{} {} at {:.1f},{:.1f},{} facing {:.0f} frame {}, {:.0f} of {:.0f} walked, {} points", TileX(w.visit), TileY(w.visit),
					w.stage == 0 ? "to the door" : w.stage == 1 ? "inside" : "off", w.x / TILE_SIZE, w.y / TILE_SIZE, w.z, w.facing, w.frame, w.walked, w.at.back(), w.path.size());
			continue;
		}
		out += fmt::format("\n  town {} at {:.1f},{:.1f},{} facing {:.0f} frame {}, {:.0f} of {:.0f} walked, {} points", w.town.base(), w.x, w.y, w.z, w.facing, w.frame, w.walked, w.at.back(), w.path.size());
	}
	TileIndex middle = TurnedViewMiddle();
	if (middle == INVALID_TILE) return out + "\n  (the main view is not turned)";
	const Town *best = nullptr;
	for (const Town *town : Town::Iterate()) {
		if (best == nullptr || DistanceManhattan(town->xy, middle) < DistanceManhattan(best->xy, middle)) best = town;
	}
	if (best == nullptr) return out;
	std::vector<Door> doors;
	TownDoors(best, doors);
	out += fmt::format("\n  town {} at {},{}: {} doors onto its roads", best->index.base(), TileX(best->xy), TileY(best->xy), doors.size());
	for (const Door &d : doors) out += fmt::format("\n    house {},{} door {:.1f},{:.1f} road {},{}", TileX(d.house), TileY(d.house), d.door.x, d.door.y, TileX(d.road), TileY(d.road));
	return out;
}
