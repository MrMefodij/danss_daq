cmake_minimum_required(VERSION 3.16...4.4)
project(DANSS_SlowControl VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# MIDAS нужен только MIDAS-frontend'ам (danss_psu_fe, danss_sc_fe). Без него собираются
# утилиты и опрос без MIDAS: modbus_scanner, modbus_tool, danss_frontend --standalone, тесты.
#   cmake ..                    — с MIDAS, если задан $MIDASSYS, иначе без (с предупреждением)
#   cmake -DWITH_MIDAS=OFF ..   — явно без MIDAS
if(DEFINED ENV{MIDASSYS})
    set(_with_midas_default ON)
else()
    set(_with_midas_default OFF)
endif()
option(WITH_MIDAS "Build MIDAS frontends (danss_psu_fe, danss_sc_fe); needs $MIDASSYS" ${_with_midas_default})

if(WITH_MIDAS)
    if(NOT DEFINED ENV{MIDASSYS})
        message(FATAL_ERROR "WITH_MIDAS=ON, but MIDASSYS is not set. Set MIDASSYS to the MIDAS directory "
                            "or build without MIDAS: cmake -DWITH_MIDAS=OFF ..")
    endif()
    set(MIDAS_DIR $ENV{MIDASSYS})
    message(STATUS "MIDAS directory: ${MIDAS_DIR}")
    # Цели MIDAS (midas::midas) с путями к mvodb/midasio и zlib, с которыми MIDAS собран
    find_package(Midas REQUIRED PATHS ${MIDAS_DIR} NO_DEFAULT_PATH)
else()
    message(WARNING "Building WITHOUT MIDAS: danss_psu_fe and danss_sc_fe are skipped "
                    "(set MIDASSYS or -DWITH_MIDAS=ON to build them)")
endif()

# Find Boost
find_package(Boost 1.65 REQUIRED COMPONENTS system thread)
if(Boost_FOUND)
    message(STATUS "Boost version: ${Boost_VERSION}")
    include_directories(${Boost_INCLUDE_DIRS})
endif()

# Find Threads
find_package(Threads REQUIRED)

# JSON (nlohmann/json, header-only). По порядку:
#   1) -DJSON_INCLUDE_DIR=<каталог, где лежит nlohmann/json.hpp> — для машин без интернета;
#   2) установленный в системе / conda (find_package);
#   3) скачать с GitHub (FetchContent).
set(JSON_INCLUDE_DIR "" CACHE PATH "Directory containing nlohmann/json.hpp (offline builds)")
if(JSON_INCLUDE_DIR)
    if(NOT EXISTS "${JSON_INCLUDE_DIR}/nlohmann/json.hpp")
        message(FATAL_ERROR "JSON_INCLUDE_DIR=${JSON_INCLUDE_DIR}: nlohmann/json.hpp not found there")
    endif()
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE ${JSON_INCLUDE_DIR})
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    message(STATUS "nlohmann/json: ${JSON_INCLUDE_DIR}")
else()
    find_package(nlohmann_json 3.2 QUIET)
    if(nlohmann_json_FOUND)
        message(STATUS "nlohmann/json: installed ${nlohmann_json_VERSION}")
    else()
        message(STATUS "nlohmann/json: downloading v3.12.0 from GitHub")
        include(FetchContent)
        FetchContent_Declare(
            json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.12.0
        )
        FetchContent_MakeAvailable(json)
    endif()
endif()

# Project include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/common
    ${CMAKE_SOURCE_DIR}/include/modbus
    ${CMAKE_SOURCE_DIR}/include/eeprom
    ${CMAKE_SOURCE_DIR}/include/power
)

# Source files
set(MODBUS_SOURCES
    src/modbus/modbus_rtu.cpp
    src/modbus/modbus_crc.cpp
    src/modbus/board_client.cpp
)

set(RS485_SOURCES
    src/rs485/rs485_manager.cpp
    src/rs485/board_registry.cpp
)

set(EEPROM_SOURCES
    src/eeprom/eeprom_manager.cpp
    src/eeprom/eeprom_validator.cpp
)

set(SLOWCONTROL_SOURCES
    src/slowcontrol/periodic_poller.cpp
    src/slowcontrol/data_collector.cpp
    src/slowcontrol/exclusions_file.cpp
    src/slowcontrol/setpoints.cpp
)

set(SYNC_SOURCES
    src/sync/state_machine.cpp
)

set(POWER_SOURCES
    src/power/owon_psu.cpp
    src/power/power_controller.cpp
    src/power/psu_worker.cpp
)

set(UTILS_SOURCES
    src/utils/logger.cpp
    src/utils/config_loader.cpp
)

set(FRONTEND_SOURCES
    src/frontend/main.cpp
    src/frontend/equipment.cpp
    src/power/power_equipment.cpp
)

# Static library with core functionality
add_library(danss_core STATIC
    ${MODBUS_SOURCES}
    ${RS485_SOURCES}
    ${EEPROM_SOURCES}
    ${SLOWCONTROL_SOURCES}
    ${SYNC_SOURCES}
    ${POWER_SOURCES}
    ${UTILS_SOURCES}
)

target_link_libraries(danss_core
    ${Boost_LIBRARIES}
    Threads::Threads
    nlohmann_json::nlohmann_json
)

# Main frontend executable
add_executable(danss_frontend
    ${FRONTEND_SOURCES}
)

# danss_frontend MIDAS не использует (--standalone, --test-modbus); MIDAS-программы —
# danss_psu_fe и danss_sc_fe на TMFE
target_link_libraries(danss_frontend
    danss_core
    ${Boost_LIBRARIES}
    Threads::Threads
)

# Modbus scanner utility
add_executable(modbus_scanner
    src/utils/modbus_scanner.cpp
)

target_link_libraries(modbus_scanner
    danss_core
    ${Boost_LIBRARIES}
    Threads::Threads
)

# Обслуживание одной платы: чтение/запись регистров, DAC с проверкой, scratchpad, адрес
add_executable(modbus_tool
    src/utils/modbus_tool.cpp
)

target_link_libraries(modbus_tool
    danss_core
    ${Boost_LIBRARIES}
    Threads::Threads
)

if(WITH_MIDAS)
    # PowerSupply frontend (OWON ODP6033) — отдельная программа на TMFE
    add_executable(danss_psu_fe
        src/frontend/psu_frontend.cxx
    )

    target_link_libraries(danss_psu_fe
        danss_core
        midas::midas
        ${Boost_LIBRARIES}
        Threads::Threads
    )

    # SlowControl: опрос плат SiPM в MIDAS (TMFE), equipment "SlowControl"
    add_executable(danss_sc_fe
        src/frontend/sc_frontend.cxx
    )

    target_link_libraries(danss_sc_fe
        danss_core
        midas::midas
        ${Boost_LIBRARIES}
        Threads::Threads
    )
endif()

# Install targets
install(TARGETS danss_frontend modbus_scanner modbus_tool RUNTIME DESTINATION bin)
if(WITH_MIDAS)
    install(TARGETS danss_psu_fe danss_sc_fe RUNTIME DESTINATION bin)
endif()

install(DIRECTORY config/
    DESTINATION etc/danss
    FILES_MATCHING PATTERN "*.json"
)

# Testing (optional)
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "Configuration Summary:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  MIDAS frontends: ${WITH_MIDAS}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C++ flags: ${CMAKE_CXX_FLAGS}")
if(WITH_MIDAS)
    message(STATUS "  MIDAS: ${MIDAS_DIR}")
endif()
message(STATUS "  Boost: ${Boost_VERSION}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
