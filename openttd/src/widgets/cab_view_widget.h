/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
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
	WID_CV_BRAKE,   ///< Hold the vehicle to a lower speed.
	WID_CV_THROTTLE, ///< Let it go faster again.
	WID_CV_FULL,    ///< The window over the whole screen, and back.
	WID_CV_LEFT,    ///< Steer to the left (a ship).
	WID_CV_AUTO,    ///< Go as the orders say again.
	WID_CV_RIGHT,   ///< Steer to the right (a ship).
};

#endif /* WIDGETS_CAB_VIEW_WIDGET_H */
