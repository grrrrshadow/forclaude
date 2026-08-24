/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file copypaste_widget.h Types related to the copy/paste widgets. */

#ifndef WIDGETS_COPYPASTE_WIDGET_H
#define WIDGETS_COPYPASTE_WIDGET_H

/** Widgets of the #CopyPasteWindow class. */
enum CopyPasteWidgets : WidgetID {
	WID_CP_CAPTION, ///< Caption of the window.
	WID_CP_COPY,    ///< Mark out an area and remember what is built on it.
	WID_CP_PASTE,   ///< Build what was remembered somewhere else.
	WID_CP_CLEAR,   ///< Forget what was remembered.
	WID_CP_INFO,    ///< What is currently remembered.
};

#endif /* WIDGETS_COPYPASTE_WIDGET_H */
