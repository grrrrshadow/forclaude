/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file crash_report_send.cpp The crash reports of the game sent to its developers when it starts again.
 *
 * The player (30. 10.): "have them send us the crash reports from the save folder when the game starts; a crash
 * report is a problem, so send it to us without asking", "we need the most data: when the logs are made in the save
 * folder, we want them", "we want the save too". When the game starts and finds a crash report in the folder of the
 * saves (CrashLog: crash<time>.json.log) not sent yet, it sends to the developers' Discord channel the report with its
 * save and its screenshot (not its crash.dmp: mostly empty), every diary in that folder -- those of the game that
 * crashed, copied to the crash's files before this game writes its own -- and the newest autosave (every 3 minutes by
 * default); then the older crashes. A few messages, each of at most DISCORD_FILES files and DISCORD_BYTES bytes. Sent,
 * the crash's files are renamed "...-odeslano", the copies let go of, and the player is told; not sent (no network),
 * they are sent the next time.
 *
 * Where to: the webhook given to the build (DECOUPLE_CRASH_WEBHOOK, a secret of the repository, as anyone who has it
 * can write to the channel or delete the webhook), or for the rig the environment's DECOUPLE_CRASH_WEBHOOK; without
 * one nothing is sent. The name of the player's account in the paths of the diaries is left out ("<uzivatel>").
 */

#include "stdafx.h"
#include "crash_report_send.h"
#include "fileio_func.h"
#include "debug.h"
#include "error.h"
#include "rev.h"
#include "string_func.h"
#include "strings_func.h"
#include "network/core/http.h"
#include "3rdparty/nlohmann/json.hpp"
#include "table/strings.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "safeguards.h"

/** At most so many files in a message to the channel (Discord's limit) ... */
static constexpr size_t DISCORD_FILES = 10;
/** ... and so many bytes of them (Discord's 10 MB, less what the message itself takes). */
static constexpr size_t DISCORD_BYTES = 8u * 1024 * 1024;
/** At most so many messages a report: the files beyond are left out (Discord takes five messages in two seconds). */
static constexpr size_t DISCORD_MESSAGES = 5;

/** A file of the report, read when it is sent. */
struct ReportFile {
	std::string name;
	std::filesystem::path path; ///< its file in the folder of the saves: marked or let go of when it is sent
	size_t size;                ///< of what is sent: of a diary at most its end (TextOfDiary())
};

static std::vector<ReportFile> _report_files;                   ///< what goes, in order: the newest crash's, with the diaries and the autosave, then the older crashes'
static std::vector<std::filesystem::path> _report_not_attached; ///< files of crashes empty or too big for a message: marked with the first message, never to go
static std::vector<std::string> _report_too_big;                ///< the names of those too big, told in the first message
static std::string _report_reason;                              ///< why the game went down, from the newest report

/** Where the reports go: the rig's, else the build's; empty: nowhere. */
static std::string CrashReportWebhook()
{
	if (const char *rig = std::getenv("DECOUPLE_CRASH_WEBHOOK"); rig != nullptr && *rig != '\0') return rig;
#ifdef DECOUPLE_CRASH_WEBHOOK
	return DECOUPLE_CRASH_WEBHOOK;
#else
	return {};
#endif
}

/** The name of the player's account, which stands in the paths of the diaries. */
static std::string AccountName()
{
#ifdef _WIN32
	if (const wchar_t *name = _wgetenv(L"USERNAME"); name != nullptr) return FS2OTTD(name);
	return {};
#else
	const char *name = std::getenv("USER");
	return name != nullptr ? name : "";
#endif
}

/** A file whole, empty when it cannot be read. */
static std::string ReadWhole(const std::filesystem::path &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/**
 * A diary as it is sent: the name of the player's account in its paths left out (between two slashes of a path), and of
 * one too big for a message its end, where the game went down.
 */
static std::string TextOfDiary(std::string text, std::string_view account)
{
	if (account.size() >= 2) {
		for (size_t at = text.find(account); at != std::string::npos; at = text.find(account, at + 1)) {
			const size_t end = at + account.size();
			if (at == 0 || end >= text.size()) continue;
			const char before = text[at - 1], after = text[end];
			if ((before != '\\' && before != '/') || (after != '\\' && after != '/')) continue;
			text.replace(at, account.size(), "<uzivatel>");
		}
	}
	if (text.size() > DISCORD_BYTES) text = fmt::format("(jen konec deniku)\n{}", std::string_view(text).substr(text.size() - (DISCORD_BYTES - 32)));
	return text;
}

/** Whether a file of the folder of the saves is a diary: the game's *.log, and its log.txt of what went wrong (anomaly_log.cpp). */
static bool IsDiary(std::string_view name)
{
	return name.ends_with(".log") || name.ends_with(".txt");
}

/** The crash a file of the folder of the saves is of, "crash<time>" (crash<time>.json.log, crash<time>-snimky.log); empty: none. */
static std::string CrashOf(std::string_view name)
{
	if (!name.starts_with("crash")) return {};
	size_t end = 5;
	while (end < name.size() && name[end] >= '0' && name[end] <= '9') end++;
	return end > 5 ? std::string(name.substr(0, end)) : std::string{};
}

/** The newest of files, nullptr none. */
static const std::filesystem::path *NewestOf(const std::vector<std::filesystem::path> &files)
{
	std::error_code ec;
	const std::filesystem::path *newest = nullptr;
	for (const std::filesystem::path &path : files) {
		if (newest == nullptr || std::filesystem::last_write_time(path, ec) > std::filesystem::last_write_time(*newest, ec)) newest = &path;
	}
	return newest;
}

/**
 * When the game starts, before it writes its own diaries: a crash report in the folder of the saves not sent yet, and
 * what goes with it, found to be sent (SendCrashReport()). The diaries are those of the game that crashed only now: the
 * game writes them anew (the player, 30. 10.: "they are written over in the saves by new logs"). So the first time
 * after a crash they are copied to its files, "crash<time>-<diary>", with the newest autosave (written over in a few
 * minutes too), to go with it however late it goes; of the mouse's diaries, each of a game ("mouse and crash are not
 * written over"), the newest only.
 */
void CollectCrashReport()
{
	if (CrashReportWebhook().empty()) return;
	const std::string folder = FioFindDirectory(Subdirectory::Save);
	std::error_code ec;
	std::map<std::string, std::vector<std::filesystem::path>, std::greater<>> crashes; ///< the newest first
	std::vector<std::filesystem::path> diaries, mice;
	std::string last_crash; ///< the newest crash, sent or not
	bool report = false;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string name = FS2OTTD(entry.path().filename().native());
		last_crash = std::max(last_crash, CrashOf(name));
		if (name.find("-odeslano") != std::string::npos) continue;
		if (const std::string crash = CrashOf(name); !crash.empty()) {
			if (name.ends_with(".dmp")) continue; // mostly empty (the player)
			crashes[crash].push_back(entry.path());
			report |= name.ends_with(".json.log");
		} else if (name.starts_with("mouse") && name.ends_with(".log")) {
			mice.push_back(entry.path());
		} else if (IsDiary(name)) {
			diaries.push_back(entry.path());
		}
	}
	if (!report) return;

	/* The diaries of the game that crashed and the newest autosave kept with the newest crash, the first time; not with
	 * an older crash left for this time (a newer one sent): the diaries are no more of the game that crashed. */
	auto &[newest_crash, newest_files] = *crashes.begin();
	const bool kept = std::ranges::any_of(newest_files, [&newest_crash](const std::filesystem::path &path) {
		return FS2OTTD(path.filename().native()).starts_with(newest_crash + "-");
	});
	if (!kept && newest_crash == last_crash) {
		if (const std::filesystem::path *mouse = NewestOf(mice); mouse != nullptr) diaries.push_back(*mouse);
		std::vector<std::filesystem::path> autosaves;
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(FioFindDirectory(Subdirectory::Autosave)), ec)) {
			if (entry.is_regular_file(ec) && entry.path().extension() == ".sav") autosaves.push_back(entry.path());
		}
		if (const std::filesystem::path *autosave = NewestOf(autosaves); autosave != nullptr) diaries.push_back(*autosave);
		for (const std::filesystem::path &path : diaries) {
			const std::filesystem::path copy = std::filesystem::path(OTTD2FS(folder)) / OTTD2FS(newest_crash + "-" + FS2OTTD(path.filename().native()));
			if (std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing, ec)) newest_files.push_back(copy);
		}
	}

	bool newest = true;
	for (const auto &[crash, files] : crashes) {
		/* Of each crash its report first, then its screenshot and its save, the diaries, the autosave. */
		std::vector<std::filesystem::path> ordered = files;
		auto rank = [](const std::filesystem::path &path) {
			const std::string name = FS2OTTD(path.filename().native());
			if (name.ends_with(".json.log")) return 0;
			if (!name.starts_with(CrashOf(name) + "-")) return 1;
			return IsDiary(name) ? 2 : 3;
		};
		std::ranges::stable_sort(ordered, {}, rank);
		for (const std::filesystem::path &path : ordered) {
			std::string name = FS2OTTD(path.filename().native());
			uintmax_t size = std::filesystem::file_size(path, ec);
			if (ec) continue;
			if (IsDiary(name)) size = std::min<uintmax_t>(size, DISCORD_BYTES);
			if (newest && name.ends_with(".json.log")) {
				auto json = nlohmann::json::parse(ReadWhole(path), nullptr, false);
				if (!json.is_discarded() && json.contains("crash") && json["crash"].contains("reason") && json["crash"]["reason"].is_string()) {
					_report_reason = json["crash"]["reason"].get<std::string>();
				}
			}
			/* Discord takes no empty file: such a crash report (the crash went too wrong to write it) only marked. */
			if (size == 0 || size > DISCORD_BYTES) {
				if (size > 0) _report_too_big.push_back(fmt::format("{} ({:.1f} MB)", name, size / (1024.0 * 1024.0)));
				_report_not_attached.push_back(path);
				continue;
			}
			_report_files.push_back({std::move(name), path, static_cast<size_t>(size)});
		}
		newest = false;
	}
	Debug(misc, 1, "Crash report: {} files to send, {} not (empty or too big)", _report_files.size(), _report_not_attached.size());
}

/**
 * Files of crash reports sent: the copies of the diaries and the autosave let go of, the crash's own files kept, marked:
 * crash<time>.json.log is crash<time>-odeslano.json.log.
 */
static void MarkSent(const std::vector<std::filesystem::path> &files)
{
	std::error_code ec;
	for (const std::filesystem::path &path : files) {
		const std::string name = FS2OTTD(path.filename().native());
		if (name.starts_with(CrashOf(name) + "-")) {
			std::filesystem::remove(path, ec);
			continue;
		}
		const size_t dot = std::min(name.find('.'), name.size());
		std::filesystem::path sent = path;
		sent.replace_filename(OTTD2FS(name.substr(0, dot) + "-odeslano" + name.substr(dot)));
		std::filesystem::rename(path, sent, ec);
	}
}

/** A message of the report on its way to the channel: sent, its files are marked (MarkSent()); not, they go the next time. */
struct CrashReportMessage : HTTPCallback {
	std::vector<std::filesystem::path> files;
	bool done = false, ok = false;

	void OnFailure() override
	{
		this->done = true;
		CrashReportMessage::Finished();
	}

	void OnReceiveData(std::unique_ptr<char[]> data, size_t) override
	{
		if (data != nullptr) return; // Discord's answer, if any
		this->done = true;
		this->ok = true;
		MarkSent(this->files);
		CrashReportMessage::Finished();
	}

	bool IsCancelled() const override { return false; }

	static void Finished();
};

static std::vector<std::unique_ptr<CrashReportMessage>> _report_messages;

/** A message of the report answered: when all are, the player told of those sent. */
void CrashReportMessage::Finished()
{
	if (!std::ranges::all_of(_report_messages, [](const auto &m) { return m->done; })) return;
	const size_t sent = std::ranges::count_if(_report_messages, [](const auto &m) { return m->ok; });
	Debug(misc, 0, "Crash report: {} of {} messages sent{}", sent, _report_messages.size(), sent < _report_messages.size() ? ", the rest the next time" : "");
	if (sent > 0) ShowErrorMessage(GetEncodedString(STR_CRASH_REPORT_SENT), {}, WarningLevel::Info);
}

/** The report collected (CollectCrashReport()) sent, when the game's network is up: a message for each few files. */
void SendCrashReport()
{
	if (_report_files.empty() && _report_not_attached.empty()) return;
	const std::string webhook = CrashReportWebhook();
	/* The files in messages, in order: at most so many files and bytes each; the rest the next time. */
	std::vector<std::vector<const ReportFile *>> messages;
	size_t bytes = 0, later = 0;
	for (const ReportFile &file : _report_files) {
		const bool fits = !messages.empty() && messages.back().size() < DISCORD_FILES && bytes + file.size <= DISCORD_BYTES;
		if (later == 0 && !fits && messages.size() < DISCORD_MESSAGES) {
			messages.emplace_back();
			bytes = 0;
		} else if (later != 0 || !fits) {
			later++;
			continue;
		}
		messages.back().push_back(&file);
		bytes += file.size;
	}
	if (messages.empty()) messages.emplace_back(); // only crash reports not attached: told of
	const std::string account = AccountName();
	const std::string boundary = fmt::format("decouple{:x}", std::chrono::steady_clock::now().time_since_epoch().count());
	for (size_t k = 0; k < messages.size(); k++) {
		std::string text = fmt::format("Pad hry Decouple {} ({}/{})", _openttd_revision, k + 1, messages.size());
		if (k == 0 && !_report_reason.empty()) text += "\nDuvod: " + _report_reason;
		if (k == 0 && !_report_too_big.empty()) {
			text += "\nNeposlano (moc velke):";
			for (const std::string &name : _report_too_big) text += " " + name;
		}
		if (k == 0 && later != 0) text += fmt::format("\nStarsi pady pristim startem: {} souboru", later);
		if (text.size() > 1900) text.resize(1900);
		nlohmann::json payload;
		payload["username"] = "Decouple - pady";
		payload["content"] = text;
		/* A character cut in two at the end replaced, not thrown at. */
		const std::string json = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		std::string body = fmt::format("--{}\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n{}\r\n", boundary, json);
		auto message = std::make_unique<CrashReportMessage>();
		if (k == 0) message->files = _report_not_attached;
		for (size_t f = 0; f < messages[k].size(); f++) {
			const ReportFile &file = *messages[k][f];
			std::string data = ReadWhole(file.path);
			if (IsDiary(file.name)) data = TextOfDiary(std::move(data), account);
			body += fmt::format("--{}\r\nContent-Disposition: form-data; name=\"files[{}]\"; filename=\"{}\"\r\nContent-Type: application/octet-stream\r\n\r\n", boundary, f, file.name);
			body += data;
			body += "\r\n";
			message->files.push_back(file.path);
		}
		body += fmt::format("--{}--\r\n", boundary);
		_report_messages.push_back(std::move(message));
		NetworkHTTPSocketHandler::Connect(webhook, _report_messages.back().get(), std::move(body), fmt::format("multipart/form-data; boundary={}", boundary));
	}
	Debug(misc, 1, "Crash report: {} messages on their way", messages.size());
}
