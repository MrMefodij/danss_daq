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

# Find MIDAS
if(NOT DEFINED ENV{MIDASSYS})
    message(FATAL_ERROR "MIDASSYS environment variable not set. Please source MIDAS setup script.")
endif()

set(MIDAS_DIR $ENV{MIDASSYS})
message(STATUS "MIDAS directory: ${MIDAS_DIR}")

# MIDAS include and libraries
include_directories(
    ${MIDAS_DIR}/include
    ${MIDAS_DIR}/mfe
)

link_directories(
    ${MIDAS_DIR}/lib
    ${MIDAS_DIR}/linux/lib
)

# Find Boost
find_package(Boost 1.65 REQUIRED COMPONENTS system thread)
if(Boost_FOUND)
    message(STATUS "Boost version: ${Boost_VERSION}")
    include_directories(${Boost_INCLUDE_DIRS})
endif()

# Find Threads
find_package(Threads REQUIRED)

# JSON library (header-only nlohmann/json - will be downloaded if needed)
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(json)

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
)

set(SYNC_SOURCES
    src/sync/state_machine.cpp
)

set(POWER_SOURCES
    src/power/owon_psu.cpp
    src/power/power_controller.cpp
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

target_link_libraries(danss_frontend
    danss_core
    mfe
    midas
    ${Boost_LIBRARIES}
    Threads::Threads
#    rt
    util
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

# Install targets
install(TARGETS danss_frontend modbus_scanner
    RUNTIME DESTINATION bin
)

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
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C++ flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "  MIDAS: ${MIDAS_DIR}")
message(STATUS "  Boost: ${Boost_VERSION}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
