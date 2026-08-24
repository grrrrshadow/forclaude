/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.h Blueprint (copy and paste of infrastructure) data structures. */

#ifndef BLUEPRINT_H
#define BLUEPRINT_H

#include "bridge.h"
#include "direction_type.h"
#include "map_type.h"
#include "rail_type.h"
#include "road_type.h"
#include "signal_type.h"
#include "station_type.h"
#include "tilearea_type.h"
#include "track_type.h"
#include "transport_type.h"

/** Number of available blueprint slots. */
static const uint NUM_BLUEPRINT_SLOTS = 8;

/** Maximum size of a blueprint along either axis. */
static const uint MAX_BLUEPRINT_DIMENSION = 255;

/** Maximum absolute value of the paste height offset. */
static const int8_t MAX_BLUEPRINT_HEIGHT_OFFSET = 8;

/** The maximum length of a blueprint name in characters including '\0'. */
static const uint MAX_LENGTH_BLUEPRINT_NAME_CHARS = 32;

/** How the landscape is adjusted while pasting a blueprint. */
enum class BlueprintTerraformMode : uint8_t {
	None,    ///< Do not terraform; only paste what fits the current landscape.
	Minimal, ///< Terraform only the tiles that receive blueprint elements.
	Full,    ///< Terraform the whole pasted area to match the copied landscape.
};

/**
 * Options controlling how a blueprint is pasted.
 *
 * The transformation is an element of the dihedral group D4, stored as
 * "reflect against the NW-SE axis first (if #reflected), then rotate
 * #rotation quarter turns anticlockwise".
 */
struct BlueprintPasteOptions {
	bool paste_rail = true;        ///< Paste rail transport infrastructure.
	bool paste_road = true;        ///< Paste road transport infrastructure.
	bool paste_water = true;       ///< Paste water transport infrastructure.
	bool paste_air = true;         ///< Paste air transport infrastructure.
	bool convert_railtype = false; ///< Convert rail to the current rail type when pasting.
	bool mirror_signals = false;   ///< Mirror signals when pasting.
	bool upgrade_bridges = false;  ///< Upgrade bridges when pasting.
	bool with_stations = true;     ///< Paste stations and waypoints too.
	BlueprintTerraformMode terraform_mode = BlueprintTerraformMode::None; ///< Landscape adjustment mode.
	uint8_t rotation = 0;          ///< Quarter turns anticlockwise, applied after the reflection.
	bool reflected = false;        ///< Whether to reflect against the NW-SE axis before rotating.
	int8_t height_offset = 0;      ///< Height offset applied when pasting.

	/**
	 * Compose an additional rotation onto the current transformation.
	 * @param quarter_turns_ccw Number of quarter turns anticlockwise (may be negative).
	 */
	void AddRotation(int quarter_turns_ccw)
	{
		this->rotation = static_cast<uint8_t>((this->rotation + quarter_turns_ccw + 4) % 4);
	}

	/**
	 * Compose an additional reflection onto the current transformation.
	 * Any reflection axis of D4 can be written as "reflect against NW-SE, then rotate".
	 * @param axis_rotation Quarter turns anticlockwise the reflection axis is rotated relative to the NW-SE axis times two (0 = NW-SE, 2 = NE-SW).
	 */
	void AddReflection(int axis_rotation)
	{
		/* F_axis = R^axis_rotation * F  and  F * R^r = R^(-r) * F, hence
		 * F_axis * (R^r * F^f) = R^((axis_rotation - r) mod 4) * F^(1 - f). */
		this->rotation = static_cast<uint8_t>((axis_rotation - this->rotation + 4) % 4);
		this->reflected = !this->reflected;
	}

	/** Reset the transformation to the identity. */
	void ResetTransformation()
	{
		this->rotation = 0;
		this->reflected = false;
	}
};

/** A plain rail track piece (also the rail part of a level crossing). */
struct BlueprintRailTrack {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	Track track;           ///< Track piece on the tile.
	RailType railtype;     ///< Rail type of the track.
};

/** Signals on one track piece. */
struct BlueprintSignal {
	TileIndexDiffC offset;  ///< Tile offset relative to the blueprint origin.
	Track track;            ///< Track piece the signals are on.
	SignalType type;        ///< Type of the signals.
	SignalVariant variant;  ///< Electric / semaphore.
	bool along;             ///< Signal along the track direction of #track.
	bool against;           ///< Signal against the track direction of #track.
};

/** A rail depot. */
struct BlueprintRailDepot {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	DiagDirection dir;     ///< Direction the depot exits to.
	RailType railtype;     ///< Rail type of the depot.
};

/** A bridge or tunnel; both ends lie within the copied area. */
struct BlueprintTunnelBridge {
	TileIndexDiffC offset;    ///< Tile offset of the northern end.
	TileIndexDiffC other_end; ///< Tile offset of the southern end.
	TransportType transport;  ///< Rail / road / water (aqueduct; bridge only).
	bool is_bridge;           ///< True for bridges, false for tunnels.
	BridgeType bridge_type;   ///< Bridge type (only valid when #is_bridge).
	RailType railtype;        ///< Rail type (only valid for rail transport).
	RoadType roadtype;        ///< Road type (only valid for road transport, may be INVALID_ROADTYPE).
	RoadType tramtype;        ///< Tram type (only valid for road transport, may be INVALID_ROADTYPE).
};

/** Road or tram bits on one tile (also the road part of a level crossing). */
struct BlueprintRoad {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	RoadBits bits;         ///< Road pieces on the tile.
	RoadType roadtype;     ///< Road or tram type of the pieces.
};

/** A road depot. */
struct BlueprintRoadDepot {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	DiagDirection dir;     ///< Direction the depot exits to.
	RoadType roadtype;     ///< Road or tram type of the depot.
};

/** A canal tile. */
struct BlueprintCanal {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
};

/** A lock (anchored at its middle part). */
struct BlueprintLock {
	TileIndexDiffC offset; ///< Tile offset of the middle part.
	DiagDirection dir;     ///< Direction of the lock.
};

/** A ship depot (anchored at its northern part). */
struct BlueprintShipDepot {
	TileIndexDiffC offset; ///< Tile offset of the northern part.
	Axis axis;             ///< Axis of the depot.
};

/** One tile of a rail station or rail waypoint. */
struct BlueprintStationTile {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	bool is_waypoint;      ///< True for waypoint tiles, false for station tiles.
	Axis axis;             ///< Axis of the platform.
	RailType railtype;     ///< Rail type of the platform.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
};

/** A road stop (bus / truck / road waypoint). */
struct BlueprintRoadStop {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	StationType type;      ///< StationType::Bus, StationType::Truck or StationType::RoadWaypoint.
	bool drive_through;    ///< True for drive-through stops.
	DiagDirection dir;     ///< Entrance direction (bay stops only).
	Axis axis;             ///< Axis (drive-through stops only).
	RoadType roadtype;     ///< Road type on the stop (may be INVALID_ROADTYPE).
	RoadType tramtype;     ///< Tram type on the stop (may be INVALID_ROADTYPE).
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
};

/** A dock (anchored at its land part). */
struct BlueprintDock {
	TileIndexDiffC offset; ///< Tile offset of the land part.
	DiagDirection dir;     ///< Direction of the dock.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
};

/** A buoy. */
struct BlueprintBuoy {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
};

/** An airport (anchored at its northern tile). */
struct BlueprintAirport {
	TileIndexDiffC offset; ///< Tile offset of the northern airport tile.
	uint8_t type;          ///< Airport type.
	uint8_t layout;        ///< Airport layout (rotation).
	uint16_t w;            ///< Size of the airport along the X axis.
	uint16_t h;            ///< Size of the airport along the Y axis.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
};

/** Contents of one blueprint: infrastructure copied from a rectangular map area. */
struct Blueprint {
	uint16_t width = 0;  ///< Size of the copied area along the X axis.
	uint16_t height = 0; ///< Size of the copied area along the Y axis.
	std::string name;    ///< Player-given name; empty when never named. Not counted as content by #IsEmpty.

	std::vector<BlueprintRailTrack> rail_tracks;
	std::vector<BlueprintSignal> signals;
	std::vector<BlueprintRailDepot> rail_depots;
	std::vector<BlueprintTunnelBridge> tunnel_bridges;
	std::vector<BlueprintRoad> roads;
	std::vector<BlueprintRoadDepot> road_depots;
	std::vector<BlueprintCanal> canals;
	std::vector<BlueprintLock> locks;
	std::vector<BlueprintShipDepot> ship_depots;
	std::vector<BlueprintStationTile> station_tiles;
	std::vector<BlueprintRoadStop> road_stops;
	std::vector<BlueprintDock> docks;
	std::vector<BlueprintBuoy> buoys;
	std::vector<BlueprintAirport> airports;

	/** Heights of the tile corners of the copied area, (width + 1) x (height + 1) values, index = y * (width + 1) + x. */
	std::vector<uint8_t> corner_heights;

	uint CountItems() const;
	void Clear();

	/** Is there anything stored in this blueprint? */
	bool IsEmpty() const { return this->CountItems() == 0; }
};

/**
 * Everything a paste command needs: a blueprint that already has the
 * transformation, the paste filters and the rail/signal/bridge options
 * applied, plus the landscape adjustment settings.
 */
struct BlueprintPasteData {
	Blueprint blueprint;                                                  ///< Pre-transformed and pre-filtered blueprint.
	BlueprintTerraformMode terraform_mode = BlueprintTerraformMode::None; ///< Landscape adjustment mode.
	int8_t height_offset = 0;                                             ///< Height offset applied when pasting.
};

/** One rectangle of equal station tiles, used to rebuild stations/waypoints. */
struct BlueprintStationRect {
	TileIndexDiffC offset; ///< Northern tile of the rectangle.
	uint16_t w;            ///< Extent along the X axis.
	uint16_t h;            ///< Extent along the Y axis.
	bool is_waypoint;      ///< True for waypoints.
	Axis axis;             ///< Platform axis.
	RailType railtype;     ///< Rail type.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
};

Blueprint &GetBlueprint(uint slot);
uint CopyAreaToBlueprint(TileArea area, Blueprint &blueprint);
std::vector<BlueprintStationRect> GroupStationTiles(const Blueprint &blueprint);

TileIndexDiffC TransformBlueprintPoint(TileIndexDiffC point, uint16_t bounds_x, uint16_t bounds_y, uint8_t rotation, bool reflected);
Track TransformBlueprintTrack(Track track, uint8_t rotation, bool reflected);
DiagDirection TransformBlueprintDiagDir(DiagDirection dir, uint8_t rotation, bool reflected);
Axis TransformBlueprintAxis(Axis axis, uint8_t rotation);
RoadBits TransformBlueprintRoadBits(RoadBits bits, uint8_t rotation, bool reflected);
Blueprint TransformBlueprint(const Blueprint &blueprint, uint8_t rotation, bool reflected);
BlueprintPasteData PrepareBlueprintPaste(const Blueprint &blueprint, const BlueprintPasteOptions &options, RailType convert_to);

#endif /* BLUEPRINT_H */
