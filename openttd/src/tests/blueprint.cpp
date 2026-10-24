/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.cpp Tests for copying infrastructure into a blueprint. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../blueprint.h"
#include "../blueprint_cmd.h"
#include "../company_func.h"
#include "../map_func.h"
#include "../rail_map.h"

#include "../safeguards.h"

TEST_CASE("Blueprint - copy respects area and ownership")
{
	Map::Allocate(64, 64);
	_local_company = OWNER_BEGIN;

	/* An own track, a foreign track, and an own track outside the copied area. */
	MakeRailNormal(TileXY(10, 10), OWNER_BEGIN, TrackBits{Track::X}, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(11, 10), OWNER_NONE, TrackBits{Track::Y}, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(20, 20), OWNER_BEGIN, TrackBits{Track::X}, RAILTYPE_RAIL);

	Blueprint blueprint;
	uint count = CopyAreaToBlueprint(TileArea(TileXY(9, 9), TileXY(12, 12)), blueprint);

	CHECK(count == 1);
	CHECK(!blueprint.IsEmpty());

	/* The blueprint is shrunk to the single copied track tile. */
	CHECK(blueprint.width == 1);
	CHECK(blueprint.height == 1);

	REQUIRE(blueprint.rail_tracks.size() == 1);
	CHECK(blueprint.rail_tracks[0].offset.x == 0);
	CHECK(blueprint.rail_tracks[0].offset.y == 0);
	CHECK(blueprint.rail_tracks[0].track == Track::X);
	CHECK(blueprint.rail_tracks[0].railtype == RAILTYPE_RAIL);

	CHECK(blueprint.corner_heights.size() == 2 * 2);
}

TEST_CASE("Blueprint - name persists across Clear() and re-copy")
{
	Map::Allocate(64, 64);
	_local_company = OWNER_BEGIN;

	MakeRailNormal(TileXY(5, 5), OWNER_BEGIN, TrackBits{Track::X}, RAILTYPE_RAIL);

	Blueprint blueprint;
	blueprint.name = "My Layout";

	/* Re-copying (which clears the blueprint first) must not lose the name. */
	CopyAreaToBlueprint(TileArea(TileXY(5, 5), TileXY(5, 5)), blueprint);
	CHECK(blueprint.name == "My Layout");
	REQUIRE(blueprint.rail_tracks.size() == 1);

	blueprint.Clear();
	CHECK(blueprint.name == "My Layout");
	CHECK(blueprint.IsEmpty());
}

TEST_CASE("Blueprint - copy shrinks to the smallest area containing all elements")
{
	Map::Allocate(64, 64);
	_local_company = OWNER_BEGIN;

	/* Two tracks spanning a 2x3 area inside a much larger selection. */
	MakeRailNormal(TileXY(40, 40), OWNER_BEGIN, TrackBits{Track::X}, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(41, 42), OWNER_BEGIN, TrackBits{Track::Y}, RAILTYPE_RAIL);

	Blueprint blueprint;
	uint count = CopyAreaToBlueprint(TileArea(TileXY(35, 36), TileXY(46, 47)), blueprint);

	CHECK(count == 2);
	CHECK(blueprint.width == 2);
	CHECK(blueprint.height == 3);

	REQUIRE(blueprint.rail_tracks.size() == 2);
	CHECK(blueprint.rail_tracks[0].offset.x == 0);
	CHECK(blueprint.rail_tracks[0].offset.y == 0);
	CHECK(blueprint.rail_tracks[1].offset.x == 1);
	CHECK(blueprint.rail_tracks[1].offset.y == 2);

	CHECK(blueprint.corner_heights.size() == 3 * 4);
}

TEST_CASE("Blueprint - copy of empty area yields empty blueprint")
{
	Map::Allocate(64, 64);
	_local_company = OWNER_BEGIN;

	Blueprint blueprint;
	blueprint.rail_tracks.push_back({{0, 0}, Track::X, RAILTYPE_RAIL}); // Must be cleared by the copy.

	uint count = CopyAreaToBlueprint(TileArea(TileXY(30, 30), TileXY(33, 33)), blueprint);

	CHECK(count == 0);
	CHECK(blueprint.IsEmpty());
	CHECK(blueprint.rail_tracks.empty());
	CHECK(blueprint.width == 4);
}

TEST_CASE("Blueprint - track transformation")
{
	/* Four rotations or two reflections are the identity. */
	for (Track track : TRACK_BIT_ALL) {
		CHECK(TransformBlueprintTrack(track, 4, false) == track);
		CHECK(TransformBlueprintTrack(TransformBlueprintTrack(track, 0, true), 0, true) == track);
	}

	/* Rotating 90 degrees anticlockwise moves the north corner to the west. */
	CHECK(TransformBlueprintTrack(Track::X, 1, false) == Track::Y);
	CHECK(TransformBlueprintTrack(Track::Y, 1, false) == Track::X);
	CHECK(TransformBlueprintTrack(Track::Upper, 1, false) == Track::Left);
	CHECK(TransformBlueprintTrack(Track::Left, 1, false) == Track::Lower);

	/* Reflection against the NW-SE axis keeps X/Y and swaps upper/left, lower/right. */
	CHECK(TransformBlueprintTrack(Track::X, 0, true) == Track::X);
	CHECK(TransformBlueprintTrack(Track::Upper, 0, true) == Track::Left);

	/* Reflect + rotate twice equals a reflection against the NE-SW axis. */
	CHECK(TransformBlueprintTrack(Track::Upper, 2, true) == Track::Right);
	CHECK(TransformBlueprintTrack(Track::X, 2, true) == Track::X);
}

TEST_CASE("Blueprint - point transformation")
{
	/* Identity. */
	CHECK(TransformBlueprintPoint({2, 1}, 4, 3, 0, false).x == 2);
	CHECK(TransformBlueprintPoint({2, 1}, 4, 3, 0, false).y == 1);

	/* One anticlockwise quarter turn in a 4x3 box: (x, y) -> (3 - 1 - y, x). */
	TileIndexDiffC p = TransformBlueprintPoint({2, 1}, 4, 3, 1, false);
	CHECK(p.x == 1);
	CHECK(p.y == 2);

	/* The north corner (0, 0) moves to the west corner. */
	p = TransformBlueprintPoint({0, 0}, 4, 3, 1, false);
	CHECK(p.x == 2);
	CHECK(p.y == 0);

	/* Four quarter turns are the identity. */
	p = TransformBlueprintPoint({3, 2}, 4, 3, 4, false);
	CHECK(p.x == 3);
	CHECK(p.y == 2);

	/* Reflection against the NW-SE axis mirrors the X coordinate. */
	p = TransformBlueprintPoint({0, 2}, 4, 3, 0, true);
	CHECK(p.x == 3);
	CHECK(p.y == 2);
}

TEST_CASE("Blueprint - direction, axis and road bit transformation")
{
	/* Rotation by 90 degrees anticlockwise: NE -> NW -> SW -> SE -> NE. */
	CHECK(TransformBlueprintDiagDir(DiagDirection::NE, 1, false) == DiagDirection::NW);
	CHECK(TransformBlueprintDiagDir(DiagDirection::NW, 1, false) == DiagDirection::SW);
	CHECK(TransformBlueprintDiagDir(DiagDirection::NE, 4, false) == DiagDirection::NE);

	/* Reflection against the NW-SE axis: NE and SW swap, NW and SE stay. */
	CHECK(TransformBlueprintDiagDir(DiagDirection::NE, 0, true) == DiagDirection::SW);
	CHECK(TransformBlueprintDiagDir(DiagDirection::SE, 0, true) == DiagDirection::SE);

	CHECK(TransformBlueprintAxis(Axis::X, 1) == Axis::Y);
	CHECK(TransformBlueprintAxis(Axis::X, 2) == Axis::X);

	CHECK(TransformBlueprintRoadBits(RoadBits{RoadBit::NE}, 1, false) == RoadBits{RoadBit::NW});
	CHECK(TransformBlueprintRoadBits(ROAD_X, 1, false) == ROAD_Y);
	CHECK(TransformBlueprintRoadBits(ROAD_ALL, 1, true) == ROAD_ALL);
	CHECK(TransformBlueprintRoadBits(RoadBits{RoadBit::NE} | RoadBits{RoadBit::SE}, 0, true) == (RoadBits{RoadBit::SW} | RoadBits{RoadBit::SE}));
}

TEST_CASE("Blueprint - transforming a blueprint moves elements consistently")
{
	/* A 3x2 blueprint with a single X track in its north corner. */
	Blueprint blueprint;
	blueprint.width = 3;
	blueprint.height = 2;
	blueprint.rail_tracks.push_back({{0, 0}, Track::X, RAILTYPE_RAIL});
	blueprint.corner_heights.assign(4 * 3, 0);
	REQUIRE(blueprint.rail_tracks.size() == 1);

	/* Rotate 90 degrees anticlockwise: the 3x2 area becomes 2x3, the track at
	 * (0, 0) moves to the west corner (1, 0) and becomes a Y track. */
	Blueprint rotated = TransformBlueprint(blueprint, 1, false);
	CHECK(rotated.width == 2);
	CHECK(rotated.height == 3);
	REQUIRE(rotated.rail_tracks.size() == 1);
	CHECK(rotated.rail_tracks[0].offset.x == 1);
	CHECK(rotated.rail_tracks[0].offset.y == 0);
	CHECK(rotated.rail_tracks[0].track == Track::Y);
	CHECK(rotated.corner_heights.size() == 3 * 4);

	/* Rotating four times gives back the original. */
	Blueprint full_circle = TransformBlueprint(TransformBlueprint(rotated, 3, false), 0, false);
	CHECK(full_circle.width == 3);
	REQUIRE(full_circle.rail_tracks.size() == 1);
	CHECK(full_circle.rail_tracks[0].offset.x == 0);
	CHECK(full_circle.rail_tracks[0].offset.y == 0);
	CHECK(full_circle.rail_tracks[0].track == Track::X);
}

TEST_CASE("Blueprint - two tracks with signals on one tile")
{
	Map::Allocate(64, 64);
	_local_company = OWNER_BEGIN;

	MakeRailNormal(TileXY(5, 5), OWNER_BEGIN, TrackBits{Track::X} | TrackBits{Track::Y}, RAILTYPE_RAIL);

	Blueprint blueprint;
	uint count = CopyAreaToBlueprint(TileArea(TileXY(5, 5), TileXY(5, 5)), blueprint);

	CHECK(count == 2);
	CHECK(blueprint.width == 1);
	CHECK(blueprint.height == 1);
	REQUIRE(blueprint.rail_tracks.size() == 2);
	CHECK(blueprint.rail_tracks[0].offset.x == 0);
	CHECK(blueprint.rail_tracks[0].offset.y == 0);
	CHECK(blueprint.signals.empty());
}

TEST_CASE("Blueprint - paste data serialization round-trip")
{
	BlueprintPasteData data;
	data.terraform_mode = BlueprintTerraformMode::Minimal;
	data.height_offset = -3;

	Blueprint &bp = data.blueprint;
	bp.width = 3;
	bp.height = 2;
	bp.name = "Test Junction";
	bp.rail_tracks.push_back({{1, 0}, Track::Lower, RAILTYPE_MONO});
	bp.signals.push_back({{2, 1}, Track::X, SignalType::Path, SignalVariant::Semaphore, true, false});
	bp.tunnel_bridges.push_back({{0, 0}, {2, 0}, TransportType::Road, true, 5, INVALID_RAILTYPE, ROADTYPE_ROAD, INVALID_ROADTYPE});
	bp.station_tiles.push_back({{0, 1}, false, Axis::Y, RAILTYPE_ELECTRIC, StationID{42}});
	bp.corner_heights = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

	std::vector<uint8_t> buffer = EndianBufferWriter<>::FromValue(data);
	BlueprintPasteData result = EndianBufferReader::ToValue<BlueprintPasteData>(buffer);

	CHECK(result.terraform_mode == BlueprintTerraformMode::Minimal);
	CHECK(result.height_offset == -3);
	CHECK(result.blueprint.width == 3);
	CHECK(result.blueprint.height == 2);
	CHECK(result.blueprint.name == "Test Junction");

	REQUIRE(result.blueprint.rail_tracks.size() == 1);
	CHECK(result.blueprint.rail_tracks[0].offset.x == 1);
	CHECK(result.blueprint.rail_tracks[0].offset.y == 0);
	CHECK(result.blueprint.rail_tracks[0].track == Track::Lower);
	CHECK(result.blueprint.rail_tracks[0].railtype == RAILTYPE_MONO);

	REQUIRE(result.blueprint.signals.size() == 1);
	CHECK(result.blueprint.signals[0].type == SignalType::Path);
	CHECK(result.blueprint.signals[0].variant == SignalVariant::Semaphore);
	CHECK(result.blueprint.signals[0].along);
	CHECK(!result.blueprint.signals[0].against);

	REQUIRE(result.blueprint.tunnel_bridges.size() == 1);
	CHECK(result.blueprint.tunnel_bridges[0].other_end.x == 2);
	CHECK(result.blueprint.tunnel_bridges[0].is_bridge);
	CHECK(result.blueprint.tunnel_bridges[0].transport == TransportType::Road);
	CHECK(result.blueprint.tunnel_bridges[0].roadtype == ROADTYPE_ROAD);
	CHECK(result.blueprint.tunnel_bridges[0].tramtype == INVALID_ROADTYPE);

	REQUIRE(result.blueprint.station_tiles.size() == 1);
	CHECK(result.blueprint.station_tiles[0].axis == Axis::Y);
	CHECK(result.blueprint.station_tiles[0].railtype == RAILTYPE_ELECTRIC);
	CHECK(result.blueprint.station_tiles[0].station == StationID{42});

	REQUIRE(result.blueprint.corner_heights.size() == 12);
	CHECK(result.blueprint.corner_heights[11] == 12);
	CHECK(result.blueprint.roads.empty());
	CHECK(result.blueprint.airports.empty());
}

TEST_CASE("Blueprint - full slot set serialization round-trip")
{
	/* The blueprint set file format wraps NUM_BLUEPRINT_SLOTS blueprints in a single
	 * length-prefixed vector, exactly like ReadBlueprintVector/WriteBlueprintVector
	 * already do for e.g. rail_tracks; this exercises that same generic mechanism one
	 * level deeper (a vector of Blueprint, each already containing several vectors). */
	std::vector<Blueprint> slots;
	slots.resize(NUM_BLUEPRINT_SLOTS);
	slots[0].name = "Empty but named";
	slots[2].width = 2;
	slots[2].height = 1;
	slots[2].name = "Junction";
	slots[2].rail_tracks.push_back({{0, 0}, Track::X, RAILTYPE_RAIL});
	slots[2].rail_tracks.push_back({{1, 0}, Track::X, RAILTYPE_RAIL});
	slots[2].corner_heights = {3, 3, 3, 3, 3, 3};
	slots[7].width = 1;
	slots[7].height = 1;
	slots[7].road_depots.push_back({{0, 0}, DiagDirection::NE, ROADTYPE_ROAD});

	std::vector<uint8_t> buffer;
	EndianBufferWriter<> writer{buffer};
	WriteBlueprintVector(writer, slots);

	std::vector<Blueprint> result;
	EndianBufferReader reader{buffer};
	ReadBlueprintVector(reader, result);

	REQUIRE(result.size() == NUM_BLUEPRINT_SLOTS);
	CHECK(result[0].name == "Empty but named");
	CHECK(result[0].IsEmpty());
	CHECK(result[1].name.empty());
	CHECK(result[1].IsEmpty());

	CHECK(result[2].name == "Junction");
	REQUIRE(result[2].rail_tracks.size() == 2);
	CHECK(result[2].rail_tracks[1].offset.x == 1);
	CHECK(result[2].corner_heights.size() == 6);

	REQUIRE(result[7].road_depots.size() == 1);
	CHECK(result[7].road_depots[0].dir == DiagDirection::NE);
	CHECK(result[7].road_depots[0].roadtype == ROADTYPE_ROAD);
}
