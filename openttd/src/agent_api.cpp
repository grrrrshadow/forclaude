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
#include "company_base.h"
#include "company_func.h"
#include "console_func.h"
#include "debug.h"
#include "fileio_func.h"
#include "industry.h"
#include "landscape.h"
#include "map_func.h"
#include "openttd.h"
#include "rev.h"
#include "screenshot.h"
#include "settings_type.h"
#include "station_base.h"
#include "strings_func.h"
#include "tile_cmd.h"
#include "tile_map.h"
#include "timer/timer_game_calendar.h"
#include "town.h"
#include "vehicle_base.h"

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
{"method":"POST","path":"/console","what":"console commands, one a line of the body; answers what they printed"},
{"method":"GET","path":"/screenshot","what":"a picture of the main view, in the game's screenshot format (PNG where the build has it)"}
]})json";

const std::string_view GUIDE =
	"OpenTTD Decouple: the interface for an agent\n"
	"\n"
	"The server listens on 127.0.0.1 only and is off till the console's command 'api' (or 'api <port>') turns it on;\n"
	"'api off' stops it. Every answer but /guide and /screenshot is JSON with \"ok\".\n"
	"\n"
	"GET /state            the date, whether the game is paused, the size of the map, the local company's money,\n"
	"                      how many vehicles, towns, industries and stations there are.\n"
	"GET /map?x=&y=&w=&h=  the tiles of an area: kind, height, slope, owner and what the land info tool says of it.\n"
	"POST /console         the body is console commands, one a line. The answer has \"lines\": what they printed.\n"
	"                      All the game's console commands work: 'help', 'list_cmds', 'pause', 'unpause', 'setting',\n"
	"                      'save', 'load', 'screenshot', and the test rig's 'test3d ...' and 'testlod ...'.\n"
	"GET /screenshot       a picture of the main view as the game's screenshot command makes it.\n"
	"\n"
	"Tiles are x, y from the map's north corner; a tile's index is y * (the map's width) + x.\n"
	"Requests are done on the game's thread between its ticks, one at a time, in the order they came.\n";

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
	std::filesystem::remove(std::filesystem::u8path(path), ec); // the answer has it; none left in the player's screenshots
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

Answer Route(const std::string &method, const std::string &target, const std::string &body)
{
	const size_t q = target.find('?');
	const std::string path = target.substr(0, q);
	std::map<std::string, std::string> query;
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
	if (path == "/" && method == "GET") return {200, "application/json; charset=utf-8", std::string(ROUTES)};
	if (path == "/guide" && method == "GET") return {200, "text/plain; charset=utf-8", std::string(GUIDE)};
	if (path == "/state" && method == "GET") return OnGameThread([] { return StateAnswer(); });
	if (path == "/map" && method == "GET") return OnGameThread([query] { return MapAnswer(query); });
	if (path == "/console" && method == "POST") return OnGameThread([body] { return ConsoleAnswer(body); });
	if (path == "/screenshot" && method == "GET") return ScreenshotAnswer();
	if (path == "/" || path == "/guide" || path == "/state" || path == "/map" || path == "/console" || path == "/screenshot") return Error(405, "this path takes another method");
	return Error(404, "no such path; GET / lists them");
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
	static const std::map<int, std::string_view> reasons = {{200, "OK"}, {400, "Bad Request"}, {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"}, {409, "Conflict"},
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
