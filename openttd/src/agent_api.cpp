/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_api.cpp The game's interface for a program that plays it (agent_api.h). */

#include "stdafx.h"
#include "agent_api.h"
#include "network/core/os_abstraction.h"
#include "3rdparty/nlohmann/json.hpp"
#include "bridge.h"
#include "cargotype.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "console_func.h"
#include "core/backup_type.hpp"
#include "debug.h"
#include "engine_base.h"
#include "engine_func.h"
#include "fileio_func.h"
#include "industry.h"
#include "landscape.h"
#include "landscape_cmd.h"
#include "map_func.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"
#include "openttd.h"
#include "order_base.h"
#include "order_cmd.h"
#include "rail_cmd.h"
#include "rev.h"
#include "road_cmd.h"
#include "screenshot.h"
#include "settings_type.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "string_func.h"
#include "strings_func.h"
#include "terraform_cmd.h"
#include "tile_cmd.h"
#include "tile_map.h"
#include "timer/timer_game_calendar.h"
#include "town.h"
#include "tunnelbridge_cmd.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
#include "vehiclelist.h"

#include "table/strings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "safeguards.h"

/** What the console prints goes here too while a request runs its commands (IConsolePrint() of console.cpp). */
extern std::vector<std::string> *_iconsole_capture;

namespace {

/** A request's answer. */
struct Answer {
	int status = 200;
	std::string type = "application/json; charset=utf-8";
	std::string body;
};

/** What a request waits for done on the game's thread. */
struct Job {
	std::function<Answer()> run;
	std::promise<Answer> done;
};

std::mutex _api_mutex;                    ///< of the jobs
std::deque<std::shared_ptr<Job>> _api_jobs;
std::thread _api_thread;
std::atomic<bool> _api_stop = false;
std::atomic<uint16_t> _api_port = 0;
SOCKET _api_listen = INVALID_SOCKET;
bool _api_in_job = false;     ///< a request is being done on the game's thread (AgentApiPump())
bool _api_stop_after = false; ///< and asked the interface stopped: done after its answer

constexpr size_t REQUEST_MOST = 1 << 20;                 ///< bytes of a request at most
constexpr std::chrono::seconds GAME_WAIT{120};           ///< a request waits so long for the game at most
constexpr int MAP_AREA_MOST = 64 * 64;                   ///< tiles of an area asked at most

std::string JsonString(std::string_view s)
{
	std::string out = "\"";
	for (const char ch : s) {
		const unsigned char c = static_cast<unsigned char>(ch);
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20) {
					out += fmt::format("\\u{:04x}", c);
				} else {
					out += ch;
				}
		}
	}
	return out + "\"";
}

Answer Error(int status, std::string_view what)
{
	return {status, "application/json; charset=utf-8", fmt::format("{{\"ok\":false,\"error\":{}}}", JsonString(what))};
}

const std::string_view ROUTES = R"json({"ok":true,"game":"OpenTTD Decouple","routes":[
{"method":"GET","path":"/","what":"this list"},
{"method":"GET","path":"/guide","what":"how to use the interface, plain text"},
{"method":"GET","path":"/state","what":"the state of the game: date, pause, map, company, counts"},
{"method":"GET","path":"/map?x=&y=&w=&h=","what":"the tiles of an area as data (64x64 at most); w and h default to 1"},
{"method":"GET","path":"/industries","what":"every industry: where, what it makes and takes, last month's production"},
{"method":"GET","path":"/vehicles?type=","what":"every vehicle (train, road, ship, aircraft): where, speed, cargo, order, profit"},
{"method":"GET","path":"/towns","what":"every town: where, how many people"},
{"method":"GET","path":"/stations","what":"every station: where, what it has, the cargo waiting"},
{"method":"GET","path":"/engines?type=","what":"what the company can buy"},
{"method":"POST","path":"/build?what=&x=&y=...","what":"build as the company: road, rail, rail_station, road_stop, rail_depot, road_depot, dock, bridge, tunnel, clear, level"},
{"method":"POST","path":"/vehicle?do=&id=...","what":"buy, start, stop, sell, depot, order"},
{"method":"POST","path":"/console","what":"console commands, one a line of the body; answers what they printed"},
{"method":"GET","path":"/screenshot","what":"a picture of the screen as it is shown, in the game's screenshot format (PNG where the build has it)"},
{"method":"POST","path":"/mcp","what":"the same as tools of the Model Context Protocol (JSON-RPC, one message a request)"}
]})json";

const std::string_view GUIDE =
	"OpenTTD Decouple: the interface for an agent\n"
	"\n"
	"The server listens on 127.0.0.1 only and is off till it is turned on: the console's command 'api' (or 'api <port>'),\n"
	"the game started with -A (or -A <port>), or the environment variable OTTD_AGENT_API (a port, or 1). 'api off' stops\n"
	"it. Every answer but /guide and /screenshot is JSON with \"ok\".\n"
	"\n"
	"GET /state            the date, whether the game is paused, the size of the map, the local company's money,\n"
	"                      how many vehicles, towns, industries and stations there are.\n"
	"GET /map?x=&y=&w=&h=  the tiles of an area: kind, height, slope, owner and what the land info tool says of it.\n"
	"GET /industries       each industry: id, name, kind, x, y, w, h, town, \"makes\" (cargo, last month's production and\n"
	"                      how much of it was carried away, percent) and \"takes\" (cargo).\n"
	"GET /vehicles?type=   each vehicle: id, unit, type, name, x, y (tile), speed, max_speed, stopped, in_depot, crashed,\n"
	"                      cargo (name, count, capacity), order (kind, destination), profits, age, parts.\n"
	"GET /towns            each town: id, name, x, y, population, houses.\n"
	"GET /stations         each station: id, name, x, y, owner, what it has (rail, truck, bus, airport, dock), waiting cargo.\n"
	"GET /engines?type=    what the company can buy now: id, name, type, capacity and cargo, max_speed.\n"
	"\n"
	"POST /build           builds as the local company; the parameters go in the query. \"what\" is one of:\n"
	"    road         x, y, x2, y2 (a straight line along x or y)              [roadtype]\n"
	"    rail         x, y, x2, y2 (a straight line along x or y)              [railtype: 0 rail, 4 power line, 5 pipeline]\n"
	"    rail_station x, y, axis (x or y), tracks, length                      [railtype]\n"
	"    road_stop    x, y, dir (0 NE, 1 SE, 2 SW, 3 NW), truck (0, 1), through (0, 1) [roadtype]\n"
	"    rail_depot   x, y, dir                                                 [railtype]\n"
	"    road_depot   x, y, dir                                                 [roadtype]\n"
	"    dock         x, y (a sloped coast tile)\n"
	"    bridge       x, y, x2, y2, transport (road, rail)                      [bridge, railtype, roadtype]\n"
	"    tunnel       x, y (the tile of its mouth), transport (road, rail)      [railtype, roadtype]\n"
	"    clear        x, y [x2, y2]\n"
	"    level        x, y, x2, y2, mode (level, raise, lower)\n"
	"  The answer: \"ok\", \"cost\", and \"error\" with the game's own words when it could not be built.\n"
	"\n"
	"POST /vehicle         \"do\" is one of:\n"
	"    buy    x, y (a depot's tile), engine (an id of /engines)  -> \"id\" of the new vehicle\n"
	"    start, stop, sell, depot (send it to a depot)   id\n"
	"    order  id, x, y (a tile of a station: go there, appended to its orders)\n"
	"\n"
	"POST /console         the body is console commands, one a line. The answer has \"lines\": what they printed.\n"
	"                      All the game's console commands work: 'help', 'list_cmds', 'pause', 'unpause', 'setting',\n"
	"                      'save', 'load', and the test rig's 'test3d ...' and 'testlod ...'.\n"
	"GET /screenshot       a picture of the screen as it is shown.\n"
	"POST /mcp             the Model Context Protocol over HTTP: initialize, tools/list, tools/call. The tools are the\n"
	"                      paths above by their names (state, map, industries, vehicles, towns, stations, engines, build,\n"
	"                      vehicle, console, screenshot, guide), their parameters the tool's arguments.\n"
	"\n"
	"Tiles are x, y from the map's north corner; a tile's index is y * (the map's width) + x.\n"
	"Requests are done between the frames, the game standing meanwhile, one at a time, in the order they came.\n";

std::string_view TileKindName(TileType type)
{
	static const std::string_view names[] = {"clear", "rail", "road", "house", "trees", "station", "water", "void", "industry", "tunnelbridge", "object"};
	const size_t k = to_underlying(type);
	return k < std::size(names) ? names[k] : "unknown";
}

Answer StateAnswer()
{
	std::string s = "{\"ok\":true";
	const std::string_view modes[] = {"menu", "normal", "editor", "bootstrap"};
	const size_t mode = to_underlying(_game_mode);
	s += fmt::format(",\"version\":{},\"mode\":{}", JsonString(_openttd_revision), JsonString(mode < std::size(modes) ? modes[mode] : "unknown"));
	s += fmt::format(",\"paused\":{}", _pause_mode.Any() ? "true" : "false");
	if (_game_mode == GameMode::Normal || _game_mode == GameMode::Editor) {
		const auto ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
		s += fmt::format(",\"date\":\"{:04}-{:02}-{:02}\"", ymd.year.base(), ymd.month + 1, ymd.day);
		s += fmt::format(",\"map\":{{\"width\":{},\"height\":{}}}", Map::SizeX(), Map::SizeY());
		s += fmt::format(",\"landscape\":{}", to_underlying(_settings_game.game_creation.landscape));
		if (const Company *c = Company::GetIfValid(_local_company); c != nullptr) {
			s += fmt::format(",\"company\":{{\"id\":{},\"money\":{},\"loan\":{}}}", c->index.base(), static_cast<int64_t>(c->money), static_cast<int64_t>(c->current_loan));
		} else {
			s += ",\"company\":null";
		}
		int vehicles[4] = {0, 0, 0, 0};
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (!v->IsPrimaryVehicle()) continue;
			const size_t k = to_underlying(v->type);
			if (k < 4) vehicles[k]++;
		}
		s += fmt::format(",\"vehicles\":{{\"train\":{},\"road\":{},\"ship\":{},\"aircraft\":{}}}", vehicles[0], vehicles[1], vehicles[2], vehicles[3]);
		s += fmt::format(",\"towns\":{},\"industries\":{},\"stations\":{}", Town::GetNumItems(), Industry::GetNumItems(), Station::GetNumItems());
	}
	return {200, "application/json; charset=utf-8", s + "}"};
}

Answer MapAnswer(const std::map<std::string, std::string> &query)
{
	if (_game_mode != GameMode::Normal && _game_mode != GameMode::Editor) return Error(409, "no game is running");
	auto number = [&query](const char *key, int otherwise) {
		auto it = query.find(key);
		int value = otherwise;
		if (it != query.end() && !it->second.empty()) std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
		return value;
	};
	const int x = number("x", -1), y = number("y", -1), w = number("w", 1), h = number("h", 1);
	if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > static_cast<int>(Map::SizeX()) || y + h > static_cast<int>(Map::SizeY())) return Error(400, "x, y, w, h outside the map");
	if (w * h > MAP_AREA_MOST) return Error(400, "more than 64x64 tiles asked");
	std::string s = fmt::format("{{\"ok\":true,\"x\":{},\"y\":{},\"w\":{},\"h\":{},\"tiles\":[", x, y, w, h);
	bool first = true;
	for (int ty = y; ty < y + h; ty++) {
		for (int tx = x; tx < x + w; tx++) {
			const TileIndex tile = TileXY(tx, ty);
			TileDesc td{};
			GetTileDesc(tile, td);
			const std::string what = td.name.has_value() ? *td.name : GetString(td.str, td.dparam);
			const Owner owner = IsTileType(tile, TileType::Void) || IsTileType(tile, TileType::House) || IsTileType(tile, TileType::Industry) ? INVALID_OWNER : GetTileOwner(tile);
			s += fmt::format("{}{{\"x\":{},\"y\":{},\"index\":{},\"kind\":{},\"z\":{},\"slope\":{},\"owner\":{},\"what\":{}}}", first ? "" : ",", tx, ty, tile.base(),
					JsonString(TileKindName(GetTileType(tile))), GetTileZ(tile), to_underlying(GetTileSlope(tile)), owner == INVALID_OWNER ? -1 : static_cast<int>(owner.base()), JsonString(what));
			first = false;
		}
	}
	return {200, "application/json; charset=utf-8", s + "]}"};
}

Answer ConsoleAnswer(const std::string &body)
{
	std::vector<std::string> lines;
	int commands = 0;
	{
		_iconsole_capture = &lines;
		size_t at = 0;
		while (at <= body.size()) {
			size_t end = body.find('\n', at);
			if (end == std::string::npos) end = body.size();
			std::string_view line(body.data() + at, end - at);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
			if (!line.empty()) {
				IConsoleCmdExec(line);
				commands++;
			}
			at = end + 1;
		}
		_iconsole_capture = nullptr;
	}
	std::string s = fmt::format("{{\"ok\":true,\"commands\":{},\"lines\":[", commands);
	for (size_t k = 0; k < lines.size(); k++) s += (k == 0 ? "" : ",") + JsonString(lines[k]);
	return {200, "application/json; charset=utf-8", s + "]}"};
}

using Params = std::map<std::string, std::string>;
using json = nlohmann::json;

Answer JsonAnswer(const json &j, int status = 200)
{
	return {status, "application/json; charset=utf-8", j.dump()};
}

int ParamInt(const Params &p, const char *key, int otherwise)
{
	auto it = p.find(key);
	int value = otherwise;
	if (it != p.end() && !it->second.empty()) std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
	return value;
}

std::string ParamText(const Params &p, const char *key, std::string_view otherwise = {})
{
	auto it = p.find(key);
	return it == p.end() ? std::string(otherwise) : it->second;
}

/** The tile of two parameters, INVALID_TILE when they are not on the map. */
TileIndex ParamTile(const Params &p, const char *kx, const char *ky)
{
	const int x = ParamInt(p, kx, -1), y = ParamInt(p, ky, -1);
	if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) return INVALID_TILE;
	return TileXY(x, y);
}

bool GameRuns()
{
	return _game_mode == GameMode::Normal || _game_mode == GameMode::Editor;
}

std::string CargoName(CargoType cargo)
{
	return IsValidCargoType(cargo) ? GetString(CargoSpec::Get(cargo)->name) : std::string();
}

std::string_view VehicleKindName(VehicleType type)
{
	static const std::string_view names[] = {"train", "road", "ship", "aircraft"};
	const size_t k = to_underlying(type);
	return k < std::size(names) ? names[k] : "other";
}

/** The kind of vehicle a parameter names; VehicleType::End: any. */
VehicleType ParamVehicleKind(const Params &p)
{
	const std::string type = ParamText(p, "type");
	for (VehicleType t : {VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft}) {
		if (type == VehicleKindName(t)) return t;
	}
	return VehicleType::End;
}

Answer IndustriesAnswer()
{
	if (!GameRuns()) return Error(409, "no game is running");
	json list = json::array();
	for (const Industry *i : Industry::Iterate()) {
		json one = {{"id", i->index.base()}, {"name", GetString(STR_INDUSTRY_NAME, i->index)}, {"kind", GetString(GetIndustrySpec(i->type)->name)},
				{"x", TileX(i->location.tile)}, {"y", TileY(i->location.tile)}, {"w", i->location.w}, {"h", i->location.h}};
		if (i->town != nullptr) one["town"] = GetString(STR_TOWN_NAME, i->town->index);
		json makes = json::array(), takes = json::array();
		for (const auto &pc : i->produced) {
			if (!IsValidCargoType(pc.cargo)) continue;
			makes.push_back({{"cargo", CargoName(pc.cargo)}, {"last_month", pc.history[LAST_MONTH].production}, {"carried_percent", ToPercent8(pc.history[LAST_MONTH].PctTransported())}});
		}
		for (const auto &ac : i->accepted) {
			if (IsValidCargoType(ac.cargo)) takes.push_back(CargoName(ac.cargo));
		}
		one["makes"] = std::move(makes);
		one["takes"] = std::move(takes);
		list.push_back(std::move(one));
	}
	return JsonAnswer({{"ok", true}, {"industries", std::move(list)}});
}

Answer VehiclesAnswer(const Params &p)
{
	if (!GameRuns()) return Error(409, "no game is running");
	const VehicleType only = ParamVehicleKind(p);
	static const std::map<OrderType, std::string_view> order_names = {{OT_NOTHING, "none"}, {OT_GOTO_STATION, "go to station"}, {OT_GOTO_DEPOT, "go to depot"},
			{OT_LOADING, "loading"}, {OT_LEAVESTATION, "leaving station"}, {OT_DUMMY, "none"}, {OT_GOTO_WAYPOINT, "go to waypoint"}, {OT_CONDITIONAL, "conditional"}};
	json list = json::array();
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || (only != VehicleType::End && v->type != only)) continue;
		int parts = 0;
		std::map<CargoType, std::pair<uint, uint>> cargo; // carried, capacity
		for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
			parts++;
			if (u->cargo_cap == 0 || !IsValidCargoType(u->cargo_type)) continue;
			cargo[u->cargo_type].first += u->cargo.StoredCount();
			cargo[u->cargo_type].second += u->cargo_cap;
		}
		json carried = json::array();
		for (const auto &[type, n] : cargo) carried.push_back({{"cargo", CargoName(type)}, {"count", n.first}, {"capacity", n.second}});
		const TileIndex at = TileVirtXY(v->x_pos, v->y_pos);
		json one = {{"id", v->index.base()}, {"unit", v->unitnumber}, {"type", VehicleKindName(v->type)}, {"name", GetString(STR_VEHICLE_NAME, v->index)},
				{"owner", v->owner.base()}, {"x", TileX(at)}, {"y", TileY(at)}, {"speed", v->GetDisplaySpeed()}, {"max_speed", v->GetDisplayMaxSpeed()},
				{"stopped", v->vehstatus.Test(VehState::Stopped)}, {"in_depot", v->IsStoppedInDepot()}, {"crashed", v->vehstatus.Test(VehState::Crashed)},
				{"parts", parts}, {"cargo", std::move(carried)}, {"orders", v->GetNumOrders()},
				{"profit_this_year", static_cast<int64_t>(v->GetDisplayProfitThisYear())}, {"profit_last_year", static_cast<int64_t>(v->GetDisplayProfitLastYear())},
				{"age_days", v->age.base()}};
		const auto name = order_names.find(v->current_order.GetType());
		json order = {{"kind", name != order_names.end() ? name->second : "other"}};
		if (v->current_order.IsType(OT_GOTO_STATION) || v->current_order.IsType(OT_LOADING)) {
			if (const Station *st = Station::GetIfValid(v->current_order.GetDestination().ToStationID()); st != nullptr) order["station"] = GetString(STR_STATION_NAME, st->index);
		}
		if (v->dest_tile != INVALID_TILE && IsValidTile(v->dest_tile)) {
			order["x"] = TileX(v->dest_tile);
			order["y"] = TileY(v->dest_tile);
		}
		one["order"] = std::move(order);
		list.push_back(std::move(one));
	}
	return JsonAnswer({{"ok", true}, {"vehicles", std::move(list)}});
}

Answer TownsAnswer()
{
	if (!GameRuns()) return Error(409, "no game is running");
	json list = json::array();
	for (const Town *t : Town::Iterate()) {
		list.push_back({{"id", t->index.base()}, {"name", GetString(STR_TOWN_NAME, t->index)}, {"x", TileX(t->xy)}, {"y", TileY(t->xy)},
				{"population", t->cache.population}, {"houses", t->cache.num_houses}});
	}
	return JsonAnswer({{"ok", true}, {"towns", std::move(list)}});
}

Answer StationsAnswer()
{
	if (!GameRuns()) return Error(409, "no game is running");
	json list = json::array();
	for (const Station *st : Station::Iterate()) {
		json has = json::array();
		if (st->facilities.Test(StationFacility::Train)) has.push_back("rail");
		if (st->facilities.Test(StationFacility::TruckStop)) has.push_back("truck");
		if (st->facilities.Test(StationFacility::BusStop)) has.push_back("bus");
		if (st->facilities.Test(StationFacility::Airport)) has.push_back("airport");
		if (st->facilities.Test(StationFacility::Dock)) has.push_back("dock");
		json waiting = json::array();
		for (const CargoSpec *cs : CargoSpec::Iterate()) {
			const CargoType c = cs->Index();
			const uint n = st->goods[c].HasData() ? st->goods[c].GetData().cargo.TotalCount() : 0;
			if (n > 0) waiting.push_back({{"cargo", CargoName(c)}, {"count", n}});
		}
		list.push_back({{"id", st->index.base()}, {"name", GetString(STR_STATION_NAME, st->index)}, {"x", TileX(st->xy)}, {"y", TileY(st->xy)},
				{"owner", st->owner.base()}, {"has", std::move(has)}, {"waiting", std::move(waiting)}});
	}
	return JsonAnswer({{"ok", true}, {"stations", std::move(list)}});
}

Answer EnginesAnswer(const Params &p)
{
	if (!GameRuns()) return Error(409, "no game is running");
	if (Company::GetIfValid(_local_company) == nullptr) return Error(409, "there is no company to buy for");
	const VehicleType only = ParamVehicleKind(p);
	json list = json::array();
	for (const Engine *e : Engine::Iterate()) {
		if ((only != VehicleType::End && e->type != only) || !IsEngineBuildable(e->index, e->type, _local_company)) continue;
		json one = {{"id", e->index.base()}, {"name", GetString(STR_ENGINE_NAME, e->index)}, {"type", VehicleKindName(e->type)}, {"max_speed", e->GetDisplayMaxSpeed()}};
		const CargoType cargo = e->GetDefaultCargoType();
		if (IsValidCargoType(cargo)) {
			one["cargo"] = CargoName(cargo);
			one["capacity"] = e->GetDisplayDefaultCapacity();
		}
		if (e->type == VehicleType::Train) {
			one["wagon"] = e->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Wagon;
			if (const auto rt = e->VehInfo<RailVehicleInfo>().railtypes.GetNthSetBit(0); rt.has_value()) one["railtype"] = to_underlying(*rt);
		}
		list.push_back(std::move(one));
	}
	return JsonAnswer({{"ok", true}, {"engines", std::move(list)}});
}

/** What a command of the game came to, as an answer. */
Answer CommandAnswer(const CommandCost &cost, json more = json::object())
{
	more["ok"] = cost.Succeeded();
	if (cost.Succeeded()) {
		more["cost"] = static_cast<int64_t>(cost.GetCost());
	} else {
		const StringID why = cost.GetErrorMessage();
		more["error"] = why == INVALID_STRING_ID ? std::string("the game refused it") : GetString(why);
	}
	return JsonAnswer(more, cost.Succeeded() ? 200 : 409);
}

DiagDirection ParamDir(const Params &p)
{
	return static_cast<DiagDirection>(ParamInt(p, "dir", 0) & 3);
}

Answer BuildAnswer(const Params &p)
{
	if (_game_mode != GameMode::Normal) return Error(409, "no game is running");
	if (Company::GetIfValid(_local_company) == nullptr) return Error(409, "there is no company to build for");
	const std::string what = ParamText(p, "what");
	const TileIndex a = ParamTile(p, "x", "y");
	if (a == INVALID_TILE) return Error(400, "x, y are not on the map");
	const TileIndex b = p.contains("x2") || p.contains("y2") ? ParamTile(p, "x2", "y2") : a;
	if (b == INVALID_TILE) return Error(400, "x2, y2 are not on the map");
	const RailType railtype = static_cast<RailType>(Clamp(ParamInt(p, "railtype", 0), 0, RAILTYPE_END - 1));
	const RoadType roadtype = static_cast<RoadType>(Clamp(ParamInt(p, "roadtype", 0), 0, ROADTYPE_END - 1));
	const bool along_x = TileY(a) == TileY(b);
	const bool straight = along_x || TileX(a) == TileX(b);
	AutoRestoreBackup company(_current_company, _local_company);
	const DoCommandFlags flags{DoCommandFlag::Execute};

	if (what == "road") {
		if (!straight) return Error(400, "a road goes straight along x or y: x == x2 or y == y2");
		const TileIndex from = std::min(a, b), to = std::max(a, b);
		return CommandAnswer(Command<Commands::BuildRoadLong>::Do(flags, to, from, roadtype, along_x ? Axis::X : Axis::Y, DisallowedRoadDirections{}, false, true, false));
	}
	if (what == "rail") {
		if (!straight) return Error(400, "a track goes straight along x or y: x == x2 or y == y2");
		return CommandAnswer(Command<Commands::BuildRailLong>::Do(flags, b, a, railtype, along_x ? Track::X : Track::Y, false, false));
	}
	if (what == "rail_station") {
		const Axis axis = ParamText(p, "axis", "x") == "y" ? Axis::Y : Axis::X;
		return CommandAnswer(Command<Commands::BuildRailStation>::Do(flags, a, railtype, axis, static_cast<uint8_t>(Clamp(ParamInt(p, "tracks", 1), 1, 16)),
				static_cast<uint8_t>(Clamp(ParamInt(p, "length", 1), 1, 16)), STAT_CLASS_DFLT, 0, StationID::Invalid(), false));
	}
	if (what == "road_stop") {
		const bool through = ParamInt(p, "through", 0) != 0;
		return CommandAnswer(Command<Commands::BuildRoadStop>::Do(flags, a, 1, 1, ParamInt(p, "truck", 0) != 0 ? RoadStopType::Truck : RoadStopType::Bus, through,
				ParamDir(p), roadtype, ROADSTOP_CLASS_DFLT, 0, StationID::Invalid(), false));
	}
	if (what == "rail_depot") return CommandAnswer(Command<Commands::BuildRailDepot>::Do(flags, a, railtype, ParamDir(p)));
	if (what == "road_depot") return CommandAnswer(Command<Commands::BuildRoadDepot>::Do(flags, a, roadtype, ParamDir(p)));
	if (what == "dock") return CommandAnswer(Command<Commands::BuildDock>::Do(flags, a, StationID::Invalid(), false));
	if (what == "bridge" || what == "tunnel") {
		const bool rail = ParamText(p, "transport", "road") == "rail";
		const TransportType transport = rail ? TransportType::Rail : TransportType::Road;
		if (what == "tunnel") return CommandAnswer(Command<Commands::BuildTunnel>::Do(flags, a, transport, rail ? railtype : INVALID_RAILTYPE, rail ? INVALID_ROADTYPE : roadtype));
		return CommandAnswer(Command<Commands::BuildBridge>::Do(flags, b, a, transport, static_cast<BridgeType>(Clamp(ParamInt(p, "bridge", 0), 0, MAX_BRIDGES - 1)),
				rail ? railtype : INVALID_RAILTYPE, rail ? INVALID_ROADTYPE : roadtype));
	}
	if (what == "clear") return CommandAnswer(std::get<0>(Command<Commands::ClearArea>::Do(flags, b, a, false)));
	if (what == "level") {
		const std::string mode = ParamText(p, "mode", "level");
		const LevelMode lm = mode == "raise" ? LevelMode::Raise : mode == "lower" ? LevelMode::Lower : LevelMode::Level;
		return CommandAnswer(std::get<0>(Command<Commands::LevelLand>::Do(flags, b, a, false, lm)));
	}
	return Error(400, "what: road, rail, rail_station, road_stop, rail_depot, road_depot, dock, bridge, tunnel, clear or level");
}

Answer VehicleAnswer(const Params &p)
{
	if (_game_mode != GameMode::Normal) return Error(409, "no game is running");
	if (Company::GetIfValid(_local_company) == nullptr) return Error(409, "there is no company");
	const std::string todo = ParamText(p, "do");
	AutoRestoreBackup company(_current_company, _local_company);
	const DoCommandFlags flags{DoCommandFlag::Execute};
	if (todo == "buy") {
		const TileIndex depot = ParamTile(p, "x", "y");
		if (depot == INVALID_TILE) return Error(400, "x, y: the tile of a depot");
		const int engine = ParamInt(p, "engine", -1);
		if (engine < 0 || Engine::GetIfValid(static_cast<EngineID>(engine)) == nullptr) return Error(400, "engine: an id of /engines");
		const auto made = Command<Commands::BuildVehicle>::Do(flags, depot, static_cast<EngineID>(engine), true, INVALID_CARGO, ClientID::Invalid);
		json more = json::object();
		if (std::get<0>(made).Succeeded()) more["id"] = std::get<1>(made).base();
		return CommandAnswer(std::get<0>(made), std::move(more));
	}
	const int id = ParamInt(p, "id", -1);
	const Vehicle *v = id < 0 ? nullptr : Vehicle::GetIfValid(static_cast<VehicleID>(id));
	if (v == nullptr || !v->IsPrimaryVehicle()) return Error(400, "id: an id of /vehicles");
	if (v->owner != _local_company) return Error(403, "the vehicle is another company's");
	if (todo == "start" || todo == "stop") {
		const bool stopped = v->vehstatus.Test(VehState::Stopped);
		if (stopped == (todo == "stop")) return JsonAnswer({{"ok", true}, {"cost", 0}, {"already", true}});
		return CommandAnswer(Command<Commands::StartStopVehicle>::Do(flags, v->index, false));
	}
	if (todo == "sell") return CommandAnswer(Command<Commands::SellVehicle>::Do(flags, v->index, true, false, ClientID::Invalid));
	if (todo == "depot") return CommandAnswer(Command<Commands::SendVehicleToDepot>::Do(flags, v->index, DepotCommandFlags{}, {}));
	if (todo == "order") {
		const TileIndex tile = ParamTile(p, "x", "y");
		const Station *st = tile != INVALID_TILE && IsTileType(tile, TileType::Station) ? Station::GetByTile(tile) : nullptr;
		if (st == nullptr) return Error(400, "x, y: a tile of a station");
		Order order{};
		order.MakeGoToStation(st->index);
		/* A fresh order stops at the near end of the platform, which only a train may do; the orders window sets the far end. */
		order.SetStopLocation(OrderStopLocation::FarEnd);
		return CommandAnswer(Command<Commands::InsertOrder>::Do(flags, v->index, v->GetNumOrders(), order));
	}
	return Error(400, "do: buy, start, stop, sell, depot or order");
}

Answer OnGameThread(std::function<Answer()> run);

/** A picture of the main view: asked on the game's thread, made on the video driver's, waited for and read here (the server's thread). */
Answer ScreenshotAnswer()
{
	std::string path;
	/* As the video driver shows it, with what the card lays (the map); else as the game's screenshot of the view. */
	if (!ScreenshotOfShown(path)) {
	const uint32_t before = LastScreenshotMade();
	const Answer asked = OnGameThread([] {
		if (_game_mode != GameMode::Normal && _game_mode != GameMode::Editor) return Error(409, "no game is running");
		_screenshot_no_message = true;
		if (!MakeScreenshot(SC_VIEWPORT, "agent_api")) return Error(500, "the screenshot could not be made");
		return Answer{};
	});
	if (asked.status != 200) return asked;
	for (int k = 0; LastScreenshotMade(&path) == before; k++) {
		if (k > 300 || _api_stop) return Error(504, "the screenshot was not made in time");
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (path.empty()) return Error(500, "the screenshot could not be made");
	}
	auto file = FileHandle::Open(path, "rb");
	if (!file.has_value()) return Error(500, "the screenshot's file cannot be read");
	Answer a;
	char buffer[65536];
	for (size_t n; (n = fread(buffer, 1, sizeof(buffer), *file)) > 0;) a.body.append(buffer, n);
	file.reset();
	std::error_code ec;
	std::filesystem::remove(OTTD2FS(path), ec); // the answer has it; none left in the player's screenshots
	a.type = path.ends_with(".png") ? "image/png" : path.ends_with(".bmp") ? "image/bmp" : path.ends_with(".pcx") ? "image/x-pcx" : "application/octet-stream";
	return a;
}

/** A request done on the game's thread, waited for here. */
Answer OnGameThread(std::function<Answer()> run)
{
	auto job = std::make_shared<Job>();
	job->run = std::move(run);
	std::future<Answer> done = job->done.get_future();
	{
		std::lock_guard lock(_api_mutex);
		_api_jobs.push_back(job);
	}
	if (done.wait_for(GAME_WAIT) != std::future_status::ready) return Error(504, "the game did not answer in time");
	return done.get();
}

std::string Lower(std::string_view s)
{
	std::string out(s);
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

/** A request by its path: what the HTTP routes and the tools of MCP share. */
Answer Dispatch(const std::string &method, const std::string &path, const Params &query, const std::string &body)
{
	static const std::map<std::string_view, std::string_view> paths = {{"/", "GET"}, {"/guide", "GET"}, {"/state", "GET"}, {"/map", "GET"}, {"/industries", "GET"},
			{"/vehicles", "GET"}, {"/towns", "GET"}, {"/stations", "GET"}, {"/engines", "GET"}, {"/build", "POST"}, {"/vehicle", "POST"}, {"/console", "POST"},
			{"/screenshot", "GET"}, {"/mcp", "POST"}};
	const auto known = paths.find(path);
	if (known == paths.end()) return Error(404, "no such path; GET / lists them");
	if (known->second != method) return Error(405, "this path takes another method");
	if (path == "/") return {200, "application/json; charset=utf-8", std::string(ROUTES)};
	if (path == "/guide") return {200, "text/plain; charset=utf-8", std::string(GUIDE)};
	if (path == "/state") return OnGameThread([] { return StateAnswer(); });
	if (path == "/map") return OnGameThread([query] { return MapAnswer(query); });
	if (path == "/industries") return OnGameThread([] { return IndustriesAnswer(); });
	if (path == "/vehicles") return OnGameThread([query] { return VehiclesAnswer(query); });
	if (path == "/towns") return OnGameThread([] { return TownsAnswer(); });
	if (path == "/stations") return OnGameThread([] { return StationsAnswer(); });
	if (path == "/engines") return OnGameThread([query] { return EnginesAnswer(query); });
	if (path == "/build") return OnGameThread([query] { return BuildAnswer(query); });
	if (path == "/vehicle") return OnGameThread([query] { return VehicleAnswer(query); });
	if (path == "/console") return OnGameThread([body] { return ConsoleAnswer(body); });
	if (path == "/screenshot") return ScreenshotAnswer();
	return Error(404, "no such path; GET / lists them");
}

std::string Base64(std::string_view data)
{
	static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		const uint32_t a = static_cast<uint8_t>(data[i]), b = i + 1 < data.size() ? static_cast<uint8_t>(data[i + 1]) : 0, c = i + 2 < data.size() ? static_cast<uint8_t>(data[i + 2]) : 0;
		const uint32_t n = a << 16 | b << 8 | c;
		out += digits[n >> 18 & 63];
		out += digits[n >> 12 & 63];
		out += i + 1 < data.size() ? digits[n >> 6 & 63] : '=';
		out += i + 2 < data.size() ? digits[n & 63] : '=';
	}
	return out;
}

/** The tools of the Model Context Protocol: each a path above, its parameters the tool's arguments. */
struct McpTool {
	std::string_view name, path, method, what;
	std::string_view arguments; ///< the properties of its arguments, as JSON
};

const McpTool MCP_TOOLS[] = {
	{"guide", "/guide", "GET", "How to use this interface: every tool and its parameters.", "{}"},
	{"state", "/state", "GET", "The state of the game: date, pause, map size, the company's money, counts of vehicles, towns, industries, stations.", "{}"},
	{"map", "/map", "GET", "The tiles of an area as data: kind, height, slope, owner, what stands there. 64x64 tiles at most.",
			R"({"x":{"type":"integer"},"y":{"type":"integer"},"w":{"type":"integer"},"h":{"type":"integer"}})"},
	{"industries", "/industries", "GET", "Every industry: where it is, what it makes (last month's production, percent carried away) and takes.", "{}"},
	{"vehicles", "/vehicles", "GET", "Every vehicle: where, speed, cargo, current order, profit.", R"({"type":{"type":"string","enum":["train","road","ship","aircraft"]}})"},
	{"towns", "/towns", "GET", "Every town: where, population.", "{}"},
	{"stations", "/stations", "GET", "Every station: where, what it has, the cargo waiting.", "{}"},
	{"engines", "/engines", "GET", "What the company can buy now.", R"({"type":{"type":"string","enum":["train","road","ship","aircraft"]}})"},
	{"build", "/build", "POST", "Build as the company. what: road, rail, rail_station, road_stop, rail_depot, road_depot, dock, bridge, tunnel, clear, level. See the guide for each one's parameters.",
			R"({"what":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"x2":{"type":"integer"},"y2":{"type":"integer"},"dir":{"type":"integer"},"axis":{"type":"string"},"tracks":{"type":"integer"},"length":{"type":"integer"},"railtype":{"type":"integer"},"roadtype":{"type":"integer"},"truck":{"type":"integer"},"through":{"type":"integer"},"transport":{"type":"string"},"bridge":{"type":"integer"},"mode":{"type":"string"}})"},
	{"vehicle", "/vehicle", "POST", "A vehicle of the company. do: buy (x, y of a depot, engine), start, stop, sell, depot (id), order (id, x, y of a station).",
			R"({"do":{"type":"string"},"id":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"engine":{"type":"integer"}})"},
	{"console", "/console", "POST", "Run console commands of the game, one a line; answers what they printed.", R"({"commands":{"type":"string"}})"},
	{"screenshot", "/screenshot", "GET", "A picture of the screen as it is shown.", "{}"},
};

/** A message of the Model Context Protocol (JSON-RPC 2.0), one a request; a notification gets 202 and nothing. */
Answer McpAnswer(const std::string &body)
{
	json message = json::parse(body, nullptr, false);
	if (message.is_discarded() || !message.is_object()) return JsonAnswer({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "not JSON"}}}}, 400);
	const json id = message.contains("id") ? message["id"] : json();
	const std::string method = message.value("method", "");
	auto result = [&id](json r) { return JsonAnswer({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(r)}}); };
	auto error = [&id](int code, std::string_view what) { return JsonAnswer({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", what}}}}); };
	if (!message.contains("id")) return {202, "application/json; charset=utf-8", ""}; // a notification (notifications/initialized)
	if (method == "initialize") {
		std::string version = "2025-03-26";
		if (message.contains("params") && message["params"].is_object() && message["params"].contains("protocolVersion") && message["params"]["protocolVersion"].is_string()) {
			version = message["params"]["protocolVersion"].get<std::string>();
		}
		return result({{"protocolVersion", version}, {"capabilities", {{"tools", json::object()}}}, {"serverInfo", {{"name", "openttd-decouple"}, {"version", _openttd_revision}}},
				{"instructions", "The game OpenTTD Decouple. Call the tool 'guide' first."}});
	}
	if (method == "ping") return result(json::object());
	if (method == "tools/list") {
		json tools = json::array();
		for (const McpTool &t : MCP_TOOLS) {
			tools.push_back({{"name", t.name}, {"description", t.what}, {"inputSchema", {{"type", "object"}, {"properties", json::parse(t.arguments)}}}});
		}
		return result({{"tools", std::move(tools)}});
	}
	if (method == "tools/call") {
		const json params = message.value("params", json::object());
		const std::string name = params.is_object() ? params.value("name", "") : std::string();
		const json arguments = params.is_object() && params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
		for (const McpTool &t : MCP_TOOLS) {
			if (name != t.name) continue;
			Params query;
			std::string commands;
			for (const auto &[key, value] : arguments.items()) {
				const std::string text = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "1" : "0") : value.dump();
				if (t.name == "console" && key == "commands") {
					commands = text;
				} else {
					query[key] = text;
				}
			}
			const Answer a = Dispatch(std::string(t.method), std::string(t.path), query, commands);
			json content = json::array();
			if (a.type.starts_with("image/")) {
				content.push_back({{"type", "image"}, {"data", Base64(a.body)}, {"mimeType", a.type}});
			} else {
				content.push_back({{"type", "text"}, {"text", a.body}});
			}
			return result({{"content", std::move(content)}, {"isError", a.status != 200}});
		}
		return error(-32602, "no such tool");
	}
	return error(-32601, "no such method");
}

Answer Route(const std::string &method, const std::string &target, const std::string &body)
{
	const size_t q = target.find('?');
	const std::string path = target.substr(0, q);
	Params query;
	if (q != std::string::npos) {
		size_t at = q + 1;
		while (at < target.size()) {
			size_t end = target.find('&', at);
			if (end == std::string::npos) end = target.size();
			const std::string pair = target.substr(at, end - at);
			const size_t eq = pair.find('=');
			query[pair.substr(0, eq)] = eq == std::string::npos ? "" : pair.substr(eq + 1);
			at = end + 1;
		}
	}
	if (path == "/mcp" && method == "POST") return McpAnswer(body);
	return Dispatch(method, path, query, body);
}

void SendAll(SOCKET s, std::string_view data)
{
	while (!data.empty()) {
		const auto n = send(s, data.data(), static_cast<int>(std::min<size_t>(data.size(), 1 << 16)), 0);
		if (n <= 0) return;
		data.remove_prefix(static_cast<size_t>(n));
	}
}

/** Whether there is something to read on a socket within so many milliseconds. */
bool Readable(SOCKET s, int ms)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	timeval tv{ms / 1000, (ms % 1000) * 1000};
	return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

void Serve(SOCKET s)
{
	std::string request;
	size_t head_end = std::string::npos;
	size_t length = 0;
	char buffer[8192];
	/* The head, then as much of the body as it says. */
	while (request.size() < REQUEST_MOST) {
		if (head_end != std::string::npos && request.size() >= head_end + 4 + length) break;
		if (!Readable(s, 5000)) return;
		const auto n = recv(s, buffer, sizeof(buffer), 0);
		if (n <= 0) return;
		request.append(buffer, static_cast<size_t>(n));
		if (head_end == std::string::npos) {
			head_end = request.find("\r\n\r\n");
			if (head_end != std::string::npos) {
				const std::string head = Lower(std::string_view(request).substr(0, head_end));
				if (size_t at = head.find("\r\ncontent-length:"); at != std::string::npos) {
					at += 17;
					while (at < head.size() && head[at] == ' ') at++;
					std::from_chars(head.data() + at, head.data() + head.size(), length);
				}
				if (length > REQUEST_MOST) length = REQUEST_MOST;
			}
		}
	}
	Answer answer;
	if (head_end == std::string::npos) {
		answer = Error(400, "not a request of HTTP");
	} else {
		const std::string head = request.substr(0, head_end);
		const std::string lower = Lower(head);
		const size_t line_end = head.find("\r\n");
		const std::string first = head.substr(0, line_end);
		const size_t a = first.find(' '), b = first.find(' ', a == std::string::npos ? 0 : a + 1);
		/* Only this computer, and no page of a browser: the Host it was asked by must be it, and no Origin. */
		auto header = [&lower](const char *name) -> std::string {
			const std::string key = std::string("\r\n") + name + ":";
			const size_t at = lower.find(key);
			if (at == std::string::npos) return {};
			const size_t from = at + key.size(), to = lower.find("\r\n", from);
			std::string value = lower.substr(from, to == std::string::npos ? std::string::npos : to - from);
			while (!value.empty() && value.front() == ' ') value.erase(value.begin());
			return value;
		};
		const std::string host = header("host");
		const bool host_ok = host.empty() || host.starts_with("127.0.0.1") || host.starts_with("localhost");
		if (a == std::string::npos || b == std::string::npos) {
			answer = Error(400, "not a request of HTTP");
		} else if (!host_ok || lower.find("\r\norigin:") != std::string::npos) {
			answer = Error(403, "asked from elsewhere than this computer, or by a page of a browser");
		} else {
			try {
				answer = Route(first.substr(0, a), first.substr(a + 1, b - a - 1), request.substr(std::min(request.size(), head_end + 4), length));
			} catch (const std::exception &e) {
				answer = Error(500, e.what());
			}
		}
	}
	static const std::map<int, std::string_view> reasons = {{200, "OK"}, {202, "Accepted"}, {400, "Bad Request"}, {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"}, {409, "Conflict"},
			{500, "Internal Server Error"}, {504, "Gateway Timeout"}};
	const auto reason = reasons.find(answer.status);
	SendAll(s, fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", answer.status,
			reason != reasons.end() ? reason->second : "Error", answer.type, answer.body.size()));
	SendAll(s, answer.body);
}

/** The server's thread: one request at a time. */
void Listen()
{
	while (!_api_stop) {
		if (!Readable(_api_listen, 200)) continue;
		const SOCKET s = accept(_api_listen, nullptr, nullptr);
		if (s == INVALID_SOCKET) continue;
		Serve(s);
		closesocket(s);
	}
}

} // namespace

std::string AgentApiStart(uint16_t port)
{
	AgentApiStop();
	const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET) return "no socket";
	int yes = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this computer alone
	if (bind(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || listen(s, 8) != 0) {
		closesocket(s);
		return fmt::format("port {} cannot be listened on (taken?)", port);
	}
	_api_listen = s;
	_api_stop = false;
	_api_port = port;
	_api_thread = std::thread(&Listen);
	Debug(misc, 0, "Agent interface: listening on 127.0.0.1:{}", port);
	return {};
}

void AgentApiStop()
{
	if (!_api_thread.joinable()) return;
	/* Asked by a request of its own ('api off' through /console): after its answer (AgentApiPump()) -- the server's
	 * thread waits for that answer, and this would wait for the thread. */
	if (_api_in_job) {
		_api_stop_after = true;
		return;
	}
	_api_stop = true;
	/* What waits for the game is answered that it ended. */
	{
		std::lock_guard lock(_api_mutex);
		for (const auto &job : _api_jobs) job->done.set_value(Error(503, "the interface was stopped"));
		_api_jobs.clear();
	}
	_api_thread.join();
	closesocket(_api_listen);
	_api_listen = INVALID_SOCKET;
	_api_port = 0;
}

uint16_t AgentApiPort()
{
	return _api_port;
}

void AgentApiPump()
{
	if (_api_port == 0) return;
	for (;;) {
		std::shared_ptr<Job> job;
		{
			std::lock_guard lock(_api_mutex);
			if (_api_jobs.empty()) return;
			job = _api_jobs.front();
			_api_jobs.pop_front();
		}
		Answer answer;
		_api_in_job = true;
		try {
			answer = job->run();
		} catch (const std::exception &e) {
			answer = Error(500, e.what());
		}
		_api_in_job = false;
		job->done.set_value(std::move(answer));
		if (_api_stop_after) {
			_api_stop_after = false;
			AgentApiStop();
			return;
		}
	}
}
