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
#include "climate_industries.h"
#include "cargotype.h"
#include "three_d_objects.h"
#include "viewport_func.h"
#include "water_map.h"
#include "timer/timer.h"
#include "timer/timer_game_economy.h"
#include "timer/timer_game_tick.h"

#include "table/strings.h"

#include "safeguards.h"

/** The things of the inventory, in the order of WalkerItem. */
static const WalkerItemSpec _walker_items[] = {
	{STR_WALKER_ITEM_HAMMER, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_ARMOUR, 0, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_BOOTS, 0, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_BEER, 3, 10, WalkerItemKind::Thing, CT_ALCOHOL},
	{STR_WALKER_ITEM_CIGARETTES, 5, 5, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_MARIJUANA, 15, 25, WalkerItemKind::Thing, CT_MARIJUANA},
	{STR_WALKER_ITEM_GOODS, 40, 0, WalkerItemKind::Thing, CT_GOODS},
	{STR_WALKER_ITEM_BOW, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_ARROWS, 0, 0, WalkerItemKind::Ammunition},
	{STR_WALKER_ITEM_CROSSBOW, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_BOLTS, 0, 0, WalkerItemKind::Ammunition},
	{STR_WALKER_ITEM_SWORD, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_TWO_HANDED_SWORD, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_TORCH, 2, 0, WalkerItemKind::Light},
	{STR_WALKER_ITEM_LANTERN, 12, 0, WalkerItemKind::Light},
	{STR_WALKER_ITEM_MACHINE_GUN, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_BULLETS, 0, 0, WalkerItemKind::Ammunition},
	{STR_WALKER_ITEM_ROCKET_LAUNCHER, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_ROCKETS, 0, 0, WalkerItemKind::Ammunition},
	{STR_WALKER_ITEM_MUSHROOMS, 0, 8, WalkerItemKind::Thing, CargoLabel{'MUSH'}},
	{STR_WALKER_ITEM_SEEDLING, 8, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_HAY, 0, 0, WalkerItemKind::Thing, CT_HEMP_FIBRE},
	{STR_WALKER_ITEM_WAR_HAMMER, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_PITCHFORK, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_PAPER_LANTERN, 6, 0, WalkerItemKind::Light},
	{STR_WALKER_ITEM_IRON_LANTERN, 15, 0, WalkerItemKind::Light},
	{STR_WALKER_ITEM_MUSHROOM_GOULASH, 6, 30, WalkerItemKind::Thing},
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
 * Whether a thing is sold at a tile: beer at the pub, and Matylda's seedlings in their pots there (the player, 21. 10.),
 * its mushroom goulash while mushrooms come to it (the player, 24. 10.),
 * marijuana at the vending machine and the coffeeshop, cigarettes and goods at a shop of a town (the player), the torch
 * and the lantern there too. At WALKER_SOLD_BY_MAT: by Mat walking about the town -- his marijuana, a piece at a time
 * (the player: "Mat has only mari, a piece into the inventory"); where he walks is the drawing's, not the game's, so the
 * walker's computer says he is near.
 */
bool IsWalkerItemSoldAt(WalkerItem item, TileIndex tile)
{
	if (item >= WalkerItem::End || GetWalkerItemSpec(item).price == 0) return false;
	if (tile == WALKER_SOLD_BY_MAT) return item == WalkerItem::Marijuana;
	if (!IsValidTile(tile)) return false;
	IndustryType type = IT_INVALID;
	if (IsTileType(tile, TileType::Industry) && IsIndustryCompleted(tile)) type = Industry::GetByTile(tile)->type;
	switch (item) {
		case WalkerItem::Beer:
		case WalkerItem::Seedling: return type == IT_PUB;
		case WalkerItem::MushroomGoulash: return type == IT_PUB && PubCooksGoulash(Industry::GetByTile(tile));
		case WalkerItem::Marijuana: return type == IT_WEED_MACHINE || type == IT_COFFEESHOP;
		case WalkerItem::Cigarettes:
		case WalkerItem::Goods:
		case WalkerItem::Torch:
		case WalkerItem::Lantern:
		case WalkerItem::PaperLantern:
		case WalkerItem::IronLantern: return IsShopHouse(tile);
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
 * An arrow or a bolt shot from the walker's bow or crossbow (first_person_gui.cpp): one less of it.
 * @param flags operation to perform
 * @param item the ammunition
 * @return nothing, or an error
 */
CommandCost CmdShootWalkerItem(DoCommandFlags flags, WalkerItem item)
{
	if (item >= WalkerItem::End || GetWalkerItemSpec(item).kind != WalkerItemKind::Ammunition) return CMD_ERROR;
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	if (c->walker_items[to_underlying(item)] == 0) return CommandCost(STR_ERROR_WALKER_HAS_NONE);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)]--;
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * The walker feeds a dragon of a dragon farm (first_person_gui.cpp; the player, 26. 10.: "beer and marijuana ... and we
 * will feed hay too"): one less of it. Only to watch, the dragon smokes or breathes fire; nothing else comes of it.
 * @param flags operation to perform
 * @param item the beer, the marijuana or the hay
 * @return nothing, or an error
 */
CommandCost CmdFeedWalkerItem(DoCommandFlags flags, WalkerItem item)
{
	if (item != WalkerItem::Beer && item != WalkerItem::Marijuana && item != WalkerItem::Hay) return CMD_ERROR;
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	if (c->walker_items[to_underlying(item)] == 0) return CommandCost(STR_ERROR_WALKER_HAS_NONE);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)]--;
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
		InvalidateWindowData(WindowClass::DragonFeed, 0);
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
		case WalkerItemKind::Weapon:
		case WalkerItemKind::Light: return std::max<uint16_t>(carried, 1);
		case WalkerItemKind::Ammunition:
			/* The machine gun's bullets and the rockets each by their own full count, as the quiver is to the arrows. */
			if (item == WalkerItem::Bullets) return std::max<uint16_t>(carried, static_cast<uint16_t>(std::min<uint>(WALKER_ITEM_MAX, quiver * WALKER_BULLETS_FULL / WALKER_AMMUNITION_FULL)));
			if (item == WalkerItem::Rockets) return std::max<uint16_t>(carried, static_cast<uint16_t>(quiver * WALKER_ROCKETS_FULL / WALKER_AMMUNITION_FULL));
			return std::max(carried, quiver);
		default: return carried;
	}
}

/**
 * The cheat iddqd (the console): every weapon the walker has not, the arrows filled up (WalkerCheatCount()).
 * @param flags operation to perform
 * @param quiver the arrows filled up to (WALKER_AMMUNITION_FULL)
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

/** What a dog of the sellers costs ("cena", pounds), inflated as the things of the inventory; 0 one not sold. */
Money WalkerPetPrice(std::string_view name)
{
	const ThreeDAnimal *a = GetThreeDAnimal(name);
	if (a == nullptr || !a->sold) return 0;
	return Money(static_cast<int64_t>(a->price) * static_cast<int64_t>(_economy.inflation_prices) >> 16);
}

/**
 * A dog bought of a seller of dogs (the player, 18. 10.: "the dogs will be sold by three sellers and will walk with
 * the player"), paid with the company's money: it walks with the walker from now on, instead of the one it had. An
 * empty name lets the walker's dog go.
 * @param flags operation to perform
 * @param name the dog, a ThreeDAnimal sold ("prodej")
 * @return the cost or an error
 */
CommandCost CmdBuyWalkerPet(DoCommandFlags flags, const std::string &name)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	CommandCost cost(ExpensesType::Other);
	if (!name.empty()) {
		const Money price = WalkerPetPrice(name);
		if (price <= 0) return CommandCost(STR_ERROR_WALKER_NOT_SOLD_HERE);
		if (name == c->walker_pet) return CommandCost(STR_ERROR_WALKER_HAS_THIS_DOG);
		cost.AddCost(price);
	}
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_pet = name;
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return cost;
}

/**
 * A thing the walker picks up where it is (the player, 18. 10.: "the mushrooms will be picked about the clay towns"):
 * one more of it in the inventory. Only the mushrooms are picked so, and the lantern of the forest cabin's table (the
 * player, 26. 10.: "I pick up the lantern with E and that is it").
 * @param flags operation to perform
 * @param item the thing
 * @return nothing, or an error
 */
CommandCost CmdPickWalkerItem(DoCommandFlags flags, WalkerItem item)
{
	if (item != WalkerItem::Mushrooms && item != WalkerItem::Lantern) return CMD_ERROR;
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	if (c->walker_items[to_underlying(item)] >= WALKER_ITEM_MAX) return CommandCost(STR_ERROR_WALKER_CARRIES_TOO_MANY);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)]++;
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * A thing put under a quick key of the walker (the player, 23. 10.: "the player assigns them as in Morrowind, what is
 * under 1 and under 2"): off any other key it was under, the one there before taken off.
 * @param key the key, 0 for 1 to 8 for 9
 * @param item the thing, End to empty the key
 */
CommandCost CmdSetWalkerQuickKey(DoCommandFlags flags, uint8_t key, WalkerItem item)
{
	if (key >= WALKER_QUICK_KEYS || item > WalkerItem::End) return CMD_ERROR;
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	if (flags.Test(DoCommandFlag::Execute)) {
		for (uint8_t &k : c->walker_quick) {
			if (k == to_underlying(item)) k = to_underlying(WalkerItem::End);
		}
		c->walker_quick[key] = to_underlying(item);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/** The plants the walkers planted (WalkerPlant), saved with the game. */
std::vector<WalkerPlant> _walker_plants;

/** How many days old a plant is. */
int32_t WalkerPlantAgeDays(const WalkerPlant &p)
{
	return std::max<int32_t>(0, TimerGameEconomy::date.base() - p.planted);
}

/** How a plant is now: in its pot for its first seconds, then growing, ripe at half a year, hay at a year. */
WalkerPlantStage GetWalkerPlantStage(const WalkerPlant &p)
{
	if (TimerGameTick::counter < p.planted_tick + WALKER_PLANT_POT_TICKS) return WalkerPlantStage::InPot;
	const int32_t age = WalkerPlantAgeDays(p);
	if (age >= WALKER_PLANT_HAY_DAYS) return WalkerPlantStage::Hay;
	if (age >= WALKER_PLANT_RIPE_DAYS) return WalkerPlantStage::Ripe;
	return WalkerPlantStage::Growing;
}

/** The nearest plant within reach of a point (the walker), metres; nullptr none. */
const WalkerPlant *WalkerPlantNear(double x, double y, double reach_m)
{
	const WalkerPlant *best = nullptr;
	double best_d = reach_m * OBJECT_UNITS_PER_METRE;
	for (const WalkerPlant &p : _walker_plants) {
		const double d = std::hypot(p.x - x, p.y - y);
		if (d <= best_d) {
			best_d = d;
			best = &p;
		}
	}
	return best;
}

/** The map where a plant grows drawn again: it changed. */
static void MarkWalkerPlantDirty(const WalkerPlant &p)
{
	const TileIndex tile = TileVirtXY(p.x, p.y);
	if (IsValidTile(tile)) MarkTileDirtyByTile(tile);
}

/** The plants of a company at the most: a garden, not a plantation (that is an industry). */
static constexpr size_t WALKER_PLANTS_MAX = 64;

/**
 * Plant a seedling of the walker's inventory where it stands, in front of it: in its pot for a few seconds, then in
 * the ground (WalkerPlant).
 * @param flags operation to perform
 * @param x,y where, world units
 * @return nothing, or an error
 */
CommandCost CmdPlantWalkerSeedling(DoCommandFlags flags, int32_t x, int32_t y)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || x < 0 || y < 0) return CMD_ERROR;
	const TileIndex tile = TileVirtXY(x, y);
	if (!IsValidTile(tile) || x >= static_cast<int32_t>(Map::SizeX() * TILE_SIZE) || y >= static_cast<int32_t>(Map::SizeY() * TILE_SIZE)) return CMD_ERROR;
	if (IsTileType(tile, TileType::Water)) return CommandCost(STR_ERROR_WALKER_CANNOT_PLANT_HERE);
	if (c->walker_items[to_underlying(WalkerItem::Seedling)] == 0) return CommandCost(STR_ERROR_WALKER_HAS_NONE);
	if (std::ranges::count_if(_walker_plants, [&](const WalkerPlant &p) { return p.owner == c->index.base(); }) >= static_cast<ptrdiff_t>(WALKER_PLANTS_MAX)) return CommandCost(STR_ERROR_WALKER_TOO_MANY_PLANTS);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(WalkerItem::Seedling)]--;
		WalkerPlant &p = _walker_plants.emplace_back();
		p.x = x;
		p.y = y;
		p.planted = TimerGameEconomy::date.base();
		p.planted_tick = TimerGameTick::counter;
		p.owner = c->index.base();
		MarkWalkerPlantDirty(p);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * Harvest the walker's plant at a place: ripe, its marijuana into the inventory; too late, its hay.
 * @param flags operation to perform
 * @param x,y where it grows, world units (WalkerPlant::x, y)
 * @return nothing, or an error
 */
CommandCost CmdHarvestWalkerPlant(DoCommandFlags flags, int32_t x, int32_t y)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	auto it = std::ranges::find_if(_walker_plants, [&](const WalkerPlant &p) { return p.x == x && p.y == y; });
	if (it == _walker_plants.end()) return CMD_ERROR;
	const WalkerPlantStage stage = GetWalkerPlantStage(*it);
	if (stage != WalkerPlantStage::Ripe && stage != WalkerPlantStage::Hay) return CommandCost(STR_ERROR_WALKER_PLANT_NOT_RIPE);
	const WalkerItem item = stage == WalkerPlantStage::Ripe ? WalkerItem::Marijuana : WalkerItem::Hay;
	const uint16_t count = stage == WalkerPlantStage::Ripe ? WALKER_PLANT_HARVEST : 1;
	if (c->walker_items[to_underlying(item)] + count > WALKER_ITEM_MAX) return CommandCost(STR_ERROR_WALKER_CARRIES_TOO_MANY);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)] += count;
		MarkWalkerPlantDirty(*it);
		_walker_plants.erase(it);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/** A plant out of its pot, or a day on (it grows), drawn again on the map. */
static const IntervalTimer<TimerGameTick> _walker_plants_out_of_pot({TimerGameTick::Priority::None, 1}, [](auto) {
	for (const WalkerPlant &p : _walker_plants) {
		if (TimerGameTick::counter == p.planted_tick + WALKER_PLANT_POT_TICKS) MarkWalkerPlantDirty(p);
	}
});
static const IntervalTimer<TimerGameEconomy> _walker_plants_grow({TimerGameEconomy::Trigger::Day, TimerGameEconomy::Priority::None}, [](auto) {
	for (const WalkerPlant &p : _walker_plants) MarkWalkerPlantDirty(p);
});
