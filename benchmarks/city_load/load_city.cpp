// Замер загрузки расчётного пакета ядром (#37): время каждой загрузки, память таблиц и пик
// резидентной памяти процесса. Повторные загрузки в тот же CityData проверяют, что прежние
// таблицы освобождаются (пик не растёт с числом повторов).
// Запуск: mesim_load_city <каталог пакета> [повторов]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/resource.h>

#include "mesim/city_data.h"
#include "mesim/version.h"

namespace {

double peak_rss_mb() {
	struct rusage usage {};
	getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
	return usage.ru_maxrss / 1e6;  // байты
#else
	return usage.ru_maxrss / 1e3;  // килобайты
#endif
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2 || argc > 3) {
		std::fprintf(stderr, "Использование: mesim_load_city <каталог пакета> [повторов]\n");
		return 2;
	}
	const int repeats = argc == 3 ? std::atoi(argv[2]) : 5;
	if (repeats < 1) {
		std::fprintf(stderr, "Число повторов — положительное целое\n");
		return 2;
	}
	std::printf("ядро %s, пакет %s\n", mesim::core_version_string(), argv[1]);
	mesim::CityData city;
	for (int i = 0; i < repeats; ++i) {
		const auto start = std::chrono::steady_clock::now();
		const mesim::Status status = mesim::load_city_package(argv[1], city);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (!status.ok()) {
			std::fprintf(stderr, "ошибка %s: %s\n", mesim::status_code_name(status.code), status.message.c_str());
			return 1;
		}
		std::printf("загрузка %d: %.1f мс, таблицы %.1f МБ, пик RSS %.1f МБ\n", i + 1, ms, city.memory_bytes() / 1e6, peak_rss_mb());
	}
	std::printf("%s %s: зданий %llu, узлов %llu, дуг %llu, остановок %llu\n", city.package_id().c_str(), city.package_version().c_str(),
			static_cast<unsigned long long>(city.table(mesim::TableId::buildings).rows()),
			static_cast<unsigned long long>(city.table(mesim::TableId::road_nodes).rows()),
			static_cast<unsigned long long>(city.table(mesim::TableId::road_arcs).rows()),
			static_cast<unsigned long long>(city.table(mesim::TableId::stops).rows()));
	return 0;
}
