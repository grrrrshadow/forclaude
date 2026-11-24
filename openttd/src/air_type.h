/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file air_type.h Types of the modular airports' tiles (air_map.h). */

#ifndef AIR_TYPE_H
#define AIR_TYPE_H

#include <cstdint>

/** The kinds of a modular airport's tiles. */
enum class ModularAirportKind : uint8_t {
	Taxiway = 0, ///< Asphalt or gravel with taxiing tracks.
	RunwayEnd = 1, ///< One end of a runway.
	RunwayMiddle = 2, ///< A runway between its ends.
	Stand = 3, ///< Where an aircraft stops and loads.
	Helipad = 4, ///< A helicopter's pad on the ground.
	Heliport = 5, ///< A helicopter's pad on a building.
	Hangar = 6, ///< The aircraft's depot.
	Building = 7, ///< A building: terminal, tower... (only the catchment and the look).
	End, ///< End marker.
};

/** The surface of a modular airport's tile. */
enum class AirportSurface : uint8_t {
	Asphalt = 0, ///< Asphalt.
	Gravel = 1, ///< Gravel.
	End, ///< End marker.
};

/** How many looks of buildings a modular airport has (the old airports' buildings). */
static constexpr uint8_t MODULAR_AIRPORT_BUILDINGS = 6;

#endif /* AIR_TYPE_H */
