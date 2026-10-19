/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file walker_inventory_gui.cpp The window of the walker's inventory (walker_inventory.h): its health, the things it
 * carries -- a click uses the beer or the marijuana, takes a weapon in the hand or puts it away -- and the things sold
 * where it stands -- a click buys one.
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
#include "three_d_animals.h"
#include "three_d_objects.h"
#include "town.h"
#include "tile_map.h"
#include "widgets/walker_inventory_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** The tile near the walker of the view from the first person where things are sold, or INVALID_TILE (first_person_gui.cpp). */
TileIndex FirstPersonShopNearby();
/** The tile the walker of the view from the first person stands on, or INVALID_TILE (first_person_gui.cpp). */
TileIndex FirstPersonWalkerTile();
/** The weapons of the walker of the view from the first person (first_person_gui.cpp): a weapon's key, the one in the hand, one taken. */
int FirstPersonWeaponKey(WalkerItem item);
WalkerItem FirstPersonWeaponInHand();
void FirstPersonTakeWeapon(WalkerItem item);
bool FirstPersonOpen();
/** The light in the left hand of the walker (first_person_gui.cpp): the one there, one taken or put out. */
WalkerItem FirstPersonLightInHand();
void FirstPersonTakeLight(WalkerItem item);
/** Where the walker of the view from the first person stands, world units; false when the view is not open. */
bool FirstPersonWalkerAt(double &x, double &y);
/** The direction the walker of the view from the first person faces, radians (first_person_gui.cpp). */
double FirstPersonWalkerHeading();

/**
 * Where the walker stands, as told under its health (the player, #287: out of the view from the first person into the
 * inventory): the town it is near, its tile and the direction it faces; empty without the view.
 */
static std::string WalkerWhere()
{
	double x, y;
	if (!FirstPersonWalkerAt(x, y)) return {};
	const TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	const Town *town = ClosestTownFromTile(tile, UINT_MAX);
	if (town == nullptr) return {};
	const int deg = static_cast<int>(std::lround(std::fmod(FirstPersonWalkerHeading() * 180.0 / M_PI + 360.0, 360.0)));
	return GetString(STR_FIRST_PERSON_WHERE, town->index, TileX(tile), TileY(tile), deg);
}

/** How near a seller of dogs the walker buys of it, metres. */
static constexpr double SELLER_REACH_M = 6.0;

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
	/** What a click on a line does. */
	enum class LineDoes : uint8_t {
		Use,  ///< use one (the beer, the marijuana)
		Buy,  ///< buy one here
		Hand, ///< a weapon into the hand, or put away
		Light, ///< a light into the left hand, or put out
		BuyDog, ///< buy a dog of the seller near
		LetDogGo, ///< let the walker's dog go
	};
	/** A line of the list a click acts on. */
	struct Line {
		int top, bottom;
		WalkerItem item;
		LineDoes does;
		std::string dog = {}; ///< the dog a line buys
	};
	mutable std::vector<Line> lines; ///< where the clickable lines were drawn
	TileIndex shop = INVALID_TILE;   ///< where things are sold near the walker, as last seen
	WalkerItem in_hand = WalkerItem::End; ///< the weapon in the walker's hand, as last drawn
	mutable std::string where;       ///< where the walker is (WalkerWhere()), as last drawn
	mutable std::string seller;      ///< the seller of dogs near the walker, as last seen; empty none
	mutable std::vector<std::string> dogs; ///< and the dogs it sells

	/** The seller of dogs near the walker now, and what it sells; whether that changed. */
	bool SeeSeller() const
	{
		double x = 0, y = 0;
		const ThreeDDogSeller *s = FirstPersonWalkerAt(x, y) ? ThreeDDogSellerNear(x, y, SELLER_REACH_M) : nullptr;
		std::string name = s != nullptr ? s->name : std::string();
		std::vector<std::string> dogs = s != nullptr ? s->offer : std::vector<std::string>();
		if (name == this->seller && dogs == this->dogs) return false;
		this->seller = std::move(name);
		this->dogs = std::move(dogs);
		return true;
	}

	WalkerInventoryWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->CreateNestedTree();
		this->FinishInitNested(number);
		this->shop = FirstPersonShopNearby();
		this->SeeSeller();
	}

	/** The walker walks on: what is sold where it stands now, where it is; and the weapon in its hand. */
	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		TileIndex now = FirstPersonShopNearby();
		const WalkerItem hand = FirstPersonWeaponInHand();
		std::string where = WalkerWhere();
		if (this->SeeSeller() || now != this->shop || hand != this->in_hand || where != this->where) {
			this->shop = now;
			this->in_hand = hand;
			this->where = std::move(where);
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
		y += lh;
		/* Where the walker is. */
		this->where = WalkerWhere();
		if (!this->where.empty()) {
			DrawString(ir.left, ir.right, y, this->where, TextColour::White);
			y += lh;
		}
		y += 4;

		/* What it carries; the beer and the marijuana used with a click, a weapon taken in the hand (as its key 1 to 5
		 * does in the view from the first person, Morrowind's quick keys) or put away. */
		DrawString(ir.left, ir.right, y, STR_WALKER_CARRIED, TextColour::Yellow);
		y += lh;
		const WalkerItem hand = FirstPersonWeaponInHand();
		for (uint8_t i = 0; i < to_underlying(WalkerItem::End); i++) {
			const WalkerItem item = static_cast<WalkerItem>(i);
			const uint count = c->walker_items[i];
			if (count == 0) continue;
			const WalkerItemSpec &spec = GetWalkerItemSpec(item);
			DrawString(ir.left + 8, ir.right, y, GetString(STR_WALKER_ITEM_COUNT, spec.name, count), item == hand ? TextColour::Yellow : TextColour::White);
			const int key = FirstPersonWeaponKey(item);
			if (spec.heal != 0) {
				DrawString(ir.left, ir.right, y, GetString(STR_WALKER_USE, spec.heal), TextColour::Orange, AlignmentH::End);
				this->lines.push_back({y, y + lh, item, LineDoes::Use});
			} else if (key != 0 && FirstPersonOpen()) {
				DrawString(ir.left, ir.right, y, GetString(item == hand ? STR_WALKER_IN_HAND : STR_WALKER_TAKE, key), item == hand ? TextColour::Green : TextColour::Orange, AlignmentH::End);
				this->lines.push_back({y, y + lh, item, LineDoes::Hand});
			} else if (spec.kind == WalkerItemKind::Light && FirstPersonOpen()) {
				const bool lit = FirstPersonLightInHand() == item;
				DrawString(ir.left, ir.right, y, lit ? STR_WALKER_LIGHT_IN_HAND : STR_WALKER_TAKE_LIGHT, lit ? TextColour::Green : TextColour::Orange, AlignmentH::End);
				this->lines.push_back({y, y + lh, item, LineDoes::Light});
			}
			y += lh;
		}
		/* The walker's dog, let go with a click. */
		if (const ThreeDAnimal *dog = c->walker_pet.empty() ? nullptr : GetThreeDAnimal(c->walker_pet); dog != nullptr) {
			DrawString(ir.left + 8, ir.right, y, GetString(STR_WALKER_YOUR_DOG, dog->Title()), TextColour::White);
			DrawString(ir.left, ir.right, y, STR_WALKER_LET_DOG_GO, TextColour::Orange, AlignmentH::End);
			this->lines.push_back({y, y + lh, WalkerItem::End, LineDoes::LetDogGo});
			y += lh;
		}
		y += 4;

		/* The dogs of a seller near; bought with a click (the walker's dog then, instead of the one it had). */
		this->SeeSeller();
		const ThreeDAnimal *seller = this->seller.empty() ? nullptr : GetThreeDAnimal(this->seller);
		if (seller != nullptr) {
			DrawString(ir.left, ir.right, y, GetString(STR_WALKER_DOGS_FOR_SALE, seller->Title()), TextColour::Yellow);
			y += lh;
			for (const std::string &name : this->dogs) {
				const ThreeDAnimal *dog = GetThreeDAnimal(name);
				if (dog == nullptr) continue;
				DrawString(ir.left + 8, ir.right, y, GetString(STR_WALKER_DOG, dog->Title()), name == c->walker_pet ? TextColour::Yellow : TextColour::White);
				DrawString(ir.left, ir.right, y, GetString(STR_WALKER_BUY, WalkerPetPrice(name)), TextColour::Green, AlignmentH::End);
				this->lines.push_back({y, y + lh, WalkerItem::End, LineDoes::BuyDog, name});
				y += lh;
			}
			y += 4;
		}

		/* What is sold here; bought with a click. */
		const StringID place = WalkerShopName(this->shop);
		if (place == INVALID_STRING_ID) {
			if (seller == nullptr) DrawStringMultiLine(ir.left, ir.right, y, ir.bottom, STR_WALKER_NOTHING_FOR_SALE, TextColour::Silver);
			return;
		}
		DrawString(ir.left, ir.right, y, GetString(STR_WALKER_FOR_SALE, place), TextColour::Yellow);
		y += lh;
		for (uint8_t i = 0; i < to_underlying(WalkerItem::End); i++) {
			const WalkerItem item = static_cast<WalkerItem>(i);
			if (!IsWalkerItemSoldAt(item, this->shop)) continue;
			DrawString(ir.left + 8, ir.right, y, GetWalkerItemSpec(item).name, TextColour::White);
			DrawString(ir.left, ir.right, y, GetString(STR_WALKER_BUY, WalkerItemPrice(item)), TextColour::Green, AlignmentH::End);
			this->lines.push_back({y, y + lh, item, LineDoes::Buy});
			y += lh;
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_WI_LIST) return;
		for (const Line &l : this->lines) {
			if (pt.y < l.top || pt.y >= l.bottom) continue;
			switch (l.does) {
				case LineDoes::Buy:
					Command<Commands::BuyWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_BUY, this->shop, l.item);
					break;
				case LineDoes::Use: {
					const TileIndex here = FirstPersonWalkerTile();
					Command<Commands::UseWalkerItem>::Post(STR_ERROR_WALKER_CAN_T_USE, IsValidTile(here) ? here : TileIndex{}, l.item);
					break;
				}
				case LineDoes::Hand:
					FirstPersonTakeWeapon(l.item);
					this->in_hand = FirstPersonWeaponInHand();
					this->SetWidgetDirty(WID_WI_LIST);
					break;
				case LineDoes::Light:
					FirstPersonTakeLight(l.item);
					this->SetWidgetDirty(WID_WI_LIST);
					break;
				case LineDoes::BuyDog:
					Command<Commands::BuyWalkerPet>::Post(STR_ERROR_WALKER_CAN_T_BUY, l.dog);
					break;
				case LineDoes::LetDogGo:
					Command<Commands::BuyWalkerPet>::Post(std::string());
					break;
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

/**
 * The rig: a click on the line of a thing in the inventory open, as drawn last (its window focused, as a click does).
 * @return what the line said a click does: "pouzit", "koupit", "ruka", or "" for no such line
 */
std::string RigWalkerInventoryClick(WalkerItem item)
{
	WalkerInventoryWindow *w = static_cast<WalkerInventoryWindow *>(FindWindowById(WindowClass::WalkerInventory, 0));
	if (w == nullptr) return {};
	for (const WalkerInventoryWindow::Line &l : w->lines) {
		if (l.item != item) continue;
		SetFocusedWindow(w);
		w->OnClick({w->width / 2, l.top + 1}, WID_WI_LIST, 1);
		return l.does == WalkerInventoryWindow::LineDoes::Buy ? "koupit" : l.does == WalkerInventoryWindow::LineDoes::Use ? "pouzit" : "ruka";
	}
	return {};
}
