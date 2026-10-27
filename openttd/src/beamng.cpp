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

/**
 * The files of the mod's shared folder, vehicles/common/ (its own wheels, the Škoda 1203's S1203_wheels: their parts,
 * their model, their materials and textures), by their paths from the mod's root ("vehicles/common/..."), in order.
 */
std::vector<std::string> ListModCommonFiles(const BeamngVehicle &v)
{
	static constexpr std::string_view COMMON = "vehicles/common/";
	std::vector<std::string> files;
	if (!v.zip.empty()) {
		const Zip *zip = OpenZip(v.zip);
		if (zip == nullptr) return files;
		for (const ZipEntry &e : zip->entries) {
			if (e.name.size() > COMMON.size() && e.name.back() != '/' && StrEqualsIgnoreCase(std::string_view(e.name).substr(0, COMMON.size()), COMMON)) files.push_back(e.name);
		}
	} else {
		/* The mod's root: the folder above vehicles/<name>/. */
		std::string root = v.folder;
		for (int up = 0; up < 2 && root.size() > 1; up++) {
			root.pop_back();
			root.erase(root.find_last_of(PATHSEPCHAR) + 1);
		}
		std::string vehicles = FindInFolder(root, "vehicles");
		std::string common = vehicles.empty() ? std::string() : FindInFolder(vehicles + PATHSEP, "common");
		std::error_code ec;
		if (!common.empty()) {
			for (const auto &entry : std::filesystem::recursive_directory_iterator(OTTD2FS(common), ec)) {
				if (!entry.is_regular_file(ec)) continue;
				std::string rest = FS2OTTD(entry.path().lexically_relative(OTTD2FS(common)).native());
				std::replace(rest.begin(), rest.end(), '\\', '/');
				files.push_back(std::string(COMMON) + rest);
			}
		}
	}
	std::sort(files.begin(), files.end());
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

/**
 * An expression of a .jbeam ("$=..."), as BeamNG reckons it in Lua: numbers, the variables ("$name"), + - * / and
 * brackets, comparisons (== ~= != < <= > >=), nil, and case(when, then, ..., else), min, max, abs. A variable not
 * known, or anything else, is nil.
 */
class JbeamExpression {
	using Value = std::optional<double>;
	std::string_view s;
	size_t p = 0;
	const std::map<std::string, double> &vars;

	void Skip()
	{
		while (this->p < this->s.size() && (this->s[this->p] == ' ' || this->s[this->p] == '\t')) this->p++;
	}

	bool Eat(std::string_view t)
	{
		this->Skip();
		if (this->s.substr(this->p, t.size()) != t) return false;
		this->p += t.size();
		return true;
	}

	std::string Name()
	{
		this->Skip();
		const size_t from = this->p;
		while (this->p < this->s.size() && (std::isalnum(static_cast<unsigned char>(this->s[this->p])) || this->s[this->p] == '_')) this->p++;
		return std::string(this->s.substr(from, this->p - from));
	}

	Value Primary()
	{
		this->Skip();
		if (this->Eat("(")) {
			Value v = this->Compare();
			this->Eat(")");
			return v;
		}
		if (this->p < this->s.size() && this->s[this->p] == '$') {
			this->p++;
			auto it = this->vars.find("$" + this->Name());
			return it != this->vars.end() ? Value(it->second) : Value();
		}
		if (this->p < this->s.size() && (std::isdigit(static_cast<unsigned char>(this->s[this->p])) || this->s[this->p] == '.')) {
			double d = 0;
			auto [end, ec] = std::from_chars(this->s.data() + this->p, this->s.data() + this->s.size(), d);
			if (ec != std::errc()) return {};
			this->p = end - this->s.data();
			return d;
		}
		const std::string name = this->Name();
		if (name == "true") return 1;
		if (name == "false") return 0;
		if (name.empty() || !this->Eat("(")) return {};
		std::vector<Value> args;
		if (!this->Eat(")")) {
			do {
				args.push_back(this->Compare());
			} while (this->Eat(","));
			this->Eat(")");
		}
		if (name == "case") {
			size_t i = 0;
			for (; i + 1 < args.size(); i += 2) {
				if (args[i].has_value() && *args[i] != 0) return args[i + 1];
			}
			return i < args.size() ? args[i] : Value();
		}
		if ((name == "min" || name == "max") && !args.empty()) {
			Value v = args[0];
			for (const Value &a : args) {
				if (!v.has_value() || !a.has_value()) return {};
				v = name == "min" ? std::min(*v, *a) : std::max(*v, *a);
			}
			return v;
		}
		if (name == "abs" && args.size() == 1 && args[0].has_value()) return std::abs(*args[0]);
		return {};
	}

	Value Unary()
	{
		if (this->Eat("-")) {
			Value v = this->Unary();
			return v.has_value() ? Value(-*v) : v;
		}
		return this->Primary();
	}

	Value Term()
	{
		Value v = this->Unary();
		for (;;) {
			if (this->Eat("*")) {
				Value r = this->Unary();
				v = v.has_value() && r.has_value() ? Value(*v * *r) : Value();
			} else if (this->Eat("/")) {
				Value r = this->Unary();
				v = v.has_value() && r.has_value() && *r != 0 ? Value(*v / *r) : Value();
			} else {
				return v;
			}
		}
	}

	Value Sum()
	{
		Value v = this->Term();
		for (;;) {
			if (this->Eat("+")) {
				Value r = this->Term();
				v = v.has_value() && r.has_value() ? Value(*v + *r) : Value();
			} else if (this->Eat("-")) {
				Value r = this->Term();
				v = v.has_value() && r.has_value() ? Value(*v - *r) : Value();
			} else {
				return v;
			}
		}
	}

	Value Compare()
	{
		Value a = this->Sum();
		for (std::string_view op : {"==", "~=", "!=", "<=", ">=", "<", ">"}) {
			if (!this->Eat(op)) continue;
			this->Skip();
			Value b = this->s.substr(this->p, 3) == "nil" ? (this->p += 3, Value()) : this->Sum();
			const bool both = a.has_value() && b.has_value();
			bool r = false;
			if (op == "==") r = both ? *a == *b : a.has_value() == b.has_value();
			if (op == "~=" || op == "!=") r = both ? *a != *b : a.has_value() != b.has_value();
			if (op == "<=") r = both && *a <= *b;
			if (op == ">=") r = both && *a >= *b;
			if (op == "<") r = both && *a < *b;
			if (op == ">") r = both && *a > *b;
			return r ? 1 : 0;
		}
		return a;
	}

public:
	JbeamExpression(std::string_view text, const std::map<std::string, double> &variables) : s(text), vars(variables) {}

	/** Its value, nothing for nil. */
	Value Evaluate()
	{
		return this->Compare();
	}
};

/** A number of a .jbeam: a number, a variable ("$name") or an expression of them ("$=..."); def when none, or nil. */
double JbeamNumber(const JValue &v, const std::map<std::string, double> &vars, double def = 0)
{
	if (v.type == JValue::Type::Number) return v.number;
	if (!v.IsString() || v.text.empty() || v.text[0] != '$') return def;
	if (v.text.starts_with("$=")) {
		const std::optional<double> r = JbeamExpression(std::string_view(v.text).substr(2), vars).Evaluate();
		return r.has_value() ? *r : def;
	}
	auto it = vars.find(v.text);
	return it != vars.end() ? it->second : def;
}

/** Whether a value of a .jbeam is a number, or a variable or an expression that can make one (a node's place). */
bool IsJbeamNumber(const JValue &v)
{
	return v.type == JValue::Type::Number || (v.IsString() && !v.text.empty() && v.text[0] == '$');
}

/* ---- The car: its parts and figures ---- */

/** The parts of a car's .jbeam files, the main one and those its slots name, in that order. */
struct CarParts {
	std::vector<JValue> files;
	std::vector<const JValue *> used;
	std::map<std::string, std::array<double, 3>> nodes; ///< the places of the nodes of the parts used, metres in the axes of BeamNG
	std::map<std::string, double> vars;                 ///< the variables ("$name"): the configuration's, else the parts' own
	std::map<const JValue *, std::array<double, 3>> offsets; ///< how far its slots move a part used ("nodeOffset")
	std::string skin;             ///< the configuration's paint design: "globalSkin" of a part used (the Škoda 1203's VB, the slot "paint_design")
	std::vector<uint32_t> paints; ///< the configuration's own paints ("paints" of the .pc, its "baseColor"), Colour::data: the colours of its paint design
	const JValue *tarp = nullptr; ///< the tarp over a flatbed's bed: the part of the bed's slot of a roof ("..._roof") the configuration leaves empty, drawn when the game covers a load (BeamngMesh::PART_TARP)

	/** A number of the parts' (JbeamNumber()). */
	double Number(const JValue &v, double def = 0) const
	{
		return JbeamNumber(v, this->vars, def);
	}

	/**
	 * A place of a part (a node's, a flexbody's) moved as its slots move it ("nodeOffset" of the slot and those above
	 * it): along and up as it is, across away from the middle, so one offset puts both wheels of an axle out (the
	 * Škoda 1203's wheels, BeamNG's x of the slot's "trackoffset").
	 */
	std::array<double, 3> Moved(const JValue *part, double x, double y, double z) const
	{
		auto it = this->offsets.find(part);
		if (it == this->offsets.end()) return {x, y, z};
		const std::array<double, 3> &o = it->second;
		return {x + (x > 0 ? o[0] : (x < 0 ? -o[0] : 0)), y + o[1], z + o[2]};
	}
};

/** A slot of a part: its type, the part it takes by default, its options ("nodeOffset"), nullptr none. */
struct PartSlot {
	std::string type;
	std::string part;
	const JValue *options = nullptr;
};

/** The slots of a part and the part each takes by default: "slots" (type, default) or "slots2" (name, ..., default). */
std::vector<PartSlot> SlotsOf(const JValue &part)
{
	std::vector<PartSlot> out;
	for (const char *key : {"slots", "slots2"}) {
		const JValue *slots = part.Get(key);
		if (slots == nullptr || !slots->IsArray()) continue;
		const size_t at_default = std::string_view(key) == "slots2" ? 3 : 1;
		for (const JValue &row : slots->items) {
			if (!row.IsArray() || row.items.size() <= at_default || !row.items[0].IsString()) continue;
			if (row.items[0].text == "type" || row.items[0].text == "name") continue; // the heading
			const JValue *options = row.items.back().IsObject() ? &row.items.back() : nullptr;
			out.push_back({row.items[0].text, row.items[at_default].IsString() ? row.items[at_default].text : std::string(), options});
		}
	}
	return out;
}

uint32_t ColourOf(const JValue &v);

/** Read the car's .jbeam files and pick its parts: the main one, then what each slot takes (by the .pc, else by default). */
static bool IsS1203(const BeamngVehicle &v);
static void ChooseMakeParts(const BeamngVehicle &v, int make, std::map<std::string, std::string> &chosen);

/** Is it a part for a slot of the type: its "slotType" the type, or one of them. BeamNG puts no other there (the Škoda 1203's plastic masks name its round headlights for their foglights' slots). */
static bool FitsSlot(const JValue &part, const std::string &type)
{
	const JValue *t = part.Get("slotType");
	if (t == nullptr) return true;
	if (t->IsString()) return StrEqualsIgnoreCase(t->text, type);
	if (t->IsArray()) return std::ranges::any_of(t->items, [&type](const JValue &i) { return i.IsString() && StrEqualsIgnoreCase(i.text, type); });
	return true;
}

bool ReadCarParts(const BeamngVehicle &v, const JValue *info, CarParts &parts, int make = -1)
{
	std::vector<std::string> files = ListCarFiles(v);
	std::sort(files.begin(), files.end());
	std::map<std::string, std::string> chosen;
	/* The configuration: its own, else info.json's "default_pc", else the only .pc. */
	std::string pc = !v.base_config.empty() ? v.base_config + ".pc" : v.config.empty() ? std::string() : v.config + ".pc";
	if (const JValue *d = info != nullptr ? info->Get("default_pc") : nullptr; pc.empty() && d != nullptr && d->IsString()) pc = d->text + ".pc";
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
		/* Its own values of the variables (the trackoffset of the wheels). */
		if (const JValue *vars = cfg.Get("vars"); vars != nullptr && vars->IsObject()) {
			for (const auto &[name, value] : vars->members) {
				if (value.type == JValue::Type::Number) parts.vars[name] = value.number;
			}
		}
		/* Its own paints, in their order: the colours its paint design lays (the yellow and the white of the VB). */
		if (const JValue *paints = cfg.Get("paints"); paints != nullptr && paints->IsArray()) {
			for (const JValue &paint : paints->items) {
				if (const JValue *base = paint.Get("baseColor"); base != nullptr) parts.paints.push_back(ColourOf(*base));
			}
		}
	}
	ChooseMakeParts(v, make, chosen);
	for (const std::string &f : files) {
		if (EndsWithIgnoreCase(f, ".jbeam")) parts.files.push_back(ParseJson(ReadModFile(v, f)));
	}
	/* And the parts the mod shares in vehicles/common/ (the Škoda 1203's wheels), after the car's own. */
	for (const std::string &f : ListModCommonFiles(v)) {
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
	std::map<const JValue *, std::pair<const JValue *, const JValue *>> slotted; // a part: the part and the slot's options it is in
	while (!todo.empty() && parts.used.size() < 400) {
		const JValue *part = todo.front();
		todo.erase(todo.begin());
		if (!seen.insert(part).second) continue;
		parts.used.push_back(part);
		for (const PartSlot &slot : SlotsOf(*part)) {
			/* Not the load a mod puts on its bed (the wooden crate of the Škoda 1203's flatbed, "S1203_bed_cargo"): the
			 * game lays its own there, those of its cargo (FindBed()). */
			if (EndsWithIgnoreCase(slot.type, "_bed_cargo")) continue;
			auto c = chosen.find(slot.type);
			const std::string &name = c != chosen.end() ? c->second : slot.part;
			if (name.empty()) continue;
			auto it = by_name.find(name);
			if (it == by_name.end() || !FitsSlot(*it->second, slot.type)) continue;
			todo.push_back(it->second);
			slotted.emplace(it->second, std::make_pair(part, slot.options));
		}
	}
	/* Its paint design: the skin a part used names for the whole car ("globalSkin"). */
	for (const JValue *part : parts.used) {
		if (const JValue *skin = part->Get("globalSkin"); skin != nullptr && skin->IsString() && !skin->text.empty()) parts.skin = skin->text;
	}
	/* The variables: the configuration's, else the default of the part that has them ([name, type, unit, category, default, ...]). */
	for (const JValue *part : parts.used) {
		const JValue *vars = part->Get("variables");
		if (vars == nullptr || !vars->IsArray()) continue;
		for (const JValue &row : vars->items) {
			if (!row.IsArray() || row.items.size() < 5 || !row.items[0].IsString() || !row.items[0].text.starts_with("$")) continue;
			if (row.items[4].type == JValue::Type::Number) parts.vars.emplace(row.items[0].text, row.items[4].number);
		}
	}
	/* How far the slots move the parts: each by its slot's "nodeOffset", and by those above it (a part before its own in parts.used). */
	for (const JValue *part : parts.used) {
		auto from = slotted.find(part);
		if (from == slotted.end()) continue;
		std::array<double, 3> off = {0, 0, 0};
		if (auto above = parts.offsets.find(from->second.first); above != parts.offsets.end()) off = above->second;
		const JValue *node_offset = from->second.second != nullptr ? from->second.second->Get("nodeOffset") : nullptr;
		if (node_offset != nullptr && node_offset->IsObject()) {
			static const char *const AXES[3] = {"x", "y", "z"};
			for (int a = 0; a < 3; a++) {
				if (const JValue *by = node_offset->Get(AXES[a]); by != nullptr) off[a] += parts.Number(*by);
			}
		}
		if (off[0] != 0 || off[1] != 0 || off[2] != 0) parts.offsets[part] = off;
	}
	/* The tarp of a flatbed (the player, 27. 10.: "je tam valnik s plachtou, když poveze naklad ktery nemame grafiku
	 * připravenou tak plná s plachtou"): the first part made for its bed's empty slot of a roof (the Škoda 1203's
	 * S1203_roof, "Tarp Roof"), moved as its bed is. */
	for (const JValue *part : parts.used) {
		const JValue *type = part->Get("slotType");
		if (type == nullptr || !type->IsString() || !EndsWithIgnoreCase(type->text, "_bed")) continue;
		for (const PartSlot &slot : SlotsOf(*part)) {
			if (!EndsWithIgnoreCase(slot.type, "_roof")) continue;
			auto c = chosen.find(slot.type);
			if (!(c != chosen.end() ? c->second : slot.part).empty()) continue;
			for (const auto &[name, p] : by_name) {
				const JValue *t = p->Get("slotType");
				if (t == nullptr || !t->IsString() || t->text != slot.type) continue;
				parts.tarp = p;
				if (auto off = parts.offsets.find(part); off != parts.offsets.end()) parts.offsets[p] = off->second;
				break;
			}
		}
		break;
	}
	/* The nodes: a row [id, x, y, z, {...}], weighed by the "nodeWeight" set before it or in it; moved by their part's slots. */
	for (const JValue *part : parts.used) {
		const JValue *nodes = part->Get("nodes");
		if (nodes == nullptr || !nodes->IsArray()) continue;
		for (const JValue &row : nodes->items) {
			if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString() || !IsJbeamNumber(row.items[1])) continue;
			parts.nodes[row.items[0].text] = parts.Moved(part, parts.Number(row.items[1]), parts.Number(row.items[2]), parts.Number(row.items[3]));
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

/**
 * The cargoes of our Škoda 1203 with a tarp (the colleague's osobacky/plachta_naklady.json, TAZ 1203 plachta 0x91 of
 * the vans): the vans and the flatbeds of BeamNG carry them (the player, 26. 10.: "give them their cargoes as the
 * flatbed and the van, you have our 1203s for a pattern"), mail first, as theirs.
 */
static const std::vector<std::string> TARP_1203_CARGOES = {
	"PASS", "BEER", "MAIL", "BDMT", "FOOD", "FARM", "FRUT", "LVPT", "DYES", "HOPS", "TEXT", "GLAS", "WDPR", "WOOL",
	"FISH", "PAPR", "SUGR", "NUTS", "JAVA", "COAT", "STEL", "TYRE", "STST", "PIPE", "STSE", "STSH", "STWR", "STAL",
	"ALUM", "STCB", "CMNT", "CSTI", "VENG", "POWR", "CIGR", "TBCO", "COLA", "BEAN", "FURN", "TATO", "ELEC", "NODC",
	"FZDR", "SWET", "CTCD", "SOAP", "MEAT", "SEED", "OLSD", "CHSE", "CERA", "BUBL", "TOFF", "CCPR", "BRCK", "STBL",
	"FOCA", "PPWK", "RBAR", "SEAL", "STPP", "STTB", "TYCO", "WELD", "SESP", "LVST", "PUMP", "CBLK", "COPR", "FECR",
	"FERT", "KAOL", "MILK", "PLAS", "RUBR", "ZINC", "BAKE", "COBL", "EOIL", "FRVG", "IRON", "METL", "NICK", "OYST",
	"PCL_", "QLME", "SALT", "SASH", "PLST", "YETI", "YETY", "RSGR", "MARI", "STIG", "STSL", "STBR", "STPL", "STSW",
	"FEAL", "PLNT", "CHEM", "WORK", "PRIS", "ENUM", "MPAR", "PPAR", "RAMT", "TINP", "LEAT", "PRNT", "WINE", "FLOU",
	"LETH", "CRAN", "LFEQ", "SCPR", "STTP", "SWRP", "TIN_", "WDCH", "BRAM"
};
/** The people among them: none on a flatbed (they sit in the cab). */
static const std::vector<std::string> PEOPLE_CARGOES = {"PASS", "WORK", "PRIS", "YETI", "YETY"};

/**
 * Our Škoda 1203 over the years (the player, 26. 10.: "the Škoda 1203, then the TAZ 1203 and at last the TAZ 1500 and
 * the 1.9 D", "make it nice when you write it for the 1203 alone"): made at Vrchlabí from 1968, then at Trnava from
 * 1981, the TAZ 1500 with the 1433 cm³ engine from 1993 and the diesel of VW from 1996. Each configuration a car of
 * each make; the model the same, the power its make's. None of them is ever retired (the player: "the production
 * never stops").
 */
struct BeamngMake {
	std::string_view name;
	int from;       ///< the year it comes
	double kw;      ///< its engine's power
};
static constexpr BeamngMake S1203_MAKES[] = {
	{"Škoda 1203", 1968, 35}, // 1.2 l, 47 k
	{"TAZ 1203", 1981, 35},
	{"TAZ 1500", 1993, 40},   // 1433 cm³
	{"TAZ 1.9D", 1996, 47},   // the diesel of VW, 1.9 l, 64 k
};
/** What each configuration of the 1203 is, in its name. */
static constexpr std::pair<std::string_view, std::string_view> S1203_KINDS[] = {
	{"police2", "VB žlutá"}, {"police", "VB"}, {"ambulance", "vojenská sanitka"}, {"ambulance2", "sanitka"}, {"hearse", "pohřební"}, {"flatbed", "valník"},
	{"van", "dodávka"}, {"com", "kombi"}, {"passenger", "mikrobus"}, {"lux", "De Luxe"}, {"spoje", "Spoje"}, {"fire", "hasiči"},
	{"pajda", "Pajda karavan"},
};
/**
 * The configurations of our 1203 the game makes of the mod's own (BeamngVehicle::base_config): the Pajda karavan (the
 * player, 27. 10.: "one of the first 1203s, Škoda from Vrchlabí, round lights", "pajda karavan") of the bus, the body
 * of 1968; a Škoda only, of the petrol blue of the old 2D GRF.
 */
static constexpr std::pair<std::string_view, std::string_view> S1203_OWN_CONFIGS[] = {
	{"pajda", "passenger"},
};

/** Is it our Škoda 1203: the folder "S1203" of its mod. */
static bool IsS1203(const BeamngVehicle &v)
{
	return StrEqualsIgnoreCase(v.id, "S1203");
}

/**
 * The front of a make of our 1203 (the player, 27. 10.: "the 1203s have square lights, but those are no Škodas any
 * more, that is TAZ", "the 1500 has round ones on some, that is a mistake"): the Škoda its chrome mask with round
 * headlights and indicators, its radiator the Škoda's; the TAZs their plastic mask of TAZ with square ones and its
 * letters, the TAZ radiator. Whatever the configuration had there -- but the De Luxe's four lamps and a flatbed's
 * heavy radiator, its own.
 */
static void ChooseMakeParts(const BeamngVehicle &v, int make, std::map<std::string, std::string> &chosen)
{
	if (make < 0 || !IsS1203(v)) return;
	if (auto mask = chosen.find("S1203_mask"); mask != chosen.end() && mask->second == "S1203_mask_quad") return;
	const bool skoda = make == 0;
	if (skoda) {
		chosen["S1203_mask"] = "S1203_mask";
		for (const char *slot : {"S1203_headlight_L", "S1203_headlight_R", "S1203_indicator_FL", "S1203_indicator_FR"}) chosen[slot] = slot;
	} else {
		chosen["S1203_mask"] = "S1203_mask_new_taz";
		for (const char *slot : {"S1203_headlight_new_L", "S1203_headlight_new_R", "S1203_indicator_new_FL", "S1203_indicator_new_FR"}) chosen[slot] = slot;
		chosen["S1203_mask_lettering"] = "S1203_lettering_TAZ-S1203";
	}
	if (auto radiator = chosen.find("S1203_radiator"); radiator == chosen.end() || radiator->second != "S1203_radiator_super") {
		chosen["S1203_radiator"] = skoda ? "S1203_radiator" : "S1203_radiator_taz";
	}
}

/**
 * The cars of the purchase list made of a configuration (their numbers in the GRF come later, FindBeamngVehicles()):
 * the car itself from the first year of its info.json, or our 1203 as each of its makes.
 */
static void MakeModels(BeamngVehicle &v, const std::string &own_config)
{
	v.models.clear();
	if (!IsS1203(v)) {
		BeamngModel m;
		m.name = v.name;
		m.year_from = v.year_from;
		m.power_kw = v.power_kw;
		m.speed_kmh = v.speed_kmh;
		v.models.push_back(std::move(m));
		return;
	}
	std::string kind = own_config;
	for (const auto &[config, name] : S1203_KINDS) {
		if (StrEqualsIgnoreCase(own_config, config)) {
			kind = name;
			break;
		}
	}
	const bool skoda_only = !v.base_config.empty();
	for (const BeamngMake &make : S1203_MAKES) {
		if (skoda_only && &make != &S1203_MAKES[0]) break;
		BeamngModel m;
		m.make = static_cast<int>(&make - S1203_MAKES);
		m.name = std::string(make.name) + (kind.empty() ? std::string() : " " + kind);
		/* From its make's year, or its own first one when later (the De Luxe from 1969). */
		m.year_from = std::max(make.from, v.year_from);
		m.power_kw = make.kw;
		/* As fast as the mod's car, by the third root of the power against the air. */
		m.speed_kmh = v.power_kw > 0 ? v.speed_kmh * std::cbrt(make.kw / v.power_kw) : v.speed_kmh;
		v.models.push_back(std::move(m));
	}
}

/**
 * The paints of our 1203 to choose from in the configurator (the player, 27. 10.: "the colours of my old 1203 GRF, the
 * 2D one, the sprites", "red, yellow, blue", "the tarp grey, yellow"): as the old VWT1-S1203modradodavka.grf has
 * them, each configuration first the one it had there -- the flatbed red with a yellow tarp ("a wooden flatbed, a tarp
 * could go on, a car of three colours, the body red, a nicely coloured car"), the bus yellow, the van blue; the kombi,
 * the car itself, red. The designs (VB, the ambulances, Spoje, the firemen), the hearse and the De Luxe in two colours
 * keep their own.
 */
static void ChoosePaints(BeamngVehicle &v, const std::string &own_config)
{
	if (!IsS1203(v)) return;
	static const BeamngVehicle::Choice RED{"červená", 0xFFBF5B55}, YELLOW{"žlutá", 0xFFC4A64C}, BLUE{"modrá", 0xFF3A7DA6};
	if (StrEqualsIgnoreCase(own_config, "flatbed") || StrEqualsIgnoreCase(own_config, "com")) {
		v.body_paints = {RED, YELLOW, BLUE};
	} else if (StrEqualsIgnoreCase(own_config, "passenger")) {
		v.body_paints = {YELLOW, RED, BLUE};
	} else if (StrEqualsIgnoreCase(own_config, "van")) {
		v.body_paints = {BLUE, RED, YELLOW};
	} else if (StrEqualsIgnoreCase(own_config, "pajda")) {
		v.body_paints = {{"petrolejová", 0xFF4B7F99}, RED, YELLOW, BLUE};
	} else {
		return;
	}
	if (v.has_tarp) v.tarp_paints = {{"žlutá", 0xFFA99A3B}, {"šedá", 0xFFA5A396}};
	/* The wooden flatbed ("dřevěnej valník") brown as the old one's, whatever the body. */
	if (v.has_bed) v.bed_paint = 0xFF604225;
}

/**
 * A car of its folder in the mod, if it is one with a model and an engine.
 * @param config its configuration, a .pc of the folder without ".pc"; empty the default one
 */
/**
 * The bed of a flatbed, where the game lays the pieces of its shared loads (the player, 27. 10.: "ingame naklady dej na
 * valniky 1203 1500"): the nodes of the part in the slot of its bed ("..._bed", the Škoda 1203's S1203_bed, its floor
 * and its gates). Its floor the height most of them are at, from its back to its front and from side to side as far
 * as those of the floor go, its sides as high as the highest of them over it.
 */
static void FindBed(const CarParts &parts, const CarFrame &frame, BeamngVehicle &v)
{
	static constexpr double FLOOR_M = 0.03; ///< how far a node of the floor may be from its height
	for (const JValue *part : parts.used) {
		const JValue *type = part->Get("slotType");
		if (type == nullptr || !type->IsString() || !EndsWithIgnoreCase(type->text, "_bed")) continue;
		const JValue *nodes = part->Get("nodes");
		if (nodes == nullptr || !nodes->IsArray()) continue;
		std::vector<std::array<double, 3>> at;
		for (const JValue &row : nodes->items) {
			if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString()) continue;
			if (auto n = parts.nodes.find(row.items[0].text); n != parts.nodes.end()) at.push_back(n->second);
		}
		double floor = 0;
		size_t most = 0;
		for (const auto &a : at) {
			const size_t near = std::ranges::count_if(at, [&a](const auto &b) { return std::abs(b[2] - a[2]) <= FLOOR_M; });
			if (near > most) {
				most = near;
				floor = a[2];
			}
		}
		if (most < 4) continue;
		double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, top = floor;
		for (const auto &a : at) {
			top = std::max(top, a[2]);
			if (std::abs(a[2] - floor) > FLOOR_M) continue;
			x0 = std::min(x0, a[0]);
			x1 = std::max(x1, a[0]);
			y0 = std::min(y0, a[1]);
			y1 = std::max(y1, a[1]);
		}
		if (x1 - x0 < 0.5 || y1 - y0 < 0.5) continue;
		/* BeamNG's y backwards is the game's x forward, its x to the left the game's y. */
		const std::array<float, 3> back = ToObject(frame, x0, y1, floor), front = ToObject(frame, x1, y0, floor);
		v.bed_x[0] = std::min(back[0], front[0]);
		v.bed_x[1] = std::max(back[0], front[0]);
		v.bed_y[0] = std::min(back[1], front[1]);
		v.bed_y[1] = std::max(back[1], front[1]);
		v.bed_floor = back[2];
		v.bed_sides = static_cast<float>(top - floor);
		/* No load higher than its tarp goes (a 1203 does not carry its bales two high over its cab). */
		if (const JValue *tarp_nodes = parts.tarp != nullptr ? parts.tarp->Get("nodes") : nullptr; tarp_nodes != nullptr && tarp_nodes->IsArray()) {
			double high = floor;
			for (const JValue &row : tarp_nodes->items) {
				if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString() || !IsJbeamNumber(row.items[1])) continue;
				high = std::max(high, parts.Moved(parts.tarp, parts.Number(row.items[1]), parts.Number(row.items[2]), parts.Number(row.items[3]))[2]);
			}
			if (high > floor + 0.3) v.bed_top = ToObject(frame, 0, 0, high)[2];
		}
		v.has_bed = true;
		return;
	}
}

std::optional<BeamngVehicle> ReadCar(const std::string &zip, const std::string &folder, const std::string &id, const std::string &config = {})
{
	BeamngVehicle v;
	v.zip = zip;
	v.folder = folder;
	v.id = id;
	v.config = config;
	if (StrEqualsIgnoreCase(id, "S1203")) {
		for (const auto &[own, base] : S1203_OWN_CONFIGS) {
			if (StrEqualsIgnoreCase(config, own)) v.base_config = base;
		}
	}
	JValue info = ParseJson(ReadModFile(v, "info.json"));
	/* Its configuration's name: its own, or for the car itself info.json's "default_pc". */
	std::string own_config = config;
	if (const JValue *d = info.Get("default_pc"); own_config.empty() && d != nullptr && d->IsString()) own_config = d->text;
	/* The configuration's own info_<name>.json over the car's: its name ("Configuration"), its years, its paint, its body. */
	if (!own_config.empty() && info.IsObject()) {
		JValue own = ParseJson(ReadModFile(v, "info_" + (v.base_config.empty() ? own_config : v.base_config) + ".json"));
		if (own.IsObject()) {
			for (auto &[k, val] : own.members) {
				auto it = std::ranges::find_if(info.members, [&k](const auto &m) { return m.first == k; });
				if (it != info.members.end()) {
					it->second = val;
				} else {
					info.members.emplace_back(k, val);
				}
			}
		}
	}
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
	if (!config.empty()) {
		const JValue *c = info.Get("Configuration");
		v.name += " " + (v.base_config.empty() && c != nullptr && c->IsString() && !c->text.empty() ? c->text : config);
	}
	if (const JValue *a = info.Get("Author"); a != nullptr && a->IsString()) v.author = a->text;
	if (const JValue *y = info.Get("Years"); y != nullptr && y->IsObject()) {
		if (const JValue *lo = y->Get("min"); lo != nullptr) v.year_from = static_cast<int>(lo->Num());
	}
	const JValue *type = info.Get("Type");
	if (type != nullptr && type->IsString() && (StrEqualsIgnoreCase(type->text, "Prop") || StrEqualsIgnoreCase(type->text, "Trailer"))) return std::nullopt;

	/* The paints: "colors" (names to "r g b a") with "default_color", or "paints" (names to {baseColor}) with "defaultPaintName1". */
	std::string def;
	if (const JValue *d = info.Get("default_color"); d != nullptr && d->IsString()) def = d->text;
	if (const JValue *d = info.Get("defaultPaintName1"); d != nullptr && d->IsString()) def = d->text;
	std::vector<std::pair<std::string, uint32_t>> named;
	if (const JValue *c = info.Get("colors"); c != nullptr && c->IsObject()) {
		for (const auto &[n, col] : c->members) named.emplace_back(n, ColourOf(col));
	}
	if (const JValue *c = info.Get("paints"); c != nullptr && c->IsObject()) {
		for (const auto &[n, paint] : c->members) {
			if (const JValue *base = paint.Get("baseColor"); base != nullptr) named.emplace_back(n, ColourOf(*base));
		}
	}
	/* The default first, by its name, else the first whose name has it (the player, 27. 10., "proč je cely auto zeleny
	 * vždycky": the 1203's flatbed's "Blue" is none of its paints, which begin with "Aquamarine"; its "Sky Blue" is). */
	auto def_paint = std::ranges::find_if(named, [&def](const auto &p) { return p.first == def; });
	if (def_paint == named.end() && !def.empty()) def_paint = std::ranges::find_if(named, [&def](const auto &p) { return StrContainsIgnoreCase(p.first, def); });
	if (def_paint != named.end()) std::rotate(named.begin(), def_paint, def_paint + 1);
	for (const auto &[n, colour] : named) v.paints.push_back(colour);
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
				if (!row.IsArray() || row.items.size() < 4 || !row.items[0].IsString() || !IsJbeamNumber(row.items[1])) continue;
				double w = weight;
				for (size_t i = 4; i < row.items.size(); i++) {
					if (const JValue *own = row.items[i].Get("nodeWeight"); own != nullptr && own->type == JValue::Type::Number) w = own->number;
				}
				mass += w;
				const auto [x, y, z] = parts.Moved(part, parts.Number(row.items[1]), parts.Number(row.items[2]), parts.Number(row.items[3]));
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
	_frames[v.Package() + "|" + v.folder + "|" + v.config] = frame;

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
	/*
	 * Its cargoes by what it is (the player, 26. 10., of the 1203's configurations): a police car ("VB") or an ambulance
	 * one passenger or one studentka; a passenger van its people; a flatbed and a van the cargoes of our 1203 with a
	 * tarp, the flatbed without people. Any other car every cargo of the game (the player: "give the BeamNG cars all
	 * the cargoes in the game"). The post's van (the player, 27. 10.: "spoje to je pošta ceniny") the mail and the
	 * valuables of each climate. The army's ambulance (the khaki "ambulance" of the 1203, the yellow is "ambulance2")
	 * as the other ("to asi nebudem vozit vybušniny sanitkou").
	 */
	std::string what = own_config + " " + v.base_config + " ";
	if (const JValue *c = info.Get("Configuration"); c != nullptr && c->IsString()) what += c->text;
	strtolower(what);
	auto has = [&what](std::initializer_list<std::string_view> words) { return std::ranges::any_of(words, [&what](std::string_view w) { return what.find(w) != std::string::npos; }); };
	if (has({"spoje"})) {
		v.cargoes = {"MAIL", "VALU", "GOLD", "DIAM"};
		v.goods = true;
	} else if (has({"police", "vb", "ambulance", "sanit"})) {
		v.cargoes = {"PASS", "STUD"};
		v.capacity = 1;
		v.goods = false;
	} else if (has({"passenger", "lux", "bus", "osob"})) {
		v.cargoes = {"PASS", "STUD"};
		v.capacity = std::max(v.capacity, 8);
		v.goods = false;
	} else if (has({"flatbed", "pickup", "valnik", "valník"}) || style.find("pickup") != std::string::npos || style.find("flatbed") != std::string::npos) {
		for (const std::string &c : TARP_1203_CARGOES) {
			if (std::ranges::find(PEOPLE_CARGOES, c) == PEOPLE_CARGOES.end()) v.cargoes.push_back(c);
		}
		v.goods = true;
		FindBed(parts, frame, v);
		v.has_tarp = v.has_bed && parts.tarp != nullptr;
	} else if (style.find("van") != std::string::npos || has({"van", "com", "hearse", "fire", "dodav"})) {
		v.cargoes = TARP_1203_CARGOES;
		v.goods = true;
	}
	/* Mail first, as our 1203's. */
	if (auto mail = std::ranges::find(v.cargoes, std::string("MAIL")); mail != v.cargoes.end()) std::rotate(v.cargoes.begin(), mail, mail + 1);

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
	/* One GRF a folder, of the id it always had: the games saved with the car keep it. */
	v.grfid = GrfIdOf(zip_name + "/" + id);
	MakeModels(v, own_config);
	ChoosePaints(v, own_config);
	return v;
}

/**
 * The configurations of a car's folder other than its default one, each a car of its own (the player, test 4: the
 * 1203 "has many variants... we need the VB, and the flatbed and the vans"): its .pc files but info.json's
 * "default_pc" (the car itself), by their names.
 */
std::vector<std::string> OtherConfigs(const std::string &zip, const std::string &folder, const std::string &id)
{
	BeamngVehicle v;
	v.zip = zip;
	v.folder = folder;
	v.id = id;
	JValue info = ParseJson(ReadModFile(v, "info.json"));
	std::string def;
	if (const JValue *d = info.IsObject() ? info.Get("default_pc") : nullptr; d != nullptr && d->IsString()) def = d->text;
	std::vector<std::string> files = ListCarFiles(v);
	std::sort(files.begin(), files.end());
	std::vector<std::string> out;
	for (const std::string &f : files) {
		if (!EndsWithIgnoreCase(f, ".pc")) continue;
		const std::string name = f.substr(0, f.size() - 3);
		/* Without "default_pc" the car itself is its first .pc, as ReadCarParts takes it. */
		if (def.empty()) {
			def = name;
			continue;
		}
		if (!StrEqualsIgnoreCase(name, def)) out.push_back(name);
	}
	return out;
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

/** The days from 1920 to the first of January of a year, as a GRF gives the introduction of a vehicle. */
static uint32_t DaysSince1920(int year)
{
	uint32_t days = 0;
	for (int y = 1920; y < year; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
	return days;
}

/**
 * The bytes of a NewGRF of a car's folder (container version 1, GRF version 8: Actions 8, 0 and 4, no sprites): a
 * road vehicle for each car of the purchase list of each of its configurations (BeamngVehicle::models).
 * @param cars the folder's configurations, the car itself first
 */
std::vector<uint8_t> MakeGrf(const std::vector<const BeamngVehicle *> &cars)
{
	const BeamngVehicle &first = *cars.front();
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
	/* A number as an extended byte: itself, or 0xFF and a word. */
	auto ext = [&word](std::vector<uint8_t> &b, uint16_t x) {
		if (x < 0xFF) {
			b.push_back(static_cast<uint8_t>(x));
		} else {
			b.push_back(0xFF);
			word(b, x);
		}
	};

	{
		std::vector<uint8_t> a8 = {0x08, 0x08};
		dword(a8, first.grfid);
		text(a8, "BeamNG: " + first.name);
		std::string author = first.author.empty() ? std::string() : ", autor " + first.author;
		text(a8, "Auto z modu BeamNG.drive (" + first.id + author + "). Model a textury patří autorům modu a zůstávají ve své složce: jen pro tvou hru, nešířit.");
		sprites.push_back(std::move(a8));
	}

	/* The cargoes of all of them by their labels: one translation table (global property 9), each refit list by its numbers. */
	std::vector<std::string> labels;
	for (const BeamngVehicle *v : cars) {
		for (const std::string &c : v->cargoes) {
			if (std::ranges::find(labels, c) == labels.end()) labels.push_back(c);
		}
	}
	if (labels.size() > 255) labels.resize(255);
	if (!labels.empty()) {
		std::vector<uint8_t> ctt = {0x00, 0x08, 0x01, static_cast<uint8_t>(labels.size()), 0x00, 0x09};
		for (const std::string &l : labels) {
			for (int k = 0; k < 4; k++) ctt.push_back(k < static_cast<int>(l.size()) ? static_cast<uint8_t>(l[k]) : ' ');
		}
		sprites.push_back(std::move(ctt));
	}

	/*
	 * The purchase list folds the cars of a make under its first one, the car itself's where it has the make (the
	 * player, 26. 10.: "the make is to unfold into the van, the flatbed"): the player keeps the vehicles that never
	 * expire, so the old makes stay there beside the new.
	 */
	std::map<int, uint16_t> make_first;
	for (const BeamngVehicle *v : cars) {
		for (const BeamngModel &m : v->models) {
			if (m.make >= 0) make_first.try_emplace(m.make, m.id);
		}
	}

	for (const BeamngVehicle *car : cars) {
		const BeamngVehicle &v = *car;
		std::vector<uint8_t> refits;
		for (const std::string &c : v.cargoes) {
			auto at = std::ranges::find(labels, c);
			if (at != labels.end()) refits.push_back(static_cast<uint8_t>(at - labels.begin()));
		}
		for (const BeamngModel &m : v.models) {
			const double hp = m.power_kw / 0.7457;
			/* Prices by the original game's bus (MPS Regal: 90 hp, cost 120, running 91), a car of the same power less. */
			const double rel = std::max(hp, 20.0) / 90.0;
			const uint8_t cost = static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround((v.goods || v.capacity > 4 ? 100 : 50) * std::pow(rel, 0.45))), 8, 255));
			const uint8_t running = static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround((v.goods || v.capacity > 4 ? 80 : 40) * std::pow(rel, 0.4))), 8, 255));
			std::vector<uint8_t> a0 = {0x00, 0x01, 0, 0x01};
			ext(a0, m.id);
			const size_t count_at = 2;
			uint8_t props = 0;
			auto prop = [&](uint8_t p) { a0.push_back(p); props++; };
			/* It comes in its first year (the player, 26. 10.: "if you have the year of BeamNG, use it") and is never
			 * retired ("the production never stops"). */
			prop(0x00); word(a0, m.year_from > 1920 ? std::min<uint32_t>(DaysSince1920(m.year_from), 0xFFFF) : 0);
			prop(0x02); a0.push_back(20);                  // reliability decay
			prop(0x03); a0.push_back(15);                  // a vehicle's life, years
			prop(0x04); a0.push_back(0xFF);                // the model never retires
			prop(0x06); a0.push_back(0x0F);                // every climate
			prop(0x07); a0.push_back(5);                   // loading speed
			prop(0x0E); a0.push_back(0xFF);                // the set's own sprites: drawn from its model (GetThreeDVehicleObjectSprite())
			prop(0x0F); a0.push_back(static_cast<uint8_t>(v.capacity));
			if (!refits.empty()) {
				prop(0x10); a0.push_back(refits.front());  // the first of its list, its own
				prop(0x1D); word(a0, 0x0000);              // no classes: its list alone
				prop(0x1E); word(a0, 0x0000);
				prop(0x24); a0.push_back(static_cast<uint8_t>(refits.size()));
				a0.insert(a0.end(), refits.begin(), refits.end());
			} else {
				prop(0x10); a0.push_back(v.goods ? 0xFF : 0x00); // passengers, or the first goods it takes
				/* Refitted to every cargo the game has (the player, 26. 10.: "give the BeamNG cars all the cargoes in the game"). */
				prop(0x1D); word(a0, 0xFFFF);              // the classes of cargo it may be refitted to: all
				prop(0x1E); word(a0, 0x8000);              // but the road vehicles (CT_ROLA, special): the player's car carriers only
			}
			prop(0x11); a0.push_back(cost);
			prop(0x09); a0.push_back(running);
			prop(0x13); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(hp / 10)), 1, 255)));       // power, 10 hp
			prop(0x14); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(v.mass_t * 4)), 1, 255)));  // weight, 1/4 t
			prop(0x15); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(m.speed_kmh / 2.0117)), 1, 255))); // speed, 1.25 mph
			prop(0x18); a0.push_back(static_cast<uint8_t>(Clamp<int>(static_cast<int>(std::lround(v.effort * 255)), 1, 255)));  // tractive effort
			if (auto first_of_make = make_first.find(m.make); first_of_make != make_first.end() && first_of_make->second != m.id) {
				prop(0x26); word(a0, first_of_make->second); // under the first of its make
			}
			a0[count_at] = props;
			sprites.push_back(std::move(a0));

			std::vector<uint8_t> a4 = {0x04, 0x01, 0x7F, 0x01};
			ext(a4, m.id);
			text(a4, m.name);
			sprites.push_back(std::move(a4));
		}
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
	double unit = 1;    ///< its .dae's metres to a unit
	bool y_up = false;  ///< its .dae's up is y
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

/** How much of what is behind the glass of a car hides where its material has no "opacityMap" (BeamngMesh::Material::alpha). */
static constexpr uint8_t GLASS_WITHOUT_MAP = 77;

/** What the game takes of a material of BeamNG. */
struct CarMaterial {
	uint32_t colour = 0xFFB4B4B4; ///< "diffuseColor" (or "baseColorFactor") of its first layer
	std::string texture;          ///< "diffuseMap" (or "colorMap", "baseColorMap") of its first layer
	bool paint = false;           ///< "instanceDiffuse": the car's paint
	std::string mask;             ///< "opacityMap" on the second texture coordinates: holes where it is dark
	bool mask_uv1 = false;
	std::string paint_mask;       ///< the paint's layer's "opacityMap": the paint where it is light, the first layer's picture where dark
	bool paint_mask_uv1 = false;
	std::string palette;          ///< the paint's layer's "colorPaletteMap": which of the paints where, red the first, green the second, blue the third (a paint design's)
	std::string decal;            ///< a layer after the paint's with a picture of its own ("baseColorMap"): laid over the paint, the VB's letters ...
	std::string decal_mask;       ///< ... where its "opacityMap" is light
	bool see_through = false;     ///< "translucent" and not "alphaTest", blended ("translucentBlendOp" not "None": the Škoda 1203's tyres are not) -- the glass: its "opacityMap" how much hides what is behind it, not holes
	bool both_sides = false;      ///< "doubleSided": seen from behind too; BeamNG leaves out the side of the others facing away
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
	/* The car's own, then those the mod shares in vehicles/common/ (its wheels'); a name the car's own if both have it. */
	std::vector<std::string> files = ListCarFiles(v);
	const size_t own = files.size();
	for (const std::string &f : ListModCommonFiles(v)) {
		if (EndsWithIgnoreCase(f, ".cs") || EndsWithIgnoreCase(f, "materials.json")) files.push_back(f);
	}
	for (size_t at = 0; at < files.size(); at++) {
		const std::string &f = files[at];
		const bool shared = at >= own;
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
				auto yes = [&get](const std::string &k) { const std::string val = get(k); return val == "true" || val == "1"; };
				m.see_through = yes("translucent") && !yes("alphaTest") && get("translucentBlendOp") != "None";
				m.both_sides = yes("doubleSided");
				std::string map_to = get("mapTo");
				const std::string key = map_to.empty() ? name : map_to;
				if (shared) {
					out.emplace(key, m);
				} else {
					out[key] = m;
				}
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
					/* The first's BeamNG's empty picture ("common/null.dds"), the picture of a layer after it (the
					 * Škoda 1203's tyre: its rubber in the second). */
					if (m.texture.empty() || m.texture.find("common/null") != std::string::npos) {
						for (const JValue &st : stages->items) {
							for (const char *k : {"diffuseMap", "colorMap", "baseColorMap"}) {
								const JValue *t = st.Get(k);
								if (t != nullptr && t->IsString() && t->text.find("common/null") == std::string::npos && (m.texture.empty() || m.texture.find("common/null") != std::string::npos)) m.texture = t->text;
							}
						}
					}
					if (const JValue *t = first->Get("opacityMap"); t != nullptr && t->IsString()) m.mask = t->text;
					if (const JValue *t = first->Get("opacityMapUseUV"); t != nullptr) m.mask_uv1 = t->Num() == 1;
				}
				if (stages != nullptr && stages->IsArray()) {
					auto painted = [](const JValue &st) {
						for (const char *k : {"instanceDiffuse", "useColorPalette"}) {
							if (const JValue *b = st.Get(k); b != nullptr && b->IsTrue()) return true;
						}
						return false;
					};
					auto of_palette = [](const JValue &st) {
						const JValue *t = st.Get("colorPaletteMap");
						return (t != nullptr && t->IsString()) || (st.Get("useColorPalette") != nullptr && st.Get("useColorPalette")->IsTrue());
					};
					/*
					 * The paint's layer: the painted one of a palette, else any painted (the tarp: its whole picture in the
					 * paint). The Škoda 1203's inside, extras and gear stick have their first layer painted too, but the
					 * paint only where the mask of their palette's layer is light, the metal (the player, 27. 10.: "it looks
					 * silly when you see inside and inside all is blue").
					 */
					const JValue *paint_layer = nullptr;
					for (const JValue &st : stages->items) {
						if (painted(st) && of_palette(st)) {
							paint_layer = &st;
							break;
						}
					}
					for (const JValue &st : stages->items) {
						if (paint_layer == nullptr && painted(st)) paint_layer = &st;
					}
					if (paint_layer != nullptr) {
						m.paint = true;
						/* The paint a layer over the first, where its mask is light (the Škoda 1203's body: its rubber, chrome
						 * and lamps in the first layer's picture, dark in the mask). */
						if (paint_layer != first) {
							if (const JValue *t = paint_layer->Get("opacityMap"); t != nullptr && t->IsString()) m.paint_mask = t->text;
							if (const JValue *t = paint_layer->Get("opacityMapUseUV"); t != nullptr) m.paint_mask_uv1 = t->Num() == 1;
							if (const JValue *t = paint_layer->Get("colorPaletteMap"); t != nullptr && t->IsString()) m.palette = t->text;
						}
						/* A painted layer after the paint's with a picture and a mask of its own: a paint design's letters over it. */
						for (const JValue *st = paint_layer + 1; st < stages->items.data() + stages->items.size() && m.decal.empty(); st++) {
							const JValue *t = st->Get("baseColorMap"), *o = st->Get("opacityMap");
							if (!painted(*st) || t == nullptr || !t->IsString() || o == nullptr || !o->IsString()) continue;
							m.decal = t->text;
							m.decal_mask = o->text;
						}
					}
				}
				const JValue *translucent = mat.Get("translucent"), *alpha_test = mat.Get("alphaTest"), *blend = mat.Get("translucentBlendOp");
				/* Translucent without blending ("translucentBlendOp": "None") is drawn whole (the tyres of the Škoda 1203,
				 * the player: "the tyres are see-through"). */
				const bool blended = blend == nullptr || !blend->IsString() || blend->text != "None";
				m.see_through = translucent != nullptr && translucent->IsTrue() && !(alpha_test != nullptr && alpha_test->IsTrue()) && blended;
				const JValue *both = mat.Get("doubleSided");
				m.both_sides = both != nullptr && both->IsTrue();
				std::string map_to = mat.Get("mapTo") != nullptr && mat.Get("mapTo")->IsString() ? mat.Get("mapTo")->text : name;
				if (shared) {
					out.emplace(map_to, m);
				} else {
					out[map_to] = m;
				}
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

/*
 * BC7 (BeamNG's own textures, the Škoda 1203's: DXGI 98 and 99): a block of 16 bytes, one of eight modes -- one to
 * three subsets of its pixels, each between two endpoints, its pixels' indices weighing them. The tables of the
 * partitions and of the subsets' anchor pixels are the format's (Microsoft's "BC7 Format Mode Reference").
 */

/** The subset of each pixel of a block of two subsets, by its partition: bit n the pixel n. */
static const uint16_t BC7_PARTITIONS_2[64] = {
	0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
	0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
	0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
	0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
	0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
	0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
	0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
	0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
/** Of three subsets: two bits a pixel. */
static const uint32_t BC7_PARTITIONS_3[64] = {
	0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
	0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
	0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
	0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
	0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
	0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
	0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
	0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
};
/** The anchor pixel of the second subset of two, by the partition (the first subset's is pixel 0). */
static const uint8_t BC7_ANCHORS_2[64] = {
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
	15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
	6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
/** The anchor pixels of the second and the third subset of three. */
static const uint8_t BC7_ANCHORS_3[64][2] = {
	{3, 15}, {3, 8}, {15, 8}, {15, 3}, {8, 15}, {3, 15}, {15, 3}, {15, 8},
	{8, 15}, {8, 15}, {6, 15}, {6, 15}, {6, 15}, {5, 15}, {3, 15}, {3, 8},
	{3, 15}, {3, 8}, {8, 15}, {15, 3}, {3, 15}, {3, 8}, {6, 15}, {10, 8},
	{5, 3}, {8, 15}, {8, 6}, {6, 10}, {8, 15}, {5, 15}, {15, 10}, {15, 8},
	{8, 15}, {15, 3}, {3, 15}, {5, 10}, {6, 10}, {10, 8}, {8, 9}, {15, 10},
	{15, 6}, {3, 15}, {15, 8}, {5, 15}, {15, 3}, {15, 6}, {15, 6}, {15, 8},
	{3, 15}, {15, 3}, {5, 15}, {5, 15}, {5, 15}, {8, 15}, {5, 15}, {10, 15},
	{5, 15}, {10, 15}, {8, 15}, {13, 15}, {15, 3}, {12, 15}, {3, 15}, {3, 8},
};

/** A block of BC7 as 16 pixels of RGBA, row by row. */
static void Bc7Block(const uint8_t *b, uint8_t out[16][4])
{
	struct Mode {
		uint8_t subsets, partition_bits, rotation_bits, selection_bits, colour_bits, alpha_bits, endpoint_pbits, shared_pbits, index_bits, index2_bits;
	};
	static const Mode MODES[8] = {
		{3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
		{2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
		{3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
		{2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
		{1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
		{1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
		{1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
		{2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
	};
	static const uint8_t WEIGHTS_2[4] = {0, 21, 43, 64};
	static const uint8_t WEIGHTS_3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
	static const uint8_t WEIGHTS_4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
	int at = 0;
	auto bits = [&](int n) -> uint32_t {
		uint32_t v = 0;
		for (int i = 0; i < n; i++, at++) v |= static_cast<uint32_t>((b[at >> 3] >> (at & 7)) & 1) << i;
		return v;
	};
	int mode = 0;
	while (mode < 8 && bits(1) == 0) mode++;
	if (mode == 8) {
		for (int i = 0; i < 16; i++) out[i][0] = out[i][1] = out[i][2] = out[i][3] = 0;
		return;
	}
	const Mode &m = MODES[mode];
	const uint32_t partition = bits(m.partition_bits), rotation = bits(m.rotation_bits), selection = bits(m.selection_bits);
	const int ends = m.subsets * 2;
	uint32_t e[6][4] = {};
	for (int c = 0; c < 3; c++) {
		for (int k = 0; k < ends; k++) e[k][c] = bits(m.colour_bits);
	}
	for (int k = 0; k < ends; k++) e[k][3] = m.alpha_bits > 0 ? bits(m.alpha_bits) : 255;
	/* The p-bits: a lowest bit more of every endpoint's numbers, of each endpoint or shared by a subset's two. */
	int colour_bits = m.colour_bits, alpha_bits = m.alpha_bits;
	if (m.endpoint_pbits || m.shared_pbits) {
		uint32_t p[6] = {};
		if (m.endpoint_pbits) {
			for (int k = 0; k < ends; k++) p[k] = bits(1);
		} else {
			for (int s = 0; s < m.subsets; s++) p[2 * s] = p[2 * s + 1] = bits(1);
		}
		for (int k = 0; k < ends; k++) {
			for (int c = 0; c < 4; c++) {
				if (c < 3 || m.alpha_bits > 0) e[k][c] = e[k][c] << 1 | p[k];
			}
		}
		colour_bits++;
		if (alpha_bits > 0) alpha_bits++;
	}
	auto expand = [](uint32_t v, int n) { return static_cast<uint32_t>((v << (8 - n)) | (v >> (2 * n - 8))); };
	for (int k = 0; k < ends; k++) {
		for (int c = 0; c < 3; c++) e[k][c] = expand(e[k][c], colour_bits);
		if (alpha_bits > 0) e[k][3] = expand(e[k][3], alpha_bits);
	}
	auto subset_of = [&](int i) -> int {
		if (m.subsets == 2) return (BC7_PARTITIONS_2[partition] >> i) & 1;
		if (m.subsets == 3) return (BC7_PARTITIONS_3[partition] >> (2 * i)) & 3;
		return 0;
	};
	auto anchor = [&](int i) {
		if (i == 0) return true;
		if (m.subsets == 2) return i == BC7_ANCHORS_2[partition];
		if (m.subsets == 3) return i == BC7_ANCHORS_3[partition][0] || i == BC7_ANCHORS_3[partition][1];
		return false;
	};
	uint8_t index[16] = {}, index2[16] = {};
	for (int i = 0; i < 16; i++) index[i] = static_cast<uint8_t>(bits(m.index_bits - (anchor(i) ? 1 : 0)));
	if (m.index2_bits > 0) {
		for (int i = 0; i < 16; i++) index2[i] = static_cast<uint8_t>(bits(m.index2_bits - (i == 0 ? 1 : 0)));
	}
	auto weight = [](int n, int i) { return n == 2 ? WEIGHTS_2[i] : n == 3 ? WEIGHTS_3[i] : WEIGHTS_4[i]; };
	for (int i = 0; i < 16; i++) {
		const int s = subset_of(i);
		const uint32_t *e0 = e[2 * s], *e1 = e[2 * s + 1];
		/* Of two sets of indices (modes 4 and 5) the colour by one and the alpha by the other, which by the selection bit. */
		int cw, aw;
		if (m.index2_bits > 0) {
			cw = selection ? weight(m.index2_bits, index2[i]) : weight(m.index_bits, index[i]);
			aw = selection ? weight(m.index_bits, index[i]) : weight(m.index2_bits, index2[i]);
		} else {
			cw = aw = weight(m.index_bits, index[i]);
		}
		for (int c = 0; c < 4; c++) {
			const int w = c < 3 ? cw : aw;
			out[i][c] = static_cast<uint8_t>(((64 - w) * e0[c] + w * e1[c] + 32) >> 6);
		}
		if (rotation > 0) std::swap(out[i][3], out[i][rotation - 1]);
	}
}

/**
 * A .dds as RGBA: BC1, BC2, BC3, BC4 (DXT1, DXT3, DXT5, ATI1; of the old header or of DX10's), BC7 (DX10's) and 32-bit
 * pixels; the first of its sizes no bigger than 1024. BC5 (normals) and BC6 not.
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
	enum class Kind : uint8_t { None, Bc1, Bc2, Bc3, Bc4, Bc7, Rgba, Bgra } kind = Kind::None;
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
		else if (dxgi == 98 || dxgi == 99) kind = Kind::Bc7;
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
			if (kind == Kind::Bc7) {
				uint8_t px[16][4];
				Bc7Block(b, px);
				for (int i = 0; i < 16; i++) {
					const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
					if (x < w && y < h) std::copy_n(px[i], 4, &rgba[(static_cast<size_t>(y) * w + x) * 4]);
				}
				continue;
			}
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
	/* As BeamNG finds it: by its name, else as .dds or .png whichever the material named (the Škoda 1203's materials
	 * name main_d.color.png, its picture is main_d.color.dds; its wheels' the other way round); with its folder, else
	 * the car's own. */
	auto read = [&v](const std::string &file) {
		std::vector<uint8_t> bytes = ReadModFile(v, file);
		const size_t dot = file.find_last_of('.');
		if (bytes.empty() && dot != std::string::npos) {
			for (const char *ext : {".dds", ".png"}) {
				if (!StrEqualsIgnoreCase(std::string_view(file).substr(dot), ext)) bytes = ReadModFile(v, file.substr(0, dot) + ext);
				if (!bytes.empty()) break;
			}
		}
		return bytes;
	};
	std::vector<uint8_t> bytes = read(name);
	if (bytes.empty()) {
		/* A texture named without its folder: the car's own. */
		std::string base = name.substr(name.find_last_of("/\\") + 1);
		if (base != name) bytes = read(base);
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
 * The outer side of a triangle: 1 as its corners go round (counter-clockwise seen from outside), -1 the other way --
 * a mirrored node's, or one whose corners joined turned it round --, by its corners' normals; 0 not known. A sheet of
 * two sides is two triangles on the same corners facing apart (the Škoda 1203's blind over its back side windows:
 * painted outside, the trim inside): BeamNG leaves out the side facing away, drawn both each came through the other.
 */
static int8_t FacingOf(const std::vector<float> &pos, const std::vector<float> &nrm, const uint32_t *corner)
{
	const float *a = &pos[corner[0] * 3], *b = &pos[corner[1] * 3], *c = &pos[corner[2] * 3];
	const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
	const double gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
	double s = 0;
	for (int j = 0; j < 3; j++) {
		const float *n = &nrm[corner[j] * 3];
		s += gx * n[0] + gy * n[1] + gz * n[2];
	}
	return s > 0 ? 1 : (s < 0 ? -1 : 0);
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
		int16_t cell; ///< the paint's: its material's cell (BeamngMesh::tri_cell), its corners not those of another's
		bool operator<(const Key &o) const { return std::tie(x, y, z, nx, ny, nz, material, cell) < std::tie(o.x, o.y, o.z, o.nx, o.ny, o.nz, o.material, o.cell); }
	};
	std::map<Key, uint32_t> corners;
	std::vector<float> pos, nrm, uv;
	std::vector<float> sums; // the normals summed, then made one long
	std::vector<uint32_t> tri;
	std::vector<uint8_t> tri_tex;
	std::vector<int16_t> tri_cell;
	std::vector<int8_t> tri_facing;
	std::vector<uint8_t> tri_part;
	for (size_t t = 0; t + 2 < mesh.tri.size(); t += 3) {
		const uint8_t material = t / 3 < mesh.tri_tex.size() ? mesh.tri_tex[t / 3] : 0;
		const int16_t paint_cell = t / 3 < mesh.tri_cell.size() ? mesh.tri_cell[t / 3] : -1;
		uint32_t made[3];
		for (int j = 0; j < 3; j++) {
			const uint32_t c = mesh.tri[t + j];
			const float *p = &mesh.pos[c * 3], *n = &mesh.nrm[c * 3];
			Key key{static_cast<int32_t>(std::floor(p[0] / cell)), static_cast<int32_t>(std::floor(p[1] / cell)), static_cast<int32_t>(std::floor(p[2] / cell)),
					static_cast<int8_t>(std::lround(n[0] * 2)), static_cast<int8_t>(std::lround(n[1] * 2)), static_cast<int8_t>(std::lround(n[2] * 2)), material, paint_cell};
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
		tri_cell.push_back(paint_cell);
		tri_facing.push_back(t / 3 < mesh.tri_facing.size() ? mesh.tri_facing[t / 3] : 0);
		if (!mesh.tri_part.empty()) tri_part.push_back(t / 3 < mesh.tri_part.size() ? mesh.tri_part[t / 3] : 0);
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
	mesh.tri_cell = std::move(tri_cell);
	mesh.tri_facing = std::move(tri_facing);
	mesh.tri_part = std::move(tri_part);
}

} // namespace

/** A .dds of a mod as the game reads it (DecodeDds()): the rig's 'test3d dds'. */
bool DecodeBeamngDds(const std::vector<uint8_t> &d, int &w, int &h, std::vector<uint8_t> &rgba)
{
	return DecodeDds(d, w, h, rgba);
}

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
	/* A configuration of a folder: its cars numbered on from the folder's last, its GRF the folder's. */
	auto take = [&](std::optional<BeamngVehicle> car, uint16_t &next_id) {
		if (!car.has_value()) return;
		if (std::ranges::any_of(_beamng_vehicles, [&car](const BeamngVehicle &o) { return o.grfid == car->grfid && (o.Package() != car->Package() || o.folder != car->folder); })) {
			Debug(grf, 0, "BeamNG: {} {} has the same GRF id as another, left out", car->Package(), car->id);
			return;
		}
		auto same_folder = std::ranges::find_if(_beamng_vehicles, [&car](const BeamngVehicle &o) { return o.grfid == car->grfid; });
		if (same_folder != _beamng_vehicles.end()) {
			car->grf_path = same_folder->grf_path;
		} else {
			car->grf_path = grf_folder + car->id + ".grf";
			if (std::ranges::any_of(_beamng_vehicles, [&car](const BeamngVehicle &o) { return StrEqualsIgnoreCase(o.grf_path, car->grf_path); })) {
				car->grf_path = grf_folder + car->id + fmt::format("_{:08X}", car->grfid) + ".grf";
			}
		}
		for (BeamngModel &m : car->models) m.id = next_id++;
		Debug(grf, 1, "BeamNG: {} ({} {}): {} {}, {:.0f} kW, {:.0f} km/h, {:.2f} t, effort {:.2f}, {} paints, GRF {:02X}{:02X}{:02X}{:02X}", car->name, car->Package(), car->id,
				car->goods ? "goods" : "seats", car->capacity, car->power_kw, car->speed_kmh, car->mass_t, car->effort, car->paints.size(),
				car->grfid & 0xFF, (car->grfid >> 8) & 0xFF, (car->grfid >> 16) & 0xFF, car->grfid >> 24);
		for (const BeamngModel &m : car->models) {
			Debug(grf, 1, "BeamNG:   {} '{}': from {}, {:.0f} kW, {:.0f} km/h", m.id, m.name, m.year_from, m.power_kw, m.speed_kmh);
		}
		_beamng_vehicles.push_back(std::move(*car));
	};
	/* A car, and each of its other configurations as cars of their own when it is one; all of them in one GRF. */
	auto take_all = [&](const std::string &zip, const std::string &car_folder, const std::string &id) {
		std::optional<BeamngVehicle> car = ReadCar(zip, car_folder, id);
		if (!car.has_value()) return;
		uint16_t next_id = BEAMNG_ENGINE_ID;
		take(std::move(car), next_id);
		const std::vector<std::string> configs = OtherConfigs(zip, car_folder, id);
		for (const std::string &config : configs) take(ReadCar(zip, car_folder, id, config), next_id);
		/* The game's own configurations of our 1203, of a .pc the mod has; numbered after the mod's, so these keep theirs. */
		if (!StrEqualsIgnoreCase(id, "S1203")) return;
		for (const auto &[own, base] : S1203_OWN_CONFIGS) {
			if (std::ranges::any_of(configs, [base](const std::string &c) { return StrEqualsIgnoreCase(c, base); })) take(ReadCar(zip, car_folder, id, std::string(own)), next_id);
		}
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
				for (const std::string &car : cars) take_all(folder + file, car, car.substr(9, car.size() - 10));
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
						take_all({}, vehicles + id + PATHSEP, id);
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
		/* Only one the game made: small (a few kB, more with many configurations), with its words. */
		std::vector<uint8_t> old = ReadBytes(folder + file);
		if (old.empty() || old.size() > 65536 || std::search(old.begin(), old.end(), MARK.begin(), MARK.end()) == old.end()) continue;
		std::filesystem::remove(entry.path(), ec);
		Debug(grf, 1, "BeamNG: {} removed, its car is no longer there", folder + file);
	}
	if (!found.empty()) FioCreateDirectory(folder);
	/* A GRF a folder: its configurations in the order found, the car itself first. */
	std::vector<std::string> written;
	for (const BeamngVehicle &v : found) {
		if (std::ranges::find(written, v.grf_path) != written.end()) continue;
		written.push_back(v.grf_path);
		std::vector<const BeamngVehicle *> cars;
		for (const BeamngVehicle &o : found) {
			if (o.grf_path == v.grf_path) cars.push_back(&o);
		}
		std::vector<uint8_t> grf = MakeGrf(cars);
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
 * A wheel of the game's own on a car whose mod has none (ReadBeamngShape()): a tyre, its tread and its two sides, and a
 * grey rim filling it, about the line between two hub nodes (metres in the axes of BeamNG) at their middle.
 * @param r its radius, metres
 * @param width the tyre's width, metres
 * @param rim the rim's radius, metres
 */
static void AddBeamngWheel(BeamngMesh &mesh, const CarFrame &frame, const std::array<double, 3> &a, const std::array<double, 3> &b, double r, double width, double rim)
{
	auto material = [&](const char *name, uint32_t colour) {
		for (size_t i = 0; i < mesh.materials.size(); i++) {
			if (mesh.materials[i].name == name) return static_cast<uint8_t>(i);
		}
		if (mesh.materials.size() >= 255) return static_cast<uint8_t>(0);
		BeamngMesh::Material m;
		m.name = name;
		m.flat = colour;
		mesh.materials.push_back(std::move(m));
		return static_cast<uint8_t>(mesh.materials.size() - 1);
	};
	const uint8_t tyre = material("wheel of the game: tyre", 0xFF1E1E1E), metal = material("wheel of the game: rim", 0xFF9C9C9C);
	/* Its axis, and two ways across it. */
	double ax[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	const double len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
	if (len < 1e-6) return;
	for (double &c : ax) c /= len;
	const double mid[3] = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2};
	double u[3] = {0, 0, 1};
	if (std::abs(ax[2]) > 0.9) u[0] = 1, u[2] = 0;
	double w[3] = {ax[1] * u[2] - ax[2] * u[1], ax[2] * u[0] - ax[0] * u[2], ax[0] * u[1] - ax[1] * u[0]};
	const double wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	for (double &c : w) c /= wl;
	const double v2[3] = {w[1] * ax[2] - w[2] * ax[1], w[2] * ax[0] - w[0] * ax[2], w[0] * ax[1] - w[1] * ax[0]};
	auto corner = [&](double radius, double angle, double side, const double *n) {
		double p[3];
		for (int k = 0; k < 3; k++) p[k] = mid[k] + ax[k] * side * width / 2 + (w[k] * std::cos(angle) + v2[k] * std::sin(angle)) * radius;
		auto o = ToObject(frame, p[0], p[1], p[2]);
		mesh.pos.insert(mesh.pos.end(), o.begin(), o.end());
		/* (The normal into the object's axes as the meshes' are.) */
		mesh.nrm.push_back(static_cast<float>(-n[1]));
		mesh.nrm.push_back(static_cast<float>(n[0]));
		mesh.nrm.push_back(static_cast<float>(n[2]));
		mesh.uv.insert(mesh.uv.end(), {0.5f, 0.5f});
	};
	auto quad = [&](uint8_t tex, std::array<double, 4> radii, std::array<double, 4> angles, std::array<double, 4> sides, const std::array<double, 3> &n0, const std::array<double, 3> &n1) {
		const uint32_t base = static_cast<uint32_t>(mesh.pos.size() / 3);
		for (int k = 0; k < 4; k++) corner(radii[k], angles[k], sides[k], (k == 1 || k == 2) ? n1.data() : n0.data());
		mesh.tri.insert(mesh.tri.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
		mesh.tri_tex.insert(mesh.tri_tex.end(), {tex, tex});
	};
	constexpr int SEGMENTS = 16;
	for (int i = 0; i < SEGMENTS; i++) {
		const double t0 = i * 2 * M_PI / SEGMENTS, t1 = (i + 1) * 2 * M_PI / SEGMENTS;
		auto out = [&](double t) { return std::array<double, 3>{w[0] * std::cos(t) + v2[0] * std::sin(t), w[1] * std::cos(t) + v2[1] * std::sin(t), w[2] * std::cos(t) + v2[2] * std::sin(t)}; };
		const std::array<double, 3> o0 = out(t0), o1 = out(t1);
		/* The tread, and each side: the tyre's ring, and the rim inside it a little sunk. */
		quad(tyre, {r, r, r, r}, {t0, t1, t1, t0}, {-1, -1, 1, 1}, o0, o1);
		for (double side : {-1.0, 1.0}) {
			const std::array<double, 3> n = {ax[0] * side, ax[1] * side, ax[2] * side};
			quad(tyre, {rim, r, r, rim}, {t0, t0, t1, t1}, {side, side, side, side}, n, n);
			quad(metal, {0, rim, rim, 0}, {t0, t0, t1, t1}, {side * 0.8, side * 0.8, side * 0.8, side * 0.8}, n, n);
		}
	}
}

/**
 * A paint design ("globalSkin", the Škoda 1203's VB in yellow and white with its letters) laid into a painted material's
 * picture: where the paint's mask is light, the configuration's paint its palette names there (red the first, green the
 * second, blue the third) at the picture's lightness, as the game paints a car (three_d_gpu.cpp); over it its letters
 * where their mask is light. The mask is then dark all over: the car keeps the design's colours, not the GRF's.
 * @param paints the configuration's paints (CarParts::paints), else the car's own
 */
static void LaySkin(const BeamngVehicle &v, const CarMaterial &cm, const std::vector<uint32_t> &paints, BeamngMesh::PaintSource &src)
{
	if (src.rgba.empty() || src.w <= 0 || src.h <= 0) return;
	BeamngMesh::Material palette, decal, decal_mask;
	if (!ReadTexture(v, cm.palette, palette, nullptr)) {
		Debug(misc, 1, "BeamNG {}: paint design {} without its palette {}", v.id, src.name, cm.palette);
		return;
	}
	const bool letters = !cm.decal.empty() && ReadTexture(v, cm.decal, decal, nullptr) && ReadTexture(v, cm.decal_mask, decal_mask, nullptr);
	auto at = [](const std::vector<uint8_t> &rgba, int w, int h, double u, double vv) -> const uint8_t * {
		const int x = std::clamp(static_cast<int>(u * w), 0, w - 1), y = std::clamp(static_cast<int>(vv * h), 0, h - 1);
		return &rgba[(static_cast<size_t>(y) * w + x) * 4];
	};
	auto paint = [&](size_t k) { return k < paints.size() ? paints[k] : k < v.paints.size() ? v.paints[k] : 0xFFFFFFFFu; };
	const uint32_t colours[3] = {paint(0), paint(1), paint(2)};
	for (int y = 0; y < src.h; y++) {
		for (int x = 0; x < src.w; x++) {
			const double u = (x + 0.5) / src.w, vv = (y + 0.5) / src.h;
			uint8_t *c = &src.rgba[(static_cast<size_t>(y) * src.w + x) * 4];
			const double m = src.mask_rgba.empty() ? 1.0 : at(src.mask_rgba, src.mw, src.mh, u, vv)[0] / 255.0;
			if (m > 0) {
				const uint8_t *p = at(palette.rgba, palette.w, palette.h, u, vv);
				const double sum = p[0] + p[1] + p[2];
				const double light = std::max({c[0], c[1], c[2]}) / 255.0;
				for (int k = 0; k < 3; k++) {
					double want = 0;
					for (int j = 0; j < 3; j++) {
						const uint8_t channel = static_cast<uint8_t>(colours[j] >> (16 - 8 * k));
						want += channel * (sum > 0 ? p[j] / sum : j == 0 ? 1.0 : 0.0);
					}
					c[k] = static_cast<uint8_t>(std::lround(c[k] + (std::min(255.0, want * light) - c[k]) * m));
				}
			}
			if (letters) {
				const double a = at(decal_mask.rgba, decal_mask.w, decal_mask.h, u, vv)[0] / 255.0;
				if (a > 0) {
					const uint8_t *d = at(decal.rgba, decal.w, decal.h, u, vv);
					for (int k = 0; k < 3; k++) c[k] = static_cast<uint8_t>(std::lround(c[k] + (d[k] - c[k]) * a));
				}
			}
		}
	}
	Debug(misc, 1, "BeamNG {}: paint design {} laid ({} paints, letters {})", v.id, src.name, std::min<size_t>(3, paints.size()), letters ? "yes" : "no");
	src.png.clear();
	src.mw = src.w;
	src.mh = src.h;
	src.mask_rgba.assign(static_cast<size_t>(src.w) * src.h * 4, 0);
	src.mask_png.clear();
}

/**
 * Read a car's model for drawing: the meshes its flexbodies name, each where its flexbody puts it ("pos", "rot" in
 * degrees about x, y and z, "scale"; else where its node in the .dae is), in the frame of the vehicle objects; the
 * materials' colours, textures and paint; the body's holes cut by its mask; the glass of its lights and where they are.
 */
bool ReadBeamngShape(const BeamngVehicle &v, BeamngMesh &mesh, int make)
{
	JValue info = ParseJson(ReadModFile(v, "info.json"));
	CarParts parts;
	if (!ReadCarParts(v, info.IsObject() ? &info : nullptr, parts, make)) return false;
	auto fr = _frames.find(v.Package() + "|" + v.folder + "|" + v.config);
	const CarFrame frame = fr != _frames.end() ? fr->second : CarFrame{};

	/* The model: the .dae files of the folder; and those the mod shares in vehicles/common/ when a flexbody names a mesh
	 * they have (the Škoda 1203's wheels, S1203_wheels/wheel.dae). The first that has a mesh places it. */
	std::vector<std::string> daes;
	for (const std::string &f : ListCarFiles(v)) {
		if (EndsWithIgnoreCase(f, ".dae")) daes.push_back(f);
	}
	if (daes.empty()) return false;
	const size_t own_daes = daes.size();
	std::vector<std::string> common_daes;
	for (const std::string &f : ListModCommonFiles(v)) {
		if (EndsWithIgnoreCase(f, ".dae")) common_daes.push_back(f);
	}
	/* The parts drawn: those used, and the tarp of a flatbed, a part of the shape of its own. */
	std::vector<const JValue *> drawn = parts.used;
	if (parts.tarp != nullptr) drawn.push_back(parts.tarp);
	std::set<std::string> wanted; // the meshes the flexbodies name
	for (const JValue *part : drawn) {
		const JValue *flex = part->Get("flexbodies");
		if (flex == nullptr || !flex->IsArray()) continue;
		for (const JValue &row : flex->items) {
			if (!row.IsArray() || row.items.empty() || !row.items[0].IsString() || row.items[0].text == "mesh") continue;
			std::string name = row.items[0].text;
			strtolower(name);
			wanted.insert(name);
		}
	}
	std::vector<XmlNode> docs(daes.size() + common_daes.size());
	std::map<std::string, Placed> placed;
	for (size_t i = 0; i < daes.size(); i++) {
		/* The shared ones when the car's own leave a mesh out. */
		if (i + 1 == own_daes && std::ranges::any_of(wanted, [&placed](const std::string &n) { return !placed.contains(n); })) {
			daes.insert(daes.end(), common_daes.begin(), common_daes.end());
		}
		std::vector<uint8_t> bytes = ReadModFile(v, daes[i]);
		if (!ParseXml(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), docs[i])) continue;
		const XmlNode *collada = docs[i].Kid("COLLADA");
		if (collada == nullptr) continue;
		double unit = 1;
		bool y_up = false;
		if (const XmlNode *asset = collada->Kid("asset"); asset != nullptr) {
			if (const XmlNode *u = asset->Kid("unit"); u != nullptr && u->Attr("meter") != nullptr) unit = NumberOf(*u->Attr("meter"));
			if (const XmlNode *up = asset->Kid("up_axis"); up != nullptr) y_up = up->text.find("Y_UP") != std::string::npos;
		}
		if (unit <= 0) unit = 1;
		std::map<std::string, const XmlNode *> geometries;
		if (const XmlNode *lib = collada->Kid("library_geometries"); lib != nullptr) {
			for (const XmlNode &g : lib->kids) {
				if (g.name == "geometry") geometries[g.AttrOr("id")] = &g;
			}
		}
		std::map<std::string, Placed> here;
		if (const XmlNode *lib = collada->Kid("library_visual_scenes"); lib != nullptr) {
			for (const XmlNode &scene : lib->kids) {
				for (const XmlNode &n : scene.kids) {
					if (n.name == "node") CollectNodes(n, IDENTITY, geometries, here);
				}
			}
		}
		/* The names of the materials: the scene's material by its id, its name. */
		if (const XmlNode *lib = collada->Kid("library_materials"); lib != nullptr) {
			std::map<std::string, std::string> names;
			for (const XmlNode &m : lib->kids) names[m.AttrOr("id")] = m.AttrOr("name").empty() ? m.AttrOr("id") : m.AttrOr("name");
			for (auto &[key, p] : here) {
				for (auto &[symbol, target] : p.materials) {
					auto it = names.find(target);
					if (it != names.end()) target = it->second;
				}
			}
		}
		for (auto &[key, p] : here) {
			p.unit = unit;
			p.y_up = y_up;
			placed.emplace(key, std::move(p));
		}
	}

	std::map<std::string, CarMaterial> materials = ReadMaterials(v);
	/* The glowing glass: a material of "glowMap" with its lights ("lowbeam", "highbeam": ahead; "brake": at the back). */
	std::map<std::string, bool> glow; // material to whether it is at the back
	/* A material of the glow map is a stand-in, of no layers of its own (the Škoda 1203's "S1203_brakelight_glass", the
	 * green lamps of its dash): BeamNG draws its "off" material in its place while it does not glow (the player: "the
	 * rear light is not coloured"). */
	std::map<std::string, std::string> glow_off;
	for (const JValue *part : parts.used) {
		if (const JValue *g = part->Get("glowMap"); g != nullptr && g->IsObject()) {
			for (const auto &[name, entry] : g->members) {
				if (const JValue *off = entry.Get("off"); off != nullptr && off->IsString() && materials.contains(off->text)) glow_off.try_emplace(name, off->text);
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
	BeamngMesh::Material paint;
	paint.name = "paint";
	paint.flat = 0xFFFFFFFF;
	mesh.materials.push_back(std::move(paint));
	std::map<std::string, uint8_t> index;
	std::map<std::string, std::pair<int, std::pair<int, std::vector<uint8_t>>>> masks; // mask texture: w, h, grey
	/* own: a painted material as one of its own, out of the paint, for what (the tarp, the bed: BeamngMesh::own_paints). */
	auto material_of = [&](const std::string &name, std::string_view own = {}) -> uint8_t {
		const std::string key = own.empty() ? name : name + "|" + std::string(own);
		auto it = index.find(key);
		if (it != index.end()) return it->second;
		auto m = materials.find(name);
		uint8_t at = 0;
		if (m == materials.end() || !m->second.paint || !own.empty()) {
			if (mesh.materials.size() >= 255) return 0;
			BeamngMesh::Material made;
			made.name = name;
			made.flat = m != materials.end() ? m->second.colour : 0xFFB4B4B4;
			if (m != materials.end()) ReadTexture(v, m->second.texture, made, &made.png);
			if (m != materials.end() && m->second.see_through) {
				/* The glass: as much of what is behind it hides as its "opacityMap" has it where, else a little all over. */
				BeamngMesh::Material opacity;
				if (ReadTexture(v, m->second.mask, opacity, &opacity.png)) {
					made.alpha = 254;
					made.opacity_rgba = std::move(opacity.rgba);
					made.opacity_png = std::move(opacity.png);
					made.ow = opacity.w;
					made.oh = opacity.h;
				} else {
					made.alpha = GLASS_WITHOUT_MAP;
				}
			}
			at = static_cast<uint8_t>(mesh.materials.size());
			mesh.materials.push_back(std::move(made));
		}
		index[key] = at;
		return at;
	};
	/* A material of the configuration's paint design instead, where the mod has one ("<material>.skin.<design>": BeamNG's
	 * "globalSkin", the Škoda 1203's VB). */
	auto skinned = [&](const std::string &name) {
		if (parts.skin.empty()) return name;
		const std::string designed = name + ".skin." + parts.skin;
		return materials.contains(designed) ? designed : name;
	};
	/* A painted material's cell of the paint's texture: its first layer's picture and its paint's mask (CarMaterial::paint_mask). */
	std::map<std::string, int16_t> cells;
	auto cell_of = [&](const std::string &name, const CarMaterial &cm) -> int16_t {
		auto it = cells.find(name);
		if (it != cells.end()) return it->second;
		BeamngMesh::PaintSource src;
		src.name = name;
		BeamngMesh::Material base, mask;
		if (ReadTexture(v, cm.texture, base, &base.png)) {
			src.w = base.w;
			src.h = base.h;
			src.rgba = std::move(base.rgba);
			src.png = std::move(base.png);
		}
		if (!cm.paint_mask.empty() && ReadTexture(v, cm.paint_mask, mask, &mask.png)) {
			src.mw = mask.w;
			src.mh = mask.h;
			src.mask_rgba = std::move(mask.rgba);
			src.mask_png = std::move(mask.png);
		}
		/* A paint design's material (its name "<material>.skin.<design>"): its colours laid in. */
		if (!cm.palette.empty() && name.find(".skin.") != std::string::npos) LaySkin(v, cm, parts.paints, src);
		const int16_t at = static_cast<int16_t>(mesh.paint_sources.size());
		mesh.paint_sources.push_back(std::move(src));
		cells[name] = at;
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
	std::set<std::string> drawn_groups; // the groups of nodes the meshes drawn hang on (a wheel's "wheel_FR", "tire_FR")
	for (const JValue *part : drawn) {
		const JValue *flex = part->Get("flexbodies");
		if (flex == nullptr || !flex->IsArray()) continue;
		const size_t first_tri = mesh.tri.size() / 3;
		/* The bed's (its slot "..._bed", FindBed()) painted apart from the body (BeamngVehicle::bed_paint). */
		const JValue *slot = part->Get("slotType");
		const bool bed_part = v.bed_paint != 0 && slot != nullptr && slot->IsString() && EndsWithIgnoreCase(slot->text, "_bed");
		/* The tarp's triangles a part of their own. */
		auto mark_tarp = [&]() {
			if (part != parts.tarp) return;
			mesh.tri_part.resize(mesh.tri.size() / 3, 0);
			std::fill(mesh.tri_part.begin() + first_tri, mesh.tri_part.end(), BeamngMesh::PART_TARP);
		};
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
			if (row.items.size() > 1 && row.items[1].IsArray()) {
				for (const JValue &g : row.items[1].items) {
					if (g.IsString()) drawn_groups.insert(g.text);
				}
			}
			/* Its placing: "pos", "rot" (degrees, about x, then y, then z), "scale"; the node's matrix of the .dae before it. */
			double pos[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
			for (size_t i = 1; i < row.items.size(); i++) {
				const JValue &o = row.items[i];
				if (!o.IsObject()) continue;
				auto xyz = [&](const char *key, double *out) {
					if (const JValue *p = o.Get(key); p != nullptr && p->IsObject()) {
						out[0] = p->Get("x") != nullptr ? parts.Number(*p->Get("x"), out[0]) : out[0];
						out[1] = p->Get("y") != nullptr ? parts.Number(*p->Get("y"), out[1]) : out[1];
						out[2] = p->Get("z") != nullptr ? parts.Number(*p->Get("z"), out[2]) : out[2];
					}
				};
				xyz("pos", pos);
				xyz("rot", rot);
				xyz("scale", scale);
			}
			/* Where its part's slots move it (the wheels out to their track). */
			const auto moved = parts.Moved(part, pos[0], pos[1], pos[2]);
			std::copy(moved.begin(), moved.end(), pos);
			const double rx = rot[0] * M_PI / 180, ry = rot[1] * M_PI / 180, rz = rot[2] * M_PI / 180;
			const Matrix mx = {1, 0, 0, 0, 0, std::cos(rx), -std::sin(rx), 0, 0, std::sin(rx), std::cos(rx), 0, 0, 0, 0, 1};
			const Matrix my = {std::cos(ry), 0, std::sin(ry), 0, 0, 1, 0, 0, -std::sin(ry), 0, std::cos(ry), 0, 0, 0, 0, 1};
			const Matrix mz = {std::cos(rz), -std::sin(rz), 0, 0, std::sin(rz), std::cos(rz), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
			Matrix place = Multiply(Multiply(mz, my), mx);
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++) place[r * 4 + c] *= scale[c];
				place[r * 4 + 3] = pos[r];
			}
			/* The units of its .dae and its up: metres, z up. */
			const double unit = pl->second.unit;
			Matrix units = IDENTITY;
			units[0] = units[5] = units[10] = unit;
			if (pl->second.y_up) units = {unit, 0, 0, 0, 0, 0, -unit, 0, 0, unit, 0, 0, 0, 0, 0, 1};
			const Matrix m = Multiply(place, Multiply(units, pl->second.world));
			for (Triangles &t : ReadGeometry(*pl->second.geometry)) {
				auto bound = pl->second.materials.find(t.material);
				const std::string named = skinned(bound != pl->second.materials.end() ? bound->second : t.material);
				/* Drawn as its "off" material if it is one of the glow map; glowing at night by its own name. */
				auto off = glow_off.find(named);
				const std::string material = off != glow_off.end() ? skinned(off->second) : named;
				auto cm = materials.find(material);
				/* The tarp's paint its own, apart from the body's, where the player chooses it (BeamngVehicle::tarp_paints);
				 * and the bed's (BeamngVehicle::bed_paint). */
				const bool painted = cm != materials.end() && cm->second.paint;
				const bool own_tarp = part == parts.tarp && !v.tarp_paints.empty() && painted;
				const bool own_bed = bed_part && painted;
				const uint8_t tex = material_of(material, own_tarp ? "tarp" : own_bed ? "bed" : "");
				if ((own_tarp || own_bed) && std::ranges::find(mesh.own_paints, tex, &std::pair<uint8_t, uint8_t>::first) == mesh.own_paints.end()) {
					mesh.own_paints.emplace_back(tex, own_tarp ? BeamngMesh::OWN_TARP : BeamngMesh::OWN_BED);
				}
				const bool cut = cm != materials.end() && !cm->second.mask.empty() && !cm->second.see_through;
				auto gl = glow.find(named);
				const size_t corners = t.pos.size() / 3;
				const int16_t cell = painted && !own_tarp && !own_bed ? cell_of(material, cm->second) : -1;
				for (size_t c = 0; c + 2 < corners; c += 3) {
					if (cut) {
						/* A hole of the body where its mask is dark at the triangle's middle. */
						const std::vector<float> &uvs = cm->second.mask_uv1 ? t.uv1 : t.uv0;
						const float u = (uvs[c * 2] + uvs[c * 2 + 2] + uvs[c * 2 + 4]) / 3, w = (uvs[c * 2 + 1] + uvs[c * 2 + 3] + uvs[c * 2 + 5]) / 3;
						if (mask_value(cm->second.mask, u, w) < 128) continue;
					}
					const uint8_t tri_tex = tex;
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
					mesh.tri_tex.push_back(tri_tex);
					mesh.tri_cell.push_back(cell);
					/* Seen from one side (its outer side counted when the shape is done, FacingOf()), or from both. */
					mesh.tri_facing.push_back(cm != materials.end() && cm->second.both_sides ? 0 : 1);
					if (gl != glow.end()) {
						mesh.night_colour.push_back(gl->second ? 0xFFFF2814u : 0xFFFFF4D8u);
						mesh.night_kind.push_back(gl->second ? 1 : 0);
					}
				}
			}
		}
		mark_tarp();
	}
	/*
	 * Its wheels the mod leaves to BeamNG's own parts (the player, 26. 10.: the Škoda 1203 "has no wheels": its
	 * "S1203_wheel_F", tyres and hubcaps are in none of its files): for each wheel no mesh hangs on, a wheel of its own on
	 * its hub -- a tyre of the wheel's radius and a grey rim, between its two hub nodes.
	 */
	int made_wheels = 0;
	for (const JValue *part : parts.used) {
		for (const char *kind : {"pressureWheels", "hubWheels", "hubWheelsTSV", "pressureWheelsTSV"}) {
			const JValue *wheels = part->Get(kind);
			if (wheels == nullptr || !wheels->IsArray()) continue;
			double r = 0.3, width = 0.17, hub = 0;
			for (const JValue &row : wheels->items) {
				if (row.IsObject()) {
					for (const auto &[k, val] : row.members) {
						if (k == "radius" && val.type == JValue::Type::Number) r = val.number;
						if (k == "tireWidth" && val.type == JValue::Type::Number) width = val.number;
						if (k == "hubRadius" && val.type == JValue::Type::Number) hub = val.number;
					}
					continue;
				}
				if (!row.IsArray() || row.items.size() < 5 || !row.items[0].IsString() || row.items[0].text == "name") continue;
				if (!row.items[3].IsString() || !row.items[4].IsString()) continue;
				const std::string hub_group = row.items[1].IsString() ? row.items[1].text : std::string();
				const std::string tire_group = row.items[2].IsString() ? row.items[2].text : std::string();
				if ((!hub_group.empty() && drawn_groups.contains(hub_group)) || (!tire_group.empty() && drawn_groups.contains(tire_group))) continue;
				auto n1 = parts.nodes.find(row.items[3].text), n2 = parts.nodes.find(row.items[4].text);
				if (n1 == parts.nodes.end() || n2 == parts.nodes.end()) continue;
				AddBeamngWheel(mesh, frame, n1->second, n2->second, r, width, hub > 0 ? hub : r * 0.62);
				made_wheels++;
			}
		}
	}
	if (mesh.tri.empty()) return false;
	/* The paint's triangles into their materials' cells of its texture: each where it is in its own picture (one beyond
	 * its edge moved back whole into it), then in the cell. */
	if (!mesh.paint_sources.empty()) {
		const int side = BeamngMesh::PaintSide(mesh.paint_sources.size());
		for (size_t t = 0; t < mesh.tri_cell.size(); t++) {
			const int16_t cell = mesh.tri_cell[t];
			if (cell < 0) continue;
			const uint32_t *c = &mesh.tri[t * 3];
			float su = mesh.uv[c[0] * 2], sv = mesh.uv[c[0] * 2 + 1];
			for (int j = 1; j < 3; j++) {
				su = std::min(su, mesh.uv[c[j] * 2]);
				sv = std::min(sv, mesh.uv[c[j] * 2 + 1]);
			}
			su = std::floor(su + 1e-4f);
			sv = std::floor(sv + 1e-4f);
			for (int j = 0; j < 3; j++) {
				float &u = mesh.uv[c[j] * 2], &w = mesh.uv[c[j] * 2 + 1];
				u = (cell % side + std::clamp(u - su, 0.0f, 1.0f)) / side;
				w = (cell / side + std::clamp(w - sv, 0.0f, 1.0f)) / side;
			}
		}
	}
	const size_t full = mesh.tri.size() / 3;
	Simplify(mesh, 0.02f);
	/* Its triangles' outer sides where they are now, the corners joined. */
	for (size_t t = 0; t < mesh.tri_facing.size(); t++) {
		if (mesh.tri_facing[t] != 0) mesh.tri_facing[t] = FacingOf(mesh.pos, mesh.nrm, &mesh.tri[t * 3]);
	}

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
	Debug(misc, 1, "BeamNG {}: {} meshes ({} not in the model), {} wheels of the game's own, {} triangles ({} in the model), {} materials, {} of glass, lights {} ahead and {} at the back",
			v.name, meshes, missing, made_wheels, mesh.tri.size() / 3, full, mesh.materials.size(), mesh.night_tri.size() / 3, mesh.front_lights.size() / 3, mesh.back_lights.size() / 3);
	return true;
}
