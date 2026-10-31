/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/**
 * @file simtrains.cpp Trains of Microsoft Train Simulator (MSTS) in newgrf/simtrains/ (simtrains.h).
 *
 * What is read of the files of MSTS, from what they hold, by the game's own reading:
 *  - every file may begin "SIMISA@F", its length, "@@@@" and the rest packed by zlib; or "SIMISA@@@@@@@@@@" and the
 *    rest as it is;
 *  - the .eng is text, UTF-16: names followed by brackets, figures with their units ("944kW", "62.1mph", "73t");
 *  - the .s is the model, binary ("JINX0s1b"): blocks of a number, a length and a label, nested; the points, the
 *    normals, the texture's points, the matrices of the parts with their hierarchy, the textures and the triangles
 *    of each part, for each distance (the nearest is read);
 *  - the .ace is a texture: its rows of red, green, blue, a mask of a bit and alpha, for each size halved.
 */

#include "stdafx.h"
#include "simtrains.h"
#include "fileio_func.h"
#include "debug.h"
#include "string_func.h"
#include "core/math_func.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>

#if defined(WITH_ZLIB)
#	include <zlib.h>
#endif
#ifdef _WIN32
#	include <windows.h>
#endif

#include "safeguards.h"

namespace {

/** A whole file, or nothing. */
std::vector<uint8_t> ReadBytes(const std::string &path)
{
	std::vector<uint8_t> data;
	auto f = FileHandle::Open(path, "rb");
	if (!f.has_value()) return data;
	fseek(*f, 0, SEEK_END);
	long len = ftell(*f);
	fseek(*f, 0, SEEK_SET);
	if (len <= 0 || len > (256L << 20)) return data;
	data.resize(static_cast<size_t>(len));
	if (fread(data.data(), 1, data.size(), *f) != data.size()) data.clear();
	return data;
}

/** The file of a folder by its name whatever the case of its letters (MSTS came from Windows: "OKNA.ace" is okna.ace). */
std::string FindInFolder(const std::string &folder, const std::string &name)
{
	std::string exact = folder + name;
	if (FileExists(exact)) return exact;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) {
		std::string file = FS2OTTD(entry.path().filename().native());
		if (StrEqualsIgnoreCase(file, name)) return folder + file;
	}
	return {};
}

/**
 * A file named by a path of MSTS from a folder ("../../common.cab/cab451/451.cvf", "\\" or "/" between the names):
 * ".." the folder above, each name whatever the case of its letters. A folder only passed through by ".." need not be
 * there (an engine's CABVIEW, the folder MSTS reads its cab's path from).
 * @param folder where the path starts, ending with the path separator
 * @return the file, empty when it is not there
 */
std::string FindByMstsPath(const std::string &folder, const std::string &path)
{
	std::vector<std::string> names;
	std::string name;
	for (char c : path) {
		if (c == '\\' || c == '/') {
			if (!name.empty()) names.push_back(std::move(name));
			name.clear();
		} else {
			name += c;
		}
	}
	if (!name.empty()) names.push_back(std::move(name));
	if (names.empty()) return {};
	/* The folders of the start, so that ".." can go above it without asking the disk. */
	std::string at = folder;
	while (!at.empty() && at.back() == PATHSEPCHAR) at.pop_back();
	size_t i = 0;
	for (; i + 1 < names.size(); i++) {
		if (names[i] == ".") continue;
		if (names[i] == "..") {
			const size_t cut = at.find_last_of(PATHSEPCHAR);
			if (cut == std::string::npos) return {};
			at.resize(cut);
			continue;
		}
		const std::string found = FindInFolder(at + PATHSEP, names[i]);
		if (found.empty()) return {};
		at = found;
	}
	return FindInFolder(at + PATHSEP, names.back());
}

/**
 * The content of a file of MSTS past its header: unpacked when it is packed ("SIMISA@F").
 * @return false when it is packed and cannot be unpacked
 */
bool Unpack(std::vector<uint8_t> &data)
{
	static constexpr std::string_view PACKED = "SIMISA@F", PLAIN = "SIMISA@@@@@@@@@@";
	auto starts = [&data](std::string_view s) { return data.size() >= s.size() && std::equal(s.begin(), s.end(), data.begin()); };
	if (starts(PLAIN)) {
		data.erase(data.begin(), data.begin() + PLAIN.size());
		return true;
	}
	if (!starts(PACKED)) return true;
	if (data.size() < 16) return false;
#if defined(WITH_ZLIB)
	uLongf size = data[8] | data[9] << 8 | data[10] << 16 | static_cast<uLongf>(data[11]) << 24;
	if (size == 0 || size > (512u << 20)) return false;
	std::vector<uint8_t> out(size);
	if (uncompress(out.data(), &size, data.data() + 16, static_cast<uLong>(data.size() - 16)) != Z_OK) return false;
	out.resize(size);
	data = std::move(out);
	return true;
#else
	return false;
#endif
}

/* ---- The text of an .eng ---- */

/** A name of the text with what is in its brackets: words and inner names. */
struct TextNode {
	std::string name;
	std::vector<std::string> values;
	std::vector<TextNode> children;

	const TextNode *Child(std::string_view n) const
	{
		for (const TextNode &c : this->children) {
			if (StrEqualsIgnoreCase(c.name, n)) return &c;
		}
		return nullptr;
	}
	std::string Value(size_t i = 0) const { return i < this->values.size() ? this->values[i] : std::string(); }
};

/** The text of a file of MSTS as UTF-8: UTF-16 with its mark, else as it is. */
std::string TextOf(const std::vector<uint8_t> &data)
{
	std::string out;
	const bool marked = data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE;
	/* Unpacked text of a packed file may come without the mark: every second byte nought. */
	const bool unmarked = !marked && data.size() >= 8 && data[1] == 0 && data[3] == 0 && data[5] == 0 && data[0] != 0;
	if (marked || unmarked) {
		for (size_t i = marked ? 2 : 0; i + 1 < data.size(); i += 2) {
			char32_t c = data[i] | data[i + 1] << 8;
			if (c >= 0xD800 && c < 0xE000) continue;
			if (c < 0x80) {
				out += static_cast<char>(c);
			} else if (c < 0x800) {
				out += static_cast<char>(0xC0 | (c >> 6));
				out += static_cast<char>(0x80 | (c & 0x3F));
			} else {
				out += static_cast<char>(0xE0 | (c >> 12));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (c & 0x3F));
			}
		}
		return out;
	}
	return std::string(data.begin(), data.end());
}

/** The text read into names and their brackets; comments left out. */
std::vector<TextNode> ParseText(const std::string &text)
{
	std::vector<std::string> tokens;
	size_t i = 0;
	while (i < text.size()) {
		char c = text[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			i++;
		} else if (c == '(' || c == ')') {
			tokens.emplace_back(1, c);
			i++;
		} else if (c == '"') {
			size_t end = text.find('"', i + 1);
			if (end == std::string::npos) end = text.size();
			tokens.push_back(text.substr(i + 1, end - i - 1));
			i = end + 1;
		} else {
			size_t start = i;
			while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '(' && text[i] != ')') i++;
			tokens.push_back(text.substr(start, i - start));
		}
	}
	/* Recursive descent: a word followed by "(" opens a name. */
	size_t at = 0;
	auto parse = [&](auto &&self, std::vector<TextNode> &into, std::vector<std::string> *values) -> void {
		while (at < tokens.size()) {
			const std::string &t = tokens[at];
			if (t == ")") {
				at++;
				return;
			}
			if (t == "(") {
				/* A bracket without a name: read and forgotten. */
				at++;
				std::vector<TextNode> lost;
				self(self, lost, nullptr);
				continue;
			}
			if (at + 1 < tokens.size() && tokens[at + 1] == "(") {
				TextNode n;
				n.name = t;
				at += 2;
				self(self, n.children, &n.values);
				if (!StrEqualsIgnoreCase(n.name, "comment") && !StrEqualsIgnoreCase(n.name, "skip")) into.push_back(std::move(n));
				continue;
			}
			if (values != nullptr) values->push_back(t);
			at++;
		}
	};
	std::vector<TextNode> top;
	parse(parse, top, nullptr);
	return top;
}

/** A figure with its unit ("944kW", "62.1mph", "2.5*73t"), in the unit asked: the unit's factor to it, by its name. */
double ParseFigure(const std::string &text, std::initializer_list<std::pair<std::string_view, double>> units, double plain = 1)
{
	if (text.empty()) return 0;
	const char *b = text.data(), *e = text.data() + text.size();
	double v = 0;
	auto [p, ec] = std::from_chars(b, e, v);
	if (ec != std::errc()) return 0;
	/* "2.5*73t": the figure times the next. */
	if (p < e && *p == '*') return v * ParseFigure(std::string(p + 1, e), units, plain);
	std::string unit(p, e);
	if (unit.empty()) return v * plain;
	for (const auto &[name, factor] : units) {
		if (StrEqualsIgnoreCase(unit, name)) return v * factor;
	}
	return v * plain;
}

/* ---- The .s ---- */

/** The numbers of the blocks of a binary .s that are read (tokens of MSTS). */
enum ShapeToken : uint16_t {
	TOK_NORMALS = 5, TOK_POINTS = 7, TOK_UV_POINTS = 9, TOK_IMAGES = 14, TOK_TEXTURES = 16,
	TOK_LOD_CONTROLS = 31, TOK_DISTANCE_LEVELS = 36, TOK_DISTANCE_LEVEL_HEADER = 34, TOK_DLEVEL_SELECTION = 35,
	TOK_SUB_OBJECTS = 38, TOK_VTX_STATES = 47, TOK_VERTICES = 50, TOK_PRIMITIVES = 53, TOK_PRIM_STATES = 55,
	TOK_PRIM_STATE_IDX = 56, TOK_INDEXED_TRILIST = 60, TOK_VERTEX_IDXS = 63, TOK_MATRICES = 66, TOK_HIERARCHY = 67,
	TOK_SHAPE = 71,
};

/** A block of a binary .s: its number and where its content is. */
struct Block {
	uint16_t token = 0;
	size_t start = 0, end = 0;
	std::string label;
};

/** Reading a binary .s; every read is checked against the end of the data. */
class ShapeReader {
public:
	explicit ShapeReader(const std::vector<uint8_t> &d) : d(d) {}

	bool ok = true;

	bool BlockAt(size_t pos, size_t end, Block &b)
	{
		if (pos + 9 > end) return this->Fail();
		b.token = static_cast<uint16_t>(this->d[pos] | this->d[pos + 1] << 8);
		uint32_t len = this->U32(pos + 4);
		size_t body = pos + 8;
		if (len < 1 || body + len > end) return this->Fail();
		size_t label_len = this->d[body];
		b.label.clear();
		for (size_t k = 0; k < label_len && body + 2 + 2 * k < body + len; k++) b.label += static_cast<char>(this->d[body + 1 + 2 * k]);
		b.start = body + 1 + 2 * label_len;
		b.end = body + len;
		if (b.start > b.end) return this->Fail();
		return true;
	}

	/** The blocks within a block, after its count when it is a list. */
	std::vector<Block> Children(const Block &parent, bool counted)
	{
		std::vector<Block> out;
		size_t pos = parent.start + (counted ? 4 : 0);
		while (pos < parent.end && this->ok) {
			Block b;
			if (!this->BlockAt(pos, parent.end, b)) break;
			out.push_back(b);
			pos = b.end;
		}
		return out;
	}

	uint32_t U32(size_t pos)
	{
		if (pos + 4 > this->d.size()) {
			this->Fail();
			return 0;
		}
		return this->d[pos] | this->d[pos + 1] << 8 | this->d[pos + 2] << 16 | static_cast<uint32_t>(this->d[pos + 3]) << 24;
	}
	int32_t I32(size_t pos) { return static_cast<int32_t>(this->U32(pos)); }
	float F32(size_t pos)
	{
		uint32_t u = this->U32(pos);
		const float f = std::bit_cast<float>(u);
		return std::isfinite(f) ? f : 0.0f;
	}
	/** A string of the content: its length in characters, then UTF-16 (only the ASCII of it kept). */
	std::string WString(size_t pos)
	{
		if (pos + 2 > this->d.size()) {
			this->Fail();
			return {};
		}
		size_t n = this->d[pos] | this->d[pos + 1] << 8;
		std::string s;
		for (size_t k = 0; k < n && pos + 3 + 2 * k < this->d.size(); k++) s += static_cast<char>(this->d[pos + 2 + 2 * k]);
		return s;
	}

private:
	const std::vector<uint8_t> &d;
	bool Fail()
	{
		this->ok = false;
		return false;
	}
};

/** A matrix of a part: the three axes and the place, rows, as MSTS keeps them. */
struct Matrix {
	float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

	void Apply(float &x, float &y, float &z, bool place) const
	{
		const float nx = x * m[0] + y * m[3] + z * m[6] + (place ? m[9] : 0);
		const float ny = x * m[1] + y * m[4] + z * m[7] + (place ? m[10] : 0);
		const float nz = x * m[2] + y * m[5] + z * m[8] + (place ? m[11] : 0);
		x = nx;
		y = ny;
		z = nz;
	}
};

/* ---- The .ace ---- */

/** A colour of 5, 6 and 5 bits as red, green and blue of 8. */
void Rgb565(uint16_t c, uint8_t *out)
{
	const uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
	out[0] = static_cast<uint8_t>(r << 3 | r >> 2);
	out[1] = static_cast<uint8_t>(g << 2 | g >> 4);
	out[2] = static_cast<uint8_t>(b << 3 | b >> 2);
}

/**
 * A block of four by four pixels packed as DXT (S3TC): DXT1 is its colours, eight bytes; DXT3 and DXT5 have eight
 * bytes of alpha before them (four bits a pixel; or two alphas and three bits a pixel between them).
 * @param b the block
 * @param kind 1, 3 or 5
 * @param[out] out the sixteen pixels, RGBA, rows of four
 */
void DecodeDxtBlock(const uint8_t *b, int kind, uint8_t out[16][4])
{
	const uint8_t *colours = kind == 1 ? b : b + 8;
	const uint16_t c0 = colours[0] | colours[1] << 8, c1 = colours[2] | colours[3] << 8;
	uint8_t palette[4][4];
	Rgb565(c0, palette[0]);
	Rgb565(c1, palette[1]);
	palette[0][3] = palette[1][3] = palette[2][3] = palette[3][3] = 255;
	if (c0 > c1 || kind != 1) {
		for (int k = 0; k < 3; k++) {
			palette[2][k] = static_cast<uint8_t>((2 * palette[0][k] + palette[1][k]) / 3);
			palette[3][k] = static_cast<uint8_t>((palette[0][k] + 2 * palette[1][k]) / 3);
		}
	} else {
		/* Three colours and transparent. */
		for (int k = 0; k < 3; k++) {
			palette[2][k] = static_cast<uint8_t>((palette[0][k] + palette[1][k]) / 2);
			palette[3][k] = 0;
		}
		palette[3][3] = 0;
	}
	const uint32_t bits = colours[4] | colours[5] << 8 | colours[6] << 16 | static_cast<uint32_t>(colours[7]) << 24;
	for (int i = 0; i < 16; i++) std::copy_n(palette[(bits >> (2 * i)) & 3], 4, out[i]);

	if (kind == 3) {
		for (int i = 0; i < 16; i++) {
			const uint8_t a = (b[i / 2] >> (4 * (i % 2))) & 0x0F;
			out[i][3] = static_cast<uint8_t>(a << 4 | a);
		}
	} else if (kind == 5) {
		uint8_t alpha[8];
		alpha[0] = b[0];
		alpha[1] = b[1];
		if (alpha[0] > alpha[1]) {
			for (int k = 1; k < 7; k++) alpha[k + 1] = static_cast<uint8_t>(((7 - k) * alpha[0] + k * alpha[1]) / 7);
		} else {
			for (int k = 1; k < 5; k++) alpha[k + 1] = static_cast<uint8_t>(((5 - k) * alpha[0] + k * alpha[1]) / 5);
			alpha[6] = 0;
			alpha[7] = 255;
		}
		uint64_t abits = 0;
		for (int k = 0; k < 6; k++) abits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
		for (int i = 0; i < 16; i++) out[i][3] = alpha[(abits >> (3 * i)) & 7];
	}
}

/**
 * A texture of MSTS as RGBA: of its sizes halved, the first no wider or higher than a thousand pixels -- the game
 * draws vehicles small, and four textures of two thousand pixels would take a quarter of a gigabyte to keep.
 */
bool ReadAce(const std::string &path, SimtrainsMesh::Texture &tex)
{
	std::vector<uint8_t> d = ReadBytes(path);
	if (d.empty() || !Unpack(d) || d.size() < 152) return false;
	auto i32 = [&d](size_t p) { return p + 4 <= d.size() ? static_cast<int32_t>(d[p] | d[p + 1] << 8 | d[p + 2] << 16 | static_cast<uint32_t>(d[p + 3]) << 24) : 0; };
	const int32_t flags = i32(4), w = i32(8), h = i32(12), format = i32(16), channels = i32(20);
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || channels <= 0 || channels > 8) return false;
	/* The channels: their size in bits, their kind (2 the mask, 3 red, 4 green, 5 blue, 6 alpha). */
	std::vector<std::pair<int, int>> chans;
	size_t pos = 24 + 128;
	for (int c = 0; c < channels; c++) {
		chans.emplace_back(i32(pos), i32(pos + 8));
		pos += 16;
	}
	/* Of the sizes halved, the one wanted: which it is and its size. */
	const bool mips = (flags & 0x01) != 0;
	int lw = w, lh = h, level = 0;
	size_t row_index = 0;
	while (mips && (lw > 1024 || lh > 1024) && (lw > 1 || lh > 1)) {
		row_index += lh;
		lw = std::max(1, lw / 2);
		lh = std::max(1, lh / 2);
		level++;
	}
	if (!mips && (lw > 2048 || lh > 2048)) return false;
	tex.w = lw;
	tex.h = lh;
	tex.rgba.assign(static_cast<size_t>(lw) * lh * 4, 255);

	if ((flags & 0x10) != 0) {
		/* Packed as DXT: a table of where each size begins, then each size as its length and its blocks. */
		const int kind = format == 0x16 ? 5 : format == 0x14 ? 3 : 1;
		int levels = 1;
		for (int lw2 = w, lh2 = h; mips && (lw2 > 1 || lh2 > 1); lw2 = std::max(1, lw2 / 2), lh2 = std::max(1, lh2 / 2)) levels++;
		const size_t bw = static_cast<size_t>(std::max(1, (lw + 3) / 4)), bh = static_cast<size_t>(std::max(1, (lh + 3) / 4));
		const size_t block = kind == 1 ? 8 : 16, expected = bw * bh * block;
		/* Where the blocks of the wanted size are: after its length where the table says, or walking the sizes
		 * one after the other; both checked by the length the size must have. */
		size_t at = SIZE_MAX;
		const size_t table = pos;
		const size_t listed = static_cast<uint32_t>(i32(table + 4 * level));
		if (listed + 4 + expected <= d.size() && static_cast<size_t>(static_cast<uint32_t>(i32(listed))) == expected) at = listed + 4;
		if (at == SIZE_MAX) {
			size_t p = table + 4 * static_cast<size_t>(levels);
			for (int k = 0; k <= level && p + 4 <= d.size(); k++) {
				const size_t len = static_cast<uint32_t>(i32(p));
				if (k == level && len == expected && p + 4 + len <= d.size()) at = p + 4;
				p += 4 + len;
			}
		}
		if (at == SIZE_MAX && listed + expected <= d.size()) at = listed;
		if (at == SIZE_MAX) {
			Debug(grf, 0, "Simtrains: {}: the packed texture's size {}x{} is not where it should be", path, lw, lh);
			return false;
		}
		uint8_t px[16][4];
		for (size_t by = 0; by < bh; by++) {
			for (size_t bx = 0; bx < bw; bx++) {
				DecodeDxtBlock(&d[at + (by * bw + bx) * block], kind, px);
				for (int i = 0; i < 16; i++) {
					const size_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
					if (x < static_cast<size_t>(lw) && y < static_cast<size_t>(lh)) std::copy_n(px[i], 4, &tex.rgba[(y * lw + x) * 4]);
				}
			}
		}
		return true;
	}

	for (int y = 0; y < lh; y++) {
		size_t row = static_cast<uint32_t>(i32(pos + (row_index + y) * 4));
		for (const auto &[bits, kind] : chans) {
			const size_t bytes = bits == 1 ? static_cast<size_t>((lw + 7) / 8) : static_cast<size_t>(lw);
			if (row + bytes > d.size()) return false;
			const int slot = kind == 3 ? 0 : kind == 4 ? 1 : kind == 5 ? 2 : kind == 6 ? 3 : kind == 2 ? 4 : -1;
			uint8_t *out = &tex.rgba[static_cast<size_t>(y) * lw * 4];
			if (slot >= 0 && slot < 4 && bits == 8) {
				for (int x = 0; x < lw; x++) out[x * 4 + slot] = d[row + x];
			} else if (slot == 4 && bits == 1) {
				/* The mask: transparent where it is nought, unless an alpha channel says otherwise after it. */
				for (int x = 0; x < lw; x++) {
					if ((d[row + x / 8] & (0x80 >> (x % 8))) == 0) out[x * 4 + 3] = 0;
				}
			}
			row += bytes;
		}
	}
	return true;
}

} // namespace

/** A texture of MSTS (.ace) as RGBA, of its sizes halved the first no bigger than 1024 (for the rig, test3d ace). */
bool ReadSimtrainsTexture(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba)
{
	SimtrainsMesh::Texture t;
	if (!ReadAce(path, t)) return false;
	w = t.w;
	h = t.h;
	rgba = std::move(t.rgba);
	return true;
}

namespace {

/**
 * The picture of a cab by night: MSTS looks for it in the folder "night" beside the picture or its .cvf; the cabs of
 * the Czech authors have "den" and "noc" side by side (451.cvf, cab_den.bat and cab_noc.bat copying one over).
 * @return the night's, empty none
 */
std::string NightPictureOf(const std::string &picture, const std::string &cvf)
{
	const size_t slash = picture.find_last_of(PATHSEPCHAR);
	if (slash == std::string::npos) return {};
	const std::string folder = picture.substr(0, slash + 1), file = picture.substr(slash + 1);
	for (const char *night : {"night", "noc"}) {
		std::string found = FindByMstsPath(folder, std::string(night) + "/" + file);
		if (!found.empty()) return found;
		const size_t cvf_slash = cvf.find_last_of(PATHSEPCHAR);
		if (cvf_slash != std::string::npos) {
			found = FindByMstsPath(cvf.substr(0, cvf_slash + 1), std::string(night) + "/" + file);
			if (!found.empty()) return found;
		}
	}
	/* ".../den/x.ace" by day, ".../noc/x.ace" by night. */
	const std::string above = folder.substr(0, folder.size() - 1);
	const size_t up = above.find_last_of(PATHSEPCHAR);
	if (up == std::string::npos) return {};
	const std::string day = above.substr(up + 1);
	if (StrEqualsIgnoreCase(day, "den") || StrEqualsIgnoreCase(day, "day")) {
		for (const char *night : {"noc", "night"}) {
			const std::string found = FindByMstsPath(above.substr(0, up + 1), std::string(night) + "/" + file);
			if (!found.empty()) return found;
		}
	}
	return {};
}

/** The figures of a name's values, as many as it has. */
std::vector<float> FiguresOf(const TextNode *n)
{
	std::vector<float> out;
	if (n == nullptr) return out;
	for (const std::string &v : n->values) out.push_back(static_cast<float>(ParseFigure(v, {})));
	return out;
}

} // namespace

/**
 * Read a cab of MSTS (SimtrainsCab).
 * @param path its .cvf
 * @return false when it cannot be read or has no picture
 */
bool ReadSimtrainsCab(const std::string &path, SimtrainsCab &cab)
{
	std::vector<uint8_t> data = ReadBytes(path);
	if (data.empty() || !Unpack(data)) return false;
	const std::vector<TextNode> top = ParseText(TextOf(data));
	const TextNode *file = nullptr;
	for (const TextNode &n : top) {
		if (StrEqualsIgnoreCase(n.name, "Tr_CabViewFile")) file = &n;
	}
	if (file == nullptr) return false;
	cab = {};
	cab.path = path;
	const size_t slash = path.find_last_of(PATHSEPCHAR);
	const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
	/* The pictures and their places, one after the other: a "CabViewFile", then its "Position" and "Direction". */
	for (const TextNode &n : file->children) {
		if (StrEqualsIgnoreCase(n.name, "CabViewFile")) {
			SimtrainsCab::View view;
			view.image = FindByMstsPath(folder, n.Value());
			if (view.image.empty()) Debug(grf, 1, "Simtrains: {}: its picture {} is not there", path, n.Value());
			view.night = view.image.empty() ? std::string() : NightPictureOf(view.image, path);
			cab.views.push_back(std::move(view));
		} else if (StrEqualsIgnoreCase(n.name, "Position") && !cab.views.empty()) {
			const std::vector<float> f = FiguresOf(&n);
			if (f.size() >= 3) {
				/* MSTS: x to the right, y up, z forward. */
				SimtrainsCab::View &view = cab.views.back();
				view.eye[0] = f[2];
				view.eye[1] = -f[0];
				view.eye[2] = f[1];
			}
		} else if (StrEqualsIgnoreCase(n.name, "Direction") && !cab.views.empty()) {
			const std::vector<float> f = FiguresOf(&n);
			if (f.size() >= 2) {
				cab.views.back().pitch = f[0];
				cab.views.back().yaw = f[1];
			}
		}
	}
	std::erase_if(cab.views, [](const SimtrainsCab::View &v) { return v.image.empty(); });
	if (cab.views.empty()) return false;

	const TextNode *controls = file->Child("CabViewControls");
	if (controls == nullptr) return true;
	for (const TextNode &n : controls->children) {
		SimtrainsCab::Control c;
		c.kind = n.name;
		if (const TextNode *t = n.Child("Type"); t != nullptr) c.type = t->Value();
		if (const std::vector<float> f = FiguresOf(n.Child("Position")); f.size() >= 4) {
			c.x = static_cast<int>(f[0]);
			c.y = static_cast<int>(f[1]);
			c.w = static_cast<int>(f[2]);
			c.h = static_cast<int>(f[3]);
		}
		if (const TextNode *g = n.Child("Graphic"); g != nullptr && !g->Value().empty()) {
			c.graphic = FindByMstsPath(folder, g->Value());
			if (!c.graphic.empty()) c.graphic_night = NightPictureOf(c.graphic, path);
		}
		if (const TextNode *t = n.Child("Style"); t != nullptr) c.style = t->Value();
		if (const std::vector<float> f = FiguresOf(n.Child("ScaleRange")); f.size() >= 2) {
			c.min = f[0];
			c.max = f[1];
		}
		if (const std::vector<float> f = FiguresOf(n.Child("ScalePos")); f.size() >= 2) {
			c.from = f[0];
			c.to = f[1];
		}
		if (const std::vector<float> f = FiguresOf(n.Child("Pivot")); !f.empty()) c.pivot = static_cast<int>(f[0]);
		if (const std::vector<float> f = FiguresOf(n.Child("DirIncrease")); !f.empty()) c.dir_increase = static_cast<int>(f[0]);
		if (const std::vector<float> f = FiguresOf(n.Child("Orientation")); !f.empty()) c.orientation = static_cast<int>(f[0]);
		if (const std::vector<float> f = FiguresOf(n.Child("Area")); f.size() >= 4) {
			for (int k = 0; k < 4; k++) c.area[k] = static_cast<int>(f[k]);
		}
		if (const std::vector<float> f = FiguresOf(n.Child("NumFrames")); f.size() >= 3) {
			c.frames = static_cast<int>(f[0]);
			c.cols = static_cast<int>(f[1]);
			c.rows = static_cast<int>(f[2]);
		}
		/* "NumPositions ( 8 0 1 2 ... )": the count first. */
		if (const std::vector<float> f = FiguresOf(n.Child("NumPositions")); f.size() >= 2) {
			for (size_t k = 1; k < f.size(); k++) c.positions.push_back(static_cast<int>(f[k]));
		}
		if (const std::vector<float> f = FiguresOf(n.Child("NumValues")); f.size() >= 2) c.values.assign(f.begin() + 1, f.end());
		if (const std::vector<float> f = FiguresOf(n.Child("MouseControl")); !f.empty()) c.mouse = f[0] != 0;
		const TextNode *colour = n.Child("ControlColour");
		if (colour == nullptr) {
			if (const TextNode *pos = n.Child("PositiveColour"); pos != nullptr) colour = pos->Child("ControlColour");
		}
		if (const std::vector<float> f = FiguresOf(colour); f.size() >= 3) {
			c.colour = static_cast<uint32_t>(Clamp(static_cast<int>(f[0]), 0, 255)) << 16 | static_cast<uint32_t>(Clamp(static_cast<int>(f[1]), 0, 255)) << 8 | static_cast<uint32_t>(Clamp(static_cast<int>(f[2]), 0, 255));
		}
		cab.controls.push_back(std::move(c));
	}
	return true;
}

/**
 * The driver's eye of a cab of MSTS: where its first picture is seen from.
 * @param eye metres in the frame of the game's objects (x forward, y left, z up)
 * @return false when the cab cannot be read
 */
bool ReadSimtrainsCabEye(const std::string &path, float eye[3])
{
	SimtrainsCab cab;
	if (!ReadSimtrainsCab(path, cab)) return false;
	std::copy(std::begin(cab.views[0].eye), std::end(cab.views[0].eye), eye);
	return true;
}

namespace {

/** The GRF's id of an engine: "ST" and a number of its .eng's name, the same on every computer. */
uint32_t GrfIdOf(const std::string &eng)
{
	uint32_t h = 2166136261u;
	for (char c : eng) {
		h ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
		h *= 16777619u;
	}
	return 'S' | 'T' << 8 | ((h ^ (h >> 16)) & 0xFFFF) << 16;
}

/** The .eng and .wag files of a Trains/Trainset folder of MSTS. */
class EngScanner : public FileScanner {
public:
	std::vector<std::string> files;

	bool AddFile(const std::string &filename, size_t, const std::string &tar_filename) override
	{
		if (!tar_filename.empty()) return false;
		this->files.push_back(filename);
		return true;
	}
};

#ifdef _WIN32
/** A text of the registry of Windows, or nothing. */
std::string RegistryText(HKEY root, const wchar_t *key, const wchar_t *value, REGSAM view)
{
	HKEY k;
	if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
	wchar_t buf[MAX_PATH * 2] = {};
	DWORD size = static_cast<DWORD>(sizeof(buf) - sizeof(wchar_t));
	DWORD type = 0;
	std::string out;
	if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) out = FS2OTTD(buf);
	RegCloseKey(k);
	return out;
}

/** Every text of a key of the registry of Windows. */
void RegistryTexts(HKEY root, const wchar_t *key, std::vector<std::string> &out)
{
	HKEY k;
	if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
	for (DWORD i = 0; i < 256; i++) {
		wchar_t name[256] = {};
		DWORD name_len = static_cast<DWORD>(std::size(name));
		wchar_t data[MAX_PATH * 2] = {};
		DWORD data_size = static_cast<DWORD>(sizeof(data) - sizeof(wchar_t));
		DWORD type = 0;
		LONG r = RegEnumValueW(k, i, name, &name_len, nullptr, &type, reinterpret_cast<LPBYTE>(data), &data_size);
		if (r == ERROR_NO_MORE_ITEMS) break;
		if (r == ERROR_SUCCESS && type == REG_SZ) out.push_back(FS2OTTD(data));
	}
	RegCloseKey(k);
}
#endif

/** A folder of a folder by its name whatever the case of its letters, with the separator at the end; empty when there is none. */
std::string SubFolder(const std::string &folder, std::string_view name)
{
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) {
		if (!entry.is_directory(ec)) continue;
		std::string sub = FS2OTTD(entry.path().filename().native());
		if (StrEqualsIgnoreCase(sub, name)) return folder + sub + PATHSEP;
	}
	return {};
}

/**
 * The folders the trains are looked for in: newgrf/simtrains/ of the game's folders, for a player without MSTS (the
 * player: "in newgrf simtrains too, what if the player has no simtrains at all"); and the folders MSTS keeps its
 * trains in, Trains/Trainset of each that is there: the folders the player names ("simtrains_path" of the
 * configuration, separated by ';'), the one MSTS was installed in and those Open Rails was given (the registry of
 * Windows), and where MSTS usually is.
 */
std::vector<std::string> TrainsetFolders()
{
	std::vector<std::string> folders;
	auto add = [&folders](const std::string &folder) {
		std::string key = folder;
		strtolower(key);
		if (std::ranges::any_of(folders, [&key](const std::string &f) { std::string k = f; strtolower(k); return k == key; })) return;
		folders.push_back(folder);
	};
	for (Searchpath sp : _valid_searchpaths) {
		std::string dir = FioGetDirectory(sp, Subdirectory::NewGrf);
		if (dir.empty()) continue;
		std::string own = SubFolder(dir, "simtrains");
		if (!own.empty()) add(own);
	}
	std::vector<std::string> roots;
	for (std::string_view rest = _simtrains_path; !rest.empty();) {
		size_t semi = rest.find(';');
		std::string one(rest.substr(0, semi));
		StrTrimInPlace(one);
		if (!one.empty()) roots.push_back(one);
		rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
	}
#ifdef _WIN32
	std::string msts = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Microsoft Games\\Train Simulator\\1.0", L"Path", KEY_WOW64_32KEY);
	if (!msts.empty()) roots.push_back(msts);
	RegistryTexts(HKEY_CURRENT_USER, L"Software\\OpenRails\\ORTS\\Folders", roots);
	for (const char *usual : {"C:\\Program Files (x86)\\Microsoft Games\\Train Simulator", "C:\\Program Files\\Microsoft Games\\Train Simulator",
			"C:\\Train Simulator", "C:\\MSTS", "D:\\Train Simulator", "D:\\MSTS"}) {
		roots.emplace_back(usual);
	}
#endif
	for (std::string root : roots) {
		if (root.empty()) continue;
		if (root.back() != PATHSEPCHAR) root += PATHSEP;
		std::string trains = SubFolder(root, "Trains");
		std::string trainset = trains.empty() ? std::string() : SubFolder(trains, "Trainset");
		/* The player may name the Trainset folder itself, or Trains. */
		if (trainset.empty()) trainset = SubFolder(root, "Trainset");
		if (!trainset.empty()) add(trainset);
	}
	return folders;
}

/** The folder the GRFs made of the trains go in: newgrf/simtrains/ of the player's own folder (the NewGRFs are looked for there). */
std::string GrfFolder()
{
	std::string dir = FioGetDirectory(Searchpath::PersonalDir, Subdirectory::NewGrf);
	if (dir.empty()) return {};
	return dir + "simtrains" PATHSEP;
}

/**
 * Its length of the "Size" of its wagon (width, height, length), and the vehicles of the game it is made of: a
 * vehicle of the game is at most a step of the chain of eight, a step of diagonal track (as the vehicle objects,
 * three_d_vehicle_object.cpp, a tile 14.84 m), so a longer one is articulated -- else its model is longer than the
 * place the game keeps for it and the next vehicle's runs into it (the player, 16. 10.: "they are pressed into each
 * other, no gap").
 */
void ReadSize(const TextNode *wagon, SimtrainsEngine &e)
{
	const TextNode *size = wagon->Child("Size");
	if (size == nullptr) return;
	e.length_m = ParseFigure(size->Value(2), {{"m", 1}, {"cm", 0.01}, {"mm", 0.001}, {"ft", 0.3048}, {"in", 0.0254}});
	static constexpr double STEPS_PER_METRE = 16.0 / 14.83765 / 1.41421356;
	const int steps = static_cast<int>(std::lround(e.length_m * STEPS_PER_METRE));
	e.parts = Clamp((steps + 7) / 8, 1, 8);
}

/**
 * A vehicle of its .eng or .wag, if it is one with a model: an engine of an .eng; a carriage or a goods wagon of a
 * .wag (a tender, and a .wag that is an engine of the same folder hauled dead, its model the engine's, are left out).
 */
std::optional<SimtrainsEngine> ReadEng(const std::string &path)
{
	std::vector<uint8_t> data = ReadBytes(path);
	if (data.empty() || !Unpack(data)) return std::nullopt;
	std::vector<TextNode> top = ParseText(TextOf(data));
	const TextNode *wagon = nullptr, *engine = nullptr;
	for (const TextNode &n : top) {
		if (StrEqualsIgnoreCase(n.name, "Wagon")) wagon = &n;
		if (StrEqualsIgnoreCase(n.name, "Engine")) engine = &n;
	}
	const bool is_wag = path.size() > 4 && StrEqualsIgnoreCase(std::string_view(path).substr(path.size() - 4), ".wag");
	if (wagon == nullptr || (engine == nullptr && !is_wag)) return std::nullopt;
	if (is_wag) {
		/* A wagon. */
		SimtrainsEngine e;
		size_t slash = path.find_last_of(PATHSEPCHAR);
		e.folder = path.substr(0, slash + 1);
		e.eng = path.substr(slash + 1, path.size() - slash - 1 - 4);
		e.wagon = true;
		const TextNode *type = wagon->Child("Type");
		const std::string kind = type != nullptr ? type->Value() : std::string();
		if (StrEqualsIgnoreCase(kind, "Tender") || StrEqualsIgnoreCase(kind, "Engine")) return std::nullopt;
		e.passengers = StrEqualsIgnoreCase(kind, "Carriage");
		const TextNode *shape = wagon->Child("WagonShape");
		if (shape == nullptr) return std::nullopt;
		e.shape = FindInFolder(e.folder, shape->Value());
		if (e.shape.empty()) {
			Debug(grf, 0, "Simtrains: {}: its model {} is not there", path, shape->Value());
			return std::nullopt;
		}
		/* An engine of this folder with the same model: the engine hauled dead ("ms"), not a wagon. */
		std::error_code ec;
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(e.folder), ec)) {
			std::string file = FS2OTTD(entry.path().filename().native());
			if (file.size() < 5 || !StrEqualsIgnoreCase(std::string_view(file).substr(file.size() - 4), ".eng")) continue;
			std::vector<uint8_t> other = ReadBytes(e.folder + file);
			if (other.empty() || !Unpack(other)) continue;
			for (const TextNode &n : ParseText(TextOf(other))) {
				const TextNode *s = StrEqualsIgnoreCase(n.name, "Wagon") ? n.Child("WagonShape") : nullptr;
				if (s != nullptr && StrEqualsIgnoreCase(s->Value(), shape->Value())) return std::nullopt;
			}
		}
		const TextNode *name = wagon->Child("Name");
		e.name = name != nullptr && !name->Value().empty() ? name->Value() : e.eng;
		if (const TextNode *t = wagon->Child("Mass"); t != nullptr) e.mass_t = ParseFigure(t->Value(), {{"t", 1}, {"kg", 0.001}, {"t-uk", 1.016}, {"t-us", 0.907}, {"lb", 0.000454}}, 0.001);
		if (e.passengers) {
			const TextNode *seats = wagon->Child("PassengerCapacity");
			e.capacity = seats != nullptr ? static_cast<int>(std::lround(ParseFigure(seats->Value(), {}))) : 0;
			if (e.capacity <= 0) e.capacity = 40;
		} else {
			/* MSTS says nothing of what a goods wagon carries: as much as an original wagon of the game. */
			e.capacity = 30;
		}
		e.capacity = Clamp(e.capacity, 1, 255);
		if (e.mass_t <= 0) e.mass_t = 20;
		ReadSize(wagon, e);
		e.grfid = GrfIdOf(e.eng);
		return e;
	}

	SimtrainsEngine e;
	size_t slash = path.find_last_of(PATHSEPCHAR);
	e.folder = path.substr(0, slash + 1);
	e.eng = path.substr(slash + 1, path.size() - slash - 1 - 4);
	const TextNode *shape = wagon->Child("WagonShape");
	if (shape == nullptr) return std::nullopt;
	e.shape = FindInFolder(e.folder, shape->Value());
	if (e.shape.empty()) {
		Debug(grf, 0, "Simtrains: {}: its model {} is not there", path, shape->Value());
		return std::nullopt;
	}
	const TextNode *name = engine->Child("Name");
	if (name == nullptr) name = wagon->Child("Name");
	e.name = name != nullptr && !name->Value().empty() ? name->Value() : e.eng;

	if (const TextNode *t = engine->Child("Type"); t != nullptr) {
		if (StrEqualsIgnoreCase(t->Value(), "Electric")) e.traction = SimtrainsEngine::Traction::Electric;
		if (StrEqualsIgnoreCase(t->Value(), "Steam")) e.traction = SimtrainsEngine::Traction::Steam;
	}
	if (const TextNode *t = engine->Child("MaxPower"); t != nullptr) e.power_kw = ParseFigure(t->Value(), {{"kW", 1}, {"W", 0.001}, {"hp", 0.7457}, {"MW", 1000}}, 0.001);
	if (const TextNode *t = engine->Child("MaxForce"); t != nullptr) e.force_kn = ParseFigure(t->Value(), {{"kN", 1}, {"N", 0.001}, {"lbf", 0.004448}}, 0.001);
	if (const TextNode *t = engine->Child("MaxVelocity"); t != nullptr) e.speed_kmh = ParseFigure(t->Value(), {{"mph", 1.609344}, {"kmh", 1}, {"km/h", 1}, {"kph", 1}, {"m/s", 3.6}}, 3.6);
	if (const TextNode *t = wagon->Child("Mass"); t != nullptr) e.mass_t = ParseFigure(t->Value(), {{"t", 1}, {"kg", 0.001}, {"t-uk", 1.016}, {"t-us", 0.907}, {"lb", 0.000454}}, 0.001);
	/* The driver's eye when the engine's cab of MSTS is not there (its .cvf has the eye, three_d_objects.cpp): where the
	 * driver's head looks out of the side window ("HeadOut"), drawn in inside the body to the seat by that window,
	 * looking ahead. Not "Inside" with its PassengerCabinHeadPos: that is a passenger's place, looking out to the side
	 * (the ZSSK 750's is its side window; the player, 31. 10.: "the 750: I look as from a steam engine's side window"). */
	if (const TextNode *head = engine->Child("HeadOut"); head != nullptr && head->values.size() >= 3) {
		float x = static_cast<float>(ParseFigure(head->Value(0), {})), y = static_cast<float>(ParseFigure(head->Value(1), {})), z = static_cast<float>(ParseFigure(head->Value(2), {}));
		double width_m = 3.0;
		if (const TextNode *size = wagon->Child("Size"); size != nullptr && size->values.size() >= 3) {
			width_m = ParseFigure(size->Value(0), {{"m", 1}, {"cm", 0.01}, {"mm", 0.001}, {"ft", 0.3048}, {"in", 0.0254}});
		}
		/* The seat's middle 0.8 m in from the side of the body. */
		const float in = static_cast<float>(std::max(0.0, width_m / 2 - 0.8));
		x = std::clamp(x, -in, in);
		/* MSTS: x to the right, y up, z forward. */
		e.cab[0] = z;
		e.cab[1] = -x;
		e.cab[2] = y;
		e.has_cab = true;
	}
	/* The notches of its throttle and whether it brakes electrically: the desk's controls (simtrains_cab.h). */
	if (const TextNode *controllers = engine->Child("EngineControllers"); controllers != nullptr) {
		if (const TextNode *throttle = controllers->Child("Throttle"); throttle != nullptr) {
			if (const TextNode *notches = throttle->Child("NumNotches"); notches != nullptr) {
				for (const TextNode &n : notches->children) {
					if (!StrEqualsIgnoreCase(n.name, "Notch")) continue;
					const float f = static_cast<float>(ParseFigure(n.Value(), {}));
					if (f > 0.001f) e.notches.push_back(std::min(f, 1.0f));
				}
			}
		}
		if (controllers->Child("Brake_Dynamic") != nullptr) e.dynamic = true;
	}
	if (const TextNode *t = engine->Child("DynamicBrakesMaximumForce"); t != nullptr && ParseFigure(t->Value(), {{"kN", 1}, {"N", 0.001}}, 0.001) > 0) e.dynamic = true;
	if (const TextNode *cab = engine->Child("CabView"); cab != nullptr && !cab->Value().empty()) {
		/* MSTS reads it from the folder CABVIEW of the engine's; a cab of several engines is in a folder above. */
		e.cvf = FindByMstsPath(e.folder + "CABVIEW" PATHSEP, cab->Value());
		if (e.cvf.empty()) e.cvf = FindByMstsPath(e.folder, cab->Value());
		if (e.cvf.empty()) Debug(grf, 1, "Simtrains: {}: its cab {} is not there", path, cab->Value());
	}
	if (e.power_kw <= 0 || e.speed_kmh <= 0 || e.mass_t <= 0) {
		Debug(grf, 0, "Simtrains: {}: no power, speed or mass", path);
		return std::nullopt;
	}
	ReadSize(wagon, e);
	e.grfid = GrfIdOf(e.eng);
	return e;
}

/* ---- The GRF ---- */

/** The bytes of a NewGRF of one engine (container version 1, GRF version 8: Actions 8, 0, 2 and 3 for its articulated parts, and 4; no sprites). */
std::vector<uint8_t> MakeGrf(const SimtrainsEngine &e)
{
	std::vector<std::vector<uint8_t>> sprites;
	auto text = [](std::vector<uint8_t> &b, const std::string &s) {
		/* "Þ" first: the text is UTF-8. */
		b.push_back(0xC3);
		b.push_back(0x9E);
		b.insert(b.end(), s.begin(), s.end());
		b.push_back(0);
	};
	auto word = [](std::vector<uint8_t> &b, uint32_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); };
	auto dword = [&word](std::vector<uint8_t> &b, uint32_t v) { word(b, v & 0xFFFF); word(b, v >> 16); };

	{
		std::vector<uint8_t> a8 = {0x08, 0x08};
		dword(a8, e.grfid);
		text(a8, "Simtrains: " + e.name);
		text(a8, std::string(e.wagon ? "Vůz" : "Vlak") + " z Microsoft Train Simulatoru (" + e.eng + (e.wagon ? ".wag" : ".eng") +
				").");
		sprites.push_back(std::move(a8));
	}

	const double hp = e.power_kw / 0.7457;
	/* Prices by the original game's diesel of the same power (SH/Hendry '25': 1250 hp, cost 15, running 95). */
	const double rel = std::max(hp, 100.0) / 1250.0;
	const uint8_t cost = static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(15 * std::pow(rel, 0.45))), 5, 255));
	const uint8_t running = static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(95 * std::pow(rel, 0.4))), 20, 255));
	const uint32_t weight = static_cast<uint32_t>(Clamp<int>(static_cast<int>(std::lround(e.mass_t)), 1, 1023));
	/* The tractive effort, as the force against the weight, at most the original engines' 0.3. */
	int effort = 76;
	if (e.force_kn > 0) effort = Clamp<int>(static_cast<int>(std::lround(e.force_kn / (e.mass_t * 9.81) * 256)), 20, 76);
	uint8_t traction = 0x08;
	uint32_t running_base = 0x4C36;
	if (e.traction == SimtrainsEngine::Traction::Electric) {
		traction = 0x28;
		running_base = 0x4C3C;
	} else if (e.traction == SimtrainsEngine::Traction::Steam) {
		traction = 0x00;
		running_base = 0x4C30;
	}
	if (e.wagon) {
		/* A wagon: no power, no speed of its own; passengers, or goods of any kind but passengers. Priced as the
		 * original game's wagons (its passenger carriage 247). */
		std::vector<uint8_t> a0 = {0x00, 0x00, 0, 0x01, static_cast<uint8_t>(SIMTRAINS_ENGINE_ID)};
		uint8_t props = 0;
		auto prop = [&](uint8_t p) { a0.push_back(p); props++; };
		prop(0x00); word(a0, 0);                       // introduced 1920: there in every game
		prop(0x02); a0.push_back(20);
		prop(0x03); a0.push_back(40);
		prop(0x04); a0.push_back(0xFF);
		prop(0x06); a0.push_back(0x0F);
		prop(0x07); a0.push_back(5);
		prop(0x05); a0.push_back(0);
		prop(0x09); word(a0, 0);                       // no speed limit of its own
		prop(0x0B); word(a0, 0);                       // no power: a wagon
		prop(0x0D); a0.push_back(0);
		prop(0x12); a0.push_back(0xFD);                // drawn from its model
		prop(0x14); a0.push_back(static_cast<uint8_t>(e.capacity));
		prop(0x15); a0.push_back(e.passengers ? 0x00 : 0xFF); // passengers, or the first goods it takes
		prop(0x28); word(a0, e.passengers ? 0x0001 : 0xFFFE); // the classes of cargo it may be refitted to: a goods wagon every one but passengers
		prop(0x29); word(a0, e.passengers ? 0x0000 : 0x8001); // and not the road vehicles (CT_ROLA, special): the player's car carriers only
		prop(0x16); a0.push_back(weight & 0xFF);
		prop(0x24); a0.push_back(static_cast<uint8_t>(weight >> 8));
		prop(0x17); a0.push_back(e.passengers ? 247 : 200);
		if (e.parts > 1) { prop(0x1E); a0.push_back(0x10); } // its articulated parts by callback 16
		a0[2] = props;
		sprites.push_back(std::move(a0));
	} else {
		std::vector<uint8_t> a0 = {0x00, 0x00, 0, 0x01, static_cast<uint8_t>(SIMTRAINS_ENGINE_ID)};
		uint8_t props = 0;
		auto prop = [&](uint8_t p) { a0.push_back(p); props++; };
		prop(0x00); word(a0, 0);                       // introduced 1920: there in every game
		prop(0x02); a0.push_back(20);                  // reliability decay
		prop(0x03); a0.push_back(30);                  // a vehicle's life, years
		prop(0x04); a0.push_back(0xFF);                // the model never retires
		prop(0x06); a0.push_back(0x0F);                // every climate
		prop(0x07); a0.push_back(5);                   // loading speed
		prop(0x05); a0.push_back(0);                   // plain rail (electrified for an electric, by its traction)
		prop(0x19); a0.push_back(traction);
		prop(0x09); word(a0, static_cast<uint32_t>(Clamp<int>(static_cast<int>(std::lround(e.speed_kmh / 1.00584)), 1, 0xFFFE)));
		prop(0x0B); word(a0, static_cast<uint32_t>(Clamp<int>(static_cast<int>(std::lround(hp)), 1, 0xFFFF)));
		prop(0x13); a0.push_back(0);                   // one head
		prop(0x0D); a0.push_back(running);
		prop(0x0E); dword(a0, running_base);
		prop(0x12); a0.push_back(0xFD);                // the set's own sprites: drawn from its model (GetThreeDVehicleObjectSprite())
		prop(0x14); a0.push_back(0);                   // no cargo
		prop(0x16); a0.push_back(weight & 0xFF);
		prop(0x24); a0.push_back(static_cast<uint8_t>(weight >> 8));
		prop(0x17); a0.push_back(cost);
		prop(0x1F); a0.push_back(static_cast<uint8_t>(effort));
		if (e.parts > 1) { prop(0x1E); a0.push_back(0x10); } // its articulated parts by callback 16
		a0[2] = props;
		sprites.push_back(std::move(a0));
	}
	/* A vehicle longer than one of the game: the parts after it, the numbers after its own (ReadSize()). Each is
	 * nothing but its length -- no power, weight or load, in no climate so never bought alone; the model is drawn
	 * over them all from the middle one. */
	for (int k = 1; k < e.parts; k++) {
		std::vector<uint8_t> a0 = {0x00, 0x00, 0, 0x01, static_cast<uint8_t>(SIMTRAINS_ENGINE_ID + k)};
		uint8_t props = 0;
		auto prop = [&](uint8_t p) { a0.push_back(p); props++; };
		prop(0x00); word(a0, 0);
		prop(0x06); a0.push_back(0x00);                // no climate: a part only
		prop(0x05); a0.push_back(0);
		prop(0x09); word(a0, 0);
		prop(0x0B); word(a0, 0);
		prop(0x0D); a0.push_back(0);
		prop(0x12); a0.push_back(0xFD);                // drawn with the model (GetThreeDVehicleObjectSprite())
		prop(0x14); a0.push_back(0);
		if (e.wagon) {
			prop(0x15); a0.push_back(e.passengers ? 0x00 : 0xFF);
			prop(0x28); word(a0, e.passengers ? 0x0001 : 0xFFFE);
			prop(0x29); word(a0, e.passengers ? 0x0000 : 0x8001);
		}
		prop(0x16); a0.push_back(0);
		a0[2] = props;
		sprites.push_back(std::move(a0));
	}
	if (e.parts > 1) {
		/* Callback 16 (the articulated parts): for the part asked for (variable 10, from 1) its number, after the
		 * last 7FFF; any other callback fails, and so does the picture (the model is drawn instead). */
		std::vector<uint8_t> parts = {0x02, 0x00, 0x00, 0x81, 0x10, 0x00, 0xFF, static_cast<uint8_t>(e.parts - 1)};
		for (int k = 1; k < e.parts; k++) {
			word(parts, 0x8000 | (SIMTRAINS_ENGINE_ID + k));
			parts.push_back(static_cast<uint8_t>(k));
			parts.push_back(static_cast<uint8_t>(k));
		}
		word(parts, 0xFFFF);
		sprites.push_back(std::move(parts));
		std::vector<uint8_t> which = {0x02, 0x00, 0x01, 0x89, 0x0C, 0x00};
		dword(which, 0xFFFF);
		which.push_back(1);
		word(which, 0x0000);
		dword(which, 0x16);
		dword(which, 0x16);
		word(which, 0x7FFF);
		sprites.push_back(std::move(which));
		std::vector<uint8_t> a3 = {0x03, 0x00, 0x01, static_cast<uint8_t>(SIMTRAINS_ENGINE_ID), 0x00};
		word(a3, 0x0001);
		sprites.push_back(std::move(a3));
	}
	{
		std::vector<uint8_t> a4 = {0x04, 0x00, 0x7F, 0x01, static_cast<uint8_t>(SIMTRAINS_ENGINE_ID)};
		text(a4, e.name);
		sprites.push_back(std::move(a4));
	}

	std::vector<uint8_t> grf;
	word(grf, 4);
	grf.push_back(0xFF);
	dword(grf, static_cast<uint32_t>(sprites.size()));
	for (const std::vector<uint8_t> &s : sprites) {
		word(grf, static_cast<uint32_t>(s.size()));
		grf.push_back(0xFF);
		grf.insert(grf.end(), s.begin(), s.end());
	}
	word(grf, 0);
	return grf;
}

std::vector<SimtrainsEngine> _simtrains_engines;
bool _simtrains_scanned = false;

} // namespace

/** "simtrains_path" of the configuration: folders of MSTS the player keeps trains in besides the usual ones, separated by ';'. */
std::string _simtrains_path;

namespace {

} // namespace

/** The engines of MSTS in the folders "simtrains" of the NewGRF folders, looked for once (and again by PrepareSimtrainsGrfs()). */
const std::vector<SimtrainsEngine> &FindSimtrainsEngines()
{
	if (_simtrains_scanned) return _simtrains_engines;
	_simtrains_scanned = true;
	_simtrains_engines.clear();
	EngScanner scanner;
	for (const std::string &folder : TrainsetFolders()) {
		Debug(grf, 1, "Simtrains: trains looked for in {}", folder);
		scanner.Scan(".eng", folder, true);
		scanner.Scan(".wag", folder, true);
	}
	std::sort(scanner.files.begin(), scanner.files.end());
	const std::string grf_folder = GrfFolder();
	for (const std::string &file : scanner.files) {
		std::optional<SimtrainsEngine> e = ReadEng(file);
		if (!e.has_value() || grf_folder.empty()) continue;
		e->grf_path = grf_folder + e->eng + ".grf";
		if (std::ranges::any_of(_simtrains_engines, [&e](const SimtrainsEngine &o) { return o.grfid == e->grfid; })) {
			Debug(grf, 0, "Simtrains: {} has the same GRF id as another, left out", file);
			continue;
		}
		Debug(grf, 1, "Simtrains: {} ({}): {}{:.0f} kW, {:.0f} km/h, {:.0f} t, {:.2f} m in {} parts, GRF {:02X}{:02X}{:02X}{:02X}", e->name, e->eng,
				e->wagon ? fmt::format("{} {}, ", e->passengers ? "seats" : "goods", e->capacity) : std::string(), e->power_kw, e->speed_kmh, e->mass_t,
				e->length_m, e->parts, e->grfid & 0xFF, (e->grfid >> 8) & 0xFF, (e->grfid >> 16) & 0xFF, e->grfid >> 24);
		_simtrains_engines.push_back(std::move(*e));
	}
	return _simtrains_engines;
}

/**
 * The engine of MSTS a GRF made of it is (PrepareSimtrainsGrfs()).
 * @param grfid the GRF's id, as the game keeps it
 * @return nullptr none
 */
const SimtrainsEngine *SimtrainsEngineOf(uint32_t grfid)
{
	for (const SimtrainsEngine &e : FindSimtrainsEngines()) {
		if (e.grfid == grfid) return &e;
	}
	return nullptr;
}

/**
 * Before the NewGRFs are looked for: a NewGRF in newgrf/simtrains/ for every vehicle of MSTS, of the figures of its
 * .eng or .wag (written only when it changed, so its checksum stays). The player then has it among the NewGRFs as
 * any other, by the vehicle's name. One made by the game whose vehicle is no longer there goes.
 */
void PrepareSimtrainsGrfs()
{
	_simtrains_scanned = false;
	const std::vector<SimtrainsEngine> &found = FindSimtrainsEngines();
	const std::string folder = GrfFolder();
	if (folder.empty()) return;
	if (!found.empty()) FioCreateDirectory(folder);
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) {
		std::string file = FS2OTTD(entry.path().filename().native());
		if (file.size() < 5 || !StrEqualsIgnoreCase(std::string_view(file).substr(file.size() - 4), ".grf")) continue;
		if (std::ranges::any_of(found, [&](const SimtrainsEngine &e) { return StrEqualsIgnoreCase(e.eng + ".grf", file); })) continue;
		/* Only one the game made: small, with its words. */
		std::vector<uint8_t> old = ReadBytes(folder + file);
		static constexpr std::string_view MARK = "Microsoft Train Simulatoru";
		if (old.empty() || old.size() > 4096 || std::search(old.begin(), old.end(), MARK.begin(), MARK.end()) == old.end()) continue;
		std::filesystem::remove(entry.path(), ec);
		Debug(grf, 1, "Simtrains: {} removed, its vehicle is no longer there", folder + file);
	}
	for (const SimtrainsEngine &e : found) {
		/* Made beside the .eng by the game before: the same set twice. */
		const std::string beside = e.folder + e.eng + ".grf";
		if (beside != e.grf_path) {
			std::vector<uint8_t> old = ReadBytes(beside);
			static constexpr std::string_view MARK = "Microsoft Train Simulatoru";
			if (!old.empty() && old.size() <= 4096 && std::search(old.begin(), old.end(), MARK.begin(), MARK.end()) != old.end()) {
				std::filesystem::remove(OTTD2FS(beside), ec);
				Debug(grf, 1, "Simtrains: {} removed, the GRF is in {} now", beside, folder);
			}
		}
		std::vector<uint8_t> grf = MakeGrf(e);
		const std::string &path = e.grf_path;
		if (ReadBytes(path) == grf) continue;
		auto f = FileHandle::Open(path, "wb");
		if (!f.has_value() || fwrite(grf.data(), 1, grf.size(), *f) != grf.size()) {
			Debug(grf, 0, "Simtrains: {} cannot be written", path);
			continue;
		}
		Debug(grf, 1, "Simtrains: {} written", path);
	}
}

namespace {

/** A model of MSTS as read, binary or text, before it is put together (BuildMesh()). */
struct ShapeData {
	std::vector<std::array<float, 3>> points, normals;
	std::vector<std::array<float, 2>> uvs;
	std::vector<Matrix> matrices;
	std::vector<int32_t> hierarchy;         ///< of the nearest distance: the matrix above each, -1 none
	std::vector<std::string> images;
	std::vector<uint32_t> texture_image;    ///< the image of each texture
	std::vector<int32_t> vtx_matrix;        ///< the matrix of each vertex state
	struct PrimState {
		int32_t texture = -1;
		int32_t vtx_state = 0;
	};
	std::vector<PrimState> prim_states;
	/** A part of the nearest distance: its vertices (point, normal, texture's point) and its lists of triangles by state. */
	struct Sub {
		struct Vertex {
			uint32_t point = 0, normal = 0, uv = 0;
		};
		std::vector<Vertex> vertices;
		std::vector<std::pair<int32_t, std::vector<uint32_t>>> lists;
	};
	std::vector<Sub> subs;
	float distance = 1e30f;
};

/** The binary model ("JINX0s1b"): blocks by their numbers. */
bool ReadBinaryShape(const std::vector<uint8_t> &d, ShapeData &out, const std::string &path)
{
	ShapeReader r(d);
	Block shape;
	if (!r.BlockAt(16, d.size(), shape) || shape.token != TOK_SHAPE) return false;
	Block lods{};
	for (const Block &b : r.Children(shape, false)) {
		switch (b.token) {
			case TOK_POINTS:
				for (const Block &p : r.Children(b, true)) out.points.push_back({r.F32(p.start), r.F32(p.start + 4), r.F32(p.start + 8)});
				break;
			case TOK_NORMALS:
				for (const Block &p : r.Children(b, true)) out.normals.push_back({r.F32(p.start), r.F32(p.start + 4), r.F32(p.start + 8)});
				break;
			case TOK_UV_POINTS:
				for (const Block &p : r.Children(b, true)) out.uvs.push_back({r.F32(p.start), r.F32(p.start + 4)});
				break;
			case TOK_MATRICES:
				for (const Block &p : r.Children(b, true)) {
					Matrix m;
					for (int k = 0; k < 12; k++) m.m[k] = r.F32(p.start + 4 * k);
					out.matrices.push_back(m);
				}
				break;
			case TOK_IMAGES:
				for (const Block &p : r.Children(b, true)) out.images.push_back(r.WString(p.start));
				break;
			case TOK_TEXTURES:
				for (const Block &p : r.Children(b, true)) out.texture_image.push_back(r.U32(p.start));
				break;
			case TOK_VTX_STATES:
				for (const Block &p : r.Children(b, true)) out.vtx_matrix.push_back(r.I32(p.start + 4));
				break;
			case TOK_PRIM_STATES:
				for (const Block &p : r.Children(b, true)) {
					ShapeData::PrimState ps;
					/* flags, shader, then a block of the texture's indices, the bias of the depth, the vertex state. */
					Block idx;
					if (!r.BlockAt(p.start + 8, p.end, idx)) break;
					if (r.U32(idx.start) > 0) ps.texture = r.I32(idx.start + 4);
					ps.vtx_state = r.I32(idx.end + 4);
					out.prim_states.push_back(ps);
				}
				break;
			case TOK_LOD_CONTROLS:
				lods = b;
				break;
			default:
				break;
		}
		if (!r.ok) break;
	}
	if (!r.ok || lods.end == 0 || out.points.empty()) {
		Debug(grf, 0, "Simtrains: {} cannot be read", path);
		return false;
	}

	/* The nearest distance of the first control of distances. */
	Block best{};
	for (const Block &lod : r.Children(lods, true)) {
		for (const Block &levels : r.Children(lod, false)) {
			if (levels.token != TOK_DISTANCE_LEVELS) continue;
			for (const Block &level : r.Children(levels, true)) {
				float distance = 1e29f;
				std::vector<int32_t> h;
				for (const Block &part : r.Children(level, false)) {
					if (part.token != TOK_DISTANCE_LEVEL_HEADER) continue;
					for (const Block &x : r.Children(part, false)) {
						if (x.token == TOK_DLEVEL_SELECTION) distance = r.F32(x.start);
						if (x.token == TOK_HIERARCHY) {
							uint32_t n = std::min<uint32_t>(r.U32(x.start), 4096);
							for (uint32_t k = 0; k < n; k++) h.push_back(r.I32(x.start + 4 + 4 * k));
						}
					}
				}
				if (distance < out.distance) {
					out.distance = distance;
					best = level;
					out.hierarchy = std::move(h);
				}
			}
		}
		break;
	}
	if (best.end == 0) return false;

	for (const Block &part : r.Children(best, false)) {
		if (part.token != TOK_SUB_OBJECTS) continue;
		for (const Block &sub : r.Children(part, true)) {
			ShapeData::Sub so;
			for (const Block &x : r.Children(sub, false)) {
				if (x.token == TOK_VERTICES) {
					for (const Block &v : r.Children(x, true)) {
						ShapeData::Sub::Vertex vx{r.U32(v.start + 4), r.U32(v.start + 8), 0};
						Block uvb;
						if (r.BlockAt(v.start + 20, v.end, uvb) && r.U32(uvb.start) > 0) vx.uv = r.U32(uvb.start + 4);
						so.vertices.push_back(vx);
					}
				}
				if (x.token != TOK_PRIMITIVES) continue;
				int32_t state = 0;
				for (const Block &p : r.Children(x, true)) {
					if (p.token == TOK_PRIM_STATE_IDX) {
						state = r.I32(p.start);
						continue;
					}
					if (p.token != TOK_INDEXED_TRILIST) continue;
					for (const Block &l : r.Children(p, false)) {
						if (l.token != TOK_VERTEX_IDXS) continue;
						const uint32_t n = r.U32(l.start);
						if (l.start + 4 + static_cast<size_t>(n) * 4 > l.end) break;
						std::vector<uint32_t> idx(n);
						for (uint32_t k = 0; k < n; k++) idx[k] = r.U32(l.start + 4 + 4 * k);
						so.lists.emplace_back(state, std::move(idx));
					}
				}
			}
			out.subs.push_back(std::move(so));
		}
	}
	if (!r.ok) Debug(grf, 0, "Simtrains: {} read only in part", path);
	return true;
}

/** A number of the text: a whole one, or nought. */
int64_t TextInt(const std::string &t)
{
	int64_t v = 0;
	auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
	return ec == std::errc() ? v : 0;
}

/** A number of the text with a fraction, or nought. */
float TextFloat(const std::string &t)
{
	float v = 0;
	auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
	return ec == std::errc() && std::isfinite(v) ? v : 0.0f;
}

/**
 * The text model ("JINX0s1t"): the same as the binary one written out as names and brackets, the older models'
 * way. A part may be named before its bracket ("matrix RAM ( ... )"), so the inner names of a list are taken in
 * their order whatever they are called.
 */
bool ReadTextShape(const std::string &text, ShapeData &out, const std::string &path)
{
	std::vector<TextNode> top = ParseText(text);
	const TextNode *shape = nullptr;
	for (const TextNode &n : top) {
		if (StrEqualsIgnoreCase(n.name, "shape")) shape = &n;
	}
	if (shape == nullptr) {
		Debug(grf, 0, "Simtrains: {}: no shape in the text", path);
		return false;
	}
	auto floats = [](const TextNode &n, size_t count, float *into) {
		for (size_t k = 0; k < count; k++) into[k] = TextFloat(n.Value(k));
	};
	const TextNode *lods = nullptr;
	for (const TextNode &b : shape->children) {
		const std::string &name = b.name;
		if (StrEqualsIgnoreCase(name, "points")) {
			for (const TextNode &p : b.children) {
				std::array<float, 3> v{};
				floats(p, 3, v.data());
				out.points.push_back(v);
			}
		} else if (StrEqualsIgnoreCase(name, "normals")) {
			for (const TextNode &p : b.children) {
				std::array<float, 3> v{};
				floats(p, 3, v.data());
				out.normals.push_back(v);
			}
		} else if (StrEqualsIgnoreCase(name, "uv_points")) {
			for (const TextNode &p : b.children) {
				std::array<float, 2> v{};
				floats(p, 2, v.data());
				out.uvs.push_back(v);
			}
		} else if (StrEqualsIgnoreCase(name, "matrices")) {
			for (const TextNode &p : b.children) {
				Matrix m;
				floats(p, 12, m.m);
				out.matrices.push_back(m);
			}
		} else if (StrEqualsIgnoreCase(name, "images")) {
			for (const TextNode &p : b.children) out.images.push_back(p.Value());
		} else if (StrEqualsIgnoreCase(name, "textures")) {
			for (const TextNode &p : b.children) out.texture_image.push_back(static_cast<uint32_t>(TextInt(p.Value())));
		} else if (StrEqualsIgnoreCase(name, "vtx_states")) {
			for (const TextNode &p : b.children) out.vtx_matrix.push_back(static_cast<int32_t>(TextInt(p.Value(1))));
		} else if (StrEqualsIgnoreCase(name, "prim_states")) {
			for (const TextNode &p : b.children) {
				/* flags, shader, the texture's indices (a bracket of their own), the bias of the depth, the vertex state ... */
				ShapeData::PrimState ps;
				if (const TextNode *idx = p.Child("tex_idxs"); idx != nullptr && TextInt(idx->Value(0)) > 0) ps.texture = static_cast<int32_t>(TextInt(idx->Value(1)));
				ps.vtx_state = static_cast<int32_t>(TextInt(p.Value(3)));
				out.prim_states.push_back(ps);
			}
		} else if (StrEqualsIgnoreCase(name, "lod_controls")) {
			lods = &b;
		}
	}
	if (lods == nullptr || out.points.empty()) {
		Debug(grf, 0, "Simtrains: {} cannot be read", path);
		return false;
	}

	/* The nearest distance of the first control of distances. */
	const TextNode *best = nullptr;
	for (const TextNode &lod : lods->children) {
		const TextNode *levels = lod.Child("distance_levels");
		if (levels == nullptr) continue;
		for (const TextNode &level : levels->children) {
			float distance = 1e29f;
			std::vector<int32_t> h;
			if (const TextNode *head = level.Child("distance_level_header"); head != nullptr) {
				if (const TextNode *sel = head->Child("dlevel_selection"); sel != nullptr) distance = TextFloat(sel->Value());
				if (const TextNode *hier = head->Child("hierarchy"); hier != nullptr) {
					const size_t n = std::min<size_t>(static_cast<size_t>(std::max<int64_t>(TextInt(hier->Value(0)), 0)), 4096);
					for (size_t k = 0; k < n; k++) h.push_back(static_cast<int32_t>(TextInt(hier->Value(k + 1))));
				}
			}
			if (distance < out.distance) {
				out.distance = distance;
				best = &level;
				out.hierarchy = std::move(h);
			}
		}
		break;
	}
	if (best == nullptr) return false;

	const TextNode *subs = best->Child("sub_objects");
	if (subs == nullptr) return false;
	for (const TextNode &sub : subs->children) {
		ShapeData::Sub so;
		if (const TextNode *vertices = sub.Child("vertices"); vertices != nullptr) {
			for (const TextNode &v : vertices->children) {
				/* flags, point, normal, two colours, then its texture's points in a bracket. */
				ShapeData::Sub::Vertex vx{static_cast<uint32_t>(TextInt(v.Value(1))), static_cast<uint32_t>(TextInt(v.Value(2))), 0};
				if (const TextNode *uvs = v.Child("vertex_uvs"); uvs != nullptr && TextInt(uvs->Value(0)) > 0) vx.uv = static_cast<uint32_t>(TextInt(uvs->Value(1)));
				so.vertices.push_back(vx);
			}
		}
		if (const TextNode *prims = sub.Child("primitives"); prims != nullptr) {
			int32_t state = 0;
			for (const TextNode &p : prims->children) {
				if (StrEqualsIgnoreCase(p.name, "prim_state_idx")) {
					state = static_cast<int32_t>(TextInt(p.Value()));
					continue;
				}
				if (!StrEqualsIgnoreCase(p.name, "indexed_trilist")) continue;
				const TextNode *l = p.Child("vertex_idxs");
				if (l == nullptr) continue;
				const size_t n = std::min<size_t>(static_cast<size_t>(std::max<int64_t>(TextInt(l->Value(0)), 0)), l->values.empty() ? 0 : l->values.size() - 1);
				std::vector<uint32_t> idx(n);
				for (size_t k = 0; k < n; k++) idx[k] = static_cast<uint32_t>(TextInt(l->Value(k + 1)));
				so.lists.emplace_back(state, std::move(idx));
			}
		}
		out.subs.push_back(std::move(so));
	}
	return true;
}

} // namespace

/**
 * The model of an engine of MSTS (its .s) for drawing, binary or text: the nearest of its distances, every part
 * placed by its matrix and those above it, turned into the frame of the game's vehicle objects; its textures beside it.
 */
bool ReadSimtrainsShape(const std::string &path, SimtrainsMesh &mesh)
{
	std::vector<uint8_t> d = ReadBytes(path);
	if (d.empty() || !Unpack(d)) return false;
	ShapeData sd;
	if (d.size() >= 16 && std::string_view(reinterpret_cast<const char *>(d.data()), 8) == "JINX0s1b") {
		if (!ReadBinaryShape(d, sd, path)) return false;
	} else {
		std::string text = TextOf(d);
		if (text.find("JINX0s1t") == std::string::npos && text.find("shape") == std::string::npos) {
			Debug(grf, 0, "Simtrains: {} is not a model of MSTS", path);
			return false;
		}
		if (!ReadTextShape(text, sd, path)) return false;
	}

	/* Where a point of a matrix's part is in the model: through the matrix and those above it. */
	auto place = [&](int32_t matrix, float &x, float &y, float &z, bool is_point) {
		for (int guard = 0; matrix >= 0 && static_cast<size_t>(matrix) < sd.matrices.size() && guard < 64; guard++) {
			sd.matrices[matrix].Apply(x, y, z, is_point);
			matrix = static_cast<size_t>(matrix) < sd.hierarchy.size() ? sd.hierarchy[matrix] : -1;
		}
	};

	/* The textures: of the images the textures name, each read once; the triangles by the texture they use. */
	std::map<uint32_t, uint8_t> tex_of_image;
	const std::string folder = path.substr(0, path.find_last_of(PATHSEPCHAR) + 1);
	auto texture_slot = [&](int32_t texture) -> uint8_t {
		if (texture < 0 || static_cast<size_t>(texture) >= sd.texture_image.size()) return 0;
		uint32_t image = sd.texture_image[texture];
		auto it = tex_of_image.find(image);
		if (it != tex_of_image.end()) return it->second;
		uint8_t slot = static_cast<uint8_t>(std::min<size_t>(mesh.textures.size(), 254));
		SimtrainsMesh::Texture t;
		t.name = image < sd.images.size() ? sd.images[image] : std::string();
		std::string file = t.name.empty() ? std::string() : FindInFolder(folder, t.name);
		if (file.empty() || !ReadAce(file, t)) Debug(grf, 0, "Simtrains: the texture {} of {} cannot be read", t.name, path);
		mesh.textures.push_back(std::move(t));
		tex_of_image.emplace(image, slot);
		return slot;
	};

	for (const ShapeData::Sub &so : sd.subs) {
		for (const auto &[state, idx] : so.lists) {
			const ShapeData::PrimState ps = state >= 0 && static_cast<size_t>(state) < sd.prim_states.size() ? sd.prim_states[state] : ShapeData::PrimState{};
			const int32_t matrix = ps.vtx_state >= 0 && static_cast<size_t>(ps.vtx_state) < sd.vtx_matrix.size() ? sd.vtx_matrix[ps.vtx_state] : -1;
			const uint8_t slot = texture_slot(ps.texture);
			for (size_t k = 0; k + 2 < idx.size(); k += 3) {
				uint32_t corner[3];
				bool good = true;
				const size_t pos_before = mesh.pos.size(), nrm_before = mesh.nrm.size(), uv_before = mesh.uv.size();
				for (int c = 0; c < 3; c++) {
					const uint32_t vi = idx[k + c];
					if (vi >= so.vertices.size() || so.vertices[vi].point >= sd.points.size()) {
						good = false;
						break;
					}
					const ShapeData::Sub::Vertex &v = so.vertices[vi];
					float px = sd.points[v.point][0], py = sd.points[v.point][1], pz = sd.points[v.point][2];
					place(matrix, px, py, pz, true);
					float nx = 0, ny = 1, nz = 0;
					if (v.normal < sd.normals.size()) {
						nx = sd.normals[v.normal][0];
						ny = sd.normals[v.normal][1];
						nz = sd.normals[v.normal][2];
						place(matrix, nx, ny, nz, false);
					}
					/* MSTS: x to the right, y up, z forward; the game's objects: x forward, y left, z up. */
					corner[c] = static_cast<uint32_t>(mesh.pos.size() / 3);
					mesh.pos.insert(mesh.pos.end(), {pz, -px, py});
					mesh.nrm.insert(mesh.nrm.end(), {nz, -nx, ny});
					if (v.uv < sd.uvs.size()) {
						mesh.uv.insert(mesh.uv.end(), {sd.uvs[v.uv][0], sd.uvs[v.uv][1]});
					} else {
						mesh.uv.insert(mesh.uv.end(), {0.0f, 0.0f});
					}
				}
				if (!good) {
					/* The corners of a triangle already put in, taken out again. */
					mesh.pos.resize(pos_before);
					mesh.nrm.resize(nrm_before);
					mesh.uv.resize(uv_before);
					continue;
				}
				/* Mirrored into the game's frame, so the other way round. */
				mesh.tri.insert(mesh.tri.end(), {corner[0], corner[2], corner[1]});
				mesh.tri_tex.push_back(slot);
			}
		}
	}
	Debug(grf, 1, "Simtrains: {}: {} triangles, {} textures (distance {:.0f} m)", path, mesh.tri.size() / 3, mesh.textures.size(), sd.distance);
	return !mesh.tri.empty();
}
