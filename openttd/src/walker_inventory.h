/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file walker_inventory.h The inventory of the walker of the view from the first person (first_person_gui.cpp).
 *
 * The player (15. 10.), after Morrowind: "a normal inventory: armour, boots, he will have beer, and cigarettes and
 * marijuana, we carry those, and he buys goods", "the big hammer, I went through the whole of Morrowind with a big
 * hammer", "we will add more". It is the company's (CompanyProperties::walker_items), saved with the game, and paid
 * for with the company's money: beer at the pub, marijuana at the vending machine and the coffeeshop, cigarettes and
 * goods at a shop of a town (a house that takes goods). Beer heals, marijuana heals more; the rest is carried for now
 * (dressing and spells later). The hammer is there from the start, and the bow and the crossbow with fifty arrows and
 * fifty bolts (the player, 17. 10.: "put the bow, arrows, crossbow and bolts there"; WalkerStartItems()), and a torch and a
 * lantern for the left hand (18. 10.: "take the torch too"), sold at the shops too.
 */

#ifndef WALKER_INVENTORY_H
#define WALKER_INVENTORY_H

#include "economy_type.h"
#include "strings_type.h"
#include "tile_type.h"

#include <array>

/** The things the walker can carry. Their numbers are saved: new ones go at the end. */
enum class WalkerItem : uint8_t {
	Hammer,     ///< the big hammer, from the start
	Armour,     ///< armour (to put on later)
	Boots,      ///< boots (to put on later)
	Beer,       ///< beer: heals
	Cigarettes, ///< cigarettes
	Marijuana,  ///< marijuana: heals more
	Goods,      ///< goods bought in a town
	Bow,        ///< the bow (the colleague's, objekty/predmety/luk.glb): drawn and shot; from the start
	Arrows,     ///< arrows for the bow, used up as they are shot; fifty from the start
	Crossbow,   ///< the crossbow: shot, then drawn again; from the start
	Bolts,      ///< bolts for the crossbow; fifty from the start
	Sword,      ///< the sword, swung as the hammer
	TwoHandedSword, ///< the two-handed sword, swung as the hammer
	Torch,      ///< a torch (the colleague's, objekty/predmety/pochoden.glb): in the left hand, it lights about the walker at night
	Lantern,    ///< a lantern (Roblox Lantern, objekty/predmety/lucerna.glb): so too
	MachineGun, ///< the machine gun (objekty/predmety/kulomet.glb): fires bursts while the button is held
	Bullets,    ///< its bullets, used up as they are shot
	RocketLauncher, ///< the rocket launcher (objekty/predmety/raketomet.glb): a rocket, then loaded again
	Rockets,    ///< its rockets, exploding where they hit
	Mushrooms,  ///< mushrooms picked about the clay towns (the colleague's objekty/houby, E by one)
	End,        ///< the end of the known things
};

/** What kind of thing it is, for the cheat (CmdWalkerCheat()): a weapon is given once, ammunition filled up. */
enum class WalkerItemKind : uint8_t {
	Thing,      ///< carried, bought, used
	Weapon,     ///< a weapon: one is enough
	Ammunition, ///< shot by a weapon, used up
	Light,      ///< a light in the left hand (the torch, the lantern): one is enough
};

/** The places of the inventory kept and saved: room for the things to come. */
static constexpr size_t WALKER_ITEM_SLOTS = 32;
/** The most of one thing carried. */
static constexpr uint16_t WALKER_ITEM_MAX = 999;
/** The arrows and the bolts the cheat fills up to. */
static constexpr uint16_t WALKER_AMMUNITION_FULL = 50;
/** The bullets of the machine gun the cheat fills up to: ten bursts of a second's. */
static constexpr uint16_t WALKER_BULLETS_FULL = 300;
/** The rockets the cheat fills up to. */
static constexpr uint16_t WALKER_ROCKETS_FULL = 10;
/** The walker's full health. */
static constexpr uint8_t WALKER_HEALTH_MAX = 100;

/** What a new company's walker carries (CompanyProperties::walker_items): the hammer, the bow and the crossbow, a quiver of each. */
constexpr std::array<uint16_t, WALKER_ITEM_SLOTS> WalkerStartItems()
{
	std::array<uint16_t, WALKER_ITEM_SLOTS> items{};
	items[static_cast<size_t>(WalkerItem::Hammer)] = 1;
	items[static_cast<size_t>(WalkerItem::Bow)] = 1;
	items[static_cast<size_t>(WalkerItem::Arrows)] = WALKER_AMMUNITION_FULL;
	items[static_cast<size_t>(WalkerItem::Crossbow)] = 1;
	items[static_cast<size_t>(WalkerItem::Bolts)] = WALKER_AMMUNITION_FULL;
	items[static_cast<size_t>(WalkerItem::Torch)] = 1;
	items[static_cast<size_t>(WalkerItem::Lantern)] = 1;
	items[static_cast<size_t>(WalkerItem::MachineGun)] = 1;
	items[static_cast<size_t>(WalkerItem::Bullets)] = WALKER_BULLETS_FULL;
	items[static_cast<size_t>(WalkerItem::RocketLauncher)] = 1;
	items[static_cast<size_t>(WalkerItem::Rockets)] = WALKER_ROCKETS_FULL;
	return items;
}

/** What a thing of the inventory is. */
struct WalkerItemSpec {
	StringID name;    ///< its name
	int64_t price;    ///< what it costs, base currency before inflation; 0 not sold
	uint8_t heal;     ///< the health it gives used, 0 not usable
	WalkerItemKind kind; ///< a weapon, ammunition or a thing
};

const WalkerItemSpec &GetWalkerItemSpec(WalkerItem item);
bool IsWalkerItemSoldAt(WalkerItem item, TileIndex tile);
Money WalkerItemPrice(WalkerItem item);
StringID WalkerShopName(TileIndex tile);
uint16_t WalkerCheatCount(WalkerItem item, uint16_t carried, uint16_t quiver);

#endif /* WALKER_INVENTORY_H */
