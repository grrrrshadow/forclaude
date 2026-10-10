/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file industry_cmd.cpp Handling of industry tiles. */

#include "stdafx.h"
#include "misc/history_type.hpp"
#include "misc/history_func.hpp"
#include "clear_map.h"
#include "industry.h"
#include "station_base.h"
#include "landscape.h"
#include "viewport_func.h"
#include "command_func.h"
#include "town.h"
#include "news_func.h"
#include "cheat_type.h"
#include "company_base.h"
#include "genworld.h"
#include "tree_map.h"
#include "newgrf_cargo.h"
#include "newgrf_debug.h"
#include "newgrf_industrytiles.h"
#include "autoslope.h"
#include "water.h"
#include "strings_func.h"
#include "window_func.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "animated_tile_func.h"
#include "aircraft.h"
#include "ship.h"
#include "train.h"
#include "roadveh.h"
#include "road_on_rail.h"
#include "airport.h"
#include "effectvehicle_func.h"
#include "effectvehicle_base.h"
#include "ai/ai.hpp"
#include "core/pool_func.hpp"
#include "subsidy_func.h"
#include "core/backup_type.hpp"
#include "object_base.h"
#include "game/game.hpp"
#include "error.h"
#include "string_func.h"
#include "console_func.h"
#include "industry_cmd.h"
#include "climate_industries.h"
#include "landscape_cmd.h"
#include "terraform_cmd.h"
#include "map_func.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "timer/timer_game_tick.h"

#include "table/strings.h"
#include "table/industry_land.h"
#include "table/build_industry.h"

#include "safeguards.h"

/** Whether this player's industry windows show how much of the building is left. */
extern bool _show_train_orientation;

bool _show_industry_health = false;
TimerGameEconomy::Date _industry_health_until{};

/**
 * How much of an industry's building is still standing, as a percentage.
 * @param i the industry
 * @return how much of it is left, 0 to 100
 */
uint GetIndustryHealthPercent(const Industry *i)
{
	return i->health;
}

/**
 * Take a slice off an industry's building, and pull it down if that was the
 * last of it.
 *
 * Pulling it down is the ordinary closure the game already does when an
 * industry's day is over: the industry is deleted, which clears its tiles and
 * closes whatever was looking at it. Nothing is left standing for a building
 * with nothing left of it.
 *
 * @param i    the industry
 * @param hurt how much to take off, in percent
 * @return whether the industry was pulled down (and so no longer exists)
 */
bool DamageIndustry(Industry *i, uint hurt)
{
	if (i->health > hurt) {
		i->health -= hurt;
		SetWindowDirty(WindowClass::IndustryView, i->index);
		return false;
	}

	i->health = 0;
	delete i;
	return true;
}

/** Half the length of a raid's carpet, along the line of flight, in tiles. */
static const int RAID_LENGTH = 3;
/** Half the width of a raid's carpet, across the line of flight, in tiles. */
static const int RAID_WIDTH = 1;
/** Where the carpet stops being solid: past this, only every other tile smokes. */
static const int RAID_SOLID = 1;
/** How much of a building one raid takes off, in percent: four of them pull it down. */
static const uint RAID_HURT = 30;
/** How long the smoke of a raid hangs about: three weeks, in ticks. */
static const uint16_t RAID_SMOKE_LIFE = 21 * Ticks::DAY_TICKS;
/** How long what is left of the buildings stays readable after a raid. */
static const int RAID_HEALTH_SHOWN_DAYS = 14;
/**
 * How long a vehicle caught under the smoke stays broken, in ticks: a week
 * longer than the smoke hangs about, so the wreckage outlasts the sight of
 * it.
 */
static const uint RAID_BREAKDOWN_LIFE = RAID_SMOKE_LIFE + 7 * Ticks::DAY_TICKS;
/**
 * What the game's own countdown is set to meanwhile.
 *
 * It is one byte counting at a step every other tick, so it could never
 * reach a month by itself; RAID_BREAKDOWN_LIFE holds the vehicle down and
 * this only finishes the job afterwards. It has to be something other than
 * zero all the same, or the countdown wraps round on its first step.
 */
static const uint8_t RAID_BREAKDOWN_DELAY = 0xC0;

/**
 * Break down whatever the smoke came down on.
 *
 * Everything on the carpet the moment it lands -- trains, road vehicles,
 * ships, and aircraft on the ground -- gets the breakdown the game itself
 * hands out: due next tick, and the game does the smoke, the sound and the
 * stopping. Whose it is does not come into it; a raid on your own junction
 * stops your own trains.
 *
 * Aircraft in the air are above it and fly on. Nothing hidden -- in a shed,
 * in a hangar, under a hill -- and nothing already stopped, broken or
 * wrecked, since there is nothing left to stop.
 *
 * It is the engine that has to be under the smoke. A train is a row of
 * vehicles each on its own tile, and the wagons do not count: the player
 * said the engine is what breaks, and a carpet across the back of a train
 * leaves it running.
 *
 * @param carpet the tiles the smoke came down on
 * @return how many vehicles were caught
 */
static uint RaidBreakVehicles(const std::vector<TileIndex> &carpet)
{
	std::set<TileIndex> under(carpet.begin(), carpet.end());
	uint caught = 0;
	for (Vehicle *v : Vehicle::Iterate()) {
		switch (v->type) {
			case VehicleType::Train:
			case VehicleType::Road:
			case VehicleType::Ship:
			case VehicleType::Aircraft:
				break;
			default:
				continue;
		}
		if (v->First() != v) continue;
		if (under.count(v->tile) == 0) continue;
		if (v->vehstatus.Any({VehState::Stopped, VehState::Crashed, VehState::Hidden})) continue;
		if (v->type == VehicleType::Aircraft && Aircraft::From(v)->state == FLYING) continue;
		if (v->breakdown_ctr != 0) continue;

		v->breakdown_ctr = 2;
		v->breakdown_delay = RAID_BREAKDOWN_DELAY;
		v->breakdown_chance = 0;
		v->raid_broken_until = TimerGameTick::counter + RAID_BREAKDOWN_LIFE;
		caught++;
	}
	return caught;
}

/**
 * The tiles a raid covers: a carpet laid along the line of flight.
 *
 * Seven tiles long and three wide, lying the way the aircraft was heading.
 * The middle three by three is solid smoke; the two tiles at each end are
 * thinner, every other tile of them, so the carpet fades out rather than
 * ending square.
 *
 * @param tile   where the crosshair was put down
 * @param facing which way the aircraft was heading
 * @return the tiles that smoke, the middle of the carpet first
 */
static std::vector<TileIndex> RaidCarpet(TileIndex tile, Direction facing)
{
	/* The nearest of the four diagonal directions gives the axis to lay the
	 * carpet along; a carpet at 45 degrees is not a carpet the player can
	 * aim. */
	DiagDirection along = DirToDiagDir(facing);
	int step_x = (along == DiagDirection::SW) ? 1 : (along == DiagDirection::NE ? -1 : 0);
	int step_y = (along == DiagDirection::SE) ? 1 : (along == DiagDirection::NW ? -1 : 0);
	/* Across is the other axis. */
	int cross_x = (step_x == 0) ? 1 : 0;
	int cross_y = (step_y == 0) ? 1 : 0;

	int x = TileX(tile);
	int y = TileY(tile);

	std::vector<TileIndex> tiles;
	for (int along_i = -RAID_LENGTH; along_i <= RAID_LENGTH; along_i++) {
		for (int cross_i = -RAID_WIDTH; cross_i <= RAID_WIDTH; cross_i++) {
			/* Thin at the ends: every other tile, so half of them. */
			if (abs(along_i) > RAID_SOLID && ((along_i + cross_i) & 1) != 0) continue;

			int tx = x + along_i * step_x + cross_i * cross_x;
			int ty = y + along_i * step_y + cross_i * cross_y;
			if (tx < 0 || ty < 0 || tx >= (int)Map::SizeX() || ty >= (int)Map::SizeY()) continue;
			tiles.push_back(TileXY(tx, ty));
		}
	}
	return tiles;
}

/**
 * How many houses one raid on a town knocks down.
 *
 * The player's numbers: mostly one, sometimes two, rarely three. Nothing is
 * levelled wholesale -- a raid dents a town, it does not erase it.
 *
 * @return the number of houses to pull down
 */
static uint RaidHousesToLevel()
{
	uint roll = RandomRange(10);
	if (roll < 6) return 1;
	if (roll < 9) return 2;
	return 3;
}

/**
 * How many are said to have died in a raid on a building.
 *
 * A made-up number, as the player asked -- nothing in the game counts the
 * people inside a factory. Only its size follows from who is being counted:
 * a shop floor holds a lot of workers, an office fewer clerks, and a
 * boardroom on a visit is a handful.
 *
 * @param raid which raid on this building this is, counting from one
 * @return the number for the news
 */
static uint RaidVictims(uint raid)
{
	switch (raid) {
		case 1: return 10 + RandomRange(90);   // the shop floor
		case 2: return 5 + RandomRange(45);    // the offices
		default: return 1 + RandomRange(9);    // shareholders on a visit
	}
}

/**
 * Put the raid on a building in the papers.
 *
 * The line names the player who ordered it -- the manager's name, which is
 * what a company has of a person -- and which of them died, which follows
 * from how many raids the building has taken. The last one, the one that
 * pulls it down, says nobody got out.
 *
 * The building's name is written into the message here and now rather than
 * being looked up when the paper is read, because the raid that earns the
 * last message is also the one that deletes the building: by the time the
 * news is on screen there would be nothing left to ask.
 *
 * @param who   the company whose aircraft dropped it
 * @param name  the building's name, as it reads now
 * @param tile  where the news points
 * @param raid  which raid on this building this is, counting from one
 * @param gone  whether this raid pulled it down
 */
static void RaidIndustryNews(Owner who, const std::string &name, TileIndex tile, uint raid, bool gone)
{
	if (!Company::IsValidID(who)) return;

	EncodedString headline = gone
			? GetEncodedString(STR_NEWS_RAID_INDUSTRY_EVERYBODY, static_cast<CompanyID>(who), name)
			: GetEncodedString(STR_NEWS_RAID_INDUSTRY_WORKERS + std::min(raid, 3u) - 1,
					static_cast<CompanyID>(who), name, RaidVictims(raid));
	AddTileNewsItem(std::move(headline), NewsType::Accident, tile);
}

/**
 * Drop the smoke of a raid over a spot and take it out of what stands under
 * it: industry buildings lose a slice of what they are made of, towns lose
 * houses and the people in them.
 *
 * Called when the aircraft gets there, not when the player points at the
 * spot: the errand is a flight, and this is its end.
 *
 * @param tile   where the smoke comes down
 * @param facing which way the aircraft was heading, so the carpet lies along it
 * @param who    the company whose aircraft dropped it, for the papers
 */
void DropRaidSmoke(TileIndex tile, Direction facing, Owner who)
{
	std::vector<TileIndex> carpet = RaidCarpet(tile, facing);

	uint puffs = 0;
	for (TileIndex t : carpet) {
		EffectVehicle *smoke = CreateEffectVehicleAbove(TileX(t) * TILE_SIZE + TILE_SIZE / 2,
				TileY(t) * TILE_SIZE + TILE_SIZE / 2, 0, EV_BREAKDOWN_SMOKE);
		if (smoke == nullptr) continue;
		smoke->animation_state = RAID_SMOKE_LIFE;
		puffs++;
	}

	/* And whatever was standing or driving under it. */
	uint broken = RaidBreakVehicles(carpet);

	/* What the smoke came down on. Each industry is hurt once however many of
	 * its tiles are under the carpet, so a big works is not pulled down faster
	 * than a small one for being big. Collected first and hurt afterwards,
	 * because hurting one can delete it and walking the map over a deleted
	 * industry is how this would go wrong. */
	std::set<IndustryID> caught;
	std::vector<TileIndex> houses;
	for (TileIndex t : carpet) {
		if (IsTileType(t, TileType::Industry)) caught.insert(GetIndustryIndex(t));
		if (IsTileType(t, TileType::House)) houses.push_back(t);
	}

	for (IndustryID id : caught) {
		Industry *i = Industry::GetIfValid(id);
		if (i == nullptr) continue;
		uint before = i->health;
		/* Which raid on this building this is, read off what is left of it:
		 * every raid takes the same slice, so what is missing says how many
		 * have been through. Read before the hit, and kept, because the hit
		 * can delete the building underneath us. */
		uint raid = (100 - before) / RAID_HURT + 1;
		std::string name = i->GetCachedName();
		TileIndex where = i->location.tile;
		bool gone = DamageIndustry(i, RAID_HURT);
		RaidIndustryNews(who, name, where, raid, gone);
		if (_show_train_orientation) {
			IConsolePrint(CC_INFO, "nalet: prumysl {} na ({},{}) {} -> {}, nalet c.{}", id.base(), TileX(tile), TileY(tile),
					before, gone ? "zbouran" : fmt::format("{}", (uint)Industry::Get(id)->health), raid);
		}
	}

	/* And the town. A house that comes down takes its people with it (that is
	 * what clearing one does), and what is left is bare broken ground until
	 * the town builds there again. */
	uint levelled = 0;
	/* What each town lost, so the papers can say it. Counted here rather than
	 * asked for afterwards: pulling a house down is the only moment the number
	 * of people in it is known. */
	std::map<TownID, std::pair<uint, uint>> town_losses; // people, houses
	if (!houses.empty()) {
		uint wanted = RaidHousesToLevel();
		while (levelled < wanted && !houses.empty()) {
			size_t pick = RandomRange((uint)houses.size());
			TileIndex t = houses[pick];
			houses.erase(houses.begin() + pick);
			if (!IsTileType(t, TileType::House)) continue; // a neighbour took it down with it

			Town *town = Town::GetByTile(t);
			uint before_pop = town->cache.population;
			ClearTownHouse(town, t);
			/* Rubble, not a lawn. */
			if (IsTileType(t, TileType::Clear)) MakeClear(t, ClearGround::Rough, 3);
			MarkTileDirtyByTile(t);
			auto &loss = town_losses[town->index];
			loss.first += before_pop - town->cache.population;
			loss.second++;
			levelled++;
		}
	}

	/* One line per town, however many houses of it came down. */
	if (Company::IsValidID(who)) {
		for (const auto &[town_id, loss] : town_losses) {
			AddTileNewsItem(GetEncodedString(STR_NEWS_RAID_TOWN, static_cast<CompanyID>(who), town_id, loss.first, loss.second),
					NewsType::Accident, tile);
		}
	}

	/* The errand is over, so the crosshair is put away and the player has to
	 * ask for it again. What is left of the buildings stays readable for a
	 * fortnight, so the raid can be judged after it. */
	_show_industry_health = false;
	_industry_health_until = TimerGameEconomy::date + RAID_HEALTH_SHOWN_DAYS;
	InvalidateWindowClassesData(WindowClass::IndustryView);
	SetWindowClassesDirty(WindowClass::IndustryView);
	/* And the crosshair row goes out of the vehicle windows with it, rather
	 * than sitting there until something else happens to rebuild them. */
	InvalidateWindowClassesData(WindowClass::VehicleView);
	SetWindowClassesDirty(WindowClass::VehicleView);

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "nalet: na ({},{}) smer {} - {} oblacku, prumyslu {}, domu srovnano {}, porouchano {}",
				TileX(tile), TileY(tile), to_underlying(facing), puffs, (uint)caught.size(), levelled, broken);
	}
}

/** How far the bomb reaches, in tiles each way: everything within is wreckage. */
static const int RAID_BOMB_REACH = 3;

/**
 * Let a bomb go off: explosives from a car aboard a ship or an aircraft, the
 * player's "atom bomb". The game's own big explosion, and within three tiles
 * each way everything is gone at one go -- an industry with any of itself
 * there is pulled down (a factory at one blow, the player's words), every
 * house comes down with its people, and every vehicle standing or driving
 * there is a wreck: trains, road vehicles and aircraft on the ground. Nothing
 * is broken down -- a wreck is past that. Aircraft in the air fly on; ships
 * have no wreck of their own in the game and sail on. The papers write it up
 * as they write up a raid.
 *
 * @param tile where it goes off
 * @param who  the company whose car it was, for the papers
 */
void DropRaidBomb(TileIndex tile, Owner who)
{
	const int cx = TileX(tile);
	const int cy = TileY(tile);
	auto within = [cx, cy](TileIndex t) {
		return std::max(abs((int)TileX(t) - cx), abs((int)TileY(t) - cy)) <= RAID_BOMB_REACH;
	};

	/* The explosion: the big one in the middle and a ring of them round it. */
	CreateEffectVehicleAbove(cx * TILE_SIZE + TILE_SIZE / 2, cy * TILE_SIZE + TILE_SIZE / 2, 0, EV_EXPLOSION_LARGE);
	for (int i = 0; i < 12; i++) {
		int dx = (int)RandomRange(2 * RAID_BOMB_REACH + 1) - RAID_BOMB_REACH;
		int dy = (int)RandomRange(2 * RAID_BOMB_REACH + 1) - RAID_BOMB_REACH;
		int x = Clamp(cx + dx, 0, (int)Map::MaxX());
		int y = Clamp(cy + dy, 0, (int)Map::MaxY());
		CreateEffectVehicleAbove(x * TILE_SIZE + TILE_SIZE / 2, y * TILE_SIZE + TILE_SIZE / 2, 0, EV_EXPLOSION_LARGE);
	}
	if (_settings_client.sound.disaster) SndPlayTileFx(SND_12_EXPLOSION, tile);

	/* The vehicles, collected first: wrecking one changes what iterating the
	 * rest would see. A train with any of itself within is wrecked whole. */
	std::set<Vehicle *> hit;
	for (Vehicle *v : Vehicle::Iterate()) {
		switch (v->type) {
			case VehicleType::Train:
			case VehicleType::Road:
			case VehicleType::Aircraft:
				break;
			default:
				continue;
		}
		if (!within(v->tile)) continue;
		if (v->vehstatus.Any({VehState::Crashed, VehState::Hidden})) continue; // in a shed, aboard something, already gone
		if (v->type == VehicleType::Aircraft && (!Aircraft::From(v)->IsNormalAircraft() || Aircraft::From(v)->state == FLYING)) continue;
		hit.insert(v->First());
	}
	uint wrecked = 0;
	for (Vehicle *v : hit) {
		switch (v->type) {
			case VehicleType::Train:
				if (Train::From(v)->IsWrecked()) continue;
				TrainCrashed(Train::From(v));
				break;
			case VehicleType::Road:
				if (RoadVehicle::From(v)->IsCarried()) continue;
				v->Crash();
				break;
			case VehicleType::Aircraft:
				v->Crash();
				for (Vehicle *u = v; u != nullptr; u = u->Next()) u->cargo.Truncate();
				break;
			default:
				continue;
		}
		wrecked++;
	}

	/* Industries and houses under it, collected before anything is pulled
	 * down under the walk. */
	std::set<IndustryID> caught;
	std::vector<TileIndex> houses;
	for (int y = std::max(cy - RAID_BOMB_REACH, 0); y <= std::min(cy + RAID_BOMB_REACH, (int)Map::MaxY()); y++) {
		for (int x = std::max(cx - RAID_BOMB_REACH, 0); x <= std::min(cx + RAID_BOMB_REACH, (int)Map::MaxX()); x++) {
			TileIndex t = TileXY(x, y);
			if (IsTileType(t, TileType::Industry)) caught.insert(GetIndustryIndex(t));
			if (IsTileType(t, TileType::House)) houses.push_back(t);
		}
	}
	for (IndustryID id : caught) {
		Industry *i = Industry::GetIfValid(id);
		if (i == nullptr) continue;
		std::string name = i->GetCachedName();
		TileIndex where = i->location.tile;
		uint before = i->health;
		uint raid = (100 - before) / RAID_HURT + 1;
		DamageIndustry(i, 100);
		RaidIndustryNews(who, name, where, raid, true);
	}
	std::map<TownID, std::pair<uint, uint>> town_losses; // people, houses
	for (TileIndex t : houses) {
		if (!IsTileType(t, TileType::House)) continue; // came down with a neighbour
		Town *town = Town::GetByTile(t);
		uint before_pop = town->cache.population;
		ClearTownHouse(town, t);
		if (IsTileType(t, TileType::Clear)) MakeClear(t, ClearGround::Rough, 3);
		MarkTileDirtyByTile(t);
		auto &loss = town_losses[town->index];
		loss.first += before_pop - town->cache.population;
		loss.second++;
	}
	if (Company::IsValidID(who)) {
		for (const auto &[town_id, loss] : town_losses) {
			AddTileNewsItem(GetEncodedString(STR_NEWS_RAID_TOWN, static_cast<CompanyID>(who), town_id, loss.first, loss.second),
					NewsType::Accident, tile);
		}
	}

	/* The errand is over, as after smoke (DropRaidSmoke()). */
	_show_industry_health = false;
	_industry_health_until = TimerGameEconomy::date + RAID_HEALTH_SHOWN_DAYS;
	InvalidateWindowClassesData(WindowClass::IndustryView);
	SetWindowClassesDirty(WindowClass::IndustryView);
	InvalidateWindowClassesData(WindowClass::VehicleView);
	SetWindowClassesDirty(WindowClass::VehicleView);

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "atomovka: na ({},{}) - vraku {}, prumyslu zbourano {}, domu srovnano {}",
				cx, cy, wrecked, (uint)caught.size(), (uint)houses.size());
	}
}

/**
 * Drop what an aircraft carries over its target: a bomb for every car of
 * explosives aboard, one after another along its line of flight, two tiles
 * apart -- the player's rule, as many bombs as cars -- and each car's
 * explosives gone with its bomb; or, with none aboard, the smoke.
 *
 * @param carrier the aircraft
 * @param tile    the target
 * @param facing  which way it is flying
 */
void DropRaidPayload(Vehicle *carrier, TileIndex tile, Direction facing)
{
	std::vector<RoadVehicle *> cars = ExplosiveCarsAboard(carrier);
	if (cars.empty()) {
		DropRaidSmoke(tile, facing, carrier->owner);
		return;
	}
	TileIndexDiffC step = TileIndexDiffCByDiagDir(DirToDiagDir(facing));
	int x = TileX(tile);
	int y = TileY(tile);
	for (RoadVehicle *car : cars) {
		SpendExplosives(car);
		DropRaidBomb(TileXY(Clamp(x, 0, (int)Map::MaxX()), Clamp(y, 0, (int)Map::MaxY())), carrier->owner);
		x += 2 * step.x;
		y += 2 * step.y;
	}
}

/**
 * Is anybody already out on an errand?
 *
 * One at a time across the whole fleet, not one of each: while an aircraft is
 * on its way no ship can be sent, and while a ship is on its way no aircraft
 * can. The player's rule.
 *
 * @param except a vehicle to overlook -- the one being given the errand
 * @return whether somebody else is already out
 */
bool IsAnyoneRaiding(const Vehicle *except)
{
	for (const Aircraft *a : Aircraft::Iterate()) {
		if (a != except && a->raid_target != INVALID_TILE) return true;
	}
	for (const Ship *s : Ship::Iterate()) {
		if (s != except && s->raid_target != INVALID_TILE) return true;
	}
	/* A rocket already in the air counts too. The ship that fired it has let
	 * its errand go and would otherwise be offered the crosshair again while
	 * its own rocket is still on its way. */
	for (const EffectVehicle *e : EffectVehicle::Iterate()) {
		if (e->subtype == EV_RAID_ROCKET) return true;
	}
	return false;
}

/**
 * Send an aircraft or a ship to drop smoke on a spot.
 *
 * The player points the crosshair; this hands the vehicle the errand and it
 * makes its own way there (see AircraftRaidController() and
 * ShipRaidController()). One vehicle at a time across the whole fleet.
 *
 * The two have different rules, because they are different jobs. An aircraft
 * has to be empty and on the ground: it flies over and drops what it is
 * carrying, so it must be carrying nothing else. A ship may be any ship at
 * all -- loaded, empty, in a shed or at sea -- because it shoots from the
 * water and goes back to what it was doing.
 *
 * A command rather than something the window does itself, because it changes
 * the world: in a network game the journey, the smoke and the damage have to
 * happen on every machine alike.
 *
 * @param flags  type of operation
 * @param tile   where the crosshair was put down
 * @param veh_id the aircraft or ship to send
 * @return the cost of this operation or an error
 */
CommandCost CmdRaid(DoCommandFlags flags, TileIndex tile, VehicleID veh_id)
{
	Vehicle *v = Vehicle::GetIfValid(veh_id);
	if (v == nullptr || v->First() != v) return CMD_ERROR;
	if (v->type != VehicleType::Aircraft && v->type != VehicleType::Ship) return CMD_ERROR;

	CommandCost ret = CheckOwnership(v->owner);
	if (ret.Failed()) return ret;

	if (tile >= Map::Size()) return CMD_ERROR;

	/* One at a time, aircraft and ships together. */
	if (IsAnyoneRaiding(v)) return CommandCost(STR_ERROR_ANOTHER_AIRCRAFT_IS_ALREADY_OUT);

	if (v->type == VehicleType::Ship) {
		Ship *s = Ship::From(v);

		/* Somewhere to shoot from. Out of the rocket's reach of any water,
		 * there is no errand to give -- better said now than after a ship has
		 * spent a season finding out. */
		extern TileIndex FindRaidWaterForShip(const Ship *v, TileIndex target, uint range);
		TileIndex sail_to = FindRaidWaterForShip(s, tile, RAID_SHIP_REACH);
		if (sail_to == INVALID_TILE) return CommandCost(STR_ERROR_RAID_OUT_OF_REACH);

		if (!flags.Test(DoCommandFlag::Execute)) return CommandCost();

		s->raid_target = tile;
		s->raid_sail_to = sail_to;
		s->raid_return_to = s->dest_tile;
		s->raid_closest = DistanceManhattan(s->tile, sail_to);
		s->raid_stale = 0;
		if (s->vehstatus.Test(VehState::Stopped)) s->vehstatus.Reset(VehState::Stopped);
		s->SetDestTile(sail_to);
		SetWindowDirty(WindowClass::VehicleView, s->index);
		SetWindowClassesDirty(WindowClass::VehicleView);
		/* Marking the windows for repainting is not enough: whether the
		 * crosshair row is in a window at all is worked out when the window
		 * counts its rows, and that only runs when something tells it its
		 * contents changed. Without this the crosshair stayed on offer for
		 * the whole flight and the raid could be given twice. */
		InvalidateWindowClassesData(WindowClass::VehicleView);

		if (_show_train_orientation) {
			IConsolePrint(CC_INFO, "nalet: lod {} poslana na ({},{}), plout na ({},{})", s->unitnumber,
					TileX(tile), TileY(tile), TileX(sail_to), TileY(sail_to));
		}
		return CommandCost();
	}

	Aircraft *a = Aircraft::From(v);
	if (!a->IsNormalAircraft()) return CMD_ERROR;

	/* The player's rule: an empty aircraft, and one that is on the ground.
	 * Empty is asked of the whole aircraft, its mail compartment included --
	 * a plane with something aboard is carrying it somewhere. Unless what it
	 * carries is a car of explosives: then it is loaded with the bomb itself,
	 * the player's word, and goes whatever else is aboard. */
	if (a->state >= TAKEOFF && a->state <= HELIENDLANDING) return CommandCost(STR_ERROR_AIRCRAFT_MUST_BE_ON_THE_GROUND);
	if (!CarriesExplosiveCar(a)) {
		for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
			if (u->cargo.TotalCount() != 0) return CommandCost(STR_ERROR_AIRCRAFT_MUST_BE_EMPTY);
		}
	}

	if (!flags.Test(DoCommandFlag::Execute)) return CommandCost();

	a->raid_target = tile;
	/* Standing still with the brake on would keep it in the shed for ever. */
	if (a->vehstatus.Test(VehState::Stopped)) a->vehstatus.Reset(VehState::Stopped);
	SetWindowDirty(WindowClass::VehicleView, a->index);
	SetWindowClassesDirty(WindowClass::VehicleView);
	/* Marking the windows for repainting is not enough: whether the
	 * crosshair row is in a window at all is worked out when the window
	 * counts its rows, and that only runs when something tells it its
	 * contents changed. Without this the crosshair stayed on offer for
	 * the whole flight and the raid could be given twice. */
	InvalidateWindowClassesData(WindowClass::VehicleView);

	if (_show_train_orientation) {
		IConsolePrint(CC_INFO, "nalet: letadlo {} posláno na ({},{})", a->unitnumber, TileX(tile), TileY(tile));
	}
	return CommandCost();
}

IndustryPool _industry_pool("Industry");
INSTANTIATE_POOL_METHODS(Industry)

void ShowIndustryViewWindow(IndustryID industry);
void BuildOilRig(TileIndex tile);

static uint8_t _industry_sound_ctr;
static TileIndex _industry_sound_tile;

std::array<FlatSet<IndustryID>, NUM_INDUSTRYTYPES> Industry::industries;

IndustrySpec _industry_specs[NUM_INDUSTRYTYPES];
IndustryTileSpec _industry_tile_specs[NUM_INDUSTRYTILES];
IndustryBuildData _industry_builder; ///< In-game manager of industries.

static int WhoCanServiceIndustry(Industry *ind);

/**
 * Put the marijuana plantation (economy.extra_industries) and its tile in
 * their places, the last industry type and the last industry tile: the fruit
 * plantation of the base graphics, in every climate, growing marijuana. Its
 * tile is drawn as the fruit plantation's (the substitute of a tile no set
 * gives graphics to), so a set that puts its own tile in place of the fruit
 * plantation's changes the fruit plantation only; and to a set asking about
 * it the plantation is a fruit plantation.
 */
static void SetupMarijuanaPlantation()
{
	const IndustrySpec &fruit = _origin_industry_specs[IT_FRUIT_PLANTATION];
	const IndustryGfx fruit_tile = fruit.layouts.front().front().gfx;

	IndustryTileSpec &tile = _industry_tile_specs[GFX_MARIJUANA_PLANTATION];
	tile = _origin_industry_tile_specs[fruit_tile];
	tile.grf_prop.subst_id = fruit_tile;

	IndustrySpec &spec = _industry_specs[IT_MARIJUANA_PLANTATION];
	spec = fruit;
	/* The fruit plantation's one shape, PLANTATION_WIDTH tiles along x and
	 * PLANTATION_HEIGHT along y, with the row of the field road left out.
	 * That row stays the player's -- the player's word: the road is drawn
	 * there, and a real road with a stop can be laid under the picture --
	 * and the road, the shed and the girls on it are drawn over it from the
	 * row behind (DrawMarijuanaPlantationTile()). The row has to be flat
	 * open land all the same, and its trees go (CheckIfIndustryIsAllowed(),
	 * DoCreateNewIndustry()). */
	IndustryTileLayout layout;
	for (uint y = 0; y < PLANTATION_HEIGHT; y++) {
		if (y == PLANTATION_ROAD_ROW) continue;
		for (uint x = 0; x < PLANTATION_WIDTH; x++) {
			layout.push_back({TileIndexDiffC{static_cast<int16_t>(x), static_cast<int16_t>(y)}, GFX_MARIJUANA_PLANTATION});
		}
	}
	spec.layouts = {layout};
	spec.produced_cargo_label[0] = CT_MARIJUANA;
	/* And hemp fibre from the same plants, as much again: the player's chain
	 * to explosives starts here (the oil refinery takes it, see
	 * ResolveExtraIndustryCargoes()). */
	spec.produced_cargo_label[1] = CT_HEMP_FIBRE;
	spec.production_rate[1] = spec.production_rate[0];
	std::fill(std::begin(spec.conflicting), std::end(spec.conflicting), IT_INVALID);
	spec.climate_availability = {LandscapeType::Temperate, LandscapeType::Arctic, LandscapeType::Tropic, LandscapeType::Toyland};
	std::fill(std::begin(spec.appear_ingame), std::end(spec.appear_ingame), fruit.appear_ingame[to_underlying(LandscapeType::Tropic)]);
	std::fill(std::begin(spec.appear_creation), std::end(spec.appear_creation), fruit.appear_creation[to_underlying(LandscapeType::Tropic)]);
	spec.map_colour = PixelColour{0x54};
	spec.name = STR_INDUSTRY_NAME_MARIJUANA_PLANTATION;
	spec.grf_prop.subst_id = IT_FRUIT_PLANTATION;
	spec.enabled = true;
}

/**
 * The coffeeshop's tile, stage by stage, as the desert house with the palm
 * tree (house 0x4E) is drawn: foundations, the walls, the house.
 */
static const DrawBuildingsTileStruct _coffeeshop_draw_tile_data[] = {
	{ {{0, 0, 0}, {16, 16, 40}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0x11EC, PAL_NONE }, 0 },
	{ {{0, 0, 0}, {16, 16, 40}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0x11ED, PAL_NONE }, 0 },
	{ {{0, 0, 0}, {16, 16, 40}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0x11ED, PAL_NONE }, 0 },
	{ {{0, 0, 0}, {16, 16, 40}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0x11EE, PAL_NONE }, 0 },
};
static_assert(std::size(_coffeeshop_draw_tile_data) == INDUSTRY_COMPLETED + 1);

/** A tile of the girls' grammar school or the vending machine: bare land while it is built, then the grass with the building sprite on it. */
#define OWN_TILE(building, height) { \
		{ {{0, 0, 0}, {16, 16, 0}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0, PAL_NONE }, 0 }, \
		{ {{0, 0, 0}, {16, 16, 0}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0, PAL_NONE }, 0 }, \
		{ {{0, 0, 0}, {16, 16, 0}, {}}, { SPR_FLAT_BARE_LAND, PAL_NONE }, { 0, PAL_NONE }, 0 }, \
		{ {{0, 0, 0}, {16, 16, height}, {}}, { SPR_FLAT_GRASS_TILE, PAL_NONE }, { building, PAL_NONE }, 0 }, \
	}

/**
 * The tiles of the girls' grammar school, north, west, east and south, and of
 * the vending machine, in the order of their tile numbers from
 * GFX_GYMNASIUM_NORTH down. The school is drawn in vertical strips, one per
 * tile (openttd_gymnazium.py): the south tile, the front one, carries the
 * middle of the building with what of the north tile stands behind it, so the
 * north tile is grass only.
 */
static const DrawBuildingsTileStruct _game_own_draw_tile_data[][INDUSTRY_COMPLETED + 1] = {
	OWN_TILE(0, 0),                     // GFX_GYMNASIUM_NORTH
	OWN_TILE(SPR_GYMNASIUM_WEST, 20),   // GFX_GYMNASIUM_WEST
	OWN_TILE(SPR_GYMNASIUM_EAST, 30),   // GFX_GYMNASIUM_EAST
	OWN_TILE(SPR_GYMNASIUM_SOUTH, 50),  // GFX_GYMNASIUM_SOUTH
	OWN_TILE(SPR_WEED_MACHINE, 20),     // GFX_WEED_MACHINE
	OWN_TILE(SPR_STATUE_STONE, 40),     // GFX_STATUE
	OWN_TILE(SPR_HUT_BACK, 40),         // GFX_HUT_BACK
	OWN_TILE(SPR_HUT_FRONT, 40),        // GFX_HUT_FRONT
};
static_assert(std::size(_game_own_draw_tile_data) == GFX_GYMNASIUM_NORTH - GFX_GAME_OWN_FIRST + 1);

/** A tile of the marijuana plantation: bare land while it is built; finished, it is laid from its pictures by DrawMarijuanaPlantationTile(), nothing from here. */
static const DrawBuildingsTileStruct _marijuana_plantation_draw_tile_data[] = OWN_TILE(0, 30);
#undef OWN_TILE

/**
 * How a tile of the game's own industries that is not drawn as an original
 * tile is drawn: the coffeeshop, the girls' grammar school, the vending
 * machine, the statue and the marijuana plantation.
 * @param gfx the tile
 * @param stage its construction stage
 * @return its drawing, or nullptr for any other tile
 */
static const DrawBuildingsTileStruct *GameOwnIndustryDrawTile(IndustryGfx gfx, uint stage)
{
	if (gfx == GFX_COFFEESHOP) return &_coffeeshop_draw_tile_data[stage];
	if (gfx == GFX_MARIJUANA_PLANTATION) return &_marijuana_plantation_draw_tile_data[stage];
	if (gfx >= GFX_GAME_OWN_FIRST && gfx <= GFX_GYMNASIUM_NORTH) return &_game_own_draw_tile_data[GFX_GYMNASIUM_NORTH - gfx][stage];
	return nullptr;
}

/** For how many days after a delivery the girls are about the school, the machine, the statue or the coffeeshop's hut: the player's word. */
static const int GIRLS_DAYS = 30;

static bool DeliveredLately(const Industry *ind, CargoLabel label);

/**
 * Are there girls about a school, a vending machine, a statue or a
 * plantation? There are for GIRLS_DAYS after the last delivery of what
 * brings them: studentky to the school, the statue and the plantation,
 * marijuana to the machine.
 * @param ind the industry
 * @return whether the picture with the girls is drawn
 */
bool HasGirls(const Industry *ind)
{
	return DeliveredLately(ind, ind->type == IT_WEED_MACHINE ? CT_MARIJUANA : CT_STUDENTKY);
}

/**
 * Did a cargo come to an industry in the last GIRLS_DAYS?
 * @param ind the industry
 * @param label the cargo
 * @return whether it did
 */
static bool DeliveredLately(const Industry *ind, CargoLabel label)
{
	CargoType cargo = GetCargoTypeByLabel(label);
	if (!IsValidCargoType(cargo)) return false;
	auto it = ind->GetCargoAccepted(cargo);
	if (it == std::end(ind->accepted) || it->last_accepted == TimerGameEconomy::Date{}) return false;
	return TimerGameEconomy::date.base() - it->last_accepted.base() < GIRLS_DAYS;
}

/**
 * The girls at the coffeeshop's hut, the player's word: none until studentky
 * come; standing for GIRLS_DAYS after they came; sitting while marijuana came
 * in those days too. Marijuana without studentky changes nothing.
 * @param ind the coffeeshop
 * @return which girls are drawn
 */
HutGirls HutGirlsAt(const Industry *ind)
{
	if (!DeliveredLately(ind, CT_STUDENTKY)) return HutGirls::None;
	return DeliveredLately(ind, CT_MARIJUANA) ? HutGirls::Sitting : HutGirls::Standing;
}

/**
 * Are there fireworks over the coffeeshop's hut? The player's word: explosives
 * light them, but only while the girls sit on the benches -- studentky and
 * marijuana came as well. They go on for GIRLS_DAYS after the explosives, as
 * the girls do after theirs.
 * @param ind the coffeeshop
 * @return whether the fireworks go off
 */
bool HutHasFireworks(const Industry *ind)
{
	return HutGirlsAt(ind) == HutGirls::Sitting && DeliveredLately(ind, CT_EXPLOSIVES);
}

/** How long each part of one firework lasts, in ticks: the launch, the three phases of the ball, and the dark sky before the next. */
static const uint FIREWORK_TICKS[] = {24, 10, 14, 20, 32};
/** How long one firework lasts, in ticks. */
static const uint FIREWORK_CYCLE = 24 + 10 + 14 + 20 + 32;

/**
 * What of the fireworks is in the sky over a hut at a tick. One firework
 * after another: the yellow launch, the ball in three phases, a dark pause.
 * The colour of each phase -- green, blue or red -- is picked anew for every
 * phase of every firework, the player's word, so no two are alike. Only the
 * picture depends on it, never the game: the pick is a hash of the tile, the
 * firework's number and the phase, not the game's random numbers, and every
 * hut has its fireworks at its own time.
 * @param tile the hut's front tile
 * @param tick the tick
 * @return the sprite, or 0 for the dark sky between two fireworks
 */
SpriteID HutFireworkSprite(TileIndex tile, uint64_t tick)
{
	uint64_t t = tick + tile.base() * 37ULL;
	uint64_t firework = t / FIREWORK_CYCLE;
	uint at = static_cast<uint>(t % FIREWORK_CYCLE);
	uint part = 0;
	while (at >= FIREWORK_TICKS[part]) at -= FIREWORK_TICKS[part++];
	if (part == 0) return SPR_FIREWORK_START;
	if (part == 4) return 0;
	uint32_t h = static_cast<uint32_t>(tile.base()) * 2654435761U ^ static_cast<uint32_t>(firework) * 40503U ^ part * 0x9E3779B9U ^ static_cast<uint32_t>(firework >> 32);
	h ^= h >> 15;
	h *= 0x2C1B3C6DU;
	h ^= h >> 12;
	uint colour = h % 3;
	return SPR_FIREWORK_BALL + colour * 3 + (part - 1);
}

/**
 * How many days the girls have to tend the plantation before the plants
 * stand grown: a little under the GIRLS_DAYS one delivery keeps them about,
 * so that one delivery sees the field through to grown.
 */
static const uint PLANTATION_GROWN_AFTER = 28;
/** How many days of care the plantation remembers at most. */
static const uint PLANTATION_CARE_MAX = 60;
/** How long a plantation the girls stopped coming to keeps its plants and its yield: half a year, the player's word. */
static const uint PLANTATION_WITHER_DAYS = 180;

/**
 * What stands on a marijuana plantation. The player's word: nothing grows
 * until the girls come; once they come the plants start, and are grown after
 * PLANTATION_GROWN_AFTER days of their care; the girls themselves are about
 * for GIRLS_DAYS after a delivery, as at the school (HasGirls()); and when
 * they have not come for PLANTATION_WITHER_DAYS the field is bare again and
 * yields nothing (TendMarijuanaPlantations()).
 * @param ind the plantation
 * @return what is on it
 */
PlantationStage MarijuanaPlantationStage(const Industry *ind)
{
	if (ind->plantation_care == 0) return PlantationStage::Bare;
	return ind->plantation_care < PLANTATION_GROWN_AFTER ? PlantationStage::Small : PlantationStage::Grown;
}

/**
 * A day on the marijuana plantations: a day with girls about is a day of
 * care and puts off the withering; a day without brings it a day nearer, and
 * on the last one the field is bare.
 */
static void TendMarijuanaPlantations()
{
	for (IndustryID id : Industry::industries[IT_MARIJUANA_PLANTATION]) {
		Industry *i = Industry::Get(id);
		if (HasGirls(i)) {
			i->plantation_care = static_cast<uint8_t>(std::min<uint>(i->plantation_care + 1, PLANTATION_CARE_MAX));
			i->plantation_days_left = PLANTATION_WITHER_DAYS;
		} else if (i->plantation_days_left > 0 && --i->plantation_days_left == 0) {
			i->plantation_care = 0;
		}
		/* The plants and the girls change with the day; drawn afresh, cheaply. */
		MarkGameOwnIndustryDirty(i);
	}
}

/**
 * Plantations of a game saved before the girls tended the field (afterload):
 * they stand grown as they did, and stay so for the half year the girls have
 * to start coming in.
 */
void GrantPlantationsOfOldGames()
{
	for (IndustryID id : Industry::industries[IT_MARIJUANA_PLANTATION]) {
		Industry *i = Industry::Get(id);
		i->plantation_care = PLANTATION_GROWN_AFTER;
		i->plantation_days_left = PLANTATION_WITHER_DAYS;
	}
}

/**
 * The sprite of the plants on a plantation's tile, or of the soil when there
 * are none: what the rig reads as the tile's building sprite.
 * @param ind the plantation
 * @return the sprite
 */
static SpriteID MarijuanaPlantationSprite(const Industry *ind)
{
	switch (MarijuanaPlantationStage(ind)) {
		case PlantationStage::Small: return SPR_MARIJUANA_PLANTS_SMALL;
		case PlantationStage::Grown: return SPR_MARIJUANA_PLANTS_GROWN;
		default: return SPR_MARIJUANA_SOIL;
	}
}

/**
 * Which of the six girls at work is on a field tile of the plantation, by
 * the tile's place in it: ten studentky on the plantation with the four by
 * the road, the player's count, each at her own plant.
 * @param x the tile's place along x
 * @param y the tile's place along y
 * @return the girl, 0 to PLANTATION_WORKER_COUNT - 1, or -1 for no girl on this tile
 */
static int PlantationWorkerAt(uint x, uint y)
{
	static const int8_t workers[PLANTATION_HEIGHT][PLANTATION_WIDTH] = {
		{-1,  0, -1, -1,  1},
		{ 2, -1, -1,  3, -1},
		{-1, -1, -1, -1, -1}, // the road
		{-1,  4, -1, -1,  5},
	};
	if (x >= PLANTATION_WIDTH || y >= PLANTATION_HEIGHT) return -1;
	return workers[y][x];
}

/**
 * Is a tile the field road of a marijuana plantation: the row the industry
 * leaves to the player, with the road picture drawn over it? Known by the
 * plantation tile behind it. Trees do not grow on it (CanPlantTreesOnTile()):
 * they would stand in the road.
 * @param tile the tile
 * @return whether it is
 */
bool IsMarijuanaPlantationRoad(TileIndex tile)
{
	if (TileY(tile) < PLANTATION_ROAD_ROW) return false;
	TileIndex behind = TileAddXY(tile, 0, -1);
	if (!IsTileType(behind, TileType::Industry) || GetIndustryGfx(behind) != GFX_MARIJUANA_PLANTATION) return false;
	const Industry *ind = Industry::GetByTile(behind);
	return TileY(tile) == TileY(ind->location.tile) + PLANTATION_ROAD_ROW && TileX(tile) - TileX(ind->location.tile) < PLANTATION_WIDTH;
}

/** On which tile of the field road, counted along x from its north-east end where the shed is, the girls by its north-west edge stand, and those by its south-east edge. */
static const uint PLANTATION_ROAD_GIRLS_NW_X = 3;
static const uint PLANTATION_ROAD_GIRLS_SE_X = 1;

/**
 * Lay a finished tile of the marijuana plantation from its pictures, after
 * the ground: the soil, the plants on it as its child, a girl at work as the
 * next; and from the row behind the field road, the road over the player's
 * tile a tile further on, and on it the shed, the haystack and the girls by
 * the road.
 *
 * Where each goes in the sorting is the colleague's working-out (grrrrf
 * hra/zarovnani-budov/ODPOVED-PRO-HRU.md, 3 and 4 October), after the
 * player saw cars vanish under the road and the girls under a stop:
 *
 * - the road is held by the field tile behind it, as ISR/DWE Objects II hold
 *   their road-overlapping tile: a box over both tiles with no height
 *   (zmax below zmin), which the sorter always puts behind whatever stands
 *   on the road -- the player's cars, a stop -- while the picture is drawn a
 *   tile on, where the road is. A box of height 1 on the road tile met the
 *   cars' boxes and was sorted by the sum of its corners: a car going
 *   south-west went under the road on entering the tile, and one going
 *   north-east lost its tail to the road of the tile it was leaving;
 * - the girls by the road stand where the player's stop may stand, so each
 *   pair is a sprite of its own with a box of its own, the picture where the
 *   road is: the north-west pair after the stop's far half (y 0 to 2) and
 *   before the cars (y 4 on), the south-east pair after the stop's near half
 *   (y 13 to 15) and the cars, before the field in front. As children of the
 *   road they went under the stop;
 * - the shed and the haystack at the road's end hide the halves of a stop
 *   there (the player's wish), so they are drawn after them the same way:
 *   the shed with the north-west box, the haystack with the south-east one.
 *
 * The soil's children follow the soil whatever else is drawn, so the plants
 * never come apart from their tile.
 * @param ti the tile, after the ground was drawn
 * @param ind the plantation
 */
static void DrawMarijuanaPlantationTile(const TileInfo *ti, const Industry *ind)
{
	static const SpriteBounds FIELD_BOUNDS{{0, 0, 0}, {TILE_SIZE, TILE_SIZE, 30}, {}};
	/* From the field tile holding the road: over it and the road tile, no height, the picture a tile on. */
	static const SpriteBounds ROAD_BOUNDS{{0, 0, 0}, {TILE_SIZE, 2 * TILE_SIZE, 0}, {0, TILE_SIZE, 0}};
	/* From the road tile: a thin box by its north-west or south-east edge, the picture back at the tile's corner. */
	static const SpriteBounds BY_NW_EDGE_BOUNDS{{0, 3, 0}, {TILE_SIZE, 1, 16}, {0, -3, 0}};
	static const SpriteBounds BY_SE_EDGE_BOUNDS{{0, TILE_SIZE, 0}, {TILE_SIZE, 1, 16}, {0, -static_cast<int8_t>(TILE_SIZE), 0}};
	bool transparent = IsTransparencySet(TransparencyOption::Industries);
	uint x = TileX(ti->tile) - TileX(ind->location.tile);
	uint y = TileY(ti->tile) - TileY(ind->location.tile);
	PlantationStage stage = MarijuanaPlantationStage(ind);
	bool girls = HasGirls(ind);

	AddSortableSpriteToDraw(SPR_MARIJUANA_SOIL, PAL_NONE, ti->x, ti->y, ti->z, FIELD_BOUNDS, transparent);
	if (stage != PlantationStage::Bare) {
		bool grown = stage == PlantationStage::Grown;
		AddChildSpriteScreen(grown ? SPR_MARIJUANA_PLANTS_GROWN : SPR_MARIJUANA_PLANTS_SMALL, PAL_NONE, 0, 0, transparent, nullptr, false, false);
		int worker = PlantationWorkerAt(x, y);
		if (girls && worker >= 0) {
			AddChildSpriteScreen((grown ? SPR_MARIJUANA_WORKER_GROWN : SPR_MARIJUANA_WORKER_SMALL) + worker, PAL_NONE, 0, 0, transparent, nullptr, false, false);
		}
	}

	if (y + 1 == PLANTATION_ROAD_ROW && x < PLANTATION_WIDTH) {
		int road_z = GetTilePixelZ(TileAddXY(ti->tile, 0, 1));
		int road_y = ti->y + TILE_SIZE;
		AddSortableSpriteToDraw(SPR_MARIJUANA_ROAD, PAL_NONE, ti->x, ti->y, road_z, ROAD_BOUNDS, transparent);
		if (x == 0) {
			AddSortableSpriteToDraw(SPR_MARIJUANA_SHED, PAL_NONE, ti->x, road_y, road_z, BY_NW_EDGE_BOUNDS, transparent);
			AddSortableSpriteToDraw(SPR_MARIJUANA_HAY, PAL_NONE, ti->x, road_y, road_z, BY_SE_EDGE_BOUNDS, transparent);
		}
		if (girls && x == PLANTATION_ROAD_GIRLS_NW_X) AddSortableSpriteToDraw(SPR_MARIJUANA_ROAD_GIRLS_NW, PAL_NONE, ti->x, road_y, road_z, BY_NW_EDGE_BOUNDS, transparent);
		if (girls && x == PLANTATION_ROAD_GIRLS_SE_X) AddSortableSpriteToDraw(SPR_MARIJUANA_ROAD_GIRLS_SE, PAL_NONE, ti->x, road_y, road_z, BY_SE_EDGE_BOUNDS, transparent);
	}
}

/**
 * The building sprite a finished tile of one of the game's own industries is
 * drawn with, of the pictures it has: the school, the machine and the statue
 * with girls about them or without (HasGirls()), the statue of stone or
 * bronze by the industry's random bits.
 * @param ind the industry
 * @param gfx its tile type
 * @param sprite what its drawing table gives
 * @return the sprite to draw
 */
static SpriteID GameOwnIndustrySprite(const Industry *ind, IndustryGfx gfx, SpriteID sprite)
{
	switch (gfx) {
		case GFX_GYMNASIUM_WEST: return HasGirls(ind) ? SPR_GYMNASIUM_GIRLS_WEST : sprite;
		case GFX_GYMNASIUM_SOUTH: return HasGirls(ind) ? SPR_GYMNASIUM_GIRLS_SOUTH : sprite;
		case GFX_WEED_MACHINE: return HasGirls(ind) ? SPR_WEED_MACHINE_GIRLS : sprite;
		case GFX_STATUE:
			if (HasBit(ind->random, 0)) return HasGirls(ind) ? SPR_STATUE_BRONZE_GIRLS : SPR_STATUE_BRONZE;
			return HasGirls(ind) ? SPR_STATUE_STONE_GIRLS : SPR_STATUE_STONE;
		default: return sprite;
	}
}

/**
 * The building sprite a tile of one of the game's own industries is drawn
 * with now, for the rig to read.
 * @param tile an industry tile
 * @return the sprite, 0 when there is none or the tile is not one of these
 */
SpriteID GameOwnIndustryTileSprite(TileIndex tile)
{
	IndustryGfx gfx = GetIndustryGfx(tile);
	const DrawBuildingsTileStruct *dits = GameOwnIndustryDrawTile(gfx, INDUSTRY_COMPLETED);
	if (dits == nullptr || !IsIndustryCompleted(tile)) return 0;
	if (gfx == GFX_MARIJUANA_PLANTATION) return MarijuanaPlantationSprite(Industry::GetByTile(tile));
	return GameOwnIndustrySprite(Industry::GetByTile(tile), gfx, dits->building.sprite);
}

/**
 * Have the tiles of one of the game's own industries drawn anew, when a
 * delivery may have brought the girls (HasGirls()).
 * @param ind the industry
 */
void MarkGameOwnIndustryDirty(const Industry *ind)
{
	if (ind->type != IT_GYMNASIUM && ind->type != IT_WEED_MACHINE && ind->type != IT_STATUE && ind->type != IT_COFFEESHOP && ind->type != IT_MARIJUANA_PLANTATION) return;
	for (TileIndex tile : ind->location) {
		if (IsTileType(tile, TileType::Industry) && GetIndustryIndex(tile) == ind->index) MarkTileDirtyByTile(tile);
	}
}

/**
 * What the coffeeshop takes, all of each (8/8): marijuana, and the cargoes of
 * sets that end at it -- tobacco (Industries of the Caribbean), paper,
 * tourists and alcohol (the sets name rum and alcohol alike) -- and the
 * students, called studentky in this game (NameStudentCargo()). Taken where
 * the game has them; a label no set brought is left out.
 */
static const std::array<CargoLabel, 7> COFFEESHOP_CARGOES{CT_MARIJUANA, CargoLabel{'TBCO'}, CT_PAPER, CargoLabel{'TOUR'}, CargoLabel{'BEER'}, CT_EXPLOSIVES, CT_STUDENTKY};

/**
 * What the girls' grammar school takes, all of each: the studentky it makes
 * as well, paper, and paints, under both labels the sets give them (DYES and
 * COAT). Taken where the game has them; a label no set brought is left out.
 */
static const std::array<CargoLabel, 4> GYMNASIUM_CARGOES{CT_STUDENTKY, CT_PAPER, CargoLabel{'DYES'}, CargoLabel{'COAT'}};

/** What the vending machine by the school takes: marijuana. */
static const std::array<CargoLabel, 1> WEED_MACHINE_CARGOES{CT_MARIJUANA};

/** What the statue of Karel Macha takes: studentky and tourists. */
static const std::array<CargoLabel, 2> STATUE_CARGOES{CT_STUDENTKY, CargoLabel{'TOUR'}};

/** What the marijuana plantation takes: the studentky who tend it (TendMarijuanaPlantations()). */
static const std::array<CargoLabel, 1> PLANTATION_CARGOES{CT_STUDENTKY};

/** How many studentky the girls' grammar school makes, as an industry's production rate. */
static const uint8_t GYMNASIUM_PRODUCTION_RATE = 10;

/**
 * The cargoes the coffeeshop takes where the game has them.
 * @return their labels
 */
std::span<const CargoLabel> CoffeeshopCargoes()
{
	return COFFEESHOP_CARGOES;
}

/**
 * Put one of the game's industries that stand in towns
 * (economy.extra_industries) and its tiles in their places: the bank of the
 * arctic and desert towns -- built only in a town, producing nothing -- with
 * the layout given, in every climate. What it takes is set once the cargoes
 * are (ResolveExtraIndustryCargoes()). To a set asking about it the industry
 * is that bank.
 * @param type the industry
 * @param layout its tiles
 * @param name its name
 * @param map_colour its colour on the map
 */
static void SetupTownIndustry(IndustryType type, const IndustryTileLayout &layout, StringID name, uint8_t map_colour)
{
	const IndustrySpec &bank = _origin_industry_specs[IT_BANK_TROPIC_ARCTIC];
	const IndustryGfx bank_tile = bank.layouts.front().front().gfx;

	for (const IndustryTileLayoutTile &t : layout) {
		IndustryTileSpec &tile = _industry_tile_specs[t.gfx];
		tile = _origin_industry_tile_specs[bank_tile];
		tile.grf_prop.subst_id = bank_tile;
		tile.accepts_cargo_label.fill(CT_INVALID);
		tile.accepts_cargo.fill(INVALID_CARGO);
		tile.acceptance.fill(0);
	}

	IndustrySpec &spec = _industry_specs[type];
	spec = bank;
	spec.layouts = {layout};
	spec.accepts_cargo_label.fill(CT_INVALID);
	spec.produced_cargo_label.fill(CT_INVALID);
	for (auto &multipliers : spec.input_cargo_multiplier) std::fill(std::begin(multipliers), std::end(multipliers), 0);
	std::fill(std::begin(spec.conflicting), std::end(spec.conflicting), IT_INVALID);
	spec.climate_availability = {LandscapeType::Temperate, LandscapeType::Arctic, LandscapeType::Tropic, LandscapeType::Toyland};
	std::fill(std::begin(spec.appear_ingame), std::end(spec.appear_ingame), bank.appear_ingame[to_underlying(LandscapeType::Tropic)]);
	std::fill(std::begin(spec.appear_creation), std::end(spec.appear_creation), bank.appear_creation[to_underlying(LandscapeType::Tropic)]);
	spec.map_colour = PixelColour{map_colour};
	spec.name = name;
	spec.grf_prop.subst_id = IT_BANK_TROPIC_ARCTIC;
	spec.enabled = true;
}

/**
 * Put the coffeeshop (economy.extra_industries) and its tiles in their
 * places, the industry type before the plantation's: the player's hut on 2x1
 * tiles along x, the back one with the hut, the front one with the yard
 * (SPR_HUT_FRONT and the rest). A coffeeshop built before the hut keeps its
 * one tile, GFX_COFFEESHOP, drawn as the desert house with the palm tree, and
 * takes what the hut's tiles take (ResolveExtraIndustryCargoes()).
 */
static void SetupCoffeeshop()
{
	SetupTownIndustry(IT_COFFEESHOP, {
			IndustryTileLayoutTile{TileIndexDiffC{0, 0}, GFX_HUT_BACK},
			IndustryTileLayoutTile{TileIndexDiffC{1, 0}, GFX_HUT_FRONT},
		}, STR_INDUSTRY_NAME_COFFEESHOP, 0xCF);
	_industry_tile_specs[GFX_COFFEESHOP] = _industry_tile_specs[GFX_HUT_BACK];
}

/**
 * Put the girls' grammar school, the vending machine by it and the statue of
 * Karel Macha (economy.extra_industries) in their places, drawn as the player
 * drew them (SPR_GYMNASIUM_WEST and the rest). The school is 2x2 and makes
 * studentky; the machine is one tile and stands at most two tiles from a
 * school (IsNearGymnasium()); the statue is one tile, in a town or out of
 * one, and takes studentky and tourists.
 */
static void SetupGymnasium()
{
	SetupTownIndustry(IT_GYMNASIUM, {
			IndustryTileLayoutTile{TileIndexDiffC{0, 0}, GFX_GYMNASIUM_NORTH},
			IndustryTileLayoutTile{TileIndexDiffC{1, 0}, GFX_GYMNASIUM_WEST},
			IndustryTileLayoutTile{TileIndexDiffC{0, 1}, GFX_GYMNASIUM_EAST},
			IndustryTileLayoutTile{TileIndexDiffC{1, 1}, GFX_GYMNASIUM_SOUTH},
		}, STR_INDUSTRY_NAME_GYMNASIUM, 0xAB);
	IndustrySpec &spec = _industry_specs[IT_GYMNASIUM];
	spec.produced_cargo_label[0] = CT_STUDENTKY;
	spec.production_rate[0] = GYMNASIUM_PRODUCTION_RATE;

	SetupTownIndustry(IT_WEED_MACHINE, {IndustryTileLayoutTile{TileIndexDiffC{0, 0}, GFX_WEED_MACHINE}}, STR_INDUSTRY_NAME_WEED_MACHINE, 0xCF);

	/* The statue stands in a town or out of one: on clear land, then. */
	SetupTownIndustry(IT_STATUE, {IndustryTileLayoutTile{TileIndexDiffC{0, 0}, GFX_STATUE}}, STR_INDUSTRY_NAME_STATUE, 0x0F);
	_industry_specs[IT_STATUE].behaviour.Reset(IndustryBehaviour::OnlyInTown);
}

/**
 * Give one of the game's own industries a cargo to take and, if asked, one to
 * make of it (economy.extra_industries): what it is given of the new input
 * comes out as the new output and nothing else, and its own inputs make what
 * they always made. It takes the new cargo at every tile that takes its first
 * own cargo, as much of it. An industry a set has put in its place is the
 * set's to fit out.
 * @param type the industry
 * @param in   the cargo it is to take
 * @param out  the cargo it is to make of it, or INVALID_CARGO for none
 */
static void AddExtraIndustryInput(IndustryType type, CargoType in, CargoType out)
{
	IndustrySpec &spec = _industry_specs[type];
	if (!spec.enabled || spec.grf_prop.HasGrfFile() || !IsValidCargoType(in)) return;
	if (std::ranges::find(spec.accepts_cargo, in) != spec.accepts_cargo.end()) return;
	CargoType own = spec.accepts_cargo[0];

	/* The first free places in the lists: the industry's own come first. */
	auto in_free = std::ranges::find_if(spec.accepts_cargo, [](CargoType c) { return !IsValidCargoType(c); });
	if (in_free == spec.accepts_cargo.end()) return;
	size_t in_at = in_free - spec.accepts_cargo.begin();
	*in_free = in;
	std::fill(std::begin(spec.input_cargo_multiplier[in_at]), std::end(spec.input_cargo_multiplier[in_at]), 0);

	if (IsValidCargoType(out)) {
		auto out_free = std::ranges::find_if(spec.produced_cargo, [](CargoType c) { return !IsValidCargoType(c); });
		if (out_free != spec.produced_cargo.end()) {
			size_t out_at = out_free - spec.produced_cargo.begin();
			*out_free = out;
			spec.production_rate[out_at] = 0; // made only of what it is given
			for (size_t i = 0; i < std::size(spec.accepts_cargo); i++) spec.input_cargo_multiplier[i][out_at] = 0;
			spec.input_cargo_multiplier[in_at][out_at] = 256;
		}
	}

	if (!IsValidCargoType(own)) return;
	for (const IndustryTileLayout &layout : spec.layouts) {
		for (const IndustryTileLayoutTile &t : layout) {
			if (t.gfx >= NEW_INDUSTRYTILEOFFSET) continue;
			IndustryTileSpec &tile = _industry_tile_specs[t.gfx];
			if (tile.grf_prop.HasGrfFile()) continue;
			auto takes_own = std::ranges::find(tile.accepts_cargo, own);
			if (takes_own == tile.accepts_cargo.end()) continue;
			if (std::ranges::find(tile.accepts_cargo, in) != tile.accepts_cargo.end()) continue;
			auto free = std::ranges::find_if(tile.accepts_cargo, [](CargoType c) { return !IsValidCargoType(c); });
			if (free == tile.accepts_cargo.end()) continue;
			*free = in;
			tile.acceptance[free - tile.accepts_cargo.begin()] = tile.acceptance[takes_own - tile.accepts_cargo.begin()];
		}
	}
}

/**
 * The player's chain from the marijuana plantation to the bomb: the oil
 * refinery makes explosives of hemp fibre, beside goods of oil. The
 * coffeeshop takes the explosives (COFFEESHOP_CARGOES) -- the player's last
 * word, after banks and gangsters were thought about and put aside.
 */
static void ResolveExplosivesChain()
{
	CargoType fibre = GetCargoTypeByLabel(CT_HEMP_FIBRE);
	CargoType explosives = GetCargoTypeByLabel(CT_EXPLOSIVES);
	if (!IsValidCargoType(fibre) || !IsValidCargoType(explosives)) return;
	AddExtraIndustryInput(IT_OIL_REFINERY, fibre, explosives);
}

/**
 * An industry built before the extra industries had the cargoes they have now
 * -- the refinery before it took hemp fibre, the plantation before it grew
 * it, the coffeeshop before it took explosives -- is given what its kind now
 * takes and makes, at the places the kind has them, so the deliveries are
 * counted against the right input and output. Nothing it had is moved or
 * taken away. Called after a game is loaded.
 */
void UpdateExtraIndustryCargoes()
{
	if (!_settings_game.economy.extra_industries) return;
	for (Industry *i : Industry::Iterate()) {
		if (i->type != IT_OIL_REFINERY && i->type != IT_MARIJUANA_PLANTATION && i->type != IT_COFFEESHOP && i->type != IT_GYMNASIUM && i->type != IT_WEED_MACHINE && i->type != IT_STATUE) continue;
		const IndustrySpec *spec = GetIndustrySpec(i->type);
		if (spec->grf_prop.HasGrfFile()) continue;
		for (size_t index = 0; index < std::size(spec->accepts_cargo); index++) {
			CargoType cargo = spec->accepts_cargo[index];
			if (!IsValidCargoType(cargo) || index < i->accepted.size() || i->IsCargoAccepted(cargo)) continue;
			while (i->accepted.size() < index) i->accepted.emplace_back().cargo = INVALID_CARGO;
			i->accepted.emplace_back().cargo = cargo;
		}
		for (size_t index = 0; index < std::size(spec->produced_cargo); index++) {
			CargoType cargo = spec->produced_cargo[index];
			if (!IsValidCargoType(cargo) || index < i->produced.size() || i->IsCargoProduced(cargo)) continue;
			while (i->produced.size() < index) i->produced.emplace_back().cargo = INVALID_CARGO;
			Industry::ProducedCargo &p = i->produced.emplace_back();
			p.cargo = cargo;
			p.rate = spec->production_rate[index];
		}
	}
}

/**
 * Let one of the game's industries in towns take all of each of the cargoes
 * the game has of a list: the industry, so that what is delivered is taken,
 * and every one of its tiles, so that a station by it takes it.
 * @param type the industry
 * @param labels the cargoes
 */
static void ResolveTownIndustryCargoes(IndustryType type, std::span<const CargoLabel> labels)
{
	IndustrySpec &spec = _industry_specs[type];
	if (!spec.enabled || spec.grf_prop.HasGrfFile()) return;

	spec.accepts_cargo.fill(INVALID_CARGO);
	for (const IndustryTileLayoutTile &t : spec.layouts.front()) {
		IndustryTileSpec &tile = _industry_tile_specs[t.gfx];
		tile.accepts_cargo.fill(INVALID_CARGO);
		tile.acceptance.fill(0);
	}
	size_t next = 0;
	for (CargoLabel label : labels) {
		CargoType cargo = GetCargoTypeByLabel(label);
		if (!IsValidCargoType(cargo)) continue;
		spec.accepts_cargo[next] = cargo;
		for (const IndustryTileLayoutTile &t : spec.layouts.front()) {
			IndustryTileSpec &tile = _industry_tile_specs[t.gfx];
			tile.accepts_cargo[next] = cargo;
			tile.acceptance[next] = 8;
		}
		next++;
	}
}

void ResolveExtraIndustryCargoes()
{
	if (!_settings_game.economy.extra_industries) return;
	ResolveExplosivesChain();
	ResolveTownIndustryCargoes(IT_COFFEESHOP, COFFEESHOP_CARGOES);
	/* The desert house of a coffeeshop built before the hut takes the same. */
	if (GetIndustrySpec(IT_COFFEESHOP)->enabled && !GetIndustrySpec(IT_COFFEESHOP)->grf_prop.HasGrfFile()) {
		_industry_tile_specs[GFX_COFFEESHOP] = _industry_tile_specs[GFX_HUT_BACK];
	}
	ResolveTownIndustryCargoes(IT_GYMNASIUM, GYMNASIUM_CARGOES);
	ResolveTownIndustryCargoes(IT_WEED_MACHINE, WEED_MACHINE_CARGOES);
	ResolveTownIndustryCargoes(IT_STATUE, STATUE_CARGOES);
	ResolveTownIndustryCargoes(IT_MARIJUANA_PLANTATION, PLANTATION_CARGOES);
}

/**
 * This function initialize the spec arrays of both
 * industry and industry tiles.
 * It adjusts the enabling of the industry too, based on climate availability.
 * This will allow for clearer testings
 */
void ResetIndustries()
{
	auto industry_insert = std::copy(std::begin(_origin_industry_specs), std::end(_origin_industry_specs), std::begin(_industry_specs));
	std::fill(industry_insert, std::end(_industry_specs), IndustrySpec{});

	/* Enable only the current climate industries, and those of the climates switched on (IndustryClimatesOn()). */
	for (auto it = std::begin(_industry_specs); it != industry_insert; ++it) {
		it->enabled = it->climate_availability.Test(_settings_game.game_creation.landscape) || it->climate_availability.Any(IndustryClimatesOn());
	}

	auto industry_tile_insert = std::copy(std::begin(_origin_industry_tile_specs), std::end(_origin_industry_tile_specs), std::begin(_industry_tile_specs));
	std::fill(industry_tile_insert, std::end(_industry_tile_specs), IndustryTileSpec{});

	/* Before any set is read, so that the override manager passes their types by. */
	if (_settings_game.economy.extra_industries) {
		SetupMarijuanaPlantation();
		SetupCoffeeshop();
		SetupGymnasium();
	}

	/* Reset any overrides that have been set. */
	_industile_mngr.ResetOverride();
	_industry_mngr.ResetOverride();
}

/**
 * The climates whose original industries are switched on in this game beside
 * the played one's (economy.industries_temperate and the rest). See
 * climate_industries.h.
 * @return the climates, none when all are off
 */
LandscapeTypes IndustryClimatesOn()
{
	LandscapeTypes on{};
	if (_settings_game.economy.industries_temperate) on.Set(LandscapeType::Temperate);
	if (_settings_game.economy.industries_arctic) on.Set(LandscapeType::Arctic);
	if (_settings_game.economy.industries_tropic) on.Set(LandscapeType::Tropic);
	if (_settings_game.economy.industries_toyland) on.Set(LandscapeType::Toyland);
	return on;
}

/**
 * Is this an original industry of a climate switched on, one that no set
 * switches off or puts its own in place of?
 * @param type the industry type, as the original table numbers it
 * @return whether it is kept
 */
bool IsOriginalIndustryKept(IndustryType type)
{
	return type < NEW_INDUSTRYOFFSET && _origin_industry_specs[type].climate_availability.Any(IndustryClimatesOn());
}

/**
 * Is this an original industry tile one of the kept original industries is
 * built of, so no set puts its own tile in place of it?
 * @param gfx the tile, as the original table numbers it
 * @return whether it is kept
 */
bool IsOriginalIndustryTileKept(IndustryGfx gfx)
{
	if (gfx >= NEW_INDUSTRYTILEOFFSET) return false;
	for (IndustryType type = 0; type < NEW_INDUSTRYOFFSET; type++) {
		if (!IsOriginalIndustryKept(type)) continue;
		for (const IndustryTileLayout &layout : _origin_industry_specs[type].layouts) {
			for (const IndustryTileLayoutTile &tile : layout) {
				if (tile.gfx == gfx) return true;
			}
		}
	}
	return false;
}

/**
 * The climate an original industry is at home in, for the cargoes it
 * produces: the climate played when the industry is of it, otherwise the
 * first climate switched on that it is of.
 * @param type the industry type, as the original table numbers it
 * @return the climate
 */
LandscapeType IndustryHomeClimate(IndustryType type)
{
	LandscapeType played = _settings_game.game_creation.landscape;
	if (type >= NEW_INDUSTRYOFFSET) return played;
	LandscapeTypes availability = _origin_industry_specs[type].climate_availability;
	if (availability.Test(played)) return played;
	for (LandscapeType climate : {LandscapeType::Temperate, LandscapeType::Arctic, LandscapeType::Tropic, LandscapeType::Toyland}) {
		if (availability.Test(climate) && IndustryClimatesOn().Test(climate)) return climate;
	}
	return played;
}

/**
 * Which cargo a mixed cargo is in one climate.
 * @param mixed the mixed cargo
 * @param climate the climate
 * @return its label there, CT_INVALID where the climate has none of it (toyland)
 */
CargoLabel MixedCargoLabelFor(MixedCargoType mixed, LandscapeType climate)
{
	switch (mixed) {
		case MCT_LIVESTOCK_FRUIT:
			switch (climate) {
				case LandscapeType::Temperate: case LandscapeType::Arctic: return CT_LIVESTOCK;
				case LandscapeType::Tropic: return CT_FRUIT;
				default: return CT_INVALID;
			}
		case MCT_GRAIN_WHEAT_MAIZE:
			switch (climate) {
				case LandscapeType::Temperate: return CT_GRAIN;
				case LandscapeType::Arctic: return CT_WHEAT;
				case LandscapeType::Tropic: return CT_MAIZE;
				default: return CT_INVALID;
			}
		case MCT_VALUABLES_GOLD_DIAMONDS:
			switch (climate) {
				case LandscapeType::Temperate: return CT_VALUABLES;
				case LandscapeType::Arctic: return CT_GOLD;
				case LandscapeType::Tropic: return CT_DIAMONDS;
				default: return CT_INVALID;
			}
		default: NOT_REACHED();
	}
}

/**
 * The labels an original cargo entry stands for: itself, or for a mixed
 * cargo its kind in each of the climates given.
 * @param label the entry
 * @param climates the climates
 * @return the labels, without duplicates and without CT_INVALID
 */
static std::vector<CargoLabel> CargoLabelsIn(const std::variant<CargoLabel, MixedCargoType> &label, LandscapeTypes climates)
{
	std::vector<CargoLabel> labels;
	auto add = [&labels](CargoLabel l) {
		if (l != CT_INVALID && std::ranges::find(labels, l) == labels.end()) labels.push_back(l);
	};
	if (std::holds_alternative<CargoLabel>(label)) {
		add(std::get<CargoLabel>(label));
	} else {
		for (LandscapeType climate : climates) add(MixedCargoLabelFor(std::get<MixedCargoType>(label), climate));
	}
	return labels;
}

/** The climates whose kinds of a mixed cargo an original industry takes: the played one and those switched on. */
static LandscapeTypes AcceptingClimates()
{
	LandscapeTypes climates = IndustryClimatesOn();
	climates.Set(_settings_game.game_creation.landscape);
	return climates;
}

/**
 * The cargoes the original industries of this game need that the climate
 * played may lack: what each produces in its home climate, what each takes
 * in every climate on; and marijuana, when the game's marijuana plantation
 * is in it (economy.extra_industries), with hemp fibre, explosives and the
 * studentky of the churches and parks. Nothing when neither is.
 * @return the labels
 */
std::vector<CargoLabel> CargoLabelsOfClimateIndustries()
{
	std::vector<CargoLabel> labels;
	if (_settings_game.economy.extra_industries) {
		labels.push_back(CT_MARIJUANA);
		labels.push_back(CT_HEMP_FIBRE);
		labels.push_back(CT_EXPLOSIVES);
		labels.push_back(CT_STUDENTKY);
	}
	if (IndustryClimatesOn().None()) return labels;
	auto add = [&labels](const std::vector<CargoLabel> &more) {
		for (CargoLabel l : more) {
			if (std::ranges::find(labels, l) == labels.end()) labels.push_back(l);
		}
	};
	LandscapeType played = _settings_game.game_creation.landscape;
	for (IndustryType type = 0; type < NEW_INDUSTRYOFFSET; type++) {
		const IndustrySpec &spec = _origin_industry_specs[type];
		if (!spec.climate_availability.Test(played) && !IsOriginalIndustryKept(type)) continue;
		for (const auto &label : spec.produced_cargo_label) add(CargoLabelsIn(label, IndustryHomeClimate(type)));
		for (const auto &label : spec.accepts_cargo_label) add(CargoLabelsIn(label, AcceptingClimates()));
		for (const IndustryTileLayout &layout : spec.layouts) {
			for (const IndustryTileLayoutTile &tile : layout) {
				if (tile.gfx >= NEW_INDUSTRYTILEOFFSET) continue;
				for (const auto &label : _origin_industry_tile_specs[tile.gfx].accepts_cargo_label) add(CargoLabelsIn(label, AcceptingClimates()));
			}
		}
	}
	return labels;
}

/**
 * Give the original industries and industry tiles their cargoes when a
 * climate is switched on (climate_industries.h): an industry produces in its
 * home climate (IndustryHomeClimate()), and where it or its tile takes a
 * mixed cargo, it takes the kind of every climate on as well, in the free
 * places of its list, with the same multiplier or acceptance. Called once
 * the cargoes are set (FinaliseIndustriesArray()); with no climate switched
 * on it does nothing, and the game resolves the cargoes as it always did.
 */
void ResolveOriginalIndustryCargoes()
{
	if (IndustryClimatesOn().None()) return;

	for (IndustryType type = 0; type < NEW_INDUSTRYOFFSET; type++) {
		IndustrySpec &spec = _industry_specs[type];
		if (spec.grf_prop.HasGrfFile()) continue; // a set's industry in this place resolves its own

		for (size_t i = 0; i < std::size(spec.produced_cargo_label); i++) {
			std::vector<CargoLabel> labels = CargoLabelsIn(spec.produced_cargo_label[i], IndustryHomeClimate(type));
			spec.produced_cargo[i] = labels.empty() ? INVALID_CARGO : GetCargoTypeByLabel(labels.front());
		}

		size_t next_free = std::size(spec.accepts_cargo_label);
		for (size_t i = 0; i < std::size(spec.accepts_cargo_label); i++) {
			bool first = true;
			for (CargoLabel label : CargoLabelsIn(spec.accepts_cargo_label[i], AcceptingClimates())) {
				CargoType cargo = GetCargoTypeByLabel(label);
				if (!IsValidCargoType(cargo)) continue;
				if (first) {
					spec.accepts_cargo[i] = cargo;
					first = false;
				} else if (next_free < std::size(spec.accepts_cargo) && std::ranges::find(spec.accepts_cargo, cargo) == spec.accepts_cargo.end()) {
					spec.accepts_cargo[next_free] = cargo;
					std::copy(std::begin(spec.input_cargo_multiplier[i]), std::end(spec.input_cargo_multiplier[i]), std::begin(spec.input_cargo_multiplier[next_free]));
					next_free++;
				}
			}
			if (first) spec.accepts_cargo[i] = INVALID_CARGO;
		}
	}

	for (IndustryGfx gfx = 0; gfx < NEW_INDUSTRYTILEOFFSET; gfx++) {
		IndustryTileSpec &tile = _industry_tile_specs[gfx];
		if (tile.grf_prop.HasGrfFile()) continue;
		size_t next_free = std::size(tile.accepts_cargo_label);
		for (size_t i = 0; i < std::size(tile.accepts_cargo_label); i++) {
			bool first = true;
			for (CargoLabel label : CargoLabelsIn(tile.accepts_cargo_label[i], AcceptingClimates())) {
				CargoType cargo = GetCargoTypeByLabel(label);
				if (!IsValidCargoType(cargo)) continue;
				if (first) {
					tile.accepts_cargo[i] = cargo;
					first = false;
				} else if (next_free < std::size(tile.accepts_cargo) && std::ranges::find(tile.accepts_cargo, cargo) == tile.accepts_cargo.end()) {
					tile.accepts_cargo[next_free] = cargo;
					tile.acceptance[next_free] = tile.acceptance[i];
					next_free++;
				}
			}
			if (first) tile.accepts_cargo[i] = INVALID_CARGO;
		}
	}
}

/**
 * How likely an original industry is to appear, where the climate played
 * gives it no chance because it is not of it: the chance of its own climate,
 * the best of those switched on.
 * @param type the industry type
 * @param creation the chance at map creation, otherwise during the game
 * @return the chance
 */
uint8_t OriginalIndustryChance(IndustryType type, bool creation)
{
	const IndustrySpec *spec = GetIndustrySpec(type);
	LandscapeType played = _settings_game.game_creation.landscape;
	uint8_t chance = creation ? spec->appear_creation[to_underlying(played)] : spec->appear_ingame[to_underlying(played)];
	if (type >= NEW_INDUSTRYOFFSET || spec->grf_prop.HasGrfFile() || spec->climate_availability.Test(played)) return chance;
	for (LandscapeType climate : IndustryClimatesOn()) {
		if (!spec->climate_availability.Test(climate)) continue;
		chance = std::max(chance, creation ? spec->appear_creation[to_underlying(climate)] : spec->appear_ingame[to_underlying(climate)]);
	}
	return chance;
}

/**
 * Retrieve the type for this industry.  Although it is accessed by a tile,
 * it will return the general type of industry, and not the sprite index
 * as would do GetIndustryGfx.
 * @param tile that is queried
 * @pre IsTileType(tile, TileType::Industry)
 * @return general type for this industry, as defined in industry.h
 */
IndustryType GetIndustryType(Tile tile)
{
	assert(IsTileType(tile, TileType::Industry));

	const Industry *ind = Industry::GetByTile(tile);
	assert(ind != nullptr);
	return ind->type;
}

/**
 * Accessor for array _industry_specs.
 * This will ensure at once : proper access and
 * not allowing modifications of it.
 * @param thistype of industry (which is the index in _industry_specs)
 * @pre thistype < NUM_INDUSTRYTYPES
 * @return a pointer to the corresponding industry spec
 */
const IndustrySpec *GetIndustrySpec(IndustryType thistype)
{
	assert(thistype < NUM_INDUSTRYTYPES);
	return &_industry_specs[thistype];
}

/**
 * Accessor for array _industry_tile_specs.
 * This will ensure at once : proper access and
 * not allowing modifications of it.
 * @param gfx of industrytile (which is the index in _industry_tile_specs)
 * @pre gfx < INVALID_INDUSTRYTILE
 * @return a pointer to the corresponding industrytile spec
 */
const IndustryTileSpec *GetIndustryTileSpec(IndustryGfx gfx)
{
	assert(gfx < NUM_INDUSTRYTILES);
	return &_industry_tile_specs[gfx];
}

/** Remove any reference to this industry from the game. */
Industry::~Industry()
{
	if (CleaningPool()) return;

	/* Industry can also be destroyed when not fully initialized.
	 * This means that we do not have to clear tiles either.
	 * Also we must not decrement industry counts in that case. */
	if (this->location.w == 0) return;

	const bool has_neutral_station = this->neutral_station != nullptr;

	for (TileIndex tile_cur : this->location) {
		if (IsTileType(tile_cur, TileType::Industry)) {
			if (GetIndustryIndex(tile_cur) == this->index) {
				DeleteNewGRFInspectWindow(GrfSpecFeature::IndustryTiles, tile_cur.base());

				/* MakeWaterKeepingClass() can also handle 'land' */
				MakeWaterKeepingClass(tile_cur, OWNER_NONE);
			}
		} else if (IsTileType(tile_cur, TileType::Station) && IsOilRig(tile_cur)) {
			DeleteOilRig(tile_cur);
		}
	}

	if (has_neutral_station) {
		/* Remove possible docking tiles */
		for (TileIndex tile_cur : this->location) {
			ClearDockingTilesCheckingNeighbours(tile_cur);
		}
	}

	if (GetIndustrySpec(this->type)->behaviour.Test(IndustryBehaviour::PlantFields)) {
		TileArea ta = TileArea(this->location.tile, 0, 0).Expand(21);

		/* Remove the farmland and convert it to regular tiles over time. */
		for (TileIndex tile_cur : ta) {
			if (IsTileType(tile_cur, TileType::Clear) && IsClearGround(tile_cur, ClearGround::Fields) &&
					GetIndustryIndexOfField(tile_cur) == this->index) {
				SetIndustryIndexOfField(tile_cur, IndustryID::Invalid());
			}
		}
	}

	/* don't let any disaster vehicle target invalid industry */
	ReleaseDisastersTargetingIndustry(this->index);

	/* Clear the persistent storage. */
	delete this->psa;

	auto &industries = Industry::industries[type];
	industries.erase(this->index);

	DeleteIndustryNews(this->index);
	CloseWindowById(WindowClass::IndustryView, this->index);
	CloseWindowById(WindowClass::IndustryProductionGraph, this->index);
	DeleteNewGRFInspectWindow(GrfSpecFeature::Industries, this->index);

	Source src{this->index, SourceType::Industry};
	DeleteSubsidyWith(src);
	CargoPacket::InvalidateAllFrom(src);

	for (Station *st : this->stations_near) {
		st->RemoveIndustryToDeliver(this);
	}
}

/**
 * Invalidating some stuff after removing item from the pool.
 * @param index index of deleted item
 */
void Industry::PostDestructor(size_t)
{
	InvalidateWindowData(WindowClass::IndustryDirectory, 0, IDIWD_FORCE_REBUILD);
	SetWindowDirty(WindowClass::BuildIndustry, 0);
}


/**
 * Return a random valid industry.
 * @return random industry, nullptr if there are no industries
 */
/* static */ Industry *Industry::GetRandom()
{
	if (Industry::GetNumItems() == 0) return nullptr;
	int num = RandomRange((uint16_t)Industry::GetNumItems());
	size_t index = std::numeric_limits<size_t>::max();

	while (num >= 0) {
		num--;
		index++;

		/* Make sure we have a valid industry */
		while (!Industry::IsValidID(index)) {
			index++;
			assert(index < Industry::GetPoolSize());
		}
	}

	return Industry::Get(index);
}


static void IndustryDrawSugarMine(const TileInfo *ti)
{
	if (!IsIndustryCompleted(ti->tile)) return;

	const DrawIndustryAnimationStruct *d = &_draw_industry_spec1[GetAnimationFrame(ti->tile)];

	AddChildSpriteScreen(SPR_IT_SUGAR_MINE_SIEVE + d->image_1, PAL_NONE, d->x, 0);

	if (d->image_2 != 0) {
		AddChildSpriteScreen(SPR_IT_SUGAR_MINE_CLOUDS + d->image_2 - 1, PAL_NONE, 8, 41);
	}

	if (d->image_3 != 0) {
		AddChildSpriteScreen(SPR_IT_SUGAR_MINE_PILE + d->image_3 - 1, PAL_NONE,
			_drawtile_proc1[d->image_3 - 1].x, _drawtile_proc1[d->image_3 - 1].y);
	}
}

static void IndustryDrawToffeeQuarry(const TileInfo *ti)
{
	uint8_t x = 0;

	if (IsIndustryCompleted(ti->tile)) {
		x = _industry_anim_offs_toffee[GetAnimationFrame(ti->tile)];
		if (x == 0xFF) {
			x = 0;
		}
	}

	AddChildSpriteScreen(SPR_IT_TOFFEE_QUARRY_SHOVEL, PAL_NONE, 22 - x, 24 + x);
	AddChildSpriteScreen(SPR_IT_TOFFEE_QUARRY_TOFFEE, PAL_NONE, 6, 14);
}

static void IndustryDrawBubbleGenerator( const TileInfo *ti)
{
	if (IsIndustryCompleted(ti->tile)) {
		AddChildSpriteScreen(SPR_IT_BUBBLE_GENERATOR_BUBBLE, PAL_NONE, 5, _industry_anim_offs_bubbles[GetAnimationFrame(ti->tile)]);
	}
	AddChildSpriteScreen(SPR_IT_BUBBLE_GENERATOR_SPRING, PAL_NONE, 3, 67);
}

static void IndustryDrawToyFactory(const TileInfo *ti)
{
	const DrawIndustryAnimationStruct *d = &_industry_anim_offs_toys[GetAnimationFrame(ti->tile)];

	if (d->image_1 != 0xFF) {
		AddChildSpriteScreen(SPR_IT_TOY_FACTORY_CLAY, PAL_NONE, d->x, 96 + d->image_1);
	}

	if (d->image_2 != 0xFF) {
		AddChildSpriteScreen(SPR_IT_TOY_FACTORY_ROBOT, PAL_NONE, 16 - d->image_2 * 2, 100 + d->image_2);
	}

	AddChildSpriteScreen(SPR_IT_TOY_FACTORY_STAMP, PAL_NONE, 7, d->image_3);
	AddChildSpriteScreen(SPR_IT_TOY_FACTORY_STAMP_HOLDER, PAL_NONE, 0, 42);
}

static void IndustryDrawCoalPlantSparks(const TileInfo *ti)
{
	if (IsIndustryCompleted(ti->tile)) {
		uint8_t image = GetAnimationFrame(ti->tile);

		if (image != 0 && image < 7) {
			AddChildSpriteScreen(image + SPR_IT_POWER_PLANT_TRANSFORMERS,
				PAL_NONE,
				_coal_plant_sparks[image - 1].x,
				_coal_plant_sparks[image - 1].y
			);
		}
	}
}

typedef void IndustryDrawTileProc(const TileInfo *ti);
static IndustryDrawTileProc * const _industry_draw_tile_procs[5] = {
	IndustryDrawSugarMine,
	IndustryDrawToffeeQuarry,
	IndustryDrawBubbleGenerator,
	IndustryDrawToyFactory,
	IndustryDrawCoalPlantSparks,
};

/**
 * A sprite of an original industry as the industry's own climate draws it,
 * where the base graphics file of the climate played has put its own in its
 * place (CLIMATE_INDUSTRY_SPRITE_RANGES): the cotton candy forest in the
 * temperate climate, the desert farm in the arctic.
 * @param image the sprite, with its flags
 * @param climate the industry's own climate (IndustryHomeClimate())
 * @return the sprite to draw, with the same flags
 */
SpriteID ClimateIndustrySprite(SpriteID image, LandscapeType climate)
{
	if (climate == _settings_game.game_creation.landscape) return image;
	int index = ClimateIndustrySpriteIndex(GB(image, 0, SPRITE_WIDTH));
	if (index < 0) return image;
	SpriteID own = SPR_CLIMATE_INDUSTRY_BASE + to_underlying(climate) * CLIMATE_INDUSTRY_SPRITES_PER_CLIMATE + index;
	SB(image, 0, SPRITE_WIDTH, own);
	return image;
}

/**
 * Every sprite among CLIMATE_INDUSTRY_SPRITE_RANGES that the original
 * industries of this game draw, with the sprite drawn for it.
 * @return one entry per industry and sprite
 */
std::vector<ClimateIndustrySpriteUse> ClimateIndustrySpriteUses()
{
	std::vector<ClimateIndustrySpriteUse> uses;
	for (IndustryType type = 0; type < NEW_INDUSTRYOFFSET; type++) {
		const IndustrySpec *spec = GetIndustrySpec(type);
		if (!spec->enabled || spec->grf_prop.HasGrfFile()) continue;
		LandscapeType climate = IndustryHomeClimate(type);
		std::vector<SpriteID> seen;
		for (const IndustryTileLayout &layout : spec->layouts) {
			for (const IndustryTileLayoutTile &tile : layout) {
				if (tile.gfx >= NEW_INDUSTRYTILEOFFSET) continue;
				for (uint stage = 0; stage < 4; stage++) {
					const DrawBuildingsTileStruct &dits = _industry_draw_tile_data[tile.gfx << 2 | stage];
					for (SpriteID image : {dits.ground.sprite, dits.building.sprite}) {
						SpriteID sprite = GB(image, 0, SPRITE_WIDTH);
						if (ClimateIndustrySpriteIndex(sprite) < 0 || std::ranges::find(seen, sprite) != seen.end()) continue;
						seen.push_back(sprite);
						uses.push_back({type, climate, sprite, GB(ClimateIndustrySprite(sprite, climate), 0, SPRITE_WIDTH)});
					}
				}
			}
		}
	}
	return uses;
}

/** @copydoc DrawTileProc */
static void DrawTile_Industry(TileInfo *ti)
{
	IndustryGfx gfx = GetIndustryGfx(ti->tile);
	Industry *ind = Industry::GetByTile(ti->tile);
	const IndustryTileSpec *indts = GetIndustryTileSpec(gfx);

	/* Retrieve pointer to the draw industry tile struct */
	if (gfx >= NEW_INDUSTRYTILEOFFSET && GameOwnIndustryDrawTile(gfx, 0) == nullptr) {
		/* Draw the tile using the specialized method of newgrf industrytile.
		 * DrawNewIndustry will return false if ever the resolver could not
		 * find any sprite to display.  So in this case, we will jump on the
		 * substitute gfx instead. */
		if (indts->grf_prop.HasSpriteGroups() && DrawNewIndustryTile(ti, ind, gfx, indts)) {
			return;
		} else {
			/* No sprite group (or no valid one) found, meaning no graphics associated.
			 * Use the substitute one instead */
			if (indts->grf_prop.subst_id != INVALID_INDUSTRYTILE) {
				gfx = indts->grf_prop.subst_id;
				/* And point the industrytile spec accordingly */
				indts = GetIndustryTileSpec(gfx);
			}
		}
	}

	const uint stage = indts->anim_state ? GetAnimationFrame(ti->tile) & INDUSTRY_COMPLETED : GetIndustryConstructionStage(ti->tile);
	const DrawBuildingsTileStruct *dits = GameOwnIndustryDrawTile(gfx, stage);
	if (dits == nullptr) dits = &_industry_draw_tile_data[gfx << 2 | stage];

	/* An original industry of another climate than the one played is drawn as
	 * its own climate draws it. */
	const LandscapeType climate = IndustryHomeClimate(ind->type);

	SpriteID image = ClimateIndustrySprite(dits->ground.sprite, climate);

	/* DrawFoundation() modifies ti->z and ti->tileh */
	if (ti->tileh != SLOPE_FLAT) DrawFoundation(ti, Foundation::Leveled);

	/* If the ground sprite is the default flat water sprite, draw also canal/river borders.
	 * Do not do this if the tile's WaterClass is 'land'. */
	if (image == SPR_FLAT_WATER_TILE && IsTileOnWater(ti->tile)) {
		DrawWaterClassGround(ti);
	} else {
		DrawGroundSprite(image, GroundSpritePaletteTransform(image, dits->ground.pal, GetColourPalette(ind->random_colour)));
	}

	/* If industries are transparent and invisible, do not draw the upper part */
	if (IsInvisibilitySet(TransparencyOption::Industries)) return;

	/* The plantation is laid from its own pictures. */
	if (gfx == GFX_MARIJUANA_PLANTATION && stage == INDUSTRY_COMPLETED) {
		DrawMarijuanaPlantationTile(ti, ind);
		return;
	}

	/* Add industry on top of the ground? */
	image = ClimateIndustrySprite(dits->building.sprite, climate);
	if (stage == INDUSTRY_COMPLETED && image != 0) image = GameOwnIndustrySprite(ind, gfx, image);
	if (image != 0) {
		AddSortableSpriteToDraw(image, SpriteLayoutPaletteTransform(image, dits->building.pal, GetColourPalette(ind->random_colour)),
			*ti, *dits, IsTransparencySet(TransparencyOption::Industries));

		/* The girls at the coffeeshop's hut, laid over its front tile. Their
		 * offsets count from the tile's north corner, as the hut's do. */
		if (stage == INDUSTRY_COMPLETED && gfx == GFX_HUT_FRONT) {
			HutGirls girls = HutGirlsAt(ind);
			if (girls != HutGirls::None) {
				AddChildSpriteScreen(girls == HutGirls::Sitting ? SPR_HUT_GIRLS_SITTING : SPR_HUT_GIRLS_STANDING, PAL_NONE, 0, 0,
						IsTransparencySet(TransparencyOption::Industries), nullptr, false, false);
			}
			/* The fireworks over the yard, after the girls; the tile is
			 * animated while they go on (AnimateTile_Industry()). */
			if (HutHasFireworks(ind)) {
				SpriteID firework = HutFireworkSprite(ti->tile, TimerGameTick::counter);
				if (firework != 0) AddChildSpriteScreen(firework, PAL_NONE, 0, 0, IsTransparencySet(TransparencyOption::Industries), nullptr, false, false);
			}
		}

		if (IsTransparencySet(TransparencyOption::Industries)) return;
	}

	{
		int proc = dits->draw_proc - 1;
		if (proc >= 0) _industry_draw_tile_procs[proc](ti);
	}
}

/** @copydoc GetFoundationProc */
static Foundation GetFoundation_Industry(TileIndex tile, Slope tileh)
{
	IndustryGfx gfx = GetIndustryGfx(tile);

	/* For NewGRF industry tiles we might not be drawing a foundation. We need to
	 * account for this, as other structures should
	 * draw the wall of the foundation in this case.
	 */
	if (gfx >= NEW_INDUSTRYTILEOFFSET) {
		const IndustryTileSpec *indts = GetIndustryTileSpec(gfx);
		if (indts->callback_mask.Test(IndustryTileCallbackMask::DrawFoundations)) {
			uint32_t callback_res = GetIndustryTileCallback(CBID_INDTILE_DRAW_FOUNDATIONS, 0, 0, gfx, Industry::GetByTile(tile), tile);
			if (callback_res != CALLBACK_FAILED && !ConvertBooleanCallback(indts->grf_prop.grffile, CBID_INDTILE_DRAW_FOUNDATIONS, callback_res)) return Foundation::None;
		}
	}
	return FlatteningFoundation(tileh);
}

/** @copydoc AddAcceptedCargoProc */
static void AddAcceptedCargo_Industry(TileIndex tile, CargoArray &acceptance, CargoTypes &always_accepted)
{
	IndustryGfx gfx = GetIndustryGfx(tile);
	const IndustryTileSpec *itspec = GetIndustryTileSpec(gfx);
	const Industry *ind = Industry::GetByTile(tile);

	/* Starting point for acceptance */
	auto accepts_cargo = itspec->accepts_cargo;
	auto cargo_acceptance = itspec->acceptance;

	if (itspec->special_flags.Test(IndustryTileSpecialFlag::AcceptsAllCargo)) {
		/* Copy all accepted cargoes from industry itself */
		for (const auto &a : ind->accepted) {
			auto pos = std::ranges::find(accepts_cargo, a.cargo);
			if (pos == std::end(accepts_cargo)) {
				/* Not found, insert */
				pos = std::ranges::find(accepts_cargo, INVALID_CARGO);
				if (pos == std::end(accepts_cargo)) continue; // nowhere to place, give up on this one
				*pos = a.cargo;
			}
			cargo_acceptance[std::distance(std::begin(accepts_cargo), pos)] += 8;
		}
	}

	if (itspec->callback_mask.Test(IndustryTileCallbackMask::AcceptCargo)) {
		/* Try callback for accepts list, if success override all existing accepts */
		uint16_t res = GetIndustryTileCallback(CBID_INDTILE_ACCEPT_CARGO, 0, 0, gfx, Industry::GetByTile(tile), tile);
		if (res != CALLBACK_FAILED) {
			accepts_cargo.fill(INVALID_CARGO);
			for (uint i = 0; i < INDUSTRY_ORIGINAL_NUM_INPUTS; i++) accepts_cargo[i] = GetCargoTranslation(GB(res, i * 5, 5), itspec->grf_prop.grffile);
		}
	}

	if (itspec->callback_mask.Test(IndustryTileCallbackMask::CargoAcceptance)) {
		/* Try callback for acceptance list, if success override all existing acceptance */
		uint16_t res = GetIndustryTileCallback(CBID_INDTILE_CARGO_ACCEPTANCE, 0, 0, gfx, Industry::GetByTile(tile), tile);
		if (res != CALLBACK_FAILED) {
			cargo_acceptance.fill(0);
			for (uint i = 0; i < INDUSTRY_ORIGINAL_NUM_INPUTS; i++) cargo_acceptance[i] = GB(res, i * 4, 4);
		}
	}

	for (size_t i = 0; i < std::size(itspec->accepts_cargo); i++) {
		CargoType cargo = accepts_cargo[i];
		if (!IsValidCargoType(cargo) || cargo_acceptance[i] <= 0) continue; // work only with valid cargoes

		/* Add accepted cargo */
		acceptance[cargo] += cargo_acceptance[i];

		/* Maybe set 'always accepted' bit (if it's not set already) */
		if (always_accepted.Test(cargo)) continue;

		/* Test whether the industry itself accepts the cargo type */
		if (ind->IsCargoAccepted(cargo)) continue;

		/* If the industry itself doesn't accept this cargo, set 'always accepted' bit */
		always_accepted.Set(cargo);
	}
}

/** @copydoc GetTileDescProc */
static void GetTileDesc_Industry(TileIndex tile, TileDesc &td)
{
	const Industry *i = Industry::GetByTile(tile);
	const IndustrySpec *is = GetIndustrySpec(i->type);

	td.owner[0] = i->owner;
	td.str = is->name;
	if (!IsIndustryCompleted(tile)) {
		td.dparam = td.str;
		td.str = STR_LAI_TOWN_INDUSTRY_DESCRIPTION_UNDER_CONSTRUCTION;
	}

	if (is->grf_prop.HasGrfFile()) {
		td.grf = GetGRFConfig(is->grf_prop.grfid)->GetName();
	}
}

/** @copydoc ClearTileProc */
static CommandCost ClearTile_Industry(TileIndex tile, DoCommandFlags flags)
{
	Industry *i = Industry::GetByTile(tile);
	const IndustrySpec *indspec = GetIndustrySpec(i->type);

	/* water can destroy industries
	 * in editor you can bulldoze industries
	 * with magic_bulldozer cheat you can destroy industries
	 * (area around OILRIG is water, so water shouldn't flood it
	 */
	if ((_current_company != OWNER_WATER && _game_mode != GameMode::Editor &&
			!_cheats.magic_bulldozer.value) ||
			flags.Test(DoCommandFlag::Auto) ||
			(_current_company == OWNER_WATER &&
				(indspec->behaviour.Test(IndustryBehaviour::BuiltOnWater) ||
				HasBit(GetIndustryTileSpec(GetIndustryGfx(tile))->slopes_refused, 5)))) {

		if (flags.Test(DoCommandFlag::Auto)) {
			return CommandCostWithParam(STR_ERROR_GENERIC_OBJECT_IN_THE_WAY, indspec->name);
		}
		return CommandCost(INVALID_STRING_ID);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		AI::BroadcastNewEvent(new ScriptEventIndustryClose(i->index));
		Game::NewEvent(new ScriptEventIndustryClose(i->index));
		delete i;
	}
	return CommandCost(ExpensesType::Construction, indspec->GetRemovalCost());
}

/**
 * Move produced cargo from industry to nearby stations.
 * @param tile Industry tile
 * @return true if any cargo was moved.
 */
static bool TransportIndustryGoods(TileIndex tile)
{
	Industry *i = Industry::GetByTile(tile);
	const IndustrySpec *indspec = GetIndustrySpec(i->type);
	bool moved_cargo = false;

	for (auto &p : i->produced) {
		uint cw = ClampTo<uint8_t>(p.waiting);
		if (cw > indspec->minimal_cargo && IsValidCargoType(p.cargo)) {
			p.waiting -= cw;

			/* fluctuating economy? */
			if (EconomyIsInRecession()) cw = (cw + 1) / 2;

			p.history[THIS_MONTH].production += cw;

			uint am = MoveGoodsToStation(p.cargo, cw, {i->index, SourceType::Industry}, i->stations_near, i->exclusive_consumer);
			p.history[THIS_MONTH].transported += am;

			moved_cargo |= (am != 0);
		}
	}

	return moved_cargo;
}

static void AnimateSugarSieve(TileIndex tile)
{
	uint8_t m = GetAnimationFrame(tile) + 1;

	if (_settings_client.sound.ambient) {
		switch (m & 7) {
			case 2: SndPlayTileFx(SND_2D_SUGAR_MINE_1, tile); break;
			case 6: SndPlayTileFx(SND_29_SUGAR_MINE_2, tile); break;
		}
	}

	if (m >= 96) {
		m = 0;
		DeleteAnimatedTile(tile);
	}
	SetAnimationFrame(tile, m);

	MarkTileDirtyByTile(tile);
}

static void AnimateToffeeQuarry(TileIndex tile)
{
	uint8_t m = GetAnimationFrame(tile);

	if (_industry_anim_offs_toffee[m] == 0xFF && _settings_client.sound.ambient) {
		SndPlayTileFx(SND_30_TOFFEE_QUARRY, tile);
	}

	if (++m >= 70) {
		m = 0;
		DeleteAnimatedTile(tile);
	}
	SetAnimationFrame(tile, m);

	MarkTileDirtyByTile(tile);
}

static void AnimateBubbleCatcher(TileIndex tile)
{
	uint8_t m = GetAnimationFrame(tile);

	if (++m >= 40) {
		m = 0;
		DeleteAnimatedTile(tile);
	}
	SetAnimationFrame(tile, m);

	MarkTileDirtyByTile(tile);
}

static void AnimatePowerPlantSparks(TileIndex tile)
{
	uint8_t m = GetAnimationFrame(tile);
	if (m == 6) {
		SetAnimationFrame(tile, 0);
		DeleteAnimatedTile(tile);
	} else {
		SetAnimationFrame(tile, m + 1);
	}
	MarkTileDirtyByTile(tile);
}

static void AnimateToyFactory(TileIndex tile)
{
	uint8_t m = GetAnimationFrame(tile) + 1;

	switch (m) {
		case  1: if (_settings_client.sound.ambient) SndPlayTileFx(SND_2C_TOY_FACTORY_1, tile); break;
		case 23: if (_settings_client.sound.ambient) SndPlayTileFx(SND_2B_TOY_FACTORY_2, tile); break;
		case 28: if (_settings_client.sound.ambient) SndPlayTileFx(SND_2A_TOY_FACTORY_3, tile); break;
		default:
			if (m >= 50) {
				int n = GetIndustryAnimationLoop(tile) + 1;
				m = 0;
				if (n >= 8) {
					n = 0;
					DeleteAnimatedTile(tile);
				}
				SetIndustryAnimationLoop(tile, n);
			}
	}

	SetAnimationFrame(tile, m);
	MarkTileDirtyByTile(tile);
}

static void AnimatePlasticFountain(TileIndex tile, IndustryGfx gfx)
{
	gfx = (gfx < GFX_PLASTIC_FOUNTAIN_ANIMATED_8) ? gfx + 1 : GFX_PLASTIC_FOUNTAIN_ANIMATED_1;
	SetIndustryGfx(tile, gfx);
	MarkTileDirtyByTile(tile);
}

static void AnimateOilWell(TileIndex tile, IndustryGfx gfx)
{
	bool b = Chance16(1, 7);
	uint8_t m = GetAnimationFrame(tile) + 1;
	if (m == 4 && (m = 0, ++gfx) == GFX_OILWELL_ANIMATED_3 + 1 && (gfx = GFX_OILWELL_ANIMATED_1, b)) {
		SetIndustryGfx(tile, GFX_OILWELL_NOT_ANIMATED);
		SetIndustryConstructionStage(tile, 3);
		DeleteAnimatedTile(tile);
	} else {
		SetAnimationFrame(tile, m);
		SetIndustryGfx(tile, gfx);
	}
	MarkTileDirtyByTile(tile);
}

static void AnimateMineTower(TileIndex tile)
{
	int state = TimerGameTick::counter & 0x7FF;

	if ((state -= 0x400) < 0) return;

	if (state < 0x1A0) {
		if (state < 0x20 || state >= 0x180) {
			uint8_t m = GetAnimationFrame(tile);
			if (!(m & 0x40)) {
				SetAnimationFrame(tile, m | 0x40);
				if (_settings_client.sound.ambient) SndPlayTileFx(SND_0B_MINE, tile);
			}
			if (state & 7) return;
		} else {
			if (state & 3) return;
		}
		uint8_t m = (GetAnimationFrame(tile) + 1) | 0x40;
		if (m > 0xC2) m = 0xC0;
		SetAnimationFrame(tile, m);
		MarkTileDirtyByTile(tile);
	} else if (state >= 0x200 && state < 0x3A0) {
		int i = (state < 0x220 || state >= 0x380) ? 7 : 3;
		if (state & i) return;

		uint8_t m = (GetAnimationFrame(tile) & 0xBF) - 1;
		if (m < 0x80) m = 0x82;
		SetAnimationFrame(tile, m);
		MarkTileDirtyByTile(tile);
	}
}

/** @copydoc AnimateTileProc */
static void AnimateTile_Industry(TileIndex tile)
{
	IndustryGfx gfx = GetIndustryGfx(tile);

	if (GetIndustryTileSpec(gfx)->animation.status != AnimationStatus::NoAnimation) {
		AnimateNewIndustryTile(tile);
		return;
	}

	switch (gfx) {
	case GFX_HUT_FRONT:
		/* The fireworks over the coffeeshop's hut: drawn anew when the
		 * picture in the sky changes, and the animation stops with them. */
		if (!HutHasFireworks(Industry::GetByTile(tile))) {
			MarkTileDirtyByTile(tile);
			DeleteAnimatedTile(tile);
		} else if (HutFireworkSprite(tile, TimerGameTick::counter) != HutFireworkSprite(tile, TimerGameTick::counter - 1)) {
			MarkTileDirtyByTile(tile);
		}
		break;

	case GFX_SUGAR_MINE_SIEVE:
		if ((TimerGameTick::counter & 1) == 0) AnimateSugarSieve(tile);
		break;

	case GFX_TOFFEE_QUARRY:
		if ((TimerGameTick::counter & 3) == 0) AnimateToffeeQuarry(tile);
		break;

	case GFX_BUBBLE_CATCHER:
		if ((TimerGameTick::counter & 1) == 0) AnimateBubbleCatcher(tile);
		break;

	case GFX_POWERPLANT_SPARKS:
		if ((TimerGameTick::counter & 3) == 0) AnimatePowerPlantSparks(tile);
		break;

	case GFX_TOY_FACTORY:
		if ((TimerGameTick::counter & 1) == 0) AnimateToyFactory(tile);
		break;

	case GFX_PLASTIC_FOUNTAIN_ANIMATED_1: case GFX_PLASTIC_FOUNTAIN_ANIMATED_2:
	case GFX_PLASTIC_FOUNTAIN_ANIMATED_3: case GFX_PLASTIC_FOUNTAIN_ANIMATED_4:
	case GFX_PLASTIC_FOUNTAIN_ANIMATED_5: case GFX_PLASTIC_FOUNTAIN_ANIMATED_6:
	case GFX_PLASTIC_FOUNTAIN_ANIMATED_7: case GFX_PLASTIC_FOUNTAIN_ANIMATED_8:
		if ((TimerGameTick::counter & 3) == 0) AnimatePlasticFountain(tile, gfx);
		break;

	case GFX_OILWELL_ANIMATED_1:
	case GFX_OILWELL_ANIMATED_2:
	case GFX_OILWELL_ANIMATED_3:
		if ((TimerGameTick::counter & 7) == 0) AnimateOilWell(tile, gfx);
		break;

	case GFX_COAL_MINE_TOWER_ANIMATED:
	case GFX_COPPER_MINE_TOWER_ANIMATED:
	case GFX_GOLD_MINE_TOWER_ANIMATED:
		AnimateMineTower(tile);
		break;
	}
}

static void CreateChimneySmoke(TileIndex tile)
{
	uint x = TileX(tile) * TILE_SIZE;
	uint y = TileY(tile) * TILE_SIZE;
	int z = GetTileMaxPixelZ(tile);

	CreateEffectVehicle(x + 15, y + 14, z + 59, EV_CHIMNEY_SMOKE);
}

static void MakeIndustryTileBigger(TileIndex tile)
{
	uint8_t cnt = GetIndustryConstructionCounter(tile) + 1;
	if (cnt != 4) {
		SetIndustryConstructionCounter(tile, cnt);
		return;
	}

	uint8_t stage = GetIndustryConstructionStage(tile) + 1;
	SetIndustryConstructionCounter(tile, 0);
	SetIndustryConstructionStage(tile, stage);
	TriggerIndustryTileAnimation_ConstructionStageChanged(tile, false);
	if (stage == INDUSTRY_COMPLETED) SetIndustryCompleted(tile);

	MarkTileDirtyByTile(tile);

	if (!IsIndustryCompleted(tile)) return;

	IndustryGfx gfx = GetIndustryGfx(tile);
	if (gfx >= NEW_INDUSTRYTILEOFFSET) {
		/* New industries are already animated on construction. */
		return;
	}

	switch (gfx) {
	case GFX_POWERPLANT_CHIMNEY:
		CreateChimneySmoke(tile);
		break;

	case GFX_OILRIG_1: {
		/* Do not require an industry tile to be after the first two GFX_OILRIG_1
		 * tiles (like the default oil rig). Do a proper check to ensure the
		 * tiles belong to the same industry and based on that build the oil rig's
		 * station. */
		TileIndex other = tile + TileDiffXY(0, 1);

		if (IsTileType(other, TileType::Industry) &&
				GetIndustryGfx(other) == GFX_OILRIG_1 &&
				GetIndustryIndex(tile) == GetIndustryIndex(other)) {
			BuildOilRig(tile);
		}
		break;
	}

	case GFX_TOY_FACTORY:
	case GFX_BUBBLE_CATCHER:
	case GFX_TOFFEE_QUARRY:
		SetAnimationFrame(tile, 0);
		SetIndustryAnimationLoop(tile, 0);
		break;

	case GFX_PLASTIC_FOUNTAIN_ANIMATED_1: case GFX_PLASTIC_FOUNTAIN_ANIMATED_2:
	case GFX_PLASTIC_FOUNTAIN_ANIMATED_3: case GFX_PLASTIC_FOUNTAIN_ANIMATED_4:
	case GFX_PLASTIC_FOUNTAIN_ANIMATED_5: case GFX_PLASTIC_FOUNTAIN_ANIMATED_6:
	case GFX_PLASTIC_FOUNTAIN_ANIMATED_7: case GFX_PLASTIC_FOUNTAIN_ANIMATED_8:
		AddAnimatedTile(tile, false);
		break;
	}
}

static void TileLoopIndustry_BubbleGenerator(TileIndex tile)
{
	static const int8_t _bubble_spawn_location[3][4] = {
		{ 11,   0, -4, -14 },
		{ -4, -10, -4,   1 },
		{ 49,  59, 60,  65 },
	};

	if (_settings_client.sound.ambient) SndPlayTileFx(SND_2E_BUBBLE_GENERATOR, tile);

	int dir = Random() & 3;

	EffectVehicle *v = CreateEffectVehicleAbove(
		TileX(tile) * TILE_SIZE + _bubble_spawn_location[0][dir],
		TileY(tile) * TILE_SIZE + _bubble_spawn_location[1][dir],
		_bubble_spawn_location[2][dir],
		EV_BUBBLE
	);

	if (v != nullptr) v->animation_substate = dir;
}

/** @copydoc TileLoopProc */
static void TileLoop_Industry(TileIndex tile)
{
	if (IsTileOnWater(tile)) TileLoop_Water(tile);

	/* Normally this doesn't happen, but if an industry NewGRF is removed
	 * an industry that was previously build on water can now be flooded.
	 * If this happens the tile is no longer an industry tile after
	 * returning from TileLoop_Water. */
	if (!IsTileType(tile, TileType::Industry)) return;

	TriggerIndustryTileRandomisation(tile, IndustryRandomTrigger::TileLoop);

	if (!IsIndustryCompleted(tile)) {
		MakeIndustryTileBigger(tile);
		return;
	}

	/* The girls leave and the plants grow without anything telling the tile:
	 * it is drawn anew every round (GameOwnIndustrySprite()). */
	if (GameOwnIndustryDrawTile(GetIndustryGfx(tile), INDUSTRY_COMPLETED) != nullptr) MarkTileDirtyByTile(tile);
	/* The fireworks over the coffeeshop's hut start here, at most a tile
	 * loop after the delivery that lights them; AnimateTile_Industry() stops
	 * them when they are over. */
	if (GetIndustryGfx(tile) == GFX_HUT_FRONT && HutHasFireworks(Industry::GetByTile(tile))) AddAnimatedTile(tile, false);

	if (_game_mode == GameMode::Editor) return;

	if (TransportIndustryGoods(tile) && !TriggerIndustryAnimation(Industry::GetByTile(tile), IndustryAnimationTrigger::CargoDistributed)) {
		uint newgfx = GetIndustryTileSpec(GetIndustryGfx(tile))->anim_production;

		if (newgfx != INDUSTRYTILE_NOANIM) {
			ResetIndustryConstructionStage(tile);
			SetIndustryCompleted(tile);
			SetIndustryGfx(tile, newgfx);
			MarkTileDirtyByTile(tile);
			return;
		}
	}

	if (TriggerIndustryTileAnimation(tile, IndustryAnimationTrigger::TileLoop)) return;

	IndustryGfx newgfx = GetIndustryTileSpec(GetIndustryGfx(tile))->anim_next;
	if (newgfx != INDUSTRYTILE_NOANIM) {
		ResetIndustryConstructionStage(tile);
		SetIndustryGfx(tile, newgfx);
		MarkTileDirtyByTile(tile);
		return;
	}

	IndustryGfx gfx = GetIndustryGfx(tile);
	switch (gfx) {
	case GFX_COAL_MINE_TOWER_NOT_ANIMATED:
	case GFX_COPPER_MINE_TOWER_NOT_ANIMATED:
	case GFX_GOLD_MINE_TOWER_NOT_ANIMATED:
		if (!(TimerGameTick::counter & 0x400) && Chance16(1, 2)) {
			switch (gfx) {
				case GFX_COAL_MINE_TOWER_NOT_ANIMATED:   gfx = GFX_COAL_MINE_TOWER_ANIMATED;   break;
				case GFX_COPPER_MINE_TOWER_NOT_ANIMATED: gfx = GFX_COPPER_MINE_TOWER_ANIMATED; break;
				case GFX_GOLD_MINE_TOWER_NOT_ANIMATED:   gfx = GFX_GOLD_MINE_TOWER_ANIMATED;   break;
			}
			SetIndustryGfx(tile, gfx);
			SetAnimationFrame(tile, 0x80);
			AddAnimatedTile(tile);
		}
		break;

	case GFX_OILWELL_NOT_ANIMATED:
		if (Chance16(1, 6)) {
			SetIndustryGfx(tile, GFX_OILWELL_ANIMATED_1);
			SetAnimationFrame(tile, 0);
			AddAnimatedTile(tile);
		}
		break;

	case GFX_COAL_MINE_TOWER_ANIMATED:
	case GFX_COPPER_MINE_TOWER_ANIMATED:
	case GFX_GOLD_MINE_TOWER_ANIMATED:
		if (!(TimerGameTick::counter & 0x400)) {
			switch (gfx) {
				case GFX_COAL_MINE_TOWER_ANIMATED:   gfx = GFX_COAL_MINE_TOWER_NOT_ANIMATED;   break;
				case GFX_COPPER_MINE_TOWER_ANIMATED: gfx = GFX_COPPER_MINE_TOWER_NOT_ANIMATED; break;
				case GFX_GOLD_MINE_TOWER_ANIMATED:   gfx = GFX_GOLD_MINE_TOWER_NOT_ANIMATED;   break;
			}
			SetIndustryGfx(tile, gfx);
			SetIndustryCompleted(tile);
			SetIndustryConstructionStage(tile, 3);
			DeleteAnimatedTile(tile);
			MarkTileDirtyByTile(tile);
		}
		break;

	case GFX_POWERPLANT_SPARKS:
		if (Chance16(1, 3)) {
			if (_settings_client.sound.ambient) SndPlayTileFx(SND_0C_POWER_STATION, tile);
			AddAnimatedTile(tile);
		}
		break;

	case GFX_COPPER_MINE_CHIMNEY:
		CreateEffectVehicleAbove(TileX(tile) * TILE_SIZE + 6, TileY(tile) * TILE_SIZE + 6, 43, EV_COPPER_MINE_SMOKE);
		break;


	case GFX_TOY_FACTORY: {
			Industry *i = Industry::GetByTile(tile);
			if (i->was_cargo_delivered) {
				i->was_cargo_delivered = false;
				SetIndustryAnimationLoop(tile, 0);
				AddAnimatedTile(tile);
			}
		}
		break;

	case GFX_BUBBLE_GENERATOR:
		TileLoopIndustry_BubbleGenerator(tile);
		break;

	case GFX_TOFFEE_QUARRY:
		AddAnimatedTile(tile);
		break;

	case GFX_SUGAR_MINE_SIEVE:
		if (Chance16(1, 3)) AddAnimatedTile(tile);
		break;
	}
}

/** @copydoc ClickTileProc */
static bool ClickTile_Industry(TileIndex tile)
{
	ShowIndustryViewWindow(GetIndustryIndex(tile));
	return true;
}

/** @copydoc ChangeTileOwnerProc */
static void ChangeTileOwner_Industry(TileIndex tile, Owner old_owner, Owner new_owner)
{
	/* If the founder merges, the industry was created by the merged company */
	Industry *i = Industry::GetByTile(tile);
	if (i->founder == old_owner) i->founder = (new_owner == INVALID_OWNER) ? OWNER_NONE : new_owner;

	if (i->exclusive_supplier == old_owner) i->exclusive_supplier = new_owner;
	if (i->exclusive_consumer == old_owner) i->exclusive_consumer = new_owner;
}

/**
 * Check whether the tile is a forest.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a forest
 */
bool IsTileForestIndustry(TileIndex tile)
{
	/* Check for industry tile */
	if (!IsTileType(tile, TileType::Industry)) return false;

	const Industry *ind = Industry::GetByTile(tile);

	/* Check for organic industry (i.e. not processing or extractive) */
	if (!GetIndustrySpec(ind->type)->life_type.Test(IndustryLifeType::Organic)) return false;

	/* Check for wood production */
	return std::any_of(std::begin(ind->produced), std::end(ind->produced), [](const auto &p) { return IsValidCargoType(p.cargo) && CargoSpec::Get(p.cargo)->label == CT_WOOD; });
}

static const uint8_t _plantfarmfield_type[] = {1, 1, 1, 1, 1, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6};

/**
 * Check whether the tile can be replaced by a farm field.
 * @param tile The tile to investigate.
 * @param allow_fields Can we replace an existing field?
 * @param allow_rough Can we build on rough tiles? (clear or trees)
 * @return true if the tile can become a farm field
 */
static bool IsSuitableForFarmField(TileIndex tile, bool allow_fields, bool allow_rough)
{
	switch (GetTileType(tile)) {
		case TileType::Clear:
			if (IsSnowTile(tile)) return false;
			switch (GetClearGround(tile)) {
				case ClearGround::Desert: return false;
				case ClearGround::Rough: return allow_rough;
				case ClearGround::Fields: return allow_fields;
				default: return true;
			}
		case TileType::Trees: return GetTreeGround(tile) != TreeGround::Shore && (allow_rough || GetTreeGround(tile) != TreeGround::Rough);
		default:       return false;
	}
}

/**
 * Build farm field fence
 * @param tile the tile to position the fence on
 * @param size the size of the field being planted in tiles
 * @param type type of fence to set
 * @param side the side of the tile to attempt placement
 */
static void SetupFarmFieldFence(TileIndex tile, int size, uint8_t type, DiagDirection side)
{
	TileIndexDiff diff = TileOffsByAxis(OtherAxis(DiagDirToAxis(side)));
	TileIndexDiff neighbour_diff = TileOffsByDiagDir(side);

	do {
		tile = Map::WrapToMap(tile);

		if (IsTileType(tile, TileType::Clear) && IsClearGround(tile, ClearGround::Fields)) {
			TileIndex neighbour = tile + neighbour_diff;
			if (!IsTileType(neighbour, TileType::Clear) || !IsClearGround(neighbour, ClearGround::Fields) || GetFence(neighbour, ReverseDiagDir(side)) == 0) {
				/* Add fence as long as neighbouring tile does not already have a fence in the same position. */
				uint8_t or_ = type;

				if (or_ == 1 && Chance16(1, 7)) or_ = 2;

				SetFence(tile, side, or_);
			}
		}

		tile += diff;
	} while (--size);
}

static void PlantFarmField(TileIndex tile, IndustryID industry)
{
	if (_settings_game.game_creation.landscape == LandscapeType::Arctic) {
		if (GetTileZ(tile) + 2 >= GetSnowLine()) return;
	}

	/* determine field size */
	uint32_t r = (Random() & 0x303) + 0x404;
	if (_settings_game.game_creation.landscape == LandscapeType::Arctic) r += 0x404;
	uint size_x = GB(r, 0, 8);
	uint size_y = GB(r, 8, 8);

	TileArea ta(tile - TileDiffXY(std::min(TileX(tile), size_x / 2), std::min(TileY(tile), size_y / 2)), size_x, size_y);
	ta.ClampToMap();

	if (ta.w == 0 || ta.h == 0) return;

	/* check the amount of bad tiles */
	int count = 0;
	for (TileIndex cur_tile : ta) {
		assert(cur_tile < Map::Size());
		count += IsSuitableForFarmField(cur_tile, false, false);
	}
	if (count * 2 < ta.w * ta.h) return;

	/* determine type of field */
	r = Random();
	uint counter = GB(r, 5, 3);
	uint field_type = GB(r, 8, 8) * 9 >> 8;

	/* make field */
	for (TileIndex cur_tile : ta) {
		assert(cur_tile < Map::Size());
		if (IsSuitableForFarmField(cur_tile, true, true)) {
			MakeField(cur_tile, field_type, industry);
			SetClearCounter(cur_tile, counter);
			MarkTileDirtyByTile(cur_tile);
		}
	}

	int type = 3;
	if (_settings_game.game_creation.landscape != LandscapeType::Arctic && _settings_game.game_creation.landscape != LandscapeType::Tropic) {
		type = _plantfarmfield_type[Random() & 0xF];
	}

	SetupFarmFieldFence(ta.tile, ta.h, type, DiagDirection::NE);
	SetupFarmFieldFence(ta.tile, ta.w, type, DiagDirection::NW);
	SetupFarmFieldFence(ta.tile + TileDiffXY(ta.w - 1, 0), ta.h, type, DiagDirection::SW);
	SetupFarmFieldFence(ta.tile + TileDiffXY(0, ta.h - 1), ta.w, type, DiagDirection::SE);
}

void PlantRandomFarmField(const Industry *i)
{
	int x = i->location.w / 2 + Random() % 31 - 16;
	int y = i->location.h / 2 + Random() % 31 - 16;

	TileIndex tile = TileAddWrap(i->location.tile, x, y);

	if (tile != INVALID_TILE) PlantFarmField(tile, i->index);
}

/**
 * Perform a circular search around the Lumber Mill in order to find trees to cut
 * @param i industry
 */
static void ChopLumberMillTrees(Industry *i)
{
	/* Don't process lumber mill if cargo is not set up correctly. */
	auto itp = std::begin(i->produced);
	if (itp == std::end(i->produced) || !IsValidCargoType(itp->cargo)) return;

	/* We only want to cut trees if all tiles are completed. */
	for (TileIndex tile_cur : i->location) {
		if (i->TileBelongsToIndustry(tile_cur)) {
			if (!IsIndustryCompleted(tile_cur)) return;
		}
	}

	for (auto tile : SpiralTileSequence(i->location.tile, 40)) { // 40x40 tiles  to search.
		if (!IsTileType(tile, TileType::Trees) || GetTreeGrowth(tile) < TreeGrowthStage::Grown) continue;

		/* found a tree */
		_industry_sound_ctr = 1;
		_industry_sound_tile = tile;
		if (_settings_client.sound.ambient) SndPlayTileFx(SND_38_LUMBER_MILL_1, tile);

		AutoRestoreBackup cur_company(_current_company, OWNER_NONE);
		Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, tile);

		/* Add according value to waiting cargo. */
		itp->waiting = ClampTo<uint16_t>(itp->waiting + ScaleByCargoScale(45, false));
		break;
	}
}

/**
 * Helper for ProduceIndustryGoods that scales and produces cargos.
 * @param i The industry
 * @param scale Should we scale production of this cargo directly?
 */
static void ProduceIndustryGoodsHelper(Industry *i, bool scale)
{
	for (auto &p : i->produced) {
		if (!IsValidCargoType(p.cargo)) continue;

		uint16_t amount = p.rate;
		if (scale) amount = ScaleByCargoScale(amount, false);

		p.waiting = ClampTo<uint16_t>(p.waiting + amount);
	}
}

static void ProduceIndustryGoods(Industry *i)
{
	const IndustrySpec *indsp = GetIndustrySpec(i->type);

	/* play a sound? */
	if ((i->counter & 0x3F) == 0) {
		uint32_t r;
		if (Chance16R(1, 14, r) && !indsp->random_sounds.empty() && _settings_client.sound.ambient) {
			if (std::any_of(std::begin(i->produced), std::end(i->produced), [](const auto &p) { return p.history[LAST_MONTH].production > 0; })) {
				/* Play sound since last month had production */
				SndPlayTileFx(
					static_cast<SoundFx>(indsp->random_sounds[((r >> 16) * indsp->random_sounds.size()) >> 16]),
					i->location.tile);
			}
		}
	}

	i->counter--;

	/* If using an industry callback, scale the callback interval by cargo scale percentage. */
	if (indsp->callback_mask.Test(IndustryCallbackMask::Production256Ticks)) {
		if (i->counter % ScaleByInverseCargoScale(Ticks::INDUSTRY_PRODUCE_TICKS, false) == 0) {
			IndustryProductionCallback(i, 1);
			ProduceIndustryGoodsHelper(i, false);
		}
	}

	/*
	 * All other production and special effects happen every 256 ticks, and cargo production is just scaled by the cargo scale percentage.
	 * This keeps a slow trickle of production to avoid confusion at low scale factors when the industry seems to be doing nothing for a long period of time.
	 */
	if ((i->counter % Ticks::INDUSTRY_PRODUCE_TICKS) == 0) {
		/* Handle non-callback cargo production. A bare marijuana plantation
		 * -- no girls have tended it, or none for half a year -- yields
		 * nothing (MarijuanaPlantationStage()). */
		bool bare_plantation = i->type == IT_MARIJUANA_PLANTATION && MarijuanaPlantationStage(i) == PlantationStage::Bare;
		if (!indsp->callback_mask.Test(IndustryCallbackMask::Production256Ticks) && !bare_plantation) ProduceIndustryGoodsHelper(i, true);

		IndustryBehaviours indbehav = indsp->behaviour;

		if (indbehav.Test(IndustryBehaviour::PlantFields)) {
			uint16_t cb_res = CALLBACK_FAILED;
			if (indsp->callback_mask.Test(IndustryCallbackMask::SpecialEffect)) {
				cb_res = GetIndustryCallback(CBID_INDUSTRY_SPECIAL_EFFECT, Random(), 0, i, i->type, i->location.tile);
			}

			bool plant;
			if (cb_res != CALLBACK_FAILED) {
				plant = ConvertBooleanCallback(indsp->grf_prop.grffile, CBID_INDUSTRY_SPECIAL_EFFECT, cb_res);
			} else {
				plant = Chance16(1, 8);
			}

			if (plant) PlantRandomFarmField(i);
		}
		if (indbehav.Test(IndustryBehaviour::CutTrees)) {
			uint16_t cb_res = CALLBACK_FAILED;
			if (indsp->callback_mask.Test(IndustryCallbackMask::SpecialEffect)) {
				cb_res = GetIndustryCallback(CBID_INDUSTRY_SPECIAL_EFFECT, Random(), 1, i, i->type, i->location.tile);
			}

			bool cut;
			if (cb_res != CALLBACK_FAILED) {
				cut = ConvertBooleanCallback(indsp->grf_prop.grffile, CBID_INDUSTRY_SPECIAL_EFFECT, cb_res);
			} else {
				cut = ((i->counter % Ticks::INDUSTRY_CUT_TREE_TICKS) == 0);
			}

			if (cut) ChopLumberMillTrees(i);
		}

		TriggerIndustryRandomisation(i, IndustryRandomTrigger::IndustryTick);
		TriggerIndustryAnimation(i, IndustryAnimationTrigger::IndustryTick);
	}
}

void OnTick_Industry()
{
	if (_industry_sound_ctr != 0) {
		_industry_sound_ctr++;

		if (_industry_sound_ctr == 75) {
			if (_settings_client.sound.ambient) SndPlayTileFx(SND_37_LUMBER_MILL_2, _industry_sound_tile);
		} else if (_industry_sound_ctr == 160) {
			_industry_sound_ctr = 0;
			if (_settings_client.sound.ambient) SndPlayTileFx(SND_36_LUMBER_MILL_3, _industry_sound_tile);
		}
	}

	if (_game_mode == GameMode::Editor) return;

	for (Industry *i : Industry::Iterate()) {
		ProduceIndustryGoods(i);

		if ((TimerGameTick::counter + i->index) % Ticks::DAY_TICKS == 0) {
			for (auto &a : i->accepted) a.accumulated_waiting += a.waiting;
		}
	}
}

/**
 * Check the conditions of #IndustryCheck::None (Always succeeds).
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_None(TileIndex)
{
	return CommandCost();
}

/**
 * Check the conditions of #IndustryCheck::Forest (Industry should be build above snow-line in arctic climate).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_Forest(TileIndex tile)
{
	if (_settings_game.game_creation.landscape == LandscapeType::Arctic) {
		if (GetTileZ(tile) < HighestSnowLine() + 2) {
			return CommandCost(STR_ERROR_FOREST_CAN_ONLY_BE_PLANTED);
		}
	}
	return CommandCost();
}

/**
 * Check if a tile is within a distance from map edges, scaled by map dimensions independently.
 * Each dimension is checked independently, and dimensions smaller than 256 are not scaled.
 * @param tile Which tile to check distance of.
 * @param maxdist Normal distance on a 256x256 map.
 * @return True if the tile is near the map edge.
 */
static bool CheckScaledDistanceFromEdge(TileIndex tile, uint maxdist)
{
	uint maxdist_x = maxdist;
	uint maxdist_y = maxdist;

	if (Map::SizeX() > 256) maxdist_x *= Map::SizeX() / 256;
	if (Map::SizeY() > 256) maxdist_y *= Map::SizeY() / 256;

	if (DistanceFromEdgeDir(tile, DiagDirection::NE) < maxdist_x) return true;
	if (DistanceFromEdgeDir(tile, DiagDirection::NW) < maxdist_y) return true;
	if (DistanceFromEdgeDir(tile, DiagDirection::SW) < maxdist_x) return true;
	if (DistanceFromEdgeDir(tile, DiagDirection::SE) < maxdist_y) return true;

	return false;
}

/**
 * Check the conditions of #IndustryCheck::Refinery (Industry should be positioned near edge of the map).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_OilRefinery(TileIndex tile)
{
	if (_game_mode == GameMode::Editor) return CommandCost();

	if (CheckScaledDistanceFromEdge(TileAddXY(tile, 1, 1), _settings_game.game_creation.oil_refinery_limit)) return CommandCost();

	return CommandCost(STR_ERROR_CAN_ONLY_BE_POSITIONED);
}

extern bool _ignore_industry_restrictions;

/**
 * Check the conditions of #IndustryCheck::OilRig (Industries at sea should be positioned near edge of the map).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_OilRig(TileIndex tile)
{
	if (_game_mode == GameMode::Editor && _ignore_industry_restrictions) return CommandCost();

	if (TileHeight(tile) == 0 &&
			CheckScaledDistanceFromEdge(TileAddXY(tile, 1, 1), _settings_game.game_creation.oil_refinery_limit)) return CommandCost();

	return CommandCost(STR_ERROR_CAN_ONLY_BE_POSITIONED);
}

/**
 * Check the conditions of #IndustryCheck::Farm (Industry should be below snow-line in arctic).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_Farm(TileIndex tile)
{
	if (_settings_game.game_creation.landscape == LandscapeType::Arctic) {
		if (GetTileZ(tile) + 2 >= HighestSnowLine()) {
			return CommandCost(STR_ERROR_SITE_UNSUITABLE);
		}
	}
	return CommandCost();
}

/**
 * Check the conditions of #IndustryCheck::Plantation (Industry should NOT be in the desert).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_Plantation(TileIndex tile)
{
	if (GetTropicZone(tile) == TropicZone::Desert) {
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}
	return CommandCost();
}

/**
 * Check the conditions of #IndustryCheck::Water (Industry should be in the desert).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_Water(TileIndex tile)
{
	/* Outside the desert climate there is no desert to ask for: a desert
	 * industry switched on in another climate (climate_industries.h) stands
	 * wherever the rest may. */
	if (_settings_game.game_creation.landscape != LandscapeType::Tropic) return CommandCost();
	if (GetTropicZone(tile) != TropicZone::Desert) {
		return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_IN_DESERT);
	}
	return CommandCost();
}

/**
 * Check the conditions of #IndustryCheck::Lumbermill (Industry should be in the rainforest).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_Lumbermill(TileIndex tile)
{
	/* No rainforest outside the desert climate either; see CheckNewIndustry_Water(). */
	if (_settings_game.game_creation.landscape != LandscapeType::Tropic) return CommandCost();
	if (GetTropicZone(tile) != TropicZone::Rainforest) {
		return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_IN_RAINFOREST);
	}
	return CommandCost();
}

/**
 * Check the conditions of #IndustryCheck::BubbleGen (Industry should be in low land).
 * @param tile %Tile to perform the checking.
 * @return Succeeded or failed command.
 */
static CommandCost CheckNewIndustry_BubbleGen(TileIndex tile)
{
	if (GetTileZ(tile) > 4) {
		return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_IN_LOW_AREAS);
	}
	return CommandCost();
}

/**
 * Industrytype check function signature.
 * @param tile %Tile to check.
 * @return Succeeded or failed command.
 */
typedef CommandCost CheckNewIndustryProc(TileIndex tile);

/** Check functions for different types of industry. */
static constexpr EnumIndexArray<CheckNewIndustryProc * const, IndustryCheck, IndustryCheck::End> _check_new_industry_procs = {
	CheckNewIndustry_None, // IndustryCheck::None
	CheckNewIndustry_Forest, // IndustryCheck::Forest
	CheckNewIndustry_OilRefinery, // IndustryCheck::Refinery
	CheckNewIndustry_Farm, // IndustryCheck::Farm
	CheckNewIndustry_Plantation, // IndustryCheck::Plantation
	CheckNewIndustry_Water, // IndustryCheck::Water
	CheckNewIndustry_Lumbermill, // IndustryCheck::Lumbermill
	CheckNewIndustry_BubbleGen, // IndustryCheck::BubbleGen
	CheckNewIndustry_OilRig, // IndustryCheck::OilRig
};

/**
 * Find a town for the industry, while checking for multiple industries in the same town.
 * @param tile Position of the industry to build.
 * @param type Industry type.
 * @param[out] t Pointer to return town for the new industry, \c nullptr is written if no good town can be found.
 * @return Succeeded or failed command.
 *
 * @pre \c *t != nullptr
 * @post \c *t points to a town on success, and \c nullptr on failure.
 */
static CommandCost FindTownForIndustry(TileIndex tile, IndustryType type, Town **t)
{
	*t = ClosestTownFromTile(tile, UINT_MAX);

	if (_settings_game.economy.multiple_industry_per_town) return CommandCost();

	for (const IndustryID &industry : Industry::industries[type]) {
		if (Industry::Get(industry)->town == *t) {
			*t = nullptr;
			return CommandCost(STR_ERROR_ONLY_ONE_ALLOWED_PER_TOWN);
		}
	}

	return CommandCost();
}

bool IsSlopeRefused(Slope current, Slope refused)
{
	if (IsSteepSlope(current)) return true;
	if (current != SLOPE_FLAT) {
		if (IsSteepSlope(refused)) return true;

		Slope t = ComplementSlope(current);

		if ((refused & SLOPE_W) && (t & SLOPE_NW)) return true;
		if ((refused & SLOPE_S) && (t & SLOPE_NE)) return true;
		if ((refused & SLOPE_E) && (t & SLOPE_SW)) return true;
		if ((refused & SLOPE_N) && (t & SLOPE_SE)) return true;
	}

	return false;
}

/**
 * Are the tiles of the industry free?
 * @param tile                    Position to check.
 * @param layout                  Industry tiles table.
 * @param type                    Type of the industry.
 * @return Failed or succeeded command.
 */
static CommandCost CheckIfIndustryTilesAreFree(TileIndex tile, const IndustryTileLayout &layout, IndustryType type)
{
	IndustryBehaviours ind_behav = GetIndustrySpec(type)->behaviour;

	for (const IndustryTileLayoutTile &it : layout) {
		IndustryGfx gfx = GetTranslatedIndustryTileID(it.gfx);
		TileIndex cur_tile = TileAddWrap(tile, it.ti.x, it.ti.y);

		if (!IsValidTile(cur_tile)) {
			return CommandCost(STR_ERROR_SITE_UNSUITABLE);
		}

		if (gfx == GFX_WATERTILE_SPECIALCHECK) {
			if (!IsWaterTile(cur_tile) ||
					!IsTileFlat(cur_tile)) {
				return CommandCost(STR_ERROR_SITE_UNSUITABLE);
			}
		} else {
			CommandCost ret = EnsureNoVehicleOnGround(cur_tile);
			if (ret.Failed()) return ret;
			if (IsBridgeAbove(cur_tile)) return CommandCost(STR_ERROR_SITE_UNSUITABLE);

			const IndustryTileSpec *its = GetIndustryTileSpec(gfx);

			/* Perform land/water check if not disabled */
			if (!HasBit(its->slopes_refused, 5) && ((HasTileWaterClass(cur_tile) && IsTileOnWater(cur_tile)) != ind_behav.Test(IndustryBehaviour::BuiltOnWater))) return CommandCost(STR_ERROR_SITE_UNSUITABLE);

			if (ind_behav.Any({IndustryBehaviour::OnlyInTown, IndustryBehaviour::Town1200More}) || // Tile must be a house
					(ind_behav.Test(IndustryBehaviour::OnlyNearTown) && IsTileType(cur_tile, TileType::House))) { // Tile is allowed to be a house (and it is a house)
				if (!IsTileType(cur_tile, TileType::House)) {
					return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_IN_TOWNS);
				}

				/* Clear the tiles as OWNER_TOWN to not affect town rating, and to not clear protected buildings */
				AutoRestoreBackup cur_company(_current_company, OWNER_TOWN);
				ret = Command<Commands::LandscapeClear>::Do({}, cur_tile);

				if (ret.Failed()) return ret;
			} else {
				/* Clear the tiles, but do not affect town ratings */
				ret = Command<Commands::LandscapeClear>::Do({DoCommandFlag::Auto, DoCommandFlag::NoTestTownRating, DoCommandFlag::NoModifyTownRating}, cur_tile);
				if (ret.Failed()) return ret;
			}
		}
	}

	return CommandCost();
}

/**
 * Check slope requirements for industry tiles.
 * @param tile                    Position to check.
 * @param layout                  Industry tiles table.
 * @param layout_index            The index of the layout to build/fund
 * @param type                    Type of the industry.
 * @param initial_random_bits     The random bits the industry is going to have after construction.
 * @param founder                 Industry founder
 * @param creation_type           The circumstances the industry is created under.
 * @param[out] custom_shape_check Perform custom check for the site.
 * @return Failed or succeeded command.
 */
static CommandCost CheckIfIndustryTileSlopes(TileIndex tile, const IndustryTileLayout &layout, size_t layout_index, IndustryType type, uint16_t initial_random_bits, Owner founder, IndustryAvailabilityCallType creation_type, bool *custom_shape_check = nullptr)
{
	bool refused_slope = false;
	bool custom_shape = false;

	for (const IndustryTileLayoutTile &it : layout) {
		IndustryGfx gfx = GetTranslatedIndustryTileID(it.gfx);
		TileIndex cur_tile = TileAddWrap(tile, it.ti.x, it.ti.y);
		assert(IsValidTile(cur_tile)); // checked before in CheckIfIndustryTilesAreFree

		if (gfx != GFX_WATERTILE_SPECIALCHECK) {
			const IndustryTileSpec *its = GetIndustryTileSpec(gfx);

			if (its->callback_mask.Test(IndustryTileCallbackMask::ShapeCheck)) {
				custom_shape = true;
				CommandCost ret = PerformIndustryTileSlopeCheck(tile, cur_tile, its, type, gfx, layout_index, initial_random_bits, founder, creation_type);
				if (ret.Failed()) return ret;
			} else {
				Slope tileh = GetTileSlope(cur_tile);
				refused_slope |= IsSlopeRefused(tileh, its->slopes_refused);
			}
		}
	}

	if (custom_shape_check != nullptr) *custom_shape_check = custom_shape;

	/* It is almost impossible to have a fully flat land in TG, so what we
	 *  do is that we check if we can make the land flat later on. See
	 *  CheckIfCanLevelIndustryPlatform(). */
	if (!refused_slope || (_settings_game.game_creation.land_generator == LG_TERRAGENESIS && _generating_world && !custom_shape && !_ignore_industry_restrictions)) {
		return CommandCost();
	}
	return CommandCost(STR_ERROR_SITE_UNSUITABLE);
}

/**
 * Is the industry allowed to be built at this place for the town?
 * @param tile Tile to construct the industry.
 * @param type Type of the industry.
 * @param t    Town authority that the industry belongs to.
 * @return Succeeded or failed command.
 */
/** How far, at most, the vending machine stands from a girls' grammar school, in tiles: the player's word, so the girls do not have far. */
static const uint WEED_MACHINE_MAX_DISTANCE = 2;
/** How near, at least, the coffeeshop, a girls' grammar school and a statue of Karel Macha stand to one another, in tiles: they do the same thing, so they stay apart. */
static const uint COFFEESHOP_GYMNASIUM_MIN_DISTANCE = 10;

/**
 * How many tiles an area is from the nearest industry of a type: the larger
 * of the gaps along x and y between the nearest tiles, 0 for touching.
 * @param area the area
 * @param type the industry type
 * @return the distance, or UINT_MAX when there is none of the type
 */
uint DistanceToIndustryType(const TileArea &area, IndustryType type)
{
	uint best = UINT_MAX;
	uint ax0 = TileX(area.tile);
	uint ay0 = TileY(area.tile);
	uint ax1 = ax0 + area.w - 1;
	uint ay1 = ay0 + area.h - 1;
	for (IndustryID id : Industry::industries[type]) {
		const TileArea &other = Industry::Get(id)->location;
		uint bx0 = TileX(other.tile);
		uint by0 = TileY(other.tile);
		uint bx1 = bx0 + other.w - 1;
		uint by1 = by0 + other.h - 1;
		uint dx = ax1 < bx0 ? bx0 - ax1 : (bx1 < ax0 ? ax0 - bx1 : 0);
		uint dy = ay1 < by0 ? by0 - ay1 : (by1 < ay0 ? ay0 - by1 : 0);
		best = std::min(best, std::max(dx, dy));
	}
	return best;
}

/**
 * Is a tile at most WEED_MACHINE_MAX_DISTANCE tiles from a girls' grammar
 * school, counting from the nearest of its tiles?
 * @param tile the tile
 * @return whether a school is that near
 */
bool IsNearGymnasium(TileIndex tile)
{
	return DistanceToIndustryType(TileArea(tile, 1, 1), IT_GYMNASIUM) <= WEED_MACHINE_MAX_DISTANCE;
}

/**
 * A tile where a vending machine may stand: at most
 * WEED_MACHINE_MAX_DISTANCE tiles from a girls' grammar school picked at
 * random. Random tiles of the whole map would almost never hit one.
 * @return the tile, or INVALID_TILE when there is no school or it is off the map
 */
static TileIndex RandomTileNearGymnasium()
{
	const auto &schools = Industry::industries[IT_GYMNASIUM];
	if (schools.empty()) return INVALID_TILE;
	auto it = std::next(schools.begin(), RandomRange(static_cast<uint32_t>(schools.size())));
	const TileArea &area = Industry::Get(*it)->location;
	int span = WEED_MACHINE_MAX_DISTANCE * 2;
	int x = static_cast<int>(TileX(area.tile)) - static_cast<int>(WEED_MACHINE_MAX_DISTANCE) + static_cast<int>(RandomRange(area.w + span));
	int y = static_cast<int>(TileY(area.tile)) - static_cast<int>(WEED_MACHINE_MAX_DISTANCE) + static_cast<int>(RandomRange(area.h + span));
	if (x < 1 || y < 1 || x >= static_cast<int>(Map::MaxX()) || y >= static_cast<int>(Map::MaxY())) return INVALID_TILE;
	return TileXY(x, y);
}

static CommandCost CheckIfIndustryIsAllowed(TileIndex tile, IndustryType type, const Town *t)
{
	if (type == IT_WEED_MACHINE && !IsNearGymnasium(tile)) {
		return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_NEAR_GYMNASIUM);
	}
	/* The coffeeshop, the school and the statue keep apart, each from the
	 * other two: they do the same thing. The plantation keeps the same
	 * distance from all of them and from the vending machine, the player's
	 * word, and they from it. */
	const TileArea area(tile, type == IT_GYMNASIUM ? 2 : type == IT_MARIJUANA_PLANTATION ? PLANTATION_WIDTH : 1,
			type == IT_GYMNASIUM ? 2 : type == IT_MARIJUANA_PLANTATION ? PLANTATION_HEIGHT : 1);
	if (type == IT_COFFEESHOP || type == IT_GYMNASIUM || type == IT_STATUE || type == IT_MARIJUANA_PLANTATION) {
		if (type != IT_GYMNASIUM && DistanceToIndustryType(area, IT_GYMNASIUM) < COFFEESHOP_GYMNASIUM_MIN_DISTANCE) {
			return CommandCost(STR_ERROR_TOO_CLOSE_TO_GYMNASIUM);
		}
		if (type != IT_COFFEESHOP && DistanceToIndustryType(area, IT_COFFEESHOP) < COFFEESHOP_GYMNASIUM_MIN_DISTANCE) {
			return CommandCost(STR_ERROR_TOO_CLOSE_TO_COFFEESHOP);
		}
		if (type != IT_STATUE && DistanceToIndustryType(area, IT_STATUE) < COFFEESHOP_GYMNASIUM_MIN_DISTANCE) {
			return CommandCost(STR_ERROR_TOO_CLOSE_TO_STATUE);
		}
	}
	if (type == IT_MARIJUANA_PLANTATION && DistanceToIndustryType(area, IT_WEED_MACHINE) < COFFEESHOP_GYMNASIUM_MIN_DISTANCE) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_WEED_MACHINE);
	}
	if ((type == IT_COFFEESHOP || type == IT_GYMNASIUM || type == IT_STATUE || type == IT_WEED_MACHINE) &&
			DistanceToIndustryType(area, IT_MARIJUANA_PLANTATION) < COFFEESHOP_GYMNASIUM_MIN_DISTANCE) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_PLANTATION);
	}

	/* The plantation's field road lies on a row the industry does not take
	 * (SetupMarijuanaPlantation()): that row has to be flat open land, bare
	 * or with trees, which go when the plantation is built, so that the road
	 * picture drawn over it lies on the ground and the player can lay a road
	 * there. */
	if (type == IT_MARIJUANA_PLANTATION) {
		for (uint x = 0; x < PLANTATION_WIDTH; x++) {
			TileIndex road_tile = TileAddWrap(tile, x, PLANTATION_ROAD_ROW);
			if (!IsValidTile(road_tile) || !IsTileFlat(road_tile) || IsBridgeAbove(road_tile) ||
					!(IsTileType(road_tile, TileType::Clear) || IsTileType(road_tile, TileType::Trees)) ||
					(HasTileWaterClass(road_tile) && IsTileOnWater(road_tile))) {
				return CommandCost(STR_ERROR_SITE_UNSUITABLE);
			}
			CommandCost ret = EnsureNoVehicleOnGround(road_tile);
			if (ret.Failed()) return ret;
		}
	}

	if (GetIndustrySpec(type)->behaviour.Test(IndustryBehaviour::Town1200More) && t->cache.population < 1200) {
		return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_IN_TOWNS_WITH_POPULATION_OF_1200);
	}

	if (GetIndustrySpec(type)->behaviour.Test(IndustryBehaviour::OnlyNearTown) && DistanceMax(t->xy, tile) > 9) {
		return CommandCost(STR_ERROR_CAN_ONLY_BE_BUILT_NEAR_TOWN_CENTER);
	}

	return CommandCost();
}

static bool CheckCanTerraformSurroundingTiles(TileIndex tile, uint height, int internal)
{
	/* Check if we don't leave the map */
	if (TileX(tile) == 0 || TileY(tile) == 0 || GetTileType(tile) == TileType::Void) return false;

	TileArea ta(tile - TileDiffXY(1, 1), 2, 2);
	for (TileIndex tile_walk : ta) {
		uint curh = TileHeight(tile_walk);
		/* Is the tile clear? */
		if ((GetTileType(tile_walk) != TileType::Clear) && (GetTileType(tile_walk) != TileType::Trees)) return false;

		/* Don't allow too big of a change if this is the sub-tile check */
		if (internal != 0 && Delta(curh, height) > 1) return false;

		/* Different height, so the surrounding tiles of this tile
		 *  has to be correct too (in level, or almost in level)
		 *  else you get a chain-reaction of terraforming. */
		if (internal == 0 && curh != height) {
			if (TileX(tile_walk) == 0 || TileY(tile_walk) == 0 || !CheckCanTerraformSurroundingTiles(tile_walk + TileDiffXY(-1, -1), height, internal + 1)) {
				return false;
			}
		}
	}

	return true;
}

/**
 * This function tries to flatten out the land below an industry, without
 *  damaging the surroundings too much.
 * @param tile The tile to place the industry's starting point at.
 * @param flags Flags describing how to execute this command.
 * @param layout The layout to build.
 * @return \c true iff the industry can be built at this location.
 */
static bool CheckIfCanLevelIndustryPlatform(TileIndex tile, DoCommandFlags flags, const IndustryTileLayout &layout)
{
	int max_x = 0;
	int max_y = 0;

	/* Finds dimensions of largest variant of this industry */
	for (const IndustryTileLayoutTile &it : layout) {
		if (it.gfx == GFX_WATERTILE_SPECIALCHECK) continue; // watercheck tiles don't count for footprint size
		if (it.ti.x > max_x) max_x = it.ti.x;
		if (it.ti.y > max_y) max_y = it.ti.y;
	}

	/* Remember level height */
	uint h = TileHeight(tile);

	if (TileX(tile) <= _settings_game.construction.industry_platform + 1U || TileY(tile) <= _settings_game.construction.industry_platform + 1U) return false;
	/* Check that all tiles in area and surrounding are clear
	 * this determines that there are no obstructing items */

	/* TileArea::Expand is not used here as we need to abort
	 * instead of clamping if the bounds cannot expanded. */
	TileArea ta(tile + TileDiffXY(-_settings_game.construction.industry_platform, -_settings_game.construction.industry_platform),
			max_x + 2 + 2 * _settings_game.construction.industry_platform, max_y + 2 + 2 * _settings_game.construction.industry_platform);

	if (TileX(ta.tile) + ta.w >= Map::MaxX() || TileY(ta.tile) + ta.h >= Map::MaxY()) return false;

	/* _current_company is OWNER_NONE for randomly generated industries and in editor, or the company who funded or prospected the industry.
	 * Perform terraforming as OWNER_TOWN to disable autoslope and town ratings. */
	AutoRestoreBackup cur_company(_current_company, OWNER_TOWN);

	for (TileIndex tile_walk : ta) {
		uint curh = TileHeight(tile_walk);
		if (curh != h) {
			/* This tile needs terraforming. Check if we can do that without
			 *  damaging the surroundings too much. */
			if (!CheckCanTerraformSurroundingTiles(tile_walk, h, 0)) {
				return false;
			}
			/* This is not 100% correct check, but the best we can do without modifying the map.
			 *  What is missing, is if the difference in height is more than 1.. */
			if (ExtractCommandCost(Command<Commands::TerraformLand>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), tile_walk, SLOPE_N, curh <= h)).Failed()) {
				return false;
			}
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Terraform the land under the industry */
		for (TileIndex tile_walk : ta) {
			uint curh = TileHeight(tile_walk);
			while (curh != h) {
				/* We give the terraforming for free here, because we can't calculate
				 *  exact cost in the test-round, and as we all know, that will cause
				 *  a nice assert if they don't match ;) */
				Command<Commands::TerraformLand>::Do(flags, tile_walk, SLOPE_N, curh <= h);
				curh += (curh > h) ? -1 : 1;
			}
		}
	}

	return true;
}


/**
 * Check that the new industry is far enough from conflicting industries.
 * @param tile Tile to construct the industry.
 * @param type Type of the new industry.
 * @return Succeeded or failed command.
 */
static CommandCost CheckIfFarEnoughFromConflictingIndustry(TileIndex tile, IndustryType type)
{
	const IndustrySpec *indspec = GetIndustrySpec(type);

	for (IndustryType conflicting_type : indspec->conflicting) {
		if (conflicting_type == IT_INVALID) continue;

		for (const IndustryID &industry : Industry::industries[conflicting_type]) {
			/* Within 14 tiles from another industry is considered close */
			if (DistanceMax(tile, Industry::Get(industry)->location.tile) > 14) continue;

			return CommandCost(STR_ERROR_INDUSTRY_TOO_CLOSE);
		}
	}
	return CommandCost();
}

/**
 * Advertise about a new industry opening.
 * @param ind Industry being opened.
 */
static void AdvertiseIndustryOpening(const Industry *ind)
{
	const IndustrySpec *ind_spc = GetIndustrySpec(ind->type);
	EncodedString headline;
	if (ind_spc->new_industry_text > STR_LAST_STRINGID) {
		headline = GetEncodedString(ind_spc->new_industry_text, ind_spc->name, STR_TOWN_NAME, ind->town->index);
	} else {
		headline = GetEncodedString(ind_spc->new_industry_text, ind_spc->name, ind->town->index);
	}
	AddIndustryNewsItem(std::move(headline), NewsType::IndustryOpen, ind->index);
	AI::BroadcastNewEvent(new ScriptEventIndustryOpen(ind->index));
	Game::NewEvent(new ScriptEventIndustryOpen(ind->index));
}

/**
 * Populate an industry's list of nearby stations, and if it accepts any cargo, also
 * add the industry to each station's nearby industry list.
 * @param ind Industry
 */
static void PopulateStationsNearby(Industry *ind)
{
	if (ind->neutral_station != nullptr && !_settings_game.station.serve_neutral_industries) {
		/* Industry has a neutral station. Use it and ignore any other nearby stations. */
		ind->stations_near.insert(ind->neutral_station);
		ind->neutral_station->industries_near.clear();
		ind->neutral_station->industries_near.insert(IndustryListEntry{0, ind});
		return;
	}

	ForAllStationsAroundTiles(ind->location, [ind](Station *st, TileIndex tile) {
		if (!IsTileType(tile, TileType::Industry) || GetIndustryIndex(tile) != ind->index) return false;
		ind->stations_near.insert(st);
		st->AddIndustryToDeliver(ind, tile);
		return false;
	});
}

/**
 * Put an industry on the map.
 * @param i                   Just allocated poolitem, mostly empty.
 * @param tile                North tile of the industry.
 * @param type                Type of the industry.
 * @param layout              Industrylayout to build.
 * @param layout_index        Number of the industry layout.
 * @param t                   Nearest town.
 * @param founder             Founder of the industry; OWNER_NONE in case of random construction.
 * @param initial_random_bits Random bits for the industry.
 */
static void DoCreateNewIndustry(Industry *i, TileIndex tile, IndustryType type, const IndustryTileLayout &layout, size_t layout_index, Town *t, Owner founder, uint16_t initial_random_bits)
{
	const IndustrySpec *indspec = GetIndustrySpec(type);

	i->location = TileArea(tile, 1, 1);
	i->type = type;

	auto &industries = Industry::industries[type];
	industries.insert(i->index);

	size_t produced_count = 0;
	for (size_t index = 0; index < std::size(indspec->produced_cargo); ++index) {
		if (IsValidCargoType(indspec->produced_cargo[index])) {
			produced_count = index + 1;
		}
	}
	for (size_t index = 0; index < produced_count; ++index) {
		Industry::ProducedCargo &p = i->produced.emplace_back();
		p.cargo = indspec->produced_cargo[index];
		p.rate = indspec->production_rate[index];
	}

	size_t accepted_count = 0;
	for (size_t index = 0; index < std::size(indspec->accepts_cargo); ++index) {
		if (IsValidCargoType(indspec->accepts_cargo[index])) {
			accepted_count = index + 1;
		}
	}
	for (size_t index = 0; index < accepted_count; ++index) {
		Industry::AcceptedCargo &a = i->accepted.emplace_back();
		a.cargo = indspec->accepts_cargo[index];
	}

	/* Randomize initial production if non-original economy is used and there are no production related callbacks. */
	if (!indspec->UsesOriginalEconomy()) {
		for (auto &p : i->produced) {
			p.rate = ClampTo<uint8_t>((RandomRange(256) + 128) * p.rate >> 8);
		}
	}

	i->town = t;
	i->owner = OWNER_NONE;

	uint16_t r = Random();
	i->random_colour = static_cast<Colours>(GB(r, 0, 4));
	i->counter = GB(r, 4, 12);
	i->random = initial_random_bits;
	i->was_cargo_delivered = false;
	i->last_prod_year = TimerGameEconomy::year;
	i->founder = founder;
	i->ctlflags = {};

	i->construction_date = TimerGameCalendar::date;
	i->construction_type = (_game_mode == GameMode::Editor) ? IndustryConstructionType::ScenarioEditor :
			(_generating_world ? IndustryConstructionType::MapGeneration : IndustryConstructionType::Gameplay);

	/* Adding 1 here makes it conform to specs of var44 of varaction2 for industries
	 * 0 = created prior of newindustries
	 * else, chosen layout + 1 */
	i->selected_layout = (uint8_t)(layout_index + 1);

	i->exclusive_supplier = INVALID_OWNER;
	i->exclusive_consumer = INVALID_OWNER;

	i->prod_level = PRODLEVEL_DEFAULT;

	/* Call callbacks after the regular fields got initialised. */

	if (indspec->callback_mask.Test(IndustryCallbackMask::ProdChangeBuild)) {
		uint16_t res = GetIndustryCallback(CBID_INDUSTRY_PROD_CHANGE_BUILD, 0, Random(), i, type, INVALID_TILE);
		if (res != CALLBACK_FAILED) {
			if (res < PRODLEVEL_MINIMUM || res > PRODLEVEL_MAXIMUM) {
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_PROD_CHANGE_BUILD, res);
			} else {
				i->prod_level = res;
				i->RecomputeProductionMultipliers();
			}
		}
	}

	if (_generating_world) {
		if (indspec->callback_mask.Test(IndustryCallbackMask::Production256Ticks)) {
			IndustryProductionCallback(i, 1);
			for (auto &p : i->produced) {
				if (IsValidCargoType(p.cargo)) p.history[LAST_MONTH].production = ScaleByCargoScale(p.waiting * 8, false);
				p.waiting = 0;
			}
		}

		for (auto &p : i->produced) {
			if (IsValidCargoType(p.cargo)) p.history[LAST_MONTH].production += ScaleByCargoScale(p.rate * 8, false);
		}

		UpdateValidHistory(i->valid_history, HISTORY_YEAR, TimerGameEconomy::month);
	}

	if (indspec->callback_mask.Test(IndustryCallbackMask::DecideColour)) {
		uint16_t res = GetIndustryCallback(CBID_INDUSTRY_DECIDE_COLOUR, 0, 0, i, type, INVALID_TILE);
		if (res != CALLBACK_FAILED) {
			if (GB(res, 4, 11) != 0) ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_DECIDE_COLOUR, res);
			i->random_colour = static_cast<Colours>(GB(res, 0, 4));
		}
	}

	if (indspec->callback_mask.Test(IndustryCallbackMask::InputCargoTypes)) {
		/* Clear all input cargo types */
		i->accepted.clear();
		/* Query actual types */
		uint maxcargoes = indspec->behaviour.Test(IndustryBehaviour::CargoTypesUnlimited) ? INDUSTRY_NUM_INPUTS : 3;
		for (uint j = 0; j < maxcargoes; j++) {
			uint16_t res = GetIndustryCallback(CBID_INDUSTRY_INPUT_CARGO_TYPES, j, 0, i, type, INVALID_TILE);
			if (res == CALLBACK_FAILED || GB(res, 0, 8) == UINT8_MAX) break;
			if (indspec->grf_prop.grffile->grf_version >= 8 && res >= 0x100) {
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_INPUT_CARGO_TYPES, res);
				break;
			}
			CargoType cargo = GetCargoTranslation(GB(res, 0, 8), indspec->grf_prop.grffile);
			/* Industries without "unlimited" cargo types support depend on the specific order/slots of cargo types.
			 * They need to be able to blank out specific slots without aborting the callback sequence,
			 * and solve this by returning undefined cargo indexes. Skip these. */
			if (!IsValidCargoType(cargo) && !indspec->behaviour.Test(IndustryBehaviour::CargoTypesUnlimited)) {
				/* As slots are allocated as needed now, this means we do need to add a slot for the invalid cargo. */
				Industry::AcceptedCargo &a = i->accepted.emplace_back();
				a.cargo = INVALID_CARGO;
				continue;
			}
			/* Verify valid cargo */
			if (std::ranges::find(indspec->accepts_cargo, cargo) == std::end(indspec->accepts_cargo)) {
				/* Cargo not in spec, error in NewGRF */
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_INPUT_CARGO_TYPES, res);
				break;
			}
			if (std::any_of(std::begin(i->accepted), std::begin(i->accepted) + j, [&cargo](const auto &a) { return a.cargo == cargo; })) {
				/* Duplicate cargo */
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_INPUT_CARGO_TYPES, res);
				break;
			}
			Industry::AcceptedCargo &a = i->accepted.emplace_back();
			a.cargo = cargo;
		}
	}

	if (indspec->callback_mask.Test(IndustryCallbackMask::OutputCargoTypes)) {
		/* Clear all output cargo types */
		i->produced.clear();
		/* Query actual types */
		uint maxcargoes = indspec->behaviour.Test(IndustryBehaviour::CargoTypesUnlimited) ? INDUSTRY_NUM_OUTPUTS : 2;
		for (uint j = 0; j < maxcargoes; j++) {
			uint16_t res = GetIndustryCallback(CBID_INDUSTRY_OUTPUT_CARGO_TYPES, j, 0, i, type, INVALID_TILE);
			if (res == CALLBACK_FAILED || GB(res, 0, 8) == UINT8_MAX) break;
			if (indspec->grf_prop.grffile->grf_version >= 8 && res >= 0x100) {
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_OUTPUT_CARGO_TYPES, res);
				break;
			}
			CargoType cargo = GetCargoTranslation(GB(res, 0, 8), indspec->grf_prop.grffile);
			/* Allow older GRFs to skip slots. */
			if (!IsValidCargoType(cargo) && !indspec->behaviour.Test(IndustryBehaviour::CargoTypesUnlimited)) {
				/* As slots are allocated as needed now, this means we do need to add a slot for the invalid cargo. */
				Industry::ProducedCargo &p = i->produced.emplace_back();
				p.cargo = INVALID_CARGO;
				continue;
			}
			/* Verify valid cargo */
			if (std::ranges::find(indspec->produced_cargo, cargo) == std::end(indspec->produced_cargo)) {
				/* Cargo not in spec, error in NewGRF */
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_OUTPUT_CARGO_TYPES, res);
				break;
			}
			if (std::any_of(std::begin(i->produced), std::begin(i->produced) + j, [&cargo](const auto &p) { return p.cargo == cargo; })) {
				/* Duplicate cargo */
				ErrorUnknownCallbackResult(indspec->grf_prop.grfid, CBID_INDUSTRY_OUTPUT_CARGO_TYPES, res);
				break;
			}
			Industry::ProducedCargo &p = i->produced.emplace_back();
			p.cargo = cargo;
		}
	}

	/* Plant the tiles */

	for (const IndustryTileLayoutTile &it : layout) {
		TileIndex cur_tile = tile + ToTileIndexDiff(it.ti);

		if (it.gfx != GFX_WATERTILE_SPECIALCHECK) {
			i->location.Add(cur_tile);

			WaterClass wc = (IsWaterTile(cur_tile) ? GetWaterClass(cur_tile) : WaterClass::Invalid);

			Command<Commands::LandscapeClear>::Do({DoCommandFlag::Execute, DoCommandFlag::NoTestTownRating, DoCommandFlag::NoModifyTownRating}, cur_tile);

			MakeIndustry(cur_tile, i->index, it.gfx, Random(), wc);

			if (_generating_world) {
				SetIndustryConstructionCounter(cur_tile, 3);
				SetIndustryConstructionStage(cur_tile, 2);
			}

			/* it->gfx is stored in the map. But the translated ID cur_gfx is the interesting one */
			IndustryGfx cur_gfx = GetTranslatedIndustryTileID(it.gfx);
			const IndustryTileSpec *its = GetIndustryTileSpec(cur_gfx);
			if (its->animation.status != AnimationStatus::NoAnimation) AddAnimatedTile(cur_tile);
		}
	}

	/* The plantation's field road: the trees on the row left to the player
	 * go, the row itself stays the player's (CheckIfIndustryIsAllowed()). */
	if (type == IT_MARIJUANA_PLANTATION) {
		for (uint x = 0; x < PLANTATION_WIDTH; x++) {
			TileIndex road_tile = TileAddXY(tile, x, PLANTATION_ROAD_ROW);
			if (IsTileType(road_tile, TileType::Trees)) {
				Command<Commands::LandscapeClear>::Do({DoCommandFlag::Execute, DoCommandFlag::NoTestTownRating, DoCommandFlag::NoModifyTownRating}, road_tile);
			}
		}
	}

	/* Call callbacks after all tiles have been created. */
	for (TileIndex cur_tile : i->location) {
		if (i->TileBelongsToIndustry(cur_tile)) {
			/* There are no shared random bits, consistent with "MakeIndustryTileBigger" in tile loop.
			 * So, trigger tiles individually */
			TriggerIndustryTileAnimation_ConstructionStageChanged(cur_tile, true);
		}
	}

	if (GetIndustrySpec(i->type)->behaviour.Test(IndustryBehaviour::PlantOnBuild)) {
		for (uint j = 0; j != 50; j++) PlantRandomFarmField(i);
	}
	InvalidateWindowData(WindowClass::IndustryDirectory, 0, IDIWD_FORCE_REBUILD);
	SetWindowDirty(WindowClass::BuildIndustry, 0);

	if (!_generating_world) PopulateStationsNearby(i);
}

/**
 * Helper function for Build/Fund an industry
 * @param tile tile where industry is built
 * @param type of industry to build
 * @param flags of operations to conduct
 * @param indspec pointer to industry specifications
 * @param layout_index the index of the itsepc to build/fund
 * @param random_var8f random seed (possibly) used by industries
 * @param random_initial_bits The random bits the industry is going to have after construction.
 * @param founder Founder of the industry
 * @param creation_type The circumstances the industry is created under.
 * @param[out] ip Pointer to store newly created industry.
 * @return Succeeded or failed command.
 *
 * @post \c *ip contains the newly created industry if all checks are successful and the \a flags request actual creation, else it contains \c nullptr afterwards.
 */
static CommandCost CreateNewIndustryHelper(TileIndex tile, IndustryType type, DoCommandFlags flags, const IndustrySpec *indspec, size_t layout_index, uint32_t random_var8f, uint16_t random_initial_bits, Owner founder, IndustryAvailabilityCallType creation_type, Industry **ip)
{
	assert(layout_index < indspec->layouts.size());
	const IndustryTileLayout &layout = indspec->layouts[layout_index];

	*ip = nullptr;

	/* 1. Cheap: Built-in checks on industry level. */
	CommandCost ret = CheckIfFarEnoughFromConflictingIndustry(tile, type);
	if (ret.Failed()) return ret;

	Town *t = nullptr;
	ret = FindTownForIndustry(tile, type, &t);
	if (ret.Failed()) return ret;
	assert(t != nullptr);

	ret = CheckIfIndustryIsAllowed(tile, type, t);
	if (ret.Failed()) return ret;

	/* 2. Built-in checks on industry tiles. */
	std::vector<ClearedObjectArea> object_areas(_cleared_object_areas);
	ret = CheckIfIndustryTilesAreFree(tile, layout, type);
	_cleared_object_areas = std::move(object_areas);
	if (ret.Failed()) return ret;

	/* 3. NewGRF-defined checks on industry level. */
	if (GetIndustrySpec(type)->callback_mask.Test(IndustryCallbackMask::Location)) {
		ret = CheckIfCallBackAllowsCreation(tile, type, layout_index, random_var8f, random_initial_bits, founder, creation_type);
	} else {
		ret = _check_new_industry_procs[indspec->check_proc](tile);
	}
	if (ret.Failed()) return ret;

	/* 4. Expensive: NewGRF-defined checks on industry tiles. */
	bool custom_shape_check = false;
	ret = CheckIfIndustryTileSlopes(tile, layout, layout_index, type, random_initial_bits, founder, creation_type, &custom_shape_check);
	if (ret.Failed()) return ret;

	if (!custom_shape_check && _settings_game.game_creation.land_generator == LG_TERRAGENESIS && _generating_world &&
			!_ignore_industry_restrictions && !CheckIfCanLevelIndustryPlatform(tile, DoCommandFlag::NoWater, layout)) {
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}

	if (!Industry::CanAllocateItem()) return CommandCost(STR_ERROR_TOO_MANY_INDUSTRIES);

	if (flags.Test(DoCommandFlag::Execute)) {
		*ip = Industry::Create(tile);
		if (!custom_shape_check) CheckIfCanLevelIndustryPlatform(tile, {DoCommandFlag::NoWater, DoCommandFlag::Execute}, layout);
		DoCreateNewIndustry(*ip, tile, type, layout, layout_index, t, founder, random_initial_bits);
	}

	return CommandCost();
}

/**
 * Build/Fund an industry
 * @param flags of operations to conduct
 * @param tile tile where industry is built
 * @param it industry type see build_industry.h and see industry.h
 * @param first_layout first layout to try
 * @param fund false = prospect, true = fund (only valid if current company is DEITY)
 * @param seed seed to use for desyncfree randomisations
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildIndustry(DoCommandFlags flags, TileIndex tile, IndustryType it, uint32_t first_layout, bool fund, uint32_t seed)
{
	if (it >= NUM_INDUSTRYTYPES) return CMD_ERROR;

	const IndustrySpec *indspec = GetIndustrySpec(it);

	/* Check if the to-be built/founded industry is available for this climate. */
	if (!indspec->enabled || indspec->layouts.empty()) return CMD_ERROR;

	/* If the setting for raw-material industries is not on, you cannot build raw-material industries.
	 * Raw material industries are industries that do not accept cargo (at least for now) */
	if (_game_mode != GameMode::Editor && _current_company != OWNER_DEITY && _settings_game.construction.raw_industry_construction == 0 && indspec->IsRawIndustry()) {
		return CMD_ERROR;
	}

	if (_game_mode != GameMode::Editor && GetIndustryProbabilityCallback(it, _current_company == OWNER_DEITY ? IndustryAvailabilityCallType::RandomCreation : IndustryAvailabilityCallType::UserCreation, 1) == 0) {
		return CMD_ERROR;
	}

	Randomizer randomizer;
	randomizer.SetSeed(seed);
	uint16_t random_initial_bits = GB(seed, 0, 16);
	uint32_t random_var8f = randomizer.Next();
	size_t num_layouts = indspec->layouts.size();
	CommandCost ret = CommandCost(STR_ERROR_SITE_UNSUITABLE);
	const bool deity_prospect = _current_company == OWNER_DEITY && !fund;

	Industry *ind = nullptr;
	if (deity_prospect || (_game_mode != GameMode::Editor && _current_company != OWNER_DEITY && _settings_game.construction.raw_industry_construction == 2 && indspec->IsRawIndustry())) {
		if (flags.Test(DoCommandFlag::Execute)) {
			/* Prospecting has a chance to fail, however we cannot guarantee that something can
			 * be built on the map, so the chance gets lower when the map is fuller, but there
			 * is nothing we can really do about that. */
			bool prospect_success = deity_prospect || Random() <= indspec->prospecting_chance;
			if (prospect_success) {
				/* Prospected industries are build as OWNER_TOWN to not e.g. be build on owned land of the founder */
				IndustryAvailabilityCallType calltype = _current_company == OWNER_DEITY ? IndustryAvailabilityCallType::RandomCreation : IndustryAvailabilityCallType::ProspectCreation;
				AutoRestoreBackup cur_company(_current_company, OWNER_TOWN);
				for (int i = 0; i < 5000; i++) {
					/* We should not have more than one Random() in a function call
					 * because parameter evaluation order is not guaranteed in the c++ standard
					 */
					tile = RandomTile();
					/* Start with a random layout */
					size_t layout = RandomRange((uint32_t)num_layouts);
					/* Check now each layout, starting with the random one */
					for (size_t j = 0; j < num_layouts; j++) {
						layout = (layout + 1) % num_layouts;
						ret = CreateNewIndustryHelper(tile, it, flags, indspec, layout, random_var8f, random_initial_bits, cur_company.GetOriginalValue(), calltype, &ind);
						if (ret.Succeeded()) break;
					}
					if (ret.Succeeded()) break;
				}
			}
			if (ret.Failed() && IsLocalCompany()) {
				if (prospect_success) {
					ShowErrorMessage(GetEncodedString(STR_ERROR_CAN_T_PROSPECT_INDUSTRY), GetEncodedString(STR_ERROR_NO_SUITABLE_PLACES_FOR_PROSPECTING), WarningLevel::Info);
				} else {
					ShowErrorMessage(GetEncodedString(STR_ERROR_CAN_T_PROSPECT_INDUSTRY), GetEncodedString(STR_ERROR_PROSPECTING_WAS_UNLUCKY), WarningLevel::Info);
				}
			}
		}
	} else {
		size_t layout = first_layout;
		if (layout >= num_layouts) return CMD_ERROR;

		/* Check subsequently each layout, starting with the given layout in p1 */
		for (size_t i = 0; i < num_layouts; i++) {
			layout = (layout + 1) % num_layouts;
			ret = CreateNewIndustryHelper(tile, it, flags, indspec, layout, random_var8f, random_initial_bits, _current_company, _current_company == OWNER_DEITY ? IndustryAvailabilityCallType::RandomCreation : IndustryAvailabilityCallType::UserCreation, &ind);
			if (ret.Succeeded()) break;
		}

		/* If it still failed, there's no suitable layout to build here, return the error */
		if (ret.Failed()) return ret;
	}

	if (flags.Test(DoCommandFlag::Execute) && ind != nullptr && _game_mode != GameMode::Editor) {
		AdvertiseIndustryOpening(ind);
	}

	return CommandCost(ExpensesType::Other, indspec->GetConstructionCost());
}

/**
 * Set industry control flags.
 * @param flags Type of operation.
 * @param ind_id IndustryID
 * @param ctlflags IndustryControlFlags
 * @return Empty cost or an error.
 */
CommandCost CmdIndustrySetFlags(DoCommandFlags flags, IndustryID ind_id, IndustryControlFlags ctlflags)
{
	if (_current_company != OWNER_DEITY) return CMD_ERROR;

	Industry *ind = Industry::GetIfValid(ind_id);
	if (ind == nullptr) return CMD_ERROR;
	if (!ctlflags.IsValid()) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) ind->ctlflags = ctlflags;

	return CommandCost();
}

/**
 * Set industry production.
 * @param flags Type of operation.
 * @param ind_id IndustryID
 * @param prod_level Production level.
 * @param show_news Show a news message on production change.
 * @param custom_news Custom news message text.
 * @return Empty cost or an error.
 */
CommandCost CmdIndustrySetProduction(DoCommandFlags flags, IndustryID ind_id, uint8_t prod_level, bool show_news, const EncodedString &custom_news)
{
	if (_current_company != OWNER_DEITY) return CMD_ERROR;
	if (prod_level < PRODLEVEL_MINIMUM || prod_level > PRODLEVEL_MAXIMUM) return CMD_ERROR;

	Industry *ind = Industry::GetIfValid(ind_id);
	if (ind == nullptr) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		ind->ctlflags.Set(IndustryControlFlag::ExternalProdLevel);
		ind->prod_level = prod_level;
		ind->RecomputeProductionMultipliers();

		/* Show news message if requested. */
		if (show_news && prod_level != ind->prod_level) {
			NewsType nt;
			switch (WhoCanServiceIndustry(ind)) {
				case 0: nt = NewsType::IndustryNobody;  break;
				case 1: nt = NewsType::IndustryOther;   break;
				case 2: nt = NewsType::IndustryCompany; break;
				default: NOT_REACHED();
			}

			/* Set parameters of news string */
			EncodedString headline;
			if (!custom_news.empty()) {
				headline = custom_news;
			} else {
				StringID str = (prod_level > ind->prod_level)
					? GetIndustrySpec(ind->type)->production_up_text
					: GetIndustrySpec(ind->type)->production_down_text;

				if (str > STR_LAST_STRINGID) {
					headline = GetEncodedString(str, STR_TOWN_NAME, ind->town->index, GetIndustrySpec(ind->type)->name);
				} else {
					headline = GetEncodedString(str, ind->index);
				}
			}

			AddIndustryNewsItem(std::move(headline), nt, ind->index);
		}
	}

	return CommandCost();
}

/**
 * Change exclusive consumer or supplier for the industry.
 * @param flags Type of operation.
 * @param ind_id IndustryID
 * @param company_id CompanyID to set or INVALID_OWNER (available to everyone) or
 *                   OWNER_NONE (neutral stations only) or OWNER_DEITY (no one)
 * @param consumer Set exclusive consumer if true, supplier if false.
 * @return Empty cost or an error.
 */
CommandCost CmdIndustrySetExclusivity(DoCommandFlags flags, IndustryID ind_id, Owner company_id, bool consumer)
{
	if (_current_company != OWNER_DEITY) return CMD_ERROR;

	Industry *ind = Industry::GetIfValid(ind_id);
	if (ind == nullptr) return CMD_ERROR;

	if (company_id != OWNER_NONE && company_id != INVALID_OWNER && company_id != OWNER_DEITY
		&& !Company::IsValidID(company_id)) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		if (consumer) {
			ind->exclusive_consumer = company_id;
		} else {
			ind->exclusive_supplier = company_id;
		}
	}


	return CommandCost();
}

/**
 * Change additional industry text.
 * @param flags Type of operation.
 * @param ind_id IndustryID
 * @param text - Additional industry text.
 * @return Empty cost or an error.
 */
CommandCost CmdIndustrySetText(DoCommandFlags flags, IndustryID ind_id, const EncodedString &text)
{
	if (_current_company != OWNER_DEITY) return CMD_ERROR;

	Industry *ind = Industry::GetIfValid(ind_id);
	if (ind == nullptr) return CMD_ERROR;

	if (flags.Test(DoCommandFlag::Execute)) {
		ind->text.clear();
		if (!text.empty()) ind->text = text;
		InvalidateWindowData(WindowClass::IndustryView, ind->index);
	}

	return CommandCost();
}

/**
 * Create a new industry of random layout.
 * @param tile The location to build the industry.
 * @param type The industry type to build.
 * @param creation_type The circumstances the industry is created under.
 * @return the created industry or nullptr if it failed.
 */
static Industry *CreateNewIndustry(TileIndex tile, IndustryType type, IndustryAvailabilityCallType creation_type)
{
	const IndustrySpec *indspec = GetIndustrySpec(type);

	uint32_t seed = Random();
	uint32_t seed2 = Random();
	Industry *i = nullptr;
	size_t layout_index = RandomRange((uint32_t)indspec->layouts.size());
	[[maybe_unused]] CommandCost ret = CreateNewIndustryHelper(tile, type, DoCommandFlag::Execute, indspec, layout_index, seed, GB(seed2, 0, 16), OWNER_NONE, creation_type, &i);
	assert(i != nullptr || ret.Failed());
	return i;
}

/**
 * Compute the appearance probability for an industry during map creation.
 * @param it Industry type to compute.
 * @param water Whether to get probability of land-based, water-based, (or both, if std::nullopt), industry types.
 * @param[out] force_at_least_one Returns whether at least one instance should be forced on map creation.
 * @return Relative probability for the industry to appear.
 */
static uint32_t GetScaledIndustryGenerationProbability(IndustryType it, std::optional<bool> water, bool *force_at_least_one)
{
	const IndustrySpec *ind_spc = GetIndustrySpec(it);
	if (water.has_value() && ind_spc->behaviour.Test(IndustryBehaviour::BuiltOnWater) != *water) return 0;

	uint32_t chance = OriginalIndustryChance(it, true);
	if (!ind_spc->enabled || ind_spc->layouts.empty() ||
			(_game_mode != GameMode::Editor && _settings_game.difficulty.industry_density == IndustryDensity::FundedOnly) ||
			(chance = GetIndustryProbabilityCallback(it, IndustryAvailabilityCallType::MapGeneration, chance)) == 0) {
		*force_at_least_one = false;
		return 0;
	} else {
		chance *= 16; // to increase precision
		/* We want industries appearing at coast to appear less often on bigger maps, as length of coast increases slower than map area.
		 * For simplicity we scale in both cases, though scaling the probabilities of all industries has no effect. */
		chance = (ind_spc->check_proc == IndustryCheck::Refinery || ind_spc->check_proc == IndustryCheck::OilRig) ? Map::ScaleBySize1D(chance) : Map::ScaleBySize(chance);

		*force_at_least_one = (chance > 0) && !ind_spc->behaviour.Test(IndustryBehaviour::NoBuildMapCreation) && (_game_mode != GameMode::Editor);
		return chance;
	}
}

/**
 * Compute the probability for constructing a new industry during game play.
 * @param it Industry type to compute.
 * @param[out] min_number Minimal number of industries that should exist at the map.
 * @return Relative probability for the industry to appear.
 */
static uint16_t GetIndustryGamePlayProbability(IndustryType it, uint8_t *min_number)
{
	if (_settings_game.difficulty.industry_density == IndustryDensity::FundedOnly) {
		*min_number = 0;
		return 0;
	}

	const IndustrySpec *ind_spc = GetIndustrySpec(it);
	uint8_t chance = OriginalIndustryChance(it, false);
	if (!ind_spc->enabled || ind_spc->layouts.empty() ||
			(ind_spc->behaviour.Test(IndustryBehaviour::Before1950) && TimerGameCalendar::year > 1950) ||
			(ind_spc->behaviour.Test(IndustryBehaviour::After1960) && TimerGameCalendar::year < 1960) ||
			(chance = GetIndustryProbabilityCallback(it, IndustryAvailabilityCallType::RandomCreation, chance)) == 0) {
		*min_number = 0;
		return 0;
	}
	*min_number = ind_spc->behaviour.Test(IndustryBehaviour::CanCloseLastInstance) ? 1 : 0;
	return chance;
}

/**
 * Get wanted number of industries on the map.
 * @return Wanted number of industries at the map.
 */
static uint GetNumberOfIndustries()
{
	/* Number of industries on a 256x256 map. */
	static const uint16_t numof_industry_table[] = {
		0,    // none
		0,    // minimal
		10,   // very low
		25,   // low
		55,   // normal
		80,   // high
		0,    // custom
	};

	assert(lengthof(numof_industry_table) == to_underlying(IndustryDensity::End));
	IndustryDensity density = (_game_mode != GameMode::Editor) ? _settings_game.difficulty.industry_density : IndustryDensity::VeryLow;

	if (density == IndustryDensity::Custom) return std::min<uint>(IndustryPool::MAX_SIZE, _settings_game.game_creation.custom_industry_number);

	return std::min<uint>(IndustryPool::MAX_SIZE, Map::ScaleBySize(numof_industry_table[to_underlying(density)]));
}

/**
 * Try to place the industry in the game.
 * Since there is no feedback why placement fails, there is no other option
 * than to try a few times before concluding it does not work.
 * @param type     Industry type of the desired industry.
 * @param creation_type The circumstances the industry is created under.
 * @param try_hard Try very hard to find a place. (Used to place at least one industry per type.)
 * @return Pointer to created industry, or \c nullptr if creation failed.
 */
static Industry *PlaceIndustry(IndustryType type, IndustryAvailabilityCallType creation_type, bool try_hard)
{
	uint tries = try_hard ? 10000u : 2000u;
	for (; tries > 0; tries--) {
		if (type == IT_WEED_MACHINE && Industry::industries[IT_GYMNASIUM].empty()) return nullptr;
		TileIndex tile = type == IT_WEED_MACHINE ? RandomTileNearGymnasium() : RandomTile();
		if (tile == INVALID_TILE) continue;
		Industry *ind = CreateNewIndustry(tile, type, creation_type);
		if (ind != nullptr) return ind;
	}
	return nullptr;
}

/**
 * Try to build a industry on the map.
 * @param type IndustryType of the desired industry
 * @param water Whether building a water or land based industry.
 * @param try_hard Try very hard to find a place. (Used to place at least one industry per type)
 */
static void PlaceInitialIndustry(IndustryType type, bool water, bool try_hard)
{
	AutoRestoreBackup cur_company(_current_company, OWNER_NONE);

	IncreaseGeneratingWorldProgress(water ? GenWorldProgress::WaterIndustries : GenWorldProgress::LandIndustries);
	PlaceIndustry(type, IndustryAvailabilityCallType::MapGeneration, try_hard);
}

/**
 * Get total number of industries existing in the game.
 * @return Number of industries currently in the game.
 */
static uint GetCurrentTotalNumberOfIndustries()
{
	uint total = 0;
	for (const auto &industries : Industry::industries) {
		total += static_cast<uint16_t>(std::size(industries));
	}
	return total;
}


/** Reset the entry. */
void IndustryTypeBuildData::Reset()
{
	this->probability  = 0;
	this->min_number   = 0;
	this->target_count = 0;
	this->max_wait     = 1;
	this->wait_count   = 0;
}

/** Completely reset the industry build data. */
void IndustryBuildData::Reset()
{
	this->wanted_inds = GetCurrentTotalNumberOfIndustries() << 16;

	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		this->builddata[it].Reset();
	}
}

/** Monthly update of industry build data. */
void IndustryBuildData::EconomyMonthlyLoop()
{
	static const int NEWINDS_PER_MONTH = 0x38000 / (10 * 12); // lower 16 bits is a float fraction, 3.5 industries per decade, divided by 10 * 12 months.
	if (_settings_game.difficulty.industry_density == IndustryDensity::FundedOnly) return; // 'no industries' setting.

	/* To prevent running out of unused industries for the player to connect,
	 * add a fraction of new industries each month, but only if the manager can keep up. */
	uint max_behind = 1 + std::min(99u, Map::ScaleBySize(3)); // At most 2 industries for small maps, and 100 at the biggest map (about 6 months industry build attempts).
	if (GetCurrentTotalNumberOfIndustries() + max_behind >= (this->wanted_inds >> 16)) {
		this->wanted_inds += Map::ScaleBySize(NEWINDS_PER_MONTH);
	}
}

struct IndustryGenerationProbabilities {
	std::array<uint32_t, NUM_INDUSTRYTYPES> probs{};
	std::array<bool, NUM_INDUSTRYTYPES> force_one{};
	uint64_t total = 0;
	uint num_forced = 0;
};

/**
 * Get scaled industry generation probabilities.
 * @param water Whether to get land or water industry probabilities.
 * @returns Probability information.
 */
static IndustryGenerationProbabilities GetScaledProbabilities(bool water)
{
	IndustryGenerationProbabilities p{};

	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		p.probs[it] = GetScaledIndustryGenerationProbability(it, water, &p.force_one[it]);
		p.total += p.probs[it];
		if (p.force_one[it]) p.num_forced++;
	}

	return p;
}

/**
 * This function will create random industries during game creation.
 * It will scale the amount of industries by mapsize and difficulty level.
 */
void GenerateIndustries()
{
	if (_game_mode != GameMode::Editor && _settings_game.difficulty.industry_density == IndustryDensity::FundedOnly) return; // No industries in the game.

	/* Get the probabilities for all industries. This is done first as we need the total of
	 * both land and water for scaling later. */
	IndustryGenerationProbabilities lprob = GetScaledProbabilities(false);
	IndustryGenerationProbabilities wprob = GetScaledProbabilities(true);

	/* Run generation twice, for land and water industries in turn. */
	for (bool water = false;; water = true) {
		auto &p = water ? wprob : lprob;

		/* Total number of industries scaled by land/water proportion. */
		uint total_amount = 0;
		if (lprob.total + wprob.total > 0) total_amount = p.total * GetNumberOfIndustries() / (lprob.total + wprob.total);

		/* Scale land-based industries to the land proportion, unless the player has set a custom industry count. */
		if (!water && _settings_game.difficulty.industry_density != IndustryDensity::Custom) total_amount = Map::ScaleByLandProportion(total_amount);

		/* Ensure that forced industries are generated even if the scaled amounts are too low. */
		if (p.total == 0 || total_amount < p.num_forced) {
			/* Only place the forced ones */
			total_amount = p.num_forced;
		}

		SetGeneratingWorldProgress(water ? GenWorldProgress::WaterIndustries : GenWorldProgress::LandIndustries, total_amount);

		/* Try to build one industry per type independent of any probabilities */
		for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
			if (p.force_one[it]) {
				assert(total_amount > 0);
				total_amount--;
				PlaceInitialIndustry(it, water, true);
			}
		}

		/* Add the remaining industries according to their probabilities */
		for (uint i = 0; i < total_amount; i++) {
			uint32_t r = RandomRange(p.total);
			IndustryType it = 0;
			while (r >= p.probs[it]) {
				r -= p.probs[it];
				it++;
				assert(it < NUM_INDUSTRYTYPES);
			}
			assert(p.probs[it] > 0);
			PlaceInitialIndustry(it, water, false);
		}

		if (water) break;
	}

	_industry_builder.Reset();
}

template <>
Industry::ProducedHistory SumHistory(std::span<const Industry::ProducedHistory> history)
{
	uint32_t production = std::accumulate(std::begin(history), std::end(history), 0, [](uint32_t r, const auto &p) { return r + p.production; });
	uint32_t transported = std::accumulate(std::begin(history), std::end(history), 0, [](uint32_t r, const auto &p) { return r + p.transported; });
	auto count = std::size(history);
	return {.production = ClampTo<uint16_t>(production / count), .transported = ClampTo<uint16_t>(transported / count)};
}

template <>
Industry::AcceptedHistory SumHistory(std::span<const Industry::AcceptedHistory> history)
{
	uint32_t accepted = std::accumulate(std::begin(history), std::end(history), 0, [](uint32_t r, const auto &a) { return r + a.accepted; });
	uint32_t waiting = std::accumulate(std::begin(history), std::end(history), 0, [](uint32_t r, const auto &a) { return r + a.waiting; });;
	auto count = std::size(history);
	return {.accepted = ClampTo<uint16_t>(accepted / count), .waiting = ClampTo<uint16_t>(waiting / count)};
}

/**
 * Monthly update of industry statistics.
 * @param i Industry to update.
 */
static void UpdateIndustryStatistics(Industry *i)
{
	auto month = TimerGameEconomy::month;
	UpdateValidHistory(i->valid_history, HISTORY_YEAR, month);

	for (auto &p : i->produced) {
		if (IsValidCargoType(p.cargo)) {
			if (p.history[THIS_MONTH].production != 0) i->last_prod_year = TimerGameEconomy::year;

			RotateHistory(p.history, i->valid_history, HISTORY_YEAR, month);
		}
	}

	for (auto &a : i->accepted) {
		if (!IsValidCargoType(a.cargo)) continue;
		if (a.history == nullptr) continue;

		(*a.history)[THIS_MONTH].waiting = GetAndResetAccumulatedAverage<uint16_t>(a.accumulated_waiting);
		RotateHistory(*a.history, i->valid_history, HISTORY_YEAR, month);
	}
}

/**
 * Recompute #production_rate for current #prod_level.
 * This function is only valid when not using smooth economy.
 */
void Industry::RecomputeProductionMultipliers()
{
	const IndustrySpec *indspec = GetIndustrySpec(this->type);
	assert(indspec->UsesOriginalEconomy());

	/* Rates are rounded up, so e.g. oilrig always produces some passengers */
	for (auto &p : this->produced) {
		p.rate = ClampTo<uint8_t>(CeilDiv(indspec->production_rate[&p - this->produced.data()] * this->prod_level, PRODLEVEL_DEFAULT));
	}
}

void Industry::FillCachedName() const
{
	auto tmp_params = MakeParameters(this->index);
	this->cached_name = GetStringWithArgs(STR_INDUSTRY_NAME, tmp_params);
}

void ClearAllIndustryCachedNames()
{
	for (Industry *ind : Industry::Iterate()) {
		ind->cached_name.clear();
	}
}

/**
 * Set the #probability and #min_number fields for the industry type \a it for a running game.
 * @param it Industry type.
 * @return At least one of the fields has changed value.
 */
bool IndustryTypeBuildData::GetIndustryTypeData(IndustryType it)
{
	uint8_t min_number;
	uint32_t probability = GetIndustryGamePlayProbability(it, &min_number);
	bool changed = min_number != this->min_number || probability != this->probability;
	this->min_number = min_number;
	this->probability = probability;
	return changed;
}

/** Decide how many industries of each type are needed. */
void IndustryBuildData::SetupTargetCount()
{
	bool changed = false;
	uint num_planned = 0; // Number of industries planned in the industry build data.
	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		changed |= this->builddata[it].GetIndustryTypeData(it);
		num_planned += this->builddata[it].target_count;
	}
	uint total_amount = this->wanted_inds >> 16; // Desired total number of industries.
	changed |= num_planned != total_amount;
	if (!changed) return; // All industries are still the same, no need to re-randomize.

	/* Initialize the target counts. */
	uint force_build = 0;  // Number of industries that should always be available.
	uint32_t total_prob = 0; // Sum of probabilities.
	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		IndustryTypeBuildData *ibd = this->builddata + it;
		force_build += ibd->min_number;
		ibd->target_count = ibd->min_number;
		total_prob += ibd->probability;
	}

	if (total_prob == 0) return; // No buildable industries.

	/* Subtract forced industries from the number of industries available for construction. */
	total_amount = (total_amount <= force_build) ? 0 : total_amount - force_build;

	/* Assign number of industries that should be aimed for, by using the probability as a weight. */
	while (total_amount > 0) {
		uint32_t r = RandomRange(total_prob);
		IndustryType it = 0;
		while (r >= this->builddata[it].probability) {
			r -= this->builddata[it].probability;
			it++;
			assert(it < NUM_INDUSTRYTYPES);
		}
		assert(this->builddata[it].probability > 0);
		this->builddata[it].target_count++;
		total_amount--;
	}
}

/**
 * Try to create a random industry, during gameplay
 */
void IndustryBuildData::TryBuildNewIndustry()
{
	this->SetupTargetCount();

	int missing = 0;       // Number of industries that need to be build.
	uint count = 0;        // Number of industry types eligible for build.
	uint32_t total_prob = 0; // Sum of probabilities.
	IndustryType forced_build = NUM_INDUSTRYTYPES; // Industry type that should be forcibly build.
	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		int difference = this->builddata[it].target_count - Industry::GetIndustryTypeCount(it);
		missing += difference;
		if (this->builddata[it].wait_count > 0) continue; // This type may not be built now.
		if (difference > 0) {
			if (Industry::GetIndustryTypeCount(it) == 0 && this->builddata[it].min_number > 0) {
				/* An industry that should exist at least once, is not available. Force it, trying the most needed one first. */
				if (forced_build == NUM_INDUSTRYTYPES ||
						difference > this->builddata[forced_build].target_count - Industry::GetIndustryTypeCount(forced_build)) {
					forced_build = it;
				}
			}
			total_prob += difference;
			count++;
		}
	}

	if (EconomyIsInRecession() || (forced_build == NUM_INDUSTRYTYPES && (missing <= 0 || total_prob == 0))) count = 0; // Skip creation of an industry.

	if (count >= 1) {
		/* If not forced, pick a weighted random industry to build.
		 * For the case that count == 1, there is no need to draw a random number. */
		IndustryType it;
		if (forced_build != NUM_INDUSTRYTYPES) {
			it = forced_build;
		} else {
			/* Non-forced, select an industry type to build (weighted random). */
			uint32_t r = 0; // Initialized to silence the compiler.
			if (count > 1) r = RandomRange(total_prob);
			for (it = 0; it < NUM_INDUSTRYTYPES; it++) {
				if (this->builddata[it].wait_count > 0) continue; // Type may not be built now.
				int difference = this->builddata[it].target_count - Industry::GetIndustryTypeCount(it);
				if (difference <= 0) continue; // Too many of this kind.
				if (count == 1) break;
				if (r < (uint)difference) break;
				r -= difference;
			}
			assert(it < NUM_INDUSTRYTYPES && this->builddata[it].target_count > Industry::GetIndustryTypeCount(it));
		}

		/* Try to create the industry. */
		const Industry *ind = PlaceIndustry(it, IndustryAvailabilityCallType::RandomCreation, false);
		if (ind == nullptr) {
			this->builddata[it].wait_count = this->builddata[it].max_wait + 1; // Compensate for decrementing below.
			this->builddata[it].max_wait = std::min(1000, this->builddata[it].max_wait + 2);
		} else {
			AdvertiseIndustryOpening(ind);
			this->builddata[it].max_wait = std::max(this->builddata[it].max_wait / 2, 1); // Reduce waiting time of the industry type.
		}
	}

	/* Decrement wait counters. */
	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		if (this->builddata[it].wait_count > 0) this->builddata[it].wait_count--;
	}
}

/**
 * Protects an industry from closure if the appropriate flags and conditions are met
 * CanCloseLastInstance must be set (which, by default, it is not) and the
 * count of industries of this type must one (or lower) in order to be protected
 * against closure.
 * @param type IndustryType been queried
 * @result true if protection is on, false otherwise (except for oil wells)
 */
static bool CheckIndustryCloseDownProtection(IndustryType type)
{
	const IndustrySpec *indspec = GetIndustrySpec(type);

	/* oil wells (or the industries with that flag set) are always allowed to closedown */
	if (indspec->behaviour.Test(IndustryBehaviour::DontIncrProd) && _settings_game.game_creation.landscape == LandscapeType::Temperate) return false;
	return !indspec->behaviour.Test(IndustryBehaviour::CanCloseLastInstance) && Industry::GetIndustryTypeCount(type) <= 1;
}

/**
 * Can given cargo type be accepted or produced by the industry?
 * @param cargo Cargo type to consider.
 * @param ind The industry to consider.
 * @param *c_accepts Pointer to boolean for acceptance of cargo
 * @param *c_produces Pointer to boolean for production of cargo
 * @post \c *c_accepts is set when industry accepts the cargo type,
 *       \c *c_produces is set when the industry produces the cargo type
 */
static void CanCargoServiceIndustry(CargoType cargo, Industry *ind, bool *c_accepts, bool *c_produces)
{
	if (!IsValidCargoType(cargo)) return;

	/* Check for acceptance of cargo */
	if (ind->IsCargoAccepted(cargo) && !IndustryTemporarilyRefusesCargo(ind, cargo)) *c_accepts = true;

	/* Check for produced cargo */
	if (ind->IsCargoProduced(cargo)) *c_produces = true;
}

/**
 * Compute who can service the industry.
 *
 * Here, 'can service' means that they have trains and stations close enough
 * to the industry with the right cargo type and the right orders (ie has the
 * technical means).
 *
 * @param ind: Industry being investigated.
 *
 * @return: 0 if nobody can service the industry, 2 if the local company can
 * service the industry, and 1 otherwise (only competitors can service the
 * industry)
 */
int WhoCanServiceIndustry(Industry *ind)
{
	if (ind->stations_near.empty()) return 0; // No stations found at all => nobody services

	int result = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		/* Is it worthwhile to try this vehicle? */
		if (v->owner != _local_company && result != 0) continue;

		/* Check whether it accepts the right kind of cargo */
		bool c_accepts = false;
		bool c_produces = false;
		if (v->type == VehicleType::Train && v->IsFrontEngine()) {
			for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				CanCargoServiceIndustry(u->cargo_type, ind, &c_accepts, &c_produces);
			}
		} else if (v->type == VehicleType::Road || v->type == VehicleType::Ship || v->type == VehicleType::Aircraft) {
			CanCargoServiceIndustry(v->cargo_type, ind, &c_accepts, &c_produces);
		} else {
			continue;
		}
		if (!c_accepts && !c_produces) continue; // Wrong cargo

		/* Check orders of the vehicle.
		 * We cannot check the first of shared orders only, since the first vehicle in such a chain
		 * may have a different cargo type.
		 */
		for (const Order &o : v->Orders()) {
			if (o.IsType(OT_GOTO_STATION) && o.GetUnloadType() != OrderUnloadType::Transfer) {
				/* Vehicle visits a station to load or unload */
				Station *st = Station::Get(o.GetDestination().ToStationID());
				assert(st != nullptr);

				/* Same cargo produced by industry is dropped here => not serviced by vehicle v */
				if (o.GetUnloadType() == OrderUnloadType::Unload && !c_accepts) break;

				if (ind->stations_near.find(st) != ind->stations_near.end()) {
					if (v->owner == _local_company) return 2; // Company services industry
					result = 1; // Competitor services industry
				}
			}
		}
	}
	return result;
}

/**
 * Report news that industry production has changed significantly
 *
 * @param ind Industry with changed production
 * @param cargo Cargo type that has changed
 * @param percent Percentage of change (>0 means increase, <0 means decrease)
 */
static void ReportNewsProductionChangeIndustry(Industry *ind, CargoType cargo, int percent)
{
	NewsType nt;

	switch (WhoCanServiceIndustry(ind)) {
		case 0: nt = NewsType::IndustryNobody;  break;
		case 1: nt = NewsType::IndustryOther;   break;
		case 2: nt = NewsType::IndustryCompany; break;
		default: NOT_REACHED();
	}
	AddIndustryNewsItem(
		GetEncodedString(percent >= 0 ? STR_NEWS_INDUSTRY_PRODUCTION_INCREASE_SMOOTH : STR_NEWS_INDUSTRY_PRODUCTION_DECREASE_SMOOTH,
			CargoSpec::Get(cargo)->name, ind->index, abs(percent)
		),
		nt,
		ind->index
	);
}

static const uint PERCENT_TRANSPORTED_60 = 153;
static const uint PERCENT_TRANSPORTED_80 = 204;

/**
 * Change industry production or do closure
 * @param i Industry for which changes are performed
 * @param monthly true if it's the monthly call, false if it's the random call
 */
static void ChangeIndustryProduction(Industry *i, bool monthly)
{
	StringID str = STR_NULL;
	bool closeit = false;
	const IndustrySpec *indspec = GetIndustrySpec(i->type);
	bool standard = false;
	bool suppress_message = false;
	bool recalculate_multipliers = false; ///< reinitialize production_rate to match prod_level
	/* use original economy for industries using production related callbacks */
	bool original_economy = indspec->UsesOriginalEconomy();
	uint8_t div = 0;
	uint8_t mul = 0;
	int8_t increment = 0;

	bool callback_enabled = indspec->callback_mask.Test(monthly ? IndustryCallbackMask::MonthlyProdChange : IndustryCallbackMask::ProductionChange);
	if (callback_enabled) {
		std::array<int32_t, 1> regs100;
		uint16_t res = GetIndustryCallback(monthly ? CBID_INDUSTRY_MONTHLYPROD_CHANGE : CBID_INDUSTRY_PRODUCTION_CHANGE, 0, Random(), i, i->type, i->location.tile, regs100);
		if (res != CALLBACK_FAILED) { // failed callback means "do nothing"
			suppress_message = HasBit(res, 7);
			/* Get the custom message if any */
			if (HasBit(res, 8)) str = MapGRFStringID(indspec->grf_prop.grfid, GRFStringID(GB(regs100[0], 0, 16)));
			res = GB(res, 0, 4);
			switch (res) {
				default: NOT_REACHED();
				case 0x0: break;                  // Do nothing, but show the custom message if any
				case 0x1: div = 1; break;         // Halve industry production. If production reaches the quarter of the default, the industry is closed instead.
				case 0x2: mul = 1; break;         // Double industry production if it hasn't reached eight times of the original yet.
				case 0x3: closeit = true; break;  // The industry announces imminent closure, and is physically removed from the map next month.
				case 0x4: standard = true; break; // Do the standard random production change as if this industry was a primary one.
				case 0x5: case 0x6: case 0x7:     // Divide production by 4, 8, 16
				case 0x8: div = res - 0x3; break; // Divide production by 32
				case 0x9: case 0xA: case 0xB:     // Multiply production by 4, 8, 16
				case 0xC: mul = res - 0x7; break; // Multiply production by 32
				case 0xD:                         // decrement production
				case 0xE:                         // increment production
					increment = res == 0x0D ? -1 : 1;
					break;
				case 0xF:                         // Set production to third byte of register 0x100
					i->prod_level = Clamp(GB(regs100[0], 16, 8), PRODLEVEL_MINIMUM, PRODLEVEL_MAXIMUM);
					recalculate_multipliers = true;
					break;
			}
		}
	} else {
		if (monthly == original_economy) return;
		if (!original_economy && _settings_game.economy.type == EconomyType::Frozen) return;
		if (indspec->life_type == INDUSTRYLIFE_BLACK_HOLE) return;
	}

	if (standard || (!callback_enabled && indspec->life_type.Any({IndustryLifeType::Organic, IndustryLifeType::Extractive}))) {
		/* decrease or increase */
		bool only_decrease = indspec->behaviour.Test(IndustryBehaviour::DontIncrProd) && _settings_game.game_creation.landscape == LandscapeType::Temperate;

		if (original_economy) {
			if (only_decrease || Chance16(1, 3)) {
				/* If more than 60% transported, 66% chance of increase, else 33% chance of increase */
				if (!only_decrease && (i->GetProduced(0).history[LAST_MONTH].PctTransported() > PERCENT_TRANSPORTED_60) != Chance16(1, 3)) {
					mul = 1; // Increase production
				} else {
					div = 1; // Decrease production
				}
			}
		} else if (_settings_game.economy.type == EconomyType::Smooth) {
			closeit = !i->ctlflags.Any({IndustryControlFlag::NoClosure, IndustryControlFlag::NoProductionDecrease});
			for (auto &p : i->produced) {
				if (!IsValidCargoType(p.cargo)) continue;
				uint32_t r = Random();
				int old_prod, new_prod, percent;
				new_prod = old_prod = p.rate;

				/* 1 in 22 chance to change production rate, randomly up/down depending on percent transported last month */
				if (Chance16I(1, 22, r >> 16)) {
					int prod_change_direction;
					if (only_decrease) {
						prod_change_direction = -1;
					} else if (p.history[LAST_MONTH].PctTransported() <= PERCENT_TRANSPORTED_60) {
						prod_change_direction = Chance16I(1, 3, r) ? 1 : -1;
					} else if (p.history[LAST_MONTH].PctTransported() <= PERCENT_TRANSPORTED_80) {
						prod_change_direction = Chance16I(2, 3, r) ? 1 : -1;
					} else {
						prod_change_direction = Chance16I(5, 6, r) ? 1 : -1;
					}
					new_prod += prod_change_direction * (std::max(((RandomRange(50) + 10) * old_prod) >> 8, 1U));
				}

				/* Prevent production to overflow or Oil Rig passengers to be over-"produced" */
				new_prod = Clamp(new_prod, 1, 255);
				if (IsValidCargoType(p.cargo) && p.cargo == GetCargoTypeByLabel(CT_PASSENGERS) && !indspec->behaviour.Test(IndustryBehaviour::NoPaxProdClamp)) {
					new_prod = Clamp(new_prod, 0, 16);
				}

				/* If override flags are set, prevent actually changing production if any was decided on */
				if (i->ctlflags.Test(IndustryControlFlag::NoProductionDecrease) && new_prod < old_prod) continue;
				if (i->ctlflags.Test(IndustryControlFlag::NoProductionIncrease) && new_prod > old_prod) continue;

				/* Do not stop closing the industry when it has the lowest possible production rate */
				if (new_prod == old_prod && old_prod > 1) {
					closeit = false;
					continue;
				}

				percent = (old_prod == 0) ? 100 : (new_prod * 100 / old_prod - 100);
				p.rate = new_prod;

				/* Close the industry when it has the lowest possible production rate */
				if (new_prod > 1) closeit = false;

				if (abs(percent) >= 10) {
					ReportNewsProductionChangeIndustry(i, p.cargo, percent);
				}
			}
		}
	}

	/* If override flags are set, prevent actually changing production if any was decided on */
	if (i->ctlflags.Test(IndustryControlFlag::NoProductionDecrease) && (div > 0 || increment < 0)) return;
	if (i->ctlflags.Test(IndustryControlFlag::NoProductionIncrease) && (mul > 0 || increment > 0)) return;
	if (i->ctlflags.Test(IndustryControlFlag::ExternalProdLevel)) {
		div = 0;
		mul = 0;
		increment = 0;
	}

	if (!callback_enabled && indspec->life_type.Test(IndustryLifeType::Processing)) {
		if (TimerGameEconomy::year - i->last_prod_year >= PROCESSING_INDUSTRY_ABANDONMENT_YEARS && Chance16(1, original_economy ? 2 : 180)) {
			closeit = true;
		}
	}

	/* Increase if needed */
	while (mul-- != 0 && i->prod_level < PRODLEVEL_MAXIMUM) {
		i->prod_level = std::min<int>(i->prod_level * 2, PRODLEVEL_MAXIMUM);
		recalculate_multipliers = true;
		if (str == STR_NULL) str = indspec->production_up_text;
	}

	/* Decrease if needed */
	while (div-- != 0 && !closeit) {
		if (i->prod_level == PRODLEVEL_MINIMUM) {
			closeit = true;
			break;
		} else {
			i->prod_level = std::max<int>(i->prod_level / 2, PRODLEVEL_MINIMUM);
			recalculate_multipliers = true;
			if (str == STR_NULL) str = indspec->production_down_text;
		}
	}

	/* Increase or Decreasing the production level if needed */
	if (increment != 0) {
		if (increment < 0 && i->prod_level == PRODLEVEL_MINIMUM) {
			closeit = true;
		} else {
			i->prod_level = ClampU(i->prod_level + increment, PRODLEVEL_MINIMUM, PRODLEVEL_MAXIMUM);
			recalculate_multipliers = true;
		}
	}

	/* Recalculate production_rate
	 * For non-smooth economy these should always be synchronized with prod_level */
	if (recalculate_multipliers) i->RecomputeProductionMultipliers();

	/* Close if needed and allowed */
	if (closeit && !CheckIndustryCloseDownProtection(i->type) && !i->ctlflags.Test(IndustryControlFlag::NoClosure)) {
		i->prod_level = PRODLEVEL_CLOSURE;
		SetWindowDirty(WindowClass::IndustryView, i->index);
		str = indspec->closure_text;
	}

	if (!suppress_message && str != STR_NULL) {
		NewsType nt;
		/* Compute news category */
		if (closeit) {
			nt = NewsType::IndustryClose;
			AI::BroadcastNewEvent(new ScriptEventIndustryClose(i->index));
			Game::NewEvent(new ScriptEventIndustryClose(i->index));
		} else {
			switch (WhoCanServiceIndustry(i)) {
				case 0: nt = NewsType::IndustryNobody;  break;
				case 1: nt = NewsType::IndustryOther;   break;
				case 2: nt = NewsType::IndustryCompany; break;
				default: NOT_REACHED();
			}
		}
		/* Set parameters of news string */
		EncodedString headline;
		if (str > STR_LAST_STRINGID) {
			headline = GetEncodedString(str, STR_TOWN_NAME, i->town->index, indspec->name);
		} else if (closeit) {
			headline = GetEncodedString(str, STR_FORMAT_INDUSTRY_NAME, i->town->index, indspec->name);
		} else {
			headline = GetEncodedString(str, i->index);
		}
		/* and report the news to the user */
		if (closeit) {
			AddTileNewsItem(std::move(headline), nt, i->location.tile + TileDiffXY(1, 1));
		} else {
			AddIndustryNewsItem(std::move(headline), nt, i->index);
		}
	}
}

/**
 * Every economy day handler for the industry changes
 * Taking the original map size of 256*256, the number of random changes was always of just one unit.
 * But it cannot be the same on smaller or bigger maps. That number has to be scaled up or down.
 * For small maps, it implies that less than one change per month is required, while on bigger maps,
 * it would be way more. The daily loop handles those changes.
 */
static const IntervalTimer<TimerGameEconomy> _economy_industries_daily({TimerGameEconomy::Trigger::Day, TimerGameEconomy::Priority::Industry}, [](auto)
{
	TendMarijuanaPlantations();

	_economy.industry_daily_change_counter += _economy.industry_daily_increment;

	/* Bits 16-31 of industry_construction_counter contain the number of industries to change/create today,
	 * the lower 16 bit are a fractional part that might accumulate over several days until it
	 * is sufficient for an industry. */
	uint16_t change_loop = _economy.industry_daily_change_counter >> 16;

	/* Reset the active part of the counter, just keeping the "fractional part" */
	_economy.industry_daily_change_counter &= 0xFFFF;

	if (change_loop == 0) {
		return;  // Nothing to do? get out
	}

	AutoRestoreBackup cur_company(_current_company, OWNER_NONE);

	/* perform the required industry changes for the day */

	uint perc = 3; // Between 3% and 9% chance of creating a new industry.
	if ((_industry_builder.wanted_inds >> 16) > GetCurrentTotalNumberOfIndustries()) {
		perc = std::min(9u, perc + (_industry_builder.wanted_inds >> 16) - GetCurrentTotalNumberOfIndustries());
	}
	for (uint16_t j = 0; j < change_loop; j++) {
		if (Chance16(perc, 100)) {
			_industry_builder.TryBuildNewIndustry();
		} else {
			Industry *i = Industry::GetRandom();
			if (i != nullptr) {
				ChangeIndustryProduction(i, false);
				SetWindowDirty(WindowClass::IndustryView, i->index);
			}
		}
	}

	/* production-change */
	InvalidateWindowData(WindowClass::IndustryDirectory, 0, IDIWD_PRODUCTION_CHANGE);
});

/** Economy monthly loop for industries. */
static const IntervalTimer<TimerGameEconomy> _economy_industries_monthly({TimerGameEconomy::Trigger::Month, TimerGameEconomy::Priority::Industry}, [](auto)
{
	AutoRestoreBackup cur_company(_current_company, OWNER_NONE);

	_industry_builder.EconomyMonthlyLoop();

	for (Industry *i : Industry::Iterate()) {
		UpdateIndustryStatistics(i);
		if (i->prod_level == PRODLEVEL_CLOSURE) {
			delete i;
		} else {
			ChangeIndustryProduction(i, true);
			SetWindowDirty(WindowClass::IndustryView, i->index);
		}
	}

	/* production-change */
	InvalidateWindowData(WindowClass::IndustryDirectory, 0, IDIWD_PRODUCTION_CHANGE);
});


void InitializeIndustries()
{
	Industry::industries.fill({});
	_industry_sound_tile = TileIndex{};

	_industry_builder.Reset();
}

/** Verify whether the generated industries are complete, and warn the user if not. */
void CheckIndustries()
{
	int count = 0;
	for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
		if (Industry::GetIndustryTypeCount(it) > 0) continue; // Types of existing industries can be skipped.

		bool force_at_least_one;
		uint32_t chance = GetScaledIndustryGenerationProbability(it, std::nullopt, &force_at_least_one);
		if (chance == 0 || !force_at_least_one) continue; // Types that are not available can be skipped.

		const IndustrySpec *is = GetIndustrySpec(it);
		ShowErrorMessage(GetEncodedString(STR_ERROR_NO_SUITABLE_PLACES_FOR_INDUSTRIES, is->name),
			GetEncodedString(STR_ERROR_NO_SUITABLE_PLACES_FOR_INDUSTRIES_EXPLANATION), WarningLevel::Warning);

		count++;
		if (count >= 3) break; // Don't swamp the user with errors.
	}
}

/**
 * Is an industry with the spec a raw industry?
 * @return true if it should be handled as a raw industry
 */
bool IndustrySpec::IsRawIndustry() const
{
	return this->life_type.Any({IndustryLifeType::Extractive, IndustryLifeType::Organic});
}

/**
 * Is an industry with the spec a processing industry?
 * @return true if it should be handled as a processing industry
 */
bool IndustrySpec::IsProcessingIndustry() const
{
	/* Lumber mills are neither raw nor processing */
	return this->life_type.Test(IndustryLifeType::Processing) &&
			!this->behaviour.Test(IndustryBehaviour::CutTrees);
}

/**
 * Get the cost for constructing this industry
 * @return the cost (inflation corrected etc)
 */
Money IndustrySpec::GetConstructionCost() const
{
	/* Building raw industries like secondary uses different price base */
	return (_price[(_settings_game.construction.raw_industry_construction == 1 && this->IsRawIndustry()) ?
			Price::BuildIndustryRaw : Price::BuildIndustry] * this->cost_multiplier) >> 8;
}

/**
 * Get the cost for removing this industry
 * Take note that the cost will always be zero for non-grf industries.
 * Only if the grf author did specified a cost will it be applicable.
 * @return the cost (inflation corrected etc)
 */
Money IndustrySpec::GetRemovalCost() const
{
	return (_price[Price::ClearIndustry] * this->removal_cost_multiplier) >> 8;
}

/**
 * Determines whether this industrytype uses standard/newgrf production changes.
 * @return true if original economy is used.
 */
bool IndustrySpec::UsesOriginalEconomy() const
{
	return _settings_game.economy.type == EconomyType::Original ||
		this->callback_mask.Any({
			IndustryCallbackMask::Production256Ticks,
			IndustryCallbackMask::ProductionCargoArrival, // production callbacks
			IndustryCallbackMask::MonthlyProdChange,
			IndustryCallbackMask::ProductionChange,
			IndustryCallbackMask::ProdChangeBuild}); // production change callbacks
}

/** @copydoc TerraformTileProc */
static CommandCost TerraformTile_Industry(TileIndex tile, DoCommandFlags flags, int z_new, Slope tileh_new)
{
	if (AutoslopeEnabled()) {
		/* We imitate here TTDP's behaviour:
		 *  - Both new and old slope must not be steep.
		 *  - TileMaxZ must not be changed.
		 *  - Allow autoslope by default.
		 *  - Disallow autoslope if callback succeeds and returns non-zero.
		 */
		Slope tileh_old = GetTileSlope(tile);
		/* TileMaxZ must not be changed. Slopes must not be steep. */
		if (!IsSteepSlope(tileh_old) && !IsSteepSlope(tileh_new) && (GetTileMaxZ(tile) == z_new + GetSlopeMaxZ(tileh_new))) {
			const IndustryGfx gfx = GetIndustryGfx(tile);
			const IndustryTileSpec *itspec = GetIndustryTileSpec(gfx);

			/* Call callback 3C 'disable autosloping for industry tiles'. */
			if (itspec->callback_mask.Test(IndustryTileCallbackMask::Autoslope)) {
				/* If the callback fails, allow autoslope. */
				uint16_t res = GetIndustryTileCallback(CBID_INDTILE_AUTOSLOPE, 0, 0, gfx, Industry::GetByTile(tile), tile);
				if (res == CALLBACK_FAILED || !ConvertBooleanCallback(itspec->grf_prop.grffile, CBID_INDTILE_AUTOSLOPE, res)) return CommandCost(ExpensesType::Construction, _price[Price::BuildFoundation]);
			} else {
				/* allow autoslope */
				return CommandCost(ExpensesType::Construction, _price[Price::BuildFoundation]);
			}
		}
	}
	return Command<Commands::LandscapeClear>::Do(flags, tile);
}

/** TileTypeProcs definitions for TileType::Industry tiles. */
extern const TileTypeProcs _tile_type_industry_procs = {
	.draw_tile_proc = DrawTile_Industry,
	.get_slope_pixel_z_proc = [](TileIndex tile, uint, uint, bool) { return GetTileMaxPixelZ(tile); },
	.clear_tile_proc = ClearTile_Industry,
	.add_accepted_cargo_proc = AddAcceptedCargo_Industry,
	.get_tile_desc_proc = GetTileDesc_Industry,
	.click_tile_proc = ClickTile_Industry,
	.animate_tile_proc = AnimateTile_Industry,
	.tile_loop_proc = TileLoop_Industry,
	.change_tile_owner_proc = ChangeTileOwner_Industry,
	.get_foundation_proc = GetFoundation_Industry,
	.terraform_tile_proc = TerraformTile_Industry,
};

bool IndustryCompare::operator() (const IndustryListEntry &lhs, const IndustryListEntry &rhs) const
{
	/* Compare by distance first and use index as a tiebreaker. */
	return std::tie(lhs.distance, lhs.industry->index) < std::tie(rhs.distance, rhs.industry->index);
}

/**
 * Remove unused industry accepted/produced slots -- entries after the last slot with valid cargo.
 * @param ind Industry to trim slots.
 */
void TrimIndustryAcceptedProduced(Industry *ind)
{
	auto ita = std::find_if(std::rbegin(ind->accepted), std::rend(ind->accepted), [](const auto &a) { return IsValidCargoType(a.cargo); });
	ind->accepted.erase(ita.base(), std::end(ind->accepted));
	ind->accepted.shrink_to_fit();

	auto itp = std::find_if(std::rbegin(ind->produced), std::rend(ind->produced), [](const auto &p) { return IsValidCargoType(p.cargo); });
	ind->produced.erase(itp.base(), std::end(ind->produced));
	ind->produced.shrink_to_fit();
}
