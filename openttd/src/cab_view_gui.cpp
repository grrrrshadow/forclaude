/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cab_view_gui.cpp The window of the view from a vehicle's cab (cab_view.h).
 *
 * The world ahead of the vehicle, drawn anew every few hundredths of a second
 * while the window is open (DrawCabView() in viewport.cpp), and under it the
 * panel the player asked for from the video: the speed on a gauge and in
 * figures, the top speed, the next stop, what the vehicle is doing and how full
 * it is; buttons to its own window, to put the map on it and to stop it.
 */

#include "stdafx.h"
#include "cab_view.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "vehicle_gui.h"
#include "viewport_func.h"
#include "command_func.h"
#include "vehicle_cmd.h"
#include "openttd.h"
#include "company_func.h"
#include "cab_drive.h"
#include "roadveh.h"
#include "road.h"
#include "station_map.h"
#include "tile_map.h"
#include "order_base.h"
#include "direction_func.h"
#include "gfx_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "blitter/factory.hpp"
#include "core/backup_type.hpp"
#include "aircraft.h"
#include "airport.h"
#include "ship.h"
#include "engine_base.h"
#include "sound_func.h"
#include "newgrf_sound.h"
#include "widgets/cab_view_widget.h"

#include "table/strings.h"

#include <array>
#include <climits>
#include <optional>
#include <cmath>

#include "safeguards.h"

static constexpr NWidgetPart _nested_cab_view_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_CV_CAPTION), SetStringTip(STR_CAB_VIEW_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_CV_FULL), SetSpriteTip(SPR_LARGE_SMALL_WINDOW, STR_CAB_VIEW_FULL_TOOLTIP), SetAspect(1),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(WWT_EMPTY, Colours::Invalid, WID_CV_CANVAS), SetMinimalSize(360, 240), SetResize(1, 1), SetFill(1, 1),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey, WID_CV_PANEL), SetMinimalSize(360, 50), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_VEHICLE), SetStringTip(STR_CAB_VIEW_VEHICLE, STR_CAB_VIEW_VEHICLE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_CENTRE), SetStringTip(STR_CAB_VIEW_CENTRE, STR_CAB_VIEW_CENTRE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_BRAKE), SetStringTip(STR_CAB_VIEW_BRAKE, STR_CAB_VIEW_BRAKE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_THROTTLE), SetStringTip(STR_CAB_VIEW_THROTTLE, STR_CAB_VIEW_THROTTLE_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_STOP), SetStringTip(STR_CAB_VIEW_STOP, STR_CAB_VIEW_STOP_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_LEFT), SetStringTip(STR_CAB_VIEW_LEFT, STR_CAB_VIEW_LEFT_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_AUTO), SetStringTip(STR_CAB_VIEW_AUTO, STR_CAB_VIEW_AUTO_TOOLTIP), SetFill(2, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_RIGHT), SetStringTip(STR_CAB_VIEW_RIGHT, STR_CAB_VIEW_RIGHT_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

/**
 * Whether the player can take a vehicle's wheel from its cab now and drive it freely (CmdCabDrive()): a road vehicle on
 * the road (not a tram, not in its depot, a tunnel or a bay), a ship on the water (not in its shed or a tunnel), an
 * aircraft in the air (not taking off or landing). A train follows its rails: only its speed is the player's.
 */
static bool CabCanDriveFreely(const Vehicle *v)
{
	switch (v->type) {
		case VehicleType::Road: {
			const RoadVehicle *rv = RoadVehicle::From(v);
			return !RoadTypeIsTram(rv->roadtype) && !rv->IsInDepot() && rv->state != RVSB_WORMHOLE && !IsBayRoadStopTile(rv->tile) && !rv->IsCarried();
		}
		case VehicleType::Ship: {
			const Ship *sh = Ship::From(v);
			return !sh->IsInDepot() && sh->state != Track::Wormhole;
		}
		case VehicleType::Aircraft:
			return Aircraft::From(v)->state == FLYING;
		default:
			return false;
	}
}

/**
 * Left or right from the cab (its buttons, its arrows): a heading driven freely or a ship's a small angle; a road
 * vehicle on the road the turn at the next junction, like a blinker, the other way back to straight on.
 * @return whether it turns by a step again while held (a heading)
 */
bool CabTurn(const Vehicle *v, bool left)
{
	if (v->cab_free || v->type == VehicleType::Ship || (v->type == VehicleType::Aircraft && CabCanDriveFreely(v))) {
		/* Driven freely, a ship or an aircraft in the air: the heading a small angle left or right (cab_drive.h). */
		Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(left ? CabDriveOp::Left : CabDriveOp::Right));
		return true;
	}
	if (v->type == VehicleType::Road) {
		DirDiff now = v->cab_turn != Vehicle::CAB_TURN_NONE ? static_cast<DirDiff>(v->cab_turn) : DirDiff::Same;
		DirDiff asked = left ? DirDiff::Left90 : DirDiff::Right90;
		DirDiff opposite = left ? DirDiff::Right90 : DirDiff::Left90;
		DirDiff next = now == opposite ? DirDiff::Same : asked;
		Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(to_underlying(next)));
	}
	return false;
}

/**
 * The throttle or the brake from the cab: the brake lowers the speed the vehicle is held to by a step from where it is
 * now; the throttle raises it, and past the vehicle's own top speed lets it go free again (the limit 0).
 */
void CabSpeed(const Vehicle *v, bool faster)
{
	int max_speed = std::max(1, v->GetDisplayMaxSpeed());
	int step = std::max(5, max_speed / 10);
	int now = v->GetDisplaySpeed();
	int limit = v->cab_speed_limit;
	int base = limit != 0 ? std::min(limit, now) : now;
	int next;
	if (!faster) {
		next = std::max(5, base - step);
	} else {
		base = limit != 0 ? limit : now;
		next = base + step;
		if (next >= max_speed) next = 0;
	}
	Command<Commands::SetCabSpeedLimit>::Post(v->tile, v->index, static_cast<uint16_t>(next));
}

/** The vehicle started or stopped from the cab. */
void CabStartStop(const Vehicle *v)
{
	Command<Commands::StartStopVehicle>::Post(STR_ERROR_CAN_T_STOP_START_TRAIN + to_underlying(v->type), v->tile, v->index, false);
}

/**
 * The horn of a car (the player, 17. 10.: "can the car honk?"; the sound from him: "a horn for the car"): the sounds
 * of the sets have none, only the engines starting at a stop, so it is the game's own, baseset/decouple/zvuky/
 * klakson.wav (GetGameOwnSound()). Without the file the car has no horn.
 */
static std::optional<SoundID> CarHornSound()
{
	return GetGameOwnSound("decouple" PATHSEP "zvuky" PATHSEP "klakson.wav", Subdirectory::Baseset);
}

/**
 * The horn from the cab (I; the player, 17. 10.: "in the train the key I hoots; can the car honk?"): a train's
 * whistle or horn as it has leaving a station, a ship's as it leaves its quay, a car's horn. Heard as from inside.
 */
void CabHorn(const Vehicle *v)
{
	switch (v->type) {
		case VehicleType::Train: {
			static const SoundFx whistle[] = {SND_04_DEPARTURE_STEAM, SND_0A_DEPARTURE_TRAIN, SND_0A_DEPARTURE_TRAIN, SND_47_DEPARTURE_MONORAIL, SND_41_DEPARTURE_MAGLEV};
			SndPlayFx(whistle[to_underlying(RailVehInfo(v->engine_type)->engclass)]);
			break;
		}
		case VehicleType::Road:
			if (auto horn = CarHornSound(); horn.has_value()) SndPlayFx(*horn);
			break;
		case VehicleType::Ship:
			SndPlayFx(ShipVehInfo(v->engine_type)->sfx);
			break;
		default:
			break;
	}
}

/**
 * The lights switched from the cab (F): on if they are off now (by day too), off if they are on (at night too). Only
 * how this computer draws the vehicle (Vehicle::cab_lights, VehicleLightsOn()).
 */
void CabToggleLights(const Vehicle *v)
{
	Vehicle *front = Vehicle::Get(v->First()->index);
	front->cab_lights = VehicleLightsOn(front) ? Vehicle::CAB_LIGHTS_OFF : Vehicle::CAB_LIGHTS_ON;
	MarkWholeScreenDirty();
}

/**
 * The button and the key of the route (the player, 17. 10.: "as soon as you get into the cab you drive; instead of the
 * off road there is the button 'go your route'"): to its route -- back to its orders from where it is driven freely,
 * the turn or the heading asked for forgotten, held to no speed --, or the wheel taken again.
 */
void CabToggleRoute(const Vehicle *v, CabArrows &arrows)
{
	if (arrows.route) {
		arrows.route = false;
		arrows.free_wait = 0;
		return;
	}
	arrows.route = true;
	if (v->owner != _local_company) return;
	AutoRestoreBackup cur_company(_current_company, _local_company);
	if (v->cab_free) {
		if (!v->cab_rejoin) Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(CabDriveOp::Rejoin));
	} else if (v->cab_heading != Direction::Invalid || v->cab_turn != Vehicle::CAB_TURN_NONE) {
		Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(0xFF));
	}
	if (v->cab_speed_limit != 0) Command<Commands::SetCabSpeedLimit>::Post(v->tile, v->index, static_cast<uint16_t>(0));
}

/**
 * Whoever steers from the cab lets go when leaving it (the cab view closed, the walker getting out): a ship or a road
 * vehicle goes back to its orders; driven freely, back from the next road or course, and a road vehicle out in the
 * fields stops there, not to wander off without anyone at the wheel.
 */
void CabLetGo(const Vehicle *v)
{
	if (_game_mode != GameMode::Normal || _exit_game || _switch_mode != SwitchMode::None || v == nullptr || v->owner != _local_company || !v->IsCabSteered()) return;
	if (v->cab_free) {
		if (!v->cab_rejoin) Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(CabDriveOp::Rejoin));
		if (v->type == VehicleType::Road && !IsTileType(v->tile, TileType::Road) && !v->vehstatus.Test(VehState::Stopped)) {
			Command<Commands::StartStopVehicle>::Post(v->tile, v->index, false);
		}
	} else {
		Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(0xFF));
	}
}

/** The arrows held: what is pressed now does it, what is held does it again in its time. Only the owner drives. */
void CabArrows::Drive(const Vehicle *v, DirectionKeys now, int ms)
{
	if (v == nullptr || v->owner != _local_company || v->vehstatus.Test(VehState::Crashed) || this->route) now = {};
	/* As the player's click: the local company's command (the windows' timers run where nobody is). */
	AutoRestoreBackup cur_company(_current_company, _local_company);
	/* At the wheel: as soon as the vehicle can be driven freely, it is (out of its depot, an aircraft in the air);
	 * asked once in half a second while the command is on its way. */
	if (v != nullptr && v->owner == _local_company && !this->route && !v->vehstatus.Test(VehState::Crashed) && (!v->cab_free || v->cab_rejoin)) {
		if ((this->free_wait -= ms) <= 0 && (v->cab_free || CabCanDriveFreely(v))) {
			this->free_wait = 500;
			Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(CabDriveOp::Free));
		}
	}
	const bool flying = v != nullptr && v->type == VehicleType::Aircraft;
	auto arrow = [&](DirectionKey key) {
		switch (key) {
			case DirectionKey::Left:
			case DirectionKey::Right:
				/* A train follows its rails. */
				if (v->type == VehicleType::Train) return false;
				return CabTurn(v, key == DirectionKey::Left);
			case DirectionKey::Up:
			case DirectionKey::Down:
				/* An aircraft: its height instead of its speed (the player, 17. 10.). */
				if (flying) {
					if (!v->cab_free) return false;
					Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(key == DirectionKey::Up ? CabDriveOp::Up : CabDriveOp::Down));
					return true;
				}
				if (key == DirectionKey::Up && v->vehstatus.Test(VehState::Stopped)) {
					CabStartStop(v);
					return false;
				}
				CabSpeed(v, key == DirectionKey::Up);
				return true;
		}
		return false;
	};
	for (DirectionKey key : {DirectionKey::Left, DirectionKey::Up, DirectionKey::Right, DirectionKey::Down}) {
		int &left = this->repeat_in[to_underlying(key)];
		if (!now.Test(key)) continue;
		/* Turning and an aircraft's height: about seven steps a second (a quarter turn held a second and a quarter);
		 * the speed three. */
		const int again = key == DirectionKey::Left || key == DirectionKey::Right || flying ? 150 : 300;
		if (!this->held.Test(key)) {
			left = arrow(key) ? again + 150 : INT_MAX;
		} else if (left != INT_MAX && (left -= ms) <= 0) {
			left = arrow(key) ? again : INT_MAX;
		}
	}
	this->held = now;
}

/** The window of the view from a vehicle's cab; the window number is the vehicle. */
struct CabViewWindow : Window {
	mutable CabViewCamera camera{};   ///< the eye between frames
	bool full = false;                 ///< over the whole screen
	Rect before_full{};                ///< where it was before that
	mutable CabViewPicture picture{}; ///< the last picture

	/* Drawn anew while the vehicle moves: about twenty-five times a second. */
	IntervalTimer<TimerWindow> redraw{std::chrono::milliseconds(40), [this](auto) {
		this->SetWidgetDirty(WID_CV_CANVAS);
		this->SetWidgetDirty(WID_CV_PANEL);
		this->SetWidgetDirty(WID_CV_STOP);
	}};

	/* The arrows drive (CabArrows), looked at this often. */
	CabArrows arrows{};
	static constexpr int DRIVE_MS = 30;
	IntervalTimer<TimerWindow> drive{std::chrono::milliseconds(DRIVE_MS), [this](auto) {
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		if (v != nullptr) this->arrows.Drive(v, ArrowsWindow() == this ? _dirkeys : DirectionKeys{}, DRIVE_MS);
	}};

	CabViewWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->FinishInitNested(number);
		const Vehicle *v = Vehicle::GetIfValid(number);
		if (v != nullptr) this->owner = v->owner;
	}

	void Close(int data = 0) override
	{
		/* Whoever steers from the cab lets go when leaving it: a ship or a road vehicle goes back to its orders. */
		CabLetGo(Vehicle::GetIfValid(this->window_number));
		this->Window::Close(data);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		switch (widget) {
			case WID_CV_CAPTION:
				return GetString(STR_CAB_VIEW_CAPTION, this->window_number);
			case WID_CV_STOP:
				return GetString(v != nullptr && v->vehstatus.Test(VehState::Stopped) ? STR_CAB_VIEW_GO : STR_CAB_VIEW_STOP);
			case WID_CV_AUTO:
				/* Its route, or the wheel again. */
				return GetString(this->arrows.route ? STR_CAB_VIEW_DRIVE : STR_CAB_VIEW_AUTO);
			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void OnPaint() override
	{
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		if (v == nullptr) {
			this->Close();
			return;
		}
		/* A road vehicle, a ship and an aircraft can be steered: a train follows its rails. Only the company's. */
		const bool ours = v->owner == _local_company;
		const bool steerable = ours && v->type != VehicleType::Train;
		this->SetWidgetDisabledState(WID_CV_LEFT, !steerable);
		this->SetWidgetDisabledState(WID_CV_RIGHT, !steerable);
		this->SetWidgetDisabledState(WID_CV_AUTO, !ours);
		this->DrawWidgets();
	}

	/** The picture onto the screen, within the clipping of the moment. */
	static void BlitCabPicture(const CabViewPicture &pic, int px, int py)
	{
		const DrawPixelInfo *dpi = _cur_dpi;
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		int x0 = std::max(px, dpi->left), x1 = std::min(px + pic.width, dpi->left + dpi->width);
		int y0 = std::max(py, dpi->top), y1 = std::min(py + pic.height, dpi->top + dpi->height);
		for (int y = y0; y < y1; y++) {
			const uint32_t *src = &pic.pixels[static_cast<size_t>(y - py) * pic.width];
			uint32_t *dst = static_cast<uint32_t *>(blitter->MoveTo(dpi->dst_ptr, x0 - dpi->left, y - dpi->top));
			for (int x = x0; x < x1; x++, dst++) {
				Colour sc(src[x - px]);
				*dst = Colour(sc.r, sc.g, sc.b).data;
			}
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		if (v == nullptr) return;
		switch (widget) {
			case WID_CV_CANVAS: {
				int w = r.Width(), h = r.Height();
				if (w <= 0 || h <= 0) return;
				if (!DrawCabView(v, w, h, this->camera, this->picture)) {
					GfxFillRect(r, GetColourGradient(Colours::Grey, Shade::Darkest));
					return;
				}
				BlitCabPicture(this->picture, r.left, r.top);
				break;
			}
			case WID_CV_PANEL:
				this->DrawPanel(r, v);
				break;
			default:
				break;
		}
	}

	/** How full the vehicle is, per cent of what it can carry, over all its parts. */
	static int LoadPercent(const Vehicle *v)
	{
		uint cap = 0, has = 0;
		for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
			cap += u->cargo_cap;
			has += u->cargo.StoredCount();
		}
		if (cap == 0) return 0;
		return static_cast<int>(std::min<uint>(100, has * 100 / cap));
	}

	/** The panel under the view: the gauge, the figures, the next stop, the state and the load. */
	void DrawPanel(const Rect &r, const Vehicle *v) const
	{
		GfxFillRect(r, GetColourGradient(Colours::Grey, Shade::Darkest));
		const PixelColour light = GetColourGradient(Colours::Grey, Shade::Lightest);
		const PixelColour dim = GetColourGradient(Colours::Grey, Shade::Normal);
		const PixelColour red = GetColourGradient(Colours::Red, Shade::Light);
		const PixelColour blue = GetColourGradient(Colours::LightBlue, Shade::Light);

		/* The gauge: a three-quarter arc, the needle from the bottom left round to the bottom right. */
		int rad = std::max(8, std::min(r.Height() / 2 - 3, 24));
		int gx = r.left + rad + 6, gy = r.top + r.Height() / 2;
		uint speed = v->GetDisplaySpeed(), max_speed = std::max<uint>(1, v->GetDisplayMaxSpeed());
		double frac = std::min(1.0, speed / static_cast<double>(max_speed));
		const double start = 225.0 * M_PI / 180.0, sweep = 270.0 * M_PI / 180.0;
		const int segments = 24;
		for (int i = 0; i < segments; i++) {
			double a0 = start - sweep * i / segments, a1 = start - sweep * (i + 1) / segments;
			GfxDrawLine(gx + static_cast<int>(std::lround(rad * std::cos(a0))), gy - static_cast<int>(std::lround(rad * std::sin(a0))),
					gx + static_cast<int>(std::lround(rad * std::cos(a1))), gy - static_cast<int>(std::lround(rad * std::sin(a1))), i < segments * 3 / 4 ? light : red);
		}
		double na = start - sweep * frac;
		GfxDrawLine(gx, gy, gx + static_cast<int>(std::lround((rad - 2) * std::cos(na))), gy - static_cast<int>(std::lround((rad - 2) * std::sin(na))), red);
		GfxFillRect(gx - 1, gy - 1, gx + 1, gy + 1, light);

		/* The figures beside it. */
		int tx = gx + rad + 8;
		int ty = r.top + 4;
		int text_right = tx + std::max(60, r.Width() / 3);
		DrawString(tx, text_right, ty, GetString(STR_CAB_VIEW_SPEED, PackVelocity(speed, v->type)), TextColour::White, AlignmentH::Start, false, FontSize::Large);
		DrawString(tx, text_right, ty + GetCharacterHeight(FontSize::Large) + 1, GetString(STR_CAB_VIEW_MAX_SPEED, PackVelocity(max_speed, v->type)), TextColour::Silver, AlignmentH::Start, false, FontSize::Small);

		/* The next stop and the state. */
		int lw = std::max(40, r.Width() / 6);
		int lx = r.right - 4 - lw;
		int sx = text_right + 8, sright = lx - 8;
		std::string stop;
		const Order &o = v->current_order;
		if (o.IsType(OT_GOTO_STATION) || o.IsType(OT_LOADING)) {
			stop = GetString(STR_CAB_VIEW_NEXT_STOP, o.GetDestination().ToStationID());
		} else if (o.IsType(OT_GOTO_DEPOT)) {
			stop = GetString(STR_CAB_VIEW_TO_DEPOT);
		} else if (o.IsType(OT_GOTO_WAYPOINT)) {
			stop = GetString(STR_CAB_VIEW_NEXT_STOP, o.GetDestination().ToStationID());
		} else {
			stop = GetString(STR_CAB_VIEW_NO_DESTINATION);
		}
		DrawString(sx, sright, ty, stop, TextColour::White);
		StringID state = STR_CAB_VIEW_STATE_RUNNING;
		if (v->vehstatus.Test(VehState::Stopped)) {
			state = STR_CAB_VIEW_STATE_STOPPED;
		} else if (v->breakdown_ctr != 0) {
			state = STR_CAB_VIEW_STATE_BROKEN;
		} else if (o.IsType(OT_LOADING)) {
			state = STR_CAB_VIEW_STATE_LOADING;
		}
		int state_y = ty + GetCharacterHeight(FontSize::Normal) + 2;
		std::string state_text = GetString(state);
		DrawString(sx, sright, state_y, state_text, TextColour::FromString);
		int note_x = sx + GetStringBoundingBox(state_text).width + 8;
		if (v->cab_speed_limit != 0) {
			/* Held to a speed from here: said beside the state. */
			std::string limit = GetString(STR_CAB_VIEW_LIMIT, PackVelocity(v->cab_speed_limit, v->type));
			DrawString(note_x, sright, state_y, limit, TextColour::FromString);
			note_x += GetStringBoundingBox(limit).width + 8;
		}
		if (v->cab_free) {
			/* Driven freely: so, or on the way back to the orders. */
			DrawString(note_x, sright, state_y, GetString(v->cab_rejoin ? STR_CAB_VIEW_FREE_BACK : STR_CAB_VIEW_FREE), TextColour::FromString);
		} else if (v->cab_turn != Vehicle::CAB_TURN_NONE) {
			/* A road vehicle: the turn it takes at the next junction. */
			StringID turn = STR_CAB_VIEW_STEERED_STRAIGHT;
			if (v->cab_turn == to_underlying(DirDiff::Right90)) turn = STR_CAB_VIEW_STEERED_RIGHT;
			if (v->cab_turn == to_underlying(DirDiff::Left90)) turn = STR_CAB_VIEW_STEERED_LEFT;
			DrawString(note_x, sright, state_y, GetString(turn), TextColour::FromString);
		} else if (v->cab_heading != Direction::Invalid) {
			DrawString(note_x, sright, state_y, GetString(STR_CAB_VIEW_STEERED), TextColour::FromString);
		}

		/* The load, as a figure and a bar. */
		int percent = LoadPercent(v);
		DrawString(lx, r.right - 4, ty, GetString(STR_CAB_VIEW_LOAD, percent), TextColour::White, AlignmentH::End);
		int by = ty + GetCharacterHeight(FontSize::Normal) + 4;
		GfxFillRect(lx, by, r.right - 4, by + 4, dim);
		if (percent > 0) GfxFillRect(lx, by, lx + (lw * percent) / 100, by + 4, blue);
	}

	EventState OnKeyPress(char32_t key, uint16_t keycode) override
	{
		/* The arrows are the driver's, read as held (Drive()). */
		switch (keycode & ~WKC_SPECIAL_KEYS) {
			case WKC_UP: case WKC_DOWN: case WKC_LEFT: case WKC_RIGHT:
				return EventState::Handled;
			default:
				break;
		}
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		if (v == nullptr) return EventState::NotHandled;
		/* I: the horn; F: the lights; T: its route, or the wheel again. */
		if (key == 'i' || key == 'I') {
			CabHorn(v);
			return EventState::Handled;
		}
		if (key == 'f' || key == 'F') {
			CabToggleLights(v);
			return EventState::Handled;
		}
		if (key == 't' || key == 'T') {
			CabToggleRoute(v, this->arrows);
			this->SetDirty();
			return EventState::Handled;
		}
		return EventState::NotHandled;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		if (v == nullptr) return;
		switch (widget) {
			case WID_CV_VEHICLE:
				ShowVehicleViewWindow(v);
				break;
			case WID_CV_CENTRE:
				ScrollMainWindowTo(v->x_pos, v->y_pos, v->z_pos);
				break;
			case WID_CV_STOP:
				CabStartStop(v);
				break;
			case WID_CV_FULL: {
				/* Over the whole screen, and back where it was (the player: a button beside the pin, as big as the screen). */
				if (!this->full) {
					this->before_full = {this->left, this->top, this->left + this->width, this->top + this->height};
					this->full = true;
					this->left = 0;
					this->top = 0;
					ResizeWindow(this, _screen.width - this->width, _screen.height - this->height, false);
				} else {
					this->full = false;
					this->left = this->before_full.left;
					this->top = this->before_full.top;
					ResizeWindow(this, this->before_full.Width() - this->width, this->before_full.Height() - this->height, false);
				}
				MarkWholeScreenDirty();
				break;
			}
			case WID_CV_LEFT:
			case WID_CV_RIGHT:
				CabTurn(v, widget == WID_CV_LEFT);
				break;
			case WID_CV_AUTO:
				CabToggleRoute(v, this->arrows);
				this->SetDirty();
				break;
			case WID_CV_BRAKE:
			case WID_CV_THROTTLE:
				CabSpeed(v, widget == WID_CV_THROTTLE);
				break;
			default:
				break;
		}
	}
};

static WindowDesc _cab_view_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::CabView, WindowClass::None,
	{},
	_nested_cab_view_widgets
);

/**
 * The window the arrows are for (the player, 17. 10.: "while the cab view or the first person view is open, the arrows
 * always go to that window, not to the map"): the view from a cab or from the first person, the one with the focus,
 * else the front one open. None while an edit box takes the keys.
 */
Window *ArrowsWindow()
{
	if (_game_mode != GameMode::Normal || EditBoxInGlobalFocus()) return nullptr;
	auto takes = [](const Window *w) { return w != nullptr && (w->window_class == WindowClass::CabView || w->window_class == WindowClass::FirstPerson); };
	if (takes(_focused_window)) return _focused_window;
	for (Window *w : Window::IterateFromFront()) {
		if (takes(w)) return w;
	}
	return nullptr;
}

/** Open the view from the cab of a vehicle, or bring it to the front. */
void ShowCabViewWindow(const Vehicle *v)
{
	if (v == nullptr) return;
	AllocateWindowDescFront<CabViewWindow>(_cab_view_desc, v->index);
}

/** The rig: every cab view open, with how long its last picture took and what was in it. */
std::string DescribeCabViews()
{
	std::string s;
	for (const Window *w : Window::Iterate()) {
		if (w->window_class != WindowClass::CabView) continue;
		const CabViewWindow *cw = static_cast<const CabViewWindow *>(w);
		s += fmt::format("kabina vozidla {}: {}x{} px, {:.1f} ms, {} dlazdic, {} plakatu, smer {:.0f} stupnu", static_cast<int32_t>(w->window_number), cw->picture.width, cw->picture.height,
				cw->camera.ms, cw->camera.tiles, cw->camera.billboards, cw->camera.heading * 180.0 / M_PI);
		const Vehicle *v = Vehicle::GetIfValid(w->window_number);
		if (v != nullptr) {
			s += fmt::format(", zatoceni {} volne {} yaw {} limit {} rychlost {} stoji {}", v->cab_turn, v->cab_free, v->cab_yaw, v->cab_speed_limit, v->GetDisplaySpeed(), v->vehstatus.Test(VehState::Stopped) ? "ano" : "ne");
			if (v->type == VehicleType::Aircraft) s += fmt::format(" vyska {} cil {} stav {} na {},{}", v->z_pos, v->cab_altitude, Aircraft::From(v)->state, v->x_pos, v->y_pos);
		}
		s += "; ";
	}
	return s.empty() ? "zadna kabina" : s;
}

/** The rig: the car's horn, as read from its file. */
std::string DescribeCarHorn()
{
	auto horn = CarHornSound();
	const SoundEntry *e = horn.has_value() ? GetSound(*horn) : nullptr;
	if (e == nullptr || e->data == nullptr) return "klakson neni";
	return fmt::format("klakson zvuk {}: {} Hz, {} bitu, {} vzorku", *horn, e->rate, e->bits_per_sample, e->data->size() / (e->bits_per_sample / 8));
}
