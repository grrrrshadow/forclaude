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
};

/**
 * Build the list of cargoes a coupling order can ask for: every cargo in the
 * game, plus asking for none in particular.
 * @return the list to show
 */
static DropDownList BuildCoupleCargoDropDown()
{
	DropDownList list;
	list.push_back(MakeDropDownListStringItem(STR_ORDER_COUPLE_CARGO_ANY, INVALID_CARGO, false));
	for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
		list.push_back(MakeDropDownListStringItem(cs->name, cs->Index(), false));
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
				if (!order->GetNonStopType().Test(OrderNonStopFlag::GoVia)) {
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
					line += GetString(STR_ORDER_GOTO_COUPLE_SUFFIX);

					/* And what it is going to accept, for each filter that has
					 * been set. Read off the line, a whole list of orders says
					 * at a glance which engine is going for which wagons. */
					if (order->GetCoupleLoad() != OrderCoupleLoad::Any) {
						line += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_PART, STR_ORDER_COUPLE_LOAD_ANY + to_underlying(order->GetCoupleLoad()));
					}
					if (IsValidCargoType(order->GetCoupleCargo())) {
						line += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_CARGO, CargoSpec::Get(order->GetCoupleCargo())->name);
					}
					if (order->GetCoupleCount() != 0) {
						line += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_COUNT, order->GetCoupleCount());
					}
				}

				/* Waiting for a couple had no way of showing at all, so the
				 * only place it could be read was a button that speaks for one
				 * order at a time. Everything an order is going to do belongs
				 * on its own line, where a whole list can be read at once. */
				if (v->type == VehicleType::Train && order->ShouldWaitForCouple()) line += GetString(STR_ORDER_WAIT_COUPLE_SUFFIX);

				/* How many vehicles stay with the train belongs on the order
				 * line with everything else the order is going to do. A button
				 * speaks for the one order selected, so a train that decouples
				 * at several stations learned nothing from it about any. */
				if (!timetable && v->type == VehicleType::Train && order->GetDecoupleCount() != 0) {
					line += GetString(STR_ORDER_DECOUPLE_SUFFIX, order->GetDecoupleCount());
				}

				/* Reversing out is about where the train goes next, not about
				 * how long it stays, so it has no place in the timetable. Nor
				 * is it shown when the order decouples here, because then it is
				 * not carried out -- an order saved before the two were made
				 * exclusive can still have both set, and saying so would be a
				 * plain lie about what the train is going to do. */
				if (!timetable && v->type == VehicleType::Train && order->ShouldReverseOutOfStation() &&
						order->GetDecoupleCount() == 0) {
					line += GetString(STR_ORDER_REVERSE_OUT_SUFFIX);
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
				line += GetString(STR_ORDER_TURN_AROUND_DEPOT_SUFFIX);
			}

			/* A depot order that collects or puts down reads it off its own
			 * line, same as a station order does. */
			if (v->type == VehicleType::Train && order->ShouldGoToCouple()) {
				line += GetString(STR_ORDER_GOTO_COUPLE_SUFFIX);
				if (order->GetCoupleLoad() != OrderCoupleLoad::Any) {
					line += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_PART, STR_ORDER_COUPLE_LOAD_ANY + to_underlying(order->GetCoupleLoad()));
				}
				if (IsValidCargoType(order->GetCoupleCargo())) {
					line += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_CARGO, CargoSpec::Get(order->GetCoupleCargo())->name);
				}
				if (order->GetCoupleCount() != 0) {
					line += GetString(STR_ORDER_COUPLE_FILTER_SUFFIX_COUNT, order->GetCoupleCount());
				}
			}
			if (!timetable && v->type == VehicleType::Train && order->GetDecoupleCount() != 0) {
				line += GetString(STR_ORDER_DECOUPLE_SUFFIX, order->GetDecoupleCount());
			}

			break;

		case OT_GOTO_WAYPOINT:
			line = GetString(order->GetNonStopType().Test(OrderNonStopFlag::NonStop) ? STR_ORDER_GO_NON_STOP_TO_WAYPOINT : STR_ORDER_GO_TO_WAYPOINT, order->GetDestination());
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

	DrawString(rtl ? left : middle, rtl ? middle : right, y, line, colour);
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
private:
	/** Under what reason are we using the PlaceObject functionality? */
	enum OrderPlaceObjectState : uint8_t {
		OPOS_NONE,
		OPOS_GOTO,
		OPOS_CONDITIONAL,
		OPOS_SHARE,
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
		DP_COUPLE_ROW_EMPTY   = 2, ///< Hold the row's height open when it has no buttons to show.
	};

	int selected_order = -1;
	/** Is the currently-open query string editing the decouple count (WID_O_DECOUPLE_COUNT) rather than a conditional order value (WID_O_COND_VALUE)? Both use the same OnQueryTextFinished. */
	bool querying_decouple_count = false;
	/** Same again for the number of vehicles a coupling order will accept (WID_O_COUPLE_COUNT). */
	bool querying_couple_count = false;
	/** The filter row has appeared or gone, so the window has to be laid out again at a moment when nothing is being delivered to it. */
	bool couple_filter_resized = false;
	VehicleOrderID order_over = INVALID_VEH_ORDER_ID; ///< Order over which another order is dragged, \c INVALID_VEH_ORDER_ID if none.
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
				fill.height = resize.height = GetCharacterHeight(FontSize::Normal);
				size.height = 6 * resize.height + padding.height;
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
		/* skip */
		this->SetWidgetDisabledState(WID_O_SKIP, this->vehicle->GetNumOrders() <= 1);

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

					/* Can only do refitting when stopping at the destination and loading cargo.
					 * Also enable the button if a refit is already set to allow clearing it. */
					this->SetWidgetDisabledState(WID_O_REFIT_DROPDOWN,
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
					this->SetWidgetDisabledState(WID_O_REFIT,
							order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service) || order->GetDepotActionType().Test(OrderDepotActionFlag::Halt) ||
							(!this->can_do_refit && !order->IsRefit()));
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
					this->SetWidgetDisabledState(WID_O_COND_VALUE, ocv == OrderConditionVariable::DrivingBackwards || ocv == OrderConditionVariable::RequiresService || ocv == OrderConditionVariable::Unconditionally);
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
				bool decoupling = order->GetDecoupleCount() != 0;
				bool reversing_out = order->ShouldReverseOutOfStation();

				this->SetWidgetDisabledState(WID_O_GOTO_COUPLE, waiting || decoupling);
				this->SetWidgetDisabledState(WID_O_WAIT_COUPLE, collecting || decoupling || reversing_out);

				bool can_reverse_out = !decoupling && !waiting;
				this->SetWidgetDisabledState(WID_O_REVERSE_OUT, !can_reverse_out);
				this->SetWidgetLoweredState(WID_O_REVERSE_OUT, can_reverse_out && reversing_out);
			} else if (is_train && order->IsType(OT_GOTO_DEPOT)) {
				decouple_sel->SetDisplayedPlane(DP_COUPLE_ROW_DEPOT);
				this->SetWidgetLoweredState(WID_O_TURN_AROUND_DEPOT, order->ShouldTurnAroundInDepot());

				/* Putting wagons down and taking wagons on are opposites here the
				 * same way they are at a station: one order does one or the
				 * other. Waiting has no button at a depot at all -- trains couple
				 * at stations, in the open; the only thing to couple to in a
				 * shed is a stored rake. */
				bool collecting = order->ShouldGoToCouple();
				bool decoupling = order->GetDecoupleCount() != 0;
				this->SetWidgetLoweredState(WID_O_GOTO_COUPLE_DEPOT, collecting);
				this->SetWidgetDisabledState(WID_O_GOTO_COUPLE_DEPOT, decoupling);
				this->SetWidgetLoweredState(WID_O_DECOUPLE_DEPOT, decoupling);
				this->SetWidgetDisabledState(WID_O_DECOUPLE_DEPOT, collecting);
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
		 * station order; what changes is whether the rest of it can be used. */
		bool can_decouple = this->vehicle->type == VehicleType::Train && order != nullptr && order->IsType(OT_GOTO_STATION);
		bool decoupling = can_decouple && order->GetDecoupleCount() != 0;

		this->SetWidgetDisabledState(WID_O_DECOUPLE, !can_decouple ||
				order->ShouldReverseOutOfStation() || order->ShouldWaitForCouple() || order->ShouldGoToCouple());
		this->SetWidgetLoweredState(WID_O_DECOUPLE, decoupling);

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

		this->SetDirty();
	}

	void OnPaint() override
	{
		if (this->vehicle->owner != _local_company) {
			this->selected_order = -1; // Disable selection any selected row at a competitor order window.
		} else {
			this->SetWidgetLoweredState(WID_O_GOTO, this->goto_type != OPOS_NONE);
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

			case WID_O_COUPLE_CARGO: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				if (!IsValidCargoType(order->GetCoupleCargo())) return GetString(STR_ORDER_COUPLE_CARGO_ANY);
				return GetString(STR_ORDER_COUPLE_CARGO_TYPE, CargoSpec::Get(order->GetCoupleCargo())->name);
			}

			case WID_O_COUPLE_COUNT: {
				const Order *order = this->vehicle->GetOrder(this->OrderGetSel());
				if (order == nullptr) return {};
				if (order->GetCoupleCount() == 0) return GetString(STR_ORDER_COUPLE_COUNT_ANY);
				return GetString(STR_ORDER_COUPLE_COUNT_BUTTON, order->GetCoupleCount());
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
						SetObjectToPlaceWnd(SPR_CURSOR_MOUSE, PAL_NONE, HT_DRAG, this);
					}
				}

				this->UpdateButtonState();
				break;
			}

			case WID_O_SKIP:
				this->OrderClick_Skip();
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
					if (ocv == OrderConditionVariable::DrivingBackwards && this->vehicle->type != VehicleType::Train) continue;
					list.push_back(MakeDropDownListStringItem(STR_ORDER_CONDITIONAL_LOAD_PERCENTAGE + to_underlying(ocv), to_underlying(ocv)));
				}
				ShowDropDownList(this, std::move(list), to_underlying(this->vehicle->GetOrder(this->OrderGetSel())->GetConditionVariable()), WID_O_COND_VARIABLE);
				break;
			}

			case WID_O_COND_COMPARATOR: {
				const Order *o = this->vehicle->GetOrder(this->OrderGetSel());
				assert(o != nullptr);
				ShowDropDownMenu(this, _order_conditional_condition, to_underlying(o->GetConditionComparator()), WID_O_COND_COMPARATOR, 0, (o->GetConditionVariable() == OrderConditionVariable::RequiresService || o->GetConditionVariable() == OrderConditionVariable::DrivingBackwards) ? 0x3F : 0xC0);
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
				/* One button for the whole thing: it asks how many vehicles the
				 * train keeps here, and nought is how decoupling is switched
				 * off again.
				 *
				 * Pressed while it is already on, it simply switches decoupling
				 * off. There is nothing left to ask at that point, and a number
				 * box standing in the way of turning something off is a box that
				 * only ever gets a nought typed into it. */
				if (order->GetDecoupleCount() != 0) {
					Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_DECOUPLE_COUNT, 0);
					break;
				}
				/* Switch it on at one first, and only then ask. One -- the
				 * engine keeps itself and puts everything behind it down -- is
				 * what is wanted nearly every time, and a number box is not
				 * something to have to work through to get it.
				 *
				 * It has to be this way round: a query box calls back only when
				 * the text is changed, so a box that opens already saying the
				 * right answer does nothing at all when it is accepted. Setting
				 * it first means accepting the box leaves it at one, and typing
				 * something else changes it. */
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_DECOUPLE_COUNT, 1);

				this->querying_decouple_count = true;
				this->querying_couple_count = false;
				ShowQueryString(GetString(STR_JUST_INT, 1), STR_ORDER_DECOUPLE_COUNT_CAPT, 4, this, CS_NUMERAL, {});
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
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_GOTO_COUPLE, order->ShouldGoToCouple() ? 0 : 1);
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
				ShowDropDownList(this, BuildCoupleCargoDropDown(), order->GetCoupleCargo(), WID_O_COUPLE_CARGO);
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
				ShowQueryString(GetString(STR_JUST_INT, order->GetCoupleCount()), STR_ORDER_COUPLE_COUNT_CAPT, 4, this, CS_NUMERAL, {});
				break;
			}

			case WID_O_SHARED_ORDER_LIST:
				ShowVehicleListWindow(this->vehicle);
				break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value() || str->empty()) return;

		VehicleOrderID sel = this->OrderGetSel();
		auto value = ParseInteger(*str, 10, true);
		if (!value.has_value()) return;

		if (this->querying_decouple_count) {
			Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, sel, MOF_DECOUPLE_COUNT, Clamp(*value, 0, UINT8_MAX));
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

			case WID_O_COUPLE_CARGO:
				Command<Commands::ModifyOrder>::Post(STR_ERROR_CAN_T_MODIFY_THIS_ORDER, this->vehicle->tile, this->vehicle->index, this->OrderGetSel(), MOF_COUPLE_CARGO, index);
				break;

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

		/* Remove drag highlighting if it exists. */
		if (this->order_over != INVALID_VEH_ORDER_ID) {
			this->order_over = INVALID_VEH_ORDER_ID;
			this->SetWidgetDirty(WID_O_ORDER_LIST);
		}
	}

	void OnMouseDrag(Point pt, WidgetID widget) override
	{
		if (this->selected_order != -1 && widget == WID_O_ORDER_LIST) {
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
			this->ReInit();
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
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_SKIP), SetMinimalSize(93, 12), SetFill(1, 0),
													SetStringTip(STR_ORDERS_SKIP_BUTTON, STR_ORDERS_SKIP_TOOLTIP), SetResize(1, 0),
			/* Reversing out of a station is done to a whole order the way
			 * skipping and deleting are, so it sits with them. All four are
			 * narrowed so the row stays the width it was. */
			NWidget(WWT_TEXTBTN, Colours::Grey, WID_O_REVERSE_OUT), SetMinimalSize(93, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_REVERSE_OUT, STR_ORDER_REVERSE_OUT_TOOLTIP), SetResize(1, 0),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_BOTTOM_MIDDLE),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_DELETE), SetMinimalSize(93, 12), SetFill(1, 0),
														SetStringTip(STR_ORDERS_DELETE_BUTTON, STR_ORDERS_DELETE_TOOLTIP), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_STOP_SHARING), SetMinimalSize(93, 12), SetFill(1, 0),
														SetStringTip(STR_ORDERS_STOP_SHARING_BUTTON, STR_ORDERS_STOP_SHARING_TOOLTIP), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_BUTTON_DROPDOWN, Colours::Grey, WID_O_GOTO), SetMinimalSize(93, 12), SetFill(1, 0),
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
		/* The same height with nothing in it, so the window does not change size
		 * as the player clicks from one order to another. A panel rather than a
		 * gap: a gap is a hole in the window with the desktop showing through,
		 * which looks like something missing rather than like a row with
		 * nothing in it. */
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_PANEL, Colours::Grey), SetMinimalSize(124, 12), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
	EndContainer(),

	/* What a coupling order will accept when it gets there: how full the wagons
	 * are, what they carry, and how many of them there are. Only there while an
	 * order is actually going to collect something -- an order that is not
	 * carries no settings for what it is not going to do. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	NWidget(NWID_SELECTION, Colours::Invalid, WID_O_SEL_COUPLE_FILTER),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COUPLE_LOAD), SetMinimalSize(124, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_COUPLE_LOAD_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_O_COUPLE_CARGO), SetMinimalSize(124, 12), SetFill(1, 0),
													SetToolTip(STR_ORDER_COUPLE_CARGO_TOOLTIP), SetResize(1, 0),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_O_COUPLE_COUNT), SetMinimalSize(124, 12), SetFill(1, 0),
													SetStringTip(STR_ORDER_COUPLE_COUNT_BUTTON, STR_ORDER_COUPLE_COUNT_TOOLTIP), SetResize(1, 0),
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
