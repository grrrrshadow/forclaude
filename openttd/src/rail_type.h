/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_type.h The different types of rail. */

#ifndef RAIL_TYPE_H
#define RAIL_TYPE_H

#include "core/enum_type.hpp"

typedef uint32_t RailTypeLabel;

static const RailTypeLabel RAILTYPE_LABEL_RAIL     = 'RAIL';
static const RailTypeLabel RAILTYPE_LABEL_ELECTRIC = 'ELRL';
static const RailTypeLabel RAILTYPE_LABEL_MONO     = 'MONO';
static const RailTypeLabel RAILTYPE_LABEL_MAGLEV   = 'MGLV';
/** The game's own power line (the player, 14. 11.: pylons with cables for the cargo electricity): a rail type, drawn in 3D always (DrawThreeDPowerLine() of rail_cmd.cpp). */
static const RailTypeLabel RAILTYPE_LABEL_POWER_LINE = 'DVED';
/** The game's own pipeline (the player, 14. 11.: pipes for the cargoes the tankers carry): a rail type, drawn in 3D always, a pipe on saddles in place of the track (DrawThreeDTrack() of rail_cmd.cpp). */
static const RailTypeLabel RAILTYPE_LABEL_PIPE = 'DPOT';

/**
 * Enumeration for all possible railtypes.
 */
enum RailType : uint8_t {
	RAILTYPE_BEGIN    = 0,          ///< Used for iterations
	RAILTYPE_RAIL     = 0,          ///< Standard non-electric rails
	RAILTYPE_ELECTRIC = 1,          ///< Electric rails
	RAILTYPE_MONO     = 2,          ///< Monorail
	RAILTYPE_MAGLEV   = 3,          ///< Maglev
	RAILTYPE_POWER_LINE = 4,        ///< The game's own power line (RAILTYPE_LABEL_POWER_LINE): allocated right after the four (ResetRailTypes())
	RAILTYPE_PIPE     = 5,          ///< The game's own pipeline (RAILTYPE_LABEL_PIPE): after it
	RAILTYPE_END      = 64,         ///< Used for iterations
	INVALID_RAILTYPE  = 0xFF,       ///< Flag for invalid railtype
};

/** Bitset of \c RailType elements. */
using RailTypes = EnumBitSet<RailType, uint64_t>;

static constexpr RailTypes INVALID_RAILTYPES{UINT64_MAX};

#endif /* RAIL_TYPE_H */
