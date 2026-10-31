/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file three_d_animals.h The dogs, the animals, the ghosts and the sellers of dogs about the walker (ThreeDAnimal). */

#ifndef THREE_D_ANIMALS_H
#define THREE_D_ANIMALS_H

#include "zoom_type.h"

#include <string>
#include <vector>

void AddThreeDAnimalsToViewport(ZoomLevel zoom);
void AddThreeDAnimalsToEye();
bool HitThreeDAnimal(double x, double y, double heading, double reach_m, int damage);
bool HitThreeDAnimalAt(double x, double y, double z, int damage);
int BlastThreeDAnimals(double x, double y, double z, double radius_m, int damage);
uint TakeThreeDAnimalDamage();
bool SendThreeDAnimal(std::string_view name, double x, double y);
std::string DescribeThreeDAnimals();
bool ThreeDAnimalOfPlace(std::string_view place, double &x, double &y);

/** A seller of dogs near the walker: what it sells (the names of ThreeDAnimal), where it stands. */
struct ThreeDDogSeller {
	std::string name;                ///< the seller, a ThreeDAnimal
	std::vector<std::string> offer;  ///< the dogs it sells, three to five of those sold
	double x = 0, y = 0;             ///< world units
};
const ThreeDDogSeller *ThreeDDogSellerNear(double x, double y, double reach_m);
/** How near a seller of dogs the walker sells from it (the inventory) and is told so (the line of the first person view), metres. */
static constexpr double THREE_D_SELLER_REACH_M = 6.0;
bool ThreeDNurseNear(double x, double y, double reach_m);
bool ThreeDNurseHealsWalker();

struct ThreeDMushroom;
void AddThreeDMushroomsToEye();
const ThreeDMushroom *ThreeDMushroomNear(double x, double y, double reach_m);
bool PickThreeDMushroom(double x, double y, double reach_m);
bool ThreeDMushroomPlace(double x, double y, int tiles, double &mx, double &my, std::string &name);

#endif /* THREE_D_ANIMALS_H */
