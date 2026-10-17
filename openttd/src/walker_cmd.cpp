/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file walker_cmd.cpp The inventory of the walker of the view from the first person: what is sold where, buying, using (walker_inventory.h). */

#include "stdafx.h"
#include "walker_cmd.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "window_func.h"
#include "tile_map.h"
#include "house.h"
#include "town_map.h"
#include "industry.h"
#include "industry_map.h"
#include "cargotype.h"

#include "table/strings.h"

#include "safeguards.h"

/** The things of the inventory, in the order of WalkerItem. */
static const WalkerItemSpec _walker_items[] = {
	{STR_WALKER_ITEM_HAMMER, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_ARMOUR, 0, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_BOOTS, 0, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_BEER, 3, 10, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_CIGARETTES, 5, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_MARIJUANA, 15, 25, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_GOODS, 40, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_BOW, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_ARROWS, 0, 0, WalkerItemKind::Ammunition},
};
static_assert(std::size(_walker_items) == to_underlying(WalkerItem::End));

/** What a thing of the inventory is. */
const WalkerItemSpec &GetWalkerItemSpec(WalkerItem item)
{
	assert(item < WalkerItem::End);
	return _walker_items[to_underlying(item)];
}

/** What a thing costs now: its price, with the inflation of the game. */
Money WalkerItemPrice(WalkerItem item)
{
	return Money(GetWalkerItemSpec(item).price * static_cast<int64_t>(_economy.inflation_prices) >> 16);
}

/** Whether a house is a shop of a town: one that takes goods. */
static bool IsShopHouse(TileIndex tile)
{
	if (!IsTileType(tile, TileType::House)) return false;
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));
	CargoType goods = GetCargoTypeByLabel(CT_GOODS);
	if (!IsValidCargoType(goods)) return false;
	for (uint i = 0; i < std::size(hs->accepts_cargo); i++) {
		if (hs->accepts_cargo[i] == goods && hs->cargo_acceptance[i] > 0) return true;
	}
	return false;
}

/**
 * Whether a thing is sold at a tile: beer at the pub, marijuana at the vending machine and the coffeeshop, cigarettes
 * and goods at a shop of a town (the player).
 */
bool IsWalkerItemSoldAt(WalkerItem item, TileIndex tile)
{
	if (item >= WalkerItem::End || GetWalkerItemSpec(item).price == 0 || !IsValidTile(tile)) return false;
	IndustryType type = IT_INVALID;
	if (IsTileType(tile, TileType::Industry) && IsIndustryCompleted(tile)) type = Industry::GetByTile(tile)->type;
	switch (item) {
		case WalkerItem::Beer: return type == IT_PUB;
		case WalkerItem::Marijuana: return type == IT_WEED_MACHINE || type == IT_COFFEESHOP;
		case WalkerItem::Cigarettes:
		case WalkerItem::Goods: return IsShopHouse(tile);
		default: return false;
	}
}

/** The name of the place a tile sells things at, or INVALID_STRING_ID when it sells nothing. */
StringID WalkerShopName(TileIndex tile)
{
	if (!IsValidTile(tile)) return INVALID_STRING_ID;
	if (IsTileType(tile, TileType::Industry) && IsIndustryCompleted(tile)) {
		IndustryType type = Industry::GetByTile(tile)->type;
		if (type == IT_PUB || type == IT_WEED_MACHINE || type == IT_COFFEESHOP) return GetIndustrySpec(type)->name;
	}
	if (IsShopHouse(tile)) return STR_WALKER_SHOP;
	return INVALID_STRING_ID;
}

/**
 * Buy a thing for the walker's inventory, paid with the company's money.
 * @param flags operation to perform
 * @param tile where it is sold
 * @param item the thing
 * @return the cost or an error
 */
CommandCost CmdBuyWalkerItem(DoCommandFlags flags, TileIndex tile, WalkerItem item)
{
	if (item >= WalkerItem::End) return CMD_ERROR;
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	if (!IsWalkerItemSoldAt(item, tile)) return CommandCost(STR_ERROR_WALKER_NOT_SOLD_HERE);
	if (c->walker_items[to_underlying(item)] >= WALKER_ITEM_MAX) return CommandCost(STR_ERROR_WALKER_CARRIES_TOO_MANY);
	CommandCost cost(ExpensesType::Other, WalkerItemPrice(item));
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)]++;
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return cost;
}

/**
 * Use a thing of the walker's inventory: drink the beer, smoke the marijuana -- each gives health.
 * @param flags operation to perform
 * @param item the thing
 * @return nothing, or an error
 */
CommandCost CmdUseWalkerItem(DoCommandFlags flags, WalkerItem item)
{
	if (item >= WalkerItem::End) return CMD_ERROR;
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	const WalkerItemSpec &spec = GetWalkerItemSpec(item);
	if (spec.heal == 0) return CommandCost(STR_ERROR_WALKER_CANNOT_USE);
	if (c->walker_items[to_underlying(item)] == 0) return CommandCost(STR_ERROR_WALKER_HAS_NONE);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)]--;
		c->walker_health = static_cast<uint8_t>(std::min<uint>(WALKER_HEALTH_MAX, c->walker_health + spec.heal));
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * Hurt the walker of the view from the first person: a dragon's blow (the player, 15. 10.: "they attack the player").
 * At no health left the view closes and the player is back on the map (first_person_gui.cpp).
 * @param flags operation to perform
 * @param damage how much health it takes
 * @return nothing, or an error
 */
CommandCost CmdHurtWalker(DoCommandFlags flags, uint8_t damage)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_health = static_cast<uint8_t>(c->walker_health > damage ? c->walker_health - damage : 0);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * The walker of the view from the first person back, after it had no health left: the view opened again.
 * @param flags operation to perform
 * @param health the health it comes back with (the view gives it all)
 * @return nothing, or an error when it has health still
 */
CommandCost CmdReviveWalker(DoCommandFlags flags, uint8_t health)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || c->walker_health != 0 || health == 0) return CMD_ERROR;
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_health = std::min<uint8_t>(health, WALKER_HEALTH_MAX);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * How many of a thing the walker carries after the cheat (the player, 17. 10.: "the cheat iddqd at the console gives
 * me all the weapons; what is in the inventory it will not give a second time, it fills up the arrows; when there is
 * no bow it gives the bow too"): a weapon not carried one, the ammunition up to a full quiver, the rest as it is.
 * @param item the thing
 * @param carried how many of it the walker carries
 * @param quiver the ammunition filled up to
 */
uint16_t WalkerCheatCount(WalkerItem item, uint16_t carried, uint16_t quiver)
{
	switch (GetWalkerItemSpec(item).kind) {
		case WalkerItemKind::Weapon: return std::max<uint16_t>(carried, 1);
		case WalkerItemKind::Ammunition: return std::max(carried, quiver);
		default: return carried;
	}
}

/**
 * The cheat iddqd (the console): every weapon the walker has not, the arrows filled up (WalkerCheatCount()).
 * @param flags operation to perform
 * @param quiver the arrows filled up to (WALKER_ARROWS_FULL)
 * @return nothing, or an error
 */
CommandCost CmdWalkerCheat(DoCommandFlags flags, uint16_t quiver)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || quiver > WALKER_ITEM_MAX) return CMD_ERROR;
	if (flags.Test(DoCommandFlag::Execute)) {
		for (uint8_t i = 0; i < to_underlying(WalkerItem::End); i++) {
			c->walker_items[i] = WalkerCheatCount(static_cast<WalkerItem>(i), c->walker_items[i], quiver);
		}
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}
