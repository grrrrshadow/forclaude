/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file air_map.h Map accessors of the modular airports.
 *
 * The player (17. 11.): "I do not want a runway to the north, I do not want to place a whole airport. What I want:
 * the buildings separately, an asphalt taxiway and a runway, plus heliports separately, so that I build the airport
 * myself. Which way the runway goes I do not care." So an airport built tile by tile, from the idea of J0anJosep's
 * MultitileAirports branch (hra/3d/PLAN-MODULARNI-LETISTE.md in grrrrf).
 *
 * A modular airport tile is a station tile of its own type (StationType::ModularAirport): the old fixed airports'
 * code -- their tile graphics, NewGRF airport tiles, animation, the aircraft's state machines, scripts' airport
 * queries -- never sees it (IsAirport() is false for it). Its bits:
 *   m3 bits 0..3  the surface (AirportSurface);  m3 bits 4..7 the random bits of every station tile
 *   m4            the building (the index of its look, ModularAirportBuildings; later its 3D object)
 *   m5 bits 0..3  the kind of the tile (ModularAirportKind)
 *   m5 bit  4     a runway's axis
 *   m5 bit  5     a runway end at the higher coordinate of its axis (else the lower one)
 *   m5 bits 4..5  a hangar's entrance, a building's front (DiagDirection)
 *   m8 bits 0..5  the taxiing tracks on the tile (TrackBits, as a railway's)
 *   m8 bits 6..11 the tracks reserved by an aircraft (as a railway's path reservation)
 *   m8 bit  12    the runway reserved by an aircraft landing or taking off
 */

#ifndef AIR_MAP_H
#define AIR_MAP_H

#include "air_type.h"
#include "station_map.h"
#include "track_func.h"

/**
 * Is this station tile a tile of a modular airport?
 * @param t the tile
 * @pre IsTileType(t, TileType::Station)
 */
inline bool IsModularAirport(Tile t)
{
	return GetStationType(t) == StationType::ModularAirport;
}

/**
 * Is this tile a tile of a modular airport?
 * @param t the tile
 */
inline bool IsModularAirportTile(Tile t)
{
	return IsTileType(t, TileType::Station) && IsModularAirport(t);
}

/** @pre IsModularAirportTile(t) */
inline ModularAirportKind GetModularAirportKind(Tile t)
{
	assert(IsModularAirportTile(t));
	return static_cast<ModularAirportKind>(GB(t.m5(), 0, 4));
}

/** @pre IsModularAirportTile(t) */
inline void SetModularAirportKind(Tile t, ModularAirportKind kind)
{
	assert(IsModularAirportTile(t));
	SB(t.m5(), 0, 4, to_underlying(kind));
}

/** Is it a runway's tile, an end or the middle? @pre IsModularAirportTile(t) */
inline bool IsModularAirportRunway(Tile t)
{
	ModularAirportKind kind = GetModularAirportKind(t);
	return kind == ModularAirportKind::RunwayEnd || kind == ModularAirportKind::RunwayMiddle;
}

/** @pre IsModularAirportTile(t) */
inline AirportSurface GetModularAirportSurface(Tile t)
{
	assert(IsModularAirportTile(t));
	return static_cast<AirportSurface>(GB(t.m3(), 0, 4));
}

/** @pre IsModularAirportTile(t) */
inline void SetModularAirportSurface(Tile t, AirportSurface surface)
{
	assert(IsModularAirportTile(t));
	SB(t.m3(), 0, 4, to_underlying(surface));
}

/** A runway's axis. @pre IsModularAirportRunway(t) */
inline Axis GetModularAirportRunwayAxis(Tile t)
{
	assert(IsModularAirportRunway(t));
	return static_cast<Axis>(GB(t.m5(), 4, 1));
}

/** Is a runway's end at the higher coordinate of its axis? @pre GetModularAirportKind(t) == ModularAirportKind::RunwayEnd */
inline bool IsModularAirportRunwayHighEnd(Tile t)
{
	assert(GetModularAirportKind(t) == ModularAirportKind::RunwayEnd);
	return HasBit(t.m5(), 5);
}

/** Set a runway tile's axis and which end it is (ignored in the middle). @pre IsModularAirportRunway(t) */
inline void SetModularAirportRunway(Tile t, Axis axis, bool high_end)
{
	assert(IsModularAirportRunway(t));
	SB(t.m5(), 4, 1, to_underlying(axis));
	AssignBit(t.m5(), 5, high_end);
}

/** A hangar's entrance or a building's front. @pre the tile is a hangar or a building */
inline DiagDirection GetModularAirportDirection(Tile t)
{
	assert(GetModularAirportKind(t) == ModularAirportKind::Hangar || GetModularAirportKind(t) == ModularAirportKind::Building);
	return static_cast<DiagDirection>(GB(t.m5(), 4, 2));
}

/** @pre the tile is a hangar or a building */
inline void SetModularAirportDirection(Tile t, DiagDirection dir)
{
	assert(GetModularAirportKind(t) == ModularAirportKind::Hangar || GetModularAirportKind(t) == ModularAirportKind::Building);
	SB(t.m5(), 4, 2, to_underlying(dir));
}

/** The building's look. @pre GetModularAirportKind(t) == ModularAirportKind::Building */
inline uint8_t GetModularAirportBuilding(Tile t)
{
	assert(GetModularAirportKind(t) == ModularAirportKind::Building);
	return t.m4();
}

/** @pre GetModularAirportKind(t) == ModularAirportKind::Building */
inline void SetModularAirportBuilding(Tile t, uint8_t building)
{
	assert(GetModularAirportKind(t) == ModularAirportKind::Building);
	t.m4() = building;
}

/** The taxiing tracks on the tile. @pre IsModularAirportTile(t) */
inline TrackBits GetModularAirportTracks(Tile t)
{
	assert(IsModularAirportTile(t));
	return static_cast<TrackBits>(GB(t.m8(), 0, 6));
}

/** @pre IsModularAirportTile(t) */
inline void SetModularAirportTracks(Tile t, TrackBits tracks)
{
	assert(IsModularAirportTile(t));
	SB(t.m8(), 0, 6, tracks.base());
}

/** The taxiing tracks reserved by an aircraft. @pre IsModularAirportTile(t) */
inline TrackBits GetModularAirportReservedTracks(Tile t)
{
	assert(IsModularAirportTile(t));
	return static_cast<TrackBits>(GB(t.m8(), 6, 6));
}

/** @pre IsModularAirportTile(t) */
inline void SetModularAirportReservedTracks(Tile t, TrackBits tracks)
{
	assert(IsModularAirportTile(t));
	SB(t.m8(), 6, 6, tracks.base());
}

/** Is the runway this tile is of reserved? @pre IsModularAirportRunway(t) */
inline bool HasModularAirportRunwayReservation(Tile t)
{
	assert(IsModularAirportRunway(t));
	return HasBit(t.m8(), 12);
}

/** @pre IsModularAirportRunway(t) */
inline void SetModularAirportRunwayReservation(Tile t, bool reserved)
{
	assert(IsModularAirportRunway(t));
	AssignBit(t.m8(), 12, reserved);
}

/**
 * Make a tile a modular airport's tile.
 * @param t the tile
 * @param o its owner
 * @param sid its station
 * @param kind its kind
 * @param surface its surface
 */
inline void MakeModularAirport(Tile t, Owner o, StationID sid, ModularAirportKind kind, AirportSurface surface)
{
	MakeStation(t, o, sid, StationType::ModularAirport, to_underlying(kind));
	SB(t.m3(), 0, 4, to_underlying(surface));
}

#endif /* AIR_MAP_H */
