/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_model.cpp The game's buildings rendered from eight directions
 * with depth and normals (ThreeDModel, table/three_d_models.h), drawn turned
 * to any angle between the renders.
 *
 * Every pixel of a render is a world point: the camera that took it says where
 * (ThreeDModelView), and the depth sprite says how far along the line of
 * sight. So the eight renders are eight point clouds in one frame, the
 * building's, and the view at any angle is drawn from the two renders either
 * side of it: each point of each is put through the game's turned projection
 * (ProjectTurnedF()) and splat where it lands, the nearest point of a render
 * winning its pixel; where both renders have a point at a pixel within
 * SAME_SURFACE_M of each other along the line of sight it is the same surface
 * and their colours mix, by the angle (the nearer render weighs more) and by
 * how squarely each render saw the surface (its normal against the line of
 * sight); where one has it, it alone. At the eight directions themselves the
 * picture is the render. Nothing is guessed from outlines.
 *
 * The renders are read from their sprites (SPR_DECOUPLE_3D_BASE,
 * ReadRawSprite16x()) when first needed and kept per level of zoom: a pixel
 * of a level is a block of 1 << zoom pixels of the render, the nearest
 * surface in the block (ReduceViewLevel()), so the work of drawing a building
 * goes with the pixels it covers on the screen, not with the render. The
 * levels are kept up to VIEW_CACHE_BYTES, the least recently used going first.
 * A model's picture at the angle and zoom last drawn is kept too, since every
 * block of the screen and every redraw at the same angle asks for the same
 * one; turning asks for new ones.
 */

#include "stdafx.h"
#include "three_d.h"
#include "spritecache.h"
#include "landscape.h"
#include "zoom_func.h"
#include "gfx_type.h"
#include "core/math_func.hpp"
#include "debug.h"

#include <chrono>
#include <cmath>
#include <list>
#include <map>

#include "table/sprites.h"
#include "table/three_d_models.h"

#include "safeguards.h"

static constexpr int NUM_MODEL_VIEWS = 8;
static constexpr int DEPTH_ZERO = 32768;           ///< the depth sprite's value for nula_m
static constexpr double DEPTH_UNIT_M = 0.001;      ///< a step of the depth sprite's value, metres
static constexpr int SAME_SURFACE_DEPTH = 400;     ///< two points this close along the line of sight (depth values, 0.4 m) are the same surface
static constexpr double SAME_SURFACE_M = SAME_SURFACE_DEPTH * DEPTH_UNIT_M;
static constexpr int MAX_LEVEL_SHIFT = 7;          ///< the coarsest level kept: a pixel for 128 of the render (ZoomLevel::Out8x)
static constexpr size_t VIEW_CACHE_BYTES = static_cast<size_t>(320) << 20; ///< how much of the renders is kept in memory
static constexpr size_t MAX_PICTURE_PIXELS = static_cast<size_t>(32) << 20; ///< a picture bigger than this is not drawn (a bad table)

/**
 * The model of a house of the original graphics.
 * @param house the HouseID of the house's north tile
 * @return its model, or nullptr for a house without one
 */
const ThreeDModel *GetThreeDHouseModel(HouseID house)
{
	for (const ThreeDModel &m : _three_d_models) {
		if (m.kind == ThreeDModelKind::House && m.id == house) return &m;
	}
	return nullptr;
}

/**
 * The model of one of the game's own industries.
 * @param type the industry's type
 * @param variant whether bit 0 of the industry's random bits is set, which picks the statue of bronze
 * @return its model, or nullptr for an industry without one
 */
const ThreeDModel *GetThreeDIndustryModel(IndustryType type, bool variant)
{
	const ThreeDModel *plain = nullptr;
	for (const ThreeDModel &m : _three_d_models) {
		if (m.id != type) continue;
		if (m.kind == ThreeDModelKind::IndustryVariant) {
			if (variant) return &m;
		} else if (m.kind == ThreeDModelKind::Industry) {
			plain = &m;
		}
	}
	return plain;
}

/** A model by its name in the table, for the rig; nullptr when there is none. */
const ThreeDModel *GetThreeDModelByName(std::string_view name)
{
	for (const ThreeDModel &m : _three_d_models) {
		if (name == m.name) return &m;
	}
	return nullptr;
}

/** The model at an index of the table (GetThreeDModelIndex()); nullptr past the end. */
const ThreeDModel *GetThreeDModel(uint index)
{
	return index < std::size(_three_d_models) ? &_three_d_models[index] : nullptr;
}

/** A model's index in the table, to carry it in a small field. */
uint GetThreeDModelIndex(const ThreeDModel *model)
{
	return static_cast<uint>(model - _three_d_models);
}

uint GetThreeDModelCount()
{
	return static_cast<uint>(std::size(_three_d_models));
}

/**
 * A render of a model at a level: every pixel a world point with a colour and
 * a normal, a pixel of the level standing for a block of step by step pixels
 * of the render.
 */
struct ThreeDViewLevel {
	int width = 0;
	int height = 0;
	int step = 1;                 ///< pixels of the render to a pixel here, 1 << shift
	std::vector<uint32_t> colour; ///< Colour::data, alpha 0 where there is nothing
	std::vector<uint16_t> depth;  ///< the depth sprite's value, 0 where there is nothing
	std::vector<int8_t> normal;   ///< three a pixel: the normal in the model's frame, times 127
	uint16_t depth_min = 0;       ///< the least depth value of the render
	uint16_t depth_max = 0;       ///< and the most

	size_t Bytes() const
	{
		return this->colour.size() * sizeof(uint32_t) + this->depth.size() * sizeof(uint16_t) + this->normal.size() + sizeof(*this);
	}
};

/** Which level of which render of which model. */
struct ThreeDViewKey {
	uint16_t model;
	uint8_t view;
	uint8_t shift;

	auto operator<=>(const ThreeDViewKey &) const = default;
};

using ThreeDViewList = std::list<std::pair<ThreeDViewKey, ThreeDViewLevel>>;
static ThreeDViewList _view_lru; ///< the levels kept, the most recently used first
static std::map<ThreeDViewKey, ThreeDViewList::iterator> _view_index;
static size_t _view_bytes = 0;

/** The models' pictures at the angle and zoom the map was last drawn at, one a model (ThreeDPictureCache). */
static ThreeDPictureCache _pictures;

/** Everything kept of the renders and the pictures made from them is dropped: the sprites are being reloaded. */
void ClearThreeDModelCache()
{
	_view_index.clear();
	_view_lru.clear();
	_view_bytes = 0;
	_pictures.clear();
}

/**
 * Read the three sprites of a render and make its finest level.
 * @param model the model
 * @param view which of its eight renders
 * @param[out] level the level, every pixel of the render
 * @return whether the sprites were there and of the size the table says
 */
static bool ReadViewLevel(const ThreeDModel *model, int view, ThreeDViewLevel &level)
{
	const ThreeDModelView &v = model->views[view];
	SpriteID base = SPR_DECOUPLE_3D_BASE + model->first + view * 3;
	std::vector<uint32_t> colour, depth, normals;
	int w[3], h[3], x, y;
	if (!ReadRawSprite16x(base, colour, w[0], h[0], x, y) || !ReadRawSprite16x(base + 1, depth, w[1], h[1], x, y) || !ReadRawSprite16x(base + 2, normals, w[2], h[2], x, y)) {
		Debug(sprite, 1, "3D model {} direction {}: no 16x sprites at {}", model->name, view, base);
		return false;
	}
	if (w[0] != v.width || h[0] != v.height || w[1] != w[0] || h[1] != h[0] || w[2] != w[0] || h[2] != h[0]) {
		Debug(sprite, 0, "3D model {} direction {}: the sprites are {}x{}, {}x{}, {}x{}, the table says {}x{}", model->name, view, w[0], h[0], w[1], h[1], w[2], h[2], v.width, v.height);
		return false;
	}

	level.width = w[0];
	level.height = h[0];
	level.step = 1;
	size_t n = static_cast<size_t>(level.width) * level.height;
	level.colour.resize(n);
	level.depth.resize(n);
	level.normal.resize(n * 3);
	uint16_t zmin = UINT16_MAX, zmax = 0;
	for (size_t i = 0; i < n; i++) {
		Colour c(colour[i]), d(depth[i]), nn(normals[i]);
		uint16_t z = (d.a == 0 || c.a == 0) ? 0 : static_cast<uint16_t>((d.r << 8) | d.g);
		if (z == 0) {
			level.colour[i] = 0;
			level.depth[i] = 0;
			level.normal[i * 3] = level.normal[i * 3 + 1] = level.normal[i * 3 + 2] = 0;
			continue;
		}
		level.colour[i] = colour[i];
		level.depth[i] = z;
		level.normal[i * 3] = static_cast<int8_t>(Clamp(nn.r - 128, -127, 127));
		level.normal[i * 3 + 1] = static_cast<int8_t>(Clamp(nn.g - 128, -127, 127));
		level.normal[i * 3 + 2] = static_cast<int8_t>(Clamp(nn.b - 128, -127, 127));
		zmin = std::min(zmin, z);
		zmax = std::max(zmax, z);
	}
	level.depth_min = std::min(zmin, zmax);
	level.depth_max = zmax;
	return true;
}

/**
 * The next coarser level: each pixel the nearest surface of a block of two by
 * two pixels of the finer one, its colour and normal the mean of the pixels of
 * that surface, its alpha the block's cover.
 */
static void ReduceViewLevel(const ThreeDViewLevel &fine, ThreeDViewLevel &coarse)
{
	coarse.width = (fine.width + 1) / 2;
	coarse.height = (fine.height + 1) / 2;
	coarse.step = fine.step * 2;
	size_t n = static_cast<size_t>(coarse.width) * coarse.height;
	coarse.colour.assign(n, 0);
	coarse.depth.assign(n, 0);
	coarse.normal.assign(n * 3, 0);
	coarse.depth_min = fine.depth_min;
	coarse.depth_max = fine.depth_max;
	for (int Y = 0; Y < coarse.height; Y++) {
		for (int X = 0; X < coarse.width; X++) {
			size_t src[4];
			int count = 0;
			uint16_t nearest = UINT16_MAX;
			for (int dy = 0; dy < 2; dy++) {
				int y = 2 * Y + dy;
				if (y >= fine.height) continue;
				for (int dx = 0; dx < 2; dx++) {
					int x = 2 * X + dx;
					if (x >= fine.width) continue;
					size_t i = static_cast<size_t>(y) * fine.width + x;
					src[count++] = i;
					if (fine.depth[i] != 0) nearest = std::min(nearest, fine.depth[i]);
				}
			}
			if (nearest == UINT16_MAX) continue;
			int r = 0, g = 0, b = 0, a = 0, nx = 0, ny = 0, nz = 0, z = 0, near = 0;
			for (int k = 0; k < count; k++) {
				size_t i = src[k];
				if (fine.depth[i] == 0) continue;
				Colour c(fine.colour[i]);
				a += c.a;
				if (fine.depth[i] > nearest + SAME_SURFACE_DEPTH) continue;
				r += c.r; g += c.g; b += c.b;
				nx += fine.normal[i * 3]; ny += fine.normal[i * 3 + 1]; nz += fine.normal[i * 3 + 2];
				z += fine.depth[i];
				near++;
			}
			size_t o = static_cast<size_t>(Y) * coarse.width + X;
			coarse.colour[o] = Colour(static_cast<uint8_t>(r / near), static_cast<uint8_t>(g / near), static_cast<uint8_t>(b / near), static_cast<uint8_t>(a / count)).data;
			coarse.depth[o] = static_cast<uint16_t>(std::max(1, z / near));
			double len = std::sqrt(static_cast<double>(nx) * nx + static_cast<double>(ny) * ny + static_cast<double>(nz) * nz);
			if (len > 0) {
				coarse.normal[o * 3] = static_cast<int8_t>(std::lround(nx / len * 127));
				coarse.normal[o * 3 + 1] = static_cast<int8_t>(std::lround(ny / len * 127));
				coarse.normal[o * 3 + 2] = static_cast<int8_t>(std::lround(nz / len * 127));
			}
		}
	}
}

/**
 * Drop the least recently used levels until the kept ones fit: never the one
 * just asked for, and never the two most recently used, which a picture being
 * made may be reading (the two renders either side of the angle).
 */
static void TrimViewCache(ThreeDViewList::iterator keep)
{
	while (_view_bytes > VIEW_CACHE_BYTES && _view_lru.size() > 3) {
		auto last = std::prev(_view_lru.end());
		if (last == keep) last = std::prev(last);
		if (last == _view_lru.begin() || last == std::next(_view_lru.begin())) break;
		_view_bytes -= last->second.Bytes();
		_view_index.erase(last->first);
		_view_lru.erase(last);
	}
}

/**
 * A level of a render, read and made when it is not kept. Reading a render
 * makes the level asked for and every coarser one, since they are small and
 * the reading is the cost.
 * @param model the model
 * @param view which of its eight renders
 * @param shift the level: a pixel for 1 << shift pixels of the render
 * @return the level, or nullptr when the render's sprites are not there
 */
static const ThreeDViewLevel *GetViewLevel(const ThreeDModel *model, int view, int shift)
{
	shift = Clamp(shift, 0, MAX_LEVEL_SHIFT);
	ThreeDViewKey key{static_cast<uint16_t>(GetThreeDModelIndex(model)), static_cast<uint8_t>(view), static_cast<uint8_t>(shift)};
	auto found = _view_index.find(key);
	if (found != _view_index.end()) {
		_view_lru.splice(_view_lru.begin(), _view_lru, found->second);
		return &found->second->second;
	}

	ThreeDViewLevel fine;
	if (!ReadViewLevel(model, view, fine)) return nullptr;
	ThreeDViewList::iterator wanted = _view_lru.end();
	for (int s = 0; s <= MAX_LEVEL_SHIFT; s++) {
		if (s > 0) {
			ThreeDViewLevel coarse;
			ReduceViewLevel(fine, coarse);
			fine = std::move(coarse);
		}
		if (s < shift) continue;
		ThreeDViewKey k{key.model, key.view, static_cast<uint8_t>(s)};
		if (_view_index.find(k) != _view_index.end()) continue;
		ThreeDViewLevel copy = fine;
		_view_bytes += copy.Bytes();
		_view_lru.emplace_front(k, std::move(copy));
		_view_index[k] = _view_lru.begin();
		if (s == shift) wanted = _view_lru.begin();
	}
	if (wanted == _view_lru.end()) wanted = _view_index.find(key)->second;
	TrimViewCache(wanted);
	return &wanted->second;
}

/**
 * The model's frame into the game's world: metres to world units, the axes
 * swapped the way the plot's corners say, the height stretched as the game
 * draws it (a tile is TILE_SIZE units wide and a height level TILE_HEIGHT
 * units, which the game's projection draws TILE_HEIGHT pixels tall).
 */
struct ThreeDModelFrame {
	double s;  ///< world units to a metre along the ground
	double sz; ///< world units to a metre up

	void ToWorld(double bx, double by, double bz, double &gx, double &gy, double &gz) const
	{
		gx = -by * this->s;
		gy = bx * this->s;
		gz = bz * this->sz;
	}

	void ToModel(double gx, double gy, double gz, double &bx, double &by, double &bz) const
	{
		bx = gy / this->s;
		by = -gx / this->s;
		bz = gz / this->sz;
	}
};

static ThreeDModelFrame FrameOf(const ThreeDModel *model)
{
	/* How many 16x pixels a metre of height moves a point up the picture: the vertical world axis on the camera's down axis. */
	const ThreeDModelView &v = model->views[0];
	double dd = static_cast<double>(v.dolu[0]) * v.dolu[0] + static_cast<double>(v.dolu[1]) * v.dolu[1] + static_cast<double>(v.dolu[2]) * v.dolu[2];
	double px_per_m_up = -v.dolu[2] / dd;
	return {TILE_SIZE / static_cast<double>(model->tile_m), px_per_m_up / ZOOM_BASE};
}

/** A point of a render through the turned projection: its pixel and its depth are affine in the pixel's row, column and depth value. */
struct ThreeDViewMap {
	double x0, xi, xj, xz; ///< pixel x from the origin's: the constant, per column, per row, per depth value
	double y0, yi, yj, yz;
	double d0, di, dj, dz; ///< the depth along the line of sight of the angle, metres
};

/**
 * The map of a level of a render into the picture of the angle.
 * @param v the render
 * @param level its level
 * @param frame the model's frame
 * @param axis the turned projection of the three world axes, pixels of the zoom (x, y each)
 * @param sight the line of sight of the angle in the model's frame, unit
 */
static ThreeDViewMap MapView(const ThreeDModelView &v, const ThreeDViewLevel &level, const ThreeDModelFrame &frame, const double axis[3][2], const double sight[3])
{
	/* The world point of pixel (I, J) of the level at depth value z: b0 + I bi + J bj + z bz. */
	double b0[3], bi[3], bj[3], bz[3];
	for (int c = 0; c < 3; c++) {
		b0[c] = v.pocatek[c] + (v.nula_m - DEPTH_ZERO * DEPTH_UNIT_M) * v.pohled[c] + 0.5 * level.step * (v.vpravo[c] + v.dolu[c]);
		bi[c] = level.step * v.vpravo[c];
		bj[c] = level.step * v.dolu[c];
		bz[c] = DEPTH_UNIT_M * v.pohled[c];
	}
	ThreeDViewMap m;
	auto project = [&](const double b[3], double &px, double &py, double &pd) {
		double gx, gy, gz;
		frame.ToWorld(b[0], b[1], b[2], gx, gy, gz);
		px = gx * axis[0][0] + gy * axis[1][0] + gz * axis[2][0];
		py = gx * axis[0][1] + gy * axis[1][1] + gz * axis[2][1];
		pd = b[0] * sight[0] + b[1] * sight[1] + b[2] * sight[2];
	};
	project(b0, m.x0, m.y0, m.d0);
	project(bi, m.xi, m.yi, m.di);
	project(bj, m.xj, m.yj, m.dj);
	project(bz, m.xz, m.yz, m.dz);
	return m;
}

/** One render splat into the picture: the nearest point at each pixel. */
struct ThreeDSplat {
	std::vector<float> depth;   ///< along the line of sight, metres; infinity where nothing landed
	std::vector<uint32_t> colour;
	std::vector<float> weight;  ///< the render's weight at the angle times how squarely it saw the surface
};

/**
 * Splat a level of a render into the picture.
 * @param level the level
 * @param map its map into the picture
 * @param left,top the picture's top-left pixel from the origin
 * @param width,height the picture
 * @param sight the line of sight in the model's frame
 * @param weight the render's weight at the angle
 * @param[out] out the splat
 */
static void SplatView(const ThreeDViewLevel &level, const ThreeDViewMap &map, int left, int top, int width, int height, const double sight[3], float weight, ThreeDSplat &out)
{
	size_t n = static_cast<size_t>(width) * height;
	out.depth.assign(n, INFINITY);
	out.colour.assign(n, 0);
	out.weight.assign(n, 0.0f);
	const float sx = static_cast<float>(sight[0]), sy = static_cast<float>(sight[1]), sz = static_cast<float>(sight[2]);
	for (int J = 0; J < level.height; J++) {
		const uint16_t *drow = &level.depth[static_cast<size_t>(J) * level.width];
		const uint32_t *crow = &level.colour[static_cast<size_t>(J) * level.width];
		const int8_t *nrow = &level.normal[static_cast<size_t>(J) * level.width * 3];
		double rx = map.x0 + map.xj * J - left, ry = map.y0 + map.yj * J - top, rd = map.d0 + map.dj * J;
		for (int I = 0; I < level.width; I++, rx += map.xi, ry += map.yi, rd += map.di) {
			uint16_t z = drow[I];
			if (z == 0) continue;
			double px = rx + map.xz * z, py = ry + map.yz * z;
			float pd = static_cast<float>(rd + map.dz * z);
			int x0 = static_cast<int>(std::floor(px - 0.5)), y0 = static_cast<int>(std::floor(py - 0.5));
			if (x0 < -1 || y0 < -1 || x0 >= width || y0 >= height) continue;
			float facing = -(nrow[I * 3] * sx + nrow[I * 3 + 1] * sy + nrow[I * 3 + 2] * sz) / 127.0f;
			if (facing < 0) facing = 0;
			float w = (facing * facing + 0.02f) * weight;
			for (int dy = 0; dy < 2; dy++) {
				int y = y0 + dy;
				if (y < 0 || y >= height) continue;
				for (int dx = 0; dx < 2; dx++) {
					int x = x0 + dx;
					if (x < 0 || x >= width) continue;
					size_t o = static_cast<size_t>(y) * width + x;
					if (pd < out.depth[o]) {
						out.depth[o] = pd;
						out.colour[o] = crow[I];
						out.weight[o] = w;
					}
				}
			}
		}
	}
}

/**
 * The picture of a model at the angle the view is turned to, at a zoom: made
 * from the two renders either side of the angle when it is not the one kept.
 * @param model the model
 * @param zoom the zoom of the viewport
 * @return the picture, or nullptr when the renders are not there
 */
const ThreeDModelPicture *GetThreeDModelPicture(const ThreeDModel *model, ZoomLevel zoom)
{
	return GetThreeDModelPictureAt(model, zoom, GetViewYaw(), _pictures);
}

/**
 * The picture of a model turned by any angle, at a zoom, for the previews
 * (three_d_preview_gui.cpp) and for the map: the world turned by the angle
 * about the model's origin and drawn by the game's projection, from the two
 * renders either side of the angle.
 * @param model the model
 * @param zoom the zoom
 * @param yaw the angle, degrees, the way the map view counts it
 * @param cache where the picture is kept, one a model, by the angle and zoom last asked for
 * @return the picture, or nullptr when the renders are not there
 */
const ThreeDModelPicture *GetThreeDModelPictureAt(const ThreeDModel *model, ZoomLevel zoom, float yaw, ThreeDPictureCache &cache)
{
	uint index = GetThreeDModelIndex(model);
	auto kept = cache.find(index);
	if (kept != cache.end() && kept->second.yaw == yaw && kept->second.zoom == zoom) return &kept->second.picture;

	/* The projection of the world turned by the angle: a world point (x, y, z) about the origin goes to
	 * (c x - s y, s x + c y, z) and then through RemapCoords() (TurnWorldPoint(), ProjectTurnedF()); it is linear,
	 * so three columns, in pixels of the zoom. */
	double a = yaw * M_PI / 180.0;
	double c = std::cos(a), s = std::sin(a);
	double scale = 1.0 / (1 << to_underlying(zoom));
	auto project = [&](double x, double y, double z, double &px, double &py) {
		double xr = c * x - s * y, yr = s * x + c * y;
		px = (yr - xr) * 2 * ZOOM_BASE * scale;
		py = (yr + xr - z) * ZOOM_BASE * scale;
	};
	double axis[3][2];
	project(1, 0, 0, axis[0][0], axis[0][1]);
	project(0, 1, 0, axis[1][0], axis[1][1]);
	project(0, 0, 1, axis[2][0], axis[2][1]);

	/* The line of sight in the model's frame: the game looks along (-1, -1, -2) of the turned world, which is
	 * (c, s; -s, c) applied to (-1, -1) of the world as it is. */
	ThreeDModelFrame frame = FrameOf(model);
	double sight[3];
	{
		double ux = c * -1 + s * -1, uy = -s * -1 + c * -1;
		double bx, by, bz;
		frame.ToModel(ux, uy, -2.0, bx, by, bz);
		double len = std::sqrt(bx * bx + by * by + bz * bz);
		sight[0] = bx / len; sight[1] = by / len; sight[2] = bz / len;
	}

	/* The two renders either side of it, by the azimuth, and their weights. */
	double az = std::atan2(sight[1], sight[0]);
	double diff[NUM_MODEL_VIEWS];
	int k0 = 0;
	for (int k = 0; k < NUM_MODEL_VIEWS; k++) {
		double d = az - std::atan2(model->views[k].pohled[1], model->views[k].pohled[0]);
		d = std::fmod(d + 3 * M_PI, 2 * M_PI) - M_PI;
		diff[k] = std::abs(d);
		if (diff[k] < diff[k0]) k0 = k;
	}
	int k1 = -1;
	for (int k = 0; k < NUM_MODEL_VIEWS; k++) {
		if (k != k0 && (k1 < 0 || diff[k] < diff[k1])) k1 = k;
	}
	double t = diff[k0] + diff[k1] > 0 ? diff[k0] / (diff[k0] + diff[k1]) : 0.0;
	if (t < 0.005) k1 = -1; // at a render itself: that render alone
	int views[2] = {k0, k1};
	float weights[2] = {static_cast<float>(1.0 - t), static_cast<float>(t)};

	/* The levels of the two, and the picture's extent: their corners at the least and the most depth. */
	int shift = to_underlying(zoom);
	const ThreeDViewLevel *levels[2] = {nullptr, nullptr};
	ThreeDViewMap maps[2];
	double minx = INFINITY, maxx = -INFINITY, miny = INFINITY, maxy = -INFINITY;
	for (int s = 0; s < 2; s++) {
		if (views[s] < 0) continue;
		levels[s] = GetViewLevel(model, views[s], shift);
		if (levels[s] == nullptr) return nullptr;
		maps[s] = MapView(model->views[views[s]], *levels[s], frame, axis, sight);
		for (int corner = 0; corner < 8; corner++) {
			double I = (corner & 1) ? levels[s]->width : 0, J = (corner & 2) ? levels[s]->height : 0;
			double z = (corner & 4) ? levels[s]->depth_max : levels[s]->depth_min;
			double px = maps[s].x0 + maps[s].xi * I + maps[s].xj * J + maps[s].xz * z;
			double py = maps[s].y0 + maps[s].yi * I + maps[s].yj * J + maps[s].yz * z;
			minx = std::min(minx, px); maxx = std::max(maxx, px);
			miny = std::min(miny, py); maxy = std::max(maxy, py);
		}
	}
	ThreeDPictureEntry &entry = cache[index];
	entry.yaw = yaw;
	entry.zoom = zoom;
	ThreeDModelPicture &pic = entry.picture;
	pic.left = static_cast<int>(std::floor(minx)) - 2;
	pic.top = static_cast<int>(std::floor(miny)) - 2;
	pic.width = static_cast<int>(std::ceil(maxx)) + 3 - pic.left;
	pic.height = static_cast<int>(std::ceil(maxy)) + 3 - pic.top;
	if (pic.width <= 0 || pic.height <= 0 || static_cast<size_t>(pic.width) * pic.height > MAX_PICTURE_PIXELS) {
		pic.width = pic.height = 0;
		pic.pixels.clear();
		return nullptr;
	}

	/* Each render splat, then the two put together. */
	static ThreeDSplat splats[2];
	for (int s = 0; s < 2; s++) {
		if (levels[s] == nullptr) continue;
		SplatView(*levels[s], maps[s], pic.left, pic.top, pic.width, pic.height, sight, weights[s], splats[s]);
	}
	size_t n = static_cast<size_t>(pic.width) * pic.height;
	pic.pixels.assign(n, 0);
	if (levels[1] == nullptr) {
		for (size_t o = 0; o < n; o++) {
			if (splats[0].depth[o] != INFINITY) pic.pixels[o] = splats[0].colour[o];
		}
	} else {
		for (size_t o = 0; o < n; o++) {
			float da = splats[0].depth[o], db = splats[1].depth[o];
			bool has_a = da != INFINITY, has_b = db != INFINITY;
			if (!has_a && !has_b) continue;
			if (has_a && has_b && std::abs(da - db) < SAME_SURFACE_M) {
				Colour a(splats[0].colour[o]), b(splats[1].colour[o]);
				float wa = splats[0].weight[o], wb = splats[1].weight[o], sum = wa + wb;
				if (sum <= 0) { pic.pixels[o] = a.data; continue; }
				wa /= sum; wb /= sum;
				pic.pixels[o] = Colour(
					static_cast<uint8_t>(a.r * wa + b.r * wb + 0.5f),
					static_cast<uint8_t>(a.g * wa + b.g * wb + 0.5f),
					static_cast<uint8_t>(a.b * wa + b.b * wb + 0.5f),
					static_cast<uint8_t>(a.a * wa + b.a * wb + 0.5f)).data;
			} else if (has_a && (!has_b || da < db)) {
				pic.pixels[o] = splats[0].colour[o];
			} else {
				pic.pixels[o] = splats[1].colour[o];
			}
		}
	}
	return &pic;
}

/** What the rig prints of a model: its sprites, its first render read, how long that took (test3d model). */
std::string DescribeThreeDModel(const ThreeDModel *model)
{
	SpriteID base = SPR_DECOUPLE_3D_BASE + model->first;
	int present = 0;
	for (int i = 0; i < NUM_MODEL_VIEWS * 3; i++) {
		if (SpriteExists(base + i)) present++;
	}
	std::string s = fmt::format("{}: {} {} {}x{} tiles, sprites {}..{} ({} of 24 there)", model->name, static_cast<int>(model->kind), model->id, model->tiles_x, model->tiles_y, base, base + NUM_MODEL_VIEWS * 3 - 1, present);
	if (present == 0) return s;
	auto t0 = std::chrono::steady_clock::now();
	ThreeDViewLevel level;
	if (!ReadViewLevel(model, 0, level)) return s + "; d0 could not be read";
	auto t1 = std::chrono::steady_clock::now();
	size_t points = 0;
	for (uint16_t z : level.depth) if (z != 0) points++;
	ThreeDModelFrame frame = FrameOf(model);
	s += fmt::format("; d0 {}x{}, {} points, depth {:.2f}..{:.2f} m, read in {:.1f} ms; a metre is {:.3f} units along, {:.3f} up; kept {} MB of renders",
			level.width, level.height, points,
			model->views[0].nula_m + (level.depth_min - DEPTH_ZERO) * DEPTH_UNIT_M, model->views[0].nula_m + (level.depth_max - DEPTH_ZERO) * DEPTH_UNIT_M,
			std::chrono::duration<double, std::milli>(t1 - t0).count(), frame.s, frame.sz, _view_bytes >> 20);
	return s;
}
