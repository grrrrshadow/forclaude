/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargo_cutout.cpp The loads cut out of textures: which have been made, and making them. */

#include "stdafx.h"
#include "cargo_cutout.h"
#include "spritecache.h"
#include "debug.h"

#include <map>

#include "safeguards.h"

/** The loads cut out so far: the stencil, the texture and the start, to the sprite made. */
static std::map<std::tuple<SpriteID, SpriteID, int16_t, int16_t>, SpriteID> _cutout_sprites;

/** How many loads there may be cut out at once; past it a stencil is drawn as it is. */
static constexpr size_t CUTOUT_SPRITE_LIMIT = 1 << 16;

/**
 * A load cut out of a texture by a stencil, from a start in the texture: the
 * same sprite each time the three are the same (SetCutoutSprite()).
 * @param stencil the picture of the load's shape and shading
 * @param texture the picture of the cargo
 * @param x where in the texture the cut starts, in pixels of the normal zoom
 * @param y and down
 * @return the load; the stencil itself when it cannot be made
 */
SpriteID CutoutSprite(SpriteID stencil, SpriteID texture, int16_t x, int16_t y)
{
	const auto key = std::tuple{stencil, texture, x, y};
	auto it = _cutout_sprites.find(key);
	if (it != _cutout_sprites.end()) return it->second;
	if (_cutout_sprites.size() >= CUTOUT_SPRITE_LIMIT) return stencil;

	SpriteID made = AllocateDerivedSpriteID();
	if (made == 0 || !SetCutoutSprite(made, stencil, texture, x, y)) return stencil;
	_cutout_sprites.emplace(key, made);
	return made;
}

/** Forget the loads cut out: the sprites they were cut from are read anew (GfxInitSpriteMem()). */
void ResetCutoutSprites()
{
	if (!_cutout_sprites.empty()) Debug(sprite, 3, "Forgetting {} loads cut out of textures", _cutout_sprites.size());
	_cutout_sprites.clear();
}

/**
 * How many loads are cut out, for the rig.
 * @return the count
 */
size_t GetCutoutSpriteCount()
{
	return _cutout_sprites.size();
}
