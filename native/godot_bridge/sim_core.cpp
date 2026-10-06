#include "sim_core.h"

#include <godot_cpp/core/class_db.hpp>

#include "mesim/version.h"

namespace godot {

void SimCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_core_version"), &SimCore::get_core_version);
}

String SimCore::get_core_version() const {
	return String(mesim::core_version_string());
}

} // namespace godot
