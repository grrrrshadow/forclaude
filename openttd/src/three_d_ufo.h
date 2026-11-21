/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_ufo.h The flying saucer (the colleague's hra/3d/PROSBA-UFO.md; the player, 24. 10.: "the saucer for the
 * disasters in 3D, and at night it may fly as the ghosts and not wreck the rails, a beam of light shining down"): the
 * small and the big UFO of the disasters drawn as it, and one flying over the towns at night, breaking nothing.
 */

#ifndef THREE_D_UFO_H
#define THREE_D_UFO_H

#include "zoom_type.h"
#include "three_d_witches.h"

#include <string>
#include <vector>

struct Vehicle;

bool ThreeDUfoDrawsDisaster(const Vehicle *v);
void AddThreeDUfosToViewport(ZoomLevel zoom);
void AddThreeDUfosToEye();
void AddThreeDUfoLights(bool eye, std::vector<ThreeDWitchLight> &out);
bool SendThreeDUfo(double x, double y, double heading);
bool MakeThreeDUfoDo(std::string_view what);
std::string DescribeThreeDUfos();

#endif /* THREE_D_UFO_H */
