/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_config.cpp The details a vehicle's set lets the player choose: reading them from the set, giving the choice back to it. */

#include "stdafx.h"
#include "vehicle_config.h"
#include "vehicle_base.h"
#include "engine_base.h"
#include "newgrf.h"
#include "newgrf_config.h"
#include "newgrf_engine.h"
#include "newgrf_callbacks.h"
#include "newgrf_text.h"
#include "roadveh.h"
#include "train.h"
#include "window_func.h"

#include "safeguards.h"

/**
 * One text from the set (CBID_VEHICLE_DECOUPLE_CONFIG_TEXT): a detail's name
 * or one of its options.
 * @param engine the vehicle's engine
 * @param aspect which detail
 * @param option which of its options, or 0xFF for the detail's own name
 * @return the text, or nothing when the set has no such detail or option
 */
static std::optional<std::string> VehicleConfigText(EngineID engine, uint aspect, uint option)
{
	std::array<int32_t, 16> regs100;
	uint16_t cb = GetVehicleCallback(CBID_VEHICLE_DECOUPLE_CONFIG_TEXT, (aspect << 8) | option, 0, engine, nullptr, regs100);
	if (cb == CALLBACK_FAILED || cb == 0x400) return std::nullopt;
	const GRFFile *grffile = Engine::Get(engine)->GetGRF();
	if (grffile == nullptr) return std::nullopt;
	if (cb == 0x40F) return GetGRFStringWithTextStack(grffile, static_cast<GRFStringID>(regs100[0]), std::span{regs100}.subspan(1));
	if (cb > 0x400) {
		ErrorUnknownCallbackResult(grffile->grfid, CBID_VEHICLE_DECOUPLE_CONFIG_TEXT, cb);
		return std::nullopt;
	}
	return GetGRFStringWithTextStack(grffile, GRFSTR_MISC_GRF_TEXT + cb, regs100);
}

/**
 * The details a vehicle's set offers on it, with their options, as the set
 * names them. Only a set that asked for 'decouple_vehicle_config' is asked
 * at all; the details end where the set has no name for the next one, the
 * options of each where it has no name for the next option.
 * @param engine the vehicle's engine
 * @return the details, in the set's order; empty when there are none
 */
std::vector<VehicleConfigAspect> GetVehicleConfigAspects(EngineID engine)
{
	std::vector<VehicleConfigAspect> aspects;
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->GetGRF() == nullptr) return aspects;
	const GRFConfig *config = GetGRFConfig(e->GetGRFID());
	if (config == nullptr || !config->vehicle_config) return aspects;

	for (uint a = 0; a < VEHICLE_CONFIG_MAX_ASPECTS; a++) {
		std::optional<std::string> name = VehicleConfigText(engine, a, 0xFF);
		if (!name.has_value()) break;
		VehicleConfigAspect aspect;
		aspect.name = std::move(*name);
		for (uint o = 0; o < VEHICLE_CONFIG_MAX_OPTIONS; o++) {
			std::optional<std::string> option = VehicleConfigText(engine, a, o);
			if (!option.has_value()) break;
			aspect.options.push_back(std::move(*option));
		}
		/* A detail with nothing to choose ends the list: the ones after it
		 * would move up a place and no longer be the byte the set reads. */
		if (aspect.options.empty()) break;
		aspects.push_back(std::move(aspect));
	}
	return aspects;
}

/**
 * What the set reads in variable 5C: byte i is the option the player chose
 * for detail i. Kept on the vehicle's front, since the whole vehicle is
 * configured as one, and read from there for every part.
 * @param v any part of the vehicle
 */
uint32_t GetVehicleConfigVariable(const Vehicle *v)
{
	const Vehicle *front = v->First();
	uint32_t result = 0;
	for (uint i = 0; i < front->config_options.size(); i++) {
		result |= static_cast<uint32_t>(front->config_options[i]) << (8 * i);
	}
	return result;
}

/**
 * The details changed: the set is to draw the vehicle anew. A vehicle
 * standing in a depot is not asked for its pictures again until it moves, so
 * every part is asked here, and the windows showing it redrawn.
 * @param front the vehicle's front
 */
void ApplyVehicleConfig(Vehicle *front)
{
	for (Vehicle *u = front; u != nullptr; u = u->Next()) {
		u->InvalidateNewGRFCache();
		/* A standing vehicle's picture is looked at again only when it turns
		 * or comes into view; this makes it look now. */
		u->sprite_cache.last_direction = Direction::Invalid;
		switch (u->type) {
			case VehicleType::Road: RoadVehicle::From(u)->UpdateViewport(true, false); break;
			case VehicleType::Train: Train::From(u)->UpdateViewport(true, false); break;
			default: u->UpdateViewport(true); break;
		}
	}
	SetWindowDirty(WindowClass::VehicleView, front->index);
	SetWindowDirty(WindowClass::VehicleDetails, front->index);
	InvalidateWindowData(WindowClass::VehicleConfig, front->index);
}
