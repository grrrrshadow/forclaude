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

/**
 * A car of a mod of BeamNG.drive as the player has it: a .zip in the mods folder of BeamNG (or one unpacked there),
 * in a folder the player names ("beamng_path"), or in newgrf/beamng/ of the game's folders (the player, 16. 10.:
 * "shall we drive mods of BeamNG?"). Its folder vehicles/<name>/ read, the game makes of it a NewGRF of one road
 * vehicle (PrepareBeamngGrfs()) in newgrf/beamng/, drawn from its model.
 *
 * Nothing of it is ever the game's: the mods are the player's own downloads and their authors', each under its own
 * licence (the official cars of BeamNG may not go to another game at all). So the game only reads them where they
 * are, and the GRF it writes holds the figures of the mod and no part of the model.
 */
struct BeamngVehicle {
	std::string zip;              ///< the .zip it is in, or empty for an unpacked mod
	std::string folder;           ///< its folder: in the .zip "vehicles/<name>/", else the path of that folder with the separator
	std::string id;               ///< the name of its folder
	std::string name;             ///< "Name" of its info.json, with "Brand" before it
	std::string author;           ///< "Author"
	std::vector<uint32_t> paints; ///< the colours of its info.json ("colors", "paints"), Colour::data; the default first
	double power_kw = 0;          ///< the most of its engine's torque curve
	double speed_kmh = 0;         ///< as fast as its gearing and its power against the air let it
	double mass_t = 0;            ///< the weights of its nodes
	double effort = 0;            ///< the most force at its wheels against its weight
	bool goods = false;           ///< a lorry: goods of any kind, not passengers
	int capacity = 4;             ///< the passengers it takes, or the goods
	bool has_cab = false;         ///< a camera of the driver given
	float cab[3] = {};            ///< the eye in the cab, metres in the frame of the game's vehicle objects (x forward, y left, z up)
	uint32_t grfid = 0;           ///< the GRF's id, of its .zip's and folder's names

	std::string grf_path;         ///< where the GRF made of it is: newgrf/beamng/ of the player's own folder
	std::string Package() const { return this->zip.empty() ? this->folder : this->zip; }
};

/** The car's number in the GRF made of it: past the original road vehicles, so a new one and not a copy of one. */
static constexpr uint16_t BEAMNG_ENGINE_ID = 200;

extern std::string _beamng_path;
const std::vector<BeamngVehicle> &FindBeamngVehicles();
void PrepareBeamngGrfs();

/** A car's model read for drawing, in the frame of the game's vehicle objects (three_d_objects.h): metres, x forward, y left, z up, z 0 the ground. */
struct BeamngMesh {
	std::vector<float> pos;       ///< x, y, z of each corner
	std::vector<float> nrm;       ///< its normal
	std::vector<float> uv;        ///< u, v of each corner
	std::vector<uint32_t> tri;    ///< three corners a triangle
	std::vector<uint8_t> tri_tex; ///< each triangle's material: 0 the paint (white, painted in the car's colour)
	/** A material: its texture as RGBA, 4 bytes a pixel, or its colour alone. */
	struct Material {
		std::string name;
		int w = 0, h = 0;
		std::vector<uint8_t> rgba;
		std::vector<uint8_t> png; ///< or its texture a PNG, as it is
		uint32_t flat = 0;        ///< its colour (Colour::data) without a texture
	};
	std::vector<Material> materials;
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
