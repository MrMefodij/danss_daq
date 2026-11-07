#ifndef DANSS_BOARD_CLIENT_H
#define DANSS_BOARD_CLIENT_H

#include "common/types.h"
#include "modbus/modbus_rtu.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace danss {

/// @brief Операции с одной процессорной платой по карте регистров v2 (doc/MODBUS_MAP_v2.md).
///
/// Тонкий слой над ModbusRTU: знает адреса регистров, раскладку блоков и процедуры
/// (команды CSR, scratchpad, смена адреса). Ничего не кэширует и не хранит состояние платы —
/// это дело BoardRegistry/поллера. Все методы возвращают false при ошибке и кладут текст
/// в last_error().
class BoardClient {
public:
    BoardClient(ModbusRTU& bus, uint8_t address) : bus_(bus), address_(address) {}

    uint8_t address() const { return address_; }
    void set_address(uint8_t a) { address_ = a; }
    const std::string& last_error() const { return error_; }

    // ---- чтение --------------------------------------------------------------

    /// Input 0–27 одной транзакцией: счётчик, HV/trip, токи, температуры, CPU
    bool read_status(BoardData& out);
    /// Holding 0–100 одной транзакцией: DAC, HV enable, gain/trip enable, CSR, задержка, адрес
    bool read_settings(BoardData& out);
    /// Input 150–245: применённые значения DAC
    bool read_dac_readback(BoardData& out);
    /// Input 100–149 + 246: ID термометров, CPU ID, смещение датчика CPU
    bool read_ids(BoardData& out);
    /// Input 250–761 (5 транзакций)
    bool read_scratchpad(ScratchpadImage& out);
    /// Только счётчик обновлений (input 0)
    bool read_update_counter(uint16_t& out);

    // ---- запись --------------------------------------------------------------

    bool write_dac(size_t channel, uint16_t value);
    bool write_all_dac(const std::array<uint16_t, constants::NUM_DAC_CHANNELS>& values);
    bool write_hv_enable(uint16_t mask);       ///< holding 96, бит на канал
    bool write_gain_trip_enable(uint16_t v);   ///< holding 97
    bool write_cycle_delay(uint16_t v);        ///< holding 99

    /// Дождаться следующего цикла платы (счётчик input 0 изменился). Нужен, например,
    /// перед сверкой DAC: плата применяет уставки в своём цикле, не мгновенно.
    bool wait_next_cycle(double timeout_s, double* waited_s = nullptr);

    /// Записать уставки DAC, дождаться цикла и сравнить с обратным чтением.
    /// @param mismatches каналы, где применённое значение отличается от записанного
    bool write_dac_verified(const std::array<uint16_t, constants::NUM_DAC_CHANNELS>& values,
                            std::vector<size_t>& mismatches, double timeout_s = 5.0);

    // ---- команды CSR (holding 98) и scratchpad ---------------------------------

    /// Записать команду в CSR и дождаться статуса. Пока плата пишет во flash, она может
    /// не отвечать — запрос статуса повторяется до timeout_s.
    /// @param status_out прочитанный статус (CSR_STATUS_OK / _UNKNOWN_COMMAND / _ERROR)
    bool csr_command(uint16_t command, uint16_t& status_out, double timeout_s = 5.0);

    /// Стереть (103) и сохранить (105) текущие значения в scratchpad, затем прочитать
    /// scratchpad и проверить контрольную сумму и совпадение с holding-регистрами.
    bool save_to_scratchpad(std::string& report);
    /// Загрузить значения из scratchpad в рабочие регистры (101)
    bool load_from_scratchpad();
    /// Только стереть scratchpad (103) — первый шаг смены адреса
    bool erase_scratchpad();

    /// Назначить адрес плате, которая сейчас на заводском 247: записать holding 100,
    /// сохранить в scratchpad. Объект после успеха продолжает работать по новому адресу,
    /// если плата на нём отвечает; иначе нужна перезагрузка платы (report скажет).
    bool assign_address(uint8_t new_address, std::string& report);

private:
    bool read_input(uint16_t start, uint16_t count, std::vector<uint16_t>& v);
    bool read_holding(uint16_t start, uint16_t count, std::vector<uint16_t>& v);
    bool fail(const std::string& what);

    ModbusRTU& bus_;
    uint8_t address_;
    std::string error_;
};

/// 4 регистра ID термометра -> 16 hex-цифр (старшее слово первым)
std::string format_thermometer_id(const uint16_t* regs);
/// 2 регистра CPU ID -> 8 hex-цифр
std::string format_cpu_id(const uint16_t* regs);

} // namespace danss

#endif // DANSS_BOARD_CLIENT_H
