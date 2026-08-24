/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_cmd.h Command definitions related to pasting blueprints. */

#ifndef BLUEPRINT_CMD_H
#define BLUEPRINT_CMD_H

#include "blueprint.h"
#include "command_type.h"
#include "misc/endian_buffer.hpp"

CommandCost CmdPasteBlueprint(DoCommandFlags flags, TileIndex origin, const BlueprintPasteData &data);

DEF_CMD_TRAIT(Commands::PasteBlueprint, CmdPasteBlueprint, CommandFlag::NoTest, CommandType::LandscapeConstruction) // the landscape may change between test and execution

/* Serialization of a blueprint for the paste command. */

/** Maximum number of items read back per blueprint vector; large enough for the corner heights of a maximum-size blueprint. */
static const uint32_t MAX_BLUEPRINT_VECTOR_LENGTH = (MAX_BLUEPRINT_DIMENSION + 1) * (MAX_BLUEPRINT_DIMENSION + 1);

/** Serialized size (in bytes) above which a paste command does not fit a network packet any more. */
static const size_t MAX_BLUEPRINT_COMMAND_SIZE = 30000;

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const TileIndexDiffC &data)
{
	return buffer << data.x << data.y;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, TileIndexDiffC &data)
{
	return buffer >> data.x >> data.y;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintRailTrack &data)
{
	return buffer << data.offset << data.track << data.railtype;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintRailTrack &data)
{
	return buffer >> data.offset >> data.track >> data.railtype;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintSignal &data)
{
	return buffer << data.offset << data.track << data.type << data.variant << data.along << data.against;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintSignal &data)
{
	return buffer >> data.offset >> data.track >> data.type >> data.variant >> data.along >> data.against;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintRailDepot &data)
{
	return buffer << data.offset << data.dir << data.railtype;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintRailDepot &data)
{
	return buffer >> data.offset >> data.dir >> data.railtype;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintTunnelBridge &data)
{
	return buffer << data.offset << data.other_end << data.transport << data.is_bridge << data.bridge_type << data.railtype << data.roadtype << data.tramtype;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintTunnelBridge &data)
{
	return buffer >> data.offset >> data.other_end >> data.transport >> data.is_bridge >> data.bridge_type >> data.railtype >> data.roadtype >> data.tramtype;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintRoad &data)
{
	return buffer << data.offset << data.bits << data.roadtype;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintRoad &data)
{
	return buffer >> data.offset >> data.bits >> data.roadtype;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintRoadDepot &data)
{
	return buffer << data.offset << data.dir << data.roadtype;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintRoadDepot &data)
{
	return buffer >> data.offset >> data.dir >> data.roadtype;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintCanal &data)
{
	return buffer << data.offset;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintCanal &data)
{
	return buffer >> data.offset;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintLock &data)
{
	return buffer << data.offset << data.dir;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintLock &data)
{
	return buffer >> data.offset >> data.dir;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintShipDepot &data)
{
	return buffer << data.offset << data.axis;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintShipDepot &data)
{
	return buffer >> data.offset >> data.axis;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintStationTile &data)
{
	return buffer << data.offset << data.is_waypoint << data.axis << data.railtype << data.station;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintStationTile &data)
{
	return buffer >> data.offset >> data.is_waypoint >> data.axis >> data.railtype >> data.station;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintRoadStop &data)
{
	return buffer << data.offset << data.type << data.drive_through << data.dir << data.axis << data.roadtype << data.tramtype << data.station;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintRoadStop &data)
{
	return buffer >> data.offset >> data.type >> data.drive_through >> data.dir >> data.axis >> data.roadtype >> data.tramtype >> data.station;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintDock &data)
{
	return buffer << data.offset << data.dir << data.station;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintDock &data)
{
	return buffer >> data.offset >> data.dir >> data.station;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintBuoy &data)
{
	return buffer << data.offset;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintBuoy &data)
{
	return buffer >> data.offset;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintAirport &data)
{
	return buffer << data.offset << data.type << data.layout << data.w << data.h << data.station;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintAirport &data)
{
	return buffer >> data.offset >> data.type >> data.layout >> data.w >> data.h >> data.station;
}

/** Write a length-prefixed blueprint vector to a command buffer. */
template <typename Tcont, typename Titer, typename T>
inline void WriteBlueprintVector(EndianBufferWriter<Tcont, Titer> &buffer, const std::vector<T> &data)
{
	buffer << static_cast<uint32_t>(data.size());
	for (const T &item : data) buffer << item;
}

/** Read a length-prefixed blueprint vector from a command buffer; the length is capped against malicious data. */
template <typename T>
inline void ReadBlueprintVector(EndianBufferReader &buffer, std::vector<T> &data)
{
	uint32_t count = 0;
	buffer >> count;
	count = std::min(count, MAX_BLUEPRINT_VECTOR_LENGTH);
	data.clear();
	for (uint32_t i = 0; i < count; i++) {
		T item{};
		buffer >> item;
		data.push_back(item);
	}
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const Blueprint &data)
{
	buffer << data.width << data.height << data.name;
	WriteBlueprintVector(buffer, data.rail_tracks);
	WriteBlueprintVector(buffer, data.signals);
	WriteBlueprintVector(buffer, data.rail_depots);
	WriteBlueprintVector(buffer, data.tunnel_bridges);
	WriteBlueprintVector(buffer, data.roads);
	WriteBlueprintVector(buffer, data.road_depots);
	WriteBlueprintVector(buffer, data.canals);
	WriteBlueprintVector(buffer, data.locks);
	WriteBlueprintVector(buffer, data.ship_depots);
	WriteBlueprintVector(buffer, data.station_tiles);
	WriteBlueprintVector(buffer, data.road_stops);
	WriteBlueprintVector(buffer, data.docks);
	WriteBlueprintVector(buffer, data.buoys);
	WriteBlueprintVector(buffer, data.airports);
	WriteBlueprintVector(buffer, data.corner_heights);
	return buffer;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, Blueprint &data)
{
	buffer >> data.width >> data.height >> data.name;
	ReadBlueprintVector(buffer, data.rail_tracks);
	ReadBlueprintVector(buffer, data.signals);
	ReadBlueprintVector(buffer, data.rail_depots);
	ReadBlueprintVector(buffer, data.tunnel_bridges);
	ReadBlueprintVector(buffer, data.roads);
	ReadBlueprintVector(buffer, data.road_depots);
	ReadBlueprintVector(buffer, data.canals);
	ReadBlueprintVector(buffer, data.locks);
	ReadBlueprintVector(buffer, data.ship_depots);
	ReadBlueprintVector(buffer, data.station_tiles);
	ReadBlueprintVector(buffer, data.road_stops);
	ReadBlueprintVector(buffer, data.docks);
	ReadBlueprintVector(buffer, data.buoys);
	ReadBlueprintVector(buffer, data.airports);
	ReadBlueprintVector(buffer, data.corner_heights);
	return buffer;
}

template <typename Tcont, typename Titer>
inline EndianBufferWriter<Tcont, Titer> &operator <<(EndianBufferWriter<Tcont, Titer> &buffer, const BlueprintPasteData &data)
{
	return buffer << data.blueprint << data.terraform_mode << data.height_offset;
}

inline EndianBufferReader &operator >>(EndianBufferReader &buffer, BlueprintPasteData &data)
{
	return buffer >> data.blueprint >> data.terraform_mode >> data.height_offset;
}

#endif /* BLUEPRINT_CMD_H */
