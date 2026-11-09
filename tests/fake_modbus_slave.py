"""Имитатор процессорной платы по карте регистров v2 (doc/MODBUS_MAP_v2.md) на псевдотерминале.

Нужен, чтобы проверять modbus_scanner и modbus_tool без железа (Linux, macOS).

  python3 tests/fake_modbus_slave.py [--address 1[,2,...]] [--state board.json] [--period 0.5]
                                     [--analog 7] [--control control.txt]

Печатает имя устройства (например /dev/pts/3 или /dev/ttys005) — его и передавайте утилитам.

Что умеет (так, как это описано в письме Игоря; детали, которых в письме нет, — допущения):
  - FC 03/04/06/16; чужие адреса — молчание; регистр вне карты — исключение 02;
  - input 0 растёт раз в --period секунд; обратное чтение DAC (150–245) обновляется
    в цикле из holding 0–95; биты HV enable в input 1 = holding 96;
  - CSR (holding 98): 101 загрузить, 103 стереть, 105 сохранить (без стирания перед
    сохранением — статус 20), неизвестная команда — статус 10; команда выполняется
    в ближайшем цикле, после 103/105 плата 0.3 с не отвечает (запись во flash);
  - scratchpad 512 слов, контрольная сумма 0xAA55; --state хранит его между запусками:
    «перезагрузка платы» = перезапуск имитатора с тем же --state;
  - адрес (holding 100) записывается только на заводском 247 и начинает действовать сразу
    после сохранения (подтверждено 07.11.2026); со стёртым scratchpad плата стартует на 247;
  - scratchpad загружается в рабочие регистры при старте (как у реальной платы);
  - несколько плат на одной «шине»: --address 1,2,4 (у каждой свой файл <--state>_<адрес>);
  - --analog — маска подключённых аналоговых плат (бит на плату, по умолчанию 7 = все три):
    у неподключённой обратное чтение её 32 DAC = 0 (как у платы 17 без периферии);
  - --counters — маска подключённых счётчиков (бит на канал 0–11, по умолчанию 0xFFF):
    у неподключённого ID термометра 0 и температура 0x8001 (ошибка доступа, письмо Игоря 09.11);
  - токи raw 345 + канал; температуры 400 + 16·канал (25 °C + канал), канал 11 — 0xFFF0 (−1 °C);
  - у новой платы (без --state) уставки DAC 100 + номер канала;
  - --control файл — сценарии сбоев для тестов: дописывайте строки «<адрес> <команда> [арг]»:
      freeze / unfreeze   — цикл платы стоит (счётчик не растёт)
      silent / talk       — плата перестаёт / снова начинает отвечать
      reboot              — перезагрузка (счётчик с нуля, уставки из scratchpad)
      trip <маска>        — биты trip по аналоговым платам
      analog <маска>      — какие аналоговые платы подключены
      counters <маска>    — какие счётчики подключены
"""
import argparse
import json
import os
import pty
import select
import sys
import threading
import time
import tty

FACTORY = 247
SP_WORDS = 512


def crc(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return bytes([c & 0xFF, c >> 8])


class Board:
    def __init__(self, address, state_file, period, analog_mask=7, counter_mask=0xFFF):
        self.lock = threading.Lock()
        self.frozen = False
        self.silent = False
        self.trip_mask = 0
        self.analog_mask = analog_mask
        self.counter_mask = counter_mask
        self.state_file = state_file
        self.period = period
        self.counter = 0
        self.holding = [0] * 101
        self.holding[99] = int(period * 1000)
        self.holding[0:96] = [100 + i for i in range(96)]
        self.readback = [0] * 96
        self.sp = [0xFFFF] * SP_WORDS
        self.erased = False
        self.pending_cmd = None
        self.busy_until = 0.0
        self.address = address
        if state_file and os.path.exists(state_file):
            self.sp = json.load(open(state_file))["scratchpad"]
        elif address != FACTORY:
            self._save_sp(address)  # новая «прошитая» плата с рабочим адресом
        if self.sp_ok():
            self.address = self.sp[100]
            self.holding[0:96] = self.sp[0:96]
            self.holding[96] = self.sp[96]
            self.holding[99] = self.sp[99] or self.holding[99]
        else:
            self.address = FACTORY
        self.holding[100] = self.address
        self.readback = self._readback()

    def analog(self, board):
        return (self.analog_mask >> board) & 1 == 1

    def has_counter(self, ch):
        return (self.counter_mask >> ch) & 1 == 1

    def reboot(self):
        with self.lock:
            self.counter = 0
            self.pending_cmd = None
            self.erased = False
            if self.sp_ok():
                self.holding[0:96] = self.sp[0:96]
                self.holding[96] = self.sp[96]
                self.holding[99] = self.sp[99]
            self.readback = self._readback()

    def _readback(self):
        return [self.holding[i] if self.analog(i // 32) else 0 for i in range(96)]

    def control(self, cmd, arg):
        with self.lock:
            if cmd == "freeze": self.frozen = True
            elif cmd == "unfreeze": self.frozen = False
            elif cmd == "silent": self.silent = True
            elif cmd == "talk": self.silent = False
            elif cmd == "trip": self.trip_mask = int(arg, 0)
            elif cmd == "analog": self.analog_mask = int(arg, 0)
            elif cmd == "counters": self.counter_mask = int(arg, 0)
        if cmd == "reboot":
            self.reboot()

    def sp_ok(self):
        return sum(self.sp) & 0xFFFF == 0xAA55

    def _save_sp(self, address):
        sp = [0] * SP_WORDS
        sp[0:96] = self.holding[0:96]
        sp[96] = self.holding[96]
        sp[99] = self.holding[99]
        sp[100] = address
        sp[511] = (0xAA55 - sum(sp[:511])) & 0xFFFF
        self.sp = sp
        if self.state_file:
            json.dump({"scratchpad": sp}, open(self.state_file, "w"))

    def cycle(self):
        with self.lock:
            if self.frozen:
                return
            cmd, self.pending_cmd = self.pending_cmd, None
            if cmd is not None:
                status = 0
                if cmd == 0:
                    pass
                elif cmd == 101:
                    if self.sp_ok():
                        self.holding[0:96] = self.sp[0:96]
                        self.holding[96] = self.sp[96]
                        self.holding[99] = self.sp[99]
                    else:
                        status = 20
                elif cmd == 103:
                    self.sp = [0xFFFF] * SP_WORDS
                    self.erased = True
                    self.busy_until = time.time() + 0.3
                    if self.state_file:
                        json.dump({"scratchpad": self.sp}, open(self.state_file, "w"))
                elif cmd == 105:
                    if not self.erased:
                        status = 20
                    else:
                        self._save_sp(self.holding[100])
                        self.erased = False
                        self.address = self.holding[100]  # новый адрес действует сразу
                        self.busy_until = time.time() + 0.3
                else:
                    status = 10
                self.holding[98] = status
            self.readback = self._readback()
            self.counter = (self.counter + 1) & 0xFFFF

    def input_reg(self, r):
        if r == 0:
            return self.counter
        if r == 1:
            return (self.holding[96] & 0x0FFF) | ((self.trip_mask & self.analog_mask & 0x7) << 12)
        if r == 2:
            return self.holding[97] & 0x0707  # допущение: раскладка как у input 2
        if 3 <= r <= 14:
            return 345 + r - 3
        if 15 <= r <= 26:
            ch = r - 15
            if not self.has_counter(ch):
                return 0x8001
            return 0xFFF0 if ch == 11 else 400 + 16 * ch
        if r == 27:
            return 3000
        if 28 <= r <= 99 or 247 <= r <= 249:
            return 0
        if 100 <= r <= 147:
            t, w = divmod(r - 100, 4)
            return [0x28FF, 0x641E, 0x8316, 0x0550 + t][w] if self.has_counter(t) else 0
        if r == 148:
            return 0x1234
        if r == 149:
            return 0x5678
        if 150 <= r <= 245:
            return self.readback[r - 150]
        if r == 246:
            return 7
        if 250 <= r <= 761:
            return self.sp[r - 250]
        if r == 1000:
            return 0xDEB6
        return None

    def handle(self, frame):
        """frame без CRC -> ответ без CRC или None (молчание)"""
        addr, fc = frame[0], frame[1]
        if addr != self.address or self.silent or time.time() < self.busy_until:
            return None
        start = (frame[2] << 8) | frame[3]
        with self.lock:
            if fc in (3, 4):
                count = (frame[4] << 8) | frame[5]
                if not 1 <= count <= 125:
                    return bytes([addr, fc | 0x80, 3])
                vals = []
                for r in range(start, start + count):
                    v = self.input_reg(r) if fc == 4 else (self.holding[r] if r <= 100 else (0 if r == 1000 else None))
                    if v is None:
                        return bytes([addr, fc | 0x80, 2])
                    vals.append(v)
                return bytes([addr, fc, 2 * count]) + b"".join(v.to_bytes(2, "big") for v in vals)
            if fc == 6:
                value = (frame[4] << 8) | frame[5]
                err = self.write(start, [value])
                return bytes([addr, fc | 0x80, err]) if err else bytes(frame[:6])
            if fc == 16:
                count = (frame[4] << 8) | frame[5]
                vals = [(frame[7 + 2 * i] << 8) | frame[8 + 2 * i] for i in range(count)]
                err = self.write(start, vals)
                return bytes([addr, fc | 0x80, err]) if err else bytes(frame[:6])
            return bytes([addr, fc | 0x80, 1])

    def write(self, start, vals):
        for i, v in enumerate(vals):
            r = start + i
            if r == 1000:
                continue
            if r > 100:
                return 2
            if r == 100 and self.address != FACTORY:
                return 3  # адрес меняется только на заводском 247
        for i, v in enumerate(vals):
            r = start + i
            if r == 98:
                self.pending_cmd = v
            elif r <= 100:
                self.holding[r] = v
        return 0


def frame_len(buf):
    """Ожидаемая длина запроса по началу буфера (None — ещё не ясно)"""
    if len(buf) < 2:
        return None
    if buf[1] in (3, 4, 6):
        return 8
    if buf[1] == 16:
        return 9 + buf[6] if len(buf) >= 7 else None
    return 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", default="1", help="адрес или список через запятую")
    ap.add_argument("--analog", type=lambda x: int(x, 0), default=7)
    ap.add_argument("--counters", type=lambda x: int(x, 0), default=0xFFF)
    ap.add_argument("--control", default="")
    ap.add_argument("--state", default="")
    ap.add_argument("--period", type=float, default=0.5)
    a = ap.parse_args()

    addrs = [int(x) for x in a.address.split(",")]
    boards = []
    for ad in addrs:
        state = a.state if len(addrs) == 1 else (f"{a.state}_{ad}" if a.state else "")
        boards.append(Board(ad, state, a.period, a.analog, a.counters))
    master, slave = pty.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    for b in boards:
        print(f"board address {b.address}, scratchpad {'OK' if b.sp_ok() else 'erased'}", file=sys.stderr, flush=True)

    def ticker():
        done = 0
        while True:
            time.sleep(a.period)
            for b in boards:
                b.cycle()
            if a.control and os.path.exists(a.control):
                lines = open(a.control).read().splitlines()
                for line in lines[done:]:
                    parts = line.split()
                    if len(parts) >= 2:
                        for b in boards:
                            if b.address == int(parts[0]):
                                b.control(parts[1], parts[2] if len(parts) > 2 else "0")
                done = len(lines)

    threading.Thread(target=ticker, daemon=True).start()

    buf = b""
    while True:
        r, _, _ = select.select([master], [], [], 0.05)
        if not r:
            buf = b""  # пауза между кадрами — начало нового кадра
            continue
        buf += os.read(master, 512)
        while True:
            n = frame_len(buf)
            if n is None or len(buf) < n:
                break
            frame, buf = buf[:n], buf[n:]
            if crc(frame[:-2]) != frame[-2:]:
                buf = b""
                break
            reply = None
            for b in boards:
                reply = b.handle(frame[:-2])
                if reply is not None:
                    break
            if reply is not None:
                os.write(master, reply + crc(reply))


if __name__ == "__main__":
    main()
