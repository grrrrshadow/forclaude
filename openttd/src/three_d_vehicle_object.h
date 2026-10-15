/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_vehicle_object.h Vehicles drawn from 3D objects (three_d_objects.h) instead of their sets' sprites.
 *
 * The player (14. 10.): "we will never render anything again"; vehicles go into the game as .glb objects, the
 * colleague's Sergej first (grrrrf hra/3d/PROSBA-VOZIDLA-3D.md). For an engine with an object the game makes the
 * sprites itself: when one is first drawn, the object is drawn with the camera of the sprites (30 degrees above
 * the horizon, 48.8 pixels to a metre at 16x, foreshortened 1/sqrt 2 along the vehicle in the four directions of
 * straight track, as the sets draw) and lit from its normals, into a sprite of the sprite cache of the direction;
 * so the map facing north, the depot, the lists and the purchase window draw it as any sprite. In the turned view
 * it is drawn at the angle of the view directly. One rigid object over the pieces of an articulated vehicle,
 * hung from the middle one; the other pieces draw nothing.
 */

#ifndef THREE_D_VEHICLE_OBJECT_H
#define THREE_D_VEHICLE_OBJECT_H

#include "engine_type.h"
#include "direction_type.h"
#include "vehicle_type.h"
#include "zoom_type.h"
#include "tile_type.h"
#include "spriteloader/spriteloader.hpp"

struct VehicleSpriteSeq;
struct Vehicle;
struct RoundTrackPose;

int ThreeDVehicleObjectPieceLength(EngineID engine, int set_length);
bool GetThreeDVehicleObjectSprite(EngineID engine, const Vehicle *v, Direction direction, EngineImageType image_type, VehicleSpriteSeq *result);
ZoomLevels MakeThreeDVehicleObjectSprite(uint32_t request, SpriteLoader::SpriteCollection &sprite);
void ResetThreeDVehicleObjectSprites();
bool AddThreeDVehicleObjectToViewport(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose);
std::string DescribeThreeDVehicleObjectSprites();
bool AddThreeDFigureToViewport(std::string_view name, float scale, int x, int y, int z, float facing, ZoomLevel zoom, TileIndex tile = INVALID_TILE, uint32_t phase = 0, int frame = -1);
struct ThreeDModelPicture;
const ThreeDModelPicture *GetThreeDFigurePicture(std::string_view name, float scale, float facing, ZoomLevel zoom, TileIndex tile, uint32_t phase, int *height = nullptr, int frame = -1);

/* three_d_vehicle.cpp */
int ThreeDEngineLength(EngineID engine);
Point ThreeDSpriteAnchor(VehicleType type, int length, Direction dir);
double ThreeDChainPositions(VehicleType type, const std::vector<int> &lengths, std::vector<double> &pos);

/* spritecache.cpp */
bool SetThreeDVehicleObjectSprite(SpriteID sprite, uint32_t request);

#endif /* THREE_D_VEHICLE_OBJECT_H */
