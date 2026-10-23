/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file frame_diary.h The diary of the frames, snimky.log in the player's folder: where the time of a frame goes on the
 * player's own machine (the player, 23. 10.: "the arrow twice worked, then not; fix the slow frames, the cause, not the
 * consequences" -- a double tap is lost when a frame is longer than 150 ms, the rig's 'chodec tuktuk'). Every five
 * seconds a line: how many frames, the longest and what it spent its time on, the game's ticks, the view from the eyes.
 */

#ifndef FRAME_DIARY_H
#define FRAME_DIARY_H

/** The parts of a frame of the drawing (VideoDriver::Tick()), milliseconds. */
struct FrameDiaryParts {
	double buffer = 0; ///< the video buffer locked (a wait for the screen)
	double lock = 0;   ///< the game's state taken from the game's thread (a wait for its tick)
	double input = 0;  ///< the keys, the mouse and the system's messages
	double windows = 0; ///< the windows updated and drawn (UpdateWindows())
	double paint = 0;  ///< the picture to the screen (the video driver's Paint())
	double total = 0;
};

void FrameDiaryDraw(const FrameDiaryParts &parts);
void FrameDiaryGameTick(double ms);
void FrameDiaryEyeCollected(double ms);
void FrameDiaryEyeDrawn(double ms);

#endif /* FRAME_DIARY_H */
