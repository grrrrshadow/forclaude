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
#include "cab_drive.h"
#include "roadveh.h"
#include "vehicle_base.h"
#include "bridge_map.h"

#include <cmath>
#include <functional>
#include <map>
#include <span>
#include <unordered_map>

#include "safeguards.h"

bool FirstPersonWalkerAt(double &x, double &y);
const Vehicle *FirstPersonRiding();
bool ThreeDWindscreenOf(const Vehicle *v, double &x, double &y, double &z, double &incline);
bool ThreeDDriverEyeOf(VehicleID id, double &x, double &y, double &z);

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

/*
 * Mat run into by the car the player drives from its cab (the player, 19. 11.: "when I drive and run into Mat, let him
 * fly over the roof as in GTA 6, and then he gets up and goes where he needed. Only Mat, the others I do not want to
 * run into"; "the face at the driver, one leg over the roof, so that from the other views it is seen they fly"; slower
 * "just push"): a moment on the windscreen, his face to the driver and his feet over the roof, then thrown over the car
 * turning over, lying a while where he fell, up again and on his way. Slower than THROW_KMH only pushed aside. Only the
 * screen, as the walkers: the game knows nothing of it. The figure has no bones, so he flies stiff, a whole.
 */
constexpr double THROW_KMH = 15.0;        ///< slower than this (km/h) pushed aside, not thrown
double _throw_kmh = THROW_KMH;            ///< as it is now: the rig pushes at any speed ('test3d lenny odstrc <km/h>')
constexpr int THROW_GLASS_TICKS = 10;     ///< the ticks he lies on the windscreen
constexpr int THROW_LIE_TICKS = 60;       ///< lying where he fell
constexpr int THROW_UP_TICKS = 20;        ///< getting up
constexpr int THROW_PUSH_TICKS = 10;      ///< pushed aside
constexpr double THROW_GLASS_DEG = 25.0;  ///< the windscreen's slope over the level: he lies along it, his head to the bonnet
constexpr double THROW_PUSH_M = 1.3;      ///< how far aside a push puts him at least, metres (past the car's side)
constexpr double THROW_BEHIND_M = 1.5;    ///< he lands so far behind the car, metres (as long as it drives on as fast)
constexpr double THROW_GRAVITY = 9.81;    ///< metres a second a second
constexpr double TICK_SECONDS = MILLISECONDS_PER_TICK / 1000.0;

/*
 * Lenny in the way of the car the player drives from its cab (the player, 19. 11.: "and Lenny we could spawn into the
 * way", "Lenny is done, but I do not want to meet him when I do not drive", "at most 2x a drive; I get out, I get in,
 * there may be Lenny again", "Lenny falls on the ground and vanishes", slower "just push"): standing on the road ahead,
 * gesturing at the car; run into, on the glass and over the car as Mat (Throw), gone where he falls. Only the screen,
 * as the walkers. Not in the streets otherwise: his record has no "kde".
 */
constexpr std::string_view LENNY_FIGURE = "lenny";        ///< standing still: flying, pushed
constexpr std::string_view LENNY_GESTURE = "lenny/gesto"; ///< gesturing at the car coming
constexpr int LENNY_PER_DRIVE = 2;                        ///< he comes so often a drive at most
constexpr uint64_t LENNY_FIRST = 555, LENNY_FIRST_MORE = 555; ///< the first time after so many ticks of the drive (15 s), and up to so many more
constexpr uint64_t LENNY_AGAIN = 1110, LENNY_AGAIN_MORE = 1110; ///< again after so many ticks (30 s), and up to so many more
constexpr uint64_t LENNY_RETRY = 37;                      ///< no road ahead: looked for again after so long (a second)
constexpr double LENNY_MIN_KMH = 20.0;                    ///< he comes only to a car going at least so fast
constexpr double LENNY_AHEAD_M[] = {45.0, 40.0, 50.0, 35.0, 55.0}; ///< so far ahead on the road, metres, the first that is road
constexpr double LENNY_GONE_M = 80.0;                     ///< gone when the car is so far (metres), or past him by THROW_BEHIND_M * 10
constexpr uint64_t LENNY_STANDS = 1480;                   ///< gone after standing so long (40 s)

/** The car the player drives from its cab (FirstPersonRiding(), Vehicle::IsCabSteered()), as a throw sees it. */
struct DrivenCar {
	VehicleID id = VehicleID::Invalid();
	double x = 0, y = 0, z = 0;   ///< its middle, world units; its foot, units of height
	double fx = 1, fy = 0;        ///< the way it heads
	float heading = 0;            ///< and in degrees, 0 along +x
	double front = 2.2, half_width = 0.9, roof = 1.45, length = 4.4; ///< its shape, metres as drawn
	double kmh = 0;               ///< its speed
	double step = 0;              ///< how far it went the last tick, world units
};

/** A walker being thrown over or pushed aside by the car (DrivenCar). */
struct Throw {
	enum Phase : uint8_t { NONE, GLASS, FLY, LIE, UP, PUSH, GONE };
	Phase phase = NONE;
	int ticks = 0;                ///< in the phase so far
	VehicleID car = VehicleID::Invalid();
	double side = 0;              ///< his place across the car at the hit, metres, its left (+y) positive
	double x = 0, y = 0, z = 0;   ///< where he is, world units and units of height (his feet)
	double vx = 0, vy = 0, vz = 0; ///< flying: a tick
	double gravity = 0;           ///< flying: units of height a tick a tick
	float facing = 0, pitch = 0, roll = 0; ///< how he is turned, degrees
	float spin = 0;               ///< flying: degrees a tick he turns over
	double from_x = 0, from_y = 0, to_x = 0, to_y = 0; ///< pushed aside: from where to where
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
	Throw thrown;                   ///< run into by the car the player drives (Throw)
	bool lenny = false;             ///< Lenny in the way of the car (Lenny): no way of his own, gone where he falls
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

/** Where the car the player drives was the tick before (DrivenCar::step), by the car. */
std::map<VehicleID, std::pair<double, double>> _driven_were;

/** The car the player drives from its cab now, if one (a road vehicle; no train, the player: "1 vlak ne"); without its step (GetDrivenCar()). */
bool ReadDrivenCar(DrivenCar &c)
{
	const Vehicle *v = FirstPersonRiding();
	if (v == nullptr || v->type != VehicleType::Road || !v->First()->IsCabSteered()) return false;
	v = v->First();
	c.id = v->index;
	/* Driven freely its place to the subunit (a whole unit only every few ticks): where it is, and its step smooth. */
	c.x = v->cab_free ? static_cast<double>(v->cab_px) / CAB_SUBUNIT : v->x_pos;
	c.y = v->cab_free ? static_cast<double>(v->cab_py) / CAB_SUBUNIT : v->y_pos;
	c.z = v->z_pos;
	const int yaw = v->cab_free ? v->cab_yaw : (v->direction != Direction::Invalid ? DirectionToCabYaw(v->direction) : 0);
	const double heading = yaw * 2 * M_PI / CAB_YAW_TURN;
	c.fx = std::cos(heading);
	c.fy = std::sin(heading);
	c.heading = static_cast<float>(heading * 180.0 / M_PI);
	c.kmh = v->GetDisplaySpeed();
	const float scale = WalkerScale();
	if (const ThreeDVehicleObject *o = ThreeDVehicleObjectOf(v); o != nullptr) {
		if (const ThreeDVehicleShape *shape = o->TryGetShape(0); shape != nullptr && shape->x1 > shape->x0) {
			c.front = shape->x1 * scale;
			c.length = (shape->x1 - shape->x0) * scale;
			/* (Its width and height where known: the extent across and up is not kept for every shape.) */
			const float across = std::max({std::abs(shape->y0), std::abs(shape->y1), std::abs(shape->all[2]), std::abs(shape->all[3])});
			if (across > 0.4f) c.half_width = across * scale;
			const float roof = std::max(shape->z1, shape->all[5]);
			if (roof > 0.8f) c.roof = roof * scale;
		}
	}
	return true;
}

/** The car the player drives from its cab now, if one, with its step: once a tick. */
bool GetDrivenCar(DrivenCar &c)
{
	if (!ReadDrivenCar(c)) {
		_driven_were.clear();
		return false;
	}
	auto were = _driven_were.find(c.id);
	const double step = were != _driven_were.end() ? std::hypot(c.x - were->second.first, c.y - were->second.second) : 0.0;
	_driven_were[c.id] = {c.x, c.y};
	/* Smoothed: on the road its place moves by whole units. */
	static double smooth = 0;
	smooth = were != _driven_were.end() ? smooth * 0.8 + step * 0.2 : step;
	c.step = smooth;
	return true;
}

/** Where a point of the car's own frame is in the world (metres ahead and to its left), world units. */
void CarPoint(const DrivenCar &c, double ahead, double left, double &x, double &y)
{
	const double a = ahead * OBJECT_UNITS_PER_METRE, l = left * OBJECT_UNITS_PER_METRE;
	x = c.x + c.fx * a - c.fy * l;
	y = c.y + c.fy * a + c.fx * l;
}

/**
 * On the windscreen of the car: his front (+x) to the glass and the driver behind it, his head (+z) down along it to
 * the bonnet, his feet back up it over the roof (the player: "the face at the driver, one leg over the roof"). Where
 * its model has the glass before the driver's eye (ThreeDWindscreenOf(): "each car has its windscreen differently
 * high"); a car without a model before the eye in the driver's head (it has no glass); else over the edge of its
 * roof as its shape has it, on the driver's side.
 */
void OnGlass(Throw &t, const DrivenCar &c)
{
	t.facing = c.heading;
	t.roll = 0;
	double gx, gy, gz, incline = THROW_GLASS_DEG;
	const Vehicle *v = Vehicle::GetIfValid(c.id);
	bool glass = v != nullptr && ThreeDWindscreenOf(v, gx, gy, gz, incline);
	t.pitch = static_cast<float>(-(90.0 + incline));
	/* Without a model no glass: before the eye in the driver's head, a little over it. */
	if (!glass && ThreeDDriverEyeOf(c.id, gx, gy, gz)) {
		gx += c.fx * 0.65 * OBJECT_UNITS_PER_METRE;
		gy += c.fy * 0.65 * OBJECT_UNITS_PER_METRE;
		gz += 0.1 * OBJECT_HEIGHT_PER_METRE;
		glass = true;
	}
	if (glass) {
		/* His face there, his feet back up the glass from it. */
		const double face = 1.55 * WalkerScale(), a = incline * M_PI / 180.0;
		t.x = gx - c.fx * face * std::cos(a) * OBJECT_UNITS_PER_METRE;
		t.y = gy - c.fy * face * std::cos(a) * OBJECT_UNITS_PER_METRE;
		t.z = gz + face * std::sin(a) * OBJECT_HEIGHT_PER_METRE;
		return;
	}
	const double roof_edge = c.front - 0.42 * c.length;
	CarPoint(c, roof_edge, std::min(0.35, c.half_width * 0.5), t.x, t.y);
	t.z = c.z + (c.roof + 0.3) * OBJECT_HEIGHT_PER_METRE;
}

/** Whether the car runs into him now: he is just before its front, within its width, as high. */
bool HitBy(const Walker &w, const DrivenCar &c)
{
	if (w.stage == 1 || w.thrown.phase != Throw::NONE || c.kmh < 2) return false;
	const double dx = w.x - c.x, dy = w.y - c.y;
	const double ahead = (dx * c.fx + dy * c.fy) / OBJECT_UNITS_PER_METRE, left = (-dx * c.fy + dy * c.fx) / OBJECT_UNITS_PER_METRE;
	if (std::abs(w.zf - c.z) > 2.0 * OBJECT_HEIGHT_PER_METRE) return false;
	return ahead >= c.front - 0.6 && ahead <= c.front + 0.4 && std::abs(left) <= c.half_width + 0.25;
}

/** Run into: thrown over the car, or pushed aside when it is slow. */
void StartThrow(Walker &w, const DrivenCar &c)
{
	Throw &t = w.thrown;
	t = {};
	t.car = c.id;
	t.side = (-(w.x - c.x) * c.fy + (w.y - c.y) * c.fx) / OBJECT_UNITS_PER_METRE;
	if (c.kmh < _throw_kmh) {
		t.phase = Throw::PUSH;
		t.from_x = t.x = w.x;
		t.from_y = t.y = w.y;
		/* Aside out of its way: at least so far, and past its side (a lorry's half width is more than the push). */
		const double away = (t.side >= 0 ? 1.0 : -1.0) * std::max(THROW_PUSH_M, c.half_width + 0.4 - std::abs(t.side)) * OBJECT_UNITS_PER_METRE;
		t.to_x = w.x - c.fy * away;
		t.to_y = w.y + c.fx * away;
		t.z = w.zf;
		t.facing = w.facing;
		return;
	}
	t.phase = Throw::GLASS;
	OnGlass(t, c);
}

/** Back on his way from where he is: straight to his way again, then along the rest of it. */
void Resume(Walker &w, const ThreeDVehicleObject *fig)
{
	size_t i = 1;
	while (i + 1 < w.path.size() && w.at[i] < w.walked) i++;
	std::vector<Point2> path{{w.x, w.y}};
	path.insert(path.end(), w.path.begin() + i, w.path.end());
	w.path = std::move(path);
	Measure(w);
	w.walked = 0;
	Place(w, fig);
}

/**
 * A tick of a walker being thrown (Throw): his place and how he is turned; false when it is over (he walks on).
 * @param c the car the player drives now, nullptr none
 */
bool Thrown(Walker &w, const ThreeDVehicleObject *fig, const DrivenCar *c)
{
	Throw &t = w.thrown;
	t.ticks++;
	switch (t.phase) {
		case Throw::GLASS: {
			/* On the glass as long as the car is there; then off over it. */
			if (c != nullptr && c->id == t.car) OnGlass(t, *c);
			if (t.ticks < THROW_GLASS_TICKS && c != nullptr && c->id == t.car) break;
			/* Thrown up and over: as far forward as the car goes but falling behind it, to land THROW_BEHIND_M behind
			 * its back as long as it drives on as fast (it may not: then wherever), turning over backwards to land on
			 * his back. */
			const double kmh = c != nullptr ? c->kmh : THROW_KMH;
			const double up = std::clamp(0.3 * kmh / 3.6, 2.5, 7.0); // metres a second
			const double h = (t.z - (c != nullptr ? c->z : t.z - OBJECT_HEIGHT_PER_METRE)) / OBJECT_HEIGHT_PER_METRE;
			const double seconds = (up + std::sqrt(up * up + 2 * THROW_GRAVITY * std::max(0.0, h))) / THROW_GRAVITY;
			const double ticks = std::max(1.0, seconds / TICK_SECONDS);
			t.gravity = THROW_GRAVITY * OBJECT_HEIGHT_PER_METRE * TICK_SECONDS * TICK_SECONDS;
			t.vz = up * OBJECT_HEIGHT_PER_METRE * TICK_SECONDS;
			double fwd = 0;
			if (c != nullptr) {
				/* From where he lies on it to behind its back. */
				const double on = ((t.x - c->x) * c->fx + (t.y - c->y) * c->fy) / OBJECT_UNITS_PER_METRE;
				const double back_m = std::max(0.5, on - (c->front - c->length)) + THROW_BEHIND_M;
				const double lag = std::clamp(back_m * OBJECT_UNITS_PER_METRE / (ticks * std::max(c->step, 0.01)), 0.1, 0.9);
				fwd = c->step * (1.0 - lag);
				t.vx = c->fx * fwd;
				t.vy = c->fy * fwd;
			}
			/* From lying on the glass (-115) round backwards once and a bit to his back (-630 = 90). */
			t.spin = static_cast<float>((-630.0 - t.pitch) / ticks);
			t.phase = Throw::FLY;
			t.ticks = 0;
			break;
		}
		case Throw::FLY: {
			t.x += t.vx;
			t.y += t.vy;
			t.z += t.vz;
			t.vz -= t.gravity;
			t.pitch += t.spin;
			t.roll = static_cast<float>(25.0 * std::sin(t.ticks * 0.35));
			const double ground = GroundZF(t.x, t.y, false);
			if (t.vz < 0 && t.z <= ground && w.lenny) {
				/* Lenny falls on the ground and is gone (the player: "leny dopadne na zem a zmizí"). */
				MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
				t.phase = Throw::GONE;
				return false;
			}
			if (t.vz < 0 && t.z <= ground) {
				/* Down on his back, a little above the ground (his body's depth). */
				t.z = ground + 0.15 * OBJECT_HEIGHT_PER_METRE;
				t.pitch = 90;
				t.roll = 0;
				t.phase = Throw::LIE;
				t.ticks = 0;
			}
			break;
		}
		case Throw::LIE:
			if (t.ticks >= THROW_LIE_TICKS) {
				t.phase = Throw::UP;
				t.ticks = 0;
			}
			break;
		case Throw::UP: {
			const double f = std::min(1.0, static_cast<double>(t.ticks) / THROW_UP_TICKS);
			t.pitch = static_cast<float>(90.0 * (1.0 - f));
			const double ground = GroundZF(t.x, t.y, false);
			t.z = ground + 0.15 * OBJECT_HEIGHT_PER_METRE * (1.0 - f);
			if (f >= 1.0) {
				w.x = t.x;
				w.y = t.y;
				t = {};
				Resume(w, fig);
				return false;
			}
			break;
		}
		case Throw::PUSH: {
			const double f = std::min(1.0, static_cast<double>(t.ticks) / THROW_PUSH_TICKS);
			t.x = t.from_x + (t.to_x - t.from_x) * f;
			t.y = t.from_y + (t.to_y - t.from_y) * f;
			t.z = GroundZF(t.x, t.y, false);
			if (f >= 1.0) {
				w.x = t.x;
				w.y = t.y;
				t = {};
				if (w.lenny) {
					/* Lenny has no way: he stands where he was pushed. */
					w.zf = GroundZF(w.x, w.y, false);
					w.z = static_cast<int>(std::lround(w.zf));
					MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
					Redraw(w, fig);
					return false;
				}
				Resume(w, fig);
				return false;
			}
			break;
		}
		case Throw::NONE:
		case Throw::GONE: return false;
	}
	/* Where he is drawn: as he walks, the map where he was drawn last and where he is now (Redraw()). */
	w.x = t.x;
	w.y = t.y;
	w.zf = t.z;
	w.z = static_cast<int>(std::lround(t.z));
	w.facing = t.facing;
	const int ix = std::clamp<int>(static_cast<int>(w.x), 0, Map::MaxX() * TILE_SIZE), iy = std::clamp<int>(static_cast<int>(w.y), 0, Map::MaxY() * TILE_SIZE);
	w.tile = TileVirtXY(ix, iy);
	MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
	Redraw(w, fig);
	return true;
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

/** Lenny on the road (Lenny): none, or one. */
std::vector<Walker> _lennies;
/** The drive Lenny comes to (Lenny): the car, how often he came on this drive, when he may come next and since when he stands. */
struct LennyDrive {
	VehicleID car = VehicleID::Invalid();
	int met = 0;
	uint64_t next = 0;
	uint64_t standing_since = 0;
};
LennyDrive _lenny_drive;

/** Lenny's figure, if it is there. */
const ThreeDVehicleObject *LennyFigure()
{
	return GetThreeDFigure(LENNY_FIGURE);
}

/** Lenny gone from the road, his place on the map drawn again. */
void LennyGone()
{
	for (const Walker &w : _lennies) MarkStandingDirty(LennyFigure(), w.drawn_x, w.drawn_y, w.drawn_z, false);
	_lennies.clear();
}

/**
 * Where on the road ahead of the car Lenny stands so many metres off: the road all the way to there (plain road tiles,
 * not a crossing, a stop or a bridge's end), the ground there as high as the car's (not under its bridge, not over its
 * tunnel); false when not.
 */
bool LennyPlace(const DrivenCar &c, double metres, double &x, double &y, double &z)
{
	x = c.x + c.fx * metres * OBJECT_UNITS_PER_METRE;
	y = c.y + c.fy * metres * OBJECT_UNITS_PER_METRE;
	if (x < TILE_SIZE || y < TILE_SIZE || x >= Map::MaxX() * TILE_SIZE || y >= Map::MaxY() * TILE_SIZE) return false;
	for (double f : {0.25, 0.5, 0.75, 1.0}) {
		const TileIndex t = TileVirtXY(static_cast<uint>(c.x + (x - c.x) * f), static_cast<uint>(c.y + (y - c.y) * f));
		if (!IsValidTile(t) || !IsNormalRoadTile(t) || !HasTileRoadType(t, RoadTramType::Road) || IsBridgeAbove(t)) return false;
	}
	z = GroundZF(x, y, false);
	return std::abs(z - c.z) <= 1.5 * OBJECT_HEIGHT_PER_METRE;
}

/** Lenny on the road ahead of the car now, if there is road; whether he came (counted for the drive). */
bool SpawnLenny(const DrivenCar &c, std::span<const double> ahead)
{
	const ThreeDVehicleObject *fig = LennyFigure();
	if (fig == nullptr || !_lennies.empty() || _lenny_drive.met >= LENNY_PER_DRIVE) return false;
	for (double metres : ahead) {
		double x, y, z;
		if (!LennyPlace(c, metres, x, y, z)) continue;
		Walker w{};
		w.lenny = true;
		w.x = x;
		w.y = y;
		w.zf = z;
		w.z = static_cast<int>(std::lround(z));
		w.tile = TileVirtXY(static_cast<uint>(x), static_cast<uint>(y));
		/* Facing the car coming, gesturing at it. */
		w.facing = static_cast<float>(std::remainder(c.heading + 180.0, 360.0));
		w.frame = 0;
		Redraw(w, fig);
		_lennies.push_back(std::move(w));
		_lenny_drive.met++;
		_lenny_drive.standing_since = TimerGameTick::counter;
		Debug(misc, 1, "3D: Lenny on the road {:.0f} m ahead of the car {} ({} of {} this drive)", metres, c.id.base(), _lenny_drive.met, LENNY_PER_DRIVE);
		return true;
	}
	return false;
}

/**
 * A tick of Lenny (Lenny): none while the player does not drive a car from its cab, a new drive when he gets in again
 * (or into another); on the road ahead now and then, at most LENNY_PER_DRIVE times a drive; run into, thrown as Mat
 * and gone where he falls; gone too when the car is far or past him, or after a while.
 * @param c the car the player drives, nullptr none
 */
void LennyTick(const DrivenCar *c)
{
	const uint64_t now = TimerGameTick::counter;
	if (c == nullptr) {
		LennyGone();
		_lenny_drive = {};
		return;
	}
	if (c->id != _lenny_drive.car) {
		LennyGone();
		_lenny_drive = {c->id, 0, now + LENNY_FIRST + InteractiveRandomRange(static_cast<uint32_t>(LENNY_FIRST_MORE)), 0};
	}
	const ThreeDVehicleObject *fig = LennyFigure();
	if (fig == nullptr) return;
	if (!_lennies.empty()) {
		Walker &w = _lennies.front();
		if (w.thrown.phase != Throw::NONE || HitBy(w, *c)) {
			if (w.thrown.phase == Throw::NONE) StartThrow(w, *c);
			if (!Thrown(w, fig, c) && w.thrown.phase == Throw::GONE) {
				_lennies.clear();
				_lenny_drive.next = now + LENNY_AGAIN + InteractiveRandomRange(static_cast<uint32_t>(LENNY_AGAIN_MORE));
			}
			return;
		}
		/* Gone when the car is far, or past him, or after a while. */
		const double dx = w.x - c->x, dy = w.y - c->y;
		const double ahead = (dx * c->fx + dy * c->fy) / OBJECT_UNITS_PER_METRE;
		if (std::hypot(dx, dy) > LENNY_GONE_M * OBJECT_UNITS_PER_METRE || ahead < -10.0 * THROW_BEHIND_M || now - _lenny_drive.standing_since > LENNY_STANDS) {
			LennyGone();
			_lenny_drive.next = now + LENNY_AGAIN + InteractiveRandomRange(static_cast<uint32_t>(LENNY_AGAIN_MORE));
			return;
		}
		/* Gesturing at the car: the frames of the pose; the map drawn again only when its fewer poses change (ThreeDMapPose()). */
		if (const ThreeDVehicleObject *g = GetThreeDFigure(LENNY_GESTURE); g != nullptr && g->frames > 1) {
			const int frame = static_cast<int>((now / g->frame_ticks) % g->frames);
			if (frame != w.frame) {
				const bool map_pose = ThreeDMapPose(LENNY_GESTURE, frame) != ThreeDMapPose(LENNY_GESTURE, std::max(0, w.frame));
				w.frame = frame;
				if (map_pose) {
					MarkStandingDirty(fig, w.drawn_x, w.drawn_y, w.drawn_z, false);
					Redraw(w, fig);
				}
			}
		}
		return;
	}
	if (_lenny_drive.met >= LENNY_PER_DRIVE || now < _lenny_drive.next || c->kmh < LENNY_MIN_KMH) return;
	if (!SpawnLenny(*c, LENNY_AHEAD_M)) _lenny_drive.next = now + LENNY_RETRY;
}

/** Every tick: the walkers walk on; at their doors they go in; the towns near the view get theirs. */
const IntervalTimer<TimerGameTick> _walkers_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	/* The car the player drives from its cab: they are thrown over it when it runs into them (Throw), Lenny comes into
	 * its way (Lenny). */
	DrivenCar car;
	const bool driving = GetDrivenCar(car);
	LennyTick(driving ? &car : nullptr);
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
		if (w.thrown.phase != Throw::NONE || (driving && HitBy(w, car))) {
			if (w.thrown.phase == Throw::NONE) StartThrow(w, car);
			Thrown(w, fig, driving ? &car : nullptr);
			++it;
			continue;
		}
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
		/* In its state "jizda" when its record has one (its beacon off, the Tatra 613 VB as the one bought; the player,
		 * 18. 11.: "how is it not the same as the Tatra from the depot"), else as it is. */
		AddThreeDObjectToViewport(d.car, scale, static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)), d.z, d.facing, zoom, IsNightNow(), d.car->StateNamed("jizda"));
	}
}

/** The cars of the game's own driving about in the view from the eyes: their objects among its figures (AddCabCar()). */
void AddThreeDDriversToEye()
{
	const float scale = WalkerScale();
	for (const Driver &d : _drivers) {
		if (d.path.empty() || d.tile == INVALID_TILE) continue;
		AddCabCar(d.car, scale, d.x, d.y, d.zf, d.facing, IsNightNow(), d.car->StateNamed("jizda"));
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

/** A walker on the turned view being drawn (AddThreeDWalkersToViewport()): Mat, or Lenny. */
static void AddWalkerToViewport(const Walker &w, ZoomLevel zoom, float scale)
{
	if (w.stage == 1) return; // inside a depot or a lighthouse
	/* Mat, or Lenny gesturing at the car, still when thrown or pushed. */
	const bool thrown = w.thrown.phase != Throw::NONE;
	const std::string_view name = !w.lenny ? WALKER_FIGURE : thrown ? LENNY_FIGURE : LENNY_GESTURE;
	/* (Lenny's gesture on the map in its fewer poses, ThreeDMapPose().) */
	const int frame = w.lenny ? (thrown ? 0 : ThreeDMapPose(name, std::max(0, w.frame))) : std::max(0, w.frame);
	/* Where it was drawn again last (Walker::drawn_x): as the screen has it. */
	const int x = static_cast<int>(std::lround(w.drawn_x)), y = static_cast<int>(std::lround(w.drawn_y));
	const TileIndex tile = TileVirtXY(std::clamp<int>(x, 0, Map::MaxX() * TILE_SIZE), std::clamp<int>(y, 0, Map::MaxY() * TILE_SIZE));
	if (thrown && w.thrown.phase != Throw::PUSH) {
		/* Thrown over the car (Throw): turned every way, in a box about him as long as he is. */
		const int r = static_cast<int>(std::lround(1.0 * scale * OBJECT_UNITS_PER_METRE)), tall = static_cast<int>(std::lround(2.0 * scale * OBJECT_HEIGHT_PER_METRE));
		const std::array<int, 6> box{x - r, y - r, w.drawn_z - tall, x + r, y + r, w.drawn_z + tall};
		AddThreeDFigureToViewportInBox(name, scale, x, y, w.drawn_z, w.drawn_facing, zoom, tile, 0, frame, 0, &box, 0.0f,
				static_cast<int>(std::lround(w.thrown.pitch)), static_cast<int>(std::lround(w.thrown.roll)));
		return;
	}
	AddThreeDFigureToViewport(name, scale, x, y, w.drawn_z, w.drawn_facing, zoom, tile, 0, frame);
}

/** Put the walkers on the turned view being drawn (after its vehicles; ViewportDoDrawTurned()): Mat and Lenny. */
void AddThreeDWalkersToViewport(ZoomLevel zoom)
{
	if ((_walkers.empty() && _lennies.empty()) || IsTransparencySet(TransparencyOption::Houses)) return;
	const float scale = WalkerScale();
	for (const Walker &w : _walkers) AddWalkerToViewport(w, zoom, scale);
	for (const Walker &w : _lennies) AddWalkerToViewport(w, zoom, scale);
}

/** A walker in the view from the eyes at its true size (AddThreeDWalkersToEye()): Mat, or Lenny. */
static void AddWalkerToEye(const Walker &w, float scale)
{
	if (w.stage == 1) return; // inside a depot or a lighthouse
	const bool thrown = w.thrown.phase != Throw::NONE;
	const std::string_view name = !w.lenny ? WALKER_FIGURE : thrown ? LENNY_FIGURE : LENNY_GESTURE;
	const int frame = w.lenny && thrown ? 0 : std::max(0, w.frame);
	if (thrown && w.thrown.phase != Throw::PUSH) {
		/* Thrown over the car (Throw): on its windscreen his face to the driver, then over it turning over. On the
		 * glass where it is in this frame, the eye in it (the car went on since the tick). */
		Throw t = w.thrown;
		if (DrivenCar c; t.phase == Throw::GLASS && ReadDrivenCar(c) && c.id == t.car) {
			OnGlass(t, c);
		} else {
			t.x = w.x;
			t.y = w.y;
			t.z = w.zf;
			t.facing = w.facing;
		}
		AddCabTumbler(name, scale, t.x, t.y, t.z, t.facing, frame, t.pitch, t.roll);
		return;
	}
	/* Where it is and on the slope's plane, not at whole units (the player, test4: the stairs). */
	AddCabFigure(name, scale, w.x, w.y, w.zf, w.facing, frame);
}

/** The walkers in the view from the eyes, among its figures at their true size (AddCabFigure()): Mat and Lenny. */
void AddThreeDWalkersToEye(float scale)
{
	for (const Walker &w : _walkers) AddWalkerToEye(w, scale);
	for (const Walker &w : _lennies) AddWalkerToEye(w, scale);
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

/**
 * Whether a walker lies on the windscreen of the car the player drives, or flies over it (Throw): the view from its cab
 * draws what is near then (its near plane 2 units off, he was cut off on the glass).
 */
bool ThreeDThrownNearCab()
{
	auto near = [](const Walker &w) { return w.thrown.phase == Throw::GLASS || w.thrown.phase == Throw::FLY; };
	return std::ranges::any_of(_walkers, near) || std::ranges::any_of(_lennies, near);
}

/** The rig ('test3d lenny [metres]'): Lenny on the road so far ahead of the car the player drives now, as when he comes (counted for the drive). */
std::string RigSpawnLenny(double metres)
{
	/* 'lenny odstrc <km/h>': pushed aside slower than that (THROW_KMH), the rig's car going faster. */
	if (metres < 0) {
		_throw_kmh = -metres;
		return fmt::format("odstrci pod {:.0f} km/h", _throw_kmh);
	}
	DrivenCar c;
	if (!ReadDrivenCar(c)) return "nikdo neridi";
	if (_lenny_drive.car != c.id) return "jizda jeste nezacala (tik)";
	const double ahead[] = {metres};
	if (!SpawnLenny(c, ahead)) {
		/* Why not: the road on the way to there, how high the ground is there. */
		std::string why;
		const double x = c.x + c.fx * metres * OBJECT_UNITS_PER_METRE, y = c.y + c.fy * metres * OBJECT_UNITS_PER_METRE;
		for (double f : {0.25, 0.5, 0.75, 1.0}) {
			const TileIndex t = TileVirtXY(static_cast<uint>(c.x + (x - c.x) * f), static_cast<uint>(c.y + (y - c.y) * f));
			why += fmt::format(" {},{}:{}{}", TileX(t), TileY(t), IsNormalRoadTile(t) ? "silnice" : "ne", IsBridgeAbove(t) ? "+most" : "");
		}
		return fmt::format("neprisel (uz {} z {}, stoji {}, figura {};{} zem {:.1f} auto {:.1f})", _lenny_drive.met, LENNY_PER_DRIVE, _lennies.size(), LennyFigure() != nullptr, why, GroundZF(x, y, false), c.z);
	}
	const Walker &w = _lennies.front();
	return fmt::format("lenny na {:.1f},{:.1f},{:.1f} smer {:.0f}, {} z {}", w.x, w.y, w.zf, w.facing, _lenny_drive.met, LENNY_PER_DRIVE);
}

/** The rig ('test3d srazit'): where the first walker is and which way he walks, world units and degrees; false when none. */
bool ThreeDWalkerHeading(double &x, double &y, float &facing)
{
	for (const Walker &w : _walkers) {
		if (w.stage == 1) continue;
		x = w.x;
		y = w.y;
		facing = w.facing;
		return true;
	}
	return false;
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
		if (w.thrown.phase != Throw::NONE) out += fmt::format(", thrown {} tick {} z {:.1f} pitch {:.0f} roll {:.0f}", to_underlying(w.thrown.phase), w.thrown.ticks, w.thrown.z, w.thrown.pitch, w.thrown.roll);
	}
	out += fmt::format("\n  Lenny {} of {} this drive (car {}), next at tick {} (now {})", _lenny_drive.met, LENNY_PER_DRIVE, _lenny_drive.car.base(), _lenny_drive.next, TimerGameTick::counter);
	for (const Walker &w : _lennies) {
		out += fmt::format("\n  lenny at {:.1f},{:.1f},{} facing {:.0f} frame {}", w.x, w.y, w.z, w.facing, w.frame);
		if (w.thrown.phase != Throw::NONE) out += fmt::format(", thrown {} tick {} z {:.1f} pitch {:.0f} roll {:.0f}", to_underlying(w.thrown.phase), w.thrown.ticks, w.thrown.z, w.thrown.pitch, w.thrown.roll);
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
