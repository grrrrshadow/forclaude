/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file walker_sl.cpp The plants the walkers planted (WalkerPlant, walker_inventory.h), saved and loaded. */

#include "../stdafx.h"

#include "saveload.h"

#include "../walker_inventory.h"

#include "../safeguards.h"

static const SaveLoad _walker_plant_desc[] = {
	SLE_VAR(WalkerPlant, x, VarTypes::I32),
	SLE_VAR(WalkerPlant, y, VarTypes::I32),
	SLE_VAR(WalkerPlant, planted, VarTypes::I32),
	SLE_VAR(WalkerPlant, planted_tick, VarTypes::U64),
	SLE_VAR(WalkerPlant, owner, VarTypes::U8),
};

struct WPLTChunkHandler : ChunkHandler {
	WPLTChunkHandler() : ChunkHandler('WPLT', ChunkType::Table) {}

	void Save() const override
	{
		SlTableHeader(_walker_plant_desc);
		int index = 0;
		for (WalkerPlant &p : _walker_plants) {
			SlSetArrayIndex(index++);
			SlObject(&p, _walker_plant_desc);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlTableHeader(_walker_plant_desc);
		_walker_plants.clear();
		while (SlIterateArray() != -1) {
			WalkerPlant &p = _walker_plants.emplace_back();
			SlObject(&p, slt);
		}
	}
};

static const WPLTChunkHandler WPLT;
static const ChunkHandlerRef walker_chunk_handlers[] = {
	WPLT,
};

extern const ChunkHandlerTable _walker_chunk_handlers(walker_chunk_handlers);
