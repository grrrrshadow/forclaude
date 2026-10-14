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
#include "widgets/cab_view_widget.h"

#include "table/strings.h"

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
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_AUTO), SetStringTip(STR_CAB_VIEW_AUTO, STR_CAB_VIEW_AUTO_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_OFFROAD), SetStringTip(STR_CAB_VIEW_OFFROAD, STR_CAB_VIEW_OFFROAD_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_CV_RIGHT), SetStringTip(STR_CAB_VIEW_RIGHT, STR_CAB_VIEW_RIGHT_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

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
		const Vehicle *v = Vehicle::GetIfValid(this->window_number);
		if (_game_mode == GameMode::Normal && !_exit_game && _switch_mode == SwitchMode::None && v != nullptr && v->owner == _local_company && v->IsCabSteered()) {
			if (v->cab_free) {
				/* Driven freely: back to the orders from the next road or course; a road vehicle out in the fields
				 * stops there, not to wander off without anyone at the wheel. */
				if (!v->cab_rejoin) Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(CabDriveOp::Rejoin));
				if (v->type == VehicleType::Road && !IsTileType(v->tile, TileType::Road) && !v->vehstatus.Test(VehState::Stopped)) {
					Command<Commands::StartStopVehicle>::Post(v->tile, v->index, false);
				}
			} else {
				Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(0xFF));
			}
		}
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
		/* A ship and a road vehicle can be steered: a train follows its rails, an aircraft its route. */
		bool steerable = v->type == VehicleType::Ship || v->type == VehicleType::Road;
		this->SetWidgetDisabledState(WID_CV_LEFT, !steerable);
		this->SetWidgetDisabledState(WID_CV_RIGHT, !steerable);
		this->SetWidgetDisabledState(WID_CV_AUTO, !steerable || !v->IsCabSteered() || (v->cab_free && v->cab_rejoin));
		/* Off the road: a single road vehicle on the road, not a tram, not in its depot, a tunnel or a bay (CmdCabDrive()). */
		bool can_leave_road = false;
		if (v->type == VehicleType::Road && !v->cab_free) {
			const RoadVehicle *rv = RoadVehicle::From(v);
			can_leave_road = !rv->HasArticulatedPart() && !RoadTypeIsTram(rv->roadtype) && !rv->IsInDepot() && rv->state != RVSB_WORMHOLE && !IsBayRoadStopTile(rv->tile) && !rv->IsCarried();
		}
		this->SetWidgetDisabledState(WID_CV_OFFROAD, !can_leave_road);
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
				Command<Commands::StartStopVehicle>::Post(STR_ERROR_CAN_T_STOP_START_TRAIN + to_underlying(v->type), v->tile, v->index, false);
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
			case WID_CV_RIGHT: {
				if (v->cab_free || v->type == VehicleType::Ship) {
					/* Driven freely, or a ship: the heading a small angle left or right (cab_drive.h). */
					Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(widget == WID_CV_LEFT ? CabDriveOp::Left : CabDriveOp::Right));
					break;
				}
				if (v->type == VehicleType::Road) {
					/* A road vehicle: the turn at the next junction, like a blinker; the other way back to straight on. */
					DirDiff now = v->cab_turn != Vehicle::CAB_TURN_NONE ? static_cast<DirDiff>(v->cab_turn) : DirDiff::Same;
					DirDiff asked = widget == WID_CV_LEFT ? DirDiff::Left90 : DirDiff::Right90;
					DirDiff opposite = widget == WID_CV_LEFT ? DirDiff::Right90 : DirDiff::Left90;
					DirDiff next = now == opposite ? DirDiff::Same : asked;
					Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(to_underlying(next)));
					break;
				}
				/* A ship: an eighth of a turn from the heading so far, or from the way it goes now. */
				if (v->type != VehicleType::Ship) break;
				Direction from = v->cab_heading != Direction::Invalid ? v->cab_heading : v->direction;
				if (from == Direction::Invalid) break;
				Direction to = ChangeDir(from, widget == WID_CV_LEFT ? DirDiff::Left45 : DirDiff::Right45);
				Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(to_underlying(to)));
				break;
			}
			case WID_CV_AUTO:
				if (v->cab_free) {
					/* Back to the orders from the next road or course it comes onto. */
					Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(CabDriveOp::Rejoin));
				} else {
					Command<Commands::SetCabHeading>::Post(v->tile, v->index, static_cast<uint8_t>(0xFF));
				}
				break;
			case WID_CV_OFFROAD:
				Command<Commands::CabDrive>::Post(v->tile, v->index, to_underlying(CabDriveOp::Free));
				break;
			case WID_CV_BRAKE:
			case WID_CV_THROTTLE: {
				/* The brake lowers the speed the vehicle is held to by a step from where it is now; the throttle
				 * raises it, and past the vehicle's own top speed lets it go free again (the limit 0). */
				int max_speed = std::max(1, v->GetDisplayMaxSpeed());
				int step = std::max(5, max_speed / 10);
				int now = v->GetDisplaySpeed();
				int limit = v->cab_speed_limit;
				int base = limit != 0 ? std::min(limit, now) : now;
				int next;
				if (widget == WID_CV_BRAKE) {
					next = std::max(5, base - step);
				} else {
					base = limit != 0 ? limit : now;
					next = base + step;
					if (next >= max_speed) next = 0;
				}
				Command<Commands::SetCabSpeedLimit>::Post(v->tile, v->index, static_cast<uint16_t>(next));
				break;
			}
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
		s += fmt::format("kabina vozidla {}: {}x{} px, {:.1f} ms, {} dlazdic, {} plakatu, smer {:.0f} stupnu; ", static_cast<int32_t>(w->window_number), cw->picture.width, cw->picture.height,
				cw->camera.ms, cw->camera.tiles, cw->camera.billboards, cw->camera.heading * 180.0 / M_PI);
	}
	return s.empty() ? "zadna kabina" : s;
}
