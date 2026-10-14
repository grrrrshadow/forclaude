/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_objects.h Buildings as real 3D objects (glTF binary, .glb), drawn by the game itself in the turned
 * view and the cab view.
 *
 * The player (13. 10.): "the next houses will be real 3D objects, not eight renders, to save megabytes"; the
 * colleague made every temperate house of the original game so (grrrrf hra/3d/objekty/, the format in
 * hra/3d/OBJEKTY-3D.md): one .glb a building, triangles with one texture, metres, the origin at the north corner of
 * its north tile, and an index objekty.json saying which house and which of its colour variants each one is. They
 * are read from baseset/decouple/objekty/<set>/ the first time they are wanted. The view facing north keeps the
 * sprite: an object is drawn only where the view is 3D.
 *
 * Vehicles too (14. 10.), the player: "we will never render anything again"; the colleague's Sergej first
 * (hra/3d/PROSBA-VOZIDLA-3D.md): entries "druh": "vozidlo" of an index, an engine of a set as one .glb. A vehicle
 * has no sprites of its own then: the game draws its sprites from the object (three_d_vehicle_object.cpp).
 */

#ifndef THREE_D_OBJECTS_H
#define THREE_D_OBJECTS_H

#include "house_type.h"
#include "engine_type.h"
#include "vehicle_type.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

/** A texture of an object at one level of detail: 32bpp pixels in the blitter's colours, 0 where it is transparent. */
struct ThreeDObjectTexture {
	int w = 0;
	int h = 0;
	std::vector<uint32_t> px;
};

/** A building as a 3D object. */
struct ThreeDObject {
	std::string name;         ///< its name in the index
	std::string file;         ///< the .glb it came from
	HouseID house = UINT16_MAX; ///< the house of the original game it is (its north tile's); UINT16_MAX for a set's house
	uint32_t grfid = 0;       ///< a house of a set (NewGRF): the set's id as the game keeps it, 0 for an original house
	uint16_t grf_house = 0;   ///< and the house's number in the set (its Action 0 id, the north tile's)
	uint8_t variants = 0x0F;  ///< the colour variants (bit per slot, TileHash2Bit()) it is
	int tiles_x = 1;          ///< its size in tiles
	int tiles_y = 1;
	std::vector<float> pos;   ///< x, y, z of each corner: world units along x and y from the north corner, units of height up
	std::vector<float> uv;    ///< u, v of each corner in the texture, 0..1
	std::vector<uint32_t> tri; ///< three corners a triangle
	float top = 0;            ///< its height, units of height
	std::vector<uint8_t> png; ///< the texture as it is in the file, read into pixels the first time it is drawn
	mutable std::vector<ThreeDObjectTexture> levels; ///< the texture, halved for each level of detail

	const ThreeDObjectTexture *GetTexture(int level) const;
};

/**
 * The shape of a vehicle as a 3D object, shared by its paints (the colleague's hra/3d/PROSBA-VOZIDLA-3D.md): metres in
 * the vehicle's frame -- x forward, y to its left, z up, the origin in the middle of the vehicle on the top of the
 * rail -- with the normals, so the game lights it itself.
 */
struct ThreeDVehicleShape {
	std::vector<float> pos;    ///< x, y, z of each corner, metres
	std::vector<float> nrm;    ///< its normal; empty when the file has none (drawn unlit)
	std::vector<float> uv;     ///< u, v of each corner in its texture
	std::vector<uint32_t> tri; ///< three corners a triangle
	std::vector<uint8_t> tri_tex; ///< the texture of each triangle: 0 the body's (the one painted, "textura", and the one the paint mask is for), then the others of the file and of the wheels
	std::vector<std::vector<uint8_t>> pngs; ///< the textures, as they are in the files
	std::vector<uint32_t> flats; ///< for each texture, the colour where a material has no picture
	float x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0; ///< its extent, metres
};

/** The paint of a vehicle object ("lak"): which pixels of the body's texture are paint, how they fade with age, a colour by number. */
struct ThreeDVehiclePaint {
	std::string mask_file;      ///< "maska": a grey PNG of the body's texture's layout, 255 where it is paint
	bool fades = false;         ///< "cil" given: the paint fades to it with age
	uint32_t target = 0;        ///< "cil": the colour it fades to (Colour::data)
	int years = 20;             ///< "let": in so many years it is the target's
	int step = 1;               ///< "krok": a step of fading every so many years
	bool coloured = false;      ///< "barva" given: the paint is that colour, at the lightness of the texture
	uint32_t colour = 0;        ///< "barva" (Colour::data)
	int start = 0;              ///< "zacatek": the texture's lightness the colour is at its own (0: the brightest of the mask)
};

/** A figure riding in a vehicle object ("posadka"): a figure of an index ("druh": "postava"), where its feet stand, turned. */
struct ThreeDVehicleCrew {
	std::string figure;         ///< "postava": the figure's name
	float at[3] = {};           ///< "poloha": where its feet stand, metres in the vehicle's frame
	float turn = 0;             ///< "otoceni": degrees about z, 0 facing forward as the vehicle
};

/** Wheels of a vehicle object from a file of their own ("kola"): one wheel, at every place it is. */
struct ThreeDVehicleWheels {
	std::string file;           ///< "soubor": the wheel, its centre at the origin, its axle along y, its outer face to +y (the left)
	std::vector<std::array<float, 3>> at; ///< "polohy": the centres, metres in the vehicle's frame
	bool both_sides = true;     ///< "oba_boky": each also mirrored to the other side
};

/**
 * A vehicle as a 3D object: one engine of a set (NewGRF) by the set's id and the engine's number in it, drawn by the
 * game from the object instead of the set's sprites -- as sprites of the eight directions it makes itself, and at
 * any angle in the turned view. One rigid object over the pieces it names, drawn by its anchor piece about the middle
 * of the chain; the other pieces draw nothing.
 */
struct ThreeDVehicleObject {
	std::string name;               ///< its name in the index
	std::string file;               ///< the .glb, its path
	std::string paint_file;         ///< a texture over the same layout instead of the file's ("textura"), its path; empty for the file's
	VehicleType type = VehicleType::Invalid;
	uint32_t grfid = 0;             ///< the set's id as the game keeps it
	std::vector<uint16_t> pieces;   ///< the engines (their numbers in the set, Action 0) of the pieces it covers, front first
	int anchor = 0;                 ///< the piece it is drawn by: the middle one ("kotva": "stred")
	bool has_cab = false;           ///< "kabina": where the eye of the cab view is
	float cab[3] = {};              ///< x, y, z, metres
	ThreeDVehiclePaint paint;       ///< "lak"
	std::vector<ThreeDVehicleWheels> wheels; ///< "kola"
	std::vector<ThreeDVehicleCrew> crew;     ///< "posadka": never fading, lit as the vehicle

	const ThreeDVehicleShape *GetShape() const;
	const ThreeDObjectTexture *GetTexture(int level, int index = 0) const;
	const ThreeDObjectTexture *GetPaintMask(int level) const;

	mutable std::shared_ptr<const ThreeDVehicleShape> shape; ///< read the first time it is drawn
	mutable bool shape_tried = false;
	mutable std::vector<std::vector<ThreeDObjectTexture>> levels; ///< the textures, each halved for each level of detail
	mutable std::vector<ThreeDObjectTexture> mask_levels;         ///< the paint mask likewise (its grey in the red)
	mutable bool mask_tried = false;
};

const ThreeDVehicleObject *GetThreeDVehicleObject(EngineID engine, int *piece = nullptr);
const ThreeDVehicleObject *GetThreeDFigure(std::string_view name);
std::string DescribeThreeDVehicleObjects();

const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot);
const ThreeDObject *GetThreeDHouseObjectAny(HouseID north_house, uint colour_slot);
std::string DescribeThreeDObjects();

#endif /* THREE_D_OBJECTS_H */
