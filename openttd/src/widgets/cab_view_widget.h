/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file cab_view_widget.h Types related to the cab view widgets (cab_view.h). */

#ifndef WIDGETS_CAB_VIEW_WIDGET_H
#define WIDGETS_CAB_VIEW_WIDGET_H

/** Widgets of the #CabViewWindow class. */
enum CabViewWidgets : WidgetID {
	WID_CV_CAPTION, ///< Caption of the window: the vehicle's name.
	WID_CV_CANVAS,  ///< The view ahead.
	WID_CV_PANEL,   ///< The panel under it: the speed, the next stop, the load.
	WID_CV_VEHICLE, ///< Open the vehicle's window.
	WID_CV_CENTRE,  ///< Put the main view on the vehicle.
	WID_CV_STOP,    ///< Stop the vehicle, or start it again.
	WID_CV_FULL,    ///< The window over the whole screen, and back.
	WID_CV_AUTO,    ///< Go as the orders say again, or take the wheel.
	WID_CV_LOAD,    ///< Load at the stop it stands on (CmdCabCargo()).
	WID_CV_UNLOAD,  ///< Unload there.
	WID_CV_CAMERA,  ///< The next camera: the interior, without it, over the roof (CabCamera).
};

#endif /* WIDGETS_CAB_VIEW_WIDGET_H */
