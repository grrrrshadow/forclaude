/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file layer_shift.h A layer of a vehicle's sprite stack moved by the set:
 * the same picture drawn a little elsewhere, by registers 0x103 and 0x104 of
 * the layer (GetCustomEngineSprite()), in sixteenths of a pixel of the normal
 * zoom. The colleague's word: a cart pushed is the cart pulled, from the
 * direction four on, and a lorry going back is the lorry going there -- the
 * same pixels with another alignment, which the file so far held twice. The
 * game makes a sprite of the picture with its offsets moved when it is read
 * (SetShiftedSprite()), once for each picture and shift (ShiftedSprite()).
 */

#ifndef LAYER_SHIFT_H
#define LAYER_SHIFT_H

#include "gfx_type.h"

SpriteID ShiftedSprite(SpriteID sprite, int16_t dx, int16_t dy);
void ResetShiftedSprites();
size_t GetShiftedSpriteCount();

#endif /* LAYER_SHIFT_H */
