#include "mesim/version.h"

namespace mesim {

namespace {
constexpr CoreVersion kVersion{ 0, 1, 0 };
constexpr const char *kVersionString = "0.1.0";
} // namespace

CoreVersion core_version() {
	return kVersion;
}

const char *core_version_string() {
	return kVersionString;
}

} // namespace mesim
