/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file yapf_destrail.hpp Determining the destination for rail vehicles. */

#ifndef YAPF_DESTRAIL_HPP
#define YAPF_DESTRAIL_HPP

#include "../../train.h"
#include "../pathfinder_func.h"
#include "../pathfinder_type.h"

class CYapfDestinationRailBase {
protected:
	RailTypes compatible_railtypes;

public:
	void SetDestination(const Train *v, bool override_rail_type = false)
	{
		this->compatible_railtypes = v->compatible_railtypes;
		if (override_rail_type) this->compatible_railtypes.Set(GetAllCompatibleRailTypes(v->railtypes));
	}

	bool IsCompatibleRailType(RailType rt)
	{
		return this->compatible_railtypes.Test(rt);
	}

	RailTypes GetCompatibleRailTypes() const
	{
		return this->compatible_railtypes;
	}
};

template <class Types>
class CYapfDestinationAnyDepotRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir());
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, [[maybe_unused]] Trackdir td)
	{
		return IsRailDepotTile(tile);
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		n.estimate = n.cost;
		return true;
	}
};

template <class Types>
class CYapfDestinationAnySafeTileRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables
	typedef typename Types::TrackFollower TrackFollower; ///< TrackFollower. Need to typedef for gcc 2.95

	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

	void SetDestination(const Train *v, bool override_rail_type = false)
	{
		/* See the matching call in CYapfDestinationTileOrStationRailT. */
		if (IsFetchingCasualty(v->First())) Yapf().DisableCache(true);
		this->CYapfDestinationRailBase::SetDestination(v, override_rail_type);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir());
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, Trackdir td)
	{
		return IsSafeWaitingPosition(Yapf().GetVehicle(), tile, td, true, !TrackFollower::Allow90degTurns()) &&
				IsWaitingPositionFree(Yapf().GetVehicle(), tile, td, !TrackFollower::Allow90degTurns());
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		n.estimate = n.cost;
		return true;
	}
};

template <class Types>
class CYapfDestinationTileOrStationRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

protected:
	TileIndex dest_tile;
	TrackdirBits dest_trackdirs;
	StationID dest_station_id;
	bool any_depot;
	bool couple_at_dest_station; ///< Destination station only counts on the platform holding our coupling partner.

	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

public:
	void SetDestination(const Train *v)
	{
		/* A rescue engine on its way out reads two things off the track
		 * differently from everybody else: a one-way signal facing it is not a
		 * dead end, and coming at a path signal from behind costs it nothing
		 * (see SignalCost). Both of those answers are stored in the **shared**
		 * segment cache, which is keyed by track and thrown away only when the
		 * layout changes -- it has no idea which train asked.
		 *
		 * So on a line ordinary trains have already used, the segments are long
		 * since filed as "dead end at that signal", and the rescue engine is
		 * handed that answer and never plans a route up the line at all. It is
		 * not the reservation failing; the road is never even offered. On a rig
		 * with two trains and a clean map the segments are usually computed by
		 * the tow itself and everything looks fine, which is why this only ever
		 * showed up in a real game.
		 *
		 * Whoever makes a cached answer depend on who is asking has to keep out
		 * of the cache. Same reasoning as BlockedByFreeWagons being left out of
		 * ESRF_CACHED_MASK, and the same remedy the complex-waypoint case below
		 * already uses. */
		if (IsFetchingCasualty(v->First())) Yapf().DisableCache(true);

		this->any_depot = false;
		this->couple_at_dest_station = false;
		switch (v->current_order.GetType()) {
			case OT_GOTO_WAYPOINT:
				if (!Waypoint::Get(v->current_order.GetDestination().ToStationID())->IsSingleTile()) {
					/* In case of 'complex' waypoints we need to do a look
					 * ahead. This look ahead messes a bit about, which
					 * means that it 'corrupts' the cache. To prevent this
					 * we disable caching when we're looking for a complex
					 * waypoint. */
					Yapf().DisableCache(true);
				}
				[[fallthrough]];

			case OT_GOTO_STATION:
				this->dest_tile = CalcClosestStationTile(v->current_order.GetDestination().ToStationID(), v->GetMovingFront()->tile, v->current_order.IsType(OT_GOTO_STATION) ? StationType::Rail : StationType::RailWaypoint);
				this->dest_station_id = v->current_order.GetDestination().ToStationID();
				this->dest_trackdirs = INVALID_TRACKDIR_BIT;
				/* A "go to couple" order names a station, but the point of it
				 * is a specific consist standing at that station, not the
				 * station itself. Remember to narrow the destination test to
				 * the platform actually holding it. */
				this->couple_at_dest_station = v->current_order.IsType(OT_GOTO_STATION) && v->current_order.ShouldGoToCouple();
				break;

			case OT_GOTO_DEPOT:
				if (v->current_order.GetDepotActionType().Test(OrderDepotActionFlag::NearestDepot)) {
					this->any_depot = true;
				}
				[[fallthrough]];

			default:
				this->dest_tile = v->dest_tile == INVALID_TILE ? TileIndex{} : v->dest_tile;
				this->dest_station_id = StationID::Invalid();
				this->dest_trackdirs = GetTileTrackStatus(this->dest_tile, TransportType::Rail, RoadTramType::Invalid).trackdirs;
				break;
		}
		this->CYapfDestinationRailBase::SetDestination(v);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir());
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, Trackdir td)
	{
		if (this->dest_station_id != StationID::Invalid()) {
			if (!HasStationTileRail(tile)
					|| (GetStationIndex(tile) != this->dest_station_id)
					|| (GetRailStationTrack(tile) != TrackdirToTrack(td))) {
				return false;
			}

			/* Any platform of the station will do -- unless we were sent here
			 * to couple, in which case only the one our partner is standing
			 * on is of any use. Without this the search settles for whichever
			 * platform it reaches first (an empty one is cheaper, having no
			 * reservation to cross), the train arrives at the wrong platform,
			 * and then waits forever for a partner that is never going to
			 * become adjacent. See FEATURE_DESIGN_COUPLING_TOW.md. */
			if (this->couple_at_dest_station) return IsCouplePartnerOnPlatform(Yapf().GetVehicle(), tile);

			return true;
		}

		if (this->any_depot) {
			return IsRailDepotTile(tile);
		}

		/* A rescue engine's destination is wherever its casualty stopped, and
		 * that can be a platform. A platform is one step to the search, which
		 * reports the far end of it and never the tile in the middle the
		 * casualty stands on, so the destination was walked straight over
		 * and reported unreachable. The platform the casualty stands on is
		 * the destination, whichever of its tiles the step lands on. */
		if (IsRailStationTile(tile) && IsRailStationTile(this->dest_tile) && IsCompatibleTrainStationTile(tile, this->dest_tile)) {
			const Train *v = Yapf().GetVehicle();
			if (v != nullptr && IsFetchingCasualty(v->First()) && IsRescueTargetOnTile(v, this->dest_tile) &&
					GetRailStationTrack(tile) == TrackdirToTrack(td)) {
				return true;
			}
		}

		return (tile == this->dest_tile) && this->dest_trackdirs.Test(td);
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		if (this->PfDetectDestination(n)) {
			n.estimate = n.cost;
			return true;
		}

		n.estimate = n.cost + OctileDistanceCost(n.GetLastTile(), n.GetLastTrackdir(), this->dest_tile);
		assert(n.estimate >= n.parent->estimate);
		return true;
	}
};

#endif /* YAPF_DESTRAIL_HPP */
