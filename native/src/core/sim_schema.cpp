#include "sim_schema.h"

namespace mesim {

const char *table_name(TableId id) {
	static const char *const kNames[kTableCount] = {
		"buildings", "zones", "road_nodes", "road_arcs", "stops", "route_pieces", "transfers", "facilities", "gateways",
		"road_classes", "transit_modes", "transit_periods", "transit_headways", "mobility", "mobility_estimates",
		"census_age", "census_household_size", "census_household_children", "census_labour", "census_study",
	};
	const size_t i = static_cast<size_t>(id);
	return i < kTableCount ? kNames[i] : "?";
}

namespace schema {

namespace {

constexpr uint64_t kU16Unknown = 0xFFFFu - 1;
constexpr uint64_t kU32Unknown = 0xFFFFFFFFull - 1;
constexpr uint64_t kU64Unknown = ~uint64_t(0) - 1;
constexpr uint64_t kShareOne = 10000;

Rule ref(TableId table, bool nullable) {
	return Rule{ RuleKind::ref, static_cast<uint8_t>(table), nullable, 0 };
}
Rule code(CodeSet set, bool nullable) {
	return Rule{ RuleKind::code, static_cast<uint8_t>(set), nullable, 0 };
}
Rule boolean() {
	return Rule{ RuleKind::boolean, 0, false, 0 };
}
Rule max(uint64_t value) {
	return Rule{ RuleKind::max, 0, false, value };
}
Rule max_or_none(uint64_t value) {
	return Rule{ RuleKind::max_or_none, 0, false, value };
}
Rule min(uint64_t value) {
	return Rule{ RuleKind::min, 0, false, value };
}
Rule mask(CodeSet set) {
	return Rule{ RuleKind::mask, static_cast<uint8_t>(set), false, 0 };
}
Rule all(uint64_t value) {
	return Rule{ RuleKind::all, 0, false, value };
}
Rule csr(uint8_t size) {
	return Rule{ RuleKind::csr, size, false, 0 };
}

ColumnSpec col(const char *name, ColumnType type, std::vector<Rule> rules = {}) {
	return ColumnSpec{ name, type, Length::rows, "", std::move(rules) };
}
ColumnSpec offsets(const char *name, uint8_t pool) {
	return ColumnSpec{ name, ColumnType::u32, Length::rows_plus_1, "", { csr(pool) } };
}
ColumnSpec pool(const char *name, ColumnType type, const char *offsets_column, std::vector<Rule> rules) {
	return ColumnSpec{ name, type, Length::pool, offsets_column, std::move(rules) };
}

ColumnSpec osm_id(const char *name) {
	return col(name, ColumnType::u64, { min(1), max(kU64Unknown - 1) });
}
ColumnSpec count32(const char *name) {
	return col(name, ColumnType::u32, { max(kU32Unknown - 1) });
}
ColumnSpec census_region() {
	return col("region", ColumnType::u8, { code(CodeSet::census_region, false) });
}

using T = ColumnType;

std::vector<TableSpec> make_tables() {
	std::vector<TableSpec> out;
	{
		TableSpec t{ TableId::buildings, 1, "estimated", {} };
		t.columns = {
			col("osm_type", T::u8, { code(CodeSet::osm_type, false) }),
			osm_id("osm_id"),
			col("x_cm", T::i32), col("y_cm", T::i32),
			col("zone", T::u32, { ref(TableId::zones, true) }),
			col("territory", T::u8, { code(CodeSet::territory, false) }),
			col("inside_mkad", T::u8, { boolean() }),
			col("function", T::u8, { code(CodeSet::building_function, false) }),
			col("function_source", T::u8, { code(CodeSet::function_source, false) }),
			count32("footprint_dm2"), count32("floor_area_dm2"), count32("residents"),
		};
		for (const std::string &function : fixed_codes(CodeSet::share_function)) {
			t.columns.push_back(ColumnSpec{ "share_" + function, T::u16, Length::rows, "", { max(kShareOne) } });
		}
		t.columns.push_back(col("road_node_depart", T::u32, { ref(TableId::road_nodes, true) }));
		t.columns.push_back(col("road_node_arrive", T::u32, { ref(TableId::road_nodes, true) }));
		out.push_back(std::move(t));
	}
	out.push_back(TableSpec{ TableId::zones, 1, "estimated", {
		col("ix", T::i32), col("iy", T::i32),
		count32("residents"), count32("area_dm2"),
		col("moscow_share", T::u16, { max(kShareOne) }),
		col("inside_mkad_share", T::u16, { max(kShareOne) }),
	} });
	out.push_back(TableSpec{ TableId::road_nodes, 1, "estimated", {
		osm_id("osm_id"),
		col("x_cm", T::i32), col("y_cm", T::i32),
		col("inside", T::u8, { boolean() }),
		col("zone", T::u32, { ref(TableId::zones, true) }),
		col("weak_component", T::u32, { ref(TableId::road_nodes, false) }),
		col("strong_component", T::u32, { ref(TableId::road_nodes, false) }),
		col("car_main", T::u8, { boolean() }),
		offsets("arc_offset", static_cast<uint8_t>(TableId::road_arcs)),
	} });
	out.push_back(TableSpec{ TableId::road_arcs, 1, "estimated", {
		col("to", T::u32, { ref(TableId::road_nodes, false) }),
		count32("length_cm"),
		col("road_class", T::u8, { code(CodeSet::road_class, false) }),
		col("car", T::u8, { boolean() }),
		col("internal", T::u8, { boolean() }),
		col("bridge", T::u8, { boolean() }),
		col("tunnel", T::u8, { boolean() }),
		col("layer", T::i8),
		col("way_id", T::u64, { min(1), max((uint64_t(1) << 63) - 1) }),
		col("way_part", T::u16, { max(kU16Unknown - 1) }),
		col("reverse", T::u8, { boolean() }),
		col("by_edge", T::u32, { ref(TableId::road_arcs, false) }),
	} });
	out.push_back(TableSpec{ TableId::stops, 1, "estimated", {
		col("osm_type", T::u8, { code(CodeSet::osm_type, false) }),
		osm_id("osm_id"),
		col("kind", T::u8, { code(CodeSet::stop_kind, false) }),
		col("x_cm", T::i32), col("y_cm", T::i32),
		col("inside", T::u8, { boolean() }),
		col("zone", T::u32, { ref(TableId::zones, true) }),
		col("modes", T::u8, { mask(CodeSet::mode) }),
	} });
	out.push_back(TableSpec{ TableId::route_pieces, 1, "estimated", {
		osm_id("route_osm_id"),
		col("piece", T::u16, { max(kU16Unknown - 1) }),
		col("mode", T::u8, { code(CodeSet::mode, false) }),
		offsets("stop_offset", size_stops_pool),
		pool("stops", T::u32, "stop_offset", { ref(TableId::stops, false) }),
		pool("segment_cm", T::u32, "stop_offset", { max(kU32Unknown - 1) }),
	} });
	out.push_back(TableSpec{ TableId::transfers, 1, "estimated", {
		osm_id("stop_area_osm_id"),
		col("from_stop", T::u32, { ref(TableId::stops, false) }),
		col("to_stop", T::u32, { ref(TableId::stops, false) }),
		count32("distance_cm"),
		col("walk_s", T::u32, { max(86400) }),
	} });
	out.push_back(TableSpec{ TableId::facilities, 1, "estimated", {
		col("osm_type", T::u8, { code(CodeSet::osm_type, false) }),
		osm_id("osm_id"),
		col("kind", T::u8, { code(CodeSet::facility_kind, false) }),
		col("x_cm", T::i32), col("y_cm", T::i32),
		col("zone", T::u32, { ref(TableId::zones, true) }),
		col("capacity", T::u32, { all(kU32Unknown) }),  // источника вместимости нет: всегда unknown, не 0
		offsets("building_offset", size_buildings_pool),
		pool("buildings", T::u32, "building_offset", { ref(TableId::buildings, false) }),
	} });
	out.push_back(TableSpec{ TableId::gateways, 1, "estimated", {
		col("kind", T::u8, { code(CodeSet::gateway_kind, false) }),
		col("x_cm", T::i32), col("y_cm", T::i32),
		col("zone", T::u32, { ref(TableId::zones, true) }),
		col("road_class", T::u8, { code(CodeSet::road_class, true) }),
		col("mode", T::u8, { code(CodeSet::mode, true) }),
		col("arc", T::u32, { ref(TableId::road_arcs, true) }),
		col("inside_node", T::u32, { ref(TableId::road_nodes, true) }),
		col("stop", T::u32, { ref(TableId::stops, true) }),
		col("route_osm_id", T::u64, { max_or_none(kU64Unknown - 1) }),
	} });
	out.push_back(TableSpec{ TableId::road_classes, 1, "game_setting", {
		col("car_speed_mm_s", T::u32, { max(100000) }),
	} });
	out.push_back(TableSpec{ TableId::transit_modes, 2, "game_setting", {
		col("speed_mm_s", T::u32, { min(1), max(100000) }),
		col("capacity", T::u32, { min(1), max(100000) }),
		col("wagons", T::u16, { min(1), max(16) }),
		col("min_headway_s", T::u32, { min(1), max(86400) }),
	} });
	out.push_back(TableSpec{ TableId::transit_periods, 1, "game_setting", {
		col("start_s", T::u32, { max(86399) }),
		col("end_s", T::u32, { max(86399) }),
	} });
	out.push_back(TableSpec{ TableId::transit_headways, 1, "game_setting", {
		col("mode", T::u8, { code(CodeSet::mode, false) }),
		col("period", T::u8, { code(CodeSet::period, false) }),
		col("headway_s", T::u32, { min(1), max_or_none(86400) }),
	} });
	out.push_back(TableSpec{ TableId::mobility, 2, "game_setting", {
		col("walk_speed_mm_s", T::u32, { min(1), max(10000) }),
		col("transfer_overhead_s", T::u32, { max(3600) }),
		col("car_park_s", T::u32, { max(3600) }),
		col("access_radius_cm", T::u32, { min(1), max(1000000) }),
		col("access_stops_max", T::u16, { min(1), max(64) }),
		col("transfer_radius_cm", T::u32, { max(1000000) }),
		col("board_penalty_s", T::u32, { max(3600) }),
		col("max_wait_s", T::u32, { min(1), max(86400) }),
		col("walk_only_max_s", T::u32, { max(86400) }),
		col("origin_cell_cm", T::u32, { min(1), max(100000) }),
		col("crowding_penalty", T::u16, { max(60000) }),
		col("crowding_iterations", T::u16, { max(100) }),
		col("building_node_max_cm", T::u32, { max(1000000) }),
		col("demand_load_bp", T::u16, { max(kShareOne) }),
	} });
	out.push_back(TableSpec{ TableId::mobility_estimates, 1, "estimated", {
		col("walk_detour", T::u16, { min(kShareOne), max(60000) }),
	} });
	out.push_back(TableSpec{ TableId::census_age, 1, "observed", {
		census_region(),
		col("age_low", T::u8, { max(120) }), col("age_high", T::u8, { max(120) }),
		count32("persons"),
	} });
	out.push_back(TableSpec{ TableId::census_household_size, 1, "observed", {
		census_region(),
		col("size", T::u8, { min(1), max(6) }),
		count32("households"), count32("persons"),
	} });
	out.push_back(TableSpec{ TableId::census_household_children, 1, "observed", {
		census_region(),
		col("size", T::u8, { min(2), max(6) }),
		count32("households"), count32("with_1_child"), count32("with_2_children"),
		count32("with_3_plus_children"), count32("children_in_3_plus"),
	} });
	out.push_back(TableSpec{ TableId::census_labour, 1, "observed", {
		census_region(),
		col("age_low", T::u8, { max(120) }), col("age_high", T::u8, { max(120) }),
		count32("total"), count32("status_indicated"), count32("employed"),
	} });
	out.push_back(TableSpec{ TableId::census_study, 1, "observed", {
		census_region(),
		col("age_low", T::u8, { max(120) }), col("age_high", T::u8, { max(120) }),
		count32("total"), count32("not_indicated"), count32("preschool"), count32("general"), count32("professional"),
	} });
	return out;
}

} // namespace

const char *code_set_name(CodeSet set) {
	static const char *const kNames[] = {
		"osm_type", "territory", "building_function", "function_source", "share_function", "road_class",
		"mode", "stop_kind", "facility_kind", "gateway_kind", "census_region", "period",
	};
	const size_t i = static_cast<size_t>(set);
	return i < static_cast<size_t>(CodeSet::count) ? kNames[i] : "?";
}

const std::vector<std::string> &fixed_codes(CodeSet set) {
	static const std::vector<std::string> kCodes[] = {
		{ "n", "w", "r" },
		{ "moscow", "buffer" },
		{ "unknown", "residential", "work", "retail", "education", "medical", "transport", "control", "other" },
		{ "tag", "tag+poi", "poi", "site", "unknown" },
		{ "residential", "work", "retail", "education", "medical", "transport", "control", "other" },
		{ "service", "pedestrian", "residential", "tertiary", "secondary", "primary", "trunk", "motorway" },
		{ "bus", "share_taxi", "trolleybus", "tram", "light_rail", "monorail", "train", "subway" },
		{ "platform", "stop_position", "station" },
		{ "hospital", "clinic", "doctors", "university", "college", "school", "kindergarten", "police" },
		{ "road", "transit" },
		{ "moscow", "moscow_oblast" },
		{},
	};
	static const std::vector<std::string> kEmpty;
	const size_t i = static_cast<size_t>(set);
	return i < static_cast<size_t>(CodeSet::count) ? kCodes[i] : kEmpty;
}

const std::vector<TableSpec> &tables() {
	static const std::vector<TableSpec> kTables = make_tables();
	return kTables;
}

size_t item_size(ColumnType type) {
	switch (type) {
		case ColumnType::u8:
		case ColumnType::i8:
			return 1;
		case ColumnType::u16:
		case ColumnType::i16:
			return 2;
		case ColumnType::u32:
		case ColumnType::i32:
		case ColumnType::f32:
			return 4;
		case ColumnType::u64:
		case ColumnType::i64:
		case ColumnType::f64:
			return 8;
	}
	return 0;
}

const char *type_name(ColumnType type) {
	static const char *const kNames[] = { "?", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64" };
	const size_t i = static_cast<size_t>(type);
	return i <= 10 ? kNames[i] : "?";
}

} // namespace schema
} // namespace mesim
