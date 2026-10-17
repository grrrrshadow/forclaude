/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cargo_cutout.h A load cut out of one texture by a stencil, the
 * player's word: one picture of marijuana for every body, and for each
 * vehicle, direction and load only the outline, which takes next to nothing
 * in the file. A set draws the load as two layers of its sprite stack, the
 * stencil with bit 30 of register 100 and the texture right after it
 * (GetCustomEngineSprite()); the game makes one sprite of the two when it is
 * read (SetCutoutSprite(), CutOutTexture()) and draws that, with the
 * colours of the set over it (true_colour.h) like any load. Made once for
 * each stencil, texture and start (CutoutSprite()).
 */

#ifndef CARGO_CUTOUT_H
#define CARGO_CUTOUT_H

#include "gfx_type.h"

SpriteID CutoutSprite(SpriteID stencil, SpriteID texture, int16_t x, int16_t y);
void ResetCutoutSprites();
size_t GetCutoutSpriteCount();

#endif /* CARGO_CUTOUT_H */
