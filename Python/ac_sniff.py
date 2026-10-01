import socket, struct, time, traceback

AC_HOST    = "127.0.0.1"
AC_PORT    = 9996
LOCAL_PORT = 4212

def send_op(sock, op):
    sock.sendto(struct.pack("<iii", 1, 1, op), (AC_HOST, AC_PORT))

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", LOCAL_PORT))
    sock.settimeout(3.0)

    print("Sending HANDSHAKE...")
    send_op(sock, 0)

    try:
        data, addr = sock.recvfrom(2048)
    except socket.timeout:
        print("No response from AC. Is a session loaded?")
        sock.close()
        return

    print(f"Handshake response: {len(data)} bytes  first_hex={data[:8].hex()}")

    print("Sending SUBSCRIBE_UPDATE...")
    send_op(sock, 1)

    print(f"\n{'#':>6}  {'bytes':>5}  {'[0]':>4}  {'speed':>7}  {'rpm':>7}  gear")
    print("-" * 50)

    count = 0
    t_start = time.monotonic()
    sock.settimeout(2.0)

    while True:
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            print(f"\nTimeout. {count} packets in {time.monotonic()-t_start:.1f}s")
            break
        except Exception as e:
            print(f"recv error: {e}")
            traceback.print_exc()
            break

        count += 1
        first = data[0] if data else 0

        if len(data) >= 80:
            try:
                size,  = struct.unpack_from("<i", data,  4)
                speed, = struct.unpack_from("<f", data,  8)
                rpm,   = struct.unpack_from("<f", data, 68)
                gear,  = struct.unpack_from("<i", data, 76)
                gear_s = {-1:"R", 0:"N"}.get(gear, str(gear))
            except Exception:
                size, speed, rpm, gear_s = 0, 0.0, 0.0, "?"
        else:
            size, speed, rpm, gear_s = 0, 0.0, 0.0, "?"

        if count <= 10 or count % 60 == 0:
            print(f"{count:>6}  {len(data):>5}  0x{first:02x}  "
                  f"{speed:>7.1f}  {rpm:>7.1f}  {gear_s}")

    send_op(sock, 3)
    sock.close()

if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        input("Press Enter to close...")
