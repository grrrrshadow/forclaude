/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
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

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(_WIN32) && defined(WITH_OPENGL)
#	define THREE_D_GPU_WGL
#	include <windows.h>
#elif !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#	define THREE_D_GPU_EGL
#	include "library_loader.h"
#endif

#include "safeguards.h"

std::atomic<bool> _three_d_gpu_off = true;
std::atomic<bool> _three_d_gpu_show = true;

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
	return _personal_dir + (last ? "3d_karta_minule.log" : "3d_karta.log");
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

/** A step with the card about to be taken: kept while the start and the first pictures are written step by step. */
[[maybe_unused]] static void Step(std::string_view what)
{
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
			const uint32_t at = static_cast<uint32_t>(made->indices.size());
			if (!runs.empty() && runs.back().texture == ti && runs.back().first + runs.back().count == at) {
				runs.back().count += 3;
			} else {
				runs.push_back({at, 3, ti});
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

#if defined(THREE_D_GPU_WGL) || defined(THREE_D_GPU_EGL)

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
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
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
constexpr GLenum GL_R8 = 0x8229;
constexpr GLenum GL_R32F = 0x822E;
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE1 = 0x84C1;
constexpr GLenum GL_TEXTURE2 = 0x84C2;
constexpr GLenum GL_TEXTURE3 = 0x84C3;
constexpr GLenum GL_MAX_RENDERBUFFER_SIZE = 0x84E8;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
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
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;

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
	X(void, glGenRenderbuffers, (GLsizei n, GLuint *rbs)) \
	X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *rbs)) \
	X(void, glBindRenderbuffer, (GLenum target, GLuint rb)) \
	X(void, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h)) \
	X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
	X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
	X(void, glBindVertexArray, (GLuint array)) \
	X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
	X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers)) \
	X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
	X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
	X(void, glEnableVertexAttribArray, (GLuint index)) \
	X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
	X(void, glVertexAttrib3f, (GLuint index, GLfloat x, GLfloat y, GLfloat z)) \
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
	X(void, glUniform1f, (GLint location, GLfloat v))

/** The functions of OpenGL, of the card's context (only its thread calls them). */
struct GlFunctions {
#define THREE_D_GL_MEMBER(ret, name, args) ret (THREE_D_GL_API *name) args = nullptr;
	THREE_D_GL_FUNCTIONS(THREE_D_GL_MEMBER)
#undef THREE_D_GL_MEMBER
};
GlFunctions gl;

/** Any function, before it is cast to what it is. */
using GpuProc = void (*)();

/* The card's thread and what is asked of it. */
enum class GpuState { NotStarted, Starting, Ready, Failed };
struct GpuJob {
	std::span<ThreeDGpuPicture *const> pics; ///< pictures drawn at once (DrawPicturesOnCard()); else
	ThreeDGpuEyePicture *eye = nullptr; ///< the view from the eyes (DrawEyeOnCard())
	ThreeDGpuEyeFrame *frame = nullptr; ///< else the view from the eyes whole (DrawEyeFrameOnCard())
	struct ShownRead *read = nullptr;   ///< else a picture shown straight from the card read back (ReadShownOnCard())
	std::span<ThreeDGpuBuildingPicture *const> buildings; ///< else the buildings of the turned map (DrawBuildingsOnCard())
	bool done = false;
	bool ok = false;
};
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
		_gpu_jobs.push_front(&job);
	} else {
		_gpu_jobs.push_back(&job);
	}
}
bool _gpu_stop = false;
std::thread _gpu_thread;
/* The video driver's OpenGL (ThreeDGpuVideoContext()): an EGL display and context, or a WGL device and context. The
 * card's context made sharing its things with it (_video_shared), the view from the eyes is shown straight from the
 * card: the driver draws the card's texture where the picture is (ThreeDGpuEyeFrame::shown). */
std::mutex _video_mutex;
void *_video_display = nullptr;
void *_video_context = nullptr;
std::atomic<bool> _video_shared = false;
std::string _gpu_describe = "not started";
double _gpu_ms = 0;                 ///< the time the card took for the pictures since the last ThreeDGpuTimes()
uint64_t _gpu_pictures = 0;         ///< and how many
uint64_t _gpu_failed = 0;           ///< and how many it could not draw (the processor did)

/* What the card keeps, by the thing's number (ThreeDUniqueId): the corners of the shapes, their triangles, the
 * textures; let go of the longest not drawn when it holds more than GPU_BYTES_KEPT. Only the card's thread uses these. */
struct GpuShape {
	GLuint vao = 0, pos = 0, nrm = 0, uv = 0, wheel = 0;
	bool has_nrm = false;
	bool has_wheel = false; ///< each corner's turning wheel (ThreeDVehicleShape::corner_wheel) as attribute 3
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
};
std::unordered_map<uint64_t, GpuShape> _card_shapes;
std::unordered_map<uint64_t, GpuIndices> _card_indices;
/* A building of the view from the eyes (DrawEyeOnCard()): each triangle three corners of its own -- x, y, z, u, v and
 * the triangle's light, its corners' mean as the processor shades it --, by texture and side facing out. */
struct GpuBuilding {
	GLuint vao = 0, vbo = 0;
	struct Run {
		GLint first = 0;
		GLsizei count = 0;
		uint8_t texture = 0;
		int8_t facing = 0;
	};
	std::vector<Run> runs;
	size_t bytes = 0;
	uint64_t used = 0;
};
std::unordered_map<uint64_t, GpuTexture> _card_textures;
std::unordered_map<uint64_t, GpuBuilding> _card_buildings;
size_t _card_bytes = 0;
std::atomic<size_t> _card_sent = 0; ///< the rig: bytes put on the card since the last ThreeDGpuTimes()
std::atomic<int> _card_trims = 0;   ///< and how many times it let go of what it held the longest not drawn
uint64_t _card_clock = 0;
constexpr size_t GPU_BYTES_KEPT = 1024u * 1024 * 1024; ///< (the player, 20. 10.: "today's cards have 8 GB"; 128 MB after #285 was Winlator's old OpenGL)

/* The programs, the picture drawn into, the corners of the lights at night. */
struct GpuProgram {
	GLuint program = 0;
	GLint matrix = -1, light = -1, lit = -1, tex = -1, glass = -1, glass_alpha = -1, glow = -1;
	/* The paint (ThreeDGpuPaint). */
	GLint paint = -1, detail = -1, detail_colours = -1, paint_mask = -1, addon_mask = -1, rust_mask = -1;
	GLint colour = -1, target = -1, start = -1, fade = -1, addon_colour = -1, addon_start = -1, tint = -1;
	GLint rust_colour = -1, rust_time = -1, rust_ramp = -1;
	GLint near_plane = -1; ///< the view from the eyes' nearest (EYE_BUILDING_FRAGMENT)
	GLint wheels = -1, wheel_count = -1; ///< the view from the eyes' turning wheels (EyeMeshSource())
	GLint depth_range = -1; ///< the nearness a picture's depths span (MAP_BUILDING_VERTEX)
};
GpuProgram _mesh_program, _night_program, _eye_building_program, _eye_mesh_program, _map_building_program;
/* The buildings of the turned map drawn into (DrawBuildingsOnCard()): their colours, their nearness as floats, depths. */
GLuint _bfbo = 0, _b_colour_rb = 0, _b_near_rb = 0, _b_depth_rb = 0;
int _bfbo_w = 0, _bfbo_h = 0;
GLuint _fbo = 0, _colour_rb = 0, _lit_rb = 0, _depth_rb = 0;
int _fbo_w = 0, _fbo_h = 0;
/* The view from the eyes drawn into: its colours, and its depth as floats (near / how far ahead, the larger nearer). */
GLuint _eye_fbo = 0, _eye_colour_rb = 0, _eye_glow_rb = 0, _eye_depth_rb = 0;
int _eye_w = 0, _eye_h = 0;
GLuint _night_vao = 0, _night_vbo = 0;
GLint _max_texture = 0, _max_target = 0;
/* The whole view from the eyes on the card (DrawEyeFrameOnCard()). */
GLuint _frame_colour[2] = {0, 0}, _frame_glow = 0, _frame_depth = 0;
GLuint _frame_geometry_fbo[2] = {0, 0}, _frame_pixel_fbo[2] = {0, 0};
int _frame_w = 0, _frame_h = 0;
GLuint _frame_atlas = 0;
int _frame_atlas_w = 0, _frame_atlas_h = 0;
std::vector<uint32_t> _frame_atlas_px;
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
const char *const MESH_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
uniform mat4 u_matrix;
uniform vec3 u_light;
uniform float u_lit;
noperspective out vec2 v_uv;
noperspective out float v_light;
void main()
{
	gl_Position = u_matrix * vec4(a_pos, 1.0);
	v_uv = a_uv;
	v_light = u_lit > 0.5 ? 0.55 + 0.45 * max(0.0, dot(a_nrm, u_light)) : 1.0;
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
uniform vec4 u_glow;
uniform int u_paint;
uniform sampler2D u_detail, u_detail_colours, u_paint_mask, u_addon_mask, u_rust_mask;
uniform vec3 u_colour, u_target, u_addon_colour, u_tint, u_rust_colour;
uniform float u_start, u_fade, u_addon_start, u_rust_time, u_rust_ramp;
noperspective in vec2 v_uv;
noperspective in float v_light;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_lit;

const int PAINT_DETAIL = 1, PAINT_BODY = 2, PAINT_MASK = 4, PAINT_COLOURED = 8, PAINT_ADDON = 16, PAINT_TINT = 32, PAINT_RUST = 64, PAINT_GRADUAL = 128;

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
	if ((u_paint & PAINT_RUST) != 0) {
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
	vec3 own = floor(t.rgb * 255.0 + 0.5);
	if (u_paint != 0 && t != vec4(0.0)) own = Paint(own, (vec2(at) + 0.5) / vec2(size));
	/* Its own light (ThreeDVehicleShape::tex_glow): u_glow.a 0 none, 1 the texel times its colour, 2 its colour. */
	vec3 e = u_glow.a == 0.0 ? vec3(0.0) : (u_glow.a < 1.5 ? floor(u_glow.rgb * own / 255.0) : u_glow.rgb);
	vec3 c = floor(min(vec3(255.0), own * v_light + e)) / 255.0;
	o_lit = u_glow.a == 0.0 ? vec4(0.0) : vec4(1.0);
	o_colour = u_glass == 0 ? vec4(c, 1.0) : vec4(c * u_glass_alpha, u_glass_alpha);
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
out vec2 v_uv;
flat out float v_light;
void main()
{
	gl_Position = u_matrix * vec4(a_pos, 1.0);
	v_uv = a_uv;
	v_light = a_light;
}
)";

const char *const EYE_BUILDING_FRAGMENT = R"(#version 330 core
uniform sampler2D u_tex;
uniform float u_near;
in vec2 v_uv;
flat in float v_light;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_glow;
void main()
{
	o_glow = vec4(0.0);
	ivec2 size = textureSize(u_tex, 0);
	ivec2 at = ivec2(floor(v_uv * vec2(size)));
	if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) discard;
	vec4 t = texelFetch(u_tex, at, 0);
	if (t == vec4(0.0)) discard;
	vec3 c = floor(t.rgb * 255.0 + 0.5);
	if (v_light != 256.0) c = min(vec3(255.0), floor(c * v_light / 256.0));
	o_colour = vec4(c / 255.0, 1.0);
	gl_FragDepth = u_near * gl_FragCoord.w;
}
)";

/* The buildings of the turned map (DrawBuildingsOnCard()), as the processor draws them into their pictures
 * (DrawObjectInto(), DrawTexturedTriangle() of viewport.cpp): each pixel at its middle the texel under it -- one the
 * texture has nothing for (0) takes the nearest drawn within three, in the processor's order --, shaded by the light of
 * the corners across the triangle (256 as it is) to whole numbers; the nearer kept, and how near written out. */
const char *const MAP_BUILDING_VERTEX = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in float a_light;
uniform mat4 u_matrix;
uniform vec2 u_depth_range;
noperspective out vec2 v_uv;
noperspective out float v_light;
noperspective out float v_near;
void main()
{
	gl_Position = u_matrix * vec4(a_pos, 1.0);
	v_uv = a_uv;
	v_light = a_light;
	v_near = u_depth_range.x + (gl_Position.z + 1.0) * 0.5 * (u_depth_range.y - u_depth_range.x);
}
)";

const char *const MAP_BUILDING_FRAGMENT = R"(#version 330 core
uniform sampler2D u_tex;
noperspective in vec2 v_uv;
noperspective in float v_light;
noperspective in float v_near;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_near;
const ivec2 NEAR[24] = ivec2[24](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1),
		ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(2, 1), ivec2(-2, 1), ivec2(2, -1), ivec2(-2, -1),
		ivec2(3, 0), ivec2(-3, 0), ivec2(0, 3), ivec2(0, -3), ivec2(3, 1), ivec2(-3, 1), ivec2(3, -1), ivec2(-3, -1));
void main()
{
	ivec2 size = textureSize(u_tex, 0);
	ivec2 at = ivec2(floor(v_uv * vec2(size)));
	if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y) discard;
	vec4 t = texelFetch(u_tex, at, 0);
	if (t == vec4(0.0)) {
		for (int k = 0; k < 24; k++) {
			ivec2 n = at + NEAR[k];
			if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y) continue;
			t = texelFetch(u_tex, n, 0);
			if (t != vec4(0.0)) break;
		}
		if (t == vec4(0.0)) discard;
	}
	vec3 c = floor(t.rgb * 255.0 + 0.5);
	float l = floor(max(0.0, v_light));
	c = min(vec3(255.0), floor(c * l / 256.0));
	o_colour = vec4(c / 255.0, t.a);
	o_near = vec4(v_near, 0.0, 0.0, 1.0);
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
		 * texel by its colour's strongest; see-through by the glass's alpha. */
		replace("o_lit = u_glow.a == 0.0 ? vec4(0.0) : vec4(1.0);", "o_lit = u_glow.a == 0.0 ? vec4(0.0) : vec4(floor(own * max(u_glow.r, max(u_glow.g, u_glow.b)) / 256.0) / 255.0, 1.0);\n\tif (u_glass != 0) o_lit *= u_glass_alpha;");
	} else {
		/* A wheel that turns (its corners' wheel in attribute 3): its corners and normals moved by its matrix first. */
		s.insert(s.find('\n') + 1, "layout(location = 3) in float a_wheel;\nuniform mat4 u_wheels[32];\nuniform int u_wheel_count;\n");
		replace("gl_Position = u_matrix * vec4(a_pos, 1.0);", "int k = int(a_wheel + 0.5);\n\tvec3 p = a_pos, n = a_nrm;\n\tif (k > 0 && k <= u_wheel_count) {\n\t\tp = (u_wheels[k - 1] * vec4(p, 1.0)).xyz;\n\t\tn = mat3(u_wheels[k - 1]) * n;\n\t}\n\tgl_Position = u_matrix * vec4(p, 1.0);");
		replace("dot(a_nrm, u_light)", "dot(n, u_light)");
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
	vec3 c = floor(t.rgb * 255.0 + 0.5);
	float light = (how & 4) != 0 ? v_corner_light : v_light;
	if (light != 256.0) c = min(vec3(255.0), floor(c * light / 256.0));
	o_colour = vec4(c / 255.0, v_how.x >= 256.0 ? 1.0 : v_how.x / 256.0);
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
layout(location = 0) out vec4 o_colour;
ivec2 Pixel() { return ivec2(gl_FragCoord.xy); }
vec3 ColourAt(ivec2 p) { return floor(texelFetch(u_colour, p, 0).rgb * 255.0 + 0.5); }
float IwAt(ivec2 p) { return texelFetch(u_depth, p, 0).r / u_near; }
/* CabPixelInWorld(): x, y from the eye, the height as x and y from the eye's. */
vec3 InWorld(vec2 s, float iw)
{
	float f = 1.0 / iw;
	float r = (s.x - u_eye_lens.y) * f / u_eye_lens.x, u = (u_eye_lens.z - s.y) * f / u_eye_lens.x;
	return vec3(f * u_eye_dir.x + r * u_eye_dir.z, f * u_eye_dir.y + r * u_eye_dir.w, u);
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
uniform float u_from, u_to, u_horizon;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float z = IwAt(p);
	float f = 0.0;
	if (z <= 0.0) {
		if (float(p.y) >= u_horizon) f = 1.0;
	} else {
		f = clamp((1.0 / z - u_from) / (u_to - u_from), 0.0, 1.0);
	}
	if (f > 0.0) c = floor(c + (u_haze - c) * f);
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

/* The lights lighting what is about them (DrawEyeLampLight(), DrawEyeWitchLight()), one after the other: each pixel
 * within reach its own colour out of the dark (u_night) in the light's. The lights four texels each of u_lights. */
const char *const EYE_LAMPS_FRAGMENT = R"(
uniform sampler2D u_lights;
uniform int u_count;
uniform vec3 u_night;
uniform float u_zu_per_unit;
void main()
{
	ivec2 p = Pixel();
	vec3 c = ColourAt(p);
	float iw = IwAt(p);
	if (iw > 0.0) {
		vec3 w = InWorld(vec2(p) + 0.5, iw);
		for (int i = 0; i < u_count; i++) {
			vec4 l0 = texelFetch(u_lights, ivec2(i * 4, 0), 0), l1 = texelFetch(u_lights, ivec2(i * 4 + 1, 0), 0);
			vec4 l2 = texelFetch(u_lights, ivec2(i * 4 + 2, 0), 0), l3 = texelFetch(u_lights, ivec2(i * 4 + 3, 0), 0);
			if (p.x < int(l3.x) || p.y < int(l3.y) || p.x > int(l3.z) || p.y > int(l3.w)) continue;
			int kind = int(l1.w + 0.5);
			float reach = l0.w, k;
			if (kind == 0) {
				/* A house's lamp: down from it only. */
				if (w.z > l0.z + 0.5) continue;
				float d = length(w - l0.xyz);
				if (d >= reach) continue;
				k = (1.0 - d / reach) * (1.0 - d / reach) * 0.7;
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
			vec3 own = min(vec3(255.0), c * 256.0 / u_night);
			c = floor(min(vec3(255.0), c + own * l1.rgb / 255.0 * k));
		}
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

/* A picture of its own laid in where it is and nearer (a tunnel seen through its portal, CabDrawTunnelSeenIn()). */
const char *const EYE_LAY_FRAGMENT = R"(#version 330 core
uniform sampler2D u_lay;
uniform sampler2D u_lay_z;
uniform float u_near;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_glow;
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	vec4 c = texelFetch(u_lay, p, 0);
	float z = texelFetch(u_lay_z, p, 0).r;
	if (z <= 0.0 || c == vec4(0.0)) discard;
	o_colour = vec4(c.rgb, 1.0);
	o_glow = vec4(0.0);
	gl_FragDepth = u_near * z;
}
)";

/* A soft spot of light added (AddNightLight() of viewport.cpp): a quad about it, the whole of it gone where the picture
 * is nearer than its middle (by hidden_by). Its corner, middle and sizes, colour and strength, depth and hidden_by. */
const char *const EYE_GLOW_VERTEX = R"(#version 330 core
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_spot;   // sx, sy, rx, ry
layout(location = 2) in vec4 a_colour; // r, g, b, strength
layout(location = 3) in vec2 a_hidden; // iw, hidden_by
uniform sampler2D u_depth;
uniform float u_near;
uniform vec2 u_size;
flat out vec4 v_spot;
flat out vec4 v_colour;
void main()
{
	v_spot = a_spot;
	v_colour = a_colour;
	ivec2 m = ivec2(int(a_spot.x), int(a_spot.y));
	bool hidden = a_hidden.x > 0.0 && m.x >= 0 && m.y >= 0 && m.x < int(u_size.x) && m.y < int(u_size.y) && texelFetch(u_depth, m, 0).r / u_near > a_hidden.x * a_hidden.y;
	vec2 at = a_spot.xy + a_corner * (a_spot.zw + 1.0);
	gl_Position = hidden ? vec4(2.0, 2.0, 0.0, 1.0) : vec4(2.0 * at / u_size - 1.0, 0.0, 1.0);
}
)";

const char *const EYE_GLOW_FRAGMENT = R"(#version 330 core
flat in vec4 v_spot;
flat in vec4 v_colour;
layout(location = 0) out vec4 o_colour;
void main()
{
	vec2 d = (gl_FragCoord.xy - v_spot.xy) / v_spot.zw;
	float d2 = dot(d, d);
	if (d2 >= 1.0) discard;
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
		why = "no context of OpenGL";
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
	gl.name = reinterpret_cast<decltype(gl.name)>(GetGlProc(#name)); \
	if (gl.name == nullptr) { \
		why = "OpenGL without " #name; \
		return false; \
	}
	THREE_D_GL_FUNCTIONS(THREE_D_GL_LOAD)
#undef THREE_D_GL_LOAD
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

bool MakeProgram(GpuProgram &p, const char *vertex, const char *fragment, std::string &why)
{
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
	p.light = gl.glGetUniformLocation(p.program, "u_light");
	p.lit = gl.glGetUniformLocation(p.program, "u_lit");
	p.tex = gl.glGetUniformLocation(p.program, "u_tex");
	p.glass = gl.glGetUniformLocation(p.program, "u_glass");
	p.glass_alpha = gl.glGetUniformLocation(p.program, "u_glass_alpha");
	p.glow = gl.glGetUniformLocation(p.program, "u_glow");
	p.paint = gl.glGetUniformLocation(p.program, "u_paint");
	p.detail = gl.glGetUniformLocation(p.program, "u_detail");
	p.detail_colours = gl.glGetUniformLocation(p.program, "u_detail_colours");
	p.paint_mask = gl.glGetUniformLocation(p.program, "u_paint_mask");
	p.addon_mask = gl.glGetUniformLocation(p.program, "u_addon_mask");
	p.rust_mask = gl.glGetUniformLocation(p.program, "u_rust_mask");
	p.colour = gl.glGetUniformLocation(p.program, "u_colour");
	p.target = gl.glGetUniformLocation(p.program, "u_target");
	p.start = gl.glGetUniformLocation(p.program, "u_start");
	p.fade = gl.glGetUniformLocation(p.program, "u_fade");
	p.addon_colour = gl.glGetUniformLocation(p.program, "u_addon_colour");
	p.addon_start = gl.glGetUniformLocation(p.program, "u_addon_start");
	p.tint = gl.glGetUniformLocation(p.program, "u_tint");
	p.rust_colour = gl.glGetUniformLocation(p.program, "u_rust_colour");
	p.rust_time = gl.glGetUniformLocation(p.program, "u_rust_time");
	p.rust_ramp = gl.glGetUniformLocation(p.program, "u_rust_ramp");
	p.near_plane = gl.glGetUniformLocation(p.program, "u_near");
	p.wheels = gl.glGetUniformLocation(p.program, "u_wheels");
	p.wheel_count = gl.glGetUniformLocation(p.program, "u_wheel_count");
	p.depth_range = gl.glGetUniformLocation(p.program, "u_depth_range");
	return true;
}

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
	Step("preklad programu budov plochy (shader)");
	if (!MakeProgram(_map_building_program, MAP_BUILDING_VERTEX, MAP_BUILDING_FRAGMENT, why)) return false;
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
	gl.glGenFramebuffers(1, &_bfbo);
	gl.glGenRenderbuffers(1, &_b_colour_rb);
	gl.glGenRenderbuffers(1, &_b_near_rb);
	gl.glGenRenderbuffers(1, &_b_depth_rb);
	gl.glGenFramebuffers(1, &_eye_fbo);
	gl.glGenRenderbuffers(1, &_eye_colour_rb);
	gl.glGenRenderbuffers(1, &_eye_glow_rb);
	gl.glGenRenderbuffers(1, &_eye_depth_rb);
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
	Step("preklad programu snimku z oci (shader)");
	StartEyeFrame();
	describe = fmt::format("{}, OpenGL {}, textures up to {}, pictures up to {}{}", renderer != nullptr ? renderer : "?", version, _max_texture, _max_target, _frame_programs ? "" : ", the view from the eyes in parts");
	return true;
}

void ForgetCard()
{
	Step("vse z karty pryc");
	for (auto &[uid, s] : _card_shapes) {
		gl.glDeleteVertexArrays(1, &s.vao);
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
	_card_buildings.clear();
	_card_bytes = 0;
}

void EndCard()
{
	if (gl.glDeleteBuffers != nullptr) {
		ForgetCard();
		if (_fbo != 0) gl.glDeleteFramebuffers(1, &_fbo);
		if (_eye_fbo != 0) gl.glDeleteFramebuffers(1, &_eye_fbo);
		if (_bfbo != 0) gl.glDeleteFramebuffers(1, &_bfbo);
		const GLuint rbs[] = {_colour_rb, _lit_rb, _depth_rb, _eye_colour_rb, _eye_glow_rb, _eye_depth_rb, _b_colour_rb, _b_near_rb, _b_depth_rb};
		for (GLuint rb : rbs) if (rb != 0) gl.glDeleteRenderbuffers(1, &rb);
		if (_night_vbo != 0) gl.glDeleteBuffers(1, &_night_vbo);
		if (_night_vao != 0) gl.glDeleteVertexArrays(1, &_night_vao);
		if (_mesh_program.program != 0) gl.glDeleteProgram(_mesh_program.program);
		if (_night_program.program != 0) gl.glDeleteProgram(_night_program.program);
		if (_eye_building_program.program != 0) gl.glDeleteProgram(_eye_building_program.program);
		if (_eye_mesh_program.program != 0) gl.glDeleteProgram(_eye_mesh_program.program);
		if (_map_building_program.program != 0) gl.glDeleteProgram(_map_building_program.program);
		EndEyeFrame();
	}
	EndContext();
}

/** Let go of what the card holds the longest not drawn, till it holds three quarters of GPU_BYTES_KEPT. */
void TrimCard()
{
	if (_card_bytes <= GPU_BYTES_KEPT) return;
	struct Held {
		uint64_t used;
		int kind;
		uint64_t uid;
	};
	std::vector<Held> held;
	for (const auto &[uid, s] : _card_shapes) if (s.used < _card_clock) held.push_back({s.used, 0, uid});
	for (const auto &[uid, i] : _card_indices) if (i.used < _card_clock) held.push_back({i.used, 1, uid});
	for (const auto &[uid, t] : _card_textures) if (t.used < _card_clock) held.push_back({t.used, 2, uid});
	for (const auto &[uid, b] : _card_buildings) if (b.used < _card_clock) held.push_back({b.used, 3, uid});
	std::ranges::sort(held, [](const Held &a, const Held &b) { return a.used < b.used; });
	Step(fmt::format("uvolneni karty ({} MB)", _card_bytes >> 20));
	_card_trims++;
	for (const Held &h : held) {
		if (_card_bytes <= GPU_BYTES_KEPT / 4 * 3) break;
		if (h.kind == 0) {
			GpuShape &s = _card_shapes[h.uid];
			gl.glDeleteVertexArrays(1, &s.vao);
			const GLuint buffers[] = {s.pos, s.nrm, s.uv, s.wheel};
			for (GLuint b : buffers) if (b != 0) gl.glDeleteBuffers(1, &b);
			_card_bytes -= s.bytes;
			_card_shapes.erase(h.uid);
		} else if (h.kind == 1) {
			GpuIndices &i = _card_indices[h.uid];
			gl.glDeleteBuffers(1, &i.ibo);
			_card_bytes -= i.bytes;
			_card_indices.erase(h.uid);
		} else if (h.kind == 2) {
			GpuTexture &t = _card_textures[h.uid];
			gl.glDeleteTextures(1, &t.name);
			_card_bytes -= t.bytes;
			_card_textures.erase(h.uid);
		} else {
			GpuBuilding &b = _card_buildings[h.uid];
			gl.glDeleteVertexArrays(1, &b.vao);
			gl.glDeleteBuffers(1, &b.vbo);
			_card_bytes -= b.bytes;
			_card_buildings.erase(h.uid);
		}
	}
}

/** The corners of a shape on the card: put there the first time. */
const GpuShape *ShapeOnCard(const ThreeDVehicleShape &s)
{
	auto it = _card_shapes.find(s.uid.value);
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
	/* Each corner's turning wheel, a byte (the view from the eyes turns them, EyeMeshSource()). */
	g.has_wheel = s.corner_wheel.size() == corners;
	if (g.has_wheel) {
		gl.glGenBuffers(1, &g.wheel);
		gl.glBindBuffer(GL_ARRAY_BUFFER, g.wheel);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(corners), s.corner_wheel.data(), GL_STATIC_DRAW);
		gl.glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_FALSE, 0, nullptr);
		gl.glEnableVertexAttribArray(3);
	}
	gl.glBindVertexArray(0);
	g.bytes = corners * (sizeof(float) * (g.has_nrm ? 8 : 5) + (g.has_wheel ? 1 : 0));
	g.used = _card_clock;
	_card_bytes += g.bytes;
	_card_sent += g.bytes;
	return &_card_shapes.emplace(s.uid.value, g).first->second;
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
	_card_sent += g.bytes;
	return &_card_indices.emplace(t.uid, g).first->second;
}

/** A texture on the card, its texels as they are (Colour::data). */
const GpuTexture *TextureOnCard(const ThreeDObjectTexture &t)
{
	auto it = _card_textures.find(t.uid.value);
	if (it != _card_textures.end()) {
		it->second.used = _card_clock;
		return &it->second;
	}
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
	_card_sent += g.bytes;
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
	_card_sent += g.bytes;
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

/* A texture's paint on the card: its masks' names (ThreeDGpuPaint). */
struct PaintOnCard {
	GLuint detail = 0, detail_colours = 0, paint_mask = 0, addon_mask = 0, rust_mask = 0;
};

/** A mesh of a picture as the card draws it (PictureOnCard()): its shape, its triangles, its textures and their paints there. */
struct MeshOnCard {
	const ThreeDGpuMesh *mesh;
	const GpuShape *shape;
	const GpuIndices *indices;
	std::vector<GLuint> textures;
	std::vector<PaintOnCard> paints;
};

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
			const ThreeDGpuPaint &paint = m.paints[i];
			PaintOnCard &pc = d.paints[i];
			auto mask = [](const ThreeDObjectTexture *tex, GLuint &name) {
				if (tex == nullptr) return true;
				const GpuTexture *on = TextureOnCard(*tex);
				if (on == nullptr) return false;
				name = on->name;
				return true;
			};
			if (paint.detail != nullptr) {
				const GpuTexture *on = DetailMaskOnCard(*paint.detail);
				if (on == nullptr || paint.detail_colours == nullptr) return false;
				pc.detail = on->name;
			}
			if (!mask(paint.detail_colours, pc.detail_colours) || !mask(paint.paint_mask, pc.paint_mask) || !mask(paint.addon_mask, pc.addon_mask) || !mask(paint.rust_mask, pc.rust_mask)) return false;
		}
		draws.push_back(std::move(d));
	}
	return true;
}

/**
 * Draw a picture into the frame bound, its corner at x, y (DrawPicturesOnCard()): the frame cleared, its depths its own
 * -- the pictures beside it are not where it is.
 */
void DrawPictureAt(const ThreeDGpuPicture &pic, const std::vector<MeshOnCard> &draws, int x, int y)
{
	const int w = pic.width, h = pic.height;
	gl.glViewport(x, y, w, h);
	gl.glDisable(GL_BLEND);
	gl.glEnable(GL_DEPTH_TEST);
	gl.glDepthFunc(GL_LESS);
	gl.glDepthMask(GL_TRUE);
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
	gl.glUseProgram(_mesh_program.program);
	gl.glUniform1i(_mesh_program.tex, 0);
	gl.glUniform1i(_mesh_program.detail, 1);
	gl.glUniform1i(_mesh_program.detail_colours, 2);
	gl.glUniform1i(_mesh_program.paint_mask, 3);
	gl.glUniform1i(_mesh_program.addon_mask, 4);
	gl.glUniform1i(_mesh_program.rust_mask, 5);
	gl.glUniform1f(_mesh_program.glass_alpha, pic.glass_alpha);
	gl.glActiveTexture(GL_TEXTURE0);
	/* A texture's paint: what of it to do (the bits of MESH_FRAGMENT), its colours and its masks on their units. */
	constexpr int PAINT_DETAIL = 1, PAINT_BODY = 2, PAINT_MASK = 4, PAINT_COLOURED = 8, PAINT_ADDON = 16, PAINT_TINT = 32, PAINT_RUST = 64, PAINT_GRADUAL = 128;
	auto set_paint = [](const ThreeDGpuPaint *paint, const PaintOnCard *pc) {
		if (paint == nullptr || pc == nullptr || !paint->Any()) {
			gl.glUniform1i(_mesh_program.paint, 0);
			return;
		}
		int bits = 0;
		if (pc->detail != 0) bits |= PAINT_DETAIL;
		if (paint->body) bits |= PAINT_BODY;
		if (pc->paint_mask != 0) bits |= PAINT_MASK;
		if (paint->coloured) bits |= PAINT_COLOURED;
		if (pc->addon_mask != 0) bits |= PAINT_ADDON;
		if (paint->tinted) bits |= PAINT_TINT;
		if (pc->rust_mask != 0) bits |= PAINT_RUST;
		if (paint->rust_gradual) bits |= PAINT_GRADUAL;
		gl.glUniform1i(_mesh_program.paint, bits);
		gl.glUniform3f(_mesh_program.colour, paint->colour[0], paint->colour[1], paint->colour[2]);
		gl.glUniform3f(_mesh_program.target, paint->target[0], paint->target[1], paint->target[2]);
		gl.glUniform1f(_mesh_program.start, paint->start);
		gl.glUniform1f(_mesh_program.fade, paint->fade);
		gl.glUniform3f(_mesh_program.addon_colour, paint->addon_colour[0], paint->addon_colour[1], paint->addon_colour[2]);
		gl.glUniform1f(_mesh_program.addon_start, paint->addon_start);
		gl.glUniform3f(_mesh_program.tint, paint->tint[0], paint->tint[1], paint->tint[2]);
		gl.glUniform3f(_mesh_program.rust_colour, paint->rust_colour[0], paint->rust_colour[1], paint->rust_colour[2]);
		gl.glUniform1f(_mesh_program.rust_time, paint->rust_time);
		gl.glUniform1f(_mesh_program.rust_ramp, paint->rust_ramp);
		const GLuint units[5] = {pc->detail, pc->detail_colours, pc->paint_mask, pc->addon_mask, pc->rust_mask};
		for (int k = 0; k < 5; k++) {
			if (units[k] == 0) continue;
			gl.glActiveTexture(GL_TEXTURE0 + 1 + k);
			gl.glBindTexture(GL_TEXTURE_2D, units[k]);
		}
		gl.glActiveTexture(GL_TEXTURE0);
	};
	auto draw_runs = [&](const MeshOnCard &d, const std::vector<ThreeDGpuTriangles::Run> &runs, bool glass) {
		const ThreeDGpuMatrix m = Multiply(to_card, d.mesh->matrix);
		gl.glUniformMatrix4fv(_mesh_program.matrix, 1, GL_TRUE, m.data());
		gl.glUniform3f(_mesh_program.light, d.mesh->light[0], d.mesh->light[1], d.mesh->light[2]);
		gl.glUniform1f(_mesh_program.lit, d.mesh->lit && d.shape->has_nrm ? 1.0f : 0.0f);
		gl.glUniform1i(_mesh_program.glass, glass ? 1 : 0);
		gl.glBindVertexArray(d.shape->vao);
		gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices->ibo);
		if (!d.shape->has_nrm) gl.glVertexAttrib3f(1, 0, 0, 1);
		for (const ThreeDGpuTriangles::Run &r : runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0) continue;
			set_paint(r.texture < d.mesh->paints.size() ? &d.mesh->paints[r.texture] : nullptr, &d.paints[r.texture]);
			/* Its material's own light, and see-through by its own alpha (a material "BLEND") or the glass's. */
			const uint32_t glow = d.mesh->shape->Glow(r.texture);
			const Colour e(glow);
			gl.glUniform4f(_mesh_program.glow, e.r, e.g, e.b, glow == 0 ? 0.0f : (e.a == ThreeDVehicleShape::GLOW_TEXTURED ? 1.0f : 2.0f));
			if (glass) {
				const uint8_t alpha = d.mesh->shape->Alpha(r.texture);
				gl.glUniform1f(_mesh_program.glass_alpha, alpha < 255 ? alpha / 255.0f : pic.glass_alpha);
			}
			gl.glBindTexture(GL_TEXTURE_2D, d.textures[r.texture]);
			gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(static_cast<uintptr_t>(r.first) * sizeof(uint32_t)));
		}
	};
	for (const MeshOnCard &d : draws) draw_runs(d, d.mesh->triangles->opaque, false);
	/* The glass after all else, over what is behind it, hiding nothing (no depth written); lit only where it has a light
	 * of its own (its lit 1 with alpha 1 over what is there, else 0 with alpha 0 leaving it). */
	gl.glDepthMask(GL_FALSE);
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	for (const MeshOnCard &d : draws) {
		if (!d.mesh->triangles->glass.empty()) draw_runs(d, d.mesh->triangles->glass, true);
	}
	gl.glDisable(GL_BLEND);
	gl.glDepthMask(GL_TRUE);
	/* The glass of the lights on at night, over the body. */
	if (!pic.night.empty()) {
		std::vector<float> corners;
		corners.reserve(pic.night.size() * 18);
		for (const ThreeDGpuNightTriangle &t : pic.night) {
			const Colour c(t.colour);
			for (int k = 0; k < 3; k++) corners.insert(corners.end(), {t.x[k], t.y[k], t.depth[k], c.r / 255.0f, c.g / 255.0f, c.b / 255.0f});
		}
		gl.glUseProgram(_night_program.program);
		gl.glUniformMatrix4fv(_night_program.matrix, 1, GL_TRUE, to_card.data());
		gl.glBindVertexArray(_night_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _night_vbo);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(corners.size() * sizeof(float)), corners.data(), GL_STREAM_DRAW);
		gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(pic.night.size() * 3));
	}
	gl.glBindVertexArray(0);
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
			if (y + pic.height > _max_target) break;
			placed[end].x = x;
			placed[end].y = y;
			x += pic.width;
			row = std::max(row, pic.height);
			read_lit |= pic.read_lit;
		}
		const int frame_h = y + row;
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
		for (size_t k = at; k < end; k++) DrawPictureAt(*placed[k].pic, placed[k].draws, placed[k].x, placed[k].y);
		/* Read back once, each picture's rows out of it. */
		Step(fmt::format("cteni obrazku {}-{} zpet (glReadPixels)", _gpu_pictures + 1 + at, _gpu_pictures + end));
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

/** The buildings of the turned map drawn into, at least as big as asked (grown in steps; DrawBuildingsOnCard()). */
bool BuildingTargetOfSize(int w, int h)
{
	if (w <= _bfbo_w && h <= _bfbo_h) return true;
	_bfbo_w = std::min(_max_target, (std::max(_bfbo_w, w) + 255) / 256 * 256);
	_bfbo_h = std::min(_max_target, (std::max(_bfbo_h, h) + 255) / 256 * 256);
	Step(fmt::format("ramec budov plochy {}x{} (glRenderbufferStorage, glCheckFramebufferStatus)", _bfbo_w, _bfbo_h));
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _b_colour_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, _bfbo_w, _bfbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _b_near_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_R32F, _bfbo_w, _bfbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, _b_depth_rb);
	gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, _bfbo_w, _bfbo_h);
	gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _bfbo);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _b_colour_rb);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, _b_near_rb);
	gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _b_depth_rb);
	if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		_bfbo_w = _bfbo_h = 0;
		return false;
	}
	return w <= _bfbo_w && h <= _bfbo_h;
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
	std::map<std::pair<uint8_t, int8_t>, std::vector<float>> by;
	for (size_t t = 0; t + 2 < b.tri.size(); t += 3) {
		const uint32_t ia = b.tri[t], ib = b.tri[t + 1], ic = b.tri[t + 2];
		if (std::max({ia, ib, ic}) >= corners) continue;
		const uint8_t texture = t / 3 < b.tri_tex.size() ? b.tri_tex[t / 3] : 0;
		const int8_t facing = t / 3 < b.facing.size() ? b.facing[t / 3] : 0;
		std::vector<float> &v = by[{texture, facing}];
		for (uint32_t i : {ia, ib, ic}) v.insert(v.end(), {b.pos[i * 3], b.pos[i * 3 + 1], b.pos[i * 3 + 2], b.uv[i * 2], b.uv[i * 2 + 1], lit ? static_cast<float>(b.light[i]) : 256.0f});
	}
	GpuBuilding g;
	std::vector<float> all;
	for (const auto &[k, v] : by) {
		g.runs.push_back({static_cast<GLint>(all.size() / 6), static_cast<GLsizei>(v.size() / 6), k.first, k.second});
		all.insert(all.end(), v.begin(), v.end());
	}
	Step(fmt::format("budova plochy na kartu: {} trojuhelniku", all.size() / 18));
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
	_card_sent += g.bytes;
	return &_card_buildings.emplace(key, std::move(g)).first->second;
}

/**
 * The pictures of the buildings of the turned map drawn by the card all at once (ThreeDGpuRenderBuildings(); the
 * player, 21. 10.: as the map turns, the processor projected and sorted every building's triangles every frame, again
 * for every part of the screen it is in): side by side in rows in one frame, its colours and nearness read back once.
 * Each picture's ThreeDGpuBuildingPicture::drawn tells whether it was drawn; one not drawn the processor draws.
 */
void DrawBuildingsOnCard(std::span<ThreeDGpuBuildingPicture *const> pics)
{
	_card_clock++;
	struct Placed {
		ThreeDGpuBuildingPicture *pic;
		const GpuBuilding *on;
		std::vector<GLuint> textures;
		int x = 0, y = 0;
	};
	std::vector<Placed> placed;
	for (ThreeDGpuBuildingPicture *pic : pics) {
		pic->drawn = false;
		if (pic->width <= 0 || pic->height <= 0 || pic->width > _max_target || pic->height > _max_target) continue;
		const GpuBuilding *on = MapBuildingOnCard(pic->building);
		if (on == nullptr) continue;
		Placed p{pic, on, {}};
		bool ok = true;
		for (const ThreeDObjectTexture *t : pic->building.textures) {
			const GpuTexture *g = t != nullptr ? TextureOnCard(*t) : nullptr;
			p.textures.push_back(g != nullptr ? g->name : 0);
			if (t != nullptr && g == nullptr) ok = false;
		}
		if (ok) placed.push_back(std::move(p));
	}
	std::ranges::stable_sort(placed, [](const Placed &a, const Placed &b) { return a.pic->height > b.pic->height; });
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	std::vector<uint32_t> colours;
	std::vector<float> nearness;
	for (size_t at = 0; at < placed.size();) {
		int frame_w = 2048;
		for (size_t k = at; k < placed.size(); k++) frame_w = std::max(frame_w, placed[k].pic->width);
		frame_w = std::min(frame_w, _max_target);
		int x = 0, y = 0, row = 0;
		size_t end = at;
		for (; end < placed.size(); end++) {
			const ThreeDGpuBuildingPicture &pic = *placed[end].pic;
			if (x + pic.width > frame_w) {
				y += row;
				x = 0;
				row = 0;
			}
			if (y + pic.height > _max_target) break;
			placed[end].x = x;
			placed[end].y = y;
			x += pic.width;
			row = std::max(row, pic.height);
		}
		const int frame_h = y + row;
		Step(fmt::format("budovy plochy {}: {}x{}", end - at, frame_w, frame_h));
		if (!BuildingTargetOfSize(frame_w, frame_h)) return;
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _bfbo);
		gl.glDrawBuffers(2, both);
		gl.glDisable(GL_DITHER);
		gl.glDisable(GL_BLEND);
		gl.glEnable(GL_SCISSOR_TEST);
		gl.glScissor(0, 0, frame_w, frame_h);
		gl.glDepthMask(GL_TRUE);
		gl.glClearColor(0, 0, 0, 0);
		gl.glClearDepth(0.0);
		gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		gl.glDisable(GL_SCISSOR_TEST);
		/* The nearer kept: the larger nearness, its depth the larger. */
		gl.glEnable(GL_DEPTH_TEST);
		gl.glDepthFunc(GL_GEQUAL);
		gl.glFrontFace(GL_CCW);
		gl.glUseProgram(_map_building_program.program);
		gl.glUniform1i(_map_building_program.tex, 0);
		gl.glActiveTexture(GL_TEXTURE0);
		for (size_t k = at; k < end; k++) {
			const Placed &p = placed[k];
			const ThreeDGpuBuildingPicture &pic = *p.pic;
			const int w = pic.width, h = pic.height;
			gl.glViewport(p.x, p.y, w, h);
			/* A pixel x, y of the picture is the card's x, y of its place, its rows from the bottom: the rows read back
			 * are the picture's from the top. */
			const float dn = pic.depth_near, df = pic.depth_far;
			const float dz = df > dn ? 2.0f / (df - dn) : 1.0f;
			const ThreeDGpuMatrix to_card = {
				2.0f / w, 0, 0, -1,
				0, 2.0f / h, 0, -1,
				0, 0, dz, -1 - dn * dz,
				0, 0, 0, 1,
			};
			const ThreeDGpuMatrix m = Multiply(to_card, pic.matrix);
			gl.glUniformMatrix4fv(_map_building_program.matrix, 1, GL_TRUE, m.data());
			gl.glUniform2f(_map_building_program.depth_range, dn, df);
			gl.glBindVertexArray(p.on->vao);
			for (const GpuBuilding::Run &r : p.on->runs) {
				if (r.texture >= p.textures.size() || p.textures[r.texture] == 0) continue;
				/* The side turned away left out, as the processor: a triangle seen from outside goes round clockwise
				 * on the picture (rows down), counter-clockwise on the card's (rows up, the same numbers). */
				if (!pic.building.cull || r.facing == 0) {
					gl.glDisable(GL_CULL_FACE);
				} else {
					gl.glEnable(GL_CULL_FACE);
					gl.glCullFace(r.facing > 0 ? GL_FRONT : GL_BACK);
				}
				gl.glBindTexture(GL_TEXTURE_2D, p.textures[r.texture]);
				gl.glDrawArrays(GL_TRIANGLES, r.first, r.count);
			}
		}
		gl.glDisable(GL_CULL_FACE);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glBindVertexArray(0);
		Step(fmt::format("cteni budov plochy zpet (glReadPixels), {}x{}", frame_w, frame_h));
		colours.resize(static_cast<size_t>(frame_w) * frame_h);
		nearness.resize(static_cast<size_t>(frame_w) * frame_h);
		gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
		gl.glReadPixels(0, 0, frame_w, frame_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, colours.data());
		gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
		gl.glReadPixels(0, 0, frame_w, frame_h, GL_RED, GL_FLOAT, nearness.data());
		gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
		const GLenum error = gl.glGetError();
		if (error != 0) {
			Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x}, the processor draws the buildings", error);
			if (error == GL_OUT_OF_MEMORY) ForgetCard();
			return;
		}
		for (size_t k = at; k < end; k++) {
			ThreeDGpuBuildingPicture &pic = *placed[k].pic;
			const int w = pic.width, h = pic.height, px = placed[k].x, py = placed[k].y;
			pic.pixels.resize(static_cast<size_t>(w) * h);
			pic.depth.resize(static_cast<size_t>(w) * h);
			for (int r = 0; r < h; r++) {
				const size_t from = static_cast<size_t>(py + r) * frame_w + px, to = static_cast<size_t>(r) * w;
				std::copy_n(colours.begin() + from, w, pic.pixels.begin() + to);
				for (int c = 0; c < w; c++) pic.depth[to + c] = pic.pixels[to + c] == 0 ? -std::numeric_limits<float>::infinity() : nearness[from + c];
			}
			pic.drawn = true;
		}
		at = end;
	}
	TrimCard();
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
		g.runs.push_back({static_cast<GLint>(all.size() / 6), static_cast<GLsizei>(v.size() / 6), key.first, key.second});
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
	_card_sent += g.bytes;
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
	};
	struct MeshDraw {
		const ThreeDGpuMesh *mesh;
		const GpuShape *shape;
		const GpuIndices *indices;
		std::vector<GLuint> textures;
	};
	std::vector<Draw> draws;
	std::vector<MeshDraw> mesh_draws;
	float glass_alpha = 0.15f;
};

void DrawEyeMeshes(const EyeObjectDraws &o, float near_plane, bool glass);

/** What the objects of the view from the eyes need on the card put there: false, something it cannot hold. */
bool PrepareEyeObjects(const ThreeDGpuEyePicture &pic, EyeObjectDraws &out)
{
	out.glass_alpha = pic.glass_alpha;
	for (const ThreeDGpuEyeBuilding &e : pic.buildings) {
		const GpuBuilding *on = BuildingOnCard(e.building);
		if (on == nullptr) continue;
		EyeObjectDraws::Draw d{&e, on, std::vector<GLuint>(e.building.textures.size(), 0)};
		for (const GpuBuilding::Run &r : on->runs) {
			if (r.texture >= d.textures.size() || e.building.textures[r.texture] == nullptr || d.textures[r.texture] != 0) continue;
			const GpuTexture *t = TextureOnCard(*e.building.textures[r.texture]);
			if (t == nullptr) return false;
			d.textures[r.texture] = t->name;
		}
		out.draws.push_back(std::move(d));
	}
	/* The figures' shapes, triangles and textures (painted for the eye already). */
	for (const ThreeDGpuMesh &m : pic.meshes) {
		if (m.shape == nullptr || m.triangles == nullptr || m.triangles->indices.empty()) continue;
		EyeObjectDraws::MeshDraw d{&m, ShapeOnCard(*m.shape), IndicesOnCard(*m.triangles), std::vector<GLuint>(m.textures.size(), 0)};
		if (d.shape == nullptr || d.indices == nullptr) return false;
		/* As DrawOnCard(): a triangle naming a corner the shape has not, the processor draws. */
		if (d.indices->largest >= d.shape->corners) return false;
		for (size_t i = 0; i < m.textures.size(); i++) {
			if (m.textures[i] == nullptr || i >= m.triangles->used.size() || !m.triangles->used[i]) continue;
			const GpuTexture *t = TextureOnCard(*m.textures[i]);
			if (t == nullptr) return false;
			d.textures[i] = t->name;
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
void DrawEyeObjects(const EyeObjectDraws &o, float near_plane)
{
	gl.glUseProgram(_eye_building_program.program);
	gl.glUniform1i(_eye_building_program.tex, 0);
	gl.glUniform1f(_eye_building_program.near_plane, near_plane);
	gl.glActiveTexture(GL_TEXTURE0);
	gl.glCullFace(GL_BACK);
	for (const EyeObjectDraws::Draw &d : o.draws) {
		gl.glUniformMatrix4fv(_eye_building_program.matrix, 1, GL_TRUE, d.eye->matrix.data());
		gl.glBindVertexArray(d.on->vao);
		for (const GpuBuilding::Run &r : d.on->runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0) continue;
			if (r.facing == 0 || !d.eye->building.cull) {
				gl.glDisable(GL_CULL_FACE);
			} else {
				gl.glEnable(GL_CULL_FACE);
				gl.glFrontFace(r.facing > 0 ? GL_CW : GL_CCW);
			}
			gl.glBindTexture(GL_TEXTURE_2D, d.textures[r.texture]);
			gl.glDrawArrays(GL_TRIANGLES, r.first, r.count);
		}
	}
	gl.glDisable(GL_CULL_FACE);
	DrawEyeMeshes(o, near_plane, false);
}

/**
 * The figures of the view from the eyes, as DrawOnCard() draws their pictures: opaque, or their glass over what is
 * behind it (laid over by its alpha, its colour multiplied in), its depth written.
 */
void DrawEyeMeshes(const EyeObjectDraws &o, float near_plane, bool glass)
{
	gl.glUseProgram(_eye_mesh_program.program);
	gl.glUniform1i(_eye_mesh_program.tex, 0);
	gl.glUniform1i(_eye_mesh_program.paint, 0);
	gl.glUniform1f(_eye_mesh_program.near_plane, near_plane);
	gl.glActiveTexture(GL_TEXTURE0);
	if (glass) {
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
	for (const EyeObjectDraws::MeshDraw &d : o.mesh_draws) {
		const std::vector<ThreeDGpuTriangles::Run> &runs = glass ? d.mesh->triangles->glass : d.mesh->triangles->opaque;
		if (runs.empty()) continue;
		gl.glUniformMatrix4fv(_eye_mesh_program.matrix, 1, GL_TRUE, d.mesh->matrix.data());
		gl.glUniform3f(_eye_mesh_program.light, d.mesh->light[0], d.mesh->light[1], d.mesh->light[2]);
		gl.glUniform1f(_eye_mesh_program.lit, d.mesh->lit && d.shape->has_nrm ? 1.0f : 0.0f);
		gl.glUniform1i(_eye_mesh_program.glass, glass ? 1 : 0);
		gl.glBindVertexArray(d.shape->vao);
		gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices->ibo);
		if (!d.shape->has_nrm) gl.glVertexAttrib3f(1, 0, 0, 1);
		if (!d.shape->has_wheel) gl.glVertexAttrib3f(3, 0, 0, 0);
		const size_t wheels = d.shape->has_wheel ? std::min(d.mesh->wheels.size(), THREE_D_GPU_EYE_WHEELS) : 0;
		gl.glUniform1i(_eye_mesh_program.wheel_count, static_cast<GLint>(wheels));
		if (wheels > 0) gl.glUniformMatrix4fv(_eye_mesh_program.wheels, static_cast<GLsizei>(wheels), GL_TRUE, d.mesh->wheels[0].data());
		for (const ThreeDGpuTriangles::Run &r : runs) {
			if (r.texture >= d.textures.size() || d.textures[r.texture] == 0) continue;
			const uint32_t glow = d.mesh->shape->Glow(r.texture);
			const Colour e(glow);
			gl.glUniform4f(_eye_mesh_program.glow, e.r, e.g, e.b, glow == 0 ? 0.0f : (e.a == ThreeDVehicleShape::GLOW_TEXTURED ? 1.0f : 2.0f));
			if (glass) {
				const uint8_t alpha = d.mesh->shape->Alpha(r.texture);
				gl.glUniform1f(_eye_mesh_program.glass_alpha, alpha < 255 ? alpha / 255.0f : o.glass_alpha);
			}
			gl.glBindTexture(GL_TEXTURE_2D, d.textures[r.texture]);
			gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(static_cast<uintptr_t>(r.first) * sizeof(uint32_t)));
		}
	}
	if (glass) gl.glDisable(GL_BLEND);
	gl.glBindVertexArray(0);
}

/**
 * Draw the objects of the view from the eyes on the card and read them back (ThreeDGpuRenderEye()): the depth the
 * larger the nearer (near / how far ahead, cleared 0).
 */
bool DrawEyeOnCard(ThreeDGpuEyePicture &pic)
{
	const int w = pic.width, h = pic.height;
	if (w <= 0 || h <= 0 || w > _max_target || h > _max_target || pic.near_plane <= 0) return false;
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
	const GLsizei glow_stride = 12 * sizeof(float);
	gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, glow_stride, nullptr);
	gl.glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, glow_stride, reinterpret_cast<const void *>(2 * sizeof(float)));
	gl.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, glow_stride, reinterpret_cast<const void *>(6 * sizeof(float)));
	gl.glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, glow_stride, reinterpret_cast<const void *>(10 * sizeof(float)));
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
	_card_sent += g.bytes;
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
		if (t.uid != 0 || t.w <= 0 || t.h <= 0) continue;
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
	auto job = [&](const GpuProgram &p) {
		gl.glBindFramebuffer(GL_FRAMEBUFFER, _frame_pixel_fbo[1 - now]);
		gl.glDrawBuffers(1, colour_only);
		gl.glDisable(GL_DEPTH_TEST);
		gl.glDisable(GL_BLEND);
		gl.glUseProgram(p.program);
		gl.glActiveTexture(GL_TEXTURE0);
		gl.glBindTexture(GL_TEXTURE_2D, _frame_colour[now]);
		gl.glActiveTexture(GL_TEXTURE1);
		gl.glBindTexture(GL_TEXTURE_2D, _frame_depth);
		gl.glUniform1i(gl.glGetUniformLocation(p.program, "u_colour"), 0);
		gl.glUniform1i(gl.glGetUniformLocation(p.program, "u_depth"), 1);
		gl.glUniform1f(gl.glGetUniformLocation(p.program, "u_near"), depth_scale);
		gl.glUniform4f(gl.glGetUniformLocation(p.program, "u_eye_dir"), f.eye[3], f.eye[4], f.eye[5], f.eye[6]);
		gl.glUniform3f(gl.glGetUniformLocation(p.program, "u_eye_lens"), f.eye[7], f.eye[8], f.eye[9]);
		gl.glUniform2f(gl.glGetUniformLocation(p.program, "u_size"), static_cast<float>(w), static_cast<float>(h));
	};
	auto done = [&]() {
		gl.glBindVertexArray(_frame_empty_vao);
		gl.glDrawArrays(GL_TRIANGLES, 0, 3);
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
		gl.glUniform1i(gl.glGetUniformLocation(p.program, "u_depth"), 1);
		gl.glUniform1f(gl.glGetUniformLocation(p.program, "u_near"), depth_scale);
		gl.glUniform2f(gl.glGetUniformLocation(p.program, "u_size"), static_cast<float>(w), static_cast<float>(h));
		gl.glUniform4f(gl.glGetUniformLocation(p.program, "u_eye_dir"), f.eye[3], f.eye[4], f.eye[5], f.eye[6]);
		gl.glUniform3f(gl.glGetUniformLocation(p.program, "u_eye_lens"), f.eye[7], f.eye[8], f.eye[9]);
	};
	auto glows = [&](const std::vector<ThreeDGpuEyeGlow> &list) {
		if (list.empty()) return;
		std::vector<float> v;
		v.reserve(list.size() * 6 * 12);
		static const float quad[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
		for (const ThreeDGpuEyeGlow &g : list) {
			if (g.rx < 0.5f || g.ry < 0.5f) continue;
			for (const auto &q : quad) v.insert(v.end(), {q[0], q[1], g.sx, g.sy, g.rx, g.ry, g.r, g.g, g.b, g.strength, g.iw, g.hidden_by});
		}
		if (v.empty()) return;
		over(_frame_glow_program);
		gl.glEnable(GL_BLEND);
		gl.glBlendFunc(GL_ONE, GL_ONE);
		gl.glBindVertexArray(_frame_glow_vao);
		gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_glow_vbo);
		gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
		gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 12));
		gl.glDisable(GL_BLEND);
	};
	/* The figures' glass, after what is solid; seen through, its depth not kept (what is behind it is lit, hazed and
	 * seen to be hidden as it is). */
	std::vector<size_t> glass_waiting;
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
				job(_frame_background_program);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_background_program.program, "u_horizon"), s.horizon);
				done();
				break;
			case Kind::Triangles: {
				geometry();
				gl.glUseProgram(_frame_tri_program.program);
				gl.glUniform1i(gl.glGetUniformLocation(_frame_tri_program.program, "u_tex"), 0);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_tri_program.program, "u_near"), depth_scale);
				gl.glUniform2f(gl.glGetUniformLocation(_frame_tri_program.program, "u_size"), static_cast<float>(w), static_cast<float>(h));
				gl.glActiveTexture(GL_TEXTURE0);
				gl.glBindVertexArray(_frame_vao);
				for (size_t r = s.first_run; r < s.first_run + s.runs && r < f.runs.size(); r++) {
					/* Those after it of the same texture at once. */
					const ThreeDGpuEyeRun &run = f.runs[r];
					uint32_t count = run.count;
					while (r + 1 < s.first_run + s.runs && r + 1 < f.runs.size() && names[f.runs[r + 1].texture] == names[run.texture] && f.runs[r + 1].glass == run.glass && f.runs[r + 1].first == run.first + count) {
						count += f.runs[++r].count;
					}
					if (run.glass) {
						gl.glEnable(GL_BLEND);
						gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
						gl.glDepthMask(GL_FALSE);
					}
					gl.glBindTexture(GL_TEXTURE_2D, names[run.texture]);
					gl.glDrawArrays(GL_TRIANGLES, static_cast<GLint>(run.first), static_cast<GLsizei>(count));
					if (run.glass) {
						gl.glDisable(GL_BLEND);
						gl.glDepthMask(GL_TRUE);
					}
				}
				gl.glBindVertexArray(0);
				break;
			}
			case Kind::Objects:
				if (s.objects == nullptr) break;
				geometry();
				DrawEyeObjects(objects[i], depth_scale);
				glass_waiting.push_back(i);
				break;
			case Kind::Lay: {
				if (s.lay_px.size() < static_cast<size_t>(w) * h || s.lay_z.size() < static_cast<size_t>(w) * h) break;
				gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_lay);
				gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s.lay_px.data());
				gl.glActiveTexture(GL_TEXTURE3);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_lay_z);
				gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, s.lay_z.data());
				geometry();
				gl.glDepthFunc(GL_GEQUAL);
				gl.glUseProgram(_frame_lay_program.program);
				gl.glUniform1i(gl.glGetUniformLocation(_frame_lay_program.program, "u_lay"), 2);
				gl.glUniform1i(gl.glGetUniformLocation(_frame_lay_program.program, "u_lay_z"), 3);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_lay_program.program, "u_near"), depth_scale);
				gl.glBindVertexArray(_frame_empty_vao);
				gl.glDrawArrays(GL_TRIANGLES, 0, 3);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Haze:
				job(_frame_haze_program);
				gl.glUniform3f(gl.glGetUniformLocation(_frame_haze_program.program, "u_haze"), s.colour[0], s.colour[1], s.colour[2]);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_haze_program.program, "u_from"), s.from);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_haze_program.program, "u_reach"), s.depth);
				done();
				break;
			case Kind::FarHaze:
				job(_frame_far_haze_program);
				gl.glUniform3f(gl.glGetUniformLocation(_frame_far_haze_program.program, "u_haze"), s.colour[0], s.colour[1], s.colour[2]);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_far_haze_program.program, "u_from"), s.from);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_far_haze_program.program, "u_to"), s.to);
				gl.glUniform1f(gl.glGetUniformLocation(_frame_far_haze_program.program, "u_horizon"), s.horizon);
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
				job(_frame_shadow_program);
				const GLuint p = _frame_shadow_program.program;
				gl.glUniform1i(gl.glGetUniformLocation(p, "u_map"), 2);
				gl.glUniform3f(gl.glGetUniformLocation(p, "u_sun"), m.sun[0], m.sun[1], m.sun[2]);
				gl.glUniform3f(gl.glGetUniformLocation(p, "u_a"), m.a[0], m.a[1], m.a[2]);
				gl.glUniform3f(gl.glGetUniformLocation(p, "u_b"), m.b[0], m.b[1], m.b[2]);
				gl.glUniform4f(gl.glGetUniformLocation(p, "u_map_place"), m.u0, m.v0, m.texel, m.bias);
				gl.glUniform1f(gl.glGetUniformLocation(p, "u_sun0"), m.sun0);
				done();
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, 0);
				gl.glActiveTexture(GL_TEXTURE0);
				break;
			}
			case Kind::Night:
				job(_frame_night_program);
				gl.glUniform3f(gl.glGetUniformLocation(_frame_night_program.program, "u_night"), s.colour[0], s.colour[1], s.colour[2]);
				done();
				glows(s.glows);
				break;
			case Kind::Glows:
				glows(s.glows);
				break;
			case Kind::GlowAfterNight:
				job(_frame_glow_after_program);
				gl.glActiveTexture(GL_TEXTURE2);
				gl.glBindTexture(GL_TEXTURE_2D, _frame_glow);
				gl.glUniform1i(gl.glGetUniformLocation(_frame_glow_after_program.program, "u_glow"), 2);
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
				job(_frame_lamps_program);
				const GLuint p = _frame_lamps_program.program;
				gl.glUniform1i(gl.glGetUniformLocation(p, "u_lights"), 2);
				gl.glUniform1i(gl.glGetUniformLocation(p, "u_count"), static_cast<GLint>(s.lights.size()));
				gl.glUniform3f(gl.glGetUniformLocation(p, "u_night"), s.colour[0], s.colour[1], s.colour[2]);
				gl.glUniform1f(gl.glGetUniformLocation(p, "u_zu_per_unit"), s.depth);
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
				job(_frame_blasts_program);
				gl.glUniform1i(gl.glGetUniformLocation(_frame_blasts_program.program, "u_blasts"), 2);
				gl.glUniform1i(gl.glGetUniformLocation(_frame_blasts_program.program, "u_count"), static_cast<GLint>(s.blasts.size()));
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
					gl.glUniform1f(gl.glGetUniformLocation(_frame_star_program.program, "u_horizon"), s.horizon);
					gl.glEnable(GL_BLEND);
					gl.glBlendFunc(GL_ONE, GL_ONE);
					gl.glBindVertexArray(_frame_star_vao);
					gl.glBindBuffer(GL_ARRAY_BUFFER, _frame_star_vbo);
					gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(stars.size() * sizeof(float)), stars.data(), GL_STREAM_DRAW);
					gl.glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(stars.size() / 5));
					gl.glDisable(GL_BLEND);
				}
				if (s.moon[2] > 0) {
					const float rad = s.moon[2];
					const float box[4] = {std::max(0.0f, static_cast<float>(static_cast<int>(s.moon[0] - rad * 4))), std::max(0.0f, static_cast<float>(static_cast<int>(s.moon[1] - rad * 4))),
							std::min(w - 1.0f, static_cast<float>(static_cast<int>(s.moon[0] + rad * 4))), std::min(h - 1.0f, static_cast<float>(static_cast<int>(s.moon[1] + rad * 4)))};
					for (int glow = 0; glow < 2; glow++) {
						over(_frame_moon_program);
						const GLuint p = _frame_moon_program.program;
						gl.glUniform3f(gl.glGetUniformLocation(p, "u_moon"), s.moon[0], s.moon[1], s.moon[2]);
						gl.glUniform4f(gl.glGetUniformLocation(p, "u_box"), box[0], box[1], box[2], box[3]);
						gl.glUniform1f(gl.glGetUniformLocation(p, "u_horizon"), s.horizon);
						gl.glUniform1i(gl.glGetUniformLocation(p, "u_glow"), glow);
						if (glow == 1) {
							gl.glEnable(GL_BLEND);
							gl.glBlendFunc(GL_ONE, GL_ONE);
						}
						gl.glBindVertexArray(_frame_empty_vao);
						gl.glDrawArrays(GL_TRIANGLES, 0, 3);
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
		gl.glUniform1i(gl.glGetUniformLocation(_frame_copy_program.program, "u_colour"), 0);
		gl.glBindVertexArray(_frame_empty_vao);
		gl.glDrawArrays(GL_TRIANGLES, 0, 3);
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
			} else if (!job->buildings.empty()) {
				DrawBuildingsOnCard(job->buildings);
				drawn = std::ranges::any_of(job->buildings, [](const ThreeDGpuBuildingPicture *p) { return p->drawn; });
			} else {
				DrawPicturesOnCard(job->pics);
				drawn = std::ranges::any_of(job->pics, [](const ThreeDGpuPicture *p) { return p->drawn; });
			}
		} catch (...) {
			drawn = false;
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
	for (GpuJob *job : _gpu_jobs) job->done = true;
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

/** Whether the card draws the 3D pictures now, not starting it nor waiting for it (the thread making the pictures of the map). */
bool ThreeDGpuReady()
{
	if (_three_d_gpu_off) return false;
	std::lock_guard lock(_gpu_mutex);
	return _gpu_state == GpuState::Ready;
}

/**
 * Whether the card draws the 3D pictures: the first time asked its thread is started and waited for (its context made,
 * its programs compiled). False for good where there is no card to be had, and while the rig has the processor draw.
 */
bool ThreeDGpuAvailable()
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
	_gpu_done.wait(lock, [] { return _gpu_state != GpuState::Starting; });
	return _gpu_state == GpuState::Ready;
}

/**
 * Draw a picture on the card, on whichever thread asks (it waits): its pixels and the lit ones read back.
 * @return false: not drawn (no card, or a picture it cannot hold): the processor draws it
 */
/**
 * Draw the pictures of buildings of the turned map on the card, all at once, on whichever thread asks (it waits).
 * @return false: none drawn (no card); each ThreeDGpuBuildingPicture::drawn tells
 */
bool ThreeDGpuRenderBuildings(std::span<ThreeDGpuBuildingPicture *const> pics)
{
	for (ThreeDGpuBuildingPicture *pic : pics) pic->drawn = false;
	if (pics.empty() || !ThreeDGpuAvailable()) return false;
	GpuJob job{};
	job.buildings = pics;
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready) return false;
	QueueGpuJob(job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
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
}

/** What draws the 3D pictures: the card, or why not. */
std::string ThreeDGpuDescribe()
{
	std::lock_guard lock(_gpu_mutex);
	if (!_gpu_died_at.empty() && _three_d_gpu_off) return fmt::format("the processor (the card brought the game down the last time, at \"{}\")", _gpu_died_at);
	if (_three_d_gpu_off) return "the processor (the card switched off)";
	return _gpu_describe;
}

/** The rig: the pictures the card drew since asked last, and its time. */
std::string ThreeDGpuTimes()
{
	std::lock_guard lock(_gpu_mutex);
	std::string s = fmt::format("card: {} pictures in {:.1f} ms{}, holds {} MB, {} MB put on it, let go {}x", _gpu_pictures, _gpu_ms, _gpu_failed > 0 ? fmt::format(", {} left to the processor", _gpu_failed) : std::string(),
			_card_bytes >> 20, _card_sent.load() >> 20, _card_trims.load());
	_card_sent = 0;
	_card_trims = 0;
	_gpu_ms = 0;
	_gpu_pictures = 0;
	_gpu_failed = 0;
	return s;
}

#else /* no card */

bool ThreeDGpuAvailable() { return false; }
bool ThreeDGpuRenderEye(ThreeDGpuEyePicture &) { return false; }
bool ThreeDGpuRenderEyeFrame(ThreeDGpuEyeFrame &) { return false; }
void ThreeDGpuVideoContext(void *, void *, bool) {}
void ThreeDGpuVideoContextGone() {}
void *ThreeDGpuOwnContext() { return nullptr; }
bool ThreeDGpuShows() { return false; }
bool ThreeDGpuReady() { return false; }
bool ThreeDGpuReadShown(uint32_t, int, int, std::vector<uint32_t> &) { return false; }
bool ThreeDGpuRender(std::span<ThreeDGpuPicture *const>) { return false; }
bool ThreeDGpuRenderBuildings(std::span<ThreeDGpuBuildingPicture *const>) { return false; }
void ThreeDGpuStop() {}
std::string ThreeDGpuDescribe() { return "the processor (no card on this system)"; }
std::string ThreeDGpuTimes() { return "card: none"; }

#endif
