/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * Changed for ottd Decouple by Karel Mácha: the changes Copyright (C) 2026 Karel Mácha.
 */

/** @file opengl_shader.h OpenGL shader programs. */

/** Vertex shader that positions a sprite on screen.. */
static const char *_vertex_shader_sprite[] = {
	"#version 110\n",
	"uniform vec4 sprite;",
	"uniform vec2 screen;",
	"attribute vec2 position, colour_uv;",
	"varying vec2 colour_tex_uv;",
	"void main() {",
	"  vec2 size = sprite.zw / screen.xy;",
	"  vec2 offset = ((2.0 * sprite.xy + sprite.zw) / screen.xy - 1.0) * vec2(1.0, -1.0);",
	"  colour_tex_uv = colour_uv;",
	"  gl_Position = vec4(position * size + offset, 0.0, 1.0);",
	"}",
};

/** GLSL 1.50 vertex shader that positions a sprite on screen. */
static const char *_vertex_shader_sprite_150[] = {
	"#version 150\n",
	"uniform vec4 sprite;",
	"uniform vec2 screen;",
	"in vec2 position, colour_uv;",
	"out vec2 colour_tex_uv;",
	"void main() {",
	"  vec2 size = sprite.zw / screen.xy;",
	"  vec2 offset = ((2.0 * sprite.xy + sprite.zw) / screen.xy - 1.0) * vec2(1.0, -1.0);",
	"  colour_tex_uv = colour_uv;",
	"  gl_Position = vec4(position * size + offset, 0.0, 1.0);",
	"}",
};

/** Fragment shader that reads the fragment colour from a 32bpp texture. */
static const char *_frag_shader_direct[] = {
	"#version 110\n",
	"uniform sampler2D colour_tex;",
	"varying vec2 colour_tex_uv;",
	"void main() {",
	"  gl_FragData[0] = texture2D(colour_tex, colour_tex_uv);",
	"}",
};

/** GLSL 1.50 fragment shader that reads the fragment colour from a 32bpp texture. */
static const char *_frag_shader_direct_150[] = {
	"#version 150\n",
	"uniform sampler2D colour_tex;",
	"in vec2 colour_tex_uv;",
	"out vec4 colour;",
	"void main() {",
	"  colour = texture(colour_tex, colour_tex_uv);",
	"}",
};

/** Fragment shader that performs a palette lookup to read the colour from an 8bpp texture. */
static const char *_frag_shader_palette[] = {
	"#version 110\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler1D palette;",
	"varying vec2 colour_tex_uv;",
	"void main() {",
	"  float idx = texture2D(colour_tex, colour_tex_uv).r;",
	"  gl_FragData[0] = texture1D(palette, idx);",
	"}",
};

/** GLSL 1.50 fragment shader that performs a palette lookup to read the colour from an 8bpp texture. */
static const char *_frag_shader_palette_150[] = {
	"#version 150\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler1D palette;",
	"in vec2 colour_tex_uv;",
	"out vec4 colour;",
	"void main() {",
	"  float idx = texture(colour_tex, colour_tex_uv).r;",
	"  colour = texture(palette, idx);",
	"}",
};


/** Fragment shader function for remap brightness modulation. */
static const char *_frag_shader_remap_func = \
	"float max3(vec3 v) {"
	"  return max(max(v.x, v.y), v.z);"
	"}"
	""
	"vec3 adj_brightness(vec3 colour, float brightness) {"
	"  vec3 adj = colour * (brightness > 0.0 ? brightness / 0.5 : 1.0);"
	"  vec3 ob_vec = clamp(adj - 1.0, 0.0, 1.0);"
	"  float ob = (ob_vec.r + ob_vec.g + ob_vec.b) / 2.0;"
	""
	"  return clamp(adj + ob * (1.0 - adj), 0.0, 1.0);"
	"}";

/** Fragment shader that performs a palette lookup to read the colour from an 8bpp texture. */
static const char *_frag_shader_rgb_mask_blend[] = {
	"#version 110\n",
	"#extension GL_ATI_shader_texture_lod: enable\n",
	"#extension GL_ARB_shader_texture_lod: enable\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler1D palette;",
	"uniform sampler2D remap_tex;",
	"uniform bool rgb;",
	"uniform float zoom;",
	"varying vec2 colour_tex_uv;",
	"",
	_frag_shader_remap_func,
	"",
	"void main() {",
	"  float idx = texture2DLod(remap_tex, colour_tex_uv, zoom).r;",
	"  vec4 remap_col = texture1D(palette, idx);",
	"  vec4 rgb_col = texture2DLod(colour_tex, colour_tex_uv, zoom);",
	"",
	"  gl_FragData[0].a = rgb ? rgb_col.a : remap_col.a;",
	"  gl_FragData[0].rgb = idx > 0.0 ? adj_brightness(remap_col.rgb, max3(rgb_col.rgb)) : rgb_col.rgb;",
	"}",
};

/** GLSL 1.50 fragment shader that performs a palette lookup to read the colour from an 8bpp texture. */
static const char *_frag_shader_rgb_mask_blend_150[] = {
	"#version 150\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler1D palette;",
	"uniform sampler2D remap_tex;",
	"uniform float zoom;",
	"uniform bool rgb;",
	"in vec2 colour_tex_uv;",
	"out vec4 colour;",
	"",
	_frag_shader_remap_func,
	"",
	"void main() {",
	"  float idx = textureLod(remap_tex, colour_tex_uv, zoom).r;",
	"  vec4 remap_col = texture(palette, idx);",
	"  vec4 rgb_col = textureLod(colour_tex, colour_tex_uv, zoom);",
	"",
	"  colour.a = rgb ? rgb_col.a : remap_col.a;",
	"  colour.rgb = idx > 0.0 ? adj_brightness(remap_col.rgb, max3(rgb_col.rgb)) : rgb_col.rgb;",
	"}",
};

/** Fragment shader that performs a palette lookup to read the colour from a sprite texture. */
static const char *_frag_shader_sprite_blend[] = {
	"#version 110\n",
	"#extension GL_ATI_shader_texture_lod: enable\n",
	"#extension GL_ARB_shader_texture_lod: enable\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler1D palette;",
	"uniform sampler2D remap_tex;",
	"uniform sampler1D pal;",
	"uniform float zoom;",
	"uniform bool rgb;",
	"uniform bool crash;",
	"varying vec2 colour_tex_uv;",
	"",
	_frag_shader_remap_func,
	"",
	"void main() {",
	"  float idx = texture2DLod(remap_tex, colour_tex_uv, zoom).r;",
	"  float r = texture1D(pal, idx).r;",
	"  vec4 remap_col = texture1D(palette, idx);",
	"  vec4 rgb_col = texture2DLod(colour_tex, colour_tex_uv, zoom);",
	"",
	"  if (crash && idx == 0.0) rgb_col.rgb = vec2(dot(rgb_col.rgb, vec3(0.199325561523, 0.391342163085, 0.076004028320)), 0.0).rrr;"
	"  gl_FragData[0].a = rgb && (r > 0.0 || idx == 0.0) ? rgb_col.a : remap_col.a;",
	"  gl_FragData[0].rgb = idx > 0.0 ? adj_brightness(remap_col.rgb, max3(rgb_col.rgb)) : rgb_col.rgb;",
	"}",
};

/** GLSL 1.50 fragment shader that performs a palette lookup to read the colour from a sprite texture. */
static const char *_frag_shader_sprite_blend_150[] = {
	"#version 150\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler1D palette;",
	"uniform sampler2D remap_tex;",
	"uniform sampler1D pal;",
	"uniform float zoom;",
	"uniform bool rgb;",
	"uniform bool crash;",
	"in vec2 colour_tex_uv;",
	"out vec4 colour;",
	"",
	_frag_shader_remap_func,
	"",
	"void main() {",
	"  float idx =  textureLod(remap_tex, colour_tex_uv, zoom).r;"
	"  float r = texture(pal, idx).r;",
	"  vec4 remap_col = texture(palette, r);",
	"  vec4 rgb_col = textureLod(colour_tex, colour_tex_uv, zoom);",
	"",
	"  if (crash && idx == 0.0) rgb_col.rgb = vec2(dot(rgb_col.rgb, vec3(0.199325561523, 0.391342163085, 0.076004028320)), 0.0).rrr;"
	"  colour.a = rgb && (r > 0.0 || idx == 0.0) ? rgb_col.a : remap_col.a;",
	"  colour.rgb = idx > 0.0 ? adj_brightness(remap_col.rgb, max3(rgb_col.rgb)) : rgb_col.rgb;",
	"}",
};

/**
 * Fragment shader that draws a picture of the graphics card where the screen's pixels are of alpha below 255
 * (OpenGLBackend::ShowThreeDPicture()): the screen's pixel laid over it, its colour with its alpha in it (what the
 * blitter drew over the hole: a shadow, an edge, a letter).
 */
static const char *_frag_shader_three_d[] = {
	"#version 110\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler2D screen_tex;",
	"uniform vec2 screen_size;",
	"uniform vec4 tint;",
	"uniform vec4 light;",
	"uniform sampler2D over_tex;",
	"uniform sampler2D over_lit_tex;",
	"uniform float has_over;",
	"uniform vec4 part;",
	"varying vec2 colour_tex_uv;",
	"void main() {",
	"  vec4 s = texture2D(screen_tex, vec2(gl_FragCoord.x / screen_size.x, 1.0 - gl_FragCoord.y / screen_size.y));",
	"  if (s.a > 0.998) discard;",
	"  vec2 uv = part.xy + colour_tex_uv * part.zw;",
	"  vec3 c = texture2D(colour_tex, uv).rgb * light.rgb;",
	"  if (has_over > 0.5) {",
	"    vec4 o = texture2D(over_tex, uv);",
	"    float l = texture2D(over_lit_tex, uv).r;",
	"    c = o.rgb * mix(light.rgb, vec3(1.0), l) + (1.0 - o.a) * c;",
	"  }",
	"  gl_FragData[0] = vec4(s.rgb + (1.0 - s.a) * mix(c, tint.rgb, tint.a), 1.0);",
	"}",
};

/** GLSL 1.50 fragment shader that draws a picture of the graphics card where the screen's pixels are of alpha below 255. */
static const char *_frag_shader_three_d_150[] = {
	"#version 150\n",
	"uniform sampler2D colour_tex;",
	"uniform sampler2D screen_tex;",
	"uniform vec2 screen_size;",
	"uniform vec4 tint;",
	"uniform vec4 light;",
	"uniform sampler2D over_tex;",
	"uniform sampler2D over_lit_tex;",
	"uniform float has_over;",
	"uniform vec4 part;",
	"in vec2 colour_tex_uv;",
	"out vec4 colour;",
	"void main() {",
	"  vec4 s = texture(screen_tex, vec2(gl_FragCoord.x / screen_size.x, 1.0 - gl_FragCoord.y / screen_size.y));",
	"  if (s.a > 0.998) discard;",
	"  vec2 uv = part.xy + colour_tex_uv * part.zw;",
	"  vec3 c = texture(colour_tex, uv).rgb * light.rgb;",
	"  if (has_over > 0.5) {",
	"    vec4 o = texture(over_tex, uv);",
	"    float l = texture(over_lit_tex, uv).r;",
	"    c = o.rgb * mix(light.rgb, vec3(1.0), l) + (1.0 - o.a) * c;",
	"  }",
	"  colour = vec4(s.rgb + (1.0 - s.a) * mix(c, tint.rgb, tint.a), 1.0);",
	"}",
};

/**
 * Fragment shader of a glow of the night over a picture of the graphics card (OpenGLBackend::SetThreeDGlows()): added
 * where the screen leaves the picture seen, a soft spot as the processor's (AddNightLight()), only when the picture over
 * it has a lit pixel near its middle (two pixels about it, as the processor's night looked).
 */
static const char *_frag_shader_three_d_glow[] = {
	"#version 110\n",
	"uniform sampler2D screen_tex;",
	"uniform sampler2D over_lit_tex;",
	"uniform vec2 screen_size;",
	"uniform vec4 glow;",
	"uniform vec4 lit_at;",
	"varying vec2 colour_tex_uv;",
	"void main() {",
	"  vec4 s = texture2D(screen_tex, vec2(gl_FragCoord.x / screen_size.x, 1.0 - gl_FragCoord.y / screen_size.y));",
	"  if (s.a > 0.998) discard;",
	"  vec2 d = colour_tex_uv * 2.0 - 1.0;",
	"  float d2 = dot(d, d);",
	"  if (d2 >= 1.0) discard;",
	"  float lit = 0.0;",
	"  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) lit = max(lit, texture2D(over_lit_tex, lit_at.xy + vec2(float(x), float(y)) * lit_at.zw).r);",
	"  if (lit < 0.002) discard;",
	"  float f = glow.a * (1.0 - d2) * (1.0 - d2);",
	"  gl_FragData[0] = vec4(glow.rgb * f, 1.0);",
	"}",
};

/** GLSL 1.50 fragment shader of a glow of the night over a picture of the graphics card. */
static const char *_frag_shader_three_d_glow_150[] = {
	"#version 150\n",
	"uniform sampler2D screen_tex;",
	"uniform sampler2D over_lit_tex;",
	"uniform vec2 screen_size;",
	"uniform vec4 glow;",
	"uniform vec4 lit_at;",
	"in vec2 colour_tex_uv;",
	"out vec4 colour;",
	"void main() {",
	"  vec4 s = texture(screen_tex, vec2(gl_FragCoord.x / screen_size.x, 1.0 - gl_FragCoord.y / screen_size.y));",
	"  if (s.a > 0.998) discard;",
	"  vec2 d = colour_tex_uv * 2.0 - 1.0;",
	"  float d2 = dot(d, d);",
	"  if (d2 >= 1.0) discard;",
	"  float lit = 0.0;",
	"  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) lit = max(lit, texture(over_lit_tex, lit_at.xy + vec2(float(x), float(y)) * lit_at.zw).r);",
	"  if (lit < 0.002) discard;",
	"  float f = glow.a * (1.0 - d2) * (1.0 - d2);",
	"  colour = vec4(glow.rgb * f, 1.0);",
	"}",
};
