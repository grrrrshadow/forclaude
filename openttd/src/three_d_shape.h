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
 * The walls a building's sprite never shows take the pattern of those it shows the same way (ThreeDWallPatterns), for
 * the hand tables too, and the building turns all round.
 */

#ifndef THREE_D_SHAPE_H
#define THREE_D_SHAPE_H

#include "three_d.h"
#include "gfx_type.h"

#include <array>
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
	float one_fit = 0;              ///< the same of the best one box with its roof
	float kept = 0;                 ///< the share of the sprite's pixels inside the shape: drawn on its faces (the rest is not drawn)
	float box_kept = 0;             ///< the same of the box drawn before
	float ms = 0;                   ///< how long finding it took (the thread in the background)
	float read_ms = 0;              ///< how long reading the sprite's pixels took (the main thread)
	int tried = 0;                  ///< how many shapes were tried
};

/**
 * Where a wall of a building the sprite never shows (facing -X or -Y) takes its pixels from (GetThreeDWallPatterns()):
 * a piece of a wall of the same box the sprite shows, laid along it as many times as fit. Where the wall it takes them
 * from repeats a pattern (a window and the wall beside it), the piece is one repeat, the one most like the others: a
 * door or a shop's sign is on the front only.
 */
struct ThreeDWallSource {
	int8_t axis = -1;      ///< the wall it takes them from: 0 the box's wall facing +X, 1 facing +Y; -1 none (mirrored from the wall opposite, as before)
	bool periodic = false; ///< whether the piece is one repeat of that wall's pattern, else all of that wall the sprite shows
	bool mirror = false;   ///< whether the wall opposite is shown nearly whole: a gable over this wall is mirrored from its gable as before
	float start = 0;       ///< where the piece begins along that wall: world units along Y (the wall facing +X) or X (facing +Y), in the frame of the boxes
	float length = 0;      ///< how long it is
};

/** The walls a building's sprite never shows, for each of its boxes (GetThreeDWallPatterns()). */
struct ThreeDWallPatterns {
	std::vector<std::array<ThreeDWallSource, 2>> back; ///< for each box: [0] its wall facing -X, [1] facing -Y
	std::atomic<bool> ready = false;                   ///< whether they are read: the thread in the background reads them, the walls are mirrored meanwhile

	/* For the rig (test3d tvary). */
	SpriteID image = 0;
	float ms = 0;      ///< how long reading them took (the thread in the background)
	float read_ms = 0; ///< how long reading the sprite's pixels took (the main thread)
	std::string said;  ///< what was found on each wall the sprite shows
};

extern bool _three_d_sprite_shapes_on;

const ThreeDSpriteShape *GetThreeDSpriteShape(SpriteID image, int origin_x, int origin_y, int ex, int ey, int ez, int narrow_x0, int narrow_y0, int narrow_dx, int narrow_dy);
float ThreeDSpriteShapeFit(SpriteID image, int origin_x, int origin_y, std::span<const ThreeDPart> parts, double dx, double dy, double dz);
const ThreeDWallPatterns *GetThreeDWallPatterns(SpriteID image, int origin_x, int origin_y, std::span<const ThreeDPart> parts, bool cut, double cx0, double cx1, double cy0, double cy1);
void ForgetThreeDSpriteShapes();
std::string DescribeThreeDSpriteShapes();
std::string DescribeThreeDSpriteShapeTotals();
std::string DescribeThreeDWallPatterns();

#endif /* THREE_D_SHAPE_H */
