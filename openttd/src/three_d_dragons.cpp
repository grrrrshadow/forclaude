/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_dragons.cpp The dragons flying in and fighting the walker of the view from the first person (three_d_dragons.h). */

#include "stdafx.h"
#include "three_d_dragons.h"
#include "three_d_objects.h"
#include "three_d_navigation.h"
#include "three_d_vehicle_object.h"
#include "cab_view.h"
#include "openttd.h"
#include "map_func.h"
#include "landscape.h"
#include "viewport_func.h"
#include "three_d.h"
#include "core/random_func.hpp"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "debug.h"

#include <cmath>

#include "safeguards.h"

/** The walker of the view from the first person: where it stands, world units; false when the view is not open (first_person_gui.cpp). */
bool FirstPersonWalkerAt(double &x, double &y);

namespace {

constexpr uint MAX_DRAGONS = 3;            ///< at most so many on the map at once
constexpr uint ARRIVE_ONE_IN = 4000;        ///< a dragon comes one tick in so many (about two minutes)
constexpr uint64_t LIFE_TICKS = 13000;      ///< a walking dragon flies off again after so long (about six minutes)
constexpr int HEALTH = 6;                   ///< blows of the hammer a walking dragon takes
constexpr int FLYER_HEALTH = 3;             ///< and a flying one, before it flies off
constexpr uint64_t DEAD_TICKS = 1500;       ///< a fallen dragon lies so long
constexpr double CHASE_M = 45.0;            ///< a dragon this near the walker goes for it, metres
constexpr double GAP_M = 1.5;               ///< and strikes from its body this far, metres
constexpr uint8_t BLOW = 12;                ///< the health a walking dragon's blow takes
constexpr uint8_t SWOOP = 8;                ///< and a flying one's swoop
constexpr double FLY_M = 25.0;              ///< how high the flying dragons fly, metres
constexpr double LAND_M = 35.0;             ///< how high the walking dragons come down from, metres

enum class State : uint8_t {
	Arrive, ///< coming down where it lands
	Idle,   ///< standing
	Wander, ///< walking somewhere
	Chase,  ///< going for the walker
	Attack, ///< striking
	Dying,  ///< falling
	Dead,   ///< lying
	Leave,  ///< flying off upwards
	Fly,    ///< a flying dragon crossing the map
};

struct Dragon {
	std::string kind;      ///< the creature of the indices
	double length_m = 6;   ///< its size, metres
	double height_m = 5;
	State state = State::Idle;
	std::string anim;      ///< the animation it is in
	uint64_t anim_start = 0;
	std::string from_anim; ///< the animation it was in before, its pose passing over into this one's (with bones)
	int from_frame = 0;    ///< the frame of it it left
	double pace = 1;       ///< how fast the animation goes (chasing: faster)
	uint64_t until = 0;    ///< when its state ends
	uint64_t born = 0;
	double x = 0, y = 0;   ///< world units
	double lift = 0;       ///< over the ground, metres
	double heading = 0;    ///< radians, the direction it faces
	double tx = 0, ty = 0; ///< where it goes
	int health = HEALTH;
	bool struck = false;   ///< the blow of the attack dealt
	bool swooped = false;  ///< a flyer's swoop dealt
	int frame = 0;
	int z = 0;             ///< height units, drawn at
	std::vector<NavPoint> way; ///< the way it walks round what stands between (NavFindWay()), world units
	size_t way_at = 0;     ///< the point of it it walks to
	double way_tx = 0, way_ty = 0; ///< where the way goes
	uint64_t way_made = 0; ///< when it was found
};

std::vector<Dragon> _dragons;
uint _damage = 0; ///< the health the dragons took from the walker since the view last asked

const ThreeDCreature *Creature(std::string_view name)
{
	for (const ThreeDCreature &c : GetThreeDCreatures()) {
		if (c.name == name) return &c;
	}
	return nullptr;
}

const ThreeDVehicleObject *Anim(const Dragon &d, std::string_view anim)
{
	return GetThreeDFigure(d.kind + "/" + std::string(anim));
}

/** Go into an animation (again from its start with restart), passing over from the one it is in. */
void SetAnim(Dragon &d, std::string_view anim, uint64_t now, double pace = 1.0, bool restart = false)
{
	d.pace = pace;
	if (d.anim == anim && !restart) return;
	d.from_anim = d.anim;
	d.from_frame = d.frame;
	d.anim = anim;
	d.anim_start = now;
}

/** The animation it rests in: "klid", else what it has. */
std::string_view RestAnim(const ThreeDCreature &c)
{
	if (c.Has("klid")) return "klid";
	if (c.Has("chuze")) return "chuze";
	return c.animations.front();
}

int FrameOf(const Dragon &d, uint64_t now)
{
	const ThreeDVehicleObject *f = Anim(d, d.anim);
	if (f == nullptr || f->frames < 2) return 0;
	const uint64_t n = static_cast<uint64_t>(static_cast<double>(now - d.anim_start) * d.pace) / f->frame_ticks;
	if (!f->loop) return static_cast<int>(std::min<uint64_t>(n, f->frames - 1));
	return static_cast<int>(n % f->frames);
}

bool AnimDone(const Dragon &d, uint64_t now)
{
	const ThreeDVehicleObject *f = Anim(d, d.anim);
	if (f == nullptr) return true;
	return static_cast<double>(now - d.anim_start) * d.pace >= static_cast<double>(std::max(1, f->frames) * f->frame_ticks);
}

/** Metres a tick it covers in an animation: the animation's step over the round of its frames. */
double Speed(const Dragon &d, std::string_view anim, double fallback)
{
	const ThreeDVehicleObject *f = Anim(d, anim);
	if (f == nullptr || f->walk_step <= 0 || f->frames < 1) return fallback * d.pace;
	return f->walk_step / (f->frames * f->frame_ticks) * d.pace;
}

bool OnMap(double x, double y)
{
	return x >= TILE_SIZE && y >= TILE_SIZE && x < (Map::SizeX() - 1) * TILE_SIZE && y < (Map::SizeY() - 1) * TILE_SIZE;
}

/** Its part of the screen dirty, as big as it is and as high as it flies. */
void MarkDirty(const Dragon &d)
{
	const Point pt = RemapCoords(static_cast<int>(d.x), static_cast<int>(d.y), d.z);
	const int r = static_cast<int>(d.length_m * OBJECT_UNITS_PER_METRE * 4 * ZOOM_BASE) + 32 * ZOOM_BASE;
	const int up = static_cast<int>(d.height_m * OBJECT_HEIGHT_PER_METRE * ZOOM_BASE) + r;
	/* Turned (three_d.h): the box it flies in, down to the ground for its shadow. */
	DirtyPlace place;
	place.kind = DirtyPlace::Kind::Box;
	const double reach = d.length_m * OBJECT_UNITS_PER_METRE + TILE_SIZE;
	place.x0 = d.x - reach;
	place.x1 = d.x + reach;
	place.y0 = d.y - reach;
	place.y1 = d.y + reach;
	place.z0 = 0;
	place.z1 = d.z + d.height_m * OBJECT_HEIGHT_PER_METRE + reach;
	DirtyPlaceScope scope(place);
	MarkAllViewportsDirty(pt.x - r, pt.y - up, pt.x + r, pt.y + r);
}

/** Step towards a point at a speed, turning to it; whether it got there. */
bool StepTo(Dragon &d, double tx, double ty, double speed_m)
{
	const double dx = tx - d.x, dy = ty - d.y, dist = std::hypot(dx, dy);
	if (dist > 1e-6) d.heading = std::atan2(dy, dx);
	const double step = speed_m * OBJECT_UNITS_PER_METRE;
	if (dist <= step) {
		d.x = tx;
		d.y = ty;
		return true;
	}
	const double nx = d.x + dx / dist * step, ny = d.y + dy / dist * step;
	if (!OnMap(nx, ny)) return true;
	d.x = nx;
	d.y = ny;
	return false;
}

/**
 * Walk towards a point by the way round the houses, the water and the steep slopes (NavFindWay(), the technologies
 * of OpenMW, the player, 16. 10.), found again when the point has moved a tile or now and then.
 * @return whether it got there, or as near as it can
 */
bool WalkTo(Dragon &d, double tx, double ty, double speed_m, uint64_t now)
{
	if (d.way.empty() || std::hypot(tx - d.way_tx, ty - d.way_ty) > TILE_SIZE || now - d.way_made > 74) {
		d.way = NavFindWay(d.x, d.y, tx, ty);
		d.way_at = 0;
		d.way_tx = tx;
		d.way_ty = ty;
		d.way_made = now;
		if (d.way.empty()) return true;
	}
	if (d.way_at >= d.way.size()) return true;
	const auto [wx, wy] = d.way[d.way_at];
	if (!StepTo(d, wx, wy, speed_m)) return false;
	d.way_at++;
	return d.way_at >= d.way.size();
}

/** A place to walk to, within a few tiles, where it can stand. */
void PickWander(Dragon &d)
{
	for (int tries = 0; tries < 8; tries++) {
		const double a = InteractiveRandomRange(360) * M_PI / 180.0, r = (2 + InteractiveRandomRange(5)) * TILE_SIZE;
		double x = d.x + std::cos(a) * r, y = d.y + std::sin(a) * r;
		if (!OnMap(x, y) || !NavNearestWalkable(x, y, 2) || !OnMap(x, y)) continue;
		d.tx = x;
		d.ty = y;
		d.way.clear();
		return;
	}
	d.tx = d.x;
	d.ty = d.y;
}

void Update(Dragon &d, uint64_t now, bool walker, double wx, double wy)
{
	const ThreeDCreature *c = Creature(d.kind);
	if (c == nullptr) {
		d.state = State::Dead;
		d.until = now;
		return;
	}
	const double dist_m = walker ? std::hypot(wx - d.x, wy - d.y) / OBJECT_UNITS_PER_METRE : 1e9;
	const bool fights = c->Has("utok");
	const double reach_m = d.length_m / 2 + GAP_M;
	switch (d.state) {
		case State::Arrive:
			SetAnim(d, RestAnim(*c), now);
			d.lift = std::max(0.0, d.lift - 0.25);
			if (d.lift <= 0) {
				d.state = State::Idle;
				d.until = now + 60;
			}
			break;

		case State::Idle:
			SetAnim(d, RestAnim(*c), now);
			if (fights && dist_m < CHASE_M) {
				d.state = State::Chase;
			} else if (now - d.born > LIFE_TICKS) {
				d.state = State::Leave;
			} else if (now >= d.until && c->Has("chuze")) {
				PickWander(d);
				d.state = State::Wander;
			}
			break;

		case State::Wander:
			SetAnim(d, "chuze", now, 1.5);
			if (fights && dist_m < CHASE_M) {
				d.state = State::Chase;
			} else if (WalkTo(d, d.tx, d.ty, Speed(d, "chuze", 0.02), now)) {
				d.state = State::Idle;
				d.until = now + 100 + InteractiveRandomRange(200);
			}
			break;

		case State::Chase:
			SetAnim(d, c->Has("chuze") ? "chuze" : RestAnim(*c), now, 3.0);
			if (!walker || dist_m > CHASE_M * 1.5) {
				d.state = State::Idle;
				d.until = now + 60;
			} else if (dist_m <= reach_m) {
				d.heading = std::atan2(wy - d.y, wx - d.x);
				d.state = State::Attack;
				d.struck = false;
				SetAnim(d, "utok", now, 1.0, true);
			} else {
				WalkTo(d, wx, wy, Speed(d, "chuze", 0.02), now);
			}
			break;

		case State::Attack: {
			SetAnim(d, "utok", now);
			if (walker) d.heading = std::atan2(wy - d.y, wx - d.x);
			const ThreeDVehicleObject *f = Anim(d, "utok");
			const int blow_frame = f != nullptr ? f->frames * 5 / 8 : 0;
			if (!d.struck && FrameOf(d, now) >= blow_frame) {
				d.struck = true;
				if (dist_m <= reach_m + 1.0) _damage += BLOW;
			}
			if (AnimDone(d, now)) {
				d.state = State::Idle;
				d.until = now + 40;
			}
			break;
		}

		case State::Dying:
			SetAnim(d, "smrt", now);
			if (AnimDone(d, now)) {
				d.state = State::Dead;
				d.until = now + DEAD_TICKS;
			}
			break;

		case State::Dead:
			break;

		case State::Leave:
			SetAnim(d, c->Has("let") ? "let" : RestAnim(*c), now);
			d.lift += 0.3;
			if (d.lift > LAND_M + 10) {
				d.state = State::Dead;
				d.until = now;
			}
			break;

		case State::Fly: {
			SetAnim(d, "let", now);
			/* Over the walker: down on it once, then up again. */
			double want = FLY_M;
			if (walker && !d.swooped && dist_m < 60.0) {
				d.heading = std::atan2(wy - d.y, wx - d.x);
				want = 3.0;
				if (dist_m < 5.0 && d.lift < 7.0) {
					_damage += SWOOP;
					d.swooped = true;
				}
			}
			d.lift += std::clamp(want - d.lift, -0.4, 0.3);
			const double step = Speed(d, "let", 0.25) * OBJECT_UNITS_PER_METRE;
			const double nx = d.x + std::cos(d.heading) * step, ny = d.y + std::sin(d.heading) * step;
			if (!OnMap(nx, ny)) {
				d.state = State::Dead;
				d.until = now;
				break;
			}
			d.x = nx;
			d.y = ny;
			break;
		}
	}
}

/** Every tick: a dragon comes now and then; those here live on, the fallen ones go after a while. */
const IntervalTimer<TimerGameTick> _dragons_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	if (_game_mode != GameMode::Normal) {
		_dragons.clear();
		return;
	}
	const std::vector<ThreeDCreature> &kinds = GetThreeDCreatures();
	if (kinds.empty()) return;
	const uint64_t now = TimerGameTick::counter;
	double wx = 0, wy = 0;
	const bool walker = FirstPersonWalkerAt(wx, wy);

	if (_dragons.size() < MAX_DRAGONS && InteractiveRandomRange(ARRIVE_ONE_IN) == 0) {
		/* Anywhere; with the walker out, mostly near it. */
		const ThreeDCreature &c = kinds[InteractiveRandomRange(static_cast<uint32_t>(kinds.size()))];
		double x, y;
		if (walker && InteractiveRandomRange(3) != 0) {
			const double a = InteractiveRandomRange(360) * M_PI / 180.0, r = (8 + InteractiveRandomRange(8)) * TILE_SIZE;
			x = wx + std::cos(a) * r;
			y = wy + std::sin(a) * r;
		} else {
			x = (1 + InteractiveRandomRange(Map::SizeX() - 2)) * TILE_SIZE;
			y = (1 + InteractiveRandomRange(Map::SizeY() - 2)) * TILE_SIZE;
		}
		if (OnMap(x, y)) SendThreeDDragon(c.name, x, y);
	}

	for (auto it = _dragons.begin(); it != _dragons.end();) {
		Dragon &d = *it;
		MarkDirty(d);
		Update(d, now, walker, wx, wy);
		if (d.state == State::Dead && now >= d.until) {
			it = _dragons.erase(it);
			continue;
		}
		d.frame = FrameOf(d, now);
		d.z = static_cast<int>(std::lround(FirstPersonGroundZ(d.x, d.y) + d.lift * OBJECT_HEIGHT_PER_METRE));
		MarkDirty(d);
		++it;
	}
});

/**
 * The figure of its pose and the frame of it: its animation's, or a passage into it from the one before
 * (GetThreeDFigure()), mixing the two poses bone by bone over its first ticks -- the technologies of OpenMW, the
 * player, 16. 10.: the animations blend into each other.
 */
std::string Figure(const Dragon &d, int &frame)
{
	frame = d.frame;
	const std::string own = d.kind + "/" + d.anim;
	if (d.from_anim.empty() || d.from_anim == d.anim) return own;
	const uint64_t since = TimerGameTick::counter - d.anim_start;
	const std::string passage = fmt::format("{}/{}@{}>{}", d.kind, d.from_anim, d.from_frame, d.anim);
	const ThreeDVehicleObject *p = GetThreeDFigure(passage);
	if (p == nullptr || p->blend_from == nullptr || since >= static_cast<uint64_t>(p->blend_ticks)) return own;
	frame = d.frame * p->blend_ticks + static_cast<int>(since);
	return passage;
}

/** Which way it faces as the figures count it, in steps (a few pictures for every turn). */
float Facing(const Dragon &d)
{
	return static_cast<float>(std::round(d.heading * 180.0 / M_PI / 10.0) * 10.0);
}

} // namespace

/** The dragons on the map, as figures (the turned view, and facing north with the houses as objects). */
void AddThreeDDragonsToViewport(ZoomLevel zoom)
{
	for (const Dragon &d : _dragons) {
		int frame;
		const std::string figure = Figure(d, frame);
		AddThreeDFigureToViewport(figure, 1.0f, static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)), d.z, Facing(d), zoom, INVALID_TILE, 0, frame);
	}
}

/** The dragons in the view from an eye: billboards of their pictures (AddCabFigure()). */
void AddThreeDDragonsToEye()
{
	for (const Dragon &d : _dragons) {
		int frame;
		const std::string figure = Figure(d, frame);
		AddCabFigure(figure, 1.0f, static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)), d.z, Facing(d), frame);
	}
}

namespace {

/** Whether a dragon can be hit: not falling, dead or leaving. */
bool Hittable(const Dragon &d)
{
	return d.state != State::Dying && d.state != State::Dead && d.state != State::Leave;
}

/** A dragon hit: so much less health; a walking dragon falls after its last, a flying one flies off. */
void Strike(Dragon &d, int damage)
{
	d.health -= damage;
	if (d.health > 0) return;
	const ThreeDCreature *c = Creature(d.kind);
	if (d.state == State::Fly) {
		d.swooped = true;
	} else if (c != nullptr && c->Has("smrt")) {
		d.state = State::Dying;
		SetAnim(d, "smrt", TimerGameTick::counter, 1.0, true);
	} else {
		d.state = State::Leave;
	}
}

} // namespace

/**
 * A blow of the walker's hammer or sword: the dragon it reaches in front of it takes it (Strike()).
 * @param x,y where the walker stands, world units
 * @param heading the direction it faces, radians
 * @param reach_m how far the weapon reaches, metres
 * @param damage how much health the blow takes
 * @return whether a dragon was hit
 */
bool HitThreeDDragon(double x, double y, double heading, double reach_m, int damage)
{
	for (Dragon &d : _dragons) {
		if (!Hittable(d) || d.lift > 4.0) continue;
		const double dx = d.x - x, dy = d.y - y;
		const double gap_m = std::hypot(dx, dy) / OBJECT_UNITS_PER_METRE - d.length_m * 0.4;
		if (gap_m > reach_m) continue;
		const double off = std::abs(std::remainder(std::atan2(dy, dx) - heading, 2 * M_PI));
		if (off > 70.0 * M_PI / 180.0) continue;
		Strike(d, damage);
		return true;
	}
	return false;
}

/**
 * An arrow or a bolt of the walker (the player, 17. 10.: the bow and the crossbow) at a point: the dragon whose body
 * it is in takes it (Strike()) -- a flying one too, however high.
 * @param x,y,z the point, world units and units of height
 * @param damage how much health it takes
 * @return whether a dragon was hit
 */
bool HitThreeDDragonAt(double x, double y, double z, int damage)
{
	for (Dragon &d : _dragons) {
		if (!Hittable(d)) continue;
		/* Its body: about as wide as half its length, as high as it is tall, from its feet. */
		const double r = std::max(1.0, d.length_m * 0.3) * OBJECT_UNITS_PER_METRE;
		if (std::hypot(d.x - x, d.y - y) > r) continue;
		if (z < d.z - 0.3 * OBJECT_HEIGHT_PER_METRE || z > d.z + d.height_m * OBJECT_HEIGHT_PER_METRE) continue;
		Strike(d, damage);
		return true;
	}
	return false;
}

/** The health the dragons took from the walker since the last asking; asked by the view from the first person, which takes it through a command. */
uint TakeThreeDDragonDamage()
{
	uint taken = _damage;
	_damage = 0;
	return taken;
}

/**
 * A dragon comes now (or the rig sends one): a flying one crosses the map through the place, a walking one comes
 * down there.
 * @param kind the creature of the indices, or empty for any
 * @param x,y where, world units
 * @return whether one came
 */
bool SendThreeDDragon(std::string_view kind, double x, double y)
{
	const std::vector<ThreeDCreature> &kinds = GetThreeDCreatures();
	if (kinds.empty() || !OnMap(x, y)) return false;
	const ThreeDCreature *c = kind.empty() ? &kinds[InteractiveRandomRange(static_cast<uint32_t>(kinds.size()))] : Creature(kind);
	if (c == nullptr) return false;
	const uint64_t now = TimerGameTick::counter;
	Dragon d;
	d.kind = c->name;
	d.length_m = std::max(1.0f, c->size[0]);
	d.height_m = std::max(1.0f, c->size[2]);
	d.born = now;
	d.heading = InteractiveRandomRange(360) * M_PI / 180.0;
	if (c->Has("let") && !c->Has("chuze")) {
		/* Across the map: from some way back along its heading, through the place. */
		const double back = 20.0 * TILE_SIZE;
		d.x = std::clamp(x - std::cos(d.heading) * back, 1.5 * TILE_SIZE, (Map::SizeX() - 1.5) * TILE_SIZE);
		d.y = std::clamp(y - std::sin(d.heading) * back, 1.5 * TILE_SIZE, (Map::SizeY() - 1.5) * TILE_SIZE);
		d.heading = std::atan2(y - d.y, x - d.x);
		d.lift = FLY_M;
		d.state = State::Fly;
		d.health = FLYER_HEALTH;
		SetAnim(d, "let", now);
	} else {
		/* Down where it can stand: not on a house or in the water. */
		d.x = x;
		d.y = y;
		NavNearestWalkable(d.x, d.y, 6);
		d.lift = LAND_M;
		d.state = State::Arrive;
		d.health = HEALTH;
		SetAnim(d, RestAnim(*c), now);
	}
	d.z = static_cast<int>(std::lround(FirstPersonGroundZ(d.x, d.y) + d.lift * OBJECT_HEIGHT_PER_METRE));
	Debug(misc, 1, "3D dragon: {} comes at {:.0f},{:.0f}", d.kind, d.x / TILE_SIZE, d.y / TILE_SIZE);
	_dragons.push_back(std::move(d));
	return true;
}

/** The rig: the dragons, a line each. */
std::string DescribeThreeDDragons()
{
	static const char *const STATES[] = {"prilet", "stoji", "chodi", "honi", "utoci", "pada", "lezi", "odlet", "leti"};
	std::string out = fmt::format("{} draku", _dragons.size());
	for (const Dragon &d : _dragons) {
		out += fmt::format("\n  {} {} na {:.1f},{:.1f} vyska {:.1f} m, smer {:.0f}, {} snimek {}, zdravi {}", d.kind, STATES[static_cast<int>(d.state)],
				d.x / TILE_SIZE, d.y / TILE_SIZE, d.lift, d.heading * 180.0 / M_PI, d.anim, d.frame, d.health);
		/* The figure drawn: a passage from the animation before while it lasts (Figure()). */
		int frame;
		const std::string figure = Figure(d, frame);
		const ThreeDVehicleObject *f = GetThreeDFigure(figure);
		if (f != nullptr && f->blend_from != nullptr) out += fmt::format(", prechod {} snimek {}", figure, frame);
		if (f != nullptr && !f->skeleton_file.empty() && f->GetShape(frame) != nullptr) out += fmt::format(", kostra ({} snimku)", f->frames);
	}
	return out;
}
