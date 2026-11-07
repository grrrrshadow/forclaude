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
#include "train.h"
#include "command_func.h"
#include "vehicle_cmd.h"
#include "company_func.h"
#include "strings_func.h"
#include "core/backup_type.hpp"
#include "settings_type.h"
#include "table/strings.h"
#include "pathfinder/follow_track.hpp"
#include "pbs.h"
#include "rail_map.h"
#include "track_func.h"
#include "sound_func.h"
#include "timer/timer_game_tick.h"
#include "3rdparty/fmt/chrono.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
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
	desk.base.clear();
	desk.drawn.clear();
	desk.shown_valid = false;
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

/* ---- The controls of the desk drawn over its first picture (SimtrainsCab::Control, SimtrainsDeskState) ---- */

namespace {

/** A picture of a control (its "Graphic"), premultiplied (Colour::data), read once. */
struct ControlGraphic {
	int w = 0, h = 0;
	std::vector<uint32_t> px;
};

const ControlGraphic *ControlGraphicOf(const std::string &path)
{
	if (path.empty()) return nullptr;
	static std::map<std::string, ControlGraphic> read;
	auto [it, added] = read.try_emplace(path);
	if (added) {
		ControlGraphic &g = it->second;
		std::vector<uint8_t> rgba;
		if (!ReadSimtrainsTexture(path, g.w, g.h, rgba) || g.w <= 0 || g.h <= 0) {
			Debug(grf, 1, "Simtrains: control picture {} cannot be read", path);
			g = {};
		} else {
			g.px.resize(static_cast<size_t>(g.w) * g.h);
			for (size_t i = 0; i < g.px.size(); i++) {
				const uint a = rgba[i * 4 + 3];
				g.px[i] = Colour(static_cast<uint8_t>(rgba[i * 4] * a / 255), static_cast<uint8_t>(rgba[i * 4 + 1] * a / 255), static_cast<uint8_t>(rgba[i * 4 + 2] * a / 255), static_cast<uint8_t>(a)).data;
			}
		}
	}
	return it->second.w > 0 ? &it->second : nullptr;
}

/** A premultiplied pixel over another. */
inline uint32_t Over(uint32_t src, uint32_t dst)
{
	const Colour s(src);
	if (s.a == 255) return src;
	if (s.a == 0) return dst;
	const Colour d(dst);
	const uint k = 255 - s.a;
	return Colour(static_cast<uint8_t>(s.r + d.r * k / 255), static_cast<uint8_t>(s.g + d.g * k / 255), static_cast<uint8_t>(s.b + d.b * k / 255), static_cast<uint8_t>(s.a + d.a * k / 255)).data;
}

/**
 * A pixel of a part of a graphic, between its pixels as far as it lies between them; nothing outside the part.
 * @param u,v where, pixels of the part from its top left
 * @param x0,y0,w,h the part
 */
uint32_t Sample(const ControlGraphic &g, float u, float v, int x0, int y0, int w, int h)
{
	const float fu = u - 0.5f, fv = v - 0.5f;
	const int iu = static_cast<int>(std::floor(fu)), iv = static_cast<int>(std::floor(fv));
	const float au = fu - iu, av = fv - iv;
	float r = 0, gr = 0, b = 0, a = 0;
	for (int k = 0; k < 4; k++) {
		const int x = iu + (k & 1), y = iv + (k >> 1);
		if (x < 0 || y < 0 || x >= w || y >= h) continue;
		const float wt = ((k & 1) != 0 ? au : 1 - au) * ((k >> 1) != 0 ? av : 1 - av);
		const Colour c(g.px[static_cast<size_t>(y0 + y) * g.w + x0 + x]);
		r += c.r * wt;
		gr += c.g * wt;
		b += c.b * wt;
		a += c.a * wt;
	}
	auto byte = [](float f) { return static_cast<uint8_t>(std::clamp(static_cast<int>(f + 0.5f), 0, 255)); };
	return Colour(byte(r), byte(gr), byte(b), byte(a)).data;
}

/** Draws the controls into the first picture of the desk, its places those of MSTS's screen of 640 by 480 (SimtrainsCab). */
struct DeskPainter {
	SimtrainsCabDesk::Picture &pic;
	std::vector<std::array<int, 4>> &drawn; ///< what was drawn over, to be put back from the picture as read
	float sx, sy; ///< the picture's pixels a pixel of MSTS's screen

	/** A rect of MSTS's screen to the picture's pixels, within it, told drawn over: left, top, right, bottom. */
	std::array<int, 4> Mark(float x, float y, float w, float h)
	{
		const std::array<int, 4> r = {
			std::clamp(static_cast<int>(std::floor(x * this->sx)), 0, this->pic.w), std::clamp(static_cast<int>(std::floor(y * this->sy)), 0, this->pic.h),
			std::clamp(static_cast<int>(std::ceil((x + w) * this->sx)), 0, this->pic.w), std::clamp(static_cast<int>(std::ceil((y + h) * this->sy)), 0, this->pic.h)};
		if (r[2] > r[0] && r[3] > r[1]) this->drawn.push_back(r);
		return r;
	}

	/** A part of a graphic (fx, fy, fw, fh, its pixels) laid stretched over a rect of MSTS's screen. */
	void Lay(const ControlGraphic &g, int fx, int fy, int fw, int fh, float x, float y, float w, float h)
	{
		if (fw <= 0 || fh <= 0 || w <= 0 || h <= 0) return;
		const std::array<int, 4> r = this->Mark(x, y, w, h);
		const float x0 = x * this->sx, y0 = y * this->sy, pw = w * this->sx, ph = h * this->sy;
		for (int py = r[1]; py < r[3]; py++) {
			const float v = (py + 0.5f - y0) / ph * fh;
			uint32_t *row = &this->pic.px[static_cast<size_t>(py) * this->pic.w];
			for (int px = r[0]; px < r[2]; px++) {
				const float u = (px + 0.5f - x0) / pw * fw;
				row[px] = Over(Sample(g, u, v, fx, fy, fw, fh), row[px]);
			}
		}
	}

	/** A rect of MSTS's screen filled with a colour (0xRRGGBB). */
	void Fill(float x, float y, float w, float h, uint32_t colour)
	{
		const std::array<int, 4> r = this->Mark(x, y, w, h);
		const uint32_t c = Colour(static_cast<uint8_t>(colour >> 16), static_cast<uint8_t>(colour >> 8), static_cast<uint8_t>(colour)).data;
		for (int py = r[1]; py < r[3]; py++) std::fill(&this->pic.px[static_cast<size_t>(py) * this->pic.w + r[0]], &this->pic.px[static_cast<size_t>(py) * this->pic.w + r[2]], c);
	}

	/**
	 * A dial's needle (MSTS's "Dial"): pointing up it lies over x, y, w, h of MSTS's screen; it turns about the point
	 * pivot below its top in the middle of its width.
	 * @param degrees turned from up, clockwise
	 */
	void Needle(const ControlGraphic &g, float x, float y, float w, float h, float pivot, float degrees)
	{
		if (w <= 0 || h <= 0) return;
		const float cx = x + w / 2, cy = y + pivot;
		const float reach = std::max(pivot, h - pivot) + w;
		const std::array<int, 4> r = this->Mark(cx - reach, cy - reach, 2 * reach, 2 * reach);
		const float a = degrees * static_cast<float>(M_PI) / 180.0f, c = std::cos(a), s = std::sin(a);
		for (int py = r[1]; py < r[3]; py++) {
			const float dy = (py + 0.5f) / this->sy - cy;
			uint32_t *row = &this->pic.px[static_cast<size_t>(py) * this->pic.w];
			for (int px = r[0]; px < r[2]; px++) {
				const float dx = (px + 0.5f) / this->sx - cx;
				/* Back to the needle pointing up: across it, and down it from the pivot. */
				const float lx = dx * c + dy * s, ly = -dx * s + dy * c;
				const float u = (lx + w / 2) / w * g.w, v = (ly + pivot) / h * g.h;
				if (u < -1 || v < -1 || u > g.w + 1 || v > g.h + 1) continue;
				row[px] = Over(Sample(g, u, v, 0, 0, g.w, g.h), row[px]);
			}
		}
	}

	/**
	 * Figures as an LCD shows them, seven segments each, within a rect of MSTS's screen.
	 * @param justification 1 left, 2 in the middle, 3 right
	 */
	void Figures(std::string_view text, float x, float y, float w, float h, int justification, uint32_t colour)
	{
		if (text.empty() || w <= 0 || h <= 0) return;
		/* Segments of 0-9: a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle. */
		static constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
		float ch = h, cw = ch * 0.55f, gap = cw * 0.3f;
		auto width_of = [&]() {
			float total = 0;
			for (char c : text) total += (c == ':' || c == '.' ? cw * 0.4f : cw) + gap;
			return total - gap;
		};
		if (width_of() > w) {
			const float k = w / width_of();
			ch *= k;
			cw *= k;
			gap *= k;
		}
		const float total = width_of();
		float at = justification == 1 ? x : justification == 2 ? x + (w - total) / 2 : x + w - total;
		const float top = y + (h - ch) / 2, t = std::max(ch / 8, 1.0f / this->sy);
		for (char c : text) {
			if (c == ':' || c == '.') {
				const float d = cw * 0.4f;
				if (c == ':') this->Fill(at + (d - t) / 2, top + ch * 0.3f, t, t, colour);
				this->Fill(at + (d - t) / 2, top + (c == ':' ? ch * 0.65f : ch - t), t, t, colour);
				at += d + gap;
				continue;
			}
			if (c >= '0' && c <= '9') {
				const uint8_t s = SEGMENTS[c - '0'];
				const float mid = top + (ch - t) / 2;
				if (s & 0x01) this->Fill(at, top, cw, t, colour);
				if (s & 0x02) this->Fill(at + cw - t, top, t, ch / 2, colour);
				if (s & 0x04) this->Fill(at + cw - t, top + ch / 2, t, ch / 2, colour);
				if (s & 0x08) this->Fill(at, top + ch - t, cw, t, colour);
				if (s & 0x10) this->Fill(at, top + ch / 2, t, ch / 2, colour);
				if (s & 0x20) this->Fill(at, top, t, ch / 2, colour);
				if (s & 0x40) this->Fill(at, mid, cw, t, colour);
			} else if (c == '-') {
				this->Fill(at, top + (ch - t) / 2, cw, t, colour);
			}
			at += cw + gap;
		}
	}
};

/** A figure of the desk in a control's units ("Units"): speeds from km/h, pressures from bar, voltages from kV, currents from A. */
float InUnits(const SimtrainsCab::Control &c, float value)
{
	const std::string &u = c.units;
	if (StrEqualsIgnoreCase(u, "MILES_PER_HOUR")) return value / 1.609344f;
	if (StrEqualsIgnoreCase(u, "METRES_PER_SEC")) return value / 3.6f;
	if (StrEqualsIgnoreCase(u, "PSI")) return value * 14.5038f;
	if (StrEqualsIgnoreCase(u, "KPA")) return value * 100;
	if (StrEqualsIgnoreCase(u, "INCHES_OF_MERCURY")) return value * 29.53f;
	if (StrEqualsIgnoreCase(u, "KGS_PER_SQUARE_CM")) return value * 1.0197f;
	if (StrEqualsIgnoreCase(u, "VOLTS")) return value * 1000;
	if (StrEqualsIgnoreCase(u, "KILO_AMPS")) return value / 1000;
	return value;
}

/** What a control shows of the desk (its "Type"), in its units. */
float ValueOf(const SimtrainsCab::Control &c, const SimtrainsDeskState &s)
{
	const std::string &t = c.type;
	auto is = [&t](std::string_view what) { return StrEqualsIgnoreCase(t, what); };
	if (is("SPEEDOMETER")) return InUnits(c, s.speed_kmh);
	if (is("BRAKE_CYL")) return InUnits(c, s.brake_cylinder);
	if (is("BRAKE_PIPE") || is("EQ_RES")) return InUnits(c, s.brake_pipe);
	if (is("MAIN_RES")) return InUnits(c, s.main_reservoir);
	if (is("LINE_VOLTAGE")) {
		/* A desk of the alternating current reads its tens of kilovolts. */
		if (!s.pantograph) return 0;
		const float kv = InUnits(c, 1) > 1 ? c.max / 1000 : c.max;
		return InUnits(c, kv > 10 ? 25.0f : s.line_kv);
	}
	if (is("AMMETER")) return InUnits(c, s.amps);
	if (is("THROTTLE") || is("THROTTLE_DISPLAY")) return s.throttle;
	if (is("TRAIN_BRAKE")) return s.train_brake;
	if (is("ENGINE_BRAKE")) return s.engine_brake;
	if (is("DYNAMIC_BRAKE") || is("DYNAMIC_BRAKE_DISPLAY")) return s.dynamic;
	if (is("CP_HANDLE")) return s.handle;
	if (is("DIRECTION") || is("DIRECTION_DISPLAY")) return static_cast<float>(s.direction);
	if (is("PANTOGRAPH") || is("PANTOGRAPH2") || is("PANTO_DISPLAY")) return s.pantograph ? 1.0f : 0.0f;
	if (is("FRONT_HLIGHT")) return static_cast<float>(s.lights);
	if (is("HORN") || is("BELL") || is("WHISTLE")) return s.horn ? 1.0f : 0.0f;
	if (is("RESET")) return s.alerter_pressed ? 1.0f : 0.0f;
	if (is("ALERTER_DISPLAY") || is("VIGILANCE")) return s.alerter_lamp ? 1.0f : 0.0f;
	if (is("EMERGENCY_BRAKE")) return s.emergency ? 1.0f : 0.0f;
	if (is("WIPERS")) return s.repeater_off ? 1.0f : 0.0f;
	return 0;
}

/**
 * The frame of a control drawn by its frames (a lever, a switch, a lamp): a two-state one off or on, a three-state one
 * its state, a display by the values its states show from; one with positions the position whose value the control
 * has reached -- between two positions of frames between them, the frame as far between them (a brake's range);
 * without, its range spread over its frames.
 */
int FrameOf(const SimtrainsCab::Control &c, float v)
{
	const int frames = std::max(1, c.frames);
	int f = 0;
	if (StrEqualsIgnoreCase(c.kind, "TwoState")) {
		f = v > 0 ? 1 : 0;
	} else if (StrEqualsIgnoreCase(c.kind, "TriState")) {
		f = static_cast<int>(std::lround(v));
	} else if (!c.switch_values.empty()) {
		for (size_t i = 0; i < c.switch_values.size(); i++) {
			if (v >= c.switch_values[i]) f = static_cast<int>(i);
		}
	} else if (c.values.size() >= 2 && c.values.size() == c.positions.size()) {
		size_t i = 0;
		while (i + 1 < c.values.size() && v >= c.values[i + 1]) i++;
		f = c.positions[i];
		if (i + 1 < c.values.size() && std::abs(c.positions[i + 1] - c.positions[i]) > 1 && c.values[i + 1] > c.values[i] && v > c.values[i]) {
			const float k = (v - c.values[i]) / (c.values[i + 1] - c.values[i]);
			f = c.positions[i] + static_cast<int>(std::lround(k * (c.positions[i + 1] - c.positions[i])));
		}
	} else if (c.max > c.min) {
		f = static_cast<int>(std::lround(std::clamp((v - c.min) / (c.max - c.min), 0.0f, 1.0f) * (frames - 1)));
	}
	return std::clamp(f, 0, frames - 1);
}

/** One control drawn over the desk's first picture as the train stands (SimtrainsDeskState). */
void DrawControl(DeskPainter &p, const SimtrainsCab::Control &c, const SimtrainsDeskState &s, bool night)
{
	const std::string &graphic_path = night && !c.graphic_night.empty() ? c.graphic_night : c.graphic;
	const ControlGraphic *g = ControlGraphicOf(graphic_path);
	const float v = ValueOf(c, s);
	const float range = c.max - c.min;
	const float share = range != 0 ? std::clamp((v - c.min) / range, 0.0f, 1.0f) : 0.0f;
	if (StrEqualsIgnoreCase(c.kind, "Dial")) {
		/* Turned from "ScalePos"'s first to its second, clockwise (DirIncrease 0) or the other way round. */
		if (g == nullptr || c.w <= 0 || c.h <= 0) return;
		const int way = c.dir_increase != 0 ? -1 : 1;
		float sweep = way * (c.to - c.from);
		while (sweep <= 0) sweep += 360;
		p.Needle(*g, static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.w), static_cast<float>(c.h), static_cast<float>(c.pivot), c.from + way * sweep * share);
		return;
	}
	if (StrEqualsIgnoreCase(c.kind, "Gauge")) {
		/* Along its length: up (Orientation 1) or across, from the end its "DirIncrease" says; a pointer of the size
		 * of its "Area" moving along it, or a bar of its colour growing. */
		const bool upright = c.orientation == 1;
		const float len = upright ? static_cast<float>(c.h) : static_cast<float>(c.w);
		const float aw = c.area[2] > 0 ? static_cast<float>(c.area[2]) : static_cast<float>(c.w), ah = c.area[3] > 0 ? static_cast<float>(c.area[3]) : static_cast<float>(c.h);
		const bool backwards = upright ? c.dir_increase != 0 : c.dir_increase != 0;
		if (StrEqualsIgnoreCase(c.style, "POINTER")) {
			const float along = (upright ? len - ah : len - aw) * (backwards ? 1 - share : share);
			const float x = upright ? static_cast<float>(c.x) : c.x + along, y = upright ? c.y + along : static_cast<float>(c.y);
			if (g != nullptr) {
				p.Lay(*g, 0, 0, g->w, g->h, x, y, aw, ah);
			} else {
				p.Fill(x, y, aw, ah, c.colour);
			}
		} else {
			const float filled = len * share;
			if (upright) {
				p.Fill(static_cast<float>(c.x), backwards ? c.y + len - filled : static_cast<float>(c.y), static_cast<float>(c.w), filled, c.colour);
			} else {
				p.Fill(backwards ? c.x + len - filled : static_cast<float>(c.x), static_cast<float>(c.y), filled, static_cast<float>(c.h), c.colour);
			}
		}
		return;
	}
	if (StrEqualsIgnoreCase(c.kind, "Digital")) {
		/* The figure as MSTS writes it: its places after the point, noughts before it. */
		const float figure = std::abs(v);
		std::string text = fmt::format("{:.{}f}", figure, std::clamp(c.accuracy, 0, 3));
		const size_t point = text.find('.');
		const size_t before = point == std::string::npos ? text.size() : point;
		if (c.leading_zeros > 0 && before < static_cast<size_t>(c.leading_zeros)) text.insert(0, c.leading_zeros - before, '0');
		if (v < 0 && !StrEqualsIgnoreCase(c.type, "SPEEDOMETER")) text.insert(0, "-");
		p.Figures(text, static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.w), static_cast<float>(c.h), c.justification, c.colour);
		return;
	}
	if (StrEqualsIgnoreCase(c.kind, "DigitalClock")) {
		const std::string text = c.accuracy > 0 ? fmt::format("{:02}:{:02}:{:02}", s.hour, s.minute, s.second) : fmt::format("{:02}:{:02}", s.hour, s.minute);
		p.Figures(text, static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.w), static_cast<float>(c.h), 2, c.colour);
		return;
	}
	if (g == nullptr || c.w <= 0 || c.h <= 0) return;
	int frame = 0, cols = std::max(1, c.cols), rows = std::max(1, c.rows);
	if (StrEqualsIgnoreCase(c.kind, "CabSignalDisplay")) {
		/* The repeater: MSTS's aspects in their order, eight frames four across (stop 0, approach 3, clear 7); dark
		 * with nothing to show or switched off. */
		if (s.aspect < 0 || s.repeater_off) return;
		cols = c.cols > 0 ? c.cols : 4;
		rows = c.rows > 0 ? c.rows : 2;
		frame = s.aspect == 0 ? 0 : s.aspect == 1 ? 3 : 7;
		frame = std::min(frame, cols * rows - 1);
	} else {
		frame = FrameOf(c, v);
	}
	const int fw = g->w / cols, fh = g->h / rows;
	p.Lay(*g, frame % cols * fw, frame / cols * fh, fw, fh, static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.w), static_cast<float>(c.h));
}

} // namespace

/**
 * The controls of the desk drawn into its first picture as the train stands: what they were drawn over put back from
 * the picture as read first, then each in its order.
 */
static void DrawDeskControls(SimtrainsCabDesk &desk, const SimtrainsCab &cab, const SimtrainsDeskState &state)
{
	if (desk.pictures.empty() || desk.pictures[0].w == 0) return;
	SimtrainsCabDesk::Picture &pic = desk.pictures[0];
	if (desk.base.size() != pic.px.size()) desk.base = pic.px;
	for (const std::array<int, 4> &r : desk.drawn) {
		for (int y = r[1]; y < r[3]; y++) {
			const size_t at = static_cast<size_t>(y) * pic.w;
			std::copy(&desk.base[at + r[0]], &desk.base[at + r[2]], &pic.px[at + r[0]]);
		}
	}
	desk.drawn.clear();
	DeskPainter painter{pic, desk.drawn, pic.w / 640.0f, pic.h / 480.0f};
	for (const SimtrainsCab::Control &c : cab.controls) DrawControl(painter, c, state, desk.night);
}

/**
 * Draw the desk of a cab of MSTS over the view from the cab just drawn into the window (BlitEyePicture()): over its
 * pixels, or into the hole the graphics card's picture shows through, premultiplied so that the video driver lays the
 * card's picture under it where the desk's windows are clear.
 * @param left where the picture is in the window
 * @return false when there is nothing to draw
 */
bool DrawSimtrainsCabDesk(SimtrainsCabDesk &desk, const SimtrainsCab &cab, const CabViewCamera &camera, int left, int top, int width, int height, const SimtrainsDeskState *state)
{
	if (width <= 0 || height <= 0 || cab.views.empty()) return false;
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return false;
	const bool night = IsNightNow();
	if (desk.path != cab.path || desk.night != night || desk.pictures.size() != cab.views.size()) ReadDeskPictures(desk, cab, night);
	if (desk.width != width || desk.height != height || desk.head != camera.head || desk.pitch != camera.pitch) MakeDeskLookUp(desk, cab, camera, width, height);
	/* Its controls as the train stands, drawn again when that changed. */
	if (state != nullptr && (!desk.shown_valid || !(desk.shown == *state))) {
		DrawDeskControls(desk, cab, *state);
		desk.shown = *state;
		desk.shown_valid = true;
	}

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

/* ---- The player at the desk (SimtrainsCabDriver) ---- */

/** Whether a vehicle is a train whose head is an engine of MSTS: it is driven from its desk (SimtrainsCabDriver). */
/**
 * Ready the eye for a picture of the cab: in the cab of MSTS (the camera Inside, the engine's cab read) where that cab
 * has its eye, the desk to go over the picture (CabViewCamera::msts).
 * @return the cab, nullptr when the picture is not of one
 */
const SimtrainsCab *SimtrainsCabSeat::Prepare(const Vehicle *v, CabViewCamera &camera)
{
	const SimtrainsCab *cab = v != nullptr && CabCameraChosen() == CabCamera::Inside ? SimtrainsCabOf(v) : nullptr;
	camera.msts = cab != nullptr;
	return cab;
}

/** The desk over the picture of the cab made with the eye readied (Prepare()). */
void SimtrainsCabSeat::Draw(const Vehicle *v, const CabViewCamera &camera, int left, int top, int width, int height)
{
	if (!camera.msts) return;
	if (const SimtrainsCab *cab = SimtrainsCabOf(v); cab != nullptr) {
		const SimtrainsDeskState state = SimtrainsDeskStateOf(v);
		DrawSimtrainsCabDesk(this->desk, *cab, camera, left, top, width, height, &state);
	}
}

/**
 * The arrows held drive a train of MSTS by its desk's combined lever (SimtrainsCabDriver::Drive()); with the keys of
 * MSTS chosen they do nothing.
 * @return whether it is a train of MSTS: its arrows are the desk's, not those of the other vehicles (CabArrows)
 */
bool SimtrainsCabSeat::Drive(const Vehicle *v, DirectionKeys keys, int ms)
{
	if (!IsSimtrainsTrain(v)) return false;
	if (!_settings_client.gui.cab_msts_keys) {
		this->driver.Drive(v, keys, ms);
	} else {
		this->driver.Watch(v);
	}
	return true;
}

/** A key of MSTS pressed at the desk of a train of MSTS, when they are chosen. @return whether it was one */
bool SimtrainsCabSeat::Key(const Vehicle *v, char32_t key, uint16_t keycode)
{
	return _settings_client.gui.cab_msts_keys && IsSimtrainsTrain(v) && this->driver.Key(v, key, keycode);
}

/** The arrows, or the keys of MSTS (the button "controls" of the cab). */
void SimtrainsCabSeat::ToggleControls()
{
	_settings_client.gui.cab_msts_keys = !_settings_client.gui.cab_msts_keys;
	this->driver.read = false;
}

bool IsSimtrainsTrain(const Vehicle *v)
{
	return v != nullptr && v->type == VehicleType::Train && SimtrainsEngineOf(v->First()->GetGRFID()) != nullptr;
}

/** A control of the desk to the game, as the player at it (CmdTrainCabControl()). */
static void SendControl(const Train *t, TrainCabControl control, uint8_t value)
{
	Command<Commands::TrainCabControl>::Post(t->tile, t->index, to_underlying(control), value);
}

/** The train taken over by the player, if not yet; started, if it stood stopped. */
static void TakeOver(const Train *t)
{
	if (t->cab_driven) return;
	SendControl(t, TrainCabControl::Driven, 1);
	if (t->vehstatus.Test(VehState::Stopped)) Command<Commands::StartStopVehicle>::Post(t->tile, t->index, false);
}

/**
 * What the desk keeps of its own, on this computer alone, by the train: its vigilance device with the keys of MSTS,
 * the repeater switched off (V, as the 451's readme has it), and when the horn sounded.
 *
 * The vigilance device is as MSTS has it (the player, 1. 11.: "as the vigilance device is in MSTS, so it will be with
 * us with the controls of MSTS"), its times the model's own (SimtrainsEngine::Vigilance; "an American one's differs
 * from a Czech engine's"). It watches from the first Z (the player: from the first Z, so the phone without keys never
 * meets it), then Z or any control moved starts its time again; standing, the train keeps it at nought. Its time out,
 * its lamp lights and it beeps; its warning out too, it brakes and cuts the power, till its penalty time is over and
 * Z is pressed or the train stands. Counted in the game's ticks: a paused game stops it.
 */
struct DeskOwn {
	bool watching = false;     ///< Z pressed once: the vigilance device watches
	uint64_t reset_at = 0;     ///< the tick it was last set back to nought
	uint64_t seen_at = 0;      ///< the tick it was last looked at (SimtrainsCabDriver::Watch())
	bool penalty = false;      ///< it braked
	uint64_t penalty_at = 0;   ///< the tick it braked
	uint64_t pressed_at = 0;   ///< the tick Z was last pressed: the desk's button pressed a moment
	uint64_t beeped_at = 0;    ///< the tick it last beeped warning
	bool repeater_off = false;
	std::chrono::steady_clock::time_point horn{};
};
static std::map<uint32_t, DeskOwn> _desk_own;

static DeskOwn &DeskOwnOf(const Vehicle *v)
{
	return _desk_own[v->First()->index.base()];
}

/** Seconds of the game since a tick. */
static float SecondsSince(uint64_t tick)
{
	return TimerGameTick::counter < tick ? 0.0f : static_cast<float>(TimerGameTick::counter - tick) * MILLISECONDS_PER_TICK / 1000.0f;
}

/** The ticks the button of the vigilance device shows pressed, and between the beeps of its warning. */
static constexpr uint64_t ALERTER_PRESSED_TICKS = 15;
static constexpr uint64_t ALERTER_BEEP_TICKS = 18;

/** What the vigilance device of a train does now (SimtrainsDeskState::alerter): 0 none, 1 watching, 2 warning, 3 braked. */
static int AlerterStage(const Train *t, const DeskOwn &own, const SimtrainsEngine *e)
{
	if (!_settings_client.gui.cab_msts_keys || !t->cab_driven || e == nullptr || !e->vigilance.present || !own.watching) return 0;
	if (own.penalty) return 3;
	return SecondsSince(own.reset_at) >= e->vigilance.time_limit ? 2 : 1;
}

/** The horn sounded from a cab (CabHorn()): its button on the desk pressed a moment; a control moved, for the vigilance device. */
void NoteCabHorn(const Vehicle *v)
{
	if (v == nullptr || v->type != VehicleType::Train) return;
	DeskOwn &own = DeskOwnOf(v);
	own.horn = std::chrono::steady_clock::now();
	own.reset_at = TimerGameTick::counter;
}

/** How many notches its throttle has past nought: the .eng's, else eight. */
static int NotchCount(const SimtrainsEngine *e)
{
	return e != nullptr && !e->notches.empty() ? static_cast<int>(e->notches.size()) : 8;
}

/** The throttle at a notch (1 the first), of 255. */
static uint8_t NotchPower(const SimtrainsEngine *e, int notch)
{
	if (notch <= 0) return 0;
	const int n = NotchCount(e);
	const float share = e != nullptr && !e->notches.empty() ? e->notches[std::min(notch, n) - 1] : std::min(notch, n) / static_cast<float>(n);
	return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(share * 255)), 1, 255));
}

/** The notch the throttle stands nearest to. */
static int NotchOf(const SimtrainsEngine *e, uint8_t power)
{
	if (power == 0) return 0;
	int best = 1;
	for (int n = 1; n <= NotchCount(e); n++) {
		if (std::abs(NotchPower(e, n) - power) < std::abs(NotchPower(e, best) - power)) best = n;
	}
	return best;
}

static int DynamicSteps(const SimtrainsEngine *e)
{
	return e != nullptr && e->dynamic ? SimtrainsCabDriver::DYNAMIC_STEPS : 0;
}

/** The arrows' combined lever where the train's controls stand. */
static int LeverOf(const Train *t, const SimtrainsEngine *e)
{
	const int dyn = DynamicSteps(e);
	if (t->cab_power > 0) return NotchOf(e, t->cab_power);
	if (t->cab_brake > 0) return -(dyn + std::max(1, static_cast<int>(std::lround(t->cab_brake * SimtrainsCabDriver::BRAKE_STEPS / 255.0))));
	if (t->cab_dynamic > 0 && dyn > 0) return -std::max(1, static_cast<int>(std::lround(t->cab_dynamic * dyn / 255.0)));
	return 0;
}

/** The controls the combined lever stands for, sent where they differ from the train's. */
static void ApplyLever(const Train *t, const SimtrainsEngine *e, int lever)
{
	const int dyn = DynamicSteps(e);
	uint8_t power = 0, dynamic = 0, brake = 0;
	if (lever > 0) {
		power = NotchPower(e, lever);
	} else if (lever < 0) {
		const int down = -lever;
		if (down <= dyn) {
			dynamic = static_cast<uint8_t>(down * 255 / dyn);
		} else {
			dynamic = dyn > 0 ? 255 : 0;
			brake = static_cast<uint8_t>((down - dyn) * 255 / SimtrainsCabDriver::BRAKE_STEPS);
		}
	}
	if (t->cab_power != power) SendControl(t, TrainCabControl::Power, power);
	if (t->cab_dynamic != dynamic) SendControl(t, TrainCabControl::Dynamic, dynamic);
	if (t->cab_brake != brake) SendControl(t, TrainCabControl::Brake, brake);
}

/** The vigilance device's braking let off (SimtrainsCabDriver::Watch(), Z): its emergency brake released, its time from nought. */
static void LetOffVigilance(const Train *t, const SimtrainsEngine *e, DeskOwn &own)
{
	own.penalty = false;
	own.reset_at = TimerGameTick::counter;
	if (e->vigilance.emergency_brake && t->cab_emergency) SendControl(t, TrainCabControl::Emergency, 0);
}

/** The vigilance device's braking held: the power off and the brake on, as its .eng says. */
static void HoldVigilanceBrake(const Train *t, const SimtrainsEngine *e)
{
	const SimtrainsEngine::Vigilance &vm = e->vigilance;
	if (vm.cuts_power && t->cab_power > 0) SendControl(t, TrainCabControl::Power, 0);
	if (vm.emergency_brake && !t->cab_emergency) SendControl(t, TrainCabControl::Emergency, 1);
	if (vm.full_brake && t->cab_brake < 255) SendControl(t, TrainCabControl::Brake, 255);
}

/** Not looked at for this many ticks (the desk left a while), the vigilance device's time starts from then. */
static constexpr uint64_t ALERTER_GAP_TICKS = 200;

/**
 * The vigilance device of a train driven from its desk with the keys of MSTS, looked at every so often (DeskOwn): it
 * warns when its time is out, brakes when its warning is out too, and lets off when it may.
 */
void SimtrainsCabDriver::Watch(const Vehicle *v)
{
	if (!IsSimtrainsTrain(v) || v->owner != _local_company || v->vehstatus.Test(VehState::Crashed)) return;
	const Train *t = Train::From(v->First());
	const SimtrainsEngine *e = SimtrainsEngineOf(t->GetGRFID());
	if (e == nullptr || !e->vigilance.present) return;
	AutoRestoreBackup cur_company(_current_company, _local_company);
	DeskOwn &own = DeskOwnOf(t);
	const uint64_t now = TimerGameTick::counter;
	if (now < own.seen_at) {
		/* Another game since, its ticks from nought again: nothing of the old one's device. */
		const bool off = own.repeater_off;
		own = DeskOwn{};
		own.repeater_off = off;
	}
	const bool gap = now - own.seen_at > ALERTER_GAP_TICKS;
	own.seen_at = now;
	if (!own.watching) return;
	if (!t->cab_driven) {
		/* The game drives it: nothing to watch. */
		own.penalty = false;
		own.reset_at = now;
		return;
	}
	const SimtrainsEngine::Vigilance &vm = e->vigilance;
	const bool standing = t->cur_speed == 0;
	if (own.penalty) {
		if (standing && vm.reset_on_zero_speed && SecondsSince(own.penalty_at) >= vm.penalty_limit) {
			LetOffVigilance(t, e, own);
			this->done = "zivak pustil";
		} else {
			HoldVigilanceBrake(t, e);
		}
		return;
	}
	if (gap || (standing && vm.reset_on_zero_speed)) {
		own.reset_at = now;
		return;
	}
	const float out = SecondsSince(own.reset_at);
	if (out >= vm.time_limit + vm.alarm_limit) {
		own.penalty = true;
		own.penalty_at = now;
		HoldVigilanceBrake(t, e);
		this->done = "zivak zabrzdil";
	} else if (out >= vm.time_limit && now - own.beeped_at >= ALERTER_BEEP_TICKS) {
		/* Its warning: the lamp, and a beep for now, till the sounds of the cab (the player: "the sounds at the end"). */
		own.beeped_at = now;
		SndPlayFx(SND_15_BEEP);
	}
}

/**
 * The rig: the game's time gone on so many seconds for the vigilance device of a train (its time, its braking),
 * without running the ticks.
 * @return what it does then
 */
std::string RigDeskVigilance(const Vehicle *v, int seconds)
{
	if (v == nullptr || v->type != VehicleType::Train) return "neni vlak";
	const Train *t = Train::From(v->First());
	const SimtrainsEngine *e = SimtrainsEngineOf(t->GetGRFID());
	if (e == nullptr || !e->vigilance.present) return "mašina živák nemá";
	DeskOwn &own = DeskOwnOf(t);
	const uint64_t ticks = static_cast<uint64_t>(std::max(seconds, 0)) * 1000 / MILLISECONDS_PER_TICK;
	auto back = [ticks](uint64_t &at) { at = at > ticks ? at - ticks : 0; };
	back(own.reset_at);
	if (own.penalty) back(own.penalty_at);
	static const char *const STAGES[] = {"neni", "hlida", "varuje", "zabrzdil"};
	const SimtrainsEngine::Vigilance &vm = e->vigilance;
	return fmt::format("zivak {}, od nuly {:.1f} s, brzdi {:.1f} s, videno pred {:.1f} s, cur_speed {} rizen {} (casy {:.0f}/{:.0f}/{:.0f} s, rychlobrzda {} plna {} tah {} stani {} tlacitko {})",
			STAGES[AlerterStage(t, own, e)], SecondsSince(own.reset_at), own.penalty ? SecondsSince(own.penalty_at) : 0.0f,
			SecondsSince(own.seen_at), t->cur_speed, t->cab_driven ? 1 : 0,
			vm.time_limit, vm.alarm_limit, vm.penalty_limit, vm.emergency_brake ? 1 : 0, vm.full_brake ? 1 : 0, vm.cuts_power ? 1 : 0,
			vm.reset_on_zero_speed ? 1 : 0, vm.reset_on_button ? 1 : 0);
}

/**
 * The arrows at the desk, looked at every so often: up and down move the combined lever a step, held again every
 * REPEAT_MS; Down held with the lever at its end the emergency brake, Up lets it off.
 * @param now the arrows held now
 * @param ms milliseconds since the last look
 */
void SimtrainsCabDriver::Drive(const Vehicle *v, DirectionKeys now, int ms)
{
	if (!IsSimtrainsTrain(v) || v->owner != _local_company || v->vehstatus.Test(VehState::Crashed)) return;
	AutoRestoreBackup cur_company(_current_company, _local_company);
	const Train *t = Train::From(v->First());
	const SimtrainsEngine *e = SimtrainsEngineOf(t->GetGRFID());
	if (!this->read) {
		this->lever = LeverOf(t, e);
		this->read = true;
	}
	const bool up = now.Test(DirectionKey::Up), down = now.Test(DirectionKey::Down);
	const int bottom = -(DynamicSteps(e) + BRAKE_STEPS), top = NotchCount(e);
	if (up != down) {
		const bool fresh = up ? !this->held.Test(DirectionKey::Up) : !this->held.Test(DirectionKey::Down);
		if (fresh || (this->repeat_ms -= ms) <= 0) {
			this->repeat_ms = fresh ? REPEAT_MS * 2 : REPEAT_MS;
			const int was = this->lever;
			this->lever = std::clamp(this->lever + (up ? 1 : -1), bottom, top);
			TakeOver(t);
			if (up && t->cab_emergency) SendControl(t, TrainCabControl::Emergency, 0);
			if (this->lever != was) ApplyLever(t, e, this->lever);
			this->done = fmt::format("paka {}", this->lever);
		}
		if (down && this->lever == bottom) {
			if ((this->bottom_ms += ms) >= EMERGENCY_MS && !t->cab_emergency) {
				SendControl(t, TrainCabControl::Emergency, 1);
				this->done = "rychlobrzda";
			}
		} else {
			this->bottom_ms = 0;
		}
	} else {
		this->repeat_ms = 0;
		this->bottom_ms = 0;
	}
	this->held = now;
}

/**
 * A key of MSTS at the desk (the setting gui.cab_msts_keys).
 * @return whether the key is one of the desk's
 */
bool SimtrainsCabDriver::Key(const Vehicle *v, char32_t key, uint16_t keycode)
{
	if (!IsSimtrainsTrain(v) || v->owner != _local_company || v->vehstatus.Test(VehState::Crashed)) return false;
	AutoRestoreBackup cur_company(_current_company, _local_company);
	const Train *t = Train::From(v->First());
	const SimtrainsEngine *e = SimtrainsEngineOf(t->GetGRFID());
	const uint16_t code = keycode & ~WKC_SPECIAL_KEYS;
	if (key == ' ' || code == WKC_SPACE) {
		CabHorn(v);
		return true;
	}
	auto step = [&](TrainCabControl control, uint8_t now, int by) {
		TakeOver(t);
		SendControl(t, control, static_cast<uint8_t>(std::clamp(now + by, 0, 255)));
	};
	/* The vigilance device braking: no power, and its brake not let off, till it lets off itself (Watch(), Z). */
	DeskOwn &own = DeskOwnOf(t);
	const bool braked = AlerterStage(t, own, e) == 3;
	switch (key) {
		case 'd': case 'D':
			if (braked && e->vigilance.cuts_power) break;
			TakeOver(t);
			SendControl(t, TrainCabControl::Power, NotchPower(e, std::min(NotchOf(e, t->cab_power) + 1, NotchCount(e))));
			break;
		case 'a': case 'A': TakeOver(t); SendControl(t, TrainCabControl::Power, NotchPower(e, NotchOf(e, t->cab_power) - 1)); break;
		case '\'': step(TrainCabControl::Brake, t->cab_brake, 255 / BRAKE_STEPS + 1); break;
		case ';':
			if (braked && e->vigilance.full_brake) break;
			step(TrainCabControl::Brake, t->cab_brake, -(255 / BRAKE_STEPS + 1));
			break;
		case ']': step(TrainCabControl::EngineBrake, t->cab_engine_brake, 64); break;
		case '[': step(TrainCabControl::EngineBrake, t->cab_engine_brake, -64); break;
		case '.': step(TrainCabControl::Dynamic, t->cab_dynamic, 255 / DYNAMIC_STEPS + 1); break;
		case ',': step(TrainCabControl::Dynamic, t->cab_dynamic, -(255 / DYNAMIC_STEPS + 1)); break;
		case 'w': case 'W': TakeOver(t); SendControl(t, TrainCabControl::Reverser, 1); break;
		case 's': case 'S': TakeOver(t); SendControl(t, TrainCabControl::Reverser, 0); break;
		case 'p': case 'P':
			/* The pantograph of an electric engine up or down: down, no power and the desk without voltage. */
			if (e == nullptr || e->traction != SimtrainsEngine::Traction::Electric) return false;
			TakeOver(t);
			SendControl(t, TrainCabControl::Pantograph, t->cab_pantograph_down ? 0 : 1);
			break;
		case 'z': case 'Z':
			/* The vigilance device's button: the first press sets it watching (the player, 1. 11.); its braking let off
			 * by it once its penalty time is over, where its .eng says so. */
			own.pressed_at = TimerGameTick::counter;
			if (!e->vigilance.present) break;
			if (!own.watching) {
				own.watching = true;
				own.seen_at = TimerGameTick::counter;
			}
			if (own.penalty && e->vigilance.reset_on_button && SecondsSince(own.penalty_at) >= e->vigilance.penalty_limit) LetOffVigilance(t, e, own);
			break;
		case 'v': case 'V': own.repeater_off = !own.repeater_off; break;
		default:
			if (code != WKC_BACKSPACE) return false;
			if (braked && t->cab_emergency && e->vigilance.emergency_brake) break;
			TakeOver(t);
			SendControl(t, TrainCabControl::Emergency, t->cab_emergency ? 0 : 1);
			break;
	}
	/* Any control of the desk moved, Z too: the vigilance device's time from nought (its braking stays). */
	own.reset_at = TimerGameTick::counter;
	this->read = false;
	this->done = fmt::format("klavesa {}", static_cast<char>(key != 0 ? key : '?'));
	return true;
}

/** The player leaves the desk of a train driven from its cab: the game drives it again (CabLetGo()). */
void SimtrainsCabLetGo(const Vehicle *v)
{
	if (v == nullptr || v->type != VehicleType::Train || v->owner != _local_company) return;
	const Train *t = Train::From(v->First());
	if (!t->cab_driven) return;
	AutoRestoreBackup cur_company(_current_company, _local_company);
	SendControl(t, TrainCabControl::Driven, 0);
	/* The game drives it: the vigilance device's braking is over with the player's driving. */
	if (auto own = _desk_own.find(t->index.base()); own != _desk_own.end()) own->second.penalty = false;
}

/** What the desk of a train driven from its cab stands at, for the panel of the cab view; empty when the game drives it. */
std::string SimtrainsDeskText(const Vehicle *v)
{
	if (v == nullptr || v->type != VehicleType::Train) return {};
	const Train *t = Train::From(v->First());
	if (!t->cab_driven) return {};
	const SimtrainsEngine *e = SimtrainsEngineOf(t->GetGRFID());
	std::string s;
	auto add = [&s](std::string part) {
		if (!s.empty()) s += "  ";
		s += part;
	};
	if (auto own = _desk_own.find(t->index.base()); own != _desk_own.end()) {
		const int stage = AlerterStage(t, own->second, e);
		if (stage == 2) add(GetString(STR_CAB_VIEW_DESK_ALERTER));
		if (stage == 3) add(GetString(STR_CAB_VIEW_DESK_ALERTER_BRAKED));
	}
	if (t->cab_emergency) add(GetString(STR_CAB_VIEW_DESK_EMERGENCY));
	if (t->cab_reverser == 0) add(GetString(STR_CAB_VIEW_DESK_NEUTRAL));
	if (t->cab_pantograph_down) add(GetString(STR_CAB_VIEW_DESK_PANTOGRAPH_DOWN));
	if (t->cab_power > 0) {
		add(GetString(STR_CAB_VIEW_DESK_POWER, NotchOf(e, t->cab_power), NotchCount(e)));
	} else if (t->cab_dynamic == 0 && t->cab_brake == 0) {
		add(GetString(STR_CAB_VIEW_DESK_IDLE));
	}
	if (t->cab_dynamic > 0) add(GetString(STR_CAB_VIEW_DESK_DYNAMIC, std::max(1, static_cast<int>(std::lround(t->cab_dynamic * SimtrainsCabDriver::DYNAMIC_STEPS / 255.0))), SimtrainsCabDriver::DYNAMIC_STEPS));
	if (t->cab_brake > 0) add(GetString(STR_CAB_VIEW_DESK_BRAKE, t->cab_brake * 100 / 255));
	if (t->cab_engine_brake > 0) add(GetString(STR_CAB_VIEW_DESK_ENGINE_BRAKE, t->cab_engine_brake * 100 / 255));
	return s;
}

/**
 * The aspect the repeater of a train shows (the player, 1. 11.: green, yellow, red): the next signal ahead facing it
 * along its booked way (else the first way on), and the one after it when that one is green. Red the stop, green with
 * a red after it (or the end of the line) the approach, green with a green after it, or none within reach, the clear.
 * @return -1 no signal within reach ahead (the repeater dark), 0 stop, 1 approach, 2 clear
 */
static int RepeaterAspect(const Train *t)
{
	if (t->track == Track::Depot || t->track == Track::Wormhole) return -1;
	Trackdir td = t->GetVehicleTrackdir();
	if (td == Trackdir::Invalid) return -1;
	TileIndex tile = t->tile;
	CFollowTrackRail ft(t, GetAllCompatibleRailTypes(t->railtypes));
	bool green_seen = false;
	for (int step = 0; step < 48; step++) {
		if (!ft.Follow(tile, td)) return green_seen ? 1 : -1;
		const TrackdirBits reserved = ft.new_td_bits & TrackBitsToTrackdirBits(GetReservedTrackbits(ft.new_tile));
		const TrackdirBits onward = reserved.Any() ? reserved : ft.new_td_bits;
		tile = ft.new_tile;
		td = FindFirstTrackdir(onward);
		if (td == Trackdir::Invalid) break;
		if (!IsTileType(tile, TileType::Railway) || !HasSignals(tile) || !HasSignalOnTrackdir(tile, td)) continue;
		const bool green = GetSignalStateByTrackdir(tile, td) == SignalState::Green;
		if (green_seen) return green ? 2 : 1;
		if (!green) return 0;
		green_seen = true;
	}
	return green_seen ? 2 : -1;
}

/**
 * What the desk of a train shows as it stands (SimtrainsDeskState): its speed, its controls where they are, the
 * pressures of its air without delay as its brake is (the player, 31. 10.), the wire's voltage with the pantograph up,
 * the motors' current as they pull or brake, the lights, the horn, the vigilance device and the repeater, the phone's
 * clock.
 */
SimtrainsDeskState SimtrainsDeskStateOf(const Vehicle *v)
{
	SimtrainsDeskState s;
	if (v == nullptr || v->type != VehicleType::Train) return s;
	const Train *t = Train::From(v->First());
	const SimtrainsEngine *e = SimtrainsEngineOf(t->GetGRFID());
	s.speed_kmh = static_cast<float>(t->GetDisplaySpeed());
	const int notch = NotchOf(e, t->cab_power), notches = NotchCount(e);
	s.throttle = notch == 0 ? 0.0f : e != nullptr && !e->notches.empty() ? e->notches[std::min(notch, notches) - 1] : notch / static_cast<float>(notches);
	s.dynamic = t->cab_dynamic / 255.0f;
	s.handle = t->cab_dynamic > 0 ? 1.0f + s.dynamic : notch / static_cast<float>(notches);
	const float f = t->cab_brake / 255.0f;
	if (t->cab_emergency) {
		s.train_brake = e != nullptr && e->brake_emergency >= 0 ? e->brake_emergency : 1.0f;
	} else if (t->cab_brake == 0) {
		s.train_brake = e != nullptr && e->brake_running >= 0 ? e->brake_running : 0.0f;
	} else if (e != nullptr && e->brake_service_from >= 0) {
		s.train_brake = e->brake_service_from + f * (e->brake_service_to - e->brake_service_from);
	} else {
		s.train_brake = f;
	}
	s.engine_brake = t->cab_engine_brake / 255.0f;
	s.emergency = t->cab_emergency;
	s.direction = t->cab_reverser == 0 ? 1 : 2;
	s.pantograph = !t->cab_pantograph_down;
	const bool electric = e != nullptr && e->traction == SimtrainsEngine::Traction::Electric;
	s.line_kv = electric && s.pantograph ? 3.0f : 0.0f;
	/* The motors: up as they pull, down as they brake electrically (the 451's readme), at most 400 A. */
	if (s.pantograph && !t->cab_emergency && t->cab_reverser != 0 && t->cab_power > 0) s.amps = 400.0f * t->cab_power / 255;
	if (t->cab_dynamic > 0 && s.speed_kmh > 8) s.amps = -300.0f * s.dynamic;
	constexpr float CYLINDER = 3.8f, PIPE = 5.0f, PIPE_SERVICE = 1.5f, RESERVOIR = 9.0f;
	s.brake_cylinder = t->cab_emergency ? CYLINDER : std::max(f, s.engine_brake) * CYLINDER;
	s.brake_pipe = t->cab_emergency ? 0.0f : PIPE - f * PIPE_SERVICE;
	s.main_reservoir = RESERVOIR;
	s.lights = VehicleLightsOn(t) ? 2 : 0;
	const auto now = std::chrono::steady_clock::now();
	auto own = _desk_own.find(t->index.base());
	if (own != _desk_own.end()) {
		s.horn = now - own->second.horn < std::chrono::milliseconds(1200);
		s.repeater_off = own->second.repeater_off;
		/* The vigilance device: its lamp while it warns, its button pressed a moment after Z. */
		s.alerter = AlerterStage(t, own->second, e);
		s.alerter_lamp = s.alerter == 2;
		const uint64_t pressed = own->second.pressed_at;
		s.alerter_pressed = pressed != 0 && TimerGameTick::counter >= pressed && TimerGameTick::counter - pressed < ALERTER_PRESSED_TICKS;
	}
	s.aspect = RepeaterAspect(t);
	const std::tm clock = fmt::localtime(std::time(nullptr));
	s.hour = clock.tm_hour;
	s.minute = clock.tm_min;
	s.second = clock.tm_sec;
	return s;
}
