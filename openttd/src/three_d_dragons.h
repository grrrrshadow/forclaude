/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_dragons.h The dragons: now and then they fly in anywhere on the map, and fight the walker.
 *
 * The player (15. 10.): "the dragons sometimes fly in, everywhere", "they attack the player", "nothing for killing a
 * dragon, I like dragons". The colleague's dragons (grrrrf hra/3d/PROSBA-DRACI.md, objekty/tvorove/) with their
 * animations as frames: the brown and the black one land, walk, rest, go for the walker of the view from the first
 * person and strike; the hammer fells them. The ice and the Chinese dragon fly over and now and then swoop down on
 * the walker. The walking one wanders about.
 *
 * Only the screen, as the walkers of the towns (three_d_walkers.h): the client's, never saved, never touching the
 * game's state or its random numbers -- but the walker's health, which the view from the first person takes through
 * a command (TakeThreeDDragonDamage(), CmdHurtWalker()).
 */

#ifndef THREE_D_DRAGONS_H
#define THREE_D_DRAGONS_H

#include "zoom_type.h"

#include <string>

void AddThreeDDragonsToViewport(ZoomLevel zoom);
void AddThreeDDragonsToEye();
bool HitThreeDDragon(double x, double y, double heading, double reach_m, int damage = 1);
bool HitThreeDDragonAt(double x, double y, double z, int damage);
uint TakeThreeDDragonDamage();
bool SendThreeDDragon(std::string_view kind, double x, double y);
std::string DescribeThreeDDragons();

#endif /* THREE_D_DRAGONS_H */
