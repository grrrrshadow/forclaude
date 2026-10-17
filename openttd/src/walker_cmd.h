/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file walker_cmd.h Commands of the walker's inventory (walker_inventory.h): buying a thing, using it. */

#ifndef WALKER_CMD_H
#define WALKER_CMD_H

#include "command_type.h"
#include "walker_inventory.h"

CommandCost CmdBuyWalkerItem(DoCommandFlags flags, TileIndex tile, WalkerItem item);
CommandCost CmdUseWalkerItem(DoCommandFlags flags, WalkerItem item);
CommandCost CmdShootWalkerItem(DoCommandFlags flags, WalkerItem item);
CommandCost CmdHurtWalker(DoCommandFlags flags, uint8_t damage);
CommandCost CmdReviveWalker(DoCommandFlags flags, uint8_t health);
CommandCost CmdWalkerCheat(DoCommandFlags flags, uint16_t quiver);

DEF_CMD_TRAIT(Commands::BuyWalkerItem, CmdBuyWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::UseWalkerItem, CmdUseWalkerItem, CommandFlag::Location, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::ShootWalkerItem, CmdShootWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::HurtWalker, CmdHurtWalker, CommandFlag::Location, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::ReviveWalker, CmdReviveWalker, CommandFlag::Location, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::WalkerCheat, CmdWalkerCheat, CommandFlag::Offline, CommandType::Cheat)

#endif /* WALKER_CMD_H */
