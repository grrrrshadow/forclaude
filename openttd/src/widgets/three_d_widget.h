/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_widget.h Types related to the widgets of the 3D previews (three_d_preview_gui.cpp). */

#ifndef WIDGETS_THREE_D_WIDGET_H
#define WIDGETS_THREE_D_WIDGET_H

/** Widgets of the preview that turns, beside the purchase window and beside the industry window (#ThreeDPreviewWindow). */
enum ThreeDPreviewWidgets : WidgetID {
	WID_TDP_CAPTION, ///< Caption: the engine or the industry.
	WID_TDP_PICTURE, ///< The picture, one of the eight directions.
	WID_TDP_LEFT,    ///< Turn to the left.
	WID_TDP_TURN,    ///< Let it turn by itself.
	WID_TDP_RIGHT,   ///< Turn to the right.
};

#endif /* WIDGETS_THREE_D_WIDGET_H */
