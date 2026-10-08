/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file vehicle_config.h The configurator (the player's word): the details a
 * vehicle's set lets the player choose on it -- who pulls the hand cart and
 * whether it is pushed or pulled, later a stripe on a tanker, a colour on a
 * trailer. The cargo is the refit's business and stays whatever is chosen here.
 *
 * The game knows nothing of what a detail means. A set that asked for the
 * feature 'decouple_vehicle_config' (newgrf_act14.cpp) names up to four
 * details on a vehicle and the options of each through callback 1C0
 * (CBID_VEHICLE_DECOUPLE_CONFIG_TEXT), the player picks one option per
 * detail in the configurator window (vehicle_config_gui.cpp) and the game
 * keeps the choice on the vehicle (Vehicle::config_options) and reads it
 * back to the set in variable 5C, a byte per detail. What the set draws or
 * does with that is the set's own: docs/decouple_vehicle_config.md.
 */

#ifndef VEHICLE_CONFIG_H
#define VEHICLE_CONFIG_H

#include "engine_type.h"
#include "vehicle_type.h"

/** How many details a set may offer on one vehicle: a byte each in variable 5C. */
static constexpr uint VEHICLE_CONFIG_MAX_ASPECTS = 4;
/** How many options a detail may have. */
static constexpr uint VEHICLE_CONFIG_MAX_OPTIONS = 32;

/** One detail a set offers on a vehicle, and its options, as the set names them. */
struct VehicleConfigAspect {
	std::string name; ///< The detail's name ("Crew", "Cart").
	std::vector<std::string> options; ///< Its options in the set's order ("College Girl", "Female Girl"); a new vehicle has the first.
};

std::vector<VehicleConfigAspect> GetVehicleConfigAspects(EngineID engine);
uint32_t GetVehicleConfigVariable(const struct Vehicle *v);
void ApplyVehicleConfig(struct Vehicle *front);
void ShowVehicleConfigWindow(const struct Vehicle *v);

#endif /* VEHICLE_CONFIG_H */
