/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_act0_cargo.cpp NewGRF Action 0x00 handler for cargo. */

#include "../stdafx.h"
#include "../debug.h"
#include "../newgrf_cargo.h"
#include "newgrf_bytereader.h"
#include "newgrf_internal.h"
#include "newgrf_stringmapping.h"

#include "../safeguards.h"

/**
 * Install cargo label in a fallback cargo list, if a NewGRF-defined cargo list is not present.
 * This allows this NewGRF to use its self-defined cargo types without needing an explicit translation table.
 * @param id ID of the cargo.
 * @param last ID of the last cargo being set up.
 * @param label Label to install.
 */
static void MaybeInstallFallbackCargoLabel(uint id, uint last, CargoLabel label)
{
	if (_cur_gps.grffile->cargo_list.empty()) {
		/* Cargo translation table isn't configured yet, assume it won't be and configure the cargo list as a fallback list. */
		auto default_cargo_list = GetCargoTranslationTable(*_cur_gps.grffile);
		_cur_gps.grffile->cargo_list.assign(default_cargo_list.begin(), default_cargo_list.end());
		_cur_gps.grffile->cargo_list_is_fallback = true;
	}

	if (_cur_gps.grffile->cargo_list_is_fallback) {
		/* Automatically fill fallback cargo list with the defined label, resizing as needed. */
		if (_cur_gps.grffile->cargo_list.size() < last) _cur_gps.grffile->cargo_list.resize(last, CT_INVALID);
		_cur_gps.grffile->cargo_list[id] = label;
	}
}

/**
 * Cargo slots when more than one set defines cargoes.
 *
 * A set numbers its cargoes from 0 and puts them into the slot of that
 * number, so two industry sets wrote over each other's cargoes and each
 * refused the other for it. Industries, industry tiles and houses have long
 * had a set's own numbers apart from the game's (OverrideManagerBase);
 * cargoes now do too. A set's cargo id stays its own, and the game keeps
 * where each one went:
 *  - a label another set already brought is that set's cargo, shared: the
 *    later set's cargo id leads to it, and the later set's properties are
 *    written over it as ever. ECS depends on that: its town vector puts a
 *    bare label and bit number into the slot of every ECS cargo, and the
 *    vector that carries the cargo gives it its name and the rest later;
 *    keeping the first set's properties left most ECS cargoes nameless;
 *  - otherwise the slot of the set's own number, as ever, when that slot is
 *    empty, the game's own cargo, or this set's -- so a game with one
 *    cargo set comes out exactly as it did;
 *  - otherwise the game's own cargo of that label, if it has one;
 *  - otherwise the first empty slot.
 * Switching a cargo off switches off the game's own cargoes and the set's
 * own, never a cargo another set brought: a shared cargo stays for as long
 * as the set that brought it wants it. Everything else about a cargo --
 * industries, vehicles, callbacks -- reaches it by label through the set's
 * cargo table, so nothing else needs to know where a cargo went.
 */
struct CargoSlots {
	std::array<const GRFFile *, NUM_CARGO> owner{}; ///< The set whose cargo a slot holds; nullptr for the game's own cargoes and empty slots.
	std::array<uint8_t, NUM_CARGO> owner_id{}; ///< The owner's own id for the cargo in a slot.
	std::map<const GRFFile *, std::array<CargoType, NUM_CARGO>> placed; ///< Per set: the slot each of its cargo ids went to, INVALID_CARGO for none yet.
	std::vector<CargoType> block; ///< For the Action 0 being read: the slot each id in it goes to, INVALID_CARGO where its properties are not applied.
	uint block_first = 0; ///< The first id of that Action 0.

	std::array<CargoType, NUM_CARGO> &PlacedBy(const GRFFile *grf)
	{
		auto [it, inserted] = this->placed.try_emplace(grf);
		if (inserted) it->second.fill(INVALID_CARGO);
		return it->second;
	}
};

static CargoSlots _cargo_slots;

/** Forget where every set's cargoes went; the NewGRFs are about to be read again. */
void ResetCargoSlots()
{
	_cargo_slots = {};
}

/**
 * Size of a cargo property, for reading an Action 0 ahead.
 * @param prop The property.
 * @return Its size in bytes, 0 for a property this game does not know.
 */
static uint CargoPropertySize(uint8_t prop)
{
	switch (prop) {
		case 0x08: case 0x0F: case 0x10: case 0x11: case 0x13: case 0x14: case 0x15: case 0x18: case 0x1A: case 0x1E:
			return 1;
		case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x16: case 0x19: case 0x1B: case 0x1C: case 0x1D: case 0x1F:
			return 2;
		case 0x12: case 0x17:
			return 4;
		default:
			return 0;
	}
}

/**
 * Decide the slot for one cargo id of the current set (see CargoSlots).
 * @param local The set's own id for the cargo.
 * @param label The label this Action 0 gives it, if it gives one.
 * @param bitnum The bit number this Action 0 gives it, if it gives one.
 * @return The slot the properties go to, INVALID_CARGO when they are not applied.
 */
static CargoType DecideCargoSlot(uint local, std::optional<CargoLabel> label, std::optional<uint8_t> bitnum)
{
	const GRFFile *grf = _cur_gps.grffile;
	auto &owner = _cargo_slots.owner;
	auto &owner_id = _cargo_slots.owner_id;
	auto &placed = _cargo_slots.PlacedBy(grf);
	const CargoType natural = static_cast<CargoType>(local);
	const CargoType mine = placed[local];

	auto claim = [&](CargoType slot) {
		owner[slot] = grf;
		owner_id[slot] = static_cast<uint8_t>(local);
		placed[local] = slot;
		return slot;
	};
	/* The set's own cargo in a slot, put there under this very id. */
	auto is_mine = [&](CargoType slot) { return IsValidCargoType(slot) && owner[slot] == grf && owner_id[slot] == local; };
	/* What the game did before sets had numbers of their own: the slot of the set's number, unless another set's cargo is in it. */
	auto natural_is_open = [&]() { return owner[natural] == nullptr || is_mine(natural); };

	const bool gives_label = label.has_value() && *label != CT_INVALID && label->base() != 0;
	const bool switches_off = (label.has_value() && !gives_label) || (!label.has_value() && bitnum == INVALID_CARGO_BITNUM);

	if (switches_off) {
		/* nml's disable_item() and its like: the set's own cargo, or the game's own. */
		if (IsValidCargoType(mine) && !is_mine(mine)) {
			/* A cargo the set shares with the set that brought it: the set lets go of it, the cargo stays. */
			placed[local] = INVALID_CARGO;
			return INVALID_CARGO;
		}
		CargoType slot = is_mine(mine) ? mine : (natural_is_open() ? natural : INVALID_CARGO);
		if (!IsValidCargoType(slot)) {
			GrfMsg(2, "CargoChangeInfo: Cargo {} is another set's, not switching it off", local);
			return INVALID_CARGO;
		}
		owner[slot] = nullptr;
		placed[local] = INVALID_CARGO;
		return slot;
	}

	if (!gives_label) {
		/* Properties without a label: to where the id went, or its own slot. */
		if (IsValidCargoType(mine)) return mine;
		return natural_is_open() ? natural : INVALID_CARGO;
	}

	/* A set giving its own cargo a new label writes over it where it is, as it always did. */
	if (is_mine(mine)) return mine;

	/* The label is another set's cargo: shared, and written over. */
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		CargoType slot = cs->Index();
		if (cs->label != *label || owner[slot] == nullptr || owner[slot] == grf) continue;
		placed[local] = slot;
		return slot;
	}

	if (natural_is_open()) return claim(natural);

	/* The game's own cargo of this label. */
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		if (cs->label == *label && owner[cs->Index()] == nullptr) return claim(cs->Index());
	}

	/* The first empty slot. */
	for (uint i = 0; i < NUM_CARGO; i++) {
		CargoType slot = static_cast<CargoType>(i);
		if (owner[slot] == nullptr && !CargoSpec::Get(slot)->IsValid()) return claim(slot);
	}

	GrfMsg(1, "CargoChangeInfo: No slot left for cargo {}", local);
	return INVALID_CARGO;
}

/**
 * Read an Action 0 for cargoes ahead and decide the slot of every cargo in
 * it: the label may be the last property in the block (FIRS writes it last),
 * and the slot has to be known before the first one is applied.
 * @param first The set's id of the first cargo in the block.
 * @param numinfo How many cargoes the block is for.
 * @param numprops How many properties it has.
 * @param buf The block, from its first property on; a copy, read here only.
 */
void PrepareCargoBlock(uint first, uint numinfo, uint numprops, ByteReader buf)
{
	_cargo_slots.block_first = first;
	_cargo_slots.block.assign(numinfo, INVALID_CARGO);
	if (first + numinfo > NUM_CARGO) return;

	std::vector<std::optional<CargoLabel>> labels(numinfo);
	std::vector<std::optional<uint8_t>> bitnums(numinfo);
	try {
		for (; numprops > 0 && buf.HasData(); numprops--) {
			uint8_t prop = buf.ReadByte();
			uint size = CargoPropertySize(prop);
			if (size == 0) break; // the rest cannot be read ahead; what was read decides
			for (uint i = 0; i < numinfo; i++) {
				if (prop == 0x17) {
					labels[i] = CargoLabel{std::byteswap(buf.ReadDWord())};
				} else if (prop == 0x08) {
					bitnums[i] = buf.ReadByte();
				} else {
					buf.Skip(size);
				}
			}
		}
	} catch (const OTTDByteReaderSignal &) {
		/* A short block; the properties read will say so themselves. */
	}

	for (uint i = 0; i < numinfo; i++) {
		_cargo_slots.block[i] = DecideCargoSlot(first + i, labels[i], bitnums[i]);
	}
}

/**
 * The slot the current set's cargo went to, for its graphics and callbacks
 * (Action 3); a shared cargo takes those of the set that gives them last,
 * as it takes its other properties.
 * @param local_id The set's own id for the cargo.
 * @return The slot, or INVALID_CARGO when the id leads to another set's cargo it does not share.
 */
CargoType CargoSlotForSpriteGroup(uint local_id)
{
	const GRFFile *grf = _cur_gps.grffile;
	CargoType mine = _cargo_slots.PlacedBy(grf)[local_id];
	if (IsValidCargoType(mine)) return mine;
	CargoType natural = static_cast<CargoType>(local_id);
	const GRFFile *owner = _cargo_slots.owner[natural];
	return owner == nullptr || (owner == grf && _cargo_slots.owner_id[natural] == local_id) ? natural : INVALID_CARGO;
}

/**
 * The slot an id of the current Action 0 goes to.
 * @param local The set's own id.
 * @return The slot, or INVALID_CARGO when its properties are not applied.
 */
static CargoType CargoSlotInBlock(uint local)
{
	if (local >= _cargo_slots.block_first && local - _cargo_slots.block_first < _cargo_slots.block.size()) {
		return _cargo_slots.block[local - _cargo_slots.block_first];
	}
	return static_cast<CargoType>(local);
}

/**
 * Define properties for cargoes
 * @param first ID of the first cargo.
 * @param last ID of the last cargo.
 * @param prop The property to change.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult CargoReserveInfo(uint first, uint last, int prop, ByteReader &buf)
{
	ChangeInfoResult ret = ChangeInfoResult::Success;

	if (last > NUM_CARGO) {
		GrfMsg(2, "CargoChangeInfo: Cargo type {} out of range (max {})", last, NUM_CARGO - 1);
		return ChangeInfoResult::InvalidId;
	}

	/* Where the properties of a cargo that is not applied go (see CargoSlots). */
	static CargoSpec not_applied;

	for (uint id = first; id < last; ++id) {
		CargoType slot = CargoSlotInBlock(id);
		CargoSpec *cs = IsValidCargoType(slot) ? CargoSpec::Get(slot) : &not_applied;

		switch (prop) {
			case 0x08: // Bit number of cargo
				if (cs == &not_applied) {
					buf.ReadByte();
					break;
				}
				cs->bitnum = buf.ReadByte();
				if (cs->IsValid()) {
					cs->grffile = _cur_gps.grffile;
					_cargo_mask.Set(cs->Index());
				} else {
					_cargo_mask.Reset(cs->Index());
				}
				BuildCargoLabelMap();
				break;

			case 0x09: // String ID for cargo type name
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->name);
				break;

			case 0x0A: // String for 1 unit of cargo
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->name_single);
				break;

			case 0x0B: // String for singular quantity of cargo (e.g. 1 tonne of coal)
			case 0x1B: // String for cargo units
				/* String for units of cargo. This is different in OpenTTD
				 * (e.g. tonnes) to TTDPatch (e.g. {COMMA} tonne of coal).
				 * Property 1B is used to set OpenTTD's behaviour. */
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->units_volume);
				break;

			case 0x0C: // String for plural quantity of cargo (e.g. 10 tonnes of coal)
			case 0x1C: // String for any amount of cargo
				/* Strings for an amount of cargo. This is different in OpenTTD
				 * (e.g. {WEIGHT} of coal) to TTDPatch (e.g. {COMMA} tonnes of coal).
				 * Property 1C is used to set OpenTTD's behaviour. */
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->quantifier);
				break;

			case 0x0D: // String for two letter cargo abbreviation
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &cs->abbrev);
				break;

			case 0x0E: // Sprite ID for cargo icon
				cs->sprite = buf.ReadWord();
				break;

			case 0x0F: // Weight of one unit of cargo
				cs->weight = buf.ReadByte();
				break;

			case 0x10: // Used for payment calculation
				cs->transit_periods[0] = buf.ReadByte();
				break;

			case 0x11: // Used for payment calculation
				cs->transit_periods[1] = buf.ReadByte();
				break;

			case 0x12: // Base cargo price
				cs->initial_payment = buf.ReadDWord();
				break;

			case 0x13: // Colour for station rating bars
				cs->rating_colour = PixelColour{buf.ReadByte()};
				break;

			case 0x14: // Colour for cargo graph
				cs->legend_colour = PixelColour{buf.ReadByte()};
				break;

			case 0x15: // Freight status
				cs->is_freight = (buf.ReadByte() != 0);
				break;

			case 0x16: // Cargo classes
				cs->classes = CargoClasses{buf.ReadWord()};
				break;

			case 0x17: { // Cargo label
				CargoLabel label{std::byteswap(buf.ReadDWord())};
				if (cs != &not_applied) {
					cs->label = label;
					BuildCargoLabelMap();
				}
				/* The set's own id leads to the label wherever the cargo went. */
				MaybeInstallFallbackCargoLabel(id, last, label);
				break;
			}

			case 0x18: { // Town growth substitute type
				uint8_t substitute_type = buf.ReadByte();

				switch (substitute_type) {
					case 0x00: cs->town_acceptance_effect = TownAcceptanceEffect::Passengers; break;
					case 0x02: cs->town_acceptance_effect = TownAcceptanceEffect::Mail; break;
					case 0x05: cs->town_acceptance_effect = TownAcceptanceEffect::Goods; break;
					case 0x09: cs->town_acceptance_effect = TownAcceptanceEffect::Water; break;
					case 0x0B: cs->town_acceptance_effect = TownAcceptanceEffect::Food; break;
					default:
						GrfMsg(1, "CargoChangeInfo: Unknown town growth substitute value {}, setting to none.", substitute_type);
						[[fallthrough]];
					case 0xFF: cs->town_acceptance_effect = TownAcceptanceEffect::None; break;
				}
				break;
			}

			case 0x19: // Town growth coefficient
				buf.ReadWord();
				break;

			case 0x1A: // Bitmask of callbacks to use
				cs->callback_mask = static_cast<CargoCallbackMasks>(buf.ReadByte());
				break;

			case 0x1D: // Vehicle capacity multiplier
				cs->multiplier = std::max<uint16_t>(1u, buf.ReadWord());
				break;

			case 0x1E: { // Town production substitute type
				uint8_t substitute_type = buf.ReadByte();

				switch (substitute_type) {
					case 0x00: cs->town_production_effect = TownProductionEffect::Passengers; break;
					case 0x02: cs->town_production_effect = TownProductionEffect::Mail; break;
					default:
						GrfMsg(1, "CargoChangeInfo: Unknown town production substitute value {}, setting to none.", substitute_type);
						[[fallthrough]];
					case 0xFF: cs->town_production_effect = TownProductionEffect::None; break;
				}
				break;
			}

			case 0x1F: // Town production multiplier
				cs->town_production_multiplier = std::max<uint16_t>(1U, buf.ReadWord());
				break;

			default:
				ret = ChangeInfoResult::Unknown;
				break;
		}
	}

	return ret;
}

/** @copydoc GrfChangeInfoHandler::Reserve */
template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::Cargoes>::Reserve(uint first, uint last, int prop, ByteReader &buf) { return CargoReserveInfo(first, last, prop, buf); }
/** @copybrief GrfChangeInfoHandler::Activation @return Always ChangeInfoResult::Unhandled. */
template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::Cargoes>::Activation(uint, uint, int, ByteReader &) { return ChangeInfoResult::Unhandled; }
