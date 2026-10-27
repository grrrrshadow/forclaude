/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file beamng.h Cars of BeamNG.drive mods the player has, as road vehicles of the game. */

#ifndef BEAMNG_H
#define BEAMNG_H

#include <string>
#include <vector>

/** The first number of the cars in the GRF made of a car's folder: past the original road vehicles, so new ones and not copies. */
static constexpr uint16_t BEAMNG_ENGINE_ID = 200;

/**
 * A car of the purchase list made of a configuration: one for most, but for our Škoda 1203 one for each of its makes
 * over the years (the player, 26. 10.: "the Škoda 1203, then the TAZ 1203 and at last the TAZ 1500 and the 1.9 D").
 */
struct BeamngModel {
	std::string name;               ///< its name in the purchase list
	uint16_t id = BEAMNG_ENGINE_ID; ///< its number in the GRF of its folder
	int make = -1;                  ///< which of the 1203's makes it is, -1 none
	int year_from = 0;              ///< the year it comes, 0 there in every game; never retired
	double power_kw = 0;
	double speed_kmh = 0;
};

/**
 * A car of a mod of BeamNG.drive as the player has it: a .zip in the mods folder of BeamNG (or one unpacked there),
 * in a folder the player names ("beamng_path"), or in newgrf/beamng/ of the game's folders (the player, 16. 10.:
 * "shall we drive mods of BeamNG?"). Its folder vehicles/<name>/ read, the game makes of it a NewGRF of road vehicles,
 * one for each of its configurations (PrepareBeamngGrfs()), in newgrf/beamng/, drawn from its model.
 *
 * Nothing of it is ever the game's: the mods are the player's own downloads and their authors', each under its own
 * licence (the official cars of BeamNG may not go to another game at all). So the game only reads them where they
 * are, and the GRF it writes holds the figures of the mod and no part of the model.
 */
struct BeamngVehicle {
	std::string zip;              ///< the .zip it is in, or empty for an unpacked mod
	std::string folder;           ///< its folder: in the .zip "vehicles/<name>/", else the path of that folder with the separator
	std::string id;               ///< the name of its folder
	std::string config;           ///< its configuration, a .pc of the folder without ".pc" (the 1203's police, flatbed, van...); empty its default one
	std::string name;             ///< "Name" of its info.json, with "Brand" before it
	std::string author;           ///< "Author"
	std::vector<uint32_t> paints; ///< the colours of its info.json ("colors", "paints"), Colour::data; the default first
	double power_kw = 0;          ///< the most of its engine's torque curve
	double speed_kmh = 0;         ///< as fast as its gearing and its power against the air let it
	double mass_t = 0;            ///< the weights of its nodes
	double effort = 0;            ///< the most force at its wheels against its weight
	bool goods = false;           ///< a lorry: goods of any kind, not passengers
	int capacity = 4;             ///< the passengers it takes, or the goods
	std::vector<std::string> cargoes; ///< the labels of the cargoes it may be refitted to, the first its own; empty every cargo of the game
	bool has_cab = false;         ///< a camera of the driver given
	float cab[3] = {};            ///< the eye in the cab, metres in the frame of the game's vehicle objects (x forward, y left, z up)
	bool has_bed = false;         ///< a flatbed: the game's shared loads lie on its bed (ThreeDLoadBed), the piece of its cargo
	float bed_x[2] = {};          ///< its bed from its back to its front, metres in that frame
	float bed_y[2] = {};          ///< from side to side
	float bed_floor = 0;          ///< the height of its floor
	float bed_sides = 0;          ///< how high its sides are over the floor
	float bed_top = 0;            ///< no piece of a load higher: the top of its tarp; 0 none
	bool has_tarp = false;        ///< the tarp of its bed is a part of its shape (BeamngMesh::PART_TARP), over a load of no pieces
	int year_from = 0;            ///< "Years" of its info.json: the first year made, 0 none given
	std::vector<BeamngModel> models; ///< the cars of the purchase list made of it
	uint32_t grfid = 0;           ///< the GRF's id, of its .zip's and folder's names: one GRF a folder, all its configurations in it

	std::string grf_path;         ///< where the GRF made of its folder is: newgrf/beamng/ of the player's own folder
	std::string Package() const { return this->zip.empty() ? this->folder : this->zip; }
};

extern std::string _beamng_path;
const std::vector<BeamngVehicle> &FindBeamngVehicles();
void PrepareBeamngGrfs();

/** A car's model read for drawing, in the frame of the game's vehicle objects (three_d_objects.h): metres, x forward, y left, z up, z 0 the ground. */
struct BeamngMesh {
	std::vector<float> pos;       ///< x, y, z of each corner
	std::vector<float> nrm;       ///< its normal
	std::vector<float> uv;        ///< u, v of each corner
	std::vector<uint32_t> tri;    ///< three corners a triangle
	std::vector<uint8_t> tri_tex; ///< each triangle's material: 0 the paint (the painted materials side by side, painted in the car's colour)
	std::vector<int16_t> tri_cell; ///< each triangle of the paint: its painted material (paint_sources), -1 none; the triangles after it none
	std::vector<int8_t> tri_facing; ///< each triangle's outer side: 1 as its corners go round, -1 the other way, 0 seen from both sides (a material "doubleSided"); the triangles after it both
	static constexpr uint8_t PART_TARP = 1; ///< tri_part: the tarp over a flatbed's bed (BeamngVehicle::has_tarp)
	std::vector<uint8_t> tri_part; ///< each triangle's part: 0 always, PART_TARP; the triangles after it 0, empty all 0
	/** A material: its texture as RGBA, 4 bytes a pixel, or its colour alone. */
	struct Material {
		std::string name;
		int w = 0, h = 0;
		std::vector<uint8_t> rgba;
		std::vector<uint8_t> png; ///< or its texture a PNG, as it is
		uint32_t flat = 0;        ///< its colour (Colour::data) without a texture
		uint8_t alpha = 255;      ///< how much of what is behind it hides at most (ThreeDVehicleShape::tex_alpha): below 255 see-through (the glass)
		/** The glass's "opacityMap": how much of what is behind it hides where (its red), RGBA of ow x oh or a PNG; none empty. */
		std::vector<uint8_t> opacity_rgba, opacity_png;
		int ow = 0, oh = 0;
	};
	std::vector<Material> materials;
	/**
	 * A material in the car's colour (BeamNG's "instanceDiffuse"): the picture of its first layer, its rubber, chrome and
	 * lamps, and the mask of its paint over it, the colour where it is light (none: all over). The paint's texture is
	 * them side by side, a square of PaintSide() a side in their order, each triangle of the paint in its own's cell.
	 */
	struct PaintSource {
		std::string name;
		int w = 0, h = 0, mw = 0, mh = 0;
		std::vector<uint8_t> rgba, png, mask_rgba, mask_png;
	};
	std::vector<PaintSource> paint_sources;
	static int PaintSide(size_t sources)
	{
		int n = 1;
		while (static_cast<size_t>(n) * n < sources) n++;
		return n;
	}
	/** The glass of its lights ("glowMap" of the mod): its triangles, red for those at the back. */
	std::vector<float> night_pos;
	std::vector<uint32_t> night_tri;
	std::vector<uint32_t> night_colour;
	std::vector<uint8_t> night_kind;
	/** Where its headlights and its lights at the back shine from (its "props" of lights), x, y, z each. */
	std::vector<float> front_lights, back_lights;
};
bool ReadBeamngShape(const BeamngVehicle &vehicle, BeamngMesh &mesh);
const BeamngVehicle *GetBeamngVehicle(size_t index);

#endif /* BEAMNG_H */
