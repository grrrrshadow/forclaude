/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file first_person_widget.h Types related to the view from the first person. */

#ifndef WIDGETS_FIRST_PERSON_WIDGET_H
#define WIDGETS_FIRST_PERSON_WIDGET_H

/** Widgets of the #FirstPersonWindow class. */
enum FirstPersonWidgets : WidgetID {
	WID_FP_CAPTION, ///< Caption of the window.
	WID_FP_CANVAS,  ///< The view from the walker's eyes.
	WID_FP_FULL,    ///< The window over the whole screen, and back.
	WID_FP_HERE,    ///< Put the walker where the main view looks.
	WID_FP_CENTRE,  ///< Put the main view on the walker.
	WID_FP_RUN_SEL, ///< Walk or run on foot, the route in a vehicle.
	WID_FP_RUN,     ///< Walk or run.
	WID_FP_ROUTE,   ///< In a vehicle: go its route, or take the wheel.
	WID_FP_LOAD,    ///< In a vehicle: load at the stop it stands on (CmdCabCargo()).
	WID_FP_UNLOAD,  ///< In a vehicle: unload there.
	WID_FP_CAMERA,  ///< In a vehicle: the next camera of its cab (CabCamera).
	WID_FP_CONTROLS_SEL, ///< In a train of MSTS: the switch of its controls, else nothing.
	WID_FP_CONTROLS, ///< In a train of MSTS: driven by the arrows or by the keys of MSTS (as the cab view's).
	WID_FP_INVENTORY, ///< The walker's inventory.
};

/** Widgets of the #DragonFeedWindow class. */
enum DragonFeedWidgets : WidgetID {
	WID_DF_CAPTION, ///< Caption of the window.
	WID_DF_LIST,    ///< What the walker carries that the dragon is fed.
};

/** Widgets of the #ForestGrillWindow class. */
enum ForestGrillWidgets : WidgetID {
	WID_FG_CAPTION, ///< Caption of the window.
	WID_FG_LIST,    ///< The animals to grill, and taking the spit off the fire.
};

#endif /* WIDGETS_FIRST_PERSON_WIDGET_H */
