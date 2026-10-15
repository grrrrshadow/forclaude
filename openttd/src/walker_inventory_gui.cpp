/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file walker_inventory_gui.cpp The window of the walker's inventory (walker_inventory.h): its health, the things it
 * carries -- a click uses the beer or the marijuana -- and the things sold where it stands -- a click buys one.
 */

#include "stdafx.h"
#include "walker_cmd.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "strings_func.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "widgets/walker_inventory_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** The tile near the walker of the view from the first person where things are sold, or INVALID_TILE (first_person_gui.cpp). */
TileIndex FirstPersonShopNearby();
/** The tile the walker of the view from the first person stands on, or INVALID_TILE (first_person_gui.cpp). */
TileIndex FirstPersonWalkerTile();

static constexpr NWidgetPart _nested_walker_inventory_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Brown),
		NWidget(WWT_CAPTION, Colours::Brown, WID_WI_CAPTION), SetStringTip(STR_WALKER_INVENTORY_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::Brown),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Brown, WID_WI_LIST), SetMinimalSize(320, 220), SetResize(1, 1), SetFill(1, 1), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_SPACER), SetFill(1, 0), SetResize(1, 0),
		NWidget(WWT_RESIZEBOX, Colours::Brown),
	EndContainer(),
};

/** The window of the walker's inventory; there is one, number 0. */
struct WalkerInventoryWindow : Window {
	/** A line of the list a click acts on. */
	struct Line {
		int top, bottom;
		WalkerItem item;
		bool buy; ///< buy one here, else use one
	};
	mutable std::vector<Line> lines; ///< where the clickable lines were drawn
	TileIndex shop = INVALID_TILE;   ///< where things are sold near the walker, as last seen

	WalkerInventoryWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->FinishInitNested(number);
		this->shop = FirstPersonShopNearby();
	}

	/** The walker walks on: what is sold where it stands now. */
	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		TileIndex now = FirstPersonShopNearby();
		if (now != this->shop) {
			this->shop = now;
			this->SetWidgetDirty(WID_WI_LIST);
		}
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetWidgetDirty(WID_WI_LIST);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_WI_LIST) return;
		this->lines.clear();
		const Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
		const int lh = GetCharacterHeight(FontSize::Normal) + 3;
		int y = ir.top;
		const Company *c = Company::GetIfValid(_local_company);
		if (c == nullptr) {
			DrawString(ir.left, ir.right, y, STR_WALKER_NO_COMPANY, TextColour::White);
			return;
		}

		/* The health, and a bar of it. */
		DrawString(ir.left, ir.right, y, GetString(STR_WALKER_HEALTH, c->walker_health, WALKER_HEALTH_MAX), TextColour::White);
		const int bar_left = ir.left + ir.Width() / 2, bar_top = y + 3, bar_bottom = y + lh - 4;
		GfxFillRect(bar_left, bar_top, ir.right, bar_bottom, GetColourGradient(Colours::Grey, Shade::Darkest));
		GfxFillRect(bar_left, bar_top, bar_left + (ir.right - bar_left) * c->walker_health / WALKER_HEALTH_MAX, bar_bottom, GetColourGradient(Colours::Red, Shade::Light));
		y += lh + 4;

		/* What it carries; the beer and the marijuana used with a click. */
		DrawString(ir.left, ir.right, y, STR_WALKER_CARRIED, TextColour::Yellow);
		y += lh;
		for (uint8_t i = 0; i < to_underlying(WalkerItem::End); i++) {
			const WalkerItem item = static_cast<WalkerItem>(i);
			const uint count = c->walker_items[i];
			if (count == 0) continue;
			const WalkerItemSpec &spec = GetWalkerItemSpec(item);
			DrawString(ir.left + 8, ir.right, y, GetString(STR_WALKER_ITEM_COUNT, spec.name, count), TextColour::White);
			if (spec.heal != 0) {
				DrawString(ir.left, ir.right, y, GetString(STR_WALKER_USE, spec.heal), TextColour::Orange, AlignmentH::End);
				this->lines.push_back({y, y + lh, item, false});
			}
			y += lh;
		}
		y += 4;

		/* What is sold here; bought with a click. */
		const StringID place = WalkerShopName(this->shop);
		if (place == INVALID_STRING_ID) {
			DrawStringMultiLine(ir.left, ir.right, y, ir.bottom, STR_WALKER_NOTHING_FOR_SALE, TextColour::Silver);
			return;
		}
		DrawString(ir.left, ir.right, y, GetString(STR_WALKER_FOR_SALE, place), TextColour::Yellow);
		y += lh;
		for (uint8_t i = 0; i < to_underlying(WalkerItem::End); i++) {
			const WalkerItem item = static_cast<WalkerItem>(i);
			if (!IsWalkerItemSoldAt(item, this->shop)) continue;
			DrawString(ir.left + 8, ir.right, y, GetWalkerItemSpec(item).name, TextColour::White);
			DrawString(ir.left, ir.right, y, GetString(STR_WALKER_BUY, WalkerItemPrice(item)), TextColour::Green, AlignmentH::End);
			this->lines.push_back({y, y + lh, item, true});
			y += lh;
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_WI_LIST) return;
		for (const Line &l : this->lines) {
			if (pt.y < l.top || pt.y >= l.bottom) continue;
			if (l.buy) {
				Command<Commands::BuyWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_BUY, this->shop, l.item);
			} else {
				const TileIndex here = FirstPersonWalkerTile();
				Command<Commands::UseWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_USE, IsValidTile(here) ? here : TileIndex{}, l.item);
			}
			return;
		}
	}
};

static WindowDesc _walker_inventory_desc(
	WindowPosition::Automatic, "walker_inventory", 0, 0,
	WindowClass::WalkerInventory, WindowClass::None,
	{},
	_nested_walker_inventory_widgets
);

/** Open the walker's inventory, or bring it to the front. */
void ShowWalkerInventoryWindow()
{
	AllocateWindowDescFront<WalkerInventoryWindow>(_walker_inventory_desc, 0);
}
