/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ship.h Base for ships. */

#ifndef SHIP_H
#define SHIP_H

#include "vehicle_base.h"
#include "water_map.h"

void GetShipSpriteSize(EngineID engine, uint &width, uint &height, int &xoffs, int &yoffs, EngineImageType image_type);
WaterClass GetEffectiveWaterClass(TileIndex tile);

/** Element of the ShipPathCache. */
struct ShipPathElement {
	Trackdir trackdir = Trackdir::Invalid; ///< Trackdir for this element.

	constexpr ShipPathElement() {}
	constexpr ShipPathElement(Trackdir trackdir) : trackdir(trackdir) {}
};

using ShipPathCache = std::vector<ShipPathElement>;

/**
 * How near the target a ship shoots from, in tiles.
 *
 * A ship does not sail up to the target and shoot at point blank. It shoots
 * the moment the target comes this near, whether it was already this near
 * when the crosshair went down or came this near on the way.
 */
static const uint RAID_SHIP_FIRING_RANGE = 30;
/**
 * How far the rocket will reach at the outside, in tiles.
 *
 * Between the firing range and this one is where a ship shoots that cannot
 * get any nearer -- a headland in the way, a bay that does not open the
 * right way. Past it there is no shot at all, and the crosshair is refused
 * when there is no water within this of the target the ship can reach.
 */
static const uint RAID_SHIP_REACH = 50;

/**
 * All ships have this type.
 */
struct Ship final : public SpecializedVehicle<Ship, VehicleType::Ship> {
	ShipPathCache path{}; ///< Cached path.
	TrackBits state{}; ///< The "track" the ship is following.
	Direction rotation = Direction::Invalid; ///< Visible direction.
	int16_t rotation_x_pos = 0; ///< NOSAVE: X Position before rotation.
	int16_t rotation_y_pos = 0; ///< NOSAVE: Y Position before rotation.

	/**
	 * Where this ship was sent to put its rocket, or INVALID_TILE.
	 *
	 * Not in the savegame, same as the aircraft's: an errand the player has
	 * not finished does not follow them into the next game.
	 */
	TileIndex raid_target = INVALID_TILE;
	/** NOSAVE: the water the ship is making for, the nearest there is to the target. */
	TileIndex raid_sail_to = INVALID_TILE;
	/** NOSAVE: where the ship was going before the errand, to be put back afterwards. */
	TileIndex raid_return_to = INVALID_TILE;
	/** NOSAVE: the closest the ship has come to the target so far, to tell progress from going nowhere. */
	uint raid_closest = 0;
	/** NOSAVE: ticks since it last came any closer; enough of them and the errand is given up. */
	uint raid_stale = 0;

	Ship(VehicleID index) : SpecializedVehicleBase(index) {}
	/** We want to 'destruct' the right class. */
	~Ship() override { this->PreDestructor(); }

	void MarkDirty() override;
	void UpdateDeltaXY() override;
	ExpensesType GetExpenseType(bool income) const override { return income ? ExpensesType::ShipRevenue : ExpensesType::ShipRun; }
	void PlayLeaveStationSound(bool force = false) const override;
	bool IsPrimaryVehicle() const override { return true; }
	void GetImage(Direction direction, EngineImageType image_type, VehicleSpriteSeq *result) const override;
	int GetDisplaySpeed() const override { return this->cur_speed / 2; }
	int GetDisplayMaxSpeed() const override { return this->vcache.cached_max_speed / 2; }
	int GetCurrentMaxSpeed() const override { return std::min<int>(this->vcache.cached_max_speed, this->current_order.GetMaxSpeed() * 2); }
	Money GetRunningCost() const override;
	bool IsInDepot() const override { return this->state == Track::Depot; }
	bool Tick() override;
	void OnNewCalendarDay() override;
	void OnNewEconomyDay() override;
	Trackdir GetVehicleTrackdir() const override;
	TileIndex GetOrderStationLocation(StationID station) override;
	ClosestDepot FindClosestDepot() override;
	void UpdateCache();
	void SetDestTile(TileIndex tile) override;
};

bool IsShipDestinationTile(TileIndex tile, StationID station);

#endif /* SHIP_H */
