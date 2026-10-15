/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d.h The 3D of this game, ours: the map view turned to any angle
 * and the previews that turn (gui.three_d, "3D: turning previews and turning
 * the map view", Game Options > Graphics, on by default).
 *
 * The map view is turned by an angle, the yaw, about the point in the middle
 * of the screen: 0 is the game as it has always been, drawn by the usual path
 * to the pixel; any other angle is drawn by the second renderer (viewport.cpp,
 * ViewportDoDrawTurned()), which takes what the tiles and vehicles say to draw
 * and draws it turned. The angle is nobody's but this client's: it is not in
 * the savegame, not in the config, and the game starts facing north every
 * time. With the setting off nothing of this exists.
 *
 * How the turn is done: the world is turned, not the camera. A world point is
 * turned about the pivot (the ground point under the middle of the screen
 * when the turn began) by the yaw and then drawn by the game's own projection,
 * RemapCoords(). So the main viewport goes on being a plain viewport of a
 * world that happens to be turned: scrolling, zooming and the dirty blocks
 * all work as they did, and only the two directions world -> screen
 * (ProjectTurned()) and screen -> world (InverseRemapCoordsTurned()) know
 * about the angle. Turning changes the pivot to the point under the middle of
 * the screen and moves the scroll position so that this point stays put.
 *
 * The player turns the view with the middle mouse button held and dragged
 * sideways (RotateViewByDrag(), 4 pixels to a degree), or with the keys Y and
 * N (15 degrees, with Ctrl 45, the next of the eight directions the vehicles
 * and our buildings have pictures for); the right button scrolls as it always
 * has. Y and N are the only two letters no window of the game binds.
 */

#ifndef THREE_D_H
#define THREE_D_H

#include "core/geometry_type.hpp"
#include "house_type.h"
#include "industry_type.h"
#include "engine_type.h"
#include "gfx_type.h"
#include "window_type.h"
#include "zoom_type.h"
#include "tile_type.h"

#include <map>
#include <span>

struct Viewport;

/** The roof of a box of a house drawn as boxes (ThreeDPart). */
enum class ThreeDRoof : uint8_t {
	Flat,   ///< a flat top at the box's height
	GableX, ///< a gable roof with the ridge along X, rising above the walls by the rise
	GableY, ///< a gable roof with the ridge along Y
	Hip,    ///< a hip roof, the same slope on all four sides
};

/** A box of a house: its footprint in tile units from the north corner of the house's north tile, its wall height in pixels, its roof. */
struct ThreeDPart {
	float x0, x1; ///< along X, 16 to a tile
	float y0, y1; ///< along Y
	float h;      ///< height of the walls, pixels
	ThreeDRoof roof;
	float rise;   ///< how far the ridge rises above the walls, pixels
};

/** A house of the original graphics as boxes, read off its one sprite by hand (table/three_d_houses.h). */
struct ThreeDHouse {
	HouseID id;         ///< the house's id (its north tile)
	uint8_t tiles_x;    ///< tiles along X
	uint8_t tiles_y;    ///< tiles along Y
	uint16_t first;     ///< first part in _three_d_house_parts
	uint16_t count;     ///< number of parts
};

const ThreeDHouse *GetThreeDHouse(HouseID house);
const ThreeDPart *GetThreeDHouseParts(const ThreeDHouse *house);

/** A picture of the turned view at an angle and a zoom: a vehicle or a figure drawn from its object, the preview of an industry. */
struct ThreeDModelPicture {
	int width = 0;
	int height = 0;
	int left = 0; ///< where the top-left pixel goes from the model's origin (the north corner of its north tile at the ground, ProjectTurned()), pixels of the zoom
	int top = 0;
	std::vector<uint32_t> pixels; ///< Colour::data, alpha 0 where there is nothing
};

/**
 * A tile of the marijuana plantation as the turned view lays it from the parts
 * of the fields (GetThreeDFieldTile(), three_d_objects.h): the field's soil, the
 * plants of the stage, a girl at work; and from the field tile before the field
 * road, the road tile behind it with the shed, the haystack and the girls by the
 * road (GetThreeDPlantationTile() in industry_cmd.cpp, after DrawMarijuanaPlantationTile()).
 */
struct ThreeDPlantationTile {
	uint8_t stage;     ///< 0 bare, 1 small plants, 2 grown
	int8_t worker;     ///< the girl at work on this tile, 0 to 5, or -1
	bool girls;        ///< whether the girls are there at all
	bool road_behind;  ///< whether the tile behind this one (y + 1) is the field road this tile holds
	uint8_t x;         ///< the tile's place along X in the plantation
	bool shed;         ///< the road tile behind has the shed and the haystack
	bool girls_nw;     ///< the road tile behind has the pair of girls by its north-west edge
	bool girls_se;     ///< and by its south-east edge
};
bool GetThreeDPlantationTile(TileIndex tile, ThreeDPlantationTile &out);
const char *ThreeDPlantationWorkerName(int worker);

/**
 * The 3D likeness of a vehicle made from its eight sprites (three_d_vehicle.cpp):
 * the surface of the visual hull of the eight outlines, each point with its colour
 * in every view, drawn at any angle for the preview beside the purchase window.
 */
struct ThreeDVehicleModel;
struct Vehicle;
const ThreeDVehicleModel *GetThreeDVehicleModel(EngineID engine, PaletteID pal, const Vehicle *veh = nullptr);
ZoomLevel ThreeDVehicleModelZoom(const ThreeDVehicleModel *model);
bool RenderThreeDVehicle(const ThreeDVehicleModel *model, ZoomLevel zoom, float azimuth, ThreeDModelPicture &out, bool on_track, int part = -1);
const ThreeDModelPicture *GetThreeDVehiclePictureAt(const ThreeDVehicleModel *model, ZoomLevel zoom, float azimuth, bool on_track, int part = -1);
Rect ThreeDVehicleFrame(const ThreeDVehicleModel *model, ZoomLevel zoom);
std::string DescribeThreeDVehicleModel(const ThreeDVehicleModel *model);
void ClearThreeDVehicleModels();
struct Vehicle;
struct RoundTrackPose;
bool AddThreeDVehicleToViewport(const Vehicle *v, PaletteID pal, ZoomLevel zoom, const RoundTrackPose *pose = nullptr);

/* The previews that turn (three_d_preview_gui.cpp). */
void ShowThreeDVehiclePreview(WindowNumber parent_number);
void ShowThreeDIndustryPreview(IndustryID industry);
void DrawThreeDPreviews();
std::string DescribeThreeDPreviews();
void ThreeDPreviewsTick();
void SetThreeDPreviewYaw(float yaw);
EngineID GetBuildVehicleSelectedEngine(WindowNumber number);
bool SelectBuildVehicleEngine(WindowNumber number, EngineID engine);

/** A step of the keys: 15 degrees, a quarter of the way to the next of the eight directions. */
static constexpr int THREE_D_KEY_STEP_DEG = 15;
/** A step of the keys with Ctrl: the next of the eight directions. */
static constexpr int THREE_D_KEY_STEP_CTRL_DEG = 45;
/** The middle button dragged this many pixels turns the view by a degree. */
static constexpr int THREE_D_DRAG_PIXELS_PER_DEG = 4;
/** Two clicks of the middle button within this many milliseconds, without a drag, put the view back to north. */
static constexpr int THREE_D_DOUBLE_CLICK_MS = 500;

/** How far past the edge of the map the turned view may go, in tiles: the void around the map the player may look at (the player: 42, not 64). */
static constexpr int THREE_D_VOID_TILES = 42;

bool ThreeDEnabled();
float GetViewYaw();
float GetViewYawSigned();
bool IsViewTurned();
bool IsViewportTurned(const Viewport &vp);
int TurnedDirectionStep();
void RotateView(float degrees);
void SetViewYaw(float degrees);
void RotateViewByDrag(int pixels);
void ResetViewYaw();
void ThreeDViewForBuilding();

void TurnWorldPoint(double x, double y, double &xr, double &yr);
void GetViewTurn(double &c, double &s, double &px, double &py);
void UnturnWorldPoint(double xr, double yr, double &x, double &y);
void ProjectTurnedF(double x, double y, double z, double &vx, double &vy);
Point ProjectTurned(int x, int y, int z);
void InverseTurnedFlatF(double vx, double vy, double z, double &x, double &y);
void TurnDirtyRect(int &left, int &top, int &right, int &bottom);
Point InverseRemapCoordsTurned(int x, int y, bool clamp_to_map = false);

#endif /* THREE_D_H */
