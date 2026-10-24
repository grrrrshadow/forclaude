/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tunnelbridge_map.h Functions that have tunnels and bridges in common. */

#ifndef TUNNELBRIDGE_MAP_H
#define TUNNELBRIDGE_MAP_H

#include "bridge_map.h"
#include "tunnel_map.h"
#include "signal_type.h"


/**
 * Get the direction pointing to the other end.
 *
 * Tunnel: Get the direction facing into the tunnel
 * Bridge: Get the direction pointing onto the bridge
 * @param t The tile to analyze
 * @pre IsTileType(t, TileType::TunnelBridge)
 * @return the above mentioned direction
 */
inline DiagDirection GetTunnelBridgeDirection(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	return (DiagDirection)GB(t.m5(), 0, 2);
}

/**
 * Tunnel: Get the transport type of the tunnel (road or rail)
 * Bridge: Get the transport type of the bridge's ramp
 * @param t The tile to analyze
 * @pre IsTileType(t, TileType::TunnelBridge)
 * @return the transport type in the tunnel/bridge
 */
inline TransportType GetTunnelBridgeTransportType(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	return (TransportType)GB(t.m5(), 2, 2);
}

/**
 * Tunnel: Is this tunnel entrance in a snowy or desert area?
 * Bridge: Does the bridge ramp lie in a snow or desert area?
 * @param t The tile to analyze
 * @pre IsTileType(t, TileType::TunnelBridge)
 * @return true if and only if the tile is in a snowy/desert area
 */
inline bool HasTunnelBridgeSnowOrDesert(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	return HasBit(t.m7(), 5);
}

/**
 * Tunnel: Places this tunnel entrance in a snowy or desert area, or takes it out of there.
 * Bridge: Sets whether the bridge ramp lies in a snow or desert area.
 * @param t the tunnel entrance / bridge ramp tile
 * @param snow_or_desert is the entrance/ramp in snow or desert (true), when
 *                       not in snow and not in desert false
 * @pre IsTileType(t, TileType::TunnelBridge)
 */
inline void SetTunnelBridgeSnowOrDesert(Tile t, bool snow_or_desert)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	SB(t.m7(), 5, 1, snow_or_desert);
}

/**
 * Determines type of the wormhole and returns its other end
 * @param t one end
 * @pre IsTileType(t, TileType::TunnelBridge)
 * @return other end
 */
inline TileIndex GetOtherTunnelBridgeEnd(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	return IsTunnel(t) ? GetOtherTunnelEnd(t) : GetOtherBridgeEnd(t);
}


/**
 * Get the reservation state of the rail tunnel/bridge
 * @pre IsTileType(t, TileType::TunnelBridge) && GetTunnelBridgeTransportType(t) == TransportType::Rail
 * @param t the tile
 * @return reservation state
 */
inline bool HasTunnelBridgeReservation(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	assert(GetTunnelBridgeTransportType(t) == TransportType::Rail);
	return HasBit(t.m5(), 4);
}

/**
 * Set the reservation state of the rail tunnel/bridge
 * @pre IsTileType(t, TileType::TunnelBridge) && GetTunnelBridgeTransportType(t) == TransportType::Rail
 * @param t the tile
 * @param b the reservation state
 */
inline void SetTunnelBridgeReservation(Tile t, bool b)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	assert(GetTunnelBridgeTransportType(t) == TransportType::Rail);
	AssignBit(t.m5(), 4, b);
}

/**
 * Get the reserved track bits for a rail tunnel/bridge
 * @pre IsTileType(t, TileType::TunnelBridge) && GetTunnelBridgeTransportType(t) == TransportType::Rail
 * @param t the tile
 * @return reserved track bits
 */
inline TrackBits GetTunnelBridgeReservationTrackBits(Tile t)
{
	return HasTunnelBridgeReservation(t) ? DiagDirToDiagTrack(GetTunnelBridgeDirection(t)) : TrackBits{};
}

/**
 * Signals on a tunnel or bridge portal.
 *
 * A tunnel or a bridge is one block from end to end, however long it is, so a
 * second train cannot follow the first into it and a long one is a wall
 * across the line. A signal on the portal breaks that.
 *
 * The state lives in m2, which is free on both a tunnel head and a bridge
 * ramp: two bits per side, so an old save loads with none of them set, which
 * reads as "no signals here" and leaves the tunnel exactly as it was. Nothing
 * is added to the save.
 *
 * Bit 0 says a signal stands here facing into the tunnel (what a train
 * entering reads), bit 1 that it shows red; bits 2 and 3 say the same for the
 * signal facing out of it. Bits 4 to 7 hold which picture they are drawn
 * with. Only the portals carry a signal that can be seen:
 * on a bridge there is nowhere to draw one but the ramps, so any division of
 * the bore itself is left invisible.
 */
enum class TunnelBridgeSignal : uint8_t {
	Entry = 0, ///< Facing into the tunnel or bridge: what a train entering reads.
	Exit = 2, ///< Facing out of it: what a train inside reads at the far end.
};

/**
 * Whether a signal of this kind stands on this portal.
 * @pre IsTileType(t, TileType::TunnelBridge)
 */
inline bool HasTunnelBridgeSignal(Tile t, TunnelBridgeSignal which)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	return HasBit(t.m2(), to_underlying(which));
}

/** Whether this portal carries any signal at all. */
inline bool IsTunnelBridgeSignalled(Tile t)
{
	return IsTileType(t, TileType::TunnelBridge) &&
			(HasTunnelBridgeSignal(t, TunnelBridgeSignal::Entry) || HasTunnelBridgeSignal(t, TunnelBridgeSignal::Exit));
}

/** Put a signal of this kind on this portal, or take it away. */
inline void SetTunnelBridgeSignal(Tile t, TunnelBridgeSignal which, bool present)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	assert(GetTunnelBridgeTransportType(t) == TransportType::Rail);
	AssignBit(t.m2(), to_underlying(which), present);
}

/**
 * What a signal of this kind is showing.
 * @pre HasTunnelBridgeSignal(t, which)
 */
inline SignalState GetTunnelBridgeSignalState(Tile t, TunnelBridgeSignal which)
{
	assert(HasTunnelBridgeSignal(t, which));
	return HasBit(t.m2(), to_underlying(which) + 1) ? SignalState::Red : SignalState::Green;
}

/** Set what a signal of this kind is showing. */
inline void SetTunnelBridgeSignalState(Tile t, TunnelBridgeSignal which, SignalState state)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	AssignBit(t.m2(), to_underlying(which) + 1, state == SignalState::Red);
}

/**
 * Which picture the signals on this portal are drawn with: the kind the
 * player was building when they were put up.
 *
 * It is a picture and nothing more. A bore is one section whatever kind of
 * signal stands on its mouths, so the type changes nothing about how it
 * works -- it is there so a line of path signals does not turn into a block
 * signal where it crosses a bridge. Bits 4 to 6 of m2 hold the type and bit
 * 7 the variant, all zero in an old save, which reads as the plain electric
 * block signal that was drawn before there was anything to read.
 *
 * @pre IsTileType(t, TileType::TunnelBridge)
 */
inline SignalType GetTunnelBridgeSignalType(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	SignalType type = static_cast<SignalType>(GB(t.m2(), 4, 3));
	return type < SignalType::End ? type : SignalType::Block;
}

/** Set which picture the signals on this portal are drawn with. */
inline void SetTunnelBridgeSignalType(Tile t, SignalType type)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	SB(t.m2(), 4, 3, to_underlying(type));
}

/** Whether the signals on this portal are drawn as semaphores. @pre IsTileType(t, TileType::TunnelBridge) */
inline SignalVariant GetTunnelBridgeSignalVariant(Tile t)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	return HasBit(t.m2(), 7) ? SignalVariant::Semaphore : SignalVariant::Electric;
}

/** Set whether the signals on this portal are drawn as semaphores. */
inline void SetTunnelBridgeSignalVariant(Tile t, SignalVariant variant)
{
	assert(IsTileType(t, TileType::TunnelBridge));
	AssignBit(t.m2(), 7, variant == SignalVariant::Semaphore);
}

#endif /* TUNNELBRIDGE_MAP_H */
