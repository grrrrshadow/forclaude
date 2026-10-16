/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
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
	if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
		for (size_t i = 2; i + 1 < data.size(); i += 2) {
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

/** The .eng files in the folders "simtrains" of the NewGRF folders (not in tars: the player puts folders there). */
class EngScanner : public FileScanner {
public:
	std::vector<std::string> files;

	bool AddFile(const std::string &filename, size_t, const std::string &tar_filename) override
	{
		if (!tar_filename.empty()) return false;
		std::string lower = filename;
		strtolower(lower);
		if (lower.find(PATHSEP "simtrains" PATHSEP) == std::string::npos) return false;
		this->files.push_back(filename);
		return true;
	}
};

/** An engine of its .eng, if it is an engine with a model; nullopt else. */
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
	if (wagon == nullptr || engine == nullptr) return std::nullopt;

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
	if (const TextNode *inside = wagon->Child("Inside"); inside != nullptr) {
		if (const TextNode *head = inside->Child("PassengerCabinHeadPos"); head != nullptr && head->values.size() >= 3) {
			const float x = static_cast<float>(ParseFigure(head->Value(0), {})), y = static_cast<float>(ParseFigure(head->Value(1), {})), z = static_cast<float>(ParseFigure(head->Value(2), {}));
			/* MSTS: x to the right, y up, z forward. */
			e.cab[0] = z;
			e.cab[1] = -x;
			e.cab[2] = y;
			e.has_cab = true;
		}
	}
	if (e.power_kw <= 0 || e.speed_kmh <= 0 || e.mass_t <= 0) {
		Debug(grf, 0, "Simtrains: {}: no power, speed or mass", path);
		return std::nullopt;
	}
	e.grfid = GrfIdOf(e.eng);
	return e;
}

/* ---- The GRF ---- */

/** The bytes of a NewGRF of one engine (container version 1, GRF version 8: Actions 8, 0 and 4, no sprites). */
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
		text(a8, "Vlak z Microsoft Train Simulatoru ze složky simtrains (" + e.eng + ".eng). Model a textury patří jejich autorům: jen pro tvou hru, nešířit.");
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
	{
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
		a0[2] = props;
		sprites.push_back(std::move(a0));
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

/** The engines of MSTS in the folders "simtrains" of the NewGRF folders, looked for once (and again by PrepareSimtrainsGrfs()). */
const std::vector<SimtrainsEngine> &FindSimtrainsEngines()
{
	if (_simtrains_scanned) return _simtrains_engines;
	_simtrains_scanned = true;
	_simtrains_engines.clear();
	EngScanner scanner;
	scanner.Scan(".eng", Subdirectory::NewGrf, false);
	std::sort(scanner.files.begin(), scanner.files.end());
	for (const std::string &file : scanner.files) {
		std::optional<SimtrainsEngine> e = ReadEng(file);
		if (!e.has_value()) continue;
		if (std::ranges::any_of(_simtrains_engines, [&e](const SimtrainsEngine &o) { return o.grfid == e->grfid; })) {
			Debug(grf, 0, "Simtrains: {} has the same GRF id as another, left out", file);
			continue;
		}
		Debug(grf, 1, "Simtrains: {} ({}): {:.0f} kW, {:.0f} km/h, {:.0f} t, GRF {:02X}{:02X}{:02X}{:02X}", e->name, e->eng, e->power_kw, e->speed_kmh, e->mass_t,
				e->grfid & 0xFF, (e->grfid >> 8) & 0xFF, (e->grfid >> 16) & 0xFF, e->grfid >> 24);
		_simtrains_engines.push_back(std::move(*e));
	}
	return _simtrains_engines;
}

/**
 * Before the NewGRFs are looked for: a NewGRF beside every engine of MSTS in a folder "simtrains", of the figures of
 * its .eng (written only when it changed, so its checksum stays). The player then has it among the NewGRFs as any
 * other, by the engine's name.
 */
void PrepareSimtrainsGrfs()
{
	_simtrains_scanned = false;
	for (const SimtrainsEngine &e : FindSimtrainsEngines()) {
		std::vector<uint8_t> grf = MakeGrf(e);
		const std::string path = e.GrfPath();
		if (ReadBytes(path) == grf) continue;
		auto f = FileHandle::Open(path, "wb");
		if (!f.has_value() || fwrite(grf.data(), 1, grf.size(), *f) != grf.size()) {
			Debug(grf, 0, "Simtrains: {} cannot be written", path);
			continue;
		}
		Debug(grf, 1, "Simtrains: {} written", path);
	}
}

/**
 * The model of an engine of MSTS (its .s) for drawing: the nearest of its distances, every part placed by its matrix
 * and those above it, turned into the frame of the game's vehicle objects; its textures beside it.
 */
bool ReadSimtrainsShape(const std::string &path, SimtrainsMesh &mesh)
{
	std::vector<uint8_t> d = ReadBytes(path);
	if (d.empty() || !Unpack(d)) return false;
	if (d.size() < 16 || std::string_view(reinterpret_cast<const char *>(d.data()), 8) != "JINX0s1b") {
		Debug(grf, 0, "Simtrains: {} is not a binary model (text models are not read yet)", path);
		return false;
	}
	ShapeReader r(d);
	Block shape;
	if (!r.BlockAt(16, d.size(), shape) || shape.token != TOK_SHAPE) return false;

	std::vector<std::array<float, 3>> points, normals;
	std::vector<std::array<float, 2>> uvs;
	std::vector<Matrix> matrices;
	std::vector<std::string> images;
	std::vector<uint32_t> texture_image;
	std::vector<int32_t> vtx_matrix;
	struct PrimState { int32_t texture = -1; int32_t vtx_state = 0; };
	std::vector<PrimState> prim_states;
	Block lods{};
	for (const Block &b : r.Children(shape, false)) {
		switch (b.token) {
			case TOK_POINTS:
				for (const Block &p : r.Children(b, true)) points.push_back({r.F32(p.start), r.F32(p.start + 4), r.F32(p.start + 8)});
				break;
			case TOK_NORMALS:
				for (const Block &p : r.Children(b, true)) normals.push_back({r.F32(p.start), r.F32(p.start + 4), r.F32(p.start + 8)});
				break;
			case TOK_UV_POINTS:
				for (const Block &p : r.Children(b, true)) uvs.push_back({r.F32(p.start), r.F32(p.start + 4)});
				break;
			case TOK_MATRICES:
				for (const Block &p : r.Children(b, true)) {
					Matrix m;
					for (int k = 0; k < 12; k++) m.m[k] = r.F32(p.start + 4 * k);
					matrices.push_back(m);
				}
				break;
			case TOK_IMAGES:
				for (const Block &p : r.Children(b, true)) images.push_back(r.WString(p.start));
				break;
			case TOK_TEXTURES:
				for (const Block &p : r.Children(b, true)) texture_image.push_back(r.U32(p.start));
				break;
			case TOK_VTX_STATES:
				for (const Block &p : r.Children(b, true)) vtx_matrix.push_back(r.I32(p.start + 4));
				break;
			case TOK_PRIM_STATES:
				for (const Block &p : r.Children(b, true)) {
					PrimState ps;
					/* flags, shader, then a block of the texture's indices, the bias of the depth, the vertex state. */
					Block idx;
					if (!r.BlockAt(p.start + 8, p.end, idx)) break;
					if (r.U32(idx.start) > 0) ps.texture = r.I32(idx.start + 4);
					ps.vtx_state = r.I32(idx.end + 4);
					prim_states.push_back(ps);
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
	if (!r.ok || lods.end == 0 || points.empty()) {
		Debug(grf, 0, "Simtrains: {} cannot be read", path);
		return false;
	}

	/* The nearest distance of the first control of distances. */
	Block best{};
	float best_distance = 1e30f;
	std::vector<int32_t> hierarchy;
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
				if (distance < best_distance) {
					best_distance = distance;
					best = level;
					hierarchy = std::move(h);
				}
			}
		}
		break;
	}
	if (best.end == 0) return false;

	/* Where a point of a matrix's part is in the model: through the matrix and those above it. */
	auto place = [&](int32_t matrix, float &x, float &y, float &z, bool is_point) {
		for (int guard = 0; matrix >= 0 && static_cast<size_t>(matrix) < matrices.size() && guard < 64; guard++) {
			matrices[matrix].Apply(x, y, z, is_point);
			matrix = static_cast<size_t>(matrix) < hierarchy.size() ? hierarchy[matrix] : -1;
		}
	};

	/* The textures: of the images the textures name, each read once; the triangles by the texture they use. */
	std::map<uint32_t, uint8_t> tex_of_image;
	const std::string folder = path.substr(0, path.find_last_of(PATHSEPCHAR) + 1);
	auto texture_slot = [&](int32_t texture) -> uint8_t {
		if (texture < 0 || static_cast<size_t>(texture) >= texture_image.size()) return 0;
		uint32_t image = texture_image[texture];
		auto it = tex_of_image.find(image);
		if (it != tex_of_image.end()) return it->second;
		uint8_t slot = static_cast<uint8_t>(std::min<size_t>(mesh.textures.size(), 254));
		SimtrainsMesh::Texture t;
		t.name = image < images.size() ? images[image] : std::string();
		std::string file = t.name.empty() ? std::string() : FindInFolder(folder, t.name);
		if (file.empty() || !ReadAce(file, t)) Debug(grf, 0, "Simtrains: the texture {} of {} cannot be read", t.name, path);
		mesh.textures.push_back(std::move(t));
		tex_of_image.emplace(image, slot);
		return slot;
	};

	for (const Block &part : r.Children(best, false)) {
		if (part.token != TOK_SUB_OBJECTS) continue;
		for (const Block &sub : r.Children(part, true)) {
			struct Vertex { uint32_t point, normal, uv; };
			std::vector<Vertex> vertices;
			Block prims{};
			for (const Block &x : r.Children(sub, false)) {
				if (x.token == TOK_VERTICES) {
					for (const Block &v : r.Children(x, true)) {
						Vertex vx{r.U32(v.start + 4), r.U32(v.start + 8), 0};
						Block uvb;
						if (r.BlockAt(v.start + 20, v.end, uvb) && r.U32(uvb.start) > 0) vx.uv = r.U32(uvb.start + 4);
						vertices.push_back(vx);
					}
				}
				if (x.token == TOK_PRIMITIVES) prims = x;
			}
			if (prims.end == 0) continue;
			int32_t state = 0;
			for (const Block &p : r.Children(prims, true)) {
				if (p.token == TOK_PRIM_STATE_IDX) {
					state = r.I32(p.start);
					continue;
				}
				if (p.token != TOK_INDEXED_TRILIST) continue;
				const PrimState ps = state >= 0 && static_cast<size_t>(state) < prim_states.size() ? prim_states[state] : PrimState{};
				const int32_t matrix = ps.vtx_state >= 0 && static_cast<size_t>(ps.vtx_state) < vtx_matrix.size() ? vtx_matrix[ps.vtx_state] : -1;
				const uint8_t slot = texture_slot(ps.texture);
				for (const Block &x : r.Children(p, false)) {
					if (x.token != TOK_VERTEX_IDXS) continue;
					const uint32_t n = r.U32(x.start);
					if (x.start + 4 + static_cast<size_t>(n) * 4 > x.end) break;
					for (uint32_t k = 0; k + 2 < n; k += 3) {
						uint32_t corner[3];
						bool good = true;
						for (int c = 0; c < 3; c++) {
							const uint32_t vi = r.U32(x.start + 4 + 4 * (k + c));
							if (vi >= vertices.size() || vertices[vi].point >= points.size()) {
								good = false;
								break;
							}
							const Vertex &v = vertices[vi];
							float px = points[v.point][0], py = points[v.point][1], pz = points[v.point][2];
							place(matrix, px, py, pz, true);
							float nx = 0, ny = 1, nz = 0;
							if (v.normal < normals.size()) {
								nx = normals[v.normal][0];
								ny = normals[v.normal][1];
								nz = normals[v.normal][2];
								place(matrix, nx, ny, nz, false);
							}
							/* MSTS: x to the right, y up, z forward; the game's objects: x forward, y left, z up. */
							corner[c] = static_cast<uint32_t>(mesh.pos.size() / 3);
							mesh.pos.insert(mesh.pos.end(), {pz, -px, py});
							mesh.nrm.insert(mesh.nrm.end(), {nz, -nx, ny});
							if (v.uv < uvs.size()) {
								mesh.uv.insert(mesh.uv.end(), {uvs[v.uv][0], uvs[v.uv][1]});
							} else {
								mesh.uv.insert(mesh.uv.end(), {0.0f, 0.0f});
							}
						}
						if (!good) continue;
						/* Mirrored into the game's frame, so the other way round. */
						mesh.tri.insert(mesh.tri.end(), {corner[0], corner[2], corner[1]});
						mesh.tri_tex.push_back(slot);
					}
				}
			}
		}
	}
	if (!r.ok) Debug(grf, 0, "Simtrains: {} read only in part", path);
	Debug(grf, 1, "Simtrains: {}: {} triangles, {} textures (distance {:.0f} m)", path, mesh.tri.size() / 3, mesh.textures.size(), best_distance);
	return !mesh.tri.empty();
}
