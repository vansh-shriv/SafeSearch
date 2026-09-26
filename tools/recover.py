#!/usr/bin/env python3
"""Send a signed SafeFlash image to a device in serial recovery mode.

  recover.py IMAGE --socket HOST:PORT        e.g. a Renode socket terminal wired to USART1
  recover.py IMAGE --serial COM3 [--baud N]  a real serial port (needs pyserial)

Protocol (bootloader/src/recovery.h): frames `0xA5 | type | len u16 | payload | crc32`, stop-and-wait.
BEGIN(total length) -> DATA(offset, <=256 bytes) ... -> END. Text the bootloader prints between frames is
skipped. Timeouts and CRC NAKs are retried; a NAK for any other reason is fatal.

Exit codes: 0 installed, 1 device refused the image (NAK), 2 no response, 3 END not acknowledged.
"""
import argparse
import socket
import struct
import sys
import time
import zlib

SOF = 0xA5
BEGIN, DATA, END, ABORT, ACK, NAK = 0x01, 0x02, 0x03, 0x04, 0x81, 0x82
CHUNK = 256
NAK_REASONS = {1: "crc", 2: "length", 3: "state", 4: "offset", 5: "size", 6: "flash", 7: "bad image", 8: "type"}
IMG_STATUS = {1: "read error", 2: "bad magic", 3: "bad header crc", 4: "bad size", 5: "below version floor",
              6: "bad image crc", 7: "bad payload hash", 8: "bad signature"}


class Timeout(Exception):
    pass


class SocketLink:
    """TCP link (e.g. a Renode socket terminal, created with telnet emulation OFF: telnet would corrupt binary
    frames). byte_delay can pace writes if a UART model ever overruns; it is off by default."""

    def __init__(self, hostport, byte_delay):
        host, port = hostport.rsplit(":", 1)
        self.s = socket.create_connection((host, int(port)), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.byte_delay = byte_delay

    def write(self, b):
        for i in range(len(b)):
            self.s.sendall(b[i:i + 1])
            if self.byte_delay:
                time.sleep(self.byte_delay)

    def read_byte(self, deadline):
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Timeout
            self.s.settimeout(left)
            try:
                d = self.s.recv(1)
            except socket.timeout:
                raise Timeout
            if not d:
                raise Timeout
            return d[0]


class SerialLink:
    def __init__(self, port, baud):
        import serial  # pyserial, only needed for real hardware
        self.s = serial.Serial(port, baud, timeout=0.1)

    def write(self, b):
        self.s.write(b)

    def read_byte(self, deadline):
        while time.monotonic() < deadline:
            d = self.s.read(1)
            if d:
                return d[0]
        raise Timeout


def frame(ftype, payload=b""):
    body = bytes([ftype]) + struct.pack("<H", len(payload)) + payload
    return bytes([SOF]) + body + struct.pack("<I", zlib.crc32(body))


def read_frame(link, timeout):
    """Next valid frame from the device as (type, payload); text and bad frames are skipped."""
    deadline = time.monotonic() + timeout
    while True:
        if link.read_byte(deadline) != SOF:
            continue
        hdr = bytes(link.read_byte(deadline) for _ in range(3))
        n = hdr[1] | (hdr[2] << 8)
        if n > 300:
            continue
        payload = bytes(link.read_byte(deadline) for _ in range(n))
        crc = bytes(link.read_byte(deadline) for _ in range(4))
        if struct.unpack("<I", crc)[0] == zlib.crc32(hdr + payload):
            return hdr[0], payload


def transact(link, ftype, payload, timeout, retries=6, verbose=False):
    """Send one frame, return the device's ACK payload. Retries timeouts and CRC NAKs."""
    for attempt in range(retries):
        link.write(frame(ftype, payload))
        try:
            rtype, rp = read_frame(link, timeout)
        except Timeout:
            if verbose:
                print(f"  timeout, retry {attempt + 1}", file=sys.stderr)
            continue
        if rtype == ACK:
            return rp
        if rtype == NAK:
            reason = rp[0] if rp else 0
            if reason == 1:    # our frame was corrupted in transit: resend
                continue
            detail = f" ({IMG_STATUS.get(rp[1], rp[1])})" if reason == 7 and len(rp) > 1 else ""
            raise SystemExit(f"device refused: {NAK_REASONS.get(reason, reason)}{detail}") from None
    raise Timeout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--socket")
    g.add_argument("--serial")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=5.0, help="per-frame reply timeout in seconds")
    ap.add_argument("--byte-delay", type=float, default=0.0, help="socket only: seconds between bytes (off by default; only needed if a UART model overruns)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    with open(a.image, "rb") as f:
        img = f.read()
    link = SocketLink(a.socket, a.byte_delay) if a.socket else SerialLink(a.serial, a.baud)

    try:
        transact(link, BEGIN, struct.pack("<I", len(img)), a.timeout * 4, verbose=a.verbose)   # erase takes a while
        for off in range(0, len(img), CHUNK):
            transact(link, DATA, struct.pack("<I", off) + img[off:off + CHUNK], a.timeout, verbose=a.verbose)
            if a.verbose:
                print(f"  {min(off + CHUNK, len(img))}/{len(img)}", file=sys.stderr)
        try:
            transact(link, END, b"", a.timeout * 4, verbose=a.verbose)   # verification (ECDSA) runs before the ACK
        except Timeout:
            print("no ACK for END: the device may have installed the image and reset", file=sys.stderr)
            return 3
    except Timeout:
        print("no response from device", file=sys.stderr)
        return 2
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 1
    print(f"installed {len(img)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
