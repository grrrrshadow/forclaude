/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file cztr_wagons.h Fetching CZTR Wagons-Cargo 1.0.0 from the content server for a game that plays FIRS 5 with another release of the set. */

#ifndef CZTR_WAGONS_H
#define CZTR_WAGONS_H

void FetchCztrWagonsForFirs5IfMissing();
bool IsFetchingCztrWagonsForFirs5();

#endif /* CZTR_WAGONS_H */
