/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file screenshot.h Functions to make screenshots. */

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

std::string_view GetCurrentScreenshotExtension();

/** Type of requested screenshot */
enum ScreenshotType : uint8_t {
	SC_VIEWPORT,    ///< Screenshot of viewport.
	SC_CRASHLOG,    ///< Raw screenshot from blitter buffer.
	SC_ZOOMEDIN,    ///< Fully zoomed in screenshot of the visible area.
	SC_DEFAULTZOOM, ///< Zoomed to default zoom level screenshot of the visible area.
	SC_WORLD,       ///< World screenshot.
	SC_HEIGHTMAP,   ///< Heightmap of the world.
	SC_MINIMAP,     ///< Minimap screenshot.
};

bool MakeHeightmapScreenshot(std::string_view filename);
void MakeScreenshotWithConfirm(ScreenshotType t);
bool MakeScreenshot(ScreenshotType t, const std::string &name, uint32_t width = 0, uint32_t height = 0);
bool MakeMinimapWorldScreenshot();

extern std::string _screenshot_format_name;
extern std::string _full_screenshot_path;

/**
 * How many screenshots were made (or failed) so far, and the last one's file -- empty when it failed. A screenshot is
 * made on the video driver's thread after MakeScreenshot() returns; who asked from another thread waits for the count
 * to go up.
 */
uint32_t LastScreenshotMade(std::string *path = nullptr);
bool ScreenshotOfShown(std::string &path);
#include <atomic>
extern std::atomic<bool> _screenshot_no_message; ///< the next screenshot shows no window saying it was made (the agent's interface)

#endif /* SCREENSHOT_H */
