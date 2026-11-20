/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file three_d_gpu.cpp The 3D objects drawn on the graphics card (three_d_gpu.h). */

#include "stdafx.h"
#include "three_d_gpu.h"
#include "three_d_objects.h"
#include "gfx_type.h"
#include "debug.h"
#include "thread.h"
#include "fileio_func.h"
#include "settings_type.h"
#include "error.h"
#include "strings_func.h"
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "table/strings.h"
#include "frame_diary.h"
#include "tile_type.h"

#include <algorithm>
#include <optional>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <thread>
#include <ctime>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32) && defined(WITH_OPENGL)
#	define THREE_D_GPU_WGL
#	include <windows.h>
#elif !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#	define THREE_D_GPU_EGL
#	include "library_loader.h"
#endif

#include "safeguards.h"

/**
 * The textures the card holds, by their numbers, for the game's threads to ask (ThreeDGpuHoldsTexture()): one it holds
 * needs its pixels in the memory no more (TrimTextures() of three_d_objects.cpp lets them go; the player's logs of
 * 15. 11.: the vehicles' textures 575 MB in the memory and 543 MB on the card, the same pictures twice). The mutex is
 * held by the card's thread while it reads a texture's pixels to put it on, and by whoever lets pixels go.
 *
 * The numbers themselves under a mutex of their own, held only while they are read or changed (after the other, when
 * both): who only asks whether the card holds one (ThreeDGpuHoldsTexture(): the main thread, of every texture of every
 * building it gives the card) does not wait for a texture being put on. It did -- the card's thread holds the first
 * mutex all the while it sends a texture's pixels, on the phone tens of milliseconds each --, so while the card took
 * new textures the main thread stood at each building (the player's diary of 20. 11., #384, the far map by night after
 * the view from the eyes: "budovy 59.1 ms (242x)" a frame, the card letting go 20 to 31 times in 5 s, 5 to 8 frames a
 * second; with the card at rest 0.04 ms a building).
 */
static std::mutex _card_texture_mutex;
static std::mutex _card_texture_uids_mutex;
static std::unordered_set<uint64_t> _card_texture_uids;

std::unique_lock<std::mutex> ThreeDGpuTexturesLock()
{
	return std::unique_lock<std::mutex>(_card_texture_mutex);
}

/** Whether the card holds a texture; under ThreeDGpuTexturesLock(). */
bool ThreeDGpuHoldsTextureLocked(uint64_t uid)
{
	std::lock_guard uids_lock(_card_texture_uids_mutex);
	return _card_texture_uids.contains(uid);
}

bool ThreeDGpuHoldsTexture(uint64_t uid)
{
	std::lock_guard uids_lock(_card_texture_uids_mutex);
	return _card_texture_uids.contains(uid);
}

/**
 * All the card holds by number, for the main thread to ask before it gives what stands on a block without waiting
 * (ThreeDGpuDrawStandingAsync()): the keys of _card_textures (textures, patterns, masks) and of _card_buildings. Set
 * by the card's thread as it puts them on and lets them go (CardHeld()).
 */
static std::mutex _card_held_mutex;
static std::unordered_set<uint64_t> _card_held_textures, _card_held_buildings;

static void CardHeld(std::unordered_set<uint64_t> &held, uint64_t key, bool on)
{
	std::lock_guard lock(_card_held_mutex);
	if (on) {
		held.insert(key);
	} else {
		held.erase(key);
	}
}

/**
 * The jobs of what stands given without waiting and not done yet (ThreeDGpuDrawStandingAsync()): while there are
 * any, the card lets nothing go (TrimCard()) -- what the main thread found it holds is there when the job comes. The
 * mutex is held by TrimCard() and by the main thread as it looks and counts one more.
 */
static std::atomic<int> _standing_async_pending = 0;
static std::mutex _card_trim_mutex;
/** For the diary: the vehicles of a job given without waiting whose kept pictures went meanwhile (DrawStandingOnCard()). */
static std::atomic<uint64_t> _standing_async_missed = 0;
/** The rig ('test3d stojici odmitnout <n>'): so many jobs given without waiting the card does not lay, to see them drawn again. */
std::atomic<int> _rig_standing_refuse = 0;
/** The size of the pictures what stands is laid from (_standing_atlas_size) for the main thread: 0 not made yet. */
static std::atomic<int> _standing_atlas_size_now = 0;

std::atomic<bool> _three_d_gpu_off = true;
std::atomic<bool> _three_d_gpu_show = true;
/** The rig ('test3d ocidomy 0|1'): the buildings of a picture from the eyes drawn those alike one after the other, their runs side by side (1; DrawEyeObjects()), or as they were gathered (0, as before 20. 11.). */
bool _eye_buildings_by_kind = true;
/** The rig ('test3d ocidavky 0|1'): the runs of the triangles of a picture from the eyes drawn texture by texture, each texture's by one call (1; DrawEyeOnCard()), or in the order they were gathered (0, as before 20. 11.). */
bool _eye_runs_by_texture = true;
/** The rig ('test3d stojici delit 0|1'): a free place bigger than asked for split, what is left free again (1; TakeFreeStandingPlace()), or taken whole (0, as before 21. 11.). */
bool _standing_free_split = true;


/*
 * The card's diary, 3d_karta.log in the player's folder. The player's build #285, the first with the card, brought the
 * whole of Winlator down twice -- its OpenGL runs in the app itself, and nothing of the game's own crash log is left
 * then. So each step with the card is written before it is taken and flushed to the disc: the last line says at what
 * it died. The start and the first pictures step by step one line after the other; then only the step of the moment,
 * on the last line again and again. "ceka" while the card waits, "konec" at the end of the game: a last line of a step
 * at the next start (ThreeDGpuCheckLastRun()) has the processor draw, with a word why, the diary kept as
 * 3d_karta_minule.log to send.
 */
static std::optional<FileHandle> _diary;
static long _diary_tail = 0;       ///< where the line of the moment is written
static int _diary_pictures = 0;    ///< the pictures written step by step so far
static constexpr int DIARY_PICTURES = 30;
static constexpr size_t DIARY_WIDTH = 120;
static std::string _gpu_died_at;   ///< the step the card died at the last time, empty when it did not
static bool _gpu_died_note = false; ///< the player not told of it yet
/* The card's context not sharing the video driver's: making it so brought the game down once (ThreeDGpuCheckLastRun()),
 * and so for good -- each diary says it again (SHARE_OFF) for the next start. The view from the eyes is read back. */
static bool _gpu_no_share = false;
static constexpr std::string_view SHARE_OFF = "sdileni s ovladacem videa vypnuto (pri nem hra jednou spadla)";

static std::string DiaryPath(bool last)
{
	return FioDiaryPath(last ? "3d_karta_minule.log" : "3d_karta.log");
}

/** A line of the diary: kept, or the line of the moment, written over by the next. */
[[maybe_unused]] static void Diary(std::string_view line, bool keep)
{
	if (!_diary.has_value()) return;
	std::string text(line.substr(0, DIARY_WIDTH));
	text.resize(DIARY_WIDTH, ' ');
	text += '\n';
	fseek(*_diary, _diary_tail, SEEK_SET);
	fwrite(text.data(), 1, text.size(), *_diary);
	fflush(*_diary);
	if (keep) _diary_tail = ftell(*_diary);
}

/* The watch of a freeze (WatchFreeze()): the step the card's thread took last and when, steady milliseconds. */
static std::mutex _watch_step_mutex;
static std::string _watch_step;
static std::atomic<int64_t> _watch_step_at = 0;
static int64_t WatchNow()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** A step with the card about to be taken: kept while the start and the first pictures are written step by step. */
[[maybe_unused]] static void Step(std::string_view what)
{
	{
		std::lock_guard lock(_watch_step_mutex);
		_watch_step = what;
	}
	_watch_step_at = WatchNow();
	Diary(fmt::format("krok {}", what), _diary_pictures < DIARY_PICTURES);
}

/**
 * Whether the card brought the game down the last time (the last line of the diary a step): then the processor draws
 * the 3D pictures, the setting switched off -- the player switches it on again to try -- and the player is told at
 * what it died; the diary kept to send. Called at the start, the settings read.
 */
void ThreeDGpuCheckLastRun()
{
	auto f = FileHandle::Open(DiaryPath(false), "rb");
	if (!f.has_value()) return;
	std::string text;
	char buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof(buffer), *f)) > 0) text.append(buffer, n);
	f->Close();
	/* Its last line not of spaces. */
	std::string last;
	size_t end = text.size();
	while (end > 0) {
		size_t start = text.rfind('\n', end - 1);
		start = start == std::string::npos ? 0 : start + 1;
		std::string line = text.substr(start, end - start);
		while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (!line.empty()) {
			last = line;
			break;
		}
		if (start == 0) break;
		end = start - 1;
	}
	if (text.find(SHARE_OFF) != std::string::npos) _gpu_no_share = true;
	if (!last.starts_with("krok ")) return;
	if (auto kept = FileHandle::Open(DiaryPath(true), "wb"); kept.has_value()) fwrite(text.data(), 1, text.size(), *kept);
	if (last.find("sdileny") != std::string::npos) {
		/* At making its context share the video driver's (the view from the eyes shown straight from the card): the card
		 * stays on, its context its own from now on. */
		_gpu_no_share = true;
		Debug(misc, 0, "3D on the graphics card: the game went down the last time at \"{}\"; the card's context is its own from now on (3d_karta_minule.log)", last.substr(5));
		return;
	}
	/* The game left in the middle of a step after the card drew its first pictures: the player closing the whole of
	 * Winlator leaves it so too (the player, 20. 10.: "I close it by closing the whole Winlator") -- the card stays on,
	 * the diary kept to send. Only a card that died before its first pictures were drawn is switched off: else the game
	 * might never get as far as its settings. */
	size_t drawn = 0;
	for (size_t at = text.find(" hotov ("); at != std::string::npos; at = text.find(" hotov (", at + 1)) drawn++;
	if (drawn >= DIARY_PICTURES) {
		Debug(misc, 1, "3D on the graphics card: the game ended the last time at \"{}\", after the card drew its first pictures; it stays on (3d_karta_minule.log)", last.substr(5));
		return;
	}
	_gpu_died_at = last.substr(5);
	_gpu_died_note = true;
	_settings_client.gui.three_d_gpu = false;
	_three_d_gpu_off = true;
	Debug(misc, 0, "3D on the graphics card: it brought the game down the last time, at \"{}\"; the processor draws (3d_karta_minule.log)", _gpu_died_at);
}

/** The player told once, in a window, that the card brought the game down the last time and at what. */
static const IntervalTimer<TimerWindow> _gpu_died_timer(std::chrono::seconds(2), [](auto) {
	if (!_gpu_died_note || FindWindowById(WindowClass::MainWindow, 0) == nullptr) return;
	_gpu_died_note = false;
	Debug(misc, 1, "3D on the graphics card: the player told it brought the game down the last time");
	ShowErrorMessage(GetEncodedString(STR_ERROR_THREE_D_GPU_DIED, _gpu_died_at), {}, WarningLevel::Warning);
});


/* The triangles of the shapes by the parts shown (ThreeDGpuTrianglesOf()), kept for the pictures to come. */
static std::mutex _triangles_mutex;
static std::map<std::tuple<uint64_t, ThreeDParts, bool, bool>, std::pair<std::shared_ptr<const ThreeDGpuTriangles>, uint64_t>> _triangles_kept;
static uint64_t _triangles_clock = 0;
static constexpr size_t TRIANGLES_KEPT = 512;

/**
 * The triangles of a shape the card draws, as RasterObject() of three_d_vehicle_object.cpp draws them: of a vehicle
 * those of the parts its state shows (the driver too), of its crew all of them; the see-through (the glass, a material
 * "BLEND") after the rest.
 * @param parts the parts shown (ThreeDVehicleObject::PartsOf())
 * @param vehicle of the vehicle, else of a figure of its crew
 * @param textures how many textures it has (a simpler shape's are its whole shape's)
 */
std::shared_ptr<const ThreeDGpuTriangles> ThreeDGpuTrianglesOf(const ThreeDVehicleShape &shape, const ThreeDParts &parts, bool vehicle, size_t textures, bool without_driver_seat)
{
	const auto key = std::make_tuple(shape.uid.value, vehicle ? parts : ThreeDParts{}, vehicle, without_driver_seat);
	{
		std::lock_guard lock(_triangles_mutex);
		auto it = _triangles_kept.find(key);
		if (it != _triangles_kept.end()) {
			it->second.second = ++_triangles_clock;
			return it->second.first;
		}
	}
	auto made = std::make_shared<ThreeDGpuTriangles>();
	made->uid = ThreeDUniqueId::Next();
	textures = std::max<size_t>(1, textures);
	const size_t corners = shape.pos.size() / 3;
	made->used.assign(textures, false);
	made->indices.reserve(shape.tri.size());
	for (int pass = 0; pass < 2; pass++) {
		std::vector<ThreeDGpuTriangles::Run> &runs = pass == 0 ? made->opaque : made->glass;
		for (size_t t = 0; t + 2 < shape.tri.size(); t += 3) {
			const size_t tn = t / 3;
			if (vehicle) {
				const uint8_t part = tn < shape.tri_part.size() ? shape.tri_part[tn] : 0;
				if (part != ThreeDVehicleShape::PART_GLASS && !ThreeDVehicleShape::Shown(part, parts)) continue;
				/* The cab the eye is in: the driver's seat and those by it, where the eye is, left out. */
				if (without_driver_seat && ThreeDVehicleShape::AtDriverSeat(part)) continue;
			}
			const bool glass = shape.IsSeeThrough(tn);
			if (glass != (pass == 1)) continue;
			if (shape.tri[t] >= corners || shape.tri[t + 1] >= corners || shape.tri[t + 2] >= corners) continue;
			uint32_t ti = tn < shape.tri_tex.size() ? shape.tri_tex[tn] : 0;
			if (ti >= textures) ti = 0;
			const int8_t facing = shape.Facing(tn);
			const uint32_t at = static_cast<uint32_t>(made->indices.size());
			if (!runs.empty() && runs.back().texture == ti && runs.back().facing == facing && runs.back().first + runs.back().count == at) {
				runs.back().count += 3;
			} else {
				runs.push_back({at, 3, ti, facing});
			}
			made->indices.insert(made->indices.end(), {shape.tri[t], shape.tri[t + 1], shape.tri[t + 2]});
			made->used[ti] = true;
		}
	}
	std::lock_guard lock(_triangles_mutex);
	auto [it, added] = _triangles_kept.emplace(key, std::make_pair(std::shared_ptr<const ThreeDGpuTriangles>(std::move(made)), ++_triangles_clock));
	if (!added) return it->second.first; // another thread was first
	if (_triangles_kept.size() > TRIANGLES_KEPT) {
		/* The half not drawn the longest let go of (a picture drawing from one holds it still). */
		const uint64_t keep_from = _triangles_clock - TRIANGLES_KEPT / 2;
		std::erase_if(_triangles_kept, [keep_from](const auto &e) { return e.second.second < keep_from; });
	}
	return _triangles_kept.at(key).first;
}

/** The rig ('test3d gpu mazani 1'): the rects of the card's pictures cleared by drawing over them whatever its glClear() does (ClearInScissor()). */
bool _clear_by_drawing = false;
/** The rig ('test3d gpu instance 0'): the meshes alike of the view from the eyes drawn at once by instances (EyeObjectDraws::Batch); off, each on its own. */
bool _eye_instances_on = true;
/** The rig ('test3d maskykarty 0'): the masks of the vehicles' paint on the card with all four channels of their texels, as their textures (as before 18. 11.; MaskOnCard()). */
bool _gpu_masks_by_channel = true;
/** The rig ('test3d gpu kosti 0|1'): the card poses the figures on bones where it can (true), or the processor their frames (false), to compare. */
std::atomic<bool> _card_poses_bones = true;

#if defined(THREE_D_GPU_WGL) || defined(THREE_D_GPU_EGL)

/** The side a surface's normal is taken to among the buildings the card paints on the map (viewport.cpp). */
extern float _standing_paint_turn;

namespace {

/* What of OpenGL is used, by its own names: no header of it is needed. */
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLdouble = double;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLsync = struct __GLsync *;

constexpr GLboolean GL_FALSE = 0;
constexpr GLboolean GL_TRUE = 1;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_ZERO = 0;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_POINTS = 0x0000;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x0100;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_GREATER = 0x0204;
constexpr GLenum GL_GEQUAL = 0x0206;
constexpr GLenum GL_ALWAYS = 0x0207;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_FRONT = 0x0404;
constexpr GLenum GL_CW = 0x0900;
constexpr GLenum GL_CCW = 0x0901;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_OUT_OF_MEMORY = 0x0505;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_DITHER = 0x0BD0;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum GL_DEPTH_FUNC = 0x0B74;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_PACK_ROW_LENGTH = 0x0D02;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum GL_RED = 0x1903;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_BGRA = 0x80E1;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_RG = 0x8227;
constexpr GLenum GL_R8 = 0x8229;
constexpr GLenum GL_RG8 = 0x822B;
constexpr GLenum GL_R32F = 0x822E;
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE1 = 0x84C1;
constexpr GLenum GL_TEXTURE2 = 0x84C2;
constexpr GLenum GL_TEXTURE3 = 0x84C3;
constexpr GLenum GL_TEXTURE5 = 0x84C5;
constexpr GLenum GL_MAX_RENDERBUFFER_SIZE = 0x84E8;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STREAM_DRAW = 0x88E0;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_COLOR_ATTACHMENT1 = 0x8CE1;
constexpr GLenum GL_COLOR_ATTACHMENT2 = 0x8CE2;
/** What stands on a view drawn into (ViewPictures::standing_fbo): its colours, which are lit, its order. */
constexpr GLenum STANDING_DRAW_BUFFERS[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;

#ifdef THREE_D_GPU_WGL
#	define THREE_D_GL_API APIENTRY
#else
#	define THREE_D_GL_API
#endif

#define THREE_D_GL_FUNCTIONS(X) \
	X(const GLubyte *, glGetString, (GLenum name)) \
	X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
	X(GLenum, glGetError, ()) \
	X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
	X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
	X(void, glClearDepth, (GLdouble d)) \
	X(void, glClear, (GLbitfield mask)) \
	X(void, glEnable, (GLenum cap)) \
	X(void, glDisable, (GLenum cap)) \
	X(void, glDepthFunc, (GLenum func)) \
	X(void, glDepthMask, (GLboolean flag)) \
	X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
	X(void, glFrontFace, (GLenum mode)) \
	X(void, glCullFace, (GLenum mode)) \
	X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor)) \
	X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices)) \
	X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
	X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data)) \
	X(void, glFinish, ()) \
	X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h)) \
	X(void, glPixelStorei, (GLenum pname, GLint param)) \
	X(void, glReadBuffer, (GLenum src)) \
	X(void, glDrawBuffers, (GLsizei n, const GLenum *bufs)) \
	X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
	X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
	X(void, glBindTexture, (GLenum target, GLuint texture)) \
	X(void, glActiveTexture, (GLenum unit)) \
	X(void, glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *data)) \
	X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data)) \
	X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
	X(void, glGenFramebuffers, (GLsizei n, GLuint *fbos)) \
	X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *fbos)) \
	X(void, glBindFramebuffer, (GLenum target, GLuint fbo)) \
	X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb)) \
	X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
	X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
	X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
	X(void, glGenRenderbuffers, (GLsizei n, GLuint *rbs)) \
	X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *rbs)) \
	X(void, glBindRenderbuffer, (GLenum target, GLuint rb)) \
	X(void, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h)) \
	X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
	X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
	X(void, glBindVertexArray, (GLuint array)) \
	X(void, glGenerateMipmap, (GLenum target)) \
	X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
	X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers)) \
	X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
	X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
	X(void, glEnableVertexAttribArray, (GLuint index)) \
	X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
	X(void, glVertexAttrib3f, (GLuint index, GLfloat x, GLfloat y, GLfloat z)) \
	X(void, glDisableVertexAttribArray, (GLuint index)) \
	X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor)) \
	X(void, glDrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount)) \
	X(GLuint, glCreateShader, (GLenum type)) \
	X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *source, const GLint *length)) \
	X(void, glCompileShader, (GLuint shader)) \
	X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *param)) \
	X(void, glGetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei *length, GLchar *log)) \
	X(void, glDeleteShader, (GLuint shader)) \
	X(GLuint, glCreateProgram, ()) \
	X(void, glAttachShader, (GLuint program, GLuint shader)) \
	X(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name)) \
	X(void, glLinkProgram, (GLuint program)) \
	X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *param)) \
	X(void, glGetProgramInfoLog, (GLuint program, GLsizei max, GLsizei *length, GLchar *log)) \
	X(void, glDeleteProgram, (GLuint program)) \
	X(void, glUseProgram, (GLuint program)) \
	X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name)) \
	X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)) \
	X(void, glUniform2f, (GLint location, GLfloat x, GLfloat y)) \
	X(void, glUniform3f, (GLint location, GLfloat x, GLfloat y, GLfloat z)) \
	X(void, glUniform4f, (GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) \
	X(void, glUniform1i, (GLint location, GLint v)) \
	X(void, glUniform2i, (GLint location, GLint x, GLint y)) \
	X(void, glUniform1f, (GLint location, GLfloat v)) \
	X(void, glUniform1fv, (GLint location, GLsizei count, const GLfloat *value)) \
	X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *value))

/** The functions of OpenGL, of the card's context (only its thread calls them). */
struct GlFunctions {
#define THREE_D_GL_MEMBER(ret, name, args) ret (THREE_D_GL_API *name) args = nullptr;
	THREE_D_GL_FUNCTIONS(THREE_D_GL_MEMBER)
#undef THREE_D_GL_MEMBER
	/* Not every OpenGL has them (3.2): without, glFinish() (ViewPicturesLaid()). */
	GLsync (THREE_D_GL_API *glFenceSync)(GLenum condition, GLbitfield flags) = nullptr;
	/* Without, a glDrawArrays() for each range (DrawStandingOnCard()). */
	void (THREE_D_GL_API *glMultiDrawArrays)(GLenum mode, const GLint *first, const GLsizei *count, GLsizei drawcount) = nullptr;
	void (THREE_D_GL_API *glDeleteSync)(GLsync sync) = nullptr;
	void (THREE_D_GL_API *glFlush)() = nullptr;
};
GlFunctions gl;

/**
 * For the diary: the calls of OpenGL the card's thread made, all and in the jobs laying the turned map (the player's
 * phone gives every call through a layer to Vulkan, where a call costs more than it draws: the zoomed out map laid
 * hundreds of buildings a block, each with its own calls). Each function of the table called through a counting step
 * (GlCounted), the real one behind it.
 */
static std::atomic<uint64_t> _gl_calls = 0;
/** The card's calls of the pictures from the eyes by what they drew: 0 the triangles' runs (the ground, the billboards), 1 the buildings, 2 the vehicles and figures, 3 their glass, for the diary. */
std::atomic<uint64_t> _gpu_eye_calls_of[4] = {};
/** The card's calls from now till it goes counted to a kind of _gpu_eye_calls_of. */
struct EyeCallsOf {
	int kind;
	uint64_t from;
	explicit EyeCallsOf(int kind);
	~EyeCallsOf();
};
EyeCallsOf::EyeCallsOf(int kind) : kind(kind), from(_gl_calls.load(std::memory_order_relaxed)) {}
EyeCallsOf::~EyeCallsOf()
{
	const uint64_t now = _gl_calls.load(std::memory_order_relaxed);
	if (now >= this->from) _gpu_eye_calls_of[this->kind] += now - this->from;
}
static std::atomic<uint64_t> _gl_calls_standing = 0, _gl_calls_ground = 0, _gl_jobs_standing = 0, _gl_jobs_ground = 0, _gl_buildings = 0, _gl_calls_buildings = 0, _gl_vehicles = 0, _gl_calls_vehicles = 0, _gl_calls_vehicles_draw = 0;
/* The calls of drawing the turned map's vehicles counted by the function, to see which of a vehicle's calls are what
 * (the player's diary of 18. 11.: 107 calls a vehicle, z5 6 frames a second; #379: 32, the rig 8 -- few vehicles a
 * batch in the game). In the diary the most of them (VehicleCallsForDiary()); the rig ('test3d gpu volani') too. */
static constexpr int GL_COUNTED_FUNCTIONS = 512;
static std::atomic<bool> _gl_count_by_function = true;
/** The batches the turned map's vehicles were drawn in (draw_vehicles of ThreeDGpuDrawStanding()), since the diary's last line. */
static std::atomic<uint64_t> _gl_vehicle_batches = 0;
static thread_local bool _gl_counting_vehicles = false;
static std::array<std::atomic<uint64_t>, GL_COUNTED_FUNCTIONS> _gl_function_calls{};
/* The same of the view from the eyes (the player's #381: some 17 thousand calls a frame from the eyes for 1 100 draws,
 * the card 83 ms a frame at night): counted while a picture or a frame from the eyes is drawn (CountingEyeCalls). */
static thread_local bool _gl_counting_eye = false;
static std::array<std::atomic<uint64_t>, GL_COUNTED_FUNCTIONS> _gl_eye_function_calls{};
static std::atomic<uint64_t> _gl_eye_frames = 0; ///< frames and pictures from the eyes drawn since the rig started counting
/** While it lives the calls of the thread are those of drawing the view from the eyes. */
struct CountingEyeCalls {
	CountingEyeCalls() { _gl_counting_eye = true; _gl_eye_frames.fetch_add(1, std::memory_order_relaxed); }
	~CountingEyeCalls() { _gl_counting_eye = false; }
};
static std::array<const char *, GL_COUNTED_FUNCTIONS> _gl_function_names{};
template <int I, typename F> struct GlCounted;
template <int I, typename R, typename... A> struct GlCounted<I, R (THREE_D_GL_API *)(A...)> {
	static constexpr int index = I;
	static inline R (THREE_D_GL_API *real)(A...) = nullptr;
	static R THREE_D_GL_API Call(A... a)
	{
		_gl_calls.fetch_add(1, std::memory_order_relaxed);
		if constexpr (I < GL_COUNTED_FUNCTIONS) {
			if ((_gl_counting_vehicles || _gl_counting_eye) && _gl_count_by_function.load(std::memory_order_relaxed)) {
				(_gl_counting_eye ? _gl_eye_function_calls : _gl_function_calls)[I].fetch_add(1, std::memory_order_relaxed);
			}
		}
		return real(a...);
	}
};

/**
 * The rig ('test3d gpu volani [0|1]'): counting the calls of drawing the turned map's vehicles by the function on or
 * off, and what was counted so far, the most first.
 */
static std::string CallsByFunction(int on, bool eye)
{
	auto &calls = eye ? _gl_eye_function_calls : _gl_function_calls;
	std::vector<std::pair<uint64_t, const char *>> all;
	uint64_t sum = 0;
	for (int i = 0; i < GL_COUNTED_FUNCTIONS; i++) {
		const uint64_t n = calls[i].load(std::memory_order_relaxed);
		if (n == 0) continue;
		all.emplace_back(n, _gl_function_names[i] != nullptr ? _gl_function_names[i] : "?");
		sum += n;
	}
	std::ranges::sort(all, std::greater<>{});
	const uint64_t frames = _gl_eye_frames.load(std::memory_order_relaxed);
	std::string s = eye ? fmt::format("{} volani z oci za {} snimku ({} na snimek)", sum, frames, frames > 0 ? sum / frames : 0) : fmt::format("{} volani vozidel", sum);
	for (const auto &[n, name] : all) s += fmt::format(", {} {}", name, n);
	if (on >= 0) {
		for (auto &c : calls) c.store(0, std::memory_order_relaxed);
		if (eye) _gl_eye_frames = 0;
		_gl_count_by_function = on != 0;
	}
	return s;
}

/** The diary: the calls of drawing the turned map's vehicles (or the view from the eyes) since its last line, the most of them by the function. */
static std::string VehicleCallsForDiary(bool eye = false)
{
	/* What the counts were at its last line: they are not started again here, the rig's 'test3d gpu volani' counts
	 * across the diary's lines (started again there, a count below what was seen is all new). */
	static std::array<uint64_t, GL_COUNTED_FUNCTIONS> seen_vehicles{}, seen_eye{};
	auto &seen = eye ? seen_eye : seen_vehicles;
	auto &calls = eye ? _gl_eye_function_calls : _gl_function_calls;
	std::vector<std::pair<uint64_t, const char *>> all;
	for (int i = 0; i < GL_COUNTED_FUNCTIONS; i++) {
		const uint64_t now = calls[i].load(std::memory_order_relaxed);
		const uint64_t n = now >= seen[i] ? now - seen[i] : now;
		seen[i] = now;
		if (n > 0) all.emplace_back(n, _gl_function_names[i] != nullptr ? _gl_function_names[i] : "?");
	}
	std::ranges::sort(all, std::greater<>{});
	std::string s;
	for (size_t k = 0; k < all.size() && k < 10; k++) s += fmt::format("{}{} {}", k == 0 ? "" : ", ", all[k].second, all[k].first);
	return s.empty() ? "-" : s;
}

/* The view's pictures shown by the video driver straight from the card -- the turned map's ground and what stands on
 * it -- laid: a fence after each job that lays them, which the video driver's context waits for on the card before it
 * shows them (ThreeDGpuViewPicturesWait()); before, glFinish() after every block, the main thread standing till the card
 * had drawn it (the player's #310: what stands on the map 25-160 ms a frame). */
static std::mutex _view_fence_mutex;
static GLsync _view_fence = nullptr;

/** The view's pictures laid by the card: fenced for the video driver, or finished where it cannot wait for a fence. */
static bool MainJobQueued();
static bool _view_fence_owed = false; ///< the card's thread: laid view pictures not fenced yet (ViewPicturesLaid())

static void ViewPicturesLaid()
{
	if (!_three_d_gpu_driver_waits || gl.glFenceSync == nullptr || gl.glDeleteSync == nullptr || gl.glFlush == nullptr) {
		gl.glFinish();
		return;
	}
	/* Another of the main thread's jobs comes next (the blocks of a frame one after another): it fences them all,
	 * the screen is not shown before it -- not a fence and a flush to the card for every block (the player's phone
	 * gives them through a layer to Vulkan; 1100 grounds in five seconds of the drag of the zoomed out map). */
	if (_gpu_fence_last_only && MainJobQueued()) {
		_view_fence_owed = true;
		return;
	}
	_view_fence_owed = false;
	GLsync fence = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	gl.glFlush();
	std::lock_guard lock(_view_fence_mutex);
	if (_view_fence != nullptr) gl.glDeleteSync(_view_fence);
	_view_fence = fence;
}

/** Any function, before it is cast to what it is. */
using GpuProc = void (*)();

/* The card's thread and what is asked of it. */
enum class GpuState { NotStarted, Starting, Ready, Failed };
struct GpuJob {
	std::span<ThreeDGpuPicture *const> pics; ///< pictures drawn at once (DrawPicturesOnCard()); else
	ThreeDGpuEyePicture *eye = nullptr; ///< the view from the eyes (DrawEyeOnCard())
	ThreeDGpuEyeFrame *frame = nullptr; ///< else the view from the eyes whole (DrawEyeFrameOnCard())
	struct ShownRead *read = nullptr;   ///< else a picture shown straight from the card read back (ReadShownOnCard())
	ThreeDGpuGround *ground = nullptr;  ///< else the ground of a block of the turned map (DrawGroundOnCard())
	ThreeDGpuStanding *standing = nullptr; ///< else what stands on a block of the turned map (DrawStandingOnCard())
	bool done = false;
	bool ok = false;
	bool main = false; ///< given by the main thread: its jobs are taken in the order given, before the others (QueueGpuJob())
	/** Given without waiting and owned by the queue (ThreeDGpuMoveViewGround()): its ground here, the job let go of by
	 * the card's thread when done; not laid, _view_move_failed told. */
	std::unique_ptr<ThreeDGpuGround> owned_ground;
	/** What stands on a block given without waiting (ThreeDGpuDrawStandingAsync()): here, the job the main thread's
	 * till ThreeDGpuStandingSettle() takes it, the card's thread only telling it done. */
	std::unique_ptr<ThreeDGpuStanding> owned_standing;
	/** And the keys of the vehicles' pictures it lays (ThreeDGpuStandingVehicle::key), their places not given to others
	 * meanwhile (SweepStandingPlaces()) till it is done. */
	std::vector<uint64_t> pinned;
};
/** A move of the view's picture given without waiting (ThreeDGpuMoveViewGround()) the card could not do: the main thread lays the view anew. */
static std::atomic<bool> _view_move_failed = false;
std::mutex _gpu_mutex;              ///< of all below
std::condition_variable _gpu_wake;  ///< the card's thread: a job, or the end
std::condition_variable _gpu_done;  ///< those asking: a job done, or the card ready (or not)
GpuState _gpu_state = GpuState::NotStarted;
std::deque<GpuJob *> _gpu_jobs;
/** The thread that draws the game's windows (the first, the main thread): its jobs first, the others after (QueueGpuJob()). */
const std::thread::id _gpu_main_thread = std::this_thread::get_id();

/**
 * A job for the card, under _gpu_mutex: the main thread's first -- the drawing of the screen waits for it, the cursor
 * and the keys with it --, else after those waiting (the pictures of the thread in the background, the frames from
 * the eyes).
 */
void QueueGpuJob(GpuJob &job)
{
	if (std::this_thread::get_id() == _gpu_main_thread) {
		/* After the main thread's jobs already there, in the order given: one it gave without waiting (the move of the
		 * view's picture, ThreeDGpuMoveViewGround(); a block's ground started early) comes before what it gives next. */
		job.main = true;
		auto it = std::find_if(_gpu_jobs.begin(), _gpu_jobs.end(), [](const GpuJob *j) { return !j->main; });
		_gpu_jobs.insert(it, &job);
	} else {
		_gpu_jobs.push_back(&job);
	}
}
/**
 * For the diary: how long the main thread stood waiting for the card, by what it waited for (the player, 17. 11.: "zoom
 * out je slabý a cuká" -- the drag of the zoomed out map stood on the card at every step), ms and times since asked.
 * 0 the move of the view's picture, 1 the ground of a block, 2 what stands on a block, 3 the ground started earlier
 * taken, 4 the moving layer, 5 what stands given without waiting, all of it at the end of the frame
 * (ThreeDGpuStandingSettle()).
 */
static constexpr int GPU_WAIT_KINDS = 6;
static std::atomic<double> _gpu_main_wait_ms[GPU_WAIT_KINDS] = {};
static std::atomic<uint32_t> _gpu_main_waits[GPU_WAIT_KINDS] = {};
static std::atomic<double> _gpu_main_wait_all_ms = 0; ///< all of it since the game started, never started over (ThreeDGpuMainWaitedMs())
/** While it lives the main thread's wait on the card is counted as of this kind (GPU_WAIT_KINDS). */
/* For the watch of a freeze (WatchFreeze()): what the main thread waits on the card for now (-1 nothing), since when. */
static std::atomic<int> _watch_wait_kind = -1;
static std::atomic<int64_t> _watch_wait_from = 0;
struct GpuMainWait {
	int kind;
	std::chrono::steady_clock::time_point from = std::chrono::steady_clock::now();
	explicit GpuMainWait(int kind) : kind(kind)
	{
		if (std::this_thread::get_id() != _gpu_main_thread) return;
		_watch_wait_from = WatchNow();
		_watch_wait_kind = kind;
	}
	~GpuMainWait()
	{
		if (std::this_thread::get_id() != _gpu_main_thread) return;
		_watch_wait_kind = -1;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - this->from).count();
		_gpu_main_wait_ms[this->kind] = _gpu_main_wait_ms[this->kind] + ms;
		_gpu_main_wait_all_ms = _gpu_main_wait_all_ms + ms;
		_gpu_main_waits[this->kind]++;
	}
};


bool _gpu_stop = false;
std::thread _gpu_thread;

/** Whether a job of the main thread waits in the queue now (ViewPicturesLaid()). */
static bool MainJobQueued()
{
	std::lock_guard lock(_gpu_mutex);
	return !_gpu_jobs.empty() && _gpu_jobs.front()->main;
}
/* The video driver's OpenGL (ThreeDGpuVideoContext()): an EGL display and context, or a WGL device and context. The
 * card's context made sharing its things with it (_video_shared), the view from the eyes is shown straight from the
 * card: the driver draws the card's texture where the picture is (ThreeDGpuEyeFrame::shown). */
std::mutex _video_mutex;
void *_video_display = nullptr;
void *_video_context = nullptr;
std::atomic<bool> _video_shared = false;
std::string _gpu_describe = "not started";
/** The card could not be started (GpuThreadMain()): why, and that the player is to be told of it; under _gpu_mutex. */
std::string _gpu_failed_why;
bool _gpu_failed_note = false;

double _gpu_ms = 0;                 ///< the time the card took for the pictures since the last ThreeDGpuTimes()
uint64_t _gpu_pictures = 0;         ///< and how many
uint64_t _gpu_failed = 0;           ///< and how many it could not draw (the processor did)
uint64_t _gpu_standing_vehicles = 0;      ///< the vehicles of the turned map it drew and shrunk since the last ThreeDGpuTimes() (ThreeDGpuStandingVehicle)
uint64_t _gpu_standing_vehicles_kept = 0; ///< and those it laid as they were kept
uint64_t _gpu_standing_triangles[2] = {}; ///< the triangles of the buildings among what stands it drew, and of all of them those in the blocks (GpuBuilding::Cell)
uint64_t _gpu_eye_batches = 0;   ///< the batches of meshes alike of the view from the eyes it drew at once since the last ThreeDGpuTimes() (EyeObjectDraws::Batch)
uint64_t _gpu_eye_instances = 0; ///< and the meshes in them
uint64_t _gpu_eye_single = 0;    ///< and the meshes it drew each on its own (a wheel that turns)
uint64_t _gpu_eye_frames = 0;    ///< the pictures from the eyes it drew since the last ThreeDGpuTimes()
uint64_t _gpu_eye_triangles = 0; ///< and the triangles in them, each instance's (buildings, figures, glass)
uint64_t _gpu_eye_calls = 0;     ///< and its draw calls for them
uint64_t _gpu_eye_kinds = 0;     ///< the different objects of their batches, summed: fewer than the batches, an object split (its detail, paint, side)
uint64_t _gpu_eye_steps = 0;     ///< the steps of objects of the pictures (ThreeDGpuEyeStep::Kind::Objects), each batched on its own
uint64_t _gpu_eye_pixels = 0;    ///< and their pixels (whether the card is held up by the triangles or by the pixels)
/** The triangles and instances of each object from the eyes since the last ThreeDGpuTimes(): what the card draws most. */
static std::map<std::string, std::pair<uint64_t, uint64_t>, std::less<>> _gpu_eye_most;
static std::mutex _gpu_eye_most_mutex;
/**
 * The blocks of the turned map whose standing things the card gave back to the processor since the last ThreeDGpuTimes()
 * (DrawStandingOnCard(): one thing it cannot lay, the processor draws the whole block -- the buildings by their
 * triangles), by what it could not lay: a box's faces, a building's shape, a building's texture, a picture.
 */
uint64_t _gpu_standing_failed[4] = {};
int _gpu_standing_failed_told = 0; ///< how many of those the card's diary told what of (at most 40)
double _gpu_ground_ms = 0;          ///< the time the card took laying the ground of the turned map since the last ThreeDGpuTimes(), and what stands on it
uint64_t _gpu_grounds = 0;          ///< and how many blocks
double _gpu_ground_only_ms = 0;     ///< of it the ground alone (the player's diary of 18. 11.: "the ground of 136 blocks in 1198 ms" was the ground and what stands together)
uint64_t _gpu_grounds_only = 0;     ///< and how many blocks of ground
uint64_t _gpu_grounds_failed = 0;   ///< and how many it could not lay (the processor did)

/* What the card keeps, by the thing's number (ThreeDUniqueId): the corners of the shapes, their triangles, the
 * textures; let go of the longest not drawn when it holds more than GPU_BYTES_KEPT. Only the card's thread uses these. */
struct GpuShape {
	GLuint vao = 0, pos = 0, nrm = 0, uv = 0, wheel = 0, joints = 0, weights = 0;
	/** Its corners with the instances' matrices and lights as attributes 4-8 (InstancedVao()), made at its first batch from the eyes. */
	mutable GLuint vao_instanced = 0;
	bool has_nrm = false;
	bool has_wheel = false; ///< each corner's turning wheel (ThreeDVehicleShape::corner_wheel) as attribute 3
	bool has_bones = false; ///< a figure on bones at rest: each corner's joints as attribute 9, their weights as 10 (ThreeDVehicleShape::corner_joints)
	size_t corners = 0;   ///< how many corners it has: no triangle drawn may name one beyond
	size_t bytes = 0;
	uint64_t used = 0;
};
struct GpuIndices {
	GLuint ibo = 0;
	uint32_t largest = 0; ///< the largest corner its triangles name
	size_t bytes = 0;
	uint64_t used = 0;
};
struct GpuTexture {
	GLuint name = 0;
	size_t bytes = 0;
	uint64_t used = 0;
	uint8_t channels = 4; ///< of a mask only those its programs read (MaskOnCard())
};
std::unordered_map<uint64_t, GpuShape> _card_shapes;
std::unordered_map<uint64_t, GpuIndices> _card_indices;
/* A building of the view from the eyes (DrawEyeOnCard()): each triangle three corners of its own -- x, y, z, u, v and
 * the triangle's light, its corners' mean as the processor shades it --, by texture and side facing out. */
struct GpuBuilding {
	GLuint vao = 0, vbo = 0;
	/** The triangles of a run in one cell of the shape (MapBuildingOnCard()): one after the other in it. */
	struct Cell {
		GLint first = 0;
		GLsizei count = 0;
		uint16_t id = 0; ///< which cell (boxes)
	};
	struct Run {
		GLint first = 0;
		GLsizei count = 0;
		uint8_t texture = 0;
		int8_t facing = 0;
		std::vector<Cell> cells; ///< its triangles cell by cell; none: not cut into cells
	};
	std::vector<Run> runs;
	std::vector<Run> night_runs;             ///< its lit glass of the night after them, by texture (ThreeDGpuBuilding::night_pos)
	std::vector<std::array<float, 6>> boxes; ///< each cell's box: the least and most x, y, z of its triangles' corners
	size_t bytes = 0;
	uint64_t used = 0;
};
std::unordered_map<uint64_t, GpuTexture> _card_textures;
std::unordered_map<uint64_t, GpuBuilding> _card_buildings;
std::atomic<size_t> _card_bytes = 0; ///< (read by the diary of the frames too, ThreeDGpuHeldMB())
/** Of it, by kind -- the shapes, their triangles, the textures, the buildings --, and what of each was put on it since
 * the last line of the diary (ThreeDGpuTimes(); the player's log of 29. 10.: the card holding 635 MB of its 384 and 755 MB
 * put on it in five seconds in the view from the eyes by a road, the phone's memory growing). */
std::array<std::atomic<size_t>, 4> _card_bytes_of{};
std::array<std::atomic<size_t>, 4> _card_sent_of{};
std::atomic<size_t> _card_sent = 0; ///< the rig: bytes put on the card since the last ThreeDGpuTimes()
/** Of _card_sent, what is not a shape, triangles, a texture or a building (none of _card_sent_of): the ground's rects
 * (SendGroundRect()) and the pictures of what stands (StandingPlace()) -- the player's diary of 17. 11.: at zoom 0 by
 * night 190 MB in five seconds put on the card that the diary did not tell what it was. */
static std::atomic<size_t> _card_sent_ground = 0, _card_sent_standing = 0;
std::atomic<int> _card_trims = 0;   ///< and how many times it let go of what it held the longest not drawn
std::atomic<int64_t> _card_biggest = 0; ///< and the most pixels of a frame of pictures read back at once (its width << 32 | its height)
uint64_t _card_clock = 0;
constexpr size_t GPU_BYTES_KEPT = 1024u * 1024 * 1024; ///< (the player, 20. 10.: "today's cards have 8 GB"; 128 MB after #285 was Winlator's old OpenGL)
/**
 * As much on a card with no memory of its own, whose memory is the system's (a phone's Mali or Adreno through Zink and
 * Vortek or VirGL, the processor's llvmpipe): the player's phone, 26. 10., the card holding a gigabyte, the phone's
 * memory over its limit and Winlator gone.
 */
constexpr size_t GPU_BYTES_KEPT_SHARED = 384u * 1024 * 1024;
size_t _card_budget = GPU_BYTES_KEPT; ///< over this much the card lets go of what was not drawn for CARD_KEPT_SECONDS (GPU_BYTES_KEPT, GPU_BYTES_KEPT_SHARED), set at its start
/**
 * What the card holds at most whatever was drawn when (TrimCard()): over it, all not drawn in this picture goes. The
 * player's test10 on the phone (snimky.log): what a scene draws was more than the 384 MB kept, so every picture let go
 * of what the next one drew -- 1 to 2 GB put on the card in 5 s turning the map, 2 to 3 frames a second; the player,
 * 14. 11.: "there should be no limit on the memory". Let go of by age now, this only so that it cannot grow for ever.
 */
constexpr size_t GPU_BYTES_AT_MOST = 3072u * 1024 * 1024;
constexpr size_t GPU_BYTES_AT_MOST_SHARED = 1536u * 1024 * 1024;
size_t _card_at_most = GPU_BYTES_AT_MOST;
/** Not drawn for so long, a thing may be let go of when the card holds more than _card_budget. */
constexpr std::chrono::seconds CARD_KEPT_SECONDS{20};
/** When each value of _card_clock began (TrimCard()), the oldest first: how long ago a thing was drawn last. */
std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>> _card_clock_times;
/* The rows of a frame of pictures drawn and read back at once (DrawPicturesOnCard()): more of
 * them in more frames -- one picture taller has a frame of its own. A batch on 2048 x 10378 at once (the rig zoomed in
 * by the town, zink), its frame some 190 MB on the card and its 85 MB read back in one go, the player's phone sharing
 * its memory with its card (24. 10.: Winlator gone in the middle of reading pictures back, no crash log of the game). */
constexpr int FRAME_ROWS = 2048;

/** How much the game and the card hold now, for the diary of the card (the step it may die at says it too). */
std::string MemoryNow()
{
	return fmt::format("hra {} MB, karta {} MB", FrameDiaryMemoryMB(), _card_bytes.load() >> 20);
}

/* The programs, the picture drawn into, the corners of the lights at night. */
struct GpuProgram {
	GLuint program = 0;
	GLint matrix = -1, light = -1, tex = -1, glass = -1, glass_alpha = -1, texel_alpha = -1, glow = -1;
	/* The paint (ThreeDGpuPaint). */
	GLint paint = -1, detail = -1, detail_colours = -1, paint_mask = -1, addon_mask = -1, rust_mask = -1;
	GLint paint_values = -1; ///< its colours and numbers at once, five vec4 (MESH_FRAGMENT, SetPaint())
	GLint rust_moments = -1, rust_delay = -1;
	GLint near_plane = -1; ///< the view from the eyes' nearest (EYE_BUILDING_FRAGMENT)
	GLint wheels = -1, wheel_count = -1; ///< the view from the eyes' turning wheels (EyeMeshSource())
	GLint depth_range = -1; ///< the nearness a picture's depths span (MAP_BUILDING_VERTEX)
	GLint instanced = -1;   ///< the view from the eyes' meshes alike drawn at once: each instance's matrix and light in attributes 4-8 (EyeMeshSource())
	GLint bones = -1, bone_row = -1; ///< a figure on bones: its joints' matrices and their row of them (MESH_VERTEX)
};
GpuProgram _mesh_program, _night_program, _eye_building_program, _eye_mesh_program, _ground_program, _standing_program;
GpuProgram _standing_building_program; ///< the buildings among what stands on the turned map (DrawStandingOnCard())
GLuint _fbo = 0, _colour_rb = 0, _lit_rb = 0, _depth_rb = 0;
int _fbo_w = 0, _fbo_h = 0;
/* The view from the eyes drawn into: its colours, and its depth as floats (near / how far ahead, the larger nearer). */
GLuint _eye_fbo = 0, _eye_colour_rb = 0, _eye_glow_rb = 0, _eye_depth_rb = 0;
int _eye_w = 0, _eye_h = 0;
GLuint _night_vao = 0, _night_vbo = 0;
GLuint _eye_instance_vbo = 0; ///< the matrices and lights of a batch of meshes alike of the view from the eyes (EyeObjectDraws::Batch)
GLint _max_texture = 0, _max_target = 0;
/* The whole view from the eyes on the card (DrawEyeFrameOnCard()). */
GLuint _frame_colour[2] = {0, 0}, _frame_glow = 0, _frame_depth = 0;
GLuint _frame_geometry_fbo[2] = {0, 0}, _frame_pixel_fbo[2] = {0, 0};
int _frame_w = 0, _frame_h = 0;
GLuint _frame_atlas = 0;
int _frame_atlas_w = 0, _frame_atlas_h = 0;
std::vector<uint32_t> _frame_atlas_px;
/* The ground of the turned map (DrawGroundOnCard()): the big textures by key, the small ones (the tiles with a foundation)
 * in one atlas filled shelf by shelf and started over when full, the block drawn into. */
struct GroundTextureOnCard {
	GLuint name = 0;
	int w = 0, h = 0;
	uint64_t generation = 0;
};
std::unordered_map<uint64_t, GroundTextureOnCard> _ground_textures;
std::unordered_map<uint64_t, std::array<int, 4>> _ground_atlas_places; ///< x, y, w, h by key
GLuint _ground_atlas = 0;
int _ground_atlas_size = 0, _ground_shelf_x = 0, _ground_shelf_y = 0, _ground_shelf_h = 0;
GLuint _gfbo = 0, _g_colour_rb = 0;
GLuint _g_move_fbo[2] = {0, 0}; ///< the kept picture of the ground moved on the card (GroundTextureOf()): from, to
/* The roads of the ground of the view from the eyes smoothed on the card (SmoothGroundRoads()). */
static constexpr int ROAD_SMOOTH_PIECE = 1024; ///< the rect smoothed by pieces so big at most
GpuProgram _road_smooth_program;
GLuint _road_smooth_vao = 0, _road_smooth_fbo = 0, _road_smooth_piece = 0;
std::atomic<int> _road_smooth_state = 0; ///< 0 not tried, 1 ready, -1 the card cannot (read by the game's thread too)
int _gfbo_w = 0, _gfbo_h = 0;
GLuint _ground_vao = 0, _ground_vbo = 0;
bool _ground_program_ok = false;
/**
 * The pictures of a view shown straight from the card (ThreeDGpuGround::view): the turned main view's (0), and the small
 * views' facing north -- the extra viewports, the vehicle windows (the player, 1. 11.: "go do the GPU, what is still
 * needed"; a small view drew the houses by the processor, 150-260 ms a frame on the phone) --, each as the main view's.
 * Its ground laid by the card into a picture of the view's size (ThreeDGpuGround::into_view), shown straight from it
 * under the screen, and one of its size it is moved through when the view moves. What stands on it laid into two
 * pictures of the view's size (ThreeDGpuStanding; the player, 26. 10.: "the card to compose the turned map", step 2):
 * their colours with their alpha multiplied in, and which of them are lit at night; made with the picture of the
 * view's ground, moved with it, shown over it.
 */
struct ViewPictures {
	GLuint ground = 0, ground_tmp = 0;
	GLuint ground_fbo = 0, ground_tmp_fbo = 0;
	int w = 0, h = 0;
	std::atomic<uint32_t> ground_name = 0; ///< ground for the other threads, 0 none (ThreeDGpuViewGround())
	std::atomic<uint64_t> made = 0;        ///< counts the times it was made anew or let go: what it held is gone
	GLuint standing = 0, standing_lit = 0, standing_fbo = 0;
	GLuint standing_depth = 0;             ///< the depth of the building being drawn among them (ThreeDGpuStandingBuilding)
	std::atomic<uint32_t> standing_name = 0, standing_lit_name = 0, standing_order_name = 0; ///< for the other threads, 0 none (ThreeDGpuViewStanding())
	GLuint standing_order = 0;             ///< the order of what stands there by each pixel (ThreeDGpuStandingItem::order), its third picture
	/* What stands with the moving things laid over it (ThreeDGpuStanding::moving): its colours and which are lit,
	 * shown instead while the moving layer is on (ThreeDGpuMovingLayer()). */
	GLuint moving = 0, moving_lit = 0, moving_fbo = 0;
	/** The depth of the moving things among themselves, a pass's alone (cleared as it begins): each pixel of each laid
	 * where it is nearer than those laid before it (DrawStandingOnCard()). */
	GLuint moving_depth = 0;
	std::atomic<uint32_t> moving_name = 0, moving_lit_name = 0;
	std::atomic<bool> moving_on = false;
	std::atomic<bool> moving_anew = false;              ///< switched on again: all of it copied first
	bool moving_whole = true;                           ///< all of it to be copied from what stands (made anew, the view moved)
	std::vector<std::array<int, 4>> moving_were;        ///< where the moving things were laid last, x, y, w, h of the picture
	std::vector<std::array<int, 4>> moving_stood_since; ///< where what stands was laid since
};
static std::array<ViewPictures, THREE_D_GPU_VIEWS> _views;

/** A view's pictures by its number (ThreeDGpuGround::view), the main view's for one there is not. */
static ViewPictures &ViewPicturesOf(int view)
{
	return _views[view >= 0 && view < THREE_D_GPU_VIEWS ? view : 0];
}

/** A view's pictures of the moving things let go of: made anew of the view's size when next laid. */
static void ForgetMovingPictures(ViewPictures &v)
{
	v.moving_name = v.moving_lit_name = 0;
	if (v.moving != 0) {
		gl.glDeleteTextures(1, &v.moving);
		gl.glDeleteTextures(1, &v.moving_lit);
	}
	v.moving = v.moving_lit = 0;
	if (v.moving_depth != 0) gl.glDeleteRenderbuffers(1, &v.moving_depth);
	v.moving_depth = 0;
	v.moving_whole = true;
	v.moving_were.clear();
	v.moving_stood_since.clear();
}

/** A view's pictures let go of (ForgetCard(), ThreeDGpuForgetView()): what they held is gone. */
static void ForgetViewPictures(ViewPictures &v)
{
	if (v.ground != 0) {
		gl.glDeleteTextures(1, &v.ground);
		gl.glDeleteTextures(1, &v.ground_tmp);
	}
	v.ground = v.ground_tmp = 0;
	v.w = v.h = 0;
	v.ground_name = 0;
	v.made++;
	if (v.standing != 0) {
		gl.glDeleteTextures(1, &v.standing);
		gl.glDeleteTextures(1, &v.standing_lit);
	}
	v.standing = v.standing_lit = 0;
	v.standing_name = v.standing_lit_name = v.standing_order_name = 0;
	if (v.standing_depth != 0) gl.glDeleteRenderbuffers(1, &v.standing_depth);
	v.standing_depth = 0;
	if (v.standing_order != 0) gl.glDeleteTextures(1, &v.standing_order);
	v.standing_order = 0;
	ForgetMovingPictures(v);
}
/* The pictures laid: a sprite's kept by its key in the atlas, the block's others in the stream; started over when full.
 * Beside the atlas how near each pixel of a vehicle kept in it is (ThreeDGpuStandingVehicle::near_span), 16 bits in
 * red and green, as it was shrunk into it (STANDING_SHRINK_FRAGMENT). */
GLuint _standing_atlas = 0, _standing_stream = 0, _standing_near = 0;
bool _standing_near_checked = false; ///< whether the atlas's frame was found complete with it
int _standing_atlas_size = 0;
struct StandingShelf {
	int x = 0, y = 0, h = 0;
};
StandingShelf _standing_atlas_shelf, _standing_stream_shelf;
std::unordered_map<uint64_t, std::array<int, 2>> _standing_places;
/** The vehicles' pictures among them (their keys, ThreeDGpuStandingVehicle::key), for the main thread to ask (ThreeDGpuStandingKeeps()). */
std::mutex _standing_kept_mutex;
std::unordered_set<uint64_t> _standing_kept_keys;
/** Of those, the ones a job given without waiting lays (GpuJob::pinned), how many jobs each: their places not given to another meanwhile (SweepStandingPlaces()). */
std::unordered_map<uint64_t, int> _standing_pinned;
/** The vehicles given with their shapes whose pictures could not be made (too big, a shape the card could not take): not asked for again. Only the card's thread uses it. */
std::unordered_set<uint64_t> _standing_failed_keys;
/**
 * The kept pictures' places as they are held: so big (a multiple of STANDING_SLOT_STEP each way), last laid from in
 * the card's pass `used` (_card_clock) at the time `when`, a vehicle's with its key among _standing_kept_keys. When the
 * pictures are full, the places of those not laid from for STANDING_SLOT_IDLE are given to new ones (StandingReserve();
 * by the time, not by the passes: a frame is one pass of the moving layer and any number of the blocks') -- not all of them
 * forgotten: the player's #374, a busy save by day, the vehicles going by filled them with their turns several times
 * a second, and each time every vehicle in sight was prepared with its shapes and drawn again (the gathering of the
 * moving layer 20 ms a frame, the card's drawing 10 ms more).
 */
struct StandingSlot {
	int w = 0, h = 0;
	uint64_t used = 0;
	std::chrono::steady_clock::time_point when{};
	uint64_t kept_key = 0;
	bool vehicle = false;
};
constexpr int STANDING_SLOT_STEP = 16;
constexpr std::chrono::milliseconds STANDING_SLOT_IDLE{1000}; ///< so long not laid from, a place is given to another picture when room is needed
std::unordered_map<uint64_t, StandingSlot> _standing_slots;
std::map<std::pair<int, int>, std::vector<std::array<int, 2>>> _standing_free; ///< the places free again, by their size
std::array<int, 2> _standing_reserved_size = {0, 0}; ///< the size of the place StandingReserve() gave last (kept)
uint64_t _standing_swept_at = 0;                     ///< the pass the idle places were last looked for
std::atomic<uint64_t> _standing_places_reused = 0, _standing_places_cleared = 0; ///< for the diary
/** For the diary: how much of the kept pictures' atlas their places take, at most since the diary asked -- the vehicles'
 * and figures' and the other pictures' (the blocks' sprites and faces), per mille of it; and how many places. Counted
 * by the card's thread about once a second (StandingFillCount()). */
std::atomic<int> _standing_fill_vehicles = 0, _standing_fill_pictures = 0, _standing_fill_places = 0, _standing_fill_free = 0;
std::atomic<int> _rig_standing_atlas_size = 0; ///< Rig ('test3d atlas <px>'): the kept pictures so big, 0 as they are

void ClearStandingPlaces()
{
	_standing_places.clear();
	_standing_slots.clear();
	_standing_free.clear();
	std::lock_guard lock(_standing_kept_mutex);
	_standing_kept_keys.clear();
	_standing_failed_keys.clear();
}

/** A kept picture laid from its place in this pass: its place stays its own. */
void StandingPlaceUsed(uint64_t key);

/** A kept picture has its place, the one StandingReserve() gave last. */
void StandingPlaceKept(uint64_t key, bool vehicle, uint64_t kept_key);
GLuint _standing_vao = 0, _standing_vbo = 0;
GLuint _standing_tri_vao = 0, _standing_tri_vbo = 0; ///< the lit glass of the buildings at night, placed already
bool _standing_program_ok = false;
/* The vehicles among them drawn first at supersample times their size (ThreeDGpuStandingVehicle): their colours and
 * which of them are lit as textures, shrunk from them as they are laid. */
GLuint _sv_fbo = 0, _sv_colour = 0, _sv_lit = 0, _sv_depth = 0;
int _sv_w = 0, _sv_h = 0;
GLuint _standing_atlas_fbo = 0; ///< the kept pictures drawn into (a vehicle shrunk into them)
/** Counts the makings and deletions of the atlas, its nearness and its frame buffer: what is attached to it known only while the same (shrink_vehicle). */
uint64_t _standing_atlas_made = 0;
GpuProgram _standing_faces_program; ///< the faces of the boxes among them (ThreeDGpuStandingFaces)
bool _standing_faces_ok = false;
GLuint _standing_faces_vao = 0, _standing_faces_vbo = 0;
/** The keys of the boxes' textures among the kept pictures (ThreeDGpuStandingFaces::key). */
constexpr uint64_t STANDING_FACES_KEY = uint64_t{1} << 60;
/** The keys of the vehicles among the kept pictures (ThreeDGpuStandingVehicle::key), apart from the sprites' and the figures'. */
constexpr uint64_t STANDING_VEHICLE_KEY = uint64_t{1} << 61;
GpuProgram _standing_shrink_program;
bool _standing_shrink_ok = false;
GpuProgram _clear_program;
bool _clear_program_ok = false;
GpuProgram _standing_copy_program; ///< what stands copied under the moving things (COPY_FRAGMENT)
bool _standing_copy_ok = false;
GLuint _shrink_vao = 0;            ///< the vehicles shrunk into the kept pictures (STANDING_SHRINK_VERTEX): x, y, z, u, v, their nearness and its range (SHRINK_FLOATS)
/** The floats of a corner of a vehicle shrunk (_shrink_vao). */
static constexpr int SHRINK_FLOATS = 13;
GLuint _standing_order_vao = 0;    ///< the pictures laid with their order and box (ThreeDGpuStandingItem::order, ::box): x, y, z, u, v, order, lowest corner and middle, highest corner and how
/** The floats of a corner of a picture laid (_standing_order_vao, STANDING_BOX_VERTEX). */
static constexpr int STANDING_BOXED_FLOATS = 14;
/** The diary: the moving things laid over the views (ThreeDGpuStanding::moving) -- times, things, pixels copied, card ms. */
std::atomic<uint64_t> _gpu_moving_passes = 0, _gpu_moving_things = 0, _gpu_moving_copied = 0;
std::atomic<double> _gpu_moving_ms = 0;
/** Of the vehicles' layer on the card, for the diary: the copying under them, the vehicles drawn anew and shrunk into their pictures (how many, how long). */
std::atomic<double> _gpu_moving_copy_ms = 0, _gpu_moving_draw_ms = 0;
std::atomic<uint64_t> _gpu_moving_drawn = 0;
/** Of the moving layer's vehicles not kept, those drawn before its picture was begun, and how long that took (ThreeDGpuDrawStanding()). */
std::atomic<uint64_t> _gpu_moving_first = 0;
std::atomic<double> _gpu_moving_first_ms = 0;
/** Whether the card's glClear() keeps to the scissor's rect (TestScissoredClear()); else ClearInScissor() draws over it. */
static bool _scissored_clear_ok = true;
/** Whether the card divides an attribute by instance (TestInstancedArrays()); else the meshes alike drawn each on its own. */
static bool _instanced_arrays_ok = true;
/* The matrices of the joints of the figures on bones the card poses (ThreeDGpuMesh::bones): a row of a texture each
 * figure, three texels a joint, its rows; the rows written in turn, so that one is not written over while a figure
 * drawn before may still read it. On a unit of its own (BONE_UNIT), apart from the textures and the paint's masks. */
static constexpr int BONE_UNIT = 6;
static constexpr int BONE_ROWS = 256;
static GLuint _bones_texture = 0;
static int _bones_next_row = 0;
/** Whether the card poses the figures on bones (TestBones()); else the processor poses their frames. */
static std::atomic<bool> _bones_ok = false;
GLuint _frame_vao = 0, _frame_vbo = 0, _frame_empty_vao = 0, _frame_glow_vao = 0, _frame_glow_vbo = 0, _frame_star_vao = 0, _frame_star_vbo = 0;
GLuint _frame_list = 0, _frame_lay = 0, _frame_lay_z = 0, _frame_shadow_map = 0;
uint64_t _frame_shadow_key = 0;
std::atomic<bool> _frame_programs = false; ///< compiled: the frame on the card is possible
GpuProgram _frame_tri_program, _frame_background_program, _frame_haze_program, _frame_far_haze_program, _frame_shadow_program;
GpuProgram _frame_night_program, _frame_lamps_program, _frame_blasts_program, _frame_glow_after_program, _frame_lay_program;
GpuProgram _frame_glow_program, _frame_star_program, _frame_moon_program, _frame_copy_program;
/* The pictures shown straight from the card (ThreeDGpuEyeFrame::shown): three in turn, the video driver drawing the
 * last while the next is made. */
GLuint _frame_shown[3] = {0, 0, 0};
int _frame_shown_w[3] = {0, 0, 0}, _frame_shown_h[3] = {0, 0, 0};
int _frame_shown_next = 0;
GLuint _frame_shown_fbo = 0;

void StartEyeFrame();
void EndEyeFrame();

/* The shapes drawn: a corner of the shape to the picture (u_matrix), lit as RasterObject() lights it -- at the
 * corners, the light between them as the processor's, straight across the picture (noperspective) --; each pixel the
 * texture's texel under it, as the processor picks it, a hole where the texel is 0. The glass laid over what is behind
 * by its alpha, the colour with its alpha multiplied in (as the processor's "over"). A material's own light added
 * unshaded, its pixels marked lit. */
/* A figure on bones (ThreeDGpuMesh::bones, u_bone_row 0 or more): each corner moved by its joints (attribute 9) as much
 * as each weighs (10), as GlbSkeleton::Skin() moves it -- their matrices summed so, its corner and its normal by them,
 * the normal made a unit again; each joint's matrix three texels of the row u_bone_row of u_bones, its rows. */
const char *const MESH_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 9) in vec4 a_joints;
layout(location = 10) in vec4 a_weights;
uniform mat4 u_matrix;
uniform vec4 u_light_lit;
#define u_light (u_light_lit.xyz)
#define u_lit (u_light_lit.w)
uniform sampler2D u_bones;
uniform int u_bone_row;
noperspective out vec2 v_uv;
noperspective out float v_light;
void main()
{
	vec3 pos = a_pos, nrm = a_nrm;
	if (u_bone_row >= 0) {
		vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0);
		for (int k = 0; k < 4; k++) {
			float w = a_weights[k];
			if (w == 0.0) continue;
			int j = int(a_joints[k] + 0.5) * 3;
			r0 += w * texelFetch(u_bones, ivec2(j, u_bone_row), 0);
			r1 += w * texelFetch(u_bones, ivec2(j + 1, u_bone_row), 0);
			r2 += w * texelFetch(u_bones, ivec2(j + 2, u_bone_row), 0);
		}
		vec4 at = vec4(a_pos, 1.0);
		pos = vec3(dot(r0, at), dot(r1, at), dot(r2, at));
		vec3 n = vec3(dot(r0.xyz, a_nrm), dot(r1.xyz, a_nrm), dot(r2.xyz, a_nrm));
		float l = length(n);
		nrm = l > 0.0 ? n / l : n;
	}
	gl_Position = u_matrix * vec4(pos, 1.0);
	v_uv = a_uv;
	v_light = u_lit > 0.5 ? 0.55 + 0.45 * max(0.0, dot(nrm, u_light)) : 1.0;
}
)";

/* The texel painted as ThreeDGpuPaint has it, as GpuTextureOf() of three_d_vehicle_object.cpp paints it on the
 * processor (the colours 0-255, each mask read at the texel's middle): the detail in its colour at the texel's lightness
 * or turned, else the body's paint coloured and faded and the add-on's colour; then the tint and the rust; cut to whole
 * numbers before it is lit. */
const char *const MESH_FRAGMENT = R"(#version 330 core
uniform sampler2D u_tex;
uniform int u_glass;
uniform float u_glass_alpha;
uniform int u_texel_alpha;
uniform vec4 u_glow;
uniform int u_paint;
uniform sampler2D u_detail, u_detail_colours, u_paint_mask, u_addon_mask, u_rust_mask;
uniform vec4 u_paint_values[5]; // set at once (SetPaint()): each a colour and a number of the paint
uniform float u_rust_delay;
#define u_colour (u_paint_values[0].xyz)
#define u_start (u_paint_values[0].w)
#define u_target (u_paint_values[1].xyz)
#define u_fade (u_paint_values[1].w)
#define u_addon_colour (u_paint_values[2].xyz)
#define u_addon_start (u_paint_values[2].w)
#define u_tint (u_paint_values[3].xyz)
#define u_rust_time (u_paint_values[3].w)
#define u_rust_colour (u_paint_values[4].xyz)
#define u_rust_ramp (u_paint_values[4].w)
uniform float u_rust_moments[64];
noperspective in vec2 v_uv;
noperspective in float v_light;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;

const int PAINT_DETAIL = 1, PAINT_BODY = 2, PAINT_MASK = 4, PAINT_COLOURED = 8, PAINT_ADDON = 16, PAINT_TINT = 32, PAINT_RUST = 64, PAINT_GRADUAL = 128, PAINT_PIECES = 256;

/* A mask's red, 0-255, at the texel's middle (u, v of the texture). */
float MaskAt(sampler2D m, vec2 uv)
{
	ivec2 size = textureSize(m, 0);
	return floor(texelFetch(m, min(ivec2(uv * vec2(size)), size - 1), 0).r * 255.0 + 0.5);
}

/* A detail turned (ShiftTrueColourHue()): its hue round by turn 256ths, its saturation and value by 128ths. */
vec3 TurnHue(vec3 c, vec3 by)
{
	vec3 f = c / 255.0;
	float mx = max(f.r, max(f.g, f.b)), mn = min(f.r, min(f.g, f.b)), d = mx - mn;
	float h = 0.0;
	if (d > 1e-9) {
		if (mx == f.r) {
			h = mod((f.g - f.b) / d, 6.0);
		} else if (mx == f.g) {
			h = (f.b - f.r) / d + 2.0;
		} else {
			h = (f.r - f.g) / d + 4.0;
		}
	}
	float s = mx > 0.0 ? d / mx : 0.0, v = mx;
	h = mod(h + (by.x - 128.0) * 6.0 / 256.0 + 6.0, 6.0);
	s = min(1.0, s * by.y / 128.0);
	v = min(1.0, v * by.z / 128.0);
	float cc = v * s, x = cc * (1.0 - abs(mod(h, 2.0) - 1.0)), m = v - cc;
	vec3 rgb;
	int k = int(h);
	if (k == 0) rgb = vec3(cc, x, 0.0);
	else if (k == 1) rgb = vec3(x, cc, 0.0);
	else if (k == 2) rgb = vec3(0.0, cc, x);
	else if (k == 3) rgb = vec3(0.0, x, cc);
	else if (k == 4) rgb = vec3(x, 0.0, cc);
	else rgb = vec3(cc, 0.0, x);
	return clamp(floor((rgb + m) * 255.0 + 0.5), 0.0, 255.0);
}

vec3 Paint(vec3 c, vec2 uv)
{
	bool by_detail = false;
	if ((u_paint & PAINT_DETAIL) != 0) {
		int m = int(MaskAt(u_detail, uv));
		if (m != 0) {
			vec4 first = texelFetch(u_detail_colours, ivec2(0, m), 0);
			float kind = floor(first.a * 255.0 + 0.5);
			if (kind == 255.0) {
				int l = int(max(c.r, max(c.g, c.b)));
				c = floor(texelFetch(u_detail_colours, ivec2(l, m), 0).rgb * 255.0 + 0.5);
				by_detail = true;
			} else if (kind == 128.0) {
				c = TurnHue(c, floor(first.rgb * 255.0 + 0.5));
				by_detail = true;
			}
		}
	}
	if ((u_paint & PAINT_BODY) != 0 && !by_detail) {
		float m = (u_paint & PAINT_MASK) != 0 ? MaskAt(u_paint_mask, uv) / 255.0 : 1.0;
		if (m > 0.0) {
			if ((u_paint & PAINT_COLOURED) != 0) {
				float l = max(c.r, max(c.g, c.b)) / u_start;
				c += (min(vec3(255.0), u_colour * l) - c) * m;
			}
			if (u_fade > 0.0) c += (u_target - c) * (u_fade * m);
		}
	}
	if ((u_paint & PAINT_ADDON) != 0 && !by_detail) {
		float m = MaskAt(u_addon_mask, uv) / 255.0;
		if (m > 0.0) {
			float l = max(c.r, max(c.g, c.b)) / u_addon_start;
			c += (min(vec3(255.0), u_addon_colour * l) - c) * m;
		}
	}
	if ((u_paint & PAINT_TINT) != 0) c = c * u_tint / 255.0;
	if ((u_paint & PAINT_PIECES) != 0) {
		/* The rust of pieces (ThreeDVehicleRust::Pieces()): the mask's red the piece, its green the delay; the piece's
		 * moment the vehicle's own (u_rust_moments). */
		ivec2 rs = textureSize(u_rust_mask, 0);
		vec4 kd = texelFetch(u_rust_mask, min(ivec2(uv * vec2(rs)), rs - 1), 0);
		int k = int(floor(kd.r * 255.0 + 0.5));
		if (k > 0 && k < 64) {
			float f = clamp((u_rust_time - (u_rust_moments[k] + u_rust_delay * floor(kd.g * 255.0 + 0.5) / 255.0)) / u_rust_ramp, 0.0, 1.0);
			c += (u_rust_colour - c) * f;
		}
	} else if ((u_paint & PAINT_RUST) != 0) {
		float m = MaskAt(u_rust_mask, uv);
		if (m > 0.0) {
			float f = (u_paint & PAINT_GRADUAL) != 0 ? clamp((u_rust_time - (m - 1.0) / 254.0) / u_rust_ramp, 0.0, 1.0) : m / 255.0 * u_rust_time;
			c += (u_rust_colour - c) * f;
		}
	}
	return floor(min(vec3(255.0), c));
}

void main()
{
	ivec2 size = textureSize(u_tex, 0);
	vec2 f = v_uv - floor(v_uv);
	ivec2 at = min(ivec2(f * vec2(size)), size - 1);
	vec4 t = texelFetch(u_tex, at, 0);
	if (u_glass == 0 && t == vec4(0.0)) discard;
	/* See-through: the glass's alpha, a material "BLEND"'s times its texel's (the saucer's beam fading down). */
	float alpha = u_texel_alpha != 0 ? u_glass_alpha * t.a : u_glass_alpha;
	if (u_glass != 0 && alpha <= 0.0) discard;
	vec3 own = floor(t.rgb * 255.0 + 0.5);
	/* Painted where the texel is drawn, not at the middle of the texel of the colour's level: the masks have their own
	 * size, and a coarse level of an atlas of colour swatches (the Tatras 1950: 8 texels a swatch) is a few texels a
	 * part -- its middle lies on another part of the mask or none (the player, 18. 11.: "tatra 141 ... v dálce zase
	 * barvu ztratila ve dvou krocích do bleděmodra a pak do šedé"). As the processor paints it (RasterObject()). */
	if (u_paint != 0 && t != vec4(0.0)) own = Paint(own, f);
	/* Its own light (ThreeDVehicleShape::tex_glow): u_glow.a 0 none, 1 the texel times its colour, 2 its colour. */
	vec3 e = u_glow.a == 0.0 ? vec3(0.0) : (u_glow.a < 1.5 ? floor(u_glow.rgb * own / 255.0) : u_glow.rgb);
	vec3 c = floor(min(vec3(255.0), own * v_light + e)) / 255.0;
	o_lit = u_glow.a == 0.0 ? vec4(0.0) : vec4(1.0);
	o_colour = u_glass == 0 ? vec4(c, 1.0) : vec4(c * alpha, alpha);
}
)";

/* The buildings of the view from the eyes (DrawEyeOnCard()), as the processor draws them there (CabRasterRows() of
 * viewport.cpp, the houses' job of the eye's frame): in perspective, each pixel the texel of its texture under it -- none
 * where the texel is 0 or off the texture --, shaded by the triangle's light (256 as it is) to whole numbers; its depth
 * near / how far ahead, as exact as a float holds it (the eye's depth is 1 / how far ahead). */
const char *const EYE_BUILDING_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_light;
uniform mat4 u_matrix;
uniform vec3 u_metres;
out vec2 v_uv;
out vec3 v_m;
flat out float v_light;
void main()
{
	gl_Position = u_matrix * vec4(a_pos, 1.0);
	v_uv = a_uv;
	v_m = a_pos * u_metres;
	v_light = a_light;
}
)";

/* The building's first texture painted as it is drawn (ThreeDGpuBuildingPaint; ThreeDObject::Painted() of
 * three_d_objects.cpp paints a copy the same way): each part where its mask is -- the texel's colour to the part's, as
 * bright as the texel is of its start --, then the part's pattern over it, laid by the surface's own place in metres:
 * across it level, up its slope (a level surface along x and y), a picture every so many metres. */
const char *const EYE_BUILDING_FRAGMENT = R"(#version 330 core
uniform sampler2D u_tex;
uniform float u_near;
uniform int u_parts;
uniform sampler2D u_mask0, u_mask1, u_mask2, u_mask3;
uniform sampler2D u_pattern0, u_pattern1, u_pattern2, u_pattern3;
uniform vec4 u_colour[4]; // red, green, blue (0 .. 255), start
uniform vec4 u_about[4];  // metres a picture of the pattern (0: none), only on a slope
in vec2 v_uv;
in vec3 v_m;
flat in float v_light;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_glow;
float part_of(sampler2D mask, ivec2 at, ivec2 size)
{
	ivec2 ms = textureSize(mask, 0);
	vec4 mp = texelFetch(mask, min(ms - 1, at * ms / size), 0);
	return mp == vec4(0.0) ? 0.0 : mp.r;
}
vec3 painted(vec3 c, float m, vec4 colour, vec4 about, sampler2D pattern, vec3 n, float nl)
{
	if (m <= 0.0) return c;
	float l = max(c.r, max(c.g, c.b)) / colour.a;
	c = clamp(floor(c + (min(vec3(255.0), colour.rgb * l) - c) * m), 0.0, 255.0);
	if (about.x <= 0.0 || nl < 1e-12) return c;
	if (about.y > 0.5 && abs(n.z) >= 0.985) return c;
	vec3 sd = vec3(1.0, 0.0, 0.0), td = vec3(0.0, 1.0, 0.0);
	if (abs(n.z) < 0.985) {
		float nxy = length(n.xy);
		sd = vec3(-n.y / nxy, n.x / nxy, 0.0);
		td = cross(n, sd);
		if (td.z < 0.0) td = -td;
	}
	vec4 q = texture(pattern, vec2(dot(v_m, sd), -dot(v_m, td)) / about.x);
	return clamp(floor(c + (q.rgb * 255.0 - c) * (q.a * m)), 0.0, 255.0);
}
void main()
{
	o_glow = vec4(0.0);
	ivec2 size = textureSize(u_tex, 0);
	ivec2 at = ivec2(floor(v_uv * vec2(size)));
	if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) discard;
	vec4 t = texelFetch(u_tex, at, 0);
	if (t == vec4(0.0)) discard;
	vec3 c = floor(t.rgb * 255.0 + 0.5);
	if (u_parts > 0) {
		vec3 n = cross(dFdx(v_m), dFdy(v_m));
		float nl = length(n);
		if (nl >= 1e-12) n /= nl;
		c = painted(c, part_of(u_mask0, at, size), u_colour[0], u_about[0], u_pattern0, n, nl);
		if (u_parts > 1) c = painted(c, part_of(u_mask1, at, size), u_colour[1], u_about[1], u_pattern1, n, nl);
		if (u_parts > 2) c = painted(c, part_of(u_mask2, at, size), u_colour[2], u_about[2], u_pattern2, n, nl);
		if (u_parts > 3) c = painted(c, part_of(u_mask3, at, size), u_colour[3], u_about[3], u_pattern3, n, nl);
	}
	if (v_light != 256.0) c = min(vec3(255.0), floor(c * v_light / 256.0));
	o_colour = vec4(c / 255.0, 1.0);
	gl_FragDepth = u_near * gl_FragCoord.w;
}
)";

/* The corners of a building among what stands on the turned map (DrawStandingOnCard()), as the processor projects them
 * (DrawObjectInto() of viewport.cpp): its corner to the block, its uv and its light across the triangle, how near. */
const char *const MAP_BUILDING_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_light;
uniform mat4 u_matrix;
uniform vec2 u_depth_range;
uniform vec3 u_metres;
noperspective out vec2 v_uv;
noperspective out float v_light;
noperspective out float v_near;
noperspective out vec3 v_m;
void main()
{
	gl_Position = u_matrix * vec4(a_pos, 1.0);
	v_uv = a_uv;
	v_m = a_pos * u_metres;
	v_light = a_light;
	v_near = u_depth_range.x + (gl_Position.z + 1.0) * 0.5 * (u_depth_range.y - u_depth_range.x);
}
)";

/* The ground of the turned map (DrawGroundOnCard()), as the processor lays it (DrawTexturedTriangle() of viewport.cpp):
 * a corner at its pixel of the block, rows down as the card's rows up; each pixel at its middle the texel under it of
 * its texture (the kept picture of the ground, or a tile's in the atlas, where the texture is in it) -- one the
 * texture has nothing for (0) takes the nearest drawn within three pixels of the sprites (u_step texels each), in the
 * processor's order --, as it is. */
const char *const GROUND_VERTEX = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_bounds;
layout(location = 3) in float a_atlas;
uniform vec2 u_size;
noperspective out vec2 v_uv;
flat out vec4 v_bounds;
flat out float v_atlas;
void main()
{
	gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);
	v_uv = a_uv;
	v_bounds = a_bounds;
	v_atlas = a_atlas;
}
)";

const char *const GROUND_FRAGMENT = R"(#version 330 core
uniform sampler2D u_ground;
uniform sampler2D u_atlas;
noperspective in vec2 v_uv;
flat in vec4 v_bounds;
flat in float v_atlas;
uniform int u_step;
out vec4 o_colour;
const ivec2 NEAR[24] = ivec2[24](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1),
		ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(2, 1), ivec2(-2, 1), ivec2(2, -1), ivec2(-2, -1),
		ivec2(3, 0), ivec2(-3, 0), ivec2(0, 3), ivec2(0, -3), ivec2(3, 1), ivec2(-3, 1), ivec2(3, -1), ivec2(-3, -1));
vec4 Texel(ivec2 at)
{
	ivec2 p = ivec2(v_bounds.xy) + at;
	return v_atlas > 0.5 ? texelFetch(u_atlas, p, 0) : texelFetch(u_ground, p, 0);
}
void main()
{
	ivec2 size = ivec2(v_bounds.zw);
	ivec2 at = ivec2(floor(v_uv));
	if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) discard;
	vec4 t = Texel(at);
	if (t == vec4(0.0)) {
		for (int k = 0; k < 24; k++) {
			ivec2 n = at + NEAR[k] * u_step;
			if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y) continue;
			t = Texel(n);
			if (t != vec4(0.0)) break;
		}
		if (t == vec4(0.0)) discard;
	}
	o_colour = t;
}
)";

/* What stands on the turned map (DrawStandingOnCard()): each picture's texel as it is, its alpha multiplied in, laid
 * over what is there; a lit one (alpha THREE_D_GPU_LIT_ALPHA) whole, marked lit. */
const char *const STANDING_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_order;
uniform vec2 u_size;
noperspective out vec2 v_uv;
flat out float v_order;
void main()
{
	gl_Position = vec4(a_pos.xy / u_size * 2.0 - 1.0, a_pos.z * 2.0 - 1.0, 1.0);
	v_uv = a_uv;
	v_order = a_order;
}
)";

/* The order of what stands on the turned map (ThreeDGpuStandingItem::order) kept by the pixels it covers: its 24 bits
 * in red, green and blue, whole -- laid with what is laid (ONE, ONE_MINUS_SRC_ALPHA), all of it or nothing (alpha 0). */
#define STANDING_ORDER_GLSL \
	"vec4 OrderColour(float order)\n" \
	"{\n" \
	"\tfloat k = floor(order + 0.5);\n" \
	"\tfloat r = floor(k / 65536.0);\n" \
	"\tfloat g = floor((k - r * 65536.0) / 256.0);\n" \
	"\treturn vec4(r, g, k - r * 65536.0 - g * 256.0, 255.0) / 255.0;\n" \
	"}\n" \
	"float NearOrder(float near)\n" \
	"{\n" \
	"\treturn clamp(floor((near + 262144.0) * 32.0 + 0.5), 1.0, 16777215.0);\n" \
	"}\n"

/* How near a pixel of a thing standing on the turned map is (ThreeDGpuStandingItem::box): the view's ray through the
 * pixel -- each point of it at w = xr + yr of the turned world, its height (g w - v) / h, how near w + 2 z -- where it
 * meets the thing's box nearest; past the box, on the upright plane through its middle (mid). g and h the map's tilt
 * (u_tilt, GetViewTilt() of three_d.h: 1 and 1 at the game's 30 degrees, the height w - v, how near 3 w - 2 v). Its
 * depth 0-1 as the buildings' (ThreeDGpuStanding::depth). With STANDING_ORDER_GLSL. */
#define STANDING_NEAR_GLSL \
	"uniform vec4 u_place;\n" \
	"uniform vec4 u_turn;\n" \
	"uniform vec2 u_depth;\n" \
	"uniform vec2 u_tilt;\n" \
	"void Slab(float p0, float d, float lo, float hi, inout vec2 w)\n" \
	"{\n" \
	"\tif (abs(d) < 1e-6) {\n" \
	"\t\tif (p0 < lo || p0 > hi) w = vec2(1e30, -1e30);\n" \
	"\t\treturn;\n" \
	"\t}\n" \
	"\tfloat t0 = (lo - p0) / d, t1 = (hi - p0) / d;\n" \
	"\tw = vec2(max(w.x, min(t0, t1)), min(w.y, max(t0, t1)));\n" \
	"}\n" \
	"float PixelNear(vec3 lo, vec3 hi, float mid)\n" \
	"{\n" \
	"\tvec2 at = (gl_FragCoord.xy + u_place.xy) * u_place.z;\n" \
	"\tfloat u = at.x / (2.0 * u_place.w), v = at.y / u_place.w;\n" \
	"\tfloat c = u_turn.x, s = u_turn.y;\n" \
	"\tfloat x0 = u_turn.z + c * (-0.5 * u - u_turn.z) + s * (0.5 * u - u_turn.w);\n" \
	"\tfloat y0 = u_turn.w - s * (-0.5 * u - u_turn.z) + c * (0.5 * u - u_turn.w);\n" \
	"\tvec2 w = vec2(-1e30, 1e30);\n" \
	"\tSlab(x0, 0.5 * (c + s), lo.x, hi.x, w);\n" \
	"\tSlab(y0, 0.5 * (c - s), lo.y, hi.y, w);\n" \
	"\tSlab(-v / u_tilt.y, u_tilt.x / u_tilt.y, lo.z, hi.z, w);\n" \
	"\tfloat at_w = w.x <= w.y ? w.y : mid;\n" \
	"\treturn at_w + 2.0 * (u_tilt.x * at_w - v) / u_tilt.y;\n" \
	"}\n" \
	"float NearDepth(float near)\n" \
	"{\n" \
	"\treturn clamp((near - u_depth.x) / u_depth.y, 0.0, 1.0);\n" \
	"}\n"

/* The pictures laid on the turned map (DrawStandingOnCard()), as STANDING_VERTEX with each one's box after its order
 * (ThreeDGpuStandingItem::box): its lowest corner and middle, its highest corner and how its depth comes -- 0 by its
 * box, 1 as given (a_pos.z: a figure in a building's depth), 2 as given, its order alone, 3 a vehicle's own, 4 by its box
 * and under every thing that moves (a shadow, ThreeDGpuStandingItem::under_movers) (STANDING_FRAGMENT). */
const char *const STANDING_BOX_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_order;
layout(location = 3) in vec4 a_lo;
layout(location = 4) in vec4 a_hi;
uniform vec2 u_size;
noperspective out vec2 v_uv;
flat out float v_order;
flat out vec4 v_lo;
flat out vec4 v_hi;
flat out float v_depth;
void main()
{
	gl_Position = vec4(a_pos.xy / u_size * 2.0 - 1.0, a_pos.z * 2.0 - 1.0, 1.0);
	v_uv = a_uv;
	v_order = a_order;
	v_lo = a_lo;
	v_hi = a_hi;
	v_depth = a_pos.z;
}
)";

const char *const STANDING_FRAGMENT = "#version 330 core\n" STANDING_ORDER_GLSL STANDING_NEAR_GLSL R"(
uniform sampler2D u_texture;
uniform int u_moving;
uniform sampler2D u_orders;
uniform sampler2D u_near_tex;
noperspective in vec2 v_uv;
flat in float v_order;
flat in vec4 v_lo;
flat in vec4 v_hi;
flat in float v_depth;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;
layout(location = 2) out vec4 o_order;
void main()
{
	vec4 t = texelFetch(u_texture, ivec2(floor(v_uv)), 0);
	if (t.a == 0.0) discard;
	/* How near this pixel of it is: by its box, as given, or a vehicle's by its own depth kept beside the atlas
	 * (how 3: v_lo its nearness's least and span, how near the point it hangs from is). */
	float order = v_order, depth = v_depth;
	if (v_hi.w > 3.5) {
		/* A shadow among the things that move (how 4): against what stands by its box, under every thing that moves --
		 * the furthest of them, none of them hidden by it whichever is laid first. */
		order = NearOrder(PixelNear(v_lo.xyz, v_hi.xyz, v_lo.w));
		depth = 0.0;
	} else if (v_hi.w > 2.5) {
		vec4 n = texelFetch(u_near_tex, ivec2(floor(v_uv)), 0);
		float near = v_lo.w + v_lo.x + (floor(n.r * 255.0 + 0.5) * 256.0 + floor(n.g * 255.0 + 0.5)) / 65535.0 * v_lo.y;
		order = NearOrder(near);
		depth = NearDepth(near);
	} else if (v_hi.w < 0.5) {
		float near = PixelNear(v_lo.xyz, v_hi.xyz, v_lo.w);
		order = NearOrder(near);
		depth = NearDepth(near);
	}
	/* A thing that moves (ThreeDGpuStanding::moving): hidden where what stands there is nearer. */
	if (u_moving != 0) {
		vec4 o = texelFetch(u_orders, ivec2(gl_FragCoord.xy), 0);
		float under = floor(o.r * 255.0 + 0.5) * 65536.0 + floor(o.g * 255.0 + 0.5) * 256.0 + floor(o.b * 255.0 + 0.5);
		if (order < under) discard;
	}
	/* Lit at night (ThreeDGpuStandingAlpha()): of alpha 254 whole; of an odd one below it see-through (a lighthouse's
	 * beam) -- how much of the pixel is lit with its alpha in it, as its colour. */
	float ta = floor(t.a * 255.0 + 0.5);
	float lit = ta == 254.0 || (ta < 254.0 && mod(ta, 2.0) > 0.5) ? 1.0 : 0.0;
	float a = ta == 254.0 ? 1.0 : t.a;
	o_colour = vec4(t.rgb, a);
	o_lit = vec4(lit * a, 0.0, 0.0, a);
	o_order = a >= 0.5 ? OrderColour(order) : vec4(0.0);
	gl_FragDepth = depth;
}
)";

/* A vehicle among what stands on the turned map (ThreeDGpuStandingVehicle), with STANDING_VERTEX: shrunk from its
 * picture on the card into the pictures kept there as FinishGpuObject() of three_d_vehicle_object.cpp shrinks it --
 * each pixel the mean of its subpixels (their alpha multiplied in); one any of them is lit of, its colour whole, of
 * alpha THREE_D_GPU_LIT_ALPHA, as a picture's lit pixel (STANDING_FRAGMENT). */
/* Its corners with how near its pixels are as attributes (the near weights and the first row, its supersampling in
 * w; the depths and the box's nearness), all the vehicles of a frame shrunk by one drawing (the player's diary of
 * 18. 11.: four calls a vehicle as uniforms). */
const char *const STANDING_SHRINK_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_near;
layout(location = 3) in vec4 a_near_range;
uniform vec2 u_size;
noperspective out vec2 v_uv;
flat out vec4 v_near;
flat out vec4 v_near_range;
void main()
{
	gl_Position = vec4(a_pos.xy / u_size * 2.0 - 1.0, a_pos.z * 2.0 - 1.0, 1.0);
	v_uv = a_uv;
	v_near = a_near;
	v_near_range = a_near_range;
}
)";

const char *const STANDING_SHRINK_FRAGMENT = R"(#version 330 core
uniform sampler2D u_colour;
uniform sampler2D u_lit;
uniform sampler2D u_depth;
flat in vec4 v_near;
flat in vec4 v_near_range;
#define u_near v_near
#define u_near_range v_near_range
#define u_ss int(v_near.w + 0.5)
noperspective in vec2 v_uv;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_near;
void main()
{
	ivec2 at = ivec2(floor(v_uv)) * u_ss;
	vec4 sum = vec4(0.0);
	float lit = 0.0;
	/* How near the nearest of its subpixels is (ThreeDGpuStandingVehicle::near_osy): its row and its depth in the
	 * picture, u_near its weights and the row of the picture's first, u_near_range the picture's depths and how near
	 * its box is, nearest to furthest -- 0-1 between them in 16 bits. */
	float best = -1e30;
	for (int dy = 0; dy < u_ss; dy++) {
		for (int dx = 0; dx < u_ss; dx++) {
			vec4 c = texelFetch(u_colour, at + ivec2(dx, dy), 0);
			sum += c;
			lit = max(lit, texelFetch(u_lit, at + ivec2(dx, dy), 0).r);
			if (c.a > 0.0) {
				float od = mix(u_near_range.x, u_near_range.y, texelFetch(u_depth, at + ivec2(dx, dy), 0).r);
				best = max(best, u_near.x * (u_near.z + float(at.y + dy) + 0.5) + u_near.y * od);
			}
		}
	}
	float k = best > -1e29 ? floor(clamp((best - u_near_range.z) / u_near_range.w, 0.0, 1.0) * 65535.0 + 0.5) : 0.0;
	float hi = floor(k / 256.0);
	o_near = vec4(hi / 255.0, (k - hi * 256.0) / 255.0, 0.0, 1.0);
	if (sum.a <= 0.0) {
		o_colour = vec4(0.0);
		return;
	}
	/* Its alpha as ThreeDGpuStandingAlpha() gives it: lit nearly whole whole, its colour as it is; see-through odd lit
	 * (a lighthouse's beam), even not; nearly whole as whole. */
	vec4 c = sum / float(u_ss * u_ss);
	float a = floor(c.a * 255.0 + 0.5);
	if (lit > 0.0) {
		if (a >= 252.0) {
			o_colour = vec4(sum.rgb / sum.a, 254.0 / 255.0);
			return;
		}
		a = a - mod(a, 2.0) + 1.0;
	} else if (a >= 253.0) {
		o_colour = vec4(sum.rgb / sum.a, 1.0);
		return;
	} else {
		a -= mod(a, 2.0);
	}
	if (a <= 0.0) {
		o_colour = vec4(0.0);
		return;
	}
	o_colour = vec4(min(c.rgb / c.a, vec3(1.0)) * (a / 255.0), a / 255.0);
}
)";

/* A rect of a picture on the card cleared by drawing over it (ClearInScissor()), with STANDING_VERTEX: its colours to
 * nothing; the depth that of the corners (the depth test passing always). */
const char *const CLEAR_FRAGMENT = R"(#version 330 core
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;
layout(location = 2) out vec4 o_order;
void main()
{
	o_colour = vec4(0.0);
	o_lit = vec4(0.0);
	o_order = vec4(0.0);
}
)";

/* What stands on the view copied under the moving things (ThreeDGpuStanding::moving), with STANDING_VERTEX: each pixel
 * of the rects drawn its colour and whether it is lit, as they are. */
const char *const COPY_FRAGMENT = R"(#version 330 core
uniform sampler2D u_colour;
uniform sampler2D u_lit;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;
void main()
{
	ivec2 at = ivec2(gl_FragCoord.xy);
	o_colour = texelFetch(u_colour, at, 0);
	o_lit = texelFetch(u_lit, at, 0);
}
)";

/* The faces of a box among what stands on the turned map (ThreeDGpuStandingFaces): a corner at its pixel of the view's
 * picture, its texel's place in the texture; each pixel at its middle the texel under it of the texture where it is
 * among the pictures (u_bounds) -- one it has nothing for (0) the nearest drawn within three when u_fill, in the
 * processor's order --, whole, not lit. */
const char *const STANDING_FACES_VERTEX = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_near;
uniform vec2 u_size;
noperspective out vec2 v_uv;
noperspective out float v_near;
void main()
{
	gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);
	v_uv = a_uv;
	v_near = a_near;
}
)";

const char *const STANDING_FACES_FRAGMENT = "#version 330 core\n" STANDING_ORDER_GLSL R"(
uniform sampler2D u_texture;
uniform vec4 u_bounds;
uniform int u_fill;
uniform vec2 u_depth;
noperspective in vec2 v_uv;
noperspective in float v_near;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;
layout(location = 2) out vec4 o_order;
const ivec2 NEAR[24] = ivec2[24](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1),
		ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(2, 1), ivec2(-2, 1), ivec2(2, -1), ivec2(-2, -1),
		ivec2(3, 0), ivec2(-3, 0), ivec2(0, 3), ivec2(0, -3), ivec2(3, 1), ivec2(-3, 1), ivec2(3, -1), ivec2(-3, -1));
void main()
{
	ivec2 size = ivec2(u_bounds.zw);
	ivec2 at = ivec2(floor(v_uv));
	if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) discard;
	vec4 t = texelFetch(u_texture, ivec2(u_bounds.xy) + at, 0);
	if (t == vec4(0.0) && u_fill != 0) {
		for (int k = 0; k < 24; k++) {
			ivec2 n = at + NEAR[k];
			if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y) continue;
			t = texelFetch(u_texture, ivec2(u_bounds.xy) + n, 0);
			if (t != vec4(0.0)) break;
		}
	}
	if (t == vec4(0.0)) discard;
	/* How near this pixel of it is: across the face from its corners (ThreeDGpuStandingFaces::corners). */
	o_colour = vec4(t.rgb, 1.0);
	o_lit = vec4(0.0, 0.0, 0.0, 1.0);
	o_order = OrderColour(NearOrder(v_near));
	gl_FragDepth = clamp((v_near - u_depth.x) / u_depth.y, 0.0, 1.0);
}
)";

/* A building among what stands on the turned map (DrawStandingOnCard()), with MAP_BUILDING_VERTEX: each pixel at its
 * middle the texel under it -- one the texture has nothing for (0) takes the nearest drawn within three, as the
 * processor (DrawTexturedTriangle() of viewport.cpp) --, as see-through as its texel (a yard's grass with the paths of the ground through it,
 * the colleague's pv_dvur_trava), not lit; or its lit glass at night (u_night), each pixel its texel as it is where the
 * texture has one, whole, marked lit. Its first texture painted as it is drawn where the card paints it (u_parts, as
 * EYE_BUILDING_FRAGMENT: no painted copy of the texture a house; u_turn the side the surface's normal is taken to, the
 * block's rows running down). */
const char *const STANDING_BUILDING_FRAGMENT = "#version 330 core\n" STANDING_ORDER_GLSL R"(
uniform sampler2D u_tex;
uniform int u_night;
uniform float u_order;
uniform int u_parts;
uniform float u_turn;
uniform sampler2D u_mask0, u_mask1, u_mask2, u_mask3;
uniform sampler2D u_pattern0, u_pattern1, u_pattern2, u_pattern3;
uniform vec4 u_colour[4]; // red, green, blue (0 .. 255), start
uniform vec4 u_about[4];  // metres a picture of the pattern (0: none), only on a slope
noperspective in vec2 v_uv;
noperspective in float v_light;
noperspective in float v_near;
noperspective in vec3 v_m;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;
layout(location = 2) out vec4 o_order;
const ivec2 NEAR[24] = ivec2[24](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1),
		ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(2, 1), ivec2(-2, 1), ivec2(2, -1), ivec2(-2, -1),
		ivec2(3, 0), ivec2(-3, 0), ivec2(0, 3), ivec2(0, -3), ivec2(3, 1), ivec2(-3, 1), ivec2(3, -1), ivec2(-3, -1));
float part_of(sampler2D mask, ivec2 at, ivec2 size)
{
	ivec2 ms = textureSize(mask, 0);
	vec4 mp = texelFetch(mask, min(ms - 1, at * ms / size), 0);
	return mp == vec4(0.0) ? 0.0 : mp.r;
}
vec3 painted(vec3 c, float m, vec4 colour, vec4 about, sampler2D pattern, vec3 n, float nl)
{
	if (m <= 0.0) return c;
	float l = max(c.r, max(c.g, c.b)) / colour.a;
	c = clamp(floor(c + (min(vec3(255.0), colour.rgb * l) - c) * m), 0.0, 255.0);
	if (about.x <= 0.0 || nl < 1e-12) return c;
	if (about.y > 0.5 && abs(n.z) >= 0.985) return c;
	vec3 sd = vec3(1.0, 0.0, 0.0), td = vec3(0.0, 1.0, 0.0);
	if (abs(n.z) < 0.985) {
		float nxy = length(n.xy);
		sd = vec3(-n.y / nxy, n.x / nxy, 0.0);
		td = cross(n, sd);
		if (td.z < 0.0) td = -td;
	}
	vec4 q = texture(pattern, vec2(dot(v_m, sd), -dot(v_m, td)) / about.x);
	return clamp(floor(c + (q.rgb * 255.0 - c) * (q.a * m)), 0.0, 255.0);
}
void main()
{
	ivec2 size = textureSize(u_tex, 0);
	ivec2 at = ivec2(floor(v_uv * vec2(size)));
	if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) discard;
	vec4 t = texelFetch(u_tex, at, 0);
	if (t == vec4(0.0)) {
		if (u_night != 0) discard;
		for (int k = 0; k < 24; k++) {
			ivec2 n = at + NEAR[k];
			if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y) continue;
			t = texelFetch(u_tex, n, 0);
			if (t != vec4(0.0)) {
				at = n;
				break;
			}
		}
		if (t == vec4(0.0)) discard;
	}
	float a = u_night != 0 ? 1.0 : t.a;
	if (a == 0.0) discard;
	vec3 c = floor(t.rgb * 255.0 + 0.5);
	if (u_parts > 0) {
		vec3 n = cross(dFdx(v_m), dFdy(v_m)) * u_turn;
		float nl = length(n);
		if (nl >= 1e-12) n /= nl;
		c = painted(c, part_of(u_mask0, at, size), u_colour[0], u_about[0], u_pattern0, n, nl);
		if (u_parts > 1) c = painted(c, part_of(u_mask1, at, size), u_colour[1], u_about[1], u_pattern1, n, nl);
		if (u_parts > 2) c = painted(c, part_of(u_mask2, at, size), u_colour[2], u_about[2], u_pattern2, n, nl);
		if (u_parts > 3) c = painted(c, part_of(u_mask3, at, size), u_colour[3], u_about[3], u_pattern3, n, nl);
	}
	if (u_night == 0) {
		float l = floor(max(0.0, v_light));
		c = min(vec3(255.0), floor(c * l / 256.0));
	}
	o_colour = vec4(c / 255.0 * a, a);
	o_lit = vec4(u_night != 0 ? 1.0 : 0.0, 0.0, 0.0, a);
	/* Its order by how near this pixel of it is (its depth as all laid on the block, ThreeDGpuStanding::depth). */
	o_order = a >= 0.5 ? OrderColour(NearOrder(v_near)) : vec4(0.0);
}
)";

/**
 * The shapes' program for the view from the eyes (DrawEyeOnCard()), made of MESH_VERTEX and MESH_FRAGMENT: in
 * perspective (the light and the texture's place across a triangle as it stands in the world, not straight across the
 * picture), its depth near / how far ahead as the buildings'.
 */
std::string EyeMeshSource(const char *source, bool fragment)
{
	std::string s = source;
	auto replace = [&s](std::string_view what, std::string_view with) {
		const size_t at = s.find(what);
		if (at != std::string::npos) s.replace(at, what.size(), with);
	};
	for (size_t at; (at = s.find("noperspective ")) != std::string::npos;) s.erase(at, 14);
	if (fragment) {
		s.insert(s.find('\n') + 1, "uniform float u_near;\n");
		s.insert(s.rfind('}'), "\tgl_FragDepth = u_near * gl_FragCoord.w;\n");
		/* Its own light alone in the second picture, as glow_after_night of viewport.cpp draws it over the dark: the
		 * texel by its colour's strongest, or -- a material shining its colour alone (u_glow.a 2), as by day -- that
		 * colour: the fire Tatra's blue beacon was the red of its body's paint by night (the player, 16. 11.: "the
		 * fire 148 blinks red ... I switch the night off and it shines blue"). See-through by the glass's alpha. */
		replace("o_lit = u_glow.a == 0.0 ? vec4(0.0) : vec4(1.0);", "o_lit = u_glow.a == 0.0 ? vec4(0.0) : vec4((u_glow.a < 1.5 ? floor(own * max(u_glow.r, max(u_glow.g, u_glow.b)) / 256.0) : u_glow.rgb) / 255.0, 1.0);\n\tif (u_glass != 0) o_lit *= alpha;");
	} else {
		/* A wheel that turns (its corners' wheel in attribute 3): its corners and normals moved by its matrix first. */
		s.insert(s.find('\n') + 1, "layout(location = 3) in float a_wheel;\nuniform mat4 u_wheels[32];\nuniform int u_wheel_count;\n");
		replace("gl_Position = u_matrix * vec4(pos, 1.0);", "int k = int(a_wheel + 0.5);\n\tvec3 p = pos, n = nrm;\n\tif (k > 0 && k <= u_wheel_count) {\n\t\tp = (u_wheels[k - 1] * vec4(p, 1.0)).xyz;\n\t\tn = mat3(u_wheels[k - 1]) * n;\n\t}\n\tgl_Position = u_matrix * vec4(p, 1.0);");
		replace("dot(nrm, u_light)", "dot(n, u_light)");
		/* Many of a shape at once (EyeObjectDraws::Batch): each instance's matrix (its rows) and light in attributes
		 * 4-8 when u_instanced is set, else the uniforms as for one. */
		s.insert(s.find('\n') + 1, "layout(location = 4) in vec4 a_m0;\nlayout(location = 5) in vec4 a_m1;\nlayout(location = 6) in vec4 a_m2;\nlayout(location = 7) in vec4 a_m3;\nlayout(location = 8) in vec4 a_light;\nuniform int u_instanced;\n");
		replace("gl_Position = u_matrix * vec4(p, 1.0);", "vec4 p4 = vec4(p, 1.0);\n\tgl_Position = u_instanced != 0 ? vec4(dot(a_m0, p4), dot(a_m1, p4), dot(a_m2, p4), dot(a_m3, p4)) : u_matrix * p4;");
		replace("v_light = u_lit > 0.5 ? 0.55 + 0.45 * max(0.0, dot(n, u_light)) : 1.0;", "vec3 light = u_instanced != 0 ? a_light.xyz : u_light;\n\tfloat lit = u_instanced != 0 ? a_light.w : u_lit;\n\tv_light = lit > 0.5 ? 0.55 + 0.45 * max(0.0, dot(n, light)) : 1.0;");
	}
	return s;
}

/* The glass of the lights at night: placed on the picture already, in its colour, marked lit. */
const char *const NIGHT_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_colour;
uniform mat4 u_matrix;
flat out vec3 v_colour;
void main()
{
	gl_Position = u_matrix * vec4(a_pos, 1.0);
	v_colour = a_colour;
}
)";

const char *const NIGHT_FRAGMENT = R"(#version 330 core
flat in vec3 v_colour;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;
void main()
{
	o_colour = vec4(v_colour, 1.0);
	o_lit = vec4(1.0);
}
)";

/*
 * The whole view from the eyes on the card (DrawEyeFrameOnCard()): its triangles placed on the picture already, as
 * CabRasterRows() of viewport.cpp draws them -- in perspective, each pixel the texel of its texture under it (its place
 * in the texture holding it: v_rect), a hole filled from the nearest texel within three where asked, the texture
 * repeated where asked, else none off its edges; shaded by its light (256 as it is) to whole numbers; see-through by
 * its alpha. Its depth as the objects' (u_near / how far ahead), its own light none.
 */
const char *const EYE_TRI_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_rect;
layout(location = 3) in vec3 a_how;
uniform vec2 u_size;
out vec2 v_uv;
flat out vec4 v_rect;
flat out float v_light;
noperspective out float v_corner_light;
flat out vec2 v_how;
void main()
{
	float w = 1.0 / a_pos.z;
	gl_Position = vec4((2.0 * a_pos.x / u_size.x - 1.0) * w, (2.0 * a_pos.y / u_size.y - 1.0) * w, 0.0, w);
	v_uv = a_uv;
	v_rect = a_rect;
	v_light = a_how.x;
	v_corner_light = a_how.x;
	v_how = a_how.yz;
}
)";

const char *const EYE_TRI_FRAGMENT = R"(#version 330 core
uniform sampler2D u_tex;
uniform float u_near;
uniform sampler2D u_grass;
uniform float u_grass_units; // world units its picture runs over each way; 0 none
uniform vec2 u_grass_eye;    // the eye's x, y within such a run
uniform vec3 u_grass_tint[14];
uniform vec4 u_eye_dir;      // fx, fy, rx, ry
uniform vec3 u_eye_lens;     // focal, cx, cy
uniform vec2 u_eye_tip;      // the sine and cosine of the eye's look tipped down (CabEye::sp, cp)
/* How far along the level the world point the pixel shows is from the eye (EyeToWorld() of viewport.cpp). */
float LevelAhead(float f)
{
	return f * u_eye_tip.y + (u_eye_lens.z - gl_FragCoord.y) * f / u_eye_lens.x * u_eye_tip.x;
}
in vec2 v_uv;
flat in vec4 v_rect;
flat in float v_light;
noperspective in float v_corner_light;
flat in vec2 v_how;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_glow;
const ivec2 NEAR[24] = ivec2[24](
	ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1),
	ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(2, 1), ivec2(-2, 1), ivec2(2, -1), ivec2(-2, -1),
	ivec2(3, 0), ivec2(-3, 0), ivec2(0, 3), ivec2(0, -3), ivec2(3, 1), ivec2(-3, 1), ivec2(3, -1), ivec2(-3, -1));
void main()
{
	ivec2 size = ivec2(v_rect.zw), origin = ivec2(v_rect.xy);
	int how = int(v_how.y + 0.5);
	ivec2 at = ivec2(floor(v_uv));
	if ((how & 2) != 0) {
		at -= size * ivec2(floor(vec2(at) / vec2(size)));
		at = clamp(at, ivec2(0), size - 1);
	} else if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) {
		discard;
	}
	vec4 t = texelFetch(u_tex, origin + at, 0);
	if (t == vec4(0.0) && (how & 1) != 0) {
		for (int k = 0; k < 24; k++) {
			ivec2 n = at + NEAR[k];
			if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y) continue;
			t = texelFetch(u_tex, origin + n, 0);
			if (t != vec4(0.0)) break;
		}
	}
	if (t == vec4(0.0)) discard;
	/* The edge between our grass and the ground not grass (a road, its marks, a pavement, bare land; CabGrassEdgeTexel()
	 * of viewport.cpp): grass where the texels about the point are grass by half or more, along the row a texel each
	 * way (five points, the ends half) -- the sprite's 2:1 stairs of an edge straight in the world a straight line
	 * again; not by a river's bank or water. */
	if ((how & 32) != 0 && u_grass_units > 0.0) {
		float fv = v_uv.y - 0.5;
		int bv = int(floor(fv));
		float wv = fv - float(bv);
		int kinds = 0;
		bool plain = true;
		ivec2 b0 = ivec2(floor(v_uv - 0.5));
		for (int k = 0; k < 4; k++) {
			vec4 q = texelFetch(u_tex, origin + clamp(b0 + ivec2(k & 1, k >> 1), ivec2(0), size - 1), 0);
			if (q == vec4(0.0)) continue;
			int qa = int(q.a * 255.0 + 0.5);
			if (qa < 252) plain = false;
			kinds |= qa == 252 ? 1 : 2;
		}
		if (plain && kinds == 3) {
			float grass = 0.0, drawn = 0.0, best_grass = -1.0, best_other = -1.0;
			vec4 a_grass = vec4(0.0), an_other = vec4(0.0);
			for (int s = -2; s <= 2; s++) {
				float fu = v_uv.x - 0.5 + float(s) * 0.5;
				int bu = int(floor(fu));
				float wu = fu - float(bu);
				for (int k = 0; k < 4; k++) {
					ivec2 o = ivec2(k & 1, k >> 1);
					vec4 q = texelFetch(u_tex, origin + clamp(ivec2(bu, bv) + o, ivec2(0), size - 1), 0);
					if (q == vec4(0.0)) continue;
					int qa = int(q.a * 255.0 + 0.5);
					if (qa < 252) plain = false;
					float wk = (o.x == 1 ? wu : 1.0 - wu) * (o.y == 1 ? wv : 1.0 - wv) * (abs(s) == 2 ? 0.5 : 1.0);
					drawn += wk;
					float near = s == 0 ? wk : 0.0;
					if (qa == 252) {
						grass += wk;
						if (near > best_grass) { best_grass = near; a_grass = q; }
					} else if (near > best_other) {
						best_other = near;
						an_other = q;
					}
				}
			}
			if (plain && drawn > 0.0) {
				bool is_grass = int(t.a * 255.0 + 0.5) == 252;
				if (grass >= 0.5 * drawn) {
					if (!is_grass && a_grass != vec4(0.0)) t = a_grass;
				} else if (is_grass && an_other != vec4(0.0)) {
					t = an_other;
				}
			}
		}
	}
	/* A road's texel of the ground (THREE_D_GPU_EYE_ROAD): between the four about the point, of the road's alone --
	 * its surface as they blend, a white mark where the marks have two fifths of them or more (a mark of two texels
	 * touching at a corner one, not two), so that its edge is sharp however near (DrawEyeRoad() of viewport.cpp,
	 * CabRoadTexel()). */
	if ((how & 16) != 0) {
		int ta = int(t.a * 255.0 + 0.5);
		if (ta == 253 || ta == 254) {
			vec2 f = v_uv - 0.5;
			ivec2 b = ivec2(floor(f));
			vec2 w = f - vec2(b);
			vec3 mark = vec3(0.0), surface = vec3(0.0);
			float m = 0.0, r = 0.0;
			for (int k = 0; k < 4; k++) {
				ivec2 o = ivec2(k & 1, k >> 1);
				vec4 q = texelFetch(u_tex, origin + clamp(b + o, ivec2(0), size - 1), 0);
				int qa = int(q.a * 255.0 + 0.5);
				float wk = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
				if (qa == 254) {
					m += wk;
					mark += wk * q.rgb;
				} else if (qa == 253) {
					surface += wk * q.rgb;
				}
				if (qa == 253 || qa == 254) r += wk;
			}
			if (r > 0.0) t.rgb = m >= 0.4 * r ? mark / max(m, 1e-4) : surface / max(r - m, 1e-4);
		} else if (ta == 251) {
			/* Water (DrawEyeWater() of viewport.cpp): between the water's texels alone, its banks and foam sharp. */
			vec2 f = v_uv - 0.5;
			ivec2 b = ivec2(floor(f));
			vec2 w = f - vec2(b);
			vec3 sum = vec3(0.0);
			float r = 0.0;
			for (int k = 0; k < 4; k++) {
				ivec2 o = ivec2(k & 1, k >> 1);
				vec4 q = texelFetch(u_tex, origin + clamp(b + o, ivec2(0), size - 1), 0);
				if (int(q.a * 255.0 + 0.5) != 251) continue;
				float wk = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
				sum += wk * q.rgb;
				r += wk;
			}
			if (r > 1e-4) t.rgb = sum / r;
		}
	}
	/* The edge of a river's bank (THREE_D_GPU_BANK_EDGE + its sides of the land, DrawEyeWater() of viewport.cpp): our
	 * grass where the world point the pixel shows is inside the bank's wavy edge (EyeRiverBankInside(), CabBankTexel():
	 * THREE_D_GPU_BANK_REACH 1.75 units in from a side of the land, waves of 8 and 16/3 units along it), else the water's
	 * colour of the texel -- round however near. The point known within a run of our grass, of whole tiles of 16 units. */
	int bank_mark = int(t.a * 255.0 + 0.5);
	if ((how & 32) != 0 && u_grass_units > 0.0 && bank_mark > 234 && bank_mark < 250) {
		float f = 1.0 / gl_FragCoord.w;
		float side = (gl_FragCoord.x - u_eye_lens.y) * f / u_eye_lens.x, level = LevelAhead(f);
		vec2 world = u_grass_eye + vec2(level * u_eye_dir.x + side * u_eye_dir.z, level * u_eye_dir.y + side * u_eye_dir.w);
		vec2 l = world - 16.0 * floor(world / 16.0);
		vec2 reach = 1.75 + 0.2 * sin(world * 0.7853982) + 0.15 * sin(world * 1.1780972 + 1.3);
		int sides = bank_mark - 234;
		float inside = -1e9;
		if ((sides & 1) != 0) inside = max(inside, reach.y - l.x);
		if ((sides & 2) != 0) inside = max(inside, reach.x - (16.0 - l.y));
		if ((sides & 4) != 0) inside = max(inside, reach.y - (16.0 - l.x));
		if ((sides & 8) != 0) inside = max(inside, reach.x - l.y);
		t = inside > 0.0 ? vec4(3.0 / 255.0, 128.0 / 255.0, 0.0, 252.0 / 255.0) : vec4(t.rgb, 1.0);
	}
	/* Our grass's texel of the ground (THREE_D_GPU_EYE_GRASS): its colour our grass's picture at the world point the pixel
	 * shows, between its four texels, repeated; tinted to its kind (red), lit as the game's grass is there (green,
	 * 128ths) -- DrawOurGrass() and CabGrassTexel() of viewport.cpp. */
	if ((how & 32) != 0 && u_grass_units > 0.0 && int(t.a * 255.0 + 0.5) == 252) {
		float f = 1.0 / gl_FragCoord.w;
		float side = (gl_FragCoord.x - u_eye_lens.y) * f / u_eye_lens.x, level = LevelAhead(f);
		vec2 world = u_grass_eye + vec2(level * u_eye_dir.x + side * u_eye_dir.z, level * u_eye_dir.y + side * u_eye_dir.w);
		ivec2 gs = textureSize(u_grass, 0);
		vec2 g = world * vec2(gs) / u_grass_units - 0.5;
		ivec2 b = ivec2(floor(g));
		vec2 w = g - vec2(b);
		vec3 sum = vec3(0.0);
		for (int k = 0; k < 4; k++) {
			ivec2 o = ivec2(k & 1, k >> 1);
			ivec2 q = b + o;
			q -= gs * ivec2(floor(vec2(q) / vec2(gs)));
			float wk = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
			sum += wk * floor(texelFetch(u_grass, clamp(q, ivec2(0), gs - 1), 0).rgb * 255.0 + 0.5);
		}
		int kind = clamp(int(t.r * 255.0 + 0.5), 0, 13);
		float lit = floor(t.g * 255.0 + 0.5) / 128.0;
		t = vec4(clamp(floor(sum * u_grass_tint[kind] * lit), 0.0, 255.0) / 255.0, 1.0);
	}
	/* See-through by so many 256ths; a material "BLEND"'s times its texel's alpha (THREE_D_GPU_EYE_TEXEL_ALPHA). */
	float alpha = v_how.x >= 256.0 ? 1.0 : v_how.x / 256.0;
	if ((how & 8) != 0) alpha *= t.a;
	if (alpha <= 0.0) discard;
	vec3 c = floor(t.rgb * 255.0 + 0.5);
	float light = (how & 4) != 0 ? v_corner_light : v_light;
	if (light != 256.0) c = min(vec3(255.0), floor(c * light / 256.0));
	o_colour = vec4(c / 255.0, alpha);
	o_glow = vec4(0.0);
	gl_FragDepth = u_near * gl_FragCoord.w;
}
)";

/* The jobs over the whole picture: one triangle over it, each pixel computed from the picture so far (u_colour), its
 * depth (u_depth: u_near / how far ahead, 0 nothing) and the job's own numbers, as the processor computes it. */
const char *const EYE_PIXEL_VERTEX = R"(#version 330 core
void main()
{
	gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1), 0.0, 1.0);
}
)";

/* What the jobs share: the pixel, its colour 0-255, its 1 / how far ahead, where it is in the world from the eye. */
const char *const EYE_PIXEL_COMMON = R"(#version 330 core
uniform sampler2D u_colour;
uniform sampler2D u_depth;
uniform float u_near;
uniform vec4 u_eye_dir;   // fx, fy, rx, ry
uniform vec3 u_eye_lens;  // focal, cx, cy
uniform vec2 u_eye_tip;   // the sine and cosine of the eye's look tipped down (CabEye::sp, cp)
layout(location = 0) out vec4 o_colour;
ivec2 Pixel() { return ivec2(gl_FragCoord.xy); }
vec3 ColourAt(ivec2 p) { return floor(texelFetch(u_colour, p, 0).rgb * 255.0 + 0.5); }
float IwAt(ivec2 p) { return texelFetch(u_depth, p, 0).r / u_near; }
/* CabPixelInWorld(): x, y from the eye, the height as x and y from the eye's. */
vec3 InWorld(vec2 s, float iw)
{
	float f = 1.0 / iw;
	float r = (s.x - u_eye_lens.y) * f / u_eye_lens.x, u = (u_eye_lens.z - s.y) * f / u_eye_lens.x;
	float level = f * u_eye_tip.y + u * u_eye_tip.x, up = u * u_eye_tip.y - f * u_eye_tip.x;
	return vec3(level * u_eye_dir.x + r * u_eye_dir.z, level * u_eye_dir.y + r * u_eye_dir.w, up);
}
void Put(vec3 c) { o_colour = vec4(c / 255.0, 1.0); }
)";

/* The sky down to the horizon, the far ground under it (CollectEyeFrame()). */
const char *const EYE_BACKGROUND_FRAGMENT = R"(
uniform float u_horizon;
void main()
{
	float y = floor(gl_FragCoord.y);
	if (y < u_horizon) {
		float t = y / u_horizon;
		Put(floor(vec3(60.0, 110.0, 210.0) + vec3(105.0, 90.0, 30.0) * t));
	} else {
		Put(vec3(84.0, 120.0, 64.0));
	}
}
)";

/* The haze: the far fades into its colour, the more the farther. */
const char *const EYE_HAZE_FRAGMENT = R"(
uniform vec3 u_haze;
uniform float u_from, u_reach;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float z = IwAt(p);
	if (z > 0.0) {
		float dist = 1.0 / z;
		if (dist > u_from) c = floor(c + (u_haze - c) * ((1.0 - exp(-(dist - u_from) / u_reach)) * 0.8));
	}
	Put(c);
}
)";

/* The haze far off: from u_from to u_to all of it; the ground beyond the land (nothing drawn under the horizon) all haze. */
const char *const EYE_FAR_HAZE_FRAGMENT = R"(
uniform vec3 u_haze;
uniform vec3 u_absorb; // under the water: what it takes of red, green and blue a world unit past u_from; 0 the haze from u_from to u_to
uniform float u_from, u_to, u_horizon;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float z = IwAt(p);
	vec3 f = vec3(0.0);
	if (z <= 0.0) {
		if (float(p.y) >= u_horizon) f = vec3(1.0);
	} else if (u_absorb != vec3(0.0)) {
		f = 1.0 - exp(-u_absorb * max(0.0, 1.0 / z - u_from));
	} else {
		f = vec3(clamp((1.0 / z - u_from) / (u_to - u_from), 0.0, 1.0));
	}
	if (f != vec3(0.0)) c = floor(c + (u_haze - c) * f);
	Put(c);
}
)";

/* The sun's shadows (DrawEyeShadows()): darker where something is nearer to the sun on the map of the shadows, unless
 * the surface is turned away from the sun (dark of its own light). The map's u, v and the sun's depth from the eye. */
const char *const EYE_SHADOW_FRAGMENT = R"(
uniform sampler2D u_map;
uniform vec3 u_sun, u_a, u_b;
uniform vec4 u_map_place; // u0, v0, texel, bias (from the eye)
uniform float u_sun0;     // the sun's depth of the eye
uniform vec2 u_size;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float iw = IwAt(p);
	if (iw > 0.0 && p.x < int(u_size.x) - 1 && p.y < int(u_size.y) - 1) {
		vec3 w = InWorld(vec2(p) + 0.5, iw);
		float u = (dot(u_a, w) - u_map_place.x) / u_map_place.z, v = (dot(u_b, w) - u_map_place.y) / u_map_place.z;
		ivec2 size = textureSize(u_map, 0);
		if (u >= 0.0 && v >= 0.0 && u < float(size.x) && v < float(size.y)) {
			float nearest = texelFetch(u_map, ivec2(u, v), 0).r;
			if (nearest - u_sun0 > dot(u_sun, w) + u_map_place.w) {
				bool turned_away = false;
				float iw_r = IwAt(p + ivec2(1, 0)), iw_d = IwAt(p + ivec2(0, 1));
				if (iw_r > 0.0 && iw_d > 0.0 && abs(iw_r - iw) < iw * 0.05 && abs(iw_d - iw) < iw * 0.05) {
					vec3 e = InWorld(vec2(p) + vec2(1.5, 0.5), iw_r) - w, g = InWorld(vec2(p) + vec2(0.5, 1.5), iw_d) - w;
					vec3 n = cross(e, g);
					if (dot(n, -w) < 0.0) n = -n;
					float nl = length(n);
					turned_away = nl > 0.0 && dot(n, u_sun) / nl < 0.05;
				}
				if (!turned_away) c = floor(c * vec3(160.0, 165.0, 180.0) / 256.0);
			}
		}
	}
	Put(c);
}
)";

/* The night (DrawEyeNight()): the picture darkened, so many 256ths of red, green and blue left. */
const char *const EYE_NIGHT_FRAGMENT = R"(
uniform vec3 u_night;
void main()
{
	Put(floor(ColourAt(Pixel()) * u_night / 256.0));
}
)";

/* The lights lighting what is about them (DrawEyeLights()), all at once: each pixel within reach its own colour out of
 * the dark (u_night, taken from the dark pixel once) in the light of all of them together, a day's at most. The lights
 * four texels each of u_lights. In a tunnel (u_tube_axis 0 or 1) the dark of its tube too, by the nearer portal as it is
 * drawn (EyeTubeDark()), and what is bright already less; by day nothing out of the tube (u_tube_day). */
const char *const EYE_LAMPS_FRAGMENT = R"(
uniform sampler2D u_lights;
uniform int u_count;
uniform vec3 u_night;
uniform float u_zu_per_unit;
uniform vec4 u_tube;
uniform int u_tube_axis;
uniform int u_tube_day;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float iw = IwAt(p);
	if (iw > 0.0) {
		vec3 w = InWorld(vec2(p) + 0.5, iw);
		float dark = 1.0;
		bool tube = false;
		if (u_tube_axis >= 0) {
			float s = u_tube_axis == 0 ? w.x : w.y, across = (u_tube_axis == 0 ? w.y : w.x) - u_tube.z;
			if (s >= u_tube.x && s <= u_tube.y && abs(across) <= u_tube.w) {
				tube = true;
				dark = 0.10 + 0.90 * exp(-min(s - u_tube.x, u_tube.y - s) / 10.0);
			}
		}
		if (u_tube_day != 0 && !tube) {
			Put(c);
			return;
		}
		vec3 light = vec3(0.0);
		for (int i = 0; i < u_count; i++) {
			vec4 l0 = texelFetch(u_lights, ivec2(i * 4, 0), 0), l1 = texelFetch(u_lights, ivec2(i * 4 + 1, 0), 0);
			vec4 l2 = texelFetch(u_lights, ivec2(i * 4 + 2, 0), 0), l3 = texelFetch(u_lights, ivec2(i * 4 + 3, 0), 0);
			if (p.x < int(l3.x) || p.y < int(l3.y) || p.x > int(l3.z) || p.y > int(l3.w)) continue;
			int kind = int(l1.w + 0.5);
			float reach = l0.w, k;
			if (kind == 0) {
				/* A house's lamp: down from it, over it less and less up to 1.2 m (CAB_LAMP_ABOVE of viewport.cpp). */
				float above = (w.z - l0.z) / u_zu_per_unit;
				if (above >= 1.294) continue;
				float d = length(w - l0.xyz);
				if (d >= reach) continue;
				k = (1.0 - d / reach) * (1.0 - d / reach) * 0.7;
				if (above > 0.0) k *= 1.0 - above / 1.294;
			} else {
				vec3 o = vec3(w.xy - l0.xy, (w.z - l0.z) / u_zu_per_unit);
				float d = length(o);
				if (kind == 1) {
					if (d >= reach) continue;
					k = (1.0 - d / reach) * (1.0 - d / reach) * 0.8;
				} else {
					float along = dot(o, l2.xyz);
					if (along <= 0.0 || along >= reach || d < 1e-6) continue;
					float cosa = along / d;
					if (cosa <= l2.w) continue;
					float middle = (cosa - l2.w) / (1.0 - l2.w);
					k = (1.0 - along / reach) * min(1.0, middle * 2.5) * 0.9;
				}
			}
			light += l1.rgb / 255.0 * k;
		}
		vec3 own = min(vec3(255.0), c * 256.0 / (u_night * dark));
		vec3 add = own * min(light, vec3(1.0));
		if (tube) add *= vec3(1.0) - c / 255.0;
		c = floor(min(vec3(255.0), c + add));
	}
	Put(c);
}
)";

/* The smoke and the blasts (DrawEyeBlasts()), one after the other, each a soft ball over what is behind it, hidden by
 * the pixel behind what is nearer than its middle. Two texels each of u_blasts. */
const char *const EYE_BLASTS_FRAGMENT = R"(
uniform sampler2D u_blasts;
uniform int u_count;
uniform vec2 u_size;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float iw = IwAt(p);
	for (int i = 0; i < u_count; i++) {
		vec4 b0 = texelFetch(u_blasts, ivec2(i * 2, 0), 0), b1 = texelFetch(u_blasts, ivec2(i * 2 + 1, 0), 0);
		int x0 = max(0, int(b0.x - b0.z)), x1 = min(int(u_size.x) - 1, int(b0.x + b0.z));
		int y0 = max(0, int(b0.y - b0.z)), y1 = min(int(u_size.y) - 1, int(b0.y + b0.z));
		if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) continue;
		vec2 d = (vec2(p) + 0.5 - b0.xy) / b0.z;
		float d2 = dot(d, d);
		if (d2 >= 1.0 || iw > b0.w * 1.02) continue;
		c = floor(c + (b1.rgb - c) * (b1.a * (1.0 - d2)));
	}
	Put(c);
}
)";

/* What has a light of its own again over the dark (glow_after_night of viewport.cpp), where it is still what is seen:
 * whole, or see-through over what is there. */
const char *const EYE_GLOW_AFTER_FRAGMENT = R"(
uniform sampler2D u_glow;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	vec4 g = texelFetch(u_glow, p, 0);
	float a = floor(g.a * 255.0 + 0.5);
	vec3 gc = floor(g.rgb * 255.0 + 0.5);
	if (a >= 255.0) {
		c = gc;
	} else if (a > 0.0) {
		c = min(vec3(255.0), gc + floor((c * (255.0 - a) + 127.0) / 255.0));
	}
	Put(c);
}
)";

/* A picture of its own laid in where it is and nearer (a tunnel seen through its portal, CabDrawTunnelSeenIn()): of
 * the rectangle of the picture from u_lay_at on, as big as it is. */
const char *const EYE_LAY_FRAGMENT = R"(#version 330 core
uniform sampler2D u_lay;
uniform sampler2D u_lay_z;
uniform ivec2 u_lay_at;
uniform float u_near;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_glow;
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy) - u_lay_at;
	if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, textureSize(u_lay, 0)))) discard;
	vec4 c = texelFetch(u_lay, p, 0);
	float z = texelFetch(u_lay_z, p, 0).r;
	if (z <= 0.0 || c == vec4(0.0)) discard;
	o_colour = vec4(c.rgb, 1.0);
	o_glow = vec4(0.0);
	gl_FragDepth = u_near * z;
}
)";

/* A soft spot of light added (AddNightLight() of viewport.cpp): a quad about it, the whole of it gone where the picture
 * is nearer than its middle (by hidden_by); a lamp's also each of its pixels where what is drawn there is nearer than
 * it -- a lamp seen through a car's window does not spill over the car before it (a pool of light on the ground is
 * all drawn). Its corner, middle and sizes, colour and strength, depth, hidden_by and whether it is a lamp's. */
const char *const EYE_GLOW_VERTEX = R"(#version 330 core
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_spot;   // sx, sy, rx, ry
layout(location = 2) in vec4 a_colour; // r, g, b, strength
layout(location = 3) in vec3 a_hidden; // iw, hidden_by, lamp
uniform sampler2D u_depth;
uniform float u_near;
uniform vec2 u_size;
flat out vec4 v_spot;
flat out vec4 v_colour;
flat out vec3 v_hidden;
void main()
{
	v_spot = a_spot;
	v_colour = a_colour;
	v_hidden = a_hidden;
	ivec2 m = ivec2(int(a_spot.x), int(a_spot.y));
	bool hidden = a_hidden.x > 0.0 && m.x >= 0 && m.y >= 0 && m.x < int(u_size.x) && m.y < int(u_size.y) && texelFetch(u_depth, m, 0).r / u_near > a_hidden.x * a_hidden.y;
	vec2 at = a_spot.xy + a_corner * (a_spot.zw + 1.0);
	gl_Position = hidden ? vec4(2.0, 2.0, 0.0, 1.0) : vec4(2.0 * at / u_size - 1.0, 0.0, 1.0);
}
)";

const char *const EYE_GLOW_FRAGMENT = R"(#version 330 core
uniform sampler2D u_depth;
uniform float u_near;
flat in vec4 v_spot;
flat in vec4 v_colour;
flat in vec3 v_hidden;
layout(location = 0) out vec4 o_colour;
void main()
{
	vec2 d = (gl_FragCoord.xy - v_spot.xy) / v_spot.zw;
	float d2 = dot(d, d);
	if (d2 >= 1.0) discard;
	if (v_hidden.z > 0.5 && v_hidden.x > 0.0 && texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r / u_near > v_hidden.x * v_hidden.y) discard;
	float f = v_colour.a * (1.0 - d2) * (1.0 - d2);
	o_colour = vec4(floor(v_colour.rgb * f) / 255.0, 1.0);
}
)";

/* The stars (DrawEyeSky()): a pixel each of what is added, where nothing is drawn above the horizon. */
const char *const EYE_STAR_VERTEX = R"(#version 330 core
layout(location = 0) in vec2 a_at;
layout(location = 1) in vec3 a_add;
uniform vec2 u_size;
flat out vec3 v_add;
void main()
{
	v_add = a_add;
	gl_Position = vec4(2.0 * (a_at + 0.5) / u_size - 1.0, 0.0, 1.0);
}
)";

const char *const EYE_STAR_FRAGMENT = R"(#version 330 core
uniform sampler2D u_depth;
uniform float u_horizon;
flat in vec3 v_add;
layout(location = 0) out vec4 o_colour;
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	if (float(p.y) >= u_horizon || texelFetch(u_depth, p, 0).r > 0.0) discard;
	o_colour = vec4(v_add / 255.0, 1.0);
}
)";

/* The picture as it is, into the picture shown (ThreeDGpuEyeFrame::shown). */
const char *const EYE_COPY_FRAGMENT = R"(
void main()
{
	Put(ColourAt(Pixel()));
}
)";

/* The moon (DrawEyeSky()): its face (u_glow 0, laid over) or the glow about it (1, added), where nothing is drawn above the horizon. */
const char *const EYE_MOON_FRAGMENT = R"(
uniform vec3 u_moon; // x, y, radius
uniform vec4 u_box;  // x0, y0, x1, y1
uniform float u_horizon;
uniform int u_glow;
void main()
{
	ivec2 p = Pixel();
	if (p.x < int(u_box.x) || p.y < int(u_box.y) || p.x > int(u_box.z) || p.y > int(u_box.w)) discard;
	if (float(p.y) >= u_horizon || IwAt(p) > 0.0) discard;
	vec2 d = (vec2(p) + 0.5 - u_moon.xy) / u_moon.z;
	float d2 = dot(d, d);
	if (u_glow == 0) {
		if (d2 > 1.0) discard;
		float k = 0.75 + 0.25 * sqrt(1.0 - d2);
		const vec3 seas[4] = vec3[4](vec3(-0.30, -0.20, 0.32), vec3(0.25, -0.35, 0.22), vec3(0.10, 0.30, 0.28), vec3(-0.35, 0.35, 0.15));
		for (int i = 0; i < 4; i++) {
			vec2 e = d - seas[i].xy;
			if (dot(e, e) < seas[i].z * seas[i].z) k *= 0.82;
		}
		Put(floor(vec3(240.0, 236.0, 218.0) * k));
	} else {
		if (d2 <= 1.0) discard;
		float g = max(0.0, 1.0 - (sqrt(d2) - 1.0) / 3.0);
		if (g <= 0.0) discard;
		float a = floor(g * g * 90.0);
		Put(floor(vec3(150.0, 165.0, 200.0) * a / 255.0));
	}
}
)";

#ifdef THREE_D_GPU_EGL
/* EGL, by its own names. */
using EGLDisplay = void *;
using EGLContext = void *;
using EGLConfig = void *;
using EGLSurface = void *;
using EGLint = int32_t;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040;
constexpr EGLint EGL_OPENGL_BIT = 0x0008;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_EXTENSIONS = 0x3055;
constexpr EGLenum EGL_OPENGL_API = 0x30A2;
constexpr EGLint EGL_CONTEXT_MAJOR_VERSION = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VERSION = 0x30FB;
constexpr EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD;
constexpr EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x0001;
constexpr EGLenum EGL_PLATFORM_SURFACELESS_MESA = 0x31DD;

struct EglContext {
	std::unique_ptr<LibraryLoader> lib;     ///< libEGL
	std::unique_ptr<LibraryLoader> gl_lib;  ///< libOpenGL, when EGL does not give the functions of the core
	GpuProc (*GetProcAddress)(const char *) = nullptr;
	EGLDisplay (*GetDisplay)(void *) = nullptr;
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *) = nullptr;
	EGLBoolean (*Terminate)(EGLDisplay) = nullptr;
	EGLBoolean (*BindAPI)(EGLenum) = nullptr;
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
	EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext) = nullptr;
	EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = nullptr;
	EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface) = nullptr;
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
	EGLint (*GetError)() = nullptr;
	const char *(*QueryString)(EGLDisplay, EGLint) = nullptr;
	EGLBoolean (*ReleaseThread)() = nullptr;
	EGLDisplay display = nullptr;
	EGLContext context = nullptr;
	EGLSurface surface = nullptr;
	bool video_display = false; ///< the display is the video driver's (shared with it): not ended with the card
};
EglContext _egl;

GpuProc GetGlProc(const char *name)
{
	GpuProc p = _egl.GetProcAddress != nullptr ? _egl.GetProcAddress(name) : nullptr;
	if (p != nullptr) return p;
	/* EGL before 1.5 gives only the extensions' functions: the core's from the library of OpenGL. */
	for (const char *file : {"libOpenGL.so.0", "libGL.so.1"}) {
		if (_egl.gl_lib == nullptr) {
			_egl.gl_lib = std::make_unique<LibraryLoader>(file);
			if (_egl.gl_lib->HasError()) {
				_egl.gl_lib.reset();
				continue;
			}
		}
		void (*f)() = _egl.gl_lib->GetFunction(name);
		return f;
	}
	return nullptr;
}

/** A context of OpenGL 3.3 of its own: without a window, of Mesa's render node where there is one. */
bool MakeEglContext(EGLContext share);

bool StartContext(std::string &why)
{
	_egl.lib = std::make_unique<LibraryLoader>("libEGL.so.1");
	if (_egl.lib->HasError()) {
		why = "no libEGL.so.1: " + _egl.lib->GetLastError();
		return false;
	}
	LibraryLoader &l = *_egl.lib;
	_egl.GetProcAddress = l.GetFunction("eglGetProcAddress");
	_egl.GetDisplay = l.GetFunction("eglGetDisplay");
	_egl.Initialize = l.GetFunction("eglInitialize");
	_egl.Terminate = l.GetFunction("eglTerminate");
	_egl.BindAPI = l.GetFunction("eglBindAPI");
	_egl.ChooseConfig = l.GetFunction("eglChooseConfig");
	_egl.CreateContext = l.GetFunction("eglCreateContext");
	_egl.DestroyContext = l.GetFunction("eglDestroyContext");
	_egl.CreatePbufferSurface = l.GetFunction("eglCreatePbufferSurface");
	_egl.DestroySurface = l.GetFunction("eglDestroySurface");
	_egl.MakeCurrent = l.GetFunction("eglMakeCurrent");
	_egl.GetError = l.GetFunction("eglGetError");
	_egl.QueryString = l.GetFunction("eglQueryString");
	_egl.ReleaseThread = l.GetFunction("eglReleaseThread");
	if (l.HasError()) {
		why = "libEGL.so.1 without " + l.GetLastError();
		return false;
	}
	EGLint major = 0, minor = 0;
	/* On the video driver's display, sharing its things with its context, when it has one of EGL (ThreeDGpuVideoContext()). */
	EGLDisplay video_display = nullptr;
	EGLContext video_context = nullptr;
	if (_gpu_no_share) {
		Diary(SHARE_OFF, true);
	} else {
		std::lock_guard lock(_video_mutex);
		video_display = _video_display;
		video_context = _video_context;
	}
	if (video_display != nullptr && video_context != nullptr && _egl.Initialize(video_display, &major, &minor)) {
		_egl.display = video_display;
		_egl.video_display = true;
		Step("kontext sdileny s ovladacem videa (EGL)");
		if (MakeEglContext(video_context)) {
			_video_shared = true;
			return true;
		}
		_egl.display = nullptr;
		_egl.video_display = false;
	}
	using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void *, const EGLint *);
	auto platform_display = reinterpret_cast<GetPlatformDisplay>(_egl.GetProcAddress("eglGetPlatformDisplayEXT"));
	if (platform_display != nullptr) {
		_egl.display = platform_display(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
		if (_egl.display != nullptr && !_egl.Initialize(_egl.display, &major, &minor)) _egl.display = nullptr;
	}
	if (_egl.display == nullptr) {
		_egl.display = _egl.GetDisplay(nullptr);
		if (_egl.display == nullptr || !_egl.Initialize(_egl.display, &major, &minor)) {
			why = fmt::format("no EGL display (error {:#x})", _egl.GetError());
			_egl.display = nullptr;
			return false;
		}
	}
	if (!MakeEglContext(nullptr)) {
		why = fmt::format("no context of OpenGL 3.3 (EGL error {:#x})", _egl.GetError());
		return false;
	}
	return true;
}

/**
 * The context of OpenGL 3.3 on the display (StartContext()), current: without a configuration and a surface where EGL
 * can, else with a pixel's buffer of its own.
 * @param share the context it shares its things with, nullptr none
 */
bool MakeEglContext(EGLContext share)
{
	if (!_egl.BindAPI(EGL_OPENGL_API)) return false;
	const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	const char *extensions = _egl.QueryString(_egl.display, EGL_EXTENSIONS);
	const std::string_view ext = extensions != nullptr ? extensions : "";
	if (ext.find("EGL_KHR_no_config_context") != std::string_view::npos && ext.find("EGL_KHR_surfaceless_context") != std::string_view::npos) {
		_egl.context = _egl.CreateContext(_egl.display, nullptr, share, context_attributes);
		if (_egl.context != nullptr && !_egl.MakeCurrent(_egl.display, nullptr, nullptr, _egl.context)) {
			_egl.DestroyContext(_egl.display, _egl.context);
			_egl.context = nullptr;
		}
	}
	if (_egl.context == nullptr) {
		const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
		EGLConfig config = nullptr;
		EGLint configs = 0;
		if (!_egl.ChooseConfig(_egl.display, config_attributes, &config, 1, &configs) || configs < 1) return false;
		const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		_egl.surface = _egl.CreatePbufferSurface(_egl.display, config, surface_attributes);
		_egl.context = _egl.CreateContext(_egl.display, config, share, context_attributes);
		if (_egl.surface == nullptr || _egl.context == nullptr || !_egl.MakeCurrent(_egl.display, _egl.surface, _egl.surface, _egl.context)) {
			if (_egl.context != nullptr) _egl.DestroyContext(_egl.display, _egl.context);
			if (_egl.surface != nullptr) _egl.DestroySurface(_egl.display, _egl.surface);
			_egl.context = _egl.surface = nullptr;
			return false;
		}
	}
	return true;
}

void EndContext()
{
	if (_egl.display != nullptr) {
		_egl.MakeCurrent(_egl.display, nullptr, nullptr, nullptr);
		if (_egl.context != nullptr) _egl.DestroyContext(_egl.display, _egl.context);
		if (_egl.surface != nullptr) _egl.DestroySurface(_egl.display, _egl.surface);
		/* (The video driver's display stays: it is its.) */
		if (!_egl.video_display) _egl.Terminate(_egl.display);
	}
	_video_shared = false;
	if (_egl.ReleaseThread != nullptr) _egl.ReleaseThread();
	_egl.context = _egl.surface = _egl.display = nullptr;
}

void PumpMessages() {}
#endif /* THREE_D_GPU_EGL */

#ifdef THREE_D_GPU_WGL
HWND _gpu_window = nullptr;
HDC _gpu_dc = nullptr;
HGLRC _gpu_rc = nullptr;

GpuProc GetGlProc(const char *name)
{
	const INT_PTR p = reinterpret_cast<INT_PTR>(wglGetProcAddress(name));
	/* Some drivers give 1, 2, 3 or -1 for none; the functions of OpenGL 1.1 are opengl32's own. */
	if (p != 0 && p != 1 && p != 2 && p != 3 && p != -1) return reinterpret_cast<GpuProc>(p);
	return reinterpret_cast<GpuProc>(GetProcAddress(GetModuleHandleW(L"opengl32"), name));
}

/** A context of OpenGL 3.3 of its own, of a hidden window of the card's thread. */
bool StartContext(std::string &why)
{
	Step("okno pro kartu");
	_gpu_window = CreateWindowW(L"STATIC", L"OpenTTD 3D", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (_gpu_window == nullptr) {
		why = "no window for the context";
		return false;
	}
	_gpu_dc = GetDC(_gpu_window);
	/* The video driver's context (ThreeDGpuVideoContext()): the card's shares its things with it, on the pixel format
	 * of its device. */
	HDC video_dc = nullptr;
	HGLRC video_rc = nullptr;
	if (_gpu_no_share) {
		Diary(SHARE_OFF, true);
	} else {
		std::lock_guard lock(_video_mutex);
		video_dc = static_cast<HDC>(_video_display);
		video_rc = static_cast<HGLRC>(_video_context);
	}
	PIXELFORMATDESCRIPTOR pfd = {};
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.iLayerType = PFD_MAIN_PLANE;
	Step(video_dc != nullptr ? "format pixelu sdileny s ovladacem videa" : "format pixelu");
	int format = 0;
	if (video_dc != nullptr) {
		const int video_format = GetPixelFormat(video_dc);
		PIXELFORMATDESCRIPTOR video_pfd = {};
		if (video_format > 0 && DescribePixelFormat(video_dc, video_format, sizeof(video_pfd), &video_pfd) != 0 && SetPixelFormat(_gpu_dc, video_format, &video_pfd)) format = video_format;
	}
	if (format == 0) {
		format = ChoosePixelFormat(_gpu_dc, &pfd);
		if (format == 0 || !SetPixelFormat(_gpu_dc, format, &pfd)) {
			why = "no pixel format for OpenGL";
			return false;
		}
	}
	Step("prvni kontext OpenGL (wglCreateContext, wglMakeCurrent)");
	HGLRC first = wglCreateContext(_gpu_dc);
	if (first == nullptr || !wglMakeCurrent(_gpu_dc, first)) {
		/* Why, as Windows tells it (the player's #361: "no context of OpenGL" and no word more, the whole game drawn
		 * by the processor). */
		why = fmt::format("no context of OpenGL ({} failed, Windows error {})", first == nullptr ? "wglCreateContext" : "wglMakeCurrent", static_cast<uint32_t>(GetLastError()));
		if (first != nullptr) wglDeleteContext(first);
		return false;
	}
	_gpu_rc = first;
	/* The core of 3.3 where the driver has it (Mesa under Wine gives only 2.1 or 3.0 else); the first context else. */
	using CreateContextAttribs = HGLRC (WINAPI *)(HDC, HGLRC, const int *);
	auto create = reinterpret_cast<CreateContextAttribs>(GetGlProc("wglCreateContextAttribsARB"));
	if (create != nullptr) {
		const int attributes[] = {0x2091 /* WGL_CONTEXT_MAJOR_VERSION_ARB */, 3, 0x2092 /* WGL_CONTEXT_MINOR_VERSION_ARB */, 3,
				0x9126 /* WGL_CONTEXT_PROFILE_MASK_ARB */, 0x0001 /* WGL_CONTEXT_CORE_PROFILE_BIT_ARB */, 0};
		Step("kontext OpenGL 3.3 core (wglCreateContextAttribsARB)");
		HGLRC core = nullptr;
		if (video_rc != nullptr) {
			Step("kontext OpenGL 3.3 core sdileny s ovladacem videa");
			core = create(_gpu_dc, video_rc, attributes);
			if (core != nullptr) _video_shared = true;
		}
		if (core == nullptr) core = create(_gpu_dc, nullptr, attributes);
		if (core != nullptr && wglMakeCurrent(_gpu_dc, core)) {
			wglDeleteContext(first);
			_gpu_rc = core;
		} else {
			if (core != nullptr) wglDeleteContext(core);
			_video_shared = false;
			wglMakeCurrent(_gpu_dc, first);
		}
	}
	if (_gpu_rc == first && video_rc != nullptr) {
		/* The first context, its lists shared with the video driver's before anything is made in it. */
		wglMakeCurrent(nullptr, nullptr);
		_video_shared = wglShareLists(video_rc, first) != FALSE;
		wglMakeCurrent(_gpu_dc, first);
	}
	return true;
}


void EndContext()
{
	_video_shared = false;
	wglMakeCurrent(nullptr, nullptr);
	if (_gpu_rc != nullptr) wglDeleteContext(_gpu_rc);
	if (_gpu_dc != nullptr) ReleaseDC(_gpu_window, _gpu_dc);
	if (_gpu_window != nullptr) DestroyWindow(_gpu_window);
	_gpu_rc = nullptr;
	_gpu_dc = nullptr;
	_gpu_window = nullptr;
}

/** The messages of the hidden window (it is the card's thread's). */
void PumpMessages()
{
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}
#endif /* THREE_D_GPU_WGL */

bool LoadGlFunctions(std::string &why)
{
#define THREE_D_GL_LOAD(ret, name, args) \
	{ \
		using Slot = GlCounted<__COUNTER__, decltype(gl.name)>; \
		if constexpr (Slot::index < GL_COUNTED_FUNCTIONS) _gl_function_names[Slot::index] = #name; \
		Slot::real = reinterpret_cast<decltype(gl.name)>(GetGlProc(#name)); \
		if (Slot::real == nullptr) { \
			why = "OpenGL without " #name; \
			return false; \
		} \
		gl.name = &Slot::Call; \
	}
	THREE_D_GL_FUNCTIONS(THREE_D_GL_LOAD)
#undef THREE_D_GL_LOAD
	gl.glFenceSync = reinterpret_cast<decltype(gl.glFenceSync)>(GetGlProc("glFenceSync"));
	gl.glMultiDrawArrays = reinterpret_cast<decltype(gl.glMultiDrawArrays)>(GetGlProc("glMultiDrawArrays"));
	gl.glDeleteSync = reinterpret_cast<decltype(gl.glDeleteSync)>(GetGlProc("glDeleteSync"));
	gl.glFlush = reinterpret_cast<decltype(gl.glFlush)>(GetGlProc("glFlush"));
	return true;
}

GLuint CompileShader(GLenum type, const char *source, std::string &why)
{
	GLuint shader = gl.glCreateShader(type);
	gl.glShaderSource(shader, 1, &source, nullptr);
	gl.glCompileShader(shader);
	GLint ok = 0;
	gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		char log[1024] = {};
		gl.glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
		why = fmt::format("a shader not compiled: {}", log);
		gl.glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/**
 * Whether the card divides an attribute of a VAO by instance (glVertexAttribDivisor(), ARB_instanced_arrays), as the
 * batches of the view from the eyes read their instances' matrices (EyeObjectDraws::Batch). The player's phone (#323,
 * 2. 11., zink on Vortek, Mesa 22.2.5) names OpenGL 4.4 yet refused it with GL_INVALID_OPERATION each frame: every
 * corner of a batch then read a matrix of its own from the instances' buffer, the trees and the figures drawn as
 * splinters or not at all, past the buffer's end. A VAO of its own: the bound one of a picture is kept as it was.
 * @return whether the card took the divisor
 */
static bool TestInstancedArrays()
{
	for (int i = 0; i < 16 && gl.glGetError() != 0; i++) {}
	GLuint vao = 0;
	gl.glGenVertexArrays(1, &vao);
	gl.glBindVertexArray(vao);
	gl.glVertexAttribDivisor(4, 1);
	const bool ok = gl.glGetError() == 0;
	gl.glVertexAttribDivisor(4, 0);
	gl.glBindVertexArray(0);
	gl.glDeleteVertexArrays(1, &vao);
	for (int i = 0; i < 16 && gl.glGetError() != 0; i++) {}
	return ok;
}

/**
 * Whether the card's glClear() keeps to the scissor's rect in a picture of two colours, as the pictures of the turned
 * map are cleared block by block (DrawStandingOnCard(); the player's phone, 1. 11.: behind the cars of the map rows of
 * their pictures, gone when the map moved -- which clears the pictures whole): a picture cleared whole to white, a
 * rect in it cleared under the scissor, a pixel read back inside it and one outside, in both colours. Told in the
 * card's diary; where it does not keep to it, ClearInScissor() draws over the rect.
 */
static void TestScissoredClear()
{
	const int w = 256, h = 192;
	GLuint tex[2] = {}, fbo = 0;
	gl.glGenTextures(2, tex);
	for (GLuint name : tex) {
		gl.glBindTexture(GL_TEXTURE_2D, name);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glGenFramebuffers(1, &fbo);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex[1], 0);
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	gl.glDrawBuffers(2, both);
	if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		gl.glDisable(GL_SCISSOR_TEST);
		gl.glViewport(0, 0, w, h);
		gl.glClearColor(1, 1, 1, 1);
		gl.glClear(GL_COLOR_BUFFER_BIT);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glScissor(70, 50, 64, 48);
		gl.glClearColor(0, 0, 0, 0);
		gl.glClear(GL_COLOR_BUFFER_BIT);
		gl.glDisable(GL_SCISSOR_TEST);
		uint8_t in[2][4] = {}, out[2][4] = {};
		for (int k = 0; k < 2; k++) {
			gl.glReadBuffer(k == 0 ? GL_COLOR_ATTACHMENT0 : GL_COLOR_ATTACHMENT1);
			gl.glReadPixels(100, 70, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, in[k]);
			gl.glReadPixels(20, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out[k]);
		}
		const bool inside = in[0][3] == 0 && in[1][3] == 0, outside = out[0][3] == 255 && out[1][3] == 255;
		_scissored_clear_ok = inside && outside;
		const std::string line = fmt::format("cisteni s nuzkami: uvnitr {} ({} {}), venku {} ({} {}){}", inside ? "smazano" : "NESMAZANO", in[0][3], in[1][3],
				outside ? "zustalo" : "SMAZANO", out[0][3], out[1][3], _scissored_clear_ok ? "" : "; karta maze kreslenim");
		Diary(line, true);
		Debug(misc, 1, "3D on the graphics card: {}", line);
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.glDeleteFramebuffers(1, &fbo);
	gl.glDeleteTextures(2, tex);
	gl.glGetError();
}

/**
 * The scissor's clear as what stands on a block of the turned map has it (DrawStandingOnCard()), not on its own as
 * TestScissoredClear() tries it: the picture with what stood on it drawn, its rect cleared under the scissor, then the
 * vehicles' frame bound, cleared and drawn into, one shrunk from it into the kept pictures, and back in the picture a
 * vehicle laid in the rect. Zink lost such a clear (the player's #322, 2. 11., zink on Mali: behind the cars and the
 * animals of the map rows of their old pictures, none of the rect cleared; zink of the rig, the other way, cleared the
 * cars laid after it -- the map without cars), the plain test passing on both. Read back: the rect's other pixels
 * cleared, the vehicle there, the picture past the rect as it was. Told in the card's diary.
 * @return whether the card kept to it; else ClearInScissor() draws over the rect
 */
GLuint NewGroundTexture(int w, int h);
static bool TestScissoredClearAmongPictures()
{
	if (!_standing_program_ok || _standing_vao == 0) return true;
	const int w = 256, h = 192, fw = 128, fh = 64, kw = 64, kh = 64;
	GLuint pic[2] = {}, frame[2] = {}, kept = 0, paint = 0, fbo[3] = {}, depth[2] = {};
	for (GLuint &t : pic) t = NewGroundTexture(w, h);
	for (GLuint &t : frame) t = NewGroundTexture(fw, fh);
	kept = NewGroundTexture(kw, kh);
	/* Two texels to draw with: white, red. */
	static const uint8_t texels[8] = {255, 255, 255, 255, 255, 0, 0, 255};
	gl.glGenTextures(1, &paint);
	gl.glBindTexture(GL_TEXTURE_2D, paint);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glGenRenderbuffers(2, depth);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, depth[0]);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, depth[1]);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fw, fh);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.glGenFramebuffers(3, fbo);
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	static const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
	bool complete = true;
	for (int k = 0; k < 2; k++) {
		gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo[k]);
		const GLuint *t = k == 0 ? pic : frame;
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[0], 0);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t[1], 0);
		gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth[k]);
		gl.glDrawBuffers(2, both);
		complete &= gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo[2]);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, kept, 0);
	gl.glDrawBuffers(1, one);
	complete &= gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

	bool ok = true;
	if (complete) {
		const GLuint p = _standing_program.program;
		gl.glUseProgram(p);
		gl.glUniform1i(gl.glGetUniformLocation(p, "u_texture"), 0);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindVertexArray(_standing_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glDisable(GL_CULL_FACE);
		/* A quad of the texel at u of the texture bound, over x0,y0 - x1,y1 of a picture of size sw, sh. */
		auto quad = [&](int sw, int sh, float x0, float y0, float x1, float y1, float u) {
			gl.glUniform2f(gl.glGetUniformLocation(p, "u_size"), static_cast<float>(sw), static_cast<float>(sh));
			const float c[] = {x0, y0, 0, u, 0.5f, x1, y0, 0, u, 0.5f, x1, y1, 0, u, 0.5f, x0, y0, 0, u, 0.5f, x1, y1, 0, u, 0.5f, x0, y1, 0, u, 0.5f};
			gl.glBufferData(GL_ARRAY_BUFFER, sizeof(c), c, GL_STREAM_DRAW);
			gl.glDrawArrays(GL_TRIANGLES, 0, 6);
		};
		/* What stood on it: the picture drawn whole, white, a frame before. */
		gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		gl.glDrawBuffers(2, both);
		gl.glViewport(0, 0, w, h);
		gl.glDisable(GL_SCISSOR_TEST);
		gl.glDisable(GL_BLEND);
		gl.glClearColor(0, 0, 0, 0);
		gl.glClear(GL_COLOR_BUFFER_BIT);
		gl.glBindTexture(GL_TEXTURE_2D, paint);
		quad(w, h, 0, 0, static_cast<float>(w), static_cast<float>(h), 0.5f);
		gl.glFlush();
		/* A block of it laid again: its rect cleared under the scissor. */
		const int rx = 70, ry = 50, rw = 64, rh = 48;
		gl.glScissor(rx, ry, rw, rh);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glClear(GL_COLOR_BUFFER_BIT);
		/* A vehicle not kept yet: the vehicles' frame cleared and drawn into (red)... */
		gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
		gl.glDrawBuffers(2, both);
		gl.glViewport(0, 0, fw, fh);
		gl.glScissor(0, 0, fw, fh);
		gl.glClearDepth(1.0);
		gl.glDepthMask(GL_TRUE);
		gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		gl.glDisable(GL_SCISSOR_TEST);
		quad(fw, fh, 0, 0, static_cast<float>(fw), static_cast<float>(fh), 1.5f);
		/* ...shrunk from it into the kept pictures... */
		gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo[2]);
		gl.glDrawBuffers(1, one);
		gl.glViewport(0, 0, kw, kh);
		gl.glBindTexture(GL_TEXTURE_2D, frame[0]);
		quad(kw, kh, 0, 0, static_cast<float>(kw), static_cast<float>(kh), 0.5f);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		/* ...and laid from them in the rect, over what is there. */
		gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		gl.glDrawBuffers(2, both);
		gl.glViewport(0, 0, w, h);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glScissor(rx, ry, rw, rh);
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		gl.glBindTexture(GL_TEXTURE_2D, kept);
		quad(w, h, 80, 60, 96, 76, 0.5f);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glDisable(GL_BLEND);
		gl.glDisable(GL_SCISSOR_TEST);
		/* Read back: in the rect past the vehicle, the vehicle, past the rect. */
		uint8_t cleared[4] = {}, vehicle[4] = {}, past[4] = {};
		gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
		gl.glReadPixels(120, 90, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cleared);
		gl.glReadPixels(88, 68, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, vehicle);
		gl.glReadPixels(20, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, past);
		const bool was_cleared = cleared[3] == 0, laid = vehicle[3] == 255 && vehicle[0] == 255 && vehicle[1] == 0, stayed = past[3] == 255 && past[1] == 255;
		ok = was_cleared && laid && stayed;
		const std::string line = fmt::format("cisteni s nuzkami mezi obrazky: obdelnik {} ({}), auto v nem {} ({} {} {}), venku {} ({}){}",
				was_cleared ? "smazan" : "NESMAZAN", cleared[3], laid ? "je" : "NENI", vehicle[0], vehicle[1], vehicle[3],
				stayed ? "zustalo" : "SMAZANO", past[3], ok ? "" : "; karta maze kreslenim");
		Diary(line, true);
		Debug(misc, 1, "3D on the graphics card: {}", line);
		gl.glBindVertexArray(0);
		gl.glUseProgram(0);
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.glDeleteFramebuffers(3, fbo);
	gl.glDeleteRenderbuffers(2, depth);
	gl.glDeleteTextures(2, pic);
	gl.glDeleteTextures(2, frame);
	gl.glDeleteTextures(1, &kept);
	gl.glDeleteTextures(1, &paint);
	gl.glGetError();
	return ok;
}

/*
 * The first draw of each kind in a run waited for at once, its step named before it. The player's phone (6. 11.: into
 * the view from the eyes on a farm's field, and under a dragon) went down in its driver compiling a shader, both times
 * in the glFinish at the end of the first frame from the eyes, and the diary could not say for which of its draws: the
 * card compiles a program's shader anew for each way of drawing with it (blending, the depth, the corners' attributes).
 * Waited for at its first draw, the last line of the diary says which.
 */
static std::set<std::string, std::less<>> _first_draws;

/**
 * Before a draw: whether it is the first of its kind in this run, its step written and kept -- they are few, and say
 * what the card has drawn (FirstDrawDone() after it then).
 */
static bool FirstDraw(std::string_view what)
{
	if (_first_draws.contains(what)) return false;
	_first_draws.emplace(what);
	Diary(fmt::format("krok prvni kresleni {} (glFinish)", what), true);
	return true;
}

/** After the first draw of its kind: the card waited for, its shader compiled for it now. */
static void FirstDrawDone(bool first)
{
	if (first) gl.glFinish();
}

/**
 * FirstDraw() of a kind told by a number (the draws from the eyes, some thousand a frame): its words made and looked up
 * only the first time -- before, a fmt::format() and a lookup of the set at every draw.
 * @param key the kind (FIRST_DRAW_* and its bits)
 * @param words what makes its words for FirstDraw()
 */
template <typename F>
static bool FirstDrawOf(uint32_t key, F words)
{
	static std::set<uint32_t> seen;
	if (seen.contains(key)) return false;
	seen.insert(key);
	return FirstDraw(words());
}
static constexpr uint32_t FIRST_DRAW_EYE_MESH = 0x100, FIRST_DRAW_EYE_BUILDING = 0x200, FIRST_DRAW_EYE_TRIANGLES = 0x300;

/**
 * A uniform's location in a program, asked of the card once (the steps of a frame from the eyes asked at every frame:
 * some 220 calls a frame of the player's #381); forgotten when a program is made (MakeProgram()), its name may be an
 * old one's.
 */
static std::map<std::pair<GLuint, std::string>, GLint, std::less<>> _uniforms_known;
static GLint UniformOf(GLuint program, std::string_view name)
{
	auto key = std::make_pair(program, std::string(name));
	auto it = _uniforms_known.find(key);
	if (it != _uniforms_known.end()) return it->second;
	const GLint at = gl.glGetUniformLocation(program, key.second.c_str());
	_uniforms_known.emplace(std::move(key), at);
	return at;
}

bool MakeProgram(GpuProgram &p, const char *vertex, const char *fragment, std::string &why)
{
	_uniforms_known.clear();
	GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex, why);
	if (vs == 0) return false;
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment, why);
	if (fs == 0) {
		gl.glDeleteShader(vs);
		return false;
	}
	p.program = gl.glCreateProgram();
	gl.glAttachShader(p.program, vs);
	gl.glAttachShader(p.program, fs);
	gl.glLinkProgram(p.program);
	gl.glDeleteShader(vs);
	gl.glDeleteShader(fs);
	GLint ok = 0;
	gl.glGetProgramiv(p.program, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE) {
		char log[1024] = {};
		gl.glGetProgramInfoLog(p.program, sizeof(log) - 1, nullptr, log);
		why = fmt::format("a program not linked: {}", log);
		return false;
	}
	p.matrix = gl.glGetUniformLocation(p.program, "u_matrix");
	p.light = gl.glGetUniformLocation(p.program, "u_light_lit");
	p.tex = gl.glGetUniformLocation(p.program, "u_tex");
	p.glass = gl.glGetUniformLocation(p.program, "u_glass");
	p.glass_alpha = gl.glGetUniformLocation(p.program, "u_glass_alpha");
	p.texel_alpha = gl.glGetUniformLocation(p.program, "u_texel_alpha");
	p.glow = gl.glGetUniformLocation(p.program, "u_glow");
	p.paint = gl.glGetUniformLocation(p.program, "u_paint");
	p.detail = gl.glGetUniformLocation(p.program, "u_detail");
	p.detail_colours = gl.glGetUniformLocation(p.program, "u_detail_colours");
	p.paint_mask = gl.glGetUniformLocation(p.program, "u_paint_mask");
	p.addon_mask = gl.glGetUniformLocation(p.program, "u_addon_mask");
	p.rust_mask = gl.glGetUniformLocation(p.program, "u_rust_mask");
	p.paint_values = gl.glGetUniformLocation(p.program, "u_paint_values");
	p.rust_moments = gl.glGetUniformLocation(p.program, "u_rust_moments");
	p.rust_delay = gl.glGetUniformLocation(p.program, "u_rust_delay");
	p.near_plane = gl.glGetUniformLocation(p.program, "u_near");
	p.wheels = gl.glGetUniformLocation(p.program, "u_wheels");
	p.wheel_count = gl.glGetUniformLocation(p.program, "u_wheel_count");
	p.depth_range = gl.glGetUniformLocation(p.program, "u_depth_range");
	p.instanced = gl.glGetUniformLocation(p.program, "u_instanced");
	p.bones = gl.glGetUniformLocation(p.program, "u_bones");
	p.bone_row = gl.glGetUniformLocation(p.program, "u_bone_row");
	return true;
}

static bool TestBones();

/** The context, the functions, the programs: whether the card draws, and what it is. */
bool StartCard(std::string &why, std::string &describe)
{
	if (!StartContext(why)) return false;
	Step("funkce OpenGL");
	if (!LoadGlFunctions(why)) return false;
	Step("verze OpenGL (glGetString)");
	const char *version = reinterpret_cast<const char *>(gl.glGetString(GL_VERSION));
	const char *renderer = reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER));
	Diary(fmt::format("karta: {}, OpenGL {}", renderer != nullptr ? renderer : "?", version != nullptr ? version : "?"), true);
	/* "4.5 (Core Profile) Mesa ...": the first two numbers. */
	int major = 0, minor = 0;
	const char *c = version != nullptr ? version : "";
	for (; *c >= '0' && *c <= '9'; c++) major = major * 10 + (*c - '0');
	if (*c == '.') {
		for (c++; *c >= '0' && *c <= '9'; c++) minor = minor * 10 + (*c - '0');
	}
	if (major * 10 + std::min(minor, 9) < 33) {
		why = fmt::format("OpenGL {} (3.3 needed)", version != nullptr ? version : "?");
		return false;
	}
	Step("preklad programu vozidel (shader)");
	if (!MakeProgram(_mesh_program, MESH_VERTEX, MESH_FRAGMENT, why)) return false;
	Step("preklad programu svetel (shader)");
	if (!MakeProgram(_night_program, NIGHT_VERTEX, NIGHT_FRAGMENT, why)) return false;
	Step("preklad programu budov z oci (shader)");
	if (!MakeProgram(_eye_building_program, EYE_BUILDING_VERTEX, EYE_BUILDING_FRAGMENT, why)) return false;
	Step("preklad programu zeme plochy (shader)");
	{
		std::string ground_why;
		_ground_program_ok = MakeProgram(_ground_program, GROUND_VERTEX, GROUND_FRAGMENT, ground_why);
		if (!_ground_program_ok) Diary("zem plochy kresli procesor: " + ground_why, true);
		std::string standing_why;
		_standing_program_ok = MakeProgram(_standing_program, STANDING_BOX_VERTEX, STANDING_FRAGMENT, standing_why) &&
				MakeProgram(_standing_building_program, MAP_BUILDING_VERTEX, STANDING_BUILDING_FRAGMENT, standing_why);
		if (!_standing_program_ok) Diary("co stoji na plose kresli procesor: " + standing_why, true);
		std::string faces_why;
		_standing_faces_ok = _standing_program_ok && MakeProgram(_standing_faces_program, STANDING_FACES_VERTEX, STANDING_FACES_FRAGMENT, faces_why);
		if (_standing_program_ok && !_standing_faces_ok) Diary("krabice plochy kresli procesor: " + faces_why, true);
		std::string shrink_why;
		_standing_shrink_ok = _standing_program_ok && MakeProgram(_standing_shrink_program, STANDING_SHRINK_VERTEX, STANDING_SHRINK_FRAGMENT, shrink_why);
		if (_standing_program_ok && !_standing_shrink_ok) Diary("vozidla plochy jako obrazky: " + shrink_why, true);
		std::string clear_why;
		_clear_program_ok = MakeProgram(_clear_program, STANDING_VERTEX, CLEAR_FRAGMENT, clear_why);
		if (!_clear_program_ok) Diary("mazani kreslenim nejde: " + clear_why, true);
		std::string copy_why;
		_standing_copy_ok = _standing_program_ok && MakeProgram(_standing_copy_program, STANDING_VERTEX, COPY_FRAGMENT, copy_why);
		if (_standing_program_ok && !_standing_copy_ok) Diary("vrstva aut nejde, auta s tim, co stoji: " + copy_why, true);
	}
	Step("preklad programu postav z oci (shader)");
	const std::string eye_vertex = EyeMeshSource(MESH_VERTEX, false), eye_fragment = EyeMeshSource(MESH_FRAGMENT, true);
	if (!MakeProgram(_eye_mesh_program, eye_vertex.c_str(), eye_fragment.c_str(), why)) return false;
	Step("ramec a buffery (framebuffer, renderbuffer, vertex array)");
	gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_max_texture);
	gl.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &_max_target);
	_max_target = std::min(_max_target, 16384);
	gl.glGenFramebuffers(1, &_fbo);
	gl.glGenRenderbuffers(1, &_colour_rb);
	gl.glGenRenderbuffers(1, &_lit_rb);
	gl.glGenRenderbuffers(1, &_depth_rb);
	gl.glGenFramebuffers(1, &_eye_fbo);
	gl.glGenRenderbuffers(1, &_eye_colour_rb);
	gl.glGenRenderbuffers(1, &_eye_glow_rb);
	gl.glGenRenderbuffers(1, &_eye_depth_rb);
	gl.glGenFramebuffers(1, &_gfbo);
	gl.glGenFramebuffers(2, _g_move_fbo);
	gl.glGenRenderbuffers(1, &_g_colour_rb);
	gl.glGenVertexArrays(1, &_ground_vao);
	gl.glGenBuffers(1, &_ground_vbo);
	gl.glBindVertexArray(_ground_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _ground_vbo);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), reinterpret_cast<const void *>(2 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), reinterpret_cast<const void *>(4 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 9 * sizeof(float), reinterpret_cast<const void *>(8 * sizeof(float)));
	gl.glEnableVertexAttribArray(3);
	gl.glBindVertexArray(0);
	gl.glGenVertexArrays(1, &_standing_vao);
	gl.glGenBuffers(1, &_standing_vbo);
	gl.glBindVertexArray(_standing_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glBindVertexArray(0);
	/* The same corners with how near the vehicle's pixels are after them, for shrinking vehicles (STANDING_SHRINK_VERTEX). */
	gl.glGenVertexArrays(1, &_shrink_vao);
	gl.glBindVertexArray(_shrink_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
	static constexpr GLsizei SHRINK_STRIDE = SHRINK_FLOATS * sizeof(float);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, SHRINK_STRIDE, nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, SHRINK_STRIDE, reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, SHRINK_STRIDE, reinterpret_cast<const void *>(5 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, SHRINK_STRIDE, reinterpret_cast<const void *>(9 * sizeof(float)));
	gl.glEnableVertexAttribArray(3);
	gl.glBindVertexArray(0);
	/* The same corners with their order and their box after them (ThreeDGpuStandingItem::order, ::box), for the pictures
	 * laid (STANDING_BOX_VERTEX). */
	gl.glGenVertexArrays(1, &_standing_order_vao);
	gl.glBindVertexArray(_standing_order_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
	static constexpr GLsizei BOXED = STANDING_BOXED_FLOATS * sizeof(float);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BOXED, nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, BOXED, reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, BOXED, reinterpret_cast<const void *>(5 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, BOXED, reinterpret_cast<const void *>(6 * sizeof(float)));
	gl.glEnableVertexAttribArray(3);
	gl.glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, BOXED, reinterpret_cast<const void *>(10 * sizeof(float)));
	gl.glEnableVertexAttribArray(4);
	gl.glBindVertexArray(0);
	gl.glGenVertexArrays(1, &_standing_faces_vao);
	gl.glGenBuffers(1, &_standing_faces_vbo);
	gl.glBindVertexArray(_standing_faces_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_faces_vbo);
	static constexpr GLsizei FACE = ThreeDGpuStandingFaces::FLOATS * sizeof(float);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, FACE, nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, FACE, reinterpret_cast<const void *>(2 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, FACE, reinterpret_cast<const void *>(4 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glBindVertexArray(0);
	gl.glGenVertexArrays(1, &_standing_tri_vao);
	gl.glGenBuffers(1, &_standing_tri_vbo);
	gl.glBindVertexArray(_standing_tri_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_tri_vbo);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(5 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glBindVertexArray(0);
	gl.glGenVertexArrays(1, &_night_vao);
	gl.glGenBuffers(1, &_night_vbo);
	gl.glBindVertexArray(_night_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _night_vbo);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glBindVertexArray(0);
	if (gl.glGetError() != 0) {
		why = "OpenGL failed at the start";
		return false;
	}
	Step("zkouska cisteni s nuzkami");
	TestScissoredClear();
	Step("zkouska cisteni s nuzkami mezi obrazky");
	if (_scissored_clear_ok && !TestScissoredClearAmongPictures()) _scissored_clear_ok = false;
	/* Zink (OpenGL over Vulkan; the player's phone, Mali through Winlator) puts a clear off till the next draw, and
	 * what stands on a block of the turned map changes the picture drawn into between the clear and the draw: its
	 * Mesa 22.2 lost the clear (the player's #322: rows of old cars and animals behind the moving ones) or did it after
	 * the draw (the rig's zink: the map without cars, not every time), both passing the tests above. Cleared by
	 * drawing over the rect, which it keeps in order. */
	if (_scissored_clear_ok && renderer != nullptr && std::string_view(renderer).find("zink") != std::string_view::npos) {
		_scissored_clear_ok = false;
		Diary("zink odklada mazani: karta maze kreslenim", true);
	}
	Step("zkouska instanci (glVertexAttribDivisor)");
	_instanced_arrays_ok = TestInstancedArrays();
	if (!_instanced_arrays_ok) Diary("karta neumi atributy po instancich (ARB_instanced_arrays): z oci kazdy objekt zvlast", true);
	Step("zkouska kosti na karte");
	_bones_ok = TestBones();
	Step("preklad programu snimku z oci (shader)");
	StartEyeFrame();
	/* A card whose memory is the system's keeps less (GPU_BYTES_KEPT_SHARED). */
	const std::string_view name = renderer != nullptr ? renderer : "";
	_card_budget = GPU_BYTES_KEPT;
	_card_at_most = GPU_BYTES_AT_MOST;
	for (std::string_view shared : {"Mali", "Adreno", "PowerVR", "Vortek", "virgl", "llvmpipe", "softpipe"}) {
		if (name.find(shared) != std::string_view::npos) {
			_card_budget = GPU_BYTES_KEPT_SHARED;
			_card_at_most = GPU_BYTES_AT_MOST_SHARED;
		}
	}
	describe = fmt::format("{}, OpenGL {}, textures up to {}, pictures up to {}, over {} MB lets go of what was not drawn for {} s, holds {} MB at most{}", renderer != nullptr ? renderer : "?", version, _max_texture, _max_target, _card_budget >> 20, CARD_KEPT_SECONDS.count(), _card_at_most >> 20, _frame_programs ? "" : ", the view from the eyes in parts");
	return true;
}

void ForgetCard()
{
	Step("vse z karty pryc");
	for (auto &[uid, s] : _card_shapes) {
		gl.glDeleteVertexArrays(1, &s.vao);
		if (s.vao_instanced != 0) gl.glDeleteVertexArrays(1, &s.vao_instanced);
		const GLuint buffers[] = {s.pos, s.nrm, s.uv, s.wheel};
		for (GLuint b : buffers) if (b != 0) gl.glDeleteBuffers(1, &b);
	}
	for (auto &[uid, i] : _card_indices) gl.glDeleteBuffers(1, &i.ibo);
	for (auto &[uid, t] : _card_textures) gl.glDeleteTextures(1, &t.name);
	for (auto &[uid, b] : _card_buildings) {
		gl.glDeleteVertexArrays(1, &b.vao);
		gl.glDeleteBuffers(1, &b.vbo);
	}
	_card_shapes.clear();
	_card_indices.clear();
	_card_textures.clear();
	{
		std::lock_guard lock(_card_texture_mutex);
		std::lock_guard uids_lock(_card_texture_uids_mutex);
		_card_texture_uids.clear();
	}
	_card_buildings.clear();
	{
		std::lock_guard lock(_card_held_mutex);
		_card_held_textures.clear();
		_card_held_buildings.clear();
	}
	_card_bytes = 0;
	for (std::atomic<size_t> &b : _card_bytes_of) b = 0;
	for (auto &[key, t] : _ground_textures) gl.glDeleteTextures(1, &t.name);
	_ground_textures.clear();
	if (_ground_atlas != 0) gl.glDeleteTextures(1, &_ground_atlas);
	_ground_atlas = 0;
	_ground_atlas_size = 0;
	_ground_atlas_places.clear();
	for (ViewPictures &v : _views) ForgetViewPictures(v);
	if (_standing_atlas != 0) {
		gl.glDeleteTextures(1, &_standing_atlas);
		gl.glDeleteTextures(1, &_standing_stream);
	}
	if (_standing_near != 0) gl.glDeleteTextures(1, &_standing_near);
	_standing_atlas = _standing_stream = _standing_near = 0;
	_standing_atlas_made++;
	_standing_atlas_size = 0;
	_standing_atlas_size_now = 0;
	ClearStandingPlaces();
	if (_sv_colour != 0) {
		gl.glDeleteTextures(1, &_sv_colour);
		gl.glDeleteTextures(1, &_sv_lit);
	}
	_sv_colour = _sv_lit = 0;
	if (_sv_depth != 0) gl.glDeleteTextures(1, &_sv_depth);
	_sv_depth = 0;
	_sv_w = _sv_h = 0;
}

void EndCard()
{
	if (gl.glDeleteBuffers != nullptr) {
		ForgetCard();
		if (_fbo != 0) gl.glDeleteFramebuffers(1, &_fbo);
		if (_eye_fbo != 0) gl.glDeleteFramebuffers(1, &_eye_fbo);
		if (_gfbo != 0) gl.glDeleteFramebuffers(1, &_gfbo);
		if (_g_move_fbo[0] != 0) gl.glDeleteFramebuffers(2, _g_move_fbo);
		if (_road_smooth_fbo != 0) gl.glDeleteFramebuffers(1, &_road_smooth_fbo);
		if (_road_smooth_piece != 0) gl.glDeleteTextures(1, &_road_smooth_piece);
		if (_road_smooth_vao != 0) gl.glDeleteVertexArrays(1, &_road_smooth_vao);
		if (_road_smooth_program.program != 0) gl.glDeleteProgram(_road_smooth_program.program);
		for (ViewPictures &v : _views) {
			for (GLuint *fbo : {&v.ground_fbo, &v.ground_tmp_fbo, &v.standing_fbo, &v.moving_fbo}) {
				if (*fbo != 0) gl.glDeleteFramebuffers(1, fbo);
				*fbo = 0;
			}
		}
		if (_ground_vbo != 0) gl.glDeleteBuffers(1, &_ground_vbo);
		if (_ground_vao != 0) gl.glDeleteVertexArrays(1, &_ground_vao);
		if (_ground_program.program != 0) gl.glDeleteProgram(_ground_program.program);
		if (_standing_vbo != 0) gl.glDeleteBuffers(1, &_standing_vbo);
		if (_standing_vao != 0) gl.glDeleteVertexArrays(1, &_standing_vao);
		if (_standing_order_vao != 0) gl.glDeleteVertexArrays(1, &_standing_order_vao);
		if (_shrink_vao != 0) gl.glDeleteVertexArrays(1, &_shrink_vao);
		if (_standing_copy_program.program != 0) gl.glDeleteProgram(_standing_copy_program.program);
		if (_standing_tri_vbo != 0) gl.glDeleteBuffers(1, &_standing_tri_vbo);
		if (_standing_tri_vao != 0) gl.glDeleteVertexArrays(1, &_standing_tri_vao);
		if (_standing_program.program != 0) gl.glDeleteProgram(_standing_program.program);
		if (_standing_building_program.program != 0) gl.glDeleteProgram(_standing_building_program.program);
		if (_standing_shrink_program.program != 0) gl.glDeleteProgram(_standing_shrink_program.program);
		if (_standing_faces_program.program != 0) gl.glDeleteProgram(_standing_faces_program.program);
		if (_standing_faces_vbo != 0) gl.glDeleteBuffers(1, &_standing_faces_vbo);
		if (_standing_faces_vao != 0) gl.glDeleteVertexArrays(1, &_standing_faces_vao);
		if (_sv_fbo != 0) gl.glDeleteFramebuffers(1, &_sv_fbo);
		if (_standing_atlas_fbo != 0) gl.glDeleteFramebuffers(1, &_standing_atlas_fbo);
		_standing_atlas_made++;
		const GLuint rbs[] = {_colour_rb, _lit_rb, _depth_rb, _eye_colour_rb, _eye_glow_rb, _eye_depth_rb, _g_colour_rb};
		for (GLuint rb : rbs) if (rb != 0) gl.glDeleteRenderbuffers(1, &rb);
		if (_night_vbo != 0) gl.glDeleteBuffers(1, &_night_vbo);
		if (_night_vao != 0) gl.glDeleteVertexArrays(1, &_night_vao);
		if (_mesh_program.program != 0) gl.glDeleteProgram(_mesh_program.program);
		if (_night_program.program != 0) gl.glDeleteProgram(_night_program.program);
		if (_eye_building_program.program != 0) gl.glDeleteProgram(_eye_building_program.program);
		if (_eye_mesh_program.program != 0) gl.glDeleteProgram(_eye_mesh_program.program);
		if (_bones_texture != 0) gl.glDeleteTextures(1, &_bones_texture);
		_bones_texture = 0;
		_bones_ok = false;
		EndEyeFrame();
	}
	EndContext();
}

/**
 * Let go of what the card holds and did not draw for CARD_KEPT_SECONDS, the longest not drawn first, when it holds more
 * than _card_budget, till it holds three quarters of that: what the scene draws stays however much it is. Over
 * _card_at_most all not drawn in this picture may go.
 */
void TrimCard()
{
	/* When this value of the clock began; the last one that began CARD_KEPT_SECONDS ago or more. */
	const auto now = std::chrono::steady_clock::now();
	if (_card_clock_times.empty() || _card_clock_times.back().first != _card_clock) _card_clock_times.emplace_back(_card_clock, now);
	uint64_t old_clock = 0;
	bool any_old = false;
	while (_card_clock_times.size() > 1 && now - _card_clock_times[1].second >= CARD_KEPT_SECONDS) _card_clock_times.pop_front();
	if (now - _card_clock_times.front().second >= CARD_KEPT_SECONDS) {
		old_clock = _card_clock_times.front().first;
		any_old = true;
	}
	if (_card_bytes <= _card_budget) return;
	/* Not while what stands is given without waiting: a job of it lays from what the main thread found here (it is
	 * trimmed once the last of them is done, GpuThreadMain()). */
	std::lock_guard trim_lock(_card_trim_mutex);
	if (_standing_async_pending > 0) return;
	const bool too_much = _card_bytes > _card_at_most;
	if (!too_much && !any_old) return;
	const size_t down_to = (too_much ? _card_at_most : _card_budget) / 4 * 3;
	auto may_go = [&](uint64_t used) { return used < _card_clock && (too_much || used < old_clock); };
	struct Held {
		uint64_t used;
		int kind;
		uint64_t uid;
	};
	std::vector<Held> held;
	for (const auto &[uid, s] : _card_shapes) if (may_go(s.used)) held.push_back({s.used, 0, uid});
	for (const auto &[uid, i] : _card_indices) if (may_go(i.used)) held.push_back({i.used, 1, uid});
	for (const auto &[uid, t] : _card_textures) if (may_go(t.used)) held.push_back({t.used, 2, uid});
	for (const auto &[uid, b] : _card_buildings) if (may_go(b.used)) held.push_back({b.used, 3, uid});
	if (held.empty()) return;
	std::ranges::sort(held, [](const Held &a, const Held &b) { return a.used < b.used; });
	Step(fmt::format("uvolneni karty ({} MB{})", _card_bytes >> 20, too_much ? ", nad nejvyssi mez" : ""));
	_card_trims++;
	for (const Held &h : held) {
		if (_card_bytes <= down_to) break;
		if (h.kind == 0) {
			GpuShape &s = _card_shapes[h.uid];
			gl.glDeleteVertexArrays(1, &s.vao);
			if (s.vao_instanced != 0) gl.glDeleteVertexArrays(1, &s.vao_instanced);
			const GLuint buffers[] = {s.pos, s.nrm, s.uv, s.wheel, s.joints, s.weights};
			for (GLuint b : buffers) if (b != 0) gl.glDeleteBuffers(1, &b);
			_card_bytes -= s.bytes;
			_card_bytes_of[0] -= s.bytes;
			_card_shapes.erase(h.uid);
		} else if (h.kind == 1) {
			GpuIndices &i = _card_indices[h.uid];
			gl.glDeleteBuffers(1, &i.ibo);
			_card_bytes -= i.bytes;
			_card_bytes_of[1] -= i.bytes;
			_card_indices.erase(h.uid);
		} else if (h.kind == 2) {
			GpuTexture &t = _card_textures[h.uid];
			gl.glDeleteTextures(1, &t.name);
			_card_bytes -= t.bytes;
			_card_bytes_of[2] -= t.bytes;
			_card_textures.erase(h.uid);
			CardHeld(_card_held_textures, h.uid, false);
			std::lock_guard lock(_card_texture_mutex);
			std::lock_guard uids_lock(_card_texture_uids_mutex);
			_card_texture_uids.erase(h.uid);
		} else {
			GpuBuilding &b = _card_buildings[h.uid];
			gl.glDeleteVertexArrays(1, &b.vao);
			gl.glDeleteBuffers(1, &b.vbo);
			_card_bytes -= b.bytes;
			_card_bytes_of[3] -= b.bytes;
			_card_buildings.erase(h.uid);
			CardHeld(_card_held_buildings, h.uid, false);
		}
	}
}

/**
 * What the card keeps a shape's corners by: shapes that hold the very same lists of them (ThreeDSharedList: a tractor's
 * records for each trailer it pulls) one set of buffers, by the lists' numbers; a figure on bones and a shape with no
 * such lists by its own number.
 */
static uint64_t CardShapeKey(const ThreeDVehicleShape &s)
{
	if (!s.corner_joints.empty() || s.pos.Identity() == 0 || s.uv.Identity() == 0) return s.uid.value;
	uint64_t h = 0x9e3779b97f4a7c15ull;
	for (uint64_t id : {s.pos.Identity(), s.nrm.Identity(), s.uv.Identity(), s.corner_wheel.Identity()}) {
		h ^= id + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
		h *= 0xff51afd7ed558ccdull;
		h ^= h >> 33;
	}
	return h | 1ull << 63; // (apart from the shapes' own numbers, counted from one)
}

/** The corners of a shape on the card: put there the first time. */
const GpuShape *ShapeOnCard(const ThreeDVehicleShape &s)
{
	const uint64_t key = CardShapeKey(s);
	auto it = _card_shapes.find(key);
	if (it != _card_shapes.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	const size_t corners = s.pos.size() / 3;
	if (corners == 0 || s.uv.size() < corners * 2) return nullptr;
	GpuShape g;
	g.has_nrm = s.nrm.size() == s.pos.size();
	g.corners = corners;
	Step(fmt::format("tvar na kartu: {} rohu{}", corners, g.has_nrm ? ", normaly" : ""));
	gl.glGenVertexArrays(1, &g.vao);
	gl.glBindVertexArray(g.vao);
	auto buffer = [](GLuint &name, const std::vector<float> &data, size_t floats, GLuint index, GLint size) {
		gl.glGenBuffers(1, &name);
		gl.glBindBuffer(GL_ARRAY_BUFFER, name);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(floats * sizeof(float)), data.data(), GL_STATIC_DRAW);
		gl.glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, 0, nullptr);
		gl.glEnableVertexAttribArray(index);
	};
	buffer(g.pos, s.pos, corners * 3, 0, 3);
	if (g.has_nrm) buffer(g.nrm, s.nrm, corners * 3, 1, 3);
	buffer(g.uv, s.uv, corners * 2, 2, 2);
	/* Each corner's turning wheel (the view from the eyes turns them, EyeMeshSource()), as a float: a byte read as a
	 * whole number (not normalized) is a format the phone's card has not, and zink then rewrites the vertex shader for
	 * it as the first figure is drawn. In every one of the player's diaries where the first figure from the eyes was
	 * one with turning wheels the phone's driver fell in its shader compiler there or a step later (five of five, 6. to
	 * 15. 11.: SIGSEGV in Mali's cmpbe_v2_compile_multiple_shaders, Winlator "CRASH_NATIVE"); where another figure was
	 * first, all its kinds passed, the wheels too. Plain floats need no such rewriting. */
	g.has_wheel = s.corner_wheel.size() == corners;
	if (g.has_wheel) {
		std::vector<float> wheel(s.corner_wheel.begin(), s.corner_wheel.end());
		gl.glGenBuffers(1, &g.wheel);
		gl.glBindBuffer(GL_ARRAY_BUFFER, g.wheel);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(wheel.size() * sizeof(float)), wheel.data(), GL_STATIC_DRAW);
		gl.glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
		gl.glEnableVertexAttribArray(3);
	}
	/* A figure on bones at rest (ThreeDBonesFrame): each corner's four joints, bytes read as whole numbers in floats,
	 * and how much each moves it -- the card poses it by its frame's joints (MESH_VERTEX). */
	g.has_bones = s.corner_joints.size() == corners * 4 && s.corner_weights.size() == corners * 4;
	if (g.has_bones) {
		/* The joints as floats too, for the same reason as the wheels. */
		std::vector<float> joints(s.corner_joints.begin(), s.corner_joints.end());
		gl.glGenBuffers(1, &g.joints);
		gl.glBindBuffer(GL_ARRAY_BUFFER, g.joints);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(joints.size() * sizeof(float)), joints.data(), GL_STATIC_DRAW);
		gl.glVertexAttribPointer(9, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
		gl.glEnableVertexAttribArray(9);
		buffer(g.weights, s.corner_weights, corners * 4, 10, 4);
	}
	gl.glBindVertexArray(0);
	g.bytes = corners * (sizeof(float) * (g.has_nrm ? 8 : 5) + (g.has_wheel ? sizeof(float) : 0) + (g.has_bones ? 8 * sizeof(float) : 0));
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[0] += g.bytes;
	_card_sent_of[0] += g.bytes;
	_card_sent += g.bytes;
	return &_card_shapes.emplace(key, g).first->second;
}

/** The triangles of a shape on the card. */
const GpuIndices *IndicesOnCard(const ThreeDGpuTriangles &t)
{
	auto it = _card_indices.find(t.uid);
	if (it != _card_indices.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	/* Filled as an array (an array of elements is a vertex array's, bound only when drawn). */
	GpuIndices g;
	for (uint32_t i : t.indices) g.largest = std::max(g.largest, i);
	Step(fmt::format("trojuhelniky na kartu: {} (nejvyssi roh {})", t.indices.size() / 3, g.largest));
	gl.glGenBuffers(1, &g.ibo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, g.ibo);
	gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(t.indices.size() * sizeof(uint32_t)), t.indices.data(), GL_STATIC_DRAW);
	g.bytes = t.indices.size() * sizeof(uint32_t);
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[1] += g.bytes;
	_card_sent_of[1] += g.bytes;
	_card_sent += g.bytes;
	return &_card_indices.emplace(t.uid, g).first->second;
}

/**
 * Let go of a texture the card holds with fewer channels than asked for now (MaskOnCard()): it is put on again. Not
 * else: the same pixels are never a mask and a texture drawn in its colours.
 * @return false: not now, while what stands is given without waiting (TrimCard()); not drawn by the card this time
 */
static bool LetGoOfTextureWithFewerChannels(std::unordered_map<uint64_t, GpuTexture>::iterator it)
{
	std::lock_guard trim_lock(_card_trim_mutex);
	if (_standing_async_pending > 0) return false;
	gl.glDeleteTextures(1, &it->second.name);
	_card_bytes -= it->second.bytes;
	_card_bytes_of[2] -= it->second.bytes;
	CardHeld(_card_held_textures, it->first, false);
	{
		std::lock_guard lock(_card_texture_mutex);
		std::lock_guard uids_lock(_card_texture_uids_mutex);
		_card_texture_uids.erase(it->first);
	}
	_card_textures.erase(it);
	return true;
}

/** A texture on the card, its texels as they are (Colour::data). */
const GpuTexture *TextureOnCard(const ThreeDObjectTexture &t)
{
	auto it = _card_textures.find(t.uid.value);
	if (it != _card_textures.end() && it->second.channels < 4 && !LetGoOfTextureWithFewerChannels(it)) return nullptr;
	if (it = _card_textures.find(t.uid.value); it != _card_textures.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	/* Its pixels read under the lock: none let go of meanwhile (ThreeDGpuTexturesLock()). One whose pixels were let go
	 * of as the card held it, and the card let it go since: not drawn this picture, made again by who asks for it. */
	std::lock_guard pixels_lock(_card_texture_mutex);
	if (t.w <= 0 || t.h <= 0 || t.w > _max_texture || t.h > _max_texture || t.px.size() < static_cast<size_t>(t.w) * t.h) return nullptr;
	Step(fmt::format("textura na kartu: {}x{}", t.w, t.h));
	GpuTexture g;
	gl.glGenTextures(1, &g.name);
	gl.glBindTexture(GL_TEXTURE_2D, g.name);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t.px.data());
	g.bytes = static_cast<size_t>(t.w) * t.h * 4;
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[2] += g.bytes;
	_card_sent_of[2] += g.bytes;
	_card_sent += g.bytes;
	{
		std::lock_guard uids_lock(_card_texture_uids_mutex);
		_card_texture_uids.insert(t.uid.value);
	}
	CardHeld(_card_held_textures, t.uid.value, true);
	return &_card_textures.emplace(t.uid.value, g).first->second;
}

/**
 * A mask of a vehicle's paint on the card (PaintMasksOnCard()): only the channels the programs read of it -- its red
 * (MaskAt() of MESH_FRAGMENT), the rust of pieces its red and green --, a quarter or a half of it with all four
 * (TextureOnCard()). From the eyes among the lorries of a depot the masks were 90 MB of the 700 the card held, as many
 * as their textures and each of them as big.
 * @param channels 1 its red, 2 its red and green
 */
static const GpuTexture *MaskOnCard(const ThreeDObjectTexture &t, int channels)
{
	auto it = _card_textures.find(t.uid.value);
	if (it != _card_textures.end() && it->second.channels < channels && !LetGoOfTextureWithFewerChannels(it)) return nullptr;
	if (it = _card_textures.find(t.uid.value); it != _card_textures.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	if (!_gpu_masks_by_channel) return TextureOnCard(t);
	/* Its pixels read under the lock, as TextureOnCard(). */
	std::lock_guard pixels_lock(_card_texture_mutex);
	if (t.w <= 0 || t.h <= 0 || t.w > _max_texture || t.h > _max_texture || t.px.size() < static_cast<size_t>(t.w) * t.h) return nullptr;
	Step(fmt::format("maska na kartu: {}x{}, {} kanal(y)", t.w, t.h, channels));
	static std::vector<uint8_t> bytes; // (the card's thread alone)
	const size_t n = static_cast<size_t>(t.w) * t.h;
	bytes.resize(n * channels);
	for (size_t i = 0; i < n; i++) {
		const Colour c(t.px[i]);
		bytes[i * channels] = c.r;
		if (channels > 1) bytes[i * channels + 1] = c.g;
	}
	GpuTexture g;
	gl.glGenTextures(1, &g.name);
	gl.glBindTexture(GL_TEXTURE_2D, g.name);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, channels == 1 ? GL_R8 : GL_RG8, t.w, t.h, 0, channels == 1 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, bytes.data());
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	/* Not kept: the largest mask's bytes would stay as long as the game. */
	if (bytes.capacity() > (4u << 20)) std::vector<uint8_t>().swap(bytes);
	g.bytes = n * channels;
	g.channels = static_cast<uint8_t>(channels);
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[2] += g.bytes;
	_card_sent_of[2] += g.bytes;
	_card_sent += g.bytes;
	{
		std::lock_guard uids_lock(_card_texture_uids_mutex);
		_card_texture_uids.insert(t.uid.value);
	}
	CardHeld(_card_held_textures, t.uid.value, true);
	return &_card_textures.emplace(t.uid.value, g).first->second;
}

/**
 * A pattern laid over a building's part (ThreeDGpuBuildingPaint::pattern) on the card: repeating, smoothed, with its
 * smaller pictures made by the card -- one for every building it is laid on.
 */
const GpuTexture *PatternOnCard(const ThreeDObjectTexture &t)
{
	auto it = _card_textures.find(t.uid.value);
	if (it != _card_textures.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	if (t.w <= 0 || t.h <= 0 || t.w > _max_texture || t.h > _max_texture || t.px.size() < static_cast<size_t>(t.w) * t.h) return nullptr;
	Step(fmt::format("tapeta na kartu: {}x{}", t.w, t.h));
	GpuTexture g;
	gl.glGenTextures(1, &g.name);
	gl.glBindTexture(GL_TEXTURE_2D, g.name);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t.px.data());
	gl.glGenerateMipmap(GL_TEXTURE_2D);
	g.bytes = static_cast<size_t>(t.w) * t.h * 4 * 4 / 3;
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[2] += g.bytes;
	_card_sent_of[2] += g.bytes;
	_card_sent += g.bytes;
	CardHeld(_card_held_textures, t.uid.value, true);
	return &_card_textures.emplace(t.uid.value, g).first->second;
}

/** A mask of details on the card, its index in the red of each texel (8 bits as it is). */
const GpuTexture *DetailMaskOnCard(const ThreeDDetailMask &m)
{
	auto it = _card_textures.find(m.uid.value);
	if (it != _card_textures.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	if (m.w <= 0 || m.h <= 0 || m.w > _max_texture || m.h > _max_texture || m.index.size() < static_cast<size_t>(m.w) * m.h) return nullptr;
	Step(fmt::format("maska podrobnosti na kartu: {}x{}", m.w, m.h));
	GpuTexture g;
	gl.glGenTextures(1, &g.name);
	gl.glBindTexture(GL_TEXTURE_2D, g.name);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m.w, m.h, 0, GL_RED, GL_UNSIGNED_BYTE, m.index.data());
	g.bytes = static_cast<size_t>(m.w) * m.h;
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[2] += g.bytes;
	_card_sent_of[2] += g.bytes;
	_card_sent += g.bytes;
	CardHeld(_card_held_textures, m.uid.value, true);
	return &_card_textures.emplace(m.uid.value, g).first->second;
}

/** The picture drawn into, at least as big as asked (grown in steps, shrunk again after a very big one). */
bool TargetOfSize(int w, int h)
{
	const bool too_big = static_cast<int64_t>(_fbo_w) * _fbo_h > 16 * 1024 * 1024 && static_cast<int64_t>(w) * h * 4 < static_cast<int64_t>(_fbo_w) * _fbo_h;
	if (w <= _fbo_w && h <= _fbo_h && !too_big) return true;
	const int nw = too_big ? w : std::max(_fbo_w, w), nh = too_big ? h : std::max(_fbo_h, h);
	_fbo_w = std::min(_max_target, (nw + 255) / 256 * 256);
	_fbo_h = std::min(_max_target, (nh + 255) / 256 * 256);
	Step(fmt::format("ramec {}x{} (glRenderbufferStorage, glCheckFramebufferStatus)", _fbo_w, _fbo_h));
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _colour_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, _fbo_w, _fbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _lit_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_R8, _fbo_w, _fbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _depth_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, _fbo_w, _fbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _colour_rb);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, _lit_rb);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depth_rb);
	if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		_fbo_w = _fbo_h = 0;
		return false;
	}
	return w <= _fbo_w && h <= _fbo_h;
}

ThreeDGpuMatrix Multiply(const ThreeDGpuMatrix &a, const ThreeDGpuMatrix &b)
{
	ThreeDGpuMatrix m{};
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			float s = 0;
			for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
			m[r * 4 + c] = s;
		}
	}
	return m;
}

/** The determinant of a matrix: below 0 where it mirrors what it places (its corners' order turned round). */
static double Determinant(const ThreeDGpuMatrix &m)
{
	auto minor = [&m](int skip_row, int skip_col) {
		double a[9];
		int k = 0;
		for (int r = 0; r < 4; r++) {
			if (r == skip_row) continue;
			for (int c = 0; c < 4; c++) {
				if (c != skip_col) a[k++] = m[r * 4 + c];
			}
		}
		return a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
	};
	double d = 0;
	for (int c = 0; c < 4; c++) d += (c % 2 == 0 ? 1 : -1) * m[c] * minor(0, c);
	return d;
}

/**
 * The side of a vehicle's triangles facing away left out as the processor leaves it out (ThreeDVehicleShape::FacesAway()),
 * for a run of them of one side (facing; 0 both drawn) placed by a matrix of that determinant: a triangle seen from its
 * outer side goes round counter-clockwise on the card where its side and the determinant differ in sign.
 */
static void CullVehicleRun(int8_t facing, double determinant)
{
	if (facing == 0 || determinant == 0) {
		gl.glDisable(GL_CULL_FACE);
		return;
	}
	gl.glEnable(GL_CULL_FACE);
	gl.glCullFace(GL_BACK);
	gl.glFrontFace(facing * determinant < 0 ? GL_CCW : GL_CW);
}

/* A texture's paint on the card: its masks' names (ThreeDGpuPaint). */
struct PaintOnCard {
	GLuint detail = 0, detail_colours = 0, paint_mask = 0, addon_mask = 0, rust_mask = 0;
};

/** A texture's paint's masks put on the card: false, one it cannot hold. */
static bool PaintMasksOnCard(const ThreeDGpuPaint &paint, PaintOnCard &pc)
{
	/* The colours of the details as they are; the masks only the channels read of them (MaskOnCard()). */
	auto mask = [](const ThreeDObjectTexture *tex, GLuint &name, int channels) {
		if (tex == nullptr) return true;
		const GpuTexture *on = channels < 4 ? MaskOnCard(*tex, channels) : TextureOnCard(*tex);
		if (on == nullptr) return false;
		name = on->name;
		return true;
	};
	if (paint.detail != nullptr) {
		const GpuTexture *on = DetailMaskOnCard(*paint.detail);
		if (on == nullptr || paint.detail_colours == nullptr) return false;
		pc.detail = on->name;
	}
	return mask(paint.detail_colours, pc.detail_colours, 4) && mask(paint.paint_mask, pc.paint_mask, 1) && mask(paint.addon_mask, pc.addon_mask, 1) &&
			mask(paint.rust_mask, pc.rust_mask, paint.rust_pieces != nullptr ? 2 : 1);
}

/**
 * How a program (MESH_FRAGMENT's Paint()) paints the texture drawn next: what of it to do (its bits), its colours, and
 * its masks on units 1 to 5 (the program's samplers set to them).
 */
/**
 * What SetPaint() set last for one picture of the card being drawn (DrawPictureAt()): the uniforms' values and the
 * masks on the units 1 to 5, so a run of a mesh after another with the same sets nothing again (the player, 17. 11.:
 * "zoom out je slabý a cuká"; a vehicle drawn by the card took about a hundred OpenGL calls, most of them its paint set
 * again run after run). Known only while one picture is drawn: nothing else touches them meanwhile.
 */
struct PaintSet {
	bool known = false;
	int bits = 0;
	std::array<float, 20> values{};
	std::array<GLuint, 5> units{};
	bool units_known = false;
};

/**
 * What DrawPictureAt() left set for the next picture of a batch drawn into one frame (the turned map's vehicles,
 * draw_vehicles of ThreeDGpuDrawStanding(); DrawPicturesOnCard()): the program, its samplers, the depth and the blend
 * as a picture begins, and what its runs set last -- set again only what differs (the player's diary of 18. 11.: 107
 * calls a vehicle, of the rig's 44 four were drawing; z5 6 frames a second, the main thread waiting for the card).
 * Rig 'test3d stojici stav 0' (not lean): each picture all as before.
 */
struct PictureBatch {
	bool begun = false;    ///< the mesh program in use, its samplers set, no blend, the depth test as a picture's
	PaintSet paint_set;
	float glass_alpha_set = NAN;
	std::array<float, 4> glow_set{NAN, NAN, NAN, NAN};
	int texel_alpha_set = -1, glass_set = -1, cull_set = -2, bone_row_set = INT_MIN;
	GLuint texture_set = 0, vao_set = 0, ibo_set = 0;
	bool night_set = false; ///< the program of the lights at night in use, its corners' array bound (PicturePass::Night)
};

/** After the last picture of a batch (DrawPictureAt()): left as a picture drawn alone leaves it. */
static void EndPictureBatch(const PictureBatch &b)
{
	if (b.cull_set != -2) {
		gl.glDisable(GL_CULL_FACE);
		gl.glFrontFace(GL_CCW);
	}
	gl.glBindVertexArray(0);
}

/**
 * The light of a mesh (its direction) and whether it is lit by it, one call: MESH_VERTEX's u_light_lit (the player's
 * diary of 18. 11.: a vehicle of the turned map took a vec3 and a float, of the 17 calls a vehicle).
 */
static void SetLight(const GpuProgram &program, const ThreeDGpuMesh &mesh, const GpuShape &shape)
{
	gl.glUniform4f(program.light, mesh.light[0], mesh.light[1], mesh.light[2], mesh.lit && shape.has_nrm ? 1.0f : 0.0f);
}

static void SetPaint(const GpuProgram &program, const ThreeDGpuPaint *paint, const PaintOnCard *pc, PaintSet *set = nullptr)
{
	if (paint == nullptr || pc == nullptr || !paint->Any()) {
		if (set != nullptr && set->known && set->bits == 0) return;
		gl.glUniform1i(program.paint, 0);
		if (set != nullptr) {
			set->known = true;
			set->bits = 0;
		}
		return;
	}
	constexpr int PAINT_DETAIL = 1, PAINT_BODY = 2, PAINT_MASK = 4, PAINT_COLOURED = 8, PAINT_ADDON = 16, PAINT_TINT = 32, PAINT_RUST = 64, PAINT_GRADUAL = 128, PAINT_PIECES = 256;
	int bits = 0;
	if (pc->detail != 0) bits |= PAINT_DETAIL;
	if (paint->body) bits |= PAINT_BODY;
	if (pc->paint_mask != 0) bits |= PAINT_MASK;
	if (paint->coloured) bits |= PAINT_COLOURED;
	if (pc->addon_mask != 0) bits |= PAINT_ADDON;
	if (paint->tinted) bits |= PAINT_TINT;
	if (pc->rust_mask != 0) bits |= PAINT_RUST;
	if (paint->rust_gradual) bits |= PAINT_GRADUAL;
	if (pc->rust_mask != 0 && paint->rust_pieces != nullptr) bits |= PAINT_PIECES;
	const std::array<float, 20> values = {paint->colour[0], paint->colour[1], paint->colour[2], paint->target[0], paint->target[1], paint->target[2],
			paint->start, paint->fade, paint->addon_colour[0], paint->addon_colour[1], paint->addon_colour[2], paint->addon_start,
			paint->tint[0], paint->tint[1], paint->tint[2], paint->rust_colour[0], paint->rust_colour[1], paint->rust_colour[2], paint->rust_time, paint->rust_ramp};
	const GLuint units[5] = {pc->detail, pc->detail_colours, pc->paint_mask, pc->addon_mask, pc->rust_mask};
	const bool same_values = set != nullptr && set->known && set->bits == bits && set->values == values && (bits & PAINT_PIECES) == 0;
	if (same_values) {
		/* The uniforms as they are: only the masks that differ bound. */
		bool active_changed = false;
		for (int k = 0; k < 5; k++) {
			if (units[k] == 0 || (set->units_known && set->units[k] == units[k])) continue;
			gl.glActiveTexture(GL_TEXTURE0 + 1 + k);
			gl.glBindTexture(GL_TEXTURE_2D, units[k]);
			set->units[k] = units[k];
			active_changed = true;
		}
		if (active_changed) gl.glActiveTexture(GL_TEXTURE0);
		return;
	}
	if (set != nullptr) {
		set->known = true;
		set->bits = bits;
		set->values = values;
	}
	gl.glUniform1i(program.paint, bits);
	/* Its colours and numbers in one call (MESH_FRAGMENT's u_paint_values; the player's diary of 18. 11.: ten calls of a
	 * vehicle's paint each). */
	const std::array<float, 20> packed = {paint->colour[0], paint->colour[1], paint->colour[2], paint->start,
			paint->target[0], paint->target[1], paint->target[2], paint->fade,
			paint->addon_colour[0], paint->addon_colour[1], paint->addon_colour[2], paint->addon_start,
			paint->tint[0], paint->tint[1], paint->tint[2], paint->rust_time,
			paint->rust_colour[0], paint->rust_colour[1], paint->rust_colour[2], paint->rust_ramp};
	gl.glUniform4fv(program.paint_values, 5, packed.data());
	if ((bits & PAINT_PIECES) != 0) {
		/* The moments of the vehicle's pieces of rust, in its order (ThreeDVehicleRust::Moments()). */
		std::array<float, THREE_D_RUST_PIECES> moments;
		paint->rust_pieces->Moments(paint->rust_variant > 0 ? paint->rust_variant - 1u : 0u, moments);
		gl.glUniform1fv(program.rust_moments, THREE_D_RUST_PIECES, moments.data());
		gl.glUniform1f(program.rust_delay, paint->rust_pieces->delay_share);
	}
	bool active_changed = false;
	for (int k = 0; k < 5; k++) {
		if (units[k] == 0 || (set != nullptr && set->units_known && set->units[k] == units[k])) continue;
		gl.glActiveTexture(GL_TEXTURE0 + 1 + k);
		gl.glBindTexture(GL_TEXTURE_2D, units[k]);
		if (set != nullptr) set->units[k] = units[k];
		active_changed = true;
	}
	if (set != nullptr && !set->units_known) {
		/* Those not bound now (0) left as they were: not known. */
		for (int k = 0; k < 5; k++) {
			if (units[k] == 0) set->units[k] = UINT32_MAX;
		}
		set->units_known = true;
	}
	if (active_changed || set == nullptr) gl.glActiveTexture(GL_TEXTURE0);
}

/**
 * A frame's joints on the card (BONE_UNIT): their row of the texture, -1 when they cannot be (more than a corner names).
 * Its texture made the first time.
 */
static int BonesOnCard(const ThreeDBonesFrame &b)
{
	const size_t texels = b.joints.size() / 4;
	if (texels == 0 || texels > ThreeDVehicleShape::MAX_JOINTS * 3) return -1;
	gl.glActiveTexture(GL_TEXTURE0 + BONE_UNIT);
	if (_bones_texture == 0) {
		gl.glGenTextures(1, &_bones_texture);
		gl.glBindTexture(GL_TEXTURE_2D, _bones_texture);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(ThreeDVehicleShape::MAX_JOINTS * 3), BONE_ROWS, 0, GL_RGBA, GL_FLOAT, nullptr);
	} else {
		gl.glBindTexture(GL_TEXTURE_2D, _bones_texture);
	}
	const int row = _bones_next_row;
	_bones_next_row = (_bones_next_row + 1) % BONE_ROWS;
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, static_cast<GLsizei>(texels), 1, GL_RGBA, GL_FLOAT, b.joints.data());
	gl.glActiveTexture(GL_TEXTURE0);
	return row;
}


/** A program's samplers of the paint's masks to units 1 to 5 (SetPaint()), its texture's to 0, its joints' to BONE_UNIT. */
static void PaintUnits(const GpuProgram &program)
{
	gl.glUniform1i(program.tex, 0);
	gl.glUniform1i(program.detail, 1);
	gl.glUniform1i(program.detail_colours, 2);
	gl.glUniform1i(program.paint_mask, 3);
	gl.glUniform1i(program.addon_mask, 4);
	gl.glUniform1i(program.rust_mask, 5);
	gl.glUniform1i(program.bones, BONE_UNIT);
	gl.glUniform1i(program.bone_row, -1);
}

/**
 * Nothing on the units of the paint's masks: what the view from the eyes draws into is read from those units too (its
 * depth, DrawEyeFrameOnCard()), and a program that could read what it draws into draws what the card pleases.
 */
static void ClearPaintUnits()
{
	for (int k = 1; k <= 5; k++) {
		gl.glActiveTexture(GL_TEXTURE0 + k);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
	}
	gl.glActiveTexture(GL_TEXTURE0);
}

/** A mesh of a picture as the card draws it (PictureOnCard()): its shape, its triangles, its textures and their paints there. */
struct MeshOnCard {
	const ThreeDGpuMesh *mesh;
	const GpuShape *shape;
	const GpuIndices *indices;
	std::vector<GLuint> textures;
	std::vector<PaintOnCard> paints;
};

/**
 * What a picture is drawn with first (its first mesh's corners and texture): the pictures of a batch drawn by it, those
 * of one shape and texture one after another, their arrays, texture and masks bound once for all of them (the wagons
 * of a train, the cars of one kind; the rig's 96 Tatras of 16 kinds bound the texture twice a vehicle, the vertex array
 * almost once). Each picture of a batch has its own rectangle of the frame and its depths there: the order changes no
 * pixel. Rig 'test3d stojici stav 0': drawn as placed.
 */
static std::pair<GLuint, GLuint> PictureKind(const std::vector<MeshOnCard> &draws)
{
	if (draws.empty()) return {0, 0};
	const MeshOnCard &d = draws.front();
	return {d.shape->vao, d.textures.empty() ? 0 : d.textures.front()};
}

/**
 * Put on the card what a picture draws (DrawPicturesOnCard()): its shapes, triangles, textures and masks.
 * @return false: something the card cannot hold, or triangles naming a corner their shape has not; the processor draws it
 */
bool PictureOnCard(const ThreeDGpuPicture &pic, std::vector<MeshOnCard> &draws)
{
	for (const ThreeDGpuMesh &m : pic.meshes) {
		if (m.shape == nullptr || m.triangles == nullptr || m.triangles->indices.empty()) continue;
		MeshOnCard d{&m, ShapeOnCard(*m.shape), IndicesOnCard(*m.triangles), {}, {}};
		if (d.shape == nullptr || d.indices == nullptr) return false;
		/* A figure on bones whose shape at rest has not its joints: the processor poses it. */
		if (m.bones != nullptr && !d.shape->has_bones) return false;
		/* A triangle naming a corner the shape has not would have the card read beyond its corners: a phone's card
		 * does not forgive that. The processor draws such a picture. */
		if (d.indices->largest >= d.shape->corners) {
			Debug(misc, 0, "3D on the graphics card: triangles naming corner {} of a shape of {}; the processor draws the picture", d.indices->largest, d.shape->corners);
			return false;
		}
		d.textures.assign(m.textures.size(), 0);
		d.paints.assign(m.textures.size(), {});
		for (size_t i = 0; i < m.textures.size(); i++) {
			if (m.textures[i] == nullptr || i >= m.triangles->used.size() || !m.triangles->used[i]) continue;
			const GpuTexture *t = TextureOnCard(*m.textures[i]);
			if (t == nullptr) return false;
			d.textures[i] = t->name;
			if (i >= m.paints.size() || !m.paints[i].Any()) continue;
			/* Its masks on the card too; one it cannot hold, the processor draws the picture. */
			if (!PaintMasksOnCard(m.paints[i], d.paints[i])) return false;
		}
		draws.push_back(std::move(d));
	}
	return true;
}

/** What of a picture is drawn (DrawPicturePass()): its body, its glass over it, the glass of its lights on at night over all. */
enum class PicturePass : uint8_t { Opaque, Glass, Night };

/**
 * A part of a picture drawn into the frame bound, its corner at x, y (DrawPictureAt(), DrawPictureBatch()): the frame
 * cleared, its depths its own -- the pictures beside it are not where it is. Its glass after its body, the lights of the
 * night after its glass; in a batch each part of all its pictures one after another (the depth and the blend set once).
 */
static void DrawPicturePass(const ThreeDGpuPicture &pic, const std::vector<MeshOnCard> &draws, int x, int y, PictureBatch *batch, PicturePass pass)
{
	const int w = pic.width, h = pic.height;
	gl.glViewport(x, y, w, h);
	/* What this picture's runs set last (PaintSet; the player, 17. 11.: "zoom out je slabý a cuká"): set again only when
	 * another -- in a batch what the pictures before set too. Rig 'test3d stojici stav 0' sets all every run as before. */
	const bool lean = _gpu_standing_lean;
	PictureBatch &b = *batch;
	/* The picture's pixels and depths to the card's: a pixel x, y of the picture is the card's pixel x, y of its place,
	 * its rows from the bottom -- so the rows read back are the picture's from the top. */
	const float dn = pic.depth_near, df = pic.depth_far;
	const float dz = df > dn ? 2.0f / (df - dn) : 1.0f;
	const ThreeDGpuMatrix to_card = {
		2.0f / w, 0, 0, -1,
		0, 2.0f / h, 0, -1,
		0, 0, dz, -1 - dn * dz,
		0, 0, 0, 1,
	};
	if (pass == PicturePass::Night) {
		/* The glass of the lights on at night, over the body and its glass: by its own program, bound once for the
		 * pictures of a batch one after another (the mesh program's begun again after). */
		if (pic.night.empty()) return;
		std::vector<float> corners;
		corners.reserve(pic.night.size() * 18);
		for (const ThreeDGpuNightTriangle &t : pic.night) {
			const Colour c(t.colour);
			for (int k = 0; k < 3; k++) corners.insert(corners.end(), {t.x[k], t.y[k], t.depth[k], c.r / 255.0f, c.g / 255.0f, c.b / 255.0f});
		}
		if (!lean || !b.night_set) {
			gl.glUseProgram(_night_program.program);
			gl.glBindVertexArray(_night_vao);
			gl.glBindBuffer(GL_ARRAY_BUFFER, _night_vbo);
			b.night_set = true;
		}
		gl.glUniformMatrix4fv(_night_program.matrix, 1, GL_TRUE, to_card.data());
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(corners.size() * sizeof(float)), corners.data(), GL_STREAM_DRAW);
		gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(pic.night.size() * 3));
		b.begun = false;
		b.vao_set = b.ibo_set = 0;
		return;
	}
	if (!b.begun) {
		gl.glDisable(GL_BLEND);
		gl.glEnable(GL_DEPTH_TEST);
		gl.glDepthFunc(GL_LESS);
		gl.glDepthMask(GL_TRUE);
		gl.glUseProgram(_mesh_program.program);
		PaintUnits(_mesh_program);
		b.bone_row_set = -1;
		gl.glActiveTexture(GL_TEXTURE0);
		b.begun = true;
		b.night_set = false;
	}
	if (!lean || b.glass_alpha_set != pic.glass_alpha) gl.glUniform1f(_mesh_program.glass_alpha, pic.glass_alpha);
	b.glass_alpha_set = pic.glass_alpha;
	auto draw_runs = [&](const MeshOnCard &d, const std::vector<ThreeDGpuTriangles::Run> &runs, bool glass) {
		const ThreeDGpuMatrix m = Multiply(to_card, d.mesh->matrix);
		const double determinant = Determinant(m);
		gl.glUniformMatrix4fv(_mesh_program.matrix, 1, GL_TRUE, m.data());
		SetLight(_mesh_program, *d.mesh, *d.shape);
		if (!lean || b.glass_set != (glass ? 1 : 0)) gl.glUniform1i(_mesh_program.glass, glass ? 1 : 0);
		b.glass_set = glass ? 1 : 0;
		const int bone_row = d.mesh->bones != nullptr && d.shape->has_bones ? BonesOnCard(*d.mesh->bones) : -1;
		if (!lean || bone_row != b.bone_row_set) gl.glUniform1i(_mesh_program.bone_row, bone_row);
		b.bone_row_set = bone_row;
		/* Its corners and its triangles (the element buffer is the vertex array's: bound again with another array). */
		if (!lean || d.shape->vao != b.vao_set) {
			gl.glBindVertexArray(d.shape->vao);
			b.ibo_set = 0;
		}
		b.vao_set = d.shape->vao;
		if (!lean || d.indices->ibo != b.ibo_set) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices->ibo);
		b.ibo_set = d.indices->ibo;
		if (!d.shape->has_nrm) gl.glVertexAttrib3f(1, 0, 0, 1);
		for (const ThreeDGpuTriangles::Run &r : runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0) continue;
			SetPaint(_mesh_program, r.texture < d.mesh->paints.size() ? &d.mesh->paints[r.texture] : nullptr, &d.paints[r.texture], lean ? &b.paint_set : nullptr);
			/* Its material's own light, and see-through by its own alpha (a material "BLEND") or the glass's. */
			const uint32_t glow = d.mesh->Glow(r.texture);
			const Colour e(glow);
			const std::array<float, 4> glow_now{static_cast<float>(e.r), static_cast<float>(e.g), static_cast<float>(e.b), glow == 0 ? 0.0f : (e.a == ThreeDVehicleShape::GLOW_TEXTURED ? 1.0f : 2.0f)};
			if (!lean || glow_now != b.glow_set) gl.glUniform4f(_mesh_program.glow, glow_now[0], glow_now[1], glow_now[2], glow_now[3]);
			b.glow_set = glow_now;
			if (glass) {
				const uint8_t alpha = d.mesh->shape->Alpha(r.texture);
				const float a = alpha < 255 ? alpha / 255.0f : pic.glass_alpha;
				if (!lean || a != b.glass_alpha_set) gl.glUniform1f(_mesh_program.glass_alpha, a);
				b.glass_alpha_set = a;
			}
			const int texel_alpha = glass && d.mesh->shape->Alpha(r.texture) < 255 ? 1 : 0;
			if (!lean || texel_alpha != b.texel_alpha_set) gl.glUniform1i(_mesh_program.texel_alpha, texel_alpha);
			b.texel_alpha_set = texel_alpha;
			if (!lean || b.texture_set != d.textures[r.texture]) gl.glBindTexture(GL_TEXTURE_2D, d.textures[r.texture]);
			b.texture_set = d.textures[r.texture];
			/* Its side left out: set only when another (CullVehicleRun()). */
			const int cull = (r.facing == 0 || determinant == 0) ? 0 : (r.facing * determinant < 0 ? 1 : -1);
			if (!lean || cull != b.cull_set) CullVehicleRun(r.facing, determinant);
			b.cull_set = cull;
			gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(static_cast<uintptr_t>(r.first) * sizeof(uint32_t)));
		}
		if (!lean) {
			gl.glDisable(GL_CULL_FACE);
			gl.glFrontFace(GL_CCW);
		}
	};
	if (pass == PicturePass::Opaque) {
		for (const MeshOnCard &d : draws) draw_runs(d, d.mesh->triangles->opaque, false);
		return;
	}
	/* The glass after all else, over what is behind it, hiding nothing (no depth written: the depth mask and the blend
	 * the caller's, DrawPictureGlassBegin()); lit only where it has a light of its own (its lit 1 with alpha 1 over what
	 * is there, else 0 with alpha 0 leaving it). */
	if (pass == PicturePass::Glass) {
		for (const MeshOnCard &d : draws) {
			if (!d.mesh->triangles->glass.empty()) draw_runs(d, d.mesh->triangles->glass, true);
		}
		return;
	}
}

/** Whether a picture has glass (PicturePass::Glass). */
static bool PictureHasGlass(const std::vector<MeshOnCard> &draws)
{
	return std::ranges::any_of(draws, [](const MeshOnCard &d) { return !d.mesh->triangles->glass.empty(); });
}

/** The depth kept and the glass blended over what is there (PicturePass::Glass); DrawPictureGlassEnd() back. */
static void DrawPictureGlassBegin()
{
	gl.glDepthMask(GL_FALSE);
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void DrawPictureGlassEnd()
{
	gl.glDisable(GL_BLEND);
	gl.glDepthMask(GL_TRUE);
}

/**
 * Draw a picture into the frame bound, its corner at x, y (DrawPicturesOnCard()): its body, its glass, its lights at
 * night (DrawPicturePass()). Alone (batch nullptr) left as it was before it.
 */
void DrawPictureAt(const ThreeDGpuPicture &pic, const std::vector<MeshOnCard> &draws, int x, int y, PictureBatch *batch)
{
	PictureBatch alone;
	PictureBatch &b = batch != nullptr && _gpu_standing_lean ? *batch : alone;
	DrawPicturePass(pic, draws, x, y, &b, PicturePass::Opaque);
	if (PictureHasGlass(draws) || !_gpu_standing_lean) {
		DrawPictureGlassBegin();
		DrawPicturePass(pic, draws, x, y, &b, PicturePass::Glass);
		DrawPictureGlassEnd();
	}
	if (!pic.night.empty()) DrawPicturePass(pic, draws, x, y, &b, PicturePass::Night);
	/* Alone: left as it was before this picture, no side left out, the front counter-clockwise. */
	if (&b == &alone) EndPictureBatch(b);
}

/**
 * Pictures of a batch drawn into the frame bound (draw_vehicles of ThreeDGpuDrawStanding(), DrawPicturesOnCard()): the
 * body of each, then the glass of each, then the lights at night of each -- each picture its own rectangle and depths,
 * so not a pixel otherwise than one by one (the player's diary of 19. 11.: a vehicle of the turned map 29-44 calls, its
 * glass set the depth and the blend on and off, at night its lights the program and its samplers again -- of a vehicle
 * Uniform1i 7.8). Rig 'test3d stojici stav 0': one by one as before.
 * @param at the picture n of the batch, its meshes and its corner
 */
template <typename At>
static void DrawPictureBatch(size_t n, At &&at)
{
	PictureBatch batch;
	if (!_gpu_standing_lean) {
		for (size_t k = 0; k < n; k++) {
			auto [pic, draws, x, y] = at(k);
			DrawPictureAt(*pic, *draws, x, y, &batch);
		}
		if (batch.begun || batch.cull_set != -2) EndPictureBatch(batch);
		return;
	}
	bool glass = false, night = false;
	for (size_t k = 0; k < n; k++) {
		auto [pic, draws, x, y] = at(k);
		DrawPicturePass(*pic, *draws, x, y, &batch, PicturePass::Opaque);
		glass = glass || PictureHasGlass(*draws);
		night = night || !pic->night.empty();
	}
	if (glass) {
		DrawPictureGlassBegin();
		for (size_t k = 0; k < n; k++) {
			auto [pic, draws, x, y] = at(k);
			if (PictureHasGlass(*draws)) DrawPicturePass(*pic, *draws, x, y, &batch, PicturePass::Glass);
		}
		DrawPictureGlassEnd();
	}
	if (night) {
		for (size_t k = 0; k < n; k++) {
			auto [pic, draws, x, y] = at(k);
			if (!pic->night.empty()) DrawPicturePass(*pic, *draws, x, y, &batch, PicturePass::Night);
		}
	}
	if (batch.begun || batch.cull_set != -2) EndPictureBatch(batch);
}

/**
 * Draw pictures on the card and read them back (ThreeDGpuRender()), all at once (the player, 20. 10.: "fix the card so
 * it works, it has more power to draw than the processor"; a picture alone was sent, drawn, waited for and read back,
 * 5-19 ms each on the player's phone, a turn of the map a thousand of them): side by side in rows in one frame of the
 * card, its pixels read back once, as many frames as they fill. Each picture's ThreeDGpuPicture::drawn tells whether
 * it was drawn; one not drawn the processor draws.
 */
void DrawPicturesOnCard(std::span<ThreeDGpuPicture *const> pics)
{
	_card_clock++;
	struct Placed {
		ThreeDGpuPicture *pic;
		std::vector<MeshOnCard> draws;
		int x = 0, y = 0;
	};
	std::vector<Placed> placed;
	for (ThreeDGpuPicture *pic : pics) {
		pic->drawn = false;
		if (pic->width <= 0 || pic->height <= 0 || pic->width > _max_target || pic->height > _max_target) continue;
		/* All on the card first: what it cannot hold the processor draws. */
		Placed p{pic, {}};
		if (PictureOnCard(*pic, p.draws)) placed.push_back(std::move(p));
	}
	/* The tallest first, in rows: little left empty. */
	std::ranges::stable_sort(placed, [](const Placed &a, const Placed &b) { return a.pic->height > b.pic->height; });
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	std::vector<uint32_t> colours;
	std::vector<uint8_t> lit;
	for (size_t at = 0; at < placed.size();) {
		/* A frame as wide as 2048 or the widest picture, as tall as its rows. */
		int frame_w = 2048;
		for (size_t k = at; k < placed.size(); k++) frame_w = std::max(frame_w, placed[k].pic->width);
		frame_w = std::min(frame_w, _max_target);
		int x = 0, y = 0, row = 0;
		size_t end = at;
		bool read_lit = false;
		for (; end < placed.size(); end++) {
			const ThreeDGpuPicture &pic = *placed[end].pic;
			if (x + pic.width > frame_w) {
				y += row;
				x = 0;
				row = 0;
			}
			if (y + pic.height > (end > at ? FRAME_ROWS : _max_target)) break;
			placed[end].x = x;
			placed[end].y = y;
			x += pic.width;
			row = std::max(row, pic.height);
			read_lit |= pic.read_lit;
		}
		const int frame_h = y + row;
		const int64_t was = _card_biggest.load();
		if (static_cast<int64_t>(frame_w) * frame_h > (was >> 32) * (was & 0xFFFFFFFF)) _card_biggest = static_cast<int64_t>(frame_w) << 32 | frame_h;
		Step(fmt::format("obrazky {}-{}: {} najednou, {}x{}", _gpu_pictures + 1 + at, _gpu_pictures + end, end - at, frame_w, frame_h));
		if (!TargetOfSize(frame_w, frame_h)) return;
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
		gl.glDrawBuffers(2, both);
		gl.glDisable(GL_DITHER);
		gl.glDisable(GL_CULL_FACE);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glScissor(0, 0, frame_w, frame_h);
		gl.glDepthMask(GL_TRUE);
		gl.glClearColor(0, 0, 0, 0);
		gl.glClearDepth(1.0);
		gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		gl.glDisable(GL_SCISSOR_TEST);
		Step(fmt::format("kresleni obrazku {}-{} (glDrawElements)", _gpu_pictures + 1 + at, _gpu_pictures + end));
		std::vector<const Placed *> order;
		for (size_t k = at; k < end; k++) order.push_back(&placed[k]);
		if (_gpu_standing_lean) std::ranges::stable_sort(order, [](const Placed *a, const Placed *b) { return PictureKind(a->draws) < PictureKind(b->draws); });
		DrawPictureBatch(order.size(), [&](size_t k) {
			const Placed *p = order[k];
			return std::tuple<const ThreeDGpuPicture *, const std::vector<MeshOnCard> *, int, int>{p->pic, &p->draws, p->x, p->y};
		});
		/* Read back once, each picture's rows out of it. */
		Step(fmt::format("cteni obrazku {}-{} zpet (glReadPixels), {}x{}, {}", _gpu_pictures + 1 + at, _gpu_pictures + end, frame_w, frame_h, MemoryNow()));
		colours.resize(static_cast<size_t>(frame_w) * frame_h);
		gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
		gl.glReadPixels(0, 0, frame_w, frame_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, colours.data());
		if (read_lit) {
			lit.resize(static_cast<size_t>(frame_w) * frame_h);
			gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
			gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
			gl.glReadPixels(0, 0, frame_w, frame_h, GL_RED, GL_UNSIGNED_BYTE, lit.data());
		}
		gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
		const GLenum error = gl.glGetError();
		if (error != 0) {
			Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x}, the processor draws the pictures", error);
			if (error == GL_OUT_OF_MEMORY) ForgetCard();
			return;
		}
		for (size_t k = at; k < end; k++) {
			ThreeDGpuPicture &pic = *placed[k].pic;
			const int w = pic.width, h = pic.height, px = placed[k].x, py = placed[k].y;
			pic.pixels.resize(static_cast<size_t>(w) * h);
			for (int r = 0; r < h; r++) std::copy_n(colours.begin() + static_cast<size_t>(py + r) * frame_w + px, w, pic.pixels.begin() + static_cast<size_t>(r) * w);
			if (pic.read_lit) {
				pic.lit.resize(static_cast<size_t>(w) * h);
				for (int r = 0; r < h; r++) std::copy_n(lit.begin() + static_cast<size_t>(py + r) * frame_w + px, w, pic.lit.begin() + static_cast<size_t>(r) * w);
			} else {
				pic.lit.clear();
			}
			pic.drawn = true;
		}
		at = end;
	}
	TrimCard();
}

/**
 * Whether the card poses a figure by its bones as MESH_VERTEX has it (ThreeDBonesFrame): a square of one joint drawn
 * as a picture, the joint moving it aside, read back where it went and where it stood. The player's phone refused
 * before what its OpenGL names (TestInstancedArrays()); where the card does not pose it, the processor poses the frames.
 */
static bool TestBones()
{
	ThreeDVehicleShape square;
	square.pos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
	square.uv = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
	square.tri = {0, 1, 2, 0, 2, 3};
	square.tri_tex = {0, 0};
	square.corner_joints.assign(16, 0);
	square.corner_weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
	ThreeDObjectTexture white;
	white.w = white.h = 1;
	white.px = {0xFFFFFFFFu};
	auto bones = std::make_shared<ThreeDBonesFrame>();
	bones->joints = {1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 1, 0}; // two metres along x
	ThreeDGpuPicture pic;
	pic.width = 8;
	pic.height = 4;
	pic.depth_near = -1;
	pic.depth_far = 1;
	ThreeDGpuMesh &m = pic.meshes.emplace_back();
	m.shape = &square;
	m.triangles = ThreeDGpuTrianglesOf(square, 0, false, 1);
	m.matrix = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // two pixels a metre
	m.textures.assign(m.triangles->used.size(), &white);
	m.bones = bones;
	ThreeDGpuPicture *const one[] = {&pic};
	DrawPicturesOnCard(one);
	if (!pic.drawn || pic.pixels.size() != 32) {
		Diary("kosti na karte: obrazek se nenakreslil, snimky postav pozuje procesor", true);
		return false;
	}
	const uint32_t went = pic.pixels[8 + 5], stood = pic.pixels[8 + 1];
	const bool ok = (went >> 24) == 255 && (stood >> 24) == 0;
	const std::string line = fmt::format("kosti na karte: ctverec posunuty {} ({}), na miste {} ({}){}", (went >> 24) == 255 ? "je" : "NENI", went >> 24,
			(stood >> 24) == 0 ? "neni" : "JE", stood >> 24, ok ? "" : "; snimky postav pozuje procesor");
	Diary(line, true);
	Debug(misc, 1, "3D on the graphics card: {}", line);
	return ok;
}

/**
 * A building of the turned map on the card, put there the first time (GpuBuilding, by its shape's number with the top
 * bit set: the view from the eyes keeps the same shape its own way): each corner its own light, as the processor
 * shades across a triangle; its triangles by texture and side.
 */
const GpuBuilding *MapBuildingOnCard(const ThreeDGpuBuilding &b)
{
	const uint64_t key = b.uid | (uint64_t{1} << 63);
	auto it = _card_buildings.find(key);
	if (it != _card_buildings.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	const size_t corners = b.pos.size() / 3;
	if (b.uid == 0 || corners == 0 || b.uv.size() < corners * 2) return nullptr;
	const bool lit = b.light.size() == corners;
	/* Its shape cut into cells, a quarter of a tile across and half a tile high, at most 8 each way: a block of the map
	 * drawn again (a car went by) draws the triangles of the cells it reaches into, not all of the building's
	 * (DrawStandingOnCard(); the player's #311, many cars: 30-70 ms a frame, all of it the buildings' triangles). */
	std::array<float, 3> lo = {FLT_MAX, FLT_MAX, FLT_MAX}, hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (size_t i = 0; i < corners; i++) {
		for (int a = 0; a < 3; a++) {
			lo[a] = std::min(lo[a], b.pos[i * 3 + a]);
			hi[a] = std::max(hi[a], b.pos[i * 3 + a]);
		}
	}
	static constexpr float CELL[3] = {TILE_SIZE / 4.0f, TILE_SIZE / 4.0f, TILE_SIZE / 2.0f};
	std::array<int, 3> n;
	for (int a = 0; a < 3; a++) n[a] = std::clamp(static_cast<int>(std::ceil((hi[a] - lo[a]) / CELL[a])), 1, 8);
	auto cell_of = [&](uint32_t ia, uint32_t ib, uint32_t ic) {
		int c[3];
		for (int a = 0; a < 3; a++) {
			const float mid = (b.pos[ia * 3 + a] + b.pos[ib * 3 + a] + b.pos[ic * 3 + a]) / 3.0f;
			c[a] = hi[a] > lo[a] ? std::clamp(static_cast<int>((mid - lo[a]) / (hi[a] - lo[a]) * n[a]), 0, n[a] - 1) : 0;
		}
		return static_cast<uint16_t>((c[2] * n[1] + c[1]) * n[0] + c[0]);
	};
	struct Tri {
		uint16_t cell;
		uint32_t at; ///< its first corner in tri
	};
	std::map<std::pair<uint8_t, int8_t>, std::vector<Tri>> by;
	for (size_t t = 0; t + 2 < b.tri.size(); t += 3) {
		const uint32_t ia = b.tri[t], ib = b.tri[t + 1], ic = b.tri[t + 2];
		if (std::max({ia, ib, ic}) >= corners) continue;
		const uint8_t texture = t / 3 < b.tri_tex.size() ? b.tri_tex[t / 3] : 0;
		const int8_t facing = t / 3 < b.facing.size() ? b.facing[t / 3] : 0;
		by[{texture, facing}].push_back({cell_of(ia, ib, ic), static_cast<uint32_t>(t)});
	}
	GpuBuilding g;
	g.boxes.assign(static_cast<size_t>(n[0]) * n[1] * n[2], {FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX});
	std::vector<float> all;
	for (auto &[k, tris] : by) {
		std::ranges::stable_sort(tris, {}, &Tri::cell);
		GpuBuilding::Run run{static_cast<GLint>(all.size() / 6), static_cast<GLsizei>(tris.size() * 3), k.first, k.second, {}};
		for (const Tri &tri : tris) {
			if (run.cells.empty() || run.cells.back().id != tri.cell) run.cells.push_back({static_cast<GLint>(all.size() / 6), 0, tri.cell});
			run.cells.back().count += 3;
			std::array<float, 6> &box = g.boxes[tri.cell];
			for (uint32_t i : {b.tri[tri.at], b.tri[tri.at + 1], b.tri[tri.at + 2]}) {
				all.insert(all.end(), {b.pos[i * 3], b.pos[i * 3 + 1], b.pos[i * 3 + 2], b.uv[i * 2], b.uv[i * 2 + 1], lit ? static_cast<float>(b.light[i]) : 256.0f});
				for (int a = 0; a < 3; a++) {
					box[a] = std::min(box[a], b.pos[i * 3 + a]);
					box[a + 3] = std::max(box[a + 3], b.pos[i * 3 + a]);
				}
			}
		}
		g.runs.push_back(std::move(run));
	}
	/* Its lit glass of the night after them, by texture, as bright as it is: drawn through the building's matrix as
	 * its walls are (DrawStandingOnCard()). */
	if (const size_t night_corners = b.night_pos.size() / 3; night_corners > 0 && b.night_uv.size() >= night_corners * 2) {
		std::map<uint8_t, std::vector<uint32_t>> glass;
		for (size_t t = 0; t + 2 < b.night_tri.size(); t += 3) {
			if (std::max({b.night_tri[t], b.night_tri[t + 1], b.night_tri[t + 2]}) >= night_corners) continue;
			glass[t / 3 < b.night_tex.size() ? b.night_tex[t / 3] : 0].push_back(static_cast<uint32_t>(t));
		}
		for (const auto &[texture, tris] : glass) {
			GpuBuilding::Run run{static_cast<GLint>(all.size() / 6), static_cast<GLsizei>(tris.size() * 3), texture, 0, {}};
			for (uint32_t at : tris) {
				for (uint32_t i : {b.night_tri[at], b.night_tri[at + 1], b.night_tri[at + 2]}) {
					all.insert(all.end(), {b.night_pos[i * 3], b.night_pos[i * 3 + 1], b.night_pos[i * 3 + 2], b.night_uv[i * 2], b.night_uv[i * 2 + 1], 256.0f});
				}
			}
			g.night_runs.push_back(std::move(run));
		}
	}
	Step(fmt::format("budova plochy na kartu: {} trojuhelniku v {} kostkach", all.size() / 18, g.boxes.size()));
	gl.glGenVertexArrays(1, &g.vao);
	gl.glBindVertexArray(g.vao);
	gl.glGenBuffers(1, &g.vbo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
	gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(all.size() * sizeof(float)), all.data(), GL_STATIC_DRAW);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(5 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glBindVertexArray(0);
	g.bytes = all.size() * sizeof(float);
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[3] += g.bytes;
	_card_sent_of[3] += g.bytes;
	_card_sent += g.bytes;
	CardHeld(_card_held_buildings, key, true);
	return &_card_buildings.emplace(key, std::move(g)).first->second;
}

/** The block of the ground drawn into, at least as big as asked (grown in steps). */
bool GroundTargetOfSize(int w, int h)
{
	if (w <= _gfbo_w && h <= _gfbo_h) return true;
	_gfbo_w = std::min(_max_target, (std::max(_gfbo_w, w) + 255) / 256 * 256);
	_gfbo_h = std::min(_max_target, (std::max(_gfbo_h, h) + 255) / 256 * 256);
	Step(fmt::format("ramec zeme plochy {}x{} (glRenderbufferStorage, glCheckFramebufferStatus)", _gfbo_w, _gfbo_h));
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _g_colour_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, _gfbo_w, _gfbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _gfbo);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _g_colour_rb);
	if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		_gfbo_w = _gfbo_h = 0;
		return false;
	}
	return w <= _gfbo_w && h <= _gfbo_h;
}

/** Send a rect of a texture's pixels (ThreeDGpuGroundTexture::pixels) to the texture bound, at x, y of it plus an offset. */
void SendGroundRect(const ThreeDGpuGroundTexture &t, int x, int y, int w, int h, int to_x, int to_y)
{
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, t.width);
	gl.glTexSubImage2D(GL_TEXTURE_2D, 0, to_x + x, to_y + y, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t.pixels + static_cast<size_t>(y) * t.width + x);
	gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	_card_sent += static_cast<size_t>(w) * h * 4;
	_card_sent_ground += static_cast<size_t>(w) * h * 4;
}

/** A texture of nearest texels, of a size, none of its pixels sent. */
GLuint NewGroundTexture(int w, int h)
{
	GLuint name = 0;
	gl.glGenTextures(1, &name);
	gl.glBindTexture(GL_TEXTURE_2D, name);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	return name;
}

/**
 * The surface of a road of the ground of the view from the eyes smoothed (DrawEyeRoad() of viewport.cpp, the processor
 * the same where it draws them): each of its texels (THREE_D_GPU_ROAD_SURFACE) the mean of those of it about it, seven
 * by seven, of a near lightness (to 40) -- the dither gone, the edge of a pavement kept; the water's alike, among the
 * water's texels (THREE_D_GPU_WATER_SURFACE, DrawEyeWater()). Each texel of the rect, into a picture of its own, laid
 * back; a texel beside it the rect reads as it is.
 */
const char *const ROAD_SMOOTH_VERTEX = R"(#version 330 core
void main()
{
	gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1), 0.0, 1.0);
}
)";
const char *const ROAD_SMOOTH_FRAGMENT = R"(#version 330 core
uniform sampler2D u_ground;
uniform ivec2 u_at;
layout(location = 0) out vec4 o_colour;
float Light(vec3 c) { return floor((c.r * 77.0 + c.g * 150.0 + c.b * 29.0) / 256.0); }
void main()
{
	ivec2 p = u_at + ivec2(gl_FragCoord.xy);
	ivec2 size = textureSize(u_ground, 0);
	vec4 t = texelFetch(u_ground, p, 0);
	int ta = int(t.a * 255.0 + 0.5);
	if (ta != 253 && ta != 251) {
		o_colour = t;
		return;
	}
	float l = Light(floor(t.rgb * 255.0 + 0.5));
	vec3 sum = vec3(0.0);
	float n = 0.0;
	for (int dy = -3; dy <= 3; dy++) {
		for (int dx = -3; dx <= 3; dx++) {
			ivec2 q = p + ivec2(dx, dy);
			if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
			vec4 s = texelFetch(u_ground, q, 0);
			if (int(s.a * 255.0 + 0.5) != ta) continue;
			vec3 c = floor(s.rgb * 255.0 + 0.5);
			if (abs(Light(c) - l) > 40.0) continue;
			sum += c;
			n += 1.0;
		}
	}
	o_colour = vec4(floor(sum / max(n, 1.0)) / 255.0, t.a);
}
)";

/** The roads' surface of a rect of a texture of the ground smoothed on the card (ROAD_SMOOTH_FRAGMENT). */
void SmoothGroundRoads(const GroundTextureOnCard &g, int x, int y, int w, int h)
{
	if (_road_smooth_state == 0) {
		std::string why;
		if (!MakeProgram(_road_smooth_program, ROAD_SMOOTH_VERTEX, ROAD_SMOOTH_FRAGMENT, why)) {
			Debug(misc, 0, "[3d] karta: hladky asfalt z oci nejde ({}), hladi ho procesor", why);
			_road_smooth_state = -1;
			return;
		}
		gl.glGenVertexArrays(1, &_road_smooth_vao);
		gl.glGenFramebuffers(1, &_road_smooth_fbo);
		_road_smooth_piece = NewGroundTexture(ROAD_SMOOTH_PIECE, ROAD_SMOOTH_PIECE);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		_road_smooth_state = 1;
	}
	if (_road_smooth_state < 0) return;
	x = std::max(0, x);
	y = std::max(0, y);
	w = std::min(w, g.w - x);
	h = std::min(h, g.h - y);
	if (w <= 0 || h <= 0) return;
	static const GLenum colour_only[1] = {GL_COLOR_ATTACHMENT0};
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glDisable(GL_DEPTH_TEST);
	gl.glDisable(GL_BLEND);
	gl.glUseProgram(_road_smooth_program.program);
	gl.glUniform1i(gl.glGetUniformLocation(_road_smooth_program.program, "u_ground"), 0);
	const GLint at = gl.glGetUniformLocation(_road_smooth_program.program, "u_at");
	gl.glBindVertexArray(_road_smooth_vao);
	for (int py = y; py < y + h; py += ROAD_SMOOTH_PIECE) {
		for (int px = x; px < x + w; px += ROAD_SMOOTH_PIECE) {
			const int pw = std::min(ROAD_SMOOTH_PIECE, x + w - px), ph = std::min(ROAD_SMOOTH_PIECE, y + h - py);
			/* Into the piece's picture, the texture read as it is. */
			gl.glBindFramebuffer(GL_FRAMEBUFFER, _road_smooth_fbo);
			gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _road_smooth_piece, 0);
			gl.glDrawBuffers(1, colour_only);
			gl.glViewport(0, 0, pw, ph);
			gl.glActiveTexture(GL_TEXTURE0);
			gl.glBindTexture(GL_TEXTURE_2D, g.name);
			gl.glUniform2i(at, px, py);
			gl.glDrawArrays(GL_TRIANGLES, 0, 3);
			gl.glBindTexture(GL_TEXTURE_2D, 0);
			/* Laid back into the texture. */
			gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, _road_smooth_fbo);
			gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
			gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _g_move_fbo[1]);
			gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.name, 0);
			gl.glDrawBuffers(1, colour_only);
			gl.glBlitFramebuffer(0, 0, pw, ph, px, py, px + pw, py + ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			gl.glBindFramebuffer(GL_FRAMEBUFFER, _road_smooth_fbo);
			gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		}
	}
	gl.glBindVertexArray(0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.glUseProgram(0);
}

/** A big texture of the ground on the card as it is now: sent whole the first time and in another generation, else only its rects drawn again. */
const GroundTextureOnCard *GroundTextureOf(const ThreeDGpuGroundTexture &t)
{
	if (t.width <= 0 || t.height <= 0 || t.width > _max_texture || t.height > _max_texture || t.pixels == nullptr) return nullptr;
	GroundTextureOnCard &g = _ground_textures[t.key];
	if (g.name != 0 && g.generation != t.generation && t.from_generation != 0 && g.generation == t.from_generation) {
		/* Placed anew over the map: what the card has moves to its place in the new one (the rest nothing), the rects
		 * drawn again since sent after. Sent whole, a picture of tens of megabytes went over for every few frames of
		 * turning. */
		Step(fmt::format("zem plochy na karte posunuta {}x{} -> {}x{} o {},{}", g.w, g.h, t.width, t.height, t.from_x, t.from_y));
		const GLuint moved = NewGroundTexture(t.width, t.height);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _g_move_fbo[1]);
		gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, moved, 0);
		static const GLenum colour_only[1] = {GL_COLOR_ATTACHMENT0};
		gl.glDrawBuffers(1, colour_only);
		gl.glDisable(GL_SCISSOR_TEST);
		gl.glClearColor(0, 0, 0, 0);
		gl.glClear(GL_COLOR_BUFFER_BIT);
		gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, _g_move_fbo[0]);
		gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.name, 0);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
		const int x0 = std::max(0, t.from_x), y0 = std::max(0, t.from_y);
		const int x1 = std::min(t.width, t.from_x + g.w), y1 = std::min(t.height, t.from_y + g.h);
		if (x0 < x1 && y0 < y1) gl.glBlitFramebuffer(x0 - t.from_x, y0 - t.from_y, x1 - t.from_x, y1 - t.from_y, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		gl.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		gl.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
		gl.glDeleteTextures(1, &g.name);
		g.name = moved;
		g.w = t.width;
		g.h = t.height;
		g.generation = t.generation;
		gl.glBindTexture(GL_TEXTURE_2D, g.name);
	} else if (g.name != 0 && (g.w != t.width || g.h != t.height)) {
		gl.glDeleteTextures(1, &g.name);
		g.name = 0;
	}
	if (g.name == 0) {
		Step(fmt::format("zem plochy na kartu {}x{}", t.width, t.height));
		g.name = NewGroundTexture(t.width, t.height);
		g.w = t.width;
		g.h = t.height;
		g.generation = t.generation - 1;
	} else {
		gl.glBindTexture(GL_TEXTURE_2D, g.name);
	}
	if (g.generation != t.generation) {
		SendGroundRect(t, 0, 0, t.width, t.height, 0, 0);
		g.generation = t.generation;
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		if (t.smooth_roads) SmoothGroundRoads(g, 0, 0, t.width, t.height);
	} else {
		for (const auto &[x, y, w, h] : t.changed) {
			if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > t.width || y + h > t.height) continue;
			SendGroundRect(t, x, y, w, h, 0, 0);
		}
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		if (t.smooth_roads) {
			for (const auto &[x, y, w, h] : t.changed) {
				if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > t.width || y + h > t.height) continue;
				SmoothGroundRoads(g, x, y, w, h);
			}
		}
	}
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	return &g;
}

/**
 * A small texture of the ground in the atlas: its place, sent there the first time. False when the atlas is full
 * (it is then started over) or the texture is bigger than it.
 */
bool GroundAtlasPlace(const ThreeDGpuGroundTexture &t, std::array<int, 4> &place)
{
	auto it = _ground_atlas_places.find(t.key);
	if (it != _ground_atlas_places.end()) {
		place = it->second;
		return true;
	}
	if (_ground_atlas == 0) {
		_ground_atlas_size = std::min(4096, _max_texture);
		Step(fmt::format("atlas zeme plochy {}x{}", _ground_atlas_size, _ground_atlas_size));
		_ground_atlas = NewGroundTexture(_ground_atlas_size, _ground_atlas_size);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		_ground_shelf_x = _ground_shelf_y = _ground_shelf_h = 0;
	}
	if (t.width <= 0 || t.height <= 0 || t.width > _ground_atlas_size || t.height > _ground_atlas_size || t.pixels == nullptr) return false;
	if (_ground_shelf_x + t.width > _ground_atlas_size) {
		_ground_shelf_y += _ground_shelf_h;
		_ground_shelf_x = 0;
		_ground_shelf_h = 0;
	}
	if (_ground_shelf_y + t.height > _ground_atlas_size) {
		/* Full: started over, the textures of the block sent again as they come. */
		_ground_atlas_places.clear();
		_ground_shelf_x = _ground_shelf_y = _ground_shelf_h = 0;
		return false;
	}
	place = {_ground_shelf_x, _ground_shelf_y, t.width, t.height};
	gl.glBindTexture(GL_TEXTURE_2D, _ground_atlas);
	SendGroundRect(t, 0, 0, t.width, t.height, place[0], place[1]);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	_ground_shelf_x += t.width;
	_ground_shelf_h = std::max(_ground_shelf_h, t.height);
	_ground_atlas_places[t.key] = place;
	return true;
}

/** The picture of the view's ground (ThreeDGpuGround::into_view) of a size: made anew all black when another, what it held gone. */
bool ViewGroundOfSize(ViewPictures &v, int w, int h)
{
	if (w <= 0 || h <= 0 || w > _max_target || h > _max_target) return false;
	if (v.ground != 0 && v.w == w && v.h == h) return true;
	Step(fmt::format("zem pohledu {} na karte {}x{}", &v - _views.data(), w, h));
	if (v.ground_fbo == 0) gl.glGenFramebuffers(1, &v.ground_fbo);
	if (v.ground_tmp_fbo == 0) gl.glGenFramebuffers(1, &v.ground_tmp_fbo);
	if (v.ground != 0) {
		gl.glDeleteTextures(1, &v.ground);
		gl.glDeleteTextures(1, &v.ground_tmp);
	}
	if (v.standing != 0) {
		gl.glDeleteTextures(1, &v.standing);
		gl.glDeleteTextures(1, &v.standing_lit);
	}
	if (v.standing_order != 0) gl.glDeleteTextures(1, &v.standing_order);
	ForgetMovingPictures(v);
	v.ground = NewGroundTexture(w, h);
	v.ground_tmp = NewGroundTexture(w, h);
	v.standing = NewGroundTexture(w, h);
	v.standing_lit = NewGroundTexture(w, h);
	v.standing_order = NewGroundTexture(w, h);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	static const GLenum colour_only[1] = {GL_COLOR_ATTACHMENT0};
	bool complete = true;
	for (auto [fbo, tex] : {std::pair{v.ground_fbo, v.ground}, std::pair{v.ground_tmp_fbo, v.ground_tmp}}) {
		gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		gl.glDrawBuffers(1, colour_only);
		complete &= gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		gl.glDisable(GL_SCISSOR_TEST);
		gl.glClearColor(0, 0, 0, 1);
		gl.glClear(GL_COLOR_BUFFER_BIT);
	}
	/* What stands on it: nothing yet; the depth of a building drawn among it. */
	if (v.standing_fbo == 0) gl.glGenFramebuffers(1, &v.standing_fbo);
	if (v.standing_depth == 0) gl.glGenRenderbuffers(1, &v.standing_depth);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, v.standing_depth);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, v.standing_fbo);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v.standing, 0);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, v.standing_lit, 0);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, v.standing_order, 0);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, v.standing_depth);
	gl.glDrawBuffers(3, STANDING_DRAW_BUFFERS);
	const bool standing_complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	gl.glClearColor(0, 0, 0, 0);
	gl.glClear(GL_COLOR_BUFFER_BIT);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	v.standing_name = standing_complete ? v.standing : 0;
	v.standing_lit_name = standing_complete ? v.standing_lit : 0;
	v.standing_order_name = standing_complete ? v.standing_order : 0;
	v.made++;
	if (!complete) {
		gl.glDeleteTextures(1, &v.ground);
		gl.glDeleteTextures(1, &v.ground_tmp);
		v.ground = v.ground_tmp = 0;
		v.w = v.h = 0;
		v.ground_name = 0;
		return false;
	}
	v.w = w;
	v.h = h;
	v.ground_name = v.ground;
	return true;
}

/** What the picture of the view's ground holds moved by so many pixels (the view moved; its rows down), black where nothing comes. */
void MoveViewGround(ViewPictures &v, int dx, int dy)
{
	const int w = v.w, h = v.h;
	Step(fmt::format("zem pohledu na karte posunuta o {},{}", dx, dy));
	static const GLenum colour_only[1] = {GL_COLOR_ATTACHMENT0};
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, v.ground_fbo);
	gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
	gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, v.ground_tmp_fbo);
	gl.glDrawBuffers(1, colour_only);
	gl.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, v.ground_fbo);
	gl.glDrawBuffers(1, colour_only);
	gl.glClearColor(0, 0, 0, 1);
	gl.glClear(GL_COLOR_BUFFER_BIT);
	gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, v.ground_tmp_fbo);
	gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
	const int x0 = std::max(0, dx), y0 = std::max(0, dy), x1 = std::min(w, w + dx), y1 = std::min(h, h + dy);
	if (x0 < x1 && y0 < y1) gl.glBlitFramebuffer(x0 - dx, y0 - dy, x1 - dx, y1 - dy, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	/* And what stands on it, its colours, which are lit and its order, each through the same picture; nothing where
	 * nothing comes. The moving things over it laid anew over all of it. */
	v.moving_whole = true;
	if (v.standing_name != 0) {
		for (int k = 0; k < 3; k++) {
			const GLenum attachment = STANDING_DRAW_BUFFERS[k];
			const GLenum only[3] = {k == 0 ? GL_COLOR_ATTACHMENT0 : GL_NONE, k == 1 ? GL_COLOR_ATTACHMENT1 : GL_NONE, k == 2 ? GL_COLOR_ATTACHMENT2 : GL_NONE};
			gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, v.standing_fbo);
			gl.glReadBuffer(attachment);
			gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, v.ground_tmp_fbo);
			gl.glDrawBuffers(1, colour_only);
			gl.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, v.standing_fbo);
			gl.glDrawBuffers(3, only);
			gl.glClearColor(0, 0, 0, 0);
			gl.glClear(GL_COLOR_BUFFER_BIT);
			gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, v.ground_tmp_fbo);
			gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
			if (x0 < x1 && y0 < y1) gl.glBlitFramebuffer(x0 - dx, y0 - dy, x1 - dx, y1 - dy, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/**
 * The ground of a block of the turned map laid by the card (ThreeDGpuDrawGround(); GPU point 3, the player 21. 10.:
 * "do 3"): its textures sent where they changed, its triangles drawn in their order, the block read back -- or laid
 * into the picture of the view's ground, shown straight from the card (ThreeDGpuGround::into_view).
 */
bool DrawGroundOnCard(ThreeDGpuGround &g)
{
	ViewPictures &v = ViewPicturesOf(g.view);
	if (g.forget) {
		/* The small view closed (ThreeDGpuForgetView()): its pictures and its kept ground let go of. */
		Step(fmt::format("obrazky pohledu {} z karty pryc", g.view));
		ForgetViewPictures(v);
		auto it = g.forget_key != 0 ? _ground_textures.find(g.forget_key) : _ground_textures.end();
		if (it != _ground_textures.end()) {
			gl.glDeleteTextures(1, &it->second.name);
			_ground_textures.erase(it);
		}
		return true;
	}
	if (g.into_view) {
		if (!ViewGroundOfSize(v, g.view_w, g.view_h)) return false;
		if (g.move_x != 0 || g.move_y != 0) MoveViewGround(v, g.move_x, g.move_y);
		if (g.width <= 0 || g.height <= 0) {
			ViewPicturesLaid();
			const GLenum error = gl.glGetError();
			if (error == GL_OUT_OF_MEMORY) ForgetCard();
			return error == 0;
		}
	}
	if (!_ground_program_ok || g.width <= 0 || g.height <= 0 || g.width > _max_target || g.height > _max_target || (!g.into_view && g.out == nullptr)) return false;
	/* Where each texture is: in its own (the kept picture of the ground; one such) or in the atlas. */
	std::vector<std::array<float, 5>> where(g.textures.size());
	GLuint big = 0;
	for (int round = 0;; round++) {
		bool full = false;
		for (size_t i = 0; i < g.textures.size(); i++) {
			const ThreeDGpuGroundTexture &t = g.textures[i];
			if (!t.atlas) continue;
			std::array<int, 4> place;
			if (!GroundAtlasPlace(t, place)) {
				full = true;
				break;
			}
			where[i] = {static_cast<float>(place[0]), static_cast<float>(place[1]), static_cast<float>(place[2]), static_cast<float>(place[3]), 1.0f};
		}
		if (!full) break;
		if (round > 0) return false;
	}
	for (size_t i = 0; i < g.textures.size(); i++) {
		const ThreeDGpuGroundTexture &t = g.textures[i];
		if (t.atlas) continue;
		if (big != 0) return false;
		const GroundTextureOnCard *on = GroundTextureOf(t);
		if (on == nullptr) return false;
		big = on->name;
		where[i] = {0.0f, 0.0f, static_cast<float>(t.width), static_cast<float>(t.height), 0.0f};
	}
	std::vector<float> corners;
	corners.reserve(g.triangles.size() * 27);
	for (const ThreeDGpuGroundTriangle &tri : g.triangles) {
		if (tri.texture >= where.size()) continue;
		const std::array<float, 5> &w = where[tri.texture];
		for (int k = 0; k < 3; k++) corners.insert(corners.end(), {tri.x[k], tri.y[k], tri.u[k], tri.v[k], w[0], w[1], w[2], w[3], w[4]});
	}
	Step(fmt::format("zem plochy {}x{}{}: {} trojuhelniku", g.width, g.height, g.into_view ? " do pohledu" : "", corners.size() / 27));
	if (!g.into_view && !GroundTargetOfSize(g.width, g.height)) return false;
	static const GLenum colour_only[1] = {GL_COLOR_ATTACHMENT0};
	gl.glBindFramebuffer(GL_FRAMEBUFFER, g.into_view ? v.ground_fbo : _gfbo);
	gl.glDrawBuffers(1, colour_only);
	gl.glDisable(GL_DITHER);
	gl.glDisable(GL_BLEND);
	gl.glDisable(GL_DEPTH_TEST);
	gl.glDisable(GL_CULL_FACE);
	/* Where it is laid: the block's place in the view's picture, nothing outside it touched -- only the parts asked
	 * (ThreeDGpuGround::parts), the rest of it as it was; else all the block. */
	std::vector<std::array<int, 4>> clips;
	if (g.into_view) {
		gl.glViewport(g.view_x, g.view_y, g.width, g.height);
		gl.glEnable(GL_SCISSOR_TEST);
		if (g.parts.empty()) {
			clips.push_back({g.view_x, g.view_y, g.width, g.height});
		} else {
			for (const std::array<int, 4> &p : g.parts) {
				const int x0 = std::max(0, p[0]), y0 = std::max(0, p[1]), x1 = std::min(g.width, p[0] + p[2]), y1 = std::min(g.height, p[1] + p[3]);
				if (x1 > x0 && y1 > y0) clips.push_back({g.view_x + x0, g.view_y + y0, x1 - x0, y1 - y0});
			}
		}
	} else {
		gl.glViewport(0, 0, g.width, g.height);
		gl.glDisable(GL_SCISSOR_TEST);
		clips.push_back({0, 0, g.width, g.height});
	}
	const float clear[4] = {static_cast<float>((g.clear >> 16) & 0xFF) / 255.0f, static_cast<float>((g.clear >> 8) & 0xFF) / 255.0f, static_cast<float>(g.clear & 0xFF) / 255.0f, static_cast<float>(g.clear >> 24) / 255.0f};
	gl.glClearColor(clear[0], clear[1], clear[2], clear[3]);
	if (corners.empty()) {
		for (const std::array<int, 4> &c : clips) {
			if (g.into_view) gl.glScissor(c[0], c[1], c[2], c[3]);
			gl.glClear(GL_COLOR_BUFFER_BIT);
		}
	} else {
		const GLuint p = _ground_program.program;
		gl.glUseProgram(p);
		/* The places of its uniforms looked up once a program, its texture units set once (they stay with it). */
		static GLuint ground_program = 0;
		static GLint u_size = -1, u_step = -1;
		if (ground_program != p) {
			ground_program = p;
			u_size = gl.glGetUniformLocation(p, "u_size");
			u_step = gl.glGetUniformLocation(p, "u_step");
			gl.glUniform1i(gl.glGetUniformLocation(p, "u_ground"), 0);
			gl.glUniform1i(gl.glGetUniformLocation(p, "u_atlas"), 1);
		}
		gl.glUniform2f(u_size, static_cast<float>(g.width), static_cast<float>(g.height));
		gl.glUniform1i(u_step, std::max(1, g.fill_step));
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, _ground_atlas);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, big);
		gl.glBindVertexArray(_ground_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _ground_vbo);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(corners.size() * sizeof(float)), corners.data(), GL_STREAM_DRAW);
		for (const std::array<int, 4> &c : clips) {
			if (g.into_view) gl.glScissor(c[0], c[1], c[2], c[3]);
			gl.glClear(GL_COLOR_BUFFER_BIT);
			gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(corners.size() / 9));
		}
		gl.glBindVertexArray(0);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
	}
	if (g.into_view) {
		/* Shown straight from the card: done before the screen shows it (ViewPicturesLaid()). */
		gl.glDisable(GL_SCISSOR_TEST);
		gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
		ViewPicturesLaid();
		const GLenum error = gl.glGetError();
		if (error != 0) {
			Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x}, the processor lays the ground", error);
			if (error == GL_OUT_OF_MEMORY) ForgetCard();
			return false;
		}
		return true;
	}
	Step(fmt::format("cteni zeme plochy zpet (glReadPixels), {}x{}", g.width, g.height));
	/* The card's rows up are the block's rows down: read straight into the block, its rows as far apart as its pitch. */
	gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.glPixelStorei(GL_PACK_ROW_LENGTH, g.out_pitch);
	gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
	gl.glReadPixels(0, 0, g.width, g.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g.out);
	gl.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	const GLenum error = gl.glGetError();
	if (error != 0) {
		Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x}, the processor lays the ground", error);
		if (error == GL_OUT_OF_MEMORY) ForgetCard();
		return false;
	}
	return true;
}

/**
 * Where a picture of a standing thing is on the card (DrawStandingOnCard()): a sprite's kept in the atlas by its key,
 * sent there the first time; another sent into the stream, the block's alone. A full one is started over, what was
 * drawn from them drawn first (flush).
 * @return false when it is bigger than they are
 */
/**
 * A place of so many pixels among the pictures laid (DrawStandingOnCard()), shelf by shelf: the kept ones (kept) or the
 * block's others. When they are full, those gathered are laid (flush) and they start over -- the kept forgotten.
 */
void StandingPlaceUsed(uint64_t key)
{
	auto it = _standing_slots.find(key);
	if (it == _standing_slots.end() || it->second.used == _card_clock) return;
	it->second.used = _card_clock;
	it->second.when = std::chrono::steady_clock::now();
}

void StandingPlaceKept(uint64_t key, bool vehicle, uint64_t kept_key)
{
	_standing_slots[key] = {_standing_reserved_size[0], _standing_reserved_size[1], _card_clock, std::chrono::steady_clock::now(), kept_key, vehicle};
}

/** The kept pictures' places counted for the diary (_standing_fill_vehicles), about once a second; the most since it asked. */
static void StandingFillCount()
{
	static auto counted = std::chrono::steady_clock::time_point{};
	const auto now = std::chrono::steady_clock::now();
	if (now - counted < std::chrono::seconds(1) || _standing_atlas_size <= 0) return;
	counted = now;
	uint64_t vehicles = 0, pictures = 0, free = 0;
	for (const auto &[key, slot] : _standing_slots) (slot.vehicle ? vehicles : pictures) += static_cast<uint64_t>(slot.w) * slot.h;
	for (const auto &[size, places] : _standing_free) free += static_cast<uint64_t>(size.first) * size.second * places.size();
	const uint64_t all = static_cast<uint64_t>(_standing_atlas_size) * _standing_atlas_size;
	auto most = [](std::atomic<int> &at, int v) { if (v > at.load()) at = v; };
	most(_standing_fill_vehicles, static_cast<int>(vehicles * 1000 / all));
	most(_standing_fill_pictures, static_cast<int>(pictures * 1000 / all));
	most(_standing_fill_free, static_cast<int>(free * 1000 / all));
	most(_standing_fill_places, static_cast<int>(_standing_slots.size()));
}

/** The places of the kept pictures not laid from for STANDING_SLOT_IDLE (nor in this pass) made free, their pictures forgotten; once a pass. */
static void SweepStandingPlaces()
{
	StandingFillCount();
	if (_standing_swept_at == _card_clock) return;
	_standing_swept_at = _card_clock;
	std::lock_guard lock(_standing_kept_mutex);
	const auto now = std::chrono::steady_clock::now();
	for (auto it = _standing_slots.begin(); it != _standing_slots.end();) {
		if (it->second.used == _card_clock || now - it->second.when < STANDING_SLOT_IDLE || (it->second.vehicle && _standing_pinned.contains(it->second.kept_key))) {
			++it;
			continue;
		}
		if (auto at = _standing_places.find(it->first); at != _standing_places.end()) {
			_standing_free[{it->second.w, it->second.h}].push_back(at->second);
			_standing_places.erase(at);
		}
		if (it->second.vehicle) _standing_kept_keys.erase(it->second.kept_key);
		it = _standing_slots.erase(it);
	}
}

/** A free place at least so big among the kept pictures', the smallest that is; false: none. */
static bool TakeFreeStandingPlace(int w, int h, std::array<int, 2> &place)
{
	/* The smallest that is big enough: not the first one wide enough, maybe many times as big. */
	auto best = _standing_free.end();
	for (auto it = _standing_free.lower_bound({w, 0}); it != _standing_free.end(); ++it) {
		if (it->first.second < h || it->second.empty()) continue;
		if (best == _standing_free.end() || static_cast<int64_t>(it->first.first) * it->first.second < static_cast<int64_t>(best->first.first) * best->first.second) best = it;
		if (it->first.first == w && it->first.second == h) break;
		if (!_standing_free_split) break;
	}
	if (best == _standing_free.end()) return false;
	const auto [fw, fh] = best->first;
	place = best->second.back();
	best->second.pop_back();
	if (best->second.empty()) _standing_free.erase(best);
	_standing_places_reused++;
	if (!_standing_free_split || (fw == w && fh == h)) {
		_standing_reserved_size = {fw, fh};
		return true;
	}
	/* Bigger than asked for (a vehicle's place of a near zoom given to one of a far zoom, a sixteenth of it): only as
	 * much taken, the rest free again -- beside it as high as it, and under it as wide as the place was. Taken whole,
	 * the kept pictures filled up with a few small ones in the big places the zooms in had left (the player's #387,
	 * 21. 11.: at night at z4 and z5, after zooming about, places given anew 8 000 times in 5 s, nearly every vehicle
	 * drawn again in every pass). */
	_standing_reserved_size = {w, h};
	if (fw > w) _standing_free[{fw - w, h}].push_back({place[0] + w, place[1]});
	if (fh > h) _standing_free[{fw, fh - h}].push_back({place[0], place[1] + h});
	return true;
}

std::array<int, 2> StandingReserve(bool kept, int w, int h, const std::function<void()> &flush)
{
	StandingShelf &shelf = kept ? _standing_atlas_shelf : _standing_stream_shelf;
	if (kept) {
		/* A kept one's place a multiple of the step each way: one let go of fits another of about its size. */
		w = std::min(_standing_atlas_size, (w + STANDING_SLOT_STEP - 1) / STANDING_SLOT_STEP * STANDING_SLOT_STEP);
		h = std::min(_standing_atlas_size, (h + STANDING_SLOT_STEP - 1) / STANDING_SLOT_STEP * STANDING_SLOT_STEP);
		_standing_reserved_size = {w, h};
		std::array<int, 2> free_place;
		if (TakeFreeStandingPlace(w, h, free_place)) return free_place;
		/* No room on the shelves: the places idle the longest given first, all forgotten only when none is. */
		const bool row_full = shelf.x + w > _standing_atlas_size;
		if ((row_full ? shelf.y + shelf.h + h : shelf.y + h) > _standing_atlas_size) {
			SweepStandingPlaces();
			_standing_reserved_size = {w, h};
			if (TakeFreeStandingPlace(w, h, free_place)) return free_place;
		}
	}
	if (shelf.x + w > _standing_atlas_size) {
		shelf.y += shelf.h;
		shelf.x = 0;
		shelf.h = 0;
	}
	if (shelf.y + h > _standing_atlas_size) {
		flush();
		shelf = {};
		if (kept) {
			ClearStandingPlaces();
			_standing_places_cleared++;
		}
	}
	const std::array<int, 2> place = {shelf.x, shelf.y};
	shelf.x += w;
	shelf.h = std::max(shelf.h, h);
	return place;
}

bool StandingPlace(const ThreeDGpuStandingItem &item, GLuint &texture, std::array<int, 2> &place, const std::function<void()> &flush)
{
	const ThreeDGpuStandingPicture &p = *item.picture;
	if (p.width <= 0 || p.height <= 0 || p.width > _standing_atlas_size || p.height > _standing_atlas_size) return false;
	if (p.pixels.size() < static_cast<size_t>(p.width) * p.height) return false;
	const bool kept = item.key != 0;
	if (kept) {
		auto it = _standing_places.find(item.key);
		if (it != _standing_places.end()) {
			texture = _standing_atlas;
			place = it->second;
			StandingPlaceUsed(item.key);
			return true;
		}
	}
	place = StandingReserve(kept, p.width, p.height, flush);
	texture = kept ? _standing_atlas : _standing_stream;
	gl.glBindTexture(GL_TEXTURE_2D, texture);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl.glTexSubImage2D(GL_TEXTURE_2D, 0, place[0], place[1], p.width, p.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, p.pixels.data());
	_card_sent += static_cast<size_t>(p.width) * p.height * 4;
	_card_sent_standing += static_cast<size_t>(p.width) * p.height * 4;
	if (kept) {
		_standing_places[item.key] = place;
		StandingPlaceKept(item.key, false, 0);
	}
	return true;
}

/** The picture the vehicles among what stands on the turned map are drawn into first, at least as big as asked (ThreeDGpuStandingVehicle). */
bool StandingVehiclesTargetOfSize(int w, int h)
{
	if (w <= 0 || h <= 0 || w > _max_target || h > _max_target) return false;
	if (w <= _sv_w && h <= _sv_h && _sv_colour != 0) return true;
	const int nw = std::min(_max_target, (std::max(_sv_w, w) + 255) / 256 * 256), nh = std::min(_max_target, (std::max(_sv_h, h) + 255) / 256 * 256);
	Step(fmt::format("ramec vozidel plochy {}x{}", nw, nh));
	if (_sv_fbo == 0) gl.glGenFramebuffers(1, &_sv_fbo);
	if (_sv_colour != 0) {
		gl.glDeleteTextures(1, &_sv_colour);
		gl.glDeleteTextures(1, &_sv_lit);
	}
	_sv_colour = NewGroundTexture(nw, nh);
	_sv_lit = NewGroundTexture(nw, nh);
	/* Its depth a texture: how near each pixel of a vehicle is read from it as it is shrunk (STANDING_SHRINK_FRAGMENT). */
	if (_sv_depth == 0) gl.glGenTextures(1, &_sv_depth);
	gl.glBindTexture(GL_TEXTURE_2D, _sv_depth);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, nw, nh, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _sv_fbo);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _sv_colour, 0);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, _sv_lit, 0);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _sv_depth, 0);
	const bool complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (!complete) {
		gl.glDeleteTextures(1, &_sv_colour);
		gl.glDeleteTextures(1, &_sv_lit);
		_sv_colour = _sv_lit = 0;
		_sv_w = _sv_h = 0;
		return false;
	}
	_sv_w = nw;
	_sv_h = nh;
	return true;
}

/**
 * Clear the bound picture within the scissor's rect: by glClear() where the card keeps it to the scissor; else
 * (TestScissoredClear()) by drawing over the rect -- the colours to nothing, the depth to the given one -- as the
 * pictures are drawn, which the scissor holds. What it changed of the card's state is put back.
 * @param mask GL_COLOR_BUFFER_BIT and GL_DEPTH_BUFFER_BIT, as glClear() takes them
 * @param x,y,w,h the scissor's rect, pixels of the picture
 * @param depth the depth cleared to, that of glClearDepth()
 */
static void ClearInScissor(GLbitfield mask, int x, int y, int w, int h, float depth)
{
	if ((_scissored_clear_ok && !_clear_by_drawing) || !_clear_program_ok) {
		gl.glClear(mask);
		return;
	}
	if (w <= 0 || h <= 0) return;
	const bool colour = (mask & GL_COLOR_BUFFER_BIT) != 0, deep = (mask & GL_DEPTH_BUFFER_BIT) != 0;
	GLint program = 0, vao = 0, buffer = 0, viewport[4] = {}, blend = 0, depth_test = 0, depth_func = 0, depth_write = 0;
	gl.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
	gl.glGetIntegerv(GL_VIEWPORT, viewport);
	gl.glGetIntegerv(GL_BLEND, &blend);
	gl.glGetIntegerv(GL_DEPTH_TEST, &depth_test);
	gl.glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
	gl.glGetIntegerv(GL_DEPTH_WRITEMASK, &depth_write);
	gl.glViewport(x, y, w, h);
	gl.glDisable(GL_BLEND);
	if (deep) {
		gl.glEnable(GL_DEPTH_TEST);
		gl.glDepthFunc(GL_ALWAYS);
		gl.glDepthMask(GL_TRUE);
	} else {
		gl.glDisable(GL_DEPTH_TEST);
	}
	if (!colour) gl.glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	const GLuint cp = _clear_program.program;
	gl.glUseProgram(cp);
	gl.glUniform2f(gl.glGetUniformLocation(cp, "u_size"), 1.0f, 1.0f);
	gl.glBindVertexArray(_standing_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
	const float d = std::clamp(depth, 0.0f, 1.0f);
	const float quad[] = {0, 0, d, 0, 0, 1, 0, d, 0, 0, 1, 1, d, 0, 0, 0, 0, d, 0, 0, 1, 1, d, 0, 0, 0, 1, d, 0, 0};
	gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
	gl.glDrawArrays(GL_TRIANGLES, 0, 6);
	if (!colour) gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl.glDepthFunc(static_cast<GLenum>(depth_func));
	gl.glDepthMask(depth_write != 0 ? GL_TRUE : GL_FALSE);
	if (depth_test != 0) {
		gl.glEnable(GL_DEPTH_TEST);
	} else {
		gl.glDisable(GL_DEPTH_TEST);
	}
	if (blend != 0) gl.glEnable(GL_BLEND);
	gl.glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	gl.glBindVertexArray(static_cast<GLuint>(vao));
	gl.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
	gl.glUseProgram(static_cast<GLuint>(program));
}

/** The pictures of the moving things over what stands on a view (ThreeDGpuStanding::moving), of its size: made the first time, all of what stands copied into them then. */
static bool MovingPicturesOfSize(ViewPictures &v)
{
	if (v.moving != 0) return true;
	Step(fmt::format("vrstva aut pohledu {} na karte {}x{}", &v - _views.data(), v.w, v.h));
	v.moving = NewGroundTexture(v.w, v.h);
	v.moving_lit = NewGroundTexture(v.w, v.h);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	if (v.moving_depth == 0) gl.glGenRenderbuffers(1, &v.moving_depth);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, v.moving_depth);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, v.w, v.h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	if (v.moving_fbo == 0) gl.glGenFramebuffers(1, &v.moving_fbo);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, v.moving_fbo);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v.moving, 0);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, v.moving_lit, 0);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, v.moving_depth);
	gl.glDrawBuffers(2, STANDING_DRAW_BUFFERS);
	const bool complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	v.moving_whole = true;
	v.moving_were.clear();
	v.moving_stood_since.clear();
	if (!complete) {
		ForgetMovingPictures(v);
		return false;
	}
	v.moving_name = v.moving;
	v.moving_lit_name = v.moving_lit;
	return true;
}

/** Where what stands on a block is laid (DrawStandingOnCard()), x, y, w, h pixels of the view's picture: the parts asked (ThreeDGpuStanding::parts), else all the block. */
static std::vector<std::array<int, 4>> StandingClips(const ThreeDGpuStanding &s)
{
	std::vector<std::array<int, 4>> clips;
	if (s.parts.empty()) {
		clips.push_back({s.view_x, s.view_y, s.width, s.height});
	} else {
		for (const std::array<int, 4> &p : s.parts) {
			const int x0 = std::max(0, p[0]), y0 = std::max(0, p[1]), x1 = std::min(s.width, p[0] + p[2]), y1 = std::min(s.height, p[1] + p[3]);
			if (x1 > x0 && y1 > y0) clips.push_back({s.view_x + x0, s.view_y + y0, x1 - x0, y1 - y0});
		}
	}
	return clips;
}

/** Whether what reaches r (left, top, right, bottom) reaches into the part c (x, y, w, h), pixels of the view's picture. */
static bool StandingReaches(const std::array<int, 4> &c, const std::array<int, 4> &r)
{
	return r[2] > c[0] && r[0] < c[0] + c[2] && r[3] > c[1] && r[1] < c[1] + c[3];
}

/** Where a thing standing on a block reaches: left, top, right, bottom, pixels of the view's picture; one it cannot tell of all the block. */
static std::array<int, 4> StandingReach(const ThreeDGpuStanding &s, const ThreeDGpuStandingItem &item)
{
	std::array<int, 4> r{s.view_x, s.view_y, s.view_x + s.width, s.view_y + s.height};
	if (item.faces >= 0) {
		if (static_cast<size_t>(item.faces) < s.faces.size()) {
			const std::vector<float> &c = s.faces[item.faces].corners;
			float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
			for (size_t i = 0; i + ThreeDGpuStandingFaces::FLOATS - 1 < c.size(); i += ThreeDGpuStandingFaces::FLOATS) {
				x0 = std::min(x0, c[i]);
				x1 = std::max(x1, c[i]);
				y0 = std::min(y0, c[i + 1]);
				y1 = std::max(y1, c[i + 1]);
			}
			if (x1 >= x0) r = {s.view_x + static_cast<int>(std::floor(x0)) - 1, s.view_y + static_cast<int>(std::floor(y0)) - 1, s.view_x + static_cast<int>(std::ceil(x1)) + 1, s.view_y + static_cast<int>(std::ceil(y1)) + 1};
		}
	} else if (item.vehicle != nullptr) {
		const int times = 1 << std::clamp(item.up, 0, 3);
		r = {s.view_x + item.x, s.view_y + item.y, s.view_x + item.x + item.vehicle->width * times, s.view_y + item.y + item.vehicle->height * times};
	} else if (item.building >= 0) {
		if (static_cast<size_t>(item.building) < s.buildings.size()) {
			/* Its rectangle (and its figures', its lit glass's): what its depth is cleared over. */
			const ThreeDGpuStandingBuilding &b = s.buildings[item.building];
			r = {s.view_x + b.clear_x, s.view_y + b.clear_y, s.view_x + b.clear_x + b.clear_w, s.view_y + b.clear_y + b.clear_h};
		}
	} else if (item.picture != nullptr) {
		r = {s.view_x + item.x, s.view_y + item.y, s.view_x + item.x + item.picture->width, s.view_y + item.y + item.picture->height};
	}
	return r;
}

/** Whether a thing standing on a block is laid at all: it reaches into a part (StandingClips()), and over the moving things' picture it is one of them. */
static bool StandingInSome(const ThreeDGpuStanding &s, const ThreeDGpuStandingItem &item, const std::array<int, 4> &r, std::span<const std::array<int, 4>> clips)
{
	if (s.moving && item.picture == nullptr && item.vehicle == nullptr) return false;
	return std::ranges::any_of(clips, [&](const std::array<int, 4> &c) { return StandingReaches(c, r); });
}

/** A texture, a pattern or a mask the card holds, by its number (ThreeDGpuStanding::held_only); nullptr not held: nothing put on it. */
static const GpuTexture *HeldTextureOnCard(uint64_t uid)
{
	auto it = _card_textures.find(uid);
	if (it == _card_textures.end()) return nullptr;
	it->second.used = _card_clock;
	return &it->second;
}

/** A building of the turned map the card holds, by its shape's number (MapBuildingOnCard()); nullptr not held: nothing put on it. */
static const GpuBuilding *HeldMapBuildingOnCard(uint64_t uid)
{
	auto it = _card_buildings.find(uid | (uint64_t{1} << 63));
	if (uid == 0 || it == _card_buildings.end()) return nullptr;
	it->second.used = _card_clock;
	return &it->second;
}

/**
 * What stands on a block of the turned map laid by the card into the picture of the view's standing things
 * (ThreeDGpuStanding): the block cleared, far first, each pixel of each thing where it is nearer than what is there --
 * a picture's by its box (ThreeDGpuStandingItem::box), a box's faces by their corners (ThreeDGpuStandingFaces), a
 * building's by its triangles (ThreeDGpuStandingBuilding), the
 * figures on it in its depth --, the order of each pixel laid with it. Not
 * all laid (one bigger than the card's textures, a texture it cannot hold, an error), the block left clear: the
 * processor draws what stands on it. Or the moving things of the view laid over what stands (ThreeDGpuStanding::moving).
 */
bool DrawStandingOnCard(ThreeDGpuStanding &s)
{
	ViewPictures &pics = ViewPicturesOf(s.view);
	if (!_standing_program_ok || !ViewGroundOfSize(pics, s.view_w, s.view_h) || pics.standing_name == 0) return false;
	if (s.width <= 0 || s.height <= 0) return false;
	if (s.moving && (!_standing_copy_ok || !MovingPicturesOfSize(pics))) return false;
	if (s.moving && pics.moving_anew.exchange(false)) pics.moving_whole = true;
	const auto moving_started = std::chrono::steady_clock::now();
	/* Laid into: what stands (its colours, which are lit, its order), or the picture of the moving things over it. */
	const GLuint target = s.moving ? pics.moving_fbo : pics.standing_fbo;
	const GLsizei target_buffers = s.moving ? 2 : 3;
	_card_clock++;
	/* The rig ('test3d atlas <px>'): the kept pictures so small, made anew, to fill them with a few vehicles. */
	if (const int rig = _rig_standing_atlas_size; rig > 0 && _standing_atlas != 0 && _standing_atlas_size != std::min(rig, _max_texture)) {
		gl.glDeleteTextures(1, &_standing_atlas);
		gl.glDeleteTextures(1, &_standing_stream);
		if (_standing_near != 0) gl.glDeleteTextures(1, &_standing_near);
		_standing_atlas = _standing_stream = _standing_near = 0;
	_standing_atlas_made++;
	}
	if (_standing_atlas == 0) {
		_standing_atlas_size = std::min(_rig_standing_atlas_size > 0 ? _rig_standing_atlas_size.load() : 4096, _max_texture);
		_standing_atlas_size_now = _standing_atlas_size;
		Step(fmt::format("atlas toho, co stoji na plose, {}x{}", _standing_atlas_size, _standing_atlas_size));
		_standing_atlas = NewGroundTexture(_standing_atlas_size, _standing_atlas_size);
		_standing_atlas_made++;
		_standing_stream = NewGroundTexture(_standing_atlas_size, _standing_atlas_size);
		gl.glGenTextures(1, &_standing_near);
		gl.glBindTexture(GL_TEXTURE_2D, _standing_near);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, _standing_atlas_size, _standing_atlas_size, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
		_standing_near_checked = false;
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		_standing_atlas_shelf = {};
		ClearStandingPlaces();
	}
	_standing_stream_shelf = {};
	/* Where it is laid, pixels of the view's picture: the parts asked (ThreeDGpuStanding::parts), else all the block. */
	const std::vector<std::array<int, 4>> clips = StandingClips(s);
	Step(fmt::format("{} na plose {}x{} v {} kusech: {} obrazku, {} budov", s.moving ? "vrstva aut" : "co stoji", s.width, s.height, clips.size(), s.items.size(), s.buildings.size()));
	static const GLenum colour_and_lit[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	/* The part being laid: everything that reaches into it laid there, cut to it, each part in turn. */
	std::array<int, 4> clip = clips.empty() ? std::array<int, 4>{s.view_x, s.view_y, 0, 0} : clips.front();
	/* Where each thing reaches (StandingReach()), and whether into a part at all: one that does not, not drawn, not
	 * even put on the card. */
	std::vector<std::array<int, 4>> reach(s.items.size());
	std::vector<uint8_t> in_some(s.items.size(), 0);
	for (size_t k = 0; k < s.items.size(); k++) {
		reach[k] = StandingReach(s, s.items[k]);
		in_some[k] = StandingInSome(s, s.items[k], reach[k], clips) ? 1 : 0;
	}
	/* The moving things' picture: what stands copied into it where they were laid the frame before, where what stands
	 * was laid since, and where they are now -- all of it when made anew or the view moved. */
	std::vector<std::array<int, 4>> moved_to;
	std::vector<float> quads;
	uint64_t copied = 0;
	if (s.moving) {
		auto copy = [&](int x0, int y0, int x1, int y1) {
			x0 = std::max(x0, 0);
			y0 = std::max(y0, 0);
			x1 = std::min(x1, pics.w);
			y1 = std::min(y1, pics.h);
			if (x1 <= x0 || y1 <= y0) return;
			const float fx0 = static_cast<float>(x0), fy0 = static_cast<float>(y0), fx1 = static_cast<float>(x1), fy1 = static_cast<float>(y1);
			quads.insert(quads.end(), {fx0, fy0, 0, 0, 0, fx1, fy0, 0, 0, 0, fx1, fy1, 0, 0, 0, fx0, fy0, 0, 0, 0, fx1, fy1, 0, 0, 0, fx0, fy1, 0, 0, 0});
			copied += static_cast<uint64_t>(x1 - x0) * (y1 - y0);
		};
		for (size_t k = 0; k < s.items.size(); k++) {
			if (in_some[k] != 0) moved_to.push_back({reach[k][0], reach[k][1], reach[k][2] - reach[k][0], reach[k][3] - reach[k][1]});
		}
		if (pics.moving_whole) {
			copy(0, 0, pics.w, pics.h);
		} else {
			for (const auto *rects : {&pics.moving_were, &pics.moving_stood_since, &moved_to}) {
				for (const std::array<int, 4> &r : *rects) copy(r[0], r[1], r[0] + r[2], r[1] + r[3]);
			}
		}
	}
	/* The view's picture begun: bound, cleared where what stands is laid (its order with it), and its depth -- one for
	 * all laid on it, each pixel laid where it is nearer than what is there (ThreeDGpuStanding::depth); the moving
	 * things' picture copied into. After the vehicles not kept are drawn (vehicles first, below): drawing them while it
	 * was laid left it for another frame and back, on a card drawing by tiles all of its pixels stored and read again
	 * (the player's diary of 18. 11., #379, the moving layer: "76 aut nakresleno znovu 748 ms" in 5 s, ~10 ms each). */
	auto begin_target = [&]() {
		const auto copy_started = std::chrono::steady_clock::now();
		gl.glBindFramebuffer(GL_FRAMEBUFFER, target);
		gl.glDrawBuffers(target_buffers, STANDING_DRAW_BUFFERS);
		gl.glDisable(GL_DITHER);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glDisable(GL_CULL_FACE);
		gl.glDisable(GL_BLEND);
		gl.glViewport(0, 0, s.view_w, s.view_h);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glClearColor(0, 0, 0, 0);
		/* Its depth cleared to 0 (each pixel laid where it is nearer, GL_GEQUAL): set here, whatever drew before -- the
		 * vehicles drawn first leave 1 of their own frame (draw_vehicles()), and the view's depth cleared to it had
		 * nothing laid on it (the rig: the track built new not drawn, 04cd5b7). */
		gl.glClearDepth(0.0);
		gl.glDepthMask(GL_TRUE);
		if (s.moving) {
			/* The moving things' depth among themselves, this pass's alone: all of it cleared first, before anything is
			 * drawn into the picture (no scissor: a clear of the whole every card keeps to). Laid in their order alone,
			 * far first by the middles of their boxes, a semi-trailer was laid over the cab of its own tractor coming
			 * towards the viewer (always after it, DrawnOverBefore() of three_d_vehicle_object.cpp: its front lies over
			 * the fifth wheel) -- the player, 19. 11.: "kreslí návěs který je vzadu přes auto které je vepředu". */
			gl.glDisable(GL_SCISSOR_TEST);
			gl.glClear(GL_DEPTH_BUFFER_BIT);
			gl.glEnable(GL_SCISSOR_TEST);
		}
		for (const std::array<int, 4> &c : clips) {
			if (s.moving) break;
			gl.glScissor(c[0], c[1], c[2], c[3]);
			ClearInScissor(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, c[0], c[1], c[2], c[3], 0.0f);
		}
		gl.glScissor(clip[0], clip[1], clip[2], clip[3]);
		if (s.moving) {
			if (!quads.empty()) {
				const GLuint cp = _standing_copy_program.program;
				gl.glDisable(GL_SCISSOR_TEST);
				gl.glUseProgram(cp);
				gl.glUniform2f(gl.glGetUniformLocation(cp, "u_size"), static_cast<float>(s.view_w), static_cast<float>(s.view_h));
				gl.glUniform1i(gl.glGetUniformLocation(cp, "u_colour"), 0);
				gl.glUniform1i(gl.glGetUniformLocation(cp, "u_lit"), 1);
				gl.glActiveTexture(GL_TEXTURE1);
				gl.glBindTexture(GL_TEXTURE_2D, pics.standing_lit);
				gl.glActiveTexture(GL_TEXTURE0);
				gl.glBindTexture(GL_TEXTURE_2D, pics.standing);
				gl.glBindVertexArray(_standing_vao);
				gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
				gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(quads.size() * sizeof(float)), quads.data(), GL_STREAM_DRAW);
				gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(quads.size() / 5));
				gl.glActiveTexture(GL_TEXTURE1);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glEnable(GL_SCISSOR_TEST);
			}
			_gpu_moving_copied += copied;
			_gpu_moving_copy_ms = _gpu_moving_copy_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copy_started).count();
		}
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		gl.glDepthFunc(GL_GEQUAL);
		gl.glDepthMask(GL_TRUE);
		gl.glClearDepth(0.0);
		gl.glFrontFace(GL_CCW);
	};
	const GLuint p = _standing_program.program;
	/* How near each pixel is (STANDING_NEAR_GLSL): the view's turn, where its pixels are, the depth of all on the block. */
	auto near_uniforms = [&](GLuint program) {
		gl.glUniform4f(gl.glGetUniformLocation(program, "u_place"), s.place[0], s.place[1], s.place[2], s.place[3]);
		gl.glUniform4f(gl.glGetUniformLocation(program, "u_turn"), s.turn[0], s.turn[1], s.turn[2], s.turn[3]);
		gl.glUniform2f(gl.glGetUniformLocation(program, "u_depth"), s.depth[0], std::max(1e-3f, s.depth[1]));
		gl.glUniform2f(gl.glGetUniformLocation(program, "u_tilt"), s.tilt[0], std::max(1e-3f, s.tilt[1]));
	};
	/* The program's uniforms stay with it when another is used meanwhile (a building's): set once a job, then only the
	 * program, its textures and its corners bound again -- the zoomed out map laid hundreds of buildings a block, each
	 * followed by all of this with its names looked up again (the player, 17. 11.: "zoom out je slabý a cuká"). */
	/* The state buildings one after another leave to each other (_gpu_standing_lean): -1 not known; forgotten when
	 * anything else is drawn (use_pictures(), flush()). */
	int lean_cull = -1;                 ///< 0 off, 1 the front, 2 the back left out
	int lean_depth = -1;                ///< the depth test on (1) or off (0)
	GLuint lean_tex0 = 0;               ///< the texture on unit 0, 0 not known
	GLuint lean_vao = 0;                ///< the corners bound
	bool lean_program = false;          ///< the buildings' program in use
	std::array<GLuint, 2 * THREE_D_GPU_BUILDING_PAINTS> lean_parts{}; ///< the masks and patterns on the units 1 to 8
	/* The buildings' uniforms as last set (a uniform keeps its value with its program whatever is used meanwhile): set
	 * again only when another. -1 / NaN not known. */
	int lean_u_parts = -1;
	std::array<float, 3> lean_u_metres{NAN, NAN, NAN};
	auto lean_forget = [&]() {
		lean_cull = -1;
		lean_depth = -1;
		lean_tex0 = 0;
		lean_vao = 0;
		lean_program = false;
		lean_parts.fill(0);
	};
	bool pictures_uniforms_set = false;
	auto use_pictures = [&]() {
		gl.glUseProgram(p);
		if (pictures_uniforms_set && _gpu_standing_lean) {
			/* As a building before it left the state when it was drawn alone: no depth test, no side left out. */
			if (lean_depth != 0) gl.glDisable(GL_DEPTH_TEST);
			if (lean_cull != 0) gl.glDisable(GL_CULL_FACE);
			/* What is left as known: the units 3 to 8 of the buildings' masks and patterns are not touched by the pictures
			 * (only 0, 1 and 2 are): a building after them binds only what it changed, not all eight again. */
			lean_depth = 0;
			lean_cull = 0;
			lean_tex0 = 0;
			lean_vao = _standing_order_vao;
			lean_program = false;
			lean_parts[0] = 0; // unit 1
			lean_parts[2] = 0; // unit 2
			gl.glActiveTexture(GL_TEXTURE1);
			gl.glBindTexture(GL_TEXTURE_2D, s.moving ? pics.standing_order : 0);
			gl.glActiveTexture(GL_TEXTURE2);
			gl.glBindTexture(GL_TEXTURE_2D, _standing_near);
			gl.glActiveTexture(GL_TEXTURE0);
			gl.glBindVertexArray(_standing_order_vao);
			gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
			return;
		}
		pictures_uniforms_set = true;
		near_uniforms(p);
		gl.glUniform2f(gl.glGetUniformLocation(p, "u_size"), static_cast<float>(s.view_w), static_cast<float>(s.view_h));
		gl.glUniform1i(gl.glGetUniformLocation(p, "u_texture"), 0);
		/* The moving things laid only where they come later than what stands there (its order). */
		gl.glUniform1i(gl.glGetUniformLocation(p, "u_moving"), s.moving ? 1 : 0);
		gl.glUniform1i(gl.glGetUniformLocation(p, "u_orders"), 1);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, s.moving ? pics.standing_order : 0);
		/* How near each pixel of a vehicle is, kept beside the atlas (how 3). */
		gl.glUniform1i(gl.glGetUniformLocation(p, "u_near_tex"), 2);
		gl.glActiveTexture(GL_TEXTURE2);
		gl.glBindTexture(GL_TEXTURE_2D, _standing_near);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindVertexArray(_standing_order_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
	};
	use_pictures();
	/* A building's corner on the block to the card: the block's pixels in the view's picture, its depth 0-1. */
	const float vw = static_cast<float>(s.view_w), vh = static_cast<float>(s.view_h);
	const ThreeDGpuMatrix to_view = {
		2.0f / vw, 0, 0, -1.0f + 2.0f * s.view_x / vw,
		0, 2.0f / vh, 0, -1.0f + 2.0f * s.view_y / vh,
		0, 0, 2.0f, -1.0f,
		0, 0, 0, 1,
	};
	std::vector<float> corners;
	GLuint drawing = 0;
	/* What stands laid where it is nearer than what is there, each pixel by its box (STANDING_NEAR_GLSL); the moving
	 * things by the order kept under them (STANDING_FRAGMENT), and among themselves where nearer too
	 * (ViewPictures::moving_depth). */
	auto flush = [&]() {
		if (corners.empty()) return;
		/* Lean: only what it changes forgotten (the depth test, unit 0); the masks and patterns of the buildings stay. */
		if (!_gpu_standing_lean) lean_forget();
		/* Set whatever was before: a picture put into its atlas meanwhile (StandingPlace()) binds unit 0 too. */
		gl.glEnable(GL_DEPTH_TEST);
		lean_depth = 1;
		gl.glBindTexture(GL_TEXTURE_2D, drawing);
		lean_tex0 = drawing;
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(corners.size() * sizeof(float)), corners.data(), GL_STREAM_DRAW);
		gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(corners.size() / STANDING_BOXED_FLOATS));
		corners.clear();
	};
	/* A picture's rectangle laid at x0, y0 - x1, y1 of the view's picture from u0, v0 - u1, v1 of its texture: its order,
	 * and how near each pixel is -- by its box; in the depth of the building before it as given (a figure on it); or by
	 * its order alone. */
	auto lay_rect = [&](const ThreeDGpuStandingItem &item, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
		const float o = static_cast<float>(item.order);
		float how = 0.0f, z = 0.0f;
		if (item.vehicle != nullptr && item.own_near && item.vehicle->near_span > 0 && _standing_near != 0) {
			how = 3.0f;
		} else if (!s.moving && item.depth >= 0) {
			how = 1.0f;
			z = std::min(1.0f, item.depth);
		} else if (!item.boxed) {
			how = 2.0f;
			z = std::clamp((static_cast<float>(item.order) / 32.0f - 262144.0f - s.depth[0]) / std::max(1e-3f, s.depth[1]), 0.0f, 1.0f);
		} else if (s.moving && item.under_movers) {
			how = 4.0f;
		}
		std::array<float, 8> box = {item.box[0], item.box[1], item.box[2], item.mid, item.box[3], item.box[4], item.box[5], how};
		if (how == 3.0f) box = {item.vehicle->near_min, item.vehicle->near_span, 0.0f, item.hang_near, 0.0f, 0.0f, 0.0f, how};
		auto corner = [&](float x, float y, float u, float v) {
			corners.insert(corners.end(), {x, y, z, u, v, o});
			corners.insert(corners.end(), box.begin(), box.end());
		};
		corner(x0, y0, u0, v0);
		corner(x1, y0, u1, v0);
		corner(x1, y1, u1, v1);
		corner(x0, y0, u0, v0);
		corner(x1, y1, u1, v1);
		corner(x0, y1, u0, v1);
	};
	/* A building: its depth cleared over its rectangle, its triangles by texture and side, at night its lit glass. */
	std::vector<GLuint> textures;
	std::vector<float> night;
	std::vector<uint8_t> cell_in;
	std::vector<GLint> firsts;
	std::vector<GLsizei> counts;
	int building_failed = 0; ///< why the last building could not be laid: 1 its shape, 2 a texture of it
	bool building_uniforms_set = false; ///< the building program's uniforms of the job set (draw_building)
	bool night_was_set = true;          ///< its u_night maybe 1: set to 0 before the next building's triangles
	auto draw_building = [&](const ThreeDGpuStandingBuilding &b, uint32_t order) {
		/* Given without waiting: its shape and textures as the card holds them, by their numbers alone -- nothing of the
		 * game's memory read here (ThreeDGpuStanding::held_only); one let go of meanwhile, not laid. */
		const GpuBuilding *on = s.held_only ? HeldMapBuildingOnCard(b.building.uid) : MapBuildingOnCard(b.building);
		if (on == nullptr) {
			building_failed = 1;
			if (_gpu_standing_failed_told < 40 && !s.held_only) {
				_gpu_standing_failed_told++;
				Diary(fmt::format("budova plochy procesorem: tvar {} rohu, {} uv, cislo {}", b.building.pos.size() / 3, b.building.uv.size() / 2, b.building.uid), true);
			}
			return false;
		}
		textures.clear();
		if (s.held_only) {
			for (uint64_t uid : b.texture_uids) {
				const GpuTexture *g = uid != 0 ? HeldTextureOnCard(uid) : nullptr;
				if (uid != 0 && g == nullptr) {
					building_failed = 2;
					return false;
				}
				textures.push_back(g != nullptr ? g->name : 0);
			}
		} else {
			for (const ThreeDObjectTexture *t : b.building.textures) {
				const GpuTexture *g = t != nullptr ? TextureOnCard(*t) : nullptr;
				if (t != nullptr && g == nullptr) {
					building_failed = 2;
					if (_gpu_standing_failed_told < 40) {
						_gpu_standing_failed_told++;
						Diary(fmt::format("budova plochy procesorem: textura {} z {}, {}x{}, pixelu {}, karta nejvic {}", textures.size(), b.building.textures.size(), t->w, t->h, t->px.size(), _max_texture), true);
					}
					return false;
				}
				textures.push_back(g != nullptr ? g->name : 0);
			}
		}
		/* What paints its first texture (ThreeDGpuBuildingPaint): the masks as they are, the patterns repeating and
		 * smoothed. One that cannot be put on the card: the processor draws the building, its texture painted. */
		std::array<std::array<GLuint, 2>, THREE_D_GPU_BUILDING_PAINTS> parts{};
		const int part_count = static_cast<int>(std::min(b.building.paints.size(), THREE_D_GPU_BUILDING_PAINTS));
		if (s.held_only && b.paint_uids.size() < static_cast<size_t>(part_count)) {
			building_failed = 2;
			return false;
		}
		for (int k = 0; k < part_count; k++) {
			const ThreeDGpuBuildingPaint &bp = b.building.paints[k];
			const GpuTexture *mask, *pattern;
			if (s.held_only) {
				mask = b.paint_uids[k][0] != 0 ? HeldTextureOnCard(b.paint_uids[k][0]) : nullptr;
				pattern = b.paint_uids[k][1] != 0 ? HeldTextureOnCard(b.paint_uids[k][1]) : nullptr;
			} else {
				mask = bp.mask != nullptr ? TextureOnCard(*bp.mask) : nullptr;
				pattern = bp.pattern != nullptr ? PatternOnCard(*bp.pattern) : nullptr;
			}
			if (mask == nullptr || (bp.pattern != nullptr && pattern == nullptr)) {
				building_failed = 2;
				return false;
			}
			parts[k] = {mask->name, pattern != nullptr ? pattern->name : mask->name};
		}
		/* Its depth not cleared: its triangles laid in the depth of all on the block (ThreeDGpuStanding::depth), where
		 * they are nearer than what is there. */
		const ThreeDGpuMatrix m = Multiply(to_view, b.matrix);
		/* Which of its cells reach into the block (GpuBuilding::boxes): their corners through the matrix to the
		 * view's pixels, against the block's; a cell it cannot tell of drawn. */
		cell_in.assign(on->boxes.size(), 1);
		bool all_cells = true;
		for (size_t c = 0; c < on->boxes.size() && _gpu_building_cells_on; c++) {
			const std::array<float, 6> &box = on->boxes[c];
			if (box[0] > box[3]) {
				cell_in[c] = 0;
				continue;
			}
			float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX;
			bool behind = false;
			for (int k = 0; k < 8 && !behind; k++) {
				const float x = box[(k & 1) ? 3 : 0], y = box[(k & 2) ? 4 : 1], z = box[(k & 4) ? 5 : 2];
				const float cw = m[12] * x + m[13] * y + m[14] * z + m[15];
				if (cw <= 0) {
					behind = true;
					break;
				}
				const float px = ((m[0] * x + m[1] * y + m[2] * z + m[3]) / cw + 1.0f) * 0.5f * vw;
				const float py = ((m[4] * x + m[5] * y + m[6] * z + m[7]) / cw + 1.0f) * 0.5f * vh;
				x0 = std::min(x0, px);
				x1 = std::max(x1, px);
				y0 = std::min(y0, py);
				y1 = std::max(y1, py);
			}
			if (!behind && (x1 < clip[0] - 1 || x0 > clip[0] + clip[2] + 1 || y1 < clip[1] - 1 || y0 > clip[1] + clip[3] + 1)) {
				cell_in[c] = 0;
				all_cells = false;
			}
		}
		/* None of it in the part: nothing drawn. */
		const bool night_held = b.night_held && !on->night_runs.empty();
		if (b.night.empty() && !night_held && std::ranges::none_of(cell_in, [](uint8_t in) { return in != 0; })) return true;
		const GpuProgram &bp = _standing_building_program;
		if (!_gpu_standing_lean || !lean_program) gl.glUseProgram(bp.program);
		lean_program = true;
		/* Its paint (STANDING_BUILDING_FRAGMENT): the masks on the units 1 to 4, the patterns on 5 to 8, for its runs
		 * of its first texture. The uniforms' places looked up once a program, not for every building. */
		static GLuint paint_program = 0;
		static GLint u_parts = -1, u_metres = -1, u_turn = -1, night_at = -1, u_order = -1;
		static std::array<GLint, THREE_D_GPU_BUILDING_PAINTS> u_colour, u_about;
		if (paint_program != bp.program) {
			paint_program = bp.program;
			night_at = gl.glGetUniformLocation(bp.program, "u_night");
			u_order = gl.glGetUniformLocation(bp.program, "u_order");
			u_parts = gl.glGetUniformLocation(bp.program, "u_parts");
			u_metres = gl.glGetUniformLocation(bp.program, "u_metres");
			u_turn = gl.glGetUniformLocation(bp.program, "u_turn");
			for (int k = 0; k < static_cast<int>(THREE_D_GPU_BUILDING_PAINTS); k++) {
				gl.glUniform1i(gl.glGetUniformLocation(bp.program, fmt::format("u_mask{}", k).c_str()), 1 + k);
				gl.glUniform1i(gl.glGetUniformLocation(bp.program, fmt::format("u_pattern{}", k).c_str()), 5 + k);
				u_colour[k] = gl.glGetUniformLocation(bp.program, fmt::format("u_colour[{}]", k).c_str());
				u_about[k] = gl.glGetUniformLocation(bp.program, fmt::format("u_about[{}]", k).c_str());
			}
		}
		/* The job's own once (they stay with the program): its texture unit, how near its depth's 0 and 1 are (each
		 * pixel's order by it, STANDING_BUILDING_FRAGMENT), the turn of its patterns. */
		if (!building_uniforms_set || !_gpu_standing_lean) {
			building_uniforms_set = true;
			gl.glUniform1i(bp.tex, 0);
			gl.glUniform2f(bp.depth_range, s.depth[0], s.depth[0] + std::max(1e-3f, s.depth[1]));
			gl.glUniform1f(u_turn, _standing_paint_turn);
		}
		if (night_was_set || !_gpu_standing_lean) {
			gl.glUniform1i(night_at, 0);
			night_was_set = false;
		}
		/* u_order: not read by STANDING_BUILDING_FRAGMENT (its pixels' order is by how near they are), set only when not lean. */
		if (!_gpu_standing_lean) gl.glUniform1f(u_order, static_cast<float>(order));
		gl.glUniformMatrix4fv(bp.matrix, 1, GL_TRUE, m.data());
		const bool lean = _gpu_standing_lean;
		if (!lean) lean_forget();
		bool units_changed = false;
		/* Its parts' colours and patterns: lean, each array at once. */
		if (lean && part_count > 0) {
			std::array<float, 4 * THREE_D_GPU_BUILDING_PAINTS> colours{}, abouts{};
			for (int k = 0; k < part_count; k++) {
				const ThreeDGpuBuildingPaint &p = b.building.paints[k];
				colours[4 * k] = p.colour[0];
				colours[4 * k + 1] = p.colour[1];
				colours[4 * k + 2] = p.colour[2];
				colours[4 * k + 3] = std::max(1.0f, p.start);
				abouts[4 * k] = p.pattern != nullptr ? p.pattern_m : 0.0f;
				abouts[4 * k + 1] = p.only_sloped ? 1.0f : 0.0f;
			}
			gl.glUniform4fv(u_colour[0], part_count, colours.data());
			gl.glUniform4fv(u_about[0], part_count, abouts.data());
		}
		for (int k = 0; k < part_count; k++) {
			const ThreeDGpuBuildingPaint &p = b.building.paints[k];
			if (!lean) {
				gl.glUniform4f(u_colour[k], p.colour[0], p.colour[1], p.colour[2], std::max(1.0f, p.start));
				gl.glUniform4f(u_about[k], p.pattern != nullptr ? p.pattern_m : 0.0f, p.only_sloped ? 1.0f : 0.0f, 0.0f, 0.0f);
			}
			if (!lean || lean_parts[2 * k] != parts[k][0]) {
				gl.glActiveTexture(GL_TEXTURE1 + k);
				gl.glBindTexture(GL_TEXTURE_2D, parts[k][0]);
				lean_parts[2 * k] = parts[k][0];
				units_changed = true;
			}
			if (!lean || lean_parts[2 * k + 1] != parts[k][1]) {
				gl.glActiveTexture(GL_TEXTURE5 + k);
				gl.glBindTexture(GL_TEXTURE_2D, parts[k][1]);
				lean_parts[2 * k + 1] = parts[k][1];
				units_changed = true;
			}
		}
		if (!lean || units_changed) gl.glActiveTexture(GL_TEXTURE0);
		if (!lean || lean_u_metres[0] != b.building.to_metres[0] || lean_u_metres[1] != b.building.to_metres[1] || lean_u_metres[2] != b.building.to_metres[2]) {
			gl.glUniform3f(u_metres, b.building.to_metres[0], b.building.to_metres[1], b.building.to_metres[2]);
			lean_u_metres = {b.building.to_metres[0], b.building.to_metres[1], b.building.to_metres[2]};
		}
		/* Its runs set u_parts as they need it: lean, the value left by the building before kept till a run wants another. */
		if (!lean) {
			gl.glUniform1i(u_parts, 0);
			lean_u_parts = 0;
		}
		int parts_set = lean ? lean_u_parts : 0;
		if (!lean || lean_depth != 1) {
			gl.glEnable(GL_DEPTH_TEST);
			lean_depth = 1;
		}
		if (!lean || lean_vao != on->vao) {
			gl.glBindVertexArray(on->vao);
			lean_vao = on->vao;
		}
		for (const GpuBuilding::Run &r : on->runs) {
			if (r.texture >= textures.size() || textures[r.texture] == 0) continue;
			const int run_parts = r.texture == 0 ? part_count : 0;
			if (run_parts != parts_set) {
				gl.glUniform1i(u_parts, run_parts);
				parts_set = run_parts;
				lean_u_parts = run_parts;
			}
			/* Its ranges in the cells that reach into the block, one after the other joined. */
			firsts.clear();
			counts.clear();
			if (all_cells || r.cells.empty()) {
				firsts.push_back(r.first);
				counts.push_back(r.count);
			} else {
				for (const GpuBuilding::Cell &c : r.cells) {
					if (c.id >= cell_in.size() || cell_in[c.id] == 0) continue;
					if (!firsts.empty() && firsts.back() + counts.back() == c.first) {
						counts.back() += c.count;
					} else {
						firsts.push_back(c.first);
						counts.push_back(c.count);
					}
				}
			}
			_gpu_standing_triangles[1] += static_cast<uint64_t>(r.count / 3);
			if (firsts.empty()) continue;
			/* The side turned away left out, as the processor: a triangle seen from outside goes round clockwise on the
			 * block (rows down), counter-clockwise on the card's (rows up, the same numbers). */
			const int cull = (!b.building.cull || r.facing == 0) ? 0 : r.facing > 0 ? 1 : 2;
			if (!lean || cull != lean_cull) {
				if (cull == 0) {
					gl.glDisable(GL_CULL_FACE);
				} else {
					if (!lean || lean_cull <= 0) gl.glEnable(GL_CULL_FACE);
					gl.glCullFace(cull == 1 ? GL_FRONT : GL_BACK);
				}
				lean_cull = cull;
			}
			if (!lean || lean_tex0 != textures[r.texture]) {
				gl.glBindTexture(GL_TEXTURE_2D, textures[r.texture]);
				lean_tex0 = textures[r.texture];
			}
			if (firsts.size() > 1 && gl.glMultiDrawArrays != nullptr) {
				gl.glMultiDrawArrays(GL_TRIANGLES, firsts.data(), counts.data(), static_cast<GLsizei>(firsts.size()));
			} else {
				for (size_t k = 0; k < firsts.size(); k++) gl.glDrawArrays(GL_TRIANGLES, firsts[k], counts[k]);
			}
			for (GLsizei c : counts) _gpu_standing_triangles[0] += static_cast<uint64_t>(c / 3);
		}
		if (!lean) {
			gl.glDisable(GL_CULL_FACE);
		} else if (lean_cull != 0 && (!b.night.empty() || night_held)) {
			gl.glDisable(GL_CULL_FACE);
			lean_cull = 0;
		}
		if (night_held) {
			/* Its lit glass as the card holds it after its walls' corners, through its matrix, nearer by a hair: lit
			 * (u_night), unpainted, both sides. */
			ThreeDGpuMatrix glass = m;
			glass[11] += 2.0f * b.night_nearer;
			gl.glUniform1i(night_at, 1);
			night_was_set = true;
			gl.glUniform1i(u_parts, 0);
			lean_u_parts = 0;
			gl.glUniformMatrix4fv(bp.matrix, 1, GL_TRUE, glass.data());
			for (const GpuBuilding::Run &r : on->night_runs) {
				if (r.texture >= textures.size() || textures[r.texture] == 0) continue;
				if (!lean || lean_tex0 != textures[r.texture]) {
					gl.glBindTexture(GL_TEXTURE_2D, textures[r.texture]);
					lean_tex0 = textures[r.texture];
				}
				gl.glDrawArrays(GL_TRIANGLES, r.first, r.count);
			}
		}
		if (!b.night.empty()) {
			lean_forget();
			lean_depth = 1;
			gl.glUniform1i(night_at, 1);
			night_was_set = true;
			gl.glUniform1i(u_parts, 0);
			lean_u_parts = 0;
			gl.glUniformMatrix4fv(bp.matrix, 1, GL_TRUE, to_view.data());
			gl.glBindVertexArray(_standing_tri_vao);
			gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_tri_vbo);
			const size_t tris = std::min(b.night.size() / 18, b.night_texture.size());
			for (size_t k = 0; k < textures.size(); k++) {
				if (textures[k] == 0) continue;
				night.clear();
				for (size_t t = 0; t < tris; t++) {
					if (b.night_texture[t] == k) night.insert(night.end(), b.night.begin() + t * 18, b.night.begin() + (t + 1) * 18);
				}
				if (night.empty()) continue;
				gl.glBindTexture(GL_TEXTURE_2D, textures[k]);
				gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(night.size() * sizeof(float)), night.data(), GL_STREAM_DRAW);
				gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(night.size() / 6));
			}
		}
		/* Lean, left as it is for the next building; what draws anything else sets its own (use_pictures(), flush()). */
		if (!lean) gl.glDisable(GL_DEPTH_TEST);
		_gl_buildings++;
		return true;
	};
	/* The vehicles (ThreeDGpuStandingVehicle): drawn into the picture of the card's own first -- as many as it holds at
	 * once, from the one coming on --, each then shrunk from it as it comes. Where each is in it, subpixels; -1 not
	 * drawn yet, -2 not drawn at all (one the card cannot hold). */
	std::vector<std::array<int, 2>> vehicle_at(s.items.size(), {-1, -1});
	/* A vehicle's picture drawn in the frame shrunk into the kept pictures (the atlas) at its place reserved there:
	 * shrink_on() the atlas's frame buffer, the shrinking program and the frame's pictures bound; shrink_into() one;
	 * shrink_off() back to what stands on the view. Alone (shrink_vehicle()) or all those drawn in the frame at once
	 * (draw_vehicles(); the player's diary of 18. 11.: a vehicle shrunk alone switched to the atlas and back, 36 calls). */
	/* The calls of shrinking a batch drawn at once (draw_vehicles()): not of drawing in the diary. */
	uint64_t batch_shrink_calls = 0;
	/* The shrinking program's uniforms' places (shrink_on()). */
	static struct {
		GLint size = -1, colour = -1, lit = -1, depth = -1;
	} shrink_u;
	/* The corners of the vehicles shrunk since shrink_on(), drawn at once by shrink_off(). */
	std::vector<float> shrink_corners;
	auto shrink_fits = [&](const ThreeDGpuStandingVehicle &v) {
		return v.width > 0 && v.height > 0 && v.width <= _standing_atlas_size && v.height <= _standing_atlas_size;
	};
	auto shrink_on = [&]() {
		if (_standing_atlas_fbo == 0) {
			gl.glGenFramebuffers(1, &_standing_atlas_fbo);
			_standing_atlas_made++;
		}
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _standing_atlas_fbo);
		/* The atlas and the nearness beside it attached to its frame buffer: again only when another (an attachment stays
		 * with the frame buffer; the player, 17. 11.: "zoom out je slabý a cuká", a vehicle shrunk took 45 calls). */
		static uint64_t attached_made = UINT64_MAX;
		const bool lean = _gpu_standing_lean;
		const bool attach = !lean || attached_made != _standing_atlas_made;
		if (attach) gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _standing_atlas, 0);
		/* And how near each of its pixels is beside it (ThreeDGpuStandingVehicle::near_span); a card that cannot draw
		 * into both at once lays them by their boxes. */
		if (attach) gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, _standing_near, 0);
		attached_made = _standing_atlas_made;
		if (_standing_near != 0 && !_standing_near_checked) {
			_standing_near_checked = true;
			if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				Diary("hloubka vozidel plochy: ramec atlasu s ni neuplny, vozidla po krabicich", true);
				gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
				gl.glDeleteTextures(1, &_standing_near);
				_standing_near = 0;
				_standing_atlas_made++;
			}
		}
		gl.glDrawBuffers(_standing_near != 0 ? 2 : 1, colour_and_lit);
		gl.glViewport(0, 0, _standing_atlas_size, _standing_atlas_size);
		gl.glDisable(GL_SCISSOR_TEST);
		gl.glDisable(GL_BLEND);
		gl.glDisable(GL_DEPTH_TEST);
		const GLuint sp = _standing_shrink_program.program;
		gl.glUseProgram(sp);
		/* Its uniforms' places looked up once a program, the unchanging ones set once a program and atlas size. */
		static GLuint located = 0;
		static int size_set = -1;
		static uint64_t located_made = UINT64_MAX;
		if (located != sp || located_made != _standing_atlas_made || !lean) {
			located = sp;
			located_made = _standing_atlas_made;
			shrink_u.size = gl.glGetUniformLocation(sp, "u_size");
			shrink_u.colour = gl.glGetUniformLocation(sp, "u_colour");
			shrink_u.lit = gl.glGetUniformLocation(sp, "u_lit");
			shrink_u.depth = gl.glGetUniformLocation(sp, "u_depth");
			gl.glUniform1i(shrink_u.colour, 0);
			gl.glUniform1i(shrink_u.lit, 1);
			gl.glUniform1i(shrink_u.depth, 2);
			size_set = -1;
		}
		if (size_set != _standing_atlas_size) gl.glUniform2f(shrink_u.size, static_cast<float>(_standing_atlas_size), static_cast<float>(_standing_atlas_size));
		size_set = _standing_atlas_size;
		gl.glActiveTexture(GL_TEXTURE2);
		gl.glBindTexture(GL_TEXTURE_2D, _sv_depth);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, _sv_lit);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, _sv_colour);
		gl.glBindVertexArray(_shrink_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_vbo);
		shrink_corners.clear();
	};
	auto shrink_into = [&](const ThreeDGpuStandingVehicle &v, const std::array<int, 2> &at, uint64_t key, const std::array<int, 2> &place) {
		const int ss = std::max(1, v.supersample);
		/* Its row in the picture drawn: subpixels from where it is drawn, its first row's from the point it hangs from;
		 * its supersampling with them. Its corners kept, all drawn at once (shrink_off()). */
		const float nx = v.near_osy, ny = v.near_od, nz = v.near_top - static_cast<float>(at[1]), nw = static_cast<float>(ss);
		const float rx = v.pic.depth_near, ry = v.pic.depth_far, rz = v.near_min, rw = std::max(1e-3f, v.near_span);
		const float w = static_cast<float>(v.width), h = static_cast<float>(v.height);
		const float x0 = static_cast<float>(place[0]), y0 = static_cast<float>(place[1]), x1 = x0 + w, y1 = y0 + h;
		const float u0 = static_cast<float>(at[0] / ss), v0 = static_cast<float>(at[1] / ss), u1 = u0 + w, v1 = v0 + h;
		for (const auto &[x, y, u, vv] : {std::array<float, 4>{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y0, u0, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}}) {
			shrink_corners.insert(shrink_corners.end(), {x, y, 0.0f, u, vv, nx, ny, nz, nw, rx, ry, rz, rw});
		}
		_standing_places[key] = place;
		StandingPlaceKept(key, true, v.key);
		{
			std::lock_guard lock(_standing_kept_mutex);
			_standing_kept_keys.insert(v.key);
		}
		_gpu_standing_vehicles++;
	};
	auto shrink_off = [&]() {
		if (!shrink_corners.empty()) {
			gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(shrink_corners.size() * sizeof(float)), shrink_corners.data(), GL_STREAM_DRAW);
			gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(shrink_corners.size() / SHRINK_FLOATS));
			shrink_corners.clear();
		}
		/* Not bound while the next are drawn into them. */
		gl.glActiveTexture(GL_TEXTURE2);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		/* Back to what stands on the view, as it was. */
		gl.glBindFramebuffer(GL_FRAMEBUFFER, target);
		gl.glDrawBuffers(target_buffers, STANDING_DRAW_BUFFERS);
		gl.glViewport(0, 0, s.view_w, s.view_h);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glScissor(clip[0], clip[1], clip[2], clip[3]);
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	};
	auto shrink_vehicle = [&](const ThreeDGpuStandingVehicle &v, const std::array<int, 2> &at, uint64_t key, std::array<int, 2> &place) {
		if (!shrink_fits(v)) return false;
		place = StandingReserve(true, v.width, v.height, flush);
		shrink_on();
		shrink_into(v, at, key, place);
		shrink_off();
		return true;
	};
	auto draw_vehicles = [&](size_t from, bool back = true) {
		/* Its pictures use the units 1 to 6 (their paint and bones): the buildings' masks and patterns there not known after. */
		lean_forget();
		/* The picture is drawn anew: those drawn into it before are not there any more -- all of them, not only those
		 * after this one. Drawn again too when they come (after this one, which comes again: its kept picture went as
		 * the kept pictures filled up), or they were shrunk from nothing (Mat and the trees of a town went missing with
		 * the figures on the card). One before it comes again in the block's next part (clips): its kept picture
		 * forgotten meanwhile, it was shrunk again from where it had been drawn -- where others were drawn since: a
		 * plane kept as a few trees about it, a lorry as pieces of another, another thing as the plane (the player's
		 * test12, 20. 11.: "on the map the vehicle broken, scattered, and one more shuttle by a building", trees about
		 * the plane and the sailboat; kept so by its key, the same each time the map came back to that zoom). */
		for (size_t k = 0; k < s.items.size(); k++) {
			if (vehicle_at[k][0] >= 0) vehicle_at[k] = {-1, -1};
		}
		struct Placed {
			size_t item;
			std::vector<MeshOnCard> draws;
		};
		std::vector<Placed> placed;
		std::set<uint64_t> keys;
		const int frame_w = std::min(_max_target, THREE_D_GPU_VEHICLE_FRAME);
		int x = 0, y = 0, row = 0;
		for (size_t k = from; k < s.items.size(); k++) {
			if (s.items[k].vehicle == nullptr || vehicle_at[k][0] != -1 || in_some[k] == 0) continue;
			/* Kept already, or drawn in this one before: laid from that. */
			const uint64_t key = s.items[k].vehicle->key | STANDING_VEHICLE_KEY;
			if (_standing_places.contains(key)) continue;
			/* Given without its shapes (as the main thread keeps it): another of the same look given whole draws the
			 * picture, wherever it comes in the order -- one given so before it took the look for itself, the whole one
			 * after it was passed over as "drawn in this one before", and this one was not laid: two lorries of a kind going
			 * the same way, the nearer one missing a frame and the layer laid twice (the player's diary of 19. 11.: "karta
			 * nemela 94, z toho opakovane 47 ruznych"). */
			if (s.items[k].vehicle->kept_only) {
				vehicle_at[k] = {-2, -2};
				continue;
			}
			if (!keys.insert(key).second) continue;
			const ThreeDGpuPicture &pic = s.items[k].vehicle->pic;
			std::vector<MeshOnCard> draws;
			if (pic.width <= 0 || pic.height <= 0 || pic.width > frame_w || pic.height > _max_target || !PictureOnCard(pic, draws)) {
				vehicle_at[k] = {-2, -2};
				continue;
			}
			if (x + pic.width > frame_w) {
				y += row;
				x = 0;
				row = 0;
			}
			if (y + pic.height > _max_target) break;
			vehicle_at[k] = {x, y};
			placed.push_back({k, std::move(draws)});
			x += pic.width;
			row = std::max(row, pic.height);
		}
		if (placed.empty()) return;
		const int frame_h = y + row;
		if (!StandingVehiclesTargetOfSize(frame_w, frame_h)) {
			for (const Placed &p : placed) vehicle_at[p.item] = {-2, -2};
			return;
		}
		Step(fmt::format("vozidla plochy {}: {}x{}", placed.size(), frame_w, frame_h));
		_gl_vehicle_batches++;
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _sv_fbo);
		gl.glDrawBuffers(2, colour_and_lit);
		gl.glDisable(GL_BLEND);
		gl.glScissor(0, 0, frame_w, frame_h);
		gl.glClearColor(0, 0, 0, 0);
		gl.glClearDepth(1.0);
		gl.glDepthMask(GL_TRUE);
		ClearInScissor(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, 0, 0, frame_w, frame_h, 1.0f);
		gl.glDisable(GL_SCISSOR_TEST);
		{
			/* Those of one shape and texture one after another (PictureKind()); the shrinking after keeps the order they
			 * were placed in. */
			std::vector<const Placed *> order;
			order.reserve(placed.size());
			for (const Placed &p : placed) order.push_back(&p);
			if (_gpu_standing_lean) std::ranges::stable_sort(order, [](const Placed *a, const Placed *b) { return PictureKind(a->draws) < PictureKind(b->draws); });
			DrawPictureBatch(order.size(), [&](size_t k) {
				const Placed *p = order[k];
				return std::tuple<const ThreeDGpuPicture *, const std::vector<MeshOnCard> *, int, int>{&s.items[p->item].vehicle->pic, &p->draws, vehicle_at[p->item][0], vehicle_at[p->item][1]};
			});
		}
		/* Each of them shrunk into the kept pictures at once, their places kept: laid from there as each comes (the
		 * main loop finds them kept). One whose place the atlas let go of meanwhile (emptied for a later one,
		 * StandingReserve()) is shrunk again alone as it comes, from the frame drawn here. Rig 'test3d stojici stav 0'
		 * (not lean): each alone as before. */
		if (_gpu_standing_lean && _standing_shrink_ok) {
			const uint64_t shrink_calls = _gl_calls;
			bool on = false;
			for (const Placed &p : placed) {
				const ThreeDGpuStandingVehicle &v = *s.items[p.item].vehicle;
				if (!shrink_fits(v)) continue;
				const std::array<int, 2> place = StandingReserve(true, v.width, v.height, flush);
				if (!on) shrink_on();
				on = true;
				shrink_into(v, vehicle_at[p.item], v.key | STANDING_VEHICLE_KEY, place);
				_gl_vehicles++;
			}
			if (on) shrink_off();
			if (const uint64_t after = _gl_calls; after >= shrink_calls) batch_shrink_calls += after - shrink_calls;
		}
		/* Back to what stands on the view, as it was -- unless it is begun after (begin_target()). */
		if (!back) return;
		gl.glBindFramebuffer(GL_FRAMEBUFFER, target);
		gl.glDrawBuffers(target_buffers, STANDING_DRAW_BUFFERS);
		gl.glViewport(0, 0, s.view_w, s.view_h);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glScissor(clip[0], clip[1], clip[2], clip[3]);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glDisable(GL_CULL_FACE);
		gl.glFrontFace(GL_CCW);
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		gl.glDepthFunc(GL_GEQUAL);
		gl.glDepthMask(GL_TRUE);
		gl.glClearDepth(0.0);
	};
	/* A vehicle shrunk from that picture into the kept pictures (STANDING_SHRINK_FRAGMENT), kept there by its key. */
	/* A box: its texture among the pictures (kept by its key), its faces' triangles over what is there. */
	std::vector<float> face_corners;
	auto draw_faces = [&](const ThreeDGpuStandingFaces &f, const ThreeDGpuStandingItem &item) {
		if (f.texture == nullptr || f.corners.empty()) return true;
		const uint32_t order = item.order;
		ThreeDGpuStandingItem as_picture;
		as_picture.picture = f.texture;
		as_picture.key = f.key != 0 ? ((f.key & (STANDING_FACES_KEY - 1)) | STANDING_FACES_KEY) : 0;
		GLuint texture = 0;
		std::array<int, 2> place;
		if (!StandingPlace(as_picture, texture, place, flush)) return false;
		const GLuint fp = _standing_faces_program.program;
		gl.glUseProgram(fp);
		gl.glUniform2f(gl.glGetUniformLocation(fp, "u_size"), static_cast<float>(s.view_w), static_cast<float>(s.view_h));
		gl.glUniform1i(gl.glGetUniformLocation(fp, "u_texture"), 0);
		gl.glUniform4f(gl.glGetUniformLocation(fp, "u_bounds"), static_cast<float>(place[0]), static_cast<float>(place[1]), static_cast<float>(f.texture->width), static_cast<float>(f.texture->height));
		gl.glUniform1i(gl.glGetUniformLocation(fp, "u_fill"), f.fill ? 1 : 0);
		gl.glUniform1f(gl.glGetUniformLocation(fp, "u_order"), static_cast<float>(order));
		/* Each pixel where it is nearer than what is there, by how near its corners are (STANDING_FACES_FRAGMENT). */
		gl.glUniform2f(gl.glGetUniformLocation(fp, "u_depth"), s.depth[0], std::max(1e-3f, s.depth[1]));
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, texture);
		gl.glEnable(GL_DEPTH_TEST);
		constexpr size_t N = ThreeDGpuStandingFaces::FLOATS;
		face_corners.resize(f.corners.size());
		for (size_t i = 0; i + N - 1 < f.corners.size(); i += N) {
			face_corners[i] = f.corners[i] + s.view_x;
			face_corners[i + 1] = f.corners[i + 1] + s.view_y;
			face_corners[i + 2] = f.corners[i + 2];
			face_corners[i + 3] = f.corners[i + 3];
			face_corners[i + 4] = f.corners[i + 4];
		}
		gl.glBindVertexArray(_standing_faces_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _standing_faces_vbo);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(face_corners.size() * sizeof(float)), face_corners.data(), GL_STREAM_DRAW);
		gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(face_corners.size() / N));
		return true;
	};
	/* The vehicles not kept drawn and shrunk into the kept pictures first, all at once, before the view's picture is
	 * begun (begin_target()); those whose place is kept marked used first, so that no place of theirs is given to the new
	 * ones (SweepStandingPlaces()). One the frame or the atlas has no room for is drawn as it comes, as before. The rig
	 * ('test3d stojici auta_predem 0'): each drawn as it comes. */
	if (_standing_vehicles_first && _gpu_standing_lean && _standing_shrink_ok && !s.held_only) {
		for (size_t k = 0; k < s.items.size(); k++) {
			if (s.items[k].vehicle == nullptr || in_some[k] == 0) continue;
			const uint64_t key = s.items[k].vehicle->key | STANDING_VEHICLE_KEY;
			if (_standing_places.contains(key)) StandingPlaceUsed(key);
		}
		const auto first_started = std::chrono::steady_clock::now();
		const uint64_t vehicle_calls = _gl_calls;
		_gl_counting_vehicles = true;
		batch_shrink_calls = 0;
		draw_vehicles(0, false);
		if (const uint64_t after = _gl_calls; after >= vehicle_calls + batch_shrink_calls) _gl_calls_vehicles_draw += after - vehicle_calls - batch_shrink_calls;
		if (const uint64_t after = _gl_calls; after >= vehicle_calls) _gl_calls_vehicles += after - vehicle_calls;
		_gl_counting_vehicles = false;
		if (s.moving) {
			const auto drawn_first = std::ranges::count_if(vehicle_at, [](const std::array<int, 2> &at) { return at[0] >= 0; });
			if (drawn_first > 0) {
				_gpu_moving_first += static_cast<uint64_t>(drawn_first);
				_gpu_moving_first_ms = _gpu_moving_first_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - first_started).count();
			}
		}
	}
	StandingFillCount();
	begin_target();
	use_pictures();
	drawing = 0;
	bool all = true;
	/* The rig ('test3d stojici odmitnout <n>'): so many given without waiting not laid all the same, as an error of the
	 * card's own leaves them -- the first part laid, then all of it cleared. */
	if (s.held_only && _rig_standing_refuse > 0) {
		_rig_standing_refuse--;
		all = false;
	}
	bool pictures_owed = false; ///< a building drawn last: the pictures' program owed before anything else (use_pictures())
	for (const std::array<int, 4> &part : clips) {
		clip = part;
		gl.glScissor(clip[0], clip[1], clip[2], clip[3]);
		for (size_t k = 0; k < s.items.size(); k++) {
			const ThreeDGpuStandingItem &item = s.items[k];
			if (in_some[k] == 0 || !StandingReaches(clip, reach[k])) continue;
			if (pictures_owed && item.building < 0) {
				use_pictures();
				pictures_owed = false;
			}
			if (item.faces >= 0) {
				flush();
				if (!_standing_faces_ok || static_cast<size_t>(item.faces) >= s.faces.size() || !draw_faces(s.faces[item.faces], item)) {
					_gpu_standing_failed[0]++;
					if (_gpu_standing_failed_told < 40) {
						_gpu_standing_failed_told++;
						const ThreeDGpuStandingFaces *f = static_cast<size_t>(item.faces) < s.faces.size() ? &s.faces[item.faces] : nullptr;
						Diary(fmt::format("krabice plochy procesorem: program {}, obrazek {}x{}, atlas {}", _standing_faces_ok ? "ano" : "ne",
								f != nullptr && f->texture != nullptr ? f->texture->width : 0, f != nullptr && f->texture != nullptr ? f->texture->height : 0, _standing_atlas_size), true);
					}
					all = false;
					break;
				}
				use_pictures();
				drawing = 0;
				continue;
			}
			if (item.vehicle != nullptr) {
				if (!_standing_shrink_ok) continue;
				/* Laid as a picture kept by its key: drawn and shrunk into them first, unless it is there. */
				const ThreeDGpuStandingVehicle &v = *item.vehicle;
				const uint64_t key = v.key | STANDING_VEHICLE_KEY;
				std::array<int, 2> place;
				auto kept = _standing_places.find(key);
				if (kept == _standing_places.end() && v.kept_only && !s.held_only) {
					/* Given without its shapes and the card has no picture of it -- but one of the same look given whole
					 * comes after it in this job (two lorries of a kind going the same way: the first asked for is given
					 * whole, and it may be laid after this one): the pictures of the vehicles from here on drawn now, as
					 * when the whole one is come to, and this one laid from that. Else it was told lacked, and all the
					 * layer gathered and laid once more for it (every frame while the map turns: each look new). */
					bool whole_after = false;
					for (size_t j = k + 1; j < s.items.size() && !whole_after; j++) {
						const auto &other = s.items[j].vehicle;
						whole_after = other != nullptr && !other->kept_only && other->key == v.key && in_some[j] != 0;
					}
					if (whole_after) {
						flush();
						const uint64_t vehicle_calls = _gl_calls;
						_gl_counting_vehicles = true;
						batch_shrink_calls = 0;
						draw_vehicles(k);
						if (const uint64_t after = _gl_calls; after >= vehicle_calls + batch_shrink_calls) _gl_calls_vehicles_draw += after - vehicle_calls - batch_shrink_calls;
						if (const uint64_t after = _gl_calls; after >= vehicle_calls) _gl_calls_vehicles += after - vehicle_calls;
						_gl_counting_vehicles = false;
						use_pictures();
						drawing = 0;
						kept = _standing_places.find(key);
					}
				}
				if (kept != _standing_places.end()) {
					place = kept->second;
					StandingPlaceUsed(key);
					_gpu_standing_vehicles_kept++;
				} else if (s.held_only) {
					/* Given without waiting as the card kept its picture, and it keeps it no more (all the kept
					 * pictures forgotten meanwhile): not drawn from its shapes here -- the block not laid, the asker
					 * learns it (ThreeDGpuStandingSettle()). */
					_standing_async_missed++;
					all = false;
					break;
				} else if (v.kept_only) {
					/* Given without its shapes, as the main thread keeps it, and the card has no picture of it (never
					 * laid: it was beside the view when it was given whole; or let go of since): told which, the asker
					 * gives that one whole (TurnedCardLayMovers() of viewport.cpp) -- unless its picture could not be
					 * made when it was. */
					if (!_standing_failed_keys.contains(v.key)) {
						s.missing++;
						s.missing_keys.push_back(v.key);
						/* Wholly within the block, not one coming over its edge (ThreeDGpuStanding::missing_inside). */
						const std::array<int, 4> &r = reach[k];
						if (r[0] >= s.view_x && r[1] >= s.view_y && r[2] <= s.view_x + s.width && r[3] <= s.view_y + s.height) s.missing_inside++;
					}
					continue;
				} else {
					flush();
					const auto draw_started = std::chrono::steady_clock::now();
					const uint64_t vehicle_calls = _gl_calls;
					_gl_counting_vehicles = true;
					batch_shrink_calls = 0;
					if (vehicle_at[k][0] == -1) draw_vehicles(k);
					if (const uint64_t after = _gl_calls; after >= vehicle_calls + batch_shrink_calls) _gl_calls_vehicles_draw += after - vehicle_calls - batch_shrink_calls;
					/* Shrunk with those drawn at once (draw_vehicles()), or alone now. */
					bool shrunk;
					if (auto now = _standing_places.find(key); now != _standing_places.end()) {
						place = now->second;
						StandingPlaceUsed(key);
						shrunk = true;
					} else {
						shrunk = vehicle_at[k][0] >= 0 && shrink_vehicle(v, vehicle_at[k], key, place);
						_gl_vehicles++;
					}
					if (const uint64_t after = _gl_calls; after >= vehicle_calls) _gl_calls_vehicles += after - vehicle_calls;
					_gl_counting_vehicles = false;
					if (!shrunk) {
						{
							/* (Read by the main thread too, ThreeDGpuStandingKeeps().) */
							std::lock_guard failed_lock(_standing_kept_mutex);
							if (_standing_failed_keys.size() > 4096) _standing_failed_keys.clear();
							_standing_failed_keys.insert(v.key);
						}
					}
					if (s.moving) {
						_gpu_moving_drawn++;
						_gpu_moving_draw_ms = _gpu_moving_draw_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - draw_started).count();
					}
					use_pictures();
					drawing = 0;
					if (!shrunk) continue;
				}
				/* Laid as a picture is, each pixel where it is nearer. */
				if (_standing_atlas != drawing) {
					flush();
					drawing = _standing_atlas;
				}
				const float w = static_cast<float>(v.width), h = static_cast<float>(v.height);
				const float times = static_cast<float>(1 << std::clamp(item.up, 0, 3));
				const float x0 = static_cast<float>(s.view_x + item.x), y0 = static_cast<float>(s.view_y + item.y), x1 = x0 + w * times, y1 = y0 + h * times;
				const float u0 = static_cast<float>(place[0]), v0 = static_cast<float>(place[1]), u1 = u0 + w, v1 = v0 + h;
				lay_rect(item, x0, y0, x1, y1, u0, v0, u1, v1);
				if (s.moving) _gpu_moving_things++;
				continue;
			}
			if (item.building >= 0) {
				flush();
				const uint64_t calls_before = _gl_calls;
				const bool building_drawn = static_cast<size_t>(item.building) < s.buildings.size() && draw_building(s.buildings[item.building], item.order);
				if (const uint64_t calls_after = _gl_calls; calls_after >= calls_before) _gl_calls_buildings += calls_after - calls_before;
				if (!building_drawn) {
					_gpu_standing_failed[building_failed == 2 ? 2 : 1]++;
					all = false;
					break;
				}
				/* The pictures' program again only when something else comes: buildings one after another stay with
				 * theirs. */
				if (_gpu_standing_lean) {
					pictures_owed = true;
				} else {
					use_pictures();
				}
				drawing = 0;
				continue;
			}
			if (item.picture == nullptr) continue;
			GLuint texture = 0;
			std::array<int, 2> place;
			if (!StandingPlace(item, texture, place, flush)) {
				_gpu_standing_failed[3]++;
				if (_gpu_standing_failed_told < 40) {
					_gpu_standing_failed_told++;
					Diary(fmt::format("obrazek plochy procesorem: {}x{}, pixelu {}, atlas {}", item.picture->width, item.picture->height, item.picture->pixels.size(), _standing_atlas_size), true);
				}
				/* A moving thing the card cannot lay is left out, the others laid. */
				if (s.moving) continue;
				all = false;
				break;
			}
			if (texture != drawing) {
				flush();
				drawing = texture;
			}
			const float w = static_cast<float>(item.picture->width), h = static_cast<float>(item.picture->height);
			const float x0 = static_cast<float>(s.view_x + item.x), y0 = static_cast<float>(s.view_y + item.y), x1 = x0 + w, y1 = y0 + h;
			const float u0 = static_cast<float>(place[0]), v0 = static_cast<float>(place[1]), u1 = u0 + w, v1 = v0 + h;
			lay_rect(item, x0, y0, x1, y1, u0, v0, u1, v1);
			if (s.moving) _gpu_moving_things++;
		}
		flush();
		if (!all) break;
	}
	flush();
	gl.glBindVertexArray(0);
	gl.glActiveTexture(GL_TEXTURE1);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glDisable(GL_BLEND);
	gl.glDisable(GL_DEPTH_TEST);
	if (!all) {
		for (const std::array<int, 4> &c : clips) {
			gl.glScissor(c[0], c[1], c[2], c[3]);
			ClearInScissor(GL_COLOR_BUFFER_BIT, c[0], c[1], c[2], c[3], 0.0f);
		}
	}
	gl.glDisable(GL_SCISSOR_TEST);
	if (s.moving) {
		/* Where they are now, copied over the next time; what stands laid since, all in it now. */
		pics.moving_were = std::move(moved_to);
		pics.moving_stood_since.clear();
		pics.moving_whole = false;
		_gpu_moving_passes++;
		_gpu_moving_ms = _gpu_moving_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - moving_started).count();
	} else if (pics.moving != 0) {
		/* What stands laid here: copied under the moving things the next time they are laid; too many, all of it. */
		for (const std::array<int, 4> &c : clips) pics.moving_stood_since.push_back(c);
		if (pics.moving_stood_since.size() > 512) {
			pics.moving_stood_since.clear();
			pics.moving_whole = true;
		}
	}
	/* At night which of its pixels are lit, for the glows by them. */
	if (s.read_lit && all) {
		Step(fmt::format("svitici body plochy zpet (glReadPixels), {}x{}", s.width, s.height));
		s.lit.resize(static_cast<size_t>(s.width) * s.height);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
		gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
		gl.glReadPixels(s.view_x, s.view_y, s.width, s.height, GL_RED, GL_UNSIGNED_BYTE, s.lit.data());
		gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	/* Shown straight from the card: done before the screen shows it (ViewPicturesLaid()). */
	ViewPicturesLaid();
	const GLenum error = gl.glGetError();
	if (error != 0) {
		Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x}, the processor draws what stands on the turned map", error);
		if (error == GL_OUT_OF_MEMORY) ForgetCard();
		return false;
	}
	TrimCard();
	return all;
}

/** A building of the view from the eyes on the card: put there the first time (GpuBuilding). */
const GpuBuilding *BuildingOnCard(const ThreeDGpuBuilding &b)
{
	auto it = _card_buildings.find(b.uid);
	if (it != _card_buildings.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	const size_t corners = b.pos.size() / 3;
	if (b.uid == 0 || corners == 0 || b.uv.size() < corners * 2) return nullptr;
	const bool lit = b.light.size() == corners;
	/* Its triangles by texture and side, in that order. */
	std::map<std::pair<uint8_t, int8_t>, std::vector<float>> by;
	for (size_t t = 0; t + 2 < b.tri.size(); t += 3) {
		const uint32_t ia = b.tri[t], ib = b.tri[t + 1], ic = b.tri[t + 2];
		if (std::max({ia, ib, ic}) >= corners) continue;
		const uint8_t texture = t / 3 < b.tri_tex.size() ? b.tri_tex[t / 3] : 0;
		const int8_t facing = t / 3 < b.facing.size() ? b.facing[t / 3] : 0;
		const float light = lit ? static_cast<float>((b.light[ia] + b.light[ib] + b.light[ic]) / 3) : 256.0f;
		std::vector<float> &v = by[{texture, facing}];
		for (uint32_t i : {ia, ib, ic}) v.insert(v.end(), {b.pos[i * 3], b.pos[i * 3 + 1], b.pos[i * 3 + 2], b.uv[i * 2], b.uv[i * 2 + 1], light});
	}
	GpuBuilding g;
	std::vector<float> all;
	for (const auto &[key, v] : by) {
		g.runs.push_back({static_cast<GLint>(all.size() / 6), static_cast<GLsizei>(v.size() / 6), key.first, key.second, {}});
		all.insert(all.end(), v.begin(), v.end());
	}
	Step(fmt::format("budova na kartu: {} trojuhelniku", all.size() / 18));
	gl.glGenVertexArrays(1, &g.vao);
	gl.glBindVertexArray(g.vao);
	gl.glGenBuffers(1, &g.vbo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
	gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(all.size() * sizeof(float)), all.data(), GL_STATIC_DRAW);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void *>(5 * sizeof(float)));
	gl.glEnableVertexAttribArray(2);
	gl.glBindVertexArray(0);
	g.bytes = all.size() * sizeof(float);
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[3] += g.bytes;
	_card_sent_of[3] += g.bytes;
	_card_sent += g.bytes;
	CardHeld(_card_held_buildings, b.uid, true);
	return &_card_buildings.emplace(b.uid, std::move(g)).first->second;
}

/** The view from the eyes drawn into, at least as big as asked (grown in steps). */
bool EyeTargetOfSize(int w, int h)
{
	if (w <= _eye_w && h <= _eye_h) return true;
	_eye_w = std::min(_max_target, (std::max(_eye_w, w) + 255) / 256 * 256);
	_eye_h = std::min(_max_target, (std::max(_eye_h, h) + 255) / 256 * 256);
	Step(fmt::format("ramec pohledu z oci {}x{} (glRenderbufferStorage, glCheckFramebufferStatus)", _eye_w, _eye_h));
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _eye_colour_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, _eye_w, _eye_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _eye_glow_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, _eye_w, _eye_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _eye_depth_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, _eye_w, _eye_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _eye_fbo);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _eye_colour_rb);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, _eye_glow_rb);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _eye_depth_rb);
	if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		_eye_w = _eye_h = 0;
		return false;
	}
	return w <= _eye_w && h <= _eye_h;
}

/** The objects of the view from the eyes on the card, ready to be drawn (PrepareEyeObjects()). */
struct EyeObjectDraws {
	struct Draw {
		const ThreeDGpuEyeBuilding *eye;
		const GpuBuilding *on;
		std::vector<GLuint> textures;
		/** Its first texture's parts painted as it is drawn (ThreeDGpuBuildingPaint): the mask and the pattern on the card, 0 none. */
		struct Part {
			GLuint mask = 0, pattern = 0;
		};
		std::vector<Part> parts = {};
	};
	struct MeshDraw {
		const ThreeDGpuMesh *mesh;
		const GpuShape *shape;
		const GpuIndices *indices;
		std::vector<GLuint> textures;
		std::vector<PaintOnCard> paints;
	};
	/**
	 * Meshes alike -- the same shape and triangles, textures, paint, light and side out -- drawn at once by instances
	 * (DrawEyeMeshes(); the player, 2. 11.: "try it again and properly"): the distant cars of a kind, the crew in them,
	 * a crowd of a figure, each with its matrix and light in the instances' buffer; the card draws the shape's
	 * triangles once for all of them (each drawn on its own, a frame took a third longer, 86).
	 */
	struct Batch {
		const ThreeDGpuMesh *mesh;   ///< the first of them: their shape's glow and alpha, their paint
		const GpuShape *shape;
		const GpuIndices *indices;
		std::vector<GLuint> textures;
		std::vector<PaintOnCard> paints;
		float mirrored = 1;          ///< the sign of their matrices' determinant (CullVehicleRun()); 0 none
		std::vector<float> instances; ///< each: its matrix's 16 (its rows), its light's 3, whether lit
	};
	std::vector<Draw> draws;
	std::vector<MeshDraw> mesh_draws;
	std::vector<Batch> batches;
	float glass_alpha = 0.15f;
};

void DrawEyeMeshes(const EyeObjectDraws &o, float near_plane, bool glass);

/** Whether two paints paint alike (EyeObjectDraws::Batch): the same masks, colours and times. */
static bool SamePaint(const ThreeDGpuPaint &a, const ThreeDGpuPaint &b)
{
	return a.detail == b.detail && a.detail_colours == b.detail_colours && a.body == b.body && a.paint_mask == b.paint_mask &&
			a.coloured == b.coloured && a.colour == b.colour && a.target == b.target && a.start == b.start && a.fade == b.fade &&
			a.addon_mask == b.addon_mask && a.addon_colour == b.addon_colour && a.addon_start == b.addon_start &&
			a.tinted == b.tinted && a.tint == b.tint && a.rust_mask == b.rust_mask && a.rust_colour == b.rust_colour &&
			a.rust_time == b.rust_time && a.rust_ramp == b.rust_ramp && a.rust_gradual == b.rust_gradual &&
			a.rust_pieces == b.rust_pieces && a.rust_variant == b.rust_variant;
}

/** What the objects of the view from the eyes need on the card put there: false, something it cannot hold. */
/**
 * How much new a picture from the eyes may put on the card (shapes, triangles, textures of its buildings and figures)
 * before what else is new waits for the next picture: the player's test10, two games of three down as he went to the
 * first person view among many lorries leaving a depot ("it fell because it was a great load when they leave the depot,
 * there are many of them and they are drawn"; the phone's driver down in its shader compiler, the device lost, at the
 * first draw of the vehicles with wheels -- right after that one picture had put every lorry of the scene on the card,
 * a 70 thousand corners and 16 MB of textures each). A picture a second apart the view is whole; the first draws of
 * the programs (FirstDraw()) come with little put on the card.
 */
static constexpr size_t EYE_PICTURE_NEW_BYTES = 32u << 20;
std::atomic<uint64_t> _gpu_eye_later{0}; ///< the diary: the buildings and figures of the pictures from the eyes left for a later picture so

bool PrepareEyeObjects(const ThreeDGpuEyePicture &pic, EyeObjectDraws &out)
{
	out.glass_alpha = pic.glass_alpha;
	const size_t sent_before = _card_sent.load();
	/* Whether the picture has put its fill of new things on the card (EYE_PICTURE_NEW_BYTES). */
	auto full = [sent_before]() {
		const size_t sent = _card_sent.load();
		return sent > sent_before && sent - sent_before > EYE_PICTURE_NEW_BYTES;
	};
	for (const ThreeDGpuEyeBuilding &e : pic.buildings) {
		if (full()) {
			/* A building not on the card yet, or a texture of its: the next picture. */
			bool is_new = !_card_buildings.contains(e.building.uid);
			for (const ThreeDObjectTexture *t : e.building.textures) is_new = is_new || (t != nullptr && !_card_textures.contains(t->uid.value));
			if (is_new) {
				_gpu_eye_later++;
				continue;
			}
		}
		const GpuBuilding *on = BuildingOnCard(e.building);
		if (on == nullptr) continue;
		EyeObjectDraws::Draw d{&e, on, std::vector<GLuint>(e.building.textures.size(), 0)};
		for (const GpuBuilding::Run &r : on->runs) {
			if (r.texture >= d.textures.size() || e.building.textures[r.texture] == nullptr || d.textures[r.texture] != 0) continue;
			const GpuTexture *t = TextureOnCard(*e.building.textures[r.texture]);
			if (t == nullptr) return false;
			d.textures[r.texture] = t->name;
		}
		/* What paints its first texture: the masks as they are, the patterns repeating and smoothed. One that cannot be
		 * put on the card: unpainted this picture. */
		for (const ThreeDGpuBuildingPaint &bp : e.building.paints) {
			if (d.parts.size() >= THREE_D_GPU_BUILDING_PAINTS) break;
			const GpuTexture *mask = bp.mask != nullptr ? TextureOnCard(*bp.mask) : nullptr;
			const GpuTexture *pattern = bp.pattern != nullptr ? PatternOnCard(*bp.pattern) : nullptr;
			if (mask == nullptr || (bp.pattern != nullptr && pattern == nullptr)) {
				d.parts.clear();
				break;
			}
			d.parts.push_back({mask->name, pattern != nullptr ? pattern->name : 0});
		}
		out.draws.push_back(std::move(d));
	}
	/* The figures' shapes, triangles, textures and the masks the card paints them by as it draws them (ThreeDGpuMesh::paints). */
	for (const ThreeDGpuMesh &m : pic.meshes) {
		if (m.shape == nullptr || m.triangles == nullptr || m.triangles->indices.empty()) continue;
		if (full()) {
			/* A figure whose shape or a texture is not on the card yet: the next picture. */
			bool is_new = !_card_shapes.contains(CardShapeKey(*m.shape));
			for (size_t i = 0; i < m.textures.size() && !is_new; i++) {
				if (m.textures[i] == nullptr || i >= m.triangles->used.size() || !m.triangles->used[i]) continue;
				is_new = !_card_textures.contains(m.textures[i]->uid.value);
			}
			if (is_new) {
				_gpu_eye_later++;
				continue;
			}
		}
		EyeObjectDraws::MeshDraw d{&m, ShapeOnCard(*m.shape), IndicesOnCard(*m.triangles), std::vector<GLuint>(m.textures.size(), 0), std::vector<PaintOnCard>(m.textures.size())};
		if (d.shape == nullptr || d.indices == nullptr) return false;
		if (m.bones != nullptr && !d.shape->has_bones) return false;
		/* As DrawOnCard(): a triangle naming a corner the shape has not, the processor draws. */
		if (d.indices->largest >= d.shape->corners) return false;
		bool gone = false;
		for (size_t i = 0; i < m.textures.size(); i++) {
			if (m.textures[i] == nullptr || i >= m.triangles->used.size() || !m.triangles->used[i]) continue;
			const GpuTexture *t = TextureOnCard(*m.textures[i]);
			if (t == nullptr) {
				/* Its pixels were let go of as the card held it, and the card let it go since: the figure not in this
				 * picture, its texture made again by the game as it asks for it the next time. */
				std::lock_guard lock(_card_texture_mutex);
				if (m.textures[i]->px.empty()) {
					gone = true;
					break;
				}
				return false;
			}
			d.textures[i] = t->name;
			if (i < m.paints.size() && m.paints[i].Any() && !PaintMasksOnCard(m.paints[i], d.paints[i])) return false;
		}
		if (gone) {
			_gpu_eye_later++;
			continue;
		}
		/* Among those alike, drawn at once (Batch); one with wheels that turn, or posed by its bones, on its own. */
		if (_eye_instances_on && _instanced_arrays_ok && (m.wheels.empty() || !d.shape->has_wheel) && m.bones == nullptr) {
			const double det = Determinant(m.matrix);
			const float mirrored = det > 0 ? 1.0f : (det < 0 ? -1.0f : 0.0f);
			const bool lit = m.lit && d.shape->has_nrm;
			EyeObjectDraws::Batch *batch = nullptr;
			for (EyeObjectDraws::Batch &b : out.batches) {
				if (b.shape != d.shape || b.indices != d.indices || b.mirrored != mirrored || b.textures != d.textures) continue;
				if ((b.mesh->lit && b.shape->has_nrm) != lit || b.mesh->paints.size() != m.paints.size() || b.mesh->glow_all != m.glow_all) continue;
				bool same = true;
				for (size_t i = 0; i < m.paints.size() && same; i++) same = SamePaint(b.mesh->paints[i], m.paints[i]);
				if (!same) continue;
				batch = &b;
				break;
			}
			if (batch == nullptr) {
				out.batches.push_back({&m, d.shape, d.indices, d.textures, d.paints, mirrored, {}});
				batch = &out.batches.back();
			}
			batch->instances.insert(batch->instances.end(), m.matrix.begin(), m.matrix.end());
			batch->instances.insert(batch->instances.end(), {m.light[0], m.light[1], m.light[2], lit ? 1.0f : 0.0f});
			continue;
		}
		out.mesh_draws.push_back(std::move(d));
	}
	return true;
}

/**
 * Draw the objects of the view from the eyes (PrepareEyeObjects()) into the picture bound, through its depth (near /
 * how far ahead, the larger the nearer): each side of a building that faces away left out as the processor leaves it
 * out -- on the picture y down, a triangle of side 1 drawn when its corners go round clockwise --; the figures opaque.
 */
/** The kind of a draw of the figures from the eyes for FirstDraw(): what the card compiles their program anew for. */
static std::string MeshDrawKind(bool glass, bool instanced, const GpuShape &shape, bool bones, int8_t facing, double determinant)
{
	const char *sides = facing == 0 || determinant == 0 ? "obe" : (facing * determinant < 0 ? "ccw" : "cw");
	return fmt::format("postavy z oci{}{}, normaly {}, kola {}, kosti {}, strany {}", glass ? ", sklo" : "", instanced ? ", instance" : "",
			shape.has_nrm ? 1 : 0, shape.has_wheel ? 1 : 0, bones && shape.has_bones ? 1 : 0, sides);
}

void DrawEyeObjects(const EyeObjectDraws &o, float near_plane)
{
	std::optional<EyeCallsOf> calls_of(std::in_place, 1);
	const GLuint bp = _eye_building_program.program;
	gl.glUseProgram(bp);
	gl.glUniform1i(_eye_building_program.tex, 0);
	gl.glUniform1f(_eye_building_program.near_plane, near_plane);
	/* The parts the card paints (EYE_BUILDING_FRAGMENT): their masks on the units 1 to 4, their patterns on 5 to 8. */
	const GLint u_parts = UniformOf(bp, "u_parts"), u_metres = UniformOf(bp, "u_metres");
	std::array<GLint, THREE_D_GPU_BUILDING_PAINTS> u_colour, u_about;
	for (int k = 0; k < static_cast<int>(THREE_D_GPU_BUILDING_PAINTS); k++) {
		gl.glUniform1i(UniformOf(bp, fmt::format("u_mask{}", k).c_str()), 1 + k);
		gl.glUniform1i(UniformOf(bp, fmt::format("u_pattern{}", k).c_str()), 5 + k);
		u_colour[k] = UniformOf(bp, fmt::format("u_colour[{}]", k).c_str());
		u_about[k] = UniformOf(bp, fmt::format("u_about[{}]", k).c_str());
	}
	gl.glUniform1i(u_parts, 0);
	int parts_set = 0;
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glCullFace(GL_BACK);
	/* What the runs set last: set again only what differs (as EyeDrawState); the parts' masks and patterns on their
	 * units and their colours too (houses alike one after another). */
	GLuint vao_set = 0, texture_set = 0;
	int cull_set = -2;
	std::array<GLuint, 2 * THREE_D_GPU_BUILDING_PAINTS> part_units_set{};
	std::array<float, 4 * THREE_D_GPU_BUILDING_PAINTS> colours_set, about_set;
	colours_set.fill(NAN);
	about_set.fill(NAN);
	std::array<float, 3> metres_set{NAN, NAN, NAN};
	/* The buildings alike one after the other (their shape, their textures, their paint's masks and patterns), and
	 * their runs side by side: those drawn with both sides first, then those with one, then the other -- each building
	 * as it came, tile by tile, bound its shape, its masks and its textures anew and switched the side left out at
	 * nearly every run, 23 calls a building (the rig, a street from the eyes: 2 800 of a picture's 4 600 calls the
	 * buildings'; the player's diary of 20. 11., #385: the card's calls are what a picture costs on the phone). All of
	 * it solid, each pixel by its depth: the order shows nowhere. The rig: 'test3d ocidomy 0|1'. */
	static std::vector<const EyeObjectDraws::Draw *> in_order;
	in_order.clear();
	for (const EyeObjectDraws::Draw &d : o.draws) in_order.push_back(&d);
	const bool by_kind = _eye_buildings_by_kind;
	if (by_kind) {
		std::stable_sort(in_order.begin(), in_order.end(), [](const EyeObjectDraws::Draw *a, const EyeObjectDraws::Draw *b) {
			if (a->on->vao != b->on->vao) return a->on->vao < b->on->vao;
			if (a->textures != b->textures) return a->textures < b->textures;
			for (size_t k = 0; k < a->parts.size() && k < b->parts.size(); k++) {
				if (a->parts[k].mask != b->parts[k].mask) return a->parts[k].mask < b->parts[k].mask;
				if (a->parts[k].pattern != b->parts[k].pattern) return a->parts[k].pattern < b->parts[k].pattern;
			}
			return a->parts.size() < b->parts.size();
		});
	}
	auto side_of = [](const EyeObjectDraws::Draw &d, const GpuBuilding::Run &r) { return !(r.facing != 0 && d.eye->building.cull) ? 0 : (r.facing > 0 ? 1 : 2); };
	for (int side = 0; side < (by_kind ? 3 : 1); side++) {
	for (const EyeObjectDraws::Draw *draw : in_order) {
		const EyeObjectDraws::Draw &d = *draw;
		/* Which of its runs are drawn now: all, or those of this side; with its paint only when one of them is of its first texture. */
		bool any = false, painted = false;
		for (const GpuBuilding::Run &r : d.on->runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0 || (by_kind && side_of(d, r) != side)) continue;
			any = true;
			painted = painted || r.texture == 0;
		}
		if (!any) continue;
		gl.glUniformMatrix4fv(_eye_building_program.matrix, 1, GL_TRUE, d.eye->matrix.data());
		if (d.on->vao != vao_set) gl.glBindVertexArray(d.on->vao);
		vao_set = d.on->vao;
		/* Its paint: the parts of its first texture, set once for its runs of that texture. */
		const int parts = static_cast<int>(std::min(d.parts.size(), d.eye->building.paints.size()));
		if (parts > 0 && painted) {
			std::array<float, 4 * THREE_D_GPU_BUILDING_PAINTS> colours{}, about{};
			for (int k = 0; k < parts; k++) {
				const ThreeDGpuBuildingPaint &p = d.eye->building.paints[k];
				colours[k * 4] = p.colour[0];
				colours[k * 4 + 1] = p.colour[1];
				colours[k * 4 + 2] = p.colour[2];
				colours[k * 4 + 3] = std::max(1.0f, p.start);
				about[k * 4] = d.parts[k].pattern != 0 ? p.pattern_m : 0.0f;
				about[k * 4 + 1] = p.only_sloped ? 1.0f : 0.0f;
			}
			bool active_moved = false;
			for (int k = 0; k < parts; k++) {
				const GLuint mask = d.parts[k].mask, pattern = d.parts[k].pattern != 0 ? d.parts[k].pattern : d.parts[k].mask;
				if (part_units_set[k] != mask) {
					gl.glActiveTexture(GL_TEXTURE1 + k);
					gl.glBindTexture(GL_TEXTURE_2D, mask);
					part_units_set[k] = mask;
					active_moved = true;
				}
				if (part_units_set[THREE_D_GPU_BUILDING_PAINTS + k] != pattern) {
					gl.glActiveTexture(GL_TEXTURE5 + k);
					gl.glBindTexture(GL_TEXTURE_2D, pattern);
					part_units_set[THREE_D_GPU_BUILDING_PAINTS + k] = pattern;
					active_moved = true;
				}
			}
			if (active_moved) gl.glActiveTexture(GL_TEXTURE0);
			for (int k = 0; k < parts; k++) {
				const auto c = std::span(colours).subspan(k * 4, 4), a = std::span(about).subspan(k * 4, 4);
				if (!std::ranges::equal(c, std::span(colours_set).subspan(k * 4, 4))) {
					gl.glUniform4f(u_colour[k], c[0], c[1], c[2], c[3]);
					std::ranges::copy(c, colours_set.begin() + k * 4);
				}
				if (!std::ranges::equal(a, std::span(about_set).subspan(k * 4, 4))) {
					gl.glUniform4f(u_about[k], a[0], a[1], a[2], a[3]);
					std::ranges::copy(a, about_set.begin() + k * 4);
				}
			}
			const std::array<float, 3> metres{d.eye->building.to_metres[0], d.eye->building.to_metres[1], d.eye->building.to_metres[2]};
			if (metres != metres_set) {
				gl.glUniform3f(u_metres, metres[0], metres[1], metres[2]);
				metres_set = metres;
			}
		}
		for (const GpuBuilding::Run &r : d.on->runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0) continue;
			if (by_kind && side_of(d, r) != side) continue;
			const int run_parts = r.texture == 0 ? parts : 0;
			if (run_parts != parts_set) {
				gl.glUniform1i(u_parts, run_parts);
				parts_set = run_parts;
			}
			const bool cull = r.facing != 0 && d.eye->building.cull;
			const int cull_now = !cull ? 0 : (r.facing > 0 ? 1 : 2);
			if (cull_now != cull_set) {
				if (cull_now == 0) {
					gl.glDisable(GL_CULL_FACE);
				} else {
					if (cull_set <= 0) gl.glEnable(GL_CULL_FACE);
					gl.glFrontFace(cull_now == 1 ? GL_CW : GL_CCW);
				}
				cull_set = cull_now;
			}
			if (d.textures[r.texture] != texture_set) gl.glBindTexture(GL_TEXTURE_2D, d.textures[r.texture]);
			texture_set = d.textures[r.texture];
			const bool first = FirstDrawOf(FIRST_DRAW_EYE_BUILDING | cull_now, [&] { return fmt::format("budovy z oci, strany {}", cull ? (r.facing > 0 ? "cw" : "ccw") : "obe"); });
			gl.glDrawArrays(GL_TRIANGLES, r.first, r.count);
			FirstDrawDone(first);
			_gpu_eye_triangles += r.count / 3;
			_gpu_eye_calls++;
		}
	}
	}
	gl.glDisable(GL_CULL_FACE);
	calls_of.reset();
	DrawEyeMeshes(o, near_plane, false);
}

/**
 * The figures of the view from the eyes, as DrawOnCard() draws their pictures: opaque, or their glass over what is
 * behind it (laid over by its alpha, its colour multiplied in), its depth written.
 */
/**
 * The shape's VAO for the batches of the view from the eyes (EyeObjectDraws::Batch): its corners as its own VAO has them,
 * and the instances' matrices and lights as attributes 4-8 from _eye_instance_vbo, one of each an instance -- set once,
 * not at every batch (the player's #381: 25 calls a batch to set them and take them off again, some 85 batches a frame).
 */
static GLuint InstancedVao(const GpuShape &s)
{
	if (s.vao_instanced != 0) return s.vao_instanced;
	if (_eye_instance_vbo == 0) gl.glGenBuffers(1, &_eye_instance_vbo);
	gl.glGenVertexArrays(1, &s.vao_instanced);
	gl.glBindVertexArray(s.vao_instanced);
	auto attribute = [](GLuint buffer, GLuint index, GLint size) {
		gl.glBindBuffer(GL_ARRAY_BUFFER, buffer);
		gl.glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, 0, nullptr);
		gl.glEnableVertexAttribArray(index);
	};
	attribute(s.pos, 0, 3);
	if (s.has_nrm) attribute(s.nrm, 1, 3);
	attribute(s.uv, 2, 2);
	if (s.has_wheel) attribute(s.wheel, 3, 1);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _eye_instance_vbo);
	for (GLuint a = 4; a <= 8; a++) {
		gl.glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, 20 * sizeof(float), reinterpret_cast<const void *>((a - 4) * 4 * sizeof(float)));
		gl.glEnableVertexAttribArray(a);
		gl.glVertexAttribDivisor(a, 1);
	}
	gl.glBindVertexArray(0);
	return s.vao_instanced;
}

/**
 * What the draws from the eyes (DrawEyeMeshes(), DrawEyeObjects()) set last: set again only what differs (the player's
 * #381: some 15 calls a draw, 17 thousand a frame from the eyes, the card 83 ms a frame at night).
 */
struct EyeDrawState {
	PaintSet paint;
	std::array<float, 4> glow{NAN, NAN, NAN, NAN}, light{NAN, NAN, NAN, NAN};
	float glass_alpha = NAN;
	int texel_alpha = -1, wheel_count = -1, bone_row = INT_MIN;
	int cull = -2;           ///< -2 not known, 0 off, 1 the front clockwise, 2 anticlockwise
	GLuint vao = 0, ibo = 0, texture = 0;

	/** The VAO, and with another one its element buffer not known. */
	void Vao(GLuint v)
	{
		if (v == this->vao) return;
		gl.glBindVertexArray(v);
		this->vao = v;
		this->ibo = 0;
	}
	void Ibo(GLuint i)
	{
		if (i == this->ibo) return;
		gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, i);
		this->ibo = i;
	}
	/** The texture of unit 0 (the paint's masks are on 1 to 5, the joints on BONE_UNIT, each leaving unit 0 active). */
	void Texture(GLuint t)
	{
		if (t == this->texture) return;
		gl.glBindTexture(GL_TEXTURE_2D, t);
		this->texture = t;
	}
	/** As CullVehicleRun(): a side left out, or none. */
	void Cull(int8_t facing, double determinant)
	{
		const int want = (facing == 0 || determinant == 0) ? 0 : (facing * determinant < 0 ? 2 : 1);
		if (want == this->cull) return;
		if (want == 0) {
			gl.glDisable(GL_CULL_FACE);
		} else {
			if (this->cull <= 0) {
				gl.glEnable(GL_CULL_FACE);
				gl.glCullFace(GL_BACK);
			}
			gl.glFrontFace(want == 2 ? GL_CCW : GL_CW);
		}
		this->cull = want;
	}
	void Glow(const GpuProgram &p, uint32_t glow)
	{
		const Colour e(glow);
		const std::array<float, 4> now{static_cast<float>(e.r), static_cast<float>(e.g), static_cast<float>(e.b), glow == 0 ? 0.0f : (e.a == ThreeDVehicleShape::GLOW_TEXTURED ? 1.0f : 2.0f)};
		if (now == this->glow) return;
		gl.glUniform4f(p.glow, now[0], now[1], now[2], now[3]);
		this->glow = now;
	}
	void Light(const GpuProgram &p, const ThreeDGpuMesh &mesh, const GpuShape &shape)
	{
		const std::array<float, 4> now{mesh.light[0], mesh.light[1], mesh.light[2], mesh.lit && shape.has_nrm ? 1.0f : 0.0f};
		if (now == this->light) return;
		gl.glUniform4f(p.light, now[0], now[1], now[2], now[3]);
		this->light = now;
	}
	void GlassAlpha(const GpuProgram &p, float a)
	{
		if (a == this->glass_alpha) return;
		gl.glUniform1f(p.glass_alpha, a);
		this->glass_alpha = a;
	}
	void TexelAlpha(const GpuProgram &p, int t)
	{
		if (t == this->texel_alpha) return;
		gl.glUniform1i(p.texel_alpha, t);
		this->texel_alpha = t;
	}
	void BoneRow(const GpuProgram &p, int row)
	{
		if (row == this->bone_row) return;
		gl.glUniform1i(p.bone_row, row);
		this->bone_row = row;
	}
	void WheelCount(const GpuProgram &p, int n)
	{
		if (n == this->wheel_count) return;
		gl.glUniform1i(p.wheel_count, n);
		this->wheel_count = n;
	}
};

/** The key of a kind of draw from the eyes for FirstDrawOf() (the words of MeshDrawKind()). */
static uint32_t MeshDrawKey(bool glass, bool instanced, const GpuShape &shape, bool bones, int8_t facing, double determinant)
{
	const uint32_t sides = facing == 0 || determinant == 0 ? 0 : (facing * determinant < 0 ? 1 : 2);
	return FIRST_DRAW_EYE_MESH | (glass ? 1 : 0) | (instanced ? 2 : 0) | (shape.has_nrm ? 4 : 0) | (shape.has_wheel ? 8 : 0) | (bones && shape.has_bones ? 16 : 0) | sides << 5;
}

/**
 * The figures of the view from the eyes, as DrawOnCard() draws their pictures: opaque, or their glass over what is
 * behind it (laid over by its alpha, its colour multiplied in), its depth written. Each state set only when it differs
 * from the draw before (EyeDrawState); the meshes alike at once by instances, their attributes in the shape's VAO for
 * them (InstancedVao()).
 */
void DrawEyeMeshes(const EyeObjectDraws &o, float near_plane, bool glass)
{
	const EyeCallsOf calls_of(glass ? 3 : 2);
	const GpuProgram &p = _eye_mesh_program;
	gl.glUseProgram(p.program);
	PaintUnits(p);
	ClearPaintUnits();
	gl.glUniform1f(p.near_plane, near_plane);
	gl.glUniform1i(p.instanced, 0);
	gl.glUniform1i(p.glass, glass ? 1 : 0);
	gl.glActiveTexture(GL_TEXTURE0);
	/* The attributes a shape has not (the normal, the turning wheel): their values the same for all. */
	gl.glVertexAttrib3f(1, 0, 0, 1);
	gl.glVertexAttrib3f(3, 0, 0, 0);
	if (glass) {
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
	EyeDrawState st;
	st.bone_row = -1;                                   // PaintUnits()
	st.texture = 0;
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	for (const EyeObjectDraws::MeshDraw &d : o.mesh_draws) {
		const std::vector<ThreeDGpuTriangles::Run> &runs = glass ? d.mesh->triangles->glass : d.mesh->triangles->opaque;
		if (runs.empty()) continue;
		if (!glass) _gpu_eye_single++;
		gl.glUniformMatrix4fv(p.matrix, 1, GL_TRUE, d.mesh->matrix.data());
		const double determinant = Determinant(d.mesh->matrix);
		st.Light(p, *d.mesh, *d.shape);
		st.BoneRow(p, d.mesh->bones != nullptr && d.shape->has_bones ? BonesOnCard(*d.mesh->bones) : -1);
		st.Vao(d.shape->vao);
		st.Ibo(d.indices->ibo);
		const size_t wheels = d.shape->has_wheel ? std::min(d.mesh->wheels.size(), THREE_D_GPU_EYE_WHEELS) : 0;
		st.WheelCount(p, static_cast<int>(wheels));
		if (wheels > 0) gl.glUniformMatrix4fv(p.wheels, static_cast<GLsizei>(wheels), GL_TRUE, d.mesh->wheels[0].data());
		for (const ThreeDGpuTriangles::Run &r : runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0) continue;
			st.Glow(p, d.mesh->Glow(r.texture));
			const uint8_t alpha = d.mesh->shape->Alpha(r.texture);
			if (glass) st.GlassAlpha(p, alpha < 255 ? alpha / 255.0f : o.glass_alpha);
			st.TexelAlpha(p, glass && alpha < 255 ? 1 : 0);
			SetPaint(p, r.texture < d.mesh->paints.size() ? &d.mesh->paints[r.texture] : nullptr, &d.paints[r.texture], &st.paint);
			st.Texture(d.textures[r.texture]);
			st.Cull(r.facing, -determinant);
			const bool first = FirstDrawOf(MeshDrawKey(glass, false, *d.shape, d.mesh->bones != nullptr, r.facing, -determinant),
					[&] { return MeshDrawKind(glass, false, *d.shape, d.mesh->bones != nullptr, r.facing, -determinant); });
			gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(static_cast<uintptr_t>(r.first) * sizeof(uint32_t)));
			FirstDrawDone(first);
			_gpu_eye_triangles += r.count / 3;
			_gpu_eye_calls++;
		}
	}
	/* The batches of meshes alike (EyeObjectDraws::Batch): the shape's triangles once for all its instances, their
	 * matrices and lights in attributes 4-8 of its VAO for instances from the instances' buffer, one of each an instance. */
	if (!o.batches.empty()) {
		gl.glUniform1i(p.instanced, 1);
		st.WheelCount(p, 0);
		st.BoneRow(p, -1);
		bool buffer_bound = false;
		for (const EyeObjectDraws::Batch &b : o.batches) {
			const std::vector<ThreeDGpuTriangles::Run> &runs = glass ? b.mesh->triangles->glass : b.mesh->triangles->opaque;
			if (runs.empty()) continue;
			const GLsizei count = static_cast<GLsizei>(b.instances.size() / 20);
			st.Vao(InstancedVao(*b.shape));
			st.Ibo(b.indices->ibo);
			if (!buffer_bound) {
				gl.glBindBuffer(GL_ARRAY_BUFFER, _eye_instance_vbo);
				buffer_bound = true;
			}
			gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(b.instances.size() * sizeof(float)), b.instances.data(), GL_STREAM_DRAW);
			for (const ThreeDGpuTriangles::Run &r : runs) {
				if (r.texture >= b.textures.size() || b.textures[r.texture] == 0) continue;
				st.Glow(p, b.mesh->Glow(r.texture));
				const uint8_t alpha = b.mesh->shape->Alpha(r.texture);
				if (glass) st.GlassAlpha(p, alpha < 255 ? alpha / 255.0f : o.glass_alpha);
				st.TexelAlpha(p, glass && alpha < 255 ? 1 : 0);
				SetPaint(p, r.texture < b.mesh->paints.size() ? &b.mesh->paints[r.texture] : nullptr, &b.paints[r.texture], &st.paint);
				st.Texture(b.textures[r.texture]);
				st.Cull(r.facing, -b.mirrored);
				const bool first = FirstDrawOf(MeshDrawKey(glass, true, *b.shape, false, r.facing, -b.mirrored),
						[&] { return MeshDrawKind(glass, true, *b.shape, false, r.facing, -b.mirrored); });
				gl.glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(static_cast<uintptr_t>(r.first) * sizeof(uint32_t)), count);
				FirstDrawDone(first);
				_gpu_eye_triangles += static_cast<uint64_t>(r.count / 3) * count;
				_gpu_eye_calls++;
			}
			if (!glass) {
				_gpu_eye_batches++;
				_gpu_eye_instances += count;
			}
		}
		if (buffer_bound) gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
		gl.glUniform1i(p.instanced, 0);
	}
	gl.glDisable(GL_CULL_FACE);
	gl.glFrontFace(GL_CCW);
	if (glass) gl.glDisable(GL_BLEND);
	gl.glBindVertexArray(0);
	ClearPaintUnits();
}

/**
 * For the diary (ThreeDGpuTimes()): the objects of a picture from the eyes drawn -- each one's triangles of all its
 * instances, and how many of them --, and how many different objects its batches were of.
 */
static void NoteEyeObjectsDrawn(const EyeObjectDraws &o, bool picture = true, int width = 0, int height = 0)
{
	auto triangles = [](const ThreeDGpuTriangles &t) {
		uint64_t n = 0;
		for (const auto *runs : {&t.opaque, &t.glass}) {
			for (const ThreeDGpuTriangles::Run &r : *runs) n += r.count / 3;
		}
		return n;
	};
	std::map<std::string_view, std::pair<uint64_t, uint64_t>> drawn;
	for (const EyeObjectDraws::Batch &b : o.batches) {
		const uint64_t count = b.instances.size() / 20;
		auto &d = drawn[b.mesh->name];
		d.first += triangles(*b.mesh->triangles) * count;
		d.second += count;
	}
	const size_t kinds = drawn.size();
	for (const EyeObjectDraws::Draw &b : o.draws) {
		auto &d = drawn["(budovy)"];
		for (const GpuBuilding::Run &r : b.on->runs) d.first += r.count / 3;
		d.second++;
	}
	for (const EyeObjectDraws::MeshDraw &m : o.mesh_draws) {
		auto &d = drawn[m.mesh->name];
		d.first += triangles(*m.mesh->triangles);
		d.second++;
	}
	std::lock_guard lock(_gpu_eye_most_mutex);
	if (picture) {
		_gpu_eye_frames++;
		_gpu_eye_pixels += static_cast<uint64_t>(width) * height;
	}
	_gpu_eye_steps++;
	_gpu_eye_kinds += kinds;
	for (const auto &[name, d] : drawn) {
		auto it = _gpu_eye_most.find(name);
		if (it == _gpu_eye_most.end()) it = _gpu_eye_most.emplace(std::string(name), std::pair<uint64_t, uint64_t>{0, 0}).first;
		it->second.first += d.first;
		it->second.second += d.second;
	}
}

/**
 * Draw the objects of the view from the eyes on the card and read them back (ThreeDGpuRenderEye()): the depth the
 * larger the nearer (near / how far ahead, cleared 0).
 */
bool DrawEyeOnCard(ThreeDGpuEyePicture &pic)
{
	const int w = pic.width, h = pic.height;
	if (w <= 0 || h <= 0 || w > _max_target || h > _max_target || pic.near_plane <= 0) return false;
	CountingEyeCalls counting;
	_card_clock++;
	Step(fmt::format("pohled z oci {}: {}x{}, {} budov, {} siti", _gpu_pictures + 1, w, h, pic.buildings.size(), pic.meshes.size()));
	EyeObjectDraws objects;
	if (!PrepareEyeObjects(pic, objects)) return false;
	if (!EyeTargetOfSize(w, h)) return false;
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _eye_fbo);
	gl.glViewport(0, 0, w, h);
	gl.glDrawBuffers(2, both);
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glDisable(GL_DITHER);
	gl.glDisable(GL_BLEND);
	gl.glEnable(GL_DEPTH_TEST);
	gl.glDepthFunc(GL_GREATER);
	gl.glDepthMask(GL_TRUE);
	gl.glClearColor(0, 0, 0, 0);
	gl.glClearDepth(0.0);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	Step(fmt::format("kresleni pohledu z oci {} (glDrawArrays)", _gpu_pictures + 1));
	DrawEyeObjects(objects, pic.near_plane);
	/* The glass after all else, over what is behind it, its depth written (what is behind it of the eye's own picture
	 * is laid under it there). */
	DrawEyeMeshes(objects, pic.near_plane, true);
	NoteEyeObjectsDrawn(objects, true, w, h);
	const size_t n = static_cast<size_t>(w) * h;
	pic.pixels.resize(n);
	pic.depth.resize(n);
	Step(fmt::format("cteni pohledu z oci {} zpet (glReadPixels)", _gpu_pictures + 1));
	gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
	gl.glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pic.pixels.data());
	gl.glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, pic.depth.data());
	if (pic.read_glow) {
		pic.glow.resize(n);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
		gl.glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pic.glow.data());
	} else {
		pic.glow.clear();
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	const GLenum error = gl.glGetError();
	if (error != 0) {
		Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x} in the view from the eyes, the processor draws it", error);
		if (error == GL_OUT_OF_MEMORY) ForgetCard();
		return false;
	}
	/* The depth as the eye's: 1 / how far ahead. */
	const float to_eye = 1.0f / pic.near_plane;
	for (float &d : pic.depth) d *= to_eye;
	TrimCard();
	return true;
}

/*
 * The whole view from the eyes on the card (ThreeDGpuRenderEyeFrame()): drawn into two pictures of colours taken in
 * turn -- a job over the whole picture reads the one and writes the other --, a picture of what has a light of its own
 * (the figures', glow), and the depth as a texture the jobs read (near / how far ahead, 0 nothing). The textures of the
 * frame alone laid into one (the atlas), each where its triangles find it; those the frames share kept by their number.
 */
/** Compile the programs of the frame on the card (StartCard()): without them, the frames are not drawn on it. */
void StartEyeFrame()
{
	std::string why;
	auto pixel = [&](GpuProgram &p, const char *body) {
		const std::string source = std::string(EYE_PIXEL_COMMON) + body;
		return MakeProgram(p, EYE_PIXEL_VERTEX, source.c_str(), why);
	};
	_frame_programs = MakeProgram(_frame_tri_program, EYE_TRI_VERTEX, EYE_TRI_FRAGMENT, why) &&
			pixel(_frame_background_program, EYE_BACKGROUND_FRAGMENT) && pixel(_frame_haze_program, EYE_HAZE_FRAGMENT) &&
			pixel(_frame_far_haze_program, EYE_FAR_HAZE_FRAGMENT) && pixel(_frame_shadow_program, EYE_SHADOW_FRAGMENT) &&
			pixel(_frame_night_program, EYE_NIGHT_FRAGMENT) && pixel(_frame_lamps_program, EYE_LAMPS_FRAGMENT) &&
			pixel(_frame_blasts_program, EYE_BLASTS_FRAGMENT) && pixel(_frame_glow_after_program, EYE_GLOW_AFTER_FRAGMENT) &&
			pixel(_frame_moon_program, EYE_MOON_FRAGMENT) && pixel(_frame_copy_program, EYE_COPY_FRAGMENT) && MakeProgram(_frame_lay_program, EYE_PIXEL_VERTEX, EYE_LAY_FRAGMENT, why) &&
			MakeProgram(_frame_glow_program, EYE_GLOW_VERTEX, EYE_GLOW_FRAGMENT, why) && MakeProgram(_frame_star_program, EYE_STAR_VERTEX, EYE_STAR_FRAGMENT, why);
	if (!_frame_programs) {
		Debug(misc, 1, "3D on the graphics card: the view from the eyes whole not on it ({})", why);
		Diary("snimek z oci na karte ne: " + why, true);
		return;
	}
	GLuint textures[11];
	gl.glGenTextures(11, textures);
	_frame_colour[0] = textures[0];
	_frame_colour[1] = textures[1];
	_frame_glow = textures[2];
	_frame_depth = textures[3];
	_frame_atlas = textures[4];
	_frame_list = textures[5];
	_frame_lay = textures[6];
	_frame_lay_z = textures[7];
	_frame_shadow_map = textures[8];
	for (int i = 0; i < 9; i++) {
		gl.glBindTexture(GL_TEXTURE_2D, textures[i]);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	}
	gl.glDeleteTextures(2, textures + 9);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	gl.glGenFramebuffers(2, _frame_geometry_fbo);
	gl.glGenFramebuffers(2, _frame_pixel_fbo);
	gl.glGenFramebuffers(1, &_frame_shown_fbo);
	gl.glGenVertexArrays(1, &_frame_vao);
	gl.glGenBuffers(1, &_frame_vbo);
	gl.glBindVertexArray(_frame_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_vbo);
	/* A corner: sx, sy, iw; u, v; its texture's place x, y, w, h; light, alpha, how. */
	const GLsizei stride = 12 * sizeof(float);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(3 * sizeof(float)));
	gl.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(5 * sizeof(float)));
	gl.glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(9 * sizeof(float)));
	for (GLuint a = 0; a < 4; a++) gl.glEnableVertexAttribArray(a);
	gl.glGenVertexArrays(1, &_frame_empty_vao);
	gl.glGenVertexArrays(1, &_frame_glow_vao);
	gl.glGenBuffers(1, &_frame_glow_vbo);
	gl.glBindVertexArray(_frame_glow_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_glow_vbo);
	/* A glow's corner: the corner (-1, 1), its middle and sizes, colour and strength, depth and hidden_by. */
	const GLsizei glow_stride = 13 * sizeof(float);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, glow_stride, nullptr);
	gl.glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, glow_stride, reinterpret_cast<const void *>(2 * sizeof(float)));
	gl.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, glow_stride, reinterpret_cast<const void *>(6 * sizeof(float)));
	gl.glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, glow_stride, reinterpret_cast<const void *>(10 * sizeof(float)));
	for (GLuint a = 0; a < 4; a++) gl.glEnableVertexAttribArray(a);
	gl.glGenVertexArrays(1, &_frame_star_vao);
	gl.glGenBuffers(1, &_frame_star_vbo);
	gl.glBindVertexArray(_frame_star_vao);
	gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_star_vbo);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
	gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<const void *>(2 * sizeof(float)));
	gl.glEnableVertexAttribArray(0);
	gl.glEnableVertexAttribArray(1);
	gl.glBindVertexArray(0);
	if (gl.glGetError() != 0) {
		_frame_programs = false;
		Diary("snimek z oci na karte ne: chyba OpenGL pri pripraveni", true);
	}
}

/** A picture shown straight from the card (ThreeDGpuReadShown()). */
struct ShownRead {
	uint32_t texture;
	int width, height;
	std::vector<uint32_t> *pixels;
};

/** A picture shown straight from the card read back, top row first: one of the three it is shown from, of its size. */
bool ReadShownOnCard(const ShownRead &r)
{
	int k = 0;
	while (k < 3 && (_frame_shown[k] == 0 || _frame_shown[k] != r.texture)) k++;
	if (k == 3 || _frame_shown_w[k] != r.width || _frame_shown_h[k] != r.height) return false;
	Step("cteni ukazaneho snimku zpet (snimek obrazovky)");
	r.pixels->resize(static_cast<size_t>(r.width) * r.height);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_shown_fbo);
	gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r.texture, 0);
	gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
	gl.glReadPixels(0, 0, r.width, r.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, r.pixels->data());
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return gl.glGetError() == 0;
}

/** Let go of what the frame on the card holds (EndCard()). */
void EndEyeFrame()
{
	const GLuint textures[] = {_frame_colour[0], _frame_colour[1], _frame_glow, _frame_depth, _frame_atlas, _frame_list, _frame_lay, _frame_lay_z, _frame_shadow_map};
	for (GLuint t : textures) if (t != 0) gl.glDeleteTextures(1, &t);
	for (GLuint t : _frame_shown) if (t != 0) gl.glDeleteTextures(1, &t);
	for (GLuint f : {_frame_geometry_fbo[0], _frame_geometry_fbo[1], _frame_pixel_fbo[0], _frame_pixel_fbo[1], _frame_shown_fbo}) if (f != 0) gl.glDeleteFramebuffers(1, &f);
	for (GLuint b : {_frame_vbo, _frame_glow_vbo, _frame_star_vbo}) if (b != 0) gl.glDeleteBuffers(1, &b);
	for (GLuint a : {_frame_vao, _frame_empty_vao, _frame_glow_vao, _frame_star_vao}) if (a != 0) gl.glDeleteVertexArrays(1, &a);
	for (GpuProgram *p : {&_frame_tri_program, &_frame_background_program, &_frame_haze_program, &_frame_far_haze_program, &_frame_shadow_program, &_frame_night_program,
			&_frame_lamps_program, &_frame_blasts_program, &_frame_glow_after_program, &_frame_lay_program, &_frame_glow_program, &_frame_star_program, &_frame_moon_program, &_frame_copy_program}) {
		if (p->program != 0) gl.glDeleteProgram(p->program);
	}
}

/** The frame's pictures at least as big as asked (grown in steps): the colours twice, the own light, the depth. */
bool EyeFrameTargetOfSize(int w, int h)
{
	if (w <= _frame_w && h <= _frame_h) return true;
	_frame_w = std::min(_max_target, (std::max(_frame_w, w) + 255) / 256 * 256);
	_frame_h = std::min(_max_target, (std::max(_frame_h, h) + 255) / 256 * 256);
	Step(fmt::format("ramec snimku z oci {}x{} (glTexImage2D, glCheckFramebufferStatus)", _frame_w, _frame_h));
	for (GLuint t : {_frame_colour[0], _frame_colour[1], _frame_glow}) {
		gl.glBindTexture(GL_TEXTURE_2D, t);
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _frame_w, _frame_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	}
	gl.glBindTexture(GL_TEXTURE_2D, _frame_depth);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, _frame_w, _frame_h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	for (int k = 0; k < 2; k++) {
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_geometry_fbo[k]);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _frame_colour[k], 0);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, _frame_glow, 0);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _frame_depth, 0);
		bool ok = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_pixel_fbo[k]);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _frame_colour[k], 0);
		ok = ok && gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		if (!ok) {
			gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
			_frame_w = _frame_h = 0;
			return false;
		}
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return w <= _frame_w && h <= _frame_h;
}

/** A texture the frames share on the card by its number, its texels as they are (Colour::data). */
const GpuTexture *KeptTextureOnCard(uint64_t uid, const uint32_t *px, int w, int h)
{
	auto it = _card_textures.find(uid);
	if (it != _card_textures.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
	if (w <= 0 || h <= 0 || w > _max_texture || h > _max_texture || px == nullptr) return nullptr;
	GpuTexture g;
	gl.glGenTextures(1, &g.name);
	gl.glBindTexture(GL_TEXTURE_2D, g.name);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
	g.bytes = static_cast<size_t>(w) * h * 4;
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_bytes_of[2] += g.bytes;
	_card_sent_of[2] += g.bytes;
	_card_sent += g.bytes;
	CardHeld(_card_held_textures, uid, true);
	return &_card_textures.emplace(uid, g).first->second;
}

/**
 * The frame's own textures laid into the atlas, the tallest first, row by row, and put on the card: where each is.
 * False when they do not fit in the largest texture the card has.
 */
bool EyeFrameAtlas(const ThreeDGpuEyeFrame &f, std::vector<std::array<int, 2>> &place)
{
	place.assign(f.textures.size(), {-1, -1});
	std::vector<uint32_t> order;
	int widest = 1;
	size_t area = 0;
	for (uint32_t i = 0; i < f.textures.size(); i++) {
		const ThreeDGpuEyeTexture &t = f.textures[i];
		if (t.uid != 0 || t.ground != nullptr || t.w <= 0 || t.h <= 0) continue;
		order.push_back(i);
		widest = std::max(widest, t.w);
		area += static_cast<size_t>(t.w) * t.h;
	}
	if (order.empty()) return true;
	if (widest > _max_texture) return false;
	const int aw = std::min(_max_texture, std::max({widest, 1024, (static_cast<int>(std::sqrt(static_cast<double>(area) * 1.3)) + 255) / 256 * 256}));
	std::ranges::sort(order, [&](uint32_t a, uint32_t b) { return f.textures[a].h > f.textures[b].h; });
	int x = 0, y = 0, row_h = 0;
	for (uint32_t i : order) {
		const ThreeDGpuEyeTexture &t = f.textures[i];
		if (x + t.w > aw) {
			x = 0;
			y += row_h;
			row_h = 0;
		}
		place[i] = {x, y};
		x += t.w;
		row_h = std::max(row_h, t.h);
	}
	const int ah = y + row_h;
	if (ah > _max_texture) return false;
	_frame_atlas_px.resize(static_cast<size_t>(aw) * ah);
	for (uint32_t i : order) {
		const ThreeDGpuEyeTexture &t = f.textures[i];
		for (int r = 0; r < t.h; r++) std::copy_n(t.px + static_cast<size_t>(r) * t.w, t.w, _frame_atlas_px.data() + static_cast<size_t>(place[i][1] + r) * aw + place[i][0]);
	}
	gl.glBindTexture(GL_TEXTURE_2D, _frame_atlas);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (aw > _frame_atlas_w || ah > _frame_atlas_h) {
		_frame_atlas_w = std::max(_frame_atlas_w, aw);
		_frame_atlas_h = std::min(_max_texture, std::max(_frame_atlas_h, (ah + 255) / 256 * 256));
		Step(fmt::format("atlas snimku z oci {}x{}", _frame_atlas_w, _frame_atlas_h));
		gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _frame_atlas_w, _frame_atlas_h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
	}
	gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, aw, ah, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, _frame_atlas_px.data());
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	return true;
}

/** A list of numbers of a step put on the card as a row of texels, four floats each (the lights, the blasts). */
void EyeFrameList(const std::vector<float> &values)
{
	const GLsizei n = static_cast<GLsizei>(std::max<size_t>(1, values.size() / 4));
	gl.glBindTexture(GL_TEXTURE_2D, _frame_list);
	gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, n, 1, 0, GL_RGBA, GL_FLOAT, values.empty() ? nullptr : values.data());
}

/**
 * Draw the whole view from the eyes on the card and read its colours back (ThreeDGpuRenderEyeFrame()): its steps in
 * their order -- what is drawn through the depth into the picture of the moment, the jobs over the whole picture from
 * it into the other. The figures' glass after the triangles following them (all that is solid first), before a job.
 */
bool DrawEyeFrameOnCard(ThreeDGpuEyeFrame &f)
{
	const int w = f.width, h = f.height;
	if (!_frame_programs || w <= 0 || h <= 0 || w > _max_target || h > _max_target || f.near_plane <= 0) return false;
	CountingEyeCalls counting;
	_card_clock++;
	Step(fmt::format("snimek z oci {}: {}x{}, {} kroku, {} trojuhelniku, {} textur", _gpu_pictures + 1, w, h, f.steps.size(), f.corners.size() / 3, f.textures.size()));
	/* The depth on the card: near / 64 / how far ahead, so that what is nearer than the near plane (a thing in the
	 * hand) is not cut to 1 (a float keeps its digits however small it is). */
	const float depth_scale = f.near_plane / 64.0f;
	/* What the steps need, before anything is drawn: the textures, the objects. */
	std::vector<std::array<int, 2>> place;
	if (!EyeFrameAtlas(f, place)) return false;
	std::vector<GLuint> names(f.textures.size(), 0);
	for (size_t i = 0; i < f.textures.size(); i++) {
		const ThreeDGpuEyeTexture &t = f.textures[i];
		if (t.ground != nullptr) {
			/* The kept ground of the view from the eyes, as the turned view's: what was drawn into it since sent. */
			const GroundTextureOnCard *g = GroundTextureOf(*t.ground);
			if (g == nullptr || g->w != t.w || g->h != t.h) return false;
			names[i] = g->name;
			place[i] = {0, 0};
			continue;
		}
		if (t.uid == 0) {
			names[i] = _frame_atlas;
			continue;
		}
		const GpuTexture *g = KeptTextureOnCard(t.uid, t.px, t.w, t.h);
		if (g == nullptr) return false;
		names[i] = g->name;
		place[i] = {0, 0};
	}
	std::vector<EyeObjectDraws> objects(f.steps.size());
	for (size_t i = 0; i < f.steps.size(); i++) {
		if (f.steps[i].kind == ThreeDGpuEyeStep::Kind::Objects && f.steps[i].objects != nullptr && !PrepareEyeObjects(*f.steps[i].objects, objects[i])) return false;
	}
	if (!EyeFrameTargetOfSize(w, h)) return false;
	/* The corners of all the triangles at once. */
	{
		std::vector<float> corners;
		corners.reserve(f.corners.size() * 12);
		std::vector<uint32_t> texture_of(f.corners.size(), 0);
		for (const ThreeDGpuEyeRun &r : f.runs) {
			for (uint32_t k = r.first; k < r.first + r.count && k < f.corners.size(); k++) texture_of[k] = r.texture;
		}
		for (size_t k = 0; k < f.corners.size(); k++) {
			const ThreeDGpuEyeCorner &c = f.corners[k];
			const uint32_t t = texture_of[k];
			const ThreeDGpuEyeTexture &tex = f.textures[t];
			corners.insert(corners.end(), {c.sx, c.sy, c.iw, c.u, c.v, static_cast<float>(place[t][0]), static_cast<float>(place[t][1]), static_cast<float>(tex.w), static_cast<float>(tex.h), c.light, c.alpha, c.how});
		}
		gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_vbo);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(corners.size() * sizeof(float)), corners.empty() ? nullptr : corners.data(), GL_STREAM_DRAW);
	}
	Step(fmt::format("kresleni snimku z oci {}", _gpu_pictures + 1));
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	static const GLenum colour_only[1] = {GL_COLOR_ATTACHMENT0};
	int now = 0; // the picture of the moment
	gl.glViewport(0, 0, w, h);
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glDisable(GL_DITHER);
	gl.glDisable(GL_BLEND);
	gl.glDisable(GL_CULL_FACE);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_geometry_fbo[now]);
	gl.glDrawBuffers(2, both);
	gl.glClearColor(0, 0, 0, 0);
	gl.glClearDepth(0.0);
	gl.glDepthMask(GL_TRUE);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	auto geometry = [&]() {
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_geometry_fbo[now]);
		gl.glDrawBuffers(2, both);
		gl.glEnable(GL_DEPTH_TEST);
		gl.glDepthFunc(GL_GREATER);
		gl.glDepthMask(GL_TRUE);
	};
	/* A job over the whole picture: its program, the picture of the moment read, the other written. */
	std::string_view job_name;
	auto job = [&](const GpuProgram &p, std::string_view name) {
		job_name = name;
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_pixel_fbo[1 - now]);
		gl.glDrawBuffers(1, colour_only);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glDisable(GL_BLEND);
		gl.glUseProgram(p.program);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, _frame_colour[now]);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, _frame_depth);
		gl.glUniform1i(UniformOf(p.program, "u_colour"), 0);
		gl.glUniform1i(UniformOf(p.program, "u_depth"), 1);
		gl.glUniform1f(UniformOf(p.program, "u_near"), depth_scale);
		gl.glUniform4f(UniformOf(p.program, "u_eye_dir"), f.eye[3], f.eye[4], f.eye[5], f.eye[6]);
		gl.glUniform3f(UniformOf(p.program, "u_eye_lens"), f.eye[7], f.eye[8], f.eye[9]);
		gl.glUniform2f(UniformOf(p.program, "u_eye_tip"), f.eye[10], f.eye[11]);
		gl.glUniform2f(UniformOf(p.program, "u_size"), static_cast<float>(w), static_cast<float>(h));
	};
	auto done = [&]() {
		gl.glBindVertexArray(_frame_empty_vao);
		const bool first = FirstDraw(fmt::format("snimku z oci: {}", job_name));
		gl.glDrawArrays(GL_TRIANGLES, 0, 3);
		FirstDrawDone(first);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		now = 1 - now;
	};
	/* Drawn over the picture of the moment (added, or laid over), the depth read. */
	auto over = [&](const GpuProgram &p) {
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_pixel_fbo[now]);
		gl.glDrawBuffers(1, colour_only);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glUseProgram(p.program);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, _frame_depth);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glUniform1i(UniformOf(p.program, "u_depth"), 1);
		gl.glUniform1f(UniformOf(p.program, "u_near"), depth_scale);
		gl.glUniform2f(UniformOf(p.program, "u_size"), static_cast<float>(w), static_cast<float>(h));
		gl.glUniform4f(UniformOf(p.program, "u_eye_dir"), f.eye[3], f.eye[4], f.eye[5], f.eye[6]);
		gl.glUniform3f(UniformOf(p.program, "u_eye_lens"), f.eye[7], f.eye[8], f.eye[9]);
		gl.glUniform2f(UniformOf(p.program, "u_eye_tip"), f.eye[10], f.eye[11]);
	};
	auto glows = [&](const std::vector<ThreeDGpuEyeGlow> &list) {
		if (list.empty()) return;
		std::vector<float> v;
		v.reserve(list.size() * 6 * 13);
		static const float quad[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
		for (const ThreeDGpuEyeGlow &g : list) {
			if (g.rx < 0.5f || g.ry < 0.5f) continue;
			for (const auto &q : quad) v.insert(v.end(), {q[0], q[1], g.sx, g.sy, g.rx, g.ry, g.r, g.g, g.b, g.strength, g.iw, g.hidden_by, g.lamp});
		}
		if (v.empty()) return;
		over(_frame_glow_program);
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE);
		gl.glBindVertexArray(_frame_glow_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_glow_vbo);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
		const bool first = FirstDraw("snimku z oci: zare");
		gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 13));
		FirstDrawDone(first);
		gl.glDisable(GL_BLEND);
	};
	/* The figures' glass, after what is solid; seen through, its depth not kept (what is behind it is lit, hazed and
	 * seen to be hidden as it is). */
	std::vector<size_t> glass_waiting;
	bool noted = false; // the picture counted for the diary (NoteEyeObjectsDrawn())
	auto glass = [&]() {
		if (glass_waiting.empty()) return;
		geometry();
		gl.glDepthMask(GL_FALSE);
		for (size_t i : glass_waiting) DrawEyeMeshes(objects[i], depth_scale, true);
		gl.glDepthMask(GL_TRUE);
		glass_waiting.clear();
	};
	for (size_t i = 0; i < f.steps.size(); i++) {
		const ThreeDGpuEyeStep &s = f.steps[i];
		using Kind = ThreeDGpuEyeStep::Kind;
		if (s.kind != Kind::Triangles && s.kind != Kind::Objects && s.kind != Kind::Lay) glass();
		switch (s.kind) {
			case Kind::Background:
				job(_frame_background_program, "obloha");
				gl.glUniform1f(UniformOf(_frame_background_program.program, "u_horizon"), s.horizon);
				done();
				break;
			case Kind::Triangles: {
				const EyeCallsOf calls_of(0);
				geometry();
				gl.glUseProgram(_frame_tri_program.program);
				gl.glUniform1i(UniformOf(_frame_tri_program.program, "u_tex"), 0);
				gl.glUniform1f(UniformOf(_frame_tri_program.program, "u_near"), depth_scale);
				gl.glUniform2f(UniformOf(_frame_tri_program.program, "u_size"), static_cast<float>(w), static_cast<float>(h));
				/* Our grass of the ground (THREE_D_GPU_EYE_GRASS): its picture beside the texture, where the eye is. */
				const bool grass = f.grass_texture >= 0 && static_cast<size_t>(f.grass_texture) < names.size() && f.grass_units > 0;
				gl.glUniform1i(UniformOf(_frame_tri_program.program, "u_grass"), 1);
				gl.glUniform1f(UniformOf(_frame_tri_program.program, "u_grass_units"), grass ? f.grass_units : 0.0f);
				gl.glUniform2f(UniformOf(_frame_tri_program.program, "u_grass_eye"), f.grass_eye[0], f.grass_eye[1]);
				for (int k = 0; k < THREE_D_GPU_GRASS_KINDS; k++) {
					const std::string name = fmt::format("u_grass_tint[{}]", k);
					gl.glUniform3f(UniformOf(_frame_tri_program.program, name.c_str()), f.grass_tint[k * 3], f.grass_tint[k * 3 + 1], f.grass_tint[k * 3 + 2]);
				}
				gl.glUniform4f(UniformOf(_frame_tri_program.program, "u_eye_dir"), f.eye[3], f.eye[4], f.eye[5], f.eye[6]);
				gl.glUniform3f(UniformOf(_frame_tri_program.program, "u_eye_lens"), f.eye[7], f.eye[8], f.eye[9]);
				gl.glUniform2f(UniformOf(_frame_tri_program.program, "u_eye_tip"), f.eye[10], f.eye[11]);
				gl.glActiveTexture(GL_TEXTURE1);
				gl.glBindTexture(GL_TEXTURE_2D, grass ? names[f.grass_texture] : 0);
				gl.glActiveTexture(GL_TEXTURE0);
				gl.glBindVertexArray(_frame_vao);
				/* The texture and the blend set again only when they differ from the run before (as EyeDrawState). */
				GLuint texture_set = 0;
				bool glass_set = false;
				gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				/* The order its runs are drawn in. The solid ones first, those of a texture together whatever lies between
				 * them as they were gathered (their order among themselves kept), each texture bound once and all its runs
				 * drawn by one call; the see-through after them, in their order. Each run is a billboard or a face with a
				 * texture of its own -- a tree's sprite, a sign, a wall --, gathered tile by tile, so a row of trees of three
				 * kinds was a texture bound and a call for every tree (the player's diary of 20. 11., #385, from the eyes:
				 * 818 of these calls and as many textures bound a picture, of 8000 calls; the card's calls are what a
				 * picture costs there). What is solid keeps its depth, so its order shows only where two faces lie in
				 * each other exactly. */
				static std::vector<size_t> order;
				static std::vector<GLint> firsts;
				static std::vector<GLsizei> counts;
				order.clear();
				const size_t runs_end = std::min(s.first_run + s.runs, f.runs.size());
				for (size_t r = s.first_run; r < runs_end; r++) {
					if (!_eye_runs_by_texture || !f.runs[r].glass) order.push_back(r);
				}
				if (_eye_runs_by_texture) {
					std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return names[f.runs[a].texture] < names[f.runs[b].texture]; });
					for (size_t r = s.first_run; r < runs_end; r++) {
						if (f.runs[r].glass) order.push_back(r);
					}
				}
				for (size_t k = 0; k < order.size();) {
					const ThreeDGpuEyeRun &run = f.runs[order[k]];
					/* Those after it of the same texture at once: one range where they lie one after the other. */
					firsts.assign(1, static_cast<GLint>(run.first));
					counts.assign(1, static_cast<GLsizei>(run.count));
					for (k++; k < order.size(); k++) {
						const ThreeDGpuEyeRun &next = f.runs[order[k]];
						if (names[next.texture] != names[run.texture] || next.glass != run.glass) break;
						if (static_cast<uint32_t>(firsts.back() + counts.back()) == next.first) {
							counts.back() += static_cast<GLsizei>(next.count);
						} else if (_eye_runs_by_texture) {
							firsts.push_back(static_cast<GLint>(next.first));
							counts.push_back(static_cast<GLsizei>(next.count));
						} else {
							break;
						}
					}
					if (run.glass != glass_set) {
						if (run.glass) {
							gl.glEnable(GL_BLEND);
							gl.glDepthMask(GL_FALSE);
						} else {
							gl.glDisable(GL_BLEND);
							gl.glDepthMask(GL_TRUE);
						}
						glass_set = run.glass;
					}
					if (names[run.texture] != texture_set) gl.glBindTexture(GL_TEXTURE_2D, names[run.texture]);
					texture_set = names[run.texture];
					const bool first = FirstDrawOf(FIRST_DRAW_EYE_TRIANGLES | (run.glass ? 1 : 0) | (grass ? 2 : 0),
							[&] { return fmt::format("snimku z oci: trojuhelniky{}{}", run.glass ? ", sklo" : "", grass ? ", trava" : ""); });
					if (firsts.size() > 1 && gl.glMultiDrawArrays != nullptr) {
						gl.glMultiDrawArrays(GL_TRIANGLES, firsts.data(), counts.data(), static_cast<GLsizei>(firsts.size()));
					} else {
						for (size_t j = 0; j < firsts.size(); j++) gl.glDrawArrays(GL_TRIANGLES, firsts[j], counts[j]);
					}
					FirstDrawDone(first);
				}
				if (glass_set) {
					gl.glDisable(GL_BLEND);
					gl.glDepthMask(GL_TRUE);
				}
				gl.glBindVertexArray(0);
				gl.glActiveTexture(GL_TEXTURE1);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Objects:
				if (s.objects == nullptr) break;
				geometry();
				DrawEyeObjects(objects[i], depth_scale);
				glass_waiting.push_back(i);
				NoteEyeObjectsDrawn(objects[i], !noted, w, h);
				noted = true;
				break;
			case Kind::Lay: {
				/* Only its rectangle given, drawn into (the scissor) and laid: a portal's opening of a few hundred pixels
				 * was two pictures the size of the whole put on the card, five portals a frame (the rig, a walker). */
				const auto [lx, ly, lw, lh] = s.lay_rect;
				if (lw <= 0 || lh <= 0 || lx < 0 || ly < 0 || lx + lw > w || ly + lh > h) break;
				if (s.lay_px.size() < static_cast<size_t>(lw) * lh || s.lay_z.size() < static_cast<size_t>(lw) * lh) break;
				gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_lay);
				gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s.lay_px.data());
				gl.glActiveTexture(GL_TEXTURE3);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_lay_z);
				gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, lw, lh, 0, GL_RED, GL_FLOAT, s.lay_z.data());
				geometry();
				gl.glDepthFunc(GL_GEQUAL);
				gl.glUseProgram(_frame_lay_program.program);
				gl.glUniform1i(UniformOf(_frame_lay_program.program, "u_lay"), 2);
				gl.glUniform1i(UniformOf(_frame_lay_program.program, "u_lay_z"), 3);
				gl.glUniform2i(UniformOf(_frame_lay_program.program, "u_lay_at"), lx, ly);
				gl.glUniform1f(UniformOf(_frame_lay_program.program, "u_near"), depth_scale);
				gl.glBindVertexArray(_frame_empty_vao);
				gl.glEnable(GL_SCISSOR_TEST);
				gl.glScissor(lx, ly, lw, lh);
				const bool first = FirstDraw("snimku z oci: polozeny obraz (tunel)");
				gl.glDrawArrays(GL_TRIANGLES, 0, 3);
				FirstDrawDone(first);
				gl.glDisable(GL_SCISSOR_TEST);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Haze:
				job(_frame_haze_program, "mlha");
				gl.glUniform3f(UniformOf(_frame_haze_program.program, "u_haze"), s.colour[0], s.colour[1], s.colour[2]);
				gl.glUniform1f(UniformOf(_frame_haze_program.program, "u_from"), s.from);
				gl.glUniform1f(UniformOf(_frame_haze_program.program, "u_reach"), s.depth);
				done();
				break;
			case Kind::FarHaze:
				job(_frame_far_haze_program, "mlha v dali");
				gl.glUniform3f(UniformOf(_frame_far_haze_program.program, "u_haze"), s.colour[0], s.colour[1], s.colour[2]);
				gl.glUniform1f(UniformOf(_frame_far_haze_program.program, "u_from"), s.from);
				gl.glUniform1f(UniformOf(_frame_far_haze_program.program, "u_to"), s.to);
				gl.glUniform3f(UniformOf(_frame_far_haze_program.program, "u_absorb"), s.absorb[0], s.absorb[1], s.absorb[2]);
				gl.glUniform1f(UniformOf(_frame_far_haze_program.program, "u_horizon"), s.horizon);
				done();
				break;
			case Kind::Shadows: {
				const ThreeDGpuEyeStep::ShadowMap &m = s.shadow_map;
				if (m.nearest == nullptr || m.nu <= 0 || m.nv <= 0 || m.nu > _max_texture || m.nv > _max_texture) break;
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_shadow_map);
				if (m.key != _frame_shadow_key) {
					Step(fmt::format("mapa stinu {}x{}", m.nu, m.nv));
					gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
					gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, m.nu, m.nv, 0, GL_RED, GL_FLOAT, m.nearest);
					_frame_shadow_key = m.key;
				}
				job(_frame_shadow_program, "stiny");
				const GLuint p = _frame_shadow_program.program;
				gl.glUniform1i(UniformOf(p, "u_map"), 2);
				gl.glUniform3f(UniformOf(p, "u_sun"), m.sun[0], m.sun[1], m.sun[2]);
				gl.glUniform3f(UniformOf(p, "u_a"), m.a[0], m.a[1], m.a[2]);
				gl.glUniform3f(UniformOf(p, "u_b"), m.b[0], m.b[1], m.b[2]);
				gl.glUniform4f(UniformOf(p, "u_map_place"), m.u0, m.v0, m.texel, m.bias);
				gl.glUniform1f(UniformOf(p, "u_sun0"), m.sun0);
				done();
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Night:
				job(_frame_night_program, "noc");
				gl.glUniform3f(UniformOf(_frame_night_program.program, "u_night"), s.colour[0], s.colour[1], s.colour[2]);
				done();
				glows(s.glows);
				break;
			case Kind::Glows:
				glows(s.glows);
				break;
			case Kind::GlowAfterNight:
				job(_frame_glow_after_program, "svetla po noci");
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_glow);
				gl.glUniform1i(UniformOf(_frame_glow_after_program.program, "u_glow"), 2);
				done();
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			case Kind::Lamps: {
				if (s.lights.empty()) break;
				std::vector<float> list;
				for (const ThreeDGpuEyeLight &l : s.lights) {
					list.insert(list.end(), {l.x, l.y, l.zu, l.reach, l.r, l.g, l.b, static_cast<float>(l.kind), l.dx, l.dy, l.dz, l.cos_edge,
							static_cast<float>(l.x0), static_cast<float>(l.y0), static_cast<float>(l.x1), static_cast<float>(l.y1)});
				}
				gl.glActiveTexture(GL_TEXTURE2);
				EyeFrameList(list);
				job(_frame_lamps_program, "lampy");
				const GLuint p = _frame_lamps_program.program;
				gl.glUniform1i(UniformOf(p, "u_lights"), 2);
				gl.glUniform1i(UniformOf(p, "u_count"), static_cast<GLint>(s.lights.size()));
				gl.glUniform3f(UniformOf(p, "u_night"), s.colour[0], s.colour[1], s.colour[2]);
				gl.glUniform1f(UniformOf(p, "u_zu_per_unit"), s.depth);
				gl.glUniform4f(UniformOf(p, "u_tube"), s.tube[0], s.tube[1], s.tube[2], s.tube[3]);
				gl.glUniform1i(UniformOf(p, "u_tube_axis"), s.tube_axis);
				gl.glUniform1i(UniformOf(p, "u_tube_day"), s.tube_day ? 1 : 0);
				done();
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Blasts: {
				if (s.blasts.empty()) break;
				std::vector<float> list;
				for (const ThreeDGpuEyeBlast &b : s.blasts) list.insert(list.end(), {b.sx, b.sy, b.radius, b.iw, b.r, b.g, b.b, b.alpha});
				gl.glActiveTexture(GL_TEXTURE2);
				EyeFrameList(list);
				job(_frame_blasts_program, "kour a vybuchy");
				gl.glUniform1i(UniformOf(_frame_blasts_program.program, "u_blasts"), 2);
				gl.glUniform1i(UniformOf(_frame_blasts_program.program, "u_count"), static_cast<GLint>(s.blasts.size()));
				done();
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Sky: {
				std::vector<float> stars;
				for (const ThreeDGpuEyeStar &st : s.stars) {
					auto add = [&](int x, int y, int a) {
						if (x < 0 || y < 0 || x >= w || y >= h) return;
						stars.insert(stars.end(), {static_cast<float>(x), static_cast<float>(y), std::floor(st.r * a / 255.0f), std::floor(st.g * a / 255.0f), std::floor(st.b * a / 255.0f)});
					};
					add(st.x, st.y, st.a);
					if (st.cross) {
						add(st.x - 1, st.y, st.a / 3);
						add(st.x + 1, st.y, st.a / 3);
						add(st.x, st.y - 1, st.a / 3);
						add(st.x, st.y + 1, st.a / 3);
					}
				}
				if (!stars.empty()) {
					over(_frame_star_program);
					gl.glUniform1f(UniformOf(_frame_star_program.program, "u_horizon"), s.horizon);
					gl.glEnable(GL_BLEND);
					gl.glBlendFunc(GL_ONE, GL_ONE);
					gl.glBindVertexArray(_frame_star_vao);
					gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_star_vbo);
					gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(stars.size() * sizeof(float)), stars.data(), GL_STREAM_DRAW);
					const bool first = FirstDraw("snimku z oci: hvezdy");
					gl.glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(stars.size() / 5));
					FirstDrawDone(first);
					gl.glDisable(GL_BLEND);
				}
				if (s.moon[2] > 0) {
					const float rad = s.moon[2];
					const float box[4] = {std::max(0.0f, static_cast<float>(static_cast<int>(s.moon[0] - rad * 4))), std::max(0.0f, static_cast<float>(static_cast<int>(s.moon[1] - rad * 4))),
							std::min(w - 1.0f, static_cast<float>(static_cast<int>(s.moon[0] + rad * 4))), std::min(h - 1.0f, static_cast<float>(static_cast<int>(s.moon[1] + rad * 4)))};
					for (int glow = 0; glow < 2; glow++) {
						over(_frame_moon_program);
						const GLuint p = _frame_moon_program.program;
						gl.glUniform3f(UniformOf(p, "u_moon"), s.moon[0], s.moon[1], s.moon[2]);
						gl.glUniform4f(UniformOf(p, "u_box"), box[0], box[1], box[2], box[3]);
						gl.glUniform1f(UniformOf(p, "u_horizon"), s.horizon);
						gl.glUniform1i(UniformOf(p, "u_glow"), glow);
						if (glow == 1) {
							gl.glEnable(GL_BLEND);
							gl.glBlendFunc(GL_ONE, GL_ONE);
						}
						gl.glBindVertexArray(_frame_empty_vao);
						const bool first = FirstDraw(glow == 1 ? "snimku z oci: mesic, zare" : "snimku z oci: mesic");
						gl.glDrawArrays(GL_TRIANGLES, 0, 3);
						FirstDrawDone(first);
						gl.glDisable(GL_BLEND);
					}
				}
				gl.glBindVertexArray(0);
				gl.glActiveTexture(GL_TEXTURE1);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
		}
	}
	glass();
	gl.glBindVertexArray(0);
	f.shown = 0;
	if (f.show && _video_shared) {
		/* Shown straight from the card: into the picture of the three not shown now, done before it is handed on. */
		const int k = _frame_shown_next;
		_frame_shown_next = (k + 1) % 3;
		if (_frame_shown[k] == 0) {
			gl.glGenTextures(1, &_frame_shown[k]);
			gl.glBindTexture(GL_TEXTURE_2D, _frame_shown[k]);
			gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
			gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		}
		gl.glBindTexture(GL_TEXTURE_2D, _frame_shown[k]);
		if (_frame_shown_w[k] != w || _frame_shown_h[k] != h) {
			gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
			_frame_shown_w[k] = w;
			_frame_shown_h[k] = h;
		}
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_shown_fbo);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _frame_shown[k], 0);
		gl.glDrawBuffers(1, colour_only);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glDisable(GL_BLEND);
		gl.glUseProgram(_frame_copy_program.program);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, _frame_colour[now]);
		gl.glUniform1i(UniformOf(_frame_copy_program.program, "u_colour"), 0);
		gl.glBindVertexArray(_frame_empty_vao);
		const bool first = FirstDraw("snimku z oci: kopie k ukazani");
		gl.glDrawArrays(GL_TRIANGLES, 0, 3);
		FirstDrawDone(first);
		gl.glBindVertexArray(0);
		gl.glBindTexture(GL_TEXTURE_2D, 0);
		gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
		Step(fmt::format("snimek z oci {} hotov na karte (glFinish)", _gpu_pictures + 1));
		gl.glFinish();
		f.shown = _frame_shown[k];
		f.pixels.clear();
	} else {
		f.pixels.resize(static_cast<size_t>(w) * h);
		Step(fmt::format("cteni snimku z oci {} zpet (glReadPixels)", _gpu_pictures + 1));
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_pixel_fbo[now]);
		gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
		gl.glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, f.pixels.data());
		gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	const GLenum error = gl.glGetError();
	if (error != 0) {
		Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x} in the view from the eyes whole, the processor draws it", error);
		if (error == GL_OUT_OF_MEMORY) ForgetCard();
		return false;
	}
	TrimCard();
	return true;
}

/** The card's thread: its context made, then the pictures asked for drawn till the end. */
void GpuThreadMain()
{
	std::string why, describe;
	bool ok = false;
	_diary = FileHandle::Open(DiaryPath(false), "wb");
	_diary_tail = 0;
	_diary_pictures = 0;
	Diary("OpenTTD Decouple: denik graficke karty (3D obrazky)", true);
	try {
		ok = StartCard(why, describe);
	} catch (...) {
		why = "failed at the start";
	}
	{
		std::lock_guard lock(_gpu_mutex);
		_gpu_state = ok ? GpuState::Ready : GpuState::Failed;
		_gpu_describe = ok ? describe : "none: " + why;
		if (!ok && !_gpu_stop) {
			_gpu_failed_why = why;
			_gpu_failed_note = true;
		}
	}
	_gpu_done.notify_all();
	Debug(misc, 1, "3D on the graphics card: {}", ok ? describe : "none, the processor draws (" + why + ")");
	Diary(ok ? "start v poradku: " + describe : "karta nekresli: " + why, true);
	Diary("ceka", false);
	std::unique_lock lock(_gpu_mutex);
	while (ok && !_gpu_stop) {
		if (_gpu_jobs.empty()) {
			_gpu_wake.wait_for(lock, std::chrono::milliseconds(100), [] { return _gpu_stop || !_gpu_jobs.empty(); });
			if (_gpu_jobs.empty()) {
				lock.unlock();
				PumpMessages();
				lock.lock();
			}
			continue;
		}
		GpuJob *job = _gpu_jobs.front();
		_gpu_jobs.pop_front();
		lock.unlock();
		if (job->read != nullptr) {
			if (_view_fence_owed && !MainJobQueued()) ViewPicturesLaid();
			/* Not a picture: a texture read back (a screenshot). */
			const bool read = ReadShownOnCard(*job->read);
			lock.lock();
			job->ok = read;
			job->done = true;
			_gpu_done.notify_all();
			continue;
		}
		const auto start = std::chrono::steady_clock::now();
		bool drawn = false;
		try {
			if (job->frame != nullptr) {
				drawn = DrawEyeFrameOnCard(*job->frame);
			} else if (job->eye != nullptr) {
				drawn = DrawEyeOnCard(*job->eye);
			} else if (job->ground != nullptr) {
				const uint64_t calls = _gl_calls;
				drawn = DrawGroundOnCard(*job->ground);
				_gl_calls_ground += _gl_calls - calls;
				_gl_jobs_ground++;
			} else if (job->standing != nullptr) {
				const uint64_t calls = _gl_calls;
				drawn = DrawStandingOnCard(*job->standing);
				_gl_calls_standing += _gl_calls - calls;
				_gl_jobs_standing++;
			} else {
				DrawPicturesOnCard(job->pics);
				drawn = std::ranges::any_of(job->pics, [](const ThreeDGpuPicture *p) { return p->drawn; });
			}
		} catch (...) {
			drawn = false;
		}
		/* The view's pictures laid by a job before not fenced yet (ViewPicturesLaid()): now, unless another of the main
		 * thread's jobs comes next -- before this one is told done, so the screen is never shown without it. */
		if (_view_fence_owed && !MainJobQueued()) ViewPicturesLaid();
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (job->ground != nullptr || job->standing != nullptr) {
			/* The ground of a block, every frame, and what stands on it: counted apart, in the diary only when the card could not lay it. */
			if (!drawn && _diary_pictures < DIARY_PICTURES) {
				_diary_pictures++;
				if (job->ground != nullptr) {
					Diary(fmt::format("zem plochy {}x{} kresli procesor ({:.1f} ms)", job->ground->width, job->ground->height, ms), true);
				} else {
					Diary(fmt::format("co stoji na plose {}x{} kresli procesor ({:.1f} ms)", job->standing->width, job->standing->height, ms), true);
				}
			}
			Diary("ceka", false);
			lock.lock();
			job->ok = drawn;
			job->done = true;
			_gpu_ground_ms += ms;
			_gpu_grounds++;
			if (job->ground != nullptr) {
				_gpu_ground_only_ms += ms;
				_gpu_grounds_only++;
			}
			if (!drawn) _gpu_grounds_failed++;
			if (job->owned_ground != nullptr) {
				/* Given without waiting: nobody takes it. */
				if (!drawn) _view_move_failed = true;
				delete job;
				continue;
			}
			if (job->owned_standing != nullptr) {
				/* What stands given without waiting: the main thread's (ThreeDGpuStandingSettle()). Its vehicles' places
				 * free to be given to others again; the last of them done, what was not drawn for long may go now. */
				if (!job->pinned.empty()) {
					std::lock_guard kept_lock(_standing_kept_mutex);
					for (uint64_t key : job->pinned) {
						auto it = _standing_pinned.find(key);
						if (it != _standing_pinned.end() && --it->second <= 0) _standing_pinned.erase(it);
					}
				}
				const bool last = --_standing_async_pending == 0;
				_gpu_done.notify_all();
				if (last) {
					lock.unlock();
					TrimCard();
					lock.lock();
				}
				continue;
			}
			_gpu_done.notify_all();
			continue;
		}
		if (_diary_pictures < DIARY_PICTURES) {
			_diary_pictures++;
			Diary(fmt::format("obrazek {} {} ({:.1f} ms)", _gpu_pictures + 1, drawn ? "hotov" : "kresli procesor", ms), true);
		}
		Diary("ceka", false);
		lock.lock();
		job->ok = drawn;
		job->done = true;
		_gpu_ms += ms;
		if (job->pics.empty()) {
			_gpu_pictures++;
			if (!drawn) _gpu_failed++;
		} else {
			_gpu_pictures += job->pics.size();
			_gpu_failed += std::ranges::count_if(job->pics, [](const ThreeDGpuPicture *p) { return !p->drawn; });
		}
		_gpu_done.notify_all();
	}
	for (GpuJob *job : _gpu_jobs) {
		if (job->owned_ground != nullptr) {
			delete job;
		} else {
			/* (One given without waiting, not laid: ThreeDGpuStandingSettle() takes it.) */
			if (job->owned_standing != nullptr) _standing_async_pending--;
			job->done = true;
		}
	}
	_gpu_jobs.clear();
	_gpu_state = GpuState::Failed;
	_gpu_done.notify_all();
	lock.unlock();
	Step("konec karty");
	EndCard();
	Diary("konec", false);
	_diary.reset();
}

} // namespace

/**
 * The player told once, in a window, that the card could not be started and why: the 3D pictures are the processor's
 * then, slow, the vehicles their stand-in pictures (the player's #361: "the cars are not drawn in time, they use the
 * stand-in 2D picture" -- the card had not started, and nothing in the game said so).
 */
static const IntervalTimer<TimerWindow> _gpu_failed_timer(std::chrono::seconds(2), [](auto) {
	std::string why;
	{
		std::lock_guard lock(_gpu_mutex);
		if (!_gpu_failed_note || FindWindowById(WindowClass::MainWindow, 0) == nullptr) return;
		_gpu_failed_note = false;
		why = _gpu_failed_why;
	}
	Debug(misc, 1, "3D on the graphics card: the player told it did not start ({})", why);
	ShowErrorMessage(GetEncodedString(STR_ERROR_THREE_D_GPU_NOT_STARTED, why), {}, WarningLevel::Warning);
});

uint32_t ThreeDGpuMesh::Glow(size_t texture) const
{
	return this->glow_all != 0 ? this->glow_all : this->shape->Glow(texture);
}

/** Whether the card draws the 3D pictures now, not starting it nor waiting for it (the thread making the pictures of the map). */
bool ThreeDGpuReady()
{
	if (_three_d_gpu_off) return false;
	std::lock_guard lock(_gpu_mutex);
	return _gpu_state == GpuState::Ready;
}

/** Whether the card smooths the roads of the ground of the view from the eyes (SmoothGroundRoads()): not when it was found it cannot. */
bool ThreeDGpuSmoothsRoads()
{
	return _road_smooth_state >= 0;
}

/**
 * Whether the card draws the 3D pictures: the first time asked its thread is started, not waited for -- till its
 * context is made and its programs compiled the processor draws (cota PRO-HRU.md section 7: the first frame of the
 * title waited 11 s for it on the phone). False for good where there is no card to be had, and while the rig has the
 * processor draw.
 * @param wait wait for the start (the rig's 'test3d gpu')
 */
bool ThreeDGpuAvailable(bool wait)
{
	if (_three_d_gpu_off) return false;
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state == GpuState::NotStarted) {
		_gpu_state = GpuState::Starting;
		if (!StartNewThread(&_gpu_thread, "ottd:3d-gpu", &GpuThreadMain)) {
			_gpu_state = GpuState::Failed;
			_gpu_describe = "none: no thread for it";
			return false;
		}
	}
	if (wait) _gpu_done.wait(lock, [] { return _gpu_state != GpuState::Starting; });
	return _gpu_state == GpuState::Ready;
}

/**
 * Whether the card poses the figures on bones (ThreeDBonesFrame; the player, 3. 11.: "2 bones"), as its start tried
 * it (TestBones()): their shapes at rest and their frames' joints put on it, no frame posed by the processor.
 */
bool ThreeDGpuPosesBones()
{
	return _bones_ok && _card_poses_bones && ThreeDGpuAvailable();
}

/**
 * Draw a picture on the card, on whichever thread asks (it waits): its pixels and the lit ones read back.
 * @return false: not drawn (no card, or a picture it cannot hold): the processor draws it
 */
/**
 * Lay the ground of a block of the turned map on the card, on whichever thread asks (it waits): read back into the
 * block's pixels.
 * @return false: not laid (no card, or more than it holds): the processor lays it
 */
bool ThreeDGpuDrawGround(ThreeDGpuGround &ground)
{
	if (!ThreeDGpuAvailable()) return false;
	GpuMainWait waited(ground.move_x != 0 || ground.move_y != 0 ? 0 : 1);
	GpuJob job{};
	job.ground = &ground;
	std::unique_lock lock(_gpu_mutex);
	/* Its thread ended (ThreeDGpuStop()): nobody would take the job (the windows closed at the end, ThreeDGpuForgetView()). */
	if (_gpu_state != GpuState::Ready || _gpu_stop) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
}

/**
 * Move the picture of the view on the card with the view, not waiting (G6; the player, 17. 11.: "zoom out je slabý a
 * cuká": the drag of the zoomed out map stood 13 ms a step on the card for it, the rig at zoom 5 1.3 s in five). The
 * main thread's jobs are taken in the order given (QueueGpuJob()): what it lays next comes after the move.
 * @param move the move (ThreeDGpuGround::move_x, move_y, into_view and the view), copied
 * @return false: not given (no card): the caller lays the view anew
 */
bool ThreeDGpuMoveViewGround(const ThreeDGpuGround &move)
{
	if (!ThreeDGpuAvailable()) return false;
	auto job = std::make_unique<GpuJob>();
	job->owned_ground = std::make_unique<ThreeDGpuGround>(move);
	job->ground = job->owned_ground.get();
	{
		std::lock_guard lock(_gpu_mutex);
		if (_gpu_state != GpuState::Ready || _gpu_stop) return false;
		QueueGpuJob(*job.release());
	}
	_gpu_wake.notify_one();
	return true;
}

/** Whether a move given without waiting (ThreeDGpuMoveViewGround()) was not done since asked last. */
bool ThreeDGpuTakeViewMoveFailed()
{
	return _view_move_failed.exchange(false);
}

std::atomic<bool> _three_d_gpu_driver_waits = false;
std::atomic<bool> _gpu_fence_last_only = true; ///< Rig: the view's pictures fenced only after the last of the main thread's jobs in a row (test3d stojici plot); else after every one
std::atomic<bool> _gpu_standing_lean = true; ///< Rig: what stands laid with the programs' uniforms set once a job (test3d stojici stav); else every building and after it as before
bool _standing_vehicles_first = true; ///< Rig: the vehicles not kept drawn before the view's picture is begun (test3d stojici auta_predem); else each as it comes
std::atomic<bool> _gpu_building_cells_on = true; ///< Rig: a building among what stands drawn only in the cells of it the block reaches into (test3d stojici kostky); else whole

/**
 * Have the video driver's context wait on the card for the view's pictures it shows straight from it to be laid
 * (ViewPicturesLaid()): called by it before it shows them, with the way it waits for a fence (glWaitSync()).
 * @param wait waits for the fence given, on the card
 */
void ThreeDGpuViewPicturesWait(void (*wait)(void *fence))
{
	std::lock_guard lock(_view_fence_mutex);
	if (_view_fence != nullptr) wait(_view_fence);
}

/**
 * Lay what stands on a block of the turned map on the card, into the picture of the view's standing things (it waits).
 * @return false: not laid (no card, a picture bigger than it holds): the processor draws it
 */
bool ThreeDGpuStandingKeeps(uint64_t key)
{
	std::lock_guard lock(_standing_kept_mutex);
	/* (One whose picture the card could not make it keeps as nothing: not to be given with its shapes again.) */
	return _standing_kept_keys.contains(key) || _standing_failed_keys.contains(key);
}

bool ThreeDGpuDrawStanding(ThreeDGpuStanding &standing)
{
	if (!ThreeDGpuAvailable()) return false;
	GpuMainWait waited(standing.moving ? 4 : 2);
	GpuJob job{};
	job.standing = &standing;
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready || _gpu_stop) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
}

/** The jobs of what stands given without waiting not taken yet (ThreeDGpuStandingSettle()): the main thread's. */
static std::vector<std::unique_ptr<GpuJob>> _standing_async;
/** For the diary: what stands given without waiting, the blocks of it the card did not lay all the same, and why the
 * others were waited for (ThreeDGpuStandingWait). The main thread's. */
static uint64_t _standing_async_given = 0, _standing_async_refused = 0;
static std::array<uint64_t, static_cast<size_t>(ThreeDGpuStandingWait::End)> _standing_async_why{};

/**
 * Whether the card holds all that what stands on a block is laid from, as it is given (ThreeDGpuDrawStandingAsync()):
 * the buildings' shapes and textures, their paints' masks and patterns -- their numbers taken into the job, the
 * pointers to the game's memory not read on the card's thread --, the vehicles' kept pictures (pinned), the pictures
 * no bigger than its atlas. Under _card_trim_mutex: nothing is let go of between this and the job (TrimCard()).
 * @param[out] pinned the keys of the vehicles' pictures pinned for the job
 */
static ThreeDGpuStandingWait StandingAllHeld(ThreeDGpuStanding &s, std::vector<uint64_t> &pinned)
{
	if (s.moving || s.read_lit || s.width <= 0 || s.height <= 0 || !_standing_program_ok) return ThreeDGpuStandingWait::Other;
	int atlas = _standing_atlas_size_now;
	if (const int rig = _rig_standing_atlas_size; rig > 0) atlas = std::min(atlas, rig);
	if (atlas <= 0) return ThreeDGpuStandingWait::Picture;
	auto fits = [atlas](const ThreeDGpuStandingPicture &p) {
		return p.width > 0 && p.height > 0 && p.width <= atlas && p.height <= atlas && p.pixels.size() >= static_cast<size_t>(p.width) * p.height;
	};
	/* Only what the card lays: what reaches into none of the parts it does not even look at (DrawStandingOnCard()) --
	 * a building in the piece about the parts that changed, never put on the card, kept every such piece waiting. */
	const std::vector<std::array<int, 4>> clips = StandingClips(s);
	std::vector<uint8_t> laid(s.items.size(), 0);
	for (size_t k = 0; k < s.items.size(); k++) laid[k] = StandingInSome(s, s.items[k], StandingReach(s, s.items[k]), clips) ? 1 : 0;
	std::vector<uint8_t> building_laid(s.buildings.size(), 0);
	for (size_t k = 0; k < s.items.size(); k++) {
		const ThreeDGpuStandingItem &item = s.items[k];
		if (laid[k] == 0) continue;
		if (item.faces < 0 && item.vehicle == nullptr && item.building >= 0 && static_cast<size_t>(item.building) < s.buildings.size()) building_laid[item.building] = 1;
		if (item.faces >= 0) {
			if (static_cast<size_t>(item.faces) >= s.faces.size() || !_standing_faces_ok) return ThreeDGpuStandingWait::Other;
			const ThreeDGpuStandingFaces &f = s.faces[item.faces];
			if (f.texture != nullptr && !f.corners.empty() && !fits(*f.texture)) return ThreeDGpuStandingWait::Picture;
		} else if (item.vehicle == nullptr && item.building < 0 && item.picture != nullptr && !fits(*item.picture)) {
			return ThreeDGpuStandingWait::Picture;
		}
	}
	{
		std::lock_guard lock(_card_held_mutex);
		for (size_t i = 0; i < s.buildings.size(); i++) {
			if (building_laid[i] == 0) continue;
			ThreeDGpuStandingBuilding &b = s.buildings[i];
			if (b.building.uid == 0 || !_card_held_buildings.contains(b.building.uid | (uint64_t{1} << 63))) return ThreeDGpuStandingWait::Building;
			b.texture_uids.clear();
			for (const ThreeDObjectTexture *t : b.building.textures) {
				if (t != nullptr && !_card_held_textures.contains(t->uid.value)) return ThreeDGpuStandingWait::Texture;
				b.texture_uids.push_back(t != nullptr ? t->uid.value : 0);
			}
			b.paint_uids.clear();
			const size_t parts = std::min(b.building.paints.size(), THREE_D_GPU_BUILDING_PAINTS);
			for (size_t k = 0; k < parts; k++) {
				const ThreeDGpuBuildingPaint &p = b.building.paints[k];
				if (p.mask == nullptr || !_card_held_textures.contains(p.mask->uid.value)) return ThreeDGpuStandingWait::Texture;
				if (p.pattern != nullptr && !_card_held_textures.contains(p.pattern->uid.value)) return ThreeDGpuStandingWait::Texture;
				b.paint_uids.push_back({p.mask->uid.value, p.pattern != nullptr ? p.pattern->uid.value : 0});
			}
		}
	}
	/* The vehicles laid from their kept pictures: found and pinned at once (SweepStandingPlaces() takes the same lock). */
	std::lock_guard lock(_standing_kept_mutex);
	for (size_t k = 0; k < s.items.size(); k++) {
		const ThreeDGpuStandingItem &item = s.items[k];
		if (item.vehicle == nullptr || laid[k] == 0 || !_standing_shrink_ok) continue;
		if (!_standing_kept_keys.contains(item.vehicle->key)) return ThreeDGpuStandingWait::Vehicle;
	}
	for (size_t k = 0; k < s.items.size(); k++) {
		const ThreeDGpuStandingItem &item = s.items[k];
		if (item.vehicle == nullptr || laid[k] == 0 || !_standing_shrink_ok) continue;
		_standing_pinned[item.vehicle->key]++;
		pinned.push_back(item.vehicle->key);
	}
	return ThreeDGpuStandingWait::Given;
}

/**
 * Give what stands on a block of the turned map to the card without waiting for it (the player, 17. 11.: "zoom out je
 * slabý a cuká"; the diary of 18. 11.: at zoom 5 the main thread stood 0.7-1.3 s in 5 s on the card for each block of
 * what stands, only to learn whether it was laid -- the card never said no but where something was not on it yet):
 * when all it is laid from the card holds already (StandingAllHeld()), it cannot say no but for an error of its own
 * (out of memory, everything forgotten), and the main thread goes on as if laid. What the card did not lay all the
 * same the main thread learns at ThreeDGpuStandingSettle(), before the frame is shown. Else nothing is given: the
 * caller lays it waiting (ThreeDGpuDrawStanding()), the card putting what was missing on, or the processor drawing it.
 * @param standing what stands: taken into the job when given (left as it was when not)
 * @return Given, or why not
 */
ThreeDGpuStandingWait ThreeDGpuDrawStandingAsync(ThreeDGpuStanding &standing)
{
	auto told = [](ThreeDGpuStandingWait why) {
		_standing_async_why[static_cast<size_t>(why)]++;
		return why;
	};
	if (std::this_thread::get_id() != _gpu_main_thread || !ThreeDGpuAvailable()) return told(ThreeDGpuStandingWait::Other);
	auto job = std::make_unique<GpuJob>();
	{
		/* Counted before it is looked at: from now on the card lets nothing go (TrimCard()). */
		std::lock_guard trim_lock(_card_trim_mutex);
		_standing_async_pending++;
		const ThreeDGpuStandingWait why = StandingAllHeld(standing, job->pinned);
		if (why != ThreeDGpuStandingWait::Given) {
			if (!job->pinned.empty()) {
				std::lock_guard kept_lock(_standing_kept_mutex);
				for (uint64_t key : job->pinned) {
					if (auto it = _standing_pinned.find(key); it != _standing_pinned.end() && --it->second <= 0) _standing_pinned.erase(it);
				}
			}
			_standing_async_pending--;
			return told(why);
		}
	}
	job->owned_standing = std::make_unique<ThreeDGpuStanding>(std::move(standing));
	job->owned_standing->held_only = true;
	job->standing = job->owned_standing.get();
	{
		std::lock_guard lock(_gpu_mutex);
		if (_gpu_state != GpuState::Ready || _gpu_stop) {
			/* Not taken: given back as it was, laid waiting (nothing will come of it). */
			standing = std::move(*job->owned_standing);
			standing.held_only = false;
			std::lock_guard kept_lock(_standing_kept_mutex);
			for (uint64_t key : job->pinned) {
				if (auto it = _standing_pinned.find(key); it != _standing_pinned.end() && --it->second <= 0) _standing_pinned.erase(it);
			}
			_standing_async_pending--;
			return told(ThreeDGpuStandingWait::Other);
		}
		QueueGpuJob(*job);
	}
	_gpu_wake.notify_one();
	_standing_async.push_back(std::move(job));
	_standing_async_given++;
	return told(ThreeDGpuStandingWait::Given);
}

/**
 * Wait for what stands given without waiting (ThreeDGpuDrawStandingAsync()) to be laid -- before the frame is shown,
 * its last job fenced (ViewPicturesLaid()) -- and let go of it. The main thread's.
 * @param[out] refused the blocks of it the card did not lay (cleared there): the caller has them drawn again
 * @return how many those are
 */
size_t ThreeDGpuStandingSettle(std::vector<ThreeDGpuStandingRefused> &refused)
{
	if (_standing_async.empty()) return 0;
	{
		std::unique_lock lock(_gpu_mutex);
		if (!std::ranges::all_of(_standing_async, [](const auto &job) { return job->done; })) {
			GpuMainWait waited(5);
			_gpu_done.wait(lock, [] { return std::ranges::all_of(_standing_async, [](const auto &job) { return job->done; }); });
		}
	}
	size_t n = 0;
	for (const auto &job : _standing_async) {
		if (job->ok) continue;
		const ThreeDGpuStanding &s = *job->owned_standing;
		refused.push_back({s.view, s.view_x, s.view_y, s.width, s.height});
		n++;
	}
	_standing_async_refused += n;
	_standing_async.clear();
	return n;
}

/** The one ground job given without waiting (ThreeDGpuDrawGroundStart()), until it is taken; the main thread's only. */
static std::unique_ptr<GpuJob> _ground_job;

/**
 * Give the card the ground of a block of the turned map without waiting: the asker collects and sorts the block's
 * objects meanwhile, and takes the laid ground with ThreeDGpuDrawGroundTake() before anything writes into the block
 * (GPU point 3 without the stall; the player's #299: the ground held the main thread 100 and more ms a redraw). One
 * such job at a time; the ground's pieces, its textures and the block's pixels must stand until the take.
 * @return false: no card (nothing given): the processor lays it now
 */
bool ThreeDGpuDrawGroundStart(ThreeDGpuGround &ground)
{
	assert(_ground_job == nullptr);
	if (!ThreeDGpuAvailable()) return false;
	auto job = std::make_unique<GpuJob>();
	job->ground = &ground;
	{
		std::lock_guard lock(_gpu_mutex);
		if (_gpu_state != GpuState::Ready) return false;
		QueueGpuJob(*job);
	}
	_gpu_wake.notify_one();
	_ground_job = std::move(job);
	return true;
}

/**
 * Wait for the ground given with ThreeDGpuDrawGroundStart() and let go of it.
 * @return whether the card laid it; false (it could not), the processor lays it
 */
bool ThreeDGpuDrawGroundTake()
{
	if (_ground_job == nullptr) return false;
	GpuMainWait waited(3);
	bool ok;
	{
		std::unique_lock lock(_gpu_mutex);
		_gpu_done.wait(lock, [] { return _ground_job->done; });
		ok = _ground_job->ok;
	}
	_ground_job.reset();
	return ok;
}

/** The rig ('test3d smycka'): how long the main thread has stood waiting for the card since the game started, ms: of a slow frame, how much of it was that. */
double ThreeDGpuMainWaitedMs()
{
	return _gpu_main_wait_all_ms;
}

bool ThreeDGpuRender(std::span<ThreeDGpuPicture *const> pics)
{
	for (ThreeDGpuPicture *pic : pics) pic->drawn = false;
	if (pics.empty() || !ThreeDGpuAvailable()) return false;
	GpuJob job{pics};
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
}

/**
 * Draw the objects of the view from the eyes on the card, on whichever thread asks (it waits): their pixels and depths
 * read back.
 * @return false: not drawn (no card, or something it cannot hold): the processor draws them
 */
bool ThreeDGpuRenderEye(ThreeDGpuEyePicture &pic)
{
	if (!ThreeDGpuAvailable()) return false;
	GpuJob job{{}, &pic};
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
}

/**
 * Draw the whole view from the eyes on the card, on whichever thread asks (it waits): its colours read back.
 * @return false: not drawn (no card, or something it cannot hold): the processor draws it
 */
bool ThreeDGpuRenderEyeFrame(ThreeDGpuEyeFrame &frame)
{
	if (!ThreeDGpuAvailable()) return false;
	GpuJob job{{}, nullptr, &frame};
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
}

/**
 * A picture shown straight from the card read back (ThreeDGpuEyeFrame::shown): for a screenshot of the screen.
 * @param texture the picture's
 * @param width its size
 * @param height
 * @param[out] pixels its colours (Colour::data), top row first
 * @return whether it was read
 */
bool ThreeDGpuReadShown(uint32_t texture, int width, int height, std::vector<uint32_t> &pixels)
{
	if (texture == 0 || width <= 0 || height <= 0) return false;
	ShownRead read{texture, width, height, &pixels};
	GpuJob job{};
	job.read = &read;
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
}

/**
 * The video driver's OpenGL context, made now (current on the thread calling): the card's shares its things with it,
 * and the view from the eyes is shown straight from the card (ThreeDGpuShows()). Before the card's thread starts; or
 * after, when the driver made its context sharing with the card's (ThreeDGpuOwnContext()).
 * @param display a WGL context's device; with EGL nullptr: the current display
 * @param context the WGL context; with EGL nullptr: the current context
 * @param shared_with_card the driver's context shares the card's already
 */
void ThreeDGpuVideoContext(void *display, void *context, bool shared_with_card)
{
#ifdef THREE_D_GPU_EGL
	if (context == nullptr) {
		/* The video driver's context of EGL, when it has one (SDL may have GLX): its display and it. */
		static LibraryLoader lib("libEGL.so.1");
		if (lib.HasError()) return;
		EGLDisplay (*current_display)() = lib.GetFunction("eglGetCurrentDisplay");
		EGLContext (*current_context)() = lib.GetFunction("eglGetCurrentContext");
		if (current_display == nullptr || current_context == nullptr) return;
		display = current_display();
		context = current_context();
		if (display == nullptr || context == nullptr) return;
	}
#endif
#ifdef THREE_D_GPU_WGL
	if (context == nullptr) {
		/* The current one (SDL's). */
		display = wglGetCurrentDC();
		context = wglGetCurrentContext();
		if (display == nullptr || context == nullptr) return;
	}
#endif
	std::lock_guard lock(_video_mutex);
	_video_display = display;
	_video_context = context;
	if (shared_with_card) _video_shared = true;
	Debug(misc, 1, "3D on the graphics card: the video driver's OpenGL {}", shared_with_card ? "shares the card's" : "is there to share");
}

/** The video driver's OpenGL context goes (DestroyContext()): what the card makes is read back again. */
void ThreeDGpuVideoContextGone()
{
	std::lock_guard lock(_video_mutex);
	_video_display = nullptr;
	_video_context = nullptr;
	_video_shared = false;
}

/** The card's context, for the video driver's made anew to share its things with it (WGL); nullptr: none yet. */
void *ThreeDGpuOwnContext()
{
#ifdef THREE_D_GPU_WGL
	std::lock_guard lock(_gpu_mutex);
	return _gpu_state == GpuState::Ready && !_gpu_no_share ? static_cast<void *>(_gpu_rc) : nullptr;
#else
	return nullptr;
#endif
}

/** Whether the view from the eyes is shown straight from the card: its context and the video driver's share their things. */
bool ThreeDGpuShows()
{
	return _three_d_gpu_show && !_three_d_gpu_off && _video_shared && _frame_programs;
}

/**
 * The picture of the turned main view's ground the card lays blocks into (ThreeDGpuGround::into_view), a texture of the
 * video driver's OpenGL too, its top row first.
 * @param made how many times it was made anew or let go: another than before, it holds nothing laid before
 * @return 0: none
 */
uint32_t ThreeDGpuViewGround(int view, uint64_t *made)
{
	const ViewPictures &v = ViewPicturesOf(view);
	if (made != nullptr) *made = v.made;
	return v.ground_name;
}

/** The picture of what stands on a view on the card (ThreeDGpuStanding), the card's texture, 0 none; and which of its pixels are lit. */
uint32_t ThreeDGpuViewStanding(int view, uint32_t *lit, uint32_t *order)
{
	const ViewPictures &v = ViewPicturesOf(view);
	/* How near each pixel of what stands is (its order), the moving things' none. */
	if (order != nullptr) *order = v.standing_order_name;
	/* With the moving things over it, once they were laid (ThreeDGpuMovingLayer()). */
	if (v.moving_on && v.moving_name != 0 && v.moving_lit_name != 0) {
		if (lit != nullptr) *lit = v.moving_lit_name;
		return v.moving_name;
	}
	if (lit != nullptr) *lit = v.standing_lit_name;
	return v.standing_name;
}

/**
 * Whether what stands on a view is shown with its moving things laid over it each frame (ThreeDGpuStanding::moving):
 * then what stands is laid without them, and the picture of them over it shown once they were laid; else as before,
 * what stands with them.
 * @param view the view (ThreeDGpuGround::view)
 * @param on the moving layer on
 */
void ThreeDGpuMovingLayer(int view, bool on)
{
	ViewPictures &v = ViewPicturesOf(view);
	if (on && !v.moving_on) v.moving_anew = true;
	v.moving_on = on;
}

/** For the diary: the moving things laid over the views since the last time (ThreeDGpuStanding::moving), counted anew. */
void RigThreeDGpuAtlas(int px)
{
	_rig_standing_atlas_size = px;
}

/** What the main thread waits on the card for (GpuMainWait), by its kind. */
static const char *const GPU_WAIT_NAMES[GPU_WAIT_KINDS] = {"posun pohledu", "zem bloku", "co stoji", "zem predem dana", "vrstva aut", "co stoji dane bez cekani"};

/**
 * The watch of a freeze (the player, 19. 11., #382: "zamrzlo to při pohledu shora při maximálním oddálení při posunu
 * mapy" -- the card's diary ended at a step of the ground's texture, the frames' diary five seconds before; who
 * waited on whom, and whether for ever, nothing said). The main thread calls this as it draws a frame; a frame not
 * drawn for 8, 30 and 120 seconds is written into zamrznuti.log: what the main thread waits on the card for and how
 * long, the step the card's thread took last and when, the game's last marks (CrashTrace()) -- and that the game went
 * on, when it did. Nothing is ended: the diary only.
 */
void ThreeDGpuWatchFrame()
{
	static std::atomic<int64_t> frame_at = 0;
	static std::atomic<bool> started = false;
	frame_at = WatchNow();
	if (started.exchange(true)) return;
	std::thread([] {
		static constexpr int64_t LEVELS[] = {8000, 30000, 120000};
		auto line = [](std::string_view text) {
			auto f = FileHandle::Open(FioDiaryPath("zamrznuti.log"), "a");
			if (!f.has_value()) return;
			const std::time_t tt = std::time(nullptr);
			std::tm tm{};
#if defined(_WIN32)
			localtime_s(&tm, &tt);
#else
			localtime_r(&tt, &tm);
#endif
			fmt::print(*f, "{:02}:{:02}:{:02} {}\n", tm.tm_hour, tm.tm_min, tm.tm_sec, text);
			fflush(*f);
		};
		size_t told = 0;      ///< how many of the levels of this stall were written
		int64_t told_at = 0;  ///< the frame the stall written began after
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			const int64_t now = WatchNow(), at = frame_at.load();
			if (told > 0 && at != told_at) {
				line(fmt::format("hra pokracuje: dalsi snimek po {:.1f} s", (at - told_at) / 1000.0));
				told = 0;
			}
			if (told >= std::size(LEVELS) || now - at < LEVELS[told]) continue;
			told_at = at;
			told++;
			std::string step;
			{
				std::lock_guard lock(_watch_step_mutex);
				step = _watch_step;
			}
			const int kind = _watch_wait_kind.load();
			line(fmt::format("ZAMRZLO? hlavni vlakno nenakreslilo snimek {:.0f} s; {}; karta: posledni krok \"{}\" pred {:.1f} s; posledni znacky hry:",
					(now - at) / 1000.0,
					kind >= 0 && kind < GPU_WAIT_KINDS ? fmt::format("ceka na kartu ({}) {:.1f} s", GPU_WAIT_NAMES[kind], (now - _watch_wait_from.load()) / 1000.0) : std::string("na kartu neceka"),
					step, (now - _watch_step_at.load()) / 1000.0));
			const std::vector<std::string> marks = GetCrashTrace();
			for (size_t i = marks.size() > 16 ? marks.size() - 16 : 0; i < marks.size(); i++) line("  " + marks[i]);
		}
	}).detach();
}

/** For the diary: the main thread's waits on the card since asked last (GpuMainWait), by what it waited for. */
std::string ThreeDGpuMainWaits()
{
	const char *const *names = GPU_WAIT_NAMES;
	std::string s;
	for (int k = 0; k < GPU_WAIT_KINDS; k++) {
		const uint32_t n = _gpu_main_waits[k].exchange(0);
		const double ms = _gpu_main_wait_ms[k].exchange(0);
		if (n > 0) s += fmt::format("{}{} {}x {:.0f} ms", s.empty() ? "" : ", ", names[k], n, ms);
	}
	/* What stands given without waiting, what of it the card did not lay all the same, and why the rest was waited for. */
	const auto &why = _standing_async_why;
	const uint64_t waited = why[1] + why[2] + why[3] + why[4] + why[5];
	if (_standing_async_given + waited > 0) {
		s += fmt::format("{}co stoji bez cekani {} (karta pozde nepolozila {}, auto uz nedrzela {}), s cekanim {} (karta nema budovu {}, texturu {}, auto {}, obrazek {}, jine {})", s.empty() ? "" : "; ",
				_standing_async_given, _standing_async_refused, _standing_async_missed.exchange(0), waited, why[1], why[2], why[3], why[4], why[5]);
	}
	_standing_async_given = _standing_async_refused = 0;
	_standing_async_why.fill(0);
	return s.empty() ? "-" : s;
}

std::string ThreeDGpuMovingTimes()
{
	const uint64_t passes = _gpu_moving_passes.exchange(0), things = _gpu_moving_things.exchange(0), copied = _gpu_moving_copied.exchange(0);
	const double ms = _gpu_moving_ms.exchange(0), copy_ms = _gpu_moving_copy_ms.exchange(0), draw_ms = _gpu_moving_draw_ms.exchange(0);
	const uint64_t drawn = _gpu_moving_drawn.exchange(0), first = _gpu_moving_first.exchange(0);
	const double first_ms = _gpu_moving_first_ms.exchange(0);
	if (passes == 0) return "-";
	return fmt::format("{}x, veci {}, kopie {:.1f} Mpx, karta {:.0f} ms (z toho kopie {:.0f} ms, {} aut predem {:.0f} ms, {} aut nakresleno znovu {:.0f} ms), drzene obrazky: mist dano znovu {}, vsechny zahozeny {}x, zaplneno nejvic auty a postavami {:.1f} %, obrazky {:.1f} %, volnych mist {:.1f} %, mist {}",
			passes, things, copied / 1e6, ms, copy_ms, first, first_ms, drawn, draw_ms, _standing_places_reused.exchange(0), _standing_places_cleared.exchange(0),
			_standing_fill_vehicles.exchange(0) / 10.0, _standing_fill_pictures.exchange(0) / 10.0, _standing_fill_free.exchange(0) / 10.0, _standing_fill_places.exchange(0));
}

/**
 * A small view's pictures let go of on the card (it closed, or another view takes its number), with its kept
 * ground's texture; it waits. The video driver shows them no more before (VideoDriver::HideThreeDPicture()).
 * @param view its number (ThreeDGpuGround::view)
 * @param ground_key its kept ground's texture (ThreeDGpuGroundTexture::key), 0 none
 */
void ThreeDGpuForgetView(int view, uint64_t ground_key)
{
	if (view <= 0 || view >= THREE_D_GPU_VIEWS) return;
	ThreeDGpuGround forget{};
	forget.view = view;
	forget.forget = true;
	forget.forget_key = ground_key;
	ThreeDGpuDrawGround(forget);
}

/** The card's thread ended, at the end of the game (after the threads asking it, ShutdownGame()). */
void ThreeDGpuStop()
{
	{
		std::lock_guard lock(_gpu_mutex);
		_gpu_stop = true;
		if (_gpu_state == GpuState::NotStarted) _gpu_state = GpuState::Failed;
	}
	_gpu_wake.notify_all();
	if (_gpu_thread.joinable()) _gpu_thread.join();
	/* What stands given without waiting, told done by the card's thread as it ended: let go of here, on the main thread. */
	_standing_async.clear();
}

/** What draws the 3D pictures: the card, or why not. */
std::string ThreeDGpuDescribe()
{
	std::lock_guard lock(_gpu_mutex);
	if (!_gpu_died_at.empty() && _three_d_gpu_off) return fmt::format("the processor (the card brought the game down the last time, at \"{}\")", _gpu_died_at);
	if (_three_d_gpu_off) return "the processor (the card switched off)";
	return _gpu_describe;
}

/** How much the card holds now (the shapes, triangles, textures and buildings put on it), megabytes. */
size_t ThreeDGpuHeldMB()
{
	return _card_bytes.load() >> 20;
}

/** The rig: the pictures the card drew since asked last, and its time. */
std::string ThreeDGpuTimes()
{
	std::lock_guard lock(_gpu_mutex);
	const int64_t biggest = _card_biggest.exchange(0);
	std::string s = fmt::format("card: {} pictures in {:.1f} ms{}, holds {} MB (shapes {}, triangles {}, textures {}, buildings {}), {} MB put on it (shapes {}, triangles {}, textures {}, buildings {}), let go {}x, read back at most {}x{}",
			_gpu_pictures, _gpu_ms, _gpu_failed > 0 ? fmt::format(", {} left to the processor", _gpu_failed) : std::string(), _card_bytes >> 20,
			_card_bytes_of[0].load() >> 20, _card_bytes_of[1].load() >> 20, _card_bytes_of[2].load() >> 20, _card_bytes_of[3].load() >> 20, _card_sent.load() >> 20,
			_card_sent_of[0].load() >> 20, _card_sent_of[1].load() >> 20, _card_sent_of[2].load() >> 20, _card_sent_of[3].load() >> 20, _card_trims.load(), biggest >> 32, biggest & 0xFFFFFFFF);
	for (std::atomic<size_t> &b : _card_sent_of) b = 0;
	s += fmt::format(", of it the ground {} MB and pictures of what stands {} MB", _card_sent_ground.exchange(0) >> 20, _card_sent_standing.exchange(0) >> 20);
	{
		const uint64_t jg = _gl_jobs_ground.exchange(0), js = _gl_jobs_standing.exchange(0);
		const uint64_t cg = _gl_calls_ground.exchange(0), cs = _gl_calls_standing.exchange(0);
		const uint64_t nb = _gl_buildings.exchange(0), cb = _gl_calls_buildings.exchange(0);
		const uint64_t nv = _gl_vehicles.exchange(0), cv = _gl_calls_vehicles.exchange(0);
		s += fmt::format("; OpenGL calls {} (ground {} in {} jobs, {} a job; what stands {} in {} jobs, {} a job, buildings {} with {} calls, {} a building; vehicles drawn and shrunk {} with {} calls, {} a vehicle, of them drawing {}, in {} batches; by the function {})", _gl_calls.exchange(0),
				cg, jg, jg > 0 ? cg / jg : 0, cs, js, js > 0 ? cs / js : 0, nb, cb, nb > 0 ? cb / nb : 0, nv, cv, nv > 0 ? cv / nv : 0, _gl_calls_vehicles_draw.exchange(0), _gl_vehicle_batches.exchange(0),
				_gl_count_by_function ? VehicleCallsForDiary() : std::string("-"));
	}
	if (_gpu_grounds > 0) s += fmt::format("; the ground and what stands of {} blocks in {:.1f} ms (the ground alone {} in {:.1f} ms){}", _gpu_grounds, _gpu_ground_ms, _gpu_grounds_only, _gpu_ground_only_ms,
			_gpu_grounds_failed > 0 ? fmt::format(", {} left to the processor", _gpu_grounds_failed) : std::string());
	if (_gpu_standing_vehicles + _gpu_standing_vehicles_kept > 0) s += fmt::format("; vehicles of the turned map {} drawn, {} as kept", _gpu_standing_vehicles, _gpu_standing_vehicles_kept);
	if (_gpu_eye_batches + _gpu_eye_single > 0) s += fmt::format("; from the eyes {} meshes in {} batches, {} each on its own, the calls by the function {}", _gpu_eye_instances, _gpu_eye_batches, _gpu_eye_single,
			_gl_count_by_function ? VehicleCallsForDiary(true) : std::string("-"));
	if (const uint64_t later = _gpu_eye_later.exchange(0); later > 0) s += fmt::format(", {} new left for a later picture", later);
	{
		/* A picture from the eyes as the card drew it: its triangles and draw calls, the batches' objects, the most drawn. */
		std::lock_guard most_lock(_gpu_eye_most_mutex);
		if (_gpu_eye_frames > 0) {
			const double f = static_cast<double>(_gpu_eye_frames);
			s += fmt::format("; a picture from the eyes {:.0f} thousand pixels, {:.0f} thousand triangles in {:.0f} draw calls, {:.1f} steps of objects, batches of {:.0f} objects in {:.0f}",
					_gpu_eye_pixels / f / 1000.0, _gpu_eye_triangles / f / 1000.0, _gpu_eye_calls / f, _gpu_eye_steps / f, _gpu_eye_kinds / f, _gpu_eye_batches / f);
			s += fmt::format(", the card's calls a picture: triangles' runs {:.0f}, buildings {:.0f}, vehicles and figures {:.0f}, their glass {:.0f}",
					_gpu_eye_calls_of[0].exchange(0) / f, _gpu_eye_calls_of[1].exchange(0) / f, _gpu_eye_calls_of[2].exchange(0) / f, _gpu_eye_calls_of[3].exchange(0) / f);
			std::vector<std::pair<uint64_t, const std::string *>> most;
			for (const auto &[name, d] : _gpu_eye_most) most.emplace_back(d.first, &name);
			const size_t shown = std::min<size_t>(5, most.size());
			std::partial_sort(most.begin(), most.begin() + shown, most.end(), std::greater<>());
			for (size_t i = 0; i < shown; i++) {
				const auto &d = _gpu_eye_most[*most[i].second];
				s += fmt::format("{} {} ({:.0f}x, {:.0f} thousand triangles)", i == 0 ? ", most" : ",", *most[i].second, d.second / f, d.first / f / 1000.0);
			}
		}
		_gpu_eye_frames = _gpu_eye_triangles = _gpu_eye_calls = _gpu_eye_kinds = _gpu_eye_steps = _gpu_eye_pixels = 0;
		_gpu_eye_most.clear();
	}
	_gpu_eye_batches = _gpu_eye_instances = _gpu_eye_single = 0;
	if (_gpu_standing_triangles[1] > 0) s += fmt::format("; of the buildings' {} triangles in the blocks {} drawn, in the cells there", _gpu_standing_triangles[1], _gpu_standing_triangles[0]);
	const uint64_t standing_failed = _gpu_standing_failed[0] + _gpu_standing_failed[1] + _gpu_standing_failed[2] + _gpu_standing_failed[3];
	if (standing_failed > 0) {
		s += fmt::format("; what stands on {} blocks left to the processor (a box {}, a building's shape {}, a building's texture {}, a picture {})",
				standing_failed, _gpu_standing_failed[0], _gpu_standing_failed[1], _gpu_standing_failed[2], _gpu_standing_failed[3]);
	}
	for (uint64_t &f : _gpu_standing_failed) f = 0;
	_gpu_standing_vehicles = 0;
	_gpu_standing_vehicles_kept = 0;
	_gpu_standing_triangles[0] = _gpu_standing_triangles[1] = 0;
	_gpu_ground_ms = 0;
	_gpu_grounds = 0;
	_gpu_ground_only_ms = 0;
	_gpu_grounds_only = 0;
	_gpu_grounds_failed = 0;
	_card_sent = 0;
	_card_trims = 0;
	_gpu_ms = 0;
	_gpu_pictures = 0;
	_gpu_failed = 0;
	return s;
}

/** The rig ('test3d gpu volani [oci]'): CallsByFunction() of the turned map's vehicles or of the view from the eyes. */
std::string ThreeDGpuCallsByFunction(int on, bool eye)
{
	return CallsByFunction(on, eye);
}

#else /* no card */

std::string ThreeDGpuCallsByFunction(int, bool) { return "bez karty"; }
bool ThreeDGpuAvailable(bool) { return false; }
bool ThreeDGpuPosesBones() { return false; }
bool ThreeDGpuSmoothsRoads() { return false; }
bool ThreeDGpuRenderEye(ThreeDGpuEyePicture &) { return false; }
bool ThreeDGpuRenderEyeFrame(ThreeDGpuEyeFrame &) { return false; }
void ThreeDGpuVideoContext(void *, void *, bool) {}
void ThreeDGpuVideoContextGone() {}
void *ThreeDGpuOwnContext() { return nullptr; }
bool ThreeDGpuShows() { return false; }
uint32_t ThreeDGpuViewGround(int, uint64_t *made) { if (made != nullptr) *made = 0; return 0; }
uint32_t ThreeDGpuViewStanding(int, uint32_t *lit, uint32_t *order) { if (lit != nullptr) *lit = 0; if (order != nullptr) *order = 0; return 0; }
void ThreeDGpuMovingLayer(int, bool) {}
std::string ThreeDGpuMovingTimes() { return "-"; }
std::string ThreeDGpuMainWaits() { return "-"; }
void RigThreeDGpuAtlas(int) {}
void ThreeDGpuForgetView(int, uint64_t) {}
bool ThreeDGpuDrawStanding(ThreeDGpuStanding &) { return false; }
ThreeDGpuStandingWait ThreeDGpuDrawStandingAsync(ThreeDGpuStanding &) { return ThreeDGpuStandingWait::Other; }
size_t ThreeDGpuStandingSettle(std::vector<ThreeDGpuStandingRefused> &) { return 0; }
bool ThreeDGpuStandingKeeps(uint64_t) { return false; }
void ThreeDGpuViewPicturesWait(void (*)(void *)) {}
std::atomic<bool> _three_d_gpu_driver_waits = false;
std::atomic<bool> _gpu_standing_lean = true;
bool _standing_vehicles_first = true;
std::atomic<bool> _gpu_fence_last_only = true;
std::atomic<bool> _gpu_building_cells_on = true; ///< Rig: a building among what stands drawn only in the cells of it the block reaches into (test3d stojici kostky); else whole
std::atomic<int> _rig_standing_refuse = 0;
bool ThreeDGpuReady() { return false; }
bool ThreeDGpuReadShown(uint32_t, int, int, std::vector<uint32_t> &) { return false; }
bool ThreeDGpuRender(std::span<ThreeDGpuPicture *const>) { return false; }
double ThreeDGpuMainWaitedMs() { return 0; }
bool ThreeDGpuDrawGround(ThreeDGpuGround &) { return false; }
bool ThreeDGpuMoveViewGround(const ThreeDGpuGround &) { return false; }
bool ThreeDGpuTakeViewMoveFailed() { return false; }
void ThreeDGpuStop() {}
std::string ThreeDGpuDescribe() { return "the processor (no card on this system)"; }
std::string ThreeDGpuTimes() { return "card: none"; }
size_t ThreeDGpuHeldMB() { return 0; }

#endif
