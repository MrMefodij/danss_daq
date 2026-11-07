#include "common/logger.h"

// PowerSupply MIDAS equipment реализован отдельной программой danss_psu_fe:
//   src/frontend/psu_frontend.cxx (TMFE) поверх src/power/psu_worker.cpp.
// Решение (ноябрь 2026): отдельный процесс, чтобы сбой опроса плат не мешал
// управлению HV и наоборот. Этот файл — заглушка, его можно удалить вместе
// со строкой в FRONTEND_SOURCES в CMakeLists.txt.

// Placeholder to satisfy linker
namespace danss {
namespace frontend {

void power_equipment_placeholder() {
    LOG_INFO("PowerSupply MIDAS equipment implementation pending");
}

} // namespace frontend
} // namespace danss
