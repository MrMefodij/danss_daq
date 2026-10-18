#ifndef DANSS_CONFIG_LOADER_H
#define DANSS_CONFIG_LOADER_H

#include "common/types.h"
#include <string>
#include <map>

namespace danss {

/// @brief Configuration loader from JSON files
class ConfigLoader {
public:
    /// @brief Load system configuration from JSON file
    /// @param filename Path to board_map.json
    /// @return System configuration
    static SystemConfig load_system_config(const std::string& filename);
    
    /// @brief Load EEPROM reference data from JSON file
    /// @param filename Path to eeprom_reference.json
    /// @return Map of board name to EEPROM reference data
    static std::map<std::string, BoardConfig> load_eeprom_reference(const std::string& filename);

    /// @brief Load power-supply (OWON ODP6033) configuration from JSON file
    /// @param filename Path to power_supply.json
    /// @return PSU configuration
    static PSUConfig load_power_config(const std::string& filename);
    
private:
    // TODO: Implement using nlohmann/json
};

} // namespace danss

#endif // DANSS_CONFIG_LOADER_H
