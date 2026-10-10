/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file crashlog.h Functions to be called to log a crash. */

#ifndef CRASHLOG_H
#define CRASHLOG_H

#include "3rdparty/nlohmann/json.hpp"

/**
 * Helper class for creating crash logs.
 */
class CrashLog {
private:
	/** Error message coming from #FatalError(format, ...). */
	static std::string message;

	/**
	 * Whereabouts of the code when it crashed, in words.
	 *
	 * A hardware fault on a machine with no debugging library gives an empty
	 * stacktrace and nothing else -- the crash reason is then only
	 * "EXCEPTION_ACCESS_VIOLATION", which says what happened and nothing at all
	 * about where. This is the substitute: a coarse marker the loading path
	 * moves along as it goes, so a crash report from such a machine at least
	 * names the neighbourhood. Cheap enough to set often; it is a pointer
	 * assignment to a literal, not a formatted string.
	 */
	static std::string_view stage;

	/**
	 * Anything the crash handler wants to say about this crash that is not the
	 * fault itself -- currently, that a NewGRF the savegame asked for was
	 * missing or had to be replaced by another version, which vanilla treats
	 * as reason enough to write no crash report at all.
	 */
	static std::string note;

	/**
	 * Convert system crash reason to JSON.
	 *
	 * @param survey The JSON object.
	 */
	virtual void SurveyCrash(nlohmann::json &survey) const = 0;

	/**
	 * Convert stacktrace to JSON.
	 *
	 * @param survey The JSON object.
	 */
	virtual void SurveyStacktrace(nlohmann::json &survey) const = 0;

	/**
	 * Execute the func() and return its value. If any exception / signal / crash happens,
	 * catch it and return false. This function should, in theory, never not return, even
	 * in the worst conditions.
	 *
	 * @param section_name The name of the section to be executed. Printed when a crash happens.
	 * @param func The function to call.
	 * @return true iff the function returned true.
	 */
	virtual bool TryExecute(std::string_view section_name, std::function<bool()> &&func) = 0;

protected:
	std::string CreateFileName(std::string_view ext, bool with_dir = true) const;

public:
	/** Ensure the destructor of the sub classes are called as well. */
	virtual ~CrashLog() = default;

	nlohmann::json survey;
	std::string crashlog_filename;
	std::string crashdump_filename;
	std::string savegame_filename;
	std::string screenshot_filename;

	void FillCrashLog();
	void PrintCrashLog() const;

	bool WriteCrashLog();
	virtual bool WriteCrashDump();
	bool WriteSavegame();
	bool WriteScreenshot();

	void SendSurvey() const;

	void MakeCrashLog();

	/**
	 * Initialiser for crash logs; do the appropriate things so crashes are
	 * handled by our crash handler instead of returning straight to the OS.
	 * @note must be implemented by all implementers of CrashLog.
	 */
	static void InitialiseCrashLog();

	/**
	 * Prepare crash log handler for a newly started thread.
	 * @note must be implemented by all implementers of CrashLog.
	 */
	static void InitThread();

	static void SetErrorMessage(const std::string &message);
	static void AfterCrashLogCleanup();

	/**
	 * Say where the code is now, for a crash report that will have no
	 * stacktrace. Pass a string literal: it is stored by reference, not copied.
	 */
	static void SetStage(std::string_view where) { CrashLog::stage = where; }
	static void SetNote(const std::string &what) { CrashLog::note = what; }
};

#endif /* CRASHLOG_H */
