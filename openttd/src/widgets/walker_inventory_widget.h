/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file walker_inventory_widget.h Types related to the walker's inventory. */

#ifndef WIDGETS_WALKER_INVENTORY_WIDGET_H
#define WIDGETS_WALKER_INVENTORY_WIDGET_H

/** Widgets of the #WalkerInventoryWindow class. */
enum WalkerInventoryWidgets : WidgetID {
	WID_WI_CAPTION, ///< Caption of the window.
	WID_WI_LIST,    ///< The health, the things carried and the things for sale here.
	WID_WI_SCROLLBAR, ///< Scrollbar of the list.
};

#endif /* WIDGETS_WALKER_INVENTORY_WIDGET_H */
