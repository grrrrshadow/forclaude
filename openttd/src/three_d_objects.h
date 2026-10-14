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
 */

#ifndef THREE_D_OBJECTS_H
#define THREE_D_OBJECTS_H

#include "house_type.h"

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

const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot);
const ThreeDObject *GetThreeDHouseObjectAny(HouseID north_house, uint colour_slot);
std::string DescribeThreeDObjects();

#endif /* THREE_D_OBJECTS_H */
