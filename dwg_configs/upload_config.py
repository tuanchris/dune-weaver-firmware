#!/usr/bin/env python3
"""Upload a config file to FluidNC over serial XModem, restart, show boot log.

Usage: python3 upload_config.py <config.yaml> [port] [remote-name]
"""
import serial, sys, time

SOH, EOT, ACK, NAK, CAN, CTRLZ = 0x01, 0x04, 0x06, 0x15, 0x18, 0x1A


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 1
    s.dtr = False  # avoid pulsing the ESP32 reset line on open
    s.rts = False
    s.open()
    time.sleep(0.3)
    return s


def main():
    path = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbserial-8310"
    remote = sys.argv[3] if len(sys.argv) > 3 else "/littlefs/config.yaml"

    with open(path, "rb") as f:
        data = f.read()

    s = open_port(port)
    s.reset_input_buffer()
    s.write(f"$Xmodem/Receive={remote}\n".encode())

    crc_mode = None
    start = time.time()
    while time.time() - start < 15:
        b = s.read(1)
        if not b:
            continue
        if b == b"C":
            crc_mode = True
            break
        if b[0] == NAK:
            crc_mode = False
            break
    if crc_mode is None:
        sys.exit("FAIL: no XModem handshake")

    seq = 1
    for off in range(0, len(data), 128):
        block = data[off : off + 128].ljust(128, bytes([CTRLZ]))
        pkt = bytes([SOH, seq & 0xFF, 0xFF - (seq & 0xFF)]) + block
        if crc_mode:
            c = crc16(block)
            pkt += bytes([c >> 8, c & 0xFF])
        else:
            pkt += bytes([sum(block) & 0xFF])
        for _ in range(10):
            s.write(pkt)
            s.flush()
            r = s.read(1)
            while r == b"C":
                r = s.read(1)
            if r and r[0] == ACK:
                break
            if r and r[0] == CAN:
                sys.exit("FAIL: transfer cancelled")
        else:
            sys.exit(f"FAIL: packet {seq} not acked")
        seq += 1

    for _ in range(10):
        s.write(bytes([EOT]))
        s.flush()
        r = s.read(1)
        if r and r[0] == ACK:
            break
    else:
        sys.exit("FAIL: EOT not acked")

    time.sleep(1)
    s.read(s.in_waiting or 1)
    print(f"Upload OK ({len(data)} bytes) -> {remote}")

    s.reset_input_buffer()
    s.write(b"$Bye\n")
    boot = b""
    start = time.time()
    while time.time() - start < 14:
        n = s.in_waiting
        if n:
            boot += s.read(n)
        else:
            time.sleep(0.1)
    s.close()
    print(boot.decode(errors="replace"))


if __name__ == "__main__":
    main()
