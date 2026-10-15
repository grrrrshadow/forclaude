/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file walker_inventory_widget.h Types related to the walker's inventory. */

#ifndef WIDGETS_WALKER_INVENTORY_WIDGET_H
#define WIDGETS_WALKER_INVENTORY_WIDGET_H

/** Widgets of the #WalkerInventoryWindow class. */
enum WalkerInventoryWidgets : WidgetID {
	WID_WI_CAPTION, ///< Caption of the window.
	WID_WI_LIST,    ///< The health, the things carried and the things for sale here.
};

#endif /* WIDGETS_WALKER_INVENTORY_WIDGET_H */
