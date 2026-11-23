/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * Changed for ottd Decouple by Karel Mácha: the changes Copyright (C) 2026 Karel Mácha.
 */

/**
 * @file aircraft_modular.cpp Aircraft at the airports built tile by tile (air_map.h).
 *
 * The player (17. 11.): "the buildings separately, an asphalt taxiway and a runway, plus heliports separately, so
 * that I build the airport myself; which way the runway goes I do not care". Such an airport has no movement table
 * (airport_movement.h) for its aircraft to follow; they find their own way over it, after the idea of J0anJosep's
 * MultitileAirports branch (hra/3d/PLAN-MODULARNI-LETISTE.md in grrrrf, steps 3 and 4):
 *  - A plane coming to one circles it as at any airport (the old code's holding pattern) till it can reserve a runway
 *    long enough for it, whole, and its way off it to a free stand (or to a hangar): then it flies to the start of its
 *    final approach, comes down along the runway, brakes to its far end and taxies.
 *  - On the ground an aircraft goes over the taxiing tracks as a train goes over its rails: its way is searched over
 *    the airport's tiles (a stand, a hangar and a runway's end it may turn round on), reserved whole before it moves
 *    (air_map.h, the reserved tracks of a tile) and let go of behind it. A tile reserved by one aircraft is not
 *    entered by another, a runway reserved is not crossed.
 *  - At a stand it loads as at an old airport's terminal; then it taxies to the end of a runway, waits for the runway
 *    to be free, reserves it, speeds up along it and climbs out; out of the airport the old code flies it on.
 *  - A hangar is its depot; an aircraft is bought in it and leaves it through its doors.
 *  - A helicopter flies to a free pad (a helipad, a heliport or a stand; a hangar when it goes to the depot), comes
 *    down onto it and rises from it.
 * Old airports and their aircraft are not touched: all this runs only for an airport that is modular
 * (IsModularAirportStation()).
 */

#include "stdafx.h"
#include "aircraft.h"
#include "air_map.h"
#include "airport.h"
#include "command_func.h"
#include "effectvehicle_func.h"
#include "company_func.h"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "engine_base.h"
#include "landscape.h"
#include "newgrf_sound.h"
#include "order_base.h"
#include "sound_func.h"
#include "station_base.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "track_func.h"
#include "vehicle_cmd.h"
#include "vehicle_func.h"
#include "window_func.h"

#include "table/strings.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

#include "safeguards.h"

/** How many tiles before the start of the runway a plane's final approach starts. */
static constexpr int APPROACH_TILES = 8;
/** How far from the airport a plane coming to it starts to look for a runway, tiles. */
static constexpr uint LANDING_REACH = 28;
/** How many tiles out over the end of the runway a plane climbs before the old code flies it on. */
static constexpr int CLIMB_OUT_TILES = 4;
/** The shortest runway for a plane, tiles; a fast one needs more. */
static constexpr uint RUNWAY_PLANE = 3;
static constexpr uint RUNWAY_FAST_PLANE = 5;
/** A plane on its rocket stands on its pad so many steps before it rises (the rocket stood up, the engines lit): an aircraft takes two steps a tick, so some four seconds. */
static constexpr uint8_t LAUNCH_WAIT_TICKS = 250;
/** How high a rocket rises before it flies on, height units (TILE_HEIGHT a level): twice the highest land there can be. */
static constexpr int ROCKET_APEX_Z = 2 * 255 * TILE_HEIGHT;

/** Is this station's airport one built tile by tile (and not an old one)? */
bool IsModularAirportStation(const Station *st)
{
	return st != nullptr && st->facilities.Test(StationFacility::ModularAirport) && !st->facilities.Test(StationFacility::Airport) &&
			st->modular_airport.tile != INVALID_TILE;
}

/** Is the tile a modular airport's tile of the station? */
static bool IsStationModularTile(TileIndex t, StationID sid)
{
	return IsValidTile(t) && IsModularAirportTile(t) && GetStationIndex(t) == sid;
}

/** A runway of a modular airport, from its end at the lower coordinate of its axis to the one at the higher. */
struct ModularRunway {
	std::vector<TileIndex> tiles; ///< Its tiles, low end first.
	Axis axis = Axis::X; ///< Its axis.
};

/**
 * The runway a runway tile is part of.
 * @param t a tile of the runway
 * @return its tiles; empty when it is no whole runway
 */
static ModularRunway RunwayOf(TileIndex t)
{
	ModularRunway r;
	if (!IsValidTile(t) || !IsModularAirportTile(t) || !IsModularAirportRunway(t)) return r;
	const StationID sid = GetStationIndex(t);
	r.axis = GetModularAirportRunwayAxis(t);
	const TileIndexDiff step = TileOffsByAxis(r.axis);
	auto of_runway = [&](TileIndex u) {
		return IsStationModularTile(u, sid) && IsModularAirportRunway(u) && GetModularAirportRunwayAxis(u) == r.axis;
	};
	auto low_end = [](TileIndex u) { return GetModularAirportKind(u) == ModularAirportKind::RunwayEnd && !IsModularAirportRunwayHighEnd(u); };
	auto high_end = [](TileIndex u) { return GetModularAirportKind(u) == ModularAirportKind::RunwayEnd && IsModularAirportRunwayHighEnd(u); };
	TileIndex first = t;
	for (uint i = 0; i < Map::SizeX() + Map::SizeY() && !low_end(first) && of_runway(TileIndex(first.base() - step)); i++) first = TileIndex(first.base() - step);
	if (!low_end(first)) return r;
	r.tiles.push_back(first);
	for (TileIndex u = first; !high_end(u);) {
		if (!of_runway(TileIndex(u.base() + step)) || r.tiles.size() > Map::SizeX() + Map::SizeY()) {
			r.tiles.clear();
			return r;
		}
		u = TileIndex(u.base() + step);
		r.tiles.push_back(u);
	}
	return r;
}

/** The runways of a modular airport. */
static std::vector<ModularRunway> RunwaysOf(const Station *st)
{
	std::vector<ModularRunway> runways;
	for (TileIndex t : st->modular_airport) {
		if (!IsStationModularTile(t, st->index) || GetModularAirportKind(t) != ModularAirportKind::RunwayEnd || IsModularAirportRunwayHighEnd(t)) continue;
		ModularRunway r = RunwayOf(t);
		if (!r.tiles.empty()) runways.push_back(std::move(r));
	}
	return runways;
}

/**
 * Does an aircraft of the engine come down vertically onto a pad (a helicopter), or land on a runway (a plane)? Kept
 * apart from how it takes off (TakesOffVertically()): the player's Buran is to rise vertically from a heliport and to
 * land on a runway, a flag of its own then mixes the two.
 */
static bool LandsVertically(EngineID engine)
{
	return (Engine::Get(engine)->VehInfo<AircraftVehicleInfo>().subtype & AIR_CTOL) == 0;
}

bool ThreeDEngineStartsVertically(EngineID engine);

/**
 * Is the engine a plane a rocket carries up from a pad (the player's Buran on the Energia; its set's index says so,
 * "start": "kolmo", ThreeDEngineStartsVertically())? It lands on a runway as a plane, unloads and loads at a stand, goes
 * into the hangar and comes out of it on its rocket to a pad, where it stands up and rises (the player, 19. 11.: "with
 * the rocket, take it through the depot: it unloads at the terminal for planes, goes to the depot and comes out with
 * the rocket to the heliport").
 */
static bool StartsByRocket(EngineID engine)
{
	return (Engine::Get(engine)->VehInfo<AircraftVehicleInfo>().subtype & AIR_CTOL) != 0 && ThreeDEngineStartsVertically(engine);
}

/** Does an aircraft of the engine rise vertically from its pad (a helicopter, a plane on a rocket), or take off along a runway (a plane)? */
static bool TakesOffVertically(EngineID engine)
{
	return (Engine::Get(engine)->VehInfo<AircraftVehicleInfo>().subtype & AIR_CTOL) == 0 || StartsByRocket(engine);
}

/** The shortest runway the engine lands on and takes off from, tiles. */
static uint RunwayNeeded(EngineID engine)
{
	return (Engine::Get(engine)->VehInfo<AircraftVehicleInfo>().subtype & AIR_FAST) != 0 ? RUNWAY_FAST_PLANE : RUNWAY_PLANE;
}

/**
 * Is the tile a pad a helicopter lands on: a helipad or a stand. Not the spaceport (the heliport till 22. 11.; the
 * player: "make the heliport a spaceport, and the helicopters will not start from the spaceport"): only rockets rise
 * from it (IsLaunchPad()).
 */
static bool IsHeliPad(TileIndex t)
{
	const ModularAirportKind kind = GetModularAirportKind(t);
	return kind == ModularAirportKind::Helipad || kind == ModularAirportKind::Stand;
}

/** A helicopter, not a plane a rocket carries up: it rises from a pad and comes down onto one. */
static bool IsHelicopterOnPads(EngineID engine)
{
	return TakesOffVertically(engine) && !StartsByRocket(engine);
}

/** Is the tile a pad a rocket rises from: a helicopters' pad on the ground or on its deck, not a stand? */
static bool IsLaunchPad(TileIndex t)
{
	const ModularAirportKind kind = GetModularAirportKind(t);
	return kind == ModularAirportKind::Helipad || kind == ModularAirportKind::Heliport;
}

/**
 * Can an aircraft of the engine use the modular airport: a runway long enough for a plane, a pad or a stand for a
 * helicopter; for a plane on a rocket a runway and a stand to come to, a hangar and a pad to leave from.
 */
bool CanAircraftUseModularAirport(EngineID engine, const Station *st)
{
	if (!IsModularAirportStation(st)) return false;
	bool stand = false, pad = false, launch = false;
	for (TileIndex t : st->modular_airport) {
		if (!IsStationModularTile(t, st->index)) continue;
		if (GetModularAirportKind(t) == ModularAirportKind::Stand) stand = true;
		if (IsHeliPad(t)) pad = true;
		if (IsLaunchPad(t)) launch = true;
	}
	bool runway = false;
	if (!LandsVertically(engine) || !TakesOffVertically(engine)) {
		const uint need = RunwayNeeded(engine);
		for (const ModularRunway &r : RunwaysOf(st)) {
			if (r.tiles.size() >= need) runway = true;
		}
	}
	/* Where it comes down, and from where it goes up: a pad or a runway, each by its own way. */
	if (StartsByRocket(engine)) return runway && stand && launch && ModularAirportHasHangar(st);
	if (LandsVertically(engine) ? !pad : !(runway && stand)) return false;
	if (TakesOffVertically(engine) ? !pad : !runway) return false;
	return true;
}

/** @copydoc StartsByRocket() */
bool AircraftStartsByRocket(EngineID engine)
{
	return StartsByRocket(engine);
}

/**
 * What an airport lacks for a plane a rocket carries up (CanVehicleUseStation() says only no): the list of what a hangar
 * builds left such a plane out without a word where the airport could not send it up (the player, 21. 11.: "I have
 * only the Buran with the Mrija there, the Buran with the Energia rocket is not"). It is listed at every hangar now,
 * and buying it, or sending it there, tells what is missing: the airport built tile by tile, its runway, its stand,
 * its hangar, its pad.
 * @return the reason, INVALID_STRING_ID when it can use the airport
 */
StringID GetRocketAircraftCannotUseReason(EngineID engine, const Station *st)
{
	if (!IsModularAirportStation(st)) return STR_ERROR_ROCKET_PLANE_NEEDS_MODULAR_AIRPORT;
	bool stand = false, launch = false;
	for (TileIndex t : st->modular_airport) {
		if (!IsStationModularTile(t, st->index)) continue;
		if (GetModularAirportKind(t) == ModularAirportKind::Stand) stand = true;
		if (IsLaunchPad(t)) launch = true;
	}
	const uint need = RunwayNeeded(engine);
	bool runway = false;
	for (const ModularRunway &r : RunwaysOf(st)) {
		if (r.tiles.size() >= need) runway = true;
	}
	if (!runway) return need > RUNWAY_PLANE ? STR_ERROR_ROCKET_PLANE_NEEDS_LONG_RUNWAY : STR_ERROR_ROCKET_PLANE_NEEDS_RUNWAY;
	if (!stand) return STR_ERROR_ROCKET_PLANE_NEEDS_STAND;
	if (!ModularAirportHasHangar(st)) return STR_ERROR_ROCKET_PLANE_NEEDS_HANGAR;
	if (!launch) return STR_ERROR_ROCKET_PLANE_NEEDS_PAD;
	return INVALID_STRING_ID;
}

/** Has the modular airport a hangar? */
bool ModularAirportHasHangar(const Station *st)
{
	return GetModularAirportHangar(st) != INVALID_TILE;
}

/** The first hangar of the modular airport, INVALID_TILE without one. */
TileIndex GetModularAirportHangar(const Station *st)
{
	if (!IsModularAirportStation(st)) return INVALID_TILE;
	for (TileIndex t : st->modular_airport) {
		if (IsStationModularTile(t, st->index) && GetModularAirportKind(t) == ModularAirportKind::Hangar) return t;
	}
	return INVALID_TILE;
}

/* ---------------------------------------------------------------- the ways over the ground */

/** The tracks an aircraft may go along over the tile (its own and those a stand, a runway's end, a hangar imply). */
static TrackBits PathTracks(TileIndex t)
{
	switch (GetModularAirportKind(t)) {
		case ModularAirportKind::Taxiway:
		case ModularAirportKind::Helipad:
			return GetModularAirportTracks(t);
		case ModularAirportKind::Stand:
			return GetModularAirportTracks(t) | TRACK_BIT_CROSS;
		case ModularAirportKind::RunwayEnd:
			return TRACK_BIT_CROSS;
		case ModularAirportKind::RunwayMiddle:
			return TrackBits{AxisToTrack(GetModularAirportRunwayAxis(t))};
		case ModularAirportKind::Hangar:
			return TrackBits{DiagDirToDiagTrack(GetModularAirportDirection(t))};
		default:
			return {};
	}
}

/** May an aircraft turn round on the tile (a stand, a runway's end, a hangar)? */
static bool IsPivot(TileIndex t)
{
	const ModularAirportKind kind = GetModularAirportKind(t);
	return kind == ModularAirportKind::Stand || kind == ModularAirportKind::RunwayEnd || kind == ModularAirportKind::Hangar;
}

/** Is a tile free for an aircraft's way: nothing of it reserved, and its runway not reserved (unless the aircraft's own)? */
static bool TileFree(TileIndex t, const std::vector<TileIndex> &own)
{
	if (std::ranges::find(own, t) != own.end()) return true;
	if (GetModularAirportReservedTracks(t).Any()) return false;
	if (IsModularAirportRunway(t) && HasModularAirportRunwayReservation(t)) return false;
	return true;
}

static bool RunwayFree(const ModularRunway &r, TileIndex except = INVALID_TILE);

/** Is the tile where an aircraft taxiing with the goal ends its way? */
static bool IsGoal(TileIndex t, ModularAircraftGoal goal, uint runway_needed)
{
	switch (goal) {
		case ModularAircraftGoal::Stand: return GetModularAirportKind(t) == ModularAirportKind::Stand;
		case ModularAircraftGoal::Hangar: return GetModularAirportKind(t) == ModularAirportKind::Hangar;
		case ModularAircraftGoal::Pad: return IsLaunchPad(t);
		case ModularAircraftGoal::Helipad: return GetModularAirportKind(t) == ModularAirportKind::Helipad;
		case ModularAircraftGoal::Runway: {
			if (GetModularAirportKind(t) != ModularAirportKind::RunwayEnd) return false;
			/* A runway another aircraft has something of reserved (its other end, waiting there) is not taxied to:
			 * two planes waiting at its two ends would wait for each other for good. */
			const ModularRunway r = RunwayOf(t);
			return r.tiles.size() >= runway_needed && RunwayFree(r);
		}
		default: return false;
	}
}

/**
 * Search the way of an aircraft over a modular airport to the nearest free tile of its goal (Dijkstra over the tiles
 * and their trackdirs; a stand, a runway's end and a hangar the aircraft may turn round on).
 * @param sid the airport's station
 * @param start where it is: one step, or (on a tile it may turn round on) every way off it
 * @param goal where it goes
 * @param runway_needed the shortest runway it takes off from
 * @param own the tiles reserved for it already (its runway)
 * @return the way, the start first; empty when there is none
 */
static std::vector<ModularAircraftStep> SearchWayIn(StationID sid, const std::vector<ModularAircraftStep> &start, ModularAircraftGoal goal, uint runway_needed, const std::vector<TileIndex> &own, bool through_stands)
{
	auto key = [](TileIndex t, Trackdir td) { return (static_cast<uint64_t>(t.base()) << 4) | to_underlying(td); };
	struct Seen {
		uint cost;
		uint64_t prev;
		bool start;
	};
	std::map<uint64_t, Seen> seen;
	using Item = std::tuple<uint, uint64_t>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
	for (const ModularAircraftStep &s : start) {
		const uint64_t k = key(s.tile, s.trackdir);
		seen[k] = {0, UINT64_MAX, true};
		open.emplace(0, k);
	}
	auto add = [&](TileIndex t, Trackdir td, uint cost, uint64_t prev) {
		const uint64_t k = key(t, td);
		auto it = seen.find(k);
		if (it != seen.end() && it->second.cost <= cost) return;
		seen[k] = {cost, prev, false};
		open.emplace(cost, k);
	};
	auto rebuild = [&](uint64_t k) {
		std::vector<ModularAircraftStep> way;
		while (k != UINT64_MAX) {
			way.push_back({TileIndex(static_cast<uint32_t>(k >> 4)), static_cast<Trackdir>(k & 0xF)});
			k = seen[k].prev;
		}
		std::ranges::reverse(way);
		return way;
	};
	uint expanded = 0;
	while (!open.empty() && expanded < 20000) {
		const auto [cost, k] = open.top();
		open.pop();
		const Seen here = seen[k];
		if (here.cost != cost) continue;
		expanded++;
		const TileIndex t(static_cast<uint32_t>(k >> 4));
		const Trackdir td = static_cast<Trackdir>(k & 0xF);
		if (!here.start && IsGoal(t, goal, runway_needed)) return rebuild(k);
		/* Turn round on the tile. */
		if (IsPivot(t) && !(GetModularAirportKind(t) == ModularAirportKind::Hangar && !here.start)) {
			for (Trackdir other : TrackBitsToTrackdirBits(PathTracks(t))) {
				if (other == td) continue;
				if (GetModularAirportKind(t) == ModularAirportKind::Hangar && TrackdirToExitdir(other) != GetModularAirportDirection(t)) continue;
				add(t, other, cost + 3, k);
			}
		}
		/* On to the next tile. */
		const DiagDirection exit = TrackdirToExitdir(td);
		if (GetModularAirportKind(t) == ModularAirportKind::Hangar && exit != GetModularAirportDirection(t)) continue;
		const TileIndex next = TileAddByDiagDir(t, exit);
		if (!IsStationModularTile(next, sid) || !TileFree(next, own)) continue;
		if (GetModularAirportKind(next) == ModularAirportKind::Hangar && exit != ReverseDiagDir(GetModularAirportDirection(next))) continue;
		/* A goal of another kind is passed over only where it has taxiing tracks of its own (a hangar never). */
		if (GetModularAirportKind(next) == ModularAirportKind::Hangar && goal != ModularAircraftGoal::Hangar) continue;
		/* A stand is where a way ends, not a junction: an aircraft loading on it would hold up every way through it
		 * (TEMATA 365). Through one only when there is no other way (the second search, through_stands). */
		if (GetModularAirportKind(next) == ModularAirportKind::Stand && goal != ModularAircraftGoal::Stand && !through_stands) continue;
		const uint step = IsModularAirportRunway(next) ? 3 : 1;
		/* A pad on its deck has no tracks of its own: a rocket's transporter goes onto it straight from the tile beside it. */
		const TrackBits next_tracks = goal == ModularAircraftGoal::Pad && GetModularAirportKind(next) == ModularAirportKind::Heliport ? TRACK_BIT_CROSS : PathTracks(next);
		for (Trackdir in : DiagdirReachesTrackdirs(exit) & TrackBitsToTrackdirBits(next_tracks)) add(next, in, cost + step, k);
	}
	return {};
}

/**
 * Search the way of an aircraft (SearchWayIn()): first one that ends on a stand but passes through none, then, when
 * there is none, one through free stands.
 */
static std::vector<ModularAircraftStep> SearchWay(StationID sid, const std::vector<ModularAircraftStep> &start, ModularAircraftGoal goal, uint runway_needed, const std::vector<TileIndex> &own)
{
	std::vector<ModularAircraftStep> way = SearchWayIn(sid, start, goal, runway_needed, own, false);
	if (way.empty()) way = SearchWayIn(sid, start, goal, runway_needed, own, true);
	return way;
}

/** Every way off a tile an aircraft stands on (its trackdir now first). */
static std::vector<ModularAircraftStep> WaysOff(TileIndex t, Trackdir now)
{
	std::vector<ModularAircraftStep> start;
	if (!IsPivot(t)) {
		start.push_back({t, now});
		return start;
	}
	for (Trackdir td : TrackBitsToTrackdirBits(PathTracks(t))) {
		if (GetModularAirportKind(t) == ModularAirportKind::Hangar && TrackdirToExitdir(td) != GetModularAirportDirection(t)) continue;
		start.push_back({t, td});
	}
	return start;
}

/** The tracks of the way on a tile from its step from on. */
static TrackBits WayTracksOn(const std::vector<ModularAircraftStep> &way, size_t from, TileIndex t)
{
	TrackBits tracks{};
	for (size_t i = from; i < way.size(); i++) {
		if (way[i].tile == t) tracks.Set(TrackdirToTrack(way[i].trackdir));
	}
	return tracks;
}

/** Reserve a way: its tracks on its tiles, the whole of a stand it ends at. */
static void ReserveWay(const std::vector<ModularAircraftStep> &way)
{
	for (const ModularAircraftStep &s : way) {
		SetModularAirportReservedTracks(s.tile, GetModularAirportReservedTracks(s.tile) | TrackBits{TrackdirToTrack(s.trackdir)});
	}
	if (!way.empty() && GetModularAirportKind(way.back().tile) == ModularAirportKind::Stand) {
		SetModularAirportReservedTracks(way.back().tile, GetModularAirportReservedTracks(way.back().tile) | TRACK_BIT_CROSS);
	}
}

/** Set or let go of the reservation of a whole runway. */
static void ReserveRunway(const ModularRunway &r, bool reserved)
{
	for (TileIndex t : r.tiles) SetModularAirportRunwayReservation(t, reserved);
}

/** Is a runway free: not reserved, nothing of it reserved (but the tile an aircraft waits on, except)? */
static bool RunwayFree(const ModularRunway &r, TileIndex except)
{
	for (TileIndex t : r.tiles) {
		if (HasModularAirportRunwayReservation(t)) return false;
		if (t != except && GetModularAirportReservedTracks(t).Any()) return false;
	}
	return true;
}

/** The middle of a tile, in the units of the map. */
static Point TileMiddle(TileIndex t)
{
	return {static_cast<int>(TileX(t) * TILE_SIZE + TILE_SIZE / 2), static_cast<int>(TileY(t) * TILE_SIZE + TILE_SIZE / 2)};
}

/** The middle of a tile's edge on a side. */
static Point EdgeMiddle(TileIndex t, DiagDirection side)
{
	Point p = TileMiddle(t);
	const TileIndexDiffC d = TileIndexDiffCByDiagDir(side);
	p.x += d.x * static_cast<int>(TILE_SIZE / 2);
	p.y += d.y * static_cast<int>(TILE_SIZE / 2);
	return p;
}

/** Where an aircraft at a step of its way heads for: the edge it leaves the tile by, or its middle where it turns round or stops. */
static Point StepTarget(const std::vector<ModularAircraftStep> &way, size_t i)
{
	if (i + 1 >= way.size() || way[i + 1].tile == way[i].tile) return TileMiddle(way[i].tile);
	return EdgeMiddle(way[i].tile, TrackdirToExitdir(way[i].trackdir));
}

/** The height an aircraft stands at on the ground of a tile (as at an old airport: above its highest corner). */
static int GroundZ(TileIndex t)
{
	return GetTileMaxPixelZ(t) + 1;
}

/** The height a helicopter stands at on its pad: a heliport's deck is 3 m up (objekty/letiste let_heliport), a tile's height. */
static int PadZ(TileIndex t)
{
	return GroundZ(t) + (IsValidTile(t) && IsModularAirportTile(t) && GetModularAirportKind(t) == ModularAirportKind::Heliport ? static_cast<int>(TILE_HEIGHT) : 0);
}

/** The direction from a point towards another, of the eight. */
static Direction DirectionOf(int dx, int dy)
{
	if (dx < 0) return dy < 0 ? Direction::N : dy > 0 ? Direction::E : Direction::NE;
	if (dx > 0) return dy < 0 ? Direction::W : dy > 0 ? Direction::S : Direction::SW;
	return dy < 0 ? Direction::NW : Direction::SE;
}

/**
 * Turn an aircraft on the ground towards a direction, an eighth a tick, standing.
 * @return whether it faces that way (else it turned)
 */
static bool TurnTowards(Aircraft *v, Direction want, bool moving = false)
{
	const DirDiff diff = DirDifference(want, v->direction);
	if (diff == DirDiff::Same) return true;
	/* Rolling into a curve (an eighth): turned on the go, not stopped (TEMATA 365: stop-and-turn at every edge). */
	if (moving && (diff == DirDiff::Left45 || diff == DirDiff::Right45)) {
		v->direction = want;
		v->UpdateDeltaXY();
		return true;
	}
	v->direction = ChangeDir(v->direction, LimitDirDiff(diff));
	v->cur_speed = 0;
	SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos);
	return false;
}

/** Let go of the reservations behind an aircraft that has left a tile of its way. */
static void LeaveTile(Aircraft *v, TileIndex t)
{
	if (!IsValidTile(t) || !IsModularAirportTile(t)) return;
	SetModularAirportReservedTracks(t, GetModularAirportReservedTracks(t) & WayTracksOn(v->modular.path, v->modular.path_pos, t));
}

/* ---------------------------------------------------------------- starting from a place */

/** Where a plane taxies to from where it is, by its orders. */
static ModularAircraftGoal GoalByOrders(const Aircraft *v, const Station *st, bool landed)
{
	const Order &o = v->current_order;
	if (o.IsType(OT_GOTO_DEPOT) && o.GetDestination() == st->index && ModularAirportHasHangar(st)) return ModularAircraftGoal::Hangar;
	if (landed && o.IsType(OT_GOTO_STATION) && o.GetDestination() == st->index) return ModularAircraftGoal::Stand;
	if (landed && !o.IsType(OT_GOTO_STATION) && !o.IsType(OT_GOTO_DEPOT) && ModularAirportHasHangar(st)) return ModularAircraftGoal::Hangar;
	return landed ? ModularAircraftGoal::Stand : ModularAircraftGoal::Runway;
}

/**
 * Find and reserve the way of an aircraft on the ground from where it is to its goal; it starts taxiing.
 * @param v the aircraft
 * @param goal where to
 * @param start where it is
 * @return whether it found one
 */
static bool StartTaxi(Aircraft *v, ModularAircraftGoal goal, const std::vector<ModularAircraftStep> &start)
{
	if (start.empty() || !IsValidTile(start.front().tile) || !IsModularAirportTile(start.front().tile)) return false;
	const Station *st = Station::GetByTile(start.front().tile);
	std::vector<ModularAircraftStep> way;
	/* Standing on its goal already (found again after loading). */
	if (start.size() == 1 && !IsPivot(start.front().tile) && IsGoal(start.front().tile, goal, RunwayNeeded(v->engine_type))) {
		way = start;
	} else {
		way = SearchWay(st->index, start, goal, RunwayNeeded(v->engine_type), {});
	}
	if (way.empty()) return false;
	ReserveWay(way);
	v->modular.path = std::move(way);
	v->modular.path_pos = 0;
	v->modular.goal = goal;
	v->modular.trackdir = v->modular.path.front().trackdir;
	v->modular.state = ModularAircraftState::Taxi;
	v->state = TERM1;
	return true;
}

/**
 * A plane coming to a modular airport: reserve a runway long enough, whole, and its way off it to a free stand (or a
 * hangar); then it flies to the start of its final approach.
 * @return whether it got them
 */
static bool TryStartLanding(Aircraft *v, Station *st)
{
	const uint need = RunwayNeeded(v->engine_type);
	const bool depot = v->current_order.IsType(OT_GOTO_DEPOT);
	struct Option {
		uint distance;
		size_t runway;
		bool from_low;
	};
	std::vector<ModularRunway> runways = RunwaysOf(st);
	std::vector<Option> options;
	for (size_t i = 0; i < runways.size(); i++) {
		const ModularRunway &r = runways[i];
		if (r.tiles.size() < need || !RunwayFree(r)) continue;
		for (bool from_low : {true, false}) {
			const TileIndex land = from_low ? r.tiles.front() : r.tiles.back();
			const Point p = TileMiddle(land);
			options.push_back({static_cast<uint>(abs(p.x - v->x_pos) + abs(p.y - v->y_pos)), i, from_low});
		}
	}
	std::ranges::sort(options, {}, &Option::distance);
	for (const Option &o : options) {
		const ModularRunway &r = runways[o.runway];
		const TileIndex end = o.from_low ? r.tiles.back() : r.tiles.front();
		const DiagDirection dir = r.axis == Axis::X ? (o.from_low ? DiagDirection::SW : DiagDirection::NE) : (o.from_low ? DiagDirection::SE : DiagDirection::NW);
		const ModularAircraftStep stop{end, DiagDirToDiagTrackdir(dir)};
		std::vector<ModularAircraftStep> way;
		for (ModularAircraftGoal goal : {depot ? ModularAircraftGoal::Hangar : ModularAircraftGoal::Stand, ModularAircraftGoal::Hangar}) {
			if (goal == ModularAircraftGoal::Hangar && !ModularAirportHasHangar(st)) continue;
			way = SearchWay(st->index, WaysOff(end, stop.trackdir), goal, need, r.tiles);
			if (!way.empty()) {
				v->modular.goal = goal;
				break;
			}
		}
		if (way.empty()) continue;
		ReserveRunway(r, true);
		ReserveWay(way);
		v->modular.path = std::move(way);
		v->modular.path_pos = 0;
		v->modular.tile = o.from_low ? r.tiles.front() : r.tiles.back();
		v->modular.runway_dir = dir;
		v->modular.state = ModularAircraftState::Approach;
		/* Not FLYING: the old code's flying would turn it to its next airport when its orders are processed. */
		v->state = LANDING;
		return true;
	}
	return false;
}

/**
 * A helicopter coming to a modular airport: reserve a free pad (a helipad or a heliport before a stand; a hangar when
 * it goes to the depot); then it flies to it.
 */
static bool TryStartHeliLanding(Aircraft *v, Station *st)
{
	const bool depot = v->current_order.IsType(OT_GOTO_DEPOT) && ModularAirportHasHangar(st);
	bool has_pad = false;
	for (TileIndex t : st->modular_airport) {
		if (IsStationModularTile(t, st->index) && IsHeliPad(t)) has_pad = true;
	}
	TileIndex best = INVALID_TILE;
	uint best_score = UINT_MAX;
	for (TileIndex t : st->modular_airport) {
		if (!IsStationModularTile(t, st->index)) continue;
		const ModularAirportKind kind = GetModularAirportKind(t);
		/* For the depot too onto a pad, and over the taxiways into the hangar from there (the player, 22. 11.: "the
		 * helicopters cannot start from the hangar, let them go to the helipad over the taxiway as usual"); straight
		 * into the hangar only at an airport without a pad. */
		const bool fits = depot && !has_pad ? kind == ModularAirportKind::Hangar : IsHeliPad(t);
		if (!fits || GetModularAirportReservedTracks(t).Any()) continue;
		const Point p = TileMiddle(t);
		uint score = static_cast<uint>(abs(p.x - v->x_pos) + abs(p.y - v->y_pos));
		if (kind == ModularAirportKind::Stand) score += 10000;
		if (score < best_score) {
			best_score = score;
			best = t;
		}
	}
	if (best == INVALID_TILE) return false;
	SetModularAirportReservedTracks(best, GetModularAirportReservedTracks(best) | TRACK_BIT_CROSS);
	v->modular.tile = best;
	v->modular.state = ModularAircraftState::HeliApproach;
	v->state = HELILANDING;
	v->modular.path.clear();
	v->modular.path_pos = 0;
	return true;
}

/** Hand an aircraft that has left a modular airport back to the old code: it flies on to its next airport. */
static void FlyOn(Aircraft *v)
{
	v->modular.state = ModularAircraftState::None;
	v->modular.path.clear();
	v->modular.path_pos = 0;
	v->state = FLYING;
	v->tile = TileIndex{};
	v->flags.Reset(VehicleAirFlag::HelicopterDirectDescent);
	UpdateAircraftCache(v);
	AircraftNextAirportPos_and_Order(v);
}

/* ---------------------------------------------------------------- the states */

/** An aircraft in the air goes towards a point, as the old code flies one: turning an eighth at a time. */
static bool FlyTowards(Aircraft *v, Point to, uint speed_limit, int want_z)
{
	int count = UpdateAircraftSpeed(v, speed_limit, false);
	while (count-- > 0) {
		if (abs(to.x - v->x_pos) + abs(to.y - v->y_pos) <= 4) return true;
		v->direction = GetDirectionTowards(v, to.x, to.y);
		GetNewVehiclePosResult gp = GetNewVehiclePos(v);
		int z = v->z_pos;
		if (z < want_z) z++;
		if (z > want_z) z--;
		v->tile = TileIndex{};
		SetAircraftPosition(v, gp.x, gp.y, z);
	}
	return abs(to.x - v->x_pos) + abs(to.y - v->y_pos) <= 4;
}

/** The step in a direction along a runway, in the units of the map. */
static Point RunwayStep(DiagDirection dir)
{
	const TileIndexDiffC d = TileIndexDiffCByDiagDir(dir);
	return {d.x, d.y};
}

/** Has an aircraft coming to land been ordered elsewhere meanwhile (the airport of its runway or pad not its next any more)? */
static bool OrderedElsewhere(const Aircraft *v)
{
	if (!IsValidTile(v->modular.tile) || !IsModularAirportTile(v->modular.tile)) return true;
	const Order &o = v->current_order;
	return (o.IsType(OT_GOTO_STATION) || o.IsType(OT_GOTO_DEPOT)) && o.GetDestination() != GetStationIndex(v->modular.tile);
}

/** Abort a plane's approach: its reservations let go of, it circles as the old code does. */
static void AbortApproach(Aircraft *v)
{
	ModularAircraftReleaseAll(v);
	v->modular.state = ModularAircraftState::None;
	v->state = FLYING;
	AircraftNextAirportPos_and_Order(v);
}

/** A plane flying to the start of its final approach. */
static void HandleApproach(Aircraft *v)
{
	/* Its orders changed meanwhile: it does not land here. */
	if (OrderedElsewhere(v)) {
		AbortApproach(v);
		return;
	}
	const Point land = TileMiddle(v->modular.tile);
	const Point step = RunwayStep(v->modular.runway_dir);
	Point start = {land.x - step.x * APPROACH_TILES * static_cast<int>(TILE_SIZE), land.y - step.y * APPROACH_TILES * static_cast<int>(TILE_SIZE)};
	start.x = Clamp(start.x, 0, static_cast<int>(Map::MaxX() * TILE_SIZE));
	start.y = Clamp(start.y, 0, static_cast<int>(Map::MaxY() * TILE_SIZE));
	const int want = GroundZ(v->modular.tile) + PLANE_HOLD_MAX_FLYING_ALTITUDE / 2;
	/* Too high to come down along the runway (the Buran back from space, ROCKET_APEX_Z; the player, 23. 11.: "it comes
	 * down too fast, really too fast": from thousands of units its final approach fell to the runway in a moment):
	 * round the approach's start, four tiles about it, coming down a unit a step till it is at its approach height. */
	if (v->z_pos > want + 2 * static_cast<int>(TILE_HEIGHT)) {
		constexpr int ROUND = 4 * static_cast<int>(TILE_SIZE);
		const int k = (v->tick_counter >> 6) & 3;
		Point c = {start.x + (k == 0 || k == 3 ? ROUND : -ROUND), start.y + (k < 2 ? ROUND : -ROUND)};
		c.x = Clamp(c.x, 0, static_cast<int>(Map::MaxX() * TILE_SIZE));
		c.y = Clamp(c.y, 0, static_cast<int>(Map::MaxY() * TILE_SIZE));
		FlyTowards(v, c, SPEED_LIMIT_HOLD, want);
		return;
	}
	if (FlyTowards(v, start, SPEED_LIMIT_HOLD, want)) {
		v->modular.state = ModularAircraftState::Landing;
		v->state = LANDING;
	}
}

/** A plane on its final approach: straight along the runway, coming down to touch down at its start. */
static void HandleLanding(Aircraft *v)
{
	const Point to = TileMiddle(v->modular.tile);
	const int ground = GroundZ(v->modular.tile);
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_APPROACH, false);
	while (count-- > 0) {
		const int dx = to.x - v->x_pos, dy = to.y - v->y_pos;
		if (dx == 0 && dy == 0) break;
		v->direction = DiagDirToDir(v->modular.runway_dir);
		const int x = v->x_pos + (dx > 0 ? 1 : dx < 0 ? -1 : 0);
		const int y = v->y_pos + (dy > 0 ? 1 : dy < 0 ? -1 : 0);
		int z = v->z_pos;
		const uint dist = static_cast<uint>(abs(dx) + abs(dy));
		const int t = std::max(1, static_cast<int>(dist) - 4);
		if (z - ground >= t) z -= CeilDiv(z - ground, t);
		if (z < ground) z = ground;
		v->tile = TileIndex{};
		SetAircraftPosition(v, x, y, z);
	}
	if (v->x_pos != to.x || v->y_pos != to.y) return;
	/* Touch down. */
	v->tile = TileVirtXY(v->x_pos, v->y_pos);
	SetAircraftPosition(v, v->x_pos, v->y_pos, ground);
	v->UpdateDeltaXY();
	if (!PlayVehicleSound(v, VSE_TOUCHDOWN)) SndPlayVehicleFx(SND_17_SKID_PLANE, v);
	v->modular.state = ModularAircraftState::Rollout;
	v->state = ENDLANDING;
	/* Check if the aircraft needs to be replaced or renewed and send it to a hangar if needed. */
	if (v->NeedsAutomaticServicing()) {
		AutoRestoreBackup cur_company(_current_company, v->owner);
		Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, v->index, DepotCommandFlag::Service, {});
	}
}

/** A plane braking along the runway to its far end; there it lets go of the runway and taxies on. */
static void HandleRollout(Aircraft *v)
{
	const ModularRunway r = RunwayOf(v->modular.tile);
	if (r.tiles.empty()) {
		AbortApproach(v);
		return;
	}
	const TileIndex end = v->modular.runway_dir == DiagDirection::SW || v->modular.runway_dir == DiagDirection::SE ? r.tiles.back() : r.tiles.front();
	const Point to = TileMiddle(end);
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI, false);
	while (count-- > 0 && (v->x_pos != to.x || v->y_pos != to.y)) {
		const int dx = to.x - v->x_pos, dy = to.y - v->y_pos;
		const int x = v->x_pos + (dx > 0 ? 1 : dx < 0 ? -1 : 0);
		const int y = v->y_pos + (dy > 0 ? 1 : dy < 0 ? -1 : 0);
		v->tile = TileVirtXY(x, y);
		SetAircraftPosition(v, x, y, GroundZ(v->tile));
	}
	if (v->x_pos != to.x || v->y_pos != to.y) return;
	v->cur_speed = std::min<uint16_t>(v->cur_speed, SPEED_LIMIT_TAXI * _settings_game.vehicle.plane_speed);
	/* Its way off was reserved with the runway; found again after loading. */
	if (v->modular.path.empty()) {
		const Station *st = Station::GetByTile(end);
		const ModularAircraftStep stop{end, DiagDirToDiagTrackdir(v->modular.runway_dir)};
		ModularAircraftGoal goal = GoalByOrders(v, st, true);
		std::vector<ModularAircraftStep> way = SearchWay(st->index, WaysOff(end, stop.trackdir), goal, RunwayNeeded(v->engine_type), r.tiles);
		if (way.empty() && goal != ModularAircraftGoal::Hangar && ModularAirportHasHangar(st)) {
			goal = ModularAircraftGoal::Hangar;
			way = SearchWay(st->index, WaysOff(end, stop.trackdir), goal, RunwayNeeded(v->engine_type), r.tiles);
		}
		if (way.empty()) return; // waits on the runway's end for a way
		ReserveWay(way);
		v->modular.path = std::move(way);
		v->modular.path_pos = 0;
		v->modular.goal = goal;
	}
	ReserveRunway(r, false);
	v->modular.trackdir = v->modular.path.front().trackdir;
	v->modular.state = ModularAircraftState::Taxi;
	v->state = TERM1;
}

/** An aircraft arrived at the end of its way. */
static void ArriveAtGoal(Aircraft *v)
{
	const TileIndex t = v->tile;
	/* A helicopter come out of its hangar to its pad: up from it, unless it loads here (then as at a stand). */
	if ((v->modular.goal == ModularAircraftGoal::Helipad || v->modular.goal == ModularAircraftGoal::Stand) && IsHelicopterOnPads(v->engine_type) &&
			!(v->current_order.IsType(OT_GOTO_STATION) && v->current_order.GetDestination() == v->targetairport)) {
		SetModularAirportReservedTracks(t, TRACK_BIT_CROSS);
		v->modular.tile = t;
		v->modular.state = ModularAircraftState::HeliAscend;
		v->state = HELITAKEOFF;
		v->cur_speed = 0;
		v->modular.path.clear();
		v->modular.path_pos = 0;
		return;
	}
	switch (v->modular.goal) {
		case ModularAircraftGoal::Helipad:
		case ModularAircraftGoal::Stand:
			v->modular.state = ModularAircraftState::AtStand;
			v->state = TERM1;
			v->cur_speed = 0;
			AircraftEntersTerminal(v);
			break;

		case ModularAircraftGoal::Hangar:
			SetModularAirportReservedTracks(t, {});
			v->modular.state = ModularAircraftState::InHangar;
			v->state = HANGAR;
			VehicleEnterDepot(v);
			break;

		case ModularAircraftGoal::Pad:
			/* On its pad the rocket is stood up; after a while it rises (HandleLaunch()). */
			SetModularAirportReservedTracks(t, TRACK_BIT_CROSS);
			v->modular.tile = t;
			v->modular.state = ModularAircraftState::Launch;
			v->state = HELITAKEOFF;
			v->cur_speed = 0;
			v->turn_counter = LAUNCH_WAIT_TICKS;
			SetAircraftPosition(v, v->x_pos, v->y_pos, PadZ(t));
			break;

		case ModularAircraftGoal::Runway: {
			/* Turned along the runway, it waits for it (HandleHoldShort()). */
			const ModularRunway r = RunwayOf(t);
			v->modular.tile = t;
			v->modular.runway_dir = r.tiles.front() == t ? (r.axis == Axis::X ? DiagDirection::SW : DiagDirection::SE) : (r.axis == Axis::X ? DiagDirection::NE : DiagDirection::NW);
			SetModularAirportReservedTracks(t, TRACK_BIT_CROSS);
			v->modular.state = ModularAircraftState::HoldShort;
			v->state = TERM1;
			v->cur_speed = 0;
			break;
		}

		default: NOT_REACHED();
	}
	v->modular.path.clear();
	v->modular.path_pos = 0;
}

/** An aircraft taxiing along its reserved way. */
static void HandleTaxi(Aircraft *v)
{
	std::vector<ModularAircraftStep> &way = v->modular.path;
	if (way.empty()) {
		/* Its way not reserved (after loading): searched again from where it is. */
		if ((v->tick_counter & 7) != 0) return;
		const TileIndex here = TileVirtXY(v->x_pos, v->y_pos);
		if (!IsModularAirportTile(here)) return;
		const Trackdir td = v->modular.trackdir;
		SetModularAirportReservedTracks(here, {});
		if (!StartTaxi(v, v->modular.goal, {{here, td}})) {
			SetModularAirportReservedTracks(here, TrackBits{TrackdirToTrack(td)});
		}
		return;
	}
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI);
	while (count-- > 0) {
		const Point to = StepTarget(way, v->modular.path_pos);
		if (v->x_pos == to.x && v->y_pos == to.y) {
			if (v->modular.path_pos + 1 >= way.size()) {
				ArriveAtGoal(v);
				return;
			}
			const TileIndex left = way[v->modular.path_pos].tile;
			v->modular.path_pos++;
			if (way[v->modular.path_pos].tile != left) LeaveTile(v, left);
			v->modular.trackdir = way[v->modular.path_pos].trackdir;
			continue;
		}
		const int dx = to.x - v->x_pos, dy = to.y - v->y_pos;
		if (!TurnTowards(v, DirectionOf(dx, dy), true)) return;
		const int x = v->x_pos + (dx > 0 ? 1 : dx < 0 ? -1 : 0);
		const int y = v->y_pos + (dy > 0 ? 1 : dy < 0 ? -1 : 0);
		v->tile = TileVirtXY(x, y);
		SetAircraftPosition(v, x, y, GroundZ(v->tile));
	}
}

/** What an aircraft standing at a stand does when it is done there (the old code's AircraftEventHandler_AtTerminal()). */
static void HandleAtStand(Aircraft *v)
{
	if (v->current_order.IsType(OT_NOTHING)) return;
	if (v->current_order.IsType(OT_CONDITIONAL)) return;
	const Station *st = Station::GetIfValid(v->targetairport);
	if (st == nullptr) return;
	bool go_to_hangar = false;
	switch (v->current_order.GetType()) {
		case OT_GOTO_STATION: break;
		case OT_GOTO_DEPOT: go_to_hangar = v->current_order.GetDestination() == v->targetairport; break;
		default:
			v->current_order.Free();
			go_to_hangar = true;
			break;
	}
	if (go_to_hangar && !ModularAirportHasHangar(st)) go_to_hangar = false;
	/* A plane on a rocket leaves through the hangar, where its rocket is (HandleInHangar()). */
	const bool rocket = StartsByRocket(v->engine_type);
	if (rocket && ModularAirportHasHangar(st)) go_to_hangar = true;
	if (TakesOffVertically(v->engine_type) && !rocket && !go_to_hangar) {
		if (v->current_order.IsType(OT_NOTHING)) return; // without orders it stays on its pad
		/* Up from the pad; for the hangar of this airport it taxis there (below), of another it lands on a pad there. */
		v->modular.state = ModularAircraftState::HeliAscend;
		v->state = HELITAKEOFF;
		return;
	}
	if ((v->tick_counter & 3) != 0) return;
	const TileIndex here = v->tile;
	const TrackBits mine = GetModularAirportReservedTracks(here);
	SetModularAirportReservedTracks(here, {});
	/* A helicopter come down onto its pad faces whichever way it landed: it turns there to any of the pad's ways. */
	std::vector<ModularAircraftStep> off = WaysOff(here, DiagDirToDiagTrackdir(DirToDiagDir(v->direction)));
	if (IsHelicopterOnPads(v->engine_type)) {
		off.clear();
		for (Trackdir td : TrackBitsToTrackdirBits(PathTracks(here))) off.push_back({here, td});
	}
	if (!StartTaxi(v, go_to_hangar ? ModularAircraftGoal::Hangar : ModularAircraftGoal::Runway, off)) {
		SetModularAirportReservedTracks(here, mine);
	}
}

/** What an aircraft in a hangar does (the old code's AircraftEventHandler_InHangar()). */
static void HandleInHangar(Aircraft *v)
{
	if (v->current_order.IsType(OT_GOTO_DEPOT) && v->vehstatus.Test(VehState::Stopped)) {
		v->current_order.Free();
		return;
	}
	if (v->IsWaitingForUnbunching()) return;
	if (!v->current_order.IsType(OT_GOTO_STATION) && !v->current_order.IsType(OT_GOTO_DEPOT)) return;
	if (v->current_order.IsType(OT_GOTO_DEPOT) && v->current_order.GetDestination() == v->targetairport) {
		VehicleEnterDepot(v);
		return;
	}
	const TileIndex hangar = v->tile;
	if (!IsValidTile(hangar) || !IsModularAirportTile(hangar) || GetModularAirportKind(hangar) != ModularAirportKind::Hangar) return;
	const DiagDirection doors = GetModularAirportDirection(hangar);
	const bool rocket = StartsByRocket(v->engine_type);
	if ((v->tick_counter & 3) != 0) return;
	if (GetModularAirportReservedTracks(hangar).Any()) return;
	const bool here = v->current_order.IsType(OT_GOTO_STATION) && v->current_order.GetDestination() == v->targetairport;
	if (IsHelicopterOnPads(v->engine_type)) {
		/* A helicopter out over the taxiways to a helipad, else to a stand, and up from there or loading there (the
		 * player, 22. 11.: "the helicopters cannot start from the hangar, let them go to the helipad over the taxiway
		 * as usual"; ArriveAtGoal()). */
		if (!StartTaxi(v, ModularAircraftGoal::Helipad, {{hangar, DiagDirToDiagTrackdir(doors)}}) &&
				!StartTaxi(v, ModularAircraftGoal::Stand, {{hangar, DiagDirToDiagTrackdir(doors)}})) {
			return;
		}
		AircraftLeaveHangar(v, DiagDirToDir(doors));
		return;
	}
	/* To a stand to load here; else to where it leaves from: a plane to a runway, one on a rocket to a pad. */
	if (!StartTaxi(v, here ? ModularAircraftGoal::Stand : rocket ? ModularAircraftGoal::Pad : ModularAircraftGoal::Runway, {{hangar, DiagDirToDiagTrackdir(doors)}})) return;
	AircraftLeaveHangar(v, DiagDirToDir(doors));
}

/** A plane at the end of a runway: turned along it, it waits for it to be free and reserves it. */
static void HandleHoldShort(Aircraft *v)
{
	if (!TurnTowards(v, DiagDirToDir(v->modular.runway_dir))) return;
	const ModularRunway r = RunwayOf(v->modular.tile);
	if (r.tiles.empty()) return;
	if (!RunwayFree(r, v->modular.tile)) return;
	ReserveRunway(r, true);
	SetModularAirportReservedTracks(v->modular.tile, {});
	PlayAircraftSound(v);
	v->modular.state = ModularAircraftState::TakeoffRoll;
	v->state = STARTTAKEOFF;
}

/** A plane speeding up along its runway; it lifts off over its last tile and climbs out. */
static void HandleTakeoff(Aircraft *v)
{
	const ModularRunway r = RunwayOf(v->modular.tile);
	if (r.tiles.empty()) {
		FlyOn(v);
		return;
	}
	const Point step = RunwayStep(v->modular.runway_dir);
	const Point end = TileMiddle(v->modular.tile == r.tiles.front() ? r.tiles.back() : r.tiles.front());
	const int ground = GroundZ(v->modular.tile);
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_NONE);
	while (count-- > 0) {
		const int x = v->x_pos + step.x, y = v->y_pos + step.y;
		/* How far past its last tile's middle, units (negative before it). */
		const int past = (x - end.x) * step.x + (y - end.y) * step.y;
		int z = v->z_pos;
		if (past > -static_cast<int>(TILE_SIZE)) {
			if (v->modular.state == ModularAircraftState::TakeoffRoll) {
				v->modular.state = ModularAircraftState::Climb;
				v->state = ENDTAKEOFF;
				v->UpdateDeltaXY();
			}
			z++;
		}
		const bool on_map = x > 0 && y > 0 && x < static_cast<int>(Map::MaxX() * TILE_SIZE) && y < static_cast<int>(Map::MaxY() * TILE_SIZE);
		v->tile = v->modular.state == ModularAircraftState::TakeoffRoll && on_map ? TileVirtXY(x, y) : TileIndex{};
		SetAircraftPosition(v, on_map ? x : v->x_pos, on_map ? y : v->y_pos, z);
		/* Off the runway and up: it is let go of; out over its end the old code flies the plane on. */
		if (!r.tiles.empty() && HasModularAirportRunwayReservation(r.tiles.front()) && z > ground + static_cast<int>(TILE_HEIGHT) * 2) ReserveRunway(r, false);
		if (past > CLIMB_OUT_TILES * static_cast<int>(TILE_SIZE) || !on_map) {
			if (!r.tiles.empty() && HasModularAirportRunwayReservation(r.tiles.front())) ReserveRunway(r, false);
			FlyOn(v);
			return;
		}
	}
}

/** A helicopter flying to its pad. */
static void HandleHeliApproach(Aircraft *v)
{
	if (OrderedElsewhere(v)) {
		AbortApproach(v);
		return;
	}
	const Point to = TileMiddle(v->modular.tile);
	if (abs(to.x - v->x_pos) + abs(to.y - v->y_pos) > 4) {
		FlyTowards(v, to, SPEED_LIMIT_HOLD, GetAircraftHoldMaxAltitude(v));
		return;
	}
	/* Right over it. */
	int count = UpdateAircraftSpeed(v, SPEED_LIMIT_TAXI);
	while (count-- > 0 && (v->x_pos != to.x || v->y_pos != to.y)) {
		const int dx = to.x - v->x_pos, dy = to.y - v->y_pos;
		SetAircraftPosition(v, v->x_pos + (dx > 0 ? 1 : dx < 0 ? -1 : 0), v->y_pos + (dy > 0 ? 1 : dy < 0 ? -1 : 0), v->z_pos);
	}
	if (v->x_pos != to.x || v->y_pos != to.y) return;
	v->modular.state = ModularAircraftState::HeliDescend;
	v->state = HELILANDING;
	v->flags.Set(VehicleAirFlag::HelicopterDirectDescent);
	v->UpdateDeltaXY();
}

/** The rotor of a helicopter (its third part); nullptr for an aircraft without one. */
static Aircraft *Rotor(Aircraft *v)
{
	Aircraft *u = v->Next();
	return u != nullptr ? u->Next() : nullptr;
}

/** A helicopter coming down onto its pad (the old code's helicopter landing). */
static void HandleHeliDescend(Aircraft *v)
{
	const TileIndex pad = v->modular.tile;
	const int z = PadZ(pad);
	v->tile = pad;
	if (v->z_pos == z) {
		Aircraft *u = Rotor(v);
		/* Increase speed of rotors. When speed is 80, we've landed. */
		if (u != nullptr && u->cur_speed < 80) {
			u->cur_speed += 4;
			return;
		}
		v->flags.Reset(VehicleAirFlag::HelicopterDirectDescent);
		v->cur_speed = 0;
		if (GetModularAirportKind(pad) == ModularAirportKind::Hangar) {
			SetModularAirportReservedTracks(pad, {});
			v->modular.state = ModularAircraftState::InHangar;
			v->state = HANGAR;
			VehicleEnterDepot(v);
			return;
		}
		v->modular.state = ModularAircraftState::AtStand;
		v->state = HELIPAD1;
		AircraftEntersTerminal(v);
		/* A helicopter landing on a helipad is serviced, as at an old airport with helipads. */
		if (_settings_game.order.serviceathelipad && GetModularAirportKind(pad) != ModularAirportKind::Stand) {
			v->date_of_last_service = TimerGameEconomy::date;
			v->date_of_last_service_newgrf = TimerGameCalendar::date;
			v->breakdowns_since_last_service = 0;
			v->reliability = v->GetEngine()->reliability;
			SetWindowDirty(WindowClass::VehicleDetails, v->index);
		}
		return;
	}
	int count = UpdateAircraftSpeed(v);
	if (count > 0) SetAircraftPosition(v, v->x_pos, v->y_pos, v->z_pos > z ? std::max(v->z_pos - count, z) : std::min(v->z_pos + count, z));
}

/** A plane on its rocket stood up on its pad: after a while it rises as a helicopter does (HandleHeliAscend()), with its engines' roar. */
/**
 * The smoke of a rocket's engines (the player, 19. 11.: the flame and the smoke of the rocket; the flame is the GRF's,
 * by the movement state): a white cloud of the game's own now and then, somewhere within spread of the rocket.
 * @param dz how far under the rocket (the pad's ground while it stands, the end of its flame while it climbs)
 */
static void RocketSmoke(const Aircraft *v, int spread, int dz)
{
	const uint32_t r = Random();
	if (GB(r, 0, 2) != 0) return;
	const int dx = static_cast<int>(GB(r, 2, 8)) * (2 * spread + 1) / 256 - spread;
	const int dy = static_cast<int>(GB(r, 10, 8)) * (2 * spread + 1) / 256 - spread;
	CreateEffectVehicleRel(v, dx, dy, dz, GB(r, 18, 2) == 0 ? EV_CRASH_SMOKE : EV_STEAM_SMOKE);
}

static void HandleLaunch(Aircraft *v)
{
	if (v->turn_counter > 0) {
		v->turn_counter--;
		/* Its engines lit a while before it rises: the smoke about the pad (the GRF shows its flame, MapAircraftMovementState()). */
		if (v->turn_counter <= ROCKET_IGNITION_STEPS) RocketSmoke(v, TILE_SIZE, 0);
		return;
	}
	if (!PlayVehicleSound(v, VSE_START)) {
		SoundID sfx = AircraftVehInfo(v->engine_type)->sfx;
		if (sfx < ORIGINAL_SAMPLE_COUNT) SndPlayVehicleFx(sfx, v);
	}
	v->modular.state = ModularAircraftState::HeliAscend;
	v->state = HELITAKEOFF;
}

/** A helicopter rising from its pad or its hangar (the old code's helicopter raise); in the air the old code flies it on. */
static void HandleHeliAscend(Aircraft *v)
{
	Aircraft *u = Rotor(v);
	/* Make sure the rotors don't rotate too fast. */
	if (u != nullptr && u->cur_speed > 32) {
		v->cur_speed = 0;
		if (--u->cur_speed == 32 && !PlayVehicleSound(v, VSE_START)) {
			SoundID sfx = AircraftVehInfo(v->engine_type)->sfx;
			if (sfx < ORIGINAL_SAMPLE_COUNT) sfx = SND_18_TAKEOFF_HELICOPTER;
			SndPlayVehicleFx(sfx, v);
		}
		return;
	}
	if (u != nullptr) u->cur_speed = 32;
	/* A rocket's smoke behind it as it climbs. */
	if (StartsByRocket(v->engine_type)) RocketSmoke(v, TILE_SIZE / 2, -static_cast<int>(TILE_HEIGHT));
	int count = UpdateAircraftSpeed(v);
	if (count == 0) return;
	const TileIndex pad = v->tile;
	int z_dest;
	GetAircraftFlightLevelBounds(v, &z_dest, nullptr);
	/* A rocket as high as it goes, to space (the player, 22. 11.: "shoot the Buran up as high as you can, it is a
	 * spaceship"; it looked low from above and level with the eyes from the ground), twice as fast as it climbs at
	 * first; up there it flies on and comes down to its flight level as the old code brings an aircraft down. */
	if (StartsByRocket(v->engine_type)) {
		z_dest = std::max(z_dest, ROCKET_APEX_Z);
		count *= 2;
	}
	SetAircraftPosition(v, v->x_pos, v->y_pos, std::min(v->z_pos + count, z_dest));
	/* Its pad free once it is up off it. */
	if (IsValidTile(pad) && IsModularAirportTile(pad) && GetModularAirportKind(pad) != ModularAirportKind::Hangar && v->z_pos > PadZ(pad) + static_cast<int>(TILE_HEIGHT)) {
		SetModularAirportReservedTracks(pad, {});
		v->tile = TileIndex{};
	}
	if (v->z_pos >= z_dest) {
		if (IsValidTile(pad) && IsModularAirportTile(pad) && GetModularAirportKind(pad) != ModularAirportKind::Hangar) SetModularAirportReservedTracks(pad, {});
		v->cur_speed = 0;
		FlyOn(v);
	}
}

/**
 * The modular airports' part of an aircraft's tick (AircraftEventHandler()).
 * @return whether it took the tick: the aircraft is at a modular airport, or now lands at one
 */
bool ModularAircraftController(Aircraft *v)
{
	if (v->modular.state == ModularAircraftState::None) {
		if (v->state != FLYING || v->raid_target != INVALID_TILE || v->cab_free) return false;
		Station *st = Station::GetIfValid(v->targetairport);
		if (!IsModularAirportStation(st)) return false;
		const Order &o = v->current_order;
		if ((o.IsType(OT_GOTO_STATION) || o.IsType(OT_GOTO_DEPOT)) && o.GetDestination() != v->targetairport) return false;
		if (st->owner != OWNER_NONE && st->owner != v->owner) return false;
		if (!CanVehicleUseStation(v, st)) return false;
		/* Closed for aircraft from the station's window: it circles (as at an old airport). */
		if (st->airport.blocks.Test(AirportBlock::AirportClosed)) return false;
		if ((v->tick_counter & 3) != 0) return false;
		const TileIndex here = TileVirtXYClampedToMap(v->x_pos, v->y_pos);
		const TileIndex middle = TileXY(TileX(st->modular_airport.tile) + st->modular_airport.w / 2, TileY(st->modular_airport.tile) + st->modular_airport.h / 2);
		if (DistanceManhattan(here, middle) > LANDING_REACH) return false;
		if (!(LandsVertically(v->engine_type) ? TryStartHeliLanding(v, st) : TryStartLanding(v, st))) return false;
	}

	switch (v->modular.state) {
		case ModularAircraftState::Approach: HandleApproach(v); break;
		case ModularAircraftState::Landing: HandleLanding(v); break;
		case ModularAircraftState::Rollout: HandleRollout(v); break;
		case ModularAircraftState::Taxi: HandleTaxi(v); break;
		case ModularAircraftState::AtStand: HandleAtStand(v); break;
		case ModularAircraftState::InHangar: HandleInHangar(v); break;
		case ModularAircraftState::HoldShort: HandleHoldShort(v); break;
		case ModularAircraftState::TakeoffRoll:
		case ModularAircraftState::Climb: HandleTakeoff(v); break;
		case ModularAircraftState::HeliApproach: HandleHeliApproach(v); break;
		case ModularAircraftState::HeliDescend: HandleHeliDescend(v); break;
		case ModularAircraftState::HeliAscend: HandleHeliAscend(v); break;
		case ModularAircraftState::Launch: HandleLaunch(v); break;
		default:
			v->modular.state = ModularAircraftState::None;
			return false;
	}
	return true;
}

/** An aircraft bought in a modular airport's hangar (CmdBuildAircraft()): in its middle, waiting. */
void ModularAircraftBought(Aircraft *v, TileIndex hangar)
{
	const Point p = TileMiddle(hangar);
	v->x_pos = p.x;
	v->y_pos = p.y;
	v->z_pos = GroundZ(hangar);
	Aircraft *u = v->Next();
	if (u != nullptr) {
		u->x_pos = p.x;
		u->y_pos = p.y;
		u->z_pos = GetSlopePixelZ(p.x, p.y);
	}
	v->direction = DiagDirToDir(GetModularAirportDirection(hangar));
	v->modular = {};
	v->modular.state = ModularAircraftState::InHangar;
}

/** Let go of all an aircraft holds at a modular airport (it crashed, or its approach was aborted). */
void ModularAircraftReleaseAll(Aircraft *v)
{
	if (!v->IsNormalAircraft()) return;
	for (size_t i = v->modular.path_pos; i < v->modular.path.size(); i++) {
		const TileIndex t = v->modular.path[i].tile;
		if (IsValidTile(t) && IsModularAirportTile(t)) SetModularAirportReservedTracks(t, {});
	}
	v->modular.path.clear();
	v->modular.path_pos = 0;
	switch (v->modular.state) {
		case ModularAircraftState::Approach:
		case ModularAircraftState::Landing:
		case ModularAircraftState::Rollout:
		case ModularAircraftState::TakeoffRoll:
		case ModularAircraftState::Climb: {
			const ModularRunway r = RunwayOf(v->modular.tile);
			if (!r.tiles.empty()) ReserveRunway(r, false);
			break;
		}
		case ModularAircraftState::HeliApproach:
		case ModularAircraftState::HeliDescend:
			if (IsValidTile(v->modular.tile) && IsModularAirportTile(v->modular.tile)) SetModularAirportReservedTracks(v->modular.tile, {});
			break;
		case ModularAircraftState::Taxi:
		case ModularAircraftState::AtStand:
		case ModularAircraftState::HoldShort:
		case ModularAircraftState::Launch:
			if (IsValidTile(v->tile) && IsModularAirportTile(v->tile)) SetModularAirportReservedTracks(v->tile, {});
			break;
		default: break;
	}
}

/**
 * After a game is loaded: the reservations of the modular airports made again from where their aircraft are (an
 * aircraft's way is not saved; one taxiing searches it again from where it is).
 */
void AfterLoadModularAircraft()
{
	for (const auto tile : Map::Iterate()) {
		if (!IsModularAirportTile(tile)) continue;
		SetModularAirportReservedTracks(tile, {});
		if (IsModularAirportRunway(tile)) SetModularAirportRunwayReservation(tile, false);
	}
	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft() || v->modular.state == ModularAircraftState::None) continue;
		if (v->modular.state >= ModularAircraftState::End) v->modular.state = ModularAircraftState::None;
		v->modular.path.clear();
		v->modular.path_pos = 0;
		const Station *st = Station::GetIfValid(v->targetairport);
		const bool here_ok = IsValidTile(v->tile) && IsModularAirportTile(v->tile);
		auto lost = [&]() {
			/* What it was at is gone: in the air it flies on as the old code flies it, on the ground it waits. */
			v->modular.state = ModularAircraftState::None;
			v->state = FLYING;
		};
		if (!IsModularAirportStation(st)) {
			lost();
			continue;
		}
		switch (v->modular.state) {
			case ModularAircraftState::Approach:
				/* Not near yet: it looks for a runway again. */
				lost();
				break;
			case ModularAircraftState::Landing:
			case ModularAircraftState::Rollout:
			case ModularAircraftState::TakeoffRoll:
			case ModularAircraftState::Climb: {
				const ModularRunway r = RunwayOf(v->modular.tile);
				if (r.tiles.empty()) {
					lost();
					break;
				}
				ReserveRunway(r, true);
				break;
			}
			case ModularAircraftState::Taxi:
				if (!here_ok) {
					lost();
					break;
				}
				SetModularAirportReservedTracks(v->tile, GetModularAirportReservedTracks(v->tile) | TrackBits{TrackdirToTrack(v->modular.trackdir)});
				break;
			case ModularAircraftState::AtStand:
			case ModularAircraftState::HoldShort:
			case ModularAircraftState::Launch:
				if (here_ok) SetModularAirportReservedTracks(v->tile, TRACK_BIT_CROSS);
				break;
			case ModularAircraftState::HeliApproach:
			case ModularAircraftState::HeliDescend:
				if (!IsValidTile(v->modular.tile) || !IsModularAirportTile(v->modular.tile)) {
					lost();
					break;
				}
				SetModularAirportReservedTracks(v->modular.tile, TRACK_BIT_CROSS);
				break;
			default: break;
		}
	}
}

/** Is a modular airport's tile in use by an aircraft: reserved, its runway reserved, or an aircraft in its hangar? */
bool ModularAirportTileInUse(TileIndex tile)
{
	if (!IsModularAirportTile(tile)) return false;
	if (GetModularAirportReservedTracks(tile).Any()) return true;
	if (IsModularAirportRunway(tile) && HasModularAirportRunwayReservation(tile)) return true;
	for (const Aircraft *v : Aircraft::Iterate()) {
		if (v->IsNormalAircraft() && v->tile == tile && v->modular.state != ModularAircraftState::None) return true;
	}
	return false;
}
