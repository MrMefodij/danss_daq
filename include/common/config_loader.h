#ifndef DANSS_CONFIG_LOADER_H
#define DANSS_CONFIG_LOADER_H

#include "common/types.h"
#include <map>
#include <stdexcept>
#include <string>

namespace danss {

/// @brief Ошибка чтения или проверки конфигурации.
/// Текст содержит имя файла и путь до поля, например
/// "config/power_supply.json: power_supply.channels[1].ovp_limit_v: 70 > 61 (максимум OVP прибора)".
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Загрузка конфигурации из JSON-файлов (nlohmann/json).
///
/// Правила для всех трёх файлов:
///  - файл не открылся, JSON битый, нет обязательного поля, неверный тип или значение
///    вне допустимого диапазона — исключение ConfigError, частично заполненный конфиг не возвращается;
///  - необязательное поле отсутствует — берётся значение по умолчанию из constants.h;
///  - неизвестное поле — предупреждение в лог (обычно это опечатка в имени), не ошибка.
///
/// Для power_supply.json дополнительно проверяется безопасность: пределы прибора ODP6033,
/// OVP выше целевого напряжения, OCP не ниже предела тока, интерлок обязателен для SIPM_HV.
class ConfigLoader {
public:
    /// @brief board_map.json: параметры опроса, ветки RS-485 и платы на них
    /// @throws ConfigError
    static SystemConfig load_system_config(const std::string& filename);

    /// @brief eeprom_reference.json: эталонные ID термометров и CPU
    /// @return имя платы -> эталон (address, name, thermometer_ids, cpu_id)
    /// @throws ConfigError
    static std::map<std::string, BoardConfig> load_eeprom_reference(const std::string& filename);

    /// @brief power_supply.json: блок питания OWON ODP6033
    /// @throws ConfigError
    static PSUConfig load_power_config(const std::string& filename);

    /// @brief То же, но из строки JSON (для тестов и для настроек, пришедших из ODB).
    /// @param source имя источника для сообщений об ошибках
    static SystemConfig parse_system_config(const std::string& json_text, const std::string& source);
    static std::map<std::string, BoardConfig> parse_eeprom_reference(const std::string& json_text,
                                                                     const std::string& source);
    static PSUConfig parse_power_config(const std::string& json_text, const std::string& source);

    /// @brief Обратное преобразование: PSUConfig -> текст power_supply.json.
    /// parse_power_config(power_config_to_json(c)) даёт тот же конфиг.
    static std::string power_config_to_json(const PSUConfig& config);
};

} // namespace danss

#endif // DANSS_CONFIG_LOADER_H
