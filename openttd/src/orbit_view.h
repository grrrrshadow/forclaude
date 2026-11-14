/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file orbit_view.h The camera going round a place over the map: the second axis of turning the map. See orbit_view.cpp. */

#ifndef ORBIT_VIEW_H
#define ORBIT_VIEW_H

#include "core/geometry_type.hpp"

#include <string>

struct Window;

bool OrbitViewOn();
bool OrbitTipByDrag(int dx, int dy);
void OrbitDragEnd();
bool OrbitDoubleClick();
void OrbitViewWheel(int wheel);
void OrbitViewTick(Window &main);
bool DrawOrbitViewInto(const Window &main, const Rect &r);
void OrbitViewSet(bool on, double tip_deg, double distance_m);
std::string DescribeOrbitView();

#endif /* ORBIT_VIEW_H */
