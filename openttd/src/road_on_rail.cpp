/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file road_on_rail.cpp Road vehicles riding on rail wagons; see road_on_rail.h. */

#include "stdafx.h"
#include "road_on_rail.h"
#include "roadveh.h"
#include "train.h"
#include "cargotype.h"
#include "cargopacket.h"
#include "station_base.h"
#include "station_map.h"
#include "roadstop_base.h"
#include "order_func.h"
#include "vehicle_func.h"
#include "window_func.h"
#include "console_func.h"
#include "anomaly_log.h"
#include "timer/timer_game_tick.h"

#include <map>

#include "safeguards.h"

/**
 * How far above the wagon's own position the road vehicle is drawn: the deck
 * of a flat wagon. The same for every wagon, since no wagon says how high its
 * deck is.
 */
static const int CARRIED_Z_OFFSET = 3;

/**
 * Say something about a road vehicle on the console, but only when it is news
 * -- the same discipline as SayOnChange() for trains: a vehicle that cannot
 * board goes on not being able to, every tick, for as long as it waits.
 * @param rv   the vehicle it is about
 * @param what the whole line
 */
static void SayRoad(const RoadVehicle *rv, std::string what)
{
	if (!_show_train_orientation) return;
	static std::map<VehicleID, std::string> last_said;
	std::string &prev = last_said[rv->index];
	if (prev == what) return;
	prev = std::move(what);
	IConsolePrint(CC_INFO, "{}", prev);
}

/**
 * Put the road vehicle where its wagon is. Called every tick while it rides,
 * so that it moves with the train and is drawn on it.
 * @param rv    the road vehicle
 * @param wagon the wagon it rides on
 */
static void FollowWagon(RoadVehicle *rv, const Train *wagon)
{
	rv->tile = wagon->tile;
	rv->x_pos = wagon->x_pos;
	rv->y_pos = wagon->y_pos;
	rv->z_pos = wagon->z_pos + CARRIED_Z_OFFSET;
	rv->direction = wagon->direction;
	/* In a tunnel with the wagon, out of sight with it. */
	rv->vehstatus.Set(VehState::Hidden, wagon->vehstatus.Test(VehState::Hidden));
	rv->UpdatePosition();
	rv->UpdateViewport(true, true);
}

/**
 * Say in the wagon's cargo that a road vehicle is riding on it: one unit of
 * CT_ROLA, from the station it got on at. The link (Train::carrying) is the
 * truth; the cargo is its mirror, so that the wagon is full to everything that
 * asks a wagon whether it is full -- the details window, the shunting rules
 * that move full and empty wagons apart, a set's own sprites for a loaded
 * wagon -- without each of them having to know about the link. No station
 * ever loads or unloads it (see LoadUnloadVehicle()), so it never earns and is
 * never delivered; it goes when the vehicle gets off (ClearRideMirror()).
 * @param wagon   the wagon being ridden
 * @param station the station the vehicle got on at
 */
static void MirrorRideAsCargo(Train *wagon, StationID station)
{
	if (wagon->cargo_type != _road_vehicle_cargo || wagon->cargo.TotalCount() != 0) return;
	if (!CargoPacket::CanAllocateItem()) return;
	wagon->cargo.Append(CargoPacket::Create(station, 1, Source{}));
}

/**
 * The road vehicle is off the wagon, or gone: the wagon is empty again.
 * @param wagon the wagon that was ridden
 */
static void ClearRideMirror(Train *wagon)
{
	if (wagon->cargo_type != _road_vehicle_cargo) return;
	wagon->cargo.Truncate();
}

/**
 * Is this road vehicle standing at its boarding stop, waiting for a train?
 * @param rv the road vehicle, front of its chain
 * @return whether it is there and waiting
 */
bool IsWaitingToBoardTrain(const RoadVehicle *rv)
{
	if (rv->IsCarried()) return false;
	if (!rv->current_order.IsType(OT_LOADING) || !rv->current_order.ShouldBoardAtStation()) return false;
	const Order *real = rv->GetOrder(rv->cur_real_order_index);
	return real != nullptr && real->IsType(OT_GOTO_STATION) && real->GetDestination().ToStationID() == rv->last_station_visited;
}

/**
 * Find a train standing at the station with a wagon to spare, going where the
 * road vehicle wants to go.
 *
 * "Standing at the station" is the same test a collector uses to know its
 * partner has arrived (IsConsistStandingAtStation()): some part of the train
 * on the station's own tiles and the train at a stand. A stopped train is the
 * player's and is left alone, as it is for coupling. "Going where the road
 * vehicle wants to go" is read off the train's order list: any station order
 * for that station will do, so a train on a loop is as good as one on a
 * shuttle.
 *
 * "A wagon to spare" is one wagon, one road vehicle: a wagon that is not an
 * engine, carries nothing and is not already carrying, whatever its length or
 * the vehicle's. Lengths differ every which way -- wagons, cars, lorries --
 * and the player's answer was to start with the rule that cannot be wrong and
 * refine it if it ever matters.
 *
 * @param rv        the road vehicle at the stop
 * @param station   the station it is at
 * @param next      the station its next order names, or invalid for "anywhere"
 * @param[out] wagon the wagon it would ride on
 * @return the train, or nullptr if none is standing there with room
 */
static Train *FindTrainToBoard(const RoadVehicle *rv, StationID station, StationID next, bool any_chain, Train **wagon, std::string &why)
{
	why = any_chain ? "u nastupiste nestoji zadny vagon" : "u nastupiste nestoji zadny vlak";
	for (Train *t : Train::Iterate()) {
		if (t->owner != rv->owner) continue;
		/* By train: a train, running, that the player has not parked. Onto
		 * wagons: whatever chain stands here -- a headless rake, a shunter
		 * with nowhere to go, a train the player has stopped to marshal --
		 * because standing here is the whole of what is asked of it. */
		if (any_chain) {
			if (!t->IsFrontEngine() && !t->IsFreeWagon()) continue;
			if (t->cur_speed != 0 || t->IsWrecked() || t->vehstatus.Test(VehState::Crashed)) continue;
		} else {
			if (!t->IsFrontEngine()) continue;
			if (t->cur_speed != 0 || t->IsWrecked() || t->vehstatus.Any({VehState::Crashed, VehState::Stopped})) continue;
		}
		if (!IsConsistStandingAtStation(t, station)) continue;

		if (!any_chain && next != StationID::Invalid()) {
			bool goes = false;
			for (const Order &o : t->Orders()) {
				if (o.IsType(OT_GOTO_STATION) && o.GetDestination().ToStationID() == next) {
					goes = true;
					break;
				}
			}
			if (!goes) {
				why = fmt::format("vlak {} stoji, ale nejede do stanice {}", t->unitnumber, next);
				continue;
			}
		}

		bool any_fitted = false;
		for (Train *u = t; u != nullptr; u = u->Next()) {
			if (u->IsEngine() || u->IsArticulatedPart()) continue;
			/* Only a wagon refitted to road vehicles (CT_ROLA), and an empty
			 * one: one wagon, one vehicle, and the unit of cargo a ridden
			 * wagon holds says it is taken. The rule used to be any empty
			 * wagon, and the player's own finding was that this cannot stand
			 * once boarding stops asking where the wagons are going: a rake
			 * of grain wagons waiting for its collector is not a car carrier. */
			if (u->cargo_type != _road_vehicle_cargo || u->cargo_cap == 0) continue;
			any_fitted = true;
			if (u->carrying != VehicleID::Invalid()) continue;
			if (u->cargo.TotalCount() != 0) continue;
			*wagon = u;
			return t;
		}
		why = any_fitted ?
				fmt::format("vlak {} stoji, ale zadny jeho vagon na auta neni volny", t->unitnumber) :
				fmt::format("vlak {} stoji, ale nema zadny vagon prestaveny na silnicni vozidla", t->unitnumber);
	}
	return nullptr;
}

/**
 * Board a train, if one is standing at the platform with room and going the
 * right way. Asked from the road vehicle's loading stop, once its loading is
 * done (see Vehicle::HandleLoading()).
 *
 * Boarding is leaving the stop the way an ordinary departure leaves it -- the
 * stop's bay given back, the cargo work closed out, the order list moved on
 * to the next order -- except that the vehicle then goes onto the wagon
 * instead of onto the road. From here on its own tick does nothing but keep
 * it on the wagon; see CarriedRoadVehicleTick().
 *
 * @param rv the road vehicle, front of its chain
 * @return whether it boarded
 */
bool TryBoardTrain(RoadVehicle *rv)
{
	/* One wagon, one vehicle: a chain of several parts would need several. */
	if (rv->HasArticulatedPart()) return false;
	if (!rv->current_order.IsType(OT_LOADING)) return false;

	std::vector<StationID> next;
	rv->GetNextStoppingStation(next);
	StationID target = next.empty() ? StationID::Invalid() : next.front();

	/* The other way of boarding: onto any fitted wagon standing here, and
	 * never mind where it is going. The vehicle still gets off where its
	 * next order names, whenever whatever it rides on stands there. */
	bool any_chain = rv->current_order.ShouldLoadOnWagons();

	Train *wagon = nullptr;
	std::string why;
	Train *t = FindTrainToBoard(rv, rv->last_station_visited, target, any_chain, &wagon, why);
	if (t == nullptr) {
		SayRoad(rv, fmt::format("Auto {}: ceka na vlak ve stanici {} (dal do {}) - {}", rv->unitnumber, rv->last_station_visited,
				target == StationID::Invalid() ? -1 : (int)target.base(), why));
		return false;
	}

	/* Off the road stop the way a departing vehicle comes off it. */
	RoadStop *rs = RoadStop::GetByTile(rv->tile, GetRoadStopType(rv->tile));
	rv->LeaveStation();
	rs->Leave(rv);

	/* And on to the next order, the way Vehicle::HandleLoading() moves on
	 * after a departure: the visit is done, whatever comes next is next. The
	 * current order is read afresh from the list so that the ride knows where
	 * it ends -- the order machinery would do this on the next tick, but the
	 * next tick of this vehicle is a ride, not a drive. */
	const Order *implicit = rv->GetOrder(rv->cur_implicit_order_index);
	if (implicit != nullptr && (implicit->IsType(OT_IMPLICIT) || implicit->IsType(OT_GOTO_STATION)) &&
			implicit->GetDestination() == rv->last_station_visited) {
		rv->IncrementImplicitOrderIndex();
	}
	rv->current_order.Free();
	ProcessOrders(rv);

	/* Not on any road now. The wormhole state is the one state the rest of
	 * the road code already knows to leave alone -- nothing asks a vehicle in
	 * it which piece of road it is on. */
	rv->state = RVSB_WORMHOLE;
	rv->frame = 0;
	rv->cur_speed = 0;
	rv->subspeed = 0;
	rv->overtaking = 0;
	rv->path.clear();

	rv->carried_by = wagon->index;
	wagon->carrying = rv->index;
	MirrorRideAsCargo(wagon, rv->last_station_visited);
	FollowWagon(rv, wagon);

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "Auto {}: nalozeno na vlak {} (vagon {}) ve stanici {}, vystoupi ve stanici {} (tik {})",
				rv->unitnumber, t->unitnumber, wagon->index, rv->last_station_visited,
				target == StationID::Invalid() ? -1 : (int)target.base(), TimerGameTick::counter);
	}

	InvalidateWindowData(WindowClass::VehicleView, rv->index);
	InvalidateWindowData(WindowClass::VehicleView, t->index);
	SetWindowDirty(WindowClass::VehicleDetails, t->index);
	return true;
}

/**
 * Get off at a station, onto one of its road stops, if the train is standing
 * at the station the vehicle's order names and a stop has room.
 * @param rv    the road vehicle
 * @param wagon the wagon it rides on
 * @return whether it got off
 */
static bool TryLeaveTrain(RoadVehicle *rv, Train *wagon)
{
	Train *t = wagon->First();
	if (t->cur_speed != 0) return false;
	if (!rv->current_order.IsType(OT_GOTO_STATION)) return false;
	StationID dest = rv->current_order.GetDestination().ToStationID();
	if (!IsConsistStandingAtStation(t, dest)) return false;

	const Station *st = Station::Get(dest);
	for (const RoadStop *rs = st->GetPrimaryRoadStop(rv); rs != nullptr; rs = rs->GetNextRoadStop(rv)) {
		Trackdir into;
		if (IsBayRoadStopTile(rs->xy)) {
			/* Driven into against the way it faces, like every vehicle that
			 * uses it. */
			if (rs->IsEntranceBusy() || !rs->HasFreeBay()) continue;
			into = DiagDirToDiagTrackdir(ReverseDiagDir(GetBayRoadStopDir(rs->xy)));
		} else {
			DiagDirection along = AxisToDiagDir(GetDriveThroughStopAxis(rs->xy));
			const RoadStop::Entry &entry = rs->GetEntry(along);
			if (entry.GetOccupied() + rv->gcache.cached_total_length > entry.GetLength()) continue;
			into = DiagDirToDiagTrackdir(along);
		}

		wagon->carrying = VehicleID::Invalid();
		rv->carried_by = VehicleID::Invalid();
		ClearRideMirror(wagon);
		PlaceRoadVehicleAtStopEntrance(rv, rs->xy, into);

		if (_show_train_orientation) {
			IConsolePrint(CC_INFO, "Auto {}: slozeno z vlaku {} ve stanici {} na ({},{}) (tik {})",
					rv->unitnumber, t->unitnumber, dest, TileX(rs->xy), TileY(rs->xy), TimerGameTick::counter);
		}
		InvalidateWindowData(WindowClass::VehicleView, rv->index);
		InvalidateWindowData(WindowClass::VehicleView, t->index);
		SetWindowDirty(WindowClass::VehicleDetails, t->index);
		return true;
	}
	return false;
}

/**
 * The whole of a carried road vehicle's tick: stay on the wagon, and get off
 * when the train stands where the next order says.
 *
 * A wagon that is no longer there -- sold, wrecked and cleared, or the number
 * handed out again to something else -- leaves the vehicle nowhere: it is on
 * no road and has nothing to ride. It is scrapped, and the record says so,
 * since a vehicle vanishing is exactly what the record is for.
 *
 * @param rv the road vehicle, front of its chain
 * @return whether the vehicle still exists
 */
bool CarriedRoadVehicleTick(RoadVehicle *rv)
{
	Train *wagon = Train::GetIfValid(rv->carried_by);
	if (wagon == nullptr || wagon->carrying != rv->index) {
		LogAnomaly("Auto {}: vagon, na kterem se vezlo, uz neexistuje - auto zaniklo na ({},{})",
				rv->unitnumber, TileX(rv->tile), TileY(rv->tile));
		rv->carried_by = VehicleID::Invalid();
		delete rv;
		return false;
	}

	FollowWagon(rv, wagon);

	/* Asked every tick: a train with nothing to load stands at a platform for
	 * a couple of ticks and is gone, and a look every sixteen ticks rode past
	 * the station every time. Cheap enough -- the walk over the station's
	 * stops only happens once the train is standing at the right station. */
	TryLeaveTrain(rv, wagon);
	return true;
}

/**
 * Does any wagon of this train carry a road vehicle?
 * @param t the train, its head
 * @return whether something is riding on it
 */
bool TrainCarriesRoadVehicle(const Train *t)
{
	for (const Train *u = t; u != nullptr; u = u->Next()) {
		if (u->carrying != VehicleID::Invalid()) return true;
	}
	return false;
}

/**
 * The train has crashed: whatever rode on it is destroyed. A road vehicle on
 * a wagon has no crash of its own to go through -- it is on no road -- so it
 * goes at once, and the record says so.
 * @param t the train, its head
 */
void DestroyCarriedRoadVehicles(Train *t)
{
	for (Train *u = t; u != nullptr; u = u->Next()) {
		if (u->carrying == VehicleID::Invalid()) continue;
		RoadVehicle *rv = RoadVehicle::GetIfValid(u->carrying);
		u->carrying = VehicleID::Invalid();
		ClearRideMirror(u);
		if (rv == nullptr || rv->carried_by != u->index) continue;
		LogAnomaly("Auto {}: znicene pri havarii vlaku {} na ({},{})", rv->unitnumber, t->unitnumber, TileX(u->tile), TileY(u->tile));
		rv->carried_by = VehicleID::Invalid();
		delete rv;
	}
}

/**
 * The wagon is going away: the road vehicle riding on it is left with nothing
 * to ride, and its own tick deals with that (see CarriedRoadVehicleTick()).
 * Only the link from this side is rubbed out here.
 * @param wagon the wagon being deleted
 */
void UnlinkCarriedRoadVehicle(Train *wagon)
{
	RoadVehicle *rv = RoadVehicle::GetIfValid(wagon->carrying);
	if (rv != nullptr && rv->carried_by == wagon->index) rv->carried_by = VehicleID::Invalid();
	wagon->carrying = VehicleID::Invalid();
}

/**
 * The road vehicle is going away: the wagon it rode on is empty again.
 * @param rv the road vehicle being deleted
 */
void UnlinkFromWagon(RoadVehicle *rv)
{
	Train *wagon = Train::GetIfValid(rv->carried_by);
	if (wagon != nullptr && wagon->carrying == rv->index) {
		wagon->carrying = VehicleID::Invalid();
		ClearRideMirror(wagon);
	}
	rv->carried_by = VehicleID::Invalid();
}
