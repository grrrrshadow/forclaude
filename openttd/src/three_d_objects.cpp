/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file three_d_objects.cpp Reading buildings as real 3D objects (three_d_objects.h): the index, the .glb files, their textures. */

#include "stdafx.h"
#include "three_d_objects.h"
#include "simtrains.h"
#include "beamng.h"
#include "industry.h"
#include "climate_industries.h"
#include "industry_map.h"
#include "industrytype.h"
#include "three_d_vehicle_object.h"
#include "cab_view.h"
#include "debug.h"
#include "fileio_func.h"
#include "sound_func.h"
#include "newgrf_config.h"
#include "tar_type.h"
#include "gfx_type.h"
#include "tile_type.h"
#include "house.h"
#include "map_func.h"
#include "core/bitmath_func.hpp"
#include "engine_base.h"
#include "vehicle_base.h"
#include "core/string_consumer.hpp"
#include "three_d.h"
#include "town.h"
#include "town_map.h"
#include "tile_map.h"
#include "slope_func.h"
#include "language.h"
#include "3rdparty/nlohmann/json.hpp"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "timer/timer_game_calendar.h"
#include "settings_type.h"
#include "error.h"
#include "strings_func.h"
#include "window_func.h"
#include "table/strings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef WITH_PNG
#include <png.h>
#endif /* WITH_PNG */

#include "walker_inventory.h"

#include "safeguards.h"

/** Every object read, and whether they were looked for yet. */
static std::vector<ThreeDObject> _three_d_objects;
static bool _three_d_objects_read = false;
/** The objects by the town theme (TownTheme, 0 every town), the house and the colour slot: the index into _three_d_objects. */
static std::map<std::tuple<uint8_t, HouseID, uint>, size_t> _three_d_house_objects;
/** The creatures of the indices ("druh": "tvor", the colleague's dragons). */
static std::vector<ThreeDCreature> _creatures;
/** The witches of the indices ("druh": "carodejnice"). */
static std::vector<ThreeDWitch> _witches;
/** The night flyers an index of ours takes out of the sky ("nahrazuje": the colleague's ghost floats along the streets instead). */
static std::set<std::string, std::less<>> _witches_replaced;
/** The trees of each town theme ("druh": "strom" with "tema", the colleague's mushrooms): the names of their figures. */
static std::map<uint8_t, std::vector<std::string>> _three_d_theme_trees;
/** And the same at night: each one glowing ("<jmeno>/noc") where it has its night. */
static std::map<uint8_t, std::vector<std::string>> _three_d_theme_trees_night;
/** The dogs, the animals, the ghosts and the sellers of dogs of the indices (ThreeDAnimal). */
static std::vector<ThreeDAnimal> _animals;
/** The mushrooms to pick of the indices (ThreeDMushroom). */
static std::vector<ThreeDMushroom> _mushrooms;

/** The town theme an index names ("tema"): "hlinene" the clay town (TownTheme::Clay), 0 none or one the game does not know. */
static uint8_t ParseIndexTheme(const nlohmann::json &e)
{
	const std::string theme = e.value("tema", std::string());
	if (theme == "hlinene") return to_underlying(TownTheme::Clay);
	return 0;
}
/** The objects of the houses of sets (NewGRF): by the set's id, the house's number in the set and the colour slot. */
static std::map<std::tuple<uint32_t, uint16_t, uint>, size_t> _three_d_grf_house_objects;
/** The game's own industries' objects by the industry and the variant (-1 any), and the parts of the fields by their name ("dil"). */
static std::map<std::pair<IndustryType, int8_t>, size_t> _three_d_industry_objects;
/** The objects the game's industries of another kind show ("sdili": the refinery the distillery), by the industry. */
static std::map<IndustryType, size_t> _three_d_shared_industry_objects;
static std::map<std::string, size_t, std::less<>> _three_d_field_parts;
/** The add-ons of the buildings ("druh": "doplnek"), by their name: their objects in _three_d_objects (ThreeDObject::addon_places). */
static std::map<std::string, size_t, std::less<>> _three_d_addons;
/** The street lamps (MakeStreetLamps()) by the place of the game's street light on its tile, dx * 16 + dy: objects in _three_d_objects. */
static std::map<int, size_t> _three_d_street_lamps;

/*
 * The diary of the 3D houses, 3d_domy.log in the player's folder (the player, 19. 10.: in a new game the temperate
 * houses are the boxes, the clay towns' are 3D): where each index was read from, every file that could not be read
 * and why, and every set (NewGRF) whose houses have no 3D objects (ITL Houses, which takes the place of all the
 * game's houses) -- such a house is drawn as the boxes, so far without a word. The first such problem the player is
 * told of in the game, once.
 */
static std::mutex _read_diary_mutex;
static std::optional<FileHandle> _read_diary;
static std::string _read_problem;      ///< the first problem, to tell the player
static bool _read_problem_told = false;
static std::set<uint32_t> _boxed_house_sets; ///< the sets whose houses were told of as drawn without objects

/** A line of the diary; a problem (a file not read) told to the player if it is the first. */
static void ReadDiary(std::string_view line, bool problem)
{
	std::lock_guard lock(_read_diary_mutex);
	if (!_read_diary.has_value()) _read_diary = FileHandle::Open(_personal_dir + "3d_domy.log", "w");
	if (_read_diary.has_value()) {
		std::string text(line);
		text += '\n';
		fwrite(text.data(), 1, text.size(), *_read_diary);
		fflush(*_read_diary);
	}
	if (problem && _read_problem.empty()) _read_problem = line;
}

/**
 * A house of a set (NewGRF) drawn in the turned view without a 3D object (no package of the set has it): its set
 * told of, once.
 * @param house the house of its north tile, one of a set
 */
void NoteThreeDHouseBoxed(HouseID house)
{
	const HouseSpec *hs = HouseSpec::Get(house);
	if (hs == nullptr || hs->grf_prop.grffile == nullptr) return;
	const uint32_t grfid = hs->grf_prop.grfid;
	/* (Asked for every such tile drawn: the set of the last one at once, without the lock.) */
	static std::atomic<uint32_t> last{0};
	if (last.load(std::memory_order_relaxed) == grfid) return;
	{
		std::lock_guard lock(_read_diary_mutex);
		last.store(grfid, std::memory_order_relaxed);
		if (!_boxed_house_sets.insert(grfid).second) return;
	}
	/* (The game's own houses, which have the objects, a set may switch off: ITL Houses all of them.) */
	uint originals = 0;
	for (HouseID h = 0; h < NEW_HOUSE_OFFSET; h++) {
		if (HouseSpec::Get(h)->enabled) originals++;
	}
	const GRFConfig *config = GetGRFConfig(grfid);
	const std::string name = config != nullptr ? config->GetName() : fmt::format("{:08X}", std::byteswap(grfid));
	ReadDiary(fmt::format("domy z GRF {} nemají 3D model, kreslí se krabice; zapnutých původních domů hry (ty jsou 3D): {}", name, originals), true);
}

/** The player told once, in a window, of the first file of a 3D object not read (and so drawn as the boxes). */
static const IntervalTimer<TimerWindow> _read_problem_timer(std::chrono::seconds(2), [](auto) {
	std::string problem;
	{
		std::lock_guard lock(_read_diary_mutex);
		if (_read_problem.empty() || _read_problem_told) return;
		if (FindWindowById(WindowClass::MainWindow, 0) == nullptr) return;
		_read_problem_told = true;
		problem = _read_problem;
	}
	ShowErrorMessage(GetEncodedString(STR_ERROR_THREE_D_BOXES, problem), {}, WarningLevel::Warning);
});

static void DecodeTextureLevels(const std::vector<uint8_t> &png, std::vector<ThreeDObjectTexture> &levels, uint32_t flat = 0, bool halved = true, const std::vector<float> *cover = nullptr);
/** A texture's bytes that are its pixels as they are and not a PNG begin so (DecodeTextureLevels()): a model of MSTS's. */
static constexpr std::string_view RAW_TEXTURE_MARK = "OTTD-RGBA";

/** The vehicles as objects (three_d_objects.h), and whether their indices were read yet. */
static std::vector<ThreeDVehicleObject> _vehicle_objects;
static bool _vehicle_objects_read = false;
/** The figures of the indices ("druh": "postava"), by their name: people riding in vehicles, standing at stops. */
static std::vector<ThreeDVehicleObject> _figure_objects;
/** The sounds of the things of the indices ("zvuky"): "<jmeno>/<zvuk>" to its file (a path, or in a tar: IN_TAR, IN_BASESET_TAR). */
static std::map<std::string, std::string> _thing_sounds;
/** The places of the things asked for (GetThreeDThingPlace()): "<jmeno>/<uzel>" to where it is, nothing when it has none. */
static std::map<std::string, std::optional<std::array<float, 3>>> _thing_places;

/** The clock of the objects, seconds (ForgetUnusedThreeDObjects()): what was drawn when. */
static std::atomic<uint32_t> _three_d_clock = 1;

/** Mark a thing drawn now; from the threads making textures and the one making the pictures of the vehicles too, so as an atomic. */
static inline void MarkThreeDUsed(uint32_t &used)
{
	std::atomic_ref<uint32_t>(used).store(_three_d_clock.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
/** The effects of the indices ("druh": "efekt"): the fireworks over the hut. */
static std::vector<ThreeDVehicleObject> _effect_objects;
/** Cars of the game's own that drive about by themselves ("auto" with "jizda", three_d_walkers.h): the ÚV KSČ Tatra. */
static std::vector<ThreeDVehicleObject> _driving_objects;
/** The vehicle objects by the set's id, the vehicle type and the engine's number in the set: the object and which of its pieces. */
static std::unordered_map<uint64_t, std::pair<uint32_t, int>> _vehicle_object_pieces;
/**
 * Every object a piece is of, by the same key, where it is of more than one (the colleague's lorries: the two ends of
 * the trains of lorries, the same two pieces behind every one of five semi-trailers): which of them a vehicle's piece
 * is of is told by the pieces around it (ThreeDVehicleObjectOf()).
 */
static std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int>>> _vehicle_object_shared_pieces;

/** The key of an engine of a set in _vehicle_object_pieces. */
static inline uint64_t VehiclePieceKey(uint32_t grfid, VehicleType type, uint16_t local_id)
{
	return (static_cast<uint64_t>(grfid) << 24) | (static_cast<uint64_t>(to_underlying(type)) << 16) | local_id;
}

/** A file's bytes, or nothing when it cannot be read. */
/** A path of a file in a tar of the NewGRF folders begins so: the rest is its name in the tar (FioFOpenFile()). */
static constexpr std::string_view IN_TAR = "tar:";
/** And of one in a tar in baseset/decouple/, beside the game's own sets. */
static constexpr std::string_view IN_BASESET_TAR = "btar:";

/**
 * Open a file of the objects at its first byte: a path on the disk, or IN_TAR and the name of a file in a tar of the
 * NewGRF folders (a package of a GRF), IN_BASESET_TAR one in a tar of baseset/decouple/.
 * @param[out] len its length
 */
static std::optional<FileHandle> OpenObjectFile(const std::string &path, size_t &len)
{
	len = 0;
	std::optional<FileHandle> f;
	if (path.starts_with(IN_TAR)) {
		f = FioFOpenFile(std::string_view(path).substr(IN_TAR.size()), "rb", Subdirectory::NewGrf, &len);
	} else if (path.starts_with(IN_BASESET_TAR)) {
		f = FioFOpenFile(std::string_view(path).substr(IN_BASESET_TAR.size()), "rb", Subdirectory::Baseset, &len);
	} else {
		f = FileHandle::Open(path, "rb");
		if (f.has_value()) {
			fseek(*f, 0, SEEK_END);
			long end = ftell(*f);
			fseek(*f, 0, SEEK_SET);
			len = end > 0 ? static_cast<size_t>(end) : 0;
		}
	}
	return f;
}

/** A whole file of the objects (OpenObjectFile()). */
static std::vector<uint8_t> ReadWholeFile(const std::string &path)
{
	std::vector<uint8_t> data;
	size_t len = 0;
	std::optional<FileHandle> f = OpenObjectFile(path, len);
	if (!f.has_value() || len == 0 || len > 64 * 1024 * 1024) return data;
	data.resize(len);
	if (fread(data.data(), len, 1, *f) != 1) data.clear();
	return data;
}

/*
 * The .glb files are kept only while something is read from them (GlbFilesScope): an add-on of a vehicle is read for
 * its shape, its lamps and its red lights, the records of a package have the same add-ons (a file of the colleague's
 * lorries is forty megabytes) -- read once for all of them. When it is read, the file is let go of at once, as the other
 * games do (the player, 17. 10.: "when do the other games let go of the glb"); what was made of it stays.
 */
/** The files kept by the reading of this thread, and how deep its reading is. */
static thread_local std::vector<std::pair<std::string, std::shared_ptr<const std::vector<uint8_t>>>> _glb_files_kept;
static thread_local int _glb_files_scope = 0;
/** The bytes of the files kept by all the threads now (the rig, test3d pamet). */
static std::atomic<size_t> _glb_files_kept_bytes = 0;

/** While it lives, the .glb files read on this thread are kept for each other reading of them; at the end let go of. */
struct GlbFilesScope {
	GlbFilesScope() { _glb_files_scope++; }
	~GlbFilesScope();
	GlbFilesScope(const GlbFilesScope &) = delete;
	GlbFilesScope &operator=(const GlbFilesScope &) = delete;
};

/** A .glb file whole (ReadWholeFile()), or as it was read already by the reading going on (GlbFilesScope). */
static std::shared_ptr<const std::vector<uint8_t>> GlbFileBytes(const std::string &path)
{
	for (const auto &[kept, bytes] : _glb_files_kept) {
		if (kept == path) return bytes;
	}
	auto made = std::make_shared<const std::vector<uint8_t>>(ReadWholeFile(path));
	if (_glb_files_scope > 0) {
		_glb_files_kept.emplace_back(path, made);
		_glb_files_kept_bytes += made->size();
	}
	return made;
}

/** The first bytes of a file (as ReadWholeFile() finds it): the header of a picture. */
static std::vector<uint8_t> ReadFileHead(const std::string &path, size_t most)
{
	std::vector<uint8_t> data;
	size_t len = 0;
	std::optional<FileHandle> f = OpenObjectFile(path, len);
	if (!f.has_value()) return data;
	data.resize(std::min(len, most));
	data.resize(fread(data.data(), 1, data.size(), *f));
	return data;
}

/** The little-endian 32 bits at an offset. */
static uint32_t ReadU32(std::span<const uint8_t> d, size_t at)
{
	return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}

/** The bytes of one number of an accessor's component type (glTF's 5120 to 5126). */
static size_t GlbComponentSize(int type)
{
	return type == 5126 || type == 5125 ? 4 : type == 5123 || type == 5122 ? 2 : 1;
}

/**
 * One number of an accessor: a float, or a whole number of any size, signed or not. A normalised one is the fraction of
 * its largest (a signed one -1 at the least): KHR_mesh_quantization (gltfpack, the colleague's hra/3d/PROSBA-TATRY-DOMY-
 * POSTAVY.md) stores the normals and the texture coordinates so, the positions as whole numbers, a node's transform
 * making them metres again.
 * @throw std::invalid_argument for a type glTF does not have
 */
static double ReadGlbNumber(std::span<const uint8_t> d, size_t at, int type, bool normalized)
{
	switch (type) {
		case 5126: return std::bit_cast<float>(ReadU32(d, at));
		case 5125: return ReadU32(d, at);
		case 5123: {
			const double v = d[at] | (d[at + 1] << 8);
			return normalized ? v / 65535.0 : v;
		}
		case 5122: {
			const double v = static_cast<int16_t>(d[at] | (d[at + 1] << 8));
			return normalized ? std::max(v / 32767.0, -1.0) : v;
		}
		case 5121: return normalized ? d[at] / 255.0 : d[at];
		case 5120: {
			const double v = static_cast<int8_t>(d[at]);
			return normalized ? std::max(v / 127.0, -1.0) : v;
		}
		default: throw std::invalid_argument("component type");
	}
}

/**
 * Why a .glb is not read as it is: an extension it requires that the game cannot read -- its meshes compressed
 * (gltfpack's -c, -cc: EXT_meshopt_compression; Draco), its pictures not PNG (gltfpack's -tc: KTX2, Basis; WebP).
 * Quantised meshes (KHR_mesh_quantization) and moved textures (KHR_texture_transform) are read.
 * @return empty when it can be read
 */
static std::string GlbExtensionNotRead(const nlohmann::json &j)
{
	if (!j.contains("extensionsRequired") || !j.at("extensionsRequired").is_array()) return {};
	for (const auto &e : j.at("extensionsRequired")) {
		if (!e.is_string()) continue;
		const std::string name = e.get<std::string>();
		if (name == "EXT_meshopt_compression" || name == "KHR_meshopt_compression" || name == "KHR_draco_mesh_compression" || name == "KHR_texture_basisu" || name == "EXT_texture_webp") {
			return "requires " + name + ", which the game does not read (gltfpack: without -c, -cc and -tc)";
		}
	}
	return {};
}

/**
 * A material's base colour texture: glTF's own, or the diffuse one of the older KHR_materials_pbrSpecularGlossiness,
 * which goes before it where it is (the player, #287: the fennec and a wolf "have no colour"; nine of the colleague's
 * dogs keep their colour so, their base colour only a stand-in). nullptr: none.
 */
static const nlohmann::json *GlbColourTexture(const nlohmann::json &mat)
{
	if (mat.contains("extensions") && mat.at("extensions").contains("KHR_materials_pbrSpecularGlossiness")) {
		const auto &sg = mat.at("extensions").at("KHR_materials_pbrSpecularGlossiness");
		if (sg.contains("diffuseTexture")) return &sg.at("diffuseTexture");
	}
	if (mat.contains("pbrMetallicRoughness") && mat.at("pbrMetallicRoughness").contains("baseColorTexture")) return &mat.at("pbrMetallicRoughness").at("baseColorTexture");
	return nullptr;
}

/** A material's base colour, red, green, blue and alpha (as GlbColourTexture(): the diffuse one first); nullptr: none. */
static const nlohmann::json *GlbColourFactor(const nlohmann::json &mat)
{
	const nlohmann::json *f = nullptr;
	if (mat.contains("extensions") && mat.at("extensions").contains("KHR_materials_pbrSpecularGlossiness")) {
		const auto &sg = mat.at("extensions").at("KHR_materials_pbrSpecularGlossiness");
		if (sg.contains("diffuseFactor")) f = &sg.at("diffuseFactor");
	}
	if (f == nullptr && mat.contains("pbrMetallicRoughness") && mat.at("pbrMetallicRoughness").contains("baseColorFactor")) f = &mat.at("pbrMetallicRoughness").at("baseColorFactor");
	return f != nullptr && f->is_array() && f->size() == 4 ? f : nullptr;
}

/**
 * How a material moves its base colour texture over the coordinates (KHR_texture_transform: gltfpack stores the texture
 * coordinates as fractions of where they lie, the move making them what they were): u' = a[0] + a[2] u + a[3] v,
 * v' = a[1] + a[4] u + a[5] v; {0, 0, 1, 0, 0, 1} when it does not.
 */
static std::array<double, 6> GlbUvTransform(const nlohmann::json &j, int material)
{
	std::array<double, 6> a{0, 0, 1, 0, 0, 1};
	if (material < 0 || !j.contains("materials") || static_cast<size_t>(material) >= j.at("materials").size()) return a;
	const nlohmann::json *colour = GlbColourTexture(j.at("materials").at(material));
	if (colour == nullptr) return a;
	const auto &info = *colour;
	if (!info.contains("extensions") || !info.at("extensions").contains("KHR_texture_transform")) return a;
	const auto &t = info.at("extensions").at("KHR_texture_transform");
	double offset[2] = {0, 0}, scale[2] = {1, 1};
	if (t.contains("offset")) for (int k = 0; k < 2; k++) offset[k] = t.at("offset").at(k).get<double>();
	if (t.contains("scale")) for (int k = 0; k < 2; k++) scale[k] = t.at("scale").at(k).get<double>();
	const double r = t.value("rotation", 0.0), c = std::cos(r), s = std::sin(r);
	/* translation * rotation * scale, the rotation as the extension has it */
	a = {offset[0], offset[1], c * scale[0], s * scale[1], -s * scale[0], c * scale[1]};
	return a;
}

/**
 * The .glb files whose pictures have the background about their pieces filled already ("okraje_textur" of their
 * record, the colleague's hra/3d/ZRYCHLENI-3D-GRF.md): where their triangles draw from is not worked out for them
 * (TextureCover()), nor the rest of the picture filled from it -- for a big atlas hundreds of milliseconds of reading.
 */
static std::set<std::string> _glb_margins_filled[2]; ///< of the buildings' indices, of the vehicles' (each read anew on its own)
static std::mutex _glb_margins_mutex;

/** Which of _glb_margins_filled: the files of the buildings' indices, or of the vehicles' (with their figures and effects). */
enum class MarginsOf : uint8_t { Buildings, Vehicles };

/** Note a file of a record that says its pictures have their margins filled ("okraje_textur": the width, or true). */
static void NoteMarginsFilled(const nlohmann::json &e, const std::string &path, MarginsOf of)
{
	if (!e.contains("okraje_textur")) return;
	const auto &m = e.at("okraje_textur");
	if (!(m.is_number() ? m.get<double>() > 0 : m.is_boolean() && m.get<bool>())) return;
	std::lock_guard lock(_glb_margins_mutex);
	_glb_margins_filled[to_underlying(of)].insert(path);
}

/** The indices of the buildings or of the vehicles are read anew: what they said before goes. */
static void ForgetMarginsFilled(MarginsOf of)
{
	std::lock_guard lock(_glb_margins_mutex);
	_glb_margins_filled[to_underlying(of)].clear();
}

/** Whether a file's pictures have their margins filled already (NoteMarginsFilled()). */
static bool MarginsFilled(const std::string &path)
{
	std::lock_guard lock(_glb_margins_mutex);
	return _glb_margins_filled[0].contains(path) || _glb_margins_filled[1].contains(path);
}

/**
 * A .glb as its meshes are read from it (ReadGlbMesh()): its JSON parsed once, and of its binary buffer only the
 * pieces the meshes and pictures read need (Piece()), read from the file as they are wanted. A file of the colleague's
 * add-ons is forty megabytes of ten trailers, a lorry shows one of them, and all of it was read (on a cold disk five
 * seconds for one lorry). Kept, with what was read of it, while a reading goes on on its thread (GlbFilesScope).
 */
struct GlbFile {
	std::string path;
	nlohmann::json json;
	std::string error;              ///< why it cannot be read; empty when it can
	size_t bin_at = 0, bin_len = 0; ///< its binary chunk: where in the file, how long
	size_t bytes = 0;               ///< the bytes of the pieces read

	explicit GlbFile(const std::string &path) : path(path)
	{
		if (!this->Open()) {
			this->error = "cannot be opened";
			return;
		}
		uint8_t head[20];
		if (this->length < sizeof(head) || fread(head, sizeof(head), 1, *this->file) != 1 || ReadU32(head, 0) != 0x46546C67 || ReadU32(head, 4) != 2 || ReadU32(head, 16) != 0x4E4F534A) {
			this->error = "not a glTF binary";
			return;
		}
		/* The chunks: the JSON, then the binary buffer. */
		const size_t json_len = ReadU32(head, 12);
		if (20 + json_len > this->length) {
			this->error = "its JSON is past its end";
			return;
		}
		std::string text(json_len, '\0');
		if (json_len > 0 && fread(text.data(), json_len, 1, *this->file) != 1) {
			this->error = "its JSON cannot be read";
			return;
		}
		try {
			this->json = nlohmann::json::parse(text);
		} catch (const std::exception &e) {
			this->error = e.what();
			return;
		}
		this->error = GlbExtensionNotRead(this->json);
		if (!this->error.empty()) return;
		const size_t bin_head = 20 + ((json_len + 3) & ~size_t{3});
		uint8_t chunk[8];
		if (bin_head + 8 <= this->length && fseek(*this->file, static_cast<long>(this->base + bin_head), SEEK_SET) == 0 && fread(chunk, sizeof(chunk), 1, *this->file) == 1 && ReadU32(chunk, 4) == 0x004E4942) {
			this->bin_at = bin_head + 8;
			this->bin_len = std::min<size_t>(ReadU32(chunk, 0), this->length - this->bin_at);
		}
	}

	~GlbFile()
	{
		_glb_files_kept_bytes -= this->bytes;
	}

	/**
	 * Bytes of its binary buffer, read the first time they are wanted.
	 * @throw std::out_of_range when they are past its end
	 */
	std::span<const uint8_t> Piece(size_t offset, size_t len)
	{
		if (offset > this->bin_len || len > this->bin_len - offset) throw std::out_of_range("past the buffer");
		for (const auto &[at, piece] : this->pieces) {
			if (at <= offset && offset + len <= at + piece.size()) return std::span<const uint8_t>(piece).subspan(offset - at, len);
		}
		std::vector<uint8_t> piece(len);
		if (len > 0) {
			if (!this->file.has_value() && !this->Open()) throw std::runtime_error("cannot be opened again");
			if (fseek(*this->file, static_cast<long>(this->base + this->bin_at + offset), SEEK_SET) != 0 || fread(piece.data(), len, 1, *this->file) != 1) throw std::runtime_error("cannot be read");
		}
		this->bytes += len;
		_glb_files_kept_bytes += len;
		return this->pieces.emplace_back(offset, std::move(piece)).second;
	}

	/** Let go of the open file (between the readings): what was read of it stays. */
	void Close()
	{
		this->file.reset();
	}

private:
	std::optional<FileHandle> file;
	size_t base = 0;   ///< where its first byte is in what was opened (a file in a tar)
	size_t length = 0;
	std::vector<std::pair<size_t, std::vector<uint8_t>>> pieces;

	bool Open()
	{
		this->file = OpenObjectFile(this->path, this->length);
		if (!this->file.has_value()) return false;
		const long at = ftell(*this->file);
		this->base = at > 0 ? static_cast<size_t>(at) : 0;
		return true;
	}
};

/** The .glb files being read on this thread (GlbFileOf()), kept while the reading goes on (GlbFilesScope). */
static thread_local std::vector<std::shared_ptr<GlbFile>> _glb_read_kept;

/** A .glb to read the meshes of (GlbFile), or as it is being read already by the reading going on. */
static std::shared_ptr<GlbFile> GlbFileOf(const std::string &path)
{
	for (const auto &kept : _glb_read_kept) {
		if (kept->path == path) return kept;
	}
	auto made = std::make_shared<GlbFile>(path);
	if (_glb_files_scope > 0) _glb_read_kept.push_back(made);
	return made;
}

GlbFilesScope::~GlbFilesScope()
{
	if (--_glb_files_scope > 0) return;
	for (const auto &[path, bytes] : _glb_files_kept) _glb_files_kept_bytes -= bytes->size();
	_glb_files_kept.clear();
	_glb_read_kept.clear();
}

/*
 * The textures by need (the player, 17. 10.: "textures, how do the other games do it? then probably yes"): a texture of
 * the colleague's lorries is a picture of sixteen million pixels, halved for each level of detail 85 MB, and the map
 * draws a level two or three coarser. A texture is made with only the levels from the finest asked for down (the finer
 * ones without their pixels, their sizes kept); one asked for finer is made again from its picture (the view from the
 * eyes close to it), what is not asked for any more let go of every quarter of a minute (TrimThreeDTextures()).
 */
/** The finest level of a texture that has its pixels. */
static int FinestLevel(const std::vector<ThreeDObjectTexture> &lv)
{
	for (size_t i = 0; i < lv.size(); i++) {
		if (!lv[i].px.empty()) return static_cast<int>(i);
	}
	return static_cast<int>(lv.size()) - 1;
}

/** Whether a texture has a level with its pixels (or a coarser one, the coarsest there is). */
static bool HasLevel(const std::vector<ThreeDObjectTexture> &lv, int level)
{
	return FinestLevel(lv) <= std::min(level, static_cast<int>(lv.size()) - 1);
}

/** Let go of the pixels of the levels finer than a level; their sizes stay. */
static void DropFinerLevels(std::vector<ThreeDObjectTexture> &lv, int finest)
{
	for (int i = 0; i < finest && i + 1 < static_cast<int>(lv.size()); i++) {
		lv[i].px.clear();
		lv[i].px.shrink_to_fit();
	}
}

/** The pictures of the textures as one copy each (InternThreeDTextureBytes()), by a hash of their bytes; and those made. */
static std::unordered_multimap<size_t, std::weak_ptr<const std::vector<uint8_t>>> _texture_bytes;
static size_t _texture_bytes_made = 0;
/** The textures made of the pictures (ThreeDTextureLevelsOf()), by the picture and the colour of a material without one. */
struct TextureLevelsMade {
	std::weak_ptr<const std::vector<uint8_t>> bytes; ///< its picture, while anything has it (its place cannot be another's then)
	std::weak_ptr<const std::vector<ThreeDObjectTexture>> levels;
};
static std::map<std::pair<const std::vector<uint8_t> *, uint32_t>, TextureLevelsMade> _texture_levels;
/** The textures of files made (TextureLevelsOfFile()), by the file and the colour of what is not in the picture. */
static std::map<std::pair<std::string, uint32_t>, std::weak_ptr<const std::vector<ThreeDObjectTexture>>> _file_texture_levels;
/** The masks of details made (DetailMaskOfFile()), by their file. */
static std::map<std::string, std::weak_ptr<const ThreeDDetailMask>, std::less<>> _detail_masks_made;
/**
 * Where the triangles of the files draw from each picture (TextureCover()): u and v of the three corners of each
 * triangle of a material of the picture, all of the file's meshes, by the picture.
 */
using TextureCoverTriangles = std::shared_ptr<const std::vector<float>>;
struct TextureCoverMade {
	std::weak_ptr<const std::vector<uint8_t>> bytes; ///< its picture, while anything has it
	TextureCoverTriangles cover;
};
static std::map<const std::vector<uint8_t> *, TextureCoverMade> _texture_covers;
static std::mutex _texture_bytes_mutex;

/** Where the triangles draw from a picture (the cover of its bytes as the files using it were read), nullptr not known. */
static TextureCoverTriangles TextureCover(const ThreeDTextureBytes &png)
{
	if (png == nullptr) return nullptr;
	std::lock_guard lock(_texture_bytes_mutex);
	auto it = _texture_covers.find(png.get());
	if (it == _texture_covers.end() || it->second.bytes.lock() != png) return nullptr;
	return it->second.cover;
}

/**
 * A texture's picture as one copy for all that have the same: the materials of an add-on of the colleague's lorries are
 * tens, all of one picture of sixteen million pixels, and every record of the package has the add-on. Each copy had
 * its own bytes, and each was made into its own texture (the twenty four lorries took four seconds each, at the start).
 * @return nullptr for no bytes
 */
ThreeDTextureBytes InternThreeDTextureBytes(std::span<const uint8_t> bytes)
{
	if (bytes.empty()) return nullptr;
	const size_t hash = std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
	std::lock_guard lock(_texture_bytes_mutex);
	auto [first, last] = _texture_bytes.equal_range(hash);
	for (auto it = first; it != last; ++it) {
		ThreeDTextureBytes known = it->second.lock();
		if (known != nullptr && std::ranges::equal(*known, bytes)) return known;
	}
	/* Now and then those nothing has any more go. */
	if (++_texture_bytes_made % 256 == 0) std::erase_if(_texture_bytes, [](const auto &e) { return e.second.expired(); });
	auto made = std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end());
	_texture_bytes.emplace(hash, made);
	return made;
}

/**
 * A texture made of a picture (DecodeTextureLevels()), one for all the objects that have the same picture.
 * @param png the picture (InternThreeDTextureBytes()), nullptr none: the material's colour
 * @param flat the material's colour where it has no picture
 * @param finest the finest level wanted: those finer are made without their pixels
 */
static ThreeDTextureLevels ThreeDTextureLevelsOf(const ThreeDTextureBytes &png, uint32_t flat, int finest = 0)
{
	const TextureCoverTriangles cover = TextureCover(png);
	const auto key = std::make_pair(png.get(), flat);
	{
		std::lock_guard lock(_texture_bytes_mutex);
		auto it = _texture_levels.find(key);
		if (it != _texture_levels.end()) {
			ThreeDTextureLevels known = it->second.levels.lock();
			if (known != nullptr && it->second.bytes.lock() == png && HasLevel(*known, finest)) return known;
		}
	}
	static const std::vector<uint8_t> none;
	auto made = std::make_shared<std::vector<ThreeDObjectTexture>>();
	DecodeTextureLevels(png != nullptr ? *png : none, *made, flat, true, cover.get());
	DropFinerLevels(*made, finest);
	std::lock_guard lock(_texture_bytes_mutex);
	if (_texture_levels.size() > 64) std::erase_if(_texture_levels, [](const auto &e) { return e.second.levels.expired(); });
	_texture_levels[key] = TextureLevelsMade{png, made};
	return made;
}

/**
 * A texture of a file of its own (a paint, "textura"; a mask), one for all the objects that have the file.
 * @param flat the colour of the pixels the picture has not
 * @param of the picture of the model whose layout it has (the body's, an add-on's): where its triangles draw from (TextureCover())
 * @return nullptr when it cannot be read
 */
static ThreeDTextureLevels TextureLevelsOfFile(const std::string &file, uint32_t flat, const ThreeDTextureBytes &of = nullptr, int finest = 0)
{
	const auto key = std::make_pair(file, flat);
	{
		std::lock_guard lock(_texture_bytes_mutex);
		auto it = _file_texture_levels.find(key);
		if (it != _file_texture_levels.end()) {
			if (ThreeDTextureLevels known = it->second.lock(); known != nullptr && HasLevel(*known, finest)) return known;
		}
	}
	std::vector<uint8_t> png = ReadWholeFile(file);
	if (png.empty()) return nullptr;
	auto made = std::make_shared<std::vector<ThreeDObjectTexture>>();
	const TextureCoverTriangles cover = TextureCover(of);
	DecodeTextureLevels(png, *made, flat, true, cover.get());
	DropFinerLevels(*made, finest);
	std::lock_guard lock(_texture_bytes_mutex);
	_file_texture_levels[key] = made;
	return made;
}

/**
 * A mask of a file ("lak" "maska", "rez" "maska", an add-on's "barva" "maska"), its grey in the red; nullptr without it.
 * @param of the picture of the model it is the mask of (TextureLevelsOfFile())
 */
static ThreeDTextureLevels MaskLevelsOfFile(const std::string &file, const ThreeDTextureBytes &of = nullptr, int finest = 0)
{
	return TextureLevelsOfFile(file, Colour(0, 0, 0).data, of, finest);
}

/**
 * A mask of details of a file (ThreeDDetailMask), one for all the objects that have it; nullptr when it cannot be read.
 * @param of the picture of the model it is the mask of (TextureLevelsOfFile())
 */
static std::shared_ptr<const ThreeDDetailMask> DetailMaskOfFile(const std::string &file, const ThreeDTextureBytes &of = nullptr)
{
	{
		std::lock_guard lock(_texture_bytes_mutex);
		auto it = _detail_masks_made.find(file);
		if (it != _detail_masks_made.end()) {
			if (auto known = it->second.lock(); known != nullptr) return known;
		}
	}
	std::vector<ThreeDObjectTexture> levels;
	std::vector<uint8_t> png = ReadWholeFile(file);
	/* Only as it is: a mask index is not halved; where no triangle draws from it, the index nearest. */
	const TextureCoverTriangles cover = TextureCover(of);
	if (!png.empty()) DecodeTextureLevels(png, levels, Colour(0, 0, 0).data, false, cover.get());
	if (levels.empty() || levels[0].w <= 1) return nullptr;
	auto mask = std::make_shared<ThreeDDetailMask>();
	mask->w = levels[0].w;
	mask->h = levels[0].h;
	mask->index.resize(levels[0].px.size());
	for (size_t i = 0; i < mask->index.size(); i++) mask->index[i] = levels[0].px[i] == 0 ? 0 : Colour(levels[0].px[i]).r;
	std::lock_guard lock(_texture_bytes_mutex);
	_detail_masks_made[file] = mask;
	return mask;
}

/** A texture of a .glb: the PNG as it is in the file, or a plain colour where a material has none. */
struct GlbTexture {
	std::string name;          ///< the material's name
	ThreeDTextureBytes png;    ///< its picture, nullptr none
	uint32_t flat = 0; ///< the material's base colour factor (Colour::data) when it has no picture
	bool double_sided = false; ///< the material's "doubleSided": its triangles are seen from behind too
	uint32_t glow = 0;         ///< its own light (ThreeDVehicleShape::tex_glow), 0 none
	uint8_t alpha = 255;       ///< how much of what is behind it hides (ThreeDVehicleShape::tex_alpha)
};

/**
 * A material's own light and see-through (the colleague's hra/3d/PROSBA-NOC-ZBRANE-TRABANT-LAMPY.md: the fire Tatra's
 * beacon lit, the ghost "BLEND, 0.7, it shines by itself at night"): its "emissiveFactor" -- times the texel with an
 * "emissiveTexture" --, and with "alphaMode" "BLEND" the alpha of its base colour.
 */
static void ReadMaterialLight(const nlohmann::json &mat, GlbTexture &t)
{
	if (mat.contains("emissiveFactor") && mat.at("emissiveFactor").is_array() && mat.at("emissiveFactor").size() == 3) {
		const auto &f = mat.at("emissiveFactor");
		auto c = [&](int k) { return static_cast<uint8_t>(std::clamp(f.at(k).get<double>(), 0.0, 1.0) * 255.0 + 0.5); };
		const Colour e(c(0), c(1), c(2), mat.contains("emissiveTexture") ? ThreeDVehicleShape::GLOW_TEXTURED : ThreeDVehicleShape::GLOW_FLAT);
		if (e.r != 0 || e.g != 0 || e.b != 0) t.glow = e.data;
	}
	if (mat.value("alphaMode", std::string()) == "BLEND") {
		if (const nlohmann::json *f = GlbColourFactor(mat); f != nullptr) {
			t.alpha = static_cast<uint8_t>(std::clamp(f->at(3).get<double>(), 0.0, 1.0) * 255.0 + 0.5);
		}
	}
}

/** The meshes of a .glb as read: metres, in the game's axes (x, y, z up), normals when the file has them. */
struct GlbMesh {
	std::vector<float> pos;    ///< x, y, z of each corner
	std::vector<float> nrm;    ///< its normal, or empty
	std::vector<float> uv;     ///< u, v of each corner
	std::vector<uint32_t> tri; ///< three corners a triangle
	std::vector<uint8_t> tri_tex;      ///< the texture of each triangle, an index into textures
	std::vector<GlbTexture> textures;  ///< the textures of the materials
	std::vector<uint8_t> png;  ///< the first material's texture as it is in the file (a building's one texture)
	std::vector<uint16_t> tri_group;   ///< with groups, each triangle's: n the node groups[n - 1] it is under, 0 none
	std::vector<uint16_t> tri_mark;    ///< with marks, each triangle's: n the node marks[n - 1] it is under, 0 none
	std::vector<uint8_t> tri_glass;    ///< with a glass suffix, each triangle: 1 under a node whose name ends so
};

/**
 * What of a .glb ReadGlbMesh() reads, beyond its meshes: see there. The parts of a vehicle's add-ons (the colleague's
 * hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md) need more of it than a building does -- only the nodes the states show, each
 * part's windows, lamps and wheels told apart within it.
 */
struct GlbReadOptions {
	bool all_nodes = false;
	std::string_view frame_prefix;
	int frame = 0;
	std::string_view only_node;
	std::string_view point_prefix;
	std::vector<float> *points = nullptr;
	std::vector<uint16_t> *point_groups = nullptr; ///< with points, each place's group (as tri_group), 0 none
	std::string_view skip_node;
	const std::vector<std::string> *groups = nullptr;
	bool only_groups = false;     ///< with groups, only the meshes under the groups' nodes (an add-on: the nodes no state shows not read at all)
	bool skip_night = false;      ///< the nodes "noc" and "noc_<...>" and their children left out (lit only at night)
	bool only_night = false;      ///< only the meshes under the nodes "noc" and "noc_<...>"
	const std::vector<std::string> *marks = nullptr; ///< a second marking of nodes by name, as groups (GlbMesh::tri_mark): a part's wheels
	std::string_view glass_suffix; ///< the nodes whose names end so (and their children) are glass (GlbMesh::tri_glass)
	bool points_only = false;     ///< only the places (points), no mesh read: the lights at the back of an add-on's parts
	/** With groups and only_groups, the nodes whose materials are taken as textures first, in the order they are met, before
	 * only those of groups are read: the textures of an add-on the same whatever of its nodes are read. */
	const std::vector<std::string> *material_groups = nullptr;
};

/** Whether a node is of the lights at night: "noc", or "noc_<part>" of an add-on's part. */
static inline bool IsNightNode(std::string_view name)
{
	return name == "noc" || name.starts_with("noc_");
}

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
 * @param only_node with all_nodes, only the meshes of the node so named and its children (a building's "noc")
 * @param point_prefix with all_nodes and points, the nodes whose names start so are places: their origins go to points
 * @param points where the places go, metres in the game's axes as the corners
 * @param skip_node with all_nodes, the node so named and its children are left out (a vehicle's "noc", drawn only at night)
 * @return whether there is something to draw
 * @param groups with all_nodes, the nodes whose triangles (and their children's) are marked (GlbMesh::tri_group)
 */
static bool ReadGlbMesh(const std::string &path, GlbMesh &mesh, const GlbReadOptions &o);
static bool ReadGlbMesh(const std::string &path, GlbMesh &mesh, bool all_nodes = false, std::string_view frame_prefix = {}, int frame = 0, std::string_view only_node = {}, std::string_view point_prefix = {}, std::vector<float> *points = nullptr, std::string_view skip_node = {}, const std::vector<std::string> *groups = nullptr)
{
	GlbReadOptions o;
	o.all_nodes = all_nodes;
	o.frame_prefix = frame_prefix;
	o.frame = frame;
	o.only_node = only_node;
	o.point_prefix = point_prefix;
	o.points = points;
	o.skip_node = skip_node;
	o.groups = groups;
	return ReadGlbMesh(path, mesh, o);
}

/** @copydoc ReadGlbMesh() */
static bool ReadGlbMesh(const std::string &path, GlbMesh &mesh, const GlbReadOptions &o)
{
	const bool all_nodes = o.all_nodes;
	const std::string_view frame_prefix = o.frame_prefix;
	const int frame = o.frame;
	const std::string_view only_node = o.only_node;
	const std::string_view point_prefix = o.point_prefix;
	std::vector<float> *points = o.points;
	const std::string_view skip_node = o.skip_node;
	const std::vector<std::string> *groups = o.groups;
	const std::vector<std::string> *marks = o.marks;
	const std::shared_ptr<GlbFile> glb = GlbFileOf(path);
	/* Its file let go of when this reading is done; what was read of it stays while the reading goes on. */
	struct CloseAfter {
		GlbFile &glb;
		~CloseAfter() { this->glb.Close(); }
	} close_after{*glb};
	if (!glb->error.empty()) {
		Debug(misc, 0, "3D object {}: {}", path, glb->error);
		ReadDiary(fmt::format("soubor {}: {}", path, glb->error), true);
		return false;
	}
	using Matrix = std::array<double, 16>; // column-major
	try {
		const nlohmann::json &j = glb->json;
		const auto &accessors = j.at("accessors");
		const auto &views = j.at("bufferViews");
		/* An accessor's bytes, read from the file when first wanted (GlbFile::Piece()), and how they lie. */
		struct AccessorBytes {
			std::span<const uint8_t> bytes;
			int type = 0;
			bool normalized = false;
			size_t count = 0, stride = 0, size = 0;
		};
		auto accessor_bytes = [&](int index, int comps) {
			const auto &a = accessors.at(index);
			const auto &v = views.at(a.at("bufferView").get<int>());
			AccessorBytes r;
			r.type = a.at("componentType").get<int>();
			r.normalized = a.value("normalized", false);
			r.count = a.at("count").get<size_t>();
			r.size = GlbComponentSize(r.type);
			r.stride = v.value("byteStride", 0);
			if (r.stride == 0) r.stride = r.size * comps;
			const size_t offset = v.value("byteOffset", 0) + a.value("byteOffset", 0);
			r.bytes = glb->Piece(offset, r.count == 0 ? 0 : (r.count - 1) * r.stride + r.size * comps);
			return r;
		};
		/* An accessor's elements as floats: copied as they are when they are floats one after the other; whole
		 * numbers (a quantised mesh) as their values, normalised ones as fractions (ReadGlbNumber()). */
		auto read = [&](int index, int comps, std::vector<float> &out) {
			const AccessorBytes a = accessor_bytes(index, comps);
			out.resize(a.count * comps);
			if constexpr (std::endian::native == std::endian::little) {
				if (a.type == 5126 && a.stride == 4 * static_cast<size_t>(comps)) {
					std::copy_n(a.bytes.data(), out.size() * sizeof(float), reinterpret_cast<uint8_t *>(out.data()));
					return;
				}
			}
			for (size_t i = 0; i < a.count; i++) {
				for (int c = 0; c < comps; c++) {
					out[i * comps + c] = static_cast<float>(ReadGlbNumber(a.bytes, i * a.stride + c * a.size, a.type, a.normalized));
				}
			}
		};
		/* The corners of a primitive's triangles: its indices, or each corner once without them. */
		auto read_indices = [&](const nlohmann::json &p, size_t corners, std::vector<uint32_t> &out) {
			if (!p.contains("indices")) {
				out.resize(corners);
				std::iota(out.begin(), out.end(), 0u);
				return;
			}
			const AccessorBytes a = accessor_bytes(p.at("indices").get<int>(), 1);
			out.resize(a.count);
			for (size_t i = 0; i < a.count; i++) {
				const size_t at = i * a.stride;
				switch (a.type) {
					case 5125: out[i] = ReadU32(a.bytes, at); break;
					case 5123: out[i] = a.bytes[at] | (a.bytes[at + 1] << 8); break;
					case 5121: out[i] = a.bytes[at]; break;
					default: throw std::invalid_argument("component type of the indices");
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

		/* The textures, by the material: a picture in the buffer, or the material's colour. The materials of a picture
		 * share its bytes, and so do the files and shapes with the same picture (InternThreeDTextureBytes()). */
		std::map<int, uint8_t> material_texture;
		std::map<int, ThreeDTextureBytes> image_bytes;
		auto texture_of = [&](int material) -> uint8_t {
			auto it = material_texture.find(material);
			if (it != material_texture.end()) return it->second;
			GlbTexture t;
			t.flat = Colour(150, 150, 150).data;
			if (material >= 0 && j.contains("materials")) {
				const auto &mat = j.at("materials").at(material);
				t.name = mat.value("name", std::string());
				t.double_sided = mat.value("doubleSided", false);
				ReadMaterialLight(mat, t);
				{
					if (const nlohmann::json *f = GlbColourFactor(mat); f != nullptr) {
						auto c = [&](int k) { return static_cast<uint8_t>(std::clamp(f->at(k).get<double>(), 0.0, 1.0) * 255.0 + 0.5); };
						t.flat = Colour(c(0), c(1), c(2)).data;
					}
					if (const nlohmann::json *colour = GlbColourTexture(mat); colour != nullptr && j.contains("textures") && j.contains("images")) {
						int tex = colour->at("index").get<int>();
						int image = j.at("textures").at(tex).at("source").get<int>();
						const auto &img = j.at("images").at(image);
						auto known = image_bytes.find(image);
						if (known != image_bytes.end()) {
							t.png = known->second;
						} else if (img.contains("bufferView")) {
							const auto &v = views.at(img.at("bufferView").get<int>());
							size_t offset = v.value("byteOffset", 0), len = v.at("byteLength").get<size_t>();
							if (offset <= glb->bin_len && len <= glb->bin_len - offset) t.png = InternThreeDTextureBytes(glb->Piece(offset, len));
							image_bytes.emplace(image, t.png);
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
		auto add_mesh = [&](int mesh_index, const Matrix &m, uint16_t group, uint16_t mark = 0, bool glass = false) {
			const auto &prims = j.at("meshes").at(mesh_index).at("primitives");
			/* The normals turned by the inverse transpose of the transform's turning -- the cross products of its columns,
			 * mirrored the other way round -- so a node stretched more one way than another (a quantised mesh's node,
			 * Blender's unapplied scale) keeps them square to its faces. */
			const double ca[3] = {m[0], m[1], m[2]}, cb[3] = {m[4], m[5], m[6]}, cc[3] = {m[8], m[9], m[10]};
			double nm[9] = {
				cb[1] * cc[2] - cb[2] * cc[1], cb[2] * cc[0] - cb[0] * cc[2], cb[0] * cc[1] - cb[1] * cc[0],
				cc[1] * ca[2] - cc[2] * ca[1], cc[2] * ca[0] - cc[0] * ca[2], cc[0] * ca[1] - cc[1] * ca[0],
				ca[1] * cb[2] - ca[2] * cb[1], ca[2] * cb[0] - ca[0] * cb[2], ca[0] * cb[1] - ca[1] * cb[0],
			};
			if (ca[0] * nm[0] + ca[1] * nm[1] + ca[2] * nm[2] < 0) for (double &e : nm) e = -e;
			for (const auto &p : prims) {
				if (p.value("mode", 4) != 4) continue; // triangles only
				std::vector<float> pos, nrm, uv;
				std::vector<uint32_t> idx;
				read(p.at("attributes").at("POSITION").get<int>(), 3, pos);
				if (p.at("attributes").contains("TEXCOORD_0")) read(p.at("attributes").at("TEXCOORD_0").get<int>(), 2, uv);
				if (p.at("attributes").contains("NORMAL")) read(p.at("attributes").at("NORMAL").get<int>(), 3, nrm);
				size_t corners = pos.size() / 3;
				if (uv.size() != corners * 2) uv.assign(corners * 2, 0.0f);
				if (nrm.size() != corners * 3) normals = false;
				read_indices(p, corners, idx);
				int material = p.value("material", -1);
				if (first_material < 0 && material >= 0) first_material = material;
				uint8_t tex = texture_of(material);
				const std::array<double, 6> ut = GlbUvTransform(j, material);
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
					const double u = uv[i * 2], v = uv[i * 2 + 1];
					mesh.uv.push_back(static_cast<float>(ut[0] + ut[2] * u + ut[3] * v));
					mesh.uv.push_back(static_cast<float>(ut[1] + ut[4] * u + ut[5] * v));
					if (normals) {
						double nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
						double hx = nm[0] * nx + nm[3] * ny + nm[6] * nz, hy = nm[1] * nx + nm[4] * ny + nm[7] * nz, hz = nm[2] * nx + nm[5] * ny + nm[8] * nz;
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
					if (groups != nullptr) mesh.tri_group.push_back(group);
					if (marks != nullptr) mesh.tri_mark.push_back(mark);
					if (!o.glass_suffix.empty()) mesh.tri_glass.push_back(glass ? 1 : 0);
				}
			}
		};

		const Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		/* The file's first material is the first texture -- a vehicle's body, the one its paint is for -- whatever
		 * node is read first (the V3S's glass was, and its paint went on the glass). */
		if (all_nodes && !o.points_only && j.contains("materials") && !j.at("materials").empty()) texture_of(0);
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
			/* A node to visit: its parent's transform, whether its meshes are read, its group, its mark, its glass. */
			struct NodeToVisit {
				int n;
				Matrix parent;
				bool inside;
				uint16_t group;
				uint16_t mark;
				bool glass;
			};
			std::vector<NodeToVisit> todo;
			if (o.material_groups != nullptr && o.only_groups && !o.points_only) {
				/* The textures of all the nodes of material_groups first, in the order the meshes are met. */
				std::vector<std::pair<int, bool>> seen;
				for (int r : roots) seen.emplace_back(r, false);
				size_t walked = 0;
				while (!seen.empty() && walked++ < 100000) {
					auto [n, in] = seen.back();
					seen.pop_back();
					const auto &node = nodes.at(n);
					const std::string node_name = node.contains("name") && node.at("name").is_string() ? node.at("name").get<std::string>() : std::string();
					if (o.skip_night && IsNightNode(node_name)) continue;
					if (std::ranges::find(*o.material_groups, node_name) != o.material_groups->end()) in = true;
					if (in && node.contains("mesh")) {
						for (const auto &p : j.at("meshes").at(node.at("mesh").get<int>()).at("primitives")) {
							if (p.value("mode", 4) == 4) texture_of(p.value("material", -1));
						}
					}
					if (node.contains("children")) for (const auto &c : node.at("children")) seen.emplace_back(c.get<int>(), in);
				}
			}
			const bool inside_at_first = only_node.empty() && !o.only_night && !(o.only_groups && groups != nullptr);
			for (int r : roots) todo.push_back({r, identity, inside_at_first, 0, 0, false});
			size_t visited = 0;
			while (!todo.empty() && visited++ < 100000) {
				auto [n, parent, inside, group, mark, glass] = todo.back();
				todo.pop_back();
				const auto &node = nodes.at(n);
				const std::string node_name = node.contains("name") && node.at("name").is_string() ? node.at("name").get<std::string>() : std::string();
				if (!skip_node.empty() && node_name == skip_node) continue;
				if (o.skip_night && IsNightNode(node_name)) continue;
				if (!only_node.empty() && node_name == only_node) inside = true;
				if (groups != nullptr) {
					auto g = std::ranges::find(*groups, node_name);
					if (g != groups->end()) {
						group = static_cast<uint16_t>(g - groups->begin() + 1);
						if (o.only_groups && !o.only_night) inside = true;
					}
				}
				/* Only the lights at night: inside the node "noc" of the part's node (with groups, a group's only). */
				if (o.only_night && IsNightNode(node_name) && (!o.only_groups || group != 0)) inside = true;
				if (marks != nullptr) {
					auto g = std::ranges::find(*marks, node_name);
					if (g != marks->end()) mark = static_cast<uint16_t>(g - marks->begin() + 1);
				}
				if (!o.glass_suffix.empty() && node_name.ends_with(o.glass_suffix)) glass = true;
				if (!frame_prefix.empty() && node.contains("name") && node.at("name").is_string()) {
					std::string name = node.at("name").get<std::string>();
					if (name.starts_with(frame_prefix)) {
						auto number = ParseInteger<int>(std::string_view(name).substr(frame_prefix.size()), 10);
						if (!number.has_value() || *number != frame) continue;
					}
				}
				Matrix m = multiply(parent, local(node));
				if (points != nullptr && !point_prefix.empty() && node_name.starts_with(point_prefix)) {
					/* A place: its origin, turned from glTF's axes as the corners are. */
					points->push_back(static_cast<float>(m[12]));
					points->push_back(static_cast<float>(-m[14]));
					points->push_back(static_cast<float>(m[13]));
					if (o.point_groups != nullptr) o.point_groups->push_back(group);
				}
				if (inside && !o.points_only && node.contains("mesh")) add_mesh(node.at("mesh").get<int>(), m, group, mark, glass);
				if (node.contains("children")) for (const auto &c : node.at("children")) todo.push_back({c.get<int>(), m, inside, group, mark, glass});
			}
		} else {
			/* The first node with a mesh not of the night (the lit windows, "noc", are read on their own), with its
			 * parents' transforms: gltfpack puts each mesh in a node of its own under the named one, its transform
			 * making the quantised positions metres again, the named node's transform above it. */
			Matrix m = identity;
			int mesh_index = 0;
			if (j.contains("nodes")) {
				const auto &nodes = j.at("nodes");
				std::vector<int> parent(nodes.size(), -1);
				for (size_t i = 0; i < nodes.size(); i++) {
					if (!nodes.at(i).contains("children")) continue;
					for (const auto &c : nodes.at(i).at("children")) {
						const size_t child = c.get<size_t>();
						if (child < parent.size() && parent[child] < 0 && child != i) parent[child] = static_cast<int>(i);
					}
				}
				auto name_of = [&](int n) {
					const auto &node = nodes.at(n);
					return node.contains("name") && node.at("name").is_string() ? node.at("name").get<std::string>() : std::string();
				};
				for (size_t i = 0; i < nodes.size(); i++) {
					if (!nodes.at(i).contains("mesh")) continue;
					bool night = false;
					Matrix w = local(nodes.at(i));
					size_t up = 0;
					for (int n = parent[i]; n >= 0 && up++ < nodes.size(); n = parent[n]) {
						if (IsNightNode(name_of(n))) night = true;
						w = multiply(local(nodes.at(n)), w);
					}
					if (night || IsNightNode(name_of(static_cast<int>(i)))) continue;
					mesh_index = nodes.at(i).at("mesh").get<int>();
					m = w;
					break;
				}
			}
			add_mesh(mesh_index, m, 0);
		}
		/* Where in each of its pictures read now all the file's triangles draw from, whatever nodes were read: an
		 * atlas is shared by the vehicles of a package, each showing other nodes of it (TextureCover()). */
		auto image_of = [&](int material) {
			if (material < 0 || !j.contains("materials") || !j.contains("textures")) return -1;
			const nlohmann::json *colour = GlbColourTexture(j.at("materials").at(material));
			if (colour == nullptr) return -1;
			const int tex = colour->at("index").get<int>();
			return j.at("textures").at(tex).value("source", -1);
		};
		const bool margins_filled = MarginsFilled(path);
		for (const auto &[image, bytes] : image_bytes) {
			if (margins_filled || bytes == nullptr || TextureCover(bytes) != nullptr || !j.contains("meshes")) continue;
			auto cover = std::make_shared<std::vector<float>>();
			for (const auto &me : j.at("meshes")) {
				for (const auto &p : me.at("primitives")) {
					if (p.value("mode", 4) != 4 || !p.at("attributes").contains("TEXCOORD_0") || image_of(p.value("material", -1)) != image) continue;
					std::vector<float> uv;
					std::vector<uint32_t> idx;
					read(p.at("attributes").at("TEXCOORD_0").get<int>(), 2, uv);
					const size_t corners = uv.size() / 2;
					read_indices(p, corners, idx);
					const std::array<double, 6> ut = GlbUvTransform(j, p.value("material", -1));
					for (size_t i = 0; i + 2 < idx.size(); i += 3) {
						for (int k = 0; k < 3; k++) {
							const size_t c = static_cast<size_t>(idx[i + k]);
							if (c >= corners) break;
							const double u = uv[c * 2], v = uv[c * 2 + 1];
							cover->push_back(static_cast<float>(ut[0] + ut[2] * u + ut[3] * v));
							cover->push_back(static_cast<float>(ut[1] + ut[4] * u + ut[5] * v));
						}
						cover->resize(cover->size() / 6 * 6);
					}
				}
			}
			std::lock_guard lock(_texture_bytes_mutex);
			if (_texture_covers.size() > 256) std::erase_if(_texture_covers, [](const auto &e) { return e.second.bytes.expired(); });
			_texture_covers[bytes.get()] = TextureCoverMade{bytes, std::move(cover)};
		}
		if (!normals || mesh.nrm.size() != mesh.pos.size()) mesh.nrm.clear();
		/* A building's one texture: the first material's. */
		if (first_material >= 0) {
			auto it = material_texture.find(first_material);
			if (it != material_texture.end() && it->second < mesh.textures.size() && mesh.textures[it->second].png != nullptr) mesh.png = *mesh.textures[it->second].png;
		}
	} catch (const std::exception &e) {
		Debug(misc, 0, "3D object {}: {}", path, e.what());
		ReadDiary(fmt::format("soubor {}: {}", path, e.what()), true);
		return false;
	}
	return !mesh.tri.empty();
}

/*
 * The bones of a creature (the technologies of OpenMW, the player, 16. 10.: skinning and blending): the colleague's
 * "kostra" of a creature (hra/3d/objekty/tvorove, drak_*_kostra.glb) is its mesh with a skeleton and its animations
 * by the same names as its frames. Its frames are posed from them a tick each, not a frame every eight ticks, and from
 * one animation to the next the poses pass over (GetThreeDFigure("<jmeno>/<a>@<snimek>><b>")).
 */

/** A matrix of the bones, column-major as glTF writes it. */
using BoneMatrix = std::array<double, 16>;

static BoneMatrix BoneMultiply(const BoneMatrix &a, const BoneMatrix &b)
{
	BoneMatrix c{};
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			double sum = 0;
			for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
			c[col * 4 + row] = sum;
		}
	}
	return c;
}

/** A bone's place against its parent: moved, turned (a quaternion x, y, z, w) and scaled. */
struct BonePose {
	double t[3] = {0, 0, 0};
	double r[4] = {0, 0, 0, 1};
	double s[3] = {1, 1, 1};

	BoneMatrix Matrix() const
	{
		const double x = this->r[0], y = this->r[1], z = this->r[2], w = this->r[3];
		const double m[9] = {
			1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
			2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
			2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y),
		};
		BoneMatrix out{};
		for (int col = 0; col < 3; col++) {
			for (int row = 0; row < 3; row++) out[col * 4 + row] = m[col * 3 + row] * this->s[col];
		}
		out[12] = this->t[0];
		out[13] = this->t[1];
		out[14] = this->t[2];
		out[15] = 1;
		return out;
	}

	/** Between this one (0) and another (1): moved and scaled in a line, turned the short way round. */
	BonePose Mix(const BonePose &o, double k) const
	{
		BonePose p;
		for (int i = 0; i < 3; i++) {
			p.t[i] = this->t[i] + (o.t[i] - this->t[i]) * k;
			p.s[i] = this->s[i] + (o.s[i] - this->s[i]) * k;
		}
		double dot = 0;
		for (int i = 0; i < 4; i++) dot += this->r[i] * o.r[i];
		const double sign = dot < 0 ? -1 : 1;
		double len = 0;
		for (int i = 0; i < 4; i++) {
			p.r[i] = this->r[i] + (sign * o.r[i] - this->r[i]) * k;
			len += p.r[i] * p.r[i];
		}
		len = std::sqrt(len);
		if (len > 0) for (int i = 0; i < 4; i++) p.r[i] /= len;
		return p;
	}
};

/** A creature's mesh with its bones and its animations (ReadGlbSkeleton()). */
struct GlbSkeleton {
	struct Node {
		int parent = -1;
		bool has_matrix = false; ///< its place a matrix (not animated)
		BoneMatrix matrix{};
		BonePose rest;
	};
	struct Channel {
		int node = 0;
		int path = 0;            ///< 0 moved, 1 turned, 2 scaled
		bool step = false;       ///< "STEP": held until the next key, else in a line between them
		bool cubic = false;      ///< "CUBICSPLINE": its keys with their tangents (read as in a line)
		std::vector<float> times;
		std::vector<float> values;
	};
	struct Animation {
		std::string name;
		double start = 0, end = 0; ///< seconds
		std::vector<Channel> channels;
	};
	std::vector<Node> nodes;
	std::vector<int> joints;               ///< the nodes of the joints
	std::vector<BoneMatrix> inverse_bind;  ///< and their matrices from the mesh to them
	std::vector<double> pos, nrm, uv;      ///< the mesh, glTF's axes
	std::vector<uint32_t> tri;
	std::vector<uint8_t> tri_tex;
	std::vector<std::array<uint16_t, 4>> joint_of; ///< each corner's joints (indices into joints)
	std::vector<std::array<float, 4>> weight_of;   ///< and how much each moves it
	std::vector<GlbTexture> textures;
	std::vector<Animation> animations;

	const Animation *Find(std::string_view name) const
	{
		for (const Animation &a : this->animations) {
			if (a.name == name) return &a;
		}
		return nullptr;
	}

	/** Every bone's place against its parent in an animation at a time, seconds (none: at rest). */
	std::vector<BonePose> Pose(const Animation *anim, double time) const
	{
		std::vector<BonePose> pose;
		pose.reserve(this->nodes.size());
		for (const Node &n : this->nodes) pose.push_back(n.rest);
		if (anim == nullptr) return pose;
		for (const Channel &c : anim->channels) {
			if (c.node < 0 || static_cast<size_t>(c.node) >= pose.size() || c.times.empty()) continue;
			const int comps = c.path == 1 ? 4 : 3;
			const size_t keys = c.times.size();
			auto value = [&](size_t key, int comp) {
				return static_cast<double>(c.values[(c.cubic ? key * 3 + 1 : key) * comps + comp]);
			};
			size_t hi = std::upper_bound(c.times.begin(), c.times.end(), static_cast<float>(time)) - c.times.begin();
			size_t lo = hi == 0 ? 0 : hi - 1;
			if (hi >= keys) hi = keys - 1;
			double k = 0;
			if (!c.step && hi != lo && c.times[hi] > c.times[lo]) k = std::clamp((time - c.times[lo]) / (c.times[hi] - c.times[lo]), 0.0, 1.0);
			BonePose a = pose[c.node], b = pose[c.node];
			double *pa = c.path == 0 ? a.t : c.path == 1 ? a.r : a.s;
			double *pb = c.path == 0 ? b.t : c.path == 1 ? b.r : b.s;
			for (int i = 0; i < comps; i++) {
				pa[i] = value(lo, i);
				pb[i] = value(hi, i);
			}
			pose[c.node] = a.Mix(b, k);
		}
		return pose;
	}

	/**
	 * The mesh in a pose: each corner moved by its joints, as much as each weighs; turned from glTF's axes into the
	 * game's (x, -z, y), metres.
	 */
	void Skin(const std::vector<BonePose> &pose, std::vector<float> &out_pos, std::vector<float> &out_nrm) const
	{
		std::vector<BoneMatrix> global(this->nodes.size());
		std::vector<bool> done(this->nodes.size(), false);
		std::function<const BoneMatrix &(int)> place = [&](int i) -> const BoneMatrix & {
			if (!done[i]) {
				const Node &n = this->nodes[i];
				const BoneMatrix own = n.has_matrix ? n.matrix : pose[i].Matrix();
				global[i] = n.parent >= 0 ? BoneMultiply(place(n.parent), own) : own;
				done[i] = true;
			}
			return global[i];
		};
		std::vector<BoneMatrix> joint(this->joints.size());
		for (size_t k = 0; k < this->joints.size(); k++) joint[k] = BoneMultiply(place(this->joints[k]), this->inverse_bind[k]);
		const size_t corners = this->pos.size() / 3;
		out_pos.resize(corners * 3);
		out_nrm.resize(this->nrm.size() == corners * 3 ? corners * 3 : 0);
		for (size_t i = 0; i < corners; i++) {
			BoneMatrix m{};
			for (int k = 0; k < 4; k++) {
				const float w = this->weight_of[i][k];
				if (w == 0 || this->joint_of[i][k] >= joint.size()) continue;
				const BoneMatrix &j = joint[this->joint_of[i][k]];
				for (int e = 0; e < 16; e++) m[e] += j[e] * w;
			}
			const double X = this->pos[i * 3], Y = this->pos[i * 3 + 1], Z = this->pos[i * 3 + 2];
			const double gx = m[0] * X + m[4] * Y + m[8] * Z + m[12];
			const double gy = m[1] * X + m[5] * Y + m[9] * Z + m[13];
			const double gz = m[2] * X + m[6] * Y + m[10] * Z + m[14];
			out_pos[i * 3] = static_cast<float>(gx);
			out_pos[i * 3 + 1] = static_cast<float>(-gz);
			out_pos[i * 3 + 2] = static_cast<float>(gy);
			if (out_nrm.empty()) continue;
			const double nx = this->nrm[i * 3], ny = this->nrm[i * 3 + 1], nz = this->nrm[i * 3 + 2];
			double hx = m[0] * nx + m[4] * ny + m[8] * nz, hy = m[1] * nx + m[5] * ny + m[9] * nz, hz = m[2] * nx + m[6] * ny + m[10] * nz;
			const double l = std::sqrt(hx * hx + hy * hy + hz * hz);
			if (l > 0) {
				hx /= l;
				hy /= l;
				hz /= l;
			}
			out_nrm[i * 3] = static_cast<float>(hx);
			out_nrm[i * 3 + 1] = static_cast<float>(-hz);
			out_nrm[i * 3 + 2] = static_cast<float>(hy);
		}
	}
};

/**
 * An accessor's elements of a glTF binary as numbers: the floats, or the whole numbers, normalised ones as fractions
 * (ReadGlbNumber(): gltfpack's turnings of the bones as normalised shorts, the weights as normalised bytes).
 */
static void ReadGlbAccessor(const nlohmann::json &j, std::span<const uint8_t> bin, int index, int comps, std::vector<double> &out)
{
	const auto &a = j.at("accessors").at(index);
	const auto &v = j.at("bufferViews").at(a.at("bufferView").get<int>());
	const size_t offset = v.value("byteOffset", 0) + a.value("byteOffset", 0);
	const int type = a.at("componentType").get<int>();
	const bool normalized = a.value("normalized", false);
	const size_t count = a.at("count").get<size_t>();
	const size_t size = GlbComponentSize(type);
	size_t stride = v.value("byteStride", 0);
	if (stride == 0) stride = size * comps;
	out.resize(count * comps);
	for (size_t i = 0; i < count; i++) {
		for (int c = 0; c < comps; c++) {
			const size_t at = offset + i * stride + c * size;
			if (at + size > bin.size()) throw std::out_of_range("accessor past the buffer");
			out[i * comps + c] = ReadGlbNumber(bin, at, type, normalized);
		}
	}
}

/**
 * Read a creature's mesh with its bones and animations (its "kostra"), kept for the next asking.
 * @return nullptr when the file has no mesh with a skin
 */
static const GlbSkeleton *ReadGlbSkeleton(const std::string &path)
{
	/* One at a time: a shape may be made on the thread making the pictures of the vehicles too (MakeOnce()). */
	static std::mutex kept_mutex;
	std::lock_guard lock(kept_mutex);
	static std::map<std::string, std::unique_ptr<GlbSkeleton>> kept;
	auto found = kept.find(path);
	if (found != kept.end()) return found->second.get();
	std::unique_ptr<GlbSkeleton> &slot = kept[path];

	const std::shared_ptr<const std::vector<uint8_t>> file = GlbFileBytes(path);
	const std::vector<uint8_t> &d = *file;
	if (d.size() < 20 || ReadU32(d, 0) != 0x46546C67 || ReadU32(d, 4) != 2) {
		Debug(misc, 0, "3D skeleton {}: not a glTF binary", path);
		return nullptr;
	}
	std::string json_text;
	/* (Where it is in the file: a file of the colleague's add-ons is tens of megabytes.) */
	std::span<const uint8_t> bin;
	for (size_t at = 12; at + 8 <= d.size();) {
		uint32_t len = ReadU32(d, at), type = ReadU32(d, at + 4);
		if (at + 8 + len > d.size()) break;
		if (type == 0x4E4F534A) json_text.assign(reinterpret_cast<const char *>(&d[at + 8]), len);
		if (type == 0x004E4942) bin = std::span<const uint8_t>(d).subspan(at + 8, len);
		at += 8 + ((len + 3) & ~3u);
	}
	auto sk = std::make_unique<GlbSkeleton>();
	try {
		nlohmann::json j = nlohmann::json::parse(json_text);
		if (const std::string why = GlbExtensionNotRead(j); !why.empty()) throw std::invalid_argument(why);
		const auto &nodes = j.at("nodes");
		sk->nodes.resize(nodes.size());
		for (size_t i = 0; i < nodes.size(); i++) {
			const auto &n = nodes.at(i);
			GlbSkeleton::Node &node = sk->nodes[i];
			if (n.contains("matrix")) {
				node.has_matrix = true;
				for (int k = 0; k < 16; k++) node.matrix[k] = n.at("matrix").at(k).get<double>();
			}
			if (n.contains("translation")) for (int k = 0; k < 3; k++) node.rest.t[k] = n.at("translation").at(k).get<double>();
			if (n.contains("rotation")) for (int k = 0; k < 4; k++) node.rest.r[k] = n.at("rotation").at(k).get<double>();
			if (n.contains("scale")) for (int k = 0; k < 3; k++) node.rest.s[k] = n.at("scale").at(k).get<double>();
		}
		for (size_t i = 0; i < nodes.size(); i++) {
			if (!nodes.at(i).contains("children")) continue;
			for (const auto &c : nodes.at(i).at("children")) {
				const size_t child = c.get<size_t>();
				if (child < sk->nodes.size()) sk->nodes[child].parent = static_cast<int>(i);
			}
		}
		/* The joints of every skin, one after the other; a skin's joints from where its first is. */
		std::vector<size_t> skin_first;
		if (j.contains("skins")) {
			for (const auto &skin : j.at("skins")) {
				skin_first.push_back(sk->joints.size());
				const size_t first = sk->joints.size();
				for (const auto &jn : skin.at("joints")) sk->joints.push_back(jn.get<int>());
				sk->inverse_bind.resize(sk->joints.size(), BoneMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
				if (skin.contains("inverseBindMatrices")) {
					std::vector<double> m;
					ReadGlbAccessor(j, bin, skin.at("inverseBindMatrices").get<int>(), 16, m);
					for (size_t k = 0; first + k < sk->joints.size() && (k + 1) * 16 <= m.size(); k++) std::copy_n(&m[k * 16], 16, sk->inverse_bind[first + k].begin());
				}
			}
		}
		/* The textures by the material: the picture in the buffer, else its colour (shared, InternThreeDTextureBytes()). */
		std::map<int, uint8_t> material_texture;
		std::map<int, ThreeDTextureBytes> image_bytes;
		auto texture_of = [&](int material) -> uint8_t {
			auto it = material_texture.find(material);
			if (it != material_texture.end()) return it->second;
			GlbTexture t;
			t.flat = Colour(150, 150, 150).data;
			if (material >= 0 && j.contains("materials")) {
				const auto &mat = j.at("materials").at(material);
				t.name = mat.value("name", std::string());
				ReadMaterialLight(mat, t);
				{
					if (const nlohmann::json *f = GlbColourFactor(mat); f != nullptr) {
						auto c = [&](int k) { return static_cast<uint8_t>(std::clamp(f->at(k).get<double>(), 0.0, 1.0) * 255.0 + 0.5); };
						t.flat = Colour(c(0), c(1), c(2)).data;
					}
					if (const nlohmann::json *colour = GlbColourTexture(mat); colour != nullptr && j.contains("textures") && j.contains("images")) {
						const int image = j.at("textures").at(colour->at("index").get<int>()).at("source").get<int>();
						const auto &img = j.at("images").at(image);
						auto known = image_bytes.find(image);
						if (known != image_bytes.end()) {
							t.png = known->second;
						} else if (img.contains("bufferView")) {
							const auto &v = j.at("bufferViews").at(img.at("bufferView").get<int>());
							const size_t offset = v.value("byteOffset", 0), len = v.at("byteLength").get<size_t>();
							if (offset + len <= bin.size()) t.png = InternThreeDTextureBytes(bin.subspan(offset, len));
							image_bytes.emplace(image, t.png);
						}
					}
				}
			}
			const uint8_t index = static_cast<uint8_t>(std::min<size_t>(sk->textures.size(), 255));
			if (sk->textures.size() < 255) sk->textures.push_back(std::move(t));
			material_texture.emplace(material, index);
			return index;
		};
		/* Every node with a mesh and a skin (the body, its eyes). */
		for (const auto &node : nodes) {
			if (!node.contains("mesh") || !node.contains("skin")) continue;
			const size_t skin_index = node.at("skin").get<size_t>();
			if (skin_index >= skin_first.size()) continue;
			for (const auto &p : j.at("meshes").at(node.at("mesh").get<int>()).at("primitives")) {
				if (p.value("mode", 4) != 4) continue;
				const auto &attr = p.at("attributes");
				if (!attr.contains("JOINTS_0") || !attr.contains("WEIGHTS_0")) continue;
				std::vector<double> pos, nrm, uv, idx, jn, wt;
				ReadGlbAccessor(j, bin, attr.at("POSITION").get<int>(), 3, pos);
				if (attr.contains("NORMAL")) ReadGlbAccessor(j, bin, attr.at("NORMAL").get<int>(), 3, nrm);
				if (attr.contains("TEXCOORD_0")) ReadGlbAccessor(j, bin, attr.at("TEXCOORD_0").get<int>(), 2, uv);
				ReadGlbAccessor(j, bin, attr.at("JOINTS_0").get<int>(), 4, jn);
				ReadGlbAccessor(j, bin, attr.at("WEIGHTS_0").get<int>(), 4, wt);
				const size_t corners = pos.size() / 3;
				if (uv.size() != corners * 2) uv.assign(corners * 2, 0.0);
				if (jn.size() != corners * 4 || wt.size() != corners * 4) continue;
				/* Weights stored as whole numbers are fractions of their largest (read so when they say they are normalised). */
				const auto &wa = j.at("accessors").at(attr.at("WEIGHTS_0").get<int>());
				const int wtype = wa.at("componentType").get<int>();
				const double wscale = wa.value("normalized", false) ? 1.0 : wtype == 5121 ? 1.0 / 255 : wtype == 5123 ? 1.0 / 65535 : 1.0;
				if (p.contains("indices")) {
					ReadGlbAccessor(j, bin, p.at("indices").get<int>(), 1, idx);
				} else {
					for (size_t i = 0; i < corners; i++) idx.push_back(static_cast<double>(i));
				}
				const uint8_t tex = texture_of(p.value("material", -1));
				const std::array<double, 6> ut = GlbUvTransform(j, p.value("material", -1));
				for (size_t i = 0; i < corners; i++) {
					const double u = uv[i * 2], v = uv[i * 2 + 1];
					uv[i * 2] = ut[0] + ut[2] * u + ut[3] * v;
					uv[i * 2 + 1] = ut[1] + ut[4] * u + ut[5] * v;
				}
				const uint32_t base = static_cast<uint32_t>(sk->pos.size() / 3);
				const bool normals = nrm.size() == corners * 3 && (sk->pos.empty() || !sk->nrm.empty());
				if (!normals) sk->nrm.clear();
				sk->pos.insert(sk->pos.end(), pos.begin(), pos.end());
				if (normals) sk->nrm.insert(sk->nrm.end(), nrm.begin(), nrm.end());
				sk->uv.insert(sk->uv.end(), uv.begin(), uv.end());
				for (size_t i = 0; i < corners; i++) {
					std::array<uint16_t, 4> js;
					std::array<float, 4> ws;
					float sum = 0;
					for (int k = 0; k < 4; k++) {
						js[k] = static_cast<uint16_t>(skin_first[skin_index] + static_cast<size_t>(jn[i * 4 + k]));
						ws[k] = static_cast<float>(wt[i * 4 + k] * wscale);
						sum += ws[k];
					}
					if (sum > 0) for (float &w : ws) w /= sum;
					sk->joint_of.push_back(js);
					sk->weight_of.push_back(ws);
				}
				for (size_t t = 0; t + 2 < idx.size(); t += 3) {
					if (idx[t] >= corners || idx[t + 1] >= corners || idx[t + 2] >= corners) continue;
					for (int k = 0; k < 3; k++) sk->tri.push_back(base + static_cast<uint32_t>(idx[t + k]));
					sk->tri_tex.push_back(tex);
				}
			}
		}
		if (sk->tri.empty()) {
			Debug(misc, 0, "3D skeleton {}: no mesh with a skin", path);
			return nullptr;
		}
		if (j.contains("animations")) {
			for (const auto &a : j.at("animations")) {
				GlbSkeleton::Animation anim;
				anim.name = a.value("name", std::string());
				anim.start = 1e18;
				anim.end = -1e18;
				const auto &samplers = a.at("samplers");
				for (const auto &c : a.at("channels")) {
					const auto &target = c.at("target");
					if (!target.contains("node")) continue;
					const std::string path_name = target.at("path").get<std::string>();
					const int path_kind = path_name == "translation" ? 0 : path_name == "rotation" ? 1 : path_name == "scale" ? 2 : -1;
					if (path_kind < 0) continue; // the weights of morph targets: none here
					const auto &s = samplers.at(c.at("sampler").get<int>());
					GlbSkeleton::Channel ch;
					ch.node = target.at("node").get<int>();
					ch.path = path_kind;
					const std::string interp = s.value("interpolation", std::string("LINEAR"));
					ch.step = interp == "STEP";
					ch.cubic = interp == "CUBICSPLINE";
					std::vector<double> times, values;
					ReadGlbAccessor(j, bin, s.at("input").get<int>(), 1, times);
					ReadGlbAccessor(j, bin, s.at("output").get<int>(), path_kind == 1 ? 4 : 3, values);
					if (times.empty() || values.size() < times.size() * (path_kind == 1 ? 4 : 3) * (ch.cubic ? 3 : 1)) continue;
					ch.times.assign(times.begin(), times.end());
					ch.values.assign(values.begin(), values.end());
					anim.start = std::min(anim.start, times.front());
					anim.end = std::max(anim.end, times.back());
					anim.channels.push_back(std::move(ch));
				}
				if (!anim.channels.empty()) sk->animations.push_back(std::move(anim));
			}
		}
	} catch (const std::exception &e) {
		Debug(misc, 0, "3D skeleton {}: {}", path, e.what());
		return nullptr;
	}
	if (sk->tri.empty()) return nullptr;
	Debug(misc, 1, "3D skeleton {}: {} joints, {} corners, {} animations", path, sk->joints.size(), sk->pos.size() / 3, sk->animations.size());
	slot = std::move(sk);
	return slot.get();
}

/**
 * Read one .glb into a building: its mesh (ReadGlbMesh()) from metres into units, the texture kept as it is.
 * @return whether there is something to draw
 */
/**
 * Which side of each triangle faces out: its corners' order, or the other when its normals say so (a mirrored node);
 * neither for a material seen from both sides. Drawn, the side facing away is left out, so the back walls never come
 * through the front ones (DrawTurnedObject()).
 */
static std::vector<int8_t> TriangleFacing(const GlbMesh &mesh)
{
	std::vector<int8_t> facing;
	facing.reserve(mesh.tri.size() / 3);
	for (size_t t = 0; t + 2 < mesh.tri.size(); t += 3) {
		size_t k = t / 3;
		bool both = k < mesh.tri_tex.size() && mesh.tri_tex[k] < mesh.textures.size() && mesh.textures[mesh.tri_tex[k]].double_sided;
		const float *a = &mesh.pos[mesh.tri[t] * 3], *b = &mesh.pos[mesh.tri[t + 1] * 3], *c = &mesh.pos[mesh.tri[t + 2] * 3];
		double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
		double gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
		int8_t f = 1;
		if (both || gx * gx + gy * gy + gz * gz < 1e-12) {
			f = 0;
		} else if (!mesh.nrm.empty()) {
			double s = 0;
			for (int i = 0; i < 3; i++) {
				const float *n = &mesh.nrm[mesh.tri[t + i] * 3];
				s += gx * n[0] + gy * n[1] + gz * n[2];
			}
			f = s > 0 ? 1 : (s < 0 ? -1 : 0);
		}
		facing.push_back(f);
	}
	return facing;
}

/**
 * The light of each corner from its normal: lit by the game as the vehicles are, by one light fixed to the world (the
 * colleague's hra/3d/PROSBA-DOMY-Z-BUDOVY3D.md); its axes are the world's. Without normals, the light is in the texture.
 */
static std::vector<uint16_t> CornerLight(const std::vector<float> &nrm)
{
	static const double L[3] = {-0.55, 0.35, 0.75};
	const double ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	std::vector<uint16_t> light;
	light.reserve(nrm.size() / 3);
	for (size_t i = 0; i + 2 < nrm.size(); i += 3) {
		double d = (nrm[i] * L[0] + nrm[i + 1] * L[1] + nrm[i + 2] * L[2]) / ll;
		light.push_back(static_cast<uint16_t>(std::lround(256.0 * (0.55 + 0.45 * std::max(0.0, d)))));
	}
	return light;
}

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
	obj.facing = TriangleFacing(mesh);
	obj.uv = std::move(mesh.uv);
	obj.tri = std::move(mesh.tri);
	obj.png = std::move(mesh.png);
	/* Its night: the node "noc" (the lit windows) and the places "svetlo_NN" (the glows), when it has them. */
	GlbMesh night;
	std::vector<float> lights;
	ReadGlbMesh(path, night, true, {}, 0, "noc", "svetlo_", &lights);
	for (size_t i = 0; i + 2 < night.pos.size(); i += 3) {
		obj.night_pos.push_back(static_cast<float>(night.pos[i] * OBJECT_UNITS_PER_METRE));
		obj.night_pos.push_back(static_cast<float>(night.pos[i + 1] * OBJECT_UNITS_PER_METRE));
		obj.night_pos.push_back(static_cast<float>(night.pos[i + 2] * OBJECT_HEIGHT_PER_METRE));
	}
	obj.night_uv = std::move(night.uv);
	obj.night_tri = std::move(night.tri);
	for (size_t i = 0; i + 2 < lights.size(); i += 3) {
		obj.lights.push_back(static_cast<float>(lights[i] * OBJECT_UNITS_PER_METRE));
		obj.lights.push_back(static_cast<float>(lights[i + 1] * OBJECT_UNITS_PER_METRE));
		obj.lights.push_back(static_cast<float>(lights[i + 2] * OBJECT_HEIGHT_PER_METRE));
	}
	if (!mesh.nrm.empty()) obj.light = CornerLight(mesh.nrm);
	/* An add-on is lit again as it is turned at each of its places (LayAddons()): its normals kept. */
	if (obj.addon) {
		obj.nrm = std::move(mesh.nrm);
		return true;
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
 * The indices of the packages of GRFs (the player, 15. 10.: "objects must be as independent as GRFs: whether I give
 * the Sergej object or not, the game cannot carry it, it is to be with the GRF"; the colleague's
 * hra/3d/PROSBA-OBJEKTY-U-GRF.md): <package>/objekty/<set>/objekty.json beside a GRF anywhere in the NewGRF folders,
 * a folder or a tar, as the game finds GRFs.
 */
class PackageIndexScanner : public FileScanner {
public:
	std::vector<std::string> indices;
	std::string skip;                        ///< the folder of the game's own sets, read as those (baseset/decouple/objekty/)
	std::string_view in_tar = IN_TAR;        ///< how a file in a tar is named

	bool AddFile(const std::string &filename, size_t, const std::string &tar_filename) override
	{
		if (!this->skip.empty() && filename.starts_with(this->skip)) return false;
		/* .../objekty/<set>/objekty.json */
		std::string_view name = filename;
		size_t last = name.find_last_of(PATHSEPCHAR);
		if (last == std::string_view::npos || name.substr(last + 1) != "objekty.json") return false;
		size_t set = name.find_last_of(PATHSEPCHAR, last - 1);
		if (set == std::string_view::npos || set < 7 || name.substr(set - 7, 7) != "objekty" || (set > 7 && name[set - 8] != PATHSEPCHAR)) return false;
		this->indices.push_back(tar_filename.empty() ? filename : std::string(this->in_tar) + filename);
		return true;
	}
};

/**
 * The indices of the objects: the game's own, baseset/decouple/objekty/<set>/objekty.json (the houses of the game and
 * the figures it draws), each set from the first place the game reads its data from that has it -- the player's own
 * folder before the game's, so a set there needs no build of the game; then the packages of the GRFs. In the same
 * order on every computer: a house in two folders (the colleague's 04, the same block below the snow in mirne/ and
 * in subarkticke/) is the first folder's.
 */
static std::vector<std::string> FindObjectIndices()
{
	std::vector<std::string> indices;
	std::set<std::string> sets;
	for (Searchpath sp : _valid_searchpaths) {
		std::string dir = FioGetDirectory(sp, Subdirectory::Baseset) + "decouple" PATHSEP "objekty";
		std::error_code ec;
		if (!std::filesystem::is_directory(OTTD2FS(dir), ec)) continue;
		std::vector<std::string> here;
		for (const auto &entry : std::filesystem::directory_iterator(OTTD2FS(dir), ec)) {
			std::string set = FS2OTTD(entry.path().filename().native());
			std::string index = FS2OTTD(entry.path().native()) + PATHSEP "objekty.json";
			if (sets.contains(set) || !std::filesystem::is_regular_file(OTTD2FS(index), ec)) continue;
			sets.insert(set);
			here.push_back(index);
		}
		std::sort(here.begin(), here.end());
		indices.insert(indices.end(), here.begin(), here.end());
	}
	PackageIndexScanner packages;
	packages.Scan(".json", Subdirectory::NewGrf);
	/* And packages in baseset/decouple/ beside the game's own sets, a folder or a tar (the player: "from the decouple
	 * folder, tar"); a GRF there is one of the game's own (GRFFileScanner). */
	for (Searchpath sp : _valid_searchpaths) {
		std::string dir = FioGetDirectory(sp, Subdirectory::Baseset) + "decouple" PATHSEP;
		if (!FileExists(dir)) continue;
		PackageIndexScanner here;
		here.skip = dir + "objekty" PATHSEP;
		here.Scan(".json", dir);
		packages.indices.insert(packages.indices.end(), here.indices.begin(), here.indices.end());
	}
	{
		PackageIndexScanner tars;
		tars.in_tar = IN_BASESET_TAR;
		for (const auto &[name, entry] : _tar_filelist[Subdirectory::Baseset]) {
			if (IsInBuiltinGRFDir(entry.tar_filename)) tars.AddFile(name, 0, entry.tar_filename);
		}
		packages.indices.insert(packages.indices.end(), tars.indices.begin(), tars.indices.end());
	}
	std::sort(packages.indices.begin(), packages.indices.end());
	Debug(misc, 1, "3D objects: {} sets of the game's own, {} of the packages of GRFs", indices.size(), packages.indices.size());
	for (const std::string &index : packages.indices) Debug(misc, 2, "3D objects: a package's set {}", index);
	indices.insert(indices.end(), packages.indices.begin(), packages.indices.end());
	return indices;
}

/**
 * Where the game looks for the packages of GRFs, for the log: the NewGRF folders and baseset/decouple/ of each place
 * the game reads its data from, folders and tars in them.
 */
std::string ThreeDPackagePlaces()
{
	std::string places;
	for (Searchpath sp : _valid_searchpaths) {
		for (const std::string &dir : {FioGetDirectory(sp, Subdirectory::NewGrf), FioGetDirectory(sp, Subdirectory::Baseset) + "decouple" PATHSEP}) {
			if (!FileExists(dir) || places.find(dir) != std::string::npos) continue;
			if (!places.empty()) places += ", ";
			places += dir;
		}
	}
	return places.empty() ? std::string("no folder") : places;
}

/**
 * Whether a package lies beside the GRF of an id the game has loaded: the GRF's folder, as the game names it (in a
 * NewGRF folder, or in a tar), is the package's. The player may keep an old and a new version of a set side by side
 * (the colleague's hra/3d/PROSBA-TATRA-V6.md: "the one beside the loaded GRF wins"). The names in a tar are the
 * game's in small letters, so compared so.
 */
static bool PackageBesideLoadedGrf(std::string package, uint32_t grfid)
{
	auto plain = [](std::string &s) {
		for (char &ch : s) {
			if (ch == '\\') ch = '/';
			if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
		}
	};
	for (const auto &c : _grfconfig) {
		if (c->ident.grfid != grfid) continue;
		std::string grf = c->filename;
		plain(grf);
		const size_t sep = grf.find_last_of('/');
		if (sep == std::string::npos) return false;
		const std::string folder = grf.substr(0, sep + 1);
		if (package.starts_with(IN_TAR)) package.erase(0, IN_TAR.size());
		if (package.starts_with(IN_BASESET_TAR)) package.erase(0, IN_BASESET_TAR.size());
		plain(package);
		/* The package's folder is the one its objekty/ is in, beside the GRF. */
		if (package.ends_with("objekty/")) package.resize(package.size() - std::string_view("objekty/").size());
		return package.ends_with(folder) && (package.size() == folder.size() || package[package.size() - folder.size() - 1] == '/');
	}
	return false;
}

/** The package an index is of: the folder its set's folder is in (.../objekty/), the figures of a vehicle found there first. */
static std::string PackageOf(const std::string &index)
{
	size_t last = index.find_last_of(PATHSEPCHAR);
	size_t set = last == std::string::npos || last == 0 ? std::string::npos : index.find_last_of(PATHSEPCHAR, last - 1);
	return set == std::string::npos ? std::string() : index.substr(0, set + 1);
}

/** The most parts painted a colour by number an object has: ten of three bits below the top bit of its paint (PaintBits()). */
static constexpr size_t THREE_D_PAINT_PARTS_MAX = 10;

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
		if (obj.colours.size() < THREE_D_PAINT_PARTS_MAX) obj.colours.push_back(std::move(part));
	}
}

/**
 * The plants standing on a building or a field ("rostliny", the colleague's hra/3d/PROSBA-PRUMYSL-3D.md): a plant's
 * file ("druh": "rostlina" of the index, its foot at the origin, 1 m high) at every place of the list -- "poloha"
 * metres in the building's frame, "otoceni" degrees about z, "meritko" [x, y, z] its size in metres. Drawn as the
 * figures, a picture of the plant at the view's angle kept for every one alike: baked into the building they would
 * be hundreds of thousands of triangles a tile.
 */
static void ReadPlants(const nlohmann::json &e, ThreeDObject &obj)
{
	if (!e.contains("rostliny") || !e.at("rostliny").is_array()) return;
	for (const auto &p : e.at("rostliny")) {
		if (!p.is_object() || !p.contains("soubor") || !p.contains("poloha") || !p.at("poloha").is_array() || p.at("poloha").size() != 3) continue;
		ThreeDObjectFigure plant;
		plant.figure = p.at("soubor").get<std::string>();
		if (plant.figure.ends_with(".glb")) plant.figure.resize(plant.figure.size() - 4);
		for (int k = 0; k < 3; k++) plant.at[k] = p.at("poloha").at(k).get<float>();
		plant.facing = p.value("otoceni", 0.0f);
		if (p.contains("meritko") && p.at("meritko").is_array() && p.at("meritko").size() == 3) {
			const float sx = p.at("meritko").at(0).get<float>(), sy = p.at("meritko").at(1).get<float>(), sz = p.at("meritko").at(2).get<float>();
			plant.scale = std::clamp((sx + sy) / 2, 0.05f, 50.0f);
			plant.stretch = std::clamp(sz / plant.scale, 0.1f, 10.0f);
		} else if (p.contains("meritko") && p.at("meritko").is_number()) {
			plant.scale = std::clamp(p.at("meritko").get<float>(), 0.05f, 50.0f);
		}
		obj.plants.push_back(std::move(plant));
	}
}

/**
 * The name of a house that is a 3D object ("nazev"), shown in the land area information instead of the original house's
 * (the colleague's hra/3d/PROSBA-UV-KSC.md: house 0x1F is the ÚV KSČ building): one name, or one per language.
 */
static void ReadBuildingTitle(const nlohmann::json &e, ThreeDObject &obj)
{
	/* Where a car of the game's own driving to it stops ("stani", the ÚV KSČ building), metres in the house's frame. */
	if (e.contains("stani") && e.at("stani").is_object() && e.at("stani").contains("poloha") && e.at("stani").at("poloha").is_array() && e.at("stani").at("poloha").size() == 3) {
		const auto &st = e.at("stani");
		obj.stop[0] = st.at("poloha").at(0).get<float>() * OBJECT_UNITS_PER_METRE;
		obj.stop[1] = st.at("poloha").at(1).get<float>() * OBJECT_UNITS_PER_METRE;
		obj.stop[2] = st.at("poloha").at(2).get<float>() * OBJECT_HEIGHT_PER_METRE;
		obj.stop_turn = st.value("otoceni", 0.0f);
		obj.has_stop = true;
	}
	if (!e.contains("nazev")) return;
	const auto &n = e.at("nazev");
	if (n.is_string()) {
		obj.title_cs = obj.title_en = n.get<std::string>();
	} else if (n.is_object()) {
		obj.title_cs = n.value("cs", std::string());
		obj.title_en = n.value("en", obj.title_cs);
		if (obj.title_cs.empty()) obj.title_cs = obj.title_en;
	}
}

/**
 * The figure a place names: "postava", and of a figure of poses the pose it is in ("poza", the player, 18. 10.: "one
 * figure object everywhere, with what it has to do"), "<postava>/<poza>". Without a pose the figure as it is named.
 */
static std::string FigureNamed(const nlohmann::json &f)
{
	std::string name = f.at("postava").get<std::string>();
	if (f.contains("poza") && f.at("poza").is_string() && !f.at("poza").get<std::string>().empty()) name += "/" + f.at("poza").get<std::string>();
	return name;
}

/** The figures standing on a building ("postavy") and its doors ("dvere"). */
/** A figure of a list ("postava", "poza", "poloha", "otoceni", "meritko"); whether it is one. */
static bool ReadFigure(const nlohmann::json &f, ThreeDObjectFigure &fig)
{
	if (!f.is_object() || !f.contains("postava") || !f.contains("poloha")) return false;
	const auto &at = f.at("poloha");
	if (!at.is_array() || at.size() != 3) return false;
	fig.figure = FigureNamed(f);
	for (int k = 0; k < 3; k++) fig.at[k] = at.at(k).get<float>();
	fig.facing = f.value("otoceni", 0.0f);
	fig.scale = std::clamp(f.value("meritko", 1.0f), 0.1f, 10.0f);
	return true;
}

static void ReadBuildingFigures(const nlohmann::json &e, ThreeDObject &obj)
{
	if (e.contains("postavy") && e.at("postavy").is_array()) for (const auto &f : e.at("postavy")) {
		ThreeDObjectFigure fig;
		if (ReadFigure(f, fig)) obj.figures.push_back(std::move(fig));
	}
	/* Its event ("akce", the player, 18. 10.: on the new stadium every week or two for one day the firemen with their
	 * Tatra, the pig on the spit, Mat drawing beer and the girls of the pub sitting there): the figures standing about it
	 * on its days (GetThreeDHouseEvent()), each as one of "postavy"; without "postava" the game's own -- the spit, Mat at
	 * the tap, the girls sitting by turns (their files of a pose a file till the figures have poses). */
	if (e.contains("akce") && e.at("akce").is_object()) {
		const auto &event = e.at("akce");
		auto stand = [&obj](const nlohmann::json &p, std::string_view figure) {
			if (!p.is_object()) return;
			nlohmann::json f = p;
			if (!f.contains("postava")) {
				if (figure.empty()) return;
				f["postava"] = figure;
				f.erase("poza");
			}
			ThreeDObjectFigure fig;
			if (ReadFigure(f, fig)) obj.event_figures.push_back(std::move(fig));
		};
		if (event.contains("rozen")) stand(event.at("rozen"), "rozen");
		if (event.contains("mat")) stand(event.at("mat"), "mat_toci");
		static constexpr std::string_view girls[] = {"college_girl_sedi", "people_girl_sedi", "galaxia_sedi"};
		if (event.contains("holky") && event.at("holky").is_array()) {
			size_t k = 0;
			for (const auto &g : event.at("holky")) stand(g, girls[k++ % std::size(girls)]);
		}
		/* The Tatra standing on the pitch (the colleague's "tatra": its record "zaznam", its set "sada" or the sets of its
		 * layers -- the cab, the grille, the body), empty -- the player, 18. 10.: "on the pitch stands an empty Tatra,
		 * without a driver" --, drawn as a figure of its own name "vozidlo:<record>|<set>|<set>..."
		 * (GetThreeDStandingVehicle() of three_d_vehicle_object.cpp); none without its package. */
		if (event.contains("tatra") && event.at("tatra").is_object()) {
			const auto &t = event.at("tatra");
			const std::string record = t.value("zaznam", t.value("objekt", std::string()));
			if (!record.empty()) {
				nlohmann::json f = t;
				std::string name = "vozidlo:" + record;
				if (t.contains("sada") && t.at("sada").is_string()) name += "|" + t.at("sada").get<std::string>();
				if (t.contains("sada") && t.at("sada").is_array()) {
					for (const auto &set : t.at("sada")) {
						if (set.is_string()) name += "|" + set.get<std::string>();
					}
				}
				f["postava"] = name;
				f.erase("poza");
				ThreeDObjectFigure fig;
				if (ReadFigure(f, fig)) obj.event_figures.push_back(std::move(fig));
			}
		}
		/* The firemen are the Tatras' (their package's figures): only those named. */
		if (event.contains("hasici") && event.at("hasici").is_array()) {
			for (const auto &h : event.at("hasici")) stand(h, {});
		}
	}
	/* Its add-ons ("doplnky"): an add-on of the index by its name ("jmeno"), where its foot stands (metres in the
	 * building's frame, as "postavy"), turned about it ("otoceni", degrees, 0 as it is made, 90 its +x to +y), sized
	 * ("meritko"). Laid into its shape when it is read (LayAddons()). */
	if (e.contains("doplnky") && e.at("doplnky").is_array()) for (const auto &a : e.at("doplnky")) {
		if (!a.is_object() || !a.contains("jmeno") || !a.at("jmeno").is_string() || !a.contains("poloha")) continue;
		const auto &at = a.at("poloha");
		if (!at.is_array() || at.size() != 3) continue;
		ThreeDObjectFigure place;
		place.figure = a.at("jmeno").get<std::string>();
		for (int k = 0; k < 3; k++) place.at[k] = at.at(k).get<float>();
		place.facing = a.value("otoceni", 0.0f);
		place.scale = std::clamp(a.value("meritko", 1.0f), 0.05f, 20.0f);
		obj.addon_places.push_back(std::move(place));
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
	/* The places on its flat roof (the colleague's desert houses, "strechy"): a monkey sits there. */
	if (e.contains("strechy") && e.at("strechy").is_array()) {
		for (const auto &r : e.at("strechy")) {
			if (!r.is_object() || !r.contains("poloha") || !r.at("poloha").is_array() || r.at("poloha").size() < 3) continue;
			ThreeDObjectRoof roof;
			for (int k = 0; k < 3; k++) roof.at[k] = r.at("poloha").at(k).get<float>();
			if (r.contains("smer") && r.at("smer").is_array() && r.at("smer").size() >= 2) {
				for (int k = 0; k < 2; k++) roof.out[k] = r.at("smer").at(k).get<float>();
			}
			obj.roofs.push_back(roof);
		}
	}
}

/** Where Matylda stands by the pub, metres in its frame (the pub's object, its north corner), and her plants in front of her. */
static constexpr float PUB_MATYLDA[3] = {6.0f, 22.0f, 0.0f};
static constexpr float PUB_MATYLDA_FACING = 0.0f;

/**
 * Matylda by the pub with her marijuana in pots (the player, 21. 10.: "Matylda will sell the whole plant in a pot, by
 * the pub there should be marijuana plants with a pot"; she sells them, walker_cmd.cpp): she always stands there, her
 * pots on the grass in front of her, a plant in each (our kvetinac, the colleague's kytka_mari_*). Added to the pub's
 * object here, its index the colleague's.
 */
static void AddMatyldaToPub(ThreeDObject &obj)
{
	ThreeDObjectFigure matylda;
	matylda.figure = "matylda";
	std::copy(std::begin(PUB_MATYLDA), std::end(PUB_MATYLDA), matylda.at);
	matylda.facing = PUB_MATYLDA_FACING;
	matylda.scale = 2.0f; // as the pub's own figures
	obj.figures.push_back(std::move(matylda));
	static const char *const PLANTS[] = {"kytka_mari_mlada", "kytka_mari_vysoka", "kytka_mari_kosata"};
	static const float SIZES[] = {WALKER_POT_PLANT_M, 1.1f, 1.3f};
	for (int i = 0; i < 3; i++) {
		const float x = PUB_MATYLDA[0] + 1.5f, y = PUB_MATYLDA[1] - 1.3f + 1.3f * i;
		ThreeDObjectFigure pot;
		pot.figure = "kvetinac";
		pot.at[0] = x;
		pot.at[1] = y;
		pot.facing = 30.0f * i;
		pot.scale = WALKER_POT_M;
		pot.stretch = WALKER_POT_HEIGHT_M / WALKER_POT_M;
		obj.plants.push_back(std::move(pot));
		ThreeDObjectFigure plant;
		plant.figure = PLANTS[i];
		plant.at[0] = x;
		plant.at[1] = y;
		plant.at[2] = WALKER_POT_SOIL_M;
		plant.facing = 70.0f * i;
		plant.scale = SIZES[i];
		obj.plants.push_back(std::move(plant));
	}
}

/** The game's own industry an index names ("prumysl"): its constant's name; 0xFF for one the game has not (yet). */
static IndustryType IndexIndustryType(std::string_view name)
{
	if (name == "IT_GYMNASIUM") return IT_GYMNASIUM;
	if (name == "IT_STATUE") return IT_STATUE;
	if (name == "IT_WEED_MACHINE") return IT_WEED_MACHINE;
	if (name == "IT_COFFEESHOP") return IT_COFFEESHOP;
	if (name == "IT_MARIJUANA_PLANTATION") return IT_MARIJUANA_PLANTATION;
	if (name == "lihovar" || name == "IT_DISTILLERY") return IT_DISTILLERY;
	if (name == "hospoda" || name == "IT_PUB") return IT_PUB;
	if (name == "IT_OIL_REFINERY") return 4; // the original game's, IT_OIL_REFINERY of table/build_industry.h
	return 0xFF;
}

/**
 * One of the game's own industries as a 3D object ("druh": "prumysl", the colleague's hra/3d/PROSBA-PRUMYSL-3D.md):
 * "prumysl" the industry, "varianta" the statue's stone ("kamen") or bronze ("bronz"); or a part of a field ("druh":
 * "pole", "dil") the game lays tile by tile: the soil with the girls at work on it ("prace", of marijuana and of
 * potatoes), the field road with the girls by it ("u_cesty"), the shed, the haystack, the plants of each stage (only
 * "rostliny", no file of its own).
 */
static void ReadIndustryObject(const nlohmann::json &e, const std::string &folder, bool field)
{
	ThreeDObject obj;
	obj.file = e.value("soubor", std::string());
	obj.name = e.value("jmeno", obj.file);
	obj.variants = 0; // no house's
	obj.tiles_x = std::max(1, e.value("dlazdic_x", 1));
	obj.tiles_y = std::max(1, e.value("dlazdic_y", 1));
	if (!obj.file.empty() && !ReadGlb(folder + obj.file, obj)) return;
	ReadColourParts(e, folder, obj);
	ReadBuildingFigures(e, obj);
	ReadBuildingTitle(e, obj);
	ReadPlants(e, obj);
	if (field) {
		obj.field_part = e.value("dil", std::string());
		if (obj.field_part.empty()) return;
		if (e.contains("prace") && e.at("prace").is_object()) {
			static const char *const crops[2] = {"marihuana", "brambory"};
			for (int k = 0; k < 2; k++) {
				if (!e.at("prace").contains(crops[k]) || !e.at("prace").at(crops[k]).is_array()) continue;
				for (const auto &f : e.at("prace").at(crops[k])) {
					ThreeDObjectFigure fig;
					if (ReadFigure(f, fig)) obj.work[k].push_back(std::move(fig));
				}
			}
		}
		if (e.contains("u_cesty") && e.at("u_cesty").is_array()) {
			for (const auto &f : e.at("u_cesty")) {
				ThreeDObjectFigure fig;
				if (ReadFigure(f, fig)) obj.by_road[f.value("okraj", std::string("sz")) == "jv" ? 1 : 0].push_back(std::move(fig));
			}
		}
	} else {
		/* Its girls when they are there (the colleague's hra/3d/PROSBA-PRUMYSL-HOLKY-NOC.md): after the deliveries. */
		auto list = [&e](const char *key, std::vector<ThreeDObjectFigure> &into) {
			if (!e.contains(key) || !e.at(key).is_array()) return;
			for (const auto &f : e.at(key)) {
				ThreeDObjectFigure fig;
				if (ReadFigure(f, fig)) into.push_back(std::move(fig));
			}
		};
		list("postavy_s_holkami", obj.girls);
		list("postavy_stoji", obj.hut_girls[0]);
		list("postavy_sedi", obj.hut_girls[1]);
		obj.industry_name = e.value("prumysl", std::string());
		obj.industry = IndexIndustryType(obj.industry_name);
		const std::string variant = e.value("varianta", std::string());
		obj.industry_variant = variant == "kamen" ? 0 : variant == "bronz" ? 1 : -1;
		/* One statue for both, its stone or bronze a colour of its palette (the colleague's hra/3d/SOCHA-BARVA-PRO-HRU.md;
		 * the player: "the statue is there twice, make it one with a colour"). */
		if (e.contains("varianty") && e.at("varianty").is_object()) {
			for (const auto &[name, at] : e.at("varianty").items()) {
				if (!at.is_number_integer()) continue;
				if (name == "kamen") obj.variant_paint[0] = static_cast<int8_t>(std::clamp(at.get<int>(), 0, 15));
				if (name == "bronz") obj.variant_paint[1] = static_cast<int8_t>(std::clamp(at.get<int>(), 0, 15));
			}
		}
		if (e.contains("sdili") && e.at("sdili").is_array()) {
			for (const auto &sh : e.at("sdili")) {
				if (sh.is_object() && sh.contains("prumysl")) obj.shared_with.push_back(sh.at("prumysl").get<std::string>());
			}
		}
		if (obj.industry == 0xFF) Debug(misc, 1, "3D object {}: the game has no industry {} (yet)", obj.name, obj.industry_name);
		if (obj.industry == IT_PUB) AddMatyldaToPub(obj);
	}
	if (obj.tri.empty() && obj.plants.empty()) return;
	_three_d_objects.push_back(std::move(obj));
}

/** A part of the original industries ("druh": "dil") as read, in metres, to be placed on the tiles. */
struct IndustryPartMesh {
	GlbMesh mesh;              ///< its day
	std::vector<int8_t> facing; ///< its triangles' outer sides (TriangleFacing())
	GlbMesh night;             ///< its lit windows (the node "noc")
	std::vector<float> lights; ///< its glows ("svetlo_NN")
	size_t holder = SIZE_MAX;  ///< the object of the part, keeping its texture
};

/** A tile of the original industries drawn from the parts ("druh": "prumysl_dlazdice"): the objects by their places. */
struct OriginalIndustryTile {
	std::string ground;          ///< "podklad": the name of its object
	std::vector<size_t> parts;   ///< its parts placed
	std::vector<std::string> whole; ///< parts of another set ("sada", the field's haystack and shed) standing as they are on the tile, by name
	size_t plants = SIZE_MAX;    ///< "rostliny"
	bool member = false;         ///< "soucast_bloku"
	int anchor_dx = 0, anchor_dy = 0;
};
/** The tiles of the original industries by their gfx (GetThreeDOriginalIndustryTile()). */
static std::map<IndustryGfx, OriginalIndustryTile> _original_industry_tiles;

/**
 * A part placed on a tile (the colleague's hra/3d/PROSBA-PRUMYSL-VANILLA-3D.md): its origin in the middle of its
 * ground plan, facing +x, turned ("otoceni", degrees about z, as the figures), sized ("meritko") and put where
 * "poloha" says, metres from the north corner of the tile (of the block's anchor); lit by its normals turned with it.
 * Its texture is the part's own object's (ThreeDObject::texture_of).
 */
static ThreeDObject PlaceIndustryPart(const IndustryPartMesh &part, std::string name, const float at[3], float turn, float scale, int tiles_x, int tiles_y)
{
	ThreeDObject obj;
	obj.name = std::move(name);
	obj.variants = 0; // no house's
	obj.tiles_x = tiles_x;
	obj.tiles_y = tiles_y;
	obj.texture_of = part.holder;
	const double a = turn * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
	auto place = [&](const std::vector<float> &from, std::vector<float> &to) {
		to.reserve(from.size());
		for (size_t i = 0; i + 2 < from.size(); i += 3) {
			const double x = from[i] * scale, y = from[i + 1] * scale, z = from[i + 2] * scale;
			to.push_back(static_cast<float>((at[0] + x * c - y * s) * OBJECT_UNITS_PER_METRE));
			to.push_back(static_cast<float>((at[1] + x * s + y * c) * OBJECT_UNITS_PER_METRE));
			to.push_back(static_cast<float>((at[2] + z) * OBJECT_HEIGHT_PER_METRE));
		}
	};
	place(part.mesh.pos, obj.pos);
	for (size_t i = 2; i < obj.pos.size(); i += 3) obj.top = std::max(obj.top, obj.pos[i]);
	obj.uv = part.mesh.uv;
	obj.tri = part.mesh.tri;
	obj.facing = part.facing;
	if (!part.mesh.nrm.empty()) {
		std::vector<float> turned(part.mesh.nrm.size());
		for (size_t i = 0; i + 2 < turned.size(); i += 3) {
			const float nx = part.mesh.nrm[i], ny = part.mesh.nrm[i + 1];
			turned[i] = static_cast<float>(nx * c - ny * s);
			turned[i + 1] = static_cast<float>(nx * s + ny * c);
			turned[i + 2] = part.mesh.nrm[i + 2];
		}
		obj.light = CornerLight(turned);
	}
	place(part.night.pos, obj.night_pos);
	obj.night_uv = part.night.uv;
	obj.night_tri = part.night.tri;
	place(part.lights, obj.lights);
	return obj;
}

/**
 * The parts of an index of the original industries ("druh": "dil"), each read once: its object (its own texture,
 * and the yards drawn as they are as the ground of a tile), and its mesh in metres to be placed on the tiles.
 */
static std::map<std::string, IndustryPartMesh> ReadIndustryParts(const nlohmann::json &list, const std::string &folder)
{
	std::map<std::string, IndustryPartMesh> parts;
	for (const auto &e : list) {
		if (e.value("druh", std::string()) != "dil" || !e.contains("soubor")) continue;
		const std::string file = e.at("soubor").get<std::string>();
		const std::string path = folder + file;
		IndustryPartMesh pm;
		if (!ReadGlbMesh(path, pm.mesh)) continue;
		ReadGlbMesh(path, pm.night, true, {}, 0, "noc", "svetlo_", &pm.lights);
		pm.facing = TriangleFacing(pm.mesh);
		ThreeDObject holder;
		holder.file = file;
		holder.name = e.value("jmeno", file);
		holder.variants = 0; // no house's
		if (!ReadGlb(path, holder)) continue;
		pm.holder = _three_d_objects.size();
		std::string name = holder.name;
		_three_d_objects.push_back(std::move(holder));
		parts.emplace(std::move(name), std::move(pm));
	}
	return parts;
}

/** A tile of the original industries ("druh": "prumysl_dlazdice"): its ground, its parts placed, its plants, for each of its gfx. */
static void ReadOriginalIndustryTile(const nlohmann::json &e, const std::map<std::string, IndustryPartMesh> &parts)
{
	OriginalIndustryTile tile;
	tile.ground = e.value("podklad", std::string());
	const std::string name = e.value("jmeno", std::string("prumysl_dlazdice"));
	int bx = 1, by = 1;
	if (e.contains("blok") && e.at("blok").is_array() && e.at("blok").size() == 2) {
		bx = std::clamp(e.at("blok").at(0).get<int>(), 1, 8);
		by = std::clamp(e.at("blok").at(1).get<int>(), 1, 8);
	}
	if (e.contains("soucast_bloku") && e.at("soucast_bloku").is_object()) {
		const auto &m = e.at("soucast_bloku");
		tile.member = true;
		if (m.contains("posun") && m.at("posun").is_array() && m.at("posun").size() == 2) {
			tile.anchor_dx = m.at("posun").at(0).get<int>();
			tile.anchor_dy = m.at("posun").at(1).get<int>();
		}
	}
	if (e.contains("dily") && e.at("dily").is_array()) {
		for (const auto &d : e.at("dily")) {
			if (!d.is_object() || !d.contains("dil") || !d.contains("poloha") || !d.at("poloha").is_array() || d.at("poloha").size() != 3) continue;
			const std::string part = d.at("dil").get<std::string>();
			float at[3];
			for (int k = 0; k < 3; k++) at[k] = d.at("poloha").at(k).get<float>();
			auto it = parts.find(part);
			if (it == parts.end()) {
				/* A part of another set, a whole tile as it is (the haystack, the shed of the fields): found by its name
				 * when every set is read. Placed elsewhere or turned it would need its mesh, which is not kept. */
				if (d.contains("sada") && at[0] == 0 && at[1] == 0 && at[2] == 0 && d.value("otoceni", 0.0f) == 0 && d.value("meritko", 1.0f) == 1) {
					tile.whole.push_back(part);
				} else {
					Debug(misc, 0, "3D objects {}: no part {}", name, part);
				}
				continue;
			}
			tile.parts.push_back(_three_d_objects.size());
			_three_d_objects.push_back(PlaceIndustryPart(it->second, name + "/" + part, at, d.value("otoceni", 0.0f), d.value("meritko", 1.0f), bx, by));
		}
	}
	/* Its add-ons ("doplnky", as a house's: the shared trees and bushes of the colleague's STROMY-SPOLECNE-PRO-HRU.md):
	 * one object of them alone, without a file of its own (as a street lamp, MakeStreetLamps()). */
	if (e.contains("doplnky")) {
		ThreeDObject addons;
		addons.name = name + "/doplnky";
		addons.variants = 0; // no house's
		addons.tiles_x = bx;
		addons.tiles_y = by;
		ReadBuildingFigures(e, addons);
		if (!addons.addon_places.empty()) {
			tile.parts.push_back(_three_d_objects.size());
			_three_d_objects.push_back(std::move(addons));
		}
	}
	ThreeDObject plants;
	plants.name = name + "/rostliny";
	plants.variants = 0; // no house's
	ReadPlants(e, plants);
	if (!plants.plants.empty()) {
		tile.plants = _three_d_objects.size();
		_three_d_objects.push_back(std::move(plants));
	}
	if (e.contains("gfx") && e.at("gfx").is_array()) {
		for (const auto &g : e.at("gfx")) {
			if (!_original_industry_tiles.emplace(static_cast<IndustryGfx>(g.get<int>()), tile).second) Debug(misc, 0, "3D objects {}: gfx {} has two records", name, g.get<int>());
		}
	}
}

/** Look for the buildings under baseset/decouple/objekty/<set>/objekty.json, in every place the game reads its data from. */
/** The lamp of the colleague's set "lampy" standing where the game has its street lights. */
static constexpr std::string_view STREET_LAMP = "lampa_na_sloupu";

/**
 * The street lamps (the colleague's hra/3d/PROSBA-NOC-ZBRANE-TRABANT-LAMPY.md, point 4: "lamps along the roads and
 * streets, where exactly the game decides"): where the game has its own street lights -- the roadside of the towns'
 * paved streets (Roadside::StreetLights), at the four places of a tile its sprites stand at, by the middle of an edge --
 * the lamp on its post, its arm over the street towards the tile's middle. An object of that add-on alone at each of
 * the places (no file of its own: Read() lays the add-on into it), drawn as the buildings are (GetThreeDStreetLamp()).
 */
static void MakeStreetLamps()
{
	_three_d_street_lamps.clear();
	if (!_three_d_addons.contains(STREET_LAMP)) return;
	static constexpr int PLACES[][2] = {{1, 8}, {14, 8}, {8, 1}, {8, 14}};
	for (const auto &[dx, dy] : PLACES) {
		ThreeDObject lamp;
		lamp.name = fmt::format("{} {},{}", STREET_LAMP, dx, dy);
		lamp.variants = 0;
		lamp.street = true;
		ThreeDObjectFigure &place = lamp.addon_places.emplace_back();
		place.figure = std::string(STREET_LAMP);
		place.at[0] = static_cast<float>(dx / OBJECT_UNITS_PER_METRE);
		place.at[1] = static_cast<float>(dy / OBJECT_UNITS_PER_METRE);
		place.facing = static_cast<float>(std::atan2(8.0 - dy, 8.0 - dx) * 180.0 / M_PI);
		_three_d_street_lamps.emplace(dx * 16 + dy, _three_d_objects.size());
		_three_d_objects.push_back(std::move(lamp));
	}
}

static void ReadThreeDObjects()
{
	_three_d_objects_read = true;
	ForgetMarginsFilled(MarginsOf::Buildings);
	GlbFilesScope files;
	const auto start = std::chrono::steady_clock::now();
	std::vector<std::string> indices = FindObjectIndices();
	for (const std::string &index : indices) {
		std::vector<uint8_t> text = ReadWholeFile(index);
		std::string folder = index.substr(0, index.size() - std::string_view("objekty.json").size());
		try {
			nlohmann::json list = nlohmann::json::parse(text.begin(), text.end());
			ReadDiary(fmt::format("index {}: {} záznamů", index, list.size()), false);
			const std::map<std::string, IndustryPartMesh> parts = ReadIndustryParts(list, folder);
			for (const auto &e : list) {
				const std::string kind = e.value("druh", std::string("dum"));
				if (kind == "prumysl" || kind == "pole") {
					ReadIndustryObject(e, folder, kind == "pole");
					continue;
				}
				if (kind == "prumysl_dlazdice") {
					ReadOriginalIndustryTile(e, parts);
					continue;
				}
				if (kind == "doplnek" && e.contains("soubor") && e.at("soubor").is_string()) {
					/* An add-on of the buildings (a parasol, a bench): one file, its foot at the origin, facing +x, read
					 * the first time a building with it is drawn (LayAddons()); never a house of its own (variants 0). */
					ThreeDObject addon;
					addon.file = e.at("soubor").get<std::string>();
					addon.name = e.value("jmeno", addon.file);
					addon.glb_path = folder + addon.file;
					NoteMarginsFilled(e, addon.glb_path, MarginsOf::Buildings);
					ReadColourParts(e, folder, addon);
					addon.trunk_r = std::clamp(e.value("kmen_r", 0.0f), 0.0f, 5.0f);
					addon.addon = true;
					addon.variants = 0;
					if (!_three_d_addons.emplace(addon.name, _three_d_objects.size()).second) {
						Debug(misc, 0, "3D objects: add-on {} has two records, the first is used", addon.name);
						continue;
					}
					_three_d_objects.push_back(std::move(addon));
					continue;
				}
				if (kind != "dum") continue;
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
					obj.glb_path = folder + obj.file;
					NoteMarginsFilled(e, obj.glb_path, MarginsOf::Buildings);
					ReadColourParts(e, folder, obj);
					ReadBuildingFigures(e, obj);
					ReadBuildingTitle(e, obj);
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
				obj.theme = ParseIndexTheme(e);
				obj.tiles_x = std::max(1, e.value("dlazdic_x", 1));
				obj.tiles_y = std::max(1, e.value("dlazdic_y", 1));
				if (e.contains("varianty")) {
					obj.variants = 0;
					for (const auto &v : e.at("varianty")) obj.variants |= 1 << (v.get<int>() & 3);
				}
				/* Its shape and texture read the first time it is drawn (ThreeDObject::Read()). */
				obj.glb_path = folder + obj.file;
				NoteMarginsFilled(e, obj.glb_path, MarginsOf::Buildings);
				ReadColourParts(e, folder, obj);
				ReadBuildingFigures(e, obj);
				ReadBuildingTitle(e, obj);
				const std::string snow = houses.size() > 1 && e.contains("snih") ? folder + e.at("snih").get<std::string>() : std::string();
				for (size_t k = 1; k < houses.size(); k++) {
					/* The same shape for the next house of the list; the last ones (above the snow) in the snowy texture. */
					ThreeDObject other = obj;
					other.house = houses[k];
					other.name = obj.name + (snow.empty() ? "" : " snih");
					other.snow_path = snow;
					_three_d_objects.push_back(std::move(other));
				}
				_three_d_objects.push_back(std::move(obj));
			}
		} catch (const std::exception &ex) {
			Debug(misc, 0, "3D objects {}: {}", index, ex.what());
			ReadDiary(fmt::format("index {}: {} (zbytek indexu se nečetl)", index, ex.what()), true);
		}
	}
	MakeStreetLamps();
	for (size_t i = 0; i < _three_d_objects.size(); i++) {
		const ThreeDObject &o = _three_d_objects[i];
		if (o.industry != 0xFF && (o.variant_paint[0] >= 0 || o.variant_paint[1] >= 0)) {
			/* One object for both of its variants by colour: it, and not the old one of each (kept in the set a while). */
			for (int8_t v = 0; v < 2; v++) {
				if (o.variant_paint[v] >= 0) _three_d_industry_objects.insert_or_assign(std::make_pair(o.industry, v), i);
			}
		} else if (o.industry != 0xFF && !_three_d_industry_objects.emplace(std::make_pair(o.industry, o.industry_variant), i).second) {
			const auto it = _three_d_industry_objects.find(std::make_pair(o.industry, o.industry_variant));
			if (_three_d_objects[it->second].variant_paint[0] < 0 && _three_d_objects[it->second].variant_paint[1] < 0) {
				Debug(misc, 0, "3D objects: industry {} variant {} has two records, {} is not drawn", o.industry_name, o.industry_variant, o.name);
			}
		}
		for (const std::string &name : o.shared_with) {
			const IndustryType type = IndexIndustryType(name);
			if (type == 0xFF) {
				Debug(misc, 1, "3D object {}: the game has no industry {} to share it", o.name, name);
			} else {
				_three_d_shared_industry_objects.emplace(type, i);
			}
		}
		if (!o.field_part.empty()) _three_d_field_parts.emplace(o.field_part, i);
		for (uint slot = 0; slot < 4; slot++) {
			if (!(o.variants & (1 << slot))) continue;
			/* One building on a house and variant, the first record of the index (the colleague's
			 * hra/3d/PROSBA-0x1F-JEDEN-DUM.md): a second one is told of, never drawn over it. */
			bool first;
			size_t drawn;
			if (o.grfid != 0) {
				auto [it, added] = _three_d_grf_house_objects.emplace(std::make_tuple(o.grfid, o.grf_house, slot), i);
				first = added;
				drawn = it->second;
			} else {
				auto [it, added] = _three_d_house_objects.emplace(std::make_tuple(o.theme, o.house, slot), i);
				first = added;
				drawn = it->second;
			}
			if (!first) Debug(misc, 0, "3D objects: house {:#04x} variant {} has two records, {} is drawn and {} not", o.grfid != 0 ? o.grf_house : o.house, slot, _three_d_objects[drawn].name, o.name);
		}
	}
	Debug(misc, 1, "3D objects: {} read from {} index(es) in {} ms, the houses' shapes as they are drawn", _three_d_objects.size(), indices.size(), std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	if (indices.empty()) {
		ReadDiary("žádný index baseset/decouple/objekty/<sada>/objekty.json se nenašel, všechny domy jsou krabice", true);
	} else {
		std::set<HouseID> plain, themed;
		for (const auto &[key, i] : _three_d_house_objects) (std::get<0>(key) == 0 ? plain : themed).insert(std::get<1>(key));
		std::set<std::pair<uint32_t, uint16_t>> grf;
		for (const auto &[key, i] : _three_d_grf_house_objects) grf.emplace(std::get<0>(key), std::get<1>(key));
		ReadDiary(fmt::format("celkem {} objektů z {} indexů; 3D model má {} domů hry, {} domů tematických měst a {} domů z GRF", _three_d_objects.size(), indices.size(), plain.size(), themed.size(), grf.size()), false);
	}
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
	if (text == "auto" || text == "silnice" || text == "silnicni" || text == "road") return VehicleType::Road;
	if (text == "lod" || text == "ship") return VehicleType::Ship;
	if (text == "letadlo" || text == "aircraft") return VehicleType::Aircraft;
	return VehicleType::Invalid;
}

/** Read the vehicles of the indices ("druh": "vozidlo"); their .glb are read when they are first drawn. */
static void ReadThreeDVehicleObjects()
{
	_vehicle_objects_read = true;
	ForgetMarginsFilled(MarginsOf::Vehicles);
	GlbFilesScope files;
	for (const std::string &index : FindObjectIndices()) {
		std::vector<uint8_t> text = ReadWholeFile(index);
		std::string folder = index.substr(0, index.size() - std::string_view("objekty.json").size());
		std::string package = PackageOf(index);
		try {
			nlohmann::json list = nlohmann::json::parse(text.begin(), text.end());
			for (const auto &e : list) {
				if (e.value("druh", std::string("dum")) == "efekt" && e.contains("soubor")) {
					/* An effect (the colleague's hra/3d/PROSBA-POSTAVY-ANIMACE.md, the fireworks): frames as a figure's,
					 * metres, unlit when it has no normals; a material of it coloured from a palette by the game. */
					ThreeDVehicleObject fx;
					fx.package = package;
					fx.file = folder + e.at("soubor").get<std::string>();
					NoteMarginsFilled(e, fx.file, MarginsOf::Vehicles);
					fx.name = e.value("jmeno", e.at("soubor").get<std::string>());
					if (e.contains("animace") && e.at("animace").is_object()) {
						const auto &a = e.at("animace");
						fx.frame_prefix = a.value("uzly", std::string("snimek_"));
						fx.frames = std::clamp(a.value("snimku", 0), 0, 64);
						if (fx.frames < 2) fx.frame_prefix.clear();
					}
					if (e.contains("barvy") && e.at("barvy").is_array() && !e.at("barvy").empty()) {
						const auto &b = e.at("barvy").at(0);
						fx.tint_material = b.value("material", std::string());
						if (b.contains("paleta") && b.at("paleta").is_array()) {
							for (const auto &c : b.at("paleta")) {
								auto n = c.is_string() ? ParseInteger<uint32_t>(c.get<std::string>(), 16) : std::nullopt;
								if (n.has_value()) fx.tint_palette.push_back(Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data);
							}
						}
					}
					_effect_objects.push_back(std::move(fx));
					continue;
				}
				const std::string figure_kind = e.value("druh", std::string("dum"));
				if ((figure_kind == "tvor" || figure_kind == "carodejnice") && e.contains("soubor") && e.contains("animace") && e.at("animace").is_object()) {
					/* A creature: each of its animations a figure of its own, "<jmeno>/<animace>". A witch so too, the
					 * glass of her lights and their places read from each frame. */
					const bool witch = figure_kind == "carodejnice";
					ThreeDCreature creature;
					creature.name = e.value("jmeno", e.at("soubor").get<std::string>());
					if (e.contains("rozmer_m") && e.at("rozmer_m").is_array() && e.at("rozmer_m").size() == 3) {
						for (int k = 0; k < 3; k++) creature.size[k] = e.at("rozmer_m").at(k).get<float>();
					}
					for (const auto &[anim, a] : e.at("animace").items()) {
						if (!a.is_object()) continue;
						ThreeDVehicleObject fig;
						fig.package = package;
						fig.file = folder + e.at("soubor").get<std::string>();
						NoteMarginsFilled(e, fig.file, MarginsOf::Vehicles);
						if (e.contains("textura") && e.at("textura").is_string()) fig.paint_file = folder + e.at("textura").get<std::string>();
						fig.name = creature.Figure(anim);
						fig.frame_prefix = a.value("uzly", anim + "_");
						fig.frames = std::clamp(a.value("snimku", 0), 0, 64);
						fig.frame_ticks = std::clamp(a.value("tiku", 1), 1, 1000);
						fig.loop = a.value("smycka", true);
						fig.walk_step = std::clamp(a.value("krok_m", 0.0f), 0.0f, 100.0f);
						fig.frame_lights = witch;
						if (fig.frames < 2) fig.frame_prefix.clear();
						/* With its bones ("kostra") and an animation of the same name there: a frame a tick, the round
						 * as long as the frames' was. */
						if (e.contains("kostra") && e.at("kostra").is_string() && fig.frames >= 2) {
							const std::string bones = folder + e.at("kostra").get<std::string>();
							const GlbSkeleton *sk = ReadGlbSkeleton(bones);
							if (sk != nullptr && sk->Find(anim) != nullptr) {
								fig.skeleton_file = bones;
								fig.skeleton_anim = anim;
								fig.frames = std::min(fig.frames * fig.frame_ticks, 4096);
								fig.frame_ticks = 1;
							}
						}
						_figure_objects.push_back(std::move(fig));
						creature.animations.push_back(anim);
					}
					if (creature.animations.empty()) continue;
					if (!witch) {
						_creatures.push_back(std::move(creature));
						continue;
					}
					/* Her size, and her lights as the index has them (the colleague's hra/3d/PROSBA-CARODEJNICE.md). */
					ThreeDWitch w;
					w.name = creature.name;
					w.animations = std::move(creature.animations);
					std::copy(std::begin(creature.size), std::end(creature.size), std::begin(w.size));
					w.scale = std::clamp(e.value("meritko", 2.0f), 0.1f, 20.0f);
					auto colour_of = [](const nlohmann::json &l) -> uint32_t {
						auto n = l.contains("barva") && l.at("barva").is_string() ? ParseInteger<uint32_t>(l.at("barva").get<std::string>(), 16) : std::nullopt;
						return n.has_value() ? Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data : 0;
					};
					if (e.contains("svetla") && e.at("svetla").is_object()) {
						const auto &lights = e.at("svetla");
						if (lights.contains("lucerna") && lights.at("lucerna").is_object()) {
							w.lantern_colour = colour_of(lights.at("lucerna"));
							w.lantern_reach_m = std::clamp(lights.at("lucerna").value("dosah_m", 0.0f), 0.0f, 50.0f);
						}
						if (lights.contains("svitilna") && lights.at("svitilna").is_object()) {
							w.torch_colour = colour_of(lights.at("svitilna"));
							w.torch_reach_m = std::clamp(lights.at("svitilna").value("dosah_m", 0.0f), 0.0f, 100.0f);
							w.torch_angle = std::clamp(lights.at("svitilna").value("uhel", 0.0f), 1.0f, 120.0f);
						}
					}
					if (!_witches_replaced.contains(w.name)) _witches.push_back(std::move(w));
					continue;
				}
				const bool animal = figure_kind == "pes" || figure_kind == "zvire" || figure_kind == "strasidlo" || (figure_kind == "postava" && e.contains("prodavac"));
				if ((figure_kind == "postava" || figure_kind == "rostlina" || figure_kind == "predmet" || figure_kind == "strom" || animal) && e.contains("soubor")) {
					/* A figure: metres, the origin between its feet on the ground, facing +x. A plant ("rostlina", the
					 * colleague's hra/3d/PROSBA-PRUMYSL-3D.md) as one: its foot at the origin, 1 m high, by the name of
					 * its file in the lists of plants ("rostliny"). */
					ThreeDVehicleObject fig;
					fig.package = package;
					fig.file = folder + e.at("soubor").get<std::string>();
					NoteMarginsFilled(e, fig.file, MarginsOf::Vehicles);
					fig.name = e.value("jmeno", e.at("soubor").get<std::string>());
					fig.thing = figure_kind == "predmet";
					/* One pose of a file of several, each a node (the colleague's girls of the lorries, point 11): that node only. */
					if (e.contains("uzel") && e.at("uzel").is_string()) fig.only_node = e.at("uzel").get<std::string>();
					/* A figure of poses (the player, 18. 10.: "the figures modular, one figure object everywhere with what it
					 * has to do -- walk, smoke"; hra/3d/POSTAVY-MODULARNI-A-STADION.md): one file with bones ("kostra", else
					 * "soubor"), each pose ("pozy") an animation of it ("animace", else the pose's name), a figure of its own
					 * "<jmeno>/<poza>": of one frame it stands so (the animation's first key), of more ("snimku", each held
					 * "tiku" ticks) it goes round the animation ("smycka" false: once, staying on the last), walking "krok_m"
					 * a round. The figure by its name alone is its pose "stoji", else its first. Placed with "poza" beside
					 * "postava" (FigureNamed()). Its file is read when it is first drawn, not here. */
					/* A dog, an animal, a ghost, a seller of dogs: what it is, where it is found, how it takes the walker (ThreeDAnimal). */
					auto note_animal = [&](std::vector<std::string> poses) {
						if (!animal) return;
						ThreeDAnimal a;
						a.name = fig.name;
						a.kind = figure_kind == "pes" ? ThreeDAnimal::Kind::Dog : figure_kind == "zvire" ? ThreeDAnimal::Kind::Animal : figure_kind == "strasidlo" ? ThreeDAnimal::Kind::Ghost : ThreeDAnimal::Kind::Seller;
						const std::string temper = e.value("povaha", std::string());
						a.temper = temper == "utoci" ? ThreeDAnimal::Temper::Attacks : temper == "vyprovokovat" ? ThreeDAnimal::Temper::Provoked : ThreeDAnimal::Temper::Peaceful;
						auto place = [&](std::string_view where) {
							if (where == "ulice") a.places |= ThreeDAnimal::STREET;
							if (where == "dvere") a.places |= ThreeDAnimal::DOOR;
							if (where == "okoli") a.places |= ThreeDAnimal::ROUND;
							if (where == "strecha") a.places |= ThreeDAnimal::ROOF;
							if (where == "koleje") a.places |= ThreeDAnimal::RAIL;
						};
						if (e.contains("kde") && e.at("kde").is_string()) place(e.at("kde").get<std::string>());
						if (e.contains("kde") && e.at("kde").is_array()) {
							for (const auto &w : e.at("kde")) {
								if (w.is_string()) place(w.get<std::string>());
							}
						}
						const std::string climate = e.value("klima", std::string());
						a.climate = climate == "mirne" ? ThreeDAnimal::TEMPERATE : climate == "poust" ? ThreeDAnimal::DESERT : climate == "subarkticke" ? ThreeDAnimal::ARCTIC : 0;
						a.theme = ParseIndexTheme(e);
						a.night = e.value("kdy", std::string()) == "noc";
						a.day = e.value("kdy", std::string()) == "den";
						a.sold = e.value("prodej", false);
						a.flies = e.value("leta", false);
						a.price = std::clamp(e.value("cena", 0), 0, 10000000);
						if (e.contains("rozmer_m") && e.at("rozmer_m").is_array() && e.at("rozmer_m").size() == 3) {
							for (int k = 0; k < 3; k++) a.size[k] = std::max(0.05f, e.at("rozmer_m").at(k).get<float>());
						}
						a.scale = std::clamp(e.value("meritko", 2.0f), 0.1f, 20.0f);
						a.hover_m = std::clamp(e.value("vyska_m", 0.0f), 0.0f, 50.0f);
						/* In place of a night flyer of the indices (the player, 22. 10.: "the ghost in the bedsheet should not
						 * attack, it should float along the street like the walkers"): that one out of the sky. */
						if (e.contains("nahrazuje") && e.at("nahrazuje").is_string()) {
							const std::string replaced = e.at("nahrazuje").get<std::string>();
							_witches_replaced.insert(replaced);
							std::erase_if(_witches, [&](const ThreeDWitch &w) { return w.name == replaced; });
						}
						if (e.contains("nazev") && e.at("nazev").is_object()) {
							a.title_cs = e.at("nazev").value("cs", std::string());
							a.title_en = e.at("nazev").value("en", a.title_cs);
							if (a.title_cs.empty()) a.title_cs = a.title_en;
						}
						if (a.title_en.empty()) a.title_cs = a.title_en = a.name;
						a.poses = std::move(poses);
						_animals.push_back(std::move(a));
					};
					/* A ghost's poses of frames ("animace" by the pose, the colleague's mushroom warrior: "chuze" with
					 * "uzly" "chuze_"), as a creature's: a figure each, "<jmeno>/<poza>", the figure by its name alone
					 * its rest ("klid", else its first). */
					if (animal && !e.contains("pozy") && e.contains("animace") && e.at("animace").is_object() && !e.at("animace").empty() && e.at("animace").begin()->is_object()) {
						std::vector<std::string> names;
						for (const auto &[pose, a] : e.at("animace").items()) {
							if (!a.is_object()) continue;
							ThreeDVehicleObject one = fig;
							one.name = fig.name + "/" + pose;
							one.frame_prefix = a.value("uzly", pose + "_");
							one.frames = std::clamp(a.value("snimku", 0), 0, 64);
							one.frame_ticks = std::clamp(a.value("tiku", 1), 1, 1000);
							one.loop = a.value("smycka", true);
							one.walk_step = std::clamp(a.value("krok_m", 0.0f), 0.0f, 100.0f);
							if (one.frames < 2) one.frame_prefix.clear();
							_figure_objects.push_back(std::move(one));
							names.push_back(pose);
						}
						if (names.empty()) continue;
						const std::string rest = fig.name + "/" + (std::ranges::find(names, "klid") != names.end() ? std::string("klid") : names.front());
						for (size_t k = _figure_objects.size(); k-- > 0;) {
							if (_figure_objects[k].name != rest) continue;
							ThreeDVehicleObject one = _figure_objects[k];
							one.name = fig.name;
							_figure_objects.push_back(std::move(one));
							break;
						}
						note_animal(std::move(names));
						continue;
					}
					if ((figure_kind == "postava" || animal) && e.contains("pozy") && e.at("pozy").is_object() && !e.at("pozy").empty()) {
						const std::string bones = folder + e.value("kostra", e.at("soubor").get<std::string>());
						const auto &poses = e.at("pozy");
						auto posed = [&](const std::string &pose, const nlohmann::json &p, const std::string &name) {
							ThreeDVehicleObject one = fig;
							one.name = name;
							one.skeleton_file = bones;
							one.skeleton_anim = p.is_object() && p.contains("animace") && p.at("animace").is_string() ? p.at("animace").get<std::string>() : pose;
							one.frames = p.is_object() ? std::clamp(p.value("snimku", 1), 1, 64) : 1;
							one.frame_ticks = p.is_object() ? std::clamp(p.value("tiku", 1), 1, 1000) : 1;
							one.loop = p.is_object() ? p.value("smycka", true) : true;
							one.walk_step = p.is_object() ? std::clamp(p.value("krok_m", 0.0f), 0.0f, 100.0f) : 0.0f;
							/* (A figure of frames walks by them: the frames of the pose, though of bones, not of nodes.) */
							if (one.frames >= 2) one.frame_prefix = pose + "_";
							_figure_objects.push_back(std::move(one));
						};
						std::vector<std::string> names;
						for (const auto &[pose, p] : poses.items()) {
							posed(pose, p, fig.name + "/" + pose);
							names.push_back(pose);
						}
						const std::string first = poses.contains("stoji") ? std::string("stoji") : poses.contains("klid") ? std::string("klid") : poses.begin().key();
						posed(first, poses.at(first), fig.name);
						note_animal(std::move(names));
						continue;
					}
					note_animal({});
					if (fig.thing) {
						/* A thing's states, each a whole thing in a node of its own (the colleague's bow, "volno" and
						 * "napnuto"; the crossbow, "natazeno" and "vystreleno"): a figure each, "<jmeno>/<stav>", that node
						 * only. And its sounds ("zvuky": the string drawn, the shot), files beside it. */
						if (e.contains("stavy") && e.at("stavy").is_object()) {
							for (const auto &[state, about] : e.at("stavy").items()) {
								ThreeDVehicleObject one = fig;
								one.name = fig.name + "/" + state;
								one.only_node = state;
								_figure_objects.push_back(std::move(one));
							}
						}
						if (e.contains("zvuky") && e.at("zvuky").is_object()) {
							for (const auto &[sound, file] : e.at("zvuky").items()) {
								if (file.is_string()) _thing_sounds[fig.name + "/" + sound] = folder + file.get<std::string>();
							}
						}
					}
					if (e.value("druh", std::string()) == "rostlina") {
						fig.name = e.at("soubor").get<std::string>();
						if (fig.name.ends_with(".glb")) fig.name.resize(fig.name.size() - 4);
					}
					if (e.contains("animace") && e.at("animace").is_object()) {
						/* It moves: its frames are nodes of the file, drawn one at a time, a frame every so many ticks. */
						const auto &a = e.at("animace");
						fig.frame_prefix = a.value("uzly", std::string("snimek_"));
						fig.frames = std::clamp(a.value("snimku", 0), 0, 64);
						fig.frame_ticks = std::clamp(a.value("tiku", 1), 1, 1000);
						if (fig.frames < 2) fig.frame_prefix.clear();
						if (a.contains("chuze") && a.at("chuze").is_object()) fig.walk_step = std::clamp(a.at("chuze").value("krok_m", 0.0f), 0.0f, 10.0f);
					}
					/* A light (the colleague's torch and lantern): its glass ("noc") drawn with it, lit by its flame; and
					 * a weapon that shoots rockets, a figure of it shot (its loaded rocket, "raketa_v_hlavni", left out). */
					if (fig.thing && e.value("kategorie", std::string()) == "svetlo") {
						fig.night_glass = true;
						/* Its light as the index has it (the colleague's torch, 23. 10.: "the fire lights about, 10 m, it flickers"). */
						if (e.contains("svetlo") && e.at("svetlo").is_object()) {
							const auto &l = e.at("svetlo");
							auto n = l.contains("barva") && l.at("barva").is_string() ? ParseInteger<uint32_t>(l.at("barva").get<std::string>(), 16) : std::nullopt;
							if (n.has_value()) fig.light_colour = *n & 0xFFFFFF;
							fig.light_reach_m = std::clamp(l.value("dosah_m", 0.0f), 0.0f, 50.0f);
							fig.light_flickers = l.value("blika", false);
						}
					}
					/* A thing glowing at night (the colleague's glowing mushrooms, "kdy": "noc" with "noc"): its glass
					 * part of it, lit; and a mushroom to pick among the mushrooms, its glowing one beside it. */
					if (fig.thing && e.value("kdy", std::string()) == "noc" && e.contains("noc")) fig.night_glass = true;
					if (fig.thing && e.value("kategorie", std::string()) == "houba") {
						const bool glowing = e.value("kdy", std::string()) == "noc";
						std::string base = fig.name;
						if (glowing && base.ends_with("_svitici")) base.resize(base.size() - std::string_view("_svitici").size());
						auto it = std::ranges::find(_mushrooms, base, &ThreeDMushroom::name);
						if (it == _mushrooms.end()) {
							ThreeDMushroom m;
							m.name = base;
							_mushrooms.push_back(std::move(m));
							it = std::prev(_mushrooms.end());
						}
						if (glowing) {
							it->night = fig.name;
						} else {
							it->theme = ParseIndexTheme(e);
							it->height_m = std::clamp(e.value("vyska_m", 0.1f), 0.01f, 5.0f);
							if (e.contains("nazev") && e.at("nazev").is_object()) {
								it->title_cs = e.at("nazev").value("cs", std::string());
								it->title_en = e.at("nazev").value("en", it->title_cs);
							}
						}
					}
					if (fig.thing && e.value("strelba", std::string()) == "raketa") {
						ThreeDVehicleObject shot = fig;
						shot.name = fig.name + "/prazdny";
						shot.skip_node = "raketa_v_hlavni";
						_figure_objects.push_back(std::move(shot));
					}
					/* A light's flame (the colleague's torch and lantern, "plamen"): its frames, the rest of it in each. */
					if (fig.thing && e.contains("plamen") && e.at("plamen").is_object()) {
						const auto &a = e.at("plamen");
						fig.frame_prefix = a.value("uzly", std::string("plamen_"));
						fig.frames = std::clamp(a.value("snimku", 0), 0, 64);
						fig.frame_ticks = std::clamp(a.value("tiku", 1), 1, 1000);
						if (fig.frames < 2) fig.frame_prefix.clear();
					}
					/* A thing that turns (the colleague's hra/3d/PROSBA-HOSPODA.md, the spit: "we will turn it"): about an
					 * axis through a point, a turn in so many ticks, smoothly; and its frames ("stridani", the pig, the
					 * calf and the lamb) one after another, each for so many turns. */
					if (e.contains("otaceni") && e.at("otaceni").is_object()) {
						const auto &o = e.at("otaceni");
						fig.spin_ticks = std::clamp(o.value("tiku_na_otacku", 0), 0, 100000);
						if (o.contains("osa") && o.at("osa").is_array() && o.at("osa").size() == 3) {
							for (int k = 0; k < 3; k++) fig.spin_axis[k] = o.at("osa").at(k).get<float>();
						}
						if (o.contains("stred") && o.at("stred").is_array() && o.at("stred").size() == 3) {
							for (int k = 0; k < 3; k++) fig.spin_centre[k] = o.at("stred").at(k).get<float>();
						}
						const float len = std::sqrt(fig.spin_axis[0] * fig.spin_axis[0] + fig.spin_axis[1] * fig.spin_axis[1] + fig.spin_axis[2] * fig.spin_axis[2]);
						if (len < 1e-6f) {
							fig.spin_ticks = 0;
						} else {
							for (float &c : fig.spin_axis) c /= len;
						}
					}
					if (e.contains("stridani") && e.at("stridani").is_object()) {
						const auto &a = e.at("stridani");
						fig.frame_prefix = a.value("uzly", std::string("snimek_"));
						fig.frames = std::clamp(a.value("snimku", 0), 0, 64);
						fig.frame_ticks = std::clamp(std::max(1, fig.spin_ticks) * std::max(1, a.value("otocek", 1)), 1, 1000000);
						if (fig.frames < 2) fig.frame_prefix.clear();
					}
					/* A tree of a town theme (the clay town's mushrooms): among the trees the theme's towns have on their land;
					 * with its night ("noc": the colleague's mushrooms glowing under their caps) a figure "<jmeno>/noc" with it
					 * lit, drawn at night. */
					if (figure_kind == "strom" && e.contains("noc") && e.at("noc").is_object()) {
						ThreeDVehicleObject night = fig;
						night.name = fig.name + "/noc";
						night.night_glass = true;
						_figure_objects.push_back(std::move(night));
					}
					if (figure_kind == "strom") {
						const uint8_t theme = ParseIndexTheme(e);
						if (theme != 0) {
							auto &trees = _three_d_theme_trees[theme];
							if (std::ranges::find(trees, fig.name) == trees.end()) {
								trees.push_back(fig.name);
								const bool glows = e.contains("noc") && e.at("noc").is_object();
								_three_d_theme_trees_night[theme].push_back(glows ? fig.name + "/noc" : fig.name);
							}
						}
					}
					_figure_objects.push_back(std::move(fig));
					continue;
				}
				/* A vehicle of a set: "vozidlo", or "auto" (the colleague's Tatras, hra/3d/PROSBA-TATRA-613-CIVILNI.md) with
				 * the set's id; an "auto" without one is a car of the game's own that drives about by itself (later). */
				const std::string kind = e.value("druh", std::string("dum"));
				const bool driving = kind == "auto" && !e.contains("grf") && e.contains("jizda") && e.at("jizda").is_object();
				if (kind != "vozidlo" && !(kind == "auto" && e.contains("grf")) && !driving) continue;
				ThreeDVehicleObject obj;
				obj.package = package;
				obj.name = e.value("jmeno", e.at("soubor").get<std::string>());
				obj.file = folder + e.at("soubor").get<std::string>();
				NoteMarginsFilled(e, obj.file, MarginsOf::Vehicles);
				obj.type = ParseIndexVehicleType(e.value("typ", std::string(kind == "auto" ? "silnice" : "vlak")));
				auto grfid = ParseIndexGrfId(e.value("grf", std::string()));
				if (obj.type == VehicleType::Invalid || (!grfid.has_value() && !driving)) {
					Debug(misc, 0, "3D vehicle {}: no type or set", obj.name);
					continue;
				}
				if (driving) {
					/* A car of the game's own, no set's: it drives by itself between the houses named, stop to stop. */
					const auto &j = e.at("jizda");
					if (j.value("k", std::string()) != "dum" || !j.contains("nahrazuje_house")) continue;
					obj.drive_house = static_cast<HouseID>(j.at("nahrazuje_house").get<int>());
					obj.drive_kmh = std::clamp(j.value("rychlost_kmh", 60.0f), 5.0f, 200.0f);
					obj.drive_stand = std::clamp(j.value("stoji_tiku", 400), 0, 100000);
					obj.pieces.push_back(0);
				} else {
					obj.grfid = *grfid;
				}
				if (e.contains("clanky")) {
					for (const auto &piece : e.at("clanky")) {
						auto n = ParseIndexNumber(piece);
						if (n.has_value()) obj.pieces.push_back(static_cast<uint16_t>(*n));
					}
				}
				if (!driving && obj.pieces.empty() && e.contains("motor")) {
					auto n = ParseIndexNumber(e.at("motor"));
					if (n.has_value()) obj.pieces.push_back(static_cast<uint16_t>(*n));
				}
				if (obj.pieces.empty()) {
					Debug(misc, 0, "3D vehicle {}: no engine", obj.name);
					continue;
				}
				/* The lengths of its pieces in its set its object's length is for (the colleague's lorries: the snout 4 and
				 * the lorry 8, a trailer 8 and its bumper 4): its metres to a length of its set, the set saying how long
				 * each piece is now (ThreeDVehicleObjectPieceLengthOf()). */
				if (e.contains("delky_clanku_grf") && e.at("delky_clanku_grf").is_array() && e.at("delky_clanku_grf").size() == obj.pieces.size()) {
					for (const auto &l : e.at("delky_clanku_grf")) obj.grf_lengths.push_back(l.is_number_integer() ? std::clamp(l.get<int>(), 1, static_cast<int>(VEHICLE_LENGTH)) : static_cast<int>(VEHICLE_LENGTH));
				}
				if (e.contains("rozmer_m") && e.at("rozmer_m").is_object()) obj.length_m = std::clamp(e.at("rozmer_m").value("delka", 0.0f), 0.0f, 200.0f);
				/* The piece it hangs from: the middle one, the first, or the one named. The middle of an even count of
				 * pieces is the one the middle of its length is in (a lorry: its snout 4, itself 8 -- the lorry). */
				std::string anchor = e.contains("kotva") && e.at("kotva").is_string() ? e.at("kotva").get<std::string>() : "stred";
				obj.anchor = anchor == "prvni" || anchor == "predni" ? 0 : (static_cast<int>(obj.pieces.size()) - 1) / 2;
				if (anchor == "stred" && !obj.grf_lengths.empty()) {
					int total = 0, at = 0;
					for (int l : obj.grf_lengths) total += l;
					for (size_t k = 0; k < obj.grf_lengths.size(); k++) {
						if (2 * (at + obj.grf_lengths[k]) > total) {
							obj.anchor = static_cast<int>(k);
							break;
						}
						at += obj.grf_lengths[k];
					}
				}
				if (e.contains("kotva") && e.at("kotva").is_number_integer()) obj.anchor = std::clamp(e.at("kotva").get<int>(), 0, static_cast<int>(obj.pieces.size()) - 1);
				/* Its couplings: where the one after it hangs, where it hangs from the one before. */
				if (e.contains("spojeni") && e.at("spojeni").is_object()) {
					auto point = [&](const char *key, bool &has, float *at) {
						const auto &s = e.at("spojeni");
						if (!s.contains(key) || !s.at(key).is_array() || s.at(key).size() != 3) return;
						for (int k = 0; k < 3; k++) at[k] = s.at(key).at(k).get<float>();
						has = true;
					};
					point("vpredu", obj.has_couple_front, obj.couple_front);
					point("vzadu", obj.has_couple_back, obj.couple_back);
				}
				if (e.contains("maska_podrobnosti") && e.at("maska_podrobnosti").is_string()) obj.detail_file = folder + e.at("maska_podrobnosti").get<std::string>();
				/* No "meritko": the object is in true metres, and the size it is drawn at is its set's parameter
				 * ("velikost") or else the game's setting (the player, 14. 10.: "the game can make it bigger, so there
				 * need not be two files"). */
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
				/* The crew: figures riding in it, never fading; bigger than they are by "meritko" (the girls on a lorry's back). */
				auto read_crew = [](const nlohmann::json &list, std::vector<ThreeDVehicleCrew> &out) {
					if (!list.is_array()) return;
					for (const auto &c : list) {
						if (!c.is_object() || !c.contains("postava") || !c.contains("poloha")) continue;
						ThreeDVehicleCrew crew;
						crew.figure = FigureNamed(c);
						const auto &at = c.at("poloha");
						if (!at.is_array() || at.size() != 3) continue;
						for (int k = 0; k < 3; k++) crew.at[k] = at.at(k).get<float>();
						crew.turn = c.value("otoceni", 0.0f);
						crew.scale = std::clamp(c.value("meritko", 1.0f), 0.1f, 10.0f);
						crew.driver_seat = c.value("misto_ridicky", false);
						out.push_back(std::move(crew));
					}
				};
				if (e.contains("posadka")) read_crew(e.at("posadka"), obj.crew);
				/* The parts a state shows (the colleague's hra/3d/PROSBA-V3S-3D.md): each node of each add-on, then each
				 * group of the crew; part 0 is what is always drawn. */
				obj.part_names.assign(1, std::string());
				if (e.contains("doplnky") && e.at("doplnky").is_array()) {
					for (const auto &a : e.at("doplnky")) {
						if (!a.is_object() || !a.contains("soubor") || !a.at("soubor").is_string()) continue;
						ThreeDVehicleAddon addon;
						addon.key = a.at("soubor").get<std::string>();
						addon.file = folder + addon.key;
						/* The add-ons' files are the vehicle's record's, its "okraje_textur" theirs too (or their own). */
						NoteMarginsFilled(e, addon.file, MarginsOf::Vehicles);
						NoteMarginsFilled(a, addon.file, MarginsOf::Vehicles);
						if (a.contains("poloha") && a.at("poloha").is_array() && a.at("poloha").size() == 3) {
							for (int k = 0; k < 3; k++) addon.at[k] = a.at("poloha").at(k).get<float>();
						}
						if (a.contains("uzly") && a.at("uzly").is_array()) {
							for (const auto &n : a.at("uzly")) {
								if (n.is_string()) addon.nodes.push_back(n.get<std::string>());
							}
						}
						if (a.contains("barva") && a.at("barva").is_object()) {
							const auto &b = a.at("barva");
							if (b.contains("maska") && b.at("maska").is_string()) addon.mask_file = folder + b.at("maska").get<std::string>();
							addon.start = std::clamp(b.value("zacatek", 0), 0, 255);
						}
						if (a.contains("maska_podrobnosti") && a.at("maska_podrobnosti").is_string()) addon.detail_file = folder + a.at("maska_podrobnosti").get<std::string>();
						for (const std::string &n : addon.nodes) obj.part_names.push_back(addon.key + "/" + n);
						obj.addons.push_back(std::move(addon));
					}
				}
				if (e.contains("posadky") && e.at("posadky").is_object()) {
					for (const auto &[group, list] : e.at("posadky").items()) {
						std::vector<ThreeDVehicleCrew> crew;
						read_crew(list, crew);
						obj.part_names.push_back("posadky/" + group);
						obj.crew_groups.emplace_back(group, std::move(crew));
					}
				}
				/* The record's own crew a part too: drawn in every state but one standing empty (EmptyStateOf()). */
				if (!obj.crew.empty()) obj.part_names.push_back("posadka");
				if (obj.part_names.size() > ThreeDParts::COUNT) {
					Debug(misc, 0, "3D vehicle {}: {} parts shown by the states, only {} can be", obj.name, obj.part_names.size() - 1, ThreeDParts::COUNT - 1);
					obj.part_names.resize(ThreeDParts::COUNT);
				}
				auto part_of = [&](const std::string &name) {
					auto it = std::ranges::find(obj.part_names, name);
					return it == obj.part_names.end() || it == obj.part_names.begin() ? 0 : static_cast<int>(it - obj.part_names.begin());
				};
				/* The groups of the crew one of which sits at the driver's seat (the colleague's hra/3d/TATRY-V3-PRO-HRU.md:
				 * Mat driving the fire Tatra): where a state shows one, the record's driver is not drawn (PartsOf()). */
				for (const auto &[group, list] : obj.crew_groups) {
					const int part = part_of("posadky/" + group);
					if (part > 0 && std::ranges::any_of(list, [](const ThreeDVehicleCrew &c) { return c.driver_seat; })) obj.seat_groups.Set(part);
				}
				obj.crew_record_part = part_of("posadka");
				/* The wheels of its own model that turn as it goes, the front ones steering. */
				if (e.contains("kola_v_modelu") && e.at("kola_v_modelu").is_array()) {
					for (const auto &k : e.at("kola_v_modelu")) {
						if (!k.is_object() || !k.contains("uzel") || !k.contains("stred") || !k.at("stred").is_array() || k.at("stred").size() != 3) continue;
						ThreeDModelWheel w;
						w.node = k.at("uzel").get<std::string>();
						if (k.contains("soubor") && k.at("soubor").is_string()) w.addon = k.at("soubor").get<std::string>();
						for (int a = 0; a < 3; a++) w.centre[a] = k.at("stred").at(a).get<float>();
						w.radius = std::clamp(k.value("polomer_m", 0.5f), 0.05f, 5.0f);
						w.steers = k.value("rizeni", false);
						w.max_angle = std::clamp(k.value("max_uhel", 35.0f), 0.0f, 80.0f);
						obj.model_wheels.push_back(std::move(w));
						if (obj.model_wheels.size() >= 254) break;
					}
				}
				/* Its windows: see-through. */
				if (e.contains("sklo") && e.at("sklo").is_object()) {
					obj.glass_node = e.at("sklo").value("uzel", std::string());
					obj.glass_alpha = std::clamp(e.at("sklo").value("alfa", 0.15f), 0.0f, 1.0f);
				}
				/* The states its GRF picks by its sprites ("stavy"): a colour of the paint, how far it has faded and rusted. */
				if (e.contains("stavy") && e.at("stavy").is_array()) {
					for (const auto &st : e.at("stavy")) {
						if (!st.is_object() || !st.contains("sprity") || !st.at("sprity").is_array() || st.at("sprity").size() != 2) continue;
						auto first = ParseIndexNumber(st.at("sprity").at(0)), last = ParseIndexNumber(st.at("sprity").at(1));
						if (!first.has_value() || !last.has_value() || *last < *first) continue;
						ThreeDVehicleState state;
						state.first = *first;
						state.last = *last;
						if (st.contains("barva") && st.at("barva").is_string()) {
							std::string t = st.at("barva").get<std::string>();
							if (t.starts_with("#")) t = t.substr(1);
							auto n = t.size() == 6 ? ParseInteger<uint32_t>(t, 16) : std::nullopt;
							if (n.has_value()) state.colour = Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data;
							state.given |= ThreeDVehicleState::GIVEN_COLOUR;
						}
						state.fade = std::clamp(st.value("lak", 0.0f), 0.0f, 1.0f);
						state.rust = std::clamp(st.value("rez", 0.0f), 0.0f, 1.0f);
						if (st.contains("lak")) state.given |= ThreeDVehicleState::GIVEN_FADE;
						if (st.contains("rez")) state.given |= ThreeDVehicleState::GIVEN_RUST;
						/* Its layer of the GRF's sprites; the whole object turned. */
						state.layer = std::clamp(st.value("vrstva", 0), 0, 255);
						/* The piece whose sets it is of (the colleague's wagons: "the middle's sets decide"). */
						if (st.contains("clanek") && st.at("clanek").is_string()) {
							const std::string piece = st.at("clanek").get<std::string>();
							state.piece = piece == "hlava" ? 0 : piece == "stred" ? 1 : piece == "zad" ? 2 : -1;
							obj.states_of_anchor = true;
						}
						if (st.contains("otoceni") && st.at("otoceni").is_number()) {
							state.turn = static_cast<float>(std::remainder(st.at("otoceni").get<double>(), 360.0));
							state.given |= ThreeDVehicleState::GIVEN_TURN;
						}
						/* The node of each add-on it shows, the colour each takes, the group of the crew it shows. */
						if (st.contains("doplnky") && st.at("doplnky").is_object()) {
							state.addon_parts.assign(obj.addons.size(), -1);
							for (const auto &[file, node] : st.at("doplnky").items()) {
								const int part = node.is_string() ? part_of(file + "/" + node.get<std::string>()) : 0;
								if (part > 0) state.parts.Set(part);
								for (size_t a = 0; a < obj.addons.size(); a++) {
									if (obj.addons[a].key == file) state.addon_parts[a] = static_cast<int16_t>(part);
								}
							}
						}
						if (st.contains("barvy_doplnku") && st.at("barvy_doplnku").is_object()) {
							state.addon_colours.assign(obj.addons.size(), 0);
							for (const auto &[file, value] : st.at("barvy_doplnku").items()) {
								if (!value.is_string()) continue;
								std::string c = value.get<std::string>();
								if (c.starts_with("#")) c = c.substr(1);
								auto n = c.size() == 6 ? ParseInteger<uint32_t>(c, 16) : std::nullopt;
								for (size_t a = 0; a < obj.addons.size(); a++) {
									if (n.has_value() && obj.addons[a].key == file) state.addon_colours[a] = Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data;
								}
							}
						}
						if (st.contains("sada") && st.at("sada").is_string()) state.set_name = st.at("sada").get<std::string>();
						if (st.contains("posadka") && st.at("posadka").is_string()) {
							const int part = part_of("posadky/" + st.at("posadka").get<std::string>());
							if (part > 0) state.parts.Set(part);
							state.crew_part = part;
						}
						obj.states.push_back(state);
						if (obj.states.size() >= 255) break;
					}
				}
				/* The rust: a second mask over the body's texture, rusting with age after the paint has faded. */
				if (e.contains("rez") && e.at("rez").is_object()) {
					const auto &rez = e.at("rez");
					if (rez.contains("maska") && rez.at("maska").is_string()) obj.rust.mask_file = folder + rez.at("maska").get<std::string>();
					if (rez.contains("cil") && rez.at("cil").is_string()) {
						std::string t = rez.at("cil").get<std::string>();
						if (t.starts_with("#")) t = t.substr(1);
						auto n = t.size() == 6 ? ParseInteger<uint32_t>(t, 16) : std::nullopt;
						if (n.has_value()) obj.rust.target = Colour(static_cast<uint8_t>(*n >> 16), static_cast<uint8_t>(*n >> 8), static_cast<uint8_t>(*n)).data;
					}
					if (rez.contains("postupne") && rez.at("postupne").is_object()) {
						obj.rust.gradual = true;
						obj.rust.ramp = std::clamp(rez.at("postupne").value("nabeh", 0.25f), 0.01f, 1.0f);
					}
					if (obj.rust.target == 0) obj.rust.mask_file.clear();
				}
				/* The size as a parameter of its set: each value the scale it draws at; else the game's setting. */
				if (e.contains("velikost") && e.at("velikost").is_object()) {
					const auto &size = e.at("velikost");
					auto param = size.contains("parametr") ? ParseIndexNumber(size.at("parametr")) : std::nullopt;
					if (param.has_value() && *param < 0x80 && size.contains("hodnoty") && size.at("hodnoty").is_object()) {
						for (const auto &[value, scale] : size.at("hodnoty").items()) {
							auto v = ParseInteger<uint32_t>(value, 10);
							if (v.has_value() && scale.is_number() && scale.get<float>() > 0.1f) obj.size.values.emplace_back(*v, std::clamp(scale.get<float>(), 0.25f, 4.0f));
						}
						if (!obj.size.values.empty()) obj.size.param = static_cast<int>(*param);
					}
				}
				/* Wheels from a file of their own: one wheel at every place, mirrored to the other side. */
				if (e.contains("kola") && e.at("kola").is_array()) {
					for (const auto &k : e.at("kola")) {
						if (!k.is_object() || !k.contains("soubor") || !k.contains("polohy")) continue;
						ThreeDVehicleWheels w;
						w.file = folder + k.at("soubor").get<std::string>();
						NoteMarginsFilled(e, w.file, MarginsOf::Vehicles);
						w.both_sides = k.value("oba_boky", true);
						for (const auto &p : k.at("polohy")) {
							if (!p.is_array() || p.size() != 3) continue;
							w.at.push_back({p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>()});
						}
						if (!w.at.empty()) obj.wheels.push_back(std::move(w));
					}
				}
				if (driving) {
					_driving_objects.push_back(std::move(obj));
				} else {
					_vehicle_objects.push_back(std::move(obj));
				}
			}
		} catch (const std::exception &ex) {
			Debug(misc, 0, "3D vehicles {}: {}", index, ex.what());
		}
	}
	/* The engines of MSTS the player put in newgrf/simtrains/ (simtrains.h): each the one engine of the GRF made of
	 * it, drawn from its model. */
	for (const SimtrainsEngine &e : FindSimtrainsEngines()) {
		ThreeDVehicleObject obj;
		obj.name = e.name;
		obj.package = e.folder;
		obj.file = e.shape;
		obj.type = VehicleType::Train;
		obj.grfid = e.grfid;
		/* A long one is articulated (ReadSize()): its parts are pieces, the model hung from the middle one. */
		for (int k = 0; k < e.parts; k++) obj.pieces.push_back(static_cast<uint16_t>(SIMTRAINS_ENGINE_ID + k));
		obj.anchor = (e.parts - 1) / 2;
		if (e.has_cab) {
			std::copy(std::begin(e.cab), std::end(e.cab), obj.cab);
			obj.has_cab = true;
		}
		_vehicle_objects.push_back(std::move(obj));
	}
	/* The cars of the mods of BeamNG (beamng.h): each the one road vehicle of the GRF made of it, drawn from its model,
	 * one of its mod's paints each car (the player, 16. 10.: "the colour of the mod"). */
	for (const BeamngVehicle &b : FindBeamngVehicles()) {
		ThreeDVehicleObject obj;
		obj.name = b.name;
		obj.package = b.Package();
		obj.file = "beamng:" + b.Package() + "|" + b.folder;
		obj.type = VehicleType::Road;
		obj.grfid = b.grfid;
		obj.pieces.push_back(BEAMNG_ENGINE_ID);
		for (uint32_t colour : b.paints) {
			ThreeDVehicleState st;
			st.colour = colour;
			obj.states.push_back(st);
		}
		obj.paint_random = true;
		if (b.has_cab) {
			std::copy(std::begin(b.cab), std::end(b.cab), obj.cab);
			obj.has_cab = true;
		}
		_vehicle_objects.push_back(std::move(obj));
	}
	/* A piece is the first object's that has it -- unless another package of the same set lies beside the GRF loaded. */
	for (size_t i = 0; i < _vehicle_objects.size(); i++) {
		const ThreeDVehicleObject &o = _vehicle_objects[i];
		const bool here = PackageBesideLoadedGrf(o.package, o.grfid);
		for (size_t k = 0; k < o.pieces.size(); k++) {
			auto [it, added] = _vehicle_object_pieces.emplace(VehiclePieceKey(o.grfid, o.type, o.pieces[k]), std::make_pair(static_cast<uint32_t>(i), static_cast<int>(k)));
			/* A piece of several objects of the same package: all of them, the pieces around it to tell which. */
			if (!added && _vehicle_objects[it->second.first].package == o.package) {
				auto &all = _vehicle_object_shared_pieces[it->first];
				if (all.empty()) all.push_back(it->second);
				all.emplace_back(static_cast<uint32_t>(i), static_cast<int>(k));
				continue;
			}
			if (added || !here || PackageBesideLoadedGrf(_vehicle_objects[it->second.first].package, o.grfid)) continue;
			Debug(misc, 1, "3D vehicles: {} of {} beside its loaded GRF, not of {}", o.name, o.package, _vehicle_objects[it->second.first].package);
			it->second = std::make_pair(static_cast<uint32_t>(i), static_cast<int>(k));
		}
	}
	Debug(misc, 1, "3D vehicles: {} in the indices, {} figures", _vehicle_objects.size(), _figure_objects.size());
}

/**
 * The size of a set's vehicle objects as its parameter ("velikost"), the first of its objects that has one.
 * @param grfid the set's id
 * @return nullptr when none of its objects has it
 */
const ThreeDVehicleSize *GetThreeDVehicleSize(uint32_t grfid)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	const ThreeDVehicleSize *first = nullptr;
	for (const ThreeDVehicleObject &o : _vehicle_objects) {
		if (o.grfid != grfid || o.size.param < 0) continue;
		/* Of the package beside the GRF loaded, when there are more (an old and a new version). */
		if (PackageBesideLoadedGrf(o.package, grfid)) return &o.size;
		if (first == nullptr) first = &o.size;
	}
	return first;
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

/** The key of a vehicle's piece in _vehicle_object_pieces, 0 for none of a set. */
static uint64_t PieceKeyOf(const Vehicle *v)
{
	const Engine *e = v != nullptr ? Engine::GetIfValid(v->engine_type) : nullptr;
	if (e == nullptr || e->grf_prop.grffile == nullptr) return 0;
	return VehiclePieceKey(e->grf_prop.grfid, e->type, e->grf_prop.local_id);
}

/**
 * The object a piece of a vehicle on the map is of, and which of its pieces it is: as its engine says
 * (GetThreeDVehicleObject()) -- unless the engine is a piece of several objects (the colleague's lorries: the ends of
 * the trains of lorries behind each of five semi-trailers, PROSBA-NAKLADAKY-TATRA-3D.md point 3: "take them to the
 * semi-trailer that goes before them"), and then the one whose pieces the pieces before and after it are.
 * @param v the piece, a part of an articulated vehicle or a wagon
 * @param[out] piece which of the object's pieces it is
 */
const ThreeDVehicleObject *ThreeDVehicleObjectOf(const Vehicle *v, int *piece)
{
	if (v == nullptr) return nullptr;
	const ThreeDVehicleObject *obj = GetThreeDVehicleObject(v->engine_type, piece);
	if (obj == nullptr || _vehicle_object_shared_pieces.empty()) return obj;
	auto all = _vehicle_object_shared_pieces.find(PieceKeyOf(v));
	if (all == _vehicle_object_shared_pieces.end()) return obj;
	/* The one whose pieces before and after match the vehicle's the furthest. */
	int best_score = -1;
	for (const auto &[index, k] : all->second) {
		const ThreeDVehicleObject &o = _vehicle_objects[index];
		int score = 0;
		const Vehicle *u = v;
		for (int b = k - 1; b >= 0; b--) {
			u = u->Previous();
			if (u == nullptr || PieceKeyOf(u) != VehiclePieceKey(o.grfid, o.type, o.pieces[b])) break;
			score++;
		}
		u = v;
		for (size_t f = k + 1; f < o.pieces.size(); f++) {
			u = u->Next();
			if (u == nullptr || PieceKeyOf(u) != VehiclePieceKey(o.grfid, o.type, o.pieces[f])) break;
			score++;
		}
		if (score > best_score) {
			best_score = score;
			obj = &o;
			if (piece != nullptr) *piece = k;
		}
	}
	return obj;
}

/**
 * Forget the vehicles and figures read from the indices, to read them again the next time one is looked for: the
 * GRFs are loaded anew (GfxInitSpriteMem()), and the packages beside them may have come or gone since -- read the
 * first time early, before the game had looked through the tars of the NewGRF folders, they had none.
 */
void ResetThreeDVehicleObjects()
{
	_vehicle_objects.clear();
	_figure_objects.clear();
	_effect_objects.clear();
	_driving_objects.clear();
	_vehicle_object_pieces.clear();
	_vehicle_object_shared_pieces.clear();
	_three_d_theme_trees.clear();
	_three_d_theme_trees_night.clear();
	_creatures.clear();
	_witches.clear();
	_witches_replaced.clear();
	_animals.clear();
	_mushrooms.clear();
	_thing_sounds.clear();
	_thing_places.clear();
	_vehicle_objects_read = false;
}

/**
 * A vehicle of the packages by its record's name ("jmeno": a building's event names the fire Tatra so), nullptr none:
 * of two packages of the same set (v2 and v3 of the Tatras in the folder) the one beside its GRF loaded, as the
 * engines take it.
 */
const ThreeDVehicleObject *GetThreeDVehicleObjectNamed(std::string_view name)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	const ThreeDVehicleObject *first = nullptr;
	for (const ThreeDVehicleObject &o : _vehicle_objects) {
		if (o.name != name) continue;
		if (PackageBesideLoadedGrf(o.package, o.grfid)) return &o;
		if (first == nullptr) first = &o;
	}
	return first;
}

/** The cars of the game's own that drive about by themselves (three_d_walkers.h). */
const std::vector<ThreeDVehicleObject> &GetThreeDDrivingCars()
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	return _driving_objects;
}

/** An effect of the indices by its name ("druh": "efekt"), or nullptr. */
const ThreeDVehicleObject *GetThreeDEffect(std::string_view name)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	for (const ThreeDVehicleObject &f : _effect_objects) {
		if (f.name == name) return &f;
	}
	return nullptr;
}

/** A figure of the indices by its name ("druh": "postava"), or nullptr; one of the package given first (a vehicle's crew). */
/** The passages of the creatures from one animation to the next, made as they are asked for (GetThreeDFigure()). */
static std::map<std::string, std::unique_ptr<ThreeDVehicleObject>, std::less<>> _passages;

/** How many ticks a creature passes from one animation to the next (GetThreeDFigure()). */
static constexpr int CREATURE_BLEND_TICKS = 8;

/**
 * A place of a thing of the indices: an empty node of its file (the colleague's things: "uchyt" where the hand holds
 * it, "zarez_napnuto" where the arrow's nock sits on the string drawn), metres in the thing's frame.
 * @param thing the thing ("jmeno", or "<jmeno>/<stav>": the same file)
 * @param node the node
 * @return where, or nothing when the thing or the node is not there
 */
std::optional<std::array<float, 3>> GetThreeDThingPlace(std::string_view thing, std::string_view node)
{
	const std::string key = fmt::format("{}/{}", thing, node);
	auto known = _thing_places.find(key);
	if (known != _thing_places.end()) return known->second;
	std::optional<std::array<float, 3>> found;
	const ThreeDVehicleObject *fig = GetThreeDFigure(thing);
	if (fig != nullptr) {
		GlbMesh mesh;
		std::vector<float> points;
		GlbReadOptions o;
		o.all_nodes = true;
		o.only_node = fig->only_node.empty() ? std::string_view(node) : std::string_view(fig->only_node);
		o.point_prefix = node;
		o.points = &points;
		ReadGlbMesh(fig->file, mesh, o);
		if (points.size() >= 3) found = std::array<float, 3>{points[0], points[1], points[2]};
	}
	_thing_places.emplace(key, found);
	return found;
}

/**
 * A sound of a thing of the indices ("zvuky": the bow's string drawn, "napnuti", and its shot, "vystrel"), read once
 * (GetGameOwnSound()).
 * @return the sound, or nothing when the thing has none so named or its file cannot be read
 */
std::optional<SoundID> GetThreeDThingSound(std::string_view thing, std::string_view sound)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	auto it = _thing_sounds.find(fmt::format("{}/{}", thing, sound));
	if (it == _thing_sounds.end()) return std::nullopt;
	const std::string &path = it->second;
	if (path.starts_with(IN_TAR)) return GetGameOwnSound(path.substr(IN_TAR.size()), Subdirectory::NewGrf);
	if (path.starts_with(IN_BASESET_TAR)) return GetGameOwnSound(path.substr(IN_BASESET_TAR.size()), Subdirectory::Baseset);
	return GetGameOwnSound(path, Subdirectory::None);
}

const ThreeDVehicleObject *GetThreeDFigure(std::string_view name, std::string_view package)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	/* A creature's passage from one animation to another: "<jmeno>/<from>@<frame>><to>", made the first time. */
	const size_t at = name.find('@'), to_at = name.find('>');
	if (at != std::string_view::npos && to_at != std::string_view::npos && at < to_at) {
		auto it = _passages.find(name);
		if (it != _passages.end()) return it->second.get();
		const size_t slash = name.find('/');
		const auto frame = ParseInteger<int>(name.substr(at + 1, to_at - at - 1), 10);
		if (slash == std::string_view::npos || slash > at || !frame.has_value()) return nullptr;
		const ThreeDVehicleObject *from = GetThreeDFigure(name.substr(0, at), package);
		const ThreeDVehicleObject *to = GetThreeDFigure(std::string(name.substr(0, slash + 1)) + std::string(name.substr(to_at + 1)), package);
		if (from == nullptr || to == nullptr || from->skeleton_file.empty() || from->skeleton_file != to->skeleton_file) return to;
		auto passage = std::make_unique<ThreeDVehicleObject>();
		passage->name = std::string(name);
		passage->package = to->package;
		passage->file = to->file;
		passage->paint_file = to->paint_file;
		passage->type = to->type;
		passage->skeleton_file = to->skeleton_file;
		passage->skeleton_anim = to->skeleton_anim;
		passage->blend_from = from;
		passage->blend_to = to;
		passage->blend_from_frame = std::clamp(*frame, 0, std::max(0, from->frames - 1));
		passage->blend_ticks = CREATURE_BLEND_TICKS;
		passage->frames = std::max(1, to->frames) * CREATURE_BLEND_TICKS;
		passage->frame_ticks = 1;
		passage->loop = to->loop;
		passage->walk_step = to->walk_step;
		return _passages.emplace(std::string(name), std::move(passage)).first->second.get();
	}
	/* A vehicle's crew from its own package first (the package of a GRF carries its driver). */
	if (!package.empty()) {
		for (const ThreeDVehicleObject &f : _figure_objects) {
			if (f.name == name && f.package == package) return &f;
		}
	}
	for (const ThreeDVehicleObject &f : _figure_objects) {
		if (f.name == name) return &f;
	}
	return nullptr;
}

/*
 * What is made of a vehicle object the first time it is drawn -- its shape read, its textures and its masks decoded,
 * its simpler shapes, its crew that walks -- is made on two threads: the main one (the view from the eyes, the
 * sprites, the previews) and the one making the pictures of the vehicles of the map in the background
 * (three_d_vehicle_object.cpp; the player, 17. 10.: "put a thread in the background"). MakeOnce(): under one lock
 * whether a thing is made; if not, the first thread to ask makes it outside the lock (a texture of sixteen million
 * pixels takes half a second) and stores it under the lock, the other waits for it. Nothing made is replaced while
 * a thread may hold it: what is forgotten (ThreeDVehicleObject::Forget()) is forgotten only while no picture is
 * being made, and the objects are read anew only after that thread is stopped (ResetThreeDVehicleObjectSprites()).
 */
static std::mutex _lazy_mutex;
static std::condition_variable _lazy_made;

/** The next number of a shape or a texture (ThreeDUniqueId): never 0, never one given before. */
uint64_t ThreeDUniqueId::Next()
{
	static std::atomic<uint64_t> next{1};
	return next++;
}
/** The things being made now: the object (or the shape) and which thing of it (LazyThing). */
static std::set<std::pair<const void *, int>> _lazy_making;
/** The things asked of the thread in the background and not made yet (MadeOrAsked()). */
static std::set<std::pair<const void *, int>> _lazy_asked;
/** Which thing of an object is made (MakeOnce()): of a vehicle object, of a shape its simpler ones, of a building its textures and shapes. */
enum LazyThing : int {
	LAZY_SHAPE = 0,
	LAZY_WALKING = 1,
	LAZY_PAINT_MASK = 2,
	LAZY_RUST_MASK = 3,
	LAZY_HOUSE_LEVELS = 4,   ///< a building's texture at all its levels
	LAZY_HOUSE_GLB = 5,      ///< a house's shape and texture from its .glb (ThreeDObject::Read())
	LAZY_FRAME = 1000,       ///< + the frame
	LAZY_TEXTURE = 2000,     ///< + the texture's index
	LAZY_ADDON_MASK = 3000,  ///< + the add-on
	LAZY_DETAIL_MASK = 4000, ///< + the add-on + 1
	LAZY_SIMPLER = 5000,     ///< + the level
	LAZY_HOUSE_SHAPE = 6000, ///< + the level
	LAZY_HOUSE_MASK = 7000,  ///< + the part painted a colour
	LAZY_HOUSE_PAINT = 1 << 20, ///< + the level + 16 times the paint (its halves mixed into sixteen bits)
};

/**
 * Make a thing of an object once, on whichever thread asks for it first; the other waits for it.
 * @param of the object (or the shape) it is of
 * @param thing which of its things (LazyThing)
 * @param made whether it is there: looked at under the lock, so it must not ask for another thing
 * @param make make it: outside the lock, so it may ask for the others (the shape of a crew's figure)
 * @param store keep what was made (even nothing, when it could not be made): under the lock, as made()
 */
template <typename Made, typename Make, typename Store>
static void MakeOnce(const void *of, int thing, Made &&made, Make &&make, Store &&store)
{
	const std::pair<const void *, int> key(of, thing);
	std::unique_lock lock(_lazy_mutex);
	for (;;) {
		if (made()) return;
		if (_lazy_making.insert(key).second) break;
		/* Another thread is making it: wait for it. */
		_lazy_made.wait(lock);
	}
	lock.unlock();
	try {
		auto result = make();
		lock.lock();
		store(std::move(result));
	} catch (...) {
		if (!lock.owns_lock()) lock.lock();
		_lazy_making.erase(key);
		_lazy_asked.erase(key);
		_lazy_made.notify_all();
		throw;
	}
	_lazy_making.erase(key);
	_lazy_asked.erase(key);
	_lazy_made.notify_all();
}

/**
 * Whether a thing of an object is made; if not, it is asked of the thread in the background (once, ThreeDLoadInBackground()),
 * to be there a frame or two on -- the view from the eyes draws without it meanwhile (the Try... getters).
 * @param made whether it is there: under the lock, as MakeOnce()'s
 * @param make the job that makes it (a getter that does)
 */
template <typename Made>
static bool MadeOrAsked(const void *of, int thing, Made &&made, std::function<void()> make)
{
	const std::pair<const void *, int> key(of, thing);
	{
		std::lock_guard lock(_lazy_mutex);
		if (made()) return true;
		if (!_lazy_asked.insert(key).second) return false; // asked already
	}
	ThreeDLoadInBackground(std::move(make));
	if (!_three_d_load_at_once) return false;
	/* The rig: made now, so it is there. */
	std::lock_guard lock(_lazy_mutex);
	return made();
}

/** The shapes read, by their file and wheels: the paints of a vehicle share one. Read on both threads (MakeOnce()). */
static std::map<std::string, std::shared_ptr<const ThreeDVehicleShape>> _vehicle_shapes;
static std::mutex _vehicle_shapes_mutex;

/** The shape read already for a key (MakeShape()), nullptr none. */
static std::shared_ptr<const ThreeDVehicleShape> KnownVehicleShape(const std::string &key)
{
	std::lock_guard lock(_vehicle_shapes_mutex);
	auto it = _vehicle_shapes.find(key);
	return it != _vehicle_shapes.end() ? it->second : nullptr;
}

/**
 * A shape read let go of the room its lists grew into as it was read (each up to twice what it holds: the vehicles'
 * shapes took a third more than their corners and triangles).
 */
static void ShrinkShape(ThreeDVehicleShape &s)
{
	s.pos.shrink_to_fit();
	s.nrm.shrink_to_fit();
	s.uv.shrink_to_fit();
	s.tri.shrink_to_fit();
	s.tri_tex.shrink_to_fit();
	s.night_pos.shrink_to_fit();
	s.night_tri.shrink_to_fit();
	s.night_colour.shrink_to_fit();
	s.night_back.shrink_to_fit();
	s.tri_part.shrink_to_fit();
	s.tex_addon.shrink_to_fit();
	s.tex_glow.shrink_to_fit();
	s.tex_alpha.shrink_to_fit();
	s.corner_wheel.shrink_to_fit();
	s.tri_glass.shrink_to_fit();
	s.night_part.shrink_to_fit();
	s.lanterns.shrink_to_fit();
	s.torches.shrink_to_fit();
	s.torch_aims.shrink_to_fit();
	s.front_part.shrink_to_fit();
	s.back_part.shrink_to_fit();
}

/** The extent of all the corners of a shape read (ThreeDVehicleShape::all). */
static void ExtentOfAll(ThreeDVehicleShape &s)
{
	if (s.pos.size() < 3) return;
	for (int k = 0; k < 3; k++) {
		s.all[k * 2] = std::numeric_limits<float>::infinity();
		s.all[k * 2 + 1] = -std::numeric_limits<float>::infinity();
	}
	for (size_t i = 0; i + 2 < s.pos.size(); i += 3) {
		for (int k = 0; k < 3; k++) {
			s.all[k * 2] = std::min(s.all[k * 2], s.pos[i + k]);
			s.all[k * 2 + 1] = std::max(s.all[k * 2 + 1], s.pos[i + k]);
		}
	}
}

/** Keep a shape read for its key (MakeShape()). */
static void KeepVehicleShape(const std::string &key, const std::shared_ptr<const ThreeDVehicleShape> &made)
{
	std::lock_guard lock(_vehicle_shapes_mutex);
	_vehicle_shapes.emplace(key, made);
}

/** How many shapes are kept (the rig). */
static size_t VehicleShapesKept()
{
	std::lock_guard lock(_vehicle_shapes_mutex);
	return _vehicle_shapes.size();
}

/** The frame of a figure that moves at a tick of the game: one every frame_ticks, from phase on, round and round. */
int ThreeDVehicleObject::FrameAt(uint64_t tick, uint32_t phase) const
{
	if (this->frames < 2) return 0;
	return static_cast<int>((tick / this->frame_ticks + phase) % this->frames);
}

/**
 * The shapes made again with more parts in the place of others (ThreeDVehicleObject::GetShape()): a thread may draw from
 * those yet, so they are let go of only between the frames (TrimThreeDTextures()). Under _lazy_mutex.
 */
static std::vector<std::shared_ptr<const ThreeDVehicleShape>> _shapes_replaced;

/** The object's shape as it is now, read from its .glb the first time (only what is always drawn); nullptr when it cannot be. A figure that moves: its first frame. */
const ThreeDVehicleShape *ThreeDVehicleObject::GetShape() const
{
	return this->GetShape(0, 1);
}

/**
 * The object's shape with the parts of its states asked for (PartsOf(); on either thread, MakeOnce()). It has only
 * the parts of the states drawn (the player, 17. 10.: "optimise the whole process with the objects first"): the
 * colleague's trailer of a Tatra is one object of all its ten bodies, three quarters of a million triangles, and a
 * state shows one. A part not in it yet is added: the shape is made again with it, the one before kept till nothing
 * draws from it. A figure that moves: its frame (GetShape(int)).
 */
const ThreeDVehicleShape *ThreeDVehicleObject::GetShape(int frame, const ThreeDParts &asked) const
{
	if (this->frames >= 2 && (frame > 0 || this->blend_from != nullptr)) return this->GetShape(frame);
	MarkThreeDUsed(this->used);
	const ThreeDParts parts = asked | 1;
	MakeOnce(this, LAZY_SHAPE, [this, parts] { return this->shape_tried && (this->shape == nullptr || this->shape_parts.Contains(parts)); }, [this, parts] {
		ThreeDParts want = parts;
		{
			std::lock_guard lock(_lazy_mutex);
			want |= this->shape_parts;
		}
		return std::make_pair(this->MakeShape(0, want), want);
	}, [this](std::pair<std::shared_ptr<const ThreeDVehicleShape>, ThreeDParts> made) {
		if (this->shape != nullptr) _shapes_replaced.push_back(std::move(this->shape));
		this->shape = std::move(made.first);
		this->shape_parts = this->shape != nullptr ? made.second : ThreeDParts{};
		this->shape_tried = true;
		if (this->shape != nullptr && this->shape->all[1] > this->shape->all[0]) {
			std::copy(std::begin(this->shape->all), std::end(this->shape->all), this->extent);
			this->extent_known = true;
			this->extent_version++;
		}
	});
	std::lock_guard lock(_lazy_mutex);
	return this->shape.get();
}

/**
 * The extent of all the corners of the object's shape (ThreeDVehicleShape::all) if it was ever read: how big its
 * sprite is before it is made (ThreeDVehicleObjectSpriteBounds()). Never reads it: asked in the game's tick.
 * @param[out] version each time it grew another (a part added to the shape)
 */
bool ThreeDVehicleObject::KnownExtent(float extent[6], uint32_t *version) const
{
	std::lock_guard lock(_lazy_mutex);
	if (!this->extent_known) return false;
	std::copy(std::begin(this->extent), std::end(this->extent), extent);
	if (version != nullptr) *version = this->extent_version;
	return true;
}

/** Whether the object's shape was read (or tried): a Try... getter that gives nothing then gives nothing for good. */
bool ThreeDVehicleObject::ShapeKnown() const
{
	std::lock_guard lock(_lazy_mutex);
	return this->shape != nullptr || this->shape_tried;
}

/** The object's shape (a frame of it) if it is read already; else nullptr, and it is read in the background (TryGetShape(int, uint64_t)). */
const ThreeDVehicleShape *ThreeDVehicleObject::TryGetShape(int frame) const
{
	return this->TryGetShape(frame, 1);
}

/**
 * The object's shape (a frame of it) if it is read already, with the parts asked for if they are added already: the
 * parts it has not yet are added in the background (MadeOrAsked()), the shape as it is till then; nullptr when there
 * is none yet. A figure's frame that cannot be read: its shape, as GetShape(int) gives it.
 */
const ThreeDVehicleShape *ThreeDVehicleObject::TryGetShape(int frame, const ThreeDParts &asked) const
{
	MarkThreeDUsed(this->used);
	if (this->frames < 2 || (frame <= 0 && this->blend_from == nullptr)) {
		const ThreeDParts parts = asked | 1;
		MadeOrAsked(this, LAZY_SHAPE, [this, parts] { return this->shape_tried && (this->shape == nullptr || this->shape_parts.Contains(parts)); }, [this, parts] { this->GetShape(0, parts); });
		std::lock_guard lock(_lazy_mutex);
		return this->shape.get();
	}
	frame = std::max(frame, 0);
	frame %= this->frames;
	if (!MadeOrAsked(this, LAZY_FRAME + frame, [this, frame] { return static_cast<size_t>(frame) < this->frame_shapes.size() && this->frame_shapes[frame] != nullptr; }, [this, frame] { this->GetShape(frame); })) return nullptr;
	return this->frame_shapes[frame].get();
}

/** The simpler shape of a shape at a level (ThreeDVehicleShape::Simpler()): its corners joined in cells of 2 cm times two to the level. */
static std::shared_ptr<const ThreeDVehicleShape> MakeSimplerShape(const ThreeDVehicleShape &shape, int level)
{
	const double cell = 0.02 * (1 << level);
	const size_t corners = shape.pos.size() / 3;
	const bool has_nrm = shape.nrm.size() == shape.pos.size(), has_uv = shape.uv.size() == corners * 2;
	/* Each corner's texture and part: its first triangle's. */
	std::vector<uint8_t> material(corners, 0), part(corners, 0);
	for (size_t t = shape.tri.size(); t >= 3; t -= 3) {
		const uint8_t m = (t - 3) / 3 < shape.tri_tex.size() ? shape.tri_tex[(t - 3) / 3] : 0;
		const uint8_t p = (t - 3) / 3 < shape.tri_part.size() ? shape.tri_part[(t - 3) / 3] : 0;
		for (int k = 0; k < 3; k++) {
			material[shape.tri[t - 3 + k]] = m;
			part[shape.tri[t - 3 + k]] = p;
		}
	}
	/* The islands of the texture: the corners joined by triangles, a piece of the model drawn from one piece of the
	 * texture (a file splits its corners where the texture does, at a seam, between the islands of an atlas). */
	std::vector<uint32_t> island(corners);
	std::iota(island.begin(), island.end(), 0u);
	auto root = [&island](uint32_t i) {
		while (island[i] != i) {
			island[i] = island[island[i]];
			i = island[i];
		}
		return i;
	};
	for (size_t t = 0; t + 2 < shape.tri.size(); t += 3) {
		const uint32_t a = root(shape.tri[t]);
		for (int k = 1; k < 3; k++) {
			const uint32_t b = root(shape.tri[t + k]);
			if (a != b) island[b] = a;
		}
	}
	/* Corners in one place of the model and of the texture, facing to the same side, are one there: a file of flat
	 * faces (the colleague's ghost, each of its triangles with three corners of its own) has them apart, so each of
	 * its triangles was an island of its own, and those smaller than a cell were left out -- the ghost full of holes
	 * from the eyes. Joined (weld), they are of one island and face the way of all of them together. */
	std::vector<uint32_t> weld(corners);
	{
		struct WeldHash {
			size_t operator()(const std::array<int64_t, 6> &k) const
			{
				uint64_t h = 0;
				for (int64_t v : k) h = (h ^ static_cast<uint64_t>(v)) * 0x9E3779B97F4A7C15ull;
				return static_cast<size_t>(h ^ (h >> 29));
			}
		};
		std::unordered_map<std::array<int64_t, 6>, uint32_t, WeldHash> seen;
		seen.reserve(corners);
		for (size_t i = 0; i < corners; i++) {
			std::array<int64_t, 6> k{};
			for (int a = 0; a < 3; a++) k[a] = std::llround(shape.pos[i * 3 + a] * 1e5);
			if (has_uv) for (int a = 0; a < 2; a++) k[3 + a] = std::llround(shape.uv[i * 2 + a] * 1e6);
			/* The side it faces (a sheet's two sides apart, each its own way). */
			if (has_nrm) k[5] = shape.nrm[i * 3] + shape.nrm[i * 3 + 1] + shape.nrm[i * 3 + 2] >= 0 ? 1 : 0;
			weld[i] = seen.emplace(k, static_cast<uint32_t>(i)).first->second;
		}
	}
	std::vector<float> weld_nrm(has_nrm ? shape.nrm.size() : 0, 0.0f);
	for (size_t i = 0; i < corners; i++) {
		if (weld[i] != i) {
			const uint32_t a = root(weld[i]), b = root(static_cast<uint32_t>(i));
			if (a != b) island[b] = a;
		}
		if (has_nrm) for (int a = 0; a < 3; a++) weld_nrm[weld[i] * 3 + a] += shape.nrm[i * 3 + a];
	}
	/* What else keeps corners apart: their part (an add-on's node, the glass, the driver), their wheel, and their island
	 * of the texture: corners of one place of the model in two places of the texture (two islands, a seam) joined took
	 * one's place for both, and the triangles between them drew from anywhere between the islands -- a car black and
	 * speckled from a few metres on (the player, 17. 10.). Within an island they join: it is one piece of the texture.
	 * (It was a 128th of the texture before: the colleague's lorries, their corners far apart in their atlases, kept
	 * 374 of 692 thousand triangles at the farthest zoom.) */
	auto apart_of = [&](size_t i) {
		return static_cast<uint64_t>(part[i]) | static_cast<uint64_t>(i < shape.corner_wheel.size() ? shape.corner_wheel[i] : 0u) << 8 | static_cast<uint64_t>(root(static_cast<uint32_t>(i))) << 16;
	};
	/* The cell of a corner: where it is, which way it faces (the axis it faces most along), its texture. */
	auto key_of = [&](size_t i) {
		uint64_t k = 0;
		for (int a = 0; a < 3; a++) k = k * 4096 + static_cast<uint64_t>(static_cast<int64_t>(std::floor(shape.pos[i * 3 + a] / cell)) & 4095);
		int face = 0;
		if (has_nrm) {
			const float nx = weld_nrm[weld[i] * 3], ny = weld_nrm[weld[i] * 3 + 1], nz = weld_nrm[weld[i] * 3 + 2];
			const float ax = std::abs(nx), ay = std::abs(ny), az = std::abs(nz);
			face = ax >= ay && ax >= az ? (nx < 0 ? 1 : 2) : (ay >= az ? (ny < 0 ? 3 : 4) : (nz < 0 ? 5 : 6));
		}
		return (k * 8 + face) * 256 + material[i];
	};
	auto out = std::make_shared<ThreeDVehicleShape>();
	struct CellHash {
		size_t operator()(const std::pair<uint64_t, uint64_t> &k) const { return std::hash<uint64_t>{}(k.first * 0x9E3779B97F4A7C15ull ^ k.second); }
	};
	std::unordered_map<std::pair<uint64_t, uint64_t>, uint32_t, CellHash> cells;
	std::vector<uint32_t> joined(corners);
	std::vector<uint32_t> count;
	const bool wheels = !shape.corner_wheel.empty();
	for (size_t i = 0; i < corners; i++) {
		auto [it, added] = cells.emplace(std::make_pair(key_of(i), apart_of(i)), static_cast<uint32_t>(count.size()));
		joined[i] = it->second;
		if (added) {
			count.push_back(0);
			out->pos.insert(out->pos.end(), 3, 0.0f);
			if (has_nrm) out->nrm.insert(out->nrm.end(), 3, 0.0f);
			if (has_uv) out->uv.insert(out->uv.end(), 2, 0.0f);
			if (wheels) out->corner_wheel.push_back(shape.corner_wheel[i]);
		}
		const uint32_t c = it->second;
		count[c]++;
		for (int a = 0; a < 3; a++) out->pos[c * 3 + a] += shape.pos[i * 3 + a];
		if (has_nrm) for (int a = 0; a < 3; a++) out->nrm[c * 3 + a] += shape.nrm[i * 3 + a];
		/* The places in the texture of its corners, all in one small piece of it (apart_of()): their middle. */
		if (has_uv) for (int a = 0; a < 2; a++) out->uv[c * 2 + a] += shape.uv[i * 2 + a];
	}
	for (size_t c = 0; c < count.size(); c++) {
		for (int a = 0; a < 3; a++) out->pos[c * 3 + a] /= count[c];
		if (has_uv) for (int a = 0; a < 2; a++) out->uv[c * 2 + a] /= count[c];
		if (has_nrm) {
			float *n = &out->nrm[c * 3];
			const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (l > 0) for (int a = 0; a < 3; a++) n[a] /= l;
		}
	}
	for (size_t t = 0; t + 2 < shape.tri.size(); t += 3) {
		const uint32_t a = joined[shape.tri[t]], b = joined[shape.tri[t + 1]], c = joined[shape.tri[t + 2]];
		if (a == b || b == c || a == c) continue;
		out->tri.insert(out->tri.end(), {a, b, c});
		out->tri_tex.push_back(t / 3 < shape.tri_tex.size() ? shape.tri_tex[t / 3] : 0);
		if (!shape.tri_part.empty()) out->tri_part.push_back(t / 3 < shape.tri_part.size() ? shape.tri_part[t / 3] : 0);
		if (!shape.tri_glass.empty()) out->tri_glass.push_back(t / 3 < shape.tri_glass.size() ? shape.tri_glass[t / 3] : 0);
	}
	out->tex_addon = shape.tex_addon;
	out->tex_glow = shape.tex_glow;
	out->tex_alpha = shape.tex_alpha;
	out->glass_alpha = shape.glass_alpha;
	out->x0 = shape.x0; out->x1 = shape.x1; out->y0 = shape.y0; out->y1 = shape.y1; out->z0 = shape.z0; out->z1 = shape.z1;
	ShrinkShape(*out);
	Debug(misc, 3, "3D shape simpler at {} cm: {} of {} triangles", std::lround(cell * 100), out->tri.size() / 3, shape.tri.size() / 3);
	return out;
}

/**
 * A simpler shape of this one for a model drawn small (the view from the eyes; the player, 16. 10.: "save when they
 * are far, it need not be drawn in detail far"): its corners joined in cells of about the size asked for -- 2, 4,
 * ... 64 cm, made the first time each is asked for -- each cell's corners one at their middle, at the place in the
 * texture of the first of them, the triangles left with three corners kept. A cell joins only corners facing much
 * the same way and of the same texture, so the edges of a body stay. The lights and the textures stay the shape's.
 * @param cell_m the size of a cell asked for, metres of the model; below 2 cm the shape itself
 */
const ThreeDVehicleShape *ThreeDVehicleShape::Simpler(double cell_m) const
{
	if (!(cell_m >= 0.02) || this->tri.size() < 3 * 512) return this;
	const int level = std::min(SIMPLER_LEVELS - 1, static_cast<int>(std::floor(std::log2(cell_m / 0.02))));
	std::shared_ptr<const ThreeDVehicleShape> &made = this->simpler.shapes[level];
	MakeOnce(this, LAZY_SIMPLER + level, [&made] { return made != nullptr; }, [this, level] { return MakeSimplerShape(*this, level); }, [&made](std::shared_ptr<const ThreeDVehicleShape> out) { made = std::move(out); });
	return made.get();
}

/** The simpler shape if it is made already; else the shape itself stands in, and the simpler one is made in the background. */
const ThreeDVehicleShape *ThreeDVehicleShape::TrySimpler(double cell_m) const
{
	if (!(cell_m >= 0.02) || this->tri.size() < 3 * 512) return this;
	const int level = std::min(SIMPLER_LEVELS - 1, static_cast<int>(std::floor(std::log2(cell_m / 0.02))));
	const std::shared_ptr<const ThreeDVehicleShape> &made = this->simpler.shapes[level];
	if (!MadeOrAsked(this, LAZY_SIMPLER + level, [&made] { return made != nullptr; }, [this, cell_m] { this->Simpler(cell_m); })) return this;
	return made.get();
}

/** A frame of a figure that moves (FrameAt()), read the first time it is drawn (MakeOnce()); the others as GetShape(). */
const ThreeDVehicleShape *ThreeDVehicleObject::GetShape(int frame) const
{
	MarkThreeDUsed(this->used);
	if (this->frames < 2 || (frame <= 0 && this->blend_from == nullptr)) return this->GetShape();
	frame = std::max(frame, 0);
	frame %= this->frames;
	MakeOnce(this, LAZY_FRAME + frame, [this, frame] {
		if (this->frame_shapes.size() < static_cast<size_t>(this->frames)) this->frame_shapes.resize(this->frames);
		return this->frame_shapes[frame] != nullptr;
	}, [this, frame] { return this->MakeShape(frame); }, [this, frame](std::shared_ptr<const ThreeDVehicleShape> made) { this->frame_shapes[frame] = std::move(made); });
	const ThreeDVehicleShape *made = this->frame_shapes[frame].get();
	return made != nullptr ? made : this->GetShape();
}

/**
 * A frame of a creature with bones (skeleton_file): the mesh posed by its animation at the frame's time -- a round of
 * the frames is the animation from its first key to its last; one that does not go round ends on its last. A
 * passage (blend_from) mixes the pose it comes from and the one it goes to, bone by bone, more of the latter each
 * tick. Kept for the creatures of the same bones (the brown and the black dragon).
 */
std::shared_ptr<const ThreeDVehicleShape> ThreeDVehicleObject::MakePosedShape(int frame) const
{
	GlbFilesScope files;
	std::string key = fmt::format("{}#{}#{}/{}{}", this->skeleton_file, this->skeleton_anim, frame, this->frames, this->loop ? "o" : "-");
	if (this->blend_from != nullptr) key += fmt::format("<{}#{}/{}x{}", this->blend_from->skeleton_anim, this->blend_from_frame, this->blend_from->frames, this->blend_ticks);
	if (auto known = KnownVehicleShape(key); known != nullptr) return known;
	const GlbSkeleton *sk = ReadGlbSkeleton(this->skeleton_file);
	if (sk == nullptr) return nullptr;
	/* The time of a frame of a figure, seconds. */
	auto time_of = [](const ThreeDVehicleObject &f, const GlbSkeleton::Animation *a, int fr) {
		if (a == nullptr) return 0.0;
		if (f.frames < 2) return a->start;
		const double phase = f.loop ? static_cast<double>(fr) / f.frames : static_cast<double>(fr) / (f.frames - 1);
		return a->start + (a->end - a->start) * std::clamp(phase, 0.0, 1.0);
	};
	std::vector<BonePose> pose;
	if (this->blend_from != nullptr && this->blend_to != nullptr && this->blend_ticks > 0) {
		const int own = frame / this->blend_ticks, tick = frame % this->blend_ticks;
		const GlbSkeleton::Animation *from = sk->Find(this->blend_from->skeleton_anim), *to = sk->Find(this->blend_to->skeleton_anim);
		const std::vector<BonePose> a = sk->Pose(from, time_of(*this->blend_from, from, this->blend_from_frame));
		const std::vector<BonePose> b = sk->Pose(to, time_of(*this->blend_to, to, own));
		const double k = (tick + 1.0) / (this->blend_ticks + 1.0);
		pose.reserve(a.size());
		for (size_t i = 0; i < a.size(); i++) pose.push_back(a[i].Mix(b[i], k));
	} else {
		const GlbSkeleton::Animation *anim = sk->Find(this->skeleton_anim);
		pose = sk->Pose(anim, time_of(*this, anim, frame));
	}
	auto made = std::make_shared<ThreeDVehicleShape>();
	sk->Skin(pose, made->pos, made->nrm);
	made->uv.assign(sk->uv.begin(), sk->uv.end());
	made->tri = sk->tri;
	made->tri_tex = sk->tri_tex;
	for (const GlbTexture &t : sk->textures) {
		made->pngs.push_back(t.png);
		made->flats.push_back(t.flat);
		made->tex_names.push_back(t.name);
		made->tex_glow.push_back(t.glow);
		made->tex_alpha.push_back(t.alpha);
	}
	/* Its extent as posed (how big it is in the view from the eyes, CabFigure). */
	if (!made->pos.empty()) {
		made->x0 = made->y0 = made->z0 = std::numeric_limits<float>::infinity();
		made->x1 = made->y1 = made->z1 = -std::numeric_limits<float>::infinity();
		for (size_t i = 0; i + 2 < made->pos.size(); i += 3) {
			made->x0 = std::min(made->x0, made->pos[i]); made->x1 = std::max(made->x1, made->pos[i]);
			made->y0 = std::min(made->y0, made->pos[i + 1]); made->y1 = std::max(made->y1, made->pos[i + 1]);
			made->z0 = std::min(made->z0, made->pos[i + 2]); made->z1 = std::max(made->z1, made->pos[i + 2]);
		}
	}
	ShrinkShape(*made);
	ExtentOfAll(*made);
	KeepVehicleShape(key, made);
	return made;
}

/** Read the object's shape (a frame of it), or the one read already for the same file, wheels, crew and frame. */
/**
 * Make the object's shape (a frame of it): its file, its wheels, its crew, and of its add-ons the nodes of the parts
 * asked for (GetShape(int, uint64_t)) -- the textures of all its add-ons all the same, in the same order whatever parts
 * are asked for (a texture is asked for by its index: ThreeDVehicleObject::GetTexture()).
 * @param parts the parts of its states (PartsOf()), bit n the part n
 */
std::shared_ptr<const ThreeDVehicleShape> ThreeDVehicleObject::MakeShape(int frame, const ThreeDParts &parts) const
{
	GlbFilesScope files;
	if (!this->skeleton_file.empty()) return this->MakePosedShape(frame);
	const auto started = std::chrono::steady_clock::now();
	std::string key = this->file;
	if (!this->frame_prefix.empty()) key += fmt::format("#{}{}", this->frame_prefix, frame);
	for (const ThreeDVehicleWheels &w : this->wheels) {
		key += fmt::format("|{}{}", w.file, w.both_sides ? "*2" : "");
		for (const auto &at : w.at) key += fmt::format(",{},{},{}", at[0], at[1], at[2]);
	}
	for (const ThreeDVehicleCrew &c : this->crew) key += fmt::format("|{}@{},{},{}/{}x{}", c.figure, c.at[0], c.at[1], c.at[2], c.turn, c.scale);
	for (const auto &[group, list] : this->crew_groups) {
		key += "|=" + group;
		for (const ThreeDVehicleCrew &c : list) key += fmt::format("|{}@{},{},{}/{}x{}", c.figure, c.at[0], c.at[1], c.at[2], c.turn, c.scale);
	}
	for (const ThreeDVehicleAddon &a : this->addons) {
		key += fmt::format("|+{}@{},{},{}", a.file, a.at[0], a.at[1], a.at[2]);
		for (const std::string &n : a.nodes) key += "/" + n;
	}
	for (const ThreeDModelWheel &w : this->model_wheels) key += "|o" + w.addon + "/" + w.node;
	if (!this->glass_node.empty()) key += "|s" + this->glass_node;
	if (!this->only_node.empty()) key += "|u" + this->only_node;
	/* Read otherwise, so a shape of its own (the player, #286: the clay town's giant mushroom at night was its day's
	 * shape, read first, without the glowing underside of its cap): the night kept or left out, a node left out. */
	if (this->night_glass) key += "|n";
	if (this->frame_lights) key += "|f";
	if (!this->skip_node.empty()) key += "|x" + this->skip_node;
	if (this->has_cab) key += fmt::format("|k{},{},{}", this->cab[0], this->cab[1], this->cab[2]);
	if (!this->addons.empty()) key += fmt::format("|p{:x}.{:x}", parts.bits[1], parts.bits[0]);
	if (auto known = KnownVehicleShape(key); known != nullptr) return known;
	GlbMesh mesh;
	if (this->file.starts_with("beamng:")) {
		/* A car of a mod of BeamNG (beamng.h): its materials as they are, the paint first, white; its lights of the mod.
		 * One at a time: the zips of the mods are opened as they are read (beamng.cpp), and this may be either thread. */
		static std::mutex beamng_mutex;
		std::lock_guard beamng_lock(beamng_mutex);
		const BeamngVehicle *car = nullptr;
		for (const BeamngVehicle &b : FindBeamngVehicles()) {
			if (this->file == "beamng:" + b.Package() + "|" + b.folder) car = &b;
		}
		BeamngMesh bm;
		if (car == nullptr || !ReadBeamngShape(*car, bm)) return nullptr;
		auto made = std::make_shared<ThreeDVehicleShape>();
		made->pos = std::move(bm.pos);
		made->nrm = std::move(bm.nrm);
		made->uv = std::move(bm.uv);
		made->tri = std::move(bm.tri);
		made->tri_tex = std::move(bm.tri_tex);
		for (BeamngMesh::Material &m : bm.materials) {
			std::vector<uint8_t> png = std::move(m.png);
			if (png.empty() && !m.rgba.empty()) {
				png.assign(RAW_TEXTURE_MARK.begin(), RAW_TEXTURE_MARK.end());
				for (uint32_t v : {static_cast<uint32_t>(m.w), static_cast<uint32_t>(m.h)}) {
					for (int k = 0; k < 4; k++) png.push_back(static_cast<uint8_t>(v >> (8 * k)));
				}
				png.insert(png.end(), m.rgba.begin(), m.rgba.end());
			}
			made->pngs.push_back(InternThreeDTextureBytes(png));
			made->flats.push_back(m.flat);
			made->tex_names.push_back(std::move(m.name));
			made->tex_glow.push_back(0);
			made->tex_alpha.push_back(255);
		}
		made->paint_whole = true;
		made->night_pos = std::move(bm.night_pos);
		made->night_tri = std::move(bm.night_tri);
		made->night_colour = std::move(bm.night_colour);
		made->night_back = std::move(bm.night_back);
		made->front_lights = std::move(bm.front_lights);
		made->back_lights = std::move(bm.back_lights);
		made->x0 = made->y0 = made->z0 = 1e9f;
		made->x1 = made->y1 = made->z1 = -1e9f;
		for (size_t i = 0; i + 2 < made->pos.size(); i += 3) {
			made->x0 = std::min(made->x0, made->pos[i]); made->x1 = std::max(made->x1, made->pos[i]);
			made->y0 = std::min(made->y0, made->pos[i + 1]); made->y1 = std::max(made->y1, made->pos[i + 1]);
			made->z0 = std::min(made->z0, made->pos[i + 2]); made->z1 = std::max(made->z1, made->pos[i + 2]);
		}
		Debug(misc, 1, "3D vehicle {}: {} triangles, {} materials, {:.2f}..{:.2f} x {:.2f}..{:.2f} x {:.2f}..{:.2f} m", this->name, made->tri.size() / 3, made->pngs.size(),
				made->x0, made->x1, made->y0, made->y1, made->z0, made->z1);
		ShrinkShape(*made);
		ExtentOfAll(*made);
		KeepVehicleShape(key, made);
		return made;
	}
	if (this->file.ends_with(".s") || this->file.ends_with(".S")) {
		/* A model of MSTS (simtrains.h): its textures as they are, RGBA, marked so (DecodeTextureLevels()). */
		SimtrainsMesh msts;
		if (!ReadSimtrainsShape(this->file, msts)) return nullptr;
		mesh.pos = std::move(msts.pos);
		mesh.nrm = std::move(msts.nrm);
		mesh.uv = std::move(msts.uv);
		mesh.tri = std::move(msts.tri);
		mesh.tri_tex = std::move(msts.tri_tex);
		for (SimtrainsMesh::Texture &t : msts.textures) {
			GlbTexture g;
			g.name = t.name;
			if (!t.rgba.empty()) {
				std::vector<uint8_t> raw(RAW_TEXTURE_MARK.begin(), RAW_TEXTURE_MARK.end());
				for (uint32_t v : {static_cast<uint32_t>(t.w), static_cast<uint32_t>(t.h)}) {
					for (int k = 0; k < 4; k++) raw.push_back(static_cast<uint8_t>(v >> (8 * k)));
				}
				raw.insert(raw.end(), t.rgba.begin(), t.rgba.end());
				g.png = InternThreeDTextureBytes(raw);
			}
			mesh.textures.push_back(std::move(g));
		}
	} else {
		/* Its glass and the wheels of its model that turn, each its nodes marked (tri_group: 1 the glass, then the wheels). */
		std::vector<std::string> marked{this->glass_node.empty() ? std::string("\x01") : this->glass_node};
		/* (A wheel of an add-on is marked in the add-on: a name never found here keeps the numbering of the wheels.) */
		for (const ThreeDModelWheel &w : this->model_wheels) marked.push_back(w.addon.empty() ? w.node : std::string("\x01"));
		if (this->frame_lights) {
			/* A witch's frame: the glass of her lights ("noc_NN" of the frame) read below. */
			GlbReadOptions o;
			o.all_nodes = true;
			o.frame_prefix = this->frame_prefix;
			o.frame = frame;
			o.only_node = this->only_node;
			o.skip_night = true;
			o.groups = &marked;
			if (!ReadGlbMesh(this->file, mesh, o)) return nullptr;
		} else if (!this->skip_node.empty() || this->night_glass) {
			/* A node left out (the rocket shot), the lights at night as they are or a light's glass kept. */
			GlbReadOptions o;
			o.all_nodes = true;
			o.frame_prefix = this->frame_prefix;
			o.frame = frame;
			o.only_node = this->only_node;
			o.skip_node = this->skip_node;
			o.skip_night = !this->night_glass;
			o.groups = &marked;
			if (!ReadGlbMesh(this->file, mesh, o)) return nullptr;
		} else if (!ReadGlbMesh(this->file, mesh, true, this->frame_prefix, frame, this->only_node, {}, nullptr, "noc", &marked)) {
			return nullptr;
		}
	}
	auto made = std::make_shared<ThreeDVehicleShape>();
	if (!this->file.ends_with(".s") && !this->file.ends_with(".S")) {
		/* Its lights: the glass of "noc" in the colours of its materials, and the places they shine from. A
		 * witch's of her frame: the glass of its "noc_NN", the places of her lantern and her torch in it. */
		GlbMesh night, none;
		if (this->frame_lights) {
			GlbReadOptions o;
			o.all_nodes = true;
			o.frame_prefix = this->frame_prefix;
			o.frame = frame;
			o.only_night = true;
			ReadGlbMesh(this->file, night, o);
			o.only_night = false;
			o.only_node = "\x01";
			o.points_only = true;
			for (auto [prefix, list] : {std::pair{"lucerna_", &made->lanterns}, std::pair{"svitilna_", &made->torches}, std::pair{"paprsek_", &made->torch_aims}}) {
				o.point_prefix = prefix;
				o.points = list;
				ReadGlbMesh(this->file, none, o);
			}
			/* A torch without the aim of its beam, or an aim without its torch, shines nowhere. */
			if (made->torches.size() != made->torch_aims.size()) {
				Debug(misc, 0, "3D figure {}: frame {} has {} torches and {} aims of their beams", this->name, frame, made->torches.size() / 3, made->torch_aims.size() / 3);
				made->torches.clear();
				made->torch_aims.clear();
			}
		} else {
			ReadGlbMesh(this->file, night, true, {}, 0, "noc", "svetlo_predni_", &made->front_lights);
			ReadGlbMesh(this->file, none, true, {}, 0, "\x01", "svetlo_zadni_", &made->back_lights);
		}
		made->night_pos = std::move(night.pos);
		made->night_tri = std::move(night.tri);
		for (uint8_t t : night.tri_tex) {
			const GlbTexture *tex = t < night.textures.size() ? &night.textures[t] : nullptr;
			made->night_colour.push_back(tex != nullptr ? tex->flat : Colour(255, 244, 216).data);
			made->night_back.push_back(tex != nullptr && tex->name.find("zadni") != std::string::npos ? 1 : 0);
		}
		if (made->HasLights()) Debug(misc, 2, "3D vehicle {}: lights, {} triangles of glass, {} ahead, {} at the back", this->name, made->night_tri.size() / 3, made->front_lights.size() / 3, made->back_lights.size() / 3);
	}
	made->pos = std::move(mesh.pos);
	made->nrm = std::move(mesh.nrm);
	made->uv = std::move(mesh.uv);
	made->tri = std::move(mesh.tri);
	made->tri_tex = std::move(mesh.tri_tex);
	for (GlbTexture &t : mesh.textures) {
		made->pngs.push_back(std::move(t.png));
		made->flats.push_back(t.flat);
		made->tex_names.push_back(std::move(t.name));
		made->tex_glow.push_back(t.glow);
		made->tex_alpha.push_back(t.alpha);
	}
	/* The glass and the wheels that turn (the colleague's hra/3d/PROSBA-V3S-3D.md). */
	made->glass_alpha = this->glass_alpha;
	if (!mesh.tri_group.empty() && std::ranges::any_of(mesh.tri_group, [](uint16_t g) { return g != 0; })) {
		made->tri_part.assign(made->tri.size() / 3, 0);
		for (size_t t = 0; t < mesh.tri_group.size() && t < made->tri_part.size(); t++) {
			const uint16_t g = mesh.tri_group[t];
			if (g == 1) {
				made->tri_part[t] = ThreeDVehicleShape::PART_GLASS;
			} else if (g >= 2) {
				if (made->corner_wheel.empty()) made->corner_wheel.assign(made->pos.size() / 3, 0);
				for (int k = 0; k < 3; k++) made->corner_wheel[made->tri[t * 3 + k]] = static_cast<uint8_t>(g - 1);
			}
		}
	}
	/* Each triangle's part as the others are added: those so far are of the body, part 0. */
	auto mark_part = [&made](uint8_t part, size_t first_triangle) {
		if (part == 0 && made->tri_part.empty()) return;
		made->tri_part.resize(first_triangle, 0);
		made->tri_part.resize(made->tri.size() / 3, part);
	};
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
			made->tex_names.push_back(t.name);
			made->tex_glow.push_back(t.glow);
			made->tex_alpha.push_back(t.alpha);
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
	/* The crew: each figure's shape sized, turned about z and stood at its place, its textures after the others. The
	 * driver -- the one sitting at the eye of the cab -- is not drawn from that eye. A figure bigger than it is ("meritko")
	 * stays that big everywhere, the view from the eyes too. A figure that walks is not of the shape: it is drawn on its
	 * own, its frame by the way gone (WalkingCrew()). */
	auto add_crew = [&](const ThreeDVehicleCrew &c, uint8_t part) {
		const ThreeDVehicleObject *fig = GetThreeDFigure(c.figure, this->package);
		if (fig != nullptr && fig->Walks()) return; // drawn on its own, a frame by the way gone (WalkingCrew())
		const ThreeDVehicleShape *fs = fig != nullptr ? fig->GetShape() : nullptr;
		if (fs == nullptr) {
			Debug(misc, 0, "3D vehicle {}: the figure {} is not there", this->name, c.figure);
			return;
		}
		if ((part == 0 || part == this->crew_record_part) && this->has_cab && std::hypot(c.at[0] - this->cab[0], c.at[1] - this->cab[1]) < 0.4f) part = ThreeDVehicleShape::PART_DRIVER;
		const size_t first_triangle = made->tri.size() / 3;
		const bool lit = !made->nrm.empty() && !fs->nrm.empty();
		if (!lit) made->nrm.clear();
		uint8_t first_tex = static_cast<uint8_t>(std::min<size_t>(made->pngs.size(), 255));
		for (size_t t = 0; t < fs->pngs.size() && made->pngs.size() < 255; t++) {
			made->pngs.push_back(fs->pngs[t]);
			made->flats.push_back(fs->flats[t]);
			made->tex_names.push_back(t < fs->tex_names.size() ? fs->tex_names[t] : std::string());
			made->tex_glow.push_back(fs->Glow(t));
			made->tex_alpha.push_back(fs->Alpha(t));
		}
		const float a = c.turn * static_cast<float>(M_PI) / 180.0f, ca = std::cos(a), sa = std::sin(a), s = c.scale;
		uint32_t base = static_cast<uint32_t>(made->pos.size() / 3);
		for (size_t i = 0; i < fs->pos.size(); i += 3) {
			made->pos.push_back(c.at[0] + (fs->pos[i] * ca - fs->pos[i + 1] * sa) * s);
			made->pos.push_back(c.at[1] + (fs->pos[i] * sa + fs->pos[i + 1] * ca) * s);
			made->pos.push_back(c.at[2] + fs->pos[i + 2] * s);
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
		mark_part(part, first_triangle);
	};
	for (const ThreeDVehicleCrew &c : this->crew) add_crew(c, static_cast<uint8_t>(this->crew_record_part));
	/* What a state shows -- a group of the crew, the load -- is not of the vehicle's size (its length on the map). */
	const size_t extent_floats = made->pos.size();
	auto part_of = [&](const std::string &name) {
		auto p = std::ranges::find(this->part_names, name);
		return p == this->part_names.end() || p == this->part_names.begin() || p - this->part_names.begin() >= ThreeDParts::COUNT ? uint8_t{0} : static_cast<uint8_t>(p - this->part_names.begin());
	};
	for (const auto &[group, list] : this->crew_groups) {
		const uint8_t part = part_of("posadky/" + group);
		if (part == 0) continue;
		/* One at the driver's seat (Mat driving) not drawn from the own cab's eye, as the record's driver is not. */
		for (const ThreeDVehicleCrew &c : list) add_crew(c, c.driver_seat ? static_cast<uint8_t>(ThreeDVehicleShape::PART_SEAT + part) : part);
	}
	/* The add-ons: of each, only the nodes a state may show, each its part, its textures after the others (marked as
	 * the add-on's, for the colour a state gives it). */
	for (size_t ai = 0; ai < this->addons.size(); ai++) {
		const ThreeDVehicleAddon &addon = this->addons[ai];
		/* Only the nodes a state may show (the others not read at all: a file of the loads has seventy five), without
		 * the glass of their lamps; their windows ("<dil>_sklo") and their wheels marked (the colleague's
		 * hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md, points 6 and 10). */
		std::vector<std::string> wheel_nodes;
		std::vector<uint8_t> wheel_of;
		for (size_t w = 0; w < this->model_wheels.size(); w++) {
			if (this->model_wheels[w].addon != addon.key) continue;
			wheel_nodes.push_back(this->model_wheels[w].node);
			wheel_of.push_back(static_cast<uint8_t>(std::min<size_t>(w + 1, 255)));
		}
		/* Only the nodes of the parts asked for, each with its part (once: the triangles of an add-on are a million);
		 * the textures of all its nodes, in the order they are met (ReadGlbMesh(): material_groups). */
		std::vector<std::string> nodes;
		std::vector<uint8_t> node_part;
		for (const std::string &node : addon.nodes) {
			const uint8_t part = part_of(addon.key + "/" + node);
			if (part == 0 || !parts.Has(part)) continue;
			nodes.push_back(node);
			node_part.push_back(part);
		}
		GlbReadOptions read;
		read.all_nodes = true;
		read.groups = &nodes;
		read.material_groups = &addon.nodes;
		read.only_groups = true;
		read.skip_night = true;
		read.marks = &wheel_nodes;
		read.glass_suffix = "_sklo";
		GlbMesh am;
		if (!ReadGlbMesh(addon.file, am, read) && am.textures.empty()) {
			Debug(misc, 0, "3D vehicle {}: the add-on {} cannot be read", this->name, addon.file);
			continue;
		}
		/* The lamps of its parts (point 5): the glass of each part's "noc_<dil>", the places its lights shine from. */
		{
			GlbReadOptions lamps;
			lamps.all_nodes = true;
			lamps.groups = &nodes;
			lamps.only_groups = true;
			lamps.only_night = true;
			std::vector<float> front, back;
			std::vector<uint16_t> front_group, back_group;
			lamps.point_prefix = "svetlo_predni_";
			lamps.points = &front;
			lamps.point_groups = &front_group;
			GlbMesh night;
			ReadGlbMesh(addon.file, night, lamps);
			GlbReadOptions rear = lamps;
			rear.points_only = true;
			rear.only_night = false;
			rear.only_node = "\x01";
			rear.point_prefix = "svetlo_zadni_";
			rear.points = &back;
			rear.point_groups = &back_group;
			GlbMesh none;
			ReadGlbMesh(addon.file, none, rear);
			auto group_part = [&](uint16_t g) { return g > 0 && g <= node_part.size() ? node_part[g - 1] : uint8_t{0}; };
			/* The body's own lamps so far: always (part 0). */
			made->night_part.resize(made->night_tri.size() / 3, 0);
			made->front_part.resize(made->front_lights.size() / 3, 0);
			made->back_part.resize(made->back_lights.size() / 3, 0);
			const uint32_t night_base = static_cast<uint32_t>(made->night_pos.size() / 3);
			for (size_t i = 0; i + 2 < night.pos.size(); i += 3) {
				for (int k = 0; k < 3; k++) made->night_pos.push_back(night.pos[i + k] + addon.at[k]);
			}
			for (size_t t = 0; t + 2 < night.tri.size(); t += 3) {
				const uint8_t part = group_part(t / 3 < night.tri_group.size() ? night.tri_group[t / 3] : 0);
				if (part == 0) continue; // a part no state shows
				for (int k = 0; k < 3; k++) made->night_tri.push_back(night_base + night.tri[t + k]);
				const uint8_t tx = night.tri_tex[t / 3];
				const GlbTexture *tex = tx < night.textures.size() ? &night.textures[tx] : nullptr;
				made->night_colour.push_back(tex != nullptr ? tex->flat : Colour(255, 244, 216).data);
				made->night_back.push_back(tex != nullptr && tex->name.find("zadni") != std::string::npos ? 1 : 0);
				made->night_part.push_back(part);
			}
			auto add_points = [&](const std::vector<float> &points, const std::vector<uint16_t> &groups, std::vector<float> &to, std::vector<uint8_t> &to_part) {
				for (size_t p = 0; p * 3 + 2 < points.size(); p++) {
					const uint8_t part = group_part(p < groups.size() ? groups[p] : 0);
					if (part == 0) continue;
					for (int k = 0; k < 3; k++) to.push_back(points[p * 3 + k] + addon.at[k]);
					to_part.push_back(part);
				}
			};
			add_points(front, front_group, made->front_lights, made->front_part);
			add_points(back, back_group, made->back_lights, made->back_part);
		}
		/* (An add-on of none of the parts asked for adds its textures only.) */
		const bool lit = !made->nrm.empty() && !am.nrm.empty();
		if (!lit && !am.pos.empty()) made->nrm.clear();
		uint8_t first_tex = static_cast<uint8_t>(std::min<size_t>(made->pngs.size(), 255));
		made->tex_addon.resize(made->pngs.size(), -1);
		for (GlbTexture &tx : am.textures) {
			if (made->pngs.size() >= 255) break;
			made->pngs.push_back(std::move(tx.png));
			made->flats.push_back(tx.flat);
			made->tex_names.push_back(std::move(tx.name));
			made->tex_glow.push_back(tx.glow);
			made->tex_alpha.push_back(tx.alpha);
			made->tex_addon.push_back(static_cast<int8_t>(std::min<size_t>(ai, 127)));
		}
		uint32_t base = static_cast<uint32_t>(made->pos.size() / 3);
		made->pos.reserve(made->pos.size() + am.pos.size());
		if (lit) made->nrm.reserve(made->nrm.size() + am.nrm.size());
		made->tri.reserve(made->tri.size() + am.tri.size());
		made->tri_tex.reserve(made->tri_tex.size() + am.tri.size() / 3);
		for (size_t i = 0; i + 2 < am.pos.size(); i += 3) {
			for (int k = 0; k < 3; k++) made->pos.push_back(am.pos[i + k] + addon.at[k]);
			if (lit) for (int k = 0; k < 3; k++) made->nrm.push_back(am.nrm[i + k]);
		}
		made->uv.insert(made->uv.end(), am.uv.begin(), am.uv.end());
		for (size_t t = 0; t + 2 < am.tri.size(); t += 3) {
			const uint16_t g = t / 3 < am.tri_group.size() ? am.tri_group[t / 3] : 0;
			const uint8_t part = g > 0 && g <= node_part.size() ? node_part[g - 1] : 0;
			if (part == 0) continue; // a node no state shows
			const size_t first_triangle = made->tri.size() / 3;
			for (int k = 0; k < 3; k++) made->tri.push_back(base + am.tri[t + k]);
			made->tri_tex.push_back(static_cast<uint8_t>(std::min<int>(255, first_tex + am.tri_tex[t / 3])));
			mark_part(part, first_triangle);
			/* Its window: see-through, shown with its part. */
			if (t / 3 < am.tri_glass.size() && am.tri_glass[t / 3] != 0) {
				made->tri_glass.resize(first_triangle, 0);
				made->tri_glass.push_back(1);
			}
			/* Its wheel: the corners turn about the wheel's middle (the colleague's point 10). */
			const uint16_t m = t / 3 < am.tri_mark.size() ? am.tri_mark[t / 3] : 0;
			if (m > 0 && m <= wheel_of.size()) {
				if (made->corner_wheel.size() < made->pos.size() / 3) made->corner_wheel.resize(made->pos.size() / 3, 0);
				for (int k = 0; k < 3; k++) made->corner_wheel[base + am.tri[t + k]] = wheel_of[m - 1];
			}
		}
	}
	if (!made->tri_glass.empty()) made->tri_glass.resize(made->tri.size() / 3, 0);
	if (!made->corner_wheel.empty()) made->corner_wheel.resize(made->pos.size() / 3, 0);
	if (!made->tex_addon.empty()) made->tex_addon.resize(made->pngs.size(), -1);
	if (!made->tri_part.empty()) made->tri_part.resize(made->tri.size() / 3, 0);
	made->x0 = made->y0 = made->z0 = 1e9f;
	made->x1 = made->y1 = made->z1 = -1e9f;
	for (size_t i = 0; i + 2 < extent_floats; i += 3) {
		made->x0 = std::min(made->x0, made->pos[i]); made->x1 = std::max(made->x1, made->pos[i]);
		made->y0 = std::min(made->y0, made->pos[i + 1]); made->y1 = std::max(made->y1, made->pos[i + 1]);
		made->z0 = std::min(made->z0, made->pos[i + 2]); made->z1 = std::max(made->z1, made->pos[i + 2]);
	}
	Debug(misc, 1, "3D vehicle {}{}: {} triangles ({} of them wheels of their own files), {} corners{}, {} textures, {:.2f} x {:.2f} x {:.2f} m, read in {} ms", this->name,
			this->frame_prefix.empty() ? std::string() : fmt::format(" frame {}", frame), made->tri.size() / 3, wheel_triangles,
			made->pos.size() / 3, made->nrm.empty() ? " without normals" : "", made->pngs.size(), made->x1 - made->x0, made->y1 - made->y0, made->z1 - made->z0,
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
	ShrinkShape(*made);
	ExtentOfAll(*made);
	KeepVehicleShape(key, made);
	return made;
}

/**
 * The picture of the model a mask or a paint of its own file lays over (TextureLevelsOfFile()): its body's (the first
 * texture) or an add-on's, nullptr not known.
 * @param addon -1 the body, else the add-on
 */
static ThreeDTextureBytes PictureUnder(const ThreeDVehicleShape *s, int addon)
{
	if (s == nullptr) return nullptr;
	if (addon < 0) return s->pngs.empty() ? nullptr : s->pngs[0];
	for (size_t i = 0; i < s->pngs.size() && i < s->tex_addon.size(); i++) {
		if (s->tex_addon[i] == addon) return s->pngs[i];
	}
	return nullptr;
}

/** The size of a picture from its first bytes: a PNG's header, or the mark of pixels as they are (RAW_TEXTURE_MARK). */
static bool ImageSizeOfHead(std::span<const uint8_t> head, int &w, int &h)
{
	static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	auto be32 = [&head](size_t at) { return static_cast<uint32_t>(head[at]) << 24 | head[at + 1] << 16 | head[at + 2] << 8 | head[at + 3]; };
	if (head.size() >= 24 && std::equal(std::begin(PNG_SIGNATURE), std::end(PNG_SIGNATURE), head.begin())) {
		w = static_cast<int>(std::min<uint32_t>(be32(16), 1 << 16));
		h = static_cast<int>(std::min<uint32_t>(be32(20), 1 << 16));
		return w > 0 && h > 0;
	}
	const size_t at = RAW_TEXTURE_MARK.size();
	if (head.size() >= at + 8 && std::equal(RAW_TEXTURE_MARK.begin(), RAW_TEXTURE_MARK.end(), head.begin())) {
		w = static_cast<int>(std::min<uint32_t>(ReadU32(head, at), 1 << 16));
		h = static_cast<int>(std::min<uint32_t>(ReadU32(head, at + 4), 1 << 16));
		return w > 0 && h > 0;
	}
	return false;
}

/**
 * The textures made again finer in the place of others (KeepTexture()): a thread may draw from those yet, so they are
 * let go of only between the frames (ThreeDVehicleObject::TrimTextures()). Under _lazy_mutex.
 */
static std::vector<ThreeDTextureLevels> _textures_replaced;
/** The finest level of each texture drawn since the last trimming, by the texture. Under _lazy_mutex. */
static std::unordered_map<const void *, int> _texture_levels_asked;

/** Keep a texture made in the place of the one there (under _lazy_mutex): that one is kept a while yet. */
static void KeepTexture(ThreeDTextureLevels &place, ThreeDTextureLevels made)
{
	if (place != nullptr && place != made) _textures_replaced.push_back(std::move(place));
	place = std::move(made);
}

/** A level of a texture kept, or its finest there when it has not that one (a coarser one); noted as drawn. Under _lazy_mutex. */
static const ThreeDObjectTexture *LevelOfKept(const ThreeDTextureLevels &place, int level)
{
	if (place == nullptr || place->empty()) return nullptr;
	const std::vector<ThreeDObjectTexture> &lv = *place;
	auto [it, added] = _texture_levels_asked.try_emplace(&lv, level);
	if (!added) it->second = std::min(it->second, level);
	return &lv[std::clamp<int>(level, FinestLevel(lv), static_cast<int>(lv.size()) - 1)];
}

/**
 * One of the object's textures at a level of detail, made the first time and again when a finer level is wanted than it
 * has (the textures by need): the body's (0) is the paint ("textura") when there is one, else the file's; the others
 * the file's and the wheels'.
 */
const ThreeDObjectTexture *ThreeDVehicleObject::GetTexture(int level, int index) const
{
	MarkThreeDUsed(this->used);
	const ThreeDVehicleShape *s = this->GetShape();
	const size_t count = s != nullptr ? std::max<size_t>(1, s->pngs.size()) : 1;
	if (index < 0 || static_cast<size_t>(index) >= count) index = 0;
	level = std::max(level, 0);
	MakeOnce(this, LAZY_TEXTURE + index, [this, count, index, level] {
		if (this->levels.size() < count) this->levels.resize(count);
		return this->levels[index] != nullptr && HasLevel(*this->levels[index], level);
	}, [this, s, index, level] {
		ThreeDTextureLevels lv;
		ThreeDTextureBytes png;
		uint32_t flat = 0;
		if (index == 0 && !this->paint_file.empty()) lv = TextureLevelsOfFile(this->paint_file, 0, PictureUnder(s, -1), level);
		if (lv == nullptr && s != nullptr && static_cast<size_t>(index) < s->pngs.size()) {
			png = s->pngs[index];
			flat = s->flats[index];
		}
		if (lv == nullptr) lv = ThreeDTextureLevelsOf(png, flat, level);
		return lv;
	}, [this, index](ThreeDTextureLevels made) { KeepTexture(this->levels[index], std::move(made)); });
	std::lock_guard lock(_lazy_mutex);
	return static_cast<size_t>(index) < this->levels.size() ? LevelOfKept(this->levels[index], level) : nullptr;
}

/**
 * One of the object's textures at a level of detail if it is made already; else made in the background, and till
 * then its finest level there is (a coarser one), or nullptr when there is none.
 */
const ThreeDObjectTexture *ThreeDVehicleObject::TryGetTexture(int level, int index) const
{
	MarkThreeDUsed(this->used);
	const ThreeDVehicleShape *s = this->TryGetShape();
	if (s == nullptr && !this->ShapeKnown()) return nullptr;
	const size_t count = s != nullptr ? std::max<size_t>(1, s->pngs.size()) : 1;
	if (index < 0 || static_cast<size_t>(index) >= count) index = 0;
	level = std::max(level, 0);
	MadeOrAsked(this, LAZY_TEXTURE + index, [this, index, level] { return static_cast<size_t>(index) < this->levels.size() && this->levels[index] != nullptr && HasLevel(*this->levels[index], level); }, [this, level, index] { this->GetTexture(level, index); });
	std::lock_guard lock(_lazy_mutex);
	return static_cast<size_t>(index) < this->levels.size() ? LevelOfKept(this->levels[index], level) : nullptr;
}

/**
 * The size of one of the object's textures at its finest without making it: of the texture made, else of its picture
 * (the header of the PNG). How fine a level a picture of the object needs (TexelsPerMetre() of three_d_vehicle_object.cpp).
 * @return false: not known (1 x 1 then)
 */
bool ThreeDVehicleObject::TextureSize(int index, int &w, int &h) const
{
	w = h = 1;
	ThreeDTextureBytes png;
	{
		std::lock_guard lock(_lazy_mutex);
		if (index >= 0 && static_cast<size_t>(index) < this->levels.size() && this->levels[index] != nullptr && !this->levels[index]->empty()) {
			w = this->levels[index]->front().w;
			h = this->levels[index]->front().h;
			return true;
		}
		/* The shape read: a figure that moves has its frames' only (they share their pictures). */
		const ThreeDVehicleShape *s = this->shape.get();
		for (size_t f = 0; s == nullptr && f < this->frame_shapes.size(); f++) s = this->frame_shapes[f].get();
		if (s != nullptr && index >= 0 && static_cast<size_t>(index) < s->pngs.size()) png = s->pngs[index];
	}
	std::vector<uint8_t> head;
	if (index == 0 && !this->paint_file.empty()) {
		head = ReadFileHead(this->paint_file, 32);
	} else if (png != nullptr) {
		head.assign(png->begin(), png->begin() + std::min<size_t>(png->size(), 32));
	}
	return ImageSizeOfHead(head, w, h);
}

/** The rust mask ("rez": "maska") at a level of detail, its grey in the red; nullptr without one. */
const ThreeDObjectTexture *ThreeDVehicleObject::GetRustMask(int level) const
{
	if (this->rust.mask_file.empty()) return nullptr;
	level = std::max(level, 0);
	MakeOnce(this, LAZY_RUST_MASK, [this, level] { return this->rust_tried && (this->rust_levels == nullptr || HasLevel(*this->rust_levels, level)); }, [this, level] { return MaskLevelsOfFile(this->rust.mask_file, PictureUnder(this->GetShape(), -1), level); }, [this](ThreeDTextureLevels made) {
		KeepTexture(this->rust_levels, std::move(made));
		this->rust_tried = true;
	});
	std::lock_guard lock(_lazy_mutex);
	return LevelOfKept(this->rust_levels, level);
}

/** The paint mask ("lak": "maska") at a level of detail, its grey in the red; nullptr without one. */
const ThreeDObjectTexture *ThreeDVehicleObject::GetPaintMask(int level) const
{
	if (this->paint.mask_file.empty()) return nullptr;
	level = std::max(level, 0);
	MakeOnce(this, LAZY_PAINT_MASK, [this, level] { return this->mask_tried && (this->mask_levels == nullptr || HasLevel(*this->mask_levels, level)); }, [this, level] { return MaskLevelsOfFile(this->paint.mask_file, PictureUnder(this->GetShape(), -1), level); }, [this](ThreeDTextureLevels made) {
		KeepTexture(this->mask_levels, std::move(made));
		this->mask_tried = true;
	});
	std::lock_guard lock(_lazy_mutex);
	return LevelOfKept(this->mask_levels, level);
}

/** An add-on's colour mask ("doplnky" "barva" "maska") at a level of detail, its grey in the red; nullptr without one. */
const ThreeDObjectTexture *ThreeDVehicleObject::GetAddonMask(size_t addon, int level) const
{
	if (addon >= this->addons.size() || this->addons[addon].mask_file.empty()) return nullptr;
	level = std::max(level, 0);
	MakeOnce(this, LAZY_ADDON_MASK + static_cast<int>(addon), [this, addon, level] {
		if (this->addon_mask_levels.size() < this->addons.size()) {
			this->addon_mask_levels.resize(this->addons.size());
			this->addon_mask_tried.resize(this->addons.size(), false);
		}
		return this->addon_mask_tried[addon] && (this->addon_mask_levels[addon] == nullptr || HasLevel(*this->addon_mask_levels[addon], level));
	}, [this, addon, level] { return MaskLevelsOfFile(this->addons[addon].mask_file, PictureUnder(this->GetShape(), static_cast<int>(addon)), level); }, [this, addon](ThreeDTextureLevels made) {
		KeepTexture(this->addon_mask_levels[addon], std::move(made));
		this->addon_mask_tried[addon] = true;
	});
	std::lock_guard lock(_lazy_mutex);
	return addon < this->addon_mask_levels.size() ? LevelOfKept(this->addon_mask_levels[addon], level) : nullptr;
}

/**
 * Between the frames (ForgetUnusedThreeDObjects()): of each of the object's textures and masks not trimmed yet the
 * levels finer than any drawn since the last time let go of -- of one not drawn at all, all but those of 256 texels
 * and less. Under _lazy_mutex.
 * @param seen the textures trimmed already (they are shared)
 * @return the bytes let go of
 */
size_t ThreeDVehicleObject::TrimTextures(std::set<const void *> &seen) const
{
	size_t freed = 0;
	auto trim = [&](const ThreeDTextureLevels &place) {
		if (place == nullptr || place->empty() || !seen.insert(place.get()).second) return;
		/* Nothing draws from it now (between the frames): its pixels may go. */
		std::vector<ThreeDObjectTexture> &lv = const_cast<std::vector<ThreeDObjectTexture> &>(*place);
		int keep = static_cast<int>(lv.size()) - 1;
		auto it = _texture_levels_asked.find(&lv);
		if (it != _texture_levels_asked.end()) {
			keep = std::min(keep, it->second);
		} else {
			while (keep > 0 && std::max(lv[keep - 1].w, lv[keep - 1].h) <= 256) keep--;
		}
		for (int i = 0; i < keep; i++) freed += lv[i].px.capacity() * sizeof(uint32_t);
		DropFinerLevels(lv, keep);
	};
	for (const ThreeDTextureLevels &place : this->levels) trim(place);
	trim(this->mask_levels);
	trim(this->rust_levels);
	for (const ThreeDTextureLevels &place : this->addon_mask_levels) trim(place);
	return freed;
}

/** Between the frames: the textures replaced let go of, those of the vehicles and the figures trimmed (TrimTextures()). */
static void TrimThreeDTextures()
{
	std::lock_guard lock(_lazy_mutex);
	_textures_replaced.clear();
	_shapes_replaced.clear();
	std::set<const void *> seen;
	size_t freed = 0;
	for (std::vector<ThreeDVehicleObject> *list : {&_vehicle_objects, &_figure_objects}) {
		for (const ThreeDVehicleObject &v : *list) freed += v.TrimTextures(seen);
	}
	_texture_levels_asked.clear();
	if (freed > 0) Debug(misc, 1, "3D textures: {:.1f} MB of levels finer than drawn let go of", freed / (1024.0 * 1024.0));
}

/**
 * A mask of details (ThreeDDetailMask, "maska_podrobnosti"), read whole the first time: its red as the mask index.
 * @param addon -1 the body's (its first texture), else the add-on's
 * @return nullptr without one
 */
const ThreeDDetailMask *ThreeDVehicleObject::GetDetailMask(int addon) const
{
	const size_t at = static_cast<size_t>(addon + 1);
	if (addon < -1 || at > this->addons.size()) return nullptr;
	const std::string &file = addon < 0 ? this->detail_file : this->addons[addon].detail_file;
	if (file.empty()) return nullptr;
	MakeOnce(this, LAZY_DETAIL_MASK + static_cast<int>(at), [this, at] {
		if (this->detail_masks.size() <= this->addons.size()) {
			this->detail_masks.resize(this->addons.size() + 1);
			this->detail_tried.resize(this->addons.size() + 1, false);
		}
		return static_cast<bool>(this->detail_tried[at]);
	}, [this, &file, addon] {
		std::shared_ptr<const ThreeDDetailMask> mask = DetailMaskOfFile(file, PictureUnder(this->GetShape(), addon));
		if (mask == nullptr) Debug(misc, 0, "3D vehicle {}: the mask of details {} cannot be read", this->name, file);
		return mask;
	}, [this, at](std::shared_ptr<const ThreeDDetailMask> made) {
		this->detail_masks[at] = std::move(made);
		this->detail_tried[at] = true;
	});
	return this->detail_masks[at].get();
}

/** Whether any of its textures has a mask of details: then its states carry the vehicle's colours of its set (ComposeState()). */
bool ThreeDVehicleObject::HasDetails() const
{
	if (!this->detail_file.empty()) return true;
	return std::ranges::any_of(this->addons, [](const ThreeDVehicleAddon &a) { return !a.detail_file.empty(); });
}

/**
 * Its length, metres: "rozmer_m" when the index gives it (a lorry's main file is only its length, PROSBA point 4 --
 * and the length is asked for every engine of a set the moment it is loaded, long before any is drawn: its add-ons
 * are not read for it), else its shape's.
 */
float ThreeDVehicleObject::Length() const
{
	if (this->length_m > 0) return this->length_m;
	const ThreeDVehicleShape *shape = this->GetShape();
	return shape != nullptr ? shape->x1 - shape->x0 : 0.0f;
}

/**
 * The parts drawn in a state (its step, StateStep() of three_d_vehicle_object.cpp: 0 none): what is always drawn (bit
 * 0) and what the state shows -- an add-on's node, a group of the crew. Without a state no add-on and no group.
 */
ThreeDParts ThreeDVehicleObject::PartsOf(int state) const
{
	const ThreeDVehicleState *s = this->State(state);
	ThreeDParts parts = s != nullptr ? s->parts : ThreeDParts{};
	if (s != nullptr && s->empty) {
		/* Standing empty: none of its crew, the record's driver neither (bit 0). */
		if (s->crew_part > 0) parts.Clear(s->crew_part);
		return parts;
	}
	/* Bit 0: the record's driver drawn -- not where the state's group of the crew takes the driver's seat. */
	if (s == nullptr || s->crew_part <= 0 || !this->seat_groups.Has(s->crew_part)) parts.Set(0);
	if (this->crew_record_part > 0) parts.Set(this->crew_record_part);
	return parts;
}

/** The first of its states drawn for the GRF's set so named ("sada"), 0 none (as it is). */
int ThreeDVehicleObject::StateNamed(std::string_view set) const
{
	for (size_t i = 0; i < this->states.size(); i++) {
		if (this->states[i].set_name == set) return static_cast<int>(i + 1);
	}
	return 0;
}

/**
 * A state as another (0 as it is) but standing empty: none of its crew drawn (the player, 18. 10.: "on the pitch stands
 * an empty Tatra, without a driver"). Made the first time, a step after its states as ComposeState() makes them.
 */
int ThreeDVehicleObject::EmptyStateOf(int step) const
{
	ThreeDVehicleState made;
	if (const ThreeDVehicleState *s = this->State(step); s != nullptr) {
		std::lock_guard lock(_lazy_mutex);
		auto it = this->empty_steps.find(step);
		if (it != this->empty_steps.end()) return it->second;
		made = *s;
	} else {
		std::lock_guard lock(_lazy_mutex);
		auto it = this->empty_steps.find(0);
		if (it != this->empty_steps.end()) return it->second;
		made.addon_parts.assign(this->addons.size(), -1);
		made.addon_colours.assign(this->addons.size(), 0);
	}
	made.empty = true;
	std::lock_guard lock(_lazy_mutex);
	auto [it, added] = this->empty_steps.emplace(step, 0);
	if (!added) return it->second;
	this->composed.push_back(std::move(made));
	it->second = static_cast<int>(this->states.size() + this->composed.size());
	return it->second;
}

/**
 * A state by its step (StateStep() of three_d_vehicle_object.cpp): 1 to the count of "stavy" those, after them the
 * states made of the states of layers (ComposeState()); nullptr for 0, none.
 */
const ThreeDVehicleState *ThreeDVehicleObject::State(int step) const
{
	if (step <= 0) return nullptr;
	if (static_cast<size_t>(step) <= this->states.size()) return &this->states[step - 1];
	const size_t k = static_cast<size_t>(step) - this->states.size() - 1;
	/* The composed ones under the lock: made on the main thread as the pictures are made on theirs (a state once made stays where it is). */
	std::lock_guard lock(_lazy_mutex);
	return k < this->composed.size() ? &this->composed[k] : nullptr;
}

/**
 * The state of the states the GRF picks for the layers of its sprites (the colleague's hra/3d/PROSBA-SPECIAL-3D.md):
 * the keys of a later layer replace those of an earlier one, the add-ons' nodes ("doplnky") and colours
 * ("barvy_doplnku") each add-on by itself. Made the first time, kept by the steps of the layers.
 * The vehicle's colours of its set (callback 1C1, true_colour.h) go into the state made, for its masks of details
 * (the colleague's hra/3d/PROSBA-NAKLADAKY-TATRA-3D.md, point 2): a vehicle in other colours is another state, so
 * its pictures are made apart (ObjectPicture()) as its sprites are in 2D.
 * @param layers the step of each layer's state, the first layer first, 0 for a layer of no state
 * @param true_colours the vehicle's colours (GetVehicleTrueColours()), 0 none
 * @return its step: the one state itself when only one layer has a state and there are no colours, 0 when there is nothing
 */
int ThreeDVehicleObject::ComposeState(const std::vector<int> &layers, uint16_t true_colours) const
{
	int only = 0, count = 0;
	for (int step : layers) {
		if (step > 0 && static_cast<size_t>(step) <= this->states.size()) {
			only = step;
			count++;
		}
	}
	if (count <= 1 && true_colours == 0) return only;
	std::lock_guard lock(_lazy_mutex);
	auto it = this->composed_steps.find({layers, true_colours});
	if (it != this->composed_steps.end()) return it->second;
	ThreeDVehicleState made;
	made.true_colours = true_colours;
	made.addon_parts.assign(this->addons.size(), -1);
	made.addon_colours.assign(this->addons.size(), 0);
	for (int step : layers) {
		if (step <= 0 || static_cast<size_t>(step) > this->states.size()) continue;
		const ThreeDVehicleState &s = this->states[step - 1];
		if (s.given & ThreeDVehicleState::GIVEN_COLOUR) made.colour = s.colour;
		if (s.given & ThreeDVehicleState::GIVEN_FADE) made.fade = s.fade;
		if (s.given & ThreeDVehicleState::GIVEN_RUST) made.rust = s.rust;
		if (s.given & ThreeDVehicleState::GIVEN_TURN) made.turn = s.turn;
		made.given |= s.given;
		for (size_t a = 0; a < s.addon_parts.size() && a < made.addon_parts.size(); a++) {
			if (s.addon_parts[a] >= 0) made.addon_parts[a] = s.addon_parts[a];
		}
		for (size_t a = 0; a < s.addon_colours.size() && a < made.addon_colours.size(); a++) {
			if (s.addon_colours[a] != 0) made.addon_colours[a] = s.addon_colours[a];
		}
		if (s.crew_part >= 0) made.crew_part = s.crew_part;
	}
	for (int16_t part : made.addon_parts) {
		if (part > 0) made.parts.Set(part);
	}
	if (made.crew_part > 0) made.parts.Set(made.crew_part);
	this->composed.push_back(std::move(made));
	const int step = static_cast<int>(this->states.size() + this->composed.size());
	this->composed_steps.emplace(std::make_pair(layers, true_colours), step);
	return step;
}

/**
 * The crew that walks as the vehicle goes (ThreeDWalkingCrew): of its crew and its groups the figures whose frames are
 * steps ("animace" with "chuze"), each drawn on its own, not in its shape (MakeShape()).
 */
const std::vector<ThreeDWalkingCrew> &ThreeDVehicleObject::WalkingCrew() const
{
	MakeOnce(this, LAZY_WALKING, [this] { return this->walking_found; }, [this] {
		std::vector<ThreeDWalkingCrew> found;
		auto add = [&](const ThreeDVehicleCrew &c, uint8_t part) {
			const ThreeDVehicleObject *fig = GetThreeDFigure(c.figure, this->package);
			if (fig == nullptr || !fig->Walks()) return;
			ThreeDWalkingCrew w;
			w.figure = fig;
			std::copy(std::begin(c.at), std::end(c.at), w.at);
			w.turn = c.turn;
			w.scale = c.scale;
			w.part = part;
			found.push_back(w);
		};
		for (const ThreeDVehicleCrew &c : this->crew) add(c, static_cast<uint8_t>(this->crew_record_part));
		for (const auto &[group, list] : this->crew_groups) {
			auto p = std::ranges::find(this->part_names, "posadky/" + group);
			if (p == this->part_names.end() || p == this->part_names.begin() || p - this->part_names.begin() >= ThreeDParts::COUNT) continue;
			for (const ThreeDVehicleCrew &c : list) add(c, static_cast<uint8_t>(p - this->part_names.begin()));
		}
		return found;
	}, [this](std::vector<ThreeDWalkingCrew> made) {
		this->walking = std::move(made);
		this->walking_found = true;
	});
	return this->walking;
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
		std::string lengths;
		for (uint16_t p : o.pieces) {
			EngineID id = _engine_mngr.GetID(o.type, p, o.grfid);
			engines += id == EngineID::Invalid() ? " -" : fmt::format(" {}", id.base());
			if (id != EngineID::Invalid()) lengths += fmt::format(" {}", ThreeDVehicleObjectPieceLength(id, 8));
		}
		out += fmt::format("\n  {} grf {:08X} type {} pieces{} (engines{}) anchor {}{}{}{}", o.name, std::byteswap(o.grfid), to_underlying(o.type), pieces, engines, o.anchor, o.paint_file.empty() ? "" : " paint",
				o.size.param < 0 ? "" : fmt::format(" size param {}", o.size.param), lengths.empty() ? "" : fmt::format(" lengths{}", lengths));
		if (!o.paint.mask_file.empty()) out += fmt::format(" paint mask {} fades {} start {}", o.paint.mask_file.substr(o.paint.mask_file.rfind('/') + 1), o.paint.fades, o.paint.start);
		if (!o.states.empty()) out += fmt::format(" states {}", o.states.size());
		/* The package it is of, and whether its pieces are drawn from it (of two versions, the one beside the GRF loaded). */
		auto drawn = o.pieces.empty() ? _vehicle_object_pieces.end() : _vehicle_object_pieces.find(VehiclePieceKey(o.grfid, o.type, o.pieces[0]));
		const bool used = drawn != _vehicle_object_pieces.end() && &_vehicle_objects[drawn->second.first] == &o;
		out += fmt::format(" package {}{}", o.package, used ? "" : " (not used)");
	}
	return out;
}

/**
 * The 3D object of a house of the original game, if there is one.
 * @param north_house the house of its north tile
 * @param colour_slot the colour variant of its north tile (TileHash2Bit() of the tile)
 */
/**
 * The 3D object of one of the game's own industries ("druh": "prumysl"), or nullptr.
 * @param variant the statue of bronze (the industry's random bit 0), else of stone
 */
const ThreeDObject *GetThreeDIndustryObject(IndustryType type, bool variant)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	auto it = _three_d_industry_objects.find(std::make_pair(type, static_cast<int8_t>(variant ? 1 : 0)));
	if (it == _three_d_industry_objects.end()) it = _three_d_industry_objects.find(std::make_pair(type, static_cast<int8_t>(-1)));
	return it == _three_d_industry_objects.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * The object of another kind an industry of the original game shows ("sdili": the refinery the distillery, the player,
 * 23. 10.: "the refinery and the distillery may share, it is the same plant") and where: on the square of the
 * industry's own tiles as big as the object nearest its middle, the other tiles its yard (GetThreeDIndustryYard()).
 * @param[out] square the square's north tile
 * @return the object, nullptr none or no square of the industry's tiles big enough
 */
static const ThreeDObject *SharedIndustryObject(const Industry *ind, TileIndex &square)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	auto it = _three_d_shared_industry_objects.find(ind->type);
	if (it == _three_d_shared_industry_objects.end() || GetIndustrySpec(ind->type)->grf_prop.HasGrfFile()) return nullptr;
	const ThreeDObject *obj = &_three_d_objects[it->second];
	const int w = ind->location.w, h = ind->location.h;
	double best = -1;
	for (int y0 = 0; y0 + obj->tiles_y <= h; y0++) {
		for (int x0 = 0; x0 + obj->tiles_x <= w; x0++) {
			bool own = true;
			for (int y = 0; y < obj->tiles_y && own; y++) {
				for (int x = 0; x < obj->tiles_x && own; x++) {
					const TileIndex t = TileAddXY(ind->location.tile, x0 + x, y0 + y);
					own = IsTileType(t, TileType::Industry) && GetIndustryIndex(t) == ind->index;
				}
			}
			if (!own) continue;
			const double off = std::hypot(x0 + obj->tiles_x / 2.0 - w / 2.0, y0 + obj->tiles_y / 2.0 - h / 2.0);
			if (best >= 0 && off >= best) continue;
			best = off;
			square = TileAddXY(ind->location.tile, x0, y0);
		}
	}
	return best >= 0 ? obj : nullptr;
}

/**
 * The 3D object an industry is drawn as: its type's, if the industry is as big as the object -- a pub of one tile
 * built before the pub was two by two (the colleague, 16. 10.: "the pub twice as big, on 2 x 2 tiles"), or a
 * coffeeshop of one tile from before the hut, stays as it was drawn then rather than lie over its neighbours.
 */
const ThreeDObject *GetThreeDIndustryObjectOf(const Industry *ind)
{
	if (ind == nullptr) return nullptr;
	const ThreeDObject *obj = GetThreeDIndustryObject(ind->type, HasBit(ind->random, 0));
	if (obj == nullptr) {
		TileIndex square;
		return SharedIndustryObject(ind, square);
	}
	if (obj->tiles_x != ind->location.w || obj->tiles_y != ind->location.h) return nullptr;
	return obj;
}

/** The north tile of the 3D object an industry is drawn as (GetThreeDIndustryObjectOf()): its own, or the square of a shared one. */
TileIndex GetThreeDIndustryObjectOrigin(const Industry *ind)
{
	TileIndex square;
	if (GetThreeDIndustryObject(ind->type, HasBit(ind->random, 0)) == nullptr && SharedIndustryObject(ind, square) != nullptr) return square;
	return ind->location.tile;
}

/**
 * A tile of an industry drawn as a shared object outside the object's square: its own tanks and columns of the original
 * game (GetThreeDOriginalIndustryTile()) on trodden earth as the distillery's yard -- the colleague asked for asphalt,
 * the player: "away with the asphalt about the distillery, it looks silly; the refinery is the same plant, the
 * distilling of oil, tanks". nullptr for any other tile.
 */
const ThreeDIndustryTileParts *GetThreeDIndustryYard(const Industry *ind, TileIndex tile)
{
	TileIndex square;
	if (GetThreeDIndustryObject(ind->type, HasBit(ind->random, 0)) != nullptr) return nullptr;
	const ThreeDObject *obj = SharedIndustryObject(ind, square);
	if (obj == nullptr) return nullptr;
	const uint dx = TileX(tile) - TileX(square), dy = TileY(tile) - TileY(square);
	if (dx < static_cast<uint>(obj->tiles_x) && dy < static_cast<uint>(obj->tiles_y)) return nullptr;
	static std::map<IndustryGfx, ThreeDIndustryTileParts> yards;
	const IndustryGfx gfx = GetIndustryGfx(tile);
	auto [it, added] = yards.try_emplace(gfx);
	if (added) {
		if (const ThreeDIndustryTileParts *own = GetThreeDOriginalIndustryTile(gfx); own != nullptr) it->second = *own;
		it->second.ground = GetThreeDObjectByName("pv_dvur_hlina");
		it->second.member = false;
	}
	return &it->second;
}

/**
 * The girls of an industry's object that are there now (the colleague's hra/3d/PROSBA-PRUMYSL-HOLKY-NOC.md): those
 * that come with the deliveries ("postavy_s_holkami", while HasGirls()), or those of the hut standing about the yard
 * or sitting on the benches (HutGirlsAt()). Its own "postavy" (the pub's) are always there besides.
 */
std::span<const ThreeDObjectFigure> GetThreeDIndustryGirls(const Industry *ind, const ThreeDObject *obj)
{
	if (ind == nullptr || obj == nullptr) return {};
	if (!obj->girls.empty() && HasGirls(ind)) return obj->girls;
	if (!obj->hut_girls[0].empty() || !obj->hut_girls[1].empty()) {
		switch (HutGirlsAt(ind)) {
			case HutGirls::Standing: return obj->hut_girls[0];
			case HutGirls::Sitting: return obj->hut_girls[1];
			default: break;
		}
	}
	return {};
}

/** The rig: every building's event on today (test3d akce 1). */
bool _three_d_event_forced = false;

/**
 * The figures of a building's event standing about it today ("akce", the new stadium's): every 8 to 14 days one day,
 * each building on days of its own -- the days counted in blocks of 11, the event on one of the first 4 days of each
 * block, picked by the building's place and the block. Nothing on the other days.
 */
std::span<const ThreeDObjectFigure> GetThreeDHouseEvent(const ThreeDObject *obj, TileIndex origin)
{
	if (obj == nullptr || obj->event_figures.empty()) return {};
	if (_three_d_event_forced) return obj->event_figures;
	const uint32_t seed = origin.base() * 2654435761u;
	const int64_t day = static_cast<int64_t>(TimerGameCalendar::date.base()) + seed % 11;
	const uint64_t block = static_cast<uint64_t>(day / 11);
	const uint32_t pick = static_cast<uint32_t>(((block * 40503u) ^ seed) * 2246822519u) >> 28;
	if (day % 11 != static_cast<int64_t>(pick % 4)) return {};
	return obj->event_figures;
}

/** A building's object by its name in the index ("jmeno"), for the rig; nullptr when there is none. */
const ThreeDObject *GetThreeDObjectByName(std::string_view name)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	for (const ThreeDObject &o : _three_d_objects) {
		if (o.name == name) return &o;
	}
	return nullptr;
}

/** A part of the fields by its name ("dil": "zem", "cesta", "mari_vzrostle", ...), or nullptr. */
/** The street lamp standing where the game has a street light on a tile (MakeStreetLamps()): its place there, world units; nullptr none. */
const ThreeDObject *GetThreeDStreetLamp(int dx, int dy)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	auto it = _three_d_street_lamps.find(dx * 16 + dy);
	return it == _three_d_street_lamps.end() ? nullptr : &_three_d_objects[it->second];
}

const ThreeDObject *GetThreeDFieldPart(std::string_view part)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	auto it = _three_d_field_parts.find(part);
	return it == _three_d_field_parts.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * A tile of an industry of the original game drawn from the shared parts (set prumysl_vanilla): its ground, its parts,
 * its plants; nullptr when the sets have none for the gfx (Toyland).
 */
const ThreeDIndustryTileParts *GetThreeDOriginalIndustryTile(IndustryGfx gfx)
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	static std::map<IndustryGfx, ThreeDIndustryTileParts> resolved;
	static bool made = false;
	if (!made) {
		/* The objects by their places, now that all the indices are read and the list stays as it is. */
		made = true;
		for (const auto &[g, t] : _original_industry_tiles) {
			ThreeDIndustryTileParts &r = resolved[g];
			r.ground = t.ground.empty() ? nullptr : GetThreeDObjectByName(t.ground);
			if (!t.ground.empty() && r.ground == nullptr) Debug(misc, 0, "3D objects: gfx {} has no ground {}", g, t.ground);
			for (size_t i : t.parts) r.parts.push_back(&_three_d_objects[i]);
			for (const std::string &w : t.whole) {
				const ThreeDObject *o = GetThreeDObjectByName(w);
				if (o != nullptr) r.parts.push_back(o); else Debug(misc, 0, "3D objects: gfx {} has no part {}", g, w);
			}
			if (t.plants != SIZE_MAX) r.plants = &_three_d_objects[t.plants];
			r.member = t.member;
			r.anchor_dx = t.anchor_dx;
			r.anchor_dy = t.anchor_dy;
		}
	}
	auto it = resolved.find(gfx);
	return it == resolved.end() ? nullptr : &it->second;
}

/**
 * The plants of a field (wheat on the farm's fields) on a sloped tile: each lifted to the ground under it, the heights
 * of the tile's corners shared between them (an object of its own a slope, made the first time).
 */
const ThreeDObject *GetThreeDSlopedPlants(const ThreeDObject *plants, Slope slope)
{
	if (plants == nullptr || slope == SLOPE_FLAT) return plants;
	static std::map<std::pair<const ThreeDObject *, Slope>, std::unique_ptr<ThreeDObject>> sloped;
	auto &made = sloped[{plants, slope}];
	if (made == nullptr) {
		made = std::make_unique<ThreeDObject>();
		made->name = plants->name + fmt::format("/svah{}", to_underlying(slope));
		made->variants = 0;
		made->plants = plants->plants;
		/* The corners' heights in metres: north (x 0, y 0), east (x 0, y 1), south (1, 1), west (1, 0). */
		const double level = TILE_HEIGHT / OBJECT_HEIGHT_PER_METRE;
		const bool steep = IsSteepSlope(slope);
		auto corner = [&](Corner c) -> double {
			double h = (to_underlying(slope) & to_underlying(SlopeWithOneCornerRaised(c))) != 0 ? level : 0;
			if (steep && GetHighestSlopeCorner(slope) == c) h = 2 * level;
			return h;
		};
		const double n = corner(CORNER_N), e = corner(CORNER_E), s = corner(CORNER_S), w = corner(CORNER_W);
		const double side = TILE_SIZE / OBJECT_UNITS_PER_METRE;
		for (ThreeDObjectFigure &f : made->plants) {
			const double u = std::clamp(f.at[0] / side, 0.0, 1.0), v = std::clamp(f.at[1] / side, 0.0, 1.0);
			f.at[2] += static_cast<float>(n * (1 - u) * (1 - v) + e * (1 - u) * v + s * u * v + w * u * (1 - v));
		}
	}
	return made.get();
}

/**
 * A tile of the marijuana plantation (and the potato field) laid from the parts of the fields: what
 * GetThreeDPlantationTile() says is on it and on the road tile behind it.
 * @return whether the fields have their parts at all (the soil)
 */
bool GetThreeDFieldTile(const ThreeDPlantationTile &tile, ThreeDFieldTile &out)
{
	out = {};
	out.soil = GetThreeDFieldPart("zem");
	if (out.soil == nullptr) return false;
	const int crop = tile.crop & 1;
	static const char *const small[2] = {"mari_male", "brambory_male"};
	static const char *const grown[2] = {"mari_vzrostle", "brambory_vzrostle"};
	if (tile.stage == 1) out.plants = GetThreeDFieldPart(small[crop]);
	if (tile.stage == 2) out.plants = GetThreeDFieldPart(grown[crop]);
	if (tile.worker >= 0 && tile.stage != 0) {
		const std::string name = std::string("pole_") + ThreeDPlantationWorkerName(tile.worker);
		const std::vector<ThreeDObjectFigure> &work = out.soil->work[crop];
		for (size_t i = 0; i < work.size(); i++) {
			if (work[i].figure == name) out.worker = std::span(work).subspan(i, 1);
		}
	}
	if (tile.road_behind) {
		out.road = GetThreeDFieldPart("cesta");
		if (tile.shed) {
			out.shed = GetThreeDFieldPart("bouda");
			out.hay = GetThreeDFieldPart("seno");
		}
		if (out.road != nullptr) {
			if (tile.girls_nw) out.girls[0] = out.road->by_road[0];
			if (tile.girls_se) out.girls[1] = out.road->by_road[1];
		}
	}
	return true;
}

/**
 * The 3D object of an original house.
 * @param north_house the house of its north tile
 * @param colour_slot the colour variant of its north tile (TileHash2Bit() of the tile)
 * @param theme the theme of its town (TownTheme): its own object where the theme has one, else the house's as in every town
 */
const ThreeDObject *GetThreeDHouseObject(HouseID north_house, uint colour_slot, uint8_t theme)
{
	/* Kept as the first 3D of this game drew it, the boxes of its hand table, in memory of it (the player, 14. 10.:
	 * "I am nostalgic, the white house of many storeys, tall, I would keep"): the tall office block. */
	static constexpr HouseID KEPT_AS_HAND_TABLE[] = {0x1c};
	if (std::ranges::find(KEPT_AS_HAND_TABLE, north_house) != std::end(KEPT_AS_HAND_TABLE)) return nullptr;
	/* The original houses of OpenTTD wanted instead (gui.three_d_houses): no house is a 3D object. */
	if (_settings_client.gui.three_d_houses != 0) return nullptr;
	if (!_three_d_objects_read) ReadThreeDObjects();
	if (theme != 0) {
		auto themed = _three_d_house_objects.find(std::make_tuple(theme, north_house, colour_slot & 3));
		if (themed != _three_d_house_objects.end()) return &_three_d_objects[themed->second];
	}
	auto it = _three_d_house_objects.find(std::make_tuple(static_cast<uint8_t>(0), north_house, colour_slot & 3));
	return it == _three_d_house_objects.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * The 3D object of any house, an original's or a set's (NewGRF): a set's house by the set's id and its number in
 * the set (the Mars houses, the game's own Mars towns).
 * @param north_house the house of its north tile
 * @param colour_slot the colour variant of its north tile (TileHash2Bit() of the tile)
 */
const ThreeDObject *GetThreeDHouseObjectAny(HouseID north_house, uint colour_slot, uint8_t theme)
{
	if (north_house < NEW_HOUSE_OFFSET) return GetThreeDHouseObject(north_house, colour_slot, theme);
	if (_settings_client.gui.three_d_houses != 0) return nullptr;
	if (!_three_d_objects_read) ReadThreeDObjects();
	const HouseSpec *hs = HouseSpec::Get(north_house);
	if (hs == nullptr || hs->grf_prop.grffile == nullptr) return nullptr;
	auto it = _three_d_grf_house_objects.find(std::make_tuple(hs->grf_prop.grfid, hs->grf_prop.local_id, colour_slot & 3));
	return it == _three_d_grf_house_objects.end() ? nullptr : &_three_d_objects[it->second];
}

/**
 * The 3D object of the house standing on a tile, as its town looks (Town::theme): the clay town's clay house.
 * @param north_house the house of its north tile
 * @param origin its north tile
 */
const ThreeDObject *GetThreeDHouseObjectAt(HouseID north_house, TileIndex origin)
{
	uint8_t theme = 0;
	if (IsTileType(origin, TileType::House)) {
		const Town *t = Town::GetIfValid(GetTownIndex(origin));
		if (t != nullptr) theme = to_underlying(t->theme);
	}
	return GetThreeDHouseObjectAny(north_house, TileHash2Bit(TileX(origin) * TILE_SIZE, TileY(origin) * TILE_SIZE), theme);
}

/** The trees of a town theme (TownTheme, "druh": "strom" with its "tema"): the names of their figures, none for the plain towns. */
std::span<const std::string> GetThreeDThemeTrees(uint8_t theme, bool night)
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	const auto &trees = night ? _three_d_theme_trees_night : _three_d_theme_trees;
	auto it = trees.find(theme);
	if (it == trees.end()) return {};
	return it->second;
}

/** The creatures of the indices ("druh": "tvor", the colleague's dragons), read the first time they are asked for. */
const std::vector<ThreeDCreature> &GetThreeDCreatures()
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	return _creatures;
}

/** The witches of the indices ("druh": "carodejnice"), read the first time they are asked for. */
const std::vector<ThreeDWitch> &GetThreeDWitches()
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	return _witches;
}

/** The dogs, the animals, the ghosts and the sellers of dogs of the indices, read the first time they are asked for. */
const std::vector<ThreeDAnimal> &GetThreeDAnimals()
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	return _animals;
}

/** One of them by its name, or nullptr. */
const ThreeDAnimal *GetThreeDAnimal(std::string_view name)
{
	for (const ThreeDAnimal &a : GetThreeDAnimals()) {
		if (a.name == name) return &a;
	}
	return nullptr;
}

/** Its name in Czech or else in English. */
const std::string &ThreeDAnimal::Title() const
{
	return std::string_view(_current_language->isocode).starts_with("cs") ? this->title_cs : this->title_en;
}

/** The mushrooms to pick of the indices, read the first time they are asked for. */
const std::vector<ThreeDMushroom> &GetThreeDMushrooms()
{
	if (!_vehicle_objects_read) ReadThreeDVehicleObjects();
	return _mushrooms;
}

/**
 * The name the land area information shows for a house that is drawn as a 3D object with a name of its own ("nazev"),
 * in Czech or else in English; none with the 3D off, while it is built, or for a house without.
 * @param tile a tile of the house
 */
std::optional<std::string> GetThreeDHouseTitle(TileIndex tile)
{
	if (!ThreeDEnabled() || !IsHouseCompleted(tile)) return std::nullopt;
	HouseID north = GetHouseType(tile);
	TileIndex origin = tile + GetHouseNorthPart(north);
	const ThreeDObject *obj = GetThreeDHouseObjectAt(north, origin);
	if (obj == nullptr || obj->title_en.empty()) return std::nullopt;
	return std::string_view(_current_language->isocode).starts_with("cs") ? obj->title_cs : obj->title_en;
}

/**
 * A texture's levels of detail: level 0 as in the PNG, every next one halved (the colour of the pixels that have one,
 * a pixel transparent when most of its four are). A texture that cannot be read is a plain grey, so the shape still
 * shows.
 */
/**
 * Which texels of a texture its model's triangles draw from: those whose middles are in a triangle of the cover
 * (TextureCover()), and the one under each triangle's middle (a triangle smaller than a texel).
 */
static std::vector<uint8_t> TextureTexelsDrawn(int w, int h, const std::vector<float> &cover)
{
	std::vector<uint8_t> drawn(static_cast<size_t>(w) * h, 0);
	/* A triangle over more than the texture repeated draws from all of it: no texel to fill (a 512 x 1024 of the
	 * colleague's took 300 ms, each such triangle tried at every texel). */
	for (size_t t = 0; t + 5 < cover.size(); t += 6) {
		const float u0 = std::min({cover[t], cover[t + 2], cover[t + 4]}), u1 = std::max({cover[t], cover[t + 2], cover[t + 4]});
		const float v0 = std::min({cover[t + 1], cover[t + 3], cover[t + 5]}), v1 = std::max({cover[t + 1], cover[t + 3], cover[t + 5]});
		if (u1 - u0 >= 1.0f && v1 - v0 >= 1.0f) {
			std::fill(drawn.begin(), drawn.end(), 1);
			return drawn;
		}
	}
	auto mark = [&](double x, double y) {
		const int px = static_cast<int>(std::floor(x)), py = static_cast<int>(std::floor(y));
		if (px >= 0 && py >= 0 && px < w && py < h) drawn[static_cast<size_t>(py) * w + px] = 1;
	};
	for (size_t t = 0; t + 5 < cover.size(); t += 6) {
		double x[3], y[3];
		for (int k = 0; k < 3; k++) {
			x[k] = cover[t + k * 2] * w;
			y[k] = cover[t + k * 2 + 1] * h;
		}
		/* A texture repeated: the triangle where its first corner is. */
		const double ox = std::floor(x[0] / w) * w, oy = std::floor(y[0] / h) * h;
		for (int k = 0; k < 3; k++) {
			x[k] -= ox;
			y[k] -= oy;
		}
		mark((x[0] + x[1] + x[2]) / 3, (y[0] + y[1] + y[2]) / 3);
		const double area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
		if (std::abs(area) < 1e-9) continue;
		/* Row by row: where the row's middle crosses the triangle's edges, the texels whose middles are between them
		 * at once (each texel tried was the most of the time of an atlas of the colleague's). */
		const int y0 = std::max(0, static_cast<int>(std::floor(std::min({y[0], y[1], y[2]})))), y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]}))));
		for (int py = y0; py <= y1; py++) {
			const double cy = py + 0.5;
			double lo = std::numeric_limits<double>::infinity(), hi = -std::numeric_limits<double>::infinity();
			for (int k = 0; k < 3; k++) {
				const double ax = x[k], ay = y[k], bx = x[(k + 1) % 3], by = y[(k + 1) % 3];
				if (cy < std::min(ay, by) - 1e-9 || cy > std::max(ay, by) + 1e-9) continue;
				if (std::abs(by - ay) < 1e-12) {
					lo = std::min({lo, ax, bx});
					hi = std::max({hi, ax, bx});
					continue;
				}
				const double cx = ax + (bx - ax) * (cy - ay) / (by - ay);
				lo = std::min(lo, cx);
				hi = std::max(hi, cx);
			}
			if (!(lo <= hi)) continue;
			const int px0 = std::max(0, static_cast<int>(std::ceil(lo - 0.5 - 1e-6))), px1 = std::min(w - 1, static_cast<int>(std::floor(hi - 0.5 + 1e-6)));
			if (px0 <= px1) std::fill_n(drawn.begin() + static_cast<size_t>(py) * w + px0, px1 - px0 + 1, 1);
		}
	}
	return drawn;
}

/** How far from the texels drawn from those not are filled (FillTexelsNotDrawn()): a triangle reads a texel or two past its edge, no more. */
static constexpr int FILL_BAND = 8;

/**
 * The texels no triangle draws from filled from the nearest that one does, its colour (or its index, a mask's) --
 * the colleague's atlases put every triangle on a piece of its own on black, and a triangle drew black at its edges
 * (a texel's middle just past it) and in the smaller textures of the distance (the black mixed in): the cars black
 * lines, then black from a few metres on (the player, 17. 10.). Only a band of FILL_BAND texels about them: the whole
 * of the empty atlas filled was most of the time a texture took (a 4096 x 3824 one 345 ms; the player, 17. 10.:
 * "the atlas cover, 50 to 80 ms an atlas").
 * @param drawn which texels are drawn from (TextureTexelsDrawn())
 */
static void FillTexelsNotDrawn(ThreeDObjectTexture &t, const std::vector<uint8_t> &drawn)
{
	const int w = t.w, h = t.h;
	/* Done: drawn from (a hole of the texture where it is drawn stays one, and gives nothing), or filled. */
	std::vector<uint8_t> done(drawn);
	std::vector<uint32_t> queue, next;
	/* The first: those drawn from with a colour beside one that is not. */
	for (int y = 0; y < h; y++) {
		const size_t row = static_cast<size_t>(y) * w;
		for (int x = 0; x < w; x++) {
			const size_t i = row + x;
			if (!drawn[i] || t.px[i] == 0) continue;
			if ((x > 0 && !drawn[i - 1]) || (x + 1 < w && !drawn[i + 1]) || (y > 0 && !drawn[i - w]) || (y + 1 < h && !drawn[i + w])) queue.push_back(static_cast<uint32_t>(i));
		}
	}
	for (int band = 0; band < FILL_BAND && !queue.empty(); band++) {
		next.clear();
		for (const uint32_t i : queue) {
			const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
			const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
			for (int k = 0; k < 4; k++) {
				if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
				const size_t j = static_cast<size_t>(ny[k]) * w + nx[k];
				if (done[j]) continue;
				done[j] = 1;
				t.px[j] = t.px[i];
				next.push_back(static_cast<uint32_t>(j));
			}
		}
		queue.swap(next);
	}
}

/** The texels drawn from of the last textures of a size and a cover (TextureTexelsDrawn()): a texture and its masks share the layout. */
struct TexelsDrawnKept {
	const std::vector<float> *cover;
	size_t size;     ///< and its size and a print of its numbers: another cover may come to its place
	uint64_t print;
	int w, h;
	std::shared_ptr<const std::vector<uint8_t>> drawn;
};
static std::deque<TexelsDrawnKept> _texels_drawn_kept;
static std::mutex _texels_drawn_mutex;

/** The texels drawn from (TextureTexelsDrawn()), once for a texture and the masks of its layout. */
static std::shared_ptr<const std::vector<uint8_t>> TexelsDrawnOf(int w, int h, const std::vector<float> &cover)
{
	uint64_t print = 1469598103934665603ULL;
	for (size_t i = 0; i < cover.size(); i += 97) print = (print ^ std::bit_cast<uint32_t>(cover[i])) * 1099511628211ULL;
	{
		std::lock_guard lock(_texels_drawn_mutex);
		for (const TexelsDrawnKept &k : _texels_drawn_kept) {
			if (k.cover == &cover && k.size == cover.size() && k.print == print && k.w == w && k.h == h) return k.drawn;
		}
	}
	auto made = std::make_shared<const std::vector<uint8_t>>(TextureTexelsDrawn(w, h, cover));
	std::lock_guard lock(_texels_drawn_mutex);
	_texels_drawn_kept.push_back({&cover, cover.size(), print, w, h, made});
	/* A few: they are of the textures of one object being made. */
	while (_texels_drawn_kept.size() > 3) _texels_drawn_kept.pop_front();
	return made;
}

static void DecodeTextureLevels(const std::vector<uint8_t> &png, std::vector<ThreeDObjectTexture> &levels, uint32_t flat, bool halved, const std::vector<float> *cover)
{
	ThreeDObjectTexture t;
	if (png.size() >= RAW_TEXTURE_MARK.size() + 8 && std::equal(RAW_TEXTURE_MARK.begin(), RAW_TEXTURE_MARK.end(), png.begin())) {
		/* Pixels as they are (a texture of MSTS, simtrains.h): the mark, the width and the height, then RGBA. */
		auto u32 = [&png](size_t p) { return png[p] | png[p + 1] << 8 | png[p + 2] << 16 | static_cast<uint32_t>(png[p + 3]) << 24; };
		const size_t at = RAW_TEXTURE_MARK.size();
		const uint32_t w = u32(at), h = u32(at + 4);
		if (w > 0 && h > 0 && w <= 8192 && h <= 8192 && png.size() >= at + 8 + static_cast<size_t>(w) * h * 4) {
			t.w = static_cast<int>(w);
			t.h = static_cast<int>(h);
			t.px.resize(static_cast<size_t>(w) * h);
			const uint8_t *src = png.data() + at + 8;
			for (size_t i = 0; i < t.px.size(); i++, src += 4) t.px[i] = src[3] >= 128 ? Colour(src[0], src[1], src[2]).data : 0;
		}
	}
#ifdef WITH_PNG
	if (!png.empty() && t.px.empty()) {
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
	/* Where the model draws from it, when that is known (a texture of a .glb, a mask of its layout): the rest filled. */
	std::vector<uint8_t> drawn;
	const auto cover_start = std::chrono::steady_clock::now();
	double ms_drawn = 0, ms_fill = 0;
	if (cover != nullptr && !cover->empty() && t.w > 1) {
		drawn = *TexelsDrawnOf(t.w, t.h, *cover);
		ms_drawn = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cover_start).count();
		FillTexelsNotDrawn(t, drawn);
		ms_fill = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cover_start).count() - ms_drawn;
	}
	const int w0 = t.w, h0 = t.h;
	levels.push_back(std::move(t));
	while (halved && (levels.back().w > 1 || levels.back().h > 1)) {
		const ThreeDObjectTexture &a = levels.back();
		ThreeDObjectTexture b;
		b.w = std::max(1, a.w / 2);
		b.h = std::max(1, a.h / 2);
		b.px.resize(static_cast<size_t>(b.w) * b.h);
		/* A texel of the smaller texture is the mean of the four it covers -- of those drawn from, when that is known:
		 * the filled ones are not mixed in, and it is drawn from when any of them is (then filled again). */
		std::vector<uint8_t> drawn_b;
		if (!drawn.empty()) drawn_b.assign(b.px.size(), 0);
		for (int y = 0; y < b.h; y++) {
			for (int x = 0; x < b.w; x++) {
				uint r = 0, g = 0, bl = 0, n = 0, holes = 0;
				bool any_drawn = false;
				for (int k = 0; k < 4; k++) {
					int sx = std::min(a.w - 1, x * 2 + (k & 1)), sy = std::min(a.h - 1, y * 2 + (k >> 1));
					const size_t at = static_cast<size_t>(sy) * a.w + sx;
					if (!drawn.empty() && !drawn[at]) continue;
					any_drawn = true;
					uint32_t p = a.px[at];
					if (p == 0) {
						holes++;
						continue;
					}
					Colour c(p);
					r += c.r;
					g += c.g;
					bl += c.b;
					n++;
				}
				const size_t to = static_cast<size_t>(y) * b.w + x;
				if (drawn.empty()) {
					b.px[to] = n >= 2 ? Colour(r / n, g / n, bl / n).data : 0;
				} else {
					drawn_b[to] = any_drawn ? 1 : 0;
					b.px[to] = n > 0 && n >= holes ? Colour(r / n, g / n, bl / n).data : 0;
				}
			}
		}
		if (!drawn.empty()) FillTexelsNotDrawn(b, drawn_b);
		drawn = std::move(drawn_b);
		levels.push_back(std::move(b));
	}
	if (cover != nullptr && !cover->empty() && w0 > 1) Debug(misc, 2, "3D texture {}x{}: cover of {} triangles {:.1f} ms, filled {:.1f} ms, all with the levels {:.1f} ms", w0, h0, cover->size() / 6, ms_drawn, ms_fill, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cover_start).count());
}

/** The painted textures kept, bytes; past PAINTED_BUDGET all are dropped and made again as they are drawn. */
static std::atomic<size_t> _painted_bytes = 0;
static constexpr size_t PAINTED_BUDGET = static_cast<size_t>(192) << 20;

/**
 * The bits of a part's colour in a paint (PaintAt()): four, sixteen colours a palette, for up to seven parts; three,
 * eight colours, for more (the colleague's distillery has nine parts, 23. 10.), all of them below the top bit.
 */
static uint8_t PaintBits(size_t parts)
{
	return parts <= 7 ? 4 : 3;
}

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
	const uint8_t bits = PaintBits(this->colours.size());
	for (size_t k = 0; k < this->colours.size(); k++) {
		uint32_t x = h ^ static_cast<uint32_t>(k + 1) * 0x27D4EB2Fu;
		x ^= x >> 15;
		x *= 0x2C1B3C6Du;
		x ^= x >> 12;
		x *= 0x297A2D39u;
		x ^= x >> 15;
		paint |= (x % std::min<size_t>(this->colours[k].palette.size(), 1u << bits)) << (bits * k);
	}
	/* The statue of stone or of bronze (variant_paint): its first part the colour of its variant, by a bit of the
	 * tile's hash -- of the tile only, as the rest: the frame from the eyes paints on threads of its own. */
	if (this->variant_paint[0] >= 0 || this->variant_paint[1] >= 0) {
		const int8_t c = this->variant_paint[(h >> 7) & 1];
		if (c >= 0 && c < (1 << bits) && static_cast<size_t>(c) < this->colours[0].palette.size()) paint = (paint & ~((1u << bits) - 1)) | static_cast<uint32_t>(c);
	}
	return paint;
}

/**
 * The colours of an add-on's texture of the house whose north tile is a tile (GetTextureOf()): of an add-on painted a
 * colour by number ("barvy" of its record) a colour of its palette for each part by a hash of the tile and of its place
 * in the house -- the parasols of a café each another, those of the next house others --; 0 for one not painted so.
 * @param texture the add-on's texture, 1, 2, ... (ThreeDObjectShapeView::TriangleTexture())
 */
uint32_t ThreeDObject::AddonPaintAt(TileIndex origin, uint8_t texture) const
{
	if (texture == 0 || texture > this->addon_objects.size()) return 0;
	const ThreeDObject &a = _three_d_objects[this->addon_objects[texture - 1]];
	if (a.colours.empty()) return 0;
	const uint32_t place = texture <= this->addon_places_of.size() ? this->addon_places_of[texture - 1] : 0;
	uint32_t h = TileX(origin) * 0x9E3779B1u ^ TileY(origin) * 0x85EBCA77u ^ (place + 1) * 0x165667B1u;
	uint32_t paint = 1u << 31;
	const uint8_t bits = PaintBits(a.colours.size());
	for (size_t k = 0; k < a.colours.size(); k++) {
		uint32_t x = h ^ static_cast<uint32_t>(k + 1) * 0x27D4EB2Fu;
		x ^= x >> 15;
		x *= 0x2C1B3C6Du;
		x ^= x >> 12;
		x *= 0x297A2D39u;
		x ^= x >> 15;
		paint |= (x % std::min<size_t>(a.colours[k].palette.size(), 1u << bits)) << (bits * k);
	}
	return paint;
}

/** The size of a cell of a level of the shapes (ThreeDObject::GetShape()): metres. */
static double ShapeCellMetres(int level)
{
	return 0.05 * (1 << (level - 1));
}

/**
 * The level of the shape of a building seen so near (the technologies of OpenMW, the player, 16. 10.: what is far
 * away it draws simpler): the corners merged while they move by at most so many pixels. A town of houses was half a
 * million triangles, most of them smaller than a pixel.
 * @param pixels_per_metre how many pixels of the picture a metre of the building's nearest part takes
 * @param shift how far a corner may move, pixels: the map's houses are seen sharp, those far from the eyes in haze
 */
int ThreeDShapeLevel(double pixels_per_metre, double shift)
{
	int level = 0;
	while (level + 1 < THREE_D_SHAPE_LEVELS && ShapeCellMetres(level + 1) * pixels_per_metre <= shift) level++;
	return level;
}

/** A building's shape simplified for a level (ThreeDObject::GetShape()): its corners merged in the cells of the level. */
static std::shared_ptr<const ThreeDObjectShape> MakeHouseShape(const ThreeDObject &o, int level)
{
	const size_t corners = o.pos.size() / 3;
	const bool lit = o.light.size() == corners;
	const double cell_xy = ShapeCellMetres(level) * OBJECT_UNITS_PER_METRE;
	const double cell_z = ShapeCellMetres(level) * OBJECT_HEIGHT_PER_METRE;
	/* A texture about a tile across: a corner's place in it moves as far as the corner; but never more than a
	 * 48th of it, so the parts of the texture (a wall, a roof) do not run into each other. */
	const double cell_uv = std::min(cell_xy / TILE_SIZE, 1.0 / 48);
	auto cell = [](double v, double size, int bits) {
		const int64_t half = int64_t{1} << (bits - 1);
		return static_cast<uint64_t>(std::clamp<int64_t>(static_cast<int64_t>(std::floor(v / size)) + half, 0, 2 * half - 1));
	};
	struct Sum {
		double x = 0, y = 0, z = 0, u = 0, v = 0, l = 0;
		uint32_t n = 0;
		uint8_t tex = 0;
	};
	/* The cells of each texture apart: a corner of an add-on is never one with the building's (ThreeDObject::tri_tex). */
	std::vector<std::unordered_map<uint64_t, uint32_t>> cells(o.Textures());
	std::vector<Sum> sums;
	std::vector<uint32_t> to(corners);
	for (size_t i = 0; i < corners; i++) {
		const double x = o.pos[i * 3], y = o.pos[i * 3 + 1], z = o.pos[i * 3 + 2];
		const double u = o.uv[i * 2], v = o.uv[i * 2 + 1];
		const uint8_t tex = i < o.corner_tex.size() && o.corner_tex[i] < cells.size() ? o.corner_tex[i] : 0;
		const uint64_t key = cell(x, cell_xy, 13) | cell(y, cell_xy, 13) << 13 | cell(z, cell_z, 13) << 26 | cell(u, cell_uv, 12) << 39 | cell(v, cell_uv, 12) << 51;
		auto [it, added] = cells[tex].try_emplace(key, static_cast<uint32_t>(sums.size()));
		if (added) sums.emplace_back().tex = tex;
		Sum &sum = sums[it->second];
		sum.x += x;
		sum.y += y;
		sum.z += z;
		sum.u += u;
		sum.v += v;
		sum.l += lit ? o.light[i] : 256;
		sum.n++;
		to[i] = it->second;
	}
	auto shape = std::make_shared<ThreeDObjectShape>();
	const bool textures = !o.tri_tex.empty();
	for (const Sum &sum : sums) {
		shape->pos.insert(shape->pos.end(), {static_cast<float>(sum.x / sum.n), static_cast<float>(sum.y / sum.n), static_cast<float>(sum.z / sum.n)});
		shape->uv.insert(shape->uv.end(), {static_cast<float>(sum.u / sum.n), static_cast<float>(sum.v / sum.n)});
		if (lit) shape->light.push_back(static_cast<uint16_t>(sum.l / sum.n + 0.5));
		if (textures) shape->corner_tex.push_back(sum.tex);
	}
	std::unordered_set<uint64_t> had;
	for (size_t t = 0; t + 2 < o.tri.size(); t += 3) {
		std::array<uint32_t, 3> c = {to[o.tri[t]], to[o.tri[t + 1]], to[o.tri[t + 2]]};
		if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2]) continue;
		/* The same triangle however its corners are counted round (fewer than 2^21 corners). */
		std::ranges::rotate(c, std::ranges::min_element(c));
		if (!had.insert(static_cast<uint64_t>(c[0]) | static_cast<uint64_t>(c[1]) << 21 | static_cast<uint64_t>(c[2]) << 42).second) continue;
		shape->tri.insert(shape->tri.end(), c.begin(), c.end());
		shape->facing.push_back(t / 3 < o.facing.size() ? o.facing[t / 3] : 0);
		if (textures) shape->tri_tex.push_back(t / 3 < o.tri_tex.size() ? o.tri_tex[t / 3] : 0);
	}
	return shape;
}

/**
 * The shape of a building at a level of detail. Its corners in a cell of the level's size, at about the same place in
 * the texture, are one corner: in the middle of them, their texture's place and light their average. A seam of the
 * texture stays, its corners at different places of it; a triangle with two of its corners in one cell has no area
 * and goes, and so does one already there.
 * @param level 0 its own shape, else simplified: made the first time (not on several cores at once but by
 *              PrepareThreeDObjectShapes())
 */
/**
 * Lay a building's add-ons into its shape as it is read (hra/3d/DOPLNKY-DOMU-A-POSTAVY.md, the player: "these things
 * shared by all the objects we will call add-ons"): each add-on of the index read once (its own Read()), at each of
 * its places turned about its foot, sized and stood there, its corners' light from its normals as turned (the light is
 * the world's), its triangles with its own texture (tri_tex) -- drawn among the building's own through the same depth,
 * whatever draws the building.
 * @param house the building, its places of add-ons
 * @param read what is read of it, the add-ons laid in
 */
static void LayAddons(const ThreeDObject &house, ThreeDObject &read)
{
	if (house.addon_places.empty()) return;
	read.tri_tex.assign(read.tri.size() / 3, 0);
	read.corner_tex.assign(read.pos.size() / 3, 0);
	size_t laid = 0;
	for (const ThreeDObjectFigure &place : house.addon_places) {
		auto found = _three_d_addons.find(place.figure);
		if (found == _three_d_addons.end()) {
			Debug(misc, 1, "3D object {}: no add-on '{}' in the indices", house.name, place.figure);
			continue;
		}
		const ThreeDObject &a = _three_d_objects[found->second];
		if (!a.Read() || a.tri.empty()) continue;
		/* Its texture: one for all its places; one of its own at each place when it is painted a colour by number,
		 * each place its colour (AddonPaintAt()). */
		const uint16_t place_number = a.colours.empty() ? UINT16_MAX : static_cast<uint16_t>(std::min<size_t>(&place - house.addon_places.data(), UINT16_MAX - 1));
		auto slot = read.addon_objects.end();
		if (place_number == UINT16_MAX) {
			for (size_t k = 0; k < read.addon_objects.size(); k++) {
				if (read.addon_objects[k] == found->second && read.addon_places_of[k] == UINT16_MAX) slot = read.addon_objects.begin() + k;
			}
		}
		if (slot == read.addon_objects.end()) {
			if (read.addon_objects.size() >= UINT8_MAX - 1) continue;
			read.addon_objects.push_back(found->second);
			read.addon_places_of.push_back(place_number);
			slot = read.addon_objects.end() - 1;
		}
		const uint8_t tex = static_cast<uint8_t>(1 + (slot - read.addon_objects.begin()));
		const double turn = place.facing * M_PI / 180.0, c = std::cos(turn), s = std::sin(turn);
		const uint32_t base = static_cast<uint32_t>(read.pos.size() / 3);
		const size_t corners = a.pos.size() / 3;
		/* Lit by its normals turned; a building without them lit wholly (its light is in its texture), and the other way. */
		const bool lit = a.nrm.size() == a.pos.size();
		if ((lit || !read.light.empty()) && read.light.size() != base) read.light.resize(base, 256);
		std::vector<float> turned_nrm;
		for (size_t i = 0; i < corners; i++) {
			const double x = a.pos[i * 3], y = a.pos[i * 3 + 1], z = a.pos[i * 3 + 2];
			read.pos.push_back(static_cast<float>((x * c - y * s) * place.scale + place.at[0] * OBJECT_UNITS_PER_METRE));
			read.pos.push_back(static_cast<float>((x * s + y * c) * place.scale + place.at[1] * OBJECT_UNITS_PER_METRE));
			read.pos.push_back(static_cast<float>(z * place.scale + place.at[2] * OBJECT_HEIGHT_PER_METRE));
			read.top = std::max(read.top, read.pos.back());
			read.uv.push_back(i * 2 + 1 < a.uv.size() ? a.uv[i * 2] : 0.0f);
			read.uv.push_back(i * 2 + 1 < a.uv.size() ? a.uv[i * 2 + 1] : 0.0f);
			read.corner_tex.push_back(tex);
			if (lit) {
				const float nx = a.nrm[i * 3], ny = a.nrm[i * 3 + 1];
				turned_nrm.insert(turned_nrm.end(), {static_cast<float>(nx * c - ny * s), static_cast<float>(nx * s + ny * c), a.nrm[i * 3 + 2]});
			}
		}
		if (lit) {
			const std::vector<uint16_t> light = CornerLight(turned_nrm);
			read.light.insert(read.light.end(), light.begin(), light.end());
		} else if (!read.light.empty()) {
			read.light.resize(read.pos.size() / 3, 256);
		}
		/* A tree ("kmen_r", the colleague's STROMY-SPOLECNE-PRO-HRU.md): its crown in no one's way, only its trunk. */
		if (a.trunk_r > 0) {
			read.passable_tris.push_back(static_cast<uint32_t>(read.tri.size() / 3));
			read.passable_tris.push_back(static_cast<uint32_t>((read.tri.size() + a.tri.size()) / 3));
			read.trunks.insert(read.trunks.end(), {place.at[0] * OBJECT_UNITS_PER_METRE, place.at[1] * OBJECT_UNITS_PER_METRE, a.trunk_r * place.scale * OBJECT_UNITS_PER_METRE});
		}
		/* (Turned and sized the same along each axis, a triangle keeps its outer side.) */
		for (size_t t = 0; t + 2 < a.tri.size(); t += 3) {
			read.tri.insert(read.tri.end(), {base + a.tri[t], base + a.tri[t + 1], base + a.tri[t + 2]});
			read.facing.push_back(t / 3 < a.facing.size() ? a.facing[t / 3] : 0);
			read.tri_tex.push_back(tex);
		}
		/* Its night (the colleague's lamps: their paper, glass and flame in "noc", their glows "svetlo_NN"): laid as it
		 * is, the glass in its own texture -- the paper in its colour at the place, as by day. */
		auto lay = [&](const float *p, std::vector<float> &out) {
			const double x = p[0], y = p[1];
			out.push_back(static_cast<float>((x * c - y * s) * place.scale + place.at[0] * OBJECT_UNITS_PER_METRE));
			out.push_back(static_cast<float>((x * s + y * c) * place.scale + place.at[1] * OBJECT_UNITS_PER_METRE));
			out.push_back(static_cast<float>(p[2] * place.scale + place.at[2] * OBJECT_HEIGHT_PER_METRE));
		};
		if (!a.night_tri.empty()) {
			read.night_tex.resize(read.night_tri.size() / 3, 0);
			const uint32_t night_base = static_cast<uint32_t>(read.night_pos.size() / 3);
			for (size_t i = 0; i + 2 < a.night_pos.size(); i += 3) {
				lay(&a.night_pos[i], read.night_pos);
				read.night_uv.push_back(i / 3 * 2 + 1 < a.night_uv.size() ? a.night_uv[i / 3 * 2] : 0.0f);
				read.night_uv.push_back(i / 3 * 2 + 1 < a.night_uv.size() ? a.night_uv[i / 3 * 2 + 1] : 0.0f);
			}
			for (size_t t = 0; t + 2 < a.night_tri.size(); t += 3) {
				read.night_tri.insert(read.night_tri.end(), {night_base + a.night_tri[t], night_base + a.night_tri[t + 1], night_base + a.night_tri[t + 2]});
				read.night_tex.push_back(tex);
			}
		}
		for (size_t i = 0; i + 2 < a.lights.size(); i += 3) lay(&a.lights[i], read.lights);
		laid++;
	}
	Debug(misc, 2, "3D object {}: {} of its {} add-ons laid, {} textures of add-ons", house.name, laid, house.addon_places.size(), read.addon_objects.size());
}

/**
 * Read the house's shape and texture from its .glb (glb_path) the first time they are wanted, on whichever thread asks
 * first (MakeOnce()); after that they are never written again, and may be looked at on any thread.
 * @return whether it has them
 */
bool ThreeDObject::Read() const
{
	if (this->IsRead()) return true;
	MakeOnce(this, LAZY_HOUSE_GLB, [this] { return this->glb_state.value.load() != 0; }, [this] {
		GlbFilesScope files;
		const auto start = std::chrono::steady_clock::now();
		ThreeDObject read;
		read.name = this->name;
		read.tiles_x = this->tiles_x;
		read.tiles_y = this->tiles_y;
		read.addon = this->addon;
		/* Without a file of its own, its add-ons alone (a street lamp, MakeStreetLamps()). */
		bool ok = this->glb_path.empty() ? !this->addon_places.empty() : ReadGlb(this->glb_path, read);
		if (ok && !this->snow_path.empty()) {
			std::vector<uint8_t> snow = ReadWholeFile(this->snow_path);
			if (!snow.empty()) read.png = std::move(snow);
		}
		if (ok) LayAddons(*this, read);
		Debug(misc, 2, "3D object {}: {} triangles read in {} ms", this->name, read.tri.size() / 3, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
		if (!ok) ReadDiary(fmt::format("budova {} ({}) se nenačetla, kreslí se krabice", this->name, this->glb_path.empty() ? std::string("jen doplňky") : this->glb_path), true);
		return std::make_pair(ok, std::move(read));
	}, [this](std::pair<bool, ThreeDObject> made) {
		/* (The objects are in _three_d_objects, never const themselves: what is read is put into them once.) */
		ThreeDObject &self = const_cast<ThreeDObject &>(*this);
		if (made.first) {
			ThreeDObject &r = made.second;
			self.pos = std::move(r.pos);
			self.night_pos = std::move(r.night_pos);
			self.night_uv = std::move(r.night_uv);
			self.night_tri = std::move(r.night_tri);
			self.night_tex = std::move(r.night_tex);
			self.lights = std::move(r.lights);
			self.uv = std::move(r.uv);
			self.tri = std::move(r.tri);
			self.facing = std::move(r.facing);
			self.top = r.top;
			if (!r.pos.empty()) {
				self.extent[0] = self.extent[2] = std::numeric_limits<float>::infinity();
				self.extent[1] = self.extent[3] = -std::numeric_limits<float>::infinity();
				for (size_t i = 0; i + 2 < r.pos.size(); i += 3) {
					self.extent[0] = std::min(self.extent[0], r.pos[i]);
					self.extent[1] = std::max(self.extent[1], r.pos[i]);
					self.extent[2] = std::min(self.extent[2], r.pos[i + 1]);
					self.extent[3] = std::max(self.extent[3], r.pos[i + 1]);
				}
			}
			self.png = std::move(r.png);
			self.light = std::move(r.light);
			self.tri_tex = std::move(r.tri_tex);
			self.corner_tex = std::move(r.corner_tex);
			self.addon_objects = std::move(r.addon_objects);
			self.addon_places_of = std::move(r.addon_places_of);
			self.passable_tris = std::move(r.passable_tris);
			self.trunks = std::move(r.trunks);
			self.nrm = std::move(r.nrm);
		}
		self.glb_state.value.store(made.first ? 1 : 2, std::memory_order_release);
	});
	return this->IsRead();
}

/** Whether the house's shape and texture are read (Read()); if not, they are read in the background, there a frame or two on. */
bool ThreeDObject::TryRead() const
{
	if (this->IsRead()) return true;
	MadeOrAsked(this, LAZY_HOUSE_GLB, [this] { return this->glb_state.value.load() != 0; }, [this] { this->Read(); });
	return this->IsRead();
}

ThreeDObjectShapeView ThreeDObject::GetShape(int level) const
{
	MarkThreeDUsed(this->used);
	if (!this->Read()) return {};
	level = std::clamp(level, 0, THREE_D_SHAPE_LEVELS - 1);
	/* Its own: wanted so, or too many corners to count the triangles below. */
	if (level == 0 || this->pos.size() / 3 >= (1u << 21)) return {this->pos, this->uv, this->light, this->tri, this->facing, this->tri_tex, this->corner_tex, this->shape_uid.value};
	MakeOnce(this, LAZY_HOUSE_SHAPE + level, [this, level] { return this->shapes[level] != nullptr; }, [this, level] { return MakeHouseShape(*this, level); }, [this, level](std::shared_ptr<const ThreeDObjectShape> made) { this->shapes[level] = std::move(made); });
	const ThreeDObjectShape &s = *this->shapes[level];
	return {s.pos, s.uv, s.light, s.tri, s.facing, s.tri_tex, s.corner_tex, s.uid.value};
}

/** The building's shape simplified for a level if it is made already; else its own shape stands in, and the simpler one is made in the background. */
ThreeDObjectShapeView ThreeDObject::TryGetShape(int level) const
{
	MarkThreeDUsed(this->used);
	if (!this->TryRead()) return {};
	level = std::clamp(level, 0, THREE_D_SHAPE_LEVELS - 1);
	if (level == 0 || this->pos.size() / 3 >= (1u << 21)) return {this->pos, this->uv, this->light, this->tri, this->facing, this->tri_tex, this->corner_tex, this->shape_uid.value};
	if (!MadeOrAsked(this, LAZY_HOUSE_SHAPE + level, [this, level] { return this->shapes[level] != nullptr; }, [this, level] { this->GetShape(level); })) return {this->pos, this->uv, this->light, this->tri, this->facing, this->tri_tex, this->corner_tex, this->shape_uid.value};
	const ThreeDObjectShape &s = *this->shapes[level];
	return {s.pos, s.uv, s.light, s.tri, s.facing, s.tri_tex, s.corner_tex, s.uid.value};
}

/** The cells of a tile across for the footprints of the buildings (ThreeDObject::Blocks()): about a quarter of a metre, finer than the walker's half-width. */
static constexpr int FOOTPRINT_CELLS = 64;

/**
 * Whether a building stands at a place as high as a walker's body, from its knees to over its head (the
 * technologies of OpenMW, the player, 16. 10.: what one bumps into is the shape itself, not its tile): a park, the
 * square round a fountain, the space under a roof on pillars can be walked into, a wall cannot. Its triangles cut to
 * that height, their outlines and their insides on a grid of cells, made the first time.
 * @param x,y the place, world units from the building's north corner
 */
bool ThreeDObject::Blocks(double x, double y) const
{
	this->Read();
	const int w = this->tiles_x * FOOTPRINT_CELLS, h = this->tiles_y * FOOTPRINT_CELLS;
	const double cell = static_cast<double>(TILE_SIZE) / FOOTPRINT_CELLS;
	if (this->footprint_w == 0) {
		this->footprint.assign(static_cast<size_t>(w) * h, 0);
		this->footprint_w = w;
		const double lo = 0.3 * OBJECT_HEIGHT_PER_METRE, hi = 2.0 * OBJECT_HEIGHT_PER_METRE;
		auto mark = [&](double px, double py) {
			const int cx = static_cast<int>(std::floor(px / cell)), cy = static_cast<int>(std::floor(py / cell));
			if (cx >= 0 && cy >= 0 && cx < w && cy < h) this->footprint[static_cast<size_t>(cy) * w + cx] = 1;
		};
		size_t run = 0; // the run of passable triangles (a tree's crown) at or after this one
		for (size_t t = 0; t + 2 < this->tri.size(); t += 3) {
			while (run + 1 < this->passable_tris.size() && t / 3 >= this->passable_tris[run + 1]) run += 2;
			if (run + 1 < this->passable_tris.size() && t / 3 >= this->passable_tris[run]) continue;
			/* The triangle cut to the height of a body (a polygon, cut by z >= lo and z <= hi). */
			std::vector<std::array<double, 3>> poly;
			for (int k = 0; k < 3; k++) {
				const uint32_t i = this->tri[t + k];
				poly.push_back({this->pos[i * 3], this->pos[i * 3 + 1], this->pos[i * 3 + 2]});
			}
			for (int side = 0; side < 2 && !poly.empty(); side++) {
				const double limit = side == 0 ? lo : hi;
				auto inside = [&](const std::array<double, 3> &p) { return side == 0 ? p[2] >= limit : p[2] <= limit; };
				std::vector<std::array<double, 3>> cut;
				for (size_t k = 0; k < poly.size(); k++) {
					const auto &a = poly[k], &b = poly[(k + 1) % poly.size()];
					if (inside(a)) cut.push_back(a);
					if (inside(a) != inside(b)) {
						const double f = (limit - a[2]) / (b[2] - a[2]);
						cut.push_back({a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, limit});
					}
				}
				poly = std::move(cut);
			}
			if (poly.size() < 2) continue;
			/* Its outline, half a cell a step: a wall seen from above is a line. */
			double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
			for (size_t k = 0; k < poly.size(); k++) {
				const auto &a = poly[k], &b = poly[(k + 1) % poly.size()];
				x0 = std::min(x0, a[0]);
				x1 = std::max(x1, a[0]);
				y0 = std::min(y0, a[1]);
				y1 = std::max(y1, a[1]);
				const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(b[0] - a[0], b[1] - a[1]) / (cell / 2))));
				for (int s = 0; s <= steps; s++) mark(a[0] + (b[0] - a[0]) * s / steps, a[1] + (b[1] - a[1]) * s / steps);
			}
			/* And its inside, the middle of each cell under it (a table top, a low roof). */
			for (int cy = std::max(0, static_cast<int>(y0 / cell)); cy <= std::min(h - 1, static_cast<int>(y1 / cell)); cy++) {
				for (int cx = std::max(0, static_cast<int>(x0 / cell)); cx <= std::min(w - 1, static_cast<int>(x1 / cell)); cx++) {
					const double px = (cx + 0.5) * cell, py = (cy + 0.5) * cell;
					bool in = false;
					for (size_t k = 0, l = poly.size() - 1; k < poly.size(); l = k++) {
						const auto &a = poly[k], &b = poly[l];
						if ((a[1] > py) != (b[1] > py) && px < (b[0] - a[0]) * (py - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
					}
					if (in) this->footprint[static_cast<size_t>(cy) * w + cx] = 1;
				}
			}
		}
		/* The trunks of its trees: the cells within each, and the one its middle is in. */
		for (size_t i = 0; i + 2 < this->trunks.size(); i += 3) {
			const double tx = this->trunks[i], ty = this->trunks[i + 1], tr = this->trunks[i + 2];
			mark(tx, ty);
			for (int cy = std::max(0, static_cast<int>((ty - tr) / cell)); cy <= std::min(h - 1, static_cast<int>((ty + tr) / cell)); cy++) {
				for (int cx = std::max(0, static_cast<int>((tx - tr) / cell)); cx <= std::min(w - 1, static_cast<int>((tx + tr) / cell)); cx++) {
					if (std::hypot((cx + 0.5) * cell - tx, (cy + 0.5) * cell - ty) <= tr) this->footprint[static_cast<size_t>(cy) * w + cx] = 1;
				}
			}
		}
		/*
		 * Inside: the building is hollow, its walls only. What a walker cannot get to from outside -- its middle kept
		 * a walker's half-width (WALKER_CELLS) from every wall, a door narrower than a walker shut -- is solid too.
		 */
		static constexpr double WALKER_CELLS_EXACT = THREE_D_WALKER_RADIUS_M * OBJECT_UNITS_PER_METRE * FOOTPRINT_CELLS / TILE_SIZE;
		static constexpr int WALKER_CELLS = static_cast<int>(WALKER_CELLS_EXACT) + (WALKER_CELLS_EXACT > static_cast<int>(WALKER_CELLS_EXACT) ? 1 : 0);
		/* On a grid with a margin round the tiles, free: the walker comes from there. */
		static constexpr int MARGIN = WALKER_CELLS + 1;
		const int ew = w + 2 * MARGIN, eh = h + 2 * MARGIN;
		const size_t cells = static_cast<size_t>(ew) * eh;
		auto own = [&](size_t i) -> int {
			const int x0 = static_cast<int>(i % ew) - MARGIN, y0 = static_cast<int>(i / ew) - MARGIN;
			return x0 >= 0 && y0 >= 0 && x0 < w && y0 < h ? y0 * w + x0 : -1;
		};
		std::vector<int> wall_dist(cells, INT_MAX);
		std::vector<size_t> queue;
		for (size_t i = 0; i < cells; i++) {
			const int o = own(i);
			if (o >= 0 && this->footprint[o] != 0) {
				wall_dist[i] = 0;
				queue.push_back(i);
			}
		}
		auto spread = [&](std::vector<size_t> &q, auto &&step) {
			for (size_t at = 0; at < q.size(); at++) {
				const int x0 = static_cast<int>(q[at] % ew), y0 = static_cast<int>(q[at] / ew);
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x0 + dx, ny = y0 + dy;
						if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= ew || ny >= eh) continue;
						const size_t n = static_cast<size_t>(ny) * ew + nx;
						if (step(q[at], n)) q.push_back(n);
					}
				}
			}
		};
		spread(queue, [&](size_t from, size_t n) {
			if (wall_dist[n] <= wall_dist[from] + 1) return false;
			wall_dist[n] = wall_dist[from] + 1;
			return true;
		});
		/* Where the walker's middle can be, reached from the margin. */
		std::vector<int> reach(cells, INT_MAX);
		queue.clear();
		for (size_t i = 0; i < cells; i++) {
			const int x0 = static_cast<int>(i % ew), y0 = static_cast<int>(i / ew);
			if ((x0 == 0 || y0 == 0 || x0 == ew - 1 || y0 == eh - 1) && wall_dist[i] > WALKER_CELLS) {
				reach[i] = 0;
				queue.push_back(i);
			}
		}
		spread(queue, [&](size_t, size_t n) {
			if (reach[n] == 0 || wall_dist[n] <= WALKER_CELLS) return false;
			reach[n] = 0;
			return true;
		});
		/* And the walker's body round it: up to its half-width from there. */
		spread(queue, [&](size_t from, size_t n) {
			if (wall_dist[n] == 0 || reach[n] <= reach[from] + 1 || reach[from] >= WALKER_CELLS) return false;
			reach[n] = reach[from] + 1;
			return true;
		});
		for (size_t i = 0; i < cells; i++) {
			const int o = own(i);
			if (o >= 0 && reach[i] == INT_MAX) this->footprint[o] = 1;
		}
	}
	const int cx = static_cast<int>(std::floor(x / cell)), cy = static_cast<int>(std::floor(y / cell));
	if (cx < 0 || cy < 0 || cx >= w || cy >= h) return false;
	return this->footprint[static_cast<size_t>(cy) * w + cx] != 0;
}

/**
 * Make the shapes of objects about to be drawn on every core at once, as their textures
 * (PrepareThreeDObjectTextures()): a town seen for the first time from afar wanted thirty of them.
 * @param wanted the shapes: object and level
 */
void PrepareThreeDObjectShapes(std::span<const std::pair<const ThreeDObject *, int>> wanted)
{
	std::vector<std::pair<const ThreeDObject *, int>> jobs;
	{
		std::lock_guard lock(_lazy_mutex);
		for (const auto &[o, level] : wanted) {
			/* Not read yet: read on a core of its own too (ThreeDObject::Read()). */
			if (o == nullptr || level < 0 || level >= THREE_D_SHAPE_LEVELS || (o->IsRead() && (level == 0 || o->shapes[level] != nullptr))) continue;
			if (std::ranges::find(jobs, std::make_pair(o, level)) == jobs.end()) jobs.emplace_back(o, level);
		}
	}
	if (jobs.empty()) return;
	const size_t threads = std::min<size_t>(jobs.size(), std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16));
	std::atomic<size_t> next = 0;
	auto work = [&] {
		/* Each its own object and level: a slot of shapes no other thread touches. */
		for (size_t j; (j = next++) < jobs.size();) jobs[j].first->GetShape(jobs[j].second);
	};
	std::vector<std::thread> pool;
	for (size_t i = 1; i < threads; i++) pool.emplace_back(work);
	work();
	for (std::thread &t : pool) t.join();
}

/**
 * The object's texture at a level of detail (DecodeTextureLevels()), read from the PNG the first time; painted
 * (PaintAt()) as the vehicles' colour by number: each pixel of a part's mask the part's colour at the texture's
 * lightness, its own where the texture is "zacatek" light.
 */
/**
 * The texture of the object at a level of detail, painted by number (PaintAt()), made the first time it is asked for.
 * @param may_drop whether the painted textures of every object may be dropped here when they take too much memory;
 *                 not while textures are made on several cores at once (PrepareThreeDObjectTextures())
 */
/** Which thing of a building a painted texture is (MakeOnce()): its level and paint. */
static int LazyHousePaint(int level, uint32_t paint)
{
	return LAZY_HOUSE_PAINT + level + 16 * static_cast<int>((paint ^ (paint >> 16)) & 0xFFFF);
}

/**
 * Let the painted textures of every building go when they take too much memory (PAINTED_BUDGET): not while a frame of
 * the view from the eyes is drawn from them (EyeFrameDrawing()).
 */
static void DropPaintedTextures()
{
	if (_painted_bytes <= PAINTED_BUDGET || EyeFrameDrawing()) return;
	std::lock_guard lock(_lazy_mutex);
	for (const ThreeDObject &o : _three_d_objects) o.painted.clear();
	_painted_bytes = 0;
}

/** The building's texture at all its levels of detail, read from its PNG the first time (on either thread, MakeOnce()). */
static void MakeHouseLevels(const ThreeDObject &o)
{
	MakeOnce(&o, LAZY_HOUSE_LEVELS, [&o] { return !o.levels.empty(); }, [&o] {
		std::vector<ThreeDObjectTexture> made;
		DecodeTextureLevels(o.png, made);
		return made;
	}, [&o](std::vector<ThreeDObjectTexture> made) { o.levels = std::move(made); });
}

const ThreeDObjectTexture *ThreeDObject::GetTexture(int level, uint32_t paint, bool may_drop) const
{
	MarkThreeDUsed(this->used);
	/* A part placed on a tile of an original industry: the part's texture, once for all its places. */
	if (this->texture_of < _three_d_objects.size() && &_three_d_objects[this->texture_of] != this) return _three_d_objects[this->texture_of].GetTexture(level, paint, may_drop);
	this->Read();
	MakeHouseLevels(*this);
	level = std::clamp<int>(level, 0, static_cast<int>(this->levels.size()) - 1);
	const ThreeDObjectTexture *plain = &this->levels[level];
	if (paint == 0 || this->colours.empty()) return plain;
	if (may_drop) DropPaintedTextures();
	const std::pair<int, uint32_t> key(level, paint);
	MakeOnce(this, LazyHousePaint(level, paint), [this, key] { return this->painted.contains(key); }, [this, plain, paint] { return this->Painted(*plain, paint); }, [this, key](ThreeDObjectTexture t) {
		_painted_bytes += t.px.size() * sizeof(uint32_t);
		this->painted.emplace(key, std::move(t));
	});
	std::lock_guard lock(_lazy_mutex);
	auto it = this->painted.find(key);
	return it != this->painted.end() ? &it->second : plain;
}

/**
 * A texture of the building's triangles (ThreeDObjectShapeView::TriangleTexture()): 0 its own, painted; n its add-on
 * n's, as it is (the add-ons are not painted by the building's colours).
 */
const ThreeDObjectTexture *ThreeDObject::GetTextureOf(uint8_t texture, int level, uint32_t paint) const
{
	if (texture == 0 || texture > this->addon_objects.size()) return this->GetTexture(level, paint);
	return _three_d_objects[this->addon_objects[texture - 1]].GetTexture(level, paint);
}

/** A texture of the building's triangles if it is made already (as TryGetTexture()); else made in the background. */
const ThreeDObjectTexture *ThreeDObject::TryGetTextureOf(uint8_t texture, int level, uint32_t paint) const
{
	if (texture == 0 || texture > this->addon_objects.size()) return this->TryGetTexture(level, paint);
	return _three_d_objects[this->addon_objects[texture - 1]].TryGetTexture(level, paint);
}

/**
 * The building's texture at a level of detail if it is read already, painted (PaintAt()) if that is made already;
 * else nullptr (nothing read yet) or the texture unpainted (its paint not made yet), and what is missing is made in the
 * background (MadeOrAsked()).
 */
const ThreeDObjectTexture *ThreeDObject::TryGetTexture(int level, uint32_t paint) const
{
	MarkThreeDUsed(this->used);
	if (this->texture_of < _three_d_objects.size() && &_three_d_objects[this->texture_of] != this) return _three_d_objects[this->texture_of].TryGetTexture(level, paint);
	if (!this->TryRead()) return nullptr;
	if (!MadeOrAsked(this, LAZY_HOUSE_LEVELS, [this] { return !this->levels.empty(); }, [this, level, paint] { this->GetTexture(level, paint, false); })) return nullptr;
	level = std::clamp<int>(level, 0, static_cast<int>(this->levels.size()) - 1);
	const ThreeDObjectTexture *plain = &this->levels[level];
	if (paint == 0 || this->colours.empty()) return plain;
	const std::pair<int, uint32_t> key(level, paint);
	if (!MadeOrAsked(this, LazyHousePaint(level, paint), [this, key] { return this->painted.contains(key); }, [this, level, paint] { this->GetTexture(level, paint, false); })) return plain;
	std::lock_guard lock(_lazy_mutex);
	auto it = this->painted.find(key);
	return it != this->painted.end() ? &it->second : plain;
}

/** The texture painted by number (PaintAt()): each pixel of a part's mask the part's colour at the texture's lightness, its own where the texture is "zacatek" light. */
ThreeDObjectTexture ThreeDObject::Painted(const ThreeDObjectTexture &plain, uint32_t paint) const
{
	ThreeDObjectTexture t = plain;
	const uint8_t bits = PaintBits(this->colours.size());
	for (size_t k = 0; k < this->colours.size(); k++) {
		const ThreeDObjectColourPart &part = this->colours[k];
		/* The mask, read the first time (on either thread). */
		MakeOnce(&part, LAZY_HOUSE_MASK + static_cast<int>(k), [&part] { return !part.levels.empty(); }, [&part] {
			std::vector<ThreeDObjectTexture> made;
			DecodeTextureLevels(part.mask_png, made);
			return made;
		}, [&part](std::vector<ThreeDObjectTexture> made) { part.levels = std::move(made); });
		/* The mask's level nearest the texture's, read at the same place. */
		const ThreeDObjectTexture *mask = &part.levels.back();
		for (const ThreeDObjectTexture &m : part.levels) {
			if (m.w <= t.w) {
				mask = &m;
				break;
			}
		}
		const Colour colour(part.palette[GB(paint, static_cast<uint8_t>(bits * k), bits) % part.palette.size()]);
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
	return t;
}

/**
 * Make the textures of objects about to be drawn on every core of the processor at once, before they are drawn (the
 * technologies of OpenMW, the player, 16. 10.: it makes its world ready in threads of its own): reading the PNG and
 * painting it took the first picture of a town two seconds, one texture after the other. Each object's textures on
 * one thread, the objects shared among the threads.
 * @param wanted the textures: object, level of detail, paint
 */
void PrepareThreeDObjectTextures(std::span<const ThreeDTextureWanted> wanted)
{
	/* What is not made yet, by object. */
	std::vector<std::pair<const ThreeDObject *, std::vector<std::pair<int, uint32_t>>>> jobs;
	{
		std::lock_guard lock(_lazy_mutex);
		for (const ThreeDTextureWanted &w : wanted) {
			const ThreeDObject *o = w.obj;
			if (o == nullptr) continue;
			/* A part placed on an industry's tile: its part's texture, made once for all its places (one thread a texture). */
			if (o->texture_of < _three_d_objects.size()) o = &_three_d_objects[o->texture_of];
			if (!o->levels.empty()) {
				const int level = std::clamp<int>(w.level, 0, static_cast<int>(o->levels.size()) - 1);
				if (w.paint == 0 || o->colours.empty() || o->painted.contains({level, w.paint})) continue;
			}
			auto it = std::ranges::find(jobs, o, &std::pair<const ThreeDObject *, std::vector<std::pair<int, uint32_t>>>::first);
			if (it == jobs.end()) it = jobs.insert(jobs.end(), {o, {}});
			if (std::ranges::find(it->second, std::make_pair(w.level, w.paint)) == it->second.end()) it->second.emplace_back(w.level, w.paint);
		}
	}
	if (jobs.size() < 2) return; // as it comes, when drawn
	DropPaintedTextures();
	std::atomic<size_t> next = 0;
	auto work = [&]() {
		for (size_t j; (j = next++) < jobs.size();) {
			for (const auto &[level, paint] : jobs[j].second) jobs[j].first->GetTexture(level, paint, false);
		}
	};
	const size_t cores = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16);
	std::vector<std::thread> threads;
	for (size_t i = 1; i < std::min(cores, jobs.size()); i++) threads.emplace_back(work);
	work();
	for (std::thread &t : threads) t.join();
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
		if (!o.industry_name.empty()) out += fmt::format(", industry {} ({}) variant {}", o.industry_name, o.industry, o.industry_variant);
		if (!o.field_part.empty()) out += fmt::format(", field part {}, {} + {} at work, {} + {} by the road", o.field_part, o.work[0].size(), o.work[1].size(), o.by_road[0].size(), o.by_road[1].size());
		if (!o.plants.empty()) out += fmt::format(", {} plants", o.plants.size());
	}
	return out;
}

/** Forget what was made of the building to draw it -- its textures, painted or not, its simplified shapes -- to be made again when it is drawn. */
void ThreeDObject::Forget() const
{
	std::lock_guard lock(_lazy_mutex);
	this->levels.clear();
	this->levels.shrink_to_fit();
	size_t painted = 0;
	for (const auto &[key, t] : this->painted) painted += t.px.size() * sizeof(uint32_t);
	this->painted.clear();
	_painted_bytes -= std::min<size_t>(painted, _painted_bytes);
	for (const ThreeDObjectColourPart &part : this->colours) {
		part.levels.clear();
		part.levels.shrink_to_fit();
	}
	for (auto &shape : this->shapes) shape.reset();
}

/** Forget what was made of the vehicle or figure to draw it: its textures and the shapes of its frames (a dragon's poses). */
void ThreeDVehicleObject::Forget() const
{
	std::lock_guard lock(_lazy_mutex);
	this->levels.clear();
	this->mask_levels.reset();
	this->mask_tried = false;
	this->rust_levels.reset();
	this->rust_tried = false;
	this->addon_mask_levels.clear();
	this->addon_mask_tried.clear();
	this->detail_masks.clear();
	this->detail_tried.clear();
	this->frame_shapes.clear();
	this->frame_shapes.shrink_to_fit();
	this->shape.reset();
	this->shape_tried = false;
	this->shape_parts = {};
}

/** How long a thing not drawn keeps what was made of it, seconds. */
static constexpr uint32_t THREE_D_FORGET_AFTER = 120;

/**
 * Forget what was made to draw the things not drawn for two minutes (the technologies of OpenMW, the player, 16. 10.:
 * what is far from the player is let go of): the textures and the simplified shapes of the buildings, the textures and
 * the poses of the figures and the creatures, the passages between their animations, the shapes no one keeps. A big
 * map walked over kept all of it before. Between the frames, never while they are drawn.
 */
static void ForgetUnusedThreeDObjects()
{
	_three_d_clock += 15;
	/* Not while the pictures of the vehicles are being made on their thread (three_d_vehicle_object.cpp) or a frame
	 * of the view from the eyes is drawn (EyeFrameDrawing()): they hold what the objects made; the next time, a
	 * quarter of a minute on. */
	if (ThreeDPicturesBeingMade() || EyeFrameDrawing()) return;
	TrimThreeDTextures();
	TrimThreeDEyePainted();
	if (_three_d_clock <= THREE_D_FORGET_AFTER) return;
	const uint32_t before = _three_d_clock - THREE_D_FORGET_AFTER;
	size_t forgotten = 0;
	for (const ThreeDObject &o : _three_d_objects) {
		if (o.used >= before || (o.levels.empty() && o.painted.empty() && std::ranges::all_of(o.shapes, [](const auto &p) { return p == nullptr; }))) continue;
		o.Forget();
		forgotten++;
	}
	for (std::vector<ThreeDVehicleObject> *list : {&_vehicle_objects, &_figure_objects}) {
		for (const ThreeDVehicleObject &v : *list) {
			if (v.used >= before || v.frame_shapes.empty()) continue;
			v.Forget();
			forgotten++;
		}
	}
	for (auto it = _passages.begin(); it != _passages.end();) {
		if (it->second->used < before) {
			it = _passages.erase(it);
			forgotten++;
		} else {
			++it;
		}
	}
	/* The shapes read that nothing keeps any more (the poses of a passage gone, a vehicle's frames). */
	{
		std::lock_guard lock(_vehicle_shapes_mutex);
		for (auto it = _vehicle_shapes.begin(); it != _vehicle_shapes.end();) {
			if (it->second.use_count() == 1) {
				it = _vehicle_shapes.erase(it);
			} else {
				++it;
			}
		}
	}
	if (forgotten > 0) Debug(misc, 1, "3D objects: {} not drawn for {} s forgotten", forgotten, THREE_D_FORGET_AFTER);
}

/** The clock of the objects goes on in real time, every fifteen seconds (ForgetUnusedThreeDObjects()). */
static IntervalTimer<TimerWindow> _forget_timer(std::chrono::seconds(15), [](auto) { ForgetUnusedThreeDObjects(); });

/** The memory of a vector's elements. */
template <typename T>
static size_t VectorBytes(const std::vector<T> &v)
{
	return v.capacity() * sizeof(T);
}

/** The memory of a vehicle's shape and of its simpler shapes made: its corners, triangles and lights (its textures apart). */
size_t ThreeDVehicleShape::Bytes() const
{
	size_t b = VectorBytes(this->pos) + VectorBytes(this->nrm) + VectorBytes(this->uv) + VectorBytes(this->tri) + VectorBytes(this->tri_tex);
	b += VectorBytes(this->night_pos) + VectorBytes(this->night_tri) + VectorBytes(this->night_colour) + VectorBytes(this->night_back);
	b += VectorBytes(this->tri_part) + VectorBytes(this->corner_wheel) + VectorBytes(this->tri_glass) + VectorBytes(this->night_part);
	b += VectorBytes(this->lanterns) + VectorBytes(this->torches) + VectorBytes(this->torch_aims);
	b += VectorBytes(this->tex_glow) + VectorBytes(this->tex_alpha);
	for (const auto &simpler : this->simpler.shapes) {
		if (simpler != nullptr) b += simpler->Bytes();
	}
	return b;
}

/**
 * The rig ('test3d pamet'): how much memory what was made of the 3D objects takes -- the vehicles' shapes, their
 * textures halved for each level of detail, the pictures of the textures as they are in the files, the .glb files kept
 * a moment, the buildings' textures (painted too) and simplified shapes.
 */
std::string DescribeThreeDObjectMemory()
{
	auto mb = [](size_t b) { return b / (1024.0 * 1024.0); };
	size_t shapes = 0, shape_count = 0;
	{
		std::lock_guard lock(_vehicle_shapes_mutex);
		for (const auto &[key, shape] : _vehicle_shapes) {
			if (shape == nullptr) continue;
			shapes += shape->Bytes();
			shape_count++;
		}
	}
	size_t textures = 0, texture_count = 0, pngs = 0, png_count = 0, masks = 0;
	{
		std::lock_guard lock(_texture_bytes_mutex);
		std::set<const void *> seen;
		auto levels_bytes = [&](const std::shared_ptr<const std::vector<ThreeDObjectTexture>> &lv) {
			if (lv == nullptr || !seen.insert(lv.get()).second) return;
			for (const ThreeDObjectTexture &t : *lv) textures += VectorBytes(t.px);
			texture_count++;
		};
		for (const auto &[key, made] : _texture_levels) levels_bytes(made.levels.lock());
		for (const auto &[key, made] : _file_texture_levels) levels_bytes(made.lock());
		for (const auto &[key, made] : _detail_masks_made) {
			if (auto m = made.lock(); m != nullptr) masks += VectorBytes(m->index);
		}
		for (const auto &[hash, bytes] : _texture_bytes) {
			if (auto b = bytes.lock(); b != nullptr) {
				pngs += b->size();
				png_count++;
			}
		}
	}
	const size_t glb_files = _glb_files_kept_bytes;
	size_t houses = 0, house_count = 0, house_glb = 0, houses_read = 0;
	{
		std::lock_guard lock(_lazy_mutex);
		for (const ThreeDObject &o : _three_d_objects) {
			/* What is read of its .glb (ThreeDObject::Read()): its shape and its texture's picture. */
			if (!o.glb_path.empty() && o.IsRead()) {
				houses_read++;
				house_glb += VectorBytes(o.pos) + VectorBytes(o.uv) + VectorBytes(o.tri) + VectorBytes(o.facing) + VectorBytes(o.light) + VectorBytes(o.png) + VectorBytes(o.night_pos) + VectorBytes(o.night_uv) + VectorBytes(o.night_tri);
			}
			size_t b = 0;
			for (const ThreeDObjectTexture &t : o.levels) b += VectorBytes(t.px);
			for (const auto &[key, t] : o.painted) b += VectorBytes(t.px);
			for (const auto &shape : o.shapes) {
				if (shape != nullptr) b += VectorBytes(shape->pos) + VectorBytes(shape->uv) + VectorBytes(shape->light) + VectorBytes(shape->tri) + VectorBytes(shape->facing);
			}
			if (b == 0) continue;
			houses += b;
			house_count++;
		}
	}
	return fmt::format("glb: tvary vozidel {:.1f} MB ({}), textury po urovnich {:.1f} MB ({}), masky podrobnosti {:.1f} MB, obrazky textur ze souboru {:.1f} MB ({}), drzene soubory .glb {:.1f} MB, domy (textury, lakovane, tvary) {:.1f} MB ({}), domy prectene z glb {:.1f} MB ({})",
			mb(shapes), shape_count, mb(textures), texture_count, mb(masks), mb(pngs), png_count, mb(glb_files), mb(houses), house_count, mb(house_glb), houses_read);
}

/** The rig: every house read now (ThreeDObject::Read()), as the start of the game read them all before. */
std::string RigReadAllThreeDHouses()
{
	if (!_three_d_objects_read) ReadThreeDObjects();
	const auto start = std::chrono::steady_clock::now();
	size_t read = 0, bytes = 0;
	for (const ThreeDObject &o : _three_d_objects) {
		if (o.glb_path.empty() || o.IsRead()) continue;
		if (!o.Read()) continue;
		read++;
		bytes += VectorBytes(o.pos) + VectorBytes(o.uv) + VectorBytes(o.tri) + VectorBytes(o.facing) + VectorBytes(o.light) + VectorBytes(o.png);
	}
	return fmt::format("domu precteno {} za {} ms, {:.1f} MB", read, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(), bytes / (1024.0 * 1024.0));
}

/** The rig: the clock of the objects on by so many seconds, and what is not drawn since forgotten. */
std::string RigForgetThreeDObjects(uint seconds)
{
	for (uint s = 0; s < seconds; s += 15) ForgetUnusedThreeDObjects();
	size_t textures = 0, shapes = 0;
	for (const ThreeDObject &o : _three_d_objects) {
		if (!o.levels.empty()) textures++;
		for (const auto &p : o.shapes) shapes += p != nullptr;
	}
	return fmt::format("hodiny {} s, domu s texturou {}, zjednodusenych tvaru {}, prechodu {}, tvaru vozidel {}", _three_d_clock.load(), textures, shapes, _passages.size(), VehicleShapesKept());
}

/**
 * How big the glow of a light of an object ("svetlo_NN", its x, y, z at i in lights) is drawn, a share of a lone
 * light's (THREE_D_GLOW_UNITS): no bigger than the way to the nearest other light of it. Bulbs set close in a row -- the
 * cinema's marquee, eleven of them 0.44 m apart -- light as a row of bulbs; with each the glow of a window they added
 * up to one white blot over the door (the player, 23. 10.: "the cinema shines too much at night").
 */
float ThreeDGlowShare(const std::vector<float> &lights, size_t i)
{
	double nearest = THREE_D_GLOW_UNITS;
	for (size_t j = 0; j + 2 < lights.size(); j += 3) {
		if (j == i) continue;
		const double dx = lights[j] - lights[i], dy = lights[j + 1] - lights[i + 1];
		const double dz = (lights[j + 2] - lights[i + 2]) / OBJECT_HEIGHT_PER_METRE * OBJECT_UNITS_PER_METRE;
		nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
	}
	return static_cast<float>(std::clamp(nearest / THREE_D_GLOW_UNITS, 0.3, 1.0));
}

/** A tree's trunk blocks the walker so far about its foot, metres; a plant lower than THREE_D_TRUNK_PLANT_M none (the crops, the flowers). */
static constexpr double THREE_D_TRUNK_M = 0.3;
static constexpr double THREE_D_TRUNK_PLANT_M = 4.0;

/**
 * Whether what stands on a tile of an industry of the original game drawn from the parts the industries share keeps
 * the walker off a world point: its buildings by their shape (ThreeDObject::Blocks(), from the anchor of their block),
 * its trees by their trunks only -- the crown is over the head. Before, every such tile was a wall, and the forest
 * could not be walked into (the player, 23. 10.: "the distillery can be walked through and the forest I cannot enter").
 * @return whether it blocks; nullopt for a tile the parts do not draw (a set's, one being built: as before)
 */
std::optional<bool> ThreeDOriginalIndustryBlocks(TileIndex tile, double x, double y)
{
	if (!IsTileType(tile, TileType::Industry) || !IsIndustryCompleted(tile)) return std::nullopt;
	auto parts_of = [](TileIndex t) -> const ThreeDIndustryTileParts * {
		const IndustryGfx gfx = GetIndustryGfx(t);
		if (gfx >= NEW_INDUSTRYTILEOFFSET || GetIndustryTileSpec(gfx)->grf_prop.HasGrfFile()) return nullptr;
		return GetThreeDOriginalIndustryTile(gfx);
	};
	const ThreeDIndustryTileParts *tp = parts_of(tile);
	if (tp == nullptr) return std::nullopt;
	TileIndex anchor = tile;
	if (tp->member) {
		anchor = TileAddWrap(tile, -tp->anchor_dx, -tp->anchor_dy);
		if (anchor == INVALID_TILE || !IsTileType(anchor, TileType::Industry) || GetIndustryIndex(anchor) != GetIndustryIndex(tile)) return false;
		tp = parts_of(anchor);
		if (tp == nullptr || tp->member) return false;
	}
	const double ox = x - TileX(anchor) * TILE_SIZE, oy = y - TileY(anchor) * TILE_SIZE;
	for (const ThreeDObject *part : tp->parts) {
		if (part->Blocks(ox, oy)) return true;
	}
	if (tp->plants != nullptr) {
		for (const ThreeDObjectFigure &p : tp->plants->plants) {
			if (p.scale * p.stretch < THREE_D_TRUNK_PLANT_M) continue;
			if (std::hypot(ox - p.at[0] * OBJECT_UNITS_PER_METRE, oy - p.at[1] * OBJECT_UNITS_PER_METRE) < THREE_D_TRUNK_M * OBJECT_UNITS_PER_METRE) return true;
		}
	}
	return false;
}
