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
#include "table/strings.h"

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

/* ---- The player at the desk (SimtrainsCabDriver) ---- */

/** Whether a vehicle is a train whose head is an engine of MSTS: it is driven from its desk (SimtrainsCabDriver). */
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
	switch (key) {
		case 'd': case 'D': TakeOver(t); SendControl(t, TrainCabControl::Power, NotchPower(e, std::min(NotchOf(e, t->cab_power) + 1, NotchCount(e)))); break;
		case 'a': case 'A': TakeOver(t); SendControl(t, TrainCabControl::Power, NotchPower(e, NotchOf(e, t->cab_power) - 1)); break;
		case '\'': step(TrainCabControl::Brake, t->cab_brake, 255 / BRAKE_STEPS + 1); break;
		case ';': step(TrainCabControl::Brake, t->cab_brake, -(255 / BRAKE_STEPS + 1)); break;
		case ']': step(TrainCabControl::EngineBrake, t->cab_engine_brake, 64); break;
		case '[': step(TrainCabControl::EngineBrake, t->cab_engine_brake, -64); break;
		case '.': step(TrainCabControl::Dynamic, t->cab_dynamic, 255 / DYNAMIC_STEPS + 1); break;
		case ',': step(TrainCabControl::Dynamic, t->cab_dynamic, -(255 / DYNAMIC_STEPS + 1)); break;
		case 'w': case 'W': TakeOver(t); SendControl(t, TrainCabControl::Reverser, 1); break;
		case 's': case 'S': TakeOver(t); SendControl(t, TrainCabControl::Reverser, 0); break;
		default:
			if (code != WKC_BACKSPACE) return false;
			TakeOver(t);
			SendControl(t, TrainCabControl::Emergency, t->cab_emergency ? 0 : 1);
			break;
	}
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
	if (t->cab_emergency) add(GetString(STR_CAB_VIEW_DESK_EMERGENCY));
	if (t->cab_reverser == 0) add(GetString(STR_CAB_VIEW_DESK_NEUTRAL));
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
