/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_gui.cpp GUI for the blueprint (copy and paste) feature. */

#include "stdafx.h"
#include "blueprint.h"
#include "blueprint_cmd.h"
#include "blueprint_gui.h"
#include "blueprint_version.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/math_func.hpp"
#include "dropdown_func.h"
#include "error.h"
#include "fileio_func.h"
#include "gfx_func.h"
#include "hotkeys.h"
#include "map_func.h"
#include "palette_func.h"
#include "zoom_func.h"
#include "network/network.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"
#include "querystring_gui.h"
#include "textbuf_gui.h"
#include "rail_cmd.h"
#include "rail_gui.h"
#include "road_cmd.h"
#include "road_func.h"
#include "sound_func.h"
#include "station_cmd.h"
#include "strings_func.h"
#include "tile_map.h"
#include "tilehighlight_func.h"
#include "track_func.h"
#include "tunnelbridge_cmd.h"
#include "viewport_func.h"
#include "water_cmd.h"
#include "water_map.h"
#include "waypoint_cmd.h"
#include "window_func.h"
#include "window_gui.h"

#include <algorithm>
#include <filesystem>

#include "widgets/blueprint_widget.h"
#include "widgets/misc_widget.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

bool SetClipboardContents(const std::string &contents); ///< Implemented per OS.

/** Identifying prefix of a blueprint in clipboard text form; the trailing digit is the format version. */
static constexpr std::string_view BLUEPRINT_CLIPBOARD_PREFIX = "OTTD-BP-1;";

/** Base64 alphabet used for blueprint clipboard strings. */
static constexpr std::string_view BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Encode bytes as base64 (RFC 4648, with padding). */
static std::string Base64Encode(std::span<const uint8_t> data)
{
	std::string result;
	result.reserve((data.size() + 2) / 3 * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		uint32_t chunk = data[i] << 16;
		if (i + 1 < data.size()) chunk |= data[i + 1] << 8;
		if (i + 2 < data.size()) chunk |= data[i + 2];
		result.push_back(BASE64_ALPHABET[(chunk >> 18) & 0x3F]);
		result.push_back(BASE64_ALPHABET[(chunk >> 12) & 0x3F]);
		result.push_back(i + 1 < data.size() ? BASE64_ALPHABET[(chunk >> 6) & 0x3F] : '=');
		result.push_back(i + 2 < data.size() ? BASE64_ALPHABET[chunk & 0x3F] : '=');
	}
	return result;
}

/** Decode base64 text; whitespace is not tolerated. */
static std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text)
{
	while (!text.empty() && text.back() == '=') text.remove_suffix(1);
	std::vector<uint8_t> result;
	result.reserve(text.size() * 3 / 4);
	uint32_t chunk = 0;
	uint bits = 0;
	for (char c : text) {
		size_t value = BASE64_ALPHABET.find(c);
		if (value == std::string_view::npos) return std::nullopt;
		chunk = (chunk << 6) | static_cast<uint32_t>(value);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			result.push_back(static_cast<uint8_t>(chunk >> bits));
		}
	}
	return result;
}

/**
 * Try to parse a blueprint from its text form.
 * @param text The text to parse; surrounding whitespace is tolerated.
 * @param[out] blueprint Filled with the parsed blueprint on success.
 * @return Whether the text was a valid blueprint.
 */
static bool ParseBlueprintText(std::string_view text, Blueprint &blueprint)
{
	if (text.size() > 8 * 1024 * 1024) return false;

	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
	while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
	if (!text.starts_with(BLUEPRINT_CLIPBOARD_PREFIX)) return false;

	auto bytes = Base64Decode(text.substr(BLUEPRINT_CLIPBOARD_PREFIX.size()));
	if (!bytes.has_value()) return false;

	Blueprint result = EndianBufferReader::ToValue<Blueprint>(*bytes);
	if (result.width == 0 || result.height == 0) return false;
	if (result.width > MAX_BLUEPRINT_DIMENSION || result.height > MAX_BLUEPRINT_DIMENSION) return false;
	if (result.IsEmpty()) return false;
	size_t corners = static_cast<size_t>(result.width + 1) * (result.height + 1);
	if (!result.corner_heights.empty() && result.corner_heights.size() != corners) return false;

	blueprint = std::move(result);
	return true;
}

/**
 * Encode a blueprint into its shareable text form.
 * @param blueprint The blueprint to encode.
 * @return The text form of the blueprint.
 */
static std::string EncodeBlueprintText(const Blueprint &blueprint)
{
	auto buffer = EndianBufferWriter<>::FromValue(blueprint);
	std::string text{BLUEPRINT_CLIPBOARD_PREFIX};
	text += Base64Encode(buffer);
	return text;
}

/** Identifying prefix of a full 8-slot blueprint set saved to a file; the trailing digit is the format version. */
static constexpr std::string_view BLUEPRINT_SET_FILE_PREFIX = "OTTD-BPSET-1;";

/** File extension used for exported blueprint sets, appended to the player-typed file name. */
static constexpr std::string_view BLUEPRINT_SET_FILE_EXTENSION = ".txt";

/** Maximum length of a blueprint set file name (without extension) in characters including '\0'. */
static const uint MAX_LENGTH_BLUEPRINT_FILE_NAME_CHARS = 48;

/**
 * Validate and trim a player-typed file name.
 * @param name Raw text entered by the player.
 * @return The trimmed name, or \c std::nullopt if it is empty or contains characters that are unsafe in a file name.
 */
static std::optional<std::string> SanitizeBlueprintFileName(std::string_view name)
{
	while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
	while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
	if (name.empty() || name == "." || name == "..") return std::nullopt;
	if (name.find_first_of("/\\:*?\"<>|") != std::string_view::npos) return std::nullopt;
	return std::string{name};
}

/**
 * Encode all blueprint slots into their shareable text form.
 * @return The text form of every slot, in order.
 */
static std::string EncodeBlueprintSetText()
{
	std::vector<Blueprint> all_slots;
	all_slots.reserve(NUM_BLUEPRINT_SLOTS);
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) all_slots.push_back(GetBlueprint(i));

	std::vector<uint8_t> buffer;
	EndianBufferWriter<> writer{buffer};
	WriteBlueprintVector(writer, all_slots);

	std::string text{BLUEPRINT_SET_FILE_PREFIX};
	text += Base64Encode(buffer);
	return text;
}

/**
 * Try to parse a full 8-slot blueprint set from its text form.
 * @param text The text to parse; surrounding whitespace is tolerated.
 * @param[out] slots Filled with exactly #NUM_BLUEPRINT_SLOTS blueprints on success.
 * @return Whether the text was a valid blueprint set.
 */
static bool ParseBlueprintSetText(std::string_view text, std::vector<Blueprint> &slots)
{
	if (text.size() > 8 * 1024 * 1024) return false;

	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
	while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
	if (!text.starts_with(BLUEPRINT_SET_FILE_PREFIX)) return false;

	auto bytes = Base64Decode(text.substr(BLUEPRINT_SET_FILE_PREFIX.size()));
	if (!bytes.has_value()) return false;

	EndianBufferReader reader{*bytes};
	std::vector<Blueprint> result;
	ReadBlueprintVector(reader, result);
	if (result.size() != NUM_BLUEPRINT_SLOTS) return false;

	for (const Blueprint &bp : result) {
		if (bp.width > MAX_BLUEPRINT_DIMENSION || bp.height > MAX_BLUEPRINT_DIMENSION) return false;
		size_t corners = static_cast<size_t>(bp.width + 1) * (bp.height + 1);
		if (!bp.corner_heights.empty() && bp.corner_heights.size() != corners) return false;
	}

	slots = std::move(result);
	return true;
}

/**
 * Absolute path of the blueprint set directory.
 *
 * Deliberately not resolved via FioFOpenFile()'s multi-searchpath lookup: that mechanism tries the current
 * working directory before the personal data directory, which is wrong here specifically because this patch's
 * own `blueprint/` folder (containing this very file) can sit inside the source tree - i.e. inside the working
 * directory used to launch the game - and would silently "win" over the real, writable personal directory.
 * Reading, writing and listing all go through this single, unambiguous path instead.
 * @return The directory, always ending in a path separator.
 */
static std::string GetBlueprintBaseDirectory()
{
	return _personal_dir + "blueprint" PATHSEP;
}

/**
 * Write all blueprint slots to a file in the blueprint directory.
 * @param file_name File name including extension, relative to #GetBlueprintBaseDirectory.
 * @return Whether the file was written successfully.
 */
static bool WriteBlueprintSetFile(const std::string &file_name)
{
	auto file = FileHandle::Open(GetBlueprintBaseDirectory() + file_name, "wb");
	if (!file.has_value()) return false;

	std::string text = EncodeBlueprintSetText();
	return fwrite(text.data(), 1, text.size(), *file) == text.size();
}

/**
 * Read a full 8-slot blueprint set from a file in the blueprint directory.
 * @param file_name File name including extension, relative to #GetBlueprintBaseDirectory.
 * @param[out] slots Filled with exactly #NUM_BLUEPRINT_SLOTS blueprints on success.
 * @return Whether a valid blueprint set was read.
 */
static bool ReadBlueprintSetFile(const std::string &file_name, std::vector<Blueprint> &slots)
{
	auto file = FileHandle::Open(GetBlueprintBaseDirectory() + file_name, "rb");
	if (!file.has_value()) return false;

	std::string content;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), *file)) > 0) {
		content.append(buf, n);
		if (content.size() > 8 * 1024 * 1024) return false;
	}

	return ParseBlueprintSetText(content, slots);
}

/** File in the blueprint directory holding the automatic snapshot of all slots. */
static constexpr std::string_view BLUEPRINT_AUTOSAVE_FILE = "autosave.txt";

/**
 * Persist all blueprint slots to the autosave file. Called after every change
 * to a slot (copy, import, rename), so the slots survive a game restart;
 * failures are silent - the autosave is a convenience, not a player-initiated
 * export.
 */
static void SaveBlueprintAutosave()
{
	WriteBlueprintSetFile(std::string{BLUEPRINT_AUTOSAVE_FILE});
}

/** Restore the blueprint slots from the autosave file, once per game launch. */
static void LoadBlueprintAutosave()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	std::vector<Blueprint> slots;
	if (!ReadBlueprintSetFile(std::string{BLUEPRINT_AUTOSAVE_FILE}, slots)) return;
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) GetBlueprint(i) = std::move(slots[i]);
}

/** Client-side state of the blueprint toolbar. */
struct BlueprintToolbarState {
	uint8_t slot = 0;              ///< Active blueprint slot (0-based).
	BlueprintPasteOptions options; ///< Filters, transformation and terraform settings for pasting.
};

/** Current state of the blueprint toolbar; kept while the game is running. */
static BlueprintToolbarState _blueprint_state;

/** Restore the toolbar toggles from their openttd.cfg settings, once per game launch. */
static void LoadBlueprintStateFromSettings()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	const GUISettings &s = _settings_client.gui;
	BlueprintPasteOptions &o = _blueprint_state.options;
	o.paste_rail = s.blueprint_paste_rail;
	o.paste_road = s.blueprint_paste_road;
	o.paste_water = s.blueprint_paste_water;
	o.paste_air = s.blueprint_paste_air;
	o.convert_railtype = s.blueprint_convert_railtype;
	o.mirror_signals = s.blueprint_mirror_signals;
	o.upgrade_bridges = s.blueprint_upgrade_bridges;
	o.with_stations = s.blueprint_with_stations;
	o.terraform_mode = static_cast<BlueprintTerraformMode>(std::min<uint8_t>(s.blueprint_terraform_mode, 2));
}

/** Write the toolbar toggles through to their openttd.cfg settings, so they survive a game restart. */
static void SaveBlueprintStateToSettings()
{
	GUISettings &s = _settings_client.gui;
	const BlueprintPasteOptions &o = _blueprint_state.options;
	s.blueprint_paste_rail = o.paste_rail;
	s.blueprint_paste_road = o.paste_road;
	s.blueprint_paste_water = o.paste_water;
	s.blueprint_paste_air = o.paste_air;
	s.blueprint_convert_railtype = o.convert_railtype;
	s.blueprint_mirror_signals = o.mirror_signals;
	s.blueprint_upgrade_bridges = o.upgrade_bridges;
	s.blueprint_with_stations = o.with_stations;
	s.blueprint_terraform_mode = static_cast<uint8_t>(o.terraform_mode);
}

/** Cached paste preview overlays; rebuilt lazily when #_paste_preview_dirty. */
static BlueprintPastePreview _paste_preview;
static bool _paste_preview_dirty = true;
static BlueprintPasteData _paste_preview_data;         ///< Prepared paste data matching #_paste_preview.
static TileIndex _paste_preview_origin = INVALID_TILE; ///< Origin the blocked mask of #_paste_preview was computed for.
static Money _paste_preview_cost = 0;                  ///< Estimated cost of pasting at #_paste_preview_origin; 0 when unknown.

/** Rebuild #_paste_preview from the active slot, transformation and filters. */
static void RebuildPastePreview()
{
	_paste_preview_dirty = false;
	_paste_preview_origin = INVALID_TILE;

	_paste_preview_data = PrepareBlueprintPaste(GetBlueprint(_blueprint_state.slot), _blueprint_state.options, GetCurrentRailType());
	const Blueprint &bp = _paste_preview_data.blueprint;

	_paste_preview.width = bp.width;
	_paste_preview.height = bp.height;
	_paste_preview.tiles.assign(static_cast<size_t>(bp.width) * bp.height, 0);
	_paste_preview.blocked.assign(static_cast<size_t>(bp.width) * bp.height, false);

	auto mark = [&](int x, int y, TrackBits bits) {
		if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
		_paste_preview.tiles[static_cast<size_t>(y) * bp.width + x] |= bits.base();
	};
	auto mark_bit = [&](int x, int y, uint8_t bit) {
		if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
		SetBit(_paste_preview.tiles[static_cast<size_t>(y) * bp.width + x], bit);
	};
	auto highlight = [&](int x, int y) {
		mark_bit(x, y, BLUEPRINT_PREVIEW_HIGHLIGHT_BIT);
	};

	for (const BlueprintRailTrack &rt : bp.rail_tracks) mark(rt.offset.x, rt.offset.y, TrackBits{rt.track});
	for (const BlueprintRailDepot &depot : bp.rail_depots) highlight(depot.offset.x, depot.offset.y);
	for (const BlueprintRoad &road : bp.roads) {
		/* Show straight-through roads as a line overlay, anything else as a highlight. */
		if (road.bits == ROAD_X) {
			mark(road.offset.x, road.offset.y, TrackBits{Track::X});
		} else if (road.bits == ROAD_Y) {
			mark(road.offset.x, road.offset.y, TrackBits{Track::Y});
		} else {
			highlight(road.offset.x, road.offset.y);
		}
	}
	for (const BlueprintRoadDepot &depot : bp.road_depots) highlight(depot.offset.x, depot.offset.y);
	for (const BlueprintCanal &canal : bp.canals) highlight(canal.offset.x, canal.offset.y);
	for (const BlueprintLock &lock : bp.locks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
		highlight(lock.offset.x - d.x, lock.offset.y - d.y);
		highlight(lock.offset.x, lock.offset.y);
		highlight(lock.offset.x + d.x, lock.offset.y + d.y);
	}
	for (const BlueprintShipDepot &depot : bp.ship_depots) {
		highlight(depot.offset.x, depot.offset.y);
		highlight(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0));
	}
	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		highlight(tb.offset.x, tb.offset.y);
		highlight(tb.other_end.x, tb.other_end.y);
	}
	for (const BlueprintStationTile &st : bp.station_tiles) {
		mark(st.offset.x, st.offset.y, st.axis == Axis::X ? TrackBits{Track::X} : TrackBits{Track::Y});
		mark_bit(st.offset.x, st.offset.y, BLUEPRINT_PREVIEW_HIGHLIGHT_BIT);
	}
	for (const BlueprintRoadStop &stop : bp.road_stops) highlight(stop.offset.x, stop.offset.y);
	for (const BlueprintDock &dock : bp.docks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
		highlight(dock.offset.x, dock.offset.y);
		highlight(dock.offset.x + d.x, dock.offset.y + d.y);
	}
	for (const BlueprintBuoy &buoy : bp.buoys) highlight(buoy.offset.x, buoy.offset.y);
	for (const BlueprintAirport &airport : bp.airports) {
		for (uint dy = 0; dy < airport.h; dy++) {
			for (uint dx = 0; dx < airport.w; dx++) highlight(airport.offset.x + dx, airport.offset.y + dy);
		}
	}
}

/**
 * Would this tile block the paste even with terraforming?
 * @param tile Target tile.
 * @param wants_track Whether the pasted element on this tile carries tracks or roads.
 * @return True when a hard obstacle occupies the tile.
 */
static bool BlueprintTileBlocked(TileIndex tile, bool wants_track)
{
	switch (GetTileType(tile)) {
		case TileType::Clear:
		case TileType::Trees:
			return false;

		case TileType::Water:
			return wants_track;

		case TileType::Railway:
		case TileType::Road:
		case TileType::Station:
		case TileType::TunnelBridge: {
			Owner owner = GetTileOwner(tile);
			return owner != _local_company && owner != OWNER_NONE && owner != OWNER_TOWN;
		}

		default: /* Houses, industries, objects, void, ... */
			return true;
	}
}

/** Test-run a build command for the paste preview; only success matters. */
template <Commands Tcmd, typename... Targs>
static bool PreviewTest(Targs... args)
{
	auto result = Command<Tcmd>::Do(DoCommandFlags{}, args...);
	if constexpr (std::is_same_v<std::decay_t<decltype(result)>, CommandCost>) {
		return result.Succeeded();
	} else {
		return std::get<0>(result).Succeeded();
	}
}

/**
 * Recompute the blocked mask of #_paste_preview for a new target position.
 *
 * With terraforming enabled the paste flattens the land first, so only hard
 * obstacles are marked. Without terraforming the actual build commands are
 * test-run on the current landscape; parts whose success depends on other
 * parts of the same paste (signals, waypoint surfaces, ship depots on
 * to-be-built canals) are left optimistic.
 * @param origin Northern tile of the target area.
 */
static void RecomputePasteBlocked(TileIndex origin)
{
	_paste_preview_origin = origin;

	/* Estimated cost of the whole paste at this origin: a test run of the real
	 * command, so terraforming and skipped parts are priced exactly like the
	 * eventual click. Shown as a hover tooltip by the toolbar window. */
	_paste_preview_cost = 0;
	if (_settings_client.gui.measure_tooltip) {
		CommandCost cost = Command<Commands::PasteBlueprint>::Do(DoCommandFlags{}, origin, _paste_preview_data);
		if (cost.Succeeded()) _paste_preview_cost = cost.GetCost();
	}

	const Blueprint &bp = _paste_preview_data.blueprint;
	_paste_preview.blocked.assign(_paste_preview.tiles.size(), false);

	auto block = [&](int x, int y) {
		if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
		_paste_preview.blocked[static_cast<size_t>(y) * bp.width + x] = true;
	};
	auto tile_valid = [&](TileIndexDiffC offset) {
		return TileX(origin) + offset.x <= Map::MaxX() && TileY(origin) + offset.y <= Map::MaxY();
	};
	auto target = [&](TileIndexDiffC offset) {
		return TileAddXY(origin, offset.x, offset.y);
	};

	if (_paste_preview_data.terraform_mode != BlueprintTerraformMode::None) {
		/* The land will be flattened first; only hard obstacles block the paste. */
		for (uint y = 0; y < bp.height; y++) {
			for (uint x = 0; x < bp.width; x++) {
				uint8_t overlay = _paste_preview.tiles[static_cast<size_t>(y) * bp.width + x];
				if (overlay == 0) continue;
				TileIndexDiffC offset = {static_cast<int16_t>(x), static_cast<int16_t>(y)};
				if (!tile_valid(offset) || BlueprintTileBlocked(target(offset), GB(overlay, 0, 6) != 0)) block(x, y);
			}
		}
		return;
	}

	/* No terraforming: test-run the build commands on the current landscape. */
	for (const BlueprintCanal &canal : bp.canals) {
		if (!tile_valid(canal.offset)) { block(canal.offset.x, canal.offset.y); continue; }
		if (!PreviewTest<Commands::BuildCanal>(target(canal.offset), target(canal.offset), WaterClass::Canal, false)) block(canal.offset.x, canal.offset.y);
	}
	for (const BlueprintLock &lock : bp.locks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
		if (tile_valid(lock.offset) && PreviewTest<Commands::BuildLock>(target(lock.offset))) continue;
		block(lock.offset.x - d.x, lock.offset.y - d.y);
		block(lock.offset.x, lock.offset.y);
		block(lock.offset.x + d.x, lock.offset.y + d.y);
	}
	if (bp.canals.empty() && bp.locks.empty()) {
		/* Only meaningful when the depot does not sit on water built by the same paste. */
		for (const BlueprintShipDepot &depot : bp.ship_depots) {
			if (tile_valid(depot.offset) && PreviewTest<Commands::BuildShipDepot>(target(depot.offset), depot.axis)) continue;
			block(depot.offset.x, depot.offset.y);
			block(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0));
		}
	}
	for (const BlueprintRailTrack &rt : bp.rail_tracks) {
		if (tile_valid(rt.offset) && PreviewTest<Commands::BuildRail>(target(rt.offset), rt.railtype, rt.track, false)) continue;
		block(rt.offset.x, rt.offset.y);
	}
	for (const BlueprintRailDepot &depot : bp.rail_depots) {
		if (tile_valid(depot.offset) && PreviewTest<Commands::BuildRailDepot>(target(depot.offset), depot.railtype, depot.dir)) continue;
		block(depot.offset.x, depot.offset.y);
	}
	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		/* The build commands take the rail type and the road type separately;
		 * only the one that matches the transport type is looked at. */
		RailType railtype = tb.transport == TransportType::Rail ? tb.railtype : INVALID_RAILTYPE;
		RoadType roadtype = INVALID_ROADTYPE;
		if (tb.transport == TransportType::Road) roadtype = tb.roadtype != INVALID_ROADTYPE ? tb.roadtype : tb.tramtype;
		if (tile_valid(tb.offset) && tile_valid(tb.other_end)) {
			bool ok = tb.is_bridge ?
					PreviewTest<Commands::BuildBridge>(target(tb.other_end), target(tb.offset), tb.transport, tb.bridge_type, railtype, roadtype) :
					PreviewTest<Commands::BuildTunnel>(target(tb.offset), tb.transport, railtype, roadtype);
			if (ok) continue;
		}
		block(tb.offset.x, tb.offset.y);
		block(tb.other_end.x, tb.other_end.y);
	}
	for (const BlueprintRoad &road : bp.roads) {
		if (tile_valid(road.offset) && PreviewTest<Commands::BuildRoad>(target(road.offset), road.bits, road.roadtype, DisallowedRoadDirections{}, TownID::Invalid())) continue;
		block(road.offset.x, road.offset.y);
	}
	for (const BlueprintRoadDepot &depot : bp.road_depots) {
		if (tile_valid(depot.offset) && PreviewTest<Commands::BuildRoadDepot>(target(depot.offset), depot.roadtype, depot.dir)) continue;
		block(depot.offset.x, depot.offset.y);
	}
	for (const BlueprintStationRect &rect : GroupStationTiles(bp)) {
		bool ok;
		if (rect.is_waypoint) {
			/* Test the tracks the waypoint will sit on; the waypoint itself follows them. */
			Track track = AxisToTrack(rect.axis);
			for (uint dy = 0; dy < rect.h; dy++) {
				for (uint dx = 0; dx < rect.w; dx++) {
					TileIndexDiffC offset = {static_cast<int16_t>(rect.offset.x + dx), static_cast<int16_t>(rect.offset.y + dy)};
					if (tile_valid(offset) && PreviewTest<Commands::BuildRail>(target(offset), rect.railtype, track, false)) continue;
					block(offset.x, offset.y);
				}
			}
			continue;
		}
		uint8_t numtracks = static_cast<uint8_t>(rect.axis == Axis::X ? rect.h : rect.w);
		uint8_t plat_len = static_cast<uint8_t>(rect.axis == Axis::X ? rect.w : rect.h);
		ok = tile_valid(rect.offset) && PreviewTest<Commands::BuildRailStation>(target(rect.offset), rect.railtype, rect.axis, numtracks, plat_len,
				STAT_CLASS_DFLT, 0, NEW_STATION, true);
		if (ok) continue;
		for (uint dy = 0; dy < rect.h; dy++) {
			for (uint dx = 0; dx < rect.w; dx++) block(rect.offset.x + dx, rect.offset.y + dy);
		}
	}
	for (const BlueprintRoadStop &stop : bp.road_stops) {
		RoadType rt = stop.roadtype != INVALID_ROADTYPE ? stop.roadtype : stop.tramtype;
		DiagDirection ddir = stop.drive_through ? AxisToDiagDir(stop.axis) : stop.dir;
		bool ok = false;
		if (tile_valid(stop.offset)) {
			if (stop.type == StationType::RoadWaypoint) {
				/* Test the road the waypoint will sit on; the waypoint itself follows it. */
				ok = PreviewTest<Commands::BuildRoad>(target(stop.offset), AxisToRoadBits(stop.axis), rt, DisallowedRoadDirections{}, TownID::Invalid());
			} else if (IsValidDiagDirection(ddir)) {
				RoadStopType stop_type = stop.type == StationType::Truck ? RoadStopType::Truck : RoadStopType::Bus;
				ok = PreviewTest<Commands::BuildRoadStop>(target(stop.offset), 1, 1, stop_type, stop.drive_through,
						ddir, rt, ROADSTOP_CLASS_DFLT, 0, NEW_STATION, true);
			}
		}
		if (!ok) block(stop.offset.x, stop.offset.y);
	}
	for (const BlueprintDock &dock : bp.docks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
		if (tile_valid(dock.offset) && PreviewTest<Commands::BuildDock>(target(dock.offset), NEW_STATION, true)) continue;
		block(dock.offset.x, dock.offset.y);
		block(dock.offset.x + d.x, dock.offset.y + d.y);
	}
	for (const BlueprintBuoy &buoy : bp.buoys) {
		if (tile_valid(buoy.offset) && PreviewTest<Commands::BuildBuoy>(target(buoy.offset))) continue;
		block(buoy.offset.x, buoy.offset.y);
	}
	for (const BlueprintAirport &airport : bp.airports) {
		if (tile_valid(airport.offset) && PreviewTest<Commands::BuildAirport>(target(airport.offset), airport.type, airport.layout, NEW_STATION, true)) continue;
		for (uint dy = 0; dy < airport.h; dy++) {
			for (uint dx = 0; dx < airport.w; dx++) block(airport.offset.x + dx, airport.offset.y + dy);
		}
	}
}

/** Tile categories of the hover preview schematic, in increasing draw priority. */
enum class BlueprintPreviewCell : uint8_t {
	Empty,   ///< Nothing on this tile.
	Water,   ///< Canals, locks, ship depots, buoys, aqueducts.
	Road,    ///< Roads, road depots, road tunnels and bridges.
	Rail,    ///< Rail tracks, rail depots, rail tunnels and bridges.
	Airport, ///< Airport tiles.
	Station, ///< Stations, waypoints, road stops, docks.
};

/** Colours the schematic cells are drawn in, indexed by #BlueprintPreviewCell. */
static constexpr PixelColour BLUEPRINT_PREVIEW_CELL_COLOURS[] = {
	PC_BLACK,      // Empty (not drawn)
	PC_LIGHT_BLUE, // Water
	PC_ORANGE,     // Road
	PC_WHITE,      // Rail
	PC_RED,        // Airport
	PC_YELLOW,     // Station
};

/** Downscaled top-down schematic of a blueprint for the hover preview. */
struct BlueprintSchematic {
	uint16_t width;  ///< Extent along the X axis, in tiles.
	uint16_t height; ///< Extent along the Y axis, in tiles.
	std::vector<BlueprintPreviewCell> cells; ///< Cell categories, index = y * width + x.

	BlueprintSchematic(const Blueprint &bp) : width(bp.width), height(bp.height)
	{
		this->cells.assign(static_cast<size_t>(this->width) * this->height, BlueprintPreviewCell::Empty);
		auto mark = [&](int x, int y, BlueprintPreviewCell cell) {
			if (x < 0 || y < 0 || x >= this->width || y >= this->height) return;
			BlueprintPreviewCell &current = this->cells[static_cast<size_t>(y) * this->width + x];
			if (cell > current) current = cell;
		};

		for (const BlueprintRailTrack &rt : bp.rail_tracks) mark(rt.offset.x, rt.offset.y, BlueprintPreviewCell::Rail);
		for (const BlueprintRailDepot &depot : bp.rail_depots) mark(depot.offset.x, depot.offset.y, BlueprintPreviewCell::Rail);
		for (const BlueprintRoad &road : bp.roads) mark(road.offset.x, road.offset.y, BlueprintPreviewCell::Road);
		for (const BlueprintRoadDepot &depot : bp.road_depots) mark(depot.offset.x, depot.offset.y, BlueprintPreviewCell::Road);
		for (const BlueprintCanal &canal : bp.canals) mark(canal.offset.x, canal.offset.y, BlueprintPreviewCell::Water);
		for (const BlueprintLock &lock : bp.locks) {
			TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
			for (int i = -1; i <= 1; i++) mark(lock.offset.x + i * d.x, lock.offset.y + i * d.y, BlueprintPreviewCell::Water);
		}
		for (const BlueprintShipDepot &depot : bp.ship_depots) {
			mark(depot.offset.x, depot.offset.y, BlueprintPreviewCell::Water);
			mark(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0), BlueprintPreviewCell::Water);
		}
		for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
			BlueprintPreviewCell cell = tb.transport == TransportType::Rail ? BlueprintPreviewCell::Rail :
					tb.transport == TransportType::Road ? BlueprintPreviewCell::Road : BlueprintPreviewCell::Water;
			int dx = (tb.other_end.x > tb.offset.x) - (tb.other_end.x < tb.offset.x);
			int dy = (tb.other_end.y > tb.offset.y) - (tb.other_end.y < tb.offset.y);
			int x = tb.offset.x;
			int y = tb.offset.y;
			mark(x, y, cell);
			for (uint steps = 0; (x != tb.other_end.x || y != tb.other_end.y) && steps < MAX_BLUEPRINT_DIMENSION; steps++) {
				x += dx;
				y += dy;
				mark(x, y, cell);
			}
		}
		for (const BlueprintStationTile &st : bp.station_tiles) mark(st.offset.x, st.offset.y, BlueprintPreviewCell::Station);
		for (const BlueprintRoadStop &stop : bp.road_stops) mark(stop.offset.x, stop.offset.y, BlueprintPreviewCell::Station);
		for (const BlueprintDock &dock : bp.docks) {
			TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
			mark(dock.offset.x, dock.offset.y, BlueprintPreviewCell::Station);
			mark(dock.offset.x + d.x, dock.offset.y + d.y, BlueprintPreviewCell::Station);
		}
		for (const BlueprintBuoy &buoy : bp.buoys) mark(buoy.offset.x, buoy.offset.y, BlueprintPreviewCell::Water);
		for (const BlueprintAirport &airport : bp.airports) {
			for (uint dy2 = 0; dy2 < airport.h; dy2++) {
				for (uint dx2 = 0; dx2 < airport.w; dx2++) mark(airport.offset.x + dx2, airport.offset.y + dy2, BlueprintPreviewCell::Airport);
			}
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_preview_widgets = {
	NWidget(WWT_EMPTY, Colours::Invalid, WID_TT_BACKGROUND),
};

static WindowDesc _blueprint_preview_desc(
	WindowPosition::Manual, {}, 0, 0, // Coordinates and sizes are not used,
	WindowClass::ToolTips, WindowClass::None,
	{WindowDefaultFlag::NoFocus, WindowDefaultFlag::NoClose},
	_nested_blueprint_preview_widgets
);

/** Tooltip-style window showing a schematic preview of a blueprint slot. */
struct BlueprintPreviewWindow : public Window {
	BlueprintSchematic schematic;       ///< Schematic of the previewed blueprint.
	std::string name;                   ///< Name of the previewed blueprint; empty when unnamed.
	TooltipCloseCondition close_cond{}; ///< Condition for closing the window.

	BlueprintPreviewWindow(Window *parent, const Blueprint &blueprint, TooltipCloseCondition close_cond) :
			Window(_blueprint_preview_desc), schematic(blueprint), name(blueprint.name), close_cond(close_cond)
	{
		this->parent = parent;
		this->InitNested();
		this->flags.Reset(WindowFlag::WhiteBorder);
	}

	/** Get the drawn size of one schematic cell in pixels. */
	int GetCellSize() const
	{
		int max_dim = std::max<int>(std::max(this->schematic.width, this->schematic.height), 1);
		return Clamp(ScaleGUITrad(160) / max_dim, 1, ScaleGUITrad(5));
	}

	/** Height reserved for the name line, including the gap to the schematic; 0 when unnamed. */
	int GetNameHeight() const
	{
		if (this->name.empty()) return 0;
		return GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
	}

	Point OnInitialPosition([[maybe_unused]] int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		/* Position like a tooltip: below the cursor, flipped above it when there is no room. */
		int scr_top = GetMainViewTop() + 2;
		int scr_bot = GetMainViewBottom() - 2;

		Point pt;
		pt.y = SoftClamp(_cursor.pos.y + _cursor.total_size.y + _cursor.total_offs.y + 5, scr_top, scr_bot);
		if (pt.y + sm_height > scr_bot) pt.y = std::min(_cursor.pos.y + _cursor.total_offs.y - 5, scr_bot) - sm_height;
		pt.x = sm_width >= _screen.width ? 0 : SoftClamp(_cursor.pos.x - (sm_width >> 1), 0, _screen.width - sm_width);

		return pt;
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_TT_BACKGROUND) return;

		int cell = this->GetCellSize();
		size.width  = std::max<uint>(this->schematic.width * cell, GetStringBoundingBox(this->name).width)
				+ WidgetDimensions::scaled.framerect.Horizontal() + WidgetDimensions::scaled.fullbevel.Horizontal();
		size.height = this->schematic.height * cell + this->GetNameHeight()
				+ WidgetDimensions::scaled.framerect.Vertical() + WidgetDimensions::scaled.fullbevel.Vertical();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TT_BACKGROUND) return;
		GfxFillRect(r, PC_BLACK);
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		GfxFillRect(ir, PC_DARK_GREY);

		Rect content = r.Shrink(WidgetDimensions::scaled.framerect).Shrink(WidgetDimensions::scaled.fullbevel);
		int name_height = this->GetNameHeight();
		if (name_height > 0) {
			DrawString(content.left, content.right, content.top, this->name, TextColour::White, AlignmentH::Centre);
			content.top += name_height;
		}

		int cell = this->GetCellSize();
		int ox = content.left + (content.Width() - this->schematic.width * cell) / 2;
		int oy = content.top + (content.Height() - this->schematic.height * cell) / 2;

		for (uint y = 0; y < this->schematic.height; y++) {
			for (uint x = 0; x < this->schematic.width; x++) {
				BlueprintPreviewCell cell_content = this->schematic.cells[static_cast<size_t>(y) * this->schematic.width + x];
				if (cell_content == BlueprintPreviewCell::Empty) continue;
				GfxFillRect(ox + static_cast<int>(x) * cell, oy + static_cast<int>(y) * cell,
						ox + static_cast<int>(x + 1) * cell - 1, oy + static_cast<int>(y + 1) * cell - 1,
						BLUEPRINT_PREVIEW_CELL_COLOURS[static_cast<size_t>(cell_content)]);
			}
		}
	}

	void OnMouseLoop() override
	{
		/* Always close when the cursor is not in our window. */
		if (!_cursor.in_window) {
			this->Close();
			return;
		}

		switch (this->close_cond) {
			case TooltipCloseCondition::RightClick: if (!_right_button_down) this->Close(); break;
			case TooltipCloseCondition::Hover: if (!_mouse_hovering) this->Close(); break;
			case TooltipCloseCondition::None: break;

			case TooltipCloseCondition::ExitViewport: {
				Window *w = FindWindowFromPt(_cursor.pos.x, _cursor.pos.y);
				if (w == nullptr || IsPtInWindowViewport(w, _cursor.pos.x, _cursor.pos.y) == nullptr) this->Close();
				break;
			}
		}
	}
};

/**
 * Show the schematic hover preview of a blueprint, replacing any tooltip.
 * @param parent The window the preview belongs to.
 * @param blueprint The blueprint to preview.
 * @param close_cond Condition under which the preview closes again.
 */
static void ShowBlueprintPreview(Window *parent, const Blueprint &blueprint, TooltipCloseCondition close_cond)
{
	CloseWindowById(WindowClass::ToolTips, 0);
	if (!_cursor.in_window) return;
	new BlueprintPreviewWindow(parent, blueprint, close_cond);
}

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_text_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BTX_CAPTION), SetStringTip(STR_BLUEPRINT_EXPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(WWT_EDITBOX, Colours::DarkGreen, WID_BTX_TEXT), SetMinimalSize(320, 0), SetFill(1, 0), SetPadding(2, 2, 2, 2),
	EndContainer(),
	NWidget(NWID_SELECTION, Colours::Invalid, WID_BTX_SEL),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_COPY), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_COPY_TO_CLIPBOARD),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_PASTE), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_PASTE_FROM_CLIPBOARD),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_CANCEL), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BUTTON_CANCEL),
				NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_OK), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_IMPORT),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _blueprint_text_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::BlueprintText, WindowClass::BlueprintToolbar,
	{},
	_nested_blueprint_text_widgets
);

/** Dialog showing a blueprint as text (export) or accepting blueprint text (import). */
struct BlueprintTextWindow : Window {
	QueryString text_editbox;         ///< Editbox holding the blueprint text.
	bool import_mode;                 ///< True for the import dialog, false for the export dialog.
	std::string export_text;          ///< Full text of the exported blueprint; may exceed the editbox capacity.
	std::optional<Blueprint> pending; ///< Parsed blueprint waiting for the overwrite confirmation.

	BlueprintTextWindow(WindowDesc &desc, Window *parent, std::string &&text, bool import_mode) :
			Window(desc), text_editbox(UINT16_MAX - 1), import_mode(import_mode), export_text(std::move(text))
	{
		this->parent = parent;
		this->CreateNestedTree();
		this->GetWidget<NWidgetStacked>(WID_BTX_SEL)->SetDisplayedPlane(import_mode ? 1 : 0);
		this->querystrings[WID_BTX_TEXT] = &this->text_editbox;
		this->text_editbox.caption = import_mode ? STR_BLUEPRINT_IMPORT_CAPTION : STR_BLUEPRINT_EXPORT_CAPTION;
		this->FinishInitNested(0);

		if (import_mode) {
			this->text_editbox.ok_button = WID_BTX_OK;
			this->text_editbox.cancel_button = WID_BTX_CANCEL;
		} else {
			this->text_editbox.text.Assign(this->export_text);
		}
		this->SetFocusedWidget(WID_BTX_TEXT);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_BTX_CAPTION) {
			return GetString(this->import_mode ? STR_BLUEPRINT_IMPORT_CAPTION : STR_BLUEPRINT_EXPORT_CAPTION);
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BTX_COPY:
				if (!SetClipboardContents(this->export_text)) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_CLIPBOARD_EXPORT), {}, WarningLevel::Info);
				}
				break;

			case WID_BTX_PASTE:
				if (this->text_editbox.text.InsertClipboard()) {
					this->SetWidgetDirty(WID_BTX_TEXT);
					this->OnEditboxChanged(WID_BTX_TEXT);
				} else {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_CLIPBOARD_PASTE), {}, WarningLevel::Info);
				}
				break;

			case WID_BTX_CANCEL:
				this->Close();
				break;

			case WID_BTX_OK:
				this->TryImport();
				break;

			default: break;
		}
	}

	/** Parse the entered text; ask for confirmation when the active slot is occupied. */
	void TryImport()
	{
		Blueprint blueprint;
		if (!ParseBlueprintText(this->text_editbox.text.GetText(), blueprint)) {
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_INVALID_TEXT), {}, WarningLevel::Info);
			return;
		}

		if (!GetBlueprint(_blueprint_state.slot).IsEmpty()) {
			this->pending = std::move(blueprint);
			ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE),
					this, BlueprintTextWindow::OverwriteCallback);
			return;
		}

		this->ApplyImport(std::move(blueprint));
	}

	/** Store the imported blueprint in the active slot and close the dialog. */
	void ApplyImport(Blueprint &&blueprint)
	{
		GetBlueprint(_blueprint_state.slot) = std::move(blueprint);
		SaveBlueprintAutosave();
		_paste_preview_dirty = true;
		SetWindowDirty(WindowClass::BlueprintToolbar, 0);
		this->Close();
	}

	/** Callback of the overwrite confirmation query; the parent is the text dialog. */
	static void OverwriteCallback(Window *w, bool confirmed)
	{
		BlueprintTextWindow *btw = static_cast<BlueprintTextWindow *>(w);
		if (!confirmed || !btw->pending.has_value()) {
			btw->pending.reset();
			return;
		}
		Blueprint blueprint = std::move(*btw->pending);
		btw->pending.reset();
		btw->ApplyImport(std::move(blueprint));
	}
};

/**
 * Open the blueprint text dialog, replacing any open one.
 * @param parent The blueprint toolbar.
 * @param text Text to show (export mode); empty for import mode.
 * @param import_mode True for the import dialog, false for the export dialog.
 */
static void ShowBlueprintTextWindow(Window *parent, std::string &&text, bool import_mode)
{
	CloseWindowById(WindowClass::BlueprintText, 0);
	new BlueprintTextWindow(_blueprint_text_desc, parent, std::move(text), import_mode);
}

/** Whether any blueprint slot currently has content or a name worth warning about before overwriting. */
static bool AnyBlueprintSlotOccupied()
{
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) {
		const Blueprint &bp = GetBlueprint(i);
		if (!bp.IsEmpty() || !bp.name.empty()) return true;
	}
	return false;
}

/** Replace all blueprint slots with the given set and refresh the toolbar/preview. */
static void ApplyImportedBlueprintSet(std::vector<Blueprint> &&slots)
{
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) GetBlueprint(i) = std::move(slots[i]);
	SaveBlueprintAutosave();
	_paste_preview_dirty = true;
	SetWindowDirty(WindowClass::BlueprintToolbar, 0);
}

/** One entry in a #BlueprintFileWindow directory listing. */
struct BlueprintFileEntry {
	std::string name; ///< File or directory name, without any path.
	bool is_dir;       ///< True for directories (and the synthetic ".." parent entry).
};

/**
 * List the blueprint-set files and subdirectories directly inside a directory.
 * @param relative_dir Directory to list, relative to #GetBlueprintBaseDirectory, empty for the root.
 * @return The entries, subdirectories first, each group sorted by name; a ".." entry is prepended when not at the root.
 */
static std::vector<BlueprintFileEntry> ListBlueprintDirectory(const std::string &relative_dir)
{
	std::vector<BlueprintFileEntry> entries;

	std::error_code error_code;
	for (const auto &dir_entry : std::filesystem::directory_iterator(OTTD2FS(GetBlueprintBaseDirectory() + relative_dir), error_code)) {
		std::string name = FS2OTTD(dir_entry.path().filename().native());
		if (name.empty() || name.front() == '.') continue;

		if (dir_entry.is_directory()) {
			entries.push_back({std::move(name), true});
		} else if (std::string_view{name}.ends_with(BLUEPRINT_SET_FILE_EXTENSION)) {
			entries.push_back({std::move(name), false});
		}
	}

	std::sort(entries.begin(), entries.end(), [](const BlueprintFileEntry &a, const BlueprintFileEntry &b) {
		if (a.is_dir != b.is_dir) return a.is_dir;
		return a.name < b.name;
	});

	if (!relative_dir.empty()) entries.insert(entries.begin(), BlueprintFileEntry{"..", true});
	return entries;
}

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_file_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BTF_CAPTION), SetStringTip(STR_BLUEPRINT_QUERY_EXPORT_FILE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(WWT_LABEL, Colours::Invalid, WID_BTF_PATH), SetFill(1, 0), SetPadding(2, 2, 0, 2), SetAlignment(AlignmentH::Start),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_MATRIX, Colours::DarkGreen, WID_BTF_LIST), SetMinimalSize(300, 0), SetFill(1, 1), SetResize(0, 1),
				SetMatrixDataTip(1, 0, STR_BLUEPRINT_TOOLTIP_FILE_LIST), SetScrollbar(WID_BTF_SCROLLBAR),
		NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_BTF_SCROLLBAR),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(WWT_EDITBOX, Colours::DarkGreen, WID_BTF_NAME), SetMinimalSize(300, 0), SetFill(1, 0), SetPadding(2, 2, 2, 2),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTF_CANCEL), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BUTTON_CANCEL),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTF_OK), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_EXPORT),
	EndContainer(),
};

static WindowDesc _blueprint_file_desc(
	WindowPosition::Center, {}, 0, 0,
	WindowClass::BlueprintFile, WindowClass::BlueprintToolbar,
	WindowDefaultFlag::Construction,
	_nested_blueprint_file_widgets
);

/** Dialog for exporting/importing all blueprint slots to/from a named file, with directory browsing. */
struct BlueprintFileWindow : Window {
	bool import_mode;                          ///< True for the import dialog, false for the export dialog.
	std::string current_dir;                   ///< Directory being browsed, relative to GetBlueprintBaseDirectory(); empty at the root.
	std::vector<BlueprintFileEntry> entries;    ///< Contents of #current_dir.
	Scrollbar *vscroll = nullptr;
	QueryString name_editbox;                  ///< Editbox holding the file name (without extension).
	std::string pending_overwrite_file;         ///< File name awaiting an overwrite confirmation (export).
	std::vector<Blueprint> pending_import;      ///< Blueprint set awaiting a replace-all confirmation (import).

	BlueprintFileWindow(WindowDesc &desc, Window *parent, bool import_mode) :
			Window(desc), import_mode(import_mode), name_editbox(MAX_LENGTH_BLUEPRINT_FILE_NAME_CHARS * MAX_CHAR_LENGTH, MAX_LENGTH_BLUEPRINT_FILE_NAME_CHARS)
	{
		this->parent = parent;
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_BTF_SCROLLBAR);
		this->querystrings[WID_BTF_NAME] = &this->name_editbox;
		this->name_editbox.caption = import_mode ? STR_BLUEPRINT_QUERY_IMPORT_FILE_CAPTION : STR_BLUEPRINT_QUERY_EXPORT_FILE_CAPTION;
		this->name_editbox.ok_button = WID_BTF_OK;
		this->name_editbox.cancel_button = WID_BTF_CANCEL;
		this->GetWidget<NWidgetCore>(WID_BTF_OK)->SetString(import_mode ? STR_BLUEPRINT_IMPORT : STR_BLUEPRINT_EXPORT);
		this->FinishInitNested(0);
		this->RefreshFileList();
		this->SetFocusedWidget(WID_BTF_NAME);
	}

	/** Rescan #current_dir and reset the list scroll position. */
	void RefreshFileList()
	{
		this->entries = ListBlueprintDirectory(this->current_dir);
		this->vscroll->SetCount(this->entries.size());
		this->SetWidgetDirty(WID_BTF_LIST);
		this->SetWidgetDirty(WID_BTF_PATH);
	}

	/** Step #current_dir into a subdirectory, or up to its parent for "..". */
	void Navigate(const std::string &dir_name)
	{
		if (dir_name == "..") {
			/* Strip the trailing path separator, then everything after the next-to-last one. */
			size_t end = this->current_dir.size() - 1;
			size_t slash = this->current_dir.find_last_of(PATHSEPCHAR, end > 0 ? end - 1 : 0);
			this->current_dir = (slash == std::string::npos) ? std::string{} : this->current_dir.substr(0, slash + 1);
		} else {
			this->current_dir += dir_name + PATHSEP;
		}
		this->RefreshFileList();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_BTF_CAPTION:
				return GetString(this->import_mode ? STR_BLUEPRINT_QUERY_IMPORT_FILE_CAPTION : STR_BLUEPRINT_QUERY_EXPORT_FILE_CAPTION);

			case WID_BTF_PATH:
				return std::string{"blueprint" PATHSEP} + this->current_dir;

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_BTF_LIST) return;
		fill.height = resize.height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.matrix.Vertical();
		size.height = 8 * resize.height;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_BTF_LIST) return;

		Rect ir = r.WithHeight(this->resize.step_height).Shrink(WidgetDimensions::scaled.matrix);
		auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->entries);
		for (auto it = first; it != last; ++it) {
			std::string label = it->is_dir ? (it->name + PATHSEP) : it->name;
			DrawString(ir.left, ir.right, ir.top, label, it->is_dir ? TextColour::Yellow : TextColour::Black);
			ir = ir.Translate(0, this->resize.step_height);
		}
	}

	/** Validate the typed name and perform the export or import for it. */
	void TryConfirm()
	{
		auto name = SanitizeBlueprintFileName(this->name_editbox.text.GetText());
		if (!name.has_value()) {
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_INVALID_FILE_NAME), {}, WarningLevel::Info);
			return;
		}
		std::string relative_file = this->current_dir + *name + std::string{BLUEPRINT_SET_FILE_EXTENSION};

		if (this->import_mode) {
			std::vector<Blueprint> loaded;
			if (!ReadBlueprintSetFile(relative_file, loaded)) {
				ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_FILE_NOT_FOUND), {}, WarningLevel::Info);
				return;
			}
			if (AnyBlueprintSlotOccupied()) {
				this->pending_import = std::move(loaded);
				ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_ALL_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_ALL),
						this, BlueprintFileWindow::OverwriteAllCallback);
				return;
			}
			ApplyImportedBlueprintSet(std::move(loaded));
			this->Close();
		} else {
			if (FileExists(GetBlueprintBaseDirectory() + relative_file)) {
				this->pending_overwrite_file = std::move(relative_file);
				ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_FILE_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_FILE),
						this, BlueprintFileWindow::OverwriteFileCallback);
				return;
			}
			if (!WriteBlueprintSetFile(relative_file)) {
				ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_FILE_WRITE), {}, WarningLevel::Info);
				return;
			}
			this->Close();
		}
	}

	/** Callback of the export overwrite confirmation query. */
	static void OverwriteFileCallback(Window *w, bool confirmed)
	{
		BlueprintFileWindow *bfw = static_cast<BlueprintFileWindow *>(w);
		if (confirmed) {
			if (WriteBlueprintSetFile(bfw->pending_overwrite_file)) {
				bfw->pending_overwrite_file.clear();
				bfw->Close();
				return;
			}
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_FILE_WRITE), {}, WarningLevel::Info);
		}
		bfw->pending_overwrite_file.clear();
	}

	/** Callback of the import replace-all confirmation query. */
	static void OverwriteAllCallback(Window *w, bool confirmed)
	{
		BlueprintFileWindow *bfw = static_cast<BlueprintFileWindow *>(w);
		if (confirmed && bfw->pending_import.size() == NUM_BLUEPRINT_SLOTS) {
			ApplyImportedBlueprintSet(std::move(bfw->pending_import));
			bfw->pending_import.clear();
			bfw->Close();
			return;
		}
		bfw->pending_import.clear();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BTF_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->entries, pt.y, this, WID_BTF_LIST);
				if (it == this->entries.end()) return;

				if (it->is_dir) {
					this->Navigate(it->name);
					return;
				}

				if (click_count >= 2) {
					this->name_editbox.text.Assign(it->name.substr(0, it->name.size() - BLUEPRINT_SET_FILE_EXTENSION.size()));
					this->TryConfirm();
				} else {
					this->name_editbox.text.Assign(it->name.substr(0, it->name.size() - BLUEPRINT_SET_FILE_EXTENSION.size()));
					this->SetWidgetDirty(WID_BTF_NAME);
				}
				return;
			}

			case WID_BTF_CANCEL:
				this->Close();
				return;

			case WID_BTF_OK:
				this->TryConfirm();
				return;

			default: return;
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_BTF_LIST);
	}
};

/**
 * Open the blueprint file dialog, replacing any open one.
 * @param parent The blueprint toolbar.
 * @param import_mode True for the import dialog, false for the export dialog.
 */
static void ShowBlueprintFileWindow(Window *parent, bool import_mode)
{
	CloseWindowById(WindowClass::BlueprintFile, 0);
	new BlueprintFileWindow(_blueprint_file_desc, parent, import_mode);
}

/** Blueprint toolbar managing class. */
struct BlueprintToolbarWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET; ///< Last started user action.
	uint8_t renaming_slot = 0;                  ///< Slot targeted by the in-flight rename query, if any.

	BlueprintToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
	}

	/**
	 * Get the sprite visualising the current transformation.
	 * @return Sprite to show on the transformation indicator button.
	 */
	SpriteID GetTransformationSprite() const
	{
		return SPR_BLUEPRINT_TRANSFORM_FIRST + _blueprint_state.options.rotation + (_blueprint_state.options.reflected ? 4 : 0);
	}

	/**
	 * Get the sprite visualising the current terraform mode.
	 * @return Sprite to show on the terraform button.
	 */
	SpriteID GetTerraformSprite() const
	{
		/* TODO Blueprint: proper tri-state icon (red/yellow/green) instead of these placeholders (M6). */
		switch (_blueprint_state.options.terraform_mode) {
			case BlueprintTerraformMode::None: return SPR_IMG_LEVEL_LAND;
			case BlueprintTerraformMode::Minimal: return SPR_IMG_TERRAFORM_DOWN;
			case BlueprintTerraformMode::Full: return SPR_IMG_TERRAFORM_UP;
			default: NOT_REACHED();
		}
	}

	/** Push the toolbar state into the widget lowered states and dynamic sprites. */
	void UpdateWidgetStates()
	{
		for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) {
			this->SetWidgetLoweredState(WID_BT_SLOT_1 + i, _blueprint_state.slot == i);
		}
		this->SetWidgetLoweredState(WID_BT_PASTE_RAIL, _blueprint_state.options.paste_rail);
		this->SetWidgetLoweredState(WID_BT_PASTE_ROAD, _blueprint_state.options.paste_road);
		this->SetWidgetLoweredState(WID_BT_PASTE_WATER, _blueprint_state.options.paste_water);
		this->SetWidgetLoweredState(WID_BT_PASTE_AIR, _blueprint_state.options.paste_air);
		this->SetWidgetLoweredState(WID_BT_CONVERT_RAILTYPE, _blueprint_state.options.convert_railtype);
		this->SetWidgetLoweredState(WID_BT_MIRROR_SIGNALS, _blueprint_state.options.mirror_signals);
		this->SetWidgetLoweredState(WID_BT_UPGRADE_BRIDGES, _blueprint_state.options.upgrade_bridges);
		this->SetWidgetLoweredState(WID_BT_WITH_STATIONS, _blueprint_state.options.with_stations);
		this->SetWidgetLoweredState(WID_BT_TERRAFORM, _blueprint_state.options.terraform_mode != BlueprintTerraformMode::None);

		this->GetWidget<NWidgetCore>(WID_BT_TERRAFORM)->SetSprite(this->GetTerraformSprite());
		this->GetWidget<NWidgetCore>(WID_BT_TRANSFORMATION)->SetSprite(this->GetTransformationSprite());

		this->SetWidgetDisabledState(WID_BT_HEIGHT_UP, _blueprint_state.options.height_offset >= MAX_BLUEPRINT_HEIGHT_OFFSET);
		this->SetWidgetDisabledState(WID_BT_HEIGHT_DOWN, _blueprint_state.options.height_offset <= -MAX_BLUEPRINT_HEIGHT_OFFSET);

		this->SetWidgetDisabledState(WID_BT_PASTE, GetBlueprint(_blueprint_state.slot).IsEmpty());

		/* Keep the selection rectangle in sync with the pasted size while the paste tool is active. */
		if (this->IsWidgetLowered(WID_BT_PASTE)) this->UpdatePasteSelectSize();
	}

	/** Set the viewport selection rectangle to the transformed size of the active blueprint. */
	void UpdatePasteSelectSize() const
	{
		const Blueprint &blueprint = GetBlueprint(_blueprint_state.slot);
		bool swap_axes = (_blueprint_state.options.rotation % 2) != 0;
		uint16_t w = swap_axes ? blueprint.height : blueprint.width;
		uint16_t h = swap_axes ? blueprint.width : blueprint.height;
		SetTileSelectSize(std::max<uint16_t>(w, 1), std::max<uint16_t>(h, 1));
	}

	void OnPaint() override
	{
		this->UpdateWidgetStates();
		this->DrawWidgets();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_1 + NUM_BLUEPRINT_SLOTS)) {
			return GetString(STR_BLUEPRINT_SLOT, widget - WID_BT_SLOT_1 + 1);
		}

		switch (widget) {
			case WID_BT_HEIGHT_DISPLAY:
				return GetString(_blueprint_state.options.height_offset < 0 ? STR_BLUEPRINT_HEIGHT_NEGATIVE : STR_BLUEPRINT_HEIGHT_POSITIVE, abs(_blueprint_state.options.height_offset));

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	/** Draw a small "has content" marker in the top-right corner of filled slot buttons. */
	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (!IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_1 + NUM_BLUEPRINT_SLOTS)) return;
		if (GetBlueprint(static_cast<uint>(widget - WID_BT_SLOT_1)).IsEmpty()) return;

		int size = ScaleGUITrad(3);
		int inset = ScaleGUITrad(2);
		GfxFillRect(r.right - inset - size, r.top + inset, r.right - inset, r.top + inset + size, PC_GREEN);
	}

	/** Replace the tooltip of named or filled slot buttons with the name and schematic preview. */
	bool OnTooltip([[maybe_unused]] Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		if (!IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_1 + NUM_BLUEPRINT_SLOTS)) return false;

		const Blueprint &blueprint = GetBlueprint(static_cast<uint>(widget - WID_BT_SLOT_1));
		if (blueprint.IsEmpty() && blueprint.name.empty()) return false;

		ShowBlueprintPreview(this, blueprint, close_cond);
		return true;
	}

	/** Open a rename query for a blueprint slot, prefilled with its current name. */
	void ShowRenameQuery(uint8_t slot)
	{
		this->renaming_slot = slot;
		ShowQueryString(GetBlueprint(slot).name, STR_BLUEPRINT_QUERY_RENAME_CAPTION, MAX_LENGTH_BLUEPRINT_NAME_CHARS,
				this, CS_ALPHANUMERAL, {QueryStringFlag::LengthIsInChars});
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value()) return;
		GetBlueprint(this->renaming_slot).name = std::move(*str);
		SaveBlueprintAutosave();
		this->SetDirty();
	}

	/** Entries of the "Export/Import" menu. */
	enum ExportImportMenuItem : int {
		EIMI_EXPORT_SLOT, ///< Export the active slot as text.
		EIMI_EXPORT_ALL,  ///< Export all slots to a file.
		EIMI_IMPORT_SLOT, ///< Import text into the active slot.
		EIMI_IMPORT_ALL,  ///< Import all slots from a file.
	};

	/** Open the "Export/Import" menu: single slot via text, or all slots via a file. */
	void ShowExportImportMenu()
	{
		DropDownList list;
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_EXPORT_MENU_SLOT, EIMI_EXPORT_SLOT, GetBlueprint(_blueprint_state.slot).IsEmpty()));
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_EXPORT_MENU_ALL, EIMI_EXPORT_ALL, !AnyBlueprintSlotOccupied()));
		list.push_back(MakeDropDownListDividerItem());
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_IMPORT_MENU_SLOT, EIMI_IMPORT_SLOT));
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_IMPORT_MENU_ALL, EIMI_IMPORT_ALL));
		ShowDropDownList(this, std::move(list), -1, WID_BT_EXPORT_IMPORT);
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget != WID_BT_EXPORT_IMPORT) return;
		switch (index) {
			case EIMI_EXPORT_SLOT:
				ShowBlueprintTextWindow(this, EncodeBlueprintText(GetBlueprint(_blueprint_state.slot)), false);
				break;

			case EIMI_EXPORT_ALL:
				ShowBlueprintFileWindow(this, false);
				break;

			case EIMI_IMPORT_SLOT:
				ShowBlueprintTextWindow(this, std::string{}, true);
				break;

			case EIMI_IMPORT_ALL:
				ShowBlueprintFileWindow(this, true);
				break;

			default: break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_1 + NUM_BLUEPRINT_SLOTS)) {
			uint8_t slot = static_cast<uint8_t>(widget - WID_BT_SLOT_1);
			if (click_count >= 2) {
				this->ShowRenameQuery(slot);
				return;
			}
			_blueprint_state.slot = slot;
			_paste_preview_dirty = true;
			if (this->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
			this->SetDirty();
			return;
		}

		switch (widget) {
			case WID_BT_COPY:
				HandlePlacePushButton(this, WID_BT_COPY, SPR_CURSOR_CLONE_TRAIN, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_BT_PASTE:
				HandlePlacePushButton(this, WID_BT_PASTE, SPR_CURSOR_QUERY, HT_RECT);
				if (this->IsWidgetLowered(WID_BT_PASTE)) this->UpdatePasteSelectSize();
				this->last_user_action = widget;
				break;

			case WID_BT_EXPORT_IMPORT:
				this->ShowExportImportMenu();
				return;

			case WID_BT_INFO:
				ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_INFO_VERSION, std::string{BLUEPRINT_PATCH_VERSION}), {}, WarningLevel::Info);
				return;

			case WID_BT_PASTE_RAIL: _blueprint_state.options.paste_rail = !_blueprint_state.options.paste_rail; break;
			case WID_BT_PASTE_ROAD: _blueprint_state.options.paste_road = !_blueprint_state.options.paste_road; break;
			case WID_BT_PASTE_WATER: _blueprint_state.options.paste_water = !_blueprint_state.options.paste_water; break;
			case WID_BT_PASTE_AIR: _blueprint_state.options.paste_air = !_blueprint_state.options.paste_air; break;
			case WID_BT_CONVERT_RAILTYPE: _blueprint_state.options.convert_railtype = !_blueprint_state.options.convert_railtype; break;
			case WID_BT_MIRROR_SIGNALS: _blueprint_state.options.mirror_signals = !_blueprint_state.options.mirror_signals; break;
			case WID_BT_UPGRADE_BRIDGES: _blueprint_state.options.upgrade_bridges = !_blueprint_state.options.upgrade_bridges; break;
			case WID_BT_WITH_STATIONS: _blueprint_state.options.with_stations = !_blueprint_state.options.with_stations; break;

			case WID_BT_TERRAFORM:
				_blueprint_state.options.terraform_mode = static_cast<BlueprintTerraformMode>((static_cast<uint8_t>(_blueprint_state.options.terraform_mode) + 1) % 3);
				break;

			case WID_BT_TRANSFORMATION:
				_blueprint_state.options.ResetTransformation();
				break;

			case WID_BT_ROTATE_CCW:
				_blueprint_state.options.AddRotation(1);
				break;

			case WID_BT_ROTATE_CW:
				_blueprint_state.options.AddRotation(-1);
				break;

			case WID_BT_REFLECT_NW_SE:
				_blueprint_state.options.AddReflection(0);
				break;

			case WID_BT_REFLECT_NE_SW:
				_blueprint_state.options.AddReflection(2);
				break;

			case WID_BT_HEIGHT_UP:
				_blueprint_state.options.height_offset = std::min<int>(_blueprint_state.options.height_offset + 1, MAX_BLUEPRINT_HEIGHT_OFFSET);
				break;

			case WID_BT_HEIGHT_DOWN:
				_blueprint_state.options.height_offset = std::max<int>(_blueprint_state.options.height_offset - 1, -MAX_BLUEPRINT_HEIGHT_OFFSET);
				break;

			default: return;
		}
		SaveBlueprintStateToSettings();
		_paste_preview_dirty = true;
		if (this->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
		this->SetDirty();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_BT_COPY:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BLUEPRINT_COPY);
				break;

			case WID_BT_PASTE: {
				BlueprintPasteData data = PrepareBlueprintPaste(GetBlueprint(_blueprint_state.slot), _blueprint_state.options, GetCurrentRailType());
				if (data.blueprint.IsEmpty()) break;
				if (_networking && EndianBufferWriter<>::FromValue(data).size() > MAX_BLUEPRINT_COMMAND_SIZE) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_TOO_LARGE), {}, WarningLevel::Info);
					break;
				}
				Command<Commands::PasteBlueprint>::Post(STR_ERROR_CAN_T_PASTE_BLUEPRINT, CcPlaySound_CONSTRUCTION_OTHER, tile, data);
				break;
			}

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x == -1) return;

		switch (select_proc) {
			case DDSP_BLUEPRINT_COPY: {
				TileArea area(start_tile, end_tile);
				if (area.w > MAX_BLUEPRINT_DIMENSION || area.h > MAX_BLUEPRINT_DIMENSION) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_AREA_TOO_LARGE), {}, WarningLevel::Info);
					break;
				}
				uint count = CopyAreaToBlueprint(area, GetBlueprint(_blueprint_state.slot));
				SaveBlueprintAutosave();
				_paste_preview_dirty = true;
				if (count == 0) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_NOTHING_TO_COPY), {}, WarningLevel::Info);
				} else if (_settings_client.sound.confirm) {
					SndPlayTileFx(SND_1F_CONSTRUCTION_OTHER, end_tile);
				}
				this->SetDirty();
				break;
			}

			default: NOT_REACHED();
		}
	}

	TileIndex last_cost_origin = INVALID_TILE; ///< Origin the paste cost tooltip was last shown for.

	void OnPlaceObjectAbort() override
	{
		this->RaiseWidget(WID_BT_COPY);
		this->RaiseWidget(WID_BT_PASTE);
		this->last_cost_origin = INVALID_TILE;
		this->SetDirty();
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		/* Show the estimated paste cost (computed with the blocked mask during
		 * viewport drawing) as a hover tooltip, like the drag measurement tooltips. */
		if (!_settings_client.gui.measure_tooltip || !this->IsWidgetLowered(WID_BT_PASTE)) return;
		if (_paste_preview_origin == INVALID_TILE || _paste_preview_origin == this->last_cost_origin) return;
		this->last_cost_origin = _paste_preview_origin;
		if (_paste_preview_cost <= 0) return;
		GuiShowTooltips(this, GetEncodedString(STR_MESSAGE_ESTIMATED_COST, _paste_preview_cost), TooltipCloseCondition::ExitViewport);
	}

	static EventState BlueprintToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GameMode::Normal) return EventState::NotHandled;
		Window *w = FindWindowById(WindowClass::BlueprintToolbar, 0);
		if (w == nullptr) w = ShowBlueprintToolbar();
		if (w == nullptr) return EventState::NotHandled;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"blueprint_toolbar", {
		Hotkey('1', "slot_1", WID_BT_SLOT_1),
		Hotkey('2', "slot_2", WID_BT_SLOT_2),
		Hotkey('3', "slot_3", WID_BT_SLOT_3),
		Hotkey('4', "slot_4", WID_BT_SLOT_4),
		Hotkey('5', "slot_5", WID_BT_SLOT_5),
		Hotkey('6', "slot_6", WID_BT_SLOT_6),
		Hotkey('7', "slot_7", WID_BT_SLOT_7),
		Hotkey('8', "slot_8", WID_BT_SLOT_8),
		Hotkey('C', "copy", WID_BT_COPY),
		Hotkey('V', "paste", WID_BT_PASTE),
		Hotkey('Q', "rotate_ccw", WID_BT_ROTATE_CCW),
		Hotkey('E', "rotate_cw", WID_BT_ROTATE_CW),
		Hotkey('F', "reflect_nw_se", WID_BT_REFLECT_NW_SE),
		Hotkey('G', "reflect_ne_sw", WID_BT_REFLECT_NE_SW),
		Hotkey('M', "mirror_signals", WID_BT_MIRROR_SIGNALS),
		Hotkey('X', "reset_transform", WID_BT_TRANSFORMATION),
	}, BlueprintToolbarGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_BLUEPRINT_TOOLBAR, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_1), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_2), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_3), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_4), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_5), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_6), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_7), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_8), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_COPY), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_COPY, STR_BLUEPRINT_TOOLTIP_COPY),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_PASTE), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_PASTE, STR_BLUEPRINT_TOOLTIP_PASTE),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BT_EXPORT_IMPORT),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_EXPORT_IMPORT, STR_BLUEPRINT_TOOLTIP_EXPORT_IMPORT),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_PASTE_RAIL), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_BUILDRAIL, STR_BLUEPRINT_TOOLTIP_PASTE_RAIL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_PASTE_ROAD), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_BUILDROAD, STR_BLUEPRINT_TOOLTIP_PASTE_ROAD),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_PASTE_WATER), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_BUILDWATER, STR_BLUEPRINT_TOOLTIP_PASTE_WATER),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_PASTE_AIR), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_BUILDAIR, STR_BLUEPRINT_TOOLTIP_PASTE_AIR),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_CONVERT_RAILTYPE), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_CONVERT_RAIL, STR_BLUEPRINT_TOOLTIP_CONVERT_RAILTYPE),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_MIRROR_SIGNALS), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_MIRROR_SIGNALS, STR_BLUEPRINT_TOOLTIP_MIRROR_SIGNALS),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_UPGRADE_BRIDGES), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_BRIDGE, STR_BLUEPRINT_TOOLTIP_UPGRADE_BRIDGES),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_WITH_STATIONS), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_RAIL_STATION, STR_BLUEPRINT_TOOLTIP_WITH_STATIONS),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_TERRAFORM), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_TERRAFORM_UP, STR_BLUEPRINT_TOOLTIP_TERRAFORM),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_TRANSFORMATION), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_TRANSFORM_FIRST, STR_BLUEPRINT_TOOLTIP_TRANSFORMATION),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_ROTATE_CCW), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_ROTATE_CCW, STR_BLUEPRINT_TOOLTIP_ROTATE_CCW),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_ROTATE_CW), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_ROTATE_CW, STR_BLUEPRINT_TOOLTIP_ROTATE_CW),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_REFLECT_NW_SE), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_REFLECT_NW_SE, STR_BLUEPRINT_TOOLTIP_REFLECT_NW_SE),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_REFLECT_NE_SW), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_REFLECT_NE_SW, STR_BLUEPRINT_TOOLTIP_REFLECT_NE_SW),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PANEL, Colours::DarkGreen),
			NWidget(WWT_LABEL, Colours::Invalid, WID_BT_HEIGHT_DISPLAY), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_HEIGHT_POSITIVE, STR_BLUEPRINT_TOOLTIP_HEIGHT_DISPLAY),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_HEIGHT_UP), SetMinimalSize(12, 11),
								SetFill(0, 1), SetSpriteTip(SPR_ARROW_UP, STR_BLUEPRINT_TOOLTIP_HEIGHT_UP),
			NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_HEIGHT_DOWN), SetMinimalSize(12, 11),
								SetFill(0, 1), SetSpriteTip(SPR_ARROW_DOWN, STR_BLUEPRINT_TOOLTIP_HEIGHT_DOWN),
		EndContainer(),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_INFO), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_QUERY, STR_BLUEPRINT_TOOLTIP_INFO),
	EndContainer(),
};

static WindowDesc _blueprint_toolbar_desc(
	WindowPosition::Automatic, "toolbar_blueprint", 0, 0,
	WindowClass::BlueprintToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_blueprint_toolbar_widgets,
	&BlueprintToolbarWindow::hotkeys
);

/**
 * Show the blueprint toolbar.
 * @return The toolbar window if it was (already) open, else \c nullptr.
 */
Window *ShowBlueprintToolbar()
{
	if (!Company::IsValidID(_local_company)) return nullptr;
	LoadBlueprintAutosave();
	LoadBlueprintStateFromSettings();
	return AllocateWindowDescFront<BlueprintToolbarWindow>(_blueprint_toolbar_desc, 0);
}

/**
 * Get the paste preview overlays for the viewport.
 * @param origin Northern tile of the hovered target area.
 * @return The preview, or \c nullptr when the paste tool is not active.
 */
const BlueprintPastePreview *GetBlueprintPastePreview(TileIndex origin)
{
	Window *w = FindWindowById(WindowClass::BlueprintToolbar, 0);
	if (w == nullptr || !w->IsWidgetLowered(WID_BT_PASTE)) return nullptr;
	if (GetBlueprint(_blueprint_state.slot).IsEmpty()) return nullptr;
	if (_paste_preview_dirty) RebuildPastePreview();
	if (_paste_preview_origin != origin && origin < Map::Size()) RecomputePasteBlocked(origin);
	return &_paste_preview;
}
