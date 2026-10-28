/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_animals.cpp The dogs, the animals, the ghosts and the sellers of dogs about the walker (the player,
 * 18. 10.: "the dogs that will not walk with me will not be sold, they will stand in the streets of the towns and lie
 * before the doors of the houses", "animals about the temperate towns", "the desert towns get giraffes, elephants, and
 * monkeys in the streets and on the houses", "the mushroom warrior at night about the clay towns"; "the sellers turn
 * up at random, they stand, or walk when they can; each has only three to five animals on offer"; "the ghosts attack,
 * the ghost I have to provoke; the elephant, the lion I have to provoke, start a fight, else they are peaceful and do
 * not attack"; the colleague's hra/3d/PSI-ZVIRATA-HOUBY-PRO-HRU.md).
 *
 * Where each is found, how it takes the walker and what it looks like is its index's (ThreeDAnimal); here they only
 * live: only the screen, never saved, about where the life of the towns goes on (ThreeDLifeMiddle(): the walker of the
 * view from the first person, else the middle of the turned main view), as the technologies of OpenMW keep only the
 * world about the player. A few of each kind of place are kept there, one more now and then; those left far behind go.
 */

#include "stdafx.h"
#include "three_d_animals.h"
#include "three_d_objects.h"
#include "three_d_navigation.h"
#include "three_d_vehicle_object.h"
#include "three_d_walkers.h"
#include "three_d.h"
#include "cab_view.h"
#include "openttd.h"
#include "map_func.h"
#include "tile_map.h"
#include "landscape.h"
#include "viewport_func.h"
#include "town.h"
#include "industry.h"
#include "industry_map.h"
#include "clear_map.h"
#include "house.h"
#include "rail_map.h"
#include "track_func.h"
#include "tile_cmd.h"
#include "settings_type.h"
#include "company_base.h"
#include "company_func.h"
#include "core/random_func.hpp"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "debug.h"

#include <cmath>
#include <map>

#include "safeguards.h"

/** The walker of the view from the first person: where it stands, world units; false when the view is not open (first_person_gui.cpp). */
bool FirstPersonWalkerAt(double &x, double &y);
double FirstPersonWalkerHeading();
/** Whether one can stand on the ground at a world point, as the walker can (first_person_gui.cpp). */
bool FirstPersonGroundWalkable(double x, double y);

namespace {

constexpr uint REACH = 14;                 ///< tiles about the middle they come to
constexpr uint GONE = 30;                  ///< and go past, tiles (Manhattan)
constexpr uint TOWN_REACH = 24;            ///< towns this near the middle have theirs (tiles, Manhattan)
constexpr uint STREET_KEPT = 5;            ///< so many in the streets about the middle
constexpr uint DOOR_KEPT = 2;              ///< before the doors
constexpr uint ROOF_KEPT = 4;              ///< on the roofs
constexpr uint ROUND_KEPT = 5;             ///< in the land about the towns
constexpr uint FOREST_KEPT = 5;            ///< in the forests about the middle (the game told the colleague: 4 to 6 a forest)
constexpr uint FOREST_GHOSTS_KEPT = 2;     ///< and at night the ghosts of the forest with a clearing ("noc_postavy")
constexpr uint GHOSTS_KEPT = 3;            ///< ghosts ("kdy": at night, by day, or always)
constexpr uint RAIL_KEPT = 3;              ///< and ghosts along the railway lines about the middle, besides
constexpr double RAIL_RIDE_M = 0.05;       ///< one riding the rails without a pose of walking goes so far a tick, metres
/** The one who rides the rails as a little engine (the player, 24. 10.: "only Billy rides the rails"): no other ghost. */
constexpr std::string_view RAIL_RIDER = "billy_tricykl";
/** The ghost in its sheet of each church about the middle (the player, 25. 10.: "more of the ghost in the sheet about
 * the church, one to each church; it will vanish, go into the church and about it"). */
constexpr std::string_view CHURCH_GHOST = "strasidlo_ulice";
constexpr uint CHURCHES_KEPT = 4;          ///< at most so many churches about the middle have theirs
constexpr double CHURCH_ROUND_TILES = 0.7; ///< it goes about its church so far out from its walls, tiles
constexpr uint64_t CHURCH_INSIDE_TICKS = 600; ///< and is in it, or gone, at least so long (and as long again at most)
constexpr uint SELLERS_KEPT = 1;           ///< sellers of dogs
/** The dogs of the farms (the player, 27. 10.: "put the little dogs on the farm"): the true dogs of the streets, not the
 * cats, the foxes, the wolves the index has as dogs too. */
constexpr std::string_view FARM_DOGS[] = {"pes_lovecky", "pes_hnedy", "pes_veseli", "pes_ulicni", "pes_nafoceny", "pes_roztomily", "pes_maly", "baset", "malamut", "jack_russell", "bulmastif", "bulterier"};
constexpr uint FARM_DOGS_KEPT = 2;         ///< so many about each farm's yard ...
constexpr uint FARMS_KEPT = 4;             ///< ... of so many farms about the middle at most
constexpr double YARD_DOG_M = 0.3;         ///< a dog of a yard keeps so far from the walls of its buildings, metres
/** And by the fences of the pastures (the player, 27. 10.: "on the farm by the paddock too, by the sheep, and to guard
 * the cows, the piglets"): one about each, going along its fence outside, looking in at the animals. */
constexpr uint PASTURES_KEPT = 4;          ///< so many pastures about the middle have theirs at most
constexpr double PASTURE_OUT_M = 0.8;      ///< it goes so far out from the fence, metres
constexpr uint64_t SELLER_TICKS = 20000;   ///< a seller stays so long (about nine minutes) ...
constexpr uint64_t SELLER_AWAY_TICKS = 4000; ///< ... and the next comes after about so long
constexpr uint64_t DEAD_TICKS = 600;       ///< a fallen one lies so long
constexpr double CHASE_M = 30.0;           ///< one fighting goes for the walker this near, metres
constexpr double GIVE_UP_M = 50.0;         ///< and gives up this far
constexpr double GAP_M = 1.0;              ///< and strikes from its body this far, metres
constexpr double FLEE_M = 25.0;            ///< one struck that does not fight runs this far away
constexpr double WANDER_TILES = 2.5;       ///< one wandering keeps this near its place
constexpr double FOLLOW_M = 2.0;           ///< the walker's dog keeps this far beside it
constexpr double AHEAD_M = 5.0;            ///< and this far before it, the walker walking (PAR14-PRO-HRU.md: 4-6 m, to be seen)
constexpr double NEAR_REST_M = 10.0;       ///< resting, it stays where it is this near the walker standing
constexpr uint64_t STILL_TICKS = 20;       ///< the walker not moving so long: standing
constexpr double PAVEMENT = 5.5;           ///< the pavement from the middle of a road tile, world units (as Mat's)
constexpr float EYE_OF_MAP = 0.5f;         ///< the eye sees them at their true size, the map at "meritko" (2 as the girls)

/** Where one lives. */
enum class Place : uint8_t {
	Street, ///< in a street of a town
	Door,   ///< before a door
	Roof,   ///< on a roof
	Round,  ///< in the land about a town
	Ghost,  ///< a ghost out at night
	Rail,   ///< a ghost along a railway line, going along its rails (the player, 23. 10.: "more ghosts by the tracks; Billy rides along the rails")
	Seller, ///< a seller of dogs in a street
	Pet,    ///< the walker's dog, walking with it
	Forest, ///< in a forest, always (the player, 24. 10.: "trees, mushrooms and the little animals are to be in the forest all the time")
	ForestNight, ///< a ghost of the forest with a clearing at night, at its places ("noc_postavy")
	Church, ///< the ghost in its sheet of a church, about it and in it (CHURCH_GHOST)
	Farm,   ///< a dog of a farm, about its yard (FARM_DOGS)
	Pasture, ///< a dog of a farm by the fence of a pasture, guarding its animals (FARM_DOGS)
};

enum class State : uint8_t {
	Idle,   ///< standing, sitting, lying
	Wander, ///< walking about its place
	Chase,  ///< going for the walker
	Attack, ///< striking
	Flee,   ///< running from the walker
	Follow, ///< the walker's dog going after it
	Dying,  ///< falling
	Dead,   ///< lying, gone after a while
};

struct Beast {
	std::string kind;      ///< its ThreeDAnimal
	Place place = Place::Street;
	State state = State::Idle;
	std::string pose;      ///< its pose ("" the figure as it stands)
	uint64_t pose_start = 0;
	std::string from_pose; ///< the pose it was in before (with bones its pose passes over into this one)
	int from_frame = 0;
	double pace = 1;       ///< how fast its pose goes
	uint64_t until = 0;    ///< when its state ends
	uint64_t born = 0;
	double x = 0, y = 0;   ///< world units
	double up = 0;         ///< over the ground, height units (on a roof; the flying dog)
	double home_x = 0, home_y = 0; ///< its place
	double heading = 0;    ///< radians
	double tx = 0, ty = 0; ///< where it goes
	int health = 1;
	bool angry = false;    ///< provoked: it fights
	bool struck = false;   ///< its blow of this attack dealt
	int frame = 0;
	int z = 0;             ///< height units, drawn at
	std::vector<std::string> offer; ///< a seller's dogs
	std::vector<NavPoint> way;      ///< its way round what stands between (NavFindWay())
	size_t way_at = 0;
	double way_tx = 0, way_ty = 0;
	uint64_t way_made = 0;
	std::string next_pose; ///< the pose it goes into when the one it is in (sitting down, getting up) is done
	TileIndex rail_tile = INVALID_TILE;       ///< riding the rails (Place::Rail): the tile it rides through ...
	Trackdir rail_td = Trackdir::Invalid;     ///< ... along this track of it, to its far edge
	TileIndex church = INVALID_TILE;          ///< its church's north tile (Place::Church)
	IndustryID farm = IndustryID::Invalid();  ///< its farm (Place::Farm)
	TileIndex pasture = INVALID_TILE;         ///< its pasture's north tile (Place::Pasture)
	bool going_in = false;                    ///< on its way to the church's door, to go in
	bool inside = false;                      ///< in the church, or vanished: not seen, not hit, till "until"
	bool out_at_door = false;                 ///< and it comes out of the door (else it appears about the church)
};

std::vector<Beast> _beasts;
bool _walker_moving = false;      ///< the walker walked a moment ago (its dog goes before it; standing, it rests)
double _walker_heading = 0;       ///< the way the walker faces, radians
double _walker_last_x = 0, _walker_last_y = 0; ///< where the walker was the tick before
uint64_t _walker_moved = 0;       ///< the tick the walker last moved
uint _damage = 0;                 ///< the health they took from the walker since the view last asked
uint64_t _next_seller = 0;        ///< when a seller may come again
std::vector<ThreeDDogSeller> _sellers_seen; ///< ThreeDDogSellerNear(): the sellers as they are now

/*
 * The mushrooms to pick (the player, 18. 10.: "the mushrooms will be picked about the clay towns, the glowing ones only
 * at night"): on the land of the towns of their theme, where nothing is built, one on a tile in so many, where and
 * which by the tile -- the same each time it is looked at, nothing kept; only those picked are kept, till they grow
 * again. At night the glowing one of each.
 */
constexpr uint MUSHROOM_ONE_IN = 3;          ///< a tile of the land in so many has one
constexpr int MUSHROOM_SEEN_TILES = 3;       ///< seen this many tiles about the eye
/** How much bigger than the colleague's true size (5-10 cm) they grow (the player, #287: "the mushrooms are too small,
 * make them much bigger"): a quarter to half a metre. */
constexpr float MUSHROOM_SCALE = 5.0f;
constexpr uint64_t MUSHROOM_REGROW_TICKS = 74 * 30; ///< a picked one grows again after about a month
std::map<uint64_t, uint64_t> _mushrooms_picked; ///< the mushrooms picked (MushroomAt::key), and when

/** A mushroom where it grows. */
struct MushroomAt {
	uint64_t key = 0;    ///< which: its tile, and in the forest with a clearing its place there besides
	const ThreeDMushroom *kind = nullptr;
	double x = 0, y = 0; ///< world units
	float facing = 0;
};

/** The mushroom of a tile, if it has one now. */
bool MushroomOnTile(TileIndex t, MushroomAt &at)
{
	/* In the forest on every tile, growing all the time, any kind (the player, 24. 10.: "the mushrooms grow there all
	 * the time, the girls only pick them"); elsewhere about the towns of their theme. */
	const bool forest = ThreeDForestTile(t);
	if (!forest && !IsTileType(t, TileType::Clear) && !IsTileType(t, TileType::Trees)) return false;
	const uint32_t h = (TileX(t) * 2654435761u) ^ (TileY(t) * 40503u) ^ 0x5bd1e995u;
	if (!forest && (h >> 3) % MUSHROOM_ONE_IN != 0) return false;
	const std::vector<ThreeDMushroom> &kinds = GetThreeDMushrooms();
	if (kinds.empty()) return false;
	const ThreeDMushroom &m = kinds[(h >> 9) % kinds.size()];
	if (!forest && m.theme != 0 && to_underlying(GetTownThemeAt(t)) != m.theme) return false;
	auto picked = _mushrooms_picked.find(t.base());
	if (picked != _mushrooms_picked.end()) {
		if (TimerGameTick::counter - picked->second < MUSHROOM_REGROW_TICKS) return false;
		_mushrooms_picked.erase(picked);
	}
	at.key = t.base();
	at.kind = &m;
	at.x = TileX(t) * TILE_SIZE + 2 + (h >> 13) % (TILE_SIZE - 4);
	at.y = TileY(t) * TILE_SIZE + 2 + (h >> 18) % (TILE_SIZE - 4);
	at.facing = static_cast<float>((h >> 23) % 24 * 15);
	return true;
}

/**
 * A mushroom of the forest with a clearing at one of the places of the part of its tile ("houby" of the colleague's
 * hra/3d/LES-PRO-HRU.md), if it has one now: there all the time, a kind of any theme by the place, gone a while when
 * picked.
 */
bool ForestMushroom(TileIndex t, const ThreeDForestPart &part, size_t place, MushroomAt &at)
{
	const std::vector<ThreeDMushroom> &kinds = GetThreeDMushrooms();
	if (kinds.empty() || place >= part.mushrooms.size()) return false;
	at.key = (static_cast<uint64_t>(place) + 1) << 32 | t.base();
	auto picked = _mushrooms_picked.find(at.key);
	if (picked != _mushrooms_picked.end()) {
		if (TimerGameTick::counter - picked->second < MUSHROOM_REGROW_TICKS) return false;
		_mushrooms_picked.erase(picked);
	}
	const uint32_t h = (TileX(t) * 2654435761u) ^ (TileY(t) * 40503u) ^ (static_cast<uint32_t>(place) * 0x9e3779b9u);
	at.kind = &kinds[(h >> 9) % kinds.size()];
	const ThreeDObjectFigure &f = part.mushrooms[place];
	at.x = TileX(t) * TILE_SIZE + f.at[0] * OBJECT_UNITS_PER_METRE;
	at.y = TileY(t) * TILE_SIZE + f.at[1] * OBJECT_UNITS_PER_METRE;
	at.facing = static_cast<float>((h >> 23) % 24 * 15);
	return true;
}

/** The mushrooms about a point, so many tiles each way: each with its tile. */
template <typename F>
void ForMushroomsNear(double x, double y, int tiles, F fn)
{
	const int cx = static_cast<int>(x / TILE_SIZE), cy = static_cast<int>(y / TILE_SIZE);
	for (int ty = std::max(1, cy - tiles); ty <= std::min<int>(Map::MaxY() - 1, cy + tiles); ty++) {
		for (int tx = std::max(1, cx - tiles); tx <= std::min<int>(Map::MaxX() - 1, cx + tiles); tx++) {
			const TileIndex t = TileXY(tx, ty);
			if (const ThreeDForestPart *forest = ThreeDGameForestPartAt(t); forest != nullptr) {
				for (size_t place = 0; place < forest->mushrooms.size(); place++) {
					MushroomAt at;
					if (ForestMushroom(t, *forest, place, at)) fn(t, at);
				}
				continue;
			}
			MushroomAt at;
			if (MushroomOnTile(t, at)) fn(t, at);
		}
	}
}

const ThreeDAnimal *KindOf(const Beast &b)
{
	return GetThreeDAnimal(b.kind);
}

/** Its figure in a pose: "<jmeno>/<poza>", the figure as it stands where it has not the pose. */
std::string FigureIn(const Beast &b, std::string_view pose)
{
	return pose.empty() ? b.kind : b.kind + "/" + std::string(pose);
}

const ThreeDVehicleObject *PoseFigure(const Beast &b, std::string_view pose)
{
	return GetThreeDFigure(FigureIn(b, pose));
}

/** Its pose for a state, of those it has: the first it has of the list, else as it stands. */
std::string_view PoseOf(const ThreeDAnimal &a, std::initializer_list<std::string_view> wanted)
{
	for (std::string_view p : wanted) {
		if (a.Has(p)) return p;
	}
	return {};
}

/** Into a pose (from its start again with restart), passing over from the one it is in. */
void SetPose(Beast &b, std::string_view pose, uint64_t now, double pace = 1.0, bool restart = false)
{
	b.pace = pace;
	if (b.pose == pose && !restart) return;
	b.from_pose = b.pose;
	b.from_frame = b.frame;
	b.pose = pose;
	b.pose_start = now;
}

int FrameOf(const Beast &b, uint64_t now)
{
	const ThreeDVehicleObject *f = PoseFigure(b, b.pose);
	if (f == nullptr || f->frames < 2) return 0;
	const uint64_t n = static_cast<uint64_t>(static_cast<double>(now - b.pose_start) * b.pace) / f->frame_ticks;
	/* Falling it falls once and lies so (the colleague's poses do not say). */
	if (!f->loop || b.pose == "smrt") return static_cast<int>(std::min<uint64_t>(n, f->frames - 1));
	return static_cast<int>(n % f->frames);
}

/** Whether its pose has gone round once (one of a single frame: after about a second). */
bool PoseDone(const Beast &b, uint64_t now)
{
	const ThreeDVehicleObject *f = PoseFigure(b, b.pose);
	const double ticks = f != nullptr && f->frames >= 2 ? static_cast<double>(f->frames * f->frame_ticks) : 30.0;
	return static_cast<double>(now - b.pose_start) * b.pace >= ticks;
}

/** Metres a tick it covers in a pose: its step over the round of its frames ("krok_m"), else so much. */
double Speed(const Beast &b, std::string_view pose, double fallback)
{
	const ThreeDVehicleObject *f = PoseFigure(b, pose);
	if (f == nullptr || f->walk_step <= 0 || f->frames < 1) return fallback * b.pace;
	return f->walk_step / (f->frames * f->frame_ticks) * b.pace;
}

/** Whether it can go anywhere: a pose of walking or running, or it flies. */
bool Moves(const ThreeDAnimal &a)
{
	return a.flies || a.Has("chuze") || a.Has("beh") || a.Has("skok");
}

bool OnMap(double x, double y)
{
	return x >= TILE_SIZE && y >= TILE_SIZE && x < (Map::SizeX() - 1) * TILE_SIZE && y < (Map::SizeY() - 1) * TILE_SIZE;
}

/** How much its blow takes of the walker, by how big it is: a cat a little, an elephant a lot. */
uint8_t BlowOf(const ThreeDAnimal &a)
{
	return static_cast<uint8_t>(std::clamp(std::lround(3.0 + 4.0 * a.size[2]), 3L, 25L));
}

/** How many blows of the hammer it takes, by how big it is. */
int HealthOf(const ThreeDAnimal &a)
{
	return static_cast<int>(std::clamp(std::lround(1.0 + a.size[0] * a.size[2]), 2L, 12L));
}

/** Its part of the screen dirty, as big as it is drawn. */
void MarkDirty(const Beast &b, const ThreeDAnimal &a)
{
	/* As big as it is drawn on the map (AddThreeDAnimalsToViewport()), floating as high. */
	const float scale = a.scale * (a.kind == ThreeDAnimal::Kind::Seller ? 1.0f : THREE_D_MAP_CREATURE_SCALE);
	const double len_m = std::max(a.size[0], a.size[1]) * scale, tall_m = (a.size[2] + a.hover_m) * scale;
	const Point pt = RemapCoords(static_cast<int>(b.x), static_cast<int>(b.y), b.z);
	const int r = static_cast<int>(len_m * OBJECT_UNITS_PER_METRE * 4 * ZOOM_BASE) + 32 * ZOOM_BASE;
	const int up = static_cast<int>(tall_m * OBJECT_HEIGHT_PER_METRE * ZOOM_BASE) + r;
	DirtyPlace place;
	place.kind = DirtyPlace::Kind::Box;
	const double reach = len_m * OBJECT_UNITS_PER_METRE + TILE_SIZE / 2.0;
	place.x0 = b.x - reach;
	place.x1 = b.x + reach;
	place.y0 = b.y - reach;
	place.y1 = b.y + reach;
	place.z0 = b.z - TILE_HEIGHT;
	place.z1 = b.z + tall_m * OBJECT_HEIGHT_PER_METRE + reach;
	DirtyPlaceScope scope(place);
	MarkAllViewportsDirty(pt.x - r, pt.y - up, pt.x + r, pt.y + r);
}

/** Step towards a point at a speed, turning to it; whether it got there. */
bool StepTo(Beast &b, double tx, double ty, double speed_m)
{
	const double dx = tx - b.x, dy = ty - b.y, dist = std::hypot(dx, dy);
	if (dist > 1e-6) b.heading = std::atan2(dy, dx);
	const double step = speed_m * OBJECT_UNITS_PER_METRE;
	if (dist <= step) {
		b.x = tx;
		b.y = ty;
		return true;
	}
	const double nx = b.x + dx / dist * step, ny = b.y + dy / dist * step;
	if (!OnMap(nx, ny)) return true;
	b.x = nx;
	b.y = ny;
	return false;
}

/** Walk towards a point by the way round what stands between (NavFindWay(), as the dragons); whether it got there. */
bool WalkTo(Beast &b, double tx, double ty, double speed_m, uint64_t now, bool flies)
{
	if (flies) return StepTo(b, tx, ty, speed_m);
	if (b.way.empty() || std::hypot(tx - b.way_tx, ty - b.way_ty) > TILE_SIZE || now - b.way_made > 74) {
		b.way = NavFindWay(b.x, b.y, tx, ty, 24);
		b.way_at = 0;
		b.way_tx = tx;
		b.way_ty = ty;
		b.way_made = now;
		if (b.way.empty()) return true;
	}
	if (b.way_at >= b.way.size()) return true;
	const auto [wx, wy] = b.way[b.way_at];
	if (!StepTo(b, wx, wy, speed_m)) return false;
	b.way_at++;
	return b.way_at >= b.way.size();
}

/**
 * Whether a dog can stand at a point of the yard of a farm: on its tiles, among its buildings by their shapes, as the
 * walker (the tiles of an industry are not walkable to the way over the tiles, NavWalkable()).
 */
bool InFarmYard(IndustryID farm, double x, double y)
{
	if (!OnMap(x, y)) return false;
	const double r = YARD_DOG_M * OBJECT_UNITS_PER_METRE;
	for (int k = -1; k < 4; k++) {
		const double px = k < 0 ? x : x + r * std::cos(k * M_PI / 2), py = k < 0 ? y : y + r * std::sin(k * M_PI / 2);
		const TileIndex t = TileVirtXY(static_cast<int>(px), static_cast<int>(py));
		if (!IsTileType(t, TileType::Industry) || GetIndustryIndex(t) != farm || !FirstPersonGroundWalkable(px, py)) return false;
	}
	return true;
}

/** Whether a dog of a farm can walk straight from one point of its yard to another (InFarmYard() each half metre). */
bool FarmYardWay(IndustryID farm, double fx, double fy, double tx, double ty)
{
	const int steps = std::max(1, static_cast<int>(std::hypot(tx - fx, ty - fy) / (0.5 * OBJECT_UNITS_PER_METRE)));
	for (int k = 1; k <= steps; k++) {
		if (!InFarmYard(farm, fx + (tx - fx) * k / steps, fy + (ty - fy) * k / steps)) return false;
	}
	return true;
}

/** The way round a pasture outside its fence (Place::Pasture), world units: a rectangle. */
struct PastureRing {
	double x0, y0, x1, y1;

	explicit PastureRing(TileIndex north)
	{
		uint px, py, w, h;
		GetPastureSpot(north, px, py, w, h);
		const double out = PASTURE_OUT_M * OBJECT_UNITS_PER_METRE;
		this->x0 = TileX(north) * TILE_SIZE - out;
		this->y0 = TileY(north) * TILE_SIZE - out;
		this->x1 = (TileX(north) + w) * TILE_SIZE + out;
		this->y1 = (TileY(north) + h) * TILE_SIZE + out;
	}

	double Length() const { return 2 * (this->x1 - this->x0 + this->y1 - this->y0); }

	/** The corners, so far round from the first (x0, y0). */
	std::array<double, 4> Corners() const
	{
		const double a = this->x1 - this->x0, b = this->y1 - this->y0;
		return {0, a, a + b, 2 * a + b};
	}

	/** The point so far round. */
	void At(double s, double &x, double &y) const
	{
		const double a = this->x1 - this->x0, b = this->y1 - this->y0;
		s = std::fmod(s, this->Length());
		if (s < 0) s += this->Length();
		if (s < a) {
			x = this->x0 + s;
			y = this->y0;
		} else if (s < a + b) {
			x = this->x1;
			y = this->y0 + (s - a);
		} else if (s < 2 * a + b) {
			x = this->x1 - (s - a - b);
			y = this->y1;
		} else {
			x = this->x0;
			y = this->y1 - (s - 2 * a - b);
		}
	}

	/** How far round the point of it nearest to one. */
	double Nearest(double x, double y) const
	{
		const double a = this->x1 - this->x0, b = this->y1 - this->y0;
		const double cx = std::clamp(x, this->x0, this->x1), cy = std::clamp(y, this->y0, this->y1);
		const double d[] = {std::abs(y - this->y0), std::abs(x - this->x1), std::abs(y - this->y1), std::abs(x - this->x0)};
		const int side = static_cast<int>(std::ranges::min_element(d) - std::begin(d));
		switch (side) {
			case 0: return cx - this->x0;
			case 1: return a + (cy - this->y0);
			case 2: return a + b + (this->x1 - cx);
			default: return 2 * a + b + (this->y1 - cy);
		}
	}
};

/** Whether a tile is the north tile of a pasture. */
bool IsPastureNorthTile(TileIndex t)
{
	if (!IsPasture(t)) return false;
	uint x, y, w, h;
	GetPastureSpot(t, x, y, w, h);
	return x == 0 && y == 0;
}

/**
 * The next place of a dog by a pasture's fence: on along it either way, a corner at most (so that it goes round the
 * fence, not through the pasture), where it can stand.
 */
void PickFenceWalk(Beast &b)
{
	b.tx = b.x;
	b.ty = b.y;
	if (!IsPastureNorthTile(b.pasture)) return;
	const PastureRing ring(b.pasture);
	const double s = ring.Nearest(b.x, b.y), len = ring.Length();
	const double step = (0.4 + InteractiveRandomRange(100) / 100.0) * TILE_SIZE;
	const double first = InteractiveRandomRange(2) ? 1 : -1;
	for (double way : {first, -first}) {
		double to = s + way * step;
		/* The first corner on the way, if it comes before. */
		for (double c : ring.Corners()) {
			for (double k : {c - len, c, c + len}) {
				if (way > 0 ? (k > s + 0.5 && k < to) : (k < s - 0.5 && k > to)) to = k;
			}
		}
		double x, y;
		ring.At(to, x, y);
		const double ox = x, oy = y;
		if (!OnMap(x, y) || !NavNearestWalkable(x, y, 1) || std::hypot(x - ox, y - oy) > TILE_SIZE / 4.0) continue;
		b.tx = x;
		b.ty = y;
		b.way.clear();
		return;
	}
}

/** A place to walk to near its own, where it can stand (a farm's dog: in its yard; by a pasture: along its fence). */
void PickWander(Beast &b)
{
	if (b.place == Place::Pasture) {
		PickFenceWalk(b);
		return;
	}
	for (int tries = 0; tries < 6; tries++) {
		const double a = InteractiveRandomRange(360) * M_PI / 180.0, r = (0.3 + InteractiveRandomRange(100) / 100.0) * WANDER_TILES * TILE_SIZE;
		double x = b.home_x + std::cos(a) * r, y = b.home_y + std::sin(a) * r;
		if (b.place == Place::Farm) {
			/* In its yard, straight among the buildings. */
			if (!FarmYardWay(b.farm, b.x, b.y, x, y)) continue;
		} else if (!OnMap(x, y) || !NavNearestWalkable(x, y, 1) || !OnMap(x, y)) {
			continue;
		}
		b.tx = x;
		b.ty = y;
		b.way.clear();
		return;
	}
	b.tx = b.x;
	b.ty = b.y;
}

/** Whether a tile is the north tile of a church built (BuildingFlag::IsChurch, of any climate's). */
bool IsChurchNorthTile(TileIndex t)
{
	if (!IsValidTile(t) || !IsTileType(t, TileType::House) || !IsHouseCompleted(t)) return false;
	HouseID house = GetHouseType(t);
	return GetHouseNorthPart(house) == 0 && HouseSpec::Get(house)->building_flags.Test(BuildingFlag::IsChurch);
}

/** A church's middle and how far its walls go from it at most, world units. */
void ChurchMiddle(TileIndex church, double &x, double &y, double &radius)
{
	const BuildingFlags flags = HouseSpec::Get(GetHouseType(church))->building_flags;
	const int sx = flags.Any({BuildingFlag::Size2x1, BuildingFlag::Size2x2}) ? 2 : 1;
	const int sy = flags.Any({BuildingFlag::Size1x2, BuildingFlag::Size2x2}) ? 2 : 1;
	x = (TileX(church) + sx / 2.0) * TILE_SIZE;
	y = (TileY(church) + sy / 2.0) * TILE_SIZE;
	radius = std::hypot(sx, sy) / 2.0 * TILE_SIZE;
}

/**
 * The church's door, a step out of it: its 3D object's first door ("dvere"); without one the middle of its side to a
 * road beside it, else of its south-east side.
 */
void ChurchDoor(TileIndex church, double &x, double &y)
{
	const HouseID house = GetHouseType(church);
	if (const ThreeDObject *obj = GetThreeDHouseObjectAt(house, church); obj != nullptr && !obj->doors.empty()) {
		const ThreeDObjectDoor &d = obj->doors.front();
		const double len = std::max(1e-3, static_cast<double>(std::hypot(d.out[0], d.out[1])));
		x = TileX(church) * TILE_SIZE + (d.at[0] + d.out[0] / len * 0.8) * OBJECT_UNITS_PER_METRE;
		y = TileY(church) * TILE_SIZE + (d.at[1] + d.out[1] / len * 0.8) * OBJECT_UNITS_PER_METRE;
		return;
	}
	double mx, my, radius;
	ChurchMiddle(church, mx, my, radius);
	static const int8_t DX[4] = {1, 0, -1, 0}, DY[4] = {0, 1, 0, -1};
	int side = 1;
	for (int k = 0; k < 4; k++) {
		const int tx = static_cast<int>(mx / TILE_SIZE + DX[k] * (radius / TILE_SIZE + 0.2)), ty = static_cast<int>(my / TILE_SIZE + DY[k] * (radius / TILE_SIZE + 0.2));
		if (tx < 0 || ty < 0 || tx >= static_cast<int>(Map::SizeX()) || ty >= static_cast<int>(Map::SizeY())) continue;
		if (IsTileType(TileXY(tx, ty), TileType::Road)) {
			side = k;
			break;
		}
	}
	x = mx + DX[side] * (radius * 0.75 + 0.1 * TILE_SIZE);
	y = my + DY[side] * (radius * 0.75 + 0.1 * TILE_SIZE);
	NavNearestWalkable(x, y, 1);
}

/** A place about the church, a little out from its walls, where it can stand. @return whether there is one */
bool PickAboutChurch(TileIndex church, double &x, double &y)
{
	double mx, my, radius;
	ChurchMiddle(church, mx, my, radius);
	for (int tries = 0; tries < 8; tries++) {
		const double a = InteractiveRandomRange(360) * M_PI / 180.0, r = radius + (0.3 + InteractiveRandomRange(100) / 100.0) * CHURCH_ROUND_TILES * TILE_SIZE;
		double px = mx + std::cos(a) * r, py = my + std::sin(a) * r;
		if (!OnMap(px, py) || !NavNearestWalkable(px, py, 1) || !OnMap(px, py)) continue;
		x = px;
		y = py;
		return true;
	}
	return false;
}

/**
 * The way a plain railway tile's straight track runs, radians (along x 0, along y a quarter turn); false none
 * straight (a curve, a crossing of both: it stays where it is).
 */
bool RailAxis(TileIndex tile, double &heading)
{
	if (!IsValidTile(tile) || !IsPlainRailTile(tile)) return false;
	const TrackBits bits = GetTrackBits(tile);
	if (bits == TrackBits{Track::X}) {
		heading = 0;
		return true;
	}
	if (bits == TrackBits{Track::Y}) {
		heading = M_PI / 2;
		return true;
	}
	return false;
}

/** The middle of a tile's edge on a side, world units: where one riding the rails passes from tile to tile. */
void EdgeMiddle(TileIndex t, DiagDirection side, double &x, double &y)
{
	static constexpr double DX[] = {0, 0.5, 1, 0.5}, DY[] = {0.5, 1, 0.5, 0};
	x = (TileX(t) + DX[to_underlying(side)]) * TILE_SIZE;
	y = (TileY(t) + DY[to_underlying(side)]) * TILE_SIZE;
}

/**
 * On along the rails as a train goes (the player, 24. 10.: "Billy on his tricycle is to ride the rails like a little
 * engine, not come after me to attack; he is to ride past along the rails"): from the far edge of the tile it rides
 * through into the tile beyond, along a track there that goes on from that edge -- straight on where it can, else any,
 * curves and switches as a train --, to that tile's far edge; at the end of the line back the way it came.
 */
void PickRailRide(Beast &b)
{
	if (b.rail_tile == INVALID_TILE || b.rail_td == Trackdir::Invalid) {
		b.tx = b.x;
		b.ty = b.y;
		return;
	}
	const DiagDirection out = TrackdirToExitdir(b.rail_td);
	const TileIndex next = TileAddWrap(b.rail_tile, TileIndexDiffCByDiagDir(out).x, TileIndexDiffCByDiagDir(out).y);
	const TrackdirBits on = next == INVALID_TILE ? TrackdirBits{} : GetTileTrackStatus(next, TransportType::Rail, RoadTramType::Invalid).trackdirs & DiagdirReachesTrackdirs(out);
	if (on.None()) {
		/* The end of the line (a buffer, a tunnel, a bridge, the edge of the map): back across the tile. */
		b.rail_td = ReverseTrackdir(b.rail_td);
	} else {
		b.rail_tile = next;
		if (!on.Test(b.rail_td)) b.rail_td = on.GetNthSetBit(InteractiveRandomRange(on.Count())).value_or(FindFirstTrackdir(on));
	}
	EdgeMiddle(b.rail_tile, TrackdirToExitdir(b.rail_td), b.tx, b.ty);
	b.way.clear();
}

/** A straight railway tile near the middle, at random: the middle of its track, world units, and its way; false none found. */
bool PickRail(TileIndex middle, double &x, double &y, double &heading)
{
	for (int tries = 0; tries < 32; tries++) {
		const int dx = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int dy = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int tx = static_cast<int>(TileX(middle)) + dx, ty = static_cast<int>(TileY(middle)) + dy;
		if (tx < 1 || ty < 1 || tx >= static_cast<int>(Map::MaxX()) || ty >= static_cast<int>(Map::MaxY())) continue;
		if (!RailAxis(TileXY(tx, ty), heading)) continue;
		if (InteractiveRandomRange(2)) heading += M_PI;
		x = tx * TILE_SIZE + TILE_SIZE / 2.0;
		y = ty * TILE_SIZE + TILE_SIZE / 2.0;
		return true;
	}
	return false;
}

/** Its rest: the dog before a door lies (asleep), the others stand; a seller as it stands. */
std::string_view RestPose(const Beast &b, const ThreeDAnimal &a)
{
	if (b.place == Place::Door) return PoseOf(a, {"lezi", "klid"});
	return PoseOf(a, {"klid"});
}

/** Whether it fights the walker now: one that attacks, or one provoked. */
/** Whether it rides the rails (PickRailRide()): Billy by them; the other ghosts there haunt as anywhere. */
bool RidesRails(const Beast &b, const ThreeDAnimal &a)
{
	return b.place == Place::Rail && a.name == RAIL_RIDER;
}

bool Fights(const Beast &b, const ThreeDAnimal &a)
{
	/* Billy rides past (the player, 24. 10.: "he is not to come after me to attack"). */
	if (b.place == Place::Pet || b.place == Place::Seller || RidesRails(b, a)) return false;
	return a.temper == ThreeDAnimal::Temper::Attacks || b.angry;
}

/** Whether a pose is of sitting or lying, or going into one: it gets up before it goes anywhere. */
bool Down(std::string_view pose)
{
	return pose == "sedi" || pose == "sedani" || pose == "lezi" || pose == "lehani";
}

/**
 * The walker's dog (the player, 19. 10., PROSBA-PAR14.md point 1: "the dogs will run before me; when I stand they sit
 * quietly; when I set off, the dog sets off before me"; "when I stop it stops too and sits, lies, wags"): the walker
 * walking, it goes before it the way it goes, running to get there; the walker standing, it stops near it and rests,
 * now sitting, now lying, now looking about (its poses "sedi", "lezi", "klid", the colleague's carried over between the
 * dogs), sitting down and getting up as its poses "sedani", "lehani", "vstavani", "vstavani_lezi" show; setting off, it
 * gets up first. A dog without those poses stands.
 */
void Follow(Beast &b, const ThreeDAnimal &a, uint64_t now, double wx, double wy, double dist_m, std::string_view walk)
{
	if (dist_m > 60.0) {
		/* Left far behind (the walker got into a vehicle, went off): it comes along. */
		b.x = wx + FOLLOW_M * OBJECT_UNITS_PER_METRE;
		b.y = wy;
		if (!a.flies) NavNearestWalkable(b.x, b.y, 1);
		b.way.clear();
		b.next_pose.clear();
		return;
	}
	/* Sitting down, lying down or getting up: that first. */
	if (!b.next_pose.empty()) {
		if (!PoseDone(b, now)) return;
		SetPose(b, b.next_pose, now);
		b.next_pose.clear();
	}
	const bool go = _walker_moving || dist_m > NEAR_REST_M;
	if (go && Down(b.pose)) {
		const bool lying = b.pose == "lezi" || b.pose == "lehani";
		const std::string_view up = lying ? PoseOf(a, {"vstavani_lezi", "vstavani"}) : PoseOf(a, {"vstavani"});
		if (!up.empty()) {
			SetPose(b, up, now, 1.0, true);
			b.next_pose = std::string(walk);
			return;
		}
	}
	if (go) {
		/* Before the walker, the way it faces; standing, before it too, where it is seen. */
		const double gx = wx + std::cos(_walker_heading) * AHEAD_M * OBJECT_UNITS_PER_METRE, gy = wy + std::sin(_walker_heading) * AHEAD_M * OBJECT_UNITS_PER_METRE;
		const double gap_m = std::hypot(gx - b.x, gy - b.y) / OBJECT_UNITS_PER_METRE;
		if (gap_m < 0.6) {
			/* There: along with the walker, the way it goes. */
			b.heading = _walker_heading;
			SetPose(b, _walker_moving ? walk : RestPose(b, a), now, 1.3);
			return;
		}
		const bool far = gap_m > 3.0 && a.Has("beh");
		const std::string_view pose = far ? std::string_view("beh") : walk;
		SetPose(b, pose, now, far ? 1.0 : 1.3);
		/* As fast as it has to: past the walker to before it, and not behind it running. */
		const double speed = std::max(Speed(b, pose, 0.03), std::min(gap_m * 0.08, 0.35));
		/* Straight there where nothing stands between (the place moves with the walker every tick: a way found to
		 * where it was is behind it already); round what stands between else. */
		if (a.flies || NavStraight(b.x, b.y, gx, gy)) {
			b.way.clear();
			StepTo(b, gx, gy, speed);
		} else {
			WalkTo(b, gx, gy, speed, now, a.flies);
		}
		b.until = now + 60 + InteractiveRandomRange(120);
		return;
	}
	/* The walker stands: so does the dog, looking at it; now and then it sits, lies down, gets up, looks about. */
	b.heading = std::atan2(wy - b.y, wx - b.x);
	if (b.pose == walk || b.pose == "beh") SetPose(b, PoseOf(a, {"klid", "stoji"}), now);
	if (now < b.until) return;
	b.until = now + 200 + InteractiveRandomRange(400);
	const bool sits = b.pose == "sedi", lies = b.pose == "lezi";
	std::vector<std::string_view> next;
	if (a.Has("sedi") && !sits) next.push_back("sedi");
	if (a.Has("lezi") && !lies) next.push_back("lezi");
	if (a.Has("klid") && b.pose != "klid") next.push_back("klid");
	if (next.empty()) return;
	const std::string_view want = next[InteractiveRandomRange(static_cast<uint32_t>(next.size()))];
	/* From sitting or lying, up first; then down into the next, as its poses show. */
	if (sits || lies) {
		const std::string_view up = lies ? PoseOf(a, {"vstavani_lezi", "vstavani"}) : PoseOf(a, {"vstavani"});
		if (!up.empty() && want == "klid") {
			SetPose(b, up, now, 1.0, true);
			b.next_pose = std::string(want);
			return;
		}
	}
	const std::string_view into = want == "sedi" ? PoseOf(a, {"sedani"}) : want == "lezi" ? PoseOf(a, {"lehani"}) : std::string_view{};
	if (!into.empty() && !sits && !lies) {
		SetPose(b, into, now, 1.0, true);
		b.next_pose = std::string(want);
		return;
	}
	SetPose(b, want, now);
}

/**
 * The ghost of a church, not fighting: about its church, now and then into it by its door, or gone where it is; a while
 * after back out of the door, or about the church somewhere else.
 */
void ChurchGhost(Beast &b, const ThreeDAnimal &a, uint64_t now)
{
	if (b.inside) {
		if (now < b.until) return;
		double x = b.x, y = b.y;
		if (b.out_at_door) {
			ChurchDoor(b.church, x, y);
		} else {
			PickAboutChurch(b.church, x, y);
		}
		b.inside = false;
		b.x = b.tx = x;
		b.y = b.ty = y;
		b.way.clear();
		b.state = State::Idle;
		b.until = now + 60 + InteractiveRandomRange(120);
		MarkDirty(b, a);
		return;
	}
	const std::string_view walk = PoseOf(a, {"chuze", "beh"});
	if (b.state == State::Wander) {
		SetPose(b, walk, now);
		if (!WalkTo(b, b.tx, b.ty, Speed(b, walk, 0.015), now, a.flies)) return;
		b.state = State::Idle;
		if (b.going_in) {
			/* In through the door: out of it again after a while, or somewhere about the church. */
			b.going_in = false;
			b.inside = true;
			b.out_at_door = InteractiveRandomRange(2) == 0;
			b.until = now + CHURCH_INSIDE_TICKS + InteractiveRandomRange(CHURCH_INSIDE_TICKS);
			MarkDirty(b, a);
		} else {
			b.until = now + 100 + InteractiveRandomRange(300);
		}
		return;
	}
	SetPose(b, RestPose(b, a), now);
	if (now < b.until) return;
	const uint32_t pick = InteractiveRandomRange(6);
	if (pick == 0) {
		/* Gone where it stands, for a while. */
		b.inside = true;
		b.out_at_door = InteractiveRandomRange(2) == 0;
		b.until = now + CHURCH_INSIDE_TICKS / 2 + InteractiveRandomRange(CHURCH_INSIDE_TICKS);
		MarkDirty(b, a);
		return;
	}
	double x = b.x, y = b.y;
	b.going_in = pick <= 2;
	if (b.going_in) {
		ChurchDoor(b.church, x, y);
	} else if (!PickAboutChurch(b.church, x, y)) {
		b.until = now + 100;
		return;
	}
	b.tx = x;
	b.ty = y;
	b.way.clear();
	b.state = State::Wander;
}

void Update(Beast &b, const ThreeDAnimal &a, uint64_t now, bool walker, double wx, double wy)
{
	/* The ghost of a church about it and in it; struck, it runs as the others (Strike()). */
	if (b.place == Place::Church && (b.inside || b.state == State::Idle || b.state == State::Wander) && !b.angry) {
		ChurchGhost(b, a, now);
		return;
	}
	const double dist_m = walker ? std::hypot(wx - b.x, wy - b.y) / OBJECT_UNITS_PER_METRE : 1e9;
	const double reach_m = std::max(a.size[0], a.size[1]) / 2 + GAP_M;
	/* The hare and the squirrel hop ("skok", the colleague's hra/3d/LES-PRO-HRU.md: up and down on the spot, moved on
	 * "krok_m" a round by the game) where others walk. */
	const std::string_view walk = PoseOf(a, {"chuze", "skok", "beh"}), run = PoseOf(a, {"beh", "skok", "chuze"});
	switch (b.state) {
		case State::Idle:
			SetPose(b, RestPose(b, a), now);
			/* A seller and the dog before the door look at the walker coming; the dog growls ("vrci") near. */
			if ((b.place == Place::Seller || b.place == Place::Door) && dist_m < 8.0) b.heading = std::atan2(wy - b.y, wx - b.x);
			if (b.place == Place::Door && dist_m < 4.0 && a.Has("vrci")) SetPose(b, "vrci", now);
			if (Fights(b, a) && dist_m < CHASE_M) {
				b.state = State::Chase;
			} else if (now >= b.until && Moves(a) && (b.place == Place::Round || b.place == Place::Forest || b.place == Place::ForestNight || b.place == Place::Ghost || b.place == Place::Farm || b.place == Place::Pasture || (b.place == Place::Rail && !RidesRails(b, a)) || (b.place == Place::Street && a.Has("chuze")))) {
				PickWander(b);
				b.state = State::Wander;
			} else if (RidesRails(b, a)) {
				PickRailRide(b);
				b.state = State::Wander;
			}
			break;

		case State::Wander:
			SetPose(b, walk, now);
			if (Fights(b, a) && dist_m < CHASE_M) {
				b.state = State::Chase;
			} else if (WalkTo(b, b.tx, b.ty, Speed(b, walk, RidesRails(b, a) ? RAIL_RIDE_M : 0.015), now, a.flies || RidesRails(b, a) || b.place == Place::Farm)) {
				/* Along the rails on into the next tile without a stop, as a train. */
				if (RidesRails(b, a)) {
					PickRailRide(b);
				} else {
					b.state = State::Idle;
					b.until = now + 150 + InteractiveRandomRange(450);
					/* By the fence it looks in at the animals it guards. */
					if (b.place == Place::Pasture) b.heading = std::atan2(b.home_y - b.y, b.home_x - b.x);
				}
			}
			break;

		case State::Chase:
			SetPose(b, Moves(a) ? run : RestPose(b, a), now, 1.2);
			if (!walker || dist_m > GIVE_UP_M) {
				b.state = State::Idle;
				b.until = now + 60;
			} else if (dist_m <= reach_m) {
				b.heading = std::atan2(wy - b.y, wx - b.x);
				/* A breath between two blows. */
				if (now < b.until) {
					SetPose(b, RestPose(b, a), now);
					break;
				}
				b.state = State::Attack;
				b.struck = false;
				SetPose(b, PoseOf(a, {"utok", "vrci", "dupe"}), now, 1.0, true);
			} else if (Moves(a)) {
				WalkTo(b, wx, wy, Speed(b, run, 0.03), now, a.flies);
			} else {
				/* One that cannot walk (on a roof, before the door without legs that walk) turns to the walker. */
				b.heading = std::atan2(wy - b.y, wx - b.x);
			}
			break;

		case State::Attack: {
			if (walker) b.heading = std::atan2(wy - b.y, wx - b.x);
			const ThreeDVehicleObject *f = PoseFigure(b, b.pose);
			const bool frames = f != nullptr && f->frames >= 2;
			const bool blow = frames ? FrameOf(b, now) >= f->frames * 5 / 8 : now - b.pose_start >= 18;
			if (!b.struck && blow) {
				b.struck = true;
				if (dist_m <= reach_m + 1.0) _damage += BlowOf(a);
			}
			if (PoseDone(b, now)) {
				b.state = State::Chase;
				b.until = now + 25 + InteractiveRandomRange(30);
			}
			break;
		}

		case State::Flee: {
			SetPose(b, run, now, 1.5);
			if (!Moves(a) || now >= b.until) {
				b.state = State::Idle;
				b.until = now + 300;
				b.home_x = b.x;
				b.home_y = b.y;
				break;
			}
			if (WalkTo(b, b.tx, b.ty, Speed(b, run, 0.03), now, a.flies)) {
				b.state = State::Idle;
				b.until = now + 300;
				b.home_x = b.x;
				b.home_y = b.y;
			}
			break;
		}

		case State::Follow:
			if (walker) Follow(b, a, now, wx, wy, dist_m, walk);
			break;

		case State::Dying:
			SetPose(b, "smrt", now);
			if (PoseDone(b, now)) {
				b.state = State::Dead;
				b.until = now + DEAD_TICKS;
			}
			break;

		case State::Dead:
			break;
	}
}

/** Whether a tile's land is about a town of a kind of towns ("klima") and a theme ("tema"). */
bool TownFits(const Town *t, TileIndex tile, const ThreeDAnimal &a)
{
	if (a.theme != 0 && to_underlying(GetTownThemeAt(tile)) != a.theme) return false;
	if (a.climate == 0) return true;
	uint8_t look = ThreeDAnimal::TEMPERATE;
	const LandscapeType land = _settings_game.game_creation.landscape;
	if (land == LandscapeType::Tropic || GetThemedLand(t->xy) == ThemedLand::Desert) {
		look = ThreeDAnimal::DESERT;
	} else if (land == LandscapeType::Arctic) {
		look = ThreeDAnimal::ARCTIC;
	} else if (land == LandscapeType::Toyland) {
		look = 0;
	}
	return (look & a.climate) != 0;
}

/** The kinds of a place that fit a town now (its kind of towns, its theme, the time of day), and one picked of them. */
const ThreeDAnimal *PickKind(const Town *t, TileIndex tile, uint8_t where, std::initializer_list<ThreeDAnimal::Kind> kinds, bool night)
{
	std::vector<const ThreeDAnimal *> fit;
	for (const ThreeDAnimal &a : GetThreeDAnimals()) {
		if ((a.places & where) == 0 || a.sold) continue;
		if (std::ranges::find(kinds, a.kind) == kinds.end()) continue;
		if (!a.OutNow(night)) continue;
		if (!TownFits(t, tile, a)) continue;
		fit.push_back(&a);
	}
	if (fit.empty()) return nullptr;
	return fit[InteractiveRandomRange(static_cast<uint32_t>(fit.size()))];
}

/** A beast of a kind put at a place, facing a way. */
Beast &Put(const ThreeDAnimal &a, Place place, double x, double y, double up, double heading, uint64_t now)
{
	Beast &b = _beasts.emplace_back();
	b.kind = a.name;
	b.place = place;
	b.x = b.home_x = b.tx = x;
	b.y = b.home_y = b.ty = y;
	b.up = up + a.hover_m * OBJECT_HEIGHT_PER_METRE;
	b.heading = heading;
	b.born = now;
	b.health = HealthOf(a);
	b.until = now + InteractiveRandomRange(300);
	SetPose(b, RestPose(b, a), now);
	b.pose_start = now - std::min<uint64_t>(now, InteractiveRandomRange(200)); // not all in step
	b.z = static_cast<int>(std::lround(FirstPersonGroundZ(b.x, b.y) + b.up));
	Debug(misc, 2, "3D animal: {} comes at {:.1f},{:.1f}", a.name, x / TILE_SIZE, y / TILE_SIZE);
	return b;
}

/** A road tile of a town near the middle, at random: one of its pavements, world units; false none found. */
bool PickPavement(const Town *t, TileIndex middle, double &x, double &y)
{
	for (int tries = 0; tries < 24; tries++) {
		const int dx = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int dy = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int tx = static_cast<int>(TileX(middle)) + dx, ty = static_cast<int>(TileY(middle)) + dy;
		if (tx < 1 || ty < 1 || tx >= static_cast<int>(Map::MaxX()) || ty >= static_cast<int>(Map::MaxY())) continue;
		const TileIndex tile = TileXY(tx, ty);
		if (!IsThreeDWalkway(tile, t)) continue;
		const double side = InteractiveRandomRange(2) ? PAVEMENT : -PAVEMENT;
		x = tx * TILE_SIZE + TILE_SIZE / 2.0 + (InteractiveRandomRange(2) ? side : 0);
		y = ty * TILE_SIZE + TILE_SIZE / 2.0 + (x == tx * TILE_SIZE + TILE_SIZE / 2.0 ? side : 0);
		return true;
	}
	return false;
}

/** A tile of the land about a town (past its houses, where one can stand), near the middle, at random; false none found. */
bool PickLand(const Town *t, TileIndex middle, bool town_land, double &x, double &y)
{
	for (int tries = 0; tries < 24; tries++) {
		const int dx = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int dy = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int tx = static_cast<int>(TileX(middle)) + dx, ty = static_cast<int>(TileY(middle)) + dy;
		if (tx < 1 || ty < 1 || tx >= static_cast<int>(Map::MaxX()) || ty >= static_cast<int>(Map::MaxY())) continue;
		const TileIndex tile = TileXY(tx, ty);
		if (!IsTileType(tile, TileType::Clear) && !IsTileType(tile, TileType::Trees)) continue;
		/* Out of the town, not too far from it (a ghost of a theme: on the theme's land). */
		const uint dist2 = DistanceSquare(tile, t->xy);
		if (!town_land && dist2 < t->cache.squared_town_zone_radius[to_underlying(HouseZone::TownEdge)]) continue;
		if (dist2 > 30u * 30u) continue;
		x = tx * TILE_SIZE + 2 + InteractiveRandomRange(TILE_SIZE - 4);
		y = ty * TILE_SIZE + 2 + InteractiveRandomRange(TILE_SIZE - 4);
		if (!NavNearestWalkable(x, y, 1)) continue;
		return true;
	}
	return false;
}

/**
 * A place of the game, or at night of the ghosts, of the forest with a clearing near the middle, at random ("zvirata",
 * "noc_postavy" of its parts): where, world units, the way it faces, and the kinds that come there; false none.
 */
bool PickGameForestPlace(TileIndex middle, bool ghosts, double &x, double &y, double &heading, std::span<const std::string> &kinds)
{
	std::vector<std::pair<TileIndex, const ThreeDForestPlace *>> found;
	const int cx = TileX(middle), cy = TileY(middle), r = static_cast<int>(REACH);
	for (int ty = std::max(1, cy - r); ty <= std::min<int>(Map::MaxY() - 1, cy + r); ty++) {
		for (int tx = std::max(1, cx - r); tx <= std::min<int>(Map::MaxX() - 1, cx + r); tx++) {
			const TileIndex t = TileXY(tx, ty);
			const ThreeDForestPart *part = ThreeDGameForestPartAt(t);
			if (part == nullptr) continue;
			for (const ThreeDForestPlace &p : ghosts ? part->ghosts : part->animals) found.emplace_back(t, &p);
		}
	}
	if (found.empty()) return false;
	const auto &[t, p] = found[InteractiveRandomRange(static_cast<uint32_t>(found.size()))];
	x = TileX(t) * TILE_SIZE + p->at.at[0] * OBJECT_UNITS_PER_METRE;
	y = TileY(t) * TILE_SIZE + p->at.at[1] * OBJECT_UNITS_PER_METRE;
	heading = p->at.facing * M_PI / 180.0;
	kinds = p->kinds;
	return true;
}

/** One of the animals of the indices named, of the kinds, out now; nullptr none. */
const ThreeDAnimal *PickNamed(std::span<const std::string> names, bool night, std::initializer_list<ThreeDAnimal::Kind> kinds)
{
	std::vector<const ThreeDAnimal *> fit;
	for (const ThreeDAnimal &a : GetThreeDAnimals()) {
		if (a.sold || !a.OutNow(night) || std::ranges::find(kinds, a.kind) == kinds.end()) continue;
		if (std::ranges::find(names, a.name) != names.end()) fit.push_back(&a);
	}
	if (fit.empty()) return nullptr;
	return fit[InteractiveRandomRange(static_cast<uint32_t>(fit.size()))];
}

/** A tile of a forest near the middle, at random (ThreeDForestTile()): a place on it to stand, world units; false none found. */
bool PickForest(TileIndex middle, double &x, double &y)
{
	for (int tries = 0; tries < 48; tries++) {
		const int dx = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int dy = static_cast<int>(InteractiveRandomRange(2 * REACH + 1)) - static_cast<int>(REACH);
		const int tx = static_cast<int>(TileX(middle)) + dx, ty = static_cast<int>(TileY(middle)) + dy;
		if (tx < 1 || ty < 1 || tx >= static_cast<int>(Map::MaxX()) || ty >= static_cast<int>(Map::MaxY())) continue;
		if (!ThreeDForestTile(TileXY(tx, ty))) continue;
		x = tx * TILE_SIZE + 2 + InteractiveRandomRange(TILE_SIZE - 4);
		y = ty * TILE_SIZE + 2 + InteractiveRandomRange(TILE_SIZE - 4);
		if (!NavNearestWalkable(x, y, 1)) continue;
		return true;
	}
	return false;
}

/** The houses about the middle with what a place needs: doors before them, or places on their roofs. */
template <typename F>
void ForHousesNear(TileIndex middle, F fn)
{
	const int cx = TileX(middle), cy = TileY(middle), r = static_cast<int>(REACH);
	for (int y = std::max(1, cy - r); y <= std::min<int>(Map::MaxY() - 1, cy + r); y++) {
		for (int x = std::max(1, cx - r); x <= std::min<int>(Map::MaxX() - 1, cx + r); x++) {
			const TileIndex t = TileXY(x, y);
			if (!IsTileType(t, TileType::House) || !IsHouseCompleted(t)) continue;
			HouseID house = GetHouseType(t);
			if (GetHouseNorthPart(house) != 0) continue;
			const ThreeDObject *obj = GetThreeDHouseObjectAt(house, t);
			if (obj != nullptr) fn(t, *obj);
		}
	}
}

/** Whether one is already about a point (so that two do not stand in each other). */
bool Taken(double x, double y, double within)
{
	return std::ranges::any_of(_beasts, [&](const Beast &b) { return std::hypot(b.home_x - x, b.home_y - y) < within; });
}

/** Whether an industry is a farm: one that lays fields about it (of any climate's, of a NewGRF too). */
bool IsFarm(const Industry *i)
{
	return i != nullptr && GetIndustrySpec(i->type)->behaviour.Test(IndustryBehaviour::PlantFields);
}

/** The farms with a tile about the middle, so many as have their dogs at most (FARMS_KEPT). */
std::vector<const Industry *> FarmsNear(TileIndex middle)
{
	std::vector<const Industry *> farms;
	const int cx = TileX(middle), cy = TileY(middle), r = static_cast<int>(REACH);
	for (const Industry *i : Industry::Iterate()) {
		if (!IsFarm(i) || farms.size() >= FARMS_KEPT) continue;
		const int x = TileX(i->location.tile), y = TileY(i->location.tile);
		if (x + static_cast<int>(i->location.w) < cx - r || x > cx + r || y + static_cast<int>(i->location.h) < cy - r || y > cy + r) continue;
		farms.push_back(i);
	}
	return farms;
}

/** The pastures about the middle (their north tiles), so many as have their dogs at most (PASTURES_KEPT). */
std::vector<TileIndex> PasturesNear(TileIndex middle)
{
	std::vector<TileIndex> pastures;
	const int cx = TileX(middle), cy = TileY(middle), r = static_cast<int>(REACH);
	for (int ty = std::max(1, cy - r); ty <= std::min<int>(Map::MaxY() - 1, cy + r) && pastures.size() < PASTURES_KEPT; ty++) {
		for (int tx = std::max(1, cx - r); tx <= std::min<int>(Map::MaxX() - 1, cx + r) && pastures.size() < PASTURES_KEPT; tx++) {
			if (IsPastureNorthTile(TileXY(tx, ty))) pastures.push_back(TileXY(tx, ty));
		}
	}
	return pastures;
}

/** One of the dogs of the farms out now; nullptr none. */
const ThreeDAnimal *PickFarmDog(bool night)
{
	const std::vector<std::string> names(std::begin(FARM_DOGS), std::end(FARM_DOGS));
	return PickNamed(names, night, {ThreeDAnimal::Kind::Dog});
}

/** One more of a place about the middle where it has fewer than it keeps; whether one came. */
bool Spawn(Place place, TileIndex middle, const Town *t, bool night, uint64_t now)
{
	double x = 0, y = 0;
	switch (place) {
		case Place::Street: {
			if (!PickPavement(t, middle, x, y) || Taken(x, y, 6)) return false;
			const ThreeDAnimal *a = PickKind(t, TileVirtXY(static_cast<int>(x), static_cast<int>(y)), ThreeDAnimal::STREET, {ThreeDAnimal::Kind::Dog, ThreeDAnimal::Kind::Animal}, night);
			if (a == nullptr) return false;
			Put(*a, place, x, y, 0, InteractiveRandomRange(360) * M_PI / 180.0, now);
			return true;
		}
		case Place::Seller: {
			if (!PickPavement(t, middle, x, y) || Taken(x, y, 6)) return false;
			std::vector<const ThreeDAnimal *> sellers;
			std::vector<std::string> sold;
			for (const ThreeDAnimal &a : GetThreeDAnimals()) {
				if (a.kind == ThreeDAnimal::Kind::Seller) sellers.push_back(&a);
				if (a.sold) sold.push_back(a.name);
			}
			if (sellers.empty() || sold.empty()) return false;
			Beast &b = Put(*sellers[InteractiveRandomRange(static_cast<uint32_t>(sellers.size()))], place, x, y, 0, InteractiveRandomRange(360) * M_PI / 180.0, now);
			/* Three to five of the dogs sold, at random (the player: "all sell everything, but each has only 3-5 on offer"). */
			const size_t n = std::min<size_t>(sold.size(), 3 + InteractiveRandomRange(3));
			for (size_t k = 0; k < n; k++) {
				const size_t pick = k + InteractiveRandomRange(static_cast<uint32_t>(sold.size() - k));
				std::swap(sold[k], sold[pick]);
				b.offer.push_back(sold[k]);
			}
			b.until = now + SELLER_TICKS;
			return true;
		}
		case Place::Door: {
			std::vector<std::pair<TileIndex, ThreeDObjectDoor>> doors;
			ForHousesNear(middle, [&](TileIndex house, const ThreeDObject &obj) {
				for (const ThreeDObjectDoor &d : obj.doors) doors.emplace_back(house, d);
			});
			if (doors.empty()) return false;
			const auto &[house, d] = doors[InteractiveRandomRange(static_cast<uint32_t>(doors.size()))];
			const double len = std::max(1e-3, static_cast<double>(std::hypot(d.out[0], d.out[1])));
			x = TileX(house) * TILE_SIZE + (d.at[0] + d.out[0] / len * 1.0) * OBJECT_UNITS_PER_METRE;
			y = TileY(house) * TILE_SIZE + (d.at[1] + d.out[1] / len * 1.0) * OBJECT_UNITS_PER_METRE;
			if (Taken(x, y, 6)) return false;
			const Town *town = ClosestTownFromTile(house, UINT_MAX);
			const ThreeDAnimal *a = town != nullptr ? PickKind(town, house, ThreeDAnimal::DOOR, {ThreeDAnimal::Kind::Dog, ThreeDAnimal::Kind::Animal}, night) : nullptr;
			if (a == nullptr) return false;
			/* Along the door, its head to the street. */
			Put(*a, place, x, y, 0, std::atan2(d.out[1], d.out[0]) + M_PI / 2, now);
			return true;
		}
		case Place::Roof: {
			std::vector<std::pair<TileIndex, ThreeDObjectRoof>> roofs;
			ForHousesNear(middle, [&](TileIndex house, const ThreeDObject &obj) {
				for (const ThreeDObjectRoof &r : obj.roofs) roofs.emplace_back(house, r);
			});
			if (roofs.empty()) return false;
			const auto &[house, r] = roofs[InteractiveRandomRange(static_cast<uint32_t>(roofs.size()))];
			x = TileX(house) * TILE_SIZE + r.at[0] * OBJECT_UNITS_PER_METRE;
			y = TileY(house) * TILE_SIZE + r.at[1] * OBJECT_UNITS_PER_METRE;
			if (Taken(x, y, 2)) return false;
			const Town *town = ClosestTownFromTile(house, UINT_MAX);
			const ThreeDAnimal *a = town != nullptr ? PickKind(town, house, ThreeDAnimal::ROOF, {ThreeDAnimal::Kind::Animal, ThreeDAnimal::Kind::Dog}, night) : nullptr;
			if (a == nullptr) return false;
			/* On the roof: its height over the house's ground, the house standing on its highest corner. */
			const double ground = FirstPersonGroundZ(x, y);
			const double up = std::max(0.0, GetTileMaxPixelZ(house) + r.at[2] * OBJECT_HEIGHT_PER_METRE - ground);
			Put(*a, place, x, y, up, std::atan2(r.out[1], r.out[0]), now);
			return true;
		}
		case Place::Round: {
			if (!PickLand(t, middle, false, x, y) || Taken(x, y, 24)) return false;
			const ThreeDAnimal *a = PickKind(t, TileVirtXY(static_cast<int>(x), static_cast<int>(y)), ThreeDAnimal::ROUND, {ThreeDAnimal::Kind::Animal, ThreeDAnimal::Kind::Dog}, night);
			if (a == nullptr) return false;
			Put(*a, place, x, y, 0, InteractiveRandomRange(360) * M_PI / 180.0, now);
			return true;
		}
		case Place::Forest: {
			/* The game of the forest with a clearing at the places of its parts, of their kinds (the doe, the deer, the
			 * fox, the wolf cub, the hare, the squirrel, the hedgehog); of the original forests among their trees, of the
			 * climate (the deer, the elk, the bear). */
			double heading = 0;
			std::span<const std::string> kinds;
			if (PickGameForestPlace(middle, false, x, y, heading, kinds)) {
				const ThreeDAnimal *a = PickNamed(kinds, night, {ThreeDAnimal::Kind::Animal, ThreeDAnimal::Kind::Dog});
				if (a == nullptr || Taken(x, y, 8)) return false;
				Put(*a, place, x, y, 0, heading, now);
				return true;
			}
			if (!PickForest(middle, x, y) || Taken(x, y, 24)) return false;
			const ThreeDAnimal *a = PickKind(t, TileVirtXY(static_cast<int>(x), static_cast<int>(y)), ThreeDAnimal::ROUND, {ThreeDAnimal::Kind::Animal}, night);
			if (a == nullptr) return false;
			Put(*a, place, x, y, 0, InteractiveRandomRange(360) * M_PI / 180.0, now);
			return true;
		}
		case Place::ForestNight: {
			/* At night the ghosts of the forest with a clearing at its places, of their kinds (the kobold). */
			double heading = 0;
			std::span<const std::string> kinds;
			if (!night || !PickGameForestPlace(middle, true, x, y, heading, kinds) || Taken(x, y, 8)) return false;
			const ThreeDAnimal *a = PickNamed(kinds, night, {ThreeDAnimal::Kind::Ghost});
			if (a == nullptr) return false;
			Put(*a, place, x, y, 0, heading, now);
			return true;
		}
		case Place::Ghost: {
			/* About the town (the mushroom warrior: about the clay towns, on their land), or in its streets. Billy only by
			 * the rails (Place::Rail, RAIL_RIDER). */
			const bool street = InteractiveRandomRange(2) == 0;
			if (street ? !PickPavement(t, middle, x, y) : !PickLand(t, middle, true, x, y)) return false;
			const ThreeDAnimal *a = PickKind(t, TileVirtXY(static_cast<int>(x), static_cast<int>(y)), street ? ThreeDAnimal::STREET : ThreeDAnimal::ROUND, {ThreeDAnimal::Kind::Ghost}, night);
			if (a == nullptr || a->name == RAIL_RIDER) return false;
			Put(*a, place, x, y, 0, InteractiveRandomRange(360) * M_PI / 180.0, now);
			return true;
		}
		case Place::Rail: {
			/* Any ghost out now that fits the land of the town nearest (its kind of towns, its theme), on the rails. */
			double heading = 0;
			if (!PickRail(middle, x, y, heading) || Taken(x, y, 2 * TILE_SIZE)) return false;
			const ThreeDAnimal *a = PickKind(t, TileVirtXY(static_cast<int>(x), static_cast<int>(y)), ThreeDAnimal::STREET | ThreeDAnimal::ROUND | ThreeDAnimal::RAIL, {ThreeDAnimal::Kind::Ghost}, night);
			if (a == nullptr) return false;
			Beast &b = Put(*a, place, x, y, 0, heading, now);
			/* Billy off along the track the way he faces (PickRailRide()); another about the line as anywhere. */
			b.rail_tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
			const bool along_x = std::abs(std::cos(heading)) > 0.5;
			b.rail_td = along_x ? (std::cos(heading) > 0 ? Trackdir::X_SW : Trackdir::X_NE) : (std::sin(heading) > 0 ? Trackdir::Y_SE : Trackdir::Y_NW);
			return true;
		}
		case Place::Church: {
			/* The ghost in its sheet at a church about the middle that has none yet, before its door. */
			const ThreeDAnimal *a = GetThreeDAnimal(CHURCH_GHOST);
			if (a == nullptr || !a->OutNow(night)) return false;
			const int cx = TileX(middle), cy = TileY(middle), r = static_cast<int>(REACH);
			for (int ty = std::max(1, cy - r); ty <= std::min<int>(Map::MaxY() - 1, cy + r); ty++) {
				for (int tx = std::max(1, cx - r); tx <= std::min<int>(Map::MaxX() - 1, cx + r); tx++) {
					const TileIndex church = TileXY(tx, ty);
					if (!IsChurchNorthTile(church)) continue;
					if (std::ranges::any_of(_beasts, [church](const Beast &o) { return o.place == Place::Church && o.church == church; })) continue;
					ChurchDoor(church, x, y);
					double mx, my, radius;
					ChurchMiddle(church, mx, my, radius);
					Beast &b = Put(*a, place, x, y, 0, std::atan2(y - my, x - mx), now);
					b.church = church;
					b.home_x = mx;
					b.home_y = my;
					return true;
				}
			}
			return false;
		}
		case Place::Farm: {
			/* A dog to a farm about the middle with fewer than its own, on its yard where one can stand. */
			for (const Industry *farm : FarmsNear(middle)) {
				const IndustryID id = farm->index;
				if (std::ranges::count_if(_beasts, [id](const Beast &o) { return o.place == Place::Farm && o.farm == id && o.state != State::Dead; }) >= FARM_DOGS_KEPT) continue;
				bool found = false;
				for (int tries = 0; tries < 12 && !found; tries++) {
					const TileIndex tile = farm->location.tile + TileDiffXY(InteractiveRandomRange(farm->location.w), InteractiveRandomRange(farm->location.h));
					if (!IsTileType(tile, TileType::Industry) || GetIndustryIndex(tile) != id) continue;
					x = TileX(tile) * TILE_SIZE + 2 + InteractiveRandomRange(TILE_SIZE - 4);
					y = TileY(tile) * TILE_SIZE + 2 + InteractiveRandomRange(TILE_SIZE - 4);
					found = InFarmYard(id, x, y) && !Taken(x, y, 6);
				}
				if (!found) return false;
				const ThreeDAnimal *a = PickFarmDog(night);
				if (a == nullptr) return false;
				Beast &b = Put(*a, place, x, y, 0, InteractiveRandomRange(360) * M_PI / 180.0, now);
				b.farm = id;
				return true;
			}
			return false;
		}
		case Place::Pasture: {
			/* A dog to a pasture about the middle without its own, somewhere along its fence, looking in. */
			for (TileIndex north : PasturesNear(middle)) {
				if (std::ranges::any_of(_beasts, [north](const Beast &o) { return o.place == Place::Pasture && o.pasture == north && o.state != State::Dead; })) continue;
				const PastureRing ring(north);
				ring.At(InteractiveRandomRange(1000) / 1000.0 * ring.Length(), x, y);
				const double ox = x, oy = y;
				if (!OnMap(x, y) || !NavNearestWalkable(x, y, 1) || std::hypot(x - ox, y - oy) > TILE_SIZE / 4.0) return false;
				const ThreeDAnimal *a = PickFarmDog(night);
				if (a == nullptr) return false;
				const double mx = (ring.x0 + ring.x1) / 2, my = (ring.y0 + ring.y1) / 2;
				Beast &b = Put(*a, place, x, y, 0, std::atan2(my - y, mx - x), now);
				b.pasture = north;
				b.home_x = mx;
				b.home_y = my;
				return true;
			}
			return false;
		}
		case Place::Pet:
			return false;
	}
	return false;
}

/** How many churches about the middle (ChurchGhost()), so many as have their ghosts at most (CHURCHES_KEPT). */
uint ChurchesNear(TileIndex middle)
{
	uint n = 0;
	const int cx = TileX(middle), cy = TileY(middle), r = static_cast<int>(REACH);
	for (int ty = std::max(1, cy - r); ty <= std::min<int>(Map::MaxY() - 1, cy + r) && n < CHURCHES_KEPT; ty++) {
		for (int tx = std::max(1, cx - r); tx <= std::min<int>(Map::MaxX() - 1, cx + r) && n < CHURCHES_KEPT; tx++) {
			if (IsChurchNorthTile(TileXY(tx, ty))) n++;
		}
	}
	return n;
}

/** How many of a place are about the middle. */
uint Count(Place place)
{
	return static_cast<uint>(std::ranges::count_if(_beasts, [&](const Beast &b) { return b.place == place && b.state != State::Dead; }));
}

/** The walker's dog, the company's (CompanyProperties::walker_pet); empty none. */
std::string_view PetName()
{
	const Company *c = Company::GetIfValid(_local_company);
	return c != nullptr ? std::string_view(c->walker_pet) : std::string_view();
}

/** The walker's dog as it should be: there with the walker out, beside it; gone without. */
void KeepPet(bool walker, double wx, double wy, uint64_t now)
{
	auto it = std::ranges::find(_beasts, Place::Pet, &Beast::place);
	const std::string_view pet = PetName();
	const ThreeDAnimal *a = pet.empty() ? nullptr : GetThreeDAnimal(pet);
	if (!walker || a == nullptr) {
		if (it != _beasts.end()) _beasts.erase(it);
		return;
	}
	if (it != _beasts.end() && it->kind == a->name) return;
	if (it != _beasts.end()) _beasts.erase(it);
	double x = wx + FOLLOW_M * OBJECT_UNITS_PER_METRE, y = wy;
	if (!a->flies) NavNearestWalkable(x, y, 1);
	Beast &b = Put(*a, Place::Pet, x, y, a->flies ? 2.0 * OBJECT_HEIGHT_PER_METRE : 0, 0, now);
	b.state = State::Follow;
}

/** Every tick: they live on; those left far behind go; now and then one more where a place has fewer than it keeps. */
const IntervalTimer<TimerGameTick> _animals_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	const TileIndex middle = _game_mode == GameMode::Normal ? ThreeDLifeMiddle() : INVALID_TILE;
	if (middle == INVALID_TILE || GetThreeDAnimals().empty()) {
		for (const Beast &b : _beasts) {
			const ThreeDAnimal *a = KindOf(b);
			if (a != nullptr) MarkDirty(b, *a);
		}
		_beasts.clear();
		return;
	}
	const uint64_t now = TimerGameTick::counter;
	double wx = 0, wy = 0;
	const bool walker = FirstPersonWalkerAt(wx, wy);
	const bool night = IsNightNow();
	if (walker) {
		if (std::hypot(wx - _walker_last_x, wy - _walker_last_y) > 0.01) _walker_moved = now;
		_walker_last_x = wx;
		_walker_last_y = wy;
		_walker_moving = now - _walker_moved < STILL_TICKS;
		_walker_heading = FirstPersonWalkerHeading();
	}
	KeepPet(walker, wx, wy, now);

	for (auto it = _beasts.begin(); it != _beasts.end();) {
		Beast &b = *it;
		const ThreeDAnimal *a = KindOf(b);
		const TileIndex at = TileVirtXY(static_cast<int>(std::clamp(b.x, 0.0, (Map::SizeX() - 1.0) * TILE_SIZE)), static_cast<int>(std::clamp(b.y, 0.0, (Map::SizeY() - 1.0) * TILE_SIZE)));
		const bool gone = a == nullptr || (b.place != Place::Pet && DistanceManhattan(at, middle) > GONE) || (b.state == State::Dead && now >= b.until) ||
				((b.place == Place::Ghost || b.place == Place::Rail || b.place == Place::ForestNight || b.place == Place::Church) && !a->OutNow(night)) || (b.place == Place::Seller && now >= b.until && b.state == State::Idle) ||
				(b.place == Place::Church && !IsChurchNorthTile(b.church));
		if (gone) {
			if (a != nullptr) MarkDirty(b, *a);
			if (b.place == Place::Seller) _next_seller = now + SELLER_AWAY_TICKS / 2 + InteractiveRandomRange(SELLER_AWAY_TICKS);
			it = _beasts.erase(it);
			continue;
		}
		const double ox = b.x, oy = b.y, oh = b.heading;
		const int oframe = b.frame;
		const std::string opose = b.pose;
		Update(b, *a, now, walker, wx, wy);
		b.frame = FrameOf(b, now);
		const bool moved = b.x != ox || b.y != oy || b.heading != oh || b.frame != oframe || b.pose != opose;
		if (moved) MarkDirty(b, *a);
		b.z = static_cast<int>(std::lround(FirstPersonGroundZ(b.x, b.y) + b.up));
		if (moved) MarkDirty(b, *a);
		++it;
	}

	if (now % 29 != 0) return;
	/* The town nearest the middle, within reach; the ghosts of the railway lines come out of the towns too, with the
	 * nearest town's land (its kind of towns, its theme). */
	const Town *t = ClosestTownFromTile(middle, TOWN_REACH);
	const Town *any = t != nullptr ? t : ClosestTownFromTile(middle, UINT_MAX);
	if (any == nullptr) return;
	struct Wanted {
		Place place;
		uint kept;
	};
	const Wanted wanted[] = {
		{Place::Street, STREET_KEPT}, {Place::Door, DOOR_KEPT}, {Place::Roof, ROOF_KEPT}, {Place::Round, ROUND_KEPT},
		{Place::Ghost, GHOSTS_KEPT}, {Place::Seller, now >= _next_seller ? SELLERS_KEPT : 0u}, {Place::Rail, RAIL_KEPT},
		{Place::Forest, FOREST_KEPT}, {Place::ForestNight, night ? FOREST_GHOSTS_KEPT : 0u}, {Place::Church, CHURCHES_KEPT},
		{Place::Farm, FARM_DOGS_KEPT}, {Place::Pasture, 1},
	};
	/* One a time, the place picked in turn. The forests' game and ghosts are there without a town near too. */
	const Wanted &w = wanted[(now / 29) % std::size(wanted)];
	if (t == nullptr && w.place != Place::Rail && w.place != Place::Forest && w.place != Place::ForestNight && w.place != Place::Farm && w.place != Place::Pasture) return;
	/* A church's ghost to each church about the middle; the dogs to each farm. */
	const uint kept = w.place == Place::Church ? (night ? ChurchesNear(middle) : 0u) : w.place == Place::Farm ? static_cast<uint>(FarmsNear(middle).size()) * w.kept :
			w.place == Place::Pasture ? static_cast<uint>(PasturesNear(middle).size()) * w.kept : w.kept;
	if (Count(w.place) < kept) Spawn(w.place, middle, t != nullptr ? t : any, night, now);
});

/** The figure of its pose and the frame of it: its pose's, or a passage into it from the one before (with bones, as the dragons'). */
std::string Figure(const Beast &b, int &frame)
{
	frame = b.frame;
	const std::string own = FigureIn(b, b.pose);
	if (b.from_pose == b.pose || b.from_pose.empty() || b.pose.empty()) return own;
	const uint64_t since = TimerGameTick::counter - b.pose_start;
	const std::string passage = fmt::format("{}/{}@{}>{}", b.kind, b.from_pose, b.from_frame, b.pose);
	const ThreeDVehicleObject *p = GetThreeDFigure(passage);
	if (p == nullptr || p->blend_from == nullptr || since >= static_cast<uint64_t>(p->blend_ticks)) return own;
	frame = b.frame * p->blend_ticks + static_cast<int>(since);
	return passage;
}

/** Which way it faces as the figures count it, in steps of ten degrees (a few pictures for every turn). */
float Facing(const Beast &b)
{
	return static_cast<float>(std::round(b.heading * 180.0 / M_PI / 10.0) * 10.0);
}

/** Whether one can be hit: not the walker's dog or a seller, not falling or dead. */
bool Hittable(const Beast &b)
{
	return b.place != Place::Pet && b.place != Place::Seller && !b.inside && b.state != State::Dying && b.state != State::Dead;
}

/** One hit: so much less health; it falls after its last; one provoked fights from now on, a peaceful one runs. */
void Strike(Beast &b, int damage)
{
	const ThreeDAnimal *a = KindOf(b);
	if (a == nullptr) return;
	const uint64_t now = TimerGameTick::counter;
	b.health -= damage;
	if (b.health <= 0) {
		if (a->Has("smrt")) {
			b.state = State::Dying;
			SetPose(b, "smrt", now, 1.0, true);
		} else {
			b.state = State::Dead;
			b.until = now; // gone (no pose of falling)
		}
		return;
	}
	if (a->temper == ThreeDAnimal::Temper::Provoked) b.angry = true;
	/* Billy rides on. */
	if (RidesRails(b, *a)) return;
	if (Fights(b, *a)) {
		if (b.state != State::Attack) b.state = State::Chase;
		return;
	}
	/* A peaceful one runs from the walker. */
	double wx = 0, wy = 0;
	if (!FirstPersonWalkerAt(wx, wy) || !Moves(*a)) return;
	const double dx = b.x - wx, dy = b.y - wy, d = std::max(1e-6, std::hypot(dx, dy));
	b.tx = b.x + dx / d * FLEE_M * OBJECT_UNITS_PER_METRE;
	b.ty = b.y + dy / d * FLEE_M * OBJECT_UNITS_PER_METRE;
	if (!a->flies) NavNearestWalkable(b.tx, b.ty, 2);
	b.way.clear();
	b.state = State::Flee;
	b.until = now + 300;
}

/** Its body: about as wide as it is long, as high as it is tall, from its feet. */
bool InBody(const Beast &b, const ThreeDAnimal &a, double x, double y, double z, double more_m)
{
	const double r = (std::max(0.4, static_cast<double>(std::max(a.size[0], a.size[1])) * 0.5) + more_m) * OBJECT_UNITS_PER_METRE;
	if (std::hypot(b.x - x, b.y - y) > r) return false;
	return z >= b.z - (0.3 + more_m) * OBJECT_HEIGHT_PER_METRE && z <= b.z + (a.size[2] + more_m) * OBJECT_HEIGHT_PER_METRE;
}

} // namespace

/**
 * The dogs, the animals and the ghosts on the map, as figures at their "meritko", three times that (the sellers, people,
 * as the girls; the dogs of the farms as their animals); one floating ("vyska_m") floats as much higher as it is bigger (the turned view, and north with the
 * houses as objects).
 */
void AddThreeDAnimalsToViewport(ZoomLevel zoom)
{
	for (const Beast &b : _beasts) {
		const ThreeDAnimal *a = KindOf(b);
		if (a == nullptr || b.inside) continue;
		/* A few poses of each, no passages between them (ThreeDMapPose()). */
		const std::string figure = FigureIn(b, b.pose);
		/* The dogs of a farm as big as the cows and the sheep of its pastures, their true size ("meritko_zvirat"). */
		const bool farm = b.place == Place::Farm || b.place == Place::Pasture;
		const float scale = farm ? 1.0f : a->scale * (a->kind == ThreeDAnimal::Kind::Seller ? 1.0f : THREE_D_MAP_CREATURE_SCALE);
		const int z = b.z + static_cast<int>(std::lround(a->hover_m * OBJECT_HEIGHT_PER_METRE * (scale - 1.0f)));
		AddThreeDFigureToViewport(figure, scale, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)), z, Facing(b), zoom, INVALID_TILE, 0, ThreeDMapPose(figure, b.frame));
	}
}

/** And in the view from an eye, at their true size; the sellers, people, a head taller as the girls (AddCabFigure()). */
void AddThreeDAnimalsToEye()
{
	for (const Beast &b : _beasts) {
		const ThreeDAnimal *a = KindOf(b);
		if (a == nullptr || b.inside) continue;
		int frame;
		const std::string figure = Figure(b, frame);
		const float people = a->kind == ThreeDAnimal::Kind::Seller ? THREE_D_EYE_PEOPLE_SCALE : 1.0f;
		AddCabFigure(figure, a->scale * EYE_OF_MAP * people, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)), b.z, Facing(b), frame);
	}
}

/** The mushrooms about the walker in the view from its eyes, at their true size; at night the glowing ones. */
void AddThreeDMushroomsToEye()
{
	double wx = 0, wy = 0;
	if (!FirstPersonWalkerAt(wx, wy) || GetThreeDMushrooms().empty()) return;
	const bool night = IsNightNow();
	ForMushroomsNear(wx, wy, MUSHROOM_SEEN_TILES, [&](TileIndex, const MushroomAt &at) {
		const std::string &figure = night && !at.kind->night.empty() ? at.kind->night : at.kind->name;
		AddCabFigure(figure, MUSHROOM_SCALE, static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y)), static_cast<int>(std::lround(FirstPersonGroundZ(at.x, at.y))), at.facing, 0);
	});
}

/** The mushroom nearest a point within reach (metres), its name in Czech or English; nullptr none. */
const ThreeDMushroom *ThreeDMushroomNear(double x, double y, double reach_m)
{
	const ThreeDMushroom *best = nullptr;
	double best_d = reach_m * OBJECT_UNITS_PER_METRE;
	ForMushroomsNear(x, y, 1, [&](TileIndex, const MushroomAt &at) {
		const double d = std::hypot(at.x - x, at.y - y);
		if (d <= best_d) {
			best_d = d;
			best = at.kind;
		}
	});
	return best;
}

/** The rig: the mushroom nearest a point, so many tiles about: where it is and what it is; false none. */
bool ThreeDMushroomPlace(double x, double y, int tiles, double &mx, double &my, std::string &name)
{
	double best = 1e18;
	ForMushroomsNear(x, y, tiles, [&](TileIndex, const MushroomAt &at) {
		const double d = std::hypot(at.x - x, at.y - y);
		if (d >= best) return;
		best = d;
		mx = at.x;
		my = at.y;
		name = at.kind->name;
	});
	return best < 1e18;
}

/** The mushroom nearest a point within reach picked: gone till it grows again. @return whether one was */
bool PickThreeDMushroom(double x, double y, double reach_m)
{
	std::optional<uint64_t> best;
	double best_d = reach_m * OBJECT_UNITS_PER_METRE;
	ForMushroomsNear(x, y, 1, [&](TileIndex, const MushroomAt &at) {
		const double d = std::hypot(at.x - x, at.y - y);
		if (d <= best_d) {
			best_d = d;
			best = at.key;
		}
	});
	if (!best.has_value()) return false;
	_mushrooms_picked[*best] = TimerGameTick::counter;
	return true;
}

/**
 * A blow of the walker's hammer or sword: the one it reaches in front of it takes it (Strike()).
 * @return whether one was hit
 */
bool HitThreeDAnimal(double x, double y, double heading, double reach_m, int damage)
{
	for (Beast &b : _beasts) {
		const ThreeDAnimal *a = KindOf(b);
		if (a == nullptr || !Hittable(b) || b.up > 2.0 * OBJECT_HEIGHT_PER_METRE) continue;
		const double dx = b.x - x, dy = b.y - y;
		const double gap_m = std::hypot(dx, dy) / OBJECT_UNITS_PER_METRE - std::max(a->size[0], a->size[1]) * 0.4;
		if (gap_m > reach_m) continue;
		const double off = std::abs(std::remainder(std::atan2(dy, dx) - heading, 2 * M_PI));
		if (off > 70.0 * M_PI / 180.0) continue;
		Strike(b, damage);
		return true;
	}
	return false;
}

/** An arrow, a bolt, a bullet or a rocket at a point: the one whose body it is in takes it. */
bool HitThreeDAnimalAt(double x, double y, double z, int damage)
{
	for (Beast &b : _beasts) {
		const ThreeDAnimal *a = KindOf(b);
		if (a == nullptr || !Hittable(b) || !InBody(b, *a, x, y, z, 0)) continue;
		Strike(b, damage);
		return true;
	}
	return false;
}

/** A rocket's blast: every one within its reach struck so hard. @return how many */
int BlastThreeDAnimals(double x, double y, double z, double radius_m, int damage)
{
	int struck = 0;
	for (Beast &b : _beasts) {
		const ThreeDAnimal *a = KindOf(b);
		if (a == nullptr || !Hittable(b) || !InBody(b, *a, x, y, z, radius_m)) continue;
		Strike(b, damage);
		struck++;
	}
	return struck;
}

/** The health they took from the walker since the last asking (asked by the view from the first person, taken through a command). */
uint TakeThreeDAnimalDamage()
{
	uint taken = _damage;
	_damage = 0;
	return taken;
}

/**
 * A seller of dogs within reach of a point (the walker), as it is now: what it sells; nullptr none.
 * @param reach_m metres
 */
const ThreeDDogSeller *ThreeDDogSellerNear(double x, double y, double reach_m)
{
	_sellers_seen.clear();
	for (const Beast &b : _beasts) {
		if (b.place != Place::Seller || b.offer.empty()) continue;
		if (std::hypot(b.x - x, b.y - y) > reach_m * OBJECT_UNITS_PER_METRE) continue;
		ThreeDDogSeller &s = _sellers_seen.emplace_back();
		s.name = b.kind;
		s.offer = b.offer;
		s.x = b.x;
		s.y = b.y;
		return &s;
	}
	return nullptr;
}

/**
 * The rig: one of a kind put at a place now (where it lives by its kind: a seller with its offer, a ghost as a
 * ghost, the others in a street); with an empty name one of each place's.
 * @return whether one came
 */
bool SendThreeDAnimal(std::string_view name, double x, double y)
{
	if (!OnMap(x, y)) return false;
	const uint64_t now = TimerGameTick::counter;
	const ThreeDAnimal *a = GetThreeDAnimal(name);
	if (a == nullptr) return false;
	const Place place = a->kind == ThreeDAnimal::Kind::Seller ? Place::Seller : a->kind == ThreeDAnimal::Kind::Ghost ? Place::Ghost : (a->places & ThreeDAnimal::DOOR) ? Place::Door : (a->places & ThreeDAnimal::ROUND) ? Place::Round : Place::Street;
	if (!a->flies) NavNearestWalkable(x, y, 4);
	double wx = 0, wy = 0;
	const double facing = FirstPersonWalkerAt(wx, wy) ? std::atan2(wy - y, wx - x) : InteractiveRandomRange(360) * M_PI / 180.0;
	Beast &b = Put(*a, place, x, y, 0, facing, now);
	if (place == Place::Seller) {
		for (const ThreeDAnimal &s : GetThreeDAnimals()) {
			if (s.sold && b.offer.size() < 5) b.offer.push_back(s.name);
		}
		b.until = now + SELLER_TICKS;
	}
	return true;
}

/** The names of the places (Place), to the rig. */
static const char *const PLACES[] = {"ulice", "dvere", "strecha", "okoli", "strasidlo", "koleje", "prodavac", "pes chodce", "les", "les v noci", "kostel", "farma", "pastvina"};

/** The rig: where the first of a place (its name, PLACES) is, world units; false none. */
bool ThreeDAnimalOfPlace(std::string_view place, double &x, double &y)
{
	for (const Beast &b : _beasts) {
		if (PLACES[static_cast<int>(b.place)] != place) continue;
		x = b.x;
		y = b.y;
		return true;
	}
	return false;
}

/** The rig: those about, a line each. */
std::string DescribeThreeDAnimals()
{
	static const char *const STATES[] = {"stoji", "chodi", "honi", "utoci", "utika", "jde za chodcem", "pada", "lezi"};
	std::string out = fmt::format("{} zvirat (druhu v indexech {}), pes chodce: {}", _beasts.size(), GetThreeDAnimals().size(), PetName().empty() ? std::string_view("-") : PetName());
	for (const Beast &b : _beasts) {
		out += fmt::format("\n  {} ({}) {} na {:.1f},{:.1f} vyska {:.1f} m, smer {:.0f}, poza {} snimek {}, zdravi {}{}{}", b.kind, PLACES[static_cast<int>(b.place)], STATES[static_cast<int>(b.state)],
				b.x / TILE_SIZE, b.y / TILE_SIZE, b.up / OBJECT_HEIGHT_PER_METRE, b.heading * 180.0 / M_PI, b.pose.empty() ? "-" : b.pose, b.frame, b.health, b.angry ? ", vyprovokovany" : "",
				b.place != Place::Church ? std::string() : fmt::format(", kostel {},{}{}{}", TileX(b.church), TileY(b.church), b.inside ? ", uvnitr/zmizel" : "", b.going_in ? ", jde dovnitr" : ""));
		if (!b.offer.empty()) {
			out += ", prodava:";
			for (const std::string &o : b.offer) out += " " + o;
		}
	}
	return out;
}
