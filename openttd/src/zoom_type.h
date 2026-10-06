/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file zoom_type.h Types related to zooming in and out. */

#ifndef ZOOM_TYPE_H
#define ZOOM_TYPE_H

#include "core/enum_type.hpp"

/**
 * All zoom levels we know.
 *
 * The underlying type is signed so subtract-and-Clamp works without need for casting.
 */
enum class ZoomLevel : int8_t {
	/* Our possible zoom-levels */
	Begin = 0, ///< Begin for iteration.
	Min = Begin, ///< Minimum zoom level.
	In16x = Begin, ///< Zoomed 16 times in: this build's own level past 8x, off by default and switched on in gui.zoom_min; only sets of this game's own draw it (zoom code 7, "zin16", LoadSpriteV2()), every other sprite gets it doubled from its 8x (ResizeSprites()), and only when it is switched on, since it keeps four times the sprite memory of 8x.
	In8x, ///< Zoomed 8 times in: this build's own level past the original 4x, on by default (gui.zoom_min); every sprite is given it (ResizeSprites()), from its finest level -- the mean of each 2x2 block of a 16x the set has, doubled from a coarser one -- and only sets of this game's own can draw it (zoom code 6, "zin8", LoadSpriteV2()).
	In4x, ///< Zoomed 4 times in.
	In2x, ///< Zoomed 2 times in.
	Normal, ///< The normal zoom level.
	Out2x, ///< Zoomed 2 times out.
	Out4x, ///< Zoomed 4 times out.
	Out8x, ///< Zoomed 8 times out.
	Max = Out8x, ///< Maximum zoom level.
	End, ///< End for iteration.

	/* Here we define in which zoom viewports are */
	Viewport = Normal, ///< Default zoom level for viewports.
	News = Normal, ///< Default zoom level for the news messages.
	Industry = Out2x, ///< Default zoom level for the industry view.
	Town = Normal, ///< Default zoom level for the town view.
	Aircraft = Normal, ///< Default zoom level for the aircraft view.
	Ship = Normal, ///< Default zoom level for the ship view.
	Train = Normal, ///< Default zoom level for the train view.
	RoadVehicle = Normal, ///< Default zoom level for the road vehicle view.
	WorldScreenshot = Normal, ///< Default zoom level for the world screen shot.

	Detail = Out2x, ///< All zoom levels below or equal to this will result in details on the screen, like road-work, ...
	TextEffect = Out2x, ///< All zoom levels above this will not show text effects.
};
DECLARE_INCREMENT_DECREMENT_OPERATORS(ZoomLevel)
DECLARE_ENUM_AS_SEQUENTIAL(ZoomLevel)

/** Bitset of \c ZoomLevel elements. */
using ZoomLevels = EnumBitSet<ZoomLevel, uint8_t>;

/* The viewports count in pixels of the most zoomed-in level, so with In16x
 * there are 16 of them to a pixel at normal zoom (the original had 4, the
 * first build of this game's own 8). A savegame or a config from before a
 * level is brought up to it (SaveLoadVersion::ZoomIn8x, ZoomIn16x,
 * IFV_ZOOM_IN_8X, IFV_ZOOM_IN_16X). */
static const uint ZOOM_BASE_SHIFT = to_underlying(ZoomLevel::Normal);
static uint const ZOOM_BASE = 1U << ZOOM_BASE_SHIFT;

extern int _gui_scale;
extern int _gui_scale_cfg;

extern ZoomLevel _gui_zoom;
extern ZoomLevel _font_zoom;

static const int MIN_INTERFACE_SCALE = 100;
static const int MAX_INTERFACE_SCALE = 500;

#endif /* ZOOM_TYPE_H */
