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
 * (a tower's, three units across or less, may be a spire)
 * (the idea, and the 2.5, from Václav Loubek's cab view, vaclavloubek/OpenTTD, 2026; the code is this game's own).
 * Where one box does not fit well (a tower on a base, a church, a house in an L), more boxes are added the same way,
 * one at a time, up to four, each kept only where it fits clearly better.
 * A shape is read once per sprite and where it hangs in its bounding box, and kept: the sprite's pixels here, the
 * shape by the thread in the background (ThreeDLoadInBackground()).
 */

#include "stdafx.h"
#include "three_d_shape.h"
#include "gfx_func.h"
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

/** The steepest roof kept: its rise over half its span (Loubek's 2.5 : 1). */
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

/*
 * The walls a building's sprite never shows (facing -X and -Y; the plan's point 3). The game draws them from a wall the
 * sprite shows: where that wall repeats a pattern -- a window and the wall beside it -- one repeat, the one most like
 * the others, laid along the hidden wall as many times as fit; a door or a shop's sign is on the front only. The
 * pattern is read on the sprite's own pixels, in world units, the same at every zoom.
 */

/** How far apart the columns of a wall are looked at, units along it (half a pixel of the normal zoom). */
constexpr double WALL_STEP = 0.25;

/** How far apart the rows, units of height (half a pixel). */
constexpr double WALL_ROW = 0.5;

/** The rows from this height up only: below, the sprite's bottom edge steps with the ground (BUILDING_WALL_BAND, viewport.cpp). */
constexpr double WALL_FROM = 1.5;

/** The shortest repeat looked for: one unit, two pixels. */
constexpr int SHORTEST_REPEAT = 4;

/**
 * The shortest piece laid on a hidden wall: two units, four pixels, as many repeats as make it up. A shorter piece holds
 * no window and the wall beside it: the floors' bands match at any shift, a repeat of one unit is found on them, and
 * the hidden wall came out as stripes.
 */
constexpr int SHORTEST_PIECE = 8;

/**
 * A repeat is the wall's pattern when its columns differ from those one repeat on by at most this share of what any two
 * of the wall's pixels differ by (the 8bpp walls' dithering keeps windows that match well above nothing).
 */
constexpr double REPEATS = 0.6;

/** A wall whose pixels differ this little (the squares of red, green and blue together, on average) is plain: any piece of it repeats. */
constexpr double PLAIN_WALL = 200.0;

/** A sprite's pixels as they are, the finest level its file has: the patterns of its walls are read on them. */
struct Picture {
	int x0 = 0, y0 = 0;        ///< screen units (ZoomLevel::Min) of its top left corner from the screen point of the boxes' origin
	int scale = 1;             ///< screen units per pixel
	int w = 0, h = 0;
	std::vector<uint32_t> px;  ///< alpha << 24 | red << 16 | green << 8 | blue
};

/** Read a sprite's pixels, the finest level its file has (8bpp in the colours of the palette). */
bool ReadPicture(SpriteID image, int origin_x, int origin_y, Picture &p)
{
	SpriteLoader::SpriteCollection collection;
	bool is_32bpp;
	const ZoomLevels levels = ReadRawSprite(image, collection, is_32bpp);
	ZoomLevel use = ZoomLevel::End;
	for (ZoomLevel z : levels) {
		if (use == ZoomLevel::End || z < use) use = z;
	}
	if (use == ZoomLevel::End) return false;
	const SpriteLoader::Sprite &s = collection[use];
	if (s.width == 0 || s.height == 0 || s.data == nullptr) return false;
	p.scale = 1 << to_underlying(use);
	p.x0 = origin_x + s.x_offs * p.scale;
	p.y0 = origin_y + s.y_offs * p.scale;
	p.w = s.width;
	p.h = s.height;
	p.px.assign(static_cast<size_t>(p.w) * p.h, 0);
	for (size_t i = 0; i < p.px.size(); i++) {
		const SpriteLoader::CommonPixel &c = s.data[i];
		uint32_t a, r, g, b;
		if (is_32bpp) {
			a = c.a;
			if (c.m != 0) {
				const Colour &pal = _cur_palette.palette[c.m];
				r = pal.r;
				g = pal.g;
				b = pal.b;
			} else {
				r = c.r;
				g = c.g;
				b = c.b;
			}
		} else {
			if (c.m == 0) continue;
			const Colour &pal = _cur_palette.palette[c.m];
			a = 255;
			r = pal.r;
			g = pal.g;
			b = pal.b;
		}
		p.px[i] = a << 24 | r << 16 | g << 8 | b;
	}
	return true;
}

/** The pixel of the sprite the game draws a world point of the boxes' frame at; 0 outside the sprite. */
inline uint32_t PixelAt(const Picture &p, double x, double y, double z)
{
	const double sx = (y - x) * 2 * ZOOM_BASE, sy = (x + y - z) * ZOOM_BASE;
	const int i = static_cast<int>(std::floor((sx - p.x0) / p.scale)), j = static_cast<int>(std::floor((sy - p.y0) / p.scale));
	if (i < 0 || j < 0 || i >= p.w || j >= p.h) return 0;
	return p.px[static_cast<size_t>(j) * p.w + i];
}

/** How far apart two colours are: the squares of the differences of red, green and blue together. */
inline int ColourDiff(uint32_t a, uint32_t b)
{
	const int dr = static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
	const int dg = static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF);
	const int db = static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF);
	return dr * dr + dg * dg + db * db;
}

/** How high a part's roof is over a point of its footprint, above its walls (as AddBoxFaces() draws it, MARGIN outside its numbers). */
double RoofOver(const ThreeDPart &p, double x, double y)
{
	const double x0 = p.x0 - MARGIN, x1 = p.x1 + MARGIN, y0 = p.y0 - MARGIN, y1 = p.y1 + MARGIN;
	const double hx = (x1 - x0) / 2, hy = (y1 - y0) / 2, dx = hx - std::abs(x - (x0 + x1) / 2), dy = hy - std::abs(y - (y0 + y1) / 2);
	double share = 0;
	switch (p.roof) {
		case ThreeDRoof::Flat: return 0;
		case ThreeDRoof::GableX: share = dy / hy; break;
		case ThreeDRoof::GableY: share = dx / hx; break;
		case ThreeDRoof::Hip: share = std::min(dx, dy) / std::min(hx, hy); break;
	}
	return p.rise * std::clamp(share, 0.0, 1.0);
}

/**
 * Whether another part stands in front of a point of a part's wall in the game's view: the line from the point to the
 * eye ((1, 1, 2) in the world: the game's view moves no pixel along it) passes through it.
 */
bool HiddenByOther(std::span<const ThreeDPart> parts, size_t idx, double x, double y, double z)
{
	for (size_t j = 0; j < parts.size(); j++) {
		if (j == idx) continue;
		const ThreeDPart &p = parts[j];
		const double x0 = p.x0 - MARGIN, x1 = p.x1 + MARGIN, y0 = p.y0 - MARGIN, y1 = p.y1 + MARGIN, h = p.h + 2 * MARGIN;
		/* Where the line is over the part's footprint and below its top. */
		const double t0 = std::max({0.0, x0 - x, y0 - y, -z / 2}), t1 = std::min({x1 - x, y1 - y, (h + p.rise - z) / 2});
		if (t0 > t1) continue;
		if (p.rise <= 0 || z + 2 * t0 <= h) return true;
		for (double t = t0; t <= t1; t += WALL_STEP / 2) {
			if (z + 2 * t <= h + RoofOver(p, x + t, y + t)) return true;
		}
	}
	return false;
}

/** What a wall the sprite shows looks like (LookAtWall()). */
struct WallLook {
	bool seen = false;     ///< whether the sprite shows enough of it
	bool periodic = false; ///< whether it repeats a pattern
	double start = 0;      ///< the piece to lay on a hidden wall: one repeat, else all of it the sprite shows (units along it)
	double length = 0;
	double whole = 0;      ///< the share of it the sprite shows
	double ratio = 1;      ///< how the repeat found differs, of how any two of its pixels do (the rig)
	int tried = 0;         ///< the repeat looked at last, columns (the rig)
};

/**
 * Read a wall of a part the sprite shows (axis 0 the wall facing +X, 1 facing +Y): its columns, the longest run of them
 * the sprite shows, the repeat of its pattern and the repeat most like the others.
 */
WallLook LookAtWall(const Picture &pic, std::span<const ThreeDPart> parts, size_t idx, int axis, bool cut, double cx0, double cx1, double cy0, double cy1)
{
	WallLook look;
	const ThreeDPart &p = parts[idx];
	const double x1 = p.x1 + MARGIN, y1 = p.y1 + MARGIN, h = p.h + 2 * MARGIN;
	const double c0 = axis == 0 ? p.y0 - MARGIN : p.x0 - MARGIN, c1 = axis == 0 ? y1 : x1;
	const int cols = static_cast<int>((c1 - c0) / WALL_STEP), rows = static_cast<int>((h - WALL_FROM) / WALL_ROW);
	if (cols < SHORTEST_REPEAT || rows < 2) return look;
	/* A house of several tiles: this tile's sprite shows only what is on this tile. */
	const double lo = !cut ? -1e9 : (axis == 0 ? cy0 : cx0), hi = !cut ? 1e9 : (axis == 0 ? cy1 : cx1);
	if (cut && (axis == 0 ? (x1 < cx0 || x1 > cx1) : (y1 < cy0 || y1 > cy1))) return look;

	/* The pixels of its columns, 0 where the sprite does not show it (empty, or another part in front). */
	std::vector<uint32_t> s(static_cast<size_t>(cols) * rows, 0);
	std::vector<uint8_t> shown(cols, 0);
	int shown_cols = 0;
	for (int k = 0; k < cols; k++) {
		const double c = c0 + (k + 0.5) * WALL_STEP;
		if (c < lo || c > hi) continue;
		const double x = axis == 0 ? x1 : c, y = axis == 0 ? c : y1;
		int valid = 0;
		for (int j = 0; j < rows; j++) {
			const double z = WALL_FROM + (j + 0.5) * WALL_ROW;
			const uint32_t v = PixelAt(pic, x, y, z);
			if ((v >> 24) < 128 || HiddenByOther(parts, idx, x, y, z)) continue;
			s[static_cast<size_t>(k) * rows + j] = v | 0xFF000000u;
			valid++;
		}
		if (valid * 10 >= rows * 6) {
			shown[k] = 1;
			shown_cols++;
		}
	}
	look.whole = static_cast<double>(shown_cols) / cols;

	/* The longest run of columns it shows. */
	int k0 = 0, k1 = 0;
	for (int k = 0; k < cols;) {
		if (!shown[k]) {
			k++;
			continue;
		}
		int e = k;
		while (e < cols && shown[e]) e++;
		if (e - k > k1 - k0) {
			k0 = k;
			k1 = e;
		}
		k = e;
	}
	const int n = k1 - k0;
	if (n < SHORTEST_REPEAT) return look;
	look.seen = true;
	look.start = c0 + k0 * WALL_STEP;
	look.length = n * WALL_STEP;

	/* How much columns a and b on differ, len of them: the mean over the pixels both show; -1 where none. */
	auto differ = [&](int a, int b, int len) {
		double sum = 0;
		int count = 0;
		for (int i = 0; i < len; i++) {
			const uint32_t *ca = &s[static_cast<size_t>(a + i) * rows], *cb = &s[static_cast<size_t>(b + i) * rows];
			for (int j = 0; j < rows; j++) {
				if (ca[j] == 0 || cb[j] == 0) continue;
				sum += ColourDiff(ca[j], cb[j]);
				count++;
			}
		}
		return count > 0 ? sum / count : -1.0;
	};

	/* What any two of its pixels differ by: twice the variance of red, green and blue together. */
	double mean[3] = {0, 0, 0}, sq[3] = {0, 0, 0};
	int count = 0;
	for (int k = k0; k < k1; k++) {
		for (int j = 0; j < rows; j++) {
			const uint32_t v = s[static_cast<size_t>(k) * rows + j];
			if (v == 0) continue;
			for (int ch = 0; ch < 3; ch++) {
				const double c = (v >> (16 - 8 * ch)) & 0xFF;
				mean[ch] += c;
				sq[ch] += c * c;
			}
			count++;
		}
	}
	if (count == 0) return look;
	double any = 0;
	for (int ch = 0; ch < 3; ch++) {
		const double m = mean[ch] / count;
		any += 2 * std::max(0.0, sq[ch] / count - m * m);
	}

	/* The repeat: the shift its columns differ least by from the columns that far on; of that, a half, a third ... where
	 * that differs hardly more (two repeats of the windows match as well as one); as many of it as make SHORTEST_PIECE. */
	int repeat = 0;
	if (any < PLAIN_WALL) {
		repeat = std::min(n, SHORTEST_PIECE);
		look.ratio = 0;
	} else if (n >= 2 * SHORTEST_REPEAT) {
		std::vector<double> d(n / 2 + 2, -1.0);
		int best = 0;
		for (int lag = SHORTEST_REPEAT; lag <= n / 2; lag++) {
			d[lag] = differ(k0, k0 + lag, n - lag);
			if (d[lag] >= 0 && (best == 0 || d[lag] < d[best])) best = lag;
		}
		if (best != 0) {
			for (int div = best / SHORTEST_REPEAT; div >= 2; div--) {
				const int around = static_cast<int>(std::lround(static_cast<double>(best) / div));
				int fit = 0;
				for (int lag = std::max(SHORTEST_REPEAT, around - 1); lag <= std::min(n / 2, around + 1); lag++) {
					if (d[lag] >= 0 && (fit == 0 || d[lag] < d[fit])) fit = lag;
				}
				if (fit != 0 && d[fit] <= d[best] * 1.08 + 0.005 * any) {
					best = fit;
					break;
				}
			}
			look.ratio = d[best] / any;
			look.tried = best;
			if (look.ratio <= REPEATS) {
				repeat = best;
				while (repeat < SHORTEST_PIECE && repeat + best <= n) repeat += best;
			}
		}
	}
	if (repeat == 0) return look;

	/* The repeat most like the others: a door or a sign differs from the windows beside it. */
	int best = -1;
	double best_cost = 1e30;
	const double middle = (k0 + k1 - repeat) / 2.0;
	for (int a = k0; a + repeat <= k1; a++) {
		double sum = 0;
		int compared = 0;
		for (int r = -3; r <= 3; r++) {
			const int b = a + r * repeat;
			if (r == 0 || b < k0 || b + repeat > k1) continue;
			const double v = differ(a, b, repeat);
			if (v < 0) continue;
			sum += v;
			compared++;
		}
		if (compared == 0) continue;
		/* Of two alike, the one nearer the middle (away from the corners). */
		const double cost = sum / compared + 1e-3 * std::abs(a - middle);
		if (cost < best_cost) {
			best_cost = cost;
			best = a;
		}
	}
	if (best < 0) return look;
	look.periodic = true;
	look.start = c0 + best * WALL_STEP;
	look.length = repeat * WALL_STEP;
	return look;
}

/** Read the walls of a building's boxes its sprite shows, and choose where each hidden wall takes its pixels from (the thread in the background). */
void FindWallPatterns(ThreeDWallPatterns &w, const Picture &pic, std::span<const ThreeDPart> parts, bool cut, double cx0, double cx1, double cy0, double cy1)
{
	w.back.assign(parts.size(), {});
	for (size_t i = 0; i < parts.size(); i++) {
		const WallLook look[2] = {
			LookAtWall(pic, parts, i, 0, cut, cx0, cx1, cy0, cy1),
			LookAtWall(pic, parts, i, 1, cut, cx0, cx1, cy0, cy1),
		};
		for (int axis = 0; axis < 2; axis++) {
			const WallLook &l = look[axis];
			if (!l.seen) {
				w.said += fmt::format(" dil {} stena {}: neni videt ({:.0f} %);", i, axis == 0 ? "+X" : "+Y", l.whole * 100);
			} else if (l.periodic) {
				w.said += fmt::format(" dil {} stena {}: vzor {:.2f} od {:.2f} (lisi se {:.0f} %, videt {:.0f} %);", i, axis == 0 ? "+X" : "+Y", l.length, l.start, l.ratio * 100, l.whole * 100);
			} else {
				w.said += fmt::format(" dil {} stena {}: bez vzoru (nejblizsi {:.2f} lisi se {:.0f} %), cela {:.2f} od {:.2f} (videt {:.0f} %);", i, axis == 0 ? "+X" : "+Y", l.tried * WALL_STEP, l.ratio * 100, l.length, l.start, l.whole * 100);
			}
		}
		/* Each hidden wall: from its opposite where that repeats, else from the other where that does, else from all of
		 * the opposite the sprite shows, else of the other. */
		for (int back = 0; back < 2; back++) {
			const WallLook &opposite = look[back], &other = look[1 - back];
			int from = -1;
			if (opposite.periodic) {
				from = back;
			} else if (other.periodic) {
				from = 1 - back;
			} else if (opposite.seen) {
				from = back;
			} else if (other.seen) {
				from = 1 - back;
			}
			ThreeDWallSource &src = w.back[i][back];
			src.mirror = opposite.whole >= 0.9;
			if (from < 0) continue;
			src.axis = static_cast<int8_t>(from);
			src.periodic = look[from].periodic;
			src.start = static_cast<float>(look[from].start);
			src.length = static_cast<float>(look[from].length);
		}
	}
}

/** The kept wall patterns, by what they were read from (GetThreeDWallPatterns()). */
std::unordered_map<uint64_t, std::shared_ptr<ThreeDWallPatterns>> _wall_patterns;

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

/**
 * Where the walls a building's sprite never shows (facing -X and -Y) take their pixels from, read the first time and
 * kept: the sprite's pixels here (the sprite loader is the main thread's), the walls in the background.
 * @param image the sprite
 * @param origin_x,origin_y where the sprite hangs (its x, y before the offsets), screen units from the screen point of the boxes' origin
 * @param parts the boxes, in their frame (world units, heights up from its ground)
 * @param cut whether the boxes are cut to the rectangle cx0..cx1, cy0..cy1 (one tile of a house of several: the sprite shows that tile only)
 * @return the walls; used only once read (ThreeDWallPatterns::ready), mirrored as before meanwhile
 */
const ThreeDWallPatterns *GetThreeDWallPatterns(SpriteID image, int origin_x, int origin_y, std::span<const ThreeDPart> parts, bool cut, double cx0, double cx1, double cy0, double cy1)
{
	uint64_t key = 1469598103934665603ULL;
	auto mix = [&key](int64_t v) {
		key ^= static_cast<uint64_t>(v);
		key *= 1099511628211ULL;
	};
	mix(image);
	mix(origin_x);
	mix(origin_y);
	mix(cut ? 1 : 0);
	if (cut) {
		for (double v : {cx0, cx1, cy0, cy1}) mix(std::lround(v * 64));
	}
	for (const ThreeDPart &p : parts) {
		for (float v : {p.x0, p.x1, p.y0, p.y1, p.h, p.rise}) mix(std::lround(v * 64));
		mix(static_cast<int64_t>(p.roof));
	}
	auto it = _wall_patterns.find(key);
	if (it == _wall_patterns.end()) {
		auto w = std::make_shared<ThreeDWallPatterns>();
		w->image = image;
		const auto t0 = std::chrono::steady_clock::now();
		auto pic = std::make_shared<Picture>();
		const bool read = ReadPicture(image, origin_x, origin_y, *pic);
		w->read_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (!read) {
			w->ready.store(true, std::memory_order_release);
		} else {
			ThreeDLoadInBackground([w, pic, boxes = std::vector<ThreeDPart>(parts.begin(), parts.end()), cut, cx0, cx1, cy0, cy1] {
				const auto t1 = std::chrono::steady_clock::now();
				FindWallPatterns(*w, *pic, boxes, cut, cx0, cx1, cy0, cy1);
				w->ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t1).count();
				w->ready.store(true, std::memory_order_release);
			});
		}
		it = _wall_patterns.emplace(key, std::move(w)).first;
	}
	return it->second.get();
}

/** Forget the kept shapes and wall patterns: the sprites are read anew (GfxClearSpriteCache(), GfxInitSpriteMem()), or the rig asks. */
void ForgetThreeDSpriteShapes()
{
	/* A shape the thread is finding still is its own (shared): it is dropped when found. */
	_shapes.clear();
	_wall_patterns.clear();
}

/** The rig (test3d tvary): the walls the sprites show, what was found on them, and where the hidden ones take their pixels from. */
std::string DescribeThreeDWallPatterns()
{
	std::vector<const ThreeDWallPatterns *> all;
	for (const auto &[key, w] : _wall_patterns) {
		if (w->ready.load(std::memory_order_acquire)) all.push_back(w.get());
	}
	std::sort(all.begin(), all.end(), [](const ThreeDWallPatterns *a, const ThreeDWallPatterns *b) { return a->image < b->image; });
	std::string s;
	int walls = 0, periodic = 0, whole = 0, mirrored = 0;
	double ms = 0, read_ms = 0;
	for (const ThreeDWallPatterns *w : all) {
		s += fmt::format("zadni steny spritu {}:{} pixely {:.1f} ms, steny {:.1f} ms\n", w->image, w->said, w->read_ms, w->ms);
		ms += w->ms;
		read_ms += w->read_ms;
		for (const auto &part : w->back) {
			for (const ThreeDWallSource &src : part) {
				walls++;
				if (src.axis < 0) {
					mirrored++;
				} else if (src.periodic) {
					periodic++;
				} else {
					whole++;
				}
			}
		}
	}
	s += fmt::format("zadni steny: {} spritu, {} sten: vzorem {}, celou viditelnou {}, zrcadlem {}; pixely {:.0f} ms (hlavni vlakno), steny {:.0f} ms (na pozadi)\n", all.size(), walls, periodic, whole, mirrored, read_ms, ms);
	return s;
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
