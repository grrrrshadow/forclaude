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
#include "timer/timer_game_calendar.h"
#include "window_func.h"

#include "safeguards.h"

/** A text of the configurator as the set gives it, and whether it is hidden now. */
struct VehicleConfigTextResult {
	std::string text;
	bool hidden = false;
};

/**
 * One text from the set (CBID_VEHICLE_DECOUPLE_CONFIG_TEXT): a detail's name
 * or one of its options. Asked with the vehicle when there is one, so the
 * set may answer by what it carries, has chosen or how old it is; result 401
 * is a detail or an option that is there but hidden on this vehicle now --
 * not offered, its place kept, the ones after it still theirs.
 * @param engine the vehicle's engine
 * @param v the vehicle, or nullptr in the purchase list
 * @param aspect which detail
 * @param option which of its options, or 0xFF for the detail's own name
 * @return the text, or nothing when the set has no such detail or option
 */
static std::optional<VehicleConfigTextResult> VehicleConfigText(EngineID engine, const Vehicle *v, uint aspect, uint option)
{
	std::array<int32_t, 16> regs100;
	uint16_t cb = GetVehicleCallback(CBID_VEHICLE_DECOUPLE_CONFIG_TEXT, (aspect << 8) | option, 0, engine, v, regs100);
	if (cb == CALLBACK_FAILED || cb == 0x400) return std::nullopt;
	if (cb == 0x401) return VehicleConfigTextResult{{}, true};
	const GRFFile *grffile = Engine::Get(engine)->GetGRF();
	if (grffile == nullptr) return std::nullopt;
	if (cb == 0x40F) return VehicleConfigTextResult{GetGRFStringWithTextStack(grffile, static_cast<GRFStringID>(regs100[0]), std::span{regs100}.subspan(1))};
	if (cb > 0x400) {
		ErrorUnknownCallbackResult(grffile->grfid, CBID_VEHICLE_DECOUPLE_CONFIG_TEXT, cb);
		return std::nullopt;
	}
	return VehicleConfigTextResult{GetGRFStringWithTextStack(grffile, GRFSTR_MISC_GRF_TEXT + cb, regs100)};
}

/**
 * The details a vehicle's set offers on it, with their options, as the set
 * names them. Only a set that asked for 'decouple_vehicle_config' is asked
 * at all; the details end where the set has no name for the next one, the
 * options of each where it has no name for the next option. A detail or an
 * option the set hides on this vehicle now is there with its place, marked
 * hidden (VehicleConfigAspect::hidden, VehicleConfigOption::hidden).
 * @param engine the vehicle's engine
 * @param v the vehicle the set is asked about: the part that carries the choices; nullptr in the purchase list
 * @return the details, in the set's order; empty when there are none
 */
std::vector<VehicleConfigAspect> GetVehicleConfigAspects(EngineID engine, const Vehicle *v)
{
	std::vector<VehicleConfigAspect> aspects;
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->GetGRF() == nullptr) return aspects;
	const GRFConfig *config = GetGRFConfig(e->GetGRFID());
	if (config == nullptr || !config->vehicle_config) return aspects;
	if (v != nullptr) v = VehicleConfigHead(v);

	for (uint a = 0; a < VEHICLE_CONFIG_MAX_ASPECTS; a++) {
		std::optional<VehicleConfigTextResult> name = VehicleConfigText(engine, v, a, 0xFF);
		if (!name.has_value()) break;
		VehicleConfigAspect aspect;
		aspect.name = std::move(name->text);
		aspect.hidden = name->hidden;
		for (uint o = 0; o < VEHICLE_CONFIG_MAX_OPTIONS; o++) {
			std::optional<VehicleConfigTextResult> option = VehicleConfigText(engine, v, a, o);
			if (!option.has_value()) break;
			aspect.options.push_back({std::move(option->text), option->hidden});
		}
		/* A detail with nothing to choose ends the list: the ones after it
		 * would move up a place and no longer be the byte the set reads. */
		if (aspect.options.empty()) break;
		aspects.push_back(std::move(aspect));
	}
	return aspects;
}

/**
 * The vehicle that carries the choices for a part: the one configured as
 * one. A train's wagon or engine carries its own, since a wagon changes
 * engines and keeps its paint and its graffiti (the player: "a wagon has
 * another engine every minute"); its articulated parts follow it. A road
 * vehicle's trailer follows the tractor, a ship and an aircraft are one.
 * @param v any part of a vehicle
 * @return the part that carries the choices
 */
const Vehicle *VehicleConfigHead(const Vehicle *v)
{
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v);
		if (t->IsRearDualheaded()) t = t->other_multiheaded_part;
		return t->GetFirstEnginePart();
	}
	return v->First();
}

Vehicle *VehicleConfigHead(Vehicle *v)
{
	return const_cast<Vehicle *>(VehicleConfigHead(static_cast<const Vehicle *>(v)));
}

/**
 * Does any part of a vehicle have details to choose: the refit window's
 * Configurator button is lit for it.
 * @param front the vehicle's front
 */
bool VehicleHasConfig(const Vehicle *front)
{
	for (const Vehicle *u = front; u != nullptr; u = u->Next()) {
		if (VehicleConfigHead(u) != u) continue;
		if (!GetVehicleConfigAspects(u->engine_type, u).empty()) return true;
	}
	return false;
}

/**
 * What the set reads in variable 5C, 5D, 5E or 5F: byte i is the option the
 * player chose for detail first + i. Read from the part that carries the
 * choices (VehicleConfigHead()), the same for every part of it.
 * @param v any part of the vehicle
 * @param first the first of the four details: 0 for variable 5C, 4 for 5D, 8 for 5E, 12 for 5F
 */
uint32_t GetVehicleConfigVariable(const Vehicle *v, uint first)
{
	const Vehicle *head = VehicleConfigHead(v);
	uint32_t result = 0;
	for (uint i = 0; i < 4; i++) {
		result |= static_cast<uint32_t>(head->config_options[first + i]) << (8 * i);
	}
	return result;
}

/**
 * Give a vehicle the choices of another, part by part: a clone takes the
 * original's, a wagon of an old game its engine's (AfterLoadGame()). The two
 * are walked together; where one is shorter the rest keeps what it has.
 * @param from_front the vehicle to take the choices from, its front
 * @param to_front the vehicle to give them to, its front
 */
void CopyVehicleConfig(const Vehicle *from_front, Vehicle *to_front)
{
	const Vehicle *from = from_front;
	for (Vehicle *to = to_front; to != nullptr && from != nullptr; to = to->Next(), from = from->Next()) {
		if (VehicleConfigHead(to) != to) continue;
		to->config_options = VehicleConfigHead(from)->config_options;
	}
	ApplyVehicleConfig(to_front);
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
 * register 101. A paint that fades with age gives the colour it fades to in
 * register 102 (0x00RRGGBB), over how many years in 103 and in steps of how
 * many years in 104 (AgeTrueColour()). A detail the set answers nothing for
 * is no colour; an option it answers nothing for leaves the pixels as drawn,
 * the company colour where the mask is the company colour's.
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
	if (regs100[3] > 0) {
		range.aged_r = GB(regs100[2], 16, 8);
		range.aged_g = GB(regs100[2], 8, 8);
		range.aged_b = GB(regs100[2], 0, 8);
		range.age_years = static_cast<uint8_t>(std::min<int32_t>(regs100[3], 255));
		range.age_step = static_cast<uint8_t>(Clamp<int32_t>(regs100[4], 1, 255));
	}
	return range;
}

/** The colours of an engine in the options chosen as the set gives them, fading and all, asked of the set once: the engine and the options, to the colours. */
static std::map<std::pair<EngineID, VehicleConfigOptions>, TrueColourSet> _engine_colour_ranges;
/** GetTrueColourEpoch() when _engine_colour_ranges was last good. */
static uint16_t _engine_colour_ranges_epoch = 0;

/**
 * The colours of an engine of a set in the options chosen, as the set gives
 * them: every detail of its set that is a colour, in the colour of the
 * option chosen, with the fading the set gave it.
 * @param engine the engine
 * @param options the option chosen for each detail
 * @return the colours; empty for none
 */
static const TrueColourSet &EngineColourRanges(EngineID engine, const VehicleConfigOptions &options)
{
	static const TrueColourSet none;
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->GetGRF() == nullptr || !e->GetGRF()->vehicle_config) return none;

	if (_engine_colour_ranges_epoch != GetTrueColourEpoch()) {
		_engine_colour_ranges.clear();
		_engine_colour_ranges_epoch = GetTrueColourEpoch();
	}
	auto it = _engine_colour_ranges.find({engine, options});
	if (it != _engine_colour_ranges.end()) return it->second;

	TrueColourSet colours;
	for (uint a = 0; a < VEHICLE_CONFIG_MAX_ASPECTS; a++) {
		std::optional<TrueColourRange> range = VehicleConfigColour(engine, a, options[a]);
		if (range.has_value()) colours.push_back(*range);
	}
	return _engine_colour_ranges.emplace(std::pair{engine, options}, std::move(colours)).first->second;
}

/**
 * The colours an engine of a set is drawn in, with the options chosen and at
 * an age: every detail of its set that is a colour, in the colour of the
 * option chosen, faded as far as the age takes it (AgeTrueColour()).
 * @param engine the engine
 * @param options the option chosen for each detail; all nought in the purchase list
 * @param age_years the vehicle's age in whole years; 0 in the purchase list
 * @return the colours (InternTrueColourSet()); 0 for none
 */
uint16_t GetEngineTrueColours(EngineID engine, const VehicleConfigOptions &options, uint age_years)
{
	const TrueColourSet &ranges = EngineColourRanges(engine, options);
	if (ranges.empty()) return 0;
	TrueColourSet colours;
	for (const TrueColourRange &range : ranges) colours.push_back(AgeTrueColour(range, age_years));
	return InternTrueColourSet(colours);
}

/**
 * A vehicle's age in whole years, for the fading of its paint: the age of
 * the part that carries the choices, so a wagon's parts fade together.
 * @param v the part
 */
static uint VehicleAgeYears(const Vehicle *v)
{
	return static_cast<uint>(std::min<int64_t>(255, VehicleConfigHead(v)->age.base() / CalendarTime::DAYS_IN_YEAR));
}

/**
 * The colours a part of a vehicle is drawn in: its engine's in the options
 * chosen on the part that carries them (VehicleConfigHead()), at the
 * vehicle's age. Kept on the part until the player chooses anew
 * (ApplyVehicleConfig()), the sets are read anew, or another year has gone.
 * @param v the part
 * @return the colours (InternTrueColourSet()); 0 for none
 */
uint16_t GetVehicleTrueColours(const Vehicle *v)
{
	const uint years = VehicleAgeYears(v);
	if (v->true_colours_epoch == GetTrueColourEpoch() && v->true_colours_age_years == years) return v->true_colours;
	v->true_colours = GetEngineTrueColours(v->engine_type, VehicleConfigHead(v)->config_options, years);
	v->true_colours_epoch = GetTrueColourEpoch();
	v->true_colours_age_years = static_cast<uint8_t>(years);
	return v->true_colours;
}

/**
 * For the rig: the colours of a vehicle as it is drawn now, its engine's in
 * its options at its age (DescribeEngineTrueColours()).
 * @param v the vehicle
 * @return the description, one line
 */
std::string DescribeVehicleTrueColours(const Vehicle *v)
{
	return DescribeEngineTrueColours(v->engine_type, VehicleConfigHead(v)->config_options, VehicleAgeYears(v));
}

/**
 * For the rig: the colours of an engine in the options chosen at an age,
 * detail by detail, as the set gives them and as they are at that age.
 * @param engine the engine
 * @param options the options chosen
 * @param age_years the age in whole years
 * @return the description, one line
 */
std::string DescribeEngineTrueColours(EngineID engine, const VehicleConfigOptions &options, uint age_years)
{
	uint16_t id = GetEngineTrueColours(engine, options, age_years);
	if (id == 0) return "bez barev";
	std::string out = fmt::format("sada {}", id);
	for (const TrueColourRange &range : EngineColourRanges(engine, options)) {
		if (range.age_years != 0) out += fmt::format(" [bledne k #{:02X}{:02X}{:02X} za {} let po {}]", range.aged_r, range.aged_g, range.aged_b, range.age_years, range.age_step);
	}
	for (const TrueColourRange &range : GetTrueColourSet(id)) {
		out += fmt::format(" [maska 0x{:02X}-0x{:02X} #{:02X}{:02X}{:02X} svetla {}/{}/{}]", range.first, range.first + TRUE_COLOUR_RANGE_SIZE - 1,
				range.r, range.g, range.b, range.light_start, range.light_stop, range.light_max);
	}
	return out;
}

/**
 * The details changed: the set is to draw the vehicle anew. A vehicle
 * standing in a depot is not asked for its pictures again until it moves, so
 * every part is asked here, and the windows showing it redrawn. The whole
 * consist is done over, whichever part changed: a wagon's choice may be read
 * by its neighbours too.
 * @param v any part of the vehicle
 */
void ApplyVehicleConfig(Vehicle *v)
{
	Vehicle *front = v->First();
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
