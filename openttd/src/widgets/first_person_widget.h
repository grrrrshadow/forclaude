/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
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
	WID_FP_RUN,     ///< Walk or run.
	WID_FP_INVENTORY, ///< The walker's inventory.
};

/** Widgets of the #DragonFeedWindow class. */
enum DragonFeedWidgets : WidgetID {
	WID_DF_CAPTION, ///< Caption of the window.
	WID_DF_LIST,    ///< What the walker carries that the dragon is fed.
};

#endif /* WIDGETS_FIRST_PERSON_WIDGET_H */
