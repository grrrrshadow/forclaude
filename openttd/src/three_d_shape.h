/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_shape.h The shape of a building read off its one sprite by the game itself: a box with a roof that
 * covers the sprite's pixels in the game's own view as well as it can (three_d_shape.cpp). The turned view draws a
 * building without a hand table so (box_kind 13), instead of the box the game sorts the sprite by, which is often much
 * bigger than the building and has no roof. The sprite's pixels are read on the main thread, the shape is found by the
 * thread in the background; till then the building is drawn as before and its place is drawn again when it comes.
 */

#ifndef THREE_D_SHAPE_H
#define THREE_D_SHAPE_H

#include "three_d.h"
#include "gfx_type.h"

#include <atomic>
#include <span>
#include <string>
#include <vector>

/** A building's shape read off its sprite: its boxes, in world units from the north corner of the sprite's bounding box, heights up from its bottom. */
struct ThreeDSpriteShape {
	std::vector<ThreeDPart> parts; ///< the boxes, as ThreeDPart (pixels up are z units)
	double centre_x = 0;           ///< the middle of the footprint along X, what the front snaps about
	double centre_y = 0;           ///< and along Y
	double period = 90;            ///< after how many degrees the front comes round again: 90 for a square footprint, 180 for a longer one
	bool ok = false;               ///< whether it is drawn: it fits the sprite better than the bounding box does
	std::atomic<bool> ready = false; ///< whether it is read: the thread in the background reads it (ThreeDLoadInBackground()), the box as before is drawn meanwhile

	/* What it was read from, and how well it fits: for the rig (test3d tvary). */
	SpriteID image = 0;
	int origin_x = 0, origin_y = 0; ///< where the sprite hangs, screen units (ZoomLevel::Min) from the bounding box's north corner
	int ex = 0, ey = 0, ez = 0;     ///< the bounding box's extent
	float fit = 0;                  ///< how well the shape covers the sprite in the game's view: intersection over union (a pixel left outside twice), 0..1
	float box_fit = 0;              ///< the same of the box drawn before (the bounding box narrowed to the sprite's width)
	float flat_fit = 0;             ///< the same of the best box with a flat top
	float kept = 0;                 ///< the share of the sprite's pixels inside the shape: drawn on its faces (the rest is not drawn)
	float box_kept = 0;             ///< the same of the box drawn before
	float ms = 0;                   ///< how long finding it took (the thread in the background)
	float read_ms = 0;              ///< how long reading the sprite's pixels took (the main thread)
	int tried = 0;                  ///< how many shapes were tried
};

extern bool _three_d_sprite_shapes_on;

const ThreeDSpriteShape *GetThreeDSpriteShape(SpriteID image, int origin_x, int origin_y, int ex, int ey, int ez, int narrow_x0, int narrow_y0, int narrow_dx, int narrow_dy);
float ThreeDSpriteShapeFit(SpriteID image, int origin_x, int origin_y, std::span<const ThreeDPart> parts, double dx, double dy, double dz);
void ForgetThreeDSpriteShapes();
std::string DescribeThreeDSpriteShapes();
std::string DescribeThreeDSpriteShapeTotals();

#endif /* THREE_D_SHAPE_H */
