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
 * deck is -- so it is a number chosen by eye against the wagons this is
 * actually used with, and 'testpaluba' moves it while the game runs, because
 * the eye that has to choose it is at the screen and not here.
 */
int _carried_z_offset = 4;

/**
 * How far behind the front of its drawn box a road vehicle's own position
 * sits. A rail vehicle is drawn about its middle and a road vehicle from its
 * front -- "Unlike trains, road vehicles do not have their offsets moved to
 * the centre", as RoadVehicle::UpdateDeltaXY() puts it -- so putting a road
 * vehicle at a wagon's position does not put it on the wagon. Both numbers
 * are read off the two UpdateDeltaXY().
 */
static const int ROAD_VEHICLE_NOSE = 3;

/**
 * How long a wagon is, counting the pieces a set builds one wagon out of: a
 * long wagon is a short visible head with invisible articulated pieces behind
 * it (CZTR's freight wagons are 3 + 8 + 3 long), and it is the whole of them
 * that a road vehicle stands on.
 * @param wagon the wagon, its head
 * @return the length, in the eighths of a tile that lengths are measured in
 */
static uint WagonUnitLength(const Train *wagon)
{
	uint length = 0;
	for (const Train *p = wagon; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) {
		length += p->gcache.cached_veh_length;
	}
	return length;
}

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
 * Put the road vehicle on its wagon, nose to the wagon's nose. Called every
 * tick while it rides, so that it moves with the train and is drawn on it.
 *
 * Where the vehicle goes is not simply where the wagon is, for two reasons.
 * A long wagon is several pieces and the one the vehicle rides on -- the head,
 * the only piece a train's chain names -- can be a short stub at either end of
 * it, so its position is not the wagon's middle. And the two kinds of vehicle
 * are drawn from different places: a wagon about its own position, a road
 * vehicle from its front. Put together, a vehicle put where the wagon's head
 * is starts at one end of the wagon and hangs over onto the next one, which is
 * what the player saw. So the whole wagon's ends are worked out from its
 * pieces, and the vehicle is laid out from the front one -- its trailer, if it
 * has one, behind it.
 *
 * It stands in the middle of the wagon, and that is a decision rather than an
 * accident. Nose to the leading end is where a lorry driven onto a flat wagon
 * really does come to rest, and it is what this did at first, but it only
 * looked right while the train ran straight: the four directions a train takes
 * on a curve went through the other branch below and came out in the middle
 * instead, so a lorry slid along its wagon every time the train went round a
 * bend. And the end of a wagon is the worst place to put the join, because
 * every disagreement about where a picture sits around its vehicle -- between
 * a wagon and a lorry, between a set and the game's own graphics, between one
 * heading and the next in the same set -- shows there with nothing beside it
 * to hide it. In the middle the same error is half as far from either end and
 * reads as "not quite centred" rather than "hanging off the end". Exact it
 * cannot be: the deck of every wagon is at its own height and no wagon says
 * where it is. The player asked for the middle after aligning a set by hand,
 * and for the vehicle to ride a little higher.
 *
 * @param rv    the road vehicle, front of its chain
 * @param wagon the wagon it rides on, its head
 */
static void FollowWagon(RoadVehicle *rv, const Train *wagon)
{
	/* Which way the wagon is drawn, not which way it is written down. A wagon
	 * coupled up the other way round has its recorded direction reversed and
	 * its Flipped flag set, which leaves its picture exactly as it was --
	 * nothing moved, so nothing may look different (see
	 * NormaliseCoupledConsistFacing()). A lorry standing on its deck is part of
	 * that picture: read off the recorded direction it spun round on the spot
	 * while the wagon under it did not, and moved to the wagon's other end
	 * besides, because which end is the nose is worked out from this too. The
	 * player saw it at one station and not at the other, which is exactly where
	 * the coupling flips the rake and where it does not. */
	Direction dir = wagon->flags.Test(VehicleRailFlag::Flipped) ? ReverseDir(wagon->direction) : wagon->direction;
	int x = wagon->x_pos;
	int y = wagon->y_pos;

	/* One step backwards along the wagon, for laying a trailer out behind its
	 * lorry and for finding the middle of the two together. On a curve this is
	 * only roughly the way the vehicle points, which is as much as a moving
	 * train needs. */
	static const DiagDirectionIndexArray<Point> _step_back{{{
		{  1,  0 }, // DiagDirection::NE, which faces -x
		{  0, -1 }, // DiagDirection::SE, which faces +y
		{ -1,  0 }, // DiagDirection::SW, which faces +x
		{  0,  1 }, // DiagDirection::NW, which faces -y
	}}};
	const Point &back = _step_back[DirToDiagDir(dir)];

	/* A lorry and its trailer are put on as one, so the whole chain's length is
	 * what has to end up in the middle.
	 *
	 * How far in front of the wagon's middle the first vehicle stands follows
	 * from that: half the chain reaches forward from its middle, and the
	 * vehicle's own position sits that much behind the front of its box. The
	 * wagon's length does not come into it at all -- put the chain's middle on
	 * the wagon's middle and a wagon of any length is right. Both cases below
	 * use this one line, which is what makes a lorry stop jumping along its
	 * wagon when the train goes round a bend. */
	int chain = 0;
	for (const RoadVehicle *u = rv; u != nullptr; u = u->Next()) chain += u->gcache.cached_veh_length;
	const int ahead_of_middle = chain / 2 - (VEHICLE_LENGTH - ROAD_VEHICLE_NOSE);

	if (IsDiagonalDirection(dir)) {
		/* Along the rails: which axis they run on, and which way along it the
		 * wagon faces. */
		bool along_y = (dir == Direction::NW || dir == Direction::SE);
		bool forward = (dir == Direction::SE || dir == Direction::SW);

		/* Both ends of the whole wagon. A rail vehicle is drawn about its own
		 * position, half its length reaching either way. */
		int lo = INT_MAX;
		int hi = INT_MIN;
		for (const Train *p = wagon; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) {
			int pos = along_y ? p->y_pos : p->x_pos;
			int len = p->gcache.cached_veh_length;
			lo = std::min(lo, pos - (forward ? len / 2 : (len + 1) / 2));
			hi = std::max(hi, pos + (forward ? (len + 1) / 2 : len / 2));
		}

		/* The middle of the whole wagon, and the chain set forward from it. */
		int at = (lo + hi) / 2 + (forward ? ahead_of_middle : -ahead_of_middle);
		if (along_y) {
			y = at;
		} else {
			x = at;
		}
	} else {
		/* On a curve the wagon's pieces lie round the bend, so its middle is
		 * taken as the middle of its two ends and the way it faces is only
		 * roughly the step below -- which is as much as a moving train needs. */
		const Train *last = wagon;
		while (last->HasArticulatedPart()) last = last->GetNextArticulatedPart();
		x = (wagon->x_pos + last->x_pos) / 2 - back.x * ahead_of_middle;
		y = (wagon->y_pos + last->y_pos) / 2 - back.y * ahead_of_middle;
	}


	const RoadVehicle *ahead = nullptr;
	for (RoadVehicle *u = rv; u != nullptr; u = u->Next()) {
		if (ahead != nullptr) {
			x += back.x * ahead->gcache.cached_veh_length;
			y += back.y * ahead->gcache.cached_veh_length;
		}
		u->tile = TileVirtXY(x, y);
		u->x_pos = x;
		u->y_pos = y;
		u->z_pos = wagon->z_pos + _carried_z_offset;
		u->direction = dir;
		/* In a tunnel with the wagon, out of sight with it. */
		u->vehstatus.Set(VehState::Hidden, wagon->vehstatus.Test(VehState::Hidden));
		u->UpdatePosition();
		u->UpdateViewport(true, true);
		ahead = u;
	}
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
 * player's and is left alone -- unless any ride at all will do, where a train
 * the player has stopped to marshal is one of the rides meant. "Going where
 * the road vehicle wants to go" is read off the order list: any station order
 * for that station will do, so a train on a loop is as good as one on a
 * shuttle. A rake left at a platform has an order list too -- the one the
 * engine that dropped it wrote, whose last order names what the rake is for
 * (Order::GetDecoupleCargoDest()) -- so the same question is asked of it in
 * the same way.
 *
 * Which of the two kinds is looked at, and whether the question about where it
 * is going is asked at all, is the player's choice of OrderBoardMode.
 *
 * "A wagon to spare" is one wagon, one road vehicle: a wagon that is not an
 * engine, carries nothing, is not already carrying, and is at least as long as
 * the whole vehicle -- a lorry with a trailer is one vehicle and needs a wagon
 * that it fits on lengthwise. That is what decides which of a set's wagons can
 * carry what, without this code having to know any of their names: a short
 * flat wagon takes a car, and only the long ones take a lorry and trailer. A
 * vehicle that finds nothing long enough waits at the stop and says so.
 *
 * @param rv        the road vehicle at the stop
 * @param station   the station it is at
 * @param next      the station its next order names, or invalid for "anywhere"
 * @param mode      which kind of ride it asked for
 * @param[out] wagon the wagon it would ride on
 * @return the train, or nullptr if none is standing there with room
 */
static Train *FindTrainToBoard(const RoadVehicle *rv, StationID station, StationID next, OrderBoardMode mode, Train **wagon, std::string &why)
{
	/* A train -- which a shunter is, and so is a train the player has stopped
	 * -- or a rake standing at the platform by itself. And whether it has to
	 * be going this vehicle's way, or may be going anywhere. */
	bool want_train = (mode == OrderBoardMode::TrainToNext || mode == OrderBoardMode::TrainAnywhere);
	bool only_towards = (mode == OrderBoardMode::TrainToNext || mode == OrderBoardMode::WagonsToNext);

	why = want_train ? "u nastupiste nestoji zadny vlak" : "u nastupiste nestoji zadna rada vagonu";
	for (Train *t : Train::Iterate()) {
		if (t->owner != rv->owner) continue;
		if (want_train) {
			if (!t->IsFrontEngine()) continue;
			if (t->cur_speed != 0 || t->IsWrecked() || t->vehstatus.Test(VehState::Crashed)) continue;
			if (only_towards && t->vehstatus.Test(VehState::Stopped)) continue;
		} else {
			if (!t->IsFreeWagon()) continue;
			if (t->cur_speed != 0 || t->IsWrecked() || t->vehstatus.Test(VehState::Crashed)) continue;
		}
		if (!IsConsistStandingAtStation(t, station)) continue;

		if (only_towards && next != StationID::Invalid()) {
			bool goes = false;
			for (const Order &o : t->Orders()) {
				if (o.IsType(OT_GOTO_STATION) && o.GetDestination().ToStationID() == next) {
					goes = true;
					break;
				}
			}
			if (!goes) {
				why = want_train ?
							fmt::format("vlak {} stoji, ale nejede do stanice {}", t->unitnumber, next) :
							fmt::format("rada u nastupiste stoji, ale neni urcena pro stanici {}", next);
				continue;
			}
		}

		bool any_fitted = false;
		bool any_short = false;
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
			/* Long enough for the whole of it, trailer and all. */
			if (WagonUnitLength(u) < rv->gcache.cached_total_length) {
				any_short = true;
				continue;
			}
			*wagon = u;
			return t;
		}
		if (any_short) {
			why = fmt::format("vlak {} stoji, ale jeho volne vagony jsou na auto dlouhe {} kratke", t->unitnumber, rv->gcache.cached_total_length);
		} else {
			why = any_fitted ?
					fmt::format("vlak {} stoji, ale zadny jeho vagon na auta neni volny", t->unitnumber) :
					fmt::format("vlak {} stoji, ale nema zadny vagon prestaveny na silnicni vozidla", t->unitnumber);
		}
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
	if (!rv->current_order.IsType(OT_LOADING)) return false;

	std::vector<StationID> next;
	rv->GetNextStoppingStation(next);
	StationID target = next.empty() ? StationID::Invalid() : next.front();

	/* Somewhere to get off first. A vehicle whose orders name no further
	 * station has no ride to take: it used to get on anyway and then off
	 * again at the next station the train stood at, which -- for a train
	 * unloading everything where it stood -- was the stop it had just left,
	 * so it hopped on and off in the same place for ever. Standing at the
	 * stop and saying that it is waiting is the honest state; the player
	 * gives it orders, or tells the train to unload everything, and it goes. */
	if (target == StationID::Invalid()) {
		SayRoad(rv, fmt::format("Auto {}: ceka ve stanici {} - nema prikaz, kde vystoupit", rv->unitnumber, rv->last_station_visited));
		return false;
	}

	Train *wagon = nullptr;
	std::string why;
	Train *t = FindTrainToBoard(rv, rv->last_station_visited, target, rv->current_order.GetBoardMode(), &wagon, why);
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

	/* Not on any road now, trailer and all. The wormhole state is the one
	 * state the rest of the road code already knows to leave alone -- nothing
	 * asks a vehicle in it which piece of road it is on. */
	for (RoadVehicle *u = rv; u != nullptr; u = u->Next()) {
		u->state = RVSB_WORMHOLE;
		u->frame = 0;
		u->overtaking = 0;
	}
	rv->cur_speed = 0;
	rv->subspeed = 0;
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
 * Is this train, standing at a station, under orders to put down everything it
 * is carrying? A road vehicle on a wagon is something the train is carrying,
 * so "unload all" and "transfer" mean it too -- that is the player's way of
 * getting a vehicle off a train that is not going where the vehicle wanted,
 * and the only way at all for one that boarded with nowhere to go (one of the
 * "anywhere" ways of boarding, see OrderBoardMode).
 * @param t the train, its head
 * @return whether everything aboard is to be put down here
 */
static bool IsTrainUnloadingEverything(const Train *t)
{
	if (!t->current_order.IsType(OT_LOADING)) return false;
	OrderUnloadType unload = t->current_order.GetUnloadType();
	return unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer;
}

/**
 * Get off at a station, onto one of its road stops, if the train is standing
 * at the station the vehicle's order names -- or wherever it stands, if the
 * train has been told to unload everything -- and a stop has room.
 * @param rv    the road vehicle
 * @param wagon the wagon it rides on
 * @return whether it got off
 */
static bool TryLeaveTrain(RoadVehicle *rv, Train *wagon)
{
	Train *t = wagon->First();
	if (t->cur_speed != 0) return false;

	/* Where it gets off: the station its own next order names, if the train is
	 * standing there. */
	StationID dest = StationID::Invalid();
	if (rv->current_order.IsType(OT_GOTO_STATION)) {
		StationID wanted = rv->current_order.GetDestination().ToStationID();
		if (IsConsistStandingAtStation(t, wanted)) dest = wanted;
	}
	/* Failing that, wherever the train stands if it is putting everything
	 * down. The vehicle carries on from there under its own orders, by road. */
	if (dest == StationID::Invalid() && IsTrainUnloadingEverything(t) &&
			t->last_station_visited != StationID::Invalid() && IsConsistStandingAtStation(t, t->last_station_visited)) {
		dest = t->last_station_visited;
	}
	if (dest == StationID::Invalid()) return false;

	/* Never back down at the stop it got on at. A vehicle put down where it
	 * got on drives round to the stop, gets on again and is put down again,
	 * for ever -- which is what a train told to unload everything did to a
	 * vehicle that had just got on there. Where it got on is written in the
	 * wagon's own cargo (see MirrorRideAsCargo()), so the ride carries it. */
	if (dest == wagon->cargo.GetFirstStation()) return false;

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
