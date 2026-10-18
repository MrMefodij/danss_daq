#!/usr/bin/env python3
"""
Минимальный эмулятор SCPI-сервера OWON ODP6033 поверх TCP (текстовый
протокол, команды/ответы разделены '\n'), для проверки OwonPSU /
PowerSupplyController без реального блока питания.

Поддерживает ровно тот набор команд, которым пользуется наш клиент:
  *IDN?
  SYSTem:REMote / SYSTem:LOCal
  INSTrument:NSELect <1|2|3>
  VOLTage <v> / CURRent <v>
  VOLTage:LIMit <v> / CURRent:LIMit <v>
  OUTPut:STATe <0|1>
  MEASure:VOLTage:DC? / MEASure:CURRent:DC? / MEASure:POWer:DC?
  MEASure:VOLTage:ALL:DC? / MEASure:CURRent:ALL:DC?
  OUTPut:STATe:ALL?

Измеренное напряжение/ток канала = заданные значения, если выход включен,
иначе 0 — этого достаточно, чтобы проверить ramp/interlock/poll логику
контроллера, не эмулируя реальную нагрузку.

Запуск: python3 mock_owon_server.py [port]  (по умолчанию 15000)
"""
import socket
import sys
import threading

class Channel:
    def __init__(self):
        self.voltage = 0.0
        self.current = 0.0
        self.ovp = 0.0
        self.ocp = 0.0
        self.output = False

def handle_client(conn, addr, channels, verbose):
    selected = 1
    buf = b""
    with conn:
        conn_file = conn.makefile("rwb", buffering=0)
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode("ascii", errors="replace").strip().rstrip("\r")
                if not cmd:
                    continue
                if verbose:
                    print(f"[{addr}] ch{selected} <- {cmd}")
                reply = None
                upper = cmd.upper()

                if upper == "*IDN?":
                    reply = "OWON,ODP6033,SN00000001,FV:1.00.00"
                elif upper in ("SYSTEM:REMOTE", "SYST:REM"):
                    pass
                elif upper in ("SYSTEM:LOCAL", "SYST:LOC"):
                    pass
                elif upper.startswith("INSTRUMENT:NSELECT") or upper.startswith("INST:NSEL"):
                    try:
                        selected = int(cmd.split()[-1])
                    except ValueError:
                        pass
                elif upper.startswith("VOLTAGE:LIMIT") or upper.startswith("VOLT:LIM"):
                    channels[selected].ovp = float(cmd.split()[-1])
                elif upper.startswith("CURRENT:LIMIT") or upper.startswith("CURR:LIM"):
                    channels[selected].ocp = float(cmd.split()[-1])
                elif upper.startswith("VOLTAGE") or upper.startswith("VOLT"):
                    channels[selected].voltage = float(cmd.split()[-1])
                elif upper.startswith("CURRENT") or upper.startswith("CURR"):
                    channels[selected].current = float(cmd.split()[-1])
                elif upper.startswith("OUTPUT:STATE:ALL?") or upper.startswith("OUTP:STAT:ALL?"):
                    reply = ",".join("1" if channels[c].output else "0" for c in (1, 2, 3))
                elif upper.startswith("OUTPUT:STATE") or upper.startswith("OUTP:STAT"):
                    val = cmd.split()[-1]
                    channels[selected].output = val in ("1", "ON")
                elif upper.startswith("MEASURE:VOLTAGE:ALL") or upper.startswith("MEAS:VOLT:ALL"):
                    reply = ",".join(
                        f"{(channels[c].voltage if channels[c].output else 0.0):.3f}" for c in (1, 2, 3)
                    )
                elif upper.startswith("MEASURE:CURRENT:ALL") or upper.startswith("MEAS:CURR:ALL"):
                    reply = ",".join(
                        f"{(channels[c].current if channels[c].output else 0.0):.3f}" for c in (1, 2, 3)
                    )
                elif upper.startswith("MEASURE:VOLTAGE") or upper.startswith("MEAS:VOLT"):
                    ch = channels[selected]
                    reply = f"{(ch.voltage if ch.output else 0.0):.3f}"
                elif upper.startswith("MEASURE:CURRENT") or upper.startswith("MEAS:CURR"):
                    ch = channels[selected]
                    reply = f"{(ch.current if ch.output else 0.0):.3f}"
                elif upper.startswith("MEASURE:POWER") or upper.startswith("MEAS:POW"):
                    ch = channels[selected]
                    v = ch.voltage if ch.output else 0.0
                    i = ch.current if ch.output else 0.0
                    reply = f"{(v * i):.3f}"
                elif cmd.endswith("?"):
                    reply = "0"

                if reply is not None:
                    if verbose:
                        print(f"[{addr}] ch{selected} -> {reply}")
                    conn.sendall((reply + "\n").encode("ascii"))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 15000
    verbose = "-v" in sys.argv
    channels = {1: Channel(), 2: Channel(), 3: Channel()}

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    print(f"mock OWON ODP6033 SCPI server listening on 127.0.0.1:{port}")

    try:
        while True:
            conn, addr = srv.accept()
            t = threading.Thread(target=handle_client, args=(conn, addr, channels, verbose), daemon=True)
            t.start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
