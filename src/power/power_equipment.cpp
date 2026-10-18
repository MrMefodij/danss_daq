#include "common/logger.h"

// TODO (Phase 3): Implement MIDAS "PowerSupply" equipment class wrapping
// PowerSupplyController — mirrors src/frontend/equipment.cpp's SlowControl
// equipment, but on its own poll cycle (config_.poll_interval_sec) since a
// SCPI/TCP round-trip should not block/extend the Modbus poll cycle.
//
// Responsibilities once implemented:
// - init(): ConfigLoader::load_power_config(), PowerSupplyController::connect(),
//   apply_protection_limits()
// - read_event(): PowerSupplyController::poll() -> PSUD bank (see
//   constants::BANK_NAME_PSU_DATA) into ODB_PATH_PSU_VARIABLES
// - set_interlock_ready() driven from SlowControl's board-online tracking
//   (e.g. all boards on a branch reporting online) — see
//   PowerSupplyController::set_interlock_ready()
// - begin_of_run()/RunStateMachine hooks: enable_channel() for configured
//   channels on STOPPED -> PHYSICS/CALIBRATION; emergency_shutdown() on
//   ANY -> STOPPED and on fatal alarms

// Placeholder to satisfy linker
namespace danss {
namespace frontend {

void power_equipment_placeholder() {
    LOG_INFO("PowerSupply MIDAS equipment implementation pending");
}

} // namespace frontend
} // namespace danss
