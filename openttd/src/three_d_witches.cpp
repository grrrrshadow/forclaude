/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_witches.cpp The witches flying over the towns at night, a lantern and a hand torch shining (three_d_witches.h). */

#include "stdafx.h"
#include "three_d_witches.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"
#include "three_d.h"
#include "cab_view.h"
#include "openttd.h"
#include "map_func.h"
#include "town.h"
#include "town_map.h"
#include "house.h"
#include "viewport_func.h"
#include "landscape.h"
#include "core/random_func.hpp"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "debug.h"

#include <cmath>

#include "safeguards.h"

bool FirstPersonWalkerAt(double &x, double &y);

namespace {

constexpr uint MAX_WITCHES = 3;           ///< at most so many in the air at once
constexpr uint64_t NEXT_TICKS = 450;      ///< one more comes after so long (about a quarter of a minute)
constexpr double FLY_M = 16.0;            ///< how high over the ground they fly, metres (her torch reaching the ground: the colleague's shine 23 degrees down)
constexpr double OVER_ROOF_M = 8.0;       ///< and at least so high over a roof
constexpr double ROOF_GUESS_M = 30.0;     ///< a house's roof whose height is not known yet (its .glb not read)
constexpr double TURN = 0.8 * M_PI / 180; ///< how far a witch turns in a tick, radians
constexpr double CLIMB_M = 0.4;           ///< how far she rises or sinks in a tick, metres
constexpr double FALLBACK_STEP_M = 0.12;  ///< metres a tick of her true size, when her animation does not say ("krok_m")
constexpr double APPROACH_TILES = 14.0;   ///< how far from a town she comes from
constexpr uint WALKER_TOWN_TILES = 40;    ///< the walker's town, this near it (Manhattan): most come to it
constexpr float FACING_STEP = 10.0f;      ///< her pictures every so many degrees she turns (each made on the thread)
/** The girls at a stop are drawn twice their size on the map, as they are from the eyes: a witch "as big as them" so. */
constexpr float EYE_OF_MAP = 0.5f;

struct Witch {
	std::string kind;      ///< the witch of the indices
	float scale = 2;       ///< her size on the map ("meritko")
	double x = 0, y = 0;   ///< world units
	double lift = FLY_M;   ///< over the ground, metres
	double heading = 0;    ///< radians, the direction she flies
	double tx = 0, ty = 0; ///< where she flies to
	bool to_town = true;   ///< to a town's middle; else to a place beside one
	TownID town = TownID::Invalid(); ///< the town she flies to, or last flew over
	double last_dist = 1e18; ///< how far from where she flies to she was a tick ago
	uint32_t phase = 0;    ///< her frames and her bobbing, each her own
	int frame = 0;
	int z = 0;             ///< height units, drawn at
	bool falling = false;  ///< shot down (HitThreeDWitchAt(), BlastThreeDWitches()): falls, and is gone at the ground
	double fall_ms = 0;    ///< how fast she falls, metres a second
};

std::vector<Witch> _witches;
uint64_t _last_came = 0;

const ThreeDWitch *Kind(std::string_view name)
{
	for (const ThreeDWitch &w : GetThreeDWitches()) {
		if (w.name == name) return &w;
	}
	return nullptr;
}

/** The figure she flies by: her animation "let", else her first. */
const ThreeDVehicleObject *Flying(const ThreeDWitch &c)
{
	return GetThreeDFigure(c.Figure(c.Has("let") ? std::string_view("let") : std::string_view(c.animations.front())));
}

bool OnMap(double x, double y)
{
	return x >= TILE_SIZE && y >= TILE_SIZE && x < (Map::SizeX() - 1) * TILE_SIZE && y < (Map::SizeY() - 1) * TILE_SIZE;
}

/** How many fly at once: one, two from four towns, three from ten. */
uint Wanted()
{
	const size_t towns = Town::GetNumItems();
	if (towns == 0) return 0;
	return std::min<uint>(MAX_WITCHES, 1 + (towns >= 4 ? 1 : 0) + (towns >= 10 ? 1 : 0));
}

/** A town by the interface's random numbers (Town::GetRandom() takes the game's), another than one where there are more; nullptr none. */
const Town *PickTown(TownID not_this)
{
	const size_t towns = Town::GetNumItems();
	if (towns == 0) return nullptr;
	const Town *pick = nullptr;
	for (int tries = 0; tries < 4 && (pick == nullptr || (pick->index == not_this && towns > 1)); tries++) {
		uint n = InteractiveRandomRange(static_cast<uint32_t>(towns));
		for (const Town *t : Town::Iterate()) {
			if (n-- == 0) {
				pick = t;
				break;
			}
		}
	}
	return pick;
}

/** The middle of a town, world units. */
void TownMiddle(const Town *t, double &x, double &y)
{
	x = TileX(t->xy) * TILE_SIZE + TILE_SIZE / 2.0;
	y = TileY(t->xy) * TILE_SIZE + TILE_SIZE / 2.0;
}

/** Where she flies next: over a town she came to, on to another (a place beside it when it is the only one); from beside one, over a town. */
void PickNext(Witch &w)
{
	if (w.to_town && Town::GetNumItems() < 2) {
		const double a = InteractiveRandomRange(360) * M_PI / 180.0, r = (10 + InteractiveRandomRange(9)) * TILE_SIZE;
		w.tx = std::clamp(w.tx + std::cos(a) * r, 2.0 * TILE_SIZE, (Map::SizeX() - 2.0) * TILE_SIZE);
		w.ty = std::clamp(w.ty + std::sin(a) * r, 2.0 * TILE_SIZE, (Map::SizeY() - 2.0) * TILE_SIZE);
		w.to_town = false;
	} else if (const Town *t = PickTown(w.town); t != nullptr) {
		TownMiddle(t, w.tx, w.ty);
		w.town = t->index;
		w.to_town = true;
	}
	w.last_dist = 1e18;
}

/** How high the roof of a house there is, units of height; the ground where there is none. */
double RoofAt(double x, double y)
{
	const double ground = FirstPersonGroundZ(x, y);
	if (!OnMap(x, y)) return ground;
	const TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	if (!IsTileType(tile, TileType::House)) return ground;
	HouseID north = GetHouseType(tile);
	const TileIndex origin = tile + GetHouseNorthPart(north);
	const ThreeDObject *obj = IsHouseCompleted(tile) ? GetThreeDHouseObjectAt(north, origin) : nullptr;
	if (obj != nullptr && obj->IsRead() && obj->top > 0) return ground + obj->top;
	return ground + ROOF_GUESS_M * OBJECT_HEIGHT_PER_METRE;
}

/** Metres a tick she flies: her animation's step over the round of its frames, as big as she is drawn. */
double StepM(const Witch &w, const ThreeDWitch &c)
{
	const ThreeDVehicleObject *f = Flying(c);
	if (f == nullptr || f->walk_step <= 0 || f->frames < 1) return FALLBACK_STEP_M * w.scale;
	return f->walk_step / (f->frames * f->frame_ticks) * w.scale;
}

/** The facing of her pictures, degrees (a few pictures for every turn). */
float Facing(const Witch &w)
{
	return static_cast<float>(std::round(w.heading * 180.0 / M_PI / FACING_STEP) * FACING_STEP);
}

/**
 * The lights of a witch flying: her lantern and her torch where they are in her frame, turned as her picture, as big as
 * she is drawn; what they are like her index's ("svetla"). Only of the frames read already: nothing is read for them.
 * @param eye of the view from an eye (as big as she is drawn there), else of the map
 */
void LightsOf(const Witch &w, const ThreeDWitch &c, bool eye, std::vector<ThreeDWitchLight> &out)
{
	const ThreeDVehicleObject *f = Flying(c);
	const ThreeDVehicleShape *shape = f != nullptr ? f->TryGetShape(w.frame) : nullptr;
	if (shape == nullptr) return;
	const double size = w.scale * (eye ? EYE_OF_MAP : 1.0f);
	const double a = Facing(w) * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
	/* A place of her frame in the world. */
	auto place = [&](const std::vector<float> &list, size_t i, double &x, double &y, double &z) {
		const double px = list[i * 3] * size * OBJECT_UNITS_PER_METRE, py = list[i * 3 + 1] * size * OBJECT_UNITS_PER_METRE;
		x = w.x + px * ca - py * sa;
		y = w.y + px * sa + py * ca;
		z = w.z + list[i * 3 + 2] * size * OBJECT_HEIGHT_PER_METRE;
	};
	if (c.lantern_colour != 0 && c.lantern_reach_m > 0) {
		for (size_t i = 0; i < shape->lanterns.size() / 3; i++) {
			ThreeDWitchLight l;
			place(shape->lanterns, i, l.x, l.y, l.z);
			l.reach = c.lantern_reach_m * size * OBJECT_UNITS_PER_METRE;
			l.colour = Colour(c.lantern_colour);
			out.push_back(l);
		}
	}
	if (c.torch_colour == 0 || c.torch_reach_m <= 0) return;
	for (size_t i = 0; i < shape->torches.size() / 3 && i < shape->torch_aims.size() / 3; i++) {
		ThreeDWitchLight l;
		l.torch = true;
		place(shape->torches, i, l.x, l.y, l.z);
		double ax, ay, az;
		place(shape->torch_aims, i, ax, ay, az);
		/* Its way in metres' terms: z as long as x and y. */
		l.dx = ax - l.x;
		l.dy = ay - l.y;
		l.dz = (az - l.z) / OBJECT_HEIGHT_PER_METRE * OBJECT_UNITS_PER_METRE;
		const double len = std::sqrt(l.dx * l.dx + l.dy * l.dy + l.dz * l.dz);
		if (len < 1e-6) continue;
		l.dx /= len;
		l.dy /= len;
		l.dz /= len;
		l.reach = c.torch_reach_m * size * OBJECT_UNITS_PER_METRE;
		l.half_angle = c.torch_angle * M_PI / 360.0;
		l.colour = Colour(c.torch_colour);
		/* Where the beam falls on the ground, a little further than it reaches still seen. */
		if (l.dz < -0.05) {
			const double down = (l.z - FirstPersonGroundZ(l.x, l.y)) / OBJECT_HEIGHT_PER_METRE * OBJECT_UNITS_PER_METRE;
			const double along = down / -l.dz;
			if (along > 0 && along < l.reach * 1.3) l.ground = along;
		}
		out.push_back(l);
	}
}

/**
 * Her part of the screen dirty: herself, the light all round her lantern, the beam of her torch down to its pool --
 * only as far as they are now, a witch flying every tick.
 */
void MarkDirty(const Witch &w, const ThreeDWitch &c)
{
	const double up = OBJECT_HEIGHT_PER_METRE / OBJECT_UNITS_PER_METRE;
	const double body = std::max({c.size[0], c.size[1], c.size[2], 1.0f}) * w.scale * 0.6 * OBJECT_UNITS_PER_METRE + 2;
	double x0 = w.x - body, x1 = w.x + body, y0 = w.y - body, y1 = w.y + body, z0 = w.z - body * up, z1 = w.z + body * up;
	auto take = [&](double x, double y, double z, double r) {
		x0 = std::min(x0, x - r);
		x1 = std::max(x1, x + r);
		y0 = std::min(y0, y - r);
		y1 = std::max(y1, y + r);
		z0 = std::min(z0, z - r * up);
		z1 = std::max(z1, z + r * up);
	};
	std::vector<ThreeDWitchLight> lights;
	LightsOf(w, c, false, lights);
	for (const ThreeDWitchLight &l : lights) {
		if (!l.torch) {
			take(l.x, l.y, l.z, l.reach);
			continue;
		}
		const double len = l.ground > 0 ? l.ground : l.reach;
		take(l.x, l.y, l.z, 2);
		take(l.x + l.dx * len, l.y + l.dy * len, l.z + l.dz * len * up, len * std::tan(l.half_angle) * 1.3 + 2);
	}
	DirtyPlace place;
	place.kind = DirtyPlace::Kind::Box;
	place.x0 = x0;
	place.x1 = x1;
	place.y0 = y0;
	place.y1 = y1;
	place.z0 = std::max(0.0, z0);
	place.z1 = z1;
	DirtyPlaceScope scope(place);
	/* Facing north: the box's corners on the screen. */
	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (double x : {x0, x1}) {
		for (double y : {y0, y1}) {
			for (double z : {place.z0, z1}) {
				const Point pt = RemapCoords(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
				left = std::min(left, pt.x);
				right = std::max(right, pt.x);
				top = std::min(top, pt.y);
				bottom = std::max(bottom, pt.y);
			}
		}
	}
	MarkAllViewportsDirty(left - 2 * ZOOM_BASE, top - 2 * ZOOM_BASE, right + 2 * ZOOM_BASE, bottom + 2 * ZOOM_BASE);
}

/** A tick of her flight: turning towards where she flies, on as fast as her animation, over the roofs. */
void Update(Witch &w, const ThreeDWitch &c, uint64_t now)
{
	if (w.falling) {
		/* Shot down (the colleague: "she then falls, no animation of death, a fall is enough"): on a little way as she
		 * flew, ever faster down, her frame as it was. */
		const double dt = MILLISECONDS_PER_TICK / 1000.0;
		w.fall_ms += 9.81 * dt;
		w.lift -= w.fall_ms * dt;
		const double step = 0.5 * StepM(w, c) * OBJECT_UNITS_PER_METRE;
		const double nx = w.x + std::cos(w.heading) * step, ny = w.y + std::sin(w.heading) * step;
		if (OnMap(nx, ny)) {
			w.x = nx;
			w.y = ny;
		}
		w.z = static_cast<int>(std::lround(FirstPersonGroundZ(w.x, w.y) + std::max(0.0, w.lift) * OBJECT_HEIGHT_PER_METRE));
		return;
	}
	double dx = w.tx - w.x, dy = w.ty - w.y, dist = std::hypot(dx, dy);
	/* There, or past it (turning she may not come nearer): on. */
	if (dist < 1.5 * TILE_SIZE || (dist < 6.0 * TILE_SIZE && dist > w.last_dist)) {
		PickNext(w);
		dx = w.tx - w.x;
		dy = w.ty - w.y;
		dist = std::hypot(dx, dy);
	}
	w.last_dist = dist;
	const double turn = std::remainder(std::atan2(dy, dx) - w.heading, 2 * M_PI);
	w.heading += std::clamp(turn, -TURN, TURN);
	const double step = StepM(w, c) * OBJECT_UNITS_PER_METRE;
	double nx = w.x + std::cos(w.heading) * step, ny = w.y + std::sin(w.heading) * step;
	if (!OnMap(nx, ny)) {
		/* At the edge of the map: back to the middle of it. */
		w.tx = Map::SizeX() * TILE_SIZE / 2.0;
		w.ty = Map::SizeY() * TILE_SIZE / 2.0;
		w.to_town = false;
		w.last_dist = 1e18;
		nx = std::clamp(nx, 1.0 * TILE_SIZE, (Map::SizeX() - 1.01) * TILE_SIZE);
		ny = std::clamp(ny, 1.0 * TILE_SIZE, (Map::SizeY() - 1.01) * TILE_SIZE);
	}
	w.x = nx;
	w.y = ny;
	/* Over the ground and over the roofs where she is and a little ahead, bobbing a little. */
	const double ahead = 2.0 * TILE_SIZE;
	const double ground = FirstPersonGroundZ(w.x, w.y);
	const double roof = std::max(RoofAt(w.x, w.y), RoofAt(w.x + std::cos(w.heading) * ahead, w.y + std::sin(w.heading) * ahead));
	const double bob = 0.6 * std::sin((now + w.phase) * 0.03);
	const double want = std::max(FLY_M, (roof - ground) / OBJECT_HEIGHT_PER_METRE + OVER_ROOF_M) + bob;
	w.lift += std::clamp(want - w.lift, -CLIMB_M, CLIMB_M);
	w.z = static_cast<int>(std::lround(ground + w.lift * OBJECT_HEIGHT_PER_METRE));
	const ThreeDVehicleObject *f = Flying(c);
	w.frame = f != nullptr ? f->FrameAt(now, w.phase) : 0;
}

/** Every tick at night: they come one by one, fly on; by day there are none. */
const IntervalTimer<TimerGameTick> _witches_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	if (_game_mode != GameMode::Normal || !IsNightNow() || GetThreeDWitches().empty()) {
		for (const Witch &w : _witches) {
			if (const ThreeDWitch *c = Kind(w.kind); c != nullptr) MarkDirty(w, *c);
		}
		_witches.clear();
		return;
	}
	const uint64_t now = TimerGameTick::counter;
	if (_witches.size() < Wanted() && (_witches.empty() || now - _last_came >= NEXT_TICKS)) {
		const std::vector<ThreeDWitch> &kinds = GetThreeDWitches();
		const ThreeDWitch &c = kinds[InteractiveRandomRange(static_cast<uint32_t>(kinds.size()))];
		/* With the walker out, mostly to its town (the player, 22. 10.: the ghost and the witches were never met). */
		double wx = 0, wy = 0;
		const Town *near = FirstPersonWalkerAt(wx, wy) && InteractiveRandomRange(3) != 0 ? ClosestTownFromTile(TileVirtXY(static_cast<int>(wx), static_cast<int>(wy)), WALKER_TOWN_TILES) : nullptr;
		if (const Town *t = near != nullptr ? near : PickTown(TownID::Invalid()); t != nullptr) {
			/* From some way off a town, towards it. */
			double mx, my;
			TownMiddle(t, mx, my);
			const double a = InteractiveRandomRange(360) * M_PI / 180.0;
			const double x = std::clamp(mx + std::cos(a) * APPROACH_TILES * TILE_SIZE, 2.0 * TILE_SIZE, (Map::SizeX() - 2.0) * TILE_SIZE);
			const double y = std::clamp(my + std::sin(a) * APPROACH_TILES * TILE_SIZE, 2.0 * TILE_SIZE, (Map::SizeY() - 2.0) * TILE_SIZE);
			SendThreeDWitch(c.name, x, y, std::atan2(my - y, mx - x));
		}
		_last_came = now;
	}
	for (auto it = _witches.begin(); it != _witches.end();) {
		const ThreeDWitch *c = Kind(it->kind);
		if (c == nullptr) {
			it = _witches.erase(it);
			continue;
		}
		MarkDirty(*it, *c);
		Update(*it, *c, now);
		MarkDirty(*it, *c);
		/* Shot down, at the ground: gone. */
		if (it->falling && it->lift <= 0) {
			it = _witches.erase(it);
			continue;
		}
		++it;
	}
});

/**
 * Whether a point is in a witch's body as the eyes see it (her size there, EYE_OF_MAP): about her middle, where her
 * origin is, as wide as she is long and as high as she is.
 */
bool InBody(const Witch &w, const ThreeDWitch &c, double x, double y, double z, double margin_m)
{
	const double s = w.scale * EYE_OF_MAP;
	const double r = (std::max(0.6, static_cast<double>(std::max(c.size[0], c.size[1]))) * 0.5 * s + margin_m) * OBJECT_UNITS_PER_METRE;
	const double h = (std::max(0.6, static_cast<double>(c.size[2])) * 0.5 * s + margin_m) * OBJECT_HEIGHT_PER_METRE;
	return std::hypot(w.x - x, w.y - y) <= r && std::abs(w.z - z) <= h;
}

} // namespace

/**
 * A shot that hits a witch or the ghost flying (the colleague's hra/3d/PROSBA-NOC-ZBRANE-TRABANT-LAMPY.md: "an arrow,
 * a bullet and a rocket hit them as the dragons"): the one it is in falls.
 * @param x,y,z the point of the shot, world units and units of height
 * @return whether it hit one
 */
bool HitThreeDWitchAt(double x, double y, double z)
{
	for (Witch &w : _witches) {
		const ThreeDWitch *c = Kind(w.kind);
		if (w.falling || c == nullptr || !InBody(w, *c, x, y, z, 0)) continue;
		w.falling = true;
		return true;
	}
	return false;
}

/**
 * A blast (a rocket's, of a radius of 4 m): every witch and ghost within its reach falls.
 * @return how many fell
 */
int BlastThreeDWitches(double x, double y, double z, double radius_m)
{
	int fell = 0;
	for (Witch &w : _witches) {
		const ThreeDWitch *c = Kind(w.kind);
		if (w.falling || c == nullptr || !InBody(w, *c, x, y, z, radius_m)) continue;
		w.falling = true;
		fell++;
	}
	return fell;
}

/**
 * Send a witch flying from a place (the rig's 'test3d carodejnice'; at night they come by themselves): towards the
 * nearest town ahead of her, then on from town to town.
 * @param kind the witch of the indices, empty any
 * @param heading which way she flies at first, radians
 * @return false: no such witch, or not on the map
 */
bool SendThreeDWitch(std::string_view kind, double x, double y, double heading)
{
	const std::vector<ThreeDWitch> &kinds = GetThreeDWitches();
	if (kinds.empty() || !OnMap(x, y)) return false;
	const ThreeDWitch *c = kind.empty() ? &kinds[InteractiveRandomRange(static_cast<uint32_t>(kinds.size()))] : Kind(kind);
	if (c == nullptr) return false;
	Witch w;
	w.kind = c->name;
	w.scale = c->scale;
	w.x = x;
	w.y = y;
	w.heading = heading;
	w.phase = InteractiveRandom();
	/* Where she flies to: the town nearest the way she faces, else any. */
	double best = 1e18;
	for (const Town *t : Town::Iterate()) {
		double mx, my;
		TownMiddle(t, mx, my);
		const double dx = mx - x, dy = my - y, along = dx * std::cos(heading) + dy * std::sin(heading);
		const double d = std::hypot(dx, dy) + (along < 0 ? 1e9 : 0);
		if (d < best) {
			best = d;
			w.tx = mx;
			w.ty = my;
			w.town = t->index;
		}
	}
	if (w.town == TownID::Invalid()) {
		w.tx = x + std::cos(heading) * APPROACH_TILES * TILE_SIZE;
		w.ty = y + std::sin(heading) * APPROACH_TILES * TILE_SIZE;
		w.to_town = false;
	}
	w.lift = std::max(FLY_M, (RoofAt(x, y) - FirstPersonGroundZ(x, y)) / OBJECT_HEIGHT_PER_METRE + OVER_ROOF_M);
	w.z = static_cast<int>(std::lround(FirstPersonGroundZ(x, y) + w.lift * OBJECT_HEIGHT_PER_METRE));
	const ThreeDVehicleObject *f = Flying(*c);
	w.frame = f != nullptr ? f->FrameAt(TimerGameTick::counter, w.phase) : 0;
	Debug(misc, 1, "3D witch: {} comes at {:.0f},{:.0f}", w.kind, x / TILE_SIZE, y / TILE_SIZE);
	MarkDirty(w, *c);
	_witches.push_back(std::move(w));
	return true;
}

/** The witches on the map, as figures as big as she is drawn ("meritko"; the turned view, and facing north with the houses as objects). */
void AddThreeDWitchesToViewport(ZoomLevel zoom)
{
	for (const Witch &w : _witches) {
		const ThreeDWitch *c = Kind(w.kind);
		const ThreeDVehicleObject *f = c != nullptr ? Flying(*c) : nullptr;
		if (f == nullptr) continue;
		AddThreeDFigureToViewport(f->name, w.scale, static_cast<int>(std::lround(w.x)), static_cast<int>(std::lround(w.y)), w.z, Facing(w), zoom, INVALID_TILE, 0, w.frame);
	}
}

/** The witches in the view from an eye: as big as the girls at a stop are there (AddCabFigure()). */
void AddThreeDWitchesToEye()
{
	for (const Witch &w : _witches) {
		const ThreeDWitch *c = Kind(w.kind);
		const ThreeDVehicleObject *f = c != nullptr ? Flying(*c) : nullptr;
		if (f == nullptr) continue;
		AddCabFigure(f->name, w.scale * EYE_OF_MAP, static_cast<int>(std::lround(w.x)), static_cast<int>(std::lround(w.y)), w.z, Facing(w), w.frame);
	}
}

/**
 * The lights of the witches flying (LightsOf()): each one's lantern and torch.
 * @param eye of the view from an eye (as big as she is drawn there), else of the map
 */
std::vector<ThreeDWitchLight> GetThreeDWitchLights(bool eye)
{
	std::vector<ThreeDWitchLight> out;
	for (const Witch &w : _witches) {
		if (const ThreeDWitch *c = Kind(w.kind); c != nullptr) LightsOf(w, *c, eye, out);
	}
	return out;
}

/** The rig ('test3d carodejnice'): the witches flying, a line each. */
std::string DescribeThreeDWitches()
{
	std::string s = fmt::format("{} witches of the indices, {} flying{}", GetThreeDWitches().size(), _witches.size(), IsNightNow() ? "" : " (day: none fly)");
	for (const Witch &w : _witches) {
		s += fmt::format("; {} at {:.1f},{:.1f} {:.0f} m up facing {:.0f} frame {} to {:.0f},{:.0f}{}", w.kind, w.x / TILE_SIZE, w.y / TILE_SIZE, w.lift, Facing(w), w.frame, w.tx / TILE_SIZE, w.ty / TILE_SIZE, w.falling ? fmt::format(" falling {:.1f} m/s", w.fall_ms) : std::string());
	}
	for (const ThreeDWitchLight &l : GetThreeDWitchLights(false)) {
		s += fmt::format("; {} at {:.2f},{:.2f},{} reach {:.1f} tiles{}", l.torch ? "torch" : "lantern", l.x / TILE_SIZE, l.y / TILE_SIZE, static_cast<int>(l.z), l.reach / TILE_SIZE,
				l.torch ? fmt::format(" way {:.2f},{:.2f},{:.2f} ground {:.1f} tiles", l.dx, l.dy, l.dz, l.ground / TILE_SIZE) : std::string());
	}
	return s;
}
