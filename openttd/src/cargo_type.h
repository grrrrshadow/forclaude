/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargo_type.h Types related to cargoes... */

#ifndef CARGO_TYPE_H
#define CARGO_TYPE_H

#include "core/strong_typedef_type.hpp"
#include "core/enum_type.hpp"

/** Globally unique label of a cargo type. */
using CargoLabel = StrongType::Typedef<uint32_t, struct CargoLabelTag, StrongType::Compare>;

/**
 * Cargo slots to indicate a cargo type within a game.
 */
enum CargoType : uint8_t {};
DECLARE_INCREMENT_DECREMENT_OPERATORS(CargoType)

/**
 * Available types of cargo
 * Labels may be re-used between different climates.
 */

/* Temperate */
static constexpr CargoLabel CT_PASSENGERS{'PASS'};
static constexpr CargoLabel CT_COAL{'COAL'};
static constexpr CargoLabel CT_MAIL{'MAIL'};
static constexpr CargoLabel CT_OIL{'OIL_'};
static constexpr CargoLabel CT_LIVESTOCK{'LVST'};
static constexpr CargoLabel CT_GOODS{'GOOD'};
static constexpr CargoLabel CT_GRAIN{'GRAI'};
static constexpr CargoLabel CT_WOOD{'WOOD'};
static constexpr CargoLabel CT_IRON_ORE{'IORE'};
static constexpr CargoLabel CT_STEEL{'STEL'};
static constexpr CargoLabel CT_VALUABLES{'VALU'};

/* Arctic */
static constexpr CargoLabel CT_WHEAT{'WHEA'};
static constexpr CargoLabel CT_PAPER{'PAPR'};
static constexpr CargoLabel CT_GOLD{'GOLD'};
static constexpr CargoLabel CT_FOOD{'FOOD'};

/* Tropic */
static constexpr CargoLabel CT_RUBBER{'RUBR'};
static constexpr CargoLabel CT_FRUIT{'FRUT'};
static constexpr CargoLabel CT_MAIZE{'MAIZ'};
static constexpr CargoLabel CT_COPPER_ORE{'CORE'};
static constexpr CargoLabel CT_WATER{'WATR'};
static constexpr CargoLabel CT_DIAMONDS{'DIAM'};

/* Toyland */
static constexpr CargoLabel CT_SUGAR{'SUGR'};
static constexpr CargoLabel CT_TOYS{'TOYS'};
static constexpr CargoLabel CT_BATTERIES{'BATT'};
static constexpr CargoLabel CT_CANDY{'SWET'};
static constexpr CargoLabel CT_TOFFEE{'TOFF'};
static constexpr CargoLabel CT_COLA{'COLA'};
static constexpr CargoLabel CT_COTTON_CANDY{'CTCD'};
static constexpr CargoLabel CT_BUBBLES{'BUBL'};
static constexpr CargoLabel CT_PLASTIC{'PLST'};
static constexpr CargoLabel CT_FIZZY_DRINKS{'FZDR'};

/** Dummy label for engines that carry no cargo; they actually carry 0 passengers. */
static constexpr CargoLabel CT_NONE = CT_PASSENGERS;

/**
 * Road vehicles carried on rail wagons -- "Rollende Landstrasse", ROLA. A real
 * cargo so that a wagon fitted for it has a capacity (one vehicle), is full
 * or empty like any other, and can be named in a set's cargo table by its
 * label; no industry makes it and no station takes it, the vehicle on the
 * wagon is the cargo. It sits in the topmost cargo slot, out of the way of
 * every set that defines its own cargoes from the bottom up. See
 * road_on_rail.h.
 */
static constexpr CargoLabel CT_ROLA{'ROLA'};

static constexpr CargoLabel CT_INVALID{UINT32_MAX}; ///< Invalid cargo type.

static constexpr CargoType NUM_ORIGINAL_CARGO{12}; ///< Original number of cargo types.
static constexpr CargoType NUM_CARGO{128}; ///< Maximum number of cargo types in a game. Two words of CargoTypes: two large industry sets side by side define more than 64 between them.

/* CARGO_AUTO_REFIT and CARGO_NO_REFIT are stored in save-games for refit-orders, so should not be changed. */
static constexpr CargoType CARGO_AUTO_REFIT{0xFD}; ///< Automatically choose cargo type when doing auto refitting.
static constexpr CargoType CARGO_NO_REFIT{0xFE}; ///< Do not refit cargo of a vehicle (used in vehicle orders and auto-replace/auto-renew).

static constexpr CargoType INVALID_CARGO{UINT8_MAX};

/** Mixed cargo types for definitions with cargo that can vary depending on climate. */
enum MixedCargoType : uint8_t {
	MCT_LIVESTOCK_FRUIT, ///< Cargo can be livestock or fruit.
	MCT_GRAIN_WHEAT_MAIZE, ///< Cargo can be grain, wheat or maize.
	MCT_VALUABLES_GOLD_DIAMONDS, ///< Cargo can be valuables, gold or diamonds.
};

/**
 * Special cargo filter criteria.
 * These are used by user interface code only and must not be assigned to any entity. Not all values are valid for every UI filter.
 */
namespace CargoFilterCriteria {
	static constexpr CargoType CF_ANY{NUM_CARGO}; ///< Show all items independent of carried cargo (i.e. no filtering)
	static constexpr CargoType CF_NONE{NUM_CARGO + 1}; ///< Show only items which do not carry cargo (e.g. train engines)
	static constexpr CargoType CF_ENGINES{NUM_CARGO + 2}; ///< Show only engines (for rail vehicles only)
	static constexpr CargoType CF_FREIGHT{NUM_CARGO + 3}; ///< Show only vehicles which carry any freight (non-passenger) cargo

	static constexpr CargoType CF_NO_RATING{NUM_CARGO + 4}; ///< Show items with no rating (station list)
	static constexpr CargoType CF_SELECT_ALL{NUM_CARGO + 5}; ///< Select all items (station list)
	static constexpr CargoType CF_EXPAND_LIST{NUM_CARGO + 6}; ///< Expand list to show all items (station list)
};

/**
 * Test whether cargo type is not INVALID_CARGO.
 * @param cargo The cargo to check.
 * @return \c false iff the cargo is INVALID_CARGO.
 */
inline bool IsValidCargoType(CargoType cargo) { return cargo != INVALID_CARGO; }

/** Bitset of \c CargoType elements. */
/**
 * A set of cargo types, one bit each, for NUM_CARGO of them.
 *
 * EnumBitSet keeps its bits in one integer and so stops at 64 cargoes. This
 * game holds 128, so this set keeps its bits in two words and offers the same
 * face: Set/Reset/Flip/Test, Any/All/None, the operators, a count, and a walk
 * over the cargoes set. It is on purpose not ConvertibleThroughBase: a mask
 * does not fit the one word a string parameter has, so it travels as a string
 * instead (StringParameter(const CargoTypes &), {CARGO_LIST}). What a NewGRF
 * or an old savegame hands over in one word is the first 64 cargoes: Low(),
 * and the constructor from one word.
 */
class CargoTypes {
public:
	static constexpr uint WORDS = (to_underlying(NUM_CARGO) + 63) / 64;
	static_assert(to_underlying(NUM_CARGO) % 64 == 0, "CargoTypes counts on whole words");

	std::array<uint64_t, WORDS> bits{}; ///< The bits, word 0 holding the cargoes 0..63. Public for the saveload tables and for nothing else.

	constexpr CargoTypes() = default;
	constexpr CargoTypes(CargoType cargo) { this->Set(cargo); }
	/** The cargoes of one word, 0..63: what a NewGRF, an old savegame or a graph's exclusion bits hand over. */
	explicit constexpr CargoTypes(uint64_t low) { this->bits[0] = low; }
	constexpr CargoTypes(std::initializer_list<const CargoType> values)
	{
		for (const CargoType &value : values) this->Set(value);
	}

	/**
	 * A set from its words.
	 * @param low the cargoes 0..63
	 * @param high the cargoes 64..127
	 * @return the set
	 */
	static constexpr CargoTypes FromWords(uint64_t low, uint64_t high)
	{
		CargoTypes result;
		result.bits[0] = low;
		result.bits[1] = high;
		return result;
	}

	constexpr auto operator<=>(const CargoTypes &) const noexcept = default;

	/** Is this a cargo of the game at all? Values beyond NUM_CARGO (INVALID_CARGO, CF_ANY...) are in no set and set nothing. */
	static constexpr bool InRange(CargoType cargo) { return to_underlying(cargo) < to_underlying(NUM_CARGO); }
	static constexpr uint Word(CargoType cargo) { return to_underlying(cargo) >> 6; }
	static constexpr uint64_t Bit(CargoType cargo) { return 1ULL << (to_underlying(cargo) & 63); }

	inline constexpr CargoTypes &Set()
	{
		this->bits.fill(UINT64_MAX);
		return *this;
	}

	inline constexpr CargoTypes &Set(CargoType cargo)
	{
		if (InRange(cargo)) this->bits[Word(cargo)] |= Bit(cargo);
		return *this;
	}

	inline constexpr CargoTypes &Set(const CargoTypes &other)
	{
		for (uint i = 0; i < WORDS; i++) this->bits[i] |= other.bits[i];
		return *this;
	}

	inline constexpr CargoTypes &Set(CargoType cargo, bool set)
	{
		return set ? this->Set(cargo) : this->Reset(cargo);
	}

	inline constexpr CargoTypes &Reset()
	{
		this->bits.fill(0);
		return *this;
	}

	inline constexpr CargoTypes &Reset(CargoType cargo)
	{
		if (InRange(cargo)) this->bits[Word(cargo)] &= ~Bit(cargo);
		return *this;
	}

	inline constexpr CargoTypes &Reset(const CargoTypes &other)
	{
		for (uint i = 0; i < WORDS; i++) this->bits[i] &= ~other.bits[i];
		return *this;
	}

	inline constexpr CargoTypes &Flip()
	{
		for (uint64_t &word : this->bits) word = ~word;
		return *this;
	}

	inline constexpr CargoTypes &Flip(CargoType cargo)
	{
		if (InRange(cargo)) this->bits[Word(cargo)] ^= Bit(cargo);
		return *this;
	}

	inline constexpr CargoTypes &Flip(const CargoTypes &other)
	{
		for (uint i = 0; i < WORDS; i++) this->bits[i] ^= other.bits[i];
		return *this;
	}

	inline constexpr bool Test(CargoType cargo) const
	{
		return InRange(cargo) && (this->bits[Word(cargo)] & Bit(cargo)) != 0;
	}

	inline constexpr bool All(const CargoTypes &other) const
	{
		for (uint i = 0; i < WORDS; i++) {
			if ((this->bits[i] & other.bits[i]) != other.bits[i]) return false;
		}
		return true;
	}

	inline constexpr bool All() const
	{
		for (uint64_t word : this->bits) {
			if (word != UINT64_MAX) return false;
		}
		return true;
	}

	inline constexpr bool Any(const CargoTypes &other) const
	{
		for (uint i = 0; i < WORDS; i++) {
			if ((this->bits[i] & other.bits[i]) != 0) return true;
		}
		return false;
	}

	inline constexpr bool Any() const
	{
		for (uint64_t word : this->bits) {
			if (word != 0) return true;
		}
		return false;
	}

	inline constexpr bool None() const { return !this->Any(); }

	inline constexpr CargoTypes &operator|=(const CargoTypes &other) { return this->Set(other); }
	inline constexpr CargoTypes operator|(const CargoTypes &other) const { CargoTypes result = *this; return result.Set(other); }
	inline constexpr CargoTypes &operator&=(const CargoTypes &other)
	{
		for (uint i = 0; i < WORDS; i++) this->bits[i] &= other.bits[i];
		return *this;
	}
	inline constexpr CargoTypes operator&(const CargoTypes &other) const { CargoTypes result = *this; return result &= other; }
	inline constexpr CargoTypes &operator^=(const CargoTypes &other) { return this->Flip(other); }
	inline constexpr CargoTypes operator^(const CargoTypes &other) const { CargoTypes result = *this; return result.Flip(other); }

	/** The first word: the cargoes 0..63, for what takes one word -- a NewGRF variable above all. */
	inline constexpr uint64_t Low() const { return this->bits[0]; }
	/** The second word: the cargoes 64..127. */
	inline constexpr uint64_t High() const { return this->bits[1]; }

	inline constexpr bool IsValid() const { return true; }

	inline uint Count() const
	{
		uint count = 0;
		for (uint64_t word : this->bits) count += std::popcount(word);
		return count;
	}

	/**
	 * The n-th cargo set, counting from the lowest.
	 * @param n which one, 0 the lowest
	 * @return the cargo, or nothing when fewer are set
	 */
	std::optional<CargoType> GetNthSetBit(uint n) const
	{
		for (CargoType cargo : *this) {
			if (n-- == 0) return cargo;
		}
		return std::nullopt;
	}

	/** Walks the cargoes set, lowest first. */
	struct Iterator {
		typedef CargoType value_type;
		typedef value_type *pointer;
		typedef value_type &reference;
		typedef size_t difference_type;
		typedef std::forward_iterator_tag iterator_category;

		explicit Iterator(const CargoTypes &set, uint position) : set(set), position(position) { this->Settle(); }
		bool operator==(const Iterator &other) const { return this->position == other.position; }
		bool operator!=(const Iterator &other) const { return this->position != other.position; }
		CargoType operator*() const { return static_cast<CargoType>(this->position); }
		Iterator &operator++() { this->position++; this->Settle(); return *this; }

	private:
		const CargoTypes &set;
		uint position; ///< The cargo looked at; NUM_CARGO once past the last.

		/** Move on to the next cargo set, or to the end. */
		void Settle()
		{
			while (this->position < to_underlying(NUM_CARGO)) {
				uint64_t rest = this->set.bits[this->position >> 6] >> (this->position & 63);
				if (rest == 0) {
					this->position = (this->position | 63) + 1;
					continue;
				}
				this->position += std::countr_zero(rest);
				return;
			}
			this->position = to_underlying(NUM_CARGO);
		}
	};

	Iterator begin() const { return Iterator(*this, 0); }
	Iterator end() const { return Iterator(*this, to_underlying(NUM_CARGO)); }
};

static constexpr CargoTypes ALL_CARGOTYPES = CargoTypes::FromWords(UINT64_MAX, UINT64_MAX);

/** Class for storing amounts of cargo */
struct CargoArray : std::array<uint, NUM_CARGO> {
	/**
	 * Get the sum of all cargo amounts.
	 * @return The sum.
	 */
	template <typename T>
	inline const T GetSum() const
	{
		return std::reduce(this->begin(), this->end(), T{});
	}

	/**
	 * Get the amount of cargos that have an amount.
	 * @return The amount.
	 */
	inline uint GetCount() const
	{
		return std::ranges::count_if(*this, [](uint amount) { return amount != 0; });
	}
};

#endif /* CARGO_TYPE_H */
