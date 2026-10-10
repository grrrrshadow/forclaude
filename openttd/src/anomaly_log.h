/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file anomaly_log.h A running record of things the game should never have had to cope with.
 *
 * Not a trace. The orientation trace (vlak123) says what the game is doing and
 * is read while watching it; this says what it had to work around, is always
 * on, and is read afterwards -- usually days afterwards, by which time nobody
 * remembers what they were doing. It exists because the game stopped crashing:
 * a fault used to announce itself with a crash report, and now the same fault
 * is a train that quietly stands still for the rest of the game and nobody
 * ever hears about it.
 *
 * So the bar for a line here is high: the game did something it should never
 * have had to do. Anything that happens in ordinary play belongs on the trace.
 */

#ifndef ANOMALY_LOG_H
#define ANOMALY_LOG_H

#include <string>
#include "3rdparty/fmt/format.h"

void LogAnomaly(const std::string &what);

/**
 * Write a line about something that should not have happened.
 * @param format the format string, and its arguments
 */
template <typename A, typename... Args>
inline void LogAnomaly(fmt::format_string<A, Args...> format, A &&first_arg, Args &&... other_args)
{
	LogAnomaly(fmt::format(format, std::forward<A>(first_arg), std::forward<Args>(other_args)...));
}

void StartAnomalyLogForGame(std::string_view what);
std::string GetAnomalyLogPath();
uint GetAnomalyLogLines();
bool IsAnomalyLogOn();
void SetAnomalyLogOn(bool on);
void CloseAnomalyLog();

#endif /* ANOMALY_LOG_H */
