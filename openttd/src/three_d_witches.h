/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_witches.h The witches: at night they fly over the towns, a lantern and a hand torch shining.
 *
 * The player (18. 10.): "we need witches flying at night", "make the witches as big as the girls at a stop", "about
 * the lantern I want light all round, and from the hand torch a beam", "yes, let the witches fly" -- one to three at
 * once over the towns, from town to town, only while it is dark, on the turned map and from the eyes. The colleague's
 * witches (grrrrf hra/3d/PROSBA-CARODEJNICE.md, objekty/carodejnice/, "druh": "carodejnice") fly by the frames of
 * their animation "let"; the places of the lantern and of the torch are in each frame, what their light is like in
 * the index ("svetla"). Seen only: they take nothing of the game's, and its random numbers are not touched.
 */

#ifndef THREE_D_WITCHES_H
#define THREE_D_WITCHES_H

#include "zoom_type.h"
#include "gfx_type.h"

#include <string>
#include <vector>

/** A light of a witch in the world: her lantern's light all round, or her torch's beam. */
struct ThreeDWitchLight {
	bool torch = false;        ///< a torch's beam; else a lantern
	double x = 0, y = 0, z = 0; ///< where it shines from: world units, z units of height
	double dx = 0, dy = 0, dz = 0; ///< a torch's beam: which way, a unit long (z as the metres along x and y go)
	double reach = 0;          ///< how far it shines, world units along the ground
	double half_angle = 0;     ///< a torch's cone: half its angle, radians
	double ground = 0;         ///< a torch's beam: how far along it it falls on the ground, world units; 0 not within its reach
	Colour colour;
};

void AddThreeDWitchesToViewport(ZoomLevel zoom);
void AddThreeDWitchesToEye();
std::vector<ThreeDWitchLight> GetThreeDWitchLights(bool eye);
bool SendThreeDWitch(std::string_view kind, double x, double y, double heading);
std::string DescribeThreeDWitches();

#endif /* THREE_D_WITCHES_H */
