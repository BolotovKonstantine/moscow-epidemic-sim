# Сборка чистого ядра без Godot и godot-cpp: тесты и инструменты замеров.
# Библиотеку GDExtension собирает SConstruct; оба берут одни и те же исходники src/core/*.cpp.
# Команды — в native/AGENTS.md.
cmake_minimum_required(VERSION 3.20)
project(mesim_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

option(MESIM_SANITIZE "AddressSanitizer и UndefinedBehaviorSanitizer" OFF)

# Как у godot-cpp: ядро обязано собираться без исключений и RTTI.
set(MESIM_FLAGS -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
if(MESIM_SANITIZE)
	list(APPEND MESIM_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
	add_link_options(-fsanitize=address,undefined)
endif()

file(GLOB MESIM_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp)
# Планировщик поездок (#42) считает группы в нескольких потоках (std::thread).
find_package(Threads REQUIRED)
add_library(mesim_core STATIC ${MESIM_CORE_SOURCES})
target_include_directories(mesim_core PUBLIC include)
target_link_libraries(mesim_core PUBLIC Threads::Threads)
target_compile_options(mesim_core PRIVATE ${MESIM_FLAGS})

add_executable(mesim_core_tests tests/core_tests.cpp)
target_include_directories(mesim_core_tests PRIVATE src/core)
target_compile_options(mesim_core_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_core_tests PRIVATE mesim_core)

add_executable(mesim_population_tests tests/population_tests.cpp)
target_compile_options(mesim_population_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_population_tests PRIVATE mesim_core)

add_executable(mesim_clock_tests tests/clock_tests.cpp)
target_include_directories(mesim_clock_tests PRIVATE src/core)
target_compile_options(mesim_clock_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_clock_tests PRIVATE mesim_core)

add_executable(mesim_route_tests tests/route_tests.cpp)
target_include_directories(mesim_route_tests PRIVATE src/core)
target_compile_options(mesim_route_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_route_tests PRIVATE mesim_core)

add_executable(mesim_assignment_tests tests/assignment_tests.cpp)
target_compile_options(mesim_assignment_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_assignment_tests PRIVATE mesim_core)

add_executable(mesim_mobility_tests tests/mobility_tests.cpp)
target_include_directories(mesim_mobility_tests PRIVATE src/core)
target_compile_options(mesim_mobility_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_mobility_tests PRIVATE mesim_core)

add_executable(mesim_schedule_tests tests/schedule_tests.cpp)
target_compile_options(mesim_schedule_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_schedule_tests PRIVATE mesim_core)

add_executable(mesim_session_tests tests/session_tests.cpp)
target_compile_options(mesim_session_tests PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_session_tests PRIVATE mesim_core)

add_executable(mesim_load_city ../benchmarks/city_load/load_city.cpp)
target_compile_options(mesim_load_city PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_load_city PRIVATE mesim_core)

add_executable(mesim_population ../benchmarks/population/population.cpp)
target_compile_options(mesim_population PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_population PRIVATE mesim_core)

add_executable(mesim_event_queue ../benchmarks/event_queue/event_queue.cpp)
target_compile_options(mesim_event_queue PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_event_queue PRIVATE mesim_core)

add_executable(mesim_route_bench ../benchmarks/routes/route_bench.cpp)
target_compile_options(mesim_route_bench PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_route_bench PRIVATE mesim_core)

add_executable(mesim_assign_bench ../benchmarks/assignment/assign_bench.cpp)
target_compile_options(mesim_assign_bench PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_assign_bench PRIVATE mesim_core)

add_executable(mesim_sched_bench ../benchmarks/schedules/sched_bench.cpp)
target_compile_options(mesim_sched_bench PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_sched_bench PRIVATE mesim_core)

add_executable(mesim_session_bench ../benchmarks/session/session_bench.cpp)
target_compile_options(mesim_session_bench PRIVATE ${MESIM_FLAGS})
target_link_libraries(mesim_session_bench PRIVATE mesim_core)

enable_testing()
add_test(NAME core_tests COMMAND mesim_core_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/sim_mini)
add_test(NAME clock_tests COMMAND mesim_clock_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/scenarios)
add_test(NAME route_tests COMMAND mesim_route_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/scenarios ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/sim_mini)
add_test(NAME mobility_tests COMMAND mesim_mobility_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/scenarios)
add_test(NAME schedule_tests COMMAND mesim_schedule_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/sim_mini ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME assignment_tests COMMAND mesim_assignment_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/sim_mini)
add_test(NAME session_tests COMMAND mesim_session_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/sim_mini ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME population_tests COMMAND mesim_population_tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/sim_mini)
