/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * Changed for ottd Decouple by Karel Mácha: the changes Copyright (C) 2026 Karel Mácha.
 */

/** @file rail_cmd.cpp Handling of rail tiles. */

#include "stdafx.h"
#include "landscape.h"
#include "viewport_func.h"
#include "command_func.h"
#include "depot_base.h"
#include "pathfinder/yapf/yapf_cache.h"
#include "newgrf_debug.h"
#include "newgrf_railtype.h"
#include "newgrf_signals.h"
#include "train.h"
#include "autoslope.h"
#include "water.h"
#include "tunnelbridge_map.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "tunnelbridge.h"
#include "elrail_func.h"
#include "town.h"
#include "pbs.h"
#include "pathfinder/follow_track.hpp"
#include "company_base.h"
#include "core/backup_type.hpp"
#include "core/container_func.hpp"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "strings_func.h"
#include "company_gui.h"
#include "object_map.h"
#include "rail_cmd.h"
#include "landscape_cmd.h"

#include "table/strings.h"
#include "table/railtypes.h"
#include "table/track_land.h"

#include "round_track.h"
#include "three_d.h"
#include "three_d_objects.h"
#include "three_d_vehicle_object.h"

#include "safeguards.h"

/** Helper type for lists/vectors of trains */
typedef std::vector<Train *> TrainList;

RailTypeInfo _railtypes[RAILTYPE_END];
std::vector<RailType> _sorted_railtypes; ///< Sorted list of rail types.
RailTypes _railtypes_hidden_mask;

/** Enum holding the signal offset in the sprite sheet according to the side it is representing. */
enum SignalOffsets {
	SIGNAL_TO_SOUTHWEST,
	SIGNAL_TO_NORTHEAST,
	SIGNAL_TO_SOUTHEAST,
	SIGNAL_TO_NORTHWEST,
	SIGNAL_TO_EAST,
	SIGNAL_TO_WEST,
	SIGNAL_TO_SOUTH,
	SIGNAL_TO_NORTH,
};

/**
 * Reset all rail type information to its default values.
 */
void ResetRailTypes()
{
	static_assert(std::size(_original_railtypes) <= std::size(_railtypes));

	auto insert = std::copy(std::begin(_original_railtypes), std::end(_original_railtypes), std::begin(_railtypes));
	std::fill(insert, std::end(_railtypes), RailTypeInfo{});

	_railtypes_hidden_mask = {};
}

void ResolveRailTypeGUISprites(RailTypeInfo *rti)
{
	SpriteID cursors_base = GetCustomRailSprite(rti, INVALID_TILE, RailSpriteType::UI);
	if (cursors_base != 0) {
		rti->gui_sprites.build_ns_rail = cursors_base +  0;
		rti->gui_sprites.build_x_rail  = cursors_base +  1;
		rti->gui_sprites.build_ew_rail = cursors_base +  2;
		rti->gui_sprites.build_y_rail  = cursors_base +  3;
		rti->gui_sprites.auto_rail     = cursors_base +  4;
		rti->gui_sprites.build_depot   = cursors_base +  5;
		rti->gui_sprites.build_tunnel  = cursors_base +  6;
		rti->gui_sprites.convert_rail  = cursors_base +  7;
		rti->cursor.rail_ns   = cursors_base +  8;
		rti->cursor.rail_swne = cursors_base +  9;
		rti->cursor.rail_ew   = cursors_base + 10;
		rti->cursor.rail_nwse = cursors_base + 11;
		rti->cursor.autorail  = cursors_base + 12;
		rti->cursor.depot     = cursors_base + 13;
		rti->cursor.tunnel    = cursors_base + 14;
		rti->cursor.convert   = cursors_base + 15;
	}

	/* Array of default GUI signal sprite numbers. */
	const EnumIndexArray<EnumIndexArray<SpriteID, SignalType, SignalType::End>, SignalVariant, SignalVariant::End> _signal_lookup{{{
		{SPR_IMG_SIGNAL_ELECTRIC_NORM,  SPR_IMG_SIGNAL_ELECTRIC_ENTRY, SPR_IMG_SIGNAL_ELECTRIC_EXIT,
		 SPR_IMG_SIGNAL_ELECTRIC_COMBO, SPR_IMG_SIGNAL_ELECTRIC_PBS,   SPR_IMG_SIGNAL_ELECTRIC_PBS_OWAY},

		{SPR_IMG_SIGNAL_SEMAPHORE_NORM,  SPR_IMG_SIGNAL_SEMAPHORE_ENTRY, SPR_IMG_SIGNAL_SEMAPHORE_EXIT,
		 SPR_IMG_SIGNAL_SEMAPHORE_COMBO, SPR_IMG_SIGNAL_SEMAPHORE_PBS,   SPR_IMG_SIGNAL_SEMAPHORE_PBS_OWAY},
	}}};

	for (SignalType type : EnumRange(SignalType::End)) {
		for (SignalVariant var : {SignalVariant::Electric, SignalVariant::Semaphore}) {
			SpriteID red = GetCustomSignalSprite(rti, INVALID_TILE, type, var, SignalAspect::Red, true);
			SpriteID green = GetCustomSignalSprite(rti, INVALID_TILE, type, var, SignalAspect::Green, true);
			rti->gui_sprites.signals[type][var][SignalState::Red] = (red != 0) ? red + SIGNAL_TO_SOUTH : _signal_lookup[var][type];
			rti->gui_sprites.signals[type][var][SignalState::Green] = (green != 0) ? green + SIGNAL_TO_SOUTH : _signal_lookup[var][type] + 1;
		}
	}
}

/**
 * Compare railtypes based on their sorting order.
 * @param first  The railtype to compare to.
 * @param second The railtype to compare.
 * @return True iff the first should be sorted before the second.
 */
static bool CompareRailTypes(const RailType &first, const RailType &second)
{
	return GetRailTypeInfo(first)->sorting_order < GetRailTypeInfo(second)->sorting_order;
}

/**
 * Resolve sprites of custom rail types
 */
void InitRailTypes()
{
	_sorted_railtypes.clear();
	for (RailTypeInfo &rti : _railtypes) {
		RailType rt = rti.Index();

		ResolveRailTypeGUISprites(&rti);
		_railtypes_hidden_mask.Set(rt, rti.flags.Test(RailTypeFlag::Hidden));

		if (rti.label == 0) continue;
		_sorted_railtypes.push_back(rt);
	}
	std::sort(_sorted_railtypes.begin(), _sorted_railtypes.end(), CompareRailTypes);
}

/**
 * Allocate a new rail type label
 * @param label The label of the rail type.
 * @return The allocated type, or \c INVALID_RAILTYPE upon failures.
 */
RailType AllocateRailType(RailTypeLabel label)
{
	auto it = std::ranges::find(_railtypes, 0, &RailTypeInfo::label);
	if (it == std::end(_railtypes)) return INVALID_RAILTYPE;

	RailTypeInfo &rti = *it;
	RailType rt = rti.Index();

	/* Set up new rail type based on default rail. */
	rti = _original_railtypes[RAILTYPE_RAIL];
	rti.label = label;
	rti.alternate_labels.clear();

	/* Make us compatible with ourself. */
	rti.powered_railtypes = rt;
	rti.compatible_railtypes = rt;

	/* We also introduce ourself. */
	rti.introduces_railtypes = rt;

	/* Default sort order; order of allocation, but with some
	 * offsets so it's easier for NewGRF to pick a spot without
	 * changing the order of other (original) rail types.
	 * The << is so you can place other railtypes in between the
	 * other railtypes, the 7 is to be able to place something
	 * before the first (default) rail type. */
	rti.sorting_order = rt << 4 | 7;

	return rt;
}

static const uint8_t _track_sloped_sprites[14] = {
	14, 15, 22, 13,
	 0, 21, 17, 12,
	23,  0, 18, 20,
	19, 16
};


/*         4
 *     ---------
 *    |\       /|
 *    | \    1/ |
 *    |  \   /  |
 *    |   \ /   |
 *  16|    \    |32
 *    |   / \2  |
 *    |  /   \  |
 *    | /     \ |
 *    |/       \|
 *     ---------
 *         8
 */



/* MAP2 byte:    abcd???? => Signal On? Same coding as map3lo
 * MAP3LO byte:  abcd???? => Signal Exists?
 *               a and b are for diagonals, upper and left,
 *               one for each direction. (ie a == NE->SW, b ==
 *               SW->NE, or v.v., I don't know. b and c are
 *               similar for lower and right.
 * MAP2 byte:    ????abcd => Type of ground.
 * MAP3LO byte:  ????abcd => Type of rail.
 * MAP5:         00abcdef => rail
 *               01abcdef => rail w/ signals
 *               10uuuuuu => unused
 *               11uuuudd => rail depot
 */

/**
 * Tests if a vehicle interacts with the specified track.
 * All track bits interact except parallel #TRACK_BIT_HORZ or #TRACK_BIT_VERT.
 *
 * @param tile The tile.
 * @param track The track.
 * @return Succeeded command (no train found), or a failed command (a train was found).
 */
static CommandCost EnsureNoTrainOnTrack(TileIndex tile, Track track)
{
	return EnsureNoTrainOnTrackBits(tile, track);
}

/**
 * Check that the new track bits may be built.
 * @param tile %Tile to build on.
 * @param to_build New track bits.
 * @return Succeeded or failed command.
 */
static CommandCost CheckTrackCombination(TileIndex tile, TrackBits to_build)
{
	if (!IsPlainRail(tile)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);

	/* So, we have a tile with tracks on it (and possibly signals). Let's see
	 * what tracks first */
	TrackBits current = GetTrackBits(tile); // The current track layout.
	TrackBits future = current | to_build;  // The track layout we want to build.

	/* Are we really building something new? */
	if (current == future) {
		/* Nothing new is being built */
		return CommandCost(STR_ERROR_ALREADY_BUILT);
	}

	/* Normally, we may overlap and any combination is valid */
	return CommandCost();
}


/** Valid TrackBits on a specific (non-steep)-slope without foundation */
static const TrackBits _valid_tracks_without_foundation[15] = {
	TRACK_BIT_ALL,
	Track::Right,
	Track::Upper,
	Track::X,

	Track::Left,
	{},
	Track::Y,
	Track::Lower,

	Track::Lower,
	Track::Y,
	{},
	Track::Left,

	Track::X,
	Track::Upper,
	Track::Right,
};

/** Valid TrackBits on a specific (non-steep)-slope with leveled foundation */
static const TrackBits _valid_tracks_on_leveled_foundation[15] = {
	{},
	Track::Left,
	Track::Lower,
	{Track::Y, Track::Lower, Track::Left},

	Track::Right,
	TRACK_BIT_ALL,
	{Track::X, Track::Lower, Track::Right},
	TRACK_BIT_ALL,

	Track::Upper,
	{Track::X, Track::Upper, Track::Left},
	TRACK_BIT_ALL,
	TRACK_BIT_ALL,

	{Track::Y, Track::Upper, Track::Right},
	TRACK_BIT_ALL,
	TRACK_BIT_ALL,
};

/**
 * Checks if a track combination is valid on a specific slope and returns the needed foundation.
 *
 * @param tileh Tile slope.
 * @param bits  Trackbits.
 * @return Needed foundation or Foundation::Invalid if track/slope combination is not allowed.
 */
Foundation GetRailFoundation(Slope tileh, TrackBits bits)
{
	if (bits.None()) return Foundation::None;

	if (IsSteepSlope(tileh)) {
		/* Test for inclined foundations */
		if (bits == Track::X) return Foundation::InclinedX;
		if (bits == Track::Y) return Foundation::InclinedY;

		/* Get higher track */
		Corner highest_corner = GetHighestSlopeCorner(tileh);
		TrackBits higher_track = CornerToTrackBits(highest_corner);

		/* Only higher track? */
		if (bits == higher_track) return HalftileFoundation(highest_corner);

		/* Overlap with higher track? */
		if (TracksOverlap(bits | higher_track)) return Foundation::Invalid;

		/* either lower track or both higher and lower track */
		return bits.Any(higher_track) ? Foundation::SteepBoth : Foundation::SteepLower;
	} else {
		if (TrackBits{bits}.Reset(_valid_tracks_without_foundation[tileh]).None()) return Foundation::None;

		bool valid_on_leveled = TrackBits{bits}.Reset(_valid_tracks_on_leveled_foundation[tileh]).None();

		Corner track_corner;
		switch (bits.base()) {
			case TrackBits{Track::Left}.base(): track_corner = CORNER_W; break;
			case TrackBits{Track::Lower}.base(): track_corner = CORNER_S; break;
			case TrackBits{Track::Right}.base(): track_corner = CORNER_E; break;
			case TrackBits{Track::Upper}.base(): track_corner = CORNER_N; break;

			case TRACK_BIT_HORZ.base():
				if (tileh == SLOPE_N) return HalftileFoundation(CORNER_N);
				if (tileh == SLOPE_S) return HalftileFoundation(CORNER_S);
				return (valid_on_leveled ? Foundation::Leveled : Foundation::Invalid);

			case TRACK_BIT_VERT.base():
				if (tileh == SLOPE_W) return HalftileFoundation(CORNER_W);
				if (tileh == SLOPE_E) return HalftileFoundation(CORNER_E);
				return (valid_on_leveled ? Foundation::Leveled : Foundation::Invalid);

			case TrackBits{Track::X}.base():
				if (IsSlopeWithOneCornerRaised(tileh)) return Foundation::InclinedX;
				return (valid_on_leveled ? Foundation::Leveled : Foundation::Invalid);

			case TrackBits{Track::Y}.base():
				if (IsSlopeWithOneCornerRaised(tileh)) return Foundation::InclinedY;
				return (valid_on_leveled ? Foundation::Leveled : Foundation::Invalid);

			default:
				return (valid_on_leveled ? Foundation::Leveled : Foundation::Invalid);
		}
		/* Single diagonal track */

		/* Track must be at least valid on leveled foundation */
		if (!valid_on_leveled) return Foundation::Invalid;

		/* If slope has three raised corners, build leveled foundation */
		if (IsSlopeWithThreeCornersRaised(tileh)) return Foundation::Leveled;

		/* If neighboured corners of track_corner are lowered, build halftile foundation */
		if ((tileh & SlopeWithThreeCornersRaised(OppositeCorner(track_corner))) == SlopeWithOneCornerRaised(track_corner)) return HalftileFoundation(track_corner);

		/* else special anti-zig-zag foundation */
		return SpecialRailFoundation(track_corner);
	}
}


/**
 * Tests if a track can be build on a tile.
 *
 * @param tileh Tile slope.
 * @param rail_bits Tracks to build.
 * @param existing Tracks already built.
 * @param tile Tile (used for water test)
 * @return Error message or cost for foundation building.
 */
static CommandCost CheckRailSlope(Slope tileh, TrackBits rail_bits, TrackBits existing, TileIndex tile)
{
	/* don't allow building on the lower side of a coast */
	if (GetFloodingBehaviour(tile) != FloodingBehaviour::None) {
		if (!IsSteepSlope(tileh) && TrackBits{rail_bits}.Set(existing).Reset(_valid_tracks_on_leveled_foundation[tileh]).Any()) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);
	}

	Foundation f_new = GetRailFoundation(tileh, TrackBits{rail_bits}.Set(existing));

	/* check track/slope combination */
	if ((f_new == Foundation::Invalid) ||
			((f_new != Foundation::None) && (!_settings_game.construction.build_on_slopes))) {
		return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
	}

	Foundation f_old = GetRailFoundation(tileh, existing);
	return CommandCost(ExpensesType::Construction, f_new != f_old ? _price[Price::BuildFoundation] : (Money)0);
}

/* Validate functions for rail building */
static inline bool ValParamTrackOrientation(Track track)
{
	return IsValidTrack(track);
}

/**
 * Build a single piece of rail
 * @param flags operation to perform
 * @param tile tile  to build on
 * @param railtype railtype of being built piece (normal, mono, maglev)
 * @param track track-orientation
 * @param auto_remove_signals false = error on signal in the way, true = auto remove signals when in the way
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildSingleRail(DoCommandFlags flags, TileIndex tile, RailType railtype, Track track, bool auto_remove_signals)
{
	CommandCost cost(ExpensesType::Construction);

	if (!ValParamRailType(railtype) || !ValParamTrackOrientation(track)) return CMD_ERROR;

	Slope tileh = GetTileSlope(tile);
	TrackBits trackbit = track;

	switch (GetTileType(tile)) {
		case TileType::Railway: {
			CommandCost ret = CheckTileOwnership(tile);
			if (ret.Failed()) return ret;

			if (!IsPlainRail(tile)) return Command<Commands::LandscapeClear>::Do(flags, tile); // just get appropriate error message

			if (!IsCompatibleRail(GetRailType(tile), railtype)) return CommandCost(STR_ERROR_IMPOSSIBLE_TRACK_COMBINATION);

			ret = CheckTrackCombination(tile, trackbit);
			if (ret.Succeeded()) ret = EnsureNoTrainOnTrack(tile, track);
			if (ret.Failed()) return ret;

			ret = CheckRailSlope(tileh, trackbit, GetTrackBits(tile), tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret.GetCost());

			if (HasSignals(tile) && TracksOverlap(GetTrackBits(tile) | track)) {
				/* If adding the new track causes any overlap, all signals must be removed first */
				if (!auto_remove_signals) return CommandCost(STR_ERROR_MUST_REMOVE_SIGNALS_FIRST);

				for (Track existing_track : GetTrackBits(tile)) {
					if (HasSignalOnTrack(tile, existing_track)) {
						CommandCost ret_remove_signals = Command<Commands::RemoveSignal>::Do(flags, tile, existing_track);
						if (ret_remove_signals.Failed()) return ret_remove_signals;
						cost.AddCost(ret_remove_signals.GetCost());
					}
				}
			}

			/* If the rail types don't match, try to convert only if engines of
			 * the new rail type are not powered on the present rail type and engines of
			 * the present rail type are powered on the new rail type. */
			if (GetRailType(tile) != railtype && !HasPowerOnRail(railtype, GetRailType(tile))) {
				if (HasPowerOnRail(GetRailType(tile), railtype)) {
					ret = Command<Commands::ConvertRail>::Do(flags, tile, tile, railtype, false);
					if (ret.Failed()) return ret;
					cost.AddCost(ret.GetCost());
				} else {
					return CMD_ERROR;
				}
			}

			if (flags.Test(DoCommandFlag::Execute)) {
				SetRailGroundType(tile, RailGroundType::Barren);
				TrackBits bits = GetTrackBits(tile);
				SetTrackBits(tile, bits | trackbit);
				/* Subtract old infrastructure count. */
				uint pieces = bits.Count();
				if (TracksOverlap(bits)) pieces *= pieces;
				Company::Get(GetTileOwner(tile))->infrastructure.rail[GetRailType(tile)] -= pieces;
				/* Add new infrastructure count. */
				pieces = (bits | trackbit).Count();
				if (TracksOverlap(bits | trackbit)) pieces *= pieces;
				Company::Get(GetTileOwner(tile))->infrastructure.rail[GetRailType(tile)] += pieces;
				DirtyCompanyInfrastructureWindows(GetTileOwner(tile));
			}
			break;
		}

		case TileType::Road: {
			/* Level crossings may only be built on these slopes */
			if (!HasBit(VALID_LEVEL_CROSSING_SLOPES, tileh)) return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);

			if (!_settings_game.construction.crossing_with_competitor && _current_company != OWNER_DEITY) {
				CommandCost ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			CommandCost ret = EnsureNoVehicleOnGround(tile);
			if (ret.Failed()) return ret;

			if (IsNormalRoad(tile)) {
				if (HasRoadWorks(tile)) return CommandCost(STR_ERROR_ROAD_WORKS_IN_PROGRESS);

				if (GetDisallowedRoadDirections(tile).Any()) return CommandCost(STR_ERROR_CROSSING_ON_ONEWAY_ROAD);

				if (RailNoLevelCrossings(railtype)) return CommandCost(STR_ERROR_CROSSING_DISALLOWED_RAIL);

				RoadType roadtype_road = GetRoadTypeRoad(tile);
				RoadType roadtype_tram = GetRoadTypeTram(tile);

				if (roadtype_road != INVALID_ROADTYPE && RoadNoLevelCrossing(roadtype_road)) return CommandCost(STR_ERROR_CROSSING_DISALLOWED_ROAD);
				if (roadtype_tram != INVALID_ROADTYPE && RoadNoLevelCrossing(roadtype_tram)) return CommandCost(STR_ERROR_CROSSING_DISALLOWED_ROAD);

				RoadBits road = GetRoadBits(tile, RoadTramType::Road);
				RoadBits tram = GetRoadBits(tile, RoadTramType::Tram);
				if ((track == Track::X && !(road | tram).Any(ROAD_X)) ||
						(track == Track::Y && !(road | tram).Any(ROAD_Y))) {
					Owner road_owner = GetRoadOwner(tile, RoadTramType::Road);
					Owner tram_owner = GetRoadOwner(tile, RoadTramType::Tram);
					/* Disallow breaking end-of-line of someone else
					 * so trams can still reverse on this tile. */
					if (Company::IsValidID(tram_owner) && tram.Count() == 1) {
						ret = CheckOwnership(tram_owner);
						if (ret.Failed()) return ret;
					}

					uint num_new_road_pieces = road.Any() ? 2 - road.Count() : 0;
					if (num_new_road_pieces > 0) {
						cost.AddCost(num_new_road_pieces * RoadBuildCost(roadtype_road));
					}

					uint num_new_tram_pieces = tram.Any() ? 2 - tram.Count() : 0;
					if (num_new_tram_pieces > 0) {
						cost.AddCost(num_new_tram_pieces * RoadBuildCost(roadtype_tram));
					}

					if (flags.Test(DoCommandFlag::Execute)) {
						MakeRoadCrossing(tile, road_owner, tram_owner, _current_company, (track == Track::X ? Axis::Y : Axis::X), railtype, roadtype_road, roadtype_tram, GetTownIndex(tile));
						UpdateLevelCrossing(tile, false);
						MarkDirtyAdjacentLevelCrossingTiles(tile, GetCrossingRoadAxis(tile));
						Company::Get(_current_company)->infrastructure.rail[railtype] += LEVELCROSSING_TRACKBIT_FACTOR;
						DirtyCompanyInfrastructureWindows(_current_company);
						if (num_new_road_pieces > 0 && Company::IsValidID(road_owner)) {
							Company::Get(road_owner)->infrastructure.road[roadtype_road] += num_new_road_pieces;
							DirtyCompanyInfrastructureWindows(road_owner);
						}
						if (num_new_tram_pieces > 0 && Company::IsValidID(tram_owner)) {
							Company::Get(tram_owner)->infrastructure.road[roadtype_tram] += num_new_tram_pieces;
							DirtyCompanyInfrastructureWindows(tram_owner);
						}
					}
					break;
				}
			}

			if (IsLevelCrossing(tile) && GetCrossingRailTrack(tile) == trackbit) {
				return CommandCost(STR_ERROR_ALREADY_BUILT);
			}
			[[fallthrough]];
		}

		default: {
			/* Will there be flat water on the lower halftile? */
			bool water_ground = IsTileType(tile, TileType::Water) && IsSlopeWithOneCornerRaised(tileh);

			CommandCost ret = CheckRailSlope(tileh, trackbit, {}, tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret.GetCost());

			ret = Command<Commands::LandscapeClear>::Do(flags, tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret.GetCost());

			if (water_ground) {
				cost.AddCost(-_price[Price::ClearWater]);
				cost.AddCost(_price[Price::ClearRough]);
			}

			if (flags.Test(DoCommandFlag::Execute)) {
				MakeRailNormal(tile, _current_company, trackbit, railtype);
				if (water_ground) {
					SetRailGroundType(tile, RailGroundType::HalfTileWater);
					if (IsPossibleDockingTile(tile)) CheckForDockingTile(tile);
				}
				Company::Get(_current_company)->infrastructure.rail[railtype]++;
				DirtyCompanyInfrastructureWindows(_current_company);
			}
			break;
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		MarkTileDirtyByTile(tile);
		AddTrackToSignalBuffer(tile, track, _current_company);
		YapfNotifyTrackLayoutChange(tile, track);
	}

	cost.AddCost(RailBuildCost(railtype));
	return cost;
}

/**
 * Remove a single piece of track
 * @param flags operation to perform
 * @param tile tile to remove track from
 * @param track rail orientation
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveSingleRail(DoCommandFlags flags, TileIndex tile, Track track)
{
	CommandCost cost(ExpensesType::Construction);
	bool crossing = false;

	if (!ValParamTrackOrientation(track)) return CMD_ERROR;

	/* Need to read tile owner now because it may change when the rail is removed
	 * Also, in case of floods, _current_company != owner
	 * There may be invalid tiletype even in exec run (when removing long track),
	 * so do not call GetTileOwner(tile) in any case here */
	Owner owner = INVALID_OWNER;

	Train *v = nullptr;

	switch (GetTileType(tile)) {
		case TileType::Road: {
			if (!IsLevelCrossing(tile) || GetCrossingRailTrack(tile) != track) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);

			if (_current_company != OWNER_WATER) {
				CommandCost ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			if (!flags.Test(DoCommandFlag::Bankrupt)) {
				CommandCost ret = EnsureNoVehicleOnGround(tile);
				if (ret.Failed()) return ret;
			}

			cost.AddCost(RailClearCost(GetRailType(tile)));

			if (flags.Test(DoCommandFlag::Execute)) {
				UpdateAdjacentLevelCrossingTilesOnLevelCrossingRemoval(tile, GetCrossingRoadAxis(tile));

				if (HasReservedTracks(tile, track)) {
					v = GetTrainForReservation(tile, track);
					if (v != nullptr) FreeTrainTrackReservation(v);
				}

				owner = GetTileOwner(tile);
				Company::Get(owner)->infrastructure.rail[GetRailType(tile)] -= LEVELCROSSING_TRACKBIT_FACTOR;
				DirtyCompanyInfrastructureWindows(owner);
				MakeRoadNormal(tile, GetCrossingRoadBits(tile), GetRoadTypeRoad(tile), GetRoadTypeTram(tile), GetTownIndex(tile), GetRoadOwner(tile, RoadTramType::Road), GetRoadOwner(tile, RoadTramType::Tram));
				DeleteNewGRFInspectWindow(GrfSpecFeature::RailTypes, tile.base());
			}
			break;
		}

		case TileType::Railway: {
			TrackBits present;
			/* There are no rails present at depots. */
			if (!IsPlainRail(tile)) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);

			if (_current_company != OWNER_WATER) {
				CommandCost ret = CheckTileOwnership(tile);
				if (ret.Failed()) return ret;
			}

			CommandCost ret = EnsureNoTrainOnTrack(tile, track);
			if (ret.Failed()) return ret;

			present = GetTrackBits(tile);
			if (!present.Test(track)) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
			if (present == TRACK_BIT_CROSS) crossing = true;

			cost.AddCost(RailClearCost(GetRailType(tile)));

			/* Charge extra to remove signals on the track, if they are there */
			if (HasSignalOnTrack(tile, track)) {
				cost.AddCost(Command<Commands::RemoveSignal>::Do(flags, tile, track));
			}

			if (flags.Test(DoCommandFlag::Execute)) {
				if (HasReservedTracks(tile, track)) {
					v = GetTrainForReservation(tile, track);
					if (v != nullptr) FreeTrainTrackReservation(v);
				}

				owner = GetTileOwner(tile);

				/* Subtract old infrastructure count. */
				uint pieces = present.Count();
				if (TracksOverlap(present)) pieces *= pieces;
				Company::Get(owner)->infrastructure.rail[GetRailType(tile)] -= pieces;
				/* Add new infrastructure count. */
				present.Flip(track);
				pieces = present.Count();
				if (TracksOverlap(present)) pieces *= pieces;
				Company::Get(owner)->infrastructure.rail[GetRailType(tile)] += pieces;
				DirtyCompanyInfrastructureWindows(owner);

				if (present.None()) {
					Slope tileh = GetTileSlope(tile);
					/* If there is flat water on the lower halftile, convert the tile to shore so the water remains */
					if (GetRailGroundType(tile) == RailGroundType::HalfTileWater && IsSlopeWithOneCornerRaised(tileh)) {
						bool docking = IsDockingTile(tile);
						MakeShore(tile);
						SetDockingTile(tile, docking);
					} else {
						DoClearSquare(tile);
					}
					DeleteNewGRFInspectWindow(GrfSpecFeature::RailTypes, tile.base());
				} else {
					SetTrackBits(tile, present);
					SetTrackReservation(tile, GetRailReservationTrackBits(tile) & present);
				}
			}
			break;
		}

		default: return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* if we got that far, 'owner' variable is set correctly */
		assert(Company::IsValidID(owner));

		MarkTileDirtyByTile(tile);
		if (crossing) {
			/* crossing is set when only TRACK_BIT_X and TRACK_BIT_Y are set. As we
			 * are removing one of these pieces, we'll need to update signals for
			 * both directions explicitly, as after the track is removed it won't
			 * 'connect' with the other piece. */
			AddTrackToSignalBuffer(tile, Track::X, owner);
			AddTrackToSignalBuffer(tile, Track::Y, owner);
			YapfNotifyTrackLayoutChange(tile, Track::X);
			YapfNotifyTrackLayoutChange(tile, Track::Y);
		} else {
			AddTrackToSignalBuffer(tile, track, owner);
			YapfNotifyTrackLayoutChange(tile, track);
		}

		if (v != nullptr) TryPathReserve(v, true);
	}

	return cost;
}


/**
 * Called from water_cmd if a non-flat rail-tile gets flooded and should be converted to shore.
 * The function floods the lower halftile, if the tile has a halftile foundation.
 *
 * @param t The tile to flood.
 * @return true if something was flooded.
 */
bool FloodHalftile(TileIndex t)
{
	assert(IsPlainRailTile(t));

	bool flooded = false;
	if (GetRailGroundType(t) == RailGroundType::HalfTileWater) return flooded;

	Slope tileh = GetTileSlope(t);
	TrackBits rail_bits = GetTrackBits(t);

	if (IsSlopeWithOneCornerRaised(tileh)) {
		TrackBits lower_track = CornerToTrackBits(OppositeCorner(GetHighestSlopeCorner(tileh)));

		TrackBits to_remove = lower_track & rail_bits;
		if (to_remove.Any()) {
			AutoRestoreBackup cur_company(_current_company, OWNER_WATER);
			flooded = Command<Commands::RemoveRail>::Do(DoCommandFlag::Execute, t, FindFirstTrack(to_remove)).Succeeded();
			if (!flooded) return flooded; // not yet floodable

			rail_bits.Reset(to_remove);
			if (rail_bits.None()) {
				MakeShore(t);
				MarkTileDirtyByTile(t);
				return flooded;
			}
		}

		if (IsNonContinuousFoundation(GetRailFoundation(tileh, rail_bits))) {
			flooded = true;
			SetRailGroundType(t, RailGroundType::HalfTileWater);
			MarkTileDirtyByTile(t);
		}
	} else {
		/* Make shore on steep slopes and 'three-corners-raised'-slopes. */
		if (ApplyFoundationToSlope(GetRailFoundation(tileh, rail_bits), tileh) == 0) {
			if (IsSteepSlope(tileh) || IsSlopeWithThreeCornersRaised(tileh)) {
				flooded = true;
				SetRailGroundType(t, RailGroundType::HalfTileWater);
				MarkTileDirtyByTile(t);
			}
		}
	}
	return flooded;
}

/** Tile deltas for each trackdir. */
static const TrackdirIndexArray<TileIndexDiffC> _trackdelta{{{
	{ -1,  0 }, {  0,  1 }, { -1,  0 }, {  0,  1 }, {  1,  0 }, {  0,  1 },
	{  0,  0 },
	{  0,  0 },
	{  1,  0 }, {  0, -1 }, {  0, -1 }, {  1,  0 }, {  0, -1 }, { -1,  0 },
	{  0,  0 },
	{  0,  0 }
}}};

/**
 * Get the other Trackdir for a non-diagonal Trackdir.
 * i.e. upper -> lower, left -> right, etc.
 * @param trackdir the trackdir.
 * @return The other trackdir.
 */
static Trackdir GetOtherTrackdir(Trackdir trackdir)
{
	return static_cast<Trackdir>(to_underlying(trackdir) ^ 1);
}

static CommandCost ValidateAutoDrag(Trackdir *trackdir, TileIndex start, TileIndex end)
{
	int x = TileX(start);
	int y = TileY(start);
	int ex = TileX(end);
	int ey = TileY(end);

	if (!ValParamTrackOrientation(TrackdirToTrack(*trackdir))) return CMD_ERROR;

	/* calculate delta x,y from start to end tile */
	int dx = ex - x;
	int dy = ey - y;

	/* calculate delta x,y for the first direction */
	int trdx = _trackdelta[*trackdir].x;
	int trdy = _trackdelta[*trackdir].y;

	if (!IsDiagonalTrackdir(*trackdir)) {
		trdx += _trackdelta[GetOtherTrackdir(*trackdir)].x;
		trdy += _trackdelta[GetOtherTrackdir(*trackdir)].y;
	}

	/* validate the direction */
	while ((trdx <= 0 && dx > 0) ||
			(trdx >= 0 && dx < 0) ||
			(trdy <= 0 && dy > 0) ||
			(trdy >= 0 && dy < 0)) {
		if (!HasBit(to_underlying(*trackdir), 3)) { // first direction is invalid, try the other
			*trackdir = ReverseTrackdir(*trackdir); // reverse the direction
			trdx = -trdx;
			trdy = -trdy;
		} else { // other direction is invalid too, invalid drag
			return CMD_ERROR;
		}
	}

	/* (for diagonal tracks, this is already made sure of by above test), but:
	 * for non-diagonal tracks, check if the start and end tile are on 1 line */
	if (!IsDiagonalTrackdir(*trackdir)) {
		trdx = _trackdelta[*trackdir].x;
		trdy = _trackdelta[*trackdir].y;
		if (abs(dx) != abs(dy) && abs(dx) + abs(trdy) != abs(dy) + abs(trdx)) return CMD_ERROR;
	}

	return CommandCost();
}

/**
 * Build or remove a stretch of railroad tracks.
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param railtype railroad type normal/maglev (0 = normal, 1 = mono, 2 = maglev), only used for building
 * @param track track-orientation
 * @param remove remove tracks?
 * @param auto_remove_signals false = error on signal in the way, true = auto remove signals when in the way, only used for building
 * @param fail_on_obstacle false = build starting from and up to an obstacle, true = fail if an obstacle is found (used for AIs)
 * @return the cost of this operation or an error
 */
static CommandCost CmdRailTrackHelper(DoCommandFlags flags, TileIndex tile, TileIndex end_tile, RailType railtype, Track track, bool remove, bool auto_remove_signals, bool fail_on_obstacle)
{
	CommandCost total_cost(ExpensesType::Construction);

	if ((!remove && !ValParamRailType(railtype)) || !ValParamTrackOrientation(track)) return CMD_ERROR;
	if (end_tile >= Map::Size() || tile >= Map::Size()) return CMD_ERROR;

	Trackdir trackdir = TrackToTrackdir(track);

	CommandCost ret = ValidateAutoDrag(&trackdir, tile, end_tile);
	if (ret.Failed()) return ret;

	bool had_success = false;
	CommandCost last_error = CMD_ERROR;
	for (;;) {
		ret = remove ? Command<Commands::RemoveRail>::Do(flags, tile, TrackdirToTrack(trackdir)) : Command<Commands::BuildRail>::Do(flags, tile, railtype, TrackdirToTrack(trackdir), auto_remove_signals);
		if (!remove && !fail_on_obstacle && last_error.GetErrorMessage() == STR_ERROR_ALREADY_BUILT) had_success = true;

		if (ret.Failed()) {
			last_error = std::move(ret);
			if (last_error.GetErrorMessage() != STR_ERROR_ALREADY_BUILT && !remove) {
				if (fail_on_obstacle) return last_error;
				if (had_success) break; // Keep going if we haven't constructed any rail yet, skipping the start of the drag
			}

			/* Ownership errors are more important. */
			if (last_error.GetErrorMessage() == STR_ERROR_OWNED_BY && remove) break;
		} else {
			had_success = true;
			total_cost.AddCost(ret.GetCost());
		}

		if (tile == end_tile) break;

		tile += ToTileIndexDiff(_trackdelta[trackdir]);

		/* toggle railbit for the non-diagonal tracks */
		if (!IsDiagonalTrackdir(trackdir)) trackdir = GetOtherTrackdir(trackdir);
	}

	if (had_success) return total_cost;
	return last_error;
}

/**
 * Build rail on a stretch of track.
 * Stub for the unified rail builder/remover
 * @param flags operation to perform
 * @param end_tile end tile of drag
 * @param start_tile start tile of drag
 * @param railtype railroad type normal/maglev (0 = normal, 1 = mono, 2 = maglev), only used for building
 * @param track track-orientation
 * @param auto_remove_signals false = build up to an obstacle, true = fail if an obstacle is found (used for AIs).
 * @param fail_on_obstacle false = error on signal in the way, true = auto remove signals when in the way
 * @return The cost of this operation or an error.
 * @see CmdRailTrackHelper
 */
CommandCost CmdBuildRailroadTrack(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile, RailType railtype, Track track, bool auto_remove_signals, bool fail_on_obstacle)
{
	return CmdRailTrackHelper(flags, start_tile, end_tile, railtype, track, false, auto_remove_signals, fail_on_obstacle);
}

/**
 * Build rail on a stretch of track.
 * Stub for the unified rail builder/remover
 * @param flags operation to perform
 * @param end_tile end tile of drag
 * @param start_tile start tile of drag
 * @param track track-orientation
 * @return the cost of this operation or an error
 * @see CmdRailTrackHelper
 */
CommandCost CmdRemoveRailroadTrack(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile, Track track)
{
	return CmdRailTrackHelper(flags, start_tile, end_tile, INVALID_RAILTYPE, track, true, false, false);
}

/**
 * Build a train depot
 * @param flags operation to perform
 * @param tile position of the train depot
 * @param railtype rail type
 * @param dir entrance direction
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildTrainDepot(DoCommandFlags flags, TileIndex tile, RailType railtype, DiagDirection dir)
{
	/* check railtype and valid direction for depot (0 through 3), 4 in total */
	if (!ValParamRailType(railtype) || !IsValidDiagDirection(dir)) return CMD_ERROR;

	Slope tileh = GetTileSlope(tile);

	CommandCost cost(ExpensesType::Construction);

	/* Prohibit construction if
	 * The tile is non-flat AND
	 * 1) build-on-slopes is disabled
	 * 2) the tile is steep i.e. spans two height levels
	 * 3) the exit points in the wrong direction
	 */

	if (tileh != SLOPE_FLAT) {
		if (!_settings_game.construction.build_on_slopes) return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		if (!CanBuildDepotByTileh(dir, tileh)) return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
		cost.AddCost(_price[Price::BuildFoundation]);
	}

	/* Allow the user to rotate the depot instead of having to destroy it and build it again */
	bool rotate_existing_depot = false;
	Train *v = nullptr;
	if (IsRailDepotTile(tile) && railtype == GetRailType(tile)) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;

		DiagDirection old_dir = GetRailDepotDirection(tile);
		if (dir == old_dir) return CommandCost();

		ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;

		if (HasDepotReservation(tile)) {
			v = GetTrainForReservation(tile, DiagDirToDiagTrack(old_dir));
		}

		rotate_existing_depot = true;
	}

	if (!rotate_existing_depot) {
		cost.AddCost(Command<Commands::LandscapeClear>::Do(flags, tile));
		if (cost.Failed()) return cost;

		if (IsBridgeAbove(tile)) {
			int height_diff = GetTileMaxZ(tile) + MINIMAL_DEPOT_BRIDGE_HEIGHT - GetBridgeHeight(GetSouthernBridgeEnd(tile));
			if (height_diff > 0) return CommandCostWithParam(STR_ERROR_BRIDGE_TOO_LOW_FOR_TRAIN_DEPOT, height_diff * TILE_HEIGHT_STEP);
		}

		if (!Depot::CanAllocateItem()) return CMD_ERROR;
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		if (v != nullptr) FreeTrainTrackReservation(v);

		if (rotate_existing_depot) {
			SetDepotReservation(tile, false);
			SetRailDepotExitDirection(tile, dir);
		} else {
			Depot *d = Depot::Create(tile);

			MakeRailDepot(tile, _current_company, d->index, dir, railtype);
			MakeDefaultName(d);

			Company::Get(_current_company)->infrastructure.rail[railtype]++;
			DirtyCompanyInfrastructureWindows(_current_company);
		}

		MarkTileDirtyByTile(tile);
		AddSideToSignalBuffer(tile, DiagDirection::Invalid, _current_company);
		YapfNotifyTrackLayoutChange(tile, DiagDirToDiagTrack(dir));

		if (v != nullptr) TryPathReserve(v, true);
	}

	cost.AddCost(_price[Price::BuildDepotTrain]);
	cost.AddCost(RailBuildCost(railtype));
	return cost;
}

/**
 * Put a signal on a rail tunnel's or bridge's portal, or take it off again.
 *
 * Both ends are done at once: the end the player clicked gets the signal a
 * train entering reads, the far end the one a train inside reads on its way
 * out. A bore signalled at one end only would be a trap, and there is nowhere
 * to click in the middle of a bridge anyway.
 *
 * The kind of signal the player was building is remembered and drawn, so that
 * a line of path signals does not turn into a block signal where it crosses a
 * bridge. It is a picture and nothing else: a bore is one section whichever
 * kind stands on its mouths.
 *
 * @param flags type of operation
 * @param tile the portal clicked
 * @param remove take the signals off instead of putting them on
 * @param sigtype which signal the player is building, for the picture
 * @param sigvar electric or semaphore, likewise
 * @return the cost of this operation or an error
 */
CommandCost BuildTunnelBridgeSignals(DoCommandFlags flags, TileIndex tile, bool remove, SignalType sigtype, SignalVariant sigvar)
{
	CommandCost ret = CheckTileOwnership(tile);
	if (ret.Failed()) return ret;

	TileIndex other = GetOtherTunnelBridgeEnd(tile);
	bool has = HasTunnelBridgeSignal(tile, TunnelBridgeSignal::Entry);
	if (remove && !has) return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);
	if (!remove && has) return CommandCost(); // already signalled, nothing to do and nothing to pay

	/* A train in the bore would find the world changed under it. */
	if (!remove || HasTunnelBridgeReservation(tile)) {
		ret = TunnelBridgeIsFree(tile, other);
		if (ret.Failed()) return ret;
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		Company *c = Company::GetIfValid(GetTileOwner(tile));
		for (TileIndex t : {tile, other}) {
			SetTunnelBridgeSignal(t, TunnelBridgeSignal::Entry, !remove);
			SetTunnelBridgeSignal(t, TunnelBridgeSignal::Exit, !remove);
			if (!remove) {
				SetTunnelBridgeSignalState(t, TunnelBridgeSignal::Entry, SignalState::Green);
				SetTunnelBridgeSignalState(t, TunnelBridgeSignal::Exit, SignalState::Green);
				SetTunnelBridgeSignalType(t, sigtype);
				SetTunnelBridgeSignalVariant(t, sigvar);
			} else {
				SetTunnelBridgeSignalType(t, SignalType::Block);
				SetTunnelBridgeSignalVariant(t, SignalVariant::Electric);
			}
			MarkTileDirtyByTile(t);
		}
		if (c != nullptr) {
			c->infrastructure.signal += remove ? -4 : 4;
			DirtyCompanyInfrastructureWindows(c->index);
		}
		AddSideToSignalBuffer(tile, DiagDirection::Invalid, GetTileOwner(tile));
		AddSideToSignalBuffer(other, DiagDirection::Invalid, GetTileOwner(other));
	}

	/* Four signal heads, two at each end. */
	return CommandCost(ExpensesType::Construction, 4 * (remove ? _price[Price::ClearSignals] : _price[Price::BuildSignals]));
}

/**
 * Build signals, alternate between double/single, signal/semaphore,
 * pre/exit/combo-signals, and what-else not. If the rail piece does not
 * have any signals, signal cycling is ignored
 * @param flags operation to perform
 * @param tile tile where to build the signals
 * @param track track-orientation
 * @param sigtype type of the signal
 * @param sigvar variant of signal type (normal/semaphore)
 * @param ctrl_pressed true = override signal/semaphore, or pre/exit/combo signal or toggle variant (CTRL-toggle)
 * @param convert_signal convert the present signal type and variant
 * @param cycle_start start cycle from this signal type
 * @param cycle_stop wrap around after this signal type
 * @param num_dir_cycle cycle the signal direction this many times
 * @param skip_existing_signals true = don't modify an existing signal but don't fail either, false = always set new signal type
 * @param signals_copy used for CmdBuildManySignals() to copy direction of first signal
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildSingleSignal(DoCommandFlags flags, TileIndex tile, Track track, SignalType sigtype, SignalVariant sigvar, bool convert_signal, bool skip_existing_signals, bool ctrl_pressed, SignalType cycle_start, SignalType cycle_stop, uint8_t num_dir_cycle, uint8_t signals_copy)
{
	if (sigtype >= SignalType::End || sigvar >= SignalVariant::End) return CMD_ERROR;
	if (cycle_start > cycle_stop || cycle_stop >= SignalType::End) return CMD_ERROR;

	if (ctrl_pressed) sigvar = (sigvar == SignalVariant::Electric ? SignalVariant::Semaphore : SignalVariant::Electric);

	/* A tunnel or a bridge takes its signals at the mouths and nowhere else,
	 * so a click on one means the whole bore. There is nothing to choose
	 * about: no type, no variant, no direction -- both ends get a head each
	 * way, which is the only arrangement that makes sense on a stretch a
	 * train cannot stop in. */
	if (IsTileType(tile, TileType::TunnelBridge) && GetTunnelBridgeTransportType(tile) == TransportType::Rail) {
		return BuildTunnelBridgeSignals(flags, tile, false, sigtype, sigvar);
	}

	/* You can only build signals on plain rail tiles, and the selected track must exist */
	if (!ValParamTrackOrientation(track) || !IsPlainRailTile(tile) ||
			!HasTrack(tile, track)) {
		return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
	}
	/* Protect against invalid signal copying */
	if (signals_copy != 0 && (signals_copy & SignalOnTrack(track)) == 0) return CMD_ERROR;

	CommandCost ret = CheckTileOwnership(tile);
	if (ret.Failed()) return ret;

	/* See if this is a valid track combination for signals (no overlap) */
	if (TracksOverlap(GetTrackBits(tile))) return CommandCost(STR_ERROR_NO_SUITABLE_RAILROAD_TRACK);

	/* In case we don't want to change an existing signal, return without error. */
	if (skip_existing_signals && HasSignalOnTrack(tile, track)) return CommandCost();

	/* you can not convert a signal if no signal is on track */
	if (convert_signal && !HasSignalOnTrack(tile, track)) return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);

	CommandCost cost;
	if (!HasSignalOnTrack(tile, track)) {
		/* build new signals */
		cost = CommandCost(ExpensesType::Construction, _price[Price::BuildSignals]);
	} else {
		if (signals_copy != 0 && sigvar != GetSignalVariant(tile, track)) {
			/* convert signals <-> semaphores */
			cost = CommandCost(ExpensesType::Construction, _price[Price::BuildSignals] + _price[Price::ClearSignals]);

		} else if (convert_signal) {
			/* convert button pressed */
			if (ctrl_pressed || GetSignalVariant(tile, track) != sigvar) {
				/* it costs money to change signal variant (light or semaphore) */
				cost = CommandCost(ExpensesType::Construction, _price[Price::BuildSignals] + _price[Price::ClearSignals]);
			} else {
				/* it is free to change signal type (block, exit, entry, combo, path, etc) */
				cost = CommandCost();
			}

		} else {
			/* it is free to change orientation or number of signals on the tile (for block/presignals which allow signals in both directions) */
			cost = CommandCost();
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		Train *v = nullptr;
		/* The new/changed signal could block our path. As this can lead to
		 * stale reservations, we clear the path reservation here and try
		 * to redo it later on. */
		if (HasReservedTracks(tile, track)) {
			v = GetTrainForReservation(tile, track);
			if (v != nullptr) FreeTrainTrackReservation(v);
		}

		if (!HasSignals(tile)) {
			/* there are no signals at all on this tile yet */
			SetHasSignals(tile, true);
			SetSignalStates(tile, 0xF); // all signals are on
			SetPresentSignals(tile, 0); // no signals built by default
			SetSignalType(tile, track, sigtype);
			SetSignalVariant(tile, track, sigvar);
		}

		/* Subtract old signal infrastructure count. */
		Company::Get(GetTileOwner(tile))->infrastructure.signal -= CountBits(GetPresentSignals(tile));

		if (signals_copy == 0) {
			if (!HasSignalOnTrack(tile, track)) {
				/* build new signals */
				SetPresentSignals(tile, GetPresentSignals(tile) | (IsPbsSignal(sigtype) ? KillFirstBit(SignalOnTrack(track)) : SignalOnTrack(track)));
				SetSignalType(tile, track, sigtype);
				SetSignalVariant(tile, track, sigvar);
				while (num_dir_cycle-- > 0) CycleSignalSide(tile, track);
			} else {
				if (convert_signal) {
					/* convert signal button pressed */
					if (ctrl_pressed) {
						/* toggle the present signal variant: SignalVariant::Electric <-> SignalVariant::Semaphore */
						SetSignalVariant(tile, track, (GetSignalVariant(tile, track) == SignalVariant::Electric) ? SignalVariant::Semaphore : SignalVariant::Electric);
						/* Query current signal type so the check for PBS signals below works. */
						sigtype = GetSignalType(tile, track);
					} else {
						/* convert the present signal to the chosen type and variant */
						SetSignalType(tile, track, sigtype);
						SetSignalVariant(tile, track, sigvar);
						if (IsPbsSignal(sigtype) && (GetPresentSignals(tile) & SignalOnTrack(track)) == SignalOnTrack(track)) {
							SetPresentSignals(tile, (GetPresentSignals(tile) & ~SignalOnTrack(track)) | KillFirstBit(SignalOnTrack(track)));
						}
					}

				} else if (ctrl_pressed) {
					/* cycle between cycle_start and cycle_end */
					sigtype = static_cast<SignalType>(to_underlying(GetSignalType(tile, track)) + 1);

					if (sigtype < cycle_start || sigtype > cycle_stop) sigtype = cycle_start;

					SetSignalType(tile, track, sigtype);
					if (IsPbsSignal(sigtype) && (GetPresentSignals(tile) & SignalOnTrack(track)) == SignalOnTrack(track)) {
						SetPresentSignals(tile, (GetPresentSignals(tile) & ~SignalOnTrack(track)) | KillFirstBit(SignalOnTrack(track)));
					}
				} else {
					/* cycle the signal side: both -> left -> right -> both -> ... */
					CycleSignalSide(tile, track);
					/* Query current signal type so the check for PBS signals below works. */
					sigtype = GetSignalType(tile, track);
				}
			}
		} else {
			/* If CmdBuildManySignals is called with copying signals, just copy the
			 * direction of the first signal given as parameter by CmdBuildManySignals */
			SetPresentSignals(tile, (GetPresentSignals(tile) & ~SignalOnTrack(track)) | (signals_copy & SignalOnTrack(track)));
			SetSignalVariant(tile, track, sigvar);
			SetSignalType(tile, track, sigtype);
		}

		/* Add new signal infrastructure count. */
		Company::Get(GetTileOwner(tile))->infrastructure.signal += CountBits(GetPresentSignals(tile));
		DirtyCompanyInfrastructureWindows(GetTileOwner(tile));

		if (IsPbsSignal(sigtype)) {
			/* PBS signals should show red unless they are on reserved tiles without a train. */
			uint mask = GetPresentSignals(tile) & SignalOnTrack(track);
			SetSignalStates(tile, (GetSignalStates(tile) & ~mask) | ((GetRailReservationTrackBits(tile).Test(track) && EnsureNoVehicleOnGround(tile).Succeeded() ? UINT_MAX : 0) & mask));
		}
		MarkTileDirtyByTile(tile);
		AddTrackToSignalBuffer(tile, track, _current_company);
		YapfNotifyTrackLayoutChange(tile, track);
		if (v != nullptr && v->track != Track::Depot) {
			Train *moving_front = v->GetMovingFront();
			/* Extend the train's path if it's not stopped or loading, or not at a safe position. */
			if (!((v->vehstatus.Test(VehState::Stopped) && v->cur_speed == 0) || v->current_order.IsType(OT_LOADING)) ||
					!IsSafeWaitingPosition(v, moving_front->tile, moving_front->GetVehicleTrackdir(), true, _settings_game.pf.forbid_90_deg)) {
				TryPathReserve(v, true);
			}
		}
	}

	return cost;
}

static bool AdvanceSignalAutoFill(TileIndex &tile, Trackdir &trackdir, bool remove)
{
	/* We only process starting tiles of tunnels or bridges so jump to the other end before moving further. */
	if (IsTileType(tile, TileType::TunnelBridge)) tile = GetOtherTunnelBridgeEnd(tile);

	tile = AddTileIndexDiffCWrap(tile, _trackdelta[trackdir]);
	if (tile == INVALID_TILE) return false;

	/* Check for track bits on the new tile */
	TrackdirBits trackdirbits = GetTileTrackStatus(tile, TransportType::Rail, RoadTramType::Invalid).trackdirs;

	if (TracksOverlap(TrackdirBitsToTrackBits(trackdirbits))) return false;
	trackdirbits &= TrackdirReachesTrackdirs(trackdir);

	/* No track bits, must stop */
	if (trackdirbits.None()) return false;

	/* Get the first track dir */
	trackdir = RemoveFirstTrackdir(trackdirbits);

	/* Any left? It's a junction so we stop */
	if (trackdirbits.Any()) return false;

	switch (GetTileType(tile)) {
		case TileType::Railway:
			if (IsRailDepot(tile)) return false;
			if (!remove && HasSignalOnTrack(tile, TrackdirToTrack(trackdir))) return false;
			break;

		case TileType::Road:
			if (!IsLevelCrossing(tile)) return false;
			break;

		case TileType::TunnelBridge: {
			if (GetTunnelBridgeTransportType(tile) != TransportType::Rail) return false;
			if (GetTunnelBridgeDirection(tile) != TrackdirToExitdir(trackdir)) return false;
			break;
		}

		default: return false;
	}
	return true;
}

/**
 * Build many signals by dragging; AutoSignals
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param track track-orientation
 * @param sigtype default signal type
 * @param sigvar signal variant to build
 * @param mode true = override signal/semaphore, or pre/exit/combo signal (CTRL-toggle)
 * @param remove remove signals?
 * @param autofill fill beyond selected stretch?
 * @param minimise_gaps false = keep fixed distance, true = minimise gaps between signals
 * @param signal_density user defined signals_density
 * @return the cost of this operation or an error
 */
static CommandCost CmdSignalTrackHelper(DoCommandFlags flags, TileIndex tile, TileIndex end_tile, Track track, SignalType sigtype, SignalVariant sigvar, bool mode, bool remove, bool autofill, bool minimise_gaps, int signal_density)
{
	CommandCost total_cost(ExpensesType::Construction);

	if (end_tile >= Map::Size() || !ValParamTrackOrientation(track)) return CMD_ERROR;
	if (signal_density == 0 || signal_density > 20) return CMD_ERROR;
	if (!remove && (sigtype >= SignalType::End || sigvar >= SignalVariant::End)) return CMD_ERROR;

	if (!IsPlainRailTile(tile)) return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
	TileIndex start_tile = tile;

	/* Interpret signal_density as the logical length of said amount of tiles in X/Y direction. */
	signal_density *= TILE_AXIAL_DISTANCE;

	Trackdir trackdir = TrackToTrackdir(track);
	CommandCost ret = ValidateAutoDrag(&trackdir, tile, end_tile);
	if (ret.Failed()) return ret;

	track = TrackdirToTrack(trackdir); // trackdir might have changed, keep track in sync
	Trackdir start_trackdir = trackdir;

	/* Must start on a valid track to be able to avoid loops */
	if (!HasTrack(tile, track)) return CMD_ERROR;

	uint8_t signals;
	/* copy the signal-style of the first rail-piece if existing */
	if (HasSignalOnTrack(tile, track)) {
		signals = GetPresentSignals(tile) & SignalOnTrack(track);
		assert(signals != 0);

		/* copy signal/semaphores style (independent of CTRL) */
		sigvar = GetSignalVariant(tile, track);

		sigtype = GetSignalType(tile, track);
		/* Don't but copy entry or exit-signal type */
		if (sigtype == SignalType::Entry || sigtype == SignalType::Exit) sigtype = SignalType::Block;
	} else { // no signals exist, drag a two-way signal stretch
		signals = IsPbsSignal(sigtype) ? SignalAlongTrackdir(trackdir) : SignalOnTrack(track);
	}

	uint8_t signal_dir = 0;
	if (signals & SignalAlongTrackdir(trackdir))   SetBit(signal_dir, 0);
	if (signals & SignalAgainstTrackdir(trackdir)) SetBit(signal_dir, 1);

	/* signal_ctr         - amount of tiles already processed
	 * last_used_ctr      - amount of tiles before previously placed signal
	 * signals_density    - setting to put signal on every Nth tile (double space on |, -- tracks)
	 * last_suitable_ctr  - amount of tiles before last possible signal place
	 * last_suitable_tile - last tile where it is possible to place a signal
	 * last_suitable_trackdir - trackdir of the last tile
	 **********
	 * trackdir   - trackdir to build with autorail
	 * semaphores - semaphores or signals
	 * signals    - is there a signal/semaphore on the first tile, copy its style (two-way/single-way)
	 *              and convert all others to semaphore/signal
	 * remove     - 1 remove signals, 0 build signals */
	int signal_ctr = 0;
	int last_used_ctr = -signal_density; // to force signal at first tile
	int last_suitable_ctr = 0;
	TileIndex last_suitable_tile = INVALID_TILE;
	Trackdir last_suitable_trackdir = Trackdir::Invalid;
	CommandCost last_error = CMD_ERROR;
	bool had_success = false;
	auto build_signal = [&](TileIndex tile, Trackdir trackdir, bool test_only) {
		/* Pick the correct orientation for the track direction */
		uint8_t signals = 0;
		if (HasBit(signal_dir, 0)) signals |= SignalAlongTrackdir(trackdir);
		if (HasBit(signal_dir, 1)) signals |= SignalAgainstTrackdir(trackdir);

		DoCommandFlags do_flags = test_only ? DoCommandFlags{flags}.Reset(DoCommandFlag::Execute) : flags;
		CommandCost ret = remove ? Command<Commands::RemoveSignal>::Do(do_flags, tile, TrackdirToTrack(trackdir)) : Command<Commands::BuildSignal>::Do(do_flags, tile, TrackdirToTrack(trackdir), sigtype, sigvar, false, signal_ctr == 0, mode, SignalType::Block, SignalType::Block, 0, signals);

		if (test_only) return ret.Succeeded();

		if (ret.Succeeded()) {
			had_success = true;
			total_cost.AddCost(ret.GetCost());
		} else {
			/* The "No railway" error is the least important one. */
			if (ret.GetErrorMessage() != STR_ERROR_THERE_IS_NO_RAILROAD_TRACK ||
					last_error.GetErrorMessage() == INVALID_STRING_ID) {
				last_error = ret;
			}
		}
		return ret.Succeeded();
	};

	for (;;) {
		/* A bore counts as one section however close together the drag was
		 * asked to put its signals: they can only stand at the two mouths, so
		 * a drag that runs across one is taken to mean them and the spacing
		 * has no say in it. */
		if (IsTileType(tile, TileType::TunnelBridge) && GetTunnelBridgeTransportType(tile) == TransportType::Rail) {
			CommandCost ret = BuildTunnelBridgeSignals(flags, tile, remove, sigtype, sigvar);
			if (ret.Succeeded()) {
				had_success = true;
				total_cost.AddCost(ret.GetCost());
			} else if (last_error.GetErrorMessage() == INVALID_STRING_ID) {
				last_error = ret;
			}
		} else if (remove) {
			/* In remove mode last_* stuff doesn't matter, we simply try to clear every tile. */
			build_signal(tile, trackdir, false);
		} else if (minimise_gaps) {
			/* We're trying to minimize gaps wherever possible, so keep track of last suitable
			 * position and use it if current gap exceeds required signal density. */

			if (signal_ctr > last_used_ctr + signal_density && last_suitable_tile != INVALID_TILE) {
				/* We overshot so build a signal in last good location. */
				if (build_signal(last_suitable_tile, last_suitable_trackdir, false)) {
					last_suitable_tile = INVALID_TILE;
					last_used_ctr = last_suitable_ctr;
				}
			}

			if (signal_ctr == last_used_ctr + signal_density) {
				/* Current gap matches the required density, build a signal.  */
				if (build_signal(tile, trackdir, false)) {
					last_used_ctr = signal_ctr;
					last_suitable_tile = INVALID_TILE;
				}
			} else {
				/* Test tile for a potential signal spot. */
				if (build_signal(tile, trackdir, true)) {
					last_suitable_tile = tile;
					last_suitable_ctr = signal_ctr;
					last_suitable_trackdir = trackdir;
				}
			}
		} else if (signal_ctr >= last_used_ctr + signal_density) {
			/* We're always keeping regular interval between signals so doesn't matter whether we succeed or not. */
			build_signal(tile, trackdir, false);
			last_used_ctr = signal_ctr;
		}

		if (autofill) {
			switch (GetTileType(tile)) {
				case TileType::Railway:
					signal_ctr += (IsDiagonalTrackdir(trackdir) ? TILE_AXIAL_DISTANCE : TILE_CORNER_DISTANCE);
					break;

				case TileType::Road:
					signal_ctr += TILE_AXIAL_DISTANCE;
					break;

				case TileType::TunnelBridge: {
					uint len = (GetTunnelBridgeLength(tile, GetOtherTunnelBridgeEnd(tile)) + 2) * TILE_AXIAL_DISTANCE;
					if (remove || minimise_gaps) {
						signal_ctr += len;
					} else {
						/* To keep regular interval we need to emulate placing signals on a bridge.
						 * We start with TILE_AXIAL_DISTANCE as one bridge tile gets processed in the main loop. */
						signal_ctr += TILE_AXIAL_DISTANCE;
						for (uint i = TILE_AXIAL_DISTANCE; i < len; i += TILE_AXIAL_DISTANCE) {
							if (signal_ctr >= last_used_ctr + signal_density) last_used_ctr = signal_ctr;
							signal_ctr += TILE_AXIAL_DISTANCE;
						}
					}
					break;
				}

				default: break;
			}

			if (!AdvanceSignalAutoFill(tile, trackdir, remove)) break;

			/* Prevent possible loops */
			if (tile == start_tile && trackdir == start_trackdir) break;
		} else {
			if (tile == end_tile) break;

			signal_ctr += (IsDiagonalTrackdir(trackdir) ? TILE_AXIAL_DISTANCE : TILE_CORNER_DISTANCE);
			/* toggle railbit for the non-diagonal tracks (|, -- tracks) */

			tile += ToTileIndexDiff(_trackdelta[trackdir]);
			if (!IsDiagonalTrackdir(trackdir)) trackdir = GetOtherTrackdir(trackdir);
		}
	}

	/* We may end up with the current gap exceeding the signal density so fix that if needed. */
	if (!remove && minimise_gaps && signal_ctr > last_used_ctr + signal_density && last_suitable_tile != INVALID_TILE) {
		build_signal(last_suitable_tile, last_suitable_trackdir, false);
	}

	return had_success ? total_cost : last_error;
}

/**
 * Build signals on a stretch of track.
 * Stub for the unified signal builder/remover
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param track track-orientation
 * @param sigtype default signal type
 * @param sigvar signal variant to build
 * @param mode true = override signal/semaphore, or pre/exit/combo signal (CTRL-toggle)
 * @param autofill fill beyond selected stretch?
 * @param minimise_gaps false = keep fixed distance, true = minimise gaps between signals
 * @param signal_density user defined signals_density
 * @return the cost of this operation or an error
 * @see CmdSignalTrackHelper
 */
CommandCost CmdBuildSignalTrack(DoCommandFlags flags, TileIndex tile, TileIndex end_tile, Track track, SignalType sigtype, SignalVariant sigvar, bool mode, bool autofill, bool minimise_gaps, uint8_t signal_density)
{
	return CmdSignalTrackHelper(flags, tile, end_tile, track, sigtype, sigvar, mode, false, autofill, minimise_gaps, signal_density);
}

/**
 * Remove signals
 * @param flags operation to perform
 * @param tile coordinates where signal is being deleted from
 * @param track track-orientation
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveSingleSignal(DoCommandFlags flags, TileIndex tile, Track track)
{
	/* The mouths of a bore hold one set of signals between them, so they go
	 * together the same way they were put up. */
	if (IsTileType(tile, TileType::TunnelBridge) && GetTunnelBridgeTransportType(tile) == TransportType::Rail) {
		return BuildTunnelBridgeSignals(flags, tile, true, SignalType::Block, SignalVariant::Electric);
	}

	if (!ValParamTrackOrientation(track) || !IsPlainRailTile(tile) || !HasTrack(tile, track)) {
		return CommandCost(STR_ERROR_THERE_IS_NO_RAILROAD_TRACK);
	}
	if (!HasSignalOnTrack(tile, track)) {
		return CommandCost(STR_ERROR_THERE_ARE_NO_SIGNALS);
	}

	/* Only water can remove signals from anyone */
	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;
	}

	/* Do it? */
	if (flags.Test(DoCommandFlag::Execute)) {
		Train *v = nullptr;
		if (HasReservedTracks(tile, track)) {
			v = GetTrainForReservation(tile, track);
		} else if (IsPbsSignal(GetSignalType(tile, track))) {
			/* PBS signal, might be the end of a path reservation. */
			Trackdir td = TrackToTrackdir(track);
			for (int i = 0; v == nullptr && i < 2; i++, td = ReverseTrackdir(td)) {
				/* Only test the active signal side. */
				if (!HasSignalOnTrackdir(tile, ReverseTrackdir(td))) continue;
				TileIndex next = TileAddByDiagDir(tile, TrackdirToExitdir(td));
				TrackBits tracks = TrackdirBitsToTrackBits(TrackdirReachesTrackdirs(td));
				if (HasReservedTracks(next, tracks)) {
					v = GetTrainForReservation(next, TrackBitsToTrack(GetReservedTrackbits(next) & tracks));
				}
			}
		}
		Company::Get(GetTileOwner(tile))->infrastructure.signal -= CountBits(GetPresentSignals(tile));
		SetPresentSignals(tile, GetPresentSignals(tile) & ~SignalOnTrack(track));
		Company::Get(GetTileOwner(tile))->infrastructure.signal += CountBits(GetPresentSignals(tile));
		DirtyCompanyInfrastructureWindows(GetTileOwner(tile));

		/* removed last signal from tile? */
		if (GetPresentSignals(tile) == 0) {
			SetSignalStates(tile, 0);
			SetHasSignals(tile, false);
			SetSignalVariant(tile, Track::Invalid, SignalVariant::Electric); // remove any possible semaphores
		}

		AddTrackToSignalBuffer(tile, track, GetTileOwner(tile));
		YapfNotifyTrackLayoutChange(tile, track);
		if (v != nullptr) TryPathReserve(v, false);

		MarkTileDirtyByTile(tile);
	}

	return CommandCost(ExpensesType::Construction, _price[Price::ClearSignals]);
}

/**
 * Remove signals on a stretch of track.
 * Stub for the unified signal builder/remover
 * @param flags operation to perform
 * @param tile start tile of drag
 * @param end_tile end tile of drag
 * @param track track-orientation
 * @param autofill fill beyond selected stretch?
 * @return the cost of this operation or an error
 * @see CmdSignalTrackHelper
 */
CommandCost CmdRemoveSignalTrack(DoCommandFlags flags, TileIndex tile, TileIndex end_tile, Track track, bool autofill)
{
	return CmdSignalTrackHelper(flags, tile, end_tile, track, SignalType::Block, SignalVariant::Electric, false, true, autofill, false, 1); // bit 5 is remove bit
}

/**
 * Convert one rail type to the other. You can convert normal rail to
 * monorail/maglev easily or vice-versa.
 * @param flags operation to perform
 * @param tile end tile of rail conversion drag
 * @param area_start start tile of drag
 * @param totype new railtype to convert to.
 * @param diagonal build diagonally or not.
 * @return the cost of this operation or an error
 */
CommandCost CmdConvertRail(DoCommandFlags flags, TileIndex tile, TileIndex area_start, RailType totype, bool diagonal)
{
	TileIndex area_end = tile;

	if (!ValParamRailType(totype)) return CMD_ERROR;
	if (area_start >= Map::Size()) return CMD_ERROR;

	TrainList affected_trains;

	CommandCost cost(ExpensesType::Construction);
	CommandCost error = CommandCost(STR_ERROR_NO_SUITABLE_RAILROAD_TRACK); // by default, there is no track to convert.
	bool found_convertible_track = false; // whether we actually did convert some track (see bug #7633)

	std::unique_ptr<TileIterator> iter = TileIterator::Create(area_start, area_end, diagonal);
	for (; (tile = *iter) != INVALID_TILE; ++(*iter)) {
		TileType tt = GetTileType(tile);

		/* Check if there is any track on tile */
		switch (tt) {
			case TileType::Railway:
				break;
			case TileType::Station:
				if (!HasStationRail(tile)) continue;
				break;
			case TileType::Road:
				if (!IsLevelCrossing(tile)) continue;
				if (RailNoLevelCrossings(totype)) {
					error.MakeError(STR_ERROR_CROSSING_DISALLOWED_RAIL);
					continue;
				}
				break;
			case TileType::TunnelBridge:
				if (GetTunnelBridgeTransportType(tile) != TransportType::Rail) continue;
				break;
			default: continue;
		}

		/* Original railtype we are converting from */
		RailType type = GetRailType(tile);

		/* Converting to the same type or converting 'hidden' elrail -> rail */
		if (type == totype || (_settings_game.vehicle.disable_elrails && totype == RAILTYPE_RAIL && type == RAILTYPE_ELECTRIC)) continue;

		/* Trying to convert other's rail */
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) {
			error = std::move(ret);
			continue;
		}

		std::vector<Train *> vehicles_affected;

		/* Vehicle on the tile when not converting Rail <-> ElRail
		 * Tunnels and bridges have special check later */
		if (tt != TileType::TunnelBridge) {
			if (!IsCompatibleRail(type, totype)) {
				ret = IsPlainRailTile(tile) ? EnsureNoTrainOnTrackBits(tile, GetTrackBits(tile)) : EnsureNoVehicleOnGround(tile);
				if (ret.Failed()) {
					error = std::move(ret);
					continue;
				}
			}
			if (flags.Test(DoCommandFlag::Execute)) { // we can safely convert, too
				for (Track track : GetReservedTrackbits(tile)) {
					Train *v = GetTrainForReservation(tile, track);
					if (v != nullptr && !HasPowerOnRail(v->railtypes, totype)) {
						/* No power on new rail type, reroute. */
						FreeTrainTrackReservation(v);
						vehicles_affected.push_back(v);
					}
				}

				/* Update the company infrastructure counters. */
				if (!IsRailStationTile(tile) || !IsStationTileBlocked(tile)) {
					Company *c = Company::Get(GetTileOwner(tile));
					uint num_pieces = IsLevelCrossingTile(tile) ? LEVELCROSSING_TRACKBIT_FACTOR : 1;
					if (IsPlainRailTile(tile)) {
						TrackBits bits = GetTrackBits(tile);
						num_pieces = bits.Count();
						if (TracksOverlap(bits)) num_pieces *= num_pieces;
					}
					c->infrastructure.rail[type] -= num_pieces;
					c->infrastructure.rail[totype] += num_pieces;
					DirtyCompanyInfrastructureWindows(c->index);
				}

				SetRailType(tile, totype);
				MarkTileDirtyByTile(tile);
				/* update power of train on this tile */
				for (Vehicle *v : VehiclesOnTile(tile)) {
					if (v->type == VehicleType::Train) include(affected_trains, Train::From(v)->First());
				}
			}
		}

		switch (tt) {
			default: NOT_REACHED();
			case TileType::Railway:
				switch (GetRailTileType(tile)) {
					case RailTileType::Depot:
						if (flags.Test(DoCommandFlag::Execute)) {
							/* notify YAPF about the track layout change */
							YapfNotifyTrackLayoutChange(tile, GetRailDepotTrack(tile));

							/* Update build vehicle window related to this depot */
							InvalidateWindowData(WindowClass::VehicleDepot, tile);
							InvalidateWindowData(WindowClass::BuildVehicle, tile);
						}
						found_convertible_track = true;
						cost.AddCost(RailConvertCost(type, totype));
						break;

					default: // RailTileType::Normal, RailTileType::Signals
						if (flags.Test(DoCommandFlag::Execute)) {
							/* notify YAPF about the track layout change */
							for (Track track : GetTrackBits(tile)) {
								YapfNotifyTrackLayoutChange(tile, track);
							}
						}
						found_convertible_track = true;
						cost.AddCost(RailConvertCost(type, totype) * GetTrackBits(tile).Count());
						break;
				}
				break;

			case TileType::TunnelBridge: {
				TileIndex endtile = GetOtherTunnelBridgeEnd(tile);

				/* If both ends of tunnel/bridge are in the range, do not try to convert twice -
				 * it would cause assert because of different test and exec runs */
				if (endtile < tile) {
					if (diagonal) {
						if (DiagonalTileArea(area_start, area_end).Contains(endtile)) continue;
					} else {
						if (OrthogonalTileArea(area_start, area_end).Contains(endtile)) continue;
					}
				}

				/* When not converting rail <-> el. rail, any vehicle cannot be in tunnel/bridge */
				if (!IsCompatibleRail(GetRailType(tile), totype)) {
					ret = TunnelBridgeIsFree(tile, endtile);
					if (ret.Failed()) {
						error = std::move(ret);
						continue;
					}
				}

				if (flags.Test(DoCommandFlag::Execute)) {
					Track track = DiagDirToDiagTrack(GetTunnelBridgeDirection(tile));
					if (HasTunnelBridgeReservation(tile)) {
						Train *v = GetTrainForReservation(tile, track);
						if (v != nullptr && !HasPowerOnRail(v->railtypes, totype)) {
							/* No power on new rail type, reroute. */
							FreeTrainTrackReservation(v);
							vehicles_affected.push_back(v);
						}
					}

					/* Update the company infrastructure counters. */
					uint num_pieces = (GetTunnelBridgeLength(tile, endtile) + 2) * TUNNELBRIDGE_TRACKBIT_FACTOR;
					Company *c = Company::Get(GetTileOwner(tile));
					c->infrastructure.rail[GetRailType(tile)] -= num_pieces;
					c->infrastructure.rail[totype] += num_pieces;
					DirtyCompanyInfrastructureWindows(c->index);

					SetRailType(tile, totype);
					SetRailType(endtile, totype);

					for (Vehicle *v : VehiclesOnTile(tile)) {
						if (v->type == VehicleType::Train) include(affected_trains, Train::From(v)->First());
					}
					for (Vehicle *v : VehiclesOnTile(endtile)) {
						if (v->type == VehicleType::Train) include(affected_trains, Train::From(v)->First());
					}

					YapfNotifyTrackLayoutChange(tile, track);
					YapfNotifyTrackLayoutChange(endtile, track);

					if (IsBridge(tile)) {
						MarkBridgeDirty(tile);
					} else {
						MarkTileDirtyByTile(tile);
						MarkTileDirtyByTile(endtile);
					}
				}

				found_convertible_track = true;
				cost.AddCost((GetTunnelBridgeLength(tile, endtile) + 2) * RailConvertCost(type, totype));
				break;
			}

			case TileType::Station:
			case TileType::Road:
				if (flags.Test(DoCommandFlag::Execute)) {
					Track track = ((tt == TileType::Station) ? GetRailStationTrack(tile) : GetCrossingRailTrack(tile));
					YapfNotifyTrackLayoutChange(tile, track);
				}

				found_convertible_track = true;
				cost.AddCost(RailConvertCost(type, totype));
				break;
		}

		for (uint i = 0; i < vehicles_affected.size(); ++i) {
			TryPathReserve(vehicles_affected[i], true);
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Railtype changed, update trains as when entering different track */
		for (Train *v : affected_trains) {
			v->ConsistChanged(CCF_TRACK);
		}
	}

	return found_convertible_track ? cost : error;
}

static CommandCost RemoveTrainDepot(TileIndex tile, DoCommandFlags flags)
{
	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;
	}

	CommandCost ret = EnsureNoVehicleOnGround(tile);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		/* read variables before the depot is removed */
		DiagDirection dir = GetRailDepotDirection(tile);
		Owner owner = GetTileOwner(tile);
		Train *v = nullptr;

		if (HasDepotReservation(tile)) {
			v = GetTrainForReservation(tile, DiagDirToDiagTrack(dir));
			if (v != nullptr) FreeTrainTrackReservation(v);
		}

		Company::Get(owner)->infrastructure.rail[GetRailType(tile)]--;
		DirtyCompanyInfrastructureWindows(owner);

		delete Depot::GetByTile(tile);
		DoClearSquare(tile);
		AddSideToSignalBuffer(tile, dir, owner);
		YapfNotifyTrackLayoutChange(tile, DiagDirToDiagTrack(dir));
		if (v != nullptr) TryPathReserve(v, true);
	}

	return CommandCost(ExpensesType::Construction, _price[Price::ClearDepotTrain]);
}

/** @copydoc ClearTileProc */
static CommandCost ClearTile_Rail(TileIndex tile, DoCommandFlags flags)
{
	CommandCost cost(ExpensesType::Construction);

	if (flags.Test(DoCommandFlag::Auto)) {
		if (!IsTileOwner(tile, _current_company)) {
			return CommandCost(STR_ERROR_AREA_IS_OWNED_BY_ANOTHER);
		}

		if (IsPlainRail(tile)) {
			return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);
		} else {
			return CommandCost(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
		}
	}

	switch (GetRailTileType(tile)) {
		case RailTileType::Signals:
		case RailTileType::Normal: {
			Slope tileh = GetTileSlope(tile);
			/* Is there flat water on the lower halftile that gets cleared expensively? */
			bool water_ground = (GetRailGroundType(tile) == RailGroundType::HalfTileWater && IsSlopeWithOneCornerRaised(tileh));

			for (Track track : GetTrackBits(tile)) {
				CommandCost ret = Command<Commands::RemoveRail>::Do(flags, tile, track);
				if (ret.Failed()) return ret;
				cost.AddCost(ret.GetCost());
			}

			/* When bankrupting, don't make water dirty, there could be a ship on lower halftile.
			 * Same holds for non-companies clearing the tile, e.g. disasters. */
			if (water_ground && !flags.Test(DoCommandFlag::Bankrupt) && Company::IsValidID(_current_company)) {
				CommandCost ret = EnsureNoVehicleOnGround(tile);
				if (ret.Failed()) return ret;

				/* The track was removed, and left a coast tile. Now also clear the water. */
				if (flags.Test(DoCommandFlag::Execute)) {
					DoClearSquare(tile);
				}
				cost.AddCost(_price[Price::ClearWater]);
			}

			return cost;
		}

		case RailTileType::Depot:
			return RemoveTrainDepot(tile, flags);

		default:
			return CMD_ERROR;
	}
}

/**
 * Get surface height in point (x,y)
 * On tiles with halftile foundations move (x,y) to a safe point wrt. track
 * @param x The world X-coordinate.
 * @param y The world Y-coordinate.
 * @param track The track to get the height for.
 * @return The world Z-coordinate.
 */
static uint GetSafeSlopeZ(uint x, uint y, Track track)
{
	switch (track) {
		case Track::Upper: x &= ~0xF; y &= ~0xF; break;
		case Track::Lower: x |=  0xF; y |=  0xF; break;
		case Track::Left:  x |=  0xF; y &= ~0xF; break;
		case Track::Right: x &= ~0xF; y |=  0xF; break;
		default: break;
	}
	return GetSlopePixelZ(x, y);
}

/**
 * Helper to determine whether the train signals are to be placed on the right side.
 * @return \c true iff signals are to be drawn on the right side.
 */
static bool IsTrainSignalSideRight()
{
	switch (_settings_game.construction.train_signal_side) {
		case TrainSignalSide::Left:
			return false;
		case TrainSignalSide::Right:
			return true;
		case TrainSignalSide::RoadVehicleDrivingSide:
			return _settings_game.vehicle.road_side == RoadVehicleDrivingSide::Right;
		default:
			NOT_REACHED();
	}
}

/**
 * Whether a path signal should show the warning aspect instead of a plain
 * green: the next signal along the line is at danger, so a train may pass this
 * one but is going to have to stop at that one and ought to be braking
 * already. Green keeps its old meaning, the way is clear.
 *
 * What it is not: "the road booked through me ends here". A road is only ever
 * booked as far ahead as a train needs (see CheckNextTrainTile()), so its end
 * is where the look-ahead stopped and not something in the way -- read that
 * way, every signal a few tiles in front of a train warned on a clear line,
 * which is no warning at all.
 *
 * This is the aspect trains have been driving to since gentle braking came in
 * -- a train looks a braking distance ahead and slows for the end of its road
 * (see BrakingCeiling() and CheckNextTrainTile() in train_cmd.cpp) -- only
 * now it is on the signal as well, so a player can see why a train is slowing
 * where the line looks clear.
 *
 * Read off the map at drawing time and nowhere stored: it is the state of the
 * road beyond, which changes as trains book and give back track, and there is
 * no room on the map for it (and no reason to make room, since it is only a
 * picture). Nothing here changes anything.
 *
 * @param tile the signal's tile
 * @param td the trackdir the signal faces along
 * @return true to draw the warning aspect
 */
bool IsPathSignalWarning(TileIndex tile, Trackdir td)
{
	/* Any kind of signal, not only a path one. "The next signal along is at
	 * danger" is what a distant signal has always said, and the plain block
	 * signal is what most track is laid with -- it is the one the aspect is
	 * worth the most on. */
	/* Asked about a bore's mouth as well, which carries a signal of its own
	 * but is not a rail tile: asking one of those about a signal puts the game
	 * down (the headless rig draws, and the battery caught it). The kind that
	 * guards itself was what stood here before; this one does not. */
	if (!IsTileType(tile, TileType::Railway) || !HasSignalOnTrackdir(tile, td)) return false;
	if (GetSignalStateByTrackdir(tile, td) != SignalState::Green) return false;

	/* How far to look for the next signal. A signal further off than this
	 * says nothing useful to a driver at this one, and the walk has to end
	 * somewhere: it runs every time the signal is drawn. */
	constexpr int MAX_TILES = 24;

	/* How many signals before a red show the warning: the player's setting
	 * (vehicle.train_warning_signals). Each of them is looked for no further
	 * than MAX_TILES from the one before. */
	int signals_left = WarningSignalCount();

	CFollowTrackRail ft(GetTileOwner(tile), GetRailTypeInfo(GetRailType(tile))->compatible_railtypes);
	TileIndex cur = tile;
	Trackdir cur_td = td;
	for (int i = 0; i < MAX_TILES; i++) {
		if (!ft.Follow(cur, cur_td)) return false; // the line ends: nothing ahead shows anything
		/* Which way on: the booked road where there is one, otherwise the
		 * only way there is. At a junction with nothing booked, where a train
		 * would go is not settled, and a signal down one of the ways says
		 * nothing about the other. */
		TrackdirBits reserved = ft.new_td_bits & TrackBitsToTrackdirBits(GetReservedTrackbits(ft.new_tile));
		TrackdirBits onward = reserved.Any() ? reserved : ft.new_td_bits;
		if (onward.Count() != 1) return false;
		cur = ft.new_tile;
		cur_td = FindFirstTrackdir(onward);
		/* Only plain rail carries signals, and asking anything else about one
		 * puts the game down: the line runs through platforms, sheds, tunnels
		 * and bridges as well (measured -- the rig draws, so the battery
		 * caught it on the station scenes). */
		if (IsTileType(cur, TileType::Railway) && HasSignalOnTrackdir(cur, cur_td)) {
			if (GetSignalStateByTrackdir(cur, cur_td) == SignalState::Red) return true;
			if (--signals_left == 0) return false;
			i = -1; // the next signal is looked for from this one
		}
	}
	return false;
}

/**
 * How many signals before a red show the warning aspect, and so how many
 * signals ahead a driver knows the state of: the player's setting
 * (vehicle.train_warning_signals), 1 to 3.
 */
int WarningSignalCount()
{
	return Clamp<int>(_settings_game.vehicle.train_warning_signals, 1, 3);
}

/** Our Czech light signal's top over its foot, metres (objekty/nase/navestidlo.py): its box on the map. */
static constexpr double THREE_D_SIGNAL_TOP_M = 5.3;

/**
 * Our Czech light signal (the player, 8. 11.: "the lights one Czech", "2 yes"; objekty/nase/navestidlo.py) at a signal on
 * our 3D track, of whatever kind and variant, instead of its sprite: at its place beside the track, its lights to the
 * trains it stands for (against its trackdir), lit as it shows -- red stop, green clear, the top yellow the warning.
 * @param td the trackdir the signal is for
 * @param x,y,z where it stands, world units and units of height
 * @return whether it was drawn so: no sprite of the game's
 */
static bool DrawThreeDSignal(TileIndex tile, Trackdir td, SignalAspect aspect, int x, int y, int z)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return false;
	if (!ThreeDTrackShownOn(tile) || GetThreeDFigure("cz_navestidlo") == nullptr) return false;
	/* The way a train goes along each trackdir, world units: NE, SE, E, E, S, S, -, -, SW, NW, W, W, N, N, -, -. */
	static constexpr std::array<std::array<int8_t, 2>, 16> GOES{{
		{{-1, 0}}, {{0, 1}}, {{-1, 1}}, {{-1, 1}}, {{1, 1}}, {{1, 1}}, {{0, 0}}, {{0, 0}},
		{{1, 0}}, {{0, -1}}, {{1, -1}}, {{1, -1}}, {{-1, -1}}, {{-1, -1}}, {{0, 0}}, {{0, 0}},
	}};
	const auto &g = GOES[to_underlying(td) & 15];
	const float facing = static_cast<float>(std::atan2(-g[1], -g[0]) * 180.0 / M_PI);
	const std::string_view name = aspect == SignalAspect::Red ? "cz_navestidlo_cervena" : aspect == SignalAspect::Warning ? "cz_navestidlo_zluta" : "cz_navestidlo_zelena";
	if (eye) {
		AddCabFigure(name, 1.0f, x, y, z, facing);
		return true;
	}
	const std::array<int, 6> box{x - 1, y - 1, z, x, y, z + static_cast<int>(std::lround(THREE_D_SIGNAL_TOP_M * OBJECT_HEIGHT_PER_METRE))};
	AddThreeDFigureToViewportInBox(name, 1.0f, x, y, z, facing, _cur_dpi->zoom, tile, 0, -1, 0, &box);
	return true;
}

/**
 * Draw one signal, of a kind and variant said outright rather than read off
 * the tile: a tunnel or bridge portal carries a signal too and keeps it
 * nowhere the rail map can be asked about it.
 */
static void DrawSignalSprite(TileIndex tile, const RailTypeInfo *rti, Track track, SignalType type, SignalVariant variant, SignalState condition, SignalOffsets image, uint pos, Trackdir td)
{
	static const Point SignalPositions[2][12] = {
		{ // Signals on the left side
		/*  LEFT      LEFT      RIGHT     RIGHT     UPPER     UPPER */
			{ 8,  5}, {14,  1}, { 1, 14}, { 9, 11}, { 1,  0}, { 3, 10},
		/*  LOWER     LOWER     X         X         Y         Y     */
			{11,  4}, {14, 14}, {11,  3}, { 4, 13}, { 3,  4}, {11, 13}
		}, { // Signals on the right side
		/*  LEFT      LEFT      RIGHT     RIGHT     UPPER     UPPER */
			{14,  1}, {12, 10}, { 4,  6}, { 1, 14}, {10,  4}, { 0,  1},
		/*  LOWER     LOWER     X         X         Y         Y     */
			{14, 14}, { 5, 12}, {11, 13}, { 4,  3}, {13,  4}, { 3, 11}
		}
	};

	bool signal_on_right = IsTrainSignalSideRight();
	int x = TileX(tile) * TILE_SIZE + SignalPositions[signal_on_right][pos].x;
	int y = TileY(tile) * TILE_SIZE + SignalPositions[signal_on_right][pos].y;
	/* By a round curve: by its arc (round_track.h). */
	if (RoundTrackDrawing() && IsPlainRailTile(tile)) RoundTrackSignalAt(tile, track, x, y);

	/* Which of the three this signal is showing. The warning is worked out
	 * from the road ahead; every kind of signal can show it, a block signal
	 * included -- "the next one along is at danger" is what a distant signal
	 * has always said, and the block signal is the one most track is laid
	 * with. */
	SignalAspect aspect = condition == SignalState::Red ? SignalAspect::Red : SignalAspect::Green;
	if (aspect == SignalAspect::Green && IsPathSignalWarning(tile, td)) aspect = SignalAspect::Warning;

	/* On our 3D track our Czech light signal (DrawThreeDSignal()). */
	if (DrawThreeDSignal(tile, td, aspect, x, y, GetSafeSlopeZ(x, y, track))) return;

	/* The rail type's own signals first, as the patchpack orders it: a set
	 * that draws signals for one kind of track means them for that track. A
	 * set that draws signal styles means them for everything, so it is asked
	 * next; and the base set answers last. */
	PaletteID pal = PAL_NONE;
	SpriteID sprite = GetCustomSignalSprite(rti, tile, type, variant, aspect);
	if (sprite == 0) sprite = GetCustomSignalStyleSprite(GetSignalStyleInUse(), rti, tile, type, variant, aspect, false);

	if (sprite == 0 && aspect == SignalAspect::Warning) {
		/* Nothing drew the warning -- the set has two aspects only, or none of
		 * its own at all. Then it is that signal's own green with the lamp
		 * repainted, which is a picture of the right signal whoever drew it. */
		pal = PALETTE_SIGNAL_WARNING;
		sprite = GetCustomSignalSprite(rti, tile, type, variant, SignalAspect::Green);
		if (sprite == 0) sprite = GetCustomSignalStyleSprite(GetSignalStyleInUse(), rti, tile, type, variant, SignalAspect::Green, false);
	}

	if (sprite != 0) {
		sprite += image;
	} else {
		/* Normal electric signals are stored in a different sprite block than all other signals. */
		sprite = (type == SignalType::Block && variant == SignalVariant::Electric) ? SPR_ORIGINAL_SIGNALS_BASE : SPR_SIGNALS_BASE - 16;
		/* The warning is the green one repainted, so it is the green that is
		 * asked for here. */
		SignalState shown = aspect == SignalAspect::Warning ? SignalState::Green : condition;
		sprite += to_underlying(type) * 16 + to_underlying(variant) * 64 + image * 2 + to_underlying(shown) + (type >= SignalType::Path ? 64 : 0);
	}

	AddSortableSpriteToDraw(sprite, pal, x, y, GetSafeSlopeZ(x, y, track), {{}, {1, 1, BB_HEIGHT_UNDER_BRIDGE}, {}});
}

/** Offsets for drawing fences */
struct FenceOffset : SpriteBounds {
	Corner height_ref; ///< Corner to use height offset from.

	constexpr FenceOffset(Corner height_ref, int8_t origin_x, int8_t origin_y, uint8_t extent_x, uint8_t extent_y) :
		SpriteBounds({origin_x, origin_y, 0}, {extent_x, extent_y, 4}, {}), height_ref(height_ref) {}
};

/** Offsets for drawing fences */
static const FenceOffset _fence_offsets[] = {
	{ CORNER_INVALID,  0,  1, 16,  1 }, // RFO_FLAT_X_NW
	{ CORNER_INVALID,  1,  0,  1, 16 }, // RFO_FLAT_Y_NE
	{ CORNER_W,        8,  8,  1,  1 }, // RFO_FLAT_LEFT
	{ CORNER_N,        8,  8,  1,  1 }, // RFO_FLAT_UPPER
	{ CORNER_INVALID,  0,  1, 16,  1 }, // RFO_SLOPE_SW_NW
	{ CORNER_INVALID,  1,  0,  1, 16 }, // RFO_SLOPE_SE_NE
	{ CORNER_INVALID,  0,  1, 16,  1 }, // RFO_SLOPE_NE_NW
	{ CORNER_INVALID,  1,  0,  1, 16 }, // RFO_SLOPE_NW_NE
	{ CORNER_INVALID,  0, 15, 16,  1 }, // RFO_FLAT_X_SE
	{ CORNER_INVALID, 15,  0,  1, 16 }, // RFO_FLAT_Y_SW
	{ CORNER_E,        8,  8,  1,  1 }, // RFO_FLAT_RIGHT
	{ CORNER_S,        8,  8,  1,  1 }, // RFO_FLAT_LOWER
	{ CORNER_INVALID,  0, 15, 16,  1 }, // RFO_SLOPE_SW_SE
	{ CORNER_INVALID, 15,  0,  1, 16 }, // RFO_SLOPE_SE_SW
	{ CORNER_INVALID,  0, 15, 16,  1 }, // RFO_SLOPE_NE_SE
	{ CORNER_INVALID, 15,  0,  1, 16 }, // RFO_SLOPE_NW_SW
};

/**
 * Draw a track fence.
 * @param ti Tile drawing information.
 * @param psid First fence sprite and palette.
 * @param num_sprites Number of fence sprites.
 * @param rfo Fence to draw.
 */
static void DrawTrackFence(const TileInfo *ti, const PalSpriteID &psid, uint num_sprites, RailFenceOffset rfo)
{
	int z = ti->z;
	if (IsValidCorner(_fence_offsets[rfo].height_ref)) {
		z += GetSlopePixelZInCorner(RemoveHalftileSlope(ti->tileh), _fence_offsets[rfo].height_ref);
	}
	AddSortableSpriteToDraw(psid.sprite + (rfo % num_sprites), psid.pal, ti->x, ti->y, z, _fence_offsets[rfo]);
}

/**
 * Draw fence at NW border matching the tile slope.
 * @param ti Tile drawing information.
 * @param psid First fence sprite and palette.
 * @param num_sprites Number of fence sprites.
 */
static void DrawTrackFence_NW(const TileInfo *ti, const PalSpriteID &psid, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_X_NW;
	if (ti->tileh & SLOPE_NW) rfo = (ti->tileh & SLOPE_W) ? RFO_SLOPE_SW_NW : RFO_SLOPE_NE_NW;
	DrawTrackFence(ti, psid, num_sprites, rfo);
}

/**
 * Draw fence at SE border matching the tile slope.
 * @param ti Tile drawing information.
 * @param psid First fence sprite and palette.
 * @param num_sprites Number of fence sprites.
 */
static void DrawTrackFence_SE(const TileInfo *ti, const PalSpriteID &psid, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_X_SE;
	if (ti->tileh & SLOPE_SE) rfo = (ti->tileh & SLOPE_S) ? RFO_SLOPE_SW_SE : RFO_SLOPE_NE_SE;
	DrawTrackFence(ti, psid, num_sprites, rfo);
}

/**
 * Draw fence at NE border matching the tile slope.
 * @param ti Tile drawing information.
 * @param psid First fence sprite and palette.
 * @param num_sprites Number of fence sprites.
 */
static void DrawTrackFence_NE(const TileInfo *ti, const PalSpriteID &psid, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_Y_NE;
	if (ti->tileh & SLOPE_NE) rfo = (ti->tileh & SLOPE_E) ? RFO_SLOPE_SE_NE : RFO_SLOPE_NW_NE;
	DrawTrackFence(ti, psid, num_sprites, rfo);
}

/**
 * Draw fence at SW border matching the tile slope.
 * @param ti Tile drawing information.
 * @param psid First fence sprite and palette.
 * @param num_sprites Number of fence sprites.
 */
static void DrawTrackFence_SW(const TileInfo *ti, const PalSpriteID &psid, uint num_sprites)
{
	RailFenceOffset rfo = RFO_FLAT_Y_SW;
	if (ti->tileh & SLOPE_SW) rfo = (ti->tileh & SLOPE_S) ? RFO_SLOPE_SE_SW : RFO_SLOPE_NW_SW;
	DrawTrackFence(ti, psid, num_sprites, rfo);
}

/**
 * Draw track fences.
 * @param ti Tile drawing information.
 * @param rti Rail type information.
 * @param pal The palette to apply while drawing.
 */
static void DrawTrackDetails(const TileInfo *ti, const RailTypeInfo *rti, PaletteID pal)
{
	/* A player who does not want fences beside his tracks says so in the
	 * settings, rather than having to find a graphics set that draws them as
	 * nothing. Only the picture is left out: the tile goes on recording where a
	 * fence belongs (see TileLoop_Track()), so the fences are all back the
	 * moment this is turned on again, and a game played with them off is the
	 * same game as one played with them on. */
	extern bool _draw_fences;
	if (!_settings_client.gui.show_rail_fences || !_draw_fences) return;

	/* Base sprite for track fences.
	 * Note: Halftile slopes only have fences on the upper part. */
	uint num_sprites = 0;
	PalSpriteID psid{
		.sprite = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Fences, IsHalftileSlope(ti->tileh) ? TileContext::UpperHalftile : TileContext::Normal, &num_sprites),
		.pal = pal,
	};
	if (psid.sprite == 0) {
		psid.sprite = SPR_TRACK_FENCE_FLAT_X;
		num_sprites = 8;
	}

	assert(num_sprites > 0);

	switch (GetRailGroundType(ti->tile)) {
		case RailGroundType::FenceNW:     DrawTrackFence_NW(ti, psid, num_sprites); break;
		case RailGroundType::FenceSE:     DrawTrackFence_SE(ti, psid, num_sprites); break;
		case RailGroundType::FenceSENW:   DrawTrackFence_NW(ti, psid, num_sprites);
		                                  DrawTrackFence_SE(ti, psid, num_sprites); break;
		case RailGroundType::FenceNE:     DrawTrackFence_NE(ti, psid, num_sprites); break;
		case RailGroundType::FenceSW:     DrawTrackFence_SW(ti, psid, num_sprites); break;
		case RailGroundType::FenceNESW:   DrawTrackFence_NE(ti, psid, num_sprites);
		                                  DrawTrackFence_SW(ti, psid, num_sprites); break;
		case RailGroundType::FenceVert1:  DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_LEFT);  break;
		case RailGroundType::FenceVert2:  DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_RIGHT); break;
		case RailGroundType::FenceHoriz1: DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_UPPER); break;
		case RailGroundType::FenceHoriz2: DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_LOWER); break;
		case RailGroundType::HalfTileWater: {
			Corner track_corner;
			if (IsHalftileSlope(ti->tileh)) {
				/* Steep slope or one-corner-raised slope with halftile foundation */
				track_corner = GetHalftileSlopeCorner(ti->tileh);
			} else {
				/* Three-corner-raised slope */
				track_corner = OppositeCorner(GetHighestSlopeCorner(ComplementSlope(ti->tileh)));
			}
			switch (track_corner) {
				case CORNER_W: DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_LEFT);  break;
				case CORNER_S: DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_LOWER); break;
				case CORNER_E: DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_RIGHT); break;
				case CORNER_N: DrawTrackFence(ti, psid, num_sprites, RFO_FLAT_UPPER); break;
				default: NOT_REACHED();
			}
			break;
		}
		default: break;
	}
}

static const int INF = 1000; ///< Big number compared to tilesprite size.
/** SubSprite for drawing the track halftile of 'three-corners-raised'-sloped rail sprites. */
static constexpr CornerIndexArray<SubSprite> _halftile_sub_sprite = {{{
	{ -INF    , -INF  , 32 - 33, INF     }, // CORNER_W, clip 33 pixels from right
	{ -INF    ,  0 + 7, INF    , INF     }, // CORNER_S, clip 7 pixels from top
	{ -31 + 33, -INF  , INF    , INF     }, // CORNER_E, clip 33 pixels from left
	{ -INF    , -INF  , INF    , 30 - 23 }  // CORNER_N, clip 23 pixels from bottom
}}};

static inline void DrawTrackSprite(SpriteID sprite, PaletteID pal, const TileInfo *ti, Slope s)
{
	DrawGroundSprite(sprite, pal, nullptr, 0, (ti->tileh & s) ? -8 : 0);
}

static void DrawTrackBitsOverlay(TileInfo *ti, TrackBits track, const RailTypeInfo *rti)
{
	RailGroundType rgt = GetRailGroundType(ti->tile);
	Foundation f = GetRailFoundation(ti->tileh, track);
	Corner halftile_corner = CORNER_INVALID;

	if (IsNonContinuousFoundation(f)) {
		/* Save halftile corner */
		halftile_corner = (f == Foundation::SteepBoth ? GetHighestSlopeCorner(ti->tileh) : GetHalftileFoundationCorner(f));
		/* Draw lower part first */
		track.Reset(CornerToTrackBits(halftile_corner));
		f = (f == Foundation::SteepBoth ? Foundation::SteepLower : Foundation::None);
	}

	DrawFoundation(ti, f);
	/* DrawFoundation modifies ti */

	/* Draw ground */
	if (rgt == RailGroundType::HalfTileWater) {
		if (track.Any() || IsSteepSlope(ti->tileh)) {
			/* three-corner-raised slope or steep slope with track on upper part */
			DrawShoreTile(ti->tileh);
		} else {
			/* single-corner-raised slope with track on upper part */
			DrawGroundSprite(SPR_FLAT_WATER_TILE, PAL_NONE);
		}
	} else {
		SpriteID image;

		switch (rgt) {
			case RailGroundType::Barren: image = SPR_FLAT_BARE_LAND; break;
			case RailGroundType::SnowOrDesert: image = SPR_FLAT_SNOW_DESERT_TILE; break;
			default: image = SPR_FLAT_GRASS_TILE; break;
		}

		image += SlopeToSpriteOffset(ti->tileh);

		DrawGroundSprite(image, PAL_NONE);
	}

	bool no_combine = ti->tileh == SLOPE_FLAT && rti->flags.Test(RailTypeFlag::NoSpriteCombine);
	SpriteID overlay = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Overlay);
	SpriteID ground = GetCustomRailSprite(rti, ti->tile, no_combine ? RailSpriteType::GroundComplete : RailSpriteType::Ground);
	TrackBits pbs = _settings_client.gui.show_track_reservation ? GetRailReservationTrackBits(ti->tile) : TrackBits{};

	if (track.None()) {
		/* Half-tile foundation, no track here? */
	} else if (no_combine) {
		/* Use trackbits as direct index from ground sprite, subtract 1
		 * because there is no sprite for no bits. */
		DrawGroundSprite(ground + track.base() - 1, PAL_NONE);

		/* Draw reserved track bits */
		if (pbs.Test(Track::X))     DrawGroundSprite(overlay + RTO_X, PALETTE_CRASH);
		if (pbs.Test(Track::Y))     DrawGroundSprite(overlay + RTO_Y, PALETTE_CRASH);
		if (pbs.Test(Track::Upper)) DrawTrackSprite(overlay + RTO_N, PALETTE_CRASH, ti, SLOPE_N);
		if (pbs.Test(Track::Lower)) DrawTrackSprite(overlay + RTO_S, PALETTE_CRASH, ti, SLOPE_S);
		if (pbs.Test(Track::Right)) DrawTrackSprite(overlay + RTO_E, PALETTE_CRASH, ti, SLOPE_E);
		if (pbs.Test(Track::Left))  DrawTrackSprite(overlay + RTO_W, PALETTE_CRASH, ti, SLOPE_W);
	} else if (ti->tileh == SLOPE_NW && track == Track::Y) {
		DrawGroundSprite(ground + RTO_SLOPE_NW, PAL_NONE);
		if (pbs.Any()) DrawGroundSprite(overlay + RTO_SLOPE_NW, PALETTE_CRASH);
	} else if (ti->tileh == SLOPE_NE && track == Track::X) {
		DrawGroundSprite(ground + RTO_SLOPE_NE, PAL_NONE);
		if (pbs.Any()) DrawGroundSprite(overlay + RTO_SLOPE_NE, PALETTE_CRASH);
	} else if (ti->tileh == SLOPE_SE && track == Track::Y) {
		DrawGroundSprite(ground + RTO_SLOPE_SE, PAL_NONE);
		if (pbs.Any()) DrawGroundSprite(overlay + RTO_SLOPE_SE, PALETTE_CRASH);
	} else if (ti->tileh == SLOPE_SW && track == Track::X) {
		DrawGroundSprite(ground + RTO_SLOPE_SW, PAL_NONE);
		if (pbs.Any()) DrawGroundSprite(overlay + RTO_SLOPE_SW, PALETTE_CRASH);
	} else {
		switch (track.base()) {
			/* Draw single ground sprite when not overlapping. No track overlay
			 * is necessary for these sprites. */
			case TrackBits{Track::X}.base(): DrawGroundSprite(ground + RTO_X, PAL_NONE); break;
			case TrackBits{Track::Y}.base(): DrawGroundSprite(ground + RTO_Y, PAL_NONE); break;
			case TrackBits{Track::Upper}.base(): DrawTrackSprite(ground + RTO_N, PAL_NONE, ti, SLOPE_N); break;
			case TrackBits{Track::Lower}.base(): DrawTrackSprite(ground + RTO_S, PAL_NONE, ti, SLOPE_S); break;
			case TrackBits{Track::Right}.base(): DrawTrackSprite(ground + RTO_E, PAL_NONE, ti, SLOPE_E); break;
			case TrackBits{Track::Left}.base(): DrawTrackSprite(ground + RTO_W, PAL_NONE, ti, SLOPE_W); break;
			case TRACK_BIT_CROSS.base(): DrawGroundSprite(ground + RTO_CROSSING_XY, PAL_NONE); break;
			case TRACK_BIT_HORZ.base():
				DrawTrackSprite(ground + RTO_N, PAL_NONE, ti, SLOPE_N);
			    DrawTrackSprite(ground + RTO_S, PAL_NONE, ti, SLOPE_S);
				break;
			case TRACK_BIT_VERT.base():
				DrawTrackSprite(ground + RTO_E, PAL_NONE, ti, SLOPE_E);
			    DrawTrackSprite(ground + RTO_W, PAL_NONE, ti, SLOPE_W);
				break;

			default:
				/* We're drawing a junction tile */
				if (!track.Any(TRACK_BIT_3WAY_NE)) {
					DrawGroundSprite(ground + RTO_JUNCTION_SW, PAL_NONE);
				} else if (!track.Any(TRACK_BIT_3WAY_SW)) {
					DrawGroundSprite(ground + RTO_JUNCTION_NE, PAL_NONE);
				} else if (!track.Any(TRACK_BIT_3WAY_NW)) {
					DrawGroundSprite(ground + RTO_JUNCTION_SE, PAL_NONE);
				} else if (!track.Any(TRACK_BIT_3WAY_SE)) {
					DrawGroundSprite(ground + RTO_JUNCTION_NW, PAL_NONE);
				} else {
					DrawGroundSprite(ground + RTO_JUNCTION_NSEW, PAL_NONE);
				}

				/* Mask out PBS bits as we shall draw them afterwards anyway. */
				track.Reset(pbs);

				/* Draw regular track bits */
				if (track.Test(Track::X))     DrawGroundSprite(overlay + RTO_X, PAL_NONE);
				if (track.Test(Track::Y))     DrawGroundSprite(overlay + RTO_Y, PAL_NONE);
				if (track.Test(Track::Upper)) DrawGroundSprite(overlay + RTO_N, PAL_NONE);
				if (track.Test(Track::Lower)) DrawGroundSprite(overlay + RTO_S, PAL_NONE);
				if (track.Test(Track::Right)) DrawGroundSprite(overlay + RTO_E, PAL_NONE);
				if (track.Test(Track::Left))  DrawGroundSprite(overlay + RTO_W, PAL_NONE);
		}

		/* Draw reserved track bits */
		if (pbs.Test(Track::X))     DrawGroundSprite(overlay + RTO_X, PALETTE_CRASH);
		if (pbs.Test(Track::Y))     DrawGroundSprite(overlay + RTO_Y, PALETTE_CRASH);
		if (pbs.Test(Track::Upper)) DrawTrackSprite(overlay + RTO_N, PALETTE_CRASH, ti, SLOPE_N);
		if (pbs.Test(Track::Lower)) DrawTrackSprite(overlay + RTO_S, PALETTE_CRASH, ti, SLOPE_S);
		if (pbs.Test(Track::Right)) DrawTrackSprite(overlay + RTO_E, PALETTE_CRASH, ti, SLOPE_E);
		if (pbs.Test(Track::Left))  DrawTrackSprite(overlay + RTO_W, PALETTE_CRASH, ti, SLOPE_W);
	}

	if (IsValidCorner(halftile_corner)) {
		DrawFoundation(ti, HalftileFoundation(halftile_corner));
		overlay = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Overlay, TileContext::UpperHalftile);
		ground = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Ground, TileContext::UpperHalftile);

		/* Draw higher halftile-overlay: Use the sloped sprites with three corners raised. They probably best fit the lightning. */
		Slope fake_slope = SlopeWithThreeCornersRaised(OppositeCorner(halftile_corner));

		SpriteID image;
		switch (rgt) {
			case RailGroundType::Barren: image = SPR_FLAT_BARE_LAND; break;
			case RailGroundType::SnowOrDesert:
			case RailGroundType::HalfTileSnow: image = SPR_FLAT_SNOW_DESERT_TILE; break;
			default: image = SPR_FLAT_GRASS_TILE; break;
		}

		image += SlopeToSpriteOffset(fake_slope);

		DrawGroundSprite(image, PAL_NONE, &(_halftile_sub_sprite[halftile_corner]));

		track = CornerToTrackBits(halftile_corner);

		int offset;
		switch (FindFirstTrack(track)) {
			default: NOT_REACHED();
			case Track::Upper: offset = RTO_N; break;
			case Track::Lower: offset = RTO_S; break;
			case Track::Right: offset = RTO_E; break;
			case Track::Left:  offset = RTO_W; break;
		}

		DrawTrackSprite(ground + offset, PAL_NONE, ti, fake_slope);
		if (_settings_client.gui.show_track_reservation && HasReservedTracks(ti->tile, track)) {
			DrawTrackSprite(overlay + offset, PALETTE_CRASH, ti, fake_slope);
		}
	}
}

/**
 * Returns which of the 5 junction-'Rail underlays' to use for the given track bits.
 * See https://newgrf-specs.tt-wiki.net/wiki/Action3/Railtypes for the cases.
 * @pre The track bits describe a junction, so at least two bits are set.
 * @param track The track bits to consider.
 * @return The offset in the five junction ground sprites.
 */
static int GetJunctionGroundSpriteOffset(TrackBits track)
{
	/* If none of the tracks end up in the NE corner, return the ground sprite
	 * where the NE of the tile is not covered. Repeat for the other directions.
	 * What remains are junctions where all directions are covered. */
	if (!track.Any(TRACK_BIT_3WAY_NE)) return 0;
	if (!track.Any(TRACK_BIT_3WAY_SW)) return 1;
	if (!track.Any(TRACK_BIT_3WAY_NW)) return 2;
	if (!track.Any(TRACK_BIT_3WAY_SE)) return 3;
	return 4;
}

/** Our track's rails' tops over the ground, metres (objekty/nase/koleje.py RAIL_TOP_M): the trains raised so much in 3D. */
static constexpr double THREE_D_RAIL_TOP_M = 0.372;
/** Half the width of our track's embankment, metres (koleje.py BED_W): the box a piece fills. */
static constexpr double THREE_D_TRACK_HALF_M = 1.7;

/** How finely the centre line of an arc is followed where one stands on its embankment (ThreeDTrackStandOver()), world units: a metre. */
static constexpr double THREE_D_ARC_STEP = OBJECT_UNITS_PER_METRE;

/** A piece of our track on a tile (DrawThreeDTrack()): where its middle is and which way it goes, world units from the tile's north corner. */
struct ThreeDTrackPiece {
	double x, y;    ///< its middle
	double ax, ay;  ///< from its middle to its end
	float facing;   ///< its +x, degrees (0 along +x, 90 along +y)
};

/** The pieces of the game's tracks: X and Y the tile long through its middle, the four others from the middle of an edge to the middle of the next. */
static const TrackIndexArray<ThreeDTrackPiece> THREE_D_TRACK_PIECES = {{{
	{8, 8, 8, 0, 0.0f},     // X
	{8, 8, 0, 8, 90.0f},    // Y
	{4, 4, 4, -4, -45.0f},  // upper (north corner)
	{12, 12, 4, -4, -45.0f}, // lower (south corner)
	{12, 4, 4, 4, 45.0f},   // left (west corner)
	{4, 12, 4, 4, 45.0f},   // right (east corner)
}}};

/**
 * Whether a tile's track is ours in 3D where it is drawn now (the player, 8. 11.: "the rails from above on and in 3D on
 * too by default"; ThreeDGroupShown(): from above or from the eyes by their settings): a plain rail of the normal
 * railways (not a monorail, not a maglev), not on the half of a tile's foundation nor by the water. A piece of a round
 * curve too (round_track.h; the player, 8. 11.: "the curves in 3D, yes"): laid along its arc (DrawThreeDTrack()). And a
 * station's, a waypoint's, a depot's, a level crossing's (ThreeDTrackOtherOn()).
 */
/**
 * The tracks of a tile our 3D track may lie on, of any kind: plain rail's pieces; a rail station's, a waypoint's, a
 * level crossing's, a depot's the one along its way; none of another tile.
 */
TrackBits ThreeDTrackBitsOf(TileIndex tile)
{
	if (!IsValidTile(tile)) return {};
	switch (GetTileType(tile)) {
		case TileType::Railway: return IsPlainRail(tile) ? GetTrackBits(tile) : TrackBits{DiagDirToDiagTrack(GetRailDepotDirection(tile))};
		case TileType::Station: return HasStationRail(tile) ? TrackBits{AxisToTrack(GetRailStationAxis(tile))} : TrackBits{};
		case TileType::Road: return IsLevelCrossing(tile) ? TrackBits{AxisToTrack(GetCrossingRailAxis(tile))} : TrackBits{};
		case TileType::TunnelBridge: return GetTunnelBridgeTransportType(tile) == TransportType::Rail ? TrackBits{DiagDirToDiagTrack(GetTunnelBridgeDirection(tile))} : TrackBits{};
		default: return {};
	}
}

/**
 * Whether our 3D track is on a tile of another kind than plain rail (the player, 8. 11.: "finish the 3D track";
 * DrawThreeDTrackOn()): a rail station's or a waypoint's -- not a set's station, which keeps its own look --, a depot's,
 * a level crossing's, a railway bridge's end (and over its bridge, DrawThreeDTrackOverBridge()), a railway tunnel's end
 * (and in it from the eyes); of the normal railways.
 */
static bool ThreeDTrackOtherOn(TileIndex tile)
{
	if (ThreeDTrackBitsOf(tile).None()) return false;
	if (GetRailTypeInfo(GetRailType(tile))->acceleration_type != VehicleAccelerationModel::Normal) return false;
	if (IsTileType(tile, TileType::Station) && IsCustomStationSpecIndex(tile)) return false;
	return GetThreeDFigure("kolej_rovna_drevo") != nullptr;
}

bool ThreeDTrackShownOn(TileIndex tile)
{
	if (!ThreeDEnabled() || !IsValidTile(tile) || !ThreeDGroupShown(ThreeDGroup::Rails)) return false;
	if (!IsPlainRailTile(tile)) return ThreeDTrackOtherOn(tile);
	if (GetRailTypeInfo(GetRailType(tile))->acceleration_type != VehicleAccelerationModel::Normal) return false;
	if (GetRailGroundType(tile) == RailGroundType::HalfTileWater) return false;
	const TrackBits track = GetTrackBits(tile);
	const Foundation f = GetRailFoundation(GetTileSlope(tile), track);
	if (IsNonContinuousFoundation(f)) return false;
	/* The tile after its foundation: flat, or up a slope along the track (X or Y); a piece across a corner level along
	 * its way (the game lays one so on a slope without a foundation). Else the game's sprites. */
	Slope s = GetTileSlope(tile);
	ApplyFoundationToSlope(f, s);
	for (Track t : track) {
		const ThreeDTrackPiece &p = THREE_D_TRACK_PIECES[t];
		if (s == SLOPE_FLAT) continue;
		if (t == Track::X ? (s == SLOPE_NE || s == SLOPE_SW) : t == Track::Y ? (s == SLOPE_NW || s == SLOPE_SE) : false) continue;
		if (t == Track::X || t == Track::Y) return false;
		auto at = [&](double x, double y) { return GetPartialPixelZ(std::clamp(static_cast<int>(x), 0, 15), std::clamp(static_cast<int>(y), 0, 15), s); };
		if (at(p.x - p.ax, p.y - p.ay) != at(p.x + p.ax, p.y + p.ay)) return false;
	}
	return GetThreeDFigure("kolej_rovna_drevo") != nullptr;
}

/** Where one stands on our track: its sleepers' tops over the ground, metres (koleje.py BED_H + SLEEPER_OVER). */
static constexpr double THREE_D_TRACK_STAND_M = 0.18;

/**
 * How high over the ground one stands at a point of a tile on our track's embankment (the player, 8. 11.: "the walker up
 * onto the embankment, sure"; FirstPersonStandZ()), metres: on its sleepers; 0 off it, or where the track is the game's.
 * @param lx,ly the point, world units from the tile's north corner
 */
double ThreeDTrackStandOver(TileIndex tile, double lx, double ly)
{
	if (!ThreeDTrackShownOn(tile)) return 0.0;
	const double half = THREE_D_TRACK_HALF_M * OBJECT_UNITS_PER_METRE;
	/* A round piece (the view from the eyes draws the round curves when they are on): along its arc. */
	const bool round = RoundTrackEnabled() && IsRoundTrackTile(tile);
	const double wx = TileX(tile) * TILE_SIZE + lx, wy = TileY(tile) * TILE_SIZE + ly;
	/* A level crossing: on its panels and its ramps, as the cars; beside the road on its track. */
	if (IsLevelCrossingTile(tile)) {
		if (const double over = ThreeDRoadOverTrack(tile, wx, wy); over > 0) return over;
	}
	for (Track t : ThreeDTrackBitsOf(tile)) {
		if (round) {
			const std::vector<RoundTrackPoint> line = RoundTrackCentreLine(tile, t, THREE_D_ARC_STEP);
			if (!line.empty()) {
				for (size_t i = 0; i + 1 < line.size(); i++) {
					const double sx = line[i + 1].x - line[i].x, sy = line[i + 1].y - line[i].y, len = std::hypot(sx, sy);
					if (len <= 0) continue;
					const double dx = wx - line[i].x, dy = wy - line[i].y, along = (dx * sx + dy * sy) / len;
					if (along >= 0 && along <= len && std::abs(dx * sy - dy * sx) / len <= half) return THREE_D_TRACK_STAND_M;
				}
				continue;
			}
		}
		const ThreeDTrackPiece &p = THREE_D_TRACK_PIECES[t];
		const double len = std::hypot(p.ax, p.ay), ux = p.ax / len, uy = p.ay / len;
		const double dx = lx - p.x, dy = ly - p.y;
		if (std::abs(dx * ux + dy * uy) <= len && std::abs(dx * uy - dy * ux) <= half) return THREE_D_TRACK_STAND_M;
	}
	return 0.0;
}

/**
 * How high a train's wheels run over the ground on a tile, metres: on our track in 3D its rails' tops
 * (ThreeDTrackShownOn()), else on the ground.
 */
double ThreeDTrackRailTop(TileIndex tile)
{
	return ThreeDTrackShownOn(tile) ? THREE_D_RAIL_TOP_M : 0.0;
}

/** Our catenary's wires over the ground, metres (objekty/nase/trakce.py CONTACT, CARRIER): the contact wire, the carrier wire. */
static constexpr double THREE_D_CONTACT_M = 5.872, THREE_D_CARRIER_M = 7.272;
/** In a tunnel under its vault (4.6 m from the eyes, CAB_TUNNEL_TOP of viewport.cpp): the contact wire (CabDrawTube()), the carrier wire just over it at the portal. */
static constexpr double THREE_D_TUNNEL_CONTACT_M = 4.4, THREE_D_TUNNEL_CARRIER_M = 4.55;
/** Our mast's top and where its arm begins, metres (trakce.py): the boxes they fill on the map. */
static constexpr double THREE_D_MAST_TOP_M = 8.85, THREE_D_ARM_LOW_M = 5.8;
/** The wires' colours from the eyes (GetThreeDWires() has them from above): the contact wire copper, the carrier wire darker. */
static const uint32_t THREE_D_CONTACT_COLOUR = Colour(92, 70, 52).data, THREE_D_CARRIER_COLOUR = Colour(54, 56, 58).data;

/** Whether a tile's track has our catenary where it is drawn now: our 3D track, electrified, the catenary wanted (ThreeDGroupShown()), no bridge over it. */
static bool ThreeDCatenaryOn(TileIndex tile)
{
	if (!ThreeDTrackShownOn(tile) || !HasRailCatenary(GetRailType(tile)) || _settings_game.vehicle.disable_elrails || IsBridgeAbove(tile)) return false;
	return ThreeDGroupShown(ThreeDGroup::Catenary) && GetThreeDFigure("trakce_stozar") != nullptr;
}

/**
 * Our catenary over our track on a tile (the player, 8. 11.: "such masts for the electrified track, for the catenary,
 * but simple, the wires as the fishing rod's line"; "faithful to the view from above, so one on each tile"; "the spacing
 * between the masts in the settings, as the spacing of the signals"; "2 may be over both"):
 *  - the wires over each piece of track from edge to edge, the contact wire 5.5 m over the rails, the carrier wire
 *    1.4 m over it, along a round curve's arc; from above thin square ropes (GetThreeDWires()), from the eyes lines
 *    (AddCabWire());
 *  - a mast (objekty/nase/trakce.py) at a corner of the tile by the middle of its edge the track crosses, as the game's
 *    pylons stand at the tiles' sides, its arm out over the track: a straight track (X, Y) at the edge where it comes
 *    in from the north, its mast at the corner on the north side of it -- tracks of the same way side by side two by
 *    two, each two one mast at the corner between them, an arm to each --; a piece across a corner at that corner, its
 *    arm to its middle;
 *    a round piece at its middle on the outside of its turn. Every so many tiles (gui.three_d_catenary_spacing), none
 *    where they are switched off (gui.three_d_catenary_masts_*): the wires alone.
 * On a bridge (ti its deck: its height, flat or up its ramp) each tile its own mast, none shared with the tracks beside
 * on the ground; on a tunnel's end the wires down to the tunnel's contact wire at its portal (CabDrawTube() of
 * viewport.cpp), no mast where the portal is at its edge on the north (it would stand in its face).
 * @param s the tile's slope after its foundation
 * @param round the tile is of a round curve drawn round
 * @param half_from a depot's track: only its wires from the edge its door is at to its middle, no mast; DiagDirection::Invalid none
 * @param pairs whether its masts are shared with the tracks of the same way beside (not on a bridge)
 * @param portal a tunnel's end: the edge its portal is at; DiagDirection::Invalid none
 */
static void DrawThreeDCatenary(const TileInfo *ti, TrackBits track, Slope s, bool round, bool eye, DiagDirection half_from = DiagDirection::Invalid, bool pairs = true, DiagDirection portal = DiagDirection::Invalid)
{
	const TileIndex tile = ti->tile;
	const int tx = TileX(tile), ty = TileY(tile);
	const double ox = tx * TILE_SIZE, oy = ty * TILE_SIZE;
	const double hpm = OBJECT_HEIGHT_PER_METRE, upm = OBJECT_UNITS_PER_METRE;
	/* The ground's height at a point of the tile over ti->z, units: the plane the track lies on. */
	auto ground = [&](double lx, double ly) -> double {
		switch (s) {
			case SLOPE_FLAT: return 0;
			case SLOPE_SW: return lx / TILE_SIZE * TILE_HEIGHT;
			case SLOPE_NE: return (TILE_SIZE - lx) / TILE_SIZE * TILE_HEIGHT;
			case SLOPE_SE: return ly / TILE_SIZE * TILE_HEIGHT;
			case SLOPE_NW: return (TILE_SIZE - ly) / TILE_SIZE * TILE_HEIGHT;
			default: return GetPartialPixelZ(std::clamp(static_cast<int>(lx), 0, 15), std::clamp(static_cast<int>(ly), 0, 15), s);
		}
	};

	/* The wires: from above gathered into the tile's object, from the eyes each a line. */
	std::vector<ThreeDWire> wires;
	/* How much lower a wire is at a point: at a tunnel's portal down to the tunnel's wires, metres. */
	auto lowered = [&](double lx, double ly, uint8_t k) -> double {
		bool at = false;
		switch (portal) {
			case DiagDirection::NE: at = lx < 0.5; break;
			case DiagDirection::SW: at = lx > TILE_SIZE - 0.5; break;
			case DiagDirection::NW: at = ly < 0.5; break;
			case DiagDirection::SE: at = ly > TILE_SIZE - 0.5; break;
			default: break;
		}
		if (!at) return 0;
		return k == 0 ? THREE_D_CONTACT_M - THREE_D_TUNNEL_CONTACT_M : THREE_D_CARRIER_M - THREE_D_TUNNEL_CARRIER_M;
	};
	auto wire = [&](double ax, double ay, double bx, double by) {
		const double ga = ground(ax, ay), gb = ground(bx, by);
		for (uint8_t k = 0; k < 2; k++) {
			const double m = k == 0 ? THREE_D_CONTACT_M : THREE_D_CARRIER_M, da = lowered(ax, ay, k), db = lowered(bx, by, k);
			if (eye) {
				/* The ground of its ends lifted from the eyes as the land (EYE_LEVEL_STRETCH), its metres over it not: a
				 * lowered end's metres taken off its ground so much less. */
				AddCabWire(ox + ax, oy + ay, ti->z + ga - da * hpm / EYE_LEVEL_STRETCH, ox + bx, oy + by, ti->z + gb - db * hpm / EYE_LEVEL_STRETCH, m, k == 0 ? THREE_D_CONTACT_COLOUR : THREE_D_CARRIER_COLOUR);
			} else {
				wires.push_back({static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(ga + (m - da) * hpm), static_cast<float>(bx), static_cast<float>(by), static_cast<float>(gb + (m - db) * hpm), k});
			}
		}
	};

	/* A mast at a point of the tile with its arms (facing, degrees, 0 along +x), each so many units long; from above
	 * each in its box -- the mast a narrow column, an arm from the mast to its end, high up --, so that a train under the
	 * arm is before or behind the mast as it is. */
	const bool masts = eye ? _settings_client.gui.three_d_catenary_masts_eye : _settings_client.gui.three_d_catenary_masts_map;
	const int every = std::max<int>(1, _settings_client.gui.three_d_catenary_spacing);
	auto every_at = [every](int v) { return ((v % every) + every) % every == 0; };
	auto mast = [&](double lx, double ly, std::initializer_list<float> arms, std::string_view arm_name, double arm_len) {
		if (!masts) return;
		const double px = ox + lx, py = oy + ly;
		const int z = ti->z + static_cast<int>(std::lround(ground(lx, ly)));
		if (eye) {
			AddCabFigure("trakce_stozar", 1.0f, px, py, z, 0.0f);
			for (float f : arms) AddCabFigure(arm_name, 1.0f, px, py, z, f);
			return;
		}
		const int ix = static_cast<int>(std::lround(px)), iy = static_cast<int>(std::lround(py));
		const int top = z + static_cast<int>(std::lround(THREE_D_MAST_TOP_M * hpm));
		const std::array<int, 6> column{ix - 1, iy - 1, z, ix, iy, top};
		AddThreeDFigureToViewportInBox("trakce_stozar", 1.0f, ix, iy, z, 0.0f, _cur_dpi->zoom, tile, 0, -1, 0, &column);
		for (float f : arms) {
			const double a = f * M_PI / 180.0, ex = px + std::cos(a) * arm_len, ey = py + std::sin(a) * arm_len;
			const int bx0 = static_cast<int>(std::floor(std::min(px, ex))), by0 = static_cast<int>(std::floor(std::min(py, ey)));
			const int bx1 = std::max(bx0, static_cast<int>(std::ceil(std::max(px, ex))) - 1), by1 = std::max(by0, static_cast<int>(std::ceil(std::max(py, ey))) - 1);
			const std::array<int, 6> box{bx0, by0, z + static_cast<int>(std::lround(THREE_D_ARM_LOW_M * hpm)), bx1, by1, top};
			AddThreeDFigureToViewportInBox(arm_name, 1.0f, ix, iy, z, f, _cur_dpi->zoom, tile, 0, -1, 0, &box);
		}
	};
	const double arm = TILE_SIZE / 2.0, arm_diagonal = TILE_SIZE / 2.0 / std::sqrt(2.0);
	/* A neighbour's straight track of the same way beside, with our catenary: one mast between them. */
	auto beside = [&](int dx, int dy, Track t) {
		if (!pairs) return false;
		const int nx = tx + dx, ny = ty + dy;
		if (nx < 0 || ny < 0 || nx >= static_cast<int>(Map::SizeX()) || ny >= static_cast<int>(Map::SizeY())) return false;
		const TileIndex n = TileXY(nx, ny);
		if (!ThreeDCatenaryOn(n) || !ThreeDTrackBitsOf(n).Test(t)) return false;
		return !(RoundTrackDrawing() && IsRoundTrackTile(n) && !RoundTrackCentreLine(n, t, TILE_SIZE).empty());
	};

	for (Track t : track) {
		if (round) {
			/* Along its arc; its mast at its middle, on the outside of its turn (where the arc turns left, its centre is
			 * on the left). */
			const std::vector<RoundTrackPoint> line = RoundTrackCentreLine(tile, t, 2.0 * upm);
			if (!line.empty()) {
				for (size_t i = 0; i + 1 < line.size(); i++) wire(line[i].x - ox, line[i].y - oy, line[i + 1].x - ox, line[i + 1].y - oy);
				if (every_at(tx + ty)) {
					const RoundTrackPoint &m = line[line.size() / 2];
					const double turn = line.front().tx * line.back().ty - line.front().ty * line.back().tx;
					const double side = turn > 0 ? -1.0 : 1.0; // the left normal (-ty, tx) times it: outward
					const double nx = -m.ty * side, ny = m.tx * side;
					mast(m.x - ox + nx * arm, m.y - oy + ny * arm, {static_cast<float>(std::atan2(-ny, -nx) * 180.0 / M_PI)}, "trakce_vyloznik", arm);
				}
				continue;
			}
		}
		const ThreeDTrackPiece &p = THREE_D_TRACK_PIECES[t];
		if (half_from != DiagDirection::Invalid) {
			const TileIndexDiffC d = TileIndexDiffCByDiagDir(half_from);
			wire(8 + d.x * 8, 8 + d.y * 8, 8, 8);
			continue;
		}
		wire(p.x - p.ax, p.y - p.ay, p.x + p.ax, p.y + p.ay);
		switch (t) {
			case Track::X:
			case Track::Y: {
				/* At its edge on the north (X: north-east, Y: north-west), at the corner on the north side of it; tracks
				 * of the same way side by side two by two from the first on the north, each two the mast at the corner
				 * between them, an arm to each. */
				const bool x = t == Track::X;
				if (portal == DiagDirection::NE || portal == DiagDirection::NW || !every_at(x ? tx : ty)) break;
				const int sx = x ? 0 : 1, sy = x ? 1 : 0;
				int before = 0;
				while (before < 16 && beside(-sx * (before + 1), -sy * (before + 1), t)) before++;
				if (before % 2 == 1) break; // the second of a two: the first has the mast
				const float out = x ? 90.0f : 0.0f;
				if (beside(sx, sy, t)) {
					mast(sx * TILE_SIZE, sy * TILE_SIZE, {out + 180.0f, out}, "trakce_vyloznik", arm);
				} else {
					mast(0, 0, {out}, "trakce_vyloznik", arm);
				}
				break;
			}
			default: {
				/* Across a corner: at that corner, its arm to its middle. */
				if (!every_at(t == Track::Upper || t == Track::Lower ? tx - ty : tx + ty)) break;
				const double cx = (t == Track::Lower || t == Track::Left) ? TILE_SIZE : 0, cy = (t == Track::Lower || t == Track::Right) ? TILE_SIZE : 0;
				mast(cx, cy, {static_cast<float>(std::atan2(p.y - cy, p.x - cx) * 180.0 / M_PI)}, "trakce_vyloznik_sikmy", arm_diagonal);
				break;
			}
		}
	}
	if (!eye && !wires.empty()) {
		/* As thick as a pixel and a half at the zoom: a unit and a fifth at the normal one. */
		const double thick = 1.2 * std::pow(2.0, to_underlying(_cur_dpi->zoom) - to_underlying(ZoomLevel::Normal));
		AddThreeDTileObject(GetThreeDWires(tile, wires, thick), tile, ti->z);
	}
}

/**
 * Our track in 3D (the player, test3: "our own 3D rails"; 8. 11.: "the rails from above on and in 3D on too by default",
 * "a very low embankment", "the sleepers over the embankment, a low embankment so one can walk over it"): the ground of
 * the tile without the track's sprites, each piece of track our object (objekty/nase/koleje.py) -- a straight one the
 * tile long, one across a corner, one up a slope (a level of the map 3 m, from the eyes 5 m); wooden sleepers, concrete
 * on an electrified track. In the turned view and among the objects facing north each in the box it fills
 * (AddThreeDFigureToViewportInBox()), from the eyes lying on the ground (AddCabGroundFigure()). On a tile of a round
 * curve (round_track.h; the player: "the curves in 3D, yes") a round piece along its arc in one (GetThreeDTrackArc()).
 * @param ti the tile
 * @param track its tracks
 * @return whether it was drawn so: its sprites of the track left out
 */
static void DrawThreeDBufferStops(const TileInfo *ti, Track t, int z, bool eye);

static bool DrawThreeDTrack(TileInfo *ti, TrackBits track)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return false;
	if (track.None() || !ThreeDTrackShownOn(ti->tile)) return false;

	/* A tile of a round curve (round_track.h): flat, the ground the game draws under no track; its round pieces along
	 * their arcs, no strip of the game's track drawn after the ground. */
	const bool round = RoundTrackDrawing() && IsRoundTrackTile(ti->tile);
	/* The tile after its foundation (ThreeDTrackShownOn(): flat, or as the pieces lie on it). */
	const Foundation f = GetRailFoundation(ti->tileh, track);
	Slope s = ti->tileh;
	ApplyFoundationToSlope(f, s);
	if (round) {
		DrawRoundTrackGround(ti->tile);
	} else {
		DrawFoundation(ti, f);
		SpriteID ground;
		switch (GetRailGroundType(ti->tile)) {
			case RailGroundType::Barren: ground = SPR_FLAT_BARE_LAND; break;
			case RailGroundType::SnowOrDesert:
			case RailGroundType::HalfTileSnow: ground = SPR_FLAT_SNOW_DESERT_TILE; break;
			default: ground = SPR_FLAT_GRASS_TILE; break;
		}
		DrawGroundSprite(ground + SlopeToSpriteOffset(ti->tileh), PAL_NONE);
	}

	const bool concrete = HasRailCatenary(GetRailType(ti->tile));
	const std::string_view straight = concrete ? "kolej_rovna_beton" : "kolej_rovna_drevo";
	const std::string_view diagonal = concrete ? "kolej_sikma_beton" : "kolej_sikma_drevo";
	const std::string_view slope = eye ? (concrete ? "kolej_svah_oci_beton" : "kolej_svah_oci_drevo") : (concrete ? "kolej_svah_beton" : "kolej_svah_drevo");
	const double ox = TileX(ti->tile) * TILE_SIZE, oy = TileY(ti->tile) * TILE_SIZE;
	const double half = THREE_D_TRACK_HALF_M * OBJECT_UNITS_PER_METRE;
	const int rails = std::max(1, static_cast<int>(std::lround(THREE_D_RAIL_TOP_M * OBJECT_HEIGHT_PER_METRE)));

	/* A piece with its middle at (x, y), from it to its end (ax, ay): in the turned view in its box (along it to its
	 * ends, across it the embankment's half each side, up from its foot to its rails' tops), from the eyes lying on the
	 * ground. */
	auto put = [&](std::string_view name, double x, double y, double ax, double ay, int z, int foot, int tall, float facing) {
		if (eye) {
			AddCabGroundFigure(name, x, y, z, facing);
			return;
		}
		const double len = std::max(std::hypot(ax, ay), 1e-6), cx = -ay / len * half, cy = ax / len * half;
		const double hx = std::abs(ax) + std::abs(cx), hy = std::abs(ay) + std::abs(cy);
		const int bx0 = static_cast<int>(std::lround(x - hx)), by0 = static_cast<int>(std::lround(y - hy));
		const int bx1 = std::max(bx0, static_cast<int>(std::lround(x + hx)) - 1), by1 = std::max(by0, static_cast<int>(std::lround(y + hy)) - 1);
		const std::array<int, 6> box{bx0, by0, foot, bx1, by1, foot + tall - 1};
		AddThreeDFigureToViewportInBox(name, 1.0f, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), z, facing, _cur_dpi->zoom, ti->tile, 0, -1, 0, &box);
	};

	for (Track t : track) {
		/* A round piece along its arc in one (GetThreeDTrackArc(); the player: "like the fishing rod's line, a thick square
		 * rope"), an object standing on the tile as a house does; a piece straight within its tile as on any tile. */
		if (round) {
			if (const ThreeDObject *arc = GetThreeDTrackArc(ti->tile, t, concrete); arc != nullptr) {
				AddThreeDTileObject(arc, ti->tile, ti->z);
				continue;
			}
		}
		const ThreeDTrackPiece &p = THREE_D_TRACK_PIECES[t];
		std::string_view name = (t == Track::X || t == Track::Y) ? straight : diagonal;
		float facing = p.facing;
		int z = ti->z;
		int foot = ti->z;
		int tall = rails;
		if (s != SLOPE_FLAT && (t == Track::X || t == Track::Y)) {
			/* Up the slope: its middle half a level up, its +x uphill; its box from the slope's foot. */
			name = slope;
			z += TILE_HEIGHT / 2;
			facing = s == SLOPE_SW ? 0.0f : s == SLOPE_SE ? 90.0f : s == SLOPE_NE ? 180.0f : 270.0f;
			tall += TILE_HEIGHT;
		} else if (s != SLOPE_FLAT) {
			z += GetPartialPixelZ(static_cast<int>(p.x), static_cast<int>(p.y), s);
			foot = z;
		}
		put(name, ox + p.x, oy + p.y, p.ax, p.ay, z, foot, tall, facing);
		/* At a dead end of a straight piece on the level its buffer stop (DrawThreeDBufferStops()). */
		if (s == SLOPE_FLAT) DrawThreeDBufferStops(ti, t, ti->z, eye);
	}
	/* Its catenary over it, when it is electrified (DrawThreeDCatenary()). */
	if (ThreeDCatenaryOn(ti->tile)) DrawThreeDCatenary(ti, track, s, round, eye);
	return true;
}

/**
 * A level crossing's panels each side of the track's middle and its ramps down to the road; how far along the track
 * from the tile's middle they go, the road's half width; metres (koleje.py PANEL_HALF, RAMP, ROAD_HALF).
 */
static constexpr double THREE_D_PANEL_HALF_M = 2.2, THREE_D_RAMP_M = 2.5, THREE_D_CROSSING_ROAD_HALF_M = 4.6;

/**
 * How high a road vehicle (or the walker) stands over the road at a point of a level crossing with our 3D track
 * (DrawThreeDTrackOn(); objekty/nase/koleje.py "prejezd"), metres: on its panels as high as the rails' heads, down its
 * ramps to the road each side; 0 off them -- beside the road the track as anywhere --, or where the crossing is the game's.
 * @param x,y the point, world units
 */
double ThreeDRoadOverTrack(TileIndex tile, double x, double y)
{
	if (!IsValidTile(tile) || !IsLevelCrossingTile(tile) || !ThreeDTrackShownOn(tile)) return 0.0;
	const bool along_x = GetCrossingRailAxis(tile) == Axis::X;
	const double mx = TileX(tile) * TILE_SIZE + TILE_SIZE / 2.0, my = TileY(tile) * TILE_SIZE + TILE_SIZE / 2.0;
	if (std::abs(along_x ? x - mx : y - my) / OBJECT_UNITS_PER_METRE > THREE_D_CROSSING_ROAD_HALF_M) return 0.0;
	const double d = std::abs(along_x ? y - my : x - mx) / OBJECT_UNITS_PER_METRE;
	if (d <= THREE_D_PANEL_HALF_M) return THREE_D_RAIL_TOP_M;
	return THREE_D_RAIL_TOP_M * std::max(0.0, 1.0 - (d - THREE_D_PANEL_HALF_M) / THREE_D_RAMP_M);
}

/** How high our track's box reaches on the map, units of height: to its rails' tops. */
static int ThreeDRailsTall()
{
	return std::max(1, static_cast<int>(std::lround(THREE_D_RAIL_TOP_M * OBJECT_HEIGHT_PER_METRE)));
}

/**
 * Our buffer stop (objekty/nase/zarazedlo.py): from its buffers back along the track (its own piece of track: 1.8 m),
 * across it each way, its top; metres.
 */
static constexpr double THREE_D_STOP_LONG_M = 1.55, THREE_D_STOP_STUB_M = 1.8, THREE_D_STOP_HALF_M = 1.15, THREE_D_STOP_TOP_M = 2.25;

/**
 * Our Štůrc buffer stop (the player, 8. 11.: "the buffer stop on the track", "yes the buffer stop"; objekty/nase/zarazedlo.py)
 * at each dead end of a straight piece of our track: where at an end of it no track goes on from the tile's edge (the
 * next tile's track entered from there: a depot by its door, a tunnel or a bridge by its mouth), its buffers to the
 * trains. A train's front comes to the very edge before it turns back: the buffers there, the stop beyond on its own
 * piece of track, where the next tile is free land level with it; else within the tile, its back at the edge.
 * @param z the track's height, units (the tile flat after its foundation)
 */
static void DrawThreeDBufferStops(const TileInfo *ti, Track t, int z, bool eye)
{
	if ((t != Track::X && t != Track::Y) || GetThreeDFigure("zarazedlo") == nullptr) return;
	const Axis axis = t == Track::X ? Axis::X : Axis::Y;
	for (const DiagDirection dir : {AxisToDiagDir(axis), ReverseDiagDir(AxisToDiagDir(axis))}) {
		const TileIndexDiffC d = TileIndexDiffCByDiagDir(dir);
		const TileIndex n = AddTileIndexDiffCWrap(ti->tile, d);
		if (n != INVALID_TILE && GetTileTrackStatus(n, TransportType::Rail, RoadTramType::Invalid, ReverseDiagDir(dir)).trackdirs.Any(DiagdirReachesTrackdirs(dir))) continue;
		/* Its buffers at the middle of that edge, facing back into the tile, the stop beyond on its piece of track; or
		 * within the tile, its back at the edge. */
		const bool beyond = n != INVALID_TILE && (IsTileType(n, TileType::Clear) || IsTileType(n, TileType::Trees)) && GetTileSlope(n) == SLOPE_FLAT && GetTilePixelZ(n) == z;
		const double upm = OBJECT_UNITS_PER_METRE, back = (beyond ? THREE_D_STOP_STUB_M : THREE_D_STOP_LONG_M) * upm;
		const double edge_x = TileX(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0 + d.x * TILE_SIZE / 2.0, edge_y = TileY(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0 + d.y * TILE_SIZE / 2.0;
		const double bx = beyond ? edge_x : edge_x - d.x * back, by = beyond ? edge_y : edge_y - d.y * back;
		const std::string_view name = beyond ? "zarazedlo_kolej" : "zarazedlo";
		const float facing = static_cast<float>(std::atan2(-d.y, -d.x) * 180.0 / M_PI);
		if (eye) {
			AddCabFigure(name, 1.0f, bx, by, z, facing);
			continue;
		}
		const double across = THREE_D_STOP_HALF_M * upm, fx = bx + d.x * back, fy = by + d.y * back;
		const double x0 = std::min(bx, fx) - (axis == Axis::Y ? across : 0), x1 = std::max(bx, fx) + (axis == Axis::Y ? across : 0);
		const double y0 = std::min(by, fy) - (axis == Axis::X ? across : 0), y1 = std::max(by, fy) + (axis == Axis::X ? across : 0);
		const int bx0 = static_cast<int>(std::floor(x0)), by0 = static_cast<int>(std::floor(y0));
		const std::array<int, 6> box{bx0, by0, z, std::max(bx0, static_cast<int>(std::ceil(x1)) - 1), std::max(by0, static_cast<int>(std::ceil(y1)) - 1),
				z + static_cast<int>(std::lround(THREE_D_STOP_TOP_M * OBJECT_HEIGHT_PER_METRE))};
		AddThreeDFigureToViewportInBox(name, 1.0f, static_cast<int>(std::lround(bx)), static_cast<int>(std::lround(by)), z, facing, _cur_dpi->zoom, ti->tile, 0, -1, 0, &box);
	}
}

/**
 * A straight piece of our track: in the turned view and among the objects facing north in its box (along it to its
 * ends, across it so far, up from its foot), from the eyes lying there (AddCabGroundFigure()).
 * @param x,y its middle, world units
 * @param z its foot at its middle, units of height
 * @param along,across half its box along its way and across it, world units
 * @param foot,tall where its box begins and how high it is, units of height
 */
static void PutThreeDStraight(const TileInfo *ti, std::string_view name, Axis axis, double x, double y, int z, float facing, double along, double across, int foot, int tall)
{
	if (ViewportDrawingEye()) {
		AddCabGroundFigure(name, x, y, z, facing);
		return;
	}
	const double hx = axis == Axis::X ? along : across, hy = axis == Axis::X ? across : along;
	const int bx0 = static_cast<int>(std::lround(x - hx)), by0 = static_cast<int>(std::lround(y - hy));
	const int bx1 = std::max(bx0, static_cast<int>(std::lround(x + hx)) - 1), by1 = std::max(by0, static_cast<int>(std::lround(y + hy)) - 1);
	const std::array<int, 6> box{bx0, by0, foot, bx1, by1, foot + tall - 1};
	AddThreeDFigureToViewportInBox(name, 1.0f, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), z, facing, _cur_dpi->zoom, ti->tile, 0, -1, 0, &box);
}

/** Our level crossing's AŽD posts (objekty/nase/azd.py): from the track's middle along the road, from the road's middle aside, metres. */
static constexpr double THREE_D_CROSSING_POST_TRACK_M = 5.6, THREE_D_CROSSING_POST_SIDE_M = 5.0;
/** How long a light of theirs is on, and off, the game's ticks: half a second, the red ones in turn, the white one by itself. */
static constexpr int THREE_D_CROSSING_BLINK_TICKS = 18;
/** In a post's frame (azd.py: its lights to +x, the road at -y; metres): its footprint, its top, where its barrier turns and how long it is. */
static constexpr double THREE_D_POST_X0 = -0.3, THREE_D_POST_X1 = 0.4, THREE_D_POST_Y0 = -0.35, THREE_D_POST_Y1 = 0.9, THREE_D_POST_TOP_M = 3.8;
static constexpr double THREE_D_BARRIER_X = 0.06, THREE_D_BARRIER_Y = 0.25, THREE_D_BARRIER_Z = 1.45, THREE_D_BARRIER_LEN = 5.0;

/**
 * The posts of our level crossing (the player, 8. 11.: "the lights one Czech and the crossing", "1 yes"): the AŽD post of
 * the player's model, for each way the road's traffic comes the one on its right before the track (on its left driving
 * on the left: the mirrored post), its warning lights to it, its barrier over the right half of the road. The crossing
 * closed (IsCrossingBarred(): a train is coming) the barriers down and the two red lights in turn; open, the barriers up
 * and the white light blinking slowly. On the map the tile drawn again as the lights change (NoteThreeDBlinking()).
 * @param rail_axis the track's way; the road across it
 */
static void DrawThreeDCrossingPosts(const TileInfo *ti, Axis rail_axis, bool eye)
{
	if (GetThreeDFigure("azd_sloupek") == nullptr) return;
	const bool closed = IsCrossingBarred(ti->tile);
	const bool on_left = _settings_game.vehicle.road_side == RoadVehicleDrivingSide::Left;
	const bool phase = (TimerGameTick::counter / THREE_D_CROSSING_BLINK_TICKS) % 2 == 0;
	const std::string_view lit = closed ? (phase ? "_l" : "_p") : (phase ? "_b" : "");
	const std::string_view mirror = on_left ? "_z" : "";
	const std::string post = fmt::format("azd_sloupek{}{}", lit, mirror);
	const std::string barrier = fmt::format("azd_zavora_{}{}", closed ? "dole" : "nahore", mirror);
	const double upm = OBJECT_UNITS_PER_METRE, hpm = OBJECT_HEIGHT_PER_METRE;
	const double mx = TileX(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0, my = TileY(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0;
	for (const int way : {1, -1}) {
		/* The way the traffic goes along the road (across the track), its right (left), the post before the track there. */
		const double dx = rail_axis == Axis::X ? 0.0 : way, dy = rail_axis == Axis::X ? way : 0.0;
		const double side = on_left ? -1.0 : 1.0, rx = dy * side, ry = -dx * side;
		const double px = mx - dx * THREE_D_CROSSING_POST_TRACK_M * upm + rx * THREE_D_CROSSING_POST_SIDE_M * upm;
		const double py = my - dy * THREE_D_CROSSING_POST_TRACK_M * upm + ry * THREE_D_CROSSING_POST_SIDE_M * upm;
		/* Its lights to the traffic coming: its frame's +x against the way. */
		const double fa = std::atan2(-dy, -dx), ca = std::cos(fa), sa = std::sin(fa);
		const float facing = static_cast<float>(fa * 180.0 / M_PI);
		if (eye) {
			AddCabFigure(post, 1.0f, px, py, ti->z, facing);
			AddCabFigure(barrier, 1.0f, px, py, ti->z, facing);
			continue;
		}
		/* A box of a part of it on the map: its corners in the post's frame (y mirrored driving on the left), world units. */
		auto box = [&](double a0, double a1, double b0, double b1, int z0, int z1) {
			double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
			for (const double a : {a0, a1}) {
				for (const double b0m : {b0, b1}) {
					const double b = on_left ? -b0m : b0m;
					const double wx = px + (a * ca - b * sa) * upm, wy = py + (a * sa + b * ca) * upm;
					x0 = std::min(x0, wx);
					y0 = std::min(y0, wy);
					x1 = std::max(x1, wx);
					y1 = std::max(y1, wy);
				}
			}
			const int bx0 = static_cast<int>(std::floor(x0)), by0 = static_cast<int>(std::floor(y0));
			return std::array<int, 6>{bx0, by0, z0, std::max(bx0, static_cast<int>(std::ceil(x1)) - 1), std::max(by0, static_cast<int>(std::ceil(y1)) - 1), z1};
		};
		const int ix = static_cast<int>(std::lround(px)), iy = static_cast<int>(std::lround(py));
		const int pivot = ti->z + static_cast<int>(std::lround(THREE_D_BARRIER_Z * hpm));
		const std::array<int, 6> post_box = box(THREE_D_POST_X0, THREE_D_POST_X1, THREE_D_POST_Y0, THREE_D_POST_Y1, ti->z, ti->z + static_cast<int>(std::lround(THREE_D_POST_TOP_M * hpm)));
		const std::array<int, 6> barrier_box = closed ?
				box(THREE_D_BARRIER_X - 0.1, THREE_D_BARRIER_X + 0.1, THREE_D_BARRIER_Y - THREE_D_BARRIER_LEN, THREE_D_BARRIER_Y - 0.2, pivot - 1, pivot + 1) :
				box(THREE_D_BARRIER_X - 0.1, THREE_D_BARRIER_X + 0.1, THREE_D_BARRIER_Y - 0.1, THREE_D_BARRIER_Y + 0.1, pivot + 1, pivot + static_cast<int>(std::lround(THREE_D_BARRIER_LEN * hpm)));
		AddThreeDFigureToViewportInBox(post, 1.0f, ix, iy, ti->z, facing, _cur_dpi->zoom, ti->tile, 0, -1, 0, &post_box);
		AddThreeDFigureToViewportInBox(barrier, 1.0f, ix, iy, ti->z, facing, _cur_dpi->zoom, ti->tile, 0, -1, 0, &barrier_box);
	}
	if (!eye) {
		NoteThreeDBlinking(ti->tile, THREE_D_CROSSING_BLINK_TICKS, TileX(ti->tile) * TILE_SIZE, TileY(ti->tile) * TILE_SIZE, (TileX(ti->tile) + 1) * TILE_SIZE, (TileY(ti->tile) + 1) * TILE_SIZE,
				ti->z, ti->z + static_cast<int>(std::lround(THREE_D_POST_TOP_M * hpm)) + 2);
	}
}

/**
 * Our 3D track on a tile of another kind than plain rail (the player, 8. 11.: "finish the 3D track"): a rail station's or
 * a waypoint's (their platforms and buildings the game's), a depot's from its door to its middle, a level crossing's --
 * its concrete panels as high as the rails' heads across the track and its ramps for the road (ThreeDRoadOverTrack()).
 * A straight piece along its way, as on plain rail (DrawThreeDTrack()), on the tile flat after its foundation; its
 * catenary over it (a depot's wires alone). The ground under it the caller's.
 * @param axis the way of the track
 * @param door a depot's: the edge its door is at; DiagDirection::Invalid the whole tile
 * @param crossing a level crossing's piece
 * @return whether it was drawn so: the caller leaves out its sprites of the track
 */
bool DrawThreeDTrackOn(const TileInfo *ti, Axis axis, DiagDirection door, bool crossing)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return false;
	if (!ThreeDTrackShownOn(ti->tile) || ti->tileh != SLOPE_FLAT) return false;
	if (crossing && GetThreeDFigure("kolej_prejezd_drevo") == nullptr) return false;
	const bool concrete = HasRailCatenary(GetRailType(ti->tile));
	const bool half = door != DiagDirection::Invalid;
	const std::string_view name = crossing ? (concrete ? "kolej_prejezd_beton" : "kolej_prejezd_drevo") : half ? (concrete ? "kolej_pul_beton" : "kolej_pul_drevo") : (concrete ? "kolej_rovna_beton" : "kolej_rovna_drevo");
	/* Its middle: the tile's, a depot's half way from its door to it. */
	double mx = TILE_SIZE / 2.0, my = TILE_SIZE / 2.0;
	if (half) {
		const TileIndexDiffC d = TileIndexDiffCByDiagDir(door);
		mx += d.x * TILE_SIZE / 4.0;
		my += d.y * TILE_SIZE / 4.0;
	}
	const double x = TileX(ti->tile) * TILE_SIZE + mx, y = TileY(ti->tile) * TILE_SIZE + my;
	/* Across it its embankment, a crossing's panels and ramps. */
	const double across = (crossing ? THREE_D_PANEL_HALF_M + THREE_D_RAMP_M : THREE_D_TRACK_HALF_M) * OBJECT_UNITS_PER_METRE;
	PutThreeDStraight(ti, name, axis, x, y, ti->z, axis == Axis::X ? 0.0f : 90.0f, half ? TILE_SIZE / 4.0 : TILE_SIZE / 2.0, across, ti->z, ThreeDRailsTall());
	/* A level crossing's AŽD posts with their lights and barriers; a station's track ending at its edge, its buffer stop. */
	if (crossing) DrawThreeDCrossingPosts(ti, axis, eye);
	if (!crossing && !half) DrawThreeDBufferStops(ti, AxisToTrack(axis), ti->z, eye);
	if (ThreeDCatenaryOn(ti->tile)) DrawThreeDCatenary(ti, TrackBits{AxisToTrack(axis)}, SLOPE_FLAT, false, eye, door);
	return true;
}

/** The straight piece of our track along a way (koleje.py kolej_rovna_*): wooden sleepers, concrete on an electrified railway. */
static std::string_view ThreeDStraightName(RailType rt)
{
	return HasRailCatenary(rt) ? "kolej_rovna_beton" : "kolej_rovna_drevo";
}

/**
 * Our 3D track on a railway bridge's end (the player, 8. 11.: "finish the 3D track"): up its ramp a piece up a slope (a
 * level of the map 3 m, from the eyes 5 m) from the ground to the bridge, or level with the bridge on a foundation; its
 * catenary over it, its mast its own. The ramp is the bridge's (its sprites, or the colleague's 3D pieces, whose deck's
 * band with the game's rails viewport.cpp leaves out).
 * @return whether it was drawn so
 */
bool DrawThreeDTrackBridgeHead(const TileInfo *ti)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return false;
	if (!ThreeDTrackShownOn(ti->tile)) return false;
	const DiagDirection dir = GetTunnelBridgeDirection(ti->tile);
	const Axis axis = DiagDirToAxis(dir);
	const int bz = GetBridgePixelHeight(ti->tile);
	const double ox = TileX(ti->tile) * TILE_SIZE, oy = TileY(ti->tile) * TILE_SIZE;
	/* Its outer end's middle (the bridge goes on from its inner one): on the ground, or level with the bridge. */
	const bool inner_low = dir == DiagDirection::NE || dir == DiagDirection::NW;
	const double out = inner_low ? TILE_SIZE - 1 : 0;
	const int out_z = GetSlopePixelZ(static_cast<int>(ox + (axis == Axis::X ? out : TILE_SIZE / 2)), static_cast<int>(oy + (axis == Axis::X ? TILE_SIZE / 2 : out)), true);
	const bool rises = bz - out_z >= static_cast<int>(TILE_HEIGHT) / 2;
	const RailType rt = GetRailType(ti->tile);
	const double x = ox + TILE_SIZE / 2.0, y = oy + TILE_SIZE / 2.0, across = THREE_D_TRACK_HALF_M * OBJECT_UNITS_PER_METRE;
	/* The deck it lies on: its catenary's ground. */
	TileInfo deck = *ti;
	deck.tileh = SLOPE_FLAT;
	deck.z = bz;
	if (rises) {
		const bool concrete = HasRailCatenary(rt);
		const std::string_view name = eye ? (concrete ? "kolej_svah_oci_beton" : "kolej_svah_oci_drevo") : (concrete ? "kolej_svah_beton" : "kolej_svah_drevo");
		/* Its middle half a level under the bridge, its +x up into it. */
		const float facing = dir == DiagDirection::SW ? 0.0f : dir == DiagDirection::SE ? 90.0f : dir == DiagDirection::NE ? 180.0f : 270.0f;
		PutThreeDStraight(ti, name, axis, x, y, bz - static_cast<int>(TILE_HEIGHT) / 2, facing, TILE_SIZE / 2.0, across, bz - static_cast<int>(TILE_HEIGHT), ThreeDRailsTall() + static_cast<int>(TILE_HEIGHT));
		deck.z = bz - static_cast<int>(TILE_HEIGHT);
		deck.tileh = dir == DiagDirection::SW ? SLOPE_SW : dir == DiagDirection::SE ? SLOPE_SE : dir == DiagDirection::NE ? SLOPE_NE : SLOPE_NW;
	} else {
		PutThreeDStraight(ti, ThreeDStraightName(rt), axis, x, y, bz, axis == Axis::X ? 0.0f : 90.0f, TILE_SIZE / 2.0, across, bz, ThreeDRailsTall());
	}
	if (ThreeDCatenaryOn(ti->tile)) DrawThreeDCatenary(&deck, TrackBits{AxisToTrack(axis)}, deck.tileh, false, eye, DiagDirection::Invalid, false);
	return true;
}

/**
 * Our 3D track over a tile under a railway bridge (DrawBridgeMiddle()): a straight piece the tile long on the bridge's
 * deck at its height, its catenary over it, a mast on each tile of its own. The deck is the bridge's (its sprites, or
 * the colleague's 3D pieces, whose deck's band with the game's rails viewport.cpp leaves out).
 * @param south the bridge's southern end
 */
void DrawThreeDTrackOverBridge(const TileInfo *ti, TileIndex south)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return;
	if (!ThreeDTrackShownOn(south)) return;
	const Axis axis = GetBridgeAxis(ti->tile);
	const int bz = GetBridgePixelHeight(south);
	const double x = TileX(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0, y = TileY(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0;
	PutThreeDStraight(ti, ThreeDStraightName(GetRailType(south)), axis, x, y, bz, axis == Axis::X ? 0.0f : 90.0f, TILE_SIZE / 2.0, THREE_D_TRACK_HALF_M * OBJECT_UNITS_PER_METRE, bz, ThreeDRailsTall());
	if (!ThreeDCatenaryOn(south)) return;
	TileInfo deck = *ti;
	deck.tileh = SLOPE_FLAT;
	deck.z = bz;
	DrawThreeDCatenary(&deck, TrackBits{AxisToTrack(axis)}, SLOPE_FLAT, false, eye, DiagDirection::Invalid, false);
}

/**
 * Our 3D track on a railway tunnel's end (the player, 8. 11.: "finish the 3D track"): a straight piece the tile long,
 * level with the tunnel's floor into its portal; its catenary over it, no mast where the portal stands at its edge on
 * the north (the tile before it has one at its outer edge). In the tunnel from the eyes its look along the tube
 * (CabDrawTube() of viewport.cpp).
 * @return whether it was drawn so: the tunnel's ground without the game's rails
 */
bool DrawThreeDTrackTunnel(const TileInfo *ti)
{
	const bool eye = ViewportDrawingEye();
	if (!eye && !ViewportDrawingTurned() && !ViewportDrawingNorthObjects()) return false;
	if (!ThreeDTrackShownOn(ti->tile)) return false;
	const DiagDirection dir = GetTunnelBridgeDirection(ti->tile);
	const Axis axis = DiagDirToAxis(dir);
	const double x = TileX(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0, y = TileY(ti->tile) * TILE_SIZE + TILE_SIZE / 2.0;
	PutThreeDStraight(ti, ThreeDStraightName(GetRailType(ti->tile)), axis, x, y, ti->z, axis == Axis::X ? 0.0f : 90.0f, TILE_SIZE / 2.0, THREE_D_TRACK_HALF_M * OBJECT_UNITS_PER_METRE, ti->z, ThreeDRailsTall());
	if (!ThreeDCatenaryOn(ti->tile)) return true;
	TileInfo floor = *ti;
	floor.tileh = SLOPE_FLAT;
	DrawThreeDCatenary(&floor, TrackBits{AxisToTrack(axis)}, SLOPE_FLAT, false, eye, DiagDirection::Invalid, true, dir);
	return true;
}

/**
 * Draw ground sprite and track bits
 * @param ti TileInfo
 * @param track TrackBits to draw
 */
static void DrawTrackBits(TileInfo *ti, TrackBits track)
{
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(ti->tile));

	/* Ours in 3D: the ground and our pieces of track, along the arcs of a round curve too, no sprite of the track
	 * (DrawThreeDTrack()). */
	if (DrawThreeDTrack(ti, track)) return;

	/* Part of a round curve (round_track.h): the ground only; the track is drawn along its arc after the ground. */
	if (RoundTrackDrawing() && IsRoundTrackTile(ti->tile)) {
		DrawRoundTrackGround(ti->tile);
		RegisterRoundTrackTile(ti->tile);
		return;
	}

	if (rti->UsesOverlay()) {
		DrawTrackBitsOverlay(ti, track, rti);
		return;
	}

	RailGroundType rgt = GetRailGroundType(ti->tile);
	Foundation f = GetRailFoundation(ti->tileh, track);
	Corner halftile_corner = CORNER_INVALID;

	if (IsNonContinuousFoundation(f)) {
		/* Save halftile corner */
		halftile_corner = (f == Foundation::SteepBoth ? GetHighestSlopeCorner(ti->tileh) : GetHalftileFoundationCorner(f));
		/* Draw lower part first */
		track.Reset(CornerToTrackBits(halftile_corner));
		f = (f == Foundation::SteepBoth ? Foundation::SteepLower : Foundation::None);
	}

	DrawFoundation(ti, f);
	/* DrawFoundation modifies ti */

	SpriteID image;
	PaletteID pal = PAL_NONE;
	const SubSprite *sub = nullptr;
	bool junction = false;

	/* Select the sprite to use. */
	if (track.None()) {
		/* Clear ground (only track on halftile foundation) */
		if (rgt == RailGroundType::HalfTileWater) {
			if (IsSteepSlope(ti->tileh)) {
				DrawShoreTile(ti->tileh);
				image = 0;
			} else {
				image = SPR_FLAT_WATER_TILE;
			}
		} else {
			switch (rgt) {
				case RailGroundType::Barren: image = SPR_FLAT_BARE_LAND; break;
				case RailGroundType::SnowOrDesert: image = SPR_FLAT_SNOW_DESERT_TILE; break;
				default: image = SPR_FLAT_GRASS_TILE; break;
			}
			image += SlopeToSpriteOffset(ti->tileh);
		}
	} else {
		if (ti->tileh != SLOPE_FLAT) {
			/* track on non-flat ground */
			image = _track_sloped_sprites[ti->tileh - 1] + rti->base_sprites.track_y;
		} else {
			/* track on flat ground */
			switch (track.base()) {
				/* single track, select combined track + ground sprite*/
				case TrackBits{Track::Y}.base(): image = rti->base_sprites.track_y; break;
				case TrackBits{Track::X}.base(): image = rti->base_sprites.track_y + 1; break;
				case TrackBits{Track::Upper}.base(): image = rti->base_sprites.track_y + 2; break;
				case TrackBits{Track::Lower}.base(): image = rti->base_sprites.track_y + 3; break;
				case TrackBits{Track::Right}.base(): image = rti->base_sprites.track_y + 4; break;
				case TrackBits{Track::Left}.base(): image = rti->base_sprites.track_y + 5; break;
				case TRACK_BIT_CROSS.base(): image = rti->base_sprites.track_y + 6; break;

				/* double diagonal track, select combined track + ground sprite*/
				case TRACK_BIT_HORZ.base(): image = rti->base_sprites.track_ns; break;
				case TRACK_BIT_VERT.base(): image = rti->base_sprites.track_ns + 1; break;

				/* junction, select only ground sprite, handle track sprite later */
				default:
					junction = true;
					image = rti->base_sprites.ground + GetJunctionGroundSpriteOffset(track);
					break;
			}
		}

		switch (rgt) {
			case RailGroundType::Barren: pal = PALETTE_TO_BARE_LAND; break;
			case RailGroundType::SnowOrDesert: image += rti->snow_offset; break;
			case RailGroundType::HalfTileWater: {
				/* three-corner-raised slope */
				DrawShoreTile(ti->tileh);
				Corner track_corner = OppositeCorner(GetHighestSlopeCorner(ComplementSlope(ti->tileh)));
				sub = &(_halftile_sub_sprite[track_corner]);
				break;
			}
			default: break;
		}
	}

	if (image != 0) DrawGroundSprite(image, pal, sub);

	/* Draw track pieces individually for junction tiles */
	if (junction) {
		if (track.Test(Track::X))     DrawGroundSprite(rti->base_sprites.single_x, PAL_NONE);
		if (track.Test(Track::Y))     DrawGroundSprite(rti->base_sprites.single_y, PAL_NONE);
		if (track.Test(Track::Upper)) DrawGroundSprite(rti->base_sprites.single_n, PAL_NONE);
		if (track.Test(Track::Lower)) DrawGroundSprite(rti->base_sprites.single_s, PAL_NONE);
		if (track.Test(Track::Left))  DrawGroundSprite(rti->base_sprites.single_w, PAL_NONE);
		if (track.Test(Track::Right)) DrawGroundSprite(rti->base_sprites.single_e, PAL_NONE);
	}

	/* PBS debugging, draw reserved tracks darker */
	if (_game_mode != GameMode::Menu && _settings_client.gui.show_track_reservation) {
		/* Get reservation, but mask track on halftile slope */
		TrackBits pbs = GetRailReservationTrackBits(ti->tile) & track;
		if (pbs.Test(Track::X)) {
			if (ti->tileh == SLOPE_FLAT || ti->tileh == SLOPE_ELEVATED) {
				DrawGroundSprite(rti->base_sprites.single_x, PALETTE_CRASH);
			} else {
				DrawGroundSprite(_track_sloped_sprites[ti->tileh - 1] + rti->base_sprites.single_sloped - 20, PALETTE_CRASH);
			}
		}
		if (pbs.Test(Track::Y)) {
			if (ti->tileh == SLOPE_FLAT || ti->tileh == SLOPE_ELEVATED) {
				DrawGroundSprite(rti->base_sprites.single_y, PALETTE_CRASH);
			} else {
				DrawGroundSprite(_track_sloped_sprites[ti->tileh - 1] + rti->base_sprites.single_sloped - 20, PALETTE_CRASH);
			}
		}
		if (pbs.Test(Track::Upper)) DrawGroundSprite(rti->base_sprites.single_n, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_N ? -(int)TILE_HEIGHT : 0);
		if (pbs.Test(Track::Lower)) DrawGroundSprite(rti->base_sprites.single_s, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_S ? -(int)TILE_HEIGHT : 0);
		if (pbs.Test(Track::Left))  DrawGroundSprite(rti->base_sprites.single_w, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_W ? -(int)TILE_HEIGHT : 0);
		if (pbs.Test(Track::Right)) DrawGroundSprite(rti->base_sprites.single_e, PALETTE_CRASH, nullptr, 0, ti->tileh & SLOPE_E ? -(int)TILE_HEIGHT : 0);
	}

	if (IsValidCorner(halftile_corner)) {
		DrawFoundation(ti, HalftileFoundation(halftile_corner));

		/* Draw higher halftile-overlay: Use the sloped sprites with three corners raised. They probably best fit the lightning. */
		Slope fake_slope = SlopeWithThreeCornersRaised(OppositeCorner(halftile_corner));
		image = _track_sloped_sprites[fake_slope - 1] + rti->base_sprites.track_y;
		pal = PAL_NONE;
		switch (rgt) {
			case RailGroundType::Barren: pal = PALETTE_TO_BARE_LAND; break;
			case RailGroundType::SnowOrDesert:
			case RailGroundType::HalfTileSnow: image += rti->snow_offset; break; // higher part has snow in this case too
			default: break;
		}
		DrawGroundSprite(image, pal, &(_halftile_sub_sprite[halftile_corner]));

		if (_game_mode != GameMode::Menu && _settings_client.gui.show_track_reservation && HasReservedTracks(ti->tile, CornerToTrackBits(halftile_corner))) {
			static constexpr CornerIndexArray<uint8_t> corner_to_track_sprite = {3, 1, 2, 0};
			DrawGroundSprite(corner_to_track_sprite[halftile_corner] + rti->base_sprites.single_n, PALETTE_CRASH, nullptr, 0, -static_cast<int>(TILE_HEIGHT));
		}
	}
}

/**
 * Draw the signal on a rail tunnel's or bridge's portal.
 *
 * At the mouth, facing the way a train goes in, which is the only place there
 * is: on a bridge nothing can be drawn between the ramps, so a bore divided
 * into stretches keeps those divisions to itself and shows only its ends.
 *
 * @param ti the tile being drawn
 */
void DrawTunnelBridgePortalSignal(const TileInfo *ti)
{
	if (!IsTunnelBridgeSignalled(ti->tile)) return;
	if (GetTunnelBridgeTransportType(ti->tile) != TransportType::Rail) return;
	if (!HasTunnelBridgeSignal(ti->tile, TunnelBridgeSignal::Entry)) return;

	/* By the direction a train takes going in: the same picture and the same
	 * corner of the tile that a signal for that trackdir gets on plain rail
	 * (see DrawSignals()). */
	static const DiagDirectionIndexArray<SignalOffsets> images{{{SIGNAL_TO_NORTHEAST, SIGNAL_TO_SOUTHEAST, SIGNAL_TO_SOUTHWEST, SIGNAL_TO_NORTHWEST}}};
	static const DiagDirectionIndexArray<uint8_t> positions{{{9, 10, 8, 11}}};

	DiagDirection dir = GetTunnelBridgeDirection(ti->tile);
	Track track = DiagDirToDiagTrack(dir);
	Trackdir td = TrackExitdirToTrackdir(track, dir);
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(ti->tile));
	SignalState state = GetTunnelBridgeSignalState(ti->tile, TunnelBridgeSignal::Entry);

	/* Drawn as the kind the player was building when it was put here, so a
	 * line of path signals keeps its look across a bridge. Which kind it is
	 * changes nothing about the bore. */
	DrawSignalSprite(ti->tile, rti, track, GetTunnelBridgeSignalType(ti->tile), GetTunnelBridgeSignalVariant(ti->tile), state, images[dir], positions[dir], td);
}

/** Draw one signal of a plain rail tile, reading its kind off the tile. */
static void DrawSingleSignal(TileIndex tile, const RailTypeInfo *rti, Track track, SignalState condition, SignalOffsets image, uint pos, Trackdir td)
{
	DrawSignalSprite(tile, rti, track, GetSignalType(tile, track), GetSignalVariant(tile, track), condition, image, pos, td);
}

static void DrawSignals(TileIndex tile, TrackBits rails, const RailTypeInfo *rti)
{
	auto MAYBE_DRAW_SIGNAL = [&](uint8_t signalbit, SignalOffsets image, uint pos, Track track) {
		if (!IsSignalPresent(tile, signalbit)) return;
		/* Which way this signal faces, as a trackdir: the warning aspect has
		 * to read the road beyond it, and only a trackdir says which way that
		 * is. Of the track's two, it is the one this signal sits along. */
		Trackdir td = TrackToTrackdir(track);
		if (!HasBit(SignalAlongTrackdir(td), signalbit)) td = ReverseTrackdir(td);
		DrawSingleSignal(tile, rti, track, GetSingleSignalState(tile, signalbit), image, pos, td);
	};

	if (!rails.Test(Track::Y)) {
		if (!rails.Test(Track::X)) {
			if (rails.Test(Track::Left)) {
				MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_NORTH, 0, Track::Left);
				MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_SOUTH, 1, Track::Left);
			}
			if (rails.Test(Track::Right)) {
				MAYBE_DRAW_SIGNAL(0, SIGNAL_TO_NORTH, 2, Track::Right);
				MAYBE_DRAW_SIGNAL(1, SIGNAL_TO_SOUTH, 3, Track::Right);
			}
			if (rails.Test(Track::Upper)) {
				MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_WEST, 4, Track::Upper);
				MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_EAST, 5, Track::Upper);
			}
			if (rails.Test(Track::Lower)) {
				MAYBE_DRAW_SIGNAL(1, SIGNAL_TO_WEST, 6, Track::Lower);
				MAYBE_DRAW_SIGNAL(0, SIGNAL_TO_EAST, 7, Track::Lower);
			}
		} else {
			MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_SOUTHWEST, 8, Track::X);
			MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_NORTHEAST, 9, Track::X);
		}
	} else {
		MAYBE_DRAW_SIGNAL(3, SIGNAL_TO_SOUTHEAST, 10, Track::Y);
		MAYBE_DRAW_SIGNAL(2, SIGNAL_TO_NORTHWEST, 11, Track::Y);
	}
}

/** @copydoc DrawTileProc */
static void DrawTile_Rail(TileInfo *ti)
{
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(ti->tile));
	BridgePillarFlags blocked_pillars{};
	PaletteID pal = GetCompanyPalette(GetTileOwner(ti->tile));

	if (IsPlainRail(ti->tile)) {
		TrackBits rails = GetTrackBits(ti->tile);

		DrawTrackBits(ti, rails);

		if (_display_opt.Test(DisplayOption::FullDetail)) DrawTrackDetails(ti, rti, pal);

		/* Not over a round curve: its wires would hang over the straight pieces, not the arc (a switch keeps them, over its straight way). */
		if (HasRailCatenaryDrawn(GetRailType(ti->tile)) && !(RoundTrackDrawing() && RoundTrackHidesCatenary(ti->tile))) DrawRailCatenary(ti);

		if (HasSignals(ti->tile)) DrawSignals(ti->tile, rails, rti);

		if (IsBridgeAbove(ti->tile)) {
			if (rails.Any(TRACK_BIT_3WAY_NE)) blocked_pillars.Set(BridgePillarFlag::EdgeNE);
			if (rails.Any(TRACK_BIT_3WAY_SE)) blocked_pillars.Set(BridgePillarFlag::EdgeSE);
			if (rails.Any(TRACK_BIT_3WAY_SW)) blocked_pillars.Set(BridgePillarFlag::EdgeSW);
			if (rails.Any(TRACK_BIT_3WAY_NW)) blocked_pillars.Set(BridgePillarFlag::EdgeNW);
		}
	} else {
		/* draw depot */
		const DrawTileSprites *dts;
		DiagDirection dir = GetRailDepotDirection(ti->tile);

		if (ti->tileh != SLOPE_FLAT) DrawFoundation(ti, Foundation::Leveled);

		/* Our 3D track from its door to its middle (DrawThreeDTrackOn()): its ground plain, no track of the game's. */
		const bool three_d = DrawThreeDTrackOn(ti, DiagDirToAxis(dir), dir, false);

		if (IsInvisibilitySet(TransparencyOption::Buildings)) {
			/* Draw rail instead of depot */
			dts = &_depot_invisible_gfx_table[dir];
		} else {
			dts = &_depot_gfx_table[dir];
		}

		SpriteID image;
		if (three_d || rti->UsesOverlay()) {
			image = SPR_FLAT_GRASS_TILE;
		} else {
			image = dts->ground.sprite;
			if (image != SPR_FLAT_GRASS_TILE) image += rti->GetRailtypeSpriteOffset();
		}

		/* Adjust ground tile for desert and snow. */
		if (IsSnowOrDesertRailGround(ti->tile)) {
			if (image != SPR_FLAT_GRASS_TILE) {
				image += rti->snow_offset; // tile with tracks
			} else {
				image = SPR_FLAT_SNOW_DESERT_TILE; // flat ground
			}
		}

		DrawGroundSprite(image, GroundSpritePaletteTransform(image, PAL_NONE, pal));

		if (three_d) {
			/* Its track is ours. */
		} else if (rti->UsesOverlay()) {
			SpriteID ground = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Ground);

			switch (GetRailDepotDirection(ti->tile)) {
				case DiagDirection::NE:
					if (!IsInvisibilitySet(TransparencyOption::Buildings)) break;
					[[fallthrough]];
				case DiagDirection::SW:
					DrawGroundSprite(ground + RTO_X, PAL_NONE);
					break;
				case DiagDirection::NW:
					if (!IsInvisibilitySet(TransparencyOption::Buildings)) break;
					[[fallthrough]];
				case DiagDirection::SE:
					DrawGroundSprite(ground + RTO_Y, PAL_NONE);
					break;
				default:
					break;
			}

			if (_settings_client.gui.show_track_reservation && HasDepotReservation(ti->tile)) {
				SpriteID overlay = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Overlay);

				switch (GetRailDepotDirection(ti->tile)) {
					case DiagDirection::NE:
						if (!IsInvisibilitySet(TransparencyOption::Buildings)) break;
						[[fallthrough]];
					case DiagDirection::SW:
						DrawGroundSprite(overlay + RTO_X, PALETTE_CRASH);
						break;
					case DiagDirection::NW:
						if (!IsInvisibilitySet(TransparencyOption::Buildings)) break;
						[[fallthrough]];
					case DiagDirection::SE:
						DrawGroundSprite(overlay + RTO_Y, PALETTE_CRASH);
						break;
					default:
						break;
				}
			}
		} else {
			/* PBS debugging, draw reserved tracks darker */
			if (_game_mode != GameMode::Menu && _settings_client.gui.show_track_reservation && HasDepotReservation(ti->tile)) {
				switch (GetRailDepotDirection(ti->tile)) {
					case DiagDirection::NE:
						if (!IsInvisibilitySet(TransparencyOption::Buildings)) break;
						[[fallthrough]];
					case DiagDirection::SW:
						DrawGroundSprite(rti->base_sprites.single_x, PALETTE_CRASH);
						break;
					case DiagDirection::NW:
						if (!IsInvisibilitySet(TransparencyOption::Buildings)) break;
						[[fallthrough]];
					case DiagDirection::SE:
						DrawGroundSprite(rti->base_sprites.single_y, PALETTE_CRASH);
						break;
					default:
						break;
				}
			}
		}
		int depot_sprite = GetCustomRailSprite(rti, ti->tile, RailSpriteType::Depot);
		int relocation = depot_sprite != 0 ? depot_sprite - SPR_RAIL_DEPOT_SE_1 : rti->GetRailtypeSpriteOffset();

		if (HasRailCatenaryDrawn(GetRailType(ti->tile))) DrawRailCatenary(ti);

		DrawRailTileSeq(ti, dts, TransparencyOption::Buildings, relocation, 0, pal);
		blocked_pillars = BRIDGEPILLARFLAGS_ALL;
	}
	DrawBridgeMiddle(ti, blocked_pillars);
}

/**
 * Draw train depot sprite in the UI.
 * @param x X position.
 * @param y Y position.
 * @param dir Direction of depot.
 * @param railtype Railtype of depot.
 */
void DrawTrainDepotSprite(int x, int y, DiagDirection dir, RailType railtype)
{
	const DrawTileSprites *dts = &_depot_gfx_table[dir];
	const RailTypeInfo *rti = GetRailTypeInfo(railtype);
	SpriteID image = rti->UsesOverlay() ? SPR_FLAT_GRASS_TILE : dts->ground.sprite;
	uint32_t offset = rti->GetRailtypeSpriteOffset();

	if (image != SPR_FLAT_GRASS_TILE) image += offset;
	PaletteID palette = GetCompanyPalette(_local_company);

	DrawSprite(image, PAL_NONE, x, y);

	if (rti->UsesOverlay()) {
		SpriteID ground = GetCustomRailSprite(rti, INVALID_TILE, RailSpriteType::Ground);

		switch (dir) {
			case DiagDirection::SW: DrawSprite(ground + RTO_X, PAL_NONE, x, y); break;
			case DiagDirection::SE: DrawSprite(ground + RTO_Y, PAL_NONE, x, y); break;
			default: break;
		}
	}
	int depot_sprite = GetCustomRailSprite(rti, INVALID_TILE, RailSpriteType::Depot);
	if (depot_sprite != 0) offset = depot_sprite - SPR_RAIL_DEPOT_SE_1;

	DrawRailTileSeqInGUI(x, y, dts, offset, 0, palette);
}

/** @copydoc GetSlopePixelZProc */
static int GetSlopePixelZ_Rail(TileIndex tile, uint x, uint y, [[maybe_unused]] bool ground_vehicle)
{
	if (IsPlainRail(tile)) {
		auto [tileh, z] = GetTilePixelSlope(tile);
		if (tileh == SLOPE_FLAT) return z;

		z += ApplyPixelFoundationToSlope(GetRailFoundation(tileh, GetTrackBits(tile)), tileh);
		return z + GetPartialPixelZ(x & 0xF, y & 0xF, tileh);
	} else {
		return GetTileMaxPixelZ(tile);
	}
}

/** @copydoc GetFoundationProc */
static Foundation GetFoundation_Rail(TileIndex tile, Slope tileh)
{
	return IsPlainRail(tile) ? GetRailFoundation(tileh, GetTrackBits(tile)) : FlatteningFoundation(tileh);
}

/** @copydoc TileLoopProc */
static void TileLoop_Rail(TileIndex tile)
{
	RailGroundType old_ground = GetRailGroundType(tile);
	RailGroundType new_ground;

	if (old_ground == RailGroundType::HalfTileWater) {
		TileLoop_Water(tile);
		return;
	}

	switch (SnowLandscape()) {
		case LandscapeType::Arctic: {
			auto [slope, z] = GetTileSlopeZ(tile);
			bool half = false;

			/* for non-flat track, use lower part of track
			 * in other cases, use the highest part with track */
			if (IsPlainRail(tile)) {
				TrackBits track = GetTrackBits(tile);
				Foundation f = GetRailFoundation(slope, track);

				switch (f) {
					case Foundation::None:
						/* no foundation - is the track on the upper side of three corners raised tile? */
						if (IsSlopeWithThreeCornersRaised(slope)) z++;
						break;

					case Foundation::InclinedX:
					case Foundation::InclinedY:
						/* sloped track - is it on a steep slope? */
						if (IsSteepSlope(slope)) z++;
						break;

					case Foundation::SteepLower:
						/* only lower part of steep slope */
						z++;
						break;

					default:
						/* if it is a steep slope, then there is a track on higher part */
						if (IsSteepSlope(slope)) z++;
						z++;
						break;
				}

				half = IsNonContinuousFoundation(f);
			} else {
				/* is the depot on a non-flat tile? */
				if (slope != SLOPE_FLAT) z++;
			}

			/* 'z' is now the lowest part of the highest track bit -
			 * for sloped track, it is 'z' of lower part
			 * for two track bits, it is 'z' of higher track bit
			 * For non-continuous foundations (and STEEP_BOTH), 'half' is set */
			if (z > GetSnowLine()) {
				if (half && z - GetSnowLine() == 1) {
					/* track on non-continuous foundation, lower part is not under snow */
					new_ground = RailGroundType::HalfTileSnow;
				} else {
					new_ground = RailGroundType::SnowOrDesert;
				}
				goto set_ground;
			}
			break;
			}

		case LandscapeType::Tropic:
			if (GetTropicZone(tile) == TropicZone::Desert) {
				new_ground = RailGroundType::SnowOrDesert;
				goto set_ground;
			}
			break;

		default:
			break;
	}

	new_ground = RailGroundType::Grass;

	if (IsPlainRail(tile) && old_ground != RailGroundType::Barren) { // wait until bottom is green
		/* determine direction of fence */
		TrackBits rail = GetTrackBits(tile);

		Owner owner = GetTileOwner(tile);
		DiagDirections fences{};

		for (DiagDirection d : EnumRange(DiagDirection::End)) {
			static constexpr DiagDirectionIndexArray<TrackBits> dir_to_trackbits{TRACK_BIT_3WAY_NE, TRACK_BIT_3WAY_SE, TRACK_BIT_3WAY_SW, TRACK_BIT_3WAY_NW};

			/* Track bit on this edge => no fence. */
			if (rail.Any(dir_to_trackbits[d])) continue;

			TileIndex tile2 = tile + TileOffsByDiagDir(d);

			/* Show fences if it's a house, industry, object, road, tunnelbridge or not owned by us. */
			if (!IsValidTile(tile2) || IsTileType(tile2, TileType::House) || IsTileType(tile2, TileType::Industry) ||
					IsTileType(tile2, TileType::Road) || (IsTileType(tile2, TileType::Object) && !IsObjectType(tile2, OBJECT_OWNED_LAND)) || IsTileType(tile2, TileType::TunnelBridge) || !IsTileOwner(tile2, owner)) {
				fences.Set(d);
			}
		}

		switch (fences.base()) {
			case DiagDirections{}.base(): break;
			case DiagDirections{DiagDirection::NE}.base(): new_ground = RailGroundType::FenceNE; break;
			case DiagDirections{DiagDirection::SE}.base(): new_ground = RailGroundType::FenceSE; break;
			case DiagDirections{DiagDirection::SW}.base(): new_ground = RailGroundType::FenceSW; break;
			case DiagDirections{DiagDirection::NW}.base(): new_ground = RailGroundType::FenceNW; break;
			case DiagDirections{DiagDirection::NE, DiagDirection::SW}.base(): new_ground = RailGroundType::FenceNESW; break;
			case DiagDirections{DiagDirection::SE, DiagDirection::NW}.base(): new_ground = RailGroundType::FenceSENW; break;
			case DiagDirections{DiagDirection::NE, DiagDirection::SE}.base(): new_ground = RailGroundType::FenceVert1; break;
			case DiagDirections{DiagDirection::NE, DiagDirection::NW}.base(): new_ground = RailGroundType::FenceHoriz2; break;
			case DiagDirections{DiagDirection::SE, DiagDirection::SW}.base(): new_ground = RailGroundType::FenceHoriz1; break;
			case DiagDirections{DiagDirection::SW, DiagDirection::NW}.base(): new_ground = RailGroundType::FenceVert2; break;
			default: NOT_REACHED();
		}
	}

set_ground:
	if (old_ground != new_ground) {
		SetRailGroundType(tile, new_ground);
		MarkTileDirtyByTile(tile);
	}
}


/** @copydoc GetTileTrackStatusProc */
static TrackStatus GetTileTrackStatus_Rail(TileIndex tile, TransportType mode, [[maybe_unused]] RoadTramType sub_mode, DiagDirection side)
{
	/* Case of half tile slope with water. */
	if (mode == TransportType::Water && IsPlainRail(tile) && GetRailGroundType(tile) == RailGroundType::HalfTileWater && IsSlopeWithOneCornerRaised(GetTileSlope(tile))) {
		TrackBits tb = GetTrackBits(tile);
		switch (tb.base()) {
			default: NOT_REACHED();
			case TrackBits{Track::Upper}.base(): tb = Track::Lower; break;
			case TrackBits{Track::Lower}.base(): tb = Track::Upper; break;
			case TrackBits{Track::Left}.base():  tb = Track::Right; break;
			case TrackBits{Track::Right}.base(): tb = Track::Left; break;
		}
		return {TrackBitsToTrackdirBits(tb), {}};
	}

	if (mode != TransportType::Rail) return {};

	TrackBits trackbits{};
	TrackdirBits red_signals{};

	switch (GetRailTileType(tile)) {
		default: NOT_REACHED();
		case RailTileType::Normal:
			trackbits = GetTrackBits(tile);
			break;

		case RailTileType::Signals: {
			trackbits = GetTrackBits(tile);
			uint8_t a = GetPresentSignals(tile);
			uint b = GetSignalStates(tile);

			b &= a;

			/* When signals are not present (in neither direction),
			 * we pretend them to be green. Otherwise, it depends on
			 * the signal type. For signals that are only active from
			 * one side, we set the missing signals explicitly to
			 * `green'. Otherwise, they implicitly become `red'. */
			if (!IsOnewaySignal(tile, Track::Upper) || (a & SignalOnTrack(Track::Upper)) == 0) b |= ~a & SignalOnTrack(Track::Upper);
			if (!IsOnewaySignal(tile, Track::Lower) || (a & SignalOnTrack(Track::Lower)) == 0) b |= ~a & SignalOnTrack(Track::Lower);

			if ((b & 0x8) == 0) red_signals.Set({Trackdir::Left_N, Trackdir::X_NE, Trackdir::Y_SE, Trackdir::Upper_E});
			if ((b & 0x4) == 0) red_signals.Set({Trackdir::Left_S, Trackdir::X_SW, Trackdir::Y_NW, Trackdir::Upper_W});
			if ((b & 0x2) == 0) red_signals.Set({Trackdir::Right_N, Trackdir::Lower_E});
			if ((b & 0x1) == 0) red_signals.Set({Trackdir::Right_S, Trackdir::Lower_W});

			break;
		}

		case RailTileType::Depot: {
			DiagDirection dir = GetRailDepotDirection(tile);

			if (side != DiagDirection::Invalid && side != dir) break;

			trackbits = DiagDirToDiagTrack(dir);
			break;
		}
	}

	return {TrackBitsToTrackdirBits(trackbits), red_signals};
}

/** @copydoc ClickTileProc */
static bool ClickTile_Rail(TileIndex tile)
{
	if (!IsRailDepot(tile)) return false;

	ShowDepotWindow(tile, VehicleType::Train);
	return true;
}

/** @copydoc GetTileDescProc */
static void GetTileDesc_Rail(TileIndex tile, TileDesc &td)
{
	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
	td.rail_speed = rti->max_speed;
	td.railtype = rti->strings.name;
	td.owner[0] = GetTileOwner(tile);
	switch (GetRailTileType(tile)) {
		case RailTileType::Normal:
			td.str = STR_LAI_RAIL_DESCRIPTION_TRACK;
			break;

		case RailTileType::Signals: {
			static constexpr EnumIndexArray<EnumIndexArray<StringID, SignalType, SignalType::End>, SignalType, SignalType::End> signal_type{{{
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_SIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PRESIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_NOENTRYSIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PRESIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRESIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_NOENTRYSIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXITSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_NOENTRYSIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBOSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_NOENTRYSIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBSSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_NOENTRYSIGNALS
				},
				{
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NORMAL_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PRE_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_EXIT_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_COMBO_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_PBS_NOENTRYSIGNALS,
					STR_LAI_RAIL_DESCRIPTION_TRACK_WITH_NOENTRYSIGNALS
				},
			}}};

			SignalType primary_signal;
			SignalType secondary_signal;
			if (HasSignalOnTrack(tile, Track::Upper)) {
				primary_signal = GetSignalType(tile, Track::Upper);
				secondary_signal = HasSignalOnTrack(tile, Track::Lower) ? GetSignalType(tile, Track::Lower) : primary_signal;
			} else {
				secondary_signal = primary_signal = GetSignalType(tile, Track::Lower);
			}

			td.str = signal_type[secondary_signal][primary_signal];
			break;
		}

		case RailTileType::Depot:
			td.str = STR_LAI_RAIL_DESCRIPTION_TRAIN_DEPOT;
			if (_settings_game.vehicle.train_acceleration_model != AccelerationModel::Original) {
				if (td.rail_speed > 0) {
					td.rail_speed = std::min<uint16_t>(td.rail_speed, 61);
				} else {
					td.rail_speed = 61;
				}
			}
			td.build_date = Depot::GetByTile(tile)->build_date;
			break;

		default:
			NOT_REACHED();
	}
}

/** @copydoc ChangeTileOwnerProc */
static void ChangeTileOwner_Rail(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (!IsTileOwner(tile, old_owner)) return;

	if (new_owner != INVALID_OWNER) {
		/* Update company infrastructure counts. No need to dirty windows here, we'll redraw the whole screen anyway. */
		uint num_pieces = 1;
		if (IsPlainRail(tile)) {
			TrackBits bits = GetTrackBits(tile);
			num_pieces = bits.Count();
			if (TracksOverlap(bits)) num_pieces *= num_pieces;
		}
		RailType rt = GetRailType(tile);
		Company::Get(old_owner)->infrastructure.rail[rt] -= num_pieces;
		Company::Get(new_owner)->infrastructure.rail[rt] += num_pieces;

		if (HasSignals(tile)) {
			uint num_sigs = CountBits(GetPresentSignals(tile));
			Company::Get(old_owner)->infrastructure.signal -= num_sigs;
			Company::Get(new_owner)->infrastructure.signal += num_sigs;
		}

		SetTileOwner(tile, new_owner);
	} else {
		Command<Commands::LandscapeClear>::Do({DoCommandFlag::Execute, DoCommandFlag::Bankrupt}, tile);
	}
}

/** Coordinates to detect when a train is approaching a depot from behind for each depot direction. */
static constexpr DiagDirectionIndexArray<Coord2D<uint8_t>> _fractcoords_behind{{{
	{15, 8}, // NE
	{8, 0}, // SE
	{0, 8}, // SW
	{8, 15}, // NW
}}};

/** Coordinates where a train should enter a depot for each depot direction. */
static constexpr DiagDirectionIndexArray<Coord2D<uint8_t>> _fractcoords_enter{{{
	{10, 8}, // NE
	{8, 4}, // SE
	{4, 8}, // SW
	{8, 10}, // NW
}}};

/**
 * Offsets (to be multiplied by length) from the depot enter coordinates where
 * a train should appear when exiting a depot for each depot direction.
 */
static constexpr DiagDirectionIndexArray<Coord2D<int8_t>> _deltacoord_leaveoffset{{{
	{-1, 0}, // NE
	{0, 1}, // SE
	{1, 0}, // SW
	{0, -1}, // NW
}}};

/**
 * Compute number of ticks when next wagon will leave a depot.
 * Negative means next wagon should have left depot n ticks before.
 * @param v vehicle outside (leaving) the depot
 * @return number of ticks when the next wagon will leave
 */
int TicksToLeaveDepot(const Train *v)
{
	DiagDirection dir = GetRailDepotDirection(v->tile);
	int length = v->CalcNextVehicleOffset() + 1;

	switch (dir) {
		case DiagDirection::NE: return  (static_cast<int>(v->x_pos & TILE_UNIT_MASK) - (_fractcoords_enter[dir].x - length));
		case DiagDirection::SE: return -(static_cast<int>(v->y_pos & TILE_UNIT_MASK) - (_fractcoords_enter[dir].y + length));
		case DiagDirection::SW: return -(static_cast<int>(v->x_pos & TILE_UNIT_MASK) - (_fractcoords_enter[dir].x + length));
		case DiagDirection::NW: return  (static_cast<int>(v->y_pos & TILE_UNIT_MASK) - (_fractcoords_enter[dir].y - length));
		default: NOT_REACHED();
	}
}

/** @copydoc VehicleEnterTileProc */
static VehicleEnterTileStates VehicleEnterTile_Rail(Vehicle *v, TileIndex tile, int x, int y)
{
	/* This routine applies only to trains in depot tiles. */
	if (v->type != VehicleType::Train || !IsRailDepotTile(tile)) return {};

	/* Depot direction. */
	DiagDirection dir = GetRailDepotDirection(tile);

	Coord2D<uint8_t> fract_coord{
		static_cast<uint8_t>(x & TILE_UNIT_MASK),
		static_cast<uint8_t>(y & TILE_UNIT_MASK)};

	/* Make sure a train is not entering the tile from behind. */
	if (_fractcoords_behind[dir] == fract_coord) return VehicleEnterTileState::CannotEnter;

	/* Leaving depot? */
	if (v->GetMovingDirection() == DiagDirToDir(dir)) {
		/* Calculate the point where the following wagon should be activated. */
		int length = Train::From(v)->CalcNextVehicleOffset() + 1;

		Coord2D<uint8_t> fract_coord_leave{
			static_cast<uint8_t>(_fractcoords_enter[dir].x + length * _deltacoord_leaveoffset[dir].x),
			static_cast<uint8_t>(_fractcoords_enter[dir].y + length * _deltacoord_leaveoffset[dir].y)};

		if (fract_coord_leave == fract_coord) {
			/* Leave the depot. */
			if ((v = v->GetMovingNext()) != nullptr) {
				v->vehstatus.Reset(VehState::Hidden);
				Train::From(v)->track = AxisToTrack(DiagDirToAxis(dir));
			}
		}
	} else if (_fractcoords_enter[dir] == fract_coord) {
		/* Entering depot. */
		assert(DiagDirToDir(ReverseDiagDir(dir)) == v->GetMovingDirection());
		Train::From(v)->track = Track::Depot;
		v->vehstatus.Set(VehState::Hidden);
		v->tile = tile;
		/* The train is parked when the last of it is in -- which used to be
		 * read as "this vehicle has nobody behind it". That is the same thing
		 * for a train that drives in the ordinary way, and a different thing
		 * for one turned round in the doorway on its way out: the vehicles
		 * that never got out are behind the ones driving back in, so the last
		 * to come through the door has a whole hidden tail behind it, and the
		 * one with nobody behind it is standing in here already -- hidden at
		 * the door point, where every parked vehicle stands, and where turning
		 * the train round asks this of it (UpdateStatusAfterSwap()). Read that
		 * way the train was parked the moment it was turned, with a wagon
		 * still out on the line. So the question is asked of the whole train. */
		if (IsWholeTrainInsideDepot(Train::From(v))) {
			Train *consist = Train::From(v)->First();
			/* A train comes back out of a depot the same way round it went
			 * in: the end that led the way in leads the way out again, so one
			 * that reversed in reverses out.
			 *
			 * The two outcomes are mirrors of each other and only ever one of
			 * them is done. Reversing every vehicle's facing turns the
			 * direction of travel round and leaves the leading end as it was,
			 * which keeps the train the same way round. Flipping which end
			 * leads turns the direction of travel round on its own and swaps
			 * the ends besides, which turns the train round. Doing both cancels
			 * the direction of travel back to what it was and drives the train
			 * at the dead end.
			 *
			 * A depot holds the whole train on one hidden tile, so it has no
			 * extent there and either end is free to come out first; that is
			 * what makes this a choice rather than something geometry decides.
			 * It does need the leaving code to send out whichever end leads
			 * rather than always the head of the chain, which is why
			 * CheckTrainStayInDepot works from GetMovingFront().
			 *
			 * The engine used to bring every train out facing forwards,
			 * turning round any that had reversed in, on the grounds that it
			 * lets a player straighten out a confused one. But trains visit
			 * depots on their own for scheduled servicing, and one that comes
			 * back the other way round has had a working setup quietly
			 * rearranged. So turning round happens when it is asked for: by the
			 * order, or by hand with the reverse button while the train is
			 * stopped inside. See Order::ShouldTurnAroundInDepot() and
			 * FEATURE_DESIGN_COUPLING_TOW.md. */
			if (consist->current_order.IsType(OT_GOTO_DEPOT) && consist->current_order.ShouldTurnAroundInDepot()) {
				consist->vehicle_flags.Flip(VehicleFlag::DrivingBackwards);
			} else {
				for (Train *u = consist; u != nullptr; u = u->Next()) {
					u->direction = ReverseDir(u->direction);
				}
			}
			/* A train in a depot is parked, not on its way anywhere, so it
			 * holds no path. Whatever it still had reserved when it arrived
			 * has to go back, or it is left behind as track marked taken with
			 * nothing standing on it and nothing coming to release it -- which
			 * is what other trains then follow into the depot.
			 *
			 * Unless what lies outside the door is somebody else's: a
			 * reservation carries no owner, and the walk out of the door
			 * cannot tell a stale leftover from the road a train has this
			 * moment booked to the same door. Freeing that one put two trains
			 * on the mouth tile at speed. See IsDepotDoorBookedByAnother(). */
			if (!IsDepotDoorBookedByAnother(consist)) FreeTrainTrackReservation(consist);

			VehicleEnterDepot(consist);
		}

		InvalidateWindowData(WindowClass::VehicleDepot, v->tile);
		return VehicleEnterTileState::EnteredWormhole;
	}

	return {};
}

/**
 * Tests if autoslope is allowed.
 *
 * @param tile The tile.
 * @param flags Terraform command flags.
 * @param z_old Old TileZ.
 * @param tileh_old Old TileSlope.
 * @param z_new New TileZ.
 * @param tileh_new New TileSlope.
 * @param rail_bits Trackbits.
 * @return The cost of this operation or an error.
 */
static CommandCost TestAutoslopeOnRailTile(TileIndex tile, DoCommandFlags flags, int z_old, Slope tileh_old, int z_new, Slope tileh_new, TrackBits rail_bits)
{
	if (!_settings_game.construction.build_on_slopes || !AutoslopeEnabled()) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);

	/* Is the slope-rail_bits combination valid in general? I.e. is it safe to call GetRailFoundation() ? */
	if (CheckRailSlope(tileh_new, rail_bits, {}, tile).Failed()) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);

	/* Get the slopes on top of the foundations */
	z_old += ApplyFoundationToSlope(GetRailFoundation(tileh_old, rail_bits), tileh_old);
	z_new += ApplyFoundationToSlope(GetRailFoundation(tileh_new, rail_bits), tileh_new);

	Corner track_corner;
	switch (rail_bits.base()) {
		case TrackBits{Track::Left}.base():  track_corner = CORNER_W; break;
		case TrackBits{Track::Lower}.base(): track_corner = CORNER_S; break;
		case TrackBits{Track::Right}.base(): track_corner = CORNER_E; break;
		case TrackBits{Track::Upper}.base(): track_corner = CORNER_N; break;

		/* Surface slope must not be changed */
		default:
			if (z_old != z_new || tileh_old != tileh_new) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);
			return CommandCost(ExpensesType::Construction, _price[Price::BuildFoundation]);
	}

	/* The height of the track_corner must not be changed. The rest ensures GetRailFoundation() already. */
	z_old += GetSlopeZInCorner(RemoveHalftileSlope(tileh_old), track_corner);
	z_new += GetSlopeZInCorner(RemoveHalftileSlope(tileh_new), track_corner);
	if (z_old != z_new) return CommandCost(STR_ERROR_MUST_REMOVE_RAILROAD_TRACK);

	CommandCost cost = CommandCost(ExpensesType::Construction, _price[Price::BuildFoundation]);
	/* Make the ground dirty, if surface slope has changed */
	if (tileh_old != tileh_new) {
		/* If there is flat water on the lower halftile add the cost for clearing it */
		if (GetRailGroundType(tile) == RailGroundType::HalfTileWater && IsSlopeWithOneCornerRaised(tileh_old)) cost.AddCost(_price[Price::ClearWater]);
		if (flags.Test(DoCommandFlag::Execute)) SetRailGroundType(tile, RailGroundType::Barren);
	}
	return  cost;
}

/** @copydoc TerraformTileProc */
static CommandCost TerraformTile_Rail(TileIndex tile, DoCommandFlags flags, int z_new, Slope tileh_new)
{
	auto [tileh_old, z_old] = GetTileSlopeZ(tile);
	if (IsPlainRail(tile)) {
		TrackBits rail_bits = GetTrackBits(tile);
		/* Is there flat water on the lower halftile that must be cleared expensively? */
		bool was_water = (GetRailGroundType(tile) == RailGroundType::HalfTileWater && IsSlopeWithOneCornerRaised(tileh_old));

		/* Allow clearing the water only if there is no ship */
		if (was_water && HasVehicleOnTile(tile, [](const Vehicle *v) {
				return v->type == VehicleType::Ship;
			})) return CommandCost(STR_ERROR_SHIP_IN_THE_WAY);

		/* First test autoslope. However if it succeeds we still have to test the rest, because non-autoslope terraforming is cheaper. */
		CommandCost autoslope_result = TestAutoslopeOnRailTile(tile, flags, z_old, tileh_old, z_new, tileh_new, rail_bits);

		/* When there is only a single horizontal/vertical track, one corner can be terraformed. */
		Corner allowed_corner;
		switch (rail_bits.base()) {
			case TrackBits{Track::Right}.base(): allowed_corner = CORNER_W; break;
			case TrackBits{Track::Upper}.base(): allowed_corner = CORNER_S; break;
			case TrackBits{Track::Left}.base():  allowed_corner = CORNER_E; break;
			case TrackBits{Track::Lower}.base(): allowed_corner = CORNER_N; break;
			default: return autoslope_result;
		}

		Foundation f_old = GetRailFoundation(tileh_old, rail_bits);

		/* Do not allow terraforming if allowed_corner is part of anti-zig-zag foundations */
		if (tileh_old != SLOPE_NS && tileh_old != SLOPE_EW && IsSpecialRailFoundation(f_old)) return autoslope_result;

		/* Everything is valid, which only changes allowed_corner */
		for (Corner corner = (Corner)0; corner < CORNER_END; corner = (Corner)(corner + 1)) {
			if (allowed_corner == corner) continue;
			if (z_old + GetSlopeZInCorner(tileh_old, corner) != z_new + GetSlopeZInCorner(tileh_new, corner)) return autoslope_result;
		}

		/* Make the ground dirty */
		if (flags.Test(DoCommandFlag::Execute)) SetRailGroundType(tile, RailGroundType::Barren);

		/* allow terraforming */
		return CommandCost(ExpensesType::Construction, was_water ? _price[Price::ClearWater] : (Money)0);
	} else if (_settings_game.construction.build_on_slopes && AutoslopeEnabled() &&
			AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, GetRailDepotDirection(tile))) {
		return CommandCost(ExpensesType::Construction, _price[Price::BuildFoundation]);
	}
	return Command<Commands::LandscapeClear>::Do(flags, tile);
}

/** @copydoc CheckBuildAboveProc */
static CommandCost CheckBuildAbove_Rail(TileIndex tile, [[maybe_unused]] DoCommandFlags flags, [[maybe_unused]] Axis axis, int height)
{
	if (IsPlainRail(tile)) return CommandCost();

	int height_diff = GetTileMaxZ(tile) + MINIMAL_DEPOT_BRIDGE_HEIGHT - height;
	if (height_diff <= 0) return CommandCost();
	return CommandCostWithParam(STR_ERROR_BRIDGE_TOO_LOW_FOR_TRAIN_DEPOT, height_diff * TILE_HEIGHT_STEP);
}

/** TileTypeProcs definitions for TileType::Rail tiles. */
extern const TileTypeProcs _tile_type_rail_procs = {
	.draw_tile_proc = DrawTile_Rail,
	.get_slope_pixel_z_proc = GetSlopePixelZ_Rail,
	.clear_tile_proc = ClearTile_Rail,
	.get_tile_desc_proc = GetTileDesc_Rail,
	.get_tile_track_status_proc = GetTileTrackStatus_Rail,
	.click_tile_proc = ClickTile_Rail,
	.tile_loop_proc = TileLoop_Rail,
	.change_tile_owner_proc = ChangeTileOwner_Rail,
	.vehicle_enter_tile_proc = VehicleEnterTile_Rail,
	.get_foundation_proc = GetFoundation_Rail,
	.terraform_tile_proc = TerraformTile_Rail,
	.check_build_above_proc = CheckBuildAbove_Rail,
};
