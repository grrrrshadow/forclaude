/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file beamng.cpp Cars of mods of BeamNG.drive the player has (beamng.h).
 *
 * What is read of a mod, by the game's own reading:
 *  - the mod is a .zip (stored or deflated entries) or a folder; a car is its folder vehicles/<name>/;
 *  - the .jbeam files are JSON written loosely (comments, commas left out or left over): parts by their names, the
 *    "main" one and those its slots name by default (or its .pc names); of them the nodes (their places and
 *    weights), the wheels, the engine's torque curve, the gearbox, the final drive, the cameras, the flexbodies (the
 *    meshes of the model and where they are put), the lights ("props") and the glass that glows ("glowMap");
 *  - the model, a .dae (COLLADA, XML): the meshes the flexbodies name, through their nodes' matrices;
 *  - the materials (materials.cs, *.materials.json): a colour, a texture, the paint of the car ("instanceDiffuse")
 *    and the mask that cuts holes in the body (an "opacityMap" of the second texture coordinates);
 *  - the textures, .dds (BC1, BC2, BC3, BC4 or plain) or .png;
 *  - info.json: the name, the brand, the author, the colours of the paint.
 */

#include "stdafx.h"
#include "beamng.h"
#include "fileio_func.h"
#include "debug.h"
#include "string_func.h"
#include "core/math_func.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#if defined(WITH_ZLIB)
#	include <zlib.h>
#endif

#include "safeguards.h"

namespace {

/* ---- Files: whole, or the entries of a .zip ---- */

/** A whole file, or nothing. */
std::vector<uint8_t> ReadBytes(const std::string &path)
{
	std::vector<uint8_t> data;
	auto f = FileHandle::Open(path, "rb");
	if (!f.has_value()) return data;
	fseek(*f, 0, SEEK_END);
	long len = ftell(*f);
	fseek(*f, 0, SEEK_SET);
	if (len <= 0 || len > (512L << 20)) return data;
	data.resize(static_cast<size_t>(len));
	if (fread(data.data(), 1, data.size(), *f) != data.size()) data.clear();
	return data;
}

uint16_t Le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

/** An entry of a .zip: its path ('/' between the folders), how it is packed, where its local header is. */
struct ZipEntry {
	std::string name;
	uint16_t method = 0;
	uint32_t packed = 0, size = 0, header = 0;
};

/** A .zip's central directory. */
struct Zip {
	std::string path;
	std::vector<ZipEntry> entries;

	const ZipEntry *Find(std::string_view name) const
	{
		for (const ZipEntry &e : this->entries) {
			if (StrEqualsIgnoreCase(e.name, name)) return &e;
		}
		return nullptr;
	}
};

/** The central directory of a .zip: the record of its end found in its last bytes, then its entries (no zip64). */
bool ReadZipDirectory(const std::string &path, Zip &zip)
{
	auto f = FileHandle::Open(path, "rb");
	if (!f.has_value()) return false;
	fseek(*f, 0, SEEK_END);
	const long len = ftell(*f);
	if (len < 22) return false;
	const long tail = std::min<long>(len, 22 + 65535);
	std::vector<uint8_t> end(static_cast<size_t>(tail));
	fseek(*f, len - tail, SEEK_SET);
	if (fread(end.data(), 1, end.size(), *f) != end.size()) return false;
	long at = -1;
	for (long i = tail - 22; i >= 0; i--) {
		if (Le32(&end[i]) == 0x06054b50) {
			at = i;
			break;
		}
	}
	if (at < 0) return false;
	const uint32_t count = Le16(&end[at + 10]), size = Le32(&end[at + 12]), offset = Le32(&end[at + 16]);
	if (offset == 0xFFFFFFFF || static_cast<uint64_t>(offset) + size > static_cast<uint64_t>(len)) return false;
	std::vector<uint8_t> dir(size);
	fseek(*f, static_cast<long>(offset), SEEK_SET);
	if (fread(dir.data(), 1, dir.size(), *f) != dir.size()) return false;
	zip.path = path;
	size_t p = 0;
	for (uint32_t i = 0; i < count && p + 46 <= dir.size(); i++) {
		if (Le32(&dir[p]) != 0x02014b50) break;
		ZipEntry e;
		e.method = Le16(&dir[p + 10]);
		e.packed = Le32(&dir[p + 20]);
		e.size = Le32(&dir[p + 24]);
		const uint16_t name_len = Le16(&dir[p + 28]), extra_len = Le16(&dir[p + 30]), comment_len = Le16(&dir[p + 32]);
		e.header = Le32(&dir[p + 42]);
		if (p + 46 + name_len > dir.size()) break;
		e.name.assign(reinterpret_cast<const char *>(&dir[p + 46]), name_len);
		std::replace(e.name.begin(), e.name.end(), '\\', '/');
		zip.entries.push_back(std::move(e));
		p += 46 + name_len + extra_len + comment_len;
	}
	return true;
}

/** An entry's bytes, unpacked: stored, or deflated (by zlib). */
std::vector<uint8_t> ReadZipEntry(const Zip &zip, const ZipEntry &e)
{
	std::vector<uint8_t> out;
	if (e.size > (512u << 20) || e.packed > (512u << 20)) return out;
	auto f = FileHandle::Open(zip.path, "rb");
	if (!f.has_value()) return out;
	uint8_t local[30];
	if (fseek(*f, static_cast<long>(e.header), SEEK_SET) != 0 || fread(local, 1, sizeof(local), *f) != sizeof(local) || Le32(local) != 0x04034b50) return out;
	fseek(*f, static_cast<long>(e.header + 30 + Le16(local + 26) + Le16(local + 28)), SEEK_SET);
	std::vector<uint8_t> packed(e.packed);
	if (fread(packed.data(), 1, packed.size(), *f) != packed.size()) return out;
	if (e.method == 0) return packed;
	if (e.method != 8) return out;
#if defined(WITH_ZLIB)
	out.resize(e.size);
	z_stream z{};
	if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return {};
	z.next_in = packed.data();
	z.avail_in = static_cast<uInt>(packed.size());
	z.next_out = out.data();
	z.avail_out = static_cast<uInt>(out.size());
	const int r = inflate(&z, Z_FINISH);
	inflateEnd(&z);
	if (r != Z_STREAM_END) out.clear();
#endif
	return out;
}

/** The directories of the .zip files looked at, by their paths. */
std::map<std::string, Zip> _zips;

const Zip *OpenZip(const std::string &path)
{
	auto it = _zips.find(path);
	if (it != _zips.end()) return &it->second;
	Zip zip;
	if (!ReadZipDirectory(path, zip)) return nullptr;
	return &_zips.emplace(path, std::move(zip)).first->second;
}

/** The file of a folder by its name whatever the case of its letters (the mods come from Windows). */
std::string FindInFolder(const std::string &folder, std::string_view name)
{
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) {
		std::string file = FS2OTTD(entry.path().filename().native());
		if (StrEqualsIgnoreCase(file, name)) return folder + file;
	}
	return {};
}

/**
 * A file of the car: by its name in the car's folder, or by its path from the mod's root ("vehicles/..."), whatever
 * the case of its letters; empty when it is not in the mod (a texture of BeamNG's own, "vehicles/common/...").
 */
std::vector<uint8_t> ReadModFile(const BeamngVehicle &v, std::string name)
{
	std::replace(name.begin(), name.end(), '\\', '/');
	while (!name.empty() && name[0] == '/') name.erase(0, 1);
	const bool from_root = name.size() > 9 && StrEqualsIgnoreCase(std::string_view(name).substr(0, 9), "vehicles/");
	if (!v.zip.empty()) {
		const Zip *zip = OpenZip(v.zip);
		if (zip == nullptr) return {};
		const ZipEntry *e = zip->Find(from_root ? name : v.folder + name);
		return e != nullptr ? ReadZipEntry(*zip, *e) : std::vector<uint8_t>();
	}
	std::string folder = v.folder;
	if (from_root) {
		/* The mod's root: the folder above vehicles/<name>/. */
		for (int up = 0; up < 2 && folder.size() > 1; up++) {
			folder.pop_back();
			folder.erase(folder.find_last_of(PATHSEPCHAR) + 1);
		}
		size_t slash;
		while ((slash = name.find('/')) != std::string::npos) {
			std::string sub = FindInFolder(folder, name.substr(0, slash));
			if (sub.empty()) return {};
			folder = sub + PATHSEP;
			name.erase(0, slash + 1);
		}
	}
	std::string path = FindInFolder(folder, name);
	return path.empty() ? std::vector<uint8_t>() : ReadBytes(path);
}

/** The names of the files directly in the car's folder. */
std::vector<std::string> ListCarFiles(const BeamngVehicle &v)
{
	std::vector<std::string> files;
	if (!v.zip.empty()) {
		const Zip *zip = OpenZip(v.zip);
		if (zip == nullptr) return files;
		for (const ZipEntry &e : zip->entries) {
			if (e.name.size() <= v.folder.size() || !StrEqualsIgnoreCase(std::string_view(e.name).substr(0, v.folder.size()), v.folder)) continue;
			std::string rest = e.name.substr(v.folder.size());
			if (rest.find('/') == std::string::npos) files.push_back(std::move(rest));
		}
		return files;
	}
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(v.folder), ec)) {
		if (entry.is_regular_file(ec)) files.push_back(FS2OTTD(entry.path().filename().native()));
	}
	return files;
}

bool EndsWithIgnoreCase(std::string_view s, std::string_view end)
{
	return s.size() >= end.size() && StrEqualsIgnoreCase(s.substr(s.size() - end.size()), end);
}

/* ---- JSON as the .jbeam files write it ---- */

/** A value of a .jbeam (or of a .json): null, true or false, a number, a text, a list or an object. */
struct JValue {
	enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
	Type type = Type::Null;
	double number = 0;
	std::string text;
	std::vector<JValue> items;                           ///< a list's
	std::vector<std::pair<std::string, JValue>> members; ///< an object's, in their order

	const JValue *Get(std::string_view key) const
	{
		if (this->type != Type::Object) return nullptr;
		for (const auto &[k, v] : this->members) {
			if (k == key) return &v;
		}
		return nullptr;
	}
	double Num(double def = 0) const { return this->type == Type::Number ? this->number : (this->type == Type::Bool ? (this->number != 0 ? 1 : 0) : def); }
	bool IsArray() const { return this->type == Type::Array; }
	bool IsObject() const { return this->type == Type::Object; }
	bool IsString() const { return this->type == Type::String; }
	bool IsTrue() const { return (this->type == Type::Bool && this->number != 0) || (this->type == Type::String && (this->text == "true" || this->text == "1")) || (this->type == Type::Number && this->number != 0); }
};

/**
 * JSON as BeamNG writes it: comments (of two slashes, and between slash-star and star-slash), the commas between the items left out or left over, the
 * words of no quotes (FLT_MAX). What cannot be made out is passed over.
 */
class JParser {
	std::string_view s;
	size_t p = 0;

	void Skip()
	{
		while (this->p < this->s.size()) {
			const char c = this->s[this->p];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
				this->p++;
			} else if (c == '/' && this->p + 1 < this->s.size() && this->s[this->p + 1] == '/') {
				while (this->p < this->s.size() && this->s[this->p] != '\n') this->p++;
			} else if (c == '/' && this->p + 1 < this->s.size() && this->s[this->p + 1] == '*') {
				size_t e = this->s.find("*/", this->p + 2);
				this->p = e == std::string_view::npos ? this->s.size() : e + 2;
			} else {
				break;
			}
		}
	}

	std::string Text()
	{
		std::string out;
		this->p++; // the quote
		while (this->p < this->s.size() && this->s[this->p] != '"') {
			char c = this->s[this->p++];
			if (c == '\\' && this->p < this->s.size()) {
				c = this->s[this->p++];
				if (c == 'n') c = '\n';
				else if (c == 't') c = '\t';
				else if (c == 'u') {
					this->p = std::min(this->s.size(), this->p + 4);
					c = '?';
				}
			}
			out += c;
		}
		this->p++; // the quote
		return out;
	}

	std::string Word()
	{
		size_t b = this->p;
		while (this->p < this->s.size()) {
			const char c = this->s[this->p];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ':' || c == '{' || c == '}' || c == '[' || c == ']' || c == '"') break;
			this->p++;
		}
		return std::string(this->s.substr(b, this->p - b));
	}

public:
	explicit JParser(std::string_view text) : s(text) {}

	JValue Value(int depth = 0)
	{
		JValue v;
		this->Skip();
		if (this->p >= this->s.size() || depth > 200) return v;
		const char c = this->s[this->p];
		if (c == '{' || c == '[') {
			const bool object = c == '{';
			v.type = object ? JValue::Type::Object : JValue::Type::Array;
			this->p++;
			for (;;) {
				this->Skip();
				if (this->p >= this->s.size()) break;
				const char d = this->s[this->p];
				if (d == '}' || d == ']') {
					this->p++;
					break;
				}
				if (object) {
					std::string key = d == '"' ? this->Text() : this->Word();
					this->Skip();
					if (this->p < this->s.size() && this->s[this->p] == ':') this->p++;
					v.members.emplace_back(std::move(key), this->Value(depth + 1));
				} else {
					const size_t before = this->p;
					v.items.push_back(this->Value(depth + 1));
					if (this->p == before) this->p++; // a character that is no value
				}
			}
		} else if (c == '"') {
			v.type = JValue::Type::String;
			v.text = this->Text();
		} else if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
			std::string w = this->Word();
			const char *b = w.data() + (w[0] == '+' ? 1 : 0);
			auto [end, ec] = std::from_chars(b, w.data() + w.size(), v.number);
			v.type = ec == std::errc() ? JValue::Type::Number : JValue::Type::String;
			if (v.type == JValue::Type::String) v.text = w;
		} else {
			std::string w = this->Word();
			if (w.empty()) {
				this->p++;
			} else if (w == "true" || w == "false") {
				v.type = JValue::Type::Bool;
				v.number = w == "true" ? 1 : 0;
			} else if (w != "null") {
				v.type = JValue::Type::String;
				v.text = std::move(w);
			}
		}
		return v;
	}
};

JValue ParseJson(const std::vector<uint8_t> &bytes)
{
	std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF) text.remove_prefix(3); // the mark of UTF-8
	return JParser(text).Value();
}

/** Every value of the key in the tree, depth first (a gearbox, a differential: wherever a part keeps it). */
void FindAll(const JValue &v, std::string_view key, std::vector<const JValue *> &out, int depth = 0)
{
	if (depth > 40) return;
	if (v.IsObject()) {
		for (const auto &[k, m] : v.members) {
			if (k == key) out.push_back(&m);
			FindAll(m, key, out, depth + 1);
		}
	} else if (v.IsArray()) {
		for (const JValue &i : v.items) FindAll(i, key, out, depth + 1);
	}
}

/* ---- The car: its parts and figures ---- */

/** The parts of a car's .jbeam files, the main one and those its slots name, in that order. */
struct CarParts {
	std::vector<JValue> files;
	std::vector<const JValue *> used;
	std::map<std::string, std::array<double, 3>> nodes; ///< the places of the nodes of the parts used, metres in the axes of BeamNG
};

/** The slots of a part and the part each takes by default: "slots" (type, default) or "slots2" (name, ..., default). */
std::vector<std::pair<std::string, std::string>> SlotsOf(const JValue &part)
{
	std::vector<std::pair<std::string, std::string>> out;
	for (const char *key : {"slots", "slots2"}) {
		const JValue *slots = part.Get(key);
		if (slots == nullptr || !slots->IsArray()) continue;
		const size_t at_default = std::string_view(key) == "slots2" ? 3 : 1;
		for (const JValue &row : slots->items) {
			if (!row.IsArray() || row.items.size() <= at_default || !row.items[0].IsString()) continue;
			if (row.items[0].text == "type" || row.items[0].text == "name") continue; // the heading
			out.emplace_back(row.items[0].text, row.items[at_default].IsString() ? row.items[at_default].text : std::string());
		}
	}
	return out;
}

/** Read the car's .jbeam files and pick its parts: the main one, then what each slot takes (by the .pc, else by default). */
bool ReadCarParts(const BeamngVehicle &v, const JValue *info, CarParts &parts)
{
	std::vector<std::string> files = ListCarFiles(v);
	std::sort(files.begin(), files.end());
	std::map<std::string, std::string> chosen;
	/* The configuration: info.json's "default_pc", else the only .pc. */
	std::string pc;
	if (const JValue *d = info != nullptr ? info->Get("default_pc") : nullptr; d != nullptr && d->IsString()) pc = d->text + ".pc";
	if (pc.empty()) {
		for (const std::string &f : files) {
			if (EndsWithIgnoreCase(f, ".pc")) {
				pc = f;
				break;
			}
		}
	}
	if (!pc.empty()) {
		JValue cfg = ParseJson(ReadModFile(v, pc));
		if (const JValue *p = cfg.Get("parts"); p != nullptr && p->IsObject()) {
			for (const auto &[slot, part] : p->members) {
				if (part.IsString()) chosen[slot] = part.text;
			}
		}
	}
	for (const std::string &f : files) {
		if (EndsWithIgnoreCase(f, ".jbeam")) parts.files.push_back(ParseJson(ReadModFile(v, f)));
	}
	std::map<std::string, const JValue *> by_name;
	const JValue *main = nullptr;
	for (const JValue &file : parts.files) {
		for (const auto &[name, part] : file.members) {
			if (!part.IsObject()) continue;
			by_name.emplace(name, &part);
			const JValue *type = part.Get("slotType");
			if (main == nullptr && type != nullptr && type->IsString() && type->text == "main") main = &part;
		}
	}
	if (main == nullptr) return false;
	std::set<const JValue *> seen;
	std::vector<const JValue *> todo = {main};
	while (!todo.empty() && parts.used.size() < 400) {
		const JValue *part = todo.front();
		todo.erase(todo.begin());
		if (!seen.insert(part).second) continue;
		parts.used.push_back(part);
		for (const auto &[slot, def] : SlotsOf(*part)) {
			auto c = chosen.find(slot);
			const std::string &name = c != chosen.end() ? c->second : def;
			if (name.empty()) continue;
			auto it = by_name.find(name);
			if (it != by_name.end()) todo.push_back(it->second);
		}
	}
	/* The nodes: a row [id, x, y, z, {...}], weighed by the "nodeWeight" set before it or in it. */
	for (const JValue *part : parts.used) {
		const JValue *nodes = part->Get("nodes");
		if (nodes == nullptr || !nodes->IsArray()) continue;
		for (const JValue &row : nodes->items) {
			if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString() || row.items[1].type != JValue::Type::Number) continue;
			parts.nodes[row.items[0].text] = {row.items[1].Num(), row.items[2].Num(), row.items[3].Num()};
		}
	}
	return true;
}

/** A colour of BeamNG, "r g b a" or [r, g, b, a] from 0 to 1, as Colour::data. */
uint32_t ColourOf(const JValue &v)
{
	double c[3] = {0.5, 0.5, 0.5};
	if (v.IsString()) {
		std::string_view rest = v.text;
		for (int i = 0; i < 3; i++) {
			while (!rest.empty() && rest[0] == ' ') rest.remove_prefix(1);
			auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), c[i]);
			if (ec != std::errc()) break;
			rest.remove_prefix(end - rest.data());
		}
	} else if (v.IsArray()) {
		for (size_t i = 0; i < 3 && i < v.items.size(); i++) c[i] = v.items[i].Num(0.5);
	}
	auto byte = [](double x) { return static_cast<uint32_t>(Clamp<int>(static_cast<int>(std::lround(x * 255)), 0, 255)); };
	return 0xFF000000u | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
}

/** The id of the GRF made of a car: "BN" and a number of its mod's and folder's names, the same on every computer. */
uint32_t GrfIdOf(const std::string &key)
{
	uint32_t h = 2166136261u;
	for (char c : key) {
		h ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
		h *= 16777619u;
	}
	return 'B' | 'N' << 8 | ((h ^ (h >> 16)) & 0xFFFF) << 16;
}

/** The middle of the car along it and its ground, in the axes of BeamNG: y of the middle of its nodes, z of the bottoms of its wheels. */
struct CarFrame {
	double mid_y = 0, ground_z = 0;
};
std::map<std::string, CarFrame> _frames; ///< by BeamngVehicle::Package() and folder

/** A place of BeamNG (x to the car's left, y backwards, z up) in the frame of the game's vehicle objects (x forward, y left, z up). */
std::array<float, 3> ToObject(const CarFrame &f, double x, double y, double z)
{
	return {static_cast<float>(-(y - f.mid_y)), static_cast<float>(x), static_cast<float>(z - f.ground_z)};
}

/** A car of its folder in the mod, if it is one with a model and an engine. */
std::optional<BeamngVehicle> ReadCar(const std::string &zip, const std::string &folder, const std::string &id)
{
	BeamngVehicle v;
	v.zip = zip;
	v.folder = folder;
	v.id = id;
	JValue info = ParseJson(ReadModFile(v, "info.json"));
	CarParts parts;
	if (!ReadCarParts(v, info.IsObject() ? &info : nullptr, parts)) return std::nullopt;
	bool model = false;
	for (const std::string &f : ListCarFiles(v)) {
		if (EndsWithIgnoreCase(f, ".dae")) model = true;
	}
	if (!model) {
		Debug(grf, 1, "BeamNG: {} {}: no model of its own (a configuration of a car of BeamNG?), left out", v.Package(), id);
		return std::nullopt;
	}

	/* The names. */
	std::string brand, name;
	if (const JValue *b = info.Get("Brand"); b != nullptr && b->IsString()) brand = b->text;
	if (const JValue *n = info.Get("Name"); n != nullptr && n->IsString()) name = n->text;
	if (name.empty()) name = id;
	v.name = !brand.empty() && name.find(brand) == std::string::npos ? brand + " " + name : name;
	if (const JValue *a = info.Get("Author"); a != nullptr && a->IsString()) v.author = a->text;
	const JValue *type = info.Get("Type");
	if (type != nullptr && type->IsString() && (StrEqualsIgnoreCase(type->text, "Prop") || StrEqualsIgnoreCase(type->text, "Trailer"))) return std::nullopt;

	/* The paints: "colors" (names to "r g b a") with "default_color", or "paints" (names to {baseColor}) with "defaultPaintName1". */
	std::string def;
	if (const JValue *d = info.Get("default_color"); d != nullptr && d->IsString()) def = d->text;
	if (const JValue *d = info.Get("defaultPaintName1"); d != nullptr && d->IsString()) def = d->text;
	auto add_paint = [&](const std::string &n, uint32_t colour) {
		if (n == def) {
			v.paints.insert(v.paints.begin(), colour);
		} else {
			v.paints.push_back(colour);
		}
	};
	if (const JValue *c = info.Get("colors"); c != nullptr && c->IsObject()) {
		for (const auto &[n, col] : c->members) add_paint(n, ColourOf(col));
	}
	if (const JValue *c = info.Get("paints"); c != nullptr && c->IsObject()) {
		for (const auto &[n, paint] : c->members) {
			if (const JValue *base = paint.Get("baseColor"); base != nullptr) add_paint(n, ColourOf(*base));
		}
	}
	if (v.paints.empty()) v.paints.push_back(0xFF8C8C8C);

	/* Its weight: the nodes', each by the "nodeWeight" before it or its own (25 kg as BeamNG has it otherwise), and the wheels' nodes. */
	double mass = 0, radius = 0;
	double min_y = 1e9, max_y = -1e9, min_x = 1e9, max_x = -1e9, min_z = 1e9, max_z = -1e9;
	std::vector<std::pair<std::string, std::string>> hubs;
	for (const JValue *part : parts.used) {
		if (const JValue *nodes = part->Get("nodes"); nodes != nullptr && nodes->IsArray()) {
			double weight = 25;
			for (const JValue &row : nodes->items) {
				if (row.IsObject()) {
					if (const JValue *w = row.Get("nodeWeight"); w != nullptr && w->type == JValue::Type::Number) weight = w->number;
					continue;
				}
				if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString() || row.items[1].type != JValue::Type::Number) continue;
				double w = weight;
				for (size_t i = 4; i < row.items.size(); i++) {
					if (const JValue *own = row.items[i].Get("nodeWeight"); own != nullptr && own->type == JValue::Type::Number) w = own->number;
				}
				mass += w;
				const double x = row.items[1].Num(), y = row.items[2].Num(), z = row.items[3].Num();
				min_x = std::min(min_x, x); max_x = std::max(max_x, x);
				min_y = std::min(min_y, y); max_y = std::max(max_y, y);
				min_z = std::min(min_z, z); max_z = std::max(max_z, z);
			}
		}
		for (const char *kind : {"pressureWheels", "hubWheels", "hubWheelsTSV", "pressureWheelsTSV"}) {
			const JValue *wheels = part->Get(kind);
			if (wheels == nullptr || !wheels->IsArray()) continue;
			double node_weight = 0.5, hub_weight = 0.5, rays = 16, r = 0.3;
			bool tire = true;
			for (const JValue &row : wheels->items) {
				if (row.IsObject()) {
					for (const auto &[k, val] : row.members) {
						if (k == "nodeWeight") node_weight = val.Num(node_weight);
						if (k == "hubNodeWeight") hub_weight = val.Num(hub_weight);
						if (k == "numRays") rays = val.Num(rays);
						if (k == "radius") r = val.Num(r);
						if (k == "hasTire") tire = val.IsTrue();
					}
					continue;
				}
				if (!row.IsArray() || row.items.size() < 5 || !row.items[0].IsString() || row.items[0].text == "name") continue;
				mass += rays * 2 * hub_weight + (tire ? rays * 2 * node_weight : 0);
				radius = std::max(radius, r);
				if (row.items[3].IsString() && row.items[4].IsString()) hubs.emplace_back(row.items[3].text, row.items[4].text);
			}
		}
	}
	if (radius <= 0) radius = 0.32;
	v.mass_t = std::max(0.3, mass / 1000);

	/* Its frame: the middle of its nodes along it, the ground under its wheels' hubs. */
	CarFrame frame;
	frame.mid_y = min_y < max_y ? (min_y + max_y) / 2 : 0;
	double hub_z = 1e9;
	for (const auto &[a, b] : hubs) {
		for (const std::string &n : {a, b}) {
			auto it = parts.nodes.find(n);
			if (it != parts.nodes.end()) hub_z = std::min(hub_z, it->second[2]);
		}
	}
	frame.ground_z = hub_z < 1e8 ? hub_z - radius : (min_z < 1e8 ? min_z : 0);
	_frames[v.Package() + "|" + v.folder] = frame;

	/* The engine: the most power of its torque curve ([rpm, Nm] rows), its revolutions at most. */
	double power = 0, max_torque = 0, max_rpm = 0;
	std::vector<const JValue *> found;
	for (const JValue *part : parts.used) FindAll(*part, "torque", found);
	for (const JValue *t : found) {
		if (!t->IsArray()) continue;
		for (const JValue &row : t->items) {
			if (!row.IsArray() || row.items.size() < 2 || row.items[0].type != JValue::Type::Number) continue;
			const double rpm = row.items[0].Num(), nm = row.items[1].Num();
			power = std::max(power, rpm * nm * 2 * M_PI / 60 / 1000);
			max_torque = std::max(max_torque, nm);
			max_rpm = std::max(max_rpm, rpm);
		}
	}
	found.clear();
	for (const JValue *part : parts.used) FindAll(*part, "maxRPM", found);
	for (const JValue *m : found) max_rpm = std::max(max_rpm, m->Num());
	if (power <= 0) {
		Debug(grf, 1, "BeamNG: {} {}: no engine, left out", v.Package(), id);
		return std::nullopt;
	}
	v.power_kw = power;

	/* The gearing: the gearbox's lowest and highest forward gear, the final drive (a differential's "gearRatio"). */
	double first = 3.5, top = 1.0, final_drive = 0;
	found.clear();
	for (const JValue *part : parts.used) FindAll(*part, "gearRatios", found);
	for (const JValue *g : found) {
		if (!g->IsArray()) continue;
		std::vector<double> forward;
		for (const JValue &r : g->items) {
			if (r.Num() > 0) forward.push_back(r.Num());
		}
		if (forward.empty()) continue;
		first = forward.front();
		top = forward.back();
		break;
	}
	found.clear();
	for (const JValue *part : parts.used) FindAll(*part, "gearRatio", found);
	for (const JValue *g : found) {
		if (g->Num() >= 1) {
			final_drive = g->Num();
			break;
		}
	}
	if (final_drive <= 0) final_drive = 3.9;

	/* As fast as its gearing lets it at its most revolutions, or its power against the air and the road. */
	const double width = max_x > min_x ? max_x - min_x : 1.8, height = max_z > min_z ? max_z - min_z : 1.5;
	const double drag_area = 0.4 * width * height * 0.85;
	const double usable = power * 1000 * 0.88, rolling = 0.012 * mass * 9.81;
	double lo = 1, hi = 150;
	for (int i = 0; i < 60; i++) {
		const double mid = (lo + hi) / 2;
		if (0.5 * 1.2 * drag_area * mid * mid * mid + rolling * mid < usable) lo = mid; else hi = mid;
	}
	double speed = lo;
	if (max_rpm > 0) speed = std::min(speed, max_rpm / (top * final_drive) * 2 * M_PI * radius / 60);
	v.speed_kmh = Clamp(speed * 3.6, 10.0, 400.0);
	v.effort = max_torque > 0 ? Clamp(max_torque * first * final_drive * 0.9 / radius / (mass * 9.81), 0.1, 1.0) : 0.5;

	/* A lorry carries goods; a car its four, a bus more. */
	std::string style;
	if (const JValue *s = info.Get("Body Style"); s != nullptr && s->IsString()) style = s->text;
	strtolower(style);
	if (style.find("bus") != std::string::npos) {
		v.capacity = 30;
	} else if (style.find("truck") != std::string::npos || v.mass_t > 3.5) {
		v.goods = true;
		v.capacity = Clamp(static_cast<int>(std::lround(v.mass_t * 2)), 5, 40);
	}

	/* The driver's eye: the camera inside of the driver ("driver", "dash"), else the first. */
	if (const JValue *main = parts.used.front(); main != nullptr) {
		const JValue *cams = main->Get("camerasInternal");
		const JValue *best = nullptr;
		int best_rank = 0;
		if (cams != nullptr && cams->IsArray()) {
			for (const JValue &row : cams->items) {
				if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString() || row.items[1].type != JValue::Type::Number) continue;
				const std::string &kind = row.items[0].text;
				int rank = kind == "driver" ? 4 : kind == "dash" ? 3 : kind.find("driver") != std::string::npos ? 2 : 1;
				if (rank > best_rank) {
					best_rank = rank;
					best = &row;
				}
			}
		}
		if (best != nullptr) {
			auto eye = ToObject(frame, best->items[1].Num(), best->items[2].Num(), best->items[3].Num());
			std::copy(eye.begin(), eye.end(), v.cab);
			v.has_cab = true;
		}
	}
	const std::string zip_name = zip.empty() ? folder : zip.substr(zip.find_last_of(PATHSEPCHAR) + 1);
	v.grfid = GrfIdOf(zip_name + "/" + id);
	return v;
}

/* ---- Where the mods are ---- */

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
 * The folders of mods: newgrf/beamng/ of the game's folders (for a player without BeamNG), the folders the player
 * names ("beamng_path", separated by ';': a folder of mods, or the user folder of BeamNG), and the mods folders of
 * BeamNG's user folders where BeamNG keeps them (its versions under %LOCALAPPDATA%\BeamNG.drive, the older
 * Documents\BeamNG.drive), each with its "repo" (the mods of BeamNG's repository).
 */
std::vector<std::string> ModFolders()
{
	std::vector<std::string> folders;
	auto add = [&folders](const std::string &folder) {
		if (folder.empty()) return;
		std::string key = folder;
		strtolower(key);
		if (std::ranges::any_of(folders, [&key](const std::string &f) { std::string k = f; strtolower(k); return k == key; })) return;
		folders.push_back(folder);
	};
	for (Searchpath sp : _valid_searchpaths) {
		std::string dir = FioGetDirectory(sp, Subdirectory::NewGrf);
		if (!dir.empty()) add(SubFolder(dir, "beamng"));
	}
	auto user_folder = [&](std::string root) {
		if (root.empty()) return;
		if (root.back() != PATHSEPCHAR) root += PATHSEP;
		std::string mods = SubFolder(root, "mods");
		if (mods.empty()) {
			add(root);
			return;
		}
		add(mods);
		add(SubFolder(mods, "repo"));
	};
	for (std::string_view rest = _beamng_path; !rest.empty();) {
		size_t semi = rest.find(';');
		std::string one(rest.substr(0, semi));
		StrTrimInPlace(one);
		user_folder(one);
		rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
	}
	std::vector<std::string> homes;
#ifdef _WIN32
	if (const wchar_t *local = _wgetenv(L"LOCALAPPDATA"); local != nullptr) homes.push_back(FS2OTTD(local) + PATHSEP "BeamNG.drive" PATHSEP);
	if (const wchar_t *profile = _wgetenv(L"USERPROFILE"); profile != nullptr) {
		homes.push_back(FS2OTTD(profile) + PATHSEP "Documents" PATHSEP "BeamNG.drive" PATHSEP);
		homes.push_back(FS2OTTD(profile) + PATHSEP "AppData" PATHSEP "Local" PATHSEP "BeamNG.drive" PATHSEP);
	}
#else
	if (auto home = GetEnv("HOME"); home.has_value()) {
		homes.push_back(std::string(*home) + "/.local/share/BeamNG.drive/");
		homes.push_back(std::string(*home) + "/.local/share/BeamNG/BeamNG.drive/");
	}
#endif
	for (const std::string &home : homes) {
		std::error_code ec;
		if (!std::filesystem::is_directory(OTTD2FS(home), ec)) continue;
		user_folder(home);
		/* The user folder of each version ("0.32", "current"). */
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(home), ec)) {
			if (entry.is_directory(ec)) user_folder(home + FS2OTTD(entry.path().filename().native()));
		}
	}
	return folders;
}

/** The folder the GRFs made of the cars go in: newgrf/beamng/ of the player's own folder. */
std::string GrfFolder()
{
	std::string dir = FioGetDirectory(Searchpath::PersonalDir, Subdirectory::NewGrf);
	if (dir.empty()) return {};
	return dir + "beamng" PATHSEP;
}

/* ---- The GRF ---- */

/** The bytes of a NewGRF of one road vehicle (container version 1, GRF version 8: Actions 8, 0 and 4, no sprites). */
std::vector<uint8_t> MakeGrf(const BeamngVehicle &v)
{
	std::vector<std::vector<uint8_t>> sprites;
	auto text = [](std::vector<uint8_t> &b, const std::string &s) {
		/* "Þ" first: the text is UTF-8. */
		b.push_back(0xC3);
		b.push_back(0x9E);
		b.insert(b.end(), s.begin(), s.end());
		b.push_back(0);
	};
	auto word = [](std::vector<uint8_t> &b, uint32_t x) { b.push_back(x & 0xFF); b.push_back((x >> 8) & 0xFF); };
	auto dword = [&word](std::vector<uint8_t> &b, uint32_t x) { word(b, x & 0xFFFF); word(b, x >> 16); };

	{
		std::vector<uint8_t> a8 = {0x08, 0x08};
		dword(a8, v.grfid);
		text(a8, "BeamNG: " + v.name);
		std::string author = v.author.empty() ? std::string() : ", autor " + v.author;
		text(a8, "Auto z modu BeamNG.drive (" + v.id + author + "). Model a textury patří autorům modu a zůstávají ve své složce: jen pro tvou hru, nešířit.");
		sprites.push_back(std::move(a8));
	}

	const double hp = v.power_kw / 0.7457;
	/* Prices by the original game's bus (MPS Regal: 90 hp, cost 120, running 91), a car of the same power less. */
	const double rel = std::max(hp, 20.0) / 90.0;
	const uint8_t cost = static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround((v.goods || v.capacity > 4 ? 100 : 50) * std::pow(rel, 0.45))), 8, 255));
	const uint8_t running = static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround((v.goods || v.capacity > 4 ? 80 : 40) * std::pow(rel, 0.4))), 8, 255));
	std::vector<uint8_t> a0 = {0x00, 0x01, 0, 0x01, static_cast<uint8_t>(BEAMNG_ENGINE_ID)};
	uint8_t props = 0;
	auto prop = [&](uint8_t p) { a0.push_back(p); props++; };
	prop(0x00); word(a0, 0);                       // introduced 1920: there in every game
	prop(0x02); a0.push_back(20);                  // reliability decay
	prop(0x03); a0.push_back(15);                  // a vehicle's life, years
	prop(0x04); a0.push_back(0xFF);                // the model never retires
	prop(0x06); a0.push_back(0x0F);                // every climate
	prop(0x07); a0.push_back(5);                   // loading speed
	prop(0x0E); a0.push_back(0xFF);                // the set's own sprites: drawn from its model (GetThreeDVehicleObjectSprite())
	prop(0x0F); a0.push_back(static_cast<uint8_t>(v.capacity));
	prop(0x10); a0.push_back(v.goods ? 0xFF : 0x00); // passengers, or the first goods it takes
	prop(0x1D); word(a0, v.goods ? 0xFFFE : 0x0001); // the classes of cargo it may be refitted to
	prop(0x1E); word(a0, v.goods ? 0x0001 : 0x0000);
	prop(0x11); a0.push_back(cost);
	prop(0x09); a0.push_back(running);
	prop(0x13); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(hp / 10)), 1, 255)));       // power, 10 hp
	prop(0x14); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(v.mass_t * 4)), 1, 255)));  // weight, 1/4 t
	prop(0x15); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(v.speed_kmh / 2.0117)), 1, 255))); // speed, 1.25 mph
	prop(0x18); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(v.effort * 255)), 1, 255)));  // tractive effort
	a0[2] = props;
	sprites.push_back(std::move(a0));
	{
		std::vector<uint8_t> a4 = {0x04, 0x01, 0x7F, 0x01, static_cast<uint8_t>(BEAMNG_ENGINE_ID)};
		text(a4, v.name);
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

std::vector<BeamngVehicle> _beamng_vehicles;
bool _beamng_scanned = false;

/* ---- The model: COLLADA ---- */

/** An element of XML: its name (without its namespace), its attributes, its elements and its text. */
struct XmlNode {
	std::string name;
	std::vector<std::pair<std::string, std::string>> attrs;
	std::vector<XmlNode> kids;
	std::string text;

	const std::string *Attr(std::string_view key) const
	{
		for (const auto &[k, v] : this->attrs) {
			if (k == key) return &v;
		}
		return nullptr;
	}
	std::string AttrOr(std::string_view key) const { const std::string *a = this->Attr(key); return a != nullptr ? *a : std::string(); }
	const XmlNode *Kid(std::string_view key) const
	{
		for (const XmlNode &k : this->kids) {
			if (k.name == key) return &k;
		}
		return nullptr;
	}
};

/** XML as COLLADA writes it: elements, attributes and texts; declarations, comments and CDATA passed over. */
bool ParseXml(std::string_view s, XmlNode &root)
{
	std::vector<XmlNode *> stack = {&root};
	size_t p = 0;
	while (p < s.size()) {
		size_t lt = s.find('<', p);
		if (lt == std::string_view::npos) lt = s.size();
		if (lt > p && stack.size() > 1) stack.back()->text.append(s.substr(p, lt - p));
		if (lt >= s.size()) break;
		p = lt + 1;
		if (s.substr(p, 3) == "!--") {
			size_t e = s.find("-->", p);
			p = e == std::string_view::npos ? s.size() : e + 3;
			continue;
		}
		if (s.substr(p, 8) == "![CDATA[") {
			size_t e = s.find("]]>", p);
			if (stack.size() > 1) stack.back()->text.append(s.substr(p + 8, (e == std::string_view::npos ? s.size() : e) - p - 8));
			p = e == std::string_view::npos ? s.size() : e + 3;
			continue;
		}
		if (p < s.size() && (s[p] == '?' || s[p] == '!')) {
			size_t e = s.find('>', p);
			p = e == std::string_view::npos ? s.size() : e + 1;
			continue;
		}
		if (p < s.size() && s[p] == '/') {
			size_t e = s.find('>', p);
			p = e == std::string_view::npos ? s.size() : e + 1;
			if (stack.size() > 1) stack.pop_back();
			continue;
		}
		size_t e = p;
		while (e < s.size() && s[e] != ' ' && s[e] != '>' && s[e] != '/' && s[e] != '\t' && s[e] != '\r' && s[e] != '\n') e++;
		XmlNode node;
		node.name = std::string(s.substr(p, e - p));
		if (size_t colon = node.name.find(':'); colon != std::string::npos) node.name.erase(0, colon + 1);
		p = e;
		bool closed = false;
		while (p < s.size()) {
			while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
			if (p >= s.size()) break;
			if (s[p] == '>') {
				p++;
				break;
			}
			if (s[p] == '/') {
				closed = true;
				p++;
				continue;
			}
			size_t eq = s.find('=', p);
			if (eq == std::string_view::npos) return false;
			std::string key(s.substr(p, eq - p));
			while (!key.empty() && key.back() == ' ') key.pop_back();
			p = eq + 1;
			while (p < s.size() && s[p] == ' ') p++;
			if (p >= s.size()) break;
			const char q = s[p];
			size_t end = s.find(q, p + 1);
			if (end == std::string_view::npos) return false;
			node.attrs.emplace_back(std::move(key), std::string(s.substr(p + 1, end - p - 1)));
			p = end + 1;
		}
		XmlNode *parent = stack.back();
		parent->kids.push_back(std::move(node));
		if (!closed && stack.size() < 64) stack.push_back(&parent->kids.back());
	}
	return true;
}

/** The numbers of a text, as many as there are. */
std::vector<float> Numbers(std::string_view s)
{
	std::vector<float> out;
	const char *p = s.data(), *e = s.data() + s.size();
	while (p < e) {
		while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
		if (p >= e) break;
		float f = 0;
		auto [next, ec] = std::from_chars(p, e, f);
		if (ec != std::errc()) {
			p++;
			continue;
		}
		out.push_back(f);
		p = next;
	}
	return out;
}

/** A whole number of a text, 0 when it is none. */
int IntOf(std::string_view s)
{
	int v = 0;
	std::from_chars(s.data(), s.data() + s.size(), v);
	return v;
}

/** A number of a text, 0 when it is none. */
double NumberOf(std::string_view s)
{
	double v = 0;
	std::from_chars(s.data(), s.data() + s.size(), v);
	return v;
}

/** A 4x4 matrix, rows first (as COLLADA writes it). */
using Matrix = std::array<double, 16>;
constexpr Matrix IDENTITY = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

Matrix Multiply(const Matrix &a, const Matrix &b)
{
	Matrix m{};
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			double sum = 0;
			for (int k = 0; k < 4; k++) sum += a[r * 4 + k] * b[k * 4 + c];
			m[r * 4 + c] = sum;
		}
	}
	return m;
}

/** A node's own matrix: its "matrix", else its translations, rotations and scales in their order. */
Matrix NodeMatrix(const XmlNode &n)
{
	Matrix m = IDENTITY;
	for (const XmlNode &k : n.kids) {
		std::vector<float> f = Numbers(k.text);
		Matrix t = IDENTITY;
		if (k.name == "matrix" && f.size() >= 16) {
			for (int i = 0; i < 16; i++) t[i] = f[i];
		} else if (k.name == "translate" && f.size() >= 3) {
			t[3] = f[0]; t[7] = f[1]; t[11] = f[2];
		} else if (k.name == "scale" && f.size() >= 3) {
			t[0] = f[0]; t[5] = f[1]; t[10] = f[2];
		} else if (k.name == "rotate" && f.size() >= 4) {
			double len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
			if (len <= 0) continue;
			const double x = f[0] / len, y = f[1] / len, z = f[2] / len, a = f[3] * M_PI / 180, c = std::cos(a), s = std::sin(a), C = 1 - c;
			t = {x * x * C + c, x * y * C - z * s, x * z * C + y * s, 0, y * x * C + z * s, y * y * C + c, y * z * C - x * s, 0, z * x * C - y * s, z * y * C + x * s, z * z * C + c, 0, 0, 0, 0, 1};
		} else {
			continue;
		}
		m = Multiply(m, t);
	}
	return m;
}

/** A mesh placed in the scene: its geometry, its node's matrix in the scene, its materials by their symbols. */
struct Placed {
	const XmlNode *geometry = nullptr;
	Matrix world = IDENTITY;
	std::map<std::string, std::string> materials;
};

void CollectNodes(const XmlNode &n, const Matrix &parent, const std::map<std::string, const XmlNode *> &geometries, std::map<std::string, Placed> &out, int depth = 0)
{
	if (depth > 40) return;
	const Matrix world = Multiply(parent, NodeMatrix(n));
	for (const XmlNode &k : n.kids) {
		if (k.name == "node") {
			CollectNodes(k, world, geometries, out, depth + 1);
		} else if (k.name == "instance_geometry") {
			std::string url = k.AttrOr("url");
			if (!url.empty() && url[0] == '#') url.erase(0, 1);
			auto g = geometries.find(url);
			if (g == geometries.end()) continue;
			Placed p;
			p.geometry = g->second;
			p.world = world;
			if (const XmlNode *bm = k.Kid("bind_material"); bm != nullptr) {
				if (const XmlNode *tc = bm->Kid("technique_common"); tc != nullptr) {
					for (const XmlNode &im : tc->kids) {
						std::string target = im.AttrOr("target");
						if (!target.empty() && target[0] == '#') target.erase(0, 1);
						p.materials[im.AttrOr("symbol")] = target;
					}
				}
			}
			for (const std::string &key : {n.AttrOr("name"), n.AttrOr("id"), g->second->AttrOr("name"), url}) {
				if (key.empty()) continue;
				std::string lower = key;
				strtolower(lower);
				out.emplace(lower, p);
			}
		}
	}
}

/** A triangle list of a geometry: its corners' places, normals and two sets of texture coordinates, of one material. */
struct Triangles {
	std::string material;
	std::vector<float> pos, nrm, uv0, uv1; ///< three corners a triangle, each corner its own
};

/** The triangles of a COLLADA geometry ("triangles", or "polylist" and "polygons" made triangles). */
std::vector<Triangles> ReadGeometry(const XmlNode &geometry)
{
	std::vector<Triangles> out;
	const XmlNode *mesh = geometry.Kid("mesh");
	if (mesh == nullptr) return out;
	std::map<std::string, std::pair<std::vector<float>, int>> sources;
	std::map<std::string, std::string> vertices;
	for (const XmlNode &k : mesh->kids) {
		if (k.name == "source") {
			const XmlNode *fa = k.Kid("float_array");
			if (fa == nullptr) continue;
			int stride = 3;
			if (const XmlNode *tc = k.Kid("technique_common"); tc != nullptr) {
				if (const XmlNode *acc = tc->Kid("accessor"); acc != nullptr) stride = std::max(1, IntOf(acc->AttrOr("stride")));
			}
			sources[k.AttrOr("id")] = {Numbers(fa->text), stride};
		} else if (k.name == "vertices") {
			for (const XmlNode &in : k.kids) {
				if (in.AttrOr("semantic") == "POSITION") {
					std::string src = in.AttrOr("source");
					if (!src.empty() && src[0] == '#') src.erase(0, 1);
					vertices[k.AttrOr("id")] = src;
				}
			}
		}
	}
	for (const XmlNode &k : mesh->kids) {
		if (k.name != "triangles" && k.name != "polylist" && k.name != "polygons") continue;
		struct Input { std::string semantic, source; int offset; };
		std::vector<Input> inputs;
		int stride = 0, texcoords = 0;
		for (const XmlNode &in : k.kids) {
			if (in.name != "input") continue;
			std::string src = in.AttrOr("source");
			if (!src.empty() && src[0] == '#') src.erase(0, 1);
			std::string semantic = in.AttrOr("semantic");
			if (semantic == "VERTEX") {
				auto v = vertices.find(src);
				if (v != vertices.end()) src = v->second;
			}
			if (semantic == "TEXCOORD") semantic = texcoords++ == 0 ? "UV0" : (texcoords == 2 ? "UV1" : "UVX");
			const int offset = IntOf(in.AttrOr("offset"));
			stride = std::max(stride, offset + 1);
			inputs.push_back({semantic, src, offset});
		}
		if (stride == 0) continue;
		/* The polygons: the index lists of "p", each polygon a fan of triangles. */
		std::vector<std::vector<float>> polygons;
		std::vector<int> counts;
		if (k.name == "polylist") {
			if (const XmlNode *vc = k.Kid("vcount"); vc != nullptr) {
				for (float f : Numbers(vc->text)) counts.push_back(static_cast<int>(f));
			}
		}
		for (const XmlNode &pk : k.kids) {
			if (pk.name == "p") polygons.push_back(Numbers(pk.text));
		}
		Triangles t;
		t.material = k.AttrOr("material");
		auto corner = [&](const std::vector<float> &idx, size_t c) {
			for (const Input &in : inputs) {
				const size_t at = c * stride + in.offset;
				if (at >= idx.size()) continue;
				auto s = sources.find(in.source);
				if (s == sources.end()) continue;
				const size_t i = static_cast<size_t>(idx[at]) * s->second.second;
				const std::vector<float> &data = s->second.first;
				auto take = [&](std::vector<float> &to, int n) {
					for (int j = 0; j < n; j++) to.push_back(i + j < data.size() ? data[i + j] : 0.0f);
				};
				if (in.semantic == "VERTEX") take(t.pos, 3);
				else if (in.semantic == "NORMAL") take(t.nrm, 3);
				else if (in.semantic == "UV0") take(t.uv0, 2);
				else if (in.semantic == "UV1") take(t.uv1, 2);
			}
		};
		for (const std::vector<float> &idx : polygons) {
			const size_t corners = idx.size() / stride;
			if (k.name == "triangles") {
				for (size_t c = 0; c + 2 < corners; c += 3) {
					for (size_t j = 0; j < 3; j++) corner(idx, c + j);
				}
			} else {
				size_t at = 0;
				std::vector<int> each = counts;
				if (k.name == "polygons" || each.empty()) each = {static_cast<int>(corners)};
				for (int n : each) {
					for (int j = 1; j + 1 < n; j++) {
						corner(idx, at);
						corner(idx, at + j);
						corner(idx, at + j + 1);
					}
					at += n;
				}
			}
		}
		/* Corners without a normal or coordinates get nothing: filled so every list is as long. */
		const size_t n = t.pos.size() / 3;
		if (t.nrm.size() != n * 3) t.nrm.assign(n * 3, 0.0f);
		if (t.uv0.size() != n * 2) t.uv0.assign(n * 2, 0.0f);
		if (t.uv1.size() != n * 2) t.uv1 = t.uv0;
		out.push_back(std::move(t));
	}
	return out;
}

/* ---- Materials and textures ---- */

/** What the game takes of a material of BeamNG. */
struct CarMaterial {
	uint32_t colour = 0xFFB4B4B4; ///< "diffuseColor" (or "baseColorFactor") of its first layer
	std::string texture;          ///< "diffuseMap" (or "colorMap", "baseColorMap") of its first layer
	bool paint = false;           ///< "instanceDiffuse": the car's paint
	std::string mask;             ///< "opacityMap" on the second texture coordinates: holes where it is dark
	bool mask_uv1 = false;
};

/** A text without the spaces, tabs and ends of lines about it. */
std::string Trimmed(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
	return std::string(s);
}

/** The materials of materials.cs (Torque's "singleton Material(name) { key[layer] = "value"; }") and of *.materials.json. */
std::map<std::string, CarMaterial> ReadMaterials(const BeamngVehicle &v)
{
	std::map<std::string, CarMaterial> out;
	for (const std::string &f : ListCarFiles(v)) {
		if (EndsWithIgnoreCase(f, ".cs")) {
			std::vector<uint8_t> bytes = ReadModFile(v, f);
			std::string_view s(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			size_t p = 0;
			while ((p = s.find("singleton Material(", p)) != std::string_view::npos) {
				size_t open = s.find('{', p), close = s.find("};", p);
				if (open == std::string_view::npos || close == std::string_view::npos || close < open) break;
				const size_t name_end = s.find(')', p);
				std::string name(s.substr(p + 19, name_end - p - 19));
				CarMaterial m;
				std::map<std::string, std::string> values;
				std::string_view body = s.substr(open + 1, close - open - 1);
				for (size_t q = 0; q < body.size();) {
					size_t eq = body.find('=', q), semi = body.find(';', q);
					if (eq == std::string_view::npos || semi == std::string_view::npos) break;
					if (eq > semi) {
						q = semi + 1;
						continue;
					}
					std::string key = Trimmed(body.substr(q, eq - q)), value = Trimmed(body.substr(eq + 1, semi - eq - 1));
					if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);
					values[key] = value;
					q = semi + 1;
				}
				auto get = [&](const std::string &k) { auto it = values.find(k); return it != values.end() ? it->second : std::string(); };
				if (std::string c = get("diffuseColor[0]"); !c.empty()) {
					JValue t;
					t.type = JValue::Type::String;
					t.text = c;
					m.colour = ColourOf(t);
				}
				for (const char *k : {"diffuseMap[0]", "colorMap[0]", "baseTex[0]"}) {
					if (m.texture.empty()) m.texture = get(k);
				}
				for (const auto &[k, val] : values) {
					if (k.starts_with("instanceDiffuse") && (val == "true" || val == "1")) m.paint = true;
				}
				m.mask = get("opacityMap[0]");
				m.mask_uv1 = get("opacityMapUV[0]") == "1";
				std::string map_to = get("mapTo");
				out[map_to.empty() ? name : map_to] = m;
				p = close + 2;
			}
		} else if (EndsWithIgnoreCase(f, ".materials.json") || StrEqualsIgnoreCase(f, "materials.json")) {
			JValue all = ParseJson(ReadModFile(v, f));
			for (const auto &[name, mat] : all.members) {
				if (!mat.IsObject()) continue;
				CarMaterial m;
				const JValue *stages = mat.Get("Stages");
				const JValue *first = stages != nullptr && stages->IsArray() && !stages->items.empty() ? &stages->items[0] : nullptr;
				if (first != nullptr) {
					for (const char *k : {"diffuseColor", "baseColorFactor"}) {
						if (const JValue *c = first->Get(k); c != nullptr) m.colour = ColourOf(*c);
					}
					for (const char *k : {"diffuseMap", "colorMap", "baseColorMap"}) {
						if (const JValue *t = first->Get(k); t != nullptr && t->IsString() && m.texture.empty()) m.texture = t->text;
					}
					if (const JValue *t = first->Get("opacityMap"); t != nullptr && t->IsString()) m.mask = t->text;
					if (const JValue *t = first->Get("opacityMapUseUV"); t != nullptr) m.mask_uv1 = t->Num() == 1;
				}
				if (stages != nullptr && stages->IsArray()) {
					for (const JValue &st : stages->items) {
						for (const char *k : {"instanceDiffuse", "useColorPalette"}) {
							if (const JValue *b = st.Get(k); b != nullptr && b->IsTrue()) m.paint = true;
						}
					}
				}
				std::string map_to = mat.Get("mapTo") != nullptr && mat.Get("mapTo")->IsString() ? mat.Get("mapTo")->text : name;
				out[map_to] = m;
			}
		}
	}
	return out;
}

/** The colours of a block of BC1 (DXT1): two of 5:6:5 and two between them (or one between and black). */
void Bc1Colours(const uint8_t *b, uint8_t c[4][4], bool always_four)
{
	const uint16_t c0 = Le16(b), c1 = Le16(b + 2);
	auto expand = [&](uint16_t v, uint8_t *o) {
		o[0] = static_cast<uint8_t>(((v >> 11) & 31) * 255 / 31);
		o[1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63);
		o[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
		o[3] = 255;
	};
	expand(c0, c[0]);
	expand(c1, c[1]);
	for (int i = 0; i < 3; i++) {
		if (c0 > c1 || always_four) {
			c[2][i] = static_cast<uint8_t>((2 * c[0][i] + c[1][i]) / 3);
			c[3][i] = static_cast<uint8_t>((c[0][i] + 2 * c[1][i]) / 3);
		} else {
			c[2][i] = static_cast<uint8_t>((c[0][i] + c[1][i]) / 2);
			c[3][i] = 0;
		}
	}
	c[2][3] = 255;
	c[3][3] = (c0 > c1 || always_four) ? 255 : 0;
}

/** The eight values of a block of BC4 (the alpha of BC3): two, and six or four between them (then 0 and 255). */
void Bc4Values(const uint8_t *b, uint8_t a[8])
{
	a[0] = b[0];
	a[1] = b[1];
	for (int i = 2; i < 8; i++) {
		if (a[0] > a[1]) {
			a[i] = static_cast<uint8_t>(((8 - i) * a[0] + (i - 1) * a[1]) / 7);
		} else if (i < 6) {
			a[i] = static_cast<uint8_t>(((6 - i) * a[0] + (i - 1) * a[1]) / 5);
		} else {
			a[i] = i == 6 ? 0 : 255;
		}
	}
}

/**
 * A .dds as RGBA: BC1, BC2, BC3 and BC4 (DXT1, DXT3, DXT5, ATI1; of the old header or of DX10's) and 32-bit pixels;
 * the first of its sizes no bigger than 1024. BC5 (normals), BC6 and BC7 not.
 */
bool DecodeDds(const std::vector<uint8_t> &d, int &w, int &h, std::vector<uint8_t> &rgba)
{
	if (d.size() < 128 || std::string_view(reinterpret_cast<const char *>(d.data()), 4) != "DDS ") return false;
	h = static_cast<int>(Le32(&d[12]));
	w = static_cast<int>(Le32(&d[16]));
	const uint32_t mips = std::max<uint32_t>(1, Le32(&d[28]));
	const uint32_t pf_flags = Le32(&d[80]);
	const std::string_view fourcc(reinterpret_cast<const char *>(&d[84]), 4);
	size_t at = 128;
	enum class Kind : uint8_t { None, Bc1, Bc2, Bc3, Bc4, Rgba, Bgra } kind = Kind::None;
	if (fourcc == "DXT1") kind = Kind::Bc1;
	else if (fourcc == "DXT3") kind = Kind::Bc2;
	else if (fourcc == "DXT5") kind = Kind::Bc3;
	else if (fourcc == "ATI1" || fourcc == "BC4U") kind = Kind::Bc4;
	else if (fourcc == "DX10" && d.size() >= 148) {
		const uint32_t dxgi = Le32(&d[128]);
		at = 148;
		if (dxgi == 71 || dxgi == 72) kind = Kind::Bc1;
		else if (dxgi == 74 || dxgi == 75) kind = Kind::Bc2;
		else if (dxgi == 77 || dxgi == 78) kind = Kind::Bc3;
		else if (dxgi == 80 || dxgi == 81) kind = Kind::Bc4;
		else if (dxgi == 28 || dxgi == 29) kind = Kind::Rgba;
		else if (dxgi == 87 || dxgi == 91) kind = Kind::Bgra;
	} else if ((pf_flags & 0x40) != 0 && Le32(&d[88]) == 32) {
		kind = Le32(&d[92]) == 0x00FF0000 ? Kind::Bgra : Kind::Rgba;
	}
	if (kind == Kind::None || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
	const bool blocks = kind != Kind::Rgba && kind != Kind::Bgra;
	const size_t block_bytes = (kind == Kind::Bc1 || kind == Kind::Bc4) ? 8 : 16;
	auto level_size = [&](int lw, int lh) -> size_t {
		return blocks ? static_cast<size_t>((lw + 3) / 4) * ((lh + 3) / 4) * block_bytes : static_cast<size_t>(lw) * lh * 4;
	};
	for (uint32_t m = 1; m < mips && (w > 1024 || h > 1024); m++) {
		at += level_size(w, h);
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	if (at + level_size(w, h) > d.size()) return false;
	rgba.assign(static_cast<size_t>(w) * h * 4, 0);
	if (!blocks) {
		for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
			const uint8_t *p = &d[at + i * 4];
			uint8_t *o = &rgba[i * 4];
			o[0] = kind == Kind::Bgra ? p[2] : p[0];
			o[1] = p[1];
			o[2] = kind == Kind::Bgra ? p[0] : p[2];
			o[3] = p[3];
		}
		return true;
	}
	const int bw = (w + 3) / 4, bh = (h + 3) / 4;
	for (int by = 0; by < bh; by++) {
		for (int bx = 0; bx < bw; bx++) {
			const uint8_t *b = &d[at + (static_cast<size_t>(by) * bw + bx) * block_bytes];
			uint8_t c[4][4] = {}, a[8] = {};
			const uint8_t *colour = kind == Kind::Bc1 ? b : b + 8;
			if (kind != Kind::Bc4) Bc1Colours(colour, c, kind != Kind::Bc1);
			if (kind == Kind::Bc3) Bc4Values(b, a);
			if (kind == Kind::Bc4) Bc4Values(b, a);
			const uint64_t abits = kind == Kind::Bc3 || kind == Kind::Bc4 ? (static_cast<uint64_t>(Le32(b + 2)) | static_cast<uint64_t>(Le16(b + 6)) << 32) : 0;
			const uint32_t cbits = kind == Kind::Bc4 ? 0 : Le32(colour + 4);
			for (int py = 0; py < 4; py++) {
				for (int px = 0; px < 4; px++) {
					const int x = bx * 4 + px, y = by * 4 + py;
					if (x >= w || y >= h) continue;
					const int i = py * 4 + px;
					uint8_t *o = &rgba[(static_cast<size_t>(y) * w + x) * 4];
					if (kind == Kind::Bc4) {
						const uint8_t g = a[(abits >> (3 * i)) & 7];
						o[0] = o[1] = o[2] = g;
						o[3] = 255;
						continue;
					}
					const uint8_t *cc = c[(cbits >> (2 * i)) & 3];
					o[0] = cc[0]; o[1] = cc[1]; o[2] = cc[2]; o[3] = cc[3];
					if (kind == Kind::Bc2) o[3] = static_cast<uint8_t>(((Le16(b + 2 * py) >> (4 * px)) & 15) * 17);
					if (kind == Kind::Bc3) o[3] = a[(abits >> (3 * i)) & 7];
				}
			}
		}
	}
	return true;
}

/** A texture of the mod, .dds or .png (the latter as it is, for the game's own reading of PNG); false when it is not there or not read. */
bool ReadTexture(const BeamngVehicle &v, const std::string &name, BeamngMesh::Material &m, std::vector<uint8_t> *png)
{
	if (name.empty() || name.find("common/null") != std::string::npos) return false;
	std::vector<uint8_t> bytes = ReadModFile(v, name);
	if (bytes.empty()) {
		/* A texture named without its folder: the car's own. */
		std::string base = name.substr(name.find_last_of("/\\") + 1);
		if (base != name) bytes = ReadModFile(v, base);
	}
	if (bytes.empty()) return false;
	if (bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P') {
		if (png == nullptr) return false;
		*png = std::move(bytes);
		return true;
	}
	return DecodeDds(bytes, m.w, m.h, m.rgba);
}

/**
 * A car's mesh made lighter (the player, 16. 10.: "we fight the slowing of the game"): the corners of a material
 * that fall in the same cell of the size (and face about the same way) made one, the triangles that come to nothing
 * dropped. The models of BeamNG are made to be seen from a metre (a wheel's rim of 15 000 triangles); the closest
 * zoom of the map has a pixel of 4 cm.
 */
void Simplify(BeamngMesh &mesh, float cell)
{
	struct Key {
		int32_t x, y, z;
		int8_t nx, ny, nz;
		uint8_t material;
		bool operator<(const Key &o) const { return std::tie(x, y, z, nx, ny, nz, material) < std::tie(o.x, o.y, o.z, o.nx, o.ny, o.nz, o.material); }
	};
	std::map<Key, uint32_t> corners;
	std::vector<float> pos, nrm, uv;
	std::vector<float> sums; // the normals summed, then made one long
	std::vector<uint32_t> tri;
	std::vector<uint8_t> tri_tex;
	for (size_t t = 0; t + 2 < mesh.tri.size(); t += 3) {
		const uint8_t material = t / 3 < mesh.tri_tex.size() ? mesh.tri_tex[t / 3] : 0;
		uint32_t made[3];
		for (int j = 0; j < 3; j++) {
			const uint32_t c = mesh.tri[t + j];
			const float *p = &mesh.pos[c * 3], *n = &mesh.nrm[c * 3];
			Key key{static_cast<int32_t>(std::floor(p[0] / cell)), static_cast<int32_t>(std::floor(p[1] / cell)), static_cast<int32_t>(std::floor(p[2] / cell)),
					static_cast<int8_t>(std::lround(n[0] * 2)), static_cast<int8_t>(std::lround(n[1] * 2)), static_cast<int8_t>(std::lround(n[2] * 2)), material};
			auto [it, added] = corners.emplace(key, static_cast<uint32_t>(pos.size() / 3));
			if (added) {
				/* The cell's middle, its first corner's coordinates. */
				pos.push_back((key.x + 0.5f) * cell);
				pos.push_back((key.y + 0.5f) * cell);
				pos.push_back((key.z + 0.5f) * cell);
				sums.insert(sums.end(), {0.0f, 0.0f, 0.0f});
				uv.push_back(mesh.uv[c * 2]);
				uv.push_back(mesh.uv[c * 2 + 1]);
			}
			for (int k = 0; k < 3; k++) sums[it->second * 3 + k] += n[k];
			made[j] = it->second;
		}
		if (made[0] == made[1] || made[1] == made[2] || made[0] == made[2]) continue;
		tri.insert(tri.end(), {made[0], made[1], made[2]});
		tri_tex.push_back(material);
	}
	nrm.resize(sums.size());
	for (size_t i = 0; i + 2 < sums.size(); i += 3) {
		const float len = std::sqrt(sums[i] * sums[i] + sums[i + 1] * sums[i + 1] + sums[i + 2] * sums[i + 2]);
		for (int k = 0; k < 3; k++) nrm[i + k] = len > 0 ? sums[i + k] / len : 0.0f;
	}
	mesh.pos = std::move(pos);
	mesh.nrm = std::move(nrm);
	mesh.uv = std::move(uv);
	mesh.tri = std::move(tri);
	mesh.tri_tex = std::move(tri_tex);
}

} // namespace

/** "beamng_path" of the configuration: folders of mods of BeamNG besides the usual ones, separated by ';'. */
std::string _beamng_path;

/** The cars of the mods (ModFolders()), looked for once (and again by PrepareBeamngGrfs()). */
const std::vector<BeamngVehicle> &FindBeamngVehicles()
{
	if (_beamng_scanned) return _beamng_vehicles;
	_beamng_scanned = true;
	_beamng_vehicles.clear();
	_zips.clear();
	const std::string grf_folder = GrfFolder();
	if (grf_folder.empty()) return _beamng_vehicles;
	auto take = [&](std::optional<BeamngVehicle> car) {
		if (!car.has_value()) return;
		if (std::ranges::any_of(_beamng_vehicles, [&car](const BeamngVehicle &o) { return o.grfid == car->grfid; })) {
			Debug(grf, 0, "BeamNG: {} {} has the same GRF id as another, left out", car->Package(), car->id);
			return;
		}
		car->grf_path = grf_folder + car->id + ".grf";
		if (std::ranges::any_of(_beamng_vehicles, [&car](const BeamngVehicle &o) { return StrEqualsIgnoreCase(o.grf_path, car->grf_path); })) {
			car->grf_path = grf_folder + car->id + fmt::format("_{:08X}", car->grfid) + ".grf";
		}
		Debug(grf, 1, "BeamNG: {} ({} {}): {} {}, {:.0f} kW, {:.0f} km/h, {:.2f} t, effort {:.2f}, {} paints, GRF {:02X}{:02X}{:02X}{:02X}", car->name, car->Package(), car->id,
				car->goods ? "goods" : "seats", car->capacity, car->power_kw, car->speed_kmh, car->mass_t, car->effort, car->paints.size(),
				car->grfid & 0xFF, (car->grfid >> 8) & 0xFF, (car->grfid >> 16) & 0xFF, car->grfid >> 24);
		_beamng_vehicles.push_back(std::move(*car));
	};
	for (const std::string &folder : ModFolders()) {
		Debug(grf, 1, "BeamNG: mods looked for in {}", folder);
		std::error_code ec;
		std::vector<std::filesystem::directory_entry> entries;
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) entries.push_back(entry);
		std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.path() < b.path(); });
		for (const auto &entry : entries) {
			const std::string file = FS2OTTD(entry.path().filename().native());
			if (entry.is_regular_file(ec) && EndsWithIgnoreCase(file, ".zip")) {
				const Zip *zip = OpenZip(folder + file);
				if (zip == nullptr) continue;
				std::set<std::string> cars;
				for (const ZipEntry &e : zip->entries) {
					if (e.name.size() < 10 || !StrEqualsIgnoreCase(std::string_view(e.name).substr(0, 9), "vehicles/") || !EndsWithIgnoreCase(e.name, ".jbeam")) continue;
					const size_t slash = e.name.find('/', 9);
					if (slash == std::string::npos || e.name.find('/', slash + 1) != std::string::npos) continue;
					cars.insert(e.name.substr(0, slash + 1));
				}
				for (const std::string &car : cars) take(ReadCar(folder + file, car, car.substr(9, car.size() - 10)));
			} else if (entry.is_directory(ec)) {
				/* An unpacked mod: its vehicles/ folder, also under "unpacked". */
				std::vector<std::string> roots = {folder + file + PATHSEP};
				if (StrEqualsIgnoreCase(file, "unpacked")) {
					roots.clear();
					for (const auto &mod : std::filesystem::directory_iterator(entry.path(), ec)) {
						if (mod.is_directory(ec)) roots.push_back(folder + file + PATHSEP + FS2OTTD(mod.path().filename().native()) + PATHSEP);
					}
				}
				for (const std::string &root : roots) {
					std::string vehicles = SubFolder(root, "vehicles");
					if (vehicles.empty()) continue;
					for (const auto &car : std::filesystem::directory_iterator(OTTD2FS(vehicles), ec)) {
						if (!car.is_directory(ec)) continue;
						const std::string id = FS2OTTD(car.path().filename().native());
						take(ReadCar({}, vehicles + id + PATHSEP, id));
					}
				}
			}
		}
	}
	return _beamng_vehicles;
}

/** A car of FindBeamngVehicles() by its place there. */
const BeamngVehicle *GetBeamngVehicle(size_t index)
{
	const std::vector<BeamngVehicle> &all = FindBeamngVehicles();
	return index < all.size() ? &all[index] : nullptr;
}

/**
 * Before the NewGRFs are looked for: a NewGRF in newgrf/beamng/ for every car of the mods, of its figures (written
 * only when it changed, so its checksum stays). The player then has it among the NewGRFs as any other, by the car's
 * name. One made by the game whose car is no longer there goes.
 */
void PrepareBeamngGrfs()
{
	_beamng_scanned = false;
	const std::vector<BeamngVehicle> &found = FindBeamngVehicles();
	const std::string folder = GrfFolder();
	if (folder.empty()) return;
	static constexpr std::string_view MARK = "Auto z modu BeamNG.drive";
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(folder), ec)) {
		std::string file = FS2OTTD(entry.path().filename().native());
		if (!EndsWithIgnoreCase(file, ".grf")) continue;
		if (std::ranges::any_of(found, [&](const BeamngVehicle &v) { return StrEqualsIgnoreCase(v.grf_path, folder + file); })) continue;
		/* Only one the game made: small, with its words. */
		std::vector<uint8_t> old = ReadBytes(folder + file);
		if (old.empty() || old.size() > 4096 || std::search(old.begin(), old.end(), MARK.begin(), MARK.end()) == old.end()) continue;
		std::filesystem::remove(entry.path(), ec);
		Debug(grf, 1, "BeamNG: {} removed, its car is no longer there", folder + file);
	}
	if (!found.empty()) FioCreateDirectory(folder);
	for (const BeamngVehicle &v : found) {
		std::vector<uint8_t> grf = MakeGrf(v);
		if (ReadBytes(v.grf_path) == grf) continue;
		auto f = FileHandle::Open(v.grf_path, "wb");
		if (!f.has_value()) {
			Debug(grf, 0, "BeamNG: {} cannot be written", v.grf_path);
			continue;
		}
		fwrite(grf.data(), 1, grf.size(), *f);
		Debug(grf, 1, "BeamNG: {} written", v.grf_path);
	}
}

/**
 * Read a car's model for drawing: the meshes its flexbodies name, each where its flexbody puts it ("pos", "rot" in
 * degrees about x, y and z, "scale"; else where its node in the .dae is), in the frame of the vehicle objects; the
 * materials' colours, textures and paint; the body's holes cut by its mask; the glass of its lights and where they are.
 */
bool ReadBeamngShape(const BeamngVehicle &v, BeamngMesh &mesh)
{
	JValue info = ParseJson(ReadModFile(v, "info.json"));
	CarParts parts;
	if (!ReadCarParts(v, info.IsObject() ? &info : nullptr, parts)) return false;
	auto fr = _frames.find(v.Package() + "|" + v.folder);
	const CarFrame frame = fr != _frames.end() ? fr->second : CarFrame{};

	/* The model: the first .dae of the folder, and those the flexbodies may name. */
	std::vector<std::string> daes;
	for (const std::string &f : ListCarFiles(v)) {
		if (EndsWithIgnoreCase(f, ".dae")) daes.push_back(f);
	}
	if (daes.empty()) return false;
	std::vector<XmlNode> docs(daes.size());
	std::map<std::string, Placed> placed;
	double unit = 1;
	bool y_up = false;
	for (size_t i = 0; i < daes.size(); i++) {
		std::vector<uint8_t> bytes = ReadModFile(v, daes[i]);
		if (!ParseXml(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), docs[i])) continue;
		const XmlNode *collada = docs[i].Kid("COLLADA");
		if (collada == nullptr) continue;
		if (const XmlNode *asset = collada->Kid("asset"); asset != nullptr) {
			if (const XmlNode *u = asset->Kid("unit"); u != nullptr && u->Attr("meter") != nullptr) unit = NumberOf(*u->Attr("meter"));
			if (const XmlNode *up = asset->Kid("up_axis"); up != nullptr) y_up = up->text.find("Y_UP") != std::string::npos;
		}
		std::map<std::string, const XmlNode *> geometries;
		if (const XmlNode *lib = collada->Kid("library_geometries"); lib != nullptr) {
			for (const XmlNode &g : lib->kids) {
				if (g.name == "geometry") geometries[g.AttrOr("id")] = &g;
			}
		}
		if (const XmlNode *lib = collada->Kid("library_visual_scenes"); lib != nullptr) {
			for (const XmlNode &scene : lib->kids) {
				for (const XmlNode &n : scene.kids) {
					if (n.name == "node") CollectNodes(n, IDENTITY, geometries, placed);
				}
			}
		}
		/* The names of the materials: the scene's material by its id, its name. */
		if (const XmlNode *lib = collada->Kid("library_materials"); lib != nullptr) {
			std::map<std::string, std::string> names;
			for (const XmlNode &m : lib->kids) names[m.AttrOr("id")] = m.AttrOr("name").empty() ? m.AttrOr("id") : m.AttrOr("name");
			for (auto &[key, p] : placed) {
				for (auto &[symbol, target] : p.materials) {
					auto it = names.find(target);
					if (it != names.end()) target = it->second;
				}
			}
		}
	}
	if (unit <= 0) unit = 1;

	std::map<std::string, CarMaterial> materials = ReadMaterials(v);
	/* The glowing glass: a material of "glowMap" with its lights ("lowbeam", "highbeam": ahead; "brake": at the back). */
	std::map<std::string, bool> glow; // material to whether it is at the back
	for (const JValue *part : parts.used) {
		if (const JValue *g = part->Get("glowMap"); g != nullptr && g->IsObject()) {
			for (const auto &[name, entry] : g->members) {
				const JValue *fn = entry.Get("simpleFunction");
				if (fn == nullptr || !fn->IsObject()) continue;
				bool ahead = false, back = false;
				for (const auto &[f, val] : fn->members) {
					if (f == "lowbeam" || f == "highbeam" || f == "headlights") ahead = true;
					if (f == "brake" || f == "brakelight" || f == "taillight" || f == "tail") back = true;
				}
				if (ahead || back) glow[name] = back;
			}
		}
	}

	/* The materials of the mesh: the paint first (white, painted the car's colour), then each in its order. */
	mesh.materials.push_back({"paint", 0, 0, {}, {}, 0xFFFFFFFF});
	std::map<std::string, uint8_t> index;
	std::map<std::string, std::pair<int, std::pair<int, std::vector<uint8_t>>>> masks; // mask texture: w, h, grey
	auto material_of = [&](const std::string &name) -> uint8_t {
		auto it = index.find(name);
		if (it != index.end()) return it->second;
		auto m = materials.find(name);
		uint8_t at = 0;
		if (m == materials.end() || !m->second.paint) {
			if (mesh.materials.size() >= 255) return 0;
			BeamngMesh::Material made;
			made.name = name;
			made.flat = m != materials.end() ? m->second.colour : 0xFFB4B4B4;
			if (m != materials.end()) ReadTexture(v, m->second.texture, made, &made.png);
			at = static_cast<uint8_t>(mesh.materials.size());
			mesh.materials.push_back(std::move(made));
		}
		index[name] = at;
		return at;
	};
	auto mask_value = [&](const std::string &name, float u, float vv) -> int {
		auto it = masks.find(name);
		if (it == masks.end()) {
			BeamngMesh::Material m;
			std::pair<int, std::pair<int, std::vector<uint8_t>>> made{0, {0, {}}};
			if (ReadTexture(v, name, m, nullptr)) {
				made.first = m.w;
				made.second.first = m.h;
				for (size_t i = 0; i < m.rgba.size(); i += 4) made.second.second.push_back(m.rgba[i]);
			}
			it = masks.emplace(name, std::move(made)).first;
		}
		const int w = it->second.first, h = it->second.second.first;
		if (w <= 0 || h <= 0) return 255;
		u -= std::floor(u);
		vv -= std::floor(vv);
		const int x = std::min(w - 1, static_cast<int>(u * w)), y = std::min(h - 1, static_cast<int>((1 - vv) * h));
		return it->second.second.second[static_cast<size_t>(y) * w + x];
	};

	/* The flexbodies of the parts: a mesh, its groups, and where it is put. */
	int meshes = 0, missing = 0;
	for (const JValue *part : parts.used) {
		const JValue *flex = part->Get("flexbodies");
		if (flex == nullptr || !flex->IsArray()) continue;
		for (const JValue &row : flex->items) {
			if (!row.IsArray() || row.items.empty() || !row.items[0].IsString() || row.items[0].text == "mesh") continue;
			std::string mesh_name = row.items[0].text;
			strtolower(mesh_name);
			auto pl = placed.find(mesh_name);
			if (pl == placed.end()) {
				missing++;
				continue;
			}
			meshes++;
			/* Its placing: "pos", "rot" (degrees, about x, then y, then z), "scale"; the node's matrix of the .dae before it. */
			double pos[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
			for (size_t i = 1; i < row.items.size(); i++) {
				const JValue &o = row.items[i];
				if (!o.IsObject()) continue;
				auto xyz = [&](const char *key, double *out) {
					if (const JValue *p = o.Get(key); p != nullptr && p->IsObject()) {
						out[0] = p->Get("x") != nullptr ? p->Get("x")->Num(out[0]) : out[0];
						out[1] = p->Get("y") != nullptr ? p->Get("y")->Num(out[1]) : out[1];
						out[2] = p->Get("z") != nullptr ? p->Get("z")->Num(out[2]) : out[2];
					}
				};
				xyz("pos", pos);
				xyz("rot", rot);
				xyz("scale", scale);
			}
			const double rx = rot[0] * M_PI / 180, ry = rot[1] * M_PI / 180, rz = rot[2] * M_PI / 180;
			const Matrix mx = {1, 0, 0, 0, 0, std::cos(rx), -std::sin(rx), 0, 0, std::sin(rx), std::cos(rx), 0, 0, 0, 0, 1};
			const Matrix my = {std::cos(ry), 0, std::sin(ry), 0, 0, 1, 0, 0, -std::sin(ry), 0, std::cos(ry), 0, 0, 0, 0, 1};
			const Matrix mz = {std::cos(rz), -std::sin(rz), 0, 0, std::sin(rz), std::cos(rz), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
			Matrix place = Multiply(Multiply(mz, my), mx);
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++) place[r * 4 + c] *= scale[c];
				place[r * 4 + 3] = pos[r];
			}
			/* The units of the .dae and its up: metres, z up. */
			Matrix units = IDENTITY;
			units[0] = units[5] = units[10] = unit;
			if (y_up) units = {unit, 0, 0, 0, 0, 0, -unit, 0, 0, unit, 0, 0, 0, 0, 0, 1};
			const Matrix m = Multiply(place, Multiply(units, pl->second.world));
			for (Triangles &t : ReadGeometry(*pl->second.geometry)) {
				auto bound = pl->second.materials.find(t.material);
				const std::string material = bound != pl->second.materials.end() ? bound->second : t.material;
				const uint8_t tex = material_of(material);
				auto cm = materials.find(material);
				const bool cut = cm != materials.end() && !cm->second.mask.empty();
				auto gl = glow.find(material);
				const size_t corners = t.pos.size() / 3;
				for (size_t c = 0; c + 2 < corners; c += 3) {
					if (cut) {
						/* A hole of the body where its mask is dark at the triangle's middle. */
						const std::vector<float> &uvs = cm->second.mask_uv1 ? t.uv1 : t.uv0;
						const float u = (uvs[c * 2] + uvs[c * 2 + 2] + uvs[c * 2 + 4]) / 3, w = (uvs[c * 2 + 1] + uvs[c * 2 + 3] + uvs[c * 2 + 5]) / 3;
						if (mask_value(cm->second.mask, u, w) < 128) continue;
					}
					const uint32_t base = static_cast<uint32_t>(mesh.pos.size() / 3);
					const uint32_t night_base = static_cast<uint32_t>(mesh.night_pos.size() / 3);
					for (size_t j = 0; j < 3; j++) {
						const size_t k = c + j;
						const double x = t.pos[k * 3], y = t.pos[k * 3 + 1], z = t.pos[k * 3 + 2];
						const double wx = m[0] * x + m[1] * y + m[2] * z + m[3], wy = m[4] * x + m[5] * y + m[6] * z + m[7], wz = m[8] * x + m[9] * y + m[10] * z + m[11];
						auto o = ToObject(frame, wx, wy, wz);
						mesh.pos.insert(mesh.pos.end(), o.begin(), o.end());
						const double nx = t.nrm[k * 3], ny = t.nrm[k * 3 + 1], nz = t.nrm[k * 3 + 2];
						double tx = m[0] * nx + m[1] * ny + m[2] * nz, ty = m[4] * nx + m[5] * ny + m[6] * nz, tz = m[8] * nx + m[9] * ny + m[10] * nz;
						const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
						if (len > 0) { tx /= len; ty /= len; tz /= len; }
						mesh.nrm.push_back(static_cast<float>(-ty));
						mesh.nrm.push_back(static_cast<float>(tx));
						mesh.nrm.push_back(static_cast<float>(tz));
						mesh.uv.push_back(t.uv0[k * 2]);
						mesh.uv.push_back(1.0f - t.uv0[k * 2 + 1]);
						mesh.tri.push_back(base + static_cast<uint32_t>(j));
						if (gl != glow.end()) {
							mesh.night_pos.insert(mesh.night_pos.end(), o.begin(), o.end());
							mesh.night_tri.push_back(night_base + static_cast<uint32_t>(j));
						}
					}
					mesh.tri_tex.push_back(tex);
					if (gl != glow.end()) {
						mesh.night_colour.push_back(gl->second ? 0xFFFF2814u : 0xFFFFF4D8u);
						mesh.night_kind.push_back(gl->second ? 1 : 0);
					}
				}
			}
		}
	}
	if (mesh.tri.empty()) return false;
	const size_t full = mesh.tri.size() / 3;
	Simplify(mesh, 0.02f);

	/* The lights: the nodes their props hang from ("lowbeam" ahead, "brake" at the back), a place once. */
	for (const JValue *part : parts.used) {
		const JValue *props = part->Get("props");
		if (props == nullptr || !props->IsArray()) continue;
		for (const JValue &row : props->items) {
			if (!row.IsArray() || row.items.size() < 3 || !row.items[0].IsString() || !row.items[2].IsString()) continue;
			const std::string &func = row.items[0].text;
			const bool ahead = func == "lowbeam" || func == "headlights", back = func == "brake" || func == "brakelight" || func == "taillight";
			if (!ahead && !back) continue;
			auto node = parts.nodes.find(row.items[2].text);
			if (node == parts.nodes.end()) continue;
			auto o = ToObject(frame, node->second[0], node->second[1], node->second[2]);
			std::vector<float> &list = ahead ? mesh.front_lights : mesh.back_lights;
			bool known = false;
			for (size_t i = 0; i + 2 < list.size(); i += 3) {
				if (std::abs(list[i] - o[0]) + std::abs(list[i + 1] - o[1]) + std::abs(list[i + 2] - o[2]) < 0.05f) known = true;
			}
			if (!known) list.insert(list.end(), o.begin(), o.end());
		}
	}
	Debug(misc, 1, "BeamNG {}: {} meshes ({} not in the model), {} triangles ({} in the model), {} materials, {} of glass, lights {} ahead and {} at the back",
			v.name, meshes, missing, mesh.tri.size() / 3, full, mesh.materials.size(), mesh.night_tri.size() / 3, mesh.front_lights.size() / 3, mesh.back_lights.size() / 3);
	return true;
}
