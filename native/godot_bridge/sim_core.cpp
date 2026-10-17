#include "sim_core.h"

#include <chrono>

#include <godot_cpp/core/class_db.hpp>

#include "mesim/version.h"

namespace godot {

void SimCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_core_version"), &SimCore::get_core_version);
	ClassDB::bind_method(D_METHOD("load_city_package", "package_dir"), &SimCore::load_city_package);
	ClassDB::bind_method(D_METHOD("unload_city"), &SimCore::unload_city);
	ClassDB::bind_method(D_METHOD("is_city_loaded"), &SimCore::is_city_loaded);
	ClassDB::bind_method(D_METHOD("get_city_info"), &SimCore::get_city_info);
}

String SimCore::get_core_version() const {
	return String(mesim::core_version_string());
}

Dictionary SimCore::load_city_package(const String &package_dir) {
	const auto start = std::chrono::steady_clock::now();
	const mesim::Status status = mesim::load_city_package(package_dir.utf8().get_data(), city_);
	const int64_t usec = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	Dictionary result;
	result["ok"] = status.ok();
	result["code"] = String(mesim::status_code_name(status.code));
	result["message"] = String::utf8(status.message.c_str());
	if (status.ok()) {
		result["package_id"] = String::utf8(city_.package_id().c_str());
		result["package_version"] = String::utf8(city_.package_version().c_str());
		result["tables"] = static_cast<int64_t>(mesim::kTableCount);
		result["memory_bytes"] = static_cast<int64_t>(city_.memory_bytes());
		result["load_usec"] = usec;
	}
	return result;
}

void SimCore::unload_city() {
	city_.clear();
}

bool SimCore::is_city_loaded() const {
	return city_.loaded();
}

Dictionary SimCore::get_city_info() const {
	Dictionary info;
	if (!city_.loaded()) {
		return info;
	}
	info["package_id"] = String::utf8(city_.package_id().c_str());
	info["package_version"] = String::utf8(city_.package_version().c_str());
	info["metric_crs"] = String::utf8(city_.metric_crs().c_str());
	info["plane_x0"] = city_.plane_x0();
	info["plane_y0"] = city_.plane_y0();
	info["zone_cell_m"] = static_cast<int64_t>(city_.zone_cell_m());
	info["memory_bytes"] = static_cast<int64_t>(city_.memory_bytes());
	Dictionary rows;
	for (size_t i = 0; i < mesim::kTableCount; ++i) {
		const mesim::Table &table = city_.table(static_cast<mesim::TableId>(i));
		rows[String(table.name().c_str())] = static_cast<int64_t>(table.rows());
	}
	info["rows"] = rows;
	return info;
}

} // namespace godot
