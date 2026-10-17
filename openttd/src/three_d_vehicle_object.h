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

#include <cmath>
#include <vector>

struct VehicleSpriteSeq;
struct Vehicle;
struct RoundTrackPose;

int ThreeDVehicleObjectPieceLength(EngineID engine, int set_length);
int ThreeDVehicleObjectPieceLengthOf(const Vehicle *v, int set_length);
bool GetThreeDVehicleObjectSprite(EngineID engine, const Vehicle *v, Direction direction, EngineImageType image_type, VehicleSpriteSeq *result);
void NoteThreeDVehicleWithoutObject(EngineID engine, const Vehicle *v, EngineImageType image_type, const VehicleSpriteSeq &seq);
bool WriteThreeDVehicleObjectPPM(EngineID engine, float az, float pxm, int state, int lights, const std::string &path, double walked_m = 0);
uint32_t GetVehicleSetSpriteLocalID(EngineID engine, const Vehicle *v, EngineImageType image_type);
void GetVehicleSetSpriteLocalIDs(EngineID engine, const Vehicle *v, EngineImageType image_type, std::vector<uint32_t> &ids);
ZoomLevels MakeThreeDVehicleObjectSprite(uint32_t request, SpriteLoader::SpriteCollection &sprite);
void ResetThreeDVehicleObjectSprites();
bool AddThreeDVehicleObjectToViewport(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose);
bool ThreeDVehicleObjectLitAtNight(const Vehicle *v);
struct ThreeDPictureLight;
bool GetThreeDVehicleObjectLights(const Vehicle *v, ZoomLevel zoom, const RoundTrackPose *pose, std::vector<ThreeDPictureLight> &lights, int &ax, int &ay, int &az);
struct ThreeDWorldLight;
bool GetThreeDVehicleObjectWorldLights(const Vehicle *v, std::vector<ThreeDWorldLight> &lights);

/** A vehicle (or a car of the game's own) drawn from its object in the view from the eyes (GetThreeDEyeVehicle()). */
struct ThreeDEyeVehicle {
	const struct ThreeDVehicleObject *obj = nullptr; ///< nullptr: a piece drawn with its anchor piece, nothing of its own
	float scale = 1.0f;         ///< its metres to the game's
	float along = 1.0f;         ///< its length foreshortened as the game spaces its vehicles
	double x = 0, y = 0, z = 0; ///< its origin, world units
	float facing = 0;           ///< degrees in the world, 0 along +x, 90 along +y, finer than the eight directions
	int state = 0;              ///< its state its GRF picks (paint, fading, rust), 0 none
	int lights = 0;             ///< its lights on at night (front 1, back 2)
	double rolled = 0;          ///< how far it has gone, world units: its wheels turn by it ("kola_v_modelu")
	uint64_t walk = 0;          ///< the frames of its crew that walks (WalkCode() of three_d_vehicle_object.cpp)
	double curvature = 0;       ///< how much its way turns, radians a world unit to the left: its front wheels steer by it
};
bool GetThreeDEyeVehicle(const Vehicle *v, ThreeDEyeVehicle &out);
void GetThreeDEyeObjectLights(const ThreeDEyeVehicle &ev, std::vector<ThreeDWorldLight> &lights);
struct ThreeDObjectTexture;
const ThreeDObjectTexture *GetThreeDEyeVehicleTexture(const struct ThreeDVehicleObject *obj, int state, double pixels_per_metre, int index);
struct ThreeDModelPicture;
const ThreeDModelPicture *GetThreeDVehicleObjectPreviewPicture(EngineID engine, ZoomLevel zoom, float azimuth);
const ThreeDModelPicture *GetThreeDVehicleObjectPreviewFit(EngineID engine, int side, float azimuth, int &cx, int &cy);
const ThreeDModelPicture *GetThreeDEyeVehiclePicture(const ThreeDEyeVehicle &ev, ZoomLevel zoom, float yaw);
std::string DescribeThreeDVehicleObjectSprites();
void QueueThreeDFireworks(TileIndex hut_front);
void AddThreeDEffectsToViewport(ZoomLevel zoom);
bool AddThreeDObjectToViewport(const struct ThreeDVehicleObject *obj, float scale, int x, int y, int z, float facing, ZoomLevel zoom, bool lit = false);
const std::vector<ThreeDPictureLight> *GetThreeDObjectLights(const struct ThreeDVehicleObject *obj, float scale, float facing, ZoomLevel zoom);
bool AddThreeDFigureToViewport(std::string_view name, float scale, int x, int y, int z, float facing, ZoomLevel zoom, TileIndex tile = INVALID_TILE, uint32_t phase = 0, int frame = -1);
struct ThreeDModelPicture;
const ThreeDModelPicture *GetThreeDFigurePicture(std::string_view name, float scale, float facing, ZoomLevel zoom, TileIndex tile, uint32_t phase, int *height = nullptr, int frame = -1, float yaw = NAN, int z = INT_MIN, int x = 0, int y = 0);
void PinThreeDPictures(bool pin);
bool ThreeDPicturesBeingMade();
size_t ThreeDPicturesWaiting();
void StopThreeDPictureThread();
const ThreeDModelPicture *GetThreeDPlantPicture(std::string_view name, float scale, float stretch, float facing, ZoomLevel zoom, int *height = nullptr, float yaw = NAN);
struct ThreeDHeldThing;
bool DrawThreeDHeldThing(const ThreeDHeldThing &held, uint32_t *dst, float *zb, int width, int height, double focal);

/* three_d_vehicle.cpp */
int ThreeDEngineLength(EngineID engine);
Point ThreeDSpriteAnchor(VehicleType type, int length, Direction dir);
double ThreeDChainPositions(VehicleType type, const std::vector<int> &lengths, std::vector<double> &pos);

/* spritecache.cpp */
bool SetThreeDVehicleObjectSprite(SpriteID sprite, uint32_t request);

#endif /* THREE_D_VEHICLE_OBJECT_H */
