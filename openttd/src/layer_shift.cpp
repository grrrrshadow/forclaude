/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file layer_shift.cpp The layers moved by their sets: which have been made, and making them. */

#include "stdafx.h"
#include "layer_shift.h"
#include "spritecache.h"
#include "debug.h"

#include <map>

#include "safeguards.h"

/** The pictures moved so far: the picture and the shift, to the sprite made. */
static std::map<std::tuple<SpriteID, int16_t, int16_t>, SpriteID> _shifted_sprites;

/** How many pictures there may be moved at once; past it a picture is drawn where it is. */
static constexpr size_t SHIFTED_SPRITE_LIMIT = 1 << 16;

/**
 * A picture moved by a shift: the same sprite each time the picture and the
 * shift are the same (SetShiftedSprite()).
 * @param sprite the picture
 * @param dx how far to the right, in sixteenths of a pixel of the normal zoom
 * @param dy and down
 * @return the picture moved; the picture itself when there is no shift or it cannot be made
 */
SpriteID ShiftedSprite(SpriteID sprite, int16_t dx, int16_t dy)
{
	if (dx == 0 && dy == 0) return sprite;
	const auto key = std::tuple{sprite, dx, dy};
	auto it = _shifted_sprites.find(key);
	if (it != _shifted_sprites.end()) return it->second;
	if (_shifted_sprites.size() >= SHIFTED_SPRITE_LIMIT) return sprite;

	SpriteID made = AllocateDerivedSpriteID();
	if (made == 0 || !SetShiftedSprite(made, sprite, dx, dy)) return sprite;
	_shifted_sprites.emplace(key, made);
	return made;
}

/** Forget the pictures moved: the sprites they were made from are read anew (GfxInitSpriteMem()). */
void ResetShiftedSprites()
{
	if (!_shifted_sprites.empty()) Debug(sprite, 3, "Forgetting {} pictures moved by their sets", _shifted_sprites.size());
	_shifted_sprites.clear();
}

/**
 * How many pictures are moved, for the rig.
 * @return the count
 */
size_t GetShiftedSpriteCount()
{
	return _shifted_sprites.size();
}
