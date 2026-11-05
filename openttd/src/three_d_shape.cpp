/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file three_d_shape.cpp The shape of a building read off its one sprite (three_d_shape.h).
 *
 * The sprite's pixels are laid on a grid of the 2x zoom in the game's own view. A box -- its footprint, its height
 * and a roof (flat, gable along X or Y, hip) -- is projected as the game projects the world, and the box whose outline
 * covers the sprite's pixels best is kept: the most of the sprite inside, the least of the empty around it
 * (intersection over union, a pixel of the sprite left outside counted twice: it is not drawn at all). It is found by moving one number at a time while that helps, by 2, 1, 1/2 and 1/4 of a
 * world unit, from a few starts: the bounding box, the box drawn before, a smaller box in the middle. A sloping roof is
 * kept only where it fits clearly better than a flat top and is no steeper than 2.5 : 1, else roofs come out as spikes
 * (a tower's, three units across or less, may be a spire).
 * Where one box does not fit well (a tower on a base, a church, a house in an L), more boxes are added the same way,
 * one at a time, up to four, each kept only where it fits clearly better.
 * A shape is read once per sprite and where it hangs in its bounding box, and kept: the sprite's pixels here, the
 * shape by the thread in the background (ThreeDLoadInBackground()).
 */

#include "stdafx.h"
#include "three_d_shape.h"
#include "spritecache.h"
#include "three_d_objects.h"
#include "tile_type.h"
#include "zoom_func.h"
#include "core/format.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_map>

#include "safeguards.h"

/** Whether the turned view draws the buildings without a hand table as the shapes read off their sprites (test3d tvary 0|1). */
bool _three_d_sprite_shapes_on = true;

namespace {

/** Screen units (ZoomLevel::Min) per cell of the grid the shapes are fitted on: a pixel of the 2x zoom. */
constexpr int CELL = 1 << to_underlying(ZoomLevel::In2x);

/** How far outside its numbers AddBoxFaces() (viewport.cpp) draws a box's walls, and twice that over its top. */
constexpr double MARGIN = 0.25;

/** The steepest roof kept: its rise over half its span (2.5 : 1). */
constexpr double STEEPEST_ROOF = 2.5;

/** A part this narrow (half its narrower side, units) is a tower: its roof may be a spire, up to STEEPEST_SPIRE. */
constexpr double SPIRE_HALF = 1.5;
constexpr double STEEPEST_SPIRE = 12.0;

/** How much better than a flat top a sloping roof must fit to be kept. */
constexpr double ROOF_BETTER = 0.01;

/** The same of a box added to the first (a tower's spire covers few pixels of the whole sprite). */
constexpr double ADDED_ROOF_BETTER = 0.003;

/** The share of the sprite's pixels a shape must keep (draw on its faces), or as much as the box drawn before keeps. */
constexpr double KEPT_AT_LEAST = 0.90;

/**
 * How much more a pixel of the sprite outside a shape counts than an empty cell inside it: what is outside is not drawn
 * at all (facing north neither), an empty cell only shows a little of the sprite's edge stretched.
 */
constexpr double LOST_WEIGHT = 2.0;

/** At most this many boxes scored together (the hand tables have up to five). */
constexpr size_t MAX_PARTS = 6;

/** At most this many boxes in a shape read off a sprite (a base, a body, a tower, a chimney). */
constexpr size_t MAX_SHAPE_PARTS = 4;

/** A shape fitting this well gets no more boxes. */
constexpr double GOOD_ENOUGH = 0.95;

/** How much better a shape must fit with another box for it to be kept. */
constexpr double PART_BETTER = 0.02;

/** How many of the boxes looked at once are moved into place (Descend()) when another is sought. */
constexpr size_t CANDIDATES_MOVED = 2;

/** a / b rounded down, for any sign of a. */
inline int FloorDivide(int a, int b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/** The sprite's pixels on the grid: how much of each cell they cover (0..1), and the sums of each row from its left. */
struct Target {
	int ox = 0, oy = 0;          ///< screen units of the grid's top left corner from the bounding box's north corner
	int w = 0, h = 0;            ///< cells
	std::vector<float> cover;    ///< w * h
	std::vector<double> prefix;  ///< (w + 1) per row: the sum of the row's cover left of each cell
	double total = 0;            ///< the cover of all cells
	int top_z = 0;               ///< the highest a box can reach (z units): the sprite's height
};

/**
 * Lay a sprite's pixels on the grid: of the levels its file has, the coarsest finer than the grid, else the finest;
 * a pixel counts by its alpha (8bpp: any colour but the transparent one).
 */
bool ReadTarget(SpriteID image, int origin_x, int origin_y, Target &t)
{
	SpriteLoader::SpriteCollection collection;
	bool is_32bpp;
	const ZoomLevels levels = ReadRawSprite(image, collection, is_32bpp);
	ZoomLevel use = ZoomLevel::End;
	for (ZoomLevel z : levels) {
		if (use == ZoomLevel::End || (1 << to_underlying(z)) <= CELL) use = z;
	}
	if (use == ZoomLevel::End) return false;
	const SpriteLoader::Sprite &s = collection[use];
	if (s.width == 0 || s.height == 0 || s.data == nullptr) return false;

	const int scale = 1 << to_underlying(use);
	const int rx0 = origin_x + s.x_offs * scale, ry0 = origin_y + s.y_offs * scale;
	const int gx0 = FloorDivide(rx0, CELL), gy0 = FloorDivide(ry0, CELL);
	const int gx1 = FloorDivide(rx0 + s.width * scale - 1, CELL) + 1, gy1 = FloorDivide(ry0 + s.height * scale - 1, CELL) + 1;
	t.ox = gx0 * CELL;
	t.oy = gy0 * CELL;
	t.w = gx1 - gx0;
	t.h = gy1 - gy0;
	if (t.w <= 0 || t.h <= 0 || static_cast<int64_t>(t.w) * t.h > 4'000'000) return false;
	t.cover.assign(static_cast<size_t>(t.w) * t.h, 0.0f);

	const float cell_area = static_cast<float>(CELL * CELL);
	for (int j = 0; j < s.height; j++) {
		const int py0 = ry0 + j * scale - t.oy, py1 = py0 + scale;
		for (int i = 0; i < s.width; i++) {
			const SpriteLoader::CommonPixel &px = s.data[static_cast<size_t>(j) * s.width + i];
			const float a = is_32bpp ? px.a / 255.0f : (px.m != 0 ? 1.0f : 0.0f);
			if (a <= 0) continue;
			const int px0 = rx0 + i * scale - t.ox, px1 = px0 + scale;
			for (int cy = py0 / CELL; cy * CELL < py1; cy++) {
				const int oy = std::min(py1, (cy + 1) * CELL) - std::max(py0, cy * CELL);
				for (int cx = px0 / CELL; cx * CELL < px1; cx++) {
					const int ox = std::min(px1, (cx + 1) * CELL) - std::max(px0, cx * CELL);
					t.cover[static_cast<size_t>(cy) * t.w + cx] += a * ox * oy / cell_area;
				}
			}
		}
	}

	t.prefix.assign(static_cast<size_t>(t.w + 1) * t.h, 0.0);
	t.total = 0;
	for (int y = 0; y < t.h; y++) {
		double sum = 0;
		double *row = &t.prefix[static_cast<size_t>(y) * (t.w + 1)];
		for (int x = 0; x < t.w; x++) {
			const float c = std::min(1.0f, t.cover[static_cast<size_t>(y) * t.w + x]);
			t.cover[static_cast<size_t>(y) * t.w + x] = c;
			row[x] = sum;
			sum += c;
		}
		row[t.w] = sum;
		t.total += sum;
	}
	t.top_z = (s.height * scale) / static_cast<int>(ZOOM_BASE) + static_cast<int>(TILE_HEIGHT);
	return t.total > 0;
}

/** A point on the grid. */
struct GridPoint {
	double x, y;
};

/** The outline of a part on the grid: its convex hull (anticlockwise), and the rows it spans. */
struct Outline {
	std::array<GridPoint, 12> hull;
	int n = 0;
	int row0 = 0, row1 = -1; ///< the rows whose middles it spans
};

/** Where a world point (from the bounding box's north corner) lies on the grid, as the game projects it (RemapCoords()). */
inline GridPoint Project(const Target &t, double x, double y, double z)
{
	const double sx = (y - x) * 2 * ZOOM_BASE, sy = (x + y - z) * ZOOM_BASE;
	return {(sx - t.ox) / CELL, (sy - t.oy) / CELL};
}

/** The outline of a part as AddBoxFaces() draws it, moved by dx, dy, dz. */
Outline OutlineOf(const Target &t, const ThreeDPart &p, double dx, double dy, double dz)
{
	const double x0 = p.x0 - MARGIN + dx, x1 = p.x1 + MARGIN + dx, y0 = p.y0 - MARGIN + dy, y1 = p.y1 + MARGIN + dy;
	const double z0 = dz, z1 = dz + p.h + 2 * MARGIN, zr = z1 + p.rise;
	const double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
	std::array<GridPoint, 12> pts;
	int n = 0;
	for (double z : {z0, z1}) {
		pts[n++] = Project(t, x0, y0, z);
		pts[n++] = Project(t, x1, y0, z);
		pts[n++] = Project(t, x1, y1, z);
		pts[n++] = Project(t, x0, y1, z);
	}
	switch (p.roof) {
		case ThreeDRoof::Flat: break;
		case ThreeDRoof::GableX:
			pts[n++] = Project(t, x0, my, zr);
			pts[n++] = Project(t, x1, my, zr);
			break;
		case ThreeDRoof::GableY:
			pts[n++] = Project(t, mx, y0, zr);
			pts[n++] = Project(t, mx, y1, zr);
			break;
		case ThreeDRoof::Hip: {
			const double hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
			if (hx >= hy) {
				pts[n++] = Project(t, mx - (hx - hy), my, zr);
				pts[n++] = Project(t, mx + (hx - hy), my, zr);
			} else {
				pts[n++] = Project(t, mx, my - (hy - hx), zr);
				pts[n++] = Project(t, mx, my + (hy - hx), zr);
			}
			break;
		}
	}

	/* The convex hull (Andrew's chain). */
	std::sort(pts.begin(), pts.begin() + n, [](const GridPoint &a, const GridPoint &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
	auto cross = [](const GridPoint &o, const GridPoint &a, const GridPoint &b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
	Outline out;
	std::array<GridPoint, 24> h;
	int k = 0;
	for (int i = 0; i < n; i++) {
		while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) k--;
		h[k++] = pts[i];
	}
	for (int i = n - 2, lo = k + 1; i >= 0; i--) {
		while (k >= lo && cross(h[k - 2], h[k - 1], pts[i]) <= 0) k--;
		h[k++] = pts[i];
	}
	out.n = std::min(k - 1, static_cast<int>(out.hull.size()));
	double ymin = 1e18, ymax = -1e18;
	for (int i = 0; i < out.n; i++) {
		out.hull[i] = h[i];
		ymin = std::min(ymin, h[i].y);
		ymax = std::max(ymax, h[i].y);
	}
	out.row0 = static_cast<int>(std::ceil(ymin - 0.5));
	out.row1 = static_cast<int>(std::floor(ymax - 0.5));
	return out;
}

/** The cells of a row whose middles an outline covers, [i0, i1]; false when none. */
inline bool RowSpan(const Outline &o, int row, int &i0, int &i1)
{
	const double y = row + 0.5;
	double xl = 1e18, xr = -1e18;
	for (int i = 0; i < o.n; i++) {
		const GridPoint &a = o.hull[i], &b = o.hull[(i + 1) % o.n];
		if ((y < a.y && y < b.y) || (y > a.y && y > b.y)) continue;
		double x;
		if (a.y == b.y) {
			xl = std::min({xl, a.x, b.x});
			xr = std::max({xr, a.x, b.x});
			continue;
		}
		x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
		xl = std::min(xl, x);
		xr = std::max(xr, x);
	}
	if (xl > xr) return false;
	i0 = static_cast<int>(std::ceil(xl - 0.5));
	i1 = static_cast<int>(std::floor(xr - 0.5));
	return i0 <= i1;
}

/**
 * How well parts fit the sprite: the cover inside their outlines over the cover and the cells inside together (0..1),
 * the cover outside them counted LOST_WEIGHT times.
 * @param[out] kept if given, the share of the sprite's cover inside them: what of the sprite is drawn on them at all
 */
double Score(const Target &t, std::span<const ThreeDPart> parts, double dx = 0, double dy = 0, double dz = 0, double *kept = nullptr)
{
	std::array<Outline, MAX_PARTS> outlines;
	const size_t n = std::min(parts.size(), MAX_PARTS);
	int row0 = INT_MAX, row1 = INT_MIN;
	for (size_t i = 0; i < n; i++) {
		outlines[i] = OutlineOf(t, parts[i], dx, dy, dz);
		row0 = std::min(row0, outlines[i].row0);
		row1 = std::max(row1, outlines[i].row1);
	}
	double inside = 0, area = 0;
	for (int row = row0; row <= row1; row++) {
		std::array<std::pair<int, int>, MAX_PARTS> spans;
		size_t m = 0;
		for (size_t i = 0; i < n; i++) {
			int i0, i1;
			if (row >= outlines[i].row0 && row <= outlines[i].row1 && RowSpan(outlines[i], row, i0, i1)) spans[m++] = {i0, i1};
		}
		if (m == 0) continue;
		std::sort(spans.begin(), spans.begin() + m);
		/* The spans of the row joined, each counted once. */
		int a = spans[0].first, b = spans[0].second;
		auto count = [&](int lo, int hi) {
			area += hi - lo + 1;
			if (row < 0 || row >= t.h) return;
			const int c0 = std::clamp(lo, 0, t.w), c1 = std::clamp(hi + 1, 0, t.w);
			if (c1 <= c0) return;
			const double *p = &t.prefix[static_cast<size_t>(row) * (t.w + 1)];
			inside += p[c1] - p[c0];
		};
		for (size_t i = 1; i < m; i++) {
			if (spans[i].first <= b + 1) {
				b = std::max(b, spans[i].second);
			} else {
				count(a, b);
				a = spans[i].first;
				b = spans[i].second;
			}
		}
		count(a, b);
	}
	if (kept != nullptr) *kept = t.total > 0 ? inside / t.total : 0;
	const double uni = inside + LOST_WEIGHT * (t.total - inside) + (area - inside);
	return uni > 0 ? inside / uni : 0;
}

/** Whether a part can be: a footprint and a height at all, near its bounding box, a roof no steeper than allowed. */
bool Sane(const Target &t, const ThreeDPart &p, int ex, int ey)
{
	if (p.x1 - p.x0 < 1 || p.y1 - p.y0 < 1 || p.h < 1 || p.rise < 0) return false;
	if (p.x0 < -2 || p.y0 < -2 || p.x1 > ex + 2 || p.y1 > ey + 2) return false;
	if (p.h + p.rise > t.top_z) return false;
	const double half_x = (p.x1 - p.x0) / 2, half_y = (p.y1 - p.y0) / 2;
	const double steepest = std::min(half_x, half_y) <= SPIRE_HALF ? STEEPEST_SPIRE : STEEPEST_ROOF;
	switch (p.roof) {
		case ThreeDRoof::Flat: return p.rise == 0;
		case ThreeDRoof::GableX: return p.rise <= steepest * half_y;
		case ThreeDRoof::GableY: return p.rise <= steepest * half_x;
		case ThreeDRoof::Hip: return p.rise <= steepest * std::min(half_x, half_y);
	}
	return false;
}

/**
 * Move the numbers of one part of a shape one at a time, and its footprint whole, while the shape fits the sprite
 * better: by 2, 1, 1/2 and 1/4 of a unit (or the steps given). The other parts stay where they are.
 * @return how well the shape fits at the end
 */
double Descend(const Target &t, std::vector<ThreeDPart> &parts, size_t idx, int ex, int ey, int &tried, std::initializer_list<float> steps = {2.0f, 1.0f, 0.5f, 0.25f})
{
	double best = Score(t, parts);
	tried++;
	for (float step : steps) {
		for (int round = 0; round < 64; round++) {
			bool better = false;
			for (int what = 0; what < 9; what++) {
				if (what >= 7 && parts[idx].roof == ThreeDRoof::Flat) continue;
				for (float s : {step, -step}) {
					const ThreeDPart p = parts[idx];
					ThreeDPart q = p;
					switch (what) {
						case 0: q.x0 += s; break;
						case 1: q.x1 += s; break;
						case 2: q.y0 += s; break;
						case 3: q.y1 += s; break;
						case 4: q.h += s; break;
						case 5: q.x0 += s; q.x1 += s; break;
						case 6: q.y0 += s; q.y1 += s; break;
						case 7: q.h += s; q.rise -= s; break; // the walls up, the ridge where it is
						case 8: q.rise += s; break;
					}
					if (!Sane(t, q, ex, ey)) continue;
					parts[idx] = q;
					const double v = Score(t, parts);
					tried++;
					if (v > best + 1e-9) {
						best = v;
						better = true;
						break;
					}
					parts[idx] = p;
				}
			}
			if (!better) break;
		}
	}
	return best;
}

/**
 * A sloping roof on one part of a shape, kept where it fits clearly better than the flat top (ROOF_BETTER, an added
 * part ADDED_ROOF_BETTER): a gable
 * along X or along Y, a hip, each from the rises given (shares of the walls' height).
 * @return how well the shape fits with the roof kept
 */
double FitRoof(const Target &t, std::vector<ThreeDPart> &parts, size_t idx, double fit, int ex, int ey, int &tried, std::initializer_list<float> shares)
{
	const double roof_better = idx == 0 ? ROOF_BETTER : ADDED_ROOF_BETTER;
	const ThreeDPart flat = parts[idx];
	ThreeDPart best = flat;
	double best_fit = fit;
	for (ThreeDRoof roof : {ThreeDRoof::GableX, ThreeDRoof::GableY, ThreeDRoof::Hip}) {
		for (float share : shares) {
			ThreeDPart p = flat;
			p.roof = roof;
			const float half = roof == ThreeDRoof::GableX ? (p.y1 - p.y0) / 2 : roof == ThreeDRoof::GableY ? (p.x1 - p.x0) / 2 : std::min(p.x1 - p.x0, p.y1 - p.y0) / 2;
			/* A tower's roof starts as a spire, any other as steep as wide. */
			const bool tower = std::min(p.x1 - p.x0, p.y1 - p.y0) / 2 <= SPIRE_HALF;
			p.rise = std::min(half * static_cast<float>(tower ? STEEPEST_SPIRE / 2 : 1.0), p.h * share);
			p.h -= p.rise / 2;
			if (!Sane(t, p, ex, ey)) continue;
			parts[idx] = p;
			const double v = Descend(t, parts, idx, ex, ey, tried);
			if (v > best_fit + (best.roof == ThreeDRoof::Flat ? roof_better : 0.0)) {
				best_fit = v;
				best = parts[idx];
			}
		}
	}
	parts[idx] = best;
	return best_fit;
}

/** The kept shapes, by what they were read from. */
struct ShapeKey {
	SpriteID image;
	int origin_x, origin_y, ex, ey, ez;
	bool operator==(const ShapeKey &) const = default;
};

struct ShapeKeyHash {
	size_t operator()(const ShapeKey &k) const
	{
		uint64_t h = 1469598103934665603ULL;
		for (int64_t v : {static_cast<int64_t>(k.image), static_cast<int64_t>(k.origin_x), static_cast<int64_t>(k.origin_y), static_cast<int64_t>(k.ex), static_cast<int64_t>(k.ey), static_cast<int64_t>(k.ez)}) {
			h ^= static_cast<uint64_t>(v);
			h *= 1099511628211ULL;
		}
		return static_cast<size_t>(h);
	}
};

std::unordered_map<ShapeKey, std::shared_ptr<ThreeDSpriteShape>, ShapeKeyHash> _shapes;

/** Find a building's shape on its sprite's pixels (GetThreeDSpriteShape(); the thread in the background). */
void FindShape(ThreeDSpriteShape &shape, const Target &t, int narrow_x0, int narrow_y0, int narrow_dx, int narrow_dy)
{
	const int ex = shape.ex, ey = shape.ey, ez = shape.ez;
	const float fez = static_cast<float>(ez);

	/* The box drawn before: the bounding box narrowed to the sprite's width (DrawTurnedGenericBox()). */
	const ThreeDPart before{static_cast<float>(narrow_x0), static_cast<float>(narrow_x0 + narrow_dx), static_cast<float>(narrow_y0), static_cast<float>(narrow_y0 + narrow_dy), fez, ThreeDRoof::Flat, 0.0f};
	shape.box_fit = static_cast<float>(Score(t, std::span<const ThreeDPart>(&before, 1)));

	/* A flat top, from three starts. */
	const ThreeDPart starts[] = {
		before,
		{0.0f, static_cast<float>(ex), 0.0f, static_cast<float>(ey), fez, ThreeDRoof::Flat, 0.0f},
		{ex * 0.25f, ex * 0.75f, ey * 0.25f, ey * 0.75f, fez * 0.75f, ThreeDRoof::Flat, 0.0f},
	};
	std::vector<ThreeDPart> parts(1);
	ThreeDPart flat = before;
	double fit = -1;
	for (const ThreeDPart &p : starts) {
		if (!Sane(t, p, ex, ey)) continue;
		parts[0] = p;
		const double v = Descend(t, parts, 0, ex, ey, shape.tried);
		if (v > fit) {
			fit = v;
			flat = parts[0];
		}
	}
	if (fit < 0) return;
	shape.flat_fit = static_cast<float>(fit);
	parts[0] = flat;

	/* A sloping roof over the walls, where it fits clearly better. */
	fit = FitRoof(t, parts, 0, fit, ex, ey, shape.tried, {0.25f, 0.6f});
	shape.one_fit = static_cast<float>(fit);

	/* Where one box does not fit well -- a tower on a base, a church, a house in an L -- another, as the hand tables
	 * have them: the best few of boxes of four sizes at nine places and three heights by one look each, then moved
	 * as the first was, with a roof, and the boxes before moved a little again. Kept only where it fits better by
	 * PART_BETTER; at most MAX_SHAPE_PARTS. */
	const float top = std::max(fez, static_cast<float>(t.top_z) - static_cast<float>(TILE_HEIGHT));
	while (parts.size() < MAX_SHAPE_PARTS && fit < GOOD_ENOUGH) {
		struct Candidate {
			ThreeDPart p;
			double v;
		};
		std::vector<Candidate> candidates;
		for (float size : {0.15f, 0.3f, 0.5f, 0.7f}) {
			for (float cx : {0.25f, 0.5f, 0.75f}) {
				for (float cy : {0.25f, 0.5f, 0.75f}) {
					for (float hs : {0.35f, 0.65f, 1.0f}) {
						const float w = size * ex, d = size * ey;
						const ThreeDPart p{cx * ex - w / 2, cx * ex + w / 2, cy * ey - d / 2, cy * ey + d / 2, hs * top, ThreeDRoof::Flat, 0.0f};
						if (!Sane(t, p, ex, ey)) continue;
						parts.push_back(p);
						candidates.push_back({p, Score(t, parts)});
						parts.pop_back();
						shape.tried++;
					}
				}
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.v > b.v; });
		if (candidates.size() > CANDIDATES_MOVED) candidates.resize(CANDIDATES_MOVED);

		std::vector<ThreeDPart> best_parts;
		double best_fit = fit;
		for (const Candidate &c : candidates) {
			std::vector<ThreeDPart> q = parts;
			q.push_back(c.p);
			const size_t last = q.size() - 1;
			double v = Descend(t, q, last, ex, ey, shape.tried);
			v = FitRoof(t, q, last, v, ex, ey, shape.tried, {0.4f, 1.5f});
			for (size_t i = 0; i < last; i++) v = Descend(t, q, i, ex, ey, shape.tried, {0.5f, 0.25f});
			if (v > best_fit) {
				best_fit = v;
				best_parts = std::move(q);
			}
		}
		if (best_parts.empty() || best_fit < fit + PART_BETTER) break;
		parts = std::move(best_parts);
		fit = best_fit;
	}

	shape.parts = parts;
	shape.fit = static_cast<float>(fit);
	double kept, box_kept;
	Score(t, parts, 0, 0, 0, &kept);
	Score(t, std::span<const ThreeDPart>(&before, 1), 0, 0, 0, &box_kept);
	shape.kept = static_cast<float>(kept);
	shape.box_kept = static_cast<float>(box_kept);
	/* The middle of all the footprints together, what the front snaps about. */
	float fx0 = parts[0].x0, fx1 = parts[0].x1, fy0 = parts[0].y0, fy1 = parts[0].y1;
	for (const ThreeDPart &p : parts) {
		fx0 = std::min(fx0, p.x0);
		fx1 = std::max(fx1, p.x1);
		fy0 = std::min(fy0, p.y0);
		fy1 = std::max(fy1, p.y1);
	}
	shape.centre_x = (fx0 + fx1) / 2.0;
	shape.centre_y = (fy0 + fy1) / 2.0;
	shape.period = std::abs((fx1 - fx0) - (fy1 - fy0)) < 1.0 ? 90.0 : 180.0;
	/* What the sprite has outside the shape is not drawn, facing north neither: no more lost than the box loses and a
	 * little (a mast, a crane, a tree in the garden: a picture of its own, the plan's point 6). */
	shape.ok = fit > shape.box_fit && kept >= std::min(box_kept, KEPT_AT_LEAST);
}

const char *RoofName(ThreeDRoof r)
{
	switch (r) {
		case ThreeDRoof::Flat: return "plocha";
		case ThreeDRoof::GableX: return "sedlova X";
		case ThreeDRoof::GableY: return "sedlova Y";
		case ThreeDRoof::Hip: return "valbova";
	}
	return "?";
}

} // namespace

/**
 * The shape of a building read off its sprite, read the first time and kept.
 * @param image the sprite
 * @param origin_x,origin_y where the sprite hangs (its x, y before the offsets), screen units from the screen point of its bounding box's north corner
 * @param ex,ey,ez its bounding box's extent
 * @param narrow_x0,narrow_y0,narrow_dx,narrow_dy the box drawn before (the bounding box narrowed to the sprite's width), in the bounding box's frame
 * @return the shape; drawn only when it is read (ThreeDSpriteShape::ready) and fits better than that box (ThreeDSpriteShape::ok), else the box as before
 */
const ThreeDSpriteShape *GetThreeDSpriteShape(SpriteID image, int origin_x, int origin_y, int ex, int ey, int ez, int narrow_x0, int narrow_y0, int narrow_dx, int narrow_dy)
{
	const ShapeKey key{image, origin_x, origin_y, ex, ey, ez};
	auto it = _shapes.find(key);
	if (it == _shapes.end()) {
		auto shape = std::make_shared<ThreeDSpriteShape>();
		shape->image = image;
		shape->origin_x = origin_x;
		shape->origin_y = origin_y;
		shape->ex = ex;
		shape->ey = ey;
		shape->ez = ez;
		/* The pixels here (the sprite loader is the main thread's), the shape in the background. */
		const auto t0 = std::chrono::steady_clock::now();
		auto target = std::make_shared<Target>();
		const bool read = ReadTarget(image, origin_x, origin_y, *target);
		shape->read_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (!read) {
			shape->ready.store(true, std::memory_order_release);
		} else {
			ThreeDLoadInBackground([shape, target, narrow_x0, narrow_y0, narrow_dx, narrow_dy] {
				const auto t1 = std::chrono::steady_clock::now();
				FindShape(*shape, *target, narrow_x0, narrow_y0, narrow_dx, narrow_dy);
				shape->ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t1).count();
				shape->ready.store(true, std::memory_order_release);
			});
		}
		it = _shapes.emplace(key, std::move(shape)).first;
	}
	return it->second.get();
}

/**
 * How well some boxes fit a sprite, as the shapes are judged (the rig: a hand table against the shape read off its
 * sprite).
 * @param image the sprite
 * @param origin_x,origin_y where it hangs from its bounding box's north corner (GetThreeDSpriteShape())
 * @param parts the boxes
 * @param dx,dy,dz where their frame lies in the bounding box's
 * @return intersection over union (a pixel left outside twice), 0..1; -1 when the sprite cannot be read
 */
float ThreeDSpriteShapeFit(SpriteID image, int origin_x, int origin_y, std::span<const ThreeDPart> parts, double dx, double dy, double dz)
{
	Target t;
	if (!ReadTarget(image, origin_x, origin_y, t)) return -1;
	return static_cast<float>(Score(t, parts, dx, dy, dz));
}

/** Forget the kept shapes: the sprites are read anew (GfxClearSpriteCache(), GfxInitSpriteMem()), or the rig asks. */
void ForgetThreeDSpriteShapes()
{
	/* A shape the thread is finding still is its own (shared): it is dropped when found. */
	_shapes.clear();
}

/** The rig (test3d tvary): every kept shape, with what it was read from and how well it fits. */
std::string DescribeThreeDSpriteShapes()
{
	std::vector<const ThreeDSpriteShape *> all;
	for (const auto &[key, shape] : _shapes) {
		if (shape->ready.load(std::memory_order_acquire)) all.push_back(shape.get());
	}
	std::sort(all.begin(), all.end(), [](const ThreeDSpriteShape *a, const ThreeDSpriteShape *b) { return a->image < b->image || (a->image == b->image && a->origin_x < b->origin_x); });
	std::string s;
	for (const ThreeDSpriteShape *sh : all) {
		s += fmt::format("tvar spritu {} (visi {},{}): obal {}x{}x{}, {} dilu, shoda {:.0f} % (jeden kvadr {:.0f} %, krabice predtim {:.0f} %, plocha {:.0f} %), drzi {:.0f} % spritu (krabice {:.0f} %), {} zkousek, pixely {:.1f} ms, tvar {:.1f} ms, {}", sh->image, sh->origin_x, sh->origin_y,
				sh->ex, sh->ey, sh->ez, sh->parts.size(), sh->fit * 100, sh->one_fit * 100, sh->box_fit * 100, sh->flat_fit * 100, sh->kept * 100, sh->box_kept * 100, sh->tried, sh->read_ms, sh->ms, sh->ok ? "lepsi nez krabice" : "krabice jako predtim");
		for (const ThreeDPart &p : sh->parts) {
			s += fmt::format("; x {:.2f}..{:.2f} y {:.2f}..{:.2f} vyska {:.2f} strecha {} {:.2f}", p.x0, p.x1, p.y0, p.y1, p.h, RoofName(p.roof), p.rise);
		}
		s += "\n";
	}
	return s;
}

/** The rig and the diary: how many shapes are kept, drawn, and the time it took to read them. */
std::string DescribeThreeDSpriteShapeTotals()
{
	int ready = 0, better = 0, several = 0;
	double fit = 0, box = 0, ms = 0, read_ms = 0;
	for (const auto &[key, shape] : _shapes) {
		read_ms += shape->read_ms;
		if (!shape->ready.load(std::memory_order_acquire)) continue;
		ready++;
		ms += shape->ms;
		if (!shape->ok) continue;
		better++;
		if (shape->parts.size() > 1) several++;
		fit += shape->fit;
		box += shape->box_fit;
	}
	return fmt::format("tvary spritu: {} chteno, {} nalezeno, {} lepsich nez krabice, z toho {} z vic kvadru (shoda prumerne {:.0f} %, krabice predtim {:.0f} %); pixely {:.0f} ms (hlavni vlakno), tvary {:.0f} ms (na pozadi)",
			_shapes.size(), ready, better, several, better > 0 ? fit * 100 / better : 0.0, better > 0 ? box * 100 / better : 0.0, read_ms, ms);
}
