/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file train.h Base for the train class. */

#ifndef TRAIN_H
#define TRAIN_H

#include "core/enum_type.hpp"

#include "newgrf_engine.h"
#include "cargotype.h"
#include "rail.h"
#include "engine_base.h"
#include "rail_map.h"
#include "ground_vehicle.hpp"
#include "timer/timer_game_economy.h"

struct Train;

/** Rail vehicle flags. */
enum class VehicleRailFlag : uint8_t {
	Reversing = 0, ///< Train is slowing down to reverse.
	PoweredWagon = 3, ///< Wagon is powered.
	Flipped = 4, ///< Reverse the visible direction of the vehicle.

	AllowedOnNormalRail = 6, ///< Electric train engine is allowed to run on normal rail. */
	Reversed = 7, ///< Used for vehicle var 0xFE bit 8 (toggled each time the train is reversed, accurate for first vehicle only).
	Stuck = 8, ///< Train can't get a path reservation.
	LeavingStation = 9, ///< Train is just leaving a station.
};
/** Bitset of the %VehicleRailFlag elements. */
using VehicleRailFlags = EnumBitSet<VehicleRailFlag, uint16_t>;

/** Modes for ignoring signals. */
enum TrainForceProceeding : uint8_t {
	TFP_NONE   = 0,    ///< Normal operation.
	TFP_STUCK  = 1,    ///< Proceed till next signal, but ignore being stuck till then. This includes force leaving depots.
	TFP_SIGNAL = 2,    ///< Ignore next signal, after the signal ignore being stuck.
};

/** Flags for Train::ConsistChanged */
enum class ConsistChangeFlag : uint8_t {
	Length, ///< Allow vehicles to change length.
	Capacity, ///< Allow vehicles to change capacity.
};
/** Bitset of the %ConsistChangeFlag elements. */
using ConsistChangeFlags = EnumBitSet<ConsistChangeFlag, uint8_t>;

static constexpr ConsistChangeFlags CCF_TRACK{}; ///< Valid changes while vehicle is driving, and possibly changing tracks.
static constexpr ConsistChangeFlags CCF_LOADUNLOAD{}; ///< Valid changes while vehicle is loading/unloading.
static constexpr ConsistChangeFlags CCF_AUTOREFIT{ConsistChangeFlag::Capacity}; ///< Valid changes for autorefitting in stations.
static constexpr ConsistChangeFlags CCF_REFIT{ConsistChangeFlag::Length, ConsistChangeFlag::Capacity}; ///< Valid changes for refitting in a depot.
static constexpr ConsistChangeFlags CCF_ARRANGE{ConsistChangeFlag::Length, ConsistChangeFlag::Capacity}; ///< Valid changes for arranging the consist in a depot.
static constexpr ConsistChangeFlags CCF_SAVELOAD{ConsistChangeFlag::Length}; ///< Valid changes when loading a savegame. (Everything that is not stored in the save.)

uint8_t FreightWagonMult(CargoType cargo);

void CheckTrainsLengths();

void FreeTrainTrackReservation(const Train *v);
bool TryPathReserve(Train *v, bool mark_as_stuck = false, bool first_tile_okay = false);

int GetTrainStopLocation(StationID station_id, TileIndex tile, const Train *moving_front, int *station_ahead, int *station_length);

void GetTrainSpriteSize(EngineID engine, uint &width, uint &height, int &xoffs, int &yoffs, EngineImageType image_type);

bool TrainOnCrossing(TileIndex tile);
void NormalizeTrainVehInDepot(const Train *u);

Train *GetTrainCouplePartner(const Train *v, bool *partner_is_behind = nullptr);
bool TrainAwaitsRescue(Train *v);
bool IsWholeTrainInsideDepot(const Train *v);
bool HasCoupleTarget(const Train *v);
bool IsWaitingToBeCoupled(const Train *v);
bool IsRakeClaimedForCoupling(const Train *rake);
void MarkCoupleClaimChanged(const Train *rake);
void AdoptWagonRakeOrder(Train *rake, VehicleOrderID index);
bool IsWaitingToBeRescued(const Train *v);
bool IsOnRescueRun(const Train *v);
bool IsFetchingCasualty(const Train *v);
bool HandleRescueEngineInDepot(Train *tow);
void EndRescueErrand(Train *tow);
bool IsCouplePartnerOnPlatform(const Train *v, TileIndex tile);
bool IsRescueTargetOnTile(const Train *v, TileIndex tile);
bool TryDecoupleAtStation(Train *v, uint8_t keep_count, OrderLoadType load_type, OrderUnloadType unload_type, uint16_t hold_ticks);

/** Variables that are cached to improve performance and such */
struct TrainCache {
	/** Cached wagon override spritegroup. */
	const struct SpriteGroup *cached_override = nullptr;

	/* cached values, recalculated on load and each time a vehicle is added to/removed from the consist. */
	bool cached_tilt = false; ///< train can tilt; feature provides a bonus in curves
	uint8_t user_def_data = 0; ///< Cached property 0x25. Can be set by Callback 0x36.

	int16_t cached_curve_speed_mod = 0; ///< curve speed modifier of the entire train
	uint16_t cached_max_curve_speed = 0; ///< max consist speed limited by curves

	/**
	 * Compare variables with another instance of this class.
	 * @param other The other instance of TrainCache.
	 * @return The std::strong_ordering of the comparison.
	 */
	auto operator<=>(const TrainCache &other) const = default;
};

/**
 * Why a rescue engine standing on call has not been sent to anything.
 *
 * An engine that never leaves is otherwise a closed box, and "it just sits
 * there" is all anyone can report about it. Written down by the code that
 * decides, read back by the window.
 */
enum class RescueHold : uint8_t {
	None,          ///< Nothing holding it; it is out or about to be.
	Braked,        ///< Standing with its brake on, so it is parked rather than waiting.
	HasOrders,     ///< Has orders of its own, which a rescue engine cannot have.
	NobodyWaiting, ///< Nothing anywhere is broken down or wrecked.
	NotEligible,   ///< Something is, but it does not count as waiting to be fetched.
	AllTaken,      ///< Something is waiting, but another engine is already going for it.
	ExitBlocked,   ///< Called out, but the block outside the depot is occupied.
	NoPath,        ///< Called out, but no route to the casualty can be reserved.
	NoDepot,       ///< Has the casualty in tow, but no depot it can reach to put it down in.
};

/**
 * 'Train' is either a loco or a wagon.
 */
struct Train final : public GroundVehicle<Train, VehicleType::Train> {
	VehicleRailFlags flags{}; ///< Which flags has this train currently set. @see VehicleRailFlag for more details.
	uint16_t crash_anim_pos = 0; ///< Crash animation counter.
	uint16_t wait_counter = 0; ///< Ticks waiting in front of a signal, ticks being stuck or a counter for forced proceeding through signals.

	TrainCache tcache{}; ///< Set of cached variables, recalculated on load and each time a vehicle is added to/removed from the consist.

	/** Link between the two ends of a multiheaded engine. */
	Train *other_multiheaded_part = nullptr;

	RailTypes compatible_railtypes{}; ///< With which rail types the train is compatible.
	RailTypes railtypes{}; ///< On which rail types the train can run.

	TrackBits track{}; ///< On which track the train currently is.
	TrainForceProceeding force_proceed{}; ///< How the train should behave when it encounters next obstacle.

	/* Rescue towing. See FEATURE_DESIGN_COUPLING_TOW.md. Only ever set on the
	 * head of a consist; the first two on a rescue engine, the last on the
	 * casualty it is being sent to. */
	TileIndex rescue_home_depot = INVALID_TILE; ///< Depot a rescue engine is stationed at and returns to when it is done.
	VehicleID rescue_target = VehicleID::Invalid(); ///< Casualty a rescue engine has been sent to fetch, so no two are sent to the same one.

	RescueHold rescue_hold = RescueHold::None; ///< NOSAVE: why an engine on call has not been sent anywhere, so the window can say so.
	TimerGameEconomy::Date rescue_deadline{}; ///< When a casualty gives up waiting to be fetched and sorts itself out the vanilla way. Unset while nothing is wrong.

	/**
	 * How many wagons this train keeps when it finishes the decoupling its
	 * depot order asked for, carried from the moment of arrival to the moment
	 * the work is safe to do -- **plus one**. Zero means no decoupling is owed.
	 *
	 * The plus one is because keeping no wagons at all is a perfectly ordinary
	 * order (the engine drops the lot and goes on alone), so a plain count
	 * could not tell "keep none" apart from "nothing to do".
	 *
	 * Arriving at the ordered depot concludes the order on the spot
	 * (VehicleEnterDepot() wipes it to a dummy), but taking a train apart is
	 * consist surgery and may only happen at the point in the tick where
	 * nothing is walking along the consist -- the same reason the rescue
	 * errand is handled there. So the count is written down at arrival and
	 * honoured from TrainLocoHandler(). Saved, so a game written between the
	 * two moments still owes the split after loading.
	 */
	uint8_t depot_decouple_pending = 0;

	/**
	 * The rake this train has just left standing in the shed it is in.
	 *
	 * A depot order may put wagons down and take wagons on, in that order, and
	 * what was put down must not be what gets taken back on -- otherwise the
	 * train drops its rake and picks the same one up again on the next tick,
	 * over and over, and the order never ends. The name is written down at the
	 * decoupling and holds only while this train is still standing in that
	 * shed working that order; once it has driven out, the rake is an ordinary
	 * stored one and this train may be sent back for it like anybody else.
	 *
	 * Saved, because the wait for a suitable rake to collect can be long: a
	 * game written while the train stands in the shed between the two halves
	 * of its order has to remember which rake is its own leavings.
	 */
	VehicleID depot_dropped_rake = VehicleID::Invalid();

	/**
	 * Which engine has spoken for this rake of wagons, set on the rake itself.
	 *
	 * An engine sent to collect wagons has nowhere to stop once it has set off
	 * -- reaching them is the whole route -- and nowhere else to go if it finds
	 * them gone, so two engines sent to the same rake means one of them with no
	 * errand and no way to end it. The first to want a rake takes it, and the
	 * rake is no longer offered to anyone else until that engine has it or has
	 * given up. See FEATURE_DESIGN_COUPLING_TOW.md.
	 */
	VehicleID couple_claim = VehicleID::Invalid();

	/**
	 * Which rake this engine has spoken for, set on the engine itself.
	 *
	 * The other half of the same fact, written down twice on purpose. The rake
	 * carries the name of the engine so that no second engine takes it; the
	 * engine carries the name of the rake so that it knows, without going
	 * looking, that it already has one and which one.
	 *
	 * That second half is what stops it setting off before the choice is made.
	 * Reserving track first and choosing afterwards meant the track was held
	 * against everybody else while nothing was decided, and then the engine
	 * went to whatever it had reserved rather than to what it had chosen. So:
	 * choose first, then reserve, then move.
	 */
	VehicleID couple_target = VehicleID::Invalid();

	/** Create new Train object. @copydoc GroundVehicle::GroundVehicle */
	Train(VehicleID index) : GroundVehicleBase(index) {}
	/** We want to 'destruct' the right class. */
	~Train() override { this->PreDestructor(); }

	friend struct GroundVehicle<Train, VehicleType::Train>; // GroundVehicle needs to use the acceleration functions defined at Train.

	void MarkDirty() override;
	void UpdateDeltaXY() override;
	ExpensesType GetExpenseType(bool income) const override { return income ? ExpensesType::TrainRevenue : ExpensesType::TrainRun; }
	void PlayLeaveStationSound(bool force = false) const override;
	bool IsPrimaryVehicle() const override { return this->IsFrontEngine(); }
	void GetImage(Direction direction, EngineImageType image_type, VehicleSpriteSeq *result) const override;
	int GetDisplaySpeed() const override { return this->gcache.last_speed; }
	int GetDisplayMaxSpeed() const override { return this->vcache.cached_max_speed; }
	Money GetRunningCost() const override;
	int GetCursorImageOffset() const;
	int GetDisplayImageWidth(Point *offset = nullptr) const;
	bool IsInDepot() const override { return this->track == Track::Depot; }
	bool Tick() override;
	void OnNewCalendarDay() override;
	void OnNewEconomyDay() override;
	uint Crash(bool flooded = false) override;
	Trackdir GetVehicleTrackdir() const override;
	TileIndex GetOrderStationLocation(StationID station) override;
	ClosestDepot FindClosestDepot() override;

	void ReserveTrackUnderConsist() const;

	uint16_t GetCurveSpeedLimit() const;

	void ConsistChanged(ConsistChangeFlags allowed_changes);

	int UpdateSpeed();

	void UpdateAcceleration();

	int GetCurrentMaxSpeed() const override;

	/**
	 * Get the next real (non-articulated part and non rear part of dualheaded engine) vehicle in the consist.
	 * @return Next vehicle in the consist.
	 */
	inline Train *GetNextUnit() const
	{
		Train *v = this->GetNextVehicle();
		if (v != nullptr && v->IsRearDualheaded()) v = v->GetNextVehicle();

		return v;
	}

	/**
	 * Get the previous real (non-articulated part and non rear part of dualheaded engine) vehicle in the consist.
	 * @return Previous vehicle in the consist.
	 */
	inline Train *GetPrevUnit()
	{
		Train *v = this->GetPrevVehicle();
		if (v != nullptr && v->IsRearDualheaded()) v = v->GetPrevVehicle();

		return v;
	}

	/**
	 * Calculate the offset from this vehicle's center to the following center taking the vehicle lengths into account.
	 * @return Offset from center to center.
	 */
	int CalcNextVehicleOffset() const
	{
		/* For vehicles with odd lengths the part before the center will be one unit
		 * longer than the part after the center. This means we have to round up the
		 * length of the next vehicle but may not round the length of the current
		 * vehicle. */
		uint8_t rounding = this->IsDrivingBackwards() ? 1 : 0;
		return (this->gcache.cached_veh_length + rounding) / 2 + (this->GetMovingNext() != nullptr ? this->GetMovingNext()->gcache.cached_veh_length + 1 - rounding : 0) / 2;
	}

	/**
	 * Allows to know the acceleration type of a vehicle.
	 * @return Acceleration type of the vehicle.
	 */
	inline VehicleAccelerationModel GetAccelerationType() const
	{
		return GetRailTypeInfo(GetRailType(this->tile))->acceleration_type;
	}

protected: // These functions should not be called outside acceleration code.

	/**
	 * Allows to know the power value that this vehicle will use.
	 * @return Power value from the engine in HP, or zero if the vehicle is not powered.
	 */
	inline uint16_t GetPower() const
	{
		/* Power is not added for articulated parts */
		if (!this->IsArticulatedPart() && HasPowerOnRail(this->railtypes, GetRailType(this->tile))) {
			uint16_t power = GetVehicleProperty(this, PROP_TRAIN_POWER, RailVehInfo(this->engine_type)->power);
			/* Halve power for multiheaded parts */
			if (this->IsMultiheaded()) power /= 2;
			return power;
		}

		return 0;
	}

	/**
	 * Returns a value if this articulated part is powered.
	 * @return Power value from the articulated part in HP, or zero if it is not powered.
	 */
	inline uint16_t GetPoweredPartPower() const
	{
		/* For powered wagons the engine defines the type of engine (i.e. railtype) */
		if (this->flags.Test(VehicleRailFlag::PoweredWagon) && HasPowerOnRail(this->railtypes, GetRailType(this->tile))) {
			return RailVehInfo(this->gcache.first_engine)->pow_wag_power;
		}

		return 0;
	}

	/**
	 * Allows to know the weight value that this vehicle will use.
	 * @return Weight value from the engine in tonnes.
	 */
	inline uint16_t GetWeight() const
	{
		uint16_t weight = CargoSpec::Get(this->cargo_type)->WeightOfNUnitsInTrain(this->cargo.StoredCount());

		/* Vehicle weight is not added for articulated parts. */
		if (!this->IsArticulatedPart()) {
			weight += GetVehicleProperty(this, PROP_TRAIN_WEIGHT, RailVehInfo(this->engine_type)->weight);
		}

		/* Powered wagons have extra weight added. */
		if (this->flags.Test(VehicleRailFlag::PoweredWagon)) {
			weight += RailVehInfo(this->gcache.first_engine)->pow_wag_weight;
		}

		return weight;
	}

	/**
	 * Calculates the weight value that this vehicle will have when fully loaded with its current cargo.
	 * @return Weight value in tonnes.
	 */
	uint16_t GetMaxWeight() const override;

	/**
	 * Allows to know the tractive effort value that this vehicle will use.
	 * @return Tractive effort value from the engine.
	 */
	inline uint8_t GetTractiveEffort() const
	{
		return GetVehicleProperty(this, PROP_TRAIN_TRACTIVE_EFFORT, RailVehInfo(this->engine_type)->tractive_effort);
	}

	/**
	 * Gets the area used for calculating air drag.
	 * @return Area of the engine in m^2.
	 */
	inline uint8_t GetAirDragArea() const
	{
		/* Air drag is higher in tunnels due to the limited cross-section. */
		return (this->track == Track::Wormhole && this->vehstatus.Test(VehState::Hidden)) ? 28 : 14;
	}

	/**
	 * Gets the air drag coefficient of this vehicle.
	 * @return Air drag value from the engine.
	 */
	inline uint8_t GetAirDrag() const
	{
		return RailVehInfo(this->engine_type)->air_drag;
	}

	/**
	 * Checks the current acceleration status of this vehicle.
	 * @return Acceleration status.
	 */
	inline AccelStatus GetAccelerationStatus() const
	{
		return this->vehstatus.Test(VehState::Stopped) || this->flags.Any({VehicleRailFlag::Reversing, VehicleRailFlag::Stuck}) ? AS_BRAKE : AS_ACCEL;
	}

	/**
	 * Calculates the current speed of this vehicle.
	 * @return Current speed in km/h-ish.
	 */
	inline uint16_t GetCurrentSpeed() const
	{
		return this->cur_speed;
	}

	/**
	 * Returns the rolling friction coefficient of this vehicle.
	 * @return Rolling friction coefficient in [1e-4].
	 */
	inline uint32_t GetRollingFriction() const
	{
		/* Rolling friction for steel on steel is between 0.1% and 0.2%.
		 * The friction coefficient increases with speed in a way that
		 * it doubles at 512 km/h, triples at 1024 km/h and so on. */
		return 15 * (512 + this->GetCurrentSpeed()) / 512;
	}

	/**
	 * Returns the slope steepness used by this vehicle.
	 * @return Slope steepness used by the vehicle.
	 */
	inline uint32_t GetSlopeSteepness() const
	{
		return _settings_game.vehicle.train_slope_steepness;
	}

	/**
	 * Gets the maximum speed allowed by the track for this vehicle.
	 * @return Maximum speed allowed.
	 */
	inline uint16_t GetMaxTrackSpeed() const
	{
		return GetRailTypeInfo(GetRailType(this->tile))->max_speed;
	}

	/**
	 * Returns the curve speed modifier of this vehicle.
	 * @return Current curve speed modifier, in fixed-point binary representation with 8 fractional bits.
	 */
	inline int16_t GetCurveSpeedModifier() const
	{
		return GetVehicleProperty(this, PROP_TRAIN_CURVE_SPEED_MOD, RailVehInfo(this->engine_type)->curve_speed_mod, true);
	}

	/**
	 * Checks if the vehicle is at a tile that can be sloped.
	 * @return True if the tile can be sloped.
	 */
	inline bool TileMayHaveSlopedTrack() const
	{
		/* Any track that isn't TRACK_BIT_X or TRACK_BIT_Y cannot be sloped. */
		return this->track == Track::X || this->track == Track::Y;
	}

	/**
	 * Trains can always use the faster algorithm because they
	 * have always the same direction as the track under them.
	 * @return false
	 */
	inline bool HasToUseGetSlopePixelZ()
	{
		return false;
	}
};

/**
 * Is this a headless rake of wagons standing out on the network -- one left
 * behind by a decoupling train, waiting for an engine to come and collect it?
 *
 * Vanilla only ever has engineless wagons inside a depot, where they are
 * inert: they cannot be clicked on the map, they load nothing and they go
 * nowhere. Wagons left on a platform are none of those things. They stand at a
 * station and take on cargo, and the player has to be able to see what they
 * are doing and tell them to stop. So they count as something the player deals
 * with directly, in the few places that decide whether a vehicle can be looked
 * at and started or stopped -- and nowhere else, because in every other
 * respect they are still not a train.
 *
 * @param v The vehicle to test; may be any part, the question is about the
 *          consist it belongs to.
 * @return Whether this is such a rake.
 */
/**
 * Whether the button that turns a train round is left usable while the train is
 * half in a depot.
 *
 * A testing switch, not a setting: the console command "depo123" flips it. It
 * exists because turning a train round on the depot doorstep is what freezes
 * one, and the freeze has to stay reachable to be worked on.
 */
extern bool _allow_reverse_on_depot_doorstep;

/**
 * Show a train's orientation in its status line.
 *
 * A testing switch, not a setting: the console command "vlak123" flips it. The two
 * things a train is described by cannot be read off the screen at all -- which end of
 * the list goes first, and which way the head vehicle is facing -- and nearly every
 * fault in coupling has been one of them disagreeing with the other. Off by default.
 */
extern bool _show_train_orientation;

bool IsHoldingShortOfStationWaypoint(const Train *v);

/**
 * Would asking this train to turn round do nothing, because of where it is
 * standing relative to a depot?
 *
 * Out on the line turning a train round is always fine: it changes which end
 * leads and nothing moves. Inside a depot it is fine too, but only while the
 * train is standing still -- there the whole train is on one tile with no
 * extent, so both which end leads and the order of the vehicles can be turned
 * at once. Neither of those holds anywhere in between:
 *
 * - **across the doorway**, part of the train inside and part of it out, the
 *   two halves live under different rules and turning it tears the consist
 *   apart;
 * - **inside but started**, on its way out, the train is already a train lying
 *   along the track even while every vehicle is still hidden on the depot tile,
 *   and turning it then leaves the ones still inside on the wrong side of the
 *   ones already out.
 *
 * ReverseTrainDirection() refuses in exactly these cases and returns without
 * doing anything, which from the player's side is a button that does nothing at
 * all -- so they press it again, and again. This is what greys it out instead,
 * and it is the same question the command itself asks, so the two cannot drift
 * apart. It can be turned back on from the console; see
 * #_allow_reverse_on_depot_doorstep.
 *
 * @param v The train; may be any part, the question is about the whole consist.
 * @return Whether a request to turn it round here would be refused.
 */
inline bool IsTrainReverseBlockedByDepot(const Vehicle *v)
{
	if (v->type != VehicleType::Train) return false;

	bool any_in = false;
	bool all_in = true;
	for (const Vehicle *u = v->First(); u != nullptr; u = u->Next()) {
		if (Train::From(u)->track == Track::Depot) {
			any_in = true;
		} else {
			all_in = false;
		}
	}

	if (!any_in) return false;
	return !(all_in && v->First()->vehstatus.Test(VehState::Stopped));
}

inline bool IsWaitingWagonChain(const Vehicle *v)
{
	if (v->type != VehicleType::Train) return false;
	const Train *head = Train::From(v)->First();
	return head->IsFreeWagon() && !head->IsInDepot();
}

#endif /* TRAIN_H */
