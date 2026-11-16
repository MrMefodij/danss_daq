#!/usr/bin/env python3
"""Сверка плат SiPM на стенде с базой тестирования (DANSS-II.db от Игоря, SQLite).

База:
  SiPM_boards(ID, time, temperature, Vbreak1..8, Vset1..8) — плата 8SiPM: ID её термометра
      (тот же, что читает процессорная плата в input 100–147), напряжения пробоя и рабочие
      напряжения 8 SiPM, температура при тесте;
  Strips(ID, time, BoardID_left, BoardID_right, ..., light_yield, LY1..16) — счётчик (полоса):
      ID плат 8SiPM на левом и правом концах.

Использование (ID берутся из вывода `modbus_tool <порт> <адрес> info`):

  ./bin/modbus_tool /dev/ttyACM3 1 info > info_1.txt
  ./bin/modbus_tool /dev/ttyACM3 2 info > info_2.txt
  python3 tools/sipm_db.py DANSS-II.db info_1.txt info_2.txt            # таблица и проверка пар
  python3 tools/sipm_db.py DANSS-II.db info_1.txt info_2.txt --json ref.json   # эталон ID (eeprom_reference)
  python3 tools/sipm_db.py DANSS-II.db --id 28FF1A13BC2202D2            # одна плата 8SiPM

Первый файл — «левая» процессорная плата, второй — «правая» (по письму Игоря: счётчик на
канале k одной платы — на канале 11−k другой). Скрипт это проверяет по таблице Strips.
"""
import argparse
import json
import os
import re
import sqlite3
import sys

ID_RE = re.compile(r"^\s*(\d+)\s+\d+\s+\d+\s+[\d.]+\s+\d+\s+\S+\s+\S+\s+([0-9A-F]{16})\s*$")


def read_info(path):
    """Вывод modbus_tool info -> (номер платы, CPU ID, [ID термометров по каналам 0..11])"""
    board, cpu, ids = None, None, {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^Board (\d+)", line)
        if m:
            board = int(m.group(1))
        m = re.search(r"CPU ID\s+([0-9A-F]+)", line)
        if m:
            cpu = m.group(1)
        m = ID_RE.match(line)
        if m:
            ids[int(m.group(1))] = m.group(2)
    if len(ids) != 12:
        sys.exit(f"{path}: найдено {len(ids)} каналов вместо 12 — это вывод `modbus_tool ... info`?")
    return board, cpu, [ids[ch] for ch in range(12)]


def sipm(db, bid):
    r = db.execute("SELECT time, temperature, " + ", ".join(f"Vbreak{i}" for i in range(1, 9)) + ", "
                   + ", ".join(f"Vset{i}" for i in range(1, 9)) + " FROM SiPM_boards WHERE ID=?", (bid,)).fetchone()
    if not r:
        return None
    return {"time": r[0], "temperature": r[1], "vbreak": list(r[2:10]), "vset": list(r[10:18])}


def strips(db, bid):
    """Счётчики, где стоит эта плата 8SiPM (новые первыми): [(strip, сторона L/R, ID платы на другом конце, время)]"""
    out = []
    for sid, left, right, t in db.execute(
            "SELECT ID, BoardID_left, BoardID_right, time FROM Strips WHERE BoardID_left=? OR BoardID_right=? "
            "ORDER BY time DESC", (bid, bid)):
        out.append((sid, "L" if left == bid else "R", right if left == bid else left, t))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db")
    ap.add_argument("info", nargs="*", help="вывод modbus_tool info: левая плата, затем правая")
    ap.add_argument("--id", action="append", default=[], help="ID платы 8SiPM (16 hex)")
    ap.add_argument("--json", help="записать эталон ID термометров и CPU в формате eeprom_reference.json")
    ap.add_argument("--names", default="STAND-01,STAND-02", help="имена плат для --json (по порядку файлов info)")
    a = ap.parse_args()

    db = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)

    for bid in a.id:
        s = sipm(db, bid.upper())
        print(bid, "нет в SiPM_boards" if not s else
              f"тест {s['time']} при {s['temperature']:.1f} °C; Vset {', '.join(f'{v:.2f}' for v in s['vset'])}")
        for st in strips(db, bid.upper()):
            print(f"  счётчик {st[0]} ({st[1]}), другой конец {st[2]}, {st[3]}")

    boards = [read_info(p) for p in a.info]
    names = a.names.split(",")
    for n, (addr, cpu, ids) in enumerate(boards):
        name = names[n] if n < len(names) else f"BOARD-{addr}"
        print(f"\n== {name} (адрес {addr}, CPU {cpu})")
        print(f"{'ch':>3} {'ID термометра':16} {'тест':10} {'T,°C':>5} {'Vset мин…макс, В':>17}  счётчик")
        for ch, bid in enumerate(ids):
            s = sipm(db, bid)
            st = strips(db, bid)
            vs = f"{min(s['vset']):6.2f}…{max(s['vset']):6.2f}" if s else "нет в базе"
            sts = ", ".join(f"{x[0]}{x[1]}" for x in st) or "—"
            print(f"{ch:3d} {bid} {s['time'][:10] if s else '':10} {s['temperature'] if s else 0:5.1f} {vs:>17}  {sts}")

    if len(boards) == 2:
        (_, _, left), (_, _, right) = boards
        print("\n== Пары «канал k левой — канал 11−k правой» по таблице Strips")
        ok = bad = unknown = 0
        for k in range(12):
            l, r = left[k], right[11 - k]
            sl = {x[0]: x for x in strips(db, l)}
            common = [sid for sid, x in sl.items() if x[2] == r]
            if common:
                ok += 1
                print(f"  ch{k:2d} ↔ ch{11 - k:2d}: счётчик {common[0]} — совпадает")
            elif sl or strips(db, r):
                bad += 1
                print(f"  ch{k:2d} ↔ ch{11 - k:2d}: НЕ совпадает с базой: левая плата — "
                      f"{', '.join(str(s) for s in sl) or '—'}, правая — {', '.join(str(x[0]) for x in strips(db, r)) or '—'}")
            else:
                unknown += 1
                print(f"  ch{k:2d} ↔ ch{11 - k:2d}: обеих плат нет в Strips")
        print(f"  итого: совпадает {ok}, не совпадает {bad}, нет в базе {unknown}")

    if a.json:
        out = {"description": f"Эталон ID термометров и CPU (ID сверены с {os.path.basename(a.db)})", "boards": []}
        for n, (addr, cpu, ids) in enumerate(boards):
            out["boards"].append({"name": names[n] if n < len(names) else f"BOARD-{addr}", "modbus_address": addr,
                                  "branch_id": 0, "thermometer_ids": ids, "cpu_id": cpu})
        json.dump(out, open(a.json, "w"), indent=2, ensure_ascii=False)
        print(f"\nэталон записан: {a.json}")


if __name__ == "__main__":
    main()
