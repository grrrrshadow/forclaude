/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * Changed for ottd Decouple by Karel Mácha: the changes Copyright (C) 2026 Karel Mácha.
 */

/** @file aircraft.h Base for aircraft. */

#ifndef AIRCRAFT_H
#define AIRCRAFT_H

#include "station_map.h"
#include "vehicle_base.h"
#include "track_type.h"

#include <vector>

/**
 * Base values for flight levels above ground level for 'normal' flight and holding patterns.
 * Due to speed and direction, the actual flight level may be higher.
 */
static constexpr int AIRCRAFT_MIN_FLYING_ALTITUDE = 120; ///< Minimum flying altitude above tile.
static constexpr int AIRCRAFT_MAX_FLYING_ALTITUDE = 360; ///< Maximum flying altitude above tile.
static constexpr int PLANE_HOLD_MAX_FLYING_ALTITUDE = 150; ///< holding flying altitude above tile of planes.
static constexpr int HELICOPTER_HOLD_MAX_FLYING_ALTITUDE = 184; ///< holding flying altitude above tile of helicopters.

struct Aircraft;

/** An aircraft can be one of those types. */
enum AircraftSubType : uint8_t {
	AIR_HELICOPTER = 0, ///< an helicopter
	AIR_AIRCRAFT   = 2, ///< an airplane
	AIR_SHADOW     = 4, ///< shadow of the aircraft
	AIR_ROTOR      = 6, ///< rotor of an helicopter
};

/** Flags for air vehicles; shared with disaster vehicles. */
enum class VehicleAirFlag : uint8_t {
	DestinationTooFar = 0, ///< Next destination is too far away.

	/* The next two flags are to prevent stair climbing of the aircraft. The idea is that the aircraft
	 * will ascend or descend multiple flight levels at a time instead of following the contours of the
	 * landscape at a fixed altitude. This only has effect when there are more than 15 height levels. */
	InMaximumHeightCorrection = 1, ///< The vehicle is currently lowering its altitude because it hit the upper bound.
	InMinimumHeightCorrection = 2, ///< The vehicle is currently raising its altitude because it hit the lower bound.

	HelicopterDirectDescent = 3, ///< The helicopter is descending directly at its destination (helipad or in front of hangar)
};

/** Bitset of \c VehicleAirFlag elements. */
using VehicleAirFlags = EnumBitSet<VehicleAirFlag, uint8_t>;

static const int ROTOR_Z_OFFSET         = 5;    ///< Z Offset between helicopter- and rotorsprite.

void HandleAircraftEnterHangar(Aircraft *v);
void GetAircraftSpriteSize(EngineID engine, uint &width, uint &height, int &xoffs, int &yoffs, EngineImageType image_type);
void UpdateAirplanesOnNewStation(const Station *st);
void UpdateAircraftCache(Aircraft *v, bool update_range = false);

void AircraftLeaveHangar(Aircraft *v, Direction exit_dir);
void AircraftNextAirportPos_and_Order(Aircraft *v);
void SetAircraftPosition(Aircraft *v, int x, int y, int z);

void GetAircraftFlightLevelBounds(const Vehicle *v, int *min, int *max);
int GetAircraftHoldMaxAltitude(const Aircraft *v);

static constexpr uint16_t SPEED_LIMIT_TAXI = 50; ///< Maximum speed of an aircraft while taxiing
static constexpr uint16_t SPEED_LIMIT_APPROACH = 230; ///< Maximum speed of an aircraft on finals
static constexpr uint16_t SPEED_LIMIT_BROKEN = 320; ///< Maximum speed of an aircraft that is broken
static constexpr uint16_t SPEED_LIMIT_HOLD = 425; ///< Maximum speed of an aircraft that flies the holding pattern
static constexpr uint16_t SPEED_LIMIT_NONE = UINT16_MAX; ///< No environmental speed limit. Speed limit is type dependent

int UpdateAircraftSpeed(Aircraft *v, uint speed_limit = SPEED_LIMIT_NONE, bool hard_limit = true);
void PlayAircraftSound(const Vehicle *v);
void AircraftEntersTerminal(Aircraft *v);
template <class T>
int GetAircraftFlightLevel(T *v, bool takeoff = false);

/**
 * Where an aircraft is at a modular airport (air_map.h, aircraft_modular.cpp): the airports built tile by tile have no
 * movement table, the aircraft finds its own way over their taxiing tracks. None: not at one (the old airports' table,
 * or the open air).
 */
/** A rocket stood up on its pad (ModularAircraftState::Launch): its engines are lit for so many of the last steps of its wait (the aircraft's handler steps twice a tick) -- the smoke about the pad, the movement state a GRF shows its flame by. */
static constexpr uint8_t ROCKET_IGNITION_STEPS = 90;

enum class ModularAircraftState : uint8_t {
	None = 0, ///< Not at a modular airport.
	Approach = 1, ///< A plane flying to the start of its final approach, its runway and its way off it reserved.
	Landing = 2, ///< A plane on its final approach, coming down along the runway.
	Rollout = 3, ///< A plane braking along the runway after touching down.
	Taxi = 4, ///< On the taxiing tracks, its way reserved to where it goes (ModularAircraft::goal).
	AtStand = 5, ///< Standing at a stand (a helicopter at its pad): loading, unloading, waiting for its way out.
	InHangar = 6, ///< In a hangar.
	HoldShort = 7, ///< A plane at the end of a runway, waiting for it to be free.
	TakeoffRoll = 8, ///< A plane speeding up along the runway.
	Climb = 9, ///< A plane climbing out over the end of the runway.
	HeliApproach = 10, ///< A helicopter flying to the pad it reserved.
	HeliDescend = 11, ///< A helicopter coming down onto its pad.
	HeliAscend = 12, ///< A helicopter rising from its pad or its hangar; a rocket rising from its pad.
	Launch = 13, ///< An aircraft a rocket carries, on its pad: stood up, waiting to rise; its engines lit for the last ROCKET_IGNITION_STEPS of it.
	End, ///< End marker.
};

/** Where an aircraft taxiing at a modular airport goes. */
enum class ModularAircraftGoal : uint8_t {
	Stand = 0, ///< A free stand (to load).
	Hangar = 1, ///< A hangar (to the depot).
	Runway = 2, ///< The end of a runway (to take off).
	Pad = 3, ///< A helicopters' pad, out of the hangar on its rocket (to rise from).
	Helipad = 4, ///< A helipad or else a stand, a helicopter out of its hangar or into it (to rise from, or to load at).
	End, ///< End marker.
};

/** A step of an aircraft's way over a modular airport: a tile and the way it goes over it. */
struct ModularAircraftStep {
	TileIndex tile; ///< The tile.
	Trackdir trackdir = Trackdir::Invalid; ///< Its track and direction on it.
	bool operator==(const ModularAircraftStep &) const = default;
};

/** An aircraft at a modular airport (aircraft_modular.cpp). */
struct ModularAircraft {
	ModularAircraftState state = ModularAircraftState::None; ///< Where it is. Saved.
	ModularAircraftGoal goal = ModularAircraftGoal::Stand; ///< Where it taxies to. Saved.
	DiagDirection runway_dir = DiagDirection::NE; ///< The way it lands or takes off along its runway. Saved.
	Trackdir trackdir = Trackdir::Invalid; ///< Its track and direction on the tile it taxies over. Saved.
	TileIndex tile = INVALID_TILE; ///< The end of the runway it lands at or takes off from; a helicopter's pad. Saved.
	std::vector<ModularAircraftStep> path; ///< Its way reserved, from where it is. Not saved: found again after loading.
	uint path_pos = 0; ///< The step of the way it is at.
};

/** Variables that are cached to improve performance and such. */
struct AircraftCache {
	uint32_t cached_max_range_sqr = 0; ///< Cached squared maximum range.
	uint16_t cached_max_range = 0; ///< Cached maximum range.
};

/**
 * Aircraft, helicopters, rotors and their shadows belong to this class.
 */
struct Aircraft final : public SpecializedVehicle<Aircraft, VehicleType::Aircraft> {
	uint16_t crashed_counter = 0; ///< Timer for handling crash animations.
	uint8_t pos = 0; ///< Next desired position of the aircraft.
	uint8_t previous_pos = 0; ///< Previous desired position of the aircraft.
	StationID targetairport = StationID::Invalid(); ///< Airport to go to next.
	uint8_t state = 0; ///< State of the airport. @see AirportMovementStates
	Direction last_direction = Direction::Invalid;
	uint8_t number_consecutive_turns = 0; ///< Protection to prevent the aircraft of making a lot of turns in order to reach a specific point.
	uint8_t turn_counter = 0; ///< Ticks between each turn to prevent > 45 degree turns.
	VehicleAirFlags flags{}; ///< Aircraft flags. @see VehicleAirFlags

	AircraftCache acache{};
	ModularAircraft modular{}; ///< At a modular airport (aircraft_modular.cpp).
	uint8_t space_steps = 0; ///< Steps flown coming down from space (a plane a rocket took up): it descends a unit every fourth (GetAircraftFlightLevel()); not saved

	Aircraft(VehicleID index) : SpecializedVehicleBase(index) {}
	/** We want to 'destruct' the right class. */
	~Aircraft() override { this->PreDestructor(); }

	void MarkDirty() override;
	void UpdateDeltaXY() override;
	ExpensesType GetExpenseType(bool income) const override { return income ? ExpensesType::AircraftRevenue : ExpensesType::AircraftRun; }
	bool IsPrimaryVehicle() const override                  { return this->IsNormalAircraft(); }
	void GetImage(Direction direction, EngineImageType image_type, VehicleSpriteSeq *result) const override;
	int GetDisplaySpeed() const override    { return this->cur_speed; }
	int GetDisplayMaxSpeed() const override { return this->vcache.cached_max_speed; }
	int GetSpeedOldUnits() const            { return this->vcache.cached_max_speed * 10 / 128; }
	int GetCurrentMaxSpeed() const override { return this->GetSpeedOldUnits(); }
	Money GetRunningCost() const override;

	bool IsInDepot() const override
	{
		assert(this->IsPrimaryVehicle());
		return this->vehstatus.Test(VehState::Hidden) && IsHangarTile(this->tile);
	}

	bool Tick() override;
	void OnNewCalendarDay() override;
	void OnNewEconomyDay() override;
	uint Crash(bool flooded = false) override;
	TileIndex GetOrderStationLocation(StationID station) override;
	TileIndex GetCargoTile() const override { return this->First()->tile; }
	ClosestDepot FindClosestDepot() override;

	/**
	 * Where this aircraft was sent to drop its smoke, or INVALID_TILE.
	 *
	 * Not in the savegame: an errand the player has not finished does not
	 * follow them into the next game, which is the rule the crosshair itself
	 * follows (see #_show_industry_health).
	 */
	TileIndex raid_target = INVALID_TILE;

	/**
	 * Check if the aircraft type is a normal flying device; eg
	 * not a rotor or a shadow
	 * @return Returns true if the aircraft is a helicopter/airplane and
	 * false if it is a shadow or a rotor
	 */
	inline bool IsNormalAircraft() const
	{
		/* To be fully correct the commented out functionality is the proper one,
		 * but since value can only be 0 or 2, it is sufficient to only check <= 2
		 * return (this->subtype == AIR_HELICOPTER) || (this->subtype == AIR_AIRCRAFT); */
		return this->subtype <= AIR_AIRCRAFT;
	}

	/**
	 * Get the range of this aircraft.
	 * @return Range in tiles or 0 if unlimited range.
	 */
	uint16_t GetRange() const
	{
		return this->acache.cached_max_range;
	}
};

void GetRotorImage(const Aircraft *v, EngineImageType image_type, VehicleSpriteSeq *result);

Station *GetTargetAirportIfValid(const Aircraft *v);

/* The modular airports (aircraft_modular.cpp). */
bool IsModularAirportStation(const Station *st);
bool CanAircraftUseModularAirport(EngineID engine, const Station *st);
bool AircraftStartsByRocket(EngineID engine);
StringID GetRocketAircraftCannotUseReason(EngineID engine, const Station *st);
bool ModularAirportHasHangar(const Station *st);
TileIndex GetModularAirportHangar(const Station *st);
bool ModularAircraftController(Aircraft *v);
void ModularAircraftBought(Aircraft *v, TileIndex hangar);
void ModularAircraftReleaseAll(Aircraft *v);
void AfterLoadModularAircraft();
bool ModularAirportTileInUse(TileIndex tile);

void HandleMissingAircraftOrders(Aircraft *v);

#endif /* AIRCRAFT_H */
