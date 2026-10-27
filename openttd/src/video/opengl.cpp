/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file opengl.cpp OpenGL video driver support. */

#include "../stdafx.h"

/* Define to disable buffer syncing. Will increase max fast forward FPS but produces artifacts. Mainly useful for performance testing. */
// #define NO_GL_BUFFER_SYNC
/* Define to allow software rendering backends. */
// #define GL_ALLOW_SOFTWARE_RENDERER

#if defined(_WIN32)
#	include <windows.h>
#endif

#define GL_GLEXT_PROTOTYPES
#if defined(__APPLE__)
#	define GL_SILENCE_DEPRECATION
#	include <OpenGL/gl3.h>
#else
#	include <GL/gl.h>
#endif
#include "../3rdparty/opengl/glext.h"

#include "opengl.h"
#include "../core/geometry_func.hpp"
#include "../fileio_func.h"
#include "../openttd.h"
#include "../core/math_func.hpp"
#include "../gfx_func.h"
#include "../debug.h"
#include "../blitter/factory.hpp"
#include "../zoom_func.h"
#include "../core/string_consumer.hpp"

#include "../table/opengl_shader.h"
#include "../table/sprites.h"


#include "../safeguards.h"


/* Define function pointers of all OpenGL functions that we load dynamically. */

#define GL(function) static decltype(&function) _ ## function

GL(glGetString);
GL(glGetIntegerv);
GL(glGetError);
GL(glDebugMessageControl);
GL(glDebugMessageCallback);

GL(glDisable);
GL(glEnable);
GL(glViewport);
GL(glClear);
GL(glClearColor);
GL(glBlendFunc);
GL(glDrawArrays);
GL(glReadPixels);

GL(glTexImage1D);
GL(glTexImage2D);
GL(glTexParameteri);
GL(glTexSubImage1D);
GL(glTexSubImage2D);
GL(glBindTexture);
GL(glDeleteTextures);
GL(glGenTextures);
GL(glPixelStorei);

GL(glActiveTexture);

GL(glGenBuffers);
GL(glDeleteBuffers);
GL(glBindBuffer);
GL(glBufferData);
GL(glBufferSubData);
GL(glMapBuffer);
GL(glUnmapBuffer);
GL(glClearBufferSubData);

GL(glBufferStorage);
GL(glMapBufferRange);
GL(glClientWaitSync);
GL(glFenceSync);
GL(glDeleteSync);

GL(glGenVertexArrays);
GL(glDeleteVertexArrays);
GL(glBindVertexArray);

GL(glCreateProgram);
GL(glDeleteProgram);
GL(glLinkProgram);
GL(glUseProgram);
GL(glGetProgramiv);
GL(glGetProgramInfoLog);
GL(glCreateShader);
GL(glDeleteShader);
GL(glShaderSource);
GL(glCompileShader);
GL(glAttachShader);
GL(glGetShaderiv);
GL(glGetShaderInfoLog);
GL(glGetUniformLocation);
GL(glUniform1i);
GL(glUniform1f);
GL(glUniform2f);
GL(glUniform4f);

GL(glGetAttribLocation);
GL(glEnableVertexAttribArray);
GL(glDisableVertexAttribArray);
GL(glVertexAttribPointer);
GL(glBindFragDataLocation);

#undef GL


/** A simple 2D vertex with just position and texture. */
struct Simple2DVertex {
	float x, y;
	float u, v;
};

/** Maximum number of cursor sprites to cache. */
static const int MAX_CACHED_CURSORS = 48;

/* static */ OpenGLBackend *OpenGLBackend::instance = nullptr;

GetOGLProcAddressProc GetOGLProcAddress;

static std::optional<std::string_view> GlGetString(GLenum name)
{
	auto str = reinterpret_cast<const char *>(_glGetString(name));
	if (str == nullptr) return {};
	return str;
}

/**
 * Find a substring in a string made of space delimited elements. The substring
 * has to match the complete element, partial matches don't count.
 * @param string List of space delimited elements.
 * @param substring Substring to find.
 * @return Whether the substring was found.
 */
bool HasStringInExtensionList(std::string_view string, std::string_view substring)
{
	StringConsumer consumer{string};
	while (consumer.AnyBytesLeft()) {
		if (substring == consumer.ReadUntil(" ", StringConsumer::SKIP_ALL_SEPARATORS)) return true;
	}

	return false;
}

/**
 * Check if an OpenGL extension is supported by the current context.
 * @param extension The extension string to test.
 * @return True if the extension is supported, false if not.
 */
static bool IsOpenGLExtensionSupported(std::string_view extension)
{
	static PFNGLGETSTRINGIPROC glGetStringi = nullptr;
	static bool glGetStringi_loaded = false;

	/* Starting with OpenGL 3.0 the preferred API to get the extensions
	 * has changed. Try to load the required function once. */
	if (!glGetStringi_loaded) {
		if (IsOpenGLVersionAtLeast(3, 0)) glGetStringi = (PFNGLGETSTRINGIPROC)GetOGLProcAddress("glGetStringi");
		glGetStringi_loaded = true;
	}

	if (glGetStringi != nullptr) {
		/* New style: Each supported extension can be queried and compared independently. */
		GLint num_exts;
		_glGetIntegerv(GL_NUM_EXTENSIONS, &num_exts);

		for (GLint i = 0; i < num_exts; i++) {
			const char *entry = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, i));
			if (entry != nullptr && entry == extension) return true;
		}
	} else if (auto str = GlGetString(GL_EXTENSIONS); str.has_value()) {
		/* Old style: A single, space-delimited string for all extensions. */
		return HasStringInExtensionList(*str, extension);
	}

	return false;
}

static uint8_t _gl_major_ver = 0; ///< Major OpenGL version.
static uint8_t _gl_minor_ver = 0; ///< Minor OpenGL version.

/**
 * Check if the current OpenGL version is equal or higher than a given one.
 * @param major Minimal major version.
 * @param minor Minimal minor version.
 * @pre OpenGL was initialized.
 * @return True if the OpenGL version is equal or higher than the requested one.
 */
bool IsOpenGLVersionAtLeast(uint8_t major, uint8_t minor)
{
	return (_gl_major_ver > major) || (_gl_major_ver == major && _gl_minor_ver >= minor);
}

/**
 * Try loading an OpenGL function.
 * @tparam F Type of the function pointer.
 * @param f Reference where to store the function pointer in.
 * @param name Name of the function.
 * @return True if the function could be bound.
 */
template <typename F>
static bool BindGLProc(F &f, const char *name)
{
	f = reinterpret_cast<F>(GetOGLProcAddress(name));
	return f != nullptr;
}

/**
 * Bind basic information functions.
 * @return \c true iff all procs could be bound.
 */
static bool BindBasicInfoProcs()
{
	if (!BindGLProc(_glGetString, "glGetString")) return false;
	if (!BindGLProc(_glGetIntegerv, "glGetIntegerv")) return false;
	if (!BindGLProc(_glGetError, "glGetError")) return false;

	return true;
}

/**
 * Bind OpenGL 1.0 and 1.1 functions.
 * @return \c true iff all procs could be bound.
 */
static bool BindBasicOpenGLProcs()
{
	if (!BindGLProc(_glDisable, "glDisable")) return false;
	if (!BindGLProc(_glEnable, "glEnable")) return false;
	if (!BindGLProc(_glViewport, "glViewport")) return false;
	if (!BindGLProc(_glTexImage1D, "glTexImage1D")) return false;
	if (!BindGLProc(_glTexImage2D, "glTexImage2D")) return false;
	if (!BindGLProc(_glTexParameteri, "glTexParameteri")) return false;
	if (!BindGLProc(_glTexSubImage1D, "glTexSubImage1D")) return false;
	if (!BindGLProc(_glTexSubImage2D, "glTexSubImage2D")) return false;
	if (!BindGLProc(_glBindTexture, "glBindTexture")) return false;
	if (!BindGLProc(_glDeleteTextures, "glDeleteTextures")) return false;
	if (!BindGLProc(_glGenTextures, "glGenTextures")) return false;
	if (!BindGLProc(_glPixelStorei, "glPixelStorei")) return false;
	if (!BindGLProc(_glClear, "glClear")) return false;
	if (!BindGLProc(_glClearColor, "glClearColor")) return false;
	if (!BindGLProc(_glBlendFunc, "glBlendFunc")) return false;
	if (!BindGLProc(_glDrawArrays, "glDrawArrays")) return false;
	BindGLProc(_glReadPixels, "glReadPixels"); // (the rig's picture of the screen only)

	return true;
}

/**
 * Bind texture-related extension functions.
 * @return \c true iff all extension procs could be bound.
 */
static bool BindTextureExtensions()
{
	if (IsOpenGLVersionAtLeast(1, 3)) {
		if (!BindGLProc(_glActiveTexture, "glActiveTexture")) return false;
	} else {
		if (!BindGLProc(_glActiveTexture, "glActiveTextureARB")) return false;
	}

	return true;
}

/**
 * Bind vertex buffer object extension functions.
 * @return \c true iff all extension procs could be bound.
 */
static bool BindVBOExtension()
{
	if (IsOpenGLVersionAtLeast(1, 5)) {
		if (!BindGLProc(_glGenBuffers, "glGenBuffers")) return false;
		if (!BindGLProc(_glDeleteBuffers, "glDeleteBuffers")) return false;
		if (!BindGLProc(_glBindBuffer, "glBindBuffer")) return false;
		if (!BindGLProc(_glBufferData, "glBufferData")) return false;
		if (!BindGLProc(_glBufferSubData, "glBufferSubData")) return false;
		if (!BindGLProc(_glMapBuffer, "glMapBuffer")) return false;
		if (!BindGLProc(_glUnmapBuffer, "glUnmapBuffer")) return false;
	} else {
		if (!BindGLProc(_glGenBuffers, "glGenBuffersARB")) return false;
		if (!BindGLProc(_glDeleteBuffers, "glDeleteBuffersARB")) return false;
		if (!BindGLProc(_glBindBuffer, "glBindBufferARB")) return false;
		if (!BindGLProc(_glBufferData, "glBufferDataARB")) return false;
		if (!BindGLProc(_glBufferSubData, "glBufferSubDataARB")) return false;
		if (!BindGLProc(_glMapBuffer, "glMapBufferARB")) return false;
		if (!BindGLProc(_glUnmapBuffer, "glUnmapBufferARB")) return false;
	}

	if (IsOpenGLVersionAtLeast(4, 3) || IsOpenGLExtensionSupported("GL_ARB_clear_buffer_object")) {
		BindGLProc(_glClearBufferSubData, "glClearBufferSubData");
	} else {
		_glClearBufferSubData = nullptr;
	}

	return true;
}

/**
 * Bind vertex array object extension functions.
 * @return \c true iff all extension procs could be bound.
 */
static bool BindVBAExtension()
{
	/* The APPLE and ARB variants have different semantics (that don't matter for us).
	 *  Successfully getting pointers to one variant doesn't mean it is supported for
	 *  the current context. Always check the extension strings as well. */
	if (IsOpenGLVersionAtLeast(3, 0) || IsOpenGLExtensionSupported("GL_ARB_vertex_array_object")) {
		if (!BindGLProc(_glGenVertexArrays, "glGenVertexArrays")) return false;
		if (!BindGLProc(_glDeleteVertexArrays, "glDeleteVertexArrays")) return false;
		if (!BindGLProc(_glBindVertexArray, "glBindVertexArray")) return false;
	} else if (IsOpenGLExtensionSupported("GL_APPLE_vertex_array_object")) {
		if (!BindGLProc(_glGenVertexArrays, "glGenVertexArraysAPPLE")) return false;
		if (!BindGLProc(_glDeleteVertexArrays, "glDeleteVertexArraysAPPLE")) return false;
		if (!BindGLProc(_glBindVertexArray, "glBindVertexArrayAPPLE")) return false;
	}

	return true;
}

/**
 * Bind extension functions for shader support.
 * @return \c true iff all extension procs could be bound.
 */
static bool BindShaderExtensions()
{
	if (IsOpenGLVersionAtLeast(2, 0)) {
		if (!BindGLProc(_glCreateProgram, "glCreateProgram")) return false;
		if (!BindGLProc(_glDeleteProgram, "glDeleteProgram")) return false;
		if (!BindGLProc(_glLinkProgram, "glLinkProgram")) return false;
		if (!BindGLProc(_glUseProgram, "glUseProgram")) return false;
		if (!BindGLProc(_glGetProgramiv, "glGetProgramiv")) return false;
		if (!BindGLProc(_glGetProgramInfoLog, "glGetProgramInfoLog")) return false;
		if (!BindGLProc(_glCreateShader, "glCreateShader")) return false;
		if (!BindGLProc(_glDeleteShader, "glDeleteShader")) return false;
		if (!BindGLProc(_glShaderSource, "glShaderSource")) return false;
		if (!BindGLProc(_glCompileShader, "glCompileShader")) return false;
		if (!BindGLProc(_glAttachShader, "glAttachShader")) return false;
		if (!BindGLProc(_glGetShaderiv, "glGetShaderiv")) return false;
		if (!BindGLProc(_glGetShaderInfoLog, "glGetShaderInfoLog")) return false;
		if (!BindGLProc(_glGetUniformLocation, "glGetUniformLocation")) return false;
		if (!BindGLProc(_glUniform1i, "glUniform1i")) return false;
		if (!BindGLProc(_glUniform1f, "glUniform1f")) return false;
		if (!BindGLProc(_glUniform2f, "glUniform2f")) return false;
		if (!BindGLProc(_glUniform4f, "glUniform4f")) return false;

		if (!BindGLProc(_glGetAttribLocation, "glGetAttribLocation")) return false;
		if (!BindGLProc(_glEnableVertexAttribArray, "glEnableVertexAttribArray")) return false;
		if (!BindGLProc(_glDisableVertexAttribArray, "glDisableVertexAttribArray")) return false;
		if (!BindGLProc(_glVertexAttribPointer, "glVertexAttribPointer")) return false;
	} else {
		/* In the ARB extension programs and shaders are in the same object space. */
		if (!BindGLProc(_glCreateProgram, "glCreateProgramObjectARB")) return false;
		if (!BindGLProc(_glDeleteProgram, "glDeleteObjectARB")) return false;
		if (!BindGLProc(_glLinkProgram, "glLinkProgramARB")) return false;
		if (!BindGLProc(_glUseProgram, "glUseProgramObjectARB")) return false;
		if (!BindGLProc(_glGetProgramiv, "glGetObjectParameterivARB")) return false;
		if (!BindGLProc(_glGetProgramInfoLog, "glGetInfoLogARB")) return false;
		if (!BindGLProc(_glCreateShader, "glCreateShaderObjectARB")) return false;
		if (!BindGLProc(_glDeleteShader, "glDeleteObjectARB")) return false;
		if (!BindGLProc(_glShaderSource, "glShaderSourceARB")) return false;
		if (!BindGLProc(_glCompileShader, "glCompileShaderARB")) return false;
		if (!BindGLProc(_glAttachShader, "glAttachObjectARB")) return false;
		if (!BindGLProc(_glGetShaderiv, "glGetObjectParameterivARB")) return false;
		if (!BindGLProc(_glGetShaderInfoLog, "glGetInfoLogARB")) return false;
		if (!BindGLProc(_glGetUniformLocation, "glGetUniformLocationARB")) return false;
		if (!BindGLProc(_glUniform1i, "glUniform1iARB")) return false;
		if (!BindGLProc(_glUniform1f, "glUniform1fARB")) return false;
		if (!BindGLProc(_glUniform2f, "glUniform2fARB")) return false;
		if (!BindGLProc(_glUniform4f, "glUniform4fARB")) return false;

		if (!BindGLProc(_glGetAttribLocation, "glGetAttribLocationARB")) return false;
		if (!BindGLProc(_glEnableVertexAttribArray, "glEnableVertexAttribArrayARB")) return false;
		if (!BindGLProc(_glDisableVertexAttribArray, "glDisableVertexAttribArrayARB")) return false;
		if (!BindGLProc(_glVertexAttribPointer, "glVertexAttribPointerARB")) return false;
	}

	/* Bind functions only needed when using GLSL 1.50 shaders. */
	if (IsOpenGLVersionAtLeast(3, 0)) {
		BindGLProc(_glBindFragDataLocation, "glBindFragDataLocation");
	} else if (IsOpenGLExtensionSupported("GL_EXT_gpu_shader4")) {
		BindGLProc(_glBindFragDataLocation, "glBindFragDataLocationEXT");
	} else {
		_glBindFragDataLocation = nullptr;
	}

	return true;
}

/**
 * Bind extension functions for persistent buffer mapping.
 * @return \c true iff all extension procs could be bound.
 */
static bool BindPersistentBufferExtensions()
{
	/* Optional functions for persistent buffer mapping. */
	if (IsOpenGLVersionAtLeast(3, 0)) {
		if (!BindGLProc(_glMapBufferRange, "glMapBufferRange")) return false;
	}
	if (IsOpenGLVersionAtLeast(4, 4) || IsOpenGLExtensionSupported("GL_ARB_buffer_storage")) {
		if (!BindGLProc(_glBufferStorage, "glBufferStorage")) return false;
	}
#ifndef NO_GL_BUFFER_SYNC
	if (IsOpenGLVersionAtLeast(3, 2) || IsOpenGLExtensionSupported("GL_ARB_sync")) {
		if (!BindGLProc(_glClientWaitSync, "glClientWaitSync")) return false;
		if (!BindGLProc(_glFenceSync, "glFenceSync")) return false;
		if (!BindGLProc(_glDeleteSync, "glDeleteSync")) return false;
	}
#endif

	return true;
}

/**
 * Callback to receive OpenGL debug messages.
 * @param type The type of message.
 * @param severity The severity of the issue.
 * @param message The message to convey to the end user.
 */
void APIENTRY DebugOutputCallback(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar *message, const void *)
{
	/* Make severity human readable. */
	std::string_view severity_str;
	switch (severity) {
		case GL_DEBUG_SEVERITY_HIGH:   severity_str = "high"; break;
		case GL_DEBUG_SEVERITY_MEDIUM: severity_str = "medium"; break;
		case GL_DEBUG_SEVERITY_LOW:    severity_str = "low"; break;
	}

	/* Make type human readable.*/
	std::string_view type_str = "Other";
	switch (type) {
		case GL_DEBUG_TYPE_ERROR:               type_str = "Error"; break;
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: type_str = "Deprecated"; break;
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  type_str = "Undefined behaviour"; break;
		case GL_DEBUG_TYPE_PERFORMANCE:         type_str = "Performance"; break;
		case GL_DEBUG_TYPE_PORTABILITY:         type_str = "Portability"; break;
	}

	Debug(driver, 6, "OpenGL: {} ({}) - {}", type_str, severity_str, message);
}

/** Enable OpenGL debug messages if supported. */
void SetupDebugOutput()
{
#ifndef NO_DEBUG_MESSAGES
	if (_debug_driver_level < 6) return;

	if (IsOpenGLVersionAtLeast(4, 3)) {
		BindGLProc(_glDebugMessageControl, "glDebugMessageControl");
		BindGLProc(_glDebugMessageCallback, "glDebugMessageCallback");
	} else if (IsOpenGLExtensionSupported("GL_ARB_debug_output")) {
		BindGLProc(_glDebugMessageControl, "glDebugMessageControlARB");
		BindGLProc(_glDebugMessageCallback, "glDebugMessageCallbackARB");
	}

	if (_glDebugMessageControl != nullptr && _glDebugMessageCallback != nullptr) {
		/* Enable debug output. As synchronous debug output costs performance, we only enable it with a high debug level. */
		_glEnable(GL_DEBUG_OUTPUT);
		if (_debug_driver_level >= 8) _glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);

		_glDebugMessageCallback(&DebugOutputCallback, nullptr);
		/* Enable all messages on highest debug level.*/
		_glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, _debug_driver_level >= 9 ? GL_TRUE : GL_FALSE);
		/* Get debug messages for errors and undefined/deprecated behaviour. */
		_glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR, GL_DONT_CARE, 0, nullptr, GL_TRUE);
		_glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR, GL_DONT_CARE, 0, nullptr, GL_TRUE);
		_glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, GL_DONT_CARE, 0, nullptr, GL_TRUE);
	}
#endif
}

/**
 * Create and initialize the singleton back-end class.
 * @param get_proc Callback to get an OpenGL function from the OS driver.
 * @param screen_res Current display resolution.
 * @return std::nullopt on success, error message otherwise.
 */
/* static */ std::optional<std::string_view> OpenGLBackend::Create(GetOGLProcAddressProc get_proc, const Dimension &screen_res)
{
	if (OpenGLBackend::instance != nullptr) OpenGLBackend::Destroy();

	GetOGLProcAddress = get_proc;

	OpenGLBackend::instance = new OpenGLBackend();
	return OpenGLBackend::instance->Init(screen_res);
}

/**
 * Free resources and destroy singleton back-end class.
 */
/* static */ void OpenGLBackend::Destroy()
{
	delete OpenGLBackend::instance;
	OpenGLBackend::instance = nullptr;
}

/**
 * Construct OpenGL back-end class.
 */
OpenGLBackend::OpenGLBackend() : cursor_cache(MAX_CACHED_CURSORS)
{
}

/**
 * Free allocated resources.
 */
OpenGLBackend::~OpenGLBackend()
{
	if (_glDeleteProgram != nullptr) {
		_glDeleteProgram(this->remap_program);
		_glDeleteProgram(this->vid_program);
		_glDeleteProgram(this->pal_program);
		_glDeleteProgram(this->sprite_program);
	}
	if (_glDeleteVertexArrays != nullptr) _glDeleteVertexArrays(1, &this->vao_quad);
	if (_glDeleteBuffers != nullptr) {
		_glDeleteBuffers(1, &this->vbo_quad);
		_glDeleteBuffers(1, &this->vid_pbo);
		_glDeleteBuffers(1, &this->anim_pbo);
	}
	if (_glDeleteTextures != nullptr) {
		this->InternalClearCursorCache();
		OpenGLSprite::Destroy();

		_glDeleteTextures(1, &this->vid_texture);
		_glDeleteTextures(1, &this->anim_texture);
		_glDeleteTextures(1, &this->pal_texture);
	}
}

static std::tuple<uint8_t, uint8_t> DecodeVersion(std::string_view ver)
{
	StringConsumer consumer{ver};
	int major = consumer.ReadIntegerBase<uint8_t>(10);
	if (consumer.ReadIf(".")) return {major, consumer.ReadIntegerBase<uint8_t>(10)};
	return {major, 0};
}

/**
 * Check for the needed OpenGL functionality and allocate all resources.
 * @param screen_res Current display resolution.
 * @return Error string or std::nullopt if successful.
 */
std::optional<std::string_view> OpenGLBackend::Init(const Dimension &screen_res)
{
	if (!BindBasicInfoProcs()) return "OpenGL not supported";

	/* Always query the supported OpenGL version as the current context might have changed. */
	auto ver = GlGetString(GL_VERSION);
	auto vend = GlGetString(GL_VENDOR);
	auto renderer = GlGetString(GL_RENDERER);

	if (!ver.has_value() || !vend.has_value() || !renderer.has_value()) return "OpenGL not supported";

	Debug(driver, 1, "OpenGL driver: {} - {} ({})", *vend, *renderer, *ver);

#ifndef GL_ALLOW_SOFTWARE_RENDERER
	/* Don't use MESA software rendering backends as they are slower than
	 * just using a non-OpenGL video driver. */
	if (renderer->starts_with("llvmpipe") || renderer->starts_with("softpipe")) return "Software renderer detected, not using OpenGL";
#endif

	std::tie(_gl_major_ver, _gl_minor_ver) = DecodeVersion(*ver);

#ifdef _WIN32
	/* Old drivers on Windows (especially if made by Intel) seem to be
	 * unstable, so cull the oldest stuff here.  */
	if (!IsOpenGLVersionAtLeast(3, 2)) return "Need at least OpenGL version 3.2 on Windows";
#endif

	if (!BindBasicOpenGLProcs()) return "Failed to bind basic OpenGL functions.";

	SetupDebugOutput();

	/* OpenGL 1.3 is the absolute minimum. */
	if (!IsOpenGLVersionAtLeast(1, 3)) return "OpenGL version >= 1.3 required";
	/* Check for non-power-of-two texture support. */
	if (!IsOpenGLVersionAtLeast(2, 0) && !IsOpenGLExtensionSupported("GL_ARB_texture_non_power_of_two")) return "Non-power-of-two textures not supported";
	/* Check for single element texture formats. */
	if (!IsOpenGLVersionAtLeast(3, 0) && !IsOpenGLExtensionSupported("GL_ARB_texture_rg")) return "Single element texture formats not supported";
	if (!BindTextureExtensions()) return "Failed to bind texture extension functions";
	/* Check for vertex buffer objects. */
	if (!IsOpenGLVersionAtLeast(1, 5) && !IsOpenGLExtensionSupported("ARB_vertex_buffer_object")) return "Vertex buffer objects not supported";
	if (!BindVBOExtension()) return "Failed to bind VBO extension functions";
	/* Check for pixel buffer objects. */
	if (!IsOpenGLVersionAtLeast(2, 1) && !IsOpenGLExtensionSupported("GL_ARB_pixel_buffer_object")) return "Pixel buffer objects not supported";
	/* Check for vertex array objects. */
	if (!IsOpenGLVersionAtLeast(3, 0) && (!IsOpenGLExtensionSupported("GL_ARB_vertex_array_object") || !IsOpenGLExtensionSupported("GL_APPLE_vertex_array_object"))) return "Vertex array objects not supported";
	if (!BindVBAExtension()) return "Failed to bind VBA extension functions";
	/* Check for shader objects. */
	if (!IsOpenGLVersionAtLeast(2, 0) && (!IsOpenGLExtensionSupported("GL_ARB_shader_objects") || !IsOpenGLExtensionSupported("GL_ARB_fragment_shader") || !IsOpenGLExtensionSupported("GL_ARB_vertex_shader"))) return "No shader support";
	if (!BindShaderExtensions()) return "Failed to bind shader extension functions";
	if (IsOpenGLVersionAtLeast(3, 2) && _glBindFragDataLocation == nullptr) return "OpenGL claims to support version 3.2 but doesn't have glBindFragDataLocation";

	this->persistent_mapping_supported = IsOpenGLVersionAtLeast(3, 0) && (IsOpenGLVersionAtLeast(4, 4) || IsOpenGLExtensionSupported("GL_ARB_buffer_storage"));
#ifndef NO_GL_BUFFER_SYNC
	this->persistent_mapping_supported = this->persistent_mapping_supported && (IsOpenGLVersionAtLeast(3, 2) || IsOpenGLExtensionSupported("GL_ARB_sync"));
#endif

	if (this->persistent_mapping_supported && !BindPersistentBufferExtensions()) {
		Debug(driver, 1, "OpenGL claims to support persistent buffer mapping but doesn't export all functions, not using persistent mapping.");
		this->persistent_mapping_supported = false;
	}
	if (this->persistent_mapping_supported) Debug(driver, 3, "OpenGL: Using persistent buffer mapping");

	/* Check maximum texture size against screen resolution. */
	GLint max_tex_size = 0;
	_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
	if (std::max(screen_res.width, screen_res.height) > (uint)max_tex_size) return "Max supported texture size is too small";

	/* Check available texture units. */
	GLint max_tex_units = 0;
	_glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_tex_units);
	if (max_tex_units < 4) return "Not enough simultaneous textures supported";

	Debug(driver, 2, "OpenGL shading language version: {}, texture units = {}", GlGetString(GL_SHADING_LANGUAGE_VERSION).value_or("Unknown version"), max_tex_units);

	if (!this->InitShaders()) return "Failed to initialize shaders";

	/* Setup video buffer texture. */
	_glGenTextures(1, &this->vid_texture);
	_glBindTexture(GL_TEXTURE_2D, this->vid_texture);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glBindTexture(GL_TEXTURE_2D, 0);
	if (_glGetError() != GL_NO_ERROR) return "Can't generate video buffer texture";

	/* Setup video buffer texture. */
	_glGenTextures(1, &this->anim_texture);
	_glBindTexture(GL_TEXTURE_2D, this->anim_texture);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glBindTexture(GL_TEXTURE_2D, 0);
	if (_glGetError() != GL_NO_ERROR) return "Can't generate animation buffer texture";

	/* Setup palette texture. */
	_glGenTextures(1, &this->pal_texture);
	_glBindTexture(GL_TEXTURE_1D, this->pal_texture);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAX_LEVEL, 0);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, 256, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
	_glBindTexture(GL_TEXTURE_1D, 0);
	if (_glGetError() != GL_NO_ERROR) return "Can't generate palette lookup texture";

	/* Bind uniforms in rendering shader program. */
	GLint tex_location = _glGetUniformLocation(this->vid_program, "colour_tex");
	GLint palette_location = _glGetUniformLocation(this->vid_program, "palette");
	GLint sprite_location = _glGetUniformLocation(this->vid_program, "sprite");
	GLint screen_location = _glGetUniformLocation(this->vid_program, "screen");
	_glUseProgram(this->vid_program);
	_glUniform1i(tex_location, 0);     // Texture unit 0.
	_glUniform1i(palette_location, 1); // Texture unit 1.
	/* Values that result in no transform. */
	_glUniform4f(sprite_location, 0.0f, 0.0f, 1.0f, 1.0f);
	_glUniform2f(screen_location, 1.0f, 1.0f);

	/* Bind uniforms in palette rendering shader program. */
	tex_location = _glGetUniformLocation(this->pal_program, "colour_tex");
	palette_location = _glGetUniformLocation(this->pal_program, "palette");
	sprite_location = _glGetUniformLocation(this->pal_program, "sprite");
	screen_location = _glGetUniformLocation(this->pal_program, "screen");
	_glUseProgram(this->pal_program);
	_glUniform1i(tex_location, 0);     // Texture unit 0.
	_glUniform1i(palette_location, 1); // Texture unit 1.
	_glUniform4f(sprite_location, 0.0f, 0.0f, 1.0f, 1.0f);
	_glUniform2f(screen_location, 1.0f, 1.0f);

	/* Bind uniforms in remap shader program. */
	tex_location = _glGetUniformLocation(this->remap_program, "colour_tex");
	palette_location = _glGetUniformLocation(this->remap_program, "palette");
	GLint remap_location = _glGetUniformLocation(this->remap_program, "remap_tex");
	this->remap_sprite_loc = _glGetUniformLocation(this->remap_program, "sprite");
	this->remap_screen_loc = _glGetUniformLocation(this->remap_program, "screen");
	this->remap_zoom_loc = _glGetUniformLocation(this->remap_program, "zoom");
	this->remap_rgb_loc = _glGetUniformLocation(this->remap_program, "rgb");
	_glUseProgram(this->remap_program);
	_glUniform1i(tex_location, 0);     // Texture unit 0.
	_glUniform1i(palette_location, 1); // Texture unit 1.
	_glUniform1i(remap_location, 2);   // Texture unit 2.

	/* Bind uniforms in sprite shader program. */
	tex_location = _glGetUniformLocation(this->sprite_program, "colour_tex");
	palette_location = _glGetUniformLocation(this->sprite_program, "palette");
	remap_location = _glGetUniformLocation(this->sprite_program, "remap_tex");
	GLint pal_location = _glGetUniformLocation(this->sprite_program, "pal");
	this->sprite_sprite_loc = _glGetUniformLocation(this->sprite_program, "sprite");
	this->sprite_screen_loc = _glGetUniformLocation(this->sprite_program, "screen");
	this->sprite_zoom_loc = _glGetUniformLocation(this->sprite_program, "zoom");
	this->sprite_rgb_loc = _glGetUniformLocation(this->sprite_program, "rgb");
	this->sprite_crash_loc = _glGetUniformLocation(this->sprite_program, "crash");
	_glUseProgram(this->sprite_program);
	_glUniform1i(tex_location, 0);     // Texture unit 0.
	_glUniform1i(palette_location, 1); // Texture unit 1.
	_glUniform1i(remap_location, 2);   // Texture unit 2.
	_glUniform1i(pal_location, 3);     // Texture unit 3.

	/* Bind uniforms in the program of the graphics card's pictures. */
	if (this->three_d_program != 0) {
		tex_location = _glGetUniformLocation(this->three_d_program, "colour_tex");
		GLint screen_tex_location = _glGetUniformLocation(this->three_d_program, "screen_tex");
		this->three_d_sprite_loc = _glGetUniformLocation(this->three_d_program, "sprite");
		this->three_d_screen_loc = _glGetUniformLocation(this->three_d_program, "screen");
		this->three_d_size_loc = _glGetUniformLocation(this->three_d_program, "screen_size");
		this->three_d_tint_loc = _glGetUniformLocation(this->three_d_program, "tint");
		this->three_d_light_loc = _glGetUniformLocation(this->three_d_program, "light");
		this->three_d_over_loc = _glGetUniformLocation(this->three_d_program, "has_over");
		GLint over_tex_location = _glGetUniformLocation(this->three_d_program, "over_tex");
		GLint over_lit_tex_location = _glGetUniformLocation(this->three_d_program, "over_lit_tex");
		_glUseProgram(this->three_d_program);
		_glUniform1i(tex_location, 0);           // Texture unit 0.
		_glUniform1i(screen_tex_location, 1);    // Texture unit 1.
		_glUniform1i(over_tex_location, 2);      // Texture unit 2.
		_glUniform1i(over_lit_tex_location, 3);  // Texture unit 3.
	}
	(void)_glGetError(); // Clear errors.

	/* Create pixel buffer object as video buffer storage. */
	_glGenBuffers(1, &this->vid_pbo);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vid_pbo);
	_glGenBuffers(1, &this->anim_pbo);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
	if (_glGetError() != GL_NO_ERROR) return "Can't allocate pixel buffer for video buffer";

	/* Prime vertex buffer with a full-screen quad and store
	 * the corresponding state in a vertex array object. */
	static const Simple2DVertex vert_array[] = {
		/*  x     y    u    v */
		{  1.f, -1.f, 1.f, 1.f },
		{  1.f,  1.f, 1.f, 0.f },
		{ -1.f, -1.f, 0.f, 1.f },
		{ -1.f,  1.f, 0.f, 0.f },
	};

	/* Create VAO. */
	_glGenVertexArrays(1, &this->vao_quad);
	_glBindVertexArray(this->vao_quad);

	/* Create and fill VBO. */
	_glGenBuffers(1, &this->vbo_quad);
	_glBindBuffer(GL_ARRAY_BUFFER, this->vbo_quad);
	_glBufferData(GL_ARRAY_BUFFER, sizeof(vert_array), vert_array, GL_STATIC_DRAW);
	if (_glGetError() != GL_NO_ERROR) return "Can't generate VBO for fullscreen quad";

	/* Set vertex state. */
	GLint loc_position = _glGetAttribLocation(this->vid_program, "position");
	GLint colour_position = _glGetAttribLocation(this->vid_program, "colour_uv");
	_glEnableVertexAttribArray(loc_position);
	_glEnableVertexAttribArray(colour_position);
	_glVertexAttribPointer(loc_position, 2, GL_FLOAT, GL_FALSE, sizeof(Simple2DVertex), (GLvoid *)offsetof(Simple2DVertex, x));
	_glVertexAttribPointer(colour_position, 2, GL_FLOAT, GL_FALSE, sizeof(Simple2DVertex), (GLvoid *)offsetof(Simple2DVertex, u));
	_glBindVertexArray(0);

	/* Create resources for sprite rendering. */
	if (!OpenGLSprite::Create()) return "Failed to create sprite rendering resources";

	this->PrepareContext();
	(void)_glGetError(); // Clear errors.

	return std::nullopt;
}

void OpenGLBackend::PrepareContext()
{
	_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	_glDisable(GL_DEPTH_TEST);
	/* Enable alpha blending using the src alpha factor. */
	_glEnable(GL_BLEND);
	_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

std::string OpenGLBackend::GetDriverName()
{
	auto renderer = GlGetString(GL_RENDERER);
	auto version = GlGetString(GL_VERSION);
	/* Skipping GL_VENDOR as it tends to be "obvious" from the renderer and version data, and just makes the string pointlessly longer */
	return fmt::format("{}, {}", renderer.value_or("Unknown renderer"), version.value_or("Unknown version"));
}

/**
 * Check a shader for compilation errors and log them if necessary.
 * @param shader Shader to check.
 * @return True if the shader is valid.
 */
static bool VerifyShader(GLuint shader)
{
	static ReusableBuffer<char> log_buf;

	GLint result = GL_FALSE;
	_glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	/* Output log if there is one. */
	GLint log_len = 0;
	_glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
	if (log_len > 0) {
		_glGetShaderInfoLog(shader, log_len, nullptr, log_buf.Allocate(log_len));
		Debug(driver, result != GL_TRUE ? 0 : 2, "{}", log_buf.GetBuffer()); // Always print on failure.
	}

	return result == GL_TRUE;
}

/**
 * Check a program for link errors and log them if necessary.
 * @param program Program to check.
 * @return True if the program is valid.
 */
static bool VerifyProgram(GLuint program)
{
	static ReusableBuffer<char> log_buf;

	GLint result = GL_FALSE;
	_glGetProgramiv(program, GL_LINK_STATUS, &result);

	/* Output log if there is one. */
	GLint log_len = 0;
	_glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
	if (log_len > 0) {
		_glGetProgramInfoLog(program, log_len, nullptr, log_buf.Allocate(log_len));
		Debug(driver, result != GL_TRUE ? 0 : 2, "{}", log_buf.GetBuffer()); // Always print on failure.
	}

	return result == GL_TRUE;
}

/**
 * Create all needed shader programs.
 * @return True if successful, false otherwise.
 */
bool OpenGLBackend::InitShaders()
{
	auto ver = GlGetString(GL_SHADING_LANGUAGE_VERSION);
	if (!ver.has_value()) return false;

	auto [glsl_major, glsl_minor] = DecodeVersion(*ver);

	bool glsl_150 = (IsOpenGLVersionAtLeast(3, 2) || glsl_major > 1 || (glsl_major == 1 && glsl_minor >= 5)) && _glBindFragDataLocation != nullptr;

	/* Create vertex shader. */
	GLuint vert_shader = _glCreateShader(GL_VERTEX_SHADER);
	_glShaderSource(vert_shader, glsl_150 ? lengthof(_vertex_shader_sprite_150) : lengthof(_vertex_shader_sprite), glsl_150 ? _vertex_shader_sprite_150 : _vertex_shader_sprite, nullptr);
	_glCompileShader(vert_shader);
	if (!VerifyShader(vert_shader)) return false;

	/* Create fragment shader for plain RGBA. */
	GLuint frag_shader_rgb = _glCreateShader(GL_FRAGMENT_SHADER);
	_glShaderSource(frag_shader_rgb, glsl_150 ? lengthof(_frag_shader_direct_150) : lengthof(_frag_shader_direct), glsl_150 ? _frag_shader_direct_150 : _frag_shader_direct, nullptr);
	_glCompileShader(frag_shader_rgb);
	if (!VerifyShader(frag_shader_rgb)) return false;

	/* Create fragment shader for paletted only. */
	GLuint frag_shader_pal = _glCreateShader(GL_FRAGMENT_SHADER);
	_glShaderSource(frag_shader_pal, glsl_150 ? lengthof(_frag_shader_palette_150) : lengthof(_frag_shader_palette), glsl_150 ? _frag_shader_palette_150 : _frag_shader_palette, nullptr);
	_glCompileShader(frag_shader_pal);
	if (!VerifyShader(frag_shader_pal)) return false;

	/* Sprite remap fragment shader. */
	GLuint remap_shader = _glCreateShader(GL_FRAGMENT_SHADER);
	_glShaderSource(remap_shader, glsl_150 ? lengthof(_frag_shader_rgb_mask_blend_150) : lengthof(_frag_shader_rgb_mask_blend), glsl_150 ? _frag_shader_rgb_mask_blend_150 : _frag_shader_rgb_mask_blend, nullptr);
	_glCompileShader(remap_shader);
	if (!VerifyShader(remap_shader)) return false;

	/* Sprite fragment shader. */
	GLuint sprite_shader = _glCreateShader(GL_FRAGMENT_SHADER);
	_glShaderSource(sprite_shader, glsl_150 ? lengthof(_frag_shader_sprite_blend_150) : lengthof(_frag_shader_sprite_blend), glsl_150 ? _frag_shader_sprite_blend_150 : _frag_shader_sprite_blend, nullptr);
	_glCompileShader(sprite_shader);
	if (!VerifyShader(sprite_shader)) return false;

	/* Link shaders to program. */
	this->vid_program = _glCreateProgram();
	_glAttachShader(this->vid_program, vert_shader);
	_glAttachShader(this->vid_program, frag_shader_rgb);

	this->pal_program = _glCreateProgram();
	_glAttachShader(this->pal_program, vert_shader);
	_glAttachShader(this->pal_program, frag_shader_pal);

	this->remap_program = _glCreateProgram();
	_glAttachShader(this->remap_program, vert_shader);
	_glAttachShader(this->remap_program, remap_shader);

	this->sprite_program = _glCreateProgram();
	_glAttachShader(this->sprite_program, vert_shader);
	_glAttachShader(this->sprite_program, sprite_shader);

	if (glsl_150) {
		/* Bind fragment shader outputs. */
		_glBindFragDataLocation(this->vid_program, 0, "colour");
		_glBindFragDataLocation(this->pal_program, 0, "colour");
		_glBindFragDataLocation(this->remap_program, 0, "colour");
		_glBindFragDataLocation(this->sprite_program, 0, "colour");
	}

	_glLinkProgram(this->vid_program);
	if (!VerifyProgram(this->vid_program)) return false;

	_glLinkProgram(this->pal_program);
	if (!VerifyProgram(this->pal_program)) return false;

	_glLinkProgram(this->remap_program);
	if (!VerifyProgram(this->remap_program)) return false;

	_glLinkProgram(this->sprite_program);
	if (!VerifyProgram(this->sprite_program)) return false;

	/* The pictures of the graphics card shown straight from it (three_d_gpu.h): without them, they are read back. */
	GLuint three_d_shader = _glCreateShader(GL_FRAGMENT_SHADER);
	_glShaderSource(three_d_shader, glsl_150 ? lengthof(_frag_shader_three_d_150) : lengthof(_frag_shader_three_d), glsl_150 ? _frag_shader_three_d_150 : _frag_shader_three_d, nullptr);
	_glCompileShader(three_d_shader);
	if (VerifyShader(three_d_shader)) {
		this->three_d_program = _glCreateProgram();
		_glAttachShader(this->three_d_program, vert_shader);
		_glAttachShader(this->three_d_program, three_d_shader);
		if (glsl_150) _glBindFragDataLocation(this->three_d_program, 0, "colour");
		_glLinkProgram(this->three_d_program);
		if (!VerifyProgram(this->three_d_program)) {
			_glDeleteProgram(this->three_d_program);
			this->three_d_program = 0;
		}
	}
	_glDeleteShader(three_d_shader);

	_glDeleteShader(vert_shader);
	_glDeleteShader(frag_shader_rgb);
	_glDeleteShader(frag_shader_pal);
	_glDeleteShader(remap_shader);
	_glDeleteShader(sprite_shader);

	return true;
}

/**
 * Clear the bound pixel buffer to a specific value.
 * @param len Length of the buffer.
 * @param data Value to set.
 * @tparam T Pixel type.
 */
template <class T>
static void ClearPixelBuffer(size_t len, T data)
{
	T *buf = reinterpret_cast<T *>(_glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_READ_WRITE));
	for (size_t i = 0; i < len; i++) {
		*buf++ = data;
	}
	_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
}

/**
 * Change the size of the drawing window and allocate matching resources.
 * @param w New width of the window.
 * @param h New height of the window.
 * @param force Recreate resources even if size didn't change.
 * @return \c false if nothing had to be done, \c true otherwise.
 */
bool OpenGLBackend::Resize(int w, int h, bool force)
{
	if (!force && _screen.width == w && _screen.height == h) return false;

	int bpp = BlitterFactory::GetCurrentBlitter()->GetScreenDepth();
	int pitch = Align(w, 4);
	size_t line_pixel_count = static_cast<size_t>(pitch) * h;

	_glViewport(0, 0, w, h);

	_glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch);

	this->vid_buffer = nullptr;
	if (this->persistent_mapping_supported) {
		_glDeleteBuffers(1, &this->vid_pbo);
		_glGenBuffers(1, &this->vid_pbo);
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vid_pbo);
		_glBufferStorage(GL_PIXEL_UNPACK_BUFFER, line_pixel_count * bpp / 8, nullptr, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
	} else {
		/* Re-allocate video buffer texture and backing store. */
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vid_pbo);
		_glBufferData(GL_PIXEL_UNPACK_BUFFER, line_pixel_count * bpp / 8, nullptr, GL_DYNAMIC_DRAW);
	}

	this->vid_pixels.assign(line_pixel_count * bpp / 8, 0);
	if (bpp == 32) {
		/* Initialize backing store alpha to opaque for 32bpp modes. */
		Colour black(0, 0, 0);
		std::fill_n(reinterpret_cast<uint32_t *>(this->vid_pixels.data()), line_pixel_count, black.data);
		if (_glClearBufferSubData != nullptr) {
			_glClearBufferSubData(GL_PIXEL_UNPACK_BUFFER, GL_RGBA8, 0, line_pixel_count * bpp / 8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &black.data);
		} else {
			ClearPixelBuffer<uint32_t>(line_pixel_count, black.data);
		}
	} else if (bpp == 8) {
		if (_glClearBufferSubData != nullptr) {
			uint8_t b = 0;
			_glClearBufferSubData(GL_PIXEL_UNPACK_BUFFER, GL_R8, 0, line_pixel_count, GL_RED, GL_UNSIGNED_BYTE, &b);
		} else {
			ClearPixelBuffer<uint8_t>(line_pixel_count, 0);
		}
	}

	_glActiveTexture(GL_TEXTURE0);
	_glBindTexture(GL_TEXTURE_2D, this->vid_texture);
	if (bpp == 8) {
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
	} else {
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
	}
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	/* Does this blitter need a separate animation buffer? */
	if (BlitterFactory::GetCurrentBlitter()->NeedsAnimationBuffer()) {
		this->anim_buffer = nullptr;
		if (this->persistent_mapping_supported) {
			_glDeleteBuffers(1, &this->anim_pbo);
			_glGenBuffers(1, &this->anim_pbo);
			_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
			_glBufferStorage(GL_PIXEL_UNPACK_BUFFER, line_pixel_count, nullptr, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
		} else {
			_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
			_glBufferData(GL_PIXEL_UNPACK_BUFFER, line_pixel_count, nullptr, GL_DYNAMIC_DRAW);
		}

		/* Initialize buffer as 0 == no remap. */
		this->anim_pixels.assign(line_pixel_count, 0);
		if (_glClearBufferSubData != nullptr) {
			uint8_t b = 0;
			_glClearBufferSubData(GL_PIXEL_UNPACK_BUFFER, GL_R8, 0, line_pixel_count, GL_RED, GL_UNSIGNED_BYTE, &b);
		} else {
			ClearPixelBuffer<uint8_t>(line_pixel_count, 0);
		}

		_glBindTexture(GL_TEXTURE_2D, this->anim_texture);
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	} else {
		this->anim_pixels.clear();
		if (this->anim_buffer != nullptr) {
			_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
			_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
			_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			this->anim_buffer = nullptr;
		}

		/* Allocate dummy texture that always reads as 0 == no remap. */
		uint dummy = 0;
		_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		_glBindTexture(GL_TEXTURE_2D, this->anim_texture);
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &dummy);
	}

	_glBindTexture(GL_TEXTURE_2D, 0);

	/* Set new viewport. */
	_screen.height = h;
	_screen.width = w;
	_screen.pitch = pitch;
	_screen.dst_ptr = nullptr;

	/* Update screen size in remap shader program. */
	_glUseProgram(this->remap_program);
	_glUniform2f(this->remap_screen_loc, (float)_screen.width, (float)_screen.height);

	_glClear(GL_COLOR_BUFFER_BIT);

	return true;
}

/**
 * Update the stored palette.
 * @param pal Palette array with at least 256 elements.
 * @param first First entry to update.
 * @param length Number of entries to update.
 */
void OpenGLBackend::UpdatePalette(const Colour *pal, uint first, uint length)
{
	assert(first + length <= 256);

	_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	_glActiveTexture(GL_TEXTURE1);
	_glBindTexture(GL_TEXTURE_1D, this->pal_texture);
	_glTexSubImage1D(GL_TEXTURE_1D, 0, first, length, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pal + first);
}

/**
 * Render video buffer to the screen.
 */
void OpenGLBackend::Paint()
{
	_glClear(GL_COLOR_BUFFER_BIT);

	_glDisable(GL_BLEND);

	/* Blit video buffer to screen. */
	_glActiveTexture(GL_TEXTURE0);
	_glBindTexture(GL_TEXTURE_2D, this->vid_texture);
	_glActiveTexture(GL_TEXTURE1);
	_glBindTexture(GL_TEXTURE_1D, this->pal_texture);
	/* Is the blitter relying on a separate animation buffer? */
	if (BlitterFactory::GetCurrentBlitter()->NeedsAnimationBuffer()) {
		_glActiveTexture(GL_TEXTURE2);
		_glBindTexture(GL_TEXTURE_2D, this->anim_texture);
		_glUseProgram(this->remap_program);
		_glUniform4f(this->remap_sprite_loc, 0.0f, 0.0f, 1.0f, 1.0f);
		_glUniform2f(this->remap_screen_loc, 1.0f, 1.0f);
		_glUniform1f(this->remap_zoom_loc, 0);
		_glUniform1i(this->remap_rgb_loc, 1);
	} else {
		_glUseProgram(BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 8 ? this->pal_program : this->vid_program);
	}
	_glBindVertexArray(this->vao_quad);
	_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* The pictures of the graphics card shown straight from it where their windows' are: in the holes the windows left
	 * for them (the screen's pixels of alpha below 255), under what the windows drew over them; those under the others
	 * first. */
	{
		std::lock_guard lock(this->three_d_mutex);
		if (!this->three_d_pictures.empty() && this->three_d_program != 0) {
			_glUseProgram(this->three_d_program);
			_glUniform2f(this->three_d_screen_loc, static_cast<float>(_screen.width), static_cast<float>(_screen.height));
			_glUniform2f(this->three_d_size_loc, static_cast<float>(_screen.width), static_cast<float>(_screen.height));
			_glActiveTexture(GL_TEXTURE1);
			_glBindTexture(GL_TEXTURE_2D, this->vid_texture);
			_glActiveTexture(GL_TEXTURE0);
			for (const bool under : {true, false}) for (const auto &[owner, pic] : this->three_d_pictures) {
				if (pic.under != under) continue;
				const Colour tint(pic.tint);
				_glUniform4f(this->three_d_sprite_loc, static_cast<float>(pic.r.left), static_cast<float>(pic.r.top), static_cast<float>(pic.r.right - pic.r.left + 1), static_cast<float>(pic.r.bottom - pic.r.top + 1));
				_glUniform4f(this->three_d_tint_loc, tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f, pic.tint == 0 ? 0.0f : tint.a / 255.0f);
				const Colour light(pic.light);
				_glUniform4f(this->three_d_light_loc, pic.light == 0 ? 1.0f : light.r / 256.0f, pic.light == 0 ? 1.0f : light.g / 256.0f, pic.light == 0 ? 1.0f : light.b / 256.0f, 1.0f);
				/* What stands on the turned map over its ground (ThreeDGpuViewStanding()). */
				_glUniform1f(this->three_d_over_loc, pic.over != 0 ? 1.0f : 0.0f);
				_glActiveTexture(GL_TEXTURE2);
				_glBindTexture(GL_TEXTURE_2D, pic.over);
				_glActiveTexture(GL_TEXTURE3);
				_glBindTexture(GL_TEXTURE_2D, pic.over_lit);
				_glActiveTexture(GL_TEXTURE0);
				_glBindTexture(GL_TEXTURE_2D, pic.texture);
				_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			for (GLenum unit : {GL_TEXTURE2, GL_TEXTURE3}) {
				_glActiveTexture(unit);
				_glBindTexture(GL_TEXTURE_2D, 0);
			}
			_glActiveTexture(GL_TEXTURE1);
			_glBindTexture(GL_TEXTURE_2D, 0);
			_glActiveTexture(GL_TEXTURE0);
			_glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	/* The rig: what is on the screen as it is shown (test3d obrazovka), a PPM where the game runs; after so many paints,
	 * and the game ended after it when asked. */
	extern std::string _rig_screen_picture;
	extern int _rig_screen_paints;
	extern bool _rig_screen_quit;
	if (!_rig_screen_picture.empty() && _glReadPixels != nullptr && --_rig_screen_paints <= 0) {
		std::vector<uint8_t> rgb(static_cast<size_t>(_screen.width) * _screen.height * 3);
		_glPixelStorei(GL_PACK_ALIGNMENT, 1);
		_glReadPixels(0, 0, _screen.width, _screen.height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
		if (auto f = FileHandle::Open(_rig_screen_picture + ".ppm", "wb"); f.has_value()) {
			std::string head = fmt::format("P6\n{} {}\n255\n", _screen.width, _screen.height);
			fwrite(head.data(), 1, head.size(), *f);
			for (int y = _screen.height - 1; y >= 0; y--) fwrite(rgb.data() + static_cast<size_t>(y) * _screen.width * 3, 1, static_cast<size_t>(_screen.width) * 3, *f);
		}
		Debug(driver, 0, "OpenGL: the screen as shown into {}.ppm, {} pictures straight from the graphics card", _rig_screen_picture, this->three_d_pictures.size());
		_rig_screen_picture.clear();
		if (_rig_screen_quit) _exit_game = true;
	}

	_glEnable(GL_BLEND);
}

/**
 * Show a picture of the graphics card straight from it where a window's is (VideoDriver::ShowThreeDPicture()).
 * @param owner whose it is
 * @param r where on the screen
 * @param texture the card's, shared with this context
 * @param tint laid over it by its alpha (Colour::data), 0 none
 * @param under shown under the others
 * @param light its colours multiplied by these, 256ths (Colour::data), 0 as they are
 * @param over the card's picture laid over it (what stands on the turned map), its alpha multiplied in, 0 none
 * @param over_lit which of over's pixels the light leaves as they are (its red), 0 none
 */
void OpenGLBackend::ShowThreeDPicture(const void *owner, const Rect &r, uint32_t texture, uint32_t tint, bool under, uint32_t light, uint32_t over, uint32_t over_lit)
{
	std::lock_guard lock(this->three_d_mutex);
	this->three_d_pictures[owner] = {r, texture, tint, under, light, over, over_lit};
}

/** A window's picture of the graphics card shown no more. */
void OpenGLBackend::HideThreeDPicture(const void *owner)
{
	std::lock_guard lock(this->three_d_mutex);
	this->three_d_pictures.erase(owner);
}

/** The pictures of the graphics card shown now: where on the screen, and their textures. */
std::vector<std::pair<Rect, uint32_t>> OpenGLBackend::GetThreeDPictures()
{
	std::lock_guard lock(this->three_d_mutex);
	std::vector<std::pair<Rect, uint32_t>> shown;
	for (const auto &[owner, pic] : this->three_d_pictures) shown.emplace_back(pic.r, pic.texture);
	return shown;
}

/**
 * Draw mouse cursor on screen.
 */
void OpenGLBackend::DrawMouseCursor()
{
	if (!this->cursor_in_window) return;

	/* Draw cursor on screen */
	_cur_dpi = &_screen;
	for (const auto &cs : this->cursor_sprites) {
		/* Sprites are cached by PopulateCursorCache(). */
		if (this->cursor_cache.Contains(cs.image.sprite)) {
			const OpenGLSprite *spr = this->cursor_cache.Get(cs.image.sprite).get();

			this->RenderOglSprite(spr, cs.image.pal,
					this->cursor_pos.x + cs.pos.x + UnScaleByZoom(spr->x_offs, _gui_zoom),
					this->cursor_pos.y + cs.pos.y + UnScaleByZoom(spr->y_offs, _gui_zoom),
					_gui_zoom);
		}
	}
}

class OpenGLSpriteAllocator : public SpriteAllocator {
public:
	OpenGLSpriteLRUCache &lru;
	SpriteID sprite;

	OpenGLSpriteAllocator(OpenGLSpriteLRUCache &lru, SpriteID sprite) : lru(lru), sprite(sprite) {}
protected:
	void *AllocatePtr(size_t) override { NOT_REACHED(); }
};

void OpenGLBackend::PopulateCursorCache()
{
	if (this->clear_cursor_cache) {
		/* We have a pending cursor cache clear to do first. */
		this->clear_cursor_cache = false;
		this->last_sprite_pal = (PaletteID)-1;

		this->InternalClearCursorCache();
	}

	this->cursor_pos = _cursor.pos;
	this->cursor_in_window = _cursor.in_window;

	this->cursor_sprites.clear();
	for (const auto &sc : _cursor.sprites) {
		this->cursor_sprites.emplace_back(sc);

		if (!this->cursor_cache.Contains(sc.image.sprite)) {
			OpenGLSpriteAllocator allocator(this->cursor_cache, sc.image.sprite);
			GetRawSprite(sc.image.sprite, SpriteType::Normal, &allocator, this);
		}
	}
}

/**
 * Clear all cached cursor sprites.
 */
void OpenGLBackend::InternalClearCursorCache()
{
	this->cursor_cache.Clear();
}

/**
 * Queue a request for cursor cache clear.
 */
void OpenGLBackend::ClearCursorCache()
{
	/* If the game loop is threaded, this function might be called
	 * from the game thread. As we can call OpenGL functions only
	 * on the main thread, just set a flag that is handled the next
	 * time we prepare the cursor cache for drawing. */
	this->clear_cursor_cache = true;
}

/**
 * Get a pointer to the memory for the video driver to draw to: the game's own (vid_pixels), what changed in it copied
 * to the card's buffer when it is released (ReleaseVideoBuffer()). The blitters read what is under what they draw (the
 * night darkening the map, the colours of the palette's animation realized under a picture), and reading the card's
 * memory can be very slow: the player's zink on a Mali in Winlator (#300, 25. 10.) took ~200 ms a frame to lay the
 * picture from the eyes into its window and the night slowed the title's map down, the picture itself drawn by the
 * card in ~40 ms.
 * @return Pointer to draw on.
 */
void *OpenGLBackend::GetVideoBuffer()
{
	return this->vid_pixels.data();
}

/**
 * Get a pointer to the memory for the separate animation buffer: the game's own too (anim_pixels).
 * @return Pointer to draw on.
 */
uint8_t *OpenGLBackend::GetAnimBuffer()
{
	if (this->anim_pbo == 0 || this->anim_pixels.empty()) return nullptr;
	return this->anim_pixels.data();
}

/**
 * The card's buffer of the video buffer, mapped for the changes to be copied to it.
 * @return Its memory.
 */
void *OpenGLBackend::MapVideoBuffer()
{
#ifndef NO_GL_BUFFER_SYNC
	if (this->sync_vid_mapping != nullptr) _glClientWaitSync(this->sync_vid_mapping, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000); // 100ms timeout.
#endif

	if (!this->persistent_mapping_supported) {
		assert(this->vid_buffer == nullptr);
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vid_pbo);
		this->vid_buffer = _glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	} else if (this->vid_buffer == nullptr) {
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vid_pbo);
		this->vid_buffer = _glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(_screen.pitch) * _screen.height * BlitterFactory::GetCurrentBlitter()->GetScreenDepth() / 8, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
	}

	return this->vid_buffer;
}

/**
 * The card's buffer of the animation buffer, mapped for the changes to be copied to it.
 * @return Its memory.
 */
uint8_t *OpenGLBackend::MapAnimBuffer()
{
#ifndef NO_GL_BUFFER_SYNC
	if (this->sync_anim_mapping != nullptr) _glClientWaitSync(this->sync_anim_mapping, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000); // 100ms timeout.
#endif

	if (!this->persistent_mapping_supported) {
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
		this->anim_buffer = _glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	} else if (this->anim_buffer == nullptr) {
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
		this->anim_buffer = _glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(_screen.pitch) * _screen.height, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
	}

	return (uint8_t *)this->anim_buffer;
}

/**
 * The rows of a rectangle of the screen copied from the game's memory to the card's: only written there, never read.
 * @param to the card's buffer, as large as the screen
 * @param from the game's
 * @param r the rectangle, right and bottom outside it
 * @param bytes of a pixel
 */
static void CopyScreenRect(void *to, const uint8_t *from, const Rect &r, size_t bytes)
{
	if (to == nullptr) return;
	const int left = std::max(r.left, 0), right = std::min(r.right, _screen.pitch);
	const int top = std::max(r.top, 0), bottom = std::min(r.bottom, _screen.height);
	if (left >= right) return;
	for (int y = top; y < bottom; y++) {
		const size_t at = (static_cast<size_t>(y) * _screen.pitch + left) * bytes;
		std::copy_n(from + at, (right - left) * bytes, static_cast<uint8_t *>(to) + at);
	}
}

/**
 * Update video buffer texture after the video buffer was filled.
 * @param update_rect Rectangle encompassing the dirty region of the video buffer.
 */
void OpenGLBackend::ReleaseVideoBuffer(const Rect &update_rect)
{
	assert(this->vid_pbo != 0);
	if (IsEmptyRect(update_rect)) return;

	CopyScreenRect(this->MapVideoBuffer(), this->vid_pixels.data(), update_rect, BlitterFactory::GetCurrentBlitter()->GetScreenDepth() / 8);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vid_pbo);
	if (!this->persistent_mapping_supported) {
		_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		this->vid_buffer = nullptr;
	}

#ifndef NO_GL_BUFFER_SYNC
	if (this->persistent_mapping_supported) {
		_glDeleteSync(this->sync_vid_mapping);
		this->sync_vid_mapping = nullptr;
	}
#endif

	/* Update changed rect of the video buffer texture. */
	if (!IsEmptyRect(update_rect)) {
		_glActiveTexture(GL_TEXTURE0);
		_glBindTexture(GL_TEXTURE_2D, this->vid_texture);
		_glPixelStorei(GL_UNPACK_ROW_LENGTH, _screen.pitch);
		if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 8) {
			_glTexSubImage2D(GL_TEXTURE_2D, 0, update_rect.left, update_rect.top, update_rect.right - update_rect.left, update_rect.bottom - update_rect.top, GL_RED, GL_UNSIGNED_BYTE, (GLvoid*)(size_t)(update_rect.top * _screen.pitch + update_rect.left));
		} else {
			_glTexSubImage2D(GL_TEXTURE_2D, 0, update_rect.left, update_rect.top, update_rect.right - update_rect.left, update_rect.bottom - update_rect.top, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (GLvoid*)(size_t)(update_rect.top * _screen.pitch * 4 + update_rect.left * 4));
		}

#ifndef NO_GL_BUFFER_SYNC
		if (this->persistent_mapping_supported) this->sync_vid_mapping = _glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
	}
}

/**
 * Update animation buffer texture after the animation buffer was filled.
 * @param update_rect Rectangle encompassing the dirty region of the animation buffer.
 */
void OpenGLBackend::ReleaseAnimBuffer(const Rect &update_rect)
{
	if (this->anim_pbo == 0 || this->anim_pixels.empty() || update_rect.left == update_rect.right) return;

	CopyScreenRect(this->MapAnimBuffer(), this->anim_pixels.data(), update_rect, 1);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->anim_pbo);
	if (!this->persistent_mapping_supported) {
		_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		this->anim_buffer = nullptr;
	}

#ifndef NO_GL_BUFFER_SYNC
	if (this->persistent_mapping_supported) {
		_glDeleteSync(this->sync_anim_mapping);
		this->sync_anim_mapping = nullptr;
	}
#endif

	/* Update changed rect of the video buffer texture. */
	if (update_rect.left != update_rect.right) {
		_glActiveTexture(GL_TEXTURE0);
		_glBindTexture(GL_TEXTURE_2D, this->anim_texture);
		_glPixelStorei(GL_UNPACK_ROW_LENGTH, _screen.pitch);
		_glTexSubImage2D(GL_TEXTURE_2D, 0, update_rect.left, update_rect.top, update_rect.right - update_rect.left, update_rect.bottom - update_rect.top, GL_RED, GL_UNSIGNED_BYTE, (GLvoid *)(size_t)(update_rect.top * _screen.pitch + update_rect.left));

#ifndef NO_GL_BUFFER_SYNC
		if (this->persistent_mapping_supported) this->sync_anim_mapping = _glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
	}
}

/* virtual */ Sprite *OpenGLBackend::Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator)
{
	/* This encoding is only called for mouse cursors. We don't need real sprites but OpenGLSprites to show as cursor. These need to be put in the LRU cache. */
	OpenGLSpriteAllocator &gl_allocator = static_cast<OpenGLSpriteAllocator&>(allocator);
	gl_allocator.lru.Insert(gl_allocator.sprite, std::make_unique<OpenGLSprite>(sprite_type, sprite));

	return nullptr;
}

/**
 * Render a sprite to the back buffer.
 * @param gl_sprite Sprite to render.
 * @param pal The palette to draw the sprite with.
 * @param x X position of the sprite.
 * @param y Y position of the sprite.
 * @param zoom Zoom level to use.
 */
void OpenGLBackend::RenderOglSprite(const OpenGLSprite *gl_sprite, PaletteID pal, int x, int y, ZoomLevel zoom)
{
	/* Set textures. */
	bool rgb = gl_sprite->BindTextures();
	_glActiveTexture(GL_TEXTURE0 + 1);
	_glBindTexture(GL_TEXTURE_1D, this->pal_texture);

	/* Set palette remap. */
	_glActiveTexture(GL_TEXTURE0 + 3);
	if (pal != PAL_NONE) {
		_glBindTexture(GL_TEXTURE_1D, OpenGLSprite::pal_tex);
		if (pal != this->last_sprite_pal) {
			/* Different remap palette in use, update texture. */
			_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, OpenGLSprite::pal_pbo);
			_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

			_glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, 256, GetNonSprite(GB(pal, 0, PALETTE_WIDTH), SpriteType::Recolour) + 1);
			_glTexSubImage1D(GL_TEXTURE_1D, 0, 0, 256, GL_RED, GL_UNSIGNED_BYTE, nullptr);

			_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

			this->last_sprite_pal = pal;
		}
	} else {
		_glBindTexture(GL_TEXTURE_1D, OpenGLSprite::pal_identity);
	}

	/* Set up shader program. */
	Dimension dim = gl_sprite->GetSize(zoom);
	_glUseProgram(this->sprite_program);
	_glUniform4f(this->sprite_sprite_loc, (float)x, (float)y, (float)dim.width, (float)dim.height);
	_glUniform1f(this->sprite_zoom_loc, (float)zoom);
	_glUniform2f(this->sprite_screen_loc, (float)_screen.width, (float)_screen.height);
	_glUniform1i(this->sprite_rgb_loc, rgb ? 1 : 0);
	_glUniform1i(this->sprite_crash_loc, pal == PALETTE_CRASH ? 1 : 0);

	_glBindVertexArray(this->vao_quad);
	_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


/* static */ EnumIndexArray<GLuint, OpenGLSprite::Texture, OpenGLSprite::Texture::End> OpenGLSprite::dummy_tex{};
/* static */ GLuint OpenGLSprite::pal_identity = 0;
/* static */ GLuint OpenGLSprite::pal_tex = 0;
/* static */ GLuint OpenGLSprite::pal_pbo = 0;

/**
 * Create all common resources for sprite rendering.
 * @return True if no error occurred.
 */
/* static */ bool OpenGLSprite::Create()
{
	_glGenTextures(static_cast<GLsizei>(OpenGLSprite::dummy_tex.size()), OpenGLSprite::dummy_tex.data());

	for (Texture t : EnumRange<Texture>(Texture::End)) {
		_glBindTexture(GL_TEXTURE_2D, OpenGLSprite::dummy_tex[t]);

		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	/* Load dummy RGBA texture. */
	const Colour rgb_pixel(0, 0, 0);
	_glBindTexture(GL_TEXTURE_2D, OpenGLSprite::dummy_tex[Texture::RGBA]);
	_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &rgb_pixel);

	/* Load dummy remap texture. */
	const uint pal = 0;
	_glBindTexture(GL_TEXTURE_2D, OpenGLSprite::dummy_tex[Texture::Remap]);
	_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &pal);

	/* Create palette remap textures. */
	std::array<uint8_t, 256> identity_pal;
	std::iota(std::begin(identity_pal), std::end(identity_pal), 0);

	/* Permanent texture for identity remap. */
	_glGenTextures(1, &OpenGLSprite::pal_identity);
	_glBindTexture(GL_TEXTURE_1D, OpenGLSprite::pal_identity);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAX_LEVEL, 0);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glTexImage1D(GL_TEXTURE_1D, 0, GL_R8, 256, 0, GL_RED, GL_UNSIGNED_BYTE, identity_pal.data());

	/* Dynamically updated texture for remaps. */
	_glGenTextures(1, &OpenGLSprite::pal_tex);
	_glBindTexture(GL_TEXTURE_1D, OpenGLSprite::pal_tex);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAX_LEVEL, 0);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glTexImage1D(GL_TEXTURE_1D, 0, GL_R8, 256, 0, GL_RED, GL_UNSIGNED_BYTE, identity_pal.data());

	/* Pixel buffer for remap updates. */
	_glGenBuffers(1, &OpenGLSprite::pal_pbo);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, OpenGLSprite::pal_pbo);
	_glBufferData(GL_PIXEL_UNPACK_BUFFER, 256, identity_pal.data(), GL_DYNAMIC_DRAW);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	return _glGetError() == GL_NO_ERROR;
}

/** Free all common resources for sprite rendering. */
/* static */ void OpenGLSprite::Destroy()
{
	_glDeleteTextures(static_cast<GLsizei>(OpenGLSprite::dummy_tex.size()), OpenGLSprite::dummy_tex.data());
	_glDeleteTextures(1, &OpenGLSprite::pal_identity);
	_glDeleteTextures(1, &OpenGLSprite::pal_tex);
	if (_glDeleteBuffers != nullptr) _glDeleteBuffers(1, &OpenGLSprite::pal_pbo);
}

/**
 * Create an OpenGL sprite with a palette remap part.
 * @param sprite_type The type of sprite to load.
 * @param sprite The sprite to create the OpenGL sprite for
 */
OpenGLSprite::OpenGLSprite(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite)
{
	const auto &root_sprite = sprite.Root();
	this->dim.width = root_sprite.width;
	this->dim.height = root_sprite.height;
	this->x_offs = root_sprite.x_offs;
	this->y_offs = root_sprite.y_offs;

	int levels = sprite_type == SpriteType::Font ? 1 : to_underlying(ZoomLevel::End);
	assert(levels > 0);
	(void)_glGetError();

	this->tex = {};
	_glActiveTexture(GL_TEXTURE0);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	for (Texture t : EnumRange<Texture>(Texture::End)) {
		/* Sprite component present? */
		if (t == Texture::RGBA && root_sprite.colours == SpriteComponent::Palette) continue;
		if (t == Texture::Remap && !root_sprite.colours.Test(SpriteComponent::Palette)) continue;

		/* Allocate texture. */
		_glGenTextures(1, &this->tex[t]);
		_glBindTexture(GL_TEXTURE_2D, this->tex[t]);

		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		/* Set size. */
		for (int i = 0, w = this->dim.width, h = this->dim.height; i < levels; i++, w /= 2, h /= 2) {
			assert(w * h != 0);
			if (t == Texture::Remap) {
				_glTexImage2D(GL_TEXTURE_2D, i, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
			} else {
				_glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
			}
		}
	}

	/* Upload texture data. */
	for (ZoomLevel zoom = ZoomLevel::Min; zoom <= (sprite_type == SpriteType::Font ? ZoomLevel::Min : ZoomLevel::Max); ++zoom) {
		const auto &src_sprite = sprite[zoom];
		/* A level with its size alone and no pixels (the 16x of ResizeSprites() when 16x is off) is left undefined. */
		if (src_sprite.data == nullptr) continue;
		this->Update(src_sprite.width, src_sprite.height, to_underlying(zoom), src_sprite.data);
	}

	assert(_glGetError() == GL_NO_ERROR);
}

/** Delete the textures we allocated. */
OpenGLSprite::~OpenGLSprite()
{
	_glDeleteTextures(static_cast<GLsizei>(this->tex.size()), this->tex.data());
}

/**
 * Update a single mip-map level with new pixel data.
 * @param width Width of the level.
 * @param height Height of the level.
 * @param level Mip-map level.
 * @param data New pixel data.
 */
void OpenGLSprite::Update(uint width, uint height, uint level, const SpriteLoader::CommonPixel * data)
{
	static ReusableBuffer<Colour> buf_rgba;
	static ReusableBuffer<uint8_t> buf_pal;

	_glActiveTexture(GL_TEXTURE0);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	if (this->tex[Texture::RGBA] != 0) {
		/* Unpack pixel data */
		size_t size = static_cast<size_t>(width) * height;
		Colour *rgba = buf_rgba.Allocate(size);
		for (size_t i = 0; i < size; i++) {
			rgba[i].r = data[i].r;
			rgba[i].g = data[i].g;
			rgba[i].b = data[i].b;
			rgba[i].a = data[i].a;
		}

		_glBindTexture(GL_TEXTURE_2D, this->tex[Texture::RGBA]);
		_glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, rgba);
	}

	if (this->tex[Texture::Remap] != 0) {
		/* Unpack and align pixel data. */
		size_t pitch = Align(width, 4);

		uint8_t *pal = buf_pal.Allocate(pitch * height);
		const SpriteLoader::CommonPixel *row = data;
		for (uint y = 0; y < height; y++, pal += pitch, row += width) {
			for (uint x = 0; x < width; x++) {
				pal[x] = row[x].m;
			}
		}

		_glBindTexture(GL_TEXTURE_2D, this->tex[Texture::Remap]);
		_glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, buf_pal.GetBuffer());
	}

	assert(_glGetError() == GL_NO_ERROR);
}

/**
 * Query the sprite size at a certain zoom level.
 * @param level The zoom level to query.
 * @return Sprite size at the given zoom level.
 */
inline Dimension OpenGLSprite::GetSize(ZoomLevel level) const
{
	Dimension sd = { (uint)UnScaleByZoomLower(this->dim.width, level), (uint)UnScaleByZoomLower(this->dim.height, level) };
	return sd;
}

/**
 * Bind textures for rendering this sprite.
 * @return True if the sprite has RGBA data.
 */
bool OpenGLSprite::BindTextures() const
{
	_glActiveTexture(GL_TEXTURE0);
	_glBindTexture(GL_TEXTURE_2D, this->tex[Texture::RGBA] != 0 ? this->tex[Texture::RGBA] : OpenGLSprite::dummy_tex[Texture::RGBA]);
	_glActiveTexture(GL_TEXTURE0 + 2);
	_glBindTexture(GL_TEXTURE_2D, this->tex[Texture::Remap] != 0 ? this->tex[Texture::Remap] : OpenGLSprite::dummy_tex[Texture::Remap]);

	return this->tex[Texture::RGBA] != 0;
}
