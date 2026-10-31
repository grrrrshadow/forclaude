/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file simtrains_cab.h The desk of a cab of MSTS over the view from the cab of its engine.
 *
 * The player (31. 10.): "the view from the cab, the realistic controls and the simulation of the driving and the cabs
 * of the simtrains models"; and of the view: "we have our better view, turning the head with the right button in the
 * cab view". So the eye is ours, turned by the right button; the pictures of the desk (SimtrainsCab) are fixed to the
 * engine, each where MSTS sees it -- the one ahead, the one to the left, the one to the right --, and turn with the
 * engine, not with the head. Through their clear windows the world. Between two pictures the nearer one's edge goes
 * on, so that the desk has no gap.
 */

#ifndef SIMTRAINS_CAB_H
#define SIMTRAINS_CAB_H

#include "simtrains.h"

#include <cstdint>
#include <string>
#include <vector>

struct Vehicle;
struct CabViewCamera;

/** The desk of a cab drawn in a window, kept between frames: its pictures, and which of their pixels each of the window's shows. */
struct SimtrainsCabDesk {
	std::string path;   ///< the .cvf of the pictures
	bool night = false; ///< the pictures of the night
	/** A picture of the desk, its pixels premultiplied by their alpha (Colour::data). */
	struct Picture {
		int w = 0, h = 0;
		std::vector<uint32_t> px;
	};
	std::vector<Picture> pictures;
	/* Beyond a picture's edge the cab goes on in one colour: the mean of that edge, of each picture top, bottom, left, right (premultiplied, opaque). */
	std::vector<uint32_t> fills;
	static constexpr uint32_t FILL = 0xF0000000; ///< a look-up of a fill, its index in the low bits
	/* Which pixel of which picture each pixel of the window shows: the picture in the top four bits, NONE nothing. */
	static constexpr uint32_t NONE = UINT32_MAX;
	std::vector<uint32_t> look_up;
	int width = 0, height = 0;   ///< the window's picture it was made for
	double head = 1e9, pitch = 1e9; ///< and the head turned so
};

const SimtrainsCab *SimtrainsCabOf(const Vehicle *v);
bool DrawSimtrainsCabDesk(SimtrainsCabDesk &desk, const SimtrainsCab &cab, const CabViewCamera &camera, int left, int top, int width, int height);

#endif /* SIMTRAINS_CAB_H */
