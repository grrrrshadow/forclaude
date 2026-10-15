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
#include "house.h"
#include "map_func.h"
#include "core/bitmath_func.hpp"
#include "engine_base.h"
#include "core/string_consumer.hpp"
#include "3rdparty/nlohmann/json.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <map>
#include <tuple>
#include <unordered_map>

#ifdef WITH_PNG
#include <png.h>
#endif /* WITH_PNG */

#include "safeguards.h"

/** Every object read, and whether they were looked for yet. */
static std::vector<ThreeDObject> _three_d_objects;
static bool _three_d_objects_read = false;
/** The objects by house and colour slot: the index into _three_d_objects. */
static std::map<std::pair<HouseID, uint>, size_t> _three_d_house_objects;
/** The objects of the houses of sets (NewGRF): by the set's id, the house's number in the set and the colour slot. */
static std::map<std::tuple<uint32_t, uint16_t, uint>, size_t> _three_d_grf_house_objects;

static void DecodeTextureLevels(const std::vector<uint8_t> &png, std::vector<ThreeDObjectTexture> &levels, uint32_t flat = 0);

/** The vehicles as objects (three_d_objects.h), and whether their indices were read yet. */
static std::vector<ThreeDVehicleObject> _vehicle_objects;
static bool _vehicle_objects_read = false;
/** The figures of the indices ("druh": "postava"), by their name: people riding in vehicles, standing at stops. */
static std::vector<ThreeDVehicleObject> _figure_objects;
/** The vehicle objects by the set's id, the vehicle type and the engine's number in the set: the object and which of its pieces. */
static std::unordered_map<uint64_t, std::pair<uint32_t, int>> _vehicle_object_pieces;

/** The key of an engine of a set in _vehicle_object_pieces. */
static inline uint64_t VehiclePieceKey(uint32_t grfid, VehicleType type, uint16_t local_id)
{
	return (static_cast<uint64_t>(grfid) << 24) | (static_cast<uint64_t>(to_underlying(type)) << 16) | local_id;
}

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

/** A texture of a .glb: the PNG as it is in the file, or a plain colour where a material has none. */
struct GlbTexture {
	std::vector<uint8_t> png;
	uint32_t flat = 0; ///< the material's base colour factor (Colour::data) when it has no picture
};

/** The meshes of a .glb as read: metres, in the game's axes (x, y, z up), normals when the file has them. */
struct GlbMesh {
	std::vector<float> pos;    ///< x, y, z of each corner
	std::vector<float> nrm;    ///< its normal, or empty
	std::vector<float> uv;     ///< u, v of each corner
	std::vector<uint32_t> tri; ///< three corners a triangle
	std::vector<uint8_t> tri_tex;      ///< the texture of each triangle, an index into textures
	std::vector<GlbTexture> textures;  ///< the textures of the materials
	std::vector<uint8_t> png;  ///< the first material's texture as it is in the file (a building's one texture)
};

/**
 * Read the meshes of a .glb: their triangles, their corners' positions, normals and texture coordinates, the
 * nodes' transforms applied, turned from glTF's axes (Y up, as Blender writes x, z, -y) into the game's, in metres;
 * and the PNG of each material's base colour texture, kept as it is until the object is drawn.
 * @param all_nodes every node of the scene, its children with their parents' transforms, one mesh as often as
 *                  nodes name it (a vehicle: one wheel in the file, at every place it is; Blender's linked
 *                  duplicates); else the first node with a mesh only (a building, as it always was read)
 * @param frame_prefix with all_nodes, a figure that moves (the colleague's hra/3d/PROSBA-POSTAVY-ANIMACE.md): of the
 *                     nodes whose names start so, the frames, only the one numbered frame (and its children)
 * @param frame that frame
 * @return whether there is something to draw
 */
static bool ReadGlbMesh(const std::string &path, GlbMesh &mesh, bool all_nodes = false, std::string_view frame_prefix = {}, int frame = 0)
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
	using Matrix = std::array<double, 16>; // column-major
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

		/* A node's own transform: a matrix, or translation, rotation and scale. */
		auto local = [](const nlohmann::json &node) {
			Matrix m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
			if (node.contains("matrix")) {
				for (int k = 0; k < 16; k++) m[k] = node.at("matrix").at(k).get<double>();
				return m;
			}
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
			return m;
		};
		auto multiply = [](const Matrix &a, const Matrix &b) {
			Matrix c{};
			for (int col = 0; col < 4; col++) {
				for (int row = 0; row < 4; row++) {
					double sum = 0;
					for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
					c[col * 4 + row] = sum;
				}
			}
			return c;
		};

		/* The textures, by the material: a picture in the buffer, or the material's colour. */
		std::map<int, uint8_t> material_texture;
		auto texture_of = [&](int material) -> uint8_t {
			auto it = material_texture.find(material);
			if (it != material_texture.end()) return it->second;
			GlbTexture t;
			t.flat = Colour(150, 150, 150).data;
			if (material >= 0 && j.contains("materials")) {
				const auto &mat = j.at("materials").at(material);
				if (mat.contains("pbrMetallicRoughness")) {
					const auto &pbr = mat.at("pbrMetallicRoughness");
					if (pbr.contains("baseColorFactor")) {
						const auto &f = pbr.at("baseColorFactor");
						auto c = [&](int k) { return static_cast<uint8_t>(std::clamp(f.at(k).get<double>(), 0.0, 1.0) * 255.0 + 0.5); };
						t.flat = Colour(c(0), c(1), c(2)).data;
					}
					if (pbr.contains("baseColorTexture") && j.contains("textures") && j.contains("images")) {
						int tex = pbr.at("baseColorTexture").at("index").get<int>();
						int image = j.at("textures").at(tex).at("source").get<int>();
						const auto &img = j.at("images").at(image);
						if (img.contains("bufferView")) {
							const auto &v = views.at(img.at("bufferView").get<int>());
							size_t offset = v.value("byteOffset", 0), len = v.at("byteLength").get<size_t>();
							if (offset + len <= bin.size()) t.png.assign(bin.begin() + offset, bin.begin() + offset + len);
						}
					}
				}
			}
			uint8_t index = static_cast<uint8_t>(std::min<size_t>(mesh.textures.size(), 255));
			if (mesh.textures.size() < 255) mesh.textures.push_back(std::move(t));
			material_texture.emplace(material, index);
			return index;
		};

		bool normals = true;
		int first_material = -1;
		/* One mesh's primitives, transformed. */
		auto add_mesh = [&](int mesh_index, const Matrix &m) {
			const auto &prims = j.at("meshes").at(mesh_index).at("primitives");
			for (const auto &p : prims) {
				if (p.value("mode", 4) != 4) continue; // triangles only
				std::vector<double> pos, nrm, uv, idx;
				read(p.at("attributes").at("POSITION").get<int>(), 3, pos);
				if (p.at("attributes").contains("TEXCOORD_0")) read(p.at("attributes").at("TEXCOORD_0").get<int>(), 2, uv);
				if (p.at("attributes").contains("NORMAL")) read(p.at("attributes").at("NORMAL").get<int>(), 3, nrm);
				size_t corners = pos.size() / 3;
				if (uv.size() != corners * 2) uv.assign(corners * 2, 0.0);
				if (nrm.size() != corners * 3) normals = false;
				if (p.contains("indices")) {
					read(p.at("indices").get<int>(), 1, idx);
				} else {
					for (size_t i = 0; i < corners; i++) idx.push_back(static_cast<double>(i));
				}
				int material = p.value("material", -1);
				if (first_material < 0 && material >= 0) first_material = material;
				uint8_t tex = texture_of(material);
				uint32_t base = static_cast<uint32_t>(mesh.pos.size() / 3);
				for (size_t i = 0; i < corners; i++) {
					double X = pos[i * 3], Y = pos[i * 3 + 1], Z = pos[i * 3 + 2];
					double gx = m[0] * X + m[4] * Y + m[8] * Z + m[12];
					double gy = m[1] * X + m[5] * Y + m[9] * Z + m[13];
					double gz = m[2] * X + m[6] * Y + m[10] * Z + m[14];
					/* glTF's Y is up and its Z points back along the game's -y. */
					mesh.pos.push_back(static_cast<float>(gx));
					mesh.pos.push_back(static_cast<float>(-gz));
					mesh.pos.push_back(static_cast<float>(gy));
					mesh.uv.push_back(static_cast<float>(uv[i * 2]));
					mesh.uv.push_back(static_cast<float>(uv[i * 2 + 1]));
					if (normals) {
						double nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
						double hx = m[0] * nx + m[4] * ny + m[8] * nz, hy = m[1] * nx + m[5] * ny + m[9] * nz, hz = m[2] * nx + m[6] * ny + m[10] * nz;
						double l = std::sqrt(hx * hx + hy * hy + hz * hz);
						if (l > 0) { hx /= l; hy /= l; hz /= l; }
						mesh.nrm.push_back(static_cast<float>(hx));
						mesh.nrm.push_back(static_cast<float>(-hz));
						mesh.nrm.push_back(static_cast<float>(hy));
					}
				}
				for (size_t i = 0; i + 2 < idx.size(); i += 3) {
					uint32_t a = static_cast<uint32_t>(idx[i]), b = static_cast<uint32_t>(idx[i + 1]), c = static_cast<uint32_t>(idx[i + 2]);
					if (a >= corners || b >= corners || c >= corners) continue;
					mesh.tri.push_back(base + a);
					mesh.tri.push_back(base + b);
					mesh.tri.push_back(base + c);
					mesh.tri_tex.push_back(tex);
				}
			}
		};

		const Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		if (all_nodes && j.contains("nodes")) {
			/* The scene's nodes, each with its parents' transforms; a node only reached as a child is not a root. */
			const auto &nodes = j.at("nodes");
			std::vector<int> roots;
			if (j.contains("scenes") && !j.at("scenes").empty()) {
				const auto &scene = j.at("scenes").at(j.value("scene", 0));
				if (scene.contains("nodes")) for (const auto &n : scene.at("nodes")) roots.push_back(n.get<int>());
			} else {
				std::vector<bool> child(nodes.size(), false);
				for (const auto &n : nodes) {
					if (n.contains("children")) for (const auto &c : n.at("children")) child[c.get<int>()] = true;
				}
				for (size_t i = 0; i < nodes.size(); i++) if (!child[i]) roots.push_back(static_cast<int>(i));
			}
			std::vector<std::pair<int, Matrix>> todo;
			for (int r : roots) todo.emplace_back(r, identity);
			size_t visited = 0;
			while (!todo.empty() && visited++ < 100000) {
				auto [n, parent] = todo.back();
				todo.pop_back();
				const auto &node = nodes.at(n);
				if (!frame_prefix.empty() && node.contains("name") && node.at("name").is_string()) {
					std::string name = node.at("name").get<std::string>();
					if (name.starts_with(frame_prefix)) {
						auto number = ParseInteger<int>(std::string_view(name).substr(frame_prefix.size()), 10);
						if (!number.has_value() || *number != frame) continue;
					}
				}
				Matrix m = multiply(parent, local(node));
				if (node.contains("mesh")) add_mesh(node.at("mesh").get<int>(), m);
				if (node.contains("children")) for (const auto &c : node.at("children")) todo.emplace_back(c.get<int>(), m);
			}
		} else {
			/* The first node with a mesh, its own transform. */
			Matrix m = identity;
			int mesh_index = 0;
			if (j.contains("nodes")) {
				for (const auto &node : j.at("nodes")) {
					if (!node.contains("mesh")) continue;
					mesh_index = node.at("mesh").get<int>();
					m = local(node);
					break;
				}
			}
			add_mesh(mesh_index, m);
		}
		if (!normals || mesh.nrm.size() != mesh.pos.size()) mesh.nrm.clear();
		/* A building's one texture: the first material's. */
		if (first_material >= 0) {
			auto it = material_texture.find(first_material);
			if (it != material_texture.end() && it->second < mesh.textures.size()) mesh.png = mesh.textures[it->second].png;
		}
	} catch (const std::exception &e) {
		Debug(misc, 0, "3D object {}: {}", path, e.what());
		return false;
	}
	return !mesh.tri.empty();
}

/**
 * Read one .glb into a building: its mesh (ReadGlbMesh()) from metres into units, the texture kept as it is.
 * @return whether there is something to draw
 */
static bool ReadGlb(const std::string &path, ThreeDObject &obj)
{
	GlbMesh mesh;
	if (!ReadGlbMesh(path, mesh)) return false;
	obj.pos.reserve(mesh.pos.size());
	for (size_t i = 0; i < mesh.pos.size(); i += 3) {
		obj.pos.push_back(static_cast<float>(mesh.pos[i] * OBJECT_UNITS_PER_METRE));
		obj.pos.push_back(static_cast<float>(mesh.pos[i + 1] * OBJECT_UNITS_PER_METRE));
		obj.pos.push_back(static_cast<float>(mesh.pos[i + 2] * OBJECT_HEIGHT_PER_METRE));
		obj.top = std::max(obj.top, static_cast<float>(mesh.pos[i + 2] * OBJECT_HEIGHT_PER_METRE));
	}
	obj.uv = std::move(mesh.uv);
	obj.tri = std::move(mesh.tri);
	obj.png = std::move(mesh.png);
	if (!mesh.nrm.empty()) {
		/* Normals: lit by the game as the vehicles are, by one light fixed to the world (the colleague's
		 * hra/3d/PROSBA-DOMY-Z-BUDOVY3D.md); its axes are the world's. Without, the light is in the texture. */
		static const double L[3] = {-0.55, 0.35, 0.75};
		const double ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
		obj.light.reserve(mesh.nrm.size() / 3);
		for (size_t i = 0; i + 2 < mesh.nrm.size(); i += 3) {
			double d = (mesh.nrm[i] * L[0] + mesh.nrm[i + 1] * L[1] + mesh.nrm[i + 2] * L[2]) / ll;
			obj.light.push_back(static_cast<uint16_t>(std::lround(256.0 * (0.55 + 0.45 * std::max(0.0, d)))));
		}
	}

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

/**
 * The indices baseset/decouple/objekty/<set>/objekty.json, from the first place the game reads its data from that
 * has them, in the same order on every computer: a house in two folders (the colleague's 04, the same block below
 * the snow in mirne/ and in subarkticke/) is the first folder's.
 */
static std::vector<std::string> FindObjectIndices()
{
	std::vector<std::string> indices;
	for (Searchpath sp : _valid_searchpaths) {
		std::string dir = FioGetDirectory(sp, Subdirectory::Baseset) + "decouple" PATHSEP "objekty";
		std::error_code ec;
		if (!std::filesystem::is_directory(OTTD2FS(dir), ec)) continue;
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(dir), ec)) {
			std::string index = FS2OTTD(entry.path().native()) + PATHSEP "objekty.json";
			if (std::filesystem::is_regular_file(OTTD2FS(index), ec)) indices.push_back(index);
		}
		if (!indices.empty()) break;
	}
	std::sort(indices.begin(), indices.end());
	return indices;
}

/** The parts of a house painted a colour by number ("barvy"), their masks read from beside the index. */
static void ReadColourParts(const nlohmann::json &e, const std::string &folder, ThreeDObject &obj)
{
	if (!e.contains("barvy")) return;
	for (const auto &b : e.at("barvy")) {
		ThreeDObjectColourPart part;
		part.name = b.value("cast", std::string());
		part.mask_png = ReadWholeFile(folder + b.at("maska").get<std::string>());
		part.start = std::clamp(b.value("zacatek", 255), 1, 255);
		for (const auto &c : b.at("paleta")) {
			auto rgb = ParseInteger<uint32_t>(c.get<std::string>(), 16);
			if (rgb.has_value()) part.palette.push_back(Colour(GB(*rgb, 16, 8), GB(*rgb, 8, 8), GB(*rgb, 0, 8)).data);
		}
		if (part.mask_png.empty() || part.palette.empty()) {
			Debug(misc, 0, "3D object {}: the colour part {} has no mask or no colours", obj.name, part.name);
			continue;
		}
		if (obj.colours.size() < 7) obj.colours.push_back(std::move(part));
	}
}

/** The figures standing on a building ("postavy") and its doors ("dvere"). */
static void ReadBuildingFigures(const nlohmann::json &e, ThreeDObject &obj)
{
	if (e.contains("postavy") && e.at("postavy").is_array()) for (const auto &f : e.at("postavy")) {
		if (!f.is_object() || !f.contains("postava") || !f.contains("poloha")) continue;
		const auto &at = f.at("poloha");
		if (!at.is_array() || at.size() != 3) continue;
		ThreeDObjectFigure fig;
		fig.figure = f.at("postava").get<std::string>();
		for (int k = 0; k < 3; k++) fig.at[k] = at.at(k).get<float>();
		fig.facing = f.value("otoceni", 0.0f);
		fig.scale = std::clamp(f.value("meritko", 1.0f), 0.1f, 10.0f);
		obj.figures.push_back(std::move(fig));
	}
	/* Its doors (the colleague's hra/3d/PROSBA-POSTAVY-ANIMACE.md): people come out and go in there. */
	if (e.contains("dvere") && e.at("dvere").is_array()) {
		for (const auto &d : e.at("dvere")) {
			if (!d.is_object() || !d.contains("poloha") || !d.contains("smer")) continue;
			const auto &at = d.at("poloha"), &out = d.at("smer");
			if (!at.is_array() || at.size() < 2 || !out.is_array() || out.size() < 2) continue;
			ThreeDObjectDoor door;
			for (int k = 0; k < 2; k++) {
				door.at[k] = at.at(k).get<float>();
				door.out[k] = out.at(k).get<float>();
			}
			door.width = d.value("sirka", 1.0f);
			obj.doors.push_back(door);
		}
	}
}

/** Look for the buildings under baseset/decouple/objekty/<set>/objekty.json, in every place the game reads its data from. */
static void ReadThreeDObjects()
{
	_three_d_objects_read = true;
	std::vector<std::string> indices = FindObjectIndices();
	for (const std::string &index : indices) {
		std::vector<uint8_t> text = ReadWholeFile(index);
		std::string folder = index.substr(0, index.size() - std::string_view("objekty.json").size());
		try {
			nlohmann::json list = nlohmann::json::parse(text.begin(), text.end());
			for (const auto &e : list) {
				if (e.value("druh", std::string("dum")) != "dum") continue;
				ThreeDObject obj;
				obj.file = e.at("soubor").get<std::string>();
				obj.name = e.value("jmeno", obj.file);
				if (e.contains("nahrazuje_grf") && e.contains("nahrazuje_house_grf")) {
					/* A house of a set (NewGRF): the set's id as the game prints it ("524A450B", the Mars houses, the
					 * colleague's hra/3d/objekty/mars/) and the house's number in the set. */
					std::string text = e.at("nahrazuje_grf").get<std::string>();
					auto printed = ParseInteger<uint32_t>(text, 16);
					if (!printed.has_value()) continue;
					obj.grfid = std::byteswap(*printed);
					obj.grf_house = static_cast<uint16_t>(e.at("nahrazuje_house_grf").get<int>());
					obj.tiles_x = std::max(1, e.value("dlazdic_x", 1));
					obj.tiles_y = std::max(1, e.value("dlazdic_y", 1));
					if (e.contains("varianty")) {
						obj.variants = 0;
						for (const auto &v : e.at("varianty")) obj.variants |= 1 << (v.get<int>() & 3);
					}
					if (!ReadGlb(folder + obj.file, obj)) continue;
					ReadColourParts(e, folder, obj);
					ReadBuildingFigures(e, obj);
					_three_d_objects.push_back(std::move(obj));
					continue;
				}
				if (!e.contains("nahrazuje_house")) continue;
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
				ReadColourParts(e, folder, obj);
				ReadBuildingFigures(e, obj);
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
			if (!(o.variants & (1 << slot))) continue;
			if (o.grfid != 0) {
				_three_d_grf_house_objects.emplace(std::make_tuple(o.grfid, o.grf_house, slot), i);
			} else {
				_three_d_house_objects.emplace(std::make_pair(o.house, slot), i);
			}
		}
	}
	Debug(misc, 1, "3D objects: {} read from {} index(es)", _three_d_objects.size(), indices.size());
}

/**
 * A set's id as the index writes it: four characters as in the set ("MAXb", the colleague's Sergej) or eight hex
 * digits as the game prints it ("524A450B"), into the id as the game keeps it (read from the set little-endian).
 */
static std::optional<uint32_t> ParseIndexGrfId(const std::string &text)
{
	if (text.size() == 8) {
		auto printed = ParseInteger<uint32_t>(text, 16);
		if (printed.has_value()) return std::byteswap(*printed);
	}
	if (text.size() == 4) {
		return static_cast<uint32_t>(static_cast<uint8_t>(text[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8) |
				(static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24);
	}
	return std::nullopt;
}

/** A number of the index: a JSON number, or a string in decimal or "0x" hex ("0x0100", an engine's number in its set). */
static std::optional<uint32_t> ParseIndexNumber(const nlohmann::json &value)
{
	if (value.is_number_integer()) return value.get<uint32_t>();
	if (!value.is_string()) return std::nullopt;
	std::string text = value.get<std::string>();
	if (text.starts_with("0x") || text.starts_with("0X")) return ParseInteger<uint32_t>(std::string_view(text).substr(2), 16);
	return ParseInteger<uint32_t>(text, 10);
}

/** The vehicle type an index names: "vlak", "auto", "lod", "letadlo" (or the game's own words). */
static VehicleType ParseIndexVehicleType(const std::string &text)
{
	if (text == "vlak" || text == "train") return VehicleType::Train;
	if (text == "auto" || text == "silnicni" || text == "road") return VehicleType::Road;
	if (text == "lod" || text == "ship") return VehicleType::Ship;
	if (text == "letadlo" || text == "aircraft") return VehicleType::Aircraft;
	return VehicleType::Invalid;
}

/** Read the vehicles of the indices ("druh": "vozidlo"); their .glb are read when they are first drawn. */
static void ReadThreeDVehicleObjects()
{
	_vehicle_objects_read = true;
	for (const std::string &index : FindObjectIndices()) {
		std::vector<uint8_t> text = ReadWholeFile(index);
		std::string folder = index.substr(0, index.size() - std::string_view("objekty.json").size());
		try {
			nlohmann::json list = nlohmann::json::parse(text.begin(), text.end());
			for (const auto &e : list) {
				if (e.value("druh", std::string("dum")) == "postava" && e.contains("soubor")) {
					/* A figure: metres, the origin between its feet on the ground, facing +x. */
					ThreeDVehicleObject fig;
					fig.file = folder + e.at("soubor").get<std::string>();
					fig.name = e.value("jmeno", e.at("soubor").get<std::string>());
					if (e.contains("animace") && e.at("animace").is_object()) {
						/* It moves: its frames are nodes of the file, drawn one at a time, a frame every so many ticks. */
						const auto &a = e.at("animace");
						fig.frame_prefix = a.value("uzly", std::string("snimek_"));
						fig.frames = std::clamp(a.value("snimku", 0), 0, 64);
						fig.frame_ticks = std::clamp(a.value("tiku", 1), 1, 1000);
						if (fig.frames < 2) fig.frame_prefix.clear();
						if (a.contains("chuze") && a.at("chuze").is_object()) fig.walk_step = std::clamp(a.at("chuze").value("krok_m", 0.0f), 0.0f, 10.0f);
					}
					_figure_objects.push_back(std::move(fig));
					continue;
				}
				if (e.value("druh", std::string("dum")) != "vozidlo") continue;
				ThreeDVehicleObject obj;
				obj.name = e.value("jmeno", e.at("soubor").get<std::string>());
				obj.file = folder + e.at("soubor").get<std::string>();
				obj.type = ParseIndexVehicleType(e.value("typ", std::string("vlak")));
				auto grfid = ParseIndexGrfId(e.value("grf", std::string()));
				if (obj.type == VehicleType::Invalid || !grfid.has_value()) {
					Debug(misc, 0, "3D vehicle {}: no type or set", obj.name);
					continue;
				}
				obj.grfid = *grfid;
				if (e.contains("clanky")) {
					for (const auto &piece : e.at("clanky")) {
						auto n = ParseIndexNumber(piece);
						if (n.has_value()) obj.pieces.push_back(static_cast<uint16_t>(*n));
					}
				}
				if (obj.pieces.empty() && e.contains("motor")) {
					auto n = ParseIndexNumber(e.at("motor"));
					if (n.has_value()) obj.pieces.push_back(static_cast<uint16_t>(*n));
				}
				if (obj.pieces.empty()) {
					Debug(misc, 0, "3D vehicle {}: no engine", obj.name);
					continue;
				}
				/* The piece it hangs from: the middle one, the first, or the one named. */
				std::string anchor = e.contains("kotva") && e.at("kotva").is_string() ? e.at("kotva").get<std::string>() : "stred";
				obj.anchor = anchor == "prvni" || anchor == "predni" ? 0 : (static_cast<int>(obj.pieces.size()) - 1) / 2;
				if (e.contains("kotva") && e.at("kotva").is_number_integer()) obj.anchor = std::clamp(e.at("kotva").get<int>(), 0, static_cast<int>(obj.pieces.size()) - 1);
				/* No "meritko": the object is in true metres, and the size it is drawn at is the game's setting for
				 * all of them (the player, 14. 10.: "the game can make it bigger, so there need not be two files"). */
				if (e.contains("textura") && e.at("textura").is_string()) obj.paint_file = folder + e.at("textura").get<std::string>();
				if (e.contains("kabina") && e.at("kabina").is_array() && e.at("kabina").size() == 3) {
					for (int k = 0; k < 3; k++) obj.cab[k] = e.at("kabina").at(k).get<float>();
					obj.has_cab = true;
				}
				/* The paint: its mask, how it fades with age, a colour by number. */
				if (e.contains("lak") && e.at("lak").is_object()) {
					const auto &lak = e.at("lak");
					if (lak.contains("maska") && lak.at("maska").is_string()) obj.paint.mask_file = folder + lak.at("maska").get<std::string>();
					auto colour = [](const nlohmann::json &v) -> std::optional<uint32_t> {
						if (!v.is_string()) return std::nullopt;
						std::string t = v.get<std::string>();
						if (t.starts_with("#")) t = t.substr(1);
						auto n = t.size() == 6 ? ParseInteger<uint32_t>(t, 16) : std::nullopt;
						if (!n.has_value()) return std::nullopt;
						return Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data;
					};
					if (lak.contains("cil")) {
						auto c = colour(lak.at("cil"));
						if (c.has_value()) {
							obj.paint.fades = true;
							obj.paint.target = *c;
						}
					}
					obj.paint.years = std::max(1, lak.value("let", 20));
					obj.paint.step = std::clamp(lak.value("krok", 1), 1, obj.paint.years);
					obj.paint.start = std::clamp(lak.value("zacatek", 0), 0, 255);
					if (lak.contains("barva")) {
						auto c = colour(lak.at("barva"));
						if (c.has_value()) {
							obj.paint.coloured = true;
							obj.paint.colour = *c;
						}
					}
					if (obj.paint.mask_file.empty()) obj.paint.fades = obj.paint.coloured = false;
				}
				if (e.contains("barva") && !obj.paint.mask_file.empty()) {
					/* The colour by number may stand beside "lak" too. */
					const auto &v = e.at("barva");
					if (v.is_string()) {
						std::string t = v.get<std::string>();
						if (t.starts_with("#")) t = t.substr(1);
						auto n = t.size() == 6 ? ParseInteger<uint32_t>(t, 16) : std::nullopt;
						if (n.has_value()) {
							obj.paint.coloured = true;
							obj.paint.colour = Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data;
						}
					}
				}
				/* The crew: figures riding in it, never fading. */
				if (e.contains("posadka") && e.at("posadka").is_array()) {
					for (const auto &c : e.at("posadka")) {
						if (!c.is_object() || !c.contains("postava") || !c.contains("poloha")) continue;
						ThreeDVehicleCrew crew;
						crew.figure = c.at("postava").get<std::string>();
						const auto &at = c.at("poloha");
						if (!at.is_array() || at.size() != 3) continue;
						for (int k = 0; k < 3; k++) crew.at[k] = at.at(k).get<float>();
						crew.turn = c.value("otoceni", 0.0f);
						obj.crew.push_back(std::move(crew));
					}
				}
				/* Wheels from a file of their own: one wheel at every place, mirrored to the other side. */
				if (e.contains("kola") && e.at("kola").is_array()) {
					for (const auto &k : e.at("kola")) {
						if (!k.is_object() || !k.contains("soubor") || !k.contains("polohy")) continue;
						ThreeDVehicleWheels w;
						w.file = folder + k.at("soubor").get<std::string>();
						w.both_sides = k.value("oba_boky", true);
						for (const auto &p : k.at("polohy")) {
							if (!p.is_array() || p.size() != 3) continue;
							w.at.push_back({p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>()});
						}
						if (!w.at.empty()) obj.wheels.push_back(std::move(w));
					}
				}
				_vehicle_objects.push_back(std::move(obj));
			}
		} catch (const std::exception &ex) {
			Debug(misc, 0, "3D vehicles {}: {}", index, ex.what());
		}
	}
	for (size_t i = 0; i < _vehicle_objects.size(); i++) {
		const ThreeDVehicleObject &o = _vehicle_objects[i];
		for (size_t k = 0; k < o.pieces.size(); k++) {
			_vehicle_object_pieces.emplace(VehiclePieceKey(o.grfid, o.type, o.pieces[k]), std::make_pair(static_cast<uint32_t>(i), static_cast<int>(k)));
		}
	}
	Debug(misc, 1, "3D vehicles: {} in the indices, {} figures", _vehicle_objects.size(), _figure_objects.size());
}

/**
 * The 3D object an engine is drawn as, if there is one.
 * @param engine the engine (of a piece)
 * @param[out] piece which of the object's pieces it is
 */
const ThreeDVehicleObject *GetThreeDVehicleObject(EngineID engine, int *piece)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	if (_vehicle_object_pieces.empty()) return nullptr;
	const Engine *e = Engine::GetIfValid(engine);
	if (e == nullptr || e->grf_prop.grffile == nullptr) return nullptr;
	auto it = _vehicle_object_pieces.find(VehiclePieceKey(e->grf_prop.grfid, e->type, e->grf_prop.local_id));
	if (it == _vehicle_object_pieces.end()) return nullptr;
	if (piece != nullptr) *piece = it->second.second;
	return &_vehicle_objects[it->second.first];
}

/** A figure of the indices by its name ("druh": "postava"), or nullptr. */
const ThreeDVehicleObject *GetThreeDFigure(std::string_view name)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	for (const ThreeDVehicleObject &f : _figure_objects) {
		if (f.name == name) return &f;
	}
	return nullptr;
}

/** The shapes read, by their file and wheels: the paints of a vehicle share one. */
static std::map<std::string, std::shared_ptr<const ThreeDVehicleShape>> _vehicle_shapes;

/** The frame of a figure that moves at a tick of the game: one every frame_ticks, from phase on, round and round. */
int ThreeDVehicleObject::FrameAt(uint64_t tick, uint32_t phase) const
{
	if (this->frames < 2) return 0;
	return static_cast<int>((tick / this->frame_ticks + phase) % this->frames);
}

/** The object's shape, read from its .glb the first time; nullptr when it cannot be. A figure that moves: its first frame. */
const ThreeDVehicleShape *ThreeDVehicleObject::GetShape() const
{
	if (this->shape != nullptr || this->shape_tried) return this->shape.get();
	this->shape_tried = true;
	this->shape = this->MakeShape(0);
	return this->shape.get();
}

/** A frame of a figure that moves (FrameAt()), read the first time it is drawn; the others as GetShape(). */
const ThreeDVehicleShape *ThreeDVehicleObject::GetShape(int frame) const
{
	if (this->frames < 2 || frame <= 0) return this->GetShape();
	frame %= this->frames;
	if (this->frame_shapes.size() < static_cast<size_t>(this->frames)) this->frame_shapes.resize(this->frames);
	if (this->frame_shapes[frame] == nullptr) {
		this->frame_shapes[frame] = this->MakeShape(frame);
		if (this->frame_shapes[frame] == nullptr) return this->GetShape();
	}
	return this->frame_shapes[frame].get();
}

/** Read the object's shape (a frame of it), or the one read already for the same file, wheels, crew and frame. */
std::shared_ptr<const ThreeDVehicleShape> ThreeDVehicleObject::MakeShape(int frame) const
{
	std::string key = this->file;
	if (!this->frame_prefix.empty()) key += fmt::format("#{}{}", this->frame_prefix, frame);
	for (const ThreeDVehicleWheels &w : this->wheels) {
		key += fmt::format("|{}{}", w.file, w.both_sides ? "*2" : "");
		for (const auto &at : w.at) key += fmt::format(",{},{},{}", at[0], at[1], at[2]);
	}
	for (const ThreeDVehicleCrew &c : this->crew) key += fmt::format("|{}@{},{},{}/{}", c.figure, c.at[0], c.at[1], c.at[2], c.turn);
	auto it = _vehicle_shapes.find(key);
	if (it != _vehicle_shapes.end()) return it->second;
	GlbMesh mesh;
	if (!ReadGlbMesh(this->file, mesh, true, this->frame_prefix, frame)) return nullptr;
	auto made = std::make_shared<ThreeDVehicleShape>();
	made->pos = std::move(mesh.pos);
	made->nrm = std::move(mesh.nrm);
	made->uv = std::move(mesh.uv);
	made->tri = std::move(mesh.tri);
	made->tri_tex = std::move(mesh.tri_tex);
	for (GlbTexture &t : mesh.textures) {
		made->pngs.push_back(std::move(t.png));
		made->flats.push_back(t.flat);
	}
	/* The wheels of their own files: each place a copy, the other side mirrored (y the other way, its normals too). */
	size_t wheel_triangles = 0;
	for (const ThreeDVehicleWheels &w : this->wheels) {
		GlbMesh wheel;
		if (!ReadGlbMesh(w.file, wheel, true)) {
			Debug(misc, 0, "3D vehicle {}: the wheel {} cannot be read", this->name, w.file);
			continue;
		}
		const bool lit = !made->nrm.empty() && !wheel.nrm.empty();
		if (!lit) made->nrm.clear();
		uint8_t first_tex = static_cast<uint8_t>(std::min<size_t>(made->pngs.size(), 255));
		for (GlbTexture &t : wheel.textures) {
			if (made->pngs.size() >= 255) break;
			made->pngs.push_back(t.png);
			made->flats.push_back(t.flat);
		}
		for (const auto &at : w.at) {
			for (int side = 0; side < (w.both_sides && std::abs(at[1]) > 0.001f ? 2 : 1); side++) {
				const float sy = side == 0 ? 1.0f : -1.0f;
				uint32_t base = static_cast<uint32_t>(made->pos.size() / 3);
				for (size_t i = 0; i < wheel.pos.size(); i += 3) {
					made->pos.push_back(at[0] + wheel.pos[i]);
					made->pos.push_back(sy * (at[1] + wheel.pos[i + 1]));
					made->pos.push_back(at[2] + wheel.pos[i + 2]);
					if (lit) {
						made->nrm.push_back(wheel.nrm[i]);
						made->nrm.push_back(sy * wheel.nrm[i + 1]);
						made->nrm.push_back(wheel.nrm[i + 2]);
					}
				}
				made->uv.insert(made->uv.end(), wheel.uv.begin(), wheel.uv.end());
				for (size_t t = 0; t < wheel.tri.size(); t += 3) {
					made->tri.push_back(base + wheel.tri[t]);
					made->tri.push_back(base + wheel.tri[t + 1]);
					made->tri.push_back(base + wheel.tri[t + 2]);
					made->tri_tex.push_back(static_cast<uint8_t>(std::min<int>(255, first_tex + wheel.tri_tex[t / 3])));
					wheel_triangles++;
				}
			}
		}
	}
	/* The crew: each figure's shape turned about z and stood at its place, its textures after the others. */
	for (const ThreeDVehicleCrew &c : this->crew) {
		const ThreeDVehicleObject *fig = GetThreeDFigure(c.figure);
		const ThreeDVehicleShape *fs = fig != nullptr ? fig->GetShape() : nullptr;
		if (fs == nullptr) {
			Debug(misc, 0, "3D vehicle {}: the figure {} is not there", this->name, c.figure);
			continue;
		}
		const bool lit = !made->nrm.empty() && !fs->nrm.empty();
		if (!lit) made->nrm.clear();
		uint8_t first_tex = static_cast<uint8_t>(std::min<size_t>(made->pngs.size(), 255));
		for (size_t t = 0; t < fs->pngs.size() && made->pngs.size() < 255; t++) {
			made->pngs.push_back(fs->pngs[t]);
			made->flats.push_back(fs->flats[t]);
		}
		const float a = c.turn * static_cast<float>(M_PI) / 180.0f, ca = std::cos(a), sa = std::sin(a);
		uint32_t base = static_cast<uint32_t>(made->pos.size() / 3);
		for (size_t i = 0; i < fs->pos.size(); i += 3) {
			made->pos.push_back(c.at[0] + fs->pos[i] * ca - fs->pos[i + 1] * sa);
			made->pos.push_back(c.at[1] + fs->pos[i] * sa + fs->pos[i + 1] * ca);
			made->pos.push_back(c.at[2] + fs->pos[i + 2]);
			if (lit) {
				made->nrm.push_back(fs->nrm[i] * ca - fs->nrm[i + 1] * sa);
				made->nrm.push_back(fs->nrm[i] * sa + fs->nrm[i + 1] * ca);
				made->nrm.push_back(fs->nrm[i + 2]);
			}
		}
		made->uv.insert(made->uv.end(), fs->uv.begin(), fs->uv.end());
		for (size_t t = 0; t < fs->tri.size(); t += 3) {
			made->tri.push_back(base + fs->tri[t]);
			made->tri.push_back(base + fs->tri[t + 1]);
			made->tri.push_back(base + fs->tri[t + 2]);
			made->tri_tex.push_back(static_cast<uint8_t>(std::min<int>(255, first_tex + (t / 3 < fs->tri_tex.size() ? fs->tri_tex[t / 3] : 0))));
		}
	}
	made->x0 = made->y0 = made->z0 = 1e9f;
	made->x1 = made->y1 = made->z1 = -1e9f;
	for (size_t i = 0; i < made->pos.size(); i += 3) {
		made->x0 = std::min(made->x0, made->pos[i]); made->x1 = std::max(made->x1, made->pos[i]);
		made->y0 = std::min(made->y0, made->pos[i + 1]); made->y1 = std::max(made->y1, made->pos[i + 1]);
		made->z0 = std::min(made->z0, made->pos[i + 2]); made->z1 = std::max(made->z1, made->pos[i + 2]);
	}
	Debug(misc, 1, "3D vehicle {}{}: {} triangles ({} of them wheels of their own files), {} corners{}, {} textures, {:.2f} x {:.2f} x {:.2f} m", this->name,
			this->frame_prefix.empty() ? std::string() : fmt::format(" frame {}", frame), made->tri.size() / 3, wheel_triangles,
			made->pos.size() / 3, made->nrm.empty() ? " without normals" : "", made->pngs.size(), made->x1 - made->x0, made->y1 - made->y0, made->z1 - made->z0);
	_vehicle_shapes.emplace(key, made);
	return made;
}

/**
 * One of the object's textures at a level of detail, read the first time: the body's (0) is the paint
 * ("textura") when there is one, else the file's; the others the file's and the wheels'.
 */
const ThreeDObjectTexture *ThreeDVehicleObject::GetTexture(int level, int index) const
{
	const ThreeDVehicleShape *s = this->GetShape();
	size_t count = s != nullptr ? std::max<size_t>(1, s->pngs.size()) : 1;
	if (index < 0 || static_cast<size_t>(index) >= count) index = 0;
	if (this->levels.size() < count) this->levels.resize(count);
	std::vector<ThreeDObjectTexture> &lv = this->levels[index];
	if (lv.empty()) {
		std::vector<uint8_t> png;
		uint32_t flat = 0;
		if (index == 0 && !this->paint_file.empty()) png = ReadWholeFile(this->paint_file);
		if (png.empty() && s != nullptr && static_cast<size_t>(index) < s->pngs.size()) {
			png = s->pngs[index];
			flat = s->flats[index];
		}
		DecodeTextureLevels(png, lv, flat);
	}
	return &lv[std::clamp<int>(level, 0, static_cast<int>(lv.size()) - 1)];
}

/** The paint mask ("lak": "maska") at a level of detail, its grey in the red; nullptr without one. */
const ThreeDObjectTexture *ThreeDVehicleObject::GetPaintMask(int level) const
{
	if (this->paint.mask_file.empty()) return nullptr;
	if (!this->mask_tried) {
		this->mask_tried = true;
		std::vector<uint8_t> png = ReadWholeFile(this->paint.mask_file);
		if (!png.empty()) DecodeTextureLevels(png, this->mask_levels, Colour(0, 0, 0).data);
	}
	if (this->mask_levels.empty()) return nullptr;
	return &this->mask_levels[std::clamp<int>(level, 0, static_cast<int>(this->mask_levels.size()) - 1)];
}

/** The rig: the vehicle objects of the indices, a line each. */
std::string DescribeThreeDVehicleObjects()
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	std::string out = fmt::format("{} vehicle objects", _vehicle_objects.size());
	for (const ThreeDVehicleObject &o : _vehicle_objects) {
		std::string pieces;
		for (uint16_t p : o.pieces) pieces += fmt::format(" {:#06x}", p);
		std::string engines;
		for (uint16_t p : o.pieces) {
			EngineID id = _engine_mngr.GetID(o.type, p, o.grfid);
			engines += id == EngineID::Invalid() ? " -" : fmt::format(" {}", id.base());
		}
		out += fmt::format("\n  {} grf {:08X} type {} pieces{} (engines{}) anchor {}{}", o.name, std::byteswap(o.grfid), to_underlying(o.type), pieces, engines, o.anchor, o.paint_file.empty() ? "" : " paint");
	}
	return out;
}

/**
 * The 3D object of a house of the original game, if there is one.
 * @param north_house the house of its north tile
 * @param colour_slot the colour variant of its north tile (TileHash2Bit() of the tile)
 */
const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot)
{
	/* Kept as the first 3D of this game drew it, the boxes of its hand table, in memory of it (the player, 14. 10.:
	 * "I am nostalgic, the white house of many storeys, tall, I would keep"): the tall office block. */
	static constexpr HouseID KEPT_AS_HAND_TABLE[] = {0x1c};
	if (std::ranges::find(KEPT_AS_HAND_TABLE, north_house) != std::end(KEPT_AS_HAND_TABLE)) return nullptr;
	if (!_three_d_objects_read) ReadThreeDObjects();
	auto it = _three_d_house_objects.find(std::make_pair(north_house, colour_slot & 3));
	return it == _three_d_house_objects.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * The 3D object of any house, an original's or a set's (NewGRF): a set's house by the set's id and its number in
 * the set (the Mars houses, the game's own Mars towns).
 * @param north_house the house of its north tile
 * @param colour_slot the colour variant of its north tile (TileHash2Bit() of the tile)
 */
const ThreeDObject *GetThreeDHouseObjectAny(HouseID north_house, uint colour_slot)
{
	if (north_house < NEW_HOUSE_OFFSET) return GetThreeDHouseObject(north_house, colour_slot);
	if (!_three_d_objects_read) ReadThreeDObjects();
	const HouseSpec *hs = HouseSpec::Get(north_house);
	if (hs == nullptr || hs->grf_prop.grffile == nullptr) return nullptr;
	auto it = _three_d_grf_house_objects.find(std::make_tuple(hs->grf_prop.grfid, hs->grf_prop.local_id, colour_slot & 3));
	return it == _three_d_grf_house_objects.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * A texture's levels of detail: level 0 as in the PNG, every next one halved (the colour of the pixels that have one,
 * a pixel transparent when most of its four are). A texture that cannot be read is a plain grey, so the shape still
 * shows.
 */
static void DecodeTextureLevels(const std::vector<uint8_t> &png, std::vector<ThreeDObjectTexture> &levels, uint32_t flat)
{
	ThreeDObjectTexture t;
#ifdef WITH_PNG
	if (!png.empty()) {
		png_image image{};
		image.version = PNG_IMAGE_VERSION;
		if (png_image_begin_read_from_memory(&image, png.data(), png.size())) {
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
		/* No picture: the material's colour, or a plain grey, so the shape still shows. */
		t.w = t.h = 1;
		t.px.assign(1, flat != 0 ? flat : Colour(150, 150, 150).data);
	}
	levels.push_back(std::move(t));
	while (levels.back().w > 1 || levels.back().h > 1) {
		const ThreeDObjectTexture &a = levels.back();
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
		levels.push_back(std::move(b));
	}
}

/** The painted textures kept, bytes; past PAINTED_BUDGET all are dropped and made again as they are drawn. */
static size_t _painted_bytes = 0;
static constexpr size_t PAINTED_BUDGET = static_cast<size_t>(192) << 20;

/**
 * The colours of the house whose north tile is a tile ("barvy", the colleague: "each house another"): for each part
 * a colour of its palette by a hash of the tile, another bit of it for each part; 0 for a house without parts.
 * @return the colours' numbers, four bits a part, with the top bit set
 */
uint32_t ThreeDObject::PaintAt(TileIndex origin) const
{
	if (this->colours.empty()) return 0;
	uint32_t h = TileX(origin) * 0x9E3779B1u ^ TileY(origin) * 0x85EBCA77u;
	uint32_t paint = 1u << 31;
	for (size_t k = 0; k < this->colours.size(); k++) {
		uint32_t x = h ^ static_cast<uint32_t>(k + 1) * 0x27D4EB2Fu;
		x ^= x >> 15;
		x *= 0x2C1B3C6Du;
		x ^= x >> 12;
		x *= 0x297A2D39u;
		x ^= x >> 15;
		paint |= (x % std::min<size_t>(this->colours[k].palette.size(), 16)) << (4 * k);
	}
	return paint;
}

/**
 * The object's texture at a level of detail (DecodeTextureLevels()), read from the PNG the first time; painted
 * (PaintAt()) as the vehicles' colour by number: each pixel of a part's mask the part's colour at the texture's
 * lightness, its own where the texture is "zacatek" light.
 */
const ThreeDObjectTexture *ThreeDObject::GetTexture(int level, uint32_t paint) const
{
	if (this->levels.empty()) DecodeTextureLevels(this->png, this->levels);
	level = std::clamp<int>(level, 0, static_cast<int>(this->levels.size()) - 1);
	const ThreeDObjectTexture *plain = &this->levels[level];
	if (paint == 0 || this->colours.empty()) return plain;
	auto it = this->painted.find({level, paint});
	if (it != this->painted.end()) return &it->second;

	if (_painted_bytes > PAINTED_BUDGET) {
		for (const ThreeDObject &o : _three_d_objects) o.painted.clear();
		_painted_bytes = 0;
	}
	ThreeDObjectTexture t = *plain;
	for (size_t k = 0; k < this->colours.size(); k++) {
		const ThreeDObjectColourPart &part = this->colours[k];
		if (part.levels.empty()) DecodeTextureLevels(part.mask_png, part.levels);
		/* The mask's level nearest the texture's, read at the same place. */
		const ThreeDObjectTexture *mask = &part.levels.back();
		for (const ThreeDObjectTexture &m : part.levels) {
			if (m.w <= t.w) {
				mask = &m;
				break;
			}
		}
		const Colour colour(part.palette[GB(paint, static_cast<uint8_t>(4 * k), 4) % part.palette.size()]);
		const float start = static_cast<float>(part.start);
		for (int y = 0; y < t.h; y++) {
			const int my = std::min(mask->h - 1, y * mask->h / t.h);
			for (int x = 0; x < t.w; x++) {
				uint32_t &p = t.px[static_cast<size_t>(y) * t.w + x];
				if (p == 0) continue;
				const int mx = std::min(mask->w - 1, x * mask->w / t.w);
				const uint32_t mp = mask->px[static_cast<size_t>(my) * mask->w + mx];
				const float m = mp == 0 ? 0.0f : Colour(mp).r / 255.0f;
				if (m <= 0) continue;
				Colour c(p);
				float l = std::max({c.r, c.g, c.b}) / start;
				auto mix = [&](uint8_t own, uint8_t to) { return static_cast<uint8_t>(std::clamp(own + (std::min(255.0f, to * l) - own) * m, 0.0f, 255.0f)); };
				p = Colour(mix(c.r, colour.r), mix(c.g, colour.g), mix(c.b, colour.b)).data;
			}
		}
	}
	_painted_bytes += t.px.size() * sizeof(uint32_t);
	return &this->painted.emplace(std::make_pair(level, paint), std::move(t)).first->second;
}

/** The rig: the objects read, a line each. */
std::string DescribeThreeDObjects()
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	std::string out = fmt::format("{} objects", _three_d_objects.size());
	for (const ThreeDObject &o : _three_d_objects) {
		std::string which = o.grfid != 0 ? fmt::format("grf {:08X} house {}", std::byteswap(o.grfid), o.grf_house) : fmt::format("house {:#04x}", o.house);
		out += fmt::format("\n  {} {} variants {:#x} {}x{} tiles: {} triangles, {} corners, {:.1f} high, texture {} bytes{}", o.name, which, o.variants, o.tiles_x, o.tiles_y, o.tri.size() / 3, o.pos.size() / 3, o.top, o.png.size(), o.light.empty() ? "" : ", lit from its normals");
		for (const ThreeDObjectColourPart &part : o.colours) out += fmt::format(", colour {} of {}", part.name, part.palette.size());
		for (const ThreeDObjectFigure &f : o.figures) out += fmt::format(", {} at {:.1f},{:.1f},{:.1f}", f.figure, f.at[0], f.at[1], f.at[2]);
	}
	return out;
}
