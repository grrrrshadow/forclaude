/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file true_colour.h The colours of a vehicle's set, exact -- the paint of a
 * lorry's cab, its body, its radiator -- not the nearest of the 256 of the
 * palette.
 *
 * A set paints a part of its picture by mask: eight mask indices in a row
 * over a grey picture, of which only the lightness counts. The player picks
 * the colour of each such detail in the configurator (vehicle_config.h), the
 * set gives it as red, green and blue (callback 1C1). The game makes the
 * picture again with those pixels in that colour, light where the grey is
 * light and dark where it is dark (ShadeTrueColour()), when the picture is
 * read (SetTrueColourSprite(), ReadSprite()). It is a sprite of its own then,
 * drawn by every blitter as it is, so no blitter knows of it.
 *
 * The colours one vehicle is drawn in are a set of such ranges, kept once
 * for every vehicle drawn in the same (InternTrueColourSet()); a picture in
 * one set of colours is made once (TrueColourSprite()).
 */

#ifndef TRUE_COLOUR_H
#define TRUE_COLOUR_H

#include "gfx_type.h"

/** How many mask indices in a row one colour paints. */
static constexpr uint TRUE_COLOUR_RANGE_SIZE = 8;

/**
 * One detail of the picture in one colour: the mask indices it paints, the
 * colour, and how the colour goes toward white where the grey picture is
 * lighter than the colour's own place -- the player's three numbers.
 */
struct TrueColourRange {
	uint8_t first = 0; ///< The first of the TRUE_COLOUR_RANGE_SIZE mask indices it paints.
	uint8_t r = 0; ///< Red of the colour.
	uint8_t g = 0; ///< Green of the colour.
	uint8_t b = 0; ///< Blue of the colour.
	uint8_t light_start = 128; ///< The lightness of the grey where the colour is exactly itself and starts toward white; darker grey darkens it toward black.
	uint8_t light_stop = 255; ///< The lightness of the grey where it stops toward white: lighter grey stays as light as here.
	uint8_t light_max = 128; ///< How near white it would get at the lightest grey, 255, were there no stop: 0 not at all, 255 white.

	bool operator==(const TrueColourRange &) const = default;
};

/** The colours of one vehicle, detail by detail. */
using TrueColourSet = std::vector<TrueColourRange>;

/** The lightening of a colour when the set says nothing of it: exact at 128, no stop, half way to white at 255. */
static constexpr uint32_t TRUE_COLOUR_LIGHT_DEFAULT = 128 | 255 << 8 | 128 << 16;

void SetTrueColourLight(TrueColourRange &range, uint32_t light);
Colour ShadeTrueColour(const TrueColourRange &range, uint8_t lightness);
uint8_t TrueColourLightness(const TrueColourRange &range, uint8_t m, uint8_t r, uint8_t g, uint8_t b, bool rgb);

uint16_t InternTrueColourSet(const TrueColourSet &set);
const TrueColourSet &GetTrueColourSet(uint16_t id);
uint16_t GetTrueColourSetCount();

SpriteID TrueColourSprite(SpriteID sprite, uint16_t set);
void ResetTrueColourSprites();
uint16_t GetTrueColourEpoch();
size_t GetTrueColourSpriteCount();

#endif /* TRUE_COLOUR_H */
