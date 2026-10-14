/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_objects.cpp Reading buildings as real 3D objects (three_d_objects.h): the index, the .glb files, their textures. */

#include "stdafx.h"
#include "three_d_objects.h"
#include "debug.h"
#include "fileio_func.h"
#include "gfx_type.h"
#include "tile_type.h"
#include "3rdparty/nlohmann/json.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <map>

#ifdef WITH_PNG
#include <png.h>
#endif /* WITH_PNG */

#include "safeguards.h"

/** World units along x and y to a metre of the objects: a tile, 16 units, is 14.83765 m (the renders' scale). */
static constexpr double OBJECT_UNITS_PER_METRE = 16.0 / 14.83765;
/** Units of height to a metre: a height level, 8 units, is 3 m (hra/3d/OBJEKTY-3D.md). */
static constexpr double OBJECT_HEIGHT_PER_METRE = 8.0 / 3.0;

/** Every object read, and whether they were looked for yet. */
static std::vector<ThreeDObject> _three_d_objects;
static bool _three_d_objects_read = false;
/** The objects by house and colour slot: the index into _three_d_objects. */
static std::map<std::pair<HouseID, uint>, size_t> _three_d_house_objects;

/** A file's bytes, or nothing when it cannot be read. */
static std::vector<uint8_t> ReadWholeFile(const std::string &path)
{
	std::vector<uint8_t> data;
	auto f = FileHandle::Open(path, "rb");
	if (!f.has_value()) return data;
	fseek(*f, 0, SEEK_END);
	long len = ftell(*f);
	fseek(*f, 0, SEEK_SET);
	if (len <= 0 || len > 64 * 1024 * 1024) return data;
	data.resize(len);
	if (fread(data.data(), len, 1, *f) != 1) data.clear();
	return data;
}

/** The little-endian 32 bits at an offset. */
static uint32_t ReadU32(const std::vector<uint8_t> &d, size_t at)
{
	return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}

/**
 * Read one .glb into an object: its first mesh's triangles, their corners' positions and texture coordinates, the
 * node's transform applied, turned from glTF's axes (Y up, as Blender writes x, z, -y) into the game's and from
 * metres into units; and the PNG of its base colour texture, kept as it is until the object is drawn.
 * @return whether there is something to draw
 */
static bool ReadGlb(const std::string &path, ThreeDObject &obj)
{
	std::vector<uint8_t> d = ReadWholeFile(path);
	if (d.size() < 20 || ReadU32(d, 0) != 0x46546C67 || ReadU32(d, 4) != 2) {
		Debug(misc, 0, "3D object {}: not a glTF binary", path);
		return false;
	}
	/* The chunks: the JSON, then the binary buffer. */
	std::string json_text;
	std::vector<uint8_t> bin;
	for (size_t at = 12; at + 8 <= d.size();) {
		uint32_t len = ReadU32(d, at), type = ReadU32(d, at + 4);
		if (at + 8 + len > d.size()) break;
		if (type == 0x4E4F534A) json_text.assign(reinterpret_cast<const char *>(&d[at + 8]), len);
		if (type == 0x004E4942) bin.assign(d.begin() + at + 8, d.begin() + at + 8 + len);
		at += 8 + ((len + 3) & ~3u);
	}
	try {
		nlohmann::json j = nlohmann::json::parse(json_text);
		const auto &accessors = j.at("accessors");
		const auto &views = j.at("bufferViews");
		/* An accessor's elements as floats (or whole numbers for the indices). */
		auto read = [&](int index, int comps, std::vector<double> &out) {
			const auto &a = accessors.at(index);
			const auto &v = views.at(a.at("bufferView").get<int>());
			size_t offset = v.value("byteOffset", 0) + a.value("byteOffset", 0);
			int type = a.at("componentType").get<int>();
			size_t count = a.at("count").get<size_t>();
			size_t size = type == 5126 || type == 5125 ? 4 : type == 5123 || type == 5122 ? 2 : 1;
			size_t stride = v.value("byteStride", 0);
			if (stride == 0) stride = size * comps;
			out.resize(count * comps);
			for (size_t i = 0; i < count; i++) {
				for (int c = 0; c < comps; c++) {
					size_t at = offset + i * stride + c * size;
					if (at + size > bin.size()) throw std::out_of_range("accessor past the buffer");
					double value;
					switch (type) {
						case 5126: value = std::bit_cast<float>(ReadU32(bin, at)); break;
						case 5125: value = ReadU32(bin, at); break;
						case 5123: value = bin[at] | (bin[at + 1] << 8); break;
						case 5121: value = bin[at]; break;
						default: throw std::invalid_argument("component type");
					}
					out[i * comps + c] = value;
				}
			}
		};

		/* The node's transform: a matrix, or translation, rotation and scale. */
		std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major
		int mesh_index = 0;
		if (j.contains("nodes")) {
			for (const auto &node : j.at("nodes")) {
				if (!node.contains("mesh")) continue;
				mesh_index = node.at("mesh").get<int>();
				if (node.contains("matrix")) {
					for (int k = 0; k < 16; k++) m[k] = node.at("matrix").at(k).get<double>();
				} else {
					double t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
					if (node.contains("translation")) for (int k = 0; k < 3; k++) t[k] = node.at("translation").at(k).get<double>();
					if (node.contains("scale")) for (int k = 0; k < 3; k++) s[k] = node.at("scale").at(k).get<double>();
					if (node.contains("rotation")) for (int k = 0; k < 4; k++) q[k] = node.at("rotation").at(k).get<double>();
					double x = q[0], y = q[1], z = q[2], w = q[3];
					double r[9] = {
						1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
						2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
						2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y),
					};
					for (int col = 0; col < 3; col++) {
						for (int row = 0; row < 3; row++) m[col * 4 + row] = r[col * 3 + row] * s[col];
					}
					m[12] = t[0];
					m[13] = t[1];
					m[14] = t[2];
				}
				break;
			}
		}

		const auto &prims = j.at("meshes").at(mesh_index).at("primitives");
		int material = -1;
		for (const auto &p : prims) {
			if (p.value("mode", 4) != 4) continue; // triangles only
			std::vector<double> pos, uv, idx;
			read(p.at("attributes").at("POSITION").get<int>(), 3, pos);
			if (p.at("attributes").contains("TEXCOORD_0")) read(p.at("attributes").at("TEXCOORD_0").get<int>(), 2, uv);
			size_t corners = pos.size() / 3;
			if (uv.size() != corners * 2) uv.assign(corners * 2, 0.0);
			if (p.contains("indices")) {
				read(p.at("indices").get<int>(), 1, idx);
			} else {
				for (size_t i = 0; i < corners; i++) idx.push_back(static_cast<double>(i));
			}
			if (material < 0 && p.contains("material")) material = p.at("material").get<int>();
			uint32_t base = static_cast<uint32_t>(obj.pos.size() / 3);
			for (size_t i = 0; i < corners; i++) {
				double X = pos[i * 3], Y = pos[i * 3 + 1], Z = pos[i * 3 + 2];
				double gx = m[0] * X + m[4] * Y + m[8] * Z + m[12];
				double gy = m[1] * X + m[5] * Y + m[9] * Z + m[13];
				double gz = m[2] * X + m[6] * Y + m[10] * Z + m[14];
				/* glTF's Y is up and its Z points back along the game's -y. */
				obj.pos.push_back(static_cast<float>(gx * OBJECT_UNITS_PER_METRE));
				obj.pos.push_back(static_cast<float>(-gz * OBJECT_UNITS_PER_METRE));
				obj.pos.push_back(static_cast<float>(gy * OBJECT_HEIGHT_PER_METRE));
				obj.top = std::max(obj.top, static_cast<float>(gy * OBJECT_HEIGHT_PER_METRE));
				obj.uv.push_back(static_cast<float>(uv[i * 2]));
				obj.uv.push_back(static_cast<float>(uv[i * 2 + 1]));
			}
			for (size_t i = 0; i + 2 < idx.size(); i += 3) {
				uint32_t a = static_cast<uint32_t>(idx[i]), b = static_cast<uint32_t>(idx[i + 1]), c = static_cast<uint32_t>(idx[i + 2]);
				if (a >= corners || b >= corners || c >= corners) continue;
				obj.tri.push_back(base + a);
				obj.tri.push_back(base + b);
				obj.tri.push_back(base + c);
			}
		}

		/* The base colour's picture, a PNG in the buffer. */
		if (material >= 0 && j.contains("materials") && j.contains("textures") && j.contains("images")) {
			const auto &mat = j.at("materials").at(material);
			if (mat.contains("pbrMetallicRoughness") && mat.at("pbrMetallicRoughness").contains("baseColorTexture")) {
				int tex = mat.at("pbrMetallicRoughness").at("baseColorTexture").at("index").get<int>();
				int image = j.at("textures").at(tex).at("source").get<int>();
				const auto &img = j.at("images").at(image);
				if (img.contains("bufferView")) {
					const auto &v = views.at(img.at("bufferView").get<int>());
					size_t offset = v.value("byteOffset", 0), len = v.at("byteLength").get<size_t>();
					if (offset + len <= bin.size()) obj.png.assign(bin.begin() + offset, bin.begin() + offset + len);
				}
			}
		}
	} catch (const std::exception &e) {
		Debug(misc, 0, "3D object {}: {}", path, e.what());
		return false;
	}
	if (obj.tri.empty()) return false;

	/* It should lie on its plot: say so in the log when it does not. */
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e9f;
	for (size_t i = 0; i < obj.pos.size(); i += 3) {
		x0 = std::min(x0, obj.pos[i]); x1 = std::max(x1, obj.pos[i]);
		y0 = std::min(y0, obj.pos[i + 1]); y1 = std::max(y1, obj.pos[i + 1]);
		z0 = std::min(z0, obj.pos[i + 2]);
	}
	const float slack = 1.0f;
	if (x0 < -slack || y0 < -slack || x1 > obj.tiles_x * TILE_SIZE + slack || y1 > obj.tiles_y * TILE_SIZE + slack || z0 < -slack) {
		Debug(misc, 1, "3D object {}: reaches out of its {}x{} tiles: x {:.1f}..{:.1f}, y {:.1f}..{:.1f}, z from {:.1f}", obj.name, obj.tiles_x, obj.tiles_y, x0, x1, y0, y1, z0);
	}
	return true;
}

/** Look for the objects under baseset/decouple/objekty/<set>/objekty.json, in every place the game reads its data from. */
static void ReadThreeDObjects()
{
	_three_d_objects_read = true;
	std::vector<std::string> indices;
	for (Searchpath sp : _valid_searchpaths) {
		std::string dir = FioGetDirectory(sp, Subdirectory::Baseset) + "decouple" PATHSEP "objekty";
		std::error_code ec;
		if (!std::filesystem::is_directory(OTTD2FS(dir), ec)) continue;
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(dir), ec)) {
			std::string index = FS2OTTD(entry.path().native()) + PATHSEP "objekty.json";
			if (std::filesystem::is_regular_file(OTTD2FS(index), ec)) indices.push_back(index);
		}
		if (!indices.empty()) break; // the first place that has them
	}
	/* In the same order on every computer: a house in two folders (the colleague's 04, the same block below the snow
	 * in mirne/ and in subarkticke/) is the first folder's. */
	std::sort(indices.begin(), indices.end());
	for (const std::string &index : indices) {
		std::vector<uint8_t> text = ReadWholeFile(index);
		std::string folder = index.substr(0, index.size() - std::string_view("objekty.json").size());
		try {
			nlohmann::json list = nlohmann::json::parse(text.begin(), text.end());
			for (const auto &e : list) {
				if (e.value("druh", std::string("dum")) != "dum" || !e.contains("nahrazuje_house")) continue;
				ThreeDObject obj;
				obj.file = e.at("soubor").get<std::string>();
				obj.name = e.value("jmeno", obj.file);
				/* The house it is: a number, or a list -- a sub-arctic house below the snow line and the same one above
				 * it, the second dressed in the snowy texture named by "snih" beside the .glb, over the same UVs
				 * (the colleague's hra/3d/ODPOVED-OBJEKTY.md, the other climates). */
				std::vector<HouseID> houses;
				const auto &which = e.at("nahrazuje_house");
				if (which.is_array()) {
					for (const auto &h : which) houses.push_back(static_cast<HouseID>(h.get<int>()));
				} else {
					houses.push_back(static_cast<HouseID>(which.get<int>()));
				}
				if (houses.empty()) continue;
				obj.house = houses.front();
				obj.tiles_x = std::max(1, e.value("dlazdic_x", 1));
				obj.tiles_y = std::max(1, e.value("dlazdic_y", 1));
				if (e.contains("varianty")) {
					obj.variants = 0;
					for (const auto &v : e.at("varianty")) obj.variants |= 1 << (v.get<int>() & 3);
				}
				if (!ReadGlb(folder + obj.file, obj)) continue;
				std::vector<uint8_t> snow;
				if (houses.size() > 1 && e.contains("snih")) snow = ReadWholeFile(folder + e.at("snih").get<std::string>());
				for (size_t k = 1; k < houses.size(); k++) {
					/* The same shape for the next house of the list; the last ones (above the snow) in the snowy texture. */
					ThreeDObject other = obj;
					other.house = houses[k];
					other.name = obj.name + (snow.empty() ? "" : " snih");
					if (!snow.empty()) other.png = snow;
					_three_d_objects.push_back(std::move(other));
				}
				_three_d_objects.push_back(std::move(obj));
			}
		} catch (const std::exception &ex) {
			Debug(misc, 0, "3D objects {}: {}", index, ex.what());
		}
	}
	for (size_t i = 0; i < _three_d_objects.size(); i++) {
		const ThreeDObject &o = _three_d_objects[i];
		for (uint slot = 0; slot < 4; slot++) {
			if (o.variants & (1 << slot)) _three_d_house_objects.emplace(std::make_pair(o.house, slot), i);
		}
	}
	Debug(misc, 1, "3D objects: {} read from {} index(es)", _three_d_objects.size(), indices.size());
}

/**
 * The 3D object of a house of the original game, if there is one.
 * @param north_house the house of its north tile
 * @param colour_slot the colour variant of its north tile (TileHash2Bit() of the tile)
 */
const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	auto it = _three_d_house_objects.find(std::make_pair(north_house, colour_slot & 3));
	return it == _three_d_house_objects.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * The object's texture at a level of detail: level 0 as in the file, every next one halved (the colour of the
 * pixels that have one, a pixel transparent when most of its four are). Read from the PNG the first time.
 */
const ThreeDObjectTexture *ThreeDObject::GetTexture(int level) const
{
	if (this->levels.empty()) {
		ThreeDObjectTexture t;
#ifdef WITH_PNG
		if (!this->png.empty()) {
			png_image image{};
			image.version = PNG_IMAGE_VERSION;
			if (png_image_begin_read_from_memory(&image, this->png.data(), this->png.size())) {
				image.format = PNG_FORMAT_RGBA;
				std::vector<uint8_t> rgba(PNG_IMAGE_SIZE(image));
				if (png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) {
					t.w = image.width;
					t.h = image.height;
					t.px.resize(static_cast<size_t>(t.w) * t.h);
					for (size_t i = 0; i < t.px.size(); i++) {
						const uint8_t *p = &rgba[i * 4];
						t.px[i] = p[3] >= 128 ? Colour(p[0], p[1], p[2]).data : 0;
					}
				} else {
					png_image_free(&image);
				}
			}
		}
#endif /* WITH_PNG */
		if (t.px.empty()) {
			/* No picture: a plain grey, so the shape still shows. */
			t.w = t.h = 1;
			t.px.assign(1, Colour(150, 150, 150).data);
		}
		this->levels.push_back(std::move(t));
		while (this->levels.back().w > 1 || this->levels.back().h > 1) {
			const ThreeDObjectTexture &a = this->levels.back();
			ThreeDObjectTexture b;
			b.w = std::max(1, a.w / 2);
			b.h = std::max(1, a.h / 2);
			b.px.resize(static_cast<size_t>(b.w) * b.h);
			for (int y = 0; y < b.h; y++) {
				for (int x = 0; x < b.w; x++) {
					uint r = 0, g = 0, bl = 0, n = 0;
					for (int k = 0; k < 4; k++) {
						int sx = std::min(a.w - 1, x * 2 + (k & 1)), sy = std::min(a.h - 1, y * 2 + (k >> 1));
						uint32_t p = a.px[static_cast<size_t>(sy) * a.w + sx];
						if (p == 0) continue;
						Colour c(p);
						r += c.r;
						g += c.g;
						bl += c.b;
						n++;
					}
					b.px[static_cast<size_t>(y) * b.w + x] = n >= 2 ? Colour(r / n, g / n, bl / n).data : 0;
				}
			}
			this->levels.push_back(std::move(b));
		}
	}
	return &this->levels[std::clamp<int>(level, 0, static_cast<int>(this->levels.size()) - 1)];
}

/** The rig: the objects read, a line each. */
std::string DescribeThreeDObjects()
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	std::string out = fmt::format("{} objects", _three_d_objects.size());
	for (const ThreeDObject &o : _three_d_objects) {
		out += fmt::format("\n  {} house {:#04x} variants {:#x} {}x{} tiles: {} triangles, {} corners, {:.1f} high, texture {} bytes", o.name, o.house, o.variants, o.tiles_x, o.tiles_y, o.tri.size() / 3, o.pos.size() / 3, o.top, o.png.size());
	}
	return out;
}
