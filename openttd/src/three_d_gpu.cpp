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

std::atomic<bool> _three_d_gpu_off = false;

/* The triangles of the shapes by the parts shown (ThreeDGpuTrianglesOf()), kept for the pictures to come. */
static std::mutex _triangles_mutex;
static std::map<std::tuple<uint64_t, ThreeDParts, bool>, std::pair<std::shared_ptr<const ThreeDGpuTriangles>, uint64_t>> _triangles_kept;
static uint64_t _triangles_clock = 0;
static constexpr size_t TRIANGLES_KEPT = 512;

/**
 * The triangles of a shape the card draws, as RasterObject() of three_d_vehicle_object.cpp draws them: of a vehicle
 * those of the parts its state shows (the driver too), the glass after the rest; of its crew all of them, as they are.
 * @param parts the parts shown (ThreeDVehicleObject::PartsOf())
 * @param vehicle of the vehicle, else of a figure of its crew
 * @param textures how many textures it has (a simpler shape's are its whole shape's)
 */
std::shared_ptr<const ThreeDGpuTriangles> ThreeDGpuTrianglesOf(const ThreeDVehicleShape &shape, const ThreeDParts &parts, bool vehicle, size_t textures)
{
	const auto key = std::make_tuple(shape.uid.value, vehicle ? parts : ThreeDParts{}, vehicle);
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
			bool glass = false;
			if (vehicle) {
				const uint8_t part = tn < shape.tri_part.size() ? shape.tri_part[tn] : 0;
				if (part != ThreeDVehicleShape::PART_GLASS && !ThreeDVehicleShape::Shown(part, parts)) continue;
				glass = shape.IsGlass(tn);
			}
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
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x0100;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_LESS = 0x0201;
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
constexpr GLenum GL_RED = 0x1903;
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
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_MAX_RENDERBUFFER_SIZE = 0x84E8;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STREAM_DRAW = 0x88E0;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
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
	X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor)) \
	X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices)) \
	X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
	X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data)) \
	X(void, glPixelStorei, (GLenum pname, GLint param)) \
	X(void, glReadBuffer, (GLenum src)) \
	X(void, glDrawBuffers, (GLsizei n, const GLenum *bufs)) \
	X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
	X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
	X(void, glBindTexture, (GLenum target, GLuint texture)) \
	X(void, glActiveTexture, (GLenum unit)) \
	X(void, glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *data)) \
	X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
	X(void, glGenFramebuffers, (GLsizei n, GLuint *fbos)) \
	X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *fbos)) \
	X(void, glBindFramebuffer, (GLenum target, GLuint fbo)) \
	X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb)) \
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
	X(void, glUniform3f, (GLint location, GLfloat x, GLfloat y, GLfloat z)) \
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
	ThreeDGpuPicture *pic;
	bool done = false;
	bool ok = false;
};
std::mutex _gpu_mutex;              ///< of all below
std::condition_variable _gpu_wake;  ///< the card's thread: a job, or the end
std::condition_variable _gpu_done;  ///< those asking: a job done, or the card ready (or not)
GpuState _gpu_state = GpuState::NotStarted;
std::deque<GpuJob *> _gpu_jobs;
bool _gpu_stop = false;
std::thread _gpu_thread;
std::string _gpu_describe = "not started";
double _gpu_ms = 0;                 ///< the time the card took for the pictures since the last ThreeDGpuTimes()
uint64_t _gpu_pictures = 0;         ///< and how many
uint64_t _gpu_failed = 0;           ///< and how many it could not draw (the processor did)

/* What the card keeps, by the thing's number (ThreeDUniqueId): the corners of the shapes, their triangles, the
 * textures; let go of the longest not drawn when it holds more than GPU_BYTES_KEPT. Only the card's thread uses these. */
struct GpuShape {
	GLuint vao = 0, pos = 0, nrm = 0, uv = 0;
	bool has_nrm = false;
	size_t bytes = 0;
	uint64_t used = 0;
};
struct GpuIndices {
	GLuint ibo = 0;
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
std::unordered_map<uint64_t, GpuTexture> _card_textures;
size_t _card_bytes = 0;
uint64_t _card_clock = 0;
constexpr size_t GPU_BYTES_KEPT = 384u * 1024 * 1024;

/* The programs, the picture drawn into, the corners of the lights at night. */
struct GpuProgram {
	GLuint program = 0;
	GLint matrix = -1, light = -1, lit = -1, tex = -1, glass = -1, glass_alpha = -1;
	/* The paint (ThreeDGpuPaint). */
	GLint paint = -1, detail = -1, detail_colours = -1, paint_mask = -1, addon_mask = -1, rust_mask = -1;
	GLint colour = -1, target = -1, start = -1, fade = -1, addon_colour = -1, addon_start = -1, tint = -1;
	GLint rust_colour = -1, rust_time = -1, rust_ramp = -1;
};
GpuProgram _mesh_program, _night_program;
GLuint _fbo = 0, _colour_rb = 0, _lit_rb = 0, _depth_rb = 0;
int _fbo_w = 0, _fbo_h = 0;
GLuint _night_vao = 0, _night_vbo = 0;
GLint _max_texture = 0, _max_target = 0;

/* The shapes drawn: a corner of the shape to the picture (u_matrix), lit as RasterObject() lights it -- at the
 * corners, the light between them as the processor's, straight across the picture (noperspective) --; each pixel the
 * texture's texel under it, as the processor picks it, a hole where the texel is 0. The glass laid over what is behind
 * by its alpha, the colour with its alpha multiplied in (as the processor's "over"). */
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
	vec3 c = floor(min(vec3(255.0), own * v_light)) / 255.0;
	o_lit = vec4(0.0);
	o_colour = u_glass == 0 ? vec4(c, 1.0) : vec4(c * u_glass_alpha, u_glass_alpha);
}
)";

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
	using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void *, const EGLint *);
	auto platform_display = reinterpret_cast<GetPlatformDisplay>(_egl.GetProcAddress("eglGetPlatformDisplayEXT"));
	EGLint major = 0, minor = 0;
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
	if (!_egl.BindAPI(EGL_OPENGL_API)) {
		why = "EGL without OpenGL";
		return false;
	}
	const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	const char *extensions = _egl.QueryString(_egl.display, EGL_EXTENSIONS);
	const std::string_view ext = extensions != nullptr ? extensions : "";
	/* Without a configuration and a surface where EGL can; else a pixel's buffer of its own. */
	if (ext.find("EGL_KHR_no_config_context") != std::string_view::npos && ext.find("EGL_KHR_surfaceless_context") != std::string_view::npos) {
		_egl.context = _egl.CreateContext(_egl.display, nullptr, nullptr, context_attributes);
		if (_egl.context != nullptr && !_egl.MakeCurrent(_egl.display, nullptr, nullptr, _egl.context)) {
			_egl.DestroyContext(_egl.display, _egl.context);
			_egl.context = nullptr;
		}
	}
	if (_egl.context == nullptr) {
		const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
		EGLConfig config = nullptr;
		EGLint configs = 0;
		if (!_egl.ChooseConfig(_egl.display, config_attributes, &config, 1, &configs) || configs < 1) {
			why = "no EGL configuration for OpenGL";
			return false;
		}
		const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		_egl.surface = _egl.CreatePbufferSurface(_egl.display, config, surface_attributes);
		_egl.context = _egl.CreateContext(_egl.display, config, nullptr, context_attributes);
		if (_egl.surface == nullptr || _egl.context == nullptr || !_egl.MakeCurrent(_egl.display, _egl.surface, _egl.surface, _egl.context)) {
			why = fmt::format("no context of OpenGL 3.3 (EGL error {:#x})", _egl.GetError());
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
		_egl.Terminate(_egl.display);
	}
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
	_gpu_window = CreateWindowW(L"STATIC", L"OpenTTD 3D", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (_gpu_window == nullptr) {
		why = "no window for the context";
		return false;
	}
	_gpu_dc = GetDC(_gpu_window);
	PIXELFORMATDESCRIPTOR pfd = {};
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.iLayerType = PFD_MAIN_PLANE;
	const int format = ChoosePixelFormat(_gpu_dc, &pfd);
	if (format == 0 || !SetPixelFormat(_gpu_dc, format, &pfd)) {
		why = "no pixel format for OpenGL";
		return false;
	}
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
		HGLRC core = create(_gpu_dc, nullptr, attributes);
		if (core != nullptr && wglMakeCurrent(_gpu_dc, core)) {
			wglDeleteContext(first);
			_gpu_rc = core;
		} else if (core != nullptr) {
			wglDeleteContext(core);
			wglMakeCurrent(_gpu_dc, first);
		}
	}
	return true;
}

void EndContext()
{
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
	return true;
}

/** The context, the functions, the programs: whether the card draws, and what it is. */
bool StartCard(std::string &why, std::string &describe)
{
	if (!StartContext(why) || !LoadGlFunctions(why)) return false;
	const char *version = reinterpret_cast<const char *>(gl.glGetString(GL_VERSION));
	const char *renderer = reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER));
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
	if (!MakeProgram(_mesh_program, MESH_VERTEX, MESH_FRAGMENT, why) || !MakeProgram(_night_program, NIGHT_VERTEX, NIGHT_FRAGMENT, why)) return false;
	gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_max_texture);
	gl.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &_max_target);
	_max_target = std::min(_max_target, 16384);
	gl.glGenFramebuffers(1, &_fbo);
	gl.glGenRenderbuffers(1, &_colour_rb);
	gl.glGenRenderbuffers(1, &_lit_rb);
	gl.glGenRenderbuffers(1, &_depth_rb);
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
	describe = fmt::format("{}, OpenGL {}, textures up to {}, pictures up to {}", renderer != nullptr ? renderer : "?", version, _max_texture, _max_target);
	return true;
}

void ForgetCard()
{
	for (auto &[uid, s] : _card_shapes) {
		gl.glDeleteVertexArrays(1, &s.vao);
		const GLuint buffers[] = {s.pos, s.nrm, s.uv};
		for (GLuint b : buffers) if (b != 0) gl.glDeleteBuffers(1, &b);
	}
	for (auto &[uid, i] : _card_indices) gl.glDeleteBuffers(1, &i.ibo);
	for (auto &[uid, t] : _card_textures) gl.glDeleteTextures(1, &t.name);
	_card_shapes.clear();
	_card_indices.clear();
	_card_textures.clear();
	_card_bytes = 0;
}

void EndCard()
{
	if (gl.glDeleteBuffers != nullptr) {
		ForgetCard();
		if (_fbo != 0) gl.glDeleteFramebuffers(1, &_fbo);
		const GLuint rbs[] = {_colour_rb, _lit_rb, _depth_rb};
		for (GLuint rb : rbs) if (rb != 0) gl.glDeleteRenderbuffers(1, &rb);
		if (_night_vbo != 0) gl.glDeleteBuffers(1, &_night_vbo);
		if (_night_vao != 0) gl.glDeleteVertexArrays(1, &_night_vao);
		if (_mesh_program.program != 0) gl.glDeleteProgram(_mesh_program.program);
		if (_night_program.program != 0) gl.glDeleteProgram(_night_program.program);
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
	std::ranges::sort(held, [](const Held &a, const Held &b) { return a.used < b.used; });
	for (const Held &h : held) {
		if (_card_bytes <= GPU_BYTES_KEPT / 4 * 3) break;
		if (h.kind == 0) {
			GpuShape &s = _card_shapes[h.uid];
			gl.glDeleteVertexArrays(1, &s.vao);
			const GLuint buffers[] = {s.pos, s.nrm, s.uv};
			for (GLuint b : buffers) if (b != 0) gl.glDeleteBuffers(1, &b);
			_card_bytes -= s.bytes;
			_card_shapes.erase(h.uid);
		} else if (h.kind == 1) {
			GpuIndices &i = _card_indices[h.uid];
			gl.glDeleteBuffers(1, &i.ibo);
			_card_bytes -= i.bytes;
			_card_indices.erase(h.uid);
		} else {
			GpuTexture &t = _card_textures[h.uid];
			gl.glDeleteTextures(1, &t.name);
			_card_bytes -= t.bytes;
			_card_textures.erase(h.uid);
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
	gl.glBindVertexArray(0);
	g.bytes = corners * sizeof(float) * (g.has_nrm ? 8 : 5);
	g.used = _card_clock;
	_card_bytes += g.bytes;
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
	gl.glGenBuffers(1, &g.ibo);
	gl.glBindBuffer(GL_ARRAY_BUFFER, g.ibo);
	gl.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(t.indices.size() * sizeof(uint32_t)), t.indices.data(), GL_STATIC_DRAW);
	g.bytes = t.indices.size() * sizeof(uint32_t);
	g.used = _card_clock;
	_card_bytes += g.bytes;
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

/** Draw a picture on the card and read it back (ThreeDGpuRender()). */
bool DrawOnCard(ThreeDGpuPicture &pic)
{
	const int w = pic.width, h = pic.height;
	if (w <= 0 || h <= 0 || w > _max_target || h > _max_target) return false;
	_card_clock++;
	/* All on the card first: what it cannot hold the processor draws. */
	/* A texture's paint on the card: its masks' names (ThreeDGpuPaint). */
	struct PaintOnCard {
		GLuint detail = 0, detail_colours = 0, paint_mask = 0, addon_mask = 0, rust_mask = 0;
	};
	struct Draw {
		const ThreeDGpuMesh *mesh;
		const GpuShape *shape;
		const GpuIndices *indices;
		std::vector<GLuint> textures;
		std::vector<PaintOnCard> paints;
	};
	std::vector<Draw> draws;
	for (const ThreeDGpuMesh &m : pic.meshes) {
		if (m.shape == nullptr || m.triangles == nullptr || m.triangles->indices.empty()) continue;
		Draw d{&m, ShapeOnCard(*m.shape), IndicesOnCard(*m.triangles), {}, {}};
		if (d.shape == nullptr || d.indices == nullptr) return false;
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
	if (!TargetOfSize(w, h)) return false;
	static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
	static const GLenum colour_only[2] = {GL_COLOR_ATTACHMENT0, GL_NONE};
	gl.glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
	gl.glViewport(0, 0, w, h);
	gl.glDrawBuffers(2, both);
	gl.glDisable(GL_SCISSOR_TEST);
	gl.glDisable(GL_DITHER);
	gl.glDisable(GL_CULL_FACE);
	gl.glDisable(GL_BLEND);
	gl.glEnable(GL_DEPTH_TEST);
	gl.glDepthFunc(GL_LESS);
	gl.glDepthMask(GL_TRUE);
	gl.glClearColor(0, 0, 0, 0);
	gl.glClearDepth(1.0);
	gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	/* The picture's pixels and depths to the card's: a pixel x, y of the picture is the card's pixel x, y, its rows
	 * from the bottom -- so the rows read back are the picture's from the top. */
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
	auto draw_runs = [&](const Draw &d, const std::vector<ThreeDGpuTriangles::Run> &runs, bool glass) {
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
			gl.glBindTexture(GL_TEXTURE_2D, d.textures[r.texture]);
			gl.glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(static_cast<uintptr_t>(r.first) * sizeof(uint32_t)));
		}
	};
	for (const Draw &d : draws) draw_runs(d, d.mesh->triangles->opaque, false);
	/* The glass after all else, over what is behind it, hiding nothing (no depth written), not lit. */
	gl.glDrawBuffers(2, colour_only);
	gl.glDepthMask(GL_FALSE);
	gl.glEnable(GL_BLEND);
	gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	for (const Draw &d : draws) {
		if (!d.mesh->triangles->glass.empty()) draw_runs(d, d.mesh->triangles->glass, true);
	}
	gl.glDisable(GL_BLEND);
	gl.glDepthMask(GL_TRUE);
	gl.glDrawBuffers(2, both);
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
	pic.pixels.resize(static_cast<size_t>(w) * h);
	gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.glReadBuffer(GL_COLOR_ATTACHMENT0);
	gl.glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pic.pixels.data());
	if (!pic.night.empty()) {
		pic.lit.resize(static_cast<size_t>(w) * h);
		gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
		gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
		gl.glReadPixels(0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, pic.lit.data());
	} else {
		pic.lit.clear();
	}
	gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
	const GLenum error = gl.glGetError();
	if (error != 0) {
		Debug(misc, 1, "3D on the graphics card: OpenGL error {:#x}, the processor draws the picture", error);
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
		const auto start = std::chrono::steady_clock::now();
		bool drawn = false;
		try {
			drawn = DrawOnCard(*job->pic);
		} catch (...) {
			drawn = false;
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		lock.lock();
		job->ok = drawn;
		job->done = true;
		_gpu_ms += ms;
		_gpu_pictures++;
		if (!drawn) _gpu_failed++;
		_gpu_done.notify_all();
	}
	for (GpuJob *job : _gpu_jobs) job->done = true;
	_gpu_jobs.clear();
	_gpu_state = GpuState::Failed;
	_gpu_done.notify_all();
	lock.unlock();
	EndCard();
}

} // namespace

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
bool ThreeDGpuRender(ThreeDGpuPicture &pic)
{
	if (!ThreeDGpuAvailable()) return false;
	GpuJob job{&pic};
	std::unique_lock lock(_gpu_mutex);
	if (_gpu_state != GpuState::Ready) return false;
	_gpu_jobs.push_back(&job);
	_gpu_wake.notify_one();
	_gpu_done.wait(lock, [&job] { return job.done; });
	return job.ok;
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
	if (_three_d_gpu_off) return "the processor (the card switched off)";
	return _gpu_describe;
}

/** The rig: the pictures the card drew since asked last, and its time. */
std::string ThreeDGpuTimes()
{
	std::lock_guard lock(_gpu_mutex);
	std::string s = fmt::format("card: {} pictures in {:.1f} ms{}", _gpu_pictures, _gpu_ms, _gpu_failed > 0 ? fmt::format(", {} left to the processor", _gpu_failed) : std::string());
	_gpu_ms = 0;
	_gpu_pictures = 0;
	_gpu_failed = 0;
	return s;
}

#else /* no card */

bool ThreeDGpuAvailable() { return false; }
bool ThreeDGpuRender(ThreeDGpuPicture &) { return false; }
void ThreeDGpuStop() {}
std::string ThreeDGpuDescribe() { return "the processor (no card on this system)"; }
std::string ThreeDGpuTimes() { return "card: none"; }

#endif
