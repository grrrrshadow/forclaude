/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_remap_type.h What a set asked to have called by another name. */

#ifndef NEWGRF_REMAP_TYPE_H
#define NEWGRF_REMAP_TYPE_H

/**
 * One Variational Action 2 variable a set asked for by name.
 *
 * JGR's patchpack gave its added variables no fixed numbers. A set names the
 * one it wants in an Action 14 'A2VM' block and gives a variable number with
 * a shift and a mask that together are the key; when a group of that feature
 * reads exactly that, the read is turned into the one this game answers, with
 * the shift and mask the set asked to have applied instead (see the adjust
 * parsing in newgrf_act2.cpp).
 *
 * It is read while a file is only being scanned, before there is a GRFFile to
 * put it on, so it lives on the GRFConfig first and is taken over from there.
 */
struct GRFVariableRemap {
	uint8_t feature = 0; ///< Which feature's groups this is for.
	uint8_t from = 0; ///< The variable number the set reads.
	uint8_t key_shift = 0; ///< The shift that read carries, as the rest of the key.
	uint32_t key_mask = 0; ///< The mask that read carries, likewise.
	uint8_t to = 0; ///< The variable this game answers instead.
	uint8_t shift = 0; ///< The shift to apply to the answer.
	uint32_t mask = 0; ///< The mask to apply to it.
};

#endif /* NEWGRF_REMAP_TYPE_H */
