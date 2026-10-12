/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file viewport_sprite_sorter.h Types related to sprite sorting. */

#include "gfx_type.h"

#ifndef VIEWPORT_SPRITE_SORTER_H
#define VIEWPORT_SPRITE_SORTER_H

/** Parent sprite that should be drawn */
struct ParentSpriteToDraw {
	/* Block of 16B loadable in xmm register */
	int32_t xmin;                     ///< minimal world X coordinate of bounding box
	int32_t ymin;                     ///< minimal world Y coordinate of bounding box
	int32_t zmin;                     ///< minimal world Z coordinate of bounding box
	int32_t x;                        ///< screen X coordinate of sprite

	/* Second block of 16B loadable in xmm register */
	int32_t xmax;                     ///< maximal world X coordinate of bounding box
	int32_t ymax;                     ///< maximal world Y coordinate of bounding box
	int32_t zmax;                     ///< maximal world Z coordinate of bounding box
	int32_t y;                        ///< screen Y coordinate of sprite

	SpriteID image;                 ///< sprite to draw
	PaletteID pal;                  ///< palette to use
	const SubSprite *sub;           ///< only draw a rectangular part of the sprite

	int32_t left;                     ///< minimal screen X coordinate of sprite (= x + sprite->x_offs), reference point for child sprites
	int32_t top;                      ///< minimal screen Y coordinate of sprite (= y + sprite->y_offs), reference point for child sprites

	int32_t first_child;              ///< the first child to draw.
	uint32_t order;                   ///< Used during sprite sorting

	int32_t ax;                       ///< world X of the point the sprite hangs from (x, y are RemapCoords() of it plus the pixel offset); the turned view draws it there turned (three_d.h)
	int32_t ay;                       ///< world Y of that point
	int32_t az;                       ///< world Z of that point
	const struct ThreeDHouse *box = nullptr; ///< the turned view draws this sprite as these boxes instead of a billboard (a house with a hand table)
	int32_t box_ox;                   ///< world X of the north corner of the house's north tile, the origin of the boxes
	int32_t box_oy;                   ///< world Y of it
	int32_t box_oz;                   ///< world Z of the ground the house stands on
	int32_t box_dx;                   ///< this tile's place in the house, tiles along X from its north tile
	int32_t box_dy;                   ///< and along Y (both wider than they need be: the SSE4 sorter wants the struct a multiple of 16 bytes)
};

typedef std::vector<ParentSpriteToDraw*> ParentSpriteToSortVector;

/** Type for method for checking whether a viewport sprite sorter exists. */
typedef bool (*VpSorterChecker)();
/** Type for the actual viewport sprite sorter. */
typedef void (*VpSpriteSorter)(ParentSpriteToSortVector *psd);

#ifdef WITH_SSE
bool ViewportSortParentSpritesSSE41Checker();
void ViewportSortParentSpritesSSE41(ParentSpriteToSortVector *psdv);
#endif

void InitializeSpriteSorter();

#endif /* VIEWPORT_SPRITE_SORTER_H */
