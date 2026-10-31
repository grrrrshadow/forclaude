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
#include "gfx_type.h"

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

/**
 * The player at the desk of a train of MSTS (the player, 31. 10.: "our arrows and horn are better there; put a switch at
 * the bottom of the cab view window: controls MSTS / arrows"; "the train is driven by the player, so the game does not
 * drive it"). What the arrows or the keys move goes to the game as commands (CmdTrainCabControl()); the first touch
 * takes the train over (Train::cab_driven), the window closed lets it go.
 *
 * The arrows move one combined lever, as the desks of the electric units have it: up the notches of the throttle
 * (the .eng's), down from nought the electric brake (if the engine has one, three steps), then the train's brake in
 * five steps to full service with the electric brake still on; held down at the end a second, the emergency brake.
 * The keys are those of MSTS: D and A the throttle, ' and ; the brake, ] and [ the engines' own brake, . and , the
 * electric brake, W and S the reverser, Backspace the emergency brake, Space the horn.
 */
struct SimtrainsCabDriver {
	int lever = 0;          ///< the arrows' combined lever: above nought a notch, below the electric brake, then the brake
	bool read = false;      ///< the lever read from the train (when the window opened onto a train driven already)
	DirectionKeys held{};   ///< the arrows held at the last look
	int repeat_ms = 0;      ///< milliseconds till the lever moves again while an arrow is held
	int bottom_ms = 0;      ///< milliseconds Down has been held with the lever at its end
	std::string done;       ///< what was done last, for the rig

	static constexpr int BRAKE_STEPS = 5;
	static constexpr int DYNAMIC_STEPS = 3;
	static constexpr int REPEAT_MS = 250;
	static constexpr int EMERGENCY_MS = 1000;

	void Drive(const Vehicle *v, DirectionKeys now, int ms);
	bool Key(const Vehicle *v, char32_t key, uint16_t keycode);
};

bool IsSimtrainsTrain(const Vehicle *v);
void SimtrainsCabLetGo(const Vehicle *v);
std::string SimtrainsDeskText(const Vehicle *v);

const SimtrainsCab *SimtrainsCabOf(const Vehicle *v);
bool DrawSimtrainsCabDesk(SimtrainsCabDesk &desk, const SimtrainsCab &cab, const CabViewCamera &camera, int left, int top, int width, int height);

#endif /* SIMTRAINS_CAB_H */
