/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file anomaly_log.cpp Writing the record described in anomaly_log.h. */

#include "stdafx.h"
#include "anomaly_log.h"
#include "fileio_func.h"
#include "fileio_type.h"
#include "console_func.h"
#include "debug.h"
#include "timer/timer_game_tick.h"
#include "timer/timer_game_calendar.h"
#include "rev.h"

#include <map>

#include "safeguards.h"

/** Where it lives: beside the savegames, because that is the folder the player already knows. */
static const char *ANOMALY_LOG_NAME = "log.txt";

/**
 * How long a line has to be quiet before the same line may be written again.
 * A fault that has happened once goes on happening every tick -- a train that
 * cannot find a way asks again forever -- and a file full of one line is a
 * file nobody can read. Roughly a minute of game time.
 */
static const uint ANOMALY_REPEAT_TICKS = 2000;

/** At what point the file stops growing, so a game left running overnight cannot fill a disk. */
static const uint ANOMALY_MAX_LINES = 5000;

/**
 * How many times one particular line may be written before it is taken as
 * said. Some of what is worth a line is a state rather than an event -- a
 * breakdown nobody ever came for stays a breakdown nobody ever came for --
 * and without this it would write itself into the file for the rest of the
 * game. The count goes on being kept; only the writing stops.
 */
static const uint ANOMALY_MAX_SAME = 10;

static std::optional<FileHandle> _anomaly_file; ///< Opened when a game begins, then kept open.
static bool _anomaly_on = true;                 ///< The player's switch: whether anything is written at all.
static bool _anomaly_broken = false;            ///< This game's file could not be opened; retried at the next start.
static bool _anomaly_full = false;              ///< The cap has been reached and said so.
static uint _anomaly_lines = 0;                 ///< How many lines this game has written.

/** What has been said already, so that saying it again can be counted instead of repeated. */
struct AnomalySaid {
	uint count = 0;                  ///< How many times it has happened.
	uint written = 0;                ///< How many times it has been written.
	TimerGameTick::TickCounter said = 0; ///< When it was last written.
};
static std::map<std::string, AnomalySaid> _anomaly_said;

/** The full path of the log, whether or not it exists yet. */
std::string GetAnomalyLogPath()
{
	return FioFindDirectory(Subdirectory::Save) + ANOMALY_LOG_NAME;
}

uint GetAnomalyLogLines() { return _anomaly_lines; }
bool IsAnomalyLogOn() { return _anomaly_on; }

void SetAnomalyLogOn(bool on)
{
	_anomaly_on = on;
}

/** Let go of the file, so that what is in it is on the disk. */
void CloseAnomalyLog()
{
	_anomaly_file.reset();
}

/**
 * A game has begun: start its part of the record.
 *
 * Written whether or not anything ever goes wrong, and that is the point. The
 * record used to be opened by the first fault, so a game in which nothing went
 * wrong left nothing at all -- and a player looking at a file last touched
 * hours ago cannot tell "nothing happened" from "the record is broken". Now
 * the heading is there from the start: a file whose last heading is this
 * game's is a file that is working, and an empty stretch under it means what
 * it says.
 *
 * It also puts everything counted back to nought. What a line has already
 * said, how many lines there are and whether the cap was reached are all about
 * one game; carrying them from one game into the next quietly muted the next
 * one, because a line said ten times in the game before was already taken as
 * said. The player's own switch is left alone -- that is his, not the game's.
 *
 * @param what what kind of start this is, already in words.
 */
void StartAnomalyLogForGame(std::string_view what)
{
	_anomaly_said.clear();
	_anomaly_lines = 0;
	_anomaly_full = false;
	_anomaly_broken = false;
	_anomaly_file.reset();

	if (!_anomaly_on) return;

	_anomaly_file = FileHandle::Open(GetAnomalyLogPath(), "ab");
	if (!_anomaly_file.has_value()) {
		IConsolePrint(CC_ERROR, "log: nejde psat do '{}' - zaznam vypnut do dalsi hry.", GetAnomalyLogPath());
		_anomaly_broken = true;
		return;
	}

	fmt::print(*_anomaly_file, "\n=== {} {} | {} ===\n", GetLogPrefix(true), _openttd_revision, what);
	fflush(*_anomaly_file);
}

/**
 * Put one line in the file, opening it if this is the first.
 * @param line the line, already put together
 */
static void WriteAnomalyLine(const std::string &line)
{
	if (!_anomaly_file.has_value()) {
		/* A game normally opens the file when it begins, so this is a line from
		 * before any game did -- while the NewGRFs of a game being loaded are
		 * read, for one. It gets a heading of its own so that it does not read
		 * as belonging to whatever game the file last held. */
		_anomaly_file = FileHandle::Open(GetAnomalyLogPath(), "ab");
		if (!_anomaly_file.has_value()) {
			/* Said once, to the console only: a log that cannot be written is
			 * not worth a second message every time something happens. */
			if (!_anomaly_broken) {
				IConsolePrint(CC_ERROR, "log: nejde psat do '{}' - zaznam vypnut do dalsi hry.", GetAnomalyLogPath());
				_anomaly_broken = true;
			}
			return;
		}
		fmt::print(*_anomaly_file, "\n=== {} {} | pred zacatkem hry ===\n", GetLogPrefix(true), _openttd_revision);
	}

	try {
		fmt::print(*_anomaly_file, "{}\n", line);
		/* Flushed line by line on purpose. What this file is for is the run that
		 * ended badly, and a line still sitting in a buffer when that happens is
		 * a line that was never written. */
		fflush(*_anomaly_file);
	} catch (const std::system_error &) {
		_anomaly_file.reset();
		_anomaly_broken = true;
		IConsolePrint(CC_ERROR, "log: psani selhalo - zaznam vypnut do dalsi hry.");
		return;
	}

	_anomaly_lines++;
}

/**
 * Write a line about something that should not have happened.
 *
 * Also said on the console, because a player who happens to be watching should
 * see it at the moment it happens, and because the headless rig reads the
 * console and can count these.
 *
 * @param what what happened, in one line
 */
void LogAnomaly(const std::string &what)
{
	if (!_anomaly_on || _anomaly_broken) return;

	AnomalySaid &said = _anomaly_said[what];
	said.count++;

	/* The first time it is news. After that it is news again only once in a
	 * while, and then the line says how many times it has happened, which is
	 * the part that matters -- twice is a coincidence, four hundred times is
	 * a train doing it every tick. */
	bool first = said.count == 1;
	if (!first && TimerGameTick::counter - said.said < ANOMALY_REPEAT_TICKS) return;
	if (said.written > ANOMALY_MAX_SAME) return;
	said.said = TimerGameTick::counter;
	said.written++;

	if (_anomaly_lines >= ANOMALY_MAX_LINES) {
		if (!_anomaly_full) {
			_anomaly_full = true;
			WriteAnomalyLine(fmt::format("--- {} radek, dost; dal uz se nezapisuje ---", _anomaly_lines));
		}
		return;
	}

	TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
	std::string line = fmt::format("[tik {} | {}-{:02}-{:02}] {}", TimerGameTick::counter,
			ymd.year.base(), ymd.month + 1, ymd.day, what);
	if (!first) line += fmt::format(" (uz {}x)", said.count);
	if (said.written > ANOMALY_MAX_SAME) line += " - tuhle radku uz dal nepisu";

	WriteAnomalyLine(line);
	/* Marked on the console, so that a player watching sees it as it happens
	 * and the headless rig can count these without knowing what any of them
	 * says. In the file the mark would be on every line and says nothing. */
	IConsolePrint(CC_WARNING, "ZAZNAM: {}", what);
}
