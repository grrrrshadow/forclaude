/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_act14.cpp NewGRF Action 0x14 handler. */

#include "../stdafx.h"
#include "../debug.h"
#include "../newgrf_text.h"
#include "newgrf_bytereader.h"
#include "newgrf_internal.h"

#include "../safeguards.h"

/** Callback function for 'INFO'->'NAME' to add a translation to the newgrf name. @copydoc TextHandler */
static bool ChangeGRFName(GRFLanguage langid, std::string_view str)
{
	AddGRFTextToList(_cur_gps.grfconfig->name, langid, _cur_gps.grfconfig->ident.grfid, false, str);
	return true;
}

/** Callback function for 'INFO'->'DESC' to add a translation to the newgrf description. @copydoc TextHandler */
static bool ChangeGRFDescription(GRFLanguage langid, std::string_view str)
{
	AddGRFTextToList(_cur_gps.grfconfig->info, langid, _cur_gps.grfconfig->ident.grfid, true, str);
	return true;
}

/** Callback function for 'INFO'->'URL_' to set the newgrf url. @copydoc TextHandler */
static bool ChangeGRFURL(GRFLanguage langid, std::string_view str)
{
	AddGRFTextToList(_cur_gps.grfconfig->url, langid, _cur_gps.grfconfig->ident.grfid, false, str);
	return true;
}

/** Callback function for 'INFO'->'NPAR' to set the number of valid parameters. @copydoc DataHandler */
static bool ChangeGRFNumUsedParams(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected only 1 byte for 'INFO'->'NPAR' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_cur_gps.grfconfig->num_valid_params = std::min(buf.ReadByte(), GRFConfig::MAX_NUM_PARAMS);
	}
	return true;
}

/** Callback function for 'INFO'->'PALS' to set the number of valid parameters. @copydoc DataHandler */
static bool ChangeGRFPalette(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected only 1 byte for 'INFO'->'PALS' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		char data = buf.ReadByte();
		GRFPalette pal = GRFP_GRF_UNSET;
		switch (data) {
			case '*':
			case 'A': pal = GRFP_GRF_ANY;     break;
			case 'W': pal = GRFP_GRF_WINDOWS; break;
			case 'D': pal = GRFP_GRF_DOS;     break;
			default:
				GrfMsg(2, "StaticGRFInfo: unexpected value '{:02X}' for 'INFO'->'PALS', ignoring this field", data);
				break;
		}
		if (pal != GRFP_GRF_UNSET) {
			_cur_gps.grfconfig->palette &= ~GRFP_GRF_MASK;
			_cur_gps.grfconfig->palette |= pal;
		}
	}
	return true;
}

/** Callback function for 'INFO'->'BLTR' to set the blitter info. @copydoc DataHandler */
static bool ChangeGRFBlitter(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected only 1 byte for 'INFO'->'BLTR' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		char data = buf.ReadByte();
		GRFPalette pal = GRFP_BLT_UNSET;
		switch (data) {
			case '8': pal = GRFP_BLT_UNSET; break;
			case '3': pal = GRFP_BLT_32BPP;  break;
			default:
				GrfMsg(2, "StaticGRFInfo: unexpected value '{:02X}' for 'INFO'->'BLTR', ignoring this field", data);
				return true;
		}
		_cur_gps.grfconfig->palette &= ~GRFP_BLT_MASK;
		_cur_gps.grfconfig->palette |= pal;
	}
	return true;
}

/** Callback function for 'INFO'->'VRSN' to the version of the NewGRF. @copydoc DataHandler */
static bool ChangeGRFVersion(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "StaticGRFInfo: expected 4 bytes for 'INFO'->'VRSN' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		/* Set min_loadable_version as well (default to minimal compatibility) */
		_cur_gps.grfconfig->version = _cur_gps.grfconfig->min_loadable_version = buf.ReadDWord();
	}
	return true;
}

/** Callback function for 'INFO'->'MINV' to the minimum compatible version of the NewGRF. @copydoc DataHandler */
static bool ChangeGRFMinVersion(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "StaticGRFInfo: expected 4 bytes for 'INFO'->'MINV' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_cur_gps.grfconfig->min_loadable_version = buf.ReadDWord();
		if (_cur_gps.grfconfig->version == 0) {
			GrfMsg(2, "StaticGRFInfo: 'MINV' defined before 'VRSN' or 'VRSN' set to 0, ignoring this field");
			_cur_gps.grfconfig->min_loadable_version = 0;
		}
		if (_cur_gps.grfconfig->version < _cur_gps.grfconfig->min_loadable_version) {
			GrfMsg(2, "StaticGRFInfo: 'MINV' defined as {}, limiting it to 'VRSN'", _cur_gps.grfconfig->min_loadable_version);
			_cur_gps.grfconfig->min_loadable_version = _cur_gps.grfconfig->version;
		}
	}
	return true;
}

static GRFParameterInfo *_cur_parameter; ///< The parameter which info is currently changed by the newgrf.

/** Callback function for 'INFO'->'PARAM'->param_num->'NAME' to set the name of a parameter. @copydoc TextHandler */
static bool ChangeGRFParamName(GRFLanguage langid, std::string_view str)
{
	AddGRFTextToList(_cur_parameter->name, langid, _cur_gps.grfconfig->ident.grfid, false, str);
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'DESC' to set the description of a parameter. @copydoc TextHandler */
static bool ChangeGRFParamDescription(GRFLanguage langid, std::string_view str)
{
	AddGRFTextToList(_cur_parameter->desc, langid, _cur_gps.grfconfig->ident.grfid, true, str);
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'TYPE' to set the typeof a parameter. @copydoc DataHandler */
static bool ChangeGRFParamType(size_t len, ByteReader &buf)
{
	if (len != 1) {
		GrfMsg(2, "StaticGRFInfo: expected 1 byte for 'INFO'->'PARA'->'TYPE' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		GRFParameterType type = static_cast<GRFParameterType>(buf.ReadByte());
		switch (type) {
			case GRFParameterType::UintEnum:
			case GRFParameterType::Bool:
				_cur_parameter->type = type;
				break;

			default:
				GrfMsg(3, "StaticGRFInfo: unknown parameter type {}, ignoring this field", type);
				break;
		}
	}
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'LIMI' to set the min/max value of a parameter. @copydoc DataHandler */
static bool ChangeGRFParamLimits(size_t len, ByteReader &buf)
{
	if (_cur_parameter->type != GRFParameterType::UintEnum) {
		GrfMsg(2, "StaticGRFInfo: 'INFO'->'PARA'->'LIMI' is only valid for parameters with type uint/enum, ignoring this field");
		buf.Skip(len);
	} else if (len != 8) {
		GrfMsg(2, "StaticGRFInfo: expected 8 bytes for 'INFO'->'PARA'->'LIMI' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		uint32_t min_value = buf.ReadDWord();
		uint32_t max_value = buf.ReadDWord();
		if (min_value <= max_value) {
			_cur_parameter->min_value = min_value;
			_cur_parameter->max_value = max_value;
		} else {
			GrfMsg(2, "StaticGRFInfo: 'INFO'->'PARA'->'LIMI' values are incoherent, ignoring this field");
		}
	}
	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'MASK' to set the parameter and bits to use. @copydoc DataHandler */
static bool ChangeGRFParamMask(size_t len, ByteReader &buf)
{
	if (len < 1 || len > 3) {
		GrfMsg(2, "StaticGRFInfo: expected 1 to 3 bytes for 'INFO'->'PARA'->'MASK' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		uint8_t param_nr = buf.ReadByte();
		if (param_nr >= GRFConfig::MAX_NUM_PARAMS) {
			GrfMsg(2, "StaticGRFInfo: invalid parameter number in 'INFO'->'PARA'->'MASK', param {}, ignoring this field", param_nr);
			buf.Skip(len - 1);
		} else {
			_cur_parameter->param_nr = param_nr;
			if (len >= 2) _cur_parameter->first_bit = std::min<uint8_t>(buf.ReadByte(), 31);
			if (len >= 3) _cur_parameter->num_bit = std::min<uint8_t>(buf.ReadByte(), 32 - _cur_parameter->first_bit);
		}
	}

	return true;
}

/** Callback function for 'INFO'->'PARAM'->param_num->'DFLT' to set the default value. @copydoc DataHandler */
static bool ChangeGRFParamDefault(size_t len, ByteReader &buf)
{
	if (len != 4) {
		GrfMsg(2, "StaticGRFInfo: expected 4 bytes for 'INFO'->'PARA'->'DEFA' but got {}, ignoring this field", len);
		buf.Skip(len);
	} else {
		_cur_parameter->def_value = buf.ReadDWord();
	}
	_cur_gps.grfconfig->has_param_defaults = true;
	return true;
}

/**
 * Callback to read binary data.
 * @param len The number of bytes to read.
 * @param buf The buffer to read from.
 * @return \c true iff the data could be processed.
 */
using DataHandler = bool(*)(size_t len, ByteReader &buf);

/**
 * Callback to read text data.
 * @param langid The language the text is for.
 * @param str The actual text.
 * @return \c true iff the data could be processed.
 */
using TextHandler = bool(*)(GRFLanguage langid, std::string_view str);

/**
 * Callback for parsing branch nodes.
 * @param buf The buffer to read from.
 * @return \c true iff the data could be processed.
 */
using BranchHandler = bool(*)(ByteReader &buf);

/**
 * Data structure to store the allowed id/type combinations for action 14. The
 * data can be represented as a tree with 3 types of nodes:
 * 1. Branch nodes (identified by 'C' for choice).
 * 2. Binary leaf nodes (identified by 'B').
 * 3. Text leaf nodes (identified by 'T').
 */
struct AllowedSubtags {
	/** Custom 'span' of subtags. Required because std::span with an incomplete type is UB. */
	using Span = std::pair<const AllowedSubtags *, const AllowedSubtags *>;

	uint32_t id; ///< The identifier for this node.
	std::variant<DataHandler, TextHandler, BranchHandler, Span> handler; ///< The handler for this node.
};

static bool SkipUnknownInfo(ByteReader &buf, uint8_t type);
static bool HandleNodes(ByteReader &buf, std::span<const AllowedSubtags> tags);

/**
 * Callback function for 'INFO'->'PARA'->param_num->'VALU' to set the names
 * of some parameter values (type uint/enum) or the names of some bits
 * (type bitmask). In both cases the format is the same:
 * Each subnode should be a text node with the value/bit number as id.
 * @copydoc BranchHandler
 */
static bool ChangeGRFParamValueNames(ByteReader &buf)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		uint32_t id = buf.ReadDWord();
		if (type != 'T' || id > _cur_parameter->max_value) {
			GrfMsg(2, "StaticGRFInfo: all child nodes of 'INFO'->'PARA'->param_num->'VALU' should have type 't' and the value/bit number as id");
			if (!SkipUnknownInfo(buf, type)) return false;
			type = buf.ReadByte();
			continue;
		}

		GRFLanguage langid = static_cast<GRFLanguage>(buf.ReadByte());
		std::string_view name_string = buf.ReadString();

		auto it = std::ranges::lower_bound(_cur_parameter->value_names, id, std::less{}, &GRFParameterInfo::ValueName::first);
		if (it == std::end(_cur_parameter->value_names) || it->first != id) {
			it = _cur_parameter->value_names.emplace(it, id, GRFTextList{});
		}
		AddGRFTextToList(it->second, langid, _cur_gps.grfconfig->ident.grfid, false, name_string);

		type = buf.ReadByte();
	}
	return true;
}

/** Action14 parameter tags */
static constexpr AllowedSubtags _tags_parameters[] = {
	AllowedSubtags{'NAME', ChangeGRFParamName},
	AllowedSubtags{'DESC', ChangeGRFParamDescription},
	AllowedSubtags{'TYPE', ChangeGRFParamType},
	AllowedSubtags{'LIMI', ChangeGRFParamLimits},
	AllowedSubtags{'MASK', ChangeGRFParamMask},
	AllowedSubtags{'VALU', ChangeGRFParamValueNames},
	AllowedSubtags{'DFLT', ChangeGRFParamDefault},
};

/**
 * Callback function for 'INFO'->'PARA' to set extra information about the
 * parameters. Each subnode of 'INFO'->'PARA' should be a branch node with
 * the parameter number as id. The first parameter has id 0. The maximum
 * parameter that can be changed is set by 'INFO'->'NPAR' which defaults to 80.
 * @copydoc BranchHandler
 */
static bool HandleParameterInfo(ByteReader &buf)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		uint32_t id = buf.ReadDWord();
		if (type != 'C' || id >= _cur_gps.grfconfig->num_valid_params) {
			GrfMsg(2, "StaticGRFInfo: all child nodes of 'INFO'->'PARA' should have type 'C' and their parameter number as id");
			if (!SkipUnknownInfo(buf, type)) return false;
			type = buf.ReadByte();
			continue;
		}

		if (id >= _cur_gps.grfconfig->param_info.size()) {
			_cur_gps.grfconfig->param_info.resize(id + 1);
		}
		if (!_cur_gps.grfconfig->param_info[id].has_value()) {
			_cur_gps.grfconfig->param_info[id] = GRFParameterInfo(id);
		}
		_cur_parameter = &_cur_gps.grfconfig->param_info[id].value();
		/* Read all parameter-data and process each node. */
		if (!HandleNodes(buf, _tags_parameters)) return false;
		type = buf.ReadByte();
	}
	return true;
}

/** Action14 tags for the INFO node */
static constexpr AllowedSubtags _tags_info[] = {
	AllowedSubtags{'NAME', ChangeGRFName},
	AllowedSubtags{'DESC', ChangeGRFDescription},
	AllowedSubtags{'URL_', ChangeGRFURL},
	AllowedSubtags{'NPAR', ChangeGRFNumUsedParams},
	AllowedSubtags{'PALS', ChangeGRFPalette},
	AllowedSubtags{'BLTR', ChangeGRFBlitter},
	AllowedSubtags{'VRSN', ChangeGRFVersion},
	AllowedSubtags{'MINV', ChangeGRFMinVersion},
	AllowedSubtags{'PARA', HandleParameterInfo},
};

/**
 * What a set built for JGR's patchpack says about itself, and what this game
 * answers. Two blocks matter, both read here:
 *
 * 'A0PM' asks for an Action 0 property by name and says which property number
 * it will use for it. The patchpack gave its added properties no fixed
 * numbers, so a set cannot use one without saying this first, and we cannot
 * read one without listening.
 *
 * 'FTST' asks whether a feature is there at all. The answer goes back through
 * global variable 0x8D (a bit) or 0x91 (a value), which the set then tests
 * with an Action 7 or 9 and skips its own sprites if the answer is no. Left
 * unanswered, a set draws what it would draw on plain OpenTTD -- which is why
 * this is read even though nothing else here needs it.
 *
 * Only the names this game can honour are accepted. A name we do not know is
 * left unmapped and reported, and the set carries on without that feature,
 * which is what a set that names an unimplemented property has to expect.
 */
struct GRFNameMapAction {
	std::string name{};
	int feature = -1;
	int prop_id = -1;
	int var8d_bit = -1;
	uint32_t var91_value = 0;

	void Reset() { *this = GRFNameMapAction{}; }
};
static GRFNameMapAction _cur_name_map_action;

/** Known Action 0 property names, by the patchpack's spelling. */
struct MappableProperty {
	const char *name;
	GrfSpecFeature feature;
	GRFFile::MappedProperty id;
};
static const MappableProperty _mappable_properties[] = {
	{ "railtype_extra_aspects", GrfSpecFeature::RailTypes, GRFFile::MappedProperty::RailtypeExtraAspects },
};

/** Known feature names, and the version of each this game answers to. */
struct KnownFeature {
	const char *name;
	uint8_t version;
};
static const KnownFeature _known_features[] = {
	{ "action0_railtype_extra_aspects", 1 },
};

/** @copydoc TextHandler */
static bool ChangeNameMapName(GRFLanguage, std::string_view str)
{
	_cur_name_map_action.name = str;
	return true;
}

/** @copydoc DataHandler */
static bool ChangeNameMapFeature(size_t len, ByteReader &buf)
{
	if (len != 1) { buf.Skip(len); return true; }
	_cur_name_map_action.feature = buf.ReadByte();
	return true;
}

/** @copydoc DataHandler */
static bool ChangeNameMapPropertyId(size_t len, ByteReader &buf)
{
	if (len != 1) { buf.Skip(len); return true; }
	_cur_name_map_action.prop_id = buf.ReadByte();
	return true;
}

/** @copydoc DataHandler */
static bool ChangeNameMapVar8DBit(size_t len, ByteReader &buf)
{
	if (len != 1) { buf.Skip(len); return true; }
	_cur_name_map_action.var8d_bit = buf.ReadByte();
	return true;
}

/** @copydoc DataHandler */
static bool ChangeNameMapVar91Value(size_t len, ByteReader &buf)
{
	if (len != 4) { buf.Skip(len); return true; }
	_cur_name_map_action.var91_value = buf.ReadDWord();
	return true;
}

/** @copydoc DataHandler */
static bool ChangeNameMapMinVersion(size_t len, ByteReader &buf)
{
	if (len != 1) { buf.Skip(len); return true; }
	/* Kept as the feature's asked-for version; answered against what we do. */
	_cur_name_map_action.prop_id = buf.ReadByte();
	return true;
}

/** Action14 'A0PM' tags: name a property, and say which number it will use. */
static constexpr AllowedSubtags _tags_a0pm[] = {
	AllowedSubtags{'NAME', ChangeNameMapName},
	AllowedSubtags{'FEAT', ChangeNameMapFeature},
	AllowedSubtags{'PROP', ChangeNameMapPropertyId},
	AllowedSubtags{'SETT', ChangeNameMapVar8DBit},
	AllowedSubtags{'SVAL', ChangeNameMapVar91Value},
};

/** Action14 'FTST' tags: name a feature and ask whether it is here. */
static constexpr AllowedSubtags _tags_ftst[] = {
	AllowedSubtags{'NAME', ChangeNameMapName},
	AllowedSubtags{'MINV', ChangeNameMapMinVersion},
	AllowedSubtags{'SETP', ChangeNameMapVar8DBit},
	AllowedSubtags{'SVAL', ChangeNameMapVar91Value},
};

/** Say that a test or mapping came out true, the way the set asked to be told. */
static void AnswerNameMap(bool success)
{
	const GRFNameMapAction &action = _cur_name_map_action;
	if (!success) return;
	if (action.var8d_bit >= 0 && action.var8d_bit < 32) SetBit(_cur_gps.grfconfig->feature_test_var8d, action.var8d_bit);
	if (action.var91_value != 0) {
		std::vector<uint32_t> &values = _cur_gps.grfconfig->feature_test_var91;
		if (std::ranges::find(values, action.var91_value) == std::end(values)) values.push_back(action.var91_value);
	}
}

/** @copydoc BranchHandler */
static bool HandleAction0PropertyMap(ByteReader &buf)
{
	_cur_name_map_action.Reset();
	if (!HandleNodes(buf, _tags_a0pm)) return false;

	const GRFNameMapAction &action = _cur_name_map_action;
	if (action.name.empty() || action.feature < 0 || action.prop_id <= 0) {
		GrfMsg(2, "StaticGRFInfo: 'A0PM' without a name, feature or property number, ignoring");
		return true;
	}
	for (const MappableProperty &known : _mappable_properties) {
		if (to_underlying(known.feature) != action.feature || action.name != known.name) continue;
		_cur_gps.grfconfig->action0_property_remaps[{static_cast<uint8_t>(action.feature), static_cast<uint8_t>(action.prop_id)}] = to_underlying(known.id);
		GrfMsg(2, "StaticGRFInfo: property '{}' of feature {:02X} read as number {:02X}", action.name, action.feature, action.prop_id);
		AnswerNameMap(true);
		return true;
	}
	GrfMsg(1, "StaticGRFInfo: property '{}' of feature {:02X} is not implemented, leaving it unmapped", action.name, action.feature);
	return true;
}

/** @copydoc BranchHandler */
static bool HandleFeatureTest(ByteReader &buf)
{
	_cur_name_map_action.Reset();
	if (!HandleNodes(buf, _tags_ftst)) return false;

	const GRFNameMapAction &action = _cur_name_map_action;
	if (action.name.empty()) return true;
	/* prop_id carries the asked-for minimum version here; see ChangeNameMapMinVersion(). */
	uint8_t want = action.prop_id <= 0 ? 1 : static_cast<uint8_t>(action.prop_id);
	for (const KnownFeature &known : _known_features) {
		if (action.name != known.name || known.version < want) continue;
		GrfMsg(2, "StaticGRFInfo: feature '{}' asked for and answered", action.name);
		AnswerNameMap(true);
		return true;
	}
	GrfMsg(2, "StaticGRFInfo: feature '{}' asked for and not here", action.name);
	return true;
}

/** Action14 root tags */
static constexpr AllowedSubtags _tags_root[] = {
	AllowedSubtags{'INFO', std::make_pair(std::begin(_tags_info), std::end(_tags_info))},
	AllowedSubtags{'FTST', HandleFeatureTest},
	AllowedSubtags{'A0PM', HandleAction0PropertyMap},
};


/**
 * Try to skip the current node and all subnodes (if it's a branch node).
 * @param buf Buffer.
 * @param type The node type to skip.
 * @return True if we could skip the node, false if an error occurred.
 */
static bool SkipUnknownInfo(ByteReader &buf, uint8_t type)
{
	/* type and id are already read */
	switch (type) {
		case 'C': {
			uint8_t new_type = buf.ReadByte();
			while (new_type != 0) {
				buf.ReadDWord(); // skip the id
				if (!SkipUnknownInfo(buf, new_type)) return false;
				new_type = buf.ReadByte();
			}
			break;
		}

		case 'T':
			buf.ReadByte(); // lang
			buf.ReadString(); // actual text
			break;

		case 'B': {
			uint16_t size = buf.ReadWord();
			buf.Skip(size);
			break;
		}

		default:
			return false;
	}

	return true;
}

/**
 * Handle the nodes of an Action14
 * @param type Type of node.
 * @param id ID.
 * @param buf Buffer.
 * @param subtags Allowed subtags.
 * @return Whether all tags could be handled.
 */
static bool HandleNode(uint8_t type, uint32_t id, ByteReader &buf, std::span<const AllowedSubtags> subtags)
{
	/* Visitor to get a subtag handler's type. */
	struct type_visitor {
		char operator()(const DataHandler &) { return 'B'; }
		char operator()(const TextHandler &) { return 'T'; }
		char operator()(const BranchHandler &) { return 'C'; }
		char operator()(const AllowedSubtags::Span &) { return 'C'; }
	};

	/* Visitor to evaluate a subtag handler. */
	struct evaluate_visitor {
		ByteReader &buf;

		bool operator()(const DataHandler &handler)
		{
			size_t len = buf.ReadWord();
			if (buf.Remaining() < len) return false;
			return handler(len, buf);
		}

		bool operator()(const TextHandler &handler)
		{
			GRFLanguage langid = static_cast<GRFLanguage>(buf.ReadByte());
			return handler(langid, buf.ReadString());
		}

		bool operator()(const BranchHandler &handler)
		{
			return handler(buf);
		}

		bool operator()(const AllowedSubtags::Span &subtags)
		{
			return HandleNodes(buf, {subtags.first, subtags.second});
		}
	};

	for (const auto &tag : subtags) {
		if (tag.id != std::byteswap(id) || std::visit(type_visitor{}, tag.handler) != type) continue;
		return std::visit(evaluate_visitor{buf}, tag.handler);
	}

	GrfMsg(2, "StaticGRFInfo: unknown type/id combination found, type={:c}, id={:x}", type, id);
	return SkipUnknownInfo(buf, type);
}

/**
 * Handle the contents of a 'C' choice of an Action14
 * @param buf Buffer.
 * @param subtags List of subtags.
 * @return Whether the nodes could all be handled.
 */
static bool HandleNodes(ByteReader &buf, std::span<const AllowedSubtags> subtags)
{
	uint8_t type = buf.ReadByte();
	while (type != 0) {
		uint32_t id = buf.ReadDWord();
		if (!HandleNode(type, id, buf, subtags)) return false;
		type = buf.ReadByte();
	}
	return true;
}

/**
 * Handle Action 0x14
 * @param buf Buffer.
 */
static void StaticGRFInfo(ByteReader &buf)
{
	/* <14> <type> <id> <text/data...> */
	HandleNodes(buf, _tags_root);
}

/** @copydoc GrfActionHandler::FileScan */
template <> void GrfActionHandler<0x14>::FileScan(ByteReader &buf) { StaticGRFInfo(buf); }
/** @copybrief GrfActionHandler::SafetyScan */
template <> void GrfActionHandler<0x14>::SafetyScan(ByteReader &) { }
/** @copybrief GrfActionHandler::LabelScan */
template <> void GrfActionHandler<0x14>::LabelScan(ByteReader &) { }
/** @copybrief GrfActionHandler::Init */
template <> void GrfActionHandler<0x14>::Init(ByteReader &) { }
/** @copybrief GrfActionHandler::Reserve */
template <> void GrfActionHandler<0x14>::Reserve(ByteReader &) { }
/** @copybrief GrfActionHandler::Activation */
template <> void GrfActionHandler<0x14>::Activation(ByteReader &) { }
