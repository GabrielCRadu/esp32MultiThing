"""
Quick test: verify Assetto Corsa is sending UDP telemetry on port 9996.
Run this on the gaming PC while AC is open with a session loaded.
"""

import socket
import struct
import time

AC_PORT = 9996
LOCAL_PORT = 4211  # different from ESP32's port so they don't conflict

def send_handshake(sock, op_id):
    # struct handshaker { int identifier; int version; int operationId; }
    pkt = struct.pack("<iii", 1, 1, op_id)
    sock.sendto(pkt, ("127.0.0.1", AC_PORT))

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", LOCAL_PORT))
    sock.settimeout(2.0)

    print(f"Sending HANDSHAKE to 127.0.0.1:{AC_PORT} ...")
    send_handshake(sock, 0)  # operationId=0 HANDSHAKE

    try:
        data, addr = sock.recvfrom(512)
        print(f"Got handshake response ({len(data)} bytes) from {addr}")
        # handshackerResponse: char[50] carName, char[50] driverName,
        #                       int identifier, int version,
        #                       char[50] trackName, char[50] trackConfig
        # AC uses wchar_t[50] (UTF-16LE, 100 bytes each), total response = 408 bytes
        if len(data) >= 408:
            car    = data[0:100].rstrip(b"\x00").decode("utf-16-le", errors="replace")
            driver = data[100:200].rstrip(b"\x00").decode("utf-16-le", errors="replace")
            ident, ver = struct.unpack_from("<ii", data, 200)
            track  = data[208:308].rstrip(b"\x00").decode("utf-16-le", errors="replace")
            config = data[308:408].rstrip(b"\x00").decode("utf-16-le", errors="replace")
            print(f"  Car:    {car!r}")
            print(f"  Driver: {driver!r}")
            print(f"  Track:  {track!r}  ({config!r})")
            print(f"  ID={ident}  Version={ver}")
        else:
            print(f"  Response {len(data)} bytes (expected 408 for wchar_t strings)")
    except socket.timeout:
        print("  No response — AC is probably not running a session, or Remote Telemetry is disabled.")
        print("  Enable it: AC Launcher → Settings → General → Remote Telemetry (port 9996)")
        sock.close()
        return

    print("\nSending SUBSCRIBE_UPDATE ...")
    send_handshake(sock, 1)  # operationId=1 SUBSCRIBE_UPDATE

    print("Listening for telemetry packets (10 seconds)...\n")
    count = 0
    start = time.time()
    while time.time() - start < 10:
        try:
            data, _ = sock.recvfrom(512)
            if len(data) > 0 and data[0:1] == b"a":
                count += 1
                if count <= 5 or count % 60 == 0:
                    # RTCarInfo offsets (with Windows MSVC padding):
                    # +4  int size
                    # +8  float speed_Kmh
                    # +68 float engineRPM
                    # +76 int gear
                    size_val,         = struct.unpack_from("<i",   data, 4)
                    speed_kmh,        = struct.unpack_from("<f",   data, 8)
                    engine_rpm,       = struct.unpack_from("<f",   data, 68)
                    gear,             = struct.unpack_from("<i",   data, 76)
                    gear_str = {-1:"R", 0:"N"}.get(gear, str(gear))
                    print(f"  pkt #{count:4d}  size={size_val}  "
                          f"speed={speed_kmh:6.1f} km/h  "
                          f"rpm={engine_rpm:7.1f}  gear={gear_str}  "
                          f"raw_len={len(data)}")
        except socket.timeout:
            pass

    print(f"\nReceived {count} telemetry packets in 10 s "
          f"({'OK' if count > 0 else 'NOTHING — check settings'}).")

    # Dismiss
    send_handshake(sock, 3)
    sock.close()

if __name__ == "__main__":
    main()
