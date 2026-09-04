/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file pbs.cpp PBS support routines. */

#include "stdafx.h"
#include "viewport_func.h"
#include "vehicle_func.h"
#include "newgrf_station.h"
#include "pathfinder/follow_track.hpp"

#include "safeguards.h"

/**
 * Get the reserved trackbits for any tile, regardless of type.
 * @param t the tile
 * @return the reserved trackbits, or empty on nothing reserved or
 *     a tile without rail.
 */
TrackBits GetReservedTrackbits(TileIndex t)
{
	switch (GetTileType(t)) {
		case TileType::Railway:
			if (IsRailDepot(t)) return GetDepotReservationTrackBits(t);
			if (IsPlainRail(t)) return GetRailReservationTrackBits(t);
			break;

		case TileType::Road:
			if (IsLevelCrossing(t)) return GetCrossingReservationTrackBits(t);
			break;

		case TileType::Station:
			if (HasStationRail(t)) return GetStationReservationTrackBits(t);
			break;

		case TileType::TunnelBridge:
			if (GetTunnelBridgeTransportType(t) == TransportType::Rail) return GetTunnelBridgeReservationTrackBits(t);
			break;

		default:
			break;
	}
	return {};
}

/**
 * Set the reservation for a complete station platform.
 * @pre IsRailStationTile(start)
 * @param start starting tile of the platform
 * @param dir the direction in which to follow the platform
 * @param b the state the reservation should be set to
 */
void SetRailStationPlatformReservation(TileIndex start, DiagDirection dir, bool b)
{
	TileIndex     tile = start;
	TileIndexDiff diff = TileOffsByDiagDir(dir);

	assert(IsRailStationTile(start));
	assert(GetRailStationAxis(start) == DiagDirToAxis(dir));

	do {
		SetRailStationReservation(tile, b);
		MarkTileDirtyByTile(tile);
		tile = TileAdd(tile, diff);
	} while (IsCompatibleTrainStationTile(tile, start));
}

/**
 * Try to reserve a specific track on a tile
 * @param tile the tile
 * @param t the track
 * @param trigger_stations whether to call station randomisation trigger
 * @return \c true if reservation was successful, i.e. the track was
 *     free and didn't cross any other reserved tracks.
 */
bool TryReserveRailTrack(TileIndex tile, Track t, bool trigger_stations)
{
	assert(TrackdirBitsToTrackBits(GetTileTrackStatus(tile, TransportType::Rail, RoadTramType::Invalid).trackdirs).Test(t));

	if (_settings_client.gui.show_track_reservation) {
		/* show the reserved rail if needed */
		if (IsBridgeTile(tile)) {
			MarkBridgeDirty(tile);
		} else {
			MarkTileDirtyByTile(tile);
		}
	}

	switch (GetTileType(tile)) {
		case TileType::Railway:
			if (IsPlainRail(tile)) return TryReserveTrack(tile, t);
			if (IsRailDepot(tile)) {
				if (!HasDepotReservation(tile)) {
					SetDepotReservation(tile, true);
					MarkTileDirtyByTile(tile); // some GRFs change their appearance when tile is reserved
					return true;
				}
			}
			break;

		case TileType::Road:
			if (IsLevelCrossing(tile) && !HasCrossingReservation(tile)) {
				SetCrossingReservation(tile, true);
				UpdateLevelCrossing(tile, false);
				return true;
			}
			break;

		case TileType::Station:
			if (HasStationRail(tile) && !HasStationReservation(tile)) {
				SetRailStationReservation(tile, true);
				if (trigger_stations) {
					auto *st = BaseStation::GetByTile(tile);
					TriggerStationRandomisation(st, tile, StationRandomTrigger::PathReservation);
					TriggerStationAnimation(st, tile, StationAnimationTrigger::PathReservation);
				}
				MarkTileDirtyByTile(tile); // some GRFs need redraw after reserving track
				return true;
			}
			break;

		case TileType::TunnelBridge:
			if (GetTunnelBridgeTransportType(tile) == TransportType::Rail && GetTunnelBridgeReservationTrackBits(tile).None()) {
				SetTunnelBridgeReservation(tile, true);
				return true;
			}
			break;

		default:
			break;
	}
	return false;
}

/**
 * Lift the reservation of a specific track on a tile
 * @param tile the tile
 * @param t the track
 */
void UnreserveRailTrack(TileIndex tile, Track t)
{
	assert(TrackdirBitsToTrackBits(GetTileTrackStatus(tile, TransportType::Rail, RoadTramType::Invalid).trackdirs).Test(t));

	if (_settings_client.gui.show_track_reservation) {
		if (IsBridgeTile(tile)) {
			MarkBridgeDirty(tile);
		} else {
			MarkTileDirtyByTile(tile);
		}
	}

	switch (GetTileType(tile)) {
		case TileType::Railway:
			if (IsRailDepot(tile)) {
				SetDepotReservation(tile, false);
				MarkTileDirtyByTile(tile);
				break;
			}
			if (IsPlainRail(tile)) UnreserveTrack(tile, t);
			break;

		case TileType::Road:
			if (IsLevelCrossing(tile)) {
				SetCrossingReservation(tile, false);
				UpdateLevelCrossing(tile);
			}
			break;

		case TileType::Station:
			if (HasStationRail(tile)) {
				SetRailStationReservation(tile, false);
				MarkTileDirtyByTile(tile);
			}
			break;

		case TileType::TunnelBridge:
			if (GetTunnelBridgeTransportType(tile) == TransportType::Rail) SetTunnelBridgeReservation(tile, false);
			break;

		default:
			break;
	}
}


/**
 * Follow a reservation starting from a specific tile to the end.
 * @param o The owner of the track to follow; tracks of other owners are excluded.
 * @param rts The rail types to follow the reservation for; rail types not in the mask are excluded.
 * @param tile The start tile.
 * @param trackdir The start trackdir on the tile.
 * @param ignore_oneway Whether one way signals are to be ignored.
 * @return The end of the path.
 */
static PBSTileInfo FollowReservation(Owner o, RailTypes rts, TileIndex tile, Trackdir trackdir, bool ignore_oneway = false)
{
	TileIndex start_tile = tile;
	Trackdir  start_trackdir = trackdir;
	bool      first_loop = true;

	/* Start track not reserved? This can happen if two trains
	 * are on the same tile. The reservation on the next tile
	 * is not ours in this case, so exit. */
	if (!HasReservedTracks(tile, TrackdirToTrack(trackdir))) return PBSTileInfo(tile, trackdir, false);

	/* Do not disallow 90 deg turns as the setting might have changed between reserving and now. */
	CFollowTrackRail ft(o, rts);
	while (ft.Follow(tile, trackdir)) {
		TrackdirBits reserved = ft.new_td_bits & TrackBitsToTrackdirBits(GetReservedTrackbits(ft.new_tile));

		/* No reservation --> path end found */
		if (reserved.None()) {
			if (ft.is_station) {
				/* Check skipped station tiles as well, maybe our reservation ends inside the station. */
				TileIndexDiff diff = TileOffsByDiagDir(ft.exitdir);
				while (ft.tiles_skipped-- > 0) {
					ft.new_tile -= diff;
					if (HasStationReservation(ft.new_tile)) {
						tile = ft.new_tile;
						trackdir = DiagDirToDiagTrackdir(ft.exitdir);
						break;
					}
				}
			}
			break;
		}

		/* Can't have more than one reserved trackdir */
		Trackdir new_trackdir = FindFirstTrackdir(reserved);

		/* One-way signal against us. The reservation can't be ours as it is not
		 * a safe position from our direction and we can never pass the signal. */
		if (!ignore_oneway && HasOnewaySignalBlockingTrackdir(ft.new_tile, new_trackdir)) break;

		tile = ft.new_tile;
		trackdir = new_trackdir;

		if (first_loop) {
			/* Update the start tile after we followed the track the first
			 * time. This is necessary because the track follower can skip
			 * tiles (in stations for example) which means that we might
			 * never visit our original starting tile again. */
			start_tile = tile;
			start_trackdir = trackdir;
			first_loop = false;
		} else {
			/* Loop encountered? */
			if (tile == start_tile && trackdir == start_trackdir) break;
		}
		/* Depot tile? Can't continue. */
		if (IsRailDepotTile(tile)) break;
		/* Non-pbs signal? Reservation can't continue. */
		if (HasBlockSignalOnTrackdir(tile, trackdir)) break;
	}

	return PBSTileInfo(tile, trackdir, false);
}

/**
 * Helper struct for finding the best matching vehicle on a specific track.
 */
struct FindTrainOnTrackInfo {
	PBSTileInfo res; ///< Information about the track.
	Train *best;     ///< The currently "best" vehicle we have found.

	/** Init the best location to nullptr always! */
	FindTrainOnTrackInfo() : best(nullptr) {}
};

/**
 * Find the best matching vehicle on a tile.
 * @param[in,out] info State of the finding a vehicle on a reservation.
 * @param tile The tile to search for.
 */
static void CheckTrainsOnTrack(FindTrainOnTrackInfo &info, TileIndex tile)
{
	for (Vehicle *v : VehiclesOnTile(tile)) {
		if (v->type != VehicleType::Train || v->vehstatus.Test(VehState::Crashed)) continue;

		Train *t = Train::From(v);
		if (t->track == Track::Wormhole || t->track.Test(TrackdirToTrack(info.res.trackdir))) {
			t = t->First();

			/* ALWAYS return the lowest ID (anti-desync!) */
			if (info.best == nullptr || t->index < info.best->index) info.best = t;
		}
	}
}

/**
 * Follow a train reservation to the last tile.
 *
 * @param consist the vehicle
 * @param train_on_res Is set to a train we might encounter
 * @param from_rear Follow the reservation out of the back of the consist
 *        instead of out of its front -- e.g. to look for a coupling partner
 *        standing behind a stationary train (see GetTrainCouplePartner() in
 *        train_cmd.cpp). Both ends are taken in movement order, so that they
 *        are genuinely opposite ends: for a train that has been turned round
 *        the head of the vehicle list is the back of the train, and starting
 *        there and looking backwards points along the train's own body rather
 *        than out behind it, so the search finds nothing but itself.
 * @returns The last tile of the reservation or the current train tile if no reservation present.
 */
PBSTileInfo FollowTrainReservation(const Train *consist, Vehicle **train_on_res, bool from_rear)
{
	assert(consist->type == VehicleType::Train);

	TileIndex tile;
	Trackdir trackdir;
	if (from_rear) {
		const Train *rear = consist->GetMovingBack();
		tile = rear->tile;
		trackdir = ReverseTrackdir(rear->GetVehicleTrackdir());
	} else {
		const Train *moving_front = consist->GetMovingFront();
		tile = moving_front->tile;
		trackdir = moving_front->GetVehicleTrackdir();
	}

	if (IsRailDepotTile(tile) && GetDepotReservationTrackBits(tile).None()) return PBSTileInfo(tile, trackdir, false);

	FindTrainOnTrackInfo ftoti;
	/* A rescue engine's own booking runs against the signals on the way to its
	 * casualty (see IsSafeWaitingPosition), so following it has to be willing
	 * to go the same way. Stopped at the first signal facing it, the engine
	 * would read its own reservation as ending a few tiles out and set about
	 * booking the rest all over again. */
	ftoti.res = FollowReservation(consist->owner, GetAllCompatibleRailTypes(consist->railtypes), tile, trackdir,
			IsFetchingCasualty(consist->First()));
	ftoti.res.okay = IsSafeWaitingPosition(consist, ftoti.res.tile, ftoti.res.trackdir, true, Forbid90DegFor(consist));
	if (train_on_res != nullptr) {
		CheckTrainsOnTrack(ftoti, ftoti.res.tile);
		if (ftoti.best != nullptr) *train_on_res = ftoti.best->First();
		if (*train_on_res == nullptr && IsRailStationTile(ftoti.res.tile)) {
			/* The target tile is a rail station. The track follower
			 * has stopped on the last platform tile where we haven't
			 * found a train. Also check all previous platform tiles
			 * for a possible train. */
			TileIndexDiff diff = TileOffsByDiagDir(TrackdirToExitdir(ReverseTrackdir(ftoti.res.trackdir)));
			for (TileIndex st_tile = ftoti.res.tile + diff; *train_on_res == nullptr && IsCompatibleTrainStationTile(st_tile, ftoti.res.tile); st_tile += diff) {
				CheckTrainsOnTrack(ftoti, st_tile);
				if (ftoti.best != nullptr) *train_on_res = ftoti.best->First();
			}
		}
		if (*train_on_res == nullptr && IsTileType(ftoti.res.tile, TileType::TunnelBridge)) {
			/* The target tile is a bridge/tunnel, also check the other end tile. */
			CheckTrainsOnTrack(ftoti, GetOtherTunnelBridgeEnd(ftoti.res.tile));
			if (ftoti.best != nullptr) *train_on_res = ftoti.best->First();
		}
	}
	return ftoti.res;
}

/**
 * Find the train which has reserved a specific path.
 *
 * @param tile A tile on the path.
 * @param track A reserved track on the tile.
 * @return The vehicle holding the reservation or nullptr if the path is stray.
 */
Train *GetTrainForReservation(TileIndex tile, Track track)
{
	assert(HasReservedTracks(tile, track));
	Trackdir  trackdir = TrackToTrackdir(track);

	RailTypes rts = GetRailTypeInfo(GetTileRailType(tile))->compatible_railtypes;

	/* Follow the path from tile to both ends, one of the end tiles should
	 * have a train on it. We need FollowReservation to ignore one-way signals
	 * here, as one of the two search directions will be the "wrong" way. */
	for (int i = 0; i < 2; ++i, trackdir = ReverseTrackdir(trackdir)) {
		/* If the tile has a one-way block signal in the current trackdir, skip the
		 * search in this direction as the reservation can't come from this side.*/
		if (HasOnewaySignalBlockingTrackdir(tile, ReverseTrackdir(trackdir)) && !HasPbsSignalOnTrackdir(tile, trackdir)) continue;

		FindTrainOnTrackInfo ftoti;
		ftoti.res = FollowReservation(GetTileOwner(tile), rts, tile, trackdir, true);

		CheckTrainsOnTrack(ftoti, ftoti.res.tile);
		if (ftoti.best != nullptr) return ftoti.best;

		/* Special case for stations: check the whole platform for a vehicle. */
		if (IsRailStationTile(ftoti.res.tile)) {
			TileIndexDiff diff = TileOffsByDiagDir(TrackdirToExitdir(ReverseTrackdir(ftoti.res.trackdir)));
			for (TileIndex st_tile = ftoti.res.tile + diff; IsCompatibleTrainStationTile(st_tile, ftoti.res.tile); st_tile += diff) {
				CheckTrainsOnTrack(ftoti, st_tile);
				if (ftoti.best != nullptr) return ftoti.best;
			}
		}

		/* Special case for bridges/tunnels: check the other end as well. */
		if (IsTileType(ftoti.res.tile, TileType::TunnelBridge)) {
			CheckTrainsOnTrack(ftoti, GetOtherTunnelBridgeEnd(ftoti.res.tile));
			if (ftoti.best != nullptr) return ftoti.best;
		}
	}

	return nullptr;
}

/**
 * Determine whether a certain track on a tile is a safe position to end a path.
 *
 * @param v the vehicle to test for
 * @param tile The tile
 * @param trackdir The trackdir to test
 * @param include_line_end Should end-of-line tiles be considered safe?
 * @param forbid_90deg Don't allow trains to make 90 degree turns
 * @return True if it is a safe position
 */
bool IsSafeWaitingPosition(const Train *v, TileIndex tile, Trackdir trackdir, bool include_line_end, bool forbid_90deg)
{
	if (IsRailDepotTile(tile)) return true;

	/* A rescue engine on a call-out has one safe place to stop and it is up
	 * against the casualty. Nowhere along the way will do.
	 *
	 * It has to reach a train that has stopped where it stopped, and on a line
	 * that is worked in one direction that means going up it the wrong way --
	 * the queue is behind the casualty, so the only end that ever clears is
	 * the one it was driving towards. Which in turn means the whole road has
	 * to be its own for as long as it is on it: booked from the shed door to
	 * the casualty's nose in one piece, or not set off for at all. Stopping
	 * half way, at a signal, facing the traffic, is the one thing it must
	 * never do.
	 *
	 * So this is not an exemption bolted on to make a rescue engine go where
	 * an ordinary train may not. It is the other half of the same rule: it
	 * goes against the signals, and it pays for that by having to book the
	 * entire way before it moves at all. Refusing every intermediate stopping
	 * place is what makes that reservation all-or-nothing. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	if (IsFetchingCasualty(v->First())) {
		CFollowTrackRail rescue_ft(v, GetAllCompatibleRailTypes(v->railtypes));
		if (!rescue_ft.Follow(tile, trackdir)) return include_line_end;
		rescue_ft.new_td_bits &= DiagdirReachesTrackdirs(rescue_ft.exitdir);
		if (Rail90DegTurnDisallowed(GetTileRailType(rescue_ft.old_tile), GetTileRailType(rescue_ft.new_tile), forbid_90deg)) {
			rescue_ft.new_td_bits.Reset(TrackdirCrossesTrackdirs(trackdir));
		}
		if (rescue_ft.new_td_bits.None()) return include_line_end;
		return RescueRoadTracksOnTile(v, rescue_ft.new_tile).Any(TrackdirBitsToTrackBits(rescue_ft.new_td_bits));
	}

	/* For non-pbs signals, stop on the signal tile. */
	if (HasBlockSignalOnTrackdir(tile, trackdir)) return true;

	/* Check next tile. For performance reasons, we check for 90 degree turns ourself. */
	CFollowTrackRail ft(v, GetAllCompatibleRailTypes(v->railtypes));

	/* End of track? */
	if (!ft.Follow(tile, trackdir)) {
		/* Last tile of a terminus station is a safe position. */
		if (include_line_end) return true;
	}

	/* Check for reachable tracks. */
	ft.new_td_bits &= DiagdirReachesTrackdirs(ft.exitdir);
	if (Rail90DegTurnDisallowed(GetTileRailType(ft.old_tile), GetTileRailType(ft.new_tile), forbid_90deg)) ft.new_td_bits.Reset(TrackdirCrossesTrackdirs(trackdir));
	if (ft.new_td_bits.None()) return include_line_end;

	/* Pulled up against the consist we came to couple to. This is the end of
	 * the journey, so it is a place to end a path -- and it has to be said
	 * here, not only where a waiting position is checked for being free.
	 * Otherwise the search keeps extending the path looking for somewhere safe
	 * to stop, walks into the partner's own tiles, cannot reserve them because
	 * the partner is standing on them, and gives up: the train is left waiting
	 * at the last signal before the station for a path that will never come,
	 * because what is in its way is what it was sent for. See
	 * FEATURE_DESIGN_COUPLING_TOW.md. */
	if (((v->current_order.ShouldGoToCouple() && IsCouplePartnerOnPlatform(v, ft.new_tile)) ||
			IsCoupleTargetOnTile(v, ft.new_tile) || RescueRoadTracksOnTile(v, ft.new_tile).Any(TrackdirBitsToTrackBits(ft.new_td_bits)))) {
		return true;
	}

	if (ft.new_td_bits.Count() == 1) {
		Trackdir td = FindFirstTrackdir(ft.new_td_bits);
		/* PBS signal on next trackdir? Safe position. */
		if (HasPbsSignalOnTrackdir(ft.new_tile, td)) return true;
		/* One-way PBS signal against us? Safe if end-of-line is allowed. */
		if (IsTileType(ft.new_tile, TileType::Railway) && HasSignalOnTrackdir(ft.new_tile, ReverseTrackdir(td)) &&
				GetSignalType(ft.new_tile, TrackdirToTrack(td)) == SignalType::PathOneWay) {
			return include_line_end;
		}
	}

	return false;
}

/**
 * Check if a safe position is free.
 *
 * @param v the vehicle to test for
 * @param tile The tile
 * @param trackdir The trackdir to test
 * @param forbid_90deg Don't allow trains to make 90 degree turns
 * @return True if the position is free
 */
bool IsWaitingPositionFree(const Train *v, TileIndex tile, Trackdir trackdir, bool forbid_90deg)
{
	Track     track = TrackdirToTrack(trackdir);
	TrackBits reserved = GetReservedTrackbits(tile);

	/* Tile reserved? Can never be a free waiting position. */
	if (TrackOverlapsTracks(reserved, track)) return false;

	/* Not reserved and depot or not a pbs signal -> free. */
	if (IsRailDepotTile(tile)) return true;
	if (HasBlockSignalOnTrackdir(tile, trackdir)) return true;

	/* Check the next tile, it has to be free as well. Do not filter for compatible railtypes
	 * to make sure we never accidentally join up reservations. */
	CFollowTrackRail ft(v, RailTypes{});

	if (!ft.Follow(tile, trackdir)) return true;

	/* Check for reachable tracks. */
	ft.new_td_bits &= DiagdirReachesTrackdirs(ft.exitdir);
	if (Rail90DegTurnDisallowed(GetTileRailType(ft.old_tile), GetTileRailType(ft.new_tile), forbid_90deg)) ft.new_td_bits.Reset(TrackdirCrossesTrackdirs(trackdir));

	if (!HasReservedTracks(ft.new_tile, TrackdirBitsToTrackBits(ft.new_td_bits))) return true;

	/* The next tile being taken is normally the whole point of not stopping
	 * here: a train that waits with something reserved right in front of it is
	 * a train that is going to have to wait again in a worse place. A train on
	 * its way to couple is the exception, because what is standing there is
	 * what it came for. Refusing to stop short of its own partner leaves it
	 * waiting at the last signal before the station for a path that can never
	 * clear, since the wagons it is going to collect are not going to move on
	 * their own. Pulling up against them is exactly right, and coupling takes
	 * over from there. See FEATURE_DESIGN_COUPLING_TOW.md. */
	return ((v->current_order.ShouldGoToCouple() && IsCouplePartnerOnPlatform(v, ft.new_tile)) ||
			IsCoupleTargetOnTile(v, ft.new_tile) || RescueRoadTracksOnTile(v, ft.new_tile).Any(TrackdirBitsToTrackBits(ft.new_td_bits)));
}
