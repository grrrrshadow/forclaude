/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file road_on_rail.h Road vehicles riding on rail wagons.
 *
 * A road vehicle with a "load onto train" station order drives to a road stop
 * of that station and waits there. When a train of the same company stands at
 * the station's platform with an empty wagon to spare, and that train's orders
 * take it to the station the road vehicle's next order names, the road
 * vehicle boards: it leaves the road, is drawn on the wagon and moves with it,
 * and none of its own driving runs. When the train comes to a stand at that
 * next station, the road vehicle gets off onto a road stop there, as if it
 * had driven in, and carries on with its orders -- loading or unloading there
 * if the order says so.
 *
 * The train knows nothing of this. Its orders are its own; a wagon carries one
 * road vehicle and is otherwise an ordinary wagon. What ties the two together
 * is one link each way, RoadVehicle::carried_by and Train::carrying, and the
 * road vehicle's own order list, which says where it gets on and where it gets
 * off.
 *
 * Ships and aircraft carry them too, and differently. A car goes inside rather
 * than on top: nothing of it is drawn, and the only sign of it is that the
 * vessel is that much fuller, which is what the player asked for. A ship holds
 * several -- one for every 40 of whatever it otherwise holds -- and an aircraft
 * exactly one, and only if it seats 60; the ones that come out at none are
 * never offered the fitting at all (RoadVehiclesCarriedBy(),
 * CanCarryRoadVehicles()). A vessel keeps no list of what it carries: the cars
 * say where they are, and counting them is asked only when one wants to get on.
 *
 * Boarding a ship or an aircraft is only ever offered towards the vehicle's own
 * next stop, never "wherever it goes". A car put down at the wrong port has no
 * rails to be shunted along and nothing coming to fetch it -- it would simply
 * be somewhere its driver never meant to be. For the same reason it gets out
 * only at the station its own order names, and a station with no free road stop
 * keeps it aboard for another round rather than dropping it into the sea.
 *
 * Which wagons take one: those refitted to road vehicles, a cargo of our own
 * (CT_ROLA, label "ROLA", after the rolling road) that no industry makes and
 * no station takes. It is a real cargo so that a wagon fitted for it has a
 * capacity -- one vehicle -- and is full or empty like any other wagon, and so
 * that a wagon set can name it in its cargo table and draw its own loaded
 * sprites for it. Only the car carriers take the refit: the game's own and the
 * flat wagons the player named (IsCarCarrierWagon()). While a vehicle rides, the
 * wagon holds one unit of the cargo (MirrorRideAsCargo()); the stations leave
 * that unit alone, so it pays nothing -- the vehicle on top earns with its own
 * cargo.
 *
 * The road vehicle keeps its number, its orders and its cargo throughout, the
 * same way a train carried by a rescue engine does (see CarriesAnotherTrain()).
 * Unlike that, it cannot ride in the train's own chain -- the chain is made of
 * trains -- so it is a separate vehicle whose place is copied from the wagon
 * each tick.
 */

#ifndef ROAD_ON_RAIL_H
#define ROAD_ON_RAIL_H

#include "vehicle_type.h"

class Engine; // a class in engine_base.h; MSVC mangles class and struct apart, so this must match
struct RoadVehicle;
struct Train;
struct Vehicle;

uint RoadVehiclesCarriedBy(const Engine *e, const Vehicle *v);
bool IsCarCarrierWagon(const Engine *e);
bool CanCarryRoadVehicles(const Engine *e);
uint RoadVehiclesAboard(const Vehicle *carrier);
bool TakesRoadVehiclesBesidePassengers(const Engine *e);
uint RoadVehicleRoomIn(const Vehicle *v);
void ConvertCarFerries();

bool TryBoardTrain(RoadVehicle *rv);
bool VesselHoldsForRoadVehicles(const Vehicle *v);
bool CarriedRoadVehicleTick(RoadVehicle *rv);
bool IsWaitingToBoardTrain(const RoadVehicle *rv);

bool TrainCarriesRoadVehicle(const Train *t);
void DestroyCarriedRoadVehicles(Train *t);
void UnlinkCarriedRoadVehicle(Train *wagon);
void UnlinkFromCarrier(RoadVehicle *rv);
bool CarriesRoadVehicles(const Vehicle *v);
void DestroyRoadVehiclesAboard(Vehicle *carrier);
void RestandCarriedRoadVehicles();

#endif /* ROAD_ON_RAIL_H */
