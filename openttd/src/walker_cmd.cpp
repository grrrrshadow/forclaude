/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
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
#include "vehicle_base.h"
#include "industry_map.h"
#include "climate_industries.h"
#include "cargotype.h"
#include "three_d_objects.h"
#include "viewport_func.h"
#include "water_map.h"
#include "bridge_map.h"
#include "clear_map.h"
#include "news_func.h"
#include "strings_func.h"
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
	{STR_WALKER_ITEM_MINE_HAMMER, 0, 0, WalkerItemKind::Weapon},
	{STR_WALKER_ITEM_CELTIC_TENT, 0, 0, WalkerItemKind::Thing},
	{STR_WALKER_ITEM_CAMPING_TENT, 0, 0, WalkerItemKind::Thing},
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
 * At no health left the view closes and the player is back on the map (first_person_gui.cpp). In a vehicle the blow
 * breaks the vehicle down first, as the dragons of a farm do, and only while it is broken down does the walker take it
 * (the player, 1. 11.: "first a broken-down Trabant, and at the next blow the figure gets it").
 * @param flags operation to perform
 * @param damage how much health it takes
 * @param riding the vehicle the walker is in, INVALID_VEHICLE on foot
 * @return nothing, or an error
 */
CommandCost CmdHurtWalker(DoCommandFlags flags, uint8_t damage, VehicleID riding)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	Vehicle *v = Vehicle::GetIfValid(riding);
	if (v != nullptr) v = v->First();
	if (v != nullptr && v->breakdown_ctr == 0 && !v->vehstatus.Test(VehState::Crashed) &&
			(v->type == VehicleType::Train || v->type == VehicleType::Road || v->type == VehicleType::Ship || v->type == VehicleType::Aircraft)) {
		if (flags.Test(DoCommandFlag::Execute)) {
			/* Counting down to the breakdown as CheckVehicleBreakdown() starts one: it comes in a tick or two. */
			v->breakdown_ctr = 3;
			v->breakdown_delay = static_cast<uint8_t>(GB(Random(), 0, 7) + 0x80);
			v->breakdown_chance = 0;
		}
		return CommandCost();
	}
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
		/* A tent of each kind, when it has none and none of its is pitched (the player: "iddqd fills them up"). */
		for (WalkerItem tent : {WalkerItem::CelticTent, WalkerItem::CampingTent}) {
			const bool pitched = std::ranges::any_of(_walker_tents, [&](const WalkerTent &t) { return t.owner == c->index.base() && t.item == to_underlying(tent); });
			if (!pitched && c->walker_items[to_underlying(tent)] == 0) c->walker_items[to_underlying(tent)] = 1;
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

/** What a month of the walker's health insurance costs, inflated as the dogs. */
Money WalkerNursePrice()
{
	return Money(WALKER_NURSE_PRICE * static_cast<int64_t>(_economy.inflation_prices) >> 16);
}

/** Whether the walker is insured today: a nurse of a church walks with it and heals it. */
bool WalkerInsured(const CompanyProperties &c)
{
	return c.walker_nurse_until >= TimerGameEconomy::date;
}

/**
 * A month of the walker's health insurance bought of a nurse before a church (the player, 28. 10.: "the nurses will
 * sell a monthly health insurance before the church; when I pay, they will walk with me a month and heal the wounds of
 * the bogeymen"), paid with the company's money: a nurse walks with the walker so long and heals it
 * (CmdHealWalkerByNurse()). Bought again while it runs, it runs so much longer.
 * @param flags operation to perform
 * @param months how many months of it (the inventory buys one)
 * @return the cost or an error
 */
CommandCost CmdBuyWalkerNurse(DoCommandFlags flags, uint8_t months)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || months == 0 || months > 12) return CMD_ERROR;
	CommandCost cost(ExpensesType::Other);
	cost.AddCost(WalkerNursePrice() * months);
	if (flags.Test(DoCommandFlag::Execute)) {
		const TimerGameEconomy::Date from = WalkerInsured(*c) ? c->walker_nurse_until : TimerGameEconomy::date;
		c->walker_nurse_until = from + WALKER_NURSE_DAYS * months;
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return cost;
}

/**
 * The walker's nurse heals it a little, while it is insured and has health to gain; the view from the first person asks
 * it every few seconds while she is with the walker (WALKER_NURSE_HEAL_TICKS).
 * @param flags operation to perform
 * @param health how much health she gives back, WALKER_NURSE_HEAL at most
 * @return nothing, or an error when it is not insured, has no health left (gone back to the map) or all of it
 */
CommandCost CmdHealWalkerByNurse(DoCommandFlags flags, uint8_t health)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || health == 0 || !WalkerInsured(*c) || c->walker_health == 0 || c->walker_health >= WALKER_HEALTH_MAX) return CMD_ERROR;
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_health = static_cast<uint8_t>(std::min<uint>(WALKER_HEALTH_MAX, c->walker_health + std::min(health, WALKER_NURSE_HEAL)));
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * A thing the walker picks up where it is (the player, 18. 10.: "the mushrooms will be picked about the clay towns"):
 * one more of it in the inventory. Only the mushrooms are picked so.
 * @param flags operation to perform
 * @param item the thing
 * @return nothing, or an error
 */
CommandCost CmdPickWalkerItem(DoCommandFlags flags, WalkerItem item)
{
	if (item != WalkerItem::Mushrooms) return CMD_ERROR;
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

/** The tents the walkers pitched (WalkerTent), saved with the game. */
std::vector<WalkerTent> _walker_tents;

/** Where the things of a tent lie, metres in its frame: +x out of its entrance, from the middle of its floor. */
struct WalkerTentLayout {
	std::string_view figure; ///< the tent's figure of the indices (the colleague's objekty/doplnky)
	float door;              ///< its entrance ("dvere" "poloha"), along x
	float reach;             ///< as far as its cloth goes from the middle (its awning), metres
	float fire[2];           ///< the fireplace: out before the entrance and aside (the player: "the fireplace farther from the entrance")
	float barrel[2];         ///< the shared barrel, inside at the back
};

static const WalkerTentLayout &TentLayout(uint8_t item)
{
	/* The Celtic tent's long side is its entrance under its awning (3.09 m out); the camping tent a dome 2.5 m about. */
	static constexpr WalkerTentLayout CELTIC{"stan_kelt", 1.79f, 3.3f, {5.4f, 1.7f}, {-1.15f, 1.2f}};
	static constexpr WalkerTentLayout CAMPING{"stan_kempink", 2.52f, 2.7f, {6.0f, 1.7f}, {-1.3f, 1.15f}};
	return item == to_underlying(WalkerItem::CampingTent) ? CAMPING : CELTIC;
}

/** The figures of the fireplace, the grill's trestles and its spit, and the barrel (objekty/nase, objekty/doplnky). */
static constexpr std::string_view TENT_FIRE = "ohniste_stanu";
static constexpr std::string_view TENT_TRESTLES = "kozy_grilu";
static constexpr std::string_view TENT_SPIT = "rozen";
static constexpr std::string_view TENT_BARREL = "sud_s_pipou_99914b93";
/** The spit over the fire as over the forest's (ReadForestPart()): its rod so high in the forks, sized so. */
static constexpr float TENT_SPIT_UP_M = 1.01f, TENT_SPIT_SCALE = 1.08f;
/** How near the walker the fireplace and the barrel keep it, metres. */
static constexpr double TENT_FIRE_R_M = 0.75, TENT_BARREL_R_M = 0.4;

/** A point of a tent's frame (metres) in the world (world units). */
static void TentToWorld(const WalkerTent &t, double lx, double ly, double &x, double &y)
{
	const double a = t.facing * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
	x = (TileX(TileIndex{t.tile}) + 0.5) * TILE_SIZE + (lx * c - ly * s) * OBJECT_UNITS_PER_METRE;
	y = (TileY(TileIndex{t.tile}) + 0.5) * TILE_SIZE + (lx * s + ly * c) * OBJECT_UNITS_PER_METRE;
}

/** A world point (world units) in a tent's frame (metres). */
static void WorldToTent(const WalkerTent &t, double x, double y, double &lx, double &ly)
{
	const double a = t.facing * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
	const double dx = (x - (TileX(TileIndex{t.tile}) + 0.5) * TILE_SIZE) / OBJECT_UNITS_PER_METRE;
	const double dy = (y - (TileY(TileIndex{t.tile}) + 0.5) * TILE_SIZE) / OBJECT_UNITS_PER_METRE;
	lx = dx * c + dy * s;
	ly = -dx * s + dy * c;
}

/** The tent pitched on a tile, nullptr none. */
const WalkerTent *WalkerTentAt(TileIndex tile)
{
	for (const WalkerTent &t : _walker_tents) {
		if (t.tile == tile.base()) return &t;
	}
	return nullptr;
}

/** The tent whose entrance is nearest a world point within reach of it (metres), nullptr none. */
const WalkerTent *WalkerTentEntranceNear(double x, double y, double reach_m)
{
	const WalkerTent *best = nullptr;
	double best_d = reach_m * OBJECT_UNITS_PER_METRE;
	for (const WalkerTent &t : _walker_tents) {
		double dx, dy;
		TentToWorld(t, TentLayout(t.item).door, 0, dx, dy);
		const double d = std::hypot(dx - x, dy - y);
		if (d <= best_d) {
			best_d = d;
			best = &t;
		}
	}
	return best;
}

/** Where a tent's fireplace is, world units; false no tent on the tile. */
bool WalkerTentFireAt(TileIndex tile, double &x, double &y)
{
	const WalkerTent *t = WalkerTentAt(tile);
	if (t == nullptr) return false;
	const WalkerTentLayout &l = TentLayout(t->item);
	TentToWorld(*t, l.fire[0], l.fire[1], x, y);
	return true;
}

/** The tile of the tent whose fireplace is nearest a world point within reach of it (world units), INVALID_TILE none. */
TileIndex WalkerTentFireNear(double x, double y, double reach)
{
	TileIndex best = INVALID_TILE;
	for (const WalkerTent &t : _walker_tents) {
		double fx, fy;
		WalkerTentFireAt(TileIndex{t.tile}, fx, fy);
		const double d = std::hypot(fx - x, fy - y);
		if (d < reach) {
			reach = d;
			best = TileIndex{t.tile};
		}
	}
	return best;
}

/**
 * The parts of a pitched tent as the 3D views draw them: the tent in the colour of its cloth (the first of its palette's
 * shades, ThreeDVehicleObject::PaletteState()), the barrel in it, the fireplace before it, and with an animal chosen to
 * grill (ThreeDForestGrill()) the trestles and the spit over the fire, as over the forest's.
 */
void ForWalkerTentParts(const WalkerTent &t, const std::function<void(const WalkerTentPart &)> &f)
{
	const WalkerTentLayout &l = TentLayout(t.item);
	const float facing = static_cast<float>(t.facing);
	int state = 0;
	if (const ThreeDVehicleObject *fig = GetThreeDFigure(l.figure); fig != nullptr && fig->palette_states.second >= 4) {
		state = fig->PaletteState(t.colour % (fig->palette_states.second / 4));
	}
	double x, y;
	TentToWorld(t, 0, 0, x, y);
	f({l.figure, x, y, 0.0f, facing, 1.0f, 0, state});
	TentToWorld(t, l.barrel[0], l.barrel[1], x, y);
	f({TENT_BARREL, x, y, 0.0f, facing, 1.0f, 0, 0});
	TentToWorld(t, l.fire[0], l.fire[1], x, y);
	f({TENT_FIRE, x, y, 0.0f, facing, 1.0f, 0, 0});
	if (const int animal = ThreeDForestGrill(TileIndex{t.tile}); animal >= 0) {
		/* Along the entrance, as one sits by the fire looking at the tent. */
		const float spit = std::fmod(facing + 90.0f, 360.0f);
		f({TENT_TRESTLES, x, y, 0.0f, spit, 1.0f, 0, 0});
		f({TENT_SPIT, x, y, TENT_SPIT_UP_M, spit, TENT_SPIT_SCALE, animal, 0});
	}
}

/** The cells of a tent's footprint (WalkerTentBlocks()): a tenth of a metre across, so many each way from its middle. */
static constexpr double TENT_CELL_M = 0.1;
static constexpr int TENT_CELLS = 80;
/** What of a tent is in the walker's way: its cloth and poles from the knees over the head (metres over its floor). */
static constexpr double TENT_BODY_LO_M = 0.3, TENT_BODY_HI_M = 1.8;

/**
 * The footprint of a tent's cloth and poles as high as the walker's body (TENT_BODY_LO_M to TENT_BODY_HI_M), its
 * triangles cut to that height on cells of TENT_CELL_M from its middle -- the walls of the dome, the Celtic tent's
 * poles; not its floor, not its awning over the entrance. Made the first time; empty while its figure is not read.
 */
static const std::vector<uint8_t> &TentFootprint(uint8_t item)
{
	static std::array<std::vector<uint8_t>, 2> made;
	std::vector<uint8_t> &fp = made[item == to_underlying(WalkerItem::CampingTent) ? 1 : 0];
	if (!fp.empty()) return fp;
	const ThreeDVehicleObject *fig = GetThreeDFigure(TentLayout(item).figure);
	const ThreeDVehicleShape *shape = fig != nullptr ? fig->GetShape() : nullptr;
	if (shape == nullptr) return fp;
	const int n = 2 * TENT_CELLS;
	fp.assign(static_cast<size_t>(n) * n, 0);
	auto cell = [&](double v) { return static_cast<int>(std::floor(v / TENT_CELL_M)) + TENT_CELLS; };
	for (size_t t = 0; t + 2 < shape->tri.size(); t += 3) {
		/* The triangle cut to the body's height: a convex polygon. */
		std::vector<std::array<double, 3>> poly;
		for (int k = 0; k < 3; k++) {
			const float *p = &shape->pos[shape->tri[t + k] * 3];
			poly.push_back({p[0], p[1], p[2]});
		}
		for (int side = 0; side < 2 && !poly.empty(); side++) {
			const double limit = side == 0 ? TENT_BODY_LO_M : TENT_BODY_HI_M;
			auto inside = [&](const std::array<double, 3> &p) { return side == 0 ? p[2] >= limit : p[2] <= limit; };
			std::vector<std::array<double, 3>> cut;
			for (size_t k = 0; k < poly.size(); k++) {
				const auto &a = poly[k], &b = poly[(k + 1) % poly.size()];
				if (inside(a)) cut.push_back(a);
				if (inside(a) != inside(b)) {
					const double w = (limit - a[2]) / (b[2] - a[2]);
					cut.push_back({a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, limit});
				}
			}
			poly = std::move(cut);
		}
		if (poly.empty()) continue;
		/* Its outline, a half cell a step (a wall seen from above is a line), and its inside (a sloping wall). */
		double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
		for (size_t k = 0; k < poly.size(); k++) {
			const auto &a = poly[k], &b = poly[(k + 1) % poly.size()];
			x0 = std::min(x0, a[0]); x1 = std::max(x1, a[0]);
			y0 = std::min(y0, a[1]); y1 = std::max(y1, a[1]);
			const int steps = 1 + static_cast<int>(std::hypot(b[0] - a[0], b[1] - a[1]) / (TENT_CELL_M / 2));
			for (int s = 0; s <= steps; s++) {
				const double w = static_cast<double>(s) / steps;
				const int cx = cell(a[0] + (b[0] - a[0]) * w), cy = cell(a[1] + (b[1] - a[1]) * w);
				if (cx >= 0 && cy >= 0 && cx < n && cy < n) fp[static_cast<size_t>(cy) * n + cx] = 1;
			}
		}
		if (poly.size() < 3) continue;
		for (int cy = std::max(0, cell(y0)); cy <= std::min(n - 1, cell(y1)); cy++) {
			for (int cx = std::max(0, cell(x0)); cx <= std::min(n - 1, cell(x1)); cx++) {
				const double px = (cx - TENT_CELLS + 0.5) * TENT_CELL_M, py = (cy - TENT_CELLS + 0.5) * TENT_CELL_M;
				int sign = 0;
				bool in = true;
				for (size_t k = 0; k < poly.size() && in; k++) {
					const auto &a = poly[k], &b = poly[(k + 1) % poly.size()];
					const double cr = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
					const int sg = cr > 1e-9 ? 1 : (cr < -1e-9 ? -1 : 0);
					if (sg == 0) continue;
					if (sign == 0) sign = sg;
					in = sg == sign;
				}
				if (in) fp[static_cast<size_t>(cy) * n + cx] = 1;
			}
		}
	}
	return fp;
}

/** Whether a tent, its fireplace or its barrel stands at a world point: the walker does not go there (first_person_gui.cpp). */
bool WalkerTentBlocks(double x, double y)
{
	if (_walker_tents.empty() || x < 0 || y < 0) return false;
	const TileIndex tile = TileVirtXY(static_cast<int>(x), static_cast<int>(y));
	if (!IsValidTile(tile)) return false;
	const WalkerTent *t = WalkerTentAt(tile);
	if (t == nullptr) return false;
	const WalkerTentLayout &l = TentLayout(t->item);
	double lx, ly;
	WorldToTent(*t, x, y, lx, ly);
	if (std::hypot(lx - l.fire[0], ly - l.fire[1]) < TENT_FIRE_R_M) return true;
	if (std::hypot(lx - l.barrel[0], ly - l.barrel[1]) < TENT_BARREL_R_M) return true;
	const std::vector<uint8_t> &fp = TentFootprint(t->item);
	if (fp.empty()) return false;
	const int cx = static_cast<int>(std::floor(lx / TENT_CELL_M)) + TENT_CELLS, cy = static_cast<int>(std::floor(ly / TENT_CELL_M)) + TENT_CELLS;
	return cx >= 0 && cy >= 0 && cx < 2 * TENT_CELLS && cy < 2 * TENT_CELLS && fp[static_cast<size_t>(cy) * 2 * TENT_CELLS + cx] != 0;
}

/** Whether a tree of a tile standing at a world point is left out: under a tent or its fireplace (the player: "a tree may be there"). */
bool WalkerTentHides(TileIndex tile, double x, double y)
{
	const WalkerTent *t = _walker_tents.empty() ? nullptr : WalkerTentAt(tile);
	if (t == nullptr) return false;
	const WalkerTentLayout &l = TentLayout(t->item);
	double lx, ly;
	WorldToTent(*t, x, y, lx, ly);
	return std::hypot(lx, ly) < l.reach + 0.8 || std::hypot(lx - l.fire[0], ly - l.fire[1]) < 2.0;
}

/**
 * Why a tent cannot stand on a tile, INVALID_STRING_ID when it can: clear land (grass, rough land, a field, snow, the
 * desert) or trees (the player: "a tree may be there"), level, nothing over it; not a road, a railway, a building, the
 * water, an industry (the player: "the tile must be free, no road, no building"; "not on a slope").
 */
StringID WalkerTentGroundError(TileIndex tile)
{
	if (!IsValidTile(tile) || TileX(tile) == 0 || TileY(tile) == 0 || TileX(tile) >= Map::MaxX() || TileY(tile) >= Map::MaxY()) return STR_ERROR_WALKER_TENT_NOT_FREE;
	if (!IsTileType(tile, TileType::Clear) && !IsTileType(tile, TileType::Trees)) return STR_ERROR_WALKER_TENT_NOT_FREE;
	/* A farm's field is not free land either. */
	if (IsTileType(tile, TileType::Clear) && !IsSnowTile(tile) && GetClearGround(tile) == ClearGround::Fields) return STR_ERROR_WALKER_TENT_NOT_FREE;
	if (IsBridgeAbove(tile)) return STR_ERROR_WALKER_TENT_NOT_FREE;
	if (GetTileSlope(tile) != SLOPE_FLAT) return STR_ERROR_WALKER_TENT_NOT_FLAT;
	return INVALID_STRING_ID;
}

/**
 * Where the walker pitches a tent (the player: "on the tile in front of the walker"): on the tile beside its own the
 * way it looks, of the eight about it, so that the tent in the middle of it never stands on the walker; its entrance
 * looking to the walker.
 * @param x,y the walker, world units
 * @param heading radians it looks to, 0 along +x
 * @return the tile, and the degrees its entrance looks to
 */
std::pair<TileIndex, uint16_t> WalkerTentPlace(double x, double y, double heading)
{
	const double eighth = std::round(heading / (M_PI / 4)) * (M_PI / 4);
	const int dx = static_cast<int>(std::lround(std::cos(eighth))), dy = static_cast<int>(std::lround(std::sin(eighth)));
	const int tx = static_cast<int>(std::floor(x / TILE_SIZE)) + dx, ty = static_cast<int>(std::floor(y / TILE_SIZE)) + dy;
	if (tx < 0 || ty < 0 || tx >= static_cast<int>(Map::SizeX()) || ty >= static_cast<int>(Map::SizeY())) return {INVALID_TILE, 0};
	const double cx = (tx + 0.5) * TILE_SIZE, cy = (ty + 0.5) * TILE_SIZE;
	double facing = std::atan2(y - cy, x - cx) * 180.0 / M_PI;
	if (facing < 0) facing += 360.0;
	return {TileXY(tx, ty), static_cast<uint16_t>(static_cast<int>(std::lround(facing)) % 360)};
}

/** The map about a tent drawn again: it was pitched, folded, its grill changed. */
static void MarkWalkerTentDirty(const WalkerTent &t)
{
	const TileIndex tile{t.tile};
	if (IsValidTile(tile)) MarkTileDirtyByTile(tile);
}

/**
 * Pitch a tent of the walker's inventory on a tile (WalkerTent): in its middle, its entrance looking so, its cloth a
 * colour of its palette.
 * @param flags operation to perform
 * @param tile where
 * @param item which tent (WalkerItem::CelticTent, CampingTent)
 * @param facing degrees in the world its entrance looks to
 * @param colour the colour of its cloth, of its palette (the walker's computer chose it by chance)
 * @return nothing, or an error
 */
CommandCost CmdPitchWalkerTent(DoCommandFlags flags, TileIndex tile, WalkerItem item, uint16_t facing, uint8_t colour)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr || !IsWalkerTentItem(item) || facing >= 360) return CMD_ERROR;
	if (c->walker_items[to_underlying(item)] == 0) return CommandCost(STR_ERROR_WALKER_HAS_NONE);
	if (const StringID why = WalkerTentGroundError(tile); why != INVALID_STRING_ID) return CommandCost(why);
	if (WalkerTentAt(tile) != nullptr) return CommandCost(STR_ERROR_WALKER_TENT_ALREADY);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[to_underlying(item)]--;
		WalkerTent &t = _walker_tents.emplace_back();
		t.tile = tile.base();
		t.facing = facing;
		t.item = to_underlying(item);
		t.colour = colour;
		t.owner = c->index.base();
		MarkWalkerTentDirty(t);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * Fold the walker's tent on a tile back into its inventory, the fire put out, the barrel with it.
 * @param flags operation to perform
 * @param tile where it stands
 * @return nothing, or an error
 */
CommandCost CmdStrikeWalkerTent(DoCommandFlags flags, TileIndex tile)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;
	auto it = std::ranges::find_if(_walker_tents, [&](const WalkerTent &t) { return t.tile == tile.base(); });
	if (it == _walker_tents.end() || it->owner != c->index.base()) return CMD_ERROR;
	if (c->walker_items[it->item] >= WALKER_ITEM_MAX) return CommandCost(STR_ERROR_WALKER_CARRIES_TOO_MANY);
	if (flags.Test(DoCommandFlag::Execute)) {
		c->walker_items[it->item]++;
		SetThreeDForestGrill(tile, -1);
		MarkWalkerTentDirty(*it);
		_walker_tents.erase(it);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
		InvalidateWindowData(WindowClass::WalkerInventory, 0);
	}
	return CommandCost();
}

/**
 * The tents whose ground is gone (the player, 7. 11.: "the news that Givmi "the player's name" was evicted with his
 * tent from a building plot, when I build a road over the tent; he loses the tent of the inventory if he does not
 * fold it with E, when they evict him"): something built over one (a road, a railway, a house), its tile dug to a
 * slope or flooded -- it is gone, not back in the inventory, and the news says so; a tent of a company that is no more
 * goes quietly. The same on every computer: the game's state, in the game's ticks.
 */
static const IntervalTimer<TimerGameTick> _walker_tents_evicted({TimerGameTick::Priority::None, 16}, [](auto) {
	for (auto it = _walker_tents.begin(); it != _walker_tents.end();) {
		const TileIndex tile{it->tile};
		const Company *c = Company::GetIfValid(CompanyID(it->owner));
		const bool gone = c == nullptr || WalkerTentGroundError(tile) != INVALID_STRING_ID;
		if (!gone) {
			++it;
			continue;
		}
		if (c != nullptr) {
			AddNewsItem(GetEncodedString(STR_NEWS_WALKER_TENT_EVICTED, c->index), NewsType::General, NewsStyle::Normal, {}, tile);
		}
		SetThreeDForestGrill(tile, -1);
		MarkWalkerTentDirty(*it);
		it = _walker_tents.erase(it);
		InvalidateWindowData(WindowClass::FirstPerson, 0);
	}
});
