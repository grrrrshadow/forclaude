/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tilehighlight_func.h Functions related to tile highlights. */

#ifndef TILEHIGHLIGHT_FUNC_H
#define TILEHIGHLIGHT_FUNC_H

#include "gfx_type.h"
#include "tilehighlight_type.h"

void PlaceProc_DemolishArea(TileIndex tile);
bool GUIPlaceProcDragXY(ViewportDragDropSelectionProcess proc, TileIndex start_tile, TileIndex end_tile);

bool HandlePlacePushButton(Window *w, WidgetID widget, CursorID cursor, HighLightStyle mode);
void SetObjectToPlaceWnd(CursorID icon, PaletteID pal, HighLightStyle mode, Window *w);
void SetObjectToPlace(CursorID icon, PaletteID pal, HighLightStyle mode, WindowClass window_class, WindowNumber window_num);
void ResetObjectToPlace();

void VpSelectTilesWithMethod(int x, int y, ViewportPlaceMethod method);
void VpStartDragging(ViewportDragDropSelectionProcess process);
void VpStartPlaceSizing(TileIndex tile, ViewportPlaceMethod method, ViewportDragDropSelectionProcess process);
void VpSetPresizeRange(TileIndex from, TileIndex to);
void VpSetPlaceSizingLimit(int limit);

void UpdateTileSelection();

extern TileHighlightData _thd;

/**
 * The tile marker as it stood on the frame before this one -- which is to say,
 * as the player last saw it on screen.
 *
 * A click has to be answered with what the player was looking at when they
 * pressed, not with where the marker has since moved to. Both are worked out
 * from the pointer, and the pointer can move between the frame that was drawn
 * and the frame that handles the press: on a touch screen it reliably does,
 * because the finger settles as it comes down. #UpdateTileSelection() runs
 * before clicks are dispatched, so by the time a build command is put
 * together, #_thd already describes a marker the player has never seen. This
 * is the one they did see. See #PlaceObject().
 */
extern TileHighlightData _thd_drawn;

#endif /* TILEHIGHLIGHT_FUNC_H */
