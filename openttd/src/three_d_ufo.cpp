/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_ufo.cpp The flying saucer of the disasters and of the night (three_d_ufo.h). */

#include "stdafx.h"
#include "three_d_ufo.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"
#include "three_d.h"
#include "cab_view.h"
#include "openttd.h"
#include "map_func.h"
#include "tile_map.h"
#include "town.h"
#include "town_map.h"
#include "house.h"
#include "viewport_func.h"
#include "landscape.h"
#include "disaster_vehicle.h"
#include "roadveh.h"
#include "core/random_func.hpp"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "debug.h"

#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "safeguards.h"

bool FirstPersonWalkerAt(double &x, double &y);

namespace {

constexpr float MAP_SCALE = 2.0f;           ///< on the map so many times its size: the small UFO about a tile wide, as the original's sprite
constexpr double OVER_ROOF_M = 8.0;         ///< flying at night, its bottom at least so high over a roof
constexpr double ROOF_GUESS_M = 30.0;       ///< a house's roof whose height is not known yet (its .glb not read)
constexpr double TURN = 0.6 * M_PI / 180;   ///< how far it turns in a tick, radians
constexpr double CLIMB_M = 0.3;             ///< how far it rises or sinks in a tick flying, metres
constexpr double SINK_M = 0.12;             ///< and landing, or rising from the ground
constexpr double FALLBACK_STEP_M = 0.25;    ///< metres a tick of its true size, when its animation does not say ("krok_m")
constexpr double APPROACH_TILES = 16.0;     ///< how far from a town it comes from
constexpr uint WALKER_TOWN_TILES = 40;      ///< the walker's town, this near it (Manhattan): mostly to it
constexpr uint64_t COME_TICKS = 900;        ///< at night it comes so long after the night began or the last went off the map
constexpr uint64_t HOVER_TICKS = 1100;      ///< over a town it hovers, its beam down, so long (half a minute)
constexpr uint64_t LANDED_TICKS = 1800;     ///< landed, its crew out, so long
constexpr uint64_t CREW_OUT_TICKS = 90;     ///< its crew out so long after it landed, in so long before it rises
constexpr double CREW_ROUND_TICKS = 1300.0; ///< its crew goes round it in so many ticks
constexpr int LAND_TRIES = 32;              ///< places looked at for a landing about a town
constexpr float FACING_STEP = 10.0f;        ///< its pictures every so many degrees it turns
constexpr float EYE_OF_MAP = 0.5f;          ///< a ghost as the eye sees it (its "meritko" 2 its true size): its crew so

/** What the saucer of the night is doing. */
enum class Doing : uint8_t {
	Flying,   ///< to a town, or to where it lands
	Hovering, ///< over a town, its beam down, maybe a sheep in it
	Sinking,  ///< landing, its beam down
	Landed,   ///< on the ground, its crew walking round it
	Rising,   ///< up from the ground again
};

/** The saucer flying at night. */
struct Saucer {
	std::string kind;                ///< the saucer of the indices
	double x = 0, y = 0;             ///< world units
	double lift = 0;                 ///< its middle over the ground, metres
	double heading = 0;              ///< radians, the direction it flies
	double tx = 0, ty = 0;           ///< where it flies to
	bool to_land = false;            ///< to a place it lands at; else to a town's middle
	TownID town = TownID::Invalid(); ///< the town it flies to, or last flew over
	double last_dist = 1e18;         ///< how far from where it flies to it was a tick ago
	Doing doing = Doing::Flying;
	uint64_t since = 0;              ///< the tick it began doing it
	bool sheep = false;              ///< hovering: a sheep lifted up its beam
	uint32_t phase = 0;              ///< its frames, its own
	int frame = 0;
	int z = 0;                       ///< its middle, units of height
};

std::optional<Saucer> _saucer;
uint64_t _last_gone = 0;             ///< the tick the last went (or the night began without one)

/**
 * A saucer as drawn now: the night's, or a UFO of the disasters. Where its middle is in a view depends on how big it
 * is drawn there when what is given is its bottom (a disaster's UFO: the vehicle's height, on the car it lands on).
 */
struct Pose {
	const ThreeDUfo *u = nullptr;
	double x = 0, y = 0;         ///< world units
	double base = 0;             ///< units of height: its middle, or its bottom (base_is_bottom)
	bool base_is_bottom = false;
	float size = 1;              ///< so many times its true size (the eye's; the map's MAP_SCALE times more)
	float facing = 0;            ///< degrees
	int frame = 0;
	bool beam = false;           ///< its beam down to the ground
	double sheep = -1;           ///< a sheep up its beam: 0 on the ground, 1 at its bottom; < 0 none
	bool crew = false;           ///< its crew walking round it
	double crew_angle = 0;       ///< where round it, radians
	uint32_t phase = 0;
};

const ThreeDUfo *Kind(std::string_view name)
{
	for (const ThreeDUfo &u : GetThreeDUfos()) {
		if (u.name == name) return &u;
	}
	return nullptr;
}

/** The saucer the disasters' UFOs are drawn as: the first of the indices that is for them. */
const ThreeDUfo *DisasterKind()
{
	for (const ThreeDUfo &u : GetThreeDUfos()) {
		if (u.disaster && GetThreeDFigure(u.Figure("let")) != nullptr) return &u;
	}
	return nullptr;
}

/** The saucers that fly at night. */
std::vector<const ThreeDUfo *> NightKinds()
{
	std::vector<const ThreeDUfo *> out;
	for (const ThreeDUfo &u : GetThreeDUfos()) {
		if (u.night && GetThreeDFigure(u.Figure("let")) != nullptr) out.push_back(&u);
	}
	return out;
}

bool OnMap(double x, double y)
{
	return x >= TILE_SIZE && y >= TILE_SIZE && x < (Map::SizeX() - 1) * TILE_SIZE && y < (Map::SizeY() - 1) * TILE_SIZE;
}

/** Metres from its middle down to its bottom (the hole of its beam), at its true size. */
double BottomBelow(const ThreeDUfo &u)
{
	return u.has_beam ? -u.beam_from_z : u.size[2] / 2.0;
}

/** Its middle in a view, units of height, drawn so many times its true size there. */
double MiddleZ(const Pose &p, float s)
{
	return p.base_is_bottom ? p.base + BottomBelow(*p.u) * s * OBJECT_HEIGHT_PER_METRE : p.base;
}

/** The facing of its pictures, degrees (a few pictures for every turn). */
float Facing(double heading)
{
	return static_cast<float>(std::round(heading * 180.0 / M_PI / FACING_STEP) * FACING_STEP);
}

/** The facing of a vehicle going a direction, degrees: 0 along +x (south-west), 90 along +y (south-east). */
float DirectionFacing(Direction d)
{
	static const float FACINGS[] = {225, 180, 135, 90, 45, 0, 315, 270}; // N, NE, E, SE, S, SW, W, NW
	return FACINGS[to_underlying(d) & 7];
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

/** Metres a tick it flies: its animation's step over the round of its frames, as big as it is on the map. */
double StepM(const ThreeDUfo &u)
{
	const ThreeDVehicleObject *f = GetThreeDFigure(u.Figure("let"));
	const double size = u.night_scale * MAP_SCALE;
	if (f == nullptr || f->walk_step <= 0 || f->frames < 1) return FALLBACK_STEP_M * size;
	return f->walk_step / (f->frames * f->frame_ticks) * size;
}

/** The middle of a town, world units. */
void TownMiddle(const Town *t, double &x, double &y)
{
	x = TileX(t->xy) * TILE_SIZE + TILE_SIZE / 2.0;
	y = TileY(t->xy) * TILE_SIZE + TILE_SIZE / 2.0;
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

/** A place to land at by a town: bare flat land, the tiles round it too (it is about a tile wide on the map). */
bool PlaceToLand(const Town *t, double &x, double &y)
{
	for (int k = 0; k < LAND_TRIES; k++) {
		const double a = InteractiveRandomRange(360) * M_PI / 180.0, r = 5 + InteractiveRandomRange(6);
		const int tx = TileX(t->xy) + static_cast<int>(std::lround(std::cos(a) * r)), ty = TileY(t->xy) + static_cast<int>(std::lround(std::sin(a) * r));
		if (tx < 2 || ty < 2 || tx >= static_cast<int>(Map::SizeX()) - 2 || ty >= static_cast<int>(Map::SizeY()) - 2) continue;
		bool bare = true;
		for (int dx = -1; dx <= 1 && bare; dx++) {
			for (int dy = -1; dy <= 1 && bare; dy++) {
				const TileIndex tile = TileXY(tx + dx, ty + dy);
				if (!IsTileType(tile, TileType::Clear) || GetTileSlope(tile) != SLOPE_FLAT) bare = false;
			}
		}
		if (!bare) continue;
		x = tx * TILE_SIZE + TILE_SIZE / 2.0;
		y = ty * TILE_SIZE + TILE_SIZE / 2.0;
		return true;
	}
	return false;
}

/** On to another town. */
void FlyOn(Saucer &s)
{
	if (const Town *t = PickTown(s.town); t != nullptr) {
		TownMiddle(t, s.tx, s.ty);
		s.town = t->index;
	}
	s.to_land = false;
	s.doing = Doing::Flying;
	s.last_dist = 1e18;
}

/** How high it flies over where it is: over the roofs there and a little ahead, at least as high as it hovers. */
double CruiseLift(const Saucer &s, const ThreeDUfo &u)
{
	const double size = u.night_scale * MAP_SCALE;
	const double ahead = 2.0 * TILE_SIZE;
	const double ground = FirstPersonGroundZ(s.x, s.y);
	const double roof = std::max(RoofAt(s.x, s.y), RoofAt(s.x + std::cos(s.heading) * ahead, s.y + std::sin(s.heading) * ahead));
	return std::max(u.night_lift_m * size, (roof - ground) / OBJECT_HEIGHT_PER_METRE + OVER_ROOF_M + BottomBelow(u) * size);
}

/** A tick of the saucer of the night. */
void Update(Saucer &s, const ThreeDUfo &u, uint64_t now)
{
	const double size = u.night_scale * MAP_SCALE;
	switch (s.doing) {
		case Doing::Flying: {
			double dx = s.tx - s.x, dy = s.ty - s.y, dist = std::hypot(dx, dy);
			/* There, or past it (turning it may not come nearer). */
			if (dist < (s.to_land ? 0.25 : 1.5) * TILE_SIZE || (!s.to_land && dist < 6.0 * TILE_SIZE && dist > s.last_dist)) {
				s.since = now;
				if (s.to_land) {
					s.x = s.tx;
					s.y = s.ty;
					s.doing = Doing::Sinking;
					break;
				}
				/* Over a town: it hovers there, its beam down, a sheep up it now and then; or lands by the town. */
				const Town *t = Town::GetIfValid(s.town);
				const uint pick = InteractiveRandomRange(3);
				if (pick == 0 && t != nullptr && PlaceToLand(t, s.tx, s.ty)) {
					s.to_land = true;
					s.last_dist = 1e18;
					break;
				}
				s.doing = Doing::Hovering;
				s.sheep = pick == 1;
				break;
			}
			s.last_dist = dist;
			const double turn = std::remainder(std::atan2(dy, dx) - s.heading, 2 * M_PI);
			s.heading += std::clamp(turn, -TURN, TURN);
			/* Slowing down for where it lands, not to fly round it. */
			const double step = StepM(u) * OBJECT_UNITS_PER_METRE * (s.to_land ? std::clamp(dist / (3.0 * TILE_SIZE), 0.2, 1.0) : 1.0);
			double nx = s.x + std::cos(s.heading) * step, ny = s.y + std::sin(s.heading) * step;
			if (!OnMap(nx, ny)) {
				s.tx = Map::SizeX() * TILE_SIZE / 2.0;
				s.ty = Map::SizeY() * TILE_SIZE / 2.0;
				s.to_land = false;
				s.last_dist = 1e18;
				nx = std::clamp(nx, 1.0 * TILE_SIZE, (Map::SizeX() - 1.01) * TILE_SIZE);
				ny = std::clamp(ny, 1.0 * TILE_SIZE, (Map::SizeY() - 1.01) * TILE_SIZE);
			}
			s.x = nx;
			s.y = ny;
			s.lift += std::clamp(CruiseLift(s, u) - s.lift, -CLIMB_M, CLIMB_M);
			break;
		}

		case Doing::Hovering:
			s.lift += std::clamp(CruiseLift(s, u) - s.lift, -CLIMB_M, CLIMB_M);
			if (now - s.since >= HOVER_TICKS) FlyOn(s);
			break;

		case Doing::Sinking:
			s.lift = std::max(BottomBelow(u) * size, s.lift - SINK_M * size);
			if (s.lift <= BottomBelow(u) * size + 1e-6) {
				s.doing = Doing::Landed;
				s.since = now;
			}
			break;

		case Doing::Landed:
			if (now - s.since >= LANDED_TICKS) {
				s.doing = Doing::Rising;
				s.since = now;
			}
			break;

		case Doing::Rising:
			s.lift += SINK_M * size;
			if (s.lift >= CruiseLift(s, u)) FlyOn(s);
			break;
	}
	/* A little bobbing in the air, none on the ground. */
	const double bob = s.doing == Doing::Landed || s.doing == Doing::Sinking ? 0.0 : 0.4 * std::sin((now + s.phase) * 0.025);
	s.z = static_cast<int>(std::lround(FirstPersonGroundZ(s.x, s.y) + (s.lift + bob) * OBJECT_HEIGHT_PER_METRE));
	const ThreeDVehicleObject *f = GetThreeDFigure(u.Figure("let"));
	s.frame = f != nullptr ? f->FrameAt(now, s.phase) : 0;
}

/** Whether a disaster's UFO is coming down now: the small one onto the road vehicle it took, the big one onto the rails. */
bool Landing(const DisasterVehicle *v)
{
	if (v->state != 1) return false;
	if (v->subtype == ST_SMALL_UFO) {
		const RoadVehicle *target = RoadVehicle::GetIfValid(v->dest_tile.base());
		return target != nullptr && Delta(v->x_pos, target->x_pos) + Delta(v->y_pos, target->y_pos) <= static_cast<int>(TILE_SIZE) && v->z_pos > target->z_pos;
	}
	const int x = TileX(v->dest_tile) * TILE_SIZE + TILE_SIZE / 2, y = TileY(v->dest_tile) * TILE_SIZE + TILE_SIZE / 2;
	return Delta(v->x_pos, x) + Delta(v->y_pos, y) < 8 && v->z_pos > GetSlopePixelZ(v->x_pos, v->y_pos);
}

/** Each saucer as it is now: the night's, and the disasters' UFOs. */
void ForPoses(const std::function<void(const Pose &)> &each)
{
	const uint64_t now = TimerGameTick::counter;
	if (_saucer.has_value()) {
		const Saucer &s = *_saucer;
		if (const ThreeDUfo *u = Kind(s.kind); u != nullptr) {
			Pose p;
			p.u = u;
			p.x = s.x;
			p.y = s.y;
			p.base = s.z;
			p.size = u->night_scale;
			p.facing = Facing(s.heading);
			p.frame = s.frame;
			p.phase = s.phase;
			p.beam = s.doing == Doing::Hovering || s.doing == Doing::Sinking;
			if (s.doing == Doing::Hovering && s.sheep) {
				/* Up its beam over most of its hovering, then in. */
				const double t = static_cast<double>(now - s.since) / (HOVER_TICKS * 0.8);
				if (t < 1.0) p.sheep = t;
			}
			if (s.doing == Doing::Landed && now - s.since >= CREW_OUT_TICKS && now - s.since + CREW_OUT_TICKS < LANDED_TICKS) {
				p.crew = true;
				p.crew_angle = (now - s.since - CREW_OUT_TICKS) / CREW_ROUND_TICKS * 2 * M_PI;
			}
			each(p);
		}
	}
	const ThreeDUfo *u = DisasterKind();
	if (u == nullptr) return;
	const ThreeDVehicleObject *fly = GetThreeDFigure(u->Figure("let"));
	for (const DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype != ST_SMALL_UFO && v->subtype != ST_BIG_UFO) continue;
		Pose p;
		p.u = u;
		p.x = v->x_pos;
		p.y = v->y_pos;
		p.base = v->z_pos;
		p.base_is_bottom = true;
		p.size = v->subtype == ST_SMALL_UFO ? u->small_scale : u->big_scale;
		p.facing = DirectionFacing(v->direction);
		p.phase = v->index.base() * 977;
		p.frame = fly != nullptr ? fly->FrameAt(now, p.phase) : 0;
		p.beam = Landing(v);
		each(p);
	}
}

/** Who gets out of it where it lands: the figure walking of its crew (its pose "chuze"), and how big its ghost is; empty none. */
std::string CrewFigure(const ThreeDUfo &u, float &scale)
{
	const ThreeDAnimal *a = u.crew.empty() ? nullptr : GetThreeDAnimal(u.crew);
	if (a == nullptr) return {};
	scale = a->scale * EYE_OF_MAP;
	if (std::ranges::find(a->poses, "chuze") != a->poses.end()) return u.crew + "/chuze";
	return u.crew;
}

/** How far round its middle its crew walks, metres of its true size. */
double CrewRound(const ThreeDUfo &u)
{
	return std::max(u.size[0], u.size[1]) * 0.5 + 1.5;
}

using Put = std::function<void(const std::string &figure, float scale, double x, double y, double z, float facing, int frame)>;

/**
 * Draw a saucer in a view: itself, its beam as long as reaches the ground, the sheep up it, its crew round it.
 * @param view so many times its size it is drawn there (the map's MAP_SCALE, the eye's 1)
 */
void Draw(const Pose &p, float view, const Put &put)
{
	const ThreeDUfo &u = *p.u;
	const float s = p.size * view;
	const double hpm = OBJECT_HEIGHT_PER_METRE;
	const double mid = MiddleZ(p, s);
	const double bottom = mid - BottomBelow(u) * s * hpm;
	put(u.Figure("let"), s, p.x, p.y, mid, p.facing, p.frame);
	const double ground = FirstPersonGroundZ(p.x, p.y);
	if (p.beam && u.has_beam && bottom - ground > 0.5 * hpm) {
		/* The beam alone, from its bottom down to the ground: the model's cone made as big as reaches -- a cone the same
		 * at any size, only longer and wider at its foot (in steps, a few pictures for a saucer coming down). */
		double bs = (bottom - ground) / hpm / u.beam_len_m;
		bs = std::max(1.0 / 32, std::round(bs * 32) / 32);
		put(u.Figure("paprsek"), static_cast<float>(bs), p.x, p.y, bottom - bs * u.beam_from_z * hpm, p.facing, 0);
	}
	if (p.sheep >= 0 && u.has_sheep) {
		/* The sheep turning up the beam: its middle from the ground to the saucer's bottom. */
		const ThreeDVehicleObject *f = GetThreeDFigure(u.Figure("ovce"));
		/* (Read now on the map, as its picture is; from the eyes when it comes.) */
		const ThreeDVehicleShape *shape = f == nullptr ? nullptr : ViewportDrawingEye() ? f->TryGetShape(0) : f->GetShape(0);
		if (shape != nullptr) {
			const double middle = (shape->z0 + shape->z1) / 2.0, half = (shape->z1 - shape->z0) / 2.0 * s * hpm;
			const double want = ground + half + p.sheep * std::max(0.0, bottom - ground - 2 * half);
			put(u.Figure("ovce"), s, p.x, p.y, want - middle * s * hpm, static_cast<float>(std::fmod(p.facing + p.sheep * 720.0, 360.0)), 0);
		}
	}
	if (p.crew) {
		float scale = 1;
		const std::string crew = CrewFigure(u, scale);
		const ThreeDVehicleObject *f = crew.empty() ? nullptr : GetThreeDFigure(crew);
		if (f != nullptr) {
			const double r = CrewRound(u) * s * OBJECT_UNITS_PER_METRE;
			const double cx = p.x + std::cos(p.crew_angle) * r, cy = p.y + std::sin(p.crew_angle) * r;
			const float facing = static_cast<float>(std::fmod((p.crew_angle + M_PI / 2) * 180.0 / M_PI, 360.0));
			put(crew, scale * view, cx, cy, FirstPersonGroundZ(cx, cy), Facing(facing * M_PI / 180.0), f->FrameAt(TimerGameTick::counter, p.phase));
		}
	}
}

/** The part of the screen a saucer is in, as big as the map draws it: itself, its beam and its crew down to the ground. */
void MarkDirty(const Pose &p)
{
	const ThreeDUfo &u = *p.u;
	const float s = p.size * MAP_SCALE;
	const double reach = (std::max({u.size[0], u.size[1], 1.0f}) * 0.6 + (p.crew ? 2.5 : 0.0)) * s * OBJECT_UNITS_PER_METRE + 4;
	const double mid = MiddleZ(p, s);
	DirtyPlace place;
	place.kind = DirtyPlace::Kind::Box;
	place.x0 = p.x - reach;
	place.x1 = p.x + reach;
	place.y0 = p.y - reach;
	place.y1 = p.y + reach;
	place.z0 = std::max(0.0, std::min(mid, FirstPersonGroundZ(p.x, p.y)) - 8);
	place.z1 = mid + std::max(u.size[2], 1.0f) * s * OBJECT_HEIGHT_PER_METRE + 8;
	DirtyPlaceScope scope(place);
	/* Facing north: the box's corners on the screen. */
	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (double x : {place.x0, place.x1}) {
		for (double y : {place.y0, place.y1}) {
			for (double z : {place.z0, place.z1}) {
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

/** The saucers marked dirty a tick ago: their places again, as they move off them. */
std::vector<Pose> _marked;

void MarkAll()
{
	for (const Pose &p : _marked) MarkDirty(p);
	_marked.clear();
	ForPoses([](const Pose &p) {
		MarkDirty(p);
		_marked.push_back(p);
	});
}

/** Every tick: the saucer of the night comes, flies, goes at day; the saucers' places on the screen marked. */
const IntervalTimer<TimerGameTick> _ufo_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	const uint64_t now = TimerGameTick::counter;
	if (_game_mode != GameMode::Normal || !IsNightNow()) {
		_saucer.reset();
		_last_gone = now;
	} else if (!_saucer.has_value()) {
		/* A game begun or loaded at night: the night begun now. */
		if (_last_gone == 0) _last_gone = now;
		const std::vector<const ThreeDUfo *> kinds = NightKinds();
		if (!kinds.empty() && now - _last_gone >= COME_TICKS) {
			/* With the walker out, mostly to its town (as the witches). */
			double wx = 0, wy = 0;
			const Town *near = FirstPersonWalkerAt(wx, wy) && InteractiveRandomRange(3) != 0 ? ClosestTownFromTile(TileVirtXY(static_cast<int>(wx), static_cast<int>(wy)), WALKER_TOWN_TILES) : nullptr;
			if (const Town *t = near != nullptr ? near : PickTown(TownID::Invalid()); t != nullptr) {
				double mx, my;
				TownMiddle(t, mx, my);
				const double a = InteractiveRandomRange(360) * M_PI / 180.0;
				const double x = std::clamp(mx + std::cos(a) * APPROACH_TILES * TILE_SIZE, 2.0 * TILE_SIZE, (Map::SizeX() - 2.0) * TILE_SIZE);
				const double y = std::clamp(my + std::sin(a) * APPROACH_TILES * TILE_SIZE, 2.0 * TILE_SIZE, (Map::SizeY() - 2.0) * TILE_SIZE);
				SendThreeDUfo(x, y, std::atan2(my - y, mx - x));
			}
			_last_gone = now;
		}
	}
	if (_saucer.has_value()) {
		if (const ThreeDUfo *u = Kind(_saucer->kind); u != nullptr) {
			Update(*_saucer, *u, now);
		} else {
			_saucer.reset();
		}
	}
	if (_saucer.has_value() || !_marked.empty() || DisasterKind() != nullptr) MarkAll();
});

} // namespace

/**
 * Whether a disaster's vehicle is drawn as the saucer (ForPoses()) in the 3D views, its sprite and its shadow's left
 * out: the small and the big UFO, when the indices have a saucer for the disasters.
 */
bool ThreeDUfoDrawsDisaster(const Vehicle *v)
{
	if (v->type != VehicleType::Disaster) return false;
	if (v->subtype != ST_SMALL_UFO && v->subtype != ST_BIG_UFO && v->subtype != ST_SMALL_UFO_SHADOW && v->subtype != ST_BIG_UFO_SHADOW) return false;
	return DisasterKind() != nullptr;
}

/** The saucers on the map (the turned view, and facing north with the houses as objects). */
void AddThreeDUfosToViewport(ZoomLevel zoom)
{
	ForPoses([zoom](const Pose &p) {
		Draw(p, MAP_SCALE, [zoom](const std::string &figure, float scale, double x, double y, double z, float facing, int frame) {
			AddThreeDFigureToViewport(figure, scale, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), static_cast<int>(std::lround(z)), facing, zoom, INVALID_TILE, 0, frame);
		});
	});
}

/** The saucers in the view from an eye, at their true size. */
void AddThreeDUfosToEye()
{
	ForPoses([](const Pose &p) {
		Draw(p, 1.0f, [](const std::string &figure, float scale, double x, double y, double z, float facing, int frame) {
			AddCabFigure(figure, scale, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), static_cast<int>(std::lround(z)), facing, frame);
		});
	});
}

/**
 * The saucers' lights at night, as the witches' (GetThreeDWitchLights()): the light all round its rim, and its beam
 * down with its pool on the ground.
 * @param eye of the view from an eye (as big as it is drawn there), else of the map
 */
void AddThreeDUfoLights(bool eye, std::vector<ThreeDWitchLight> &out)
{
	if (!IsNightNow()) return;
	ForPoses([eye, &out](const Pose &p) {
		const ThreeDUfo &u = *p.u;
		const float s = p.size * (eye ? 1.0f : MAP_SCALE);
		const double mid = MiddleZ(p, s), bottom = mid - BottomBelow(u) * s * OBJECT_HEIGHT_PER_METRE;
		if (u.rim_colour != 0 && u.rim_reach_m > 0) {
			ThreeDWitchLight &l = out.emplace_back();
			l.x = p.x;
			l.y = p.y;
			l.z = mid;
			l.reach = u.rim_reach_m * s * OBJECT_UNITS_PER_METRE;
			l.colour = Colour(u.rim_colour);
		}
		const double down = (bottom - FirstPersonGroundZ(p.x, p.y)) / OBJECT_HEIGHT_PER_METRE * OBJECT_UNITS_PER_METRE;
		if (!p.beam || u.beam_colour == 0 || down <= 0) return;
		ThreeDWitchLight &l = out.emplace_back();
		l.torch = true;
		l.x = p.x;
		l.y = p.y;
		l.z = bottom;
		l.dz = -1;
		l.reach = std::max<double>(u.beam_reach_m * s * OBJECT_UNITS_PER_METRE, down * 1.1);
		l.half_angle = u.beam_angle * M_PI / 360.0;
		l.ground = down;
		l.colour = Colour(u.beam_colour);
	});
}

/**
 * Send the saucer of the night flying from a place (the rig's 'test3d ufo'; at night it comes by itself): towards the
 * nearest town ahead of it, then on from town to town.
 * @param heading which way it flies at first, radians
 * @return false: no saucer of the night in the indices, or not on the map
 */
bool SendThreeDUfo(double x, double y, double heading)
{
	const std::vector<const ThreeDUfo *> kinds = NightKinds();
	if (kinds.empty() || !OnMap(x, y)) return false;
	const ThreeDUfo &u = *kinds[InteractiveRandomRange(static_cast<uint32_t>(kinds.size()))];
	Saucer s;
	s.kind = u.name;
	s.x = x;
	s.y = y;
	s.heading = heading;
	s.phase = InteractiveRandom();
	/* Where it flies to: the town nearest the way it faces, else any. */
	double best = 1e18;
	for (const Town *t : Town::Iterate()) {
		double mx, my;
		TownMiddle(t, mx, my);
		const double dx = mx - x, dy = my - y, along = dx * std::cos(heading) + dy * std::sin(heading);
		const double d = std::hypot(dx, dy) + (along < 0 ? 1e9 : 0);
		if (d < best) {
			best = d;
			s.tx = mx;
			s.ty = my;
			s.town = t->index;
		}
	}
	if (s.town == TownID::Invalid()) {
		s.tx = x + std::cos(heading) * APPROACH_TILES * TILE_SIZE;
		s.ty = y + std::sin(heading) * APPROACH_TILES * TILE_SIZE;
	}
	s.lift = CruiseLift(s, u);
	s.z = static_cast<int>(std::lround(FirstPersonGroundZ(x, y) + s.lift * OBJECT_HEIGHT_PER_METRE));
	Debug(misc, 1, "3D saucer: {} comes at {:.0f},{:.0f}", s.kind, x / TILE_SIZE, y / TILE_SIZE);
	_saucer = std::move(s);
	return true;
}

/**
 * The rig ('test3d ufo <x> <y> <stupnu> visi|ovce|pristan'): the saucer of the night made to hover where it is, its
 * beam down ("visi"), a sheep up it too ("ovce"), or to land there ("pristan").
 * @return false: none flying
 */
bool MakeThreeDUfoDo(std::string_view what)
{
	if (!_saucer.has_value()) return false;
	Saucer &s = *_saucer;
	s.since = TimerGameTick::counter;
	s.sheep = what == "ovce";
	if (what == "pristan") {
		s.doing = Doing::Sinking;
		return true;
	}
	s.doing = Doing::Hovering;
	return what == "visi" || what == "ovce";
}

/** The rig ('test3d ufo'): the saucers, a line. */
std::string DescribeThreeDUfos()
{
	static const char *const DOING[] = {"leti", "visi", "pristava", "pristal", "vzleta"};
	std::string s = fmt::format("{} saucers of the indices{}", GetThreeDUfos().size(), DisasterKind() != nullptr ? fmt::format(", disasters as {}", DisasterKind()->name) : std::string());
	if (_saucer.has_value()) {
		const Saucer &n = *_saucer;
		s += fmt::format("; night {} at {:.1f},{:.1f} {:.0f} m up {} facing {:.0f} frame {} to {:.0f},{:.0f}{}", n.kind, n.x / TILE_SIZE, n.y / TILE_SIZE, n.lift, DOING[to_underlying(n.doing)], Facing(n.heading), n.frame, n.tx / TILE_SIZE, n.ty / TILE_SIZE, n.sheep ? " with a sheep" : "");
	} else {
		s += IsNightNow() ? "; none at night yet" : "; day: none at night";
	}
	/* Its figures as read: their extent, their textures' see-through. */
	for (const ThreeDUfo &u : GetThreeDUfos()) {
		for (const char *part : {"let", "paprsek", "ovce"}) {
			const ThreeDVehicleObject *f = GetThreeDFigure(u.Figure(part));
			const ThreeDVehicleShape *shape = f != nullptr ? f->GetShape(0) : nullptr;
			if (shape == nullptr) {
				s += fmt::format("; {}: none", u.Figure(part));
				continue;
			}
			std::string alphas;
			for (uint8_t a : shape->tex_alpha) alphas += fmt::format(" {}", a);
			std::map<int, int> by_tex;
			for (uint8_t t : shape->tri_tex) by_tex[t]++;
			alphas += ", triangles by texture";
			for (const auto &[t, n] : by_tex) alphas += fmt::format(" {}:{}", t, n);
			alphas += fmt::format(", glass {}", shape->tri_glass.size());
			s += fmt::format("; {}: {} triangles, z {:.2f}..{:.2f}, alpha{}", u.Figure(part), shape->tri.size() / 3, shape->z0, shape->z1, alphas);
		}
	}
	ForPoses([&s](const Pose &p) {
		if (!p.base_is_bottom) return;
		s += fmt::format("; disaster UFO at {:.1f},{:.1f} z {:.0f} size {:.1f}{}", p.x / TILE_SIZE, p.y / TILE_SIZE, p.base, p.size, p.beam ? " beam" : "");
	});
	return s;
}
