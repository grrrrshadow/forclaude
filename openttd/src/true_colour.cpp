/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file true_colour.cpp The colours of a vehicle's set, exact: shading a colour, the sets of colours, the pictures made in them. */

#include "stdafx.h"
#include "true_colour.h"
#include "spritecache.h"
#include "debug.h"
#include "core/math_func.hpp"

#include "table/sprites.h"

#include <unordered_map>

#include "safeguards.h"

/** Every set of colours some vehicle has been drawn in; set n is at n - 1. */
static std::vector<TrueColourSet> _true_colour_sets;
/** The pictures made in a set of colours: the picture and the set, to the picture made. */
static std::unordered_map<uint64_t, SpriteID> _true_colour_sprites;
/** Where the next picture made goes; 0 until the first, which goes after the last sprite read. */
static SpriteID _true_colour_next = 0;
/** Changes whenever the sets are read anew, so every vehicle asks its colours again (GetVehicleTrueColours()). */
static uint16_t _true_colour_epoch = 1;

/** How many pictures in colours there may be at once, about five megabytes of the sprite cache's entries; past it a picture is drawn as it is. */
static constexpr size_t TRUE_COLOUR_SPRITE_LIMIT = 1 << 16;

/**
 * Give a colour the player's three numbers of its lightening, as a set
 * gives them in one register: where the colour starts toward white, where it
 * stops, how near white it would get at the lightest grey were there no
 * stop. Nought is the set saying nothing.
 * @param range the colour
 * @param light the start in bits 0-7, the stop in 8-15, the most in 16-23; or 0
 */
void SetTrueColourLight(TrueColourRange &range, uint32_t light)
{
	if (light == 0) light = TRUE_COLOUR_LIGHT_DEFAULT;
	range.light_start = GB(light, 0, 8);
	range.light_stop = GB(light, 8, 8);
	range.light_max = GB(light, 16, 8);
}

/**
 * The colour of one pixel of a detail: the colour itself where the grey
 * picture is as light as the colour's place (light_start), toward black as
 * the grey darkens, toward white as it lightens -- steadily, so that at the
 * lightest grey, 255, it would be light_max of the way to white, but no
 * further than it is at light_stop: lighter grey than that stays as light as
 * at light_stop. The player: "the first number where it starts lightening,
 * the second where it stops, the third how much at most it lightens, were
 * there no end to the lightening".
 * @param range the colour
 * @param lightness the lightness of the grey picture there, 0-255
 * @return the pixel's colour
 */
Colour ShadeTrueColour(const TrueColourRange &range, uint8_t lightness)
{
	const uint start = range.light_start;
	if (lightness <= start) {
		if (start == 0) return Colour(range.r, range.g, range.b);
		auto dark = [&](uint c) { return static_cast<uint8_t>((c * lightness + start / 2) / start); };
		return Colour(dark(range.r), dark(range.g), dark(range.b));
	}
	/* Toward white: (lightness - start) / (255 - start) of light_max / 255,
	 * the lightness taken no higher than light_stop. */
	const uint top = std::min<uint>(lightness, range.light_stop);
	if (top <= start) return Colour(range.r, range.g, range.b);
	const uint num = (top - start) * range.light_max;
	const uint den = (255 - start) * 255;
	auto light = [&](uint c) { return static_cast<uint8_t>(c + ((255 - c) * num + den / 2) / den); };
	return Colour(light(range.r), light(range.g), light(range.b));
}

/**
 * The lightness of a pixel of a detail: in a 32bpp picture the lightest of
 * its red, green and blue, as the game takes it for the company colours, and
 * black is black. A pixel of an 8bpp picture has none and is lit by its place
 * in the eight indices instead, from dark to light, the fourth the colour
 * itself.
 * @param range the detail
 * @param m the pixel's mask index, one of the detail's
 * @param r red of the pixel
 * @param g green of the pixel
 * @param b blue of the pixel
 * @param rgb whether the picture was read in 32bpp
 * @return the lightness, 0-255
 */
uint8_t TrueColourLightness(const TrueColourRange &range, uint8_t m, uint8_t r, uint8_t g, uint8_t b, bool rgb)
{
	if (rgb) return std::max({r, g, b});
	return static_cast<uint8_t>(std::min<uint>(255, 32 * (m - range.first + 1)));
}

/**
 * The number of a set of colours, the same for every vehicle drawn in the
 * same colours.
 * @param set the colours
 * @return its number, from 1; 0 for no colours, or when there are too many sets
 */
uint16_t InternTrueColourSet(const TrueColourSet &set)
{
	if (set.empty()) return 0;
	auto it = std::ranges::find(_true_colour_sets, set);
	if (it != _true_colour_sets.end()) return static_cast<uint16_t>(it - _true_colour_sets.begin() + 1);
	if (_true_colour_sets.size() >= UINT16_MAX) return 0;
	_true_colour_sets.push_back(set);
	return static_cast<uint16_t>(_true_colour_sets.size());
}

/**
 * A set of colours by its number.
 * @param id its number, from InternTrueColourSet()
 * @return the colours
 */
const TrueColourSet &GetTrueColourSet(uint16_t id)
{
	assert(id != 0 && id <= _true_colour_sets.size());
	return _true_colour_sets[id - 1];
}

/**
 * How many sets of colours there are, for the rig.
 * @return the count
 */
uint16_t GetTrueColourSetCount()
{
	return static_cast<uint16_t>(_true_colour_sets.size());
}

/**
 * A picture in a set of colours: the same picture over again, made when it is
 * read with the pixels of each detail in its colour (SetTrueColourSprite()).
 * The first time a picture is wanted in a set it gets a sprite of its own,
 * after the last sprite the game and its sets have read.
 * @param sprite the picture as the set draws it
 * @param set the colours (InternTrueColourSet())
 * @return the picture in the colours; the picture itself for no colours, or when it cannot be made
 */
SpriteID TrueColourSprite(SpriteID sprite, uint16_t set)
{
	if (set == 0) return sprite;
	const uint64_t key = static_cast<uint64_t>(sprite) << 16 | set;
	auto it = _true_colour_sprites.find(key);
	if (it != _true_colour_sprites.end()) return it->second;
	if (_true_colour_sprites.size() >= TRUE_COLOUR_SPRITE_LIMIT) return sprite;

	if (_true_colour_next == 0) _true_colour_next = GetMaxSpriteID();
	if (_true_colour_next >= MAX_SPRITES) return sprite;
	if (!SetTrueColourSprite(_true_colour_next, sprite, set)) return sprite;
	SpriteID made = _true_colour_next++;
	_true_colour_sprites.emplace(key, made);
	return made;
}

/**
 * Forget the pictures made in colours: the sprites are read anew, and with
 * them the sets that give the colours, so every vehicle asks its colours
 * again. Called whenever the sprite memory is set up (GfxInitSpriteMem()).
 */
void ResetTrueColourSprites()
{
	if (!_true_colour_sprites.empty()) Debug(sprite, 3, "Forgetting {} pictures in colours", _true_colour_sprites.size());
	_true_colour_sprites.clear();
	_true_colour_next = 0;
	if (++_true_colour_epoch == 0) _true_colour_epoch = 1;
}

/**
 * The number that changes whenever the sets are read anew; a vehicle's colours
 * are good while it has the same (Vehicle::true_colours_epoch).
 * @return the number, never 0
 */
uint16_t GetTrueColourEpoch()
{
	return _true_colour_epoch;
}

/**
 * How many pictures in colours there are, for the rig.
 * @return the count
 */
size_t GetTrueColourSpriteCount()
{
	return _true_colour_sprites.size();
}
