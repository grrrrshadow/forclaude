/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_signals.h Signal graphics that come out of a NewGRF (feature 0E). */

#ifndef NEWGRF_SIGNALS_H
#define NEWGRF_SIGNALS_H

#include "sprite.h"
#include "strings_type.h"
#include "signal_type.h"
#include "tile_type.h"

struct GRFFile;
struct SpriteGroup;
struct RailTypeInfo;

/**
 * The variable a set reads to find out which style it is being asked to draw.
 *
 * A set picks its own number for it and says which in an 'A2VM' block; the
 * read is turned into this one where the group is parsed. Out of the way of
 * the railtype variables a signal group may also read (0x40 to 0x45) and out
 * of the range that carries a parameter byte (0x60 to 0x7F).
 */
static const uint8_t NEWGRF_SIGNAL_STYLE_VAR = 0xA0;

/**
 * One signal style a set has put up for choosing.
 *
 * A set draws several, gives each a name, and says how many aspects beyond
 * red and green it can show. Which of them is used is the player's pick; the
 * set is asked for the picture with the style's own number and the aspect,
 * and answers with a group of sprites laid out the way the base set's signals
 * are (see #GetCustomSignalStyleSprite).
 */
struct SignalStyle {
	const GRFFile *grf = nullptr; ///< The set that defined it.
	uint8_t local_id = 0; ///< Its number inside that set, which is what the set is asked with.
	StringID name = 0; ///< What to call it in the list.
	uint8_t extra_aspects = 0; ///< How many aspects it draws beyond red and green.
	uint32_t electric_enabled = 0; ///< Which signal types it draws an electric picture for.
};

/** Every style every loaded set has defined, in the order they were read. */
extern std::vector<SignalStyle> _signal_styles;

void ResetSignalStyles();
uint GetSignalStyleInUse();

SpriteID GetCustomSignalStyleSprite(uint style, const RailTypeInfo *rti, TileIndex tile, SignalType type, SignalVariant variant, SignalAspect aspect, bool gui);

#endif /* NEWGRF_SIGNALS_H */
