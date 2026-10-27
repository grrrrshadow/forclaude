/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cab_drive.h Driving freely from the cab (cab_view.h): a road vehicle off the road, a ship anywhere on the
 * water, an aircraft anywhere in the air, at any angle; the aircraft at the height asked for too.
 *
 * The player (14. 10.): "can a car leave the road? we are 3D, drive in every direction"; "trees it drives through,
 * at houses it stops"; "the ship too, turning gently by small angles". A vehicle driven freely keeps a heading in
 * 128ths of a turn and its exact position in 256ths of a unit, and moves a unit along the heading for every step its
 * speed gives it -- in whole numbers only, from the table below, so every computer of a network game moves it
 * alike. It stops where it may not go and turns there. Asked back to its orders it drives on until it comes onto a
 * tile it can go on as usual -- a road a road vehicle enters by a road's end, water with a ship's course -- and goes
 * on from there as its orders say.
 */

#ifndef CAB_DRIVE_H
#define CAB_DRIVE_H

#include "direction_type.h"

/** A full turn of a heading driven freely. */
static constexpr int CAB_YAW_TURN = 128;
/** One press of left or right turns a heading driven freely this much: an eleventh-and-a-quarter degrees. */
static constexpr int CAB_YAW_STEP = 4;
/** The exact position's parts of a unit. */
static constexpr int CAB_SUBUNIT = 256;
/** One press of up or down changes the height an aircraft flown freely climbs or sinks to this much, units of z. */
static constexpr int CAB_ALTITUDE_STEP = 8;

/** What the cab asks of a vehicle driven freely (CmdCabDrive()). */
enum class CabDriveOp : uint8_t {
	Free = 0,  ///< leave the road and drive freely (a road vehicle; a ship goes freely with its first turn)
	Left = 1,  ///< turn the heading left a step
	Right = 2, ///< turn the heading right a step
	Rejoin = 3, ///< back to the orders, on the next tile the vehicle can go on as usual
	Up = 4,     ///< an aircraft flown freely: higher a step
	Down = 5,   ///< an aircraft flown freely: lower a step
	Reverse = 6, ///< a road vehicle driven freely: backwards from now on (Vehicle::cab_reverse), at most Vehicle::CAB_REVERSE_SPEED
	Forward = 7, ///< and forwards again, stopped, held to the slowest speed (CAB_SLOWEST)
};

/** The slowest speed the brake of the cab holds a vehicle to (CabSpeed()); braked further it stops. */
static constexpr int CAB_SLOWEST = 5;

/**
 * The cosine of a heading, in 256ths: a unit along it moves this far along x.
 * @param yaw the heading, in 128ths of a turn: 0 along +x (south-west), 32 along +y (south-east)
 */
inline int CabCos(int yaw)
{
	static const int16_t quarter[33] = {
		256, 256, 255, 253, 251, 248, 245, 241, 237, 231, 226, 220, 213, 206, 198, 190, 181,
		172, 162, 152, 142, 132, 121, 109, 98, 86, 74, 62, 50, 38, 25, 13, 0,
	};
	yaw &= CAB_YAW_TURN - 1;
	if (yaw <= 32) return quarter[yaw];
	if (yaw <= 64) return -quarter[64 - yaw];
	if (yaw <= 96) return -quarter[yaw - 64];
	return quarter[128 - yaw];
}

/** The sine of a heading, in 256ths: a unit along it moves this far along y. */
inline int CabSin(int yaw)
{
	return CabCos(yaw - 32);
}

/** The heading of one of the eight directions: SW 0, S 16, SE 32, E 48, NE 64, N 80, NW 96, W 112. */
inline int DirectionToCabYaw(Direction d)
{
	return (80 - 16 * static_cast<int>(to_underlying(d)) + 2 * CAB_YAW_TURN) & (CAB_YAW_TURN - 1);
}

/** The one of the eight directions nearest to a heading, for the vehicle's sprites. */
inline Direction CabYawToDirection(int yaw)
{
	int k = (80 - yaw + 8 + 2 * CAB_YAW_TURN) & (CAB_YAW_TURN - 1);
	return static_cast<Direction>(k / 16);
}

#endif /* CAB_DRIVE_H */
