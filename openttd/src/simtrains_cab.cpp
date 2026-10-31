/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file simtrains_cab.cpp The desk of a cab of MSTS over the view from the cab of its engine (simtrains_cab.h). */

#include "stdafx.h"
#include "simtrains_cab.h"
#include "cab_view.h"
#include "vehicle_base.h"
#include "viewport_func.h"
#include "three_d.h"
#include "three_d_vehicle_object.h"
#include "three_d_objects.h"
#include "gfx_func.h"
#include "blitter/factory.hpp"
#include "debug.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "safeguards.h"

/**
 * The cab of MSTS of a vehicle's engine: of its first part drawn from a model of MSTS with a cab (SimtrainsEngine::cvf),
 * read once.
 * @return nullptr none
 */
const SimtrainsCab *SimtrainsCabOf(const Vehicle *v)
{
	if (v == nullptr || !ThreeDEnabled()) return nullptr;
	std::string path;
	for (const Vehicle *u = v->First(); u != nullptr && path.empty(); u = u->Next()) {
		ThreeDEyeVehicle ev;
		if (GetThreeDEyeVehicle(u, ev) && ev.obj != nullptr) path = ev.obj->msts_cab;
	}
	if (path.empty()) return nullptr;
	static std::map<std::string, std::unique_ptr<SimtrainsCab>> read;
	auto [it, added] = read.try_emplace(path);
	if (added) {
		auto cab = std::make_unique<SimtrainsCab>();
		if (ReadSimtrainsCab(path, *cab)) {
			it->second = std::move(cab);
			Debug(grf, 1, "Simtrains: cab {}: {} pictures, {} controls", path, it->second->views.size(), it->second->controls.size());
		} else {
			Debug(grf, 0, "Simtrains: cab {} cannot be read", path);
		}
	}
	return it->second.get();
}

/** The pictures of a desk, by day or by night, premultiplied (SimtrainsCabDesk::Picture). */
static void ReadDeskPictures(SimtrainsCabDesk &desk, const SimtrainsCab &cab, bool night)
{
	desk.path = cab.path;
	desk.night = night;
	desk.pictures.clear();
	desk.fills.clear();
	desk.look_up.clear();
	desk.width = desk.height = 0;
	for (const SimtrainsCab::View &view : cab.views) {
		SimtrainsCabDesk::Picture pic;
		std::vector<uint8_t> rgba;
		const std::string &file = night && !view.night.empty() ? view.night : view.image;
		if (!ReadSimtrainsTexture(file, pic.w, pic.h, rgba) || pic.w <= 0 || pic.h <= 0) {
			Debug(grf, 0, "Simtrains: cab picture {} cannot be read", file);
			pic.w = pic.h = 0;
		} else {
			pic.px.resize(static_cast<size_t>(pic.w) * pic.h);
			for (size_t i = 0; i < pic.px.size(); i++) {
				const uint a = rgba[i * 4 + 3];
				pic.px[i] = Colour(static_cast<uint8_t>(rgba[i * 4] * a / 255), static_cast<uint8_t>(rgba[i * 4 + 1] * a / 255), static_cast<uint8_t>(rgba[i * 4 + 2] * a / 255), static_cast<uint8_t>(a)).data;
			}
		}
		/* The colours beyond its edges: the mean of what is not clear along each. */
		auto mean = [&pic](int x0, int y0, int dx, int dy, int n) {
			uint64_t r = 0, g = 0, b = 0, k = 0;
			for (int i = 0; i < n; i++) {
				const Colour c(pic.px[static_cast<size_t>(y0 + i * dy) * pic.w + x0 + i * dx]);
				if (c.a < 128) continue;
				r += c.r * 255 / c.a;
				g += c.g * 255 / c.a;
				b += c.b * 255 / c.a;
				k++;
			}
			if (k == 0) return Colour(40, 40, 44).data;
			return Colour(static_cast<uint8_t>(r / k), static_cast<uint8_t>(g / k), static_cast<uint8_t>(b / k)).data;
		};
		if (pic.w > 0) {
			desk.fills.push_back(mean(0, 0, 1, 0, pic.w));
			desk.fills.push_back(mean(0, pic.h - 1, 1, 0, pic.w));
			desk.fills.push_back(mean(0, 0, 0, 1, pic.h));
			desk.fills.push_back(mean(pic.w - 1, 0, 0, 1, pic.h));
		} else {
			desk.fills.insert(desk.fills.end(), 4, Colour(40, 40, 44).data);
		}
		desk.pictures.push_back(std::move(pic));
	}
}

/**
 * Which pixel of which picture of the desk each pixel of the window shows, the head turned as it is (SimtrainsCabDesk).
 * Each picture is a flat board before the eye where MSTS sees it ("Direction"), as big as MSTS's screen sees: 45 degrees
 * up and down, four thirds of that across. MSTS shows one at a time; turned freely, the eye would see gaps between them
 * (the 451's: ahead from -31 to 27 degrees, to the right from 46): a board further from ahead is widened to meet the one
 * nearer, so the desk goes round without a gap and the one ahead is as MSTS draws it. Above, below and past the last
 * boards the cab goes on in the colour of the edge.
 */
static void MakeDeskLookUp(SimtrainsCabDesk &desk, const SimtrainsCab &cab, const CabViewCamera &camera, int width, int height)
{
	desk.width = width;
	desk.height = height;
	desk.head = camera.head;
	desk.pitch = camera.pitch;
	desk.look_up.assign(static_cast<size_t>(width) * height, SimtrainsCabDesk::NONE);
	const double focal = CabFocal(camera, width, height);
	const double cx = width / 2.0, cy = height * 0.5 + std::tan(camera.pitch) * focal;
	const double half_h = MSTS_CAB_TAN, half_w = std::atan(MSTS_CAB_TAN * 4.0 / 3.0);

	/* The boards round the eye, left to right: their edges, radians from ahead, to the right positive. */
	struct Board {
		int picture;
		double left, right, lift;
	};
	std::vector<Board> boards;
	for (size_t i = 0; i < cab.views.size() && i < desk.pictures.size(); i++) {
		if (desk.pictures[i].w == 0) continue;
		const double c = cab.views[i].yaw * M_PI / 180.0;
		boards.push_back({static_cast<int>(i), c - half_w, c + half_w, std::tan(cab.views[i].pitch * M_PI / 180.0)});
	}
	if (boards.empty()) return;
	std::sort(boards.begin(), boards.end(), [](const Board &a, const Board &b) { return a.left + a.right < b.left + b.right; });
	for (size_t i = 0; i + 1 < boards.size(); i++) {
		Board &a = boards[i], &b = boards[i + 1];
		/* The one further from ahead meets the nearer, over a gap or under an overlap. */
		if (std::abs(a.left + a.right) <= std::abs(b.left + b.right)) {
			b.left = a.right;
		} else {
			a.right = b.left;
		}
	}

	/* The head left is positive (CabViewCamera::head); MSTS turns to the right positive. */
	const double look = -camera.head;
	for (int x = 0; x < width; x++) {
		const double r = (x + 0.5 - cx) / focal;
		const double phi = std::remainder(look + std::atan(r), 2 * M_PI);
		/* Which board, and where across it: in the tangent of the angle from its middle, as a flat board is seen. */
		int fill = -1;
		const Board *on = nullptr;
		if (phi < boards.front().left) {
			fill = boards.front().picture * 4 + 2;
		} else if (phi >= boards.back().right) {
			fill = boards.back().picture * 4 + 3;
		} else {
			for (const Board &b : boards) {
				if (phi >= b.left && phi < b.right) on = &b;
			}
		}
		if (on == nullptr && fill < 0) continue;
		const SimtrainsCabDesk::Picture *pic = on != nullptr ? &desk.pictures[on->picture] : nullptr;
		int tx = 0;
		double over = 1.0, lift = 0;
		if (on != nullptr) {
			const double mid = (on->left + on->right) / 2, half = (on->right - on->left) / 2;
			const double fu = std::clamp(0.5 + std::tan(phi - mid) / (2 * std::tan(half)), 0.0, 1.0);
			tx = std::min(pic->w - 1, static_cast<int>(fu * pic->w));
			/* Up the board: the ray's height over its way along the floor, at the board a unit before the eye. */
			over = 1.0 / (std::sqrt(1 + r * r) * std::cos(phi - mid));
			lift = on->lift;
		}
		for (int y = 0; y < height; y++) {
			uint32_t &out = desk.look_up[static_cast<size_t>(y) * width + x];
			if (on == nullptr) {
				out = SimtrainsCabDesk::FILL | static_cast<uint32_t>(fill);
				continue;
			}
			const double u = (cy - (y + 0.5)) / focal;
			const double fv = 0.5 - (u * over + lift) / (2 * half_h);
			if (fv < 0 || fv >= 1) {
				out = SimtrainsCabDesk::FILL | static_cast<uint32_t>(on->picture * 4 + (fv < 0 ? 0 : 1));
				continue;
			}
			const int ty = std::min(pic->h - 1, static_cast<int>(fv * pic->h));
			out = static_cast<uint32_t>(on->picture) << 28 | static_cast<uint32_t>(ty * pic->w + tx);
		}
	}
}

/**
 * Draw the desk of a cab of MSTS over the view from the cab just drawn into the window (BlitEyePicture()): over its
 * pixels, or into the hole the graphics card's picture shows through, premultiplied so that the video driver lays the
 * card's picture under it where the desk's windows are clear.
 * @param left where the picture is in the window
 * @return false when there is nothing to draw
 */
bool DrawSimtrainsCabDesk(SimtrainsCabDesk &desk, const SimtrainsCab &cab, const CabViewCamera &camera, int left, int top, int width, int height)
{
	if (width <= 0 || height <= 0 || cab.views.empty()) return false;
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return false;
	const bool night = IsNightNow();
	if (desk.path != cab.path || desk.night != night || desk.pictures.size() != cab.views.size()) ReadDeskPictures(desk, cab, night);
	if (desk.width != width || desk.height != height || desk.head != camera.head || desk.pitch != camera.pitch) MakeDeskLookUp(desk, cab, camera, width, height);

	const DrawPixelInfo *dpi = _cur_dpi;
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	const int x0 = std::max(left, dpi->left), x1 = std::min(left + width, dpi->left + dpi->width);
	const int y0 = std::max(top, dpi->top), y1 = std::min(top + height, dpi->top + dpi->height);
	if (x0 >= x1 || y0 >= y1) return true;
	for (int y = y0; y < y1; y++) {
		const uint32_t *look = &desk.look_up[static_cast<size_t>(y - top) * width + (x0 - left)];
		uint32_t *dst = static_cast<uint32_t *>(blitter->MoveTo(dpi->dst_ptr, x0 - dpi->left, y - dpi->top));
		for (int x = x0; x < x1; x++, look++, dst++) {
			if (*look == SimtrainsCabDesk::NONE) continue;
			if ((*look & SimtrainsCabDesk::FILL) == SimtrainsCabDesk::FILL) {
				*dst = desk.fills[*look & 0x0FFFFFFF];
				continue;
			}
			const SimtrainsCabDesk::Picture &pic = desk.pictures[*look >> 28];
			const Colour p(pic.px[*look & 0x0FFFFFFF]);
			if (p.a == 0) continue;
			if (p.a == 255) {
				*dst = p.data;
				continue;
			}
			/* Over what is there, premultiplied: over the card's hole (all nought) it stays premultiplied for the driver. */
			const Colour d(*dst);
			const uint k = 255 - p.a;
			*dst = Colour(static_cast<uint8_t>(p.r + d.r * k / 255), static_cast<uint8_t>(p.g + d.g * k / 255), static_cast<uint8_t>(p.b + d.b * k / 255), static_cast<uint8_t>(p.a + d.a * k / 255)).data;
		}
	}
	return true;
}
