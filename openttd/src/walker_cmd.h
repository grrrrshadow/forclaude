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

DEF_CMD_TRAIT(Commands::BuyWalkerItem, CmdBuyWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::UseWalkerItem, CmdUseWalkerItem, CommandFlag::Location, CommandType::OtherManagement)

#endif /* WALKER_CMD_H */
