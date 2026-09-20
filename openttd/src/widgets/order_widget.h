/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file order_widget.h Types related to the order widgets. */

#ifndef WIDGETS_ORDER_WIDGET_H
#define WIDGETS_ORDER_WIDGET_H

/** Widgets of the #OrdersWindow class. */
enum OrderWidgets : WidgetID {
	WID_O_CAPTION,                   ///< Caption of the window.
	WID_O_TIMETABLE_VIEW,            ///< Toggle timetable view.
	WID_O_ORDER_LIST,                ///< Order list panel.
	WID_O_SCROLLBAR,                 ///< Order list scrollbar.
	WID_O_SKIP,                      ///< Skip current order.
	WID_O_BACK,                      ///< Go back to the previous order; the opposite of skip.
	WID_O_DELETE,                    ///< Delete selected order.
	WID_O_STOP_SHARING,              ///< Stop sharing orders.
	WID_O_NON_STOP,                  ///< Goto non-stop to destination.
	WID_O_DEPOT_UNBUNCHING,          ///< Toggle unbunching.
	WID_O_GOTO,                      ///< Goto destination.
	WID_O_FULL_LOAD,                 ///< Select full load.
	WID_O_UNLOAD,                    ///< Select unload.
	WID_O_REFIT,                     ///< Select refit.
	WID_O_DEPOT_ACTION,              ///< Dropdown to select the depot action (stop, service if needed, unbunch).
	WID_O_REFIT_DROPDOWN,            ///< Open refit options.
	WID_O_COND_VARIABLE,             ///< Choose condition variable.
	WID_O_COND_COMPARATOR,           ///< Choose condition type.
	WID_O_COND_VALUE,                ///< Choose condition value.
	WID_O_SEL_TOP_LEFT,              ///< #NWID_SELECTION widget for left part of the top row of the 'your train' order window.
	WID_O_SEL_TOP_MIDDLE,            ///< #NWID_SELECTION widget for middle part of the top row of the 'your train' order window.
	WID_O_SEL_TOP_RIGHT,             ///< #NWID_SELECTION widget for right part of the top row of the 'your train' order window.
	WID_O_SEL_TOP_ROW_GROUNDVEHICLE, ///< #NWID_SELECTION widget for the top row of the 'your train' order window.
	WID_O_SEL_TOP_ROW,               ///< #NWID_SELECTION widget for the top row of the 'your non-trains' order window.
	WID_O_SEL_BOTTOM_MIDDLE,         ///< #NWID_SELECTION widget for the middle part of the bottom row of the 'your train' order window.
	WID_O_SHARED_ORDER_LIST,         ///< Open list of shared vehicles.
	WID_O_SEL_DECOUPLE,              ///< #NWID_SELECTION widget for the 'decouple'/'wait to couple'/'go to couple' row, trains + station orders only.
	WID_O_DECOUPLE,                  ///< Toggle decoupling wagons at this station.
	WID_O_SEL_DECOUPLE_ROW,          ///< #NWID_SELECTION widget for the row of decoupling settings, shown once decoupling is on.
	WID_O_DECOUPLE_COUNT,            ///< Set how many vehicles to keep when decoupling on departure from this station order.
	WID_O_WAIT_COUPLE,               ///< Toggle waiting at this station for a partner train to couple with.
	WID_O_GOTO_COUPLE,               ///< Toggle travelling to this station (reversing if needed) to couple with a partner train there.
	WID_O_TURN_AROUND_DEPOT,         ///< Toggle turning the train around while it is in this depot.
	WID_O_GOTO_COUPLE_DEPOT,         ///< Toggle travelling to this depot to couple with a rake of wagons stored there.
	WID_O_BOARD_MODE,                ///< Road vehicles: how this station's order gets the vehicle carried on from here, one of five (see OrderBoardMode).
	WID_O_DECOUPLE_DEPOT,            ///< Toggle decoupling wagons in this depot.
	WID_O_REVERSE_OUT,               ///< Toggle reversing out of this station on departure.
	WID_O_AUTO_DEPARTURE,            ///< Toggle leaving this station engine first and then the shortest way.
	WID_O_HONK,                      ///< Toggle sounding the horn when passing this station waypoint.
	WID_O_SEL_COUPLE_FILTER,         ///< #NWID_SELECTION widget for the row saying what a coupling order will accept, shown once it is going to collect something.
	WID_O_COUPLE_LOAD,               ///< Choose how full the wagons to be collected have to be.
	WID_O_COUPLE_CARGO,              ///< Choose which cargo the wagons to be collected have to carry.
	WID_O_COUPLE_COUNT,              ///< Set how many vehicles the rake to be collected has to have.
	WID_O_COUPLE_BUY,                ///< Pick the wagon this depot order buys when the shed is short of them, or stop it buying.
	WID_O_SEL_DECOUPLE_DEST,         ///< #NWID_SELECTION widget for the bottom row of the train orders window; trains only, always shown, because the sell button in it is not tied to any one order.
	WID_O_SEL_DECOUPLE_DEST_BTN,     ///< #NWID_SELECTION widget for the middle place of that row: the cargo destination button, or nothing while the selected order puts no wagons down.
	WID_O_DECOUPLE_CARGO_DEST,       ///< Pick the station whose cargo the wagons this order puts down are to load.
	WID_O_SELL_TRAIN,                ///< Sell this train to the scrapyard where it stands.
	WID_O_SEL_SELL_WAGONS,           ///< #NWID_SELECTION widget for the right-hand place of that row: the sell-the-dropped-wagons button, or nothing while the selected order puts no wagons down.
	WID_O_SELL_WAGONS,               ///< Sell the wagons this order puts down instead of leaving them to be collected.
};

#endif /* WIDGETS_ORDER_WIDGET_H */
