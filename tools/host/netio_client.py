#!/usr/bin/env python3
"""NetIO throughput client for the freewebcamera netutils netio server.

The board runs the RT-Thread netutils netio server (TCP 18767), whose wire
dialect is documented in third-party/netutils/netio/netio.c:

  - the client opens the connection and speaks first:
      4 bytes  command, big-endian   1 = C2S (client sends)
                                     2 = S2C (client receives)
                                     0 = quit
      4 bytes  block size, big-endian
  - then, per direction, blocks of exactly `block size` bytes where the
    FIRST byte of each block is the continuation flag:
      0x00 = more blocks follow, nonzero = this is the last block
  - a completed direction returns the server to the command state; QUIT or
    closing the socket ends the session.

(The downloaded `netio_cli.py` / sourceforge `netiocli` are unrelated: they
drive the AVR NET-IO development board over a telnet text protocol, port
50290. Different "netio" entirely. The nwlab.net NetIO-GUI speaks a
different header layout again - 2-byte version + 2-byte operation - which
this server does not accept.)

usage:
  netio_client.py <board-ip> [-p PORT] [-b BLOCK] [-t SECONDS]
                  [--c2s | --s2c | --both]

exit code 0 = both directions ran to completion; 1 = any failure.
"""

import argparse
import socket
import struct
import sys
import time

NETIO_CMD_C2S = 1
NETIO_CMD_S2C = 2
NETIO_CMD_QUIT = 0

FLAG_LAST = 1


def recv_exact(sock, size):
    buf = bytearray()
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("server closed mid-block")
        buf += chunk
    return buf


def run_c2s(sock, block, seconds):
    """Client -> server: stream flag-tagged blocks, measure client-side."""
    payload = bytearray(block)		# flag 0x00 = "more"
    more = bytearray(block)
    last = bytearray(block)
    last[0] = FLAG_LAST

    cmd = struct.pack(">II", NETIO_CMD_C2S, block)
    sock.sendall(cmd)

    sent = 0
    begin = time.monotonic()
    while time.monotonic() - begin < seconds:
        sock.sendall(more)
        sent += block
    sock.sendall(last)
    sent += block
    elapsed = time.monotonic() - begin

    mbps = sent * 8 / elapsed / 1e6
    print(f"C2S: {sent} bytes in {elapsed:.2f}s = {mbps:.2f} Mbit/s")
    return mbps


def run_s2c(sock, block, seconds):
    """Server -> client: read flag-tagged blocks until the last-block flag.

    The server streams for its internal 6-second window, then marks a final
    block; the client-side wall clock is what the throughput number reports.
    """
    cmd = struct.pack(">II", NETIO_CMD_S2C, block)
    sock.sendall(cmd)

    got = 0
    begin = time.monotonic()
    while True:
        blk = recv_exact(sock, block)
        got += block
        if blk[0] != 0:
            break
        if time.monotonic() - begin > seconds * 3:
            raise TimeoutError("server never marked the last block")
    elapsed = time.monotonic() - begin

    mbps = got * 8 / elapsed / 1e6
    print(f"S2C: {got} bytes in {elapsed:.2f}s = {mbps:.2f} Mbit/s")
    return mbps


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="board IP (the netio server side)")
    ap.add_argument("-p", "--port", type=int, default=18767)
    ap.add_argument("-b", "--block", type=int, default=65536,
                    help="block size in bytes (default 65536; the server "
                         "circularizes into a 4 KiB buffer, so any size works)")
    ap.add_argument("-t", "--time", type=float, default=6.0,
                    help="seconds per direction (default 6; the server's own "
                         "S2C window is 6s regardless)")
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--c2s", action="store_true")
    grp.add_argument("--s2c", action="store_true")
    grp.add_argument("--both", action="store_true", default=True)
    args = ap.parse_args()

    ok = True
    sock = socket.create_connection((args.host, args.port), timeout=10)
    try:
        if args.c2s or args.both:
            run_c2s(sock, args.block, args.time)
        if args.s2c or args.both:
            run_s2c(sock, args.block, args.time)
        try:
            sock.sendall(struct.pack(">II", NETIO_CMD_QUIT, 0))
        except OSError:
            pass
    except (ConnectionError, TimeoutError, OSError) as exc:
        print(f"netio: {exc}", file=sys.stderr)
        ok = False
    finally:
        sock.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
