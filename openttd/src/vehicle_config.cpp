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
#include "true_colour.h"
#include "palette_func.h"
#include "debug.h"
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
 * What the set reads in variable 5C or 5D: byte i is the option the player
 * chose for detail first + i. Kept on the vehicle's front, since the whole
 * vehicle is configured as one, and read from there for every part.
 * @param v any part of the vehicle
 * @param first the first of the four details: 0 for variable 5C, 4 for 5D
 */
uint32_t GetVehicleConfigVariable(const Vehicle *v, uint first)
{
	const Vehicle *front = v->First();
	uint32_t result = 0;
	for (uint i = 0; i < 4; i++) {
		result |= static_cast<uint32_t>(front->config_options[first + i]) << (8 * i);
	}
	return result;
}

/**
 * The colour of a detail from the set (CBID_VEHICLE_DECOUPLE_CONFIG_COLOUR).
 * Asked like the texts, of the engine alone, so a vehicle's colours follow
 * from its engine and the options chosen and nothing else.
 *
 * First the detail itself (option 0xFF): the set answers the first of the
 * eight mask indices it paints, and in register 100 the player's three
 * numbers of its lightening (SetTrueColourLight()), nought for the usual.
 * Then the option chosen: 40F, with the colour as 0x00RRGGBB in register 100
 * and, if the colour lightens otherwise than its detail, the three numbers in
 * register 101. A detail the set answers nothing for is no colour; an option
 * it answers nothing for leaves the pixels as drawn, the company colour
 * where the mask is the company colour's.
 * @param engine the engine
 * @param aspect which detail
 * @param option the option chosen for it
 * @return the colour of the detail, or nothing
 */
static std::optional<TrueColourRange> VehicleConfigColour(EngineID engine, uint aspect, uint option)
{
	std::array<int32_t, 16> regs100;
	uint16_t cb = GetVehicleCallback(CBID_VEHICLE_DECOUPLE_CONFIG_COLOUR, (aspect << 8) | 0xFF, 0, engine, nullptr, regs100);
	if (cb == CALLBACK_FAILED || cb == 0 || cb >= 0x400) return std::nullopt;
	if (cb + TRUE_COLOUR_RANGE_SIZE > PALETTE_ANIM_START) {
		/* The palette's animated colours, and past them, are not a set's to paint. */
		Debug(grf, 1, "Engine {}: detail {} paints mask indices from 0x{:X}, past 0x{:X}; no colour", engine, aspect, cb, PALETTE_ANIM_START - TRUE_COLOUR_RANGE_SIZE);
		return std::nullopt;
	}
	TrueColourRange range;
	range.first = static_cast<uint8_t>(cb);
	SetTrueColourLight(range, static_cast<uint32_t>(regs100[0]));

	cb = GetVehicleCallback(CBID_VEHICLE_DECOUPLE_CONFIG_COLOUR, (aspect << 8) | option, 0, engine, nullptr, regs100);
	if (cb != 0x40F) return std::nullopt;
	range.r = GB(regs100[0], 16, 8);
	range.g = GB(regs100[0], 8, 8);
	range.b = GB(regs100[0], 0, 8);
	if (regs100[1] != 0) SetTrueColourLight(range, static_cast<uint32_t>(regs100[1]));
	return range;
}

/** The colours of an engine in the options chosen, asked of the set once: the engine and the options, to the colours. */
static std::map<std::pair<EngineID, VehicleConfigOptions>, uint16_t> _engine_true_colours;
/** GetTrueColourEpoch() when _engine_true_colours was last good. */
static uint16_t _engine_true_colours_epoch = 0;

/**
 * The colours an engine of a set is drawn in, with the options chosen: every
 * detail of its set that is a colour, in the colour of the option chosen.
 * @param engine the engine
 * @param options the option chosen for each detail; all nought in the purchase list
 * @return the colours (InternTrueColourSet()); 0 for none
 */
uint16_t GetEngineTrueColours(EngineID engine, const VehicleConfigOptions &options)
{
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->GetGRF() == nullptr || !e->GetGRF()->vehicle_config) return 0;

	if (_engine_true_colours_epoch != GetTrueColourEpoch()) {
		_engine_true_colours.clear();
		_engine_true_colours_epoch = GetTrueColourEpoch();
	}
	auto it = _engine_true_colours.find({engine, options});
	if (it != _engine_true_colours.end()) return it->second;

	TrueColourSet colours;
	for (uint a = 0; a < VEHICLE_CONFIG_MAX_ASPECTS; a++) {
		std::optional<TrueColourRange> range = VehicleConfigColour(engine, a, options[a]);
		if (range.has_value()) colours.push_back(*range);
	}
	uint16_t id = InternTrueColourSet(colours);
	_engine_true_colours.emplace(std::pair{engine, options}, id);
	return id;
}

/**
 * The colours a part of a vehicle is drawn in: its engine's in the options
 * chosen on the vehicle's front. Kept on the part until the player chooses
 * anew (ApplyVehicleConfig()) or the sets are read anew.
 * @param v the part
 * @return the colours (InternTrueColourSet()); 0 for none
 */
uint16_t GetVehicleTrueColours(const Vehicle *v)
{
	if (v->true_colours_epoch == GetTrueColourEpoch()) return v->true_colours;
	v->true_colours = GetEngineTrueColours(v->engine_type, v->First()->config_options);
	v->true_colours_epoch = GetTrueColourEpoch();
	return v->true_colours;
}

/**
 * For the rig: the colours of an engine in the options chosen, detail by
 * detail, as the set gives them.
 * @param engine the engine
 * @param options the options chosen
 * @return the description, one line
 */
std::string DescribeEngineTrueColours(EngineID engine, const VehicleConfigOptions &options)
{
	uint16_t id = GetEngineTrueColours(engine, options);
	if (id == 0) return "bez barev";
	std::string out = fmt::format("sada {}", id);
	for (const TrueColourRange &range : GetTrueColourSet(id)) {
		out += fmt::format(" [maska 0x{:02X}-0x{:02X} #{:02X}{:02X}{:02X} svetla {}/{}/{}]", range.first, range.first + TRUE_COLOUR_RANGE_SIZE - 1,
				range.r, range.g, range.b, range.light_start, range.light_stop, range.light_max);
	}
	return out;
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
		u->true_colours_epoch = 0;
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
