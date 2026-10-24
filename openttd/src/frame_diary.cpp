/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file frame_diary.cpp The diary of the frames, snimky.log (frame_diary.h). */

#include "stdafx.h"
#include "frame_diary.h"
#include "fileio_func.h"
#include "window_func.h"
#include "window_type.h"
#include "openttd.h"
#include "three_d_gpu.h"
#include "3rdparty/fmt/format.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <optional>

#if defined(_WIN32)
#	include <windows.h>
#	include <psapi.h>
#elif defined(__linux__)
#	include <unistd.h>
#endif

#include "safeguards.h"

namespace {

constexpr std::chrono::seconds DIARY_EVERY{5}; ///< a line so often
constexpr int DIARY_LINES = 5000;              ///< and no more than so many (about seven hours)

/** A sum of times from another thread, and their longest, microseconds. */
struct Times {
	std::atomic<uint64_t> sum{0};
	std::atomic<uint64_t> longest{0};
	std::atomic<uint32_t> count{0};

	void Add(double ms)
	{
		const uint64_t us = static_cast<uint64_t>(std::max(0.0, ms) * 1000.0);
		this->sum += us;
		this->count++;
		uint64_t was = this->longest.load();
		while (us > was && !this->longest.compare_exchange_weak(was, us)) {}
	}

	/** Its count, average and longest since the last time, milliseconds; started again. */
	std::string Take()
	{
		const uint32_t n = this->count.exchange(0);
		const uint64_t sum = this->sum.exchange(0), longest = this->longest.exchange(0);
		if (n == 0) return "-";
		return fmt::format("{}x prumer {:.0f} ms nejdel {:.0f} ms", n, sum / 1000.0 / n, longest / 1000.0);
	}
};

Times _game_ticks;    ///< the game's ticks (its own thread)
Times _eye_collected; ///< the view from the eyes collected (the main thread)
Times _eye_drawn;     ///< and drawn (its thread)
Times _eye_prepared;  ///< of that on the card: its steps and triangles made ready by the processor
Times _eye_card;      ///< and the card's own work (ThreeDGpuRenderEyeFrame(): sent, drawn, shown)
std::atomic<uint64_t> _eye_corners{0};   ///< the triangles' corners sent to the card, all the frames together
std::atomic<uint32_t> _eye_by_cpu{0};    ///< frames the processor drew (no card, or one the card could not)

/* The frames of the drawing since the last line (the main thread only). */
int _frames = 0;
FrameDiaryParts _sum{};
FrameDiaryParts _longest{};
std::chrono::steady_clock::time_point _since{};
std::optional<FileHandle> _diary;
int _lines = 0;

/** What the player looks at: the view from the eyes, from a cab, or the map. */
const char *Looking()
{
	if (FindWindowById(WindowClass::FirstPerson, 0) != nullptr) return "oci";
	if (FindWindowByClass(WindowClass::CabView) != nullptr) return "kabina";
	return "mapa";
}

void WriteLine(std::chrono::steady_clock::time_point now)
{
	const double secs = std::chrono::duration<double>(now - _since).count();
	if (_lines >= DIARY_LINES || secs <= 0) return;
	if (!_diary.has_value()) {
		_diary = FileHandle::Open(_personal_dir + "snimky.log", "w");
		if (!_diary.has_value()) {
			_lines = DIARY_LINES;
			return;
		}
		fmt::print(*_diary, "OpenTTD Decouple: denik snimku (kazdych 5 s). snimky/s = jak casto hra kresli a cte klavesy; nejdelsi snimek a z ceho byl:"
				" buffer = cekani na obrazovku, zamek = cekani na tik hry, klavesy, okna = okna a jejich kresleni (i sber pohledu z oci), obraz = predani obrazu karte;"
				" tiky hry na svem vlakne; oci sber = sber pohledu z oci na hlavnim vlakne, oci kresleni = na jeho vlakne;"
				" z toho na karte: priprava = procesor chysta trojuhelniky, karta = posilani a kresleni kartou, rohu = kolik rohu trojuhelniku na snimek jde na kartu;"
				" procesorem = snimky kreslene bez karty; pamet = kolik pameti si hra vzala, karta = kolik drzi graficka karta.\n");
	}
	const std::time_t t = std::time(nullptr);
	char clock[16];
	std::strftime(clock, sizeof(clock), "%H:%M:%S", std::localtime(&t));
	const int n = std::max(1, _frames);
	const uint32_t card_frames = _eye_card.count.load();
	const uint64_t corners = _eye_corners.exchange(0);
	fmt::print(*_diary, "{} {} | snimku {:.1f}/s | nejdelsi {:.0f} ms (buffer {:.0f}, zamek {:.0f}, klavesy {:.0f}, okna {:.0f}, obraz {:.0f}) | prumer {:.0f} ms (buffer {:.0f}, zamek {:.0f}, klavesy {:.0f}, okna {:.0f}, obraz {:.0f}) | tiky hry {} | oci sber {} | oci kresleni {} | priprava {} | karta {} | rohu {} | procesorem {}x | pamet {} MB | karta {} MB\n",
			clock, Looking(), _frames / secs, _longest.total, _longest.buffer, _longest.lock, _longest.input, _longest.windows, _longest.paint,
			_sum.total / n, _sum.buffer / n, _sum.lock / n, _sum.input / n, _sum.windows / n, _sum.paint / n,
			_game_ticks.Take(), _eye_collected.Take(), _eye_drawn.Take(), _eye_prepared.Take(), _eye_card.Take(),
			card_frames > 0 ? corners / card_frames : 0, _eye_by_cpu.exchange(0), FrameDiaryMemoryMB(), ThreeDGpuHeldMB());
	fflush(*_diary);
	_lines++;
}

} // namespace

/**
 * How much memory the game holds now, megabytes: what it took of the system for itself (on Windows its private bytes,
 * elsewhere its resident pages); 0 not known. On the player's phone the card shares it (the player, 24. 10.: Winlator
 * gone without a crash log of the game, in the middle of reading pictures back from the card).
 */
size_t FrameDiaryMemoryMB()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX m{};
	m.cb = sizeof(m);
	if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m))) return 0;
	return m.PrivateUsage >> 20;
#elif defined(__linux__)
	auto f = FileHandle::Open(std::string{"/proc/self/statm"}, "r");
	if (!f.has_value()) return 0;
	char text[128];
	const size_t n = fread(text, 1, sizeof(text) - 1, *f);
	/* Its size, then its resident pages. */
	const char *at = std::find(text, text + n, ' ');
	size_t pages = 0;
	if (at == text + n || std::from_chars(at + 1, text + n, pages).ec != std::errc{}) return 0;
	return pages * static_cast<size_t>(sysconf(_SC_PAGESIZE)) >> 20;
#else
	return 0;
#endif
}

/** A frame of the view from the eyes drawn (its thread): on the card, what the processor made ready and what the card did, and how many corners went; else by the processor. */
void FrameDiaryEyeCard(bool on_card, double prepare_ms, double card_ms, size_t corners)
{
	if (!on_card) {
		_eye_by_cpu++;
		return;
	}
	_eye_prepared.Add(prepare_ms);
	_eye_card.Add(card_ms);
	_eye_corners += corners;
}

/** A frame of the drawing done (the main thread): its parts. */
void FrameDiaryDraw(const FrameDiaryParts &parts)
{
	const auto now = std::chrono::steady_clock::now();
	if (_since == std::chrono::steady_clock::time_point{}) _since = now;
	_frames++;
	_sum.buffer += parts.buffer;
	_sum.lock += parts.lock;
	_sum.input += parts.input;
	_sum.windows += parts.windows;
	_sum.paint += parts.paint;
	_sum.total += parts.total;
	if (parts.total > _longest.total) _longest = parts;
	if (now - _since < DIARY_EVERY) return;
	WriteLine(now);
	_frames = 0;
	_sum = {};
	_longest = {};
	_since = now;
}

/** A tick of the game done (its own thread), milliseconds. */
void FrameDiaryGameTick(double ms)
{
	_game_ticks.Add(ms);
}

/** The view from the eyes collected (the main thread), milliseconds. */
void FrameDiaryEyeCollected(double ms)
{
	_eye_collected.Add(ms);
}

/** And drawn (its thread), milliseconds. */
void FrameDiaryEyeDrawn(double ms)
{
	_eye_drawn.Add(ms);
}
