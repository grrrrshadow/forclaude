/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file mars_houses.h The Mars houses: a house set the game fetches itself, puts into every new game and builds its Mars towns of. */

#ifndef MARS_HOUSES_H
#define MARS_HOUSES_H

/**
 * "(Fixed) OpenGFX Mars Houses" of BaNaNaS, the set the Mars towns are built
 * of, as the game keeps a GRF id: its bytes are 52 4A 45 0B, which the game
 * prints as 524A450B. It is the Mars house set BaNaNaS offers for new games.
 *
 * The game treats it as its own rather than as one of the player's sets:
 * - it fetches it when it is missing (FetchMarsHousesIfMissing()),
 * - it puts it into every new game (ResetGRFConfig()), so that the town window
 *   always offers it,
 * - its houses go up only in towns told to build from it -- a Mars town, or
 *   one the player ticked it for -- and never in a town of every house, so
 *   that a game with the Mars towns off is the game as it always was
 *   (TryBuildTownHouse()).
 */
static constexpr uint32_t MARS_HOUSES_GRFID = 0x0B454A52;

bool HaveMarsHouses();
void FetchMarsHousesIfMissing();

#endif /* MARS_HOUSES_H */
