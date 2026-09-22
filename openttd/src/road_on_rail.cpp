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
#include "ship.h"
#include "aircraft.h"
#include "engine_base.h"
#include "engine_func.h"
#include "newgrf_engine.h"
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
#include "landscape.h"
#include "spritecache.h"

#include <map>

#include "safeguards.h"

/**
 * How high the deck of a wagon is: screen pixels above the line the wagon's
 * own wheels stand on.
 *
 * Still chosen by eye, because no wagon says where its deck is -- 'testpaluba'
 * moves it while the game runs, since the eye that has to choose it is at the
 * screen and not here. What changed is what it is measured from. It used to be
 * added to the wagon's height in the world, and the world's height is the same
 * in every direction while the wagon's picture is not, so the deck came out
 * right in one direction and wrong in the other seven -- the ring of eight
 * different best numbers the player read off a circle of track. Measured from
 * the wagon's own picture, one number does all eight.
 */
int _carried_z_offset = 6;

/**
 * How far above the wagon the carried vehicle is lifted in the world.
 *
 * Nothing to do with how high it looks -- that is the deck above, in pixels.
 * This is for the sorting of what is drawn in front of what: two boxes at the
 * same height are sorted by a rule of thumb, and the rule once put the wagon
 * in front of the vehicle it was carrying.
 */
static const int CARRIED_WORLD_LIFT = 4;

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
 * How much of a ship's own capacity one road vehicle takes up, and how much an
 * aircraft has to hold before it carries one at all. Two numbers chosen
 * against the game's own vehicles: at 40 the small ferry takes two cars and the
 * largest tanker eight, and at 60 the smallest aeroplanes are left out of the
 * fitting while everything from the second one up takes its single car.
 */
static const uint SHIP_CAPACITY_PER_ROAD_VEHICLE = 40;
static const uint AIRCRAFT_CAPACITY_FOR_ONE_ROAD_VEHICLE = 60;

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
 * How many road vehicles a vehicle of this engine carries when it is fitted
 * for them (CT_ROLA).
 *
 * A wagon carries one: one wagon, one vehicle, and the wagon's own length then
 * says whether a lorry and trailer fits on it (see FindTrainToBoard()). A ship
 * carries one for every 40 of whatever it otherwise holds, which puts the
 * game's own ships between two (a small ferry) and eight (the largest tanker)
 * and scales with a set's ships without knowing any of their names. An
 * aircraft carries exactly one, and only if it is big enough to be worth it --
 * the player asked for one car per aircraft, and a four-seater is not a car
 * ferry.
 *
 * Whatever comes out as none is never offered the fitting at all
 * (CanCarryRoadVehicles()), so the purchase list never shows a ship that would
 * carry nothing.
 *
 * @param e the engine
 * @param v the vehicle being asked about, or nullptr for the purchase list
 * @return how many road vehicles it carries
 */
uint RoadVehiclesCarriedBy(const Engine *e, const Vehicle *v)
{
	switch (e->type) {
		case VehicleType::Train:
			return (v != nullptr && v->IsArticulatedPart()) ? 0 : 1;

		case VehicleType::Ship:
			return GetEngineProperty(e->index, PROP_SHIP_CARGO_CAPACITY, e->VehInfo<ShipVehicleInfo>().capacity, v) / SHIP_CAPACITY_PER_ROAD_VEHICLE;

		case VehicleType::Aircraft:
			return GetEngineProperty(e->index, PROP_AIRCRAFT_PASSENGER_CAPACITY, e->VehInfo<AircraftVehicleInfo>().passenger_capacity, v) >= AIRCRAFT_CAPACITY_FOR_ONE_ROAD_VEHICLE ? 1 : 0;

		default:
			return 0;
	}
}

/**
 * May a vehicle of this engine be fitted for road vehicles at all?
 * @param e the engine
 * @return whether the fitting is offered for it
 */
bool CanCarryRoadVehicles(const Engine *e)
{
	if (!IsValidCargoType(_road_vehicle_cargo)) return false;
	switch (e->type) {
		case VehicleType::Train:
			return e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Wagon;

		case VehicleType::Ship:
		case VehicleType::Aircraft:
			return RoadVehiclesCarriedBy(e, nullptr) > 0;

		default:
			return false;
	}
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
 * Where a whole vehicle's picture lands on the screen: the middle of it across,
 * and the line its wheels stand on.
 *
 * The wheels, not the middle of the picture. Vehicles are of all heights and
 * the middle of a picture says nothing about where the thing touches the
 * ground -- the player's own words. What a sprite does say is its lowest row,
 * because a sprite is trimmed to what is actually painted, and the lowest
 * painted row of a vehicle is where its wheels meet what it stands on. That
 * line is the same thing on every vehicle in every set, which is what makes it
 * worth measuring from.
 *
 * Across, the best this can do is the middle of the whole picture. The middle
 * between the two wheels would be better still and cannot be had: the pictures
 * are in the cache in the form the screen wants them, not as pixels to be
 * looked through.
 *
 * Asked of the pictures that are about to be drawn, not of the positions. A
 * picture sits against its vehicle's position by whatever its own bounding box
 * says (UpdateDeltaXY(), which answers differently for each of the eight
 * directions and differently again for a road vehicle than for a wagon) plus
 * whatever the set baked into the sprite.
 *
 * @param first     the vehicle, its head
 * @param rail_only whether to stop at the end of this rail vehicle's own
 *                  pieces; a wagon's Next() walks on into the rest of the
 *                  train and would take the whole consist's width, while a
 *                  road vehicle's ends at its own trailer, which is right
 * @param[out] middle_x the middle across, in the viewport's own units
 * @param[out] wheels_y the line the wheels stand on, the same units
 */
static void DrawnPicture(const Vehicle *first, bool rail_only, int &middle_x, int &wheels_y)
{
	int lo = INT_MAX;
	int hi = INT_MIN;
	int bottom = INT_MIN;
	for (const Vehicle *v = first; v != nullptr; ) {
		Point pt = RemapCoords(v->x_pos + v->bounds.origin.x + v->bounds.offset.x,
				v->y_pos + v->bounds.origin.y + v->bounds.offset.y,
				v->z_pos + v->bounds.origin.z + v->bounds.offset.z);
		VehicleSpriteSeq seq;
		v->GetImage(v->direction, EngineImageType::OnMap, &seq);
		Rect r;
		seq.GetBounds(&r);
		lo = std::min(lo, pt.x + r.left);
		hi = std::max(hi, pt.x + r.right);
		bottom = std::max(bottom, pt.y + r.bottom);

		if (rail_only) {
			const Train *t = Train::From(v);
			v = t->HasArticulatedPart() ? t->GetNextArticulatedPart() : nullptr;
		} else {
			v = v->Next();
		}
	}
	middle_x = (lo + hi) / 2;
	wheels_y = bottom;
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
		/* The tile is taken from where the vehicle rides, not from where it is
		 * drawn: a step to the side near the edge of a tile would otherwise say
		 * it is on the tile next door, and what a carried vehicle's tile says
		 * has already taken the game down once. */
		u->tile = TileVirtXY(x, y);
		u->x_pos = x;
		u->y_pos = y;
		u->z_pos = wagon->z_pos + CARRIED_WORLD_LIFT;
		u->direction = dir;
		/* Measured below, so it must not still carry the last tick's answer. */
		u->draw_offs = {};
		u->UpdateDeltaXY();
		/* In a tunnel with the wagon, out of sight with it. */
		u->vehstatus.Set(VehState::Hidden, wagon->vehstatus.Test(VehState::Hidden));
		ahead = u;
	}

	/* And now across the screen, which the world's own grid cannot do.
	 *
	 * The two pictures are put side by side and the vehicle's is slid until
	 * its middle is over the wagon's. It is not the same as standing at the
	 * same place: a road vehicle's picture sits differently against its own
	 * position than a wagon's does, by a different amount in each of the eight
	 * directions, and the set decides how much. Chasing that with the height
	 * is what gave the player eight different best heights round a circle of
	 * track -- the ring of numbers that started this.
	 *
	 * Worked out rather than written down on purpose. Numbers read off one
	 * set's wagons would be wrong for the next set and for the game's own
	 * vehicles; this asks the pictures that are actually about to be drawn.
	 *
	 * Pixels, not steps of the world. Facing east or west, across the rails is
	 * straight up and down the screen, so no move in the world can shift the
	 * picture sideways at all there -- which is why it ends up in draw_offs and
	 * not in the position. */
	int wagon_middle, wagon_wheels;
	int car_middle, car_wheels;
	DrawnPicture(wagon, true, wagon_middle, wagon_wheels);
	DrawnPicture(rv, false, car_middle, car_wheels);
	/* On the deck, which is so many pixels above the line the wagon's own
	 * wheels stand on (_carried_z_offset). Measured from the picture and not
	 * from the wagon's height, and that is the whole point: the height is the
	 * same in every direction while the picture is not, so a deck set against
	 * the height needed a different number in each of the eight directions and
	 * a deck set against the picture needs one. */
	int want_wheels = wagon_wheels - _carried_z_offset * ZOOM_BASE;
	for (RoadVehicle *u = rv; u != nullptr; u = u->Next()) {
		u->draw_offs.x = wagon_middle - car_middle;
		u->draw_offs.y = want_wheels - car_wheels;
		u->UpdatePosition();
		u->UpdateViewport(true, true);
	}
}

/**
 * Put every carried road vehicle back where it belongs on its wagon, at once.
 *
 * For the two probes that move the deck ('testpaluba', 'testkruh'), and it is
 * not a nicety. A vehicle is put on its wagon by its own tick, and a train
 * standing still in a station or a shed ticks nothing -- so changing the deck
 * and watching a standing train showed no change at all, whatever number was
 * given. A standing train is exactly what the player stops to look at.
 */
void RestandCarriedRoadVehicles()
{
	for (RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (!rv->IsFrontEngine() || !rv->IsCarried()) continue;
		Vehicle *carrier = Vehicle::GetIfValid(rv->carried_by);
		if (carrier == nullptr || carrier->type != VehicleType::Train) continue;
		FollowWagon(rv, Train::From(carrier));
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
		/* Nothing gets on what the player has sold. It is waiting for the tow
		 * that will take it to a shed and sell it there, and a car that climbed
		 * aboard in the meantime would be sold with it -- which is what
		 * happened to the player's lorries. Said once for both kinds, because a
		 * whole train and a rake of wagons are sold the same way. */
		if (t->IsSoldForScrap()) continue;
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
 * How many road vehicles are riding on this ship or aircraft.
 *
 * A wagon carries one and says so in a link of its own (Train::carrying),
 * because its picture has to be drawn with the lorry on it every tick. A ship
 * carries several and draws none of them, so it keeps no list: the vehicles
 * themselves say where they are (RoadVehicle::carried_by), and counting them
 * is asked only when one wants to get on, never in a tick.
 *
 * @param carrier the ship or aircraft
 * @return how many are aboard
 */
static uint RoadVehiclesAboard(const Vehicle *carrier)
{
	uint aboard = 0;
	for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (!rv->IsFrontEngine()) continue;
		if (rv->carried_by == carrier->index) aboard++;
	}
	return aboard;
}

/**
 * Say in a ship's or aircraft's cargo that one more road vehicle is riding on
 * it, the way MirrorRideAsCargo() does for a wagon: one unit of CT_ROLA per
 * vehicle, so that the vessel fills up and reads as full to everything that
 * asks, and the player sees how many cars are aboard without any of them being
 * drawn.
 * @param carrier the ship or aircraft
 * @param station the station the vehicle got on at
 */
static void AddRideMirror(Vehicle *carrier, StationID station)
{
	if (carrier->cargo_type != _road_vehicle_cargo) return;
	if (!CargoPacket::CanAllocateItem()) return;
	carrier->cargo.Append(CargoPacket::Create(station, 1, Source{}));
}

/**
 * One road vehicle has got off: one unit of the mirror goes with it.
 * @param carrier the ship or aircraft
 */
static void RemoveRideMirror(Vehicle *carrier)
{
	if (carrier->cargo_type != _road_vehicle_cargo) return;
	carrier->cargo.Truncate(1);
}

/**
 * Is this ship or aircraft standing at the station, loading, with room for one
 * more road vehicle and orders that take it where the vehicle wants to go?
 *
 * "Standing at the station" is its own loading state rather than the walk over
 * the station's tiles a train needs: a ship at a dock and an aircraft at an
 * airport are at the station by being there at all, and the loading state is
 * what says they have not left yet.
 *
 * @param rv      the road vehicle at the stop
 * @param station the station it is at
 * @param next    the station its next order names
 * @param type    ship or aircraft
 * @param[out] why what to say when none is found
 * @return the ship or aircraft, or nullptr
 */
static Vehicle *FindVesselToBoard(const RoadVehicle *rv, StationID station, StationID next, VehicleType type, std::string &why)
{
	why = type == VehicleType::Ship ? "u pristavu nestoji zadna lod" : "na letisti nestoji zadne letadlo";

	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != type || v->owner != rv->owner) continue;
		if (!v->IsPrimaryVehicle()) continue;
		if (v->vehstatus.Test(VehState::Crashed) || v->vehstatus.Test(VehState::Stopped)) continue;
		if (!v->current_order.IsType(OT_LOADING) || v->last_station_visited != station) continue;

		if (v->cargo_type != _road_vehicle_cargo || v->cargo_cap == 0) {
			why = type == VehicleType::Ship ?
					fmt::format("lod {} stoji, ale neni prestavena na auta", v->unitnumber) :
					fmt::format("letadlo {} stoji, ale neni prestavene na auta", v->unitnumber);
			continue;
		}

		bool goes = false;
		for (const Order &o : v->Orders()) {
			if (o.IsType(OT_GOTO_STATION) && o.GetDestination().ToStationID() == next) {
				goes = true;
				break;
			}
		}
		if (!goes) {
			why = type == VehicleType::Ship ?
					fmt::format("lod {} stoji, ale nejede do stanice {}", v->unitnumber, next) :
					fmt::format("letadlo {} stoji, ale nejede do stanice {}", v->unitnumber, next);
			continue;
		}

		if (RoadVehiclesAboard(v) >= v->cargo_cap) {
			why = type == VehicleType::Ship ?
					fmt::format("lod {} stoji, ale je plna aut ({})", v->unitnumber, v->cargo_cap) :
					fmt::format("letadlo {} stoji, ale je plne ({})", v->unitnumber, v->cargo_cap);
			continue;
		}

		return v;
	}
	return nullptr;
}

/**
 * Put a road vehicle riding on a ship or an aircraft where the vessel is, and
 * out of sight.
 *
 * Nothing of it is drawn: the player asked for the cars to disappear into the
 * vessel and be no more than "the ship is full" until they are put down again.
 * It is still put where the vessel is rather than left where it got on, so
 * that everything that asks a vehicle where it is -- the viewport lists, the
 * "follow this vehicle" button, the map -- gets an answer that is not a lie.
 *
 * @param rv      the road vehicle, front of its chain
 * @param carrier the ship or aircraft
 */
static void RideInside(RoadVehicle *rv, const Vehicle *carrier)
{
	for (RoadVehicle *u = rv; u != nullptr; u = u->Next()) {
		u->tile = carrier->tile;
		u->x_pos = carrier->x_pos;
		u->y_pos = carrier->y_pos;
		u->z_pos = carrier->z_pos;
		u->direction = carrier->direction;
		u->vehstatus.Set(VehState::Hidden);
		u->UpdatePosition();
		u->UpdateViewport(true, true);
	}
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

	/* Rails, water or air: which of them was asked for decides what is looked
	 * for at this station. A ship and an aircraft are only ever taken towards
	 * the next stop, so there is no question about where they are going beyond
	 * asking their order lists. */
	OrderBoardMode mode = rv->current_order.GetBoardMode();
	Train *wagon = nullptr;
	std::string why;
	Vehicle *carrier = nullptr;
	if (mode == OrderBoardMode::ShipToNext || mode == OrderBoardMode::PlaneToNext) {
		carrier = FindVesselToBoard(rv, rv->last_station_visited, target,
				mode == OrderBoardMode::ShipToNext ? VehicleType::Ship : VehicleType::Aircraft, why);
	} else {
		carrier = FindTrainToBoard(rv, rv->last_station_visited, target, mode, &wagon, why);
	}
	if (carrier == nullptr) {
		SayRoad(rv, fmt::format("Auto {}: ceka na odvoz ve stanici {} (dal do {}) - {}", rv->unitnumber, rv->last_station_visited,
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

	/* What carries it, which on rails is the wagon and not the train: the link
	 * back (Train::carrying) is the wagon's, and the two have to name each
	 * other or the ride's own tick finds nothing underneath it. */
	rv->carried_by = (wagon != nullptr) ? wagon->index : carrier->index;
	if (wagon != nullptr) {
		/* On a wagon it rides in the open, on the deck, and is drawn there. */
		wagon->carrying = rv->index;
		MirrorRideAsCargo(wagon, rv->last_station_visited);
		FollowWagon(rv, wagon);
	} else {
		/* In a ship or an aircraft it rides inside, out of sight, and the only
		 * sign of it is that the vessel is that much fuller. */
		AddRideMirror(carrier, rv->last_station_visited);
		RideInside(rv, carrier);
	}

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "Auto {}: nalozeno na {} {}{} ve stanici {}, vystoupi ve stanici {} (tik {})",
				rv->unitnumber,
				wagon != nullptr ? "vlak" : (carrier->type == VehicleType::Ship ? "lod" : "letadlo"),
				carrier->unitnumber,
				wagon != nullptr ? fmt::format(" (vagon {})", wagon->index) : std::string{},
				rv->last_station_visited,
				target == StationID::Invalid() ? -1 : (int)target.base(), TimerGameTick::counter);
	}

	InvalidateWindowData(WindowClass::VehicleView, rv->index);
	InvalidateWindowData(WindowClass::VehicleView, carrier->index);
	SetWindowDirty(WindowClass::VehicleDetails, carrier->index);
	return true;
}

/**
 * Find a road stop of this station that the whole of this road vehicle fits
 * into, the way it would fit when driving in off the road.
 * @param rv        the road vehicle, front of its chain
 * @param dest      the station to get out at
 * @param[out] stop the stop's tile
 * @param[out] into the way it drives in
 * @return whether one was found
 */
static bool FindFreeStop(const RoadVehicle *rv, StationID dest, TileIndex *stop, Trackdir *into)
{
	const Station *st = Station::GetIfValid(dest);
	if (st == nullptr) return false;

	for (const RoadStop *rs = st->GetPrimaryRoadStop(rv); rs != nullptr; rs = rs->GetNextRoadStop(rv)) {
		if (IsBayRoadStopTile(rs->xy)) {
			/* Driven into against the way it faces, like every vehicle that
			 * uses it. */
			if (rs->IsEntranceBusy() || !rs->HasFreeBay()) continue;
			*into = DiagDirToDiagTrackdir(ReverseDiagDir(GetBayRoadStopDir(rs->xy)));
		} else {
			DiagDirection along = AxisToDiagDir(GetDriveThroughStopAxis(rs->xy));
			const RoadStop::Entry &entry = rs->GetEntry(along);
			if (entry.GetOccupied() + rv->gcache.cached_total_length > entry.GetLength()) continue;
			*into = DiagDirToDiagTrackdir(along);
		}
		*stop = rs->xy;
		return true;
	}
	return false;
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

	TileIndex stop = INVALID_TILE;
	Trackdir into = Trackdir::Invalid;
	if (!FindFreeStop(rv, dest, &stop, &into)) return false;

	wagon->carrying = VehicleID::Invalid();
	rv->carried_by = VehicleID::Invalid();
	ClearRideMirror(wagon);
	PlaceRoadVehicleAtStopEntrance(rv, stop, into);

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "Auto {}: slozeno z vlaku {} ve stanici {} na ({},{}) (tik {})",
				rv->unitnumber, t->unitnumber, dest, TileX(stop), TileY(stop), TimerGameTick::counter);
	}
	InvalidateWindowData(WindowClass::VehicleView, rv->index);
	InvalidateWindowData(WindowClass::VehicleView, t->index);
	SetWindowDirty(WindowClass::VehicleDetails, t->index);
	return true;
}

/**
 * Get out of a ship or an aircraft, onto one of the station's road stops.
 *
 * Only ever at the station the vehicle's own next order names, and only while
 * the vessel is there and loading. There is no "put everything down here" the
 * way a train has one: a car left at the wrong port has no rails to be shunted
 * along and nothing coming to fetch it, which is the whole reason boarding a
 * ship or an aircraft is only ever offered towards the next stop.
 *
 * A station with no free road stop -- or none at all -- keeps the car aboard,
 * and it says so. It rides on to wherever the vessel goes next and tries again
 * when the vessel is back; nothing is ever dropped into the sea.
 *
 * @param rv      the road vehicle
 * @param carrier the ship or aircraft it rides in
 * @return whether it got out
 */
static bool TryLeaveVessel(RoadVehicle *rv, Vehicle *carrier)
{
	if (!carrier->current_order.IsType(OT_LOADING)) return false;
	if (carrier->last_station_visited == StationID::Invalid()) return false;
	if (!rv->current_order.IsType(OT_GOTO_STATION)) return false;

	StationID dest = rv->current_order.GetDestination().ToStationID();
	if (dest != carrier->last_station_visited) return false;

	TileIndex stop = INVALID_TILE;
	Trackdir into = Trackdir::Invalid;
	if (!FindFreeStop(rv, dest, &stop, &into)) {
		SayRoad(rv, fmt::format("Auto {}: ceka v {} {} ve stanici {} - neni volna zastavka",
				rv->unitnumber, carrier->type == VehicleType::Ship ? "lodi" : "letadle", carrier->unitnumber, dest));
		return false;
	}

	rv->carried_by = VehicleID::Invalid();
	RemoveRideMirror(carrier);
	for (RoadVehicle *u = rv; u != nullptr; u = u->Next()) u->vehstatus.Reset(VehState::Hidden);
	PlaceRoadVehicleAtStopEntrance(rv, stop, into);

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "Auto {}: slozeno z {} {} ve stanici {} na ({},{}) (tik {})",
				rv->unitnumber, carrier->type == VehicleType::Ship ? "lodi" : "letadla", carrier->unitnumber,
				dest, TileX(stop), TileY(stop), TimerGameTick::counter);
	}
	InvalidateWindowData(WindowClass::VehicleView, rv->index);
	InvalidateWindowData(WindowClass::VehicleView, carrier->index);
	SetWindowDirty(WindowClass::VehicleDetails, carrier->index);
	return true;
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
	Vehicle *carrier = Vehicle::GetIfValid(rv->carried_by);
	Train *wagon = (carrier != nullptr && carrier->type == VehicleType::Train) ? Train::From(carrier) : nullptr;
	bool lost = carrier == nullptr || (wagon != nullptr && wagon->carrying != rv->index);
	if (!lost && wagon == nullptr && carrier->type != VehicleType::Ship && carrier->type != VehicleType::Aircraft) lost = true;
	if (lost) {
		LogAnomaly("Auto {}: to, na cem se vezlo, uz neexistuje - auto zaniklo na ({},{})",
				rv->unitnumber, TileX(rv->tile), TileY(rv->tile));
		rv->carried_by = VehicleID::Invalid();
		delete rv;
		return false;
	}

	if (wagon != nullptr) {
		FollowWagon(rv, wagon);

		/* Asked every tick: a train with nothing to load stands at a platform for
		 * a couple of ticks and is gone, and a look every sixteen ticks rode past
		 * the station every time. Cheap enough -- the walk over the station's
		 * stops only happens once the train is standing at the right station. */
		TryLeaveTrain(rv, wagon);
	} else {
		RideInside(rv, carrier);
		TryLeaveVessel(rv, carrier);
	}
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
 * The wagon is going away and the road vehicle riding on it goes with it.
 *
 * It has to go, and leaving it behind was a crash waiting to happen. A carried
 * vehicle is parked in the wormhole state -- the state nothing asks questions
 * of -- with its tile set to whatever its wagon stands on, which is rail. That
 * holds only for as long as it is carried: its tick goes to
 * CarriedRoadVehicleTick() and never reaches the driving code. Cut loose, the
 * ordinary road code runs, and the first thing it asks is how fast this
 * vehicle may go -- which for a vehicle in a wormhole means reading the bridge
 * under it. There is no bridge, and the game goes down on the spot
 * (GetBridgeType(), the player's crash of 2026-09-21).
 *
 * Putting it down instead is no answer: under a wagon is rail, and a lorry has
 * nothing to stand on there. So it goes the way a lorry inside a ship or an
 * aircraft goes when the vessel does (DestroyRoadVehiclesAboard()), and for
 * the same reason -- and the record says so, because a vehicle vanishing is
 * exactly what the record is for.
 *
 * @param wagon the wagon being deleted
 */
void UnlinkCarriedRoadVehicle(Train *wagon)
{
	RoadVehicle *rv = RoadVehicle::GetIfValid(wagon->carrying);
	wagon->carrying = VehicleID::Invalid();
	if (rv == nullptr || rv->carried_by != wagon->index) return;

	rv->carried_by = VehicleID::Invalid();
	LogAnomaly("Auto {}: zaniklo s vagonem {}, na kterem se vezlo, na ({},{})", rv->unitnumber,
			wagon->index.base(), TileX(rv->tile), TileY(rv->tile));
	delete rv;
}

/**
 * The road vehicle is going away: whatever carried it has that much room
 * again -- the wagon it stood on, or the ship or aircraft it sat inside.
 * @param rv the road vehicle being deleted
 */
void UnlinkFromCarrier(RoadVehicle *rv)
{
	Vehicle *carrier = Vehicle::GetIfValid(rv->carried_by);
	if (carrier != nullptr) {
		if (carrier->type == VehicleType::Train) {
			Train *wagon = Train::From(carrier);
			if (wagon->carrying == rv->index) {
				wagon->carrying = VehicleID::Invalid();
				ClearRideMirror(wagon);
			}
		} else {
			RemoveRideMirror(carrier);
		}
	}
	rv->carried_by = VehicleID::Invalid();
}

/**
 * Does this vehicle carry road vehicles right now? Asked of a whole train, or
 * of a ship or an aircraft.
 * @param v the vehicle, its front
 * @return whether anything is riding on or in it
 */
bool CarriesRoadVehicles(const Vehicle *v)
{
	if (v->type == VehicleType::Train) return TrainCarriesRoadVehicle(Train::From(v));
	if (v->type != VehicleType::Ship && v->type != VehicleType::Aircraft) return false;
	return RoadVehiclesAboard(v) > 0;
}

/**
 * The ship or aircraft is going away -- sold, or crashed and cleared -- and
 * whatever rode inside goes with it. A car inside a vessel is on no road and
 * has nothing to be put down on, so it goes at once, and the record says so:
 * a vehicle vanishing is exactly what the record is for.
 * @param carrier the ship or aircraft
 */
void DestroyRoadVehiclesAboard(Vehicle *carrier)
{
	for (RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (!rv->IsFrontEngine() || rv->carried_by != carrier->index) continue;
		LogAnomaly("Auto {}: zaniklo s {} {} na ({},{})", rv->unitnumber,
				carrier->type == VehicleType::Ship ? "lodi" : "letadlem", carrier->unitnumber,
				TileX(carrier->tile), TileY(carrier->tile));
		rv->carried_by = VehicleID::Invalid();
		delete rv;
	}
	RemoveRideMirror(carrier);
	carrier->cargo.Truncate();
}
