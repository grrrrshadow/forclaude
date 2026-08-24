/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.cpp Copying infrastructure from the map into a blueprint, and pasting it back. */

#include "stdafx.h"
#include "blueprint.h"
#include "blueprint_cmd.h"
#include "bridge_map.h"
#include "command_func.h"
#include "company_func.h"
#include "core/math_func.hpp"
#include "direction_func.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "road_cmd.h"
#include "road_func.h"
#include "road_map.h"
#include "settings_type.h"
#include "signal_func.h"
#include "slope_type.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "terraform_cmd.h"
#include "tile_map.h"
#include "track_func.h"
#include "tunnel_map.h"
#include "tunnelbridge_cmd.h"
#include "tunnelbridge_map.h"
#include "water_cmd.h"
#include "water_map.h"
#include "waypoint_cmd.h"

#include "safeguards.h"

/** The four blueprint slots the player can copy into. */
static Blueprint _blueprints[NUM_BLUEPRINT_SLOTS];

/**
 * Get the blueprint stored in a slot.
 * @param slot Slot number (0-based).
 * @return The blueprint of that slot.
 */
Blueprint &GetBlueprint(uint slot)
{
	assert(slot < NUM_BLUEPRINT_SLOTS);
	return _blueprints[slot];
}

/** Get the total number of copied elements. */
uint Blueprint::CountItems() const
{
	return static_cast<uint>(this->rail_tracks.size() + this->signals.size() + this->rail_depots.size()
			+ this->tunnel_bridges.size() + this->roads.size() + this->road_depots.size()
			+ this->canals.size() + this->locks.size() + this->ship_depots.size()
			+ this->station_tiles.size() + this->road_stops.size() + this->docks.size()
			+ this->buoys.size() + this->airports.size());
}

/** Remove all contents from this blueprint, keeping its name (see #CopyAreaToBlueprint). */
void Blueprint::Clear()
{
	std::string kept_name = std::move(this->name);
	*this = Blueprint{};
	this->name = std::move(kept_name);
}

/**
 * Get the offset of a tile relative to the blueprint origin.
 * @param tile Tile within the copied area.
 * @param origin Northern tile of the copied area.
 * @return Offset of \a tile.
 */
static TileIndexDiffC BlueprintOffset(TileIndex tile, TileIndex origin)
{
	return {static_cast<int16_t>(TileX(tile) - TileX(origin)), static_cast<int16_t>(TileY(tile) - TileY(origin))};
}

/**
 * Copy rail tracks and signals of a plain rail tile (or its depot).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyRailTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	if (IsRailDepot(tile)) {
		blueprint.rail_depots.push_back({offset, GetRailDepotDirection(tile), GetRailType(tile)});
		return;
	}

	for (Track track : GetTrackBits(tile)) {
		blueprint.rail_tracks.push_back({offset, track, GetRailType(tile)});

		if (HasSignalOnTrack(tile, track)) {
			Trackdir td = TrackToTrackdir(track);
			blueprint.signals.push_back({offset, track, GetSignalType(tile, track), GetSignalVariant(tile, track),
					HasSignalOnTrackdir(tile, td), HasSignalOnTrackdir(tile, ReverseTrackdir(td))});
		}
	}
}

/**
 * Copy the road and tram pieces of a tile that the local company owns.
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 * @param bits Road pieces to store for each owned road/tram type on the tile.
 */
static void CopyOwnedRoadBits(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset, RoadBits bits)
{
	for (RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
		RoadType rt = GetRoadType(tile, rtt);
		if (rt == INVALID_ROADTYPE) continue;
		if (GetRoadOwner(tile, rtt) != _local_company) continue;
		blueprint.roads.push_back({offset, bits, rt});
	}
}

/**
 * Copy a road tile (normal road, level crossing or road depot).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyRoadTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	switch (GetRoadTileType(tile)) {
		case RoadTileType::Normal:
			CopyOwnedRoadBits(blueprint, tile, offset, GetRoadBits(tile, RoadTramType::Road) | GetRoadBits(tile, RoadTramType::Tram));
			break;

		case RoadTileType::Crossing:
			/* The rail part belongs to the tile owner, the road part to the road owner(s). */
			if (GetTileOwner(tile) == _local_company) {
				blueprint.rail_tracks.push_back({offset, GetCrossingRailTrack(tile), GetRailType(tile)});
			}
			CopyOwnedRoadBits(blueprint, tile, offset, GetCrossingRoadBits(tile));
			break;

		case RoadTileType::Depot:
			if (GetTileOwner(tile) != _local_company) return;
			blueprint.road_depots.push_back({offset, GetRoadDepotDirection(tile),
					GetRoadTypeRoad(tile) != INVALID_ROADTYPE ? GetRoadTypeRoad(tile) : GetRoadTypeTram(tile)});
			break;

		default: NOT_REACHED();
	}
}

/**
 * Copy a tunnel or bridge if it is fully contained in the copied area.
 * @param blueprint Blueprint to copy into.
 * @param area The copied area.
 * @param tile Tunnel or bridge end tile.
 */
static void CopyTunnelBridgeTile(Blueprint &blueprint, const TileArea &area, TileIndex tile)
{
	if (GetTileOwner(tile) != _local_company) return;

	TileIndex other = IsTunnel(tile) ? GetOtherTunnelEnd(tile) : GetOtherBridgeEnd(tile);

	/* Record each tunnel/bridge only once, anchored at its northern end,
	 * and only when both ends are inside the copied area. */
	if (other < tile) return;
	if (!area.Contains(other)) return;

	BlueprintTunnelBridge tb{};
	tb.offset = BlueprintOffset(tile, area.tile);
	tb.other_end = BlueprintOffset(other, area.tile);
	tb.transport = GetTunnelBridgeTransportType(tile);
	tb.is_bridge = IsBridge(tile);
	tb.bridge_type = tb.is_bridge ? GetBridgeType(tile) : 0;
	tb.railtype = tb.transport == TransportType::Rail ? GetRailType(tile) : INVALID_RAILTYPE;
	tb.roadtype = tb.transport == TransportType::Road ? GetRoadTypeRoad(tile) : INVALID_ROADTYPE;
	tb.tramtype = tb.transport == TransportType::Road ? GetRoadTypeTram(tile) : INVALID_ROADTYPE;
	blueprint.tunnel_bridges.push_back(tb);
}

/**
 * Copy a water tile (canal, lock or ship depot).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyWaterTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	if (GetTileOwner(tile) != _local_company) return;

	if (IsCanal(tile)) {
		blueprint.canals.push_back({offset});
	} else if (IsLock(tile)) {
		/* Record the lock only once, at its middle part. */
		if (GetLockPart(tile) == LockPart::Middle) blueprint.locks.push_back({offset, GetLockDirection(tile)});
	} else if (IsShipDepot(tile)) {
		/* Record the depot only once, at its northern part. */
		if (GetShipDepotPart(tile) == DepotPart::North) blueprint.ship_depots.push_back({offset, GetShipDepotAxis(tile)});
	}
}

/**
 * Copy a station tile (rail station/waypoint, road stop, dock, buoy or airport).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyStationTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	if (GetTileOwner(tile) != _local_company) return;

	switch (GetStationType(tile)) {
		case StationType::Rail:
			blueprint.station_tiles.push_back({offset, false, GetRailStationAxis(tile), GetRailType(tile), GetStationIndex(tile)});
			break;

		case StationType::RailWaypoint:
			blueprint.station_tiles.push_back({offset, true, GetRailStationAxis(tile), GetRailType(tile), GetStationIndex(tile)});
			break;

		case StationType::Bus:
		case StationType::Truck:
		case StationType::RoadWaypoint: {
			BlueprintRoadStop stop{};
			stop.offset = offset;
			stop.type = GetStationType(tile);
			stop.drive_through = IsDriveThroughStopTile(tile);
			stop.dir = stop.drive_through ? DiagDirection::Invalid : GetBayRoadStopDir(tile);
			stop.axis = stop.drive_through ? GetDriveThroughStopAxis(tile) : Axis::Invalid;
			stop.roadtype = GetRoadTypeRoad(tile);
			stop.tramtype = GetRoadTypeTram(tile);
			stop.station = GetStationIndex(tile);
			blueprint.road_stops.push_back(stop);
			break;
		}

		case StationType::Dock:
			/* Record the dock only once, at its land part. */
			if (GetStationGfx(tile) < GFX_DOCK_BASE_WATER_PART) blueprint.docks.push_back({offset, GetDockDirection(tile), GetStationIndex(tile)});
			break;

		case StationType::Buoy:
			blueprint.buoys.push_back({offset});
			break;

		case StationType::Airport: {
			/* Record the airport only once, at its northern tile. */
			const Station *st = Station::GetByTile(tile);
			if (st->airport.tile == tile) blueprint.airports.push_back({offset, st->airport.type, st->airport.layout, st->airport.w, st->airport.h, st->index});
			break;
		}

		default: // Oilrigs and anything else is not copyable.
			break;
	}
}

/**
 * Shrink a freshly copied blueprint to the smallest rectangle that contains
 * all copied elements (including the extra tiles of multi-tile elements).
 * @param blueprint Blueprint to shrink; must not be empty.
 * @return The offset the blueprint contents were shifted by.
 */
static TileIndexDiffC ShrinkBlueprintToContents(Blueprint &blueprint)
{
	int min_x = blueprint.width - 1;
	int min_y = blueprint.height - 1;
	int max_x = 0;
	int max_y = 0;

	auto include = [&](int x, int y) {
		x = Clamp(x, 0, blueprint.width - 1);
		y = Clamp(y, 0, blueprint.height - 1);
		min_x = std::min(min_x, x);
		min_y = std::min(min_y, y);
		max_x = std::max(max_x, x);
		max_y = std::max(max_y, y);
	};

	for (const BlueprintRailTrack &v : blueprint.rail_tracks) include(v.offset.x, v.offset.y);
	for (const BlueprintSignal &v : blueprint.signals) include(v.offset.x, v.offset.y);
	for (const BlueprintRailDepot &v : blueprint.rail_depots) include(v.offset.x, v.offset.y);
	for (const BlueprintTunnelBridge &v : blueprint.tunnel_bridges) {
		include(v.offset.x, v.offset.y);
		include(v.other_end.x, v.other_end.y);
	}
	for (const BlueprintRoad &v : blueprint.roads) include(v.offset.x, v.offset.y);
	for (const BlueprintRoadDepot &v : blueprint.road_depots) include(v.offset.x, v.offset.y);
	for (const BlueprintCanal &v : blueprint.canals) include(v.offset.x, v.offset.y);
	for (const BlueprintLock &v : blueprint.locks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(v.dir);
		include(v.offset.x - d.x, v.offset.y - d.y);
		include(v.offset.x, v.offset.y);
		include(v.offset.x + d.x, v.offset.y + d.y);
	}
	for (const BlueprintShipDepot &v : blueprint.ship_depots) {
		include(v.offset.x, v.offset.y);
		include(v.offset.x + (v.axis == Axis::X ? 1 : 0), v.offset.y + (v.axis == Axis::Y ? 1 : 0));
	}
	for (const BlueprintStationTile &v : blueprint.station_tiles) include(v.offset.x, v.offset.y);
	for (const BlueprintRoadStop &v : blueprint.road_stops) include(v.offset.x, v.offset.y);
	for (const BlueprintDock &v : blueprint.docks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(v.dir);
		include(v.offset.x, v.offset.y);
		include(v.offset.x + d.x, v.offset.y + d.y);
	}
	for (const BlueprintBuoy &v : blueprint.buoys) include(v.offset.x, v.offset.y);
	for (const BlueprintAirport &v : blueprint.airports) {
		include(v.offset.x, v.offset.y);
		include(v.offset.x + v.w - 1, v.offset.y + v.h - 1);
	}

	auto shift = [&](TileIndexDiffC &offset) {
		offset.x = static_cast<int16_t>(offset.x - min_x);
		offset.y = static_cast<int16_t>(offset.y - min_y);
	};
	for (BlueprintRailTrack &v : blueprint.rail_tracks) shift(v.offset);
	for (BlueprintSignal &v : blueprint.signals) shift(v.offset);
	for (BlueprintRailDepot &v : blueprint.rail_depots) shift(v.offset);
	for (BlueprintTunnelBridge &v : blueprint.tunnel_bridges) {
		shift(v.offset);
		shift(v.other_end);
	}
	for (BlueprintRoad &v : blueprint.roads) shift(v.offset);
	for (BlueprintRoadDepot &v : blueprint.road_depots) shift(v.offset);
	for (BlueprintCanal &v : blueprint.canals) shift(v.offset);
	for (BlueprintLock &v : blueprint.locks) shift(v.offset);
	for (BlueprintShipDepot &v : blueprint.ship_depots) shift(v.offset);
	for (BlueprintStationTile &v : blueprint.station_tiles) shift(v.offset);
	for (BlueprintRoadStop &v : blueprint.road_stops) shift(v.offset);
	for (BlueprintDock &v : blueprint.docks) shift(v.offset);
	for (BlueprintBuoy &v : blueprint.buoys) shift(v.offset);
	for (BlueprintAirport &v : blueprint.airports) shift(v.offset);

	blueprint.width = static_cast<uint16_t>(max_x - min_x + 1);
	blueprint.height = static_cast<uint16_t>(max_y - min_y + 1);

	return {static_cast<int16_t>(min_x), static_cast<int16_t>(min_y)};
}

/**
 * Copy all infrastructure of the local company in an area into a blueprint.
 * The blueprint is shrunk to the smallest rectangle containing the copied
 * elements; empty margins of the selected area are not kept.
 * @param area Map area to copy.
 * @param blueprint Blueprint to copy into; existing contents are replaced.
 * @return Number of copied elements.
 */
uint CopyAreaToBlueprint(TileArea area, Blueprint &blueprint)
{
	blueprint.Clear();
	area.ClampToMap();
	blueprint.width = area.w;
	blueprint.height = area.h;

	for (TileIndex tile : area) {
		TileIndexDiffC offset = BlueprintOffset(tile, area.tile);

		switch (GetTileType(tile)) {
			case TileType::Railway:
				if (GetTileOwner(tile) != _local_company) break;
				CopyRailTile(blueprint, tile, offset);
				break;

			case TileType::Road:
				CopyRoadTile(blueprint, tile, offset);
				break;

			case TileType::TunnelBridge:
				CopyTunnelBridgeTile(blueprint, area, tile);
				break;

			case TileType::Water:
				CopyWaterTile(blueprint, tile, offset);
				break;

			case TileType::Station:
				CopyStationTile(blueprint, tile, offset);
				break;

			default: // Terrain, houses, industries, ... are not copyable.
				break;
		}
	}

	uint count = blueprint.CountItems();
	uint ox = TileX(area.tile);
	uint oy = TileY(area.tile);

	if (count > 0) {
		TileIndexDiffC shifted = ShrinkBlueprintToContents(blueprint);
		ox += shifted.x;
		oy += shifted.y;
	}

	/* Store the landscape: heights of all tile corners of the area, for terraforming on paste. */
	blueprint.corner_heights.reserve((blueprint.width + 1) * (blueprint.height + 1));
	for (uint y = 0; y <= blueprint.height; y++) {
		for (uint x = 0; x <= blueprint.width; x++) {
			blueprint.corner_heights.push_back(TileHeight(TileXY(std::min(ox + x, Map::MaxX()), std::min(oy + y, Map::MaxY()))));
		}
	}

	return count;
}

/**
 * Transform a point of a blueprint: reflect against the NW-SE axis first (if requested),
 * then rotate anticlockwise in quarter turns.
 * @param point Point to transform, coordinates within [0, bounds_x) x [0, bounds_y).
 * @param bounds_x Size of the point grid along the X axis.
 * @param bounds_y Size of the point grid along the Y axis.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect before rotating.
 * @return The transformed point; for odd rotations the bounds are swapped.
 */
TileIndexDiffC TransformBlueprintPoint(TileIndexDiffC point, uint16_t bounds_x, uint16_t bounds_y, uint8_t rotation, bool reflected)
{
	if (reflected) point.x = static_cast<int16_t>(bounds_x - 1 - point.x);

	for (uint i = 0; i < rotation % 4u; i++) {
		/* Rotate 90 degrees anticlockwise: north corner moves to the west. */
		point = {static_cast<int16_t>(bounds_y - 1 - point.y), point.x};
		std::swap(bounds_x, bounds_y);
	}

	return point;
}

/**
 * Transform a track piece the same way as #TransformBlueprintPoint transforms points.
 * @param track Track piece to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed track piece.
 */
Track TransformBlueprintTrack(Track track, uint8_t rotation, bool reflected)
{
	/* Reflection against the NW-SE axis: the NE and SW tile sides swap. */
	static const TrackIndexArray<Track> reflect_map{Track::X, Track::Y, Track::Left, Track::Right, Track::Upper, Track::Lower};
	/* Rotation by 90 degrees anticlockwise: the north corner moves to the west. */
	static const TrackIndexArray<Track> rotate_map{Track::Y, Track::X, Track::Left, Track::Right, Track::Lower, Track::Upper};

	if (reflected) track = reflect_map[track];
	for (uint i = 0; i < rotation % 4u; i++) track = rotate_map[track];

	return track;
}

/**
 * Transform a diagonal direction the same way as #TransformBlueprintPoint transforms points.
 * @param dir Direction to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed direction.
 */
DiagDirection TransformBlueprintDiagDir(DiagDirection dir, uint8_t rotation, bool reflected)
{
	/* Reflection against the NW-SE axis: NE and SW swap, NW and SE stay. */
	static const std::array<DiagDirection, 4> reflect_map{DiagDirection::SW, DiagDirection::SE, DiagDirection::NE, DiagDirection::NW};

	if (reflected) dir = reflect_map[to_underlying(dir)];
	/* Rotation by 90 degrees anticlockwise: NE -> NW -> SW -> SE -> NE. */
	return static_cast<DiagDirection>((to_underlying(dir) + 3 * (rotation % 4u)) % 4u);
}

/**
 * Transform an axis the same way as #TransformBlueprintPoint transforms points.
 * @param axis Axis to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @return The transformed axis (reflections never change an axis).
 */
Axis TransformBlueprintAxis(Axis axis, uint8_t rotation)
{
	return (rotation % 2) != 0 ? OtherAxis(axis) : axis;
}

/**
 * Transform road pieces the same way as #TransformBlueprintPoint transforms points.
 * @param bits Road pieces to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed road pieces.
 */
RoadBits TransformBlueprintRoadBits(RoadBits bits, uint8_t rotation, bool reflected)
{
	RoadBits result = RoadBits{};
	for (DiagDirection dir = DiagDirection::Begin; dir < DiagDirection::End; dir++) {
		if ((bits & DiagDirToRoadBits(dir)) == RoadBits{}) continue;
		result |= DiagDirToRoadBits(TransformBlueprintDiagDir(dir, rotation, reflected));
	}
	return result;
}

/**
 * Order two offsets so that the first one is the more northern map tile.
 * @param a First offset (in/out).
 * @param b Second offset (in/out).
 */
static void SortBlueprintOffsets(TileIndexDiffC &a, TileIndexDiffC &b)
{
	if (b.y < a.y || (b.y == a.y && b.x < a.x)) std::swap(a, b);
}

/**
 * Create a transformed copy of a blueprint: all offsets, orientations and the
 * stored landscape are reflected/rotated; multi-tile elements are re-anchored
 * at their northern tile.
 * @param blueprint Blueprint to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed blueprint.
 */
Blueprint TransformBlueprint(const Blueprint &blueprint, uint8_t rotation, bool reflected)
{
	Blueprint result;
	bool swap_axes = (rotation % 2) != 0;
	result.width = swap_axes ? blueprint.height : blueprint.width;
	result.height = swap_axes ? blueprint.width : blueprint.height;

	/* Transform a tile offset within the blueprint area. */
	auto tile_offset = [&](TileIndexDiffC offset) {
		return TransformBlueprintPoint(offset, blueprint.width, blueprint.height, rotation, reflected);
	};

	for (const BlueprintRailTrack &rt : blueprint.rail_tracks) {
		result.rail_tracks.push_back({tile_offset(rt.offset), TransformBlueprintTrack(rt.track, rotation, reflected), rt.railtype});
	}

	for (const BlueprintSignal &sig : blueprint.signals) {
		/* Transform the canonical trackdir of the track; when it maps onto the
		 * reversed trackdir of the new track, the two signal directions swap. */
		Trackdir td = TrackToTrackdir(sig.track);
		Track new_track = TransformBlueprintTrack(sig.track, rotation, reflected);
		Trackdir new_td = TrackExitdirToTrackdir(new_track, TransformBlueprintDiagDir(TrackdirToExitdir(td), rotation, reflected));
		bool swapped = new_td != TrackToTrackdir(new_track);
		result.signals.push_back({tile_offset(sig.offset), new_track, sig.type, sig.variant,
				swapped ? sig.against : sig.along, swapped ? sig.along : sig.against});
	}

	for (const BlueprintRailDepot &depot : blueprint.rail_depots) {
		result.rail_depots.push_back({tile_offset(depot.offset), TransformBlueprintDiagDir(depot.dir, rotation, reflected), depot.railtype});
	}

	for (BlueprintTunnelBridge tb : blueprint.tunnel_bridges) {
		tb.offset = tile_offset(tb.offset);
		tb.other_end = tile_offset(tb.other_end);
		SortBlueprintOffsets(tb.offset, tb.other_end);
		result.tunnel_bridges.push_back(tb);
	}

	for (const BlueprintRoad &road : blueprint.roads) {
		result.roads.push_back({tile_offset(road.offset), TransformBlueprintRoadBits(road.bits, rotation, reflected), road.roadtype});
	}

	for (const BlueprintRoadDepot &depot : blueprint.road_depots) {
		result.road_depots.push_back({tile_offset(depot.offset), TransformBlueprintDiagDir(depot.dir, rotation, reflected), depot.roadtype});
	}

	for (const BlueprintCanal &canal : blueprint.canals) {
		result.canals.push_back({tile_offset(canal.offset)});
	}

	for (const BlueprintLock &lock : blueprint.locks) {
		/* Locks are anchored at their (symmetric) middle part. */
		result.locks.push_back({tile_offset(lock.offset), TransformBlueprintDiagDir(lock.dir, rotation, reflected)});
	}

	for (const BlueprintShipDepot &depot : blueprint.ship_depots) {
		/* Transform both halves and re-anchor at the northern one. */
		TileIndexDiffC a = depot.offset;
		TileIndexDiffC b = {static_cast<int16_t>(a.x + (depot.axis == Axis::X ? 1 : 0)), static_cast<int16_t>(a.y + (depot.axis == Axis::Y ? 1 : 0))};
		a = tile_offset(a);
		b = tile_offset(b);
		SortBlueprintOffsets(a, b);
		result.ship_depots.push_back({a, TransformBlueprintAxis(depot.axis, rotation)});
	}

	for (const BlueprintStationTile &st : blueprint.station_tiles) {
		result.station_tiles.push_back({tile_offset(st.offset), st.is_waypoint, TransformBlueprintAxis(st.axis, rotation), st.railtype, st.station});
	}

	for (BlueprintRoadStop stop : blueprint.road_stops) {
		stop.offset = tile_offset(stop.offset);
		if (stop.drive_through) {
			stop.axis = TransformBlueprintAxis(stop.axis, rotation);
		} else {
			stop.dir = TransformBlueprintDiagDir(stop.dir, rotation, reflected);
		}
		result.road_stops.push_back(stop);
	}

	for (const BlueprintDock &dock : blueprint.docks) {
		result.docks.push_back({tile_offset(dock.offset), TransformBlueprintDiagDir(dock.dir, rotation, reflected), dock.station});
	}

	for (const BlueprintBuoy &buoy : blueprint.buoys) {
		result.buoys.push_back({tile_offset(buoy.offset)});
	}

	for (BlueprintAirport airport : blueprint.airports) {
		/* Transform two opposite corners of the airport area and re-anchor at the northern corner.
		 * The layout itself is not rotated; airport layouts are fixed by their spec. */
		TileIndexDiffC a = airport.offset;
		TileIndexDiffC b = {static_cast<int16_t>(a.x + airport.w - 1), static_cast<int16_t>(a.y + airport.h - 1)};
		a = tile_offset(a);
		b = tile_offset(b);
		airport.offset = {std::min(a.x, b.x), std::min(a.y, b.y)};
		if (swap_axes) std::swap(airport.w, airport.h);
		result.airports.push_back(airport);
	}

	/* Transform the corner height grid; it has one more point than tiles in each direction. */
	result.corner_heights.resize((result.width + 1) * (result.height + 1), 0);
	for (uint y = 0; y <= blueprint.height; y++) {
		for (uint x = 0; x <= blueprint.width; x++) {
			TileIndexDiffC p = TransformBlueprintPoint({static_cast<int16_t>(x), static_cast<int16_t>(y)},
					blueprint.width + 1, blueprint.height + 1, rotation, reflected);
			result.corner_heights[p.y * (result.width + 1) + p.x] = blueprint.corner_heights[y * (blueprint.width + 1) + x];
		}
	}

	return result;
}

/**
 * Group the per-tile station records of a blueprint into maximal rectangles of
 * equal kind (axis, waypoint flag, rail type, original station), suitable for
 * build commands.
 * @param blueprint Blueprint (must already be transformed).
 * @return The station rectangles.
 */
std::vector<BlueprintStationRect> GroupStationTiles(const Blueprint &blueprint)
{
	std::vector<BlueprintStationRect> result;
	uint w = blueprint.width;
	uint h = blueprint.height;

	struct Cell {
		bool present = false;
		bool used = false;
		bool is_waypoint = false;
		Axis axis = Axis::X;
		RailType railtype = INVALID_RAILTYPE;
		StationID station = StationID::Invalid();

		bool SameKind(const Cell &other) const
		{
			return this->present && !this->used && this->is_waypoint == other.is_waypoint && this->axis == other.axis && this->railtype == other.railtype && this->station == other.station;
		}
	};
	std::vector<Cell> grid(w * h);

	for (const BlueprintStationTile &st : blueprint.station_tiles) {
		Cell &cell = grid[st.offset.y * w + st.offset.x];
		cell.present = true;
		cell.is_waypoint = st.is_waypoint;
		cell.axis = st.axis;
		cell.railtype = st.railtype;
		cell.station = st.station;
	}

	for (uint y = 0; y < h; y++) {
		for (uint x = 0; x < w; x++) {
			Cell &cell = grid[y * w + x];
			if (!cell.present || cell.used) continue;

			/* Greedily grow a rectangle of equal tiles: first along X, then along Y. */
			uint len_x = 1;
			while (x + len_x < w && grid[y * w + x + len_x].SameKind(cell)) len_x++;

			uint len_y = 1;
			for (; y + len_y < h; len_y++) {
				bool row_matches = true;
				for (uint dx = 0; dx < len_x; dx++) {
					if (!grid[(y + len_y) * w + x + dx].SameKind(cell)) {
						row_matches = false;
						break;
					}
				}
				if (!row_matches) break;
			}

			for (uint dy = 0; dy < len_y; dy++) {
				for (uint dx = 0; dx < len_x; dx++) {
					grid[(y + dy) * w + x + dx].used = true;
				}
			}

			result.push_back({{static_cast<int16_t>(x), static_cast<int16_t>(y)},
					static_cast<uint16_t>(len_x), static_cast<uint16_t>(len_y), cell.is_waypoint, cell.axis, cell.railtype, cell.station});
		}
	}

	return result;
}

/**
 * Pick the fastest bridge type currently available for a bridge of the given length.
 * @param bridge_len Length of the bridge, excluding the ramp tiles.
 * @param fallback Bridge type to keep when no type is available at all.
 * @return The available bridge type with the highest speed limit (ties: the cheaper one).
 */
static BridgeType FindFastestBridgeType(uint bridge_len, BridgeType fallback)
{
	BridgeType best = fallback;
	const BridgeSpec *best_spec = nullptr;

	for (BridgeType bt = 0; bt < MAX_BRIDGES; bt++) {
		if (CheckBridgeAvailability(bt, bridge_len).Failed()) continue;
		const BridgeSpec *spec = GetBridgeSpec(bt);
		if (best_spec == nullptr || spec->speed > best_spec->speed || (spec->speed == best_spec->speed && spec->price < best_spec->price)) {
			best = bt;
			best_spec = spec;
		}
	}

	return best;
}

/**
 * Prepare a blueprint for the paste command: apply the transformation, the
 * paste filters and the rail/signal/bridge options, all of which are pure
 * client-side choices. The result carries everything the command needs.
 * @param blueprint Blueprint to paste (as copied).
 * @param options Filters, transformation and terraform settings to apply.
 * @param convert_to Rail type to convert rail to (when the option is enabled).
 * @return The pre-processed paste data.
 */
BlueprintPasteData PrepareBlueprintPaste(const Blueprint &blueprint, const BlueprintPasteOptions &options, RailType convert_to)
{
	BlueprintPasteData data;
	data.terraform_mode = options.terraform_mode;
	data.height_offset = options.height_offset;

	Blueprint &bp = data.blueprint;
	bp = TransformBlueprint(blueprint, options.rotation, options.reflected);

	if (!options.paste_rail) {
		bp.rail_tracks.clear();
		bp.signals.clear();
		bp.rail_depots.clear();
	}
	if (!options.paste_road) {
		bp.roads.clear();
		bp.road_depots.clear();
	}
	if (!options.paste_water) {
		bp.canals.clear();
		bp.locks.clear();
		bp.ship_depots.clear();
	}
	if (!options.paste_air) bp.airports.clear();
	std::erase_if(bp.tunnel_bridges, [&](const BlueprintTunnelBridge &tb) {
		switch (tb.transport) {
			case TransportType::Rail: return !options.paste_rail;
			case TransportType::Road: return !options.paste_road;
			default: return !options.paste_water;
		}
	});
	if (!options.with_stations || !options.paste_rail) bp.station_tiles.clear();
	if (!options.with_stations || !options.paste_road) bp.road_stops.clear();
	if (!options.with_stations || !options.paste_water) {
		bp.docks.clear();
		bp.buoys.clear();
	}

	if (options.convert_railtype && convert_to != INVALID_RAILTYPE) {
		for (BlueprintRailTrack &rt : bp.rail_tracks) rt.railtype = convert_to;
		for (BlueprintRailDepot &depot : bp.rail_depots) depot.railtype = convert_to;
		for (BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
			if (tb.transport == TransportType::Rail) tb.railtype = convert_to;
		}
		for (BlueprintStationTile &st : bp.station_tiles) st.railtype = convert_to;
	}

	if (options.mirror_signals) {
		for (BlueprintSignal &sig : bp.signals) std::swap(sig.along, sig.against);
	}

	if (options.upgrade_bridges) {
		for (BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
			if (!tb.is_bridge || tb.transport == TransportType::Water) continue;
			/* Length between the ramps, as CheckBridgeAvailability expects it. */
			uint bridge_len = std::max(abs(tb.other_end.x - tb.offset.x), abs(tb.other_end.y - tb.offset.y)) - 1;
			tb.bridge_type = FindFastestBridgeType(bridge_len, tb.bridge_type);
		}
	}

	/* The landscape is only needed when terraforming. */
	if (data.terraform_mode == BlueprintTerraformMode::None) bp.corner_heights.clear();

	return data;
}

/**
 * Check that a (possibly network-received) blueprint only contains offsets
 * within its own bounds and valid values where this file indexes by them.
 * @param bp Blueprint to check.
 * @return True when the blueprint is safe to paste.
 */
static bool BlueprintOffsetsValid(const Blueprint &bp)
{
	auto ok = [&](const TileIndexDiffC &offset) {
		return offset.x >= 0 && offset.y >= 0 && offset.x < bp.width && offset.y < bp.height;
	};

	for (const BlueprintRailTrack &v : bp.rail_tracks) if (!ok(v.offset) || !IsValidTrack(v.track)) return false;
	for (const BlueprintSignal &v : bp.signals) if (!ok(v.offset) || !IsValidTrack(v.track)) return false;
	for (const BlueprintRailDepot &v : bp.rail_depots) if (!ok(v.offset)) return false;
	for (const BlueprintTunnelBridge &v : bp.tunnel_bridges) if (!ok(v.offset) || !ok(v.other_end)) return false;
	for (const BlueprintRoad &v : bp.roads) if (!ok(v.offset)) return false;
	for (const BlueprintRoadDepot &v : bp.road_depots) if (!ok(v.offset)) return false;
	for (const BlueprintCanal &v : bp.canals) if (!ok(v.offset)) return false;
	for (const BlueprintLock &v : bp.locks) if (!ok(v.offset) || !IsValidDiagDirection(v.dir)) return false;
	for (const BlueprintShipDepot &v : bp.ship_depots) if (!ok(v.offset)) return false;
	for (const BlueprintStationTile &v : bp.station_tiles) if (!ok(v.offset) || !IsValidAxis(v.axis)) return false;
	for (const BlueprintRoadStop &v : bp.road_stops) if (!ok(v.offset) || !IsValidAxis(v.axis)) return false;
	for (const BlueprintDock &v : bp.docks) if (!ok(v.offset) || !IsValidDiagDirection(v.dir)) return false;
	for (const BlueprintBuoy &v : bp.buoys) if (!ok(v.offset)) return false;
	for (const BlueprintAirport &v : bp.airports) if (!ok(v.offset)) return false;

	return true;
}

/** Running state while pasting: available money and the accumulated result. */
struct BlueprintPasteState {
	Money money;                              ///< Money still available for further parts.
	CommandCost total{ExpensesType::Construction}; ///< Accumulated cost of the built parts.
	bool out_of_money = false;                ///< Whether building stopped due to lack of money.
	bool any_success = false;                 ///< Whether any part could be built.
};

/** Get the #CommandCost from a command result that may carry additional return values. */
template <typename T>
static const CommandCost &BlueprintCmdCost(const T &result)
{
	if constexpr (std::is_same_v<std::decay_t<T>, CommandCost>) {
		return result;
	} else {
		return std::get<0>(result);
	}
}

/**
 * Build one part of a blueprint: test it, check the money, then execute.
 * Parts that do not fit are skipped without error spam.
 * @return Whether this part was built (or would build, when only testing).
 */
template <Commands Tcmd, typename... Targs>
static bool PasteBuild(BlueprintPasteState &state, DoCommandFlags flags, Targs... args)
{
	if (state.out_of_money) return false;

	auto test_result = Command<Tcmd>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), args...);
	const CommandCost &test_cost = BlueprintCmdCost(test_result);
	if (test_cost.Failed()) return false;

	if (!flags.Test(DoCommandFlag::Execute)) {
		state.total.AddCost(test_cost.GetCost());
		state.any_success = true;
		return true;
	}

	if (test_cost.GetCost() > 0 && state.money < test_cost.GetCost()) {
		state.out_of_money = true;
		return false;
	}

	auto result = Command<Tcmd>::Do(flags, args...);
	const CommandCost &cost = BlueprintCmdCost(result);
	if (cost.Failed()) return false;

	state.money -= cost.GetCost();
	state.total.AddCost(cost.GetCost());
	state.any_success = true;
	return true;
}

/**
 * Paste a blueprint onto the map.
 *
 * Landscape first: depending on the terraform mode, the tile corners of the
 * target area are raised/lowered towards the copied landscape (shifted by the
 * height offset). Then the infrastructure is built; parts that do not fit are
 * silently skipped. The blueprint is expected to be pre-transformed and
 * pre-filtered (see #PrepareBlueprintPaste).
 *
 * @param flags Type of operation.
 * @param origin Northern tile of the target area.
 * @param data Blueprint and landscape adjustment settings.
 * @return The cost of this operation or an error.
 */
CommandCost CmdPasteBlueprint(DoCommandFlags flags, TileIndex origin, const BlueprintPasteData &data)
{
	const Blueprint &bp = data.blueprint;

	if (origin >= Map::Size()) return CMD_ERROR;
	if (bp.width < 1 || bp.height < 1 || bp.width > MAX_BLUEPRINT_DIMENSION || bp.height > MAX_BLUEPRINT_DIMENSION) return CMD_ERROR;
	if (bp.IsEmpty()) return CMD_ERROR;
	if (data.height_offset < -MAX_BLUEPRINT_HEIGHT_OFFSET || data.height_offset > MAX_BLUEPRINT_HEIGHT_OFFSET) return CMD_ERROR;
	if (data.terraform_mode != BlueprintTerraformMode::None && data.terraform_mode != BlueprintTerraformMode::Minimal && data.terraform_mode != BlueprintTerraformMode::Full) return CMD_ERROR;
	if (data.terraform_mode != BlueprintTerraformMode::None && bp.corner_heights.size() != static_cast<size_t>(bp.width + 1) * (bp.height + 1)) return CMD_ERROR;
	if (!BlueprintOffsetsValid(bp)) return CMD_ERROR;

	std::vector<BlueprintStationRect> station_rects = GroupStationTiles(bp);

	BlueprintPasteState state{GetAvailableMoneyForCommand()};

	/* Is the tile at this offset (still) on the map? */
	auto tile_valid = [&](TileIndexDiffC offset) {
		return TileX(origin) + offset.x <= Map::MaxX() && TileY(origin) + offset.y <= Map::MaxY();
	};
	auto target_tile = [&](TileIndexDiffC offset) {
		return TileAddXY(origin, offset.x, offset.y);
	};

	if (data.terraform_mode != BlueprintTerraformMode::None) {
		/* Determine which corners to terraform. */
		std::vector<bool> terraform_corner(static_cast<size_t>(bp.width + 1) * (bp.height + 1), data.terraform_mode == BlueprintTerraformMode::Full);
		if (data.terraform_mode == BlueprintTerraformMode::Minimal) {
			/* Mark the four corners of every tile that receives an element. */
			auto touch = [&](int x, int y) {
				if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
				terraform_corner[y * (bp.width + 1) + x] = true;
				terraform_corner[y * (bp.width + 1) + x + 1] = true;
				terraform_corner[(y + 1) * (bp.width + 1) + x] = true;
				terraform_corner[(y + 1) * (bp.width + 1) + x + 1] = true;
			};

			for (const BlueprintRailTrack &rt : bp.rail_tracks) touch(rt.offset.x, rt.offset.y);
			for (const BlueprintRailDepot &depot : bp.rail_depots) touch(depot.offset.x, depot.offset.y);
			for (const BlueprintRoad &road : bp.roads) touch(road.offset.x, road.offset.y);
			for (const BlueprintRoadDepot &depot : bp.road_depots) touch(depot.offset.x, depot.offset.y);
			for (const BlueprintCanal &canal : bp.canals) touch(canal.offset.x, canal.offset.y);
			for (const BlueprintLock &lock : bp.locks) {
				TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
				touch(lock.offset.x - d.x, lock.offset.y - d.y);
				touch(lock.offset.x, lock.offset.y);
				touch(lock.offset.x + d.x, lock.offset.y + d.y);
			}
			for (const BlueprintShipDepot &depot : bp.ship_depots) {
				touch(depot.offset.x, depot.offset.y);
				touch(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0));
			}
			for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
				/* Only the two end tiles matter; the span stays untouched. */
				touch(tb.offset.x, tb.offset.y);
				touch(tb.other_end.x, tb.other_end.y);
			}
			for (const BlueprintStationRect &rect : station_rects) {
				for (uint dy = 0; dy < rect.h; dy++) {
					for (uint dx = 0; dx < rect.w; dx++) touch(rect.offset.x + dx, rect.offset.y + dy);
				}
			}
			for (const BlueprintRoadStop &stop : bp.road_stops) touch(stop.offset.x, stop.offset.y);
			for (const BlueprintDock &dock : bp.docks) {
				TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
				touch(dock.offset.x, dock.offset.y);
				touch(dock.offset.x + d.x, dock.offset.y + d.y);
			}
			for (const BlueprintAirport &airport : bp.airports) {
				for (uint dy = 0; dy < airport.h; dy++) {
					for (uint dx = 0; dx < airport.w; dx++) touch(airport.offset.x + dx, airport.offset.y + dy);
				}
			}
		}

		/* The copied height at the target's north corner is pasted at the height of the
		 * pointed-at corner, shifted by the height offset; everything else follows. */
		int height_base = TileHeight(origin) + data.height_offset - bp.corner_heights[0];
		int height_limit = _settings_game.construction.map_height_limit;

		/* Terraform commands move a corner by at most one height level, so
		 * sweep the area repeatedly until nothing changes any more. */
		for (uint pass = 0; pass <= MAX_TILE_HEIGHT; pass++) {
			bool changed = false;
			for (uint cy = 0; cy <= bp.height; cy++) {
				for (uint cx = 0; cx <= bp.width; cx++) {
					if (!terraform_corner[cy * (bp.width + 1) + cx]) continue;
					if (TileX(origin) + cx > Map::MaxX() || TileY(origin) + cy > Map::MaxY()) continue;

					TileIndex corner_tile = TileAddXY(origin, cx, cy);
					int wanted = Clamp(bp.corner_heights[cy * (bp.width + 1) + cx] + height_base, 0, height_limit);
					uint current = TileHeight(corner_tile);
					if (static_cast<int>(current) == wanted) continue;

					PasteBuild<Commands::TerraformLand>(state, flags, corner_tile, SLOPE_N, static_cast<int>(current) < wanted);
					if (TileHeight(corner_tile) != current) changed = true;
				}
			}
			if (!changed) break;
		}
	}

	/* Build everything; parts that do not fit are skipped without error spam.
	 * Water before rail/road (locks and canals form the bed for ship depots),
	 * tracks before signals, stations and airports last. */
	for (const BlueprintCanal &canal : bp.canals) {
		if (!tile_valid(canal.offset)) continue;
		PasteBuild<Commands::BuildCanal>(state, flags, target_tile(canal.offset), target_tile(canal.offset), WaterClass::Canal, false);
	}
	for (const BlueprintLock &lock : bp.locks) {
		if (!tile_valid(lock.offset)) continue;
		PasteBuild<Commands::BuildLock>(state, flags, target_tile(lock.offset));
	}
	for (const BlueprintShipDepot &depot : bp.ship_depots) {
		if (!tile_valid(depot.offset)) continue;
		PasteBuild<Commands::BuildShipDepot>(state, flags, target_tile(depot.offset), depot.axis);
	}

	for (const BlueprintRailTrack &rt : bp.rail_tracks) {
		if (!tile_valid(rt.offset)) continue;
		PasteBuild<Commands::BuildRail>(state, flags, target_tile(rt.offset), rt.railtype, rt.track, false);
	}
	for (const BlueprintSignal &sig : bp.signals) {
		if (!tile_valid(sig.offset)) continue;
		Trackdir td = TrackToTrackdir(sig.track);
		uint8_t signals_copy = (sig.along ? SignalAlongTrackdir(td) : 0) | (sig.against ? SignalAgainstTrackdir(td) : 0);
		if (signals_copy == 0) continue;
		PasteBuild<Commands::BuildSignal>(state, flags, target_tile(sig.offset), sig.track, sig.type, sig.variant,
				false, false, false, SignalType::Block, SignalType::Block, 0, signals_copy);
	}
	for (const BlueprintRailDepot &depot : bp.rail_depots) {
		if (!tile_valid(depot.offset)) continue;
		PasteBuild<Commands::BuildRailDepot>(state, flags, target_tile(depot.offset), depot.railtype, depot.dir);
	}

	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		if (!tile_valid(tb.offset) || !tile_valid(tb.other_end)) continue;

		/* The build commands take the rail type and the road type separately;
		 * only the one that matches the transport type is looked at. */
		RailType railtype = tb.transport == TransportType::Rail ? tb.railtype : INVALID_RAILTYPE;
		RoadType roadtype = INVALID_ROADTYPE;
		if (tb.transport == TransportType::Road) roadtype = tb.roadtype != INVALID_ROADTYPE ? tb.roadtype : tb.tramtype;

		if (tb.is_bridge) {
			PasteBuild<Commands::BuildBridge>(state, flags, target_tile(tb.other_end), target_tile(tb.offset), tb.transport, tb.bridge_type, railtype, roadtype);
		} else {
			PasteBuild<Commands::BuildTunnel>(state, flags, target_tile(tb.offset), tb.transport, railtype, roadtype);
		}

		/* Bridges/tunnels carrying both road and tram: add the second type across the span. */
		if (tb.transport == TransportType::Road && tb.roadtype != INVALID_ROADTYPE && tb.tramtype != INVALID_ROADTYPE) {
			Axis axis = tb.offset.x == tb.other_end.x ? Axis::Y : Axis::X;
			PasteBuild<Commands::BuildRoad>(state, flags, target_tile(tb.offset), AxisToRoadBits(axis), tb.tramtype, DisallowedRoadDirections{}, TownID::Invalid());
		}
	}

	for (const BlueprintRoad &road : bp.roads) {
		if (!tile_valid(road.offset)) continue;
		PasteBuild<Commands::BuildRoad>(state, flags, target_tile(road.offset), road.bits, road.roadtype, DisallowedRoadDirections{}, TownID::Invalid());
	}
	for (const BlueprintRoadDepot &depot : bp.road_depots) {
		if (!tile_valid(depot.offset)) continue;
		PasteBuild<Commands::BuildRoadDepot>(state, flags, target_tile(depot.offset), depot.roadtype, depot.dir);
	}

	/* Parts of one original station are joined into one new station again. */
	std::map<StationID, StationID> new_station_ids;
	auto join_target = [&](StationID original) {
		auto it = new_station_ids.find(original);
		return it == new_station_ids.end() ? NEW_STATION : it->second;
	};
	auto register_new_station = [&](StationID original, TileIndexDiffC offset) {
		if (!flags.Test(DoCommandFlag::Execute) || new_station_ids.contains(original)) return;
		TileIndex tile = target_tile(offset);
		if (IsTileType(tile, TileType::Station)) new_station_ids[original] = GetStationIndex(tile);
	};

	for (const BlueprintStationRect &rect : station_rects) {
		if (!tile_valid(rect.offset)) continue;
		if (rect.is_waypoint) {
			/* Waypoints can only be placed on existing straight tracks; lay those first. */
			Track track = AxisToTrack(rect.axis);
			for (uint dy = 0; dy < rect.h; dy++) {
				for (uint dx = 0; dx < rect.w; dx++) {
					TileIndexDiffC tile_offset = {static_cast<int16_t>(rect.offset.x + dx), static_cast<int16_t>(rect.offset.y + dy)};
					if (!tile_valid(tile_offset)) continue;
					PasteBuild<Commands::BuildRail>(state, flags, target_tile(tile_offset), rect.railtype, track, false);
				}
			}

			/* One waypoint command covers one tile along the track (spanning parallel
			 * tracks), so build the rect slice by slice along its axis. */
			uint slices = rect.axis == Axis::X ? rect.w : rect.h;
			uint8_t slice_w = rect.axis == Axis::X ? 1 : static_cast<uint8_t>(rect.w);
			uint8_t slice_h = rect.axis == Axis::X ? static_cast<uint8_t>(rect.h) : 1;
			for (uint i = 0; i < slices; i++) {
				TileIndexDiffC slice = {static_cast<int16_t>(rect.offset.x + (rect.axis == Axis::X ? i : 0)),
						static_cast<int16_t>(rect.offset.y + (rect.axis == Axis::X ? 0 : i))};
				if (!tile_valid(slice)) continue;
				StationID join = join_target(rect.station);
				bool built = PasteBuild<Commands::BuildRailWaypoint>(state, flags, target_tile(slice), rect.axis, slice_w, slice_h,
						STAT_CLASS_WAYP, 0, join, true);
				if (!built && join != NEW_STATION) {
					/* Joining may be impossible (e.g. station spread); build separately. */
					built = PasteBuild<Commands::BuildRailWaypoint>(state, flags, target_tile(slice), rect.axis, slice_w, slice_h,
							STAT_CLASS_WAYP, 0, NEW_STATION, true);
				}
				if (built) register_new_station(rect.station, slice);
			}
		} else {
			uint8_t numtracks = static_cast<uint8_t>(rect.axis == Axis::X ? rect.h : rect.w);
			uint8_t plat_len = static_cast<uint8_t>(rect.axis == Axis::X ? rect.w : rect.h);
			StationID join = join_target(rect.station);
			bool built = PasteBuild<Commands::BuildRailStation>(state, flags, target_tile(rect.offset), rect.railtype, rect.axis, numtracks, plat_len,
					STAT_CLASS_DFLT, 0, join, true);
			if (!built && join != NEW_STATION) {
				/* Joining may be impossible (e.g. distant join disabled); build separately. */
				built = PasteBuild<Commands::BuildRailStation>(state, flags, target_tile(rect.offset), rect.railtype, rect.axis, numtracks, plat_len,
						STAT_CLASS_DFLT, 0, NEW_STATION, true);
			}
			if (built) register_new_station(rect.station, rect.offset);
		}
	}

	for (const BlueprintRoadStop &stop : bp.road_stops) {
		if (!tile_valid(stop.offset)) continue;
		RoadType rt = stop.roadtype != INVALID_ROADTYPE ? stop.roadtype : stop.tramtype;
		DiagDirection ddir = stop.drive_through ? AxisToDiagDir(stop.axis) : stop.dir;
		StationID join = join_target(stop.station);
		bool built;

		if (stop.type == StationType::RoadWaypoint) {
			/* Road waypoints can only be placed on existing road; lay that first. */
			if (stop.roadtype != INVALID_ROADTYPE) {
				PasteBuild<Commands::BuildRoad>(state, flags, target_tile(stop.offset), AxisToRoadBits(stop.axis), stop.roadtype, DisallowedRoadDirections{}, TownID::Invalid());
			}
			if (stop.tramtype != INVALID_ROADTYPE) {
				PasteBuild<Commands::BuildRoad>(state, flags, target_tile(stop.offset), AxisToRoadBits(stop.axis), stop.tramtype, DisallowedRoadDirections{}, TownID::Invalid());
			}
			built = PasteBuild<Commands::BuildRoadWaypoint>(state, flags, target_tile(stop.offset), stop.axis, 1, 1,
					ROADSTOP_CLASS_WAYP, 0, join, true);
			if (!built && join != NEW_STATION) {
				built = PasteBuild<Commands::BuildRoadWaypoint>(state, flags, target_tile(stop.offset), stop.axis, 1, 1,
						ROADSTOP_CLASS_WAYP, 0, NEW_STATION, true);
			}
		} else {
			if (!IsValidDiagDirection(ddir)) continue;
			RoadStopType stop_type = stop.type == StationType::Truck ? RoadStopType::Truck : RoadStopType::Bus;
			built = PasteBuild<Commands::BuildRoadStop>(state, flags, target_tile(stop.offset), 1, 1, stop_type, stop.drive_through,
					ddir, rt, ROADSTOP_CLASS_DFLT, 0, join, true);
			if (!built && join != NEW_STATION) {
				built = PasteBuild<Commands::BuildRoadStop>(state, flags, target_tile(stop.offset), 1, 1, stop_type, stop.drive_through,
						ddir, rt, ROADSTOP_CLASS_DFLT, 0, NEW_STATION, true);
			}
			/* Drive-through stops carrying both road and tram: add the second type. */
			if (built && stop.drive_through && stop.roadtype != INVALID_ROADTYPE && stop.tramtype != INVALID_ROADTYPE) {
				PasteBuild<Commands::BuildRoad>(state, flags, target_tile(stop.offset), AxisToRoadBits(stop.axis), stop.tramtype, DisallowedRoadDirections{}, TownID::Invalid());
			}
		}
		if (built) register_new_station(stop.station, stop.offset);
	}

	for (const BlueprintDock &dock : bp.docks) {
		if (!tile_valid(dock.offset)) continue;
		StationID join = join_target(dock.station);
		bool built = PasteBuild<Commands::BuildDock>(state, flags, target_tile(dock.offset), join, true);
		if (!built && join != NEW_STATION) {
			built = PasteBuild<Commands::BuildDock>(state, flags, target_tile(dock.offset), NEW_STATION, true);
		}
		if (built) register_new_station(dock.station, dock.offset);
	}
	for (const BlueprintBuoy &buoy : bp.buoys) {
		if (!tile_valid(buoy.offset)) continue;
		PasteBuild<Commands::BuildBuoy>(state, flags, target_tile(buoy.offset));
	}

	for (const BlueprintAirport &airport : bp.airports) {
		if (!tile_valid(airport.offset)) continue;
		StationID join = join_target(airport.station);
		bool built = PasteBuild<Commands::BuildAirport>(state, flags, target_tile(airport.offset), airport.type, airport.layout, join, true);
		if (!built && join != NEW_STATION) {
			built = PasteBuild<Commands::BuildAirport>(state, flags, target_tile(airport.offset), airport.type, airport.layout, NEW_STATION, true);
		}
		if (built) register_new_station(airport.station, airport.offset);
	}

	if (!state.any_success) return CMD_ERROR;
	return state.total;
}
