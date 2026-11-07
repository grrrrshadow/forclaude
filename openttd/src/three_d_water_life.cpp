/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_water_life.cpp The world under the water (the player, 7. 11.: "make the world under the water"; the
 * colleague's objekty/voda, hra/3d/PAR18-7-11-PRO-HRU.md parts 2 and 3).
 *
 * What lies on the beds is seen only from the eyes of the walker diving: on each tile of the sea a few of its plants,
 * now and then a starfish, an octopus moving its arms, rarely the big octopus; on a river's and a lake's bed their
 * grass. Always in the same places, by the tile (its hash), in colours of their palettes. Over the surface of the
 * rivers and the lakes the water lilies and, along some of their banks, the cattails in clusters, from the eyes and
 * on the map; the canals have none.
 *
 * The fish swim only about the walker, as the animals of the land live (three_d_animals.cpp), never saved: in the sea
 * schools of the small ones, the tuna, the fish of the rod; in the rivers the brown fish and the bass; the shark and
 * the orca rarely, one of each at most, only where the sea is deep (two tiles from its shores). They go as far as
 * their poses walk ("krok_m"), turn at the shores and over the beds, keep away from the diver, attack no one; the orca
 * sometimes leaps out of the water, to be seen from the shore and the boats. At night all of the sea glows its colours.
 */

#include "stdafx.h"
#include "three_d_water_life.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"
#include "cab_view.h"
#include "viewport_func.h"
#include "openttd.h"
#include "map_func.h"
#include "tile_map.h"
#include "tile_cmd.h"
#include "water_map.h"
#include "gfx_func.h"
#include "core/format.hpp"
#include "core/random_func.hpp"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"

#include <cmath>

#include "safeguards.h"

/** The walker of the view from the first person: where it stands, world units; false when the view is not open (first_person_gui.cpp). */
bool FirstPersonWalkerAt(double &x, double &y);
double FirstPersonWalkerHeading();
bool FirstPersonSwimmerEyeZ(double &z);

namespace {

/** Units of height of the game a metre is under the water: what is there is lifted with the bed as the eyes see it (EyeLevelZ()). */
constexpr double M_UP = OBJECT_HEIGHT_PER_METRE / EYE_LEVEL_STRETCH;
constexpr int BED_TILES = 2;            ///< what lies on the beds is drawn so many tiles about the eyes (they see 22 m under the water)
constexpr double SEEN_M = 30.0;         ///< and the fish so near
constexpr double SPAWN_NEAR_M = 10.0;   ///< the fish come so far from the walker ...
constexpr double SPAWN_FAR_M = 30.0;    ///< ... to so far
constexpr double BIG_SPAWN_FAR_M = 60.0; ///< the shark and the orca to so far
constexpr double GONE_M = 80.0;         ///< and go when farther
constexpr int NEAR_WATER_TILES = 3;     ///< they come when the walker is so near the water
constexpr uint SCHOOLS_KEPT = 3;        ///< schools of the small fish in the sea about the walker
constexpr uint RIVER_SCHOOLS_KEPT = 2;  ///< and in a river
constexpr uint SCHOOL_MIN = 4, SCHOOL_MAX = 8; ///< fish in a school
constexpr uint LONE_KEPT = 4;           ///< the fish swimming alone (the tuna, the fish of the rod)
constexpr uint BIG_CHANCE = 30;         ///< a shark, an orca comes one time in so many it may (rarely)
constexpr double BIG_FROM_M = 3.0;      ///< so long or longer it is one of those (the shark 4 m, the orca 7 m)
constexpr double LONE_FROM_M = 1.0;     ///< so long or longer it swims alone (the tuna 1.5 m)
constexpr double DODGE_M = 2.5;         ///< a fish keeps so far from the diver's eyes and its own half length
constexpr double CLEAR_M = 0.3;         ///< and so far over the bed and under the surface
constexpr uint64_t JUMP_EVERY = 1500;   ///< the orca leaps about so many ticks after the last time (and up to three times as long)
constexpr uint64_t REST_TICKS = 300;    ///< and rests ("klid") so long, now and then
constexpr double LEAP_M = 2.0;          ///< it leaps out of the water so high over its height, metres
constexpr double LEAP_PITCH = 40.0;     ///< its head up so many degrees as it leaps out, as much down as it falls in
/** The animals lying on the sea's bed, one on a tile in so many (the player, 7. 11.: "the octopus may be on the bed"). */
constexpr std::pair<std::string_view, uint32_t> BED_ANIMALS[] = {{"hvezdice", 3}, {"chobotnicka", 6}, {"chobotnice", 20}};
constexpr uint32_t BED_ANIMAL_OTHERS = 10; ///< another lying on the bed, one in so many
constexpr uint32_t LILY_TILES = 2;      ///< water lilies on a river's or a lake's tile one in so many
constexpr uint32_t CATTAIL_BANKS = 45;  ///< cattails along so many banks in a hundred (the player: "the cattails somewhere, nothing elsewhere")
constexpr double CATTAIL_APART_M = 0.55; ///< the cattails of a cluster so far apart along the bank
constexpr double CATTAIL_OUT_M = 0.25;  ///< and in the water from it so far ...
constexpr double CATTAIL_OUT_MORE_M = 0.45; ///< ... and up to so much farther

/** Of the tile, a number of its own for the things by chance on it (each its key). */
uint32_t TileRandom(TileIndex t, uint32_t key)
{
	uint32_t x = t.base() * 0x9E3779B1u + key * 0x85EBCA77u + 0x27D4EB2Fu;
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/** A part of one of a number between 0 and 1, of its bits from a place. */
double Fraction(uint32_t h, int bit)
{
	return static_cast<double>((h >> bit) & 0xFFF) / 4095.0;
}

/** The water there: none, the sea, or that of the land (a river, a lake, a canal). */
enum class Water : uint8_t { None, Sea, Inland };

Water WaterAt(double x, double y)
{
	if (x < 0 || y < 0 || x >= Map::SizeX() * TILE_SIZE || y >= Map::SizeY() * TILE_SIZE) return Water::None;
	const TileIndex t = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	if (!IsTileType(t, TileType::Water) || !IsWater(t)) return Water::None;
	return GetWaterClass(t) == WaterClass::Sea ? Water::Sea : Water::Inland;
}

/** How deep the water is at a point as the eyes see it, metres; 0 none. */
double DepthM(double x, double y)
{
	const std::optional<double> surface = EyeWaterSurfaceZ(x, y), bed = EyeWaterBedZ(x, y);
	if (!surface.has_value() || !bed.has_value()) return 0;
	return std::max(0.0, (*surface - *bed) / M_UP);
}

/** A colour of its palette, by a number; 0 one without. */
int PaletteOf(std::string_view figure, uint32_t n)
{
	const ThreeDVehicleObject *f = GetThreeDFigure(figure);
	return f != nullptr ? f->PaletteState(n) : 0;
}

/** How a fish swims: in a school, alone, or one of the big ones (by its length as it is seen). */
enum class Kind : uint8_t { School, Lone, Big };

Kind KindOf(const ThreeDWaterAnimal &a)
{
	const double length = a.size[0] * a.scale;
	if (length >= BIG_FROM_M) return Kind::Big;
	return a.on_rod || length >= LONE_FROM_M ? Kind::Lone : Kind::School;
}

/** One swimming. */
struct Swimmer {
	std::string kind;       ///< the animal of the water (ThreeDWaterAnimal)
	Water water = Water::Sea; ///< the water it keeps to
	double x = 0, y = 0;    ///< world units
	double z = 0;           ///< its middle, units of height of the game (lifted as the bed is, EyeLevelZ())
	double heading = 0;     ///< radians, 0 along +x
	double want = 0;        ///< the heading it turns to
	double level = 0.5;     ///< how high between the bed and the surface it would swim, 0 to 1
	float pitch = 0;        ///< degrees its head is raised
	int state = 0;          ///< its colour of its palette
	uint32_t phase = 0;     ///< its frames' start
	uint32_t school = 0;    ///< the school it is of; 0 alone
	double slot[3] = {};    ///< its place in its school by the first of it: metres ahead, to the left, up
	uint64_t turn_at = 0;   ///< it turns another way then
	std::string pose;       ///< its pose: empty swimming ("plave"), "klid" resting, "vyskok" leaping out
	uint64_t pose_start = 0;
	uint64_t pose_until = 0;
	uint64_t jump_at = 0;   ///< the orca leaps out of the water then
};

std::vector<Swimmer> _swimmers;
uint32_t _next_school = 1;

/** Its figure in its pose: as it swims the animal's own figure ("plave", ThreeDWaterAnimal). */
std::string FigureOf(const Swimmer &s)
{
	return s.pose.empty() ? s.kind : s.kind + "/" + s.pose;
}

/** Metres a tick it swims: its pose's step over the round of its frames ("krok_m"), as big as it is; one without, half its length a second. */
double SpeedM(const ThreeDWaterAnimal &a, std::string_view pose)
{
	const ThreeDVehicleObject *f = GetThreeDFigure(a.Has("plave") ? a.name + "/plave" : a.name);
	double speed = f != nullptr && f->walk_step > 0 && f->frames >= 1 ? f->walk_step * a.scale / (f->frames * f->frame_ticks) : a.size[0] * a.scale * 0.5 / 37.0;
	if (pose == "klid") speed *= 0.15;
	return speed;
}

/** The frame of its pose now: the leap once through, the others round. */
int FrameOf(const Swimmer &s, uint64_t now)
{
	const ThreeDVehicleObject *f = GetThreeDFigure(FigureOf(s));
	if (f == nullptr || f->frames < 2) return 0;
	if (s.pose == "vyskok") return static_cast<int>(std::min<uint64_t>((now - s.pose_start) / f->frame_ticks, f->frames - 1));
	return f->FrameAt(now, s.phase);
}

/** The heights it may swim between at a point, units of height: over the bed and under the surface by half of it and more; false no water. */
bool Between(const ThreeDWaterAnimal &a, double x, double y, double &low, double &high)
{
	const std::optional<double> surface = EyeWaterSurfaceZ(x, y), bed = EyeWaterBedZ(x, y);
	if (!surface.has_value() || !bed.has_value()) return false;
	const double half = a.size[2] * a.scale * 0.5 + CLEAR_M;
	low = *bed + half * M_UP;
	high = *surface - half * M_UP;
	if (high < low) low = high = (*bed + *surface) / 2;
	return true;
}

/** Whether it may swim at a point: its water, deep enough for it; the big ones only where the sea is deepest (two tiles from the shores). */
bool Swimmable(const ThreeDWaterAnimal &a, Water water, double x, double y)
{
	if (WaterAt(x, y) != water) return false;
	if (KindOf(a) == Kind::Big && EyeWaterDepth(x, y) < 2.0 * TILE_HEIGHT - 0.01) return false;
	return DepthM(x, y) >= a.size[2] * a.scale + 2 * CLEAR_M;
}

/** The first of its school (the one it follows), nullptr itself the first or alone. */
const Swimmer *LeaderOf(const Swimmer &s)
{
	if (s.school == 0) return nullptr;
	for (const Swimmer &o : _swimmers) {
		if (&o == &s) return nullptr;
		if (o.school == s.school) return &o;
	}
	return nullptr;
}

/** A tick of its swimming. */
void Swim(Swimmer &s, const ThreeDWaterAnimal &a, uint64_t now, bool diver, double dx, double dy, double dz)
{
	const double length_m = a.size[0] * a.scale;
	/* Its poses: the orca resting now and then and leaping out of the water. */
	if (!s.pose.empty() && now >= s.pose_until) {
		if (s.pose == "vyskok") s.jump_at = now + JUMP_EVERY + InteractiveRandomRange(2 * JUMP_EVERY);
		s.pose.clear();
	}
	if (s.pose.empty() && a.Has("vyskok") && now >= s.jump_at) {
		const ThreeDVehicleObject *f = GetThreeDFigure(s.kind + "/vyskok");
		s.pose = "vyskok";
		s.pose_start = now;
		s.pose_until = now + (f != nullptr ? static_cast<uint64_t>(f->frames * f->frame_ticks) : 150);
		s.level = 1.0;
	} else if (s.pose.empty() && a.Has("klid") && InteractiveRandomRange(2000) == 0) {
		s.pose = "klid";
		s.pose_start = now;
		s.pose_until = now + REST_TICKS + InteractiveRandomRange(REST_TICKS);
	}
	double speed_m = SpeedM(a, s.pose);
	double turn = (length_m >= BIG_FROM_M ? 1.0 : 3.0) * M_PI / 180.0;
	/* The leap: out of the water and back in an arc ahead, its head up and then down, its pose rolling it over (its
	 * animation does not lift it). */
	if (s.pose == "vyskok") {
		const std::optional<double> surface = EyeWaterSurfaceZ(s.x, s.y);
		if (!surface.has_value()) return;
		const double t = std::clamp(static_cast<double>(now - s.pose_start) / std::max<uint64_t>(1, s.pose_until - s.pose_start), 0.0, 1.0);
		const double tall_m = a.size[2] * a.scale;
		s.z = *surface + (-0.6 * tall_m + (tall_m + LEAP_M) * std::sin(M_PI * t)) * M_UP;
		s.pitch = static_cast<float>(LEAP_PITCH * std::cos(M_PI * t));
		const double nx = s.x + std::cos(s.heading) * speed_m * 1.5 * OBJECT_UNITS_PER_METRE, ny = s.y + std::sin(s.heading) * speed_m * 1.5 * OBJECT_UNITS_PER_METRE;
		if (WaterAt(nx, ny) == s.water) {
			s.x = nx;
			s.y = ny;
		}
		return;
	}

	/* Where it would go: now and then another way and height; in a school after its first, to its place by it. */
	if (now >= s.turn_at) {
		s.want = s.heading + (static_cast<int>(InteractiveRandomRange(121)) - 60) * M_PI / 180.0;
		s.level = s.pose == "vyskok" ? 1.0 : InteractiveRandomRange(1001) / 1000.0;
		s.turn_at = now + 60 + InteractiveRandomRange(240);
	}
	if (const Swimmer *first = LeaderOf(s); first != nullptr) {
		const double c = std::cos(first->heading), sn = std::sin(first->heading);
		const double tx = first->x + (s.slot[0] * c - s.slot[1] * sn) * OBJECT_UNITS_PER_METRE;
		const double ty = first->y + (s.slot[0] * sn + s.slot[1] * c) * OBJECT_UNITS_PER_METRE;
		const double far_m = std::hypot(tx - s.x, ty - s.y) / OBJECT_UNITS_PER_METRE;
		s.want = far_m > 0.2 ? std::atan2(ty - s.y, tx - s.x) : first->heading;
		s.level = first->level;
		speed_m *= std::clamp(0.7 + far_m, 0.7, 2.0);
	}
	/* Away from the diver, faster. */
	if (diver) {
		const double away_m = std::hypot(s.x - dx, s.y - dy) / OBJECT_UNITS_PER_METRE;
		const double up_m = std::abs(s.z - dz) / M_UP;
		if (away_m < DODGE_M + length_m * 0.5 && up_m < DODGE_M + length_m * 0.5) {
			s.want = std::atan2(s.y - dy, s.x - dx);
			speed_m *= 1.8;
			turn *= 2;
		}
	}
	/* Before it the shore or the bed too high (the shallows): it turns, there it does not go. */
	const double look = std::max(1.0, length_m) * OBJECT_UNITS_PER_METRE;
	const double ax = s.x + std::cos(s.heading) * look, ay = s.y + std::sin(s.heading) * look;
	if (!Swimmable(a, s.water, ax, ay)) {
		if (std::abs(std::remainder(s.want - s.heading, 2 * M_PI)) < M_PI / 2) {
			s.want = s.heading + (InteractiveRandomRange(2) == 0 ? 1 : -1) * (100 + InteractiveRandomRange(61)) * M_PI / 180.0;
			s.turn_at = now + 90;
		}
		speed_m = 0;
		turn *= 2;
	}
	s.heading = std::remainder(s.heading + std::clamp(std::remainder(s.want - s.heading, 2 * M_PI), -turn, turn), 2 * M_PI);
	const double nx = s.x + std::cos(s.heading) * speed_m * OBJECT_UNITS_PER_METRE, ny = s.y + std::sin(s.heading) * speed_m * OBJECT_UNITS_PER_METRE;
	if (speed_m > 0 && Swimmable(a, s.water, nx, ny)) {
		s.x = nx;
		s.y = ny;
	}
	/* Up and down to its height, its head raised so; within the water always. */
	double low = 0, high = 0;
	if (!Between(a, s.x, s.y, low, high)) return;
	const double target = low + s.level * (high - low);
	const double most = std::max(0.002, speed_m * 0.35) * M_UP;
	const double dzs = std::clamp(target - s.z, -most, most);
	s.z = std::clamp(s.z + dzs, low, high);
	const float pitch = speed_m > 0 ? static_cast<float>(std::clamp(std::atan2(dzs / M_UP, speed_m) * 180.0 / M_PI, -25.0, 25.0)) : 0.0f;
	s.pitch += (pitch - s.pitch) * 0.1f;
}

/** The fish of a water about the walker: how many schools, how many alone, and of each big one. */
uint Count(Water water, Kind kind, std::string_view name = {})
{
	uint n = 0;
	uint32_t last_school = 0;
	for (const Swimmer &s : _swimmers) {
		if (s.water != water) continue;
		const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(s.kind);
		if (a == nullptr || KindOf(*a) != kind || (!name.empty() && s.kind != name)) continue;
		if (s.school != 0) {
			if (s.school == last_school) continue;
			last_school = s.school;
		}
		n++;
	}
	return n;
}

/** Whether the walker is near a water: any of the tiles about it. */
bool NearWater(double x, double y)
{
	const int tx = static_cast<int>(x) / static_cast<int>(TILE_SIZE), ty = static_cast<int>(y) / static_cast<int>(TILE_SIZE);
	for (int dy = -NEAR_WATER_TILES; dy <= NEAR_WATER_TILES; dy++) {
		for (int dx = -NEAR_WATER_TILES; dx <= NEAR_WATER_TILES; dx++) {
			if (WaterAt((tx + dx + 0.5) * TILE_SIZE, (ty + dy + 0.5) * TILE_SIZE) != Water::None) return true;
		}
	}
	return false;
}

/** One coming, or a school of it, at a point. */
void Put(const ThreeDWaterAnimal &a, Water water, double x, double y, double heading, uint64_t now)
{
	const Kind kind = KindOf(a);
	const uint count = kind == Kind::School ? SCHOOL_MIN + InteractiveRandomRange(SCHOOL_MAX - SCHOOL_MIN + 1) : 1;
	const uint32_t school = count > 1 ? _next_school++ : 0;
	if (_next_school == 0) _next_school = 1;
	const double length_m = a.size[0] * a.scale;
	double low = 0, high = 0;
	if (!Between(a, x, y, low, high)) return;
	const double level = InteractiveRandomRange(1001) / 1000.0;
	for (uint i = 0; i < count; i++) {
		Swimmer s;
		s.kind = a.name;
		s.water = water;
		s.heading = s.want = heading;
		s.level = level;
		s.state = PaletteOf(a.name, InteractiveRandom());
		s.phase = InteractiveRandom();
		s.school = school;
		if (i > 0) {
			/* Behind and beside the first, a body or two apart. */
			const double spread = length_m * 2.5 + 0.3;
			s.slot[0] = -(InteractiveRandomRange(1001) / 1000.0) * spread;
			s.slot[1] = (InteractiveRandomRange(2001) / 1000.0 - 1.0) * spread;
		}
		s.x = x + (s.slot[0] * std::cos(heading) - s.slot[1] * std::sin(heading)) * OBJECT_UNITS_PER_METRE;
		s.y = y + (s.slot[0] * std::sin(heading) + s.slot[1] * std::cos(heading)) * OBJECT_UNITS_PER_METRE;
		if (WaterAt(s.x, s.y) != water) {
			s.x = x;
			s.y = y;
		}
		double l = low, h = high;
		Between(a, s.x, s.y, l, h);
		s.z = l + level * (h - l);
		s.turn_at = now + InteractiveRandomRange(200);
		s.jump_at = now + JUMP_EVERY / 2 + InteractiveRandomRange(JUMP_EVERY);
		_swimmers.push_back(std::move(s));
	}
}

/** Now and then one more where fewer are about the walker than are kept: in turn a school, one alone, a big one (rarely). */
void Spawn(double wx, double wy, uint64_t now)
{
	if (!NearWater(wx, wy)) return;
	static const Kind ORDER[] = {Kind::School, Kind::Lone, Kind::Big};
	const Kind kind = ORDER[(now / 16) % std::size(ORDER)];
	const double far = kind == Kind::Big ? BIG_SPAWN_FAR_M : SPAWN_FAR_M;
	const double angle = InteractiveRandomRange(3600) * M_PI / 1800.0;
	const double dist = (SPAWN_NEAR_M + InteractiveRandomRange(1001) / 1000.0 * (far - SPAWN_NEAR_M)) * OBJECT_UNITS_PER_METRE;
	const double x = wx + std::cos(angle) * dist, y = wy + std::sin(angle) * dist;
	const Water water = WaterAt(x, y);
	if (water == Water::None) return;
	std::vector<const ThreeDWaterAnimal *> kinds;
	for (const ThreeDWaterAnimal &a : GetThreeDWaterAnimals()) {
		if (!a.swims || a.sea != (water == Water::Sea) || KindOf(a) != kind) continue;
		if (kind == Kind::Big && Count(water, kind, a.name) > 0) continue;
		kinds.push_back(&a);
	}
	if (kinds.empty()) return;
	const uint kept = kind == Kind::School ? (water == Water::Sea ? SCHOOLS_KEPT : RIVER_SCHOOLS_KEPT) : kind == Kind::Lone ? LONE_KEPT : 2;
	if (Count(water, kind) >= kept) return;
	if (kind == Kind::Big && InteractiveRandomRange(BIG_CHANCE) != 0) return;
	const ThreeDWaterAnimal &a = *kinds[InteractiveRandomRange(static_cast<uint32_t>(kinds.size()))];
	if (!Swimmable(a, water, x, y)) return;
	Put(a, water, x, y, InteractiveRandomRange(3600) * M_PI / 1800.0, now);
}

/** Every tick: they swim; those left far behind go; now and then more come (Spawn()). */
const IntervalTimer<TimerGameTick> _water_life_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	double wx = 0, wy = 0;
	if (_game_mode != GameMode::Normal || !FirstPersonWalkerAt(wx, wy) || GetThreeDWaterAnimals().empty()) {
		_swimmers.clear();
		return;
	}
	const uint64_t now = TimerGameTick::counter;
	double dz = 0;
	const bool diver = FirstPersonSwimmerEyeZ(dz);
	for (auto it = _swimmers.begin(); it != _swimmers.end();) {
		const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(it->kind);
		if (a == nullptr || std::hypot(it->x - wx, it->y - wy) > GONE_M * OBJECT_UNITS_PER_METRE || WaterAt(it->x, it->y) != it->water) {
			it = _swimmers.erase(it);
			continue;
		}
		++it;
	}
	for (Swimmer &s : _swimmers) {
		const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(s.kind);
		if (a != nullptr) Swim(s, *a, now, diver, wx, wy, dz);
	}
	if (now % 16 == 0) Spawn(wx, wy, now);
});

/** What lies on a tile's bed in the view from the eyes: its plants, now and then an animal of the sea, each where its tile has it. */
void AddBedOfTile(TileIndex t, bool night, uint64_t now)
{
	if (!IsTileType(t, TileType::Water) || !IsWater(t)) return;
	const WaterClass wc = GetWaterClass(t);
	if (wc == WaterClass::Canal) return;
	const bool sea = wc == WaterClass::Sea;
	auto lay = [&](std::string_view name, float scale, float tall_m, bool glows, uint32_t h) {
		const double x = TileX(t) * static_cast<double>(TILE_SIZE) + (0.06 + 0.88 * Fraction(h, 0)) * TILE_SIZE;
		const double y = TileY(t) * static_cast<double>(TILE_SIZE) + (0.06 + 0.88 * Fraction(h, 12)) * TILE_SIZE;
		const std::optional<double> bed = EyeWaterBedZ(x, y);
		if (!bed.has_value() || DepthM(x, y) < tall_m * 1.1 + 0.2) return;
		const ThreeDVehicleObject *f = GetThreeDFigure(name);
		const int frame = f != nullptr ? f->FrameAt(now, h) : 0;
		AddCabWaterFigure(name, scale, x, y, *bed, *bed, static_cast<float>(h % 360), frame, PaletteOf(name, h >> 9), night && glows);
	};
	/* The plants of its bed: in the sea three to five, in a river or a lake none to two of its grass. */
	std::vector<const ThreeDWaterPlant *> plants;
	for (const ThreeDWaterPlant &p : GetThreeDWaterPlants()) {
		if (p.sea == sea && !p.by_bank && p.leaf_m <= 0) plants.push_back(&p);
	}
	const uint32_t h = TileRandom(t, 1);
	if (!plants.empty()) {
		const uint count = sea ? 3 + h % 3 : h % 3;
		for (uint i = 0; i < count; i++) {
			const uint32_t hh = TileRandom(t, 10 + i);
			const ThreeDWaterPlant &p = *plants[(hh >> 24) % plants.size()];
			lay(p.name, p.scale, p.size[2] * p.scale, p.glows, hh);
		}
	}
	if (!sea) return;
	/* The animals lying on the sea's bed, each on a tile in so many. */
	uint32_t key = 50;
	for (const ThreeDWaterAnimal &a : GetThreeDWaterAnimals()) {
		key++;
		if (!a.sea || a.swims) continue;
		uint32_t one_in = BED_ANIMAL_OTHERS;
		for (const auto &[name, n] : BED_ANIMALS) {
			if (a.name == name) one_in = n;
		}
		const uint32_t hh = TileRandom(t, key);
		if ((hh >> 24) % one_in != 0) continue;
		lay(a.name, a.scale, a.size[2] * a.scale, a.glows, hh);
	}
}

/** Whether a tile's side is a bank: the tile beyond it not of the water swum in (land, the coast, a lock, a dock). */
bool BankBeyond(TileIndex t, DiagDirection d)
{
	const TileIndexDiffC step = TileIndexDiffCByDiagDir(d);
	const TileIndex next = TileAddWrap(t, step.x, step.y);
	if (next == INVALID_TILE) return false;
	return !IsTileType(next, TileType::Water) || !IsWater(next);
}

/** A water lily or a cattail of a tile of a river or a lake (ForRiverPlants()). */
struct RiverPlant {
	const ThreeDWaterPlant *plant;
	bool lily;              ///< a water lily: its leaves at the surface
	float size;             ///< as big
	double x, y;            ///< where it stands, world units
	double bed, surface;    ///< the bed and the surface there, units of height
	float facing;           ///< degrees
};

/**
 * The water lilies and the cattails of a tile of a river or a lake, where they are always (the colleague's objekty/voda
 * part 3; the player, 7. 11.: "the cattails along the banks of the rivers into the water by the bank", "somewhere,
 * nothing elsewhere, five of them together"): on about every other flat tile one to three lilies; along about half of
 * the banks five or so cattails standing in the water by the bank.
 */
template <typename F>
void ForRiverPlants(TileIndex t, F fn)
{
	if (!IsTileType(t, TileType::Water) || !IsWater(t) || GetWaterClass(t) != WaterClass::River) return;
	std::vector<const ThreeDWaterPlant *> lilies, cattails;
	for (const ThreeDWaterPlant &p : GetThreeDWaterPlants()) {
		if (p.sea) continue;
		if (p.leaf_m > 0) lilies.push_back(&p);
		if (p.by_bank) cattails.push_back(&p);
	}
	const double tx = TileX(t) * static_cast<double>(TILE_SIZE), ty = TileY(t) * static_cast<double>(TILE_SIZE);
	const uint32_t h = TileRandom(t, 100);
	if (!lilies.empty() && IsTileFlat(t) && h % LILY_TILES == 0) {
		const uint count = 1 + (h >> 4) % 3;
		for (uint i = 0; i < count; i++) {
			const uint32_t hh = TileRandom(t, 110 + i);
			const ThreeDWaterPlant &p = *lilies[(hh >> 24) % lilies.size()];
			const double x = tx + (0.15 + 0.7 * Fraction(hh, 0)) * TILE_SIZE, y = ty + (0.15 + 0.7 * Fraction(hh, 12)) * TILE_SIZE;
			const std::optional<double> surface = EyeWaterSurfaceZ(x, y), bed = EyeWaterBedZ(x, y);
			const double depth_m = DepthM(x, y);
			if (!surface.has_value() || !bed.has_value() || depth_m < 0.5) continue;
			/* As big as the water is deep at most: its highest leaf at the surface, its foot on the bed or over it. */
			const float size = static_cast<float>(std::min<double>(p.scale, (depth_m - 0.1) / p.leaf_m));
			fn(RiverPlant{&p, true, size, x, y, *bed, *surface, static_cast<float>((hh >> 8) % 360)});
		}
	}
	if (cattails.empty()) return;
	for (DiagDirection d : {DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
		if (!BankBeyond(t, d)) continue;
		const uint32_t hd = TileRandom(t, 200 + to_underlying(d));
		if (hd % 100 >= CATTAIL_BANKS) continue;
		/* Along the bank (its axis), in from it. */
		const TileIndexDiffC step = TileIndexDiffCByDiagDir(d);
		const bool along_y = step.x != 0;
		const double edge = along_y ? tx + (step.x > 0 ? TILE_SIZE : 0) : ty + (step.y > 0 ? TILE_SIZE : 0);
		const double in = along_y ? -step.x : -step.y;
		const uint count = 4 + (hd >> 8) % 3;
		const double middle = (0.25 + 0.5 * Fraction(hd, 12)) * TILE_SIZE;
		for (uint k = 0; k < count; k++) {
			const uint32_t hk = TileRandom(t, 300 + to_underlying(d) * 16 + k);
			const ThreeDWaterPlant &p = *cattails[(hk >> 24) % cattails.size()];
			const double along = middle + ((k - (count - 1) / 2.0) * CATTAIL_APART_M + (Fraction(hk, 0) - 0.5) * 0.3) * OBJECT_UNITS_PER_METRE;
			const double out = (CATTAIL_OUT_M + CATTAIL_OUT_MORE_M * Fraction(hk, 12)) * OBJECT_UNITS_PER_METRE;
			const double a = std::clamp(along, 0.5, TILE_SIZE - 0.5);
			const double x = along_y ? edge + in * out : tx + a, y = along_y ? ty + a : edge + in * out;
			const std::optional<double> surface = EyeWaterSurfaceZ(x, y), bed = EyeWaterBedZ(x, y);
			if (!surface.has_value() || !bed.has_value()) continue;
			const float size = p.scale * static_cast<float>(0.85 + 0.3 * Fraction(hk, 4));
			fn(RiverPlant{&p, false, size, x, y, *bed, *surface, static_cast<float>((hk >> 16) % 360)});
		}
	}
}


} // namespace

/**
 * What lives in the water in the view from the eyes: diving, the beds about the eyes and the fish swimming; the orca
 * leaping out of the water also over it. At night all of the sea glowing.
 * @param under_water the walker's eyes under the surface
 */
void AddThreeDWaterLifeToEye(bool under_water)
{
	double wx = 0, wy = 0;
	if (!FirstPersonWalkerAt(wx, wy)) return;
	const bool night = IsNightNow();
	const uint64_t now = TimerGameTick::counter;
	if (under_water) {
		const int tx = static_cast<int>(wx) / static_cast<int>(TILE_SIZE), ty = static_cast<int>(wy) / static_cast<int>(TILE_SIZE);
		for (int dy = -BED_TILES; dy <= BED_TILES; dy++) {
			for (int dx = -BED_TILES; dx <= BED_TILES; dx++) {
				const int x = tx + dx, y = ty + dy;
				if (x < 1 || y < 1 || x >= static_cast<int>(Map::SizeX()) - 1 || y >= static_cast<int>(Map::SizeY()) - 1) continue;
				AddBedOfTile(TileXY(x, y), night, now);
			}
		}
	}
	for (const Swimmer &s : _swimmers) {
		const bool leaping = s.pose == "vyskok";
		if (!under_water && !leaping) continue;
		if (!leaping && std::hypot(s.x - wx, s.y - wy) > SEEN_M * OBJECT_UNITS_PER_METRE) continue;
		const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(s.kind);
		if (a == nullptr) continue;
		AddCabWaterFigure(FigureOf(s), a->scale, s.x, s.y, s.z, s.z, static_cast<float>(s.heading * 180.0 / M_PI), FrameOf(s, now), s.state, night && a->glows, s.pitch);
	}
}

/**
 * The water lilies and the cattails of a tile of a river or a lake (ForRiverPlants()) from the eyes, in the turned view
 * and facing north among the objects: from the eyes a lily's highest leaf a hair over the surface, a cattail standing
 * on the bed; on the map, the water not seen through there, a lily's leaves alone ("<jmeno>/hladina") and the cattails
 * on the surface.
 * @param ti the tile, of clear water
 */
void DrawThreeDRiverPlants(const TileInfo *ti)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return;
	if (!eye && _cur_dpi->zoom > ZoomLevel::Normal) return;
	ForRiverPlants(ti->tile, [&](const RiverPlant &r) {
		const ThreeDWaterPlant &p = *r.plant;
		const double z = r.lily ? r.surface + (0.03 - p.leaf_m * r.size) * OBJECT_HEIGHT_PER_METRE : (eye ? r.bed : r.surface);
		const double base = r.lily || !eye ? r.surface : r.bed;
		if (eye) {
			AddCabWaterFigure(p.name, r.size, r.x, r.y, z, base, r.facing, 0, 0, false);
		} else {
			AddThreeDFigureToViewport(r.lily ? p.name + "/hladina" : p.name, r.size, static_cast<int>(std::lround(r.x)), static_cast<int>(std::lround(r.y)), static_cast<int>(std::lround(z)), r.facing, _cur_dpi->zoom, ti->tile);
		}
	});
}

/**
 * The rig: one of a kind of the water (a school of the small ones) so far ahead of the walker, swimming its way; the
 * orca leaping out of the water at once.
 * @return whether it came
 */
bool SendThreeDWaterAnimal(std::string_view name, double ahead_m)
{
	double wx = 0, wy = 0;
	if (!FirstPersonWalkerAt(wx, wy)) return false;
	const ThreeDWaterAnimal *a = GetThreeDWaterAnimal(name);
	if (a == nullptr || !a->swims) return false;
	const double heading = FirstPersonWalkerHeading();
	const double x = wx + std::cos(heading) * ahead_m * OBJECT_UNITS_PER_METRE, y = wy + std::sin(heading) * ahead_m * OBJECT_UNITS_PER_METRE;
	const Water water = WaterAt(x, y);
	if (water == Water::None || (water == Water::Sea) != a->sea) return false;
	const size_t before = _swimmers.size();
	Put(*a, water, x, y, heading + M_PI / 2, TimerGameTick::counter);
	if (_swimmers.size() == before) return false;
	if (a->Has("vyskok")) _swimmers.back().jump_at = TimerGameTick::counter;
	return true;
}

/** The rig: those swimming about, a line each. */
std::string DescribeThreeDWaterLife()
{
	std::string out = fmt::format("{} plave (druhu zvirat ve vode {}, rostlin {})", _swimmers.size(), GetThreeDWaterAnimals().size(), GetThreeDWaterPlants().size());
	for (const Swimmer &s : _swimmers) {
		out += fmt::format("\n  {}{} na {:.1f},{:.1f} vyska {:.2f}, smer {:.0f}, hejno {}, barva {}{}", s.kind, s.pose.empty() ? std::string() : "/" + s.pose, s.x / TILE_SIZE, s.y / TILE_SIZE, s.z,
				s.heading * 180.0 / M_PI, s.school, s.state, s.water == Water::Sea ? ", more" : ", reka");
	}
	return out;
}

/** The rig: the water lilies and the cattails of the tiles about a tile, a line each. */
std::string DescribeThreeDRiverPlants(TileIndex tile)
{
	std::string out;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const TileIndex t = TileAddWrap(tile, dx, dy);
			if (t == INVALID_TILE) continue;
			ForRiverPlants(t, [&](const RiverPlant &r) {
				out += fmt::format("\n  {} na {:.2f},{:.2f} velikost {:.2f}, hloubka {:.2f} m, smer {:.0f}", r.plant->name, r.x / TILE_SIZE, r.y / TILE_SIZE, r.size, (r.surface - r.bed) / M_UP, r.facing);
			});
		}
	}
	if (out.empty()) out = "zadne";
	/* And on the whole map: how many of each, the lily nearest the tile where. */
	uint lilies = 0, cattails = 0, tiles = 0, flat = 0;
	std::string first;
	uint best = UINT_MAX;
	for (const auto t : Map::Iterate()) {
		if (!IsTileType(t, TileType::Water) || !IsWater(t) || GetWaterClass(t) != WaterClass::River) continue;
		tiles++;
		if (IsTileFlat(t)) flat++;
		ForRiverPlants(t, [&](const RiverPlant &r) {
			(r.lily ? lilies : cattails)++;
			if (r.lily && DistanceManhattan(t, tile) < best) {
				best = DistanceManhattan(t, tile);
				first = fmt::format("{:.2f},{:.2f} ({}, velikost {:.2f})", r.x / TILE_SIZE, r.y / TILE_SIZE, r.plant->name, r.size);
			}
		});
	}
	return out + fmt::format("\n  mapa: {} dlazdic reky ({} rovnych), leknin {}, orobincu {}, nejblizsi leknin {}", tiles, flat, lilies, cattails, first.empty() ? std::string("-") : first);
}
