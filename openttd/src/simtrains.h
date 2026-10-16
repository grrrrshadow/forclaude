/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file simtrains.h Trains of Microsoft Train Simulator the player puts in newgrf/simtrains/, as vehicles of the game. */

#ifndef SIMTRAINS_H
#define SIMTRAINS_H

#include <string>
#include <vector>

/**
 * An engine of Microsoft Train Simulator (MSTS) the player put in a folder "simtrains" of the NewGRF folders, as he
 * downloaded it (the player, 16. 10.: "a folder simtrains in newgrf, and we put them there; a new vehicle"): its
 * .eng read, and the game makes of it a NewGRF of one engine (PrepareSimtrainsGrfs()) drawn from its model.
 *
 * Nothing of it is ever the game's: the files are the player's own download and the authors'. Their readmes forbid
 * spreading the models or their textures to other simulators, so the game only reads them where the player put
 * them, and the GRF it writes beside them holds the figures of the .eng and no part of the model.
 */
struct SimtrainsEngine {
	std::string folder;           ///< the folder of its .eng, ending with the path separator
	std::string eng;              ///< the .eng's name without ".eng"
	std::string name;             ///< "Name"
	std::string shape;            ///< "WagonShape": the .s, its path
	enum class Traction : uint8_t { Steam, Diesel, Electric } traction = Traction::Diesel;
	double power_kw = 0;          ///< "MaxPower"
	double force_kn = 0;          ///< "MaxForce"
	double speed_kmh = 0;         ///< "MaxVelocity"
	double mass_t = 0;            ///< "Mass"
	bool has_cab = false;         ///< "PassengerCabinHeadPos" given
	float cab[3] = {};            ///< the eye in the cab, metres in the frame of the game's vehicle objects (x forward, y left, z up)
	uint32_t grfid = 0;           ///< the GRF's id, of its .eng's name

	std::string GrfPath() const { return this->folder + this->eng + ".grf"; }
};

/** The engine's number in the GRF made of it: past the original engines, so a new one and not a copy of one. */
static constexpr uint16_t SIMTRAINS_ENGINE_ID = 200;

const std::vector<SimtrainsEngine> &FindSimtrainsEngines();
void PrepareSimtrainsGrfs();

/** A model of MSTS read for drawing, in the frame of the game's vehicle objects (three_d_objects.h): metres, x forward, y left, z up. */
struct SimtrainsMesh {
	std::vector<float> pos;       ///< x, y, z of each corner
	std::vector<float> nrm;       ///< its normal
	std::vector<float> uv;        ///< u, v of each corner
	std::vector<uint32_t> tri;    ///< three corners a triangle
	std::vector<uint8_t> tri_tex; ///< each triangle's texture
	/** A texture: its pixels as RGBA, 4 bytes each; empty when its file cannot be read. */
	struct Texture {
		std::string name;
		int w = 0, h = 0;
		std::vector<uint8_t> rgba;
	};
	std::vector<Texture> textures;
};
bool ReadSimtrainsShape(const std::string &path, SimtrainsMesh &mesh);
bool ReadSimtrainsTexture(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba);

#endif /* SIMTRAINS_H */
