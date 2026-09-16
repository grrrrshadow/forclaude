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
 * The road vehicle keeps its number, its orders and its cargo throughout, the
 * same way a train carried by a rescue engine does (see CarriesAnotherTrain()).
 * Unlike that, it cannot ride in the train's own chain -- the chain is made of
 * trains -- so it is a separate vehicle whose place is copied from the wagon
 * each tick.
 */

#ifndef ROAD_ON_RAIL_H
#define ROAD_ON_RAIL_H

#include "vehicle_type.h"

struct RoadVehicle;
struct Train;

bool TryBoardTrain(RoadVehicle *rv);
bool CarriedRoadVehicleTick(RoadVehicle *rv);
bool IsWaitingToBoardTrain(const RoadVehicle *rv);

bool TrainCarriesRoadVehicle(const Train *t);
void DestroyCarriedRoadVehicles(Train *t);
void UnlinkCarriedRoadVehicle(Train *wagon);
void UnlinkFromWagon(RoadVehicle *rv);

#endif /* ROAD_ON_RAIL_H */
