/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_gpu.h The 3D objects drawn on the graphics card: the pictures of the vehicles, figures and effects
 * (RasterObject() of three_d_vehicle_object.cpp), with the same camera, light and paint, drawn by the card instead of
 * triangle by triangle on the processor, and read back as the same pictures (the player, 18. 10.: "why do we draw with
 * the processor and not normally with the graphics"; "let us start by moving all the work with 3D objects to the GPU").
 *
 * The card is driven from a thread of its own, with an OpenGL context of its own (EGL under Linux, a hidden window's
 * WGL under Windows), apart from the video driver's: whatever thread wants a picture hands it over and waits. What it
 * needs of an object -- its corners, its triangles by texture, its textures -- is put on the card once and kept by the
 * thing's number (ThreeDUniqueId). Where there is no card to be had, or a picture it cannot hold, the processor draws as
 * before.
 */

#ifndef THREE_D_GPU_H
#define THREE_D_GPU_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ThreeDVehicleShape;
struct ThreeDObjectTexture;

/** A 4x4 matrix, rows first: a point (x, y, z, 1) of a shape to the picture's (x, y pixels, z depth, 1). */
using ThreeDGpuMatrix = std::array<float, 16>;

/**
 * The triangles of a shape drawn, of the parts shown (ThreeDGpuTrianglesOf()): what the card draws a texture at a
 * time, in the order the processor draws them (the first of two at the same depth stays). Made once for a shape and
 * its parts, on whichever thread asks.
 */
struct ThreeDGpuTriangles {
	uint64_t uid = 0;                ///< its own number, for the card
	std::vector<uint32_t> indices;   ///< the corners of the triangles, the opaque first, then the glass
	struct Run {
		uint32_t first = 0;          ///< the first index
		uint32_t count = 0;          ///< and how many
		uint32_t texture = 0;        ///< the texture of these triangles (its index)
	};
	std::vector<Run> opaque;         ///< the triangles drawn through the depth
	std::vector<Run> glass;          ///< the vehicle's glass, drawn see-through after all else
	std::vector<bool> used;          ///< by texture index, whether any triangle drawn has it
};

std::shared_ptr<const ThreeDGpuTriangles> ThreeDGpuTrianglesOf(const ThreeDVehicleShape &shape, uint64_t parts, bool vehicle, size_t textures);

/** A shape drawn into a picture on the card. */
struct ThreeDGpuMesh {
	const ThreeDVehicleShape *shape = nullptr;
	std::shared_ptr<const ThreeDGpuTriangles> triangles;
	ThreeDGpuMatrix matrix{};           ///< its corners to the picture
	std::array<float, 3> light{};       ///< n . light is the light's angle at a corner of normal n (0.55 + 0.45 max(0, n . light))
	bool lit = false;                   ///< lit by its normals; else as its texture is
	std::vector<const ThreeDObjectTexture *> textures; ///< by texture index, painted as drawn; nullptr: those triangles not drawn
};

/** A triangle of the glass of a light on at night, placed on the picture already: drawn in its colour, marked lit. */
struct ThreeDGpuNightTriangle {
	float x[3], y[3], depth[3];
	uint32_t colour;                    ///< Colour::data
};

/** A picture to draw on the card, and what comes of it. */
struct ThreeDGpuPicture {
	int width = 0, height = 0;          ///< pixels
	float depth_near = 0, depth_far = 1; ///< the depths of all drawn lie between these
	float glass_alpha = 0.15f;          ///< how much of the glass's colour a layer of it lays over what is behind
	std::vector<ThreeDGpuMesh> meshes;  ///< the vehicle first, then its crew; the glass of all after all else
	std::vector<ThreeDGpuNightTriangle> night;
	std::vector<uint32_t> pixels;       ///< what comes: the colours (Colour::data), their alpha multiplied in, top row first
	std::vector<uint8_t> lit;           ///< and whether a pixel is of the lit glass of a light (not 0); empty without the night
};

bool ThreeDGpuAvailable();
bool ThreeDGpuRender(ThreeDGpuPicture &pic);
void ThreeDGpuStop();
std::string ThreeDGpuDescribe();
std::string ThreeDGpuTimes();

extern std::atomic<bool> _three_d_gpu_off; ///< the pictures drawn by the processor, as before (the rig: test3d gpu 0)

#endif /* THREE_D_GPU_H */
