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
#include <span>
#include <string>
#include <vector>

struct ThreeDVehicleShape;
struct ThreeDObjectTexture;
struct ThreeDDetailMask;
struct ThreeDParts;

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
		int8_t facing = 0;           ///< a building's: the side seen, 1 where the corners go round clockwise on the picture, -1 the other, 0 both
	};
	std::vector<Run> opaque;         ///< the triangles drawn through the depth
	std::vector<Run> glass;          ///< the vehicle's glass, drawn see-through after all else
	std::vector<bool> used;          ///< by texture index, whether any triangle drawn has it
};

std::shared_ptr<const ThreeDGpuTriangles> ThreeDGpuTrianglesOf(const ThreeDVehicleShape &shape, const ThreeDParts &parts, bool vehicle, size_t textures);

/**
 * How the card paints a texture's texels as it draws them -- as RasterObject() paints them on the processor, each step
 * only where it has what it needs: the details in the vehicle's colours of its set (a detail's mask index, and the
 * colours of its indices: detail_colours), the body's paint coloured and faded, an add-on's colour, an effect's tint,
 * the rust. Colours 0-255. The masks are a level of detail as the texture, their grey in the red; the textures drawn
 * as they are when nothing paints them.
 */
struct ThreeDGpuPaint {
	const ThreeDDetailMask *detail = nullptr;      ///< the mask index of each texel, nullptr: no details painted
	/** By mask index (the rows) the detail's colour at each lightness (the columns), its alpha 255; a detail turned
	 * (ShiftTrueColourHue()) alpha 128 and in its first texel its turn + 128, its saturation and value; else alpha 0. */
	const ThreeDObjectTexture *detail_colours = nullptr;
	bool body = false;                                    ///< the body's paint: where paint_mask is grey, or all of it
	const ThreeDObjectTexture *paint_mask = nullptr;
	bool coloured = false;                                ///< the paint's colour at the texture's lightness (start)
	std::array<float, 3> colour{}, target{};              ///< the paint's colour; what it fades to
	float start = 255, fade = 0;
	const ThreeDObjectTexture *addon_mask = nullptr;      ///< an add-on's colour where it is grey, nullptr none
	std::array<float, 3> addon_colour{};
	float addon_start = 255;
	bool tinted = false;                                  ///< multiplied by tint (an effect's colour)
	std::array<float, 3> tint{};
	const ThreeDObjectTexture *rust_mask = nullptr;       ///< the rust where it is grey, nullptr none
	std::array<float, 3> rust_colour{};
	float rust_time = 0, rust_ramp = 1;
	bool rust_gradual = false;                            ///< the grey is when it starts (else how much)

	/** Whether it paints anything. */
	bool Any() const { return this->detail != nullptr || this->body || this->addon_mask != nullptr || this->tinted || this->rust_mask != nullptr; }
};

/** A shape drawn into a picture on the card. */
struct ThreeDGpuMesh {
	const ThreeDVehicleShape *shape = nullptr;
	std::shared_ptr<const ThreeDGpuTriangles> triangles;
	ThreeDGpuMatrix matrix{};           ///< its corners to the picture
	std::array<float, 3> light{};       ///< n . light is the light's angle at a corner of normal n (0.55 + 0.45 max(0, n . light))
	bool lit = false;                   ///< lit by its normals; else as its texture is
	std::vector<const ThreeDObjectTexture *> textures; ///< by texture index, as they are; nullptr: those triangles not drawn
	std::vector<ThreeDGpuPaint> paints; ///< by texture index, how it is painted as drawn; empty or shorter: as it is
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
	std::vector<uint8_t> lit;           ///< and whether a pixel is lit (not 0): the glass of a light on, a material's own light; empty when not read_lit
	bool read_lit = false;              ///< whether the lit pixels are read back: at night, or with a material's own light
};

/**
 * A building's triangles drawn into a picture on the card, as DrawObjectInto() of viewport.cpp draws them for the
 * turned view (the player, 18. 10.: the houses on the card): each pixel its texture's texel -- a hole filled from the
 * nearest within three texels, as the processor fills it --, shaded by the light of the corners about it; the side
 * facing away left out; the nearer kept. What comes is its pixels and how near each is, for the figures standing on it.
 */
struct ThreeDGpuBuilding {
	uint64_t uid = 0;                     ///< its shape's number (ThreeDObjectShapeView::uid)
	std::span<const float> pos;           ///< its corners, world units from its north corner
	std::span<const float> uv;            ///< their u, v
	std::span<const uint16_t> light;      ///< their light, 256 as it is; empty: all 256
	std::span<const uint32_t> tri;
	std::span<const int8_t> facing;       ///< each triangle's side facing out (1 its corners' order, -1 the other, 0 both); empty: both
	std::span<const uint8_t> tri_tex;     ///< each triangle's texture; empty: all the first
	std::vector<const ThreeDObjectTexture *> textures;
	bool cull = true;                     ///< the sides facing away left out (the rig: test3d zadni 0 draws them)
};

/** A picture of a building to draw on the card, and what comes of it. */
struct ThreeDGpuBuildingPicture {
	int width = 0, height = 0;            ///< pixels
	ThreeDGpuMatrix matrix{};             ///< a corner to the picture: x, y pixels, z how near (the turned view's depth, the larger nearer)
	float depth_near = 0, depth_far = 1;  ///< the depths of all its corners lie between these
	ThreeDGpuBuilding building;
	std::vector<uint32_t> pixels;         ///< what comes: its colours (Colour::data), 0 where it is not; top row first
	std::vector<float> depth;             ///< and how near each pixel is, -infinity where it is not
};

/**
 * A building of the view from the eyes on the card (ThreeDGpuEyePicture): its triangles as the processor draws them for
 * the view (the houses' job of the eye's frame, viewport.cpp) -- each pixel its texture's texel, none where the texel is
 * 0 or off the texture, shaded by its triangle's light (its corners' mean), the side facing away left out.
 */
struct ThreeDGpuEyeBuilding {
	ThreeDGpuBuilding building;
	ThreeDGpuMatrix matrix{}; ///< its corners (world units from its north corner) to the card's clip space: w how far ahead
};

/**
 * The objects of the view from the eyes drawn on the card at once (the player, 20. 10.: "from the eyes it is to be
 * drawn by the GPU, all the objects by the GPU"), the nearer kept; what comes is their pixels and how near each is,
 * laid into the eye's picture where they are nearer than what is there.
 */
struct ThreeDGpuEyePicture {
	int width = 0, height = 0;     ///< pixels
	float near = 1;                ///< the nearest drawn, world units ahead
	std::vector<ThreeDGpuEyeBuilding> buildings;
	std::vector<ThreeDGpuMesh> meshes; ///< the figures by their triangles (CabDrawFigureModel() of viewport.cpp), each matrix to the card's clip space; their glass after all else
	float glass_alpha = 0.15f;     ///< how much of the glass's colour a layer of it lays over what is behind
	std::vector<uint32_t> pixels;  ///< what comes: the colours (Colour::data), top row first; alpha below 255 see-through, its colour multiplied in
	std::vector<float> depth;      ///< and 1 / how far ahead each is, as the eye's depth has it; 0 where nothing
};

bool ThreeDGpuAvailable();
void ThreeDGpuCheckLastRun();
bool ThreeDGpuRender(ThreeDGpuPicture &pic);
bool ThreeDGpuRenderBuilding(ThreeDGpuBuildingPicture &pic);
bool ThreeDGpuRenderEye(ThreeDGpuEyePicture &pic);
void ThreeDGpuStop();
std::string ThreeDGpuDescribe();
std::string ThreeDGpuTimes();

extern std::atomic<bool> _three_d_gpu_off; ///< the pictures drawn by the processor, as before: the setting gui.three_d_gpu off (by default), or the rig's test3d gpu 0

#endif /* THREE_D_GPU_H */
