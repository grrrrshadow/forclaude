/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file order_gui.cpp GUI related to orders. */

#include "stdafx.h"
#include "command_func.h"
#include "viewport_func.h"
#include "depot_map.h"
#include "roadveh.h"
#include "timetable.h"
#include "strings_func.h"
#include "company_func.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "textbuf_gui.h"
#include "string_func.h"
#include "tilehighlight_func.h"
#include "network/network.h"
#include "station_base.h"
#include "industry.h"
#include "waypoint_base.h"
#include "core/geometry_func.hpp"
#include "hotkeys.h"
#include "aircraft.h"
#include "engine_func.h"
#include "vehicle_func.h"
#include "vehiclelist.h"
#include "vehicle_func.h"
#include "error.h"
#include "order_cmd.h"
#include "company_cmd.h"
#include "train.h"
#include "train_cmd.h"
#include "depot_base.h"
#include "articulated_vehicles.h"
#include "road_on_rail.h"
#include "engine_base.h"
#include "engine_func.h"
#include "vehicle_gui.h"
#include "core/string_consumer.hpp"

#include "widgets/order_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** Order load types that could be given to station orders. */
static const StringID _station_load_types[][5][5] = {
	{
		/* No refitting. */
		{
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			STR_ORDER_FULL_LOAD,
			STR_ORDER_FULL_LOAD_ANY,
			STR_ORDER_NO_LOAD,
		}, {
			STR_ORDER_UNLOAD,
			INVALID_STRING_ID,
			STR_ORDER_UNLOAD_FULL_LOAD,
			STR_ORDER_UNLOAD_FULL_LOAD_ANY,
			STR_ORDER_UNLOAD_NO_LOAD,
		}, {
			STR_ORDER_TRANSFER,
			INVALID_STRING_ID,
			STR_ORDER_TRANSFER_FULL_LOAD,
			STR_ORDER_TRANSFER_FULL_LOAD_ANY,
			STR_ORDER_TRANSFER_NO_LOAD,
		}, {
			/* Unload and transfer do not work together. */
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			INVALID_STRING_ID,
		}, {
			STR_ORDER_NO_UNLOAD,
			INVALID_STRING_ID,
			STR_ORDER_NO_UNLOAD_FULL_LOAD,
			STR_ORDER_NO_UNLOAD_FULL_LOAD_ANY,
			STR_ORDER_NO_UNLOAD_NO_LOAD,
		}
	}, {
		/* With auto-refitting. No loading and auto-refitting do not work together. */
		{
			STR_ORDER_AUTO_REFIT,
			INVALID_STRING_ID,
			STR_ORDER_FULL_LOAD_REFIT,
			STR_ORDER_FULL_LOAD_ANY_REFIT,
			INVALID_STRING_ID,
		}, {
			STR_ORDER_UNLOAD_REFIT,
			INVALID_STRING_ID,
			STR_ORDER_UNLOAD_FULL_LOAD_REFIT,
			STR_ORDER_UNLOAD_FULL_LOAD_ANY_REFIT,
			INVALID_STRING_ID,
		}, {
			STR_ORDER_TRANSFER_REFIT,
			INVALID_STRING_ID,
			STR_ORDER_TRANSFER_FULL_LOAD_REFIT,
			STR_ORDER_TRANSFER_FULL_LOAD_ANY_REFIT,
			INVALID_STRING_ID,
		}, {
			/* Unload and transfer do not work together. */
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			INVALID_STRING_ID,
			INVALID_STRING_ID,
		}, {
			STR_ORDER_NO_UNLOAD_REFIT,
			INVALID_STRING_ID,
			STR_ORDER_NO_UNLOAD_FULL_LOAD_REFIT,
			STR_ORDER_NO_UNLOAD_FULL_LOAD_ANY_REFIT,
			INVALID_STRING_ID,
		}
	}
};

static const StringID _order_non_stop_dropdown[] = {
	STR_ORDER_GO_TO,
	STR_ORDER_GO_NON_STOP_TO,
	STR_ORDER_GO_VIA,
	STR_ORDER_GO_NON_STOP_VIA,
};

static const StringID _order_full_load_dropdown[] = {
	STR_ORDER_DROP_LOAD_IF_POSSIBLE,
	STR_EMPTY,
	STR_ORDER_DROP_FULL_LOAD_ALL,
	STR_ORDER_DROP_FULL_LOAD_ANY,
	STR_ORDER_DROP_NO_LOADING,
};

static const StringID _order_unload_dropdown[] = {
	STR_ORDER_DROP_UNLOAD_IF_ACCEPTED,
	STR_ORDER_DROP_UNLOAD,
	STR_ORDER_DROP_TRANSFER,
	STR_EMPTY,
	STR_ORDER_DROP_NO_UNLOADING,
};

static const StringID _order_goto_dropdown[] = {
	STR_ORDER_GO_TO,
	STR_ORDER_GO_TO_NEAREST_DEPOT,
	STR_ORDER_CONDITIONAL,
	STR_ORDER_SHARE,
};

static const StringID _order_goto_dropdown_aircraft[] = {
	STR_ORDER_GO_TO,
	STR_ORDER_GO_TO_NEAREST_HANGAR,
	STR_ORDER_CONDITIONAL,
	STR_ORDER_SHARE,
};

/** Variables for conditional orders; this defines the order of appearance in the dropdown box */
static const OrderConditionVariable _order_conditional_variable[] = {
	OrderConditionVariable::LoadPercentage,
	OrderConditionVariable::Reliability,
	OrderConditionVariable::MaxReliability,
	OrderConditionVariable::MaxSpeed,
	OrderConditionVariable::Age,
	OrderConditionVariable::RemainingLifetime,
	OrderConditionVariable::RequiresService,
	OrderConditionVariable::DrivingBackwards,
	OrderConditionVariable::WagonCount,
	OrderConditionVariable::TrainLength,
	OrderConditionVariable::NothingToCouple,
	OrderConditionVariable::RoadVehiclesWaitingToBoard,
	OrderConditionVariable::Unconditionally,
};

static const StringID _order_conditional_condition[] = {
	STR_ORDER_CONDITIONAL_COMPARATOR_EQUALS,
	STR_ORDER_CONDITIONAL_COMPARATOR_NOT_EQUALS,
	STR_ORDER_CONDITIONAL_COMPARATOR_LESS_THAN,
	STR_ORDER_CONDITIONAL_COMPARATOR_LESS_EQUALS,
	STR_ORDER_CONDITIONAL_COMPARATOR_MORE_THAN,
	STR_ORDER_CONDITIONAL_COMPARATOR_MORE_EQUALS,
	STR_ORDER_CONDITIONAL_COMPARATOR_IS_TRUE,
	STR_ORDER_CONDITIONAL_COMPARATOR_IS_FALSE,
};

extern uint ConvertSpeedToDisplaySpeed(uint speed, VehicleType type);
extern uint ConvertDisplaySpeedToSpeed(uint speed, VehicleType type);

/** How full the wagons a coupling order will collect have to be; indexed by OrderCoupleLoad. */
static const StringID _order_couple_load_dropdown[] = {
	STR_ORDER_COUPLE_LOAD_ANY,
	STR_ORDER_COUPLE_LOAD_EMPTY,
	STR_ORDER_COUPLE_LOAD_FULL,
	STR_ORDER_COUPLE_LOAD_ANY_FULL_FIRST,
	STR_ORDER_COUPLE_LOAD_ANY_EMPTY_FIRST,
};
static_assert(std::size(_order_couple_load_dropdown) == to_underlying(OrderCoupleLoad::End));

/**
 * How a road vehicle gets carried on from a station, as the player reads it.
 * The four ways first and "none of them" last, which is the order they were
 * asked for in; what each one means is in OrderBoardMode, and the list is
 * kept beside the values it names so the two cannot drift apart.
 */
static const StringID _order_board_mode_dropdown[] = {
	STR_ORDER_BOARD_MODE_TRAIN_TO_NEXT,
	STR_ORDER_BOARD_MODE_TRAIN_ANYWHERE,
	STR_ORDER_BOARD_MODE_WAGONS_TO_NEXT,
	STR_ORDER_BOARD_MODE_WAGONS_ANYWHERE,
	STR_ORDER_BOARD_MODE_PLANE_TO_NEXT,
	STR_ORDER_BOARD_MODE_SHIP_TO_NEXT,
	STR_ORDER_BOARD_MODE_NONE,
};

/** @copydoc _order_board_mode_dropdown */
static const OrderBoardMode _order_board_mode_values[] = {
	OrderBoardMode::TrainToNext,
	OrderBoardMode::TrainAnywhere,
	OrderBoardMode::WagonsToNext,
	OrderBoardMode::WagonsAnywhere,
	OrderBoardMode::PlaneToNext,
	OrderBoardMode::ShipToNext,
	OrderBoardMode::None,
};

/** Which line of #_order_board_mode_dropdown shows a given way of boarding. */
static int BoardModeToIndex(OrderBoardMode mode)
{
	for (size_t i = 0; i < std::size(_order_board_mode_values); i++) {
		if (_order_board_mode_values[i] == mode) return (int)i;
	}
	return (int)std::size(_order_board_mode_values) - 1;
}

/**
 * Build the list of cargoes a coupling order can ask for.
 *
 * Every cargo in the game, plus asking for none in particular -- unless the
 * order names a wagon, and then only what that wagon can be fitted for. The
 * player's rule: the cargo filter is narrowed by the type. Offering a cargo
 * the named wagon cannot take would be offering an order that can never match
 * anything, and the list is where that is easiest to say.
 *
 * The first entry is "every cargo", and it is only that: the named wagon
 * stays (MOF_COUPLE_CARGO in order_cmd.cpp), so the list stays narrowed to
 * it. It used to say "also lets the wagon go", and on an order that only
 * filters by type that was not even true -- the player read it and asked for
 * the plain words.
 *
 * @param order the order the list is being opened for
 * @return the list to show
 */
static DropDownList BuildCoupleCargoDropDown(const Order *order)
{
	/* What the named wagon can carry, or everything when none is named. Taken
	 * unmasked on purpose: the union is masked with _standard_cargo_mask
	 * wherever the purchase list uses it, and that mask leaves out the cargo
	 * for road vehicles -- the very one the hand-written entry below is here
	 * for. Masked, a car carrier would be the one wagon that could not be
	 * asked for its own cargo. */
	const Engine *named = order != nullptr ? Engine::GetIfValid(order->GetCoupleBuyEngine()) : nullptr;
	CargoTypes allowed = named != nullptr ? GetUnionOfArticulatedRefitMasks(order->GetCoupleBuyEngine(), true) : ALL_CARGOTYPES;

	DropDownList list;
	/* Every cargo with its icon, as the purchase list shows them; the line
	 * for every cargo takes a blank one so that the names stay in line. */
	Dimension d = GetLargestCargoIconSize();
	list.push_back(MakeDropDownListIconItem(d, SPR_EMPTY, PAL_NONE, STR_ORDER_COUPLE_CARGO_EVERY, INVALID_CARGO, false));
	/* Wagons fitted for road vehicles, named here the way the purchase list
	 * names them: their cargo is not a standard one -- it is of the special
	 * class, and the sorted list of standard cargoes stops short of those --
	 * so the loop below never offered it, while the refit and purchase menus,
	 * which add it by hand, did. A collector could be told to take wagons of
	 * every cargo but the one it was built to shuttle. See road_on_rail.h.
	 *
	 * Whether a named wagon carries it is asked of that one function rather
	 * than of a refit mask, for the same reason the purchase list asks it
	 * there: the fitting is ours and no set knows about it. */
	if (IsValidCargoType(_road_vehicle_cargo) && (named == nullptr || CanCarryRoadVehicles(named))) {
		const CargoSpec *rola = CargoSpec::Get(_road_vehicle_cargo);
		list.push_back(MakeDropDownListIconItem(d, rola->GetCargoIcon(), PAL_NONE, rola->name, _road_vehicle_cargo, false));
	}
	for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
		if (!allowed.Test(cs->Index())) continue;
		list.push_back(MakeDropDownListIconItem(d, cs->GetCargoIcon(), PAL_NONE, cs->name, cs->Index(), false));
	}
	return list;
}

static const StringID _order_depot_action_dropdown[] = {
	STR_ORDER_DROP_GO_ALWAYS_DEPOT,
	STR_ORDER_DROP_SERVICE_DEPOT,
	STR_ORDER_DROP_HALT_DEPOT,
	STR_ORDER_DROP_UNBUNCH,
};

static OrderDepotAction DepotActionStringIndex(const Order *order)
{
	if (order->GetDepotActionType().Test(OrderDepotActionFlag::Halt)) return OrderDepotAction::Stop;
	if (order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service)) return OrderDepotAction::Service;
	if (order->GetDepotActionType().Test(OrderDepotActionFlag::Unbunch)) return OrderDepotAction::Unbunch;
	return OrderDepotAction::AlwaysGo;
}

static const StringID _order_refit_action_dropdown[] = {
	STR_ORDER_DROP_REFIT_AUTO,
	STR_ORDER_DROP_REFIT_AUTO_ANY,
};

static StringID GetOrderGoToString(const Order &order)
{
	if (order.GetDepotOrderType().Test(OrderDepotTypeFlag::Service)) {
		return order.GetNonStopType().Test(OrderNonStopFlag::NonStop) ? STR_ORDER_SERVICE_NON_STOP_AT : STR_ORDER_SERVICE_AT;
	} else {
		return order.GetNonStopType().Test(OrderNonStopFlag::NonStop) ? STR_ORDER_GO_NON_STOP_TO : STR_ORDER_GO_TO;
	}
}

/**
 * Draws an order in order or timetable GUI
 * @param v Vehicle the order belongs to
 * @param order The order to draw
 * @param order_index Index of the order in the orders of the vehicle
 * @param y Y position for drawing
 * @param selected True, if the order is selected
 * @param timetable True, when drawing in the timetable GUI
 * @param left Left border for text drawing
 * @param middle X position between order index and order text
 * @param right Right border for text drawing
 */
/** Is this a waypoint order whose waypoint is a station waypoint (see #WPF_STATION_SEARCH)? */
static bool IsStationWaypointOrder(const Order *order)
{
	if (!order->IsType(OT_GOTO_WAYPOINT)) return false;
	const Waypoint *wp = Waypoint::GetIfValid(order->GetDestination().ToStationID());
	return wp != nullptr && HasBit(wp->waypoint_flags, WPF_STATION_SEARCH);
}

void DrawOrderString(const Vehicle *v, const Order *order, VehicleOrderID order_index, int y, bool selected, bool timetable, int left, int middle, int right)
{
	bool rtl = _current_text_dir == TD_RTL;

	SpriteID sprite = rtl ? SPR_ARROW_LEFT : SPR_ARROW_RIGHT;
	Dimension sprite_size = GetSpriteSize(sprite);
	if (v->cur_real_order_index == order_index) {
		/* Draw two arrows before the next real order. */
		DrawSprite(sprite, PAL_NONE, rtl ? right -     sprite_size.width : left,                     y + ((int)GetCharacterHeight(FontSize::Normal) - (int)sprite_size.height) / 2);
		DrawSprite(sprite, PAL_NONE, rtl ? right - 2 * sprite_size.width : left + sprite_size.width, y + ((int)GetCharacterHeight(FontSize::Normal) - (int)sprite_size.height) / 2);
	} else if (v->cur_implicit_order_index == order_index) {
		/* Draw one arrow before the next implicit order; the next real order will still get two arrows. */
		DrawSprite(sprite, PAL_NONE, rtl ? right -     sprite_size.width : left,                     y + ((int)GetCharacterHeight(FontSize::Normal) - (int)sprite_size.height) / 2);
	}

	ExtendedTextColour colour{TextColour::Black};
	if (order->IsType(OT_IMPLICIT)) {
		colour = ExtendedTextColour{selected ? TextColour::Silver : TextColour::Grey, ExtendedTextColourFlag::NoShade};
	} else if (selected) {
		colour = TextColour::White;
	}

	DrawString(left, rtl ? right - 2 * sprite_size.width - 3 : middle, y, GetString(STR_ORDER_INDEX, order_index + 1), colour, AlignmentH::ForceRight);

	std::string line;
	/* What this feature adds to an order -- coupling, its filters, waiting,
	 * decoupling, reversing out, the depot's coupling and putting down -- goes
	 * on a second line under the order, in the list; the timetable keeps it
	 * on the one line it has. See OrderHasSecondLine(). */
	std::string second;

	switch (order->GetType()) {
		case OT_DUMMY:
			line = GetString(STR_INVALID_ORDER);
			break;

		case OT_IMPLICIT:
			line = GetString(STR_ORDER_GO_TO_STATION, STR_ORDER_GO_TO, order->GetDestination());
			if (!timetable) line += GetString(STR_ORDER_IMPLICIT);
			break;

		case OT_GOTO_STATION: {
			OrderLoadType load = order->GetLoadType();
			OrderUnloadType unload = order->GetUnloadType();
			bool valid_station = CanVehicleUseStation(v, Station::Get(order->GetDestination().ToStationID()));

			line = GetString(valid_station ? STR_ORDER_GO_TO_STATION : STR_ORDER_GO_TO_STATION_CAN_T_USE_STATION, STR_ORDER_GO_TO + (v->IsGroundVehicle() ? order->GetNonStopType() : OrderNonStopFlags{}).base(), order->GetDestination());
			if (timetable) {
				/* Show only wait time in the timetable window. */
				if (order->GetWaitTime() > 0) {
					auto [str, value] = GetTimetableParameters(order->GetWaitTime());
					line += GetString(order->IsWaitTimetabled() ? STR_TIMETABLE_STAY_FOR : STR_TIMETABLE_STAY_FOR_ESTIMATED, str, value);
				}
			} else {
				/* Show non-stop, refit and stop location only in the order window. */
				/* A boarding order says "no load, no unload" only because boarding
				 * a train set it so (MOF_LOAD_ON_TRAIN); printing that is a bracket
				 * which tells the reader nothing and costs the width the station
				 * name needs. The line said it twice over and ran into the
				 * end-of-list line beneath it. See road_on_rail.h. */
				if (!order->GetNonStopType().Test(OrderNonStopFlag::GoVia) && !order->ShouldBoardAtStation()) {
					StringID str = _station_load_types[order->IsRefit()][to_underlying(unload)][to_underlying(load)];
					if (str != INVALID_STRING_ID) {
						if (order->IsRefit()) {
							line += GetString(str, order->IsAutoRefit() ? STR_ORDER_AUTO_REFIT_ANY : CargoSpec::Get(order->GetRefitCargo())->name);
						} else {
							line += GetString(str);
						}
					}
				}

				if (v->type == VehicleType::Train && !order->GetNonStopType().Test(OrderNonStopFlag::GoVia)) {
					/* Only show the stopping location if other than the default chosen by the player. */
					if (order->GetStopLocation() != _settings_client.gui.stop_location) {
						line += GetString(STR_ORDER_STOP_LOCATION_NEAR_END + to_underlying(order->GetStopLocation()));
					}
				}

				/* Make a "go to couple" order read as its own kind of order
				 * in the list, matching Palo123YPS's GUI, without it
				 * actually being a separate OrderType. See
				 * FEATURE_DESIGN_COUPLING_TOW.md. */
				if (v->type == VehicleType::Train && order->ShouldGoToCouple()) {
					second += GetString(order->ShouldSearchInRake() ? STR_ORDER_GOTO_COUPLE_SUFFIX_SEARCH : STR_ORDER_GOTO_COUPLE_SUFFIX);

					/* And what it is going to accept, for each filter that has
					 * been set. Read off the line, a whole list of orders says
					 * at a glance which engine is going for which wagons.
					 *
					 * Named short here, unlike on the buttons: "full" and
					 * "empty" can only be wagons and "coal" can only be a
					 * cargo, so the words "wagons" and "cargo" cost a third of
					 * the room and say nothing the reader did not already
					 * know. An order line is short and shares it with the
					 * station name. */
					if (order->GetCoupleLoad() != OrderCoupleLoad::Any) {
						second += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_PART, STR_ORDER_COUPLE_LOAD_SHORT_ANY + to_underlying(order->GetCoupleLoad()));
					}
					if (IsValidCargoType(order->GetCoupleCargo())) {
						second += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_CARGO, CargoSpec::Get(order->GetCoupleCargo())->name);
					}
					if (order->ShouldFoundRake()) {
						second += order->GetCoupleCount() != 0 ? GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_FOUND_COUNT, order->GetCoupleCount()) : GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_FOUND);
					} else if (order->GetCoupleCount() != 0) {
						/* Searching the rake counts the wagons found, not the
						 * vehicles in the rake, and "vehicles" would read it the
						 * old way: it says "such" instead. */
						bool such = order->ShouldSearchInRake();
						StringID reading = such ? STR_ORDER_COUPLE_FILTER_SUFFIX_COUNT_SUCH : STR_ORDER_COUPLE_FILTER_SUFFIX_COUNT;
						if (order->IsCoupleCountMinimum()) reading = such ? STR_ORDER_COUPLE_FILTER_SUFFIX_MIN_SUCH : STR_ORDER_COUPLE_FILTER_SUFFIX_MIN;
						if (order->IsCoupleCountMaximum()) reading = such ? STR_ORDER_COUPLE_FILTER_SUFFIX_MAX_SUCH : STR_ORDER_COUPLE_FILTER_SUFFIX_MAX;
						second += GetString(reading, order->GetCoupleCount());
					}
					/* And which type it couples, which a platform can be told as
					 * well as a shed. It was written on the shed's line only, so
					 * an order set at a platform said everything about itself
					 * except the one thing that had just been set. */
					if (const Engine *buy = Engine::GetIfValid(order->GetCoupleBuyEngine()); buy != nullptr) {
						/* A platform buys nothing, so no cargo said means every
						 * cargo -- couple this model whatever it is carrying.
						 * The model it comes out of the works with has nothing
						 * to do with anything here. */
						StringID suffix = IsValidCargoType(order->GetCoupleCargo())
								? STR_ORDER_COUPLE_BUY_ONLY_SUFFIX : STR_ORDER_COUPLE_BUY_ONLY_SUFFIX_ANY;
						second += GetString(suffix, PackEngineNameDParam(order->GetCoupleBuyEngine(), EngineNameContext::PurchaseList));
					}
				}

				/* Waiting for a couple had no way of showing at all, so the
				 * only place it could be read was a button that speaks for one
				 * order at a time. Everything an order is going to do belongs
				 * on its own line, where a whole list can be read at once. */
				if (v->type == VehicleType::Train && order->ShouldWaitForCouple()) second += GetString(STR_ORDER_WAIT_COUPLE_SUFFIX);
				/* On the order's own line, not on the second one under it. A train's
				 * couple suffixes go below because there can be a whole filter of
				 * them; a road vehicle has this one short word, and put below it
				 * was written across the end-of-list line beneath. */
				if (v->type == VehicleType::Road && order->ShouldBoardAtStation()) {
					line += GetString(STR_ORDER_BOARD_MODE_SUFFIX_TRAIN_TO_NEXT + to_underlying(order->GetBoardMode()) - 1);
				}

				/* How many vehicles stay with the train belongs on the order
				 * line with everything else the order is going to do. A button
				 * speaks for the one order selected, so a train that decouples
				 * at several stations learned nothing from it about any. */
				if (!timetable && v->type == VehicleType::Train && order->ShouldDecoupleOnDeparture()) {
					if (order->ShouldDecoupleWholeTrain()) {
						second += GetString(STR_ORDER_DECOUPLE_SUFFIX_WHOLE);
					} else {
						second += GetString(order->GetDecoupleCount() == 0 ? STR_ORDER_DECOUPLE_SUFFIX_ALL : STR_ORDER_DECOUPLE_SUFFIX, order->GetDecoupleCount());
					}
					/* And where their load is bound, on the same line: it is one
					 * of the things this order does, and a button only ever
					 * speaks for the one order that is selected. */
					if (order->GetDecoupleCargoDest() != StationID::Invalid() && Station::IsValidID(order->GetDecoupleCargoDest())) {
						second += GetString(STR_ORDER_DECOUPLE_CARGO_DEST_SUFFIX, order->GetDecoupleCargoDest());
					}
					/* And whether they are being put down at all or sold, which
					 * is the biggest thing an order can be doing to them. */
					if (order->ShouldSellDecoupled()) second += GetString(STR_ORDER_SELL_WAGONS_SUFFIX);
				}

				/* Reversing out is about where the train goes next, not about
				 * how long it stays, so it has no place in the timetable.
				 *
				 * Not shown when the order decouples here, because then it is
				 * not carried out (Vehicle::LeaveStation) -- an order can hold
				 * both flags, and saying so would be a plain lie about what the
				 * train is going to do. */
				if (!timetable && v->type == VehicleType::Train && order->ShouldReverseOutOfStation() &&
						!order->ShouldDecoupleOnDeparture()) {
					second += GetString(STR_ORDER_REVERSE_OUT_SUFFIX);
				}
				/* A decoupling order says "automatic" whatever its own flags
				 * hold, the player's word for it: how the engine leaves depends
				 * on how it came in -- it drops the wagons and goes away from
				 * them, backing off or pulling on -- and that is decided by the
				 * train, not by a button. Saying so on the line keeps the
				 * reader from looking for a reversing setting that is not there
				 * and not needed. */
				if (!timetable && v->type == VehicleType::Train &&
						(order->ShouldDepartAutomatically() || order->ShouldDecoupleOnDeparture())) {
					second += GetString(STR_ORDER_AUTO_DEPARTURE_SUFFIX);
				}
			}
			break;
		}

		case OT_GOTO_DEPOT:
			if (!order->GetDepotActionType().Test(OrderDepotActionFlag::NearestDepot)) {
				/* Going to a specific depot. */
				line = GetString(STR_ORDER_GO_TO_DEPOT_FORMAT, GetOrderGoToString(*order), v->type, order->GetDestination());
			} else if (v->type == VehicleType::Aircraft) {
				/* Going to the nearest hangar. */
				line = GetString(STR_ORDER_GO_TO_NEAREST_HANGAR_FORMAT, GetOrderGoToString(*order));
			} else {
				/* Going to the nearest depot. */
				line = GetString(STR_ORDER_GO_TO_NEAREST_DEPOT_FORMAT, GetOrderGoToString(*order), STR_ORDER_TRAIN_DEPOT + to_underlying(v->type));
			}

			/* Do not show stopping in the depot in the timetable window. */
			if (!timetable && order->GetDepotActionType().Test(OrderDepotActionFlag::Halt)) {
				line += GetString(STR_ORDER_STOP_ORDER);
			}

			/* Do not show refitting in the depot in the timetable window. */
			if (!timetable && order->IsRefit()) {
				line += GetString(order->GetDepotActionType().Test(OrderDepotActionFlag::Halt) ? STR_ORDER_REFIT_STOP_ORDER : STR_ORDER_REFIT_ORDER, CargoSpec::Get(order->GetRefitCargo())->name);
			}

			/* Show unbunching depot in both order and timetable windows. */
			if (order->GetDepotActionType().Test(OrderDepotActionFlag::Unbunch)) {
				line += GetString(STR_ORDER_WAIT_TO_UNBUNCH);
			}

			/* Turning around in the depot says which way the train comes out
			 * of it, not how long it stays, so it has no place in the
			 * timetable. See FEATURE_DESIGN_COUPLING_TOW.md. */
			if (!timetable && v->type == VehicleType::Train && order->ShouldTurnAroundInDepot()) {
				second += GetString(STR_ORDER_TURN_AROUND_DEPOT_SUFFIX);
			}

			/* A depot order that collects or puts down reads it off its own
			 * line, same as a station order does -- but in the order the work
			 * is really done in, which is the order the player would do it in
			 * by hand: leave first, then take on. */
			if (!timetable && v->type == VehicleType::Train && order->ShouldDecoupleOnDeparture()) {
				if (order->ShouldDecoupleWholeTrain()) {
					second += GetString(STR_ORDER_DEPOT_DECOUPLE_SUFFIX_WHOLE);
				} else {
					second += GetString(order->GetDecoupleCount() == 0 ? STR_ORDER_DEPOT_DECOUPLE_SUFFIX_ALL : STR_ORDER_DEPOT_DECOUPLE_SUFFIX, order->GetDecoupleCount());
				}
				if (order->ShouldSellDecoupled()) second += GetString(STR_ORDER_SELL_WAGONS_SUFFIX);
			}
			if (v->type == VehicleType::Train && order->ShouldGoToCouple()) {
				bool after_decouple = !timetable && order->ShouldDecoupleOnDeparture();
				second += GetString(after_decouple ? STR_ORDER_DEPOT_COUPLE_SUFFIX_AND : STR_ORDER_DEPOT_COUPLE_SUFFIX);
				if (order->GetCoupleLoad() != OrderCoupleLoad::Any) {
					second += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_PART, STR_ORDER_COUPLE_LOAD_SHORT_ANY + to_underlying(order->GetCoupleLoad()));
				}
				if (IsValidCargoType(order->GetCoupleCargo())) {
					second += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_CARGO, CargoSpec::Get(order->GetCoupleCargo())->name);
				}
				if (order->GetCoupleCount() != 0) {
					second += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_COUNT, order->GetCoupleCount());
				}
				/* And which wagon it buys when the shed is short, in a bracket
				 * of its own: it is not one of the filters, it is what the
				 * order does when the filters find too little. */
				if (const Engine *buy = Engine::GetIfValid(order->GetCoupleBuyEngine()); buy != nullptr) {
					/* With a wagon named, the cargo filter is off -- the two
					 * rule each other out -- so the line would say nothing at
					 * all about what this order hauls. The wagon's own cargo
					 * is written out beside it instead: read, not asked for,
					 * which is why it is inside the wagon's bracket and not a
					 * filter of its own. The player's line: "koupit typ uacs
					 * cement". */
					/* Only when the order has no cargo of its own. With one
					 * set, the line above has already said it -- and said the
					 * one that counts, the filter rather than what the model
					 * happens to come out of the works carrying. */
					/* Four strings rather than one with a piece slotted into it:
					 * a cargo is named by a StringID and so is the line it goes in,
					 * and a line built by hand and handed on as plain text is not
					 * something the string system can read back. Handed over that
					 * way it printed "invalid parameter", which is exactly what the
					 * player saw after the wagon type. */
					bool own_cargo = order->ShouldBuyWagons() && !IsValidCargoType(order->GetCoupleCargo())
							&& IsValidCargoType(buy->GetDefaultCargoType());
					/* Not buying and no cargo said is not "the one it comes out
					 * of the works with" -- nothing is coming out of any works.
					 * It is every cargo, and the line says so: couple this
					 * model whatever it happens to be carrying, and have them
					 * refitted in a shed afterwards. */
					bool any_cargo = !order->ShouldBuyWagons() && !IsValidCargoType(order->GetCoupleCargo());
					StringID suffix = order->ShouldBuyWagons()
							? (own_cargo ? STR_ORDER_COUPLE_BUY_SUFFIX_CARGO : STR_ORDER_COUPLE_BUY_SUFFIX)
							: (any_cargo ? STR_ORDER_COUPLE_BUY_ONLY_SUFFIX_ANY : STR_ORDER_COUPLE_BUY_ONLY_SUFFIX);
					auto engine = PackEngineNameDParam(order->GetCoupleBuyEngine(), EngineNameContext::PurchaseList);
					second += own_cargo
							? GetString(suffix, engine, CargoSpec::Get(buy->GetDefaultCargoType())->name)
							: GetString(suffix, engine);
				}
			}

			break;

		case OT_GOTO_WAYPOINT:
			line = GetString(order->GetNonStopType().Test(OrderNonStopFlag::NonStop) ? STR_ORDER_GO_NON_STOP_TO_WAYPOINT : STR_ORDER_GO_TO_WAYPOINT, order->GetDestination());
			/* A station waypoint says whether the train honks there, either
			 * way: the player wants the line under it never empty. */
			if (v->type == VehicleType::Train && IsStationWaypointOrder(order)) {
				second += GetString(order->ShouldHonk() ? STR_ORDER_HONK_SUFFIX : STR_ORDER_NO_HONK_SUFFIX);
			}
			break;

		case OT_CONDITIONAL:
			if (order->GetConditionVariable() == OrderConditionVariable::Unconditionally) {
				line = GetString(STR_ORDER_CONDITIONAL_UNCONDITIONAL, order->GetConditionSkipToOrder() + 1);
			} else {
				OrderConditionComparator occ = order->GetConditionComparator();

				uint value = order->GetConditionValue();
				if (order->GetConditionVariable() == OrderConditionVariable::MaxSpeed) value = ConvertSpeedToDisplaySpeed(value, v->type);

				line = GetString((occ == OrderConditionComparator::IsTrue || occ == OrderConditionComparator::IsFalse) ? STR_ORDER_CONDITIONAL_TRUE_FALSE : STR_ORDER_CONDITIONAL_NUM,
					order->GetConditionSkipToOrder() + 1,
					STR_ORDER_CONDITIONAL_LOAD_PERCENTAGE + to_underlying(order->GetConditionVariable()),
					STR_ORDER_CONDITIONAL_COMPARATOR_EQUALS + to_underlying(occ),
					value);
			}

			if (timetable && order->GetWaitTime() > 0) {
				auto [str, value] = GetTimetableParameters(order->GetWaitTime());
				line += GetString(order->IsWaitTimetabled() ? STR_TIMETABLE_AND_TRAVEL_FOR : STR_TIMETABLE_AND_TRAVEL_FOR_ESTIMATED, str, value);
			}
			break;

		default: NOT_REACHED();
	}

	/* Check range for aircraft. */
	if (v->type == VehicleType::Aircraft && Aircraft::From(v)->GetRange() > 0 && order->IsGotoOrder()) {
		if (GetOrderDistance(order_index, v->orders->GetNext(order_index), v) > Aircraft::From(v)->acache.cached_max_range_sqr) {
			line += GetString(STR_ORDER_OUT_OF_RANGE);
		}
	}

	if (timetable || second.empty()) {
		DrawString(rtl ? left : middle, rtl ? middle : right, y, line + second, colour);
	} else {
		DrawString(rtl ? left : middle, rtl ? middle : right, y, line, colour);
		DrawString(rtl ? left : middle, rtl ? middle : right, y + GetCharacterHeight(FontSize::Normal), second, colour);
	}
}

/**
 * Whether the order list shows this order on two lines: the second carries
 * what this feature adds (see the matching parts in DrawOrderString()).
 */
bool OrderHasSecondLine(const Vehicle *v, const Order *order)
{
	if (v->type != VehicleType::Train) return false;
	if (order->IsType(OT_GOTO_WAYPOINT)) return IsStationWaypointOrder(order);
	if (order->IsType(OT_GOTO_STATION)) {
		return order->ShouldGoToCouple() || order->ShouldWaitForCouple() || order->ShouldDecoupleOnDeparture() ||
				order->ShouldReverseOutOfStation() || order->ShouldDepartAutomatically();
	}
	if (order->IsType(OT_GOTO_DEPOT)) {
		return order->ShouldTurnAroundInDepot() || order->ShouldDecoupleOnDeparture() || order->ShouldGoToCouple();
	}
	return false;
}

/**
 * Get the order command a vehicle can do in a given tile.
 * @param v Vehicle involved.
 * @param tile Tile being queried.
 * @return The order associated to vehicle v in given tile (or empty order if vehicle can do nothing in the tile).
 */
static Order GetOrderCmdFromTile(const Vehicle *v, TileIndex tile)
{
	Order order{};

	/* check depot first */
	if (IsDepotTypeTile(tile, (TransportType)(uint)v->type) && IsTileOwner(tile, _local_company)) {
		order.MakeGoToDepot(GetDepotDestinationIndex(tile),
				OrderDepotTypeFlag::PartOfOrders,
				(_settings_client.gui.new_nonstop && v->IsGroundVehicle()) ? OrderNonStopFlag::NonStop : OrderNonStopFlags{});

		if (_ctrl_pressed) {
			/* Now we are allowed to set the action type. */
			order.SetDepotActionType(OrderDepotActionFlag::Unbunch);
		}

		return order;
	}

	/* check rail waypoint */
	if (IsRailWaypointTile(tile) &&
			v->type == VehicleType::Train &&
			IsTileOwner(tile, _local_company)) {
		order.MakeGoToWaypoint(GetStationIndex(tile));
		if (_settings_client.gui.new_nonstop != _ctrl_pressed) order.SetNonStopType({OrderNonStopFlag::NonStop, OrderNonStopFlag::GoVia});
		return order;
	}

	/* check road waypoint */
	if (IsRoadWaypointTile(tile) &&
			v->type == VehicleType::Road &&
			IsTileOwner(tile, _local_company)) {
		order.MakeGoToWaypoint(GetStationIndex(tile));
		if (_settings_client.gui.new_nonstop != _ctrl_pressed) order.SetNonStopType({OrderNonStopFlag::NonStop, OrderNonStopFlag::GoVia});
		return order;
	}

	/* check buoy (no ownership) */
	if (IsBuoyTile(tile) && v->type == VehicleType::Ship) {
		order.MakeGoToWaypoint(GetStationIndex(tile));
		return order;
	}

	/* check for station or industry with neutral station */
	if (IsTileType(tile, TileType::Station) || IsTileType(tile, TileType::Industry)) {
		const Station *st = nullptr;

		if (IsTileType(tile, TileType::Station)) {
			st = Station::GetByTile(tile);
		} else {
			const Industry *in = Industry::GetByTile(tile);
			st = in->neutral_station;
		}
		if (st != nullptr && (st->owner == _local_company || st->owner == OWNER_NONE)) {
			StationFacilities facil;
			switch (v->type) {
				case VehicleType::Ship:     facil = StationFacility::Dock;    break;
				case VehicleType::Train:    facil = StationFacility::Train;   break;
				case VehicleType::Aircraft: facil = StationFacility::Airport; break;
				case VehicleType::Road:     facil = {StationFacility::BusStop, StationFacility::TruckStop}; break;
				default: NOT_REACHED();
			}
			if (st->facilities.Any(facil)) {
				order.MakeGoToStation(st->index);
				if (_ctrl_pressed) order.SetLoadType(OrderLoadType::FullLoadAny);
				if (_settings_client.gui.new_nonstop && v->IsGroundVehicle()) order.SetNonStopType(OrderNonStopFlag::NonStop);
				order.SetStopLocation(v->type == VehicleType::Train ? (OrderStopLocation)(_settings_client.gui.stop_location) : OrderStopLocation::FarEnd);
				return order;
			}
		}
	}

	/* not found */
	order.Free();
	return order;
}

/** Hotkeys for order window. */
enum OrderHotKeys : int32_t {
	OHK_SKIP,
	OHK_DELETE,
	OHK_GOTO,
	OHK_NONSTOP,
	OHK_FULLLOAD,
	OHK_UNLOAD,
	OHK_NEAREST_DEPOT,
	OHK_ALWAYS_SERVICE,
	OHK_TRANSFER,
	OHK_NO_UNLOAD,
	OHK_NO_LOAD,
};

/**
 * %Order window code for all vehicles.
 *
 * At the bottom of the window two button rows are located for changing the orders of the vehicle.
 *
 * \section top-row Top row
 * The top-row is for manipulating an individual order. What row is displayed depends on the type of vehicle, and whether or not you are the owner of the vehicle.
 *
 * The top-row buttons of one of your trains or road vehicles is one of the following three cases:
 * \verbatim
 * +-----------------+-----------------+-----------------+-----------------+
 * |    NON-STOP     |    FULL_LOAD    |     UNLOAD      |      REFIT      | (normal)
 * +-----------------+-----+-----------+-----------+-----+-----------------+
 * |       COND_VAR        |    COND_COMPARATOR    |      COND_VALUE       | (for conditional orders)
 * +-----------------+-----+-----------+-----------+-----+-----------------+
 * |    NON-STOP     |      REFIT      |     SERVICE     |     (empty)     | (for depot orders)
 * +-----------------+-----------------+-----------------+-----------------+
 * \endverbatim
 *
 * Airplanes and ships have one of the following three top-row button rows:
 * \verbatim
 * +-----------------+-----------------+-----------------+
 * |    FULL_LOAD    |     UNLOAD      |      REFIT      | (normal)
 * +-----------------+-----------------+-----------------+
 * |    COND_VAR     | COND_COMPARATOR |   COND_VALUE    | (for conditional orders)
 * +-----------------+--------+--------+-----------------+
 * |            REFIT         |          SERVICE         | (for depot order)
 * +--------------------------+--------------------------+
 * \endverbatim
 *
 * \section bottom-row Bottom row
 * The second row (the bottom row) is for manipulating the list of orders:
 * \verbatim
 * +-----------------+-----------------+-----------------+
 * |      SKIP       |     DELETE      |      GOTO       |
 * +-----------------+-----------------+-----------------+
 * \endverbatim
 *
 * For vehicles of other companies, both button rows are not displayed.
 */
struct OrdersWindow : public Window {
	/* Rows are two lines high while any order of this vehicle has a second
	 * line to show (OrderHasSecondLine()); the scrollbar only knows uniform
	 * rows, so all rows grow together. */
	bool two_line_orders = false;

private:
	/** Under what reason are we using the PlaceObject functionality? */
	enum OrderPlaceObjectState : uint8_t {
		OPOS_NONE,
		OPOS_GOTO,
		OPOS_CONDITIONAL,
		OPOS_SHARE,
		OPOS_DECOUPLE_DEST, ///< Picking the station whose cargo a decoupling order's wagons are to load.
		OPOS_END,
	};

	/** Displayed planes of the #NWID_SELECTION widgets. */
	enum DisplayPane : uint8_t {
		/* WID_O_SEL_TOP_ROW_GROUNDVEHICLE */
		DP_GROUNDVEHICLE_ROW_NORMAL      = 0, ///< Display the row for normal/depot orders in the top row of the train/rv order window.
		DP_GROUNDVEHICLE_ROW_CONDITIONAL = 1, ///< Display the row for conditional orders in the top row of the train/rv order window.

		/* WID_O_SEL_TOP_LEFT */
		DP_LEFT_LOAD       = 0, ///< Display 'load' in the left button of the top row of the train/rv order window.
		DP_LEFT_REFIT      = 1, ///< Display 'refit' in the left button of the top row of the train/rv order window.

		/* WID_O_SEL_TOP_MIDDLE */
		DP_MIDDLE_UNLOAD   = 0, ///< Display 'unload' in the middle button of the top row of the train/rv order window.
		DP_MIDDLE_SERVICE  = 1, ///< Display 'service' in the middle button of the top row of the train/rv order window.

		/* WID_O_SEL_TOP_RIGHT */
		DP_RIGHT_EMPTY     = 0, ///< Display an empty panel in the right button of the top row of the train/rv order window.
		DP_RIGHT_REFIT     = 1, ///< Display 'refit' in the right button of the top  row of the train/rv order window.

		/* WID_O_SEL_TOP_ROW */
		DP_ROW_LOAD        = 0, ///< Display 'load' / 'unload' / 'refit' buttons in the top row of the ship/airplane order window.
		DP_ROW_DEPOT       = 1, ///< Display 'refit' / 'service' buttons in the top row of the ship/airplane order window.
		DP_ROW_CONDITIONAL = 2, ///< Display the conditional order buttons in the top row of the ship/airplane order window.

		/* WID_O_SEL_BOTTOM_MIDDLE */
		DP_BOTTOM_MIDDLE_DELETE       = 0, ///< Display 'delete' in the middle button of the bottom row of the vehicle order window.
		DP_BOTTOM_MIDDLE_STOP_SHARING = 1, ///< Display 'stop sharing' in the middle button of the bottom row of the vehicle order window.

		/* WID_O_SEL_DECOUPLE */
		DP_COUPLE_ROW_STATION = 0, ///< Display the decouple/couple buttons for a train's station order.
		DP_COUPLE_ROW_DEPOT   = 1, ///< Display the turn-around button for a train's depot order.
		DP_COUPLE_ROW_WAYPOINT = 2, ///< Display the horn button for a train's station waypoint order.
		DP_COUPLE_ROW_EMPTY   = 3, ///< Hold the row's height open when it has no buttons to show.
		DP_COUPLE_ROW_ROAD    = 4, ///< Display the 'load onto train' button for a road vehicle's station order.
	};

	int selected_order = -1;
	/** Is the currently-open query string editing the decouple count (WID_O_DECOUPLE_COUNT) rather than a conditional order value (WID_O_COND_VALUE)? Both use the same OnQueryTextFinished. */
	bool querying_decouple_count = false;
	/** Same again for the number of vehicles a coupling order will accept (WID_O_COUPLE_COUNT). */
	bool querying_couple_count = false;
	/** The filter row has appeared or gone, so the window has to be laid out again at a moment when nothing is being delivered to it. */
	bool couple_filter_resized = false;
	VehicleOrderID order_over = INVALID_VEH_ORDER_ID; ///< Order over which another order is dragged, \c INVALID_VEH_ORDER_ID if none.
	/**
	 * Has the pointer actually been dragged since the selected order was picked
	 * up? Only then does letting go move it.
	 *
	 * Selecting an order arms drag-and-drop, and the drop is delivered wherever
	 * the button comes up -- which is not where it went down if the hand moved a
	 * few pixels in between. On a touch screen it always does. So an order that
	 * was merely being selected, on a row boundary, was picked up and put down
	 * one row along, and the player was left with a reordered list he never
	 * asked for while trying to click the order below. Grabbing and dragging is
	 * the gesture that moves an order; a tap is not.
	 */
	bool order_dragged = false;
	OrderPlaceObjectState goto_type = OPOS_NONE;
	const Vehicle *vehicle = nullptr; ///< Vehicle owning the orders being displayed and manipulated.
	Scrollbar *vscroll = nullptr;
	bool can_do_refit = false; ///< Vehicle chain can be refitted in depot.
	bool can_do_autorefit = false; ///< Vehicle chain can be auto-refitted.

	/**
	 * Return the memorised selected order.
	 * @return the memorised order if it is a valid one
	 *  else return the number of orders
	 */
	VehicleOrderID OrderGetSel() const
	{
		int num = this->selected_order;
		return (num >= 0 && num < vehicle->GetNumOrders()) ? num : vehicle->GetNumOrders();
	}

	/**
	 * Calculate the selected order.
	 * The calculation is based on the relative (to the window) y click position and
	 *  the position of the scrollbar.
	 *
	 * @param y Y-value of the click relative to the window origin
	 * @return The selected order if the order is valid, else return \c INVALID_VEH_ORDER_ID.
	 */
	VehicleOrderID GetOrderFromPt(int y)
	{
		int32_t sel = this->vscroll->GetScrolledRowFromWidget(y, this, WID_O_ORDER_LIST, WidgetDimensions::scaled.framerect.top);
		if (sel == INT32_MAX) return INVALID_VEH_ORDER_ID;
		/* One past the orders is the 'End of Orders' line. */
		assert(IsInsideBS(sel, 0, vehicle->GetNumOrders() + 1));
		return sel;
	}

	/**
	 * Handle the click on the goto button.
	 * @param type The variant of goto button/dropdown options.
	 */
	void OrderClick_Goto(OrderPlaceObjectState type)
	{
		assert(type > OPOS_NONE && type < OPOS_END);

		static const HighLightStyle goto_place_style[OPOS_END - 1] = {
			HT_RECT | HT_VEHICLE, // OPOS_GOTO
			HT_NONE,              // OPOS_CONDITIONAL
			HT_VEHICLE,           // OPOS_SHARE
			HT_RECT,              // OPOS_DECOUPLE_DEST
		};
		SetObjectToPlaceWnd(ANIMCURSOR_PICKSTATION, PAL_NONE, goto_place_style[type - 1], this);
		this->goto_type = type;
		this->SetWidgetDirty(WID_O_GOTO);
	}

	/**
	 * Handle the click on the full load button.
	 * @param load_type Load flag to apply. If matches existing load type, toggles to default of 'load if possible'.
	 * @param toggle If we toggle or not (used for hotkey behavior)
	 */
	void OrderClick_FullLoad(OrderLoadType load_type, bool toggle = false)
	{
		VehicleOrderID sel_ord = this->OrderGetSel();
		const Order *order = this->vehicle->GetOrder(sel_ord);

		if (order == nullptr) return;

		if (toggle && order->GetLoadType() == load_type) {
			load_type = OrderLoadType::LoadIfPossible; // reset to 'default'
		}
		if (order->GetLoadType() == load_type) return; // If we still match, do nothing

		Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel_ord, MOF_LOAD, to_underlying(load_type));
	}

	/**
	 * Handle the click on the service.
	 * @param i The optional depot action to modify the order with.
	 */
	void OrderClick_Service(std::optional<OrderDepotAction> i)
	{
		VehicleOrderID sel_ord = this->OrderGetSel();

		if (!i.has_value()) {
			const Order *order = this->vehicle->GetOrder(sel_ord);
			if (order == nullptr) return;
			i = order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service) ? OrderDepotAction::AlwaysGo : OrderDepotAction::Service;
		}
		Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel_ord, MOF_DEPOT_ACTION, to_underlying(i.value()));
	}

	/**
	 * Handle the click on the service in nearest depot button.
	 */
	void OrderClick_NearestDepot()
	{
		Order order{};
		order.MakeGoToDepot(DepotID::Invalid(), OrderDepotTypeFlag::PartOfOrders,
				_settings_client.gui.new_nonstop && this->vehicle->IsGroundVehicle() ? OrderNonStopFlag::NonStop : OrderNonStopFlags{});
		order.SetDepotActionType(OrderDepotActionFlag::NearestDepot);

		Command<Commands::InsertOrder>::Post(STR_ERROR_CAN_T_INSERT_NEW_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), order);
	}

	/**
	 * Handle the click on the unload button.
	 * @param unload_type Unload flag to apply. If matches existing unload type, toggles to default of 'unload if possible'.
	 * @param toggle If we toggle or not (used for hotkey behavior)
	 */
	void OrderClick_Unload(OrderUnloadType unload_type, bool toggle = false)
	{
		VehicleOrderID sel_ord = this->OrderGetSel();
		const Order *order = this->vehicle->GetOrder(sel_ord);

		if (order == nullptr) return;

		if (toggle && order->GetUnloadType() == unload_type) {
			unload_type = OrderUnloadType::UnloadIfPossible;
		}
		if (order->GetUnloadType() == unload_type) return; // If we still match, do nothing

		Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel_ord, MOF_UNLOAD, to_underlying(unload_type));

		/* Transfer and unload orders with leave empty as default */
		if (unload_type == OrderUnloadType::Transfer || unload_type == OrderUnloadType::Unload) {
			Command<Commands::ModifyOrder>::Post(this->vehicle->tile, this->vehicle->index, sel_ord, MOF_LOAD, to_underlying(OrderLoadType::NoLoad));
			this->SetWidgetDirty(WID_O_FULL_LOAD);
		}
	}

	/**
	 * Handle the click on the nonstop button.
	 * @param non_stop what non-stop type to use; std::nullopt to use the 'next' one.
	 */
	void OrderClick_Nonstop(std::optional<OrderNonStopFlags> non_stop)
	{
		if (!this->vehicle->IsGroundVehicle()) return;

		VehicleOrderID sel_ord = this->OrderGetSel();
		const Order *order = this->vehicle->GetOrder(sel_ord);

		if (order == nullptr || order->GetNonStopType() == non_stop) return;

		/* Keypress if no value, so 'toggle' to the next */
		if (!non_stop.has_value()) {
			non_stop = order->GetNonStopType().Flip(OrderNonStopFlag::NonStop);
		}

		this->SetWidgetDirty(WID_O_NON_STOP);
		Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel_ord, MOF_NON_STOP, non_stop.value().base());
	}

	/**
	 * Handle the click on the skip button.
	 * If ctrl is pressed, skip to selected order, else skip to current order + 1
	 */
	void OrderClick_Skip()
	{
		/* Don't skip when there's nothing to skip */
		if (_ctrl_pressed && this->vehicle->cur_implicit_order_index == this->OrderGetSel()) return;
		if (this->vehicle->GetNumOrders() <= 1) return;

		Command<Commands::SkipToOrder>::Post(_ctrl_pressed ? STR_ERROR_CAN_T_SKIP_TO_ORDER : STR_ERROR_CAN_T_SKIP_ORDER,
				this->vehicle->tile, this->vehicle->index, _ctrl_pressed ? this->OrderGetSel() : ((this->vehicle->cur_implicit_order_index + 1) % this->vehicle->GetNumOrders()));
	}

	/**
	 * Handle the click on the back button: the opposite of skip, to the
	 * previous order. Steps over implicit orders the same way skip does --
	 * one place in the list -- so the two are mirror images of each other.
	 */
	void OrderClick_Back()
	{
		uint num = this->vehicle->GetNumOrders();
		if (num <= 1) return;

		Command<Commands::SkipToOrder>::Post(STR_ERROR_CAN_T_SKIP_ORDER,
				this->vehicle->tile, this->vehicle->index, (this->vehicle->cur_implicit_order_index + num - 1) % num);
	}

	/**
	 * Handle the click on the delete button.
	 */
	void OrderClick_Delete()
	{
		/* When networking, move one order lower */
		int selected = this->selected_order + (int)_networking;

		if (Command<Commands::DeleteOrder>::Post(STR_ERROR_CAN_T_DELETE_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel())) {
			this->selected_order = selected >= this->vehicle->GetNumOrders() ? -1 : selected;
			this->UpdateButtonState();
		}
	}

	/**
	 * Handle the click on the 'stop sharing' button.
	 * If 'End of Shared Orders' isn't selected, do nothing. If Ctrl is pressed, call OrderClick_Delete and exit.
	 * To stop sharing this vehicle order list, we copy the orders of a vehicle that share this order list. That way we
	 * exit the group of shared vehicles while keeping the same order list.
	 */
	void OrderClick_StopSharing()
	{
		/* Don't try to stop sharing orders if 'End of Shared Orders' isn't selected. */
		if (!this->vehicle->IsOrderListShared() || this->selected_order != this->vehicle->GetNumOrders()) return;
		/* If Ctrl is pressed, delete the order list as if we clicked the 'Delete' button. */
		if (_ctrl_pressed) {
			this->OrderClick_Delete();
			return;
		}

		/* Get another vehicle that share orders with this vehicle. */
		Vehicle *other_shared = (this->vehicle->FirstShared() == this->vehicle) ? this->vehicle->NextShared() : this->vehicle->PreviousShared();
		/* Copy the order list of the other vehicle. */
		if (Command<Commands::CloneOrder>::Post(STR_ERROR_CAN_T_STOP_SHARING_ORDER_LIST, this->vehicle->tile, CO_COPY, this->vehicle->index, other_shared->index)) {
			this->UpdateButtonState();
		}
	}

	/**
	 * Handle the click on the refit button.
	 * If ctrl is pressed, cancel refitting, else show the refit window.
	 * @param i Selected refit command.
	 * @param auto_refit Select refit for auto-refitting.
	 */
	void OrderClick_Refit(int i, bool auto_refit)
	{
		if (_ctrl_pressed) {
			/* Cancel refitting */
			Command<Commands::OrderRefit>::Post(this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), CARGO_NO_REFIT);
		} else {
			if (i == 1) { // Auto-refit to available cargo type.
				Command<Commands::OrderRefit>::Post(this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), CARGO_AUTO_REFIT);
			} else {
				ShowVehicleRefitWindow(this->vehicle, this->OrderGetSel(), this, auto_refit);
			}
		}
	}

	/** Cache auto-refittability of the vehicle chain. */
	void UpdateAutoRefitState()
	{
		this->can_do_refit = false;
		this->can_do_autorefit = false;
		for (const Vehicle *w = this->vehicle; w != nullptr; w = w->IsGroundVehicle() ? w->Next() : nullptr) {
			if (IsEngineRefittable(w->engine_type)) this->can_do_refit = true;
			if (Engine::Get(w->engine_type)->info.misc_flags.Test(EngineMiscFlag::AutoRefit)) this->can_do_autorefit = true;
		}
	}

public:
	OrdersWindow(WindowDesc &desc, const Vehicle *v) : Window(desc)
	{
		this->vehicle = v;

		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_O_SCROLLBAR);
		if (NWidgetCore *nwid = this->GetWidget<NWidgetCore>(WID_O_DEPOT_ACTION); nwid != nullptr) {
			nwid->SetToolTip(STR_ORDER_TRAIN_DEPOT_ACTION_TOOLTIP + to_underlying(v->type));
		}
		/* The filter row belongs to an order that is going to collect something,
		 * and no order is selected yet. Saying so before the window is laid out
		 * means it is built the right height from the first frame. */
		if (NWidgetStacked *filter_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_COUPLE_FILTER); filter_sel != nullptr) {
			filter_sel->SetDisplayedPlane(SZSP_NONE);
		}
		/* The bottom row itself stays for a train, because the sell button in it
		 * belongs to the train and not to any one order. Only its middle place
		 * comes and goes: the cargo destination button, or nothing, and no order
		 * is selected yet. */
		if (NWidgetStacked *dest_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_DECOUPLE_DEST_BTN); dest_sel != nullptr) {
			dest_sel->SetDisplayedPlane(1);
		}
		if (NWidgetStacked *sell_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_SELL_WAGONS); sell_sel != nullptr) {
			sell_sel->SetDisplayedPlane(1);
		}
		this->FinishInitNested(v->index);

		this->owner = v->owner;

		this->UpdateAutoRefitState();

		if (_settings_client.gui.quick_goto && v->owner == _local_company) {
			/* If there are less than 2 station, make Go To active. */
			int station_orders = std::ranges::count_if(v->Orders(), [](const Order &order) { return order.IsType(OT_GOTO_STATION); });

			if (station_orders < 2) this->OrderClick_Goto(OPOS_GOTO);
		}
		this->OnInvalidateData(VIWD_MODIFY_ORDERS);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_O_ORDER_LIST:
				fill.height = resize.height = GetCharacterHeight(FontSize::Normal) * (this->two_line_orders ? 2 : 1);
				size.height = 6 * GetCharacterHeight(FontSize::Normal) + padding.height;
				break;

			case WID_O_COND_VARIABLE: {
				Dimension d = {0, 0};
				for (const auto &ocv : _order_conditional_variable) {
					d = maxdim(d, GetStringBoundingBox(STR_ORDER_CONDITIONAL_LOAD_PERCENTAGE + to_underlying(ocv)));
				}
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_O_COND_COMPARATOR: {
				Dimension d = GetStringListBoundingBox(_order_conditional_condition);
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_O_DELETE: {
				Dimension d = maxdim(GetStringBoundingBox(STR_ORDERS_DELETE_BUTTON), GetStringBoundingBox(STR_ORDERS_DELETE_ALL_BUTTON));
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}
		}
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		VehicleOrderID from = INVALID_VEH_ORDER_ID;
		VehicleOrderID to   = INVALID_VEH_ORDER_ID;

		switch (data) {
			case VIWD_AUTOREPLACE:
				/* Autoreplace replaced the vehicle */
				this->vehicle = Vehicle::Get(this->window_number);
				[[fallthrough]];

			case VIWD_CONSIST_CHANGED:
				/* Vehicle composition was changed. */
				this->UpdateAutoRefitState();
				break;

			case VIWD_REMOVE_ALL_ORDERS:
				/* Removed / replaced all orders (after deleting / sharing) */
				if (this->selected_order == -1) break;

				this->CloseChildWindows();
				this->selected_order = -1;
				break;

			case VIWD_MODIFY_ORDERS:
				/* Some other order changes */
				break;

			default:
				if (data < 0) break;

				if (gui_scope) break; // only do this once; from command scope
				from = GB(data, 0, 8);
				to   = GB(data, 8, 8);
				/* Moving an order. If one of these is INVALID_VEH_ORDER_ID, then
				 * the order is being created / removed */
				if (this->selected_order == -1) break;

				if (from == to) break; // no need to change anything

				if (from != this->selected_order) {
					/* Moving from preceding order? */
					this->selected_order -= (int)(from <= this->selected_order);
					/* Moving to   preceding order? */
					this->selected_order += (int)(to   <= this->selected_order);
					break;
				}

				/* Now we are modifying the selected order */
				if (to == INVALID_VEH_ORDER_ID) {
					/* Deleting selected order */
					this->CloseChildWindows();
					this->selected_order = -1;
					break;
				}

				/* Moving selected order */
				this->selected_order = to;
				break;
		}

		this->vscroll->SetCount(this->vehicle->GetNumOrders() + 1);
		if (gui_scope) {
			bool two = false;
			for (const Order &o : this->vehicle->Orders()) {
				if (OrderHasSecondLine(this->vehicle, &o)) {
					two = true;
					break;
				}
			}
			if (two != this->two_line_orders) {
				this->two_line_orders = two;
				this->ReInit();
			}
		}
		if (gui_scope) this->UpdateButtonState();

		/* Scroll to the new order. */
		if (from == INVALID_VEH_ORDER_ID && to != INVALID_VEH_ORDER_ID && !this->vscroll->IsVisible(to)) {
			this->vscroll->ScrollTowards(to);
		}
	}

	void UpdateButtonState()
	{
		if (this->vehicle->owner != _local_company) return; // No buttons are displayed with competitor order windows.

		bool shared_orders = this->vehicle->IsOrderListShared();
		VehicleOrderID sel = this->OrderGetSel();
		const Order *order = this->vehicle->GetOrder(sel);

		/* Second row. */
		/* skip, and back */
		this->SetWidgetDisabledState(WID_O_SKIP, this->vehicle->GetNumOrders() <= 1);
		this->SetWidgetDisabledState(WID_O_BACK, this->vehicle->GetNumOrders() <= 1);

		/* delete / stop sharing */
		NWidgetStacked *delete_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_BOTTOM_MIDDLE);
		if (shared_orders && this->selected_order == this->vehicle->GetNumOrders()) {
			/* The 'End of Shared Orders' order is selected, show the 'stop sharing' button. */
			delete_sel->SetDisplayedPlane(DP_BOTTOM_MIDDLE_STOP_SHARING);
		} else {
			/* The 'End of Shared Orders' order isn't selected, show the 'delete' button. */
			delete_sel->SetDisplayedPlane(DP_BOTTOM_MIDDLE_DELETE);
			this->SetWidgetDisabledState(WID_O_DELETE,
				(uint)this->vehicle->GetNumOrders() + ((shared_orders || this->vehicle->GetNumOrders() != 0) ? 1 : 0) <= (uint)this->selected_order);

			/* Set the tooltip of the 'delete' button depending on whether the
			 * 'End of Orders' order or a regular order is selected. */
			NWidgetCore *nwi = this->GetWidget<NWidgetCore>(WID_O_DELETE);
			if (this->selected_order == this->vehicle->GetNumOrders()) {
				nwi->SetStringTip(STR_ORDERS_DELETE_ALL_BUTTON, STR_ORDERS_DELETE_ALL_TOOLTIP);
			} else {
				nwi->SetStringTip(STR_ORDERS_DELETE_BUTTON, STR_ORDERS_DELETE_TOOLTIP);
			}
		}

		/* First row. */
		this->RaiseWidget(WID_O_FULL_LOAD);
		this->RaiseWidget(WID_O_UNLOAD);

		/* Selection widgets. */
		/* Train or road vehicle. */
		NWidgetStacked *train_row_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_TOP_ROW_GROUNDVEHICLE);
		NWidgetStacked *left_sel      = this->GetWidget<NWidgetStacked>(WID_O_SEL_TOP_LEFT);
		NWidgetStacked *middle_sel    = this->GetWidget<NWidgetStacked>(WID_O_SEL_TOP_MIDDLE);
		NWidgetStacked *right_sel     = this->GetWidget<NWidgetStacked>(WID_O_SEL_TOP_RIGHT);
		/* Ship or airplane. */
		NWidgetStacked *row_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_TOP_ROW);
		assert(row_sel != nullptr || (train_row_sel != nullptr && left_sel != nullptr && middle_sel != nullptr && right_sel != nullptr));


		if (order == nullptr) {
			if (row_sel != nullptr) {
				row_sel->SetDisplayedPlane(DP_ROW_LOAD);
			} else {
				train_row_sel->SetDisplayedPlane(DP_GROUNDVEHICLE_ROW_NORMAL);
				left_sel->SetDisplayedPlane(DP_LEFT_LOAD);
				middle_sel->SetDisplayedPlane(DP_MIDDLE_UNLOAD);
				right_sel->SetDisplayedPlane(DP_RIGHT_EMPTY);
				this->DisableWidget(WID_O_NON_STOP);
				this->RaiseWidget(WID_O_NON_STOP);
			}
			this->DisableWidget(WID_O_FULL_LOAD);
			this->DisableWidget(WID_O_UNLOAD);
			this->DisableWidget(WID_O_REFIT_DROPDOWN);
		} else {
			this->SetWidgetDisabledState(WID_O_FULL_LOAD, order->GetNonStopType().Test(OrderNonStopFlag::GoVia)); // full load
			this->SetWidgetDisabledState(WID_O_UNLOAD,    order->GetNonStopType().Test(OrderNonStopFlag::GoVia)); // unload

			switch (order->GetType()) {
				case OT_GOTO_STATION:
					if (row_sel != nullptr) {
						row_sel->SetDisplayedPlane(DP_ROW_LOAD);
					} else {
						train_row_sel->SetDisplayedPlane(DP_GROUNDVEHICLE_ROW_NORMAL);
						left_sel->SetDisplayedPlane(DP_LEFT_LOAD);
						middle_sel->SetDisplayedPlane(DP_MIDDLE_UNLOAD);
						right_sel->SetDisplayedPlane(DP_RIGHT_REFIT);
						this->EnableWidget(WID_O_NON_STOP);
						this->SetWidgetLoweredState(WID_O_NON_STOP, order->GetNonStopType().Test(OrderNonStopFlag::NonStop));
					}
					this->SetWidgetLoweredState(WID_O_FULL_LOAD, order->GetLoadType() == OrderLoadType::FullLoadAny);
					this->SetWidgetLoweredState(WID_O_UNLOAD, order->GetUnloadType() == OrderUnloadType::Unload);

					/* A "go to couple" order starts out as "no load, no unload" --
					 * the command writes that the moment the switch goes on, since
					 * an engine collects and goes, and whether the wagons come
					 * loaded is the couple filter's question. But it is a start,
					 * not a rule: a passenger set that has just been assembled
					 * from two halves does load before it leaves, and the player
					 * says so with these two, which therefore stay live. */

					/* Can only do refitting when stopping at the destination and loading cargo.
					 * Also enable the button if a refit is already set to allow clearing it. */
					/* A train is never refitted at a platform. What it hauls is
					 * decided by what it couples and lets go of, and both of
					 * those happen at a platform too -- the rake that arrives
					 * is not the rake that leaves, so a refit told to happen on
					 * arrival is aimed at vehicles that may not be there any
					 * more. The player's own line: at a station the refit is
					 * greyed always, and the refitting is done in a shed. Other
					 * kinds of vehicle keep the ordinary rule. */
					this->SetWidgetDisabledState(WID_O_REFIT_DROPDOWN,
							this->vehicle->type == VehicleType::Train ||
							order->GetLoadType() == OrderLoadType::NoLoad || order->GetNonStopType().Test(OrderNonStopFlag::GoVia) ||
							((!this->can_do_refit || !this->can_do_autorefit) && !order->IsRefit()));

					break;

				case OT_GOTO_WAYPOINT:
					if (row_sel != nullptr) {
						row_sel->SetDisplayedPlane(DP_ROW_LOAD);
					} else {
						train_row_sel->SetDisplayedPlane(DP_GROUNDVEHICLE_ROW_NORMAL);
						left_sel->SetDisplayedPlane(DP_LEFT_LOAD);
						middle_sel->SetDisplayedPlane(DP_MIDDLE_UNLOAD);
						right_sel->SetDisplayedPlane(DP_RIGHT_EMPTY);
						this->EnableWidget(WID_O_NON_STOP);
						this->SetWidgetLoweredState(WID_O_NON_STOP, order->GetNonStopType().Test(OrderNonStopFlag::NonStop));
					}
					this->DisableWidget(WID_O_FULL_LOAD);
					this->DisableWidget(WID_O_UNLOAD);
					this->DisableWidget(WID_O_REFIT_DROPDOWN);
					break;

				case OT_GOTO_DEPOT:
					if (row_sel != nullptr) {
						row_sel->SetDisplayedPlane(DP_ROW_DEPOT);
					} else {
						train_row_sel->SetDisplayedPlane(DP_GROUNDVEHICLE_ROW_NORMAL);
						left_sel->SetDisplayedPlane(DP_LEFT_REFIT);
						middle_sel->SetDisplayedPlane(DP_MIDDLE_SERVICE);
						right_sel->SetDisplayedPlane(DP_RIGHT_EMPTY);
						this->EnableWidget(WID_O_NON_STOP);
						this->SetWidgetLoweredState(WID_O_NON_STOP, order->GetNonStopType().Test(OrderNonStopFlag::NonStop));
					}
					/* Disable refit button if the order is no 'always go' order.
					 * However, keep the service button enabled for refit-orders to allow clearing refits (without knowing about ctrl). */
					/* In a shed a train's refit is not greyed. The test that
					 * used to grey it asks whether anything in the chain can be
					 * refitted, and a collecting engine arrives with nothing
					 * behind it -- the wagons it is going to fetch are in the
					 * shed and are exactly what the refit is for. Asking the
					 * engine about them beforehand can only ever answer no. */
					this->SetWidgetDisabledState(WID_O_REFIT,
							order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service) || order->GetDepotActionType().Test(OrderDepotActionFlag::Halt) ||
							(this->vehicle->type != VehicleType::Train && !this->can_do_refit && !order->IsRefit()));
					break;

				case OT_CONDITIONAL: {
					if (row_sel != nullptr) {
						row_sel->SetDisplayedPlane(DP_ROW_CONDITIONAL);
					} else {
						train_row_sel->SetDisplayedPlane(DP_GROUNDVEHICLE_ROW_CONDITIONAL);
					}
					OrderConditionVariable ocv = order->GetConditionVariable();
					/* Set the strings for the dropdown boxes. */
					this->GetWidget<NWidgetCore>(WID_O_COND_VARIABLE)->SetString(STR_ORDER_CONDITIONAL_LOAD_PERCENTAGE + to_underlying(ocv));
					this->GetWidget<NWidgetCore>(WID_O_COND_COMPARATOR)->SetString(_order_conditional_condition[to_underlying(order->GetConditionComparator())]);
					this->SetWidgetDisabledState(WID_O_COND_COMPARATOR, ocv == OrderConditionVariable::Unconditionally);
					this->SetWidgetDisabledState(WID_O_COND_VALUE, ocv == OrderConditionVariable::DrivingBackwards || ocv == OrderConditionVariable::RequiresService || ocv == OrderConditionVariable::NothingToCouple || ocv == OrderConditionVariable::Unconditionally);
					break;
				}

				default: // every other order
					if (row_sel != nullptr) {
						row_sel->SetDisplayedPlane(DP_ROW_LOAD);
					} else {
						train_row_sel->SetDisplayedPlane(DP_GROUNDVEHICLE_ROW_NORMAL);
						left_sel->SetDisplayedPlane(DP_LEFT_LOAD);
						middle_sel->SetDisplayedPlane(DP_MIDDLE_UNLOAD);
						right_sel->SetDisplayedPlane(DP_RIGHT_EMPTY);
						this->DisableWidget(WID_O_NON_STOP);
					}
					this->DisableWidget(WID_O_FULL_LOAD);
					this->DisableWidget(WID_O_UNLOAD);
					this->DisableWidget(WID_O_REFIT_DROPDOWN);
					break;
			}
		}

		/* Disable list of vehicles with the same shared orders if there is no list */
		this->SetWidgetDisabledState(WID_O_SHARED_ORDER_LIST, !shared_orders);

		/* Couple row: trains only. Station orders get the decouple/couple
		 * buttons, depot orders the turn-around one, anything else nothing at
		 * all. See FEATURE_DESIGN_COUPLING_TOW.md. */
		NWidgetStacked *decouple_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_DECOUPLE);
		if (decouple_sel != nullptr) {
			/* Reversing out and leaving by itself are a station order's alone.
			 * Every other order of a train -- a depot's, a waypoint's, the end
			 * of the list -- has the two greyed and up. Left alone they kept
			 * whatever the order before them had, and sat lit and down on a
			 * "go to depot" order, whose train turns with the depot's own
			 * button. The station branch below sets them for real. */
			this->SetWidgetDisabledState(WID_O_REVERSE_OUT, true);
			this->SetWidgetLoweredState(WID_O_REVERSE_OUT, false);
			this->SetWidgetDisabledState(WID_O_AUTO_DEPARTURE, true);
			this->SetWidgetLoweredState(WID_O_AUTO_DEPARTURE, false);

			bool is_train = this->vehicle->type == VehicleType::Train && order != nullptr;
			if (is_train && order->IsType(OT_GOTO_STATION)) {
				decouple_sel->SetDisplayedPlane(DP_COUPLE_ROW_STATION);
				this->SetWidgetLoweredState(WID_O_WAIT_COUPLE, order->ShouldWaitForCouple());
				this->SetWidgetLoweredState(WID_O_GOTO_COUPLE, order->ShouldGoToCouple());

				/* Three things one order cannot do at once. Waiting to be
				 * collected is the opposite of going to collect, and both are
				 * the opposite of leaving part of the train behind -- an order
				 * cannot both hand vehicles over and take them on. Whichever
				 * of the three is set greys the other two out.
				 *
				 * Reversing out is a fourth, and combines fine with coupling:
				 * a train that has just picked wagons up very often wants to
				 * go back the way it came. It is only decoupling and waiting
				 * that rule it out -- a decoupling train already leaves facing
				 * the right way, and a train waiting to be collected does not
				 * decide how it leaves at all, since whoever couples to it
				 * brings the orders. */
				bool waiting = order->ShouldWaitForCouple();
				bool collecting = order->ShouldGoToCouple();
				bool decoupling = order->ShouldDecoupleOnDeparture();
				bool reversing_out = order->ShouldReverseOutOfStation();

				this->SetWidgetDisabledState(WID_O_GOTO_COUPLE, waiting || decoupling);
				this->SetWidgetDisabledState(WID_O_WAIT_COUPLE, collecting || decoupling || reversing_out);

				bool can_reverse_out = !decoupling && !waiting;
				this->SetWidgetDisabledState(WID_O_REVERSE_OUT, !can_reverse_out);
				this->SetWidgetLoweredState(WID_O_REVERSE_OUT, can_reverse_out && reversing_out);

				/* Automatic departure is the third answer to the same question
				 * and lives next to reversing out, on a couple order too. */
				bool can_auto = can_reverse_out;
				bool automatic = order->ShouldDepartAutomatically();
				this->SetWidgetDisabledState(WID_O_AUTO_DEPARTURE, !can_auto);
				/* Shown down even when greyed under a decouple: that is the
				 * setting a decoupling order carries (the decouple switch writes
				 * it), and the greyed-and-down look says "on, and not yours to
				 * change" -- the same thing the order line says with
				 * "(automatic)". */
				this->SetWidgetLoweredState(WID_O_AUTO_DEPARTURE, automatic && (can_auto || decoupling));
			} else if (is_train && order->IsType(OT_GOTO_DEPOT)) {
				decouple_sel->SetDisplayedPlane(DP_COUPLE_ROW_DEPOT);
				this->SetWidgetLoweredState(WID_O_TURN_AROUND_DEPOT, order->ShouldTurnAroundInDepot());

				/* Both at once is allowed here, unlike at a station: a shed is
				 * where a train goes to leave one rake and pick up another, and
				 * the two halves are the same piece of work at the same moment.
				 * They happen in the order the player would do them by hand --
				 * leave first, then collect -- and what was just left behind is
				 * not what gets picked up. Waiting has no button at a depot at
				 * all: trains couple at stations, in the open; the only thing to
				 * couple to in a shed is a stored rake. */
				this->SetWidgetLoweredState(WID_O_GOTO_COUPLE_DEPOT, order->ShouldGoToCouple());
				this->SetWidgetLoweredState(WID_O_DECOUPLE_DEPOT, order->ShouldDecoupleOnDeparture());
				this->SetWidgetDisabledState(WID_O_GOTO_COUPLE_DEPOT, false);
				this->SetWidgetDisabledState(WID_O_DECOUPLE_DEPOT, false);
			} else if (is_train && IsStationWaypointOrder(order)) {
				/* A station waypoint's row was empty; the player asked for the
				 * horn to live there. Pressed, the train honks as it passes. */
				decouple_sel->SetDisplayedPlane(DP_COUPLE_ROW_WAYPOINT);
				this->SetWidgetLoweredState(WID_O_HONK, order->ShouldHonk());
			} else if (this->vehicle->type == VehicleType::Road && order != nullptr && order->IsType(OT_GOTO_STATION)) {
				/* A road vehicle's station order: how it gets carried on from
				 * here. One big button to begin with, then two, now a dropdown
				 * of five -- the four ways and none of them -- because four
				 * buttons' worth of words does not fit across the window and
				 * the player wanted the whole sentence readable. See
				 * road_on_rail.h. */
				decouple_sel->SetDisplayedPlane(DP_COUPLE_ROW_ROAD);
			} else {
				/* Nothing to put in the row -- a waypoint order, the end of the
				 * list, a vehicle that is not a train -- but the row stays open
				 * all the same. Taking its height away instead makes the whole
				 * window jump to a different size as the player clicks from one
				 * order to the next, and everything below it walks up and down
				 * the screen. An empty strip is not pretty; a window that will
				 * not hold still is worse. */
				decouple_sel->SetDisplayedPlane(DP_COUPLE_ROW_EMPTY);
			}
		}

		/* The decoupling row carries the switch itself, so it is there for any
		 * station order; what changes is whether the rest of it can be used.
		 *
		 * Only in the window that has the row at all. Ships and aircraft are
		 * built from the other widget tree, which has no coupling row in it and
		 * therefore no button to reach for: asking for one there hands back
		 * nothing, and the game reads through that nothing. Same guard as the
		 * row above -- both are the same row. */
		if (decouple_sel != nullptr) {
			bool can_decouple = this->vehicle->type == VehicleType::Train && order != nullptr && order->IsType(OT_GOTO_STATION);
			bool decoupling = can_decouple && order->ShouldDecoupleOnDeparture();

			/* Reversing out does not grey this one: the player's way of putting
			 * it is that a decoupling delivery is always a reversing one -- the
			 * train backs in, leaves the wagons at the far end, and goes out the
			 * way it came -- so the switch has to stay within reach with the
			 * reversing button down. The reversing button itself stays greyed
			 * under a decouple (see above); what the train does on departure is
			 * unchanged, decoupling leaves the way it is pointing. */
			this->SetWidgetDisabledState(WID_O_DECOUPLE, !can_decouple ||
					order->ShouldWaitForCouple() || order->ShouldGoToCouple());
			this->SetWidgetLoweredState(WID_O_DECOUPLE, decoupling);

			/* Backing out of a station and leaving by itself are a train's
			 * business, and their two buttons were only ever decided on a
			 * train's order -- so in a road vehicle's window they sat there
			 * lit, offering what a lorry cannot do. The row itself has to
			 * stay: it also carries skipping and deleting, which every
			 * vehicle needs. So the two are greyed out rather than taken
			 * away, which is what the player asked for. */
			if (this->vehicle->type != VehicleType::Train) {
				this->SetWidgetDisabledState(WID_O_REVERSE_OUT, true);
				this->SetWidgetLoweredState(WID_O_REVERSE_OUT, false);
				this->SetWidgetDisabledState(WID_O_AUTO_DEPARTURE, true);
				this->SetWidgetLoweredState(WID_O_AUTO_DEPARTURE, false);
			}
		}

		/* What a coupling order will accept is only worth showing on an order
		 * that is going to collect something. An order that is not carries no
		 * settings for what it is not going to do. */
		NWidgetStacked *filter_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_COUPLE_FILTER);
		if (filter_sel != nullptr) {
			bool collecting = this->vehicle->type == VehicleType::Train && order != nullptr &&
					(order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_DEPOT)) && order->ShouldGoToCouple();
			/* A row coming and going changes how tall the window is, and that is
			 * a re-layout. It cannot be done here: this runs from inside a click
			 * being handed to this very window, and moving every widget out from
			 * under a click that is halfway through being delivered leaves the
			 * press landing on whatever has since slid into that spot -- or on
			 * nothing. Which is why not one button in the window could be
			 * pressed. Note it and do it in OnMouseLoop, once the click is
			 * finished with. */
			if (filter_sel->SetDisplayedPlane(collecting ? 0 : SZSP_NONE)) this->couple_filter_resized = true;
		}

		/* Where the wagons an order puts down are to load for. Same reasoning as
		 * the filter row: only on an order that is going to put wagons down.
		 * Only the button changes, not the row it stands in, so the window keeps
		 * its height and nothing has to be laid out again. */
		NWidgetStacked *dest_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_DECOUPLE_DEST_BTN);
		if (dest_sel != nullptr) {
			bool dropping = this->vehicle->type == VehicleType::Train && order != nullptr &&
					order->IsType(OT_GOTO_STATION) && order->ShouldDecoupleOnDeparture();
			/* A station order collects or drops, never both, so the two
			 * places are free for the collecting buttons exactly when the
			 * dropping ones are not wanted. A shed's order can do both and
			 * gets neither of the collecting buttons: there the two readings
			 * come to the same thing (Order::couple_search). */
			bool collecting = this->vehicle->type == VehicleType::Train && order != nullptr &&
					order->IsType(OT_GOTO_STATION) && order->ShouldGoToCouple();
			dest_sel->SetDisplayedPlane(dropping ? 0 : (collecting ? 2 : 1));
			if (collecting) this->SetWidgetLoweredState(WID_O_COUPLE_FIND, !order->ShouldSearchInRake());
			/* It used to be greyed out with cargo distribution off, because
			 * without it a station hands every load to everybody and saying
			 * where the wagons are bound bought nothing. It buys something
			 * now: a road vehicle told to ride "on wagons meant for my next
			 * stop" reads exactly this, and distribution has nothing to do
			 * with cars. So the button has one meaning in every game -- this
			 * rake is for that station -- and is never greyed. */
		}

		/* And the right-hand place: selling what the order puts down. That one
		 * belongs to a depot order as much as to a station order -- a shed is
		 * where a vehicle is sold -- so it is offered on both, and the middle
		 * one is not, because where a rake's cargo is bound is a platform's
		 * question. */
		NWidgetStacked *sell_sel = this->GetWidget<NWidgetStacked>(WID_O_SEL_SELL_WAGONS);
		if (sell_sel != nullptr) {
			bool dropping_any = this->vehicle->type == VehicleType::Train && order != nullptr &&
					(order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_DEPOT)) && order->ShouldDecoupleOnDeparture();
			bool collecting_here = this->vehicle->type == VehicleType::Train && order != nullptr &&
					order->IsType(OT_GOTO_STATION) && order->ShouldGoToCouple();
			sell_sel->SetDisplayedPlane(dropping_any ? 0 : (collecting_here ? 2 : 1));
			if (dropping_any) this->SetWidgetLoweredState(WID_O_SELL_WAGONS, order->ShouldSellDecoupled());
			if (collecting_here) this->SetWidgetLoweredState(WID_O_COUPLE_SEARCH, order->ShouldSearchInRake());
		}

		this->SetDirty();
	}

	/**
	 * Put the selection on one order, for the rig: the probes used to click
	 * into the list at a guessed height and the guess was a row short.
	 * @param index the order to select
	 * @return whether the order exists
	 */
	bool SelectOrderForTest(int index)
	{
		if (index < 0 || index >= this->vehicle->GetNumOrders()) return false;
		this->selected_order = index;
		this->OnInvalidateData();
		return true;
	}

	void OnPaint() override
	{
		if (this->vehicle->owner != _local_company) {
			this->selected_order = -1; // Disable selection any selected row at a competitor order window.
		} else {
			this->SetWidgetLoweredState(WID_O_GOTO, this->goto_type != OPOS_NONE);
			/* Held down while it is the one waiting for a station to be
			 * clicked, so the player can see which button loaded the pointer
			 * and which one will put it down again.
			 *
			 * Asked whether the button is there at all, because a ship's and an
			 * aircraft's order window is built from a different set of widgets
			 * and has none of ours -- and a widget that is not there answers
			 * with nothing, which this used to walk straight into. Opening an
			 * aircraft's orders brought the game down for the player the first
			 * day he had an aircraft; the same line had been waiting for a ship
			 * just as long. The rest of our row does the same test through
			 * decouple_sel in UpdateButtonState(). */
			if (this->GetWidget<NWidgetCore>(WID_O_DECOUPLE_CARGO_DEST) != nullptr) {
				this->SetWidgetLoweredState(WID_O_DECOUPLE_CARGO_DEST, this->goto_type == OPOS_DECOUPLE_DEST);
			}
			/* Nobody buys a breakdown and nobody buys a wreck, and a train that
			 * has been sold is sold. The button is greyed in all three cases,
			 * rather than refusing the sale afterwards: a press that opens a
			 * window asking "sell the train?" and then says no is a worse way of
			 * saying the same thing. Done here, on every repaint, because
			 * breaking down and crashing happen to the train out on the line and
			 * do not come past this window at all. */
			/* Wagons are bought in a shed, so the button belongs to a depot
			 * order and to no other. It sits in the filter row, which a
			 * station order has too, so on one of those it is greyed rather
			 * than left to open a list that could answer nothing: pressed
			 * there, it opened the wagon list with no depot behind it, the
			 * window built itself as a plain list with no button, and the
			 * player clicked a wagon and watched nothing happen. */
			if (this->GetWidget<NWidgetCore>(WID_O_COUPLE_BUY) != nullptr) {
					const Order *sel = this->vehicle->GetOrder(this->OrderGetSel());
					/* A platform has it too now, with the buying left out:
					 * which type to couple is worth asking wherever the order
					 * collects, and only the buying needs a shed. */
					this->SetWidgetDisabledState(WID_O_COUPLE_BUY, sel == nullptr || !sel->ShouldGoToCouple() ||
							(!sel->IsType(OT_GOTO_DEPOT) && !sel->IsType(OT_GOTO_STATION)));
			}
			/* Selling moved to the vehicle's own window, where there is an
			 * icon for it in the row that stood dark out on the line. This
			 * button is kept, dark and doing nothing, because the player asked
			 * for the place to be held in case something wants it later. */
			if (this->GetWidget<NWidgetCore>(WID_O_SELL_TRAIN) != nullptr) {
				this->SetWidgetDisabledState(WID_O_SELL_TRAIN, true);
			}
		}
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_O_ORDER_LIST) return;

		Rect ir = r.Shrink(WidgetDimensions::scaled.frametext, WidgetDimensions::scaled.framerect);
		bool rtl = _current_text_dir == TD_RTL;
		uint64_t max_value = GetParamMaxValue(this->vehicle->GetNumOrders(), 2);
		int index_column_width = GetStringBoundingBox(GetString(STR_ORDER_INDEX, max_value)).width + 2 * GetSpriteSize(rtl ? SPR_ARROW_RIGHT : SPR_ARROW_LEFT).width + WidgetDimensions::scaled.hsep_normal;
		int middle = rtl ? ir.right - index_column_width : ir.left + index_column_width;

		int y = ir.top;
		int line_height = this->GetWidget<NWidgetBase>(WID_O_ORDER_LIST)->resize_y;

		VehicleOrderID i = this->vscroll->GetPosition();
		VehicleOrderID num_orders = this->vehicle->GetNumOrders();

		/* First draw the highlighting underground if it exists. */
		if (this->order_over != INVALID_VEH_ORDER_ID) {
			while (i < num_orders) {
				/* Don't draw anything if it extends past the end of the window. */
				if (!this->vscroll->IsVisible(i)) break;

				if (i != this->selected_order && i == this->order_over) {
					/* Highlight dragged order destination. */
					int top = (this->order_over < this->selected_order ? y : y + line_height) - WidgetDimensions::scaled.framerect.top;
					int bottom = std::min(top + 2, ir.bottom);
					top = std::max(top - 3, ir.top);
					GfxFillRect(ir.left, top, ir.right, bottom, GetColourGradient(Colours::Grey, Shade::Lightest));
					break;
				}
				y += line_height;

				i++;
			}

			/* Reset counters for drawing the orders. */
			y = ir.top;
			i = this->vscroll->GetPosition();
		}

		/* Draw the orders. */
		while (i < num_orders) {
			/* Don't draw anything if it extends past the end of the window. */
			if (!this->vscroll->IsVisible(i)) break;

			DrawOrderString(this->vehicle, this->vehicle->GetOrder(i), i, y, i == this->selected_order, false, ir.left, middle, ir.right);
			y += line_height;

			i++;
		}

		if (this->vscroll->IsVisible(i)) {
			StringID str = this->vehicle->IsOrderListShared() ? STR_ORDERS_END_OF_SHARED_ORDERS : STR_ORDERS_END_OF_ORDERS;
			DrawString(rtl ? ir.left : middle, rtl ? middle : ir.right, y, str, (i == this->selected_order) ? TextColour::White : TextColour::Black);
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_O_COND_VALUE: {
				VehicleOrderID sel = this->OrderGetSel();
				const Order *order = this->vehicle->GetOrder(sel);

				if (order != nullptr && order->IsType(OT_CONDITIONAL)) {
					uint value = order->GetConditionValue();
					if (order->GetConditionVariable() == OrderConditionVariable::MaxSpeed) value = ConvertSpeedToDisplaySpeed(value, this->vehicle->type);
					return GetString(STR_JUST_COMMA, value);
				}
				return {};
			}

			case WID_O_CAPTION:
				return GetString(STR_ORDERS_CAPTION, this->vehicle->index);

			case WID_O_DEPOT_ACTION: {
				VehicleOrderID sel = this->OrderGetSel();
				const Order *order = this->vehicle->GetOrder(sel);
				if (order == nullptr || !order->IsType(OT_GOTO_DEPOT)) return {};

				/* Select the current action selected in the dropdown. The flags don't match the dropdown so we can't just use an index. */
				if (order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service)) return GetString(STR_ORDER_DROP_SERVICE_DEPOT);
				if (order->GetDepotActionType().Test(OrderDepotActionFlag::Halt)) return GetString(STR_ORDER_DROP_HALT_DEPOT);
				if (order->GetDepotActionType().Test(OrderDepotActionFlag::Unbunch)) return GetString(STR_ORDER_DROP_UNBUNCH);

				return GetString(STR_ORDER_DROP_GO_ALWAYS_DEPOT);
			}

			case WID_O_COUPLE_LOAD: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				return GetString(STR_ORDER_COUPLE_LOAD_ANY + to_underlying(order->GetCoupleLoad()));
			}

			case WID_O_BOARD_MODE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				return GetString(_order_board_mode_dropdown[BoardModeToIndex(order->GetBoardMode())]);
			}

			case WID_O_COUPLE_CARGO: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				if (!IsValidCargoType(order->GetCoupleCargo())) return GetString(STR_ORDER_COUPLE_CARGO_ANY);
				return GetString(STR_ORDER_COUPLE_CARGO_TYPE, CargoSpec::Get(order->GetCoupleCargo())->name);
			}

			case WID_O_COUPLE_BUY: {
				/* What the order holds: every type, a type it only couples,
				 * or a type it buys (in a shed). The list under it is where
				 * each of them is picked (OnClick). */
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return GetString(STR_ORDER_COUPLE_BUY_OFF);
				EngineID eid = order->GetCoupleBuyEngine();
				if (Engine::GetIfValid(eid) == nullptr) return GetString(STR_ORDER_COUPLE_BUY_OFF);
				return GetString(order->ShouldBuyWagons() ? STR_ORDER_COUPLE_BUY_ON : STR_ORDER_COUPLE_BUY_ONLY,
						PackEngineNameDParam(eid, EngineNameContext::PurchaseList));
			}

			case WID_O_COUPLE_COUNT: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				if (order->ShouldFoundRake()) {
					if (order->GetCoupleCount() == 0) return GetString(STR_ORDER_COUPLE_COUNT_FOUND_ANY);
					return GetString(STR_ORDER_COUPLE_COUNT_FOUND, order->GetCoupleCount());
				}
				if (order->GetCoupleCount() == 0) return GetString(STR_ORDER_COUPLE_COUNT_ANY);
				if (order->IsCoupleCountMinimum()) return GetString(STR_ORDER_COUPLE_COUNT_MIN, order->GetCoupleCount());
				if (order->IsCoupleCountMaximum()) return GetString(STR_ORDER_COUPLE_COUNT_MAX, order->GetCoupleCount());
				return GetString(STR_ORDER_COUPLE_COUNT_BUTTON, order->GetCoupleCount());
			}

			case WID_O_DECOUPLE_CARGO_DEST: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				StationID dest = order->GetDecoupleCargoDest();
				if (dest == StationID::Invalid() || !Station::IsValidID(dest)) return GetString(STR_ORDER_DECOUPLE_CARGO_DEST_NONE);
				return GetString(STR_ORDER_DECOUPLE_CARGO_DEST, dest);
			}

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_O_ORDER_LIST: {
				if (this->goto_type == OPOS_CONDITIONAL) {
					VehicleOrderID order_id = this->GetOrderFromPt(_cursor.pos.y - this->top);
					if (order_id != INVALID_VEH_ORDER_ID) {
						Order order{};
						order.MakeConditional(order_id);

						Command<Commands::InsertOrder>::Post(STR_ERROR_CAN_T_INSERT_NEW_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), order);
					}
					ResetObjectToPlace();
					break;
				}

				VehicleOrderID sel = this->GetOrderFromPt(pt.y);

				if (_ctrl_pressed && sel < this->vehicle->GetNumOrders()) {
					TileIndex xy = this->vehicle->GetOrder(sel)->GetLocation(this->vehicle);
					if (xy != INVALID_TILE) ScrollMainWindowToTile(xy);
					return;
				}

				/* This order won't be selected any more, close all child windows and dropdowns */
				this->CloseChildWindows();

				if (sel == INVALID_VEH_ORDER_ID || this->vehicle->owner != _local_company) {
					/* Deselect clicked order */
					this->selected_order = -1;
				} else if (sel == this->selected_order && click_count > 1) {
					if (this->vehicle->type == VehicleType::Train && sel < this->vehicle->GetNumOrders()) {
						Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER,
								this->vehicle->tile, this->vehicle->index, sel,
								MOF_STOP_LOCATION, (to_underlying(this->vehicle->GetOrder(sel)->GetStopLocation()) + 1) % to_underlying(OrderStopLocation::End));
					}
				} else {
					/* Select clicked order */
					this->selected_order = sel;

					if (this->vehicle->owner == _local_company) {
						/* Activate drag and drop */
						this->order_dragged = false;
						SetObjectToPlaceWnd(SPR_CURSOR_MOUSE, PAL_NONE, HT_DRAG, this);
					}
				}

				this->UpdateButtonState();
				break;
			}

			case WID_O_SKIP:
				this->OrderClick_Skip();
				break;

			case WID_O_BACK:
				this->OrderClick_Back();
				break;

			case WID_O_DELETE:
				this->OrderClick_Delete();
				break;

			case WID_O_STOP_SHARING:
				this->OrderClick_StopSharing();
				break;

			case WID_O_NON_STOP:
				if (this->GetWidget<NWidgetLeaf>(widget)->ButtonHit(pt)) {
					this->OrderClick_Nonstop(std::nullopt);
				} else {
					const Order *o = this->vehicle->GetOrder(this->OrderGetSel());
					assert(o != nullptr);
					ShowDropDownMenu(this, _order_non_stop_dropdown, o->GetNonStopType().base(), WID_O_NON_STOP, 0,
													o->IsType(OT_GOTO_STATION) ? 0 : (o->IsType(OT_GOTO_WAYPOINT) ? 3 : 12));
				}
				break;

			case WID_O_GOTO:
				if (this->GetWidget<NWidgetLeaf>(widget)->ButtonHit(pt)) {
					if (this->goto_type != OPOS_NONE) {
						ResetObjectToPlace();
					} else {
						this->OrderClick_Goto(OPOS_GOTO);
					}
				} else {
					int sel;
					switch (this->goto_type) {
						case OPOS_NONE:        sel = -1; break;
						case OPOS_GOTO:        sel =  0; break;
						case OPOS_CONDITIONAL: sel =  2; break;
						case OPOS_SHARE:       sel =  3; break;
						/* Picking a station for a decoupling order is not one of
						 * this dropdown's entries; nothing in it is selected. */
						case OPOS_DECOUPLE_DEST: sel = -1; break;
						default: NOT_REACHED();
					}
					ShowDropDownMenu(this, this->vehicle->type == VehicleType::Aircraft ? _order_goto_dropdown_aircraft : _order_goto_dropdown, sel, WID_O_GOTO, 0, 0);
				}
				break;

			case WID_O_FULL_LOAD:
				if (this->GetWidget<NWidgetLeaf>(widget)->ButtonHit(pt)) {
					this->OrderClick_FullLoad(OrderLoadType::FullLoadAny, true);
				} else {
					ShowDropDownMenu(this, _order_full_load_dropdown, to_underlying(this->vehicle->GetOrder(this->OrderGetSel())->GetLoadType()), WID_O_FULL_LOAD, 0, 2);
				}
				break;

			case WID_O_UNLOAD:
				if (this->GetWidget<NWidgetLeaf>(widget)->ButtonHit(pt)) {
					this->OrderClick_Unload(OrderUnloadType::Unload, true);
				} else {
					ShowDropDownMenu(this, _order_unload_dropdown, to_underlying(this->vehicle->GetOrder(this->OrderGetSel())->GetUnloadType()), WID_O_UNLOAD, 0, 8);
				}
				break;

			case WID_O_REFIT:
				this->OrderClick_Refit(0, false);
				break;

			case WID_O_DEPOT_ACTION:
				ShowDropDownMenu(this, _order_depot_action_dropdown, to_underlying(DepotActionStringIndex(this->vehicle->GetOrder(this->OrderGetSel()))), WID_O_DEPOT_ACTION, 0, 0);
				break;

			case WID_O_REFIT_DROPDOWN:
				if (this->GetWidget<NWidgetLeaf>(widget)->ButtonHit(pt)) {
					this->OrderClick_Refit(0, true);
				} else {
					ShowDropDownMenu(this, _order_refit_action_dropdown, 0, WID_O_REFIT_DROPDOWN, 0, 0);
				}
				break;

			case WID_O_TIMETABLE_VIEW:
				ShowTimetableWindow(this->vehicle);
				break;

			case WID_O_COND_VARIABLE: {
				DropDownList list;
				for (const auto &ocv : _order_conditional_variable) {
					if ((ocv == OrderConditionVariable::DrivingBackwards || ocv == OrderConditionVariable::WagonCount ||
							ocv == OrderConditionVariable::TrainLength || ocv == OrderConditionVariable::NothingToCouple) &&
							this->vehicle->type != VehicleType::Train) {
						continue;
					}
					if (ocv == OrderConditionVariable::RoadVehiclesWaitingToBoard && this->vehicle->type != VehicleType::Road) continue;
					list.push_back(MakeDropDownListStringItem(STR_ORDER_CONDITIONAL_LOAD_PERCENTAGE + to_underlying(ocv), to_underlying(ocv)));
				}
				ShowDropDownList(this, std::move(list), to_underlying(this->vehicle->GetOrder(this->OrderGetSel())->GetConditionVariable()), WID_O_COND_VARIABLE);
				break;
			}

			case WID_O_COND_COMPARATOR: {
				const Order *o = this->vehicle->GetOrder(this->OrderGetSel());
				assert(o != nullptr);
				ShowDropDownMenu(this, _order_conditional_condition, to_underlying(o->GetConditionComparator()), WID_O_COND_COMPARATOR, 0, (o->GetConditionVariable() == OrderConditionVariable::RequiresService || o->GetConditionVariable() == OrderConditionVariable::DrivingBackwards ||
						o->GetConditionVariable() == OrderConditionVariable::NothingToCouple) ? 0x3F : 0xC0);
				break;
			}

			case WID_O_COND_VALUE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				uint value = order->GetConditionValue();
				if (order->GetConditionVariable() == OrderConditionVariable::MaxSpeed) value = ConvertSpeedToDisplaySpeed(value, this->vehicle->type);
				this->querying_decouple_count = false;
				this->querying_couple_count = false;
				ShowQueryString(GetString(STR_JUST_INT, value), STR_ORDER_CONDITIONAL_VALUE_CAPT, 5, this, CS_NUMERAL, {});
				break;
			}

			case WID_O_DECOUPLE_DEPOT:
			case WID_O_DECOUPLE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				/* The button is the switch, and nothing else. It used to be the
				 * switch and the number both, with nought standing for "off" --
				 * which cost the one answer a player most often wants, "leave
				 * the whole lot here", because that is nought wagons and nought
				 * already meant something else.
				 *
				 * Pressed while it is on, it simply switches off. */
				if (order->ShouldDecoupleOnDeparture()) {
					Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_DECOUPLE, 0);
					break;
				}
				/* Reversing out becomes "automatic" the moment decoupling goes
				 * on: a decoupling train never turns round, it drops its wagons
				 * and goes away from them, backing off or pulling on as it came
				 * in -- which is what automatic departure is. Rewriting the
				 * setting, rather than leaving a reversing flag lying there
				 * unused, keeps the buttons telling the truth. Reversing is
				 * cleared first, since automatic will not go on beside it. */
				if (order->ShouldReverseOutOfStation()) {
					Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_REVERSE_OUT, 0);
					Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_AUTO_DEPARTURE, 1);
				}
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_DECOUPLE, 1);
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_DECOUPLE_COUNT, 0);
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_DECOUPLE_WHOLE, 0);

				/* And then ask how many wagons stay on, with nought already
				 * filled in, because nought is both the commonest answer and
				 * the default. Accepting the box unchanged leaves it there --
				 * a query box calls back only when the text is edited -- which
				 * is why the count is set before the box opens rather than by
				 * it. */
				/* The other answer to the same question sits in the same box, a
				 * button across its whole width: not a number of wagons but
				 * "what I came with" -- drop exactly what this train coupled.
				 * The player's design; see FindCoupledBoundary(). */
				this->querying_decouple_count = true;
				this->querying_couple_count = false;
				ShowQueryString(GetString(STR_JUST_INT, 0), STR_ORDER_DECOUPLE_COUNT_CAPT, 4, this, CS_NUMERAL, {}, STR_ORDER_DECOUPLE_WHOLE_BUTTON);
				break;
			}

			case WID_O_WAIT_COUPLE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_WAIT_COUPLE, order->ShouldWaitForCouple() ? 0 : 1);
				break;
			}

			case WID_O_GOTO_COUPLE_DEPOT:
			case WID_O_GOTO_COUPLE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				bool turning_on = !order->ShouldGoToCouple();
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_GOTO_COUPLE, turning_on ? 1 : 0);

				/* An engine sent to a station to collect something almost always
				 * wants to leave again by the side it came in: what it came for
				 * is in front of it, and the way on is behind it. So the switch
				 * is filled in with the collect order rather than left for the
				 * player to remember -- filled in, not forced: the button is
				 * there and can be pressed straight back out. Whoever would
				 * rather have it the old way turns the setting off and nothing
				 * is added at all. Not at a depot: what leads out of a shed is
				 * settled by which way the train drove in (see 2.13). */
				if (widget == WID_O_GOTO_COUPLE && _settings_client.gui.couple_auto_reverse_out &&
						order->ShouldReverseOutOfStation() != turning_on) {
					/* And taken back off again with it. What was filled in
					 * because the order collects has no business outliving the
					 * collecting: the player takes the couple order off, and is
					 * left with an order that quietly turns the train round at
					 * a station for no reason he ever asked for. Whatever is
					 * put in by itself has to come out by itself. Still only
					 * filling in, not forcing -- the button stays there and can
					 * be pressed either way afterwards. */
					Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_REVERSE_OUT, turning_on ? 1 : 0);
				}
				break;
			}

			case WID_O_BOARD_MODE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				ShowDropDownMenu(this, _order_board_mode_dropdown, BoardModeToIndex(order->GetBoardMode()), WID_O_BOARD_MODE, 0, 0);
				break;
			}

			case WID_O_REVERSE_OUT: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_REVERSE_OUT, order->ShouldReverseOutOfStation() ? 0 : 1);
				break;
			}

			case WID_O_AUTO_DEPARTURE: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_AUTO_DEPARTURE, order->ShouldDepartAutomatically() ? 0 : 1);
				break;
			}

			case WID_O_HONK: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_HONK, order->ShouldHonk() ? 0 : 1);
				break;
			}

			case WID_O_TURN_AROUND_DEPOT: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_TURN_AROUND_DEPOT, order->ShouldTurnAroundInDepot() ? 0 : 1);
				break;
			}

			case WID_O_COUPLE_LOAD: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				ShowDropDownMenu(this, _order_couple_load_dropdown, to_underlying(order->GetCoupleLoad()), WID_O_COUPLE_LOAD, 0, 0);
				break;
			}

			case WID_O_COUPLE_CARGO: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				ShowDropDownList(this, BuildCoupleCargoDropDown(order), order->GetCoupleCargo(), WID_O_COUPLE_CARGO);
				break;
			}

			case WID_O_COUPLE_BUY: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				/* A list, the player's layout: every type at the top, the
				 * default; the named type as a filter only; the same type
				 * bought, in a shed -- the one press that switches buying back
				 * on, which the old walk-round button could only do by going
				 * all the way round and picking the wagon again; and a way to
				 * pick another type. At a platform nothing can be bought, so
				 * the buying line is not there. */
				if (!order->ShouldGoToCouple()) break;
				if (!order->IsType(OT_GOTO_DEPOT) && !order->IsType(OT_GOTO_STATION)) break;
				EngineID eid = order->GetCoupleBuyEngine();
				bool named = Engine::GetIfValid(eid) != nullptr;
				DropDownList list;
				list.push_back(MakeDropDownListStringItem(STR_ORDER_COUPLE_TYPE_ALL, 0));
				if (named) {
					list.push_back(MakeDropDownListStringItem(GetString(STR_ORDER_COUPLE_TYPE_ONLY_ITEM, PackEngineNameDParam(eid, EngineNameContext::PurchaseList)), 1));
					if (order->IsType(OT_GOTO_DEPOT)) {
						list.push_back(MakeDropDownListStringItem(GetString(STR_ORDER_COUPLE_TYPE_BUY_ITEM, PackEngineNameDParam(eid, EngineNameContext::PurchaseList)), 2));
					}
				}
				list.push_back(MakeDropDownListStringItem(STR_ORDER_COUPLE_TYPE_PICK, 3));
				ShowDropDownList(this, std::move(list), !named ? 0 : (order->ShouldBuyWagons() ? 2 : 1), WID_O_COUPLE_BUY);
				break;
			}

			case WID_O_COUPLE_COUNT: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				/* The selection can be the end-of-list row, or go stale when the
				 * list shrinks under an open window; a click then has no order to
				 * act on and does nothing. Asserting here brought the game down
				 * for exactly that (crash 2026-08-28). */
				if (order == nullptr) break;
				this->querying_decouple_count = false;
				this->querying_couple_count = true;
				/* With the other answer across the box: found a rake here, of
				 * at most the number entered -- or stop founding one. Depot
				 * orders collect from a store and found nothing. */
				if (order->IsType(OT_GOTO_STATION)) {
					/* Which end of the number is meant is one question with one
					 * answer -- at most above the box, at least below it,
					 * neither down the exact count -- and whether to found a
					 * rake is another, which is why it stands between them on
					 * a button of its own. OK confirms all of it together.
					 *
					 * One help for all of it, the same one the box in the order
					 * list carries: the number and the buttons are one setting
					 * said three ways, and the player reading any part of it
					 * wants the whole answer. Two texts also meant two texts to
					 * keep true, and one of them was out of date twice. */
					ShowQueryStringWithChoice(GetString(STR_JUST_INT, order->GetCoupleCount()), STR_ORDER_COUPLE_COUNT_CAPT, 4, this, CS_NUMERAL, {},
							STR_ORDER_COUPLE_MAX_BUTTON, STR_ORDER_COUPLE_MIN_BUTTON,
							order->IsCoupleCountMaximum() ? 1 : (order->IsCoupleCountMinimum() ? 2 : 0), STR_ORDER_COUPLE_COUNT_TOOLTIP,
							STR_ORDER_COUPLE_FOUND_BUTTON, order->ShouldFoundRake(), 2);
				} else {
					ShowQueryString(GetString(STR_JUST_INT, order->GetCoupleCount()), STR_ORDER_COUPLE_COUNT_CAPT, 4, this, CS_NUMERAL, {});
				}
				break;
			}

			case WID_O_DECOUPLE_CARGO_DEST: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				/* Pressed while the pointer is already loaded, it puts it down
				 * again -- the same gesture the Go To button answers to, and
				 * the player's word for it: there was no way out of the
				 * picking but to name a station or close the window. */
				if (this->goto_type == OPOS_DECOUPLE_DEST) {
					ResetObjectToPlace();
					break;
				}
				/* Set, the button takes it back off; the same gesture as the
				 * decouple switch beside it. Unset, it puts the pointer into
				 * station-picking, because a station is picked on the map here
				 * exactly as it is for a "go to" order -- there is nothing new
				 * to learn. */
				if (order->GetDecoupleCargoDest() != StationID::Invalid()) {
					Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
							this->OrderGetSel(), MOF_DECOUPLE_CARGO_DEST, StationID::Invalid().base());
					break;
				}
				this->OrderClick_Goto(OPOS_DECOUPLE_DEST);
				break;
			}

			case WID_O_SELL_WAGONS: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
						this->OrderGetSel(), MOF_SELL_DECOUPLED, order->ShouldSellDecoupled() ? 0 : 1);
				break;
			}

			case WID_O_COUPLE_FIND:
			case WID_O_COUPLE_SEARCH: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr || !order->ShouldGoToCouple()) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
						this->OrderGetSel(), MOF_COUPLE_SEARCH, widget == WID_O_COUPLE_SEARCH ? 1 : 0);
				break;
			}

			case WID_O_SELL_TRAIN:
				/* Held open and doing nothing; the selling is done from the
				 * vehicle's own window now. Dark, so that it is plain the
				 * button is put by rather than broken. */
				break;

			case WID_O_SHARED_ORDER_LIST:
				ShowVehicleListWindow(this->vehicle);
				break;
		}
	}

	void OnQueryTextExtra(std::string_view text) override
	{
		VehicleOrderID sel = this->OrderGetSel();
		/* The decouple count box's button: drop the whole coupled train. */
		if (this->querying_decouple_count) {
			this->querying_decouple_count = false;
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_DECOUPLE_WHOLE, 1);
			return;
		}
	}

	void OnQueryTextChoice(std::string_view text, uint8_t choice, bool found) override
	{
		/* The couple count box with its toggles: the number, which end of it
		 * is meant -- exact (neither), at most (top), at least (bottom) -- and
		 * whether to found a rake, which is a question of its own.
		 *
		 * Everything switched off goes first, so the exclusions the command
		 * makes for itself never lift what was just chosen. */
		if (!this->querying_couple_count) return;
		this->querying_couple_count = false;
		VehicleOrderID sel = this->OrderGetSel();
		if (this->vehicle->GetOrder(sel) == nullptr) return;
		auto value = ParseInteger(text, 10, true);
		if (value.has_value()) {
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_COUPLE_COUNT, Clamp(*value, 0, UINT8_MAX));
		}
		auto set = [&](ModifyOrderFlags mof, bool on) {
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, mof, on ? 1 : 0);
		};
		if (choice != 1) set(MOF_COUPLE_MAX, false);
		if (choice != 2) set(MOF_COUPLE_MIN, false);
		if (!found) set(MOF_COUPLE_FOUND, false);
		if (choice == 1) set(MOF_COUPLE_MAX, true);
		if (choice == 2) set(MOF_COUPLE_MIN, true);
		if (found) set(MOF_COUPLE_FOUND, true);
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value() || str->empty()) return;

		VehicleOrderID sel = this->OrderGetSel();
		auto value = ParseInteger(*str, 10, true);
		if (!value.has_value()) return;

		if (this->querying_decouple_count) {
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_DECOUPLE_COUNT, Clamp(*value, 0, UINT8_MAX));
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_DECOUPLE_WHOLE, 0);
			return;
		}

		if (this->querying_couple_count) {
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_COUPLE_COUNT, Clamp(*value, 0, UINT8_MAX));
			return;
		}

		switch (this->vehicle->GetOrder(sel)->GetConditionVariable()) {
			case OrderConditionVariable::MaxSpeed:
				value = ConvertDisplaySpeedToSpeed(*value, this->vehicle->type);
				break;

			case OrderConditionVariable::Reliability:
			case OrderConditionVariable::MaxReliability:
			case OrderConditionVariable::LoadPercentage:
				value = Clamp(*value, 0, 100);
				break;

			default:
				break;
		}
		Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_COND_VALUE, Clamp(*value, 0, 2047));
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		switch (widget) {
			case WID_O_NON_STOP:
				this->OrderClick_Nonstop(static_cast<OrderNonStopFlags>(index));
				break;

			case WID_O_FULL_LOAD:
				this->OrderClick_FullLoad(static_cast<OrderLoadType>(index));
				break;

			case WID_O_UNLOAD:
				this->OrderClick_Unload(static_cast<OrderUnloadType>(index));
				break;

			case WID_O_GOTO:
				switch (index) {
					case 0: this->OrderClick_Goto(OPOS_GOTO); break;
					case 1: this->OrderClick_NearestDepot(); break;
					case 2: this->OrderClick_Goto(OPOS_CONDITIONAL); break;
					case 3: this->OrderClick_Goto(OPOS_SHARE); break;
					default: NOT_REACHED();
				}
				break;

			case WID_O_DEPOT_ACTION:
				this->OrderClick_Service(static_cast<OrderDepotAction>(index));
				break;

			case WID_O_COUPLE_LOAD:
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_COUPLE_LOAD, index);
				break;

			case WID_O_BOARD_MODE:
				if (index < 0 || (size_t)index >= std::size(_order_board_mode_values)) break;
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_BOARD_MODE, to_underlying(_order_board_mode_values[index]));
				break;

			case WID_O_COUPLE_CARGO:
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_COUPLE_CARGO, index);
				break;

			case WID_O_COUPLE_BUY: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) break;
				switch (index) {
					case 0: // every type
						Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
								this->OrderGetSel(), MOF_COUPLE_BUY, EngineID::Invalid().base());
						break;
					case 1: // the type, couple only
						if (order->ShouldBuyWagons()) {
							Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
									this->OrderGetSel(), MOF_COUPLE_BUY_ON, 0);
						}
						break;
					case 2: // the type, bought
						if (!order->ShouldBuyWagons()) {
							Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
									this->OrderGetSel(), MOF_COUPLE_BUY_ON, 1);
						}
						break;
					case 3: { // pick another
						const Depot *depot = Depot::GetIfValid(order->GetDestination().ToDepotID());
						ShowPickCoupleWagonWindow(this->vehicle, this->OrderGetSel(),
								order->IsType(OT_GOTO_DEPOT) && depot != nullptr ? depot->xy : INVALID_TILE, order->GetCoupleCargo());
						break;
					}
					default: break;
				}
				break;
			}

			case WID_O_REFIT_DROPDOWN:
				this->OrderClick_Refit(index, true);
				break;

			case WID_O_COND_VARIABLE:
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_COND_VARIABLE, index);
				break;

			case WID_O_COND_COMPARATOR:
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_COND_COMPARATOR, index);
				break;
		}
	}

	void OnDragDrop(Point pt, WidgetID widget) override
	{
		switch (widget) {
			case WID_O_ORDER_LIST: {
				/* Only a real drag moves an order. See order_dragged. */
				if (!this->order_dragged) break;

				VehicleOrderID from_order = this->OrderGetSel();
				VehicleOrderID to_order = this->GetOrderFromPt(pt.y);

				if (!(from_order == to_order || from_order == INVALID_VEH_ORDER_ID || from_order > this->vehicle->GetNumOrders() || to_order == INVALID_VEH_ORDER_ID || to_order > this->vehicle->GetNumOrders()) &&
						Command<Commands::MoveOrder>::Post(STR_ERROR_CAN_T_MOVE_THIS_ORDER, this->vehicle->tile, this->vehicle->index, from_order, to_order)) {
					this->selected_order = -1;
					this->UpdateButtonState();
				}
				break;
			}

			case WID_O_DELETE:
				this->OrderClick_Delete();
				break;

			case WID_O_STOP_SHARING:
				this->OrderClick_StopSharing();
				break;
		}

		ResetObjectToPlace();
		this->order_dragged = false;

		if (this->order_over != INVALID_VEH_ORDER_ID) {
			/* End of drag-and-drop, hide dragged order destination highlight. */
			this->order_over = INVALID_VEH_ORDER_ID;
			this->SetWidgetDirty(WID_O_ORDER_LIST);
		}
	}

	EventState OnHotkey(int hotkey) override
	{
		if (this->vehicle->owner != _local_company) return EventState::NotHandled;

		switch (hotkey) {
			case OHK_SKIP:           this->OrderClick_Skip(); break;
			case OHK_DELETE:         this->OrderClick_Delete(); break;
			case OHK_GOTO:           this->OrderClick_Goto(OPOS_GOTO); break;
			case OHK_NONSTOP:        this->OrderClick_Nonstop(std::nullopt); break;
			case OHK_FULLLOAD:       this->OrderClick_FullLoad(OrderLoadType::FullLoadAny, true); break;
			case OHK_UNLOAD:         this->OrderClick_Unload(OrderUnloadType::Unload, true); break;
			case OHK_NEAREST_DEPOT:  this->OrderClick_NearestDepot(); break;
			case OHK_ALWAYS_SERVICE: this->OrderClick_Service(std::nullopt); break;
			case OHK_TRANSFER:       this->OrderClick_Unload(OrderUnloadType::Transfer, true); break;
			case OHK_NO_UNLOAD:      this->OrderClick_Unload(OrderUnloadType::NoUnload, true); break;
			case OHK_NO_LOAD:        this->OrderClick_FullLoad(OrderLoadType::NoLoad, true); break;
			default: return EventState::NotHandled;
		}
		return EventState::Handled;
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (this->goto_type == OPOS_DECOUPLE_DEST) {
			/* A station of ours under the pointer, read the same way a "go to"
			 * order reads one. Anything else -- open land, somebody else's
			 * station -- is not an answer, so the pointer stays loaded and the
			 * player can try again rather than having the picking silently
			 * end on a misclick. */
			if (!IsTileType(tile, TileType::Station)) return;
			StationID st = GetStationIndex(tile);
			const Station *station = Station::GetIfValid(st);
			if (station == nullptr || (station->owner != OWNER_NONE && station->owner != this->vehicle->owner)) return;

			if (Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index,
					this->OrderGetSel(), MOF_DECOUPLE_CARGO_DEST, st.base())) {
				ResetObjectToPlace();
			}
			return;
		}

		if (this->goto_type == OPOS_GOTO) {
			const Order cmd = GetOrderCmdFromTile(this->vehicle, tile);
			if (cmd.IsType(OT_NOTHING)) return;

			if (Command<Commands::InsertOrder>::Post(STR_ERROR_CAN_T_INSERT_NEW_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), cmd)) {
				/* With quick goto the Go To button stays active */
				if (!_settings_client.gui.quick_goto) ResetObjectToPlace();
			}
		}
	}

	bool OnVehicleSelect(const Vehicle *v) override
	{
		/* v is vehicle getting orders. Only copy/clone orders if vehicle doesn't have any orders yet.
		 * We disallow copying orders of other vehicles if we already have at least one order entry
		 * ourself as it easily copies orders of vehicles within a station when we mean the station.
		 * Obviously if you press CTRL on a non-empty orders vehicle you know what you are doing
		 * TODO: give a warning message */
		bool share_order = _ctrl_pressed || this->goto_type == OPOS_SHARE;
		if (this->vehicle->GetNumOrders() != 0 && !share_order) return false;

		if (Command<Commands::CloneOrder>::Post(share_order ? STR_ERROR_CAN_T_SHARE_ORDER_LIST : STR_ERROR_CAN_T_COPY_ORDER_LIST,
				this->vehicle->tile, share_order ? CO_SHARE : CO_COPY, this->vehicle->index, v->index)) {
			this->selected_order = -1;
			ResetObjectToPlace();
		}
		return true;
	}

	/**
	 * Clones an order list from a vehicle list.  If this doesn't make sense (because not all vehicles in the list have the same orders), then it displays an error.
	 * @param begin Begin iterator of the vehicle list.
	 * @param end End iterator of the vehicle list.
	 * @return This always returns true, which indicates that the contextual action handled the mouse click.
	 *         Note that it's correct behaviour to always handle the click even though an error is displayed,
	 *         because users aren't going to expect the default action to be performed just because they overlooked that cloning doesn't make sense.
	 */
	bool OnVehicleSelect(VehicleList::const_iterator begin, VehicleList::const_iterator end) override
	{
		bool share_order = _ctrl_pressed || this->goto_type == OPOS_SHARE;
		if (this->vehicle->GetNumOrders() != 0 && !share_order) return false;

		if (!share_order) {
			/* If CTRL is not pressed: If all the vehicles in this list have the same orders, then copy orders */
			if (AllEqual(begin, end, [](const Vehicle *v1, const Vehicle *v2) {
				return VehiclesHaveSameOrderList(v1, v2);
			})) {
				OnVehicleSelect(*begin);
			} else {
				ShowErrorMessage(GetEncodedString(STR_ERROR_CAN_T_COPY_ORDER_LIST), GetEncodedString(STR_ERROR_CAN_T_COPY_ORDER_VEHICLE_LIST), WarningLevel::Info);
			}
		} else {
			/* If CTRL is pressed: If all the vehicles in this list share orders, then copy orders */
			if (AllEqual(begin, end, [](const Vehicle *v1, const Vehicle *v2) {
				return v1->FirstShared() == v2->FirstShared();
			})) {
				OnVehicleSelect(*begin);
			} else {
				ShowErrorMessage(GetEncodedString(STR_ERROR_CAN_T_SHARE_ORDER_LIST), GetEncodedString(STR_ERROR_CAN_T_SHARE_ORDER_VEHICLE_LIST), WarningLevel::Info);
			}
		}

		return true;
	}

	void OnPlaceObjectAbort() override
	{
		this->goto_type = OPOS_NONE;
		this->SetWidgetDirty(WID_O_GOTO);
		/* The other button that loads the pointer comes back up with it,
		 * however the picking ended -- a station clicked, Escape, or the
		 * button pressed a second time. */
		this->SetWidgetDirty(WID_O_DECOUPLE_CARGO_DEST);

		/* Remove drag highlighting if it exists. */
		if (this->order_over != INVALID_VEH_ORDER_ID) {
			this->order_over = INVALID_VEH_ORDER_ID;
			this->SetWidgetDirty(WID_O_ORDER_LIST);
		}
	}

	void OnMouseDrag(Point pt, WidgetID widget) override
	{
		if (this->selected_order != -1 && widget == WID_O_ORDER_LIST) {
			/* The hand is on the order and moving: this is a drag, and letting
			 * go of it now is meant to put the order down somewhere. */
			this->order_dragged = true;

			/* An order is dragged.. */
			VehicleOrderID from_order = this->OrderGetSel();
			VehicleOrderID to_order = this->GetOrderFromPt(pt.y);
			uint num_orders = this->vehicle->GetNumOrders();

			if (from_order != INVALID_VEH_ORDER_ID && from_order <= num_orders) {
				if (to_order != INVALID_VEH_ORDER_ID && to_order <= num_orders) { // ..over an existing order.
					this->order_over = to_order;
					this->SetWidgetDirty(widget);
				} else if (from_order != to_order && this->order_over != INVALID_VEH_ORDER_ID) { // ..outside of the order list.
					this->order_over = INVALID_VEH_ORDER_ID;
					this->SetWidgetDirty(widget);
				}
			}
		}
	}

	void OnResize() override
	{
		/* Update the scroll bar */
		this->vscroll->SetCapacityFromWidget(this, WID_O_ORDER_LIST, WidgetDimensions::scaled.framerect.Vertical());
	}

	void OnMouseLoop() override
	{
		/* Between frames, with no click on its way in and nothing being drawn:
		 * the one safe moment to move every widget in the window. */
		if (this->couple_filter_resized) {
			this->couple_filter_resized = false;

			/* Grow the window by the row rather than take the row out of the
			 * order list. A plain ReInit() keeps the window the height it was
			 * and hands the difference to whatever in it can stretch, which is
			 * the list -- so switching the filter on cost the player a line of
			 * orders and shoved everything above it up a row. The player's
			 * own reading of it: let the row drop out of the bottom and leave
			 * what is above it where it was.
			 *
			 * How much it grew by is not something this can be told in advance,
			 * so it is measured: the row's height before the re-layout and
			 * after it. The second call then moves the window's bottom edge by
			 * exactly that, top-left staying where it is. It goes both ways --
			 * the row leaving shrinks the window again. */
			NWidgetBase *list = this->GetWidget<NWidgetBase>(WID_O_ORDER_LIST);
			int list_before = list != nullptr ? (int)list->current_y : 0;
			this->ReInit();
			int list_after = list != nullptr ? (int)this->GetWidget<NWidgetBase>(WID_O_ORDER_LIST)->current_y : 0;
			/* Keyed on the list and not on the row: what the player must not
			 * lose is his orders, and whatever the re-layout took from them is
			 * exactly what the window has to grow by. Measured rather than
			 * reckoned, because a re-layout also clamps the window to its new
			 * smallest size and the two do not add up to the row's height. */
			if (list_after != list_before) this->ReInit(0, list_before - list_after);
		}
	}

	static inline HotkeyList hotkeys{"order", {
		Hotkey('D', "skip", OHK_SKIP),
		Hotkey('F', "delete", OHK_DELETE),
		Hotkey('G', "goto", OHK_GOTO),
		Hotkey('H', "nonstop", OHK_NONSTOP),
		Hotkey('J', "fullload", OHK_FULLLOAD),
		Hotkey('K', "unload", OHK_UNLOAD),
		Hotkey(0, "nearest_depot", OHK_NEAREST_DEPOT),
		Hotkey(0, "always_service", OHK_ALWAYS_SERVICE),
		Hotkey(0, "transfer", OHK_TRANSFER),
		Hotkey(0, "no_unload", OHK_NO_UNLOAD),
		Hotkey(0, "no_load", OHK_NO_LOAD),
	}};
};

/** Nested widget definition for "your" train orders. */
static constexpr std::initializer_list<NWidgetPart> _nested_orders_train_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_O_CAPTION),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_TIMETABLE_VIEW), SetMinimalSize(61, 14), SetStringTip(STR_ORDERS_TIMETABLE_VIEW, STR_ORDERS_TIMETABLE_VIEW_TOOLTIP),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::Grey, WID_O_ORDER_LIST), SetMinimalSize(372, 62), SetToolTip(STR_ORDERS_LIST_TOOLTIP), SetResize(1, 1), SetScrollbar(WID_O_SCROLLBAR), EndContainer(),
		NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_O_SCROLLBAR),
	EndContainer(),

	/* First button row. */
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_TOP_ROW_GROUNDVEHICLE),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_NON_STOP), SetMinimalSize(93, 12), SetFill(1, 0),
															SetStringTip(STR_ORDER_NON_STOP, STR_ORDER_TOOLTIP_NON_STOP), SetResize(1, 0),
				NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_TOP_LEFT),
					NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_FULL_LOAD), SetMinimalSize(93, 12), SetFill(1, 0),
															SetStringTip(STR_ORDER_TOGGLE_FULL_LOAD, STR_ORDER_TOOLTIP_FULL_LOAD), SetResize(1, 0),
					NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_REFIT), SetMinimalSize(93, 12), SetFill(1, 0),
															SetStringTip(STR_ORDER_REFIT, STR_ORDER_REFIT_TOOLTIP), SetResize(1, 0),
				EndContainer(),
				NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_TOP_MIDDLE),
					NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_UNLOAD), SetMinimalSize(93, 12), SetFill(1, 0),
															SetStringTip(STR_ORDER_TOGGLE_UNLOAD, STR_ORDER_TOOLTIP_UNLOAD), SetResize(1, 0),
					NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_DEPOT_ACTION), SetMinimalSize(93, 12), SetFill(1, 0),
															SetStringTip(STR_JUST_STRING), SetResize(1, 0),
				EndContainer(),
				NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_TOP_RIGHT),
					NWidget(WWT_PANEL, Colours::Grey), SetMinimalSize(93, 12), SetFill(1, 0), SetResize(1, 0), EndContainer(),
					NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_REFIT_DROPDOWN), SetMinimalSize(93, 12), SetFill(1, 0),
															SetStringTip(STR_ORDER_REFIT_AUTO, STR_ORDER_REFIT_AUTO_TOOLTIP), SetResize(1, 0),
				EndContainer(),
			EndContainer(),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COND_VARIABLE), SetMinimalSize(124, 12), SetFill(1, 0),
															SetToolTip(STR_ORDER_CONDITIONAL_VARIABLE_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COND_COMPARATOR), SetMinimalSize(124, 12), SetFill(1, 0),
															SetToolTip(STR_ORDER_CONDITIONAL_COMPARATOR_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_COND_VALUE), SetMinimalSize(124, 12), SetFill(1, 0),
															SetToolTip(STR_ORDER_CONDITIONAL_VALUE_TOOLTIP), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_O_SHARED_ORDER_LIST), SetAspect(1), SetSpriteTip(SPR_SHARED_ORDERS_ICON, STR_ORDERS_VEH_WITH_SHARED_ORDERS_LIST_TOOLTIP),
	EndContainer(),

	/* Second button row. */
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_SKIP), SetMinimalSize(74, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_SKIP_BUTTON, STR_ORDERS_SKIP_TOOLTIP), SetResize(1, 0),
			/* And its opposite beside it: back to the previous order. The
			 * player's ask -- a train sent one order too far is otherwise
			 * only brought back by skipping all the way round. */
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_BACK), SetMinimalSize(74, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_BACK_BUTTON, STR_ORDERS_BACK_TOOLTIP), SetResize(1, 0),
			/* Reversing out of a station is done to a whole order the way
			 * skipping and deleting are, so it sits with them, and the
			 * automatic departure next to it as the other answer to the same
			 * question. All five are narrowed so the row stays the width it
			 * was. */
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_REVERSE_OUT), SetMinimalSize(74, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_REVERSE_OUT, STR_ORDER_REVERSE_OUT_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_AUTO_DEPARTURE), SetMinimalSize(74, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_AUTO_DEPARTURE, STR_ORDER_AUTO_DEPARTURE_TOOLTIP), SetResize(1, 0),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_BOTTOM_MIDDLE),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_DELETE), SetMinimalSize(74, 12), SetFill(1, 0),
														SetStringTip(STR_ORDERS_DELETE_BUTTON, STR_ORDERS_DELETE_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_STOP_SHARING), SetMinimalSize(74, 12), SetFill(1, 0),
														SetStringTip(STR_ORDERS_STOP_SHARING_BUTTON, STR_ORDERS_STOP_SHARING_TOOLTIP), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_GOTO), SetMinimalSize(74, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_GO_TO_BUTTON, STR_ORDERS_GO_TO_TOOLTIP), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),

	/* Couple row: trains only. Which buttons it holds depends on what kind of
	 * order is selected -- station orders get the decouple/couple ones, depot
	 * orders the turn-around one. See FEATURE_DESIGN_COUPLING_TOW.md. */
	NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_DECOUPLE),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_WAIT_COUPLE), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_WAIT_COUPLE, STR_ORDER_WAIT_COUPLE_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_GOTO_COUPLE), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_GOTO_COUPLE, STR_ORDER_GOTO_COUPLE_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_DECOUPLE), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_DECOUPLE_BUTTON, STR_ORDERS_DECOUPLE_TOOLTIP), SetResize(1, 0),
		EndContainer(),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_TURN_AROUND_DEPOT), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_TURN_AROUND_DEPOT, STR_ORDER_TURN_AROUND_DEPOT_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_GOTO_COUPLE_DEPOT), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_GOTO_COUPLE, STR_ORDER_GOTO_COUPLE_DEPOT_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_DECOUPLE_DEPOT), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_DECOUPLE_BUTTON, STR_ORDERS_DECOUPLE_DEPOT_TOOLTIP), SetResize(1, 0),
		EndContainer(),
		/* Plane order is the enum order: station, depot, station waypoint, empty. */
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_HONK), SetMinimalSize(372, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_HONK, STR_ORDER_HONK_TOOLTIP), SetResize(1, 0),
		EndContainer(),
		/* The same height with nothing in it, so the window does not change size
		 * as the player clicks from one order to another. A panel rather than a
		 * gap: a gap is a hole in the window with the desktop showing through,
		 * which looks like something missing rather than like a row with
		 * nothing in it. */
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_PANEL, Colours::Grey), SetMinimalSize(124, 12), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
		/* A road vehicle's station order: how it gets carried on from here.
		 * The whole row is one dropdown, because the four ways plus "none"
		 * cannot be written across four buttons in the width there is, and
		 * what they say has to be readable -- a list has room for a sentence.
		 * See road_on_rail.h. */
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_BOARD_MODE), SetMinimalSize(372, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_BOARD_MODE_TOOLTIP), SetResize(1, 0),
		EndContainer(),
	EndContainer(),

	/* What a coupling order will accept when it gets there: how full the wagons
	 * are, what they carry, and how many of them there are. Only there while an
	 * order is actually going to collect something -- an order that is not
	 * carries no settings for what it is not going to do. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_COUPLE_FILTER),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COUPLE_LOAD), SetMinimalSize(93, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_COUPLE_LOAD_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COUPLE_CARGO), SetMinimalSize(93, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_COUPLE_CARGO_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_COUPLE_COUNT), SetMinimalSize(93, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_COUPLE_COUNT_BUTTON, STR_ORDER_COUPLE_COUNT_TOOLTIP), SetResize(1, 0),
			/* Three buttons of 124 re-laid as four of 93: the row is the same
			 * 372 wide it was and the window does not grow by a point. */
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COUPLE_BUY), SetMinimalSize(93, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_COUPLE_BUY_OFF, STR_ORDER_COUPLE_BUY_TOOLTIP), SetResize(1, 0),
		EndContainer(),
	EndContainer(),

	/* The bottom row, three places wide. On the left, selling the train for
	 * scrap -- which is about the train and not about any one order, and is
	 * therefore always there. In the middle, where the cargo of the wagons the
	 * selected order puts down is bound, which is only there while an order is
	 * actually going to put some down, the same way the couple filter above is
	 * only there while one is going to collect; the place itself stays, empty.
	 * The right-hand place is kept free for whatever comes next.
	 *
	 * The row used to come and go with the middle button. It stays now, because
	 * a button the player can only reach by first selecting the right kind of
	 * order is a button he cannot reach when he needs it -- and the moment he
	 * needs this one is when a train is stuck somewhere with no order worth
	 * selecting. */
	NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_DECOUPLE_DEST),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_SELL_TRAIN), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_SELL_TRAIN, STR_ORDER_SELL_TRAIN_TOOLTIP), SetResize(1, 0),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_DECOUPLE_DEST_BTN),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_DECOUPLE_CARGO_DEST), SetMinimalSize(124, 12), SetFill(1, 0),
														SetStringTip(STR_ORDER_DECOUPLE_CARGO_DEST_NONE, STR_ORDER_DECOUPLE_CARGO_DEST_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_PANEL, Colours::Grey), SetMinimalSize(124, 12), SetFill(1, 0), SetResize(1, 0),
				EndContainer(),
				/* The two places stood empty while the selected order collected
				 * rather than dropped, and they are the only room in the window
				 * for the two ways a collecting order can read its filters. */
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_COUPLE_FIND), SetMinimalSize(124, 12), SetFill(1, 0),
														SetStringTip(STR_ORDER_COUPLE_FIND, STR_ORDER_COUPLE_FIND_TOOLTIP), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_SELL_WAGONS),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_SELL_WAGONS), SetMinimalSize(124, 12), SetFill(1, 0),
														SetStringTip(STR_ORDER_SELL_WAGONS, STR_ORDER_SELL_WAGONS_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_PANEL, Colours::Grey), SetMinimalSize(124, 12), SetFill(1, 0), SetResize(1, 0),
				EndContainer(),
				/* The third plane, beside the empty panel and not inside it: a
				 * panel is a container and its EndContainer() is its own. Put
				 * inside, the selection kept two planes and was told to show a
				 * third, and the window brought the game down on its first
				 * repaint. */
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_COUPLE_SEARCH), SetMinimalSize(124, 12), SetFill(1, 0),
														SetStringTip(STR_ORDER_COUPLE_SEARCH, STR_ORDER_COUPLE_SEARCH_TOOLTIP), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
	EndContainer(),

};

/** Window definition for the train orders window. */
static WindowDesc _orders_train_desc(
	WindowPosition::Automatic, "view_vehicle_orders_train", 384, 100,
	WindowClass::VehicleOrders, WindowClass::VehicleView,
	WindowDefaultFlag::Construction,
	_nested_orders_train_widgets,
	&OrdersWindow::hotkeys
);

/** Nested widget definition for "your" orders (non-train). */
static constexpr std::initializer_list<NWidgetPart> _nested_orders_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_O_CAPTION), SetStringTip(STR_ORDERS_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_TIMETABLE_VIEW), SetMinimalSize(61, 14), SetStringTip(STR_ORDERS_TIMETABLE_VIEW, STR_ORDERS_TIMETABLE_VIEW_TOOLTIP),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::Grey, WID_O_ORDER_LIST), SetMinimalSize(372, 62), SetToolTip(STR_ORDERS_LIST_TOOLTIP), SetResize(1, 1), SetScrollbar(WID_O_SCROLLBAR), EndContainer(),
		NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_O_SCROLLBAR),
	EndContainer(),

	/* First button row. */
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_TOP_ROW),
			/* Load + unload + refit buttons. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_FULL_LOAD), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_TOGGLE_FULL_LOAD, STR_ORDER_TOOLTIP_FULL_LOAD), SetResize(1, 0),
				NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_UNLOAD), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_TOGGLE_UNLOAD, STR_ORDER_TOOLTIP_UNLOAD), SetResize(1, 0),
				NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_REFIT_DROPDOWN), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_REFIT_AUTO, STR_ORDER_REFIT_AUTO_TOOLTIP), SetResize(1, 0),
			EndContainer(),
			/* Refit + service buttons. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_REFIT), SetMinimalSize(186, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_REFIT, STR_ORDER_REFIT_TOOLTIP), SetResize(1, 0),
				NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_DEPOT_ACTION), SetMinimalSize(124, 12), SetFill(1, 0),
													SetResize(1, 0),
			EndContainer(),

			/* Buttons for setting a condition. */
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COND_VARIABLE), SetMinimalSize(124, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_CONDITIONAL_VARIABLE_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COND_COMPARATOR), SetMinimalSize(124, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_CONDITIONAL_COMPARATOR_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_COND_VALUE), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_JUST_COMMA, STR_ORDER_CONDITIONAL_VALUE_TOOLTIP), SetResize(1, 0),
			EndContainer(),
		EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_O_SHARED_ORDER_LIST), SetAspect(1), SetSpriteTip(SPR_SHARED_ORDERS_ICON, STR_ORDERS_VEH_WITH_SHARED_ORDERS_LIST_TOOLTIP),
	EndContainer(),

	/* Second button row. */
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_SKIP), SetMinimalSize(124, 12), SetFill(1, 0),
											SetStringTip(STR_ORDERS_SKIP_BUTTON, STR_ORDERS_SKIP_TOOLTIP), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_BACK), SetMinimalSize(124, 12), SetFill(1, 0),
											SetStringTip(STR_ORDERS_BACK_BUTTON, STR_ORDERS_BACK_TOOLTIP), SetResize(1, 0),
		NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_BOTTOM_MIDDLE),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_DELETE), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_DELETE_BUTTON, STR_ORDERS_DELETE_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_STOP_SHARING), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_STOP_SHARING_BUTTON, STR_ORDERS_STOP_SHARING_TOOLTIP), SetResize(1, 0),
		EndContainer(),
		NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_GOTO), SetMinimalSize(124, 12), SetFill(1, 0),
											SetStringTip(STR_ORDERS_GO_TO_BUTTON, STR_ORDERS_GO_TO_TOOLTIP), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

/** Window definition for the orders window for road vehicles, ships and aircraft. */
static WindowDesc _orders_desc(
	WindowPosition::Automatic, "view_vehicle_orders", 384, 100,
	WindowClass::VehicleOrders, WindowClass::VehicleView,
	WindowDefaultFlag::Construction,
	_nested_orders_widgets,
	&OrdersWindow::hotkeys
);

/** Nested widget definition for competitor orders. */
static constexpr std::initializer_list<NWidgetPart> _nested_other_orders_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_O_CAPTION), SetStringTip(STR_ORDERS_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_TIMETABLE_VIEW), SetMinimalSize(61, 14), SetStringTip(STR_ORDERS_TIMETABLE_VIEW, STR_ORDERS_TIMETABLE_VIEW_TOOLTIP),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::Grey, WID_O_ORDER_LIST), SetMinimalSize(372, 72), SetToolTip(STR_ORDERS_LIST_TOOLTIP), SetResize(1, 1), SetScrollbar(WID_O_SCROLLBAR), EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_O_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, Colours::Grey),
		EndContainer(),
	EndContainer(),
};

/** Window definition for the orders window for other companies. */
static WindowDesc _other_orders_desc(
	WindowPosition::Automatic, "view_vehicle_orders_competitor", 384, 86,
	WindowClass::VehicleOrders, WindowClass::VehicleView,
	WindowDefaultFlag::Construction,
	_nested_other_orders_widgets,
	&OrdersWindow::hotkeys
);

/**
 * Put an orders window's selection on one order, for the rig.
 *
 * The probes used to click into the list at a guessed height, and the guess
 * was one row short: a probe asked about order 1 was answered about order 0,
 * and said so in words that blamed the order. The window knows its own rows;
 * this asks it directly.
 * @param w     the orders window
 * @param index the order to select
 * @return whether the window was an orders window and the order exists
 */
bool TestSelectOrderInWindow(Window *w, int index)
{
	OrdersWindow *ow = dynamic_cast<OrdersWindow *>(w);
	return ow != nullptr && ow->SelectOrderForTest(index);
}

void ShowOrdersWindow(const Vehicle *v)
{
	CloseWindowById(WindowClass::VehicleDetails, v->index, false);
	CloseWindowById(WindowClass::VehicleTimetable, v->index, false);
	if (BringWindowToFrontById(WindowClass::VehicleOrders, v->index) != nullptr) return;

	/* Using a different WindowDescs for _local_company causes problems.
	 * Due to this we have to close order windows in ChangeWindowOwner/CloseCompanyWindows,
	 * because we cannot change switch the WindowDescs and keeping the old WindowDesc results
	 * in crashed due to missing widget.
	 * TODO Rewrite the order GUI to not use different WindowDescs.
	 */
	if (v->owner != _local_company) {
		new OrdersWindow(_other_orders_desc, v);
	} else {
		new OrdersWindow(v->IsGroundVehicle() ? _orders_train_desc : _orders_desc, v);
	}
}

/**
 * What the orders window lets a train do on leaving an order -- the states of
 * its "reverse out" and "leave by itself" buttons -- for the test rig. Opens
 * the window and selects the order as a click on its row would.
 * @param v the vehicle
 * @param sel the order
 * @return the two buttons' states, or nothing when there is no window
 */
std::optional<OrderDirectionButtons> TestOrderDirectionButtons(const Vehicle *v, VehicleOrderID sel)
{
	ShowOrdersWindow(v);
	OrdersWindow *w = dynamic_cast<OrdersWindow *>(FindWindowById(WindowClass::VehicleOrders, v->index));
	if (w == nullptr || !w->SelectOrderForTest(sel)) return std::nullopt;
	return OrderDirectionButtons{w->IsWidgetDisabled(WID_O_REVERSE_OUT), w->IsWidgetLowered(WID_O_REVERSE_OUT), w->IsWidgetDisabled(WID_O_AUTO_DEPARTURE), w->IsWidgetLowered(WID_O_AUTO_DEPARTURE)};
}

