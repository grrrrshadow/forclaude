/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file round_track.h Round curves of rails (gui.round_track_curves), only how they look.
 *
 * The player (14. 10.), after a video of someone's OpenTTD with curved track: "round arcs of the rails in the
 * settings"; "it will run nicely for us, we are 3D and have the angles in the curve"; "work it out, it is just one
 * arc over two, like two vanilla ones". A curve of the game's rails is pieces along the axes and on the diagonal,
 * meeting at the midpoints of the tile edges at 45 degrees. Where two such pieces meet, the centre line of the track
 * is drawn as a circular arc tangent to both, from as far before the corner as after it -- a tile at most, half
 * the straight between two corners when they are close -- so the plain curve of the game (a straight, a piece on
 * the diagonal, a straight) becomes one round arc over about two tiles. The track is drawn along it from the
 * picture of the straight track of its rail type; a train on it is drawn as its 3D likeness, turned along the arc.
 * Building, the pathfinder and the trains' movement are untouched.
 *
 * Only in the turned view (three_d.h). The player: "at angle 0 it is off, angle 0 is for building, turning is for
 * looking" -- facing north, and in every other viewport, the curves are as they always were.
 *
 * Only plain rail on flat ground without signals is rounded; a curve ends where another tile is. At a switch the
 * straight way stays straight and the way branching off it is round, drawn on over the straight way as far as its arc
 * goes there.
 */

#ifndef ROUND_TRACK_H
#define ROUND_TRACK_H

#include "tile_type.h"

struct Vehicle;
struct DrawPixelInfo;

bool RoundTrackEnabled();
bool RoundTrackDrawing();
bool IsRoundTrackTile(TileIndex tile);
bool RoundTrackHidesCatenary(TileIndex tile);
void RegisterRoundTrackTile(TileIndex tile);
void DrawRoundTrackGround(TileIndex tile);
void DrawRoundTrackStrips(const DrawPixelInfo &dpi);

/** Where a vehicle on a round curve is drawn: on the arc, along it. */
struct RoundTrackPose {
	double x = 0, y = 0;  ///< the point of the arc, world units
	float azimuth = 0;    ///< the angle its likeness is drawn at with the map facing north (ViewAzimuth() of three_d_vehicle.cpp)
};
bool GetRoundTrackPose(const Vehicle *v, RoundTrackPose &pose);

/** While a view is drawn: if it draws the round curves (the turned view), the tiles they are on are collected and drawn after the ground. */
class RoundTrackPass {
public:
	explicit RoundTrackPass(bool active);
	~RoundTrackPass();
private:
	bool was;
};

#endif /* ROUND_TRACK_H */
