/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file three_d_preview_gui.cpp The 3D previews that turn (three_d.h,
 * gui.three_d): a window beside the purchase window with the vehicle chosen
 * there, four times the size, turning through its eight directions, in the
 * company's colours; and a window beside the window of one of the game's own
 * industries with the building from its eight renders (ThreeDModel).
 *
 * The preview opens with its window and closes with it, keeps beside it, and
 * shows the direction the map view is turned to; the arrows turn it on by one
 * of the eight directions, the button in the middle lets it turn by itself.
 */

#include "stdafx.h"
#include "three_d.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "engine_base.h"
#include "engine_func.h"
#include "engine_gui.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "industry.h"
#include "spritecache.h"
#include "zoom_func.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "timer/timer.h"
#include "timer/timer_window.h"

#include "widgets/three_d_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"

/** The zoom the previews are drawn at: four times the normal size. */
static constexpr ZoomLevel PREVIEW_ZOOM = ZoomLevel::In4x;
/** How often the preview turns by itself. */
static constexpr int PREVIEW_TURN_MS = 700;
/** How often the preview looks at its window: where it is, what it shows. */
static constexpr int PREVIEW_FOLLOW_MS = 100;

static constexpr NWidgetPart _nested_three_d_vehicle_preview_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_TDP_CAPTION),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey, WID_TDP_PICTURE), SetMinimalSize(300, 170), SetResize(1, 1), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_TDP_LEFT), SetMinimalSize(22, 12), SetSpriteTip(SPR_ARROW_LEFT, STR_THREE_D_PREVIEW_LEFT_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::Grey, WID_TDP_TURN), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_THREE_D_PREVIEW_TURN, STR_THREE_D_PREVIEW_TURN_TOOLTIP),
		NWidget(WWT_PUSHIMGBTN, Colours::Grey, WID_TDP_RIGHT), SetMinimalSize(22, 12), SetSpriteTip(SPR_ARROW_RIGHT, STR_THREE_D_PREVIEW_RIGHT_TOOLTIP),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

static constexpr NWidgetPart _nested_three_d_industry_preview_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Cream),
		NWidget(WWT_CAPTION, Colours::Cream, WID_TDP_CAPTION),
		NWidget(WWT_SHADEBOX, Colours::Cream),
		NWidget(WWT_STICKYBOX, Colours::Cream),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Cream, WID_TDP_PICTURE), SetMinimalSize(300, 170), SetResize(1, 1), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHIMGBTN, Colours::Cream, WID_TDP_LEFT), SetMinimalSize(22, 12), SetSpriteTip(SPR_ARROW_LEFT, STR_THREE_D_PREVIEW_LEFT_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::Cream, WID_TDP_TURN), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_THREE_D_PREVIEW_TURN, STR_THREE_D_PREVIEW_TURN_TOOLTIP),
		NWidget(WWT_PUSHIMGBTN, Colours::Cream, WID_TDP_RIGHT), SetMinimalSize(22, 12), SetSpriteTip(SPR_ARROW_RIGHT, STR_THREE_D_PREVIEW_RIGHT_TOOLTIP),
		NWidget(WWT_RESIZEBOX, Colours::Cream),
	EndContainer(),
};

static WindowDesc _three_d_vehicle_preview_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::ThreeDVehiclePreview, WindowClass::None,
	{},
	_nested_three_d_vehicle_preview_widgets
);

static WindowDesc _three_d_industry_preview_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::ThreeDIndustryPreview, WindowClass::None,
	{},
	_nested_three_d_industry_preview_widgets
);

/** The bounds of a picture at the preview's zoom, pixels about the point it is drawn at. */
struct PreviewBounds {
	int left = 0, top = 0, right = 0, bottom = 0;
	bool any = false;

	void Add(SpriteID sprite)
	{
		const Sprite *spr = GetSprite(sprite & SPRITE_MASK, SpriteType::Normal);
		int l = UnScaleByZoom(spr->x_offs, PREVIEW_ZOOM), t = UnScaleByZoom(spr->y_offs, PREVIEW_ZOOM);
		int r = l + UnScaleByZoom(spr->width, PREVIEW_ZOOM), b = t + UnScaleByZoom(spr->height, PREVIEW_ZOOM);
		if (!this->any) {
			this->left = l; this->top = t; this->right = r; this->bottom = b;
			this->any = true;
		} else {
			this->left = std::min(this->left, l); this->top = std::min(this->top, t);
			this->right = std::max(this->right, r); this->bottom = std::max(this->bottom, b);
		}
	}
};

/**
 * The preview that turns: the vehicle chosen in the purchase window, or one
 * of the game's own industries, from eight directions.
 */
struct ThreeDPreviewWindow : Window {
	bool vehicle;            ///< a vehicle beside the purchase window, else an industry beside its window
	WindowNumber parent_number; ///< the window it belongs to: the purchase window's number, or the industry
	EngineID engine = EngineID::Invalid(); ///< the vehicle shown
	int offset = 0;          ///< how many of the eight directions the player turned it on from the map view's
	bool turning = false;    ///< whether it turns by itself
	int turn_elapsed = 0;    ///< milliseconds since it last turned by itself

	ThreeDPreviewWindow(WindowDesc &desc, bool vehicle, WindowNumber parent_number) : Window(desc), vehicle(vehicle), parent_number(parent_number)
	{
		this->CreateNestedTree();
		Point pt = this->PlaceBesideParent();
		this->left = pt.x;
		this->top = pt.y;
		this->FinishInitNested(vehicle ? static_cast<WindowNumber>(0) : parent_number);
		/* A vehicle turns by itself from the start (the player: it turns through the eight directions); a
		 * building shows the direction the map view is turned to, until the player turns it. */
		this->turning = vehicle;
		this->SetWidgetLoweredState(WID_TDP_TURN, this->turning);
		this->Follow();
	}

	/** The window this preview belongs to, or nullptr when it has gone. */
	Window *Parent() const
	{
		return FindWindowById(this->vehicle ? WindowClass::BuildVehicle : WindowClass::IndustryView, this->parent_number);
	}

	/** Where to be: to the right of the window it belongs to, or to the left of it when there is no room. */
	Point PlaceBesideParent() const
	{
		const Window *p = this->Parent();
		if (p == nullptr) return {this->left, this->top};
		int x = p->left + p->width;
		if (x + this->width > _screen.width) x = std::max(0, p->left - this->width);
		return {x, Clamp(p->top, 0, std::max(0, _screen.height - this->height))};
	}

	/** Keep beside the window, show what it shows, go with it when it goes. */
	void Follow()
	{
		Window *p = this->Parent();
		if (p == nullptr || !ThreeDEnabled()) {
			this->Close();
			return;
		}
		Point pt = this->PlaceBesideParent();
		if (pt.x != this->left || pt.y != this->top) {
			this->SetDirty();
			this->left = pt.x;
			this->top = pt.y;
			this->SetDirty();
		}
		if (this->vehicle) {
			EngineID e = GetBuildVehicleSelectedEngine(this->parent_number);
			if (e != this->engine) {
				this->engine = e;
				this->SetDirty();
			}
		}
	}

	/** The direction shown: the map view's, turned on by the player and by itself. */
	Direction Shown() const
	{
		int base = this->vehicle ? to_underlying(Direction::W) : 0;
		return static_cast<Direction>((base - TurnedDirectionStep() + this->offset) & 7);
	}

	const IntervalTimer<TimerWindow> follow_interval = {std::chrono::milliseconds(PREVIEW_FOLLOW_MS), [this](auto) {
		this->Follow();
		if (this->turning) {
			this->turn_elapsed += PREVIEW_FOLLOW_MS;
			if (this->turn_elapsed >= PREVIEW_TURN_MS) {
				this->turn_elapsed = 0;
				this->offset = (this->offset + 1) & 7;
				this->SetWidgetDirty(WID_TDP_PICTURE);
			}
		}
	}};

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_TDP_CAPTION) {
			if (this->vehicle) return GetString(STR_THREE_D_PREVIEW_VEHICLE_CAPTION, this->engine);
			return GetString(STR_THREE_D_PREVIEW_INDUSTRY_CAPTION, static_cast<IndustryID>(this->parent_number));
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_TDP_LEFT:
				this->offset = (this->offset - 1) & 7;
				this->SetWidgetDirty(WID_TDP_PICTURE);
				break;
			case WID_TDP_RIGHT:
				this->offset = (this->offset + 1) & 7;
				this->SetWidgetDirty(WID_TDP_PICTURE);
				break;
			case WID_TDP_TURN:
				this->turning = !this->turning;
				this->turn_elapsed = 0;
				this->SetWidgetLoweredState(WID_TDP_TURN, this->turning);
				this->SetWidgetDirty(WID_TDP_TURN);
				break;
			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TDP_PICTURE) return;
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		DrawPixelInfo tmp_dpi;
		if (!FillDrawPixelInfo(&tmp_dpi, ir.left, ir.top, ir.Width(), ir.Height())) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);
		int cx = ir.Width() / 2, cy = ir.Height() / 2;
		if (this->vehicle) {
			this->DrawVehicle(cx, cy);
		} else {
			this->DrawIndustry(cx, cy);
		}
	}

	/** The vehicle: its icon for the direction, in the company's colours, in the middle. */
	void DrawVehicle(int cx, int cy) const
	{
		if (this->engine == EngineID::Invalid() || !Engine::IsValidID(this->engine)) return;
		const Engine *e = Engine::Get(this->engine);
		Direction dir = this->Shown();
		PaletteID pal = GetEnginePalette(this->engine, _local_company);
		VehicleSpriteSeq front, rear;
		int pitch = 0;
		bool two = false;
		switch (e->type) {
			case VehicleType::Train:
				GetTrainIcon(this->engine, false, dir, pitch, EngineImageType::Purchase, &front);
				if (e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Multihead) {
					int pitch_rear = 0;
					GetTrainIcon(this->engine, true, ReverseDir(dir), pitch_rear, EngineImageType::Purchase, &rear);
					two = rear.IsValid();
				}
				break;
			case VehicleType::Road: GetRoadVehIcon(this->engine, dir, EngineImageType::Purchase, &front); break;
			case VehicleType::Ship: GetShipIcon(this->engine, dir, EngineImageType::Purchase, &front); break;
			case VehicleType::Aircraft: GetAircraftIcon(this->engine, dir, EngineImageType::Purchase, &front); break;
			default: return;
		}
		if (!front.IsValid()) return;

		/* The two heads of a multiheaded engine stand one behind the other along the direction, half a vehicle from the middle each. */
		int fx = 0, fy = 0, rx = 0, ry = 0;
		if (two) {
			static const int dx[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, dy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
			double len = (to_underlying(dir) & 1) ? 1.0 : std::sqrt(2.0);
			double half = TILE_SIZE / 4.0 / len;
			double wx = dx[to_underlying(dir)] * half, wy = dy[to_underlying(dir)] * half;
			fx = static_cast<int>(std::lround((wy - wx) * 2 * ZOOM_BASE / (1 << to_underlying(PREVIEW_ZOOM))));
			fy = static_cast<int>(std::lround((wy + wx) * ZOOM_BASE / (1 << to_underlying(PREVIEW_ZOOM))));
			rx = -fx;
			ry = -fy;
		}
		PreviewBounds bounds;
		for (uint i = 0; i < front.count; i++) bounds.Add(front.seq[i].sprite);
		int x = cx - (bounds.left + bounds.right) / 2, y = cy - (bounds.top + bounds.bottom) / 2 + pitch;
		auto draw = [&](const VehicleSpriteSeq &seq, int sx, int sy) {
			for (uint i = 0; i < seq.count; i++) {
				PaletteID p = (pal == PALETTE_CRASH || seq.seq[i].pal == PAL_NONE) ? pal : seq.seq[i].pal;
				DrawSprite(seq.seq[i].sprite, p, sx, sy, nullptr, PREVIEW_ZOOM);
			}
		};
		/* The one further back is drawn first. */
		if (two && ry < fy) {
			draw(rear, x + rx, y + ry);
			draw(front, x + fx, y + fy);
		} else {
			draw(front, x + fx, y + fy);
			if (two) draw(rear, x + rx, y + ry);
		}
	}

	/** The industry: the colour render of the direction, the whole building, in the middle. */
	void DrawIndustry(int cx, int cy) const
	{
		if (!Industry::IsValidID(static_cast<IndustryID>(this->parent_number))) return;
		const Industry *ind = Industry::Get(static_cast<IndustryID>(this->parent_number));
		const ThreeDModel *model = GetThreeDIndustryModel(ind->type, HasBit(ind->random, 0));
		if (model == nullptr) return;
		Direction dir = this->Shown();
		SpriteID sprite = SPR_DECOUPLE_3D_BASE + model->first + to_underlying(dir) * 3;
		if (!SpriteExists(sprite)) return;
		PreviewBounds bounds;
		bounds.Add(sprite);
		int x = cx - (bounds.left + bounds.right) / 2, y = cy - (bounds.top + bounds.bottom) / 2;
		DrawSprite(sprite, PAL_NONE, x, y, nullptr, PREVIEW_ZOOM);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->Follow();
	}
};

/**
 * Open the preview beside the purchase window, when the 3D is on.
 * @param parent_number the purchase window's number
 */
void ShowThreeDVehiclePreview(WindowNumber parent_number)
{
	if (!ThreeDEnabled()) return;
	CloseWindowById(WindowClass::ThreeDVehiclePreview, 0, false);
	new ThreeDPreviewWindow(_three_d_vehicle_preview_desc, true, parent_number);
}

/**
 * Open the preview beside an industry's window, when the 3D is on and the
 * industry is one of the game's own with eight renders.
 * @param industry the industry
 */
void ShowThreeDIndustryPreview(IndustryID industry)
{
	if (!ThreeDEnabled() || !Industry::IsValidID(industry)) return;
	const Industry *ind = Industry::Get(industry);
	if (GetThreeDIndustryModel(ind->type, HasBit(ind->random, 0)) == nullptr) return;
	CloseWindowById(WindowClass::ThreeDIndustryPreview, industry, false);
	new ThreeDPreviewWindow(_three_d_industry_preview_desc, false, industry);
}
