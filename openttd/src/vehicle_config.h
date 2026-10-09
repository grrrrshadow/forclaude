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
 * feature 'decouple_vehicle_config' (newgrf_act14.cpp) names up to sixteen
 * details on a vehicle and the options of each through callback 1C0
 * (CBID_VEHICLE_DECOUPLE_CONFIG_TEXT) -- asked with the vehicle, so that a
 * set may hide a detail or an option by what the vehicle carries, has chosen
 * or how old it is (result 401) -- the player picks one option per
 * detail in the configurator window (vehicle_config_gui.cpp) and the game
 * keeps the choice on the vehicle (Vehicle::config_options) and reads it
 * back to the set in variables 5C (details 0 to 3), 5D (4 to 7), 5E (8 to
 * 11) and 5F (12 to 15), a byte per detail. What the set draws or does with
 * that is the set's own: docs/decouple_vehicle_config.md.
 *
 * The choices are kept on the vehicle that is configured as one
 * (VehicleConfigHead()): a road vehicle with its trailer on the tractor, a
 * wagon of a train on the wagon itself, since a wagon changes engines and
 * keeps its paint and its graffiti (the player: "a wagon has another engine
 * every minute"); articulated parts follow their head.
 *
 * A detail may be a colour: the paint of the cab, of the body, of the
 * radiator. The set then says through callback 1C1
 * (CBID_VEHICLE_DECOUPLE_CONFIG_COLOUR) which mask indices of its pictures
 * the detail paints and the colour of each option, exact, and the game draws
 * the vehicle in the colours chosen (true_colour.h).
 */

#ifndef VEHICLE_CONFIG_H
#define VEHICLE_CONFIG_H

#include "engine_type.h"
#include "vehicle_type.h"
#include "window_type.h"

/** How many details a set may offer on one vehicle: a byte each in variables 5C, 5D, 5E and 5F. */
static constexpr uint VEHICLE_CONFIG_MAX_ASPECTS = 16;
/** How many options a detail may have. */
static constexpr uint VEHICLE_CONFIG_MAX_OPTIONS = 32;

/** One option of a detail, as the set names it. */
struct VehicleConfigOption {
	std::string name; ///< The option's name ("College Girl").
	bool hidden = false; ///< The set hides it on this vehicle as it is now (result 401 of callback 1C0): not offered, its place kept.
};

/** One detail a set offers on a vehicle, and its options, as the set names them. */
struct VehicleConfigAspect {
	std::string name; ///< The detail's name ("Crew", "Cart").
	std::vector<VehicleConfigOption> options; ///< Its options in the set's order ("College Girl", "Female Girl"); a new vehicle has the first.
	bool hidden = false; ///< The set hides the whole detail on this vehicle as it is now (result 401): no row for it, its place kept.

	/** Whether the option may be chosen now: there, and not hidden. */
	bool Offers(uint option) const { return option < this->options.size() && !this->options[option].hidden; }
};

/** The options chosen, detail by detail (Vehicle::config_options). */
using VehicleConfigOptions = std::array<uint8_t, VEHICLE_CONFIG_MAX_ASPECTS>;

std::vector<VehicleConfigAspect> GetVehicleConfigAspects(EngineID engine, const struct Vehicle *v = nullptr);
const struct Vehicle *VehicleConfigHead(const struct Vehicle *v);
struct Vehicle *VehicleConfigHead(struct Vehicle *v);
bool VehicleHasConfig(const struct Vehicle *front);
uint32_t GetVehicleConfigVariable(const struct Vehicle *v, uint first);
void ApplyVehicleConfig(struct Vehicle *head);
void CopyVehicleConfig(const struct Vehicle *from_front, struct Vehicle *to_front);
void ShowVehicleConfigWindow(const struct Vehicle *v, struct Window *parent, VehicleID selected, uint8_t num_vehicles);
void UpdateVehicleConfigWindowSelection(VehicleID front, VehicleID selected, uint8_t num_vehicles);

uint16_t GetEngineTrueColours(EngineID engine, const VehicleConfigOptions &options, uint age_years = 0);
uint16_t GetVehicleTrueColours(const struct Vehicle *v);
std::string DescribeEngineTrueColours(EngineID engine, const VehicleConfigOptions &options, uint age_years = 0);
std::string DescribeVehicleTrueColours(const struct Vehicle *v);

#endif /* VEHICLE_CONFIG_H */
