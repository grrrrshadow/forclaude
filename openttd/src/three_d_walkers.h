/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_walkers.h People walking about the towns in the turned view (three_d.h).
 *
 * The player (15. 10.): "Mat will walk about the town along the edge of the road tiles, there is the pavement in a
 * town, and out of the town he will not walk; only from a door, and he disappears elsewhere at a door he gets to".
 * Mat the repairman is the colleague's figure (grrrrf hra/3d/PROSBA-POSTAVY-ANIMACE.md, objekty/postavy/mat.glb),
 * walking by frames; the doors are those of the houses as 3D objects ("dvere" of their index).
 *
 * Only the screen: the walkers are the client's, made and moved by the ticks of the game while the main view is
 * turned, never saved, never touching the game's state or its random numbers.
 */

#ifndef THREE_D_WALKERS_H
#define THREE_D_WALKERS_H

#include "tile_type.h"
#include "zoom_type.h"

#include <functional>
#include <string>

TileIndex ThreeDLifeMiddle();
void AddThreeDWalkersToViewport(ZoomLevel zoom);
void AddThreeDWalkersToEye(float scale);
std::string DescribeThreeDWalkers();
bool SendThreeDWalker();
struct Town;
bool IsThreeDWalkway(TileIndex t, const Town *town);
bool ThreeDWalkerAt(int &x, int &y, int &z);
bool ThreeDMatNear(double x, double y, double reach_m);
void AddThreeDDriversToViewport(ZoomLevel zoom);
void AddThreeDDriversToEye();
struct ThreeDPictureLight;
void ForEachThreeDDriverLight(ZoomLevel zoom, const std::function<void(int x, int y, int z, const ThreeDPictureLight &light)> &fn);
std::string DescribeThreeDDrivers();

#endif /* THREE_D_WALKERS_H */
