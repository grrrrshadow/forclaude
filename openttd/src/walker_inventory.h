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

#include "cargo_type.h"
#include "economy_type.h"
#include "strings_type.h"
#include "tile_type.h"

#include <array>
#include <vector>

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
	Seedling,   ///< a seedling of marijuana in a pot, Matylda's at the pub: planted in front of the walker (WalkerPlant)
	Hay,        ///< marijuana hay, a plant harvested too late: for the dragons (the player, 21. 10.: "a dragon farm")
	WarHammer,  ///< the war hammer (the colleague's objekty/predmety/kladivo_valecne.glb, par14), swung as the hammer; from iddqd
	Pitchfork,  ///< the pitchfork (the colleague's objekty/predmety/vidle.glb, par15), struck as the hammer; from iddqd
	PaperLantern, ///< the paper lantern of Morrowind (the colleague's objekty/predmety/lucerna_papirova.glb): a light, hung from the left hand
	IronLantern,  ///< the wrought candle lantern (the colleague's objekty/predmety/lucerna_kovana.glb): a light
	MushroomGoulash, ///< mushroom goulash of the pub, while mushrooms come to it (PubCooksGoulash()): heals
	End,        ///< the end of the known things
};

/** What kind of thing it is, for the cheat (CmdWalkerCheat()): a weapon is given once, ammunition filled up. */
enum class WalkerItemKind : uint8_t {
	Thing,      ///< carried, bought, used
	Weapon,     ///< a weapon: one is enough
	Ammunition, ///< shot by a weapon, used up
	Light,      ///< a light in the left hand (the torch, the lantern): one is enough
};

/** The tile a thing is bought "at" from Mat walking about the town (CmdBuyWalkerItem()): the map's corner, no shop ever, and a tile the commands take. */
static constexpr TileIndex WALKER_SOLD_BY_MAT{0};

/** The places of the inventory kept and saved: room for the things to come. */
static constexpr size_t WALKER_ITEM_SLOTS = 32;
/**
 * The quick keys, 1 to 9 (the player, 23. 10.: "the numbers of the weapons are not needed, the player assigns them as in
 * Morrowind, what is under 1 and under 2"): each the thing the player put under it in the inventory, End none.
 */
static constexpr size_t WALKER_QUICK_KEYS = 9;

/** The quick keys as they start: none under any (the player puts things under them). */
constexpr std::array<uint8_t, WALKER_QUICK_KEYS> WalkerNoQuickKeys()
{
	std::array<uint8_t, WALKER_QUICK_KEYS> keys{};
	keys.fill(static_cast<uint8_t>(WalkerItem::End));
	return keys;
}

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
	items[static_cast<size_t>(WalkerItem::PaperLantern)] = 1;
	items[static_cast<size_t>(WalkerItem::IronLantern)] = 1;
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
	CargoLabel cargo{};  ///< the cargo of the game it is too (the player, 21. 10.: "use the cargo codes in the inventory if it is a cargo"; not shown), none 0
};

const WalkerItemSpec &GetWalkerItemSpec(WalkerItem item);
bool IsWalkerItemSoldAt(WalkerItem item, TileIndex tile);
Money WalkerItemPrice(WalkerItem item);
StringID WalkerShopName(TileIndex tile);
uint16_t WalkerCheatCount(WalkerItem item, uint16_t carried, uint16_t quiver);

/**
 * A marijuana plant the walker planted (the player, 21. 10.: "Matylda sells the whole plant in a pot ... he buys one
 * and in the inventory he has a seedling to plant, he clicks it and it stands on the ground in front of him in the pot,
 * after a while it is planted without the pot and grows, in half a year E harvests it and he gets marijuana into the
 * inventory, and in a year there will be marijuana hay"). The game's: saved, the same on every computer.
 */
struct WalkerPlant {
	int32_t x = 0, y = 0;     ///< where it grows, world units
	int32_t planted = 0;      ///< the economy's day it was planted (TimerGameEconomy::date)
	uint64_t planted_tick = 0; ///< the game's tick it was planted (TimerGameTick::counter): its first seconds in the pot
	uint8_t owner = 0;        ///< the company whose walker planted it
};

/** How a planted plant is now. */
enum class WalkerPlantStage : uint8_t {
	InPot,   ///< just put down, still in its pot
	Growing, ///< growing in the ground
	Ripe,    ///< half a year old: E harvests the marijuana
	Hay,     ///< a year old, not harvested: dried to hay, E harvests that
};

/** The pot's seconds after planting, ticks of the game. */
static constexpr uint64_t WALKER_PLANT_POT_TICKS = 110;
/** Days from planting to the harvest (half a year), and to hay (a year). */
static constexpr int32_t WALKER_PLANT_RIPE_DAYS = 182;
static constexpr int32_t WALKER_PLANT_HAY_DAYS = 365;
/** The marijuana a ripe plant gives. */
static constexpr uint16_t WALKER_PLANT_HARVEST = 3;
/** The pot of a seedling (our objekty/nase/kvetinac): across its rim and high, metres; the soil in it this high; the plant in it this high. */
static constexpr float WALKER_POT_M = 0.5f;
static constexpr float WALKER_POT_HEIGHT_M = 0.4f;
static constexpr float WALKER_POT_SOIL_M = 0.37f;
static constexpr float WALKER_POT_PLANT_M = 0.9f;
/** How far in front of the walker a seedling is planted, metres. */
static constexpr double WALKER_PLANT_AHEAD_M = 1.2;
/** How near the walker reaches a plant to harvest it, metres. */
static constexpr double WALKER_PLANT_REACH_M = 1.8;

extern std::vector<WalkerPlant> _walker_plants;
WalkerPlantStage GetWalkerPlantStage(const WalkerPlant &p);
int32_t WalkerPlantAgeDays(const WalkerPlant &p);
const WalkerPlant *WalkerPlantNear(double x, double y, double reach_m);

#endif /* WALKER_INVENTORY_H */
