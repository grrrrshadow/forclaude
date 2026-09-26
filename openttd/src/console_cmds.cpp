/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file console_cmds.cpp Implementation of the console hooks. */

#include "stdafx.h"
#include "train.h"
#include "ship.h"
#include "depot_base.h"
#include "water_map.h"
#include "water_cmd.h"
#include "road_cmd.h"
#include "roadveh.h"
#include "industry.h"
#include "town.h"
#include "town_cmd.h"
#include "town_map.h"
#include "house.h"
#include "clear_map.h"
#include "tree_map.h"
#include "rail_map.h"
#include "road_map.h"
#include "mars_houses.h"
#include "cargomonitor.h"
#include "climate_industries.h"
#include "cargotype.h"
#include "spritecache.h"
#include "green_load.h"
#include "base_media_graphics.h"
#include "table/sprites.h"
#include "core/string_consumer.hpp"
#include "console_internal.h"
#include "console_gui.h"
#include "debug.h"
#include "engine_func.h"
#include "landscape.h"
#include "saveload/saveload.h"
#include "network/core/network_game_info.h"
#include "network/network.h"
#include "network/network_func.h"
#include "network/network_base.h"
#include "network/network_admin.h"
#include "network/network_client.h"
#include "command_func.h"
#include "aircraft.h"
#include "airport.h"
#include "linkgraph/linkgraphschedule.h"
#include "timer/timer_game_economy.h"
#include "gui.h"
#include "window_gui.h"
#include "settings_func.h"
#include "settings_internal.h"
#include "fios.h"
#include "fileio_func.h"
#include "fontcache.h"
#include "screenshot.h"
#include "genworld.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "company_func.h"
#include "signal_func.h"
#include "pbs.h"
#include "vehicle_func.h"
#include "articulated_vehicles.h"
#include "cargopacket.h"
#include "station_cmd.h"
#include "tree_cmd.h"
#include "order_cmd.h"
#include "order_func.h"
#include "timetable_cmd.h"
#include "order_base.h"
#include "order_func.h"
#include "depot_map.h"
#include "station_map.h"
#include "tilehighlight_func.h"
#include "newgrf_station.h"
#include "train_cmd.h"
#include "gamelog.h"
#include "ai/ai.hpp"
#include "ai/ai_config.hpp"
#include "newgrf.h"
#include "newgrf_profiling.h"
#include "newgrf_signals.h"
#include "console_func.h"
#include "engine_base.h"
#include "effectvehicle_base.h"
#include "effectvehicle_func.h"
#include "news_gui.h"
#include "news_type.h"
#include "road.h"
#include "rail.h"
#include "tunnelbridge_map.h"
#include "game/game.hpp"
#include "3rdparty/fmt/chrono.h"
#include "company_cmd.h"
#include "misc_cmd.h"
#include "rail_cmd.h"
#include "landscape_cmd.h"
#include "industrytype.h"
#include "industry_cmd.h"
#include "terraform_cmd.h"
#include "waypoint_cmd.h"
#include "waypoint_base.h"
#include "waypoint_func.h"
#include "vehicle_gui.h"
#include "widgets/vehicle_widget.h"
#include "widgets/misc_widget.h"
#include "widgets/order_widget.h"
#include "widgets/town_widget.h"
#include "station_base.h"
#include "vehicle_cmd.h"
#include "newgrf_engine.h"
#include "tile_map.h"
#include "core/backup_type.hpp"

#if defined(WITH_ZLIB)
#include "network/network_content.h"
#endif /* WITH_ZLIB */

#include "table/strings.h"

#include "mouse_debug.h"
#include "anomaly_log.h"
#include "road_on_rail.h"

#include "safeguards.h"

/* scriptfile handling */
static uint _script_current_depth; ///< Depth of scripts running (used to abort execution when #ConReturn is encountered).

/* Scheduled execution handling. */
static std::string _scheduled_monthly_script; ///< Script scheduled to execute by the 'schedule' console command (empty if no script is scheduled).

/** Timer that runs every month of game time for the 'schedule' console command. */
static const IntervalTimer<TimerGameCalendar> _scheduled_monthly_timer = {{TimerGameCalendar::Trigger::Month, TimerGameCalendar::Priority::None}, [](auto) {
	if (_scheduled_monthly_script.empty()) {
		return;
	}

	/* Clear the schedule before rather than after the script to allow the script to itself call
	 * schedule without it getting immediately cleared. */
	const std::string filename = _scheduled_monthly_script;
	_scheduled_monthly_script.clear();

	IConsolePrint(CC_DEFAULT, "Executing scheduled script file '{}'...", filename);
	IConsoleCmdExec(fmt::format("exec {}", filename));
}};

/**
 * Parse an integer using #ParseInteger and convert it to the requested type.
 * @param arg The string to be converted.
 * @tparam T The type to return.
 * @return The number in the given type, or std::nullopt when it could not be parsed.
 */
template <typename T>
static std::optional<T> ParseType(std::string_view arg)
{
	auto i = ParseInteger(arg);
	if (i.has_value()) return static_cast<T>(*i);
	return std::nullopt;
}

/** File list storage for the console, for caching the last 'ls' command. */
class ConsoleFileList : public FileList {
public:
	ConsoleFileList(AbstractFileType abstract_filetype, bool show_dirs) : FileList(), abstract_filetype(abstract_filetype), show_dirs(show_dirs)
	{
	}

	/** Declare the file storage cache as being invalid, also clears all stored files. */
	void InvalidateFileList()
	{
		this->clear();
		this->file_list_valid = false;
	}

	/**
	 * (Re-)validate the file storage cache. Only makes a change if the storage was invalid, or if \a force_reload.
	 * @param force_reload Always reload the file storage cache.
	 */
	void ValidateFileList(bool force_reload = false)
	{
		if (force_reload || !this->file_list_valid) {
			this->BuildFileList(this->abstract_filetype, SaveLoadOperation::Load, this->show_dirs);
			this->file_list_valid = true;
		}
	}

	AbstractFileType abstract_filetype; ///< The abstract file type to list.
	bool show_dirs; ///< Whether to show directories in the file list.
	bool file_list_valid = false; ///< If set, the file list is valid.
};

static ConsoleFileList _console_file_list_savegame{AbstractFileType::Savegame, true}; ///< File storage cache for savegames.
static ConsoleFileList _console_file_list_scenario{AbstractFileType::Scenario, false}; ///< File storage cache for scenarios.
static ConsoleFileList _console_file_list_heightmap{AbstractFileType::Heightmap, false}; ///< File storage cache for heightmaps.

/****************
 * command hooks
 ****************/

/**
 * Check network availability and inform in console about failure of detection.
 * @param echo Whether to print an error message or not.
 * @return \c true iff the network is available.
 */
static inline bool NetworkAvailable(bool echo)
{
	if (!_network_available) {
		if (echo) IConsolePrint(CC_ERROR, "You cannot use this command because there is no network available.");
		return false;
	}
	return true;
}

/**
 * Check whether we are a server.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookServerOnly(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (!_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is only available to a network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are a client in a network game.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookClientOnly(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is not available to a network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are in a multiplayer game.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNeedNetwork(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (!_networking || (!_network_server && !MyClient::IsConnected())) {
		if (echo) IConsolePrint(CC_ERROR, "Not connected. This command is only available in multiplayer.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are in a multiplayer game and are playing, i.e. we are not the dedicated server.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNeedNonDedicatedNetwork(bool echo)
{
	if (!NetworkAvailable(echo)) return ConsoleHookResult::Disallow;

	if (_network_dedicated) {
		if (echo) IConsolePrint(CC_ERROR, "This command is not available to a dedicated network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether we are in singleplayer mode.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNoNetwork(bool echo)
{
	if (_networking) {
		if (echo) IConsolePrint(CC_ERROR, "This command is forbidden in multiplayer.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check if are either in singleplayer or a server.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookServerOrNoNetwork(bool echo)
{
	if (_networking && !_network_server) {
		if (echo) IConsolePrint(CC_ERROR, "This command is only available to a network server.");
		return ConsoleHookResult::Disallow;
	}
	return ConsoleHookResult::Allow;
}

/**
 * Check whether NewGRF developer tools are enabled.
 * @copydoc IConsoleHook
 */
static ConsoleHookResult ConHookNewGRFDeveloperTool(bool echo)
{
	if (_settings_client.gui.newgrf_developer_tools) {
		if (_game_mode == GameMode::Menu) {
			if (echo) IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
			return ConsoleHookResult::Disallow;
		}
		return ConHookNoNetwork(echo);
	}
	return ConsoleHookResult::Hide;
}

/**
 * Reset status of all engines.
 * @copydoc IConsoleCmdProc
 */
static bool ConResetEngines(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reset status data of all engines. This might solve some issues with 'lost' engines. Usage: 'resetengines'.");
		return true;
	}

	StartupEngines();
	return true;
}

/**
 * Reset status of the engine pool.
 * @note Resetting the pool only succeeds when there are no vehicles ingame.
 * @copydoc IConsoleCmdProc
 */
static bool ConResetEnginePool(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reset NewGRF allocations of engine slots. This will remove invalid engine definitions, and might make default engines available again.");
		return true;
	}

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (!EngineOverrideManager::ResetToCurrentNewGRFConfig()) {
		IConsolePrint(CC_ERROR, "This can only be done when there are no vehicles in the game.");
		return true;
	}

	return true;
}

#ifdef _DEBUG
/**
 * Reset a tile to bare land in debug mode.
 * @copydoc IConsoleCmdProc
 */
static bool ConResetTile(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reset a tile to bare land. Usage: 'resettile <tile>'.");
		IConsolePrint(CC_HELP, "Tile can be either decimal (34161) or hexadecimal (0x4a5B).");
		return true;
	}

	if (argv.size() == 2) {
		auto result = ParseInteger(argv[1], 0);
		if (result.has_value() && IsValidTile(*result)) {
			DoClearSquare(TileIndex{*result});
			return true;
		}
	}

	return false;
}
#endif /* _DEBUG */

/**
 * Zoom map to given level.
 * @copydoc IConsoleCmdProc
 */
static bool ConZoomToLevel(std::span<std::string_view> argv)
{
	switch (argv.size()) {
		case 0:
			IConsolePrint(CC_HELP, "Set the current zoom level of the main viewport.");
			IConsolePrint(CC_HELP, "Usage: 'zoomto <level>'.");

			if (ZoomLevel::Min < _settings_client.gui.zoom_min) {
				IConsolePrint(CC_HELP, "The lowest zoom-in level allowed by current client settings is {}.", std::max(ZoomLevel::Min, _settings_client.gui.zoom_min));
			} else {
				IConsolePrint(CC_HELP, "The lowest supported zoom-in level is {}.", std::max(ZoomLevel::Min, _settings_client.gui.zoom_min));
			}

			if (_settings_client.gui.zoom_max < ZoomLevel::Max) {
				IConsolePrint(CC_HELP, "The highest zoom-out level allowed by current client settings is {}.", std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
			} else {
				IConsolePrint(CC_HELP, "The highest supported zoom-out level is {}.", std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
			}
			return true;

		case 2: {
			auto level = ParseInteger<std::underlying_type_t<ZoomLevel>>(argv[1]);
			if (level.has_value()) {
				auto zoom_lvl = static_cast<ZoomLevel>(*level);
				if (!IsInsideMM(zoom_lvl, ZoomLevel::Begin, ZoomLevel::End)) {
					IConsolePrint(CC_ERROR, "Invalid zoom level. Valid range is {} to {}.", ZoomLevel::Min, ZoomLevel::Max);
				} else if (!IsInsideMM(zoom_lvl, _settings_client.gui.zoom_min, _settings_client.gui.zoom_max + 1)) {
					IConsolePrint(CC_ERROR, "Current client settings limit zoom levels to range {} to {}.", _settings_client.gui.zoom_min, _settings_client.gui.zoom_max);
				} else {
					Window *w = GetMainWindow();
					Viewport &vp = *w->viewport;
					while (vp.zoom > zoom_lvl) DoZoomInOutWindow(ZOOM_IN, w);
					while (vp.zoom < zoom_lvl) DoZoomInOutWindow(ZOOM_OUT, w);
				}
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Testing bench: build one engine of the borrowed wagon set in a fresh depot and ask the
 * drawing code, for every cargo in the game, whether it finds a picture or falls back to
 * the substitute's sprites -- which is the "wrong vehicle" the player sees. Usage:
 * "cztr_test <local engine id in hex>". Leaves the depot and the vehicle standing.
 */
static bool ConCztrTest(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build a wagon of the borrowed set and test-draw it carrying every cargo.");
		IConsolePrint(CC_HELP, "Usage: 'cztr_test <local engine id in hex>', e.g. 'cztr_test b1'.");
		return true;
	}
	if (argv.size() < 2) return false;

	bool all = argv[1] == "all";
	std::vector<const Engine *> engines;
	if (all) {
		for (const Engine *e : Engine::Iterate()) {
			if (e->type != VehicleType::Train || !e->has_drawn_cargoes) continue;
			if (!e->info.climates.Any()) continue; // articulated parts come along with their heads
			engines.push_back(e);
		}
	} else {
		auto local_id = ParseInteger<uint16_t>(argv[1], 16);
		if (!local_id.has_value()) return false;
		for (const Engine *e : Engine::Iterate()) {
			if (e->type != VehicleType::Train || !e->has_drawn_cargoes) continue;
			if (e->grf_prop.local_id == *local_id) { engines.push_back(e); break; }
		}
	}
	if (engines.empty()) {
		IConsolePrint(CC_ERROR, "No engine of the borrowed set matches.");
		return true;
	}

	/* A headless game has no company yet; the commands below need one to act as. */
	if (!Company::IsValidID(CompanyID::Begin())) {
		Command<Commands::CompanyControl>::Do(DoCommandFlag::Execute, CompanyCtrlAction::New, CompanyID::Invalid(), CompanyRemoveReason{}, ClientID::Invalid);
		if (!Company::IsValidID(CompanyID::Begin())) {
			IConsolePrint(CC_ERROR, "Could not create a company to test with.");
			return true;
		}
	}

	AutoRestoreBackup cur_company(_current_company, CompanyID::Begin());

	/* Sandbox conditions: this set turns every original train off, so a game with no
	 * engine set at all has no engine to derive a rail type from, and nothing could be
	 * built. The bench is here to test drawing, not availability -- and not money. */
	Company::Get(_current_company)->avail_railtypes.Set(RAILTYPE_RAIL);
	Company::Get(_current_company)->money = INT64_MAX / 2;

	/* A flat, empty spot the depot command actually accepts; the command is the judge. */
	TileIndex depot_tile = INVALID_TILE;
	for (const auto tile : Map::Iterate()) {
		if (!IsTileType(tile, TileType::Clear) || GetTileSlope(tile) != SLOPE_FLAT) continue;
		if (TileX(tile) < 2 || TileY(tile) < 2 || TileX(tile) > Map::MaxX() - 2 || TileY(tile) > Map::MaxY() - 2) continue;
		if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, tile, RAILTYPE_RAIL, DiagDirection::SE).Succeeded()) {
			depot_tile = tile;
			break;
		}
	}
	if (depot_tile == INVALID_TILE) {
		/* Say why, from one representative attempt. */
		for (const auto tile : Map::Iterate()) {
			if (!IsTileType(tile, TileType::Clear) || GetTileSlope(tile) != SLOPE_FLAT) continue;
			if (TileX(tile) < 2 || TileY(tile) < 2) continue;
			CommandCost why = Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, tile, RAILTYPE_RAIL, DiagDirection::SE);
			uint trains = 0, defaults = 0, engines_avail = 0;
			for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
				trains++;
				if (e->GetGRF() == nullptr) defaults++;
				if (e->info.climates.Test(_settings_game.game_creation.landscape) &&
						e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon &&
						TimerGameCalendar::date >= e->intro_date + CalendarTime::DAYS_IN_YEAR) engines_avail++;
			}
			IConsolePrint(CC_ERROR, "Could not build the test depot anywhere; tile {:#x} says: {} (company {} railtypes {:#x} money {} date {} trains {} defaults {} engines_avail {})",
					TileIndex(tile).base(), why.GetErrorMessage() == INVALID_STRING_ID ? "(no message)" : GetString(why.GetErrorMessage()),
					_current_company.base(), Company::Get(_current_company)->avail_railtypes.base(), (int64_t)Company::Get(_current_company)->money,
					TimerGameCalendar::date.base(), trains, defaults, engines_avail);
			break;
		}
		return true;
	}

	for (const Engine *engine : engines) {
		auto [build_ret, veh_id, refit_capacity, refit_mail, cargo_capacities] =
				Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_tile, engine->index, true, INVALID_CARGO, ClientID::Invalid);
		if (build_ret.Failed()) {
			IConsolePrint(CC_ERROR, "Could not build wagon {:#x}.", engine->grf_prop.local_id);
			continue;
		}

		Train *t = Train::GetIfValid(veh_id);
		if (t == nullptr) continue;

		std::string bad;
		for (const CargoSpec *cs : _sorted_cargo_specs) {
			CargoType cargo = cs->Index();
			if (!engine->info.refit_mask.Test(cargo)) continue;

			CommandCost refit_ret = std::get<0>(Command<Commands::RefitVehicle>::Do(DoCommandFlag::Execute, veh_id, cargo, 0, false, false, 0));

			/* Ask exactly what the player sees, piece by piece, in two directions: a
			 * picture that changes with the direction is the set's own, one that does not
			 * is the flat purchase picture, an empty one is a deliberately blank piece,
			 * and the substitute's sprites cannot come out of this path at all any more. */
			std::string verdict;
			for (const Train *u = t; u != nullptr; u = u->Next()) {
				VehicleSpriteSeq west, north;
				u->GetImage(Direction::W, EngineImageType::OnMap, &west);
				u->GetImage(Direction::N, EngineImageType::OnMap, &north);
				if (!west.IsValid()) {
					verdict += 'X';
				} else if (west.count == 1 && west.seq[0].sprite == SPR_EMPTY) {
					verdict += '.';
				} else if (west.count == north.count && std::equal(std::begin(west.seq), std::begin(west.seq) + west.count, std::begin(north.seq),
						[](const auto &a, const auto &b) { return a.sprite == b.sprite; })) {
					verdict += 'o';
				} else {
					verdict += 'O';
				}
			}

			if (!all) {
				IConsolePrint(CC_DEFAULT, "cargo {:2d} {}: refit {} sprites {} (subtype {})",
						cargo, GetString(cs->name), refit_ret.Succeeded() ? "ok" : "REFUSED", verdict, t->cargo_subtype);
			}
			if (verdict.find('X') != std::string::npos || refit_ret.Failed()) bad += fmt::format(" {}({})", cargo, verdict);
		}

		IConsolePrint(CC_DEFAULT, "wagon {:#04x}: {}", engine->grf_prop.local_id, bad.empty() ? "all cargoes draw" : ("BAD:" + bad));

		Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, veh_id, true, false, ClientID::Invalid);
	}

	IConsolePrint(CC_DEFAULT, "O = own picture, o = own purchase picture, X = the substitute's sprites (the wrong vehicle).");
	return true;
}

/**
 * Turn the orientation marks in a train's status line on or off.
 *
 * Adds two letters to whatever the train is already saying: H or Z for whether the head
 * or the tail of the list goes first, D P or D Z for whether the head vehicle's nose
 * points away from the train or into it, and R with a number when something has claimed
 * this rake for collection.
 * @copydoc IConsoleCmdProc
 */
/**
 * Toggle legacy-decouple import mode: on the next 'load', vehicle and order
 * records belonging to a save from a foreign fork (a shape this build has no
 * field for) are walked past instead of decoded, so the map and everything
 * else on it can still come in. Nothing is decoded into vehicles; there are
 * none afterwards. See the comment on _sl_legacy_decouple_import.
 * @copydoc IConsoleCmdProc
 */
static bool ConLegacyDecoupleImport(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Import an old foreign-fork save's map and infrastructure, discarding its vehicles.");
		IConsolePrint(CC_HELP, "Usage: 'legacyimport' to flip it, or 'legacyimport on' / 'legacyimport off', then 'load <file>'.");
		return true;
	}

	/* This marks the request, not the reading -- the same thing the file
	 * window's checkbox marks when Load is pressed, and spent the same way,
	 * on the one load that follows. Emphatically not the live flag itself
	 * (_sl_legacy_decouple_import): left standing, that outlives the load it
	 * was meant for and is still in force when a failed load falls back to
	 * reading the intro game, which then comes up with no vehicles in it.
	 * Measured, not guessed -- the rig caught exactly that. */
	bool &want = _file_to_saveload.legacy_decouple_import;

	if (argv.size() >= 2) {
		if (argv[1] == "on" || argv[1] == "1") {
			want = true;
		} else if (argv[1] == "off" || argv[1] == "0") {
			want = false;
		} else {
			return false;
		}
	} else {
		want = !want;
	}

	IConsolePrint(CC_DEFAULT, "Legacy decouple import is now {}.", want ? "ON - vehicles will be discarded on next load" : "off");
	return true;
}

static bool ConShowTrainOrientation(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Spell out which way round a train is running in its status line.");
		IConsolePrint(CC_HELP, "Usage: 'vlak123' to flip it, or 'vlak123 on' / 'vlak123 off'.");
		return true;
	}

	if (argv.size() >= 2) {
		if (argv[1] == "on" || argv[1] == "1") {
			_show_train_orientation = true;
		} else if (argv[1] == "off" || argv[1] == "0") {
			_show_train_orientation = false;
		} else {
			return false;
		}
	} else {
		_show_train_orientation = !_show_train_orientation;
	}

	SetWindowClassesDirty(WindowClass::VehicleView);

	IConsolePrint(CC_DEFAULT, "Train orientation marks are now {}.", _show_train_orientation ? "shown" : "hidden");
	return true;
}

/**
 * Say why a command refused, without falling over when it did not say.
 *
 * A command may fail with no message at all (the build of an airport on a
 * hillside does), and asking the string table for "no string" takes the game
 * down -- which is a poor way for a test command to report a refusal.
 *
 * @param cost what the command answered
 * @return the reason, in words
 */
static std::string RefusalReason(const CommandCost &cost)
{
	StringID err = cost.GetErrorMessage();
	if (err == INVALID_STRING_ID) return "bez duvodu";
	return GetString(err);
}

/**
 * Put an airport down and buy an aircraft in it, so a raid can be flown
 * headless. Usage: testletadlo <x> <y>
 * @copydoc IConsoleCmdProc
 */
/**
 * Build a level crossing scene, or work it. Usage:
 *   testprejezd <x> <y>  -- a rail line, a road across it, a train and a lorry
 *   testprejezd stoj     -- stop the lorry, but only once it is on the crossing
 *   testprejezd vlak     -- let the train go
 *
 * A train hitting a road vehicle is the one thing on a crossing the game
 * does not do by itself: the barriers see to that, and only a vehicle
 * standing on the rails when they come down gets caught. Staged by hand,
 * because that is the only way it happens.
 * @copydoc IConsoleCmdProc
 */
/** The train of the level crossing scene, so "testprejezd vlak" starts that one. */
static VehicleID _testprejezd_train = VehicleID::Invalid();
/** The lorry of the level crossing scene, same reason. */
static VehicleID _testprejezd_lorry = VehicleID::Invalid();

static bool ConTestLevelCrossing(std::span<std::string_view> argv)
{
	if (argv.size() == 2 && argv[1] == "stav") {
		/* What the crossing cost: the train's breakdown and the company's
		 * purse, which is the whole point of the scene. */
		const Company *c = Company::GetIfValid(CompanyID::Begin());
		const Train *t = Train::GetIfValid(_testprejezd_train);
		/* The raid's hold is in ticks from now, so the line says how much of
		 * it is left rather than the tick it ends on. */
		uint64_t held = t == nullptr || t->raid_broken_until <= TimerGameTick::counter
				? 0 : t->raid_broken_until - TimerGameTick::counter;
		IConsolePrint(CC_DEFAULT, "testprejezd: penize {}, vlak {} porucha {}/{} nalet jeste {} tiku, rychlost {}, auticek na mape {}",
				c == nullptr ? Money(0) : c->money, t == nullptr ? 0 : (uint)t->unitnumber, t == nullptr ? 0 : (uint)t->breakdown_ctr,
				t == nullptr ? 0 : (uint)t->breakdown_delay, held,
				t == nullptr ? 0 : (uint)t->cur_speed, RoadVehicle::GetNumItems());
		return true;
	}

	if (argv.size() == 2 && (argv[1] == "stoj" || argv[1] == "vlak")) {
		/* Starting and stopping is somebody's, and the console is nobody:
		 * without this the command is refused for want of an owner and the
		 * vehicle drives cheerfully on. */
		AutoRestoreBackup stop_company(_current_company, CompanyID::Begin());
		if (argv[1] == "vlak") {
			/* The scene's own train, not whichever train the save happens to
			 * list first: rig.sav is full of them. */
			Train *t = Train::GetIfValid(_testprejezd_train);
			if (t == nullptr) {
				IConsolePrint(CC_ERROR, "testprejezd: scena jeste nestoji.");
				return true;
			}
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, t->index, false);
			IConsolePrint(CC_DEFAULT, "testprejezd: vlak {} vyjel z ({},{}).", t->unitnumber, TileX(t->tile), TileY(t->tile));
			return true;
		}
		RoadVehicle *only = RoadVehicle::GetIfValid(_testprejezd_lorry);
		for (RoadVehicle *rv : RoadVehicle::Iterate()) {
			if (rv->First() != rv) continue;
			if (only != nullptr && rv != only) continue;
			if (rv->vehstatus.Test(VehState::Stopped)) {
				TileIndex at = TileVirtXY(rv->x_pos, rv->y_pos);
				IConsolePrint(CC_DEFAULT, "testprejezd: auticko {} uz stoji na ({},{}), prejezd: {}.", rv->unitnumber,
						TileX(at), TileY(at), IsLevelCrossingTile(at) ? "ano" : "ne");
				return true;
			}
			TileIndex on = TileVirtXY(rv->x_pos, rv->y_pos);
			if (!IsLevelCrossingTile(on)) {
				IConsolePrint(CC_DEFAULT, "testprejezd: auticko {} je na ({},{}), jeste ne na prejezdu.",
						rv->unitnumber, TileX(on), TileY(on));
				return true;
			}
			CommandCost stop = Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, rv->index, false);
			IConsolePrint(stop.Succeeded() ? CC_DEFAULT : CC_ERROR, "testprejezd: auticko {} na prejezdu ({},{}) - {}", rv->unitnumber,
					TileX(on), TileY(on), stop.Succeeded() ? "zastaveno" : RefusalReason(stop));
			return true;
		}
		IConsolePrint(CC_ERROR, "testprejezd: zadne auticko.");
		return true;
	}

	if (argv.size() != 3) {
		IConsolePrint(CC_HELP, "Build a level crossing scene. Usage: 'testprejezd <x> <y>', then 'testprejezd stoj' and 'testprejezd vlak'.");
		return true;
	}
	auto px = ParseInteger(argv[1]);
	auto py = ParseInteger(argv[2]);
	if (!px.has_value() || !py.has_value()) return false;
	uint x0 = (uint)*px, y0 = (uint)*py;

	if (!Company::IsValidID(CompanyID::Begin())) {
		IConsolePrint(CC_ERROR, "testprejezd: hra nema firmu, pust to ze savu.");
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, CompanyID::Begin());

	/* Flat ground first: a crossing wants the rails and the road on one level. */
	Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, TileXY(x0 + 16, y0 + 3), TileXY(x0, y0 - 3), false, LevelMode::Level);

	TileIndex depot = TileXY(x0, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot, RAILTYPE_RAIL, DiagDirection::SW).Failed()) {
		IConsolePrint(CC_ERROR, "testprejezd: depo na ({},{}) nejde postavit.", x0, y0);
		return true;
	}
	CommandCost line = Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + 15, y0), TileXY(x0 + 1, y0),
			RAILTYPE_RAIL, Track::X, false, true);
	if (line.Failed()) {
		IConsolePrint(CC_ERROR, "testprejezd: trat nejde postavit - {}", RefusalReason(line));
		return true;
	}

	/* The road crosses it in the middle, with a depot at one end so a lorry
	 * can be bought and driven onto it. */
	uint cx = x0 + 8;
	CommandCost road = Command<Commands::BuildRoadLong>::Do(DoCommandFlag::Execute, TileXY(cx, y0 + 2), TileXY(cx, y0),
			ROADTYPE_ROAD, Axis::Y, DisallowedRoadDirections{}, false, false, false);
	if (road.Failed()) {
		IConsolePrint(CC_ERROR, "testprejezd: silnice nejde postavit - {}", RefusalReason(road));
		return true;
	}
	/* One shed each side of the rails, so the lorry has a reason to keep
	 * crossing them: a road vehicle with nowhere to go stays in its shed. */
	/* The near shed sits right against the rails, so the lorry's first step
	 * out of it is onto the crossing and the rig can stop it there. */
	TileIndex road_depot = TileXY(cx, y0 - 1);
	TileIndex road_depot_far = TileXY(cx, y0 + 3);
	CommandCost rd = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, road_depot, ROADTYPE_ROAD, DiagDirection::SE);
	CommandCost rd2 = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, road_depot_far, ROADTYPE_ROAD, DiagDirection::NW);
	if (rd.Failed() || rd2.Failed()) {
		IConsolePrint(CC_ERROR, "testprejezd: silnicni depo nejde postavit - {} / {}", RefusalReason(rd), RefusalReason(rd2));
		return true;
	}

	/* One of each, the first the year offers. */
	VehicleID train = VehicleID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, e->index, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) continue;
		train = veh;
		break;
	}
	VehicleID lorry = VehicleID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, road_depot, e->index, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) continue;
		lorry = veh;
		break;
	}
	if (train == VehicleID::Invalid() || lorry == VehicleID::Invalid()) {
		IConsolePrint(CC_ERROR, "testprejezd: vlak {} auticko {} - nejde koupit.",
				train == VehicleID::Invalid() ? "ne" : "ano", lorry == VehicleID::Invalid() ? "ne" : "ano");
		return true;
	}

	_testprejezd_train = train;
	_testprejezd_lorry = lorry;

	/* Back and forth between the two sheds, over the rails every time. */
	Depot *near_depot = Depot::GetByTile(road_depot);
	Depot *far_depot = Depot::GetByTile(road_depot_far);
	if (near_depot != nullptr && far_depot != nullptr) {
		Order there{};
		there.MakeGoToDepot(DestinationID(far_depot->index), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, lorry, 0, there);
		Order back{};
		back.MakeGoToDepot(DestinationID(near_depot->index), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, lorry, 1, back);
	}

	/* The lorry drives out at once and wanders the road; the train waits for
	 * "testprejezd vlak", once the lorry has been stopped on the rails. */
	CommandCost go = Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, lorry, false);
	if (go.Failed()) IConsolePrint(CC_ERROR, "testprejezd: auticko nechce vyjet - {}", RefusalReason(go));

	IConsolePrint(CC_DEFAULT, "testprejezd: prejezd na ({},{}) {}, vlak {} v depe ({},{}), auticko {} vyjelo z ({},{}).",
			cx, y0, IsLevelCrossingTile(TileXY(cx, y0)) ? "stoji" : "CHYBI", Vehicle::Get(train)->unitnumber, x0, y0,
			Vehicle::Get(lorry)->unitnumber, TileX(road_depot), TileY(road_depot));
	return true;
}

static bool ConTestBuildAircraft(std::span<std::string_view> argv)
{
	if (argv.size() != 3) {
		IConsolePrint(CC_HELP, "Build an airport and an aircraft in it. Usage: 'testletadlo <x> <y>'.");
		return true;
	}
	auto px = ParseInteger(argv[1]);
	auto py = ParseInteger(argv[2]);
	if (!px.has_value() || !py.has_value()) return false;

	TileIndex tile = TileXY((uint)*px, (uint)*py);

	/* A headless game has no company yet, and a command with nobody to act as
	 * refuses without a word. */
	if (!Company::IsValidID(CompanyID::Begin())) {
		Command<Commands::CompanyControl>::Do(DoCommandFlag::Execute, CompanyCtrlAction::New, CompanyID::Invalid(), CompanyRemoveReason{}, ClientID::Invalid);
		if (!Company::IsValidID(CompanyID::Begin())) {
			IConsolePrint(CC_ERROR, "testletadlo: nejde zalozit firma.");
			return true;
		}
	}
	AutoRestoreBackup cur_company(_current_company, CompanyID::Begin());

	/* An airport wants flat ground; the rig levels what it was pointed at
	 * rather than making the player hunt for a spot. */
	Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, TileXY((uint)*px + 5, (uint)*py + 5), tile, false, LevelMode::Level);

	CommandCost port = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, tile, 0, 0, StationID::Invalid(), true);
	if (port.Failed()) {
		IConsolePrint(CC_ERROR, "testletadlo: letiste na ({},{}) nejde postavit - {}", *px, *py, RefusalReason(port));
		return true;
	}

	/* The first aircraft the year offers; the rig does not care which. The
	 * hangar is where one is bought, not the airport's own tile. */
	TileIndex hangar = tile;
	for (TileIndex t : TileArea(tile, 6, 6)) {
		if (IsTileType(t, TileType::Station) && IsHangar(t)) { hangar = t; break; }
	}

	std::string why = "zadny typ letadla";
	uint tried = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Aircraft)) {
		tried++;
		auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, hangar, e->index, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) {
			why = RefusalReason(cost);
			continue;
		}
		/* And an order, so the orders window has a line to draw: an empty
		 * list exercises nothing. */
		Station *st = Station::GetByTile(tile);
		if (st != nullptr) {
			Order o{};
			o.MakeGoToStation(st->index);
			/* A fresh order stops at the near end of the platform, which only a
			 * train may do; anything else is refused without a word. The orders
			 * window sets this for the player and the rig has to as well. */
			o.SetStopLocation(OrderStopLocation::FarEnd);
			Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh, 0, o);
		}

		IConsolePrint(CC_DEFAULT, "testletadlo: letiste na ({},{}), hangar ({},{}), letadlo {} koupeno, rozkazu {}.", *px, *py,
				TileX(hangar), TileY(hangar), Vehicle::Get(veh)->unitnumber, Vehicle::Get(veh)->GetNumOrders());
		return true;
	}
	IConsolePrint(CC_ERROR, "testletadlo: letiste stoji, ale zadne z {} letadel nejde koupit - {}", tried, why);
	return true;
}

/**
 * Fly a raid at a spot, the way the crosshair button does. Usage:
 * testnalet <x> <y> [unit number of the aircraft]
 * @copydoc IConsoleCmdProc
 */
/**
 * Build a ship depot on the nearest open water and buy a ship in it.
 * Usage: testlod [x] [y]
 *
 * A ship's raid can only be measured with a ship, and a ship can only be
 * bought where there is water deep enough for a shed. The rig finds it
 * rather than the scene having to know where the sea is.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestBuildShip(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	if (argv.size() == 2 && argv[1] == "trasa") {
		/* Two harbours and a ship running between them: a ship out on a route
		 * is a different thing from one idle in a shed, and the player's raid
		 * went wrong on the first and right on the second. */
		if (!Company::IsValidID(CompanyID::Begin())) {
			IConsolePrint(CC_ERROR, "testlod: hra nema firmu, pust to ze savu.");
			return true;
		}
		AutoRestoreBackup cur_company(_current_company, CompanyID::Begin());

		Ship *sh = nullptr;
		for (Ship *candidate : Ship::Iterate()) {
			if (candidate->First() != candidate) continue;
			sh = candidate;
			break;
		}
		if (sh == nullptr) {
			IConsolePrint(CC_ERROR, "testlod: nejdriv 'testlod', pak 'testlod trasa'.");
			return true;
		}

		/* Where a harbour will stand, asked of the command rather than guessed
		 * at: the nearest one to the ship, and the furthest from that. */
		std::vector<TileIndex> sites;
		for (TileIndex t : Map::Iterate()) {
			if (Command<Commands::BuildDock>::Do(DoCommandFlags{}, t, StationID::Invalid(), false).Failed()) continue;
			sites.push_back(t);
		}
		if (sites.size() < 2) {
			IConsolePrint(CC_ERROR, "testlod: na mape nejsou dve mista na pristav ({}).", sites.size());
			return true;
		}
		TileIndex first = *std::min_element(sites.begin(), sites.end(),
				[&](TileIndex a, TileIndex b) { return DistanceManhattan(a, sh->tile) < DistanceManhattan(b, sh->tile); });
		TileIndex second = *std::max_element(sites.begin(), sites.end(),
				[&](TileIndex a, TileIndex b) { return DistanceManhattan(a, first) < DistanceManhattan(b, first); });

		CommandCost d1 = Command<Commands::BuildDock>::Do(DoCommandFlag::Execute, first, StationID::Invalid(), false);
		CommandCost d2 = Command<Commands::BuildDock>::Do(DoCommandFlag::Execute, second, StationID::Invalid(), false);
		if (d1.Failed() || d2.Failed()) {
			IConsolePrint(CC_ERROR, "testlod: pristav nejde postavit - {} / {}", RefusalReason(d1), RefusalReason(d2));
			return true;
		}

		/* Out with the shed order, in with the two harbours. */
		while (sh->GetNumOrders() > 0) {
			if (Command<Commands::DeleteOrder>::Do(DoCommandFlag::Execute, sh->index, 0).Failed()) break;
		}
		uint added = 0;
		for (TileIndex t : {first, second}) {
			Station *st = IsTileType(t, TileType::Station) ? Station::GetByTile(t) : nullptr;
			if (st == nullptr) {
				IConsolePrint(CC_ERROR, "testlod: na ({},{}) neni stanice", TileX(t), TileY(t));
				continue;
			}
			Order o{};
			o.MakeGoToStation(st->index);
			/* A fresh order stops at the near end of the platform, which only a
			 * train may do; anything else is refused without a word. The orders
			 * window sets this for the player and the rig has to as well. */
			o.SetStopLocation(OrderStopLocation::FarEnd);
			CommandCost ins = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, sh->index, added, o);
			if (ins.Succeeded()) {
				added++;
			} else {
				IConsolePrint(CC_ERROR, "testlod: rozkaz na stanici {} nejde vlozit - {}", st->index.base(), RefusalReason(ins));
			}
		}
		if (sh->vehstatus.Test(VehState::Stopped)) {
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, sh->index, false);
		}
		IConsolePrint(CC_DEFAULT, "testlod: pristavy na ({},{}) a ({},{}), lod {} ma {} rozkazu a jede.",
				TileX(first), TileY(first), TileX(second), TileY(second), sh->unitnumber, added);
		return true;
	}

	if (argv.size() == 2 && argv[1] == "stav") {
		for (const Ship *sh : Ship::Iterate()) {
			IConsolePrint(CC_DEFAULT, "testlod: lod {} na ({},{}) rychlost {} v depu {} cil ({},{}) nalet ({},{}) plout ({},{}) nejbliz {} stale {}",
					sh->unitnumber, TileX(sh->tile), TileY(sh->tile), sh->cur_speed, sh->IsInDepot() ? "ano" : "ne",
					TileX(sh->dest_tile), TileY(sh->dest_tile), TileX(sh->raid_target), TileY(sh->raid_target),
					TileX(sh->raid_sail_to), TileY(sh->raid_sail_to), sh->raid_closest, sh->raid_stale);
		}
		return true;
	}
	uint sx = argv.size() > 2 ? 0 : Map::MaxX() / 2;
	uint sy = argv.size() > 2 ? 0 : Map::MaxY() / 2;
	if (argv.size() > 2) {
		auto px = ParseInteger(argv[1]);
		auto py = ParseInteger(argv[2]);
		if (!px.has_value() || !py.has_value()) return false;
		sx = (uint)*px;
		sy = (uint)*py;
	}

	if (!Company::IsValidID(CompanyID::Begin())) {
		IConsolePrint(CC_ERROR, "testlod: hra nema firmu, pust to ze savu.");
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, CompanyID::Begin());

	/* Two tiles of water side by side, as near the asked-for spot as there
	 * is any. Tried for real rather than guessed at: the command knows what
	 * counts as buildable water and the rig does not need to. */
	TileIndex depot = INVALID_TILE;
	Axis found_axis = Axis::X;
	uint best = UINT_MAX;
	for (TileIndex t : Map::Iterate()) {
		if (!IsWaterTile(t)) continue;
		uint dist = DistanceManhattan(t, TileXY(sx, sy));
		if (dist >= best) continue;
		for (Axis a : {Axis::X, Axis::Y}) {
			if (Command<Commands::BuildShipDepot>::Do(DoCommandFlags{}, t, a).Failed()) continue;
			best = dist;
			depot = t;
			found_axis = a;
			break;
		}
	}
	if (depot == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testlod: nikde na mape neni voda na depo.");
		return true;
	}

	CommandCost built = Command<Commands::BuildShipDepot>::Do(DoCommandFlag::Execute, depot, found_axis);
	if (built.Failed()) {
		IConsolePrint(CC_ERROR, "testlod: depo na ({},{}) nejde postavit - {}", TileX(depot), TileY(depot), RefusalReason(built));
		return true;
	}

	std::string why = "zadny typ lodi";
	for (const Engine *e : Engine::IterateType(VehicleType::Ship)) {
		auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, e->index, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) {
			why = RefusalReason(cost);
			continue;
		}
		/* An order to its own shed, so the ship has something to go back to
		 * after the errand: without one there is nothing to prove it was put
		 * back where it was. */
		if (const Depot *d = Depot::GetByTile(depot); d != nullptr) {
			Order home{};
			home.MakeGoToDepot(DestinationID(d->index), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
			Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh, 0, home);
		}
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh, false);
		IConsolePrint(CC_DEFAULT, "testlod: depo na ({},{}), lod {} koupena, rozkazu {}.", TileX(depot), TileY(depot),
				Vehicle::Get(veh)->unitnumber, Vehicle::Get(veh)->GetNumOrders());
		return true;
	}
	IConsolePrint(CC_ERROR, "testlod: depo stoji na ({},{}), ale zadna lod nejde koupit - {}", TileX(depot), TileY(depot), why);
	return true;
}

static bool ConTestAirRaid(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Send a raid at a spot. Usage: 'testnalet <x> <y> [lod] [unit number]'.");
		return true;
	}
	auto px = ParseInteger(argv[1]);
	auto py = ParseInteger(argv[2]);
	if (!px.has_value() || !py.has_value()) return false;
	/* Which kind, and then which one: a ship's raid is a different journey
	 * from an aircraft's and has to be walked separately. */
	VehicleType want = VehicleType::Aircraft;
	std::optional<uint> unit;
	if (argv.size() > 3) {
		if (argv[3] == "lod") {
			want = VehicleType::Ship;
		} else {
			unit = ParseInteger(argv[3]);
			if (!unit.has_value()) return false;
		}
	}
	if (argv.size() > 4) {
		unit = ParseInteger(argv[4]);
		if (!unit.has_value()) return false;
	}

	Vehicle *raider = nullptr;
	uint candidates = 0;
	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != want || v->First() != v) continue;
		candidates++;
		if (unit.has_value() ? v->unitnumber == (UnitID)*unit : raider == nullptr) raider = v;
	}
	if (raider == nullptr) {
		IConsolePrint(CC_ERROR, "testnalet: zadne takove vozidlo (na mape jich je {}).", candidates);
		return true;
	}

	TileIndex tile = TileXY(*px, *py);
	AutoRestoreBackup cur_company(_current_company, raider->owner);
	CommandCost ret = Command<Commands::Raid>::Do(DoCommandFlag::Execute, tile, raider->index);
	IConsolePrint(CC_DEFAULT, "testnalet: {} {} na ({},{}): {}", want == VehicleType::Ship ? "lod" : "letadlo",
			raider->unitnumber, *px, *py, ret.Failed() ? RefusalReason(ret) : "nalet zadan");
	return true;
}

/**
 * Walk the crosshair the way a click does: open the aircraft's window, take
 * the crosshair, put it down on a spot, and let it go again.
 * Usage: testzamerit <x> <y> [unit number]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestAimCrosshair(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Aim and drop the crosshair, as the button does. Usage: 'testzamerit <x> <y> [unit number]'.");
		return true;
	}
	auto px = ParseInteger(argv[1]);
	auto py = ParseInteger(argv[2]);
	if (!px.has_value() || !py.has_value()) return false;

	const Vehicle *plane = nullptr;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Aircraft || v->First() != v || !v->IsPrimaryVehicle()) continue;
		if (argv.size() > 3) {
			auto punit = ParseInteger(argv[3]);
			if (punit.has_value() && v->unitnumber != (UnitID)*punit) continue;
		}
		plane = v;
		break;
	}
	if (plane == nullptr) {
		IConsolePrint(CC_ERROR, "testzamerit: zadne letadlo.");
		return true;
	}

	ShowVehicleViewWindow(plane);
	Window *w = FindWindowById(WindowClass::VehicleView, plane->index);
	if (w == nullptr) {
		IConsolePrint(CC_ERROR, "testzamerit: okno letadla se neotevrelo.");
		return true;
	}

	extern void SetObjectToPlaceWnd(CursorID icon, PaletteID pal, HighLightStyle mode, Window *w);
	SetObjectToPlaceWnd(SPR_CURSOR_CROSSHAIR, PAL_NONE, HT_RECT, w);
	IConsolePrint(CC_DEFAULT, "testzamerit: zamereno");

	Point pt = {0, 0};
	w->OnPlaceObject(pt, TileXY((uint)*px, (uint)*py));
	IConsolePrint(CC_DEFAULT, "testzamerit: polozeno na ({},{})", *px, *py);
	return true;
}

/**
 * Print the size of the button sprites, so a new icon can be drawn to match.
 * Usage: testikony
 * @copydoc IConsoleCmdProc
 */
static bool ConTestIconSizes(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	struct { const char *name; SpriteID sprite; } icons[] = {
		{"prikazy (SHOW_ORDERS)", SPR_SHOW_ORDERS},
		{"detaily", SPR_SHOW_VEHICLE_DETAILS},
		{"prestavba", SPR_REFIT_VEHICLE},
		{"otoceni", SPR_FORCE_VEHICLE_TURN},
		{"ignoruj navest", SPR_IGNORE_SIGNALS},
		{"do depa (vlak)", SPR_SEND_TRAIN_TODEPOT},
		{"do hangaru", SPR_SEND_AIRCRAFT_TODEPOT},
		{"klonovat", SPR_CLONE_AIRCRAFT},
		{"odtahovka", SPR_IMG_RESCUE_ENGINE},
		{"zamerovac", SPR_IMG_CROSSHAIR},
		{"kurzor zamerovace", SPR_CURSOR_CROSSHAIR},
	};
	/* The size the game reports counts from the drawing's own zero, so a
	 * sprite carrying a hook point -- a cursor, which is hung on the mouse by
	 * its middle -- comes back that much smaller. Print the hook point too,
	 * or a 32 pixel cursor hung by its middle reads as a 16 pixel one. */
	for (const auto &icon : icons) {
		Point offset;
		Dimension d = GetSpriteSize(icon.sprite, &offset);
		IConsolePrint(CC_DEFAULT, "testikony: {} (sprite {}) = {} x {}, kresba {} x {}, zaves {},{}", icon.name, icon.sprite,
				d.width, d.height, d.width - offset.x, d.height - offset.y, offset.x, offset.y);
	}
	/* Which file each one came from, not just how big it is: a base set's
	 * extra grf lands in the same block as this build's own icons, and one
	 * that reaches past the sprites the game upstream has takes them over
	 * without a word. */
	for (SpriteID id = SPR_OPENTTD_BASE + 205; id <= SPR_OPENTTD_BASE + 229; id++) {
		Dimension d = GetSpriteSize(id);
		SpriteFile *f = GetOriginFile(id);
		IConsolePrint(CC_DEFAULT, "testikony: sprite {} (base+{}) = {} x {} z {} #{}{}", id, id - SPR_OPENTTD_BASE, d.width, d.height,
				f == nullptr ? std::string("?") : f->GetSimplifiedFilename(), GetSpriteLocalID(id),
				SpriteExists(id) ? "" : " (NEEXISTUJE)");
	}
	return true;
}

/**
 * Say where the towns are, so a raid can be aimed at one. Usage: testmesta
 * @copydoc IConsoleCmdProc
 */
static bool ConTestTowns(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	for (const Town *t : Town::Iterate()) {
		IConsolePrint(CC_DEFAULT, "mesto {}: ({},{}) obyvatel {} domu {}", t->index.base(), TileX(t->xy), TileY(t->xy),
				t->cache.population, t->cache.num_houses);
	}
	return true;
}

/**
 * Move the calendar to a year the way the date cheat does -- set the date,
 * then let the monthly round introduce whatever has been invented by then.
 * For scenes that need what a later year has: aeroplanes big enough to carry
 * a car, houses that are not built before 2030.
 * @param year the year to move to
 */
static void MoveCalendarTo(TimerGameCalendar::Year year)
{
	extern void CalendarEnginesMonthlyLoop();
	TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
	TimerGameCalendar::Date moved = TimerGameCalendar::ConvertYMDToDate(year, ymd.month, ymd.day);
	TimerGameCalendar::SetDate(moved, TimerGameCalendar::date_fract);
	if (!TimerGameEconomy::UsingWallclockUnits()) {
		TimerGameEconomy::Date moved_economy{moved.base()};
		for (Vehicle *v : Vehicle::Iterate()) v->ShiftDates(moved_economy - TimerGameEconomy::date);
		LinkGraphSchedule::instance.ShiftDates(moved_economy - TimerGameEconomy::date);
		TimerGameEconomy::SetDate(moved_economy, TimerGameEconomy::date_fract);
	}
	CalendarEnginesMonthlyLoop();
}

static std::string CountHousesBySource(TownID town);

/**
 * Snow in the temperate climate (SnowLandscape()). Once the tile loops have
 * been round the map, every clear and tree tile from a step below the snow
 * line up is snowy and none lower down is, rails and roads above the line
 * are on snow, and a town founded above the line has the arctic houses
 * unless told otherwise -- or none at all with the switch for that off. With
 * no snow in the game, not one snowy tile: the
 * temperate climate as it always was. Usage: testsnih
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSnow(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Check the snow of the temperate climate against the snow line. Usage: 'testsnih'.");
		return true;
	}
	const int line = GetSnowLine();
	uint high = 0, high_snow = 0, low = 0, low_snow = 0, rail_high = 0, rail_high_snow = 0, road_high = 0, road_high_snow = 0;
	for (TileIndex tile : Map::Iterate()) {
		int z = GetTileZ(tile);
		switch (GetTileType(tile)) {
			case TileType::Clear:
			case TileType::Trees: {
				bool snow = IsTileType(tile, TileType::Clear) ? IsSnowTile(tile) : (GetTreeGround(tile) == TreeGround::SnowOrDesert || GetTreeGround(tile) == TreeGround::RoughSnow);
				if (z >= line - 1) {
					high++;
					if (snow) high_snow++;
				} else {
					low++;
					if (snow) low_snow++;
				}
				break;
			}
			case TileType::Railway:
				if (z > line + 1) {
					rail_high++;
					if (GetRailGroundType(tile) == RailGroundType::SnowOrDesert) rail_high_snow++;
				}
				break;
			case TileType::Road:
				if (GetTileMaxZ(tile) > line + 1 && !IsRoadDepot(tile)) {
					road_high++;
					if (IsOnSnowOrDesert(tile)) road_high_snow++;
				}
				break;
			default: break;
		}
	}
	IConsolePrint(CC_DEFAULT, "testsnih: snih {}, cara {}: nad ni {} policek zeme a stromu, z toho zasnezenych {}; pod ni {}, zasnezenych {}; koleje nad {}/{}, silnice nad {}/{}",
			HasSnow() ? "je" : "neni", line, high, high_snow, low, low_snow, rail_high_snow, rail_high, road_high_snow, road_high);
	if (HasSnow()) {
		if (high_snow != high) IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - {} policek nad carou bez snehu.", high - high_snow);
		if (low_snow != 0) IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - {} policek pod carou se snehem.", low_snow);
		if (rail_high_snow != rail_high) IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - {} kolejovych policek nad carou bez snehu.", rail_high - rail_high_snow);
		if (road_high_snow != road_high) IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - {} silnicnich policek nad carou bez snehu.", road_high - road_high_snow);
	} else if (high_snow + low_snow != 0) {
		IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - {} zasnezenych policek, a snih ve hre neni.", high_snow + low_snow);
	}

	uint32_t arctic = HOUSE_SOURCE_CLIMATE + to_underlying(LandscapeType::Arctic);
	for (const Town *t : Town::Iterate()) {
		bool above = GetTileMaxZ(t->xy) > HighestSnowLine();
		std::string sets;
		for (uint i = 0; i < t->num_house_sets; i++) {
			if (!sets.empty()) sets += " + ";
			sets += HouseSourceName(t->house_sets[i]);
		}
		IConsolePrint(CC_DEFAULT, "testsnih: mesto {} '{}' {} carou, domy z [{}], stoji {}", t->index.base(), t->GetCachedName(), above ? "nad" : "pod", sets.empty() ? "vsech" : sets, CountHousesBySource(t->index));
		bool temperate_snow = HasSnow() && _settings_game.game_creation.landscape == LandscapeType::Temperate;
		bool is_arctic = t->num_house_sets == 1 && t->house_sets[0] == arctic;
		if (temperate_snow && above && _settings_game.economy.arctic_towns_on_snow && !is_arctic) {
			IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - mesto {} nad carou nema arkticke domy.", t->index.base());
		}
		if (temperate_snow && above && !_settings_game.economy.arctic_towns_on_snow && t->num_house_sets != 0) {
			IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - mesto {} nad carou ma sadu, a arkticka mesta nad carou jsou vypnuta.", t->index.base());
		}
		if (!HasSnow() && t->num_house_sets != 0) IConsolePrint(CC_ERROR, "testsnih: ODMITNUTO - mesto {} ma zvolene sady, a nikdo mu je nedal.", t->index.base());
	}
	return true;
}

/**
 * The departure buttons of the orders window, "reverse out" and "leave by
 * itself", order by order: a station order has them to use, and any other
 * order of a train -- a depot's above all -- has them greyed and up. Written
 * when the player found them lit and down on a "go to depot" order, left over
 * from the station order before it. The train's first station order is set to
 * reverse out first, so that there is something to be left over, and a depot
 * order is added at the end when the train has none.
 * Usage: testsmerdepo [unit number]  (the first train with orders when none)
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDepartureButtons(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Check the orders window's departure buttons on every order of a train. Usage: 'testsmerdepo [cislo vlaku]'.");
		return true;
	}
	Train *t = nullptr;
	std::optional<int> punit;
	if (argv.size() >= 2) {
		punit = ParseInteger(argv[1]);
		if (!punit.has_value()) return false;
	}
	/* Named, that train; else the first with a plain station order -- one
	 * that neither waits to be coupled nor decouples, so that reversing out
	 * can be set on it and there is something to be left over. */
	for (Train *c : Train::Iterate()) {
		if (c->First() != c || c->GetNumOrders() == 0) continue;
		if (punit.has_value()) {
			if (c->unitnumber != (UnitID)*punit) continue;
			t = c;
			break;
		}
		for (const Order &o : c->Orders()) {
			if (o.IsType(OT_GOTO_STATION) && !o.ShouldWaitForCouple() && !o.ShouldDecoupleOnDeparture()) {
				t = c;
				break;
			}
		}
		if (t != nullptr) break;
	}
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testsmerdepo: ODMITNUTO - zadny vlak s obycejnym stanicnim rozkazem.");
		return true;
	}
	IConsolePrint(CC_DEFAULT, "testsmerdepo: vlak {}, {} rozkazu", t->unitnumber, t->GetNumOrders());
	AutoRestoreBackup cur_company(_current_company, t->owner);

	bool has_depot = false;
	for (const Order &o : t->Orders()) {
		if (o.IsType(OT_GOTO_DEPOT)) has_depot = true;
	}
	if (!has_depot) {
		const Depot *home = nullptr;
		for (const Depot *d : Depot::Iterate()) {
			if (IsRailDepotTile(d->xy) && GetTileOwner(d->xy) == t->owner) {
				home = d;
				break;
			}
		}
		if (home == nullptr) {
			IConsolePrint(CC_ERROR, "testsmerdepo: ODMITNUTO - zadne depo, kam poslat.");
			return true;
		}
		Order o{};
		o.MakeGoToDepot(DestinationID(home->index), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
		CommandCost r = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, t->index, t->GetNumOrders(), o);
		if (r.Failed()) {
			IConsolePrint(CC_ERROR, "testsmerdepo: ODMITNUTO - rozkaz do depa se nevlozil: {}", GetString(r.GetErrorMessage()));
			return true;
		}
	}
	for (VehicleOrderID i = 0; i < t->GetNumOrders(); i++) {
		const Order *o = t->GetOrder(i);
		if (o->IsType(OT_GOTO_STATION) && !o->ShouldWaitForCouple() && !o->ShouldDecoupleOnDeparture()) {
			Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, i, MOF_REVERSE_OUT, 1);
			break;
		}
	}

	for (VehicleOrderID i = 0; i < t->GetNumOrders(); i++) {
		const Order *o = t->GetOrder(i);
		std::optional<OrderDirectionButtons> b = TestOrderDirectionButtons(t, i);
		if (!b.has_value()) {
			IConsolePrint(CC_ERROR, "testsmerdepo: ODMITNUTO - okno rozkazu se neotevrelo.");
			return true;
		}
		bool station = o->IsType(OT_GOTO_STATION);
		IConsolePrint(CC_DEFAULT, "testsmerdepo: rozkaz {} ({}): reversni chod {}{}, automaticky {}{}", i,
				station ? "stanice" : o->IsType(OT_GOTO_DEPOT) ? "depo" : "jiny",
				b->reverse_disabled ? "zasedly" : "aktivni", b->reverse_lowered ? " a zamackly" : "",
				b->auto_disabled ? "zasedly" : "aktivni", b->auto_lowered ? " a zamackly" : "");
		if (!station && (!b->reverse_disabled || b->reverse_lowered || !b->auto_disabled || b->auto_lowered)) {
			IConsolePrint(CC_ERROR, "testsmerdepo: ODMITNUTO - rozkaz {} neni do stanice, a cudliky smeru nejsou zasedle a zvednute.", i);
		}
		if (station && !o->ShouldWaitForCouple() && !o->ShouldDecoupleOnDeparture() && (b->reverse_disabled || b->reverse_lowered != o->ShouldReverseOutOfStation())) {
			IConsolePrint(CC_ERROR, "testsmerdepo: ODMITNUTO - rozkaz {} do stanice: cudlik reversniho chodu neodpovida rozkazu.", i);
		}
	}
	return true;
}

/**
 * The original industries of the climates switched on (climate_industries.h):
 * for every original industry type its climates, whether the game has it on,
 * whether a set's industry took its place, how many stand on the map, and
 * the cargoes it produces and takes. Refuses an industry of a climate on that
 * the game has off or that a set replaced, and says which of the cargoes
 * were placed for these industries. Given a number, at least that many
 * industries of the climates on have to stand on the map. And the game's
 * own industries (economy.extra_industries): the marijuana plantation and
 * the coffeeshop that takes its cargo; given a second and a third number, at
 * least that many plantations and coffeeshops have to stand on the map.
 * Usage: testprumysl [min [plantaze [hulirny]]]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestClimateIndustries(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the original industries of the climates switched on, and the game's own. Usage: 'testprumysl [min [plantaze [hulirny]]]', refusing fewer than min of them, or fewer than plantaze plantations or hulirny coffeeshops, on the map.");
		return true;
	}
	LandscapeTypes on = IndustryClimatesOn();
	static const std::array<std::string_view, 4> climate_names{"mirne", "arktida", "poust", "toyland"};
	std::string on_names;
	for (LandscapeType c : on) on_names += fmt::format("{} ", climate_names[to_underlying(c)]);
	IConsolePrint(CC_DEFAULT, "testprumysl: klima {}, zapnuto: {}", climate_names[to_underlying(_settings_game.game_creation.landscape)], on_names.empty() ? std::string{"nic"} : on_names);

	auto label_of = [](CargoType cargo) {
		if (!IsValidCargoType(cargo)) return std::string{};
		CargoLabel l = CargoSpec::Get(cargo)->label;
		uint32_t v = l.base();
		return fmt::format("{}{}{}{}", static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v));
	};

	uint on_map_of_on = 0;
	for (IndustryType type = 0; type < NEW_INDUSTRYOFFSET; type++) {
		const IndustrySpec *spec = GetIndustrySpec(type);
		bool kept = IsOriginalIndustryKept(type);
		bool played = spec->climate_availability.Test(_settings_game.game_creation.landscape);
		if (!kept && !played) continue;
		std::string climates;
		for (LandscapeType c : spec->climate_availability) climates += fmt::format("{}{}", climates.empty() ? "" : "+", climate_names[to_underlying(c)]);
		std::string produced, accepted;
		for (CargoType c : spec->produced_cargo) if (IsValidCargoType(c)) produced += label_of(c) + " ";
		for (CargoType c : spec->accepts_cargo) if (IsValidCargoType(c)) accepted += label_of(c) + " ";
		uint count = Industry::GetIndustryTypeCount(type);
		if (kept) on_map_of_on += count;
		IConsolePrint(CC_DEFAULT, "testprumysl:   {} {} [{}] {}{}{} na mape {}, vyrabi {}, bere {}", type, GetString(spec->name), climates,
				spec->enabled ? "zap" : "VYP", kept ? " drzeno" : "", spec->grf_prop.HasGrfFile() ? " SADA" : "", count, produced, accepted);
		if (kept && !spec->enabled) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - prumysl {} zapnuteho klimatu je vypnuty.", type);
		if (kept && spec->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - prumysl {} zapnuteho klimatu nahradila sada.", type);
	}
	std::string extra;
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		if (IsExtraClimateCargo(cs->Index())) extra += label_of(cs->Index()) + " ";
	}
	IConsolePrint(CC_DEFAULT, "testprumysl: dosazene naklady: {}", extra.empty() ? std::string{"zadne"} : extra);
	IConsolePrint(CC_DEFAULT, "testprumysl: prumyslu zapnutych klimat na mape: {}", on_map_of_on);

	/* The sprites the climate played puts in place of another climate's
	 * (CLIMATE_INDUSTRY_SPRITE_RANGES): an industry of another climate draws
	 * its own climate's, from the base file for the temperate climate and
	 * from the base file or its climate's own file for the others -- never
	 * the file of the climate played. */
	const GraphicsSet *set = BaseGraphics::GetUsedSet();
	auto climate_file = [set](LandscapeType c) -> const std::string & {
		return set->files[c == LandscapeType::Temperate ? to_underlying(GraphicsFileType::Base) : to_underlying(GraphicsFileType::Arctic) + to_underlying(c) - 1].filename;
	};
	uint own_drawn = 0;
	for (const ClimateIndustrySpriteUse &use : ClimateIndustrySpriteUses()) {
		if (use.climate == _settings_game.game_creation.landscape) continue;
		own_drawn++;
		const SpriteFile *origin = GetOriginFile(use.drawn);
		std::string file = origin == nullptr ? std::string{"zadny"} : origin->GetFilename();
		IConsolePrint(CC_DEFAULT, "testprumysl: grafika {} ({}): sprite {} kreslen jako {} ze {}", use.type, climate_names[to_underlying(use.climate)], use.sprite, use.drawn, file);
		bool right = use.drawn != use.sprite && (file == climate_file(LandscapeType::Temperate) || (use.climate != LandscapeType::Temperate && file == climate_file(use.climate)));
		if (!right) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - prumysl {} kresli sprite {} jinak nez jeho klima.", use.type, use.sprite);
	}
	IConsolePrint(CC_DEFAULT, "testprumysl: spritu kreslenych po klimatu prumyslu: {}", own_drawn);
	if (argv.size() >= 2) {
		auto min = ParseType<uint>(argv[1]);
		if (min.has_value() && on_map_of_on < *min) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - na mape je {} prumyslu zapnutych klimat, ceka se aspon {}.", on_map_of_on, *min);
	}

	/* The game's own industries (economy.extra_industries): when on, the
	 * marijuana plantation in every climate growing marijuana on a tile of its
	 * own, and the coffeeshop taking all of it and of the other cargoes of its
	 * list the game has; nowhere when off. No house takes marijuana either
	 * way: the houses are as they always were. */
	const IndustrySpec *mari = GetIndustrySpec(IT_MARIJUANA_PLANTATION);
	const IndustrySpec *shop = GetIndustrySpec(IT_COFFEESHOP);
	CargoType mari_cargo = GetCargoTypeByLabel(CT_MARIJUANA);
	const LandscapeTypes every_climate{LandscapeType::Temperate, LandscapeType::Arctic, LandscapeType::Tropic, LandscapeType::Toyland};
	if (IsValidCargoType(mari_cargo)) {
		for (const HouseSpec &hs : HouseSpec::Specs()) {
			if (std::ranges::find(hs.accepts_cargo, mari_cargo) == std::end(hs.accepts_cargo)) continue;
			IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - dum {} bere marihuanu, domy maji byt jak byly.", hs.Index());
		}
	}
	if (_settings_game.economy.extra_industries) {
		std::string produced;
		for (CargoType c : mari->produced_cargo) if (IsValidCargoType(c)) produced += label_of(c) + " ";
		IndustryGfx gfx = mari->layouts.front().front().gfx;
		const IndustryTileSpec *tile = GetIndustryTileSpec(gfx);
		uint count = Industry::GetIndustryTypeCount(IT_MARIJUANA_PLANTATION);
		IConsolePrint(CC_DEFAULT, "testprumysl: pridavny {} {} {}{}, dlazdice {} kreslena jako {}, na mape {}, vyrabi {}",
				IT_MARIJUANA_PLANTATION, GetString(mari->name), mari->enabled ? "zap" : "VYP", mari->grf_prop.HasGrfFile() ? " SADA" : "",
				gfx, tile->grf_prop.subst_id, count, produced);
		if (!mari->enabled || mari->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - marihuanova plantaz neni ve hre, nebo ji zabrala sada.");
		if (mari->climate_availability != every_climate) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - marihuanova plantaz neni ve vsech klimatech.");
		if (produced != "MARI ") IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - marihuanova plantaz nevyrabi marihuanu.");
		if (gfx != GFX_MARIJUANA_PLANTATION || tile->grf_prop.HasGrfFile() || tile->grf_prop.subst_id >= NEW_INDUSTRYTILEOFFSET) {
			IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - marihuanova plantaz nema svou dlazdici kreslenou jako puvodni dlazdice.");
		}
		if (argv.size() >= 3) {
			auto min = ParseType<uint>(argv[2]);
			if (min.has_value() && count < *min) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - na mape je {} marihuanovych plantazi, ceka se aspon {}.", count, *min);
		}

		/* The coffeeshop: in towns only, on a tile of its own, taking all of
		 * each cargo of its list the game has, at the industry and its tile. */
		IndustryGfx shop_gfx = shop->layouts.empty() ? INVALID_INDUSTRYTILE : shop->layouts.front().front().gfx;
		const IndustryTileSpec *shop_tile = GetIndustryTileSpec(GFX_COFFEESHOP);
		uint shops = Industry::GetIndustryTypeCount(IT_COFFEESHOP);
		std::string takes;
		for (CargoLabel label : CoffeeshopCargoes()) {
			CargoType c = GetCargoTypeByLabel(label);
			if (!IsValidCargoType(c)) continue;
			auto at = std::ranges::find(shop_tile->accepts_cargo, c);
			int level = at == std::end(shop_tile->accepts_cargo) ? 0 : shop_tile->acceptance[at - std::begin(shop_tile->accepts_cargo)];
			bool industry_takes = std::ranges::find(shop->accepts_cargo, c) != std::end(shop->accepts_cargo);
			takes += fmt::format("{} {}/8{} ", label_of(c), level, industry_takes ? "" : " (prumysl NE)");
			if (level != 8 || !industry_takes) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna nebere {} 8/8, i kdyz je ve hre.", label_of(c));
		}
		IConsolePrint(CC_DEFAULT, "testprumysl: pridavny {} {} {}{}, dlazdice {}, jen ve meste {}, na mape {}, bere {}",
				IT_COFFEESHOP, GetString(shop->name), shop->enabled ? "zap" : "VYP", shop->grf_prop.HasGrfFile() ? " SADA" : "",
				shop_gfx, shop->behaviour.Test(IndustryBehaviour::OnlyInTown) ? "ano" : "ne", shops, takes);
		if (!shop->enabled || shop->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna neni ve hre, nebo ji zabrala sada.");
		if (shop->climate_availability != every_climate) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna neni ve vsech klimatech.");
		if (!shop->behaviour.Test(IndustryBehaviour::OnlyInTown)) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna se muze stavet mimo mesto.");
		if (shop_gfx != GFX_COFFEESHOP || shop_tile->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna nema svou dlazdici.");
		for (CargoType c : shop->produced_cargo) {
			if (IsValidCargoType(c)) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna neco vyrabi ({}).", label_of(c));
		}
		if (argv.size() >= 4) {
			auto min = ParseType<uint>(argv[3]);
			if (min.has_value() && shops < *min) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - na mape je {} hulirn, ceka se aspon {}.", shops, *min);
		}
	}

	/* The game's own vehicles: every one under its own mark in the engine
	 * mapping, out of every set's reach and taken over by none; the ones for
	 * marijuana in the game exactly while the industries are, carrying it,
	 * with every loaded picture drawing some of itself green and not all. */
	for (VehicleType type : EnumRange(VehicleType::CompanyEnd)) {
		for (uint16_t internal_id = 0; internal_id < GetOriginalEngineCount(type); internal_id++) {
			if (!IsGamesOwnEngine(type, internal_id)) continue;
			EngineID id = _engine_mngr.GetID(type, internal_id, EngineOverrideManager::GAMES_OWN_GRFID);
			if (id == EngineID::Invalid()) {
				IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - vozidlo hry {}/{} neni pod znackou hry, sada ho muze prevzit.", to_underlying(type), internal_id);
				continue;
			}
			const Engine *e = Engine::Get(id);
			bool for_marijuana = IsMarijuanaEngineInfo(e->info);
			IConsolePrint(CC_DEFAULT, "testprumysl: vozidlo hry {} '{}' {}, veze {}{}", e->index, GetString(STR_ENGINE_NAME, e->index), e->IsEnabled() ? "ve hre" : "mimo hru",
					label_of(e->GetDefaultCargoType()), e->grf_prop.HasGrfFile() ? ", PREVZALA SADA" : "");
			if (e->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - sada prevzala vozidlo hry {}.", e->index);
			if (for_marijuana && e->IsEnabled() != _settings_game.economy.extra_industries) {
				IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - vozidlo na marihuanu {} je {}, pridavny prumysl je {}.", e->index, e->IsEnabled() ? "ve hre" : "mimo hru", _settings_game.economy.extra_industries ? "zapnuty" : "vypnuty");
			}
			if (for_marijuana && e->IsEnabled() && e->GetDefaultCargoType() != mari_cargo) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - vozidlo na marihuanu {} nevozi marihuanu.", e->index);
		}
	}
	/* A set's vehicle numbered where one of the game's own sits stands beside it. */
	for (const Engine *e : Engine::Iterate()) {
		if (!e->grf_prop.HasGrfFile() || !IsGamesOwnEngine(e->type, e->grf_prop.local_id)) continue;
		IConsolePrint(CC_DEFAULT, "testprumysl: vedle stoji vozidlo sady {} '{}' s cislem {}", e->index, GetString(STR_ENGINE_NAME, e->index), e->grf_prop.local_id);
	}
	if (_settings_game.economy.extra_industries) {
		for (const auto &[full, green] : GreenLoadSprites()) {
			auto [drawn_green, drawn] = GreenLoadPixels(green);
			IConsolePrint(CC_DEFAULT, "testprumysl: zeleny naklad {} z {}: zelenych {} z {}", green, full, drawn_green, drawn);
			if (drawn_green == 0 || drawn_green >= drawn) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - plny obrazek {} nema zeleny naklad, nebo je zeleny cely.", green);
		}
		/* Ships and aircraft that carry goods carry marijuana too. */
		uint goods_carriers = 0;
		for (const Engine *e : Engine::Iterate()) {
			if (e->type != VehicleType::Ship && e->type != VehicleType::Aircraft) continue;
			bool goods = false;
			for (CargoType c : e->info.refit_mask) goods |= CargoSpec::Get(c)->town_acceptance_effect == TownAcceptanceEffect::Goods;
			if (IsValidCargoType(e->GetDefaultCargoType())) goods |= CargoSpec::Get(e->GetDefaultCargoType())->town_acceptance_effect == TownAcceptanceEffect::Goods;
			if (!goods) continue;
			goods_carriers++;
			if (!e->info.refit_mask.Test(mari_cargo)) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - {} vozi zbozi, ale marihuanu ne.", GetString(STR_ENGINE_NAME, e->index));
		}
		IConsolePrint(CC_DEFAULT, "testprumysl: lodi a letadel se zbozim (a marihuanou): {}", goods_carriers);
		/* And nothing else is refitted to it: a set's coal lorry or coal
		 * wagon takes bulk cargo by class and marijuana with it, and stood
		 * above the game's own marijuana lorry in the list. */
		uint others = 0;
		for (const Engine *e : Engine::Iterate()) {
			if (e->type == VehicleType::Ship || e->type == VehicleType::Aircraft) continue;
			if (e->GetDefaultCargoType() == mari_cargo || !e->info.refit_mask.Test(mari_cargo)) continue;
			/* The St carries it as its coal drawn green, and so its parts. */
			if (IsGreenLayerWagon(e)) {
				IConsolePrint(CC_DEFAULT, "testprumysl: St {} '{}' vozi marihuanu jako zelene uhli", e->index, GetString(e->info.string_id));
				continue;
			}
			bool st_part = false;
			for (const Engine *st : Engine::IterateType(VehicleType::Train)) {
				if (!IsGreenLayerWagon(st)) continue;
				std::vector<EngineID> parts = GetArticulatedPartEngines(st->index);
				if (std::ranges::find(parts, e->index) != parts.end()) st_part = true;
			}
			if (st_part) continue;
			others++;
			IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - {} ({}) jde prestavet na marihuanu, a neni na ni.", GetString(STR_ENGINE_NAME, e->index), e->index);
		}
		IConsolePrint(CC_DEFAULT, "testprumysl: jinych vozidel s prestavbou na marihuanu: {}", others);
	}

	if (!_settings_game.economy.extra_industries) {
		IConsolePrint(CC_DEFAULT, "testprumysl: pridavny prumysl vypnuty, typy {} {} a {} {}", IT_MARIJUANA_PLANTATION, mari->enabled ? (mari->grf_prop.HasGrfFile() ? "ma sada" : "ZAPNUTY") : "volny",
				IT_COFFEESHOP, shop->enabled ? (shop->grf_prop.HasGrfFile() ? "ma sada" : "ZAPNUTY") : "volny");
		if (mari->enabled && !mari->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - marihuanova plantaz je ve hre, i kdyz je pridavny prumysl vypnuty.");
		if (shop->enabled && !shop->grf_prop.HasGrfFile()) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - hulirna je ve hre, i kdyz je pridavny prumysl vypnuty.");
		if (IsValidCargoType(mari_cargo)) IConsolePrint(CC_ERROR, "testprumysl: ODMITNUTO - marihuana je ve hre, i kdyz je pridavny prumysl vypnuty.");
	}
	return true;
}

/**
 * The set of cargo types (CargoTypes) with its 128 slots put through its
 * paces: cargoes above 64 set, tested, counted and walked; the two words;
 * the set as a string parameter and back ({CARGO_LIST} with the game's own
 * cargoes), through an encoded string too, as the news carry it; and the
 * cargo monitor number of a cargo above 64 read back, with the number of a
 * cargo below 64 unchanged from what it always was. Written with the move
 * from 64 to 128 cargoes, which the rest of the battery cannot see: every
 * scene plays with the dozen a climate has. Given a count, the game has to
 * have that many cargoes: the scene grfvzdalo plays a set that switches the
 * cargoes off and then gives up, and asks for the climate's dozen back.
 * Names after the count have to be in the list of cargoes, a name with a !
 * in front must not be: the scene nakladysdilene plays two sets that bring
 * the same cargoes, and the names say whose cargo each shared one is.
 * Usage: testnaklady [pocet [jmeno|!jmeno ...]]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCargoTypes(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Put the 128-cargo set through its paces. Usage: 'testnaklady [pocet [jmeno|!jmeno ...]]', refusing when the game has another number of cargoes, or a name is missing from the list (or, with !, in it).");
		return true;
	}
	bool ok = true;
	auto fail = [&ok](const std::string &what) {
		IConsolePrint(CC_ERROR, "testnaklady: ODMITNUTO - {}", what);
		ok = false;
	};
	auto cargo = [](uint n) { return static_cast<CargoType>(n); };

	CargoTypes set;
	set.Set(cargo(3)).Set(cargo(63)).Set(cargo(64)).Set(cargo(127));
	if (set.Count() != 4) fail(fmt::format("Count po ctyrech bitech dal {}", set.Count()));
	if (!set.Test(cargo(64)) || set.Test(cargo(65)) || !set.Test(cargo(127))) fail("Test bitu 64/65/127");
	std::string walked;
	for (CargoType c : set) walked += fmt::format("{} ", to_underlying(c));
	if (walked != "3 63 64 127 ") fail(fmt::format("iterace dala '{}'", walked));
	if (set.Low() != ((1ULL << 3) | (1ULL << 63)) || set.High() != (1ULL | (1ULL << 63))) fail("slova Low/High");
	CargoTypes flipped = set;
	flipped.Flip();
	if (flipped.Count() != 124 || flipped.Any(set) || !(flipped | set).All()) fail("Flip a sjednoceni");
	if (!ALL_CARGOTYPES.All() || ALL_CARGOTYPES.Count() != to_underlying(NUM_CARGO)) fail("ALL_CARGOTYPES");
	if (set.GetNthSetBit(2) != cargo(64) || set.GetNthSetBit(4).has_value()) fail("GetNthSetBit");
	CargoTypes invalid;
	invalid.Set(INVALID_CARGO).Set(CargoFilterCriteria::CF_ANY);
	if (invalid.Any() || invalid.Test(INVALID_CARGO)) fail("INVALID_CARGO se dostal do mnoziny");
	CargoTypes reset = set;
	reset.Reset(cargo(64)).Reset(CargoTypes{cargo(3)});
	if (reset.Count() != 2 || reset.Test(cargo(64)) || reset.Test(cargo(3))) fail("Reset");
	if (CargoTypes{(1ULL << 5) | (1ULL << 40)}.Count() != 2 || CargoTypes::FromWords(0, 1).GetNthSetBit(0) != cargo(64)) fail("konstrukce ze slov");

	/* Through a string parameter: the cargoes the game has, named as the
	 * station window names them, and the same through an encoded string. */
	CargoTypes present;
	for (const CargoSpec *cs : CargoSpec::Iterate()) present.Set(cs->Index());
	std::string listed = GetString(STR_JUST_CARGO_LIST, present);
	uint named = 0;
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		if (listed.find(GetString(cs->name)) != std::string::npos) named++;
	}
	IConsolePrint(CC_DEFAULT, "testnaklady: {} nakladu ve hre: {}", present.Count(), listed);
	if (named != present.Count()) fail(fmt::format("v seznamu je {} jmen z {}", named, present.Count()));
	if (argv.size() >= 2) {
		auto expected = ParseType<uint>(argv[1]);
		if (!expected.has_value()) {
			IConsolePrint(CC_ERROR, "testnaklady: '{}' is not a count.", argv[1]);
			return true;
		}
		if (present.Count() != *expected) fail(fmt::format("ve hre je {} nakladu, ceka se {}", present.Count(), *expected));
	}
	for (size_t i = 2; i < argv.size(); i++) {
		std::string_view name = argv[i];
		bool must_not = name.starts_with('!');
		if (must_not) name.remove_prefix(1);
		bool found = false;
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			if (GetString(cs->name) == name) found = true;
		}
		if (found == must_not) fail(must_not ? fmt::format("naklad {} ve hre je, nema byt", name) : fmt::format("naklad {} ve hre neni", name));
	}
	auto params = MakeParameters(present);
	std::string encoded = GetEncodedStringWithArgs(STR_JUST_CARGO_LIST, params).GetDecodedString();
	if (encoded != listed) fail(fmt::format("encoded string dal '{}'", encoded));
	CargoTypes high_only = CargoTypes{cargo(100)};
	if (GetString(STR_JUST_CARGO_LIST, high_only) != GetString(STR_JUST_NOTHING)) fail("seznam pro nedefinovany naklad 100 neni 'nic'");

	/* The cargo monitor: a cargo above 64 goes in and comes out, and the
	 * number of a cargo below 64 is the number it always was. */
	CargoMonitorID high = EncodeCargoTownMonitor(static_cast<CompanyID>(3), cargo(100), static_cast<TownID>(5));
	if (DecodeMonitorCargoType(high) != cargo(100) || DecodeMonitorCompany(high) != static_cast<CompanyID>(3) || DecodeMonitorTown(high) != static_cast<TownID>(5)) fail("cargo monitor s nakladem 100");
	CargoMonitorID low = EncodeCargoIndustryMonitor(static_cast<CompanyID>(2), cargo(33), static_cast<IndustryID>(7));
	uint32_t as_before = 7 | (1u << 16) | (33u << 19) | (2u << 25);
	if (low != as_before || DecodeMonitorCargoType(low) != cargo(33) || DecodeMonitorIndustry(low) != static_cast<IndustryID>(7)) fail("cislo monitoru pro naklad 33 se zmenilo");

	/* Whose industries stand on the map: with sets side by side, every set's. */
	std::map<uint32_t, uint> by_set;
	for (const Industry *ind : Industry::Iterate()) by_set[GetIndustrySpec(ind->type)->grf_prop.grfid]++;
	std::string industries;
	for (const auto &[grfid, count] : by_set) {
		const GRFConfig *c = grfid == 0 ? nullptr : GetGRFConfig(grfid);
		industries += fmt::format("{}{} {}", industries.empty() ? "" : ", ", c == nullptr ? std::string{"hra"} : c->GetName(), count);
	}
	IConsolePrint(CC_DEFAULT, "testnaklady: prumysly na mape: {}", industries.empty() ? std::string{"zadne"} : industries);

	/* The icon of the road-vehicle cargo, drawn before its name in every
	 * cargo list: the sprite and its size, 0x0 when the base set lacks it. */
	if (IsValidCargoType(_road_vehicle_cargo)) {
		SpriteID icon = CargoSpec::Get(_road_vehicle_cargo)->GetCargoIcon();
		Dimension size = GetSpriteSize(icon);
		IConsolePrint(CC_DEFAULT, "testnaklady: ikona nakladu ROLA: sprite {} ({}x{})", icon, size.width, size.height);
		if (size.width == 0 || size.height == 0) fail("ikona nakladu ROLA je prazdna");
	}
	/* And of marijuana, when the game's plantation is in (economy.extra_industries). */
	if (CargoType mari = GetCargoTypeByLabel(CT_MARIJUANA); IsValidCargoType(mari)) {
		SpriteID icon = CargoSpec::Get(mari)->GetCargoIcon();
		Dimension size = GetSpriteSize(icon);
		IConsolePrint(CC_DEFAULT, "testnaklady: ikona nakladu MARI: sprite {} ({}x{})", icon, size.width, size.height);
		if (size.width == 0 || size.height == 0) fail("ikona nakladu MARI je prazdna");
	}

	/* Only the car carriers take road vehicles: the game's own and the flat
	 * wagons the player named (IsCarCarrierWagon()), with their articulated
	 * parts. Any other wagon offering the fitting is refused by name, and so
	 * is a car carrier that does not. */
	if (IsValidCargoType(_road_vehicle_cargo)) {
		std::vector<EngineID> parts;
		for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
			if (!IsCarCarrierWagon(e)) continue;
			std::vector<EngineID> p = GetArticulatedPartEngines(e->index);
			parts.insert(parts.end(), p.begin(), p.end());
		}
		uint carriers = 0;
		for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
			if (e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon) continue;
			bool offered = e->info.refit_mask.Test(_road_vehicle_cargo);
			bool carrier = IsCarCarrierWagon(e);
			bool part = std::ranges::find(parts, e->index) != parts.end();
			if (carrier) {
				carriers++;
				IConsolePrint(CC_DEFAULT, "testnaklady: vagon na auta {} '{}' (v nabidce '{}'){}", e->index, GetString(e->info.string_id),
						GetString(STR_ENGINE_NAME, PackEngineNameDParam(e->index, EngineNameContext::PurchaseList)), e->info.climates.Any() ? "" : " (v tomto klimatu neni)");
			}
			if (offered && !carrier && !part) fail(fmt::format("vagon {} '{}' jde prestavet na auta, a neni vagon na auta", e->index, GetString(e->info.string_id)));
			if (carrier && !offered) fail(fmt::format("vagon na auta {} '{}' nejde prestavet na auta", e->index, GetString(e->info.string_id)));
		}
		IConsolePrint(CC_DEFAULT, "testnaklady: vagonu na auta {}", carriers);
		/* And the wagons whose names only look like it, so that a kind that
		 * was missed can be told from one that was left out on purpose. */
		for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
			if (e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon || IsCarCarrierWagon(e)) continue;
			if (e->info.string_id == INVALID_STRING_ID) continue;
			std::string name = GetString(e->info.string_id);
			std::string shown = GetString(STR_ENGINE_NAME, PackEngineNameDParam(e->index, EngineNameContext::PurchaseList));
			bool looks = [](const std::string &n) { return n.find("Pa") != std::string::npos || n.find("Sg") != std::string::npos || n.find("Smm") != std::string::npos; }(name) ||
					shown.find("Sg") != std::string::npos || shown.find("Smm") != std::string::npos;
			if (!looks) continue;
			IConsolePrint(CC_DEFAULT, "testnaklady: neni vagon na auta {} '{}' (v nabidce '{}')", e->index, name, shown);
		}
	}

	IConsolePrint(CC_DEFAULT, "testnaklady: NUM_CARGO {}, {}.", to_underlying(NUM_CARGO), ok ? "vse v poradku" : "s chybami");
	return true;
}

/**
 * A set of houses as the rig names it: "klima" for the houses of the climate
 * played, "mirne", "arktida", "poust" or "toyland" for a climate's houses,
 * "vse" for none chosen, or a GRF id the way the game prints it (4F474D05).
 * @param arg the name
 * @return the set, 0 for "vse", or nullopt when it names nothing
 */
static std::optional<uint32_t> ParseHouseSource(std::string_view arg)
{
	if (arg == "vse") return 0;
	if (arg == "klima") return HOUSE_SOURCE_CLIMATE + to_underlying(_settings_game.game_creation.landscape);
	static const std::string_view CLIMATES[] = {"mirne", "arktida", "poust", "toyland"};
	for (uint c = 0; c < std::size(CLIMATES); c++) {
		if (arg == CLIMATES[c]) return HOUSE_SOURCE_CLIMATE + c;
	}
	auto id = ParseInteger<uint32_t>(arg, 16);
	if (!id.has_value()) return std::nullopt;
	return std::byteswap(*id);
}

/**
 * A town as the rig names it: its number, or "posledni" for the newest one --
 * the one a scene has just founded.
 * @param arg the name
 * @return the town, or nullopt when there is none such
 */
static std::optional<TownID> ParseRigTown(std::string_view arg)
{
	if (arg == "posledni") {
		std::optional<TownID> newest;
		for (const Town *t : Town::Iterate()) newest = t->index;
		return newest;
	}
	auto id = ParseInteger(arg);
	if (!id.has_value() || !Town::IsValidID(*id)) return std::nullopt;
	return static_cast<TownID>(*id);
}

/**
 * The house standing on a tile, by its north tile: the other tiles of a house
 * of two or four have specs of their own that carry no climate, and would
 * count as a house of nowhere.
 * @param tile a house tile
 * @return the spec of the whole house
 */
static const HouseSpec &HeadHouseSpec(TileIndex tile)
{
	HouseID house = GetHouseType(tile);
	GetHouseNorthPart(house);
	return *HouseSpec::Get(house);
}

/**
 * Is this an original arctic house of the snowy kind, the one built above the
 * snow line only? Outside the arctic there is no snow (yet), so none of these
 * belongs there.
 * @param hs the house
 * @return whether it is one
 */
static bool IsSnowyOriginalHouse(const HouseSpec &hs)
{
	return hs.grf_prop.grffile == nullptr && hs.building_availability.Test(HouseZone::ClimateSubarcticAboveSnow) && !hs.building_availability.Test(HouseZone::ClimateSubarcticBelowSnow);
}

/**
 * Tell which sets of houses a town's house tiles come from.
 * @param town the town
 * @return "set:tiles" for each set, in the order they are first met
 */
static std::string CountHousesBySource(TownID town)
{
	std::vector<std::pair<uint32_t, uint>> counts;
	for (TileIndex tile : Map::Iterate()) {
		if (!IsTileType(tile, TileType::House) || GetTownIndex(tile) != town) continue;
		uint32_t source = HouseSourceOf(HeadHouseSpec(tile));
		auto it = std::ranges::find(counts, source, &std::pair<uint32_t, uint>::first);
		if (it == counts.end()) {
			counts.emplace_back(source, 1);
		} else {
			it->second++;
		}
	}
	std::string out;
	for (const auto &[source, n] : counts) {
		if (!out.empty()) out += ", ";
		out += fmt::format("{}:{}", HouseSourceName(source), n);
	}
	return out.empty() ? "zadne" : out;
}

/**
 * The themed towns ("Domy z"), driven through the windows the player uses.
 *
 * - testdomy: the sets there are to choose from, and every town's choice and
 *   houses by set;
 * - testdomy mapa (or mars): the themed towns of a new map -- of each other
 *   climate as many as its setting says, Mars towns as theirs when the game
 *   has the Mars houses, each all of its set, and no Mars house in any town
 *   of every house; with the GRFs split, every ordinary town of one GRF and
 *   the GRFs even;
 * - testdomy picker <set> [year [jmena]]: the house picker filtered to one set
 *   shows its houses and no others; with "jmena", every house listed by id
 *   and name, and the house a set put in its place;
 * - testdomy postav <x> <y> <house>: place a house by hand, as the picker
 *   does (Commands::PlaceHouse), and say what stands there;
 * - testdomy pole <x> <y> [r]: the houses within r tiles of a tile, by id and
 *   name, and how many tiles of each;
 * - testdomy rok <year>: move the calendar there (MoveCalendarTo()), for a
 *   set whose houses are not built before some year;
 * - testdomy okno <town> <set>: open the town window, press "Domy z" and click
 *   the set in the list, as the player ticks or unticks it;
 * - testdomy rust <town> <n>: let the town grow n times, and check every house
 *   it put up is of a set it was told to build from;
 * - testdomy zaloz <x> <y> <set>: found a town of one set through the found
 *   town window, and check its houses are all of it.
 *
 * A town is its number, or "posledni" for the newest. A set is "klima", "vse"
 * or a GRF id (ParseHouseSource()).
 *
 * Anything that does not come out as the player was promised is written
 * ODMITNUTO, which the battery counts.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestHouseSets(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Themed towns. Usage: 'testdomy', 'testdomy mapa', 'testdomy rok <rok>', 'testdomy okno <mesto> <sada>', 'testdomy rust <mesto> <kolikrat>', 'testdomy zaloz <x> <y> <sada>'.");
		return true;
	}
	if (argv.size() == 1) {
		for (uint32_t source : AvailableHouseSources()) {
			static const std::string_view CLIMATES[] = {"mirne", "arktida", "poust", "toyland"};
			std::string id = source >= HOUSE_SOURCE_CLIMATE ? std::string{CLIMATES[std::min<uint>(source - HOUSE_SOURCE_CLIMATE, 3)]} : fmt::format("{:08X}", std::byteswap(source));
			IConsolePrint(CC_DEFAULT, "testdomy: sada {} {}", id, HouseSourceName(source));
		}
		for (const Town *t : Town::Iterate()) {
			std::string sets;
			for (uint i = 0; i < t->num_house_sets; i++) {
				if (!sets.empty()) sets += " + ";
				sets += HouseSourceName(t->house_sets[i]);
			}
			IConsolePrint(CC_DEFAULT, "testdomy: mesto {} domy z [{}], stoji {}", t->index.base(), sets.empty() ? "vsech" : sets, CountHousesBySource(t->index));
		}
		return true;
	}

	if (argv[1] == "mars" || argv[1] == "mapa") {
		/* The themed towns of a new map: of each other climate as many as its
		 * setting says, and as many Mars towns as theirs when the game has the
		 * Mars houses, none when not; each of them all of its set; and no Mars
		 * house in a town of every house. */
		std::vector<uint32_t> sources = AvailableHouseSources();
		bool have_mars = std::ranges::find(sources, MARS_HOUSES_GRFID) != sources.end();
		const LandscapeType played = _settings_game.game_creation.landscape;
		std::vector<std::pair<uint32_t, uint>> want;
		if (played != LandscapeType::Temperate) want.emplace_back(HOUSE_SOURCE_CLIMATE + to_underlying(LandscapeType::Temperate), _settings_game.economy.temperate_towns);
		if (played != LandscapeType::Tropic) want.emplace_back(HOUSE_SOURCE_CLIMATE + to_underlying(LandscapeType::Tropic), _settings_game.economy.tropic_towns);
		if (played != LandscapeType::Toyland) want.emplace_back(HOUSE_SOURCE_CLIMATE + to_underlying(LandscapeType::Toyland), _settings_game.economy.toyland_towns);
		want.emplace_back(MARS_HOUSES_GRFID, have_mars ? _settings_game.economy.mars_towns : 0);

		std::vector<uint> found(want.size(), 0);
		for (const Town *t : Town::Iterate()) {
			uint mars = 0, wrong = 0;
			for (TileIndex tile : Map::Iterate()) {
				if (!IsTileType(tile, TileType::House) || GetTownIndex(tile) != t->index) continue;
				const HouseSpec &hs = HeadHouseSpec(tile);
				if (HouseSourceOf(hs) == MARS_HOUSES_GRFID) mars++;
				if (t->num_house_sets > 0 && !TownBuildsHouse(t, hs, {})) wrong++;
			}
			if (t->num_house_sets == 1) {
				for (size_t i = 0; i < want.size(); i++) {
					if (want[i].first != t->house_sets[0]) continue;
					found[i]++;
					IConsolePrint(CC_DEFAULT, "testdomy: tematicke mesto {} '{}' z [{}]: stoji {}", t->index.base(), t->GetCachedName(), HouseSourceName(t->house_sets[0]), CountHousesBySource(t->index));
					if (wrong > 0) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - tematicke mesto {} ma {} policek domu odjinud.", t->index.base(), wrong);
				}
			} else if (t->num_house_sets == 0 && mars > 0) {
				IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - mesto {} vsech domu postavilo {} policek marsovskych domu.", t->index.base(), mars);
			}
		}
		for (size_t i = 0; i < want.size(); i++) {
			IConsolePrint(CC_DEFAULT, "testdomy: mest z [{}]: {} z {}", HouseSourceName(want[i].first), found[i], want[i].second);
			if (found[i] != want[i].second) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - mest z [{}] ma byt {}.", HouseSourceName(want[i].first), want[i].second);
		}

		/* The GRFs split (economy.split_house_grfs): with any house GRF but
		 * Mars in the game, no ordinary town is of every house, each is of
		 * one GRF, and the GRFs have the towns evenly -- a difference of one
		 * at most. */
		if (_settings_game.economy.split_house_grfs) {
			std::vector<std::pair<uint32_t, uint>> split;
			for (uint32_t source : sources) {
				if (source < HOUSE_SOURCE_CLIMATE && source != MARS_HOUSES_GRFID) split.emplace_back(source, 0);
			}
			uint plain = 0;
			for (const Town *t : Town::Iterate()) {
				if (t->num_house_sets == 0) plain++;
				if (t->num_house_sets != 1) continue;
				auto it = std::ranges::find(split, t->house_sets[0], &std::pair<uint32_t, uint>::first);
				if (it != split.end()) it->second++;
			}
			uint lo = UINT32_MAX, hi = 0;
			for (const auto &[source, n] : split) {
				IConsolePrint(CC_DEFAULT, "testdomy: deleni: mest z [{}]: {}", HouseSourceName(source), n);
				lo = std::min(lo, n);
				hi = std::max(hi, n);
			}
			IConsolePrint(CC_DEFAULT, "testdomy: deleni: {} GRF, {} mest vsech domu", split.size(), plain);
			if (!split.empty() && plain > 0) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - GRF se deli, a {} mest stavi ze vsech domu.", plain);
			if (!split.empty() && hi - lo > 1) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - GRF se deli nerovnomerne ({} az {}).", lo, hi);
		}
		return true;
	}

	if (argv[1] == "picker" && argv.size() >= 3) {
		/* The house picker's "Houses from": the list shows the set's houses
		 * and nothing else, and something at all. */
		auto psource = ParseHouseSource(argv[2]);
		if (!psource.has_value()) return false;
		/* An optional year: the list shows the houses of that year only. */
		TimerGameCalendar::Year year{0};
		if (argv.size() >= 4) {
			auto pyear = ParseInteger(argv[3]);
			if (!pyear.has_value()) return false;
			year = TimerGameCalendar::Year{static_cast<int32_t>(*pyear)};
		}
		auto [shown, wrong] = TestHousePickerSet(*psource, year);
		IConsolePrint(CC_DEFAULT, "testdomy: picker [{}]{}: {} domu v seznamu, {} odjinud", *psource == 0 ? std::string{"vse"} : HouseSourceName(*psource), year == 0 ? std::string{} : fmt::format(" rok {}", year), shown, wrong);
		if (argv.size() >= 5 && argv[4] == "jmena") {
			for (HouseID id : ListedHousesInPicker()) {
				const HouseSpec *hs = HouseSpec::Get(id);
				HouseID placed = GetTranslatedHouseID(id);
				const HouseSpec *ph = HouseSpec::Get(placed);
				IConsolePrint(CC_DEFAULT, "testdomy:   {} {}{}", id, GetString(GetHouseName(hs)), placed == id ? std::string{} : fmt::format(" -> {} {} ({})", placed, GetString(GetHouseName(ph)), ph->grf_prop.HasGrfFile() ? HouseSourceName(ph->grf_prop.grfid) : std::string{"hra"}));
			}
		}
		if (shown == 0) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - seznam domu je prazdny.");
		if (wrong > 0) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - {} domu v seznamu neni z vybrane sady.", wrong);
		return true;
	}

	if (argv[1] == "rok" && argv.size() >= 3) {
		auto pyear = ParseInteger(argv[2]);
		if (!pyear.has_value()) return false;
		MoveCalendarTo(TimerGameCalendar::Year{static_cast<int32_t>(*pyear)});
		IConsolePrint(CC_DEFAULT, "testdomy: rok {}", TimerGameCalendar::year.base());
		return true;
	}

	if (argv[1] == "pole" && argv.size() >= 4) {
		/* The houses on the map around a tile, within r tiles: each tile's
		 * house by id and name, and how many tiles of each house there are --
		 * for telling which house a player means by where it stands. */
		auto px = ParseInteger(argv[2]);
		auto py = ParseInteger(argv[3]);
		if (!px.has_value() || !py.has_value()) return false;
		int r = 0;
		if (argv.size() >= 5) {
			auto pr = ParseInteger(argv[4]);
			if (!pr.has_value()) return false;
			r = static_cast<int>(*pr);
		}
		std::map<HouseID, uint> seen;
		for (int y = static_cast<int>(*py) - r; y <= static_cast<int>(*py) + r; y++) {
			for (int x = static_cast<int>(*px) - r; x <= static_cast<int>(*px) + r; x++) {
				if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) continue;
				TileIndex t = TileXY(x, y);
				if (!IsTileType(t, TileType::House)) continue;
				HouseID h = GetHouseType(t);
				const HouseSpec *hs = HouseSpec::Get(h);
				seen[h]++;
				IConsolePrint(CC_DEFAULT, "testdomy: pole {},{} (index {}): dum {} (cisty {}, original {}) '{}' ({})", x, y, t.base(), h, GetCleanHouseType(t), IsHouseKeptOriginal(t) ? "ano" : "ne",
						GetString(GetHouseName(hs)), hs->grf_prop.HasGrfFile() ? fmt::format("{:08X}", std::byteswap(hs->grf_prop.grfid)) : std::string{"hra"});
			}
		}
		for (const auto &[h, n] : seen) IConsolePrint(CC_DEFAULT, "testdomy: dum {} '{}': {} polic", h, GetString(GetHouseName(HouseSpec::Get(h))), n);
		return true;
	}

	if (argv[1] == "okno" && argv.size() >= 4) {
		auto ptown = ParseRigTown(argv[2]);
		auto psource = ParseHouseSource(argv[3]);
		if (!ptown.has_value() || !psource.has_value()) return false;
		TownID town = *ptown;
		ShowTownViewWindow(town);
		Window *w = FindWindowById(WindowClass::TownView, town);
		if (w == nullptr || w->GetWidget<NWidgetCore>(WID_TV_HOUSE_SETS) == nullptr) {
			IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - okno mesta nema Domy z.");
			return true;
		}
		if (w->IsWidgetDisabled(WID_TV_HOUSE_SETS)) {
			IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - Domy z jsou zatmavene.");
			return true;
		}
		IConsolePrint(CC_DEFAULT, "testdomy: okno pred: {}", w->GetWidgetString(WID_TV_HOUSE_SETS, STR_NULL));
		w->OnClick({}, WID_TV_HOUSE_SETS, 1);
		if (w->FindChildWindow(WindowClass::DropdownMenu) == nullptr) {
			IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - seznam sad se neotevrel.");
			return true;
		}
		bool was = TownBuildsFrom(Town::Get(town), *psource);
		w->OnDropdownSelect(WID_TV_HOUSE_SETS, static_cast<int>(*psource), -1);
		bool now = TownBuildsFrom(Town::Get(town), *psource);
		IConsolePrint(CC_DEFAULT, "testdomy: okno po: {}", w->GetWidgetString(WID_TV_HOUSE_SETS, STR_NULL));
		if (was == now) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - kliknuti na sadu {} nic nezmenilo.", HouseSourceName(*psource));
		/* The list stays open for the next tick; the rig closes it. */
		CloseWindowByClass(WindowClass::DropdownMenu);
		return true;
	}

	if (argv[1] == "postav" && argv.size() >= 5) {
		/* A house placed by hand: the map holds the house the set put in
		 * place of an original (GetHouseType()), and every tile of that. */
		auto px = ParseInteger(argv[2]);
		auto py = ParseInteger(argv[3]);
		auto ph = ParseInteger(argv[4]);
		if (!px.has_value() || !py.has_value() || !ph.has_value()) return false;
		TileIndex tile = TileXY(static_cast<uint>(*px), static_cast<uint>(*py));
		AutoRestoreBackup cur_company(_current_company, OWNER_DEITY);
		CommandCost ret = Command<Commands::PlaceHouse>::Do(DoCommandFlag::Execute, tile, static_cast<HouseID>(*ph), false, true);
		if (ret.Failed()) {
			IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - dum {} na {},{} nejde postavit: {}", *ph, *px, *py, ret.GetErrorMessage() == INVALID_STRING_ID ? std::string{"bez duvodu"} : GetString(ret.GetErrorMessage()));
			return true;
		}
		HouseID standing = GetHouseType(tile);
		const HouseSpec *hs = HouseSpec::Get(standing);
		uint expected = 1 + (hs->building_flags.Any(BUILDING_2_TILES_X) ? 1 : 0) + (hs->building_flags.Any(BUILDING_2_TILES_Y) ? 1 : 0) + (hs->building_flags.Any(BUILDING_HAS_4_TILES) ? 1 : 0);
		/* Every tile of the house: the ones around whose north part is this tile. */
		uint have = 0;
		for (int dx = 0; dx <= 1; dx++) {
			for (int dy = 0; dy <= 1; dy++) {
				TileIndex t = TileAddXY(tile, dx, dy);
				if (!IsTileType(t, TileType::House)) continue;
				HouseID part = GetHouseType(t);
				if (t + GetHouseNorthPart(part) == tile) have++;
			}
		}
		auto source_of = [](const HouseSpec *h) { return h->grf_prop.HasGrfFile() ? HouseSourceName(h->grf_prop.grfid) : std::string{"hra"}; };
		const HouseSpec *asked = HouseSpec::Get(static_cast<HouseID>(*ph));
		IConsolePrint(CC_DEFAULT, "testdomy: postaven dum {} '{}' ({}), na mape jako {} '{}' ({}), {} policek z {}", *ph, GetString(GetHouseName(asked)), source_of(asked), standing, GetString(GetHouseName(hs)), source_of(hs), have, expected);
		if (have != expected) {
			/* What stands on the four tiles instead. */
			for (int dx = 0; dx <= 1; dx++) {
				for (int dy = 0; dy <= 1; dy++) {
					TileIndex t = TileAddXY(tile, dx, dy);
					if (!IsTileType(t, TileType::House)) {
						IConsolePrint(CC_DEFAULT, "testdomy:   policko +{},+{}: neni dum", dx, dy);
						continue;
					}
					const HouseSpec *ts = HouseSpec::Get(GetHouseType(t));
					IConsolePrint(CC_DEFAULT, "testdomy:   policko +{},+{}: dum {} (cisty {}, original {}) '{}' ({})", dx, dy, GetHouseType(t), GetCleanHouseType(t), IsHouseKeptOriginal(t) ? "ano" : "ne", GetString(GetHouseName(ts)), source_of(ts));
				}
			}
		}
		if (have != expected) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - dum ma mit {} policek a stoji na {}.", expected, have);
		return true;
	}

	if (argv[1] == "rust" && argv.size() >= 4) {
		auto ptown = ParseRigTown(argv[2]);
		auto pn = ParseInteger(argv[3]);
		if (!ptown.has_value() || !pn.has_value()) return false;
		TownID town = *ptown;
		std::vector<bool> before(Map::Size());
		for (TileIndex tile : Map::Iterate()) before[tile.base()] = IsTileType(tile, TileType::House);
		AutoRestoreBackup cur_company(_current_company, OWNER_DEITY);
		Command<Commands::ExpandTown>::Do(DoCommandFlag::Execute, town, static_cast<uint32_t>(*pn), {TownExpandMode::Buildings, TownExpandMode::Roads});
		const Town *t = Town::Get(town);
		uint built = 0, wrong = 0, snowy = 0;
		for (TileIndex tile : Map::Iterate()) {
			if (before[tile.base()] || !IsTileType(tile, TileType::House) || GetTownIndex(tile) != town) continue;
			built++;
			const HouseSpec &hs = HeadHouseSpec(tile);
			if (t->num_house_sets > 0 && !TownBuildsHouse(t, hs, {})) wrong++;
			if (IsSnowyOriginalHouse(hs)) snowy++;
		}
		IConsolePrint(CC_DEFAULT, "testdomy: mesto {} postavilo {} policek domu, stoji {}", town.base(), built, CountHousesBySource(town));
		if (wrong > 0) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - {} policek domu z jine sady, nez ma mesto zvolene.", wrong);
		if (snowy > 0 && _settings_game.game_creation.landscape != LandscapeType::Arctic) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - {} policek zasnezenych domu, a snih tu neni.", snowy);
		return true;
	}

	if (argv[1] == "zaloz" && argv.size() >= 5) {
		auto px = ParseInteger(argv[2]);
		auto py = ParseInteger(argv[3]);
		auto psource = ParseHouseSource(argv[4]);
		if (!px.has_value() || !py.has_value() || !psource.has_value()) return false;
		/* The window is for a company that plays; a scene started without one
		 * gets one, as the player always has. */
		if (Company::GetIfValid(_local_company) == nullptr) {
			extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
			Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
			if (made == nullptr) {
				IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - neni firma, za kterou zakladat.");
				return true;
			}
			SetLocalCompany(made->index);
		}
		{
			/* Founding a town costs more than a new company has. */
			AutoRestoreBackup cur_company(_current_company, _local_company);
			Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
		}
		ShowFoundTownWindow();
		Window *w = FindWindowById(WindowClass::FoundTown, 0);
		if (w == nullptr || w->GetWidget<NWidgetCore>(WID_TF_HOUSE_SET) == nullptr) {
			IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - okno zakladani mest nema Domy z.");
			return true;
		}
		w->OnDropdownSelect(WID_TF_HOUSE_SET, static_cast<int>(*psource), -1);
		IConsolePrint(CC_DEFAULT, "testdomy: zakladam: {}", w->GetWidgetString(WID_TF_HOUSE_SET, STR_NULL));
		uint towns = static_cast<uint>(Town::GetNumItems());
		w->OnPlaceObject({}, TileXY(static_cast<uint>(*px), static_cast<uint>(*py)));
		w->Close();
		if (Town::GetNumItems() == towns) {
			/* The window's refusal went to an error window nobody sees here;
			 * ask the command the same question for its answer. */
			AutoRestoreBackup cur_company(_current_company, _local_company);
			auto [why, cost, id] = Command<Commands::FoundTown>::Do(DoCommandFlags{}, TileXY(static_cast<uint>(*px), static_cast<uint>(*py)),
					TownSize::Medium, false, _settings_game.economy.town_layout, false, 0, "testdomy", *psource);
			IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - mesto se nezalozilo: {}", why.Failed() ? GetString(why.GetErrorMessage()) : std::string{"prikaz by prosel"});
			return true;
		}
		const Town *t = nullptr;
		for (const Town *c : Town::Iterate()) t = c; // the newest
		uint wrong = 0;
		for (TileIndex tile : Map::Iterate()) {
			if (!IsTileType(tile, TileType::House) || GetTownIndex(tile) != t->index) continue;
			if (*psource != 0 && !TownBuildsHouse(t, HeadHouseSpec(tile), {})) wrong++;
		}
		IConsolePrint(CC_DEFAULT, "testdomy: zalozeno mesto {} s {} sadami, stoji {}", t->index.base(), t->num_house_sets, CountHousesBySource(t->index));
		if (*psource != 0 && !TownBuildsFrom(t, *psource)) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - mesto nema zvolenou sadu, ze ktere bylo zalozeno.");
		if (wrong > 0) IConsolePrint(CC_ERROR, "testdomy: ODMITNUTO - {} policek domu z jine sady, nez ze ktere bylo mesto zalozeno.", wrong);
		return true;
	}

	return false;
}

/**
 * Say how much of each industry building is left. Usage: teststavby
 * @copydoc IConsoleCmdProc
 */
static bool ConTestIndustryHealth(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	uint count = 0;
	for (const Industry *i : Industry::Iterate()) {
		IConsolePrint(CC_DEFAULT, "prumysl {}: ({},{}) stav {} %", i->index.base(), TileX(i->location.tile), TileY(i->location.tile), GetIndustryHealthPercent(i));
		count++;
	}
	IConsolePrint(CC_DEFAULT, "teststavby: prumyslu celkem {}", count);
	return true;
}

/**
 * Print what is in the papers, newest first. Usage: testnoviny
 *
 * A news line is written when something happens and read much later; the
 * only way to see what it says without a build in hand is to print it.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestNews(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	uint count = 0;
	for (const NewsItem &ni : GetNews()) {
		IConsolePrint(CC_DEFAULT, "testnoviny: {}", ni.headline.GetDecodedString());
		count++;
	}
	if (count == 0) IConsolePrint(CC_DEFAULT, "testnoviny: noviny jsou prazdne.");
	return true;
}

/**
 * Count the puffs of smoke still hanging over the map, and say how much
 * life the longest-lived one has left. Usage: testdym
 *
 * The smoke of a raid is meant to last three weeks from the moment it is
 * dropped; the only way to know that it does is to count it going away.
 *
 * testdym shod <x> <y> [smer] drops a carpet on the spot this tick, with no
 * ship or aircraft in it. What the smoke does to what is under it can only
 * be measured with the smoke falling on a known vehicle at a known moment,
 * and a flight arrives when it arrives.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSmoke(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	if (argv.size() >= 4 && argv[1] == "shod") {
		auto px = ParseInteger(argv[2]);
		auto py = ParseInteger(argv[3]);
		if (!px.has_value() || !py.has_value()) return false;
		Direction facing = Direction::NE;
		if (argv.size() >= 5) {
			auto pd = ParseInteger(argv[4]);
			if (!pd.has_value() || *pd > 7) return false;
			facing = static_cast<Direction>(*pd);
		}
		if (!Company::IsValidID(CompanyID::Begin())) {
			IConsolePrint(CC_ERROR, "testdym: hra nema firmu, pust to ze savu.");
			return true;
		}
		/* Its own switch, so the output that follows says what it did. */
		AutoRestoreBackup trace(_show_train_orientation, true);
		DropRaidSmoke(TileXY((uint)*px, (uint)*py), facing, CompanyID::Begin());
		return true;
	}
	uint count = 0;
	uint16_t longest = 0;
	for (const EffectVehicle *e : EffectVehicle::Iterate()) {
		if (e->subtype != EV_BREAKDOWN_SMOKE) continue;
		count++;
		longest = std::max<uint16_t>(longest, e->animation_state);
	}
	uint rockets = 0;
	for (const EffectVehicle *e : EffectVehicle::Iterate()) {
		if (e->subtype != EV_RAID_ROCKET) continue;
		rockets++;
		IConsolePrint(CC_DEFAULT, "testdym: raketa na ({},{}) pix ({},{}) vyska {} smer {} barva {} zapalnice {} cil ({},{})",
				TileX(TileVirtXY(e->x_pos, e->y_pos)), TileY(TileVirtXY(e->x_pos, e->y_pos)), e->x_pos, e->y_pos, e->z_pos,
				to_underlying(e->direction), e->animation_substate == 0 ? "seda" : "zluta", e->animation_state,
				TileX(e->dest_tile), TileY(e->dest_tile));
	}
	IConsolePrint(CC_DEFAULT, "testdym: oblacku {}, raket {}, nejdele jeste {} tiku ({} dnu), tik {}", count, rockets,
			longest, longest / Ticks::DAY_TICKS, TimerGameTick::counter);
	return true;
}

/**
 * Show, or stop showing, how much of an industry's building is left in its
 * window. Typed as "miluju karla": the console takes the first word as the
 * command and the rest as its arguments, so the second word is asked for
 * here rather than being part of the name.
 *
 * Nothing but this player's windows changes. The switch is not in the
 * savegame and is not sent to anybody, so in a network game the others go on
 * seeing exactly what they saw.
 * @copydoc IConsoleCmdProc
 */
static bool ConIndustryHealth(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	if (argv.size() < 2 || argv[1] != "karla") {
		IConsolePrint(CC_HELP, "Show how much of an industry's building is left, in its own window.");
		IConsolePrint(CC_HELP, "Usage: 'miluju karla' to flip it, or 'miluju karla on' / 'miluju karla off'.");
		return true;
	}

	/* The switch itself is the third word: the console took the first as the
	 * command name and the second is the rest of the name. */
	if (argv.size() > 2) {
		if (argv[2] == "on" || argv[2] == "1") {
			_show_industry_health = true;
		} else if (argv[2] == "off" || argv[2] == "0") {
			_show_industry_health = false;
		} else {
			return false;
		}
	} else {
		_show_industry_health = !_show_industry_health;
	}

	/* The industry windows that are already open pick it up on their next
	 * painting: the panel measures what it drew and grows or shrinks by
	 * itself. */
	SetWindowClassesDirty(WindowClass::IndustryView);
	/* The vehicle windows cannot. Whether the crosshair is in one is decided
	 * when the window works out its rows, and that only runs when something
	 * tells the window its contents have changed -- painting it again is not
	 * enough. Without this the crosshair turned up only after the player shut
	 * the window and opened it again. */
	InvalidateWindowClassesData(WindowClass::VehicleView);

	IConsolePrint(CC_DEFAULT, "Stav budovy prumyslu je ted {}.", _show_industry_health ? "videt" : "schovany");
	return true;
}

/**
 * Build the coupling test scene, headless, so the departure direction can be
 * watched from a console trace instead of from a phone.
 *
 * Lays out, on the flattest strip it can find: a depot at the west end, a
 * plain line eastwards ending blind (the reversing stub), and a four-tile
 * through platform in the middle. Then two trains in the depot: one engine
 * with three wagons, ordered to the station with "decouple, keep 1" and then
 * back to the depot to halt -- it delivers the rake and gets out of the way --
 * and one light engine with "go to couple" at the same station and a depot
 * order after it. The light engine's own hold keeps it in the shed until the
 * rake is standing at the platform, so the whole scene sequences itself.
 *
 * What to read afterwards with vlak123 on: the "pred spojenim"/"spojeno"
 * lines say what the coupling measured and decided, and the following
 * arrival/line-end lines say which way the joined train really went --
 * east to the stub is the pushed-out departure, west back into the depot is
 * the wrong one.
 * @copydoc IConsoleCmdProc
 */
static bool _testspoj_active = false;
static uint _testmapa_area[4]; ///< Last built test scene's surroundings, for a no-argument testmapa.

static Train *FindTrainByUnit(uint unit);
static bool MakeEngineOfPieces(Train *t, uint pieces);

static bool ConTestCouple(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the coupling test scene. Usage: 'testspoj'.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testspoj: only in a running game.");
		return true;
	}

	/* 'couvej' turns the collector round in the shed so it approaches
	 * backing; 'depo' moves the whole exchange into the west depot: the
	 * deliverer stores its rake in there and the collector fetches it with a
	 * go-to-couple depot order. 'rad' puts a timetabled stay on the deliver
	 * order: the engine must drop and go at once, and the rake must stand the
	 * stay out idle -- uncollectable -- before the collector gets it. */
	/* 'blok' stages the occupied platform: the deliverer keeps a wagon told
	 * to fill up on a map with no cargo, so it never pulls clear of the rake
	 * it dropped -- the collector must then never be sent for it. */
	/* 'vlek' plays the two-train shuttle: a light engine collects a whole
	 * waiting train at the east station and puts it down at a second, western
	 * station, where the dropped train's way home lies right through where
	 * the light engine stands and the other way round. */
	bool backing = false;
	bool depot_mode = false;
	bool timetabled = false;
	bool blocked = false;
	bool tow_mode = false;
	bool counted = false;
	bool parked = false;
	bool swap_mode = false;
	bool store_mode = false;
	bool found_mode = false;
	bool waypoint_mode = false;
	/* 'tendr' gives the collector an engine that carries its tender as an
	 * articulated part -- a steam engine, the whole of it one vehicle. Such a
	 * consist is a single unit and its list cannot be turned round, so meeting
	 * the rake nose first is the one coupling the game used to refuse. Nothing
	 * in the rig ever built one: the default set has no engine with a tender,
	 * so this scene wants a set that does (the rig's h2 home). */
	bool tender_mode = false;
	/* 'okno' opens the collector's vehicle window before it sets off. A window
	 * follows its train by the index of the head, and a coupling that turns
	 * the list round gives the train a new head: the window has to be handed
	 * on with the rest of the identity, or it goes on asking the old head
	 * questions only a head may be asked. The rig has no screen, but it has
	 * windows -- they are built and refreshed, just never drawn -- and that
	 * refresh is where it broke for the player. */
	bool window_mode = false;
	/* 'smes' makes the dropped rake a mixed one: wagons of two cargoes, and
	 * the collector asking for one of them. The question that needs is how
	 * much of the rake the fullness filter looks at -- with one empty wagon
	 * of the other cargo standing in it, a rake whose asked-for wagons are
	 * full is either collected or it is not. The player's own case. */
	bool mixed_mode = false;
	/* 'kloub' builds the rake of a set's wagons drawn in several pieces, and
	 * wants a set that has them (the rig's h_tir home). A collector meeting
	 * such a rake head on has every wagon flipped and its pieces traded round
	 * -- the coupling the player saw come out as two wagons drawn over each
	 * other. 'jeden' makes the rake one wagon: a single unit that has to be
	 * turned round inside itself, which used to be refused. 'clanky' makes
	 * the collector's engine a unit of three pieces (MakeEngineOfPieces()),
	 * the shape of the player's shunter, arriving nose first. */
	bool artic_wagon_mode = false;
	bool single_mode = false;
	bool pieces_mode = false;
	uint want_n = 0;
	for (size_t i = 1; i < argv.size(); i++) {
		if (argv[i] == "tendr") tender_mode = true;
		if (argv[i] == "okno") window_mode = true;
		if (argv[i] == "smes") mixed_mode = true;
		if (argv[i] == "kloub") artic_wagon_mode = true;
		if (argv[i] == "jeden") single_mode = true;
		if (argv[i] == "clanky") pieces_mode = true;
		if (argv[i] == "couvej") backing = true;
		if (argv[i] == "depo") depot_mode = true;
		if (argv[i] == "rad") timetabled = true;
		if (argv[i] == "blok") blocked = true;
		if (argv[i] == "vlek") tow_mode = true;
		if (argv[i] == "pocet") counted = true;
		if (argv[i] == "stoji") parked = true;
		if (argv[i] == "oboji") swap_mode = true;
		if (argv[i] == "sklad") store_mode = true;
		if (argv[i] == "zaloz") found_mode = true;
		if (argv[i] == "smer") waypoint_mode = true;
		/* A bare number is how many the collect order asks for, so the store
		 * scene can be pointed at any count without a word for each one. */
		uint n = 0;
		if (!argv[i].empty() && std::all_of(argv[i].begin(), argv[i].end(), [](char c) { return c >= '0' && c <= '9'; })) {
			for (char c : argv[i]) n = n * 10 + (c - '0');
			if (n != 0) want_n = n;
		}
	}
	/* 'sklad' fills the west depot with wagons in more than one stored rake
	 * before anybody gets there, so a collect order carrying a number has a
	 * store to take that number out of -- the player's own case, where a shed
	 * holds far more than one order wants. */
	if (store_mode) depot_mode = true;
	/* 'oboji' is a depot exchange on one order: the deliverer drops the rake
	 * it brought and takes a different one that is already stored in the same
	 * shed. Only a depot order may do both, and the point of the test is that
	 * what was just put down is not what gets picked up. */
	if (swap_mode) depot_mode = true;
	/* 'smer' only means anything to the founding scene: it puts one station
	 * waypoint across both platform roads and the founding order behind it,
	 * which is the arrangement the player builds and the only one in which a
	 * finished rake sends the feeder to the next platform instead of making
	 * it wait. */
	if (waypoint_mode) found_mode = true;

	/* A headless newgame (null video driver has no GUI) starts like a
	 * dedicated server: spectating, no company anywhere. Make one to build
	 * as, the same way the GUI newgame path does. */
	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testspoj: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}

	/* Enough money that no build below can fail on cost. */
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);

	/* Find engines first; their railtype decides what gets laid. */
	EngineID eid_loco = EngineID::Invalid();
	EngineID eid_wagon = EngineID::Invalid();
	/* A second kind of wagon, for the store scene: a wagon built in a shed
	 * joins any loose chain of its own kind already standing there
	 * (FindGoodVehiclePos()), and no command takes a free rake apart -- so the
	 * only way to put two separate rakes in one shed is to make them of
	 * different stock. Which is the ordinary case in a real game anyway. */
	EngineID eid_wagon2 = EngineID::Invalid();
	/* The collector's engine in 'tendr': one that brings a tender along as an
	 * articulated part. Kept apart from eid_loco so the deliverer stays an
	 * ordinary engine and only the collector is the awkward one. */
	EngineID eid_tender = EngineID::Invalid();
	/* The rake's wagon in 'kloub': one drawn in several pieces. */
	EngineID eid_artic_wagon = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type == RailVehicleType::Wagon) {
			if (eid_wagon == EngineID::Invalid()) {
				eid_wagon = e->index;
			} else if (eid_wagon2 == EngineID::Invalid()) {
				eid_wagon2 = e->index;
			}
			if (eid_artic_wagon == EngineID::Invalid() && CountArticulatedParts(e->index) > 0) eid_artic_wagon = e->index;
		} else {
			if (eid_loco == EngineID::Invalid()) eid_loco = e->index;
			if (eid_tender == EngineID::Invalid() && CountArticulatedParts(e->index) > 0) eid_tender = e->index;
		}
		if (eid_loco != EngineID::Invalid() && eid_wagon2 != EngineID::Invalid() && (!tender_mode || eid_tender != EngineID::Invalid()) &&
				(!artic_wagon_mode || eid_artic_wagon != EngineID::Invalid())) break;
	}
	if (tender_mode && eid_tender == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testspoj tendr: v teto hre neni zadna masinka s tendrem (kloubova). Chce to sadu, ktera ji ma.");
		return true;
	}
	if (artic_wagon_mode) {
		if (eid_artic_wagon == EngineID::Invalid()) {
			IConsolePrint(CC_ERROR, "testspoj kloub: v teto hre neni zadny vagon z vice clanku. Chce to sadu, ktera ho ma.");
			return true;
		}
		eid_wagon = eid_artic_wagon;
		IConsolePrint(CC_DEFAULT, "testspoj kloub: rada bude z vagonu typu {} o {} clancich.", eid_wagon.base(), CountArticulatedParts(eid_wagon) + 1);
	}
	if (eid_loco == EngineID::Invalid() || eid_wagon == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testspoj: no available engine or wagon.");
		return true;
	}

	/* The flattest clear run of tiles along the X axis. */
	static const uint LEN = 40;
	TileIndex strip = INVALID_TILE;
	for (uint y = 8; y < Map::SizeY() - 8 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			TileIndex t = TileXY(x, y);
			bool ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
			int z = ok ? GetTileZ(t) : -1;
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					strip = TileXY(x - LEN + 1, y);
					break;
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testspoj: no flat clear strip of {} tiles found.", LEN);
		return true;
	}

	uint x0 = TileX(strip);
	uint y0 = TileY(strip);
	IConsolePrint(CC_DEFAULT, "testspoj: strip at ({},{})..({},{}).", x0, y0, x0 + LEN - 1, y0);
	_testmapa_area[0] = x0; _testmapa_area[1] = y0 > 1 ? y0 - 1 : 0;
	_testmapa_area[2] = x0 + LEN - 1; _testmapa_area[3] = y0 + 1;

	/* A depot at each end of one straight line, doors facing inwards, and a
	 * through platform in the middle. The deliverer comes out of the east
	 * depot, puts its wagons down at the platform and carries on west into
	 * the west depot, so the line east of the rake is clear for the
	 * collector, which follows from the east with its next order pointing
	 * back east -- the exact shape of the failing case. A correct departure
	 * pushes the rake on west into the west depot; the wrong one turns back
	 * east into the east depot; the arrival trace names which. */
	TileIndex depot_w = TileXY(x0, y0);
	TileIndex depot_e = TileXY(x0 + LEN - 1, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_w, RAILTYPE_RAIL, DiagDirection::SW).Failed() ||
			Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_e, RAILTYPE_RAIL, DiagDirection::NE).Failed()) {
		IConsolePrint(CC_ERROR, "testspoj: depot failed.");
		return true;
	}
	if (Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, y0), TileXY(x0 + 1, y0), RAILTYPE_RAIL, Track::X, false, true).Failed()) {
		IConsolePrint(CC_ERROR, "testspoj: track failed.");
		return true;
	}
	TileIndex st_tile = TileXY(x0 + 18, y0);
	if (Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, st_tile, RAILTYPE_RAIL, Axis::X, 1, 4, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed()) {
		IConsolePrint(CC_ERROR, "testspoj: station failed.");
		return true;
	}
	/* Two-way path signals a little outside each depot and on both throats
	 * of the platform, matching how the player's test station is signalled;
	 * without any signals the whole line is one block and no depot ever lets
	 * a train out while the rake stands anywhere on it. */
	for (uint sx : {x0 + 2, x0 + 15, x0 + 24, x0 + LEN - 3}) {
		if (Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(sx, y0), Track::X, SignalType::Path, SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, 0, 0).Failed()) {
			IConsolePrint(CC_ERROR, "testspoj: signal at ({},{}) failed.", sx, y0);
			return true;
		}
	}

	/* The command wrapper normally flushes the signal-update buffer after
	 * each command; calling the commands directly skips the wrapper, so
	 * flush by hand before anything ticks. */
	UpdateSignalsInBuffer();

	StationID st_id = GetStationIndex(st_tile);
	DepotID dep_w = GetDepotIndex(depot_w);
	DepotID dep_e = GetDepotIndex(depot_e);

	if (found_mode) {
		/* A second platform beside the first, on the row below, with a switch
		 * before the station and a merge after it. A rake being founded stands
		 * on its platform for weeks, and on one track that rake is a wall: the
		 * feeder could never get back past it to the shed for the next pair,
		 * nor the collector reach it. A station that grows rakes has a
		 * through platform next to them, in any real layout too. */
		TileIndex row2_a = TileXY(x0 + 16, y0 + 1);
		TileIndex row2_b = TileXY(x0 + 23, y0 + 1);
		Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, row2_b, row2_a, false, LevelMode::Level);
		/* Each half of an S-curve sits in one column: the piece on the main
		 * row leaves by its south-east edge straight into the tile below it. */
		bool ok = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + 16, y0), RAILTYPE_RAIL, Track::Right, false).Succeeded() &&
				Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + 16, y0 + 1), RAILTYPE_RAIL, Track::Left, false).Succeeded() &&
				Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + 17, y0 + 1), RAILTYPE_RAIL, Track::X, false).Succeeded() &&
				Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, TileXY(x0 + 18, y0 + 1), RAILTYPE_RAIL, Axis::X, 1, 4, STAT_CLASS_DFLT, 0, st_id, false).Succeeded() &&
				Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + 22, y0 + 1), RAILTYPE_RAIL, Track::X, false).Succeeded() &&
				Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + 23, y0 + 1), RAILTYPE_RAIL, Track::Upper, false).Succeeded() &&
				Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + 23, y0), RAILTYPE_RAIL, Track::Lower, false).Succeeded();
		if (!ok) {
			IConsolePrint(CC_ERROR, "testspoj zaloz: druhe nastupiste se nepodarilo postavit.");
			return true;
		}
		UpdateSignalsInBuffer();

		if (waypoint_mode) {
			/* One station waypoint on the throat, ahead of the switch that
			 * splits to the second platform, so both platforms lie behind it
			 * and nothing else does. It is the waypoint that says which
			 * platforms this founding order is about -- and when the rake on
			 * one of them is finished, the other is somewhere to found the
			 * next. (A waypoint laid across both roads past the switch is the
			 * same thing to the code, which walks every tile of it; this side
			 * of the switch is simply the shorter thing to build.) */
			TileIndex wp_tile = TileXY(x0 + 14, y0);
			if (Command<Commands::BuildRailWaypoint>::Do(DoCommandFlag::Execute, wp_tile, Axis::X, 1, 1, STAT_CLASS_WAYP, 0, StationID::Invalid(), false, true).Failed()) {
				IConsolePrint(CC_ERROR, "testspoj zaloz smer: smerovani se nepodarilo postavit.");
				return true;
			}
			/* The scene's signals are one-way path signals (that is what
			 * building a path signal on bare track gives), and the one past
			 * the waypoint faces the other way -- so a train standing on the
			 * waypoint has nothing safe in front of it and never books its way
			 * there at all. Only in this scene, the throat signal is made
			 * two-way, which is how the player's own station is signalled. */
			Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(x0 + 15, y0), Track::X, SignalType::Path, SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, 0, SignalOnTrack(Track::X));
			UpdateSignalsInBuffer();
		}
	}

	if (tow_mode) {
		/* The second, western station the collected train is put down at. */
		TileIndex st2_tile = TileXY(x0 + 7, y0);
		if (Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, st2_tile, RAILTYPE_RAIL, Axis::X, 1, 2, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed()) {
			IConsolePrint(CC_ERROR, "testspoj: station 2 failed.");
			return true;
		}
		UpdateSignalsInBuffer();
		StationID st2_id = GetStationIndex(st2_tile);

		/* The train to be carried: engine and two wagons, sent to the east
		 * station to stand waiting to be collected. */
		auto [cost_a, veh_a, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_a.Failed()) {
			IConsolePrint(CC_ERROR, "testspoj: vlek train failed.");
			return true;
		}
		for (int i = 0; i < 2; i++) {
			auto [costw, wid, un_d, un_e, un_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (costw.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wid, Train::Get(veh_a)->Last()->index, false).Failed()) {
				IConsolePrint(CC_ERROR, "testspoj: vlek wagon failed.");
				return true;
			}
		}
		Order wait_at_st0;
		wait_at_st0.MakeGoToStation(st_id);
		wait_at_st0.SetLoadType(OrderLoadType::NoLoad);
		wait_at_st0.SetUnloadType(OrderUnloadType::NoUnload);
		wait_at_st0.SetWaitForCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_a, 0, wait_at_st0);

		/* The light engine: collect the waiting train at the east station,
		 * put it down at the western one keeping just itself, then home to
		 * the east depot -- right through where the dropped train stands. */
		auto [cost_b, veh_b, un_g, un_h, un_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_b.Failed()) {
			IConsolePrint(CC_ERROR, "testspoj: vlek engine failed.");
			return true;
		}
		Order collect_b;
		collect_b.MakeGoToStation(st_id);
		collect_b.SetLoadType(OrderLoadType::NoLoad);
		collect_b.SetUnloadType(OrderUnloadType::NoUnload);
		collect_b.SetGoToCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_b, 0, collect_b);
		Order drop_b;
		drop_b.MakeGoToStation(st2_id);
		drop_b.SetLoadType(OrderLoadType::NoLoad);
		drop_b.SetUnloadType(OrderUnloadType::NoUnload);
		drop_b.SetDecouple(true);
		drop_b.SetDecoupleCount(0);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_b, 1, drop_b);
		Order home_b;
		home_b.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_b, 2, home_b);

		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_a, false);
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_b, false);

		/* 'vlek blok' adds the obstacle the player staged by hand: a third
		 * engine, built with the brake on so it does not tangle with the
		 * collection run on the single line. Released mid-scene with
		 * 'testbrzda' while the tow is underway, it heads for the western
		 * station (full-load there, on a map with no cargo, so it would hold
		 * it for good) and meets the returning drop engine head to head at a
		 * signal: the drop engine gets a red and stands. 'testskip' then
		 * turns the obstacle round for home, the line frees, and the engine
		 * standing at the red must take the path by itself, unforced --
		 * the case the player staged by hand on the phone. */
		if (blocked) {
			auto [cost_c, veh_c, un_j, un_k, un_l] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
			if (cost_c.Failed()) {
				IConsolePrint(CC_ERROR, "testspoj: vlek obstacle engine failed.");
				return true;
			}
			Order park_c;
			park_c.MakeGoToStation(st2_id);
			park_c.SetLoadType(OrderLoadType::FullLoadAny);
			park_c.SetUnloadType(OrderUnloadType::NoUnload);
			Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_c, 0, park_c);
			Order home_c;
			home_c.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
			Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_c, 1, home_c);
			IConsolePrint(CC_DEFAULT, "testspoj vlek blok: prekazka=vlak {} (vyjede na 'testbrzda {}', dal ji posle 'testskip {}').",
					Train::Get(veh_c)->unitnumber, Train::Get(veh_c)->unitnumber, Train::Get(veh_c)->unitnumber);
		}

		_testspoj_active = true;
		IConsolePrint(CC_DEFAULT, "testspoj vlek: vezeny=vlak {}, masinka=vlak {}, stanice0={} stanice2={} ({}..{},{}).",
				Train::Get(veh_a)->unitnumber, Train::Get(veh_b)->unitnumber, st_id, st2_id, x0 + 7, x0 + 8, y0);
		return true;
	}

	/* The delivering train: engine and three wagons. */
	auto [cost2, veh2, unused_a, unused_b, unused_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	if (cost2.Failed()) {
		IConsolePrint(CC_ERROR, "testspoj: engine 2 failed.");
		return true;
	}
	if (mixed_mode && eid_wagon2 == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testspoj smes: v teto hre je jen jeden druh vagonu, smiseny vlak nejde postavit.");
		return true;
	}
	for (int i = 0; i < (single_mode ? 1 : 3); i++) {
		/* 'smes': the last of the three is of the other kind, and stays empty.
		 * That is the wagon the fullness question must not be asked about. */
		EngineID what = (mixed_mode && i == 2) ? eid_wagon2 : eid_wagon;
		auto [costw, wid, unused_d, unused_e, unused_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, what, true, INVALID_CARGO, ClientID::Invalid);
		if (costw.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wid, Train::Get(veh2)->Last()->index, false).Failed()) {
			IConsolePrint(CC_ERROR, "testspoj: wagon failed.");
			return true;
		}
	}
	if (mixed_mode) {
		IConsolePrint(CC_DEFAULT, "testspoj smes: rada je dva vagony nakladu {} a jeden nakladu {}.",
				(int)Engine::Get(eid_wagon)->GetDefaultCargoType(), (int)Engine::Get(eid_wagon2)->GetDefaultCargoType());
	}

	if (store_mode) {
		/* Two separate stored rakes, five and three, so the scene covers both
		 * halves of what a store means: taking part of one rake, and taking
		 * across two of them.
		 *
		 * Two kinds of wagon, because that is the only way to get two rakes.
		 * A wagon built in a shed joins any loose chain of its own kind that is
		 * already standing there (FindGoodVehiclePos()), and there is no
		 * command that takes a free rake apart afterwards -- CmdMoveRailVehicle
		 * with no destination does not detach a wagon, it looks for a good
		 * place to put it, and the good place is the chain it just came from.
		 * Two earlier versions of this scene did not know that and quietly
		 * built one rake of eight, which turned the "across two rakes" test
		 * into another "out of one rake" test that passed for the wrong
		 * reason. */
		if (eid_wagon2 == EngineID::Invalid()) {
			IConsolePrint(CC_ERROR, "testspoj sklad: k dispozici je jen jeden druh vagonu, dve oddelene rady se postavit nedaji.");
			return true;
		}
		for (int i = 0; i < 8; i++) {
			auto [costs, sid, unused_m, unused_n, unused_o] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, i < 5 ? eid_wagon : eid_wagon2, true, INVALID_CARGO, ClientID::Invalid);
			if (costs.Failed()) {
				IConsolePrint(CC_ERROR, "testspoj sklad: odlozeny vagon se nepodaril.");
				return true;
			}
		}

		/* Report what really stands there, not what was meant to. */
		std::string what;
		for (const Train *rake : Train::Iterate()) {
			if (!rake->IsFreeWagon() || rake->tile != depot_w) continue;
			uint units = 0;
			for (const Train *u = rake; u != nullptr; u = u->GetNextUnit()) units++;
			if (!what.empty()) what += " + ";
			what += fmt::format("{}", units);
		}
		IConsolePrint(CC_DEFAULT, "testspoj sklad: v depu ({},{}) lezi rady o {} vozech; rozkaz chce sebrat {}.",
				x0, y0, what, want_n != 0 ? want_n : (counted ? 3 : 0));
	}

	Order deliver;
	if (swap_mode) {
		/* Store a rake of two wagons in the west depot before anyone gets
		 * there, so the exchange has something to pick up that is not the
		 * three wagons the deliverer is about to put down. */
		VehicleID stored = VehicleID::Invalid();
		for (int i = 0; i < 2; i++) {
			auto [costs, sid, unused_j, unused_k, unused_l] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (costs.Failed()) {
				IConsolePrint(CC_ERROR, "testspoj oboji: odlozeny vagon se nepodaril.");
				return true;
			}
			if (stored == VehicleID::Invalid()) {
				stored = sid;
			} else if (Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, sid, Train::Get(stored)->Last()->index, false).Failed()) {
				IConsolePrint(CC_ERROR, "testspoj oboji: odlozene vagony se nespojily.");
				return true;
			}
		}

		/* One order, both halves: leave three behind, take the stored two on.
		 * Then home to the east depot, so the run says plainly whether the
		 * exchange finished or the train sat in the shed. */
		deliver.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
		deliver.SetDecouple(true);
		deliver.SetDecoupleCount(0);
		deliver.SetGoToCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 0, deliver);
		Order home_swap;
		home_swap.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 1, home_swap);
		IConsolePrint(CC_DEFAULT, "testspoj oboji: v depu ({},{}) lezi 2 odlozene vagony; odkladacka ma nechat 3 a vzit si je.", x0, y0);
	} else if (depot_mode) {
		/* Deliver straight into the west depot and stay there, halted; the
		 * rake is stored in the shed by the depot decouple. */
		deliver.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		deliver.SetDecouple(true);
		deliver.SetDecoupleCount(0);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 0, deliver);
	} else {
		deliver.MakeGoToStation(st_id);
		if (blocked) {
			deliver.SetLoadType(OrderLoadType::FullLoadAny);
			deliver.SetUnloadType(OrderUnloadType::NoUnload);
			deliver.SetDecouple(true);
			deliver.SetDecoupleCount(1);
			IConsolePrint(CC_DEFAULT, "testspoj: odkladacka si necha vagon na plnou nakladku - z nastupiste neodjede.");
		} else if (mixed_mode) {
			/* Left to load as an ordinary stop would, not told "load nothing".
			 * A rake put down under "load nothing" counts as full whatever is
			 * in it -- it has been told nothing more is going into it -- and
			 * the fullness filter then never has a question to answer. This
			 * scene is about that question, so the rake is dropped with an
			 * ordinary loading job, which leaves it collectable and genuinely
			 * as full or as empty as its wagons are. */
			deliver.SetUnloadType(OrderUnloadType::NoUnload);
			deliver.SetDecouple(true);
			deliver.SetDecoupleCount(0);
		} else {
			deliver.SetLoadType(OrderLoadType::NoLoad);
			deliver.SetUnloadType(OrderUnloadType::NoUnload);
			deliver.SetDecouple(true);
			deliver.SetDecoupleCount(0);
		}
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 0, deliver);
		if (timetabled) {
			/* Timetable data goes through its own command; an order inserted
			 * with the fields pre-filled is refused whole. */
			if (Command<Commands::ChangeTimetable>::Do(DoCommandFlag::Execute, veh2, 0, MTF_WAIT_TIME, 30 * Ticks::DAY_TICKS).Succeeded()) {
				IConsolePrint(CC_DEFAULT, "testspoj: odpojovaci prikaz ma jizdni rad 30 dni.");
			} else {
				IConsolePrint(CC_ERROR, "testspoj: jizdni rad se nepodaril nastavit.");
			}
		}
		Order home_w;
		home_w.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 1, home_w);
	}

	/* No separate collector in the exchange scene: the whole point is that one
	 * train does both halves, and a second engine sent to the same shed would
	 * simply race it for the stored rake. */
	if (swap_mode) {
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh2, false);
		_testspoj_active = true;
		IConsolePrint(CC_DEFAULT, "testspoj oboji: scena hotova. odkladacka=vlak {}, zapadni depo ({},{}), vychodni depo ({},{}).",
				Train::Get(veh2)->unitnumber, x0, y0, x0 + LEN - 1, y0);
		return true;
	}

	/* The collector: a light engine sent to couple, with its next stop lying
	 * behind it -- the depot it starts from -- which is the exact shape of the
	 * player's failing case. */
	auto [cost1, veh1, unused_g, unused_h, unused_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, parked ? depot_w : depot_e, tender_mode ? eid_tender : eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	if (cost1.Failed()) {
		IConsolePrint(CC_ERROR, "testspoj: engine 1 failed.");
		return true;
	}
	if (tender_mode) {
		IConsolePrint(CC_DEFAULT, "testspoj tendr: sberacka je vlak {}, {} clanku - jede na radu nosem napred.",
				Train::Get(veh1)->unitnumber, CountArticulatedParts(eid_tender) + 1);
	}
	if (pieces_mode) {
		if (!MakeEngineOfPieces(Train::Get(veh1), 2)) {
			IConsolePrint(CC_ERROR, "testspoj clanky: sberacku se nepodarilo rozdelit na clanky.");
			return true;
		}
		IConsolePrint(CC_DEFAULT, "testspoj clanky: sberacka je vlak {}, masinka ze 3 clanku ({} dilku) - jede na radu nosem napred.",
				Train::Get(veh1)->unitnumber, Train::Get(veh1)->gcache.cached_total_length);
	}
	if (window_mode) {
		ShowVehicleViewWindow(Train::Get(veh1));
		IConsolePrint(CC_DEFAULT, "testspoj okno: okno sberacky (vlak {}) otevreno.", Train::Get(veh1)->unitnumber);
	}
	Order collect;
	if (depot_mode) {
		collect.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
		collect.SetGoToCouple(true);
		/* 'stoji' builds the collector in the very depot its collect order
		 * names, so it is already standing there with nothing yet to fetch --
		 * the player's own case, where a depot order that says "stop" had
		 * parked it in that shed. Brake it mid-scene with 'testbrzda' and it
		 * must still take the wagons once they arrive. */
		/* 'pocet' adds the wagon-count filter the player's own collect order
		 * carries and the plain scene never exercised. The deliverer above
		 * stores exactly three wagons, so this asks for what is really there. */
		if (want_n != 0) {
			collect.SetCoupleCount(want_n);
		} else if (counted) {
			collect.SetCoupleCount(3);
		}
	} else {
		collect.MakeGoToStation(st_id);
		collect.SetLoadType(OrderLoadType::NoLoad);
		collect.SetUnloadType(OrderUnloadType::NoUnload);
		collect.SetGoToCouple(true);
	}
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 0, collect);
	Order home_e;
	home_e.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 1, home_e);

	/* 'testspoj couvej' turns the collector round in the shed first, so it
	 * approaches the rake driving backwards -- the same way an engine that
	 * turned on a stub arrives in the player's game. */
	if (backing) {
		Command<Commands::ReverseTrainDirection>::Do(DoCommandFlag::Execute, veh1, false);
		IConsolePrint(CC_DEFAULT, "testspoj: collector will back onto the rake.");
	}

	/* A second collector on the store scene, with the same order. Two engines
	 * sent to one store is the question the reserved rows exist to answer:
	 * each has to end up with its own share put aside for it, and neither may
	 * be handed what the other has already spoken for. */
	VehicleID veh3 = VehicleID::Invalid();
	if (store_mode) {
		auto [cost3, made3, unused_p, unused_q, unused_r] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost3.Failed()) {
			IConsolePrint(CC_ERROR, "testspoj sklad: druha sberacka se nepodarila.");
			return true;
		}
		veh3 = made3;
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh3, 0, collect);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh3, 1, home_e);
	}

	/* Deliverer first, collector after; the collector's own hold keeps it in
	 * the shed until the rake is standing at the platform. */
	/* 'zaloz [N]': the feeder founds and grows a rake at the platform. Eight
	 * wagons are stored in the west depot; the feeder fetches two at a time
	 * (depot couple order, count 2), then "couple, founding a rake of up to N"
	 * at the platform, then "decouple all" there, and round again. A second
	 * engine, built stopped in the east depot, is the collector that takes
	 * the finished rake away when the scene releases it (testbrzda). */
	if (found_mode) {
		for (int i = 0; i < 8; i++) {
			auto [costs, sid, unused_m, unused_n, unused_o] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (costs.Failed()) {
				IConsolePrint(CC_ERROR, "testspoj zaloz: odlozeny vagon se nepodaril.");
				return true;
			}
		}
		DeleteVehicleOrders(Train::Get(veh1));
		/* Shed, platform, shed: the feeder leaves the west shed pushing its
		 * pair (a founding run puts the wagons at the door, see
		 * TryCoupleAtDepot()), adds them to the rake by their end, and drives
		 * back nose first for the next pair. The depot order is non-stop so
		 * the first run in from the east shed does not make a stop of the
		 * station on the way. */
		Order fetch;
		fetch.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlag::NonStop, OrderDepotActionFlags{});
		fetch.SetGoToCouple(true);
		fetch.SetCoupleCount(2);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 0, fetch);
		uint slot = 1;
		if (waypoint_mode) {
			Order via;
			via.MakeGoToWaypoint(GetStationIndex(TileXY(x0 + 14, y0)));
			via.SetNonStopType(OrderNonStopFlag::NonStop);
			Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, slot++, via);
		}
		Order found;
		found.MakeGoToStation(st_id);
		found.SetLoadType(OrderLoadType::NoLoad);
		found.SetUnloadType(OrderUnloadType::NoUnload);
		found.SetGoToCouple(true);
		found.SetFoundRake(true);
		found.SetCoupleCount(want_n);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, slot++, found);
		Order grow;
		grow.MakeGoToStation(st_id);
		grow.SetLoadType(OrderLoadType::NoLoad);
		grow.SetUnloadType(OrderUnloadType::NoUnload);
		grow.SetDecouple(true);
		grow.SetDecoupleCount(0);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, slot++, grow);

		auto [cost4, veh4, unused_s, unused_t, unused_u] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost4.Failed()) {
			IConsolePrint(CC_ERROR, "testspoj zaloz: sberacka hotove rady se nepodarila.");
			return true;
		}
		Order take;
		take.MakeGoToStation(st_id);
		take.SetLoadType(OrderLoadType::NoLoad);
		take.SetUnloadType(OrderUnloadType::NoUnload);
		take.SetGoToCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh4, 0, take);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh4, 1, home_e);

		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh1, false);
		_testspoj_active = true;
		IConsolePrint(CC_DEFAULT, "testspoj zaloz: zakladac=vlak {} (zaklada radu do {}), sberacka hotove rady=vlak {} (zabrzdena), v depu ({},{}) lezi 8 vozu, nastupiste {} ({}..{},{}).",
				Train::Get(veh1)->unitnumber, want_n, Train::Get(veh4)->unitnumber, x0, y0, st_id, x0 + 18, x0 + 21, y0);
		return true;
	}

	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh2, false);
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh1, false);
	if (veh3 != VehicleID::Invalid()) Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh3, false);

	_testspoj_active = true;

	IConsolePrint(CC_DEFAULT, "testspoj: scene ready. deliverer=vlak {}, collector=vlak {}, station={} at ({}..{},{}), west depot ({},{}), east depot ({},{}).",
			Train::Get(veh2)->unitnumber, Train::Get(veh1)->unitnumber, st_id, x0 + 18, x0 + 21, y0, x0, y0, x0 + LEN - 1, y0);
	return true;
}

/**
 * Build the loaded-platform scene: the testspoj strip plus an industry beside
 * the platform, so the station has real cargo waiting and a "full load" order
 * stages reservations and unloads on the train standing there.
 *
 * Every other scene runs on an empty map, so every coupling and decoupling
 * they exercise happens on trains whose cargo lists are clean -- and consist
 * surgery on a train standing in a loading stop with cargo work in flight
 * (reserved cargo pulled from the station, unloads staged) was never
 * exercised at all. The player hit it first: full load plus decouple on one
 * order put the game down on the cargo-bookkeeping assert in
 * CheckCargoCapacity() the moment the split ran.
 *
 * Plain: one train, engine and two wagons, one order "full load and decouple
 * everything" at the platform. Built with the brake on -- release it with
 * 'testbrzda 1' once the industry has had time to pile cargo at the station,
 * or the arrival finds an empty platform and reserves nothing.
 *
 * 'cekat': the coupling-side twin. A waiter loads at the platform on a full
 * load order -- reservations in flight for as long as cargo trickles in --
 * and a light engine is sent to couple onto it mid-load. Release the engine
 * with 'testbrzda 2'.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCargoScene(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the loaded-platform scene (industry + full load + decouple). Usage: 'testnaklad [cekat]'.");
		return true;
	}
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testnaklad: only in a running game.");
		return true;
	}
	bool wait_mode = argv.size() >= 2 && argv[1] == "cekat";
	bool waypoint_mode = argv.size() >= 2 && argv[1] == "smerovani";
	/* 'cil' is the plain scene with the decouple order carrying a station for
	 * the wagons it puts down to load for -- the thing a rake left standing
	 * has no way of knowing on its own. A second station is built up the line
	 * to be that answer; nothing ever drives to it, it is there to be named.
	 * Read the result with 'testcil'. */
	bool dest_mode = argv.size() >= 2 && argv[1] == "cil";

	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testnaklad: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);

	/* An engine, and a wagon whose cargo some fundable industry produces --
	 * the pair is what makes the platform a loaded one. On the default set
	 * this finds the coal wagon and the coal mine. */
	EngineID eid_loco = EngineID::Invalid();
	EngineID eid_wagon = EngineID::Invalid();
	IndustryType ind_type = NUM_INDUSTRYTYPES;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type != RailVehicleType::Wagon) {
			if (eid_loco == EngineID::Invalid()) eid_loco = e->index;
			continue;
		}
		if (eid_wagon != EngineID::Invalid()) continue;
		CargoType c = e->GetDefaultCargoType();
		if (!IsValidCargoType(c)) continue;
		for (IndustryType it = 0; it < NUM_INDUSTRYTYPES && eid_wagon == EngineID::Invalid(); it++) {
			const IndustrySpec *is = GetIndustrySpec(it);
			if (is == nullptr || !is->enabled) continue;
			/* Only an industry that can stand on an ordinary field next to the
			 * strip. The first pairing this loop ever found was the passenger
			 * wagon and the oil rig -- the one industry that only builds on
			 * water -- and the scene then had nowhere to put it. */
			if (is->behaviour.Any({IndustryBehaviour::BuiltOnWater, IndustryBehaviour::Town1200More,
					IndustryBehaviour::OnlyInTown, IndustryBehaviour::OnlyNearTown, IndustryBehaviour::Before1950, IndustryBehaviour::After1960})) {
				continue;
			}
			for (CargoType pc : is->produced_cargo) {
				if (pc == c) {
					eid_wagon = e->index;
					ind_type = it;
					break;
				}
			}
		}
	}
	if (eid_loco == EngineID::Invalid() || eid_wagon == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testnaklad: no engine, or no wagon whose cargo an industry produces.");
		return true;
	}

	/* The flattest clear run of tiles along the X axis; the industry needs
	 * room beside it, so ask for clear rows next to the strip too. */
	static const uint LEN = 40;
	TileIndex strip = INVALID_TILE;
	for (uint y = 8; y < Map::SizeY() - 12 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			TileIndex t = TileXY(x, y);
			bool ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
			int z = ok ? GetTileZ(t) : -1;
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					strip = TileXY(x - LEN + 1, y);
					break;
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testnaklad: no flat clear strip of {} tiles found.", LEN);
		return true;
	}
	uint x0 = TileX(strip);
	uint y0 = TileY(strip);
	_testmapa_area[0] = x0; _testmapa_area[1] = y0 > 1 ? y0 - 1 : 0;
	_testmapa_area[2] = x0 + LEN - 1; _testmapa_area[3] = y0 + 6;

	TileIndex depot_w = TileXY(x0, y0);
	TileIndex depot_e = TileXY(x0 + LEN - 1, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_w, RAILTYPE_RAIL, DiagDirection::SW).Failed() ||
			Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_e, RAILTYPE_RAIL, DiagDirection::NE).Failed()) {
		IConsolePrint(CC_ERROR, "testnaklad: depot failed.");
		return true;
	}
	if (Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, y0), TileXY(x0 + 1, y0), RAILTYPE_RAIL, Track::X, false, true).Failed()) {
		IConsolePrint(CC_ERROR, "testnaklad: track failed.");
		return true;
	}
	TileIndex st_tile = TileXY(x0 + 18, y0);
	if (Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, st_tile, RAILTYPE_RAIL, Axis::X, 1, 4, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed()) {
		IConsolePrint(CC_ERROR, "testnaklad: station failed.");
		return true;
	}
	for (uint sx : {x0 + 2, x0 + 15, x0 + 24, x0 + LEN - 3}) {
		if (Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(sx, y0), Track::X, SignalType::Path, SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, 0, 0).Failed()) {
			IConsolePrint(CC_ERROR, "testnaklad: signal at ({},{}) failed.", sx, y0);
			return true;
		}
	}
	UpdateSignalsInBuffer();

	StationID st_id = GetStationIndex(st_tile);
	DepotID dep_w = GetDepotIndex(depot_w);
	DepotID dep_e = GetDepotIndex(depot_e);

	/* The industry, beside the platform so the station's catchment covers it.
	 * Funded as the game's own deity would fund one -- that path may place it
	 * anywhere the layout fits, so several spots are offered until one takes. */
	bool ind_built = false;
	CommandCost last_refusal;
	{
		/* Whatever the ground beside the strip looks like, the industry needs
		 * a flat patch; the strip finder only ever guaranteed the strip row. */
		Command<Commands::LevelLand>::Do(DoCommandFlag::Execute, TileXY(x0 + 28, y0 + 8), TileXY(x0 + 10, y0 + 2), false, LevelMode::Level);

		AutoRestoreBackup deity(_current_company, OWNER_DEITY);
		/* Under the platform first: the station's catchment reaches only a few
		 * tiles, and an industry funded off to one side of it feeds nothing --
		 * the scene then measures an empty platform and proves nothing. */
		for (uint dy = 2; dy <= 8 && !ind_built; dy++) {
			for (uint dx = 6; dx <= 22 && !ind_built; dx++) {
				uint ix = x0 + 10 + dx, iy = y0 + dy;
				if (ix >= Map::SizeX() - 2 || iy >= Map::SizeY() - 2) continue;
				for (uint layout = 0; layout < (uint)GetIndustrySpec(ind_type)->layouts.size() && !ind_built; layout++) {
					last_refusal = Command<Commands::BuildIndustry>::Do(DoCommandFlags{}, TileXY(ix, iy), ind_type, layout, true, 0);
					/* A fixed number, not InteractiveRandom(): that generator is
					 * not part of the game's own state and gives a different
					 * answer every run, so the industry took in one run of the
					 * scene and would not take in the next. This was the standing
					 * wobble in the two cargo scenes -- they came out zero every
					 * few runs and read as a regression each time. */
					if (Command<Commands::BuildIndustry>::Do(DoCommandFlag::Execute, TileXY(ix, iy), ind_type, layout, true, 0).Succeeded()) {
						ind_built = true;
						IConsolePrint(CC_DEFAULT, "testnaklad: prumysl '{}' zalozen u ({},{}).", GetIndustrySpec(ind_type)->name, ix, iy);
					}
				}
			}
		}
	}
	if (!ind_built) {
		/* With the reason the last spot gave: this scene has come out empty
		 * every so often for a long time and guessing why has cost three
		 * readings of the battery. */
		IConsolePrint(CC_ERROR, "testnaklad: industry would not build anywhere beside the platform - {} (prumysl {}, typ {})",
				RefusalReason(last_refusal), GetString(GetIndustrySpec(ind_type)->name), ind_type);
		return true;
	}

	/* The loaded train: engine and two wagons of the industry's cargo. */
	auto build_train = [&](TileIndex depot) -> VehicleID {
		auto [cost, veh, unused_a, unused_b, unused_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) return VehicleID::Invalid();
		for (int i = 0; i < 2; i++) {
			auto [costw, wid, unused_d, unused_e, unused_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (costw.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wid, Train::Get(veh)->Last()->index, false).Failed()) return VehicleID::Invalid();
		}
		return veh;
	};

	VehicleID veh1 = build_train(depot_e);
	if (veh1 == VehicleID::Invalid()) {
		IConsolePrint(CC_ERROR, "testnaklad: train failed.");
		return true;
	}

	if (waypoint_mode) {
		/* The player's platform-number arrangement: a rake filling up at the
		 * platform, a station waypoint on the throat in front of it, and a
		 * collector whose orders lead through that waypoint to the couple
		 * order. The collector must stand short of the waypoint -- the track
		 * beyond ends in the occupied platform -- until the rake is full,
		 * then claim it, conclude the waypoint on the spot and drive in. */
		TileIndex wp_tile = TileXY(x0 + 23, y0);
		if (Command<Commands::BuildRailWaypoint>::Do(DoCommandFlag::Execute, wp_tile, Axis::X, 1, 1, STAT_CLASS_WAYP, 0, StationID::Invalid(), false, true).Failed()) {
			IConsolePrint(CC_ERROR, "testnaklad smerovani: waypoint failed.");
			return true;
		}
		UpdateSignalsInBuffer();
		StationID wp_id = GetStationIndex(wp_tile);

		/* The rake is dropped, not driven: a rake put down with a fill-up job
		 * is nobody's until the job is done (see TryDecoupleAtStation), which
		 * is exactly the spell the collector has to spend standing short of
		 * the waypoint. */
		Order drop;
		drop.MakeGoToStation(st_id);
		drop.SetLoadType(OrderLoadType::FullLoadAny);
		drop.SetDecouple(true);
		drop.SetDecoupleCount(0);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 0, drop);
		Order dropper_home;
		dropper_home.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 1, dropper_home);
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh1, false);

		auto [cost2, veh2, unused_g, unused_h, unused_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost2.Failed()) {
			IConsolePrint(CC_ERROR, "testnaklad smerovani: collector failed.");
			return true;
		}
		Order via;
		via.MakeGoToWaypoint(wp_id);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 0, via);
		Order collect;
		collect.MakeGoToStation(st_id);
		collect.SetLoadType(OrderLoadType::NoLoad);
		collect.SetUnloadType(OrderUnloadType::NoUnload);
		collect.SetGoToCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 1, collect);
		Order home_e;
		home_e.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 2, home_e);
		IConsolePrint(CC_DEFAULT, "testnaklad smerovani: rada (vlak {}) se plni u stanice {}, nadrazni smerovani {} na ({},{}); sberacku (vlak {}) pust brzdou.",
				Train::Get(veh1)->unitnumber, st_id, wp_id, x0 + 23, y0, Train::Get(veh2)->unitnumber);
	} else if (wait_mode) {
		/* The waiter: full load at the platform, waiting to be coupled while
		 * its load is still coming in. Started at once. */
		Order load_wait;
		load_wait.MakeGoToStation(st_id);
		load_wait.SetLoadType(OrderLoadType::FullLoadAny);
		load_wait.SetWaitForCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 0, load_wait);
		Order home_w;
		home_w.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 1, home_w);
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh1, false);

		/* The collector: a light engine sent to couple onto the loading
		 * waiter. Built braked; released mid-load by the script. */
		auto [cost2, veh2, unused_g, unused_h, unused_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost2.Failed()) {
			IConsolePrint(CC_ERROR, "testnaklad: collector failed.");
			return true;
		}
		Order collect;
		collect.MakeGoToStation(st_id);
		collect.SetLoadType(OrderLoadType::NoLoad);
		collect.SetUnloadType(OrderUnloadType::NoUnload);
		collect.SetGoToCouple(true);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 0, collect);
		Order home_e;
		home_e.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 1, home_e);
		IConsolePrint(CC_DEFAULT, "testnaklad cekat: vlak {} naklada a ceka na spojeni, vlak {} pustis brzdou, az bude nakladka v behu.",
				Train::Get(veh1)->unitnumber, Train::Get(veh2)->unitnumber);
	} else {
		/* Two passes on purpose. An industry only hands its production to a
		 * station once some train has tried to load that cargo there -- until
		 * then the platform stays empty however long the mine has stood
		 * beside it, and a decouple at an empty platform has no cargo work in
		 * flight to trip over. So the first visit is an ordinary stop that
		 * opens the tap, and the second is the player's crashing combination,
		 * letter for letter: full load and put every wagon down, arriving at
		 * a platform the mine has meanwhile piled cargo on. */
		Order prime;
		prime.MakeGoToStation(st_id);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 0, prime);
		Order via_w;
		via_w.MakeGoToDepot(DestinationID(dep_w), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlags{});
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 1, via_w);
		Order deliver;
		deliver.MakeGoToStation(st_id);
		deliver.SetLoadType(OrderLoadType::FullLoadAny);
		/* Transfer, so the second arrival has cargo work in flight the moment
		 * loading begins: the wagons come back carrying what the first pass
		 * loaded, and a transfer is staged on arrival whether or not the
		 * station accepts the cargo. A reservation would do the same job, but
		 * it only gets staged once the loading loop has had a tick, and a
		 * light test train can be standing still before that -- the player's
		 * heavier train brakes into the platform for several ticks and meets
		 * the staged work either way. */
		deliver.SetUnloadType(OrderUnloadType::Transfer);
		deliver.SetDecouple(true);
		deliver.SetDecoupleCount(0);
		/* 'cil': and where the wagons it leaves behind are to load for. A
		 * second station up the line, built only to have a name to give --
		 * no train ever goes there. What is being measured is whether the
		 * put-down rake ends up with an order naming it and whether the walk
		 * that hands out cargo finds it; 'testcil' says both. */
		if (dest_mode) {
			TileIndex far_tile = TileXY(x0 + 8, y0);
			if (Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, far_tile, RAILTYPE_RAIL, Axis::X, 1, 3, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed()) {
				IConsolePrint(CC_ERROR, "testnaklad cil: druha stanice se nepostavila.");
				return true;
			}
			UpdateSignalsInBuffer();
			deliver.SetDecoupleCargoDest(GetStationIndex(far_tile));
			IConsolePrint(CC_DEFAULT, "testnaklad cil: odpojene se maji nakladat pro stanici {} na ({},{}).",
					GetStationIndex(far_tile), x0 + 8, y0);
		}
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 2, deliver);
		Order home_e;
		home_e.MakeGoToDepot(DestinationID(dep_e), OrderDepotTypeFlag::PartOfOrders, OrderNonStopFlags{}, OrderDepotActionFlag::Halt);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh1, 3, home_e);
		IConsolePrint(CC_DEFAULT, "testnaklad: vlak {} stoji na brzde; pust ho 'testbrzda {}' - prvni zastavka otevre stanici {} naklad, druha je ta padava.",
				Train::Get(veh1)->unitnumber, Train::Get(veh1)->unitnumber, st_id);
	}

	_testspoj_active = true;
	return true;
}

/**
 * Clear every pause the game holds, including the error pause a save with
 * missing NewGRFs starts under -- which the ordinary 'unpause' refuses to
 * touch. Rig-only by nature: the headless runner has no dialog to click
 * away, and a player's save is the one thing worth running exactly as it
 * came, missing sets and all.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestUnpause(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	extern PauseModes _pause_mode;
	/* Which pause it was, and which mode the game is in: a save that will not
	 * run in the rig says nothing about why, and the difference between "paused"
	 * and "not a running game at all" is the whole answer. */
	IConsolePrint(CC_DEFAULT, "testpauza: pauzy {:#x}, rezim hry {}", _pause_mode.base(), to_underlying(_game_mode));
	_pause_mode = {};
	IConsolePrint(CC_DEFAULT, "testpauza: vsechny pauzy smazany.");
	return true;
}

/**
 * Print what cargo lies waiting at every station -- the rig's eyes for the
 * loaded-platform scene, where "no crash" only means something if the
 * platform really had cargo on it.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestStationCargo(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	for (const Station *st : Station::Iterate()) {
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			CargoType c = cs->Index();
			if (!st->goods[c].HasData()) continue;
			uint waiting = st->goods[c].GetData().cargo.TotalCount();
			if (waiting == 0) continue;
			IConsolePrint(CC_DEFAULT, "stanice {}: naklad {} ceka {} jednotek", st->index, c, waiting);
		}
	}
	return true;
}

/**
 * Change the cargo filter on a collecting order while the train is already
 * working it. Stages the one thing a player does when a train stands waiting
 * for wagons it will never match: put the filter right and expect it to take.
 * Usage: testfiltr [cargo index] -- no argument clears the filter.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCoupleFilter(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change what a collect order accepts. Usage: 'testfiltr' to clear the cargo filter, 'testfiltr <cargo>', or 'testfiltr plne|prazdne|jakekoliv|radsiplne|radsiprazdne'.");
		return true;
	}

	/* A word instead of a number sets the other half of the filter: how full
	 * the wagons have to be. The two are asked together -- with a cargo named,
	 * the fullness question is asked only of the wagons carrying it -- so the
	 * rig has to be able to set both. */
	/* 'zkouska' asks the filter its own question of every waiting rake, instead
	 * of playing a coupling out and reading the answer from where the trains
	 * end up. Timing, platform holds and who claimed what all drop out of it;
	 * what is left is the filter. */
	if (argv.size() >= 2 && argv[1] == "zkouska") {
		/* Asked of a train that has not chosen yet, when there is one. A train
		 * that has already spoken for a rake is handed that rake and does not
		 * look at the offer at all -- rightly, it is its own -- so asking it
		 * which of several it prefers answers a question it is not being
		 * asked. Two passes: first the undecided, then anybody. */
		for (int pass = 0; pass < 2; pass++) {
		for (const Train *t : Train::Iterate()) {
			if (t->First() != t || !t->IsFrontEngine()) continue;
			if (pass == 0 && t->couple_target != VehicleID::Invalid()) continue;
			for (VehicleOrderID i = 0; i < t->GetNumOrders(); i++) {
				const Order *o = t->GetOrder(i);
				if (o == nullptr || !o->ShouldGoToCouple()) continue;
				bool found = CoupleOrderWouldFindSomething(t, *o);
				/* With how many rakes are standing there to be found at all:
				 * "nothing found" means one thing when a rake is waiting and
				 * the filter turned it down, and quite another when the rake
				 * has already been collected and there is nothing left to
				 * turn down. Without the count the two read the same. */
				uint waiting = 0;
				for (const Train *r : Train::Iterate()) {
					if (r->First() == r && r->IsFreeWagon()) waiting++;
				}
				/* The named wagon is said as well: the cargo filter and the
				 * wagon are set together now, and "every cargo" lets go of
				 * both -- which is a thing no counter can show and this line
				 * can. */
				/* Which of the waiting rakes it would take, and how full each
				 * of them is. "Prefer the fullest" is a question of which one
				 * is picked out of several, and no counter can see that: the
				 * train ends up coupled either way. */
				const Train *would = CoupleOrderWouldTake(t, *o);
				for (const Train *r : Train::Iterate()) {
					if (r->First() != r || !r->IsFreeWagon() || r->owner != t->owner) continue;
					IConsolePrint(CC_DEFAULT, "testfiltr: rada {} naplnena na {}%, zamluvena {}{}", r->index.base(),
							CoupleRakeFullness(r, *o),
							r->couple_claim == VehicleID::Invalid() ? "ne" : fmt::format("vlakem {}", r->couple_claim.base()),
							r == would ? " <- tuhle by vzal" : "");
				}
				IConsolePrint(CC_DEFAULT, "testfiltr: vlak {} rozkaz {} (naklad {}, naplneni {}, vagon {}, nakup {}) - {}, cekajicich rad {}",
						t->unitnumber, i, (int)(int8_t)o->GetCoupleCargo(), to_underlying(o->GetCoupleLoad()),
						(int)o->GetCoupleBuyEngine().base(), o->ShouldBuyWagons() ? "ano" : "ne",
						found ? "NASEL BY radu" : "nenasel by nic", waiting);
				return true;
			}
		}
		}
		IConsolePrint(CC_ERROR, "testfiltr: zadny vlak nema rozkaz jet se spojit.");
		return true;
	}

	if (argv.size() >= 2 && (argv[1] == "plne" || argv[1] == "prazdne" || argv[1] == "jakekoliv" ||
			argv[1] == "radsiplne" || argv[1] == "radsiprazdne")) {
		OrderCoupleLoad want = argv[1] == "plne" ? OrderCoupleLoad::Full :
				(argv[1] == "prazdne" ? OrderCoupleLoad::Empty :
				(argv[1] == "radsiplne" ? OrderCoupleLoad::AnyFullFirst :
				(argv[1] == "radsiprazdne" ? OrderCoupleLoad::AnyEmptyFirst : OrderCoupleLoad::Any)));
		for (const Train *t : Train::Iterate()) {
			if (t->First() != t || !t->IsFrontEngine()) continue;
			for (VehicleOrderID i = 0; i < t->GetNumOrders(); i++) {
				const Order *o = t->GetOrder(i);
				if (o == nullptr || !o->ShouldGoToCouple()) continue;
				AutoRestoreBackup cur_company(_current_company, t->owner);
				CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, i, MOF_COUPLE_LOAD, to_underlying(want));
				IConsolePrint(r.Failed() ? CC_ERROR : CC_DEFAULT, "testfiltr: vlak {} rozkaz {} - filtr naplneni na {} {}.",
						t->unitnumber, i, argv[1], r.Failed() ? "SELHAL" : "nastaven");
				return true;
			}
		}
		IConsolePrint(CC_ERROR, "testfiltr: zadny vlak nema rozkaz jet se spojit.");
		return true;
	}

	uint32_t cargo = (uint32_t)INVALID_CARGO;
	if (argv.size() >= 2) {
		auto n = ParseInteger(argv[1]);
		if (!n.has_value()) return false;
		cargo = (uint32_t)*n;
	}

	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFrontEngine()) continue;
		for (VehicleOrderID i = 0; i < t->GetNumOrders(); i++) {
			const Order *o = t->GetOrder(i);
			if (o == nullptr || !o->ShouldGoToCouple()) continue;
			/* Commands read whichever company happens to be current, and a
			 * console command is nobody's. Without this the order change was
			 * refused on ownership and quietly did nothing, which for a while
			 * looked exactly like the bug being tested for. */
			AutoRestoreBackup cur_company(_current_company, t->owner);
			CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, i, MOF_COUPLE_CARGO, cargo);
			IConsolePrint(r.Failed() ? CC_ERROR : CC_DEFAULT, "testfiltr: vlak {} rozkaz {} - filtr nakladu na {} {}.",
					t->unitnumber, i, (int)(int32_t)cargo, r.Failed() ? "SELHAL" : "nastaven");
			return true;
		}
	}
	IConsolePrint(CC_ERROR, "testfiltr: zadny vlak nema rozkaz jet se spojit.");
	return true;
}

/**
 * Print where every train stands right now, for reading a headless run.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCoupleState(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		if (!t->IsFrontEngine() && !t->IsFreeWagon()) continue;
		IConsolePrint(CC_DEFAULT, "vlak {}: ({},{}) rychlost {} couva {} rozkaz {} [c.{} kam ({},{})] vozu {} zasekly {} cil {} narok {} zpozdeni {} stav[{}{}{}{}]",
				t->unitnumber, TileX(t->tile), TileY(t->tile), t->cur_speed,
				t->vehicle_flags.Test(VehicleFlag::DrivingBackwards) ? "ano" : "ne",
				to_underlying(t->current_order.GetType()),
				t->cur_real_order_index, TileX(t->dest_tile), TileY(t->dest_tile),
				CountVehiclesInChain(t),
				t->flags.Test(VehicleRailFlag::Stuck) ? "ano" : "ne",
				t->couple_target == VehicleID::Invalid() ? -1 : (int)t->couple_target.base(),
				t->couple_claim == VehicleID::Invalid() ? -1 : (int)t->couple_claim.base(),
				t->lateness_counter,
				t->vehstatus.Test(VehState::Stopped) ? 'S' : '-',
				t->flags.Test(VehicleRailFlag::LeavingStation) ? 'L' : '-',
				t->flags.Test(VehicleRailFlag::Reversing) ? 'R' : '-',
				t->vehicle_flags.Test(VehicleFlag::LoadingFinished) ? 'F' : '-');
	}

	/* Road vehicles too, since one of them may be riding a train or waiting
	 * for one (road_on_rail.h): where it is, what it is doing, and whose
	 * wagon it is on. */
	for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (!rv->IsFrontEngine()) continue;
		const Train *wagon = rv->IsCarried() ? Train::GetIfValid(rv->carried_by) : nullptr;
		IConsolePrint(CC_DEFAULT, "auto {}: ({},{}) rychlost {} rozkaz {} [c.{} kam ({},{})] stav {:#x} snimek {} posledni stanice {} {}{}",
				rv->unitnumber, TileX(rv->tile), TileY(rv->tile), rv->cur_speed,
				to_underlying(rv->current_order.GetType()), rv->cur_real_order_index, TileX(rv->dest_tile), TileY(rv->dest_tile),
				rv->state, rv->frame, rv->last_station_visited,
				rv->IsCarried() ? fmt::format("VEZE SE na vlaku {}", wagon != nullptr ? (int)wagon->First()->unitnumber : -1) : (IsWaitingToBoardTrain(rv) ? "CEKA NA VLAK" : ""),
				rv->vehstatus.Test(VehState::Stopped) ? " [S]" : "");
	}

	/* The rescue side of the same picture, on the same command. Working it out
	 * meant reading three different outputs and joining them up by hand: who is
	 * on call, who is going for whom, what is holding whoever is not moving,
	 * and which broken trains are still waiting rather than about to give up.
	 * All of it is known; none of it was said. */
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFrontEngine()) continue;

		if (t->vehicle_flags.Test(VehicleFlag::RescueEngine)) {
			/* One line per RescueHold, in its order: a line missing moved every
			 * later one up by one and the last off the end of the table, which
			 * brought the rig down on a format error. */
			static const char * const drzi[] = {"nic - jede nebo vyjizdi", "ma zatazenou brzdu", "ma vlastni rozkazy",
					"nikdo necaka", "porucha se nepocita", "uz pro ni jede jina", "bliz stoji jina",
					"vyjezd z depa je blokovany", "nenajde cestu k poruse", "nema kam s poruchou", "spojeni se porad odmita, vzdala to"};
			static_assert(std::size(drzi) == to_underlying(RescueHold::CannotCouple) + 1);
			const Train *cil = Train::GetIfValid(t->rescue_target);
			IConsolePrint(CC_DEFAULT, "odtahovka {}{}: {} cil {} {} - drzi ji: {}",
					t->unitnumber, t->IsWrecked() ? " (VRAK)" : (t->breakdown_ctr == 1 ? " (porouchana)" : ""),
					t->IsInDepot() ? "v depu" : fmt::format("na ({},{})", TileX(t->tile), TileY(t->tile)),
					cil == nullptr ? "zadny" : fmt::format("vlak {}", cil->unitnumber),
					cil == nullptr ? "" : (IsRescueTargetAttached(t) ? "(uz ji veze)" : "(jede pro ni)"),
					drzi[to_underlying(t->rescue_hold)]);
			continue;
		}

		if (t->breakdown_ctr == 1 || t->IsWrecked()) {
			const Train *kdo = Train::GetIfValid(t->couple_claim);
			IConsolePrint(CC_DEFAULT, "porucha {}: na ({},{}) {} - {}, jede pro ni {} (lhuta do {}, dnes {})",
					t->unitnumber, TileX(t->tile), TileY(t->tile),
					t->IsWrecked() ? "vrak" : "porouchany",
					IsWaitingToBeRescued(t) ? "ceka na odtah" :
							t->flags.Test(VehicleRailFlag::RescueGivenUp) ? "uz na odtah neceka (odtah to vzdal)" : "uz na odtah neceka (vyprsela lhuta)",
					kdo == nullptr ? "nikdo" : fmt::format("odtahovka {}", kdo->unitnumber),
					t->rescue_deadline.base(), TimerGameEconomy::date.base());
		}
	}
	return true;
}

/**
 * Dump every train's order list, so a loaded save can be read like a map:
 * which engine has which preset orders, and where.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestOrders(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		if (!t->IsFrontEngine() && !t->IsFreeWagon()) continue;
		/* Two counts, because they are two different numbers and the orders
		 * speak the second one: vehicles in the chain, and units behind the
		 * engine -- which is what a couple, a decouple and now a conditional
		 * order all mean by "wagons". */
		IConsolePrint(CC_DEFAULT, "vlak {}: ({},{}) vozu {} vagonu {} delka {} {} rozkazu {}",
				t->unitnumber, TileX(t->tile), TileY(t->tile), CountVehiclesInChain(t), WagonUnitsBehindEngine(t),
				CeilDiv(t->gcache.cached_total_length, TILE_SIZE),
				t->IsInDepot() ? "v depu" : (t->vehstatus.Test(VehState::Stopped) ? "stopnut" : "venku"),
				t->GetNumOrders());
		int n = 0;
		for (const Order &o : t->Orders()) {
			std::string extra;
			if (o.ShouldGoToCouple()) {
				static const char *const NAKLAD[] = {"jakekoli", "prazdne", "plne", "plne napred", "prazdne napred"};
				extra += fmt::format(" SPOJIT:{}", to_underlying(o.GetCoupleLoad()) < std::size(NAKLAD) ? NAKLAD[to_underlying(o.GetCoupleLoad())] : "?");
				if (IsValidCargoType(o.GetCoupleCargo())) extra += fmt::format(" naklad {}", GetString(CargoSpec::Get(o.GetCoupleCargo())->name));
				if (o.GetCoupleCount() != 0) extra += fmt::format(" pocet {}", o.GetCoupleCount());
			}
			if (o.ShouldFoundRake()) extra += o.GetCoupleCount() != 0 ? fmt::format(" ZALOZIT:do {}", o.GetCoupleCount()) : " ZALOZIT";
			if (o.ShouldHonk()) extra += " HOUKAT";
			if (o.ShouldDepartAutomatically()) extra += " AUTO";
			if (o.IsCoupleCountMinimum()) extra += " MIN";
			if (o.IsCoupleCountMaximum()) extra += " MAX";
			if (o.ShouldWaitForCouple()) extra += " CEKAT";
			if (o.ShouldDecoupleOnDeparture()) extra += o.ShouldDecoupleWholeTrain() ? " ODPOJIT:cely vlak" : (o.GetDecoupleCount() == 0 ? " ODPOJIT:vse" : fmt::format(" ODPOJIT:nechat {}", o.GetDecoupleCount()));
			if (o.ShouldReverseOutOfStation()) extra += " REVERZ";
			if (o.IsType(OT_GOTO_DEPOT) && o.ShouldTurnAroundInDepot()) extra += " OTOC-DEPO";
			if (o.IsType(OT_CONDITIONAL)) {
				extra += fmt::format(" PODMINKA:promenna {} srovnani {} hodnota {} skok na {}",
						to_underlying(o.GetConditionVariable()), to_underlying(o.GetConditionComparator()),
						o.GetConditionValue(), o.GetConditionSkipToOrder());
			}
			if (o.IsType(OT_GOTO_STATION)) extra += fmt::format(" nakladat {} vykladat {}", to_underlying(o.GetLoadType()), to_underlying(o.GetUnloadType()));
			IConsolePrint(CC_DEFAULT, "  [{}] typ {} cil {}{}", n++, to_underlying(o.GetType()), o.GetDestination().base(), extra);
		}
	}
	return true;
}

/**
 * Save the console backlog to a text file beside the saved games, so an
 * incident can be reported whole instead of screenshotting the console a
 * window at a time. Usage: vlaksav [<name>]
 *
 * Beside the saves and not in the personal directory above them, because that
 * is the one folder the player already has open when something goes wrong: he
 * is reaching for the save anyway, and a report is a save plus this. Hunting a
 * loose file out of the folder above, on a phone, is a separate errand nobody
 * should have to run twice.
 * @copydoc IConsoleCmdProc
 */
static bool ConSaveConsoleLog(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Save the console backlog to a file. Usage: 'vlaksav [<name>]' (default vlaksav.txt).");
		return true;
	}
	std::string name = argv.size() >= 2 ? fmt::format("{}.txt", argv[1]) : "vlaksav.txt";
	std::string path = FioFindDirectory(Subdirectory::Save) + name;
	int lines = IConsoleSaveBacklog(path);
	if (lines < 0) {
		IConsolePrint(CC_ERROR, "vlaksav: nejde zapsat '{}'.", path);
	} else {
		IConsolePrint(CC_DEFAULT, "vlaksav: {} radku ulozeno do '{}'.", lines, path);
	}
	return true;
}

/**
 * Dump rail layout of a map rectangle: tracks, signals (with direction and
 * one-way-ness), reservations. Test-rig eyes for a headless run.
 * Usage: testmapa <x1> <y1> <x2> <y2>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestMap(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	std::optional<uint64_t> px1, py1, px2, py2;
	if (argv.size() < 5) {
		if (_testmapa_area[2] == 0) {
			IConsolePrint(CC_HELP, "Usage: 'testmapa <x1> <y1> <x2> <y2>' (bez argumentu: okoli posledni zkusebni sceny).");
			return true;
		}
		px1 = _testmapa_area[0]; py1 = _testmapa_area[1]; px2 = _testmapa_area[2]; py2 = _testmapa_area[3];
	} else {
		px1 = ParseInteger(argv[1]); py1 = ParseInteger(argv[2]);
		px2 = ParseInteger(argv[3]); py2 = ParseInteger(argv[4]);
		if (!px1.has_value() || !py1.has_value() || !px2.has_value() || !py2.has_value()) return false;
	}

	IConsolePrint(CC_DEFAULT, "testmapa: otaceni u navesti (reverse_at_signals) = {}", _settings_game.pf.reverse_at_signals ? "zapnuto" : "vypnuto");
	for (uint y = *py1; y <= (uint)*py2; y++) {
		for (uint x = *px1; x <= (uint)*px2; x++) {
			TileIndex tile = TileXY(x, y);
			std::string desc;
			TrackBits tracks{};
			if (IsPlainRailTile(tile)) {
				tracks = GetTrackBits(tile);
				desc = "kolej";
			} else if (IsRailDepotTile(tile)) {
				desc = fmt::format("depo {} (vrata {})", GetDepotIndex(tile).base(), to_underlying(GetRailDepotDirection(tile)));
			} else if (IsRailWaypointTile(tile)) {
				tracks = TrackBits{GetRailStationTrack(tile)};
				const Waypoint *wp = Waypoint::GetByTile(tile);
				desc = fmt::format("smerovani {}{} '{}'", wp->index.base(),
						HasBit(wp->waypoint_flags, WPF_STATION_SEARCH) ? " (nadrazni)" : "", GetString(STR_WAYPOINT_NAME, wp->index));
			} else if (IsRailStationTile(tile)) {
				tracks = TrackBits{GetRailStationTrack(tile)};
				desc = fmt::format("stanice {}", GetStationIndex(tile).base());
			} else if (IsLevelCrossingTile(tile)) {
				tracks = TrackBits{GetCrossingRailTrack(tile)};
				desc = "prejezd";
			} else if (IsTileType(tile, TileType::TunnelBridge) && GetTunnelBridgeTransportType(tile) == TransportType::Rail) {
				TileIndex other = GetOtherTunnelBridgeEnd(tile);
				tracks = TrackBits{DiagDirToDiagTrack(GetTunnelBridgeDirection(tile))};
				desc = fmt::format("{} usti smer {} druhy konec ({},{}) navestidla {}",
						IsTunnel(tile) ? "tunel" : "most", to_underlying(GetTunnelBridgeDirection(tile)),
						TileX(other), TileY(other),
						IsTunnelBridgeSignalled(tile) ? "ano" : "ne");
				if (IsTunnelBridgeSignalled(tile)) {
					desc += fmt::format(" {} typ {} {}",
							GetTunnelBridgeSignalState(tile, TunnelBridgeSignal::Entry) == SignalState::Red ? "cervena" : "zelena",
							to_underlying(GetTunnelBridgeSignalType(tile)),
							GetTunnelBridgeSignalVariant(tile) == SignalVariant::Semaphore ? "mechanicke" : "svetelne");
				}
			} else {
				continue;
			}
			std::string sigs;
			if (IsPlainRailTile(tile) && HasSignals(tile)) {
				for (Trackdir td : TRACKDIR_BIT_MASK) {
					if (!HasTrack(tile, TrackdirToTrack(td))) continue;
					if (!HasSignalOnTrackdir(tile, td)) continue;
					sigs += fmt::format(" navest[td {}]: typ {} stav {}{}",
							to_underlying(td),
							to_underlying(GetSignalType(tile, TrackdirToTrack(td))),
							GetSignalStateByTrackdir(tile, td) == SignalState::Green ? "zelena" : "cervena",
							HasSignalOnTrackdir(tile, ReverseTrackdir(td)) ? "" : " (jednosmerna)");
				}
			}
			IConsolePrint(CC_DEFAULT, "({},{}): {} koleje {:#x} rez {:#x}{}",
					x, y, desc, tracks.base(), GetReservedTrackbits(tile).base(), sigs);
		}
	}
	return true;
}

/**
 * Start every train standing in the depot on the given tile.
 * Usage: teststartdepo <x> <y>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestStartDepot(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Usage: 'teststartdepo <x> <y>'.");
		return true;
	}
	auto px = ParseInteger(argv[1]);
	auto py = ParseInteger(argv[2]);
	if (!px.has_value() || !py.has_value()) return false;
	TileIndex tile = TileXY(*px, *py);
	/* A save carried over from the player's machine loads paused, and the
	 * polite unpause can refuse; the test has to run, so the brake comes off
	 * directly. Single player, headless -- nobody else is affected. */
	_pause_mode = {};
	int started = 0;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFrontEngine()) continue;
		if (!t->IsInDepot() || t->tile != tile) continue;
		if (!t->vehstatus.Test(VehState::Stopped)) continue;
		/* "Release the trains" means the trains: light engines parked in the
		 * same shed are the collectors, and which of them runs -- and how many
		 * copies -- is the cloning step's decision, not this one's. */
		if (CountVehiclesInChain(t) < 2) continue;
		if (Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, t->index, false).Succeeded()) started++;
	}
	IConsolePrint(CC_DEFAULT, "teststartdepo: started {} trains at ({},{}).", started, *px, *py);
	return true;
}

/**
 * Clone the train with the given unit number, several times, in its depot,
 * and start the clones. Usage: testklon <unit> <count> [reverz]
 * With 'reverz', every station order of each clone (and of the original)
 * gets the reverse-out flag first.
 * @copydoc IConsoleCmdProc
 */
/** The clones the last testklon made, in the order it made them. A scene lets
 * them out by that order rather than by unit number: the game hands out the
 * lowest free number, so which numbers the clones get depends on which trains
 * the save happens to be missing, and a scene written to numbers goes quiet
 * the day the save changes. */
static std::vector<VehicleID> _testklon_made;

static void DoTestClone(uint unit, uint count, bool reverz, bool stoj = false)
{
	_pause_mode = {};
	_testklon_made.clear();

	Train *original = nullptr;
	for (Train *t : Train::Iterate()) {
		if (t->First() == t && t->IsFrontEngine() && t->unitnumber == unit) {
			original = t;
			break;
		}
	}
	if (original == nullptr || !original->IsInDepot()) {
		IConsolePrint(CC_ERROR, "testklon: train {} not found in a depot.", unit);
		return;
	}

	/* Fired from a timer, not from the console: whoever ran last is the
	 * "current" company then, and the commands below check ownership. */
	AutoRestoreBackup cur_company(_current_company, original->owner);

	auto set_reverz = [](const Train *t) {
		int n = 0;
		for (const Order &o : t->Orders()) {
			if (o.IsType(OT_GOTO_STATION) && !o.ShouldReverseOutOfStation() &&
					o.GetDecoupleCount() == 0 && !o.ShouldWaitForCouple()) {
				Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)n, MOF_REVERSE_OUT, 1);
			}
			n++;
		}
	};
	if (reverz) set_reverz(original);

	for (int i = 0; i < (int)count; i++) {
		auto [cost, cloned] = Command<Commands::CloneVehicle>::Do(DoCommandFlag::Execute, original->tile, original->index, false);
		if (cost.Failed()) {
			IConsolePrint(CC_ERROR, "testklon: clone {} failed: {}", i + 1, GetString(cost.GetErrorMessage()));
			return;
		}
		/* "stoj": the clones are built and left standing in the shed, for a
		 * scene that lets them out one at a time with testpustklon later on
		 * instead of all at once. */
		if (!stoj) Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, cloned, false);
		_testklon_made.push_back(cloned);
		IConsolePrint(CC_DEFAULT, "testklon: clone vlak {} {}.", Train::Get(cloned)->unitnumber, stoj ? "built, standing" : "started");
	}
	if (stoj) {
		_testspoj_active = true;
		return;
	}
	/* The start command is a toggle. A scene that lets the original off the
	 * brake with testbrzda in the same breath used to have it toggled straight
	 * back to a stop here, so every "clone it three times" scene ran three
	 * collectors and left the original standing in its shed -- and its count
	 * of couplings was one short for as long as those scenes have existed. */
	if (original->vehstatus.Test(VehState::Stopped)) {
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, original->index, false);
		IConsolePrint(CC_DEFAULT, "testklon: original vlak {} started{}.", original->unitnumber, reverz ? " (reverz on station orders)" : "");
	} else {
		IConsolePrint(CC_DEFAULT, "testklon: original vlak {} already running, left alone.", original->unitnumber);
	}
	_testspoj_active = true;
}

/** A clone step ordered for later, the way a hand on a phone paces it;
 * counted down by the heartbeat timer below. */
static int _testklon_delay = 0;
static uint _testklon_unit = 0;
static uint _testklon_count = 0;
static bool _testklon_reverz = false;

static bool ConTestClone(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Usage: 'testklon <unit> <count> [reverz] [stoj] [za <ticks>]'. 'stoj' builds the clones stopped in the shed, for a later testbrzda.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto pcount = ParseInteger(argv[2]);
	if (!punit.has_value() || !pcount.has_value()) return false;
	bool reverz = false;
	bool stoj = false;
	int delay = 0;
	for (size_t i = 3; i < argv.size(); i++) {
		if (argv[i] == "reverz") reverz = true;
		if (argv[i] == "stoj") stoj = true;
		if (argv[i] == "za" && i + 1 < argv.size()) {
			auto pdelay = ParseInteger(argv[i + 1]);
			if (pdelay.has_value()) delay = (int)*pdelay;
		}
	}
	if (delay > 0) {
		_testklon_unit = (uint)*punit;
		_testklon_count = (uint)*pcount;
		_testklon_reverz = reverz;
		_testklon_delay = delay;
		_testspoj_active = true;
		IConsolePrint(CC_DEFAULT, "testklon: vlak {} x{} za {} tiku.", *punit, *pcount, delay);
		return true;
	}
	DoTestClone((uint)*punit, (uint)*pcount, reverz, stoj);
	return true;
}

/**
 * Let one of the clones the last testklon made off its brake, by the order it
 * was made in. Usage: testpustklon <n> (1 = the first clone)
 * @copydoc IConsoleCmdProc
 */
static bool ConTestReleaseClone(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Let a clone made by the last 'testklon ... stoj' go. Usage: 'testpustklon <n>' (1 = first).");
		return true;
	}
	auto pn = ParseInteger(argv[1]);
	if (!pn.has_value()) return false;
	uint n = (uint)*pn;
	if (n < 1 || n > _testklon_made.size()) {
		IConsolePrint(CC_ERROR, "testpustklon: klon c.{} neexistuje (testklon jich udelal {}).", n, _testklon_made.size());
		return true;
	}
	Train *t = Train::GetIfValid(_testklon_made[n - 1]);
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testpustklon: klon c.{} uz neexistuje.", n);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, t->owner);
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, t->index, false);
	IConsolePrint(CC_DEFAULT, "testpustklon: klon c.{} je vlak {}, puzen.", n, t->unitnumber);
	return true;
}

/**
 * Build the junction-rescue scene: a casualty that breaks down bent across a
 * set of points, with both of its track pieces unconnectable from the rescue
 * engine's approach -- the exact shape in which coupling used to assert.
 *
 * Layout on the flattest strip: depots at both ends of a main line, a branch
 * curving south off the middle of it (a LOWER curve into a Y stub ending in a
 * small platform). The casualty (engine + one wagon) sets off from the east
 * depot for the branch station and is broken down by the tick watcher the
 * moment its front turns onto the branch, leaving it lying across the curve.
 * The rescue engine waits on call in the west depot; its approach enters the
 * junction tile over the edge the curve does not touch, so no driving can
 * bring it nose-to-end -- the straightening has to do it.
 * Usage: 'testodtah'.
 * @copydoc IConsoleCmdProc
 */
static VehicleID _testodtah_casualty = VehicleID::Invalid();
static bool _testodtah_sell = false; ///< The armed tile sells the train for scrap instead of breaking it down.
static bool _testodtah_sell_broken = false; ///< Try to sell the train shortly after it breaks down, which the scrapyard has to refuse.
static int _testodtah_sell_after = 0; ///< Ticks left until that attempt. Zero while nothing is pending.
static TileIndex _testodtah_break_tile = INVALID_TILE;
static TileIndex _testodtah_cross_tile = INVALID_TILE;
static TileIndex _testodtah_depot_w = INVALID_TILE;
static VehicleID _testokruh_rescue = VehicleID::Invalid();
static uint _testokruh_detour_row = 0;
static bool _testokruh_detour_seen = false;

/**
 * Print every reserved tile in the current test scene's rectangle.
 *
 * A rescue engine that will not leave its shed says only "no route", and from
 * the outside that is indistinguishable from a dozen different causes. Track
 * held by somebody else is the one that can be looked at, so it is looked at:
 * this walks the scene and says who is holding what. Usage: 'testrez'.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestReservations(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List reserved tiles. Usage: 'testrez' (test scene) or 'testrez <x0> <y0> <x1> <y1>'.");
		return true;
	}
	/* A scene built here leaves its rectangle behind; a player's own saved game
	 * does not, so the corners can be given instead. */
	uint x0 = _testmapa_area[0], y0 = _testmapa_area[1], x1 = _testmapa_area[2], y1 = _testmapa_area[3];
	if (argv.size() >= 5) {
		auto a0 = ParseInteger(argv[1]), b0 = ParseInteger(argv[2]), a1 = ParseInteger(argv[3]), b1 = ParseInteger(argv[4]);
		if (!a0 || !b0 || !a1 || !b1) return false;
		x0 = *a0; y0 = *b0; x1 = *a1; y1 = *b1;
	}

	uint n = 0;
	for (uint y = y0; y <= y1 && y < Map::SizeY(); y++) {
		for (uint x = x0; x <= x1 && x < Map::SizeX(); x++) {
			TileIndex t = TileXY(x, y);
			if (!IsTileType(t, TileType::Railway) && !IsRailStationTile(t)) continue;
			TrackBits res = GetReservedTrackbits(t);
			if (res.None()) continue;
			const Train *who = GetTrainForReservation(t, FindFirstTrack(res));
			IConsolePrint(CC_DEFAULT, "testrez: ({},{}) drzi {:#x} - vlak {}.", x, y, res.base(),
					who != nullptr ? fmt::format("{}", who->unitnumber) : "nikdo");
			n++;
		}
	}
	IConsolePrint(CC_DEFAULT, "testrez: celkem {} zamluvenych policek.", n);
	return true;
}

/**
 * List the signals on a stretch of line, with their kind and which way they
 * face. Usage: 'testnavesti <x0> <y0> <x1> <y1>'
 *
 * A reservation that reaches past a signal is only as safe as the signal is,
 * and the two-way ones are not safe in the direction nobody is looking. What
 * stands where was until now only readable by opening the map, which a
 * headless run cannot do.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSignals(std::span<std::string_view> argv)
{
	if (argv.size() < 5) {
		IConsolePrint(CC_HELP, "List signals and which way they face. Usage: 'testnavesti <x0> <y0> <x1> <y1>'.");
		return true;
	}
	auto a0 = ParseInteger(argv[1]), b0 = ParseInteger(argv[2]), a1 = ParseInteger(argv[3]), b1 = ParseInteger(argv[4]);
	if (!a0 || !b0 || !a1 || !b1) return false;

	static const char *kind[] = { "blok", "predvest-vjezd", "predvest-vyjezd", "predvest-kombi", "cestne", "cestne-jednosmer" };
	uint n = 0;
	for (uint y = *b0; y <= *b1 && y < Map::SizeY(); y++) {
		for (uint x = *a0; x <= *a1 && x < Map::SizeX(); x++) {
			TileIndex tile = TileXY(x, y);
			if (!IsTileType(tile, TileType::Railway) || GetRailTileType(tile) != RailTileType::Signals) continue;
			for (Track tr : GetTrackBits(tile)) {
				if (!HasSignalOnTrack(tile, tr)) continue;
				/* Both trackdirs of the track carry a signal on a two-way one;
				 * only one on a one-way. That is the whole difference and it
				 * is what decides whether a path may be booked through it from
				 * the other side. */
				Trackdir td = TrackToTrackdir(tr);
				bool fwd = HasSignalOnTrackdir(tile, td);
				bool rev = HasSignalOnTrackdir(tile, ReverseTrackdir(td));
				SignalType st = GetSignalType(tile, tr);
				/* What the signal is showing, each way it faces: red, green,
				 * or the warning aspect a green shows when the road booked
				 * through it ends at or before the next signal (the "yellow"
				 * a driver brakes to; see IsPathSignalWarning()). */
				auto aspect = [&](Trackdir d) -> const char * {
					if (!HasSignalOnTrackdir(tile, d)) return "";
					if (GetSignalStateByTrackdir(tile, d) == SignalState::Red) return " cervena";
					return IsPathSignalWarning(tile, d) ? " ZLUTA" : " zelena";
				};
				IConsolePrint(CC_DEFAULT, "testnavesti: ({},{}) kolej {} - {} {}, {}{}{}{}", x, y, (int)tr,
						to_underlying(st) < lengthof(kind) ? kind[to_underlying(st)] : "?",
						(fwd && rev) ? "OBOUSMERNE" : "jednosmerne",
						fwd ? "tam" : "", fwd ? aspect(td) : "",
						rev ? " zpet" : "", rev ? aspect(ReverseTrackdir(td)) : "");
				n++;
			}
		}
	}
	/* Whether the aspect has anything to draw with. It is the green signal
	 * repainted, so what has to be there is the recolour map; the pictures
	 * are whatever drew the green. */
	IConsolePrint(CC_DEFAULT, "testnavesti: prebarveni na zlutou {} (sprite {}).",
			SpriteExists(PALETTE_SIGNAL_WARNING) ? "nacteno" : "CHYBI", PALETTE_SIGNAL_WARNING);
	/* What the loaded sets said about drawing more than two aspects: the
	 * property JGR's patchpack calls "railtype_extra_aspects". Nought means
	 * the set draws red and green only and the base set's own warning sprite
	 * is used instead. */
	for (uint r = 0; r < RAILTYPE_END; r++) {
		RailType rt = static_cast<RailType>(r);
		const RailTypeInfo *rti = GetRailTypeInfo(rt);
		if (rti->label == 0) continue;
		IConsolePrint(CC_DEFAULT, "testnavesti: kolej {} - navestidla z grf {}, aspektu navic {}", to_underlying(rt),
				rti->group[RailSpriteType::Signals] != nullptr ? "ano" : "ne", rti->signal_extra_aspects);
	}
	/* And what the loaded sets offered as signal styles of their own (feature
	 * 0E). For each one, which aspects it actually answers with a picture:
	 * red, green and the one this game draws yellow. A style that answers
	 * nothing is a style whose sprites never arrived. */
	static const char *druh[] = { "blok", "predvest-vjezd", "predvest-vyjezd", "predvest-kombi", "cestne", "cestne-jednosmer" };
	for (uint i = 0; i < _signal_styles.size(); i++) {
		const SignalStyle &st = _signal_styles[i];
		std::string got;
		for (SignalAspect a : {SignalAspect::Red, SignalAspect::Green, SignalAspect::Warning}) {
			SpriteID sp = GetCustomSignalStyleSprite(i, GetRailTypeInfo(RAILTYPE_RAIL), INVALID_TILE, SignalType::Path, SignalVariant::Electric, a, true);
			got += fmt::format(" {}={}", a == SignalAspect::Red ? "cervena" : (a == SignalAspect::Green ? "zelena" : "zluta"), sp);
		}
		IConsolePrint(CC_DEFAULT, "testnavesti: styl {} '{}' (grf {:08X}, cislo {}) aspektu navic {}, sprity{}{}",
				i, GetString(st.name), st.grf != nullptr ? std::byteswap(st.grf->grfid) : 0, st.local_id, st.extra_aspects, got,
				i == GetSignalStyleInUse() ? " <- kresli se timhle" : "");
	}
	/* And, for the style actually drawn with, which of this game's own six
	 * kinds of signal the set answers for at all. What it has nothing for is
	 * drawn from the base set, so a set that answers for only some of them
	 * leaves the rest looking as they always did. */
	if (GetSignalStyleInUse() < _signal_styles.size()) {
		for (SignalVariant var : {SignalVariant::Electric, SignalVariant::Semaphore}) {
			std::string got;
			for (uint t = 0; t < lengthof(druh); t++) {
				SpriteID sp = GetCustomSignalStyleSprite(GetSignalStyleInUse(), GetRailTypeInfo(RAILTYPE_RAIL), INVALID_TILE,
						static_cast<SignalType>(t), var, SignalAspect::Green, true);
				got += fmt::format(" {}={}", druh[t], sp);
			}
			IConsolePrint(CC_DEFAULT, "testnavesti: styl v pouziti, {}:{}", var == SignalVariant::Electric ? "svetelne" : "mechanicke", got);
		}
	}
	IConsolePrint(CC_DEFAULT, "testnavesti: celkem {} navestidel, stylu z grf {}.", n, _signal_styles.size());
	return true;
}

/**
 * Walk a train and say where its parts actually are against where they
 * should be. Usage: 'testdelka [unit number]' -- all trains if left out.
 *
 * The game's own check (CheckTrainsLengths()) says only that a train's parts
 * are not spaced as they should be, and names the train. Which joint is
 * wrong, and by how much, is the thing worth knowing and is not said.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestTrainLength(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	std::optional<uint> want;
	if (argv.size() >= 2) {
		auto p = ParseInteger(argv[1]);
		if (!p.has_value()) return false;
		want = (uint)*p;
	}
	for (const Train *v : Train::Iterate()) {
		if (v->First() != v) continue;
		if (want.has_value() && v->unitnumber != (UnitID)*want) continue;
		uint bad = 0, parts = 0;
		for (const Train *u = v->GetMovingFront(), *w = v->GetMovingNext(); w != nullptr; u = w, w = w->GetMovingNext()) {
			parts++;
			if (u->track == Track::Depot) continue;
			int gap = std::max(abs(u->x_pos - w->x_pos), abs(u->y_pos - w->y_pos));
			int want_gap = u->CalcNextVehicleOffset();
			bool depot = w->track == Track::Depot;
			if (!depot && gap != want_gap) {
				bad++;
				/* The offset is the mean of the two vehicles' lengths, and a
				 * vehicle's length is a NewGRF property -- so both lengths are
				 * said too. A save whose set is not loaded has the positions
				 * of one length and the arithmetic of another. */
				IConsolePrint(CC_ERROR, "testdelka: vlak {} - clanek {} (delka {}, stroj {}) na ({},{}) a {} (delka {}, stroj {}) na ({},{}): mezera {}, ma byt {}",
						v->unitnumber, u->index.base(), u->gcache.cached_veh_length, u->engine_type.base(), u->x_pos, u->y_pos,
						w->index.base(), w->gcache.cached_veh_length, w->engine_type.base(), w->x_pos, w->y_pos, gap, want_gap);
			}
		}
		IConsolePrint(bad != 0 ? CC_ERROR : CC_INFO, "testdelka: vlak {} - {} spoju, z toho {} spatnych.",
				v->unitnumber, parts, bad);
	}
	return true;
}

/**
 * Put a signal on a rail tunnel's or bridge's portal, or take it off.
 * Usage: 'testtunel <x> <y> [pryc|tah|tahpryc]'.
 *
 * Plain, this is the command a click on the portal sends. With 'tah' it is
 * instead the drag the player does along the line -- started two tiles short
 * of the near mouth and ended two past the far one -- so that the rig can
 * check the bore is picked up by a drag that runs across it and not only by
 * a click aimed at it.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestTunnelSignal(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Signal a rail tunnel or bridge. Usage: 'testtunel <x> <y> [pryc|tah|tahctrl|tahpryc] [blok|mech]'.");
		return true;
	}
	auto px = ParseInteger(argv[1]), py = ParseInteger(argv[2]);
	if (!px.has_value() || !py.has_value()) return false;
	std::string_view how = argv.size() >= 4 ? argv[3] : "";
	bool remove = how == "pryc" || how == "tahpryc";
	bool drag = how == "tah" || how == "tahpryc" || how == "tahctrl";
	/* Ctrl on the drag means autofill: the line is followed past the end of
	 * the drag until something stops it. It is a different walk from the
	 * plain drag and has its own reasons to give up, so it is asked for
	 * separately. */
	bool autofill = how == "tahctrl";
	/* Which signal the player would be building: the portal is drawn as that
	 * kind, so the rig has to be able to ask for more than one. */
	std::string_view kind = argv.size() >= 5 ? argv[4] : "";
	SignalType sigtype = kind == "blok" ? SignalType::Block : SignalType::Path;
	SignalVariant sigvar = kind == "mech" ? SignalVariant::Semaphore : SignalVariant::Electric;
	TileIndex tile = TileXY(*px, *py);
	if (!IsTileType(tile, TileType::TunnelBridge)) {
		IConsolePrint(CC_ERROR, "testtunel: na ({},{}) neni tunel ani most.", *px, *py);
		return true;
	}
	_pause_mode = {};
	TileIndex other = GetOtherTunnelBridgeEnd(tile);
	extern CommandCost BuildTunnelBridgeSignals(DoCommandFlags flags, TileIndex tile, bool remove, SignalType sigtype, SignalVariant sigvar);
	CommandCost r;
	AutoRestoreBackup cur_company(_current_company, GetTileOwner(tile));
	if (drag) {
		/* Out of the mouth, away from the bore, two tiles each way. */
		DiagDirection out = ReverseDiagDir(GetTunnelBridgeDirection(tile));
		TileIndexDiff step = TileOffsByDiagDir(out);
		TileIndex from = tile + 2 * step, to = other - 2 * step;
		Track track = DiagDirToDiagTrack(out);
		r = remove
				? Command<Commands::RemoveSignalLong>::Do(DoCommandFlag::Execute, from, to, track, autofill)
				: Command<Commands::BuildSignalLong>::Do(DoCommandFlag::Execute, from, to, track, sigtype, sigvar, false, autofill, false, 4);
		IConsolePrint(CC_DEFAULT, "testtunel: tah ({},{})..({},{}){}", TileX(from), TileY(from), TileX(to), TileY(to), autofill ? " s ctrl" : "");
	} else {
		r = BuildTunnelBridgeSignals(DoCommandFlag::Execute, tile, remove, sigtype, sigvar);
	}
	IConsolePrint(r.Succeeded() ? CC_DEFAULT : CC_ERROR, "testtunel: ({},{})..({},{}) {} - {}, navestidla {}/{}, sviti {}",
			*px, *py, TileX(other), TileY(other), remove ? "odebrani" : "postaveni",
			r.Succeeded() ? "ok" : "chyba",
			IsTunnelBridgeSignalled(tile) ? "ano" : "ne", IsTunnelBridgeSignalled(other) ? "ano" : "ne",
			IsTunnelBridgeSignalled(tile) ? (GetTunnelBridgeSignalState(tile, TunnelBridgeSignal::Entry) == SignalState::Red ? "cervena" : "zelena") : "-");
	return true;
}

/**
 * Swap the signal on a tile for another kind, the way the toolbar's convert
 * does. Usage: 'testnavest <x> <y> <kind>' -- 0 block, 4 path, 5 no-entry path
 *
 * A probe, not a repair: whether a reservation could have been booked through
 * a given signal from behind is answered by putting one there that cannot be,
 * and running the same save again.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSetSignal(std::span<std::string_view> argv)
{
	if (argv.size() < 4) {
		IConsolePrint(CC_HELP, "Convert the signal on a tile. Usage: 'testnavest <x> <y> <kind>' (0 block, 4 path, 5 no-entry path).");
		return true;
	}
	auto px = ParseInteger(argv[1]), py = ParseInteger(argv[2]), pk = ParseInteger(argv[3]);
	if (!px || !py || !pk || *pk >= to_underlying(SignalType::End)) return false;
	TileIndex tile = TileXY(*px, *py);
	if (!IsTileType(tile, TileType::Railway) || GetRailTileType(tile) != RailTileType::Signals) {
		IConsolePrint(CC_ERROR, "testnavest: na ({},{}) zadne navestidlo neni.", *px, *py);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, GetTileOwner(tile));
	SignalType want = static_cast<SignalType>(*pk);
	for (Track tr : GetTrackBits(tile)) {
		if (!HasSignalOnTrack(tile, tr)) continue;
		CommandCost r = Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, tile, tr, want,
				GetSignalVariant(tile, tr), true, false, false, SignalType::Block, SignalType::Block, 0, 0);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testnavest: ({},{}) kolej {} -> typ {} {}", *px, *py, (int)tr,
				*pk, r.Succeeded() ? "prevedeno" : "ODMITNUTO");
	}
	return true;
}

/**
 * Take the rescue engine's home depot away and give it back again.
 *
 * The scene it serves is the one the player found and the rig had no way of
 * building: an engine that reaches its casualty, couples up, and then has
 * nowhere it can reach to put it down. Taking the shed away while the engine is
 * out is the only honest way to arrange that on a rig -- and giving it back
 * afterwards is the half that matters, because the point being tested is not
 * that the engine stops, it is that it starts again by itself once there is
 * somewhere to go. Usage: 'testdepo pryc' / 'testdepo zpet'.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRescueDepot(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Take the testodtah home depot away or give it back. Usage: 'testdepo pryc|zpet'.");
		return true;
	}
	if (_testodtah_depot_w == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testdepo: zadna scena testodtah nestoji.");
		return true;
	}

	/* Building and demolishing are the player's own actions, so they are done
	 * as the player. Without this they are attempted as whatever company ran
	 * last, which on a headless rig is nobody. */
	AutoRestoreBackup cur_company(_current_company, _local_company);

	if (argv[1] == "pryc") {
		CommandCost r = Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, _testodtah_depot_w);
		IConsolePrint(r.Failed() ? CC_ERROR : CC_DEFAULT, "testdepo: depo ({},{}) zbourano - {}.",
				TileX(_testodtah_depot_w), TileY(_testodtah_depot_w), r.Failed() ? "nepovedlo se" : "ok");
	} else {
		CommandCost r = Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, _testodtah_depot_w, RAILTYPE_RAIL, DiagDirection::SW);
		IConsolePrint(r.Failed() ? CC_ERROR : CC_DEFAULT, "testdepo: depo ({},{}) postaveno zpet - {}.",
				TileX(_testodtah_depot_w), TileY(_testodtah_depot_w), r.Failed() ? "nepovedlo se" : "ok");
	}

	/* Taking a depot off the map or putting one back leaves work in the signal
	 * buffer, and the flush that normally follows a command belongs to the path
	 * the player's clicks take, not to the one used here. This runs from the
	 * heartbeat, in the middle of the tick, so the buffer left standing is the
	 * next moving train's problem: it walks into the assertion that the buffer
	 * is empty. Emptied here instead. */
	UpdateSignalsInBuffer();
	return true;
}

/**
 * Sell a train to the scrapyard the way the player does it, and say what came
 * of it.
 *
 * Through the command and not by setting the flag by hand: what is worth
 * measuring is the whole gesture, refusals and all. A refusal is said out loud
 * and in the rig's word for it, because a sale that does not happen leaves a
 * train driving happily on and no other counter would notice.
 *
 * @param t the train to sell, front of its consist
 */
static void TestSellTrainForScrap(Train *t)
{
	/* Same trap as every other command run from a tick: it reads whichever
	 * company happens to be current, and in a timer that is nobody. */
	AutoRestoreBackup cur_company(_current_company, t->owner);
	Money before = Company::Get(t->owner)->money;
	CommandCost r = Command<Commands::SellTrainForScrap>::Do(DoCommandFlag::Execute, t->index);
	if (r.Failed()) {
		IConsolePrint(CC_ERROR, "testprodat: ODMITNUTO - {}", GetString(r.GetErrorMessage()));
		return;
	}
	IConsolePrint(CC_INFO, "testprodat: vlak {} PRODAN na ({},{}), kasa {} -> {}",
			t->unitnumber, TileX(t->tile), TileY(t->tile), before, Company::Get(t->owner)->money);
}

/**
 * Open a vehicle's own window and look at the row where refitting and selling
 * take turns. Usage: 'testikonaprodat <cislo vlaku>|vagonky'
 *
 * The rig's only way into that window. Two things are measured, and both are
 * things no counter can see: which of the two buttons is in the row at all,
 * and whether the one that is there can be pressed. The same hole the wagon
 * list had -- a window opened with no way to answer it -- and the player found
 * that one by hand.
 *
 * On a rake of wagons it goes on to press the button, because there the press
 * is the whole gesture: a train is asked "are you sure?" first and the sale
 * itself is measured by testprodat.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSellIcon(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Look at the sell icon in a vehicle's window. Usage: 'testikonaprodat <cislo vlaku>|vagonky'.");
		return true;
	}

	Train *v = nullptr;
	if (argv[1] == "vagonky") {
		for (Train *t : Train::Iterate()) {
			if (t->First() != t || !t->IsFreeWagon() || t->IsInDepot()) continue;
			v = t;
			break;
		}
		if (v == nullptr) {
			IConsolePrint(CC_ERROR, "testikonaprodat: ODMITNUTO - zadne vagonky venku nestoji.");
			return true;
		}
	} else {
		auto punit = ParseInteger(argv[1]);
		if (!punit.has_value()) return false;
		for (Train *t : Train::Iterate()) {
			if (t->First() == t && t->IsFrontEngine() && t->unitnumber == (UnitID)*punit) {
				v = t;
				break;
			}
		}
		if (v == nullptr) {
			IConsolePrint(CC_ERROR, "testikonaprodat: vlak {} nenalezen.", argv[1]);
			return true;
		}
	}

	AutoRestoreBackup cur_company(_current_company, v->owner);
	ShowVehicleViewWindow(v);
	Window *w = FindWindowById(WindowClass::VehicleView, v->index);
	if (w == nullptr) {
		IConsolePrint(CC_ERROR, "testikonaprodat: ODMITNUTO - okno vozidla se neotevrelo.");
		return true;
	}

	NWidgetStacked *sel = w->GetWidget<NWidgetStacked>(WID_VV_SELECT_REFIT_TURN);
	if (sel == nullptr || sel->shown_plane == SZSP_NONE) {
		IConsolePrint(CC_ERROR, "testikonaprodat: ODMITNUTO - v okne neni ani prestavba, ani prodej.");
		return true;
	}
	bool selling = sel->shown_plane == 1;
	bool usable = selling && !w->IsWidgetDisabled(WID_VV_SELL);
	IConsolePrint(CC_INFO, "testikonaprodat: v okne je {}, {}", selling ? "prodej" : "prestavba",
			selling ? (usable ? "da se zmacknout" : "zatmaveny") : "prodej tam neni");
	if (!selling) return true;
	if (!usable) {
		IConsolePrint(CC_ERROR, "testikonaprodat: ODMITNUTO - cudlik prodeje nejde zmacknout.");
		return true;
	}
	if (!v->IsFreeWagon()) return true;

	w->OnClick(Point{}, WID_VV_SELL, 1);

	/* And the red window the press opens. Selling what the player left standing
	 * is as final as selling a whole train, so he is asked first -- which means
	 * the scene has to answer. That the window is there at all is worth
	 * measuring on its own: without it the press would go straight through. */
	Window *q = FindWindowById(WindowClass::ConfirmPopupQuery, 0);
	if (q == nullptr) {
		IConsolePrint(CC_ERROR, "testikonaprodat: ODMITNUTO - cervene okno se nezeptalo.");
		return true;
	}
	IConsolePrint(CC_INFO, "testikonaprodat: cervene okno se pta, mackam ano");
	q->OnClick(Point{}, WID_Q_YES, 1);

	/* Said from the wagons, not from the window: what is measured is whether
	 * the press reached them at all. */
	if (!v->IsSoldForScrap()) {
		IConsolePrint(CC_ERROR, "testikonaprodat: ODMITNUTO - vagonky se po zmacknuti neoznacily jako prodane.");
		return true;
	}
	IConsolePrint(CC_INFO, "testikonaprodat: vagonky PRODANY, ceka se na odtah");
	return true;
}

/**
 * Sell the test scene's train for scrap, or a named one. Usage: 'testprodat
 * [unit number]'.
 *
 * Scheduled with testza, so that a scene can sell a train at a moment of its
 * choosing -- notably a train that has just broken down, which the scrapyard
 * has to refuse.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSell(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Sell a train for scrap the way the player does. Usage: 'testprodat [cislo vlaku]'.");
		return true;
	}
	Train *t = nullptr;
	if (argv.size() >= 2) {
		auto n = ParseInteger(argv[1]);
		if (!n.has_value()) {
			IConsolePrint(CC_ERROR, "testprodat: cislo vlaku nedava smysl.");
			return true;
		}
		for (Train *v : Train::Iterate()) {
			if (v->IsFrontEngine() && v->unitnumber == (UnitID)*n) { t = v; break; }
		}
	} else {
		t = Train::GetIfValid(_testodtah_casualty);
	}
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testprodat: takovy vlak tu neni.");
		return true;
	}
	TestSellTrainForScrap(t->First());
	return true;
}

static bool ConTestRescue(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the junction-rescue test scene. Usage: 'testodtah [rovina|krizeni|jednosmer|daleko|depo|vagony|prodat|prodatporucha|prodatdlouhy|porouchanydlouhy] [signal cycle] [dve]'.");
		return true;
	}
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testodtah: only in a running game.");
		return true;
	}
	/* 'rovina' breaks the casualty down on the plain main line instead of on
	 * the points -- the control case: no straightening may fire there.
	 * 'krizeni' breaks it right on the junction tile and lays a stranger's
	 * reservation across it, which is the shape that made freeing the
	 * casualty's own forward path bail out and leak. */
	bool plain = argv.size() >= 2 && argv[1] == "rovina";
	bool crossing = argv.size() >= 2 && argv[1] == "krizeni";
	/* 'jednosmer' puts one-way path signals on the main line instead of
	 * ordinary ones. The casualty runs westwards along it, so the signals face
	 * that way and the rescue engine coming east out of its shed is going
	 * against every one of them -- which is the player's line, where a rescue
	 * engine has to reach a casualty head-on because the queue is behind it.
	 * The trailing number is which way round the signals are built; both are
	 * tried because the command cycles rather than states it. */
	bool oneway = argv.size() >= 2 && (argv[1] == "jednosmer" || argv[1] == "daleko");
	/* 'daleko' is 'jednosmer' with the casualty at the far end of the strip, so
	 * the rescue engine has to book the whole length of the line past every
	 * signal on it rather than the two nearest ones. */
	bool faraway = argv.size() >= 2 && argv[1] == "daleko";
	/* 'depo' breaks the casualty down the moment its engine has cleared the
	 * shed door with the wagon still inside: half a train out on the line,
	 * half of it in the depot. The player's own case, which took the game
	 * down without a report. */
	bool half_in_depot = argv.size() >= 2 && (argv[1] == "depo" || argv[1] == "depozpet");
	/* 'vagony': no breakdown at all. The casualty engine leaves its wagon at
	 * the branch platform (decouple all) and goes home; the wagon then stands
	 * there until 'testodvoz' calls the tow for it. */
	bool wagons_variant = argv.size() >= 2 && argv[1] == "vagony";
	/* 'prodat': no breakdown either. The casualty is sold to the scrapyard out
	 * on the plain line, which is the other thing a rescue engine is sent to --
	 * the tow fetches it and the depot breaks it up instead of repairing it.
	 * Built on the same strip as 'rovina' so the two can be read side by side:
	 * same road, same engine, different reason for standing there. */
	bool sell_variant = argv.size() >= 2 && argv[1] == "prodat";
	/* 'prodatporucha': break the casualty down and then try to sell it a moment
	 * later, which the scrapyard has to refuse -- the player's rule that nobody
	 * buys a breakdown, and the thing that keeps the two uses of the rescue
	 * engine from meeting. A moment later and not the same tick, because a
	 * breakdown takes a tick or two to actually start. */
	bool sell_broken_variant = argv.size() >= 2 && argv[1] == "prodatporucha";
	/* 'prodatdlouhy': the sale again, but on a train long enough to hang round
	 * the junction while the tow pulls it -- the player's own shape. His was
	 * nineteen vehicles behind the tow and it came apart on the curve; the
	 * two-vehicle casualty the plain variant sells never bends at all. */
	bool sell_long_variant = argv.size() >= 2 && argv[1] == "prodatdlouhy";
	if (sell_long_variant) sell_variant = true;
	/* 'porouchanydlouhy': the same long train, broken down instead of sold.
	 * The tow cannot take it either and gives the case up -- and the casualty,
	 * which nobody has been paid for, stays where it is, with the tow standing
	 * right against it. The player's crash: the tow drove off through it. */
	bool broken_long_variant = argv.size() >= 2 && argv[1] == "porouchanydlouhy";
	/* 'depozpet' is 'depo' without the shed on the stub: pull the west depot
	 * down once the tow is out (testzbourat depo) and the only depot left to
	 * bring the casualty to is the one it is half inside of, behind the tow. */
	bool stub_shed = argv.size() >= 2 && argv[1] == "depo";
	/* A second rescue engine, on call in the depot at the far end -- the one
	 * nearer the casualty, so it is the one sent. Built here rather than by
	 * testpostav because only the scene knows where it put its depots: the
	 * strip lands wherever the map happens to be flat. */
	bool second_tow = argv.size() >= 4 ? argv[3] == "dve" : (argv.size() >= 3 && argv[2] == "dve");
	uint8_t sig_cycle = 0;
	if (oneway && argv.size() >= 3) {
		auto n = ParseInteger(argv[2]);
		if (n.has_value()) sig_cycle = (uint8_t)*n;
	}

	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testodtah: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	_settings_game.vehicle.train_rescue_towing = true;

	EngineID eid_loco = EngineID::Invalid();
	EngineID eid_wagon = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type == RailVehicleType::Wagon) {
			if (eid_wagon == EngineID::Invalid()) eid_wagon = e->index;
		} else {
			if (eid_loco == EngineID::Invalid()) eid_loco = e->index;
		}
		if (eid_loco != EngineID::Invalid() && eid_wagon != EngineID::Invalid()) break;
	}
	if (eid_loco == EngineID::Invalid() || eid_wagon == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testodtah: no available engine or wagon.");
		return true;
	}

	/* The flattest clear run, like testspoj's, plus one column somewhere in
	 * its middle with room for the branch below. */
	static const uint LEN = 40;
	TileIndex strip = INVALID_TILE;
	uint xj = 0;
	for (uint y = 8; y < Map::SizeY() - 12 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			TileIndex t = TileXY(x, y);
			bool ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
			int z = ok ? GetTileZ(t) : -1;
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					uint sx0 = x - LEN + 1;
					/* A branch column: five flat clear tiles straight down,
					 * anywhere in the middle third of the strip. */
					for (uint bx = sx0 + 12; bx <= sx0 + 27; bx++) {
						bool col_ok = true;
						for (uint dy = 1; dy <= 5; dy++) {
							TileIndex bt = TileXY(bx, y + dy);
							if (!(IsTileType(bt, TileType::Clear) || IsTileType(bt, TileType::Trees)) || GetTileSlope(bt) != SLOPE_FLAT || GetTileZ(bt) != z0) { col_ok = false; break; }
						}
						if (col_ok) {
							strip = TileXY(sx0, y);
							xj = bx;
							break;
						}
					}
					if (strip != INVALID_TILE) break;
					run--; // keep sliding the window
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testodtah: no flat clear area found.");
		return true;
	}
	uint x0 = TileX(strip);
	uint y0 = TileY(strip);
	IConsolePrint(CC_DEFAULT, "testodtah: strip at ({},{})..({},{}), junction at ({},{}).", x0, y0, x0 + LEN - 1, y0, xj, y0);
	_testmapa_area[0] = x0; _testmapa_area[1] = y0 > 1 ? y0 - 1 : 0;
	_testmapa_area[2] = x0 + LEN - 1; _testmapa_area[3] = y0 + 6;

	TileIndex depot_w = TileXY(x0, y0);
	TileIndex depot_e = TileXY(x0 + LEN - 1, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_w, RAILTYPE_RAIL, DiagDirection::SW).Failed() ||
			Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_e, RAILTYPE_RAIL, DiagDirection::NE).Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: depot failed.");
		return true;
	}
	if (Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, y0), TileXY(x0 + 1, y0), RAILTYPE_RAIL, Track::X, false, true).Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: track failed.");
		return true;
	}
	/* The branch: a curve off the main line toward the south, a short stub, a
	 * platform at its end for the casualty to be heading to. The curve is the
	 * whole point: it hangs on the two edges of the junction tile that the
	 * rescue engine's approach cannot reach. */
	if (Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(xj, y0), RAILTYPE_RAIL, Track::Lower, false).Failed() ||
			Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(xj, y0 + 1), TileXY(xj, y0 + 2), RAILTYPE_RAIL, Track::Y, false, true).Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: branch failed.");
		return true;
	}
	if (Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, TileXY(xj, y0 + 3), RAILTYPE_RAIL, Axis::Y, 1, 2, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: branch station failed.");
		return true;
	}
	for (uint sx : {x0 + 2, xj - 3, xj + 3, x0 + LEN - 3}) {
		if (Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(sx, y0), Track::X,
				oneway ? SignalType::PathOneWay : SignalType::Path, SignalVariant::Electric,
				false, false, false, SignalType::Block, SignalType::Block, oneway ? sig_cycle : 0, 0).Failed()) {
			IConsolePrint(CC_ERROR, "testodtah: signal at ({},{}) failed.", sx, y0);
			return true;
		}
	}
	UpdateSignalsInBuffer();

	if (oneway) {
		/* Say which way they actually came out, since the command cycles the
		 * direction rather than being told it. */
		for (uint sx : {x0 + 2, x0 + LEN - 3}) {
			TileIndex t = TileXY(sx, y0);
			IConsolePrint(CC_DEFAULT, "testodtah jednosmer: navestidlo ({},{}) - smer NE {}, smer SW {}.",
					sx, y0,
					HasSignalOnTrackdir(t, Trackdir::X_NE) ? "ano" : "ne",
					HasSignalOnTrackdir(t, Trackdir::X_SW) ? "ano" : "ne");
		}
	}

	StationID st_branch = GetStationIndex(TileXY(xj, y0 + 3));

	/* The casualty: engine and one wagon, short enough that when it breaks on
	 * the branch nothing of it still lies on the main line. */
	auto [cost_c, veh_c, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_c.Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: casualty engine failed.");
		return true;
	}
	auto [cost_w, veh_w, un_d, un_e, un_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_w.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, veh_w, Train::Get(veh_c)->Last()->index, false).Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: casualty wagon failed.");
		return true;
	}
	if (sell_long_variant || broken_long_variant) {
		/* Long enough that its tail is still on the straight while its head is
		 * round the curve, which is where the player's came apart. Twelve, so
		 * the joined train would be over the length the game allows and the
		 * coupling is refused for good -- which is what this scene measures:
		 * the tow gives the case up and the sold train disappears. Shortened
		 * to five it is towed in and scrapped like any other, and that is the
		 * plain 'prodat' scene's job. */
		for (int i = 0; i < 12; i++) {
			auto [cost_x, veh_x, un_x1, un_x2, un_x3] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (cost_x.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, veh_x, Train::Get(veh_c)->Last()->index, false).Failed()) {
				IConsolePrint(CC_ERROR, "testodtah: dlouhy prodavany vlak - vagon se nepodaril.");
				return true;
			}
		}
	}

	if (half_in_depot) {
		/* A long casualty, so that most of it is still in the shed when the
		 * engine has cleared the door; and points right outside that door,
		 * so the wagons come out of the depot onto a junction tile the way
		 * the player's did. */
		for (int i = 0; i < 4; i++) {
			auto [cost_x, veh_x, un_x1, un_x2, un_x3] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (cost_x.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, veh_x, Train::Get(veh_c)->Last()->index, false).Failed()) {
				IConsolePrint(CC_ERROR, "testodtah: extra casualty wagon failed.");
				return true;
			}
		}
		TileIndex pts = TileXY(x0 + LEN - 4, y0);
		CommandCost pts_res = Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, pts, RAILTYPE_RAIL, Track::Lower, false);
		if (pts_res.Failed()) {
			IConsolePrint(CC_ERROR, "testodtah: points by the depot failed at ({},{}): {}", TileX(pts), TileY(pts), GetString(pts_res.GetErrorMessage()));
			return true;
		}
		/* The stub off the points is nice to have, not needed: a corner piece
		 * alone already makes the tile a junction. The ground beside the strip
		 * was never checked to be flat, so this may not build. */
		for (uint dy = 1; dy <= 2; dy++) {
			if (Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 4, y0 + dy), RAILTYPE_RAIL, Track::Y, false).Failed()) {
				IConsolePrint(CC_WARNING, "testodtah: stub off the points not built at ({},{}); points alone.", x0 + LEN - 4, y0 + dy);
				break;
			}
		}
		/* And a shed at the end of that stub: the nearest depot to the coupling
		 * point, and one the tow can only reach by turning round -- with half
		 * the casualty still inside the depot it broke down leaving. */
		if (stub_shed && Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 4, y0 + 3), RAILTYPE_RAIL, DiagDirection::NW).Failed()) {
			IConsolePrint(CC_WARNING, "testodtah: shed on the stub not built at ({},{}).", x0 + LEN - 4, y0 + 3);
		}
		/* Track built by hand leaves its signal work queued; flushed here, as
		 * the scene did for the rest of its rails above. */
		UpdateSignalsInBuffer();
	}
	Order to_branch;
	to_branch.MakeGoToStation(st_branch);
	to_branch.SetLoadType(OrderLoadType::NoLoad);
	to_branch.SetUnloadType(OrderUnloadType::NoUnload);
	if (wagons_variant) {
		to_branch.SetDecouple(true);
		to_branch.SetDecoupleCount(0);
	}
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_c, 0, to_branch);
	/* No order after the decouple on purpose: the platform is a dead end, and
	 * an engine sent anywhere from there would have to reverse through the
	 * very wagons it just left -- the player's mistake, not the rig's. It
	 * stays parked at the buffer; the tow takes the wagons from the other
	 * end. */

	/* The rescue engine, on call in the west depot: flag set, brake off. */
	auto [cost_r, veh_r, un_g, un_h, un_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_r.Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: rescue engine failed.");
		return true;
	}
	if (Command<Commands::SetRescueEngine>::Do(DoCommandFlag::Execute, veh_r, true).Failed()) {
		IConsolePrint(CC_ERROR, "testodtah: could not station the rescue engine.");
		return true;
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_r, false);

	/* And a second one in the east depot, when asked for: it stands nearer the
	 * casualty, so the call goes to it and the west engine is the one left to
	 * pick the case up if it lets go. */
	if (second_tow) {
		auto [cost_r2, veh_r2, un_j, un_k, un_l] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_r2.Failed() || Command<Commands::SetRescueEngine>::Do(DoCommandFlag::Execute, veh_r2, true).Failed()) {
			IConsolePrint(CC_ERROR, "testodtah: second rescue engine failed.");
			return true;
		}
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_r2, false);
		IConsolePrint(CC_DEFAULT, "testodtah: druha odtahovka = vlak {} v depu ({},{}).",
				Train::Get(veh_r2)->unitnumber, TileX(depot_e), TileY(depot_e));
	}

	/* Send the casualty off; the tick watcher breaks it down the moment its
	 * front turns onto the branch. */
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_c, false);
	_testodtah_casualty = veh_c;
	_testodtah_sell = sell_variant;
	_testodtah_sell_broken = sell_broken_variant;
	_testodtah_sell_after = 0;
	_testodtah_break_tile = wagons_variant ? INVALID_TILE : half_in_depot ? TileXY(x0 + LEN - 2, y0) : faraway ? TileXY(x0 + LEN - 6, y0) :
			((plain || oneway || sell_variant || sell_broken_variant || broken_long_variant) ? TileXY(xj + 8, y0) : (crossing ? TileXY(xj, y0) : TileXY(xj, y0 + 1)));
	_testodtah_cross_tile = crossing ? TileXY(xj, y0) : INVALID_TILE;
	_testodtah_depot_w = depot_w;
	_testspoj_active = true;

	IConsolePrint(CC_DEFAULT, "testodtah: scene ready. casualty=vlak {}, rescue=vlak {}, break at ({},{}).",
			Train::Get(veh_c)->unitnumber, Train::Get(veh_r)->unitnumber, TileX(_testodtah_break_tile), TileY(_testodtah_break_tile));
	return true;
}

/**
 * Build the loop scene: two ways round to the same casualty.
 *
 * The player's railway, and the shape none of the straight-strip scenes could
 * put a question to. The rescue engine's shed opens onto a line that goes both
 * ways: the short way to the casualty is straight ahead against one-way
 * signals, and the long way round is a loop that rejoins the line beyond them.
 * Both reach the casualty, so the only thing being measured is **which one it
 * picks** -- and the wrong answer is not a failure the engine reports, it is an
 * engine that quietly drives round the houses and fetches up behind the queue
 * that piled in behind the breakdown, which is the one place it can do no good.
 *
 * Usage: 'testokruh'.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRescueLoop(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the loop-rescue test scene: short way against one-way signals vs long way round. Usage: 'testokruh'.");
		return true;
	}
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testokruh: only in a running game.");
		return true;
	}

	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testokruh: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	_settings_game.vehicle.train_rescue_towing = true;

	EngineID eid_loco = EngineID::Invalid();
	EngineID eid_wagon = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type == RailVehicleType::Wagon) {
			if (eid_wagon == EngineID::Invalid()) eid_wagon = e->index;
		} else {
			if (eid_loco == EngineID::Invalid()) eid_loco = e->index;
		}
		if (eid_loco != EngineID::Invalid() && eid_wagon != EngineID::Invalid()) break;
	}
	if (eid_loco == EngineID::Invalid() || eid_wagon == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testokruh: no available engine or wagon.");
		return true;
	}

	/* A flat clear rectangle: the main line along the top and three rows below
	 * it for the loop to hang in. */
	static const uint LEN = 34;
	static const uint DEEP = 3;
	uint x0 = 0, y0 = 0;
	bool found = false;
	for (uint y = 8; y + DEEP < Map::SizeY() - 8 && !found; y++) {
		for (uint x = 2; x + LEN < Map::SizeX() - 2 && !found; x++) {
			bool ok = true;
			int z0 = GetTileZ(TileXY(x, y));
			for (uint dx = 0; dx < LEN && ok; dx++) {
				for (uint dy = 0; dy <= DEEP && ok; dy++) {
					TileIndex t = TileXY(x + dx, y + dy);
					ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) &&
							GetTileSlope(t) == SLOPE_FLAT && GetTileZ(t) == z0;
				}
			}
			if (ok) { x0 = x; y0 = y; found = true; }
		}
	}
	if (!found) {
		IConsolePrint(CC_ERROR, "testokruh: no flat clear area found.");
		return true;
	}
	uint y1 = y0 + DEEP;
	uint xa = x0 + 8;  // where the loop leaves the main line
	uint xb = x0 + 20; // where it rejoins it
	IConsolePrint(CC_DEFAULT, "testokruh: main line ({},{})..({},{}), loop ({},{})..({},{}).",
			x0, y0, x0 + LEN - 1, y0, xa, y1, xb, y1);
	_testmapa_area[0] = x0; _testmapa_area[1] = y0 > 1 ? y0 - 1 : 0;
	_testmapa_area[2] = x0 + LEN - 1; _testmapa_area[3] = y1 + 1;

	TileIndex depot_w = TileXY(x0, y0);
	TileIndex depot_e = TileXY(x0 + LEN - 1, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_w, RAILTYPE_RAIL, DiagDirection::SW).Failed() ||
			Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_e, RAILTYPE_RAIL, DiagDirection::NE).Failed()) {
		IConsolePrint(CC_ERROR, "testokruh: depot failed.");
		return true;
	}

	/* 'bezstanice' leaves the platform out and sends the casualty to the west
	 * depot instead. The platform sits on the road the rescue engine has to
	 * take, which no earlier scene did, so it has to be possible to take it
	 * away again and see whether it is what the engine is stumbling over. */
	bool no_station = false;
	for (size_t i = 1; i < argv.size(); i++) if (argv[i] == "bezstanice") no_station = true;

	/* Main line, with the casualty's destination platform near the west end so
	 * it drives the whole way down and breaks in the far east. */
	bool line_ok;
	if (no_station) {
		line_ok = Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + 1, y0), TileXY(x0 + LEN - 2, y0), RAILTYPE_RAIL, Track::X, false, true).Succeeded();
	} else {
		line_ok = Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + 1, y0), TileXY(x0 + 3, y0), RAILTYPE_RAIL, Track::X, false, true).Succeeded() &&
				Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, TileXY(x0 + 4, y0), RAILTYPE_RAIL, Axis::X, 1, 2, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Succeeded() &&
				Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + 6, y0), TileXY(x0 + LEN - 2, y0), RAILTYPE_RAIL, Track::X, false, true).Succeeded();
	}
	if (!line_ok) {
		IConsolePrint(CC_ERROR, "testokruh: main line failed.");
		return true;
	}

	/* 'rovne' builds the same scene without the loop -- the control case, to
	 * tell a fault that belongs to the loop apart from one that belongs to
	 * everything else the scene is the first to put in a rescue engine's way. */
	bool no_loop = argv.size() >= 2 && argv[1] == "rovne";

	/* The loop. The two curves on the main line are the points the player
	 * describes -- one turning off to the right, one back in from the left. */
	struct { TileIndex tile; Track track; } curves[] = {
		{ TileXY(xa, y0), Track::Right }, // west end of the loop: line from the west, down to the south
		{ TileXY(xa, y1), Track::Left },  // from the north, away to the east
		{ TileXY(xb, y1), Track::Upper }, // from the west, back up to the north
		{ TileXY(xb, y0), Track::Lower }, // from the south, onward to the east
	};
	if (!no_loop) {
		for (const auto &c : curves) {
			if (Command<Commands::BuildRail>::Do(DoCommandFlag::Execute, c.tile, RAILTYPE_RAIL, c.track, false).Failed()) {
				IConsolePrint(CC_ERROR, "testokruh: curve at ({},{}) failed.", TileX(c.tile), TileY(c.tile));
				return true;
			}
		}
		if (Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(xa, y0 + 1), TileXY(xa, y1 - 1), RAILTYPE_RAIL, Track::Y, false, true).Failed() ||
				Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(xb, y0 + 1), TileXY(xb, y1 - 1), RAILTYPE_RAIL, Track::Y, false, true).Failed() ||
				Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(xa + 1, y1), TileXY(xb - 1, y1), RAILTYPE_RAIL, Track::X, false, true).Failed()) {
			IConsolePrint(CC_ERROR, "testokruh: loop failed.");
			return true;
		}
	}

	/* One-way signals on the short way only, facing the way the traffic runs
	 * (westwards, the way the casualty was going), so a rescue engine driving
	 * east out of its shed is against every one of them. The long way round
	 * carries none, and is the cheap way for anything that pays the ordinary
	 * price for coming at a path signal from behind. */
	for (uint sx : {xa + 3, xa + 6, xa + 9}) {
		if (Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(sx, y0), Track::X,
				SignalType::PathOneWay, SignalVariant::Electric,
				false, false, false, SignalType::Block, SignalType::Block, 0, 0).Failed()) {
			IConsolePrint(CC_ERROR, "testokruh: signal at ({},{}) failed.", sx, y0);
			return true;
		}
	}
	UpdateSignalsInBuffer();
	for (uint sx : {xa + 3, xa + 9}) {
		TileIndex t = TileXY(sx, y0);
		IConsolePrint(CC_DEFAULT, "testokruh: navestidlo ({},{}) - smer NE {}, smer SW {}.", sx, y0,
				HasSignalOnTrackdir(t, Trackdir::X_NE) ? "ano" : "ne",
				HasSignalOnTrackdir(t, Trackdir::X_SW) ? "ano" : "ne");
	}

	/* Say what actually got built at the four corners. A loop with one corner
	 * laid the wrong way round is not a loop at all -- it is a dead-end siding,
	 * the engine has no choice to make, and the scene silently measures nothing
	 * while looking like a pass. */
	if (no_loop) {
		IConsolePrint(CC_DEFAULT, "testokruh rovne: okruh se nestavi, jen prima trat.");
	} else {
		for (const auto &c : curves) {
			IConsolePrint(CC_DEFAULT, "testokruh: roh ({},{}) koleje {:#x}.", TileX(c.tile), TileY(c.tile), GetTrackBits(c.tile).base());
		}
	}

	StationID st_west = no_station ? StationID::Invalid() : GetStationIndex(TileXY(x0 + 4, y0));

	auto [cost_c, veh_c, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	auto [cost_w, veh_w, un_d, un_e, un_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_c.Failed() || cost_w.Failed() ||
			Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, veh_w, Train::Get(veh_c)->Last()->index, false).Failed()) {
		IConsolePrint(CC_ERROR, "testokruh: casualty failed.");
		return true;
	}
	Order to_west;
	if (no_station) {
		to_west.MakeGoToDepot(GetDepotIndex(depot_w), OrderDepotTypeFlags{}, OrderNonStopFlags{});
	} else {
		to_west.MakeGoToStation(st_west);
		to_west.SetLoadType(OrderLoadType::NoLoad);
		to_west.SetUnloadType(OrderUnloadType::NoUnload);
	}
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_c, 0, to_west);

	auto [cost_r, veh_r, un_g, un_h, un_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_r.Failed() || Command<Commands::SetRescueEngine>::Do(DoCommandFlag::Execute, veh_r, true).Failed()) {
		IConsolePrint(CC_ERROR, "testokruh: rescue engine failed.");
		return true;
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_r, false);
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_c, false);

	_testodtah_casualty = veh_c;
	/* East of where the loop rejoins, so both ways round really do reach it. */
	_testodtah_break_tile = TileXY(x0 + LEN - 6, y0);
	_testodtah_cross_tile = INVALID_TILE;
	_testodtah_depot_w = depot_w;
	_testokruh_rescue = veh_r;
	_testokruh_detour_row = y1;
	_testokruh_detour_seen = false;
	_testspoj_active = true;

	IConsolePrint(CC_DEFAULT, "testokruh: scene ready. casualty=vlak {}, rescue=vlak {}, break at ({},{}).",
			Train::Get(veh_c)->unitnumber, Train::Get(veh_r)->unitnumber, TileX(_testodtah_break_tile), TileY(_testodtah_break_tile));
	return true;
}

/** Break the testodtah casualty down the moment it reaches the armed tile. */
static const IntervalTimer<TimerGameTick> _testodtah_watch({TimerGameTick::Priority::None, 1}, [](auto) {
	/* Which way round the loop the rescue engine went. Said once, the first
	 * time it sets a wheel on the far side of the loop: from there on it is
	 * driving away from the casualty the long way, and no later measurement --
	 * not even a successful tow -- tells that apart from having gone straight. */
	if (_testokruh_detour_row != 0 && !_testokruh_detour_seen) {
		const Train *r = Train::GetIfValid(_testokruh_rescue);
		if (r != nullptr && TileY(r->tile) == _testokruh_detour_row) {
			_testokruh_detour_seen = true;
			IConsolePrint(CC_WARNING, "testokruh: odtahovka jede objizdkou okolo, ne proti navestidlum.");
		}
	}

	/* The deliberate refusal: a breakdown the rig then tries to sell. Counted
	 * down rather than done on the spot, because a breakdown needs a tick or
	 * two to actually start and a train that is not broken yet would be sold. */
	if (_testodtah_sell_after > 0 && --_testodtah_sell_after == 0) {
		Train *broken = Train::GetIfValid(_testodtah_casualty);
		if (broken != nullptr) TestSellTrainForScrap(broken->First());
	}

	if (_testodtah_break_tile == INVALID_TILE) return;
	Train *t = Train::GetIfValid(_testodtah_casualty);
	if (t == nullptr) {
		_testodtah_break_tile = INVALID_TILE;
		return;
	}
	if (t->tile != _testodtah_break_tile) return;
	_testodtah_break_tile = INVALID_TILE;
	if (_testodtah_sell) {
		TestSellTrainForScrap(t);
		return;
	}
	t->breakdown_ctr = 2;
	if (_testodtah_sell_broken) _testodtah_sell_after = 30;
	IConsolePrint(CC_INFO, "testodtah: vlak {} porouchan na ({},{}).", t->unitnumber, TileX(t->tile), TileY(t->tile));
	if (_testodtah_cross_tile != INVALID_TILE) {
		/* A stranger's reservation across the casualty's tile, staged: the
		 * shape that made freeing the casualty's own path bail out. */
		if (TryReserveRailTrack(_testodtah_cross_tile, Track::X)) {
			IConsolePrint(CC_INFO, "testodtah: cizi rezervace polozena pres ({},{}).", TileX(_testodtah_cross_tile), TileY(_testodtah_cross_tile));
		}
		_testodtah_cross_tile = INVALID_TILE;
	}
});

/**
 * Run a console command after a delay, once. Usage: testza <ticks> <command...>
 * Granularity is the heartbeat's 1000 ticks.
 * @copydoc IConsoleCmdProc
 */
static std::vector<std::pair<int, std::string>> _testza_queue;

static bool ConTestAfter(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Run a console command later. Usage: 'testza <ticks> <command...>'. May be given several times; each fires once.");
		return true;
	}
	auto pticks = ParseInteger(argv[1]);
	if (!pticks.has_value()) return false;
	std::string cmd;
	for (size_t i = 2; i < argv.size(); i++) {
		if (!cmd.empty()) cmd += ' ';
		cmd += argv[i];
	}
	IConsolePrint(CC_DEFAULT, "testza: '{}' za {} tiku.", cmd, *pticks);
	_testza_queue.emplace_back((int)*pticks, std::move(cmd));
	_testspoj_active = true;
	return true;
}

/**
 * List the rail engines and whether the first company may build them.
 * Stages the question "what is still on offer in a late game": model age
 * against the three life phases that decide when a model goes out of
 * production, and whether it has been marked as never going.
 * Usage: testmodely
 * @copydoc IConsoleCmdProc
 */
static bool ConTestListEngineModels(std::span<std::string_view>)
{
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Wagon) continue;
		if (!e->info.climates.Test(_settings_game.game_creation.landscape)) continue;
		/* Upkeep and reliability as the purchase list shows them: the two
		 * figures the engine-care setting moves, so that turning it on and off
		 * is something the rig can read rather than something to take on
		 * trust. */
		IConsolePrint(CC_DEFAULT, "model {:2}: {:<24} k dispozici {} stari {} mesicu, faze {}+{}+{}={} mesicu, udrzba {} spolehlivost {}{}",
				e->index.base(), GetString(e->info.string_id), e->company_avail.Test(CompanyID::Begin()) ? "ano" : "ne ", e->age,
				e->duration_phase_1, e->duration_phase_2, e->duration_phase_3,
				e->duration_phase_1 + e->duration_phase_2 + e->duration_phase_3,
				e->GetRunningCost(), e->reliability,
				e->info.base_life == 0xFF ? " (vyrabi se navzdy)" : "");
	}
	return true;
}

/**
 * How long every wagon the company can buy really is, and how many pieces a
 * set builds it out of. One wagon of each is bought in a shed put up for the
 * purpose, measured and sold again.
 *
 * Length is what decides which wagon can carry which road vehicle
 * (road_on_rail.h), and it cannot be read off an engine: a set builds a long
 * wagon out of several pieces and only says how long each piece is through a
 * callback, which is only answered once the wagon exists. So this buys one.
 * Usage: testvagonky
 * @copydoc IConsoleCmdProc
 */
static bool ConTestWagonLengths(std::span<std::string_view>)
{
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testvagonky: only in a running game.");
		return true;
	}
	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testvagonky: no company to buy as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	AutoRestoreBackup cur_company(_current_company, _local_company);

	/* One flat clear tile is all a shed needs. */
	TileIndex depot = INVALID_TILE;
	for (uint y = 4; y < Map::SizeY() - 4 && depot == INVALID_TILE; y++) {
		for (uint x = 4; x < Map::SizeX() - 4; x++) {
			TileIndex t = TileXY(x, y);
			if (!IsTileType(t, TileType::Clear) && !IsTileType(t, TileType::Trees)) continue;
			if (GetTileSlope(t) != SLOPE_FLAT) continue;
			if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, t, RAILTYPE_RAIL, DiagDirection::SW).Succeeded()) {
				depot = t;
				break;
			}
		}
	}
	if (depot == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testvagonky: nikde se nepodarilo postavit depo.");
		return true;
	}

	uint counted = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (RailVehInfo(e->index)->railveh_type != RailVehicleType::Wagon) continue;
		auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, e->index, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) {
			IConsolePrint(CC_ERROR, "testvagonky: {:<28} nekoupen - {}", GetString(e->info.string_id), RefusalReason(cost));
			continue;
		}
		const Train *t = Train::GetIfValid(veh);
		if (t == nullptr) continue;
		uint pieces = 0, length = 0;
		for (const Train *p = t; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) {
			pieces++;
			length += p->gcache.cached_veh_length;
		}
		IConsolePrint(CC_DEFAULT, "testvagonky: {:<28} delka {:2d} v {} kusech, unese auto do delky {}",
				GetString(e->info.string_id), length, pieces, length);
		counted++;
		Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, veh, false, false, ClientID::Invalid);
	}
	Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, depot);
	IConsolePrint(CC_INFO, "testvagonky: zmereno {} vagonu (delka je v osminach dlazdice, cela dlazdice je 16).", counted);
	return true;
}

/**
 * Check every train and loose rake for a wagon whose pieces do not all face
 * the same way. One wagon drawn in several pieces is one vehicle, and its
 * pieces facing different ways is a consist that will walk apart in both
 * directions the moment it moves -- which is what three crash reports turned
 * out to be. Cheap enough to ask after anything that joins or turns a train.
 * Usage: testnatoceni
 * @copydoc IConsoleCmdProc
 */
static bool ConTestFacings(std::span<std::string_view>)
{
	uint bad = 0;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		const Train *unit = nullptr;
		uint index = 0;
		for (const Train *u = t; u != nullptr; u = u->Next(), index++) {
			if (!u->IsArticulatedPart()) {
				unit = u;
				continue;
			}
			if (unit == nullptr) continue;
			/* A piece on a curve stands up to 45 degrees off its head and is
			 * right to; the player's log had eight such pieces reported, all of
			 * them one step off in a bend. What walks a wagon apart is a piece
			 * turned a right angle or more away from its own head. */
			DirDiff off = DirDifference(u->direction, unit->direction);
			if (off == DirDiff::Same || off == DirDiff::Left45 || off == DirDiff::Right45) continue;
			bad++;
			IConsolePrint(CC_ERROR, "testnatoceni: {} {} - clanek {} na ({},{}) smer {}, jeho hlava smer {}",
					t->IsFrontEngine() ? "vlak" : "rada", t->IsFrontEngine() ? t->unitnumber : (UnitID)0,
					index, TileX(u->tile), TileY(u->tile), to_underlying(u->direction), to_underlying(unit->direction));
		}
	}
	IConsolePrint(bad != 0 ? CC_ERROR : CC_INFO, "testnatoceni: rozhozenych clanku {}.", bad);
	return true;
}

/**
 * Fill a road vehicle with its own cargo, without a station or an industry.
 * A set draws a loaded vehicle differently from an empty one, and reading
 * which picture it draws needs a vehicle that has something in it; getting
 * there by waiting for an industry to produce is a scene of its own.
 * Usage: testnalozit auto <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestFillRoadVehicle(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Fill a road vehicle, or a waiting rake's wagons of one cargo. Usage: 'testnalozit auto <unit number>' or 'testnalozit rada <cargo> [<cislo rady>|kazdy2|kazdy2od2|vagonkazdy2]'.");
		return true;
	}
	/* 'rada <cargo>': fill the wagons of that cargo in every waiting rake, and
	 * leave every other wagon alone. What the fullness filter is asked about
	 * is exactly this difference, so the rig has to be able to make it. */
	if (argv[1] == "rada") {
		auto pcargo = ParseInteger(argv[2]);
		if (!pcargo.has_value()) return false;
		CargoType want = (CargoType)*pcargo;
		/* One rake by its number, when the scene needs them to differ: "prefer
		 * the fullest" cannot be measured at all while every rake in the shed
		 * is equally empty. Without a number, all of them, as before. */
		bool every_other = argv.size() >= 4 && (argv[3] == "kazdy2" || argv[3] == "kazdy2od2");
		bool odd_first = argv.size() >= 4 && argv[3] == "kazdy2od2";
		/* Every second wagon inside each rake, rather than every second rake:
		 * a rake half full and half empty is the one thing the two readings of
		 * the couple filter answer differently about, and with one rake at
		 * the platform "every second rake" is all of it or none. */
		bool every_other_wagon = argv.size() >= 4 && argv[3] == "vagonkazdy2";
		auto pwhich = (argv.size() >= 4 && !every_other && !every_other_wagon) ? ParseInteger(argv[3]) : std::nullopt;
		uint rakes = 0, put = 0, seen = 0;
		for (Train *t : Train::Iterate()) {
			if (t->First() != t || !t->IsFreeWagon()) continue;
			if (pwhich.has_value() && t->index.base() != (uint)*pwhich) continue;
			/* Every second one, so a shed comes out half full and half empty:
			 * "the fullest first" cannot be measured while everything in it is
			 * the same. */
			if (every_other && ((seen++ & 1) != 0) != odd_first) continue;
			rakes++;
			uint nth = 0;
			for (Train *u = t; u != nullptr; u = u->Next()) {
				if (u->cargo_type != want) continue;
				if (every_other_wagon && (nth++ & 1) != 0) continue;
				uint room = u->cargo_cap - u->cargo.StoredCount();
				if (room == 0 || !CargoPacket::CanAllocateItem()) continue;
				u->cargo.Append(CargoPacket::Create(t->last_station_visited, room, Source{}));
				put += room;
			}
			t->MarkDirty();
		}
		IConsolePrint(rakes == 0 ? CC_ERROR : CC_DEFAULT, "testnalozit: {} odpojenych rad, nalozeno {} jednotek nakladu {}.", rakes, put, (int)want);
		return true;
	}

	auto punit = ParseInteger(argv[2]);
	if (!punit.has_value()) return false;

	for (RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (!rv->IsFrontEngine() || rv->unitnumber != (UnitID)*punit) continue;
		uint put = 0;
		for (RoadVehicle *u = rv; u != nullptr; u = u->Next()) {
			uint room = u->cargo_cap - u->cargo.StoredCount();
			if (room == 0 || !CargoPacket::CanAllocateItem()) continue;
			u->cargo.Append(CargoPacket::Create(rv->last_station_visited, room, Source{}));
			put += room;
		}
		rv->MarkDirty();
		IConsolePrint(CC_DEFAULT, "testnalozit: auto {} nalozeno {} jednotek nakladu {}.", rv->unitnumber, put, (int)rv->cargo_type);
		return true;
	}
	IConsolePrint(CC_ERROR, "testnalozit: auto {} nenalezeno.", argv[2]);
	return true;
}

/**
 * Count the effect vehicles alive, by kind.
 * Smoke and explosions are effect vehicles that each count their own life
 * down; a kind whose count never falls is a kind that never expires. That is
 * exactly the fault the crash smoke had (TEMATA 4.26), and it is invisible
 * from a headless run without this.
 * Usage: testefekty
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCountEffects(std::span<std::string_view>)
{
	static const char *names[] = {"jiskry", "kour-parni", "kour-diesel", "kour-elektro", "vysyp",
			"vybuch-velky", "kour-poruchy", "vybuch-maly", "buldozer", "bublina",
			"kour-poruchy-letadlo", "kour-dulni"};
	std::map<uint, uint> counts;
	for (const EffectVehicle *e : EffectVehicle::Iterate()) counts[e->subtype]++;
	std::string out;
	for (const auto &[type, count] : counts) {
		fmt::format_to(std::back_inserter(out), " {}={}", type < lengthof(names) ? names[type] : "?", count);
	}
	IConsolePrint(CC_DEFAULT, "testefekty: celkem {}{}", EffectVehicle::GetNumItems(), out.empty() ? " (zadne)" : out);
	return true;
}

/**
 * Run a console command after a delay measured in ticks, once.
 * Usage: testzatik <ticks> <command...>
 *
 * The heartbeat clock of testza is whole seconds, which is too coarse to
 * catch a train on its way home with a load: the whole run home can take
 * less than a second. This one fires on a ten-tick clock instead.
 * @copydoc IConsoleCmdProc
 */
static std::vector<std::pair<int, std::string>> _testzatik_queue;

static bool ConTestAfterTicks(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Run a console command later, timed in ticks. Usage: 'testzatik <ticks> <command...>'.");
		return true;
	}
	auto pticks = ParseInteger(argv[1]);
	if (!pticks.has_value()) return false;
	std::string cmd;
	for (size_t i = 2; i < argv.size(); i++) {
		if (!cmd.empty()) cmd += ' ';
		cmd += argv[i];
	}
	IConsolePrint(CC_DEFAULT, "testzatik: '{}' za {} tiku.", cmd, *pticks);
	_testzatik_queue.emplace_back((int)*pticks, std::move(cmd));
	return true;
}

/** Fire the tick-timed delayed commands (testzatik). */
static const IntervalTimer<TimerGameTick> _testzatik_timer({TimerGameTick::Priority::None, 10}, [](auto) {
	for (auto it = _testzatik_queue.begin(); it != _testzatik_queue.end(); ) {
		it->first -= 10;
		if (it->first <= 0) {
			std::string cmd = std::move(it->second);
			it = _testzatik_queue.erase(it);
			IConsolePrint(CC_DEFAULT, "testzatik: (tik {}) {}", TimerGameTick::counter, cmd);
			IConsoleCmdExec(cmd);
		} else {
			++it;
		}
	}
});

/**
 * Sell every headless rake standing in the depot on a given tile.
 * Stages the one way a collector can arrive at a shed it was sent to and
 * find it empty: the wagons it claimed are gone by the time it gets there.
 * Usage: testzrus <x> <y>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestScrapRakesInDepot(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Sell wagons stored in a depot. Usage: 'testzrus' for all of them, or 'testzrus <x> <y>' for one depot.");
		return true;
	}
	TileIndex tile = INVALID_TILE;
	if (argv.size() == 3) {
		auto px = ParseInteger(argv[1]);
		auto py = ParseInteger(argv[2]);
		if (!px.has_value() || !py.has_value()) return false;
		tile = TileXY(*px, *py);
	} else if (argv.size() != 1) {
		return false;
	}

	std::vector<VehicleID> doomed;
	for (const Train *t : Train::Iterate()) {
		if (!t->IsFreeWagon() || t->track != Track::Depot) continue;
		if (tile != INVALID_TILE && t->tile != tile) continue;
		doomed.push_back(t->index);
	}
	for (VehicleID id : doomed) {
		const Train *t = Train::GetIfValid(id);
		if (t == nullptr) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, id, true, false, ClientID::Invalid);
	}
	IConsolePrint(CC_DEFAULT, "testzrus: zruseno {} rad odlozenych vagonku.", doomed.size());
	return true;
}

/**
 * Put a headless rake of wagons into the depot on a given tile, as if a train
 * had just left them there. Usage: testvagony <x> <y> [count]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestStoreRake(std::span<std::string_view> argv)
{
	if (argv.empty() || argv.size() > 4) {
		IConsolePrint(CC_HELP, "Store wagons in a depot. Usage: 'testvagony <x> <y> [count]',");
		IConsolePrint(CC_HELP, "or 'testvagony [count]' to put them where a train is already waiting to collect some.");
		return true;
	}

	uint count = 1;
	TileIndex tile = INVALID_TILE;
	if (argv.size() >= 3) {
		auto px = ParseInteger(argv[1]);
		auto py = ParseInteger(argv[2]);
		if (!px.has_value() || !py.has_value()) return false;
		tile = TileXY(*px, *py);
		if (argv.size() == 4) {
			auto pc = ParseInteger(argv[3]);
			if (!pc.has_value()) return false;
			count = *pc;
		}
	} else {
		if (argv.size() == 2) {
			auto pc = ParseInteger(argv[1]);
			if (!pc.has_value()) return false;
			count = *pc;
		}
		/* Whichever shed a train is sitting in waiting for wagons -- the
		 * scenes lay their track down wherever the generated map has room,
		 * so the tile is never the same twice and cannot be typed out. */
		for (const Train *t : Train::Iterate()) {
			if (!t->IsFrontEngine() || t->track != Track::Depot) continue;
			if (!t->current_order.IsType(OT_GOTO_DEPOT) || !t->current_order.ShouldGoToCouple()) continue;
			tile = t->tile;
			break;
		}
		if (tile == INVALID_TILE) {
			/* Or, failing that, whichever shed holds a rake somebody has
			 * spoken for -- which is where buying wagons is the interesting
			 * thing to do: they must not join the reserved row. */
			for (const Train *rake : Train::Iterate()) {
				if (!rake->IsFreeWagon() || rake->track != Track::Depot) continue;
				if (!IsRakeClaimedForCoupling(rake)) continue;
				tile = rake->tile;
				break;
			}
		}
		if (tile == INVALID_TILE) {
			IConsolePrint(CC_ERROR, "testvagony: zadna mashinka v depu neceka na vagonky.");
			return true;
		}
	}

	EngineID eid_wagon = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type != RailVehicleType::Wagon) continue;
		eid_wagon = e->index;
		break;
	}
	if (eid_wagon == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testvagony: zadny vagon k dispozici.");
		return true;
	}

	AutoRestoreBackup cur_company(_current_company, _local_company);
	VehicleID head = VehicleID::Invalid();
	for (uint i = 0; i < count; i++) {
		auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, tile, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) {
			IConsolePrint(CC_ERROR, "testvagony: vagon {} se nepodarilo postavit.", i);
			return true;
		}
		if (head == VehicleID::Invalid()) {
			head = veh;
		} else {
			Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, veh, Train::Get(head)->Last()->index, false);
		}
	}
	IConsolePrint(CC_DEFAULT, "testvagony: v depu ({},{}) odlozeno {} vagonu.", TileX(tile), TileY(tile), count);
	return true;
}

/**
 * Toggle a train's hand brake, the same as the player's start/stop button.
 * Meant for staged scenes: a train built stopped is released mid-scene.
 * Usage: testbrzda <unit number>
 * @copydoc IConsoleCmdProc
 */
/**
 * Pull down the west depot of the rescue scene, once the rescue engine is
 * out of it: leaves the tow with only the depot the casualty is half inside
 * of to bring it to. Usage: testzbourat depo
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDemolishDepot(std::span<std::string_view> argv)
{
	if (argv.size() != 2 || argv[1] != "depo") {
		IConsolePrint(CC_HELP, "Demolish the rescue scene's west depot. Usage: 'testzbourat depo'.");
		return true;
	}
	if (_testodtah_depot_w == INVALID_TILE || !IsRailDepotTile(_testodtah_depot_w)) {
		IConsolePrint(CC_ERROR, "testzbourat: zadne zapadni depo sceny.");
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, GetTileOwner(_testodtah_depot_w));
	CommandCost res = Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute, _testodtah_depot_w);
	UpdateSignalsInBuffer();
	IConsolePrint(res.Failed() ? CC_ERROR : CC_DEFAULT, "testzbourat: zapadni depo ({},{}) {}.", TileX(_testodtah_depot_w), TileY(_testodtah_depot_w),
			res.Failed() ? GetString(res.GetErrorMessage()) : std::string("zbourano"));
	return true;
}

/**
 * Open a train's vehicle window, the same as the player clicking on it.
 * Usage: testokno <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestOpenWindow(std::span<std::string_view> argv)
{
	if (argv.size() != 2 && !(argv.size() == 3 && (argv[1] == "smer" || argv[1] == "letadlo" || argv[1] == "rozkazy"))) {
		IConsolePrint(CC_HELP, "Open a train's window, an industry's, or a waypoint's. Usage: 'testokno <unit number>', 'testokno prumysl', 'testokno rozkazy [letadlo|lod|auto|<unit number>]' or 'testokno smer <waypoint index>'.");
		return true;
	}
	if (argv.size() == 3 && argv[1] == "smer") {
		/* A window is built when it opens, and a mistake in its widget tree
		 * is an exception at that moment -- a waypoint window built with a
		 * coloured label crashed the player's game on every click for two
		 * builds, and the rig never opened one. */
		auto pidx = ParseInteger(argv[2]);
		if (!pidx.has_value()) return false;
		const Waypoint *wp = Waypoint::GetIfValid(static_cast<StationID>(*pidx));
		if (wp == nullptr) {
			IConsolePrint(CC_ERROR, "testokno: smerovani {} neexistuje.", argv[2]);
			return true;
		}
		ShowWaypointWindow(wp);
		IConsolePrint(CC_DEFAULT, "testokno: okno smerovani {} otevreno.", wp->index.base());
		return true;
	}
	if (argv[1] == "letadlo" || argv[1] == "rozkazy") {
		/* Any vehicle, not just a train, and its orders window too: a window
		 * is built when it opens and drawn when it is painted, and a mistake
		 * in either is a crash at that moment. */
		extern void ShowOrdersWindow(const Vehicle *v);
		/* Which kind of vehicle, not just which one: the orders window is built
		 * from one widget tree for ground vehicles and another for ships and
		 * aircraft, and a mistake in the code that fills it in shows only in
		 * the tree that is missing the widget being asked for. The rig opened
		 * a train's every time and saw nothing. */
		VehicleType want = VehicleType::Invalid;
		if (argv[1] == "letadlo") want = VehicleType::Aircraft;
		std::optional<uint32_t> punit2;
		if (argv.size() > 2) {
			if (argv[2] == "letadlo") {
				want = VehicleType::Aircraft;
			} else if (argv[2] == "lod") {
				want = VehicleType::Ship;
			} else if (argv[2] == "auto") {
				/* The order window of a road vehicle: it is built from the
				 * ground-vehicle tree like a train's, but the row of buttons
				 * at the bottom is filled in differently (road_on_rail.h), and
				 * a mistake there shows only when a road vehicle's is opened. */
				want = VehicleType::Road;
			} else {
				punit2 = ParseInteger(argv[2]);
			}
		}
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (v->First() != v || !v->IsPrimaryVehicle()) continue;
			if (want != VehicleType::Invalid && v->type != want) continue;
			if (punit2.has_value() && v->unitnumber != (UnitID)*punit2) continue;
			ShowVehicleViewWindow(v);
			if (argv[1] == "rozkazy") ShowOrdersWindow(v);
			/* Whether the crosshair row is in the window right now, not
			 * whether it would be there if the window were opened again: the
			 * switch has to reach windows that are already open. */
			std::string_view crosshair = "?";
			if (Window *w = FindWindowById(WindowClass::VehicleView, v->index); w != nullptr) {
				if (NWidgetStacked *sel = w->GetWidget<NWidgetStacked>(WID_VV_SELECT_RAID); sel != nullptr) {
					crosshair = sel->shown_plane == SZSP_NONE ? "ne" : "ano";
				}
			}
			IConsolePrint(CC_DEFAULT, "testokno: okno vozidla {} ({}) otevreno, zamerovac v okne: {}.", v->unitnumber, argv[1], crosshair);
			return true;
		}
		IConsolePrint(CC_ERROR, "testokno: zadne takove vozidlo.");
		return true;
	}
	if (argv[1] == "prumysl") {
		extern void ShowIndustryViewWindow(IndustryID industry);
		/* Whatever the first industry on the map is: enough to have the
		 * window built and painted, which is where a mistake in it shows. */
		for (const Industry *i : Industry::Iterate()) {
			ShowIndustryViewWindow(i->index);
			IConsolePrint(CC_DEFAULT, "testokno: okno prumyslu {} na ({},{}) otevreno.", i->index.base(), TileX(i->location.tile), TileY(i->location.tile));
			return true;
		}
		IConsolePrint(CC_ERROR, "testokno: na mape neni zadny prumysl.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		ShowVehicleViewWindow(t);
		IConsolePrint(CC_DEFAULT, "testokno: okno vlaku {} otevreno.", t->unitnumber);
		return true;
	}
	IConsolePrint(CC_ERROR, "testokno: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * The shape of a train's wagons: how many pieces each one is made of, how long
 * each piece is and where it sits. A set builds a long wagon out of a visible
 * head and invisible articulated pieces, so a wagon that looks like one vehicle
 * on the screen is several here, and anything put "on the wagon" lands on the
 * head -- at one end of it. Which is what this is for reading.
 *
 * The same for a road vehicle ('testtvar auto <unit number>'), which is the
 * other half of the same question: a lorry with a trailer is one vehicle to
 * the player and several pieces here, and how long the whole of it is decides
 * whether it fits on a wagon.
 * Usage: testtvar <unit number> | testtvar auto <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestWagonShape(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Print the shape of a train's wagons. Usage: 'testtvar <unit number>' or 'testtvar auto <unit number>'.");
		return true;
	}
	bool road = argv[1] == "auto";
	if (road && argv.size() < 3) {
		IConsolePrint(CC_HELP, "Print the shape of a road vehicle. Usage: 'testtvar auto <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[road ? 2 : 1]);
	if (!punit.has_value()) return false;

	if (road) {
		for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
			if (!rv->IsFrontEngine() || rv->unitnumber != (UnitID)*punit) continue;
			uint index = 0;
			for (const RoadVehicle *u = rv; u != nullptr; u = u->Next(), index++) {
				IConsolePrint(CC_DEFAULT, "testtvar: auto {:2d} {} delka {} {}poz ({},{},{}) smer {} vezen {} naklad {} {}/{}",
						index, u->IsArticulatedPart() ? "cast " : "hlava",
						u->gcache.cached_veh_length,
						u->IsArticulatedPart() ? "" : fmt::format("(celkem {}) ", u->gcache.cached_total_length),
						u->x_pos, u->y_pos, u->z_pos, (int)u->direction,
						u->carried_by == VehicleID::Invalid() ? -1 : (int)u->carried_by.base(),
						IsValidCargoType(u->cargo_type) ? (int)u->cargo_type : -1, u->cargo.StoredCount(), u->cargo_cap);
			}
			return true;
		}
		IConsolePrint(CC_ERROR, "testtvar: auto {} nenalezeno.", argv[2]);
		return true;
	}

	for (const Train *t : Train::Iterate()) {
		if (!t->IsFrontEngine() || t->unitnumber != (UnitID)*punit) continue;
		uint index = 0;
		for (const Train *u = t; u != nullptr; u = u->Next(), index++) {
			uint pieces = 0, unit_len = 0;
			if (!u->IsArticulatedPart()) {
				for (const Train *p = u; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) {
					pieces++;
					unit_len += p->gcache.cached_veh_length;
				}
			}
			/* A tender made into its engine's rear head (MakeTenderRearHead())
			 * says so, and with whom: it is neither a part nor a wagon, and a
			 * pair that has come apart on load would show here as a tender with
			 * nobody, which is the one way that conversion can quietly fail. */
			std::string pair;
			if (u->flags.Test(VehicleRailFlag::TenderPair)) {
				pair = fmt::format(" par{}{}", u->IsTender() ? " tendr" : " masinka",
						u->other_multiheaded_part == nullptr ? " BEZ PARTNERA" : fmt::format(" s {}", u->other_multiheaded_part->index.base()));
			}
			IConsolePrint(CC_DEFAULT, "testtvar: {:2d} {} delka {} {}poz ({},{},{}) smer {} veze {}{}",
					index, u->IsArticulatedPart() ? "cast " : (u->IsTender() ? "tendr" : ((u->IsEngine() || u->flags.Test(VehicleRailFlag::TenderPair)) ? "masin" : "vagon")),
					u->gcache.cached_veh_length,
					u->IsArticulatedPart() ? "" : fmt::format("(celkem {} v {} kusech) ", unit_len, pieces),
					u->x_pos, u->y_pos, u->z_pos, (int)u->direction,
					u->carrying == VehicleID::Invalid() ? -1 : (int)u->carrying.base(), pair);
		}
		return true;
	}
	IConsolePrint(CC_ERROR, "testtvar: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Everything the drawing of one train is made of, piece by piece: what the
 * piece is and which set it comes from, how long it counts as, where it stands
 * and which way it faces, whether it is flipped or hidden, which piece it draws
 * itself as (a flipped piece of an articulated unit draws its mirror piece,
 * PieceDrawnAs()), the very sprites it puts on the screen, the box they are
 * drawn in, and what it carries. Long on purpose: a picture that has come
 * apart can only be read from the whole of this, and the player cannot say
 * more than "the wagons look wrong".
 *
 * @param t the train or rake, its list head
 */
static void PrintConsistDrawing(const Train *t)
{
	IConsolePrint(CC_WARNING, "testkresba: {} {}: {} clanku, couva {}, celo {}, na ({},{})",
			t->IsFrontEngine() ? "vlak" : "rada", t->IsFrontEngine() ? t->unitnumber : (UnitID)0,
			CountVehiclesInChain(t), t->vehicle_flags.Test(VehicleFlag::DrivingBackwards) ? "ano" : "ne",
			t->GetMovingFront() == t ? "hlava seznamu" : "konec seznamu", TileX(t->tile), TileY(t->tile));
	uint index = 0;
	for (const Train *u = t; u != nullptr; u = u->Next(), index++) {
		/* Its place in its own unit: the k-th of n pieces. */
		const Train *unit = u->GetFirstEnginePart();
		uint n = 0;
		uint k = 0;
		for (const Train *p = unit; ; p = p->GetNextArticulatedPart()) {
			if (p == u) k = n;
			n++;
			if (!p->HasArticulatedPart()) break;
		}
		const Train *drawn = PieceDrawnAs(u);
		VehicleSpriteSeq seq;
		u->GetImage(u->direction, EngineImageType::OnMap, &seq);
		std::string sprites;
		for (uint i = 0; i < seq.count; i++) {
			if (!sprites.empty()) sprites += '+';
			sprites += fmt::format("{}/{}", seq.seq[i].sprite, seq.seq[i].pal);
		}
		const Engine *e = u->GetEngine();
		const char *role = u->IsArticulatedPart() ? "cast" : (u->IsTender() ? "tendr" : (u->IsRearDualheaded() ? "zadni" : (u->IsEngine() ? "masin" : "vagon")));
		std::string kolej = u->track == Track::Depot ? "depo" : (u->track == Track::Wormhole ? "roura" : fmt::format("{:#x}", u->track.base()));
		IConsolePrint(CC_DEFAULT, "testkresba: {:2d} id {} {:5} kus {}/{} typ {} grf {:08X}/{} delka {} poz ({},{},{}) dl ({},{}) kolej {} smer {} preklopen {} schovany {} kresli {} sprajty {} obalka poc ({},{},{}) roz ({},{},{}) pos ({},{},{}) naklad {} {}/{} vezeno {}",
				index, u->index.base(), role, k, n, u->engine_type.base(),
				e->GetGRF() != nullptr ? std::byteswap(e->GetGRF()->grfid) : 0, e->grf_prop.local_id,
				u->gcache.cached_veh_length, u->x_pos, u->y_pos, u->z_pos, TileX(u->tile), TileY(u->tile),
				kolej, to_underlying(u->direction), u->flags.Test(VehicleRailFlag::Flipped) ? "ano" : "ne",
				u->vehstatus.Test(VehState::Hidden) ? "ano" : "ne",
				drawn == u ? "sebe" : fmt::format("jako id {}", drawn->index.base()),
				sprites.empty() ? "nic" : sprites,
				u->bounds.origin.x, u->bounds.origin.y, u->bounds.origin.z,
				u->bounds.extent.x, u->bounds.extent.y, u->bounds.extent.z,
				u->bounds.offset.x, u->bounds.offset.y, u->bounds.offset.z,
				IsValidCargoType(u->cargo_type) ? (int)u->cargo_type : -1, u->cargo.StoredCount(), u->cargo_cap,
				u->carrying == VehicleID::Invalid() ? -1 : (int)u->carrying.base());

		/* And whatever rides on it. A lorry on a wagon is part of the wagon's
		 * picture, so the question asked of it is the same one: does it face
		 * the way the wagon under it is drawn (FollowWagon())? Printed beside
		 * the wagon's own line, because the two are only ever wrong together. */
		if (u->carrying == VehicleID::Invalid()) continue;
		const RoadVehicle *carried = RoadVehicle::GetIfValid(u->carrying);
		if (carried == nullptr) continue;
		Direction drawn_dir = u->flags.Test(VehicleRailFlag::Flipped) ? ReverseDir(u->direction) : u->direction;
		uint part = 0;
		for (const RoadVehicle *c = carried; c != nullptr; c = c->Next(), part++) {
			VehicleSpriteSeq cseq;
			c->GetImage(c->direction, EngineImageType::OnMap, &cseq);
			std::string csprites;
			for (uint i = 0; i < cseq.count; i++) {
				if (!csprites.empty()) csprites += '+';
				csprites += fmt::format("{}/{}", cseq.seq[i].sprite, cseq.seq[i].pal);
			}
			IConsolePrint(c->direction == drawn_dir ? CC_DEFAULT : CC_ERROR,
					"testkresba:      veze auto {} kus {} smer {} (vagon kresleny smer {}) {} poz ({},{},{}) sprajty {}",
					c->index.base(), part, to_underlying(c->direction), to_underlying(drawn_dir),
					c->direction == drawn_dir ? "sedi" : "NESEDI", c->x_pos, c->y_pos, c->z_pos,
					csprites.empty() ? "nic" : csprites);
		}
	}
}

/**
 * The drawing dump for one train, one rake, or everything on rails.
 * Usage: testkresba <unit number> | testkresba <x> <y> | testkresba vse
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDrawing(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Print how a train is drawn, piece by piece. Usage: 'testkresba <unit number>', 'testkresba <x> <y>' (a rake by the tile of its first vehicle) or 'testkresba vse'.");
		return true;
	}
	bool all = argv[1] == "vse";
	auto punit = all ? std::optional<uint32_t>(0) : ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	std::optional<uint64_t> py;
	if (argv.size() == 3) {
		py = ParseInteger(argv[2]);
		if (!py.has_value()) return false;
	}
	bool found = false;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		if (!all && (py.has_value() ? t->tile != TileXY((uint)*punit, (uint)*py) : (!t->IsFrontEngine() || t->unitnumber != (UnitID)*punit))) continue;
		found = true;
		PrintConsistDrawing(t);
		if (!all) return true;
	}
	if (!found) IConsolePrint(CC_ERROR, "testkresba: {} nenalezen.", all ? "zadny vlak" : "vlak");
	return true;
}

/**
 * Make an engine standing in a shed into a unit of several pieces, for the rig:
 * the engine keeps its picture and its place, and gets articulated pieces of
 * its own kind behind it, the way a set's engine drawn in pieces has them.
 *
 * The rig's own sets have no such engine and the player's do, all of them: a
 * body between two invisible stubs, which is the shape that meets the
 * nose-first rule (ConsistCanBeRelinked()). The pieces made here are full
 * length, since only a set can say a piece is short, so the unit is the same
 * shape from either end and every piece shows the engine's own picture -- the
 * list and the ground can be measured on it, the picture cannot.
 *
 * @param t the engine, alone or at the head of its train, stopped in a shed
 * @param pieces how many pieces to add behind it
 * @return whether it was done
 */
static bool MakeEngineOfPieces(Train *t, uint pieces)
{
	if (!t->IsStoppedInDepot() || t->HasArticulatedPart() || t->IsMultiheaded()) return false;
	Train *v = t;
	for (uint i = 0; i < pieces; i++) {
		if (!Vehicle::CanAllocateItem()) return false;
		Train *rest = v->Next();
		Train *p = Train::Create();
		v->SetNext(p);
		if (rest != nullptr) p->SetNext(rest);

		p->subtype = 0;
		p->track = t->track;
		p->railtypes = t->railtypes;
		p->spritenum = t->spritenum;
		p->cargo_type = t->cargo_type;
		p->cargo_cap = 0;
		p->refit_cap = 0;
		p->SetArticulatedPart();

		p->direction = t->direction;
		p->owner = t->owner;
		p->tile = t->tile;
		p->x_pos = t->x_pos;
		p->y_pos = t->y_pos;
		p->z_pos = t->z_pos;
		p->date_of_last_service = t->date_of_last_service;
		p->date_of_last_service_newgrf = t->date_of_last_service_newgrf;
		p->build_year = t->build_year;
		p->vehstatus = t->vehstatus;
		p->vehstatus.Reset(VehState::Stopped);
		p->cargo_subtype = 0;
		p->max_age = CalendarTime::MIN_DATE;
		p->engine_type = t->engine_type;
		p->value = 0;
		p->sprite_cache.sprite_seq.Set(SPR_IMG_QUERY);
		p->random_bits = Random();
		p->UpdatePosition();
		v = p;
	}
	t->ConsistChanged(CCF_ARRANGE);
	InvalidateWindowData(WindowClass::VehicleDepot, t->tile);
	return true;
}

/**
 * Move the deck a carried road vehicle stands on up or down while the game is
 * running, and put every one of them on the new deck at once.
 *
 * How high a wagon's deck is, is not written anywhere -- no wagon says, and
 * every set draws its own at its own height -- so the number is chosen by eye,
 * and the eye is at the screen. This is how the player finds it without a
 * build for each guess: try, look, say the number, and it gets written in.
 * Usage: testpaluba [pixels]
 * @copydoc IConsoleCmdProc
 */
/**
 * Read a number that may have a decimal point in it, into quarters of a pixel.
 *
 * Whole pixels turned out to be too coarse a step to settle the two by-eye
 * numbers with -- the player's own words, "the moves are too big for me" -- and
 * a quarter of a pixel is a real step, not a pretend one: it is the grid the
 * finest zoom is drawn on, so at the zoom he plays at it moves the picture.
 * A comma and a full stop both do, since one of them is what he types.
 *
 * @param txt      what was typed
 * @param[out] out the number, in quarters of a pixel
 * @return whether it was a number at all
 */
static bool ParseFine(std::string_view txt, int &out)
{
	size_t i = 0;
	int sign = 1;
	if (i < txt.size() && (txt[i] == '-' || txt[i] == '+')) {
		if (txt[i] == '-') sign = -1;
		i++;
	}
	int64_t whole = 0;
	bool any = false;
	for (; i < txt.size() && txt[i] >= '0' && txt[i] <= '9'; i++) {
		whole = whole * 10 + (txt[i] - '0');
		any = true;
		if (whole > 10000) return false;
	}
	int64_t thousandths = whole * 1000;
	if (i < txt.size() && (txt[i] == ',' || txt[i] == '.')) {
		i++;
		int64_t scale = 100;
		for (; i < txt.size() && txt[i] >= '0' && txt[i] <= '9'; i++) {
			thousandths += (txt[i] - '0') * scale;
			scale /= 10;
			any = true;
			if (scale == 0) {
				/* Anything finer than a thousandth is thrown away, not refused. */
				while (i + 1 < txt.size() && txt[i + 1] >= '0' && txt[i + 1] <= '9') i++;
				break;
			}
		}
	}
	if (!any || i != txt.size()) return false;
	out = (int)(sign * ((thousandths * (int)ZOOM_BASE + 500) / 1000));
	return true;
}

/** Say a number held in quarters of a pixel the way it was typed. */
static std::string SayFine(int quarters)
{
	std::string text = fmt::format("{:.2f}", (double)quarters / (double)ZOOM_BASE);
	for (char &c : text) {
		if (c == '.') c = ',';
	}
	return text;
}

/**
 * The two by-eye numbers for a carried road vehicle, both of them one per
 * direction: with a number alone every direction is set at once, with a
 * direction in front of it only that one.
 *
 * Eight and not one because the player asked for eight. Whether he needs eight
 * is his eye's business and not this file's: if the placing is right, one
 * number does all of them. The directions are numbered as the game numbers
 * them -- 0 north, 2 east, 4 south, 6 west, the odd ones the slants.
 *
 * @param argv     what was typed
 * @param what     the eight numbers to set, in quarters of a pixel
 * @param name     the command's own name, for the lines it prints
 * @param sentence what one number means, with a {} where the number goes
 * @return whether the command was understood
 */
static bool SetPerDirection(std::span<std::string_view> argv, DirectionIndexArray<int> &what, const char *name, const char *sentence)
{
	static const std::string_view _names[] = { "S", "SV", "V", "JV", "J", "JZ", "Z", "SZ" };
	auto say = [&](uint d) {
		IConsolePrint(CC_DEFAULT, "{}: smer {} ({}) - {}", name, d, _names[d],
				fmt::format(fmt::runtime(sentence), SayFine(what[static_cast<Direction>(d)])));
	};

	/* One direction only, said the way the player says it: the number first and
	 * the direction after it, "testpaluba 6,25 smer 4". The word may have the
	 * number stuck to it -- "smer4" -- because that is how he typed it. */
	if (argv.size() >= 3) {
		int fine = 0;
		if (!ParseFine(argv[1], fine)) return false;
		std::string_view rest = argv[2];
		if (rest.starts_with("smer")) rest.remove_prefix(4);
		if (rest.empty() && argv.size() >= 4) rest = argv[3];
		auto pd = ParseInteger<int>(rest);
		if (!pd.has_value()) {
			IConsolePrint(CC_ERROR, "{}: napis treba '{} 6,25 smer 4'. Smer je 0 sever, 2 vychod, 4 jih, 6 zapad.", name, name);
			return true;
		}
		if (*pd < 0 || *pd >= (int)to_underlying(Direction::End)) {
			IConsolePrint(CC_ERROR, "{}: smer je 0 az 7.", name);
			return true;
		}
		what[static_cast<Direction>(*pd)] = fine;
		RestandCarriedRoadVehicles();
		say((uint)*pd);
		return true;
	}

	if (argv.size() == 2) {
		int fine = 0;
		if (!ParseFine(argv[1], fine)) return false;
		for (uint d = 0; d < to_underlying(Direction::End); d++) what[static_cast<Direction>(d)] = fine;
		RestandCarriedRoadVehicles();
	}
	for (uint d = 0; d < to_underlying(Direction::End); d++) say(d);
	return true;
}

/**
 * Put a bright dot on the point the game holds every vehicle's picture by, or
 * take it away again. Usage: testkotva [0|1]
 *
 * The player's own request, and it is a good one: he lines his cars up against
 * another set's, on a wagon and on a road, and cannot get them to agree in both
 * places. What he cannot see is where each set hangs its picture from, because
 * the picture is all there is. One dot says it.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSpriteAnchor(std::span<std::string_view> argv)
{
	extern bool _show_sprite_anchor;
	if (argv.size() >= 2) {
		auto p = ParseInteger<int>(argv[1]);
		if (!p.has_value()) return false;
		_show_sprite_anchor = *p != 0;
	} else {
		/* Said on its own it flips. Typed with the mouse on the game's own
		 * keyboard, a "1" and a "0" are two more journeys across the screen for
		 * nothing -- the player's point, and a switch with two states does not
		 * need to be told which one to go to. */
		_show_sprite_anchor = !_show_sprite_anchor;
	}
	MarkWholeScreenDirty();
	IConsolePrint(CC_DEFAULT, "kot: oranzovy bod na kotve spritu je {}.", _show_sprite_anchor ? "zapnuty" : "vypnuty");
	return true;
}

static bool ConTestDeckHeight(std::span<std::string_view> argv)
{
	extern DirectionIndexArray<int> _carried_z_offset;
	return SetPerDirection(argv, _carried_z_offset, "pal", "auta stoji {} bodu nad vagonem");
}

/**
 * Trim where across its wagon a carried road vehicle stands, in pixels, while
 * the game is running.
 *
 * The two pictures are already put middle over middle by themselves, and that
 * lands within a pixel or two. This is the pixel or two: a middle is the middle
 * of everything painted, so a mirror or an overhanging load pulls it off to one
 * side. Told in pixels along the way across the rails as the screen sees it --
 * see _carried_side_trim.
 * Usage: testbok [pixels]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSideTrim(std::span<std::string_view> argv)
{
	extern DirectionIndexArray<int> _carried_side_trim;
	return SetPerDirection(argv, _carried_side_trim, "bok", "auta jsou doladena o {} bodu napric vagonem");
}

/**
 * Where a carried road vehicle's picture lands beside its wagon's, across the
 * screen, in each of the eight directions a train can face.
 *
 * The player read these eight numbers off a circle of track, tile by tile, by
 * eye. They can be had without eyes and without a circle, because the game
 * knows exactly where it puts both pictures: the vehicle's position, the
 * offset its own bounding box carries (UpdateDeltaXY(), and a road vehicle's
 * differs from a wagon's by direction), and the offset baked into the sprite
 * the set draws it with. Put together, that is the pixel the picture starts
 * at -- so the gap between the middle of the car and the middle of its wagon
 * is arithmetic, not judgement.
 *
 * Measured on the train as it stands, in whatever direction it faces at that
 * moment -- so it is asked over and over while the train goes round a circle
 * of track and every direction comes up by itself. Turning the train on the
 * spot instead was tried and is worthless: a wagon of several pieces keeps
 * its pieces strung out along the old direction while each is drawn facing
 * the new one, and what comes out is a mixture of the two.
 *
 * Only the way across is worked out. How high the deck is cannot be had this
 * way -- no wagon says where its deck is, which is why 'testpaluba' is chosen
 * by eye in the first place.
 * Usage: testsmery <cislo vlaku>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDirectionGaps(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Say how far the carried vehicle's picture sits beside its wagon's, in all eight directions. Usage: 'testsmery <cislo vlaku>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;

	Train *train = nullptr;
	for (Train *t : Train::Iterate()) {
		if (t->First() == t && t->unitnumber == (UnitID)*punit) { train = t; break; }
	}
	if (train == nullptr) {
		IConsolePrint(CC_ERROR, "testsmery: vlak {} nenalezen.", argv[1]);
		return true;
	}

	auto span = [](const std::vector<const Vehicle *> &parts) {
		int lo = INT_MAX;
		int hi = INT_MIN;
		int bottom = INT_MIN;
		for (const Vehicle *v : parts) {
			Point pt = RemapCoords(v->x_pos + v->bounds.origin.x + v->bounds.offset.x,
					v->y_pos + v->bounds.origin.y + v->bounds.offset.y,
					v->z_pos + v->bounds.origin.z + v->bounds.offset.z);
			pt.x += v->draw_offs.x;
			pt.y += v->draw_offs.y;
			VehicleSpriteSeq seq;
			v->GetImage(v->direction, EngineImageType::OnMap, &seq);
			Rect r;
			seq.GetBounds(&r);
			lo = std::min(lo, pt.x + r.left);
			hi = std::max(hi, pt.x + r.right);
			bottom = std::max(bottom, pt.y + r.bottom);
		}
		return std::make_pair((lo + hi) / 2, bottom);
	};
	auto middle = [&span](const std::vector<const Vehicle *> &parts) { return span(parts).first; };
	auto wheels = [&span](const std::vector<const Vehicle *> &parts) { return span(parts).second; };

	static const std::string_view _names[] = { "S", "SV", "V", "JV", "J", "JZ", "Z", "SZ" };

	/* Every load on the train, not just the first. The player put a car of one
	 * set on a wagon in among cars of another on purpose -- so that the deck is
	 * not chosen to suit a single set's pictures. Two sets landing on the same
	 * two numbers is the whole of the claim being made here. */
	uint found = 0;
	for (Train *wagon = train; wagon != nullptr; wagon = wagon->Next()) {
		if (wagon->carrying == VehicleID::Invalid()) continue;
		RoadVehicle *car = RoadVehicle::GetIfValid(wagon->carrying);
		if (car == nullptr) continue;
		found++;

		/* The wagon's own pieces and no more. A wagon's Next() walks on into the
		 * rest of the train, which would take the whole consist's width; a road
		 * vehicle's walks its own lorry and trailer and stops, which is right. */
		std::vector<const Vehicle *> wagon_parts;
		for (const Train *p = wagon; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) {
			wagon_parts.push_back(p);
		}
		std::vector<const Vehicle *> car_parts;
		for (const RoadVehicle *u = car; u != nullptr; u = u->Next()) car_parts.push_back(u);

		const Direction d = wagon->direction;
		int across = middle(car_parts) - middle(wagon_parts);
		int above = wheels(wagon_parts) - wheels(car_parts);
		IConsolePrint(CC_INFO, "testsmery: smer {} ({}) - auto {} ({}) na vagonu {} je {:+d} bodu vedle jeho stredu, stoji {} bodu nad jeho koly, uhnuto o {:+d}",
				to_underlying(d), _names[to_underlying(d)], car->unitnumber,
				GetString(Engine::Get(car->engine_type)->info.string_id),
				GetString(Engine::Get(wagon->engine_type)->info.string_id),
				across / (int)ZOOM_BASE, above / (int)ZOOM_BASE,
				car->draw_offs.x / (int)ZOOM_BASE);
	}
	if (found == 0) IConsolePrint(CC_ERROR, "testsmery: na vlaku {} nic nestoji.", argv[1]);
	return true;
}

/**
 * Write a note of the player's own into the record, so that what he saw on the
 * screen stands in the log beside what the game wrote at that moment, with the
 * same tick on it. He has been typing his notes at the console and getting
 * "Command not found" back -- which does land in the log, but as an error, at
 * the top of the console's own output rather than among the game's lines.
 * Usage: pozn <whatever you want to say>
 * @copydoc IConsoleCmdProc
 */
static bool ConNote(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Write a note of your own into the record. Usage: 'pozn <text>'.");
		return true;
	}
	std::string text;
	for (size_t i = 1; i < argv.size(); i++) {
		if (!text.empty()) text += ' ';
		text += argv[i];
	}
	LogAnomaly("POZNAMKA HRACE: {}", text);
	IConsolePrint(CC_DEFAULT, "pozn: zapsano do zaznamu.");
	return true;
}

/**
 * Switch the mirror drawing of flipped articulated pieces (PieceDrawnAs()) on
 * or off, so the two can be compared on the same coupling: with it off, the
 * record's picture check (PictureKeptAfterJoin()) says what the old drawing
 * did to a flipped wagon of several pieces. Usage: testzrcadlo [on|off]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestMirrorDrawing(std::span<std::string_view> argv)
{
	extern bool _mirror_flipped_pieces;
	if (argv.size() >= 2) {
		if (argv[1] == "on") _mirror_flipped_pieces = true;
		else if (argv[1] == "off") _mirror_flipped_pieces = false;
		else { IConsolePrint(CC_HELP, "Usage: 'testzrcadlo [on|off]'."); return true; }
		for (Train *t : Train::Iterate()) t->UpdateViewport(true, false);
	}
	IConsolePrint(CC_DEFAULT, "testzrcadlo: preklopeny clanek kresli {}.", _mirror_flipped_pieces ? "svuj zrcadlovy kus (on)" : "sam sebe (off)");
	return true;
}

/**
 * Give a stopped engine articulated pieces, see MakeEngineOfPieces().
 * Usage: testclanky <unit number> [pieces]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestMakePieces(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Make an engine stopped in a depot a unit of several pieces. Usage: 'testclanky <unit number> [pieces, default 2]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	uint pieces = 2;
	if (argv.size() > 2) {
		auto pp = ParseInteger(argv[2]);
		if (!pp.has_value()) return false;
		pieces = (uint)*pp;
	}
	Train *t = FindTrainByUnit((uint)*punit);
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testclanky: vlak {} nenalezen.", argv[1]);
		return true;
	}
	if (!MakeEngineOfPieces(t, pieces)) {
		IConsolePrint(CC_ERROR, "testclanky: vlak {} musi stat v depu a jeho masinka byt z jednoho kusu.", argv[1]);
		return true;
	}
	uint n = 0;
	for (const Train *p = t; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) n++;
	IConsolePrint(CC_DEFAULT, "testclanky: vlak {} - masinka ma {} clanku, celkem {} dilku.", t->unitnumber, n, t->gcache.cached_total_length);
	return true;
}

/**
 * Where a vehicle's picture sits around the point the vehicle is at: the
 * sprite's own size and offsets, for each of the eight directions, and the
 * box the game draws it in.
 *
 * This is the other half of the question testtvar answers. How long a vehicle
 * counts as is one thing; where its picture is hung is another, and a set
 * chooses it freely -- so a set's lorry can sit in a different place on a
 * wagon than the game's own does, with nothing in the lengths to say why.
 * Read without a set and with one, the two answers can be compared.
 * Usage: testobraz auto|vlak <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSpriteOffsets(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Print a vehicle's sprite offsets. Usage: 'testobraz auto|vlak <unit number>'.");
		return true;
	}
	bool road = argv[1] == "auto";
	auto punit = ParseInteger(argv[2]);
	if (!punit.has_value()) return false;

	const Vehicle *found = nullptr;
	if (road) {
		for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
			if (rv->IsFrontEngine() && rv->unitnumber == (UnitID)*punit) { found = rv; break; }
		}
	} else {
		for (const Train *t : Train::Iterate()) {
			if (t->IsFrontEngine() && t->unitnumber == (UnitID)*punit) { found = t; break; }
		}
	}
	if (found == nullptr) {
		IConsolePrint(CC_ERROR, "testobraz: {} {} nenalezeno.", road ? "auto" : "vlak", argv[2]);
		return true;
	}

	uint index = 0;
	for (const Vehicle *u = found; u != nullptr; u = u->Next(), index++) {
		IConsolePrint(CC_DEFAULT, "testobraz: {} {:2d} delka {} obalka poc ({},{},{}) roz ({},{},{}) pos ({},{},{})",
				road ? "auto" : "vuz", index, road ? RoadVehicle::From(u)->gcache.cached_veh_length : Train::From(u)->gcache.cached_veh_length,
				u->bounds.origin.x, u->bounds.origin.y, u->bounds.origin.z,
				u->bounds.extent.x, u->bounds.extent.y, u->bounds.extent.z,
				u->bounds.offset.x, u->bounds.offset.y, u->bounds.offset.z);
		for (uint dir = 0; dir < to_underlying(Direction::End); dir++) {
			Direction d = (Direction)dir;
			VehicleSpriteSeq seq;
			if (road) {
				RoadVehicle::From(u)->GetImage(d, EngineImageType::OnMap, &seq);
			} else {
				Train::From(u)->GetImage(d, EngineImageType::OnMap, &seq);
			}
			if (!seq.IsValid()) continue;
			Rect r;
			seq.GetBounds(&r);
			IConsolePrint(CC_DEFAULT, "testobraz:    smer {} sprajtu {} obrazek {}x{} px, roh ({},{})",
					dir, seq.count, r.right - r.left + 1, r.bottom - r.top + 1, r.left, r.top);
		}
	}
	return true;
}

/**
 * List every vehicle of a train with what it is, for reading a consist
 * headless. Usage: testvozy <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestListUnits(std::span<std::string_view> argv)
{
	if (argv.size() != 2 && argv.size() != 3) {
		IConsolePrint(CC_HELP, "List a train's vehicles. Usage: 'testvozy <unit number>' or 'testvozy <x> <y>' (a rake without a number, by the tile of its first vehicle).");
		return true;
	}
	bool all_rakes = argv[1] == "vse";
	auto punit = all_rakes ? std::optional<uint32_t>(0) : ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	std::optional<uint64_t> py;
	if (argv.size() == 3) {
		py = ParseInteger(argv[2]);
		if (!py.has_value()) return false;
	}
	bool found = false;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		if (all_rakes) {
			if (!t->IsFreeWagon()) continue;
		} else if (py.has_value() ? t->tile != TileXY((uint)*punit, (uint)*py) : t->unitnumber != (UnitID)*punit) {
			continue;
		}
		found = true;
		IConsolePrint(CC_DEFAULT, "vlak {}: {} couva {} rozkaz {} (nakladat {} vykladat {}, c.{}/{}) ceka-na-spojeni {} lhuta-odtahu {}", t->unitnumber, t->IsFrontEngine() ? "masinka v cele" : "bez cela",
				t->vehicle_flags.Test(VehicleFlag::DrivingBackwards) ? "ano" : "ne",
				to_underlying(t->current_order.GetType()),
				to_underlying(t->current_order.GetLoadType()), to_underlying(t->current_order.GetUnloadType()),
				t->cur_real_order_index, t->GetNumOrders(),
				t->current_order.ShouldWaitForCouple() ? "ano" : "ne",
				t->rescue_deadline == TimerGameEconomy::Date{} ? "zadna" : "ano");
		uint i = 0;
		for (const Train *u = t; u != nullptr; u = u->Next(), i++) {
			/* Which way the picture's nose points: the direction, turned by
			 * Flipped, turned again by a reversed sprite set (the rear head of
			 * a dual-headed engine, or a NewGRF's reversed sprite). */
			Direction nose = u->flags.Test(VehicleRailFlag::Flipped) ? ReverseDir(u->direction) : u->direction;
			if (u->spritenum == CUSTOM_VEHICLE_SPRITENUM_REVERSED || (!IsCustomVehicleSpriteNum(u->spritenum) && u->spritenum == RailVehInfo(u->engine_type)->image_index + 1)) nose = ReverseDir(nose);
			/* Where it stands, as the map sees it: the tile, and then the
			 * track word and the pixels, which is what tells a vehicle hidden
			 * in a shed from one out on the line at the same tile -- the
			 * question every fault around a depot door comes down to. */
			const char *kolej = u->track == Track::Depot ? "depo" : (u->track == Track::Wormhole ? "roura" : "trat");
			IConsolePrint(CC_DEFAULT, "  [{}] id {} typ {} {}{}{}{}{}{} na ({},{}) {} {}px ({},{}) smer {} otoceny {} sprite {} nos {} naklad {}/{} {}", i, u->index.base(), u->engine_type.base(),
					u->IsEngine() ? "masinka" : (u->IsWagon() ? "vagon" : "cast"),
					u->IsMultiheaded() ? (u->IsRearDualheaded() ? " (zadni hlava)" : " (predni hlava)") : "",
					u->IsArticulatedPart() ? " (kloub)" : "",
					u->IsFrontEngine() ? " CELO" : "",
					u->IsFreeWagon() ? " VOLNY" : "",
					u->flags.Test(VehicleRailFlag::CoupledHere) ? " SPOJ" : "",
					TileX(u->tile), TileY(u->tile), kolej, u->vehstatus.Test(VehState::Hidden) ? "schovany " : "",
					u->x_pos, u->y_pos, to_underlying(u->direction),
					u->flags.Test(VehicleRailFlag::Flipped) ? "ano" : "ne", u->spritenum, to_underlying(nose), u->cargo.StoredCount(), u->cargo_cap,
					IsValidCargoType(u->cargo_type) ? GetString(CargoSpec::Get(u->cargo_type)->name) : "-");
		}
		if (!all_rakes) return true;
	}
	if (!found) IConsolePrint(CC_ERROR, "testvozy: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Make a headless chain standing out on the line wait to be collected, the
 * way a coupling that leaves the engine inside now does on its own. For
 * replaying a save made before that: the chains in it stand with no order.
 * Usage: testrada <x> <y> | vse
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRakeWait(std::span<std::string_view> argv)
{
	if (argv.size() != 2 && argv.size() != 3) {
		IConsolePrint(CC_HELP, "Make a headless chain wait to be collected. Usage: 'testrada <x> <y>' or 'testrada vse'.");
		return true;
	}
	bool all = argv[1] == "vse";
	TileIndex at = INVALID_TILE;
	if (!all) {
		if (argv.size() != 3) return false;
		auto px = ParseInteger(argv[1]);
		auto py = ParseInteger(argv[2]);
		if (!px.has_value() || !py.has_value()) return false;
		at = TileXY((uint)*px, (uint)*py);
	}
	uint done = 0;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFreeWagon() || t->IsInDepot()) continue;
		if (!all && t->tile != at) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		LeaveHeadlessChainWaiting(t);
		done++;
	}
	if (done == 0) IConsolePrint(CC_ERROR, "testrada: zadna rada venku.");
	return true;
}

/**
 * Build a lone engine in a depot on the map, for staging a scene on a
 * player's save; optionally station it there as a rescue engine, on call.
 * Usage: testpostav <x> <y> <engine type> [odtahovka]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestBuildEngine(std::span<std::string_view> argv)
{
	if (argv.size() != 4 && argv.size() != 5) {
		IConsolePrint(CC_HELP, "Build an engine in a depot. Usage: 'testpostav <x> <y> <engine type> [odtahovka]'.");
		return true;
	}
	auto px = ParseInteger(argv[1]);
	auto py = ParseInteger(argv[2]);
	auto pe = ParseInteger(argv[3]);
	if (!px.has_value() || !py.has_value() || !pe.has_value()) return false;
	TileIndex depot = TileXY((uint)*px, (uint)*py);
	if (!IsRailDepotTile(depot)) {
		IConsolePrint(CC_ERROR, "testpostav: na ({},{}) neni depo.", *px, *py);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, GetTileOwner(depot));
	auto [cost, veh, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, (EngineID)*pe, true, INVALID_CARGO, ClientID::Invalid);
	if (cost.Failed()) {
		IConsolePrint(CC_ERROR, "testpostav: stavba selhala - {}", GetString(cost.GetErrorMessage()));
		return true;
	}
	Train *t = Train::Get(veh);
	std::string extra;
	if (argv.size() == 5 && argv[4] == "odtahovka") {
		CommandCost res = Command<Commands::SetRescueEngine>::Do(DoCommandFlag::Execute, t->index, true);
		if (res.Succeeded()) Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, t->index, false);
		extra = res.Failed() ? fmt::format(" - odtahovka NE: {}", GetString(res.GetErrorMessage())) : " - odtahovka v pohotovosti";
	}
	IConsolePrint(CC_DEFAULT, "testpostav: vlak {} (vozidlo {}) postaven v depu ({},{}){}", t->unitnumber, t->index.base(), *px, *py, extra);
	return true;
}

/**
 * Station an engine as a rescue engine, first taking its orders away -- the
 * player's two clicks in one, for a save whose spare engine has orders.
 * Usage: testodtahovka <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestMakeRescueEngine(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Station a train as a rescue engine. Usage: 'testodtahovka <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		DeleteVehicleOrders(t);
		CommandCost res = Command<Commands::SetRescueEngine>::Do(DoCommandFlag::Execute, t->index, true);
		IConsolePrint(res.Failed() ? CC_ERROR : CC_DEFAULT, "testodtahovka: vlak {} - {}", t->unitnumber,
				res.Failed() ? GetString(res.GetErrorMessage()) : std::string("odtahovka v pohotovosti"));
		return true;
	}
	IConsolePrint(CC_ERROR, "testodtahovka: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Call a tow for a rake of waiting wagons, the same as the player pressing the
 * button in its window. Usage: testodvoz <unit number> | vse
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRequestTow(std::span<std::string_view> argv)
{
	if (argv.size() != 2 && argv.size() != 3) {
		IConsolePrint(CC_HELP, "Call a tow for waiting wagons. Usage: 'testodvoz <unit number>', 'testodvoz <x> <y>' or 'testodvoz vse'.");
		return true;
	}
	bool all = argv[1] == "vse";
	uint unit = 0;
	TileIndex at = INVALID_TILE;
	if (!all) {
		auto punit = ParseInteger(argv[1]);
		if (!punit.has_value()) return false;
		unit = *punit;
		if (argv.size() == 3) {
			auto py = ParseInteger(argv[2]);
			if (!py.has_value()) return false;
			at = TileXY(unit, (uint)*py);
		}
	}
	uint done = 0;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || !IsWaitingWagonChain(t)) continue;
		if (!all && (at != INVALID_TILE ? t->tile != at : t->unitnumber != (UnitID)unit)) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost res = Command<Commands::RequestWagonTow>::Do(DoCommandFlag::Execute, t->index, true, false);
		IConsolePrint(res.Failed() ? CC_ERROR : CC_DEFAULT, "testodvoz: rada {} na ({},{}) - {}", t->unitnumber, TileX(t->tile), TileY(t->tile),
				res.Failed() ? GetString(res.GetErrorMessage()) : std::string("odtah zavolan"));
		done++;
	}
	if (done == 0) IConsolePrint(CC_ERROR, "testodvoz: zadna cekajici rada.");
	return true;
}

/**
 * Refresh every vehicle window the way the game itself does now and then --
 * a livery changed, a setting flipped -- so that a window left pointing at a
 * vehicle which is no longer the head of anything is asked the questions it
 * would be asked in a played game. The rig never moves a mouse and never
 * changes a livery, so without this a stale window sat quiet for a whole run
 * and the crash it holds was only ever seen on the player's screen.
 * Usage: testokna
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRefreshWindows(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Refresh every vehicle window, as the game does on a livery change. Usage: 'testokna'.");
		return true;
	}
	/* Said first, before the refresh, because the refresh is what a stale
	 * window falls over on: which vehicle windows are open, and whether the
	 * vehicle each one follows is still the head of anything. A window on a
	 * vehicle that is not is the fault this command exists to provoke. */
	for (const Window *w : Window::Iterate()) {
		if (w->window_class != WindowClass::VehicleView) continue;
		const Vehicle *v = Vehicle::GetIfValid(static_cast<VehicleID>(w->window_number));
		if (v == nullptr) {
			IConsolePrint(CC_DEFAULT, "testokna: okno vozidla {} - vozidlo uz neni", static_cast<int>(w->window_number));
			continue;
		}
		bool head = v == v->First();
		IConsolePrint(head ? CC_DEFAULT : CC_ERROR, "testokna: okno vozidla {} (vlak {}) - {}", v->index.base(),
				v->First()->unitnumber, head ? "hlava" : "NENI HLAVA, okno zustalo na stare hlave");
		if (!head) LogAnomaly("Okno vozidla {} zustalo na clanku, ktery uz neni hlavou vlaku {}", v->index.base(), v->First()->unitnumber);
	}
	/* And the same question of every order list: the vehicle it names as the
	 * first of those sharing it. That is the vehicle the stale-link sweep of
	 * cargo distribution (DeleteStaleLinks()) asks IsStoppedInDepot(), a
	 * question only a head may be asked -- and a list still naming a vehicle
	 * whose train has since got another head is the same fault as a stale
	 * window, only found by the game itself rather than by the player's
	 * screen. */
	for (const OrderList *l : OrderList::Iterate()) {
		const Vehicle *v = l->GetFirstSharedVehicle();
		if (v == nullptr) continue;
		if (v == v->First()) continue;
		IConsolePrint(CC_ERROR, "testokna: rozkaznik vlaku {} jmenuje prvni sdilene vozidlo {}, ktere NENI HLAVA", v->First()->unitnumber, v->index.base());
		LogAnomaly("Rozkaznik jmenuje prvni sdilene vozidlo {}, ktere uz neni hlavou vlaku {}", v->index.base(), v->First()->unitnumber);
	}
	InvalidateWindowClassesData(WindowClass::VehicleView);
	InvalidateWindowClassesData(WindowClass::VehicleDetails);
	InvalidateWindowClassesData(WindowClass::VehicleOrders);
	IConsolePrint(CC_DEFAULT, "testokna: okna vozidel obnovena.");
	return true;
}

/**
 * Say what every rake standing and waiting is loading for: the orders it was
 * left with, and the answer the game gets when it asks that rake which
 * stations it will stop at -- which is what a station hands its cargo out by.
 *
 * Both halves matter and they are not the same question. A rake gets two
 * orders when it is put down, both naming the platform it is standing on, and
 * the walk that answers "which stations will you stop at" skips every order
 * naming the station the vehicle is already at. So those two answer nothing,
 * and with cargo distribution on the station hands such a rake only the cargo
 * it never found a route for. A third order naming somewhere else is the
 * answer, and this is how to see whether it is there and whether the walk
 * finds it. Usage: testcil
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRakeCargoDest(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Say what each waiting rake is loading for. Usage: 'testcil'.");
		return true;
	}
	uint seen = 0;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFreeWagon()) continue;
		seen++;
		std::vector<StationID> next;
		t->GetNextStoppingStation(next);
		std::string stops;
		for (StationID s : next) {
			if (!stops.empty()) stops += ", ";
			stops += fmt::format("{}", s);
		}
		if (stops.empty()) stops = "zadna";
		IConsolePrint(CC_DEFAULT, "testcil: rada {} na ({},{}) - rozkazu {}, rozkaz c.{}, ceka {}, zastavky pro nakladku: {}",
				t->index.base(), TileX(t->tile), TileY(t->tile), t->GetNumOrders(), t->cur_real_order_index,
				t->current_order.ShouldWaitForCouple() ? "ano" : "ne", stops);
		/* Into the record, so the battery's own counter catches it: a rake
		 * carrying the third order and still answering nothing is the one way
		 * this can quietly stop working. It did exactly that once already --
		 * the order was written as "load nothing, unload nothing" and the walk
		 * stepped over it -- and nothing but this line would have said so. */
		if (t->GetNumOrders() > 2 && next.empty()) {
			LogAnomaly("Rada {}: ma rozkaz s cilem nakladky, ale zadnou zastavku pro nakladku nehlasi", t->index.base());
		}
	}
	if (seen == 0) IConsolePrint(CC_ERROR, "testcil: zadna odpojena rada.");
	return true;
}

/**
 * Send a train to a depot, the way the button in its window does.
 *
 * The rig had no way to press that button, so what a train does when the
 * player calls it in could only be guessed at from the code -- and what it
 * did was stand still with its window saying it was on its way.
 *
 * Usage: testdodepa <unit number> [1 for "service only"]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSendToDepot(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Send a train to a depot. Usage: 'testdodepa <unit number> [1 = jen servis]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	bool service = argv.size() >= 3 && ParseInteger(argv[2]).value_or(0) != 0;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::SendVehicleToDepot>::Do(DoCommandFlag::Execute, t->index,
				service ? DepotCommandFlags{DepotCommandFlag::Service} : DepotCommandFlags{}, VehicleListIdentifier{});
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testdodepa: vlak {} -> depo ({}) - {}", *punit,
				service ? "servis" : "zastavit", r.Succeeded() ? std::string("poslano") : RefusalReason(r));
		return true;
	}
	IConsolePrint(CC_ERROR, "testdodepa: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Refit every wagon of a train, or of a headless rake, to road vehicles
 * (CT_ROLA), as the refit window does it. The train has to be stopped in a
 * depot, as for any refit; a rake in a depot needs nothing. With a cargo
 * number instead, it is refitted to that cargo, which is how the fitting
 * comes off again -- the rig's way of putting a wagon back the way a save
 * made before the fitting existed has it.
 * Usage: testnaauta <unit number> [cargo]   (0 for the first headless rake in a depot)
 * @copydoc IConsoleCmdProc
 */
static bool ConTestFitForRoadVehicles(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Refit a train's wagons to road vehicles. Usage: 'testnaauta <unit number> [naklad]' (0 = first headless rake in a depot; naklad = refit to that cargo instead).");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	CargoType to = _road_vehicle_cargo;
	bool back_to_own = false;
	if (argv.size() >= 3) {
		/* "puvodni": whatever the first wagon was built to carry, which is the
		 * one cargo a scene can be sure the wagon takes in any climate. */
		if (argv[2] == "puvodni") {
			back_to_own = true;
		} else {
			auto pcargo = ParseInteger(argv[2]);
			if (!pcargo.has_value() || *pcargo >= NUM_CARGO) return false;
			to = (CargoType)*pcargo;
		}
	}
	for (Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		if (*punit == 0 ? !(t->IsFreeWagon() && t->track == Track::Depot) : (!t->IsFrontEngine() || t->unitnumber != (UnitID)*punit)) continue;
		if (back_to_own) {
			/* The wagon's own cargo -- or, where the wagon is a car carrier by
			 * birth (the rig's toyland one is), the first other cargo it takes,
			 * so that "back to its own" really does take the fitting off. */
			for (const Train *u = t; u != nullptr; u = u->Next()) {
				if (RailVehInfo(u->engine_type)->railveh_type != RailVehicleType::Wagon) continue;
				const Engine *e = Engine::Get(u->engine_type);
				to = e->GetDefaultCargoType();
				if (to == _road_vehicle_cargo || !IsValidCargoType(to)) {
					for (const CargoSpec *cs : CargoSpec::Iterate()) {
						if (cs->Index() != _road_vehicle_cargo && e->info.refit_mask.Test(cs->Index())) {
							to = cs->Index();
							break;
						}
					}
				}
				break;
			}
		}
		AutoRestoreBackup cur_company(_current_company, t->owner);
		auto [r, cap, mail_cap, caps] = Command<Commands::RefitVehicle>::Do(DoCommandFlag::Execute, t->index, to, 0, false, false, 0);
		uint fitted = 0;
		for (const Train *u = t; u != nullptr; u = u->Next()) if (u->cargo_type == _road_vehicle_cargo && u->cargo_cap > 0) fitted++;
		/* A refusal is written in capitals so that the battery can count it:
		 * the fitting being refused through a train's front is a fault this
		 * scene exists to catch, and nothing else in the run would notice. */
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testnaauta: vlak {} na {} - {}; vagonu na auta {}", t->unitnumber,
				to == _road_vehicle_cargo ? std::string("auta") : fmt::format("naklad {}", to),
				r.Succeeded() ? std::string("prestaveno") : fmt::format("ODMITNUTO: {}", RefusalReason(r)), fitted);
		return true;
	}
	IConsolePrint(CC_ERROR, "testnaauta: vlak {} nenalezen.", argv[1]);
	return true;
}

static bool ConTestToggleBrake(std::span<std::string_view> argv)
{
	if (argv.size() < 2 || argv.size() > 3) {
		IConsolePrint(CC_HELP, "Toggle a vehicle's hand brake. Usage: 'testbrzda <unit number>' or 'testbrzda auto <unit number>'.");
		return true;
	}
	/* Road vehicles have hand brakes too, and their numbers are a series of
	 * their own -- so which is meant has to be said: 'testbrzda auto 3'. */
	bool road = argv[1] == "auto";
	auto punit = ParseInteger(argv[road ? 2 : 1]);
	if (!punit.has_value()) return false;

	if (road) {
		for (RoadVehicle *rv : RoadVehicle::Iterate()) {
			if (!rv->IsFrontEngine() || rv->unitnumber != (UnitID)*punit) continue;
			AutoRestoreBackup cur_company(_current_company, rv->owner);
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, rv->index, false);
			IConsolePrint(CC_DEFAULT, "testbrzda: auto {} prepnuto.", rv->unitnumber);
			return true;
		}
		IConsolePrint(CC_ERROR, "testbrzda: auto {} nenalezeno.", argv[2]);
		return true;
	}

	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		/* Fired from the heartbeat timer there is no acting company set, and
		 * the command would bounce off its ownership check, silently. */
		AutoRestoreBackup cur_company(_current_company, t->owner);
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, t->index, false);
		return true;
	}
	IConsolePrint(CC_ERROR, "testbrzda: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Print a road vehicle's orders and what its live order says, so that a
 * savegame can be checked for the live order having lost what the list still
 * has. Usage: testauta [unit number]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRoadOrders(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	std::optional<uint64_t> punit;
	if (argv.size() >= 2) {
		punit = ParseInteger(argv[1]);
		if (!punit.has_value()) return false;
	}
	for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
		if (!rv->IsFrontEngine()) continue;
		if (punit.has_value() && rv->unitnumber != (UnitID)*punit) continue;
		IConsolePrint(CC_DEFAULT, "auto {}: zivy rozkaz typ {} kam {} nakladani {}; c.{} z {} rozkazu; posledni stanice {} ceka-na-vlak {} vezen {}",
				rv->unitnumber, to_underlying(rv->current_order.GetType()),
				rv->current_order.IsType(OT_GOTO_STATION) ? (int)rv->current_order.GetDestination().ToStationID().base() : -1,
				to_underlying(rv->current_order.GetBoardMode()),
				rv->cur_real_order_index, rv->GetNumOrders(),
				rv->last_station_visited == StationID::Invalid() ? -1 : (int)rv->last_station_visited.base(),
				IsWaitingToBoardTrain(rv) ? "ano" : "ne", rv->IsCarried() ? "ano" : "ne");
		VehicleOrderID i = 0;
		for (const Order &o : rv->Orders()) {
			IConsolePrint(CC_DEFAULT, "  {}{}: typ {} kam {} nakladani {}", i == rv->cur_real_order_index ? "*" : " ", i,
					to_underlying(o.GetType()), o.IsType(OT_GOTO_STATION) ? (int)o.GetDestination().ToStationID().base() : -1,
					to_underlying(o.GetBoardMode()));
			i++;
		}
	}

	/* And what there is to ride in: every ship and aircraft fitted for cars,
	 * because "the car is still waiting" is answered as often by the vessel as
	 * by the car -- where it is, whether it is standing at a station at all,
	 * and how full it is. */
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Ship && v->type != VehicleType::Aircraft) continue;
		if (!v->IsPrimaryVehicle()) continue;
		uint aboard = 0;
		for (const RoadVehicle *rv : RoadVehicle::Iterate()) {
			if (rv->IsFrontEngine() && rv->carried_by == v->index) aboard++;
		}
		IConsolePrint(CC_DEFAULT, "{} {}: rozkaz typ {} kam {}, posledni stanice {}, naklad {} kapacita {}, aut {}, stoji {}",
				v->type == VehicleType::Ship ? "lod" : "letadlo", v->unitnumber,
				to_underlying(v->current_order.GetType()),
				v->current_order.IsType(OT_GOTO_STATION) ? (int)v->current_order.GetDestination().ToStationID().base() : -1,
				v->last_station_visited == StationID::Invalid() ? -1 : (int)v->last_station_visited.base(),
				v->cargo_type == _road_vehicle_cargo ? "auta" : "jiny", v->cargo_cap, aboard,
				v->vehstatus.Test(VehState::Stopped) ? "ano" : "ne");
	}
	return true;
}

/**
 * Switch a train's decouple order to "drop the whole coupled train", the way
 * the button in the count box does. Usage: testcelyvlak <unit number> <order>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDecoupleWhole(std::span<std::string_view> argv)
{
	if (argv.size() != 3) {
		IConsolePrint(CC_HELP, "Make a decouple order drop the whole coupled train. Usage: 'testcelyvlak <unit number> <order index>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_DECOUPLE_WHOLE, 1);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testcelyvlak: vlak {} rozkaz {} -> odpojit cely vlak {}", *punit, *porder, r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testcelyvlak: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Make a decouple order sell what it puts down, the way the button at the
 * bottom right of the orders window does. Usage: testprodatvagonky <unit
 * number> <order index> [0|1]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSellDecoupled(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Make a decouple order sell what it puts down. Usage: 'testprodatvagonky <cislo vlaku> <rozkaz> [0|1]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	uint32_t on = 1;
	if (argv.size() >= 4) {
		auto pon = ParseInteger(argv[3]);
		if (!pon.has_value()) return false;
		on = *pon;
	}
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_SELL_DECOUPLED, on);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testprodatvagonky: vlak {} rozkaz {} -> odpojene prodat {} {}", *punit, *porder, on, r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testprodatvagonky: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Open a train's orders window and say how tall it is and how many orders fit
 * in the list, with the couple filter row off and on. Usage: 'testoknorozkazu
 * <cislo vlaku> <rozkaz>'
 *
 * The rig's only way of looking at that. A row appearing in a window either
 * makes the window taller or takes the room from whatever in it can stretch --
 * here the order list -- and which of the two it does is invisible to every
 * counter. The player had it the wrong way round: switching the filter on cost
 * him a line of orders and moved everything above it up.
 * @copydoc IConsoleCmdProc
 */
/**
 * Say whether the refit button of an order is there and whether it can be
 * pressed. Usage: testprestavba <cislo vlaku> <rozkaz>
 *
 * Greying is a thing no counter can see, and this one has been changed twice
 * on guesswork already. A shed's refit and a platform's are two different
 * buttons in two different rows, so which of them is showing is worth saying
 * as well.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRefitButton(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Say whether an order's refit button can be pressed. Usage: 'testprestavba <cislo vlaku> <rozkaz>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;

	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		const Order *o = t->GetOrder((VehicleOrderID)*porder);
		if (o == nullptr) {
			IConsolePrint(CC_ERROR, "testprestavba: rozkaz {} neexistuje.", *porder);
			return true;
		}
		AutoRestoreBackup cur_company(_current_company, t->owner);
		ShowOrdersWindow(t);
		Window *w = FindWindowById(WindowClass::VehicleOrders, t->index);
		if (w == nullptr) {
			IConsolePrint(CC_ERROR, "testprestavba: ODMITNUTO - okno rozkazu se neotevrelo.");
			return true;
		}
		/* The window asks about the order the player has picked, and one that
		 * has just opened has picked nothing. */
		if (!TestSelectOrderInWindow(w, (int)*porder)) {
			IConsolePrint(CC_ERROR, "testprestavba: ODMITNUTO - rozkaz {} v okne nejde vybrat.", *porder);
			return true;
		}

		/* The two refits sit in two different rows and only one row is shown
		 * at a time, so the one belonging to the other kind of order keeps
		 * whatever state it was last left in and means nothing. Said outright,
		 * because read without that it looks like an answer. */
		auto say = [&](const char *name, WidgetID id, bool shown) {
			const NWidgetCore *wid = w->GetWidget<NWidgetCore>(id);
			if (wid == nullptr) {
				IConsolePrint(CC_INFO, "testprestavba:   {} - v okne neni", name);
				return;
			}
			if (!shown) {
				IConsolePrint(CC_INFO, "testprestavba:   {} - u tohohle rozkazu neni videt", name);
				return;
			}
			IConsolePrint(CC_INFO, "testprestavba:   {} - {}", name, w->IsWidgetDisabled(id) ? "ZATMAVENO" : "jde zmacknout");
		};
		IConsolePrint(CC_INFO, "testprestavba: vlak {} rozkaz {} ({})", t->unitnumber, *porder,
				o->IsType(OT_GOTO_DEPOT) ? "depo" : (o->IsType(OT_GOTO_STATION) ? "stanice" : "jine"));
		say("prestavet v depu", WID_O_REFIT, o->IsType(OT_GOTO_DEPOT));
		say("prestavet ve stanici", WID_O_REFIT_DROPDOWN, o->IsType(OT_GOTO_STATION));
		if (o->IsType(OT_GOTO_DEPOT)) {
			IConsolePrint(CC_INFO, "testprestavba:   nabizi naklady: {}", RefitOfferForOrder(t, (VehicleOrderID)*porder));
		}
		return true;
	}
	IConsolePrint(CC_ERROR, "testprestavba: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Say which of the two ways of reading the couple filters an order's window
 * shows pressed: find a rake like this, or search the rake for this.
 * Usage: testrezim <cislo vlaku> <rozkaz>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCoupleMode(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Say which couple reading an order's window shows pressed. Usage: 'testrezim <cislo vlaku> <rozkaz>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		const Order *o = t->GetOrder((VehicleOrderID)*porder);
		if (o == nullptr) {
			IConsolePrint(CC_ERROR, "testrezim: rozkaz {} neexistuje.", *porder);
			return true;
		}
		AutoRestoreBackup cur_company(_current_company, t->owner);
		ShowOrdersWindow(t);
		Window *w = FindWindowById(WindowClass::VehicleOrders, t->index);
		if (w == nullptr) {
			IConsolePrint(CC_ERROR, "testrezim: ODMITNUTO - okno rozkazu se neotevrelo.");
			return true;
		}
		if (!TestSelectOrderInWindow(w, (int)*porder)) {
			IConsolePrint(CC_ERROR, "testrezim: ODMITNUTO - rozkaz {} v okne nejde vybrat.", *porder);
			return true;
		}
		const NWidgetStacked *mid = w->GetWidget<NWidgetStacked>(WID_O_SEL_DECOUPLE_DEST_BTN);
		const NWidgetStacked *right = w->GetWidget<NWidgetStacked>(WID_O_SEL_SELL_WAGONS);
		bool shown = mid != nullptr && right != nullptr && mid->shown_plane == 2 && right->shown_plane == 2;
		if (!shown) {
			IConsolePrint(CC_INFO, "testrezim: vlak {} rozkaz {} - cudliky Najdi/Hledej nejsou videt (rozkaz {})", t->unitnumber, *porder,
					!o->ShouldGoToCouple() ? "nepripojuje" : (o->IsType(OT_GOTO_DEPOT) ? "je depo" : (o->IsType(OT_GOTO_STATION) ? "je stanice, a presto ne" : "neni stanice")));
			return true;
		}
		IConsolePrint(CC_INFO, "testrezim: vlak {} rozkaz {} - Najdi radu {}, Hledej v rade {}, rozkaz rika {}", t->unitnumber, *porder,
				w->IsWidgetLowered(WID_O_COUPLE_FIND) ? "STISKNUTO" : "volne",
				w->IsWidgetLowered(WID_O_COUPLE_SEARCH) ? "STISKNUTO" : "volne",
				o->ShouldSearchInRake() ? "hledej" : "najdi");
		return true;
	}
	IConsolePrint(CC_ERROR, "testrezim: vlak {} nenalezen.", argv[1]);
	return true;
}

static bool ConTestOrderWindowGrows(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Measure the orders window as the couple filter comes and goes. Usage: 'testoknorozkazu <cislo vlaku> <rozkaz>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;

	Train *t = nullptr;
	for (Train *u : Train::Iterate()) {
		if (u->First() == u && u->IsFrontEngine() && u->unitnumber == (UnitID)*punit) {
			t = u;
			break;
		}
	}
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testoknorozkazu: vlak {} nenalezen.", argv[1]);
		return true;
	}

	AutoRestoreBackup cur_company(_current_company, t->owner);
	ShowOrdersWindow(t);
	Window *w = FindWindowById(WindowClass::VehicleOrders, t->index);
	if (w == nullptr) {
		IConsolePrint(CC_ERROR, "testoknorozkazu: ODMITNUTO - okno rozkazu se neotevrelo.");
		return true;
	}

	auto say = [&](const char *when) {
		const NWidgetBase *list = w->GetWidget<NWidgetBase>(WID_O_ORDER_LIST);
		const NWidgetBase *row = w->GetWidget<NWidgetBase>(WID_O_SEL_COUPLE_FILTER);
		IConsolePrint(CC_INFO, "testoknorozkazu: {} - okno vysoke {}, seznam vysoky {}, radek filtru {}",
				when, w->height, list != nullptr ? (int)list->current_y : -1, row != nullptr ? (int)row->current_y : -1);
	};

	/* The window asks about the order the player has picked, and a window that
	 * has just opened has picked nothing. Click the first line of the list, the
	 * way he would. */
	TestSelectOrderInWindow(w, 0);

	/* Taller than its smallest, which is the window the player has: he has
	 * dragged it out or it has grown with his orders. At its smallest the row
	 * has nowhere to come from but the window's own height, so both the old
	 * behaviour and the new one look the same and the scene measures nothing. */
	ResizeWindow(w, 0, 40, false, false);

	say("filtr vypnuty");
	/* Switch the order to collecting, which is what puts the row in. */
	CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_GOTO_COUPLE, 1);
	if (r.Failed()) IConsolePrint(CC_ERROR, "testoknorozkazu: ODMITNUTO - prikaz nejde prepnout na pripojit: {}", GetString(r.GetErrorMessage()));
	w->OnInvalidateData();
	w->OnMouseLoop();
	say("filtr zapnuty");
	Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_GOTO_COUPLE, 0);
	w->OnInvalidateData();
	w->OnMouseLoop();
	say("filtr zase vypnuty");
	return true;
}

/**
 * Open the wagon list for a depot couple order and press its button, the way
 * the player does. Usage: testvybervagonu <unit number> <order index>
 *
 * The rig's only look into that window. It was written after the player found
 * by hand that the window could open with no button in it at all -- pressed
 * from a station order, which has no depot behind it, so there was nothing to
 * answer with and clicking a wagon did nothing.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestPickWagon(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Open the wagon list for an order and press its button. Usage: 'testvybervagonu <cislo vlaku> <rozkaz>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		const Order *o = t->GetOrder((VehicleOrderID)*porder);
		if (o == nullptr) {
			IConsolePrint(CC_ERROR, "testvybervagonu: rozkaz {} neexistuje.", *porder);
			return true;
		}
		AutoRestoreBackup cur_company(_current_company, t->owner);
		const Depot *depot = o->IsType(OT_GOTO_DEPOT) ? Depot::GetIfValid(o->GetDestination().ToDepotID()) : nullptr;
		TestPickCoupleWagon(t, (VehicleOrderID)*porder, depot != nullptr ? depot->xy : INVALID_TILE, o->GetCoupleCargo());
		return true;
	}
	IConsolePrint(CC_ERROR, "testvybervagonu: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Set a couple order's count -- how many vehicles the rake it collects has to
 * have. Usage: testpocet <unit number> <order index> <count>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCoupleCount(std::span<std::string_view> argv)
{
	if (argv.size() < 4) {
		IConsolePrint(CC_HELP, "Set a couple order's count. Usage: 'testpocet <cislo vlaku> <rozkaz> <pocet>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	auto pcount = ParseInteger(argv[3]);
	if (!punit.has_value() || !porder.has_value() || !pcount.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_COUNT, *pcount);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testpocet: vlak {} rozkaz {} -> sebrat {} vozu {}", *punit, *porder, *pcount,
				r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testpocet: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Tell a depot couple order which wagon to buy when the shed is short of them,
 * the way the button in the filter row does. Usage: testkoupit <unit number>
 * <order index> [<engine id>|vypnout|jentyp]
 *
 * Picking a type only names it; this probe then switches buying on, as the
 * player does with "Koupit" in the list. "jentyp" stops after the pick and
 * refuses (ODMITNUTO) if the pick alone switched buying on.
 *
 * With no engine given it takes the first wagon this company can buy, which is
 * what a scene wants: any wagon will do, the question being measured is whether
 * it buys at all and how many.
 * @copydoc IConsoleCmdProc
 */
/**
 * Say what a depot collecting order sees in its shed and why it takes it or
 * leaves it. Usage: testdepo <cislo vlaku> <rozkaz>
 *
 * The player's own case: the order bought wagons, they piled up in the shed
 * and none of them was ever coupled, and nothing he could change helped --
 * because nothing he could change was the reason. Five filters can turn a rake
 * down and no counter says which did.
 * @copydoc IConsoleCmdProc
 */
/**
 * Ask the type filter about every wagon of a train, naming that wagon's own
 * model. The answer has to be yes; it was no for every wagon a set builds out
 * of several pieces. Usage: testtypfiltr <cislo vlaku>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestTypeFilter(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Ask the type filter about each wagon, naming its own model. Usage: 'testtypfiltr <cislo vlaku>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		for (Train *u = t; u != nullptr; u = u->GetNextUnit()) {
			if (u->IsEngine() || u->IsTender() || u->IsRearDualheaded()) continue;
			uint pieces = 0;
			for (const Train *p = u; p != nullptr; p = p->HasArticulatedPart() ? p->GetNextArticulatedPart() : nullptr) pieces++;
			bool ok = CoupleTypeFilterWouldTake(u, u->engine_type);
			/* The filter is asked of a whole chain and a wagon in the middle of
			 * a train drags everything behind it into the question, so only the
			 * last one is a clean answer -- and it is the one that matters, a
			 * single wagon of several pieces standing on its own. */
			IConsolePrint(ok ? CC_INFO : CC_ERROR, "testtypfiltr: vagon {} ({}), model {}, kusu {} - filtr by radu od nej dozadu {}",
					u->index.base(), GetString(Engine::Get(u->engine_type)->info.string_id),
					u->engine_type.base(), pieces, ok ? "VZAL" : "NEVZAL");
		}
		return true;
	}
	IConsolePrint(CC_ERROR, "testtypfiltr: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Say whether a train would be told it is losing money, and which towns its
 * orders name. Usage: testvarovani <cislo vlaku>
 *
 * The rule is the player's: a train that never leaves one town is working a
 * yard and is meant to cost money, one that goes from town to town is a
 * carrier and is meant to pay. Nothing in the game shows which of the two it
 * thinks a train is, and the warning itself comes once a year.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestLossWarning(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Say whether a train would be warned about losses. Usage: 'testvarovani <cislo vlaku>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		std::string towns;
		for (const Order &o : t->Orders()) {
			const Town *town = nullptr;
			if (o.IsType(OT_GOTO_STATION) || o.IsType(OT_GOTO_WAYPOINT)) {
				const BaseStation *st = BaseStation::GetIfValid(o.GetDestination().ToStationID());
				if (st != nullptr) town = st->town;
			} else if (o.IsType(OT_GOTO_DEPOT) && !o.GetDepotActionType().Test(OrderDepotActionFlag::NearestDepot)) {
				const Depot *dep = Depot::GetIfValid(o.GetDestination().ToDepotID());
				if (dep != nullptr) town = dep->town;
			}
			if (town == nullptr) continue;
			std::string name = GetString(STR_TOWN_NAME, town->index);
			if (towns.find(name) == std::string::npos) {
				if (!towns.empty()) towns += ", ";
				towns += name;
			}
		}
		bool warn = WorthWarningAboutLosses(t);
		IConsolePrint(CC_INFO, "testvarovani: vlak {} - mesta v rozkazech: {} - {}", t->unitnumber,
				towns.empty() ? "zadne" : towns,
				warn ? "VAROVALO BY" : "mlci, dela v jednom meste");
		return true;
	}
	IConsolePrint(CC_ERROR, "testvarovani: vlak {} nenalezen.", argv[1]);
	return true;
}

static bool ConTestExplainDepot(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Say what a depot couple order sees in its shed. Usage: 'testdepofiltr <cislo vlaku> <rozkaz>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		ExplainDepotCoupling(t, (VehicleOrderID)*porder);
		return true;
	}
	IConsolePrint(CC_ERROR, "testdepo: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Set the reliability of every train's engine, so a scene about where trains
 * break down has breakdowns to count. Usage: testspolehlivost <percent>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestReliability(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Set every train's reliability. Usage: 'testspolehlivost <procenta>'.");
		return true;
	}
	auto pct = ParseInteger(argv[1]);
	if (!pct.has_value()) return false;
	uint n = 0;
	for (Train *t : Train::Iterate()) {
		if (!t->IsFrontEngine()) continue;
		t->reliability = ClampTo<uint16_t>(std::min<uint>(*pct, 100) * 0xFFFF / 100);
		n++;
	}
	IConsolePrint(CC_INFO, "testspolehlivost: {} vlaku na {}%", n, *pct);
	return true;
}

/**
 * Fill the shed a depot order names with stored wagons, or empty some out of
 * it, the way the player's own hands in the depot window would -- so past the
 * limit orders are held to. For the full-shed scenes.
 * Usage: testdepovagony <unit number> <order> <count> [jiny]
 * A positive count buys that many (the game strings them onto a chain of
 * their own model, as it does for the player); "jiny" buys a
 * model other than the one the order names, so the order will not take them.
 * A negative count sells that many of the stored ones.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDepotWagons(std::span<std::string_view> argv)
{
	if (argv.size() < 4) {
		IConsolePrint(CC_HELP, "Fill or empty the shed a depot order names. Usage: 'testdepovagony <cislo vlaku> <rozkaz> <pocet|-pocet> [jiny]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	auto pcount = ParseInteger<int>(argv[3]);
	if (!punit.has_value() || !porder.has_value() || !pcount.has_value()) return false;
	bool other = argv.size() >= 5 && argv[4] == "jiny";

	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		const Order *o = t->GetOrder((VehicleOrderID)*porder);
		if (o == nullptr || !o->IsType(OT_GOTO_DEPOT)) {
			IConsolePrint(CC_ERROR, "testdepovagony: rozkaz {} vlaku {} nevede do depa.", *porder, *punit);
			return true;
		}
		const Depot *depot = Depot::GetIfValid(o->GetDestination().ToDepotID());
		if (depot == nullptr) return true;
		AutoRestoreBackup cur_company(_current_company, t->owner);

		int done = 0;
		if (*pcount > 0) {
			EngineID eid = EngineID::Invalid();
			for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
				if (!e->company_avail.Test(t->owner)) continue;
				if (e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon) continue;
				if (other && e->index == o->GetCoupleBuyEngine()) continue;
				eid = e->index;
				break;
			}
			if (eid == EngineID::Invalid()) {
				IConsolePrint(CC_ERROR, "testdepovagony: zadny vagon k dispozici.");
				return true;
			}
			for (int i = 0; i < *pcount; i++) {
				auto [cost, id, a, b, c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot->xy, eid, false, INVALID_CARGO, ClientID::Invalid);
				if (cost.Failed()) break;
				done++;
			}
		} else {
			/* One wagon at a time, the head of a stored chain: the game strings
			 * wagons bought into a shed onto a chain of their own model, so
			 * selling whole chains would take far more than asked. */
			for (int i = 0; i < -*pcount; i++) {
				VehicleID id = VehicleID::Invalid();
				for (const Train *rake : Train::Iterate()) {
					if (!rake->IsFreeWagon() || rake->track != Track::Depot || rake->tile != depot->xy) continue;
					id = rake->index;
					break;
				}
				if (id == VehicleID::Invalid()) break;
				if (Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, id, false, false, ClientID::Invalid).Failed()) break;
				done--;
			}
		}

		uint stored = 0;
		for (const Train *rake : Train::Iterate()) {
			if (!rake->IsFreeWagon() || rake->track != Track::Depot || rake->tile != depot->xy) continue;
			for (const Train *u = rake; u != nullptr; u = u->GetNextUnit()) stored++;
		}
		IConsolePrint(CC_INFO, "testdepovagony: depo ({},{}) {} {} vagonu, stoji v nem {}", TileX(depot->xy), TileY(depot->xy),
				done >= 0 ? "koupeno" : "prodano", done >= 0 ? done : -done, stored);
		return true;
	}
	IConsolePrint(CC_ERROR, "testdepovagony: vlak {} nenalezen.", argv[1]);
	return true;
}

static bool ConTestBuyWagons(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Tell a depot couple order which wagon to buy. Usage: 'testkoupit <cislo vlaku> <rozkaz> [<model>|vypnout|jentyp]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;

	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);

		uint32_t data = EngineID::Invalid().base();
		/* "jentyp": the player's pick in the window and nothing after it --
		 * the type named, buying left off. */
		bool type_only = argv.size() >= 4 && argv[3] == "jentyp";
		if (argv.size() < 4 || argv[3] != "vypnout") {
			EngineID eid = EngineID::Invalid();
			if (argv.size() >= 4 && !type_only) {
				auto pe = ParseInteger(argv[3]);
				if (!pe.has_value()) return false;
				eid = static_cast<EngineID>(*pe);
			} else {
				/* The first wagon this company can buy -- and, when the order
				 * already names a cargo, the first that can carry it. The
				 * window the player uses opens filtered to that cargo for the
				 * same reason, and the command refuses a pair that can never
				 * match; picking blind would hand it one. */
				const Order *o = t->GetOrder((VehicleOrderID)*porder);
				CargoType want = o != nullptr ? o->GetCoupleCargo() : INVALID_CARGO;
				for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
					if (!e->company_avail.Test(t->owner)) continue;
					if (e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon) continue;
					if (IsValidCargoType(want)) {
						bool carries = want == _road_vehicle_cargo ? CanCarryRoadVehicles(e)
								: GetUnionOfArticulatedRefitMasks(e->index, true).Test(want);
						if (!carries) continue;
					}
					eid = e->index;
					break;
				}
			}
			if (eid == EngineID::Invalid()) {
				IConsolePrint(CC_ERROR, "testkoupit: zadny vagon k dispozici.");
				return true;
			}
			data = eid.base();
		}

		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_BUY, data);
		/* Picking a type only names it; this probe is the player who then
		 * switches "Koupit" on as well, in a shed, as its name says. */
		const Order *picked = t->GetOrder((VehicleOrderID)*porder);
		if (type_only) {
			/* A pick buys nothing: buying is the player's second choice. */
			bool buys = picked != nullptr && picked->ShouldBuyWagons();
			IConsolePrint(buys ? CC_ERROR : CC_INFO, "testkoupit: vlak {} rozkaz {} -> jen typ {}, kupovat {}{}", *punit, *porder,
					data, buys ? "ano" : "ne", buys ? " - ODMITNUTO, vyber typu zapnul nakup" : "");
			return true;
		}
		if (r.Succeeded() && data != EngineID::Invalid().base() && picked != nullptr && picked->IsType(OT_GOTO_DEPOT)) {
			r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_BUY_ON, 1);
		}
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testkoupit: vlak {} rozkaz {} -> koupit model {} {}", *punit, *porder,
				data, r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testkoupit: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Switch a train's couple order to "found a rake here", the way the button in
 * the count box does. Usage: testzalozit <unit number> <order> [<max>]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestFoundRake(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Make a couple order found a rake. Usage: 'testzalozit <unit number> <order index> [<max vehicles>]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		if (argv.size() >= 4) {
			auto pmax = ParseInteger(argv[3]);
			if (pmax.has_value()) Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_COUNT, *pmax);
		}
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_FOUND, 1);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testzalozit: vlak {} rozkaz {} -> zalozit radu {}", *punit, *porder, r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testzalozit: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Make a couple order's count a minimum, the way the toggle does.
 * Usage: testminimalne <unit number> <order> [<count>] [0]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCoupleMin(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Make a couple order collect any rake of at least so many vehicles. Usage: 'testminimalne <unit number> <order index> [<count>] [0 to switch it off]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	uint32_t on = argv.size() >= 5 && ParseInteger(argv[4]).value_or(1) == 0 ? 0 : 1;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		if (argv.size() >= 4) {
			auto pcount = ParseInteger(argv[3]);
			if (pcount.has_value()) Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_COUNT, *pcount);
		}
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_MIN, on);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testminimalne: vlak {} rozkaz {} -> minimalne {} {}", *punit, *porder, on != 0 ? "ano" : "ne", r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testminimalne: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Make a couple order take a rake of at most so many vehicles, the way the
 * button in the count box does. Usage:
 * testmaximalne <unit number> <order index> [<count>] [0 to switch it off]
 *
 * The other end of the same number from testminimalne. Founding used to be
 * the only way to say it and said more besides; this says only it.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestCoupleMax(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Make a couple order collect any rake of at most so many vehicles. Usage: 'testmaximalne <unit number> <order index> [<count>] [0 to switch it off]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	uint32_t on = argv.size() >= 5 && ParseInteger(argv[4]).value_or(1) == 0 ? 0 : 1;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		if (argv.size() >= 4) {
			auto pcount = ParseInteger(argv[3]);
			if (pcount.has_value()) Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_COUNT, *pcount);
		}
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_COUPLE_MAX, on);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testmaximalne: vlak {} rozkaz {} -> maximalne {} {}", *punit, *porder, on != 0 ? "ano" : "ne", r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testmaximalne: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Save the mouse record the way Ctrl and the right button do. Usage: mousedebug
 * @copydoc IConsoleCmdProc
 */
static bool ConMouseDebug(std::span<std::string_view> argv)
{
	if (argv.empty()) return false;
	std::string saved = MouseDebugSave();
	if (saved.empty()) {
		IConsolePrint(CC_ERROR, "mousedebug: debug mysi se nepodarilo ulozit.");
	} else {
		IConsolePrint(CC_DEFAULT, "mousedebug: ulozeno do {}", saved);
	}
	return true;
}

/**
 * Switch the horn on a train's waypoint order, the way the button does.
 * Usage: testhoukat <unit number> <order> [0]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestHonk(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Make a waypoint order sound the horn. Usage: 'testhoukat <unit number> <order index> [0 to switch it off]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	uint32_t on = argv.size() >= 4 && ParseInteger(argv[3]).value_or(1) == 0 ? 0 : 1;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_HONK, on);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testhoukat: vlak {} rozkaz {} -> houkat {} {}", *punit, *porder, on != 0 ? "ano" : "ne", r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testhoukat: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * The front vehicle of a given unit number, train or road vehicle.
 *
 * The rig addresses vehicles by the number the player sees, and since road
 * vehicles carry their own numbering the same number means two different
 * vehicles; which of the two a command means is said by the word "auto".
 *
 * @param road look among road vehicles instead of trains
 * @param unit the unit number to look for
 * @return that vehicle, or nullptr when there is none
 */
static Vehicle *FindRigFrontVehicle(bool road, UnitID unit)
{
	if (road) {
		for (RoadVehicle *rv : RoadVehicle::Iterate()) {
			if (rv->IsFrontEngine() && rv->unitnumber == unit) return rv;
		}
		return nullptr;
	}
	for (Train *t : Train::Iterate()) {
		if (t->First() == t && t->unitnumber == unit) return t;
	}
	return nullptr;
}

/**
 * Put a conditional order at the end of a vehicle's orders, for the rig.
 *
 * The order window is the player's way in and a headless game has none, so
 * without this there is no way to measure what a conditional order actually
 * does -- which is the whole question when a new thing to ask about is added.
 * The numbers are those of OrderConditionVariable and OrderConditionComparator
 * in order_type.h; the help lists the ones this build has.
 *
 * Usage: testpodminka [auto] <unit number> <where> <variable> <comparator> <value> <skip to>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestConditionalOrder(std::span<std::string_view> argv)
{
	/* Road vehicles have conditions of their own now (how many are waiting for
	 * a train), so the command has to be able to mean a road vehicle. Saying so
	 * with a word in front keeps every argument after it where it was. */
	std::vector<std::string_view> a(argv.begin(), argv.end());
	bool road = a.size() >= 2 && a[1] == "auto";
	if (road) a.erase(a.begin() + 1);
	const char *what = road ? "auto" : "vlak";

	/* "zkus" asks the condition about the vehicle as it stands, without putting
	 * an order anywhere: the answer alone, which is the thing worth measuring
	 * when a new thing to ask about is added. Getting a vehicle to actually walk
	 * onto a conditional order in a headless game is a scene of its own. */
	if (a.size() == 6 && a[2] == "zkus") {
		auto punit = ParseInteger(a[1]);
		auto pvar = ParseInteger(a[3]);
		auto pcmp = ParseInteger(a[4]);
		auto pval = ParseInteger(a[5]);
		if (!punit.has_value() || !pvar.has_value() || !pcmp.has_value() || !pval.has_value()) return false;
		Vehicle *v = FindRigFrontVehicle(road, (UnitID)*punit);
		if (v == nullptr) {
			IConsolePrint(CC_ERROR, "testpodminka: {} {} nenalezen.", what, *punit);
			return true;
		}
		AutoRestoreBackup cur_company(_current_company, v->owner);

		/* Put into the list, asked, and taken out again. Asked off a loose
		 * order it would be a different question: a condition that looks at
		 * the orders around it (nothing to couple, how many are waiting for a
		 * train) has to be standing among them to have anything to look at. */
		Order order;
		order.MakeConditional(0);
		order.SetConditionVariable((OrderConditionVariable)*pvar);
		order.SetConditionComparator((OrderConditionComparator)*pcmp);
		order.SetConditionValue((uint16_t)*pval);
		VehicleOrderID at = v->GetNumOrders();
		if (Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, v->index, at, order).Failed()) {
			IConsolePrint(CC_ERROR, "testpodminka: {} {} - podminku {} {} {} nelze vlozit.", what, *punit, *pvar, *pcmp, *pval);
			return true;
		}
		VehicleOrderID to = ProcessConditionalOrder(v->GetOrder(at), v);
		Command<Commands::DeleteOrder>::Do(DoCommandFlag::Execute, v->index, at);

		if (road) {
			IConsolePrint(CC_DEFAULT, "testpodminka: auto {} - podminka {} {} {} -> {}",
					*punit, *pvar, *pcmp, *pval, to == INVALID_VEH_ORDER_ID ? "propadne" : "SKOK");
		} else {
			const Train *t = Train::From(v);
			IConsolePrint(CC_DEFAULT, "testpodminka: vlak {} vagonu {} delka {} - podminka {} {} {} -> {}",
					*punit, WagonUnitsBehindEngine(t), CeilDiv(t->gcache.cached_total_length, TILE_SIZE),
					*pvar, *pcmp, *pval, to == INVALID_VEH_ORDER_ID ? "propadne" : "SKOK");
		}
		return true;
	}

	if (a.size() != 7) {
		IConsolePrint(CC_HELP, "Add a conditional order. Usage: 'testpodminka [auto] <vozidlo> <kam vlozit> <promenna> <srovnani> <hodnota> <skoc na>'.");
		IConsolePrint(CC_HELP, "Or ask without adding anything: 'testpodminka [auto] <vozidlo> zkus <promenna> <srovnani> <hodnota>'.");
		IConsolePrint(CC_HELP, "promenna: {}=naklad% {}=spolehlivost {}=max rychlost {}=vek {}=servis {}=vzdy {}=zivotnost {}=max spolehlivost {}=couva {}=vagonu {}=delka {}=neni-co-pripojit {}=aut-ceka",
				to_underlying(OrderConditionVariable::LoadPercentage), to_underlying(OrderConditionVariable::Reliability),
				to_underlying(OrderConditionVariable::MaxSpeed), to_underlying(OrderConditionVariable::Age),
				to_underlying(OrderConditionVariable::RequiresService), to_underlying(OrderConditionVariable::Unconditionally),
				to_underlying(OrderConditionVariable::RemainingLifetime), to_underlying(OrderConditionVariable::MaxReliability),
				to_underlying(OrderConditionVariable::DrivingBackwards), to_underlying(OrderConditionVariable::WagonCount),
				to_underlying(OrderConditionVariable::TrainLength), to_underlying(OrderConditionVariable::NothingToCouple),
				to_underlying(OrderConditionVariable::RoadVehiclesWaitingToBoard));
		IConsolePrint(CC_HELP, "srovnani: 0=rovno 1=nerovno 2=mensi 3=mensi-rovno 4=vetsi 5=vetsi-rovno 6=je 7=neni");
		return true;
	}
	auto punit = ParseInteger(a[1]);
	auto pwhere = ParseInteger(a[2]);
	auto pvar = ParseInteger(a[3]);
	auto pcmp = ParseInteger(a[4]);
	auto pval = ParseInteger(a[5]);
	auto pskip = ParseInteger(a[6]);
	if (!punit.has_value() || !pwhere.has_value() || !pvar.has_value() || !pcmp.has_value() || !pval.has_value() || !pskip.has_value()) return false;

	Vehicle *v = FindRigFrontVehicle(road, (UnitID)*punit);
	if (v == nullptr) {
		IConsolePrint(CC_ERROR, "testpodminka: {} {} nenalezen.", what, *punit);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, v->owner);

	/* The condition goes in aiming at order zero and is pointed where it is
	 * wanted afterwards. Inserting it is checked against the list as it stands,
	 * so an order that only exists once the condition is in -- everything past
	 * where it was put -- cannot be named in the insert itself. */
	Order order;
	order.MakeConditional(0);
	order.SetConditionVariable((OrderConditionVariable)*pvar);
	order.SetConditionComparator((OrderConditionComparator)*pcmp);
	order.SetConditionValue((uint16_t)*pval);

	CommandCost r = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, v->index, (VehicleOrderID)*pwhere, order);
	if (r.Failed()) {
		IConsolePrint(CC_ERROR, "testpodminka: {} {} na misto {} -> podminka {} {} {} - {}",
				what, *punit, *pwhere, *pvar, *pcmp, *pval, RefusalReason(r));
		return true;
	}
	CommandCost d = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, v->index, (VehicleOrderID)*pwhere, MOF_COND_DESTINATION, (uint16_t)*pskip);
	IConsolePrint(d.Succeeded() ? CC_INFO : CC_ERROR, "testpodminka: {} {} na misto {} -> podminka {} {} {}, skok na {} - {}",
			what, *punit, *pwhere, *pvar, *pcmp, *pval, *pskip, d.Succeeded() ? "vlozeno" : RefusalReason(d));
	return true;
}

/**
 * Modify one field of an order, by the ModifyOrderFlags number, for the
 * scenes that need a switch no other rig command has. The numbers are those
 * of ModifyOrderFlags in order_type.h; the help lists the ones this build has.
 * Usage: testmof <unit number> <order index> <mof> <value>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestModifyOrder(std::span<std::string_view> argv)
{
	if (argv.size() != 5) {
		IConsolePrint(CC_HELP, "Modify an order field. Usage: 'testmof <unit number> <order index> <mof> <value>'.");
		IConsolePrint(CC_HELP, "mof: {}=decouple {}=decouple-count {}=decouple-whole {}=wait-couple {}=goto-couple {}=turn-in-depot {}=reverse-out {}=couple-load {}=couple-cargo {}=couple-count {}=couple-found {}=honk {}=couple-min {}=auto-departure",
				to_underlying(MOF_DECOUPLE), to_underlying(MOF_DECOUPLE_COUNT), to_underlying(MOF_DECOUPLE_WHOLE), to_underlying(MOF_WAIT_COUPLE), to_underlying(MOF_GOTO_COUPLE),
				to_underlying(MOF_TURN_AROUND_DEPOT), to_underlying(MOF_REVERSE_OUT), to_underlying(MOF_COUPLE_LOAD), to_underlying(MOF_COUPLE_CARGO), to_underlying(MOF_COUPLE_COUNT),
				to_underlying(MOF_COUPLE_FOUND), to_underlying(MOF_HONK), to_underlying(MOF_COUPLE_MIN), to_underlying(MOF_AUTO_DEPARTURE));
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	auto pmof = ParseInteger(argv[3]);
	auto pval = ParseInteger(argv[4]);
	if (!punit.has_value() || !porder.has_value() || !pmof.has_value() || !pval.has_value()) return false;
	if (*pmof >= to_underlying(MOF_END)) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, (ModifyOrderFlags)*pmof, (uint16_t)*pval);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testmof: vlak {} rozkaz {} mof {} = {} -> {}", *punit, *porder, *pmof, *pval,
				r.Succeeded() ? std::string("nastaveno")
						: fmt::format("ODMITNUTO{}", r.GetErrorMessage() == INVALID_STRING_ID ? "" : " - " + GetString(r.GetErrorMessage())));
		/* What the couple filter holds afterwards: several fields move
		 * together (a cargo can let the model go, a model can clear the
		 * cargo), and which of them did is the question. */
		if (const Order *o = t->GetOrder((VehicleOrderID)*porder); o != nullptr) {
			IConsolePrint(CC_INFO, "testmof: filtr - naklad {}, typ {}, kupovat {}",
					IsValidCargoType(o->GetCoupleCargo()) ? fmt::format("{}", o->GetCoupleCargo()) : std::string("vsechny"),
					o->GetCoupleBuyEngine() == EngineID::Invalid() ? std::string("zadny") : fmt::format("{}", o->GetCoupleBuyEngine().base()),
					o->ShouldBuyWagons() ? "ano" : "ne");
		}
		return true;
	}
	IConsolePrint(CC_ERROR, "testmof: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Make a station order leave engine first and then the shortest way.
 * Usage: testauto <unit number> <order index> [0 to switch it off]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestAutoDeparture(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Make a station order depart automatically. Usage: 'testauto <unit number> <order index> [0 to switch it off]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto porder = ParseInteger(argv[2]);
	if (!punit.has_value() || !porder.has_value()) return false;
	uint32_t on = argv.size() >= 4 && ParseInteger(argv[3]).value_or(1) == 0 ? 0 : 1;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost r = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, t->index, (VehicleOrderID)*porder, MOF_AUTO_DEPARTURE, on);
		IConsolePrint(r.Succeeded() ? CC_INFO : CC_ERROR, "testauto: vlak {} rozkaz {} -> automaticky {} {}", *punit, *porder, on != 0 ? "ano" : "ne", r.Succeeded() ? "nastaveno" : "ODMITNUTO");
		return true;
	}
	IConsolePrint(CC_ERROR, "testauto: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Break a train down where it stands, as the game's own breakdown would.
 * Usage: testporucha <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestBreakdown(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Break a train down where it stands. Usage: 'testporucha <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		/* Straight to "broken down", past the game's own gate that lets no
		 * train break down with a part in a shed: the rig stages exactly the
		 * case the game no longer produces on its own (a wreck there still
		 * can), to keep the push-in measured. */
		t->breakdown_ctr = 1;
		t->breakdown_delay = 255;
		IConsolePrint(CC_DEFAULT, "testporucha: vlak {} se porouchal na ({},{}).", t->unitnumber, TileX(t->tile), TileY(t->tile));
		return true;
	}
	IConsolePrint(CC_ERROR, "testporucha: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Give a train orders to go round stations, one after the other, and start it:
 * for staging a player's save, where the train that was there is gone and one
 * has to be sent the same way. Stops at the far end of each platform.
 * Usage: testjed <unit number> <station id> [station id ...]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestGoRound(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Send a train round stations. Usage: 'testjed <unit number> <station id> [station id ...]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	Train *t = FindTrainByUnit(*punit);
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testjed: ODMITNUTO - vlak {} nenalezen.", argv[1]);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, t->owner);
	for (size_t i = 2; i < argv.size(); i++) {
		auto pst = ParseInteger(argv[i]);
		if (!pst.has_value() || !Station::IsValidID(static_cast<StationID>(*pst))) {
			IConsolePrint(CC_ERROR, "testjed: ODMITNUTO - stanice {} neni.", argv[i]);
			for (const Station *st : Station::Iterate()) {
				IConsolePrint(CC_DEFAULT, "testjed:   stanice {} na ({},{}){}", st->index.base(), TileX(st->xy), TileY(st->xy), st->facilities.Test(StationFacility::Train) ? " vlaky" : "");
			}
			return true;
		}
		Order o{};
		o.MakeGoToStation(static_cast<StationID>(*pst));
		o.SetStopLocation(OrderStopLocation::FarEnd);
		CommandCost res = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, t->index, t->GetNumOrders(), o);
		if (res.Failed()) IConsolePrint(CC_ERROR, "testjed: ODMITNUTO - rozkaz na stanici {} nejde: {}", *pst, GetString(res.GetErrorMessage()));
	}
	if (t->vehstatus.Test(VehState::Stopped)) Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, t->index, false);
	IConsolePrint(CC_DEFAULT, "testjed: vlak {} ma {} rozkazu a jede.", t->unitnumber, t->GetNumOrders());
	return true;
}

/** The train to break down on reaching a tile (testporuchana), and the tile. */
static UnitID _testporuchana_unit = 0;
static TileIndex _testporuchana_tile = INVALID_TILE;

/** Break the train named by testporuchana down the moment its head is on its tile. */
static const IntervalTimer<TimerGameTick> _testporuchana_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	if (_testporuchana_unit == 0) return;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != _testporuchana_unit || !t->IsFrontEngine()) continue;
		if (t->GetMovingFront()->tile != _testporuchana_tile) return;
		t->breakdown_ctr = 1;
		t->breakdown_delay = 255;
		IConsolePrint(CC_DEFAULT, "testporuchana: vlak {} se porouchal na ({},{}), rychlost {}.", t->unitnumber, TileX(_testporuchana_tile), TileY(_testporuchana_tile), t->cur_speed);
		_testporuchana_unit = 0;
		return;
	}
});

/**
 * Break a train down as soon as it gets to a tile, the way the game's own
 * breakdown catches a train wherever it happens to be.
 * Usage: testporuchana <unit number> <x> <y>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestBreakdownAt(std::span<std::string_view> argv)
{
	if (argv.size() != 4) {
		IConsolePrint(CC_HELP, "Break a train down on reaching a tile. Usage: 'testporuchana <unit number> <x> <y>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto px = ParseInteger(argv[2]);
	auto py = ParseInteger(argv[3]);
	if (!punit.has_value() || !px.has_value() || !py.has_value()) return false;
	_testporuchana_unit = static_cast<UnitID>(*punit);
	_testporuchana_tile = TileXY(static_cast<uint>(*px), static_cast<uint>(*py));
	IConsolePrint(CC_DEFAULT, "testporuchana: vlak {} ma se porouchat na ({},{}).", *punit, *px, *py);
	return true;
}

/**
 * Send a train past the signal in front of it, the same as the player's
 * "ignore signal" button. Usage: testprojet <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestForceProceed(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Make a train pass the signal ahead. Usage: 'testprojet <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		AutoRestoreBackup cur_company(_current_company, t->owner);
		CommandCost res = Command<Commands::ForceTrainProceed>::Do(DoCommandFlag::Execute, t->index);
		IConsolePrint(res.Failed() ? CC_ERROR : CC_DEFAULT, "testprojet: vlak {} - {} (force_proceed {})", t->unitnumber,
				res.Failed() ? GetString(res.GetErrorMessage()) : std::string("projizdi navest"), to_underlying(t->force_proceed));
		return true;
	}
	IConsolePrint(CC_ERROR, "testprojet: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Say what the game makes of a savegame's NewGRFs, and fetch what it will not
 * take a substitute for.
 *
 * The question this answers cannot be got at from a headless game any other
 * way: which releases the savegame names, which of them are on the disk, which
 * were swapped for another release of the same set, and which this game refuses
 * to swap (see MustMatchSavegameRelease()). Those last are what the load button
 * fetches before it loads, and "stahni" does that same fetching here.
 *
 * Usage: testgrf <savegame> [stahni]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSavegameGrfs(std::span<std::string_view> argv)
{
	if (argv.size() != 2 && argv.size() != 3) {
		IConsolePrint(CC_HELP, "Say what a savegame's NewGRFs come to. Usage: 'testgrf <soubor> [stahni]'.");
		return true;
	}

	_load_check_data.Clear();
	if (SaveOrLoad(std::string(argv[1]), SaveLoadOperation::Check, DetailedFileType::GameFile, Subdirectory::None, false) == SaveLoadResult::Error) {
		IConsolePrint(CC_ERROR, "testgrf: soubor {} nejde precist.", argv[1]);
		return true;
	}

	static const char * const stav[] = {"neznamo", "vypnuto", "nenalezeno", "pripraveno", "aktivni"};
	for (const auto &c : _load_check_data.grfconfig) {
		const char *jak = c->status == GRFStatus::NotFound ? "CHYBI" :
				(c->flags.Test(GRFConfigFlag::Compatible) ? "nahrazeno jinym vydanim" : "presne to, ktere sav jmenuje");
		IConsolePrint(CC_DEFAULT, "testgrf: {:08X} ({}) - {}, stav {}", std::byteswap(c->ident.grfid), c->filename, jak,
				to_underlying(c->status) < lengthof(stav) ? stav[to_underlying(c->status)] : "?");
	}

	std::vector<GRFIdentifier> fetch = GetSavegameReleasesToFetch(_load_check_data.grfconfig);
	IConsolePrint(CC_DEFAULT, "testgrf: nahrada se neuznava u {} sad, sit {}", fetch.size(), _network_available ? "je" : "neni");
	for (const GRFIdentifier &id : fetch) {
		IConsolePrint(CC_DEFAULT, "testgrf:   chce {:08X} soucet {}", std::byteswap(id.grfid), FormatArrayAsHex(id.md5sum));
	}

	if (argv.size() == 3 && argv[2] == "stahni") {
		if (FetchExactNewGRFs(std::move(fetch), []() { IConsolePrint(CC_DEFAULT, "testgrf: stahovani dobehlo, tady by se nacetla hra."); })) {
			IConsolePrint(CC_DEFAULT, "testgrf: stahovani zacalo.");
		} else {
			IConsolePrint(CC_DEFAULT, "testgrf: neni co stahovat, nebo neni sit.");
		}
	}
	return true;
}

/**
 * Turn a train round, the same as the player's reverse button.
 * Usage: testotoc <unit number>
 * @copydoc IConsoleCmdProc
 */
bool TrainController(Train *v, Vehicle *nomove, bool reverse = true); // From train_cmd.cpp

static bool ConTestReverse(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Turn a train round. Usage: 'testotoc <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		/* Fired from the heartbeat timer there is no acting company set, and
		 * the command would bounce off its ownership check, silently. */
		AutoRestoreBackup cur_company(_current_company, t->owner);
		Command<Commands::ReverseTrainDirection>::Do(DoCommandFlag::Execute, t->index, false);
		IConsolePrint(CC_DEFAULT, "testotoc: vlak {} otocen.", t->unitnumber);
		return true;
	}
	IConsolePrint(CC_ERROR, "testotoc: vlak {} nenalezen.", argv[1]);
	return true;
}

/** State of the "brake, fail to brake and crash" scene; see ConTestOverrun(). */
static struct {
	bool active = false;
	int phase = 0;
	VehicleID ahead = VehicleID::Invalid(); ///< the train standing in the block past the red
	VehicleID behind = VehicleID::Invalid(); ///< the train coming up to the red
	TileIndex depot = INVALID_TILE; ///< where the one behind starts
	uint red_x = 0; ///< x of the red signal the one behind comes up to
	int stop_before = -1; ///< press the stop this many tiles short of the red; -1 never
	uint last_x = 0; ///< the tile the one behind was last reported on
} _testnedobrzdil;

/**
 * Build the "brake, fail to brake and crash" scene.
 *
 * One straight line east from a depot, a platform far down it, and signals
 * facing east. A light engine drives to the platform and stands there braked,
 * so the block it stands in is taken; a second train then comes up from the
 * depot behind it at line speed towards the red that guards that block. With
 * "stopka" the player's stop is pressed on the second train a given number of
 * tiles short of the red -- from then on nobody drives it, it only brakes, and
 * whether it runs past the red into the train at the platform is what the
 * scene reads (NEDOBRZDIL, Srazka).
 *
 * "blok": three one-way block signals two tiles apart before the platform, the
 * player's "three signals in a row". "cesta": one one-way path signal. The
 * setting vehicle.train_braking is the scene's to set.
 *
 * "odtah" adds a rescue engine on call in the depot, so that the wreck is
 * fetched and the helicopter's leaving and the second news can be read.
 *
 * "les" plants trees on every open tile around the platform, so the
 * helicopter finds nowhere in the papers' picture to land and circles.
 *
 * "rozestup N": the signal before the red stands N tiles before it instead of
 * two, with nothing between the first signal and it.
 *
 * "husto": block signals every two tiles from the first to the red, the
 * player's screenshot -- two greens, two yellows and a red, and the train
 * already braking.
 *
 * The one behind is reported on every tile of the last sixteen before the
 * red, with its speed, so where it starts braking can be read.
 *
 * Usage: testnedobrzdil <blok|cesta> [stopka <tiles short of the red>] [vozu <wagons behind, 3 by default>] [rozestup <tiles>] [odtah] [les]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestOverrun(std::span<std::string_view> argv)
{
	if (argv.size() < 2 || (argv[1] != "blok" && argv[1] != "cesta")) {
		IConsolePrint(CC_HELP, "Build the fail-to-brake scene. Usage: 'testnedobrzdil <blok|cesta> [stopka <policek pred cervenou>] [vozu <pocet>] [rozestup <policek>] [odtah] [les]'.");
		return true;
	}
	bool block = argv[1] == "blok";
	int stop_before = -1;
	uint wagons = 3;
	bool with_tow = false;
	bool forest = false;
	uint spacing = 0; // tiles between the last two signals before the red; 0 for the original three in a row
	bool dense = false;
	for (size_t i = 2; i < argv.size(); i++) {
		if (argv[i] == "les") {
			forest = true;
		} else if (argv[i] == "husto") {
			dense = true;
		} else if (argv[i] == "odtah") {
			with_tow = true;
		} else if (argv[i] == "stopka" || argv[i] == "vozu" || argv[i] == "rozestup") {
			if (i + 1 >= argv.size()) return false;
			auto p = ParseInteger(argv[i + 1]);
			if (!p.has_value()) return false;
			if (argv[i] == "stopka") {
				stop_before = (int)*p;
			} else if (argv[i] == "vozu") {
				wagons = (uint)*p;
			} else {
				spacing = (uint)*p;
			}
			i++;
		}
	}
	if (spacing > 21) {
		IConsolePrint(CC_ERROR, "testnedobrzdil: rozestup 1 az 21 policek.");
		return true;
	}
	if (_game_mode != GameMode::Normal) return true;
	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) return true;
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	AutoRestoreBackup cur_company(_current_company, _local_company);

	EngineID eid_loco = EngineID::Invalid();
	EngineID eid_wagon = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type == RailVehicleType::Wagon) {
			if (eid_wagon == EngineID::Invalid()) eid_wagon = e->index;
		} else if (eid_loco == EngineID::Invalid()) {
			eid_loco = e->index;
		}
	}
	if (eid_loco == EngineID::Invalid() || eid_wagon == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testnedobrzdil: ODMITNUTO - no engine or wagon.");
		return true;
	}

	static const uint LEN = 48;
	TileIndex strip = INVALID_TILE;
	for (uint y = 8; y < Map::SizeY() - 8 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			TileIndex t = TileXY(x, y);
			bool ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
			int z = ok ? GetTileZ(t) : -1;
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					strip = TileXY(x - LEN + 1, y);
					break;
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testnedobrzdil: ODMITNUTO - no flat strip of {} tiles.", LEN);
		return true;
	}
	uint x0 = TileX(strip);
	uint y0 = TileY(strip);
	TileIndex depot = TileXY(x0, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot, RAILTYPE_RAIL, DiagDirection::SW).Failed() ||
			Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, y0), TileXY(x0 + 1, y0), RAILTYPE_RAIL, Track::X, false, true).Failed() ||
			Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, TileXY(x0 + 36, y0), RAILTYPE_RAIL, Axis::X, 1, 4, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed()) {
		IConsolePrint(CC_ERROR, "testnedobrzdil: ODMITNUTO - track, depot or platform failed.");
		return true;
	}
	StationID st = GetStationIndex(TileXY(x0 + 36, y0));

	/* Facing east, the way both trains go. */
	uint8_t east = SignalAlongTrackdir(Trackdir::X_SW);
	std::vector<uint> signals = block ? std::vector<uint>{x0 + 10, x0 + 28, x0 + 30, x0 + 32} : std::vector<uint>{x0 + 10, x0 + 32};
	/* "rozestup": one signal that far before the red and nothing else between
	 * the first signal and it, so the warning it shows is seen that much
	 * earlier -- signals close together and far apart. */
	if (spacing != 0) signals = {x0 + 10, x0 + 32 - spacing, x0 + 32};
	if (dense) {
		signals.clear();
		for (uint sx = x0 + 10; sx <= x0 + 32; sx += 2) signals.push_back(sx);
	}
	SignalType kind = block ? SignalType::Block : SignalType::Path;
	for (uint sx : signals) {
		if (Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(sx, y0), Track::X, kind, SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, 0, east).Failed()) {
			IConsolePrint(CC_ERROR, "testnedobrzdil: ODMITNUTO - signal at ({},{}) failed.", sx, y0);
			return true;
		}
	}
	UpdateSignalsInBuffer();

	if (forest) {
		uint planted = 0;
		for (uint y = (y0 > 9 ? y0 - 9 : 0); y <= y0 + 9 && y < Map::SizeY() - 1; y++) {
			for (uint x = x0 + 22; x <= x0 + LEN + 2 && x < Map::SizeX() - 1; x++) {
				TileIndex t = TileXY(x, y);
				if (!IsTileType(t, TileType::Clear)) continue;
				if (Command<Commands::PlantTree>::Do(DoCommandFlag::Execute, t, t, 0xFF, false).Succeeded()) planted++;
			}
		}
		IConsolePrint(CC_DEFAULT, "testnedobrzdil: les - vysazeno {} policek.", planted);
	}

	auto build = [&](uint wagons) -> Train * {
		auto [cost, veh, a, b, c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost.Failed()) return nullptr;
		for (uint i = 0; i < wagons; i++) {
			auto [wc, wv, wa, wb, wd] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
			if (wc.Succeeded()) Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wv, veh, false);
		}
		Order o;
		o.MakeGoToStation(st);
		o.SetNonStopType(OrderNonStopFlags{OrderNonStopFlag::NonStop});
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh, 0, o);
		return Train::Get(veh);
	};
	Train *ahead = build(0);
	Train *behind = build(wagons);
	if (ahead == nullptr || behind == nullptr) {
		IConsolePrint(CC_ERROR, "testnedobrzdil: ODMITNUTO - trains failed.");
		return true;
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, ahead->index, false);
	if (with_tow) {
		auto [cost_t, veh_t, ta, tb, tc] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_t.Failed() || Command<Commands::SetRescueEngine>::Do(DoCommandFlag::Execute, veh_t, true).Failed()) {
			IConsolePrint(CC_ERROR, "testnedobrzdil: ODMITNUTO - rescue engine failed.");
		} else {
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_t, false);
			IConsolePrint(CC_DEFAULT, "testnedobrzdil: odtahovka vlak {} v depu ({},{}).", Train::Get(veh_t)->unitnumber, x0, y0);
		}
	}

	_testnedobrzdil.active = true;
	_testnedobrzdil.phase = 0;
	_testnedobrzdil.ahead = ahead->index;
	_testnedobrzdil.behind = behind->index;
	_testnedobrzdil.depot = depot;
	_testnedobrzdil.red_x = x0 + 32;
	_testnedobrzdil.stop_before = stop_before;
	_testnedobrzdil.last_x = 0;
	IConsolePrint(CC_DEFAULT, "testnedobrzdil: {} - vlak {} jede na nastupiste ({},{}), vlak {} ({} vozu, {} t) ho dojede, cervena na ({},{}){}; nastaveni {}.",
			block ? "tri blokova navestidla" : "cestne navestidlo", ahead->unitnumber, x0 + 36, y0, behind->unitnumber,
			CountVehiclesInChain(behind), behind->gcache.cached_weight,
			x0 + 32, y0, (stop_before < 0 ? "" : fmt::format(", stopka {} policek pred ni", stop_before)) +
			(spacing == 0 ? "" : fmt::format(", navestidlo {} policek pred ni", spacing)),
			IsSignalOverrunOn() ? "ZAPNUTO" : "vypnuto");
	return true;
}

/** Drive the fail-to-brake scene along, one tick at a time. */
static const IntervalTimer<TimerGameTick> _testnedobrzdil_timer({TimerGameTick::Priority::None, 1}, [](auto) {
	if (!_testnedobrzdil.active) return;
	Train *ahead = Train::GetIfValid(_testnedobrzdil.ahead);
	Train *behind = Train::GetIfValid(_testnedobrzdil.behind);
	if (ahead == nullptr || behind == nullptr) {
		_testnedobrzdil.active = false;
		return;
	}
	AutoRestoreBackup cur_company(_current_company, behind->owner);
	if (_testnedobrzdil.phase == 0) {
		/* The one ahead has pulled up at the platform: braked there for good,
		 * and the one behind sets off. */
		if (ahead->current_order.IsType(OT_LOADING)) {
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, ahead->index, false);
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, behind->index, false);
			IConsolePrint(CC_DEFAULT, "testnedobrzdil: vlak {} stoji na nastupisti zabrzdeny, vlak {} vyjizdi (tik {}).",
					ahead->unitnumber, behind->unitnumber, TimerGameTick::counter);
			_testnedobrzdil.phase = 1;
		}
		return;
	}
	if (_testnedobrzdil.phase >= 1 && !behind->IsWrecked()) {
		const Train *front = behind->GetMovingFront();
		uint x = TileX(front->tile);
		if (x + 1 == _testnedobrzdil.red_x) {
			/* The last tile before the red, tick by tick: what the driver
			 * asks for and what the signal shows. */
			TileIndex red = TileXY(_testnedobrzdil.red_x, TileY(front->tile));
			bool green = IsTileType(red, TileType::Railway) && HasSignalOnTrackdir(red, Trackdir::X_SW) &&
					GetSignalStateByTrackdir(red, Trackdir::X_SW) == SignalState::Green;
			IConsolePrint(CC_DEFAULT, "testnedobrzdil: tik {} vlak {} poz {} rychlost {} strop {} navestidlo {}", TimerGameTick::counter,
					behind->unitnumber, front->x_pos & 0xF, behind->cur_speed,
					behind->driver_ceiling == INT32_MAX ? -1 : behind->driver_ceiling, green ? "ZELENE" : "cervene");
		}
		if (x != _testnedobrzdil.last_x && x + 16 >= _testnedobrzdil.red_x && x <= _testnedobrzdil.red_x + 2) {
			_testnedobrzdil.last_x = x;
			IConsolePrint(CC_DEFAULT, "testnedobrzdil: vlak {} na x {} ({} pred cervenou) rychlost {} strop {}", behind->unitnumber, x,
					(int)_testnedobrzdil.red_x - (int)x, behind->cur_speed, behind->driver_ceiling == INT32_MAX ? -1 : behind->driver_ceiling);
		}
	}
	if (_testnedobrzdil.phase == 1 && _testnedobrzdil.stop_before >= 0 && !behind->IsWrecked()) {
		const Train *front = behind->GetMovingFront();
		if (TileX(front->tile) + _testnedobrzdil.stop_before >= _testnedobrzdil.red_x) {
			Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, behind->index, false);
			IConsolePrint(CC_DEFAULT, "testnedobrzdil: STOPKA na vlaku {} na ({},{}) rychlosti {} (tik {}).", behind->unitnumber,
					TileX(front->tile), TileY(front->tile), behind->cur_speed, TimerGameTick::counter);
			_testnedobrzdil.phase = 2;
		}
	}
	if (_testnedobrzdil.phase >= 1 && behind->cur_speed == 0 && !behind->IsWrecked() && (_testnedobrzdil.stop_before < 0 || _testnedobrzdil.phase == 2) &&
			TileX(behind->GetMovingFront()->tile) > TileX(_testnedobrzdil.depot) + 12) {
		IConsolePrint(CC_DEFAULT, "testnedobrzdil: vlak {} zastavil na ({},{}), cervena na x {} (tik {}).", behind->unitnumber,
				TileX(behind->GetMovingFront()->tile), TileY(behind->GetMovingFront()->tile), _testnedobrzdil.red_x, TimerGameTick::counter);
		_testnedobrzdil.active = false;
	}
	if (behind->IsWrecked()) {
		IConsolePrint(CC_DEFAULT, "testnedobrzdil: vlak {} je vrak (tik {}).", behind->unitnumber, TimerGameTick::counter);
		_testnedobrzdil.active = false;
	}
});

/**
 * Say whether a train is driving backwards, and refuse when it is not the
 * way the scene expects. The counters read spojeno, odtazeno and the rest;
 * which way round a train stands is none of them, so a scene that turns a
 * train and needs it to stay turned had no counter to fail on. It has this:
 * a mismatch is written ODMITNUTO, which the battery counts.
 * Usage: testcouva <unit number> <ano|ne>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestDrivingBackwards(std::span<std::string_view> argv)
{
	if (argv.size() != 3 || (argv[2] != "ano" && argv[2] != "ne")) {
		IConsolePrint(CC_HELP, "Check whether a train is driving backwards. Usage: 'testcouva <unit number> <ano|ne>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	bool expected = argv[2] == "ano";
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		bool backwards = t->vehicle_flags.Test(VehicleFlag::DrivingBackwards);
		if (backwards == expected) {
			IConsolePrint(CC_DEFAULT, "testcouva: vlak {} couva {} - souhlasi.", t->unitnumber, backwards ? "ano" : "ne");
		} else {
			IConsolePrint(CC_ERROR, "testcouva: ODMITNUTO - vlak {} couva {}, cekano {}.", t->unitnumber, backwards ? "ano" : "ne", expected ? "ano" : "ne");
		}
		return true;
	}
	IConsolePrint(CC_ERROR, "testcouva: ODMITNUTO - vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * The running record of what the game had to work around: say where it is,
 * put a line of the player's own in it, or turn it off.
 *
 * Nothing here starts the writing -- it writes by itself, into the folder the
 * savegames are in, from the moment the game runs. That is the whole point of
 * it: what it is for is the thing nobody expected, and nobody switches on a
 * log for something they did not expect.
 *
 * Usage: log | log <text> | log vyp | log zap
 * @copydoc IConsoleCmdProc
 */
static bool ConAnomalyLog(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;

	if (argv.size() >= 2 && (argv[1] == "vyp" || argv[1] == "zap")) {
		SetAnomalyLogOn(argv[1] == "zap");
		IConsolePrint(CC_DEFAULT, "log: zaznam je {}.", IsAnomalyLogOn() ? "zapnuty" : "vypnuty");
		return true;
	}

	if (argv.size() >= 2) {
		/* The player's own mark. Worth as much as anything the game writes: it
		 * is the line that says which of the hundred things in the file was the
		 * one that looked wrong on the screen. */
		std::string note;
		for (size_t i = 1; i < argv.size(); i++) {
			if (i > 1) note += " ";
			note += argv[i];
		}
		LogAnomaly("HRAC: {}", note);
		return true;
	}

	IConsolePrint(CC_DEFAULT, "log: {} ({} radek v teto hre), zaznam {}.", GetAnomalyLogPath(),
			GetAnomalyLogLines(), IsAnomalyLogOn() ? "zapnuty" : "vypnuty");
	IConsolePrint(CC_HELP, "Zapisuje se sam. 'log <text>' prida tvoji poznamku, 'log vyp' / 'log zap' vypne a zapne.");
	return true;
}

/** Find the head of a train by its unit number, or nullptr. */
static Train *FindTrainByUnit(uint unit)
{
	for (Train *t : Train::Iterate()) {
		if (t->First() == t && t->unitnumber == (UnitID)unit) return t;
	}
	return nullptr;
}

/**
 * Wreck a train where it stands, the way a collision would, so a scene can
 * have a wreck exactly where it wants one and at the tick it wants it.
 * Usage: testvrak <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestWreck(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Wreck a train where it stands. Usage: 'testvrak <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	Train *t = FindTrainByUnit(*punit);
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testvrak: vlak {} nenalezen.", argv[1]);
		return true;
	}
	if (t->IsWrecked()) {
		IConsolePrint(CC_ERROR, "testvrak: vlak {} uz je vrak.", t->unitnumber);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, t->owner);
	TrainCrashed(t);
	IConsolePrint(CC_DEFAULT, "testvrak: vlak {} je vrak na ({},{}).", t->unitnumber, TileX(t->tile), TileY(t->tile));
	return true;
}

/**
 * Clear a wreck off the line at once (ClearWreck()), to stage a player's save
 * as it stood before a crash. Usage: 'testsmazat <unit number>'.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestClearWreck(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Clear a wreck off the line. Usage: 'testsmazat <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	Train *t = FindTrainByUnit(*punit);
	if (t == nullptr || !t->IsWrecked()) {
		IConsolePrint(CC_ERROR, "testsmazat: ODMITNUTO - vlak {} neni vrak.", argv[1]);
		return true;
	}
	TileIndex tile = t->tile;
	ClearWreck(t);
	IConsolePrint(CC_DEFAULT, "testsmazat: vrak {} z ({},{}) odklizen.", *punit, TileX(tile), TileY(tile));
	return true;
}

/**
 * The game's own test of a train's spacing (CheckTrainsLengths()), train by
 * train and pair by pair, said out loud: which train, which two pieces, how
 * far apart they stand and how far they should. The game's own says only
 * that a train is broken and pauses -- at every load, for a train coupled
 * round a curve by an older build. Each such train is refused (ODMITNUTO),
 * so a scene can count them.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestLengths(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List every train whose pieces stand wrongly spaced. Usage: 'testdelky'.");
		return true;
	}
	uint bad = 0;
	uint trains = 0;
	for (const Train *v : Train::Iterate()) {
		if (v->First() != v || v->vehstatus.Test(VehState::Crashed)) continue;
		trains++;
		for (const Train *u = v->GetMovingFront(), *w = v->GetMovingNext(); w != nullptr; u = w, w = w->GetMovingNext()) {
			if (u->track == Track::Depot || w->track == Track::Depot) continue;
			int gap = std::max(abs(u->x_pos - w->x_pos), abs(u->y_pos - w->y_pos));
			if (gap == u->CalcNextVehicleOffset()) continue;
			IConsolePrint(CC_ERROR, "testdelky: ODMITNUTO - {} {} na ({},{}): clanky {} ({},{}) a {} ({},{}) stoji {} od sebe, maji {}",
					v->IsFrontEngine() ? "vlak" : "rada", v->IsFrontEngine() ? v->unitnumber : 0, TileX(v->tile), TileY(v->tile),
					u->index.base(), u->x_pos, u->y_pos, w->index.base(), w->x_pos, w->y_pos, gap, u->CalcNextVehicleOffset());
			bad++;
			break;
		}
	}
	IConsolePrint(CC_DEFAULT, "testdelky: vlaku a rad {}, spatne rozestavenych {}", trains, bad);
	return true;
}

/**
 * The rail vehicles whose name has a piece of text in it: the name the set
 * gives the vehicle and the name the purchase list shows, which a set may
 * change over the years (CZTR's Pasy shows as Sgs from 1980). For finding the
 * vehicle a player names by what he sees.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestNames(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "List rail vehicles whose name has a piece of text in it. Usage: 'testjmena <text>'.");
		return true;
	}
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (e->info.string_id == INVALID_STRING_ID) continue;
		std::string name = GetString(e->info.string_id);
		std::string shown = GetString(STR_ENGINE_NAME, PackEngineNameDParam(e->index, EngineNameContext::PurchaseList));
		if (name.find(argv[1]) == std::string::npos && shown.find(argv[1]) == std::string::npos) continue;
		const GRFFile *grf = e->GetGRF();
		IConsolePrint(CC_DEFAULT, "testjmena: {} '{}' (v nabidce '{}'), {}, GRF {:08X} cislo {:#x}, uvedeni {}", e->index, name, shown,
				e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Wagon ? "vagon" : "hnaci", grf == nullptr ? 0 : std::byteswap(grf->grfid),
				e->grf_prop.local_id, e->intro_date);
	}
	return true;
}

/**
 * The St carrying marijuana (IsGreenLayerWagon()): build one into the first
 * rail depot of the company, fitted for marijuana and full, and ask for its
 * picture in every direction the way the screen would -- the rig has no
 * screen, so nothing else would ever make the green layers. Says how many
 * pictures each direction has and how many pixels of each layer came out
 * green; refused (ODMITNUTO) when there is no St, it cannot be fitted, it has
 * no layer over the wagon, or a layer has nothing green in it. Asked again
 * empty, it has to draw no green layer.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestGreenSt(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build every St and U wagon carrying marijuana and draw them. Usage: 'testzelenest'.");
		return true;
	}
	auto refuse = [](std::string_view why) { IConsolePrint(CC_ERROR, "testzelenest: ODMITNUTO - {}", why); return true; };
	CargoType mari = GetCargoTypeByLabel(CT_MARIJUANA);
	if (!IsValidCargoType(mari)) return refuse("marihuana v teto hre neni");
	if (Company::GetIfValid(_local_company) == nullptr) return refuse("zadna spolecnost");
	AutoRestoreBackup cur_company(_current_company, _local_company);

	std::vector<const Engine *> wagons;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!IsGreenLayerWagon(e)) continue;
		IConsolePrint(CC_DEFAULT, "testzelenest: {} '{}', {}, marihuana v prestavbe {}", e->index, GetString(e->info.string_id),
				e->company_avail.Test(_local_company) ? "k mani" : "neni k mani", e->info.refit_mask.Test(mari) ? "ano" : "NE");
		if (!e->info.refit_mask.Test(mari)) IConsolePrint(CC_ERROR, "testzelenest: ODMITNUTO - {} nejde prestavet na marihuanu", e->index);
		if (e->company_avail.Test(_local_company)) wagons.push_back(e);
	}
	if (wagons.empty()) return refuse("zadny St ani U k mani");

	TileIndex depot = INVALID_TILE;
	for (const Depot *d : Depot::Iterate()) {
		if (IsRailDepotTile(d->xy) && GetTileOwner(d->xy) == _local_company) {
			depot = d->xy;
			break;
		}
	}
	if (depot == INVALID_TILE) return refuse("zadne depo");

	for (const Engine *st : wagons) {
		auto [cost, id, cap, mail, caps] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, st->index, true, mari, ClientID::Invalid);
		Train *t = Train::GetIfValid(id);
		if (cost.Failed() || t == nullptr) {
			refuse(fmt::format("{} nejde koupit na marihuanu: {}", st->index, RefusalReason(cost)));
			continue;
		}
		/* A green picture is one of the block the green ones are made in. */
		auto is_green = [](SpriteID sprite) { return sprite >= SPR_GREEN_LAYER_BASE && sprite < SPR_GREEN_LAYER_BASE + GREEN_LAYER_SPRITE_COUNT; };
		auto draw = [&](bool full) {
			uint greens = 0;
			uint green_total = 0;
			for (Train *u = t; u != nullptr; u = u->Next()) {
				u->cargo.Truncate();
				if (full && u->cargo_cap > 0 && CargoPacket::CanAllocateItem()) u->cargo.Append(CargoPacket::Create(u->cargo_cap, 0, StationID::Invalid(), TileIndex{}, 0));
			}
			for (Direction dir : EnumRange(Direction::Begin, Direction::End)) {
				for (const Train *u = t; u != nullptr; u = u->Next()) {
					VehicleSpriteSeq seq;
					u->GetImage(dir, EngineImageType::OnMap, &seq);
					std::string line = fmt::format("testzelenest: {} {} smer {} dil {} ({}/{}) obrazku {}:", st->index, full ? "plny" : "prazdny", to_underlying(dir), u->index,
							u->cargo.StoredCount(), u->cargo_cap, seq.count);
					for (uint i = 0; i < seq.count; i++) {
						SpriteID sprite = seq.seq[i].sprite;
						auto [green, drawn] = GreenLoadPixels(sprite);
						line += fmt::format(" {}{}", sprite, is_green(sprite) ? fmt::format(" (zelena {}/{})", green, drawn) : "");
						if (!is_green(sprite)) continue;
						greens++;
						green_total += green;
						/* An empty wagon's layer draws nothing at all, which is right. */
						if (drawn > 0 && green == 0) IConsolePrint(CC_ERROR, "testzelenest: ODMITNUTO - zelena {} nema nic zeleneho", sprite);
						if (!full && green > 0) IConsolePrint(CC_ERROR, "testzelenest: ODMITNUTO - prazdny {} kresli zeleny naklad ({} pixelu)", st->index, green);
					}
					IConsolePrint(CC_DEFAULT, "{}", line);
				}
			}
			return std::pair<uint, uint>{greens, green_total};
		};
		/* The same wagon carrying coal, full, for comparison: the picture the
		 * set itself draws a loaded one with. */
		if (CargoType coal = GetCargoTypeByLabel(CT_COAL); IsValidCargoType(coal)) {
			auto [c_cost, c_id, c_cap, c_mail, c_caps] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, st->index, true, coal, ClientID::Invalid);
			if (Train *c = Train::GetIfValid(c_id); c_cost.Succeeded() && c != nullptr) {
				for (Train *u = c; u != nullptr; u = u->Next()) {
					if (u->cargo_cap > 0 && CargoPacket::CanAllocateItem()) u->cargo.Append(CargoPacket::Create(u->cargo_cap, 0, StationID::Invalid(), TileIndex{}, 0));
				}
				VehicleSpriteSeq seq;
				c->GetImage(Direction::N, EngineImageType::OnMap, &seq);
				std::string line = fmt::format("testzelenest: {} s uhlim ({}/{}) obrazku {}:", st->index, c->cargo.StoredCount(), c->cargo_cap, seq.count);
				for (uint i = 0; i < seq.count; i++) line += fmt::format(" {}", seq.seq[i].sprite);
				IConsolePrint(CC_DEFAULT, "{}", line);
				Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, c->index, true, false, ClientID::Invalid);
			}
		}
		auto [full_greens, full_green] = draw(true);
		auto [empty_greens, empty_green] = draw(false);
		IConsolePrint(CC_DEFAULT, "testzelenest: {} '{}' plny {} zelenych obrazku ({} zelenych pixelu), prazdny {}", st->index, GetString(st->info.string_id), full_greens, full_green, empty_greens);
		if (full_greens == 0 || full_green == 0) refuse(fmt::format("plny {} nema zeleny naklad", st->index));
		Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, t->index, true, false, ClientID::Invalid);
	}
	for (const auto &[layer, green] : GreenLayerSprites()) {
		IConsolePrint(CC_DEFAULT, "testzelenest: obrazek sady {} -> zeleny {}", layer, green);
	}
	return true;
}

/**
 * Build the road-on-rail scene: one straight line with a shed at each end and
 * two through platforms, a road alongside with a drive-through stop at each
 * platform belonging to the same station, a road shed, one train (engine and
 * an empty wagon) shuttling between the two stations, and one road vehicle
 * ordered to board the train at the first and get off at the second. See
 * road_on_rail.h.
 * Usage: testautovlak [how many road vehicles, 1 by default]
 *        [posun|vlakem|tirak] [vagon=<part of a name>] [auto=<part of a name>]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRoadOnRail(std::span<std::string_view> all_args)
{
	if (all_args.empty()) {
		IConsolePrint(CC_HELP, "Build the road-vehicle-on-train scene. Usage: 'testautovlak [pocet aut] [posun|vlakem|tirak] [vagon=<jmeno>] [auto=<jmeno>]'.");
		return true;
	}
	/* "vagon=" and "auto=" pick the wagon and the road vehicle by a piece of
	 * their name, wherever they stand on the line -- a set's own wagon and car
	 * put side by side, which is what a set's author needs to see measured.
	 * The rest of the arguments keep their places. */
	std::string want_wagon_name;
	std::string want_road_name;
	std::vector<std::string_view> positional;
	for (std::string_view a : all_args) {
		if (a.starts_with("vagon=")) {
			want_wagon_name = a.substr(6);
		} else if (a.starts_with("auto=")) {
			want_road_name = a.substr(5);
		} else {
			positional.push_back(a);
		}
	}
	std::span<std::string_view> argv(positional);
	auto name_has = [](EngineID e, std::string_view piece) {
		std::string name = GetString(STR_ENGINE_NAME, e);
		std::string lower_name = name, lower_piece{piece};
		std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return std::tolower(c); });
		std::transform(lower_piece.begin(), lower_piece.end(), lower_piece.begin(), [](unsigned char c) { return std::tolower(c); });
		return lower_name.find(lower_piece) != std::string::npos;
	};
	uint cars = 1;
	if (argv.size() >= 2) {
		auto pcars = ParseInteger(argv[1]);
		if (!pcars.has_value() || *pcars < 1) return false;
		cars = (uint)*pcars;
	}
	/* "posun": the train is a shunter with nowhere to go -- one order, the
	 * first station, where it then stands -- and the cars are told to board
	 * any train standing there, wherever it goes, rather than one bound for
	 * their next stop. A car boards it that way and never the other way.
	 * "vlakem" keeps the shunter but gives the cars the by-train order, which
	 * asks where the train is going; that is the control. */
	bool shunter = argv.size() >= 3 && (argv[2] == "posun" || argv[2] == "vlakem");
	bool by_train = argv.size() >= 3 && argv[2] == "vlakem";
	/* "tirak": a road vehicle made of several pieces -- a lorry with a trailer
	 * -- where the game has one, which is what the wagon lengths and the way
	 * a vehicle is laid out on a wagon are about. The game's own set has
	 * none, so this only bites with a set loaded. */
	bool want_long = argv.size() >= 3 && argv[2] == "tirak";
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testautovlak: only in a running game.");
		return true;
	}

	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testautovlak: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	AutoRestoreBackup cur_company(_current_company, _local_company);

	EngineID eid_loco = EngineID::Invalid();
	EngineID eid_wagon = EngineID::Invalid();
	uint longest_wagon = 0;
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type == RailVehicleType::Wagon) {
			/* Only a car carrier takes a car (IsCarCarrierWagon()). */
			if (!CanCarryRoadVehicles(e)) continue;
			if (!want_wagon_name.empty()) {
				if (eid_wagon == EngineID::Invalid() && name_has(e->index, want_wagon_name)) eid_wagon = e->index;
				continue;
			}
			/* For a lorry and trailer, the longest wagon the game has: one
			 * built out of several pieces, which is how a set builds a wagon
			 * longer than the game's own can be. Nothing shorter can carry
			 * one, so the car carrier would leave the scene with a lorry that
			 * rightly never boards. */
			if (want_long) {
				uint pieces = CountArticulatedParts(e->index);
				if (pieces > longest_wagon) {
					longest_wagon = pieces;
					eid_wagon = e->index;
				}
				continue;
			}
			/* The car carrier for choice -- it is the wagon the player buys for
			 * this -- and any wagon if this game has none, since the scene then
			 * still has the refit to fall back on. */
			if (e->info.cargo_type == _road_vehicle_cargo) {
				eid_wagon = e->index;
			} else if (eid_wagon == EngineID::Invalid()) {
				eid_wagon = e->index;
			}
		} else if (eid_loco == EngineID::Invalid()) {
			eid_loco = e->index;
		}
	}
	EngineID eid_road = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (GetRoadTramType(e->VehInfo<RoadVehicleInfo>().roadtype) != RoadTramType::Road) continue;
		if (!want_road_name.empty() && !name_has(e->index, want_road_name)) continue;
		if (want_long && CountArticulatedParts(e->index) == 0) continue;
		/* A name matched whole beats one that only contains the piece: a set
		 * names its vans "van", "van with a garden", "van with a garden and a
		 * crate", and asking for the van must not bring the garden. */
		if (!want_road_name.empty() && eid_road != EngineID::Invalid() &&
				GetString(STR_ENGINE_NAME, eid_road).size() <= GetString(STR_ENGINE_NAME, e->index).size()) continue;
		eid_road = e->index;
		if (want_road_name.empty()) break;
	}
	if (!want_wagon_name.empty() && eid_wagon == EngineID::Invalid()) {
		for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
			if (!e->company_avail.Test(_local_company) || RailVehInfo(e->index)->railveh_type != RailVehicleType::Wagon) continue;
			IConsolePrint(CC_INFO, "testautovlak: k mani vagon '{}' ({} dilu)", GetString(STR_ENGINE_NAME, e->index), CountArticulatedParts(e->index) + 1);
		}
	}
	if (!want_road_name.empty() && eid_road == EngineID::Invalid()) {
		for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
			if (!e->company_avail.Test(_local_company)) continue;
			IConsolePrint(CC_INFO, "testautovlak: k mani auto '{}' ({} dilu)", GetString(STR_ENGINE_NAME, e->index), CountArticulatedParts(e->index) + 1);
		}
	}
	if (eid_loco == EngineID::Invalid() || eid_wagon == EngineID::Invalid() || eid_road == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testautovlak: ODMITNUTO - no engine, wagon or road vehicle available{}{}.",
				want_wagon_name.empty() ? "" : fmt::format(" (vagon '{}' {})", want_wagon_name, eid_wagon == EngineID::Invalid() ? "nenalezen" : "ok"),
				want_road_name.empty() ? "" : fmt::format(" (auto '{}' {})", want_road_name, eid_road == EngineID::Invalid() ? "nenalezeno" : "ok"));
		return true;
	}
	IConsolePrint(CC_DEFAULT, "testautovlak: vagon '{}' ({} dilu), auto '{}' ({} dilu).",
			GetString(STR_ENGINE_NAME, eid_wagon), CountArticulatedParts(eid_wagon) + 1,
			GetString(STR_ENGINE_NAME, eid_road), CountArticulatedParts(eid_road) + 1);
	bool bus = IsCargoInClass(Engine::Get(eid_road)->GetDefaultCargoType(), CargoClass::Passengers);
	RoadStopType stop_type = bus ? RoadStopType::Bus : RoadStopType::Truck;

	/* The flattest clear run: three rows deep this time, rails, road, shed. */
	static const uint LEN = 40;
	TileIndex strip = INVALID_TILE;
	for (uint y = 8; y < Map::SizeY() - 8 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			bool ok = true;
			int z = -1;
			for (uint dy = 0; dy < 3 && ok; dy++) {
				TileIndex t = TileXY(x, y + dy);
				ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
				if (ok) {
					if (dy == 0) z = GetTileZ(t); else if (GetTileZ(t) != z) ok = false;
				}
			}
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					strip = TileXY(x - LEN + 1, y);
					break;
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testautovlak: no flat clear strip of {}x3 tiles found.", LEN);
		return true;
	}
	uint x0 = TileX(strip), y0 = TileY(strip);
	IConsolePrint(CC_DEFAULT, "testautovlak: strip at ({},{})..({},{}).", x0, y0, x0 + LEN - 1, y0);

	TileIndex depot_w = TileXY(x0, y0);
	TileIndex depot_e = TileXY(x0 + LEN - 1, y0);
	if (Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_w, RAILTYPE_RAIL, DiagDirection::SW).Failed() ||
			Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, depot_e, RAILTYPE_RAIL, DiagDirection::NE).Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: depot failed.");
		return true;
	}
	if (Command<Commands::BuildRailLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, y0), TileXY(x0 + 1, y0), RAILTYPE_RAIL, Track::X, false, true).Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: track failed.");
		return true;
	}
	TileIndex st_a = TileXY(x0 + 10, y0);
	TileIndex st_b = TileXY(x0 + 26, y0);
	if (Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, st_a, RAILTYPE_RAIL, Axis::X, 1, 3, STAT_CLASS_DFLT, 0, StationID::Invalid(), false).Failed() ||
			Command<Commands::BuildRailStation>::Do(DoCommandFlag::Execute, st_b, RAILTYPE_RAIL, Axis::X, 1, 3, STAT_CLASS_DFLT, 0, StationID::Invalid(), true).Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: station failed.");
		return true;
	}
	for (uint sx : {x0 + 2, x0 + 8, x0 + 14, x0 + 24, x0 + 30, x0 + LEN - 3}) {
		if (Command<Commands::BuildSignal>::Do(DoCommandFlag::Execute, TileXY(sx, y0), Track::X, SignalType::Path, SignalVariant::Electric, false, false, false, SignalType::Block, SignalType::Block, 0, 0).Failed()) {
			IConsolePrint(CC_ERROR, "testautovlak: signal at ({},{}) failed.", sx, y0);
			return true;
		}
	}
	StationID id_a = GetStationIndex(st_a);
	StationID id_b = GetStationIndex(st_b);

	/* The road one row over, with a stop beside each platform joined to its
	 * station, and a shed at the west end. */
	CommandCost road = Command<Commands::BuildRoadLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, y0 + 1), TileXY(x0 + 1, y0 + 1),
			ROADTYPE_ROAD, Axis::X, DisallowedRoadDirections{}, false, false, false);
	if (road.Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: road failed - {}", RefusalReason(road));
		return true;
	}
	CommandCost rs_a = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, TileXY(x0 + 11, y0 + 1), 1, 1, stop_type, true, DiagDirection::NE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, id_a, false);
	CommandCost rs_b = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, TileXY(x0 + 27, y0 + 1), 1, 1, stop_type, true, DiagDirection::NE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, id_b, false);
	if (rs_a.Failed() || rs_b.Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: road stop failed - {} / {}", RefusalReason(rs_a), RefusalReason(rs_b));
		return true;
	}
	TileIndex road_depot = TileXY(x0 + 3, y0 + 2);
	CommandCost rd = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, road_depot, ROADTYPE_ROAD, DiagDirection::NW);
	/* A straight road has no piece pointing at a shed beside it; the vehicle
	 * drove out of the door onto nothing and turned round for good. */
	CommandCost rd_link = Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, TileXY(x0 + 3, y0 + 1), RoadBits{RoadBit::SE}, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
	if (rd.Failed() || rd_link.Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: road depot failed - {} / {}", RefusalReason(rd), RefusalReason(rd_link));
		return true;
	}
	UpdateSignalsInBuffer();

	/* The train: engine and one empty wagon, shuttling A - B. */
	auto [cost_l, veh_l, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	/* Bought fitted for road vehicles, the way the buy window does it. */
	auto [cost_w, veh_w, un_d, un_e, un_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_w, eid_wagon, true, _road_vehicle_cargo, ClientID::Invalid);
	if (cost_l.Failed() || cost_w.Failed()) {
		IConsolePrint(CC_ERROR, "testautovlak: train failed - {} / {}", RefusalReason(cost_l), RefusalReason(cost_w));
		return true;
	}
	Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, veh_w, veh_l, false);
	Order to_a{};
	to_a.MakeGoToStation(id_a);
	to_a.SetNonStopType(OrderNonStopFlags{OrderNonStopFlag::NonStop});
	Order to_b{};
	to_b.MakeGoToStation(id_b);
	to_b.SetNonStopType(OrderNonStopFlags{OrderNonStopFlag::NonStop});
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_l, 0, to_a);
	if (!shunter) Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_l, 1, to_b);
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_l, false);

	/* The road vehicles: to A to board, then B. One is enough to see the ride
	 * work; more than one is what the "how many are waiting" condition is
	 * about, since a one-wagon train can only take the first of them. */
	std::string built;
	for (uint n = 0; n < cars; n++) {
		auto [cost_r, veh_r, un_g, un_h, un_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, road_depot, eid_road, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_r.Failed()) {
			IConsolePrint(CC_ERROR, "testautovlak: road vehicle failed - {}", RefusalReason(cost_r));
			return true;
		}
		/* Road vehicles have no choice of where on a platform to stop, and the
		 * order code insists on the one answer that means that. */
		Order car_a{};
		car_a.MakeGoToStation(id_a);
		car_a.SetStopLocation(OrderStopLocation::FarEnd);
		Order car_b{};
		car_b.MakeGoToStation(id_b);
		car_b.SetStopLocation(OrderStopLocation::FarEnd);
		CommandCost ins_a = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_r, 0, car_a);
		CommandCost ins_b = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_r, 1, car_b);
		if (ins_a.Failed() || ins_b.Failed()) {
			IConsolePrint(CC_ERROR, "testautovlak: road vehicle orders refused - {} / {} (stop tiles belong to stations {} and {})",
					RefusalReason(ins_a), RefusalReason(ins_b), GetStationIndex(TileXY(x0 + 11, y0 + 1)), GetStationIndex(TileXY(x0 + 27, y0 + 1)));
			return true;
		}
		CommandCost mod = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, veh_r, 0, MOF_BOARD_MODE,
				to_underlying((shunter && !by_train) ? OrderBoardMode::TrainAnywhere : OrderBoardMode::TrainToNext));
		if (mod.Failed()) {
			IConsolePrint(CC_ERROR, "testautovlak: load-on-train order refused - {}", RefusalReason(mod));
			return true;
		}
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_r, false);
		if (!built.empty()) built += ",";
		built += fmt::format("{}", RoadVehicle::Get(veh_r)->unitnumber);
	}

	_testspoj_active = true;
	IConsolePrint(CC_DEFAULT, "testautovlak: scene ready. vlak={}, auta={} ({}), stanice A={} ({},{}), B={} ({},{}).",
			Train::Get(veh_l)->unitnumber, built, bus ? "bus" : "nakladak",
			id_a, x0 + 10, y0, id_b, x0 + 26, y0);
	return true;
}

/**
 * Flip a single vehicle standing in a depot, the way Ctrl+click in the depot
 * window does. Usage: testpreklop <unit number> [vehicle position, 0 = head]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestFlipInDepot(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Flip one vehicle of a train standing in a depot. Usage: 'testpreklop <unit number> [position]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	uint pos = 0;
	if (argv.size() > 2) {
		auto ppos = ParseInteger(argv[2]);
		if (!ppos.has_value()) return false;
		pos = (uint)*ppos;
	}
	Train *t = FindTrainByUnit((uint)*punit);
	if (t == nullptr) {
		IConsolePrint(CC_ERROR, "testpreklop: vlak {} nenalezen.", argv[1]);
		return true;
	}
	Train *u = t;
	for (uint i = 0; i < pos && u != nullptr; i++) u = u->GetNextVehicle();
	if (u == nullptr) {
		IConsolePrint(CC_ERROR, "testpreklop: vlak {} nema clanek c.{}.", argv[1], pos);
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, t->owner);
	CommandCost ret = Command<Commands::ReverseTrainDirection>::Do(DoCommandFlag::Execute, u->index, true);
	IConsolePrint(CC_DEFAULT, "testpreklop: vlak {} clanek {}: {} (otoceny {})", t->unitnumber, u->index.base(),
			ret.Failed() ? GetString(ret.GetErrorMessage()) : "preklopen", u->flags.Test(VehicleRailFlag::Flipped) ? "ano" : "ne");
	return true;
}

/**
 * Move a whole train behind the last vehicle of another one, both standing in
 * the same depot -- the drag in the depot window. Usage: testpresun <src> <dst>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestMoveInDepot(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Hang a train on the tail of another in a depot. Usage: 'testpresun <unit number> <unit number of the train to join> [ctrl]' ('ctrl' drags the whole chain, as Ctrl does).");
		return true;
	}
	bool ctrl = argv.size() > 3 && argv[3] == "ctrl";
	auto psrc = ParseInteger(argv[1]);
	auto pdst = ParseInteger(argv[2]);
	if (!psrc.has_value() || !pdst.has_value()) return false;
	Train *src = FindTrainByUnit((uint)*psrc);
	Train *dst = FindTrainByUnit((uint)*pdst);
	if (src == nullptr || dst == nullptr) {
		IConsolePrint(CC_ERROR, "testpresun: vlak nenalezen.");
		return true;
	}
	AutoRestoreBackup cur_company(_current_company, src->owner);
	CommandCost ret = Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, src->index, dst->Last()->index, ctrl);
	IConsolePrint(CC_DEFAULT, "testpresun: vlak {} za vlak {}{}: {}", *psrc, *pdst, ctrl ? " (ctrl)" : "", ret.Failed() ? GetString(ret.GetErrorMessage()) : "presunut");
	return true;
}

/**
 * Tear a train open: move its leading vehicle on by a few pixels while
 * everything behind it stands still, so the consist runs with a hole in it.
 *
 * Stages the one thing the game has crashed on four times over and the rig
 * could not make on purpose once the couplings that used to make it were
 * mended: a follower that steps onto a tile after the vehicle ahead of it has
 * already left that tile's world -- gone into a depot, mostly. The hole is
 * made the way the game itself moves vehicles, one step of TrainController()
 * at a time with the rest of the train held, so the train stays on its rails
 * and only the spacing is wrong. Usage: testmezera <unit number> <pixels>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestTearConsist(std::span<std::string_view> argv)
{
	if (argv.size() != 3) {
		IConsolePrint(CC_HELP, "Open a gap behind a train's leading vehicle. Usage: 'testmezera <unit number> <pixels>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto ppx = ParseInteger(argv[2]);
	if (!punit.has_value() || !ppx.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		Train *front = t->GetMovingFront();
		Train *held = front->GetMovingNext();
		if (held == nullptr) {
			IConsolePrint(CC_ERROR, "testmezera: vlak {} ma jediny clanek, neni co roztrhnout.", t->unitnumber);
			return true;
		}
		if (front->track == Track::Depot) {
			IConsolePrint(CC_ERROR, "testmezera: vlak {} stoji v depu, tam clanky nemaji rozmer.", t->unitnumber);
			return true;
		}
		/* The step needs an acting company, like every move a train makes. */
		AutoRestoreBackup cur_company(_current_company, t->owner);
		uint done = 0;
		for (; done < (uint)*ppx; done++) {
			if (!TrainController(front, held, false)) break;
		}
		int dx = front->x_pos - held->x_pos, dy = front->y_pos - held->y_pos;
		IConsolePrint(CC_DEFAULT, "testmezera: vlak {} roztrzen o {} px za clankem {} - rozestup k clanku {} je ted {} px (chce {})",
				t->unitnumber, done, front->index.base(), held->index.base(), (int)std::sqrt(dx * dx + dy * dy), front->CalcNextVehicleOffset());
		return true;
	}
	IConsolePrint(CC_ERROR, "testmezera: vlak {} nenalezen.", argv[1]);
	return true;
}

/**
 * Skip a train's current order, the same as the player's skip button.
 * Meant for staged scenes: a parked obstacle train is released by skipping
 * the order that holds it. Usage: testskip <unit number>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestSkipOrder(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Skip a train's current order. Usage: 'testskip <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	for (Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != (UnitID)*punit) continue;
		if (t->GetNumOrders() == 0) {
			IConsolePrint(CC_ERROR, "testskip: vlak {} nema rozkazy.", t->unitnumber);
			return true;
		}
		VehicleOrderID next = (VehicleOrderID)((t->cur_implicit_order_index + 1) % t->GetNumOrders());
		/* Fired from the heartbeat timer there is no acting company set, and
		 * the command would bounce off its ownership check, silently. */
		AutoRestoreBackup cur_company(_current_company, t->owner);
		Command<Commands::SkipToOrder>::Do(DoCommandFlag::Execute, t->index, next);
		IConsolePrint(CC_DEFAULT, "testskip: vlak {} preskocil na rozkaz {}.", t->unitnumber, next);
		return true;
	}
	IConsolePrint(CC_ERROR, "testskip: vlak {} nenalezen.", argv[1]);
	return true;
}

/** A train being followed closely: unit number and how often it is reported. */
static UnitID _testsleduj_unit = 0;
static uint _testsleduj_every = 50;
static uint _testsleduj_ticks = 0;

/** Every few ticks, say where the followed train is and what it is doing. */
static const IntervalTimer<TimerGameTick> _testsleduj_timer({TimerGameTick::Priority::None, 10}, [](auto) {
	if (_testsleduj_unit == 0) return;
	_testsleduj_ticks += 10;
	if (_testsleduj_ticks < _testsleduj_every) return;
	_testsleduj_ticks = 0;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || t->unitnumber != _testsleduj_unit) continue;
		const Train *front = t->GetMovingFront();
		IConsolePrint(CC_DEFAULT, "sleduj {}: ({},{}) px ({},{}) smer {} kolej {:#x} rychlost {} rozkaz {} cil ({},{}) zasekly {} couva {} rez-pod {:#x}",
				t->unitnumber, TileX(front->tile), TileY(front->tile), front->x_pos, front->y_pos, to_underlying(front->direction),
				front->track.base(), t->cur_speed, to_underlying(t->current_order.GetType()),
				TileX(t->dest_tile), TileY(t->dest_tile),
				t->flags.Test(VehicleRailFlag::Stuck) ? "ano" : "ne",
				t->vehicle_flags.Test(VehicleFlag::DrivingBackwards) ? "ano" : "ne",
				IsTileType(front->tile, TileType::Railway) || IsRailStationTile(front->tile) ? GetReservedTrackbits(front->tile) : TrackBits{});
		PBSTileInfo res = FollowTrainReservation(t);
		IConsolePrint(CC_DEFAULT, "  max-rychlost {} brzdi-do-stanice {} wait {} force {} stuck {} rez-konec ({},{}) {} ceka-na-spojeni {} partner-cil {} narok {} odtah-drzi {}",
				t->GetCurrentMaxSpeed(), t->vehstatus.Test(VehState::TrainSlowing) ? "ano" : "ne", t->wait_counter, to_underlying(t->force_proceed),
				t->flags.Test(VehicleRailFlag::Stuck) ? "ano" : "ne",
				res.tile == INVALID_TILE ? 0 : TileX(res.tile), res.tile == INVALID_TILE ? 0 : TileY(res.tile), res.okay ? "bezpecny" : "NEbezpecny",
				IsWaitingToBeCoupled(t) ? "ano" : "ne", t->couple_target.base(), t->couple_claim.base(), to_underlying(t->rescue_hold));
		/* Where each vehicle stands and what is booked under it: a collision
		 * is always two trains with a road booked over the same ground. */
		std::string under;
		uint flipped = 0;
		for (const Train *u = t; u != nullptr; u = u->Next()) {
			TrackBits res = IsTileType(u->tile, TileType::Railway) || IsRailStationTile(u->tile) || IsRailDepotTile(u->tile) ? GetReservedTrackbits(u->tile) : TrackBits{};
			if (u->flags.Test(VehicleRailFlag::Flipped)) flipped++;
			fmt::format_to(std::back_inserter(under), " ({},{}) k{:#x}/r{:#x}/s{}", TileX(u->tile), TileY(u->tile), u->track.base(), res.base(), to_underlying(u->direction));
		}
		IConsolePrint(CC_DEFAULT, "  porucha {}/{} otocenych {} vozy:{}", t->breakdown_ctr, t->breakdown_delay, flipped, under);
		return;
	}
});

/**
 * Say where every train is, one line each, once.
 * Usage: 'testkde'
 *
 * 'testsleduj' follows one train and needs its number; on a save brought in
 * from the player's game nobody knows the numbers yet, and the question is
 * usually "who is standing and where", which is the whole list at once.
 * @copydoc IConsoleCmdProc
 */
static bool ConTestWhere(std::span<std::string_view> argv)
{
	if (argv.empty()) return true;
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFrontEngine()) continue;
		const Train *front = t->GetMovingFront();
		PBSTileInfo res = FollowTrainReservation(t);
		const char *kde = "trat";
		if (t->IsInDepot()) kde = "depo";
		else if (IsTileType(front->tile, TileType::TunnelBridge)) kde = front->track == Track::Wormhole ? "v roure" : "usti";
		else if (IsRailStationTile(front->tile)) kde = "stanice";
		IConsolePrint(CC_DEFAULT, "kde {}: ({},{}) {} rychlost {}/{} rozkaz {} zasekly {} stoji {} rez-konec ({},{}) {}",
				t->unitnumber, TileX(front->tile), TileY(front->tile), kde,
				t->cur_speed, t->GetCurrentMaxSpeed(), to_underlying(t->current_order.GetType()),
				t->flags.Test(VehicleRailFlag::Stuck) ? "ano" : "ne",
				t->vehstatus.Test(VehState::Stopped) ? "ano" : "ne",
				res.tile == INVALID_TILE ? 0 : TileX(res.tile), res.tile == INVALID_TILE ? 0 : TileY(res.tile),
				res.okay ? "bezpecny" : "NEbezpecny");
	}
	return true;
}

/**
 * Follow one train closely: report it every few ticks.
 * Usage: testsleduj <unit number> [ticks]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestFollow(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Report a train every few ticks. Usage: 'testsleduj <unit number> [ticks]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
	_testsleduj_unit = (UnitID)*punit;
	if (argv.size() >= 3) {
		auto pev = ParseInteger(argv[2]);
		if (!pev.has_value()) return false;
		_testsleduj_every = (uint)*pev;
	}
	return true;
}

/** While the test scene runs, say where everybody stands every few seconds. */
static const IntervalTimer<TimerGameTick> _testspoj_heartbeat({TimerGameTick::Priority::None, 1000}, [](auto) {
	if (!_testspoj_active) return;
	if (_testklon_delay > 0) {
		_testklon_delay -= 1000;
		if (_testklon_delay <= 0) DoTestClone(_testklon_unit, _testklon_count, _testklon_reverz);
	}
	for (auto it = _testza_queue.begin(); it != _testza_queue.end(); ) {
		it->first -= 1000;
		if (it->first <= 0) {
			std::string cmd = std::move(it->second);
			it = _testza_queue.erase(it);
			IConsoleCmdExec(cmd);
		} else {
			++it;
		}
	}
	std::string_view name = "teststav";
	std::span<std::string_view> args(&name, 1);
	ConTestCoupleState(args);
});

/**
 * Scroll to a tile on the map.
 * @copydoc IConsoleCmdProc
 */
static bool ConScrollToTile(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Center the screen on a given tile.");
		IConsolePrint(CC_HELP, "Usage: 'scrollto [instant] <tile>' or 'scrollto [instant] <x> <y>'.");
		IConsolePrint(CC_HELP, "Numbers can be either decimal (34161) or hexadecimal (0x4a5B).");
		IConsolePrint(CC_HELP, "'instant' will immediately move and redraw viewport without smooth scrolling.");
		return true;
	}
	if (argv.size() < 2) return false;

	uint32_t arg_index = 1;
	bool instant = false;
	if (argv[arg_index] == "instant") {
		++arg_index;
		instant = true;
	}

	switch (argv.size() - arg_index) {
		case 1: {
			auto result = ParseInteger(argv[arg_index], 0);
			if (result.has_value()) {
				if (*result >= Map::Size()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				ScrollMainWindowToTile(TileIndex{*result}, instant);
				return true;
			}
			break;
		}

		case 2: {
			auto x = ParseInteger(argv[arg_index], 0);
			auto y = ParseInteger(argv[arg_index + 1], 0);
			if (x.has_value() && y.has_value()) {
				if (*x >= Map::SizeX() || *y >= Map::SizeY()) {
					IConsolePrint(CC_ERROR, "Tile does not exist.");
					return true;
				}
				ScrollMainWindowToTile(TileXY(*x, *y), instant);
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Save the map to a file.
 * @copydoc IConsoleCmdProc
 */
static bool ConSave(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Save the current game. Usage: 'save <filename>'.");
		return true;
	}

	if (argv.size() == 2) {
		std::string filename = fmt::format("{}.sav", argv[1]);
		IConsolePrint(CC_DEFAULT, "Saving map...");

		if (SaveOrLoad(filename, SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::Save) != SaveLoadResult::Ok) {
			IConsolePrint(CC_ERROR, "Saving map failed.");
		} else {
			IConsolePrint(CC_INFO, "Map successfully saved to '{}'.", filename);
		}
		return true;
	}

	return false;
}

/**
 * Explicitly save the configuration.
 * @copydoc IConsoleCmdProc
 */
static bool ConSaveConfig(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Saves the configuration for new games to the configuration file, typically 'openttd.cfg'.");
		IConsolePrint(CC_HELP, "It does not save the configuration of the current game to the configuration file.");
		return true;
	}

	SaveToConfig();
	IConsolePrint(CC_DEFAULT, "Saved config.");
	return true;
}

/** Load a savegame. @copydoc IConsoleCmdProc */
static bool ConLoad(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Load a game by name or index. Usage: 'load <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_savegame.ValidateFileList();
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Savegame) {
			_switch_mode = SwitchMode::LoadGame;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a savegame.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

/** Load a scenario. @copydoc IConsoleCmdProc */
static bool ConLoadScenario(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Load a scenario by name or index. Usage: 'load_scenario <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_scenario.ValidateFileList();
	const FiosItem *item = _console_file_list_scenario.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Scenario) {
			_switch_mode = SwitchMode::LoadGame;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a scenario.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

/** Load a heightmap. @copydoc IConsoleCmdProc */
static bool ConLoadHeightmap(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Load a heightmap by name or index. Usage: 'load_heightmap <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_heightmap.ValidateFileList();
	const FiosItem *item = _console_file_list_heightmap.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Heightmap) {
			_switch_mode = SwitchMode::StartHeightmap;
			_file_to_saveload.Set(*item);
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a heightmap.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' cannot be found.", file);
	}

	return true;
}

/** Remove a savegame file from disk. @copydoc IConsoleCmdProc */
static bool ConRemove(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Remove a savegame by name or index. Usage: 'rm <file | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_savegame.ValidateFileList();
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		if (item->type.abstract == AbstractFileType::Savegame) {
			if (!FioRemove(item->name)) {
				IConsolePrint(CC_ERROR, "Failed to delete '{}'.", item->name);
			}
		} else {
			IConsolePrint(CC_ERROR, "'{}' is not a savegame.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "'{}' could not be found.", file);
	}

	_console_file_list_savegame.InvalidateFileList();
	return true;
}


/** List all the files in the current dir via console. @copydoc IConsoleCmdProc */
static bool ConListFiles(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all loadable savegames and directories in the current dir via console. Usage: 'ls | dir'.");
		return true;
	}

	_console_file_list_savegame.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_savegame.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_savegame[i].title.GetDecodedString());
	}

	return true;
}

/** List all the scenarios. @copydoc IConsoleCmdProc */
static bool ConListScenarios(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all loadable scenarios. Usage: 'list_scenarios'.");
		return true;
	}

	_console_file_list_scenario.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_scenario.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_scenario[i].title.GetDecodedString());
	}

	return true;
}

/** List all the heightmaps. @copydoc IConsoleCmdProc */
static bool ConListHeightmaps(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all loadable heightmaps. Usage: 'list_heightmaps'.");
		return true;
	}

	_console_file_list_heightmap.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list_heightmap.size(); i++) {
		IConsolePrint(CC_DEFAULT, "{}) {}", i, _console_file_list_heightmap[i].title.GetDecodedString());
	}

	return true;
}

/** Change the dir via console. @copydoc IConsoleCmdProc */
static bool ConChangeDirectory(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change the dir via console. Usage: 'cd <directory | number>'.");
		return true;
	}

	if (argv.size() != 2) return false;

	std::string_view file = argv[1];
	_console_file_list_savegame.ValidateFileList(true);
	const FiosItem *item = _console_file_list_savegame.FindItem(file);
	if (item != nullptr) {
		switch (item->type.detailed) {
			case DetailedFileType::FiosDirectory:
			case DetailedFileType::FiosDrive:
			case DetailedFileType::FiosParent:
				FiosBrowseTo(item);
				break;
			default: IConsolePrint(CC_ERROR, "{}: Not a directory.", file);
		}
	} else {
		IConsolePrint(CC_ERROR, "{}: No such file or directory.", file);
	}

	_console_file_list_savegame.InvalidateFileList();
	return true;
}

/** Print the current working directory. @copydoc IConsoleCmdProc */
static bool ConPrintWorkingDirectory(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print out the current working directory. Usage: 'pwd'.");
		return true;
	}

	/* XXX - Workaround for broken file handling */
	_console_file_list_savegame.ValidateFileList(true);
	_console_file_list_savegame.InvalidateFileList();

	IConsolePrint(CC_DEFAULT, FiosGetCurrentPath());
	return true;
}

/** Clear the console's buffer. @copydoc IConsoleCmdProc */
static bool ConClearBuffer(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Clear the console buffer. Usage: 'clear'.");
		return true;
	}

	IConsoleClearBuffer();
	SetWindowDirty(WindowClass::Console, 0);
	return true;
}


/**********************************
 * Network Core Console Commands
 **********************************/

/**
 * Helper to kick or ban a user.
 * @param arg The client id or IP address.
 * @param ban Whether to ban, when \c false only a kick is performed.
 * @param reason The reason for this action.
 * @return \c true iff the command is handled correctly, i.e. \c false to show a help message.
 */
static bool ConKickOrBan(std::string_view arg, bool ban, std::string_view reason)
{
	uint n;

	if (arg.find_first_of(".:") == std::string::npos) { // banning with ID
		auto client_id = ParseType<ClientID>(arg);
		if (!client_id.has_value()) {
			IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
			return true;
		}

		/* Don't kill the server, or the client doing the rcon. The latter can't be kicked because
		 * kicking frees closes and subsequently free the connection related instances, which we
		 * would be reading from and writing to after returning. So we would read or write data
		 * from freed memory up till the segfault triggers. */
		if (*client_id == ClientID::Server || *client_id == _redirect_console_to_client) {
			IConsolePrint(CC_ERROR, "You can not {} yourself!", ban ? "ban" : "kick");
			return true;
		}

		NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(*client_id);
		if (ci == nullptr) {
			IConsolePrint(CC_ERROR, "Invalid client-id.");
			return true;
		}

		if (!ban) {
			/* Kick only this client, not all clients with that IP */
			NetworkServerKickClient(*client_id, reason);
			return true;
		}

		/* When banning, kick+ban all clients with that IP */
		n = NetworkServerKickOrBanIP(*client_id, ban, reason);
	} else {
		n = NetworkServerKickOrBanIP(arg, ban, reason);
	}

	if (n == 0) {
		IConsolePrint(CC_DEFAULT, ban ? "Client not online, address added to banlist." : "Client not found.");
	} else {
		IConsolePrint(CC_DEFAULT, "{}ed {} client(s).", ban ? "Bann" : "Kick", n);
	}

	return true;
}

/** Kick a user from a network game. @copydoc IConsoleCmdProc */
static bool ConKick(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Kick a client from a network game. Usage: 'kick <ip | client-id> [<kick-reason>]'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	if (argv.size() != 2 && argv.size() != 3) return false;

	/* No reason supplied for kicking */
	if (argv.size() == 2) return ConKickOrBan(argv[1], false, {});

	/* Reason for kicking supplied */
	size_t kick_message_length = argv[2].size();
	if (kick_message_length >= 255) {
		IConsolePrint(CC_ERROR, "Maximum kick message length is 254 characters. You entered {} characters.", kick_message_length);
		return false;
	} else {
		return ConKickOrBan(argv[1], false, argv[2]);
	}
}

/** Ban a user from a network game. @copydoc IConsoleCmdProc */
static bool ConBan(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Ban a client from a network game. Usage: 'ban <ip | client-id> [<ban-reason>]'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		IConsolePrint(CC_HELP, "If the client is no longer online, you can still ban their IP.");
		return true;
	}

	if (argv.size() != 2 && argv.size() != 3) return false;

	/* No reason supplied for kicking */
	if (argv.size() == 2) return ConKickOrBan(argv[1], true, {});

	/* Reason for kicking supplied */
	size_t kick_message_length = argv[2].size();
	if (kick_message_length >= 255) {
		IConsolePrint(CC_ERROR, "Maximum kick message length is 254 characters. You entered {} characters.", kick_message_length);
		return false;
	} else {
		return ConKickOrBan(argv[1], true, argv[2]);
	}
}

/** Unban a user from a network game. @copydoc IConsoleCmdProc */
static bool ConUnBan(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Unban a client from a network game. Usage: 'unban <ip | banlist-index>'.");
		IConsolePrint(CC_HELP, "For a list of banned IP's, see the command 'banlist'.");
		return true;
	}

	if (argv.size() != 2) return false;

	/* Try by IP. */
	uint index;
	for (index = 0; index < _network_ban_list.size(); index++) {
		if (_network_ban_list[index] == argv[1]) break;
	}

	/* Try by index. */
	if (index >= _network_ban_list.size()) {
		index = ParseInteger(argv[1]).value_or(0) - 1U; // let it wrap
	}

	if (index < _network_ban_list.size()) {
		IConsolePrint(CC_DEFAULT, "Unbanned {}.", _network_ban_list[index]);
		_network_ban_list.erase(_network_ban_list.begin() + index);
	} else {
		IConsolePrint(CC_DEFAULT, "Invalid list index or IP not in ban-list.");
		IConsolePrint(CC_DEFAULT, "For a list of banned IP's, see the command 'banlist'.");
	}

	return true;
}

/** Show the list of banned clients. @copydoc IConsoleCmdProc */
static bool ConBanList(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the IP's of banned clients: Usage 'banlist'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Banlist:");

	uint i = 1;
	for (const auto &entry : _network_ban_list) {
		IConsolePrint(CC_DEFAULT, "  {}) {}", i, entry);
		i++;
	}

	return true;
}

/** Manually pause the game. @copydoc IConsoleCmdProc */
static bool ConPauseGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Pause a network game. Usage: 'pause'.");
		return true;
	}

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (!_pause_mode.Test(PauseMode::Normal)) {
		Command<Commands::Pause>::Post(PauseMode::Normal, true);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game paused.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already paused.");
	}

	return true;
}

/** Manually unpause the game. @copydoc IConsoleCmdProc */
static bool ConUnpauseGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Unpause a network game. Usage: 'unpause'.");
		return true;
	}

	if (_game_mode == GameMode::Menu) {
		IConsolePrint(CC_ERROR, "This command is only available in-game and in the editor.");
		return true;
	}

	if (_pause_mode.Test(PauseMode::Normal)) {
		Command<Commands::Pause>::Post(PauseMode::Normal, false);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game unpaused.");
	} else if (_pause_mode.Test(PauseMode::Error)) {
		/* What the player does with the mouse: the red box that says a train has
		 * an invalid length is clicked away and the game is started again. It
		 * comes up on nearly every load of a game with a set in it, his own
		 * included, and there is no mouse here -- so the rig says yes to it, or
		 * no save of his can ever be played in it. */
		Command<Commands::Pause>::Post(PauseMode::Error, false);
		IConsolePrint(CC_DEFAULT, "Game unpaused (chybove okno odklepnuto).");
	} else if (_pause_mode.Any()) {
		IConsolePrint(CC_DEFAULT, "Game cannot be unpaused manually; disable pause_on_join/min_active_clients.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already unpaused.");
	}

	return true;
}

/** Run a console command on the server. @copydoc IConsoleCmdProc */
static bool ConRcon(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Remote control the server from another client. Usage: 'rcon <password> <command>'.");
		IConsolePrint(CC_HELP, "Remember to enclose the command in quotes, otherwise only the first parameter is sent.");
		IConsolePrint(CC_HELP, "When your client's public key is in the 'authorized keys' for 'rcon', the password is not checked and may be '*'.");
		return true;
	}

	if (argv.size() < 3) return false;

	if (_network_server) {
		IConsoleCmdExec(argv[2]);
	} else {
		NetworkClientSendRcon(argv[1], argv[2]);
	}
	return true;
}

/** Get the status of connected clients. @copydoc IConsoleCmdProc */
static bool ConStatus(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the status of all clients connected to the server. Usage 'status'.");
		return true;
	}

	NetworkServerShowStatusToConsole();
	return true;
}

/** Get information like client/company count/limits for the server. @copydoc IConsoleCmdProc */
static bool ConServerInfo(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List current and maximum client/company limits. Usage 'server_info'.");
		IConsolePrint(CC_HELP, "You can change these values by modifying settings 'network.max_clients' and 'network.max_companies'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Invite code:                {}", _network_server_invite_code);
	IConsolePrint(CC_DEFAULT, "Current/maximum clients:    {:3d}/{:3d}", _network_game_info.clients_on, _settings_client.network.max_clients);
	IConsolePrint(CC_DEFAULT, "Current/maximum companies:  {:3d}/{:3d}", Company::GetNumItems(), _settings_client.network.max_companies);
	IConsolePrint(CC_DEFAULT, "Current spectators:         {:3d}", NetworkSpectatorCount());

	return true;
}

/** Change the name of a client. @copydoc IConsoleCmdProc */
static bool ConClientNickChange(std::span<std::string_view> argv)
{
	if (argv.size() != 3) {
		IConsolePrint(CC_HELP, "Change the nickname of a connected client. Usage: 'client_name <client-id> <new-name>'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	auto client_id = ParseType<ClientID>(argv[1]);
	if (!client_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
		return true;
	}

	if (*client_id == ClientID::Server) {
		IConsolePrint(CC_ERROR, "Please use the command 'name' to change your own name!");
		return true;
	}

	if (NetworkClientInfo::GetByClientID(*client_id) == nullptr) {
		IConsolePrint(CC_ERROR, "Invalid client-id.");
		return true;
	}

	std::string client_name{StrTrimView(argv[2], StringConsumer::WHITESPACE_NO_NEWLINE)};
	if (!NetworkIsValidClientName(client_name)) {
		IConsolePrint(CC_ERROR, "Cannot give a client an empty name.");
		return true;
	}

	if (!NetworkServerChangeClientName(*client_id, client_name)) {
		IConsolePrint(CC_ERROR, "Cannot give a client a duplicate name.");
	}

	return true;
}

/**
 * Helper to parse a company ID. Note that 'Company #1' has ID 0.
 * @param arg The string to get the company ID from.
 * @return The company's ID, or std::nullopt when no valid ID was found.
 */
static std::optional<CompanyID> ParseCompanyID(std::string_view arg)
{
	auto company_id = ParseType<CompanyID>(arg);
	if (company_id.has_value() && *company_id <= MAX_COMPANIES) return static_cast<CompanyID>(*company_id - 1);
	return company_id;
}

/** As client, join a company. @copydoc IConsoleCmdProc */
static bool ConJoinCompany(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Request joining another company. Usage: 'join <company-id>'.");
		IConsolePrint(CC_HELP, "For valid company-id see company list, use 255 for spectator.");
		return true;
	}

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	const NetworkClientInfo *info = NetworkClientInfo::GetByClientID(_network_own_client_id);
	if (info == nullptr) {
		IConsolePrint(CC_ERROR, "You have not joined the game yet!");
		return true;
	}

	/* Check we have a valid company id! */
	if (!Company::IsValidID(*company_id) && *company_id != COMPANY_SPECTATOR) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (info->client_playas == *company_id) {
		IConsolePrint(CC_ERROR, "You are already there!");
		return true;
	}

	if (*company_id != COMPANY_SPECTATOR && !Company::IsHumanID(*company_id)) {
		IConsolePrint(CC_ERROR, "Cannot join AI company.");
		return true;
	}

	if (!info->CanJoinCompany(*company_id)) {
		IConsolePrint(CC_ERROR, "You are not allowed to join this company.");
		return true;
	}

	/* non-dedicated server may just do the move! */
	if (_network_server) {
		NetworkServerDoMove(ClientID::Server, *company_id);
	} else {
		NetworkClientRequestMove(*company_id);
	}

	return true;
}

/** Move a client to a specific company. @copydoc IConsoleCmdProc */
static bool ConMoveClient(std::span<std::string_view> argv)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, "Move a client to another company. Usage: 'move <client-id> <company-id>'.");
		IConsolePrint(CC_HELP, "For valid client-id see 'clients', for valid company-id see 'companies', use 255 for moving to spectators.");
		return true;
	}

	auto client_id = ParseType<ClientID>(argv[1]);
	if (!client_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
		return true;
	}
	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(*client_id);

	auto company_id = ParseCompanyID(argv[2]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	/* check the client exists */
	if (ci == nullptr) {
		IConsolePrint(CC_ERROR, "Invalid client-id, check the command 'clients' for valid client-id's.");
		return true;
	}

	if (!Company::IsValidID(*company_id) && *company_id != COMPANY_SPECTATOR) {
		IConsolePrint(CC_ERROR, "Company does not exist. Company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (*company_id != COMPANY_SPECTATOR && !Company::IsHumanID(*company_id)) {
		IConsolePrint(CC_ERROR, "You cannot move clients to AI companies.");
		return true;
	}

	if (ci->client_id == ClientID::Server && _network_dedicated) {
		IConsolePrint(CC_ERROR, "You cannot move the server!");
		return true;
	}

	if (ci->client_playas == *company_id) {
		IConsolePrint(CC_ERROR, "You cannot move someone to where they already are!");
		return true;
	}

	/* we are the server, so force the update */
	NetworkServerDoMove(ci->client_id, *company_id);

	return true;
}

/** Remove a company from the game. @copydoc IConsoleCmdProc */
static bool ConResetCompany(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Remove an idle company from the game. Usage: 'reset_company <company-id>'.");
		IConsolePrint(CC_HELP, "For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (argv.size() != 2) return false;

	auto index = ParseCompanyID(argv[1]);
	if (!index.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	/* Check valid range */
	if (!Company::IsValidID(*index)) {
		IConsolePrint(CC_ERROR, "Company does not exist. company-id must be between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (!Company::IsHumanID(*index)) {
		IConsolePrint(CC_ERROR, "Company is owned by an AI.");
		return true;
	}

	if (NetworkCompanyHasClients(*index)) {
		IConsolePrint(CC_ERROR, "Cannot remove company: a client is connected to that company.");
		return false;
	}
	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(ClientID::Server);
	assert(ci != nullptr);
	if (ci->client_playas == *index) {
		IConsolePrint(CC_ERROR, "Cannot remove company: the server is connected to that company.");
		return true;
	}

	/* It is safe to remove this company */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::Delete, *index, CompanyRemoveReason::Manual, ClientID::Invalid);
	IConsolePrint(CC_DEFAULT, "Company deleted.");

	return true;
}

/** List the clients. @copydoc IConsoleCmdProc */
static bool ConNetworkClients(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Get a list of connected clients including their ID, name, company-id, and IP. Usage: 'clients'.");
		return true;
	}

	NetworkPrintClients();

	return true;
}

/** Connect to the last client you were connected to. @copydoc IConsoleCmdProc */
static bool ConNetworkReconnect(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reconnect to server to which you were connected last time. Usage: 'reconnect [<company-id>]'.");
		IConsolePrint(CC_HELP, "Company 255 is spectator (default, if not specified), 254 means creating new company.");
		IConsolePrint(CC_HELP, "All others are a certain company with Company 1 being #1.");
		return true;
	}

	CompanyID playas = COMPANY_SPECTATOR;
	if (argv.size() >= 2) {
		auto company_id = ParseCompanyID(argv[1]);
		if (!company_id.has_value()) {
			IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
			return true;
		}
		if (*company_id >= MAX_COMPANIES && *company_id != COMPANY_NEW_COMPANY && *company_id != COMPANY_SPECTATOR) return false;
		playas = *company_id;
	}

	if (_settings_client.network.last_joined.empty()) {
		IConsolePrint(CC_DEFAULT, "No server for reconnecting.");
		return true;
	}

	/* Don't resolve the address first, just print it directly as it comes from the config file. */
	IConsolePrint(CC_DEFAULT, "Reconnecting to {} ...", _settings_client.network.last_joined);

	return NetworkClientConnectGame(_settings_client.network.last_joined, playas);
}

/** Connect to a specific server. @copydoc IConsoleCmdProc */
static bool ConNetworkConnect(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Connect to a remote OTTD server and join the game. Usage: 'connect <ip>'.");
		IConsolePrint(CC_HELP, "IP can contain port and company: 'IP[:Port][#Company]', eg: 'server.ottd.org:443#2'.");
		IConsolePrint(CC_HELP, "Company #255 is spectator all others are a certain company with Company 1 being #1.");
		return true;
	}

	if (argv.size() < 2) return false;

	return NetworkClientConnectGame(argv[1], COMPANY_NEW_COMPANY);
}

/*********************************
 *  script file console commands
 *********************************/

/** Run a local script file. @copydoc IConsoleCmdProc */
static bool ConExec(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Execute a local script file. Usage: 'exec <script> [0]'.");
		IConsolePrint(CC_HELP, "By passing '0' after the script name, no warning about a missing script file will be shown.");
		return true;
	}

	if (argv.size() < 2) return false;

	auto script_file = FioFOpenFile(argv[1], "r", Subdirectory::Base);

	if (!script_file.has_value()) {
		if (argv.size() == 2 || argv[2] != "0") IConsolePrint(CC_ERROR, "Script file '{}' not found.", argv[1]);
		return true;
	}

	if (_script_current_depth == 11) {
		IConsolePrint(CC_ERROR, "Maximum 'exec' depth reached; script A is calling script B is calling script C ... more than 10 times.");
		return true;
	}

	_script_current_depth++;
	uint script_depth = _script_current_depth;

	char buffer[ICON_CMDLN_SIZE];
	while (fgets(buffer, sizeof(buffer), *script_file) != nullptr) {
		/* Remove newline characters from the executing script */
		std::string_view cmdline{buffer};
		auto last_non_newline = cmdline.find_last_not_of("\r\n");
		if (last_non_newline != std::string_view::npos) cmdline = cmdline.substr(0, last_non_newline + 1);

		IConsoleCmdExec(cmdline);
		/* Ensure that we are still on the same depth or that we returned via 'return'. */
		assert(_script_current_depth == script_depth || _script_current_depth == script_depth - 1);

		/* The 'return' command was executed. */
		if (_script_current_depth == script_depth - 1) break;
	}

	if (ferror(*script_file) != 0) {
		IConsolePrint(CC_ERROR, "Encountered error while trying to read from script file '{}'.", argv[1]);
	}

	if (_script_current_depth == script_depth) _script_current_depth--;
	return true;
}

/** Schedule the execution of a script. @copydoc IConsoleCmdProc */
static bool ConSchedule(std::span<std::string_view> argv)
{
	if (argv.size() < 3 || std::string_view(argv[1]) != "on-next-calendar-month") {
		IConsolePrint(CC_HELP, "Schedule a local script to execute later. Usage: 'schedule on-next-calendar-month <script>'.");
		return true;
	}

	/* Check if the file exists. It might still go away later, but helpful to show an error now. */
	if (!FioCheckFileExists(argv[2], Subdirectory::Base)) {
		IConsolePrint(CC_ERROR, "Script file '{}' not found.", argv[2]);
		return true;
	}

	/* We only support a single script scheduled, so we tell the user what's happening if there was already one. */
	std::string_view filename = std::string_view(argv[2]);
	if (!_scheduled_monthly_script.empty() && filename == _scheduled_monthly_script) {
		IConsolePrint(CC_INFO, "Script file '{}' was already scheduled to execute at the start of next calendar month.", filename);
	} else if (!_scheduled_monthly_script.empty() && filename != _scheduled_monthly_script) {
		IConsolePrint(CC_INFO, "Script file '{}' scheduled to execute at the start of next calendar month, replacing the previously scheduled script file '{}'.", filename, _scheduled_monthly_script);
	} else {
		IConsolePrint(CC_INFO, "Script file '{}' scheduled to execute at the start of next calendar month.", filename);
	}

	/* Store the filename to be used by _schedule_timer on the start of next calendar month. */
	_scheduled_monthly_script = filename;

	return true;
}

/** End the execution of the current script. @copydoc IConsoleCmdProc */
static bool ConReturn(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Stop executing a running script. Usage: 'return'.");
		return true;
	}

	_script_current_depth--;
	return true;
}

/*****************************
 *  default console commands
 ******************************/
extern bool CloseConsoleLogIfActive();
extern std::span<const GRFFile> GetAllGRFFiles();
extern void ConPrintFramerate(); // framerate_gui.cpp
extern void ShowFramerateWindow();

/** Enable or disable logging of console output. @copydoc IConsoleCmdProc */
static bool ConScript(std::span<std::string_view> argv)
{
	extern std::optional<FileHandle> _iconsole_output_file;

	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Start or stop logging console output to a file. Usage: 'script <filename>'.");
		IConsolePrint(CC_HELP, "If filename is omitted, a running log is stopped if it is active.");
		return true;
	}

	if (!CloseConsoleLogIfActive()) {
		if (argv.size() < 2) return false;

		_iconsole_output_file = FileHandle::Open(argv[1], "ab");
		if (!_iconsole_output_file.has_value()) {
			IConsolePrint(CC_ERROR, "Could not open console log file '{}'.", argv[1]);
		} else {
			IConsolePrint(CC_INFO, "Console log output started to '{}'.", argv[1]);
		}
	}

	return true;
}

/** Simply print the arguments. @copydoc IConsoleCmdProc */
static bool ConEcho(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print back the first argument to the console. Usage: 'echo <arg>'.");
		return true;
	}

	if (argv.size() < 2) return false;
	IConsolePrint(CC_DEFAULT, "{}", argv[1]);
	return true;
}

/** Print the arguments in a particular colour. @copydoc IConsoleCmdProc */
static bool ConEchoC(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print back the first argument to the console in a given colour. Usage: 'echoc <colour> <arg2>'.");
		return true;
	}

	if (argv.size() < 3) return false;

	auto colour = ParseInteger(argv[1]);
	if (!colour.has_value() || !IsInsideMM(*colour, to_underlying(TextColour::Begin), to_underlying(TextColour::End))) {
		IConsolePrint(CC_ERROR, "The colour must be a number between {} and {}.", TextColour::Begin, to_underlying(TextColour::End) - 1);
		return true;
	}

	IConsolePrint(static_cast<TextColour>(*colour), "{}", argv[2]);
	return true;
}

/** Start/create a new game. @copydoc IConsoleCmdProc */
static bool ConNewGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Start a new game. Usage: 'newgame [seed]'.");
		IConsolePrint(CC_HELP, "The server can force a new game using 'newgame'; any client joined will rejoin after the server is done generating the new game.");
		return true;
	}

	uint32_t seed = GENERATE_NEW_SEED;
	if (argv.size() >= 2) {
		auto param = ParseInteger(argv[1]);
		if (!param.has_value()) {
			IConsolePrint(CC_ERROR, "The given seed must be a valid number.");
			return true;
		}
		seed = *param;
	}

	StartNewGameWithoutGUI(seed);
	return true;
}

/** Restart the game. @copydoc IConsoleCmdProc */
static bool ConRestart(std::span<std::string_view> argv)
{
	if (argv.empty() || argv.size() > 2) {
		IConsolePrint(CC_HELP, "Restart game. Usage: 'restart [current|newgame]'.");
		IConsolePrint(CC_HELP, "Restarts a game, using either the current or newgame (default) settings.");
		IConsolePrint(CC_HELP, " * if you started from a new game, and your current/newgame settings haven't changed, the game will be identical to when you started it.");
		IConsolePrint(CC_HELP, " * if you started from a savegame / scenario / heightmap, the game might be different, because the current/newgame settings might differ.");
		return true;
	}

	if (argv.size() == 1 || std::string_view(argv[1]) == "newgame") {
		StartNewGameWithoutGUI(_settings_game.game_creation.generation_seed);
	} else {
		_settings_game.game_creation.map_x = Map::LogX();
		_settings_game.game_creation.map_y = Map::LogY();
		_switch_mode = SwitchMode::RestartGame;
	}

	return true;
}

/** Reload a game from the loaded savegame/scenario/heightmap. @copydoc IConsoleCmdProc */
static bool ConReload(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reload game. Usage: 'reload'.");
		IConsolePrint(CC_HELP, "Reloads a game if loaded via savegame / scenario / heightmap.");
		return true;
	}

	if (_file_to_saveload.ftype.abstract == AbstractFileType::None || _file_to_saveload.ftype.abstract == AbstractFileType::Invalid) {
		IConsolePrint(CC_ERROR, "No game loaded to reload.");
		return true;
	}

	/* Use a switch-mode to prevent copying over newgame settings to active settings. */
	_settings_game.game_creation.map_x = Map::LogX();
	_settings_game.game_creation.map_y = Map::LogY();
	_switch_mode = SwitchMode::ReloadGame;
	return true;
}

/**
 * Print a text buffer line by line to the console. Lines are separated by '\n'.
 * @param full_string The multi-line string to print.
 */
static void PrintLineByLine(const std::string &full_string)
{
	std::istringstream in(full_string);
	std::string line;
	while (std::getline(in, line)) {
		IConsolePrint(CC_DEFAULT, line);
	}
}

/**
 * Helper to print a list to the console.
 * @param list_function The function that gets the list.
 * @param args The arguments for the list function.
 * @return \c true, to ease the use in @see IConsoleCmdProc.
 */
template <typename F, typename ... Args>
bool PrintList(F list_function, Args... args)
{
	std::string output_str;
	auto inserter = std::back_inserter(output_str);
	list_function(inserter, args...);
	PrintLineByLine(output_str);

	return true;
}

/** List all AI libraries. @copydoc IConsoleCmdProc */
static bool ConListAILibs(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed AI libraries. Usage: 'list_ai_libs'.");
		return true;
	}

	return PrintList(AI::GetConsoleLibraryList, true);
}

/** List all AI scripts. @copydoc IConsoleCmdProc */
static bool ConListAI(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed AIs. Usage: 'list_ai'.");
		return true;
	}

	return PrintList(AI::GetConsoleList, false);
}

/** List all game script libraries. @copydoc IConsoleCmdProc */
static bool ConListGameLibs(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed Game Script libraries. Usage: 'list_game_libs'.");
		return true;
	}

	return PrintList(Game::GetConsoleLibraryList, true);
}

/** List all game scripts. @copydoc IConsoleCmdProc */
static bool ConListGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List installed Game Scripts. Usage: 'list_game'.");
		return true;
	}

	return PrintList(Game::GetConsoleList, false);
}

/** Start a new AI. @copydoc IConsoleCmdProc */
static bool ConStartAI(std::span<std::string_view> argv)
{
	if (argv.empty() || argv.size() > 3) {
		IConsolePrint(CC_HELP, "Start a new AI. Usage: 'start_ai [<AI>] [<settings>]'.");
		IConsolePrint(CC_HELP, "Start a new AI. If <AI> is given, it starts that specific AI (if found).");
		IConsolePrint(CC_HELP, "If <settings> is given, it is parsed and the AI settings are set to that.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (Company::GetNumItems() == CompanyPool::MAX_SIZE) {
		IConsolePrint(CC_ERROR, "Can't start a new AI (no more free slots).");
		return true;
	}
	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can start a new AI.");
		return true;
	}
	if (_networking && !_settings_game.ai.ai_in_multiplayer) {
		IConsolePrint(CC_ERROR, "AIs are not allowed in multiplayer by configuration.");
		IConsolePrint(CC_ERROR, "Switch AI -> AI in multiplayer to True.");
		return true;
	}
	if (!AI::CanStartNew()) {
		IConsolePrint(CC_ERROR, "Can't start a new AI.");
		return true;
	}

	int n = 0;
	/* Find the next free slot */
	for (const Company *c : Company::Iterate()) {
		if (c->index != n) break;
		n++;
	}

	AIConfig *config = AIConfig::GetConfig((CompanyID)n);
	if (argv.size() >= 2) {
		config->Change(argv[1], -1, false);

		/* If the name is not found, and there is a dot in the name,
		 * try again with the assumption everything right of the dot is
		 * the version the user wants to load. */
		if (!config->HasScript()) {
			StringConsumer consumer{std::string_view{argv[1]}};
			auto name = consumer.ReadUntilChar('.', StringConsumer::SKIP_ONE_SEPARATOR);
			if (consumer.AnyBytesLeft()) {
				auto version = consumer.TryReadIntegerBase<uint32_t>(10);
				if (!version.has_value()) {
					IConsolePrint(CC_ERROR, "The version is not a valid number.");
					return true;
				}
				config->Change(name, *version, true);
			}
		}

		if (!config->HasScript()) {
			IConsolePrint(CC_ERROR, "Failed to load the specified AI.");
			return true;
		}
		if (argv.size() == 3) {
			config->StringToSettings(argv[2]);
		}
	}

	/* Start a new AI company */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::NewAI, CompanyID::Invalid(), CompanyRemoveReason::None, ClientID::Invalid);

	return true;
}

/** Reload/restart an AI. @copydoc IConsoleCmdProc */
static bool ConReloadAI(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Reload an AI. Usage: 'reload_ai <company-id>'.");
		IConsolePrint(CC_HELP, "Reload the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can reload an AI.");
		return true;
	}

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	if (!Company::IsValidID(*company_id)) {
		IConsolePrint(CC_ERROR, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	/* In singleplayer mode the player can be in an AI company, after cheating or loading network save with an AI in first slot. */
	if (Company::IsHumanID(*company_id) || *company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Company is not controlled by an AI.");
		return true;
	}

	/* First kill the company of the AI, then start a new one. This should start the current AI again */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::Delete, *company_id, CompanyRemoveReason::Manual, ClientID::Invalid);
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::NewAI, *company_id, CompanyRemoveReason::None, ClientID::Invalid);
	IConsolePrint(CC_DEFAULT, "AI reloaded.");

	return true;
}

/** Stop a currently running AI. @copydoc IConsoleCmdProc */
static bool ConStopAI(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Stop an AI. Usage: 'stop_ai <company-id>'.");
		IConsolePrint(CC_HELP, "Stop the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can stop an AI.");
		return true;
	}

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	if (!Company::IsValidID(*company_id)) {
		IConsolePrint(CC_ERROR, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	/* In singleplayer mode the player can be in an AI company, after cheating or loading network save with an AI in first slot. */
	if (Company::IsHumanID(*company_id) || *company_id == _local_company) {
		IConsolePrint(CC_ERROR, "Company is not controlled by an AI.");
		return true;
	}

	/* Now kill the company of the AI. */
	Command<Commands::CompanyControl>::Post(CompanyCtrlAction::Delete, *company_id, CompanyRemoveReason::Manual, ClientID::Invalid);
	IConsolePrint(CC_DEFAULT, "AI stopped, company deleted.");

	return true;
}

/** Rescan the folder structure for new/changed AIs and libraries. @copydoc IConsoleCmdProc */
static bool ConRescanAI(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Rescan the AI dir for scripts. Usage: 'rescan_ai'.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can rescan the AI dir for scripts.");
		return true;
	}

	AI::Rescan();

	return true;
}

/** Rescan the folder structure for new/changed game scripts and libraries. @copydoc IConsoleCmdProc */
static bool ConRescanGame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Rescan the Game Script dir for scripts. Usage: 'rescan_game'.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsolePrint(CC_ERROR, "Only the server can rescan the Game Script dir for scripts.");
		return true;
	}

	Game::Rescan();

	return true;
}

/** Rescan the folder structure for new/changed NewGRFs. @copydoc IConsoleCmdProc */
static bool ConRescanNewGRF(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Rescan the data dir for NewGRFs. Usage: 'rescan_newgrf'.");
		return true;
	}

	if (!RequestNewGRFScan()) {
		IConsolePrint(CC_ERROR, "NewGRF scanning is already running. Please wait until completed to run again.");
	}

	return true;
}

/** Get the seed that was used to create this game. @copydoc IConsoleCmdProc */
static bool ConGetSeed(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Returns the seed used to create this game. Usage: 'getseed'.");
		IConsolePrint(CC_HELP, "The seed can be used to reproduce the exact same map as the game started with.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Generation Seed: {}", _settings_game.game_creation.generation_seed);
	return true;
}

/** Get the current game date. @copydoc IConsoleCmdProc */
static bool ConGetDate(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Returns the current date (year-month-day) of the game. Usage: 'getdate'.");
		return true;
	}

	TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
	IConsolePrint(CC_DEFAULT, "Date: {:04d}-{:02d}-{:02d}", ymd.year, ymd.month + 1, ymd.day);
	return true;
}

/** Get the current system date. @copydoc IConsoleCmdProc */
static bool ConGetSysDate(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Returns the current date (year-month-day) of your system. Usage: 'getsysdate'.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "System Date: {:%Y-%m-%d %H:%M:%S}", fmt::localtime(time(nullptr)));
	return true;
}

/** Create an alias for a command. @copydoc IConsoleCmdProc */
static bool ConAlias(std::span<std::string_view> argv)
{
	IConsoleAlias *alias;

	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Add a new alias, or redefine the behaviour of an existing alias . Usage: 'alias <name> <command>'.");
		return true;
	}

	if (argv.size() < 3) return false;

	alias = IConsole::AliasGet(std::string(argv[1]));
	if (alias == nullptr) {
		IConsole::AliasRegister(std::string(argv[1]), argv[2]);
	} else {
		alias->cmdline = argv[2];
	}
	return true;
}

/** Make a screenshot. @copydoc IConsoleCmdProc */
static bool ConScreenShot(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Create a screenshot of the game. Usage: 'screenshot [viewport | normal | big | giant | heightmap | minimap] [no_con] [size <width> <height>] [<filename>]'.");
		IConsolePrint(CC_HELP, "  'viewport' (default) makes a screenshot of the current viewport (including menus, windows).");
		IConsolePrint(CC_HELP, "  'normal' makes a screenshot of the visible area.");
		IConsolePrint(CC_HELP, "  'big' makes a zoomed-in screenshot of the visible area.");
		IConsolePrint(CC_HELP, "  'giant' makes a screenshot of the whole map.");
		IConsolePrint(CC_HELP, "  'heightmap' makes a heightmap screenshot of the map that can be loaded in as heightmap.");
		IConsolePrint(CC_HELP, "  'minimap' makes a top-viewed minimap screenshot of the whole world which represents one tile by one pixel.");
		IConsolePrint(CC_HELP, "  'no_con' hides the console to create the screenshot (only useful in combination with 'viewport').");
		IConsolePrint(CC_HELP, "  'size' sets the width and height of the viewport to make a screenshot of (only useful in combination with 'normal' or 'big').");
		IConsolePrint(CC_HELP, "  A filename ending in # will prevent overwriting existing files and will number files counting upwards.");
		return true;
	}

	if (argv.size() > 7) return false;

	ScreenshotType type = SC_VIEWPORT;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string name{};
	uint32_t arg_index = 1;

	if (argv.size() > arg_index) {
		if (argv[arg_index] == "viewport") {
			type = SC_VIEWPORT;
			arg_index += 1;
		} else if (argv[arg_index] == "normal") {
			type = SC_DEFAULTZOOM;
			arg_index += 1;
		} else if (argv[arg_index] == "big") {
			type = SC_ZOOMEDIN;
			arg_index += 1;
		} else if (argv[arg_index] == "giant") {
			type = SC_WORLD;
			arg_index += 1;
		} else if (argv[arg_index] == "heightmap") {
			type = SC_HEIGHTMAP;
			arg_index += 1;
		} else if (argv[arg_index] == "minimap") {
			type = SC_MINIMAP;
			arg_index += 1;
		}
	}

	if (argv.size() > arg_index && argv[arg_index] == "no_con") {
		if (type != SC_VIEWPORT) {
			IConsolePrint(CC_ERROR, "'no_con' can only be used in combination with 'viewport'.");
			return true;
		}
		IConsoleClose();
		arg_index += 1;
	}

	if (argv.size() > arg_index + 2 && argv[arg_index] == "size") {
		/* size <width> <height> */
		if (type != SC_DEFAULTZOOM && type != SC_ZOOMEDIN) {
			IConsolePrint(CC_ERROR, "'size' can only be used in combination with 'normal' or 'big'.");
			return true;
		}
		auto t = ParseInteger(argv[arg_index + 1]);
		if (!t.has_value()) {
			IConsolePrint(CC_ERROR, "Invalid width '{}'", argv[arg_index + 1]);
			return true;
		}
		width = *t;

		t = ParseInteger(argv[arg_index + 2]);
		if (!t.has_value()) {
			IConsolePrint(CC_ERROR, "Invalid height '{}'", argv[arg_index + 2]);
			return true;
		}
		height = *t;
		arg_index += 3;
	}

	if (argv.size() > arg_index) {
		/* Last parameter that was not one of the keywords must be the filename. */
		name = argv[arg_index];
		arg_index += 1;
	}

	if (argv.size() > arg_index) {
		/* We have parameters we did not process; means we misunderstood any of the above. */
		return false;
	}

	MakeScreenshot(type, std::move(name), width, height);
	return true;
}

/** Get debug information about a command. @copydoc IConsoleCmdProc */
static bool ConInfoCmd(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print out debugging information about a command. Usage: 'info_cmd <cmd>'.");
		return true;
	}

	if (argv.size() < 2) return false;

	const IConsoleCmd *cmd = IConsole::CmdGet(std::string(argv[1]));
	if (cmd == nullptr) {
		IConsolePrint(CC_ERROR, "The given command was not found.");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Command name: '{}'", cmd->name);

	if (cmd->hook != nullptr) IConsolePrint(CC_DEFAULT, "Command is hooked.");

	return true;
}

/** Change the debug levels of the game. @copydoc IConsoleCmdProc */
static bool ConDebugLevel(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Get/set the default debugging level for the game. Usage: 'debug_level [<level>]'.");
		IConsolePrint(CC_HELP, "Level can be any combination of names, levels. Eg 'net=5 ms=4'. Remember to enclose it in \"'\"s.");
		return true;
	}

	if (argv.size() > 2) return false;

	if (argv.size() == 1) {
		IConsolePrint(CC_DEFAULT, "Current debug-level: '{}'", GetDebugString());
	} else {
		SetDebugString(argv[1], [](std::string_view err) { IConsolePrint(CC_ERROR, "{}", err); });
	}

	return true;
}

/** Exit the game, i.e. exit the complete application. @copydoc IConsoleCmdProc */
static bool ConExit(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Exit the game. Usage: 'exit'.");
		return true;
	}

	if (_game_mode == GameMode::Normal && _settings_client.gui.autosave_on_exit) DoExitSave();

	_exit_game = true;
	return true;
}

/** Part the game, i.e. go back to the main menu. @copydoc IConsoleCmdProc */
static bool ConPart(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Leave the currently joined/running game (only ingame). Usage: 'part'.");
		return true;
	}

	if (_game_mode != GameMode::Normal) return false;

	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "A dedicated server can not leave the game.");
		return false;
	}

	_switch_mode = SwitchMode::Menu;
	return true;
}

/** Show generic help and specific help for commands. @copydoc IConsoleCmdProc */
static bool ConHelp(std::span<std::string_view> argv)
{
	if (argv.size() == 2) {
		const IConsoleCmd *cmd;
		const IConsoleAlias *alias;

		cmd = IConsole::CmdGet(std::string(argv[1]));
		if (cmd != nullptr) {
			cmd->proc({});
			return true;
		}

		alias = IConsole::AliasGet(std::string(argv[1]));
		if (alias != nullptr) {
			cmd = IConsole::CmdGet(alias->cmdline);
			if (cmd != nullptr) {
				cmd->proc({});
				return true;
			}
			IConsolePrint(CC_ERROR, "Alias is of special type, please see its execution-line: '{}'.", alias->cmdline);
			return true;
		}

		IConsolePrint(CC_ERROR, "Command not found.");
		return true;
	}

	IConsolePrint(TextColour::LightBlue, " ---- OpenTTD Console Help ---- ");
	IConsolePrint(CC_DEFAULT, " - commands: the command to list all commands is 'list_cmds'.");
	IConsolePrint(CC_DEFAULT, " call commands with '<command> <arg2> <arg3>...'");
	IConsolePrint(CC_DEFAULT, " - to assign strings, or use them as arguments, enclose it within quotes.");
	IConsolePrint(CC_DEFAULT, " like this: '<command> \"string argument with spaces\"'.");
	IConsolePrint(CC_DEFAULT, " - use 'help <command>' to get specific information.");
	IConsolePrint(CC_DEFAULT, " - scroll console output with shift + (up | down | pageup | pagedown).");
	IConsolePrint(CC_DEFAULT, " - scroll console input history with the up or down arrows.");
	IConsolePrint(CC_DEFAULT, "");
	return true;
}

/** List all registered commands that are not hidden. @copydoc IConsoleCmdProc */
static bool ConListCommands(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all registered commands. Usage: 'list_cmds [<pre-filter>]'.");
		return true;
	}

	for (auto &it : IConsole::Commands()) {
		const IConsoleCmd *cmd = &it.second;
		if (argv.size() <= 1|| cmd->name.find(argv[1]) != std::string::npos) {
			if (cmd->hook == nullptr || cmd->hook(false) != ConsoleHookResult::Hide) IConsolePrint(CC_DEFAULT, cmd->name);
		}
	}

	return true;
}

/** List all registered aliases. @copydoc IConsoleCmdProc */
static bool ConListAliases(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List all registered aliases. Usage: 'list_aliases [<pre-filter>]'.");
		return true;
	}

	for (auto &it : IConsole::Aliases()) {
		const IConsoleAlias *alias = &it.second;
		if (argv.size() <= 1 || alias->name.find(argv[1]) != std::string::npos) {
			IConsolePrint(CC_DEFAULT, "{} => {}", alias->name, alias->cmdline);
		}
	}

	return true;
}

/** List all companies. @copydoc IConsoleCmdProc */
static bool ConCompanies(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List the details of all companies in the game. Usage 'companies'.");
		return true;
	}

	for (const Company *c : Company::Iterate()) {
		/* Grab the company name */
		std::string company_name = GetString(STR_COMPANY_NAME, c->index);

		std::string colour = GetString(STR_COLOUR_DARK_BLUE + to_underlying(_company_colours[c->index]));
		IConsolePrint(CC_INFO, "#:{}({}) Company Name: '{}'  Year Founded: {}  Money: {}  Loan: {}  Value: {}  (T:{}, R:{}, P:{}, S:{}) {}",
			c->index + 1, colour, company_name,
			c->inaugurated_year, (int64_t)c->money, (int64_t)c->current_loan, (int64_t)CalculateCompanyValue(c),
			c->group_all[VehicleType::Train].num_vehicle,
			c->group_all[VehicleType::Road].num_vehicle,
			c->group_all[VehicleType::Aircraft].num_vehicle,
			c->group_all[VehicleType::Ship].num_vehicle,
			c->is_ai ? "AI" : "");
	}

	return true;
}

/** Say something to all clients in a network game. @copydoc IConsoleCmdProc */
static bool ConSay(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Chat to your fellow players in a multiplayer game. Usage: 'say \"<msg>\"'.");
		return true;
	}

	if (argv.size() != 2) return false;

	if (!_network_server) {
		NetworkClientSendChat(NetworkAction::ChatBroadcast, NetworkChatDestinationType::Broadcast, 0 /* param does not matter */, argv[1]);
	} else {
		bool from_admin = (_redirect_console_to_admin < AdminID::Invalid());
		NetworkServerSendChat(NetworkAction::ChatBroadcast, NetworkChatDestinationType::Broadcast, 0, argv[1], ClientID::Server, from_admin);
	}

	return true;
}

/** Say something to all clients in your company in a network game. @copydoc IConsoleCmdProc */
static bool ConSayCompany(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Chat to a certain company in a multiplayer game. Usage: 'say_company <company-no> \"<msg>\"'.");
		IConsolePrint(CC_HELP, "CompanyNo is the company that plays as company <companyno>, 1 through max_companies.");
		return true;
	}

	if (argv.size() != 3) return false;

	auto company_id = ParseCompanyID(argv[1]);
	if (!company_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given company-id is not a valid number.");
		return true;
	}

	if (!Company::IsValidID(*company_id)) {
		IConsolePrint(CC_DEFAULT, "Unknown company. Company range is between 1 and {}.", MAX_COMPANIES);
		return true;
	}

	if (!_network_server) {
		NetworkClientSendChat(NetworkAction::ChatTeam, NetworkChatDestinationType::Team, company_id->base(), argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < AdminID::Invalid());
		NetworkServerSendChat(NetworkAction::ChatTeam, NetworkChatDestinationType::Team, company_id->base(), argv[2], ClientID::Server, from_admin);
	}

	return true;
}

/** Say something to a specific client in a network game. @copydoc IConsoleCmdProc */
static bool ConSayClient(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Chat to a certain client in a multiplayer game. Usage: 'say_client <client-id> \"<msg>\"'.");
		IConsolePrint(CC_HELP, "For client-id's, see the command 'clients'.");
		return true;
	}

	if (argv.size() != 3) return false;

	auto client_id = ParseType<ClientID>(argv[1]);
	if (!client_id.has_value()) {
		IConsolePrint(CC_ERROR, "The given client-id is not a valid number.");
		return true;
	}

	if (!_network_server) {
		NetworkClientSendChat(NetworkAction::ChatClient, NetworkChatDestinationType::Client, to_underlying(*client_id), argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < AdminID::Invalid());
		NetworkServerSendChat(NetworkAction::ChatClient, NetworkChatDestinationType::Client, to_underlying(*client_id), argv[2], ClientID::Server, from_admin);
	}

	return true;
}

/** All the known authorized keys with their name. */
static const std::initializer_list<std::pair<std::string_view, NetworkAuthorizedKeys *>> _console_cmd_authorized_keys{
	{ "admin", &_settings_client.network.admin_authorized_keys },
	{ "rcon", &_settings_client.network.rcon_authorized_keys },
	{ "server", &_settings_client.network.server_authorized_keys },
};

/** Actions that can be performed on authorized keys from the console. */
enum class ConNetworkAuthorizedKeyAction : uint8_t {
	List, ///< List all authorized keys.
	Add, ///< Add an authorized key.
	Remove, ///< Remove an authorized key.
};

static void PerformNetworkAuthorizedKeyAction(std::string_view name, NetworkAuthorizedKeys *authorized_keys, ConNetworkAuthorizedKeyAction action, const std::string &authorized_key, CompanyID company = CompanyID::Invalid())
{
	switch (action) {
		case ConNetworkAuthorizedKeyAction::List:
			IConsolePrint(CC_WHITE, "The authorized keys for {} are:", name);
			for (auto &ak : *authorized_keys) IConsolePrint(CC_INFO, "  {}", ak);
			return;

		case ConNetworkAuthorizedKeyAction::Add:
			if (authorized_keys->Contains(authorized_key)) {
				IConsolePrint(CC_WARNING, "Not added {} to {} as it already exists.", authorized_key, name);
				return;
			}

			if (company == CompanyID::Invalid()) {
				authorized_keys->Add(authorized_key);
			} else {
				AutoRestoreBackup backup(_current_company, company);
				Command<Commands::CompanyAllowListControl>::Post(CompanyAllowListCtrlAction::AddKey, authorized_key);
			}
			IConsolePrint(CC_INFO, "Added {} to {}.", authorized_key, name);
			return;

		case ConNetworkAuthorizedKeyAction::Remove:
			if (!authorized_keys->Contains(authorized_key)) {
				IConsolePrint(CC_WARNING, "Not removed {} from {} as it does not exist.", authorized_key, name);
				return;
			}

			if (company == CompanyID::Invalid()) {
				authorized_keys->Remove(authorized_key);
			} else {
				AutoRestoreBackup backup(_current_company, company);
				Command<Commands::CompanyAllowListControl>::Post(CompanyAllowListCtrlAction::RemoveKey, authorized_key);
			}
			IConsolePrint(CC_INFO, "Removed {} from {}.", authorized_key, name);
			return;
	}
}

/** Management of authorized keys. @copydoc IConsoleCmdProc */
static bool ConNetworkAuthorizedKey(std::span<std::string_view> argv)
{
	if (argv.size() <= 2) {
		IConsolePrint(CC_HELP, "List and update authorized keys. Usage: 'authorized_key list [type]|add [type] [key]|remove [type] [key]'.");
		IConsolePrint(CC_HELP, "  list: list all the authorized keys of the given type.");
		IConsolePrint(CC_HELP, "  add: add the given key to the authorized keys of the given type.");
		IConsolePrint(CC_HELP, "  remove: remove the given key from the authorized keys of the given type; use 'all' to remove all authorized keys.");
		IConsolePrint(CC_HELP, "Instead of a key, use 'client:<id>' to add/remove the key of that given client.");

		std::string buffer;
		for (auto [name, _] : _console_cmd_authorized_keys) format_append(buffer, ", {}", name);
		IConsolePrint(CC_HELP, "The supported types are: all{} and company:<id>.", buffer);
		return true;
	}

	ConNetworkAuthorizedKeyAction action;
	std::string_view action_string = argv[1];
	if (StrEqualsIgnoreCase(action_string, "list")) {
		action = ConNetworkAuthorizedKeyAction::List;
	} else if (StrEqualsIgnoreCase(action_string, "add")) {
		action = ConNetworkAuthorizedKeyAction::Add;
	} else if (StrEqualsIgnoreCase(action_string, "remove") || StrEqualsIgnoreCase(action_string, "delete")) {
		action = ConNetworkAuthorizedKeyAction::Remove;
	} else {
		IConsolePrint(CC_WARNING, "No valid action was given.");
		return false;
	}

	std::string authorized_key;
	if (action != ConNetworkAuthorizedKeyAction::List) {
		if (argv.size() <= 3) {
			IConsolePrint(CC_ERROR, "You must enter the key.");
			return false;
		}

		authorized_key = argv[3];
		if (StrStartsWithIgnoreCase(authorized_key, "client:")) {
			auto value = ParseInteger<uint32_t>(authorized_key.substr(7));
			if (value.has_value()) authorized_key = NetworkGetPublicKeyOfClient(static_cast<ClientID>(*value));
			if (!value.has_value() || authorized_key.empty()) {
				IConsolePrint(CC_ERROR, "You must enter a valid client id; see 'clients'.");
				return false;
			}
		}

		if (authorized_key.size() != NETWORK_PUBLIC_KEY_LENGTH - 1) {
			IConsolePrint(CC_ERROR, "You must enter a valid authorized key.");
			return false;
		}
	}

	std::string_view type = argv[2];
	if (StrEqualsIgnoreCase(type, "all")) {
		for (auto [name, authorized_keys] : _console_cmd_authorized_keys) PerformNetworkAuthorizedKeyAction(name, authorized_keys, action, authorized_key);
		for (Company *c : Company::Iterate()) PerformNetworkAuthorizedKeyAction(fmt::format("company:{}", c->index + 1), &c->allow_list, action, authorized_key, c->index);
		return true;
	}

	if (StrStartsWithIgnoreCase(type, "company:")) {
		auto value = ParseInteger<uint32_t>(type.substr(8));
		Company *c = value.has_value() ? Company::GetIfValid(*value - 1) : nullptr;
		if (c == nullptr) {
			IConsolePrint(CC_ERROR, "You must enter a valid company id; see 'companies'.");
			return false;
		}

		PerformNetworkAuthorizedKeyAction(type, &c->allow_list, action, authorized_key, c->index);
		return true;
	}

	for (auto [name, authorized_keys] : _console_cmd_authorized_keys) {
		if (!StrEqualsIgnoreCase(type, name)) continue;

		PerformNetworkAuthorizedKeyAction(name, authorized_keys, action, authorized_key);
		return true;
	}

	IConsolePrint(CC_WARNING, "No valid type was given.");
	return false;
}


/* Content downloading only is available with ZLIB */
#if defined(WITH_ZLIB)

/**
 * Resolve a string to a content type.
 * @param str The string to resolve.
 * @return The content type, or #ContentType::End when the string is not a content type.
 */
static ContentType StringToContentType(std::string_view str)
{
	static const std::initializer_list<std::pair<std::string_view, ContentType>> content_types = {
		{"base",      ContentType::BaseGraphics},
		{"newgrf",    ContentType::NewGRF},
		{"ai",        ContentType::Ai},
		{"ailib",     ContentType::AiLibrary},
		{"scenario",  ContentType::Scenario},
		{"heightmap", ContentType::Heightmap},
	};
	for (const auto &ct : content_types) {
		if (StrEqualsIgnoreCase(str, ct.first)) return ct.second;
	}
	return ContentType::End;
}

/** Asynchronous callback */
struct ConsoleContentCallback : public ContentCallback {
	void OnConnect(bool success) override
	{
		IConsolePrint(CC_DEFAULT, "Content server connection {}.", success ? "established" : "failed");
	}

	void OnDisconnect() override
	{
		IConsolePrint(CC_DEFAULT, "Content server connection closed.");
	}

	void OnDownloadComplete(ContentID cid) override
	{
		IConsolePrint(CC_DEFAULT, "Completed download of {}.", cid);
	}
};

/**
 * Outputs content state information to console
 * @param ci the content info
 */
static void OutputContentState(const ContentInfo &ci)
{
	static constexpr EnumIndexArray<std::string_view, ContentType, ContentType::End> types{
		"", "Base graphics", "NewGRF", "AI", "AI library", "Scenario", "Heightmap", "Base sound", "Base music", "Game script", "GS library"
	};
	static constexpr EnumIndexArray<std::string_view, ContentInfo::State, ContentInfo::State::End> states{
		"Not selected", "Selected", "Dep Selected", "Installed", "Unknown"
	};
	static constexpr EnumIndexArray<TextColour, ContentInfo::State, ContentInfo::State::End> state_to_colour{
		CC_COMMAND, CC_INFO, CC_INFO, CC_WHITE, CC_ERROR
	};

	IConsolePrint(state_to_colour[ci.state], "{}, {}, {}, {}, {:08X}, {}", ci.id, types[ci.type], states[ci.state], ci.name, ci.unique_id, FormatArrayAsHex(ci.md5sum));
}

/** Downloading of content from the server. @copydoc IConsoleCmdProc */
static bool ConContent(std::span<std::string_view> argv)
{
	[[maybe_unused]] static ContentCallback *const cb = []() {
			auto res = new ConsoleContentCallback();
			_network_content_client.AddCallback(res);
			return res;
		}();

	if (argv.size() <= 1) {
		IConsolePrint(CC_HELP, "Query, select and download content. Usage: 'content update|upgrade|select [id]|unselect [all|id]|state [filter]|download'.");
		IConsolePrint(CC_HELP, "  update: get a new list of downloadable content; must be run first.");
		IConsolePrint(CC_HELP, "  upgrade: select all items that are upgrades.");
		IConsolePrint(CC_HELP, "  select: select a specific item given by its id. If no parameter is given, all selected content will be listed.");
		IConsolePrint(CC_HELP, "  unselect: unselect a specific item given by its id or 'all' to unselect all.");
		IConsolePrint(CC_HELP, "  state: show the download/select state of all downloadable content. Optionally give a filter string.");
		IConsolePrint(CC_HELP, "  download: download all content you've selected.");
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "update")) {
		_network_content_client.RequestContentList((argv.size() > 2) ? StringToContentType(argv[2]) : ContentType::End);
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "upgrade")) {
		_network_content_client.SelectUpgrade();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "select")) {
		if (argv.size() <= 2) {
			/* List selected content */
			IConsolePrint(CC_WHITE, "id, type, state, name");
			for (const ContentInfo &ci : _network_content_client.Info()) {
				if (ci.state != ContentInfo::State::Selected && ci.state != ContentInfo::State::Autoselected) continue;
				OutputContentState(ci);
			}
		} else if (StrEqualsIgnoreCase(argv[2], "all")) {
			/* The intention of this function was that you could download
			 * everything after a filter was applied; but this never really
			 * took off. Instead, a select few people used this functionality
			 * to download every available package on BaNaNaS. This is not in
			 * the spirit of this service. Additionally, these few people were
			 * good for 70% of the consumed bandwidth of BaNaNaS. */
			IConsolePrint(CC_ERROR, "'select all' is no longer supported since 1.11.");
		} else if (auto content_id = ParseType<ContentID>(argv[2]); content_id.has_value()) {
			_network_content_client.Select(*content_id);
		} else {
			IConsolePrint(CC_ERROR, "The given content-id is not a number or 'all'");
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "unselect")) {
		if (argv.size() <= 2) {
			IConsolePrint(CC_ERROR, "You must enter the id.");
			return false;
		}
		if (StrEqualsIgnoreCase(argv[2], "all")) {
			_network_content_client.UnselectAll();
		} else if (auto content_id = ParseType<ContentID>(argv[2]); content_id.has_value()) {
			_network_content_client.Unselect(*content_id);
		} else {
			IConsolePrint(CC_ERROR, "The given content-id is not a number or 'all'");
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "state")) {
		IConsolePrint(CC_WHITE, "id, type, state, name");
		for (const ContentInfo &ci : _network_content_client.Info()) {
			if (argv.size() > 2 && !StrContainsIgnoreCase(ci.name, argv[2])) continue;
			OutputContentState(ci);
		}
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "download")) {
		uint files;
		uint bytes;
		_network_content_client.DownloadSelectedContent(files, bytes);
		IConsolePrint(CC_DEFAULT, "Downloading {} file(s) ({} bytes).", files, bytes);
		return true;
	}

	return false;
}
#endif /* defined(WITH_ZLIB) */

/**
 * Get FontSize by name
 * @param name The name to look up.
 * @return The FontSize matching the given name,
 */
static FontSize GetFontSizeByName(std::string_view name)
{
	for (FontSize fs : EnumRange(FontSize::End)) {
		if (StrEqualsIgnoreCase(name, FontSizeToName(fs))) return fs;
	}
	return FontSize::End;
}

/** Managing the font configuration. @copydoc IConsoleCmdProc */
static bool ConFont(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Manage the fonts configuration.");
		IConsolePrint(CC_HELP, "Usage 'font'.");
		IConsolePrint(CC_HELP, "  Print out the fonts configuration.");
		IConsolePrint(CC_HELP, "  The \"Currently active\" configuration is the one actually in effect (after interface scaling and replacing unavailable fonts).");
		IConsolePrint(CC_HELP, "  The \"Requested\" configuration is the one requested via console command or config file.");
		IConsolePrint(CC_HELP, "Usage 'font [medium|small|large|mono] [<font name>] [<size>]'.");
		IConsolePrint(CC_HELP, "  Change the configuration for a font.");
		IConsolePrint(CC_HELP, "  Omitting an argument will keep the current value.");
		IConsolePrint(CC_HELP, "  Set <font name> to \"\" for the default font. Note that <size> has no effect if the default font is in use, and fixed defaults are used instead.");
		IConsolePrint(CC_HELP, "  If the sprite font is enabled in Game Options, it is used instead of the default font.");
		IConsolePrint(CC_HELP, "  The <size> is automatically multiplied by the current interface scaling.");
		return true;
	}

	if (argv.size() > 2) {
		/* First argument must be a FontSize. */
		FontSize argfs = GetFontSizeByName(argv[1]);
		if (argfs == FontSize::End) return false;

		FontCacheSubSetting *setting = GetFontCacheSubSetting(argfs);
		std::string font = setting->font;
		uint size = setting->size;
		uint8_t arg_index = 2;
		/* For <name> we want a string. */

		if (!ParseInteger(argv[arg_index]).has_value()) {
			font = argv[arg_index++];
		}

		if (argv.size() > arg_index) {
			/* For <size> we want a number. */
			auto v = ParseInteger(argv[arg_index]);
			if (v.has_value()) {
				size = *v;
				arg_index++;
			}
		}

		SetFont(argfs, font, size);
	}

	for (FontSize fs : EnumRange(FontSize::End)) {
		FontCache *fc = FontCache::Get(fs);
		FontCacheSubSetting *setting = GetFontCacheSubSetting(fs);
		/* Make sure all non sprite fonts are loaded. */
		if (!setting->font.empty() && !fc->HasParent()) {
			FontCache::LoadFontCaches(fs);
			fc = FontCache::Get(fs);
		}
		IConsolePrint(CC_DEFAULT, "{} font:", FontSizeToName(fs));
		IConsolePrint(CC_DEFAULT, "Currently active: \"{}\", size {}", fc->GetFontName(), fc->GetFontSize());
		IConsolePrint(CC_DEFAULT, "Requested: \"{}\", size {}", setting->font, setting->size);
	}

	return true;
}

/** Change settings of the current game. @copydoc IConsoleCmdProc */
static bool ConSetting(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change setting for all clients. Usage: 'setting <name> [<value>]'.");
		IConsolePrint(CC_HELP, "Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argv.size() == 1 || argv.size() > 3) return false;

	if (argv.size() == 2) {
		IConsoleGetSetting(argv[1]);
	} else {
		IConsoleSetSetting(argv[1], argv[2]);
	}

	return true;
}

/** Change settings of for a new game. @copydoc IConsoleCmdProc */
static bool ConSettingNewgame(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Change setting for the next game. Usage: 'setting_newgame <name> [<value>]'.");
		IConsolePrint(CC_HELP, "Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argv.size() == 1 || argv.size() > 3) return false;

	if (argv.size() == 2) {
		IConsoleGetSetting(argv[1], true);
	} else {
		IConsoleSetSetting(argv[1], argv[2], true);
	}

	return true;
}

/** List all settings. @copydoc IConsoleCmdProc */
static bool ConListSettings(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "List settings. Usage: 'list_settings [<pre-filter>]'.");
		return true;
	}

	if (argv.size() > 2) return false;

	IConsoleListSettings((argv.size() == 2) ? argv[1] : std::string_view{});
	return true;
}

/** Print the gamelog. @copydoc IConsoleCmdProc */
static bool ConGamelogPrint(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Print logged fundamental changes to the game since the start. Usage: 'gamelog'.");
		return true;
	}

	_gamelog.PrintConsole();
	return true;
}

/** Reload all active NewGRFs. @copydoc IConsoleCmdProc */
static bool ConNewGRFReload(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Reloads all active NewGRFs from disk. Equivalent to reapplying NewGRFs via the settings, but without asking for confirmation. This might crash OpenTTD!");
		return true;
	}

	ReloadNewGRFData();
	return true;
}

/** List the locations of all of the game's different sub directories. @copydoc IConsoleCmdProc */
static bool ConListDirs(std::span<std::string_view> argv)
{
	struct SubdirNameMap {
		std::string_view name; ///< UI name for the directory
		Subdirectory subdir; ///< Index of subdirectory type
		bool default_only; ///< Whether only the default (first existing) directory for this is interesting
	};
	static const SubdirNameMap subdir_name_map[] = {
		/* Game data directories */
		{ "baseset", Subdirectory::Baseset, false },
		{ "newgrf", Subdirectory::NewGrf, false },
		{ "ai", Subdirectory::Ai, false },
		{ "ailib", Subdirectory::AiLibrary, false },
		{ "gs", Subdirectory::Gs, false },
		{ "gslib", Subdirectory::GsLibrary, false },
		{ "scenario", Subdirectory::Scenario, false },
		{ "heightmap", Subdirectory::Heightmap, false },
		/* Default save locations for user data */
		{ "save", Subdirectory::Save, true },
		{ "autosave", Subdirectory::Autosave, true },
		{ "screenshot", Subdirectory::Screenshot, true },
		{ "social_integration", Subdirectory::SocialIntegration, true },
	};

	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "List all search paths or default directories for various categories.");
		IConsolePrint(CC_HELP, "Usage: list_dirs <category>");
		std::string cats{subdir_name_map[0].name};
		bool first = true;
		for (const SubdirNameMap &sdn : subdir_name_map) {
			if (!first) {
				cats += ", ";
				cats += sdn.name;
			}
			first = false;
		}
		IConsolePrint(CC_HELP, "Valid categories: {}", cats);
		return true;
	}

	std::set<std::string> seen_dirs;
	for (const SubdirNameMap &sdn : subdir_name_map) {
		if (!StrEqualsIgnoreCase(argv[1], sdn.name))  continue;
		bool found = false;
		for (Searchpath sp : _valid_searchpaths) {
			/* Get the directory */
			std::string path = FioGetDirectory(sp, sdn.subdir);
			/* Check it hasn't already been listed */
			if (seen_dirs.find(path) != seen_dirs.end()) continue;
			seen_dirs.insert(path);
			/* Check if exists and mark found */
			bool exists = FileExists(path);
			found |= exists;
			/* Print */
			if (!sdn.default_only || exists) {
				IConsolePrint(exists ? CC_DEFAULT : CC_INFO, "{} {}", path, exists ? "[ok]" : "[not found]");
				if (sdn.default_only) break;
			}
		}
		if (!found) {
			IConsolePrint(CC_ERROR, "No directories exist for category {}", argv[1]);
		}
		return true;
	}

	IConsolePrint(CC_ERROR, "Invalid category name: {}", argv[1]);
	return false;
}

/** Management of NewGRF profiling. @copydoc IConsoleCmdProc */
static bool ConNewGRFProfile(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Collect performance data about NewGRF sprite requests and callbacks. Sub-commands can be abbreviated.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile [list]':");
		IConsolePrint(CC_HELP, "  List all NewGRFs that can be profiled, and their status.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile select <grf-num>...':");
		IConsolePrint(CC_HELP, "  Select one or more GRFs for profiling.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile unselect <grf-num>...':");
		IConsolePrint(CC_HELP, "  Unselect one or more GRFs from profiling. Use the keyword \"all\" instead of a GRF number to unselect all. Removing an active profiler aborts data collection.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile start [<num-ticks>]':");
		IConsolePrint(CC_HELP, "  Begin profiling all selected GRFs. If a number of ticks is provided, profiling stops after that many game ticks. There are 74 ticks in a calendar day.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile stop':");
		IConsolePrint(CC_HELP, "  End profiling and write the collected data to CSV files.");
		IConsolePrint(CC_HELP, "Usage: 'newgrf_profile abort':");
		IConsolePrint(CC_HELP, "  End profiling and discard all collected data.");
		return true;
	}

	std::span<const GRFFile> files = GetAllGRFFiles();

	/* "list" sub-command */
	if (argv.size() == 1 || StrStartsWithIgnoreCase(argv[1], "lis")) {
		IConsolePrint(CC_INFO, "Loaded GRF files:");
		int i = 1;
		for (const auto &grf : files) {
			auto profiler = std::ranges::find(_newgrf_profilers, &grf, &NewGRFProfiler::grffile);
			bool selected = profiler != _newgrf_profilers.end();
			bool active = selected && profiler->active;
			TextColour tc = active ? TextColour::LightBlue : selected ? TextColour::Green : CC_INFO;
			std::string_view statustext = active ? " (active)" : selected ? " (selected)" : "";
			IConsolePrint(tc, "{}: [{:08X}] {}{}", i, std::byteswap(grf.grfid), grf.filename, statustext);
			i++;
		}
		return true;
	}

	/* "select" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sel") && argv.size() >= 3) {
		for (size_t argnum = 2; argnum < argv.size(); ++argnum) {
			auto grfnum = ParseInteger(argv[argnum]);
			if (!grfnum.has_value() || *grfnum < 1 || static_cast<size_t>(*grfnum) > files.size()) {
				IConsolePrint(CC_WARNING, "GRF number {} out of range, not added.", *grfnum);
				continue;
			}
			const GRFFile *grf = &files[*grfnum - 1];
			if (std::any_of(_newgrf_profilers.begin(), _newgrf_profilers.end(), [&](NewGRFProfiler &pr) { return pr.grffile == grf; })) {
				IConsolePrint(CC_WARNING, "GRF number {} [{:08X}] is already selected for profiling.", *grfnum, std::byteswap(grf->grfid));
				continue;
			}
			_newgrf_profilers.emplace_back(grf);
		}
		return true;
	}

	/* "unselect" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "uns") && argv.size() >= 3) {
		for (size_t argnum = 2; argnum < argv.size(); ++argnum) {
			if (StrEqualsIgnoreCase(argv[argnum], "all")) {
				_newgrf_profilers.clear();
				break;
			}
			auto grfnum = ParseInteger(argv[argnum]);
			if (!grfnum.has_value() || *grfnum < 1 || static_cast<size_t>(*grfnum) > files.size()) {
				IConsolePrint(CC_WARNING, "GRF number {} out of range, not removing.", *grfnum);
				continue;
			}
			const GRFFile *grf = &files[*grfnum - 1];
			_newgrf_profilers.erase(std::ranges::find(_newgrf_profilers, grf, &NewGRFProfiler::grffile));
		}
		return true;
	}

	/* "start" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sta")) {
		std::string grfids;
		size_t started = 0;
		for (NewGRFProfiler &pr : _newgrf_profilers) {
			if (!pr.active) {
				pr.Start();
				started++;

				if (!grfids.empty()) grfids += ", ";
				format_append(grfids, "[{:08X}]", std::byteswap(pr.grffile->grfid));
			}
		}
		if (started > 0) {
			IConsolePrint(CC_DEBUG, "Started profiling for GRFID{} {}.", (started > 1) ? "s" : "", grfids);

			if (argv.size() >= 3) {
				auto ticks = StringConsumer{argv[2]}.TryReadIntegerBase<uint64_t>(0);
				if (!ticks.has_value()) {
					IConsolePrint(CC_ERROR, "No valid amount of ticks was given, profiling will not stop automatically.");
				} else {
					NewGRFProfiler::StartTimer(*ticks);
					IConsolePrint(CC_DEBUG, "Profiling will automatically stop after {} ticks.", *ticks);
				}
			}
		} else if (_newgrf_profilers.empty()) {
			IConsolePrint(CC_ERROR, "No GRFs selected for profiling, did not start.");
		} else {
			IConsolePrint(CC_ERROR, "Did not start profiling for any GRFs, all selected GRFs are already profiling.");
		}
		return true;
	}

	/* "stop" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "sto")) {
		NewGRFProfiler::FinishAll();
		return true;
	}

	/* "abort" sub-command */
	if (StrStartsWithIgnoreCase(argv[1], "abo")) {
		for (NewGRFProfiler &pr : _newgrf_profilers) {
			pr.Abort();
		}
		NewGRFProfiler::AbortTimer();
		return true;
	}

	return false;
}

#ifdef _DEBUG
/******************
 *  debug commands
 ******************/

static void IConsoleDebugLibRegister()
{
	IConsole::CmdRegister("resettile",        ConResetTile);
	IConsole::AliasRegister("dbg_echo",       "echo %A; echo %B");
	IConsole::AliasRegister("dbg_echo2",      "echo %!");
}
#endif

/** Show the current framerate statistics. @copydoc IConsoleCmdProc */
static bool ConFramerate(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show frame rate and game speed information.");
		return true;
	}

	ConPrintFramerate();
	return true;
}

/** Show the framerate statistics window. @copydoc IConsoleCmdProc */
static bool ConFramerateWindow(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Open the frame rate window.");
		return true;
	}

	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "Can not open frame rate window on a dedicated server.");
		return false;
	}

	ShowFramerateWindow();
	return true;
}

/**
 * Format a label as a string.
 * If all elements are visible ASCII (excluding space) then the label will be formatted as a string of 4 characters,
 * otherwise it will be output as an 8-digit hexadecimal value.
 * @param label Label to format.
 * @return string representation of label.
 **/
static std::string FormatLabel(uint32_t label)
{
	if (std::isgraph(GB(label, 24, 8)) && std::isgraph(GB(label, 16, 8)) && std::isgraph(GB(label, 8, 8)) && std::isgraph(GB(label, 0, 8))) {
		return fmt::format("{:c}{:c}{:c}{:c}", GB(label, 24, 8), GB(label, 16, 8), GB(label, 8, 8), GB(label, 0, 8));
	}

	return fmt::format("{:08X}", label);
}

/** List all road types and their configuration. */
static void ConDumpRoadTypes()
{
	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    c = catenary");
	IConsolePrint(CC_DEFAULT, "    l = no level crossings");
	IConsolePrint(CC_DEFAULT, "    X = no houses");
	IConsolePrint(CC_DEFAULT, "    h = hidden");
	IConsolePrint(CC_DEFAULT, "    T = buildable by towns");

	std::map<uint32_t, const GRFFile *> grfs;
	for (RoadType rt : EnumRange(ROADTYPE_END)) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(rt);
		if (rti->label == 0) continue;
		GrfID grfid{};
		const GRFFile *grf = rti->grffile[RoadSpriteType::Ground];
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.emplace(grfid, grf);
		}
		IConsolePrint(CC_DEFAULT, "  {:02d} {} {}, Flags: {}{}{}{}{}, GRF: {:08X}, {}",
				(uint)rt,
				RoadTypeIsTram(rt) ? "Tram" : "Road",
				FormatLabel(rti->label),
				rti->flags.Test(RoadTypeFlag::Catenary)        ? 'c' : '-',
				rti->flags.Test(RoadTypeFlag::NoLevelCrossing) ? 'l' : '-',
				rti->flags.Test(RoadTypeFlag::NoHouses)        ? 'X' : '-',
				rti->flags.Test(RoadTypeFlag::Hidden)          ? 'h' : '-',
				rti->flags.Test(RoadTypeFlag::TownBuild)       ? 'T' : '-',
				std::byteswap(grfid),
				GetStringPtr(rti->strings.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}
}

/** List all rail types and their configuration. */
static void ConDumpRailTypes()
{
	IConsolePrint(CC_DEFAULT, "  Flags:");
	IConsolePrint(CC_DEFAULT, "    c = catenary");
	IConsolePrint(CC_DEFAULT, "    l = no level crossings");
	IConsolePrint(CC_DEFAULT, "    h = hidden");
	IConsolePrint(CC_DEFAULT, "    s = no sprite combine");
	IConsolePrint(CC_DEFAULT, "    a = always allow 90 degree turns");
	IConsolePrint(CC_DEFAULT, "    d = always disallow 90 degree turns");

	std::map<uint32_t, const GRFFile *> grfs;
	for (RailType rt : EnumRange(RAILTYPE_END)) {
		const RailTypeInfo *rti = GetRailTypeInfo(rt);
		if (rti->label == 0) continue;
		GrfID grfid{};
		const GRFFile *grf = rti->grffile[RailSpriteType::Ground];
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.emplace(grfid, grf);
		}
		IConsolePrint(CC_DEFAULT, "  {:02d} {}, Flags: {}{}{}{}{}{}, GRF: {:08X}, {}",
				(uint)rt,
				FormatLabel(rti->label),
				rti->flags.Test(RailTypeFlag::Catenary)        ? 'c' : '-',
				rti->flags.Test(RailTypeFlag::NoLevelCrossing) ? 'l' : '-',
				rti->flags.Test(RailTypeFlag::Hidden)          ? 'h' : '-',
				rti->flags.Test(RailTypeFlag::NoSpriteCombine) ? 's' : '-',
				rti->flags.Test(RailTypeFlag::Allow90Deg)      ? 'a' : '-',
				rti->flags.Test(RailTypeFlag::Disallow90Deg)   ? 'd' : '-',
				std::byteswap(grfid),
				GetStringPtr(rti->strings.name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}
}

/** List all cargo types and their configuration. */
static void ConDumpCargoTypes()
{
	IConsolePrint(CC_DEFAULT, "  Cargo classes:");
	IConsolePrint(CC_DEFAULT, "    p = passenger");
	IConsolePrint(CC_DEFAULT, "    m = mail");
	IConsolePrint(CC_DEFAULT, "    x = express");
	IConsolePrint(CC_DEFAULT, "    a = armoured");
	IConsolePrint(CC_DEFAULT, "    b = bulk");
	IConsolePrint(CC_DEFAULT, "    g = piece goods");
	IConsolePrint(CC_DEFAULT, "    l = liquid");
	IConsolePrint(CC_DEFAULT, "    r = refrigerated");
	IConsolePrint(CC_DEFAULT, "    h = hazardous");
	IConsolePrint(CC_DEFAULT, "    c = covered/sheltered");
	IConsolePrint(CC_DEFAULT, "    o = oversized");
	IConsolePrint(CC_DEFAULT, "    d = powderized");
	IConsolePrint(CC_DEFAULT, "    n = not pourable");
	IConsolePrint(CC_DEFAULT, "    e = potable");
	IConsolePrint(CC_DEFAULT, "    i = non-potable");
	IConsolePrint(CC_DEFAULT, "    S = special");

	std::map<uint32_t, const GRFFile *> grfs;
	for (const CargoSpec *spec : CargoSpec::Iterate()) {
		GrfID grfid{};
		const GRFFile *grf = spec->grffile;
		if (grf != nullptr) {
			grfid = grf->grfid;
			grfs.emplace(grfid, grf);
		}
		IConsolePrint(CC_DEFAULT, "  {:02d} Bit: {:2d}, Label: {}, Callback mask: 0x{:02X}, Cargo class: {}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}, GRF: {:08X}, {}",
				spec->Index(),
				spec->bitnum,
				FormatLabel(spec->label.base()),
				spec->callback_mask.base(),
				spec->classes.Test(CargoClass::Passengers)   ? 'p' : '-',
				spec->classes.Test(CargoClass::Mail)         ? 'm' : '-',
				spec->classes.Test(CargoClass::Express)      ? 'x' : '-',
				spec->classes.Test(CargoClass::Armoured)     ? 'a' : '-',
				spec->classes.Test(CargoClass::Bulk)         ? 'b' : '-',
				spec->classes.Test(CargoClass::PieceGoods)   ? 'g' : '-',
				spec->classes.Test(CargoClass::Liquid)       ? 'l' : '-',
				spec->classes.Test(CargoClass::Refrigerated) ? 'r' : '-',
				spec->classes.Test(CargoClass::Hazardous)    ? 'h' : '-',
				spec->classes.Test(CargoClass::Covered)      ? 'c' : '-',
				spec->classes.Test(CargoClass::Oversized)    ? 'o' : '-',
				spec->classes.Test(CargoClass::Powderized)   ? 'd' : '-',
				spec->classes.Test(CargoClass::NotPourable)  ? 'n' : '-',
				spec->classes.Test(CargoClass::Potable)      ? 'e' : '-',
				spec->classes.Test(CargoClass::NonPotable)   ? 'i' : '-',
				spec->classes.Test(CargoClass::Special)      ? 'S' : '-',
				std::byteswap(grfid),
				GetStringPtr(spec->name)
		);
	}
	for (const auto &grf : grfs) {
		IConsolePrint(CC_DEFAULT, "  GRF: {:08X} = {}", std::byteswap(grf.first), grf.second->filename);
	}
	/* Where the cargo for road vehicles on wagons ended up (road_on_rail.h):
	 * the one thing the table above cannot show, since it is what the fitting
	 * reads, not what the table holds. */
	if (IsValidCargoType(_road_vehicle_cargo)) {
		uint wagons = 0, offering = 0, carriers = 0, carriers_buildable = 0;
		for (const Engine *e : Engine::Iterate()) {
			if (e->type != VehicleType::Train || e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon) continue;
			wagons++;
			if (e->info.refit_mask.Test(_road_vehicle_cargo)) offering++;
			if (e->info.cargo_type == _road_vehicle_cargo) {
				carriers++;
				if (e->info.climates.Any()) carriers_buildable++;
			}
		}
		/* How big a cargo icon is, at the zoom the game is drawn at: what an
		 * icon of the cargo's own has to match. */
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			Dimension d = GetSpriteSize(cs->GetCargoIcon());
			IConsolePrint(CC_DEFAULT, "  cargo icon {}: {} x {} px (sprite {})", GetString(cs->name), d.width, d.height, cs->GetCargoIcon());
		}
		IConsolePrint(CC_DEFAULT, "  Road vehicles on wagons (ROLA): slot {}, in cargo mask: {}, in the refit mask of {} of {} wagon types; car carriers {} ({} available in this climate)",
				_road_vehicle_cargo, _cargo_mask.Test(_road_vehicle_cargo) ? "yes" : "NO", offering, wagons, carriers, carriers_buildable);
		/* Which ones they are. A car carrier is meant to be one of ours; a
		 * wagon out of somebody's set turning up here is a set's wagon that
		 * landed on this cargo by accident, and worth seeing by name. */
		for (const Engine *e : Engine::Iterate()) {
			if (e->type != VehicleType::Train || e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon) continue;
			if (e->info.cargo_type != _road_vehicle_cargo) continue;
			const GRFFile *grf = e->GetGRF();
			IConsolePrint(CC_DEFAULT, "    carrier: engine {}, local id {}, GRF {:08X}, {}available, refits {}, '{}', intro {}, company avail {}, buildable {}, hidden {}", e->index,
					e->grf_prop.local_id, grf == nullptr ? 0 : std::byteswap(grf->grfid), e->info.climates.Any() ? "" : "not ",
					e->info.refit_mask.Low(), GetString(e->info.string_id), e->intro_date,
					e->company_avail.Test(_local_company) ? "yes" : "NO",
					IsEngineBuildable(e->index, VehicleType::Train, _local_company) ? "yes" : "NO",
					e->IsHidden(_local_company) ? "yes" : "no");
		}
		/* Vehicles as freight (the label VEHI): a car carrier could borrow the
		 * picture a set drew for that, since it is a car transporter and the
		 * right shape -- but only in a game that has no such cargo of its own,
		 * or a wagon carrying cars as freight and one carrying a car that
		 * drives itself would look alike. See TEMATA8 46. Whether a set drew
		 * one cannot be counted here: what is drawn is only worked out for the
		 * one set with a cargo exception, and is found for anybody else by
		 * asking its sprite chains and seeing what comes back. So this says
		 * only how many wagons could be asked at all. */
		static constexpr CargoLabel CT_VEHI{'VEHI'};
		uint in_table = 0;
		for (const Engine *e : Engine::Iterate()) {
			if (e->type != VehicleType::Train || e->VehInfo<RailVehicleInfo>().railveh_type != RailVehicleType::Wagon) continue;
			const GRFFile *grf = e->GetGRF();
			if (grf != nullptr && find_index(grf->cargo_list, CT_VEHI) >= 0) in_table++;
		}
		IConsolePrint(CC_DEFAULT, "  VEHI (vehicles as freight): a cargo in this game: {}; wagon types naming it in their cargo table: {}",
				IsValidCargoType(GetCargoTypeByLabel(CT_VEHI)) ? "yes" : "no", in_table);
	} else {
		IConsolePrint(CC_DEFAULT, "  Road vehicles on wagons (ROLA): NOT IN THIS GAME (label not found)");
	}
}

/** Dump information about some NewGRF types. @copydoc IConsoleCmdProc */
static bool ConDumpInfo(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Dump debugging information.");
		IConsolePrint(CC_HELP, "Usage: 'dump_info roadtypes|railtypes|cargotypes'.");
		IConsolePrint(CC_HELP, "  Show information about road/tram types, rail types or cargo types.");
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "roadtypes")) {
		ConDumpRoadTypes();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "railtypes")) {
		ConDumpRailTypes();
		return true;
	}

	if (StrEqualsIgnoreCase(argv[1], "cargotypes")) {
		ConDumpCargoTypes();
		return true;
	}

	return false;
}

/** Console command registration. */
/**
 * Everything worth reading about a game at once, so that a fault caught while
 * playing needs one word typed and not five.
 *
 * The player plays; the game goes wrong; what is wanted then is the whole
 * picture, and typing five commands from memory with a broken train on the
 * screen is how half of it gets forgotten. This runs them all in one go --
 * where every train stands, where it is, what its orders say, whether any
 * wagon is pointing two ways at once, and the shape of every consist -- and
 * then 'vlaksav' puts the lot in a file.
 * Usage: testvse
 * @copydoc IConsoleCmdProc
 */
static bool ConTestEverything(std::span<std::string_view>)
{
	std::array<std::string_view, 1> self{"testvse"};

	IConsolePrint(CC_WARNING, "=== testvse: kde co stoji ===");
	ConTestCoupleState(self);
	IConsolePrint(CC_WARNING, "=== testvse: kde jsou vlaky ===");
	ConTestWhere(self);
	IConsolePrint(CC_WARNING, "=== testvse: rozkazy ===");
	ConTestOrders(self);
	IConsolePrint(CC_WARNING, "=== testvse: natoceni clanku ===");
	ConTestFacings(self);
	IConsolePrint(CC_WARNING, "=== testvse: rozkazy aut ===");
	ConTestRoadOrders(self);

	IConsolePrint(CC_WARNING, "=== testvse: tvar souprav ===");
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t || !t->IsFrontEngine()) continue;
		std::string number = fmt::format("{}", t->unitnumber);
		std::array<std::string_view, 2> args{"testtvar", number};
		ConTestWagonShape(args);
	}

	/* And how every one of them is drawn, rakes too -- the picture is the one
	 * thing the player can see and the rig cannot, so the rig writes it down. */
	IConsolePrint(CC_WARNING, "=== testvse: kresleni ===");
	for (const Train *t : Train::Iterate()) {
		if (t->First() != t) continue;
		PrintConsistDrawing(t);
	}

	IConsolePrint(CC_WARNING, "=== testvse: konec. Ulozit: vlaksav ===");
	return true;
}

/**
 * Print one setting's line the way the settings window draws it: the title
 * with the value written into it, in the language the game is running in.
 *
 * Written to answer a question that cannot be answered by reading the
 * language file: a setting's title and the string that carries its value are
 * two strings that have to agree about how many parameters pass between them,
 * and each language decides that for itself. English wraps the value in
 * STR_CONFIG_SETTING_VALUE = "{ORANGE}{STRING1}" and its titles therefore say
 * {STRING2}; Czech wraps it in "{STRING}" and its titles say {STRING}. Get it
 * wrong and the line ends in "(invalid parameter)" -- in that language only,
 * which is how it goes unnoticed. This asks the game, which settles it.
 *
 * Usage: testnapis <setting name>
 * @copydoc IConsoleCmdProc
 */
static bool ConTestNapis(std::span<std::string_view> argv)
{
	if (argv.size() < 2) {
		IConsolePrint(CC_HELP, "Print a setting's line as the window draws it. Usage: 'testnapis <nastaveni>'.");
		return true;
	}
	const SettingDesc *sd = GetSettingFromName(argv[1]);
	if (sd == nullptr || !sd->IsIntSetting()) {
		IConsolePrint(CC_ERROR, "testnapis: nezname nastaveni");
		return true;
	}
	const IntSettingDesc *isd = sd->AsIntSetting();
	int32_t value = isd->Read(&GetGameSettings());
	auto [param1, param2] = isd->GetValueParams(value);
	IConsolePrint(CC_INFO, "NAPIS: {}", GetString(isd->GetTitle(), STR_CONFIG_SETTING_VALUE, param1, param2));
	return true;
}

/**
 * Open the orders window of every vehicle the company has, one kind at a time,
 * and then refresh them all the way a repaint does.
 *
 * The windows exist without a screen -- the null video driver draws nothing
 * but the window system is the same one -- so a window that goes down when it
 * is opened goes down here too. Written when opening an aircraft's orders
 * brought the game down for the player and the rig had never opened one: every
 * scene drives vehicles, none of them looks at them.
 *
 * Usage: testrozkazokna
 * @copydoc IConsoleCmdProc
 */
static bool ConTestOrderWindows(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Open every vehicle's orders window. Usage: 'testrozkazokna'.");
		return true;
	}
	uint opened = 0;
	for (Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle()) continue;
		AutoRestoreBackup cur_company(_current_company, v->owner);
		IConsolePrint(CC_DEFAULT, "testrozkazokna: otviram rozkazy {} {}", to_underlying(v->type), v->unitnumber);
		ShowOrdersWindow(v);
		opened++;
	}
	/* And a repaint of all of them, which is where a window that only looks
	 * right until something asks it to draw gives itself away. */
	for (Window *w : Window::Iterate()) w->SetDirty();
	IConsolePrint(CC_DEFAULT, "testrozkazokna: otevreno {} oken.", opened);
	return true;
}

/**
 * Build the scene for road vehicles riding in a ship: a canal with a dock at
 * each end, a road stop of each dock's station beside it, one ship fitted to
 * carry road vehicles shuttling between them, and road vehicles ordered to
 * board at the first and get off at the second.
 *
 * The water is dug rather than found. A dock needs an inclined tile with water
 * in front of it, and a map generated flat -- which is what the rig generates,
 * on purpose (see tests/rig/README.md) -- has neither. So the scene raises two
 * corners of each dock's tile to make the slope, digs a canal along the row in
 * front, and builds the docks against it.
 *
 * "protijedouci": as many cars again wait at the second station to sail to
 * the first, so the ship that puts cars down takes others away -- the
 * player's "it does not load when it unloads, in one step".
 *
 * Usage: testautolod [how many road vehicles, 1 by default] [protijedouci]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRoadOnWater(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the road-vehicle-in-a-ship scene. Usage: 'testautolod [pocet aut] [protijedouci]'.");
		return true;
	}
	uint cars = 1;
	if (argv.size() >= 2) {
		auto pcars = ParseInteger(argv[1]);
		if (!pcars.has_value() || *pcars < 1) return false;
		cars = (uint)*pcars;
	}
	bool both_ways = argv.size() >= 3 && argv[2] == "protijedouci";
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testautolod: only in a running game.");
		return true;
	}
	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testautolod: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	AutoRestoreBackup cur_company(_current_company, _local_company);

	/* A ship that carries road vehicles at all, and any road vehicle. */
	EngineID eid_ship = EngineID::Invalid();
	EngineID eid_road = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Ship)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!CanCarryRoadVehicles(e)) continue;
		eid_ship = e->index;
		break;
	}
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		eid_road = e->index;
		break;
	}
	if (eid_ship == EngineID::Invalid() || eid_road == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testautolod: no ship that carries cars, or no road vehicle.");
		return true;
	}

	/* Five rows: the dock land, two rows of canal -- a dock reaches two tiles
	 * out over the water and refuses to be built with only one -- the road,
	 * and room for the shed. */
	static const uint LEN = 30;
	static const uint DEPTH = 5;
	TileIndex strip = INVALID_TILE;
	for (uint y = 4; y + DEPTH < Map::SizeY() - 4 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			bool ok = true;
			int z = -1;
			for (uint dy = 0; dy < DEPTH && ok; dy++) {
				TileIndex t = TileXY(x, y + dy);
				ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
				if (ok) {
					if (dy == 0) z = GetTileZ(t); else if (GetTileZ(t) != z) ok = false;
				}
			}
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					strip = TileXY(x - LEN + 1, y);
					break;
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testautolod: no flat clear area of {}x{} tiles found.", LEN, DEPTH);
		return true;
	}
	uint x0 = TileX(strip), y0 = TileY(strip);
	IConsolePrint(CC_DEFAULT, "testautolod: area at ({},{})..({},{}).", x0, y0, x0 + LEN - 1, y0 + DEPTH - 1);

	/* The canal first, along the row the docks will face. Water is built here,
	 * not dug: a hole in flat land stays a hole, since nothing floods it
	 * unless it reaches the sea. */
	CommandCost canal = Command<Commands::BuildCanal>::Do(DoCommandFlag::Execute, TileXY(x0 + 1, y0 + 1), TileXY(x0 + LEN - 2, y0 + 2), WaterClass::Canal, false);
	if (canal.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: canal failed - {}", RefusalReason(canal));
		return true;
	}

	/* Then the shore: a dock wants an inclined tile with water in front of it,
	 * so the north and west corners of each dock tile go up, which leaves the
	 * tile falling towards the canal. It has to be done in this order -- the
	 * corners a tile shares with the water beside it cannot be raised once the
	 * water is there, and these two are the pair it does not share. */
	uint dock_ax = x0 + 3;
	uint dock_bx = x0 + LEN - 4;
	for (uint x : {dock_ax, dock_bx}) {
		auto [terra, cost_t, tile_t] = Command<Commands::TerraformLand>::Do(DoCommandFlag::Execute, TileXY(x, y0), SLOPE_NW, true);
		if (terra.Failed()) {
			IConsolePrint(CC_ERROR, "testautolod: terraform at ({},{}) failed - {}", x, y0, RefusalReason(terra));
			return true;
		}
	}

	CommandCost dock_a = Command<Commands::BuildDock>::Do(DoCommandFlag::Execute, TileXY(dock_ax, y0), StationID::Invalid(), false);
	CommandCost dock_b = Command<Commands::BuildDock>::Do(DoCommandFlag::Execute, TileXY(dock_bx, y0), StationID::Invalid(), false);
	if (dock_a.Failed() || dock_b.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: dock failed - {} / {} (zeme {:#x} v {}, voda {:#x} v {}, voda? {})",
				RefusalReason(dock_a), RefusalReason(dock_b),
				(uint)GetTileSlope(TileXY(dock_ax, y0)), GetTileZ(TileXY(dock_ax, y0)),
				(uint)GetTileSlope(TileXY(dock_ax, y0 + 1)), GetTileZ(TileXY(dock_ax, y0 + 1)),
				HasTileWaterGround(TileXY(dock_ax, y0 + 1)) ? "ano" : "ne");
		return true;
	}
	StationID id_a = GetStationIndex(TileXY(dock_ax, y0));
	StationID id_b = GetStationIndex(TileXY(dock_bx, y0));

	/* A ship depot at the far end of the canal, out of the way of the docks. */
	TileIndex depot = TileXY(x0 + LEN / 2, y0 + 1);
	CommandCost sd = Command<Commands::BuildShipDepot>::Do(DoCommandFlag::Execute, depot, Axis::X);
	if (sd.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: ship depot failed - {}", RefusalReason(sd));
		return true;
	}

	/* The road along the row behind the canal, with a stop below each dock. */
	uint road_y = y0 + 3;
	CommandCost road = Command<Commands::BuildRoadLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, road_y), TileXY(x0 + 1, road_y),
			ROADTYPE_ROAD, Axis::X, DisallowedRoadDirections{}, false, false, false);
	if (road.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: road failed - {}", RefusalReason(road));
		return true;
	}
	bool bus = IsCargoInClass(Engine::Get(eid_road)->GetDefaultCargoType(), CargoClass::Passengers);
	RoadStopType stop_type = bus ? RoadStopType::Bus : RoadStopType::Truck;
	CommandCost rs_a = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, TileXY(dock_ax, road_y), 1, 1, stop_type, true, DiagDirection::NE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, id_a, false);
	CommandCost rs_b = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, TileXY(dock_bx, road_y), 1, 1, stop_type, true, DiagDirection::NE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, id_b, false);
	if (rs_a.Failed() || rs_b.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: road stop failed - {} / {}", RefusalReason(rs_a), RefusalReason(rs_b));
		return true;
	}
	TileIndex road_depot = TileXY(x0 + 6, road_y + 1);
	CommandCost rd = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, road_depot, ROADTYPE_ROAD, DiagDirection::NW);
	CommandCost rd_link = Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, TileXY(x0 + 6, road_y), RoadBits{RoadBit::SE}, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
	if (rd.Failed() || rd_link.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: road depot failed - {} / {}", RefusalReason(rd), RefusalReason(rd_link));
		return true;
	}

	/* The ship, bought fitted for road vehicles, shuttling A - B. */
	/* A passenger ship takes cars beside its passengers and is bought as it
	 * comes; any other is fitted for them. */
	CargoType ship_cargo = TakesRoadVehiclesBesidePassengers(Engine::Get(eid_ship)) ? INVALID_CARGO : _road_vehicle_cargo;
	auto [cost_s, veh_s, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot, eid_ship, true, ship_cargo, ClientID::Invalid);
	if (cost_s.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: ship failed - {}", RefusalReason(cost_s));
		return true;
	}
	Order ship_to_a{};
	ship_to_a.MakeGoToStation(id_a);
	ship_to_a.SetStopLocation(OrderStopLocation::FarEnd);
	Order ship_to_b{};
	ship_to_b.MakeGoToStation(id_b);
	ship_to_b.SetStopLocation(OrderStopLocation::FarEnd);
	CommandCost ins_sa = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_s, 0, ship_to_a);
	CommandCost ins_sb = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_s, 1, ship_to_b);
	if (ins_sa.Failed() || ins_sb.Failed()) {
		IConsolePrint(CC_ERROR, "testautolod: ship orders refused - {} / {}", RefusalReason(ins_sa), RefusalReason(ins_sb));
		return true;
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_s, false);

	/* The cars: to the first station and aboard, off at the second -- and,
	 * with "protijedouci", as many the other way. */
	std::string built;
	for (uint i = 0; i < (both_ways ? 2 * cars : cars); i++) {
		bool back = i >= cars;
		auto [cost_r, veh_r, un_d, un_e, un_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, road_depot, eid_road, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_r.Failed()) {
			IConsolePrint(CC_ERROR, "testautolod: road vehicle failed - {}", RefusalReason(cost_r));
			return true;
		}
		Order car_a{};
		car_a.MakeGoToStation(id_a);
		car_a.SetStopLocation(OrderStopLocation::FarEnd);
		Order car_b{};
		car_b.MakeGoToStation(id_b);
		car_b.SetStopLocation(OrderStopLocation::FarEnd);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_r, 0, back ? car_b : car_a);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_r, 1, back ? car_a : car_b);
		CommandCost mod = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, veh_r, 0, MOF_BOARD_MODE, to_underlying(OrderBoardMode::ShipToNext));
		if (mod.Failed()) {
			IConsolePrint(CC_ERROR, "testautolod: boarding order refused - {}", RefusalReason(mod));
			return true;
		}
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_r, false);
		if (!built.empty()) built += ",";
		built += fmt::format("{}{}", RoadVehicle::Get(veh_r)->unitnumber, back ? " zpet" : "");
	}

	IConsolePrint(CC_DEFAULT, "testautolod: lod {} (mista pro {} aut, naklad {} {}) vozi auta {} mezi stanicemi {} a {}.",
			Ship::Get(veh_s)->unitnumber, RoadVehicleRoomIn(Ship::Get(veh_s)), Ship::Get(veh_s)->cargo_cap,
			GetString(CargoSpec::Get(Ship::Get(veh_s)->cargo_type)->name), built, id_a, id_b);
	return true;
}

/**
 * Build the scene for road vehicles riding in an aircraft: two small airports
 * with a road stop of their own station beside each, one aeroplane fitted to
 * carry road vehicles shuttling between them, and one road vehicle ordered to
 * board at the first and get off at the second.
 *
 * The sister of testautovlak for the air (road_on_rail.h). An aircraft takes
 * one road vehicle and only ever towards the vehicle's next stop, so there is
 * no choice of how to board to make here.
 *
 * "tamizpet": the car boards at the second station as well, so it flies back
 * -- the player's case, an aircraft that brought a car and took none away.
 *
 * "protijedouci": a second car waits at the second station to fly to the
 * first, so the aircraft that brings one car in has another to take out --
 * the player's "one gets off, the other does not make it on in that step".
 *
 * Usage: testautoletadlo [tamizpet] [protijedouci]
 * @copydoc IConsoleCmdProc
 */
static bool ConTestRoadOnAir(std::span<std::string_view> argv)
{
	bool both_ways = false;
	bool second_car = false;
	for (size_t i = 1; i < argv.size(); i++) {
		if (argv[i] == "tamizpet") both_ways = true;
		if (argv[i] == "protijedouci") second_car = true;
	}
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the road-vehicle-in-an-aircraft scene. Usage: 'testautoletadlo [tamizpet] [protijedouci]'.");
		return true;
	}
	if (_game_mode != GameMode::Normal) {
		IConsolePrint(CC_ERROR, "testautoletadlo: only in a running game.");
		return true;
	}
	if (Company::GetIfValid(_local_company) == nullptr) {
		extern Company *DoStartupNewCompany(bool is_ai, CompanyID company);
		Company *made = DoStartupNewCompany(false, CompanyID::Invalid());
		if (made == nullptr) {
			IConsolePrint(CC_ERROR, "testautoletadlo: no company to build as.");
			return true;
		}
		SetLocalCompany(made->index);
	}
	Command<Commands::MoneyCheat>::Do(DoCommandFlag::Execute, 100000000);
	AutoRestoreBackup cur_company(_current_company, _local_company);

	/* The aeroplanes of the earliest years seat too few people to carry a car
	 * (see RoadVehiclesCarriedBy()), which is the rule working as asked and a
	 * scene that cannot be built. So the scene moves the calendar on. */
	if (TimerGameCalendar::year < TimerGameCalendar::Year{1970}) MoveCalendarTo(TimerGameCalendar::Year{1970});

	/* An aeroplane that carries a road vehicle at all (60 seats, see
	 * RoadVehiclesCarriedBy()) and can use a small airport, and any road
	 * vehicle to put in it. */
	EngineID eid_air = EngineID::Invalid();
	EngineID eid_road = EngineID::Invalid();
	for (const Engine *e : Engine::IterateType(VehicleType::Aircraft)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (e->VehInfo<AircraftVehicleInfo>().subtype != AIR_CTOL) continue;
		if (!CanCarryRoadVehicles(e)) continue;
		eid_air = e->index;
		break;
	}
	for (const Engine *e : Engine::IterateType(VehicleType::Road)) {
		if (!e->company_avail.Test(_local_company)) continue;
		eid_road = e->index;
		break;
	}
	if (eid_air == EngineID::Invalid() || eid_road == EngineID::Invalid()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: no aeroplane that carries cars, or no road vehicle.");
		return true;
	}

	/* Which airport to build: the first kind this year offers that aeroplanes
	 * can use at all. The small airport is the obvious one and it is also the
	 * one that stops being available part way through the game -- the very
	 * years this scene has to move to for an aeroplane big enough to take a
	 * car -- so the kind cannot be written down here. */
	uint8_t airport_type = NUM_AIRPORTS;
	uint ap_w = 0;
	uint ap_h = 0;
	for (uint8_t i = 0; i < NUM_AIRPORTS; i++) {
		const AirportSpec *as = AirportSpec::Get(i);
		if (!as->IsAvailable() || as->layouts.empty()) continue;
		if (!GetAirport(i)->flags.Test(AirportFTAClass::Flag::Airplanes)) continue;
		airport_type = i;
		ap_w = as->size_x;
		ap_h = as->size_y;
		break;
	}
	if (airport_type == NUM_AIRPORTS) {
		IConsolePrint(CC_ERROR, "testautoletadlo: no airport for aeroplanes is available this year.");
		return true;
	}

	/* Two airports side by side with a gap between them, and the road along the
	 * row below. */
	const uint LEN = 2 * ap_w + 16;
	const uint DEPTH = ap_h + 2;
	TileIndex strip = INVALID_TILE;
	for (uint y = 4; y + DEPTH < Map::SizeY() - 4 && strip == INVALID_TILE; y++) {
		uint run = 0;
		int z0 = 0;
		for (uint x = 2; x < Map::SizeX() - 2; x++) {
			bool ok = true;
			int z = -1;
			for (uint dy = 0; dy < DEPTH && ok; dy++) {
				TileIndex t = TileXY(x, y + dy);
				ok = (IsTileType(t, TileType::Clear) || IsTileType(t, TileType::Trees)) && GetTileSlope(t) == SLOPE_FLAT;
				if (ok) {
					if (dy == 0) z = GetTileZ(t); else if (GetTileZ(t) != z) ok = false;
				}
			}
			if (ok && (run == 0 || z == z0)) {
				if (run == 0) z0 = z;
				if (++run == LEN) {
					strip = TileXY(x - LEN + 1, y);
					break;
				}
			} else {
				run = 0;
			}
		}
	}
	if (strip == INVALID_TILE) {
		IConsolePrint(CC_ERROR, "testautoletadlo: no flat clear area of {}x{} tiles found.", LEN, DEPTH);
		return true;
	}
	uint x0 = TileX(strip), y0 = TileY(strip);
	IConsolePrint(CC_DEFAULT, "testautoletadlo: area at ({},{})..({},{}).", x0, y0, x0 + LEN - 1, y0 + DEPTH - 1);

	TileIndex ap_a = TileXY(x0 + 2, y0);
	TileIndex ap_b = TileXY(x0 + ap_w + 12, y0);
	CommandCost air_a = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, ap_a, airport_type, 0, StationID::Invalid(), false);
	CommandCost air_b = Command<Commands::BuildAirport>::Do(DoCommandFlag::Execute, ap_b, airport_type, 0, StationID::Invalid(), false);
	if (air_a.Failed() || air_b.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: airport failed - {} / {}", RefusalReason(air_a), RefusalReason(air_b));
		return true;
	}
	StationID id_a = GetStationIndex(ap_a);
	StationID id_b = GetStationIndex(ap_b);

	/* The road along the last row, with a stop below each airport joined to
	 * its station, and a shed to build the car in. */
	uint road_y = y0 + DEPTH - 1;
	CommandCost road = Command<Commands::BuildRoadLong>::Do(DoCommandFlag::Execute, TileXY(x0 + LEN - 2, road_y), TileXY(x0 + 1, road_y),
			ROADTYPE_ROAD, Axis::X, DisallowedRoadDirections{}, false, false, false);
	if (road.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: road failed - {}", RefusalReason(road));
		return true;
	}
	bool bus = IsCargoInClass(Engine::Get(eid_road)->GetDefaultCargoType(), CargoClass::Passengers);
	RoadStopType stop_type = bus ? RoadStopType::Bus : RoadStopType::Truck;
	CommandCost rs_a = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, TileXY(x0 + 3, road_y), 1, 1, stop_type, true, DiagDirection::NE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, id_a, false);
	CommandCost rs_b = Command<Commands::BuildRoadStop>::Do(DoCommandFlag::Execute, TileXY(x0 + ap_w + 13, road_y), 1, 1, stop_type, true, DiagDirection::NE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0, id_b, false);
	if (rs_a.Failed() || rs_b.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: road stop failed - {} / {}", RefusalReason(rs_a), RefusalReason(rs_b));
		return true;
	}
	TileIndex road_depot = TileXY(x0 + 6, road_y + 1);
	CommandCost rd = Command<Commands::BuildRoadDepot>::Do(DoCommandFlag::Execute, road_depot, ROADTYPE_ROAD, DiagDirection::NW);
	CommandCost rd_link = Command<Commands::BuildRoad>::Do(DoCommandFlag::Execute, TileXY(x0 + 6, road_y), RoadBits{RoadBit::SE}, ROADTYPE_ROAD, DisallowedRoadDirections{}, TownID::Invalid());
	if (rd.Failed() || rd_link.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: road depot failed - {} / {}", RefusalReason(rd), RefusalReason(rd_link));
		return true;
	}

	/* The aeroplane, bought fitted for road vehicles, shuttling A - B. */
	const Station *st_a = Station::Get(id_a);
	auto [cost_p, veh_p, un_a, un_b, un_c] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, st_a->airport.GetHangarTile(0), eid_air, true, _road_vehicle_cargo, ClientID::Invalid);
	if (cost_p.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: aeroplane failed - {}", RefusalReason(cost_p));
		return true;
	}
	/* Aircraft, like road vehicles, have no choice of where on a platform to
	 * stop, and the order code insists on the one answer that means that. */
	Order air_to_a{};
	air_to_a.MakeGoToStation(id_a);
	air_to_a.SetStopLocation(OrderStopLocation::FarEnd);
	Order air_to_b{};
	air_to_b.MakeGoToStation(id_b);
	air_to_b.SetStopLocation(OrderStopLocation::FarEnd);
	CommandCost ins_pa = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_p, 0, air_to_a);
	CommandCost ins_pb = Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_p, 1, air_to_b);
	if (ins_pa.Failed() || ins_pb.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: aeroplane orders refused - {} / {}", RefusalReason(ins_pa), RefusalReason(ins_pb));
		return true;
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_p, false);

	/* The car: to the first station and aboard, off at the second. */
	auto [cost_r, veh_r, un_d, un_e, un_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, road_depot, eid_road, true, INVALID_CARGO, ClientID::Invalid);
	if (cost_r.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: road vehicle failed - {}", RefusalReason(cost_r));
		return true;
	}
	Order car_a{};
	car_a.MakeGoToStation(id_a);
	car_a.SetStopLocation(OrderStopLocation::FarEnd);
	Order car_b{};
	car_b.MakeGoToStation(id_b);
	car_b.SetStopLocation(OrderStopLocation::FarEnd);
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_r, 0, car_a);
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_r, 1, car_b);
	CommandCost mod = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, veh_r, 0, MOF_BOARD_MODE, to_underlying(OrderBoardMode::PlaneToNext));
	if (mod.Succeeded() && both_ways) mod = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, veh_r, 1, MOF_BOARD_MODE, to_underlying(OrderBoardMode::PlaneToNext));
	if (mod.Failed()) {
		IConsolePrint(CC_ERROR, "testautoletadlo: boarding order refused - {}", RefusalReason(mod));
		return true;
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_r, false);

	if (second_car) {
		/* The other way: to the second station by road, aboard there, off at
		 * the first. */
		auto [cost_2, veh_2, un_g, un_h, un_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, road_depot, eid_road, true, INVALID_CARGO, ClientID::Invalid);
		if (cost_2.Failed()) {
			IConsolePrint(CC_ERROR, "testautoletadlo: second road vehicle failed - {}", RefusalReason(cost_2));
			return true;
		}
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_2, 0, car_b);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_2, 1, car_a);
		CommandCost mod2 = Command<Commands::ModifyOrder>::Do(DoCommandFlag::Execute, veh_2, 0, MOF_BOARD_MODE, to_underlying(OrderBoardMode::PlaneToNext));
		if (mod2.Failed()) {
			IConsolePrint(CC_ERROR, "testautoletadlo: second boarding order refused - {}", RefusalReason(mod2));
			return true;
		}
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_2, false);
		IConsolePrint(CC_DEFAULT, "testautoletadlo: auto {} ceka ve stanici {} na let do {}.", RoadVehicle::Get(veh_2)->unitnumber, id_b, id_a);
	}

	IConsolePrint(CC_DEFAULT, "testautoletadlo: letadlo {} vozi auto {} mezi stanicemi {} a {}{}.",
			Aircraft::Get(veh_p)->unitnumber, RoadVehicle::Get(veh_r)->unitnumber, id_a, id_b, both_ways ? ", tam i zpet letadlem" : "");
	return true;
}

void IConsoleStdLibRegister()
{
	IConsole::CmdRegister("debug_level",             ConDebugLevel);
	IConsole::CmdRegister("echo",                    ConEcho);
	IConsole::CmdRegister("echoc",                   ConEchoC);
	IConsole::CmdRegister("exec",                    ConExec);
	IConsole::CmdRegister("schedule",                ConSchedule);
	IConsole::CmdRegister("exit",                    ConExit);
	IConsole::CmdRegister("part",                    ConPart);
	IConsole::CmdRegister("help",                    ConHelp);
	IConsole::CmdRegister("info_cmd",                ConInfoCmd);
	IConsole::CmdRegister("list_cmds",               ConListCommands);
	IConsole::CmdRegister("list_aliases",            ConListAliases);
	IConsole::CmdRegister("newgame",                 ConNewGame);
	IConsole::CmdRegister("restart",                 ConRestart);
	IConsole::CmdRegister("reload",                  ConReload);
	IConsole::CmdRegister("getseed",                 ConGetSeed);
	IConsole::CmdRegister("getdate",                 ConGetDate);
	IConsole::CmdRegister("getsysdate",              ConGetSysDate);
	IConsole::CmdRegister("quit",                    ConExit);
	IConsole::CmdRegister("resetengines",            ConResetEngines,     ConHookNoNetwork);
	IConsole::CmdRegister("reset_enginepool",        ConResetEnginePool,  ConHookNoNetwork);
	IConsole::CmdRegister("return",                  ConReturn);
	IConsole::CmdRegister("screenshot",              ConScreenShot);
	IConsole::CmdRegister("script",                  ConScript);
	IConsole::CmdRegister("zoomto",                  ConZoomToLevel);
	IConsole::CmdRegister("scrollto",                ConScrollToTile);
	IConsole::CmdRegister("alias",                   ConAlias);
	IConsole::CmdRegister("load",                    ConLoad);
	IConsole::CmdRegister("load_save",               ConLoad);
	IConsole::CmdRegister("load_scenario",           ConLoadScenario);
	IConsole::CmdRegister("load_heightmap",          ConLoadHeightmap);
	IConsole::CmdRegister("rm",                      ConRemove);
	IConsole::CmdRegister("save",                    ConSave);
	IConsole::CmdRegister("saveconfig",              ConSaveConfig);
	IConsole::CmdRegister("ls",                      ConListFiles);
	IConsole::CmdRegister("list_saves",              ConListFiles);
	IConsole::CmdRegister("list_scenarios",          ConListScenarios);
	IConsole::CmdRegister("list_heightmaps",         ConListHeightmaps);
	IConsole::CmdRegister("cd",                      ConChangeDirectory);
	IConsole::CmdRegister("pwd",                     ConPrintWorkingDirectory);
	IConsole::CmdRegister("clear",                   ConClearBuffer);
	IConsole::CmdRegister("font",                    ConFont);
	IConsole::CmdRegister("setting",                 ConSetting);
	IConsole::CmdRegister("setting_newgame",         ConSettingNewgame);
	IConsole::CmdRegister("list_settings",           ConListSettings);
	IConsole::CmdRegister("gamelog",                 ConGamelogPrint);
	IConsole::CmdRegister("rescan_newgrf",           ConRescanNewGRF);
	IConsole::CmdRegister("list_dirs",               ConListDirs);

	IConsole::AliasRegister("dir",                   "ls");
	IConsole::AliasRegister("del",                   "rm %+");
	IConsole::AliasRegister("newmap",                "newgame");
	IConsole::AliasRegister("patch",                 "setting %+");
	IConsole::AliasRegister("set",                   "setting %+");
	IConsole::AliasRegister("set_newgame",           "setting_newgame %+");
	IConsole::AliasRegister("list_patches",          "list_settings %+");
	IConsole::AliasRegister("developer",             "setting developer %+");

	IConsole::CmdRegister("list_ai_libs",            ConListAILibs);
	IConsole::CmdRegister("list_ai",                 ConListAI);
	IConsole::CmdRegister("reload_ai",               ConReloadAI);
	IConsole::CmdRegister("rescan_ai",               ConRescanAI);
	IConsole::CmdRegister("start_ai",                ConStartAI);
	IConsole::CmdRegister("stop_ai",                 ConStopAI);

	IConsole::CmdRegister("list_game",               ConListGame);
	IConsole::CmdRegister("list_game_libs",          ConListGameLibs);
	IConsole::CmdRegister("rescan_game",             ConRescanGame);

	IConsole::CmdRegister("companies",               ConCompanies);
	IConsole::AliasRegister("players",               "companies");

	/* networking functions */

/* Content downloading is only available with ZLIB */
#if defined(WITH_ZLIB)
	IConsole::CmdRegister("content",                 ConContent);
#endif /* defined(WITH_ZLIB) */

	/*** Networking commands ***/
	IConsole::CmdRegister("say",                     ConSay,              ConHookNeedNetwork);
	IConsole::CmdRegister("say_company",             ConSayCompany,       ConHookNeedNetwork);
	IConsole::AliasRegister("say_player",            "say_company %+");
	IConsole::CmdRegister("say_client",              ConSayClient,        ConHookNeedNetwork);

	IConsole::CmdRegister("connect",                 ConNetworkConnect,   ConHookClientOnly);
	IConsole::CmdRegister("clients",                 ConNetworkClients,   ConHookNeedNetwork);
	IConsole::CmdRegister("status",                  ConStatus,           ConHookServerOnly);
	IConsole::CmdRegister("server_info",             ConServerInfo,       ConHookServerOnly);
	IConsole::AliasRegister("info",                  "server_info");
	IConsole::CmdRegister("reconnect",               ConNetworkReconnect, ConHookClientOnly);
	IConsole::CmdRegister("rcon",                    ConRcon,             ConHookNeedNetwork);

	IConsole::CmdRegister("join",                    ConJoinCompany,      ConHookNeedNonDedicatedNetwork);
	IConsole::AliasRegister("spectate",              "join 255");
	IConsole::CmdRegister("move",                    ConMoveClient,       ConHookServerOnly);
	IConsole::CmdRegister("reset_company",           ConResetCompany,     ConHookServerOnly);
	IConsole::AliasRegister("clean_company",         "reset_company %A");
	IConsole::CmdRegister("client_name",             ConClientNickChange, ConHookServerOnly);
	IConsole::CmdRegister("kick",                    ConKick,             ConHookServerOnly);
	IConsole::CmdRegister("ban",                     ConBan,              ConHookServerOnly);
	IConsole::CmdRegister("unban",                   ConUnBan,            ConHookServerOnly);
	IConsole::CmdRegister("banlist",                 ConBanList,          ConHookServerOnly);

	IConsole::CmdRegister("pause",                   ConPauseGame,        ConHookServerOrNoNetwork);
	IConsole::CmdRegister("unpause",                 ConUnpauseGame,      ConHookServerOrNoNetwork);

	IConsole::CmdRegister("authorized_key", ConNetworkAuthorizedKey, ConHookServerOnly);
	IConsole::AliasRegister("ak", "authorized_key %+");

	IConsole::AliasRegister("net_frame_freq",        "setting frame_freq %+");
	IConsole::AliasRegister("net_sync_freq",         "setting sync_freq %+");
	IConsole::AliasRegister("server_pw",             "setting server_password %+");
	IConsole::AliasRegister("server_password",       "setting server_password %+");
	IConsole::AliasRegister("rcon_pw",               "setting rcon_password %+");
	IConsole::AliasRegister("rcon_password",         "setting rcon_password %+");
	IConsole::AliasRegister("name",                  "setting client_name %+");
	IConsole::AliasRegister("server_name",           "setting server_name %+");
	IConsole::AliasRegister("server_port",           "setting server_port %+");
	IConsole::AliasRegister("max_clients",           "setting max_clients %+");
	IConsole::AliasRegister("max_companies",         "setting max_companies %+");
	IConsole::AliasRegister("max_join_time",         "setting max_join_time %+");
	IConsole::AliasRegister("pause_on_join",         "setting pause_on_join %+");
	IConsole::AliasRegister("autoclean_companies",   "setting autoclean_companies %+");
	IConsole::AliasRegister("autoclean_protected",   "setting autoclean_protected %+");
	IConsole::AliasRegister("restart_game_year",     "setting restart_game_year %+");
	IConsole::AliasRegister("min_players",           "setting min_active_clients %+");
	IConsole::AliasRegister("reload_cfg",            "setting reload_cfg %+");

	/* debugging stuff */
#ifdef _DEBUG
	IConsoleDebugLibRegister();
#endif
	IConsole::CmdRegister("fps",                     ConFramerate);
	IConsole::CmdRegister("fps_wnd",                 ConFramerateWindow);

	/* NewGRF development stuff */
	IConsole::CmdRegister("reload_newgrfs",          ConNewGRFReload,     ConHookNewGRFDeveloperTool);
	IConsole::CmdRegister("newgrf_profile",          ConNewGRFProfile,    ConHookNewGRFDeveloperTool);

	IConsole::CmdRegister("dump_info",               ConDumpInfo);

	IConsole::CmdRegister("miluju",                  ConIndustryHealth);
	IConsole::CmdRegister("testletadlo",             ConTestBuildAircraft);
	IConsole::CmdRegister("testlod",                 ConTestBuildShip);
	IConsole::CmdRegister("testprejezd",             ConTestLevelCrossing);
	IConsole::CmdRegister("testnalet",               ConTestAirRaid);
	IConsole::CmdRegister("testzamerit",             ConTestAimCrosshair);
	IConsole::CmdRegister("teststavby",              ConTestIndustryHealth);
	IConsole::CmdRegister("testmesta",               ConTestTowns);
	IConsole::CmdRegister("testdomy",                ConTestHouseSets);
	IConsole::CmdRegister("testsnih",                ConTestSnow);
	IConsole::CmdRegister("testsmerdepo",            ConTestDepartureButtons);
	IConsole::CmdRegister("testnaklady",             ConTestCargoTypes);
	IConsole::CmdRegister("testprumysl",             ConTestClimateIndustries);
	IConsole::CmdRegister("testikony",               ConTestIconSizes);
	IConsole::CmdRegister("testdym",                 ConTestSmoke);
	IConsole::CmdRegister("testnoviny",              ConTestNews);
	IConsole::CmdRegister("vlak123",                 ConShowTrainOrientation);
	IConsole::CmdRegister("legacyimport",            ConLegacyDecoupleImport);
	IConsole::CmdRegister("testspoj",                ConTestCouple);
	IConsole::CmdRegister("testnaklad",              ConTestCargoScene);
	IConsole::CmdRegister("teststanice",             ConTestStationCargo);
	IConsole::CmdRegister("testpauza",               ConTestUnpause);
	IConsole::CmdRegister("testfiltr",               ConTestCoupleFilter);
	IConsole::CmdRegister("teststav",                ConTestCoupleState);
	IConsole::CmdRegister("testvse",                 ConTestEverything);
	IConsole::CmdRegister("testpodminka",            ConTestConditionalOrder);
	IConsole::CmdRegister("testrozkazy",             ConTestOrders);
	IConsole::CmdRegister("testmapa",                ConTestMap);
	IConsole::CmdRegister("testodtah",               ConTestRescue);
	IConsole::CmdRegister("testprodat",              ConTestSell);
	IConsole::CmdRegister("testikonaprodat",         ConTestSellIcon);
	IConsole::CmdRegister("testoknorozkazu",         ConTestOrderWindowGrows);
	IConsole::CmdRegister("testprestavba",           ConTestRefitButton);
	IConsole::CmdRegister("testrezim",               ConTestCoupleMode);
	IConsole::CmdRegister("testvrak",                ConTestWreck);
	IConsole::CmdRegister("testsmazat",              ConTestClearWreck);
	IConsole::CmdRegister("testjed",                 ConTestGoRound);
	IConsole::CmdRegister("testporuchana",           ConTestBreakdownAt);
	IConsole::CmdRegister("testnapis",               ConTestNapis);
	IConsole::CmdRegister("testautovlak",            ConTestRoadOnRail);
	IConsole::CmdRegister("testautoletadlo",         ConTestRoadOnAir);
	IConsole::CmdRegister("testautolod",             ConTestRoadOnWater);
	IConsole::CmdRegister("testrozkazokna",          ConTestOrderWindows);
	IConsole::CmdRegister("testauta",                ConTestRoadOrders);
	IConsole::CmdRegister("log",                     ConAnomalyLog);
	IConsole::CmdRegister("testdepo",                ConTestRescueDepot);
	IConsole::CmdRegister("testokruh",               ConTestRescueLoop);
	IConsole::CmdRegister("testrez",                 ConTestReservations);
	IConsole::CmdRegister("testnavesti",             ConTestSignals);
	IConsole::CmdRegister("testdelka",               ConTestTrainLength);
	IConsole::CmdRegister("testnavest",              ConTestSetSignal);
	IConsole::CmdRegister("testtunel",               ConTestTunnelSignal);
	IConsole::CmdRegister("vlaksav",                 ConSaveConsoleLog);
	IConsole::CmdRegister("testza",                  ConTestAfter);
	IConsole::CmdRegister("testzatik",               ConTestAfterTicks);
	IConsole::CmdRegister("testefekty",              ConTestCountEffects);
	IConsole::CmdRegister("testmodely",              ConTestListEngineModels);
	IConsole::CmdRegister("testskip",                ConTestSkipOrder);
	IConsole::CmdRegister("testbrzda",               ConTestToggleBrake);
	IConsole::CmdRegister("testdodepa",              ConTestSendToDepot);
	IConsole::CmdRegister("testnaauta",              ConTestFitForRoadVehicles);
	IConsole::CmdRegister("testcelyvlak",            ConTestDecoupleWhole);
	IConsole::CmdRegister("testprodatvagonky",        ConTestSellDecoupled);
	IConsole::CmdRegister("testkoupit",              ConTestBuyWagons);
	IConsole::CmdRegister("testzelenest",            ConTestGreenSt);
	IConsole::CmdRegister("testjmena",               ConTestNames);
	IConsole::CmdRegister("testdelky",               ConTestLengths);
	IConsole::CmdRegister("testdepovagony",          ConTestDepotWagons);
	IConsole::CmdRegister("testspolehlivost",        ConTestReliability);
	IConsole::CmdRegister("testdepofiltr",           ConTestExplainDepot);
	IConsole::CmdRegister("testvarovani",            ConTestLossWarning);
	IConsole::CmdRegister("testtypfiltr",            ConTestTypeFilter);
	IConsole::CmdRegister("testpocet",               ConTestCoupleCount);
	IConsole::CmdRegister("testvybervagonu",         ConTestPickWagon);
	IConsole::CmdRegister("testzalozit",             ConTestFoundRake);
	IConsole::CmdRegister("testhoukat",              ConTestHonk);
	IConsole::CmdRegister("testauto",                ConTestAutoDeparture);
	IConsole::CmdRegister("testmof",                 ConTestModifyOrder);
	IConsole::CmdRegister("mousedebug",              ConMouseDebug);
	IConsole::CmdRegister("testminimalne",           ConTestCoupleMin);
	IConsole::CmdRegister("testmaximalne",           ConTestCoupleMax);
	IConsole::CmdRegister("testokno",                ConTestOpenWindow);
	IConsole::CmdRegister("testokna",                ConTestRefreshWindows);
	IConsole::CmdRegister("testcil",                 ConTestRakeCargoDest);
	IConsole::CmdRegister("testodvoz",               ConTestRequestTow);
	IConsole::CmdRegister("testrada",                ConTestRakeWait);
	IConsole::CmdRegister("testsleduj",              ConTestFollow);
	IConsole::CmdRegister("testkde",                 ConTestWhere);
	IConsole::CmdRegister("testpostav",              ConTestBuildEngine);
	IConsole::CmdRegister("testprojet",              ConTestForceProceed);
	IConsole::CmdRegister("testporucha",             ConTestBreakdown);
	IConsole::CmdRegister("testodtahovka",           ConTestMakeRescueEngine);
	IConsole::CmdRegister("testvozy",                ConTestListUnits);
	IConsole::CmdRegister("testtvar",                ConTestWagonShape);
	IConsole::CmdRegister("testvagonky",             ConTestWagonLengths);
	IConsole::CmdRegister("testnalozit",             ConTestFillRoadVehicle);
	IConsole::CmdRegister("testnatoceni",            ConTestFacings);
	IConsole::CmdRegister("testkresba",              ConTestDrawing);
	IConsole::CmdRegister("testclanky",              ConTestMakePieces);
	IConsole::CmdRegister("testzrcadlo",             ConTestMirrorDrawing);
	IConsole::CmdRegister("pozn",                    ConNote);
	IConsole::CmdRegister("testpaluba",              ConTestDeckHeight);
	IConsole::CmdRegister("testkotva",               ConTestSpriteAnchor);
	/* The same three under short names. They are typed by hand, with the mouse,
	 * on the game's own keyboard, over and over while something is lined up by
	 * eye -- so the names are as short as they can be without meaning anything
	 * else. The long ones stay for whatever is written down already. */
	IConsole::CmdRegister("pal",                     ConTestDeckHeight);
	IConsole::CmdRegister("bok",                     ConTestSideTrim);
	IConsole::CmdRegister("kot",                     ConTestSpriteAnchor);
	IConsole::CmdRegister("testbok",                 ConTestSideTrim);
	IConsole::CmdRegister("testsmery",               ConTestDirectionGaps);
	IConsole::CmdRegister("testobraz",               ConTestSpriteOffsets);
	IConsole::CmdRegister("testzbourat",             ConTestDemolishDepot);
	IConsole::CmdRegister("testzrus",                ConTestScrapRakesInDepot);
	IConsole::CmdRegister("testvagony",              ConTestStoreRake);
	IConsole::CmdRegister("testgrf",                  ConTestSavegameGrfs);
	IConsole::CmdRegister("testotoc",                ConTestReverse);
	IConsole::CmdRegister("testcouva",               ConTestDrivingBackwards);
	IConsole::CmdRegister("testnedobrzdil",          ConTestOverrun);
	IConsole::CmdRegister("testpreklop",             ConTestFlipInDepot);
	IConsole::CmdRegister("testpresun",              ConTestMoveInDepot);
	IConsole::CmdRegister("testmezera",              ConTestTearConsist);
	IConsole::CmdRegister("teststartdepo",           ConTestStartDepot);
	IConsole::CmdRegister("testklon",                ConTestClone);
	IConsole::CmdRegister("testpustklon",            ConTestReleaseClone);
	IConsole::CmdRegister("cztr_test",               ConCztrTest);
}
