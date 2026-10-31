/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file simtrains.h Trains of Microsoft Train Simulator the player puts in newgrf/simtrains/, as vehicles of the game. */

#ifndef SIMTRAINS_H
#define SIMTRAINS_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * A vehicle of Microsoft Train Simulator (MSTS) as the player downloaded it, where such trains are kept: the
 * Trains/Trainset folder of MSTS (or of Open Rails, or the folder the player names, "simtrains_path"; the player,
 * 16. 10.: "let the game look for the simtrains models in their usual folder"). Its .eng or .wag read, the game makes
 * of it a NewGRF of one vehicle (PrepareSimtrainsGrfs()) in newgrf/simtrains/, drawn from its model. Each player
 * downloads their own trains; the GRF holds the figures of the .eng, the model is read from the player's files.
 */
struct SimtrainsEngine {
	std::string folder;           ///< the folder of its .eng, ending with the path separator
	std::string eng;              ///< the .eng's (or .wag's) name without its extension
	std::string name;             ///< "Name"
	std::string shape;            ///< "WagonShape": the .s, its path
	enum class Traction : uint8_t { Steam, Diesel, Electric } traction = Traction::Diesel;
	double power_kw = 0;          ///< "MaxPower"
	double force_kn = 0;          ///< "MaxForce"
	double speed_kmh = 0;         ///< "MaxVelocity"
	double mass_t = 0;            ///< "Mass"
	bool wagon = false;           ///< a wagon (a .wag): no power, carries
	bool passengers = false;      ///< a wagon's "Type ( Carriage )": passengers, else goods of any kind
	int capacity = 0;             ///< a wagon's: "PassengerCapacity", or a goods wagon's own
	bool has_cab = false;         ///< "PassengerCabinHeadPos" given
	float cab[3] = {};            ///< the eye in the cab, metres in the frame of the game's vehicle objects (x forward, y left, z up)
	uint32_t grfid = 0;           ///< the GRF's id, of its .eng's name
	double length_m = 0;          ///< the length of its "Size", 0 when not given
	int parts = 1;                ///< the vehicles of the game it is made of, an articulated one when longer than one (ReadSize())

	std::string grf_path;         ///< where the GRF made of it is: newgrf/simtrains/ of the player's own folder
	std::string cvf;              ///< an engine's cab, its "CabView" .cvf (simtrains_cab.h); empty none
};

/** The engine's number in the GRF made of it: past the original engines, so a new one and not a copy of one. Its
 * articulated parts are the numbers after it. */
static constexpr uint16_t SIMTRAINS_ENGINE_ID = 200;

extern std::string _simtrains_path;
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

/**
 * The cab of an engine of MSTS, its .cvf (the player, 31. 10.: "the view from the cab, the realistic controls and the
 * simulation of the driving and the cabs of the simtrains models"): pictures of the driver's desk with the windows left
 * clear, each seen in one direction, and the controls drawn over them. Places are of a screen 640 by 480, as MSTS has
 * them, whatever the size of the pictures.
 */
struct SimtrainsCab {
	/** A picture of the desk ("CabViewFile"), and where it is seen. */
	struct View {
		std::string image;       ///< its .ace by day
		std::string night;       ///< by night: the same in the folder "noc" or "night" beside, empty none
		float yaw = 0;           ///< degrees it is turned from ahead, to the right positive ("Direction", the second)
		float pitch = 0;         ///< and down ("Direction", the first)
		float eye[3] = {};       ///< the driver's eye ("Position"), metres in the frame of the game's objects (x forward, y left, z up)
	};
	/** A control, a gauge or a lamp of the desk drawn over the first picture ("CabViewControls"). */
	struct Control {
		std::string kind;        ///< "Dial", "Gauge", "Lever", "TwoState", "TriState", "MultiStateDisplay", "CombinedControl", "Digital", "DigitalClock", "CabSignalDisplay", ...
		std::string type;        ///< what it shows or does, the first word of "Type": "SPEEDOMETER", "TRAIN_BRAKE", "THROTTLE", ...
		int x = 0, y = 0, w = 0, h = 0; ///< "Position", of the screen 640 by 480
		std::string graphic;     ///< "Graphic": its .ace by day, empty none
		std::string graphic_night; ///< by night
		std::string style;       ///< "Style": "NEEDLE", "POINTER", "SPRUNG", "ONOFF", "PRESSED", "WHILE_PRESSED", ...
		float min = 0, max = 1;  ///< "ScaleRange"
		float from = 0, to = 0;  ///< a dial's "ScalePos": degrees of the needle at min and at max
		int pivot = 0;           ///< a dial's "Pivot": pixels from the needle's top to its axis
		int dir_increase = 0;    ///< "DirIncrease": 1 the other way round
		int orientation = 0;     ///< "Orientation": a gauge's 1 upright
		int area[4] = {};        ///< a gauge's "Area"
		int frames = 0, cols = 0, rows = 0; ///< "NumFrames": how many frames, in how many columns and rows of its graphic
		std::vector<int> positions;   ///< "NumPositions": the frame of each position (the count first is left out)
		std::vector<float> values;    ///< "NumValues": the value of each position
		bool mouse = false;      ///< "MouseControl": the mouse moves it
		uint32_t colour = 0;     ///< the first "ControlColour" of a gauge or a figure, 0xRRGGBB
	};
	std::string path;            ///< the .cvf
	std::vector<View> views;     ///< the first ahead
	std::vector<Control> controls;
};
bool ReadSimtrainsCab(const std::string &path, SimtrainsCab &cab);
bool ReadSimtrainsCabEye(const std::string &path, float eye[3]);
bool ReadSimtrainsTexture(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba);

#endif /* SIMTRAINS_H */
