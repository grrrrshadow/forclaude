/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file console_cmds.cpp Implementation of the console hooks. */

#include "stdafx.h"
#include "train.h"
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
#include "settings_func.h"
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
#include "station_cmd.h"
#include "order_cmd.h"
#include "timetable_cmd.h"
#include "order_base.h"
#include "depot_map.h"
#include "station_map.h"
#include "newgrf_station.h"
#include "train_cmd.h"
#include "gamelog.h"
#include "ai/ai.hpp"
#include "ai/ai_config.hpp"
#include "newgrf.h"
#include "newgrf_profiling.h"
#include "console_func.h"
#include "engine_base.h"
#include "road.h"
#include "rail.h"
#include "game/game.hpp"
#include "3rdparty/fmt/chrono.h"
#include "company_cmd.h"
#include "misc_cmd.h"
#include "rail_cmd.h"
#include "landscape_cmd.h"
#include "vehicle_cmd.h"
#include "newgrf_engine.h"
#include "tile_map.h"
#include "core/backup_type.hpp"

#if defined(WITH_ZLIB)
#include "network/network_content.h"
#endif /* WITH_ZLIB */

#include "table/strings.h"

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
	uint want_n = 0;
	for (size_t i = 1; i < argv.size(); i++) {
		if (argv[i] == "couvej") backing = true;
		if (argv[i] == "depo") depot_mode = true;
		if (argv[i] == "rad") timetabled = true;
		if (argv[i] == "blok") blocked = true;
		if (argv[i] == "vlek") tow_mode = true;
		if (argv[i] == "pocet") counted = true;
		if (argv[i] == "stoji") parked = true;
		if (argv[i] == "oboji") swap_mode = true;
		if (argv[i] == "sklad") store_mode = true;
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
	for (const Engine *e : Engine::IterateType(VehicleType::Train)) {
		if (!e->company_avail.Test(_local_company)) continue;
		if (!RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)) continue;
		if (RailVehInfo(e->index)->railveh_type == RailVehicleType::Wagon) {
			if (eid_wagon == EngineID::Invalid()) {
				eid_wagon = e->index;
			} else if (eid_wagon2 == EngineID::Invalid()) {
				eid_wagon2 = e->index;
			}
		} else {
			if (eid_loco == EngineID::Invalid()) eid_loco = e->index;
		}
		if (eid_loco != EngineID::Invalid() && eid_wagon2 != EngineID::Invalid()) break;
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
		drop_b.SetDecoupleCount(1);
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
	for (int i = 0; i < 3; i++) {
		auto [costw, wid, unused_d, unused_e, unused_f] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, depot_e, eid_wagon, true, INVALID_CARGO, ClientID::Invalid);
		if (costw.Failed() || Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, wid, Train::Get(veh2)->Last()->index, false).Failed()) {
			IConsolePrint(CC_ERROR, "testspoj: wagon failed.");
			return true;
		}
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
		deliver.SetDecoupleCount(1);
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
		deliver.SetDecoupleCount(1);
		Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh2, 0, deliver);
	} else {
		deliver.MakeGoToStation(st_id);
		if (blocked) {
			deliver.SetLoadType(OrderLoadType::FullLoadAny);
			deliver.SetUnloadType(OrderUnloadType::NoUnload);
			deliver.SetDecoupleCount(2);
			IConsolePrint(CC_DEFAULT, "testspoj: odkladacka si necha vagon na plnou nakladku - z nastupiste neodjede.");
		} else {
			deliver.SetLoadType(OrderLoadType::NoLoad);
			deliver.SetUnloadType(OrderUnloadType::NoUnload);
			deliver.SetDecoupleCount(1);
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
	auto [cost1, veh1, unused_g, unused_h, unused_i] = Command<Commands::BuildVehicle>::Do(DoCommandFlag::Execute, parked ? depot_w : depot_e, eid_loco, true, INVALID_CARGO, ClientID::Invalid);
	if (cost1.Failed()) {
		IConsolePrint(CC_ERROR, "testspoj: engine 1 failed.");
		return true;
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
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh2, false);
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh1, false);
	if (veh3 != VehicleID::Invalid()) Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh3, false);

	_testspoj_active = true;

	IConsolePrint(CC_DEFAULT, "testspoj: scene ready. deliverer=vlak {}, collector=vlak {}, station={} at ({}..{},{}), west depot ({},{}), east depot ({},{}).",
			Train::Get(veh2)->unitnumber, Train::Get(veh1)->unitnumber, st_id, x0 + 18, x0 + 21, y0, x0, y0, x0 + LEN - 1, y0);
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
		IConsolePrint(CC_HELP, "Change the cargo filter on a collect order. Usage: 'testfiltr' to clear it, or 'testfiltr <cargo>'.");
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
		IConsolePrint(CC_DEFAULT, "vlak {}: ({},{}) rychlost {} couva {} rozkaz {} vozu {} zasekly {} cil {} narok {} stav[{}{}{}{}]",
				t->unitnumber, TileX(t->tile), TileY(t->tile), t->cur_speed,
				t->vehicle_flags.Test(VehicleFlag::DrivingBackwards) ? "ano" : "ne",
				to_underlying(t->current_order.GetType()), CountVehiclesInChain(t),
				t->flags.Test(VehicleRailFlag::Stuck) ? "ano" : "ne",
				t->couple_target == VehicleID::Invalid() ? -1 : (int)t->couple_target.base(),
				t->couple_claim == VehicleID::Invalid() ? -1 : (int)t->couple_claim.base(),
				t->vehstatus.Test(VehState::Stopped) ? 'S' : '-',
				t->flags.Test(VehicleRailFlag::LeavingStation) ? 'L' : '-',
				t->flags.Test(VehicleRailFlag::Reversing) ? 'R' : '-',
				t->vehicle_flags.Test(VehicleFlag::LoadingFinished) ? 'F' : '-');
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
		IConsolePrint(CC_DEFAULT, "vlak {}: ({},{}) vozu {} {} rozkazu {}",
				t->unitnumber, TileX(t->tile), TileY(t->tile), CountVehiclesInChain(t),
				t->IsInDepot() ? "v depu" : (t->vehstatus.Test(VehState::Stopped) ? "stopnut" : "venku"),
				t->GetNumOrders());
		int n = 0;
		for (const Order &o : t->Orders()) {
			std::string extra;
			if (o.ShouldGoToCouple()) extra += " SPOJIT";
			if (o.ShouldWaitForCouple()) extra += " CEKAT";
			if (o.GetDecoupleCount() != 0) extra += fmt::format(" ODPOJIT:{}", o.GetDecoupleCount());
			if (o.ShouldReverseOutOfStation()) extra += " REVERZ";
			if (o.IsType(OT_GOTO_DEPOT) && o.ShouldTurnAroundInDepot()) extra += " OTOC-DEPO";
			IConsolePrint(CC_DEFAULT, "  [{}] typ {} cil {}{}", n++, to_underlying(o.GetType()), o.GetDestination().base(), extra);
		}
	}
	return true;
}

/**
 * Save the console backlog to a text file in the personal directory, so an
 * incident can be reported whole instead of screenshotting the console a
 * window at a time. Usage: vlaksav [<name>]
 * @copydoc IConsoleCmdProc
 */
static bool ConSaveConsoleLog(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Save the console backlog to a file. Usage: 'vlaksav [<name>]' (default vlaksav.txt).");
		return true;
	}
	extern std::string _personal_dir;
	std::string name = argv.size() >= 2 ? fmt::format("{}.txt", argv[1]) : "vlaksav.txt";
	std::string path = _personal_dir + name;
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
				desc = fmt::format("depo (vrata {})", to_underlying(GetRailDepotDirection(tile)));
			} else if (IsRailStationTile(tile)) {
				tracks = TrackBits{GetRailStationTrack(tile)};
				desc = fmt::format("stanice {}", GetStationIndex(tile).base());
			} else if (IsLevelCrossingTile(tile)) {
				tracks = TrackBits{GetCrossingRailTrack(tile)};
				desc = "prejezd";
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
static void DoTestClone(uint unit, uint count, bool reverz)
{
	_pause_mode = {};

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
		Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, cloned, false);
		IConsolePrint(CC_DEFAULT, "testklon: clone vlak {} started.", Train::Get(cloned)->unitnumber);
	}
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, original->index, false);
	IConsolePrint(CC_DEFAULT, "testklon: original vlak {} started{}.", original->unitnumber, reverz ? " (reverz on station orders)" : "");
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
		IConsolePrint(CC_HELP, "Usage: 'testklon <unit> <count> [reverz] [za <ticks>]'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	auto pcount = ParseInteger(argv[2]);
	if (!punit.has_value() || !pcount.has_value()) return false;
	bool reverz = false;
	int delay = 0;
	for (size_t i = 3; i < argv.size(); i++) {
		if (argv[i] == "reverz") reverz = true;
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
	DoTestClone((uint)*punit, (uint)*pcount, reverz);
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
		IConsolePrint(CC_HELP, "List reserved tiles in the test scene. Usage: 'testrez'.");
		return true;
	}
	uint n = 0;
	for (uint y = _testmapa_area[1]; y <= _testmapa_area[3]; y++) {
		for (uint x = _testmapa_area[0]; x <= _testmapa_area[2]; x++) {
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

static bool ConTestRescue(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Build the junction-rescue test scene. Usage: 'testodtah [rovina|krizeni]'.");
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
	Order to_branch;
	to_branch.MakeGoToStation(st_branch);
	to_branch.SetLoadType(OrderLoadType::NoLoad);
	to_branch.SetUnloadType(OrderUnloadType::NoUnload);
	Command<Commands::InsertOrder>::Do(DoCommandFlag::Execute, veh_c, 0, to_branch);

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

	/* Send the casualty off; the tick watcher breaks it down the moment its
	 * front turns onto the branch. */
	Command<Commands::StartStopVehicle>::Do(DoCommandFlag::Execute, veh_c, false);
	_testodtah_casualty = veh_c;
	_testodtah_break_tile = faraway ? TileXY(x0 + LEN - 6, y0) :
			((plain || oneway) ? TileXY(xj + 8, y0) : (crossing ? TileXY(xj, y0) : TileXY(xj, y0 + 1)));
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

	if (_testodtah_break_tile == INVALID_TILE) return;
	Train *t = Train::GetIfValid(_testodtah_casualty);
	if (t == nullptr) {
		_testodtah_break_tile = INVALID_TILE;
		return;
	}
	if (t->tile != _testodtah_break_tile) return;
	t->breakdown_ctr = 2;
	_testodtah_break_tile = INVALID_TILE;
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
static bool ConTestToggleBrake(std::span<std::string_view> argv)
{
	if (argv.size() != 2) {
		IConsolePrint(CC_HELP, "Toggle a train's hand brake. Usage: 'testbrzda <unit number>'.");
		return true;
	}
	auto punit = ParseInteger(argv[1]);
	if (!punit.has_value()) return false;
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
 * Turn a train round, the same as the player's reverse button.
 * Usage: testotoc <unit number>
 * @copydoc IConsoleCmdProc
 */
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
 * Turn the depot-doorway lock on the reverse button on or off.
 *
 * A train standing half in and half out of a depot has its turn-round button
 * greyed out, because asking for it there is what freezes a train. This lets
 * that button be pressed anyway, which is the only way to work on the freeze.
 * @copydoc IConsoleCmdProc
 */
static bool ConDepotDoorstepReverse(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Allow the turn-round button while a train straddles a depot doorway.");
		IConsolePrint(CC_HELP, "Usage: 'depo123' to flip it, or 'depo123 on' / 'depo123 off'.");
		return true;
	}

	if (argv.size() >= 2) {
		if (argv[1] == "on" || argv[1] == "1") {
			_allow_reverse_on_depot_doorstep = true;
		} else if (argv[1] == "off" || argv[1] == "0") {
			_allow_reverse_on_depot_doorstep = false;
		} else {
			return false;
		}
	} else {
		_allow_reverse_on_depot_doorstep = !_allow_reverse_on_depot_doorstep;
	}

	IConsolePrint(CC_DEFAULT, "Turning round on a depot doorway is now {}.",
			_allow_reverse_on_depot_doorstep ? "allowed" : "blocked");
	/* The button is drawn from this, so every open vehicle window has to be
	 * told to look again. */
	InvalidateWindowClassesData(WindowClass::VehicleView);
	return true;
}

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
		IConsolePrint(CC_DEFAULT, "Game is in error state and cannot be unpaused via console.");
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

	IConsole::CmdRegister("depo123",                 ConDepotDoorstepReverse);
	IConsole::CmdRegister("vlak123",                 ConShowTrainOrientation);
	IConsole::CmdRegister("legacyimport",            ConLegacyDecoupleImport);
	IConsole::CmdRegister("testspoj",                ConTestCouple);
	IConsole::CmdRegister("testfiltr",               ConTestCoupleFilter);
	IConsole::CmdRegister("teststav",                ConTestCoupleState);
	IConsole::CmdRegister("testrozkazy",             ConTestOrders);
	IConsole::CmdRegister("testmapa",                ConTestMap);
	IConsole::CmdRegister("testodtah",               ConTestRescue);
	IConsole::CmdRegister("testdepo",                ConTestRescueDepot);
	IConsole::CmdRegister("testokruh",               ConTestRescueLoop);
	IConsole::CmdRegister("testrez",                 ConTestReservations);
	IConsole::CmdRegister("vlaksav",                 ConSaveConsoleLog);
	IConsole::CmdRegister("testza",                  ConTestAfter);
	IConsole::CmdRegister("testskip",                ConTestSkipOrder);
	IConsole::CmdRegister("testbrzda",               ConTestToggleBrake);
	IConsole::CmdRegister("testzrus",                ConTestScrapRakesInDepot);
	IConsole::CmdRegister("testvagony",              ConTestStoreRake);
	IConsole::CmdRegister("testotoc",                ConTestReverse);
	IConsole::CmdRegister("teststartdepo",           ConTestStartDepot);
	IConsole::CmdRegister("testklon",                ConTestClone);
	IConsole::CmdRegister("cztr_test",               ConCztrTest);
}
