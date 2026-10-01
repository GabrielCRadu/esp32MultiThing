"""
AC UDP Relay — bridges ESP32 LAN traffic to AC's loopback-only telemetry socket.

AC binds its UDP server to 127.0.0.1:9996, so LAN clients (ESP32) can't reach it.
This relay:
  - Listens on 0.0.0.0:LISTEN_PORT (default 9997) for packets from the ESP32
  - Forwards them to 127.0.0.1:9996 (AC)
  - Relays AC's replies back to whichever ESP32 address last sent a packet
  - Auto-reconnects to AC if telemetry stops (e.g. game restarted)

Run this on the same PC as Assetto Corsa alongside music.py.
Set AC_REMOTE_PORT = 9997 in the Arduino sketch.
"""

import socket
import struct
import threading
import time

AC_HOST     = "127.0.0.1"
AC_PORT     = 9996
LISTEN_PORT = 9997
BUFFER_SIZE = 512

HANDSHAKE_PKT = struct.pack("<iii", 1, 1, 0)
SUBSCRIBE_PKT = struct.pack("<iii", 1, 1, 1)

# RTCarInfo offsets (Windows MSVC natural alignment)
OFF_SIZE      = 4
OFF_SPEED_KMH = 8
OFF_RPM       = 68
OFF_GEAR      = 76

def parse_telemetry(data):
    """Return (size, speed_kmh, rpm, gear_str) or None if not a telemetry packet."""
    if len(data) < 80 or data[0:1] != b"a":
        return None
    size,      = struct.unpack_from("<i", data, OFF_SIZE)
    speed_kmh, = struct.unpack_from("<f", data, OFF_SPEED_KMH)
    rpm,       = struct.unpack_from("<f", data, OFF_RPM)
    gear,      = struct.unpack_from("<i", data, OFF_GEAR)
    gear_str   = {0: "R", 1: "N"}.get(gear, str(gear - 1))
    return size, speed_kmh, rpm, gear_str

def relay():
    lan_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    lan_sock.bind(("0.0.0.0", LISTEN_PORT))
    lan_sock.settimeout(0.5)

    ac_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ac_sock.bind(("127.0.0.1", 0))
    ac_sock.settimeout(0.01)

    print(f"[relay] Listening on 0.0.0.0:{LISTEN_PORT}  ->  {AC_HOST}:{AC_PORT}")
    print(f"[relay] Waiting for ESP32 to connect...")

    client_addr   = None
    state         = "WAIT_CLIENT"   # WAIT_CLIENT → HANDSHAKING → CONNECTED
    telem_count   = 0
    last_print_ts = 0.0
    handshake_ts  = 0.0
    last_telem_ts = 0.0   # last time a telemetry packet arrived from AC

    lock = threading.Lock()

    def ac_to_lan():
        nonlocal state, telem_count, last_print_ts, last_telem_ts
        while True:
            try:
                data, _ = ac_sock.recvfrom(BUFFER_SIZE)
            except socket.timeout:
                continue
            except OSError:
                break

            with lock:
                addr      = client_addr
                cur_state = state

            if not addr:
                continue

            # handshake response from AC → forward to ESP32, then subscribe
            if cur_state == "HANDSHAKING" and len(data) >= 208:
                def _ac_str(raw):
                    s = raw.decode("utf-16-le", errors="replace")
                    cut = s.find("%")
                    return s[:cut] if cut != -1 else s

                if len(data) >= 408:
                    car    = _ac_str(data[0:100])
                    driver = _ac_str(data[100:200])
                    track  = _ac_str(data[208:308])
                    config = _ac_str(data[308:408])
                    print(f"[relay] Handshake OK ({len(data)} bytes)")
                    print(f"        Car:    {car}")
                    print(f"        Driver: {driver}")
                    print(f"        Track:  {track}  ({config})")
                else:
                    print(f"[relay] Handshake response {len(data)} bytes (expected 408)")

                with lock:
                    state       = "CONNECTED"
                    telem_count = 0
                # Send SUBSCRIBE immediately so AC starts streaming without
                # waiting for the ESP32's next 2-second keepalive cycle
                ac_sock.sendto(SUBSCRIBE_PKT, (AC_HOST, AC_PORT))

            # telemetry packet
            elif cur_state == "CONNECTED":
                parsed = parse_telemetry(data)
                if parsed:
                    now = time.time()
                    with lock:
                        telem_count  += 1
                        last_telem_ts = now
                        cnt           = telem_count
                    if cnt <= 5 or now - last_print_ts >= 1.0:
                        size, speed, rpm, gear = parsed
                        print(f"[relay] pkt #{cnt:5d}  size={size}  "
                              f"speed={speed:6.1f} km/h  rpm={rpm:7.1f}  gear={gear}  "
                              f"raw={len(data)}b")
                        last_print_ts = now

            lan_sock.sendto(data, addr)

    threading.Thread(target=ac_to_lan, daemon=True).start()

    while True:
        try:
            data, addr = lan_sock.recvfrom(BUFFER_SIZE)
        except socket.timeout:
            now = time.time()
            with lock:
                cur_state  = state
                hs_ts      = handshake_ts
                lt         = last_telem_ts
                cur_client = client_addr

            # Handshake sent but no response from AC after 3s
            if cur_state == "HANDSHAKING" and hs_ts and now - hs_ts > 3.0:
                print(f"[relay] No handshake response from AC after 3s - is a session loaded?")
                with lock:
                    state = "WAIT_CLIENT"

            # Telemetry was flowing but stopped for >5s — AC likely restarted
            if cur_state == "CONNECTED" and lt and now - lt > 5.0:
                print(f"[relay] No telemetry for 5s - re-handshaking with AC")
                ac_sock.sendto(HANDSHAKE_PKT, (AC_HOST, AC_PORT))
                with lock:
                    state         = "HANDSHAKING"
                    handshake_ts  = now
                    last_telem_ts = 0.0   # reset to avoid repeated triggers

            continue
        except KeyboardInterrupt:
            print("[relay] Stopped.")
            break

        with lock:
            if client_addr != addr:
                print(f"[relay] ESP32 connected from {addr[0]}:{addr[1]}")
            client_addr = addr

        # classify outgoing packet (ESP32 → AC)
        if len(data) == 12:
            try:
                ident, ver, op = struct.unpack("<iii", data)
                op_names = {0: "HANDSHAKE", 1: "SUBSCRIBE_UPDATE", 3: "DISMISS"}
                op_name  = op_names.get(op, f"op={op}")
                print(f"[relay] ESP32 -> AC: {op_name}  (ident={ident} ver={ver})")
                with lock:
                    if op == 0:
                        state        = "HANDSHAKING"
                        handshake_ts = now = time.time()
                        telem_count  = 0
                    elif op == 3:
                        state = "WAIT_CLIENT"
                        print(f"[relay] Session dismissed.")
            except Exception:
                pass
        else:
            print(f"[relay] ESP32 -> AC: {len(data)}b (unknown)")

        ac_sock.sendto(data, (AC_HOST, AC_PORT))

    lan_sock.close()
    ac_sock.close()

if __name__ == "__main__":
    relay()
