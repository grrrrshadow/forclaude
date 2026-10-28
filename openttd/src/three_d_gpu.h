/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
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

std::shared_ptr<const ThreeDGpuTriangles> ThreeDGpuTrianglesOf(const ThreeDVehicleShape &shape, const ThreeDParts &parts, bool vehicle, size_t textures, bool without_driver_seat = false);

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
	/** The view from the eyes' (THREE_D_GPU_EYE_WHEELS at most): each turning wheel's corners moved so in the shape's own
	 * frame (ThreeDVehicleShape::corner_wheel n: wheels[n - 1]); empty, they stand. */
	std::vector<ThreeDGpuMatrix> wheels;
};
static constexpr size_t THREE_D_GPU_EYE_WHEELS = 32;

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
	bool drawn = false;                 ///< what came: whether the card drew it (ThreeDGpuRender()); not, the processor draws it
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
	bool drawn = false;                   ///< whether the card drew it (ThreeDGpuRenderBuildings()); not, the processor draws it
};

/**
 * A texture of the ground of the turned map on the card (ThreeDGpuGround): the kept picture of the ground facing north
 * (TurnedGround of viewport.cpp), or the picture of a tile with a foundation. The card keeps it by its key; the kept
 * picture of the ground sends only its rects drawn again since it was last laid by the card.
 */
struct ThreeDGpuGroundTexture {
	uint64_t key = 0;                 ///< the same key, the same texture on the card
	uint64_t generation = 0;          ///< another generation: its pixels sent whole, unless the card has the one before
	uint64_t from_generation = 0;     ///< a generation the card may have: moved to from_x, from_y of this one, then the rects sent
	int from_x = 0, from_y = 0;
	int width = 0, height = 0;
	const uint32_t *pixels = nullptr; ///< Colour::data, top row first, width a row
	bool atlas = false;               ///< a small one, kept among others in one texture on the card (a tile's)
	std::vector<std::array<int, 4>> changed; ///< x, y, w, h drawn again since the card last had it (or since from_generation)
};

/** A triangle of the ground: its corners where they land in the block and where they are in their texture, both in pixels. */
struct ThreeDGpuGroundTriangle {
	float x[3], y[3], u[3], v[3];
	uint32_t texture; ///< in ThreeDGpuGround::textures
};

/**
 * The ground of a block of the turned map laid by the card (GPU point 3; DrawTexturedTriangle() of viewport.cpp): the
 * triangles in their order, each over those before, every pixel at its middle the texel under it -- one its texture has
 * nothing for (0) takes the nearest drawn within three --, read back into the block's pixels.
 */
struct ThreeDGpuGround {
	int width = 0, height = 0;   ///< the block, pixels
	uint32_t clear = 0;          ///< what the block holds where no ground is
	std::vector<ThreeDGpuGroundTexture> textures;
	std::vector<ThreeDGpuGroundTriangle> triangles;
	uint32_t *out = nullptr;     ///< the block's pixels, top row first, read back into
	int out_pitch = 0;           ///< pixels a row of them
	/* Or laid into the picture of the view's ground on the card (ThreeDGpuViewGround()), not read back: shown straight
	 * from the card under the screen, the processor leaving a hole for it (the player, 26. 10.: "the card to compose the
	 * turned map"). */
	bool into_view = false;
	int view_x = 0, view_y = 0;  ///< where the block is in it, pixels from its top left
	int view_w = 0, view_h = 0;  ///< its size: the view's (made anew, all black, when another)
	int move_x = 0, move_y = 0;  ///< before the block is laid, what it holds moved by so many pixels (the view moved); a block of no size only moves it
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
	float near_plane = 1;          ///< the nearest drawn, world units ahead (not "near": a macro of windows.h)
	std::vector<ThreeDGpuEyeBuilding> buildings;
	std::vector<ThreeDGpuMesh> meshes; ///< the figures by their triangles (CabDrawFigureModel() of viewport.cpp), each matrix to the card's clip space; their glass after all else
	float glass_alpha = 0.15f;     ///< how much of the glass's colour a layer of it lays over what is behind
	bool read_glow = false;        ///< at night: what has a light of its own read back too (glow)
	std::vector<uint32_t> pixels;  ///< what comes: the colours (Colour::data), top row first; alpha below 255 see-through, its colour multiplied in
	std::vector<float> depth;      ///< and 1 / how far ahead each is, as the eye's depth has it; 0 where nothing
	/** With read_glow, a material with a light of its own by that light alone (drawn again over the dark, as
	 * glow_after_night of viewport.cpp), its alpha the glass's; 0 elsewhere. */
	std::vector<uint32_t> glow;
};

/** A texture the triangles of the eye's frame draw from (ThreeDGpuEyeFrame): its texels as they are (Colour::data), 0 none. */
struct ThreeDGpuEyeTexture {
	const uint32_t *px = nullptr;
	int w = 0, h = 0;
	uint64_t uid = 0; ///< kept on the card by this number for the frames to come; 0: this frame's alone
};

/** A corner of a triangle of the eye's frame, placed on the picture already (CabVertex of viewport.cpp), with how its triangle is drawn (CabRasterRows()). */
struct ThreeDGpuEyeCorner {
	float sx, sy; ///< on the picture, pixels, y down
	float iw;     ///< 1 / how far ahead
	float u, v;   ///< the texel of its texture there
	float light;  ///< its light, 256 as it is (ShadedPixel())
	float alpha;  ///< 256 whole, below see-through
	float how;    ///< THREE_D_GPU_EYE_FILL, THREE_D_GPU_EYE_REPEAT, THREE_D_GPU_EYE_CORNER_LIGHT
};
static constexpr int THREE_D_GPU_EYE_FILL = 1;         ///< a hole of its texture filled from the nearest texel within three (CAB_FILL_HOLES)
static constexpr int THREE_D_GPU_EYE_REPEAT = 2;       ///< its texture repeated beyond its edges (CAB_REPEAT)
static constexpr int THREE_D_GPU_EYE_CORNER_LIGHT = 4; ///< the light of each corner, straight across the picture between them (a thing in the hand)
static constexpr int THREE_D_GPU_EYE_TEXEL_ALPHA = 8;  ///< see-through times its texel's alpha (a material "BLEND", CAB_TEXEL_ALPHA)

/** Triangles of the eye's frame of one texture, in the order they are drawn: corners first to first + count. */
struct ThreeDGpuEyeRun {
	uint32_t texture = 0; ///< ThreeDGpuEyeFrame::textures
	uint32_t first = 0, count = 0;
	bool glass = false;   ///< see-through: laid over what is behind by its alpha, its depth not kept
};

/** A light of the night lighting what is about it on the eye's frame: a lamp's (DrawEyeLampLight()) or a witch's (DrawEyeWitchLight()). */
struct ThreeDGpuEyeLight {
	enum Kind : int { LAMP, LANTERN, TORCH };
	int kind = LAMP;           ///< a house's lamp shines down only; a lantern all round; a torch along its beam
	float x, y, zu;            ///< where, world units from the eye, its height as x and y (CAB_Z_SCALE)
	float reach;               ///< how far it lights, world units
	float r, g, b;             ///< its colour, 0-255
	float dx = 0, dy = 0, dz = 0; ///< a torch's beam's direction, a unit long (dz in metres as dx and dy)
	float cos_edge = 0;        ///< and the cosine of its half angle
	int x0 = 0, y0 = 0, x1 = -1, y1 = -1; ///< the pixels it may reach
};

/** A soft spot of light added to the eye's frame (AddNightLight() of viewport.cpp), hidden behind what is nearer than its middle. */
struct ThreeDGpuEyeGlow {
	float sx, sy, rx, ry;  ///< its middle and half sizes, pixels
	float r, g, b;         ///< its colour, 0-255
	float strength;
	float iw;              ///< its middle's 1 / how far ahead: hidden where the picture is nearer there by more than hidden_by; 0 never hidden
	float hidden_by = 1.05f;
	float lamp = 0;        ///< 1: a lamp's (a car's light, a window): none of it over what is drawn nearer than the lamp; 0: a pool of light, all of it
};

/** A ball of smoke or a blast laid over the eye's frame (DrawEyeBlasts()), hidden by the pixel behind what is nearer than its middle. */
struct ThreeDGpuEyeBlast {
	float sx, sy, radius, iw;
	float r, g, b, alpha;
};

/** A star of the night sky of the eye's frame (DrawEyeSky()): a pixel, and a little cross for a bright one. */
struct ThreeDGpuEyeStar {
	int x, y;
	float r, g, b; ///< 0-255
	int a;         ///< how much of its colour is added, 255ths
	bool cross;
};

/**
 * A step of the eye's frame drawn on the card (ThreeDGpuEyeFrame), as a pass or a job of the processor's frame
 * (EyeFrame of viewport.cpp) draws it: what is drawn in its order, and the jobs over the whole picture as the
 * processor computes them, pixel by pixel.
 */
struct ThreeDGpuEyeStep {
	enum class Kind : uint8_t {
		Background,     ///< the sky down to the horizon (horizon), the ground under it
		Triangles,      ///< runs first_run to first_run + runs
		Objects,        ///< the buildings and the figures by their triangles (objects); their glass after the triangles that follow
		Lay,            ///< a picture of its own laid in where it is and nearer (a tunnel seen through its portal): lay_px, lay_z
		Haze,           ///< the far fading into colour from from on, over depth (the haze job of CollectEyeFrame())
		FarHaze,        ///< the haze far off: from from to to, the ground under the horizon all of it
		Shadows,        ///< the sun's shadows of the buildings (DrawEyeShadows()): shadow_map
		Night,          ///< the picture darkened by colour 256ths, then the glows
		Glows,          ///< glows added
		GlowAfterNight, ///< the figures' own light again over the dark, where they are still what is seen
		Lamps,          ///< lights lighting what is about them (lights), each pixel's own colour out of the dark by colour (the night's 256ths)
		Sky,            ///< the stars and the moon where nothing is drawn above the horizon
		Blasts,         ///< blasts laid over
	};
	Kind kind = Kind::Triangles;
	size_t first_run = 0, runs = 0;
	std::shared_ptr<ThreeDGpuEyePicture> objects;
	std::vector<uint32_t> lay_px;
	std::vector<float> lay_z;
	std::array<float, 3> colour{};
	float from = 0, to = 0, depth = 0, horizon = 0;
	std::vector<ThreeDGpuEyeGlow> glows;
	std::vector<ThreeDGpuEyeLight> lights;
	std::vector<ThreeDGpuEyeBlast> blasts;
	std::vector<ThreeDGpuEyeStar> stars;
	std::array<float, 3> moon{};     ///< the moon on the sky: x, y, radius, pixels; radius 0 not seen
	/** The map of the shadows (CabShadowMap of viewport.cpp): kept on the card by key. */
	struct ShadowMap {
		uint64_t key = 0;
		int nu = 0, nv = 0;
		const float *nearest = nullptr;
		float u0 = 0, v0 = 0, texel = 1, bias = 0; ///< u0, v0 from the eye
		float sun0 = 0;                            ///< how near to the sun the eye is (the map's nearest from it)
		std::array<float, 3> sun{}, a{}, b{};
	} shadow_map;
};

/**
 * The whole view from the eyes drawn on the card (the player, 20. 10.: "draw as it is to be drawn, so that it does not
 * jerk"): its steps in their order, the triangles' corners and textures they point to, and what comes of it.
 */
struct ThreeDGpuEyeFrame {
	int width = 0, height = 0;
	float near_plane = 1;            ///< the nearest drawn, world units ahead
	std::array<float, 10> eye{};     ///< x, y, z, fx, fy, rx, ry, focal, cx, cy (CabEye of viewport.cpp)
	std::vector<ThreeDGpuEyeTexture> textures;
	std::vector<ThreeDGpuEyeCorner> corners;
	std::vector<ThreeDGpuEyeRun> runs;
	std::vector<ThreeDGpuEyeStep> steps;
	bool show = false;               ///< to be shown straight from the card (ThreeDGpuShows()): no pixels read back
	std::vector<uint32_t> pixels;    ///< what comes: the colours (Colour::data), top row first; or
	uint32_t shown = 0;              ///< the texture of the video driver's OpenGL it is in, top row first (shown)
};

/**
 * A picture of a thing standing on the turned map as the card lays it (ThreeDGpuStanding): its colours with their alpha
 * multiplied in (Colour::data, what it gives over black), top row first; a pixel lit at night (a window, a lamp's
 * glass: the night does not darken it) whole, of alpha THREE_D_GPU_LIT_ALPHA.
 */
struct ThreeDGpuStandingPicture {
	int width = 0, height = 0;
	std::vector<uint32_t> pixels;
};
static constexpr uint8_t THREE_D_GPU_LIT_ALPHA = 254;

/**
 * A building of the turned map drawn by the card among what stands on a block (ThreeDGpuStandingItem::building), as
 * DrawObjectInto() of viewport.cpp draws it: its triangles through a depth of its own -- cleared over its rectangle
 * first, unless it is a part drawn with the one before (the farm's hay, the doors) --, the figures on it after them, at
 * night its lit glass over them.
 */
struct ThreeDGpuStandingBuilding {
	ThreeDGpuBuilding building;
	ThreeDGpuMatrix matrix{};             ///< a corner (world units from its north corner) to the block: x, y pixels, z how near, 0-1
	bool clear = true;                    ///< its depth cleared first over clear_x, clear_y, clear_w, clear_h (pixels of the block)
	int clear_x = 0, clear_y = 0, clear_w = 0, clear_h = 0;
	/** At night its lit glass: each triangle's corners on the block already -- x, y pixels, z how near (0-1, as the
	 * matrix's), u, v of its texture (0-1), 256 --, drawn as they are where they are nearer, marked lit. */
	std::vector<float> night;
	std::vector<uint8_t> night_texture;   ///< each such triangle's texture (ThreeDGpuBuilding::textures)
};

/**
 * A vehicle of the turned map drawn by the card among what stands on a block, with no picture of it made (the player,
 * 25. 10.: "finish the move to the GPU"): its shapes as its picture would have them (PrepareGpuObject() of
 * three_d_vehicle_object.cpp), drawn at supersample times its size into a picture of the card's own, shrunk from it
 * there as the processor shrinks the picture into the pictures kept on the card, and laid from them as a picture is;
 * nothing read back. Looking the same in another block or frame (key), it is laid from them again, not drawn.
 */
struct ThreeDGpuStandingVehicle {
	ThreeDGpuPicture pic;      ///< its shapes, at supersample times the block's pixels (its size whole multiples of it)
	uint64_t key = 0;          ///< what it looks like (the object, its angles, its state): shrunk once, kept on the card by it among the pictures
	int supersample = 1;
	int left = 0, top = 0;     ///< its top left from the world point it hangs from, pixels of the block
	int width = 0, height = 0; ///< pixels of the block
	std::vector<std::shared_ptr<const ThreeDObjectTexture>> held; ///< what its paints need, kept till it is drawn
};

/**
 * The faces of a box of the turned map -- a house of the original game, an industry's building, a slab of a station, a
 * bridge, a depot -- laid by the card among what stands on a block as DrawTexturedTriangle() of viewport.cpp lays them:
 * each pixel the texel under it of the thing's sprites drawn north-up, whole -- one they have nothing for (0) the
 * nearest drawn within three, when fill --, each triangle over those before.
 */
struct ThreeDGpuStandingFaces {
	std::shared_ptr<const ThreeDGpuStandingPicture> texture; ///< its sprites drawn north-up as they are (Colour::data, 0 nothing)
	uint64_t key = 0;           ///< kept on the card by it among the pictures; 0 this block's alone
	std::vector<float> corners; ///< x, y on the block, u, v in the texture, pixels; three a triangle
	bool fill = true;
};

/** A thing standing on a block of the turned map: its picture laid at its place, or a building, a vehicle or a box drawn there. */
struct ThreeDGpuStandingItem {
	int x = 0, y = 0; ///< its top left in the block, pixels
	uint64_t key = 0; ///< kept on the card by this (a sprite's picture); 0 this block's alone
	std::shared_ptr<const ThreeDGpuStandingPicture> picture;
	/** A picture among the triangles of the building before it (a figure on it): how near it is, as that building's
	 * depth (0-1); laid where it is nearer, its depth kept. Below 0: laid over all. */
	float depth = -1;
	int building = -1; ///< not a picture: the building of ThreeDGpuStanding::buildings drawn here
	std::shared_ptr<const ThreeDGpuStandingVehicle> vehicle = nullptr; ///< not a picture: this vehicle drawn here, laid over all
	int faces = -1;   ///< not a picture: the box of ThreeDGpuStanding::faces drawn here
};

/**
 * What stands on a block of the turned map, laid by the card into the picture of the view's standing things, far
 * first, each over those before; shown straight from the card over the view's ground (ThreeDGpuViewGround()) and under
 * the screen (the player, 26. 10.: "the card to compose the turned map", step 2). The block cleared first.
 */
struct ThreeDGpuStanding {
	int view_x = 0, view_y = 0;  ///< where the block is in the picture of the view, pixels from its top left
	int width = 0, height = 0;   ///< the block
	int view_w = 0, view_h = 0;  ///< the view's (as its ground's)
	std::vector<ThreeDGpuStandingItem> items;
	std::vector<ThreeDGpuStandingBuilding> buildings;
	std::vector<ThreeDGpuStandingFaces> faces;
	/** At night: which pixels of the block are lit once all is laid (the glows of the lights only where they are
	 * seen, DrawNight() of viewport.cpp), read back into lit, a byte a pixel, top row first. */
	bool read_lit = false;
	std::vector<uint8_t> lit;
};

bool ThreeDGpuAvailable(bool wait = false);
bool ThreeDGpuReady();
void ThreeDGpuCheckLastRun();
bool ThreeDGpuRender(std::span<ThreeDGpuPicture *const> pics);
bool ThreeDGpuRenderBuildings(std::span<ThreeDGpuBuildingPicture *const> pics);
bool ThreeDGpuDrawGround(ThreeDGpuGround &ground);
uint32_t ThreeDGpuViewGround(uint64_t *made = nullptr);
bool ThreeDGpuDrawGroundStart(ThreeDGpuGround &ground);
bool ThreeDGpuDrawGroundTake();
bool ThreeDGpuDrawStanding(ThreeDGpuStanding &standing);
uint32_t ThreeDGpuViewStanding(uint32_t *lit = nullptr);
void ThreeDGpuViewPicturesWait(void (*wait)(void *fence));
bool ThreeDGpuRenderEye(ThreeDGpuEyePicture &pic);
bool ThreeDGpuRenderEyeFrame(ThreeDGpuEyeFrame &frame);
void ThreeDGpuVideoContext(void *display, void *context, bool shared_with_card = false);
void ThreeDGpuVideoContextGone();
void *ThreeDGpuOwnContext();
bool ThreeDGpuShows();
bool ThreeDGpuReadShown(uint32_t texture, int width, int height, std::vector<uint32_t> &pixels);
void ThreeDGpuStop();
std::string ThreeDGpuDescribe();
std::string ThreeDGpuTimes();
size_t ThreeDGpuHeldMB();

extern std::atomic<bool> _three_d_gpu_show; ///< the view from the eyes shown straight from the card where it can (ThreeDGpuShows()); the rig's test3d gpu ukaz 0: read back
extern std::atomic<bool> _three_d_gpu_off; ///< the pictures drawn by the processor, as before: the setting gui.three_d_gpu off (by default), or the rig's test3d gpu 0
extern std::atomic<bool> _three_d_gpu_driver_waits;
extern std::atomic<bool> _gpu_building_cells_on; ///< the video driver's context waits on the card for the view's pictures (ThreeDGpuViewPicturesWait()): the card fences them, not glFinish()

#endif /* THREE_D_GPU_H */
