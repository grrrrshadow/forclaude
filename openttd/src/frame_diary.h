/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file frame_diary.h The diary of the frames, snimky.log in the player's folder: where the time of a frame goes on the
 * player's own machine (the player, 23. 10.: "the arrow twice worked, then not; fix the slow frames, the cause, not the
 * consequences" -- a double tap is lost when a frame is longer than 150 ms, the rig's 'chodec tuktuk'). Every five
 * seconds a line: how many frames, the longest and what it spent its time on, the game's ticks, the view from the eyes.
 */

#ifndef FRAME_DIARY_H
#define FRAME_DIARY_H

#include <cstddef>
#include <cstdint>

/** The parts of a frame of the drawing (VideoDriver::Tick()), milliseconds. */
struct FrameDiaryParts {
	double buffer = 0; ///< the video buffer locked (a wait for the screen)
	double lock = 0;   ///< the game's state taken from the game's thread (a wait for its tick)
	double input = 0;  ///< the keys, the mouse and the system's messages
	double windows = 0; ///< the windows updated and drawn (UpdateWindows())
	double paint = 0;  ///< the picture to the screen (the video driver's Paint())
	double total = 0;
	double messages_ms = 0;    ///< of the input, the system's messages handled (VideoDriver::PollEvent())
	uint32_t messages = 0;     ///< how many
	uint32_t messages_cut = 0; ///< frames whose messages were left for the next frame, their time up
};

void FrameDiaryDraw(const FrameDiaryParts &parts);
void FrameDiaryGameTick(double ms);
void FrameDiaryEyeCollected(double ms);
void FrameDiaryEyeDrawn(double ms);
void FrameDiaryEyeCard(bool on_card, double prepare_ms, double card_ms, size_t corners);
void FrameDiaryWindowsTimers(double ms);
void FrameDiaryWindowsBlocks(double ms);
void FrameDiaryMapBlock(double ms);
void FrameDiaryEyeWindow(double ms);
void FrameDiaryGroundWait(double ms);
size_t FrameDiaryMemoryMB();

#endif /* FRAME_DIARY_H */
