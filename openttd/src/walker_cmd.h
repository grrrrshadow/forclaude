/*
 * This file is part of ottd Decouple by Karel Mácha, a game built on OpenTTD.
 * Copyright (C) 2026 Karel Mácha. All rights not granted by the licence below are reserved.
 * This file is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2 only.
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with it. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 * The licence grants no right to the name "ottd Decouple" or its logo.
 */

/** @file walker_cmd.h Commands of the walker's inventory (walker_inventory.h): buying a thing, using it. */

#ifndef WALKER_CMD_H
#define WALKER_CMD_H

#include "command_type.h"
#include "walker_inventory.h"

CommandCost CmdBuyWalkerItem(DoCommandFlags flags, TileIndex tile, WalkerItem item);
CommandCost CmdUseWalkerItem(DoCommandFlags flags, WalkerItem item);
CommandCost CmdShootWalkerItem(DoCommandFlags flags, WalkerItem item);
CommandCost CmdFeedWalkerItem(DoCommandFlags flags, WalkerItem item);
CommandCost CmdHurtWalker(DoCommandFlags flags, uint8_t damage);
CommandCost CmdReviveWalker(DoCommandFlags flags, uint8_t health);
CommandCost CmdWalkerCheat(DoCommandFlags flags, uint16_t quiver);
CommandCost CmdBuyWalkerPet(DoCommandFlags flags, const std::string &name);
CommandCost CmdPickWalkerItem(DoCommandFlags flags, WalkerItem item);
CommandCost CmdSetWalkerQuickKey(DoCommandFlags flags, uint8_t key, WalkerItem item);
CommandCost CmdPlantWalkerSeedling(DoCommandFlags flags, int32_t x, int32_t y);
CommandCost CmdHarvestWalkerPlant(DoCommandFlags flags, int32_t x, int32_t y);
CommandCost CmdBuyWalkerNurse(DoCommandFlags flags, uint8_t months);
CommandCost CmdHealWalkerByNurse(DoCommandFlags flags, uint8_t health);
Money WalkerPetPrice(std::string_view name);
Money WalkerNursePrice();
struct CompanyProperties;
bool WalkerInsured(const CompanyProperties &c);

/* The walker's health insurance (the player, 28. and 31. 10.: "the nurses will sell a monthly health insurance before
 * the church"; the price "is all the same to me"; "+10 every 3 s, yes"). */
static constexpr int64_t WALKER_NURSE_PRICE = 1000; ///< pounds a month of it, inflated as the dogs
static constexpr int WALKER_NURSE_DAYS = 30;        ///< a month of it, days of the economy
static constexpr uint8_t WALKER_NURSE_HEAL = 10;    ///< health the nurse gives back at a time ...
static constexpr uint WALKER_NURSE_HEAL_TICKS = 111; ///< ... every so many ticks of the game (3 s)

DEF_CMD_TRAIT(Commands::BuyWalkerItem, CmdBuyWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::UseWalkerItem, CmdUseWalkerItem, CommandFlag::Location, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::ShootWalkerItem, CmdShootWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::FeedWalkerItem, CmdFeedWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::HurtWalker, CmdHurtWalker, CommandFlag::Location, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::ReviveWalker, CmdReviveWalker, CommandFlag::Location, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::WalkerCheat, CmdWalkerCheat, CommandFlag::Offline, CommandType::Cheat)
DEF_CMD_TRAIT(Commands::BuyWalkerPet, CmdBuyWalkerPet, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::PickWalkerItem, CmdPickWalkerItem, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::SetWalkerQuickKey, CmdSetWalkerQuickKey, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::PlantWalkerSeedling, CmdPlantWalkerSeedling, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::HarvestWalkerPlant, CmdHarvestWalkerPlant, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::BuyWalkerNurse, CmdBuyWalkerNurse, {}, CommandType::OtherManagement)
DEF_CMD_TRAIT(Commands::HealWalkerByNurse, CmdHealWalkerByNurse, {}, CommandType::OtherManagement)

#endif /* WALKER_CMD_H */
