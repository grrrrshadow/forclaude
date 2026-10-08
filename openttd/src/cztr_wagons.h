/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cztr_wagons.h Fetching CZTR Wagons-Cargo 1.0.0 from the content server for a game that plays FIRS 5 with another release of the set. */

#ifndef CZTR_WAGONS_H
#define CZTR_WAGONS_H

void FetchCztrWagonsForFirs5IfMissing();
bool IsFetchingCztrWagonsForFirs5();

#endif /* CZTR_WAGONS_H */
