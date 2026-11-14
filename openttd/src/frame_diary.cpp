/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
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

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#	include <windows.h>
#	include <psapi.h>
#elif defined(__linux__)
#	include <unistd.h>
#endif

#include "safeguards.h"

std::string TakeTurnedTimesForDiary(int frames); // viewport.cpp
extern double _object_picture_ms, _object_picture_thread_ms, _likeness_picture_ms;
extern uint64_t _object_pictures_made, _object_pictures_made_thread, _likeness_pictures_made;

/**
 * The pictures of 3D objects made since the last line (the vehicles' on the main thread and on theirs, the likenesses),
 * and what the card did: the player's log of 25. 10. had the card hold more and more (268 to 882 MB) while a few things
 * moved at 4 frames a second -- whether pictures are made again and again shows here.
 */
static std::string TakePicturesForDiary()
{
	/* What the main thread read itself (a shape, a texture of a 3D object) or waited for: a click held up by it (#329). */
	extern std::atomic<uint64_t> _three_d_main_made;
	extern std::atomic<int64_t> _three_d_main_made_us, _three_d_main_waited_us, _three_d_main_longest_us;
	extern std::string TakeThreeDMainKinds();
	std::string s = fmt::format("objektu {} ({:.0f} ms), na vlakne {} ({:.0f} ms), podoby {} ({:.0f} ms); nacteno hlavnim vlaknem {} ({:.0f} ms, cekani {:.0f} ms, nejdel {:.0f} ms: {}); {}", _object_pictures_made, _object_picture_ms,
			_object_pictures_made_thread, _object_picture_thread_ms, _likeness_pictures_made, _likeness_picture_ms,
			_three_d_main_made.exchange(0), _three_d_main_made_us.exchange(0) / 1000.0, _three_d_main_waited_us.exchange(0) / 1000.0, _three_d_main_longest_us.exchange(0) / 1000.0, TakeThreeDMainKinds(), ThreeDGpuTimes());
	_object_picture_ms = _object_picture_thread_ms = _likeness_picture_ms = 0;
	_object_pictures_made = _object_pictures_made_thread = _likeness_pictures_made = 0;
	return s;
}

/**
 * What the 3D objects hold (DescribeThreeDObjectMemory()) and their pictures (DescribeThreeDPictureMemory()), and how
 * often what was not drawn for two minutes was let go of, or had to wait (ForgetUnusedThreeDObjectsNow()): the player's
 * log of 2. 11. had the memory grow from the eyes to 3.2 GB, Android killing the game, and no word of what it was.
 */
static std::string TakeThreeDMemoryForDiary()
{
	extern std::string DescribeThreeDObjectMemory();
	extern std::string DescribeThreeDPictureMemory();
	extern uint64_t _three_d_forgets_done, _three_d_forgets_put_off;
	extern void TakeSpriteCacheChurn(uint64_t &dropped, double &dropped_mb, uint64_t &reloaded, double &reload_ms);
	uint64_t dropped = 0, reloaded = 0;
	double dropped_mb = 0, reload_ms = 0;
	TakeSpriteCacheChurn(dropped, dropped_mb, reloaded, reload_ms);
	std::string s = fmt::format("{}; {}, vyhozeno {} ({:.1f} MB), znovu nacteno {} ({:.0f} ms); zapomenuti {}, odlozeno {}", DescribeThreeDObjectMemory(), DescribeThreeDPictureMemory(),
			dropped, dropped_mb, reloaded, reload_ms, _three_d_forgets_done, _three_d_forgets_put_off);
	_three_d_forgets_done = _three_d_forgets_put_off = 0;
	return s;
}

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
/* The parts of "okna" (the player's #299 log: from the eyes, "okna" was ~220 ms a frame while the eyes' own work was
 * a tenth of that -- where the rest goes was not written anywhere): the windows' clocks (the walker's step in them),
 * the dirty pieces of the screen drawn, of that the map's blocks, the canvas of the view from the eyes, and in the
 * map's blocks the wait for the card laying the ground. */
Times _windows_timers; ///< the windows' clocks (TimerWindow, the realtime ticks)
Times _windows_blocks; ///< the dirty pieces of the screen drawn (DrawDirtyBlocks())
Times _map_blocks;     ///< of them, the map's blocks (ViewportDoDraw())
Times _eye_window;     ///< and the canvas of the view from the eyes (its window's DrawWidget)
Times _ground_waits;   ///< waits for the card laying the ground of the turned map (FinishTurnedGround())
Times _eye_collected; ///< the view from the eyes collected (the main thread)
Times _eye_drawn;     ///< and drawn (its thread)
Times _eye_prepared;  ///< of that on the card: its steps and triangles made ready by the processor
Times _eye_card;      ///< and the card's own work (ThreeDGpuRenderEyeFrame(): sent, drawn, shown)
/* The dirty pieces drawn by the kind of their window (the player's log of 5. 11.: buying in a depot with twenty
 * windows of vehicles open, "okna" 100 to 600 ms a frame, the map's blocks a millisecond -- which windows it was, not
 * written): the pieces each kind painted and how long (Window::OnPaint() in DrawOverlappedWindow()). */
constexpr size_t PAINT_KINDS = 1024;
std::array<uint32_t, PAINT_KINDS> _paint_count{};
std::array<double, PAINT_KINDS> _paint_ms{};
std::atomic<uint64_t> _eye_corners{0};   ///< the triangles' corners sent to the card, all the frames together
std::atomic<uint32_t> _eye_by_cpu{0};    ///< frames the processor drew (no card, or one the card could not)

/* The frames of the drawing since the last line (the main thread only). */
int _frames = 0;
uint32_t _most_messages = 0; ///< the most system's messages a frame handled
FrameDiaryParts _sum{};
FrameDiaryParts _longest{};
std::chrono::steady_clock::time_point _since{};
std::optional<FileHandle> _diary;
int _lines = 0;

/** The name of a kind of window in the diary: the ones a slow frame was seen in by name, the others by number. */
std::string KindName(size_t kind)
{
	switch (static_cast<WindowClass>(kind)) {
		case WindowClass::MainWindow: return "mapa";
		case WindowClass::VehicleView: return "vozidlo";
		case WindowClass::VehicleDepot: return "depo";
		case WindowClass::BuildVehicle: return "nakup";
		case WindowClass::VehicleDetails: return "podrobnosti";
		case WindowClass::VehicleRefit: return "prestavba";
		case WindowClass::VehicleOrders: return "rozkazy";
		case WindowClass::VehicleTimetable: return "jizdni rad";
		case WindowClass::MainToolbar: return "lista";
		case WindowClass::Statusbar: return "stav";
		case WindowClass::News: return "zpravy";
		case WindowClass::FirstPerson: return "oci";
		default: return fmt::format("druh {}", kind);
	}
}

/** The kinds of windows that painted the longest since the last line (the five longest), started again. */
std::string TakeWindowPaints()
{
	std::vector<size_t> kinds;
	for (size_t k = 0; k < PAINT_KINDS; k++) {
		if (_paint_count[k] > 0) kinds.push_back(k);
	}
	std::ranges::sort(kinds, [](size_t a, size_t b) { return _paint_ms[a] > _paint_ms[b]; });
	std::string out;
	for (size_t i = 0; i < kinds.size() && i < 5; i++) {
		out += fmt::format("{}{} {}x {:.0f} ms", i == 0 ? "" : ", ", KindName(kinds[i]), _paint_count[kinds[i]], _paint_ms[kinds[i]]);
	}
	_paint_count.fill(0);
	_paint_ms.fill(0);
	return out.empty() ? std::string("-") : out;
}

/** What the player looks at: the view from the eyes, from a cab, or the map. */
const char *Looking()
{
	if (FindWindowById(WindowClass::FirstPerson, 0) != nullptr) return "oci";
	return "mapa";
}

void WriteLine(std::chrono::steady_clock::time_point now)
{
	const double secs = std::chrono::duration<double>(now - _since).count();
	if (_lines >= DIARY_LINES || secs <= 0) return;
	if (!_diary.has_value()) {
		_diary = FileHandle::Open(FioDiaryPath("snimky.log"), "w");
		if (!_diary.has_value()) {
			_lines = DIARY_LINES;
			return;
		}
		fmt::print(*_diary, "OpenTTD Decouple: denik snimku (kazdych 5 s). snimky/s = jak casto hra kresli a cte klavesy; nejdelsi snimek a z ceho byl:"
				" buffer = cekani na obrazovku, zamek = cekani na tik hry, klavesy, okna = okna a jejich kresleni (i sber pohledu z oci), obraz = predani obrazu karte;"
				" zpravy = zpravy systemu (mys, klavesy) v klavesach: kolik na snimek a za kolik ms, odlozeno = snimky, ktere zbytek zprav nechaly dalsimu (jejich cas vyprsel);"
				" rozpad okna: hodiny = hodiny oken (i krok chodce z oci se sberem), bloky = spinave kusy obrazovky kreslene, mapa = bloky herni plochy,"
				" okno oci = vykresleni platna z oci do okna, zem = cekani na kartu u zeme otocene plochy; druhy oken = ktera okna kreslila nejdel (kusu a ms za 5 s, mapa i s herni plochou);"
				" tiky hry na svem vlakne; oci sber = sber pohledu z oci na hlavnim vlakne, oci kresleni = na jeho vlakne;"
				" z toho na karte: priprava = procesor chysta trojuhelniky, karta = posilani a kresleni kartou, rohu = kolik rohu trojuhelniku na snimek jde na kartu;"
				" procesorem = snimky kreslene bez karty; pamet = kolik pameti si hra vzala, karta = kolik drzi graficka karta;"
				" otocena = kresleni otocene mapy na snimek: sprity do textur, stopy domu, polozeni zeme, sber, plakaty, modely (domy kreslene procesorem), a kolik bloku vzalo zem z drzeneho pohledu; zmen naraz = zmeny hlavni mapy polozene kartou jednim kusem (a proc ne), po kouscich = v kolika obdelnicich z nich karta kreslila (a kolik procent plochy kusu), v malych oknech = totez v oknech vozidel a dalsich pohledech; stojici procesorem = bloky, kde co stoji kreslil procesor (bez zeme karty, nebo karta vratila; proc v obrazky: what stands on);"
				" obrazky = kolik obrazku 3D objektu se udelalo (vozidla, podoby) a co delala karta, nacteno hlavnim vlaknem = tvary a textury 3D objektu, ktere si hlavni vlakno nacetlo samo nebo na ne cekalo (to drzi klik i snimek);"
				" pamet 3d = co drzi 3D objekty (tvary, textury, domy) a obrazky; cache spritu: kolik drzi z nastavenych MB, vyhozeno = sprity vyhozene z plne cache, znovu nacteno = kolik z vyhozenych se muselo nacist znovu a jak dlouho (cache je mala, kdyz je to hodne);"
				" zapomenuti = kolikrat se pustilo, co se 2 minuty nekreslilo, odlozeno = kolikrat to muselo pockat.\n");
	}
	const std::time_t t = std::time(nullptr);
	char clock[16];
	std::strftime(clock, sizeof(clock), "%H:%M:%S", std::localtime(&t));
	const int n = std::max(1, _frames);
	const uint32_t card_frames = _eye_card.count.load();
	const uint64_t corners = _eye_corners.exchange(0);
	fmt::print(*_diary, "{} {} | snimku {:.1f}/s | nejdelsi {:.0f} ms (buffer {:.0f}, zamek {:.0f}, klavesy {:.0f}, okna {:.0f}, obraz {:.0f}) | prumer {:.0f} ms (buffer {:.0f}, zamek {:.0f}, klavesy {:.0f}, okna {:.0f}, obraz {:.0f}) | zpravy {:.1f} na snimek {:.1f} ms, nejvic {}, nejdelsi snimek {} za {:.0f} ms, odlozeno {}x | hodiny {} | bloky {} | mapa {} | okno oci {} | zem {} | druhy oken {} | tiky hry {} | oci sber {} | oci kresleni {} | priprava {} | karta {} | rohu {} | procesorem {}x | pamet {} MB | karta {} MB | otocena {} | obrazky {} | pamet 3d {}\n",
			clock, Looking(), _frames / secs, _longest.total, _longest.buffer, _longest.lock, _longest.input, _longest.windows, _longest.paint,
			_sum.total / n, _sum.buffer / n, _sum.lock / n, _sum.input / n, _sum.windows / n, _sum.paint / n,
			static_cast<double>(_sum.messages) / n, _sum.messages_ms / n, _most_messages, _longest.messages, _longest.messages_ms, _sum.messages_cut,
			_windows_timers.Take(), _windows_blocks.Take(), _map_blocks.Take(), _eye_window.Take(), _ground_waits.Take(), TakeWindowPaints(),
			_game_ticks.Take(), _eye_collected.Take(), _eye_drawn.Take(), _eye_prepared.Take(), _eye_card.Take(),
			card_frames > 0 ? corners / card_frames : 0, _eye_by_cpu.exchange(0), FrameDiaryMemoryMB(), ThreeDGpuHeldMB(), TakeTurnedTimesForDiary(_frames), TakePicturesForDiary(), TakeThreeDMemoryForDiary());
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
	if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m))) return 0;
	/* Wine gives no private bytes (the player's phone, 26. 10.: always 0): its committed ones, else its working set. */
	if (m.PrivateUsage != 0) return m.PrivateUsage >> 20;
	if (m.PagefileUsage != 0) return m.PagefileUsage >> 20;
	return m.WorkingSetSize >> 20;
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
	_sum.messages_ms += parts.messages_ms;
	_sum.messages += parts.messages;
	_sum.messages_cut += parts.messages_cut;
	_most_messages = std::max(_most_messages, parts.messages);
	if (parts.total > _longest.total) _longest = parts;
	if (now - _since < DIARY_EVERY) return;
	WriteLine(now);
	_frames = 0;
	_sum = {};
	_longest = {};
	_most_messages = 0;
	_since = now;
}

/** A tick of the game done (its own thread), milliseconds. */
void FrameDiaryGameTick(double ms)
{
	_game_ticks.Add(ms);
}

/** The windows' clocks run (TimerWindow and the realtime ticks; the main thread), milliseconds. */
void FrameDiaryWindowsTimers(double ms)
{
	_windows_timers.Add(ms);
}

/** The dirty pieces of the screen drawn (DrawDirtyBlocks(); the main thread), milliseconds. */
void FrameDiaryWindowsBlocks(double ms)
{
	_windows_blocks.Add(ms);
}

/** A dirty piece of a window painted (DrawOverlappedWindow(); the main thread), milliseconds. */
void FrameDiaryWindowPaint(uint16_t window_class, double ms)
{
	if (window_class >= PAINT_KINDS) return;
	_paint_count[window_class]++;
	_paint_ms[window_class] += ms;
}

/** The kinds of windows that painted the longest since it was last asked (the rig's 'smycka'), started again. */
std::string FrameDiaryTakeWindowPaints()
{
	return TakeWindowPaints();
}

/** A block of the map drawn (ViewportDoDraw(); the main thread), milliseconds. */
void FrameDiaryMapBlock(double ms)
{
	_map_blocks.Add(ms);
}

/** The canvas of the view from the eyes drawn into its window (the main thread), milliseconds. */
void FrameDiaryEyeWindow(double ms)
{
	_eye_window.Add(ms);
}

/** A wait for the card laying the ground of a block of the turned map (the main thread), milliseconds. */
void FrameDiaryGroundWait(double ms)
{
	_ground_waits.Add(ms);
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
